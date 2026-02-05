// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelVertexFactory.h"
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
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param MorphFactor Blend factor 0-1
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel")
	void UpdateChunkMorphFactor(const FIntVector& ChunkCoord, float MorphFactor);

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

	/** Material used for rendering */
	UPROPERTY(EditAnywhere, Category = "Voxel")
	TObjectPtr<UMaterialInterface> VoxelMaterial;

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
	};
	TArray<FPendingChunkAdd> PendingAdds;

	/** Pending chunk removals (batched) */
	TArray<FIntVector> PendingRemovals;

	/** Whether we have pending operations to flush */
	bool HasPendingOperations() const { return PendingAdds.Num() > 0 || PendingRemovals.Num() > 0; }

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
