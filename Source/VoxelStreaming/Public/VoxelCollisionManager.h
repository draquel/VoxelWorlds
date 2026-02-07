// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelCollisionManager.generated.h"

class UVoxelWorldConfiguration;
class UVoxelChunkManager;
class UBodySetup;

/**
 * Delegate fired when a chunk's collision becomes ready.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChunkCollisionReady, const FIntVector& /*ChunkCoord*/);

/**
 * Per-chunk collision data storage.
 */
// Forward declaration
class UShapeComponent;

USTRUCT()
struct FChunkCollisionData
{
	GENERATED_BODY()

	/** Chunk coordinate */
	UPROPERTY()
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** UBodySetup containing cooked collision data */
	UPROPERTY()
	TObjectPtr<UBodySetup> BodySetup;

	/** Collision component that holds the physics body */
	UPROPERTY()
	TObjectPtr<UPrimitiveComponent> CollisionComponent;

	/** LOD level used for this collision mesh */
	UPROPERTY()
	int32 CollisionLODLevel = 0;

	/** Whether collision data is currently being cooked */
	bool bIsCooking = false;

	/** Whether collision needs to be regenerated (e.g., after edit) */
	bool bNeedsUpdate = false;

	/** Distance from viewer when last updated (for prioritization) */
	float LastDistance = 0.0f;

	FChunkCollisionData() = default;

	explicit FChunkCollisionData(const FIntVector& InChunkCoord)
		: ChunkCoord(InChunkCoord)
	{
	}

	/** Check if collision data is valid and ready to use */
	bool IsReady() const { return BodySetup != nullptr && !bIsCooking && CollisionComponent != nullptr; }
};

/**
 * Internal collision cooking request.
 */
struct FCollisionCookRequest
{
	/** Chunk coordinate */
	FIntVector ChunkCoord;

	/** LOD level for collision mesh generation */
	int32 LODLevel = 0;

	/** Priority for processing (higher = sooner) */
	float Priority = 0.0f;

	/** Mesh vertices (local space) */
	TArray<FVector3f> Vertices;

	/** Mesh triangle indices */
	TArray<uint32> Indices;

	/** Comparison for priority queue (higher priority first) */
	bool operator<(const FCollisionCookRequest& Other) const
	{
		return Priority > Other.Priority;
	}
};

/**
 * Voxel Collision Manager.
 *
 * Manages distance-based collision generation for voxel terrain.
 * Uses async cooking to prevent frame hitches.
 *
 * Design principles:
 * - Only chunks within CollisionRadius have collision
 * - Uses CollisionLODLevel for coarser collision meshes (fewer triangles)
 * - Async UBodySetup cooking to avoid main thread stalls
 * - Supports both PMC and Custom VF renderers
 *
 * Thread Safety: Must be accessed from game thread only.
 *
 * @see UVoxelChunkManager
 * @see UBodySetup
 */
UCLASS(BlueprintType)
class VOXELSTREAMING_API UVoxelCollisionManager : public UObject
{
	GENERATED_BODY()

public:
	UVoxelCollisionManager();

	// ==================== Lifecycle ====================

	/**
	 * Initialize the collision manager.
	 *
	 * @param Config World configuration
	 * @param ChunkMgr Chunk manager for accessing mesh data
	 */
	void Initialize(UVoxelWorldConfiguration* Config, UVoxelChunkManager* ChunkMgr);

	/**
	 * Shutdown and cleanup all collision resources.
	 */
	void Shutdown();

	/**
	 * Check if manager is initialized.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	bool IsInitialized() const { return bIsInitialized; }

	// ==================== Per-Frame Update ====================

	/**
	 * Update collision state based on viewer position.
	 *
	 * Call this every frame from the chunk manager tick.
	 * Handles loading/unloading collision and processing cook queue.
	 *
	 * @param ViewerPosition Current viewer/camera position
	 * @param DeltaTime Time since last update
	 */
	void Update(const FVector& ViewerPosition, float DeltaTime);

	// ==================== Dirty Marking ====================

	/**
	 * Mark a chunk's collision as dirty (needs regeneration).
	 *
	 * Call this when terrain edits occur.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	void MarkChunkDirty(const FIntVector& ChunkCoord);

	/**
	 * Force immediate regeneration of a chunk's collision.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	void RegenerateChunkCollision(const FIntVector& ChunkCoord);

	// ==================== Queries ====================

	/**
	 * Check if a chunk has collision data.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if chunk has valid collision
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	bool HasCollision(const FIntVector& ChunkCoord) const;

	/**
	 * Get a chunk's body setup.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return BodySetup or nullptr if not available
	 */
	UBodySetup* GetChunkBodySetup(const FIntVector& ChunkCoord) const;

	/**
	 * Get number of chunks with collision.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	int32 GetCollisionChunkCount() const { return CollisionData.Num(); }

	/**
	 * Get number of chunks currently being cooked.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	int32 GetCookingCount() const { return CurrentlyCooking.Num(); }

	/**
	 * Get number of chunks in cook queue.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	int32 GetCookQueueCount() const { return CookingQueue.Num(); }

	// ==================== Configuration ====================

	/**
	 * Set the collision radius (distance from viewer).
	 *
	 * @param Radius New collision radius in world units
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	void SetCollisionRadius(float Radius);

	/**
	 * Get the current collision radius.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	float GetCollisionRadius() const { return CollisionRadius; }

	/**
	 * Set the LOD level used for collision meshes.
	 *
	 * @param LODLevel LOD level (higher = coarser, fewer triangles)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	void SetCollisionLODLevel(int32 LODLevel);

	/**
	 * Get the current collision LOD level.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	int32 GetCollisionLODLevel() const { return CollisionLODLevel; }

	// ==================== Events ====================

	/** Called when a chunk's collision becomes ready */
	FOnChunkCollisionReady OnCollisionReady;

	/** Called when a chunk's collision is removed */
	FOnChunkCollisionReady OnCollisionRemoved;

	// ==================== Debug ====================

	/**
	 * Get debug statistics string.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	FString GetDebugStats() const;

	/**
	 * Draw debug visualization of collision bounds.
	 */
	void DrawDebugVisualization(UWorld* World, const FVector& ViewerPosition) const;

protected:
	// ==================== Internal Methods ====================

	/**
	 * Update which chunks should have collision based on viewer distance.
	 */
	void UpdateCollisionDecisions(const FVector& ViewerPosition);

	/**
	 * Process dirty chunks that need collision regeneration (from edits).
	 * Called every frame to ensure edits are reflected in collision promptly.
	 */
	void ProcessDirtyChunks(const FVector& ViewerPosition);

	/**
	 * Process the cooking queue (start new cooks, check completions).
	 */
	void ProcessCookingQueue();

	/**
	 * Request collision generation for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param Priority Processing priority
	 */
	void RequestCollision(const FIntVector& ChunkCoord, float Priority);

	/**
	 * Remove collision data for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	void RemoveCollision(const FIntVector& ChunkCoord);

	/**
	 * Generate collision mesh data for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param OutVerts Output vertices
	 * @param OutIndices Output indices
	 * @return True if mesh generation succeeded
	 */
	bool GenerateCollisionMesh(
		const FIntVector& ChunkCoord,
		TArray<FVector3f>& OutVerts,
		TArray<uint32>& OutIndices);

	/**
	 * Start async cooking for a collision request.
	 *
	 * @param Request The cook request with mesh data
	 */
	void StartAsyncCook(const FCollisionCookRequest& Request);

	/**
	 * Called when async cooking completes.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param bSuccess Whether cooking succeeded
	 */
	void OnCookComplete(const FIntVector& ChunkCoord, bool bSuccess);

	/**
	 * Create a new BodySetup for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return New BodySetup or nullptr
	 */
	UBodySetup* CreateBodySetup(const FIntVector& ChunkCoord);

	/**
	 * Create a collision component for a chunk and register it with the world.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param BodySetup The body setup containing collision geometry
	 * @return The created collision component or nullptr
	 */
	UPrimitiveComponent* CreateCollisionComponent(const FIntVector& ChunkCoord, UBodySetup* BodySetup);

	/**
	 * Destroy a chunk's collision component.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	void DestroyCollisionComponent(const FIntVector& ChunkCoord);

protected:
	// ==================== Configuration ====================

	/** World configuration reference */
	UPROPERTY()
	TObjectPtr<UVoxelWorldConfiguration> Configuration;

	/** Chunk manager reference (for accessing mesh data) */
	UPROPERTY()
	TObjectPtr<UVoxelChunkManager> ChunkManager;

	/** World reference for spawning collision components */
	UPROPERTY()
	TObjectPtr<UWorld> CachedWorld;

	/** Container actor for collision components */
	UPROPERTY()
	TObjectPtr<AActor> CollisionContainerActor;

	/** Whether the manager is initialized */
	bool bIsInitialized = false;

	// ==================== Collision Settings ====================

	/** Maximum distance from viewer for collision generation */
	float CollisionRadius = 5000.0f;

	/** LOD level to use for collision (higher = fewer triangles) */
	int32 CollisionLODLevel = 1;

	/** Maximum collision cooking operations per frame */
	int32 MaxCooksPerFrame = 2;

	/** Maximum concurrent cooking operations */
	int32 MaxConcurrentCooks = 4;

	// ==================== Collision Storage ====================

	/** Map of chunk coordinates to collision data */
	UPROPERTY()
	TMap<FIntVector, FChunkCollisionData> CollisionData;

	// ==================== Cooking Queue ====================

	/** Queue of chunks waiting for collision cooking */
	TArray<FCollisionCookRequest> CookingQueue;

	/** Set of chunk coords in CookingQueue for O(1) duplicate detection */
	TSet<FIntVector> CookingQueueSet;

	/** Set of chunks currently being cooked */
	TSet<FIntVector> CurrentlyCooking;

	// ==================== Cached State ====================

	/** Last viewer position for change detection */
	FVector LastViewerPosition = FVector(FLT_MAX);

	/** Threshold for viewer movement to trigger collision update */
	static constexpr float UpdateThreshold = 500.0f;

	// ==================== Statistics ====================

	/** Total collision meshes generated */
	int64 TotalCollisionsGenerated = 0;

	/** Total collision meshes removed */
	int64 TotalCollisionsRemoved = 0;
};
