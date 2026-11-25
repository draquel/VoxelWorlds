// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "Math/RandomStream.h"
#include "Hash/CityHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "ChunkRenderData.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelEditManager.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelMeshingTypes.h"
#include "VoxelNeighborSliceExtraction.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Testing/VoxelTestScopedCVar.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G283 (Documentation/FINDINGS.md): a teleport's collision cooks were repeated. About a third meshed a chunk with no
// surface, and a chunk cooked before a neighbour had voxels was cooked again when it arrived.
//
// - Collision.NoSurface.Prediction: UVoxelCollisionManager::PredictNoSurface against the three meshers (Dual Contouring,
//   Marching Cubes, cubic) on the same voxels, built as a cook builds its request (VoxelNeighborSlices::Extract): fixed
//   cases (air everywhere, a solid neighbour touching an air chunk, a missing neighbour beside rock, density 127, leaves)
//   and 300 random neighbourhoods with voxels changed inside and outside the planes a cook reads. Whenever it predicts no
//   surface, no mesher makes a triangle.
// - Collision.NoSurface.SameCollision: a plane (Dual Contouring), a block world, a plane of Marching Cubes and a planet,
//   each streamed and cooked from scratch (the viewer then moved two chunks, so neighbours arrive after cooks):
//   - under the audit (voxel.Collision.CookAudit 1: every cook meshed anyway), no chunk predicted to have no surface had a
//     triangle, while neighbours were still missing as well as after;
//   - with the skip on, then every chunk re-cooked with its neighbours all present, once with the skip off and once on:
//     every chunk's collision (its body's triangle mesh, or none) the same, the second skipping cooks. (A streamed world's
//     end state is compared this way, not stream against stream: an arrival with solid on both sides of a boundary leaves
//     the wall a cook built there buried in the rock, G32, so two streams can end with different buried triangles.)
//   - an edit that raises ground in a chunk skipped as air gets it a body.
// - Collision.NeighborWait: with voxel.Collision.NeighborWait 1 (decision D196) the plane settles, the wait held chunks,
//   chunks near the viewer got collision, and no cook is left reading as air a neighbour that has voxels.
// ---------------------------------------------------------------------------

namespace VoxelCollisionNoSurfaceTest
{
	constexpr int32 CS = 16;

	/** A chunk's voxels and its neighbours' (a missing neighbour is absent), keyed by offset (the chunk is at the origin). */
	struct FNeighborhood
	{
		TArray<FVoxelData> Own;
		TMap<FIntVector, TArray<FVoxelData>> Neighbors;
	};

	TArray<FVoxelData> Filled(const FVoxelData& Value)
	{
		TArray<FVoxelData> Out;
		Out.Init(Value, CS * CS * CS);
		return Out;
	}

	/** Every neighbour present and filled with Value. */
	FNeighborhood Uniform(const FVoxelData& OwnValue, const FVoxelData& NeighborValue)
	{
		FNeighborhood N;
		N.Own = Filled(OwnValue);
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			if (DX != 0 || DY != 0 || DZ != 0)
			{
				N.Neighbors.Add(FIntVector(DX, DY, DZ), Filled(NeighborValue));
			}
		}
		return N;
	}

	bool Predict(const FNeighborhood& N, const FVoxelMaterialTable* Materials)
	{
		const EVoxelSurfaceSide OwnSide = UVoxelCollisionManager::GetVolumeSide(N.Own, CS, Materials);
		return UVoxelCollisionManager::PredictNoSurface(OwnSide, CS, VoxelNeighborSlices::GetNeighborDepth(0, CS),
			[&N](const FIntVector& Offset)
			{
				FVoxelCollisionNeighborView View;
				View.Full = N.Neighbors.Find(Offset);
				return View;
			}, Materials);
	}

	/** The triangles a mesher makes of the chunk with its neighbours' slices, as a collision cook builds its request. */
	int32 MeshTriangles(IVoxelMesher& Mesher, const FNeighborhood& N)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector::ZeroValue;
		Request.LODLevel = 0;
		Request.ChunkSize = CS;
		Request.VoxelSize = 100.0f;
		Request.VoxelData = N.Own;
		VoxelNeighborSlices::Extract(CS, FIntVector::ZeroValue,
			[&N](const FIntVector& Coord) { return N.Neighbors.Contains(Coord); },
			[&N](const FIntVector& Coord, int32 X, int32 Y, int32 Z) { return N.Neighbors[Coord][X + Y * CS + Z * CS * CS]; },
			Request);
		Request.TransitionFaces = 0;
		for (int32 Face = 0; Face < 6; ++Face)
		{
			Request.NeighborLODLevels[Face] = 0;
		}
		FChunkMeshData Mesh;
		Mesher.GenerateMeshCPU(Request, Mesh);
		return Mesh.Indices.Num() / 3;
	}

	/** The three CPU meshers, configured as the chunk manager configures them (the surface at 0.5, the default materials). */
	struct FMeshers
	{
		FVoxelCPUDualContourMesher DC;
		FVoxelCPUMarchingCubesMesher MC;
		FVoxelCPUCubicMesher Cubic;

		FMeshers()
		{
			FVoxelMeshingConfig Config;
			Config.IsoLevel = 0.5f;
			Config.bUseTransvoxel = false;
			for (IVoxelMesher* Mesher : { static_cast<IVoxelMesher*>(&DC), static_cast<IVoxelMesher*>(&MC), static_cast<IVoxelMesher*>(&Cubic) })
			{
				Mesher->Initialize();
				Mesher->SetConfig(Config);
			}
		}

		~FMeshers()
		{
			DC.Shutdown();
			MC.Shutdown();
			Cubic.Shutdown();
		}
	};

	/** A hash of a chunk's cooked triangle mesh; 0 with no body. */
	uint64 HashBody(const UBodySetup* Body)
	{
		if (!Body || Body->TriMeshGeometries.Num() == 0 || !Body->TriMeshGeometries[0].IsValid())
		{
			return 0;
		}
		const Chaos::FTriangleMeshImplicitObject& Mesh = *Body->TriMeshGeometries[0];
		const auto& Particles = Mesh.Particles();
		TArray<float> Positions;
		const int32 Count = static_cast<int32>(Particles.Size());
		Positions.Reserve(Count * 3);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const auto& X = Particles.GetX(Index);
			Positions.Add(X[0]);
			Positions.Add(X[1]);
			Positions.Add(X[2]);
		}
		TArray<int32> Indices;
		const Chaos::FTrimeshIndexBuffer& Elements = Mesh.Elements();
		if (Elements.RequiresLargeIndices())
		{
			for (const auto& Tri : Elements.GetLargeIndexBuffer())
			{
				Indices.Append({ static_cast<int32>(Tri[0]), static_cast<int32>(Tri[1]), static_cast<int32>(Tri[2]) });
			}
		}
		else
		{
			for (const auto& Tri : Elements.GetSmallIndexBuffer())
			{
				Indices.Append({ static_cast<int32>(Tri[0]), static_cast<int32>(Tri[1]), static_cast<int32>(Tri[2]) });
			}
		}
		const uint64 PositionHash = CityHash64(reinterpret_cast<const char*>(Positions.GetData()), static_cast<uint32>(Positions.Num() * sizeof(float)));
		return CityHash64WithSeed(reinterpret_cast<const char*>(Indices.GetData()), static_cast<uint32>(Indices.Num() * sizeof(int32)), PositionHash) | 1;
	}

	/** Tick until the stream and the collision pipeline have both been idle for 90 ticks; false on timeout. */
	bool WaitForCollisionIdle(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds)
	{
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (Collision && IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			const bool bIdle = W.IsIdle() && Collision->GetCookQueueCount() == 0 && Collision->GetCookingCount() == 0;
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 90;
	}

	/** One streamed, cooked world's collision: per loaded chunk with collision, its body's hash (0: ready-empty). */
	struct FRun
	{
		TMap<FIntVector, uint64> Bodies;
		/** Every chunk re-cooked with its neighbours present, with the skip off ([0]) and on ([1]) */
		TMap<FIntVector, uint64> Recooked[2];
		/** The second re-cook's skips */
		int64 RecookSkips = 0;
		/** AuditNeighborCooks after the settle: cooks reading as air a neighbour with voxels and no re-cook pending */
		int32 StaleCooks = 0;
		FVoxelCollisionCookStats Stats;
		int64 NeighborRecooks = 0;
		bool bSettled = false;
		/** The edit's check: a chunk skipped as air got a body once ground was raised in it */
		int32 EditedSkipped = 0;
		int32 EditedGotBody = 0;
	};

	/** The world kinds the comparison runs in. */
	enum class EKind : uint8 { PlaneDC, Blocks, PlaneMC, Planet };

	const TCHAR* KindName(EKind Kind)
	{
		switch (Kind)
		{
		case EKind::PlaneDC: return TEXT("plane (Dual Contouring)");
		case EKind::Blocks: return TEXT("block world");
		case EKind::PlaneMC: return TEXT("plane (Marching Cubes)");
		default: return TEXT("planet");
		}
	}

	/**
	 * Stream and cook a world of Kind with the cvars set, move the viewer two chunks so neighbours arrive after cooks,
	 * settle, and read every chunk's collision. With bRecook, then re-cook every chunk with the skip off and with it on;
	 * with bEdit, then raise ground in a chunk skipped as air.
	 */
	FRun RunWorld(FAutomationTestBase& Test, EKind Kind, int32 Skip, int32 Audit, int32 Wait, bool bRecook, bool bEdit)
	{
		FRun R;
		VoxelTesting::FScopedCVar SkipVar(TEXT("voxel.Collision.SkipNoSurface"), Skip);
		VoxelTesting::FScopedCVar AuditVar(TEXT("voxel.Collision.CookAudit"), Audit);
		VoxelTesting::FScopedCVar WaitVar(TEXT("voxel.Collision.NeighborWait"), Wait);
		VoxelTesting::FScopedCVar WaitChunks(TEXT("voxel.Collision.NeighborWaitChunks"), 1.0f);
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("CollisionNoSurface"));
		if (Kind == EKind::Planet)
		{
			W.ConfigureSmallPlanet();
		}
		else
		{
			W.ConfigureSmallPlane();
			W.Config->MeshingMode = Kind == EKind::Blocks ? EMeshingMode::Cubic
				: (Kind == EKind::PlaneMC ? EMeshingMode::MarchingCubes : EMeshingMode::DualContouring);
		}
		W.Config->bGenerateCollision = true;
		W.Config->CollisionLODLevel = 0;
		W.StartManager();
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		if (!Collision)
		{
			Test.AddError(TEXT("The chunk manager has a collision manager"));
			W.Stop();
			return R;
		}
		// Cook every loaded chunk, the window's edge included.
		Collision->SetCollisionRadius(W.Config->ViewDistance * 3.0f);
		Collision->ResetCookStats();
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		FVector Viewer, Moved;
		if (Kind == EKind::Planet)
		{
			Viewer = W.ViewerOverSurface(FVector(0.0, 0.0, 1.0), 200.0f);
			Moved = W.ViewerOverSurface(FVector(2.0 * ChunkWorld, 2.0 * ChunkWorld, W.Config->WorldRadius).GetSafeNormal(), 200.0f);
		}
		else
		{
			Viewer = W.ViewerOverGround(ChunkWorld * 0.5, ChunkWorld * 0.5, 200.0f);
			Moved = W.ViewerOverGround(ChunkWorld * 2.5, ChunkWorld * 2.5, 200.0f);
		}
		W.SetViewer(Viewer);
		R.bSettled = W.Settle() && WaitForCollisionIdle(W, 120.0);
		W.SetViewer(Moved);
		R.bSettled = R.bSettled && W.Settle() && WaitForCollisionIdle(W, 120.0);

		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		for (const FIntVector& Chunk : Loaded)
		{
			if (Collision->HasCollision(Chunk))
			{
				R.Bodies.Add(Chunk, HashBody(Collision->GetChunkBodySetup(Chunk)));
			}
		}
		R.Stats = Collision->GetCookStats();
		R.NeighborRecooks = Collision->GetTotalNeighborRecooks();
		{
			int32 Cooked = 0, WithMissing = 0, Awaiting = 0;
			Collision->AuditNeighborCooks(Cooked, WithMissing, R.StaleCooks, Awaiting);
		}

		if (bRecook)
		{
			// Every chunk re-cooked from scratch with all its neighbours' voxels present: the cooks' order cannot matter.
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				VoxelTesting::FScopedCVar PassSkip(TEXT("voxel.Collision.SkipNoSurface"), Pass);
				Collision->ResetCookStats();
				for (const TPair<FIntVector, uint64>& Pair : R.Bodies)
				{
					Collision->RegenerateChunkCollision(Pair.Key);
				}
				R.bSettled = WaitForCollisionIdle(W, 120.0) && R.bSettled;
				for (const TPair<FIntVector, uint64>& Pair : R.Bodies)
				{
					if (Collision->HasCollision(Pair.Key))
					{
						R.Recooked[Pass].Add(Pair.Key, HashBody(Collision->GetChunkBodySetup(Pair.Key)));
					}
				}
				if (Pass == 1)
				{
					R.RecookSkips = Collision->GetCookStats().SkippedAtLaunch + Collision->GetCookStats().SkippedOnWorker;
				}
			}
		}

		if (bEdit && Kind != EKind::Planet)
		{
			// Chunks over the ground ready-empty with no body: raise a sphere of rock in the middle of a few, and each gets a
			// body (the edit re-cooks it; a skip must not stand in for the cook once there is a surface).
			UVoxelEditManager* Edits = W.Manager->GetEditManager();
			TArray<FIntVector> Targets;
			for (const TPair<FIntVector, uint64>& Pair : R.Bodies)
			{
				if (Pair.Value == 0 && Pair.Key.Z >= 1 && FMath::Abs(Pair.Key.X) <= 1 && FMath::Abs(Pair.Key.Y) <= 1 && Targets.Num() < 3)
				{
					Targets.Add(Pair.Key);
				}
			}
			for (const FIntVector& Chunk : Targets)
			{
				FVoxelBrushParams Brush;
				Brush.Shape = EVoxelBrushShape::Sphere;
				Brush.Radius = 4.0f * W.Config->VoxelSize;
				Brush.Strength = 1.0f;
				Brush.FalloffType = EVoxelBrushFalloff::Sharp;
				Brush.DensityDelta = 255;
				Brush.MaterialID = EVoxelMaterial::Stone;
				const FVector Center = W.Config->WorldOrigin + (FVector(Chunk) + FVector(0.5)) * ChunkWorld;
				Edits->ApplyBrushEdit(Center, Brush, EEditMode::Add);
			}
			WaitForCollisionIdle(W, 60.0);
			for (const FIntVector& Chunk : Targets)
			{
				++R.EditedSkipped;
				R.EditedGotBody += Collision->GetChunkBodySetup(Chunk) != nullptr ? 1 : 0;
			}
		}
		W.Stop();
		return R;
	}

	/** Chunks whose collision differs between two snapshots (a body against none, or another triangle mesh), and those only in one. */
	int32 CountDifferences(const TMap<FIntVector, uint64>& A, const TMap<FIntVector, uint64>& B, FString& OutFirst)
	{
		int32 Differences = 0;
		for (const TPair<FIntVector, uint64>& Pair : A)
		{
			const uint64* Other = B.Find(Pair.Key);
			if (!Other || *Other != Pair.Value)
			{
				if (Differences++ == 0)
				{
					OutFirst = FString::Printf(TEXT("(%d,%d,%d) %s"), Pair.Key.X, Pair.Key.Y, Pair.Key.Z,
						!Other ? TEXT("has collision in one run only") : (Pair.Value == 0 || *Other == 0 ? TEXT("empty in one run, a body in the other") : TEXT("another triangle mesh")));
				}
			}
		}
		for (const TPair<FIntVector, uint64>& Pair : B)
		{
			if (!A.Contains(Pair.Key) && Differences++ == 0)
			{
				OutFirst = FString::Printf(TEXT("(%d,%d,%d) has collision in one run only"), Pair.Key.X, Pair.Key.Y, Pair.Key.Z);
			}
		}
		return Differences;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionNoSurfacePredictionTest, "VoxelWorlds.Collision.NoSurface.Prediction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelCollisionNoSurfacePredictionTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCollisionNoSurfaceTest;
	FMeshers Meshers;
	const FVoxelMaterialTable* Materials = UVoxelCollisionManager::GetMesherMaterials(Meshers.Cubic);
	const FVoxelData Air = FVoxelData::Air();
	const FVoxelData Rock = FVoxelData::Solid(EVoxelMaterial::Stone);
	IVoxelMesher* All[] = { &Meshers.DC, &Meshers.MC, &Meshers.Cubic };
	const TCHAR* Names[] = { TEXT("Dual Contouring"), TEXT("Marching Cubes"), TEXT("cubic") };

	auto Check = [&](const TCHAR* Case, const FNeighborhood& N, bool bExpectPredicted, int32 MustMeshIndex)
	{
		const bool bPredicted = Predict(N, Materials);
		TestTrue(FString::Printf(TEXT("%s: predicted no surface %s"), Case, bExpectPredicted ? TEXT("yes") : TEXT("no")), bPredicted == bExpectPredicted);
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const int32 Triangles = MeshTriangles(*All[Index], N);
			if (bPredicted)
			{
				TestEqual(FString::Printf(TEXT("%s: %s makes no triangle where none is predicted"), Case, Names[Index]), Triangles, 0);
			}
			if (Index == MustMeshIndex)
			{
				TestTrue(FString::Printf(TEXT("%s: %s makes triangles (the case is not vacuous)"), Case, Names[Index]), Triangles > 0);
			}
		}
	};

	Check(TEXT("Air everywhere"), Uniform(Air, Air), true, -1);
	Check(TEXT("Rock everywhere"), Uniform(Rock, Rock), true, -1);
	{
		FNeighborhood N = Uniform(Air, Air);
		N.Neighbors[FIntVector(0, 0, 1)] = Filled(Rock);
		Check(TEXT("An air chunk under a rock chunk"), N, false, 0);
	}
	{
		FNeighborhood N = Uniform(Rock, Rock);
		N.Neighbors.Remove(FIntVector(1, 0, 0));
		Check(TEXT("A rock chunk beside a missing neighbour"), N, false, 0);
	}
	{
		FNeighborhood N = Uniform(Air, Air);
		N.Neighbors.Remove(FIntVector(1, 1, 0));
		N.Neighbors.Remove(FIntVector(0, 0, -1));
		Check(TEXT("An air chunk with missing neighbours"), N, true, -1);
	}
	Check(TEXT("Density 127 (air to the smooth meshers, solid to the cubic one)"), Uniform(FVoxelData(EVoxelMaterial::Stone, 127), Air), false, 2);
	Check(TEXT("A chunk of leaves in rock"), Uniform(FVoxelData::Solid(EVoxelMaterial::Leaves), Rock), false, 2);
	{
		// A voxel of air in the +X neighbour past the plane a cook reads: no mesher reads it.
		FNeighborhood N = Uniform(Rock, Rock);
		N.Neighbors[FIntVector(1, 0, 0)][CS - 1 + 5 * CS + 5 * CS * CS] = Air;
		Check(TEXT("Rock with air beyond the planes read"), N, true, -1);
	}
	{
		// The same voxel in the plane the cook reads: a surface.
		FNeighborhood N = Uniform(Rock, Rock);
		N.Neighbors[FIntVector(1, 0, 0)][0 + 5 * CS + 5 * CS * CS] = Air;
		Check(TEXT("Rock with air in the plane read"), N, false, 0);
	}

	// Random neighbourhoods: one side with random densities and materials, missing neighbours, and a few voxels changed
	// anywhere in a neighbour (within the planes read or not) to the other side or to 127.
	FRandomStream Random(283);
	int32 PredictedCount = 0;
	int32 Wrong = 0;
	constexpr int32 Cases = 300;
	for (int32 Case = 0; Case < Cases; ++Case)
	{
		const bool bAirSide = Random.FRand() < 0.5f;
		auto SideVoxel = [&Random, bAirSide]()
		{
			const uint8 Density = bAirSide ? static_cast<uint8>(Random.RandRange(0, 126)) : static_cast<uint8>(Random.RandRange(128, 255));
			const uint8 Material = Random.FRand() < 0.5f ? EVoxelMaterial::Stone : EVoxelMaterial::Dirt;
			return FVoxelData(Material, Density);
		};
		FNeighborhood N;
		N.Own.SetNumUninitialized(CS * CS * CS);
		for (FVoxelData& V : N.Own)
		{
			V = SideVoxel();
		}
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			if ((DX == 0 && DY == 0 && DZ == 0) || Random.FRand() < 0.1f)
			{
				continue;
			}
			TArray<FVoxelData>& A = N.Neighbors.Add(FIntVector(DX, DY, DZ));
			A.SetNumUninitialized(CS * CS * CS);
			for (FVoxelData& V : A)
			{
				V = SideVoxel();
			}
		}
		if (Random.FRand() < 0.6f && N.Neighbors.Num() > 0)
		{
			TArray<FIntVector> Keys;
			N.Neighbors.GetKeys(Keys);
			const int32 Changes = Random.RandRange(1, 3);
			for (int32 Change = 0; Change < Changes; ++Change)
			{
				TArray<FVoxelData>& A = N.Neighbors[Keys[Random.RandRange(0, Keys.Num() - 1)]];
				// Half the time a voxel next to the chunk (more likely read), else anywhere.
				int32 X = Random.RandRange(0, CS - 1), Y = Random.RandRange(0, CS - 1), Z = Random.RandRange(0, CS - 1);
				if (Random.FRand() < 0.5f)
				{
					X = Random.FRand() < 0.5f ? 0 : CS - 1;
				}
				A[X + Y * CS + Z * CS * CS] = Random.FRand() < 0.2f ? FVoxelData(EVoxelMaterial::Stone, 127)
					: (bAirSide ? FVoxelData::Solid(EVoxelMaterial::Stone) : FVoxelData::Air());
			}
		}
		const bool bPredicted = Predict(N, Materials);
		PredictedCount += bPredicted ? 1 : 0;
		if (bPredicted)
		{
			for (int32 Index = 0; Index < 3; ++Index)
			{
				const int32 Triangles = MeshTriangles(*All[Index], N);
				if (Triangles > 0 && Wrong++ == 0)
				{
					AddError(FString::Printf(TEXT("Random case %d: predicted no surface, %s made %d triangles"), Case, Names[Index], Triangles));
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d of %d random neighbourhoods predicted to have no surface; %d mesher runs disagreed"), PredictedCount, Cases, Wrong));
	TestTrue(TEXT("Random neighbourhoods were predicted both ways (non-vacuous)"), PredictedCount > 20 && PredictedCount < Cases - 20);
	TestEqual(TEXT("No mesher made a triangle where none was predicted"), Wrong, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionNoSurfaceSameCollisionTest, "VoxelWorlds.Collision.NoSurface.SameCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelCollisionNoSurfaceSameCollisionTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCollisionNoSurfaceTest;
	for (const EKind Kind : { EKind::PlaneDC, EKind::Blocks, EKind::PlaneMC, EKind::Planet })
	{
		const FRun Audit = RunWorld(*this, Kind, 0, 1, 0, false, false);
		const FRun New = RunWorld(*this, Kind, 1, 0, 0, true, true);
		FString First;
		const int32 Differences = CountDifferences(New.Recooked[0], New.Recooked[1], First);
		int32 Empty = 0;
		for (const TPair<FIntVector, uint64>& Pair : New.Recooked[0])
		{
			Empty += Pair.Value == 0 ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("%s: audit while streaming: %lld cooks, %lld neighbour re-cooks, %lld predicted to have no surface (%lld wrong, %lld empty not predicted), %lld of %lld neighbour re-cooks built the same triangles; streamed with the skip: %lld cooks, %lld skipped at the launch, %lld on the worker; re-cooked with all neighbours present: %d chunks (%d empty), %lld skipped with the skip on, %d differ%s%s"),
			KindName(Kind), Audit.Stats.Cooks, Audit.NeighborRecooks, Audit.Stats.PredictedEmpty, Audit.Stats.PredictedEmptyWrong,
			Audit.Stats.EmptyNotPredicted, Audit.Stats.NeighborRecooksSame, Audit.Stats.NeighborRecooks, New.Stats.Cooks,
			New.Stats.SkippedAtLaunch, New.Stats.SkippedOnWorker, New.Recooked[0].Num(), Empty, New.RecookSkips, Differences,
			First.IsEmpty() ? TEXT("") : TEXT(", first: "), *First));
		TestTrue(FString::Printf(TEXT("%s: streaming and collision settle"), KindName(Kind)), Audit.bSettled && New.bSettled);
		TestEqual(FString::Printf(TEXT("%s: the audit finds no prediction of no surface wrong"), KindName(Kind)), Audit.Stats.PredictedEmptyWrong, static_cast<int64>(0));
		TestTrue(FString::Printf(TEXT("%s: the audit predicted chunks without a surface (non-vacuous)"), KindName(Kind)), Audit.Stats.PredictedEmpty > 0);
		TestEqual(FString::Printf(TEXT("%s: the audit skipped nothing"), KindName(Kind)), Audit.Stats.SkippedAtLaunch + Audit.Stats.SkippedOnWorker, static_cast<int64>(0));
		TestTrue(FString::Printf(TEXT("%s: streaming with the skip skipped cooks"), KindName(Kind)), New.Stats.SkippedAtLaunch + New.Stats.SkippedOnWorker > 0);
		TestTrue(FString::Printf(TEXT("%s: chunks with collision, some empty (non-vacuous)"), KindName(Kind)), New.Recooked[0].Num() > 0 && Empty > 0);
		TestEqual(FString::Printf(TEXT("%s: every chunk's collision the same with the skip as without"), KindName(Kind)), Differences, 0);
		TestTrue(FString::Printf(TEXT("%s: the re-cook with the skip skipped cooks"), KindName(Kind)), New.RecookSkips > 0);
		if (Kind != EKind::Planet)
		{
			TestTrue(FString::Printf(TEXT("%s: chunks skipped as air were edited (non-vacuous)"), KindName(Kind)), New.EditedSkipped > 0);
			TestEqual(FString::Printf(TEXT("%s: each got a body from the edit"), KindName(Kind)), New.EditedGotBody, New.EditedSkipped);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionNeighborWaitTest, "VoxelWorlds.Collision.NeighborWait",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelCollisionNeighborWaitTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCollisionNoSurfaceTest;
	const FRun Free = RunWorld(*this, EKind::PlaneDC, 1, 0, 0, false, false);
	const FRun Waited = RunWorld(*this, EKind::PlaneDC, 1, 0, 1, false, false);
	AddInfo(FString::Printf(TEXT("Without the wait %lld cooks and %lld neighbour re-cooks; with it %lld cooks, %lld neighbour re-cooks, %lld holds of %lld chunks; the first collision within a chunk width last at %.2f s against %.2f; stale cooks %d and %d"),
		Free.Stats.Cooks, Free.NeighborRecooks, Waited.Stats.Cooks, Waited.NeighborRecooks, Waited.Stats.NeighborWaitHolds,
		Waited.Stats.NeighborWaitChunks, Waited.Stats.BandFirstReadyLastSeconds[0], Free.Stats.BandFirstReadyLastSeconds[0],
		Waited.StaleCooks, Free.StaleCooks));
	TestTrue(TEXT("Streaming and collision settle with and without the wait"), Free.bSettled && Waited.bSettled);
	TestTrue(TEXT("The wait held chunks (non-vacuous)"), Waited.Stats.NeighborWaitChunks > 0);
	TestEqual(TEXT("Without it nothing waited"), Free.Stats.NeighborWaitHolds, static_cast<int64>(0));
	TestTrue(TEXT("Chunks near the viewer got collision"), Waited.Stats.BandFirstReady[0] > 0);
	TestEqual(TEXT("With the wait no cook is left reading as air a neighbour that has voxels"), Waited.StaleCooks, 0);
	TestTrue(TEXT("Every loaded chunk has collision with the wait as without"), Waited.Bodies.Num() == Free.Bodies.Num() && Waited.Bodies.Num() > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
