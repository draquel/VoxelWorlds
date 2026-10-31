// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelMeshingTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Cubic streaming on the seam path (world modes review WM-C1, KC2/KC14).
//
// A real chunk manager streams a small cubic world (CPU generation, LOD forced off) into a renderer that
// records what it is given. Once the pipeline settles:
//  - every chunk was meshed exactly once: no chunk waited for, or re-meshed on, a neighbour's data;
//  - the interiors plus the face-seam buckets reproduce exactly what meshing each chunk whole against its
//    loaded neighbours' voxels gives (the legacy boundary path's result): no face missing on a chunk
//    plane, none doubled, none on a plane whose other side is not loaded (KC13: no wall at the edge of
//    the loaded region);
//  - the seam registry tracks face seams only, and every seam between two loaded chunks was built.
//
// Because each chunk is meshed once, a mesh the renderer loses is never replaced by a later re-mesh. The
// second test drops everything the renderer holds, as a recreated scene proxy does, and checks that the
// manager sends it all again (UVoxelChunkManager::RecoverLostRenderData).
// ---------------------------------------------------------------------------

namespace VoxelCubicStreamingTestUtils
{
	/** A renderer that keeps the latest chunk mesh and merged seam bucket per coordinate. */
	class FRecordingMeshRenderer : public IVoxelMeshRenderer
	{
	public:
		TMap<FIntVector, FChunkMeshData> ChunkMeshes;
		TMap<FIntVector, FChunkMeshData> SeamBuckets;
		int32 ChunkSubmits = 0;
		int32 SeamSubmits = 0;

		virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override { bInitialized = true; }
		virtual void Shutdown() override { bInitialized = false; }
		virtual bool IsInitialized() const override { return bInitialized; }
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override
		{
			++ChunkSubmits;
			ChunkMeshes.Add(ChunkCoord, MeshData);
		}
		virtual void RemoveChunk(const FIntVector& ChunkCoord) override { ChunkMeshes.Remove(ChunkCoord); }
		virtual void ClearAllChunks() override { ChunkMeshes.Reset(); }
		virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override {}
		virtual void SetAllChunksVisible(bool bVisible) override {}
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
		virtual void UpdateSeamMeshFromCPU(const FIntVector& OwnerChunkCoord, uint8 SeamSlot, int32 LODLevel, FChunkMeshData&& MeshData, bool bCrossfade) override
		{
			++SeamSubmits;
			if (MeshData.IsValid())
			{
				SeamBuckets.Add(OwnerChunkCoord, MoveTemp(MeshData));
			}
			else
			{
				SeamBuckets.Remove(OwnerChunkCoord);
			}
		}
		virtual void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 SeamSlot) override { SeamBuckets.Remove(OwnerChunkCoord); }
		virtual void ClearAllSeamMeshes() override { SeamBuckets.Reset(); }
		virtual void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const override { SeamBuckets.GetKeys(OutOwners); }
		virtual void UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor) override {}
		virtual bool IsChunkLoaded(const FIntVector& ChunkCoord) const override { return ChunkMeshes.Contains(ChunkCoord); }
		virtual int32 GetLoadedChunkCount() const override { return ChunkMeshes.Num(); }
		virtual void GetLoadedChunks(TArray<FIntVector>& OutChunks) const override { ChunkMeshes.GetKeys(OutChunks); }
		virtual int64 GetGPUMemoryUsage() const override { return 0; }
		virtual int64 GetTotalVertexCount() const override { return 0; }
		virtual int64 GetTotalTriangleCount() const override { return 0; }
		virtual bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const override { return false; }
		virtual FBox GetTotalBounds() const override { return FBox(ForceInit); }
		virtual FString GetDebugStats() const override { return FString(); }
		virtual FString GetRendererTypeName() const override { return TEXT("Recording (test)"); }

		/** Drop everything held, as a recreated scene proxy does, and report it once through ConsumeLostRenderData. */
		void SimulateProxyRecreated()
		{
			ChunkMeshes.GetKeys(LostChunks);
			ChunkMeshes.Reset();
			SeamBuckets.Reset();
			bLost = true;
		}

		virtual bool ConsumeLostRenderData(TArray<FIntVector>& OutChunks) override
		{
			OutChunks.Reset();
			if (!bLost)
			{
				return false;
			}
			bLost = false;
			OutChunks = MoveTemp(LostChunks);
			return true;
		}

	private:
		bool bInitialized = false;
		bool bLost = false;
		TArray<FIntVector> LostChunks;
	};

	/** A unit face in global voxel units: (min corner of the unit square, Dir * 256 + MaterialID). */
	using FUnitFace = FIntVector4;

	/** Split each emitted quad into unit faces in global voxel units (OffsetVoxels = chunk coord * size). */
	bool CollectUnitFaces(const FChunkMeshData& Mesh, float VoxelSize, const FIntVector& OffsetVoxels, TMap<FUnitFace, int32>& OutFaces)
	{
		if (Mesh.Positions.Num() % 4 != 0)
		{
			return false;
		}
		for (int32 Base = 0; Base < Mesh.Positions.Num(); Base += 4)
		{
			FIntVector Min(INT32_MAX), Max(INT32_MIN);
			for (int32 i = 0; i < 4; ++i)
			{
				const FVector3f& P = Mesh.Positions[Base + i];
				const FIntVector V(FMath::RoundToInt(P.X / VoxelSize), FMath::RoundToInt(P.Y / VoxelSize), FMath::RoundToInt(P.Z / VoxelSize));
				Min = FIntVector(FMath::Min(Min.X, V.X), FMath::Min(Min.Y, V.Y), FMath::Min(Min.Z, V.Z));
				Max = FIntVector(FMath::Max(Max.X, V.X), FMath::Max(Max.Y, V.Y), FMath::Max(Max.Z, V.Z));
			}
			const FVector3f& Nrm = Mesh.Normals[Base];
			const int32 Axis = FMath::Abs(Nrm.X) > 0.5f ? 0 : (FMath::Abs(Nrm.Y) > 0.5f ? 1 : 2);
			if (Min[Axis] != Max[Axis])
			{
				return false;
			}
			const int32 Dir = Axis * 2 + (Nrm[Axis] > 0.0f ? 0 : 1);
			const int32 U = (Axis + 1) % 3;
			const int32 V = (Axis + 2) % 3;
			const int32 Mat = FMath::RoundToInt(Mesh.UV1s[Base].X);
			for (int32 a = Min[U]; a < Max[U]; ++a)
			for (int32 b = Min[V]; b < Max[V]; ++b)
			{
				FIntVector Cell;
				Cell[Axis] = Min[Axis];
				Cell[U] = a;
				Cell[V] = b;
				Cell += OffsetVoxels;
				OutFaces.FindOrAdd(FUnitFace(Cell.X, Cell.Y, Cell.Z, Dir * 256 + Mat))++;
			}
		}
		return true;
	}

	/** One face plane of a chunk as the whole-chunk mesher's neighbour slice expects it. */
	TArray<FVoxelData> FaceSlice(const TArray<FVoxelData>& Neighbor, int32 N, int32 Axis, int32 Layer)
	{
		TArray<FVoxelData> Slice;
		Slice.SetNum(N * N);
		for (int32 B = 0; B < N; ++B)
		for (int32 A = 0; A < N; ++A)
		{
			int32 C[3];
			C[Axis] = Layer;
			// Slice layout (VoxelCPUCubicMesher::GetSliceVoxel): X faces Y + Z*N, Y faces X + Z*N, Z faces X + Y*N.
			const int32 First = (Axis == 0) ? 1 : 0;
			const int32 Second = (Axis == 2) ? 1 : 2;
			C[First] = A;
			C[Second] = B;
			Slice[A + B * N] = Neighbor[C[0] + C[1] * N + C[2] * N * N];
		}
		return Slice;
	}

	/** Every quad the renderer holds (interiors and seam buckets) as unit faces in global voxel units. */
	bool CollectRendered(const FRecordingMeshRenderer& Renderer, float VoxelSize, int32 N, TMap<FUnitFace, int32>& OutFaces)
	{
		bool bOnGrid = true;
		for (const TPair<FIntVector, FChunkMeshData>& Chunk : Renderer.ChunkMeshes)
		{
			bOnGrid &= CollectUnitFaces(Chunk.Value, VoxelSize, Chunk.Key * N, OutFaces);
		}
		for (const TPair<FIntVector, FChunkMeshData>& Bucket : Renderer.SeamBuckets)
		{
			bOnGrid &= CollectUnitFaces(Bucket.Value, VoxelSize, Bucket.Key * N, OutFaces);
		}
		return bOnGrid;
	}

	/** A small cubic world (CPU generation, LOD forced off) streamed by a real chunk manager into the recording renderer. */
	struct FCubicStreamingWorld
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		AActor* Owner = nullptr;
		UVoxelChunkManager* Manager = nullptr;
		FRecordingMeshRenderer Renderer;

		void Start(const TCHAR* WorldName)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);

			Config = NewObject<UVoxelWorldConfiguration>();
			Config->AddToRoot();
			Config->WorldMode = EWorldMode::InfinitePlane;
			Config->MeshingMode = EMeshingMode::Cubic;
			Config->bUseGreedyMeshing = true;
			Config->bEnableLOD = true; // ignored for cubic (D7): the run must still mesh every chunk once
			Config->ChunkSize = 16;
			Config->VoxelSize = 100.0f;
			Config->ViewDistance = 4000.0f;
			Config->bUseGPUGeneration = false;
			Config->bGenerateCollision = false;
			Config->bEnableScatter = false;
			Config->bEnableWaterLevel = false;
			Config->bEnableCaves = false;
			Config->NoiseParams.Seed = 4242;
			Config->NoiseParams.Frequency = 0.002f;
			Config->NoiseParams.Octaves = 3;
			// Clamp the bands to the view distance as the world actor does.
			for (FLODBand& Band : Config->LODBands)
			{
				Band.MaxDistance = FMath::Min(Band.MaxDistance, Config->ViewDistance);
				Band.MinDistance = FMath::Min(Band.MinDistance, Config->ViewDistance);
			}
			Config->LODBands.RemoveAll([this](const FLODBand& Band) { return Band.MinDistance >= Config->ViewDistance; });

			Owner = World->SpawnActor<AActor>();
			Manager = NewObject<UVoxelChunkManager>(Owner);
			Manager->RegisterComponent();
			Renderer.Initialize(World, Config);
			Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);

			// Stream from a fixed viewer standing on the terrain at the origin.
			const float SurfaceZ = Manager->GetGeneratedSurfaceHeight(0.0f, 0.0f);
			Manager->ResetBenchCounters();
			Manager->SetBenchmarkView(true, FVector(0.0, 0.0, SurfaceZ + 200.0));
		}

		/** Tick until the pipeline has been idle for 90 ticks; false if it does not settle within 90 s. */
		bool Settle()
		{
			int32 IdleTicks = 0;
			const double Deadline = FPlatformTime::Seconds() + 90.0;
			while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
			{
				Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
				const bool bIdle = Manager->GetPendingGenerationCount() == 0 && Manager->GetPendingMeshingCount() == 0
					&& Manager->GetAsyncGenerationInProgressCount() == 0 && Manager->GetAsyncMeshingInProgressCount() == 0
					&& Manager->GetPendingMeshUploadCount() == 0 && Manager->GetSeamJobsInFlightCount() == 0;
				IdleTicks = bIdle ? IdleTicks + 1 : 0;
				FPlatformProcess::Sleep(0.002f);
			}
			return IdleTicks >= 90;
		}

		void Stop()
		{
			Manager->SetBenchmarkView(false, FVector::ZeroVector);
			Manager->Shutdown();
			Owner->Destroy();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			Config->RemoveFromRoot();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCubicStreamingSeamPathTest,
	"VoxelWorlds.Streaming.Cubic.SeamPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCubicStreamingSeamPathTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCubicStreamingTestUtils;

	FCubicStreamingWorld Test;
	Test.Start(TEXT("VoxelCubicStreamingTestWorld"));
	UVoxelWorldConfiguration* Config = Test.Config;
	UVoxelChunkManager* Manager = Test.Manager;
	FRecordingMeshRenderer& Renderer = Test.Renderer;
	TestTrue(TEXT("The pipeline settled"), Test.Settle());

	TArray<FIntVector> Loaded;
	Manager->GetLoadedChunks(Loaded);
	const TSet<FIntVector> LoadedSet(Loaded);
	const int32 N = Config->ChunkSize;
	AddInfo(FString::Printf(TEXT("%d chunks loaded, %d chunk submits, %d seam submits, %lld re-meshes"),
		Loaded.Num(), Renderer.ChunkSubmits, Renderer.SeamSubmits, Manager->GetBenchRemeshCount()));
	TestTrue(TEXT("A neighbourhood streamed in"), Loaded.Num() >= 27);

	// Each chunk was meshed once: nothing waited for or re-meshed on a neighbour (KC2, KC14).
	TestEqual(TEXT("No chunk was meshed twice"), Manager->GetBenchRemeshCount(), (int64)0);
	TestEqual(TEXT("One chunk submit per loaded chunk"), Renderer.ChunkSubmits, Loaded.Num());

	// What the renderer holds: interiors and the merged face-seam buckets, in global voxel units.
	for (const TPair<FIntVector, FChunkMeshData>& Bucket : Renderer.SeamBuckets)
	{
		TestTrue(TEXT("Seam buckets belong to loaded chunks"), LoadedSet.Contains(Bucket.Key));
	}
	TMap<FUnitFace, int32> Rendered;
	bool bOnGrid = CollectRendered(Renderer, Config->VoxelSize, N, Rendered);
	TestTrue(TEXT("Rendered quads lie on the voxel grid"), bOnGrid);

	// Reference: each loaded chunk meshed whole against its loaded neighbours' voxels (absent ones are air),
	// minus the faces on a plane whose other side is not loaded (there is no seam to build there).
	FVoxelCPUCubicMesher Reference;
	Reference.Initialize();
	Reference.SetConfig(Manager->GetMesherPtr()->GetConfig());
	TMap<FUnitFace, int32> Expected;
	int32 OnSharedPlanes = 0;
	for (const FIntVector& Coord : Loaded)
	{
		TSharedPtr<const TArray<FVoxelData>> Own = Manager->GetEditMergedSnapshot(Coord);
		if (!TestTrue(TEXT("Loaded chunk has voxels"), Own.IsValid() && Own->Num() == N * N * N))
		{
			continue;
		}
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = Coord;
		Request.ChunkSize = N;
		Request.VoxelSize = Config->VoxelSize;
		Request.VoxelData = *Own;
		bool bHasNeighbor[6] = { false, false, false, false, false, false }; // +X -X +Y -Y +Z -Z
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (int32 Side = 0; Side < 2; ++Side)
			{
				FIntVector NCoord = Coord;
				NCoord[Axis] += (Side == 0) ? 1 : -1;
				if (!LoadedSet.Contains(NCoord))
				{
					continue;
				}
				TSharedPtr<const TArray<FVoxelData>> Neighbor = Manager->GetEditMergedSnapshot(NCoord);
				if (!Neighbor.IsValid())
				{
					continue;
				}
				bHasNeighbor[Axis * 2 + Side] = true;
				TArray<FVoxelData> Slice = FaceSlice(*Neighbor, N, Axis, (Side == 0) ? 0 : N - 1);
				TArray<FVoxelData>* Target[6] = { &Request.NeighborXPos, &Request.NeighborXNeg, &Request.NeighborYPos,
					&Request.NeighborYNeg, &Request.NeighborZPos, &Request.NeighborZNeg };
				*Target[Axis * 2 + Side] = MoveTemp(Slice);
			}
		}
		FChunkMeshData Whole;
		Reference.GenerateMeshCPU(Request, Whole);
		TMap<FUnitFace, int32> WholeFaces;
		bOnGrid &= CollectUnitFaces(Whole, Config->VoxelSize, Coord * N, WholeFaces);
		for (const TPair<FUnitFace, int32>& F : WholeFaces)
		{
			const int32 Axis = (F.Key.W / 256) / 2;
			const int32 Plane = FIntVector(F.Key.X, F.Key.Y, F.Key.Z)[Axis] - Coord[Axis] * N;
			if (Plane == 0 || Plane == N)
			{
				const bool bPositiveSide = (Plane == N);
				if (!bHasNeighbor[Axis * 2 + (bPositiveSide ? 0 : 1)])
				{
					continue; // the loaded region's edge: no seam, no wall (KC13)
				}
				++OnSharedPlanes;
			}
			Expected.FindOrAdd(F.Key) += F.Value;
		}
	}
	Reference.Shutdown();

	int32 Missing = 0, Extra = 0, Twice = 0;
	for (const TPair<FUnitFace, int32>& F : Expected)
	{
		Missing += Rendered.Contains(F.Key) ? 0 : 1;
	}
	for (const TPair<FUnitFace, int32>& F : Rendered)
	{
		Extra += Expected.Contains(F.Key) ? 0 : 1;
		Twice += F.Value > 1 ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("%d unit faces expected, %d of them on planes between loaded chunks; %d rendered"),
		Expected.Num(), OnSharedPlanes, Rendered.Num()));
	TestTrue(TEXT("Faces lie on planes between loaded chunks (the test exercises seams)"), OnSharedPlanes > 0);
	TestEqual(TEXT("No face missing"), Missing, 0);
	TestEqual(TEXT("No extra face (no wall at the loaded region's edge, KC13)"), Extra, 0);
	TestEqual(TEXT("No face rendered twice"), Twice, 0);

	Test.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCubicStreamingProxyRecreatedTest,
	"VoxelWorlds.Streaming.Cubic.RendererProxyRecreated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCubicStreamingProxyRecreatedTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCubicStreamingTestUtils;

	FCubicStreamingWorld Test;
	Test.Start(TEXT("VoxelCubicProxyRecreatedTestWorld"));
	const int32 N = Test.Config->ChunkSize;
	TestTrue(TEXT("The pipeline settled"), Test.Settle());

	TMap<FUnitFace, int32> Before;
	TestTrue(TEXT("Rendered quads lie on the voxel grid"), CollectRendered(Test.Renderer, Test.Config->VoxelSize, N, Before));
	TArray<FIntVector> BucketOwners;
	Test.Renderer.SeamBuckets.GetKeys(BucketOwners);
	const int32 ChunksBefore = Test.Renderer.ChunkMeshes.Num();
	const int32 SubmitsBefore = Test.Renderer.ChunkSubmits;
	TestTrue(TEXT("The world has chunk meshes and seam buckets"), ChunksBefore >= 27 && BucketOwners.Num() > 0);

	// The engine recreates the scene proxy (a material or render-state change): the renderer now holds nothing.
	Test.Renderer.SimulateProxyRecreated();
	TestTrue(TEXT("The pipeline settled again"), Test.Settle());

	TMap<FUnitFace, int32> After;
	CollectRendered(Test.Renderer, Test.Config->VoxelSize, N, After);
	int32 Missing = 0, Extra = 0;
	for (const TPair<FUnitFace, int32>& F : Before)
	{
		Missing += (After.FindRef(F.Key) == F.Value) ? 0 : 1;
	}
	for (const TPair<FUnitFace, int32>& F : After)
	{
		Extra += Before.Contains(F.Key) ? 0 : 1;
	}
	AddInfo(FString::Printf(TEXT("%d chunk meshes and %d seam buckets lost; %d chunk submits and %d buckets after recovery, %d / %d unit faces"),
		ChunksBefore, BucketOwners.Num(), Test.Renderer.ChunkSubmits - SubmitsBefore, Test.Renderer.SeamBuckets.Num(), After.Num(), Before.Num()));
	TestEqual(TEXT("Every chunk mesh was sent again"), Test.Renderer.ChunkMeshes.Num(), ChunksBefore);
	TestEqual(TEXT("Each lost chunk was re-meshed once"), Test.Renderer.ChunkSubmits - SubmitsBefore, ChunksBefore);
	TestEqual(TEXT("Every seam bucket was sent again"), Test.Renderer.SeamBuckets.Num(), BucketOwners.Num());
	TestEqual(TEXT("No face missing after recovery"), Missing, 0);
	TestEqual(TEXT("No extra face after recovery"), Extra, 0);

	Test.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
