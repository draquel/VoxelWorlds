// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelScatterTypes.h"
#include "VoxelRenderRegions.h"
#include "VoxelScatterRenderer.generated.h"

class UVoxelScatterManager;
class UHierarchicalInstancedStaticMeshComponent;

/**
 * Per-HISM instance pool state for recycling instances without structural HISM changes.
 * Instances are never removed from the HISM — they're hidden by setting scale to
 * FVector::ZeroVector and placed on a free list. When new instances are needed,
 * they're recycled from the free list first (via UpdateInstanceTransform).
 */
struct FHISMInstancePool
{
	/** Free list of instance indices (zero-scaled, ready for recycling) */
	TArray<int32> FreeIndices;

	/** Which instances belong to each chunk: ChunkCoord -> array of instance indices */
	TMap<FIntVector, TArray<int32>> ChunkInstanceIndices;

	/** Total allocated instances in this HISM (active + free) */
	int32 TotalAllocated = 0;
};

/**
 * A HISM's key (spherical Phase 6, WM-BP7, D23): its scatter type and its render region. Each region's HISM sits at the
 * region's origin, so its instance transforms are stored in float relative to it; heightmap worlds have one region at the
 * world origin, so one HISM per type as before.
 */
struct FVoxelScatterHISMKey
{
	int32 ScatterTypeID = 0;
	FIntVector Region = FIntVector::ZeroValue;

	FORCEINLINE bool operator==(const FVoxelScatterHISMKey& Other) const
	{
		return ScatterTypeID == Other.ScatterTypeID && Region == Other.Region;
	}
	friend FORCEINLINE uint32 GetTypeHash(const FVoxelScatterHISMKey& Key)
	{
		return HashCombine(::GetTypeHash(Key.ScatterTypeID), GetTypeHash(Key.Region));
	}
};

/**
 * Manages HISM (Hierarchical Instanced Static Mesh) components for scatter rendering.
 *
 * Creates one HISM per scatter type (per type and region on a planet, FVoxelScatterHISMKey; voxel.Scatter.RegionChunks) and
 * manages instances based on chunk lifecycle.
 * Uses an instance pool per HISM to recycle instances via UpdateInstanceTransform
 * instead of ClearInstances/AddInstances, eliminating visual flicker during
 * distance-based streaming. Writes to a HISM are held and applied at most once per voxel.Scatter.HISMBatchSeconds, and a
 * HISM whose pool outgrows its live instances is rewritten without it (world modes review G42: each change costs a UE HISM
 * all its instances, pooled ones included).
 *
 * Key responsibilities:
 * - Create and configure HISM components per scatter type
 * - Add instances from scatter spawn points (recycling pooled instances first)
 * - Release instances back to pool when chunks unload (zero-scale, no HISM removal)
 * - Track instance indices per chunk for proper pool management
 *
 * Thread Safety: Must be accessed from game thread only.
 *
 * @see UVoxelScatterManager
 * @see UHierarchicalInstancedStaticMeshComponent
 */
UCLASS()
class VOXELSCATTER_API UVoxelScatterRenderer : public UObject
{
	GENERATED_BODY()

public:
	UVoxelScatterRenderer();

	// ==================== Lifecycle ====================

	/**
	 * Initialize the scatter renderer.
	 *
	 * @param Manager The scatter manager that owns this renderer
	 * @param World World to spawn HISM components in
	 */
	void Initialize(UVoxelScatterManager* Manager, UWorld* World);

	/**
	 * Shutdown and cleanup all HISM components.
	 */
	void Shutdown();

	/**
	 * Check if renderer is initialized.
	 */
	bool IsInitialized() const { return bIsInitialized; }

	/**
	 * Per-frame tick to process deferred rebuilds.
	 * Call this once per frame to flush pending scatter type rebuilds.
	 * Rebuilds are deferred while the viewer is moving to prevent flicker.
	 *
	 * @param ViewerPosition Current viewer position
	 * @param DeltaTime Time since last tick
	 */
	void Tick(const FVector& ViewerPosition, float DeltaTime);

	// ==================== Instance Management ====================

	/**
	 * Update instances for a chunk.
	 *
	 * Adds instances to HISM components based on scatter spawn points.
	 * Removes any existing instances for this chunk first.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param ScatterData Scatter data with spawn points
	 */
	void UpdateChunkInstances(const FIntVector& ChunkCoord, const FChunkScatterData& ScatterData);

	/**
	 * Add supplemental scatter instances to existing chunk HISMs.
	 * Only adds instances for NEW scatter types in the data — does not
	 * clear or rebuild existing instances. Used by distance-based streaming
	 * to avoid the blink caused by full HISM rebuild.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param NewScatterData Scatter data containing only the new spawn points to add
	 */
	void AddSupplementalInstances(const FIntVector& ChunkCoord, const FChunkScatterData& NewScatterData);

	/**
	 * Replace all instances for one scatter type in one chunk with the given transforms.
	 * Releases the chunk+type's current instances back to the pool immediately (zero-scale),
	 * drops any queued adds for the pair, and queues the new transforms for budget-limited
	 * re-add via FlushPendingInstanceAdds — which runs every Tick, unlike full-type rebuilds
	 * that wait for a stationary viewer and a quiet generation pipeline.
	 * Used by edit-driven scatter clearing so removals render immediately during play.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param ScatterTypeID Scatter type to replace instances for
	 * @param Transforms Remaining instance transforms (empty = release only)
	 */
	void UpdateChunkTypeInstances(const FIntVector& ChunkCoord, int32 ScatterTypeID, TArray<FTransform> Transforms);

	/**
	 * Remove all instances for a chunk.
	 *
	 * Called when a chunk is unloaded.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	void RemoveChunkInstances(const FIntVector& ChunkCoord);

	/**
	 * Release instances for a specific scatter type from a chunk back to the pool.
	 * Instances are zero-scaled (hidden) and added to the free list, and adds still queued for the pair are dropped
	 * (flushed later, they would belong to a type the chunk no longer tracks, which RemoveChunkInstances never releases;
	 * world modes review G30).
	 * Batched (voxel.Scatter.HISMBatchSeconds > 0), each instance is zero-scaled where it stands, an in-place change that does
	 * not rebuild the HISM's cluster tree, at the HISM's next batch; the free list and chunk tracking change at once.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param ScatterTypeID Scatter type ID to release
	 */
	void ReleaseChunkScatterType(const FIntVector& ChunkCoord, int32 ScatterTypeID);

	/**
	 * Clear all instances from all HISM components.
	 */
	void ClearAllInstances();

	/**
	 * Apply every HISM write held for a batch now (world modes review G42). Tick applies a HISM's writes at most once per
	 * voxel.Scatter.HISMBatchSeconds and not while its cluster tree builds; the instance bookkeeping (pools, chunk indices,
	 * GetChunkInstanceCount) never waits. For callers that read the HISMs' instances directly.
	 */
	void FlushHISMWrites();

	/** HISM write batches applied since Initialize (each one HISM's held writes), for tests and the bench. */
	int64 GetTotalHISMWriteBatches() const { return TotalHISMWriteBatches; }

	/** Writes held for HISMs right now (instances to place, release or add), for tests and the bench. */
	int32 GetPendingHISMWriteCount() const;

	/** HISMs rewritten without their pooled instances since Initialize (voxel.Scatter.HISMCompactMinPooled), for tests and the bench. */
	int64 GetTotalHISMCompactions() const { return TotalHISMCompactions; }

	// ==================== HISM Management ====================

	/**
	 * Get or create the HISM component of a scatter type in a render region (WM-BP7: located at the region's origin).
	 *
	 * @param ScatterTypeID Scatter type ID
	 * @param Region Render region (FVoxelRenderRegions; (0, 0, 0) when the world is not split)
	 * @return HISM component or nullptr if scatter type has no mesh
	 */
	UHierarchicalInstancedStaticMeshComponent* GetOrCreateHISM(int32 ScatterTypeID, const FIntVector& Region = FIntVector::ZeroValue);

	/**
	 * The render regions instances are kept in (FVoxelRenderRegions::FromConfig; the manager sets them before Initialize).
	 *
	 * @param InRegions The regions
	 */
	void SetRenderRegions(const FVoxelRenderRegions& InRegions) { Regions = InRegions; }

	/** The render regions instances are kept in. */
	const FVoxelRenderRegions& GetRenderRegions() const { return Regions; }

	/**
	 * Every HISM of a scatter type, by region.
	 *
	 * @param ScatterTypeID Scatter type ID
	 * @param OutHISMs Region and component pairs
	 */
	void GetTypeHISMs(int32 ScatterTypeID, TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>>& OutHISMs) const;

	/**
	 * Refresh all HISM components after definition changes.
	 */
	void RefreshAllComponents();

	/**
	 * Get number of active HISM components.
	 */
	int32 GetHISMCount() const { return HISMComponents.Num(); }

	/**
	 * Get total instance count across all HISMs.
	 */
	int32 GetTotalInstanceCount() const;

	/** Instances placed into HISMs since Initialize (recycled or new), for the streaming bench. */
	int64 GetTotalInstancesAdded() const { return TotalInstancesAdded; }

	/** Instances released to the pools since Initialize (zero-scaled), for the streaming bench. */
	int64 GetTotalInstancesRemoved() const { return TotalInstancesRemoved; }

	/**
	 * The HISMs by size, for voxel.Scatter.Stats (world modes review G42): per type its HISMs, instances and pooled
	 * (zero-scaled) instances, and the largest HISMs.
	 *
	 * @param TopN How many of the largest HISMs to list
	 * @return One line per type and per listed HISM
	 */
	FString GetHISMBreakdown(int32 TopN = 10) const;

	/**
	 * Get approximate total memory usage of HISM instances in bytes.
	 */
	int64 GetTotalMemoryUsage() const;

	// ==================== Queries ====================

	/**
	 * Nearest active (non-hidden) instance of a scatter type within Radius of Location.
	 * @param ScatterTypeID  The scatter type whose HISM is searched.
	 * @param Location       World query point.
	 * @param Radius         Search radius (world units).
	 * @param OutIndex       Instance index in the type's HISM.
	 * @param OutTransform   Instance world transform.
	 * @return True when an instance was found.
	 */
	bool FindNearestInstance(int32 ScatterTypeID, const FVector& Location, float Radius, int32& OutIndex, FTransform& OutTransform) const;

	/**
	 * Instances a chunk holds: those in its HISMs plus those still queued for the per-frame add budget. A chunk's
	 * scatter renders once when this equals its cached spawn point count (world modes review G30). O(types + queued adds).
	 * @param ChunkCoord    The chunk.
	 * @param ScatterTypeID One scatter type, or INDEX_NONE for every type.
	 * @return Its instance count.
	 */
	int32 GetChunkInstanceCount(const FIntVector& ChunkCoord, int32 ScatterTypeID = INDEX_NONE) const;

	/**
	 * The scatter types the renderer tracks for a chunk (the types RemoveChunkInstances releases on unload).
	 * @param ChunkCoord The chunk.
	 * @return Its types; empty when it has none.
	 */
	TArray<int32> GetChunkScatterTypes(const FIntVector& ChunkCoord) const;

	// ==================== Visibility ====================

	/**
	 * Show or hide HISM components for surface-only scatter types.
	 * Used to hide foliage/trees when the viewer is underground.
	 *
	 * @param bVisible True to show surface scatter, false to hide
	 */
	void SetSurfaceScatterVisible(bool bVisible);

	// ==================== Debug ====================

	/**
	 * Get debug statistics string.
	 */
	FString GetDebugStats() const;

protected:
	// ==================== Internal Methods ====================

	/**
	 * Create HISM component for a scatter definition, located at a render region's origin.
	 */
	UHierarchicalInstancedStaticMeshComponent* CreateHISMComponent(const FScatterDefinition& Definition, const FIntVector& Region);

	/**
	 * Configure HISM component from scatter definition.
	 */
	void ConfigureHISMComponent(UHierarchicalInstancedStaticMeshComponent* HISM, const FScatterDefinition& Definition);

	/**
	 * Add instances to HISM from spawn points.
	 *
	 * @param HISM Target HISM component
	 * @param SpawnPoints Spawn points to add
	 * @param Definition Scatter definition for transform settings
	 * @param OutInstanceIndices Output array of added instance indices
	 */
	void AddInstancesToHISM(
		UHierarchicalInstancedStaticMeshComponent* HISM,
		const TArray<FScatterSpawnPoint>& SpawnPoints,
		const FScatterDefinition& Definition,
		TArray<int32>& OutInstanceIndices);

protected:
	// ==================== Components ====================

	/** Container actor for HISM components */
	UPROPERTY()
	TObjectPtr<AActor> ContainerActor;

	/** HISM components per scatter type and render region (WM-BP7) */
	TMap<FVoxelScatterHISMKey, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HISMComponents;

	/** Every HISM component (keeps HISMComponents' values referenced for the garbage collector) */
	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> AllHISMComponents;

	/** The render regions (SetRenderRegions); one at the world origin unless a planet splits them */
	FVoxelRenderRegions Regions;

	// ==================== Instance Tracking ====================

	/**
	 * Track which scatter types have instances from each chunk.
	 * Structure: ChunkCoord -> Set of ScatterTypeIDs
	 * Works alongside InstancePools which tracks individual instance indices.
	 */
	TMap<FIntVector, TSet<int32>> ChunkScatterTypes;

	/**
	 * Per-HISM instance pool. Tracks free (zero-scaled) and active
	 * instance indices to enable recycling without structural HISM changes.
	 * Keyed by scatter type and region, matching HISMComponents.
	 */
	TMap<FVoxelScatterHISMKey, FHISMInstancePool> InstancePools;

	// ==================== Deferred Instance Additions ====================

	/** Pending instance additions queued by UpdateChunkInstances for budget-limited flushing */
	struct FPendingInstanceAdd
	{
		int32 ScatterTypeID;
		FIntVector ChunkCoord;
		TArray<FTransform> Transforms;
	};

	TArray<FPendingInstanceAdd> PendingInstanceAdds;

	/** Maximum instances to add to HISMs per frame (across all scatter types) */
	int32 MaxInstanceAddsPerFrame = 2000;

	/** Flush pending instance additions within the per-frame budget. Called by Tick(). */
	void FlushPendingInstanceAdds();

	// ==================== Batched HISM Writes (world modes review G42) ====================

	/**
	 * One HISM's writes held for a batch. A UE 5.8 HISM keeps its instances in legacy mode: a change that is not in place
	 * rebuilds its whole cluster tree (an O(instances) copy on the game thread; a change during the build throws it away
	 * and starts again), and the rebuilt tree recreates its scene proxy, which registers every instance again with the
	 * distance-field and ray-tracing scenes. Writing each change as it came cost that every frame for every HISM the
	 * streaming touched; held here, a HISM takes its writes at most once per voxel.Scatter.HISMBatchSeconds.
	 */
	struct FPendingHISMWrites
	{
		/** Instance index -> its new transform (local to the HISM): a release (zero scale where it stands) or a recycled placement */
		TMap<int32, FTransform> Updates;
		/** New instances (local to the HISM), indices from the HISM's instance count when they were queued */
		TArray<FTransform> Grows;
	};

	/** Writes held per HISM */
	TMap<FVoxelScatterHISMKey, FPendingHISMWrites> PendingHISMWrites;

	/** When each HISM last took its writes (FPlatformTime::Seconds) */
	TMap<FVoxelScatterHISMKey, double> LastHISMWriteSeconds;

	/** Whether writes are held for batches (voxel.Scatter.HISMBatchSeconds > 0); otherwise every write goes to its HISM at once */
	static bool IsBatchingHISMWrites();

	/**
	 * An instance's transform (local to its HISM) with the write held for it applied.
	 *
	 * @param Key The HISM's key
	 * @param HISM The HISM
	 * @param Index Instance index (a held grow's included)
	 * @param OutLocal Its transform
	 * @return False if the index is not an instance
	 */
	bool GetEffectiveInstanceTransform(const FVoxelScatterHISMKey& Key, const UHierarchicalInstancedStaticMeshComponent* HISM, int32 Index,
		FTransform& OutLocal) const;

	/**
	 * A HISM's instance count with its held grows.
	 *
	 * @param Key The HISM's key
	 * @param HISM The HISM
	 * @return The count
	 */
	int32 GetEffectiveInstanceCount(const FVoxelScatterHISMKey& Key, const UHierarchicalInstancedStaticMeshComponent* HISM) const;

	/**
	 * Apply held writes: each HISM whose batch is due (its last batch voxel.Scatter.HISMBatchSeconds ago and its tree not
	 * building, or held for voxel.Scatter.HISMMaxHoldSeconds), or all of them.
	 *
	 * @param bAll Apply every HISM's writes now
	 */
	void ApplyPendingHISMWrites(bool bAll);

	/**
	 * Rewrite a HISM with its live instances only (each chunk's, the held writes applied), dropping its pooled ones and
	 * the writes held for it: one cleared and refilled HISM, after which every change to it costs its live instances
	 * rather than every instance it ever pooled.
	 *
	 * @param Key The HISM's key
	 * @param HISM The HISM
	 * @param Pool Its pool (its chunks' indices are renumbered, its free list emptied)
	 */
	void CompactHISM(const FVoxelScatterHISMKey& Key, UHierarchicalInstancedStaticMeshComponent* HISM, FHISMInstancePool& Pool);

	/** HISM write batches applied (GetTotalHISMWriteBatches) */
	int64 TotalHISMWriteBatches = 0;

	/** HISMs compacted (GetTotalHISMCompactions) */
	int64 TotalHISMCompactions = 0;

	// ==================== References ====================

	/** Reference to scatter manager */
	UPROPERTY()
	TObjectPtr<UVoxelScatterManager> ScatterManager;

	/** Cached world reference */
	UPROPERTY()
	TObjectPtr<UWorld> CachedWorld;

	/** Whether renderer is initialized */
	bool bIsInitialized = false;

	// ==================== Statistics ====================

	/** Total instances added */
	int64 TotalInstancesAdded = 0;

	/** Total instances removed */
	int64 TotalInstancesRemoved = 0;
};
