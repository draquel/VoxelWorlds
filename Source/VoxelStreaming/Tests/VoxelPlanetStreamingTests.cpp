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
#include "VoxelResidencyStats.h"
#include "VoxelData.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "VoxelStreamingBenchmark.h"
#include "HAL/IConsoleManager.h"
#include "VoxelChunkShell.h"
#include "VoxelNeighborSliceExtraction.h"
#include "VoxelEditManager.h"

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
		float Relief = 1600.0f;  // the terrain's height scale (planet and plane), set before Start
		int32 Octaves = 3;       // the terrain noise's octaves, set before Start
		EMeshingMode MeshingMode = EMeshingMode::DualContouring; // set before Start

		void Start(const TCHAR* WorldName, const FVector& ViewerDirection, float ViewerAltitude)
		{
			CreateWorld(WorldName);
			Config->WorldMode = EWorldMode::SphericalPlanet;
			Config->WorldOrigin = FVector::ZeroVector;
			Config->WorldRadius = PlanetStreamRadius;
			Config->PlanetHeightScale = Relief; // terrain +-800 by default
			Config->BaseHeight = 0.0f;
			Config->PlanetMaxTerrainHeight = 1000.0f;
			Config->PlanetMaxTerrainDepth = 1000.0f;
			Config->MeshingMode = MeshingMode;
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
			Config->NoiseParams.Octaves = Octaves;
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
			Config->HeightScale = Relief;
			Config->BaseHeight = 0.0f;
			Config->MeshingMode = MeshingMode;
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
			Config->NoiseParams.Octaves = Octaves;
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

/**
 * Spherical Phase 6, WM-BP3: the bench at scale. A climb (voxel.Bench.Run climb=lat:lon[:alt]) runs along local up from
 * its start over the ground, so its altitude grows by the distance flown at every site; and the residency the bench
 * samples (UVoxelChunkManager::GetResidencySnapshot) on a streamed planet counts chunks per committed LOD and per radial
 * layer (the chunk centre's altitude in chunk sizes, inside the shell), from a viewer whose altitude it reports, on the
 * ground and two and a half chunks up. The window's size at altitude is recorded, not asserted (WM-BP8 changes it).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBenchPlanetClimbRouteTest,
	"VoxelWorlds.Bench.Planet.ClimbRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelBenchPlanetClimbRouteTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	// The route: radial from the start, the altitude the start's plus the distance.
	{
		const FVector Center(1000.0, -2000.0, 500.0);
		constexpr double Radius = 250000.0;
		const FVector2D Sites[] = { { 90.0, 0.0 }, { 45.0, 20.0 }, { 0.0, 100.0 }, { 35.264, 45.0 }, { -60.0, 200.0 } };
		double WorstUp = 0.0, WorstAltitude = 0.0;
		for (const FVector2D& Site : Sites)
		{
			const FVector Direction = FVoxelStreamingBenchmark::LatLonToDirection(Site.X, Site.Y);
			const FVector Ground = Center + Direction * (Radius + 600.0);
			FVoxelBenchConfig Config;
			FVoxelStreamingBenchmark::MakeClimb(Config, Center, Ground, 500.0f, Site);
			TestTrue(TEXT("A climb is a straight path"), Config.PathType == EVoxelBenchPath::Straight && Config.bClimb);
			WorstUp = FMath::Max(WorstUp, 1.0 - FVector::DotProduct(Config.Direction, Direction));
			for (const double S : { 0.0, 1000.0, 250000.0, 2.0e6 })
			{
				const double Altitude = FVector::Dist(Config.StartPosition + Config.Direction * S, Center) - Radius;
				WorstAltitude = FMath::Max(WorstAltitude, FMath::Abs(Altitude - (600.0 + 500.0 + S)));
			}
		}
		AddInfo(FString::Printf(TEXT("Climbs at 5 sites: direction %.2e off local up, altitude %.2e uu off start + distance"), WorstUp, WorstAltitude));
		TestTrue(TEXT("The climb follows local up"), WorstUp < 1.0e-12);
		TestTrue(TEXT("The altitude grows by the distance flown"), WorstAltitude < 1.0e-3);
	}

	// The residency on a streamed planet, at the pole: on the ground, then 4,000 uu up (2.5 chunks).
	FPlanetStreamingWorld Test;
	const FVector Up = FVector(0.02, 0.03, 1.0).GetSafeNormal();
	Test.Start(TEXT("VoxelBenchClimbTestWorld"), Up, 300.0f);
	TestTrue(TEXT("The pipeline settled on the ground"), Test.Settle());
	const double GroundAltitude = Test.Manager->GetSurfacePointAt(Up).Altitude;
	const FVoxelResidencySnapshot Ground = Test.Manager->GetResidencySnapshot();
	int32 ByLODSum = 0;
	for (const int32 Count : Ground.ByLOD)
	{
		ByLODSum += Count;
	}
	int32 ByLayerSum = 0, LayersOutsideShell = 0;
	// The shell is the datum +-1,000; chunk centres lie within it plus half a chunk diagonal (1,386 uu): layers -2 to 1.
	for (const TPair<int32, int32>& Layer : Ground.ByLayer)
	{
		ByLayerSum += Layer.Value;
		LayersOutsideShell += (Layer.Key < -2 || Layer.Key > 1) ? Layer.Value : 0;
	}
	AddInfo(FString::Printf(TEXT("On the ground (altitude %.0f): %d tracked, %d ready (LOD 0/1/2: %d/%d/%d), %d radial layers, reach %.0f (ground %.0f), chunk altitudes %.0f to %.0f"),
		Ground.ViewerAltitude, Ground.Tracked, Ground.Ready, Ground.ByLOD.IsValidIndex(0) ? Ground.ByLOD[0] : 0,
		Ground.ByLOD.IsValidIndex(1) ? Ground.ByLOD[1] : 0, Ground.ByLOD.IsValidIndex(2) ? Ground.ByLOD[2] : 0, Ground.ByLayer.Num(),
		Ground.MaxDistance, Ground.MaxGroundDistance, Ground.MinChunkAltitude, Ground.MaxChunkAltitude));
	TestTrue(TEXT("Radial layers on a planet"), Ground.bRadialLayers);
	TestTrue(TEXT("A neighbourhood is tracked and ready"), Ground.Tracked >= 60 && Ground.Ready > 0 && Ground.Ready <= Ground.Tracked);
	TestEqual(TEXT("Every ready chunk counted once per LOD"), ByLODSum, Ground.Ready);
	TestEqual(TEXT("Every tracked chunk counted once per layer"), ByLayerSum, Ground.Tracked);
	TestEqual(TEXT("Every layer inside the shell"), LayersOutsideShell, 0);
	TestTrue(TEXT("The viewer's altitude over the datum"), FMath::IsNearlyEqual(Ground.ViewerAltitude, GroundAltitude + 300.0, 1.0));
	TestTrue(TEXT("The reach within the unload distance"), Ground.MaxDistance > 0.0 && Ground.MaxDistance <= 1.2 * 6000.0 + 1400.0);

	const FVector High = Test.Manager->GetSurfacePointAt(Up).Position + Up * 4000.0;
	Test.Viewer = High;
	Test.Manager->SetBenchmarkView(true, High);
	TestTrue(TEXT("The pipeline settled up high"), Test.Settle());
	const FVoxelResidencySnapshot Climbed = Test.Manager->GetResidencySnapshot();
	AddInfo(FString::Printf(TEXT("4,000 uu up (altitude %.0f): %d tracked, %d ready, %d radial layers, reach %.0f (ground %.0f)"),
		Climbed.ViewerAltitude, Climbed.Tracked, Climbed.Ready, Climbed.ByLayer.Num(), Climbed.MaxDistance, Climbed.MaxGroundDistance));
	TestTrue(TEXT("The climbed viewer's altitude"), FMath::IsNearlyEqual(Climbed.ViewerAltitude, GroundAltitude + 4000.0, 1.0));
	TestTrue(TEXT("Radial layers up high"), Climbed.bRadialLayers);
	Test.Stop();
	return true;
}

// ---------------------------------------------------------------------------
// G44 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.8): Dual Contouring's vertex normals at LOD 1 and 2 against the analytic
// surface's, as LOD 0's are. The generated density saturates a voxel either side of the surface, so a gradient with taps
// a stride apart reads 0 or 255 and snapped LOD 2's normals toward the cube directions: on a planet a ring lit brighter
// than the LOD 0/1 centre and the far field (whose normals are the analytic surface's), on a heightmap snapped slopes.
// ---------------------------------------------------------------------------

namespace VoxelPlanetStreamingTestUtils
{
	/** Per LOD: how the rendered vertex normals and triangles compare with the analytic surface's normals (G44). */
	struct FLODNormalStats
	{
		double FaceLit[3][4] = {};      // per LOD and class (0 interior, 1-3 within 1.5 cells of an X, Y or Z chunk face)
		double FaceLitAnalytic[3][4] = {};
		int32 FaceTris[3][4] = {};

		/** Brightness (as Brightness) of the LOD's triangles in a class: 0 interior, 1-3 along X, Y or Z chunk faces. */
		double FaceBrightness(int32 LOD, int32 Class) const
		{
			return FaceLitAnalytic[LOD][Class] > 0.0 ? FaceLit[LOD][Class] / FaceLitAnalytic[LOD][Class] - 1.0 : 0.0;
		}

		FString DescribeFaces() const
		{
			FString Out;
			for (int32 LOD = 0; LOD < 3; ++LOD)
			{
				Out += FString::Printf(TEXT("%sLOD %d:"), LOD > 0 ? TEXT("; ") : TEXT(""), LOD);
				for (int32 Class = 0; Class < 4; ++Class)
				{
					static const TCHAR* Names[4] = { TEXT("interior"), TEXT("X faces"), TEXT("Y faces"), TEXT("Z faces") };
					Out += FString::Printf(TEXT(" %s %d tris %+.2f%%"), Names[Class], FaceTris[LOD][Class], 100.0 * FaceBrightness(LOD, Class));
				}
			}
			return Out;
		}
		int32 Vertices[3] = { 0, 0, 0 };
		int32 Skipped = 0;              // farther from the analytic surface along up than two of their cells (steep slopes)
		double SumAngle[3] = { 0.0, 0.0, 0.0 };
		double SumAngleSq[3] = { 0.0, 0.0, 0.0 };
		// G62: the angle per LOD and bin of the vertex's phase in the voxel grid along the analytic normal's dominant axis.
		double PhaseSum[3][8] = {};
		int32 PhaseCount[3][8] = {};

		/** The angles' standard deviation (degrees). */
		double AngleSD(int32 LOD) const
		{
			if (Vertices[LOD] <= 0)
			{
				return 0.0;
			}
			const double Mean = SumAngle[LOD] / Vertices[LOD];
			return FMath::Sqrt(FMath::Max(0.0, SumAngleSq[LOD] / Vertices[LOD] - Mean * Mean));
		}

		/**
		 * The band (G62): largest minus smallest mean angle over eight bins of the vertex's phase in the voxel grid along the
		 * analytic normal's dominant axis, over bins holding 50 vertices (degrees). The central difference before G62 made a
		 * vertex's error a function of where the surface crossed the voxel layer; curvature and coarse cells add error at
		 * every phase alike.
		 */
		double Band(int32 LOD) const
		{
			double Lo = TNumericLimits<double>::Max();
			double Hi = -TNumericLimits<double>::Max();
			for (int32 Bin = 0; Bin < 8; ++Bin)
			{
				if (PhaseCount[LOD][Bin] >= 50)
				{
					const double M = PhaseSum[LOD][Bin] / PhaseCount[LOD][Bin];
					Lo = FMath::Min(Lo, M);
					Hi = FMath::Max(Hi, M);
				}
			}
			return Hi >= Lo ? Hi - Lo : 0.0;
		}

		FString DescribeBands() const
		{
			FString Out;
			for (int32 LOD = 0; LOD < 3; ++LOD)
			{
				Out += FString::Printf(TEXT("%sLOD %d: %d vertices, mean %.2f deg, SD %.2f, band %.2f"), LOD > 0 ? TEXT("; ") : TEXT(""),
					LOD, Vertices[LOD], MeanAngle(LOD), AngleSD(LOD), Band(LOD));
			}
			return Out;
		}
		FVector2D SumTangent[3] = { FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D::ZeroVector };
		double SumAreaLit[3] = { 0.0, 0.0, 0.0 };         // triangle area x max(0, N.L), N the normalized sum of its vertex normals
		double SumAreaLitAnalytic[3] = { 0.0, 0.0, 0.0 }; // triangle area x max(0, N.L), N the analytic normal at its centroid

		int32 TotalVertices() const { return Vertices[0] + Vertices[1] + Vertices[2]; }

		/** Mean angle between a vertex normal and the analytic normal (degrees). */
		double MeanAngle(int32 LOD) const { return Vertices[LOD] > 0 ? SumAngle[LOD] / Vertices[LOD] : 0.0; }

		/** The systematic tilt: the mean of (normal - analytic normal) across local up, east and north (degrees). */
		double TiltBias(int32 LOD) const
		{
			return Vertices[LOD] > 0 ? FMath::RadiansToDegrees(FMath::Asin(FMath::Min(1.0, (SumTangent[LOD] / Vertices[LOD]).Size()))) : 0.0;
		}

		/**
		 * How bright the LOD's triangles light against the analytic surface under them, per unit area (the Lambert term
		 * with the interpolated normal at each triangle's centroid, under the test's sun): 0 = the same, +0.05 = 5% brighter.
		 */
		double Brightness(int32 LOD) const { return SumAreaLitAnalytic[LOD] > 0.0 ? SumAreaLit[LOD] / SumAreaLitAnalytic[LOD] - 1.0 : 0.0; }

		FString Describe() const
		{
			FString Out;
			for (int32 LOD = 0; LOD < 3; ++LOD)
			{
				Out += FString::Printf(TEXT("%sLOD %d: %d vertices, mean %.2f deg, tilt %.2f deg, brightness %+.2f%%"),
					LOD > 0 ? TEXT("; ") : TEXT(""), LOD, Vertices[LOD], MeanAngle(LOD), TiltBias(LOD), 100.0 * Brightness(LOD));
			}
			return Out + FString::Printf(TEXT("; %d skipped"), Skipped);
		}
	};

	/**
	 * Every vertex and triangle of the recording renderer's chunk meshes and seam buckets (seam rings and slabs carry LOD 1
	 * and 2 geometry too), sorted by the LOD of the chunk it lies in, against GetSurfacePointAt's normal along its up-line.
	 */
	FLODNormalStats MeasureLODNormals(const FPlanetStreamingWorld& World, const FVector& Sun)
	{
		FLODNormalStats Stats;
		const double ChunkWorld = World.ChunkWorld;
		auto LODAt = [&](const FVector& P) -> int32
		{
			const FIntVector Home(FMath::FloorToInt(P.X / ChunkWorld), FMath::FloorToInt(P.Y / ChunkWorld), FMath::FloorToInt(P.Z / ChunkWorld));
			const int32* LOD = World.Renderer.ChunkLODs.Find(Home);
			return (LOD && *LOD >= 0 && *LOD <= 2) ? *LOD : -1;
		};
		auto AddMesh = [&](const FChunkMeshData& Mesh, const FIntVector& Coord)
		{
			const FVector Offset = FVector(Coord) * ChunkWorld;
			const int32 Num = FMath::Min(Mesh.Positions.Num(), Mesh.Normals.Num());
			for (int32 i = 0; i < Num; ++i)
			{
				const FVector P = Offset + FVector(Mesh.Positions[i]);
				const int32 LOD = LODAt(P);
				if (LOD < 0)
				{
					continue;
				}
				const FVoxelTerrainSurfacePoint Surface = World.Manager->GetSurfacePointAt(P);
				const FVector Up = World.Manager->GetUpVectorAt(P);
				if (FMath::Abs(FVector::DotProduct(P - Surface.Position, Up)) > 2.0 * World.VoxelSize * (1 << LOD))
				{
					++Stats.Skipped;
					continue;
				}
				const FVector N = FVector(Mesh.Normals[i]).GetSafeNormal();
				const FVector A = Surface.Normal.GetSafeNormal();
				FVector East = FVector::CrossProduct(FVector::UpVector, Up).GetSafeNormal();
				if (East.IsNearlyZero())
				{
					East = FVector::ForwardVector;
				}
				const FVector North = FVector::CrossProduct(Up, East);
				const FVector D = N - A;
				const double Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(N, A), -1.0, 1.0)));
				++Stats.Vertices[LOD];
				Stats.SumAngle[LOD] += Angle;
				Stats.SumAngleSq[LOD] += Angle * Angle;
				Stats.SumTangent[LOD] += FVector2D(FVector::DotProduct(D, East), FVector::DotProduct(D, North));
				// The phase along the dominant axis (chunk-local voxels: chunk origins lie on the voxel grid).
				int32 Dominant = 0;
				for (int32 Axis = 1; Axis < 3; ++Axis)
				{
					if (FMath::Abs(A[Axis]) > FMath::Abs(A[Dominant]))
					{
						Dominant = Axis;
					}
				}
				const double Local = static_cast<double>(Mesh.Positions[i][Dominant]) / World.VoxelSize;
				const int32 Bin = FMath::Clamp(static_cast<int32>((Local - FMath::FloorToDouble(Local)) * 8.0), 0, 7);
				Stats.PhaseSum[LOD][Bin] += Angle;
				++Stats.PhaseCount[LOD][Bin];
			}
			for (int32 i = 0; i + 2 < Mesh.Indices.Num(); i += 3)
			{
				const int32 I[3] = { static_cast<int32>(Mesh.Indices[i]), static_cast<int32>(Mesh.Indices[i + 1]), static_cast<int32>(Mesh.Indices[i + 2]) };
				if (I[0] >= Num || I[1] >= Num || I[2] >= Num)
				{
					continue;
				}
				const FVector P0 = Offset + FVector(Mesh.Positions[I[0]]);
				const FVector P1 = Offset + FVector(Mesh.Positions[I[1]]);
				const FVector P2 = Offset + FVector(Mesh.Positions[I[2]]);
				const FVector Centroid = (P0 + P1 + P2) / 3.0;
				const int32 LOD = LODAt(Centroid);
				const double Area = 0.5 * FVector::CrossProduct(P1 - P0, P2 - P0).Size();
				if (LOD < 0 || Area <= 0.0)
				{
					continue;
				}
				const FVector N = (FVector(Mesh.Normals[I[0]]) + FVector(Mesh.Normals[I[1]]) + FVector(Mesh.Normals[I[2]])).GetSafeNormal();
				const FVector A = World.Manager->GetSurfacePointAt(Centroid).Normal.GetSafeNormal();
				Stats.SumAreaLit[LOD] += Area * FMath::Max(0.0, FVector::DotProduct(N, Sun));
				Stats.SumAreaLitAnalytic[LOD] += Area * FMath::Max(0.0, FVector::DotProduct(A, Sun));
				// The chunk face nearest the centroid, if within 1.5 of the LOD's cells (seam slabs and the rings either side).
				int32 Class = 0;
				double Nearest = World.VoxelSize * (1 << LOD) * 1.5;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Local = Centroid[Axis] - FMath::FloorToDouble(Centroid[Axis] / ChunkWorld) * ChunkWorld;
					const double Dist = FMath::Min(Local, ChunkWorld - Local);
					if (Dist < Nearest)
					{
						Nearest = Dist;
						Class = Axis + 1;
					}
				}
				++Stats.FaceTris[LOD][Class];
				Stats.FaceLit[LOD][Class] += Area * FMath::Max(0.0, FVector::DotProduct(N, Sun));
				Stats.FaceLitAnalytic[LOD][Class] += Area * FMath::Max(0.0, FVector::DotProduct(A, Sun));
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
		return Stats;
	}

	/**
	 * G44's bound where the LOD 2 mesh resolves the terrain (gentle relief), so what differs from LOD 0 is the normal
	 * estimator: LOD 1 and 2 vertex normals tilted at most 1.5 degrees more than LOD 0's and their triangles' brightness
	 * within LOD 0's error plus 2% (the lighting step G44 is about), and their mean angle from the analytic normals within
	 * half again LOD 0's plus 5 degrees (a coarse vertex's normal is a voxel cell's up to a cell away from it, so its scatter
	 * grows with the slopes' curvature: 7 degrees at LOD 2 on the plane's 27-degree hills against 2.6 at LOD 0).
	 */
	bool LODNormalsMatch(const FLODNormalStats& Stats, FString& OutWhy)
	{
		for (int32 LOD = 1; LOD < 3; ++LOD)
		{
			if (Stats.MeanAngle(LOD) > Stats.MeanAngle(0) * 1.5 + 5.0)
			{
				OutWhy = FString::Printf(TEXT("LOD %d's mean angle %.2f against LOD 0's %.2f"), LOD, Stats.MeanAngle(LOD), Stats.MeanAngle(0));
				return false;
			}
			if (Stats.TiltBias(LOD) > Stats.TiltBias(0) + 1.5)
			{
				OutWhy = FString::Printf(TEXT("LOD %d tilts %.2f deg (LOD 0 %.2f)"), LOD, Stats.TiltBias(LOD), Stats.TiltBias(0));
				return false;
			}
			if (FMath::Abs(Stats.Brightness(LOD)) > FMath::Abs(Stats.Brightness(0)) + 0.02)
			{
				OutWhy = FString::Printf(TEXT("LOD %d lights %+.2f%% against LOD 0's %+.2f%%"), LOD, 100.0 * Stats.Brightness(LOD), 100.0 * Stats.Brightness(0));
				return false;
			}
		}
		return true;
	}

	/** Sets voxel.Mesh.VoxelShadingNormals at console priority for a scope (tests set cvars at console priority). */
	struct FScopedVoxelShadingNormals
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedVoxelShadingNormals(int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Mesh.VoxelShadingNormals"));
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedVoxelShadingNormals()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/**
	 * Streams one place with the normals before G44 (Dual Contouring) or G61 (Marching Cubes) (voxel.Mesh.VoxelShadingNormals
	 * 0) and as they are (1) and measures both. Positions are the same either way (DC's QEF keeps the stride-scale normals,
	 * MC's edge interpolation never read them), so the vertex counts must agree. With gentle relief (bStrictBound) the
	 * current normals meet LODNormalsMatch and the old ones do not; with the default relief, whose finest octave LOD 2
	 * cannot resolve, LOD 1 and 2 come closer to the analytic normals than before.
	 */
	void RunLODNormalsPlace(FAutomationTestBase& Test, const TCHAR* Place, TFunctionRef<void(FPlanetStreamingWorld&)> Start,
		const FVector& Sun, float Relief, int32 Octaves, bool bStrictBound, EMeshingMode MeshingMode = EMeshingMode::DualContouring)
	{
		const TCHAR* Finding = (MeshingMode == EMeshingMode::MarchingCubes) ? TEXT("G61") : TEXT("G44");
		FLODNormalStats Before, After;
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			FScopedVoxelShadingNormals Scoped(Mode);
			FPlanetStreamingWorld World;
			World.Relief = Relief;
			World.Octaves = Octaves;
			World.MeshingMode = MeshingMode;
			Start(World);
			Test.TestTrue(*FString::Printf(TEXT("%s (normals %d): the pipeline settled"), Place, Mode), World.Settle());
			(Mode == 0 ? Before : After) = MeasureLODNormals(World, Sun);
			World.Stop();
		}
		Test.AddInfo(FString::Printf(TEXT("%s, before %s: %s"), Place, Finding, *Before.Describe()));
		Test.AddInfo(FString::Printf(TEXT("%s, after: %s"), Place, *After.Describe()));
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: LOD %d vertices measured"), Place, LOD), After.Vertices[LOD] > 500);
			Test.TestEqual(*FString::Printf(TEXT("%s: LOD %d vertices the same with either normals (no vertex moves)"), Place, LOD),
				After.Vertices[LOD], Before.Vertices[LOD]);
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: at most 0.1%% of vertices off the analytic surface's up-line by two cells (%d)"), Place, After.Skipped),
			After.Skipped * 1000 <= After.TotalVertices());

		if (bStrictBound)
		{
			FString Why;
			const bool bAfterMatches = LODNormalsMatch(After, Why);
			Test.TestTrue(*FString::Printf(TEXT("%s: LOD 1 and 2 normals match LOD 0's against the analytic surface %s"), Place, *Why), bAfterMatches);
			FString WhyBefore;
			const bool bBeforeMatches = LODNormalsMatch(Before, WhyBefore);
			Test.TestFalse(*FString::Printf(TEXT("%s: the stride-scale normals before %s fail the bound"), Place, Finding), bBeforeMatches);
			Test.AddInfo(FString::Printf(TEXT("%s, before %s fails on: %s"), Place, Finding, *WhyBefore));
			return;
		}
		// Brightness is reported, not bounded, here: with an octave finer than LOD 2's cells it is the coarse triangles'
		// interpolation (the far field's quads near the window do the same with the analytic normals), not the estimator.
		for (int32 LOD = 1; LOD < 3; ++LOD)
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: LOD %d's mean angle %.2f below the %.2f before %s"), Place, LOD, After.MeanAngle(LOD), Before.MeanAngle(LOD), Finding),
				After.MeanAngle(LOD) < Before.MeanAngle(LOD));
		}
	}
}

/**
 * G44 on a planet: from viewers at 45N 20E (G44's site; up far from every cube axis and diagonal) and on the equator, with
 * gentle relief (+-200, two octaves: LOD 2 resolves it) LOD 1 and 2 vertex normals (chunk meshes and seam buckets) are as
 * close to the analytic surface's as LOD 0's, with no tilt and no brightness step under the local noon sun, which
 * voxel.Mesh.VoxelShadingNormals 0 (the stride-scale normals before G44) fails; with the streaming tests' relief (+-800,
 * three octaves) they come closer to LOD 0's than before.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetStreamingLODNormalsTest,
	"VoxelWorlds.Streaming.Planet.LODNormals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetStreamingLODNormalsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Direction = [](double LatDeg, double LonDeg)
	{
		const double Lat = FMath::DegreesToRadians(LatDeg);
		const double Lon = FMath::DegreesToRadians(LonDeg);
		return FVector(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	};
	const TPair<const TCHAR*, FVector> Places[] = {
		{ TEXT("45N 20E"), Direction(45.0, 20.0) },
		{ TEXT("equator"), Direction(1.0, 1.5) },
	};
	for (const TPair<const TCHAR*, FVector>& Place : Places)
	{
		// The sun at local noon with no axial tilt: over the equator at the place's longitude.
		const FVector Sun = FVector(Place.Value.X, Place.Value.Y, 0.0).GetSafeNormal();
		auto Start = [&Place](FPlanetStreamingWorld& World) { World.Start(TEXT("VoxelPlanetLODNormalsTestWorld"), Place.Value, 300.0f); };
		RunLODNormalsPlace(*this, *FString::Printf(TEXT("%s, gentle"), Place.Key), Start, Sun, 400.0f, 2, true);
		RunLODNormalsPlace(*this, Place.Key, Start, Sun, 1600.0f, 3, false);
	}
	return true;
}

/**
 * G44 on a heightmap world: the plane of the closed-surface test (chunk 16, voxel 100, three LOD bands) under an overhead
 * sun, with gentle hills (+-200, one octave of 25 voxels, which LOD 2 resolves: LOD 1 and 2 normals as close to the
 * analytic surface's as LOD 0's, which the stride-scale normals before G44 fail) and with the closed-surface test's
 * (closer to the analytic normals than before).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneStreamingLODNormalsTest,
	"VoxelWorlds.Streaming.Plane.LODNormals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneStreamingLODNormalsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Start = [](FPlanetStreamingWorld& World) { World.StartPlane(TEXT("VoxelPlaneLODNormalsTestWorld"), 23000.0f, 7000.0f, 300.0f); };
	RunLODNormalsPlace(*this, TEXT("plane east, gentle"), Start, FVector::UpVector, 400.0f, 1, true);
	RunLODNormalsPlace(*this, TEXT("plane east"), Start, FVector::UpVector, 1600.0f, 3, false);
	return true;
}

// ---------------------------------------------------------------------------
// G61 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.9): G44 in Marching Cubes. Its LOD 1 and 2 vertex normals were the
// central-difference gradient with taps a stride apart (CalculateGradientNormalLOD, CPU and GPU) on the same saturated
// density, so they snapped toward the cube directions too; now each edge vertex takes LOD 0's normal at the voxel
// crossing on its edge. The same places, reliefs and bounds as the Dual Contouring tests, on MC worlds.
// ---------------------------------------------------------------------------

/**
 * G61 on a planet: Marching Cubes chunk meshes and seam buckets from viewers at 45N 20E and on the equator, with gentle
 * relief LOD 1 and 2 normals as close to the analytic surface's as LOD 0's (no tilt, no brightness step under the local
 * noon sun), which voxel.Mesh.VoxelShadingNormals 0 (the stride-scale normals before G61) fails; with the streaming tests'
 * relief they come closer than before.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetStreamingLODNormalsMCTest,
	"VoxelWorlds.Streaming.Planet.LODNormalsMC",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetStreamingLODNormalsMCTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Direction = [](double LatDeg, double LonDeg)
	{
		const double Lat = FMath::DegreesToRadians(LatDeg);
		const double Lon = FMath::DegreesToRadians(LonDeg);
		return FVector(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	};
	const TPair<const TCHAR*, FVector> Places[] = {
		{ TEXT("MC 45N 20E"), Direction(45.0, 20.0) },
		{ TEXT("MC equator"), Direction(1.0, 1.5) },
	};
	for (const TPair<const TCHAR*, FVector>& Place : Places)
	{
		const FVector Sun = FVector(Place.Value.X, Place.Value.Y, 0.0).GetSafeNormal();
		auto Start = [&Place](FPlanetStreamingWorld& World) { World.Start(TEXT("VoxelPlanetLODNormalsMCTestWorld"), Place.Value, 300.0f); };
		RunLODNormalsPlace(*this, *FString::Printf(TEXT("%s, gentle"), Place.Key), Start, Sun, 400.0f, 2, true, EMeshingMode::MarchingCubes);
		RunLODNormalsPlace(*this, Place.Key, Start, Sun, 1600.0f, 3, false, EMeshingMode::MarchingCubes);
	}
	return true;
}

/**
 * G61 on a heightmap world: the Plane.LODNormals places with Marching Cubes. Gentle hills: LOD 1 and 2 normals as close
 * to the analytic surface's as LOD 0's, which the stride-scale normals before G61 fail; the closed-surface test's hills:
 * closer to the analytic normals than before.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneStreamingLODNormalsMCTest,
	"VoxelWorlds.Streaming.Plane.LODNormalsMC",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneStreamingLODNormalsMCTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Start = [](FPlanetStreamingWorld& World) { World.StartPlane(TEXT("VoxelPlaneLODNormalsMCTestWorld"), 23000.0f, 7000.0f, 300.0f); };
	RunLODNormalsPlace(*this, TEXT("MC plane east, gentle"), Start, FVector::UpVector, 400.0f, 1, true, EMeshingMode::MarchingCubes);
	RunLODNormalsPlace(*this, TEXT("MC plane east"), Start, FVector::UpVector, 1600.0f, 3, false, EMeshingMode::MarchingCubes);
	return true;
}

namespace VoxelPlanetStreamingTestUtils
{
	/** Sets voxel.Mesh.ScaleClampedGradients at console priority for a scope. */
	struct FScopedScaleClampedGradients
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedScaleClampedGradients(int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Mesh.ScaleClampedGradients"));
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedScaleClampedGradients()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/** Sets voxel.Mesh.CrossingGradientNormals at console priority for a scope. */
	struct FScopedCrossingGradientNormals
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedCrossingGradientNormals(int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Mesh.CrossingGradientNormals"));
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedCrossingGradientNormals()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/** Triangles along each chunk face axis light like the interior ones at every LOD (within 3%), where both have 200. */
	bool ChunkFacesMatch(const FLODNormalStats& Stats, FString& OutWhy)
	{
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			if (Stats.FaceTris[LOD][0] < 200)
			{
				continue;
			}
			for (int32 Class = 1; Class < 4; ++Class)
			{
				if (Stats.FaceTris[LOD][Class] >= 200
					&& FMath::Abs(Stats.FaceBrightness(LOD, Class) - Stats.FaceBrightness(LOD, 0)) > 0.03)
				{
					OutWhy = FString::Printf(TEXT("LOD %d's triangles along %c faces light %+.2f%% against the interior's %+.2f%%"),
						LOD, TEXT("XYZ")[Class - 1], 100.0 * Stats.FaceBrightness(LOD, Class), 100.0 * Stats.FaceBrightness(LOD, 0));
					return false;
				}
			}
		}
		return true;
	}
}

/**
 * G25 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.8): no line of shading along chunk faces. A mesh job's gradient taps clamp at
 * the edge of its voxels ("the terrain continues": no neighbour dependence), which made the clamped axis's difference
 * one-sided and up to half weight, so the normals of the cells along a chunk face leaned away from it: on a planet at 45
 * degrees, where the surface crosses every chunk face obliquely, dark lines along X and Y faces and bright ones along Z
 * faces under the noon sun (stronger at LOD 1 and 2 once their normals came from voxel-scale gradients, G44). With the
 * clamped difference scaled to the full span (voxel.Mesh.ScaleClampedGradients 1) triangles along X, Y and Z faces light
 * like the interior's at LOD 0, 1 and 2 on gentle relief at 45N 20E and on the equator; without it (0) they do not. Since
 * G62 the shading normals are a fit that skips the voxels a job does not hold instead of clamping taps, so this A/B runs on
 * the central difference (voxel.Mesh.CrossingGradientNormals 0), and the defaults (the fit) must light chunk faces like the
 * interior too.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetStreamingChunkFaceShadingTest,
	"VoxelWorlds.Streaming.Planet.ChunkFaceShading",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetStreamingChunkFaceShadingTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Direction = [](double LatDeg, double LonDeg)
	{
		const double Lat = FMath::DegreesToRadians(LatDeg);
		const double Lon = FMath::DegreesToRadians(LonDeg);
		return FVector(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	};
	const TPair<const TCHAR*, FVector> Places[] = {
		{ TEXT("45N 20E"), Direction(45.0, 20.0) },
		{ TEXT("equator"), Direction(1.0, 1.5) },
	};
	bool bAnyBeforeFails = false;
	for (const TPair<const TCHAR*, FVector>& Place : Places)
	{
		const FVector Sun = FVector(Place.Value.X, Place.Value.Y, 0.0).GetSafeNormal();
		// Mode 0: the central difference, clamp unscaled (before G25); 1: scaled (G25); 2: the defaults (G62's fit).
		FLODNormalStats Before, After, Fit;
		for (int32 Mode = 0; Mode < 3; ++Mode)
		{
			FScopedScaleClampedGradients Scoped(Mode == 0 ? 0 : 1);
			FScopedCrossingGradientNormals ScopedFit(Mode == 2 ? 1 : 0);
			FPlanetStreamingWorld World;
			World.Relief = 400.0f;
			World.Octaves = 2;
			World.Start(TEXT("VoxelPlanetChunkFaceTestWorld"), Place.Value, 300.0f);
			TestTrue(*FString::Printf(TEXT("%s (mode %d): the pipeline settled"), Place.Key, Mode), World.Settle());
			(Mode == 0 ? Before : Mode == 1 ? After : Fit) = MeasureLODNormals(World, Sun);
			World.Stop();
		}
		AddInfo(FString::Printf(TEXT("%s, unscaled clamp: %s"), Place.Key, *Before.DescribeFaces()));
		AddInfo(FString::Printf(TEXT("%s, scaled: %s"), Place.Key, *After.DescribeFaces()));
		AddInfo(FString::Printf(TEXT("%s, the fit (G62): %s"), Place.Key, *Fit.DescribeFaces()));
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			TestTrue(*FString::Printf(TEXT("%s: LOD %d has interior triangles to compare with"), Place.Key, LOD), After.FaceTris[LOD][0] >= 200);
		}
		FString Why;
		const bool bAfterMatches = ChunkFacesMatch(After, Why);
		TestTrue(*FString::Printf(TEXT("%s: triangles along chunk faces light like the interior's %s"), Place.Key, *Why), bAfterMatches);
		FString WhyFit;
		const bool bFitMatches = ChunkFacesMatch(Fit, WhyFit);
		TestTrue(*FString::Printf(TEXT("%s: with the fit too %s"), Place.Key, *WhyFit), bFitMatches);
		FString WhyBefore;
		bAnyBeforeFails |= !ChunkFacesMatch(Before, WhyBefore);
		AddInfo(FString::Printf(TEXT("%s, unscaled clamp: %s"), Place.Key, WhyBefore.IsEmpty() ? TEXT("within the bound") : *WhyBefore));
	}
	TestTrue(TEXT("The unscaled clamp shows lines along chunk faces"), bAnyBeforeFails);
	return true;
}

// ---------------------------------------------------------------------------
// G62 (WORLD_MODES_REVIEW_PLAN.md §14.1): both smooth meshers took their voxel-resolution normals from a central difference
// about the voxel the crossing floors to, whose tap under a surface lies in the saturated rock, so a vertex's error
// depended on where the surface crossed the voxel layer: bands a voxel layer apart at LOD 0, sampled sparsely by LOD 1 and
// 2 (rings around hills from altitude). The fitted gradient (voxel.Mesh.CrossingGradientNormals 1) has no such dependence.
// ---------------------------------------------------------------------------

namespace VoxelPlanetStreamingTestUtils
{
	/**
	 * Streams one place with gentle relief with the central difference before G62 (0) and the fitted gradient (1), in one
	 * meshing mode, and measures both (chunk meshes and seam buckets). Positions are the same either way, so the vertex
	 * counts must agree. LOD 0 must band less than half a degree over the voxel phase with the fit and more than a degree
	 * without it; every LOD's mean angle from the analytic normals must fall.
	 */
	void RunGradientBandsPlace(FAutomationTestBase& Test, const TCHAR* Place, TFunctionRef<void(FPlanetStreamingWorld&)> Start,
		float Relief, int32 Octaves, EMeshingMode MeshingMode)
	{
		const FVector Sun = FVector::UpVector; // unused by the bands
		FLODNormalStats Before, After;
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			FScopedCrossingGradientNormals Scoped(Mode);
			FPlanetStreamingWorld World;
			World.Relief = Relief;
			World.Octaves = Octaves;
			World.MeshingMode = MeshingMode;
			Start(World);
			Test.TestTrue(*FString::Printf(TEXT("%s (gradient %d): the pipeline settled"), Place, Mode), World.Settle());
			(Mode == 0 ? Before : After) = MeasureLODNormals(World, Sun);
			World.Stop();
		}
		Test.AddInfo(FString::Printf(TEXT("%s, before G62: %s"), Place, *Before.DescribeBands()));
		Test.AddInfo(FString::Printf(TEXT("%s, after: %s"), Place, *After.DescribeBands()));
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: LOD %d vertices measured"), Place, LOD), After.Vertices[LOD] > 500);
			Test.TestEqual(*FString::Printf(TEXT("%s: LOD %d vertices the same in either setting (no vertex moves)"), Place, LOD),
				After.Vertices[LOD], Before.Vertices[LOD]);
			Test.TestTrue(*FString::Printf(TEXT("%s: LOD %d's mean angle %.2f below the %.2f before G62"), Place, LOD, After.MeanAngle(LOD), Before.MeanAngle(LOD)),
				After.MeanAngle(LOD) < Before.MeanAngle(LOD));
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: LOD 0 bands %.2f deg over the voxel phase (at most 0.5)"), Place, After.Band(0)), After.Band(0) <= 0.5);
		Test.TestTrue(*FString::Printf(TEXT("%s: the central difference before G62 bands LOD 0 by %.2f deg (more than 1)"), Place, Before.Band(0)),
			Before.Band(0) > 1.0);
	}
}

/**
 * G62 on a planet: Dual Contouring and Marching Cubes chunk meshes and seam buckets from viewers at 45N 20E and on the
 * equator, gentle relief: with the fitted gradient LOD 0's vertex normals do not band over the surface's phase in the voxel
 * grid and every LOD comes closer to the analytic normals, where the central difference before G62 bands.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetStreamingGradientBandsTest,
	"VoxelWorlds.Streaming.Planet.GradientBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetStreamingGradientBandsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Direction = [](double LatDeg, double LonDeg)
	{
		const double Lat = FMath::DegreesToRadians(LatDeg);
		const double Lon = FMath::DegreesToRadians(LonDeg);
		return FVector(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	};
	const TPair<const TCHAR*, FVector> Places[] = {
		{ TEXT("45N 20E"), Direction(45.0, 20.0) },
		{ TEXT("equator"), Direction(1.0, 1.5) },
	};
	for (const TPair<const TCHAR*, FVector>& Place : Places)
	{
		auto Start = [&Place](FPlanetStreamingWorld& World) { World.Start(TEXT("VoxelPlanetGradientBandsTestWorld"), Place.Value, 300.0f); };
		RunGradientBandsPlace(*this, *FString::Printf(TEXT("DC %s"), Place.Key), Start, 400.0f, 2, EMeshingMode::DualContouring);
		RunGradientBandsPlace(*this, *FString::Printf(TEXT("MC %s"), Place.Key), Start, 400.0f, 2, EMeshingMode::MarchingCubes);
	}
	return true;
}

/**
 * G62 on a heightmap world: the Plane.LODNormals place (gentle hills) in Dual Contouring and Marching Cubes, under the same
 * bounds as the planet's.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneStreamingGradientBandsTest,
	"VoxelWorlds.Streaming.Plane.GradientBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneStreamingGradientBandsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Start = [](FPlanetStreamingWorld& World) { World.StartPlane(TEXT("VoxelPlaneGradientBandsTestWorld"), 23000.0f, 7000.0f, 300.0f); };
	RunGradientBandsPlace(*this, TEXT("DC plane east"), Start, 400.0f, 1, EMeshingMode::DualContouring);
	RunGradientBandsPlace(*this, TEXT("MC plane east"), Start, 400.0f, 1, EMeshingMode::MarchingCubes);
	return true;
}

// ---------------------------------------------------------------------------
// G65 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.13): far-compressed chunk data read off the game thread. A far chunk that
// compression had compacted was expanded in place on the game thread by every reader that reached it as the viewer came
// near (mesh jobs, seam jobs, collision cooks, the water pull), each about 0.3 ms at 64^3. Since G65 those readers read a
// snapshot that a worker task decodes, started when a chunk in its neighbourhood is queued, and the chunk stays compressed.
// ---------------------------------------------------------------------------

namespace VoxelPlanetStreamingTestUtils
{
	/** Sets an int console variable at console priority for a scope. */
	struct FScopedIntCVar
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 0;

		FScopedIntCVar(const TCHAR* Name, int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedIntCVar()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/** What a move over far-compressed chunks cost the game thread, and what it left. */
	struct FFarReadsResult
	{
		int32 CompactBefore = 0;      // chunks held compact (uniform or compressed) when the move began
		int32 GameThreadDecodes = 0;  // expansions on the game thread and its waits for worker decodes, during the move
		float GameThreadMs = 0.0f;
		int64 SnapshotDecodes = 0;    // snapshots decoded by worker tasks (the session)
		TMap<FIntVector, uint32> VoxelCrcs; // every loaded chunk's voxels after the move, water flags masked
		TMap<FIntVector, int32> WaterCounts; // and its water-flagged voxels
		uint32 MeshCrc = 0;           // every chunk mesh and seam bucket after the move
	};

	/** CRC of the renderer's chunk meshes and seam buckets, in coordinate order. */
	uint32 MeshesCrc(const FPlanetStreamingWorld& World)
	{
		uint32 Crc = 0;
		auto AddMap = [&Crc](const TMap<FIntVector, FChunkMeshData>& Meshes)
		{
			TArray<FIntVector> Keys;
			Meshes.GetKeys(Keys);
			Keys.Sort([](const FIntVector& A, const FIntVector& B)
			{
				return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z);
			});
			for (const FIntVector& Key : Keys)
			{
				const FChunkMeshData& Mesh = Meshes[Key];
				Crc = FCrc::MemCrc32(&Key, sizeof(Key), Crc);
				Crc = FCrc::MemCrc32(Mesh.Positions.GetData(), Mesh.Positions.Num() * Mesh.Positions.GetTypeSize(), Crc);
				Crc = FCrc::MemCrc32(Mesh.Normals.GetData(), Mesh.Normals.Num() * Mesh.Normals.GetTypeSize(), Crc);
				Crc = FCrc::MemCrc32(Mesh.Indices.GetData(), Mesh.Indices.Num() * Mesh.Indices.GetTypeSize(), Crc);
			}
		};
		AddMap(World.Renderer.ChunkMeshes);
		AddMap(World.Renderer.SeamBuckets);
		return Crc;
	}

	/**
	 * Streams a place, lets far compression compact the far band (idle frames 5), then moves the viewer by Move so its LOD
	 * bands sweep over compressed chunks, and records the move's game-thread decodes and what it left.
	 */
	FFarReadsResult RunFarReads(FAutomationTestBase& Test, const TCHAR* Place, TFunctionRef<void(FPlanetStreamingWorld&)> Start,
		const FVector& Move, int32 SnapshotReads)
	{
		FScopedIntCVar Reads(TEXT("voxel.FarCompression.SnapshotReads"), SnapshotReads);
		FScopedIntCVar Idle(TEXT("voxel.FarCompression.IdleFrames"), 5);
		FScopedIntCVar Scans(TEXT("voxel.FarCompression.MaxScansPerTick"), 64);
		FFarReadsResult Result;
		FPlanetStreamingWorld World;
		Start(World);
		Test.TestTrue(*FString::Printf(TEXT("%s (snapshot reads %d): the pipeline settled"), Place, SnapshotReads), World.Settle());

		// The far band compacts: tick until the compact count has held for 60 ticks.
		int32 LastCompact = -1;
		int32 Still = 0;
		for (int32 Tick = 0; Tick < 3000 && Still < 60; ++Tick)
		{
			World.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FPlatformProcess::Sleep(0.002f);
			const UVoxelChunkManager::FVoxelMemoryStats Memory = World.Manager->GetVoxelMemoryStats();
			const int32 Compact = Memory.UniformChunks + Memory.CompressedChunks;
			Still = (Compact == LastCompact) ? Still + 1 : 0;
			LastCompact = Compact;
		}
		Result.CompactBefore = LastCompact;

		int32 Ignored = 0;
		float IgnoredMs = 0.0f;
		VoxelResidencyStats::ConsumeGameThread(Ignored, IgnoredMs);
		World.Viewer += Move;
		World.Manager->SetBenchmarkView(true, World.Viewer);
		Test.TestTrue(*FString::Printf(TEXT("%s (snapshot reads %d): settled after the move"), Place, SnapshotReads), World.Settle());
		VoxelResidencyStats::ConsumeGameThread(Result.GameThreadDecodes, Result.GameThreadMs);
		Result.SnapshotDecodes = World.Manager->GetSnapshotDecodeCount();

		TArray<FIntVector> Loaded;
		World.Manager->GetLoadedChunks(Loaded);
		for (const FIntVector& Coord : Loaded)
		{
			const TSharedPtr<const TArray<FVoxelData>> Voxels = World.Manager->GetEditMergedSnapshot(Coord);
			if (Voxels.IsValid())
			{
				// The water flag is masked: the water pull's result at a chunk corner depends on the order neighbouring
				// chunks finish meshing, either way (G70); it is compared apart.
				TArray<FVoxelData> Masked = *Voxels;
				int32 Water = 0;
				for (FVoxelData& Voxel : Masked)
				{
					Water += Voxel.HasWaterFlag() ? 1 : 0;
					Voxel.SetWaterFlag(false);
				}
				Result.VoxelCrcs.Add(Coord, FCrc::MemCrc32(Masked.GetData(), Masked.Num() * sizeof(FVoxelData)));
				Result.WaterCounts.Add(Coord, Water);
			}
		}
		Result.MeshCrc = MeshesCrc(World);
		World.Stop();
		return Result;
	}

	/** Runs a place with the reads as before G65 and as they are, and checks the second moves them off the game thread. */
	void RunFarReadsPlace(FAutomationTestBase& Test, const TCHAR* Place, TFunctionRef<void(FPlanetStreamingWorld&)> Start, const FVector& Move)
	{
		const FFarReadsResult Before = RunFarReads(Test, Place, Start, Move, 0);
		const FFarReadsResult After = RunFarReads(Test, Place, Start, Move, 1);
		Test.AddInfo(FString::Printf(TEXT("%s: compact chunks before the move %d / %d; game-thread decodes in the move: before G65 %d (%.1f ms), after %d (%.1f ms); snapshots decoded on workers %lld"),
			Place, Before.CompactBefore, After.CompactBefore, Before.GameThreadDecodes, Before.GameThreadMs, After.GameThreadDecodes,
			After.GameThreadMs, After.SnapshotDecodes));
		Test.TestTrue(*FString::Printf(TEXT("%s: the far band compacted (%d chunks)"), Place, After.CompactBefore), After.CompactBefore >= 20);
		Test.TestTrue(*FString::Printf(TEXT("%s: before G65 the move decoded far chunks on the game thread (%d)"), Place, Before.GameThreadDecodes),
			Before.GameThreadDecodes >= 20);
		Test.TestTrue(*FString::Printf(TEXT("%s: after, at most a tenth of them (%d against %d)"), Place, After.GameThreadDecodes, Before.GameThreadDecodes),
			After.GameThreadDecodes * 10 <= Before.GameThreadDecodes);
		Test.TestTrue(*FString::Printf(TEXT("%s: after, the snapshots decode on workers (%lld)"), Place, After.SnapshotDecodes), After.SnapshotDecodes > 0);

		// The same world either way: every chunk's voxels and every mesh and seam. Water flags are reported: where they
		// differ, it is the water pull's order dependence (G70), which runs of either setting show alike.
		Test.TestEqual(*FString::Printf(TEXT("%s: the same chunks loaded"), Place), After.VoxelCrcs.Num(), Before.VoxelCrcs.Num());
		int32 Different = 0;
		for (const TPair<FIntVector, uint32>& Pair : After.VoxelCrcs)
		{
			const uint32* Other = Before.VoxelCrcs.Find(Pair.Key);
			Different += (!Other || *Other != Pair.Value) ? 1 : 0;
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: every chunk's voxels the same (water flags apart)"), Place), Different, 0);
		int32 WaterDifferent = 0;
		for (const TPair<FIntVector, int32>& Pair : After.WaterCounts)
		{
			const int32* Other = Before.WaterCounts.Find(Pair.Key);
			if (Other && *Other != Pair.Value)
			{
				++WaterDifferent;
				Test.AddInfo(FString::Printf(TEXT("%s: chunk %s holds %d water voxels against %d (the water pull's order, G70)"),
					Place, *Pair.Key.ToString(), Pair.Value, *Other));
			}
		}
		Test.AddInfo(FString::Printf(TEXT("%s: %d of %d chunks with a different water count"), Place, WaterDifferent, After.WaterCounts.Num()));
		Test.TestEqual(*FString::Printf(TEXT("%s: every mesh and seam the same"), Place), After.MeshCrc, Before.MeshCrc);
	}
}

/**
 * G65 on a planet (45N 20E, a sea at the datum, so the water pull reads too): moving the viewer 3,000 uu over a far band
 * that compression has compacted, the reads of the compressed chunks no longer expand them on the game thread (at most a
 * tenth of the decodes voxel.FarCompression.SnapshotReads 0 does there), and the world is the same either way: every chunk's
 * voxels and every mesh and seam.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetStreamingFarReadsTest,
	"VoxelWorlds.Streaming.Planet.FarReadsOffGameThread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetStreamingFarReadsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	const double Lat = FMath::DegreesToRadians(45.0);
	const double Lon = FMath::DegreesToRadians(20.0);
	const FVector Direction(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	const FVector East = FVector::CrossProduct(FVector::UpVector, Direction).GetSafeNormal();
	auto Start = [&Direction](FPlanetStreamingWorld& World) { World.Start(TEXT("VoxelPlanetFarReadsTestWorld"), Direction, 300.0f); };
	RunFarReadsPlace(*this, TEXT("45N 20E"), Start, East * 3000.0);
	return true;
}

/** G65 on a heightmap world: the same over the plane of the closed-surface test, moving 3,000 uu east. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneStreamingFarReadsTest,
	"VoxelWorlds.Streaming.Plane.FarReadsOffGameThread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneStreamingFarReadsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Start = [](FPlanetStreamingWorld& World) { World.StartPlane(TEXT("VoxelPlaneFarReadsTestWorld"), 23000.0f, 7000.0f, 300.0f); };
	RunFarReadsPlace(*this, TEXT("plane east"), Start, FVector(3000.0, 0.0, 0.0));
	return true;
}


namespace VoxelPlanetStreamingTestUtils
{
	/**
	 * One chunk's collision request prepared as a cook prepares it, with its neighbours' snapshots (voxel.Collision.
	 * NeighborShells 0) or boundary shells (1), the slices extracted as the cook's worker does, and meshed: the CRC of the
	 * mesh and of the neighbours the cook reads as air.
	 */
	uint32 CollisionCookCrc(UVoxelChunkManager* Manager, const FIntVector& Coord, int32 LODLevel, bool bShells, int32& OutNeighbors,
		int32& OutShells)
	{
		FScopedIntCVar Shells(TEXT("voxel.Collision.NeighborShells"), bShells ? 1 : 0);
		FVoxelMeshingRequest Request;
		TSharedPtr<const TArray<FVoxelData>> Own;
		TMap<FIntVector, TSharedPtr<const TArray<FVoxelData>>> Snapshots;
		TMap<FIntVector, TSharedPtr<const FVoxelChunkShell>> ShellMap;
		uint32 Missing = 0;
		TArray<UE::Tasks::FTask> Ready;
		OutNeighbors = 0;
		OutShells = 0;
		if (!Manager->PrepareCollisionMeshRequest(Coord, LODLevel, Request, Own, Snapshots, &Missing, &Ready, &ShellMap) || !Own.IsValid())
		{
			return 0;
		}
		for (UE::Tasks::FTask& Task : Ready)
		{
			Task.Wait();
		}
		Request.VoxelData = *Own;
		const int32 CS = Request.ChunkSize;
		OutNeighbors = ShellMap.Num() + Snapshots.Num();
		OutShells = ShellMap.Num();
		auto Has = [&ShellMap, &Snapshots](const FIntVector& N) { return ShellMap.Contains(N) || Snapshots.Contains(N); };
		auto Get = [&ShellMap, &Snapshots, CS](const FIntVector& N, int32 X, int32 Y, int32 Z) -> FVoxelData
		{
			if (const TSharedPtr<const FVoxelChunkShell>* Shell = ShellMap.Find(N))
			{
				return (*Shell)->Get(X, Y, Z);
			}
			if (const TSharedPtr<const TArray<FVoxelData>>* Array = Snapshots.Find(N))
			{
				const int32 Index = X + Y * CS + Z * CS * CS;
				return (*Array)->IsValidIndex(Index) ? (**Array)[Index] : FVoxelData::Air();
			}
			return FVoxelData::Air();
		};
		VoxelNeighborSlices::Extract(CS, Coord, Manager->IsDeepSliceOff(), Manager->IsDeepSliceFull(), Has, Get, Request);
		FChunkMeshData Mesh;
		Manager->GetMesherPtr()->GenerateMeshCPU(Request, Mesh);
		uint32 Crc = FCrc::MemCrc32(&Missing, sizeof(Missing));
		Crc = FCrc::MemCrc32(Mesh.Positions.GetData(), Mesh.Positions.Num() * Mesh.Positions.GetTypeSize(), Crc);
		Crc = FCrc::MemCrc32(Mesh.Normals.GetData(), Mesh.Normals.Num() * Mesh.Normals.GetTypeSize(), Crc);
		Crc = FCrc::MemCrc32(Mesh.Indices.GetData(), Mesh.Indices.Num() * Mesh.Indices.GetTypeSize(), Crc);
		return Crc;
	}

	/** Empty the snapshot and shell caches: the age sweep (every 64 ticks) with a maximum age of one tick. */
	void EvictSnapshotCaches(FPlanetStreamingWorld& World)
	{
		FScopedIntCVar Age(TEXT("voxel.Snapshot.MaxAgeTicks"), 1);
		for (int32 Tick = 0; Tick < 140; ++Tick)
		{
			World.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
		}
	}

	/**
	 * G69: streams a place, lets far compression compact the far band and digs one hole (a chunk with edits), then cooks
	 * every loaded chunk's collision request at LOD 0 and 1 with the neighbours' snapshots and with their shells: every
	 * mesh is the same, and with shells the cache holds a fraction of the bytes.
	 */
	void RunCollisionShellsPlace(FAutomationTestBase& Test, const TCHAR* Place, TFunctionRef<void(FPlanetStreamingWorld&)> Start)
	{
		FScopedIntCVar Reads(TEXT("voxel.FarCompression.SnapshotReads"), 1);
		FScopedIntCVar Idle(TEXT("voxel.FarCompression.IdleFrames"), 5);
		FScopedIntCVar Scans(TEXT("voxel.FarCompression.MaxScansPerTick"), 64);
		FPlanetStreamingWorld World;
		Start(World);
		Test.TestTrue(*FString::Printf(TEXT("%s: the pipeline settled"), Place), World.Settle());

		// A dig near the viewer: a chunk with edits (its shell is cut from its edit-merged snapshot).
		FVoxelSurfaceSample Ground;
		if (World.Manager->QueryEditMergedSurfacePoint(World.Viewer, Ground))
		{
			FVoxelBrushParams Brush;
			Brush.Shape = EVoxelBrushShape::Sphere;
			Brush.Radius = 300.0f;
			Brush.Strength = 1.0f;
			Brush.FalloffType = EVoxelBrushFalloff::Linear;
			Brush.DensityDelta = 255;
			World.Manager->GetEditManager()->ApplyBrushEdit(Ground.Position, Brush, EEditMode::Subtract);
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: settled after the dig"), Place), World.Settle());

		// The far band compacts: tick until the compact count has held for 60 ticks.
		int32 LastCompact = -1;
		int32 Still = 0;
		for (int32 Tick = 0; Tick < 3000 && Still < 60; ++Tick)
		{
			World.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FPlatformProcess::Sleep(0.002f);
			const UVoxelChunkManager::FVoxelMemoryStats Memory = World.Manager->GetVoxelMemoryStats();
			const int32 Compact = Memory.UniformChunks + Memory.CompressedChunks;
			Still = (Compact == LastCompact) ? Still + 1 : 0;
			LastCompact = Compact;
		}
		const UVoxelChunkManager::FVoxelMemoryStats Residency = World.Manager->GetVoxelMemoryStats();
		Test.TestTrue(*FString::Printf(TEXT("%s: resident, compressed and uniform chunks (%d / %d / %d)"), Place, Residency.ResidentChunks,
			Residency.CompressedChunks, Residency.UniformChunks),
			Residency.ResidentChunks > 0 && Residency.CompressedChunks > 0 && Residency.UniformChunks > 0);
		TArray<FIntVector> Edited;
		World.Manager->GetEditManager()->GetEditedChunkCoords(Edited);
		Test.TestTrue(*FString::Printf(TEXT("%s: the dig made chunks with edits (%d)"), Place, Edited.Num()), Edited.Num() > 0);

		TArray<FIntVector> Loaded;
		World.Manager->GetLoadedChunks(Loaded);
		Loaded.RemoveAll([&World](const FIntVector& C) { return !World.Manager->HasChunkVoxelData(C); });
		Loaded.Sort([](const FIntVector& A, const FIntVector& B) { return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z); });

		for (const int32 LOD : { 0, 1 })
		{
			const int32 Depth = VoxelNeighborSlices::GetNeighborDepth(LOD, World.Manager->IsDeepSliceOff(), World.Manager->IsDeepSliceFull(),
				World.Config->ChunkSize);
			TArray<uint32> Before, After;
			int32 Neighbors = 0, ShellReads = 0, Different = 0, Meshed = 0, NeighborsBefore = 0;
			EvictSnapshotCaches(World);
			for (const FIntVector& Coord : Loaded)
			{
				int32 N = 0, S = 0;
				Before.Add(CollisionCookCrc(World.Manager, Coord, LOD, false, N, S));
				NeighborsBefore += N;
			}
			const UVoxelChunkManager::FVoxelMemoryStats WithSnapshots = World.Manager->GetVoxelMemoryStats();
			const FString DescribeSnapshots = World.Manager->DescribeSnapshotCache();
			EvictSnapshotCaches(World);
			for (const FIntVector& Coord : Loaded)
			{
				int32 N = 0, S = 0;
				After.Add(CollisionCookCrc(World.Manager, Coord, LOD, true, N, S));
				Neighbors += N;
				ShellReads += S;
			}
			const UVoxelChunkManager::FVoxelMemoryStats WithShells = World.Manager->GetVoxelMemoryStats();
			const FString DescribeShells = World.Manager->DescribeSnapshotCache();
			Test.AddInfo(FString::Printf(TEXT("%s LOD %d with snapshots: %s"), Place, LOD, *DescribeSnapshots.Replace(TEXT("\n"), TEXT(" | "))));
			Test.AddInfo(FString::Printf(TEXT("%s LOD %d with shells: %s"), Place, LOD, *DescribeShells.Replace(TEXT("\n"), TEXT(" | "))));
			for (int32 I = 0; I < Before.Num(); ++I)
			{
				Meshed += Before[I] != 0 ? 1 : 0;
				Different += Before[I] != After[I] ? 1 : 0;
			}
			const double ToKB = 1.0 / 1024.0;
			Test.AddInfo(FString::Printf(TEXT("%s LOD %d (shell depth %d): %d chunks cooked, %d neighbour reads (%d from shells); cache %.0f KB ")
				TEXT("with snapshots, %.0f KB with shells (shells %.0f KB, full snapshots %.0f KB)"), Place, LOD, Depth, Loaded.Num(), Neighbors,
				ShellReads, WithSnapshots.SnapshotBytes * ToKB, WithShells.SnapshotBytes * ToKB, WithShells.ShellBytes * ToKB,
				(WithShells.SnapshotBytes - WithShells.ShellBytes) * ToKB));
			Test.TestTrue(*FString::Printf(TEXT("%s LOD %d: chunks cooked (%d)"), Place, LOD, Meshed), Meshed >= 20);
			Test.TestEqual(*FString::Printf(TEXT("%s LOD %d: the same neighbours read"), Place, LOD), Neighbors, NeighborsBefore);
			Test.TestEqual(*FString::Printf(TEXT("%s LOD %d: every collision mesh the same with shells"), Place, LOD), Different, 0);
			// Shells are taken while one is at most three quarters of a chunk (8 * Depth <= ChunkSize).
			const int32 CS = World.Config->ChunkSize;
			const bool bShellsUsed = 8 * Depth <= CS;
			Test.TestEqual(*FString::Printf(TEXT("%s LOD %d: %s"), Place, LOD,
				bShellsUsed ? TEXT("every neighbour from a shell") : TEXT("no shell deeper than three quarters of a chunk")),
				ShellReads, bShellsUsed ? Neighbors : 0);
			if (bShellsUsed)
			{
				// A shell holds 6 * Depth / ChunkSize of its chunk (the faces, overlapping at the edges); the full snapshots
				// left are the edited chunks', and no cook's own (nothing else read them).
				const double Share = 6.0 * Depth / CS;
				Test.TestTrue(*FString::Printf(TEXT("%s LOD %d: the cache with shells about the shells' share of the snapshots' (%.0f KB against %.0f KB x %.2f)"),
					Place, LOD, WithShells.SnapshotBytes * ToKB, WithSnapshots.SnapshotBytes * ToKB, Share),
					WithShells.SnapshotBytes <= 1.25 * Share * WithSnapshots.SnapshotBytes);
			}
		}
		World.Stop();
	}
}

/**
 * G69 on a planet (45N 20E): collision cooks read their neighbours from boundary shells (voxel.Collision.NeighborShells), the
 * same collision meshes as from the neighbours' whole snapshots at LOD 0 and 1, and the snapshot cache holds a fraction of
 * the bytes: resident, compressed, uniform and edited neighbours.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetCollisionShellsTest,
	"VoxelWorlds.Streaming.Planet.CollisionNeighborShells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetCollisionShellsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	const double Lat = FMath::DegreesToRadians(45.0);
	const double Lon = FMath::DegreesToRadians(20.0);
	const FVector Direction(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	auto Start = [&Direction](FPlanetStreamingWorld& World) { World.Start(TEXT("VoxelPlanetCollisionShellsTestWorld"), Direction, 300.0f); };
	RunCollisionShellsPlace(*this, TEXT("45N 20E"), Start);
	return true;
}

/** G69 on a heightmap world: the same over the plane of the far-reads test. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneCollisionShellsTest,
	"VoxelWorlds.Streaming.Plane.CollisionNeighborShells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneCollisionShellsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetStreamingTestUtils;

	auto Start = [](FPlanetStreamingWorld& World) { World.StartPlane(TEXT("VoxelPlaneCollisionShellsTestWorld"), 23000.0f, 7000.0f, 300.0f); };
	RunCollisionShellsPlace(*this, TEXT("plane"), Start);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
