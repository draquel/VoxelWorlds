// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Hash/CityHash.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelSeamRegistry.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G56 and G57 (WORLD_MODES_REVIEW_PLAN.md section 14.1, section 19.13). Every edit re-meshed the edited chunk's six face
// neighbours "so they re-extract boundary data". Since seam ownership a chunk mesh is a function of the chunk's own voxels
// and a seam job of its participants', and the edited chunk's seams restale with its re-mesh (MarkChunkDirty), so the
// neighbours' re-mesh should change nothing (G56). It did one thing: their incident seams cover all of the edited chunk's,
// and MarkChunkDirty restaled nothing for a chunk on its way to a mesh (queued or on a worker); a mesh already on a worker
// also landed without the edit (G57).
//
// - Streaming.EditRemesh.Plane / .Planet: a real chunk manager streams a small world into a renderer that records the
//   chunk meshes and seam buckets, for DC, MC and cubic (cubic on the plane only: planets are smooth) with and without LOD,
//   and with water. Edits at a chunk edge running along local up, where the surface crosses it: a system edit of one
//   chunk's voxels by its min edge (the seams there are owned by its unedited neighbours) and one by another chunk's max
//   edge, a player brush across the four chunks, and two edits that land while the chunk waits for its mesh (dispatch held)
//   and while its mesh is on a worker. Once each edit has settled, every chunk mesh and seam bucket must equal what a full
//   rebuild gives then (every chunk re-meshed, every seam rebuilt; it also gives the next edit a clean start); and with
//   voxel.Edit.NeighbourRemesh 0 they must equal the rule before's (1), as must the edit-merged voxels' shape. Water flags
//   are reported, not compared: a chunk's mesh completion pulls them across its faces from its neighbours' generated voxels
//   (PropagateWaterFromNeighbors), which the face neighbours' re-mesh also ran. The meshing work per edit (chunk meshes, seam
//   jobs) is logged for both rules.
//
// G60 (section 19.17): undo and redo restore each voxel in its own chunk and broadcast every chunk they changed (redo wrote
// every edit into an operation's first chunk and broadcast only it). Edits 6 and 7: a second player brush across the four
// chunks, settled and undone; then its redo. Both are checked against a full rebuild like the others.
// ---------------------------------------------------------------------------

namespace VoxelEditRemeshTest
{
	/** One vertex as a hash: position to 0.01 uu, normal to 1e-3, both UV sets and the colour. */
	uint64 VertexKey(const FChunkMeshData& Mesh, int32 Index)
	{
		int64 K[11] = {};
		const FVector3f& P = Mesh.Positions[Index];
		K[0] = FMath::RoundToInt64(P.X * 100.0);
		K[1] = FMath::RoundToInt64(P.Y * 100.0);
		K[2] = FMath::RoundToInt64(P.Z * 100.0);
		if (Mesh.Normals.IsValidIndex(Index))
		{
			K[3] = FMath::RoundToInt64(Mesh.Normals[Index].X * 1000.0);
			K[4] = FMath::RoundToInt64(Mesh.Normals[Index].Y * 1000.0);
			K[5] = FMath::RoundToInt64(Mesh.Normals[Index].Z * 1000.0);
		}
		if (Mesh.UVs.IsValidIndex(Index))
		{
			K[6] = FMath::RoundToInt64(Mesh.UVs[Index].X * 1000.0);
			K[7] = FMath::RoundToInt64(Mesh.UVs[Index].Y * 1000.0);
		}
		if (Mesh.UV1s.IsValidIndex(Index))
		{
			K[8] = FMath::RoundToInt64(Mesh.UV1s[Index].X * 100.0);
			K[9] = FMath::RoundToInt64(Mesh.UV1s[Index].Y * 100.0);
		}
		if (Mesh.Colors.IsValidIndex(Index))
		{
			K[10] = Mesh.Colors[Index].ToPackedARGB();
		}
		return CityHash64(reinterpret_cast<const char*>(K), sizeof(K));
	}

	/** A mesh as its sorted triangles (each its three vertices from the smallest, winding kept), so vertex and triangle order do not matter. */
	TArray<uint64> TriangleKeys(const FChunkMeshData& Mesh)
	{
		TArray<uint64> Vertices;
		Vertices.SetNumUninitialized(Mesh.Positions.Num());
		for (int32 i = 0; i < Mesh.Positions.Num(); ++i)
		{
			Vertices[i] = VertexKey(Mesh, i);
		}
		TArray<uint64> Keys;
		Keys.Reserve(Mesh.Indices.Num() / 3);
		for (int32 t = 0; t + 2 < Mesh.Indices.Num(); t += 3)
		{
			const uint32 I[3] = { Mesh.Indices[t], Mesh.Indices[t + 1], Mesh.Indices[t + 2] };
			if (!Vertices.IsValidIndex(I[0]) || !Vertices.IsValidIndex(I[1]) || !Vertices.IsValidIndex(I[2]))
			{
				Keys.Add(0);
				continue;
			}
			const uint64 H[3] = { Vertices[I[0]], Vertices[I[1]], Vertices[I[2]] };
			const int32 S = (H[0] <= H[1] && H[0] <= H[2]) ? 0 : (H[1] <= H[2] ? 1 : 2);
			const uint64 R[3] = { H[S], H[(S + 1) % 3], H[(S + 2) % 3] };
			Keys.Add(CityHash64(reinterpret_cast<const char*>(R), sizeof(R)));
		}
		Keys.Sort();
		return Keys;
	}

	/** Size of the symmetric difference of two sorted arrays: the triangles one mesh has and the other has not. */
	int32 CountDifferent(const TArray<uint64>& A, const TArray<uint64>& B)
	{
		int32 i = 0, j = 0, Different = 0;
		while (i < A.Num() && j < B.Num())
		{
			if (A[i] == B[j])
			{
				++i;
				++j;
			}
			else if (A[i] < B[j])
			{
				++i;
				++Different;
			}
			else
			{
				++j;
				++Different;
			}
		}
		return Different + (A.Num() - i) + (B.Num() - j);
	}

	/** Every chunk mesh and seam bucket the renderer holds, as triangle keys. */
	struct FGeometry
	{
		TMap<FIntVector, TArray<uint64>> Chunks;
		TMap<FIntVector, TArray<uint64>> Seams;
	};

	FGeometry CaptureGeometry(const VoxelTesting::FRecordingMeshRenderer& Renderer)
	{
		FGeometry G;
		for (const TPair<FIntVector, FChunkMeshData>& Pair : Renderer.ChunkMeshes)
		{
			G.Chunks.Add(Pair.Key, TriangleKeys(Pair.Value));
		}
		for (const TPair<FIntVector, FChunkMeshData>& Pair : Renderer.SeamBuckets)
		{
			G.Seams.Add(Pair.Key, TriangleKeys(Pair.Value));
		}
		return G;
	}

	/** Where two geometries differ: chunk meshes and seam buckets (by owner), and their triangles. */
	struct FGeometryDiff
	{
		TArray<FIntVector> Chunks;
		TArray<FIntVector> Seams;
		int32 Triangles = 0;

		FString Describe() const
		{
			FString Text = FString::Printf(TEXT("%d chunk meshes and %d seam buckets differ (%d triangles)"), Chunks.Num(), Seams.Num(), Triangles);
			for (int32 i = 0; i < FMath::Min(4, Chunks.Num()); ++i)
			{
				Text += FString::Printf(TEXT(" chunk (%d,%d,%d)"), Chunks[i].X, Chunks[i].Y, Chunks[i].Z);
			}
			for (int32 i = 0; i < FMath::Min(4, Seams.Num()); ++i)
			{
				Text += FString::Printf(TEXT(" seams of (%d,%d,%d)"), Seams[i].X, Seams[i].Y, Seams[i].Z);
			}
			return Text;
		}
	};

	void DiffMaps(const TMap<FIntVector, TArray<uint64>>& A, const TMap<FIntVector, TArray<uint64>>& B, TArray<FIntVector>& OutKeys, int32& InOutTriangles)
	{
		static const TArray<uint64> Empty;
		TSet<FIntVector> Keys;
		for (const TPair<FIntVector, TArray<uint64>>& Pair : A) { Keys.Add(Pair.Key); }
		for (const TPair<FIntVector, TArray<uint64>>& Pair : B) { Keys.Add(Pair.Key); }
		for (const FIntVector& Key : Keys)
		{
			const TArray<uint64>* InA = A.Find(Key);
			const TArray<uint64>* InB = B.Find(Key);
			const int32 Different = CountDifferent(InA ? *InA : Empty, InB ? *InB : Empty);
			if (Different > 0)
			{
				OutKeys.Add(Key);
				InOutTriangles += Different;
			}
		}
		OutKeys.Sort([](const FIntVector& L, const FIntVector& R) { return L.X != R.X ? L.X < R.X : (L.Y != R.Y ? L.Y < R.Y : L.Z < R.Z); });
	}

	FGeometryDiff Compare(const FGeometry& A, const FGeometry& B)
	{
		FGeometryDiff D;
		DiffMaps(A.Chunks, B.Chunks, D.Chunks, D.Triangles);
		DiffMaps(A.Seams, B.Seams, D.Seams, D.Triangles);
		return D;
	}

	/** A loaded chunk's edit-merged voxels: a hash of their shape (material, density, biome) and of where the water flags are. */
	struct FVoxelDigest
	{
		uint64 Shape = 0;
		uint32 Water = 0;
		int32 WaterVoxels = 0;
	};

	TMap<FIntVector, FVoxelDigest> CaptureVoxels(UVoxelChunkManager& Manager)
	{
		TMap<FIntVector, FVoxelDigest> Out;
		TArray<FIntVector> Loaded;
		Manager.GetLoadedChunks(Loaded);
		for (const FIntVector& Chunk : Loaded)
		{
			const TSharedPtr<const TArray<FVoxelData>> Snapshot = Manager.GetVoxelSnapshot(Chunk);
			if (!Snapshot.IsValid())
			{
				continue;
			}
			TArray<uint32> Packed;
			Packed.SetNumUninitialized(Snapshot->Num());
			FVoxelDigest D;
			for (int32 i = 0; i < Snapshot->Num(); ++i)
			{
				const FVoxelData& V = (*Snapshot)[i];
				Packed[i] = uint32(V.MaterialID) | (uint32(V.Density) << 8) | (uint32(V.BiomeID) << 16);
				if (V.HasWaterFlag())
				{
					D.Water = FCrc::MemCrc32(&i, sizeof(i), D.Water);
					++D.WaterVoxels;
				}
			}
			D.Shape = CityHash64(reinterpret_cast<const char*>(Packed.GetData()), Packed.Num() * sizeof(uint32));
			Out.Add(Chunk, D);
		}
		return Out;
	}

	/** Chunks whose voxels' shape (bWater: water flags) differ between two captures, or that only one holds. */
	int32 DiffVoxels(const TMap<FIntVector, FVoxelDigest>& A, const TMap<FIntVector, FVoxelDigest>& B, bool bWater)
	{
		int32 Different = 0;
		for (const TPair<FIntVector, FVoxelDigest>& Pair : A)
		{
			const FVoxelDigest* Other = B.Find(Pair.Key);
			Different += (!Other || (bWater ? (Other->Water != Pair.Value.Water || Other->WaterVoxels != Pair.Value.WaterVoxels) : Other->Shape != Pair.Value.Shape)) ? 1 : 0;
		}
		for (const TPair<FIntVector, FVoxelDigest>& Pair : B)
		{
			Different += A.Contains(Pair.Key) ? 0 : 1;
		}
		return Different;
	}

	/** Tick until the pipeline (generation, meshing, uploads, seam jobs queued or in flight) has been idle for 90 ticks. */
	bool Settle(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds = 120.0)
	{
		const FVoxelSeamRegistry* Seams = W.Manager->GetSeamRegistry();
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			const bool bIdle = W.Manager->GetPendingGenerationCount() == 0 && W.Manager->GetPendingMeshingCount() == 0
				&& W.Manager->GetAsyncGenerationInProgressCount() == 0 && W.Manager->GetAsyncMeshingInProgressCount() == 0
				&& W.Manager->GetPendingMeshUploadCount() == 0 && W.Manager->GetSeamJobsInFlightCount() == 0
				&& (!Seams || Seams->GetStats().JobQueueDepth == 0);
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 90;
	}

	void TickOnce(VoxelTesting::FTestChunkWorld& W)
	{
		W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	}

	/** Set voxel.Edit.NeighbourRemesh at console priority (a value typed in the editor's console outranks a code set). */
	int32 SetRule(FAutomationTestBase& Test, int32 Value)
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Edit.NeighbourRemesh"));
		const int32 Previous = CVar ? CVar->GetInt() : 0;
		if (CVar)
		{
			CVar->Set(Value, ECVF_SetByConsole);
		}
		if (!CVar || CVar->GetInt() != Value)
		{
			Test.AddError(FString::Printf(TEXT("voxel.Edit.NeighbourRemesh could not be set to %d"), Value));
		}
		return Previous;
	}

	/** One world to stream: the meshing mode, LOD, water and planet or plane. */
	struct FCase
	{
		const TCHAR* Name;
		bool bPlanet;
		EMeshingMode Mode;
		bool bLOD;
		bool bWater;
	};

	/** The meshing work an edit caused (FMeshWorkStats between two points). */
	struct FWork
	{
		int64 ChunkMeshes = 0;
		int64 SeamJobs = 0;
		int64 Broadcasts = 0;
		int64 EditedRemeshes = 0;
		int64 OnTheWay = 0;
		int64 AfterMeshing = 0;
		int64 NeighbourRemeshes = 0;

		static FWork Between(const UVoxelChunkManager::FMeshWorkStats& A, const UVoxelChunkManager::FMeshWorkStats& B)
		{
			FWork W;
			W.ChunkMeshes = B.ChunkMeshJobs - A.ChunkMeshJobs;
			W.SeamJobs = B.SeamJobs - A.SeamJobs;
			W.Broadcasts = B.EditBroadcasts - A.EditBroadcasts;
			W.EditedRemeshes = B.EditedRemeshes - A.EditedRemeshes;
			W.OnTheWay = B.EditedOnTheWay - A.EditedOnTheWay;
			W.AfterMeshing = B.RemeshesAfterMeshing - A.RemeshesAfterMeshing;
			W.NeighbourRemeshes = B.NeighbourRemeshes - A.NeighbourRemeshes;
			return W;
		}

		FString Describe() const
		{
			return FString::Printf(TEXT("%lld chunk meshes, %lld seam jobs (%lld broadcasts: %lld loaded re-meshed, %lld on their way, %lld re-meshed after their mesh landed; %lld face-neighbour re-meshes)"),
				ChunkMeshes, SeamJobs, Broadcasts, EditedRemeshes, OnTheWay, AfterMeshing, NeighbourRemeshes);
		}
	};

	static constexpr int32 NumEdits = 7;
	static const TCHAR* EditNames[NumEdits] = { TEXT("system dig by the min edge"), TEXT("system fill by the max edge"), TEXT("player brush across the edge"),
		TEXT("system edits while the mesh waits (dispatch held)"), TEXT("system edits while the mesh is on a worker"),
		TEXT("a second player brush across the edge, undone"), TEXT("its redo") };

	/** What one streamed run under one rule gave. */
	struct FRunResult
	{
		bool bSettled = true;
		bool bFound = false;
		FVector Q = FVector::ZeroVector;
		FIntVector E = FIntVector::ZeroValue;   // the chunk whose min edge Q is
		FIntVector E2 = FIntVector::ZeroValue;  // the chunk whose max edge Q is
		FIntVector E4 = FIntVector::ZeroValue;  // edit 4's chunk (by E's -B face)
		FIntVector E5 = FIntVector::ZeroValue;  // edit 5's chunk (by E's -A face)
		int32 LoadedChunks = 0;
		EChunkState StateAtQueuedEdit = EChunkState::Unloaded;
		EChunkState StateAtWorkerEdit = EChunkState::Unloaded;
		int32 WorkerAttempts = 0;
		FGeometry Pre;
		TMap<FIntVector, FVoxelDigest> PreVoxels;
		// Per edit, settled: the meshing work it caused, the geometry and voxels, the geometry against a full rebuild made then,
		// and the chunks whose water flags the edit (its water propagation) and that rebuild changed.
		FWork Work[NumEdits];
		FGeometry Post[NumEdits];
		TMap<FIntVector, FVoxelDigest> PostVoxels[NumEdits];
		FGeometryDiff Stale[NumEdits];
		int32 RebuildWaterChanged[NumEdits] = {};
		int32 EditWaterChanged[NumEdits] = {};
		TSet<FIntVector> EditedChunks[NumEdits];  // the chunks each edit's broadcasts named
		// The first edit (one chunk's voxels): what it changed and the seam buckets it changed whose owner it did not edit.
		TSet<FIntVector> FirstEdited;
		FGeometryDiff FirstChanged;
		int32 ChangedSeamsOfUneditedOwners = 0;
		int32 FaceNeighboursWithWater = 0;      // E's face neighbours with water flags before the edits
	};

	FVector OriginOf(const VoxelTesting::FTestChunkWorld& W) { return W.Config->WorldOrigin; }

	/** The surface along local up through P: the heightmap's height at (X, Y), or the planet's surface point along P's direction. */
	FVector SurfaceAlongUp(VoxelTesting::FTestChunkWorld& W, const FVector& P, bool bPlanet)
	{
		if (bPlanet)
		{
			return W.Manager->GetSurfacePointAt(P).Position;
		}
		return FVector(P.X, P.Y, W.Manager->GetGeneratedSurfacePoint(FVector(P.X, P.Y, 0.0)).Z);
	}

	/** P with its two axes across local up (A, B) moved onto the nearest chunk boundaries. */
	FVector SnapToEdge(const VoxelTesting::FTestChunkWorld& W, const FVector& P, int32 A, int32 B)
	{
		const double CW = W.Config->GetChunkWorldSize();
		FVector Rel = P - OriginOf(W);
		Rel[A] = FMath::RoundToDouble(Rel[A] / CW) * CW;
		Rel[B] = FMath::RoundToDouble(Rel[B] / CW) * CW;
		return OriginOf(W) + Rel;
	}

	/** The surface on the chunk edge along local up nearest P. */
	FVector EdgeSurfacePoint(VoxelTesting::FTestChunkWorld& W, const FVector& P, bool bPlanet, int32 A, int32 B)
	{
		FVector Q = SnapToEdge(W, SurfaceAlongUp(W, SnapToEdge(W, P, A, B), bPlanet), A, B);
		return Q;
	}

	/** One system edit, as one operation: every voxel of Chunk within RadiusVoxels of Center dug to air or filled with stone. */
	int32 SystemSphere(VoxelTesting::FTestChunkWorld& W, const FIntVector& Chunk, const FVector& Center, float RadiusVoxels, bool bFill)
	{
		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		const int32 CS = W.Config->ChunkSize;
		const float VS = W.Config->VoxelSize;
		const FVector ChunkMin = OriginOf(W) + FVector(Chunk) * W.Config->GetChunkWorldSize();
		const double R2 = FMath::Square(RadiusVoxels * VS);
		const EEditSource Previous = Edits->GetEditSource();
		Edits->SetEditSource(EEditSource::System);
		Edits->BeginEditOperation(TEXT("G56 test"));
		int32 Count = 0;
		for (int32 Z = 0; Z < CS; ++Z)
		for (int32 Y = 0; Y < CS; ++Y)
		for (int32 X = 0; X < CS; ++X)
		{
			const FVector Pos = ChunkMin + (FVector(X, Y, Z) + FVector(0.5)) * VS;
			if (FVector::DistSquared(Pos, Center) > R2)
			{
				continue;
			}
			const FVoxelData Current = W.Manager->GetVoxelAt(Pos);
			const FVoxelData NewData = bFill ? FVoxelData::Solid(Current.IsSolid() ? Current.MaterialID : static_cast<uint8>(EVoxelMaterial::Stone), Current.BiomeID)
				: FVoxelData::Air();
			Count += Edits->ApplyEdit(Pos, NewData, EEditMode::Set) ? 1 : 0;
		}
		Edits->EndEditOperation();
		Edits->SetEditSource(Previous);
		return Count;
	}

	/** A player's sphere brush (dig) of RadiusVoxels at Center. */
	int32 PlayerBrush(VoxelTesting::FTestChunkWorld& W, const FVector& Center, float RadiusVoxels)
	{
		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		const EEditSource Previous = Edits->GetEditSource();
		Edits->SetEditSource(EEditSource::Player);
		FVoxelBrushParams Brush;
		Brush.Shape = EVoxelBrushShape::Sphere;
		Brush.Radius = RadiusVoxels * W.Config->VoxelSize;
		Brush.Strength = 1.0f;
		Brush.FalloffType = EVoxelBrushFalloff::Sharp;
		Brush.DensityDelta = 255;
		Brush.MaterialID = EVoxelMaterial::Stone;
		const int32 Count = Edits->ApplyBrushEdit(Center, Brush, EEditMode::Subtract);
		Edits->SetEditSource(Previous);
		return Count;
	}

	/** Configure, start and settle a world for Case; false if no site qualifies. */
	bool StartWorld(VoxelTesting::FTestChunkWorld& W, const FCase& Case, FVector& OutSite)
	{
		W.Create(Case.bPlanet ? TEXT("EditRemeshPlanet") : TEXT("EditRemeshPlane"));
		if (Case.bPlanet)
		{
			W.ConfigureSmallPlanet();
		}
		else
		{
			W.ConfigureSmallPlane();
		}
		W.Config->MeshingMode = Case.Mode;
		if (Case.bLOD)
		{
			W.Config->bEnableLOD = true;
			W.Config->ViewDistance = 3600.0f;
			W.Config->LODBands = { FLODBand(0.0f, 1800.0f, 0, 1), FLODBand(1800.0f, 3600.0f, 1, 2) };
		}
		if (Case.bWater)
		{
			W.Config->bEnableWaterLevel = true;
			if (Case.bPlanet)
			{
				W.Config->WaterRadius = W.Config->WorldRadius; // a sea at the datum
				W.Config->WaterLevel = 1.0e9f;                  // the Z plane far above everything plays no part on a planet
			}
			else
			{
				W.Config->WaterLevel = 0.0f;
			}
		}
		W.StartManager();

		// The chunk edge along local up nearest the base point where the surface crosses it, for a water case one where the
		// surface lies a few voxels under the sea: on a plane the edges at whole chunks from (1, 0) chunks, on a planet those
		// around the equator of the +X face (local up +X).
		const double CW = W.Config->GetChunkWorldSize();
		const float VS = W.Config->VoxelSize;
		const int32 UpAxis = Case.bPlanet ? 0 : 2;
		const int32 A = (UpAxis + 1) % 3, B = (UpAxis + 2) % 3;
		bool bFound = false;
		double Best = 1.0e30;
		for (int32 j = -6; j <= 6; ++j)
		for (int32 i = -6; i <= 6; ++i)
		{
			const FVector Base = Case.bPlanet ? OriginOf(W) + FVector(W.Config->WorldRadius, i * CW, j * CW) : FVector((1 + i) * CW, j * CW, 0.0);
			const FVector Edge = EdgeSurfacePoint(W, Base, Case.bPlanet, A, B);
			const double Altitude = Case.bPlanet ? (Edge - OriginOf(W)).Size() - W.Config->WaterRadius : Edge.Z - W.Config->WaterLevel;
			if (Case.bWater && (Altitude > -3.0 * VS || Altitude < -12.0 * VS))
			{
				continue;
			}
			const double Distance = FMath::Square(double(i)) + FMath::Square(double(j));
			if (Distance < Best)
			{
				Best = Distance;
				OutSite = Edge;
				bFound = true;
			}
		}
		if (!bFound)
		{
			return false;
		}
		const FVector Up = Case.bPlanet ? (OutSite - OriginOf(W)).GetSafeNormal() : FVector::UpVector;
		W.SetViewer(OutSite + Up * 300.0);
		return true;
	}

	FRunResult RunCase(FAutomationTestBase& Test, const FCase& Case, int32 Rule)
	{
		FRunResult R;
		const int32 PreviousRule = SetRule(Test, Rule);
		VoxelTesting::FTestChunkWorld W;
		FVector Site;
		R.bFound = StartWorld(W, Case, Site);
		if (!R.bFound)
		{
			W.Stop();
			SetRule(Test, PreviousRule);
			return R;
		}
		R.bSettled = Settle(W);
		R.LoadedChunks = W.Manager->GetLoadedChunkCount();
		R.Pre = CaptureGeometry(W.Renderer);
		R.PreVoxels = CaptureVoxels(*W.Manager);

		// The chunk edge running along local up nearest the site, at the surface: its min edge belongs to E, its max edge to E2.
		const int32 UpAxis = Case.bPlanet ? 0 : 2;
		const int32 A = (UpAxis + 1) % 3, B = (UpAxis + 2) % 3;
		FVector AxisA = FVector::ZeroVector, AxisB = FVector::ZeroVector;
		AxisA[A] = 1.0;
		AxisB[B] = 1.0;
		const float VS = W.Config->VoxelSize;
		R.Q = EdgeSurfacePoint(W, Site, Case.bPlanet, A, B);
		const FVector Up = Case.bPlanet ? (R.Q - OriginOf(W)).GetSafeNormal() : FVector::UpVector;
		R.E = W.Manager->WorldToChunkCoord(R.Q + (AxisA + AxisB) * 0.5 * VS - Up * VS);
		R.E2 = W.Manager->WorldToChunkCoord(R.Q - (AxisA + AxisB) * 0.5 * VS - Up * VS);

		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		TSet<FIntVector> Edited;
		const FDelegateHandle Handle = Edits->OnChunkEdited.AddLambda([&Edited](const FIntVector& Chunk, EEditSource, const FVector&, float) { Edited.Add(Chunk); });

		// Each edit is measured and settled, then checked against a full rebuild (every chunk re-meshed, every seam rebuilt from the
		// voxels as they are), which also gives the next edit a clean start.
		TMap<FIntVector, FVoxelDigest> VoxelsBefore = R.PreVoxels;
		auto Measure = [&](int32 Index, TFunctionRef<void()> Edit)
		{
			Edited.Reset();
			const UVoxelChunkManager::FMeshWorkStats Before = W.Manager->GetMeshWorkStats();
			Edit();
			R.bSettled = Settle(W) && R.bSettled;
			R.Work[Index] = FWork::Between(Before, W.Manager->GetMeshWorkStats());
			R.Post[Index] = CaptureGeometry(W.Renderer);
			R.PostVoxels[Index] = CaptureVoxels(*W.Manager);
			R.EditWaterChanged[Index] = DiffVoxels(VoxelsBefore, R.PostVoxels[Index], true);
			R.EditedChunks[Index] = Edited;
			if (Index == 0)
			{
				R.FirstEdited = Edited;
				R.FirstChanged = Compare(R.Pre, R.Post[0]);
				for (const FIntVector& Owner : R.FirstChanged.Seams)
				{
					R.ChangedSeamsOfUneditedOwners += Edited.Contains(Owner) ? 0 : 1;
				}
				static const FIntVector Faces[6] = { FIntVector(1, 0, 0), FIntVector(-1, 0, 0), FIntVector(0, 1, 0), FIntVector(0, -1, 0), FIntVector(0, 0, 1), FIntVector(0, 0, -1) };
				for (const FIntVector& Face : Faces)
				{
					const FVoxelDigest* Digest = R.PreVoxels.Find(R.E + Face);
					R.FaceNeighboursWithWater += (Digest && Digest->WaterVoxels > 0) ? 1 : 0;
				}
			}
			TArray<FIntVector> Loaded;
			W.Manager->GetLoadedChunks(Loaded);
			for (const FIntVector& Chunk : Loaded)
			{
				W.Manager->MarkChunkDirty(Chunk);
			}
			R.bSettled = Settle(W) && R.bSettled;
			R.Stale[Index] = Compare(R.Post[Index], CaptureGeometry(W.Renderer));
			VoxelsBefore = CaptureVoxels(*W.Manager);
			R.RebuildWaterChanged[Index] = DiffVoxels(R.PostVoxels[Index], VoxelsBefore, true);
		};

		// 1. E's voxels by its min edge: the face and edge seams there are owned by E's unedited -A / -B neighbours.
		Measure(0, [&]() { SystemSphere(W, R.E, R.Q, 3.5f, false); });
		// 2. E2's voxels by its max edge: the seams there are E2's own.
		Measure(1, [&]() { SystemSphere(W, R.E2, R.Q, 3.5f, true); });
		// 3. A player brush across the edge (the four chunks around it).
		Measure(2, [&]() { PlayerBrush(W, R.Q, 2.5f); });
		// 4. Two edits of a chunk by E's -B face while it waits for its mesh: mesh dispatch held, so the first edit's seams are
		//    built before the second lands.
		{
			const FVector Q4 = SurfaceAlongUp(W, R.Q + AxisA * 6.0 * VS, Case.bPlanet);
			R.E4 = W.Manager->WorldToChunkCoord(Q4 + AxisB * 0.5 * VS - Up * VS);
			Measure(3, [&]()
			{
				const int32 HeldTasks = W.Config->MaxAsyncMeshTasks;
				W.Config->MaxAsyncMeshTasks = 0;
				SystemSphere(W, R.E4, Q4, 2.5f, false);
				for (int32 t = 0; t < 4; ++t)
				{
					TickOnce(W);
				}
				R.StateAtQueuedEdit = W.Manager->GetChunkState(R.E4);
				SystemSphere(W, R.E4, Q4, 1.5f, true);
				W.Config->MaxAsyncMeshTasks = HeldTasks;
			});
		}
		// 5. Two edits of a chunk by E's -A face, the second while its mesh is on a worker (dispatched in the tick after the first;
		//    its seams are scheduled in that tick too).
		Measure(4, [&]()
		{
			for (int32 Attempt = 0; Attempt < 5; ++Attempt)
			{
				++R.WorkerAttempts;
				const FVector Q5 = SurfaceAlongUp(W, R.Q + AxisB * (6.0 + 3.0 * Attempt) * VS, Case.bPlanet);
				R.E5 = W.Manager->WorldToChunkCoord(Q5 + AxisA * 0.5 * VS - Up * VS);
				SystemSphere(W, R.E5, Q5, 2.5f, false);
				for (int32 t = 0; t < 20 && W.Manager->GetChunkState(R.E5) == EChunkState::PendingMeshing; ++t)
				{
					TickOnce(W);
				}
				R.StateAtWorkerEdit = W.Manager->GetChunkState(R.E5);
				if (R.StateAtWorkerEdit == EChunkState::Meshing)
				{
					SystemSphere(W, R.E5, Q5, 1.5f, true);
					break;
				}
				R.bSettled = Settle(W) && R.bSettled;
			}
		});
		// 6. A second player brush across the edge (the four chunks), settled, then undone (G60).
		Measure(5, [&]()
		{
			PlayerBrush(W, R.Q, 2.5f);
			R.bSettled = Settle(W) && R.bSettled;
			Edits->Undo();
		});
		// 7. Its redo.
		Measure(6, [&]() { Edits->Redo(); });
		Edits->OnChunkEdited.Remove(Handle);

		W.Stop();
		SetRule(Test, PreviousRule);
		return R;
	}

	FString StateName(EChunkState State)
	{
		switch (State)
		{
		case EChunkState::Loaded: return TEXT("Loaded");
		case EChunkState::PendingMeshing: return TEXT("PendingMeshing");
		case EChunkState::Meshing: return TEXT("Meshing");
		case EChunkState::Generating: return TEXT("Generating");
		case EChunkState::PendingUnload: return TEXT("PendingUnload");
		case EChunkState::Unloaded: return TEXT("Unloaded");
		default: return FString::Printf(TEXT("state %d"), static_cast<int32>(State));
		}
	}

	void RunCases(FAutomationTestBase& Test, const TArray<FCase>& Cases)
	{
		for (const FCase& Case : Cases)
		{
			FRunResult Runs[2];
			for (const int32 Rule : { 0, 1 })
			{
				FRunResult& R = Runs[Rule];
				R = RunCase(Test, Case, Rule);
				const FString Name = FString::Printf(TEXT("%s, voxel.Edit.NeighbourRemesh %d"), Case.Name, Rule);
				if (!Test.TestTrue(FString::Printf(TEXT("%s: a site (non-vacuous)"), *Name), R.bFound))
				{
					continue;
				}
				Test.AddInfo(FString::Printf(TEXT("%s: %d chunks loaded; edge point (%.0f, %.0f, %.0f), E (%d,%d,%d) (%d face neighbours with water flags), E2 (%d,%d,%d), edit 4 in (%d,%d,%d), edit 5 in (%d,%d,%d); the first edit changed %d chunks' voxels and %s, %d seam buckets of owners it did not edit; its chunk was %s at the second of edit 4's edits (dispatch held), %s at the second of edit 5's (%d attempts)"),
					*Name, R.LoadedChunks, R.Q.X, R.Q.Y, R.Q.Z, R.E.X, R.E.Y, R.E.Z, R.FaceNeighboursWithWater, R.E2.X, R.E2.Y, R.E2.Z, R.E4.X, R.E4.Y, R.E4.Z, R.E5.X, R.E5.Y, R.E5.Z, R.FirstEdited.Num(), *R.FirstChanged.Describe(),
					R.ChangedSeamsOfUneditedOwners, *StateName(R.StateAtQueuedEdit), *StateName(R.StateAtWorkerEdit), R.WorkerAttempts));
				int64 NeighbourRemeshes = 0;
				for (int32 i = 0; i < NumEdits; ++i)
				{
					Test.AddInfo(FString::Printf(TEXT("%s: edit %d (%s): %s; water flags changed in %d chunks; against a full rebuild: %s; the rebuild changed %d chunks' water flags"), *Name, i + 1,
						EditNames[i], *R.Work[i].Describe(), R.EditWaterChanged[i], *R.Stale[i].Describe(), R.RebuildWaterChanged[i]));
					Test.TestEqual(FString::Printf(TEXT("%s: edit %d (%s): no chunk mesh differs from a full rebuild"), *Name, i + 1, EditNames[i]), R.Stale[i].Chunks.Num(), 0);
					Test.TestEqual(FString::Printf(TEXT("%s: edit %d (%s): no seam bucket differs from a full rebuild"), *Name, i + 1, EditNames[i]), R.Stale[i].Seams.Num(), 0);
					NeighbourRemeshes += R.Work[i].NeighbourRemeshes;
				}
				Test.TestTrue(FString::Printf(TEXT("%s: streaming settles"), *Name), R.bSettled);
				// The redo (edit 7) re-applies a brush at edit 3's place: it names the same chunks (G60: it named only the first).
				Test.TestTrue(FString::Printf(TEXT("%s: edit 3's brush reaches several chunks (non-vacuous: %d)"), *Name, R.EditedChunks[2].Num()),
					R.EditedChunks[2].Num() >= 2);
				Test.TestTrue(FString::Printf(TEXT("%s: edit 7 (the redo) broadcasts every chunk edit 3's brush did (%d against %d)"), *Name,
					R.EditedChunks[6].Num(), R.EditedChunks[2].Num()), R.EditedChunks[6].Includes(R.EditedChunks[2]));
				if (Case.bWater)
				{
					Test.TestTrue(FString::Printf(TEXT("%s: the first edit's chunk has face neighbours with water flags, whose re-mesh pulls flags across their faces (%d; non-vacuous)"),
						*Name, R.FaceNeighboursWithWater), R.FaceNeighboursWithWater > 0);
				}
				Test.TestTrue(FString::Printf(TEXT("%s: the first edit changed seam buckets of owners it did not edit (non-vacuous)"), *Name), R.ChangedSeamsOfUneditedOwners > 0);
				Test.TestEqual(FString::Printf(TEXT("%s: edit 4's chunk waits for its mesh at its second edit (non-vacuous)"), *Name), StateName(R.StateAtQueuedEdit),
					StateName(EChunkState::PendingMeshing));
				Test.TestTrue(FString::Printf(TEXT("%s: edit 4's second edit is counted on its way to a mesh"), *Name), R.Work[3].OnTheWay >= 1);
				if (R.StateAtWorkerEdit == EChunkState::Meshing)
				{
					// The rule before also re-meshes the face neighbours whose re-mesh for the first edit is on a worker.
					if (Rule == 0)
					{
						Test.TestEqual(FString::Printf(TEXT("%s: edit 5: the chunk re-meshes once after the mesh on the worker lands (G57)"), *Name), R.Work[4].AfterMeshing, int64(1));
					}
					else
					{
						Test.TestTrue(FString::Printf(TEXT("%s: edit 5: the chunk re-meshes after the mesh on the worker lands (G57)"), *Name), R.Work[4].AfterMeshing >= 1);
					}
				}
				else
				{
					Test.AddWarning(FString::Printf(TEXT("%s: edit 5 never found its chunk's mesh on a worker (%d attempts): the case went untested"), *Name, R.WorkerAttempts));
				}
				if (Rule == 0)
				{
					Test.TestEqual(FString::Printf(TEXT("%s: no face-neighbour re-meshes"), *Name), NeighbourRemeshes, int64(0));
				}
				else
				{
					Test.TestTrue(FString::Printf(TEXT("%s: face neighbours re-mesh (the rule before)"), *Name), NeighbourRemeshes > 0);
				}
			}
			if (!Runs[0].bFound || !Runs[1].bFound)
			{
				continue;
			}

			// The two rules: the same world before the edits, then after each edit the same geometry and voxel shape. Water flags
			// are reported: the face neighbours' re-mesh pulled flags across their faces (PropagateWaterFromNeighbors).
			const FGeometryDiff BetweenPre = Compare(Runs[0].Pre, Runs[1].Pre);
			int32 WaterChunks = 0;
			for (const TPair<FIntVector, FVoxelDigest>& Pair : Runs[0].PreVoxels)
			{
				WaterChunks += Pair.Value.WaterVoxels > 0 ? 1 : 0;
			}
			Test.AddInfo(FString::Printf(TEXT("%s: rule 0 against rule 1 before the edits: %s; %d chunks' voxel shape and %d chunks' water flags differ (%d chunks with water flags)"),
				Case.Name, *BetweenPre.Describe(), DiffVoxels(Runs[0].PreVoxels, Runs[1].PreVoxels, false), DiffVoxels(Runs[0].PreVoxels, Runs[1].PreVoxels, true), WaterChunks));
			Test.TestEqual(FString::Printf(TEXT("%s: both rules stream the same geometry before the edits"), Case.Name), BetweenPre.Chunks.Num() + BetweenPre.Seams.Num(), 0);
			for (int32 i = 0; i < NumEdits; ++i)
			{
				const FGeometryDiff Between = Compare(Runs[0].Post[i], Runs[1].Post[i]);
				const int32 Shape = DiffVoxels(Runs[0].PostVoxels[i], Runs[1].PostVoxels[i], false);
				const int32 Water = DiffVoxels(Runs[0].PostVoxels[i], Runs[1].PostVoxels[i], true);
				Test.AddInfo(FString::Printf(TEXT("%s: rule 0 against rule 1 after edit %d: %s; %d chunks' voxel shape and %d chunks' water flags differ"), Case.Name, i + 1,
					*Between.Describe(), Shape, Water));
				Test.TestEqual(FString::Printf(TEXT("%s: edit %d: no chunk mesh or seam bucket differs between the rules"), Case.Name, i + 1), Between.Chunks.Num() + Between.Seams.Num(), 0);
				Test.TestEqual(FString::Printf(TEXT("%s: edit %d: no chunk's voxel shape differs between the rules"), Case.Name, i + 1), Shape, 0);
			}
			if (Case.bWater)
			{
				Test.TestTrue(FString::Printf(TEXT("%s: chunks with water flags (non-vacuous: %d)"), Case.Name, WaterChunks), WaterChunks > 0);
			}
			const int64 Meshes0 = Runs[0].Work[0].ChunkMeshes, Meshes1 = Runs[1].Work[0].ChunkMeshes;
			Test.TestTrue(FString::Printf(TEXT("%s: the first edit meshes fewer chunks without the face neighbours' re-mesh (%lld against %lld)"), Case.Name, Meshes0, Meshes1),
				Meshes0 < Meshes1);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditRemeshPlaneTest, "VoxelWorlds.Streaming.EditRemesh.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelEditRemeshPlaneTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditRemeshTest;
	RunCases(*this, {
		{ TEXT("Plane DC"), false, EMeshingMode::DualContouring, false, false },
		{ TEXT("Plane DC LOD"), false, EMeshingMode::DualContouring, true, false },
		{ TEXT("Plane MC LOD"), false, EMeshingMode::MarchingCubes, true, false },
		{ TEXT("Plane cubic"), false, EMeshingMode::Cubic, false, false },
		{ TEXT("Plane DC water"), false, EMeshingMode::DualContouring, false, true } });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditRemeshPlanetTest, "VoxelWorlds.Streaming.EditRemesh.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelEditRemeshPlanetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelEditRemeshTest;
	RunCases(*this, {
		{ TEXT("Planet DC"), true, EMeshingMode::DualContouring, false, false },
		{ TEXT("Planet DC LOD water"), true, EMeshingMode::DualContouring, true, true },
		{ TEXT("Planet MC LOD"), true, EMeshingMode::MarchingCubes, true, false } });
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
