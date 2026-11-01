// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Seam ownership + scheduling registry (VoxelStreaming, seam-ownership refactor).
 *
 * Part of the single-owner seam-meshing architecture
 * (Documentation/Research/SEAM_OWNERSHIP_ARCHITECTURE.md §2): every chunk mesh is an INTERIOR mesh
 * (zero neighbour dependence) plus SEAM meshes, where each physical boundary between chunks is meshed
 * exactly ONCE by a single job that reads both sides' data. This registry decides WHICH seam gets
 * meshed, by WHOM, and WHEN; UVoxelChunkManager runs the jobs (DrainSeamJobs) with the mode-matched
 * seam mesher and reports them finished. It serves every meshing mode (cubic since world modes review
 * M5, which tracks face seams only: SetFaceSeamsOnly).
 *
 * The registry is pure C++ (no UObject/reflection, no RHI, no world access): all of its state is
 * pushed in through the lifecycle hooks below by UVoxelChunkManager, mirroring the exact change
 * points the #42 completion-time boundary revalidation already fires from. That keeps it unit-testable
 * headless and decoupled from the streaming component.
 *
 * @see UVoxelChunkManager (the caller that drives the hooks and the per-tick scheduler)
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
 * One participant's boundary-relevant state as captured when a seam is scheduled: ContentVersion,
 * rendered LOD and data presence, plus the participant coordinate. It is the launch-time snapshot the
 * seam job builds against; a change in any participant's ContentVersion or rendered LOD since capture
 * is what restales the seam.
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

	/**
	 * A job for this seam is queued or on a worker: set when scheduled, cleared only by
	 * NotifySeamJobFinished. A seam re-dirtied meanwhile stays dirty and
	 * is rebuilt once the job finishes, so the newest content always gets built (R24 P0: a re-dirty
	 * during flight used to be dropped and left the seam stale until something else changed it).
	 */
	bool bScheduled = false;

	/** In the scan rotation (DirtyQueue). Only READY dirty seams are queued — see MarkSeamDirty. */
	bool bQueued = false;

	/** Wall-clock of the most recent schedule (0 = never). Instrumentation: detects a seam being
	 *  scheduled again shortly after its previous build — a rebuild the old slow rotation coalesced. */
	double LastScheduledAtSeconds = 0.0;

	/**
	 * Wall-clock (FPlatformTime::Seconds) of the clean->dirty transition that started the CURRENT
	 * wait. Preserved across re-dirtying while already dirty (the wait began at the first dirtying,
	 * not the latest), cleared when the seam is scheduled. 0 = not waiting. Feeds the scan-latency
	 * instrumentation (see FVoxelSeamLatencyStats).
	 */
	double DirtiedAtSeconds = 0.0;

	/**
	 * Wall-clock at which ALL participants were resident while this seam was dirty — the moment
	 * it became buildable. A seam first dirtied with a participant still absent gets this stamped
	 * only when that participant's RegisterChunk re-dirties it. Scan wait is measured from here,
	 * so neighbour-arrival latency (a streaming property) is not mistaken for scheduler latency.
	 * 0 = not buildable yet during the current wait; cleared when scheduled.
	 */
	double ReadyAtSeconds = 0.0;
};

/**
 * A scheduled unit of seam work, run by the chunk manager's seam mesher. Priority mirrors the meshing
 * queue's tiered model (higher pops first) so near-viewer seams are meshed ahead of far ones.
 */
struct FVoxelSeamJob
{
	/** Which seam to (re)build. */
	FVoxelSeamKey Key;

	/** Scheduling priority (higher = sooner); see FVoxelSeamRegistry::ComputeSeamPriority. */
	float Priority = 0.0f;

	/** Participant snapshot captured at schedule time. */
	TArray<FVoxelSeamParticipant> Participants;

	/**
	 * The owner's residency epoch at schedule time (FVoxelSeamRegistry::GetResidencyEpoch). Carried
	 * through the async job: a result is only applied while the owner is still in that same residency
	 * (IsResidencyCurrent), so a late result can never recreate a bucket for an owner that unloaded.
	 */
	uint32 OwnerResidencyEpoch = 0;

	/** Latency instrumentation: when the seam went dirty and when this job was scheduled. Carried
	 *  through the async job so completion can report dirty->visible end to end. */
	double DirtiedAtSeconds = 0.0;
	double ReadyAtSeconds = 0.0;
	double ScheduledAtSeconds = 0.0;

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
	/** Lifetime seam jobs drained for execution (DrainSeamJobs). */
	int64 TotalSeamJobsProcessed = 0;
	/** Lifetime seam results rejected because their owner left (or left and reloaded) while in flight. */
	int64 TotalSeamResultsRejected = 0;
};

/**
 * Scheduler latency instrumentation. Answers "how long does a seam that goes dirty wait before it
 * is scheduled, and before its geometry lands?" — split NEAR the viewer (the seams a player can
 * see change under them) vs FAR. The scan over dirty seams is FIFO round-robin while priority is
 * only applied at drain, so a near seam can queue behind every never-schedulable far seam; this
 * is the measurement that confirms or refutes that as a visible-latency source.
 *
 * Interval counters reset each TakeLatencyStats; the percentile windows are rolling.
 */
struct FVoxelSeamLatencyStats
{
	// ---- Snapshot at take time ----
	int32 DirtyCount = 0;        // all seams currently dirty
	int32 NearDirtyCount = 0;    // dirty seams whose owner is within NearChunkRadius of the viewer

	// ---- Interval counters (since the previous take) ----
	int32 Examined = 0;              // dirty entries the scan looked at
	int32 RequeuedNotResident = 0;   // examined but a participant LEFT since enqueue: dropped from the rotation (log token stays "notResident")
	int32 RequeuedInFlight = 0;      // examined but a previous job is still in flight
	int32 Scheduled = 0;             // jobs actually queued
	int32 Completed = 0;             // completions reported via RecordSeamCompleted

	// ---- Job stage ----
	int32 JobQueueDepth = 0;         // snapshot: scheduled jobs not yet drained to a worker
	int32 RescheduledWithin1s = 0;   // interval: scheduled again < 1 s after the previous schedule (a rebuild the 300 ms rotation used to coalesce)
	int32 DroppedInFlight = 0;       // interval: drained then dropped because an older job for the same seam was still in flight (seam stays CLEAN — stale until re-dirtied)
	int32 DroppedParticipant = 0;    // interval: drained then dropped because a participant lost its data / mesh, or the participant tuple had the wrong shape
	int32 RejectedDepartedOwner = 0; // interval: completed but discarded because the owner unloaded (or reloaded) while the job was in flight

	// ---- Rolling windows, milliseconds ----
	// dirty -> scheduled: the TOTAL wait, which includes time spent waiting for an absent
	// participant to load. Player-relevant, but not attributable to the scheduler alone.
	int32 NearScheduleN = 0;  float NearScheduleP50Ms = 0, NearScheduleP95Ms = 0, NearScheduleMaxMs = 0;
	int32 FarScheduleN = 0;   float FarScheduleP50Ms = 0,  FarScheduleP95Ms = 0,  FarScheduleMaxMs = 0;
	// ready -> scheduled: the pure SCAN wait (the quantity the FIFO theory is about). Measured
	// from the moment the seam became buildable, so an absent neighbour cannot inflate it.
	int32 NearScanN = 0;      float NearScanP50Ms = 0, NearScanP95Ms = 0, NearScanMaxMs = 0;
	int32 FarScanN = 0;       float FarScanP50Ms = 0,  FarScanP95Ms = 0,  FarScanMaxMs = 0;
	// dirty -> completed, near only (total, incl. neighbour arrival)
	int32 NearEndToEndN = 0;  float NearEndToEndP50Ms = 0, NearEndToEndP95Ms = 0, NearEndToEndMaxMs = 0;
	// ready -> completed, near only: what the player sees once the neighbour is there — the
	// delay between "this boundary CAN be built" and "it is on screen"
	int32 NearReadyToDoneN = 0; float NearReadyToDoneP50Ms = 0, NearReadyToDoneP95Ms = 0, NearReadyToDoneMaxMs = 0;
	// scheduled -> completed (job queue + async), near only; attributes the e2e between scan and job
	int32 NearPostScheduleN = 0; float NearPostScheduleP50Ms = 0, NearPostScheduleP95Ms = 0, NearPostScheduleMaxMs = 0;
	// Post-schedule split, near only: scheduled -> dispatched (waiting for an in-flight slot),
	// dispatched -> completed (worker pool latency + result-queue pickup), and the mesher's own time.
	int32 NearQueueN = 0;  float NearQueueP50Ms = 0,  NearQueueP95Ms = 0,  NearQueueMaxMs = 0;
	int32 NearAsyncN = 0;  float NearAsyncP50Ms = 0,  NearAsyncP95Ms = 0,  NearAsyncMaxMs = 0;
	int32 NearWorkerN = 0; float NearWorkerP50Ms = 0, NearWorkerP95Ms = 0, NearWorkerMaxMs = 0;
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
	 * transition clears all state so a re-enable starts clean. The chunk manager enables it once and
	 * keeps it on (every mesher meshes through seam jobs); tests drive it directly.
	 */
	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	/**
	 * Track only each chunk's six face seams, not its edges and corners. For meshers whose boundary
	 * geometry lies entirely in face planes (cubic, world modes review WM-C1): their edge and corner
	 * jobs would only produce empty meshes. Changing it resets the registry, so set it before
	 * chunks register.
	 */
	void SetFaceSeamsOnly(bool bInFaceSeamsOnly);
	bool IsFaceSeamsOnly() const { return bFaceSeamsOnly; }

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

	// ==================== Residency epochs ====================

	/**
	 * The chunk's residency epoch: a non-zero number, unique for the registry's lifetime, assigned
	 * by every RegisterChunk (and when an Update* call first mirrors a chunk after a disable/enable).
	 * 0 = the chunk is not registered. ScheduleReadySeams stamps the owner's epoch into each job, and
	 * a completed result is applied only while the epoch is still current. Comparing epochs rather
	 * than asking "is the owner loaded" also rejects a result from an EARLIER residency of an owner
	 * that has since reloaded (VOXEL_QA_FOLLOWUPS_PLAN.md R23a).
	 *
	 * Epochs are never reused, not even across Reset/SetEnabled(false), so a result that outlives a
	 * reset cannot match a later residency.
	 */
	uint32 GetResidencyEpoch(const FIntVector& ChunkCoord) const;

	/**
	 * True when Epoch is non-zero and still ChunkCoord's current residency epoch.
	 * @param ChunkCoord The seam owner (or any chunk) the result was built for.
	 * @param Epoch      The epoch captured when the job was scheduled (FVoxelSeamJob::OwnerResidencyEpoch).
	 * @return Whether a result built for that residency may still be applied.
	 */
	bool IsResidencyCurrent(const FIntVector& ChunkCoord, uint32 Epoch) const;

	/** Count a completed seam result discarded because IsResidencyCurrent failed (instrumentation). */
	void RecordSeamResultRejected();

	// ==================== Per-tick scheduler ====================

	/**
	 * Move dirty seams whose participants are ALL resident into the priority-sorted job queue. Seams
	 * that are dirty but not yet all-resident stay dirty and are retried on a later tick (the same
	 * "wait for neighbours" gating the meshing scheduler uses).
	 *
	 * @param ViewerChunk     Viewer's current chunk coord (for near-first prioritisation).
	 * @param NearChunkRadius Chebyshev chunk radius considered "near the viewer" (near seams get the
	 *                        near-correction priority tier so they'd beat the streaming wave).
	 * @param MaxToSchedule   Cap on seams SCHEDULED this call (0 = unlimited). The scan examines up
	 *                        to 4x that (min 64) candidates in ROUND-ROBIN order — not-yet-ready
	 *                        seams rotate to the back so they can't starve schedulable ones.
	 * @return Number of seams scheduled.
	 */
	int32 ScheduleReadySeams(const FIntVector& ViewerChunk, int32 NearChunkRadius, int32 MaxToSchedule);

	/**
	 * Pop up to MaxJobs seam jobs (highest priority first) for external execution (P1 runtime
	 * pipeline: the chunk manager builds seam meshing requests from them and dispatches async).
	 * Each popped seam stays marked scheduled until the caller reports NotifySeamJobFinished: if a
	 * participant changes while the job is in flight, the seam is re-dirtied, waits for the job,
	 * and is then rebuilt, so the newer content always wins.
	 *
	 * @param OutJobs Receives the popped jobs (appended).
	 * @param MaxJobs Cap on jobs popped this call (0 = none — callers pass a positive budget).
	 * @return Number of jobs popped.
	 */
	int32 DrainSeamJobs(TArray<FVoxelSeamJob>& OutJobs, int32 MaxJobs);

	/**
	 * A drained job ended: its result was applied or rejected, or the dispatcher dropped it. The
	 * seam may schedule again; if it was re-dirtied while in flight it is rebuilt by the next scan.
	 * Safe to call for a seam that has since been pruned (no-op).
	 *
	 * @param Key The seam whose job ended.
	 */
	void NotifySeamJobFinished(const FVoxelSeamKey& Key);

	/**
	 * Mark one existing seam dirty again (no-op if untracked). For callers that discarded a build
	 * for reasons the registry cannot see, so the seam does not stay stale.
	 *
	 * @param Key The seam to rebuild.
	 */
	void RedirtySeam(const FVoxelSeamKey& Key);

	/**
	 * The seams incident to a chunk that are buildable right now (tracked, every participant
	 * resident): the set an LOD change of that chunk will rebuild. Seams with an absent
	 * participant are left out, since they cannot land until it arrives.
	 *
	 * @param ChunkCoord The chunk whose incident seams to list.
	 * @param OutSeams   Receives the keys (reset first).
	 */
	void GetReadyIncidentSeams(const FIntVector& ChunkCoord, TArray<FVoxelSeamKey>& OutSeams) const;

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

	/** The 6 face seams of the 26 (in +X, -X, +Y, -Y, +Z, -Z order), as canonical keys. */
	static void EnumerateIncidentFaceSeams(const FIntVector& ChunkCoord, TArray<FVoxelSeamKey>& OutSeams);

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

	/** True when OwnerChunk is within NearChunkRadius (Chebyshev) of ViewerChunk — the near-correction tier. */
	static bool IsNearViewer(const FIntVector& OwnerChunk, const FIntVector& ViewerChunk, int32 NearChunkRadius);

	// ==================== Latency instrumentation ====================

	/**
	 * Report a seam job's completion so dirty->completed latency is recorded. Timestamps are the
	 * ones the job carried (FVoxelSeamJob::DirtiedAtSeconds / ScheduledAtSeconds); zero is ignored.
	 * @param bNear Whether the seam's owner is near the viewer at completion time.
	 * @param DispatchedAtSeconds When the job left the queue for a worker (0 = unknown: skips the split windows).
	 * @param WorkerMs The mesher's own wall time on the worker (only recorded when DispatchedAtSeconds > 0).
	 */
	void RecordSeamCompleted(double DirtiedAtSeconds, double ReadyAtSeconds, double ScheduledAtSeconds, bool bNear,
		double DispatchedAtSeconds = 0.0, double WorkerMs = 0.0);

	/**
	 * Report a drained job the dispatcher discarded. The seam was marked clean at schedule time, so a
	 * drop leaves stale geometry until something re-dirties it — worth counting.
	 * @param bOlderJobInFlight true = an older build of the same seam was still on a worker; false = a
	 *        participant lost its data/mesh or the tuple shape was wrong.
	 */
	void RecordSeamJobDropped(bool bOlderJobInFlight);

	/** The registry's clock (FPlatformTime::Seconds unless a test overrides it) so callers stamp on the same timeline. */
	double NowSeconds() const { return Now(); }

	/**
	 * Snapshot the latency instrumentation and reset the interval counters (rolling percentile
	 * windows are kept). Counts NearDirtyCount against the given viewer.
	 */
	FVoxelSeamLatencyStats TakeLatencyStats(const FIntVector& ViewerChunk, int32 NearChunkRadius);

	/** Tests: substitute a controllable clock for FPlatformTime::Seconds. */
	void SetClockOverride(TFunction<double()> InClock) { ClockOverride = MoveTemp(InClock); }

private:
	/** Wall clock for latency stamps (overridable in tests). */
	double Now() const;

	/** Fixed-capacity rolling sample window with percentile readout. */
	struct FLatencySample
	{
		static constexpr int32 Capacity = 512;
		TArray<float> Samples;
		int32 Next = 0;
		int32 Count = 0;
		void Add(float Ms);
		void Reset() { Samples.Reset(); Next = 0; Count = 0; }
		/** Fills p50/p95/max in ms; all zero when empty. */
		void Percentiles(float& OutP50, float& OutP95, float& OutMax) const;
	};

	/** The mirrored boundary-relevant state of one registered chunk. */
	struct FChunkMirrorState
	{
		uint32 ContentVersion = 0;
		int32 MeshedLODLevel = -1;
		/** Residency epoch (see GetResidencyEpoch); assigned when the chunk enters the mirror or re-registers. */
		uint32 ResidencyEpoch = 0;
	};

	/** Find the chunk's mirror entry, creating it with a fresh residency epoch when absent. */
	FChunkMirrorState& FindOrAddMirror(const FIntVector& ChunkCoord);

	/** Next residency epoch to hand out (starts at 1: 0 means "not registered"). Survives Reset. */
	uint32 NextResidencyEpoch = 1;

	/** Hand out a fresh, never-zero epoch. */
	uint32 AllocateResidencyEpoch();

	/** Ensure the seam entry exists (creating it with derived participants); returns it. */
	FVoxelSeamState& FindOrCreateSeam(const FVoxelSeamKey& Key);

	/** Mark one seam dirty (idempotent): set the flag and add to the dirty worklist. */
	void MarkSeamDirty(FVoxelSeamState& Seam);

	/** Mark every seam incident to ChunkCoord dirty, creating entries as needed. */
	void DirtyIncidentSeams(const FIntVector& ChunkCoord);

	/** The incident seams this registry tracks: all 26, or the 6 faces when SetFaceSeamsOnly. */
	void GetTrackedIncidentSeams(const FIntVector& ChunkCoord, TArray<FVoxelSeamKey>& OutSeams) const;

	/** Remove a seam entirely (from the map, dirty set, and job queue). */
	void RemoveSeam(const FVoxelSeamKey& Key);

	/** True if every participant of Key is currently resident in the mirror. */
	bool AreAllParticipantsResident(const FVoxelSeamKey& Key) const;

	// ---- State ----

	/** Whether the registry is active. When false, all hooks/scheduler are no-ops. */
	bool bEnabled = false;

	/** Track face seams only (see SetFaceSeamsOnly). */
	bool bFaceSeamsOnly = false;

	/** Registered chunks and their boundary-relevant state (residency == presence in this map). */
	TMap<FIntVector, FChunkMirrorState> ChunkMirror;

	/** All tracked seams keyed by canonical identity. */
	TMap<FVoxelSeamKey, FVoxelSeamState> Seams;

	/** Worklist of dirty seam keys (subset of Seams with bDirty) — membership/dedup. */
	TSet<FVoxelSeamKey> DirtySeams;

	/**
	 * Scan rotation over the READY subset of DirtySeams (head index + lazy compaction). Invariant:
	 * a key is here only if MarkSeamDirty saw every participant resident (FVoxelSeamState::bQueued
	 * tracks membership). Seams whose neighbour has not loaded — or never will, e.g. the air above
	 * and solid below the surface band, up to 18 of a chunk's 26 seams — are NOT queued; they wait
	 * in DirtySeams and are enqueued by the arrival that makes them buildable. Before this, the
	 * whole dirty set rotated here and 99.6% of the per-tick scan budget was spent re-examining
	 * never-ready seams, so a near seam waited ~(dirty / scan rate) ≈ 300 ms just to be looked at.
	 * In-flight seams rotate to the back; keys no longer dirty are skipped lazily.
	 */
	TArray<FVoxelSeamKey> DirtyQueue;
	int32 DirtyQueueHead = 0;

	/** Priority-sorted pending seam jobs (ascending; highest priority at the back for O(1) pop). */
	TArray<FVoxelSeamJob> JobQueue;

	/** O(1) membership for JobQueue (guards against double-enqueue). */
	TSet<FVoxelSeamKey> JobQueueSet;

	// ---- Lifetime counters ----

	int64 TotalSeamsCreated = 0;
	int64 TotalSeamJobsScheduled = 0;
	int64 TotalSeamJobsProcessed = 0;
	int64 TotalSeamResultsRejected = 0;

	// ---- Latency instrumentation (see FVoxelSeamLatencyStats) ----

	TFunction<double()> ClockOverride;
	int32 IntervalExamined = 0;
	int32 IntervalRequeuedNotResident = 0;
	int32 IntervalRequeuedInFlight = 0;
	int32 IntervalScheduled = 0;
	int32 IntervalCompleted = 0;
	FLatencySample NearScheduleLatency;
	FLatencySample FarScheduleLatency;
	FLatencySample NearScanWindow;
	FLatencySample FarScanWindow;
	FLatencySample NearEndToEndWindow;
	FLatencySample NearReadyToDoneWindow;
	FLatencySample NearPostScheduleLatency;
	int32 IntervalRescheduledWithin1s = 0;
	int32 IntervalDroppedInFlight = 0;
	int32 IntervalDroppedParticipant = 0;
	int32 IntervalRejectedDepartedOwner = 0;
	FLatencySample NearQueueWindow;
	FLatencySample NearAsyncWindow;
	FLatencySample NearWorkerWindow;
};
