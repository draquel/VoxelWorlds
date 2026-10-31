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

/**
 * Test-only helpers: a real chunk manager streaming a small world into a renderer that only records chunk meshes, for
 * automation tests in any module that depends on VoxelStreaming (spherical Phase 4's tests: the surface queries, scatter,
 * PCG, the map, navigation; WORLD_MODES_REVIEW_PLAN.md §18). Compiled only with WITH_DEV_AUTOMATION_TESTS.
 */
namespace VoxelTesting
{
	/** A renderer that keeps the latest CPU mesh per chunk and per water tile (a heightmap world's 2D water tiles), and nothing else. */
	class FRecordingMeshRenderer : public IVoxelMeshRenderer
	{
	public:
		TMap<FIntVector, FChunkMeshData> ChunkMeshes;
		/** Water tile meshes by tile (chunk X, Y); positions local to the tile's corner at WorldOrigin + tile * chunk extent. */
		TMap<FIntVector2, FChunkMeshData> WaterTiles;
		/** Chunk meshes submitted for a chunk that already had one (re-meshes; the streaming bench's thrash, G14). */
		int64 ChunkResubmits = 0;

		virtual void UpdateWaterTileMesh(const FIntVector2& TileCoord, const FChunkMeshData& WaterMeshData) override { WaterTiles.Add(TileCoord, WaterMeshData); }
		virtual void RemoveWaterTile(const FIntVector2& TileCoord) override { WaterTiles.Remove(TileCoord); }
		virtual void ClearAllWaterTiles() override { WaterTiles.Reset(); }

		virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override { bInitialized = true; }
		virtual void Shutdown() override { bInitialized = false; }
		virtual bool IsInitialized() const override { return bInitialized; }
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override
		{
			ChunkResubmits += ChunkMeshes.Contains(ChunkCoord) ? 1 : 0;
			ChunkMeshes.Add(ChunkCoord, MeshData);
		}
		virtual void RemoveChunk(const FIntVector& ChunkCoord) override { ChunkMeshes.Remove(ChunkCoord); }
		virtual void ClearAllChunks() override { ChunkMeshes.Reset(); }
		virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override {}
		virtual void SetAllChunksVisible(bool bVisible) override {}
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
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

	/**
	 * A transient game world with a configuration, a chunk manager on an actor and the recording renderer. Create the
	 * world, fill Config (or use ConfigureSmallPlanet / ConfigureSmallPlane), StartManager, SetViewer, Settle; Stop at
	 * the end. The defaults stream on the CPU with no collision, scatter or caves.
	 */
	struct FTestChunkWorld
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		AActor* Owner = nullptr;
		UVoxelChunkManager* Manager = nullptr;
		FRecordingMeshRenderer Renderer;

		/** A transient game world and a rooted configuration. */
		void Create(const TCHAR* WorldName)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
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

		void StartManager()
		{
			Owner = World->SpawnActor<AActor>();
			Manager = NewObject<UVoxelChunkManager>(Owner);
			Manager->RegisterComponent();
			Renderer.Initialize(World, Config);
			Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);
		}

		/** Stream around a fixed viewer (the bench's viewer override). */
		void SetViewer(const FVector& Viewer)
		{
			Manager->ResetBenchCounters();
			Manager->SetBenchmarkView(true, Viewer);
		}

		/** Tick until the pipeline has been idle for 90 ticks; false if it does not settle within TimeoutSeconds. */
		bool Settle(double TimeoutSeconds = 120.0)
		{
			int32 IdleTicks = 0;
			const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
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
