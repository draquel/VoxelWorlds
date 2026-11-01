// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "VoxelCoreTypes.h"
#include "ChunkDescriptor.h"
#include "VoxelFaceSlabs.h"
#include "Tasks/Task.h"
#include "ChunkRenderData.h"
#include "LODTypes.h"
#include "IVoxelNoiseGenerator.h"
#include "VoxelTerrainConditioning.h"
#include "IVoxelMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelStreamingBenchmark.h"
#include "VoxelSeamRegistry.h"
#include "VoxelSeamTransitionHolds.h"
#include "VoxelLODEvalSchedule.h"
#include "VoxelFarOcean.h"
#include "VoxelPlanetOcean.h"
#include "VoxelSeaSurface.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelTreeInjector.h"
#include <atomic>
#include "VoxelChunkManager.generated.h"

// Forward declarations
class UVoxelWorldConfiguration;
class FVoxelGPUNoiseGenerator;
class IVoxelLODStrategy;
class IVoxelMeshRenderer;
class UVoxelMaterialAtlas;
class UVoxelEditManager;
class UVoxelCollisionManager;
class UVoxelScatterManager;
class UVoxelWaterPropagation;
class FVoxelSeamRegistry;
class FVoxelFarField;
class UBoxComponent;
class UProceduralMeshComponent;
struct FIslandBowlParams;

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

	/** Frame number when state last changed */
	UPROPERTY()
	int64 LastStateChangeFrame = 0;

	/** Frame number when last updated */
	UPROPERTY()
	int64 LastUpdateFrame = 0;

	/** Priority for processing queues */
	float Priority = 0.0f;

	/**
	 * An edit reached the chunk while its mesh was on a worker (world modes review G57): that mesh was built before the
	 * edit, so the chunk re-meshes once it lands. Cleared when a mesh is dispatched (it reads the edit layer then).
	 */
	bool bRemeshAfterMeshing = false;

	/**
	 * A mesh was submitted since the chunk's voxel data arrived, and with it the chunk's water flags were pulled across its
	 * faces from its neighbours (PropagateWaterFromNeighbors): a chunk wholly under a heightmap sea has none before that.
	 * Edit-driven water propagation reads the chunk only then (world modes review G58), and is told when it first becomes
	 * true (UVoxelWaterPropagation::OnChunkWaterSettled, G59).
	 */
	bool bWaterSettled = false;

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
/**
 * Where the loaded chunks are, seen from the streaming viewer (spherical Phase 6, WM-BP2 / WM-BP3): vox.Planet's voxel
 * reach and the streaming bench's residency columns. Plain data, built on demand by UVoxelChunkManager::GetResidencySnapshot.
 */
struct VOXELSTREAMING_API FVoxelResidencySnapshot
{
	/** Chunks the streaming holds (UVoxelChunkManager::GetLoadedChunkCount: loaded or on their way). */
	int32 Tracked = 0;

	/** Of them, chunks in state Loaded (meshed and drawn). */
	int32 Ready = 0;

	/** Ready chunks per committed LOD level (index = LOD). */
	TArray<int32> ByLOD;

	/**
	 * Tracked chunks per layer. A planet's layers are radial: floor(altitude of the chunk centre over the datum / chunk
	 * size), so layer 0 is the chunk-thick slab just above WorldRadius and -1 the one below it; a heightmap world's are
	 * the chunk Z.
	 */
	TMap<int32, int32> ByLayer;

	/** True when ByLayer is keyed by radial layer (a planet). */
	bool bRadialLayers = false;

	/** The viewer the distances are measured from (the streaming viewer of the last tick). */
	FVector Viewer = FVector::ZeroVector;

	/** The viewer's altitude over the datum (planet), else its Z over WorldOrigin. */
	double ViewerAltitude = 0.0;

	/** Farthest tracked chunk centre from the viewer (3D), 0 with none. */
	double MaxDistance = 0.0;

	/**
	 * Farthest tracked chunk centre from the viewer's ground point, as the LOD strategy measures its horizons
	 * (IVoxelLODStrategy::GetStreamingHorizon): on a planet along the ground from the terrain under the viewer (spherical
	 * Phase 6, WM-BP8), on a heightmap world from the viewer's XY (2D distance).
	 */
	double MaxGroundDistance = 0.0;

	/** Lowest and highest tracked chunk centre altitude (as ViewerAltitude). */
	double MinChunkAltitude = 0.0;
	double MaxChunkAltitude = 0.0;

	/** The camera the streaming follows; on a planet the streaming viewer may lie under it (the descent prefetch, WM-BP10). */
	FVector Camera = FVector::ZeroVector;

	/** How far under the camera the streaming viewer is, along local down (uu; 0 = at the camera). */
	double StreamingLead = 0.0;

	/** Planets: the voxel window is suspended above the cut-over, the far field alone drawing the planet (WM-BP10). */
	bool bWindowSuspended = false;

	/**
	 * Planets: the cut-over altitude in force, the altitude under which a suspended window resumes, and the streaming
	 * viewer's altitude over the surface under it (uu).
	 */
	double CutoverAltitude = 0.0;
	double CutoverResumeAltitude = 0.0;
	double ViewerSurfaceAltitude = 0.0;
};

/**
 * What the game module adds to the voxel world's report (vox.Planet / vox.World), each note at its place in a line (code
 * review CR2, FW-18 / CM-34: the plugin owns the voxel.* diagnostics; voxel.World.Stats prints the report with no notes).
 */
struct FVoxelWorldReportNotes
{
	/** After the streaming viewer's place in the reach line (" (the orbit camera)"). */
	FString Viewer;

	/** At the end of the cut-over line (a flight in progress, the landing hold). */
	FString Cutover;
};

/**
 * A change of a planet window's cut-over state (spherical Phase 6, WM-BP10), as the chunk manager records it for the
 * bench, the log and tests (G49, WORLD_MODES_REVIEW_PLAN.md §20.28): when the window resumed or suspended, where the
 * camera and the streaming viewer were, and for a suspension what the window had done since it last resumed.
 */
struct FVoxelCutoverTransition
{
	/** Seconds of streaming ticks since the world started, and the state entered: true = suspended, false = resumed. */
	double Time = 0.0;
	bool bSuspended = false;

	/** The camera's and the streaming viewer's altitude over the surface under them (uu), and the prefetch's lead. */
	double CameraOverSurface = 0.0;
	double ViewerOverSurface = 0.0;
	double Lead = 0.0;

	/** Suspensions: seconds since the window last resumed (-1: active since the world started, never resumed). */
	double ActiveSeconds = -1.0;

	/** Suspensions: chunks loaded when it suspended (all of them unload), and queued generations evicted then. */
	int32 ChunksHeld = 0;
	int32 GenerationsDropped = 0;

	/** Suspensions: chunks generated and meshed since the window last resumed (since the world started if it never did). */
	int64 GeneratedSinceResume = 0;
	int64 MeshedSinceResume = 0;
};

/**
 * A planet's far field (spherical Phase 6, D19: the analytic surface beyond the voxel window) as the bench and vox.Planet
 * report it. Inactive (all zero) until a far field exists.
 */
struct VOXELSTREAMING_API FVoxelFarFieldStats
{
	/** A far field is built and drawn. */
	bool bActive = false;

	/** Game-thread time of its last update (ms). */
	float UpdateMs = 0.0f;

	/** Patches in its trees, and the ones drawn (outside the voxel window's hole). */
	int32 Patches = 0;
	int32 DrawnPatches = 0;

	/** Patch meshes built or rebuilt in the last update, and the memory its meshes hold (MB, CPU and GPU copies, estimated) */
	int32 PatchesBuilt = 0;
	float MemoryMB = 0.0f;

	/** Drawn patches drawing their terrain, and their sea */
	int32 DrawnTerrain = 0;
	int32 DrawnSea = 0;

	/** Edge patches whose terrain is hidden because every voxel chunk under them is drawn (the hole) */
	int32 HiddenEdge = 0;

	/** Builds waiting or in flight, builds in all, and their mean time on the build threads (ms) */
	int32 BuildsPending = 0;
	int64 TotalBuilt = 0;
	float BuildMsMean = 0.0f;

	/** The far field draws the sea (the sea sphere's sag is past its budget: at scale) */
	bool bDrawsSea = false;

	/** Patch components alive (built patches, shown or not) */
	int32 Components = 0;

	/**
	 * Stand-ins over the voxel window (visual polish VP4): patches drawn in the last update because a patch under them was
	 * not ready, reaching into the window, where their coarse surface shows through the voxel terrain; the coarsest one's
	 * level (-1 none); in all, and the updates that had any
	 */
	int32 StandIns = 0;
	int32 StandInMinLevel = -1;
	int64 TotalStandIns = 0;
	int64 StandInUpdates = 0;

	/**
	 * Updates run again in a frame for the camera it renders with, after a cut (visual polish F4.3:
	 * UVoxelChunkManager::UpdateFarFieldForCamera), in all
	 */
	int64 CameraUpdates = 0;
};

/**
 * The chunk manager's work for the camera a frame renders with (visual polish F4.3; world modes review WM-MP4). The chunk
 * manager ticks in TG_PrePhysics, before the player camera managers update (UWorld::Tick: after TG_PostPhysics), so its
 * camera is the frame before's; this runs in TG_PostUpdateWork, after them (UVoxelChunkManager::TickAfterCameraUpdate):
 * after a cut the far field updates again for this frame's camera, and so do the streaming decisions.
 */
USTRUCT()
struct FVoxelFarFieldCameraTickFunction : public FTickFunction
{
	GENERATED_BODY()

	/** The chunk manager this ticks */
	class UVoxelChunkManager* Target = nullptr;

	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override;
	virtual FString DiagnosticMessage() override;
	virtual FName DiagnosticContext(bool bDetailed) override;
};

template<>
struct TStructOpsTypeTraits<FVoxelFarFieldCameraTickFunction> : public TStructOpsTypeTraitsBase2<FVoxelFarFieldCameraTickFunction>
{
	enum
	{
		WithCopy = false
	};
};

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

	// ==================== Finding the manager ====================

	/**
	 * The world's chunk manager: the first UVoxelChunkManager component on any actor of World, the one finder every
	 * system uses (code review CR2: CM-02, VCP-23, POI-25). The last hit is cached, so a hot caller (a movement
	 * component's tick) pays no actor iteration; a manager's Shutdown drops the cache.
	 *
	 * @param World The world to search (null returns null).
	 * @param bRequireInitialized true: only a manager whose Initialize has run (gameplay, POI, the map and PCG);
	 *        false: any manager, initialized or not (navigation binds before initialization).
	 * @return The manager, or null when the world has none.
	 */
	static UVoxelChunkManager* Find(const UWorld* World, bool bRequireInitialized = true);

	/** Every chunk manager component in World, in actor order, initialized or not (the console commands' loop). */
	static TArray<UVoxelChunkManager*> FindAll(const UWorld* World);

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
	 * The voxel at a world position as the chunk holds it, without the edit layer: generation's voxel, with the water
	 * flags the generated pull set; a flood an edit caused lives in the edit layer and is not here (GetVoxelAt has it).
	 * Returns Air if the position is in an unloaded or ungenerated chunk. GetVoxelAtWorldPosition until CR3.
	 *
	 * @param WorldPosition Position in world space
	 * @return Voxel data at that position
	 */
	FVoxelData GetGeneratedVoxelAt(const FVector& WorldPosition) const;

	/**
	 * The voxel at a world position as it is: GetGeneratedVoxelAt with the chunk's edit layer applied (player and system
	 * edits, edit-driven floods). Returns Air for unloaded/ungenerated chunks. Game thread only.
	 * GetEditMergedVoxelAtWorldPosition until CR3 (the glossary's "voxel": the read with edits is the plain one).
	 */
	FVoxelData GetVoxelAt(const FVector& WorldPosition) const;

	/**
	 * Start the snapshots of a neighbourhood's far-compressed chunks (the chunk and its 26 neighbours) decoding on workers
	 * ahead of their readers (world modes review G65). A chunk queued for a mesh or a collision cook is about to be read
	 * with its neighbours (its seam jobs, the water pull at its mesh's completion, the cook): each reads the snapshot,
	 * decoded by then, instead of expanding the chunk on the game thread. Chunks that are resident, empty or edited are
	 * left alone. No-op with voxel.FarCompression.SnapshotReads 0.
	 *
	 * @param ChunkCoord Centre of the neighbourhood
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Streaming")
	void PrefetchVoxelSnapshotsAround(const FIntVector& ChunkCoord);

	/**
	 * Snapshots of far-compressed chunks decoded on workers this session (G65; voxel.FarCompression.Decodes reports it).
	 *
	 * @return Decode tasks launched
	 */
	int64 GetSnapshotDecodeCount() const { return SnapshotDecodeTasks; }

	/**
	 * Snapshots of resident chunks that shared the chunk's own array this session instead of copying it (world modes review
	 * G71; voxel.Snapshot.ShareResident), and those copied (without edits).
	 *
	 * @param OutCopied Receives the copies made from resident chunks without edits
	 * @return The shared ones
	 */
	int64 GetResidentSnapshotsShared(int64& OutCopied) const
	{
		OutCopied = SnapshotCounters.Made[static_cast<int32>(ESnapshotKind::Copied)];
		return SnapshotCounters.Made[static_cast<int32>(ESnapshotKind::SharedResident)];
	}

	/**
	 * Chunks collapsed to uniform as they were generated this session (world modes review G67;
	 * voxel.FarCompression.UniformAtGeneration).
	 *
	 * @return The count
	 */
	int64 GetUniformAtGenerationCount() const { return UniformAtGenerationCount; }

	/**
	 * The seam snapshot cache's contents and session counters (world modes review G69): entries and MB by how they were
	 * made (copied from a resident chunk, with edits, decoded from a compressed or a uniform chunk), by the reader that
	 * made them and the last that read them, by age since their last read and by reads; how many workers still hold;
	 * what was made, read, replaced, evicted and removed this session.
	 *
	 * @return Multi-line description (voxel.Snapshot.Stats logs it)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Streaming|Debug")
	FString DescribeSnapshotCache() const;

	/** Clear the snapshot cache's session counters (voxel.Snapshot.Stats reset). */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Streaming|Debug")
	void ResetSnapshotCacheCounters()
	{
		SnapshotCounters = FSnapshotCacheCounters();
		RecentlyEvictedSnapshots.Reset();
		FMemory::Memzero(FaceSlabsMade, sizeof(FaceSlabsMade));
		ShellHits = 0;
		VoxelResidencyStats::ResetSharedArrayClones();
	}

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
	TSharedPtr<const TArray<FVoxelData>> GetVoxelSnapshot(const FIntVector& ChunkCoord);

	/**
	 * Whether a chunk has voxel data (resident or compressed), i.e. whether GetVoxelSnapshot would return one.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if the chunk is tracked and its voxel data is available
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	bool HasChunkVoxelData(const FIntVector& ChunkCoord) const;

	/**
	 * Whether a chunk's water flags are in place: it has voxel data and has submitted a mesh since that data arrived, which
	 * pulls the flags across its faces from its neighbours (PropagateWaterFromNeighbors; a chunk wholly under a heightmap
	 * sea gets all of its water that way). Edit-driven water propagation reads a chunk only then (world modes review G58).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return True if the chunk has voxel data with its water flags pulled
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	bool IsChunkWaterSettled(const FIntVector& ChunkCoord) const;

	/**
	 * EDIT-AWARE surface on the local up-line through a position, in any world mode (spherical SP-A3): the form of
	 * QueryEditMergedSurface that works on a planet. Heightmap modes run QueryEditMergedSurface at (X, Y) and return
	 * Position (X, Y, Height). On a planet it starts from the analytic surface along the direction from the centre and
	 * walks a digital line of edit-merged voxels along local up, one chunk either side (one voxel read per step, as the
	 * column does) to the topmost solid voxel with air above it, then finds the crossing on the up-line itself in the
	 * trilinear edit-merged density around it; the normal from that density's gradient (turned toward local up), the
	 * slope against local up. False when no loaded chunk holds the surface there (the caller falls back to
	 * GetGeneratedSurfacePoint or FVoxelSurfaceQuery::SampleSurfacePoint). Game thread only.
	 *
	 * @param WorldPos Any position on the up-line
	 * @param OutSample The surface (Position, Normal, Up, SlopeDegrees, Altitude, MaterialID, BiomeID)
	 * @return true if a loaded edit-merged surface was found
	 */
	bool QueryEditMergedSurfacePoint(const FVector& WorldPos, FVoxelSurfaceSample& OutSample) const;

	/**
	 * The surface ladder at a position, in any world mode (code review CR2, CM-21: the one entry over the three rungs):
	 * the natural surface (the world mode's, no zones), the generated surface (the conditioning zones there applied, and
	 * on a block world the top face of the block column's topmost block, as GetGeneratedSurfacePoint gives it), or the
	 * edit-merged surface of the loaded voxels (QueryEditMergedSurfacePoint; false where none is loaded). The natural and
	 * generated rungs sample the conditions through FVoxelSurfaceQuery::SampleSurfacePoint (the biome pipeline, no loaded
	 * chunk needed) and always succeed once the manager is initialized. Game thread only.
	 *
	 * @param WorldPos Any position on the up-line
	 * @param Layer Which rung
	 * @param OutSample The surface (Position, Normal, Up, SlopeDegrees, Altitude, MaterialID, BiomeID)
	 * @return true when the rung has a surface there
	 */
	bool QuerySurfacePoint(const FVector& WorldPos, EVoxelSurfaceLayer Layer, FVoxelSurfaceSample& OutSample) const;

	/**
	 * The generated (analytic) surface point on the local up-line through a position, without loaded chunks (spherical
	 * SP-A3): in heightmap modes (X, Y) at the generated column's height (the continentalness-modulated base terrain with the
	 * conditioning zones there, as generation produces it; on a block world the top of the block column's topmost block,
	 * WM-C5), the canonical far-band placement estimate for spawn, navigation and POI seeding (the near band's edits are
	 * QueryEditMergedSurfacePoint's); on a planet the terrain along the direction from the centre with the planet zones
	 * there applied (SP-B10), as the planet generator conditions it. GetSurfacePointAt is the world mode's natural surface,
	 * without zones. The one generated rung (code review CR2, CM-21).
	 *
	 * @param WorldPos Any position on the up-line
	 * @return World position of the surface point (WorldPos before Initialize)
	 */
	FVector GetGeneratedSurfacePoint(const FVector& WorldPos) const;

	/**
	 * The level of the surface under a position (the generated terrain on the up-line through it, or the sea where that
	 * lies higher), as a flight level: a distance from the centre on a planet, world Z on a heightmap world (what debug
	 * flight holds, UVCMovementComponent::GetFlightLevel; code review CR2, FW-20).
	 * @param WorldPos Any position on the up-line
	 * @param bOutSea Whether the sea is the surface there
	 * @return The level
	 */
	double GetSurfaceOrSeaLevel(const FVector& WorldPos, bool& bOutSea) const;

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
	 * The analytic terrain surface on the local up-line through a position, with its outward normal (spherical SP-A1):
	 * the point at (X, Y) in heightmap modes, along the direction from the centre on a planet. No caves, conditioning
	 * zones or edits (GetGeneratedSurfacePoint and QueryEditMergedSurfacePoint add those in heightmap modes).
	 *
	 * @param WorldPos Any position on the up-line
	 * @return The surface point, its normal and its altitude
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	FVoxelTerrainSurfacePoint GetSurfacePointAt(const FVector& WorldPos) const;

	/**
	 * Whether this world's walking surface is block tops (cubic meshing in a heightmap world mode; world modes review
	 * WM-C5, KC6). There GetGeneratedSurfacePoint and QueryEditMergedSurfacePoint return the top face of
	 * the block column's topmost block, not the density isosurface up to a block under it, and anything placed on a pad or
	 * the generated ground should stand on SnapToBlockTop of its height.
	 *
	 * @return True in a cubic heightmap world (with voxel.Cubic.BlockSurface on)
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	bool IsBlockSurface() const;

	/**
	 * The walking surface for a generated column height: in a block world the top face of the topmost block the generator
	 * makes from it (FVoxelBlockSurface::BlockTopForHeight, between Height and Height + VoxelSize); Height itself elsewhere.
	 *
	 * @param Height A generated column height (a pad height, GetTerrainHeightAt plus zones)
	 * @return World Z to stand things on
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	float SnapToBlockTop(float Height) const;

	// ==================== Voxel trees (world modes review WM-C8, KC9) ====================
	// Cubic worlds inject trees into the voxel data (FVoxelTreeInjector). They are recomputed from the same seeded
	// positions here, so gameplay can find the tree a trunk block belongs to and fell it. Game thread only.

	/**
	 * Whether this world injects voxel trees (cubic meshing in a heightmap mode, TreeMode not HISM, templates and a density).
	 *
	 * @return True when FindVoxelTreeAt can find anything
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Trees")
	bool HasVoxelTrees() const;

	/**
	 * The voxel tree whose trunk passes through a world position: of the trees the injector places with a trunk block in
	 * that voxel, the one whose trunk is there (the voxel is solid and carries the trunk material). Not a UFUNCTION
	 * (FVoxelTreeInstance is plain C++).
	 *
	 * @param WorldPos A position inside the trunk block
	 * @param OutTree The tree
	 * @return False in a world without voxel trees, or when that voxel is no placed tree's trunk block
	 */
	bool FindVoxelTreeAt(const FVector& WorldPos, FVoxelTreeInstance& OutTree) const;

	/**
	 * The voxel trees placed within a horizontal radius of a position (their bases), nearest first, that still have a trunk
	 * block above the ground in loaded voxels (QA: vox.Chop, vox.Trees).
	 *
	 * @param WorldPos Search centre
	 * @param Radius Horizontal search radius (uu)
	 * @param OutTrees Receives the trees (reset first)
	 */
	void FindVoxelTreesNear(const FVector& WorldPos, float Radius, TArray<FVoxelTreeInstance>& OutTrees) const;

	/**
	 * A voxel tree's blocks that are still as its stamp wrote them: the edit-merged voxel is solid and carries the stamp's
	 * material (a dug block, or one replaced by a neighbouring tree's stamp or by an edit, is left out).
	 *
	 * @param Tree The tree (FindVoxelTreeAt)
	 * @param OutBlocks Receives the blocks (reset first)
	 */
	void GetVoxelTreeBlocks(const FVoxelTreeInstance& Tree, TArray<FVoxelTreeVoxel>& OutBlocks) const;

	/**
	 * Fell a voxel tree: one edit operation (outside the undo history, the edit source as set by the caller) that empties
	 * every block of it GetVoxelTreeBlocks returns, except, with bKeepStump, its trunk's ground-level layer, which stays as
	 * a stump flush with the ground. Scatter, collision and navigation follow as for any edit; the edit layer saves it.
	 *
	 * @param Tree The tree
	 * @param bKeepStump Keep the trunk's lowest layer (the ground's top voxel the trunk replaced)
	 * @return Blocks removed (trunk and leaves)
	 */
	int32 FellVoxelTree(const FVoxelTreeInstance& Tree, bool bKeepStump = true);

	/**
	 * World position of a global voxel's cell centre (global voxel coordinates count from WorldOrigin in voxels).
	 *
	 * @param GlobalVoxel Global voxel coordinate
	 * @return The cell's centre
	 */
	FVector GlobalVoxelToWorldCentre(const FIntVector& GlobalVoxel) const;

	/**
	 * Where the player spawns, in every world mode: on a planet the terrain surface along the configuration's spawn
	 * direction (PlanetSpawnLocation) plus PlanetSpawnAltitude along its up (spherical SP-B11;
	 * UVoxelWorldConfiguration::GetPlanetSpawnPosition, which has no world mode, gives the datum's point instead); in the
	 * heightmap modes the configuration's WorldOrigin. GetPlanetSpawnPosition until CR3 (CM-22).
	 *
	 * @return World position
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	FVector GetSpawnPosition() const;

	/**
	 * The sea as the water rules see it (FVoxelSeaSurface::FromConfig): the sphere of WaterRadius on a planet, the plane
	 * at WaterLevel in heightmap modes, where generation floods and the water tiles draw wherever WorldOrigin is (review
	 * plan G19). Every consumer reads it: navigation, the character, the NPC spawner. bEnabled is false with water off.
	 *
	 * @return The sea surface (disabled before Initialize)
	 */
	FVoxelSeaSurface GetSeaSurface() const;

	/**
	 * Signed depth of a world position below the sea (spherical SP-E1): along the radius on a planet, along Z under the
	 * WaterLevel plane in heightmap modes (GetSeaSurface).
	 *
	 * @param WorldPos World position
	 * @return Positive below the sea, negative above; a large negative value when water is off
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	float GetSeaDepthAt(const FVector& WorldPos) const;

	/**
	 * Where KillZ has to be for a planet and the space around it not to be cut by it (spherical SP-F5, Phase 6): one
	 * margin below the lowest point of the planet's world, Center.Z - OrbitRadius (the orbit ceiling, so a pawn flying or
	 * held over the southern hemisphere at any altitude up to it lives; OuterRadius when the bounds carry none), when the
	 * current KillZ lies above that, else the current value. The chunk manager applies it to the world's settings when a
	 * planet initialises.
	 *
	 * @param CurrentKillZ The world settings' KillZ
	 * @param Bounds The planet's bounds (a shell); other shapes return CurrentKillZ
	 * @param Margin Extra depth below the lowest point
	 * @return The KillZ to use
	 */
	static double ComputePlanetKillZ(double CurrentKillZ, const FVoxelWorldBounds& Bounds, double Margin);

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

	// ==================== Far Ocean (WM-E1) ====================

	/**
	 * Whether this world draws a far ocean: the sea and the seabed past what is streamed, out to
	 * UVoxelWorldConfiguration::IslandFarOceanDistance past the edge (islands with bIslandFarOcean; see FVoxelFarOcean).
	 *
	 * @return True when the far ocean's meshes exist (they may be hidden by voxel.World.FarOcean 0)
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	bool HasFarOcean() const { return FarOceanStatic != nullptr; }

/** Show or hide the far ocean (voxel.World.FarOcean). */
	void SetFarOceanVisible(bool bVisible);

	/** One-line summary of the far ocean for logs (voxel.World.FarOceanStats). */
	FString GetFarOceanDebugString() const;

	/** The far ocean's meshes: section 0 the seabed, 1 the open-sea water, 2 its underside (null without one). */
	UProceduralMeshComponent* GetFarOceanStaticComponent() const { return FarOceanStatic; }

	/** The settings the far ocean was built with. */
	const FVoxelFarOceanSettings& GetFarOceanSettings() const { return FarOceanSettings; }

	// ==================== Planet Ocean (spherical SP-E2) ====================

	/**
	 * Whether this planet draws its sea surface: a cube sphere at WaterRadius with the configuration's water material
	 * (FVoxelPlanetOcean). Planets with water on and a WaterMeshMaterial.
	 *
	 * @return True when the ocean's mesh exists (it may be hidden by voxel.World.PlanetOcean 0)
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	bool HasPlanetOcean() const { return PlanetOcean != nullptr; }

	/** The planet ocean: section 0 the top (faces outward), section 1 the underside (null without an ocean). */
	UProceduralMeshComponent* GetPlanetOceanComponent() const { return PlanetOcean; }

	/** Whether the planet ocean's underside is shown now (the viewer is inside the sea radius). */
	bool IsPlanetOceanUndersideShown() const { return bPlanetOceanUndersideShown; }

	/** Show or hide the planet ocean (voxel.World.PlanetOcean). */
	void SetPlanetOceanVisible(bool bVisible);

	// ==================== Far Field (spherical Phase 6, WM-BP9; heightmap worlds WM-MP1) ====================

	/**
	 * Whether this world draws its far field: the analytic surface beyond the voxel window, to the horizon and from orbit
	 * (FVoxelFarField, decision D19; on a heightmap world a quadtree of root tiles over the plane, WM-MP1 / D36). Game
	 * worlds on a planet or a heightmap world with voxel.World.FarField on.
	 *
	 * @return True when the far field exists
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	bool HasFarField() const { return FarField.IsValid(); }

	/** The far field, mutable (tests: synchronous builds). */
	FVoxelFarField* GetFarFieldMutable() { return FarField.Get(); }

	/** Whether the far field draws the sea, in place of the sea sphere (at scale: FVoxelFarField::ShouldDrawSea). */
	bool DoesFarFieldDrawSea() const;

/** A built patch's component (section 0 terrain, 1 sea, 2 the sea's underside), or null. */
	UProceduralMeshComponent* GetFarFieldComponent(uint64 PackedPatchKey) const;

	/** Patch components alive. */
	int32 GetFarFieldComponentCount() const { return FarFieldComponents.Num(); }

	/**
	 * Build or drop the far field, the sea sphere and the far ocean for voxel.World.FarField: with the far field off the
	 * sea sphere draws a planet's sea and an island's far ocean builds its grid water, as before.
	 *
	 * @param bEnabled Far field on
	 */
	void SetFarFieldEnabled(bool bEnabled);

	/** One-line summary of the far field for logs (voxel.World.FarFieldStats). */
	FString GetFarFieldDebugString() const;

	/**
	 * Run one far-field update now for a viewer (tests; the tick does this every frame after the streaming sections).
	 *
	 * @param ViewerPosition The streaming viewer and camera
	 * @param FieldOfView Horizontal field of view (degrees)
	 */
	void UpdateFarFieldForViewer(const FVector& ViewerPosition, float FieldOfView = 90.0f);

	/**
	 * The far field for the camera a frame renders with (visual polish F4.3). The tick updates the far field in
	 * TG_PrePhysics with the player camera manager's camera, which is the frame before's (the camera managers update after
	 * TG_PostPhysics), so after a cut (a teleport) the first frame was drawn with the selection for the old position: on a
	 * face the camera never saw, nothing. When this frame's camera lies more than a chunk from the camera of the last
	 * update, the far field updates again for it, the streaming viewer (the window's centre) as the last update had it.
	 * Called from TG_PostUpdateWork (FVoxelFarFieldCameraTickFunction) with voxel.World.FarFieldCameraUpdate on.
	 *
	 * @param CameraPosition This frame's camera
	 * @param FieldOfView Its horizontal field of view (degrees)
	 * @return Whether the far field updated
	 */
	bool UpdateFarFieldForCamera(const FVector& CameraPosition, float FieldOfView);

	/**
	 * The camera tick's body, with the first player camera manager's camera (nothing while the bench drives the view,
	 * SetBenchmarkView, or without a camera manager): UpdateFarFieldForCamera unless voxel.World.FarFieldCameraUpdate is 0,
	 * then UpdateStreamingForCamera unless voxel.Stream.CameraCutUpdate is 0.
	 */
	void TickAfterCameraUpdate();

	/**
	 * The streaming decisions again for the camera a frame renders with, after a cut (world modes review WM-MP4, every world
	 * mode). The tick streamed for the camera of the frame before; when this frame's camera lies more than a chunk from it
	 * (a teleport, a respawn, a switch of view target), the strategy's view, the load and unload decisions, the queues'
	 * order and the first generation launches run again for it, so the chunks under the new position are requested in the
	 * frame of the cut rather than the next. The camera becomes the streaming viewer for the pass (the descent prefetch's
	 * speed starts over after a jump). Under a chunk of motion a frame (every ordinary frame) nothing runs.
	 *
	 * @param CameraPosition This frame's camera
	 * @return Whether the decisions ran again
	 */
	bool UpdateStreamingForCamera(const FVector& CameraPosition);

	/** Streaming updates run again for a frame's camera after a cut (UpdateStreamingForCamera), in all. */
	int64 GetCameraCutUpdates() const { return CameraCutUpdates; }

	/** The tick function that runs TickAfterCameraUpdate after the camera managers (tests run it as a world tick would). */
	FVoxelFarFieldCameraTickFunction& GetFarFieldCameraTickFunction() { return FarFieldCameraTick; }

	// ==================== The voxel window's cut-over (spherical Phase 6 WM-BP10, D18; every world mode since MP3) ====================

	/**
	 * Whether the voxel window is suspended: the streaming viewer above the voxel cut-over
	 * (UVoxelWorldConfiguration::VoxelCutoverAltitude, voxel.Stream.VoxelCutover), where every chunk unloads and the
	 * far field alone draws the world (IVoxelLODStrategy::IsWindowSuspended). It streams back below a fraction of it,
	 * fading in over the far field.
	 *
	 * @return True while suspended (always false off a planet)
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	bool IsVoxelWindowSuspended() const;

	/** The camera the streaming follows this tick, before the descent prefetch moves the streaming viewer under it. */
	FVector GetCameraPosition() const { return CurrentCameraPosition; }

	/** The streaming viewer this tick: the camera, or under it while it descends (the descent prefetch, every world mode). */
	FVector GetStreamingViewerPosition() const { return CurrentViewerPosition; }

	/** How far under the camera the streaming viewer is this tick (uu along local down; 0 = at the camera). */
	double GetStreamingViewerLead() const { return StreamingViewerLead; }

	/**
	 * The window's cut-over transitions since the world started, oldest first (at most MaxCutoverTransitions kept; the
	 * count of all of them is GetCutoverTransitionCount). G49's instrument: a suspension soon after a resume, with the
	 * chunks it held and the generations it dropped, is the window resumed for nothing.
	 */
	const TArray<FVoxelCutoverTransition>& GetCutoverTransitions() const { return CutoverTransitions; }
	int64 GetCutoverTransitionCount() const { return CutoverTransitionCount; }
	static constexpr int32 MaxCutoverTransitions = 256;

	/**
	 * Pure: the descent prefetch's lead (WM-BP10; every world mode since world modes review WM-MP3): under a camera descending
	 * faster than MinDescentSpeed, the streaming viewer goes ahead of it along local down by the distance it covers in
	 * LeadSeconds, at most half its altitude and stopping Clearance over the surface, so the voxel window resumes from the
	 * cut-over and refines its LOD before the camera gets there (a descent slowing toward the ground resumes it from twice
	 * the cut-over at most).
	 *
	 * @param RadialSpeed The camera's smoothed speed along local up (uu/s, negative descending)
	 * @param AltitudeOverSurface The camera's altitude over the surface under it (the terrain, or the sea where higher)
	 * @param LeadSeconds How far ahead (0 = no prefetch)
	 * @param MinDescentSpeed Slower descents have no lead (a jump, a walk downhill)
	 * @param Clearance How close to the surface the streaming viewer may go
	 * @return The lead (uu along local down, >= 0)
	 */
	static double ComputeDescentLead(double RadialSpeed, double AltitudeOverSurface, double LeadSeconds, double MinDescentSpeed, double Clearance);

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
	 * Get the edit manager.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager")
	UVoxelEditManager* GetEditManager() const { return EditManager; }

	/**
	 * Get the edit-driven water propagation (floods the voxels an edit opens to the sea), for tests and debugging.
	 *
	 * @return The propagation, or null when water is off (bEnableWaterLevel) or before Initialize
	 */
	UVoxelWaterPropagation* GetWaterPropagation() const { return WaterPropagation; }

	/**
	 * The 2D water tile's column over a world position (heightmap worlds with water tiles): the chunk layers that mark it wet,
	 * as the tile's mask has them before its dilation. For the voxel.Water.Stats command (world modes review G59).
	 *
	 * @param WorldPosition Any point over the column
	 * @param OutWetChunkZ Receives the chunk Z of each layer whose voxels hold water in the column
	 * @return True if the column's tile exists
	 */
	bool GetWaterTileColumn(const FVector& WorldPosition, TArray<int32>& OutWetChunkZ) const;

	/**
	 * Whether a chunk column (chunk X, Y) has a 2D water tile (heightmap worlds): the far field hides an edge patch's sea
	 * there (WM-MP1).
	 *
	 * @param Column Chunk X and Y
	 * @return True while the column's tile exists
	 */
	bool HasWaterTile(const FIntPoint& Column) const;

	/**
	 * G77: audit the 2D water tiles against the generated surface. Every voxel column whose generated ground lies more
	 * than a voxel under the water plane should be wet in its chunk column's tile (the tile's masks OR-ed over the
	 * layers). Logs the totals over every chunk column with a tile or a loaded chunk at the water plane's layer, then
	 * the chunk columns with the most columns missing, each with its loaded chunks (residency, edits, water settled,
	 * LOD, the stored mask's wet columns, and the mask rebuilt from the chunk's voxels as UpdateWaterTileContribution
	 * reads them: a stored mask that differs from the rebuilt one is stale; a rebuilt mask still missing columns under
	 * the water plane has no water flags there).
	 *
	 * @param MaxList      At most this many chunk columns listed (the worst first)
	 * @param DetailColumn A chunk column (X, Y) detailed whatever its count; (MAX_int32, MAX_int32) for none
	 */
	void AuditWaterTiles(int32 MaxList, const FIntPoint& DetailColumn);

	/** G77 diagnostics: mark every water tile dirty so its mesh is sent to the renderer again. @return The tile count */
	int32 ResendWaterTiles();

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

	/**
	 * What system and editor edits did to scatter since Initialize (world modes review G33), for tests and
	 * voxel.Scatter.EditAudit. An edited chunk's scatter re-extracts with its re-mesh; the chunks whose extraction reads
	 * its edited voxels (UVoxelScatterManager::GetEditReaders) re-extract once per tick from their voxels, unless they were
	 * edited themselves.
	 */
	struct FEditScatterStats
	{
		/** OnChunkEdited broadcasts from system and editor edits. */
		int64 Broadcasts = 0;
		/** Readers re-extracted (each once per tick however many edited chunks it reads). */
		int64 ReadersReextracted = 0;
		/** Readers edited in the same tick, which re-extract with their own re-mesh. */
		int64 ReadersFolded = 0;
		/** Readers without voxel data: not loaded yet (their extraction reads the edit when they load), or unloading. */
		int64 ReadersWithoutData = 0;
		/** voxel.Scatter.EditReaders 0 (the rule before G33): face neighbours regenerated. */
		int64 LegacyFaceNeighbours = 0;
	};

	/** @return What system and editor edits did to scatter since Initialize (see FEditScatterStats) */
	const FEditScatterStats& GetEditScatterStats() const { return EditScatterStats; }

	/**
	 * Meshing work since Initialize (world modes review G56, G57), for tests and voxel.Edit.MeshWork: chunk meshes and seam
	 * jobs with their worker time (streaming's as well as edits'), and what edits asked of the meshing pipeline. An edit
	 * re-meshes the chunks it changed and restales their seams; voxel.Edit.NeighbourRemesh 1 (the rule before G56) also
	 * re-meshes each edited chunk's six face neighbours.
	 */
	struct FMeshWorkStats
	{
		/** Chunk mesh jobs dispatched to a worker. */
		int64 ChunkMeshJobs = 0;
		/** Their worker time. */
		double ChunkMeshWorkerMs = 0.0;
		/** Seam jobs dispatched to a worker. */
		int64 SeamJobs = 0;
		/** Their worker time, counted when their results land. */
		double SeamWorkerMs = 0.0;
		/** OnChunkEdited broadcasts the chunk manager handled (one per chunk an edit changed). */
		int64 EditBroadcasts = 0;
		/** Edited chunks that were loaded: queued for a re-mesh, their seams restaled. */
		int64 EditedRemeshes = 0;
		/** Edits that reached a chunk on its way to a mesh (queued or on a worker): its seams restaled (G57). */
		int64 EditedOnTheWay = 0;
		/** Of those, chunks whose mesh was on a worker: re-meshed once it landed (G57). */
		int64 RemeshesAfterMeshing = 0;
		/** Face neighbours of edited chunks queued for a render re-mesh (voxel.Edit.NeighbourRemesh 1, the rule before G56). */
		int64 NeighbourRemeshes = 0;
	};

	/** @return The meshing work since Initialize (see FMeshWorkStats) */
	const FMeshWorkStats& GetMeshWorkStats() const { return MeshWorkStats; }

	/**
	 * The water pull's second passes since Initialize (world modes review G70), for tests and voxel.Water.Stats. A chunk pulls
	 * the water flags across its faces from its neighbours when its mesh lands (PropagateWaterFromNeighbors); a neighbour
	 * whose water rose after that (its own pull, or its generated water arriving) has it pulled again, so the flood reaches
	 * the same voxels whatever order the chunks finish meshing in.
	 */
	struct FWaterRepullStats
	{
		/** Chunks queued to pull again (a face neighbour's water rose after their pull). */
		int64 Queued = 0;
		/** Pulls run from the queue (Loaded chunks; the others pull again when their next mesh lands). */
		int64 Run = 0;
		/** Of those, pulls that flooded voxels. */
		int64 Flooded = 0;
		/** Voxels those pulls flooded. */
		int64 FloodedVoxels = 0;
		/** Queued chunks skipped: no voxel data any more, or not Loaded (a mesh on its way pulls when it lands). */
		int64 Skipped = 0;
		/** Chunks still queued. */
		int32 Pending = 0;
	};

	/** @return The water pull's second passes since Initialize (see FWaterRepullStats) */
	FWaterRepullStats GetWaterRepullStats() const
	{
		FWaterRepullStats Out = WaterRepullStats;
		Out.Pending = WaterRepullQueue.Num();
		return Out;
	}

	// ==================== Terrain Conditioning (Phase 6c) ====================

	/**
	 * Add a gen-time terrain conditioning zone (flattens terrain toward a target under POIs/claims).
	 * Affects chunks generated AFTER this call. For deterministic POIs, register conditioning at world
	 * init so chunks generate already-conditioned. Re-conditioning already-loaded chunks is Phase 7.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|ChunkManager|Conditioning")
	void AddConditioningZone(const FVoxelConditioningZone& Zone);

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
	 * Append the conditioning zones a world box's terrain takes, in any world mode (code review CR2, CM-20: the one
	 * gather a caller with a box needs, so it never asks the mode): on a planet the zones reaching the cap the box
	 * subtends (GetPlanetCapOfBox, GatherConditioningZonesOnSphere); on a heightmap world the zones overlapping its XY
	 * region. Game thread.
	 * @param Box      World box
	 * @param OutZones Appended with the zones
	 */
	void GatherConditioningZones(const FBox& Box, TArray<FVoxelConditioningZone>& OutZones) const;

	/**
	 * Planets (spherical SP-B10): append the planet zones (static, with a Direction, and the dynamic conditioner's
	 * GatherConditioningOnSphere) whose influence reaches a cap on the sphere: what generation layers onto the radial
	 * height there (apply with FVoxelTerrainConditioning::ApplyToAltitude). Game thread. The map, scatter and PCG use it
	 * for their analytic planet surfaces; generation gathers each chunk's by the cap its box subtends. Nothing on a
	 * heightmap world.
	 * @param Direction     Unit direction from the planet centre to the cap's centre
	 * @param AngularRadius The cap's half-angle in radians (0 for a point)
	 * @param OutZones      Appended with the zones
	 */
	void GatherConditioningZonesOnSphere(const FVector& Direction, double AngularRadius, TArray<FVoxelConditioningZone>& OutZones) const;

	/**
	 * Planets: the cap (direction and half-angle) a world box subtends from the planet centre, the query
	 * GatherConditioningZonesOnSphere takes for a chunk or a region: the box centre's direction and asin(half diagonal /
	 * distance), the whole sphere for a box around the centre.
	 * @param Box           World box
	 * @param OutDirection  The cap's direction
	 * @param OutAngle      The cap's half-angle (radians)
	 * @return False when the world is not a planet
	 */
	bool GetPlanetCapOfBox(const FBox& Box, FVector& OutDirection, double& OutAngle) const;

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
	 * @param OutMissingNeighborMask If set, receives the neighbours without data (bits of
	 *        UVoxelCollisionManager::GetNeighborBit): the mesher reads them as air, so the cook is re-run when they arrive (G32)
	 * @param OutSnapshotsReady If set, receives the worker tasks still decoding snapshots or shells, for the cook's worker
	 *        to wait for (G65); else the game thread waits for them here
	 * @param OutNeighborFaceSlabs If set and voxel.Collision.NeighborShells is on, receives the neighbours' boundary shells
	 *        instead of their snapshots (G69: a cook reads only VoxelNeighborSlices::GetNeighborDepth planes of each);
	 *        OutNeighborSnapshots is then left empty
	 * @return True if request was prepared successfully
	 */
	bool PrepareCollisionMeshRequest(
		const FIntVector& ChunkCoord,
		int32 LODLevel,
		FVoxelMeshingRequest& OutMeshRequest,
		TSharedPtr<const TArray<FVoxelData>>& OutSharedVoxels,
		TMap<FIntVector, TSharedPtr<const TArray<FVoxelData>>>& OutNeighborSnapshots,
		uint32* OutMissingNeighborMask = nullptr,
		TArray<UE::Tasks::FTask>* OutSnapshotsReady = nullptr,
		TMap<FIntVector, TSharedPtr<const FVoxelFaceSlabs>>* OutNeighborFaceSlabs = nullptr);

	/**
	 * Start the inputs of a queued collision cook decoding on workers (G65, G69): with shells (ShouldUseCollisionShells),
	 * the 26 neighbours' boundary shells (the far-compressed ones without edits; the chunk's own snapshot is decoded when the
	 * cook starts, so a long queue holds no whole snapshots); without, the 27 snapshots as PrefetchVoxelSnapshotsAround.
	 * No-op with voxel.FarCompression.SnapshotReads 0.
	 *
	 * @param ChunkCoord The chunk to be cooked
	 * @param LODLevel   The cook's LOD level (sets the shells' depth)
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Streaming")
	void PrefetchCollisionInputs(const FIntVector& ChunkCoord, int32 LODLevel);

	/**
	 * Whether collision cooks read their neighbours' boundary shells (voxel.Collision.NeighborShells, G69).
	 *
	 * @return True if they do
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Streaming")
	bool UsesCollisionNeighborShells() const;

	/**
	 * Whether a cook at this neighbour depth takes shells (G69): voxel.Collision.NeighborShells is on and a shell of that
	 * depth is at most three quarters of a chunk (8 * Depth <= ChunkSize: one plane at 64^3 is 9%, the stride + 1 planes of
	 * collision LOD 2 47%); a deeper shell would hold about as much as the chunk, so the cook reads whole snapshots.
	 *
	 * @param Depth Planes per face (VoxelNeighborSlices::GetNeighborDepth)
	 * @return True if shells are used
	 */
	bool ShouldUseCollisionShells(int32 Depth) const;

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
		int64 RendererGPUBytes = 0;    // Renderer GPU memory
		int64 CollisionBytes = 0;      // Collision manager memory
		int64 ScatterBytes = 0;        // Scatter manager + renderer memory
		int64 TotalBytes = 0;          // Sum of above
		int64 SnapshotBytes = 0;       // Shared voxel snapshots cached for seam jobs and cooks, shells included (not in TotalBytes; G65)
		int64 ShellBytes = 0;          // Of which the neighbours' boundary shells for collision cooks (G69)

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
		// Scatter sub-steps (ScatterMs = edit readers + the scatter manager's Update steps; world modes review G42)
		float ScatterEditMs = 0.0f;     // ProcessEditScatterReaders
		float ScatterApplyMs = 0.0f;    // completed extractions into the caches and the renderer
		float ScatterLaunchMs = 0.0f;   // extraction contexts and launches (with exclusion regrows)
		float ScatterDistanceMs = 0.0f; // the distance spawn: completed passes and the periodic pass
		float ScatterFlushMs = 0.0f;    // the renderer's queued instance adds into the HISMs
		int32 ScatterApplied = 0;       // completed extractions applied this tick
		int32 ScatterLaunched = 0;      // extractions launched this tick
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

		// Meshing sub-phases (MeshingMs = launch + apply)
		float MeshLaunchMs = 0.0f;  // ProcessMeshingQueue: snapshot + slices + dispatch
		float MeshApplyMs = 0.0f;   // ProcessCompletedAsyncMeshes

		// Launch-path breakdown (subsets of MeshLaunchMs)
		float MeshSnapshotMs = 0.0f;  // voxel-data snapshot copy + edit merge
		float MeshDispatchMs = 0.0f;  // LaunchAsyncMeshGeneration (worker handoff)
		int32 MeshLaunchCount = 0;    // mesh jobs launched this tick

		// Seam pipeline (seam-ownership P1+): TickSeamScheduler total — registry scheduling,
		// job drain/dispatch (snapshot lookups), and completed-seam submits to the renderer.
		float SeamMs = 0.0f;

		// Far-chunk compression sweep (ProcessFarCompressionSweep total): the candidate scan, free
		// re-applies, copies for async encodes and installs of finished ones.
		float FarCompressMs = 0.0f;
		int32 FarCompressDispatchCount = 0; // fresh encodes started this tick

		int32 FarCompressApplyCount = 0;    // finished async encodes installed this tick

		// Render-submit breakdown (RenderSubmitMs = mesh drain + unload + water tiles; flush is separate)
		float RenderMeshMs = 0.0f;       // OnChunkMeshingComplete drain loop total
		float RenderSubRendererMs = 0.0f; // subset: renderer UpdateChunkMeshFromCPU (vertex convert + handoff)
		float RenderSubScatterMs = 0.0f; // subset: ScatterManager HandOffChunkVoxels
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

	/**
	 * Where the loaded chunks are, from the streaming viewer (spherical Phase 6, WM-BP2 / WM-BP3): per committed LOD, per
	 * layer (radial on a planet) and how far they reach. vox.Planet reports it as the voxel reach; the bench samples it.
	 * Walks every tracked chunk.
	 * @return The snapshot
	 */
	FVoxelResidencySnapshot GetResidencySnapshot() const;

	/**
	 * The voxel world's report, three lines with no prefix (voxel.World.Stats logs them; vox.Planet and vox.World log them
	 * under their own names and add the pawn's line; code review CR2, FW-18 / CM-34): the world (a planet: centre, radius,
	 * shell, sea, KillZ, orbit radius and sea sphere; a heightmap world: mode, bounds, terrain Z, sea, KillZ and far
	 * ocean), the voxel window's reach from the streaming viewer with the far field (GetResidencySnapshot,
	 * GetFarFieldStats), and the cut-over and descent prefetch.
	 * @param Notes The caller's notes, each at its place in a line (empty for none)
	 * @param OutLines Receives the lines (none without a configuration)
	 */
	void GetWorldReport(const FVoxelWorldReportNotes& Notes, TArray<FString>& OutLines) const;

	/** Chunks generated this session (monotonic; the bench's generated chunks per second). */
	int64 GetTotalChunksGenerated() const { return TotalChunksGenerated; }

	/**
	 * The planet far field's figures (spherical Phase 6, D19), for the bench and vox.Planet.
	 * @return Inactive (all zero) while there is no far field
	 */
	FVoxelFarFieldStats GetFarFieldStats() const { return FarFieldStats; }

	/** Start a deterministic streaming benchmark run (drives the streaming origin, samples each
	 *  frame from TickComponent, and writes a CSV + JSON report on completion). */
	void StartBenchmark(const FVoxelBenchConfig& InConfig);

	/**
	 * Reset benchmark counters (call at the start of a benchmark run). The chunks that already have a mesh stay known, so
	 * their re-meshes count like those of chunks meshed during the run (world modes review G14; `voxel.Bench.CountPreBenchMeshes`).
	 */
	void ResetBenchCounters();

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
	 * The material atlas the renderer draws with (the world actor's MaterialAtlas), or null before Initialize or without
	 * one. The map subsystem builds its palette from it (code review CR1, REND-20).
	 */
	UVoxelMaterialAtlas* GetMaterialAtlas() const;

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
	/** Registers the far field's camera tick (FarFieldCameraTick) with the primary tick, which it follows. */
	virtual void RegisterComponentTickFunctions(bool bRegister) override;

	// ==================== Internal Update Methods ====================

	/**
	 * Build LOD query context from current camera state. The tick moves its viewer by the descent prefetch
	 * (ApplyDescentPrefetch); BuildStreamingContext gives the context with this tick's lead applied.
	 */
	FLODQueryContext BuildQueryContext() const;

	/** BuildQueryContext with the streaming viewer where the tick put it (under the camera by StreamingViewerLead). */
	FLODQueryContext BuildStreamingContext() const;

	/**
	 * Planets: move the context's viewer (the camera) down along local up by the descent prefetch's lead
	 * (ComputeDescentLead, voxel.Stream.DescentLead), from the camera's smoothed radial speed. A teleport resets the speed.
	 */
	void ApplyDescentPrefetch(FLODQueryContext& Context, float DeltaTime);

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
	 * A fresh encode runs on a background worker from a copy of the array;
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
	 * Re-mesh a chunk that just became Loaded if an edit reached it while that mesh was on a worker (world modes review G57,
	 * FVoxelChunkState::bRemeshAfterMeshing).
	 *
	 * @param ChunkCoord The chunk
	 */
	void RemeshIfEditedWhileMeshing(const FIntVector& ChunkCoord);

	/**
	 * Hand a chunk's voxels (edit-merged when it has edits) to scatter for surface extraction, as a mesh completion does.
	 * Scatter reads voxels, never the mesh (world modes review G27).
	 *
	 * @param ChunkCoord The chunk
	 * @param State Its state; its voxel data must be available
	 */
	void HandOffChunkScatter(const FIntVector& ChunkCoord, FVoxelChunkState& State);

	/**
	 * Re-extract the scatter of the chunks whose extraction reads voxels a system or editor edit changed (world modes review
	 * G33), noted by the edit handler since the last tick: each once, skipping those edited themselves (they re-extract with
	 * their re-mesh). Called every tick before the scatter manager's update.
	 */
	void ProcessEditScatterReaders();

	/**
	 * Propagate water flags from loaded neighbor chunks into this chunk.
	 *
	 * Checks 6 face-neighbor chunks for water-flagged voxels at their shared
	 * boundary. Any dry air voxels on this chunk's boundary that are below the sea
	 * (FVoxelSeaSurface: the WaterLevel plane, or a planet's WaterRadius sphere, SP-B8)
	 * and adjacent to a neighbor's water voxel become seeds for an intra-chunk BFS flood fill.
	 *
	 * It reads and writes the generated voxels: water an edit-driven flood put in the edit layer crosses chunk faces
	 * through UVoxelWaterPropagation instead, which scans the faces a chunk shares with its neighbours when its water
	 * settles (world modes review G59).
	 *
	 * The pull runs when the chunk's mesh lands. Its neighbours pull again when its water rises after theirs (G70:
	 * QueueWaterRepulls, ProcessWaterRepulls), so a flood that reaches a chunk only through a neighbour that floods later
	 * arrives whatever order the chunks mesh in.
	 *
	 * @param ChunkCoord The chunk to propagate water into
	 * @param OutFloodedFaces If set, the faces (bit = FaceOffsets index: +X, -X, +Y, -Y, +Z, -Z) on which it flooded voxels
	 * @param OutFloodedVoxels If set, the number of voxels it flooded
	 * @return True if any new water flags were set (caller should regenerate water mesh)
	 */
	bool PropagateWaterFromNeighbors(const FIntVector& ChunkCoord, uint8* OutFloodedFaces = nullptr, int32* OutFloodedVoxels = nullptr);

	/**
	 * Queue the face neighbours across the faces in FaceMask to pull water again (world modes review G70): those that have
	 * pulled once (their water settled) and reach below the sea. voxel.Water.PullAgain 0 queues nothing (the rule before).
	 *
	 * @param ChunkCoord The chunk whose water rose
	 * @param FaceMask The faces to queue across (bit = FaceOffsets index: +X, -X, +Y, -Y, +Z, -Z)
	 */
	void QueueWaterRepulls(const FIntVector& ChunkCoord, uint8 FaceMask);

	/**
	 * Run queued second pulls (G70) within about a millisecond a tick, at least one: each Loaded chunk pulls from its
	 * neighbours again, refreshes its water tile if it flooded, and queues the neighbours across the faces it flooded.
	 */
	void ProcessWaterRepulls();

	// ==================== 2D Water Tile System ====================

	/**
	 * Whether the per-chunk water tiles run: water on with a WaterMeshMaterial, in a heightmap world. They are XY
	 * columns meshed at the WaterLevel plane, so a planet (whose sea is a sphere) has none until SP-E2.
	 *
	 * @return True when water tiles are built, updated and drawn
	 */
	bool UsesWaterTiles() const;

	/**
	 * Scan a chunk's voxel data and update its water tile contribution.
	 * Called after meshing completes for a 3D chunk. A chunk with edits is scanned edit-merged (GetVoxelSnapshot), so
	 * the water an edit-driven flood put in the edit layer draws its tile (world modes review G59).
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
	 * @return The generation requests evicted
	 */
	int32 ReprioritizeQueues(const FLODQueryContext& Context);

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
	struct FVoxelSnapshotEntry
	{
		uint32 ContentVersion = 0;
		uint64 LastUsedTick = 0;
		TSharedPtr<const TArray<FVoxelData>> Data;
		/**
		 * The decode of a far-compressed chunk's snapshot on a worker (G65): Data is filled once it completes. Invalid
		 * (nothing to wait for) for a snapshot copied from a resident array.
		 */
		UE::Tasks::FTask Ready;

		// Instrumentation (G69): how the snapshot was made, when, how often it was read again and by which readers.
		uint8 Kind = 0;                       // ESnapshotKind
		uint64 CreatedTick = 0;
		int32 Hits = 0;                       // reads after the one that made it
		const TCHAR* FirstReason = nullptr;   // VoxelResidencyStats read reason that made it
		const TCHAR* LastReason = nullptr;    // and the last one that read it
		uint8 ReaderMask = 0;                 // ESnapshotReader bits of every reader that took it
	};
	TMap<FIntVector, FVoxelSnapshotEntry> VoxelSnapshotCache;

	/** Who read a cached snapshot (G69 instrumentation; FVoxelSnapshotEntry::ReaderMask). */
	enum ESnapshotReader : uint8
	{
		SnapReaderCollisionOwn = 1 << 0,
		SnapReaderCollisionNeighbor = 1 << 1,
		SnapReaderSeamJob = 1 << 2,
		SnapReaderChunkMesh = 1 << 3,
		SnapReaderPrefetch = 1 << 4,
		SnapReaderWater = 1 << 5,
		SnapReaderScatter = 1 << 6,
		SnapReaderOther = 1 << 7,
	};

	/** How a cached snapshot was made (G69). */
	enum class ESnapshotKind : uint8
	{
		Copied = 0,             // copied from a resident chunk's array
		CopiedWithEdits = 1,    // copied, the chunk's edits applied
		DecodedCompressed = 2,  // decoded on a worker from a far-compressed chunk (G65)
		DecodedUniform = 3,     // expanded on a worker from a uniform chunk (G65; voxel.Snapshot.ShareUniform 0)
		SharedUniform = 4,      // a uniform chunk's, the shared array for its value (G69)
		SharedResident = 5,     // a resident chunk's own array, shared copy-on-write (G71; voxel.Snapshot.ShareResident)
		Num = 6
	};

	/** The snapshot cache's session counters (G69; voxel.Snapshot.Stats). */
	struct FSnapshotCacheCounters
	{
		int64 Made[static_cast<int32>(ESnapshotKind::Num)] = {};
		int64 Hits = 0;
		int64 Replaced = 0;          // made again for a newer ContentVersion
		int64 EvictedByAge = 0;
		int64 EvictedByAgeUnread = 0; // evicted without a single read after the one that made it
		int64 EvictedAgeTicksSum = 0; // their lifetimes, for the mean
		int64 RemovedOnUnload = 0;
		int64 RemovedOnWater = 0;
		int64 RemadeAfterEviction = 0; // made again within 600 ticks of its eviction by age
		int64 ReleasedAfterCook = 0;   // a cook's own snapshot dropped once the cook took it, no other reader having (G69)
	};

	/**
	 * One immutable array per uniform value (G69): the snapshot of every uniform chunk with that value shares it, so an
	 * all-air or all-rock chunk costs its cache entry nothing. Keyed by the packed voxel; counted once in the memory stats.
	 */
	TMap<uint32, TSharedPtr<const TArray<FVoxelData>>> UniformSnapshotArrays;

	/** The shared array for a uniform chunk's snapshot (G69), made on first use. */
	TSharedPtr<const TArray<FVoxelData>> GetUniformSnapshotArray(const FVoxelData& Value, int32 TotalVoxels);

	/**
	 * The memory a cache entry holds of its own: none for a uniform value's shared array (counted once) or for a resident
	 * chunk's array it shares while the chunk still holds it (counted in the voxel data, G71), the chunk's full size while
	 * it decodes, else its array.
	 *
	 * @param Coord The entry's chunk
	 * @param Snap  The entry
	 * @return Bytes
	 */
	int64 GetSnapshotOwnBytes(const FIntVector& Coord, const FVoxelSnapshotEntry& Snap) const;

	/** How a cached boundary shell was made (G69). */
	enum class EFaceSlabsKind : uint8
	{
		FromResident = 0,       // cut from a resident chunk's array on the game thread (the faces only)
		FromSnapshot = 1,       // cut from the chunk's cached full snapshot (edit-merged), on a worker
		DecodedCompressed = 2,  // a far-compressed chunk decoded on a worker, only the shell kept
		SharedUniform = 3,      // a uniform chunk's: the shared shell for its value and depth
		Num = 4
	};

	/**
	 * A neighbour's boundary shell for collision cooks (G69): the voxels a cook reads from it, within the cook's
	 * neighbour depth of each face, instead of its whole array. Keyed per chunk; valid for one ContentVersion and any
	 * depth up to Depth; dropped with the chunk's snapshot (unload, the water pull's flags) and by the same age sweep.
	 */
	struct FChunkFaceSlabsEntry
	{
		uint32 ContentVersion = 0;
		int32 Depth = 0;
		uint64 LastUsedTick = 0;
		TSharedPtr<const FVoxelFaceSlabs> Shell;
		/** The worker task that fills Shell (cut from a decoding snapshot or a compressed chunk); invalid when built. */
		UE::Tasks::FTask Ready;
		uint8 Kind = 0;   // EFaceSlabsKind
		int32 Hits = 0;
	};
	TMap<FIntVector, FChunkFaceSlabsEntry> FaceSlabsCache;

	/** One shell per uniform value and depth (G69), shared like UniformSnapshotArrays. Key: packed voxel | depth << 32. */
	TMap<uint64, TSharedPtr<const FVoxelFaceSlabs>> UniformFaceSlabs;

	/** Shells made per kind and cache hits this session (G69; voxel.Snapshot.Stats). */
	int64 FaceSlabsMade[static_cast<int32>(EFaceSlabsKind::Num)] = {};
	int64 ShellHits = 0;

	/**
	 * A chunk's boundary shell of at least Depth planes for a collision cook (G69; game thread). A far-compressed chunk
	 * without edits is decoded on a worker and only its shell kept (OutReady receives that task); a resident one is cut
	 * here from its array (the faces only); a chunk with edits is cut from its edit-merged snapshot.
	 *
	 * @param Coord    The chunk
	 * @param State    Its state
	 * @param Depth    Planes needed next to each face
	 * @param OutReady If set, receives the task still filling the shell (invalid when it is built); else this waits
	 * @return The shell (being filled while OutReady is pending), or null if the chunk has no data
	 */
	TSharedPtr<const FVoxelFaceSlabs> GetChunkFaceSlabs(const FIntVector& Coord, FVoxelChunkState& State, int32 Depth,
		UE::Tasks::FTask* OutReady);

	/** Store a shell just made in FaceSlabsCache and count it (G69). */
	TSharedPtr<const FVoxelFaceSlabs> StoreChunkFaceSlabs(const FIntVector& Coord, uint32 Version, int32 Depth,
		TSharedPtr<const FVoxelFaceSlabs> Shell, UE::Tasks::FTask Ready, EFaceSlabsKind Kind);

	/** Chunks whose snapshot the age sweep evicted, and when (pruned after 600 ticks; G69 instrumentation). */
	TMap<FIntVector, uint64> RecentlyEvictedSnapshots;
	FSnapshotCacheCounters SnapshotCounters;

	/** Stamp a snapshot just made with its kind, tick and reader, and count it (G69). */
	void NoteSnapshotMade(const FIntVector& Coord, FVoxelSnapshotEntry& Entry, ESnapshotKind Kind);

	/** Snapshots of far-compressed chunks decoded on workers this session (G65). */
	int64 SnapshotDecodeTasks = 0;

	/** Chunks collapsed to uniform as they were generated (G67). */
	int64 UniformAtGenerationCount = 0;

	/** Monotonic TickSeamScheduler counter (drives the snapshot age sweep). */
	uint64 SeamTickCounter = 0;

	/**
	 * Dedicated mesher for seam jobs (created lazily when the pipeline activates), matching
	 * the configured meshing mode (CPU DC for Dual Contouring configs, CPU MC for Marching
	 * Cubes — P3, cubic since M5). Seam strips are tiny, so meshing them is cheap; a dedicated
	 * instance keeps their Config stable.
	 */
	TUniquePtr<IVoxelMesher> SeamMesher;

	/** Lazily create (mode + config matched to the main mesher) and return the CPU seam mesher. */
	IVoxelMesher* EnsureSeamMesher();

	/**
	 * Chunk-mesh and seam jobs on workers that hold a raw pointer to Mesher or SeamMesher (each counts itself from dispatch
	 * to its end): Shutdown waits for them before the meshers go (G76).
	 */
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> MesherWorkerJobs = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);

public:
	/** Seam registry access for diagnostics (performance HUD, tests). May be null. */
	const FVoxelSeamRegistry* GetSeamRegistry() const { return SeamRegistry.Get(); }

private:

	/**
	 * Local up at a chunk's centre: +Z in heightmap modes, the radial direction on a planet. The meshing and seam requests
	 * carry it (FVoxelMeshingRequest::LocalUp; spherical SP-D1, SP-D2).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Unit up vector
	 */
	FVector GetChunkLocalUp(const FIntVector& ChunkCoord) const;

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

	/**
	 * Get (building at most once per content version) the shared edit-merged snapshot of a chunk. The snapshot of a
	 * far-compressed chunk without edits is decoded by a worker task (G65): with OutReady the caller receives that task and
	 * waits for it on its own worker before reading the data; without, the game thread waits here.
	 *
	 * @param Coord    Chunk coordinate
	 * @param State    Its state
	 * @param OutReady Receives the decode to wait for (invalid when the data is ready); nullptr to wait here
	 * @return The snapshot (filled once OutReady completes)
	 */
	TSharedPtr<const TArray<FVoxelData>> GetVoxelSnapshot(const FIntVector& Coord, FVoxelChunkState& State,
		UE::Tasks::FTask* OutReady = nullptr);

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
	 * Flag to force streaming update on next tick.
	 * Set by ForceStreamingUpdate(), cleared after use.
	 */
	bool bForceStreamingUpdate = false;

	/** When EvaluateLODLevelChanges runs (viewer movement, cap left-overs, idle sweep); see FVoxelLODEvalSchedule. */
	FVoxelLODEvalSchedule LODEvalSchedule;

	// ==================== Statistics ====================

	/** Total chunks generated this session */
	int64 TotalChunksGenerated = 0;

	/** Total chunks meshed this session */
	int64 TotalChunksMeshed = 0;

	/** Total chunks unloaded this session */
	int64 TotalChunksUnloaded = 0;

	/** System and editor edits' scatter work (GetEditScatterStats). */
	FEditScatterStats EditScatterStats;

	/** Meshing work and what edits asked of it (GetMeshWorkStats). */
	FMeshWorkStats MeshWorkStats;

	/** Chunks to pull water again, in the order queued, and the set of them (G70; ProcessWaterRepulls). */
	TArray<FIntVector> WaterRepullQueue;
	TSet<FIntVector> WaterRepullSet;

	/** The water pull's second passes (GetWaterRepullStats). */
	FWaterRepullStats WaterRepullStats;

	/** Chunks a system or editor edit changed since the last ProcessEditScatterReaders (their scatter re-extracts with their re-mesh). */
	TSet<FIntVector> EditScatterEditedChunks;

	/** Chunks whose extraction reads voxels a system or editor edit changed since the last ProcessEditScatterReaders. */
	TSet<FIntVector> EditScatterReaderChunks;

	// ==================== Generation Components ====================

	/** Voxel data generator — CPU (FVoxelCPUNoiseGenerator) or GPU (FVoxelGPUNoiseGenerator) per config. */
	TUniquePtr<IVoxelNoiseGenerator> NoiseGenerator;

	/** Non-owning alias to NoiseGenerator when GPU generation is active (for the poll-based readback API). */
	FVoxelGPUNoiseGenerator* GPUGeneratorPtr = nullptr;

	/** True when bUseGPUGeneration resolved on (config flag && !-VoxelForceCPU && voxel.Stream.GPUGeneration). */
	bool bUseGPUGenerationActive = false;

	/** Analytic world mode matching the config's WorldMode (FVoxelWorldModeFactory::Create) */
	TSharedPtr<IVoxelWorldMode> WorldMode;

	/** Static terrain conditioning zones (gen-time flattening under POIs/claims; Phase 6c). */
	TArray<FVoxelConditioningZone> ConditioningZones;

	/** Optional game-supplied dynamic conditioner queried per chunk (not owned). */
	IVoxelTerrainConditioner* TerrainConditioner = nullptr;

	/** Collect conditioning zones overlapping a chunk's XY footprint into OutZones (game thread). */
	void GatherConditioningZonesForChunk(const FIntVector& ChunkCoord, TArray<FVoxelConditioningZone>& OutZones) const;

	/**
	 * Build the generation request the streaming sends for a chunk (world mode, biomes, caves, water, conditioning zones).
	 *
	 * @param ChunkCoord Chunk to generate
	 * @param LODLevel LOD of the request (0 when LOD is off)
	 * @param OutRequest Filled request
	 */
	void BuildChunkGenerationRequest(const FIntVector& ChunkCoord, int32 LODLevel, FVoxelNoiseGenerationRequest& OutRequest) const;

	/** Append conditioning zones (static + dynamic) whose influence overlaps a world-space XY region. */
	void GatherConditioningZonesForRegion(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const;

	/** Append conditioning zones (static + dynamic) whose influence covers a single world-space XY point. */
	void GatherConditioningZonesForPoint(double WorldX, double WorldY, TArray<FVoxelConditioningZone>& OutZones) const;

	/**
	 * The generated column height (GetTerrainHeightAt plus the conditioning zones there) at a sample point, as the heightmap
	 * generators compute it for a voxel column. GetGeneratedSurfacePoint's Z in smooth worlds.
	 */
	float GetGeneratedColumnHeight(double WorldX, double WorldY) const;

	/**
	 * QueryEditMergedSurface in a block world (FVoxelBlockSurface::UsesBlockSurface; WM-C5, KC6): the top face of the
	 * topmost ground voxel with no ground above it in the edit-merged column of the block containing (X, Y). Leaves of the
	 * configured voxel trees are not ground (a canopy is walked under, not on), a trunk is. The normal and slope are the
	 * edit-merged density gradient's, the terrain's slope, as in smooth worlds.
	 */
	bool QueryEditMergedBlockSurface(double WorldX, double WorldY, float EstimateZ,
		float& OutHeight, FVector& OutNormal, float& OutSlopeDegrees, uint8& OutMaterialID, uint8& OutBiomeID) const;

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
		/** Every voxel is UniformValue: found on the worker, so the chunk collapses as it is stored (G67). */
		bool bUniform = false;
		FVoxelData UniformValue;
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
	void LaunchPostReadbackProcessing(const FIntVector& ChunkCoord, FVoxelNoiseGenerationRequest GenRequest, TArray<FVoxelData> VoxelData, uint32 DispatchSerial,
		bool bUniform = false, const FVoxelData& UniformValue = FVoxelData());

	/** Whether GPU-generated chunks get voxel trees injected after the readback (cubic heightmap worlds with tree templates). */
	bool WillInjectTreesAfterReadback() const;

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
		/** The mesher's time on the worker (CPU meshers; 0 for a GPU mesher). */
		double WorkerMs = 0.0;
	};

	/** Thread-safe queue for completed async mesh results */
	TQueue<FAsyncMeshResult, EQueueMode::Mpsc> CompletedMeshQueue;

	/** Chunks currently being meshed asynchronously, with their dispatch stamps */
	TMap<FIntVector, FAsyncJobStamp> AsyncMeshingInProgress;

	/** Note: MaxAsyncMeshTasks is now in VoxelWorldConfiguration; runtime value in EffectiveMaxAsyncMeshTasks */

	/** Process completed async mesh tasks (called from game thread) */
	void ProcessCompletedAsyncMeshes();

	/** Launch async mesh generation for a chunk */
	void LaunchAsyncMeshGeneration(const FChunkLODRequest& Request, FVoxelMeshingRequest MeshRequest,
		TSharedPtr<const TArray<FVoxelData>> DeferredVoxels = nullptr, UE::Tasks::FTask DeferredDecode = UE::Tasks::FTask());

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

	// ==================== Far Ocean (WM-E1) ====================

	/** The far ocean's meshes (owned by this component's actor): the seabed (section 0, terrain material) and the open sea
	 *  (section 1, water; section 2 its underside). Inside the footprint's box the far field draws the land and the sea. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> FarOceanStatic;

	/** What the far ocean was built with */
	FVoxelFarOceanSettings FarOceanSettings;

	/** Whether the water's undersides (sections 2) are shown: only while the viewer is under the water plane */
	bool bFarOceanUndersideShown = false;

	/** Show the water's undersides when the viewer is under the water plane, hide them when it is not. */
	/** A sea underside's visibility: shown while the viewer is under it, applied only when that changes (CM-19). */
	static void UpdateUnderside(bool bBelow, bool& bShown, TFunctionRef<void(bool)> Apply);

	void UpdateFarOceanUnderside(const FVector& ViewerPosition);

	/** Build the far ocean from the world mode's bounds and the island's seabed (game worlds, islands, bIslandFarOcean). */
	void RebuildFarOcean();

	/** Destroy the far ocean's component. */
	void DestroyFarOcean();

	/** The voxel material generation gives the seabed beyond the edge (the most common one around it). */
	uint8 ResolveFarSeabedMaterial(const FIslandBowlParams& Island) const;

	// ==================== Planet Ocean (spherical SP-E2) ====================

	/**
	 * A planet's sea surface (FVoxelPlanetOcean): section 0 the top (faces outward), section 1 the underside (inward),
	 * shown only while the viewer is inside the sea radius. Owned by this component's actor, positioned at the planet
	 * centre; built once when the world starts.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> PlanetOcean;

	/** What the planet ocean was built with */
	FVoxelPlanetOceanSettings PlanetOceanSettings;

	/** Whether the planet ocean's underside (section 1) is shown: only while the viewer is inside the sea radius */
	bool bPlanetOceanUndersideShown = false;

	/**
	 * Build the planet ocean (game worlds, planets with water on and a WaterMeshMaterial), unless the far field draws the
	 * sea (at scale).
	 */
	void RebuildPlanetOcean();

	/** Destroy the planet ocean's component. */
	void DestroyPlanetOcean();

	/** Show the planet ocean's underside while the viewer is inside the sea radius, hide it outside. */
	void UpdatePlanetOceanUnderside(const FVector& ViewerPosition);

	/** Planets: move the world settings' KillZ below the planet when it would cut through it (spherical SP-F5). */
	void ApplyPlanetKillZ();

	// ==================== The far field (spherical Phase 6 WM-BP9; every world mode since MP3) ====================

	/** The far field's patches and their builds (pure; this component owns their mesh components) */
	TUniquePtr<FVoxelFarField> FarField;

	/**
	 * One procedural mesh component per built patch, at the patch's origin (owned by this component's actor): section 0
	 * the terrain (the terrain material), 1 the sea's top and 2 its underside (the water material). Sections are shown
	 * and hidden as the far field selects (a render command, no proxy rebuild); a component goes with its patch.
	 */
	UPROPERTY(Transient)
	TMap<uint64, TObjectPtr<UProceduralMeshComponent>> FarFieldComponents;

	/** Patches this many levels coarser than the edge level or finer get the sea's underside (seen only close by) */
	static constexpr int32 FarFieldUndersideLevels = 4;

	/** Vertices and indices each component holds (its memory estimate) */
	TMap<uint64, FIntPoint> FarFieldComponentSizes;
	int64 FarFieldComponentVertices = 0;
	int64 FarFieldComponentIndices = 0;

	/** The last update's figures (GetFarFieldStats) */
	FVoxelFarFieldStats FarFieldStats;

	/** The viewer's ground radius (one terrain sample when the viewer moves) and the position it was taken at */
	double FarFieldGroundRadius = 0.0;
	FVector FarFieldGroundViewer = FVector(TNumericLimits<double>::Max());

	/** The camera and streaming viewer of the last far-field update (UpdateFarFieldForCamera compares this frame's camera) */
	FVector FarFieldLastCamera = FVector::ZeroVector;
	FVector FarFieldLastViewer = FVector::ZeroVector;
	bool bFarFieldHasLastCamera = false;

	/** Far-field updates run again for a frame's camera after a cut (F4.3), in all */
	int64 FarFieldCameraUpdates = 0;

	/** Streaming updates run again for a frame's camera after a cut (WM-MP4), in all */
	int64 CameraCutUpdates = 0;

	/** Runs TickAfterCameraUpdate in TG_PostUpdateWork, after the player camera managers (F4.3, WM-MP4) */
	UPROPERTY()
	FVoxelFarFieldCameraTickFunction FarFieldCameraTick;

	/** Whether the sea's underside sections are shown (the viewer is inside the sea radius) */
	bool bFarFieldUndersideShown = false;

	/**
	 * Bumped whenever a chunk changes state or leaves: the far field recomputes which edge patches the voxel chunks
	 * cover only when it moved.
	 */
	int64 ChunkStateVersion = 0;

	/**
	 * Build the far field (game worlds, planets; bEnabled from voxel.World.FarField). Before RebuildPlanetOcean: it decides
	 * the sea.
	 */
	void RebuildFarField(bool bEnabled);

	/** Drop the far field and destroy its components. */
	void DestroyFarField();

	/**
	 * One far-field update: select, build, apply to the components; fills FarFieldStats. The camera measures the screen
	 * error and the horizon; the streaming viewer (under the camera while it descends, WM-BP10) centres the hole.
	 */
	void UpdateFarField(const FVector& CameraPosition, const FVector& ViewerPosition, float FieldOfView);

	/** Whether a voxel chunk's mesh is on screen: loaded, or re-meshing with its old mesh still drawn. */
	bool IsChunkDrawnForFarField(const FIntVector& ChunkCoord) const;

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

/** Latest viewer world position, cached each tick for the stale-cull distance test. */
	FVector CurrentViewerPosition = FVector::ZeroVector;

	/** Previous-tick viewer position for the speed estimate (FLT_MAX until first tick). */
	FVector LastViewerPosForSpeed = FVector(FLT_MAX);

	/**
	 * Smoothed viewer speed (uu/s) that drives the speed-adaptive load budget: horizontal in heightmap worlds, 3D on a
	 * planet, where motion along any axis (a climb at a pole, a flight north) brings in new chunks (spherical Phase 6).
	 */
	float ViewerLoadSpeed = 0.0f;

	/** The camera this tick, before the descent prefetch moves the streaming viewer under it (spherical Phase 6, WM-BP10). */
	FVector CurrentCameraPosition = FVector::ZeroVector;

	/** The camera's last position and its smoothed speed along local up (uu/s, negative descending), for the prefetch. */
	FVector LastPrefetchCamera = FVector(FLT_MAX);
	double CameraRadialSpeed = 0.0;

	/** How far under the camera the streaming viewer is this tick (uu along local down; 0 = at the camera). */
	double StreamingViewerLead = 0.0;

	/** Local down at the camera on the tick the lead was computed (toward a planet's centre; -Z on a heightmap world). */
	FVector StreamingViewerDown = -FVector::UpVector;

	/** The window's cut-over state on the last tick (IVoxelLODStrategy::IsWindowSuspended), to act when it changes. */
	bool bWasWindowSuspended = false;

	/** The cut-over transitions (GetCutoverTransitions), their count, and the last resume's time and counters (-1: none). */
	TArray<FVoxelCutoverTransition> CutoverTransitions;
	int64 CutoverTransitionCount = 0;
	double CutoverClock = 0.0; // seconds of streaming ticks since the world started (the transitions' Time)
	double LastWindowResumeTime = -1.0;
	int64 GeneratedAtWindowResume = 0;
	int64 MeshedAtWindowResume = 0;

	/** Smoothed frame time for stable throttle decisions (EMA) */
	float SmoothedFrameTimeMs = 16.67f;

	/**
	 * Lowest frame time observed recently (ms), drifting slowly upward. Floors the adaptive
	 * throttle's budget so its release test stays reachable: on a vsync-capped display the
	 * frame time can never drop below the present interval, and a TargetFrameRate faster than
	 * that would otherwise make the throttle a one-way latch for the whole session.
	 */
	float ObservedFrameFloorMs = 16.67f;

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

	/**
	 * Chunks with a mesh submitted since they were loaded (to detect re-mesh churn): filled at every mesh submission,
	 * emptied of a chunk when its state is removed. Kept outside a benchmark too, so a run counts the re-meshes of
	 * chunks meshed before it started (G14: a bench started after a slower, cold PIE start had meshed fewer of them and
	 * counted about twice as many LOD re-meshes). `voxel.Bench.CountPreBenchMeshes` 0 empties it at the run's start.
	 */
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
