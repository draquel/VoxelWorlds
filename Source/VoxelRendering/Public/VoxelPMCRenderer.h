// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMeshRenderer.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"
#include "VoxelPMCRenderer.generated.h"

class UMaterial;

// Forward declarations
class UProceduralMeshComponent;
class USceneComponent;

/**
 * Container actor for PMC renderer.
 *
 * Spawned at runtime to hold ProceduralMeshComponent instances.
 * Each chunk gets one PMC attached to this actor.
 * Transient - not saved to level.
 */
UCLASS(NotBlueprintable, Transient, NotPlaceable)
class VOXELRENDERING_API AVoxelPMCContainerActor : public AActor
{
	GENERATED_BODY()

public:
	AVoxelPMCContainerActor();

	/** Root component for attaching PMCs */
	UPROPERTY(VisibleAnywhere, Category = "Voxel")
	TObjectPtr<USceneComponent> RootSceneComponent;
};

/**
 * Tracking data for a single chunk's PMC.
 */
struct FPMCChunkData
{
	/** Weak reference to the mesh component */
	TWeakObjectPtr<UProceduralMeshComponent> MeshComponent;

	/** Current LOD level */
	int32 LODLevel = 0;

	/** Whether the chunk is currently visible */
	bool bIsVisible = true;

	/** World-space bounding box */
	FBox Bounds = FBox(ForceInit);

	/** Number of vertices in the mesh */
	int32 VertexCount = 0;

	/** Number of triangles in the mesh */
	int32 TriangleCount = 0;

	/** Approximate memory usage in bytes */
	SIZE_T MemoryUsage = 0;
};

/**
 * ProceduralMeshComponent-based voxel renderer.
 *
 * Fallback renderer using UE's ProceduralMeshComponent for chunk rendering.
 * Suitable for editor tools and lower-performance scenarios.
 *
 * Features:
 * - Component pooling for reduced allocations
 * - CPU-side mesh data (no GPU buffer management)
 * - Automatic collision mesh generation
 *
 * Limitations:
 * - No GPU morph-based LOD transitions
 * - Higher per-chunk draw call overhead
 * - CPU-bound mesh updates
 *
 * Thread Safety: Game thread only (PMC API requirement)
 *
 * @see IVoxelMeshRenderer
 * @see FVoxelCustomVFRenderer
 */
class VOXELRENDERING_API FVoxelPMCRenderer : public IVoxelMeshRenderer
{
public:
	FVoxelPMCRenderer();
	virtual ~FVoxelPMCRenderer();

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
		const FChunkMeshData& MeshData
	) override;
	virtual void RemoveChunk(const FIntVector& ChunkCoord) override;
	virtual void ClearAllChunks() override;

	// Visibility
	virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override;
	virtual void SetAllChunksVisible(bool bVisible) override;

	// Material Management
	virtual void SetMaterial(UMaterialInterface* Material) override;
	virtual UMaterialInterface* GetMaterial() const override;
	virtual void UpdateMaterialParameters() override;

	// LOD Transitions
	virtual void UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor) override;

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
	virtual FString GetRendererTypeName() const override;

private:
	// ==================== Component Pool Management ====================

	/** Acquire a component from the pool or create a new one */
	UProceduralMeshComponent* AcquireComponent(const FIntVector& ChunkCoord);

	/** Release a component back to the pool */
	void ReleaseComponent(UProceduralMeshComponent* PMC);

	/** Create a new PMC instance */
	UProceduralMeshComponent* CreateNewComponent();

	// ==================== Data Conversion ====================

	/** Convert FChunkMeshData to PMC format arrays */
	void ConvertMeshDataToPMCFormat(
		const FChunkMeshData& MeshData,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs,
		TArray<FLinearColor>& OutColors,
		TArray<struct FProcMeshTangent>& OutTangents
	);

	/** Calculate world-space bounds for a chunk */
	FBox CalculateChunkBounds(const FIntVector& ChunkCoord) const;

	/** Create a default material that displays vertex colors */
	void CreateDefaultVertexColorMaterial();

	// ==================== Member Data ====================

	/** Whether the renderer has been initialized */
	bool bIsInitialized = false;

	/** Cached world reference */
	TWeakObjectPtr<UWorld> CachedWorld;

	/** Cached configuration */
	TWeakObjectPtr<const UVoxelWorldConfiguration> CachedConfig;

	/** Container actor holding all PMCs */
	TWeakObjectPtr<AVoxelPMCContainerActor> ContainerActor;

	/** Current material for all chunks */
	TWeakObjectPtr<UMaterialInterface> CurrentMaterial;

	/** Strong reference to the default vertex color material we created */
	TStrongObjectPtr<UMaterial> DefaultVertexColorMaterial;

	/** Map of chunk coordinates to their data */
	TMap<FIntVector, FPMCChunkData> ChunkDataMap;

	/** Pool of reusable PMC components */
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> ComponentPool;

	/** Whether to generate collision meshes */
	bool bGenerateCollision = true;

	// ==================== Statistics ====================

	/** Total vertex count across all chunks */
	int64 TotalVertexCount = 0;

	/** Total triangle count across all chunks */
	int64 TotalTriangleCount = 0;

	/** Total memory usage across all chunks */
	SIZE_T TotalMemoryUsage = 0;
};
