// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapSubsystem.h"
#include "VoxelMap.h"
#include "VoxelChunkManager.h"
#include "VoxelSeaSurface.h"
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
#include "VoxelFarField.h"
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
#include "ProfilingDebugging/CpuProfilerTrace.h"

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
	TEXT("its detail image. 32 MB holds 2,048 ")
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
	TEXT("voxel.Map.ExploreAll marks at most this many tiles explored, nearest the pawn first, in every world mode (a ")
	TEXT("planet's atlas is 6 N^2 tiles: 40,000 on a 2.5 km planet, 16 million at 50 km, 6.4 billion at 1,000 km; ")
	TEXT("a plane's are unbounded)."),
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
		TEXT("Log the map's tile, queue, drawing and edit-overlay state. voxel.Map.Stats reset logs it, then restarts the ")
		TEXT("drawing counters (drawn, replaced, image requests, the rate)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UWorld* Target = FindPlayWorld(World);
			UVoxelMapSubsystem* Map = Target ? Target->GetSubsystem<UVoxelMapSubsystem>() : nullptr;
			if (!Map)
			{
				UE_LOG(LogVoxelMap, Warning, TEXT("voxel.Map.Stats: no PIE/Game world with a map subsystem (start PIE first)"));
				return;
			}
			UE_LOG(LogVoxelMap, Log, TEXT("voxel.Map.Stats: %s"), *Map->GetDebugStats());
			if (Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase))
			{
				Map->ResetDebugStats();
			}
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
			const double Start = FPlatformTime::Seconds();
			const int64 Count = Map->ExploreAllTiles(Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector, Radius);
			UE_LOG(LogVoxelMap, Log, TEXT("voxel.Map.ExploreAll: %lld tile(s) explored in %.1f ms on the game thread; %s"), Count,
				(FPlatformTime::Seconds() - Start) * 1000.0, *Map->GetDebugStats());
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
	 * Documentation/Research/Archive/MAP_ACCURACY_PLAN.md).
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
	ResetDebugStats();

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
	TileEditedChunks.Empty();
	OverlayDirtyTiles.Empty();
	OverlayAwaitingTiles.Empty();
	OverlayRebuildTiles.Empty();
	TileAnalyticRange.Empty();

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

	CachedNoiseParams = Config->GetNoiseParams();
	CachedChunkSize = Config->ChunkSize;
	CachedVoxelSize = Config->VoxelSize;
	CachedWorldOrigin = Config->WorldOrigin;
	bBiomesEnabled = Config->bEnableBiomes;
	CachedBiomeConfig = Config->BiomeConfiguration;

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

	// The tile frame (code review CR2, REND-15): the chunk grid, the sea and, on a planet, its tile grid (cells of the
	// cube-sphere grid, about a chunk across; spherical Phase 4, WM-SY8).
	TileFrame = FVoxelMapTileFrame();
	TileFrame.ChunkSize = CachedChunkSize;
	TileFrame.VoxelSize = CachedVoxelSize;
	TileFrame.WorldOrigin = CachedWorldOrigin;
	TileFrame.Sea = FVoxelSeaSurface::FromConfig(*Config);
	TileFrame.bPlanet = CachedWorldMode->GetWorldModeType() == EWorldMode::SphericalPlanet;

	// Elevation/depth shading ranges from the world mode's own height bounds (the generation
	// authority) instead of hardcoded constants, so the gradient spans the terrain this config
	// actually produces.
	{
		const FVoxelWorldBounds HeightBounds = CachedWorldMode->GetWorldBounds();
		TileFrame.MinAltitude = HeightBounds.MinZ;
		TileFrame.MaxAltitude = HeightBounds.MaxZ;
	}

	// Modes without real bounds (IVoxelWorldMode's default is a deliberately huge no-cull range,
	// e.g. SphericalPlanet) would flatten the gradient to a constant. Fall back to the config's own
	// noise envelope so shading still spans something meaningful.
	if (!FMath::IsFinite(TileFrame.MinAltitude) || !FMath::IsFinite(TileFrame.MaxAltitude) ||
		(TileFrame.MaxAltitude - TileFrame.MinAltitude) > 1.0e6f)
	{
		const float Center = Config->SeaLevel + Config->BaseHeight;
		TileFrame.MinAltitude = Center - Config->HeightScale;
		TileFrame.MaxAltitude = Center + Config->HeightScale;
	}

	// The world's edge: exploration stops there (WM-I2). Tiles a save restored before the world resolved are
	// checked now.
	CachedWorldBounds = CachedWorldMode->GetWorldBounds();
	TileFrame.Bounds = CachedWorldBounds;

	if (TileFrame.bPlanet)
	{
		const FSphericalPlanetParams& Planet = static_cast<const FSphericalPlanetWorldMode&>(*CachedWorldMode).GetPlanetParams();
		TileFrame.Center = Planet.PlanetCenter;
		TileFrame.Radius = Planet.PlanetRadius;
		TileFrame.Grid = FVoxelCubeSphereGrid(FVoxelCubeSphereGrid::CellsPerEdgeFor(TileFrame.Radius, static_cast<double>(CachedChunkSize) * CachedVoxelSize));
		// The terrain's altitude extent (IVoxelWorldMode::GetAltitudeBounds): the planet's elevation and depth shading.
		CachedWorldMode->GetAltitudeBounds(TileFrame.MinAltitude, TileFrame.MaxAltitude);
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
		CachedChunkSize, CachedVoxelSize, TileFrame.bPlanet
			? *FString::Printf(TEXT(", planet: %d tiles a face edge (%lld tiles, about %.0f uu each)"), TileFrame.Grid.GetCellsPerEdge(),
				TileFrame.Grid.GetNumCells(), GetTileWorldSize())
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
		++DrawStats.ImageRequests;
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
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_ExploreAll);
	if (bShuttingDown || !ResolveChunkManager())
	{
		return 0;
	}
	const double Size = GetTileWorldSize();
	if (Size <= 0.0)
	{
		return 0;
	}
	if (TileFrame.bPlanet)
	{
		// A planet: every tile of the atlas, up to the cap (spherical Phase 6, WM-BP11): 6 N^2 overflowed an int32 from
		// about 50 million tiles, and a 1,000 km planet has 6.4 billion. Past the cap the tiles nearest Center, and the
		// images of the nearest PlanetExploreAllImageTiles queued (the rest are drawn when the map shows them).
		const FVoxelCubeSphereGrid& PlanetGrid = TileFrame.Grid;
		const int64 N = PlanetGrid.GetCellsPerEdge();
		const int64 Total = 6 * N * N;
		const int64 Cap = FMath::Max<int64>(1, CVarMapExploreAllMaxTiles.GetValueOnGameThread());
		FVector Direction = (Center - TileFrame.Center).GetSafeNormal();
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

bool UVoxelMapSubsystem::IsTileExplored(FIntPoint TileCoord) const
{
	return ExploredTiles.Contains(PackTileKey(TileCoord));
}

bool UVoxelMapSubsystem::IsTileInsideWorld(FIntPoint TileCoord) const
{
	return TileFrame.IsValidTile(TileCoord);
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

// ---------------------------------------------------------------------------
// Coordinate Helpers
// ---------------------------------------------------------------------------

FIntPoint UVoxelMapSubsystem::WorldToTileCoord(const FVector& WorldPos) const
{
	return TileFrame.TileOf(WorldPos);
}

FVector UVoxelMapSubsystem::TileCoordToWorld(FIntPoint TileCoord) const
{
	return TileFrame.TileOrigin(TileCoord);
}

float UVoxelMapSubsystem::GetTileWorldSize() const
{
	return static_cast<float>(TileFrame.GetTileWorldSize());
}

bool UVoxelMapSubsystem::PlanetDirectionToTilePixel(const FVector& Direction, FIntPoint& OutTile, int32& OutPX, int32& OutPY) const
{
	return TileFrame.DirectionToTilePixel(Direction, OutTile, OutPX, OutPY);
}

FIntPoint UVoxelMapSubsystem::GetNeighbourTile(FIntPoint TileCoord, int32 DX, int32 DY) const
{
	return TileFrame.Neighbour(TileCoord, DX, DY);
}

void UVoxelMapSubsystem::GetChunkTiles(const FIntVector& ChunkCoord, TArray<FIntPoint>& OutTiles) const
{
	TileFrame.TilesOfChunk(ChunkCoord, OutTiles);
}

// ---------------------------------------------------------------------------
// Exploration
// ---------------------------------------------------------------------------

void UVoxelMapSubsystem::RequestTilesInRadius(const FVector& WorldPos, float Radius)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_RequestTilesInRadius);
	if (!ResolveChunkManager())
	{
		return;
	}

	const float ChunkWorldSize = CachedChunkSize * CachedVoxelSize;
	if (ChunkWorldSize <= 0.f)
	{
		return;
	}

	// The tiles the radius reaches (inside the world: exploration stops at an island's edge, WM-I2; on a planet the tiles
	// whose centres lie within the radius along the surface), each explored with its detail image: the minimap's area
	// wants it, drawn again if it was evicted (WM-E2).
	TArray<FIntPoint> Tiles;
	TileFrame.TilesWithinRadius(WorldPos, Radius, Tiles);
	for (const FIntPoint& TileCoord : Tiles)
	{
		const uint64 Key = PackTileKey(TileCoord);
		ExploredTiles.Add(Key);
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

// ---------------------------------------------------------------------------
// Event-Driven Generation
// ---------------------------------------------------------------------------

void UVoxelMapSubsystem::OnChunkGenerated(FIntVector ChunkCoord)
{
	if (!ResolveChunkManager())
	{
		return;
	}

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

	// The tiles the chunk spans (its column; on a planet the cells its corners span, spherical Phase 4): each explored
	// (chunks that generate are in the player's vicinity) inside the world only, an island also generating a ring of
	// floor chunks past its edge (WM-I2), with its detail image kept (drawn again if it was evicted).
	TArray<FIntPoint> Tiles;
	GetChunkTiles(ChunkCoord, Tiles);
	for (const FIntPoint& Tile : Tiles)
	{
		const uint64 TileKey = PackTileKey(Tile);
		if (IsTileInsideWorld(Tile))
		{
			ExploredTiles.Add(TileKey);
			if (!TileStore.HasDetail(TileKey))
			{
				DetailWanted.Add(TileKey);
				if (!TileQueue.IsQueued(TileKey))
				{
					QueueTileGeneration(Tile);
				}
			}
		}
		// A tile whose overlay waits for its chunks: this may be one of them.
		if (OverlayAwaitingTiles.Contains(TileKey))
		{
			MarkOverlayDirty(TileKey);
		}
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

void UVoxelMapSubsystem::RedrawAllTiles(bool bRebuildOverlays)
{
	if (bShuttingDown || !ResolveChunkManager())
	{
		return;
	}
	if (bRebuildOverlays)
	{
		for (const TPair<uint64, TArray<FIntVector>>& Pair : TileEditedChunks)
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
	// The drawing since the last reset (the map scaling plan's M0, G163): the rate, the worker time a tile takes, and how
	// many drawings replaced images the store still held or answered a view's requests.
	const double Elapsed = FMath::Max(FPlatformTime::Seconds() - DrawStats.SinceSeconds, 1.0e-3);
	const double MsPerTile = DrawStats.Drawn > 0 ? 1000.0 * DrawStats.DrawSeconds / static_cast<double>(DrawStats.Drawn) : 0.0;
	return FString::Printf(
		TEXT("tiles cached=%d explored=%d (outside the world %d) waiting=%d drawing=%d | drawn=%lld (%.0f/s over %.0f s, %.2f ms a tile on a worker) replaced=%lld image-requests=%lld fog-rebuilds=%lld (mean %.1f ms, max %.1f ms) | images detail=%d %.1f/%.0f MB overview=%d %.1f/%.0f MB evicted detail=%lld overview=%lld | overlays=%d columns=%d bytes=%d edited-tiles=%d dirty=%d awaiting=%d rebuild-queued=%d"),
		TileStore.Num(), ExploredTiles.Num(), ExploredOutside, TileQueue.NumWaiting(), TileQueue.NumDrawing(),
		DrawStats.Drawn, static_cast<double>(DrawStats.Drawn) / Elapsed, Elapsed, MsPerTile, DrawStats.Replaced, DrawStats.ImageRequests,
		DrawStats.FogRebuilds, DrawStats.FogRebuilds > 0 ? 1000.0 * DrawStats.FogSeconds / static_cast<double>(DrawStats.FogRebuilds) : 0.0,
		1000.0 * DrawStats.FogMaxSeconds,
		TileStore.NumDetail(), TileStore.GetDetailBytes() / MB, TileStore.GetDetailBudget() / MB,
		TileStore.Num(), TileStore.GetOverviewBytes() / MB, TileStore.GetOverviewBudget() / MB,
		TileStore.GetDetailEvictions(), TileStore.GetOverviewEvictions(),
		EditOverlays.Num(), MaskedColumns, Bytes, TileEditedChunks.Num(), OverlayDirtyTiles.Num(),
		OverlayAwaitingTiles.Num(), OverlayRebuildTiles.Num());
}

void UVoxelMapSubsystem::ResetDebugStats()
{
	DrawStats = FDrawStats();
	DrawStats.SinceSeconds = FPlatformTime::Seconds();
}

bool UVoxelMapSubsystem::CaptureMapImage(const FVector& Center, int32 RadiusTiles, const FString& FilePath, bool bHighlightOverlay) const
{
	const int32 Res = CachedChunkSize;
	if (!bCacheResolved || Res <= 0 || RadiusTiles < 0 || RadiusTiles > 64 || CachedVoxelSize <= 0.0f)
	{
		return false;
	}
	const int32 Size = (2 * RadiusTiles + 1) * Res;

	if (TileFrame.bPlanet)
	{
		// A planet (spherical Phase 4): the tangent plane at the position, north (toward the +Z pole) up and east right, a
		// texel per tile pixel; each texel shows the explored tile pixel its ground point's direction falls in.
		const FVector Up = FSphericalPlanetWorldMode::GetDirectionFromCenter(Center, TileFrame.Center);
		FVector East, North;
		FVoxelCubeSphere::GetTangentFrame(Up, East, North);
		const double Texel = GetTileWorldSize() / Res;
		const FVector Ground = TileFrame.Center + Up * TileFrame.Radius;
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
				PlanetDirectionToTilePixel((Point - TileFrame.Center).GetSafeNormal(), Tile, PX, PY);
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
	// The chunk on every tile it spans (its column; on a planet the cells its corners span).
	const UVoxelEditManager* EditManager = CachedEditManagerWeak.Get();
	const bool bHasEdits = EditManager && EditManager->ChunkHasEdits(ChunkCoord);
	TArray<FIntPoint> Tiles;
	GetChunkTiles(ChunkCoord, Tiles);
	for (const FIntPoint& Tile : Tiles)
	{
		const uint64 TileKey = PackTileKey(Tile);
		if (bHasEdits)
		{
			TileEditedChunks.FindOrAdd(TileKey).AddUnique(ChunkCoord);
		}
		else if (TArray<FIntVector>* Chunks = TileEditedChunks.Find(TileKey))
		{
			Chunks->Remove(ChunkCoord);
			if (Chunks->Num() == 0)
			{
				TileEditedChunks.Remove(TileKey);
			}
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
	const TArray<FIntVector>* EditedChunks = TileEditedChunks.Find(TileKey);
	if (!EditedChunks)
	{
		// No edits left (undone or cleared): the rebuild clears the overlay without chunk data.
		return true;
	}
	const UVoxelChunkManager* ChunkMgr = CachedChunkManagerWeak.Get();
	if (!ChunkMgr)
	{
		return false;
	}
	// Edited chunks that cannot reach the tile's surface (more than a chunk below its lowest analytic altitude) need not
	// be loaded: dungeon interiors under a loaded entrance.
	const FVector2f* Range = TileAnalyticRange.Find(TileKey);
	for (const FIntVector& Coord : *EditedChunks)
	{
		if (Range && !TileFrame.ChunkMayReachSurface(Coord, Range->X))
		{
			continue;
		}
		if (!ChunkMgr->HasChunkVoxelData(Coord))
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
	Input->bUseWater = TileFrame.Sea.bEnabled;
	Input->WaterLevel = static_cast<float>(TileFrame.Sea.PlaneZ);

	const int32 VolumeSize = CS * CS * CS;
	auto Snapshot = [ChunkMgr, VolumeSize](const FIntVector& Coord) -> TSharedPtr<const TArray<FVoxelData>>
	{
		TSharedPtr<const TArray<FVoxelData>> Voxels = ChunkMgr->GetVoxelSnapshot(Coord);
		return (Voxels.IsValid() && Voxels->Num() == VolumeSize) ? Voxels : nullptr;
	};

	const TArray<FIntVector>* EditedChunks = TileEditedChunks.Find(PackTileKey(TileCoord));
	if (!EditedChunks)
	{
		return Input;
	}
	TArray<int32> EditedZsArray;
	for (const FIntVector& Coord : *EditedChunks)
	{
		EditedZsArray.Add(Coord.Z);
	}
	const TArray<int32>* EditedZs = &EditedZsArray;

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
	Input->Center = TileFrame.Center;
	Input->PlanetRadius = TileFrame.Radius;
	Input->bUseWater = TileFrame.Sea.bEnabled && TileFrame.Sea.bRadial;
	Input->WaterRadius = TileFrame.Sea.Radius;

	const TArray<FIntVector>* EditedChunks = TileEditedChunks.Find(PackTileKey(TileCoord));
	if (!EditedChunks)
	{
		return Input;
	}
	const int32 VolumeSize = CS * CS * CS;
	auto Snapshot = [ChunkMgr, VolumeSize](const FIntVector& Coord) -> TSharedPtr<const TArray<FVoxelData>>
	{
		TSharedPtr<const TArray<FVoxelData>> Voxels = ChunkMgr->GetVoxelSnapshot(Coord);
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
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_LaunchWaitingTiles);
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


FVoxelMapOverlayInputs UVoxelMapSubsystem::GatherOverlayInputs(FIntPoint TileCoord) const
{
	FVoxelMapOverlayInputs Inputs;
	if (TileFrame.bPlanet)
	{
		Inputs.Planet = GatherPlanetOverlayInput(TileCoord);
	}
	else
	{
		Inputs.Heightmap = GatherOverlayInput(TileCoord);
	}
	return Inputs;
}

