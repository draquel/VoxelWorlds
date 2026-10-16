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
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelStreamingBenchmark.h"
#include "VoxelSeamRegistry.h"
#include "VoxelSeamTransitionHolds.h"
#include "VoxelLODEvalSchedule.h"
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
class UBoxComponent;

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

/** Why a chunk was (re)queued for meshing — used to attribute re-mesh thrash in the benchmark. */
enum class EVoxelRemeshReason : uint8
{
	Other = 0,        // first-time mesh or uncategorised path
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
	 * A chunk's edit-merged voxels as a shared, immutable snapshot (ChunkSize^3, chunk-local index
	 * X + Y*CS + Z*CS*CS). Built at most once per chunk content version and shared with the seam pipeline and the
	 * async collision cooks, so readers outside streaming (the map's edit overlay, QA R22) get edit-aware data
	 * without another full copy. Workers may keep the snapshot alive for as long as they need it. Game thread only.
	 * Not a UFUNCTION: TSharedPtr cannot be exposed to Blueprint.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return The snapshot, or null when the chunk has no voxel data
	 */
	TSharedPtr<const TArray<FVoxelData>> GetEditMergedSnapshot(const FIntVector& ChunkCoord);

	/**
	 * Whether a chunk has voxel data (resident or compressed), i.e. whether GetEditMergedSnapshot would return one.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if the chunk is tracked and its voxel data is available
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	bool HasChunkVoxelData(const FIntVector& ChunkCoord) const;

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

	// ==================== World Bounds and Local Frame (WM-A2) ====================
	// The configured world mode's bounds and frame (IVoxelWorldMode), for Blueprint and for game plugins, which reach
	// them through the world actor's chunk manager like the surface queries above. Before Initialize (no world mode)
	// they describe an unbounded heightmap world: inside everywhere, +Z up, altitude = Z, surface = (0, X, Y).

	/**
	 * Whether the vertical line at (X, Y) passes through the world (an island's R + falloff footprint; always on a
	 * plane). Use it to keep placement, exploration, spawning and navigation inside the world.
	 *
	 * @param WorldX World X
	 * @param WorldY World Y
	 * @return True when (X, Y) is inside the world's footprint
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	bool IsInsideWorld(double WorldX, double WorldY) const;

	/**
	 * Where the world ends: unbounded (plane), a circle or rectangle footprint (island), a shell (planet).
	 *
	 * @return The world's bounds
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	FVoxelWorldBounds GetWorldBounds() const;

	/**
	 * Local up at a world position (+Z in heightmap modes, radial on a planet).
	 *
	 * @param WorldPos World position
	 * @return Unit up vector
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	FVector GetUpVectorAt(const FVector& WorldPos) const;

	/**
	 * Signed height above the datum (SeaLevel in heightmap modes, the planet radius on a planet).
	 *
	 * @param WorldPos World position
	 * @return Altitude along the local up
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	float GetAltitudeAt(const FVector& WorldPos) const;

	/**
	 * A world position's surface coordinate, the key for 2D systems: (0, X, Y) in heightmap modes, a cube face and
	 * (U, V) on a planet.
	 *
	 * @param WorldPos World position
	 * @return Surface coordinate
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	FVoxelSurfaceCoord WorldToSurface(const FVector& WorldPos) const;

	/**
	 * The world position at a surface coordinate and an altitude above the datum (inverse of WorldToSurface).
	 *
	 * @param Coord Surface coordinate
	 * @param Altitude Height above the datum
	 * @return World position
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	FVector SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const;

	/**
	 * What generated this world, for saves (WM-A4): world mode, shape params, voxel grid and generation version.
	 * A save whose fingerprint differs is not loaded into this world.
	 *
	 * @return The fingerprint; an unrecorded one (GenerationVersion 0) before Initialize
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	FVoxelWorldFingerprint GetWorldFingerprint() const;

	/**
	 * Distance in X and Y from a point to the world's edge (FVoxelWorldBounds::GetDistanceToEdgeXY): what a soft
	 * push-back or a HUD hint near an island's R + falloff reads (WM-I5).
	 *
	 * @param WorldX World X
	 * @param WorldY World Y
	 * @return Positive inside, negative outside; a very large value for an unbounded world
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	double GetDistanceToWorldEdge(double WorldX, double WorldY) const;

	/**
	 * Number of invisible walls blocking the world's edge (WM-I5): one per chord of a circular island, four for a
	 * rectangle, none when the world is unbounded or UVoxelWorldConfiguration::bWorldBoundary is off.
	 *
	 * @return Wall count
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	int32 GetWorldBoundaryWallCount() const { return WorldBoundaryWalls.Num(); }

	/** Turn the edge walls' collision on or off (voxel.World.Boundary). The walls stay; only their collision changes. */
	void SetWorldBoundaryCollisionEnabled(bool bEnabled);

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
	 * Broadcasts OnTerrainConditioningChanged.
	 */
	void SetTerrainConditioner(IVoxelTerrainConditioner* InConditioner)
	{
		TerrainConditioner = InConditioner;
		OnTerrainConditioningChanged.Broadcast();
	}

	/**
	 * Append the conditioning zones (static + the dynamic conditioner's) whose influence overlaps a world XY
	 * region — what generation layers onto the base height there (apply with FVoxelTerrainConditioning::
	 * ApplyToHeight). Game thread. Used by height consumers outside chunk generation, e.g. map tiles.
	 * @param Region   World-space XY region
	 * @param OutZones Appended with the overlapping zones
	 */
	void GatherConditioningZones(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const
	{
		GatherConditioningZonesForRegion(Region, OutZones);
	}

	/**
	 * Fired when the terrain conditioning changes (a conditioner registered or zones added / cleared). Chunks
	 * generated earlier keep their terrain; caches of analytic heights (map tiles) should rebuild.
	 */
	FSimpleMulticastDelegate OnTerrainConditioningChanged;

	// ==================== Collision Mesh Generation ====================

	/**
	 * Prepare a meshing request for a collision cook (game thread only). Collision is meshed whole-chunk
	 * by the main mesher on a worker (VoxelCollisionManager), so nothing heavy happens here (P4a: the
	 * full-volume copy and the strided 26-neighbour slice extraction were the dominant per-cook
	 * game-thread costs): the chunk's edit-merged shared snapshot and its neighbours' are handed out,
	 * OutMeshRequest.VoxelData is left EMPTY, and the worker materializes it and runs
	 * VoxelNeighborSlices::Extract over the neighbour snapshots.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param LODLevel LOD level for collision mesh
	 * @param OutMeshRequest Output meshing request (metadata; voxels come from OutSharedVoxels)
	 * @param OutSharedVoxels Receives the chunk's shared voxel snapshot
	 * @param OutNeighborSnapshots Receives the snapshots of the 26 neighbours that have data
	 * @return True if request was prepared successfully
	 */
	bool PrepareCollisionMeshRequest(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		FVoxelMeshingRequest& OutMeshRequest,
		TSharedPtr<const TArray<FVoxelData>>& OutSharedVoxels,
		TMap<FIntVector, TSharedPtr<const TArray<FVoxelData>>>& OutNeighborSnapshots);

	/** Whether -VoxelDeepOff / -VoxelDeepFull were set (worker-side slice extraction needs them). */
	bool IsDeepSliceOff() const { return bDeepDepthOff; }
	bool IsDeepSliceFull() const { return bDeepDepthFull; }

	/**
	 * Get raw pointer to the mesher (for async dispatch).
	 * The mesher's GenerateMeshCPU is stateless and thread-safe.
	 */
	IVoxelMesher* GetMesherPtr() const { return Mesher.Get(); }

	/** True when terrain generation runs on the GPU (bUseGPUGeneration, and not forced to the CPU). */
	bool IsUsingGPUGeneration() const { return GPUGeneratorPtr != nullptr; }

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
		// Collision sub-phases (P4a attribution): cook-launch preparation (request build +
		// neighbor slices) vs completed-cook application (BodySetup + component recreation).
		float CollisionPrepMs = 0.0f;
		float CollisionApplyMs = 0.0f;
		float ScatterMs = 0.0f;
		float LODMs = 0.0f;
		float LODUpdateMs = 0.0f;   // LOD strategy Update (balanced-LOD cache rebuild), before the streaming section
		float StreamingMs = 0.0f;
		float TotalMs = 0.0f;

		// Generation sub-phases (GenerationMs = launch + poll + apply). Added to attribute the
		// residual game-thread generation cost after the post-passes moved to the GPU.
		float GenLaunchMs = 0.0f;   // ProcessGenerationQueue: build requests + dispatch
		float GenPollMs = 0.0f;     // ProcessPendingGPUReadbacks: poll fences + hand off ready data
		float GenApplyMs = 0.0f;    // ProcessCompletedAsyncGenerations total
		float GenStoreMs = 0.0f;    // apply: move result into chunk state (frees any old data)
		float GenNotifyMs = 0.0f;   // apply: OnChunkGenerationComplete (queues + neighbors + delegates)
		int32 GenApplyCount = 0;    // completions applied this tick

		// Meshing sub-phases (MeshingMs = tick + launch + apply)
		float MeshTickMs = 0.0f;    // Mesher->Tick: GPU meshers poll readbacks here
		float MeshLaunchMs = 0.0f;  // ProcessMeshingQueue: snapshot + slices + dispatch
		float MeshApplyMs = 0.0f;   // ProcessCompletedAsyncMeshes

		// Launch-path breakdown (subsets of MeshLaunchMs)
		float MeshSnapshotMs = 0.0f;  // voxel-data snapshot copy + edit merge
		float MeshDispatchMs = 0.0f;  // LaunchAsyncMeshGeneration (worker handoff for DC/MC GPU)
		int32 MeshLaunchCount = 0;    // mesh jobs launched this tick

		// Seam pipeline (seam-ownership P1+): TickSeamScheduler total — registry scheduling,
		// job drain/dispatch (snapshot lookups), and completed-seam submits to the renderer.
		float SeamMs = 0.0f;

		// Far-chunk compression sweep (ProcessFarCompressionSweep total): the candidate scan, free
		// re-applies, copies for async encodes and installs of finished ones. With
		// voxel.FarCompression.Async 0 it also holds the encodes themselves.
		float FarCompressMs = 0.0f;
		int32 FarCompressDispatchCount = 0; // fresh encodes started (async) or run (sync) this tick
		int32 FarCompressApplyCount = 0;    // finished async encodes installed this tick

		// Render-submit breakdown (RenderSubmitMs = mesh drain + unload + water tiles; flush is separate)
		float RenderMeshMs = 0.0f;       // OnChunkMeshingComplete drain loop total
		float RenderSubRendererMs = 0.0f; // subset: renderer UpdateChunkMeshFromCPU (vertex convert + handoff)
		float RenderSubScatterMs = 0.0f; // subset: ScatterManager OnChunkMeshDataReady
		float RenderSubWaterMs = 0.0f;   // subset: PropagateWaterFromNeighbors + UpdateWaterTileContribution
		float RenderUnloadMs = 0.0f;     // ProcessUnloadQueue
		float RenderWaterTileMs = 0.0f;  // ProcessDirtyWaterTiles (water tile mesh rebuilds)
		float RenderFlushMs = 0.0f;      // MeshRenderer FlushPendingOperations (end of tick)
		int32 RenderSubmitCount = 0;     // meshes submitted to the renderer this tick

		// LOD level evaluation (KI12): whether EvaluateLODLevelChanges ran this tick and what it found.
		int32 LODEvalCandidates = -1;    // chunks whose committed LOD differs from the strategy; -1 = no evaluation
		int32 LODEvalQueued = 0;         // LOD re-meshes it queued (at most the per-frame cap)
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

	/** Get count of async mesh tasks in flight */
	int32 GetAsyncMeshingInProgressCount() const { return AsyncMeshingInProgress.Num(); }

	/** Get count of async seam-mesh jobs in flight */
	int32 GetSeamJobsInFlightCount() const { return SeamJobsInFlight.Num(); }

	/** Total chunks meshed this session (monotonic — drives HUD throughput display) */
	int64 GetTotalChunksMeshed() const { return TotalChunksMeshed; }

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

	/** Seam meshes applied (submitted to the renderer) since the benchmark started. */
	int64 GetBenchSeamJobsCompleted() const { return BenchSeamJobsCompleted; }

	/**
	 * Far-compression encodes this session: fresh encodes started (async or sync), async results
	 * installed into their chunk, and async results discarded because the chunk's array changed or the
	 * chunk went away while the worker ran.
	 */
	void GetBenchFarCompressionCounts(int64& OutEncodes, int64& OutInstalled, int64& OutStale) const
	{
		OutEncodes = FarCompressEncodes;
		OutInstalled = FarCompressInstalled;
		OutStale = FarCompressStale;
	}

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

	/**
	 * LOD backlog for benchmark sampling (KI12): Loaded chunks (none of their work queued or in flight)
	 * whose committed LOD differs from the LOD strategy's current answer, i.e. LOD changes the manager
	 * has not queued yet. Walks every loaded chunk; call from the benchmark only.
	 * @param OutStale     All such chunks.
	 * @param OutStaleNear The subset the strategy puts at LOD 0 (shown coarser than they should be, near the viewer).
	 */
	void GetBenchLODBacklog(int32& OutStale, int32& OutStaleNear) const;

	/**
	 * Loaded chunks per chunk layer (Z), for benchmark sampling (§13.11). Walks every loaded chunk.
	 * @param OutByLayer Receives layer -> count (reset first).
	 */
	void GetBenchLoadedByLayer(TMap<int32, int32>& OutByLayer) const;

	/** Start a deterministic streaming benchmark run (drives the streaming origin, samples each
	 *  frame from TickComponent, and writes a CSV + JSON report on completion). */
	void StartBenchmark(const FVoxelBenchConfig& InConfig);

	/** Reset benchmark counters (call at the start of a benchmark run). */
	void ResetBenchCounters()
	{
		BenchRemeshCount = 0;
		for (int64& C : BenchRemeshByReason) { C = 0; }
		BenchSeamJobsCompleted = 0;
		BenchUnloadLagSumMs = 0.0;
		BenchUnloadLagMaxMs = 0.0;
		BenchUnloadLagCount = 0;
		BenchUnloadDistSumUU = 0.0;
		BenchUnloadDistMaxUU = 0.0;
		UnloadEnqueueTimeSeconds.Reset();
		BenchEverMeshed.Reset();
		BenchQueuedRemeshReason.Reset();
	}

	// ==================== Debug ====================

	/**
	 * Get debug statistics string.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	FString GetDebugStats() const;

	/**
	 * Residency audit (voxel.Stream.AuditResidency): finds geometry or pipeline state that outlived
	 * the chunk it belongs to. Counts, by category:
	 *  - chunks the manager still tracks beyond the unload radius, by state;
	 *  - renderer chunks the manager no longer tracks;
	 *  - seam buckets (renderer, and the manager's per-owner slot store) whose owner is not tracked;
	 *  - async generation / mesh jobs in flight longer than voxel.Stream.MeshStuckSeconds.
	 * After streaming settles every count must be zero (transient non-zero counts right after a
	 * teleport are expected). The report also carries the worst dispatch-to-complete time seen this
	 * session for generation and meshing, which is what the stuck threshold must stay well above.
	 *
	 * @param OutReport Multi-line human-readable report (counts plus the first offenders).
	 * @return Total number of findings (0 = healthy).
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager|Debug")
	int32 AuditResidency(FString& OutReport) const;

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
	 * Evaluate LOD level changes for loaded chunks and queue remeshes (at most the per-frame cap).
	 * Separated from morph updates; LODEvalSchedule decides when it runs.
	 *
	 * @param Context      This tick's query context.
	 * @param OutQueued    LOD re-meshes queued.
	 * @param OutCapReached True when it stopped at the per-frame cap with candidates left over.
	 * @return Number of candidates: tracked chunks whose committed LOD differs from the strategy's.
	 */
	int32 EvaluateLODLevelChanges(const FLODQueryContext& Context, int32& OutQueued, bool& OutCapReached);

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
	 * "Far" means the chunk's distance band has reached voxel.FarCompression.MinStride, which also
	 * holds with the LOD system off (cubic worlds).
	 * A fresh encode runs on a background worker from a copy of the array (voxel.FarCompression.Async);
	 * the sweep installs finished encodes whose chunk array is unchanged (FChunkDescriptor::AsyncCompactTicket).
	 *
	 * @param Context This tick's query context, used to place LOD-off chunks in a distance band.
	 * @param Timing  Receives this tick's dispatch and install counts.
	 */
	void ProcessFarCompressionSweep(const FLODQueryContext& Context, FVoxelTimingStats& Timing);

	/**
	 * Start an async encode of a resident far chunk: copy its array into a pooled scratch array, stamp
	 * the descriptor with a new ticket and hand the copy to a background worker.
	 *
	 * @param ChunkCoord Chunk to encode
	 * @param Descriptor Its descriptor (Resident, no encode in flight)
	 * @param Codec      General codec for a non-uniform array
	 */
	void DispatchFarCompression(const FIntVector& ChunkCoord, FChunkDescriptor& Descriptor, EVoxelChunkCodec Codec);

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
	 * The renderer lost its geometry (a recreated scene proxy starts empty): re-mesh every chunk whose mesh it
	 * held, and send every seam bucket and water tile again. Each chunk is meshed once on the seam path, so
	 * nothing else would ever replace a lost mesh. Called at the start of each tick.
	 */
	void RecoverLostRenderData();

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
		/** Carried from the FVoxelSeamJob so completion can report dirty->visible latency. */
		double DirtiedAtSeconds = 0.0;
		double ReadyAtSeconds = 0.0;
		double ScheduledAtSeconds = 0.0;
		/** Stamped at dispatch (registry clock) and measured on the worker: splits scheduled->completed. */
		double DispatchedAtSeconds = 0.0;
		double WorkerMs = 0.0;
		/** The owner's residency epoch the job was scheduled against (FVoxelSeamJob::OwnerResidencyEpoch). */
		uint32 OwnerResidencyEpoch = 0;
	};

	/** Thread-safe queue for completed async seam meshes. */
	TQueue<FCompletedSeamMesh, EQueueMode::Mpsc> CompletedSeamMeshQueue;

	/** Seam jobs currently meshing on the worker pool (bounds pipeline depth; prevents dupes). */
	TSet<FVoxelSeamKey> SeamJobsInFlight;

	/** Instrumentation (voxel.Seam.LogLatency): in-flight high-water mark and ticks on which the
	 *  per-tick drain budget (in-flight cap) could not empty the job queue, both since the previous readout. */
	int32 SeamInFlightPeak = 0;
	int32 SeamSlotStarvedTicks = 0;
	/** Instrumentation: game-thread cost of ProcessCompletedSeamMeshes (ticks that submitted >= 1
	 *  mesh) and the frame time seen by the seam scheduler tick, both since the previous readout. */
	double SeamSubmitMsAccum = 0.0;
	double SeamSubmitMsMax = 0.0;
	int32 SeamSubmitTicks = 0;
	int32 SeamSubmitCount = 0;
	double SeamFrameMsAccum = 0.0;
	double SeamFrameMsMax = 0.0;
	int32 SeamFrameTicks = 0;

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

	/**
	 * Dedicated CPU mesher for seam jobs (created lazily when the pipeline activates), matching
	 * the configured meshing mode (CPU DC for Dual Contouring configs, CPU MC for Marching
	 * Cubes — P3). Seam jobs ALWAYS mesh on the CPU worker pool — with a GPU main mesher the
	 * interior meshes route here too (voxel.Seam.CPUInteriorRouting). Seam strips are tiny, so
	 * CPU meshing them is cheap; a dedicated instance also keeps their Config stable.
	 */
	TUniquePtr<IVoxelMesher> SeamMesher;

	/** Lazily create (mode + config matched to the main mesher) and return the CPU seam mesher. */
	IVoxelMesher* EnsureSeamMesher();

public:
	/** Seam registry access for diagnostics (performance HUD, tests). May be null. */
	const FVoxelSeamRegistry* GetSeamRegistry() const { return SeamRegistry.Get(); }

private:

	/**
	 * Per-owner seam slot meshes, kept CPU-side so the renderer receives ONE merged bucket per
	 * owner instead of up to 7 (3 face + 3 edge + 1 corner). With per-slot buckets the scene
	 * proxy's per-frame batch cap was exhausted by near geometry and far seams never drew —
	 * seams appeared to vanish at distance. Re-merging on a slot update is a cheap concat of
	 * strip meshes. Slots: 0-2 face +X/+Y/+Z, 3-5 edge parallel X/Y/Z, 6 corner.
	 */
	struct FSeamOwnerSlots
	{
		FChunkMeshData Slots[7];
		int32 SlotLOD[7] = { -1, -1, -1, -1, -1, -1, -1 };
	};
	TMap<FIntVector, FSeamOwnerSlots> SeamOwnerSlots;

	/** Get (building at most once per content version) the shared edit-merged snapshot of a chunk. */
	TSharedPtr<const TArray<FVoxelData>> GetSeamVoxelSnapshot(const FIntVector& Coord, FVoxelChunkState& State);

	/**
	 * Validate + dispatch one drained seam job (face, edge or corner, same or mixed LOD). Builds the
	 * seam request from the participants' shared edit-merged snapshots and hands it to a thread-pool
	 * worker running the mode-matched seam mesher (EnsureSeamMesher); the result lands in
	 * CompletedSeamMeshQueue.
	 */
	void DispatchSeamJob(const FVoxelSeamJob& Job);

	/** Drain completed seam meshes (game thread) and submit them to the renderer's seam buckets. */
	void ProcessCompletedSeamMeshes();

	// ==================== Coordinated LOD transitions (R24) ====================

	/**
	 * Held LOD transitions: chunk interiors submitted at a new LOD with their crossfade held at
	 * alpha 0, plus the seam buckets their rebuilt seams landed in, released together once those
	 * seams have landed (or voxel.MeshFade.SeamHoldMs passes) so old interior + old seams fade to
	 * new interior + new seams on one alpha — no opening at the chunk boundary during the swap.
	 * Also defers seam buckets that are still fading from an earlier submit (see DeferBucket).
	 */
	FVoxelSeamTransitionHolds SeamHolds;

	/** Seams whose build was dropped because an older job was still in flight: rebuilt when that job finishes. */
	TSet<FVoxelSeamKey> SeamRedirtyAfterFlight;

	/** Hold instrumentation since the previous [SeamLat] readout. */
	int32 SeamHoldGroupsInterval = 0;
	double SeamHoldSecondsSumInterval = 0.0;
	double SeamHoldSecondsMaxInterval = 0.0;
	int32 SeamHoldTimeoutsInterval = 0;
	int32 SeamHoldLeftPendingInterval = 0;
	/** Seam bucket submits deferred behind a running fade since the previous readout. */
	int32 SeamBucketDeferralsInterval = 0;

	/** Hold instrumentation, lifetime. */
	int64 TotalSeamHoldGroups = 0;
	int64 TotalSeamHoldTimeouts = 0;
	double SeamHoldSecondsMaxLifetime = 0.0;

	/** True when an interior submit should wait for its seams: an LOD change of a visible chunk (PreviousMeshedLOD -> NewLOD), or a first appearance the renderer fades in (R24 P3, voxel.MeshFade.AppearHold). */
	bool ShouldHoldForSeams(const FIntVector& ChunkCoord, int32 PreviousMeshedLOD, int32 NewLOD, const FChunkMeshData& NewMesh) const;

	/** Start waiting for the seams a just-submitted LOD change or first appearance of ChunkCoord builds (its fade is already held). */
	void BeginSeamHold(const FIntVector& ChunkCoord);

	/** Release held fades in the renderer and record the hold stats. */
	void ApplySeamHoldReleases(const FVoxelSeamTransitionHolds::FRelease& Release);

	/**
	 * Merge the owner's stored seam slots into its bucket and submit it (crossfading from the one it
	 * replaces; held at alpha 0 while an open LOD transition holds the owner). An all-empty merge
	 * removes the bucket.
	 */
	void SubmitSeamOwnerBucket(const FIntVector& Owner);

	/** Submit the deferred seam buckets whose running fade has ended (see FVoxelSeamTransitionHolds::DeferBucket). */
	void FlushDueSeamBuckets();

	/**
	 * A dispatched (or drained) seam job ended: tell the registry so a seam re-dirtied in flight is
	 * rebuilt, rebuild a seam whose newer build was dropped behind it, and stop holding a transition
	 * for a seam that will not land.
	 */
	void FinishSeamJob(const FVoxelSeamKey& Key, bool bAbandoned);

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

	/** When EvaluateLODLevelChanges runs (viewer movement, cap left-overs, idle sweep); see FVoxelLODEvalSchedule. */
	FVoxelLODEvalSchedule LODEvalSchedule;

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

	/** Analytic world mode matching the config's WorldMode (FVoxelWorldModeFactory::Create) */
	TSharedPtr<IVoxelWorldMode> WorldMode;

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

	/**
	 * Dispatch stamp of one in-flight async job (generation or meshing). The serial ties a result to
	 * the dispatch that currently owns the chunk's in-flight slot: a result whose serial no longer
	 * matches was superseded (its slot was reclaimed by the stuck-job watchdog, or the chunk unloaded)
	 * and is discarded. The dispatch time feeds the watchdog and the dispatch-to-complete stats.
	 */
	struct FAsyncJobStamp
	{
		double DispatchedAtSeconds = 0.0;
		/** CurrentFrame at dispatch: the watchdog also requires a minimum number of manager ticks, so
		 *  time the manager did not tick (PIE pause, a debugger break, one long hitch) is not "stuck". */
		int64 DispatchFrame = 0;
		uint32 Serial = 0;
	};

	/** Next async job serial (0 is never issued). */
	uint32 NextAsyncJobSerial = 1;

	/** Stamp a new dispatch for ChunkCoord in InFlight (replacing any stale entry) and return its serial. */
	uint32 StampAsyncDispatch(TMap<FIntVector, FAsyncJobStamp>& InFlight, const FIntVector& ChunkCoord);

	/**
	 * Accept a completed result if it belongs to the dispatch that owns ChunkCoord's in-flight slot:
	 * removes the entry, records dispatch-to-complete time into WorstSeconds, returns true. Returns
	 * false (entry untouched) for a superseded result.
	 */
	bool AcceptAsyncResult(TMap<FIntVector, FAsyncJobStamp>& InFlight, const FIntVector& ChunkCoord, uint32 Serial, double& WorstSeconds);

	/** Release ChunkCoord's in-flight generation slot (and its pending GPU readback, if any); a late result is then rejected. */
	void CancelInFlightGeneration(const FIntVector& ChunkCoord);

	/** Release ChunkCoord's in-flight mesh slot; a late result is then rejected. */
	void CancelInFlightMesh(const FIntVector& ChunkCoord);

	/**
	 * Stuck-job watchdog (voxel.Stream.MeshStuckSeconds): an async generation or mesh job in flight
	 * longer than the threshold logs a warning, gives up its slot and its chunk goes back to the
	 * pending queue. A mesher or readback that loses a request can then no longer strand a chunk
	 * (state Meshing / Generating forever: never unloads, never remeshes, holds a slot).
	 */
	void CheckStuckAsyncJobs();

	/** Worst dispatch-to-complete time seen this session (seconds), generation and meshing. */
	double WorstGenerationAsyncSeconds = 0.0;
	double WorstMeshAsyncSeconds = 0.0;

	/** Jobs the stuck watchdog re-queued this session. */
	int64 TotalStuckGenerationRequeues = 0;
	int64 TotalStuckMeshRequeues = 0;

	/** Result of an async noise generation task */
	struct FAsyncGenerationResult
	{
		FIntVector ChunkCoord;
		TArray<FVoxelData> VoxelData;
		bool bSuccess = false;
		/** FAsyncJobStamp::Serial of the dispatch that produced this result. */
		uint32 DispatchSerial = 0;
	};

	/** Thread-safe queue for completed async generation results */
	TQueue<FAsyncGenerationResult, EQueueMode::Mpsc> CompletedGenerationQueue;

	/** Chunks currently being generated asynchronously, with their dispatch stamps */
	TMap<FIntVector, FAsyncJobStamp> AsyncGenerationInProgress;

	/** Process completed async generation tasks (called from game thread; fills the gen-apply sub-timers) */
	void ProcessCompletedAsyncGenerations(FVoxelTimingStats& Timing);

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
		uint32 DispatchSerial = 0;                // carried into the result (see FAsyncJobStamp)
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
	void LaunchPostReadbackProcessing(const FIntVector& ChunkCoord, FVoxelNoiseGenerationRequest GenRequest, TArray<FVoxelData> VoxelData, uint32 DispatchSerial);

	// ==================== Async far-chunk compression ====================

	/** A finished far-compression encode, waiting for the game thread to install it. */
	struct FFarCompressResult
	{
		FIntVector ChunkCoord;
		/** FChunkDescriptor::AsyncCompactTicket of the dispatch; a mismatch means the array changed. */
		uint32 Ticket = 0;
		FVoxelChunkCompactForm Form;
		/** The copy the worker encoded, handed back for reuse by a later dispatch. */
		TArray<FVoxelData> Scratch;
	};
	using FFarCompressResultQueue = TQueue<FFarCompressResult, EQueueMode::Mpsc>;

	/**
	 * Finished encodes. The workers hold their own reference and never touch this UObject; Shutdown
	 * swaps in a fresh queue, so a late result from the old session lands in the orphaned one.
	 */
	TSharedPtr<FFarCompressResultQueue, ESPMode::ThreadSafe> FarCompressResults;

	/** Encodes dispatched and not yet drained (capped by voxel.FarCompression.MaxInFlight). */
	int32 FarCompressJobsInFlight = 0;

	/** Bytes of the copies those encodes hold (counted in the voxel data memory stats). */
	int64 FarCompressBytesInFlight = 0;

	/**
	 * Scratch arrays for the next dispatches' copies, reused so a copy is a memcpy into pages that are
	 * already committed. Together with the in-flight copies there are never more than MaxInFlight;
	 * freed once the sweep has dispatched nothing for IdleFrames.
	 */
	TArray<TArray<FVoxelData>> FarCompressScratchPool;

	/** Last frame the sweep dispatched an encode or still had one in flight (drives the pool release). */
	int64 FarCompressLastBusyFrame = 0;

	/** Ticket source for FChunkDescriptor::AsyncCompactTicket (0 is never issued). */
	uint32 NextFarCompressTicket = 0;

	/** Session counters (see GetBenchFarCompressionCounts). */
	int64 FarCompressEncodes = 0;
	int64 FarCompressInstalled = 0;
	int64 FarCompressStale = 0;

	// ==================== Async Mesh Generation ====================

	/** Result of an async mesh generation task */
	struct FAsyncMeshResult
	{
		FIntVector ChunkCoord;
		int32 LODLevel;
		FChunkMeshData MeshData;
		bool bSuccess = false;
		/** FAsyncJobStamp::Serial of the dispatch that produced this result. */
		uint32 DispatchSerial = 0;
	};

	/** Thread-safe queue for completed async mesh results */
	TQueue<FAsyncMeshResult, EQueueMode::Mpsc> CompletedMeshQueue;

	/** Chunks currently being meshed asynchronously, with their dispatch stamps */
	TMap<FIntVector, FAsyncJobStamp> AsyncMeshingInProgress;

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

	// ==================== World Boundary (WM-I5) ====================

	/** Invisible collision walls at the world's edge, owned by this component's actor (see FVoxelWorldBoundary) */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBoxComponent>> WorldBoundaryWalls;

	/** Build the edge walls from the world mode's bounds (game worlds, bounded worlds, bWorldBoundary on). */
	void RebuildWorldBoundary();

	/** Destroy the edge walls. */
	void DestroyWorldBoundary();

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
	 *  -VoxelDeepOff => 1 (no deep data). Applied by the collision cook's slice extraction. */
	bool bDeepDepthFull = false;
	bool bDeepDepthOff = false;

	/** Stale-cull: skip meshing chunks the viewer has already moved past (beyond the unload
	 *  horizon) instead of meshing them and unloading on arrival. Default on; -VoxelNoStaleCull disables. */
	bool bStaleCull = true;

	/** Latest viewer world position, cached each tick for the stale-cull distance test. */
	FVector CurrentViewerPosition = FVector::ZeroVector;

	/** Previous-tick viewer position for horizontal-speed estimation (FLT_MAX until first tick). */
	FVector LastViewerPosForSpeed = FVector(FLT_MAX);

	/** Smoothed viewer horizontal speed (uu/s); drives the speed-adaptive load budget. */
	float ViewerHorizSpeed = 0.0f;

	/** Smoothed frame time for stable throttle decisions (EMA) */
	float SmoothedFrameTimeMs = 16.67f;

	/**
	 * Lowest frame time observed recently (ms), drifting slowly upward. Floors the adaptive
	 * throttle's budget so its release test stays reachable: on a vsync-capped display the
	 * frame time can never drop below the present interval, and a TargetFrameRate faster than
	 * that would otherwise make the throttle a one-way latch for the whole session.
	 */
	float ObservedFrameFloorMs = 16.67f;

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

	/**
	 * Re-mesh churn: mesh jobs launched for chunks that already had a mesh. Counted at launch, not when a
	 * chunk enters the meshing queue: the legacy whole-chunk path re-queues a chunk waiting for its
	 * neighbours every frame, and counting insertions reported those waits as re-meshes (G5, KC14).
	 */
	int64 BenchRemeshCount = 0;

	/** Re-mesh thrash attributed by source (see EVoxelRemeshReason). */
	int64 BenchRemeshByReason[static_cast<int32>(EVoxelRemeshReason::Count)] = {};

	/** Seam meshes applied during the active benchmark (see GetBenchSeamJobsCompleted). */
	int64 BenchSeamJobsCompleted = 0;

	/** Chunks meshed at least once during the active benchmark (to detect re-mesh churn). */
	TSet<FIntVector> BenchEverMeshed;

	/**
	 * Why each queued chunk is waiting to mesh, recorded by AddToMeshingQueue while a benchmark runs and read
	 * when its job launches. The first reason wins: a deferral re-queue (Other) does not hide the LOD change
	 * or neighbour update that queued the chunk.
	 */
	TMap<FIntVector, EVoxelRemeshReason> BenchQueuedRemeshReason;

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
