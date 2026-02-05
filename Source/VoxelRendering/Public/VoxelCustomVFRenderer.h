// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMeshRenderer.h"
#include "VoxelVertexFactory.h"

class UVoxelWorldComponent;
class UMaterialInterface;
class UMaterialParameterCollection;
class UVoxelMaterialAtlas;
class UWorld;
class UVoxelWorldConfiguration;

/**
 * GPU-driven Custom Vertex Factory renderer.
 *
 * High-performance renderer that uses a custom vertex factory to render
 * voxel meshes directly from GPU buffers without CPU readback.
 *
 * Features:
 * - Direct GPU buffer usage (no CPU-GPU sync)
 * - Custom 28-byte vertex format
 * - LOD morphing support
 * - Per-chunk frustum culling
 *
 * Thread Safety: All public methods must be called from game thread
 *
 * @see IVoxelMeshRenderer
 * @see FVoxelVertexFactory
 * @see Documentation/RENDERING_SYSTEM.md
 */
class VOXELRENDERING_API FVoxelCustomVFRenderer : public IVoxelMeshRenderer
{
public:
	FVoxelCustomVFRenderer();
	virtual ~FVoxelCustomVFRenderer();

	// ==================== IVoxelMeshRenderer Interface ====================

	// Lifecycle
	virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override;
	virtual void Shutdown() override;
	virtual bool IsInitialized() const override;

	// Mesh Updates
	virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) override;
	virtual void UpdateChunkMeshFromCPU(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		const FChunkMeshData& MeshData) override;
	virtual void RemoveChunk(const FIntVector& ChunkCoord) override;
	virtual void ClearAllChunks() override;

	// Visibility
	virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override;
	virtual void SetAllChunksVisible(bool bVisible) override;

	// Material Management
	virtual void SetMaterial(UMaterialInterface* Material) override;
	virtual UMaterialInterface* GetMaterial() const override;
	virtual void UpdateMaterialParameters() override;
	virtual void SetMaterialAtlas(UVoxelMaterialAtlas* Atlas) override;
	virtual UVoxelMaterialAtlas* GetMaterialAtlas() const override;

	// LOD Transitions
	virtual void UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor) override;
	virtual void UpdateLODTransitionsBatch(const TArray<TPair<FIntVector, float>>& Transitions) override;

	// Batched Operations
	virtual void FlushPendingOperations() override;

	// LOD Configuration
	virtual void SetLODParameterCollection(UMaterialParameterCollection* Collection) override;
	virtual void SetLODTransitionDistances(float StartDistance, float EndDistance) override;

	// Queries
	virtual bool IsChunkLoaded(const FIntVector& ChunkCoord) const override;
	virtual int32 GetLoadedChunkCount() const override;
	virtual void GetLoadedChunks(TArray<FIntVector>& OutChunks) const override;
	virtual int64 GetGPUMemoryUsage() const override;
	virtual int64 GetTotalVertexCount() const override;
	virtual int64 GetTotalTriangleCount() const override;

	// Bounds
	virtual bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const override;
	virtual FBox GetTotalBounds() const override;

	// Debugging
	virtual FString GetDebugStats() const override;
	virtual void DrawDebugVisualization(const FLODQueryContext& Context) const override;
	virtual FString GetRendererTypeName() const override;

	// ==================== Custom VF Specific ====================

	/**
	 * Get the world component used for rendering.
	 * May return nullptr if not initialized.
	 */
	UVoxelWorldComponent* GetWorldComponent() const { return WorldComponent; }

private:
	// ==================== Internal Helpers ====================

	/** Create GPU buffers from CPU mesh data */
	void CreateGPUBuffersFromCPU(
		const TArray<FVoxelVertex>& Vertices,
		const TArray<uint32>& Indices,
		FBufferRHIRef& OutVertexBuffer,
		FBufferRHIRef& OutIndexBuffer);

	/** Convert FChunkMeshData to FVoxelVertex array */
	void ConvertToVoxelVertices(
		const FChunkMeshData& MeshData,
		TArray<FVoxelVertex>& OutVertices);

	/** Calculate chunk bounds from mesh data */
	FBox CalculateChunkBounds(const FIntVector& ChunkCoord) const;

	// ==================== State ====================

	/** Whether renderer has been initialized */
	bool bIsInitialized = false;

	/** Owning world */
	TWeakObjectPtr<UWorld> CachedWorld;

	/** World configuration */
	TWeakObjectPtr<const UVoxelWorldConfiguration> CachedConfig;

	/** World component for rendering (prevent GC via AddToRoot) */
	TObjectPtr<UVoxelWorldComponent> WorldComponent;

	/** Container actor for the component */
	TWeakObjectPtr<AActor> ContainerActor;

	/** Current material */
	TWeakObjectPtr<UMaterialInterface> CurrentMaterial;

	// ==================== Statistics ====================

	/** Per-chunk tracking data */
	struct FChunkStats
	{
		int32 VertexCount = 0;
		int32 TriangleCount = 0;
		int32 LODLevel = 0;
		SIZE_T MemoryUsage = 0;
		FBox Bounds = FBox(ForceInit);
		bool bIsVisible = true;
	};

	/** Game thread chunk tracking */
	TMap<FIntVector, FChunkStats> ChunkStatsMap;

	/** Total statistics */
	int64 TotalVertexCount = 0;
	int64 TotalTriangleCount = 0;
	int64 TotalGPUMemory = 0;

	/** Cached configuration values */
	float VoxelSize = 100.0f;
	float ChunkWorldSize = 3200.0f;
};
