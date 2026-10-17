// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelSeaSurface.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "IVoxelWorldMode.h"
#include "VoxelCollisionManager.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"
#include "Chaos/TriangleMeshImplicitObject.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A planet streamed at mid latitudes (WORLD_MODES_REVIEW_PLAN.md §15, WM-P7; SPHERICAL_PLANET_REQUIREMENTS.md SP-D4,
// SP-B8, SP-C1). At 45 degrees chunks straddle the surface diagonally, which no heightmap world does.
//
// - A real chunk manager streams a small planet (CPU generation, Dual Contouring, three LOD bands, water at the datum)
//   from viewers at the pole, at 45 degrees and on the equator into a renderer that records the chunk meshes and seam
//   buckets. Once settled, the rendered surface is closed: every edge is shared by two triangles, except next to chunks
//   that are not loaded (the loaded region's edge) and at T-junctions where resolutions meet. A gap at a mixed-LOD face
//   seam (G17) fails like any other hole. Water flags lie inside the sea radius only (the Z-plane WaterLevel is set far
//   above everything), and some chunks have them.
// - The same closed-surface check on a plane with the same chunk footprint and LOD bands (G17 was not planet-specific).
// - Collision winding: a chunk at 45 degrees meshed and built as the collision manager builds its body has its surface
//   faces pointing away from the planet, in physics order and in the built body.
// ---------------------------------------------------------------------------

namespace VoxelPlanetStreamingTestUtils
{
	/** A renderer that keeps the latest chunk mesh and merged seam bucket per coordinate. */
	class FRecordingMeshRenderer : public IVoxelMeshRenderer
	{
	public:
		TMap<FIntVector, FChunkMeshData> ChunkMeshes;
		TMap<FIntVector, int32> ChunkLODs;
		TMap<FIntVector, FChunkMeshData> SeamBuckets;

		virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override { bInitialized = true; }
		virtual void Shutdown() override { bInitialized = false; }
		virtual bool IsInitialized() const override { return bInitialized; }
		virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) override {}
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override
		{
			ChunkMeshes.Add(ChunkCoord, MeshData);
			ChunkLODs.Add(ChunkCoord, LODLevel);
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

	private:
		bool bInitialized = false;
	};

	constexpr float PlanetStreamRadius = 20000.0f;
	constexpr int32 PlanetStreamChunkSize = 32;
	constexpr float PlanetStreamVoxelSize = 50.0f;
	constexpr float PlanetStreamChunkWorld = PlanetStreamChunkSize * PlanetStreamVoxelSize;

	/**
	 * A small planet (radius 20,000) streamed by a real chunk manager into the recording renderer, or (StartPlane) a
	 * plane with the same chunk footprint and LOD bands.
	 */
	struct FPlanetStreamingWorld
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		AActor* Owner = nullptr;
		UVoxelChunkManager* Manager = nullptr;
		FRecordingMeshRenderer Renderer;
		FVector Viewer = FVector::ZeroVector;
		float VoxelSize = PlanetStreamVoxelSize;
		float ChunkWorld = PlanetStreamChunkWorld;

		void Start(const TCHAR* WorldName, const FVector& ViewerDirection, float ViewerAltitude)
		{
			CreateWorld(WorldName);
			Config->WorldMode = EWorldMode::SphericalPlanet;
			Config->WorldOrigin = FVector::ZeroVector;
			Config->WorldRadius = PlanetStreamRadius;
			Config->PlanetHeightScale = 1600.0f; // terrain +-800
			Config->BaseHeight = 0.0f;
			Config->PlanetMaxTerrainHeight = 1000.0f;
			Config->PlanetMaxTerrainDepth = 1000.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Config->bEnableLOD = true;
			Config->ChunkSize = PlanetStreamChunkSize;
			Config->VoxelSize = PlanetStreamVoxelSize;
			Config->ViewDistance = 6000.0f;
			Config->LODBands = {
				FLODBand(0.0f, 2000.0f, 0, 1),
				FLODBand(2000.0f, 4000.0f, 1, 2),
				FLODBand(4000.0f, 6000.0f, 2, 4) };
			Config->bUseGPUGeneration = false;
			Config->bUseGPUMeshing = false;
			Config->bGenerateCollision = false;
			Config->bEnableScatter = false;
			Config->bEnableCaves = false;
			Config->bEnableBiomes = false;
			Config->bEnableWaterLevel = true;
			Config->WaterRadius = PlanetStreamRadius;   // a sea at the datum: coasts at every latitude
			Config->WaterLevel = 1.0e9f;        // the Z plane far above everything: it must play no part
			Config->NoiseParams.Seed = 9001;
			Config->NoiseParams.Frequency = 0.0004f;
			Config->NoiseParams.Octaves = 3;
			StartManager();

			// Stream from a viewer above the surface along ViewerDirection.
			const FVoxelTerrainSurfacePoint Surface = Manager->GetSurfacePointAt(ViewerDirection);
			Viewer = Surface.Position + ViewerDirection * ViewerAltitude;
			Manager->ResetBenchCounters();
			Manager->SetBenchmarkView(true, Viewer);
		}

		/**
		 * G17's heightmap repro: an InfinitePlane with chunk 16 and voxel 100 (the planet's 1,600 uu chunk), the same
		 * three LOD bands and rolling hills, streamed from a viewer ViewerAltitude above the surface at (X, Y).
		 */
		void StartPlane(const TCHAR* WorldName, float X, float Y, float ViewerAltitude)
		{
			CreateWorld(WorldName);
			VoxelSize = 100.0f;
			ChunkWorld = 16 * VoxelSize;
			Config->WorldMode = EWorldMode::InfinitePlane;
			Config->WorldOrigin = FVector::ZeroVector;
			Config->HeightScale = 1600.0f;
			Config->BaseHeight = 0.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Config->bEnableLOD = true;
			Config->ChunkSize = 16;
			Config->VoxelSize = VoxelSize;
			Config->ViewDistance = 6000.0f;
			Config->LODBands = {
				FLODBand(0.0f, 2000.0f, 0, 1),
				FLODBand(2000.0f, 4000.0f, 1, 2),
				FLODBand(4000.0f, 6000.0f, 2, 4) };
			Config->bUseGPUGeneration = false;
			Config->bUseGPUMeshing = false;
			Config->bGenerateCollision = false;
			Config->bEnableScatter = false;
			Config->bEnableCaves = false;
			Config->bEnableBiomes = false;
			Config->bEnableWaterLevel = false;
			Config->NoiseParams.Seed = 9001;
			Config->NoiseParams.Frequency = 0.0004f;
			Config->NoiseParams.Octaves = 3;
			StartManager();

			Viewer = FVector(X, Y, Manager->GetGeneratedSurfaceHeight(X, Y) + ViewerAltitude);
			Manager->ResetBenchCounters();
			Manager->SetBenchmarkView(true, Viewer);
		}

		void CreateWorld(const TCHAR* WorldName)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
			Config = NewObject<UVoxelWorldConfiguration>();
			Config->AddToRoot();
		}

		void StartManager()
		{
			Owner = World->SpawnActor<AActor>();
			Manager = NewObject<UVoxelChunkManager>(Owner);
			Manager->RegisterComponent();
			Renderer.Initialize(World, Config);
			Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);
		}

		/** Tick until the pipeline has been idle for 90 ticks; false if it does not settle within 120 s. */
		bool Settle()
		{
			int32 IdleTicks = 0;
			const double Deadline = FPlatformTime::Seconds() + 120.0;
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

	/** Quantized world position (1/100 uu), the weld key for the closed-surface check. */
	FIntVector WeldKey(const FVector& P)
	{
		return FIntVector(FMath::RoundToInt32(P.X * 100.0), FMath::RoundToInt32(P.Y * 100.0), FMath::RoundToInt32(P.Z * 100.0));
	}

	/**
	 * Welds world positions within 0.01 uu of each other. One vertex can reach the check as two floats a few ulps
	 * apart: a seam bucket's ring vertex is solved in the neighbour's frame and shifted by a chunk into the owner's,
	 * so a plain rounded key splits it whenever it sits on a rounding boundary (G17's last "gaps" were these).
	 */
	struct FVertexWelder
	{
		static constexpr double Tolerance = 0.01;
		TMap<FIntVector, int32> Ids;
		TArray<FVector> Positions;
		double MaxWeldDistance = 0.0; // largest distance between two positions welded into one vertex

		int32 Weld(const FVector& P)
		{
			const FIntVector Key = WeldKey(P);
			for (int32 dz = -1; dz <= 1; ++dz)
			for (int32 dy = -1; dy <= 1; ++dy)
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				if (const int32* Found = Ids.Find(Key + FIntVector(dx, dy, dz)))
				{
					const double Distance = FVector::Dist(Positions[*Found], P);
					if (Distance <= Tolerance)
					{
						MaxWeldDistance = FMath::Max(MaxWeldDistance, Distance);
						return *Found;
					}
				}
			}
			const int32 Id = Positions.Add(P);
			Ids.Add(Key, Id);
			return Id;
		}
	};

	/** The settled world's rendered surface: its size and its open edges by kind. */
	struct FClosedSurfaceCounts
	{
		int32 Chunks = 0;
		int32 ByLOD[3] = { 0, 0, 0 };
		int32 Triangles = 0;
		int32 Edges = 0;
		double MaxWeldDistance = 0.0;
		int32 AtEdge = 0;         // next to a chunk that is not loaded: the loaded region's edge
		int32 TJunctions = 0;     // along a coarser edge with a vertex of the finer side on it (no gap)
		int32 MixedSeamHoles = 0; // holes at a face seam between chunks of different LOD (G17)
		int32 OtherHoles = 0;

		FString Describe() const
		{
			return FString::Printf(TEXT("%d chunks (LOD 0/1/2: %d/%d/%d), %d triangles, %d edges, welded within %.5f uu; open: %d at the loaded region's edge, %d in T-junctions, %d at mixed-LOD face seams (G17), %d other holes"),
				Chunks, ByLOD[0], ByLOD[1], ByLOD[2], Triangles, Edges, MaxWeldDistance, AtEdge, TJunctions, MixedSeamHoles, OtherHoles);
		}
	};

	/**
	 * Welds every chunk mesh and seam bucket the recording renderer holds into one world-space surface (the chunk grid
	 * starts at WorldOrigin 0) and sorts its open edges: next to a chunk that is not loaded, in T-junctions, at a
	 * mixed-LOD face seam (G17) or elsewhere. The first few holes are logged with their place.
	 */
	FClosedSurfaceCounts CountOpenEdges(FAutomationTestBase& Test, const TCHAR* Place, const FPlanetStreamingWorld& World)
	{
		FClosedSurfaceCounts Counts;
		const double ChunkWorld = World.ChunkWorld;
		TArray<FIntVector> Loaded;
		World.Manager->GetLoadedChunks(Loaded);
		const TSet<FIntVector> LoadedSet(Loaded);
		Counts.Chunks = Loaded.Num();
		for (const TPair<FIntVector, int32>& Chunk : World.Renderer.ChunkLODs)
		{
			if (Chunk.Value >= 0 && Chunk.Value < 3)
			{
				++Counts.ByLOD[Chunk.Value];
			}
		}

		FVertexWelder Welder;
		const TArray<FVector>& VertexPos = Welder.Positions;
		TMap<TPair<int32, int32>, int32> EdgeUse;
		auto AddMesh = [&](const FChunkMeshData& Mesh, const FIntVector& Coord)
		{
			const FVector Offset = FVector(Coord) * ChunkWorld;
			TArray<int32> Ids;
			Ids.Reserve(Mesh.Positions.Num());
			for (const FVector3f& P : Mesh.Positions)
			{
				Ids.Add(Welder.Weld(Offset + FVector(P)));
			}
			for (int32 i = 0; i + 2 < Mesh.Indices.Num(); i += 3)
			{
				const int32 V[3] = { Ids[Mesh.Indices[i]], Ids[Mesh.Indices[i + 1]], Ids[Mesh.Indices[i + 2]] };
				if (V[0] == V[1] || V[1] == V[2] || V[0] == V[2])
				{
					continue;
				}
				++Counts.Triangles;
				for (int32 e = 0; e < 3; ++e)
				{
					EdgeUse.FindOrAdd(TPair<int32, int32>(FMath::Min(V[e], V[(e + 1) % 3]), FMath::Max(V[e], V[(e + 1) % 3])))++;
				}
			}
		};
		for (const TPair<FIntVector, FChunkMeshData>& Chunk : World.Renderer.ChunkMeshes)
		{
			AddMesh(Chunk.Value, Chunk.Key);
		}
		for (const TPair<FIntVector, FChunkMeshData>& Bucket : World.Renderer.SeamBuckets)
		{
			AddMesh(Bucket.Value, Bucket.Key);
		}
		Counts.Edges = EdgeUse.Num();
		Counts.MaxWeldDistance = Welder.MaxWeldDistance;

		TArray<TPair<int32, int32>> OpenEdges;
		for (const TPair<TPair<int32, int32>, int32>& Edge : EdgeUse)
		{
			if (Edge.Value == 1)
			{
				OpenEdges.Add(Edge.Key);
			}
		}
		auto OnSegment = [](const FVector& P, const FVector& A, const FVector& B, double Lo, double Hi)
		{
			const FVector AB = B - A;
			const double T = FVector::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1.0e-6);
			return T > Lo && T < Hi && FVector::Dist(P, A + AB * T) < 1.0;
		};
		auto IsTJunction = [&](const TPair<int32, int32>& E)
		{
			const FVector& A = VertexPos[E.Key];
			const FVector& B = VertexPos[E.Value];
			for (const TPair<int32, int32>& F : OpenEdges)
			{
				if (F == E)
				{
					continue;
				}
				const FVector& C = VertexPos[F.Key];
				const FVector& D = VertexPos[F.Value];
				// A finer edge's vertex inside this edge, or this edge inside a coarser one.
				if ((F.Key != E.Key && F.Key != E.Value && OnSegment(C, A, B, 1.0e-3, 1.0 - 1.0e-3))
					|| (F.Value != E.Key && F.Value != E.Value && OnSegment(D, A, B, 1.0e-3, 1.0 - 1.0e-3))
					|| (OnSegment(A, C, D, -1.0e-3, 1.0 + 1.0e-3) && OnSegment(B, C, D, -1.0e-3, 1.0 + 1.0e-3)))
				{
					return true;
				}
			}
			return false;
		};

		// The loaded region's edge: a chunk within 8 voxels (LOD 2's stride-4 cells, twice) of the edge's midpoint or
		// either end is not loaded. The ends matter: a seam that cannot be built for want of a participant leaves open
		// the edges of the quads that needed one of its cells, and a long coarse edge's midpoint can lie farther out.
		const double Reach = 8.0 * World.VoxelSize;
		for (const TPair<int32, int32>& Edge : OpenEdges)
		{
			const FVector Mid = 0.5 * (VertexPos[Edge.Key] + VertexPos[Edge.Value]);
			const FIntVector Home(FMath::FloorToInt(Mid.X / ChunkWorld), FMath::FloorToInt(Mid.Y / ChunkWorld), FMath::FloorToInt(Mid.Z / ChunkWorld));
			const FVector Probes[3] = { Mid, VertexPos[Edge.Key], VertexPos[Edge.Value] };
			bool bAtEdge = !LoadedSet.Contains(Home);
			for (int32 dz = -1; dz <= 1 && !bAtEdge; ++dz)
			for (int32 dy = -1; dy <= 1 && !bAtEdge; ++dy)
			for (int32 dx = -1; dx <= 1 && !bAtEdge; ++dx)
			{
				const FIntVector Neighbor = Home + FIntVector(dx, dy, dz);
				if (!LoadedSet.Contains(Neighbor))
				{
					const FBox Box(FVector(Neighbor) * ChunkWorld, FVector(Neighbor + FIntVector(1)) * ChunkWorld);
					for (const FVector& Probe : Probes)
					{
						bAtEdge |= Box.ComputeSquaredDistanceToPoint(Probe) <= Reach * Reach;
					}
				}
			}
			if (bAtEdge)
			{
				++Counts.AtEdge;
				continue;
			}
			if (IsTJunction(Edge))
			{
				++Counts.TJunctions;
				continue;
			}

			// The face seam across the nearest chunk face, and whether its two chunks differ in LOD.
			const FVector Local = Mid - FVector(Home) * ChunkWorld;
			int32 Axis = 0, Side = -1;
			double Nearest = TNumericLimits<double>::Max();
			for (int32 A = 0; A < 3; ++A)
			{
				if (Local[A] < Nearest) { Nearest = Local[A]; Axis = A; Side = -1; }
				if (ChunkWorld - Local[A] < Nearest) { Nearest = ChunkWorld - Local[A]; Axis = A; Side = 1; }
			}
			FIntVector Across = Home;
			Across[Axis] += Side;
			const int32 HomeLOD = World.Renderer.ChunkLODs.FindRef(Home);
			const int32 AcrossLOD = World.Renderer.ChunkLODs.FindRef(Across);
			if (HomeLOD != AcrossLOD && Nearest <= 2.0 * World.VoxelSize * (1 << FMath::Max(HomeLOD, AcrossLOD)))
			{
				++Counts.MixedSeamHoles;
				if (Counts.MixedSeamHoles <= 4)
				{
					Test.AddInfo(FString::Printf(TEXT("%s: G17 gap edge at (%.0f, %.0f, %.0f), face seam (%d,%d,%d)|(%d,%d,%d) at LOD %d|%d"),
						Place, Mid.X, Mid.Y, Mid.Z, Home.X, Home.Y, Home.Z, Across.X, Across.Y, Across.Z, HomeLOD, AcrossLOD));
				}
			}
			else
			{
				++Counts.OtherHoles;
				if (Counts.OtherHoles <= 4)
				{
					Test.AddInfo(FString::Printf(TEXT("%s: hole edge at (%.0f, %.0f, %.0f) in chunk (%d,%d,%d) at LOD %d, %.0f uu from its face toward LOD %d"),
						Place, Mid.X, Mid.Y, Mid.Z, Home.X, Home.Y, Home.Z, HomeLOD, Nearest, AcrossLOD));
				}
			}
		}
		return Counts;
	}
}

/**
 * SP-D4, SP-B8, SP-C1: a small planet streamed from viewers at the north pole, at latitude 45 (longitude 45, where
 * chunks straddle the surface diagonally on all three axes) and on the equator renders a closed surface, with chunks at
 * three LOD levels meeting through the seam path, and water flags only inside the sea radius.
 *
 * An open edge next to a chunk that is not loaded is the loaded region's edge, and one along a coarser edge with a vertex
 * of the finer side on it is a T-junction (no gap). Anything else is a hole and fails the test, holes at a face seam
 * between chunks of different LOD (G17, WORLD_MODES_REVIEW_PLAN.md §14.1: small gaps where a fine crossing faced a coarse
 * cell with no crossing of its own) among them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetStreamingClosedSurfaceTest,
	"VoxelWorlds.Streaming.Planet.ClosedSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetStreamingClosedSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const TPair<const TCHAR*, FVector> Places[] = {
		{ TEXT("north pole"), FVector(0.02, 0.03, 1.0).GetSafeNormal() },
		{ TEXT("latitude 45"), FVector(C45 * C45, C45 * C45, C45).GetSafeNormal() },
		{ TEXT("equator"), FVector(1.0, 0.03, 0.02).GetSafeNormal() },
	};
	for (const TPair<const TCHAR*, FVector>& Place : Places)
	{
		FPlanetStreamingWorld Test;
		Test.Start(TEXT("VoxelPlanetStreamingTestWorld"), Place.Value, 300.0f);
		TestTrue(*FString::Printf(TEXT("%s: the pipeline settled"), Place.Key), Test.Settle());

		const FClosedSurfaceCounts Counts = CountOpenEdges(*this, Place.Key, Test);
		TestTrue(*FString::Printf(TEXT("%s: a neighbourhood streamed in"), Place.Key), Counts.Chunks >= 60);
		TestTrue(*FString::Printf(TEXT("%s: three LOD levels meet"), Place.Key), Counts.ByLOD[0] > 0 && Counts.ByLOD[1] > 0 && Counts.ByLOD[2] > 0);
		TArray<FIntVector> Loaded;
		Test.Manager->GetLoadedChunks(Loaded);

		// Water: flags only inside the sea radius (WaterLevel, the Z plane, is far above everything), and some chunks have them.
		const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*Test.Config);
		const int32 N = PlanetStreamChunkSize;
		int32 Wet = 0, WetAbove = 0, ChunksWithWater = 0;
		for (const FIntVector& Coord : Loaded)
		{
			TSharedPtr<const TArray<FVoxelData>> Voxels = Test.Manager->GetEditMergedSnapshot(Coord);
			if (!Voxels.IsValid() || Voxels->Num() != N * N * N)
			{
				continue;
			}
			int32 WetHere = 0;
			for (int32 i = 0; i < Voxels->Num(); ++i)
			{
				if ((*Voxels)[i].HasWaterFlag())
				{
					++WetHere;
					const FVector P = FVector(Coord * N + FIntVector(i % N, (i / N) % N, i / (N * N))) * PlanetStreamVoxelSize;
					WetAbove += Sea.IsBelow(P) ? 0 : 1;
				}
			}
			Wet += WetHere;
			ChunksWithWater += WetHere > 0 ? 1 : 0;
		}

		AddInfo(FString::Printf(TEXT("%s: %s, %d seam buckets; water %d voxels in %d chunks, %d above the sea radius"),
			Place.Key, *Counts.Describe(), Test.Renderer.SeamBuckets.Num(), Wet, ChunksWithWater, WetAbove));
		TestTrue(*FString::Printf(TEXT("%s: the surface has triangles"), Place.Key), Counts.Triangles > 20000);
		TestEqual(*FString::Printf(TEXT("%s: no hole at a mixed-LOD face seam (G17)"), Place.Key), Counts.MixedSeamHoles, 0);
		TestEqual(*FString::Printf(TEXT("%s: no other hole (closed at same-LOD seams, in interiors and at the loaded region)"), Place.Key), Counts.OtherHoles, 0);
		TestTrue(*FString::Printf(TEXT("%s: the sea at the datum gives some chunks water"), Place.Key), ChunksWithWater > 0);
		TestEqual(*FString::Printf(TEXT("%s: no water above the sea radius"), Place.Key), WetAbove, 0);

		Test.Stop();
	}
	return true;
}

/**
 * G17 on a heightmap world: a plane streamed from three viewers with the planet test's chunk footprint (chunk 16, voxel
 * 100) and three LOD bands renders a closed surface. Face seams between chunks at LOD 0|1 and 1|2 cross rolling hills
 * on vertical and horizontal faces; any hole fails, a G17 gap at a mixed-LOD face seam among them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneStreamingClosedSurfaceTest,
	"VoxelWorlds.Streaming.Plane.ClosedSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneStreamingClosedSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	const TPair<const TCHAR*, FVector2D> Places[] = {
		{ TEXT("plane origin"), FVector2D(0.0, 0.0) },
		{ TEXT("plane east"), FVector2D(23000.0, 7000.0) },
		{ TEXT("plane south-west"), FVector2D(-15000.0, -31000.0) },
	};
	for (const TPair<const TCHAR*, FVector2D>& Place : Places)
	{
		FPlanetStreamingWorld Test;
		Test.StartPlane(TEXT("VoxelPlaneStreamingTestWorld"), Place.Value.X, Place.Value.Y, 300.0f);
		TestTrue(*FString::Printf(TEXT("%s: the pipeline settled"), Place.Key), Test.Settle());

		const FClosedSurfaceCounts Counts = CountOpenEdges(*this, Place.Key, Test);
		AddInfo(FString::Printf(TEXT("%s: %s, %d seam buckets"), Place.Key, *Counts.Describe(), Test.Renderer.SeamBuckets.Num()));
		TestTrue(*FString::Printf(TEXT("%s: a neighbourhood streamed in"), Place.Key), Counts.Chunks >= 60);
		TestTrue(*FString::Printf(TEXT("%s: three LOD levels meet"), Place.Key), Counts.ByLOD[0] > 0 && Counts.ByLOD[1] > 0 && Counts.ByLOD[2] > 0);
		TestTrue(*FString::Printf(TEXT("%s: the surface has triangles"), Place.Key), Counts.Triangles > 10000);
		TestEqual(*FString::Printf(TEXT("%s: no hole at a mixed-LOD face seam (G17)"), Place.Key), Counts.MixedSeamHoles, 0);
		TestEqual(*FString::Printf(TEXT("%s: no other hole (closed at same-LOD seams, in interiors and at the loaded region)"), Place.Key), Counts.OtherHoles, 0);

		Test.Stop();
	}
	return true;
}

/**
 * SP-D4: collision winding at 45 degrees. A generated planet chunk straddling the surface diagonally, meshed with Dual
 * Contouring and built into a Chaos body the way the collision manager builds it, has its surface faces pointing away
 * from the planet (along local up) in physics order and in the built body.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetCollisionWindingTest,
	"VoxelWorlds.Streaming.Planet.CollisionWindingMidLatitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetCollisionWindingTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	constexpr int32 Size = 32;
	constexpr float VoxelSize = 75.0f;
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::SphericalPlanet;
	Config->WorldRadius = 250000.0f;
	Config->PlanetHeightScale = 400.0f; // gentle relief: every surface face leans the same way as local up
	Config->PlanetMaxTerrainHeight = 1000.0f;
	Config->PlanetMaxTerrainDepth = 1000.0f;
	Config->ChunkSize = Size;
	Config->VoxelSize = VoxelSize;
	Config->NoiseParams.Frequency = 0.0001f;

	// The chunk holding the surface at latitude 45, longitude 0.
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const FVector Up = FVector(C45, 0.0, C45);
	const double ChunkWorld = Size * VoxelSize;
	auto ChunkOf = [ChunkWorld](const FVector& P)
	{
		return FIntVector(FMath::FloorToInt(P.X / ChunkWorld), FMath::FloorToInt(P.Y / ChunkWorld), FMath::FloorToInt(P.Z / ChunkWorld));
	};
	// Re-centre once: the surface along the direction of the chunk's centre, so it crosses the chunk's middle.
	FIntVector Coord = ChunkOf(Mode->GetSurfacePoint(Up, Config->NoiseParams).Position);
	Coord = ChunkOf(Mode->GetSurfacePoint((FVector(Coord) + FVector(0.5)) * ChunkWorld, Config->NoiseParams).Position);

	FVoxelNoiseGenerationRequest GenRequest;
	GenRequest.ChunkCoord = Coord;
	GenRequest.ChunkSize = Size;
	GenRequest.VoxelSize = VoxelSize;
	GenRequest.NoiseParams = Config->NoiseParams;
	FVoxelWorldModeFactory::FillGenerationRequest(*Config, GenRequest);
	FVoxelCPUNoiseGenerator Generator;
	FVoxelMeshingRequest MeshRequest;
	MeshRequest.ChunkCoord = Coord;
	MeshRequest.LODLevel = 0;
	MeshRequest.ChunkSize = Size;
	MeshRequest.VoxelSize = VoxelSize;
	TestTrue(TEXT("The chunk generates"), Generator.GenerateChunkCPU(GenRequest, MeshRequest.VoxelData));

	FVoxelMeshingConfig MeshConfig;
	MeshConfig.bUseSmoothMeshing = true;
	MeshConfig.IsoLevel = 0.5f;
	MeshConfig.bGenerateUVs = false;
	MeshConfig.bCalculateAO = false;
	MeshConfig.bGenerateSkirts = false;
	MeshConfig.QEFSVDThreshold = 0.1f;
	FVoxelCPUDualContourMesher Mesher;
	Mesher.Initialize();
	Mesher.SetConfig(MeshConfig);
	FChunkMeshData Mesh;
	const bool bMeshed = Mesher.GenerateMeshCPU(MeshRequest, Mesh);
	Mesher.Shutdown();
	if (!TestTrue(TEXT("The chunk at 45 degrees has a surface"), bMeshed && Mesh.IsValid()))
	{
		Config->RemoveFromRoot();
		return false;
	}

	// Build the body exactly as the collision manager does.
	TArray<Chaos::TVec3<Chaos::FRealSingle>> Vertices;
	for (const FVector3f& P : Mesh.Positions)
	{
		Vertices.Add(Chaos::TVec3<Chaos::FRealSingle>(P.X, P.Y, P.Z));
	}
	TArray<Chaos::TVector<int32, 3>> TrianglesIn;
	UVoxelCollisionManager::BuildPhysicsTriangles(Mesh.Indices, TrianglesIn);
	const TArray<Chaos::TVector<int32, 3>> PhysicsOrder = TrianglesIn;
	TRefCountPtr<Chaos::FTriangleMeshImplicitObject> TriMesh = new Chaos::FTriangleMeshImplicitObject(
		MoveTemp(Vertices), MoveTemp(TrianglesIn), TArray<uint16>());

	// Judge faces away from the chunk's borders (without neighbours the mesher may close the solid there).
	const FVector ChunkOrigin = FVector(Coord) * ChunkWorld;
	const float Lo = 3.0f * VoxelSize, Hi = (Size - 3) * VoxelSize;
	auto IsInner = [&](const FVector3f& Centroid)
	{
		return Centroid.X > Lo && Centroid.X < Hi && Centroid.Y > Lo && Centroid.Y < Hi && Centroid.Z > Lo && Centroid.Z < Hi;
	};
	auto LocalUp = [&](const FVector3f& Centroid)
	{
		return FVector3f((ChunkOrigin + FVector(Centroid)).GetSafeNormal());
	};
	int32 Faces = 0, Outward = 0;
	for (const Chaos::TVector<int32, 3>& T : PhysicsOrder)
	{
		const FVector3f A = Mesh.Positions[T[0]], B = Mesh.Positions[T[1]], C = Mesh.Positions[T[2]];
		const FVector3f Centroid = (A + B + C) / 3.0f;
		if (!IsInner(Centroid))
		{
			continue;
		}
		++Faces;
		Outward += FVector3f::DotProduct(FVector3f::CrossProduct(B - A, C - A).GetSafeNormal(), LocalUp(Centroid)) > 0.5f ? 1 : 0;
	}
	int32 BodyFaces = 0, BodyOutward = 0;
	auto JudgeBody = [&](const auto& Elements)
	{
		for (int32 FaceIdx = 0; FaceIdx < Elements.Num(); ++FaceIdx)
		{
			const auto& Tri = Elements[FaceIdx];
			const auto A = TriMesh->Particles().GetX(static_cast<int32>(Tri[0]));
			const auto B = TriMesh->Particles().GetX(static_cast<int32>(Tri[1]));
			const auto C = TriMesh->Particles().GetX(static_cast<int32>(Tri[2]));
			const FVector3f Centroid((A.X + B.X + C.X) / 3.0f, (A.Y + B.Y + C.Y) / 3.0f, (A.Z + B.Z + C.Z) / 3.0f);
			if (!IsInner(Centroid))
			{
				continue;
			}
			++BodyFaces;
			const auto Normal = TriMesh->GetFaceNormal(FaceIdx);
			BodyOutward += FVector3f::DotProduct(FVector3f(Normal.X, Normal.Y, Normal.Z), LocalUp(Centroid)) > 0.5f ? 1 : 0;
		}
	};
	if (TriMesh->Elements().RequiresLargeIndices())
	{
		JudgeBody(TriMesh->Elements().GetLargeIndexBuffer());
	}
	else
	{
		JudgeBody(TriMesh->Elements().GetSmallIndexBuffer());
	}
	AddInfo(FString::Printf(TEXT("Chunk (%d, %d, %d) at 45 degrees: %d of %d inner faces wound outward in physics order, %d of %d in the body"),
		Coord.X, Coord.Y, Coord.Z, Outward, Faces, BodyOutward, BodyFaces));
	TestTrue(TEXT("The chunk has inner surface faces to judge"), Faces > 100);
	TestEqual(TEXT("Every inner face in physics order points away from the planet"), Outward, Faces);
	TestEqual(TEXT("The body holds every inner face"), BodyFaces, Faces);
	TestEqual(TEXT("Every inner body face normal points away from the planet"), BodyOutward, BodyFaces);

	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
