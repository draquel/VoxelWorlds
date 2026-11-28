// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelFaceSlabs.h"
#include "Containers/Queue.h"
#include "Components/PrimitiveComponent.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "VoxelCollisionManager.generated.h"

struct FVoxelTickBudget;
class IVoxelMesher;
class FVoxelMaterialTable;

class UVoxelWorldConfiguration;
class UVoxelChunkManager;
class UBodySetup;

/**
 * Lightweight collision-only component backed by a custom BodySetup.
 *
 * Unlike hacking a UBoxComponent's FBodyInstance, this component properly
 * overrides GetBodySetup() so the physics engine always uses our trimesh
 * BodySetup for sweeps, traces, and overlap queries.
 *
 * Static: it is placed at its chunk's corner before it is registered and replaced, never moved, when the chunk is
 * re-cooked. A character standing on it is therefore on a static movement base: it sends and receives world positions,
 * which is what lets a client and a server agree about a pawn on terrain though each has its own copy of this
 * component (the container actor is not replicated; G215).
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

	/** A hash of the last applied cook's triangle mesh (voxel.Collision.CookAudit, G283): a re-cook compares its own with it. */
	uint64 LastCookHash = 0;

	/** LastCookHash holds the last applied cook's hash */
	bool bHasCookHash = false;

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

	/** When the chunk was first queued (FPlatformTime::Seconds): the neighbour wait's cap counts from it (G283, D196) */
	double QueuedSeconds = 0.0;

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

/**
 * A further focus of the collision shell (G217): on a server, the pawn of a player other than the one the streaming
 * follows. The chunk manager hands the manager this tick's (UVoxelCollisionManager::SetFurtherFoci); nothing is kept per
 * player.
 */
struct FVoxelCollisionFocus
{
	/** The pawn's position */
	FVector Position = FVector::ZeroVector;

	/** The pawn's velocity: the shell's lead and the path coverage read it */
	FVector Velocity = FVector::ZeroVector;
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
	/** What the cook was (EVoxelCollisionCookKind as a byte, G283) */
	uint8 Kind = 0;
	/** The voxels the cook read hold no surface (PredictNoSurface; computed when the skip or the audit is on, G283) */
	bool bPredictedEmpty = false;
	/** bPredictedEmpty was worked out */
	bool bPredictionComputed = false;
	/** The mesher was not run: the prediction said no surface and the skip is on (voxel.Collision.SkipNoSurface) */
	bool bSkippedMesh = false;
	/** A hash of the triangle mesh (voxel.Collision.CookAudit) */
	uint64 Hash = 0;
	bool bHasHash = false;
	/** The cook's stages on its worker, in milliseconds (voxel.Collision.CookStats, G275). */
	float WaitMs = 0.0f;
	float CopyMs = 0.0f;
	float SlicesMs = 0.0f;
	float MeshMs = 0.0f;
	float TriMeshMs = 0.0f;
	float TotalMs = 0.0f;
	/** Where the chunk's drawn mesh and its neighbours' stood at the launch (G296's measure; EVoxelCollisionOwnMesh as a byte) */
	uint8 ReachOwn = 3;
	uint8 ReachNeighborsDrawn = 0;
	uint8 ReachNeighborsTarget = 0;
	uint8 ReachNeighborsWithVoxels = 0;
	/** Seconds since the chunk's mesh was submitted, when it was drawn at the cook's LOD */
	float ReachOwnMeshAgeSeconds = 0.0f;
	/** Seconds from the request's queueing to its launch */
	float QueueWaitSeconds = 0.0f;
};

/** What a collision cook is (G283): a chunk's first, a re-cook for a neighbour's arrival, or another re-cook (an edit). */
enum class EVoxelCollisionCookKind : uint8
{
	First,
	NeighborRecook,
	OtherRecook,
};

/** A voxel's side of the surface as every mesher reads it, or both for a volume or region (G283). */
enum class EVoxelSurfaceSide : uint8
{
	Air,
	Solid,
	Mixed,
};

/**
 * What a collision cook holds of one of its 26 neighbours (G283): the neighbour's face slabs, its whole voxels, or
 * neither when it had no voxel data (read as air).
 */
struct FVoxelCollisionNeighborView
{
	/** The neighbour's face slabs (the cook's usual input, G69) */
	const FVoxelFaceSlabs* Shell = nullptr;

	/** The neighbour's whole ChunkSize^3 voxels (when the cook's depth is too deep for slabs) */
	const TArray<FVoxelData>* Full = nullptr;

	/** @return True when the cook had nothing of the neighbour */
	bool IsMissing() const { return Shell == nullptr && Full == nullptr; }

	/**
	 * Read the neighbour's voxel at its own chunk-local position, as the cook reads it.
	 *
	 * @param X, Y, Z The position (each 0..ChunkSize-1)
	 * @param ChunkSize Voxels per chunk axis
	 * @return The voxel; air when missing or not held
	 */
	FVoxelData Get(int32 X, int32 Y, int32 Z, int32 ChunkSize) const
	{
		if (Shell)
		{
			return Shell->Get(X, Y, Z);
		}
		if (Full)
		{
			const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
			return Full->IsValidIndex(Index) ? (*Full)[Index] : FVoxelData::Air();
		}
		return FVoxelData::Air();
	}
};

/** What the collision cooks cost on their workers since the last reset, summed per stage (voxel.Collision.CookStats, G275). */
struct FVoxelCollisionCookStats
{
	/** Distance bands of the first collision's arrival, in chunk widths from the nearest focus: [0,1), [1,2), [2,3), [3,5), 5 and more. */
	static constexpr int32 NumBands = 5;

	int64 Cooks = 0;
	int64 EmptyCooks = 0;
	int64 Triangles = 0;
	/** The cooks by kind (G283) */
	int64 FirstCooks = 0;
	int64 NeighborRecooks = 0;
	int64 OtherRecooks = 0;
	/** First cooks that found no surface */
	int64 FirstEmpty = 0;
	/** Re-cooks whose triangle mesh hashed the same as the one it replaced (voxel.Collision.CookAudit) */
	int64 NeighborRecooksSame = 0;
	int64 OtherRecooksSame = 0;
	/** Cooks the prediction said had no surface; of them, the mesh not run (the skip) */
	int64 PredictedEmpty = 0;
	int64 SkippedOnWorker = 0;
	/** Chunks ready-empty without a cook launched: the chunk and its neighbours uniform on one side (G283) */
	int64 SkippedAtLaunch = 0;
	/** The audit's disagreements: predicted empty but triangles, and empty but not predicted (the second is allowed) */
	int64 PredictedEmptyWrong = 0;
	int64 EmptyNotPredicted = 0;
	/** Launches the neighbour wait held back (a tick each), and the chunks it held at least once (G283, D196) */
	int64 NeighborWaitHolds = 0;
	int64 NeighborWaitChunks = 0;
	/** When the stats were reset (FPlatformTime::Seconds) */
	double ResetSeconds = 0.0;
	/** A chunk's first collision (a body or ready-empty) per distance band: how many, and the seconds after the reset (sum, last) */
	int64 BandFirstReady[NumBands] = {};
	double BandFirstReadySumSeconds[NumBands] = {};
	double BandFirstReadyLastSeconds[NumBands] = {};
	/**
	 * G296's measure (the reach line): the cooks that found a surface, [0] first cooks and [1] re-cooks, by where the chunk's own
	 * drawn mesh stood against the cook's LOD at the launch (EVoxelCollisionOwnMesh), how many also had every neighbour with
	 * voxels drawn at that LOD (the share a cook from the drawn mesh and seam meshers could take at once; a neighbour never
	 * loaded, above or below a heightmap's slab, is read as air either way) or to be drawn there (the share it could take
	 * after waiting for meshes), the drawn mesh's age at the launch, and the wait in the queue.
	 */
	static constexpr int32 NumReachOwn = 4;
	int64 ReachCooks[2] = {};
	int64 ReachOwn[2][NumReachOwn] = {};
	int64 ReachAllDrawn[2] = {};
	int64 ReachAllTarget[2] = {};
	double ReachOwnAgeSumSeconds[2] = {};
	double ReachOwnAgeMaxSeconds[2] = {};
	double QueueWaitSumSeconds[2] = {};
	double QueueWaitMaxSeconds[2] = {};
	/** Cooks launched while the chunk's mesh at the cook's LOD was coming, whose mesh was then submitted: how many and the seconds after the launch */
	int64 MeshAfterCook = 0;
	double MeshAfterCookSumSeconds = 0.0;
	/** Waiting for far-compressed snapshots to decode. */
	double WaitMs = 0.0;
	/** The chunk's voxels copied into the meshing request. */
	double CopyMs = 0.0;
	/** The neighbours' boundary slices extracted. */
	double SlicesMs = 0.0;
	/** The chunk meshed at the collision LOD. */
	double MeshMs = 0.0;
	/** The Chaos triangle mesh built from it. */
	double TriMeshMs = 0.0;
	double TotalMs = 0.0;
	double MaxTotalMs = 0.0;
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
	 * The cook priority of a dirty chunk at a distance: an edit's above a first cook's at the same distance, a
	 * neighbour arrival's re-cook below every first cook's (FChunkCollisionData::bNeighborRecookOnly), or with
	 * voxel.Collision.RecookNearChunks N > 0 a first cook's at that distance within N chunk widths of a focus (D201, G299).
	 *
	 * @param Data The chunk's collision state (bNeedsUpdate set)
	 * @param Distance The chunk centre's distance from the nearest focus
	 * @return The priority to request it at
	 */
	float GetDirtyPriority(const FChunkCollisionData& Data, float Distance) const;

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
	 * @param Budget The chunk manager's tick budget (G282): the components of chunks that left the shell are destroyed
	 *        within it, the first each tick always (null: all at once, as before)
	 */
	void Update(const FVector& ViewerPosition, float DeltaTime, const FVoxelTickBudget* Budget = nullptr);

	/** @return Collision components of chunks that left the shell still waiting to be destroyed (G282) */
	int32 GetPendingDestroyCount() const { return PendingDestroys.Num(); }

	/** @return Jumps of the focus taken as cuts rather than speed in all (G282, voxel.Collision.JumpIsCut) */
	int64 GetFocusJumps() const { return TotalFocusJumps; }

	/**
	 * The shell's further foci for the next Update (G217): the pawns of a server's other players. A chunk gets collision
	 * within the radius of any focus (the first player's pawn, as before, and each of these: GetFocusRadius), is cooked
	 * in the order of its distance from the nearest one, and loses it once outside every one's. The chunk manager sets
	 * them each tick from the players the world has then, so a player who leaves releases its shell at the next sweep.
	 *
	 * @param InFoci This tick's further foci (empty on a client and in a standalone game)
	 */
	void SetFurtherFoci(const TArray<FVoxelCollisionFocus>& InFoci) { FurtherFoci = InFoci; }

	/** @return The further foci the last SetFurtherFoci gave */
	const TArray<FVoxelCollisionFocus>& GetFurtherFoci() const { return FurtherFoci; }

	/**
	 * The radius of the shell around a further focus moving at a velocity: the configured radius plus the lead its
	 * horizontal speed adds (voxel.Collision.LeadSeconds), as the first focus's (GetCurrentEffectiveRadius), unsmoothed.
	 *
	 * @param Velocity The focus's velocity
	 * @return The radius (uu), never under GetCollisionRadius
	 */
	float GetFocusRadius(const FVector& Velocity) const;

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
	 * Called by the chunk manager.
	 *
	 * @param ChunkCoord The chunk whose voxel data arrived
	 * @return The number of cooked neighbours that read this chunk as air
	 */
	int32 OnChunkVoxelDataAvailable(const FIntVector& ChunkCoord);

	/**
	 * The chunk manager submitted a chunk's mesh (G296's measure): a cook launched while that mesh was coming counts the
	 * seconds it came after the cook (voxel.Collision.CookStats' reach line).
	 *
	 * @param ChunkCoord The chunk
	 * @param LODLevel   The submitted mesh's LOD
	 */
	void NoteChunkMeshSubmitted(const FIntVector& ChunkCoord, int32 LODLevel);

	/**
	 * The bit of a neighbour offset in FChunkCollisionData::MissingNeighborMask.
	 *
	 * @param Offset A neighbour's offset from the chunk, each component in -1..1, not all zero
	 * @return Its bit (one of 26), or 0 for an offset that is not a neighbour
	 */
	static uint32 GetNeighborBit(const FIntVector& Offset);

	/** Re-cooks requested because a neighbour missing at cook time arrived (OnChunkVoxelDataAvailable), this session. */
	int64 GetTotalNeighborRecooks() const { return TotalNeighborRecooks; }

	/** The cooks' cost per stage since the last reset. */
	const FVoxelCollisionCookStats& GetCookStats() const { return CookStats; }
	void ResetCookStats() { CookStats = FVoxelCollisionCookStats(); CookStats.ResetSeconds = FPlatformTime::Seconds(); NeighborWaitHeld.Reset(); CookedBeforeMesh.Reset(); }

	// ==================== No surface (G283) ====================

	/**
	 * The material table a mesher reads (its own, else the built-in defaults): the no-surface prediction asks it which
	 * materials the cubic mesher draws faces against.
	 *
	 * @param Mesher The mesher
	 * @return Its table (never null)
	 */
	static const FVoxelMaterialTable* GetMesherMaterials(const IVoxelMesher& Mesher);

	/**
	 * A voxel's side of the surface as every mesher reads it: air below density 127 (Dual Contouring and Marching Cubes
	 * put the surface at 0.5 of 255, the cubic mesher at VOXEL_SURFACE_THRESHOLD, 127), solid from 128; 127 itself is
	 * read as air by the first two and solid by the cubic mesher, so it is Mixed. A solid voxel of a material the cubic
	 * mesher draws faces against (non-occluding: leaves, glass) is Mixed too.
	 *
	 * @param Voxel The voxel
	 * @param Materials The mesher's material table (null: no material is non-occluding)
	 * @return Air, Solid or Mixed
	 */
	static EVoxelSurfaceSide GetSurfaceSide(const FVoxelData& Voxel, const FVoxelMaterialTable* Materials);

	/**
	 * The side of a whole chunk's voxels: a few voxels first (a chunk with a surface usually differs between its corners),
	 * then all of them, stopping at the first that differs.
	 *
	 * @param Voxels The chunk's ChunkSize^3 voxels
	 * @param ChunkSize Voxels per chunk axis
	 * @param Materials The mesher's material table (null: none non-occluding)
	 * @return Air or Solid when every voxel is on that side, else Mixed
	 */
	static EVoxelSurfaceSide GetVolumeSide(const TArray<FVoxelData>& Voxels, int32 ChunkSize, const FVoxelMaterialTable* Materials);

	/**
	 * Whether every voxel a cook reads of one neighbour is on a side: the neighbour's planes next to the face, edge or
	 * corner it shares with the chunk (Depth deep across each axis Offset is not zero on, the whole range along the others).
	 *
	 * @param Offset The neighbour's offset from the chunk (each component -1..1, not all zero)
	 * @param ChunkSize Voxels per chunk axis
	 * @param Depth Planes the cook reads across a face (VoxelNeighborSlices::GetNeighborDepth)
	 * @param Side Air or Solid
	 * @param Neighbor What the cook holds of the neighbour (missing: read as air)
	 * @param Materials The mesher's material table (null: none non-occluding)
	 * @return True when every voxel of the region is on Side
	 */
	static bool IsNeighborRegionOnSide(const FIntVector& Offset, int32 ChunkSize, int32 Depth, EVoxelSurfaceSide Side,
		const FVoxelCollisionNeighborView& Neighbor, const FVoxelMaterialTable* Materials);

	/**
	 * Whether a cook would find no surface (G283): the chunk's voxels on one side and every voxel it reads of its 26
	 * neighbours on the same side, a missing neighbour read as air (Dual Contouring's rule; Marching Cubes reads the
	 * chunk's own voxel there, which this takes as air too, so a solid chunk with a missing neighbour is never predicted
	 * empty). With no sign change anywhere in what it reads, no mesher makes a triangle.
	 *
	 * @param OwnSide The chunk's side (GetVolumeSide)
	 * @param ChunkSize Voxels per chunk axis
	 * @param Depth Planes the cook reads across a face
	 * @param GetNeighbor What the cook holds of the neighbour at an offset
	 * @param Materials The mesher's material table (null: none non-occluding)
	 * @return True when the cook's mesh would be empty
	 */
	static bool PredictNoSurface(EVoxelSurfaceSide OwnSide, int32 ChunkSize, int32 Depth,
		TFunctionRef<FVoxelCollisionNeighborView(const FIntVector&)> GetNeighbor, const FVoxelMaterialTable* Materials);

	/** @return Chunks marked ready-empty without a cook, this session (voxel.Collision.SkipNoSurface, G283) */
	int64 GetTotalSkippedAtLaunch() const { return TotalSkippedAtLaunch; }

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
	 * The neighbour audit (voxel.Collision.AuditNeighbors, G32): how many cooked chunks read a neighbour as air that has voxel data now.
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
	 * Get the radius actually used this frame (CollisionRadius plus the speed-aware lead).
	 * Diagnostics/validation for the Tier-0 fall-through fix.
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
	 * BuildPhysicsTriangles without the triangles of no area (and any naming a vertex past Positions): what the cook
	 * builds the chunk's trimesh from. Such a triangle has no normal, so a query that hit it reported a normal of no
	 * length (G230); the engine's cooker cleans them out of the meshes it cooks.
	 *
	 * @param RenderIndices Triangle list in render order (3 indices per triangle; a trailing partial triangle is ignored)
	 * @param Positions     The mesh's vertex positions the indices name
	 * @param OutTriangles  Receives the physics triangles of the render triangles that have an area (replaced, not appended)
	 * @return How many triangles were dropped
	 */
	static int32 BuildPhysicsTriangles(const TArray<uint32>& RenderIndices, const TArray<FVector3f>& Positions,
		TArray<Chaos::TVector<int32, 3>>& OutTriangles);

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
	 * Resolve the position collision should center on: the player pawn (the camera trails it in
	 * 3rd person, which shrinks the forward collision lead), or the passed viewer position when no
	 * pawn is resolvable.
	 */
	FVector ResolveFocusPosition(const FVector& FallbackViewerPosition) const;

	/**
	 * Tier 1: force-cook, at top priority, the chunk under the focus plus a short fan
	 * along its velocity across local up — including the chunk it is descending into — so ground under
	 * imminent (and downhill) footfalls is cooked before the pawn enters it. Overrides the normal
	 * center-distance priority (which deprioritizes a descending chunk whose center is far below).
	 */
	void ForcePathCoverage(const FVector& FocusPosition, const FVector& FocusVelocity);


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
	 * The neighbour wait (G283, D196; voxel.Collision.NeighborWait, off by default): whether a queued chunk's first cook
	 * waits this tick for neighbours still being generated, so it reads them instead of air and needs no re-cook when they
	 * arrive. Only a first cook away from every focus (beyond voxel.Collision.NeighborWaitChunks chunk widths) below the
	 * path coverage's priority waits, and not past voxel.Collision.NeighborWaitMaxSeconds since it was queued.
	 *
	 * @param Request The queued request
	 * @param FirstFocus The first focus's position
	 * @param Now FPlatformTime::Seconds this tick
	 * @return True to leave it in the queue this tick
	 */
	bool ShouldWaitForNeighbors(const FCollisionCookRequest& Request, const FVector& FirstFocus, double Now) const;

	/**
	 * Mark a chunk ready-empty without launching its cook when what the cook would read is known on the game thread to
	 * hold no surface (G283): the chunk and every neighbour held uniform with no edits, all on one side, a missing
	 * neighbour taken as air. The neighbours still missing are recorded, so their arrival re-cooks it as for a cook.
	 *
	 * @param ChunkCoord The chunk about to be cooked
	 * @return True if the chunk was marked ready-empty (no cook launched)
	 */
	bool TrySkipCookAtLaunch(const FIntVector& ChunkCoord);

	/** Count a chunk's first collision (a body or ready-empty) in its distance band since the stats' reset (CookStats). */
	void NoteFirstReady(const FIntVector& ChunkCoord);

	/** The first focus's position at the last Update (the stats' distance bands read it). */
	FVector LastUpdateFocus = FVector::ZeroVector;

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
	 * @param bDeferDestroy Leave its component to DestroyPendingComponents (a chunk that left the shell, G282) instead of
	 *        destroying it now
	 */
	void RemoveCollision(const FIntVector& ChunkCoord, bool bDeferDestroy = false);

	/**
	 * Destroy the components RemoveCollision left (G282): within the budget, the first always; all without one.
	 *
	 * @param Budget The tick budget, or null for all
	 * @return The components destroyed
	 */
	int32 DestroyPendingComponents(const FVoxelTickBudget* Budget);

	/** Destroy a chunk's component left by RemoveCollision now, if it has one (its chunk gets collision again). */
	void DestroyPendingComponent(const FIntVector& ChunkCoord);

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

	/** True until we've successfully queued initial collision (chunks may not be loaded on first frame) */
	bool bPendingInitialUpdate = true;

	/**
	 * Seconds since the last collision decision sweep. Sweeps only consider LOADED chunks, so a pawn that stops
	 * right after a teleport (save restore, vox.TP) would otherwise never get collision for the chunks that
	 * finish loading around it; voxel.Collision.ResweepSeconds re-runs the sweep while stationary.
	 */
	float SecondsSinceDecisions = 0.0f;

	// ==================== Tier 0: pawn-centered, speed-aware collision ====================

	/** Last focus (pawn) position, for velocity estimation. FLT_MAX until the first Update(). */
	FVector LastFocusPosition = FVector(FLT_MAX);

	/** Smoothed focus speed (uu/s); drives the speed-aware radius lead. */
	float SmoothedFocusSpeed = 0.0f;

	/** Jumps of the focus taken as cuts (more than a chunk in one tick: no speed from them; G282) */
	int64 TotalFocusJumps = 0;

	/**
	 * The components of chunks that left the shell, waiting to be destroyed within the tick budget (G282), by chunk; each
	 * with its body setup. Far from every focus by the sweep's rule, so they collide with nothing that matters meanwhile.
	 */
	struct FPendingDestroy
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		TWeakObjectPtr<UBodySetup> BodySetup;
	};
	TMap<FIntVector, FPendingDestroy> PendingDestroys;

	/** This Update's sweep leaves the components it removes to DestroyPendingComponents (a tick budget is set, G282) */
	bool bDeferSweepDestroys = false;

	/** Collision radius actually used this frame (CollisionRadius + speed lead). Refreshed in Update(). */
	float CurrentEffectiveRadius = 5000.0f;

	/** True when the focus was ~stationary last frame; used to force an eager stop->start re-decision. */
	bool bWasStationary = true;

	/** Speed (uu/s) at or below which the focus counts as stationary. */
	static constexpr float StationarySpeedThreshold = 10.0f;

	// ==================== Further foci (G217) ====================

	/** This tick's further foci (SetFurtherFoci): a server's other players' pawns. */
	TArray<FVoxelCollisionFocus> FurtherFoci;

	/** Where the further foci were at the last decision sweep: a change of their number or a move past the threshold sweeps again. */
	TArray<FVector> LastSweepFurtherFoci;

	/**
	 * A point's distance from the nearest focus: the first focus alone without further ones (the same arithmetic as
	 * before them), else the least of them all.
	 */
	float GetNearestFocusDistance(const FVector& Point, const FVector& FirstFocus) const;

	// ==================== Statistics ====================

	/** Total collision meshes generated */
	int64 TotalCollisionsGenerated = 0;

	/** Summed from each completed cook's result on the game thread. */
	FVoxelCollisionCookStats CookStats;

	/** Cooks launched while the chunk's mesh at the cook's LOD was coming: the launch's FPlatformTime::Seconds (G296's measure) */
	TMap<FIntVector, double> CookedBeforeMesh;

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

	/** Chunks marked ready-empty without a cook (TrySkipCookAtLaunch), this session */
	int64 TotalSkippedAtLaunch = 0;

	/** Chunks the neighbour wait has held since the stats' reset (to count each once) */
	TSet<FIntVector> NeighborWaitHeld;
};
