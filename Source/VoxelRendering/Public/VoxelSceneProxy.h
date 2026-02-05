// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "LocalVertexFactory.h"
#include "VoxelLocalVertexFactory.h"
#include "VoxelVertexFactory.h"  // For FVoxelChunkGPUData (legacy format input)

class UVoxelWorldComponent;

/**
 * Scene proxy for voxel world rendering.
 *
 * Uses FLocalVertexFactory (Epic's proven implementation) for reliable rendering.
 * Converts voxel vertex data to FLocalVertexFactory-compatible format.
 *
 * Manages per-chunk GPU data and issues draw calls via GetDynamicMeshElements.
 * Performs frustum culling at the chunk level for efficient rendering.
 *
 * Thread Safety: All public methods are for render thread only
 *
 * @see UVoxelWorldComponent
 * @see Documentation/RENDERING_SYSTEM.md
 */
class VOXELRENDERING_API FVoxelSceneProxy : public FPrimitiveSceneProxy
{
public:
	/**
	 * Construct scene proxy.
	 *
	 * @param InComponent Owner component
	 * @param InMaterial Material to use for rendering
	 */
	FVoxelSceneProxy(UVoxelWorldComponent* InComponent, UMaterialInterface* InMaterial);

	virtual ~FVoxelSceneProxy();

	// ==================== FPrimitiveSceneProxy Interface ====================

	virtual SIZE_T GetTypeHash() const override;

	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	virtual uint32 GetMemoryFootprint() const override;

	virtual bool CanBeOccluded() const override { return true; }

	virtual bool HasSubprimitiveOcclusionQueries() const override { return false; }

	// ==================== Chunk Management (Render Thread) ====================

	/**
	 * Update or add chunk GPU data.
	 * Converts from FVoxelChunkGPUData (FVoxelVertex format) to FLocalVertexFactory-compatible format.
	 * Must be called on render thread.
	 *
	 * @param RHICmdList RHI command list for resource initialization
	 * @param ChunkCoord Chunk coordinate
	 * @param GPUData GPU data for the chunk (in FVoxelVertex format)
	 */
	void UpdateChunkBuffers_RenderThread(FRHICommandListBase& RHICmdList, const FIntVector& ChunkCoord, const FVoxelChunkGPUData& GPUData);

	/**
	 * Update chunk directly from CPU vertex data - FAST PATH.
	 * Avoids GPU buffer roundtrip by taking CPU arrays directly.
	 * Must be called on render thread.
	 *
	 * @param RHICmdList RHI command list for resource initialization
	 * @param ChunkCoord Chunk coordinate
	 * @param Vertices CPU vertex array (will be moved)
	 * @param Indices CPU index array (will be moved)
	 * @param LODLevel LOD level of the mesh
	 * @param ChunkLocalBounds Local bounds of the mesh
	 * @param ChunkWorldPosition World position of chunk origin
	 */
	void UpdateChunkFromCPUData_RenderThread(
		FRHICommandListBase& RHICmdList,
		const FIntVector& ChunkCoord,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		int32 LODLevel,
		const FBox& ChunkLocalBounds,
		const FVector& ChunkWorldPosition);

	/**
	 * Remove a chunk.
	 * Must be called on render thread.
	 *
	 * @param ChunkCoord Chunk coordinate to remove
	 */
	void RemoveChunk_RenderThread(const FIntVector& ChunkCoord);

	/**
	 * Batch update - process multiple adds and removes in a single call.
	 * This reduces render command overhead significantly.
	 * Must be called on render thread.
	 */
	struct FBatchChunkAdd
	{
		FIntVector ChunkCoord;
		TArray<FVoxelVertex> Vertices;
		TArray<uint32> Indices;
		int32 LODLevel;
		FBox LocalBounds;
		FVector ChunkWorldPosition;
	};
	void ProcessBatchUpdate_RenderThread(
		FRHICommandListBase& RHICmdList,
		TArray<FBatchChunkAdd>&& Adds,
		TArray<FIntVector>&& Removals);

	/**
	 * Clear all chunks.
	 * Must be called on render thread.
	 */
	void ClearAllChunks_RenderThread();

	/**
	 * Set chunk visibility.
	 * Must be called on render thread.
	 */
	void SetChunkVisible_RenderThread(const FIntVector& ChunkCoord, bool bVisible);

	/**
	 * Update LOD morph factor for a chunk.
	 * Must be called on render thread.
	 */
	void UpdateChunkMorphFactor_RenderThread(const FIntVector& ChunkCoord, float MorphFactor);

	/**
	 * Update the material used for rendering.
	 * Must be called on render thread.
	 * MaterialRelevance must be computed on game thread before calling.
	 */
	void SetMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InMaterialRelevance);

	// ==================== Statistics ====================

	/** Get number of loaded chunks */
	int32 GetChunkCount() const { return ChunkRenderData.Num(); }

	/** Get total vertex count */
	int64 GetTotalVertexCount() const;

	/** Get total triangle count */
	int64 GetTotalTriangleCount() const;

	/** Get total GPU memory usage */
	SIZE_T GetGPUMemoryUsage() const;

private:
	/** Per-chunk render data (converted to FLocalVertexFactory format) */
	TMap<FIntVector, FVoxelChunkRenderData> ChunkRenderData;

	/** Per-chunk vertex buffer wrappers (needed for FLocalVertexFactory::FDataType) */
	TMap<FIntVector, TSharedPtr<FVoxelLocalVertexBuffer>> ChunkVertexBuffers;

	/** Per-chunk index buffer wrappers */
	TMap<FIntVector, TSharedPtr<FVoxelLocalIndexBuffer>> ChunkIndexBuffers;

	/** Per-chunk vertex factories (each chunk needs its own since stream components reference specific buffers) */
	TMap<FIntVector, TSharedPtr<FLocalVertexFactory>> ChunkVertexFactories;

	/** Material for rendering */
	UMaterialInterface* Material;

	/** Material relevance */
	FMaterialRelevance MaterialRelevance;

	/** Feature level */
	ERHIFeatureLevel::Type FeatureLevel;

	/** Voxel size in world units */
	float VoxelSize;

	/** Critical section for thread-safe chunk access */
	mutable FCriticalSection ChunkDataLock;
};
