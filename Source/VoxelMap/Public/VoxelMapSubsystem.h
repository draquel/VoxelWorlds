// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelMapTypes.h"
#include "VoxelMapEditOverlay.h"
#include "VoxelMapTileFrame.h"
#include "VoxelMapTileQueue.h"
#include "VoxelMapTileStore.h"
#include "VoxelCoreTypes.h"
#include "VoxelEditTypes.h"
#include "VoxelWorldModeTypes.h"
#include "VoxelCubeSphere.h"
#include "VoxelSeaSurface.h"
#include "TimerManager.h"
#include "VoxelMapSubsystem.generated.h"

class IVoxelWorldMode;
class UVoxelChunkManager;
class UVoxelEditManager;
class UVoxelBiomeConfiguration;
struct FVoxelFarFieldSurface;

/**
 * A planet's coarse albedo for the world map's globe zoomed out (spherical Phase 6, WM-BP11): a cube-sphere grid of at
 * most UVoxelMapSubsystem::GlobeAlbedoCellsPerEdge cells a face edge, each the colour of the surface at its centre as a
 * tile pixel draws it (water by depth under the sea; land the surface material, shaded by elevation and a hillshade),
 * sampled from the analytic surface the far field draws (FVoxelFarFieldSurface). Where a pixel of the globe spans many
 * tiles, the world map reads this instead of one tile pixel each.
 */
struct VOXELMAP_API FVoxelMapGlobeAlbedo
{
	/** The albedo's cells (the same faces and warp as the tile grid, coarser) */
	FVoxelCubeSphereGrid Grid;

	/** One colour per cell, atlas order: Y * 6N + X */
	TArray<FColor> Colors;

	/** Time the build took (s, on its thread) */
	double BuildSeconds = 0.0;

	/** The colour of a cell (atlas coordinate). */
	const FColor& At(const FIntPoint& Cell) const { return Colors[Cell.Y * 6 * Grid.GetCellsPerEdge() + Cell.X]; }
};

/**
 * World subsystem that manages 2D map tile data for voxel terrain.
 *
 * Generates tile images by sampling terrain height and material from
 * IVoxelWorldMode — works with any world mode without casting.
 *
 * Tile generation is dual-strategy:
 * 1. Event-driven: binds to UVoxelChunkManager::OnChunkGenerated to auto-generate
 *    tiles as chunks stream in.
 * 2. Predictive: RequestTilesInRadius() generates tiles ahead of chunk streaming
 *    using deterministic height queries (no loaded chunk data needed).
 *
 * Voxel edits (QA R22): the analytic terrain knows nothing about digs, builds or POI stamps, so each edited tile
 * keeps an FVoxelMapEditOverlay of the columns whose top surface an edit changed. UVoxelEditManager::OnChunkEdited
 * marks tiles dirty; every voxel.Map.EditRefreshSeconds the dirty tiles whose edited chunks are loaded gather their
 * edit-merged snapshots (UVoxelChunkManager::GetVoxelSnapshot) and redraw, building the overlay in the tile
 * task. Tiles whose chunks are not loaded keep their current overlay until a chunk of theirs generates.
 *
 * Tile images live under a fixed memory budget (world modes review WM-E2, finding KI15; FVoxelMapTileStore): every
 * drawn tile keeps a small overview image, and the detail image only while recently used (voxel.Map.DetailBudgetMB,
 * voxel.Map.OverviewBudgetMB). Exploration is separate and never evicted: a tile that lost its images is drawn again
 * when the minimap (RequestTilesInRadius) or the world map (RequestTileImage) asks for it.
 *
 * Planets (spherical Phase 4, WORLD_MODES_REVIEW_PLAN.md §18, WM-SY8): a tile is a cell of the cube-sphere grid
 * (FVoxelCubeSphereGrid, N = ceil((pi / 2) * R / chunk size) cells a face edge, so a tile is about a chunk across),
 * addressed by its atlas coordinate (Face * N + tile U, tile V); every FIntPoint key, the store, the queue, the budgets and
 * the save blobs work unchanged. A tile's pixels run along the face's U and V; each is the terrain along its direction
 * from the centre (material by QueryPlanetSurfaceConditions, water by the sea sphere, shading by altitude, hillshade in
 * the local frame). Edit overlays walk each pixel's line along local up and store altitudes.
 *
 * All tile generation runs on background threads. The subsystem has zero
 * knowledge of players, characters, or UI — purely manages tile data.
 */
UCLASS()
class VOXELMAP_API UVoxelMapSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// --- Tile Queries ---

	/**
	 * Get a generated tile: its detail image, or its overview when the detail was evicted (the tile's Resolution says
	 * which; callers scale by it). Counts as a use, which keeps the tile's images from eviction.
	 * @return The image, or nullptr if the tile has none (not drawn yet, or evicted)
	 */
	const FVoxelMapTile* GetTile(FIntPoint TileCoord) const;

	/** Check if a tile has an image (detail or overview), regardless of exploration state. */
	bool HasTile(FIntPoint TileCoord) const;

	/** Check if a tile has its full-resolution image. */
	bool HasTileDetail(FIntPoint TileCoord) const;

	/**
	 * Ask for an explored tile's image when it has none (evicted), or its detail when bDetail and it only has the
	 * overview. The world map calls this for the tiles it shows (WM-E2).
	 * @param TileCoord Tile coordinate (chunk X, Y)
	 * @param bDetail Want the full-resolution image (a zoomed-in view), not just the overview
	 * @return True if a drawing was queued (or the tile's queued drawing now keeps its detail)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	bool RequestTileImage(FIntPoint TileCoord, bool bDetail);

	/** The tile images and their budgets (diagnostics, tests). */
	const FVoxelMapTileStore& GetTileStore() const { return TileStore; }

	/**
	 * Pixels a side of a tile's overview image: the detail resolution over FVoxelMapTileStore::OverviewDownsample.
	 * A view that shows more texels per tile than this wants detail (RequestTileImage).
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	int32 GetTileOverviewResolution() const { return FMath::Max(1, CachedChunkSize / FVoxelMapTileStore::OverviewDownsample); }

	/** Apply voxel.Map.DetailBudgetMB / voxel.Map.OverviewBudgetMB (called when they change). */
	void ApplyTileBudgets();

	/**
	 * Debug (voxel.Map.ExploreAll): mark every tile that overlaps the world's bounds explored, as a fully explored save
	 * restores it, and queue their images (overview only). Unbounded worlds: the tiles within Radius of Center. A planet
	 * (spherical Phase 6, WM-BP11): its whole atlas up to voxel.Map.ExploreAllMaxTiles tiles (16 million on a 50 km
	 * planet, 6.4 billion at 1,000 km), else that many nearest Center; the images of the nearest
	 * PlanetExploreAllImageTiles queued, the rest drawn when the map shows them.
	 * @return Number of tiles explored
	 */
	int64 ExploreAllTiles(const FVector& Center, float Radius);

	/** Planets: ExploreAllTiles queues the images of at most this many tiles (nearest first); the rest draw when shown. */
	static constexpr int32 PlanetExploreAllImageTiles = 65536;

	// --- The globe zoomed out (spherical Phase 6, WM-BP11) ---

	/** Most cells along a globe albedo's face edge (never more than the tile grid has). */
	static constexpr int32 GlobeAlbedoCellsPerEdge = 256;

	/**
	 * A planet's coarse albedo for the globe zoomed out (FVoxelMapGlobeAlbedo). The first call starts the build on a
	 * worker thread (about a second at 256 cells a face edge); OnMapTileReady fires with (-1, -1) when it is ready.
	 * @return The albedo, or null until it is built (and in heightmap worlds)
	 */
	const FVoxelMapGlobeAlbedo* GetGlobeAlbedo();

	/**
	 * Whether any explored tile lies in a cell of the globe albedo's grid: the globe's fog of war zoomed out.
	 * @param AlbedoCell Atlas coordinate in the albedo's grid
	 * @return True when explored (false without an albedo)
	 */
	bool IsGlobeCellExplored(const FIntPoint& AlbedoCell);

	/**
	 * Pure: build a planet's globe albedo (any thread): every cell's colour from the analytic surface at its centre.
	 * @param Surface The planet's analytic surface
	 * @param CellsPerEdge Cells along a face edge
	 * @param MaterialPalette Colour per material id (256)
	 * @param AltitudeMin Lowest terrain altitude over the datum (the depth shading)
	 * @param AltitudeMax Highest (the elevation shading)
	 * @return The albedo
	 */
	static TSharedPtr<FVoxelMapGlobeAlbedo> BuildGlobeAlbedo(const FVoxelFarFieldSurface& Surface, int32 CellsPerEdge,
		const TArray<FColor>& MaterialPalette, float AltitudeMin, float AltitudeMax);

	// --- Exploration ---

	/**
	 * Mark tiles in radius as explored and request generation for any not yet cached.
	 * Call this from external code (character plugin) to drive predictive generation.
	 *
	 * @param WorldPos Player or camera world position
	 * @param Radius World-unit radius around the position to explore
	 */
	void RequestTilesInRadius(const FVector& WorldPos, float Radius);

	/** Check if a tile has been explored. */
	bool IsTileExplored(FIntPoint TileCoord) const;

	/**
	 * Whether a tile holds any of the world: it overlaps the world mode's footprint (an island's R + falloff; every
	 * tile on a plane). Only such tiles are explored, so exploration stops at the world's edge (world modes review
	 * WM-I2, KI9). True before the map has resolved its world.
	 *
	 * @param TileCoord Tile coordinate (chunk X, Y)
	 * @return True when the tile overlaps the world
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	bool IsTileInsideWorld(FIntPoint TileCoord) const;

	/**
	 * The world's bounds as the map resolved them (unbounded before the map has resolved its world). The world map
	 * clamps its pan to them.
	 *
	 * @return The bounds
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	FVoxelWorldBounds GetWorldBounds() const { return CachedWorldBounds; }

	/** Get all explored tile coords. */
	const TSet<uint64>& GetExploredTiles() const { return ExploredTiles; }

	/**
	 * The explored tiles as a compact blob for a save: zlib-compressed int32 (X, Y) pairs, base64-encoded. A string
	 * keeps it JSON-safe (packed 64-bit keys would lose precision as JSON numbers) and small (neighbouring tiles
	 * compress well).
	 * @param OutCount Receives the number of tiles encoded (needed to decode)
	 * @return The encoded tiles; empty when nothing is explored
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	FString ExportExploredTiles(int32& OutCount) const;

	/**
	 * Restore explored tiles saved by ExportExploredTiles and queue their map images. Merges into the current set:
	 * exploration only grows, and tiles already explored this session (the spawn area streams in before a save
	 * loads) stay explored.
	 * @param Encoded The blob from ExportExploredTiles
	 * @param Count   The tile count ExportExploredTiles reported
	 * @return Number of tiles restored (0 for an empty or malformed blob)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	int32 ImportExploredTiles(const FString& Encoded, int32 Count);

	/**
	 * Pure: encode tile coordinates (see ExportExploredTiles).
	 * @param Tiles Tile coordinates to encode
	 * @return Base64 of the zlib-compressed int32 (X, Y) pairs; empty for no tiles or a compression failure
	 */
	static FString EncodeTileCoords(const TArray<FIntPoint>& Tiles);

	/**
	 * Pure: decode EncodeTileCoords output.
	 * @param Encoded  The encoded blob
	 * @param Count    Number of tiles it holds
	 * @param OutTiles Receives the coordinates (emptied first)
	 * @return False for a malformed blob (OutTiles left empty)
	 */
	static bool DecodeTileCoords(const FString& Encoded, int32 Count, TArray<FIntPoint>& OutTiles);

	// --- Edit overlays (R22) ---

	/**
	 * The edit overlay a tile is drawn with: the columns whose top surface voxel edits changed.
	 * @param TileCoord Tile coordinate (chunk X, Y)
	 * @return The overlay, or null when no edit changes the tile's surface (or it is not known yet)
	 */
	const FVoxelMapEditOverlay* GetEditOverlay(FIntPoint TileCoord) const;

	/** Number of tiles with an edit overlay. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	int32 GetEditOverlayCount() const { return EditOverlays.Num(); }

	/**
	 * The edit overlays as a compact blob for a save (R22 phase 3), so edits show on the map after a load before
	 * their areas load again. See EncodeEditOverlays for the format.
	 * @param OutCount Receives the number of tiles encoded
	 * @return The encoded overlays; empty when no tile has one
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	FString ExportEditOverlays(int32& OutCount) const;

	/**
	 * Restore overlays saved by ExportEditOverlays. They replace the current overlays. Every tile whose overlay
	 * changes is redrawn and marked for a refresh, which reconciles it with the edits loaded next: a tile whose
	 * edited chunks are loaded is rebuilt from them, a tile with no edits left is cleared, and any other tile keeps
	 * the saved overlay until its chunks load. Call it before loading the edits.
	 * @param Encoded The blob from ExportEditOverlays (empty clears every overlay)
	 * @return Number of tiles restored (0 for an empty or malformed blob)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	int32 ImportEditOverlays(const FString& Encoded);

	/**
	 * Pure: encode tile overlays. Binary payload: magic 'VMEO', format version, tile count, then per tile its X, Y
	 * and FVoxelMapEditOverlay::Serialize data; zlib-compressed behind its uncompressed size, base64-encoded.
	 * @param Overlays Tiles and overlays to encode
	 * @return The blob; empty for no tiles or a compression failure
	 */
	static FString EncodeEditOverlays(const TArray<FVoxelMapTileOverlay>& Overlays);

	/**
	 * Pure: decode EncodeEditOverlays output.
	 * @param Encoded     The blob
	 * @param OutOverlays Receives the tiles (emptied first)
	 * @return False for a malformed blob (OutOverlays left empty); true for an empty blob
	 */
	static bool DecodeEditOverlays(const FString& Encoded, TArray<FVoxelMapTileOverlay>& OutOverlays);

	/**
	 * Redraw every explored tile, keeping the current images until the new ones land.
	 * @param bRebuildOverlays Also rebuild the edit overlay of every tile with edits (from loaded chunks)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	void RedrawAllTiles(bool bRebuildOverlays);

	/**
	 * One-line state summary for diagnostics (voxel.Map.Stats).
	 * @return Cached / explored / queued tiles, overlays and their pending state
	 */
	FString GetDebugStats() const;

	/**
	 * Debug (voxel.Map.Capture): write the explored tiles around a world position to a PNG, north up as the world map
	 * draws them, with the position marked. Copies the cached tile pixels, so before/after comparisons need no UI
	 * capture.
	 * @param Center            World position at the image centre
	 * @param RadiusTiles       Tiles from the centre tile to each edge (0-64)
	 * @param FilePath          Output .png path
	 * @param bHighlightOverlay Tint edit-overlay columns magenta
	 * @return True if the file was written
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map|Debug")
	bool CaptureMapImage(const FVector& Center, int32 RadiusTiles, const FString& FilePath, bool bHighlightOverlay) const;

	// --- Coordinate Helpers ---

	/** Convert a world position to a tile coordinate (chunk XY). */
	FIntPoint WorldToTileCoord(const FVector& WorldPos) const;

	/** Convert a tile coordinate to world position (tile origin corner). */
	FVector TileCoordToWorld(FIntPoint TileCoord) const;

	/** Get the world size of a single tile edge (ChunkSize * VoxelSize). */
	float GetTileWorldSize() const;

	/** Get the tile resolution (pixels per edge, matches ChunkSize). */
	int32 GetTileResolution() const { return CachedChunkSize; }

	// --- Planets (spherical Phase 4, WM-SY8) ---

	/**
	 * Whether the map is a planet's: tiles are cells of the cube-sphere grid (GetPlanetTileGrid) addressed by atlas
	 * coordinates, and a tile's pixels run along its face's U and V instead of world X and Y.
	 * @return True once the map has resolved a planet world
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	bool IsPlanetMap() const { return TileFrame.bPlanet; }

	/** The planet's tile grid (invalid in heightmap worlds and before the map has resolved its world). */
	const FVoxelCubeSphereGrid& GetPlanetTileGrid() const { return TileFrame.Grid; }

	/** The planet's centre (zero in heightmap worlds). */
	FVector GetPlanetCenter() const { return TileFrame.Center; }

	/** The planet's datum radius (zero in heightmap worlds). */
	double GetPlanetRadius() const { return TileFrame.Radius; }

	/** The map's tile frame (the planet's atlas or the heightmap's chunk-column grid; FVoxelMapTileFrame), valid once the world is resolved. */
	const FVoxelMapTileFrame& GetTileFrame() const { return TileFrame; }

	/**
	 * The tile and pixel a direction from the planet centre falls in.
	 * @param Direction Unit direction
	 * @param OutTile   Atlas coordinate of the tile
	 * @param OutPX     Pixel along the tile's U, in [0, GetTileResolution())
	 * @param OutPY     Pixel along the tile's V
	 * @return False in heightmap worlds
	 */
	bool PlanetDirectionToTilePixel(const FVector& Direction, FIntPoint& OutTile, int32& OutPX, int32& OutPY) const;

	// --- Events ---

	/** Fired on game thread when a tile finishes generating. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMapTileReady, FIntPoint);
	FOnMapTileReady OnMapTileReady;

protected:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

private:
	/** Pack an FIntPoint into a uint64 key for TMap/TSet (avoids GetTypeHash issues). */
	static uint64 PackTileKey(FIntPoint Coord);

	/** Unpack a uint64 key back to FIntPoint. */
	static FIntPoint UnpackTileKey(uint64 Key);

	/** Event-driven: called when VoxelChunkManager generates a new chunk. */
	UFUNCTION()
	void OnChunkGenerated(FIntVector ChunkCoord);

	/** Terrain conditioning changed (POI pads registered after the first tiles): rebuild every tile image. */
	void HandleTerrainConditioningChanged();

	/**
	 * Queue a tile for background generation and start what the concurrency cap allows.
	 * @param TileCoord   Tile coordinate
	 * @param bInvalidate The tile's current image is stale: a drawing already running is discarded and redone
	 * @param bPriority   Draw before the backlog (edit refreshes)
	 */
	void QueueTileGeneration(FIntPoint TileCoord, bool bInvalidate = false, bool bPriority = false);

	/** Start waiting tiles up to MaxConcurrentTileGenTasks. */
	void LaunchWaitingTiles();

	/**
	 * Launch a tile drawing (the queue already marked it drawing). Captures inputs on the game thread, draws on a
	 * background thread, stores the result back on the game thread.
	 * @param TileCoord  Tile coordinate
	 * @param Generation The tile's queue generation at start (a mismatch at completion discards the result)
	 */
	void GenerateTileAsync(FIntPoint TileCoord, uint32 Generation);

	/** Resolve the chunk manager and cache configuration. Returns true if ready. */
	bool ResolveChunkManager();

	/**
	 * Drop explored tiles outside the world (a save explored before exploration stopped at the edge).
	 * @return Number of tiles dropped
	 */
	int32 PruneExploredOutsideWorld();

	/** UVoxelEditManager::OnChunkEdited: update the edited-chunk index and mark the tile's overlay dirty. */
	void HandleChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius);

	/** Record whether a chunk has edits in TileEditedChunkZ. */
	void RecordChunkEdits(const FIntVector& ChunkCoord);

	/** Mark a tile's overlay for a rebuild at the next coalesced refresh. */
	void MarkOverlayDirty(uint64 TileKey);

	/** Arm the one-shot refresh timer (voxel.Map.EditRefreshSeconds) unless it is already running. */
	void ArmOverlayRefresh();

	/** Coalesced refresh: queue overlay rebuilds for dirty tiles whose edited chunks are loaded. */
	void FlushOverlayRefresh();

	/**
	 * Whether every edited chunk the tile's surface can depend on has voxel data: those at or above one chunk below
	 * the tile's lowest analytic surface (all of them while that is not known yet).
	 */
	bool CanRebuildOverlay(uint64 TileKey) const;

	/** Game thread: gather the tile's chunk stack (shared snapshots + edited-column tables) for an overlay build. */
	TSharedPtr<FVoxelMapOverlayBuildInput> GatherOverlayInput(FIntPoint TileCoord) const;

	/** Game thread: a planet tile's edited chunks and the loaded chunks around them, for FVoxelMapEditOverlay::BuildPlanet. */
	TSharedPtr<FVoxelMapPlanetOverlayInput> GatherPlanetOverlayInput(FIntPoint TileCoord) const;

	/** Game thread: the overlay build's inputs for a tile of this map's kind (GatherOverlayInput / GatherPlanetOverlayInput). */
	FVoxelMapOverlayInputs GatherOverlayInputs(FIntPoint TileCoord) const;

	/**
	 * Game thread: store a finished drawing (either world kind), unless it went stale or the subsystem is shutting down;
	 * then start the waiting tiles.
	 */
	void FinishTileDrawing(FIntPoint TileCoord, TArray<FColor>&& PixelData, int32 Resolution, uint32 Generation, bool bRebuild,
		const TSharedPtr<const FVoxelMapEditOverlay>& Overlay, bool bOverlayPending, float AnalyticMin, float AnalyticMax);

	/** The tile a step away: TileCoord + (DX, DY) on a plane; the grid's neighbour (across a face edge too) on a planet. */
	FIntPoint GetNeighbourTile(FIntPoint TileCoord, int32 DX, int32 DY) const;

	/** The tiles a chunk belongs to: its (X, Y) on a plane; on a planet the cells its corners span. */
	void GetChunkTiles(const FIntVector& ChunkCoord, TArray<FIntPoint>& OutTiles) const;

	// Cached references (resolved in Initialize or lazily)
	TWeakObjectPtr<UVoxelChunkManager> CachedChunkManagerWeak;

	/**
	 * Standalone analytic world mode OWNED by this subsystem (built from the world configuration in
	 * ResolveChunkManager — never borrowed from the chunk manager). Async tile tasks capture this
	 * TSharedPtr by value, so the instance outlives any in-flight task regardless of PIE-teardown
	 * order. The chunk manager frees ITS instance in EndPlay->Shutdown() while background tasks can
	 * still be running — capturing that raw pointer is what crashed on PIE stop.
	 *
	 * Carries the biome context as a VALUE snapshot (FVoxelBiomeSnapshot, set once on the game thread
	 * in CreateStandaloneWorldMode): still UObject-free — safe on a worker thread even after the world
	 * is GC'd — while GetTerrainHeightAt returns the TRUE generated surface height (continentalness
	 * offset + height-scale modulation, composed correctly per world mode).
	 */
	TSharedPtr<const IVoxelWorldMode> CachedWorldMode;

	FVoxelNoiseParams CachedNoiseParams;
	int32 CachedChunkSize = 32;
	float CachedVoxelSize = 100.0f;
	FVector CachedWorldOrigin = FVector::ZeroVector;
	bool bCacheResolved = false;

	/** The world mode's bounds (WM-I2): exploration only marks tiles that overlap them */
	FVoxelWorldBounds CachedWorldBounds;

	// Biome configuration (cached from VoxelWorldConfiguration)
	UPROPERTY()
	TObjectPtr<UVoxelBiomeConfiguration> CachedBiomeConfig;
	bool bBiomesEnabled = false;

	/**
	 * Map color per MaterialID (256 entries), resolved once in ResolveChunkManager: the material
	 * atlas' baked average-albedo colors where available, the material registry's palette otherwise.
	 * Plain data — captured by value by tile tasks.
	 */
	TArray<FColor> CachedMaterialPalette;

	/**
	 * The map's tile frame (code review CR2, REND-15): the kind of world, its tile grid (a planet's atlas or the chunk
	 * columns), the sea and the terrain's extent for the elevation and depth shading; everything the one tile drawer
	 * reads about the world. Built in ResolveChunkManager; the tile tasks copy it.
	 */
	FVoxelMapTileFrame TileFrame;

	// Tile storage: images under the budgets (WM-E2), and the exploration set (never evicted; saved)
	FVoxelMapTileStore TileStore;
	TSet<uint64> ExploredTiles;

	/** Tiles whose queued drawing should keep its detail image (the minimap's area, a zoomed-in world map) */
	TSet<uint64> DetailWanted;

	/** Which tiles wait for a (re)draw, which are drawing, and per-tile generations (game thread). */
	FVoxelMapTileQueue TileQueue;

	/** Binding on the chunk manager's OnTerrainConditioningChanged. */
	FDelegateHandle ConditioningChangedHandle;

	/** Explored tiles were imported before the map could resolve its world: queue their images once it can. */
	bool bGenerateExploredWhenReady = false;
	FCriticalSection TileMutex;		// Guards TileStore writes (made on the game thread when a drawing completes)

	/** Max concurrent async tile generation tasks. */
	static constexpr int32 MaxConcurrentTileGenTasks = 4;

	/** Whether the subsystem has bound to chunk manager delegates. */
	bool bDelegatesBound = false;

	// --- Edit overlays (R22), game thread ---

	/** Per-tile edit overlay. Immutable once stored: tile tasks share them by pointer. */
	TMap<uint64, TSharedPtr<const FVoxelMapEditOverlay>> EditOverlays;

	/** Chunks with edits, per tile (a chunk belongs to every tile it spans), kept in step with the edit manager through OnChunkEdited. */
	TMap<uint64, TArray<FIntVector>> TileEditedChunks;

	/** Tiles edited since the last refresh (coalescing buffer; water propagation edits every frame). */
	TSet<uint64> OverlayDirtyTiles;

	/** Tiles whose last overlay attempt lacked loaded chunks; retried when one of their chunks generates. */
	TSet<uint64> OverlayAwaitingTiles;

	/** Tiles whose next drawing gathers chunk data and rebuilds the overlay. */
	TSet<uint64> OverlayRebuildTiles;

	/** Lowest and highest analytic surface height per drawn tile (X = min, Y = max), for CanRebuildOverlay. */
	TMap<uint64, FVector2f> TileAnalyticRange;

	/** One-shot coalescing timer for FlushOverlayRefresh. */
	FTimerHandle OverlayRefreshTimer;

	// --- Planets (WM-SY8) ---

	/** The globe albedo (GetGlobeAlbedo), whether its build is in flight, and the albedo cells holding an explored tile. */
	TSharedPtr<const FVoxelMapGlobeAlbedo> GlobeAlbedo;
	bool bGlobeAlbedoBuilding = false;
	TSet<uint64> GlobeExploredCells;
	int32 GlobeExploredFromCount = -1;
	double GlobeExploredRefreshedSeconds = 0.0;

	/** GFrameCounter of the last overlay gather: LaunchWaitingTiles starts at most one rebuild per frame. */
	uint64 LastOverlayGatherFrame = MAX_uint64;

	/** The chunk manager's edit manager, and the OnChunkEdited binding on it. */
	TWeakObjectPtr<UVoxelEditManager> CachedEditManagerWeak;
	FDelegateHandle ChunkEditedHandle;

	/**
	 * Set in Deinitialize(). In-flight async completions arriving after this are discarded
	 * (no cache writes, no broadcasts) and no new generation tasks may start.
	 */
	bool bShuttingDown = false;
};
