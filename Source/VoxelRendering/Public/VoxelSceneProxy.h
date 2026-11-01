// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "Materials/MaterialInterface.h"
#include "LocalVertexFactory.h"
#include "VoxelLocalVertexFactory.h"
#include "VoxelBatchCounters.h"

class UVoxelRegionComponent;

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
 * (UVoxelRegionComponent). The material proxies belong to pooled MIDs of the masked fade
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

	/** Masked MID proxy for the outgoing (previous) mesh — FadeInvert=1, dithers out as alpha rises (null for an appearance) */
	const FMaterialRenderProxy* FadeOutProxy = nullptr;

	/**
	 * Held at alpha 0 by a coordinated LOD transition (R24): only the Previous mesh is visible. A
	 * resubmit while held replaces the never-shown current mesh and keeps Previous, so the surface
	 * on screen stays the one the transition started from.
	 */
	bool bHeld = false;

	/**
	 * First appearance (R24 P3): there is no Previous mesh and the current one fades in from nothing
	 * (invisible while held). A resubmit replaces the current mesh in place on the same alpha —
	 * there is nothing behind it to crossfade from, and retaining the half-shown mesh would pop it
	 * back to solid.
	 */
	bool bAppear = false;
};

/**
 * What FVoxelSceneProxy holds and draws for one chunk interior (diagnostics and tests, world modes review G74; see
 * FVoxelSceneProxy::GetChunkDrawInfo_RenderThread).
 */
struct FVoxelChunkDrawInfo
{
	/** Triangles of the current mesh (0 = none) and its LOD; a chunk with a mesh is visible (the per-chunk
	 *  visibility toggle was deleted in code review CR1, REND-11) */
	uint32 CurrentTriangles = 0;
	int32 LODLevel = -1;
	bool bVisible = false;

	/** The current mesh's vertex factory: tells one upload from the next */
	const void* VertexFactory = nullptr;

	/** Triangles of the retained Previous mesh (0 = none) */
	uint32 PreviousTriangles = 0;

	/** A crossfade or appearance state is attached: its alpha, hold and appearance flags */
	bool bHasFadeState = false;
	float FadeAlpha = 0.0f;
	bool bHeld = false;
	bool bAppear = false;

	/** A frame draws the Previous mesh dithering out under the current one dithering in */
	bool bCrossfading = false;

	/** A frame draws the current mesh alone, dithering in from nothing */
	bool bAppearing = false;

	/** A frame draws the current mesh alone with the terrain material (no fade) */
	bool DrawsCurrentOnly() const { return CurrentTriangles > 0 && bVisible && !bCrossfading && !bAppearing; }
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
 * @see UVoxelRegionComponent
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
	FVoxelSceneProxy(UVoxelRegionComponent* InComponent, UMaterialInterface* InMaterial);

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
	 * Update the material used for rendering.
	 * Must be called on render thread.
	 * MaterialRelevance must be computed on game thread before calling.
	 */
	void SetMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InMaterialRelevance);

	// ==================== Mesh-Swap Crossfade (Render Thread) ====================
	// See Documentation/Research/SEAM_OWNERSHIP_ARCHITECTURE.md §5. The game-thread driver
	// is UVoxelRegionComponent (cvars voxel.MeshFade / voxel.MeshFade.Duration).

	/**
	 * Set the masked fade material used by crossfade batches (for relevance only —
	 * the actual draw materials are the per-chunk MID proxies in FVoxelChunkFadeState).
	 * Must be called on render thread. Relevance must be computed on game thread.
	 */
	void SetFadeMaterial_RenderThread(UMaterialInterface* InMaterial, const FMaterialRelevance& InRelevance);

	/**
	 * Attach crossfade material proxies for a chunk. Fading starts rendering once both this
	 * state and the chunk's Previous mesh set exist (order-independent: the CPU batch path
	 * attaches before its swap lands, the GPU path after). An appearance (bAppear) needs no
	 * Previous set: the current mesh draws with the fade-in proxy from the moment it lands.
	 * Must be called on render thread.
	 *
	 * @param bHeld   Hold at alpha 0 until ReleaseFadeHolds_RenderThread.
	 * @param bAppear First appearance: fade the current mesh in from nothing (InFadeOutProxy may be null).
	 */
	void SetChunkFadeState_RenderThread(const FIntVector& ChunkCoord, const FMaterialRenderProxy* InFadeInProxy, const FMaterialRenderProxy* InFadeOutProxy, bool bHeld = false, bool bAppear = false);

	/**
	 * Clear the held flag of chunk and seam fades that were just released (their alphas start
	 * rising with the next alpha batch). Keys use FVoxelFadeScheduler's layout (W = -1 for a
	 * chunk interior, 0..6 for a seam bucket slot).
	 * Must be called on render thread.
	 */
	void ReleaseFadeHolds_RenderThread(const TArray<FIntVector4>& Keys);

	/**
	 * Per-tick fade alpha updates for all transitioning chunks (bookkeeping copy — the MIDs
	 * carry the visually-effective parameter).
	 * Must be called on render thread.
	 */
	void UpdateChunkFadeAlphasBatch_RenderThread(const TArray<TPair<FIntVector, float>>& Alphas);

	/**
	 * End a chunk's crossfade: release its Previous mesh set and detach fade state.
	 * Must be called on render thread.
	 */
	void ClearChunkFadeState_RenderThread(const FIntVector& ChunkCoord);

	/**
	 * End all crossfades, chunk and seam (kill-switch flips, material changes, shutdown).
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

	// ==================== Seam Meshes (Render Thread) ====================
	// Single-owner seam buckets keyed by (owner chunk, slot) — a parallel keyspace beside the
	// per-chunk maps (the water-tile pattern). Drawn with the terrain material, including the RVT
	// clone pass (seams must write RVT or the cached ground would crack). Since R24 they crossfade
	// like chunks: a swap can retain the previous bucket and both draw with complementary dither,
	// a bucket can fade in from nothing or out to nothing, and a fade can be held at alpha 0.

	/**
	 * Update (or create) a face-seam mesh's GPU data. Must be called on render thread.
	 * Uses the same vertex conversion pipeline as terrain chunks.
	 *
	 * @param RHICmdList RHI command list for resource creation
	 * @param OwnerChunkCoord Owning (min-coordinate) chunk
	 * @param Axis Face axis: 0 = +X, 1 = +Y, 2 = +Z
	 * @param LODLevel Shared LOD the seam was meshed at
	 * @param Vertices CPU vertex array in owner-local space (moved)
	 * @param Indices CPU index array (moved)
	 * @param OwnerWorldPosition World position of the owner chunk's origin
	 * @param bCrossfade Retain the replaced bucket as the crossfade Previous set instead of releasing it
	 */
	void UpdateSeamMeshFromCPUData_RenderThread(
		FRHICommandListBase& RHICmdList,
		const FIntVector& OwnerChunkCoord,
		uint8 Axis,
		int32 LODLevel,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		const FVector& OwnerWorldPosition,
		bool bCrossfade = false);

	/**
	 * Remove one seam bucket. Must be called on render thread.
	 * @param bFadeOut Keep the bucket as a Previous set that dithers out under its fade state
	 *                 (fade to nothing) instead of releasing it now.
	 */
	void RemoveSeamMesh_RenderThread(const FIntVector& OwnerChunkCoord, uint8 Axis, bool bFadeOut = false);

	/** Clear all seam buckets (and their fades). Must be called on render thread. */
	void ClearAllSeamMeshes_RenderThread();

	/**
	 * Attach crossfade material proxies for a seam bucket (same contract as SetChunkFadeState_RenderThread;
	 * with no Previous set the bucket fades in from nothing).
	 * Must be called on render thread.
	 */
	void SetSeamFadeState_RenderThread(const FIntVector& OwnerChunkCoord, uint8 Axis,
		const FMaterialRenderProxy* InFadeInProxy, const FMaterialRenderProxy* InFadeOutProxy, bool bHeld = false, bool bAppear = false);

	/** Per-tick seam fade alphas (bookkeeping copies; keys are FVoxelFadeScheduler seam keys). Render thread. */
	void UpdateSeamFadeAlphasBatch_RenderThread(const TArray<TPair<FIntVector4, float>>& Alphas);

	/** End a seam bucket's fade: release its Previous set and detach fade state. Render thread. */
	void ClearSeamFadeState_RenderThread(const FIntVector& OwnerChunkCoord, uint8 Axis);

	// ==================== Statistics ====================

	/** Get number of loaded chunks */
	int32 GetChunkCount() const { return ChunkRenderData.Num(); }

	/** Get total vertex count */
	int64 GetTotalVertexCount() const;

	/** Get total triangle count */
	int64 GetTotalTriangleCount() const;

	/** Get total GPU memory usage */
	SIZE_T GetGPUMemoryUsage() const;

	/**
	 * How the proxy draws one chunk interior now (diagnostics and tests, world modes review G74): its current mesh,
	 * the retained Previous mesh and fade state, and whether a frame crossfades them, fades the current one in or draws
	 * the current one alone with the terrain material (the test GetDynamicMeshElements makes). Render thread.
	 *
	 * @param ChunkCoord The chunk
	 * @param OutInfo    Receives what the proxy holds for it
	 * @return True if the proxy holds a current mesh for the chunk
	 */
	bool GetChunkDrawInfo_RenderThread(const FIntVector& ChunkCoord, FVoxelChunkDrawInfo& OutInfo) const;

	/**
	 * What the proxy holds for a water tile and when a main view last drew it (world modes review G77). Render thread.
	 *
	 * @param TileCoord The tile (chunk column)
	 * @param Out       Receives a line: vertices, bounds, the frames since it was drawn (or never)
	 * @return True if the proxy holds a mesh for the tile
	 */
	bool GetWaterTileDrawInfo_RenderThread(const FIntVector2& TileCoord, FString& Out) const;

	/**
	 * The crossfade state the proxy holds, for diagnostics (voxel.MeshFade.Stats): fade states and retained Previous
	 * meshes of chunk interiors and seam buckets, up to MaxEntries of the chunk ones, and for a focus chunk what the
	 * proxy holds (GetChunkDrawInfo_RenderThread) and what the last frame drew of it. Render thread.
	 *
	 * @param MaxEntries Chunk fade states to list
	 * @param FocusChunk A chunk to report and to watch in GetDynamicMeshElements from now on, or null to stop watching
	 * @return One line per item
	 */
	FString GetFadeDebugString_RenderThread(int32 MaxEntries, const FIntVector* FocusChunk) const;

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

	// ==================== Seam Mesh Data ====================

	/** Key for a single-owner face-seam bucket: owning chunk + face axis (0=+X, 1=+Y, 2=+Z). */
	struct FVoxelSeamMeshKey
	{
		FIntVector Owner = FIntVector::ZeroValue;
		uint8 Axis = 0;

		FORCEINLINE bool operator==(const FVoxelSeamMeshKey& Other) const
		{
			return Owner == Other.Owner && Axis == Other.Axis;
		}
		friend FORCEINLINE uint32 GetTypeHash(const FVoxelSeamMeshKey& Key)
		{
			// Component-wise scalar hashing: FVoxelSceneProxy's own GetTypeHash() member hides the
			// unqualified overload set here, and ::-qualification would break ADL for FIntVector.
			uint32 Hash = ::GetTypeHash(Key.Owner.X);
			Hash = HashCombine(Hash, ::GetTypeHash(Key.Owner.Y));
			Hash = HashCombine(Hash, ::GetTypeHash(Key.Owner.Z));
			Hash = HashCombine(Hash, ::GetTypeHash(static_cast<uint32>(Key.Axis)));
			return Hash;
		}
	};

	/** Per-seam render data (parallel keyspace beside the per-chunk maps — the water-tile pattern) */
	TMap<FVoxelSeamMeshKey, FVoxelChunkRenderData> SeamRenderData;

	/** Per-seam vertex buffer wrappers */
	TMap<FVoxelSeamMeshKey, TSharedPtr<FVoxelLocalVertexBuffer>> SeamVertexBuffers;

	/** Per-seam index buffer wrappers */
	TMap<FVoxelSeamMeshKey, TSharedPtr<FVoxelLocalIndexBuffer>> SeamIndexBuffers;

	/** Per-seam vertex factories */
	TMap<FVoxelSeamMeshKey, TSharedPtr<FLocalVertexFactory>> SeamVertexFactories;

	/** Retained pre-swap seam buckets mid-crossfade (or fading out to nothing) */
	TMap<FVoxelSeamMeshKey, FVoxelChunkPreviousMesh> SeamPreviousMeshes;

	/** Fade state for seam buckets mid-crossfade */
	TMap<FVoxelSeamMeshKey, FVoxelChunkFadeState> SeamFadeStates;

	/** Seam counterpart of RetireOrReleaseChunk_AssumesLocked. Caller must hold ChunkDataLock. */
	void RetireOrReleaseSeam_AssumesLocked(const FVoxelSeamMeshKey& Key, bool bMoveToPrevious);

	/** Release a seam bucket's retained Previous set and detach its fade state. Caller must hold ChunkDataLock. */
	void ReleaseSeamFadeState_AssumesLocked(const FVoxelSeamMeshKey& Key);

	/** Release every seam Previous set and fade state. Caller must hold ChunkDataLock. */
	void ReleaseAllSeamFadeStates_AssumesLocked();

	// ==================== Water Tile Data ====================

	/** Per-tile water render data (FIntVector2 key = XY chunk column) */
	TMap<FIntVector2, FVoxelChunkRenderData> WaterTileRenderData;

	/** The render-thread frame each water tile was last drawn in by a main view (G77 diagnostics; ChunkDataLock). */
	mutable TMap<FIntVector2, uint32> WaterTileDrawnFrame;

	/** The render-thread frame GetDynamicMeshElements last ran for a main view, and how often it has (G77 diagnostics). */
	mutable uint32 LastMainViewFrame = 0;
	mutable uint32 MainViewCalls = 0;
	mutable uint32 RelevanceCalls = 0;
	mutable uint32 LastRelevanceFrame = 0;
	mutable bool bLastShown = false;
	mutable uint32 MainRelevanceCalls = 0;
	mutable uint32 LastMainRelevanceFrame = 0;
	mutable uint32 ShadowGatherCalls = 0;
	mutable FVector LastRelevanceViewOrigin = FVector::ZeroVector;
	mutable bool bLastRelevanceFrustumHit = false;

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

	/**
	 * Where this proxy's primitive sits: its component's location, copied at creation (spherical Phase 6, WM-BP7, D23).
	 * Vertex positions are stored in float relative to it (ToRenderOffset): a render region's origin on a planet, the
	 * world origin (0, 0, 0) on heightmap worlds, whose positions are therefore absolute as before. Per-chunk culling
	 * bounds stay in world space.
	 */
	FVector RenderOrigin = FVector::ZeroVector;

	/** A chunk, seam or tile origin as the float offset its vertices are stored at (relative to RenderOrigin). */
	FORCEINLINE FVector3f ToRenderOffset(const FVector& WorldPosition) const
	{
		return FVector3f(WorldPosition - RenderOrigin);
	}

	/** Face types against a planet's radial up (UVoxelRegionComponent::SetRadialFaceUp), copied at creation; the centre
	 *  relative to RenderOrigin, as the vertices are */
	bool bRadialFaceUp = false;
	FVector3f FaceUpCenter = FVector3f::ZeroVector;

	/**
	 * A mesher vertex converted for upload (spherical SP-D3): on a planet the face type is measured against the radial
	 * up at the vertex's position (Vertex.Position + Offset, relative to RenderOrigin); otherwise
	 * FVoxelLocalVertex::FromVoxelVertex as is.
	 */
	FORCEINLINE FVoxelLocalVertex ConvertVertex(const FVoxelVertex& Vertex, const FVector3f& Offset) const
	{
		return bRadialFaceUp
			? FVoxelLocalVertex::FromVoxelVertex(Vertex, (Vertex.Position + Offset - FaceUpCenter).GetSafeNormal())
			: FVoxelLocalVertex::FromVoxelVertex(Vertex);
	}

	/** Critical section for thread-safe chunk access */
	mutable FCriticalSection ChunkDataLock;
};
