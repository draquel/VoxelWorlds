// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapSubsystem.h"
#include "VoxelMap.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelCoordinates.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelCPUNoiseGenerator.h"
#include "IVoxelWorldMode.h"
#include "EngineUtils.h"
#include "Async/Async.h"

// ---------------------------------------------------------------------------
// Key packing (FIntPoint -> uint64)
// ---------------------------------------------------------------------------

uint64 UVoxelMapSubsystem::PackTileKey(FIntPoint Coord)
{
	return (static_cast<uint64>(static_cast<uint32>(Coord.X)) << 32) | static_cast<uint64>(static_cast<uint32>(Coord.Y));
}

FIntPoint UVoxelMapSubsystem::UnpackTileKey(uint64 Key)
{
	return FIntPoint(
		static_cast<int32>(static_cast<uint32>(Key >> 32)),
		static_cast<int32>(static_cast<uint32>(Key & 0xFFFFFFFF))
	);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool UVoxelMapSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Only create in game worlds (PIE + standalone), not editor preview
	if (const UWorld* World = Cast<UWorld>(Outer))
	{
		return World->IsGameWorld();
	}
	return false;
}

void UVoxelMapSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogVoxelMap, Log, TEXT("UVoxelMapSubsystem initialized"));

	// Attempt to resolve chunk manager immediately.
	// It may not be available yet (depends on actor initialization order),
	// so we also resolve lazily in RequestTilesInRadius / OnChunkGenerated.
	ResolveChunkManager();
}

void UVoxelMapSubsystem::Deinitialize()
{
	// Unbind delegates
	if (bDelegatesBound && CachedChunkManagerWeak.IsValid())
	{
		CachedChunkManagerWeak->OnChunkGenerated.RemoveDynamic(this, &UVoxelMapSubsystem::OnChunkGenerated);
		bDelegatesBound = false;
	}

	TileCache.Empty();
	ExploredTiles.Empty();
	PendingTiles.Empty();

	UE_LOG(LogVoxelMap, Log, TEXT("UVoxelMapSubsystem deinitialized"));
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// Resolve Chunk Manager
// ---------------------------------------------------------------------------

bool UVoxelMapSubsystem::ResolveChunkManager()
{
	if (bCacheResolved)
	{
		return true;
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Search for chunk manager
	UVoxelChunkManager* ChunkMgr = nullptr;
	for (TActorIterator<AActor> It(const_cast<UWorld*>(World)); It; ++It)
	{
		ChunkMgr = It->FindComponentByClass<UVoxelChunkManager>();
		if (ChunkMgr && ChunkMgr->IsInitialized())
		{
			break;
		}
		ChunkMgr = nullptr;
	}

	if (!ChunkMgr)
	{
		return false;
	}

	CachedChunkManagerWeak = ChunkMgr;

	// Cache configuration
	const UVoxelWorldConfiguration* Config = ChunkMgr->GetConfiguration();
	if (!Config)
	{
		return false;
	}

	CachedWorldMode = ChunkMgr->GetWorldMode();
	if (!CachedWorldMode)
	{
		return false;
	}

	CachedNoiseParams = Config->NoiseParams;
	CachedChunkSize = Config->ChunkSize;
	CachedVoxelSize = Config->VoxelSize;
	CachedWorldOrigin = Config->WorldOrigin;
	bBiomesEnabled = Config->bEnableBiomes;
	CachedBiomeConfig = Config->BiomeConfiguration;
	bWaterEnabled = Config->bEnableWaterLevel;
	CachedWaterLevel = Config->WaterLevel;
	bCacheResolved = true;

	// Bind to chunk generation events
	if (!bDelegatesBound)
	{
		ChunkMgr->OnChunkGenerated.AddDynamic(this, &UVoxelMapSubsystem::OnChunkGenerated);
		bDelegatesBound = true;
	}

	UE_LOG(LogVoxelMap, Log, TEXT("UVoxelMapSubsystem: Resolved chunk manager. ChunkSize=%d, VoxelSize=%.0f"),
		CachedChunkSize, CachedVoxelSize);

	return true;
}

// ---------------------------------------------------------------------------
// Tile Queries
// ---------------------------------------------------------------------------

const FVoxelMapTile* UVoxelMapSubsystem::GetTile(FIntPoint TileCoord) const
{
	const uint64 Key = PackTileKey(TileCoord);
	const FVoxelMapTile* Found = TileCache.Find(Key);
	if (Found && Found->bIsReady)
	{
		return Found;
	}
	return nullptr;
}

bool UVoxelMapSubsystem::HasTile(FIntPoint TileCoord) const
{
	const uint64 Key = PackTileKey(TileCoord);
	const FVoxelMapTile* Found = TileCache.Find(Key);
	return Found && Found->bIsReady;
}

bool UVoxelMapSubsystem::IsTileExplored(FIntPoint TileCoord) const
{
	return ExploredTiles.Contains(PackTileKey(TileCoord));
}

// ---------------------------------------------------------------------------
// Coordinate Helpers
// ---------------------------------------------------------------------------

FIntPoint UVoxelMapSubsystem::WorldToTileCoord(const FVector& WorldPos) const
{
	const float ChunkWorldSize = CachedChunkSize * CachedVoxelSize;
	if (ChunkWorldSize <= 0.f)
	{
		return FIntPoint(0, 0);
	}

	return FIntPoint(
		FMath::FloorToInt((WorldPos.X - CachedWorldOrigin.X) / ChunkWorldSize),
		FMath::FloorToInt((WorldPos.Y - CachedWorldOrigin.Y) / ChunkWorldSize)
	);
}

FVector UVoxelMapSubsystem::TileCoordToWorld(FIntPoint TileCoord) const
{
	const float ChunkWorldSize = CachedChunkSize * CachedVoxelSize;
	return FVector(
		TileCoord.X * ChunkWorldSize + CachedWorldOrigin.X,
		TileCoord.Y * ChunkWorldSize + CachedWorldOrigin.Y,
		0.0f
	);
}

float UVoxelMapSubsystem::GetTileWorldSize() const
{
	return CachedChunkSize * CachedVoxelSize;
}

// ---------------------------------------------------------------------------
// Exploration
// ---------------------------------------------------------------------------

void UVoxelMapSubsystem::RequestTilesInRadius(const FVector& WorldPos, float Radius)
{
	if (!ResolveChunkManager())
	{
		return;
	}

	const float ChunkWorldSize = CachedChunkSize * CachedVoxelSize;
	if (ChunkWorldSize <= 0.f)
	{
		return;
	}

	const FIntPoint CenterTile = WorldToTileCoord(WorldPos);
	const int32 TileRadius = FMath::CeilToInt(Radius / ChunkWorldSize);

	for (int32 TY = CenterTile.Y - TileRadius; TY <= CenterTile.Y + TileRadius; ++TY)
	{
		for (int32 TX = CenterTile.X - TileRadius; TX <= CenterTile.X + TileRadius; ++TX)
		{
			const FIntPoint TileCoord(TX, TY);
			const uint64 Key = PackTileKey(TileCoord);

			ExploredTiles.Add(Key);

			if (!TileCache.Contains(Key) && !PendingTiles.Contains(Key))
			{
				QueueTileGeneration(TileCoord);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Event-Driven Generation
// ---------------------------------------------------------------------------

void UVoxelMapSubsystem::OnChunkGenerated(FIntVector ChunkCoord)
{
	if (!ResolveChunkManager())
	{
		return;
	}

	const FIntPoint TileCoord(ChunkCoord.X, ChunkCoord.Y);
	const uint64 Key = PackTileKey(TileCoord);

	// Mark as explored (chunks that generate are in the player's vicinity)
	ExploredTiles.Add(Key);

	if (!TileCache.Contains(Key) && !PendingTiles.Contains(Key))
	{
		QueueTileGeneration(TileCoord);
	}
}

// ---------------------------------------------------------------------------
// Async Tile Generation
// ---------------------------------------------------------------------------

void UVoxelMapSubsystem::QueueTileGeneration(FIntPoint TileCoord)
{
	const uint64 Key = PackTileKey(TileCoord);
	PendingTiles.Add(Key);

	// Throttle concurrent tasks
	if (ActiveAsyncTasks.Load() >= MaxConcurrentTileGenTasks)
	{
		// Leave in PendingTiles — will be picked up when a running task completes.
		// For simplicity, we rely on the next RequestTilesInRadius or OnChunkGenerated
		// call to re-check pending tiles. This is acceptable since map tile gen
		// is not latency-critical.
		return;
	}

	ActiveAsyncTasks++;
	GenerateTileAsync(TileCoord);
}

void UVoxelMapSubsystem::GenerateTileAsync(FIntPoint TileCoord)
{
	// Capture all values needed on the background thread by value.
	// CachedWorldMode and CachedNoiseParams are read-only after Initialize().
	const IVoxelWorldMode* WorldMode = CachedWorldMode;
	const FVoxelNoiseParams NoiseParams = CachedNoiseParams;
	const int32 ChunkSize = CachedChunkSize;
	const float VoxelSize = CachedVoxelSize;
	const FVector WorldOrigin = CachedWorldOrigin;
	const bool bUseBiomes = bBiomesEnabled;
	const bool bUseWater = bWaterEnabled;
	const float WaterLevel = CachedWaterLevel;

	// Capture biome config data by value for thread safety.
	// We copy the biome array and noise settings so the background thread
	// never touches the UObject. All biome APIs used below are stateless.
	TArray<FBiomeDefinition> BiomeDefs;
	float BiomeBlendWidth = 0.15f;
	bool bHasBiomeConfig = false;
	TArray<FHeightMaterialRule> HeightRules;
	bool bEnableHeightMaterials = false;

	FVoxelNoiseParams TempNoiseParams;
	TempNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	TempNoiseParams.Octaves = 2;
	TempNoiseParams.Persistence = 0.5f;
	TempNoiseParams.Lacunarity = 2.0f;
	TempNoiseParams.Amplitude = 1.0f;

	FVoxelNoiseParams MoistureNoiseParams;
	MoistureNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	MoistureNoiseParams.Octaves = 2;
	MoistureNoiseParams.Persistence = 0.5f;
	MoistureNoiseParams.Lacunarity = 2.0f;
	MoistureNoiseParams.Amplitude = 1.0f;

	// Continentalness captured params
	bool bUseContinentalness = false;
	FVoxelNoiseParams ContinentalnessNoiseParams;
	ContinentalnessNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	ContinentalnessNoiseParams.Octaves = 2;
	ContinentalnessNoiseParams.Persistence = 0.5f;
	ContinentalnessNoiseParams.Lacunarity = 2.0f;
	ContinentalnessNoiseParams.Amplitude = 1.0f;
	float ContHeightMin = -3000.0f;
	float ContHeightMid = 0.0f;
	float ContHeightMax = 1000.0f;
	float ContScaleMin = 0.2f;
	float ContScaleMax = 1.0f;

	if (bUseBiomes && CachedBiomeConfig && CachedBiomeConfig->IsValid())
	{
		bHasBiomeConfig = true;
		BiomeDefs = CachedBiomeConfig->Biomes;
		BiomeBlendWidth = CachedBiomeConfig->BiomeBlendWidth;
		bEnableHeightMaterials = CachedBiomeConfig->bEnableHeightMaterials;
		if (bEnableHeightMaterials)
		{
			HeightRules = CachedBiomeConfig->HeightMaterialRules;
		}
		TempNoiseParams.Seed = NoiseParams.Seed + CachedBiomeConfig->TemperatureSeedOffset;
		TempNoiseParams.Frequency = CachedBiomeConfig->TemperatureNoiseFrequency;
		MoistureNoiseParams.Seed = NoiseParams.Seed + CachedBiomeConfig->MoistureSeedOffset;
		MoistureNoiseParams.Frequency = CachedBiomeConfig->MoistureNoiseFrequency;

		if (CachedBiomeConfig->bEnableContinentalness)
		{
			bUseContinentalness = true;
			ContinentalnessNoiseParams.Seed = NoiseParams.Seed + CachedBiomeConfig->ContinentalnessSeedOffset;
			ContinentalnessNoiseParams.Frequency = CachedBiomeConfig->ContinentalnessNoiseFrequency;
			ContHeightMin = CachedBiomeConfig->ContinentalnessHeightMin;
			ContHeightMid = CachedBiomeConfig->ContinentalnessHeightMid;
			ContHeightMax = CachedBiomeConfig->ContinentalnessHeightMax;
			ContScaleMin = CachedBiomeConfig->ContinentalnessHeightScaleMin;
			ContScaleMax = CachedBiomeConfig->ContinentalnessHeightScaleMax;
		}
	}
	else if (bUseBiomes)
	{
		// Fallback defaults matching VoxelCPUNoiseGenerator behavior
		TempNoiseParams.Seed = NoiseParams.Seed + 1234;
		TempNoiseParams.Frequency = 0.00005f;
		MoistureNoiseParams.Seed = NoiseParams.Seed + 5678;
		MoistureNoiseParams.Frequency = 0.00007f;
	}

	// Use a weak pointer to safely call back to the subsystem
	TWeakObjectPtr<UVoxelMapSubsystem> WeakThis(this);

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[WeakThis, TileCoord, WorldMode, NoiseParams, ChunkSize, VoxelSize, WorldOrigin,
		 bUseBiomes, bUseWater, WaterLevel,
		 bHasBiomeConfig, BiomeDefs = MoveTemp(BiomeDefs), BiomeBlendWidth,
		 bEnableHeightMaterials, HeightRules = MoveTemp(HeightRules),
		 TempNoiseParams, MoistureNoiseParams,
		 bUseContinentalness, ContinentalnessNoiseParams,
		 ContHeightMin, ContHeightMid, ContHeightMax, ContScaleMin, ContScaleMax]()
	{
		if (!WorldMode)
		{
			return;
		}

		const int32 Resolution = ChunkSize;
		TArray<FColor> PixelData;
		PixelData.SetNumUninitialized(Resolution * Resolution);

		for (int32 PY = 0; PY < Resolution; ++PY)
		{
			for (int32 PX = 0; PX < Resolution; ++PX)
			{
				const float WorldX = (TileCoord.X * ChunkSize + PX) * VoxelSize + WorldOrigin.X;
				const float WorldY = (TileCoord.Y * ChunkSize + PY) * VoxelSize + WorldOrigin.Y;

				float Height = WorldMode->GetTerrainHeightAt(WorldX, WorldY, NoiseParams);

				// Sample continentalness for biome selection and height modulation
				float Continentalness = 0.0f;
				if (bUseContinentalness)
				{
					const FVector ContSamplePos(WorldX, WorldY, 0.0f);
					Continentalness = FVoxelCPUNoiseGenerator::FBM3D(ContSamplePos, ContinentalnessNoiseParams);

					// Modulate terrain height to match CPU generator
					// Piecewise linear height offset
					float HeightOffset;
					if (Continentalness < 0.0f)
					{
						HeightOffset = FMath::Lerp(ContHeightMin, ContHeightMid, Continentalness + 1.0f);
					}
					else
					{
						HeightOffset = FMath::Lerp(ContHeightMid, ContHeightMax, Continentalness);
					}

					// Height scale multiplier
					float ScaleMult = FMath::Lerp(ContScaleMin, ContScaleMax, Continentalness * 0.5f + 0.5f);

					// Reconstruct height with modulated params:
					// Original Height = SeaLevel + BaseHeight + NoiseValue * HeightScale
					// We need:        = SeaLevel + (BaseHeight + HeightOffset) + NoiseValue * (HeightScale * ScaleMult)
					// Difference:      = HeightOffset + NoiseValue * HeightScale * (ScaleMult - 1)
					// Since we don't have NoiseValue directly, approximate by using the
					// offset relative to base: NoiseContribution = Height - (SeaLevel + BaseHeight)
					// This gives: ModifiedHeight = SeaLevel + (BaseHeight + HeightOffset) + NoiseContribution * ScaleMult
					// But WorldMode->GetTerrainHeightAt already computed Height with original params.
					// Compute the noise contribution and re-apply with modulated scale.
					float BaseTerrainHeight = WorldMode->GetTerrainHeightAt(WorldX, WorldY, NoiseParams);
					// The simplest correct approach: WorldMode returns SeaLevel + BaseHeight + noise*HeightScale
					// We want SeaLevel + (BaseHeight + HeightOffset) + noise*(HeightScale*ScaleMult)
					// = BaseTerrainHeight + HeightOffset + noise*HeightScale*(ScaleMult - 1)
					// noise*HeightScale = BaseTerrainHeight - (SeaLevel + BaseHeight)
					// We don't have SeaLevel/BaseHeight separately here. Use the offset only as approximation
					// that's consistent for map rendering (height modulation shifts the whole column).
					Height = BaseTerrainHeight + HeightOffset;
				}

				// Determine surface material using biome system (matches VoxelCPUNoiseGenerator)
				uint8 MaterialID = 0;

				if (bUseBiomes)
				{
					// Sample temperature and moisture noise at this XY position
					const FVector BiomeSamplePos(WorldX, WorldY, 0.0f);
					const float Temperature = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, TempNoiseParams);
					const float Moisture = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, MoistureNoiseParams);

					if (bHasBiomeConfig)
					{
						// Use captured biome definitions for blend selection.
						// Replicate the blend logic locally since we can't call UObject methods
						// from a background thread. Select dominant biome by closest center.
						const FBiomeDefinition* BestBiome = BiomeDefs.Num() > 0 ? &BiomeDefs[0] : nullptr;
						float BestDist = MAX_FLT;
						for (const FBiomeDefinition& Biome : BiomeDefs)
						{
							if (Biome.Contains(Temperature, Moisture, Continentalness))
							{
								const float Dist = Biome.GetDistanceToCenter(Temperature, Moisture);
								if (Dist < BestDist)
								{
									BestDist = Dist;
									BestBiome = &Biome;
								}
							}
						}
						if (BestBiome)
						{
							MaterialID = BestBiome->GetMaterialAtDepth(0.0f);
						}

						// Apply height material rules (snow on peaks, etc.)
						if (bEnableHeightMaterials)
						{
							for (const FHeightMaterialRule& Rule : HeightRules)
							{
								if (Rule.Applies(Height, 0.0f))
								{
									MaterialID = Rule.MaterialID;
									break;
								}
							}
						}
					}
					else
					{
						// Fallback to static biome registry
						FBiomeBlend Blend = FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f);
						MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, 0.0f);
					}
				}
				else
				{
					// Legacy: use world mode's hardcoded material
					MaterialID = WorldMode->GetMaterialAtDepth(
						FVector(WorldX, WorldY, Height), Height, 0.0f);
				}

				FColor Color;

				if (bUseWater && Height < WaterLevel)
				{
					// Submerged terrain — render as water.
					// Deeper water is darker blue for a depth effect.
					const float Depth = WaterLevel - Height;
					const float DepthFactor = FMath::Clamp(1.0f - Depth / 3000.0f, 0.3f, 1.0f);
					Color.R = FMath::Clamp(static_cast<int32>(20 * DepthFactor), 0, 255);
					Color.G = FMath::Clamp(static_cast<int32>(80 * DepthFactor), 0, 255);
					Color.B = FMath::Clamp(static_cast<int32>(180 * DepthFactor), 0, 255);
					Color.A = 255;
				}
				else
				{
					Color = FVoxelMaterialRegistry::GetMaterialColor(MaterialID);

					// Height-based shading — elevation above the reference level
					// (water level if enabled, otherwise 0) drives a gradient from
					// dark at low land to bright at peaks, matching the water depth effect.
					const float LandBase = bUseWater ? WaterLevel : 0.0f;
					const float Elevation = Height - LandBase;
					const float ElevationFactor = FMath::Clamp(Elevation / 4000.0f, 0.0f, 1.0f);
					const float Brightness = FMath::Lerp(0.45f, 1.0f, ElevationFactor);
					Color.R = FMath::Clamp(static_cast<int32>(Color.R * Brightness), 0, 255);
					Color.G = FMath::Clamp(static_cast<int32>(Color.G * Brightness), 0, 255);
					Color.B = FMath::Clamp(static_cast<int32>(Color.B * Brightness), 0, 255);
					Color.A = 255;
				}

				PixelData[PY * Resolution + PX] = Color;
			}
		}

		// Marshal back to game thread
		AsyncTask(ENamedThreads::GameThread, [WeakThis, TileCoord, PixelData = MoveTemp(PixelData), Resolution]() mutable
		{
			UVoxelMapSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			Self->ActiveAsyncTasks--;

			const uint64 Key = PackTileKey(TileCoord);

			FVoxelMapTile Tile;
			Tile.TileCoord = TileCoord;
			Tile.Resolution = Resolution;
			Tile.PixelData = MoveTemp(PixelData);
			Tile.bIsReady = true;

			{
				FScopeLock Lock(&Self->TileMutex);
				Self->TileCache.Add(Key, MoveTemp(Tile));
				Self->PendingTiles.Remove(Key);
			}

			Self->OnMapTileReady.Broadcast(TileCoord);

			// Check if there are more pending tiles to process
			if (Self->ActiveAsyncTasks.Load() < MaxConcurrentTileGenTasks)
			{
				// Find a pending tile that isn't in the cache yet
				for (const uint64 PendingKey : Self->PendingTiles)
				{
					if (!Self->TileCache.Contains(PendingKey))
					{
						const FIntPoint PendingCoord = UnpackTileKey(PendingKey);
						Self->ActiveAsyncTasks++;
						Self->GenerateTileAsync(PendingCoord);
						break;
					}
				}
			}
		});
	});
}
