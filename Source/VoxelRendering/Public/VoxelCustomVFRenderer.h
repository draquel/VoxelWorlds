// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMeshRenderer.h"
#include "VoxelChunkGPUData.h"
#include "VoxelRenderRegions.h"

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
 * - Render regions on planets (spherical Phase 6, WM-BP7, D23): one UVoxelWorldComponent per region of
 *   voxel.Render.RegionChunks^3 chunks, located at the region's origin, so vertices are stored in float relative to it
 *   and keep their precision at any distance from the world origin (FVoxelRenderRegions). Chunks, seams (by their
 *   owner) and water tiles go to their region's component; settings go to every component and are replayed on a new
 *   one; a region's component is retired once it holds nothing. Heightmap worlds keep one component at the origin.
 *
 * Thread Safety: All public methods must be called from game thread
 *
 * @see IVoxelMeshRenderer
 * @see UVoxelWorldComponent
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
	virtual void UpdateChunkMeshFromCPU(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		FChunkMeshData&& MeshData) override;
	virtual void RemoveChunk(const FIntVector& ChunkCoord) override;
	virtual void ClearAllChunks() override;

	// Visibility
	virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override;
	virtual void SetAllChunksVisible(bool bVisible) override;

	// Material Management
	virtual void SetMaterial(UMaterialInterface* Material) override;
	virtual UMaterialInterface* GetMaterial() const override;
	virtual UMaterialInterface* GetSurfaceMaterial() const override;
	virtual void UpdateMaterialParameters() override;
	virtual void SetWaterMaterial(UMaterialInterface* Material) override;
	virtual void SetMaterialAtlas(UVoxelMaterialAtlas* Atlas) override;
	virtual UVoxelMaterialAtlas* GetMaterialAtlas() const override;

	// Water Tile Support
	virtual void UpdateWaterTileMesh(
		const FIntVector2& TileCoord,
		const FChunkMeshData& WaterMeshData) override;
	virtual void RemoveWaterTile(const FIntVector2& TileCoord) override;
	virtual void ClearAllWaterTiles() override;

	// Seam Meshes (seam-ownership P1)
	virtual void UpdateSeamMeshFromCPU(
		const FIntVector& OwnerChunkCoord,
		uint8 Axis,
		int32 LODLevel,
		FChunkMeshData&& MeshData,
		bool bCrossfade = false) override;
	virtual void SetChunkFadeHold(const FIntVector& ChunkCoord, bool bHold) override;
	virtual void SetSeamFadeHold(const FIntVector& OwnerChunkCoord, bool bHold) override;
	virtual float GetSeamFadeRemainingSeconds(const FIntVector& OwnerChunkCoord) const override;
	virtual void SetViewerPosition(const FVector& ViewerPosition) override;
	virtual bool WillFadeInChunk(const FIntVector& ChunkCoord) const override;
	virtual void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 Axis) override;
	virtual void ClearAllSeamMeshes() override;
	virtual void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const override;
	virtual bool ConsumeLostRenderData(TArray<FIntVector>& OutChunks) override;
	virtual bool GetWaterTileDrawInfo(const FIntVector2& TileCoord, FString& Out) const override;

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
	 * Get the world component used for rendering: the only one on a heightmap world; on a planet the primary one (region
	 * (0, 0, 0), kept for the renderer's lifetime; the others come and go with their regions, GetRegionComponents).
	 * May return nullptr if not initialized.
	 */
	UVoxelWorldComponent* GetWorldComponent() const { return WorldComponent; }

	/** The render regions this renderer splits the world into (one at the origin on heightmap worlds). */
	const FVoxelRenderRegions& GetRenderRegions() const { return Regions; }

	/**
	 * Every live world component by region (spherical Phase 6, WM-BP7).
	 *
	 * @param OutComponents Region coordinate and component pairs
	 */
	void GetRegionComponents(TArray<TPair<FIntVector, UVoxelWorldComponent*>>& OutComponents) const;

private:
	// ==================== Internal Helpers ====================

	/** Create GPU buffers from CPU mesh data */
	void CreateGPUBuffersFromCPU(
		const TArray<FVoxelVertex>& Vertices,
		const TArray<uint32>& Indices,
		FBufferRHIRef& OutVertexBuffer,
		FBufferRHIRef& OutIndexBuffer);

	/** Convert FChunkMeshData to FVoxelVertex array, computing local bounds in the same pass */
	void ConvertToVoxelVertices(
		const FChunkMeshData& MeshData,
		TArray<FVoxelVertex>& OutVertices,
		FBox& OutLocalBounds);

	/** Shared submit body for the copy/move UpdateChunkMeshFromCPU variants */
	void SubmitChunkMeshInternal(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		const FChunkMeshData& MeshData,
		TArray<uint32>&& Indices);

	/** Calculate chunk bounds from mesh data */
	FBox CalculateChunkBounds(const FIntVector& ChunkCoord) const;

	// ==================== Render Regions (WM-BP7) ====================

	/** The component of a chunk's (or seam owner's) region, or null when that region has none. */
	UVoxelWorldComponent* FindRegionComponent(const FIntVector& ChunkCoord) const;

	/** The component of a chunk's region, created (configured with every setting so far) when it has none. */
	UVoxelWorldComponent* GetOrCreateRegionComponent(const FIntVector& ChunkCoord);

	/** A configured, registered component at a region's origin. */
	UVoxelWorldComponent* CreateRegionComponent(const FIntVector& Region);

	/** Retire the components of regions that hold nothing any more (never the primary one). */
	void RetireEmptyRegions();

	/** The region a water tile's chunk column belongs to (its z-0 chunk). */
	FIntVector WaterTileChunk(const FIntVector2& TileCoord) const { return FIntVector(TileCoord.X, TileCoord.Y, 0); }

	// ==================== State ====================

	/** Whether renderer has been initialized */
	bool bIsInitialized = false;

	/** Owning world */
	TWeakObjectPtr<UWorld> CachedWorld;

	/** World configuration */
	TWeakObjectPtr<const UVoxelWorldConfiguration> CachedConfig;

	/** The primary world component (owned by the container actor; GetWorldComponent) */
	TObjectPtr<UVoxelWorldComponent> WorldComponent;

	/** The render regions (FVoxelRenderRegions::FromConfig) */
	FVoxelRenderRegions Regions;

	/** Every live world component by region coordinate, the primary one included (owned by the container actor) */
	TMap<FIntVector, TObjectPtr<UVoxelWorldComponent>> RegionComponents;

	/** Settings replayed on a new region component */
	TWeakObjectPtr<UVoxelMaterialAtlas> CurrentAtlas;
	TWeakObjectPtr<UMaterialParameterCollection> CurrentLODCollection;
	bool bHasLODDistances = false;
	float LODStartDistance = 0.0f;
	float LODEndDistance = 0.0f;
	bool bHasViewerPosition = false;
	FVector CurrentViewerPosition = FVector::ZeroVector;

	/** Container actor for the component */
	TWeakObjectPtr<AActor> ContainerActor;

	/** Current terrain material */
	TWeakObjectPtr<UMaterialInterface> CurrentMaterial;

	/** Current water material */
	TWeakObjectPtr<UMaterialInterface> CurrentWaterMaterial;

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
