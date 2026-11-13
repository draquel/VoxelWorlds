// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapSubsystem.h"
#include "VoxelMap.h"
#include "VoxelChunkManager.h"
#include "VoxelWaterLevels.h"
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

// Map tile images of every level of the pyramid in one least-recently-used store (MAP_PYRAMID_PLAN.md P1; it replaces
// world modes review WM-E2's detail and overview budgets): 64 x 64 images are 16 KB each, and the views ask for about 300
// at a time whatever is explored.
static TAutoConsoleVariable<float> CVarMapTileBudgetMB(
	TEXT("voxel.Map.TileBudgetMB"),
	32.0f,
	TEXT("MB of map tile images kept, every level of the tile pyramid together, least recently used dropped first (a view ")
	TEXT("asks for them again). 0 = unlimited. 32 MB holds 2,048 tiles of 64 x 64: the world map's view (about 300 tiles ")
	TEXT("at any zoom), the minimap's and the coarser tiles they fall back on."),
	FConsoleVariableDelegate::CreateStatic(&ApplyMapBudgetsToAll),
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
		TEXT("Mark every map tile of a bounded world explored (as a fully explored save restores it), a planet's whole atlas ")
		TEXT("with no cap; the views draw what they show. Args: [radius=200000] (unbounded worlds: the square of tiles within ")
		TEXT("this radius of the pawn)."),
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
	ExploredMask.Reset();
	PendingExploredBlobs.Empty();
	PrefetchLevel = INDEX_NONE;
	TileQueue.Reset();
	for (TSet<FVoxelMapTileId>& View : ViewTiles)
	{
		View.Empty();
	}
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
	TileFrame.Water = ChunkMgr->GetWaterLevels();
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

	// The explored mask's faces (MAP_PYRAMID_PLAN.md P2): a planet's six of N tiles a side; a heightmap world's one,
	// unbounded. Nothing is explored before the world resolves; saves imported before it are applied now.
	ExploredMask.Configure(TileFrame.bPlanet ? 6 : 1, TileFrame.bPlanet ? TileFrame.Grid.GetCellsPerEdge() : 0);

	bCacheResolved = true;
	ApplyTileBudgets();
	for (const TPair<FString, int32>& Blob : PendingExploredBlobs)
	{
		ApplyExploredBlob(Blob.Key, Blob.Value);
	}
	PendingExploredBlobs.Empty();
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

	// The globe's top levels, drawn now and kept (P2: they replace the globe albedo).
	PrefetchTopLevels();
	return true;
}

void UVoxelMapSubsystem::PrefetchTopLevels()
{
	PrefetchLevel = INDEX_NONE;
	if (!TileFrame.bPlanet || !TileFrame.Grid.IsValid())
	{
		return;
	}
	// The coarsest levels down to the first with at most PrefetchTilesPerFaceEdge tiles a face edge (level 9 on the 50 km
	// planet, 4 x 4 a face), coarsest first so the whole globe has an image soonest; each pinned, so trimming keeps them.
	const int32 MaxLevel = TileFrame.GetMaxLevel();
	int32 Level = MaxLevel;
	while (Level > 0 && TileFrame.TilesPerFaceEdgeAt(Level - 1) <= PrefetchTilesPerFaceEdge)
	{
		--Level;
	}
	PrefetchLevel = Level;
	TArray<FVoxelMapTileId> Ids;
	for (int32 L = MaxLevel; L >= Level; --L)
	{
		const int32 NL = TileFrame.TilesPerFaceEdgeAt(L);
		for (int32 V = 0; V < NL; ++V)
		{
			for (int32 X = 0; X < 6 * NL; ++X)
			{
				const FVoxelMapTileId Id(FIntPoint(X, V), L);
				TileStore.Pin(Id);
				Ids.Add(Id);
			}
		}
	}
	const int32 Queued = RequestView(EVoxelMapRequester::Prefetch, Ids);
	UE_LOG(LogVoxelMap, Log, TEXT("The globe's top levels %d-%d prefetched: %d tile(s), %d queued, kept."), Level, MaxLevel, Ids.Num(), Queued);
}

// ---------------------------------------------------------------------------
// Tile Queries
// ---------------------------------------------------------------------------

const FVoxelMapTile* UVoxelMapSubsystem::GetTile(FIntPoint TileCoord) const
{
	return FindTile(FVoxelMapTileId(TileCoord), /*bTouch*/ true);
}

const FVoxelMapTile* UVoxelMapSubsystem::FindTile(const FVoxelMapTileId& Id, bool bTouch) const
{
	const FVoxelMapTile* Found = TileStore.Find(Id, bTouch);
	return (Found && Found->bIsReady) ? Found : nullptr;
}

bool UVoxelMapSubsystem::HasTile(FIntPoint TileCoord) const
{
	return TileStore.Contains(FVoxelMapTileId(TileCoord));
}

int32 UVoxelMapSubsystem::RequestView(EVoxelMapRequester Requester, TConstArrayView<FVoxelMapTileId> NearestFirst)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_RequestView);
	const int32 R = static_cast<int32>(Requester);
	if (bShuttingDown || R < 0 || R >= static_cast<int32>(EVoxelMapRequester::Num) || !ResolveChunkManager())
	{
		return 0;
	}
	// The view's tiles (for invalidations), and the ones it lacks (or holds stale) in its order: they replace its last
	// request, so what left the view stops waiting.
	TSet<FVoxelMapTileId>& View = ViewTiles[R];
	View.Reset();
	TArray<FVoxelMapTileId> Missing;
	for (const FVoxelMapTileId& Id : NearestFirst)
	{
		if (!TileFrame.CanDrawTile(Id) || !TileFrame.IsValidTile(Id))
		{
			continue;
		}
		View.Add(Id);
		if (!TileStore.Contains(Id) || TileStore.IsStale(Id))
		{
			Missing.Add(Id);
		}
	}
	TileQueue.SetRequests(Requester, Missing);
	DrawStats.ImageRequests += Missing.Num();
	DrawStats.WaitingPeak = FMath::Max(DrawStats.WaitingPeak, TileQueue.NumWaiting());
	LaunchWaitingTiles();
	return Missing.Num();
}

void UVoxelMapSubsystem::ApplyTileBudgets()
{
	constexpr double MB = 1024.0 * 1024.0;
	TileStore.SetBudget(static_cast<int64>(FMath::Max(0.0f, CVarMapTileBudgetMB.GetValueOnGameThread()) * MB));
}

void UVoxelMapSubsystem::MarkExplored(FIntPoint TileCoord)
{
	int32 Face = 0, U = 0, V = 0;
	if (TileToMaskTile(TileCoord, Face, U, V) && ExploredMask.Set(Face, U, V))
	{
		++ExplorationRevision;
	}
}

bool UVoxelMapSubsystem::TileToMaskTile(FIntPoint TileCoord, int32& OutFace, int32& OutU, int32& OutV) const
{
	if (TileFrame.bPlanet)
	{
		return TileFrame.Grid.FromAtlas(TileCoord, OutFace, OutU, OutV);
	}
	OutFace = 0;
	OutU = TileCoord.X;
	OutV = TileCoord.Y;
	return true;
}

void UVoxelMapSubsystem::GetExploredTiles(TArray<FIntPoint>& OutTiles) const
{
	OutTiles.Reset(static_cast<int32>(FMath::Min<int64>(ExploredMask.Num(), MAX_int32)));
	const bool bPlanet = TileFrame.bPlanet;
	const FVoxelCubeSphereGrid& Grid = TileFrame.Grid;
	ExploredMask.ForEach([&OutTiles, bPlanet, &Grid](int32 Face, int32 U, int32 V)
	{
		OutTiles.Add(bPlanet ? Grid.ToAtlas(Face, U, V) : FIntPoint(U, V));
	});
}

bool UVoxelMapSubsystem::InvalidateTile(const FVoxelMapTileId& Id)
{
	if (!TileStore.Contains(Id) && !TileQueue.IsQueued(Id))
	{
		return false;
	}
	// A running drawing is discarded; the image stays on screen, stale, until the redraw lands.
	TileQueue.Invalidate(Id);
	TileStore.MarkStale(Id);
	for (int32 R = 0; R < static_cast<int32>(EVoxelMapRequester::Num); ++R)
	{
		if (ViewTiles[R].Contains(Id))
		{
			TileQueue.Add(static_cast<EVoxelMapRequester>(R), Id);
		}
	}
	return true;
}

void UVoxelMapSubsystem::InvalidateAncestors(FIntPoint Tile0)
{
	const int32 MaxLevel = TileFrame.GetMaxLevel();
	for (int32 Level = 1; Level <= MaxLevel; ++Level)
	{
		InvalidateTile(TileFrame.LevelTileOf(Tile0, Level));
	}
}

int32 UVoxelMapSubsystem::InvalidateAllTiles()
{
	TArray<FVoxelMapTileId> Ids;
	TileStore.GetIds(Ids);
	for (const TSet<FVoxelMapTileId>& View : ViewTiles)
	{
		for (const FVoxelMapTileId& Id : View)
		{
			if (TileQueue.IsDrawing(Id))
			{
				Ids.Add(Id);
			}
		}
	}
	int32 Invalidated = 0;
	for (const FVoxelMapTileId& Id : Ids)
	{
		Invalidated += InvalidateTile(Id) ? 1 : 0;
	}
	LaunchWaitingTiles();
	return Invalidated;
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
	// The explored mask takes whole rectangles, a block of 64 x 64 tiles at a time (MAP_PYRAMID_PLAN.md P2): no cap, no
	// order (the views ask for what they show, nearest their centre, P1), and a fully explored block costs a few bytes.
	int64 InArea = 0;
	int64 Added = 0;
	if (TileFrame.bPlanet)
	{
		// A planet: the six faces of the atlas (16 million tiles on the 50 km planet; the cap of world modes review WM-BP11
		// explored the million nearest the pawn, G163).
		const int32 N = TileFrame.Grid.GetCellsPerEdge();
		for (int32 Face = 0; Face < 6; ++Face)
		{
			Added += ExploredMask.SetRect(Face, FIntRect(0, 0, N, N));
		}
		InArea = TileFrame.Grid.GetNumCells();
	}
	else
	{
		// A heightmap world: the tiles of the area, an island's bounds (each row's runs of tiles inside the world, WM-I2) or
		// the square within Radius of Center on a plane.
		const FBox2D Area = CachedWorldBounds.IsBounded() ? CachedWorldBounds.GetBox2D()
			: FBox2D(FVector2D(Center.X - Radius, Center.Y - Radius), FVector2D(Center.X + Radius, Center.Y + Radius));
		const FIntPoint Lo = WorldToTileCoord(FVector(Area.Min.X, Area.Min.Y, 0.0));
		const FIntPoint Hi = WorldToTileCoord(FVector(Area.Max.X, Area.Max.Y, 0.0));
		if (!CachedWorldBounds.IsBounded())
		{
			Added = ExploredMask.SetRect(0, FIntRect(Lo, Hi + FIntPoint(1, 1)));
			InArea = static_cast<int64>(Hi.X - Lo.X + 1) * (Hi.Y - Lo.Y + 1);
		}
		else
		{
			for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
			{
				int32 RunStart = INDEX_NONE;
				for (int32 X = Lo.X; X <= Hi.X + 1; ++X)
				{
					const bool bInside = X <= Hi.X && IsTileInsideWorld(FIntPoint(X, Y));
					if (bInside && RunStart == INDEX_NONE)
					{
						RunStart = X;
					}
					else if (!bInside && RunStart != INDEX_NONE)
					{
						Added += ExploredMask.SetRect(0, FIntRect(RunStart, Y, X, Y + 1));
						InArea += X - RunStart;
						RunStart = INDEX_NONE;
					}
				}
			}
		}
	}
	ExplorationRevision += Added > 0 ? 1 : 0;
	UE_LOG(LogVoxelMap, Log, TEXT("ExploreAllTiles: %lld tiles of the world explored (%lld new); the mask %lld tiles in %d blocks (%d full), %.1f KB; no image queued (the views ask)."),
		InArea, Added, ExploredMask.Num(), ExploredMask.NumBlocks(), ExploredMask.NumFullBlocks(), ExploredMask.GetAllocatedSize() / 1024.0);
	return InArea;
}

bool UVoxelMapSubsystem::IsTileExplored(FIntPoint TileCoord) const
{
	int32 Face = 0, U = 0, V = 0;
	return TileToMaskTile(TileCoord, Face, U, V) && ExploredMask.IsExplored(Face, U, V);
}

bool UVoxelMapSubsystem::IsTileInsideWorld(FIntPoint TileCoord) const
{
	return TileFrame.IsValidTile(TileCoord);
}

int32 UVoxelMapSubsystem::PruneExploredOutsideWorld()
{
	// A planet's mask holds only cells of its faces (FVoxelMapExploredMask::IsInRange); a plane has no outside.
	if (TileFrame.bPlanet || !CachedWorldBounds.IsBounded())
	{
		return 0;
	}
	// A heightmap world's bounds (an island's R + falloff; tiles explored before exploration stopped at the edge): tile by
	// tile, a bounded world's mask being small.
	const int64 Dropped = ExploredMask.RemoveIf(
		[](int32 Face, const FIntRect& BlockTiles) { return true; },
		[this](int32 Face, int32 U, int32 V) { return !IsTileInsideWorld(FIntPoint(U, V)); });
	if (Dropped > 0)
	{
		UE_LOG(LogVoxelMap, Log, TEXT("Dropped %lld explored map tile(s) outside the world's bounds (%lld explored now)."), Dropped, ExploredMask.Num());
	}
	return static_cast<int32>(FMath::Min<int64>(Dropped, MAX_int32));
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
	// whose centres lie within the radius along the surface), each explored. Nothing is drawn here: the minimap asks for the
	// tiles it shows at its level (RequestView).
	TArray<FIntPoint> Tiles;
	TileFrame.TilesWithinRadius(WorldPos, Radius, Tiles);
	for (const FIntPoint& TileCoord : Tiles)
	{
		MarkExplored(TileCoord);
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

	// The tiles the chunk spans (its column; on a planet the cells its corners span, spherical Phase 4): each explored
	// (chunks that generate are in the player's vicinity) inside the world only, an island also generating a ring of
	// floor chunks past its edge (WM-I2). Nothing is drawn here: the views ask for what they show.
	TArray<FIntPoint> Tiles;
	GetChunkTiles(ChunkCoord, Tiles);
	for (const FIntPoint& Tile : Tiles)
	{
		const uint64 TileKey = PackTileKey(Tile);
		if (IsTileInsideWorld(Tile))
		{
			MarkExplored(Tile);
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
	// Tiles drawn before the POI conditioner registered show the unflattened terrain (QA W-05): every image goes stale,
	// drawings already running are discarded, and the views' tiles are drawn again. The current images stay until the new
	// ones land (R22: the map used to flash blank here), and the redraws reuse the stored edit overlays without reading
	// chunk data. A stale tile no view shows is drawn again when one asks for it.
	const int32 Redrawn = InvalidateAllTiles();
	UE_LOG(LogVoxelMap, Log, TEXT("Terrain conditioning changed: %d map tile image(s) stale, %d queued for the views."),
		Redrawn, TileQueue.NumWaiting());
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
	const int32 Redrawn = InvalidateAllTiles();
	UE_LOG(LogVoxelMap, Log, TEXT("Redrawing %d map tile(s) with an image (%d queued for the views)%s."), Redrawn, TileQueue.NumWaiting(),
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
	constexpr double MB = 1024.0 * 1024.0;
	// The drawing since the last reset (the map scaling plan's M0, G163): the rate, the worker time a tile takes, and how
	// many drawings replaced images the store still held or answered a view's requests.
	const double Elapsed = FMath::Max(FPlatformTime::Seconds() - DrawStats.SinceSeconds, 1.0e-3);
	const double MsPerTile = DrawStats.Drawn > 0 ? 1000.0 * DrawStats.DrawSeconds / static_cast<double>(DrawStats.Drawn) : 0.0;
	// The images by level (the pyramid, MAP_PYRAMID_PLAN.md P1): "L0:12 L2:280".
	TArray<FVoxelMapTileId> Ids;
	TileStore.GetIds(Ids);
	TMap<int32, int32> PerLevel;
	for (const FVoxelMapTileId& Id : Ids)
	{
		++PerLevel.FindOrAdd(Id.Level);
	}
	PerLevel.KeySort(TLess<int32>());
	FString Levels;
	for (const TPair<int32, int32>& Pair : PerLevel)
	{
		Levels += FString::Printf(TEXT("%sL%d:%d"), Levels.IsEmpty() ? TEXT("") : TEXT(" "), Pair.Key, Pair.Value);
	}
	return FString::Printf(
		TEXT("tiles cached=%d explored=%lld (mask %d blocks, %d full, %.1f KB) waiting=%d drawing=%d waiting-peak=%d (minimap %d, world map %d, edits %d, prefetch %d) | drawn=%lld (%.0f/s over %.0f s, %.2f ms a tile on a worker) replaced=%lld image-requests=%lld | images=%d %.1f/%.0f MB [%s] evicted=%lld | overlays=%d columns=%d bytes=%d edited-tiles=%d dirty=%d awaiting=%d rebuild-queued=%d"),
		TileStore.Num(), ExploredMask.Num(), ExploredMask.NumBlocks(), ExploredMask.NumFullBlocks(), ExploredMask.GetAllocatedSize() / 1024.0, TileQueue.NumWaiting(), TileQueue.NumDrawing(), DrawStats.WaitingPeak,
		TileQueue.NumWaitingFor(EVoxelMapRequester::Minimap), TileQueue.NumWaitingFor(EVoxelMapRequester::WorldMap),
		TileQueue.NumWaitingFor(EVoxelMapRequester::Edits), TileQueue.NumWaitingFor(EVoxelMapRequester::Prefetch),
		DrawStats.Drawn, static_cast<double>(DrawStats.Drawn) / Elapsed, Elapsed, MsPerTile, DrawStats.Replaced, DrawStats.ImageRequests,
		TileStore.Num(), TileStore.GetBytes() / MB, TileStore.GetBudget() / MB, *Levels, TileStore.GetEvictions(),
		EditOverlays.Num(), MaskedColumns, Bytes, TileEditedChunks.Num(), OverlayDirtyTiles.Num(),
		OverlayAwaitingTiles.Num(), OverlayRebuildTiles.Num());
}

void UVoxelMapSubsystem::ResetDebugStats()
{
	DrawStats = FDrawStats();
	DrawStats.SinceSeconds = FPlatformTime::Seconds();
	DrawStats.WaitingPeak = TileQueue.NumWaiting();
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
		bool bLastExplored = false;
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
					bLastExplored = IsTileExplored(Tile);
					LastOverlay = bHighlightOverlay ? GetEditOverlay(Tile) : nullptr;
				}
				FColor Color;
				if (!bLastExplored || !SampleStoredPixel(Tile, PX, PY, Color))
				{
					continue;
				}
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
			if (!IsTileExplored(Tile))
			{
				continue;
			}
			const FVoxelMapEditOverlay* Overlay = bHighlightOverlay ? GetEditOverlay(Tile) : nullptr;
			for (int32 PY = 0; PY < Res; ++PY)
			{
				for (int32 PX = 0; PX < Res; ++PX)
				{
					// The finest level stored over the pixel (a coarse tile's pixel covers several here).
					FColor Color;
					if (!SampleStoredPixel(Tile, PX, PY, Color))
					{
						continue;
					}
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

bool UVoxelMapSubsystem::SampleStoredPixel(FIntPoint Tile0, int32 PX, int32 PY, FColor& OutColor) const
{
	const int32 Res = CachedChunkSize;
	const int32 MaxLevel = TileFrame.GetMaxLevel();
	for (int32 Level = 0; Level <= MaxLevel; ++Level)
	{
		const FVoxelMapTileId Id = TileFrame.LevelTileOf(Tile0, Level);
		const FVoxelMapTile* Tile = TileStore.Find(Id, /*bTouch*/ false);
		if (!Tile || Tile->Resolution != Res || Tile->PixelData.Num() != Res * Res)
		{
			continue;
		}
		// The level-0 pixel's place in the coarser tile: its offset from the tile's first level-0 tile, 2^L pixels a pixel.
		const FIntPoint First = TileFrame.FirstLevel0Tile(Id);
		const int32 LX = static_cast<int32>(((static_cast<int64>(Tile0.X - First.X)) * Res + PX) >> Level);
		const int32 LY = static_cast<int32>(((static_cast<int64>(Tile0.Y - First.Y)) * Res + PY) >> Level);
		if (LX < 0 || LY < 0 || LX >= Res || LY >= Res)
		{
			continue;
		}
		OutColor = Tile->PixelData[LY * Res + LX];
		return true;
	}
	return false;
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
		// The overlay is built by a level-0 drawing (whether a view shows level 0 or not); a running one is discarded.
		const FVoxelMapTileId Id(UnpackTileKey(Key));
		TileQueue.Invalidate(Id);
		TileQueue.Add(EVoxelMapRequester::Edits, Id);
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
	// The tile's water: the levels' range over the tile (a heightmap tile's level is a world Z).
	const FVoxelWaterLevelRange TileWater = TileFrame.GetTileWater(FVoxelMapTileId(TileCoord, 0));
	Input->bUseWater = TileWater.bAny;
	Input->WaterLevel = static_cast<float>(TileWater.GetLevel());

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
	// The tile's water: the levels' range over the tile (a planet's level is a distance from the centre).
	const FVoxelWaterLevelRange TileWater = TileFrame.GetTileWater(FVoxelMapTileId(TileCoord, 0));
	Input->bUseWater = TileWater.bAny && TileFrame.Water.IsRadial();
	Input->WaterRadius = TileWater.GetLevel();

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

void UVoxelMapSubsystem::LaunchWaitingTiles()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_LaunchWaitingTiles);
	DrawStats.WaitingPeak = FMath::Max(DrawStats.WaitingPeak, TileQueue.NumWaiting());
	// At most one overlay rebuild gathers chunk data per frame: a gather shares snapshots and walks edit layers on the
	// game thread (0.1-2.5 ms per tile measured in VoxelDemo), and four in one frame would hitch. A skipped rebuild
	// keeps waiting; a next-tick retry starts it (a completion cannot be relied on: it may land in this same frame).
	bool bDeferredRebuild = false;
	auto CanStart = [this, &bDeferredRebuild](const FVoxelMapTileId& Id)
	{
		if (Id.Level == 0 && LastOverlayGatherFrame == GFrameCounter && OverlayRebuildTiles.Contains(PackTileKey(Id.Coord)))
		{
			bDeferredRebuild = true;
			return false;
		}
		return true;
	};
	FVoxelMapTileId Id;
	uint32 Generation = 0;
	while (!bShuttingDown && TileQueue.NumDrawing() < MaxConcurrentTileGenTasks && TileQueue.StartNext(Id, Generation, CanStart))
	{
		GenerateTileAsync(Id, Generation);
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

