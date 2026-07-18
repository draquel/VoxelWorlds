// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Seam ownership + scheduling registry (VoxelStreaming, seam-ownership refactor P0).
 *
 * Part of the single-owner seam-meshing architecture
 * (Documentation/Research/SEAM_OWNERSHIP_ARCHITECTURE.md §2). The end-state decomposes every chunk
 * mesh into an INTERIOR mesh (cells strictly inside the chunk — zero neighbour dependence) plus SEAM
 * meshes, where each physical boundary between chunks is meshed exactly ONCE by a single job that
 * reads both sides' data. This file implements the P0 groundwork: the data structures and scheduler
 * that decide WHICH seam gets meshed, by WHOM, and WHEN — without producing any geometry yet (the
 * seam job is a stub in P0, so there is deliberately no visual or behavioural change).
 *
 * The registry is pure C++ (no UObject/reflection, no RHI, no world access): all of its state is
 * pushed in through the lifecycle hooks below by UVoxelChunkManager, mirroring the exact change
 * points the #42 completion-time boundary revalidation already fires from. That keeps it unit-testable
 * headless and decoupled from the streaming component.
 *
 * @see UVoxelChunkManager (the caller that drives the hooks and the per-tick scheduler)
 * @see FMeshBoundaryDep (the per-neighbour dispatch-time snapshot this registry's participants mirror)
 * @see Documentation/Research/SEAM_OWNERSHIP_ARCHITECTURE.md
 */

/** The three kinds of physical boundary a chunk shares with its neighbourhood. */
enum class EVoxelSeamType : uint8
{
	/** A shared face between a chunk PAIR (2 participants). */
	Face,
	/** A shared edge between a chunk 4-tuple (4 participants). */
	Edge,
	/** A shared corner between a chunk 8-tuple (8 participants). */
	Corner
};

/**
 * Canonical identity of one seam.
 *
 * A seam is owned by the MINIMUM-COORDINATE participating chunk (the lower corner of the participant
 * box), which makes ownership deterministic with no negotiation (SEAM_OWNERSHIP_ARCHITECTURE.md §2.2).
 * Given the owner, type, and axis the full participant set is fully derived (see GetParticipants), so
 * both sides of any boundary map to the identical key — no duplication, no gaps.
 *
 * Axis meaning:
 *  - Face:   the boundary's normal axis (0 = X, 1 = Y, 2 = Z); participants {Owner, Owner + e_axis}.
 *  - Edge:   the axis the edge is PARALLEL to (0/1/2); participants are the 2x2 block in the other two axes.
 *  - Corner: unused (always 0); participants are the 2x2x2 block from Owner.
 */
struct FVoxelSeamKey
{
	/** Minimum-coordinate participating chunk (canonical owner). */
	FIntVector Owner = FIntVector::ZeroValue;

	/** Which kind of boundary this is. */
	EVoxelSeamType Type = EVoxelSeamType::Face;

	/** Face: normal axis; Edge: parallel axis; Corner: unused (0). */
	uint8 Axis = 0;

	FVoxelSeamKey() = default;
	FVoxelSeamKey(const FIntVector& InOwner, EVoxelSeamType InType, uint8 InAxis)
		: Owner(InOwner), Type(InType), Axis(InAxis) {}

	FORCEINLINE bool operator==(const FVoxelSeamKey& Other) const
	{
		return Owner == Other.Owner && Type == Other.Type && Axis == Other.Axis;
	}
	FORCEINLINE bool operator!=(const FVoxelSeamKey& Other) const { return !(*this == Other); }

	friend FORCEINLINE uint32 GetTypeHash(const FVoxelSeamKey& Key)
	{
		uint32 Hash = GetTypeHash(Key.Owner);
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint32>(Key.Type)));
		Hash = HashCombine(Hash, GetTypeHash(static_cast<uint32>(Key.Axis)));
		return Hash;
	}

	/** Human-readable description for logging/debug (e.g. "Face(1,0,0)+Y"). */
	FString ToString() const;
};

/**
 * One participant's boundary-relevant state as captured when a seam is scheduled.
 *
 * Deliberately mirrors FMeshBoundaryDep (ContentVersion + MeshedLODLevel + data-presence) plus the
 * participant coordinate. In P0 this is the launch-time snapshot the (future P1+) seam job will build
 * against; a change in any participant's ContentVersion or rendered LOD since capture is what restales
 * the seam — the same signal the #42 revalidation uses per chunk.
 */
struct FVoxelSeamParticipant
{
	/** The participating chunk's coordinate. */
	FIntVector Coord = FIntVector::ZeroValue;

	/** FChunkDescriptor::ContentVersion at capture (monotonic; bumps on generation + voxel edits). */
	uint32 ContentVersion = 0;

	/** The participant's rendered LOD at capture (-1 = not yet meshed). */
	int32 MeshedLODLevel = -1;

	/** Whether the participant's voxel data was resident (available to read) at capture. */
	bool bResident = false;
};

/**
 * Per-seam bookkeeping held by the registry.
 */
struct FVoxelSeamState
{
	/** Canonical identity (owner + type + axis). */
	FVoxelSeamKey Key;

	/**
	 * The derived participant list (coords fixed by Key; ContentVersion/LOD/resident refreshed to the
	 * live mirror each time the seam is scheduled — the launch snapshot the seam job builds against).
	 */
	TArray<FVoxelSeamParticipant> Participants;

	/** A participant's content or rendered LOD changed (or a participant loaded/unloaded) since the
	 *  seam was last meshed — it needs a (re)build. */
	bool bDirty = false;

	/** A seam job for this seam is currently enqueued/in flight (guards against double-scheduling). */
	bool bScheduled = false;
};

/**
 * A scheduled unit of seam work. In P0 the job is a stub (the processor logs/counts and produces no
 * geometry); P1+ fills in the actual seam mesher. Priority mirrors the meshing queue's tiered model
 * (higher pops first) so near-viewer seams are meshed ahead of far ones.
 */
struct FVoxelSeamJob
{
	/** Which seam to (re)build. */
	FVoxelSeamKey Key;

	/** Scheduling priority (higher = sooner); see FVoxelSeamRegistry::ComputeSeamPriority. */
	float Priority = 0.0f;

	/** Participant snapshot captured at schedule time (mirrors FMeshBoundaryDep per participant). */
	TArray<FVoxelSeamParticipant> Participants;

	/** Ascending sort (lowest priority first, highest at the back for O(1) pop) — matches FChunkLODRequest. */
	FORCEINLINE bool operator<(const FVoxelSeamJob& Other) const { return Priority < Other.Priority; }
};

/** Snapshot counters for debug/benchmark surfacing (no behavioural effect). */
struct FVoxelSeamRegistryStats
{
	/** Chunks currently mirrored (registered, not yet unregistered). */
	int32 ChunkCount = 0;
	/** Seams currently tracked (at least one participant present). */
	int32 SeamCount = 0;
	/** Seams currently marked dirty (awaiting a schedulable, all-resident window). */
	int32 DirtyCount = 0;
	/** Seam jobs currently enqueued (scheduled, not yet processed). */
	int32 JobQueueDepth = 0;
	/** Lifetime seams created. */
	int64 TotalSeamsCreated = 0;
	/** Lifetime seam jobs scheduled (dirty + all-participants-resident). */
	int64 TotalSeamJobsScheduled = 0;
	/** Lifetime seam jobs processed by the (P0: stub) processor. */
	int64 TotalSeamJobsProcessed = 0;
};

/**
 * Single-owner seam registry + scheduler. See file header for the architecture.
 *
 * Thread safety: game-thread only (driven from UVoxelChunkManager::TickComponent and its callbacks).
 * Ownership: a plain member of the chunk manager; not reflected, not serialized.
 */
class VOXELSTREAMING_API FVoxelSeamRegistry
{
public:
	FVoxelSeamRegistry() = default;

	// ==================== Enable / lifetime ====================

	/**
	 * Toggle the registry. When disabled the lifecycle hooks and scheduler are no-ops; a true->false
	 * transition clears all state so a re-enable starts clean (the manager re-registers loaded chunks
	 * as they next change). Kept behind a cvar so P0 can be A/B'd off with zero residual cost.
	 */
	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	/** Enable per-seam-job trace logging (voxel.Seam.Debug). Off = only Verbose lines, filtered out. */
	void SetDebugLogging(bool bInDebug) { bDebugLogging = bInDebug; }

	/** Drop all tracked chunks, seams, and queued jobs (Shutdown / EndPlay / disable). */
	void Reset();

	// ==================== Lifecycle hooks (pushed by the chunk manager) ====================

	/**
	 * A chunk's voxel data is available (e.g. generation completed). Mirrors the chunk's state and
	 * marks all of its incident seams dirty — a newly resident participant restales every boundary it
	 * touches (the seam-ownership analogue of QueueNeighborsForRemesh's 26-neighbour fan-out).
	 *
	 * @param ChunkCoord      Chunk coordinate.
	 * @param ContentVersion  FChunkDescriptor::ContentVersion (the seam-invalidation key).
	 * @param MeshedLODLevel  Rendered LOD (-1 if not yet meshed).
	 */
	void RegisterChunk(const FIntVector& ChunkCoord, uint32 ContentVersion, int32 MeshedLODLevel);

	/**
	 * A chunk's voxel content changed (a voxel edit / regeneration bumped ContentVersion). Updates the
	 * mirror and marks the chunk's incident seams dirty.
	 */
	void UpdateChunkContent(const FIntVector& ChunkCoord, uint32 ContentVersion);

	/**
	 * A chunk's RENDERED LOD changed (mesh submitted at a new LOD). Marks the chunk's incident seams
	 * dirty only when the LOD actually changed. No-op when unchanged.
	 */
	void UpdateChunkRenderedLOD(const FIntVector& ChunkCoord, int32 MeshedLODLevel);

	/**
	 * A chunk unloaded / its data was freed. Drops it from the mirror, marks its incident seams dirty
	 * (a participant left), and prunes any seam that now has no remaining participant.
	 */
	void UnregisterChunk(const FIntVector& ChunkCoord);

	// ==================== Per-tick scheduler ====================

	/**
	 * Move dirty seams whose participants are ALL resident into the priority-sorted job queue. Seams
	 * that are dirty but not yet all-resident stay dirty and are retried on a later tick (the same
	 * "wait for neighbours" gating the meshing scheduler uses).
	 *
	 * @param ViewerChunk     Viewer's current chunk coord (for near-first prioritisation).
	 * @param NearChunkRadius Chebyshev chunk radius considered "near the viewer" (near seams get the
	 *                        near-correction priority tier so they'd beat the streaming wave).
	 * @param MaxToSchedule   Cap on dirty seams EXAMINED this call (0 = all). Bounds per-tick cost; any
	 *                        not examined (or examined-but-not-yet-ready) are retried on a later tick.
	 * @return Number of seams scheduled.
	 */
	int32 ScheduleReadySeams(const FIntVector& ViewerChunk, int32 NearChunkRadius, int32 MaxToSchedule);

	/**
	 * Process up to MaxJobs seam jobs (highest priority first). P0: the job is a STUB — it produces no
	 * geometry, only logs (when voxel.Seam.Debug is on) and increments counters, then marks the seam
	 * clean. P1+ replaces the stub body with the actual seam mesher.
	 *
	 * @param MaxJobs Cap on jobs processed this call (0 = unlimited).
	 * @return Number of jobs processed.
	 */
	int32 ProcessSeamJobQueue(int32 MaxJobs);

	// ==================== Queries (debug + tests) ====================

	FVoxelSeamRegistryStats GetStats() const;
	int32 GetChunkCount() const { return ChunkMirror.Num(); }
	int32 GetSeamCount() const { return Seams.Num(); }
	int32 GetDirtyCount() const { return DirtySeams.Num(); }
	int32 GetJobQueueDepth() const { return JobQueue.Num(); }

	/** Find a seam's state by key (nullptr if untracked). */
	const FVoxelSeamState* FindSeam(const FVoxelSeamKey& Key) const { return Seams.Find(Key); }

	/** True if the seam exists and every one of its participants is currently resident. */
	bool IsSeamReady(const FVoxelSeamKey& Key) const;

	/** True if the seam exists and is dirty. */
	bool IsSeamDirty(const FVoxelSeamKey& Key) const;

	/** Snapshot of the current dirty seam keys (order unspecified). For tests/debug. */
	void GetDirtySeamKeys(TArray<FVoxelSeamKey>& OutKeys) const;

	/** Snapshot of all tracked seam keys (order unspecified). For tests/debug. */
	void GetAllSeamKeys(TArray<FVoxelSeamKey>& OutKeys) const;

	/**
	 * Clear every seam's dirty flag and empty the dirty worklist, leaving topology and the mirror
	 * intact. Used when (re)initialising the scheduler and by unit tests to isolate dirty propagation.
	 */
	void MarkAllSeamsClean();

	// ==================== Static topology helpers (pure functions; tested directly) ====================

	/**
	 * The 26 seams a chunk is incident to (participates in): 6 faces + 12 edges + 8 corners, each
	 * expressed as its canonical (min-coordinate owner) key. Every one of the 26 neighbour directions
	 * maps to a distinct seam, so both sides of a boundary produce the same key.
	 */
	static void EnumerateIncidentSeams(const FIntVector& ChunkCoord, TArray<FVoxelSeamKey>& OutSeams);
	static TArray<FVoxelSeamKey> EnumerateIncidentSeams(const FIntVector& ChunkCoord);

	/**
	 * The chunks participating in a seam, derived from its key: 2 (face), 4 (edge), or 8 (corner). The
	 * owner (Key.Owner) is always the first element and is the component-wise minimum of the set.
	 */
	static void GetParticipants(const FVoxelSeamKey& Key, TArray<FIntVector>& OutParticipants);
	static TArray<FIntVector> GetParticipants(const FVoxelSeamKey& Key);

	/** Component-wise minimum (the lower corner) of a participant box == the canonical owner. */
	static FIntVector ComputeOwner(const TArray<FIntVector>& Participants);

	/** Number of participants for a seam type (2 / 4 / 8). */
	static int32 GetParticipantCount(EVoxelSeamType Type);

	/**
	 * Priority for a seam owned at OwnerChunk given the viewer chunk. Near seams (within NearChunkRadius
	 * chebyshev) get the near-correction tier (65 — above first-time meshes, mirroring the near-field
	 * fast path); far seams get a distance-decreasing value below the first-time-mesh tier.
	 */
	static float ComputeSeamPriority(const FIntVector& OwnerChunk, const FIntVector& ViewerChunk, int32 NearChunkRadius);

private:
	/** The mirrored boundary-relevant state of one registered chunk. */
	struct FChunkMirrorState
	{
		uint32 ContentVersion = 0;
		int32 MeshedLODLevel = -1;
	};

	/** Ensure the seam entry exists (creating it with derived participants); returns it. */
	FVoxelSeamState& FindOrCreateSeam(const FVoxelSeamKey& Key);

	/** Mark one seam dirty (idempotent): set the flag and add to the dirty worklist. */
	void MarkSeamDirty(FVoxelSeamState& Seam);

	/** Mark every seam incident to ChunkCoord dirty, creating entries as needed. */
	void DirtyIncidentSeams(const FIntVector& ChunkCoord);

	/** Remove a seam entirely (from the map, dirty set, and job queue). */
	void RemoveSeam(const FVoxelSeamKey& Key);

	/** True if every participant of Key is currently resident in the mirror. */
	bool AreAllParticipantsResident(const FVoxelSeamKey& Key) const;

	// ---- State ----

	/** Whether the registry is active (cvar-driven). When false, all hooks/scheduler are no-ops. */
	bool bEnabled = false;

	/** Emit per-seam-job trace at Log level (cvar-driven); otherwise seam jobs trace only at Verbose. */
	bool bDebugLogging = false;

	/** Registered chunks and their boundary-relevant state (residency == presence in this map). */
	TMap<FIntVector, FChunkMirrorState> ChunkMirror;

	/** All tracked seams keyed by canonical identity. */
	TMap<FVoxelSeamKey, FVoxelSeamState> Seams;

	/** Worklist of dirty seam keys (subset of Seams with bDirty) — scanned by the scheduler. */
	TSet<FVoxelSeamKey> DirtySeams;

	/** Priority-sorted pending seam jobs (ascending; highest priority at the back for O(1) pop). */
	TArray<FVoxelSeamJob> JobQueue;

	/** O(1) membership for JobQueue (guards against double-enqueue). */
	TSet<FVoxelSeamKey> JobQueueSet;

	// ---- Lifetime counters ----

	int64 TotalSeamsCreated = 0;
	int64 TotalSeamJobsScheduled = 0;
	int64 TotalSeamJobsProcessed = 0;
};
