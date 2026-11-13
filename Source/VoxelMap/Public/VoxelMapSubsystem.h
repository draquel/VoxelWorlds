// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelMapTypes.h"
#include "VoxelMapEditOverlay.h"
#include "VoxelMapExploredMask.h"
#include "VoxelMapTileFrame.h"
#include "VoxelMapTileQueue.h"
#include "VoxelMapTileStore.h"
#include "VoxelCoreTypes.h"
#include "VoxelEditTypes.h"
#include "VoxelWorldModeTypes.h"
#include "VoxelCubeSphere.h"
#include "VoxelWaterLevels.h"
#include "TimerManager.h"
#include "VoxelMapSubsystem.generated.h"

class IVoxelWorldMode;
class UVoxelChunkManager;
class UVoxelEditManager;
class UVoxelBiomeConfiguration;

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
 * The tile pyramid (MAP_PYRAMID_PLAN.md P1, D84): a tile has a level (FVoxelMapTileId; level L covers 2^L x 2^L
 * level-0 tiles at the same pixels), and images are drawn for the views, not for exploration. Each view (the minimap,
 * the world map) asks for the tiles it shows at the level matching its texels (RequestView, LevelForTexel), nearest its
 * centre first; a new view replaces that view's last request. The images live in one least-recently-used store of every
 * level under one budget (FVoxelMapTileStore, voxel.Map.TileBudgetMB). Exploration is separate and never evicted, and
 * marking a tile explored draws nothing.
 *
 * The explored mask (MAP_PYRAMID_PLAN.md P2): exploration is an FVoxelMapExploredMask over level-0 tiles (bitmap blocks
 * of 64 x 64 tiles, full blocks collapsed), so a whole explored planet costs kilobytes, ExploreAll fills a world at once,
 * and a view's fog of war is read from it per texel at any level. On a planet the top levels of the pyramid (the
 * coarsest with at most 4 x 4 tiles a face, and those above it) are drawn when the map resolves its world and kept
 * (EVoxelMapRequester::Prefetch, pinned in the store), so the globe zoomed out always has an image.
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
	 * Get a level-0 tile's image. Counts as a use, which keeps the image from eviction.
	 * @return The image, or nullptr if the tile has none (not drawn yet, or evicted)
	 */
	const FVoxelMapTile* GetTile(FIntPoint TileCoord) const;

	/**
	 * Get a tile's image at any level of the pyramid (MAP_PYRAMID_PLAN.md P1).
	 * @param Id The tile
	 * @param bTouch Count this read as a use (keeps the image from eviction)
	 * @return The image (possibly stale: its redraw is queued), or nullptr if the tile has none
	 */
	const FVoxelMapTile* FindTile(const FVoxelMapTileId& Id, bool bTouch = true) const;

	/** Check if a level-0 tile has an image, regardless of exploration state. */
	bool HasTile(FIntPoint TileCoord) const;

	/**
	 * A view's request (MAP_PYRAMID_PLAN.md P1, D84): the tiles a view shows, at the level matching its texels
	 * (LevelForTexel), nearest its centre first. It replaces the requester's last request: tiles that left the view stop
	 * waiting. The tiles without an image (or with a stale one) are queued, the minimap's before the world map's; the
	 * images arrive through OnMapTileReady. An empty request clears the requester's (a view closed).
	 * @param Requester The view (EVoxelMapRequester::Minimap or WorldMap)
	 * @param NearestFirst The tiles it shows
	 * @return Number of tiles queued
	 */
	int32 RequestView(EVoxelMapRequester Requester, TConstArrayView<FVoxelMapTileId> NearestFirst);

	/**
	 * The pyramid level whose pixels match a texel of a view (FVoxelMapTileFrame::LevelForTexel): the coarsest whose
	 * pixels are at most the texel apart.
	 * @param TexelWorldSize The texel's size (uu)
	 * @return The level (0 before the map has resolved its world)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	int32 LevelForTexel(double TexelWorldSize) const { return bCacheResolved ? TileFrame.LevelForTexel(TexelWorldSize) : 0; }

	/** The tile images at every level and their budget (diagnostics, tests). */
	const FVoxelMapTileStore& GetTileStore() const { return TileStore; }

	/** The draw queue (diagnostics, tests). */
	const FVoxelMapTileQueue& GetTileQueue() const { return TileQueue; }

	/** Apply voxel.Map.TileBudgetMB (called when it changes). */
	void ApplyTileBudgets();

	/**
	 * Debug (voxel.Map.ExploreAll): mark every tile that overlaps the world's bounds explored, as a fully explored save
	 * restores it, filling the explored mask with no cap (MAP_PYRAMID_PLAN.md P2): a planet's six faces (16 million tiles on
	 * a 50 km planet, whole blocks at once), an island's bounds; on an unbounded world the square of tiles within Radius of
	 * Center. No image is queued: the views ask for what they show (P1).
	 * @param Center The pawn (an unbounded world's square is around it)
	 * @param Radius An unbounded world's half square (uu)
	 * @return Number of tiles of the area, explored now (those explored before included)
	 */
	int64 ExploreAllTiles(const FVector& Center, float Radius);

	// --- The globe zoomed out (MAP_PYRAMID_PLAN.md P2) ---

	/** A planet's top levels are prefetched from the coarsest with at most this many tiles along a face's edge. */
	static constexpr int32 PrefetchTilesPerFaceEdge = 4;

	/**
	 * The finest of the pyramid levels drawn when the map resolves a planet and kept (pinned in the store; with the levels
	 * above it, 6 x 16 + 6 x 4 + 6 tiles from level 9 on the 50 km planet): the globe zoomed out always has an image. They
	 * replace the globe albedo (spherical Phase 6's WM-BP11).
	 * @return The level, or INDEX_NONE (a heightmap world, or before the map has resolved its world)
	 */
	int32 GetPrefetchLevel() const { return PrefetchLevel; }

	// --- Exploration ---

	/**
	 * Mark the tiles in a radius explored (the minimap calls it for its area every refresh). Draws nothing: the views
	 * ask for the images they show (RequestView).
	 *
	 * @param WorldPos Player or camera world position
	 * @param Radius World-unit radius around the position to explore
	 */
	void RequestTilesInRadius(const FVector& WorldPos, float Radius);

	/** Check if a level-0 tile has been explored (one lookup in the explored mask). */
	bool IsTileExplored(FIntPoint TileCoord) const;

	/**
	 * The exploration (MAP_PYRAMID_PLAN.md P2): a mask over level-0 tiles by face, (Face, U, V) the cell of the cube-sphere
	 * grid's face on a planet (TileToMaskTile), (0, X, Y) the chunk column on a heightmap world. A view reads its fog of war
	 * from it per texel (FVoxelMapExploredMask::FReader::IsAnyExplored at the view's level).
	 */
	const FVoxelMapExploredMask& GetExploredMask() const { return ExploredMask; }

	/**
	 * A level-0 tile's place in the explored mask.
	 * @param TileCoord The tile (a planet's atlas coordinate; a chunk column)
	 * @param OutFace The face (0 on a heightmap world)
	 * @param OutU The tile along the face (X on a heightmap world)
	 * @param OutV The tile across it (Y)
	 * @return False for a tile outside a planet's atlas
	 */
	bool TileToMaskTile(FIntPoint TileCoord, int32& OutFace, int32& OutU, int32& OutV) const;

	/** @return The number of explored level-0 tiles */
	int64 GetNumExploredTiles() const { return ExploredMask.Num(); }

	/** A count bumped whenever a tile is newly explored: a view whose fog of war may have changed draws again. */
	uint64 GetExplorationRevision() const { return ExplorationRevision; }

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

	/**
	 * Every explored tile's coordinate (a planet's atlas coordinate; a chunk column). O(explored): tests and diagnostics.
	 * @param OutTiles Receives the tiles (emptied first)
	 */
	void GetExploredTiles(TArray<FIntPoint>& OutTiles) const;

	/**
	 * The explored tiles as a compact blob for a save (MAP_PYRAMID_PLAN.md P2): the explored mask's blocks, full ones by
	 * coordinate and partial ones by their bits (EncodeExploredMask), a few kilobytes for a whole explored planet. A string
	 * keeps it JSON-safe.
	 * @param OutCount Receives the number of tiles explored (at most MAX_int32; 0 when nothing is)
	 * @return The encoded mask; empty when nothing is explored
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	FString ExportExploredTiles(int32& OutCount) const;

	/**
	 * Restore explored tiles saved by ExportExploredTiles: the mask's blob, or the coordinate pairs saves held before the
	 * mask (EncodeTileCoords), converted. Queues no image: the views ask for what they show (MAP_PYRAMID_PLAN.md P1).
	 * Merges into the current exploration: it only grows, and tiles already explored this session (the spawn area streams
	 * in before a save loads) stay explored. Before the map has resolved its world the blob waits for it.
	 * @param Encoded The blob from ExportExploredTiles
	 * @param Count   The tile count ExportExploredTiles reported (the coordinate pairs need it to decode)
	 * @return Number of tiles the blob holds (0 for an empty or malformed blob)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	int32 ImportExploredTiles(const FString& Encoded, int32 Count);

	/**
	 * Pure: encode an explored mask (see ExportExploredTiles). Binary: magic 'VMXM' and the uncompressed size, then the
	 * zlib-compressed FVoxelMapExploredMask::Save data; base64-encoded.
	 * @param Mask The mask
	 * @return The blob; empty for an empty mask or a compression failure
	 */
	static FString EncodeExploredMask(const FVoxelMapExploredMask& Mask);

	/**
	 * Pure: decode EncodeExploredMask output.
	 * @param Encoded The blob
	 * @param OutMask Receives the mask (its configuration too)
	 * @return False when the blob is not a mask blob or is malformed (OutMask left empty)
	 */
	static bool DecodeExploredMask(const FString& Encoded, FVoxelMapExploredMask& OutMask);

	/**
	 * Pure: encode tile coordinates, the explored blob before the mask (ImportExploredTiles still reads it).
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
	 * Redraw every tile image, keeping the current images until the new ones land: the tiles the views show are queued, the
	 * rest marked stale and drawn again when a view asks for them.
	 * @param bRebuildOverlays Also rebuild the edit overlay of every tile with edits (from loaded chunks)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Map")
	void RedrawAllTiles(bool bRebuildOverlays);

	/**
	 * One-line state summary for diagnostics (voxel.Map.Stats).
	 * @return Cached / explored / queued tiles, the drawing counters since the last reset, overlays and their pending state
	 */
	FString GetDebugStats() const;

	/** Restart the drawing counters GetDebugStats reports (voxel.Map.Stats reset; the map scaling plan's M0, G163). */
	void ResetDebugStats();

	/**
	 * Debug (voxel.Map.Capture): write the explored tiles around a world position to a PNG, north up as the world map
	 * draws them, with the position marked. Copies the cached tile pixels, each level-0 pixel from the finest level stored
	 * over it, so before/after comparisons need no UI capture.
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

	/** Fired on game thread when a tile's image (at any level) is stored. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMapTileReady, const FVoxelMapTileId&);
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

	/** Mark a level-0 tile explored (ExplorationRevision counts the new ones). */
	void MarkExplored(FIntPoint TileCoord);

	/** Apply an explored blob to the mask (the world resolved). @return Tiles the blob holds, -1 when malformed */
	int32 ApplyExploredBlob(const FString& Encoded, int32 Count);

	/** On a planet: ask for the top levels' tiles and pin them (GetPrefetchLevel), when the map resolves its world. */
	void PrefetchTopLevels();

	/**
	 * A tile's image is stale (an edit or the terrain conditioning changed under it): a drawing running is discarded, the
	 * stored image stays on screen marked stale, and the tile is queued again for each view that shows it.
	 * @param Id The tile
	 * @return True if the tile had an image or a drawing
	 */
	bool InvalidateTile(const FVoxelMapTileId& Id);

	/** InvalidateTile for the coarser tiles over a level-0 tile (its edit overlay changed: their pixels read it). */
	void InvalidateAncestors(FIntPoint Tile0);

	/** Mark every stored image stale and queue again the ones the views show (RedrawAllTiles, a conditioning change). */
	int32 InvalidateAllTiles();

	/**
	 * The colour of a level-0 pixel from the finest level stored over it (voxel.Map.Capture).
	 * @return False when no level holds it
	 */
	bool SampleStoredPixel(FIntPoint Tile0, int32 PX, int32 PY, FColor& OutColor) const;

	/** Start waiting tiles up to MaxConcurrentTileGenTasks. */
	void LaunchWaitingTiles();

	/**
	 * Launch a tile drawing (the queue already marked it drawing). Captures inputs on the game thread, draws on a
	 * background thread, stores the result back on the game thread.
	 * @param Id         The tile, at any level
	 * @param Generation The tile's queue generation at start (a mismatch at completion discards the result)
	 */
	void GenerateTileAsync(const FVoxelMapTileId& Id, uint32 Generation);

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
	void FinishTileDrawing(const FVoxelMapTileId& Id, TArray<FColor>&& PixelData, int32 Resolution, uint32 Generation, bool bRebuild,
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

	// Tile storage: images of every level under one budget (MAP_PYRAMID_PLAN.md P1), and the exploration (level-0 tiles,
	// never evicted; saved; P2)
	FVoxelMapTileStore TileStore;
	FVoxelMapExploredMask ExploredMask;

	/** Explored blobs imported before the map resolved its world (the mask's faces come from the world): applied then. */
	TArray<TPair<FString, int32>> PendingExploredBlobs;

	/** The finest prefetched level (GetPrefetchLevel), INDEX_NONE without. */
	int32 PrefetchLevel = INDEX_NONE;

	/** Bumped when a tile is newly explored (GetExplorationRevision). */
	uint64 ExplorationRevision = 0;

	/** Which tiles wait for a (re)draw, by requester, which are drawing, and per-tile generations (game thread). */
	FVoxelMapTileQueue TileQueue;

	/** What each view shows, as it last asked (RequestView): an invalidated tile in a view is queued again for it. */
	TSet<FVoxelMapTileId> ViewTiles[static_cast<int32>(EVoxelMapRequester::Num)];

	/** Binding on the chunk manager's OnTerrainConditioningChanged. */
	FDelegateHandle ConditioningChangedHandle;

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

	/**
	 * Drawing counters since the last ResetDebugStats (voxel.Map.Stats; the map scaling plan's M0, G163): how fast tiles
	 * draw, how many drawings replaced an image the store still held, and how many images a view asked for.
	 */
	struct FDrawStats
	{
		/** Drawings stored. */
		int64 Drawn = 0;
		/** Of those, drawings of a tile the store already held (an invalidation). */
		int64 Replaced = 0;
		/** Tiles the views' requests queued (RequestView). */
		int64 ImageRequests = 0;
		/** The most tiles waiting at once. */
		int32 WaitingPeak = 0;
		/** Worker seconds spent drawing the stored tiles. */
		double DrawSeconds = 0.0;
		/** FPlatformTime::Seconds() at the last reset. */
		double SinceSeconds = 0.0;
	};
	FDrawStats DrawStats;

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
