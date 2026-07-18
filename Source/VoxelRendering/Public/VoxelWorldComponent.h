// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelChunkGPUData.h"
#include "ChunkRenderData.h"
#include "VoxelWorldComponent.generated.h"

class FVoxelSceneProxy;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UVoxelMaterialAtlas;

/**
 * Primitive component for rendering voxel worlds.
 *
 * Creates and manages an FVoxelSceneProxy for GPU-driven rendering.
 * Handles game thread to render thread communication for chunk updates.
 *
 * Thread Safety: All public methods must be called from game thread
 *
 * @see FVoxelSceneProxy
 * @see FVoxelCustomVFRenderer
 * @see Documentation/RENDERING_SYSTEM.md
 */
UCLASS(ClassGroup = (Voxel), meta = (BlueprintSpawnableComponent))
class VOXELRENDERING_API UVoxelWorldComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelWorldComponent(const FObjectInitializer& ObjectInitializer);
	virtual ~UVoxelWorldComponent();

	// ==================== UPrimitiveComponent Interface ====================

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* Material) override;
	virtual int32 GetNumMaterials() const override { return 1; }

	// ==================== Chunk Management ====================

	/**
	 * Update or add chunk buffers.
	 * Enqueues update to render thread.
	 *
	 * @param RenderData Chunk render data with GPU buffers
	 */
	void UpdateChunkBuffers(const FChunkRenderData& RenderData);

	/**
	 * Update chunk from GPU data directly.
	 * Enqueues update to render thread.
	 *
	 * @param GPUData Pre-constructed GPU data
	 */
	void UpdateChunkBuffersFromGPU(const FVoxelChunkGPUData& GPUData);

	/**
	 * Update chunk from CPU vertex/index data directly - FAST PATH.
	 * Skips GPU buffer roundtrip by passing CPU data straight to render thread.
	 * This is the preferred path for CPU-generated mesh data.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param Vertices CPU vertex array (will be moved)
	 * @param Indices CPU index array (will be moved)
	 * @param LODLevel LOD level of the mesh
	 * @param LocalBounds Local bounds of the mesh
	 */
	void UpdateChunkBuffersFromCPUData(
		const FIntVector& ChunkCoord,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		int32 LODLevel,
		const FBox& LocalBounds);

	/**
	 * Remove a chunk.
	 * Enqueues removal to render thread.
	 *
	 * @param ChunkCoord Chunk coordinate to remove
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void RemoveChunk(const FIntVector& ChunkCoord);

	/**
	 * Clear all chunks.
	 * Enqueues clear to render thread.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void ClearAllChunks();

	/**
	 * Flush all pending chunk operations as a single batched render command.
	 * Called once per frame to consolidate multiple add/remove operations.
	 */
	void FlushPendingOperations();

	/**
	 * Set visibility for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param bNewVisibility Whether chunk should be visible
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void SetChunkVisible(const FIntVector& ChunkCoord, bool bNewVisibility);

	/**
	 * Update LOD morph factor for a chunk.
	 * Repurposed as the crossfade alpha carrier: when the chunk has an active mesh-swap
	 * crossfade this drives its render-side FadeAlpha (see SEAM_OWNERSHIP_ARCHITECTURE.md §5).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param MorphFactor Blend factor 0-1
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void UpdateChunkMorphFactor(const FIntVector& ChunkCoord, float MorphFactor);

	// ==================== Mesh-Swap Crossfade ====================
	// Softens every chunk mesh swap (LOD flips AND correction remeshes) by drawing the old
	// and new mesh together for a short dithered crossfade, mesher-agnostic. Controlled by
	// cvars voxel.MeshFade (kill-switch) and voxel.MeshFade.Duration.

	/**
	 * Set the masked fade material used for crossfade draws.
	 * The material must be a Masked-blend variant of the terrain material with scalar
	 * parameters "FadeAlpha" (dither threshold) and "FadeInvert" (0 = dither in, 1 = dither
	 * out) driving OpacityMask via DitherTemporalAA.
	 *
	 * If never called, the component auto-derives it when SetMaterial runs: base material
	 * "<Path>/M_Foo" looks for "<Path>/M_Foo_Fade". No fade material = instant swaps (legacy).
	 * Note: name-derived materials are soft references — packaged games should either assign
	 * this explicitly or ensure the _Fade asset is cooked.
	 *
	 * @param InFadeMaterial Masked fade material, or nullptr to disable crossfading
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|LOD")
	void SetFadeMaterial(UMaterialInterface* InFadeMaterial);

	/** Get the active fade material (explicit or auto-derived), null if crossfade unavailable */
	UFUNCTION(BlueprintPure, Category = "Voxel|LOD")
	UMaterialInterface* GetFadeMaterial() const { return FadeMaterial; }

	// ==================== Water Tile Management ====================

	/**
	 * Set water material and forward to render thread.
	 *
	 * @param InMaterial Water material (typically translucent or Single Layer Water), or nullptr to disable
	 */
	void SetWaterMaterial(UMaterialInterface* InMaterial);

	/**
	 * Update water tile from CPU vertex data. Enqueues render command.
	 *
	 * @param TileCoord 2D tile coordinate (XY chunk column)
	 * @param Vertices CPU vertex array (will be moved)
	 * @param Indices CPU index array (will be moved)
	 * @param TileWorldPosition World position of tile origin
	 */
	void UpdateWaterTileFromCPUData(
		const FIntVector2& TileCoord,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		const FVector& TileWorldPosition);

	/**
	 * Remove a water tile. Enqueues render command.
	 *
	 * @param TileCoord 2D tile coordinate to remove
	 */
	void RemoveWaterTile(const FIntVector2& TileCoord);

	/**
	 * Clear all water tiles. Enqueues render command.
	 */
	void ClearAllWaterTiles();

	// ==================== Seam Meshes (seam-ownership P1) ====================

	/**
	 * Update a single-owner face-seam mesh from CPU vertex data. Enqueues render command.
	 * Positions are owner-local; the proxy applies OwnerWorldPosition.
	 *
	 * @param OwnerChunkCoord Owning (min-coordinate) chunk
	 * @param Axis Face axis: 0 = +X, 1 = +Y, 2 = +Z
	 * @param LODLevel Shared LOD the seam was meshed at
	 * @param Vertices CPU vertex array (will be moved)
	 * @param Indices CPU index array (will be moved)
	 * @param OwnerWorldPosition World position of the owner chunk's origin
	 */
	void UpdateSeamMeshFromCPUData(
		const FIntVector& OwnerChunkCoord,
		uint8 Axis,
		int32 LODLevel,
		TArray<FVoxelVertex>&& Vertices,
		TArray<uint32>&& Indices,
		const FVector& OwnerWorldPosition);

	/** Remove one face-seam mesh. Enqueues render command. */
	void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 Axis);

	/** Clear all seam meshes. Enqueues render command. */
	void ClearAllSeamMeshes();

	// ==================== Configuration ====================

	/**
	 * Set voxel size in world units.
	 * Should be called before chunks are added.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void SetVoxelSize(float InVoxelSize);

	/** Get voxel size */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	float GetVoxelSize() const { return VoxelSize; }

	/**
	 * Set chunk world size.
	 * Should be called before chunks are added.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void SetChunkWorldSize(float InChunkWorldSize);

	/** Get chunk world size */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	float GetChunkWorldSize() const { return ChunkWorldSize; }

	/**
	 * Set world origin offset.
	 * All chunk positions are relative to this origin.
	 * Should be called before chunks are added.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void SetWorldOrigin(const FVector& InWorldOrigin);

	/** Get world origin offset */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	FVector GetWorldOrigin() const { return WorldOrigin; }

	// ==================== Material Atlas ====================

	/**
	 * Set the material atlas for this voxel world.
	 * This binds atlas textures and parameters to the material.
	 *
	 * @param InAtlas The material atlas data asset to use
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Materials")
	void SetMaterialAtlas(UVoxelMaterialAtlas* InAtlas);

	/** Get the current material atlas */
	UFUNCTION(BlueprintPure, Category = "Voxel|Materials")
	UVoxelMaterialAtlas* GetMaterialAtlas() const { return MaterialAtlas; }

	/**
	 * Create a dynamic material instance from a master material.
	 * The instance will be configured with atlas parameters.
	 *
	 * @param MasterMaterial The master material to create an instance from
	 * @return The created dynamic material instance
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Materials")
	UMaterialInstanceDynamic* CreateVoxelMaterialInstance(UMaterialInterface* MasterMaterial);

	/**
	 * Update material parameters from the current atlas.
	 * Call this after changing atlas settings to refresh the material.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Materials")
	void UpdateMaterialAtlasParameters();

	/** Get whether smooth meshing mode is enabled */
	UFUNCTION(BlueprintPure, Category = "Voxel|Materials")
	bool GetUseSmoothMeshing() const { return bUseSmoothMeshing; }

	/** Set smooth meshing mode (triplanar vs packed atlas) */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Materials")
	void SetUseSmoothMeshing(bool bUseSmooth);

	// ==================== LOD Configuration ====================

	/**
	 * Set LOD transition distances for material-based morphing.
	 * Updates the Material Parameter Collection with these values.
	 *
	 * In your material, use CollectionParameter nodes with:
	 *   - "LODStartDistance": Distance where MorphFactor = 0
	 *   - "LODEndDistance": Distance where MorphFactor = 1
	 *   - "LODInvRange": 1 / (End - Start) for efficient calculation
	 *
	 * Material MorphFactor calculation:
	 *   Distance = length(WorldPosition - CameraPosition)
	 *   MorphFactor = saturate((Distance - LODStartDistance) * LODInvRange)
	 *
	 * @param InLODStartDistance Distance where LOD transition starts (MorphFactor = 0)
	 * @param InLODEndDistance Distance where LOD transition ends (MorphFactor = 1)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|LOD")
	void SetLODTransitionDistances(float InLODStartDistance, float InLODEndDistance);

	/** Get LOD start distance */
	UFUNCTION(BlueprintPure, Category = "Voxel|LOD")
	float GetLODStartDistance() const { return LODStartDistance; }

	/** Get LOD end distance */
	UFUNCTION(BlueprintPure, Category = "Voxel|LOD")
	float GetLODEndDistance() const { return LODEndDistance; }

	/**
	 * Set the Material Parameter Collection used for LOD parameters.
	 * The MPC should have scalar parameters: LODStartDistance, LODEndDistance, LODInvRange
	 *
	 * @param InCollection The Material Parameter Collection to use
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|LOD")
	void SetLODParameterCollection(UMaterialParameterCollection* InCollection);

	// ==================== Queries ====================

	/**
	 * Check if a chunk is loaded.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if chunk has data loaded
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	bool IsChunkLoaded(const FIntVector& ChunkCoord) const;

	/**
	 * Get number of loaded chunks.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	int32 GetLoadedChunkCount() const;

	/**
	 * Get all loaded chunk coordinates.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void GetLoadedChunks(TArray<FIntVector>& OutChunks) const;

	/**
	 * Get world bounds for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param OutBounds Output bounding box
	 * @return True if chunk exists
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel")
	bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const;

	// ==================== Statistics ====================

	/** Get total GPU memory usage in bytes */
	UFUNCTION(BlueprintPure, Category = "Voxel|Stats")
	int64 GetGPUMemoryUsage() const;

	/** Get total vertex count */
	UFUNCTION(BlueprintPure, Category = "Voxel|Stats")
	int64 GetTotalVertexCount() const;

	/** Get total triangle count */
	UFUNCTION(BlueprintPure, Category = "Voxel|Stats")
	int64 GetTotalTriangleCount() const;

protected:
	/** Called when scene proxy is created */
	virtual void SendRenderDynamicData_Concurrent() override;

	/** Mark render state dirty */
	void MarkRenderStateDirtyAndNotify();

private:
	/** Get scene proxy pointer (may be null) */
	FVoxelSceneProxy* GetVoxelSceneProxy() const;

	/** Material used for terrain rendering */
	UPROPERTY(EditAnywhere, Category = "Voxel")
	TObjectPtr<UMaterialInterface> VoxelMaterial;

	/** Water material for water tile rendering */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WaterMaterial;

	/** Material atlas data asset for texture configuration */
	UPROPERTY(EditAnywhere, Category = "Voxel|Materials")
	TObjectPtr<UVoxelMaterialAtlas> MaterialAtlas;

	/** Enable smooth meshing mode (triplanar projection with Texture2DArrays) vs cubic (packed atlas) */
	UPROPERTY(EditAnywhere, Category = "Voxel|Materials")
	bool bUseSmoothMeshing = false;

	/** Dynamic material instance created from the master material */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynamicMaterialInstance;

	/** Voxel size in world units */
	UPROPERTY(EditAnywhere, Category = "Voxel", meta = (ClampMin = "1"))
	float VoxelSize = 100.0f;

	/** Chunk world size */
	UPROPERTY(EditAnywhere, Category = "Voxel", meta = (ClampMin = "100"))
	float ChunkWorldSize = 3200.0f;

	/** World origin offset - all chunk positions are relative to this */
	UPROPERTY(VisibleAnywhere, Category = "Voxel")
	FVector WorldOrigin = FVector::ZeroVector;

	/** Distance where LOD transition starts (MorphFactor = 0) */
	UPROPERTY(EditAnywhere, Category = "Voxel|LOD", meta = (ClampMin = "0"))
	float LODStartDistance = 5000.0f;

	/** Distance where LOD transition ends (MorphFactor = 1) */
	UPROPERTY(EditAnywhere, Category = "Voxel|LOD", meta = (ClampMin = "0"))
	float LODEndDistance = 10000.0f;

	/**
	 * Material Parameter Collection for LOD parameters.
	 * Create an MPC asset with scalar parameters: LODStartDistance, LODEndDistance, LODInvRange
	 * Then assign it here to enable material-based LOD morphing.
	 */
	UPROPERTY(EditAnywhere, Category = "Voxel|LOD")
	TObjectPtr<UMaterialParameterCollection> LODParameterCollection;

	/** Update the Material Parameter Collection with current LOD values */
	void UpdateLODParameterCollection();

	/** Cached chunk data for game thread tracking */
	struct FChunkInfo
	{
		FBox Bounds;
		int32 LODLevel = 0;
		bool bIsVisible = true;
	};

	/** Game thread chunk tracking */
	TMap<FIntVector, FChunkInfo> ChunkInfoMap;

	// ==================== Batched Render Operations ====================
	// Instead of sending individual render commands, we batch operations
	// and flush them once per frame to reduce render command overhead

	/** Pending chunk data to add (batched) */
	struct FPendingChunkAdd
	{
		FIntVector ChunkCoord;
		TArray<FVoxelVertex> Vertices;
		TArray<uint32> Indices;
		int32 LODLevel;
		FBox LocalBounds;
		FVector ChunkWorldPosition;
		/** Crossfade this swap (chunk already had a mesh and fading is enabled) */
		bool bCrossfade = false;
	};
	TArray<FPendingChunkAdd> PendingAdds;

	/** Pending chunk removals (batched) */
	TArray<FIntVector> PendingRemovals;

	/** Whether we have pending operations to flush */
	bool HasPendingOperations() const { return PendingAdds.Num() > 0 || PendingRemovals.Num() > 0; }

	// ==================== Mesh-Swap Crossfade State ====================

	/** Masked fade material (explicit via SetFadeMaterial, or auto-derived from the base material name) */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FadeMaterial;

	/** True once SetFadeMaterial was called explicitly — auto-derivation stops overriding it */
	bool bExplicitFadeMaterial = false;

	/** MID pools (GC owners). "In" MIDs have FadeInvert=0, "Out" MIDs FadeInvert=1. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> FadeInMIDPool;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> FadeOutMIDPool;

	/** Currently unused pool entries, ready for reuse */
	TArray<UMaterialInstanceDynamic*> FreeFadeInMIDs;
	TArray<UMaterialInstanceDynamic*> FreeFadeOutMIDs;

	/** Entries retired this tick — reusable next tick, after the render thread stops referencing them */
	TArray<UMaterialInstanceDynamic*> CoolingFadeInMIDs;
	TArray<UMaterialInstanceDynamic*> CoolingFadeOutMIDs;

	/** One active mesh-swap crossfade */
	struct FActiveChunkFade
	{
		double StartTime = 0.0;
		UMaterialInstanceDynamic* InMID = nullptr;   // owned by FadeInMIDPool
		UMaterialInstanceDynamic* OutMID = nullptr;  // owned by FadeOutMIDPool
	};

	/** Active crossfades by chunk coordinate (transitioning chunks only — expect a few dozen) */
	TMap<FIntVector, FActiveChunkFade> ActiveFades;

	/** True if crossfading is possible right now (cvar on, fade material set, proxy alive) */
	bool CanCrossfade() const;

	/** Start (or restart) a crossfade for a chunk whose mesh was just resubmitted. Enqueues the fade-state attach. */
	void StartChunkCrossfade(const FIntVector& ChunkCoord);

	/** Drop a chunk's crossfade without waiting for completion (chunk removed / fade disabled). */
	void CancelChunkCrossfade(const FIntVector& ChunkCoord, bool bClearRenderState);

	/** Per-tick fade driver: advances FadeAlpha on the MIDs, batches alphas to the proxy, completes finished fades. */
	void AdvanceChunkCrossfades();

	/** Get a pooled fade MID (creating if needed), atlas parameters freshly copied, FadeInvert/FadeAlpha initialized. */
	UMaterialInstanceDynamic* AcquireFadeMID(bool bInvert);

	/** Return a fade's MIDs to the cooling lists */
	void RetireFadeMIDs(FActiveChunkFade& Fade);

	/** Auto-derive FadeMaterial from the current material's base ("M_Foo" -> "M_Foo_Fade") unless explicitly set */
	void RefreshDerivedFadeMaterial();

	/** Send the fade material + its relevance to a scene proxy (explicit target — CreateSceneProxy syncs before SceneProxy is assigned) */
	void PushFadeMaterialToProxy(FVoxelSceneProxy* TargetProxy);

	/** Critical section for chunk info access */
	mutable FCriticalSection ChunkInfoLock;

	/** Cached total bounds */
	mutable FBox CachedTotalBounds;
	mutable bool bTotalBoundsDirty = true;

	/** Statistics */
	int64 CachedVertexCount = 0;
	int64 CachedTriangleCount = 0;
	int64 CachedGPUMemory = 0;
};
