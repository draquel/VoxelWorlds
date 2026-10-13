// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelMapTypes.h"
#include "VoxelMapEditOverlay.h"
#include "VoxelMapTileQueue.h"
#include "VoxelCoreTypes.h"
#include "VoxelEditTypes.h"
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
 * edit-merged snapshots (UVoxelChunkManager::GetEditMergedSnapshot) and redraw, building the overlay in the tile
 * task. Tiles whose chunks are not loaded keep their current overlay until a chunk of theirs generates.
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

	/** Get a generated tile. Returns nullptr if not yet generated. */
	const FVoxelMapTile* GetTile(FIntPoint TileCoord) const;

	/** Check if a tile has been generated (regardless of exploration state). */
	bool HasTile(FIntPoint TileCoord) const;

	/** Get the full tile cache (for bulk iteration by UI). */
	const TMap<uint64, FVoxelMapTile>& GetTileCache() const { return TileCache; }

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

	// Biome configuration (cached from VoxelWorldConfiguration)
	UPROPERTY()
	TObjectPtr<UVoxelBiomeConfiguration> CachedBiomeConfig;
	bool bBiomesEnabled = false;

	// Water level (cached from VoxelWorldConfiguration)
	bool bWaterEnabled = false;
	float CachedWaterLevel = 0.0f;

	/**
	 * Map color per MaterialID (256 entries), resolved once in ResolveChunkManager: the material
	 * atlas' baked average-albedo colors where available, the material registry's palette otherwise.
	 * Plain data — captured by value by tile tasks.
	 */
	TArray<FColor> CachedMaterialPalette;

	/**
	 * Terrain height bounds from the world mode (IVoxelWorldMode::GetTerrainHeightBounds) — the
	 * elevation-tint and water-depth gradients are scaled to this range instead of hardcoded
	 * constants, so shading adapts to the world configuration.
	 */
	float CachedTerrainMinHeight = 0.0f;
	float CachedTerrainMaxHeight = 0.0f;

	// Tile storage
	TMap<uint64, FVoxelMapTile> TileCache;
	TSet<uint64> ExploredTiles;

	/** Which tiles wait for a (re)draw, which are drawing, and per-tile generations (game thread). */
	FVoxelMapTileQueue TileQueue;

	/** Binding on the chunk manager's OnTerrainConditioningChanged. */
	FDelegateHandle ConditioningChangedHandle;

	/** Explored tiles were imported before the map could resolve its world: queue their images once it can. */
	bool bGenerateExploredWhenReady = false;
	FCriticalSection TileMutex;		// Protects TileCache writes from async tasks

	/** Max concurrent async tile generation tasks. */
	static constexpr int32 MaxConcurrentTileGenTasks = 4;

	/** Whether the subsystem has bound to chunk manager delegates. */
	bool bDelegatesBound = false;

	// --- Edit overlays (R22), game thread ---

	/** Per-tile edit overlay. Immutable once stored: tile tasks share them by pointer. */
	TMap<uint64, TSharedPtr<const FVoxelMapEditOverlay>> EditOverlays;

	/** Chunk Zs with edits, per tile, kept in step with the edit manager through OnChunkEdited. */
	TMap<uint64, TArray<int32>> TileEditedChunkZ;

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
