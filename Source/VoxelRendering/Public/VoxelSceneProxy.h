// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "LocalVertexFactory.h"
#include "VoxelLocalVertexFactory.h"
#include "VoxelChunkGPUData.h"  // FVoxelChunkGPUData (GPU chunk buffer container)

class UVoxelWorldComponent;

/**
 * Retained pre-swap mesh for a chunk that is mid-crossfade.
 *
 * When a chunk's mesh is replaced (LOD flip or correction remesh) with crossfading enabled,
 * the outgoing GPU resources are moved here instead of being released, so
 * GetDynamicMeshElements can keep drawing the old mesh (dithering out) alongside the new one
 * (dithering in). Capped to one generation: a second swap during a fade releases the older set.
 *
 * Thread Safety: render thread only (guarded by FVoxelSceneProxy::ChunkDataLock)
 */
struct FVoxelChunkPreviousMesh
{
	/** Buffer refs, counts and bounds of the outgoing mesh */
	FVoxelChunkRenderData RenderData;

	/** Outgoing vertex buffer wrapper (still initialized — do not release until the fade ends) */
	TSharedPtr<FVoxelLocalVertexBuffer> VertexBuffer;

	/** Outgoing index buffer wrapper */
	TSharedPtr<FVoxelLocalIndexBuffer> IndexBuffer;

	/** Outgoing vertex factory (streams reference VertexBuffer) */
	TSharedPtr<FLocalVertexFactory> VertexFactory;

	/** Release all retained GPU resources */
	void ReleaseResources()
	{
		if (VertexFactory.IsValid())
		{
			VertexFactory->ReleaseResource();
			VertexFactory.Reset();
		}
		if (IndexBuffer.IsValid())
		{
			IndexBuffer->ReleaseResource();
			IndexBuffer.Reset();
		}
		if (VertexBuffer.IsValid())
		{
			VertexBuffer->ReleaseResource();
			VertexBuffer.Reset();
		}
		RenderData.ReleaseResources();
	}
};

/**
 * Render-side crossfade state for one chunk, attached by the game-thread fade driver
 * (UVoxelWorldComponent). The material proxies belong to pooled MIDs of the masked fade
 * material; the MIDs carry the animated FadeAlpha/FadeInvert parameters, so the proxies
 * stay valid pointers for the whole fade.
 *
 * Thread Safety: render thread only (guarded by FVoxelSceneProxy::ChunkDataLock)
 */
struct FVoxelChunkFadeState
{
	/** Fade progress 0..1 (0 = previous fully visible, 1 = current fully visible). Bookkeeping copy of the MID parameter. */
	float FadeAlpha = 0.0f;

	/** Masked MID proxy for the incoming (current) mesh — FadeInvert=0, dithers in as alpha rises */
	const FMaterialRenderProxy* FadeInProxy = nullptr;

	/** Masked MID proxy for the outgoing (previous) mesh — FadeInvert=1, dithers out as alpha rises */
	const FMaterialRenderProxy* FadeOutProxy = nullptr;
};

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
	 * @param bCrossfade If true and the chunk already has a mesh, retain it as the crossfade Previous set instead of releasing it
	 */
	void UpdateChunkBuffers_RenderThread(FRHICommandListBase& RHICmdList, const FIntVector& ChunkCoord, const FVoxelChunkGPUData& GPUData, bool bCrossfade = false);

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
	 * @param bCrossfade If true and the chunk already has a mesh, retain it as the crossfade Previous set instead of releasing it
	 */
	void UpdateChunkFromCPUData_RenderThread(
		FRHICommandListBase& RHICmdList,
		const FIntVector& ChunkCoord,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		int32 LODLevel,
		const FBox& ChunkLocalBounds,
		const FVector& ChunkWorldPosition,
		bool bCrossfade = false);

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
		/** Retain the replaced mesh as the crossfade Previous set (decided on the game thread at submit time) */
		bool bCrossfade = false;
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

	// ==================== Mesh-Swap Crossfade (Render Thread) ====================
	// See Documentation/Research/SEAM_OWNERSHIP_ARCHITECTURE.md §5. The game-thread driver
	// is UVoxelWorldComponent (cvars voxel.MeshFade / voxel.MeshFade.Duration).

	/**
	 * Set the masked fade material used by crossfade batches (for relevance only —
	 * the actual draw materials are the per-chunk MID proxies in FVoxelChunkFadeState).
	 * Must be called on render thread. Relevance must be computed on game thread.
	 */
	void SetFadeMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InRelevance);

	/**
	 * Attach crossfade material proxies for a chunk. Fading starts rendering once both this
	 * state and the chunk's Previous mesh set exist (order-independent: the CPU batch path
	 * attaches before its swap lands, the GPU path after).
	 * Must be called on render thread.
	 */
	void SetChunkFadeState_RenderThread(const FIntVector& ChunkCoord, const FMaterialRenderProxy* InFadeInProxy, const FMaterialRenderProxy* InFadeOutProxy);

	/**
	 * Per-tick fade alpha updates for all transitioning chunks (bookkeeping copy — the MIDs
	 * carry the visually-effective parameter). Also mirrors into the legacy MorphFactor field.
	 * Must be called on render thread.
	 */
	void UpdateChunkFadeAlphasBatch_RenderThread(const TArray<TPair<FIntVector, float>>& Alphas);

	/**
	 * End a chunk's crossfade: release its Previous mesh set and detach fade state.
	 * Must be called on render thread.
	 */
	void ClearChunkFadeState_RenderThread(const FIntVector& ChunkCoord);

	/**
	 * End all crossfades (kill-switch flips, material changes, shutdown).
	 * Must be called on render thread.
	 */
	void ClearAllChunkFadeStates_RenderThread();

	// ==================== Water Tile Management (Render Thread) ====================

	/**
	 * Set water material. Must be called on render thread.
	 * MaterialRelevance must be computed on game thread before calling.
	 *
	 * @param InMaterial Water material (typically translucent or Single Layer Water)
	 * @param InRelevance Pre-computed material relevance
	 */
	void SetWaterMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InRelevance);

	/**
	 * Update water tile GPU data from CPU vertex arrays. Must be called on render thread.
	 * Uses the same vertex conversion pipeline as terrain chunks.
	 *
	 * @param RHICmdList RHI command list for resource initialization
	 * @param TileCoord 2D tile coordinate (XY chunk column)
	 * @param Vertices CPU vertex array (will be moved)
	 * @param Indices CPU index array (will be moved)
	 * @param TileWorldPosition World position of tile origin
	 */
	void UpdateWaterTileFromCPUData_RenderThread(
		FRHICommandListBase& RHICmdList,
		const FIntVector2& TileCoord,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		const FVector& TileWorldPosition);

	/**
	 * Remove a water tile. Must be called on render thread.
	 *
	 * @param TileCoord 2D tile coordinate to remove
	 */
	void RemoveWaterTile_RenderThread(const FIntVector2& TileCoord);

	/**
	 * Clear all water tiles. Must be called on render thread.
	 */
	void ClearAllWaterTiles_RenderThread();

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
	// ==================== Terrain Chunk Data ====================

	/** Per-chunk render data (converted to FLocalVertexFactory format) */
	TMap<FIntVector, FVoxelChunkRenderData> ChunkRenderData;

	/** Per-chunk vertex buffer wrappers (needed for FLocalVertexFactory::FDataType) */
	TMap<FIntVector, TSharedPtr<FVoxelLocalVertexBuffer>> ChunkVertexBuffers;

	/** Per-chunk index buffer wrappers */
	TMap<FIntVector, TSharedPtr<FVoxelLocalIndexBuffer>> ChunkIndexBuffers;

	/** Per-chunk vertex factories (each chunk needs its own since stream components reference specific buffers) */
	TMap<FIntVector, TSharedPtr<FLocalVertexFactory>> ChunkVertexFactories;

	// ==================== Crossfade Data ====================

	/** Retained pre-swap mesh sets for chunks mid-crossfade (sparse — transitioning chunks only) */
	TMap<FIntVector, FVoxelChunkPreviousMesh> ChunkPreviousMeshes;

	/** Fade state (alpha + MID proxies) for chunks mid-crossfade, attached by the game-thread driver */
	TMap<FIntVector, FVoxelChunkFadeState> ChunkFadeStates;

	/**
	 * Remove a chunk's current mesh entry from all maps. With bMoveToPrevious the resources are
	 * retained in ChunkPreviousMeshes for crossfading (releasing any older Previous first);
	 * otherwise everything is released. Any stale Previous for the coord is always dropped.
	 * Caller must hold ChunkDataLock.
	 */
	void RetireOrReleaseChunk_AssumesLocked(const FIntVector& ChunkCoord, bool bMoveToPrevious);

	/** Release one chunk's retained Previous mesh (if any) and detach its fade state. Caller must hold ChunkDataLock. */
	void ReleaseChunkFadeState_AssumesLocked(const FIntVector& ChunkCoord);

	/** Release every retained Previous mesh and all fade states. Caller must hold ChunkDataLock. */
	void ReleaseAllChunkFadeStates_AssumesLocked();

	// ==================== Water Tile Data ====================

	/** Per-tile water render data (FIntVector2 key = XY chunk column) */
	TMap<FIntVector2, FVoxelChunkRenderData> WaterTileRenderData;

	/** Per-tile water vertex buffers */
	TMap<FIntVector2, TSharedPtr<FVoxelLocalVertexBuffer>> WaterTileVertexBuffers;

	/** Per-tile water index buffers */
	TMap<FIntVector2, TSharedPtr<FVoxelLocalIndexBuffer>> WaterTileIndexBuffers;

	/** Per-tile water vertex factories */
	TMap<FIntVector2, TSharedPtr<FLocalVertexFactory>> WaterTileVertexFactories;

	// ==================== Materials ====================

	/** Material for terrain rendering */
	UMaterialInterface* Material;

	/** Material relevance */
	FMaterialRelevance MaterialRelevance;

	/** Material for water tiles (separate from terrain material) */
	UMaterialInterface* WaterMaterial = nullptr;

	/** Cached water material relevance */
	FMaterialRelevance WaterMaterialRelevance;

	/** Masked fade material used by crossfade batches (relevance holder; may be null = crossfade unavailable) */
	UMaterialInterface* FadeMaterial = nullptr;

	/** Cached fade material relevance (masked) */
	FMaterialRelevance FadeMaterialRelevance;

	// ==================== Configuration ====================

	/** Feature level */
	ERHIFeatureLevel::Type FeatureLevel;

	/** Voxel size in world units */
	float VoxelSize;

	/** Critical section for thread-safe chunk access */
	mutable FCriticalSection ChunkDataLock;
};
