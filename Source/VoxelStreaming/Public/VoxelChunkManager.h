// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "VoxelCoreTypes.h"
#include "ChunkDescriptor.h"
#include "ChunkRenderData.h"
#include "LODTypes.h"
#include "IVoxelNoiseGenerator.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelTerrainConditioning.h"
#include "InfinitePlaneWorldMode.h"
#include "IVoxelMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelStreamingBenchmark.h"
#include "VoxelSeamRegistry.h"
#include "VoxelChunkManager.generated.h"

// Forward declarations
class UVoxelWorldConfiguration;
class FVoxelGPUNoiseGenerator;
class IVoxelLODStrategy;
class IVoxelMeshRenderer;
class FVoxelCPUMarchingCubesMesher;
class UVoxelEditManager;
class UVoxelCollisionManager;
class UVoxelScatterManager;
class UVoxelWaterPropagation;
class FVoxelSeamRegistry;

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

	/** Target LOD level (updated immediately when LOD change is queued) */
	UPROPERTY()
	int32 LODLevel = 0;

	/** LOD level of the currently rendered mesh (updated when mesh is submitted to renderer) */
	UPROPERTY()
	int32 MeshedLODLevel = 0;

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

	/**
	 * Frame of the most recent pending neighbor-remesh request (0 = none). A newly generated chunk
	 * asks its 26 loaded neighbors to re-mesh; instead of re-meshing each one immediately (which
	 * re-meshes a chunk repeatedly as its neighborhood fills in), the request is debounced — this
	 * stamp is bumped on each request and the chunk is enqueued once, after the neighborhood settles.
	 */
	int64 NeighborRemeshRequestFrame = 0;

	FVoxelChunkState() = default;

	explicit FVoxelChunkState(const FIntVector& InChunkCoord)
	{
		Descriptor.ChunkCoord = InChunkCoord;
	}
};

/**
 * One neighbour's boundary-relevant state, snapshotted when a chunk is dispatched to mesh. A chunk's
 * baked boundary depends on each neighbour's voxel content (ContentVersion), its rendered LOD
 * (MeshedLODLevel, which sets transition faces), and whether it had data at all (bHadData; an absent
 * neighbour bakes as clamped/Air). If any of these differs at mesh completion, the boundary is stale.
 */
struct FMeshBoundaryDep
{
	uint32 ContentVersion = 0;
	int32 MeshedLODLevel = -1;
	bool bHadData = false;
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

/** Why a chunk was (re)queued for meshing — used to attribute re-mesh thrash in the benchmark. */
enum class EVoxelRemeshReason : uint8
{
	Other = 0,        // first-time mesh or uncategorised path
	NeighborRemesh,   // a neighbour's voxel data became available (seam fix cascade)
	LODTransition,    // the chunk's LOD level changed as the viewer moved
	Dirty,            // explicit dirty (voxel edit, forced remesh)
	Count
};

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
	/** Out-of-line (defaulted in .cpp) so the TUniquePtr<FVoxelSeamRegistry> deleter sees the complete type. */
	virtual ~UVoxelChunkManager() override;

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

	/**
	 * Get voxel data at a world position.
	 * Returns Air if the position is in an unloaded or ungenerated chunk.
	 *
	 * @param WorldPosition Position in world space
	 * @return Voxel data at that position
	 */
	FVoxelData GetVoxelAtWorldPosition(const FVector& WorldPosition) const;

	/**
	 * Like GetVoxelAtWorldPosition, but applies the chunk's edit layer so the result reflects
	 * runtime player edits (dig/build). Returns Air for unloaded/ungenerated chunks. Game thread only.
	 */
	FVoxelData GetEditMergedVoxelAtWorldPosition(const FVector& WorldPosition) const;

	/**
	 * EDIT-AWARE surface query at a world X,Y (the near band for PCG decoration).
	 *
	 * Locates the surface from edit-merged voxel data of loaded chunks, so it reflects player
	 * digging/building and hugs the actual voxelized surface (not the analytic generator height).
	 * Returns false when no loaded chunk covers the surface there, so the caller can fall back to the
	 * generator (FVoxelSurfaceQuery::SampleSurface) for the far band. Game thread only.
	 *
	 * @param WorldX,WorldY     Sample position (world space)
	 * @param OutHeight         World Z of the surface
	 * @param OutNormal         Surface normal (from the edit-merged density gradient; up-facing)
	 * @param OutSlopeDegrees   Slope angle (0 = flat)
	 * @param OutMaterialID     Surface material from the edit-merged voxel
	 * @param OutBiomeID        Biome from the edit-merged voxel
	 * @return true if a loaded edit-merged surface was found
	 */
	bool QueryEditMergedSurface(
		double WorldX, double WorldY,
		float& OutHeight, FVector& OutNormal, float& OutSlopeDegrees,
		uint8& OutMaterialID, uint8& OutBiomeID) const;

	/**
	 * Analytic surface height at a world (X,Y): the continentalness-modulated base terrain PLUS any
	 * terrain conditioning zones — matching what generation produces — WITHOUT requiring the chunk to be
	 * loaded. This is the canonical FAR-BAND placement estimate (spawn / nav / POI seeding). For the near
	 * band where a chunk is loaded and may hold player edits, prefer QueryEditMergedSurface.
	 *
	 * Unlike the raw IVoxelWorldMode::GetTerrainHeightAt, this accounts for both continentalness (via the
	 * world mode's biome context) and conditioning-zone flattening, so the returned Z sits on the real
	 * generated isosurface even far from the origin and inside POI/claim footprints.
	 *
	 * @param WorldX,WorldY Sample position (world space)
	 * @return World Z of the generated terrain surface at (X,Y); 0 if the manager is not initialized
	 *
	 * Note: plain C++ (not a UFUNCTION) to keep double-precision world coordinates, matching the sibling
	 * QueryEditMergedSurface. A float-param BlueprintCallable wrapper can be added if BP access is needed.
	 */
	float GetGeneratedSurfaceHeight(double WorldX, double WorldY) const;

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

	/**
	 * Get the edit manager.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	UVoxelEditManager* GetEditManager() const { return EditManager; }

	/**
	 * Get the collision manager.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	UVoxelCollisionManager* GetCollisionManager() const { return CollisionManager; }

	/**
	 * Get the world mode used for terrain generation.
	 * Allows external code to query deterministic terrain height without loaded chunks.
	 *
	 * @return World mode interface, or nullptr if not initialized
	 */
	const IVoxelWorldMode* GetWorldMode() const { return WorldMode.Get(); }

	/**
	 * Get the scatter manager.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	UVoxelScatterManager* GetScatterManager() const { return ScatterManager; }

	// ==================== Terrain Conditioning (Phase 6c) ====================

	/**
	 * Add a gen-time terrain conditioning zone (flattens terrain toward a target under POIs/claims).
	 * Affects chunks generated AFTER this call. For deterministic POIs, register conditioning at world
	 * init so chunks generate already-conditioned. Re-conditioning already-loaded chunks is Phase 7.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager|Conditioning")
	void AddConditioningZone(const FVoxelConditioningZone& Zone);

	/** Remove all static conditioning zones. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager|Conditioning")
	void ClearConditioningZones();

	/** Number of registered static conditioning zones. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager|Conditioning")
	int32 GetConditioningZoneCount() const { return ConditioningZones.Num(); }

	/**
	 * Register a dynamic terrain conditioner (game-side, boundary-safe — e.g. supplying zones from POI
	 * claims). Queried per chunk at generation-request build time. Not owned; caller manages lifetime.
	 */
	void SetTerrainConditioner(IVoxelTerrainConditioner* InConditioner) { TerrainConditioner = InConditioner; }

	// ==================== Collision Mesh Generation ====================

	/**
	 * Generate collision mesh data for a chunk at a specific LOD level.
	 *
	 * Used by VoxelCollisionManager to generate collision geometry.
	 * The mesh is generated fresh from voxel data with edits applied.
	 *
	 * @param ChunkCoord Chunk coordinate to generate collision for
	 * @param LODLevel LOD level for collision mesh (higher = simpler mesh)
	 * @param OutMeshData Output mesh data (positions and indices)
	 * @return True if mesh was generated successfully
	 */
	bool GetChunkCollisionMesh(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		FChunkMeshData& OutMeshData);

	/**
	 * Prepare a meshing request for collision (game thread only).
	 *
	 * Copies voxel data, merges edits, and extracts neighbor slices.
	 * The returned request can then be dispatched to GenerateMeshCPU on a background thread.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param LODLevel LOD level for collision mesh
	 * @param OutMeshRequest Output meshing request ready for GenerateMeshCPU
	 * @return True if request was prepared successfully
	 */
	bool PrepareCollisionMeshRequest(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		FVoxelMeshingRequest& OutMeshRequest);

	/**
	 * Get raw pointer to the mesher (for async dispatch).
	 * The mesher's GenerateMeshCPU is stateless and thread-safe.
	 */
	IVoxelMesher* GetMesherPtr() const { return Mesher.Get(); }

	// ==================== Performance Stats ====================

	/** Voxel-specific memory breakdown */
	struct FVoxelMemoryStats
	{
		int64 VoxelDataBytes = 0;      // ChunkStates voxel data
		int64 EditDataBytes = 0;       // Edit manager memory
		int64 RendererCPUBytes = 0;    // Renderer CPU-side memory
		int64 RendererGPUBytes = 0;    // Renderer GPU memory
		int64 CollisionBytes = 0;      // Collision manager memory
		int64 ScatterBytes = 0;        // Scatter manager + renderer memory
		int64 TotalBytes = 0;          // Sum of above

		// Far-chunk compression residency breakdown (chunk counts + bytes reclaimed).
		int32 ResidentChunks = 0;      // raw array in memory
		int32 UniformChunks = 0;       // collapsed to a single value
		int32 CompressedChunks = 0;    // held in a compressed side buffer (PR C)
		int32 EmptyChunks = 0;         // no voxel payload
		int64 CompressionSavedBytes = 0; // raw bytes NOT resident thanks to uniform/compressed tiers
	};

	/** Per-system timing breakdown (milliseconds) */
	struct FVoxelTimingStats
	{
		float GenerationMs = 0.0f;
		float MeshingMs = 0.0f;
		float RenderSubmitMs = 0.0f;
		float CollisionMs = 0.0f;
		float ScatterMs = 0.0f;
		float LODMs = 0.0f;
		float StreamingMs = 0.0f;
		float TotalMs = 0.0f;

		// Generation sub-phases (GenerationMs = launch + poll + apply). Added to attribute the
		// residual game-thread generation cost after the post-passes moved to the GPU.
		float GenLaunchMs = 0.0f;   // ProcessGenerationQueue: build requests + dispatch
		float GenPollMs = 0.0f;     // ProcessPendingGPUReadbacks: poll fences + hand off ready data
		float GenApplyMs = 0.0f;    // ProcessCompletedAsyncGenerations total
		float GenStoreMs = 0.0f;    // apply: move result into chunk state (frees any old data)
		float GenNotifyMs = 0.0f;   // apply: OnChunkGenerationComplete (queues + neighbors + delegates)
		float GenNeighborMs = 0.0f; // subset of notify: QueueNeighborsForRemesh
		int32 GenApplyCount = 0;    // completions applied this tick

		// Meshing sub-phases (MeshingMs = tick + launch + apply)
		float MeshTickMs = 0.0f;    // Mesher->Tick: GPU meshers poll readbacks here
		float MeshLaunchMs = 0.0f;  // ProcessMeshingQueue: snapshot + slices + dispatch
		float MeshApplyMs = 0.0f;   // ProcessCompletedAsyncMeshes

		// Launch-path breakdown (subsets of MeshLaunchMs)
		float MeshSnapshotMs = 0.0f;  // voxel-data snapshot copy + edit merge
		float MeshSlicesMs = 0.0f;    // ExtractNeighborEdgeSlices
		float MeshDispatchMs = 0.0f;  // LaunchAsyncMeshGeneration (worker handoff for DC/MC GPU)
		int32 MeshLaunchCount = 0;    // mesh jobs launched this tick

		// Seam pipeline (seam-ownership P1+): TickSeamScheduler total — registry scheduling,
		// job drain/dispatch (snapshot lookups), and completed-seam submits to the renderer.
		float SeamMs = 0.0f;

		// Render-submit breakdown (RenderSubmitMs = mesh drain + unload + water tiles; flush is separate)
		float RenderMeshMs = 0.0f;       // OnChunkMeshingComplete drain loop total
		float RenderSubRendererMs = 0.0f; // subset: renderer UpdateChunkMeshFromCPU (vertex convert + handoff)
		float RenderSubScatterMs = 0.0f; // subset: ScatterManager OnChunkMeshDataReady
		float RenderSubWaterMs = 0.0f;   // subset: PropagateWaterFromNeighbors + UpdateWaterTileContribution
		float RenderUnloadMs = 0.0f;     // ProcessUnloadQueue
		float RenderWaterTileMs = 0.0f;  // ProcessDirtyWaterTiles (water tile mesh rebuilds)
		float RenderFlushMs = 0.0f;      // MeshRenderer FlushPendingOperations (end of tick)
		int32 RenderSubmitCount = 0;     // meshes submitted to the renderer this tick
	};

	/** Get voxel-specific memory usage breakdown */
	FVoxelMemoryStats GetVoxelMemoryStats() const;

	/** Get per-system timing stats from last frame */
	const FVoxelTimingStats& GetTimingStats() const { return LastTimingStats; }

	/** Get effective (adaptive) throttle values */
	int32 GetEffectiveMaxAsyncGenerationTasks() const { return EffectiveMaxAsyncGenerationTasks; }
	int32 GetEffectiveMaxAsyncMeshTasks() const { return EffectiveMaxAsyncMeshTasks; }
	int32 GetEffectiveMaxLODRemeshPerFrame() const { return EffectiveMaxLODRemeshPerFrame; }
	int32 GetEffectiveMaxPendingMeshes() const { return EffectiveMaxPendingMeshes; }
	bool AreSubsystemsDeferred() const { return bSubsystemsDeferred; }

	/** Get count of async generation tasks in flight */
	int32 GetAsyncGenerationInProgressCount() const { return AsyncGenerationInProgress.Num(); }

	// ==================== Streaming Benchmark ====================

	/** Drive streaming from a fixed position instead of the camera (deterministic benchmark). */
	void SetBenchmarkView(bool bActive, const FVector& Position) { bBenchmarkViewActive = bActive; BenchmarkViewPosition = Position; }
	bool IsBenchmarkViewActive() const { return bBenchmarkViewActive; }

	/** Queue depths for benchmark sampling. */
	int32 GetPendingUnloadCount() const { return UnloadQueue.Num(); }
	int32 GetPendingMeshUploadCount() const { return PendingMeshQueue.Num(); }

	/** Thrash: chunks re-queued for meshing after they already had a mesh. */
	int64 GetBenchRemeshCount() const { return BenchRemeshCount; }
	int64 GetBenchRemeshByReason(EVoxelRemeshReason Reason) const { return BenchRemeshByReason[static_cast<int32>(Reason)]; }

	/** Unload-lag (enqueue -> actual unload), in milliseconds. */
	void GetBenchUnloadLagStats(double& OutMeanMs, double& OutMaxMs, int64& OutCount) const
	{
		OutCount = BenchUnloadLagCount;
		OutMeanMs = (BenchUnloadLagCount > 0) ? (BenchUnloadLagSumMs / static_cast<double>(BenchUnloadLagCount)) : 0.0;
		OutMaxMs = BenchUnloadLagMaxMs;
	}

	/** Unload distance (viewer -> chunk at actual unload), in world units — the "lazy deletion"
	 *  decision in distance form (how far past the active radius chunks linger before deletion). */
	void GetBenchUnloadDistStats(double& OutMeanUU, double& OutMaxUU) const
	{
		OutMeanUU = (BenchUnloadLagCount > 0) ? (BenchUnloadDistSumUU / static_cast<double>(BenchUnloadLagCount)) : 0.0;
		OutMaxUU = BenchUnloadDistMaxUU;
	}

	/** Start a deterministic streaming benchmark run (drives the streaming origin, samples each
	 *  frame from TickComponent, and writes a CSV + JSON report on completion). */
	void StartBenchmark(const FVoxelBenchConfig& InConfig);

	/** Reset benchmark counters (call at the start of a benchmark run). */
	void ResetBenchCounters()
	{
		BenchRemeshCount = 0;
		for (int64& C : BenchRemeshByReason) { C = 0; }
		BenchUnloadLagSumMs = 0.0;
		BenchUnloadLagMaxMs = 0.0;
		BenchUnloadLagCount = 0;
		BenchUnloadDistSumUU = 0.0;
		BenchUnloadDistMaxUU = 0.0;
		UnloadEnqueueTimeSeconds.Reset();
		BenchEverMeshed.Reset();
	}

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
	 * Get the Marching Cubes mesher for debug access.
	 * Returns nullptr if mesher is not a MarchingCubes mesher or not initialized.
	 */
	FVoxelCPUMarchingCubesMesher* GetMarchingCubesMesher() const;

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
	 * Resolve the per-frame chunk load/submit budget: voxel.Stream.MaxLoadPerFrame override when > 0,
	 * else the configured MaxChunksToLoadPerFrame. Raise for fast/far traversal (high-speed regime).
	 */
	int32 ResolveMaxLoadPerFrame() const;

	/**
	 * Update LOAD decisions based on LOD strategy.
	 * Called only when viewer chunk changes (expensive operation).
	 */
	void UpdateLoadDecisions(const FLODQueryContext& Context);

	/**
	 * Update UNLOAD decisions based on LOD strategy.
	 * Called every frame to ensure orphaned chunks are cleaned up.
	 */
	void UpdateUnloadDecisions(const FLODQueryContext& Context);

	/**
	 * Process the generation queue (time-sliced).
	 *
	 * @param TimeSliceMS Maximum time to spend in milliseconds
	 */
	void ProcessGenerationQueue(float TimeSliceMS);

	/**
	 * Process the meshing queue (time-sliced; fills the mesh-launch sub-timers).
	 *
	 * @param TimeSliceMS Maximum time to spend in milliseconds
	 * @param Timing Receives the launch-path cost breakdown for this tick
	 */
	void ProcessMeshingQueue(float TimeSliceMS, FVoxelTimingStats& Timing);

	/**
	 * Process the unload queue.
	 *
	 * @param MaxChunks Maximum chunks to unload this frame
	 */
	void ProcessUnloadQueue(int32 MaxChunks);

	/**
	 * Evaluate LOD level changes for loaded chunks and queue remeshes.
	 * Separated from morph updates so it can run when queues drain,
	 * not just when the viewer moves.
	 */
	void EvaluateLODLevelChanges(const FLODQueryContext& Context);

	/**
	 * Update LOD morph factors for loaded chunks.
	 * Only morph factor interpolation — no level change detection.
	 */
	void UpdateLODMorphFactors(const FLODQueryContext& Context);

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

	/**
	 * Budgeted per-tick far-chunk voxel-data compression sweep. Compacts idle, settled far chunks
	 * (see voxel.FarCompression.* cvars); access transparently re-materializes via EnsureResident().
	 * PR B: uniform-chunk collapse only.
	 */
	void ProcessFarCompressionSweep();

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
	 * Debounced dispatch of coalesced neighbor-remesh requests (see NeighborRemeshRequestFrame):
	 * enqueue a chunk for a single neighbor-remesh once its neighborhood has settled, instead of
	 * re-meshing it once per neighbor arrival. Collapses the fan-out churn during traversal.
	 */
	void ProcessPendingNeighborRemeshes();

	/**
	 * Snapshot the 26 neighbours' boundary-relevant state (content version, rendered LOD, data
	 * availability) at the instant a chunk is dispatched to mesh. Stored in InFlightMeshDeps so the
	 * completion handler can detect a neighbour that changed WHILE this chunk was in flight — the
	 * race that bakes a stale/Air boundary (a persistent seam) with no other trigger to fix it.
	 */
	void CaptureMeshDeps(const FIntVector& ChunkCoord);

	/**
	 * Compare a just-completed chunk's neighbours against the snapshot taken at mesh launch
	 * (CaptureMeshDeps). Returns true if any neighbour's voxel content changed, its rendered LOD
	 * changed, or its data became available since launch — i.e. the boundary we just baked is stale
	 * and the chunk must re-mesh. Removes the snapshot entry. Safe to call for a chunk with no entry
	 * (returns false).
	 */
	bool RevalidateMeshDeps(const FIntVector& ChunkCoord);

	/** Discard a chunk's in-flight mesh-dependency snapshot (on unload / abandoned mesh). */
	void ClearMeshDeps(const FIntVector& ChunkCoord);

	/**
	 * Propagate water flags from loaded neighbor chunks into this chunk.
	 *
	 * Checks 6 face-neighbor chunks for water-flagged voxels at their shared
	 * boundary. Any dry air voxels on this chunk's boundary that are below water
	 * level and adjacent to a neighbor's water voxel become seeds for an
	 * intra-chunk BFS flood fill.
	 *
	 * @param ChunkCoord The chunk to propagate water into
	 * @return True if any new water flags were set (caller should regenerate water mesh)
	 */
	bool PropagateWaterFromNeighbors(const FIntVector& ChunkCoord);

	// ==================== 2D Water Tile System ====================

	/**
	 * Scan a chunk's voxel data and update its water tile contribution.
	 * Called after meshing completes for a 3D chunk.
	 *
	 * @param ChunkCoord 3D chunk coordinate
	 */
	void UpdateWaterTileContribution(const FIntVector& ChunkCoord);

	/**
	 * Remove a chunk's water tile contribution.
	 * Called when a 3D chunk is unloaded.
	 *
	 * @param ChunkCoord 3D chunk coordinate
	 */
	void RemoveWaterTileContribution(const FIntVector& ChunkCoord);

	/**
	 * Process dirty water tiles (time-sliced, called from TickComponent).
	 * Combines partial masks, generates water mesh, and sends to renderer.
	 *
	 * @param MaxTilesPerFrame Maximum number of water tiles to process this frame
	 */
	void ProcessDirtyWaterTiles(int32 MaxTilesPerFrame);

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

	// ==================== Queue Management ====================

	/**
	 * Add a chunk to the generation queue with sorted insertion.
	 * Uses O(1) set lookup for duplicate detection, O(log n) binary search for insertion.
	 *
	 * @param Request The chunk request to add
	 * @return True if added, false if already in queue
	 */
	bool AddToGenerationQueue(const FChunkLODRequest& Request);

	/**
	 * Add a chunk to the meshing queue with sorted insertion.
	 * Uses O(1) set lookup for duplicate detection, O(log n) binary search for insertion.
	 *
	 * @param Request The chunk request to add
	 * @return True if added, false if already in queue
	 */
	bool AddToMeshingQueue(const FChunkLODRequest& Request, EVoxelRemeshReason Reason = EVoxelRemeshReason::Other);

	/**
	 * P2-A: decide whether a chunk should defer meshing because a face neighbor's
	 * voxel data is not yet resident.
	 *
	 * Meshing a boundary against a non-resident neighbor makes GetVoxelAt clamp the
	 * missing plane to the chunk's own edge voxel, producing the LOD-seam tear that
	 * is never refreshed once the neighbor arrives (see LOD_SEAM_INVESTIGATION.md).
	 *
	 * @param ChunkCoord The chunk about to mesh
	 * @param bOutClampUnavoidable Set true when a neighbor lacks data but it is NOT
	 *        coming (Loaded-but-freed / unloading) — caller should mesh anyway (to
	 *        avoid a permanent stall) but may warn.
	 * @return True if meshing should be deferred (a neighbor's data is still in the
	 *         generation pipeline and will arrive).
	 */
	bool ShouldDeferMeshForNeighbors(const FIntVector& ChunkCoord, bool& bOutClampUnavoidable) const;

	/**
	 * Add a chunk to the unload queue.
	 * Uses O(1) set lookup for duplicate detection.
	 *
	 * @param ChunkCoord The chunk coordinate to add
	 * @return True if added, false if already in queue
	 */
	bool AddToUnloadQueue(const FIntVector& ChunkCoord);

	/** True if ChunkCoord is beyond the LOD strategy's unload horizon from the current viewer
	 *  position. Used to stale-cull chunks from the mesh queue before wasting a mesh job on them. */
	bool IsChunkBeyondUnloadDistance(const FIntVector& ChunkCoord) const;

	/**
	 * Remove a chunk from the generation queue.
	 *
	 * @param ChunkCoord The chunk coordinate to remove
	 */
	void RemoveFromGenerationQueue(const FIntVector& ChunkCoord);

	/**
	 * Remove a chunk from the meshing queue.
	 *
	 * @param ChunkCoord The chunk coordinate to remove
	 */
	void RemoveFromMeshingQueue(const FIntVector& ChunkCoord);

	/**
	 * Remove a chunk from the unload queue.
	 *
	 * @param ChunkCoord The chunk coordinate to remove
	 */
	void RemoveFromUnloadQueue(const FIntVector& ChunkCoord);

	/**
	 * Re-prioritize generation and meshing queues based on current viewer position.
	 * Called on viewer chunk change to ensure closest chunks are processed first.
	 * Also updates LOD levels for queued items so they mesh at the correct LOD
	 * without needing a post-load LOD transition. Evicts generation work beyond ViewDistance.
	 *
	 * @param Context Current LOD query context (viewer position, forward, etc.)
	 */
	void ReprioritizeQueues(const FLODQueryContext& Context);

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

	/**
	 * Per in-flight-mesh snapshot of the 26 neighbours' boundary state, taken when a chunk is
	 * dispatched to mesh (CaptureMeshDeps) and consumed at completion (RevalidateMeshDeps) to detect
	 * a neighbour that changed mid-flight and left a stale boundary. Bounded by the async-mesh
	 * in-flight cap (a handful of entries); not a UPROPERTY (transient, non-reflected).
	 */
	TMap<FIntVector, TArray<FMeshBoundaryDep>> InFlightMeshDeps;

	/**
	 * Single-owner seam registry (seam-ownership refactor P0 — SEAM_OWNERSHIP_ARCHITECTURE.md §2).
	 * Tracks which physical chunk boundary (face/edge/corner) is owned by which chunk, dirty-tracks
	 * seams off the same content-version/rendered-LOD change points the #42 revalidation uses, and
	 * schedules seam jobs. In P0 the seam job is a stub that produces NO geometry, so this runs
	 * alongside the existing per-chunk meshing with no visual/behavioural change. Gated by
	 * voxel.Seam.Registry. Non-UPROPERTY (plain C++ helper, not reflected/serialized).
	 */
	TUniquePtr<FVoxelSeamRegistry> SeamRegistry;

	/** Push this-tick seam enable/debug flags from cvars, then run the seam scheduler + stub processor. */
	void TickSeamScheduler();

	// ==================== Seam meshing runtime pipeline (seam-ownership P1) ====================
	// When voxel.Seam.Meshing is on AND the active mesher is the CPU DC mesher, chunks mesh
	// interior-only (EVoxelMeshCellDomain::Interior — zero neighbor deps, no slices/defer/weld)
	// and drained seam-registry jobs execute the single-owner face-seam mesher async, submitting
	// per-owner seam buckets to the renderer. Off by default: the legacy path is untouched.

	/** A finished async face-seam mesh awaiting game-thread submit to the renderer. */
	struct FCompletedSeamMesh
	{
		FVoxelSeamKey Key;
		int32 LODLevel = 0;
		FChunkMeshData MeshData;
		bool bSuccess = false;
	};

	/** Thread-safe queue for completed async seam meshes. */
	TQueue<FCompletedSeamMesh, EQueueMode::Mpsc> CompletedSeamMeshQueue;

	/** Seam jobs currently meshing on the worker pool (bounds pipeline depth; prevents dupes). */
	TSet<FVoxelSeamKey> SeamJobsInFlight;

	/**
	 * Version-keyed shared voxel snapshots for seam jobs. A chunk participates in up to 26 seams;
	 * without sharing, every dispatch copied 128KB per participant on the game thread — the
	 * traverse-phase cost found by the flip-qualification bench. One immutable snapshot is built
	 * per (chunk, ContentVersion) and shared by every job; workers keep the data alive via their
	 * TSharedPtr refs, so eviction here is always safe. Entries are dropped on unload, on
	 * deactivation, and by an age sweep in TickSeamScheduler (the cache is only useful while a
	 * chunk's seam wave is dispatching).
	 */
	struct FSeamVoxelSnapshot
	{
		uint32 ContentVersion = 0;
		uint64 LastUsedTick = 0;
		TSharedPtr<const TArray<FVoxelData>> Data;
	};
	TMap<FIntVector, FSeamVoxelSnapshot> SeamSnapshotCache;

	/** Monotonic TickSeamScheduler counter (drives the snapshot age sweep). */
	uint64 SeamTickCounter = 0;

	/** Get (building at most once per content version) the shared edit-merged snapshot of a chunk. */
	TSharedPtr<const TArray<FVoxelData>> GetSeamVoxelSnapshot(const FIntVector& Coord, FVoxelChunkState& State);

	/** This-tick resolved state of the seam-meshing pipeline (cvar + registry + CPU-DC mesher). */
	bool bSeamMeshingActive = false;

	/** Previous-tick value, to detect on/off transitions (deactivation clears seam buckets). */
	bool bSeamMeshingWasActive = false;

	/**
	 * Validate + dispatch one drained seam job: P1 executes same-LOD FACE seams only (edge/corner
	 * and mixed-LOD jobs are dropped — P2). Builds the FVoxelFaceSeamRequest from both resident
	 * descriptors (edit-merged) and hands it to a thread-pool worker running the DC face-seam
	 * mesher; the result lands in CompletedSeamMeshQueue.
	 */
	void DispatchSeamJob(const FVoxelSeamJob& Job);

	/** Drain completed seam meshes (game thread) and submit them to the renderer's seam buckets. */
	void ProcessCompletedSeamMeshes();

	/** Set of loaded chunk coordinates (for fast lookup) */
	TSet<FIntVector> LoadedChunkCoords;

	// ==================== Processing Queues ====================

	/** Chunks waiting to be generated (sorted ascending — highest priority at back for O(1) pop) */
	TArray<FChunkLODRequest> GenerationQueue;

	/** Set of chunk coords in GenerationQueue for O(1) duplicate detection */
	TSet<FIntVector> GenerationQueueSet;

	/** Chunks waiting to be meshed (sorted ascending — highest priority at back for O(1) pop) */
	TArray<FChunkLODRequest> MeshingQueue;

	/** Set of chunk coords in MeshingQueue for O(1) duplicate detection */
	TSet<FIntVector> MeshingQueueSet;

	/** Chunks waiting to be unloaded */
	TArray<FIntVector> UnloadQueue;

	/** Set of chunk coords in UnloadQueue for O(1) duplicate detection */
	TSet<FIntVector> UnloadQueueSet;

	// ==================== Runtime State ====================

	/** Current frame number */
	int64 CurrentFrame = 0;

	/** Cached viewer position */
	FVector CachedViewerPosition = FVector::ZeroVector;

	/** Cached viewer forward direction */
	FVector CachedViewerForward = FVector::ForwardVector;

	// ==================== Streaming Decision Caching ====================

	/**
	 * Cached viewer chunk coordinate - only recompute visible chunks when viewer moves to a new chunk.
	 * This dramatically reduces LOD strategy calls from every frame to only on chunk boundary crossings.
	 */
	FIntVector CachedViewerChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);

	/**
	 * Last position where streaming decisions were updated.
	 * Used to skip redundant streaming updates when viewer hasn't moved significantly.
	 */
	FVector LastStreamingUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);

	/**
	 * Last position where LOD transitions were updated.
	 * LOD updates are more sensitive to position changes than streaming decisions.
	 */
	FVector LastLODUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);

	/**
	 * Flag to force streaming update on next tick.
	 * Set by ForceStreamingUpdate(), cleared after use.
	 */
	bool bForceStreamingUpdate = false;

	/**
	 * Flag indicating that a LOD level sweep is pending.
	 * Set when all queues drain to zero, cleared after a successful sweep
	 * discovers no new work. Ensures stationary viewers eventually get correct LOD.
	 */
	bool bPendingLODSweep = false;

	/**
	 * Threshold for LOD morph factor update position delta (squared).
	 * Morph factors should update when viewer moves ~100 units.
	 */
	static constexpr float LODUpdateThresholdSq = 100.0f * 100.0f;  // 100 units

	// ==================== Statistics ====================

	/** Total chunks generated this session */
	int64 TotalChunksGenerated = 0;

	/** Total chunks meshed this session */
	int64 TotalChunksMeshed = 0;

	/** Total chunks unloaded this session */
	int64 TotalChunksUnloaded = 0;

	// ==================== Generation Components ====================

	/** Voxel data generator — CPU (FVoxelCPUNoiseGenerator) or GPU (FVoxelGPUNoiseGenerator) per config. */
	TUniquePtr<IVoxelNoiseGenerator> NoiseGenerator;

	/** Non-owning alias to NoiseGenerator when GPU generation is active (for the poll-based readback API). */
	FVoxelGPUNoiseGenerator* GPUGeneratorPtr = nullptr;

	/** True when bUseGPUGeneration resolved on (config flag && !-VoxelForceCPU && voxel.GPUGeneration.Enable). */
	bool bUseGPUGenerationActive = false;

	/** World mode for terrain generation (Infinite Plane) */
	TUniquePtr<IVoxelWorldMode> WorldMode;

	/** Static terrain conditioning zones (gen-time flattening under POIs/claims; Phase 6c). */
	TArray<FVoxelConditioningZone> ConditioningZones;

	/** Optional game-supplied dynamic conditioner queried per chunk (not owned). */
	IVoxelTerrainConditioner* TerrainConditioner = nullptr;

	/** Collect conditioning zones overlapping a chunk's XY footprint into OutZones (game thread). */
	void GatherConditioningZonesForChunk(const FIntVector& ChunkCoord, TArray<FVoxelConditioningZone>& OutZones) const;

	/** Append conditioning zones (static + dynamic) whose influence overlaps a world-space XY region. */
	void GatherConditioningZonesForRegion(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const;

	/** Append conditioning zones (static + dynamic) whose influence covers a single world-space XY point. */
	void GatherConditioningZonesForPoint(double WorldX, double WorldY, TArray<FVoxelConditioningZone>& OutZones) const;

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

	/** Note: MaxPendingMeshes is now in VoxelWorldConfiguration; runtime value in EffectiveMaxPendingMeshes */

	// ==================== Async Noise Generation ====================

	/** Result of an async noise generation task */
	struct FAsyncGenerationResult
	{
		FIntVector ChunkCoord;
		TArray<FVoxelData> VoxelData;
		bool bSuccess = false;
	};

	/** Thread-safe queue for completed async generation results */
	TQueue<FAsyncGenerationResult, EQueueMode::Mpsc> CompletedGenerationQueue;

	/** Set of chunks currently being generated asynchronously */
	TSet<FIntVector> AsyncGenerationInProgress;

	/** Process completed async generation tasks (called from game thread; fills the gen-apply sub-timers) */
	void ProcessCompletedAsyncGenerations(FVoxelTimingStats& Timing);

	/** QueueNeighborsForRemesh time accumulated across this tick (attribution; reset each tick) */
	double NeighborRemeshSecondsThisTick = 0.0;

	/** Render-submit sub-cost accumulators (attribution; reset each tick, filled in OnChunkMeshingComplete) */
	double SubmitRendererSecondsThisTick = 0.0;
	double SubmitScatterSecondsThisTick = 0.0;
	double SubmitWaterSecondsThisTick = 0.0;
	int32 SubmitsThisTick = 0;

	/** Launch async noise generation for a chunk */
	void LaunchAsyncGeneration(const FChunkLODRequest& Request, FVoxelNoiseGenerationRequest GenRequest);

	// ==================== GPU generation (poll-based async readback) ====================

	/** A chunk whose GPU density generation was dispatched and is awaiting async readback. */
	struct FPendingGPUGeneration
	{
		FVoxelGenerationHandle Handle;
		FVoxelNoiseGenerationRequest GenRequest;  // retained for CPU post-passes (tree injection)
	};

	/** GPU generations in flight, keyed by chunk coord; polled each frame in ProcessPendingGPUReadbacks. */
	TMap<FIntVector, FPendingGPUGeneration> PendingGPUReadbacks;

	/** Poll in-flight GPU readbacks; ready results are handed to a thread-pool worker for CPU post-passes. */
	void ProcessPendingGPUReadbacks();

	/**
	 * Finish a ready GPU readback and feed CompletedGenerationQueue. The water/underground
	 * post-passes already ran on the GPU inside the generation graph (AddVoxelPostPassDispatches),
	 * so this only runs cubic-mode voxel-tree injection — on a thread-pool worker, never the game
	 * thread (per-chunk volume work there caused multi-chunk frame spikes; readbacks arrive in
	 * batches). Without tree injection the result is enqueued directly.
	 */
	void LaunchPostReadbackProcessing(const FIntVector& ChunkCoord, FVoxelNoiseGenerationRequest GenRequest, TArray<FVoxelData> VoxelData);

	// ==================== Async Mesh Generation ====================

	/** Result of an async mesh generation task */
	struct FAsyncMeshResult
	{
		FIntVector ChunkCoord;
		int32 LODLevel;
		FChunkMeshData MeshData;
		bool bSuccess = false;
	};

	/** Thread-safe queue for completed async mesh results */
	TQueue<FAsyncMeshResult, EQueueMode::Mpsc> CompletedMeshQueue;

	/** Set of chunks currently being meshed asynchronously */
	TSet<FIntVector> AsyncMeshingInProgress;

	/** Note: MaxAsyncMeshTasks is now in VoxelWorldConfiguration; runtime value in EffectiveMaxAsyncMeshTasks */

	/** Process completed async mesh tasks (called from game thread) */
	void ProcessCompletedAsyncMeshes();

	/** Launch async mesh generation for a chunk */
	void LaunchAsyncMeshGeneration(const FChunkLODRequest& Request, FVoxelMeshingRequest MeshRequest);

	// ==================== Edit & Collision Systems ====================

	/** Edit manager for terrain modifications */
	UPROPERTY()
	TObjectPtr<UVoxelEditManager> EditManager;

	/** Collision manager for physics */
	UPROPERTY()
	TObjectPtr<UVoxelCollisionManager> CollisionManager;

	/** Scatter manager for vegetation placement */
	UPROPERTY()
	TObjectPtr<UVoxelScatterManager> ScatterManager;

	/** Water propagation system for flooding caves on edit */
	UPROPERTY()
	TObjectPtr<UVoxelWaterPropagation> WaterPropagation;

	// ==================== 2D Water Tile State ====================

	/** Tracks water state for a single XY chunk column (2D tile). */
	struct FWaterTileState
	{
		/** Per-Z-level partial column masks contributed by loaded 3D chunks.
		 *  Key = chunk Z coordinate, Value = ChunkSize² bool mask. */
		TMap<int32, TArray<bool>> PartialMasks;

		/** Whether the combined mask needs regeneration. */
		bool bDirty = true;
	};

	/** 2D water tile tracking: XY chunk column → water state */
	TMap<FIntVector2, FWaterTileState> WaterTiles;

	/** Queue of water tiles needing mesh regeneration */
	TArray<FIntVector2> DirtyWaterTileQueue;

	/** Set for O(1) duplicate detection in DirtyWaterTileQueue */
	TSet<FIntVector2> DirtyWaterTileSet;

	// ==================== Adaptive Throttling State ====================

	/** Effective throttle values (adjusted by adaptive throttling) */
	int32 EffectiveMaxAsyncGenerationTasks = 2;
	int32 EffectiveMaxAsyncMeshTasks = 4;
	int32 EffectiveMaxLODRemeshPerFrame = 4;
	int32 EffectiveMaxPendingMeshes = 4;

	/** Benchmark scheduler overrides parsed from the command line at Initialize (A/B tuning):
	 *  -VoxelMaxAsyncGen=N / -VoxelMaxAsyncMesh=N / -VoxelMaxLODRemesh=N / -VoxelMaxPending=N
	 *  substitute for the configuration values; -VoxelPinScheduler disables adaptive throttling
	 *  so the chosen limits hold for the whole run. -1 == use the configuration value. */
	int32 SchedOverrideAsyncGen = -1;
	int32 SchedOverrideAsyncMesh = -1;
	int32 SchedOverrideLODRemesh = -1;
	int32 SchedOverridePending = -1;
	bool bSchedPinned = false;

	/** Deep neighbour-data depth override for per-job cost (command line, parsed at Initialize).
	 *  Default = stride+1 (geometry-only: watertight, one-sided boundary normals, the cheaper job).
	 *  -VoxelDeepFull => 2*stride (adds central-difference boundary-normal reach, the old default);
	 *  -VoxelDeepOff => 1 (no deep data). Applied in ExtractNeighborEdgeSlices. */
	bool bDeepDepthFull = false;
	bool bDeepDepthOff = false;

	/** Stale-cull: skip meshing chunks the viewer has already moved past (beyond the unload
	 *  horizon) instead of meshing them and unloading on arrival. Default on; -VoxelNoStaleCull disables. */
	bool bStaleCull = true;

	/** Latest viewer world position, cached each tick for the stale-cull distance test. */
	FVector CurrentViewerPosition = FVector::ZeroVector;

	/**
	 * True when the chunk's center is within voxel.Stream.NearCorrectionDistance of the viewer.
	 * Near chunks take the boundary-correction fast path: short coalesce debounce and meshing-queue
	 * priority above the streaming wave, so a stale seam next to the player resolves in frames.
	 */
	bool IsChunkNearViewer(const FIntVector& ChunkCoord) const;

	/** Previous-tick viewer position for horizontal-speed estimation (FLT_MAX until first tick). */
	FVector LastViewerPosForSpeed = FVector(FLT_MAX);

	/** Smoothed viewer horizontal speed (uu/s); drives the speed-adaptive load budget. */
	float ViewerHorizSpeed = 0.0f;

	/** Smoothed frame time for stable throttle decisions (EMA) */
	float SmoothedFrameTimeMs = 16.67f;

	/** Whether collision/scatter updates are deferred due to heavy gen queue */
	bool bSubsystemsDeferred = false;

	// ==================== Per-Frame Timing ====================

	/** Timing stats from last frame */
	FVoxelTimingStats LastTimingStats;

	// ==================== Streaming Benchmark ====================

	/** When active, BuildQueryContext uses BenchmarkViewPosition instead of the camera/viewport,
	 *  giving a deterministic streaming origin for repeatable benchmarks (headless + PIE). */
	bool bBenchmarkViewActive = false;
	FVector BenchmarkViewPosition = FVector::ZeroVector;

	/** Re-mesh churn: count of chunks re-queued for meshing after they already had a mesh. */
	int64 BenchRemeshCount = 0;

	/** Re-mesh thrash attributed by source (see EVoxelRemeshReason). */
	int64 BenchRemeshByReason[static_cast<int32>(EVoxelRemeshReason::Count)] = {};

	/** Chunks meshed at least once during the active benchmark (to detect re-mesh churn). */
	TSet<FIntVector> BenchEverMeshed;

	/** Active benchmark run, ticked from TickComponent; reset when it finishes. */
	TUniquePtr<FVoxelStreamingBenchmark> ActiveBenchmark;

	/** Unload-lag: per-chunk enqueue time + accumulated dwell (enqueue -> actual unload) stats. */
	TMap<FIntVector, double> UnloadEnqueueTimeSeconds;
	double BenchUnloadLagSumMs = 0.0;
	double BenchUnloadLagMaxMs = 0.0;
	int64 BenchUnloadLagCount = 0;

	/** Unload distance: viewer-to-chunk distance at the moment of actual unload (uu) — how far
	 *  past the active radius chunks survive (the "lazy deletion" decision, distance form). */
	double BenchUnloadDistSumUU = 0.0;
	double BenchUnloadDistMaxUU = 0.0;
};
