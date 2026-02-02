// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelVertexFactory.h"
#include "ChunkRenderData.h"
#include "VoxelWorldComponent.generated.h"

class FVoxelSceneProxy;
class UMaterialInterface;

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

	/** Voxel size in world units */
	UPROPERTY(EditAnywhere, Category = "Voxel", meta = (ClampMin = "1"))
	float VoxelSize = 100.0f;

	/** Chunk world size */
	UPROPERTY(EditAnywhere, Category = "Voxel", meta = (ClampMin = "100"))
	float ChunkWorldSize = 3200.0f;

	/** Cached chunk data for game thread tracking */
	struct FChunkInfo
	{
		FBox Bounds;
		int32 LODLevel = 0;
		bool bIsVisible = true;
	};

	/** Game thread chunk tracking */
	TMap<FIntVector, FChunkInfo> ChunkInfoMap;

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
