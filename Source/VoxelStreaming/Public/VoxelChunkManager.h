// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VoxelCoreTypes.h"
#include "ChunkDescriptor.h"
#include "ChunkRenderData.h"
#include "LODTypes.h"
#include "VoxelCPUNoiseGenerator.h"
#include "InfinitePlaneWorldMode.h"
#include "IVoxelMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelChunkManager.generated.h"

// Forward declarations
class UVoxelWorldConfiguration;
class IVoxelLODStrategy;
class IVoxelMeshRenderer;
class FVoxelCPUSmoothMesher;

/**
 * Internal chunk state tracking.
 */
USTRUCT()
struct FVoxelChunkState
{
	GENERATED_BODY()

	/** Chunk descriptor with voxel data */
	UPROPERTY()
	FChunkDescriptor Descriptor;

	/** Current state in the streaming lifecycle */
	UPROPERTY()
	EChunkState State = EChunkState::Unloaded;

	/** Current LOD level */
	UPROPERTY()
	int32 LODLevel = 0;

	/** Current morph factor for LOD transitions */
	UPROPERTY()
	float MorphFactor = 0.0f;

	/** Frame number when state last changed */
	UPROPERTY()
	int64 LastStateChangeFrame = 0;

	/** Frame number when last updated */
	UPROPERTY()
	int64 LastUpdateFrame = 0;

	/** Priority for processing queues */
	float Priority = 0.0f;

	FVoxelChunkState() = default;

	explicit FVoxelChunkState(const FIntVector& InChunkCoord)
	{
		Descriptor.ChunkCoord = InChunkCoord;
	}
};

/**
 * Delegate fired when a chunk completes generation.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnChunkGenerated, FIntVector, ChunkCoord);

/**
 * Delegate fired when a chunk completes meshing and is visible.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnChunkLoaded, FIntVector, ChunkCoord);

/**
 * Delegate fired when a chunk is unloaded.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnChunkUnloaded, FIntVector, ChunkCoord);

/**
 * Voxel Chunk Manager Component.
 *
 * Main streaming coordinator for voxel terrain. Manages the lifecycle of
 * chunks from loading through generation, meshing, and rendering.
 *
 * Responsibilities:
 * - Track chunk states (Unloaded, PendingGeneration, Generating, etc.)
 * - Query LOD strategy for streaming decisions
 * - Queue and process generation/meshing requests (time-sliced)
 * - Update renderer with completed meshes
 * - Handle LOD transitions
 *
 * This is a skeleton implementation. Actual generation and meshing
 * will be implemented in Phase 2.
 *
 * @see IVoxelLODStrategy
 * @see IVoxelMeshRenderer
 * @see Documentation/ARCHITECTURE.md
 */
UCLASS(ClassGroup = (Voxel), meta = (BlueprintSpawnableComponent))
class VOXELSTREAMING_API UVoxelChunkManager : public UActorComponent
{
	GENERATED_BODY()

public:
	UVoxelChunkManager();

	// ==================== UActorComponent Interface ====================

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ==================== Initialization ====================

	/**
	 * Initialize the chunk manager with configuration.
	 *
	 * Must be called before the manager can process chunks.
	 * Sets up LOD strategy, renderer, and internal state.
	 *
	 * @param InConfig World configuration asset
	 * @param InLODStrategy LOD strategy to use (ownership transferred)
	 * @param InRenderer Mesh renderer to use (ownership NOT transferred)
	 */
	void Initialize(
		UVoxelWorldConfiguration* InConfig,
		IVoxelLODStrategy* InLODStrategy,
		IVoxelMeshRenderer* InRenderer
	);

	/**
	 * Shutdown and cleanup all resources.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	void Shutdown();

	/**
	 * Check if manager is initialized and ready.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	bool IsInitialized() const { return bIsInitialized; }

	// ==================== Streaming Control ====================

	/**
	 * Enable or disable chunk streaming.
	 *
	 * When disabled, no new chunks will be loaded or unloaded.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	void SetStreamingEnabled(bool bEnabled);

	/**
	 * Check if streaming is enabled.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	bool IsStreamingEnabled() const { return bStreamingEnabled; }

	/**
	 * Force update of streaming state.
	 *
	 * Call to immediately process streaming decisions without
	 * waiting for the next tick.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	void ForceStreamingUpdate();

	// ==================== Chunk Requests ====================

	/**
	 * Request a specific chunk to be loaded.
	 *
	 * @param ChunkCoord Chunk coordinate to load
	 * @param Priority Loading priority (higher = sooner)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	void RequestChunkLoad(const FIntVector& ChunkCoord, float Priority = 1.0f);

	/**
	 * Request a specific chunk to be unloaded.
	 *
	 * @param ChunkCoord Chunk coordinate to unload
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	void RequestChunkUnload(const FIntVector& ChunkCoord);

	/**
	 * Mark a chunk as dirty (needs remeshing).
	 *
	 * @param ChunkCoord Chunk coordinate to mark dirty
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	void MarkChunkDirty(const FIntVector& ChunkCoord);

	// ==================== Queries ====================

	/**
	 * Get the current state of a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Chunk state (Unloaded if not tracked)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	EChunkState GetChunkState(const FIntVector& ChunkCoord) const;

	/**
	 * Check if a chunk is fully loaded and visible.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if chunk is in Loaded state
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	bool IsChunkLoaded(const FIntVector& ChunkCoord) const;

	/**
	 * Get total number of tracked chunks (all states).
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	int32 GetTotalChunkCount() const { return ChunkStates.Num(); }

	/**
	 * Get number of fully loaded chunks.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	int32 GetLoadedChunkCount() const;

	/**
	 * Get number of chunks pending generation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	int32 GetPendingGenerationCount() const { return GenerationQueue.Num(); }

	/**
	 * Get number of chunks pending meshing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	int32 GetPendingMeshingCount() const { return MeshingQueue.Num(); }

	/**
	 * Get all loaded chunk coordinates.
	 *
	 * @param OutChunks Array to fill with coordinates
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	void GetLoadedChunks(TArray<FIntVector>& OutChunks) const;

	/**
	 * Get the chunk coordinate containing a world position.
	 *
	 * @param WorldPosition Position in world space
	 * @return Chunk coordinate
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	FIntVector WorldToChunkCoord(const FVector& WorldPosition) const;

	// ==================== Configuration Access ====================

	/**
	 * Get the world configuration.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	UVoxelWorldConfiguration* GetConfiguration() const { return Configuration; }

	/**
	 * Get the current LOD strategy.
	 */
	IVoxelLODStrategy* GetLODStrategy() const { return LODStrategy; }

	/**
	 * Get the current mesh renderer.
	 */
	IVoxelMeshRenderer* GetMeshRenderer() const { return MeshRenderer; }

	// ==================== Debug ====================

	/**
	 * Get debug statistics string.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	FString GetDebugStats() const;

	/**
	 * Draw debug visualization.
	 */
	void DrawDebugVisualization() const;

	/**
	 * Get the smooth mesher for debug access.
	 * Returns nullptr if mesher is not a smooth mesher or not initialized.
	 */
	FVoxelCPUSmoothMesher* GetSmoothMesher() const;

	// ==================== Events ====================

	/** Called when a chunk completes voxel generation */
	UPROPERTY(BlueprintAssignable, Category = "Voxel|ChunkManager")
	FOnChunkGenerated OnChunkGenerated;

	/** Called when a chunk is fully loaded and visible */
	UPROPERTY(BlueprintAssignable, Category = "Voxel|ChunkManager")
	FOnChunkLoaded OnChunkLoaded;

	/** Called when a chunk is unloaded */
	UPROPERTY(BlueprintAssignable, Category = "Voxel|ChunkManager")
	FOnChunkUnloaded OnChunkUnloaded;

protected:
	// ==================== Internal Update Methods ====================

	/**
	 * Build LOD query context from current camera state.
	 */
	FLODQueryContext BuildQueryContext() const;

	/**
	 * Update streaming decisions based on LOD strategy.
	 */
	void UpdateStreamingDecisions(const FLODQueryContext& Context);

	/**
	 * Process the generation queue (time-sliced).
	 *
	 * @param TimeSliceMS Maximum time to spend in milliseconds
	 */
	void ProcessGenerationQueue(float TimeSliceMS);

	/**
	 * Process the meshing queue (time-sliced).
	 *
	 * @param TimeSliceMS Maximum time to spend in milliseconds
	 */
	void ProcessMeshingQueue(float TimeSliceMS);

	/**
	 * Process the unload queue.
	 *
	 * @param MaxChunks Maximum chunks to unload this frame
	 */
	void ProcessUnloadQueue(int32 MaxChunks);

	/**
	 * Update LOD transitions for loaded chunks.
	 */
	void UpdateLODTransitions(const FLODQueryContext& Context);

	// ==================== Chunk State Management ====================

	/**
	 * Get or create chunk state for a coordinate.
	 */
	FVoxelChunkState& GetOrCreateChunkState(const FIntVector& ChunkCoord);

	/**
	 * Set chunk state and fire appropriate events.
	 */
	void SetChunkState(const FIntVector& ChunkCoord, EChunkState NewState);

	/**
	 * Remove chunk state tracking.
	 */
	void RemoveChunkState(const FIntVector& ChunkCoord);

	// ==================== Generation/Meshing Callbacks ====================

	/**
	 * Called when chunk generation completes.
	 * (Placeholder - actual implementation in Phase 2)
	 */
	void OnChunkGenerationComplete(const FIntVector& ChunkCoord);

	/**
	 * Called when chunk meshing completes.
	 * (Placeholder - actual implementation in Phase 2)
	 */
	void OnChunkMeshingComplete(const FIntVector& ChunkCoord);

	/**
	 * Queue neighbors for remeshing when a chunk's voxel data becomes available.
	 * This ensures seamless boundaries when chunks load in different orders.
	 * Only queues neighbors that are already in Loaded state.
	 *
	 * @param ChunkCoord The chunk that just finished generation
	 */
	void QueueNeighborsForRemesh(const FIntVector& ChunkCoord);

	/**
	 * Extract neighbor edge slices for seamless chunk boundaries.
	 *
	 * For each of 6 faces, checks if neighbor chunk is loaded and
	 * extracts the edge slice (ChunkSize² voxels) from neighbor's voxel data.
	 * Empty arrays indicate no neighbor (boundary faces will render).
	 *
	 * @param ChunkCoord Chunk coordinate to get neighbors for
	 * @param OutRequest Meshing request to populate with neighbor data
	 */
	void ExtractNeighborEdgeSlices(const FIntVector& ChunkCoord, FVoxelMeshingRequest& OutRequest);

protected:
	// ==================== Configuration ====================

	/** World configuration */
	UPROPERTY()
	TObjectPtr<UVoxelWorldConfiguration> Configuration;

	/** LOD strategy (owned by this manager) */
	IVoxelLODStrategy* LODStrategy = nullptr;

	/** Mesh renderer (NOT owned by this manager) */
	IVoxelMeshRenderer* MeshRenderer = nullptr;

	/** Whether the manager has been initialized */
	bool bIsInitialized = false;

	/** Whether streaming is enabled */
	bool bStreamingEnabled = true;

	// ==================== Chunk State ====================

	/** Map of chunk coordinates to their state */
	UPROPERTY()
	TMap<FIntVector, FVoxelChunkState> ChunkStates;

	/** Set of loaded chunk coordinates (for fast lookup) */
	TSet<FIntVector> LoadedChunkCoords;

	// ==================== Processing Queues ====================

	/** Chunks waiting to be generated (sorted by priority) */
	TArray<FChunkLODRequest> GenerationQueue;

	/** Chunks waiting to be meshed (sorted by priority) */
	TArray<FChunkLODRequest> MeshingQueue;

	/** Chunks waiting to be unloaded */
	TArray<FIntVector> UnloadQueue;

	// ==================== Runtime State ====================

	/** Current frame number */
	int64 CurrentFrame = 0;

	/** Cached viewer position */
	FVector CachedViewerPosition = FVector::ZeroVector;

	/** Cached viewer forward direction */
	FVector CachedViewerForward = FVector::ForwardVector;

	// ==================== Statistics ====================

	/** Total chunks generated this session */
	int64 TotalChunksGenerated = 0;

	/** Total chunks meshed this session */
	int64 TotalChunksMeshed = 0;

	/** Total chunks unloaded this session */
	int64 TotalChunksUnloaded = 0;

	// ==================== Generation Components ====================

	/** CPU noise generator for voxel data generation */
	TUniquePtr<FVoxelCPUNoiseGenerator> NoiseGenerator;

	/** World mode for terrain generation (Infinite Plane) */
	TUniquePtr<FInfinitePlaneWorldMode> WorldMode;

	/** CPU mesher for generating mesh geometry (polymorphic - can be cubic or smooth) */
	TUniquePtr<IVoxelMesher> Mesher;

	// ==================== Pending Mesh Storage ====================

	/** Pending mesh data waiting to be sent to renderer */
	struct FPendingMeshData
	{
		FIntVector ChunkCoord;
		int32 LODLevel;
		FChunkMeshData MeshData;
	};

	/** Queue of meshes waiting to be uploaded to renderer */
	TArray<FPendingMeshData> PendingMeshQueue;

	/** Maximum pending meshes before throttling generation - keep low to avoid render job overflow */
	static constexpr int32 MaxPendingMeshes = 4;
};
