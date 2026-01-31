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
	 * Get morph factor for LOD transition blending.
	 *
	 * Used to smoothly blend between LOD levels in the vertex shader.
	 * Returns 0 when fully at current LOD, 1 when ready to transition to next.
	 *
	 * @param ChunkCoord Chunk position in chunk coordinate space
	 * @param Context Query context with viewer and world state
	 * @return Morph factor 0-1 (0 = current LOD, 1 = next LOD)
	 */
	virtual float GetLODMorphFactor(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const = 0;

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
	 * Should respect MaxChunksToLoadPerFrame from context.
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
	 * Get chunks that should be unloaded (no longer needed).
	 *
	 * Called by ChunkManager to free memory from distant chunks.
	 * Should respect MaxChunksToUnloadPerFrame from context.
	 *
	 * @param OutUnload Output array of chunk coordinates to unload
	 * @param LoadedChunks Set of currently loaded chunk coordinates
	 * @param Context Query context with viewer and world state
	 */
	virtual void GetChunksToUnload(
		TArray<FIntVector>& OutUnload,
		const TSet<FIntVector>& LoadedChunks,
		const FLODQueryContext& Context
	) const = 0;

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
	 * Should this chunk be updated this frame?
	 *
	 * Optional throttling mechanism. Return false to skip expensive
	 * updates for chunks that haven't changed significantly.
	 *
	 * Default: Always returns true
	 *
	 * @param ChunkCoord Chunk position
	 * @param Context Query context
	 * @return True if chunk should be updated
	 */
	virtual bool ShouldUpdateChunk(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const
	{
		return true;
	}

	/**
	 * Get priority for chunk generation/loading.
	 *
	 * Higher priority chunks are processed first. Used for sorting
	 * the generation queue.
	 *
	 * Default: Returns inverse distance (closer = higher priority)
	 *
	 * @param ChunkCoord Chunk position
	 * @param Context Query context
	 * @return Priority value (higher = more important)
	 */
	virtual float GetChunkPriority(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const
	{
		return 1.0f;
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
