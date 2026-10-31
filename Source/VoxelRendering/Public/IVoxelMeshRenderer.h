// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ChunkRenderData.h"
#include "LODTypes.h"

// Forward declarations
class UWorld;
class UMaterialInterface;
class UVoxelWorldConfiguration;
class UMaterialParameterCollection;
class UVoxelMaterialAtlas;

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
	 * Update or create a chunk's mesh from CPU-side mesh data (the chunk manager's path).
	 *
	 * If the chunk already exists, its mesh is replaced.
	 * If new, the chunk is added to the render set.
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
	 * Move-enabled variant of UpdateChunkMeshFromCPU for callers that are done with the mesh
	 * data. Renderers that can consume the arrays directly (e.g. the custom-VF renderer moves
	 * the index buffer straight into its pending batch) override this to avoid copying; the
	 * default forwards to the const& version.
	 */
	virtual void UpdateChunkMeshFromCPU(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		FChunkMeshData&& MeshData
	)
	{
		UpdateChunkMeshFromCPU(ChunkCoord, LODLevel, static_cast<const FChunkMeshData&>(MeshData));
	}

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
	 * The material terrain is actually drawn with: the material instance carrying the atlas parameters when the
	 * renderer made one, else GetMaterial(). Other meshes that continue the terrain (the far ocean's seabed, WM-E1)
	 * use it with terrain vertex data (UV1.x = material id).
	 *
	 * @return The material, or null
	 */
	virtual UMaterialInterface* GetSurfaceMaterial() const { return GetMaterial(); }

	/**
	 * Force update of material parameters.
	 *
	 * Call after modifying material parameter values to ensure
	 * all chunks reflect the changes.
	 */
	virtual void UpdateMaterialParameters() = 0;

	/**
	 * Set the material used for water mesh sections.
	 *
	 * Per-chunk water meshes are rendered in a separate mesh section
	 * with this material (typically translucent or Single Layer Water).
	 *
	 * @param Material Water material to use, or nullptr to disable water mesh sections
	 */
	virtual void SetWaterMaterial(UMaterialInterface* Material)
	{
		// Default: no-op (renderer doesn't support per-chunk water)
	}

	/**
	 * Update water mesh for a 2D tile (XY chunk column).
	 *
	 * Water tiles are independent of 3D terrain chunks. Each tile has its own
	 * dedicated PMC positioned at the tile's world location.
	 *
	 * @param TileCoord 2D tile coordinate (XY chunk column)
	 * @param WaterMeshData CPU-side water surface mesh data
	 */
	virtual void UpdateWaterTileMesh(
		const FIntVector2& TileCoord,
		const FChunkMeshData& WaterMeshData)
	{
		// Default: no-op (renderer doesn't support water tiles)
	}

	/**
	 * Remove water tile and release its resources.
	 *
	 * @param TileCoord 2D tile coordinate to remove
	 */
	virtual void RemoveWaterTile(const FIntVector2& TileCoord)
	{
		// Default: no-op (renderer doesn't support water tiles)
	}

	/**
	 * Remove all water tiles.
	 * Called during Shutdown/ClearAllChunks.
	 */
	virtual void ClearAllWaterTiles()
	{
		// Default: no-op (renderer doesn't support water tiles)
	}

	// ==================== Seam Meshes (seam-ownership P1/P2) ====================
	// Single-owner seam meshes (SEAM_OWNERSHIP_ARCHITECTURE.md §2.2/§2.4): small
	// independently-swappable meshes covering the boundaries a chunk OWNS, keyed by
	// (owner chunk, SeamSlot). SeamSlot encoding: 0-2 = face seams (+X/+Y/+Z), 3-5 = edge
	// seams (parallel to X/Y/Z), 6 = the corner seam — up to 7 buckets per chunk. Rendered
	// with the terrain material (including RVT passes) but outside the per-chunk mesh /
	// crossfade machinery. Positions are OWNER-LOCAL (the renderer applies the owner's
	// world offset).

	/**
	 * Update (or create) one seam mesh owned by a chunk.
	 * An invalid/empty mesh removes the bucket (a seam whose surface vanished).
	 *
	 * @param OwnerChunkCoord Owning (min-coordinate) chunk
	 * @param SeamSlot Bucket slot: 0-2 face +X/+Y/+Z, 3-5 edge parallel X/Y/Z, 6 corner
	 * @param LODLevel Shared LOD the seam was meshed at
	 * @param MeshData Seam mesh in owner-local space (moved)
	 * @param bCrossfade Crossfade from the bucket being replaced (or to nothing, for an empty mesh)
	 *        instead of swapping instantly, where the renderer supports it (R24)
	 */
	virtual void UpdateSeamMeshFromCPU(
		const FIntVector& OwnerChunkCoord,
		uint8 SeamSlot,
		int32 LODLevel,
		FChunkMeshData&& MeshData,
		bool bCrossfade = false)
	{
		// Default: no-op (renderer doesn't support seam meshes)
	}

	/**
	 * Hold a chunk's next (or current) mesh-swap crossfade at alpha 0 — the previous mesh stays
	 * fully visible, the new one is uploaded but hidden — until released with bHold = false, which
	 * starts the fade. Used to swap an LOD-changed interior together with its rebuilt seams (R24).
	 *
	 * @param ChunkCoord Chunk whose swap to hold
	 * @param bHold True to hold, false to release
	 */
	virtual void SetChunkFadeHold(const FIntVector& ChunkCoord, bool bHold)
	{
		// Default: no-op (renderer doesn't crossfade)
	}

	/**
	 * Hold an owner's seam bucket crossfades at alpha 0 until released (see SetChunkFadeHold).
	 *
	 * @param OwnerChunkCoord Seam bucket owner
	 * @param bHold True to hold, false to release
	 */
	virtual void SetSeamFadeHold(const FIntVector& OwnerChunkCoord, bool bHold)
	{
		// Default: no-op (renderer doesn't crossfade)
	}

	/**
	 * Seconds until an owner's seam bucket finishes the crossfade it is running (0 when it is not
	 * fading, is held, or this renderer does not crossfade). A bucket replaced mid-fade loses the
	 * mesh it was fading out of, so the seam pipeline defers a resubmit until this reaches 0. A
	 * bucket fading in from nothing (R24 P3) counts as fading too.
	 *
	 * @param OwnerChunkCoord Seam bucket owner
	 * @return Remaining fade time in seconds
	 */
	virtual float GetSeamFadeRemainingSeconds(const FIntVector& OwnerChunkCoord) const
	{
		return 0.0f;
	}

	/**
	 * Tell the renderer where the streaming viewer is (sent every tick). A renderer that fades new
	 * chunks and seam buckets in from nothing (R24 P3) does so only beyond a distance from it, so
	 * nothing near the player turns see-through.
	 *
	 * @param ViewerPosition World-space viewer position
	 */
	virtual void SetViewerPosition(const FVector& ViewerPosition)
	{
		// Default: no-op (renderer doesn't fade new geometry in)
	}

	/**
	 * True if a first mesh submitted for this chunk now would fade in from nothing (R24 P3). The
	 * chunk manager then holds it (SetChunkFadeHold) until its ready seams land, so the interior and
	 * its seams fade in together.
	 *
	 * @param ChunkCoord Chunk about to receive its first mesh
	 * @return True if its appearance would fade
	 */
	virtual bool WillFadeInChunk(const FIntVector& ChunkCoord) const
	{
		return false;
	}

	/**
	 * Remove one owned seam mesh (owner unloaded or seam dissolved).
	 *
	 * @param OwnerChunkCoord Owning chunk
	 * @param SeamSlot Bucket slot: 0-2 face +X/+Y/+Z, 3-5 edge parallel X/Y/Z, 6 corner
	 */
	virtual void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 SeamSlot)
	{
		// Default: no-op (renderer doesn't support seam meshes)
	}

	/**
	 * Remove all seam meshes. Called during Shutdown/ClearAllChunks.
	 */
	virtual void ClearAllSeamMeshes()
	{
		// Default: no-op (renderer doesn't support seam meshes)
	}

	/**
	 * List the owners that currently have at least one seam bucket (diagnostics: the chunk
	 * manager's residency audit flags buckets whose owner is no longer tracked).
	 *
	 * @param OutOwners Receives the owner chunk coordinates (reset first; empty when unsupported)
	 */
	virtual void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const
	{
		OutOwners.Reset(); // Default: renderer doesn't support seam meshes
	}

	/**
	 * Report (once) that the renderer lost the geometry it had been given, so the caller can send it again.
	 * A renderer whose GPU data lives in a scene proxy loses every chunk mesh, seam bucket and water tile
	 * when the engine recreates that proxy (a material or render-state change): the new proxy starts empty.
	 * Game thread; the flag is cleared by the call.
	 *
	 * @param OutChunks Receives the chunks that had a mesh (reset first)
	 * @return True if geometry was lost since the last call
	 */
	virtual bool ConsumeLostRenderData(TArray<FIntVector>& OutChunks)
	{
		OutChunks.Reset(); // Default: the renderer keeps its geometry
		return false;
	}

	/**
	 * Diagnostics (world modes review G77): what the renderer holds for a water tile and when it last drew it.
	 * Game thread; may flush rendering commands.
	 *
	 * @param TileCoord The tile (chunk column)
	 * @param Out       Receives a line (the region, the component, the proxy)
	 * @return True if the renderer holds a mesh for the tile
	 */
	virtual bool GetWaterTileDrawInfo(const FIntVector2& TileCoord, FString& Out) const
	{
		Out = TEXT("not reported by this renderer");
		return false;
	}

	/**
	 * Set material atlas for texture lookup.
	 *
	 * The material atlas defines how MaterialIDs map to atlas positions
	 * and provides the LUT texture for per-face texture variants.
	 *
	 * @param Atlas Material atlas data asset
	 */
	virtual void SetMaterialAtlas(UVoxelMaterialAtlas* Atlas)
	{
		// Default: no-op (renderer doesn't support material atlas)
	}

	/**
	 * Get the current material atlas.
	 *
	 * @return Current material atlas or nullptr if not set
	 */
	virtual UVoxelMaterialAtlas* GetMaterialAtlas() const
	{
		return nullptr;
	}

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

	/**
	 * Flush all pending render operations as a single batched command.
	 *
	 * This consolidates multiple chunk updates/removals into a single render
	 * command to reduce render thread overhead and prevent job queue overflow.
	 *
	 * Should be called once per frame after all chunk operations are queued.
	 */
	virtual void FlushPendingOperations()
	{
		// Default: no-op (renderer doesn't batch operations)
	}

	// ==================== LOD Configuration ====================

	/**
	 * Set the Material Parameter Collection used for LOD parameters.
	 *
	 * The MPC should contain scalar parameters:
	 *   - LODStartDistance: Distance where MorphFactor = 0
	 *   - LODEndDistance: Distance where MorphFactor = 1
	 *   - LODInvRange: 1.0 / (EndDistance - StartDistance)
	 *
	 * @param Collection The Material Parameter Collection to use
	 */
	virtual void SetLODParameterCollection(UMaterialParameterCollection* Collection)
	{
		// Default: no-op (renderer doesn't support material-based LOD)
	}

	/**
	 * Set LOD transition distances for material-based morphing.
	 *
	 * These values are written to the Material Parameter Collection.
	 * The material can calculate per-pixel MorphFactor using:
	 *   Distance = length(WorldPosition - CameraPosition)
	 *   MorphFactor = saturate((Distance - StartDistance) * InvRange)
	 *
	 * @param StartDistance Distance where MorphFactor = 0
	 * @param EndDistance Distance where MorphFactor = 1
	 */
	virtual void SetLODTransitionDistances(float StartDistance, float EndDistance)
	{
		// Default: no-op (renderer doesn't support material-based LOD)
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
	 * Get total CPU-side memory usage (approximate).
	 *
	 * @return CPU memory in bytes used by renderer data structures
	 */
	virtual int64 GetCPUMemoryUsage() const { return 0; }

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
