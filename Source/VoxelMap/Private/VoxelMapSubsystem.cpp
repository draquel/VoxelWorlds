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
#include "VoxelCoordinates.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelTerrainConditioning.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelEditManager.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "VoxelPlanetFarField.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/Base64.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "VoxelResidencyStats.h"

static TAutoConsoleVariable<float> CVarMapEditRefreshSeconds(
	TEXT("voxel.Map.EditRefreshSeconds"),
	0.5f,
	TEXT("Seconds the map coalesces chunk edits before rebuilding the edited tiles' overlays. Water propagation edits ")
	TEXT("every frame, so this bounds the rebuild rate."),
	ECVF_Default);

namespace
{
	void ApplyMapBudgetsToAll(IConsoleVariable*)
	{
		for (TObjectIterator<UVoxelMapSubsystem> It; It; ++It)
		{
			It->ApplyTileBudgets();
		}
	}
}

// Map tile images under a fixed budget (world modes review WM-E2, KI15): 64 x 64 detail images are 16 KB each, and a
// fully explored 10 km island has about 34,500 tiles (0.55 GB) when every image is kept.
static TAutoConsoleVariable<float> CVarMapDetailBudgetMB(
	TEXT("voxel.Map.DetailBudgetMB"),
	32.0f,
	TEXT("MB of full-resolution map tile images kept, least recently used dropped first (their tiles keep the overview ")
	TEXT("and are drawn again in detail when the minimap or a zoomed-in world map asks). 0 = unlimited: every tile keeps ")
	TEXT("its detail image, as before WM-E2. 32 MB holds 2,048 ")
	TEXT("tiles of 64 x 64: the minimap's area and a world map zoomed in twice."),
	FConsoleVariableDelegate::CreateStatic(&ApplyMapBudgetsToAll),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarMapOverviewBudgetMB(
	TEXT("voxel.Map.OverviewBudgetMB"),
	48.0f,
	TEXT("MB of overview map tile images (a quarter of the resolution a side) kept for tiles without their detail image, ")
	TEXT("least recently used dropped first (the tile stays explored and is drawn again when looked at). 0 = unlimited. ")
	TEXT("48 MB holds 49,152 tiles of 16 x 16: an island about 12 km across, fully explored."),
	FConsoleVariableDelegate::CreateStatic(&ApplyMapBudgetsToAll),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarMapExploreAllMaxTiles(
	TEXT("voxel.Map.ExploreAllMaxTiles"),
	1000000,
	TEXT("voxel.Map.ExploreAll marks at most this many tiles explored, nearest the pawn first (spherical Phase 6, WM-BP11: a ")
	TEXT("planet's atlas is 6 N^2 tiles: 40,000 on a 2.5 km planet, 16 million at 50 km, 6.4 billion at 1,000 km; every world ")
	TEXT("mode since world modes review WM-MP9: a plane's are unbounded)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarMapEditRefreshMaxTiles(
	TEXT("voxel.Map.EditRefreshMaxTiles"),
	8,
	TEXT("Max edited tiles whose overlay rebuild one coalesced refresh starts; the rest wait for the next refresh. ")
	TEXT("Each rebuild shares the chunks' edit-merged snapshots with the seam pipeline."),
	ECVF_Default);

namespace
{
	/** The PIE / Game world a console command should act on. */
	UWorld* FindPlayWorld(UWorld* World)
	{
		if (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game))
		{
			return World;
		}
		if (GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
				{
					return Ctx.World();
				}
			}
		}
		return nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs GVoxelMapRedrawCmd(
		TEXT("voxel.Map.Redraw"),
		TEXT("Redraw every explored map tile (current images stay until the new ones land). voxel.Map.Redraw 1 also ")
		TEXT("rebuilds the edit overlay of every edited tile from loaded chunks."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UWorld* Target = FindPlayWorld(World);
			UVoxelMapSubsystem* Map = Target ? Target->GetSubsystem<UVoxelMapSubsystem>() : nullptr;
			if (!Map)
			{
				UE_LOG(LogVoxelMap, Warning, TEXT("voxel.Map.Redraw: no PIE/Game world with a map subsystem (start PIE first)"));
				return;
			}
			Map->RedrawAllTiles(Args.Num() > 0 && FCString::Atoi(*Args[0]) != 0);
		}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelMapStatsCmd(
		TEXT("voxel.Map.Stats"),
		TEXT("Log the map's tile, queue and edit-overlay state."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UWorld* Target = FindPlayWorld(World);
			const UVoxelMapSubsystem* Map = Target ? Target->GetSubsystem<UVoxelMapSubsystem>() : nullptr;
			if (!Map)
			{
				UE_LOG(LogVoxelMap, Warning, TEXT("voxel.Map.Stats: no PIE/Game world with a map subsystem (start PIE first)"));
				return;
			}
			UE_LOG(LogVoxelMap, Log, TEXT("voxel.Map.Stats: %s"), *Map->GetDebugStats());
		}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelMapExploreAllCmd(
		TEXT("voxel.Map.ExploreAll"),
		TEXT("Mark every map tile of a bounded world explored (as a fully explored save restores it) and queue their images. ")
		TEXT("Args: [radius=200000] (unbounded worlds: the tiles within this radius of the pawn)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UWorld* Target = FindPlayWorld(World);
			UVoxelMapSubsystem* Map = Target ? Target->GetSubsystem<UVoxelMapSubsystem>() : nullptr;
			const APlayerController* PC = Target ? Target->GetFirstPlayerController() : nullptr;
			const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Map)
			{
				UE_LOG(LogVoxelMap, Warning, TEXT("voxel.Map.ExploreAll: no PIE/Game world with a map subsystem (start PIE first)"));
				return;
			}
			const float Radius = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 200000.0f;
			const int64 Count = Map->ExploreAllTiles(Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector, Radius);
			UE_LOG(LogVoxelMap, Log, TEXT("voxel.Map.ExploreAll: %lld tile(s) explored; %s"), Count, *Map->GetDebugStats());
		}));

	FAutoConsoleCommandWithWorldAndArgs GVoxelMapCaptureCmd(
		TEXT("voxel.Map.Capture"),
		TEXT("Write the explored map tiles around the player to Saved/Screenshots/VoxelMap/<name>.png (north up, centre ")
		TEXT("marked). Args: [radiusTiles=6] [name=timestamp] [highlightOverlays=0] [centreX centreY] (default: the pawn)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UWorld* Target = FindPlayWorld(World);
			const UVoxelMapSubsystem* Map = Target ? Target->GetSubsystem<UVoxelMapSubsystem>() : nullptr;
			const APlayerController* PC = Target ? Target->GetFirstPlayerController() : nullptr;
			const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Map || !Pawn)
			{
				UE_LOG(LogVoxelMap, Warning, TEXT("voxel.Map.Capture: no PIE/Game world with a map subsystem and a pawn (start PIE first)"));
				return;
			}
			const int32 Radius = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 6;
			const FString Name = Args.Num() > 1 ? Args[1] : FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
			const bool bHighlight = Args.Num() > 2 && FCString::Atoi(*Args[2]) != 0;
			const FVector Centre = Args.Num() > 4
				? FVector(FCString::Atod(*Args[3]), FCString::Atod(*Args[4]), 0.0)
				: Pawn->GetActorLocation();
			const FString Path = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir() / TEXT("Screenshots/VoxelMap") / (Name + TEXT(".png")));
			const bool bOk = Map->CaptureMapImage(Centre, Radius, Path, bHighlight);
			UE_LOG(LogVoxelMap, Log, TEXT("voxel.Map.Capture: %s %s (radius %d, centre %s, pawn %s)"),
				bOk ? TEXT("wrote") : TEXT("FAILED to write"), *Path, Radius, *Centre.ToCompactString(),
				*Pawn->GetActorLocation().ToCompactString());
		}));

	/**
	 * A standalone analytic world mode for the configuration, from the factory every world-mode consumer uses
	 * (FVoxelWorldModeFactory, WM-A1), so the map's island and planet match generation.
	 *
	 * The map subsystem owns its OWN instance instead of borrowing the chunk manager's: background
	 * tile tasks capture it by TSharedPtr, and the chunk manager destroys its instance in
	 * EndPlay->Shutdown() while tile tasks can still be running — the PIE-stop use-after-free.
	 *
	 * The factory sets the biome context: world modes snapshot the biome config BY VALUE
	 * (FVoxelBiomeSnapshot — plain data, no UObject reference), so the instance stays safe for tasks
	 * that outlive the world's GC purge while GetTerrainHeightAt returns the TRUE generated surface
	 * height (continentalness offset AND height-scale modulation, composed correctly per mode; see
	 * Documentation/Research/MAP_ACCURACY_PLAN.md).
	 */
	TSharedPtr<const IVoxelWorldMode> CreateStandaloneWorldMode(const UVoxelWorldConfiguration& Config)
	{
		return FVoxelWorldModeFactory::Create(Config);
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
	if (UVoxelEditManager* EditManager = CachedEditManagerWeak.Get())
	{
		EditManager->OnChunkEdited.Remove(ChunkEditedHandle);
	}
	ChunkEditedHandle.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(OverlayRefreshTimer);
	}

	const int32 InFlight = TileQueue.NumDrawing();
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
	CachedWorldBounds = FVoxelWorldBounds();

	TileStore.Reset();
	ExploredTiles.Empty();
	DetailWanted.Empty();
	TileQueue.Reset();
	EditOverlays.Empty();
	TileEditedChunkZ.Empty();
	OverlayDirtyTiles.Empty();
	OverlayAwaitingTiles.Empty();
	OverlayRebuildTiles.Empty();
	TileAnalyticRange.Empty();
	PlanetTileEditedChunks.Empty();

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

	UVoxelChunkManager* ChunkMgr = UVoxelChunkManager::Find(World);

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
		const UVoxelMaterialAtlas* Atlas = ChunkMgr->GetMaterialAtlas();
		if (Atlas)
		{
			Atlas->BuildMapPalette(CachedMaterialPalette);
		}
		else
		{
			// No atlas: registry palette for every material.
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

	// The world's edge: exploration stops there (WM-I2). Tiles a save restored before the world resolved are
	// checked now.
	CachedWorldBounds = CachedWorldMode->GetWorldBounds();

	// A planet's tiles are cells of the cube-sphere grid, about a chunk across (spherical Phase 4, WM-SY8).
	if (CachedWorldMode->GetWorldModeType() == EWorldMode::SphericalPlanet)
	{
		const FSphericalPlanetParams& Planet = static_cast<const FSphericalPlanetWorldMode&>(*CachedWorldMode).GetPlanetParams();
		CachedPlanetCenter = Planet.PlanetCenter;
		CachedPlanetRadius = Planet.PlanetRadius;
		PlanetGrid = FVoxelCubeSphereGrid(FVoxelCubeSphereGrid::CellsPerEdgeFor(CachedPlanetRadius, static_cast<double>(CachedChunkSize) * CachedVoxelSize));
		CachedSea = FVoxelSeaSurface::FromConfig(*Config);
		CachedWorldMode->GetAltitudeBounds(CachedAltitudeMin, CachedAltitudeMax);
	}

	bCacheResolved = true;
	ApplyTileBudgets();
	PruneExploredOutsideWorld();

	// Bind to chunk generation events
	if (!bDelegatesBound)
	{
		ChunkMgr->OnChunkGenerated.AddDynamic(this, &UVoxelMapSubsystem::OnChunkGenerated);
		ConditioningChangedHandle = ChunkMgr->OnTerrainConditioningChanged.AddUObject(
			this, &UVoxelMapSubsystem::HandleTerrainConditioningChanged);
		bDelegatesBound = true;

		// Edits drive the map's edit overlays (R22). Edits made before the map resolved (POI stamps at world start)
		// are seeded from the edit layers and rebuilt as their chunks load.
		if (UVoxelEditManager* EditManager = ChunkMgr->GetEditManager())
		{
			CachedEditManagerWeak = EditManager;
			ChunkEditedHandle = EditManager->OnChunkEdited.AddUObject(this, &UVoxelMapSubsystem::HandleChunkEdited);
			TArray<FIntVector> EditedChunks;
			EditManager->GetEditedChunkCoords(EditedChunks);
			TArray<FIntPoint> ChunkTiles;
			for (const FIntVector& Coord : EditedChunks)
			{
				RecordChunkEdits(Coord);
				ChunkTiles.Reset();
				GetChunkTiles(Coord, ChunkTiles);
				for (const FIntPoint& Tile : ChunkTiles)
				{
					MarkOverlayDirty(PackTileKey(Tile));
				}
			}
		}
	}

	UE_LOG(LogVoxelMap, Log, TEXT("UVoxelMapSubsystem: Resolved chunk manager. ChunkSize=%d, VoxelSize=%.0f%s"),
		CachedChunkSize, CachedVoxelSize, PlanetGrid.IsValid()
			? *FString::Printf(TEXT(", planet: %d tiles a face edge (%lld tiles, about %.0f uu each)"), PlanetGrid.GetCellsPerEdge(),
				PlanetGrid.GetNumCells(), GetTileWorldSize())
			: TEXT(""));

	return true;
}

// ---------------------------------------------------------------------------
// Tile Queries
// ---------------------------------------------------------------------------

const FVoxelMapTile* UVoxelMapSubsystem::GetTile(FIntPoint TileCoord) const
{
	const FVoxelMapTile* Found = TileStore.Find(PackTileKey(TileCoord), /*bTouch*/ true);
	return (Found && Found->bIsReady) ? Found : nullptr;
}

bool UVoxelMapSubsystem::HasTile(FIntPoint TileCoord) const
{
	return TileStore.Contains(PackTileKey(TileCoord));
}

bool UVoxelMapSubsystem::HasTileDetail(FIntPoint TileCoord) const
{
	return TileStore.HasDetail(PackTileKey(TileCoord));
}

bool UVoxelMapSubsystem::RequestTileImage(FIntPoint TileCoord, bool bDetail)
{
	if (bShuttingDown || !ResolveChunkManager())
	{
		return false;
	}
	const uint64 Key = PackTileKey(TileCoord);
	if (!ExploredTiles.Contains(Key))
	{
		return false;
	}
	if (bDetail ? TileStore.HasDetail(Key) : TileStore.Contains(Key))
	{
		return false;
	}
	if (bDetail)
	{
		DetailWanted.Add(Key);
	}
	if (!TileQueue.IsQueued(Key))
	{
		QueueTileGeneration(TileCoord);
	}
	return true;
}

void UVoxelMapSubsystem::ApplyTileBudgets()
{
	constexpr double MB = 1024.0 * 1024.0;
	TileStore.SetBudgets(
		static_cast<int64>(FMath::Max(0.0f, CVarMapDetailBudgetMB.GetValueOnGameThread()) * MB),
		static_cast<int64>(FMath::Max(0.0f, CVarMapOverviewBudgetMB.GetValueOnGameThread()) * MB));
}

int64 UVoxelMapSubsystem::ExploreAllTiles(const FVector& Center, float Radius)
{
	if (bShuttingDown || !ResolveChunkManager())
	{
		return 0;
	}
	const double Size = GetTileWorldSize();
	if (Size <= 0.0)
	{
		return 0;
	}
	if (PlanetGrid.IsValid())
	{
		// A planet: every tile of the atlas, up to the cap (spherical Phase 6, WM-BP11): 6 N^2 overflowed an int32 from
		// about 50 million tiles, and a 1,000 km planet has 6.4 billion. Past the cap the tiles nearest Center, and the
		// images of the nearest PlanetExploreAllImageTiles queued (the rest are drawn when the map shows them).
		const int64 N = PlanetGrid.GetCellsPerEdge();
		const int64 Total = 6 * N * N;
		const int64 Cap = FMath::Max<int64>(1, CVarMapExploreAllMaxTiles.GetValueOnGameThread());
		FVector Direction = (Center - CachedPlanetCenter).GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			Direction = FVector::UpVector;
		}
		TArray<FIntPoint> Tiles;
		if (Total <= Cap)
		{
			// Nearest first, so the images that draw first are the ones around the player. The keys are computed once (a
			// projection sort recomputed each tile's direction per comparison: 79 s for the 16 million tiles of a 50 km
			// planet with the cap raised).
			TArray<TPair<double, FIntPoint>> Keyed;
			Keyed.Reserve(static_cast<int32>(Total));
			for (int32 Y = 0; Y < N; ++Y)
			{
				for (int32 X = 0; X < 6 * N; ++X)
				{
					const FIntPoint Tile(X, Y);
					Keyed.Emplace(-FVector::DotProduct(PlanetGrid.CellCentreDirection(Tile), Direction), Tile);
				}
			}
			Keyed.Sort([](const TPair<double, FIntPoint>& A, const TPair<double, FIntPoint>& B) { return A.Key < B.Key; });
			Tiles.Reserve(Keyed.Num());
			for (const TPair<double, FIntPoint>& Entry : Keyed)
			{
				Tiles.Add(Entry.Value);
			}
		}
		else
		{
			PlanetGrid.GatherWithinAngle(Direction, UE_DOUBLE_PI, static_cast<int32>(FMath::Min<int64>(Cap, MAX_int32)), Tiles);
		}
		int64 PlanetCount = 0;
		int32 Queued = 0;
		for (const FIntPoint& Tile : Tiles)
		{
			const uint64 Key = PackTileKey(Tile);
			ExploredTiles.Add(Key);
			++PlanetCount;
			if (Queued < PlanetExploreAllImageTiles && !TileStore.Contains(Key) && !TileQueue.IsQueued(Key))
			{
				TileQueue.Request(Key, /*bInvalidate*/ false);
				++Queued;
			}
		}
		LaunchWaitingTiles();
		UE_LOG(LogVoxelMap, Log, TEXT("ExploreAllTiles: %lld of the planet's %lld tiles explored (cap %lld), %d images queued."),
			PlanetCount, Total, Cap, Queued);
		return PlanetCount;
	}
	// A heightmap world: the tiles of the area (an island's bounds, or Radius around Center on a plane), nearest Center
	// first in square rings so the cap keeps the tiles around the player (world modes review WM-MP9, the planet's rule).
	FBox2D Area = CachedWorldBounds.IsBounded() ? CachedWorldBounds.GetBox2D()
		: FBox2D(FVector2D(Center.X - Radius, Center.Y - Radius), FVector2D(Center.X + Radius, Center.Y + Radius));
	const FIntPoint Lo = WorldToTileCoord(FVector(Area.Min.X, Area.Min.Y, 0.0));
	const FIntPoint Hi = WorldToTileCoord(FVector(Area.Max.X, Area.Max.Y, 0.0));
	const FIntPoint CenterTile(FMath::Clamp(WorldToTileCoord(Center).X, Lo.X, Hi.X), FMath::Clamp(WorldToTileCoord(Center).Y, Lo.Y, Hi.Y));
	const int64 Cap = FMath::Max<int64>(1, CVarMapExploreAllMaxTiles.GetValueOnGameThread());
	const int32 MaxRing = FMath::Max(FMath::Max(CenterTile.X - Lo.X, Hi.X - CenterTile.X), FMath::Max(CenterTile.Y - Lo.Y, Hi.Y - CenterTile.Y));
	int64 Count = 0;
	auto Explore = [this, &Count, Cap, &Lo, &Hi](const FIntPoint& Tile)
	{
		if (Count >= Cap || Tile.X < Lo.X || Tile.X > Hi.X || Tile.Y < Lo.Y || Tile.Y > Hi.Y || !IsTileInsideWorld(Tile))
		{
			return;
		}
		const uint64 Key = PackTileKey(Tile);
		ExploredTiles.Add(Key);
		++Count;
		if (!TileStore.Contains(Key) && !TileQueue.IsQueued(Key))
		{
			TileQueue.Request(Key, /*bInvalidate*/ false);
		}
	};
	for (int32 Ring = 0; Ring <= MaxRing && Count < Cap; ++Ring)
	{
		if (Ring == 0)
		{
			Explore(CenterTile);
			continue;
		}
		for (int32 Along = -Ring; Along <= Ring && Count < Cap; ++Along)
		{
			Explore(FIntPoint(CenterTile.X + Along, CenterTile.Y - Ring));
			Explore(FIntPoint(CenterTile.X + Along, CenterTile.Y + Ring));
			if (Along > -Ring && Along < Ring)
			{
				Explore(FIntPoint(CenterTile.X - Ring, CenterTile.Y + Along));
				Explore(FIntPoint(CenterTile.X + Ring, CenterTile.Y + Along));
			}
		}
	}
	LaunchWaitingTiles();
	if (Count >= Cap)
	{
		UE_LOG(LogVoxelMap, Log, TEXT("ExploreAllTiles: %lld tiles explored, the nearest to (%.0f, %.0f): the cap (voxel.Map.ExploreAllMaxTiles)."), Count, Center.X, Center.Y);
	}
	return Count;
}

const FVoxelMapGlobeAlbedo* UVoxelMapSubsystem::GetGlobeAlbedo()
{
	if (GlobeAlbedo.IsValid())
	{
		return GlobeAlbedo.Get();
	}
	if (bGlobeAlbedoBuilding || bShuttingDown || !ResolveChunkManager() || !PlanetGrid.IsValid())
	{
		return nullptr;
	}
	UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get();
	const UVoxelWorldConfiguration* Config = ChunkMgr ? ChunkMgr->GetConfiguration() : nullptr;
	if (!Config)
	{
		return nullptr;
	}
	// The far field's analytic surface, over a world mode of our own (the task may outlive the chunk manager's).
	TSharedPtr<const FVoxelFarFieldSurface> Surface = FVoxelFarFieldSurface::Make(*Config, FVoxelWorldModeFactory::Create(*Config));
	if (!Surface.IsValid())
	{
		return nullptr;
	}
	bGlobeAlbedoBuilding = true;
	const int32 Cells = FMath::Min(GlobeAlbedoCellsPerEdge, PlanetGrid.GetCellsPerEdge());
	TWeakObjectPtr<UVoxelMapSubsystem> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Surface, Cells, Palette = CachedMaterialPalette, Min = CachedAltitudeMin, Max = CachedAltitudeMax]()
	{
		TSharedPtr<const FVoxelMapGlobeAlbedo> Built = BuildGlobeAlbedo(*Surface, Cells, Palette, Min, Max);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Built]()
		{
			UVoxelMapSubsystem* Self = WeakThis.Get();
			if (!Self || Self->bShuttingDown)
			{
				return;
			}
			Self->GlobeAlbedo = Built;
			Self->bGlobeAlbedoBuilding = false;
			Self->GlobeExploredFromCount = -1;
			UE_LOG(LogVoxelMap, Log, TEXT("Globe albedo: %d cells a face edge built in %.2f s."), Built->Grid.GetCellsPerEdge(), Built->BuildSeconds);
			Self->OnMapTileReady.Broadcast(FIntPoint(-1, -1));
		});
	});
	return nullptr;
}

bool UVoxelMapSubsystem::IsGlobeCellExplored(const FIntPoint& AlbedoCell)
{
	if (!GlobeAlbedo.IsValid() || !PlanetGrid.IsValid())
	{
		return false;
	}
	// The albedo cells holding an explored tile, refreshed when the exploration has changed (at most once a second past
	// 100,000 explored tiles: a planet's exploration can hold a million).
	const double Now = FPlatformTime::Seconds();
	if (GlobeExploredFromCount != ExploredTiles.Num()
		&& (GlobeExploredFromCount < 0 || ExploredTiles.Num() < 100000 || Now - GlobeExploredRefreshedSeconds >= 1.0))
	{
		GlobeExploredCells.Reset();
		const int32 N = PlanetGrid.GetCellsPerEdge();
		const int32 Nc = GlobeAlbedo->Grid.GetCellsPerEdge();
		for (const uint64 Key : ExploredTiles)
		{
			int32 Face = 0, IU = 0, IV = 0;
			if (PlanetGrid.FromAtlas(UnpackTileKey(Key), Face, IU, IV))
			{
				GlobeExploredCells.Add(PackTileKey(GlobeAlbedo->Grid.ToAtlas(Face, static_cast<int32>(static_cast<int64>(IU) * Nc / N),
					static_cast<int32>(static_cast<int64>(IV) * Nc / N))));
			}
		}
		GlobeExploredFromCount = ExploredTiles.Num();
		GlobeExploredRefreshedSeconds = Now;
	}
	return GlobeExploredCells.Contains(PackTileKey(AlbedoCell));
}

TSharedPtr<FVoxelMapGlobeAlbedo> UVoxelMapSubsystem::BuildGlobeAlbedo(const FVoxelFarFieldSurface& Surface, int32 CellsPerEdge,
	const TArray<FColor>& MaterialPalette, float AltitudeMin, float AltitudeMax)
{
	const double StartTime = FPlatformTime::Seconds();
	TSharedPtr<FVoxelMapGlobeAlbedo> Out = MakeShared<FVoxelMapGlobeAlbedo>();
	const int32 N = FMath::Max(1, CellsPerEdge);
	Out->Grid = FVoxelCubeSphereGrid(N);
	const int32 Width = 6 * N;
	const FSphericalPlanetWorldMode& Planet = Surface.GetPlanet();
	const double Radius = Planet.GetPlanetParams().PlanetRadius;

	// Pass 1: each cell's altitude and surface material at its centre (no conditioning zones: a POI pad does not show
	// at this size).
	TArray<float> Altitudes;
	TArray<uint8> Materials;
	Altitudes.SetNumUninitialized(Width * N);
	Materials.SetNumUninitialized(Width * N);
	const TArray<FVoxelConditioningZone> NoZones;
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const FVector Direction = Out->Grid.CellCentreDirection(FIntPoint(X, Y));
			float Continentalness = 0.0f;
			const float Altitude = Surface.SampleAltitude(Direction, NoZones, &Continentalness);
			uint8 Material = 0, Biome = 0;
			Surface.SampleConditions(Direction, Altitude, Continentalness, Material, Biome);
			Altitudes[Y * Width + X] = Altitude;
			Materials[Y * Width + X] = Material;
		}
	});

	// Pass 2: colour as a tile pixel (VoxelMapSubsystem's planet tiles): the sea by depth, land by its material shaded
	// by elevation and by a hillshade from the neighbouring cells on the face (one-sided at a face's edge).
	const FVoxelSeaSurface& Sea = Surface.Sea;
	const bool bSeaOn = Sea.bEnabled && Sea.bRadial;
	const float SeaAltitude = bSeaOn ? static_cast<float>(Sea.Radius - Radius) : AltitudeMin;
	const float LandBase = bSeaOn ? SeaAltitude : AltitudeMin;
	const float LandSpan = FMath::Max(AltitudeMax - LandBase, 1.0f);
	const float DepthSpan = FMath::Max(LandBase - AltitudeMin, 1.0f);
	Out->Colors.SetNumUninitialized(Width * N);
	ParallelFor(N, [&](int32 Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 Index = Y * Width + X;
			const float Altitude = Altitudes[Index];
			FColor Color;
			if (bSeaOn && Altitude < SeaAltitude)
			{
				const float DepthFactor = FMath::Lerp(1.0f, 0.3f, FMath::Clamp((SeaAltitude - Altitude) / DepthSpan, 0.0f, 1.0f));
				Color = FColor(static_cast<uint8>(20 * DepthFactor), static_cast<uint8>(80 * DepthFactor), static_cast<uint8>(180 * DepthFactor), 255);
			}
			else
			{
				Color = MaterialPalette.IsValidIndex(Materials[Index]) ? MaterialPalette[Materials[Index]] : FColor(128, 128, 128, 255);
				const float Elevation = FMath::Lerp(0.75f, 1.0f, FMath::Clamp((Altitude - LandBase) / LandSpan, 0.0f, 1.0f));
				// The cell's neighbours on its face (X wraps within the face's span of the atlas).
				const int32 Face = X / N;
				const int32 IU = X % N;
				auto PointAt = [&](int32 U, int32 V)
				{
					const int32 CU = FMath::Clamp(U, 0, N - 1);
					const int32 CV = FMath::Clamp(V, 0, N - 1);
					const FIntPoint Cell(Face * N + CU, CV);
					return Out->Grid.CellCentreDirection(Cell) * (Radius + Altitudes[CV * Width + Face * N + CU]);
				};
				const FVector Up = Out->Grid.CellCentreDirection(FIntPoint(X, Y));
				FVector Normal = FVector::CrossProduct(PointAt(IU + 1, Y) - PointAt(IU - 1, Y), PointAt(IU, Y + 1) - PointAt(IU, Y - 1)).GetSafeNormal();
				if (Normal.IsNearlyZero())
				{
					Normal = Up;
				}
				else if (FVector::DotProduct(Normal, Up) < 0.0)
				{
					Normal = -Normal;
				}
				FVector East, North;
				FVoxelCubeSphere::GetTangentFrame(Up, East, North);
				const FVector LightDir = (North * -0.707 + East * -0.707 + Up).GetSafeNormal();
				const float Shade = FMath::Lerp(0.55f, 1.25f, FMath::Clamp(static_cast<float>(FVector::DotProduct(Normal, LightDir)), 0.0f, 1.0f));
				const float Brightness = Elevation * Shade;
				Color.R = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.R * Brightness), 0, 255));
				Color.G = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.G * Brightness), 0, 255));
				Color.B = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Color.B * Brightness), 0, 255));
				Color.A = 255;
			}
			Out->Colors[Index] = Color;
		}
	});
	Out->BuildSeconds = FPlatformTime::Seconds() - StartTime;
	return Out;
}

bool UVoxelMapSubsystem::IsTileExplored(FIntPoint TileCoord) const
{
	return ExploredTiles.Contains(PackTileKey(TileCoord));
}

bool UVoxelMapSubsystem::IsTileInsideWorld(FIntPoint TileCoord) const
{
	if (PlanetGrid.IsValid())
	{
		// A planet: the atlas (spherical Phase 4).
		return PlanetGrid.IsValidCell(TileCoord);
	}
	if (!CachedWorldBounds.IsBounded())
	{
		return true;
	}
	const FVector Min = TileCoordToWorld(TileCoord);
	const double Size = GetTileWorldSize();
	return CachedWorldBounds.IntersectsBox2D(FBox2D(FVector2D(Min.X, Min.Y), FVector2D(Min.X + Size, Min.Y + Size)));
}

int32 UVoxelMapSubsystem::PruneExploredOutsideWorld()
{
	if (!CachedWorldBounds.IsBounded())
	{
		return 0;
	}
	int32 Dropped = 0;
	for (auto It = ExploredTiles.CreateIterator(); It; ++It)
	{
		if (!IsTileInsideWorld(UnpackTileKey(*It)))
		{
			It.RemoveCurrent();
			++Dropped;
		}
	}
	if (Dropped > 0)
	{
		UE_LOG(LogVoxelMap, Log, TEXT("Dropped %d explored map tile(s) outside the world's bounds (%d explored now)."), Dropped, ExploredTiles.Num());
	}
	return Dropped;
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

	// Explored tiles draw from the tile cache: queue the images (four run at a time; the rest wait in the queue).
	// Tiles outside the world (explored before exploration stopped at the edge, WM-I2) are dropped first; without a
	// resolved world they are dropped when it resolves.
	if (ResolveChunkManager())
	{
		PruneExploredOutsideWorld();
		for (const FIntPoint& Tile : Tiles)
		{
			const uint64 Key = PackTileKey(Tile);
			if (!ExploredTiles.Contains(Key))
			{
				continue;
			}
			// Restored tiles keep only their overview (WM-E2): the minimap asks for detail where the player is.
			if (!TileStore.Contains(Key) && !TileQueue.IsQueued(Key))
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
	if (PlanetGrid.IsValid())
	{
		return PlanetGrid.CellOfDirection(FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, CachedPlanetCenter));
	}
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
	if (PlanetGrid.IsValid())
	{
		// A planet: the tile's (U, V) = (0, 0) corner on the datum sphere.
		return PlanetGrid.IsValidCell(TileCoord)
			? CachedPlanetCenter + PlanetGrid.DirectionAt(TileCoord, 0.0, 0.0) * CachedPlanetRadius
			: CachedPlanetCenter;
	}
	const float ChunkWorldSize = CachedChunkSize * CachedVoxelSize;
	return FVector(
		TileCoord.X * ChunkWorldSize + CachedWorldOrigin.X,
		TileCoord.Y * ChunkWorldSize + CachedWorldOrigin.Y,
		0.0f
	);
}

float UVoxelMapSubsystem::GetTileWorldSize() const
{
	if (PlanetGrid.IsValid())
	{
		// Along a face's centre lines on the datum sphere (the tangent warp keeps the rest within about 1.2x).
		return static_cast<float>(0.5 * UE_DOUBLE_PI * CachedPlanetRadius / PlanetGrid.GetCellsPerEdge());
	}
	return CachedChunkSize * CachedVoxelSize;
}

bool UVoxelMapSubsystem::PlanetDirectionToTilePixel(const FVector& Direction, FIntPoint& OutTile, int32& OutPX, int32& OutPY) const
{
	if (!PlanetGrid.IsValid())
	{
		return false;
	}
	FVector2D Fraction;
	OutTile = PlanetGrid.CellOfDirection(Direction, &Fraction);
	const int32 Res = FMath::Max(1, CachedChunkSize);
	OutPX = FMath::Clamp(FMath::FloorToInt32(Fraction.X * Res), 0, Res - 1);
	OutPY = FMath::Clamp(FMath::FloorToInt32(Fraction.Y * Res), 0, Res - 1);
	return true;
}

FIntPoint UVoxelMapSubsystem::GetNeighbourTile(FIntPoint TileCoord, int32 DX, int32 DY) const
{
	return PlanetGrid.IsValid() ? PlanetGrid.Neighbour(TileCoord, DX, DY) : TileCoord + FIntPoint(DX, DY);
}

void UVoxelMapSubsystem::GetChunkTiles(const FIntVector& ChunkCoord, TArray<FIntPoint>& OutTiles) const
{
	if (!PlanetGrid.IsValid())
	{
		OutTiles.Add(FIntPoint(ChunkCoord.X, ChunkCoord.Y));
		return;
	}
	// The cells the chunk's corners and centre span from the planet centre.
	const double ChunkWorldSize = static_cast<double>(CachedChunkSize) * CachedVoxelSize;
	TArray<FVector, TInlineAllocator<9>> Directions;
	for (int32 Corner = 0; Corner < 9; ++Corner)
	{
		const FVector Offset = Corner < 8 ? FVector(Corner & 1, (Corner >> 1) & 1, (Corner >> 2) & 1) : FVector(0.5);
		Directions.Add(FSphericalPlanetWorldMode::GetDirectionFromCenter(
			CachedWorldOrigin + (FVector(ChunkCoord) + Offset) * ChunkWorldSize, CachedPlanetCenter));
	}
	PlanetGrid.GatherSpanned(Directions, OutTiles);
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

	if (PlanetGrid.IsValid())
	{
		// A planet: the tiles whose centres lie within the radius (plus a tile) along the surface, the hemispheres apart.
		TArray<FIntPoint> Tiles;
		const double Angle = (Radius + GetTileWorldSize()) / FMath::Max(CachedPlanetRadius, 1.0);
		PlanetGrid.GatherWithinAngle(FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, CachedPlanetCenter), Angle, 16384, Tiles);
		for (const FIntPoint& Tile : Tiles)
		{
			const uint64 Key = PackTileKey(Tile);
			ExploredTiles.Add(Key);
			if (!TileStore.HasDetail(Key))
			{
				DetailWanted.Add(Key);
				if (!TileQueue.IsQueued(Key))
				{
					QueueTileGeneration(Tile);
				}
			}
		}
		return;
	}

	const FIntPoint CenterTile = WorldToTileCoord(WorldPos);
	const int32 TileRadius = FMath::CeilToInt(Radius / ChunkWorldSize);

	for (int32 TY = CenterTile.Y - TileRadius; TY <= CenterTile.Y + TileRadius; ++TY)
	{
		for (int32 TX = CenterTile.X - TileRadius; TX <= CenterTile.X + TileRadius; ++TX)
		{
			const FIntPoint TileCoord(TX, TY);
			// Exploration stops at the world's edge (WM-I2): the sea past an island's R + falloff is not mapped.
			if (!IsTileInsideWorld(TileCoord))
			{
				continue;
			}
			const uint64 Key = PackTileKey(TileCoord);

			ExploredTiles.Add(Key);

			// The minimap's area wants the detail image: drawn again if it was evicted (WM-E2).
			if (!TileStore.HasDetail(Key))
			{
				DetailWanted.Add(Key);
				if (!TileQueue.IsQueued(Key))
				{
					QueueTileGeneration(TileCoord);
				}
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
			if (!TileStore.Contains(ExploredKey) && !TileQueue.IsQueued(ExploredKey))
			{
				QueueTileGeneration(UnpackTileKey(ExploredKey));
			}
		}
	}

	if (PlanetGrid.IsValid())
	{
		// A planet: the tiles the chunk spans (spherical Phase 4), each explored with its detail image.
		TArray<FIntPoint> Tiles;
		GetChunkTiles(ChunkCoord, Tiles);
		for (const FIntPoint& Tile : Tiles)
		{
			const uint64 TileKey = PackTileKey(Tile);
			ExploredTiles.Add(TileKey);
			if (!TileStore.HasDetail(TileKey))
			{
				DetailWanted.Add(TileKey);
				if (!TileQueue.IsQueued(TileKey))
				{
					QueueTileGeneration(Tile);
				}
			}
			if (OverlayAwaitingTiles.Contains(TileKey))
			{
				MarkOverlayDirty(TileKey);
			}
		}
		return;
	}

	// Mark as explored (chunks that generate are in the player's vicinity), inside the world only: an island also
	// generates a ring of floor chunks past its edge (WM-I2).
	if (IsTileInsideWorld(TileCoord))
	{
		ExploredTiles.Add(Key);

		// Streaming is the player's vicinity: keep the detail image (drawn again if it was evicted).
		if (!TileStore.HasDetail(Key))
		{
			DetailWanted.Add(Key);
			if (!TileQueue.IsQueued(Key))
			{
				QueueTileGeneration(TileCoord);
			}
		}
	}

	// A tile whose overlay waits for its chunks: this may be one of them.
	if (OverlayAwaitingTiles.Contains(Key))
	{
		MarkOverlayDirty(Key);
	}
}

void UVoxelMapSubsystem::HandleTerrainConditioningChanged()
{
	if (bShuttingDown)
	{
		return;
	}
	// Tiles drawn before the POI conditioner registered show the unflattened terrain (QA W-05): redraw everything
	// explored, discarding drawings already running. The current images stay until the new ones land (R22: the map
	// used to flash blank here), and the redraws reuse the stored edit overlays without reading chunk data.
	// Tiles without an image (evicted, WM-E2) are drawn fresh when asked for; queued ones are invalidated.
	int32 Redrawn = 0;
	for (const uint64 Key : ExploredTiles)
	{
		if (TileStore.Contains(Key) || TileQueue.IsQueued(Key))
		{
			TileQueue.Request(Key, /*bInvalidate*/ true);
			++Redrawn;
		}
	}
	LaunchWaitingTiles();
	UE_LOG(LogVoxelMap, Log, TEXT("Terrain conditioning changed: redrawing %d map tile(s) (%d with an image kept until redrawn)."),
		Redrawn, TileStore.Num());
}

// ---------------------------------------------------------------------------
// Edit overlays (R22)
// ---------------------------------------------------------------------------

const FVoxelMapEditOverlay* UVoxelMapSubsystem::GetEditOverlay(FIntPoint TileCoord) const
{
	const TSharedPtr<const FVoxelMapEditOverlay>* Found = EditOverlays.Find(PackTileKey(TileCoord));
	return Found ? Found->Get() : nullptr;
}

namespace
{
	/** 'VMEO': map edit overlays. */
	constexpr uint32 EditOverlayBlobMagic = 0x4F454D56;

	/** Bump when the blob layout around the per-overlay data changes. */
	constexpr uint8 EditOverlayBlobVersion = 1;

	/** Refuse to inflate a claimed payload beyond this (a corrupt size field). */
	constexpr uint32 MaxEditOverlayBlobBytes = 256u * 1024u * 1024u;
}

FString UVoxelMapSubsystem::EncodeEditOverlays(const TArray<FVoxelMapTileOverlay>& Overlays)
{
	if (Overlays.Num() == 0)
	{
		return FString();
	}
	TArray<uint8> Raw;
	{
		FMemoryWriter Writer(Raw);
		uint32 Magic = EditOverlayBlobMagic;
		uint8 Version = EditOverlayBlobVersion;
		int32 Count = Overlays.Num();
		Writer << Magic << Version << Count;
		for (const FVoxelMapTileOverlay& Entry : Overlays)
		{
			int32 X = Entry.Tile.X;
			int32 Y = Entry.Tile.Y;
			Writer << X << Y;
			FVoxelMapEditOverlay Copy = Entry.Overlay;
			Copy.Serialize(Writer);
		}
	}

	int32 CompressedBytes = FCompression::CompressMemoryBound(NAME_Zlib, Raw.Num());
	TArray<uint8> Compressed;
	Compressed.SetNumUninitialized(CompressedBytes);
	if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), CompressedBytes, Raw.GetData(), Raw.Num()))
	{
		UE_LOG(LogVoxelMap, Warning, TEXT("EncodeEditOverlays: compressing %d tile overlay(s) failed."), Overlays.Num());
		return FString();
	}

	// The uncompressed size travels in front of the zlib stream: decoding needs it.
	TArray<uint8> Blob;
	{
		FMemoryWriter Writer(Blob);
		uint32 RawSize = static_cast<uint32>(Raw.Num());
		Writer << RawSize;
		Writer.Serialize(Compressed.GetData(), CompressedBytes);
	}
	return FBase64::Encode(Blob);
}

bool UVoxelMapSubsystem::DecodeEditOverlays(const FString& Encoded, TArray<FVoxelMapTileOverlay>& OutOverlays)
{
	OutOverlays.Reset();
	if (Encoded.IsEmpty())
	{
		return true;
	}
	TArray<uint8> Blob;
	if (!FBase64::Decode(Encoded, Blob) || Blob.Num() <= static_cast<int32>(sizeof(uint32)))
	{
		return false;
	}
	uint32 RawSize = 0;
	{
		FMemoryReader Header(Blob);
		Header << RawSize;
	}
	if (RawSize == 0 || RawSize > MaxEditOverlayBlobBytes)
	{
		return false;
	}
	TArray<uint8> Raw;
	Raw.SetNumUninitialized(RawSize);
	if (!FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), RawSize, Blob.GetData() + sizeof(uint32), Blob.Num() - sizeof(uint32)))
	{
		return false;
	}

	FMemoryReader Reader(Raw);
	uint32 Magic = 0;
	uint8 Version = 0;
	int32 Count = 0;
	Reader << Magic << Version << Count;
	// Each tile takes at least its coordinates plus an overlay header, so Count cannot exceed the payload.
	if (Reader.IsError() || Magic != EditOverlayBlobMagic || Version != EditOverlayBlobVersion || Count < 0
		|| Count > static_cast<int32>(RawSize / 8))
	{
		return false;
	}
	OutOverlays.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		FVoxelMapTileOverlay& Entry = OutOverlays.AddDefaulted_GetRef();
		Reader << Entry.Tile.X << Entry.Tile.Y;
		if (Reader.IsError() || !Entry.Overlay.Serialize(Reader))
		{
			OutOverlays.Reset();
			return false;
		}
	}
	return true;
}

FString UVoxelMapSubsystem::ExportEditOverlays(int32& OutCount) const
{
	TArray<FVoxelMapTileOverlay> Tiles;
	Tiles.Reserve(EditOverlays.Num());
	for (const TPair<uint64, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : EditOverlays)
	{
		if (Pair.Value.IsValid() && !Pair.Value->IsEmpty())
		{
			FVoxelMapTileOverlay& Entry = Tiles.AddDefaulted_GetRef();
			Entry.Tile = UnpackTileKey(Pair.Key);
			Entry.Overlay = *Pair.Value;
		}
	}
	// Row-major order keeps the blob deterministic for an unchanged world.
	Tiles.Sort([](const FVoxelMapTileOverlay& A, const FVoxelMapTileOverlay& B)
	{
		return A.Tile.Y != B.Tile.Y ? A.Tile.Y < B.Tile.Y : A.Tile.X < B.Tile.X;
	});
	FString Encoded = EncodeEditOverlays(Tiles);
	OutCount = Encoded.IsEmpty() ? 0 : Tiles.Num();
	return Encoded;
}

int32 UVoxelMapSubsystem::ImportEditOverlays(const FString& Encoded)
{
	if (bShuttingDown)
	{
		return 0;
	}
	TArray<FVoxelMapTileOverlay> Tiles;
	if (!DecodeEditOverlays(Encoded, Tiles))
	{
		UE_LOG(LogVoxelMap, Warning, TEXT("ImportEditOverlays: malformed map edit overlay data; edits show on the map as their areas load."));
		Tiles.Reset();
	}

	// The save replaces the overlays. Every tile whose overlay changes is redrawn, and refreshed once the edits that
	// load next are in: rebuilt where its edited chunks are loaded, cleared where no edits remain, kept otherwise.
	TSet<uint64> Touched;
	for (const TPair<uint64, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : EditOverlays)
	{
		Touched.Add(Pair.Key);
	}
	EditOverlays.Reset();
	int32 Restored = 0;
	for (FVoxelMapTileOverlay& Entry : Tiles)
	{
		const bool bResolutionMatches = !bCacheResolved || Entry.Overlay.Resolution == CachedChunkSize;
		const bool bKindMatches = !bCacheResolved || Entry.Overlay.bAltitudeHeights == PlanetGrid.IsValid();
		if (Entry.Overlay.IsEmpty() || !bResolutionMatches || !bKindMatches)
		{
			continue;
		}
		const uint64 Key = PackTileKey(Entry.Tile);
		EditOverlays.Add(Key, MakeShared<const FVoxelMapEditOverlay>(MoveTemp(Entry.Overlay)));
		Touched.Add(Key);
		++Restored;
	}
	for (const uint64 Key : Touched)
	{
		MarkOverlayDirty(Key);
		if (bCacheResolved && (TileStore.Contains(Key) || TileQueue.IsQueued(Key)))
		{
			QueueTileGeneration(UnpackTileKey(Key), /*bInvalidate*/ true);
		}
	}
	UE_LOG(LogVoxelMap, Log, TEXT("Restored %d map edit overlay(s) (%d tile(s) refreshed)."), Restored, Touched.Num());
	return Restored;
}

void UVoxelMapSubsystem::RedrawAllTiles(bool bRebuildOverlays)
{
	if (bShuttingDown || !ResolveChunkManager())
	{
		return;
	}
	if (bRebuildOverlays)
	{
		for (const TPair<uint64, TArray<int32>>& Pair : TileEditedChunkZ)
		{
			MarkOverlayDirty(Pair.Key);
		}
		for (const TPair<uint64, TArray<FIntVector>>& Pair : PlanetTileEditedChunks)
		{
			MarkOverlayDirty(Pair.Key);
		}
		// Tiles with an overlay but no edits left rebuild to empty.
		for (const TPair<uint64, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : EditOverlays)
		{
			MarkOverlayDirty(Pair.Key);
		}
	}
	int32 Redrawn = 0;
	for (const uint64 Key : ExploredTiles)
	{
		if (TileStore.Contains(Key) || TileQueue.IsQueued(Key))
		{
			TileQueue.Request(Key, /*bInvalidate*/ true);
			++Redrawn;
		}
	}
	LaunchWaitingTiles();
	UE_LOG(LogVoxelMap, Log, TEXT("Redrawing %d map tile(s) with an image%s."), Redrawn,
		bRebuildOverlays ? TEXT(" and rebuilding edit overlays") : TEXT(""));
}

FString UVoxelMapSubsystem::GetDebugStats() const
{
	int32 MaskedColumns = 0;
	int32 Bytes = 0;
	for (const TPair<uint64, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : EditOverlays)
	{
		if (Pair.Value.IsValid())
		{
			MaskedColumns += Pair.Value->Num();
			Bytes += Pair.Value->Columns.GetAllocatedSize() + Pair.Value->Heights.GetAllocatedSize()
				+ Pair.Value->Materials.GetAllocatedSize() + Pair.Value->Flags.GetAllocatedSize();
		}
	}
	int32 ExploredOutside = 0;
	for (const uint64 Key : ExploredTiles)
	{
		ExploredOutside += IsTileInsideWorld(UnpackTileKey(Key)) ? 0 : 1;
	}
	constexpr double MB = 1024.0 * 1024.0;
	return FString::Printf(
		TEXT("tiles cached=%d explored=%d (outside the world %d) waiting=%d drawing=%d | images detail=%d %.1f/%.0f MB overview=%d %.1f/%.0f MB evicted detail=%lld overview=%lld | overlays=%d columns=%d bytes=%d edited-tiles=%d dirty=%d awaiting=%d rebuild-queued=%d"),
		TileStore.Num(), ExploredTiles.Num(), ExploredOutside, TileQueue.NumWaiting(), TileQueue.NumDrawing(),
		TileStore.NumDetail(), TileStore.GetDetailBytes() / MB, TileStore.GetDetailBudget() / MB,
		TileStore.Num(), TileStore.GetOverviewBytes() / MB, TileStore.GetOverviewBudget() / MB,
		TileStore.GetDetailEvictions(), TileStore.GetOverviewEvictions(),
		EditOverlays.Num(), MaskedColumns, Bytes, TileEditedChunkZ.Num() + PlanetTileEditedChunks.Num(), OverlayDirtyTiles.Num(),
		OverlayAwaitingTiles.Num(), OverlayRebuildTiles.Num());
}

bool UVoxelMapSubsystem::CaptureMapImage(const FVector& Center, int32 RadiusTiles, const FString& FilePath, bool bHighlightOverlay) const
{
	const int32 Res = CachedChunkSize;
	if (!bCacheResolved || Res <= 0 || RadiusTiles < 0 || RadiusTiles > 64 || CachedVoxelSize <= 0.0f)
	{
		return false;
	}
	const int32 Size = (2 * RadiusTiles + 1) * Res;

	if (PlanetGrid.IsValid())
	{
		// A planet (spherical Phase 4): the tangent plane at the position, north (toward the +Z pole) up and east right, a
		// texel per tile pixel; each texel shows the explored tile pixel its ground point's direction falls in.
		const FVector Up = FSphericalPlanetWorldMode::GetDirectionFromCenter(Center, CachedPlanetCenter);
		FVector East, North;
		FVoxelCubeSphere::GetTangentFrame(Up, East, North);
		const double Texel = GetTileWorldSize() / Res;
		const FVector Ground = CachedPlanetCenter + Up * CachedPlanetRadius;
		const int32 Half = Size / 2;
		TArray64<FColor> PlanetImage;
		PlanetImage.Init(FColor(10, 10, 10, 255), static_cast<int64>(Size) * Size);
		FIntPoint LastTile(MIN_int32, MIN_int32);
		const FVoxelMapTile* LastCached = nullptr;
		const FVoxelMapEditOverlay* LastOverlay = nullptr;
		for (int32 Row = 0; Row < Size; ++Row)
		{
			for (int32 Column = 0; Column < Size; ++Column)
			{
				const FVector Point = Ground + East * ((Column - Half) * Texel) + North * ((Half - Row) * Texel);
				FIntPoint Tile;
				int32 PX = 0;
				int32 PY = 0;
				PlanetDirectionToTilePixel((Point - CachedPlanetCenter).GetSafeNormal(), Tile, PX, PY);
				if (Tile != LastTile)
				{
					LastTile = Tile;
					LastCached = IsTileExplored(Tile) ? TileStore.Find(PackTileKey(Tile), /*bTouch*/ false) : nullptr;
					LastOverlay = bHighlightOverlay ? GetEditOverlay(Tile) : nullptr;
				}
				const int32 TileRes = LastCached ? LastCached->Resolution : 0;
				if (!LastCached || TileRes <= 0 || LastCached->PixelData.Num() != TileRes * TileRes)
				{
					continue;
				}
				FColor Color = LastCached->PixelData[(PY * TileRes / Res) * TileRes + (PX * TileRes / Res)];
				if (LastOverlay && LastOverlay->FindColumn(PX, PY) != INDEX_NONE)
				{
					Color = FColor((Color.R + 255) / 2, Color.G / 2, (Color.B + 255) / 2, 255);
				}
				PlanetImage[static_cast<int64>(Row) * Size + Column] = Color;
			}
		}
		for (int32 D = -3; D <= 3; ++D)
		{
			PlanetImage[static_cast<int64>(Half) * Size + Half + D] = FColor::Red;
			PlanetImage[static_cast<int64>(Half + D) * Size + Half] = FColor::Red;
		}
		TArray64<uint8> PlanetPng;
		FImageUtils::PNGCompressImageArray(Size, Size, PlanetImage, PlanetPng);
		return PlanetPng.Num() > 0 && FFileHelper::SaveArrayToFile(PlanetPng, *FilePath);
	}

	const FIntPoint CenterTile = WorldToTileCoord(Center);
	const FIntPoint MinTile = CenterTile - FIntPoint(RadiusTiles, RadiusTiles);

	// Image rows run along world -X (north up), columns along world +Y (east right), like UVCWorldMapWidget.
	TArray64<FColor> Image;
	Image.Init(FColor(10, 10, 10, 255), static_cast<int64>(Size) * Size);
	auto Put = [&Image, Size](int32 GX, int32 GY, const FColor& Color)
	{
		if (GX >= 0 && GY >= 0 && GX < Size && GY < Size)
		{
			Image[static_cast<int64>(Size - 1 - GX) * Size + GY] = Color;
		}
	};
	for (int32 TXi = 0; TXi <= 2 * RadiusTiles; ++TXi)
	{
		for (int32 TYi = 0; TYi <= 2 * RadiusTiles; ++TYi)
		{
			const FIntPoint Tile = MinTile + FIntPoint(TXi, TYi);
			const FVoxelMapTile* Cached = IsTileExplored(Tile) ? TileStore.Find(PackTileKey(Tile), /*bTouch*/ false) : nullptr;
			const int32 TileRes = Cached ? Cached->Resolution : 0;
			if (!Cached || TileRes <= 0 || Cached->PixelData.Num() != TileRes * TileRes)
			{
				continue;
			}
			const FVoxelMapEditOverlay* Overlay = bHighlightOverlay ? GetEditOverlay(Tile) : nullptr;
			for (int32 PY = 0; PY < Res; ++PY)
			{
				for (int32 PX = 0; PX < Res; ++PX)
				{
					// An overview (evicted detail, WM-E2) is scaled up: each of its pixels covers several here.
					FColor Color = Cached->PixelData[(PY * TileRes / Res) * TileRes + (PX * TileRes / Res)];
					if (Overlay && Overlay->FindColumn(PX, PY) != INDEX_NONE)
					{
						Color = FColor((Color.R + 255) / 2, Color.G / 2, (Color.B + 255) / 2, 255);
					}
					Put(TXi * Res + PX, TYi * Res + PY, Color);
				}
			}
		}
	}

	// Mark the centre position with a small red cross.
	const int32 CX = FMath::FloorToInt((Center.X - CachedWorldOrigin.X) / CachedVoxelSize) - MinTile.X * Res;
	const int32 CY = FMath::FloorToInt((Center.Y - CachedWorldOrigin.Y) / CachedVoxelSize) - MinTile.Y * Res;
	for (int32 D = -3; D <= 3; ++D)
	{
		Put(CX + D, CY, FColor::Red);
		Put(CX, CY + D, FColor::Red);
	}

	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Size, Size, Image, Png);
	return Png.Num() > 0 && FFileHelper::SaveArrayToFile(Png, *FilePath);
}

void UVoxelMapSubsystem::HandleChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius)
{
	if (bShuttingDown)
	{
		return;
	}
	// Undo, redo, clear and load report a zero centre and radius: key on the chunk only.
	RecordChunkEdits(ChunkCoord);
	TArray<FIntPoint> Tiles;
	GetChunkTiles(ChunkCoord, Tiles);
	for (const FIntPoint& Tile : Tiles)
	{
		MarkOverlayDirty(PackTileKey(Tile));
	}
}

void UVoxelMapSubsystem::RecordChunkEdits(const FIntVector& ChunkCoord)
{
	const UVoxelEditManager* EditManager = CachedEditManagerWeak.Get();
	if (PlanetGrid.IsValid())
	{
		// A planet: the chunk on every tile it spans.
		const bool bHasEdits = EditManager && EditManager->ChunkHasEdits(ChunkCoord);
		TArray<FIntPoint> Tiles;
		GetChunkTiles(ChunkCoord, Tiles);
		for (const FIntPoint& Tile : Tiles)
		{
			const uint64 TileKey = PackTileKey(Tile);
			if (bHasEdits)
			{
				PlanetTileEditedChunks.FindOrAdd(TileKey).AddUnique(ChunkCoord);
			}
			else if (TArray<FIntVector>* Chunks = PlanetTileEditedChunks.Find(TileKey))
			{
				Chunks->Remove(ChunkCoord);
				if (Chunks->Num() == 0)
				{
					PlanetTileEditedChunks.Remove(TileKey);
				}
			}
		}
		return;
	}
	const uint64 Key = PackTileKey(FIntPoint(ChunkCoord.X, ChunkCoord.Y));
	if (EditManager && EditManager->ChunkHasEdits(ChunkCoord))
	{
		TileEditedChunkZ.FindOrAdd(Key).AddUnique(ChunkCoord.Z);
	}
	else if (TArray<int32>* Zs = TileEditedChunkZ.Find(Key))
	{
		Zs->Remove(ChunkCoord.Z);
		if (Zs->Num() == 0)
		{
			TileEditedChunkZ.Remove(Key);
		}
	}
}

void UVoxelMapSubsystem::MarkOverlayDirty(uint64 TileKey)
{
	if (bShuttingDown)
	{
		return;
	}
	OverlayDirtyTiles.Add(TileKey);
	ArmOverlayRefresh();
}

void UVoxelMapSubsystem::ArmOverlayRefresh()
{
	UWorld* World = GetWorld();
	if (!World || bShuttingDown)
	{
		return;
	}
	FTimerManager& Timers = World->GetTimerManager();
	if (!Timers.IsTimerActive(OverlayRefreshTimer))
	{
		Timers.SetTimer(OverlayRefreshTimer, this, &UVoxelMapSubsystem::FlushOverlayRefresh,
			FMath::Max(0.01f, CVarMapEditRefreshSeconds.GetValueOnGameThread()), false);
	}
}

void UVoxelMapSubsystem::FlushOverlayRefresh()
{
	if (bShuttingDown || OverlayDirtyTiles.Num() == 0)
	{
		return;
	}
	if (!ResolveChunkManager())
	{
		ArmOverlayRefresh();
		return;
	}

	// Tiles whose edited chunks are not loaded keep their overlay (the saved one after a load) and wait for those
	// chunks; the check is cheap, so every dirty tile gets it. Rebuilds are capped per refresh.
	const int32 MaxRebuilds = FMath::Max(1, CVarMapEditRefreshMaxTiles.GetValueOnGameThread());
	int32 Rebuilds = 0;
	int32 Awaiting = 0;
	const TArray<uint64> Dirty = OverlayDirtyTiles.Array();
	for (const uint64 Key : Dirty)
	{
		if (!CanRebuildOverlay(Key))
		{
			OverlayDirtyTiles.Remove(Key);
			OverlayAwaitingTiles.Add(Key);
			++Awaiting;
			continue;
		}
		if (Rebuilds >= MaxRebuilds)
		{
			continue;
		}
		OverlayDirtyTiles.Remove(Key);
		OverlayAwaitingTiles.Remove(Key);
		OverlayRebuildTiles.Add(Key);
		TileQueue.Request(Key, /*bInvalidate*/ true, /*bPriority*/ true);
		++Rebuilds;
	}
	LaunchWaitingTiles();
	UE_LOG(LogVoxelMap, Verbose, TEXT("Map edit refresh: %d overlay rebuild(s), %d tile(s) awaiting chunks, %d left for the next refresh."),
		Rebuilds, Awaiting, OverlayDirtyTiles.Num());

	if (OverlayDirtyTiles.Num() > 0)
	{
		ArmOverlayRefresh();
	}
}

bool UVoxelMapSubsystem::CanRebuildOverlay(uint64 TileKey) const
{
	if (PlanetGrid.IsValid())
	{
		const TArray<FIntVector>* EditedChunks = PlanetTileEditedChunks.Find(TileKey);
		if (!EditedChunks)
		{
			return true;
		}
		const UVoxelChunkManager* PlanetChunkMgr = CachedChunkManagerWeak.Get();
		if (!PlanetChunkMgr)
		{
			return false;
		}
		// Edited chunks wholly more than a chunk below the tile's lowest analytic altitude cannot reach its surface.
		const double ChunkWorldSize = static_cast<double>(CachedChunkSize) * CachedVoxelSize;
		const FVector2f* Range = TileAnalyticRange.Find(TileKey);
		for (const FIntVector& Coord : *EditedChunks)
		{
			if (Range)
			{
				double MaxRadius = 0.0;
				for (int32 Corner = 0; Corner < 8; ++Corner)
				{
					const FVector Offset(Corner & 1, (Corner >> 1) & 1, (Corner >> 2) & 1);
					MaxRadius = FMath::Max(MaxRadius, FVector::Dist(CachedWorldOrigin + (FVector(Coord) + Offset) * ChunkWorldSize, CachedPlanetCenter));
				}
				if (MaxRadius - CachedPlanetRadius < Range->X - ChunkWorldSize)
				{
					continue;
				}
			}
			if (!PlanetChunkMgr->HasChunkVoxelData(Coord))
			{
				return false;
			}
		}
		return true;
	}
	const TArray<int32>* EditedZs = TileEditedChunkZ.Find(TileKey);
	if (!EditedZs)
	{
		// No edits left (undone or cleared): the rebuild clears the overlay without chunk data.
		return true;
	}
	const UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get();
	if (!ChunkMgr)
	{
		return false;
	}

	// Edits more than a chunk below the analytic surface cannot reach the top surface without the chunks above them
	// being edited too, so they need not be loaded (dungeon interiors under a loaded entrance).
	int32 MinRequiredZ = MIN_int32;
	if (const FVector2f* Range = TileAnalyticRange.Find(TileKey))
	{
		const float ChunkWorldSize = CachedChunkSize * CachedVoxelSize;
		MinRequiredZ = FMath::FloorToInt((Range->X - CachedWorldOrigin.Z) / ChunkWorldSize) - 1;
	}
	const FIntPoint Tile = UnpackTileKey(TileKey);
	for (const int32 Z : *EditedZs)
	{
		if (Z >= MinRequiredZ && !ChunkMgr->HasChunkVoxelData(FIntVector(Tile.X, Tile.Y, Z)))
		{
			return false;
		}
	}
	return true;
}

TSharedPtr<FVoxelMapOverlayBuildInput> UVoxelMapSubsystem::GatherOverlayInput(FIntPoint TileCoord) const
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("map overlay"));
	UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get();
	const UVoxelEditManager* EditManager = CachedEditManagerWeak.Get();
	if (!ChunkMgr || !EditManager)
	{
		return nullptr;
	}

	TSharedPtr<FVoxelMapOverlayBuildInput> Input = MakeShared<FVoxelMapOverlayBuildInput>();
	const int32 CS = CachedChunkSize;
	Input->ChunkSize = CS;
	Input->VoxelSize = CachedVoxelSize;
	Input->WorldOriginZ = CachedWorldOrigin.Z;
	Input->bUseWater = bWaterEnabled;
	Input->WaterLevel = CachedWaterLevel;

	const int32 VolumeSize = CS * CS * CS;
	auto Snapshot = [ChunkMgr, VolumeSize](const FIntVector& Coord) -> TSharedPtr<const TArray<FVoxelData>>
	{
		TSharedPtr<const TArray<FVoxelData>> Voxels = ChunkMgr->GetEditMergedSnapshot(Coord);
		return (Voxels.IsValid() && Voxels->Num() == VolumeSize) ? Voxels : nullptr;
	};

	const TArray<int32>* EditedZs = TileEditedChunkZ.Find(PackTileKey(TileCoord));
	if (!EditedZs)
	{
		return Input;
	}

	// Edited chunks: shared edit-merged snapshot (if loaded) plus the highest edited voxel per column, which tells
	// the build which columns to scan and whether an edit reaches their surface.
	int32 MinZ = MAX_int32;
	int32 MaxZ = MIN_int32;
	for (const int32 Z : *EditedZs)
	{
		const FIntVector Coord(TileCoord.X, TileCoord.Y, Z);
		FVoxelMapOverlayChunk& Chunk = Input->Chunks.AddDefaulted_GetRef();
		Chunk.ChunkZ = Z;
		Chunk.Voxels = Snapshot(Coord);
		const FChunkEditLayer* Layer = EditManager->GetEditLayer(Coord);
		if (Layer && !Layer->IsEmpty() && Layer->ChunkSize == CS)
		{
			Chunk.TopEditedZ.Init(-1, CS * CS);
			for (const TPair<int32, FVoxelEdit>& Edit : Layer->Edits)
			{
				const int32 Index = Edit.Key;
				if (Index < 0 || Index >= VolumeSize)
				{
					continue;
				}
				const int32 Column = Index % (CS * CS);
				const int16 LocalZ = static_cast<int16>(Index / (CS * CS));
				Chunk.TopEditedZ[Column] = FMath::Max(Chunk.TopEditedZ[Column], LocalZ);
			}
		}
		MinZ = FMath::Min(MinZ, Z);
		MaxZ = FMath::Max(MaxZ, Z);
	}

	// Loaded unedited chunks between and just below the edited ones: a dig through the bottom of the edited range
	// meets their real voxels instead of the analytic estimate. Anything else reads as the analytic terrain.
	for (int32 Z = MinZ - 1; Z <= MaxZ; ++Z)
	{
		if (!EditedZs->Contains(Z))
		{
			const FIntVector Coord(TileCoord.X, TileCoord.Y, Z);
			if (TSharedPtr<const TArray<FVoxelData>> Voxels = Snapshot(Coord))
			{
				FVoxelMapOverlayChunk& Chunk = Input->Chunks.AddDefaulted_GetRef();
				Chunk.ChunkZ = Z;
				Chunk.Voxels = MoveTemp(Voxels);
			}
		}
	}
	return Input;
}

TSharedPtr<FVoxelMapPlanetOverlayInput> UVoxelMapSubsystem::GatherPlanetOverlayInput(FIntPoint TileCoord) const
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("map overlay"));
	UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get();
	const UVoxelEditManager* EditManager = CachedEditManagerWeak.Get();
	if (!ChunkMgr || !EditManager)
	{
		return nullptr;
	}

	TSharedPtr<FVoxelMapPlanetOverlayInput> Input = MakeShared<FVoxelMapPlanetOverlayInput>();
	const int32 CS = CachedChunkSize;
	Input->ChunkSize = CS;
	Input->VoxelSize = CachedVoxelSize;
	Input->Origin = CachedWorldOrigin;
	Input->Center = CachedPlanetCenter;
	Input->PlanetRadius = CachedPlanetRadius;
	Input->bUseWater = CachedSea.bEnabled && CachedSea.bRadial;
	Input->WaterRadius = CachedSea.Radius;

	const TArray<FIntVector>* EditedChunks = PlanetTileEditedChunks.Find(PackTileKey(TileCoord));
	if (!EditedChunks)
	{
		return Input;
	}
	const int32 VolumeSize = CS * CS * CS;
	auto Snapshot = [ChunkMgr, VolumeSize](const FIntVector& Coord) -> TSharedPtr<const TArray<FVoxelData>>
	{
		TSharedPtr<const TArray<FVoxelData>> Voxels = ChunkMgr->GetEditMergedSnapshot(Coord);
		return (Voxels.IsValid() && Voxels->Num() == VolumeSize) ? Voxels : nullptr;
	};

	// The edited chunks: their shared edit-merged snapshot (if loaded) and which voxels carry an edit.
	TSet<FIntVector> Listed;
	for (const FIntVector& Coord : *EditedChunks)
	{
		FVoxelMapPlanetOverlayChunk& Chunk = Input->Chunks.AddDefaulted_GetRef();
		Chunk.Coord = Coord;
		Chunk.Voxels = Snapshot(Coord);
		const FChunkEditLayer* Layer = EditManager->GetEditLayer(Coord);
		if (Layer && !Layer->IsEmpty() && Layer->ChunkSize == CS)
		{
			TSharedPtr<TBitArray<>> Bits = MakeShared<TBitArray<>>(false, VolumeSize);
			for (const TPair<int32, FVoxelEdit>& Edit : Layer->Edits)
			{
				if (Edit.Key >= 0 && Edit.Key < VolumeSize)
				{
					(*Bits)[Edit.Key] = true;
				}
			}
			Chunk.EditedVoxels = Bits;
		}
		Listed.Add(Coord);
	}

	// Loaded unedited chunks around them: a pixel's line leaves an edited chunk through its neighbours. Anything else reads
	// as the analytic terrain.
	for (const FIntVector& Coord : *EditedChunks)
	{
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		{
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const FIntVector Neighbour = Coord + FIntVector(DX, DY, DZ);
					if (Listed.Contains(Neighbour))
					{
						continue;
					}
					Listed.Add(Neighbour);
					if (TSharedPtr<const TArray<FVoxelData>> Voxels = Snapshot(Neighbour))
					{
						FVoxelMapPlanetOverlayChunk& Chunk = Input->Chunks.AddDefaulted_GetRef();
						Chunk.Coord = Neighbour;
						Chunk.Voxels = MoveTemp(Voxels);
					}
				}
			}
		}
	}
	return Input;
}

// ---------------------------------------------------------------------------
// Async Tile Generation
// ---------------------------------------------------------------------------

void UVoxelMapSubsystem::QueueTileGeneration(FIntPoint TileCoord, bool bInvalidate, bool bPriority)
{
	if (bShuttingDown)
	{
		return;
	}
	// The queue keeps the tile until a slot frees; every completion drains it (LaunchWaitingTiles), so a tile queued
	// while all slots are busy is never stranded (R22: a cached tile re-queued then used to stay pending for good).
	TileQueue.Request(PackTileKey(TileCoord), bInvalidate, bPriority);
	LaunchWaitingTiles();
}

void UVoxelMapSubsystem::LaunchWaitingTiles()
{
	// At most one overlay rebuild gathers chunk data per frame: a gather shares snapshots and walks edit layers on the
	// game thread (0.1-2.5 ms per tile measured in VoxelDemo), and four in one frame would hitch. A skipped rebuild
	// keeps waiting; a next-tick retry starts it (a completion cannot be relied on: it may land in this same frame).
	bool bDeferredRebuild = false;
	auto CanStart = [this, &bDeferredRebuild](uint64 Key)
	{
		if (LastOverlayGatherFrame == GFrameCounter && OverlayRebuildTiles.Contains(Key))
		{
			bDeferredRebuild = true;
			return false;
		}
		return true;
	};
	uint64 Key = 0;
	uint32 Generation = 0;
	while (!bShuttingDown && TileQueue.NumDrawing() < MaxConcurrentTileGenTasks && TileQueue.StartNext(Key, Generation, CanStart))
	{
		GenerateTileAsync(UnpackTileKey(Key), Generation);
	}
	if (bDeferredRebuild && !bShuttingDown)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &UVoxelMapSubsystem::LaunchWaitingTiles));
		}
	}
}

void UVoxelMapSubsystem::GenerateTileAsync(FIntPoint TileCoord, uint32 Generation)
{
	if (PlanetGrid.IsValid())
	{
		GeneratePlanetTileAsync(TileCoord, Generation);
		return;
	}
	const uint64 TileKey = PackTileKey(TileCoord);

	// Capture all values needed on the background thread by value. The world mode is OUR standalone
	// instance shared into the task: the task keeps it alive for however long it runs, so PIE
	// teardown order cannot invalidate it. (Capturing the chunk manager's raw pointer here is what
	// crashed on PIE stop — EndPlay->Shutdown() freed it under running tasks.)
	TSharedPtr<const IVoxelWorldMode> WorldMode = CachedWorldMode;
	if (!WorldMode)
	{
		// Drop the request cleanly; the tile is queued again by the next exploration or chunk event.
		TileQueue.Cancel(TileKey);
		return;
	}

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

	// Edit overlays (R22): this tile's and its eight neighbours' (index (DY + 1) * 3 + DX + 1; the hillshade apron
	// reads their border columns), plus, when the tile is due an overlay rebuild, its chunk stack gathered now.
	TArray<TSharedPtr<const FVoxelMapEditOverlay>, TFixedAllocator<9>> TileOverlays;
	TileOverlays.SetNum(9);
	TSharedPtr<FVoxelMapOverlayBuildInput> RebuildInput;
	for (int32 DY = -1; DY <= 1; ++DY)
	{
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			TileOverlays[(DY + 1) * 3 + DX + 1] = EditOverlays.FindRef(PackTileKey(TileCoord + FIntPoint(DX, DY)));
		}
	}
	if (OverlayRebuildTiles.Remove(TileKey) > 0)
	{
		const double GatherStart = FPlatformTime::Seconds();
		LastOverlayGatherFrame = GFrameCounter;
		RebuildInput = GatherOverlayInput(TileCoord);
		UE_LOG(LogVoxelMap, Verbose, TEXT("Overlay gather (%d,%d): %d chunk(s) in %.3f ms on the game thread."),
			TileCoord.X, TileCoord.Y, RebuildInput.IsValid() ? RebuildInput->Chunks.Num() : 0,
			(FPlatformTime::Seconds() - GatherStart) * 1000.0);
	}
	const bool bRebuild = RebuildInput.IsValid();

	// Use a weak pointer to safely call back to the subsystem
	TWeakObjectPtr<UVoxelMapSubsystem> WeakThis(this);

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[WeakThis, TileCoord, WorldMode, NoiseParams, ChunkSize, VoxelSize, WorldOrigin,
		 bUseBiomes, bUseWater, WaterLevel, BiomeSnapshot,
		 FallbackTempNoiseParams, FallbackMoistureNoiseParams,
		 MaterialPalette = MoveTemp(MaterialPalette), TerrainMinHeight, TerrainMaxHeight,
		 Zones = MoveTemp(Zones), Generation, TileOverlays, RebuildInput, bRebuild]() mutable
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

		// Surface material — the SAME shared pipeline as chunk generation, tree placement and
		// PCG (temperature/moisture/continentalness noise -> tiered biome blend -> blended
		// material -> priority-sorted height rules), via FVoxelSurfaceQuery + the snapshot.
		auto AnalyticMaterialAt = [&](float WorldX, float WorldY, float Height) -> uint8
		{
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
				MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, 0.0f,
					FVoxelBlendDitherKey(FVoxelBiomeBlendDither::KeyAt(FVector(WorldX, WorldY, Height), VoxelSize), NoiseParams.Seed));
			}
			else
			{
				// Legacy: use world mode's hardcoded material
				MaterialID = WorldMode->GetMaterialAtDepth(
					FVector(WorldX, WorldY, Height), Height, 0.0f);
			}
			return MaterialID;
		};
		auto PixelWorldX = [&](int32 PX) { return static_cast<float>((TileCoord.X * ChunkSize + PX) * VoxelSize + WorldOrigin.X); };
		auto PixelWorldY = [&](int32 PY) { return static_cast<float>((TileCoord.Y * ChunkSize + PY) * VoxelSize + WorldOrigin.Y); };

		// The analytic surface of the tile itself: the overlay build compares against it, and the game thread keeps
		// its range to decide which edited chunks an overlay rebuild needs loaded.
		TArray<float> AnalyticHeights;
		AnalyticHeights.SetNumUninitialized(Resolution * Resolution);
		float AnalyticMin = TNumericLimits<float>::Max();
		float AnalyticMax = TNumericLimits<float>::Lowest();
		for (int32 PY = 0; PY < Resolution; ++PY)
		{
			for (int32 PX = 0; PX < Resolution; ++PX)
			{
				const float Height = Heights[(PY + 1) * GridSize + PX + 1];
				AnalyticHeights[PY * Resolution + PX] = Height;
				AnalyticMin = FMath::Min(AnalyticMin, Height);
				AnalyticMax = FMath::Max(AnalyticMax, Height);
			}
		}

		// --- Edit overlay (R22) ---
		// Rebuild from the gathered chunk stack when due; columns that wait on an unloaded chunk keep their previous
		// value. Then draw edited columns at their edited height, material and water instead of the analytic ones,
		// including the apron cells that belong to neighbouring tiles' overlays.
		bool bOverlayPending = false;
		if (RebuildInput.IsValid())
		{
			const FVoxelMapEditOverlay Built = FVoxelMapEditOverlay::Build(*RebuildInput, AnalyticHeights,
				[&](int32 PX, int32 PY)
				{
					return AnalyticMaterialAt(PixelWorldX(PX), PixelWorldY(PY), AnalyticHeights[PY * Resolution + PX]);
				});
			bOverlayPending = Built.PendingColumns.Num() > 0;
			FVoxelMapEditOverlay Merged = FVoxelMapEditOverlay::MergePending(Built, TileOverlays[4].Get());
			Merged.PendingColumns.Empty();
			TileOverlays[4].Reset();
			if (!Merged.IsEmpty())
			{
				TileOverlays[4] = MakeShared<const FVoxelMapEditOverlay>(MoveTemp(Merged));
			}
			RebuildInput.Reset(); // release the snapshots before drawing
		}
		const TSharedPtr<const FVoxelMapEditOverlay> Overlay = TileOverlays[4];

		TArray<int32> OverlaySlot;
		OverlaySlot.Init(INDEX_NONE, Resolution * Resolution);
		for (int32 GY = 0; GY < GridSize; ++GY)
		{
			for (int32 GX = 0; GX < GridSize; ++GX)
			{
				const int32 PX = GX - 1;
				const int32 PY = GY - 1;
				const int32 DX = PX < 0 ? -1 : (PX >= Resolution ? 1 : 0);
				const int32 DY = PY < 0 ? -1 : (PY >= Resolution ? 1 : 0);
				const FVoxelMapEditOverlay* Source = TileOverlays[(DY + 1) * 3 + DX + 1].Get();
				if (!Source || Source->Resolution != Resolution)
				{
					continue;
				}
				const int32 Slot = Source->FindColumn(PX - DX * Resolution, PY - DY * Resolution);
				if (Slot != INDEX_NONE)
				{
					Heights[GY * GridSize + GX] = Source->Heights[Slot];
					if (DX == 0 && DY == 0)
					{
						OverlaySlot[PY * Resolution + PX] = Slot;
					}
				}
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
				const int32 GX = PX + 1;
				const int32 GY = PY + 1;
				const float Height = Heights[GY * GridSize + GX];

				// Edited columns carry their own material and water: water there comes from the voxels (a pit dug
				// below the water line stays land until water floods it), not from the analytic height test.
				uint8 MaterialID = 0;
				bool bWater = false;
				const int32 Slot = OverlaySlot[PY * Resolution + PX];
				if (Slot != INDEX_NONE)
				{
					MaterialID = Overlay->Materials[Slot];
					bWater = (Overlay->Flags[Slot] & FVoxelMapEditOverlay::FlagWater) != 0;
				}
				else
				{
					MaterialID = AnalyticMaterialAt(PixelWorldX(PX), PixelWorldY(PY), Height);
					bWater = bUseWater && Height < WaterLevel;
				}

				FColor Color;

				if (bWater)
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
		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, TileCoord, PixelData = MoveTemp(PixelData), Resolution, Generation, bRebuild, Overlay,
			 bOverlayPending, AnalyticMin, AnalyticMax]() mutable
		{
			if (UVoxelMapSubsystem* Self = WeakThis.Get())
			{
				Self->FinishTileDrawing(TileCoord, MoveTemp(PixelData), Resolution, Generation, bRebuild, Overlay, bOverlayPending,
					AnalyticMin, AnalyticMax);
			}
		});
	});
}

void UVoxelMapSubsystem::FinishTileDrawing(FIntPoint TileCoord, TArray<FColor>&& PixelData, int32 Resolution, uint32 Generation,
	bool bRebuild, const TSharedPtr<const FVoxelMapEditOverlay>& Overlay, bool bOverlayPending, float AnalyticMin, float AnalyticMax)
{
	if (bShuttingDown)
	{
		// Deinitialize() already ran (PIE stop / world teardown): don't resurrect cache
		// state, don't broadcast into a dying world, and don't start new tasks.
		UE_LOG(LogVoxelMap, Verbose, TEXT("Discarded tile (%d,%d) completion after shutdown"),
			TileCoord.X, TileCoord.Y);
		return;
	}

	const uint64 Key = PackTileKey(TileCoord);

	if (!TileQueue.Complete(Key, Generation))
	{
		// Invalidated while drawing (an edit refresh or a conditioning change): this result is stale. The tile
		// is waiting again and its cached image stays until the redraw lands; a discarded overlay rebuild runs
		// again with it.
		if (bRebuild)
		{
			OverlayRebuildTiles.Add(Key);
		}
	}
	else
	{
		if (bRebuild)
		{
			const TSharedPtr<const FVoxelMapEditOverlay> Previous = EditOverlays.FindRef(Key);
			if (Overlay.IsValid())
			{
				EditOverlays.Add(Key, Overlay);
			}
			else
			{
				EditOverlays.Remove(Key);
			}
			if (bOverlayPending)
			{
				OverlayAwaitingTiles.Add(Key);
			}

			// Neighbours draw this tile's border columns into their hillshade apron: redraw the cached ones
			// when those changed, so shading stays continuous across the tile edge.
			if (!FVoxelMapEditOverlay::BordersEqual(Previous.Get(), Overlay.Get()))
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const FIntPoint Neighbour = GetNeighbourTile(TileCoord, DX, DY);
						if ((DX != 0 || DY != 0) && Neighbour != TileCoord && TileStore.Contains(PackTileKey(Neighbour)))
						{
							TileQueue.Request(PackTileKey(Neighbour), /*bInvalidate*/ true, /*bPriority*/ true);
						}
					}
				}
			}
		}

		// The tile's analytic range decides which edited chunks a rebuild needs; an overlay that waited
		// without it may be able to rebuild now.
		const bool bRangeWasKnown = TileAnalyticRange.Contains(Key);
		TileAnalyticRange.Add(Key, FVector2f(AnalyticMin, AnalyticMax));
		if (!bRangeWasKnown && OverlayAwaitingTiles.Contains(Key))
		{
			MarkOverlayDirty(Key);
		}

		// Keep the detail image where something asked for it (the minimap's area, a zoomed-in world map) or the
		// tile already had it; elsewhere only the overview (WM-E2). The store trims to its budgets. An unlimited
		// detail budget (0) keeps every detail image, as the map did before WM-E2.
		const bool bKeepDetail = DetailWanted.Remove(Key) > 0 || TileStore.HasDetail(Key)
			|| TileStore.GetDetailBudget() == 0;
		{
			FScopeLock Lock(&TileMutex);
			TileStore.Store(Key, TileCoord, MoveTemp(PixelData), Resolution, bKeepDetail);
		}

		OnMapTileReady.Broadcast(TileCoord);
	}

	// Every completion drains the queue, so tiles queued while all slots were busy start now.
	LaunchWaitingTiles();
}

void UVoxelMapSubsystem::GeneratePlanetTileAsync(FIntPoint TileCoord, uint32 Generation)
{
	const uint64 TileKey = PackTileKey(TileCoord);
	TSharedPtr<const IVoxelWorldMode> WorldMode = CachedWorldMode;
	if (!WorldMode || WorldMode->GetWorldModeType() != EWorldMode::SphericalPlanet || !PlanetGrid.IsValidCell(TileCoord))
	{
		TileQueue.Cancel(TileKey);
		return;
	}

	// Everything the worker needs, by value (as GenerateTileAsync): the world mode is our own instance, shared into the task.
	const FVoxelCubeSphereGrid Grid = PlanetGrid;
	const FVoxelNoiseParams NoiseParams = CachedNoiseParams;
	const int32 ChunkSize = CachedChunkSize;
	const float VoxelSize = CachedVoxelSize;
	const FVector Center = CachedPlanetCenter;
	const double Radius = CachedPlanetRadius;
	const FVoxelSeaSurface Sea = CachedSea;
	const bool bUseBiomes = bBiomesEnabled;
	TArray<FColor> MaterialPalette = CachedMaterialPalette;
	const float AltitudeMin = CachedAltitudeMin;
	const float AltitudeMax = CachedAltitudeMax;
	const FVoxelBiomeSnapshot BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(CachedBiomeConfig);

	// Fallback climate noise for "biomes on, but no valid config", as the planet generator's.
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

	// Edit overlays: this tile's, its eight neighbours' (the hillshade apron reads their border pixels; across a face edge
	// the neighbour is turned, so apron pixels are found by direction), and the rebuild input when one is due.
	TSharedPtr<const FVoxelMapEditOverlay> OwnOverlay = EditOverlays.FindRef(TileKey);
	TArray<TPair<FIntPoint, TSharedPtr<const FVoxelMapEditOverlay>>, TInlineAllocator<8>> NeighbourOverlays;
	TSharedPtr<FVoxelMapPlanetOverlayInput> RebuildInput;
	for (int32 DY = -1; DY <= 1; ++DY)
	{
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			const FIntPoint Neighbour = GetNeighbourTile(TileCoord, DX, DY);
			if ((DX != 0 || DY != 0) && Neighbour != TileCoord)
			{
				if (TSharedPtr<const FVoxelMapEditOverlay> Found = EditOverlays.FindRef(PackTileKey(Neighbour)))
				{
					NeighbourOverlays.Add({ Neighbour, Found });
				}
			}
		}
	}
	if (OverlayRebuildTiles.Remove(TileKey) > 0)
	{
		const double GatherStart = FPlatformTime::Seconds();
		LastOverlayGatherFrame = GFrameCounter;
		RebuildInput = GatherPlanetOverlayInput(TileCoord);
		UE_LOG(LogVoxelMap, Verbose, TEXT("Planet overlay gather (%d,%d): %d chunk(s) in %.3f ms on the game thread."),
			TileCoord.X, TileCoord.Y, RebuildInput.IsValid() ? RebuildInput->Chunks.Num() : 0,
			(FPlatformTime::Seconds() - GatherStart) * 1000.0);
	}
	const bool bRebuild = RebuildInput.IsValid();

	// Planet conditioning over the tile and its apron (POI pads, spherical SP-B10): generation layers the zones onto the
	// radial height, so the map must too (QA W-05). The tile's cap: its centre's direction and 1.5 tile widths of angle.
	// Gathered here on the game thread; plain data.
	TArray<FVoxelConditioningZone> Zones;
	if (UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get())
	{
		ChunkMgr->GatherConditioningZonesOnSphere(Grid.CellCentreDirection(TileCoord), 1.5 * GetTileWorldSize() / FMath::Max(Radius, 1.0), Zones);
	}

	TWeakObjectPtr<UVoxelMapSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[WeakThis, TileCoord, Generation, WorldMode, Grid, NoiseParams, ChunkSize, VoxelSize, Center, Radius, Sea, bUseBiomes,
		 MaterialPalette = MoveTemp(MaterialPalette), AltitudeMin, AltitudeMax, BiomeSnapshot,
		 FallbackTempNoiseParams, FallbackMoistureNoiseParams, OwnOverlay, NeighbourOverlays, RebuildInput, bRebuild,
		 Zones = MoveTemp(Zones)]() mutable
	{
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*WorldMode);
		const int32 Resolution = ChunkSize;
		const int32 GridSize = Resolution + 2;
		const int32 NumPixels = Resolution * Resolution;

		// --- Pass 1: the direction through each pixel's centre and the terrain's altitude along it, with a one-pixel apron
		// (the hillshade's neighbours; past a face edge the face's plane extended, which is the neighbouring face's ground).
		TArray<FVector> Directions;
		TArray<float> Altitudes;
		Directions.SetNumUninitialized(GridSize * GridSize);
		Altitudes.SetNumUninitialized(GridSize * GridSize);
		for (int32 GY = 0; GY < GridSize; ++GY)
		{
			for (int32 GX = 0; GX < GridSize; ++GX)
			{
				const FVector Direction = Grid.DirectionAt(TileCoord, (GX - 0.5) / Resolution, (GY - 0.5) / Resolution);
				Directions[GY * GridSize + GX] = Direction;
				Altitudes[GY * GridSize + GX] = Planet.GetConditionedTerrainAltitude(Direction, NoiseParams, Zones);
			}
		}

		// Surface material as the planet generator writes it at the surface voxel (QueryPlanetSurfaceConditions).
		auto AnalyticMaterialAt = [&](const FVector& Direction, float Altitude) -> uint8
		{
			const FVector Position = Center + Direction * (Radius + Altitude);
			uint8 MaterialID = 0;
			if (bUseBiomes && BiomeSnapshot.bIsValid)
			{
				uint8 BiomeID = 0;
				FVoxelSurfaceQuery::QueryPlanetSurfaceConditions(Planet, Position, VoxelSize, NoiseParams, BiomeSnapshot, Sea, MaterialID, BiomeID, &Zones);
			}
			else if (bUseBiomes)
			{
				// The planet generator's climate: the planet noise on the datum sphere (WM-BP5)
				float Temperature, Moisture;
				FSphericalPlanetWorldMode::SampleClimate(Direction, Radius, FallbackTempNoiseParams, FallbackMoistureNoiseParams, Temperature, Moisture);
				const FBiomeBlend Blend = FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f);
				MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, 0.0f,
					FVoxelBlendDitherKey(FVoxelBiomeBlendDither::KeyAt(Position, VoxelSize), NoiseParams.Seed));
			}
			else
			{
				MaterialID = Planet.GetMaterialAtDepth(Position, static_cast<float>(Radius + Altitude), 0.0f);
			}
			return MaterialID;
		};

		// The tile's own pixels: the overlay build reads them, and the game thread keeps their altitude range.
		TArray<FVector> PixelDirections;
		TArray<float> AnalyticAltitudes;
		PixelDirections.SetNumUninitialized(NumPixels);
		AnalyticAltitudes.SetNumUninitialized(NumPixels);
		float AnalyticMin = TNumericLimits<float>::Max();
		float AnalyticMax = TNumericLimits<float>::Lowest();
		for (int32 PY = 0; PY < Resolution; ++PY)
		{
			for (int32 PX = 0; PX < Resolution; ++PX)
			{
				const int32 G = (PY + 1) * GridSize + PX + 1;
				PixelDirections[PY * Resolution + PX] = Directions[G];
				AnalyticAltitudes[PY * Resolution + PX] = Altitudes[G];
				AnalyticMin = FMath::Min(AnalyticMin, Altitudes[G]);
				AnalyticMax = FMath::Max(AnalyticMax, Altitudes[G]);
			}
		}

		// --- Edit overlay: rebuilt along each pixel's line when due, as on the plane.
		bool bOverlayPending = false;
		if (RebuildInput.IsValid())
		{
			const FVoxelMapEditOverlay Built = FVoxelMapEditOverlay::BuildPlanet(*RebuildInput, PixelDirections, AnalyticAltitudes,
				[&](int32 PX, int32 PY)
				{
					const int32 Index = PY * Resolution + PX;
					return AnalyticMaterialAt(PixelDirections[Index], AnalyticAltitudes[Index]);
				});
			bOverlayPending = Built.PendingColumns.Num() > 0;
			const FVoxelMapEditOverlay* Previous = OwnOverlay.Get();
			FVoxelMapEditOverlay Merged = FVoxelMapEditOverlay::MergePending(Built, Previous);
			Merged.PendingColumns.Empty();
			OwnOverlay.Reset();
			if (!Merged.IsEmpty())
			{
				OwnOverlay = MakeShared<const FVoxelMapEditOverlay>(MoveTemp(Merged));
			}
			RebuildInput.Reset();
		}
		const TSharedPtr<const FVoxelMapEditOverlay> Overlay = OwnOverlay;
		auto UsableOverlay = [Resolution](const FVoxelMapEditOverlay* Candidate)
		{
			return Candidate && Candidate->bAltitudeHeights && Candidate->Resolution == Resolution;
		};

		// Edited altitudes into the grid: the tile's own pixels from its overlay, the apron's from whichever neighbour holds
		// the apron pixel's direction.
		TArray<int32> OverlaySlot;
		OverlaySlot.Init(INDEX_NONE, NumPixels);
		for (int32 GY = 0; GY < GridSize; ++GY)
		{
			for (int32 GX = 0; GX < GridSize; ++GX)
			{
				const int32 G = GY * GridSize + GX;
				const bool bInterior = GX >= 1 && GY >= 1 && GX <= Resolution && GY <= Resolution;
				const FVoxelMapEditOverlay* Source = nullptr;
				int32 PX = GX - 1;
				int32 PY = GY - 1;
				if (bInterior)
				{
					Source = UsableOverlay(Overlay.Get()) ? Overlay.Get() : nullptr;
				}
				else if (NeighbourOverlays.Num() > 0)
				{
					FVector2D Fraction;
					const FIntPoint Cell = Grid.CellOfDirection(Directions[G], &Fraction);
					for (const TPair<FIntPoint, TSharedPtr<const FVoxelMapEditOverlay>>& Pair : NeighbourOverlays)
					{
						if (Pair.Key == Cell && UsableOverlay(Pair.Value.Get()))
						{
							Source = Pair.Value.Get();
							PX = FMath::Clamp(FMath::FloorToInt32(Fraction.X * Resolution), 0, Resolution - 1);
							PY = FMath::Clamp(FMath::FloorToInt32(Fraction.Y * Resolution), 0, Resolution - 1);
							break;
						}
					}
				}
				if (!Source)
				{
					continue;
				}
				const int32 Slot = Source->FindColumn(PX, PY);
				if (Slot != INDEX_NONE)
				{
					Altitudes[G] = Source->Heights[Slot];
					if (bInterior)
					{
						OverlaySlot[PY * Resolution + PX] = Slot;
					}
				}
			}
		}

		// Shading ranges from the terrain's altitude bounds: land from the sea (or the lowest terrain) to the highest, water
		// depth from the sea down to the lowest.
		const bool bSeaOn = Sea.bEnabled && Sea.bRadial;
		const float SeaAltitude = bSeaOn ? static_cast<float>(Sea.Radius - Radius) : AltitudeMin;
		const float LandBase = bSeaOn ? SeaAltitude : AltitudeMin;
		const float LandSpan = FMath::Max(AltitudeMax - LandBase, 1.0f);
		const float DepthSpan = FMath::Max(LandBase - AltitudeMin, 1.0f);
		auto PositionAt = [&](int32 G)
		{
			return Center + Directions[G] * (Radius + Altitudes[G]);
		};

		// --- Pass 2: colour, as the plane's in the local frame.
		TArray<FColor> PixelData;
		PixelData.SetNumUninitialized(NumPixels);
		for (int32 PY = 0; PY < Resolution; ++PY)
		{
			for (int32 PX = 0; PX < Resolution; ++PX)
			{
				const int32 GX = PX + 1;
				const int32 GY = PY + 1;
				const int32 G = GY * GridSize + GX;
				const float Altitude = Altitudes[G];

				uint8 MaterialID = 0;
				bool bWater = false;
				const int32 Slot = OverlaySlot[PY * Resolution + PX];
				if (Slot != INDEX_NONE)
				{
					MaterialID = Overlay->Materials[Slot];
					bWater = (Overlay->Flags[Slot] & FVoxelMapEditOverlay::FlagWater) != 0;
				}
				else
				{
					MaterialID = AnalyticMaterialAt(Directions[G], Altitude);
					bWater = bSeaOn && Radius + Altitude < Sea.Radius;
				}

				FColor Color;
				if (bWater)
				{
					const float Depth = SeaAltitude - Altitude;
					const float DepthFactor = FMath::Lerp(1.0f, 0.3f, FMath::Clamp(Depth / DepthSpan, 0.0f, 1.0f));
					Color.R = FMath::Clamp(static_cast<int32>(20 * DepthFactor), 0, 255);
					Color.G = FMath::Clamp(static_cast<int32>(80 * DepthFactor), 0, 255);
					Color.B = FMath::Clamp(static_cast<int32>(180 * DepthFactor), 0, 255);
					Color.A = 255;
				}
				else
				{
					Color = MaterialPalette[MaterialID];
					const float ElevationFactor = FMath::Clamp((Altitude - LandBase) / LandSpan, 0.0f, 1.0f);
					const float Elevation = FMath::Lerp(0.75f, 1.0f, ElevationFactor);

					// Hillshade: the surface normal from the neighbouring pixels' ground points, lit by the plane's NW key
					// light in the local frame (north toward the +Z pole).
					const FVector Up = Directions[G];
					FVector Normal = FVector::CrossProduct(PositionAt(G + 1) - PositionAt(G - 1), PositionAt(G + GridSize) - PositionAt(G - GridSize)).GetSafeNormal();
					if (Normal.IsNearlyZero())
					{
						Normal = Up;
					}
					else if (FVector::DotProduct(Normal, Up) < 0.0)
					{
						Normal = -Normal;
					}
					FVector East, North;
					FVoxelCubeSphere::GetTangentFrame(Up, East, North);
					const FVector LightDir = (North * -0.707 + East * -0.707 + Up).GetSafeNormal();
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

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, TileCoord, PixelData = MoveTemp(PixelData), Resolution, Generation, bRebuild, Overlay,
			 bOverlayPending, AnalyticMin, AnalyticMax]() mutable
		{
			if (UVoxelMapSubsystem* Self = WeakThis.Get())
			{
				Self->FinishTileDrawing(TileCoord, MoveTemp(PixelData), Resolution, Generation, bRebuild, Overlay, bOverlayPending,
					AnalyticMin, AnalyticMax);
			}
		});
	});
}
