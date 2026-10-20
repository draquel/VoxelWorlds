// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "Containers/Queue.h"
#include "Components/PrimitiveComponent.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "VoxelCollisionManager.generated.h"

class UVoxelWorldConfiguration;
class UVoxelChunkManager;
class UBodySetup;

/**
 * Lightweight collision-only component backed by a custom BodySetup.
 *
 * Unlike hacking a UBoxComponent's FBodyInstance, this component properly
 * overrides GetBodySetup() so the physics engine always uses our trimesh
 * BodySetup for sweeps, traces, and overlap queries.
 */
UCLASS(MinimalAPI)
class UVoxelCollisionComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelCollisionComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** Assign the BodySetup that contains our trimesh collision. */
	void SetCollisionBodySetup(UBodySetup* InBodySetup, const FBox& InLocalBounds);

	// --- UPrimitiveComponent overrides ---
	virtual UBodySetup* GetBodySetup() override { return CollisionBodySetup; }
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual bool ShouldCreatePhysicsState() const override { return CollisionBodySetup != nullptr; }

private:
	UPROPERTY()
	TObjectPtr<UBodySetup> CollisionBodySetup;

	/** Cached local-space bounding box (set when BodySetup is assigned). */
	FBox LocalBounds;
};

/**
 * Delegate fired when a chunk's collision becomes ready.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChunkCollisionReady, const FIntVector& /*ChunkCoord*/);

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

	/**
	 * The last cook produced no geometry: the chunk is entirely solid or entirely air, so there is
	 * nothing to collide with and no component. Counts as ready; an edit (bNeedsUpdate) re-cooks it.
	 */
	bool bEmpty = false;

	/**
	 * The neighbours (of the 26 the mesher reads; UVoxelCollisionManager::GetNeighborBit) that had no voxel data
	 * when the current cook launched. The mesher reads a missing neighbour as air, so the cook's boundary cells
	 * facing it are wrong until it is re-cooked: OnChunkVoxelDataAvailable marks the chunk dirty when one of
	 * them arrives (G32).
	 */
	uint32 MissingNeighborMask = 0;

	/**
	 * The neighbours of MissingNeighborMask that have arrived since the cook launched. The re-cook waits while another
	 * missing neighbour is still being generated, so the arrivals around a chunk fold into one re-cook.
	 */
	uint32 ArrivedNeighborMask = 0;

	/**
	 * bNeedsUpdate was set by a neighbour's arrival alone (no edit since): the re-cook queues below every first cook, so
	 * new chunks get collision first and the neighbours still arriving meanwhile fold into one re-cook. An edit clears it.
	 */
	bool bNeighborRecookOnly = false;

	/** Distance from viewer when last updated (for prioritization) */
	float LastDistance = 0.0f;

	/**
	 * Estimated cooked-trimesh memory, set when a cook result is applied. Used by
	 * GetTotalMemoryUsage instead of BodySetup->GetResourceSizeEx: walking the Chaos
	 * geometry per body cost ~1ms each, which made the per-frame debug HUD's memory
	 * readout take 80-100ms with a settled collision shell.
	 */
	int64 EstimatedBytes = 0;

	FChunkCollisionData() = default;

	explicit FChunkCollisionData(const FIntVector& InChunkCoord)
		: ChunkCoord(InChunkCoord)
	{
	}

	/**
	 * The chunk has usable collision right now: a registered body, or a cook that found no geometry.
	 * Stays true while a re-cook is in flight, because the previous body remains registered (and
	 * supporting pawns) until the new one replaces it in a single game-thread step.
	 */
	bool HasLiveCollision() const { return bEmpty || (BodySetup != nullptr && CollisionComponent != nullptr); }

	/** The collision matches the chunk's latest content: live and no cook in flight. */
	bool IsReady() const { return !bIsCooking && HasLiveCollision(); }
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

	/** Comparison for priority queue (lower priority value = earlier in array, highest at back) */
	bool operator<(const FCollisionCookRequest& Other) const
	{
		return Priority < Other.Priority;
	}
};

/**
 * Result of an async collision cooking task (mesh gen + trimesh construction on thread pool).
 */
/** One point ForcePathCoverage asks to have cooked: the chunk holding Position, at the path boost plus Bias. */
struct FVoxelCollisionCoverSample
{
	FVector Position = FVector::ZeroVector;
	float Bias = 0.0f;
};

struct FAsyncCollisionResult
{
	FIntVector ChunkCoord;
	int32 LODLevel = 0;
	TRefCountPtr<Chaos::FTriangleMeshImplicitObject> TriMesh;
	int32 NumVertices = 0;
	int32 NumTriangles = 0;
	bool bSuccess = false;
	/** Meshing succeeded but produced no triangles (all-solid or all-air chunk). Implies bSuccess. */
	bool bEmpty = false;
};

/**
 * Voxel Collision Manager.
 *
 * Manages distance-based collision generation for voxel terrain.
 * Mesh generation and trimesh construction run on a background thread pool.
 * Only physics component creation (lightweight) happens on the game thread.
 *
 * Design principles:
 * - Only chunks within CollisionRadius have collision
 * - Uses CollisionLODLevel for coarser collision meshes (fewer triangles)
 * - Async mesh gen + trimesh cook on thread pool to avoid main thread stalls
 * - Supports both PMC and Custom VF renderers
 *
 * Thread Safety: Update/Apply must be called from game thread only.
 * Background tasks access ChunkManager read-only state and stateless mesher.
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

	/**
	 * Per-Update game-thread sub-phase attribution (milliseconds), read by the chunk manager
	 * into FVoxelTimingStats after Update(): cook-launch preparation (request build + neighbor
	 * slices) vs completed-cook application (BodySetup + collision component recreation).
	 */
	float LastPrepMs = 0.0f;
	float LastApplyMs = 0.0f;

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

	/**
	 * A chunk's voxel data has just become available (its generation completed). Every cooked neighbour whose last
	 * cook ran without it (its MissingNeighborMask) gets a re-cook, so its boundary cells facing this chunk are built
	 * from the real voxels instead of the air the mesher read in their place (G32: stale collision strips along chunk
	 * faces). The re-cook is requested once none of that neighbour's other missing neighbours is still being generated
	 * (ProcessNeighborRecooks), and below every first cook in the queue; the old body stays live until it is replaced.
	 * Off with voxel.Collision.NeighborRecook 0. Called by the chunk manager.
	 *
	 * @param ChunkCoord The chunk whose voxel data arrived
	 * @return The number of cooked neighbours that read this chunk as air
	 */
	int32 OnChunkVoxelDataAvailable(const FIntVector& ChunkCoord);

	/**
	 * The bit of a neighbour offset in FChunkCollisionData::MissingNeighborMask.
	 *
	 * @param Offset A neighbour's offset from the chunk, each component in -1..1, not all zero
	 * @return Its bit (one of 26), or 0 for an offset that is not a neighbour
	 */
	static uint32 GetNeighborBit(const FIntVector& Offset);

	/** Re-cooks requested because a neighbour missing at cook time arrived (OnChunkVoxelDataAvailable), this session. */
	int64 GetTotalNeighborRecooks() const { return TotalNeighborRecooks; }

	/** Neighbour arrivals that needed no re-cook (air on both sides of the boundary), this session. */
	int64 GetTotalNeighborArrivalsSkipped() const { return TotalNeighborArrivalsSkipped; }

	/** Neighbour arrivals that needed no re-cook (solid on both sides: the cook's wall there is buried), this session. */
	int64 GetTotalNeighborArrivalsSkippedSolid() const { return TotalNeighborArrivalsSkippedSolid; }

	/**
	 * An edit changed a chunk's voxels: mark dirty the edge and corner neighbours whose cooks read the voxels it changed
	 * (the chunk manager marks the chunk and its six face neighbours already). A neighbour is marked when the edit's
	 * sphere comes within reach of its box: a cook reads up to stride + 1 voxels across a boundary (the deep planes), so
	 * that many voxels plus one. A radius of 0 or less (undo, redo) marks all of them. With solid-on-both-sides arrivals
	 * skipped (OnChunkVoxelDataAvailable), this is also what re-cooks a buried wall before a dig can expose it.
	 *
	 * @param EditedChunk The chunk whose voxels the edit changed
	 * @param EditCenter The edit's centre (world)
	 * @param EditRadius The edit's radius (world), or 0 or less when unknown
	 * @return The number of neighbours marked
	 */
	int32 MarkNeighborsDirtyForEdit(const FIntVector& EditedChunk, const FVector& EditCenter, float EditRadius);

	/**
	 * G32 audit (voxel.Collision.AuditNeighbors): how many cooked chunks read a neighbour as air that has voxel data now.
	 * With the re-cook on, a chunk is in that state only between the neighbour's arrival and the re-cook (counted as
	 * awaiting); with it off, until something else re-cooks it.
	 *
	 * @param OutCooked Chunks with a cooked body or an empty cook
	 * @param OutWithMissing Of those, cooked while some neighbour had no data (still missing or since arrived)
	 * @param OutStale Of those, a neighbour that was missing has data now and no re-cook is pending
	 * @param OutAwaiting Chunks marked for a re-cook (an arrival or an edit) that has not launched yet, or waiting for
	 *        another missing neighbour still being generated before their re-cook is requested
	 * @param OutStaleChunks If set, receives the stale chunks
	 */
	void AuditNeighborCooks(int32& OutCooked, int32& OutWithMissing, int32& OutStale, int32& OutAwaiting,
		TArray<FIntVector>* OutStaleChunks = nullptr) const;

	// ==================== Queries ====================

	/**
	 * Check whether a chunk has usable collision right now.
	 *
	 * True for a chunk with a registered collision body (including while a re-cook after an edit is in
	 * flight, since the previous body stays live until replaced) and for a chunk whose cook found no
	 * geometry (entirely solid or entirely air). False only when nothing has been cooked yet.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if a pawn in this chunk is supported by its current collision state
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	bool HasCollision(const FIntVector& ChunkCoord) const;

	/**
	 * Check whether a chunk's collision matches its latest content (live and no cook in flight).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if the chunk's collision is cooked from its current voxel data
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	bool IsCollisionUpToDate(const FIntVector& ChunkCoord) const;

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
	 * Get number of chunks currently being cooked asynchronously.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	int32 GetCookingCount() const { return AsyncCollisionInProgress.Num(); }

	/**
	 * Get number of chunks in cook queue.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Collision")
	int32 GetCookQueueCount() const { return CookingQueue.Num(); }

	/**
	 * Get approximate total memory usage of collision system in bytes.
	 */
	int64 GetTotalMemoryUsage() const;

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
	 * Get the radius actually used this frame (CollisionRadius plus the speed-aware lead when
	 * voxel.Collision.LeadFix is on). Diagnostics/validation for the Tier-0 fall-through fix.
	 * Plain (non-UFUNCTION) getter so adding it is Live-Coding-friendly.
	 */
	float GetCurrentEffectiveRadius() const { return CurrentEffectiveRadius; }

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

	/**
	 * Set the maximum number of concurrent async collision tasks.
	 *
	 * @param MaxTasks Max async tasks (1-4)
	 */
	void SetMaxAsyncCollisionTasks(int32 MaxTasks);

	// ==================== External Requests ====================

	/**
	 * Request collision generation for a chunk.
	 * Safe to call externally (e.g., from character spawn logic).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param Priority Processing priority (higher = sooner)
	 */
	void RequestCollision(const FIntVector& ChunkCoord, float Priority);

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

	/**
	 * Convert a render-order triangle list into the winding Chaos expects for a triangle mesh body.
	 * Physics winds triangles opposite to rendering: the engine cooker swaps the first two indices of
	 * every render-derived triangle (bFlipNormals), and this does the same for the voxel trimesh, which
	 * is built directly as an implicit object and so skips the cooker.
	 *
	 * @param RenderIndices Triangle list in render order (3 indices per triangle; a trailing partial triangle is ignored)
	 * @param OutTriangles  Receives one physics triangle per render triangle (replaced, not appended)
	 */
	static void BuildPhysicsTriangles(const TArray<uint32>& RenderIndices, TArray<Chaos::TVector<int32, 3>>& OutTriangles);

	/**
	 * Where the collision lead (ForcePathCoverage) looks, in the focus's local frame (spherical SP-C4): the focus itself,
	 * half a chunk along -Up under it (standing on a seam between chunk layers), then LookAhead chunks along the
	 * velocity's part across Up and, when the velocity along Up is below -1 (descending), the chunk one below each of
	 * those along -Up. With Up = (0, 0, 1) these are the heightmap points exactly (world -Z, the horizontal heading).
	 *
	 * @param Focus The pawn's position
	 * @param Velocity The pawn's velocity
	 * @param Up Local up at the focus (unit)
	 * @param ChunkWorldSize Chunk width
	 * @param LookAhead Chunks ahead to cover (0 = the two under the feet only)
	 * @param OutSamples Receives the samples, in priority order (replaced)
	 */
	static void ComputePathCoverageSamples(const FVector& Focus, const FVector& Velocity, const FVector& Up, float ChunkWorldSize,
		int32 LookAhead, TArray<FVoxelCollisionCoverSample>& OutSamples);

protected:
	// ==================== Internal Methods ====================

	/**
	 * Update which chunks should have collision based on viewer distance.
	 */
	void UpdateCollisionDecisions(const FVector& ViewerPosition);

	/**
	 * Resolve the position collision should center on. With voxel.Collision.LeadFix on, this is the
	 * player pawn (the camera trails it in 3rd person, which shrinks the forward collision lead);
	 * otherwise the passed viewer position.
	 */
	FVector ResolveFocusPosition(const FVector& FallbackViewerPosition) const;

	/**
	 * Tier 1 (LeadFix-gated): force-cook, at top priority, the chunk under the focus plus a short fan
	 * along its velocity across local up — including the chunk it is descending into — so ground under
	 * imminent (and downhill) footfalls is cooked before the pawn enters it. Overrides the normal
	 * center-distance priority (which deprioritizes a descending chunk whose center is far below).
	 */
	void ForcePathCoverage(const FVector& FocusPosition, const FVector& FocusVelocity);

	/**
	 * The cook priority of a dirty chunk at a distance: an edit's above a first cook's at the same distance, a
	 * neighbour arrival's re-cook below every first cook's (FChunkCollisionData::bNeighborRecookOnly).
	 *
	 * @param Data The chunk's collision state (bNeedsUpdate set)
	 * @param Distance The chunk centre's distance from the focus
	 * @return The priority to request it at
	 */
	float GetDirtyPriority(const FChunkCollisionData& Data, float Distance) const;

	/**
	 * Request the re-cooks of chunks whose missing neighbours arrived (PendingNeighborRecooks), each once none of its
	 * other missing neighbours is still being generated.
	 */
	void ProcessNeighborRecooks();

	/**
	 * Process dirty chunks that need collision regeneration (from edits).
	 */
	void ProcessDirtyChunks(const FVector& ViewerPosition);

	/**
	 * Process the cooking queue — launches async tasks (lightweight, no mesh gen on game thread).
	 */
	void ProcessCookingQueue();

	/**
	 * Launch async collision cook for a chunk on the thread pool.
	 * Performs mesh generation + Chaos trimesh construction off the game thread.
	 *
	 * @param Request The cook request (ChunkCoord, LODLevel, Priority)
	 */
	void LaunchAsyncCollisionCook(const FCollisionCookRequest& Request);

	/**
	 * Process completed async collision results (game thread).
	 * Drains the MPSC queue and applies results by creating physics components.
	 */
	void ProcessCompletedCollisionCooks();

	/**
	 * Apply a completed collision result on the game thread.
	 * Creates BodySetup, collision component, and registers physics.
	 *
	 * @param Result The async result containing the trimesh
	 */
	void ApplyCollisionResult(FAsyncCollisionResult& Result);

	/**
	 * Apply a cook that found no geometry: drop any previous body and mark the chunk ready-empty, so
	 * neither the decision sweep nor the path look-ahead re-requests it until an edit dirties it.
	 *
	 * @param ChunkCoord The chunk whose cook produced no triangles
	 */
	void ApplyEmptyCollisionResult(const FIntVector& ChunkCoord);

	/**
	 * Remove collision data for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 */
	void RemoveCollision(const FIntVector& ChunkCoord);

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

	/** Maximum concurrent async collision tasks */
	int32 MaxAsyncCollisionTasks = 2;

	/** Maximum completed results to apply per frame (physics registration) */
	int32 MaxAppliesPerFrame = 2;

	// ==================== Collision Storage ====================

	/** Map of chunk coordinates to collision data */
	UPROPERTY()
	TMap<FIntVector, FChunkCollisionData> CollisionData;

	// ==================== Async Cooking Pipeline ====================

	/** Queue of chunks waiting to be launched as async tasks */
	TArray<FCollisionCookRequest> CookingQueue;

	/** Set of chunk coords in CookingQueue for O(1) duplicate detection */
	TSet<FIntVector> CookingQueueSet;

	/** Set of chunks currently being cooked asynchronously on thread pool */
	TSet<FIntVector> AsyncCollisionInProgress;

	/** Thread-safe MPSC queue for completed async collision results */
	TQueue<FAsyncCollisionResult, EQueueMode::Mpsc> CompletedCollisionQueue;

	// ==================== Cached State ====================

	/** Last viewer position for change detection */
	FVector LastViewerPosition = FVector(FLT_MAX);

	/** Threshold for viewer movement to trigger collision update */
	static constexpr float UpdateThreshold = 1000.0f;

	/** True until we've successfully queued initial collision (chunks may not be loaded on first frame) */
	bool bPendingInitialUpdate = true;

	/**
	 * Seconds since the last collision decision sweep. Sweeps only consider LOADED chunks, so a pawn that stops
	 * right after a teleport (save restore, vox.TP) would otherwise never get collision for the chunks that
	 * finish loading around it; voxel.Collision.ResweepSeconds re-runs the sweep while stationary.
	 */
	float SecondsSinceDecisions = 0.0f;

	// ==================== Tier 0: pawn-centered, speed-aware collision (voxel.Collision.LeadFix) ====================

	/** Last focus (pawn) position, for velocity estimation. FLT_MAX until the first Update(). */
	FVector LastFocusPosition = FVector(FLT_MAX);

	/** Smoothed focus speed (uu/s); drives the speed-aware radius lead. */
	float SmoothedFocusSpeed = 0.0f;

	/** Collision radius actually used this frame (CollisionRadius + speed lead). Refreshed in Update(). */
	float CurrentEffectiveRadius = 5000.0f;

	/** True when the focus was ~stationary last frame; used to force an eager stop->start re-decision. */
	bool bWasStationary = true;

	/** Speed (uu/s) at or below which the focus counts as stationary. */
	static constexpr float StationarySpeedThreshold = 10.0f;

	// ==================== Statistics ====================

	/** Total collision meshes generated */
	int64 TotalCollisionsGenerated = 0;

	/** Total collision meshes removed */
	int64 TotalCollisionsRemoved = 0;

	/** Re-cooks requested because a neighbour missing at cook time arrived */
	int64 TotalNeighborRecooks = 0;

	/** Arrivals that needed no re-cook: the voxels on both sides of the boundary are air (OnChunkVoxelDataAvailable) */
	int64 TotalNeighborArrivalsSkipped = 0;

	/** Arrivals that needed no re-cook: solid on both sides, the cook's wall along the boundary buried in the rock */
	int64 TotalNeighborArrivalsSkippedSolid = 0;

	/** Chunks with neighbours arrived since their cook (FChunkCollisionData::ArrivedNeighborMask), awaiting ProcessNeighborRecooks */
	TSet<FIntVector> PendingNeighborRecooks;
};
