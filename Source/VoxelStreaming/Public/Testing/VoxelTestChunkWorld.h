// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "Testing/VoxelTestScopedCVar.h"

/**
 * Test-only helpers: a real chunk manager streaming a small world into a renderer that records what it is given, for
 * automation tests in any module that depends on VoxelStreaming (spherical Phase 4's tests: the surface queries, scatter,
 * PCG, the map, navigation; WORLD_MODES_REVIEW_PLAN.md §18). The one world fixture and the one recording renderer of the
 * plugin's tests (code review CR2 row 17, STREAM-23 / REND-07): the suites that had their own copies derive their
 * configuration from FTestChunkWorld and read the recorder's maps. Compiled only with WITH_DEV_AUTOMATION_TESTS.
 */
namespace VoxelTesting
{
	/**
	 * A renderer that keeps what the chunk manager submits and nothing else: the latest CPU mesh per chunk with its LOD,
	 * the latest seam bucket per owner chunk, the latest water tile mesh per tile (a heightmap world's 2D tiles), and the
	 * submit counts; it can also drop everything as a recreated scene proxy does (G15) and report the loss once.
	 */
	class FRecordingMeshRenderer : public IVoxelMeshRenderer
	{
	public:
		/** Keep each chunk's and tile's mesh (false: the key with an empty mesh, for a world whose meshes no test reads). */
		bool bKeepMeshes = true;

		TMap<FIntVector, FChunkMeshData> ChunkMeshes;
		/** The LOD each chunk's latest mesh was submitted at. */
		TMap<FIntVector, int32> ChunkLODs;
		/** The latest valid seam bucket per owner chunk (an empty submission removes the owner's). */
		TMap<FIntVector, FChunkMeshData> SeamBuckets;
		/** Water tile meshes by tile (chunk X, Y); positions local to the tile's corner at WorldOrigin + tile * chunk extent. */
		TMap<FIntVector2, FChunkMeshData> WaterTiles;

		/** Chunk meshes submitted (every submission). */
		int32 ChunkSubmits = 0;
		/** Seam buckets submitted (every submission, empty ones included). */
		int32 SeamSubmits = 0;
		/** Chunk meshes submitted for a chunk that already had one (re-meshes; the streaming bench's thrash, G14). */
		int64 ChunkResubmits = 0;

		virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override { bInitialized = true; }
		virtual void Shutdown() override { bInitialized = false; }
		virtual bool IsInitialized() const override { return bInitialized; }
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override
		{
			++ChunkSubmits;
			ChunkResubmits += ChunkMeshes.Contains(ChunkCoord) ? 1 : 0;
			ChunkMeshes.Add(ChunkCoord, bKeepMeshes ? MeshData : FChunkMeshData());
			ChunkLODs.Add(ChunkCoord, LODLevel);
		}
		virtual void RemoveChunk(const FIntVector& ChunkCoord) override
		{
			ChunkMeshes.Remove(ChunkCoord);
			ChunkLODs.Remove(ChunkCoord);
		}
		virtual void ClearAllChunks() override
		{
			ChunkMeshes.Reset();
			ChunkLODs.Reset();
		}
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
		virtual void UpdateSeamMeshFromCPU(const FIntVector& OwnerChunkCoord, uint8 SeamSlot, int32 LODLevel, FChunkMeshData&& MeshData, bool bCrossfade) override
		{
			++SeamSubmits;
			if (MeshData.IsValid())
			{
				SeamBuckets.Add(OwnerChunkCoord, bKeepMeshes ? MoveTemp(MeshData) : FChunkMeshData());
			}
			else
			{
				SeamBuckets.Remove(OwnerChunkCoord);
			}
		}
		virtual void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 SeamSlot) override { SeamBuckets.Remove(OwnerChunkCoord); }
		virtual void ClearAllSeamMeshes() override { SeamBuckets.Reset(); }
		virtual void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const override { SeamBuckets.GetKeys(OutOwners); }
		virtual void UpdateWaterTileMesh(const FIntVector2& TileCoord, const FChunkMeshData& WaterMeshData) override { WaterTiles.Add(TileCoord, bKeepMeshes ? WaterMeshData : FChunkMeshData()); }
		virtual void RemoveWaterTile(const FIntVector2& TileCoord) override { WaterTiles.Remove(TileCoord); }
		virtual void ClearAllWaterTiles() override { WaterTiles.Reset(); }
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

		/** Drop everything held, as a recreated scene proxy does (G15), and report the chunks once through ConsumeLostRenderData. */
		void SimulateProxyRecreated()
		{
			ChunkMeshes.GetKeys(LostChunks);
			ChunkMeshes.Reset();
			ChunkLODs.Reset();
			SeamBuckets.Reset();
			WaterTiles.Reset();
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

	/**
	 * A transient game world with a configuration, a chunk manager on an actor and the recording renderer. Create the
	 * world, fill Config (or use ConfigureSmallPlanet / ConfigureSmallPlane and the others), StartManager, SetViewer,
	 * Settle; Stop at the end (the destructor stops too). The defaults stream on the CPU with no collision, scatter or
	 * caves. A suite with its own configuration derives from this and keeps only that.
	 */
	struct FTestChunkWorld
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		AActor* Owner = nullptr;
		UVoxelChunkManager* Manager = nullptr;
		FRecordingMeshRenderer Renderer;

		/** Idle ticks Settle waits for after the pipeline empties (a suite that settled on 60 sets it). */
		int32 SettleIdleTicks = 90;

		FTestChunkWorld() = default;
		FTestChunkWorld(const FTestChunkWorld&) = delete;
		FTestChunkWorld& operator=(const FTestChunkWorld&) = delete;
		~FTestChunkWorld() { Stop(); }

		/**
		 * A transient game world and a rooted configuration.
		 * @param WorldName The world's name
		 * @param bInitializeActorsForPlay Initialise the world's actors for play (a player controller then spawns its camera
		 *        manager and joins the world's controllers)
		 */
		void Create(const TCHAR* WorldName, bool bInitializeActorsForPlay = false)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
			if (bInitializeActorsForPlay)
			{
				World->InitializeActorsForPlay(FURL());
			}
			Config = NewObject<UVoxelWorldConfiguration>();
			Config->AddToRoot();
		}

		/** The settings every helper world shares: CPU generation and meshing, DC, no collision, scatter, caves or biomes. */
		void ConfigureCommon(float VoxelSize, int32 ChunkSize, float ViewDistance)
		{
			Config->MeshingMode = EMeshingMode::DualContouring;
			Config->bEnableLOD = false;
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ViewDistance = ViewDistance;
			Config->LODBands = { FLODBand(0.0f, ViewDistance, 0, 1) };
			Config->bUseGPUGeneration = false;
			Config->bGenerateCollision = false;
			Config->bEnableScatter = false;
			Config->bEnableCaves = false;
			Config->bEnableBiomes = false;
			Config->bEnableWaterLevel = false;
			Config->NoiseParams.Seed = 9001;
			Config->NoiseParams.Frequency = 0.0004f;
			Config->NoiseParams.Octaves = 3;
		}

		/** A small planet: radius 20,000, terrain within +-800, chunks of 32 voxels of 50 uu, view distance 3,000. */
		void ConfigureSmallPlanet(float Radius = 20000.0f)
		{
			ConfigureCommon(50.0f, 32, 3000.0f);
			Config->WorldMode = EWorldMode::SphericalPlanet;
			Config->WorldOrigin = FVector::ZeroVector;
			Config->WorldRadius = Radius;
			Config->PlanetHeightScale = 1600.0f;
			Config->BaseHeight = 0.0f;
			Config->PlanetMaxTerrainHeight = 1000.0f;
			Config->PlanetMaxTerrainDepth = 1000.0f;
		}

		/** A plane with the same footprint and terrain amplitude (chunks of 32 voxels of 50 uu). */
		void ConfigureSmallPlane()
		{
			ConfigureCommon(50.0f, 32, 3000.0f);
			Config->WorldMode = EWorldMode::InfinitePlane;
			Config->WorldOrigin = FVector::ZeroVector;
			Config->HeightScale = 800.0f;
			Config->BaseHeight = 0.0f;
		}

		/**
		 * LOD on with three bands (0 to 2,000 at LOD 0, to 4,000 at LOD 1, to the view distance at LOD 2) and the view
		 * distance 6,000: the planet streaming and orbit suites' bands.
		 */
		void ConfigureThreeLODBands(float ViewDistance = 6000.0f)
		{
			Config->bEnableLOD = true;
			Config->ViewDistance = ViewDistance;
			Config->LODBands = {
				FLODBand(0.0f, 2000.0f, 0, 1),
				FLODBand(2000.0f, 4000.0f, 1, 2),
				FLODBand(4000.0f, ViewDistance, 2, 4) };
		}

		/** Clamp the default LOD bands to the view distance as the world actor does, dropping bands beyond it. */
		void ClampLODBandsToViewDistance()
		{
			for (FLODBand& Band : Config->LODBands)
			{
				Band.MaxDistance = FMath::Min(Band.MaxDistance, Config->ViewDistance);
				Band.MinDistance = FMath::Min(Band.MinDistance, Config->ViewDistance);
			}
			const float ViewDistance = Config->ViewDistance;
			Config->LODBands.RemoveAll([ViewDistance](const FLODBand& Band) { return Band.MinDistance >= ViewDistance; });
		}

		/** The owner actor and the chunk manager component, not yet initialized (add conditioning zones, then InitializeManager). */
		void SpawnManager()
		{
			Owner = World->SpawnActor<AActor>();
			Manager = NewObject<UVoxelChunkManager>(Owner);
			Manager->RegisterComponent();
		}

		/** Initialize the manager on Config with a distance-band LOD strategy and the recording renderer. */
		void InitializeManager()
		{
			Renderer.Initialize(World, Config);
			Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);
		}

		void StartManager()
		{
			SpawnManager();
			InitializeManager();
		}

		/** Stream around a fixed viewer (the bench's viewer override), the bench counters reset. */
		void SetViewer(const FVector& Viewer)
		{
			Manager->ResetBenchCounters();
			Manager->SetBenchmarkView(true, Viewer);
		}

		/** The viewer ViewerAltitude above the planet's natural surface along a direction (the planet streaming suite's). */
		FVector ViewerOverSurface(const FVector& Direction, float ViewerAltitude) const
		{
			return Manager->GetSurfacePointAt(Direction).Position + Direction * ViewerAltitude;
		}

		/** The viewer ViewerAltitude above the generated ground at (X, Y) on a heightmap world. */
		FVector ViewerOverGround(double X, double Y, float ViewerAltitude) const
		{
			return FVector(X, Y, Manager->GetGeneratedSurfacePoint(FVector(X, Y, 0.0)).Z + ViewerAltitude);
		}

		/** One tick of the manager. */
		void Tick()
		{
			Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FPlatformProcess::Sleep(0.002f);
		}

		/** Nothing queued, in flight or waiting to upload. */
		bool IsIdle() const
		{
			return Manager->GetPendingGenerationCount() == 0 && Manager->GetPendingMeshingCount() == 0
				&& Manager->GetAsyncGenerationInProgressCount() == 0 && Manager->GetAsyncMeshingInProgressCount() == 0
				&& Manager->GetPendingMeshUploadCount() == 0 && Manager->GetSeamJobsInFlightCount() == 0;
		}

		/** Tick until the pipeline has been idle for SettleIdleTicks ticks; false if it does not settle within TimeoutSeconds. */
		bool Settle(double TimeoutSeconds = 120.0)
		{
			int32 IdleTicks = 0;
			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
			while (IdleTicks < SettleIdleTicks && FPlatformTime::Seconds() < Deadline)
			{
				Tick();
				IdleTicks = IsIdle() ? IdleTicks + 1 : 0;
			}
			return IdleTicks >= SettleIdleTicks;
		}

		void Stop()
		{
			if (Manager)
			{
				Manager->SetBenchmarkView(false, FVector::ZeroVector);
				Manager->Shutdown();
			}
			if (Owner)
			{
				Owner->Destroy();
			}
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
			if (Config)
			{
				Config->RemoveFromRoot();
			}
			Manager = nullptr;
			Owner = nullptr;
			World = nullptr;
			Config = nullptr;
		}
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
