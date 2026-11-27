// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMeshRenderer.h"
#include "VoxelVertex.h"
#include "VoxelRenderRegions.h"

class UVoxelRegionComponent;
class UMaterialInterface;
class UVoxelMaterialAtlas;
class UTexture2D;
class UWorld;
class UVoxelWorldConfiguration;
struct FVoxelPrebuiltChunkMeshCards;

/**
 * The chunk meshes FVoxelMeshRenderer took with the vertices their mesh worker prepared and the ones it converted at the
 * submit (G294: the streaming tests' and the A/B's evidence of the path a submit took). Totals since the process started.
 */
struct VOXELRENDERING_API FVoxelSubmitCounters
{
	struct FCounts
	{
		/** Chunk meshes submitted with their prepared vertices (FChunkMeshData::HasRenderVertices) */
		int64 PreparedTaken = 0;
		/** Chunk meshes converted at the submit */
		int64 Converted = 0;
	};

	/** @return The totals (any thread) */
	static FCounts Get();
};

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
 * - Render regions on planets (spherical Phase 6, WM-BP7, D23): one UVoxelRegionComponent per region of
 *   voxel.Render.RegionChunks^3 chunks, located at the region's origin, so vertices are stored in float relative to it
 *   and keep their precision at any distance from the world origin (FVoxelRenderRegions). Chunks, seams (by their
 *   owner) and water tiles go to their region's component; settings go to every component and are replayed on a new
 *   one; a region's component is retired once it holds nothing. Heightmap worlds keep one component at the origin.
 *
 * Thread Safety: All public methods must be called from game thread
 *
 * @see IVoxelMeshRenderer
 * @see UVoxelRegionComponent
 * @see Documentation/RENDERING_SYSTEM.md
 */
class VOXELRENDERING_API FVoxelMeshRenderer : public IVoxelMeshRenderer
{
public:
	FVoxelMeshRenderer();
	virtual ~FVoxelMeshRenderer();

	// ==================== IVoxelMeshRenderer Interface ====================

	// Lifecycle
	virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override;
	virtual void Shutdown() override;
	virtual bool IsInitialized() const override;

	// Mesh Updates
	virtual void UpdateChunkMeshFromCPU(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		const FChunkMeshData& MeshData) override;
	virtual void UpdateChunkMeshFromCPU(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		FChunkMeshData&& MeshData) override;
	/** The chunk manager's mesh jobs prepare this renderer's vertices (G294): it takes them at the submit */
	virtual bool TakesPreparedVertices() const override { return true; }
	virtual void RemoveChunk(const FIntVector& ChunkCoord) override;
	virtual void ClearAllChunks() override;

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

	// Seam Meshes
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

	// Batched Operations
	virtual void FlushPendingOperations() override;

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
	UVoxelRegionComponent* GetOriginRegionComponent() const { return OriginRegionComponent; }

	/** The render regions this renderer splits the world into (one at the origin on heightmap worlds). */
	const FVoxelRenderRegions& GetRenderRegions() const { return Regions; }

	/**
	 * Every live world component by region (spherical Phase 6, WM-BP7).
	 *
	 * @param OutComponents Region coordinate and component pairs
	 */
	void GetRegionComponents(TArray<TPair<FIntVector, UVoxelRegionComponent*>>& OutComponents) const;

	/**
	 * Record, per chunk, a hash of what each submit hands its region (the vertices, the indices and the bounds), for tests
	 * that compare two ways of making a mesh's render data (G294: voxel.Mesh.PrepareRenderVertices). Off by default.
	 *
	 * @param bRecord Record from now on (true) or stop and forget (false)
	 */
	void SetRecordSubmittedVertexHashes(bool bRecord);

	/** @return The recorded hashes, per chunk with a mesh (SetRecordSubmittedVertexHashes) */
	const TMap<FIntVector, uint64>& GetSubmittedVertexHashes() const { return SubmittedVertexHashes; }

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

	/** Shared submit body for the copy/move UpdateChunkMeshFromCPU variants: the vertices converted or prepared */
	void SubmitChunkMeshInternal(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		TArray<FVoxelVertex>&& Vertices,
		const FBox& LocalBounds,
		TArray<uint32>&& Indices,
		const TSharedPtr<const FVoxelPrebuiltChunkMeshCards, ESPMode::ThreadSafe>& PrebuiltCards);

	/** Calculate chunk bounds from mesh data */
	FBox CalculateChunkBounds(const FIntVector& ChunkCoord) const;

	// ==================== Render Regions (WM-BP7) ====================

	/** The component of a chunk's (or seam owner's) region, or null when that region has none. */
	UVoxelRegionComponent* FindRegionComponent(const FIntVector& ChunkCoord) const;

	/** The component of a chunk's region, created (configured with every setting so far) when it has none. */
	UVoxelRegionComponent* GetOrCreateRegionComponent(const FIntVector& ChunkCoord);

	/** A configured, registered component at a region's origin. */
	UVoxelRegionComponent* CreateRegionComponent(const FIntVector& Region);

	/** Retire the components of regions that hold nothing any more (never the primary one). */
	void RetireEmptyRegions();

	/** The region a water tile's chunk column belongs to (its z-0 chunk). */
	FIntVector WaterTileChunk(const FIntVector2& TileCoord) const { return FIntVector(TileCoord.X, TileCoord.Y, 0); }

	// ==================== State ====================

	/** Whether renderer has been initialized */
	bool bIsInitialized = false;

	/** Whether this world holds the shadow method it asked for (D164, FVoxelShadowMethodOverride): released at Shutdown */
	bool bHoldsShadowMethod = false;

	/** Owning world */
	TWeakObjectPtr<UWorld> CachedWorld;

	/** World configuration */
	TWeakObjectPtr<const UVoxelWorldConfiguration> CachedConfig;

	/** The primary world component (owned by the container actor; GetOriginRegionComponent) */
	TObjectPtr<UVoxelRegionComponent> OriginRegionComponent;

	/** The render regions (FVoxelRenderRegions::FromConfig) */
	FVoxelRenderRegions Regions;

	/** Every live world component by region coordinate, the primary one included (owned by the container actor) */
	TMap<FIntVector, TObjectPtr<UVoxelRegionComponent>> RegionComponents;

	/** Settings replayed on a new region component */
	TWeakObjectPtr<UVoxelMaterialAtlas> CurrentAtlas;

	/** The world's biome lookup for the materials (FVoxelBiomeLookup, built at Initialize from the configuration's biomes; P3) */
	TStrongObjectPtr<UTexture2D> BiomeLookup;
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
		FBox Bounds = FBox(ForceInit);
	};

	/** Game thread chunk tracking */
	TMap<FIntVector, FChunkStats> ChunkStatsMap;

	/** Tests' record of what each chunk's last submit handed its region (SetRecordSubmittedVertexHashes) */
	bool bRecordSubmittedVertexHashes = false;
	TMap<FIntVector, uint64> SubmittedVertexHashes;

	/** Total statistics */
	int64 TotalVertexCount = 0;
	int64 TotalTriangleCount = 0;

	/** Cached configuration values */
	float VoxelSize = 100.0f;
	float ChunkWorldSize = 3200.0f;
};
