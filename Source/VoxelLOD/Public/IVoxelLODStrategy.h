// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LODTypes.h"

// Forward declarations
class UWorld;
class UVoxelWorldConfiguration;

/**
 * LOD Strategy Interface for voxel terrain.
 *
 * All LOD implementations must conform to this interface. Strategies determine
 * which chunks should be loaded/rendered at what detail level based on
 * viewer position and world state.
 *
 * Performance: GetLODForChunk() is called frequently and must be fast (< 1us).
 * Thread Safety: All const methods must be thread-safe for read operations.
 *
 * Implementations:
 * - FDistanceBandLODStrategy (default) - Distance-based LOD rings
 * - FQuadtreeLODStrategy (future) - Screen-space adaptive for 2D terrain
 * - FOctreeLODStrategy (future) - 3D adaptive for spherical/cave systems
 *
 * @see FDistanceBandLODStrategy
 * @see Documentation/LOD_SYSTEM.md
 */
class VOXELLOD_API IVoxelLODStrategy
{
public:
	virtual ~IVoxelLODStrategy() = default;

	// ==================== Core Queries ====================

	/**
	 * Get LOD level for a chunk at given coordinate.
	 *
	 * Called frequently (per chunk per frame). Must be fast and thread-safe.
	 *
	 * Performance: < 1us per call
	 * Thread Safety: Must be thread-safe (const, no mutations)
	 *
	 * @param ChunkCoord Chunk position in chunk coordinate space
	 * @param Context Query context with viewer and world state
	 * @return LOD level (0 = finest detail, higher = coarser)
	 */
	virtual int32 GetLODForChunk(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const = 0;

	/**
	 * Get the LOD level of the distance band a chunk falls in, even when the LOD system is off.
	 *
	 * With LOD off every chunk renders at LOD 0, but the bands still describe distance. Systems
	 * that key on how far a chunk is rather than how coarse it is (far-chunk compression) use this
	 * so they keep working on LOD-off worlds, which includes every cubic world.
	 *
	 * Default: GetLODForChunk (a strategy without a LOD-off mode has nothing to add).
	 *
	 * @param ChunkCoord Chunk position in chunk coordinate space
	 * @param Context Query context with viewer and world state
	 * @return LOD level of the chunk's distance band (0 = nearest band)
	 */
	virtual int32 GetDistanceBandLOD(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const
	{
		return GetLODForChunk(ChunkCoord, Context);
	}

	// ==================== Visibility & Streaming ====================

	/**
	 * Get all chunks that should be visible this frame.
	 *
	 * Returns the complete set of chunks that need to be rendered,
	 * sorted by priority (highest first).
	 *
	 * @param Context Query context with viewer and world state
	 * @return Array of chunk requests with LOD and priority
	 */
	virtual TArray<FChunkLODRequest> GetVisibleChunks(
		const FLODQueryContext& Context
	) const = 0;

	/**
	 * Get chunks that need to be loaded (not currently loaded).
	 *
	 * Called by ChunkManager to determine what to generate/mesh.
	 *
	 * @param OutLoad Output array of chunks to load (sorted by priority)
	 * @param LoadedChunks Set of currently loaded chunk coordinates
	 * @param Context Query context with viewer and world state
	 */
	virtual void GetChunksToLoad(
		TArray<FChunkLODRequest>& OutLoad,
		const TSet<FIntVector>& LoadedChunks,
		const FLODQueryContext& Context
	) const = 0;

	/**
	 * Get every chunk that is out of range and may be unloaded, farthest first.
	 *
	 * Called by ChunkManager to free memory from distant chunks. Return ALL candidates and do NOT
	 * apply MaxChunksToUnloadPerFrame here: the caller first drops candidates it must keep (chunks
	 * still in the generation/meshing pipeline, edit-pinned chunks) and only then applies the
	 * per-frame cap (see SelectUnloadCandidates). A strategy-side cap let refused candidates use up
	 * the budget; because the loaded set iterates in a stable order, a handful of them could stop
	 * every later chunk from ever unloading (VOXEL_QA_FOLLOWUPS_PLAN.md R23d).
	 *
	 * @param OutUnload Output array of chunk coordinates to unload, sorted farthest first
	 * @param LoadedChunks Set of currently loaded chunk coordinates
	 * @param Context Query context with viewer and world state
	 */
	virtual void GetChunksToUnload(
		TArray<FIntVector>& OutUnload,
		const TSet<FIntVector>& LoadedChunks,
		const FLODQueryContext& Context
	) const = 0;

	/**
	 * Pick this frame's unloads from a candidate list (as returned by GetChunksToUnload).
	 *
	 * Candidates the caller refuses are skipped WITHOUT spending budget; selection stops once
	 * MaxPerFrame candidates have been accepted. Candidate order is kept, so a farthest-first
	 * list stays farthest first.
	 *
	 * Performance: O(n) in the number of candidates, one CanUnload call each until the cap.
	 *
	 * @param Candidates  Unload candidates in priority order (farthest first).
	 * @param CanUnload   Caller veto: return false to keep a candidate resident this frame.
	 * @param MaxPerFrame Cap on accepted candidates (<= 0 = no cap).
	 * @param OutSelected Receives the accepted candidates (reset first).
	 * @return Number of candidates selected.
	 */
	static int32 SelectUnloadCandidates(
		const TArray<FIntVector>& Candidates,
		TFunctionRef<bool(const FIntVector&)> CanUnload,
		int32 MaxPerFrame,
		TArray<FIntVector>& OutSelected)
	{
		OutSelected.Reset();
		for (const FIntVector& Coord : Candidates)
		{
			if (MaxPerFrame > 0 && OutSelected.Num() >= MaxPerFrame)
			{
				break;
			}
			if (CanUnload(Coord))
			{
				OutSelected.Add(Coord);
			}
		}
		return OutSelected.Num();
	}

	// ==================== Lifecycle ====================

	/**
	 * Initialize strategy from world configuration.
	 *
	 * Called once when the voxel world is created. Use this to
	 * cache configuration values and pre-compute any needed data.
	 *
	 * @param WorldConfig World configuration asset
	 */
	virtual void Initialize(const UVoxelWorldConfiguration* WorldConfig) = 0;

	/**
	 * Update strategy state each frame.
	 *
	 * Called every frame from game thread before any LOD queries.
	 * Use for temporal logic, hysteresis, or state updates.
	 *
	 * @param Context Query context with viewer and world state
	 * @param DeltaTime Time since last update in seconds
	 */
	virtual void Update(const FLODQueryContext& Context, float DeltaTime) = 0;

	// ==================== Optional Optimization ====================

	/**
	 * Get the distance (world units) beyond which a loaded chunk is out of range and
	 * should be unloaded. Also used to cull chunks from the generation/mesh queue before
	 * they ever load, when the viewer has moved far past them while they sat in the backlog.
	 *
	 * Default: -1 (no unload horizon -> stale-culling disabled for this strategy).
	 *
	 * @return Unload horizon distance in world units, or <= 0 to disable.
	 */
	virtual float GetUnloadDistance() const
	{
		return -1.0f;
	}

	/**
	 * Get the load horizon (world units): the distance within which the strategy streams chunks in
	 * (the view edge). Smaller than GetUnloadDistance(), whose margin is hysteresis, not view range.
	 * The chunk manager pin-loads chunks that exist only because of edits within this radius, so an
	 * edited chunk never appears on its own beyond the rest of the terrain, and keeps them resident
	 * out to GetUnloadDistance().
	 *
	 * Default: -1 (no load horizon; callers fall back to GetUnloadDistance()).
	 *
	 * @return Load horizon distance in world units, or <= 0 when the strategy defines none.
	 */
	virtual float GetLoadDistance() const
	{
		return -1.0f;
	}

	/**
	 * The distance the load and unload horizons (GetLoadDistance, GetUnloadDistance) are measured in, from the viewer
	 * to a chunk's centre. Everything that asks whether a chunk is in range (the strategy's own load and unload, the
	 * chunk manager's queue eviction, edit pins and residency audit) uses it, so they cannot disagree (KI11).
	 *
	 * Default: 3D distance. FDistanceBandLODStrategy: horizontal distance in heightmap worlds, whose vertical extent is
	 * each column's own window (world modes review D8), so the viewer's altitude never removes the ground under it; on a
	 * planet the distance along the ground from the viewer's ground point, each chunk's radial extent being its slab
	 * (spherical Phase 6, WM-BP8).
	 *
	 * @param ChunkCenter Chunk centre, world space
	 * @param ViewerPosition Viewer position, world space
	 * @return Distance compared against the horizons, world units
	 */
	virtual float GetStreamingHorizon(const FVector& ChunkCenter, const FVector& ViewerPosition) const
	{
		return static_cast<float>(FVector::Dist(ChunkCenter, ViewerPosition));
	}

	/**
	 * Whether the window is empty because the viewer is too high for voxels to matter (spherical Phase 6, WM-BP10, D18: a
	 * planet's viewer above its voxel cut-over altitude, where the far field alone draws the planet). While it is,
	 * GetVisibleChunks returns nothing and GetChunksToUnload every loaded chunk, and the chunk manager neither pins edited
	 * chunks nor keeps queued ones. Set by Update.
	 *
	 * Default: false (the window never empties).
	 *
	 * @return True while the window is suspended
	 */
	virtual bool IsWindowSuspended() const
	{
		return false;
	}

	/**
	 * The altitude over the surface under the viewer above which the window suspends (IsWindowSuspended), and the
	 * viewer's altitude over that surface as the last Update saw it (spherical Phase 6, WM-BP10).
	 *
	 * Default: 0 (never) and 0.
	 */
	virtual double GetCutoverAltitude() const
	{
		return 0.0;
	}

	/**
	 * Whether the chunk manager has a far field to draw the world beyond the window: the cut-over is in force only
	 * while it has (D38; every mode since G78). The chunk manager sets it when the far field is built or released and
	 * every Update's context carries it (FLODQueryContext::bFarFieldActive), so GetCutoverAltitude is right before the
	 * first Update too. Default: nothing to set.
	 */
	virtual void SetFarFieldActive(bool bActive)
	{
	}
	virtual double GetViewerSurfaceAltitude() const
	{
		return 0.0;
	}

	// ==================== Debugging ====================

	/**
	 * Get debug information string.
	 *
	 * Returns human-readable debug info for on-screen display
	 * and logging. Include strategy name, configuration, and stats.
	 *
	 * @return Debug information string
	 */
	virtual FString GetDebugInfo() const = 0;

	/**
	 * Draw debug visualization in viewport.
	 *
	 * Optional: Draw LOD zones, chunk bounds, priority heat maps, etc.
	 * Only called when debug visualization is enabled.
	 *
	 * @param World World to draw in
	 * @param Context Query context
	 */
	virtual void DrawDebugVisualization(
		UWorld* World,
		const FLODQueryContext& Context
	) const
	{
		// Default: no visualization
	}
};
