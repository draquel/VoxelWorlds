// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapSubsystem.h"
#include "VoxelMap.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelWorldTestActor.h"
#include "VoxelCoordinates.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelTerrainConditioning.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "EngineUtils.h"
#include "Async/Async.h"
#include "Misc/Base64.h"
#include "Misc/Compression.h"

namespace
{
	/**
	 * Build a standalone analytic world mode from the configuration — mirrors the construction switch
	 * in UVoxelChunkManager::Initialize (the canonical version; keep in sync when adding world modes).
	 *
	 * The map subsystem owns its OWN instance instead of borrowing the chunk manager's: background
	 * tile tasks capture it by TSharedPtr, and the chunk manager destroys its instance in
	 * EndPlay->Shutdown() while tile tasks can still be running — the PIE-stop use-after-free.
	 *
	 * SetBiomeContext IS called (unlike the pre-map-accuracy version): world modes snapshot the biome
	 * config BY VALUE (FVoxelBiomeSnapshot — plain data, no UObject reference), so the instance stays
	 * safe for tasks that outlive the world's GC purge while GetTerrainHeightAt returns the TRUE
	 * generated surface height (continentalness offset AND height-scale modulation, composed correctly
	 * per mode — the former hand-rolled offset-only reapplication in GenerateTileAsync was the map's
	 * water-placement bug; see Documentation/Research/MAP_ACCURACY_PLAN.md).
	 */
	TSharedPtr<const IVoxelWorldMode> CreateStandaloneWorldMode(const UVoxelWorldConfiguration& Config)
	{
		FWorldModeTerrainParams TerrainParams;
		TerrainParams.SeaLevel = Config.SeaLevel;
		TerrainParams.HeightScale = Config.HeightScale;
		TerrainParams.BaseHeight = Config.BaseHeight;

		TSharedPtr<IVoxelWorldMode> Mode;
		switch (Config.WorldMode)
		{
		case EWorldMode::IslandBowl:
		{
			FIslandBowlParams IslandParams;
			IslandParams.Shape = static_cast<EIslandShape>(Config.IslandShape);
			IslandParams.IslandRadius = Config.IslandRadius;
			IslandParams.SizeY = Config.IslandSizeY;
			IslandParams.FalloffWidth = Config.IslandFalloffWidth;
			IslandParams.FalloffType = static_cast<EIslandFalloffType>(Config.IslandFalloffType);
			IslandParams.CenterX = Config.IslandCenterX;
			IslandParams.CenterY = Config.IslandCenterY;
			IslandParams.EdgeHeight = Config.IslandEdgeHeight;
			IslandParams.bBowlShape = Config.bIslandBowlShape;
			Mode = MakeShared<FIslandBowlWorldMode>(TerrainParams, IslandParams);
			break;
		}
		case EWorldMode::SphericalPlanet:
		{
			const FWorldModeTerrainParams PlanetTerrainParams(0.0f, Config.PlanetHeightScale, Config.BaseHeight);
			FSphericalPlanetParams PlanetParams;
			PlanetParams.PlanetRadius = Config.WorldRadius;
			PlanetParams.MaxTerrainHeight = Config.PlanetMaxTerrainHeight;
			PlanetParams.MaxTerrainDepth = Config.PlanetMaxTerrainDepth;
			PlanetParams.PlanetCenter = Config.WorldOrigin;
			Mode = MakeShared<FSphericalPlanetWorldMode>(PlanetTerrainParams, PlanetParams);
			break;
		}
		case EWorldMode::InfinitePlane:
		default:
			Mode = MakeShared<FInfinitePlaneWorldMode>(TerrainParams);
			break;
		}

		// Value-snapshot the biome config into the mode (game thread; the UObject is not retained).
		// Matches UVoxelChunkManager::Initialize, which sets the context unconditionally.
		Mode->SetBiomeContext(Config.BiomeConfiguration);
		return Mode;
	}
}

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
	// Block new task launches and mark in-flight completions for discard before anything else.
	bShuttingDown = true;

	// Unbind delegates
	if (bDelegatesBound && CachedChunkManagerWeak.IsValid())
	{
		CachedChunkManagerWeak->OnChunkGenerated.RemoveDynamic(this, &UVoxelMapSubsystem::OnChunkGenerated);
		CachedChunkManagerWeak->OnTerrainConditioningChanged.Remove(ConditioningChangedHandle);
		ConditioningChangedHandle.Reset();
		bDelegatesBound = false;
	}

	const int32 InFlight = ActiveAsyncTasks.Load();
	if (InFlight > 0)
	{
		// In-flight background tasks own their world mode (TSharedPtr capture) and every other input
		// by value, so they finish safely after this point; their game-thread completions see
		// bShuttingDown and discard.
		UE_LOG(LogVoxelMap, Log, TEXT("UVoxelMapSubsystem deinitializing with %d async tile task(s) in flight — completions will be discarded"),
			InFlight);
	}

	// Drop our reference to the standalone world mode; any running task holds its own.
	CachedWorldMode.Reset();

	TileCache.Empty();
	ExploredTiles.Empty();
	PendingTiles.Empty();
	InFlightTiles.Empty();

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

	// Build our own analytic world mode from the configuration (see CreateStandaloneWorldMode for
	// why we don't borrow the chunk manager's instance).
	CachedWorldMode = CreateStandaloneWorldMode(*Config);
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

	// Map palette: prefer the material atlas' baked per-material colors (average albedo of the
	// actual terrain textures) over the material registry's hardcoded debug palette, so map
	// colors read like the world does. Falls back per-material inside GetMapColor.
	{
		const UVoxelMaterialAtlas* Atlas = nullptr;
		if (const AVoxelWorldTestActor* WorldActor = Cast<AVoxelWorldTestActor>(ChunkMgr->GetOwner()))
		{
			Atlas = WorldActor->MaterialAtlas;
		}

		if (Atlas)
		{
			Atlas->BuildMapPalette(CachedMaterialPalette);
		}
		else
		{
			// No atlas reachable (custom world actor): registry palette for every material.
			CachedMaterialPalette.SetNumUninitialized(256);
			for (int32 i = 0; i < 256; ++i)
			{
				CachedMaterialPalette[i] = FVoxelMaterialRegistry::GetMaterialColor(static_cast<uint8>(i));
			}
		}
	}

	// Elevation/depth shading ranges from the world mode's own height bounds (the generation
	// authority) instead of hardcoded constants, so the gradient spans the terrain this config
	// actually produces.
	CachedWorldMode->GetTerrainHeightBounds(CachedTerrainMinHeight, CachedTerrainMaxHeight);

	// Modes without real bounds (IVoxelWorldMode's default is a deliberately huge no-cull range,
	// e.g. SphericalPlanet) would flatten the gradient to a constant. Fall back to the config's own
	// noise envelope so shading still spans something meaningful.
	if (!FMath::IsFinite(CachedTerrainMinHeight) || !FMath::IsFinite(CachedTerrainMaxHeight) ||
		(CachedTerrainMaxHeight - CachedTerrainMinHeight) > 1.0e6f)
	{
		const float Center = Config->SeaLevel + Config->BaseHeight;
		CachedTerrainMinHeight = Center - Config->HeightScale;
		CachedTerrainMaxHeight = Center + Config->HeightScale;
	}

	bCacheResolved = true;

	// Bind to chunk generation events
	if (!bDelegatesBound)
	{
		ChunkMgr->OnChunkGenerated.AddDynamic(this, &UVoxelMapSubsystem::OnChunkGenerated);
		ConditioningChangedHandle = ChunkMgr->OnTerrainConditioningChanged.AddUObject(
			this, &UVoxelMapSubsystem::HandleTerrainConditioningChanged);
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

FString UVoxelMapSubsystem::EncodeTileCoords(const TArray<FIntPoint>& Tiles)
{
	if (Tiles.Num() == 0)
	{
		return FString();
	}
	TArray<int32> Raw;
	Raw.Reserve(Tiles.Num() * 2);
	for (const FIntPoint& Tile : Tiles)
	{
		Raw.Add(Tile.X);
		Raw.Add(Tile.Y);
	}
	const int32 RawBytes = Raw.Num() * sizeof(int32);
	TArray<uint8> Compressed;
	int32 CompressedBytes = FCompression::CompressMemoryBound(NAME_Zlib, RawBytes);
	Compressed.SetNumUninitialized(CompressedBytes);
	if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), CompressedBytes, Raw.GetData(), RawBytes))
	{
		UE_LOG(LogVoxelMap, Warning, TEXT("EncodeTileCoords: compressing %d tiles failed."), Tiles.Num());
		return FString();
	}
	Compressed.SetNum(CompressedBytes);
	return FBase64::Encode(Compressed);
}

bool UVoxelMapSubsystem::DecodeTileCoords(const FString& Encoded, int32 Count, TArray<FIntPoint>& OutTiles)
{
	OutTiles.Reset();
	if (Encoded.IsEmpty() || Count <= 0)
	{
		return Encoded.IsEmpty() && Count <= 0;
	}
	TArray<uint8> Compressed;
	if (!FBase64::Decode(Encoded, Compressed))
	{
		return false;
	}
	TArray<int32> Raw;
	Raw.SetNumUninitialized(Count * 2);
	if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), Raw.Num() * sizeof(int32), Compressed.GetData(), Compressed.Num()))
	{
		return false;
	}
	OutTiles.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		OutTiles.Add(FIntPoint(Raw[i * 2], Raw[i * 2 + 1]));
	}
	return true;
}

FString UVoxelMapSubsystem::ExportExploredTiles(int32& OutCount) const
{
	TArray<FIntPoint> Tiles;
	Tiles.Reserve(ExploredTiles.Num());
	for (const uint64 Key : ExploredTiles)
	{
		Tiles.Add(UnpackTileKey(Key));
	}
	// Row-major order: neighbouring tiles become near-identical byte runs, which is what makes the blob small.
	Tiles.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
	FString Encoded = EncodeTileCoords(Tiles);
	OutCount = Encoded.IsEmpty() ? 0 : Tiles.Num();
	return Encoded;
}

int32 UVoxelMapSubsystem::ImportExploredTiles(const FString& Encoded, int32 Count)
{
	TArray<FIntPoint> Tiles;
	if (!DecodeTileCoords(Encoded, Count, Tiles))
	{
		UE_LOG(LogVoxelMap, Warning, TEXT("ImportExploredTiles: malformed explored-tile data (%d tiles claimed); map exploration not restored."), Count);
		return 0;
	}
	for (const FIntPoint& Tile : Tiles)
	{
		ExploredTiles.Add(PackTileKey(Tile));
	}

	// Explored tiles draw from the tile cache: queue the images (four run at a time; the rest wait in PendingTiles).
	if (ResolveChunkManager())
	{
		for (const FIntPoint& Tile : Tiles)
		{
			const uint64 Key = PackTileKey(Tile);
			if (!TileCache.Contains(Key) && !PendingTiles.Contains(Key))
			{
				QueueTileGeneration(Tile);
			}
		}
	}
	else
	{
		bGenerateExploredWhenReady = true;
	}

	UE_LOG(LogVoxelMap, Log, TEXT("Restored %d explored map tile(s) (%d explored now)."), Tiles.Num(), ExploredTiles.Num());
	return Tiles.Num();
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

	// Exploration restored before the map could resolve its world: queue those images now.
	if (bGenerateExploredWhenReady)
	{
		bGenerateExploredWhenReady = false;
		for (const uint64 ExploredKey : ExploredTiles)
		{
			if (!TileCache.Contains(ExploredKey) && !PendingTiles.Contains(ExploredKey))
			{
				QueueTileGeneration(UnpackTileKey(ExploredKey));
			}
		}
	}

	// Mark as explored (chunks that generate are in the player's vicinity)
	ExploredTiles.Add(Key);

	if (!TileCache.Contains(Key) && !PendingTiles.Contains(Key))
	{
		QueueTileGeneration(TileCoord);
	}
}

void UVoxelMapSubsystem::HandleTerrainConditioningChanged()
{
	if (bShuttingDown)
	{
		return;
	}
	// Tiles drawn before the POI conditioner registered show the unflattened terrain (QA W-05): drop
	// them, let running tasks discard their now-stale result, and redraw everything explored.
	++TileContentGeneration;
	const int32 Dropped = TileCache.Num();
	{
		FScopeLock Lock(&TileMutex);
		TileCache.Empty();
	}
	for (const uint64 Key : ExploredTiles)
	{
		if (!PendingTiles.Contains(Key))
		{
			QueueTileGeneration(UnpackTileKey(Key));
		}
	}
	UE_LOG(LogVoxelMap, Log, TEXT("Terrain conditioning changed: rebuilding %d map tile(s) (%d cached dropped)."),
		ExploredTiles.Num(), Dropped);
}

// ---------------------------------------------------------------------------
// Async Tile Generation
// ---------------------------------------------------------------------------

void UVoxelMapSubsystem::QueueTileGeneration(FIntPoint TileCoord)
{
	if (bShuttingDown)
	{
		return;
	}

	const uint64 Key = PackTileKey(TileCoord);
	PendingTiles.Add(Key);
	if (InFlightTiles.Contains(Key))
	{
		return;
	}

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
	// Capture all values needed on the background thread by value. The world mode is OUR standalone
	// instance shared into the task: the task keeps it alive for however long it runs, so PIE
	// teardown order cannot invalidate it. (Capturing the chunk manager's raw pointer here is what
	// crashed on PIE stop — EndPlay->Shutdown() freed it under running tasks.)
	TSharedPtr<const IVoxelWorldMode> WorldMode = CachedWorldMode;
	if (!WorldMode)
	{
		// Caller already incremented the in-flight counter; undo it and drop the request cleanly.
		ActiveAsyncTasks--;
		PendingTiles.Remove(PackTileKey(TileCoord));
		return;
	}
	InFlightTiles.Add(PackTileKey(TileCoord));

	const FVoxelNoiseParams NoiseParams = CachedNoiseParams;
	const int32 ChunkSize = CachedChunkSize;
	const float VoxelSize = CachedVoxelSize;
	const FVector WorldOrigin = CachedWorldOrigin;
	const bool bUseBiomes = bBiomesEnabled;
	const bool bUseWater = bWaterEnabled;
	const float WaterLevel = CachedWaterLevel;

	// Atlas-derived map palette + config-derived shading range (both plain data, captured by value).
	TArray<FColor> MaterialPalette = CachedMaterialPalette;
	const float TerrainMinHeight = CachedTerrainMinHeight;
	const float TerrainMaxHeight = CachedTerrainMaxHeight;

	// Value snapshot of the biome configuration — the background thread never touches the UObject.
	// Carries everything the shared surface-material pipeline needs (biome defs, blend width,
	// underwater + priority-sorted height rules, climate noise settings).
	const FVoxelBiomeSnapshot BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(CachedBiomeConfig);

	// Fallback climate noise for "biomes on, but no valid config" — matches the CPU generator's
	// hardcoded fallback (static registry biomes with default temperature/moisture fields).
	FVoxelNoiseParams FallbackTempNoiseParams;
	FallbackTempNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	FallbackTempNoiseParams.Octaves = 2;
	FallbackTempNoiseParams.Persistence = 0.5f;
	FallbackTempNoiseParams.Lacunarity = 2.0f;
	FallbackTempNoiseParams.Amplitude = 1.0f;
	FallbackTempNoiseParams.Seed = NoiseParams.Seed + 1234;
	FallbackTempNoiseParams.Frequency = 0.00005f;

	FVoxelNoiseParams FallbackMoistureNoiseParams = FallbackTempNoiseParams;
	FallbackMoistureNoiseParams.Seed = NoiseParams.Seed + 5678;
	FallbackMoistureNoiseParams.Frequency = 0.00007f;

	// Terrain conditioning over the tile and its 1-voxel apron (POI pads, campsites): generation layers these
	// onto the base height, so the map must too (QA W-05). Gathered here on the game thread; plain data.
	TArray<FVoxelConditioningZone> Zones;
	if (UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get())
	{
		const FVector2D TileMin(
			(TileCoord.X * ChunkSize - 1) * VoxelSize + WorldOrigin.X,
			(TileCoord.Y * ChunkSize - 1) * VoxelSize + WorldOrigin.Y);
		const FVector2D TileMax = TileMin + FVector2D((ChunkSize + 2) * VoxelSize);
		ChunkMgr->GatherConditioningZones(FBox2D(TileMin, TileMax), Zones);
	}
	const uint32 Generation = TileContentGeneration;

	// Use a weak pointer to safely call back to the subsystem
	TWeakObjectPtr<UVoxelMapSubsystem> WeakThis(this);

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[WeakThis, TileCoord, WorldMode, NoiseParams, ChunkSize, VoxelSize, WorldOrigin,
		 bUseBiomes, bUseWater, WaterLevel, BiomeSnapshot,
		 FallbackTempNoiseParams, FallbackMoistureNoiseParams,
		 MaterialPalette = MoveTemp(MaterialPalette), TerrainMinHeight, TerrainMaxHeight,
		 Zones = MoveTemp(Zones), Generation]()
	{
		// WorldMode is a TSharedPtr capture, validated non-null before launch — this task co-owns
		// the instance, so it stays valid even if the subsystem and world are torn down while we run.
		const int32 Resolution = ChunkSize;
		TArray<FColor> PixelData;
		PixelData.SetNumUninitialized(Resolution * Resolution);

		// --- Pass 1: height grid, with a 1-pixel apron on every side ---
		// The apron lets the hillshade gradient use real neighbor heights at the tile border, so
		// shading is continuous across tile seams instead of flattening at the edges.
		const int32 GridSize = Resolution + 2;
		TArray<float> Heights;
		Heights.SetNumUninitialized(GridSize * GridSize);

		for (int32 GY = 0; GY < GridSize; ++GY)
		{
			for (int32 GX = 0; GX < GridSize; ++GX)
			{
				// Grid index (GX,GY) maps to pixel (GX-1, GY-1)
				const float WorldX = (TileCoord.X * ChunkSize + GX - 1) * VoxelSize + WorldOrigin.X;
				const float WorldY = (TileCoord.Y * ChunkSize + GY - 1) * VoxelSize + WorldOrigin.Y;

				// TRUE generated surface height: the standalone mode carries a value-captured biome
				// snapshot, so this applies the same continentalness modulation (offset + height-scale)
				// and per-mode composition (e.g. IslandBowl falloff) as chunk generation.
				float Height = WorldMode->GetTerrainHeightAt(WorldX, WorldY, NoiseParams);
				if (Zones.Num() > 0)
				{
					Height = FVoxelTerrainConditioning::ApplyToHeight(WorldX, WorldY, Height, Zones);
				}
				Heights[GY * GridSize + GX] = Height;
			}
		}

		// Shading ranges derived from the world mode's real height bounds (see ResolveChunkManager).
		// Land spans [reference .. max], water depth spans [water level .. min]; both guard against
		// degenerate configs where the bounds collapse.
		const float LandBase = bUseWater ? WaterLevel : TerrainMinHeight;
		const float LandSpan = FMath::Max(TerrainMaxHeight - LandBase, 1.0f);
		const float DepthSpan = FMath::Max(LandBase - TerrainMinHeight, 1.0f);

		// Hillshade light: classic cartographic NW key light at ~45 degrees altitude.
		const FVector LightDir = FVector(-0.707f, -0.707f, 1.0f).GetSafeNormal();

		// --- Pass 2: color ---
		for (int32 PY = 0; PY < Resolution; ++PY)
		{
			for (int32 PX = 0; PX < Resolution; ++PX)
			{
				const float WorldX = (TileCoord.X * ChunkSize + PX) * VoxelSize + WorldOrigin.X;
				const float WorldY = (TileCoord.Y * ChunkSize + PY) * VoxelSize + WorldOrigin.Y;

				const int32 GX = PX + 1;
				const int32 GY = PY + 1;
				const float Height = Heights[GY * GridSize + GX];

				// Surface material — the SAME shared pipeline as chunk generation, tree placement and
				// PCG (temperature/moisture/continentalness noise -> tiered biome blend -> blended
				// material -> priority-sorted height rules), via FVoxelSurfaceQuery + the snapshot.
				uint8 MaterialID = 0;

				if (bUseBiomes && BiomeSnapshot.bIsValid)
				{
					uint8 BiomeID = 0;
					FVoxelSurfaceQuery::QuerySurfaceConditions(
						WorldX, WorldY, Height, VoxelSize,
						BiomeSnapshot, NoiseParams.Seed, bUseWater, WaterLevel,
						MaterialID, BiomeID);
				}
				else if (bUseBiomes)
				{
					// No valid biome config: static registry fallback (matches the CPU generator).
					const FVector BiomeSamplePos(WorldX, WorldY, 0.0f);
					const float Temperature = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, FallbackTempNoiseParams);
					const float Moisture = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, FallbackMoistureNoiseParams);
					FBiomeBlend Blend = FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f);
					MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, 0.0f);
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
					// Submerged terrain — render as water, deeper = darker blue. Depth is
					// normalized against the config's own terrain floor rather than a fixed
					// constant, so shallow-water configs still get a readable gradient.
					const float Depth = WaterLevel - Height;
					const float DepthFactor = FMath::Lerp(1.0f, 0.3f, FMath::Clamp(Depth / DepthSpan, 0.0f, 1.0f));
					Color.R = FMath::Clamp(static_cast<int32>(20 * DepthFactor), 0, 255);
					Color.G = FMath::Clamp(static_cast<int32>(80 * DepthFactor), 0, 255);
					Color.B = FMath::Clamp(static_cast<int32>(180 * DepthFactor), 0, 255);
					Color.A = 255;
				}
				else
				{
					// Atlas-baked average albedo of the real terrain texture (registry palette
					// where a material has no baked color).
					Color = MaterialPalette[MaterialID];

					// Elevation tint — subtle darkening of low land toward the reference level,
					// spanning the config's actual height range.
					const float ElevationFactor = FMath::Clamp((Height - LandBase) / LandSpan, 0.0f, 1.0f);
					const float Elevation = FMath::Lerp(0.75f, 1.0f, ElevationFactor);

					// Hillshade — lambert N.L from the height gradient (central differences over the
					// apron grid). This is what makes ridges and valleys legible; the elevation tint
					// alone reads flat because terrain at one altitude is one flat color.
					const float HL = Heights[GY * GridSize + (GX - 1)];
					const float HR = Heights[GY * GridSize + (GX + 1)];
					const float HD = Heights[(GY - 1) * GridSize + GX];
					const float HU = Heights[(GY + 1) * GridSize + GX];

					const float DX = (HR - HL) / (2.0f * VoxelSize);
					const float DY = (HU - HD) / (2.0f * VoxelSize);
					const FVector Normal = FVector(-DX, -DY, 1.0f).GetSafeNormal();

					// Remap N.L into a gentle range: flat ground stays near its base color, slopes
					// facing the light brighten and away-facing slopes fall into shadow.
					const float NdotL = FMath::Clamp(static_cast<float>(FVector::DotProduct(Normal, LightDir)), 0.0f, 1.0f);
					const float Shade = FMath::Lerp(0.55f, 1.25f, NdotL);

					const float Brightness = Elevation * Shade;
					Color.R = FMath::Clamp(static_cast<int32>(Color.R * Brightness), 0, 255);
					Color.G = FMath::Clamp(static_cast<int32>(Color.G * Brightness), 0, 255);
					Color.B = FMath::Clamp(static_cast<int32>(Color.B * Brightness), 0, 255);
					Color.A = 255;
				}

				PixelData[PY * Resolution + PX] = Color;
			}
		}

		// Marshal back to game thread
		AsyncTask(ENamedThreads::GameThread, [WeakThis, TileCoord, PixelData = MoveTemp(PixelData), Resolution, Generation]() mutable
		{
			UVoxelMapSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			Self->ActiveAsyncTasks--;
			Self->InFlightTiles.Remove(PackTileKey(TileCoord));

			if (Self->bShuttingDown)
			{
				// Deinitialize() already ran (PIE stop / world teardown): don't resurrect cache
				// state, don't broadcast into a dying world, and don't start new tasks.
				UE_LOG(LogVoxelMap, Verbose, TEXT("Discarded tile (%d,%d) completion after shutdown"),
					TileCoord.X, TileCoord.Y);
				return;
			}

			const uint64 Key = PackTileKey(TileCoord);

			// Conditioning changed while this tile was drawing: the image is stale. The tile stays pending, so
			// the drain below redraws it.
			if (Generation != Self->TileContentGeneration)
			{
				Self->PendingTiles.Add(Key);
			}
			else
			{
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
			}

			// Check if there are more pending tiles to process
			if (Self->ActiveAsyncTasks.Load() < MaxConcurrentTileGenTasks)
			{
				// Find a pending tile that isn't in the cache yet
				for (const uint64 PendingKey : Self->PendingTiles)
				{
					// Skip tiles already running: with a large backlog (restored exploration) the drain would
					// otherwise keep restarting the same in-flight tile.
					if (!Self->TileCache.Contains(PendingKey) && !Self->InFlightTiles.Contains(PendingKey))
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
