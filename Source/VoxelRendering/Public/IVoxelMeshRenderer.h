// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ChunkRenderData.h"
#include "LODTypes.h"

// Forward declarations
class UWorld;
class UMaterialInterface;
class UVoxelWorldConfiguration;

/**
 * Voxel Mesh Renderer Interface.
 *
 * Abstract interface for rendering voxel chunks. Implementations handle
 * the actual rendering strategy (Custom Vertex Factory vs PMC).
 *
 * The hybrid architecture allows:
 * - FVoxelCustomVFRenderer: GPU-driven rendering for maximum runtime performance
 * - FVoxelPMCRenderer: ProceduralMeshComponent fallback for editor/tools
 *
 * Thread Safety: All methods must be called from game thread only.
 *
 * @see FVoxelCustomVFRenderer
 * @see FVoxelPMCRenderer
 * @see Documentation/RENDERING_SYSTEM.md
 */
class VOXELRENDERING_API IVoxelMeshRenderer
{
public:
	virtual ~IVoxelMeshRenderer() = default;

	// ==================== Lifecycle ====================

	/**
	 * Initialize renderer with world and configuration.
	 *
	 * Called once when the voxel world is created. Allocates resources
	 * and prepares the renderer for mesh updates.
	 *
	 * @param World The UWorld to render in
	 * @param WorldConfig World configuration asset
	 */
	virtual void Initialize(
		UWorld* World,
		const UVoxelWorldConfiguration* WorldConfig
	) = 0;

	/**
	 * Shutdown and cleanup all resources.
	 *
	 * Called when the voxel world is destroyed. Must release all
	 * GPU resources, components, and allocated memory.
	 */
	virtual void Shutdown() = 0;

	/**
	 * Check if renderer has been initialized.
	 *
	 * @return True if Initialize() has been called and Shutdown() has not
	 */
	virtual bool IsInitialized() const = 0;

	// ==================== Mesh Updates ====================

	/**
	 * Update or create mesh for a chunk.
	 *
	 * If the chunk already exists, its mesh is replaced.
	 * If new, the chunk is added to the render set.
	 *
	 * For Custom VF: RenderData contains GPU buffer references
	 * For PMC: RenderData may require CPU-side vertex data
	 *
	 * @param RenderData Chunk render data with geometry information
	 */
	virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) = 0;

	/**
	 * Update chunk mesh using CPU-side mesh data.
	 *
	 * Alternative to UpdateChunkMesh for when CPU mesh data is available
	 * (e.g., for PMC renderer or collision generation).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param LODLevel Current LOD level
	 * @param MeshData CPU-side vertex and index data
	 */
	virtual void UpdateChunkMeshFromCPU(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		const FChunkMeshData& MeshData
	) = 0;

	/**
	 * Remove chunk mesh from rendering.
	 *
	 * Releases all resources associated with the chunk.
	 *
	 * @param ChunkCoord Chunk coordinate to remove
	 */
	virtual void RemoveChunk(const FIntVector& ChunkCoord) = 0;

	/**
	 * Clear all chunk meshes.
	 *
	 * Removes and releases resources for all loaded chunks.
	 */
	virtual void ClearAllChunks() = 0;

	// ==================== Visibility ====================

	/**
	 * Set visibility for a specific chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param bVisible Whether chunk should be visible
	 */
	virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) = 0;

	/**
	 * Set visibility for all chunks.
	 *
	 * @param bVisible Whether all chunks should be visible
	 */
	virtual void SetAllChunksVisible(bool bVisible) = 0;

	// ==================== Material Management ====================

	/**
	 * Set primary material for all chunks.
	 *
	 * @param Material Material instance to use for rendering
	 */
	virtual void SetMaterial(UMaterialInterface* Material) = 0;

	/**
	 * Get the current material.
	 *
	 * @return Current material or nullptr if not set
	 */
	virtual UMaterialInterface* GetMaterial() const = 0;

	/**
	 * Force update of material parameters.
	 *
	 * Call after modifying material parameter values to ensure
	 * all chunks reflect the changes.
	 */
	virtual void UpdateMaterialParameters() = 0;

	// ==================== LOD Transitions ====================

	/**
	 * Update LOD transition morph factor for a chunk.
	 *
	 * Used for smooth LOD transitions in the vertex shader.
	 * Only applicable for Custom VF renderer.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param MorphFactor Blend factor 0-1 (0 = current LOD, 1 = next LOD)
	 */
	virtual void UpdateLODTransition(
		const FIntVector& ChunkCoord,
		float MorphFactor
	) = 0;

	/**
	 * Batch update LOD transitions for multiple chunks.
	 *
	 * More efficient than calling UpdateLODTransition individually.
	 *
	 * @param Transitions Array of chunk coordinates and morph factors
	 */
	virtual void UpdateLODTransitionsBatch(
		const TArray<TPair<FIntVector, float>>& Transitions
	)
	{
		// Default: call individual updates
		for (const auto& Pair : Transitions)
		{
			UpdateLODTransition(Pair.Key, Pair.Value);
		}
	}

	// ==================== Queries ====================

	/**
	 * Check if a chunk is currently loaded/rendered.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if chunk has mesh data loaded
	 */
	virtual bool IsChunkLoaded(const FIntVector& ChunkCoord) const = 0;

	/**
	 * Get number of currently loaded chunks.
	 *
	 * @return Count of loaded chunks
	 */
	virtual int32 GetLoadedChunkCount() const = 0;

	/**
	 * Get all currently loaded chunk coordinates.
	 *
	 * @param OutChunks Array to fill with loaded chunk coordinates
	 */
	virtual void GetLoadedChunks(TArray<FIntVector>& OutChunks) const = 0;

	/**
	 * Get total GPU memory usage (approximate).
	 *
	 * @return GPU memory in bytes
	 */
	virtual int64 GetGPUMemoryUsage() const = 0;

	/**
	 * Get total vertex count across all loaded chunks.
	 *
	 * @return Total vertices
	 */
	virtual int64 GetTotalVertexCount() const = 0;

	/**
	 * Get total triangle count across all loaded chunks.
	 *
	 * @return Total triangles
	 */
	virtual int64 GetTotalTriangleCount() const = 0;

	// ==================== Bounds ====================

	/**
	 * Get world bounds of a specific chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param OutBounds Output bounding box
	 * @return True if chunk exists and bounds were retrieved
	 */
	virtual bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const = 0;

	/**
	 * Get combined world bounds of all loaded chunks.
	 *
	 * @return Bounding box encompassing all loaded chunks
	 */
	virtual FBox GetTotalBounds() const = 0;

	// ==================== Debugging ====================

	/**
	 * Get debug statistics string.
	 *
	 * Returns human-readable stats including chunk count,
	 * memory usage, and performance metrics.
	 *
	 * @return Debug statistics string
	 */
	virtual FString GetDebugStats() const = 0;

	/**
	 * Draw debug visualization.
	 *
	 * Optional: Draw chunk bounds, LOD levels, etc.
	 *
	 * @param Context LOD query context for viewer information
	 */
	virtual void DrawDebugVisualization(const FLODQueryContext& Context) const
	{
		// Default: no visualization
	}

	/**
	 * Get renderer type name for debugging.
	 *
	 * @return "CustomVF", "PMC", etc.
	 */
	virtual FString GetRendererTypeName() const = 0;
};

/**
 * Renderer type enumeration.
 */
enum class EVoxelRendererType : uint8
{
	/** GPU-driven Custom Vertex Factory renderer */
	CustomVertexFactory,

	/** ProceduralMeshComponent-based renderer */
	ProceduralMeshComponent,

	/** Automatically select based on context (PIE vs Editor) */
	Auto
};
