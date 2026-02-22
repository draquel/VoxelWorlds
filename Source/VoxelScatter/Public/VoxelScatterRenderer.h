// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelScatterTypes.h"
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
 * Manages HISM (Hierarchical Instanced Static Mesh) components for scatter rendering.
 *
 * Creates one HISM per scatter type and manages instances based on chunk lifecycle.
 * Uses an instance pool per HISM to recycle instances via UpdateInstanceTransform
 * instead of ClearInstances/AddInstances, eliminating visual flicker during
 * distance-based streaming.
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
	 * Remove all instances for a chunk.
	 *
	 * Called when a chunk is unloaded.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	void RemoveChunkInstances(const FIntVector& ChunkCoord);

	/**
	 * Release instances for a specific scatter type from a chunk back to the pool.
	 * Instances are zero-scaled (hidden) and added to the free list.
	 * Does NOT trigger a rebuild — instances remain allocated in HISM.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param ScatterTypeID Scatter type ID to release
	 */
	void ReleaseChunkScatterType(const FIntVector& ChunkCoord, int32 ScatterTypeID);

	/**
	 * Release all active instances for a scatter type back to the pool.
	 * Used by RebuildScatterType before re-adding fresh transforms.
	 *
	 * @param ScatterTypeID Scatter type to release all instances for
	 */
	void ReleaseAllForScatterType(int32 ScatterTypeID);

	/**
	 * Clear all instances from all HISM components.
	 */
	void ClearAllInstances();

	// ==================== HISM Management ====================

	/**
	 * Get or create HISM component for a scatter type.
	 *
	 * @param ScatterTypeID Scatter type ID
	 * @return HISM component or nullptr if scatter type has no mesh
	 */
	UHierarchicalInstancedStaticMeshComponent* GetOrCreateHISM(int32 ScatterTypeID);

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

	/**
	 * Get approximate total memory usage of HISM instances in bytes.
	 */
	int64 GetTotalMemoryUsage() const;

	// ==================== Debug ====================

	/**
	 * Get debug statistics string.
	 */
	FString GetDebugStats() const;

	/**
	 * Queue a scatter type for deferred rebuild.
	 * Actual rebuild happens in Tick() to batch multiple chunk updates.
	 *
	 * @param ScatterTypeID The scatter type to queue for rebuild
	 */
	void QueueRebuild(int32 ScatterTypeID);

protected:
	// ==================== Internal Methods ====================

	/**
	 * Create HISM component for a scatter definition.
	 */
	UHierarchicalInstancedStaticMeshComponent* CreateHISMComponent(const FScatterDefinition& Definition);

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

	/** HISM components per scatter type ID */
	UPROPERTY()
	TMap<int32, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HISMComponents;

	// ==================== Instance Tracking ====================

	/**
	 * Track which scatter types have instances from each chunk.
	 * Structure: ChunkCoord -> Set of ScatterTypeIDs
	 * Works alongside InstancePools which tracks individual instance indices.
	 */
	TMap<FIntVector, TSet<int32>> ChunkScatterTypes;

	/**
	 * Per-scatter-type instance pool. Tracks free (zero-scaled) and active
	 * instance indices to enable recycling without structural HISM changes.
	 * Keyed by ScatterTypeID matching HISMComponents.
	 */
	TMap<int32, FHISMInstancePool> InstancePools;

	/**
	 * Rebuild a scatter type's HISM from all cached scatter data.
	 * Clears all instances and re-adds from manager's cache.
	 *
	 * @param ScatterTypeID The scatter type to rebuild
	 */
	void RebuildScatterType(int32 ScatterTypeID);

	/**
	 * Process all pending rebuilds.
	 * Called by Tick() or can be called manually to force immediate processing.
	 */
	void FlushPendingRebuilds();

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

	// ==================== Pending Rebuilds ====================

	/** Scatter types pending rebuild - processed in FlushPendingRebuilds() */
	TSet<int32> PendingRebuildScatterTypes;

	/** Maximum number of scatter types to rebuild per frame (0 = unlimited) */
	int32 MaxRebuildsPerFrame = 0;

	/** Minimum time since last viewer movement before processing rebuilds (seconds) */
	float RebuildStationaryDelay = 0.5f;

	/** Time since viewer last moved significantly */
	float TimeSinceViewerMoved = 0.0f;

	/** Last known viewer position for movement detection */
	FVector LastViewerPosition = FVector::ZeroVector;

	/** Movement threshold to consider viewer as "moving" */
	float ViewerMovementThreshold = 50.0f;

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
