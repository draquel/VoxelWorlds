// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSeamRegistry.h"
#include "VoxelStreaming.h"
#include "Algo/BinarySearch.h"

// ============================================================================
// Seam ownership + scheduling registry (seam-ownership refactor P0).
// Pure logic: no RHI, no world access, no reflection. All state is pushed in
// by UVoxelChunkManager. In P0 the seam job is a stub — this file lands the
// data structures + scheduler that P1+ build on, with no geometry produced and
// therefore no visual/behavioural change. See VoxelSeamRegistry.h and
// Documentation/Research/SEAM_OWNERSHIP_ARCHITECTURE.md.
// ============================================================================

namespace VoxelSeamRegistryDetail
{
	/** Unit vector along an axis (0=X, 1=Y, 2=Z). */
	static FORCEINLINE FIntVector AxisUnit(int32 Axis)
	{
		return FIntVector(Axis == 0 ? 1 : 0, Axis == 1 ? 1 : 0, Axis == 2 ? 1 : 0);
	}

	/** Component-wise min(v, 0): the lower-corner offset for an owner from a direction. */
	static FORCEINLINE FIntVector MinZero(const FIntVector& V)
	{
		return FIntVector(FMath::Min(V.X, 0), FMath::Min(V.Y, 0), FMath::Min(V.Z, 0));
	}

	/** Build the canonical seam key for chunk C shared in neighbour direction D (D in {-1,0,1}^3, != 0). */
	static FVoxelSeamKey KeyForDirection(const FIntVector& C, const FIntVector& D)
	{
		// Owner = lower corner of the participant box = C shifted by the negative components of D.
		const FIntVector Owner = C + MinZero(D);
		const int32 NonZero = (D.X != 0 ? 1 : 0) + (D.Y != 0 ? 1 : 0) + (D.Z != 0 ? 1 : 0);
		if (NonZero == 1)
		{
			// Face: axis = the single nonzero component (the boundary normal).
			const uint8 Axis = (D.X != 0) ? 0 : (D.Y != 0) ? 1 : 2;
			return FVoxelSeamKey(Owner, EVoxelSeamType::Face, Axis);
		}
		if (NonZero == 2)
		{
			// Edge: axis = the zero component (the axis the edge runs parallel to).
			const uint8 Axis = (D.X == 0) ? 0 : (D.Y == 0) ? 1 : 2;
			return FVoxelSeamKey(Owner, EVoxelSeamType::Edge, Axis);
		}
		// Corner: axis unused.
		return FVoxelSeamKey(Owner, EVoxelSeamType::Corner, 0);
	}
}

// ==================== FVoxelSeamKey ====================

FString FVoxelSeamKey::ToString() const
{
	const TCHAR* TypeStr = (Type == EVoxelSeamType::Face) ? TEXT("Face")
		: (Type == EVoxelSeamType::Edge) ? TEXT("Edge") : TEXT("Corner");
	static const TCHAR* AxisStr[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
	if (Type == EVoxelSeamType::Corner)
	{
		return FString::Printf(TEXT("Corner(%d,%d,%d)"), Owner.X, Owner.Y, Owner.Z);
	}
	return FString::Printf(TEXT("%s(%d,%d,%d)%s"), TypeStr, Owner.X, Owner.Y, Owner.Z,
		AxisStr[FMath::Clamp<int32>(Axis, 0, 2)]);
}

// ==================== Static topology helpers ====================

int32 FVoxelSeamRegistry::GetParticipantCount(EVoxelSeamType Type)
{
	switch (Type)
	{
	case EVoxelSeamType::Face:   return 2;
	case EVoxelSeamType::Edge:   return 4;
	case EVoxelSeamType::Corner: return 8;
	default:                     return 0;
	}
}

void FVoxelSeamRegistry::EnumerateIncidentSeams(const FIntVector& ChunkCoord, TArray<FVoxelSeamKey>& OutSeams)
{
	using namespace VoxelSeamRegistryDetail;
	OutSeams.Reset(26);
	// All 26 neighbour directions map to 26 distinct incident seams (6 faces, 12 edges, 8 corners).
	for (int32 dz = -1; dz <= 1; ++dz)
	{
		for (int32 dy = -1; dy <= 1; ++dy)
		{
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0 && dz == 0)
				{
					continue;
				}
				OutSeams.Add(KeyForDirection(ChunkCoord, FIntVector(dx, dy, dz)));
			}
		}
	}
}

TArray<FVoxelSeamKey> FVoxelSeamRegistry::EnumerateIncidentSeams(const FIntVector& ChunkCoord)
{
	TArray<FVoxelSeamKey> Out;
	EnumerateIncidentSeams(ChunkCoord, Out);
	return Out;
}

void FVoxelSeamRegistry::GetParticipants(const FVoxelSeamKey& Key, TArray<FIntVector>& OutParticipants)
{
	using namespace VoxelSeamRegistryDetail;
	OutParticipants.Reset();
	switch (Key.Type)
	{
	case EVoxelSeamType::Face:
	{
		const FIntVector E = AxisUnit(Key.Axis);
		OutParticipants.Add(Key.Owner);
		OutParticipants.Add(Key.Owner + E);
		break;
	}
	case EVoxelSeamType::Edge:
	{
		// The two axes perpendicular to the parallel axis, in ascending order.
		const int32 A = Key.Axis;
		const int32 B = (A == 0) ? 1 : 0;
		const int32 C = (A == 2) ? 1 : 2;
		const FIntVector Eb = AxisUnit(B);
		const FIntVector Ec = AxisUnit(C);
		OutParticipants.Add(Key.Owner);
		OutParticipants.Add(Key.Owner + Eb);
		OutParticipants.Add(Key.Owner + Ec);
		OutParticipants.Add(Key.Owner + Eb + Ec);
		break;
	}
	case EVoxelSeamType::Corner:
	{
		for (int32 k = 0; k <= 1; ++k)
		{
			for (int32 j = 0; j <= 1; ++j)
			{
				for (int32 i = 0; i <= 1; ++i)
				{
					OutParticipants.Add(Key.Owner + FIntVector(i, j, k));
				}
			}
		}
		break;
	}
	}
}

TArray<FIntVector> FVoxelSeamRegistry::GetParticipants(const FVoxelSeamKey& Key)
{
	TArray<FIntVector> Out;
	GetParticipants(Key, Out);
	return Out;
}

FIntVector FVoxelSeamRegistry::ComputeOwner(const TArray<FIntVector>& Participants)
{
	if (Participants.Num() == 0)
	{
		return FIntVector::ZeroValue;
	}
	FIntVector Min = Participants[0];
	for (int32 i = 1; i < Participants.Num(); ++i)
	{
		Min.X = FMath::Min(Min.X, Participants[i].X);
		Min.Y = FMath::Min(Min.Y, Participants[i].Y);
		Min.Z = FMath::Min(Min.Z, Participants[i].Z);
	}
	return Min;
}

bool FVoxelSeamRegistry::IsNearViewer(const FIntVector& OwnerChunk, const FIntVector& ViewerChunk, int32 NearChunkRadius)
{
	const FIntVector Delta = OwnerChunk - ViewerChunk;
	const int32 Cheb = FMath::Max3(FMath::Abs(Delta.X), FMath::Abs(Delta.Y), FMath::Abs(Delta.Z));
	return Cheb <= NearChunkRadius;
}

float FVoxelSeamRegistry::ComputeSeamPriority(const FIntVector& OwnerChunk, const FIntVector& ViewerChunk, int32 NearChunkRadius)
{
	const FIntVector Delta = OwnerChunk - ViewerChunk;
	const int32 Cheb = FMath::Max3(FMath::Abs(Delta.X), FMath::Abs(Delta.Y), FMath::Abs(Delta.Z));
	if (IsNearViewer(OwnerChunk, ViewerChunk, NearChunkRadius))
	{
		// Near-correction tier: matches GNearCorrectionPriority (above first-time meshes at 50), so
		// a near seam would beat the streaming wave — the same intent as the near-field fast path.
		return 65.0f;
	}
	// Far: below the first-time-mesh tier, decreasing with distance (a refinement that waits behind
	// new chunks) — mirrors how far coalesced corrections sit low in the meshing queue.
	return FMath::Max(1.0f, 50.0f - static_cast<float>(Cheb - NearChunkRadius));
}

// ==================== Enable / lifetime ====================

void FVoxelSeamRegistry::SetEnabled(bool bInEnabled)
{
	if (bInEnabled == bEnabled)
	{
		return;
	}
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		// Clear residual state so a later re-enable starts clean (the manager re-registers loaded
		// chunks as they next change/mesh). Avoids acting on a stale mirror after an A/B toggle.
		Reset();
	}
}

void FVoxelSeamRegistry::Reset()
{
	ChunkMirror.Reset();
	Seams.Reset();
	DirtySeams.Reset();
	DirtyQueue.Reset();
	DirtyQueueHead = 0;
	JobQueue.Reset();
	JobQueueSet.Reset();
	// Lifetime counters intentionally retained across Reset for session-cumulative debug stats.
	// Latency windows are NOT retained: they describe a timeline that no longer exists.
	IntervalExamined = IntervalRequeuedNotResident = IntervalRequeuedInFlight = 0;
	IntervalScheduled = IntervalCompleted = 0;
	IntervalRescheduledWithin1s = IntervalDroppedInFlight = IntervalDroppedParticipant = 0;
	NearScheduleWindow.Reset();
	FarScheduleWindow.Reset();
	NearScanWindow.Reset();
	FarScanWindow.Reset();
	NearEndToEndWindow.Reset();
	NearReadyToDoneWindow.Reset();
	NearPostScheduleWindow.Reset();
	NearQueueWindow.Reset();
	NearAsyncWindow.Reset();
	NearWorkerWindow.Reset();
}

// ==================== Latency instrumentation ====================

double FVoxelSeamRegistry::Now() const
{
	return ClockOverride ? ClockOverride() : FPlatformTime::Seconds();
}

void FVoxelSeamRegistry::FLatencyWindow::Add(float Ms)
{
	if (Samples.Num() < Capacity)
	{
		Samples.Add(Ms);
	}
	else
	{
		Samples[Next] = Ms;
	}
	Next = (Next + 1) % Capacity;
	Count = FMath::Min(Count + 1, Capacity);
}

void FVoxelSeamRegistry::FLatencyWindow::Percentiles(float& OutP50, float& OutP95, float& OutMax) const
{
	OutP50 = OutP95 = OutMax = 0.0f;
	if (Count == 0)
	{
		return;
	}
	TArray<float> Sorted(Samples.GetData(), Count);
	Sorted.Sort();
	OutP50 = Sorted[FMath::Clamp(Count / 2, 0, Count - 1)];
	OutP95 = Sorted[FMath::Clamp((Count * 95) / 100, 0, Count - 1)];
	OutMax = Sorted[Count - 1];
}

void FVoxelSeamRegistry::RecordSeamJobDropped(bool bOlderJobInFlight)
{
	if (bOlderJobInFlight)
	{
		++IntervalDroppedInFlight;
	}
	else
	{
		++IntervalDroppedParticipant;
	}
}

void FVoxelSeamRegistry::RecordSeamCompleted(double DirtiedAtSeconds, double ReadyAtSeconds, double ScheduledAtSeconds, bool bNear,
	double DispatchedAtSeconds, double WorkerMs)
{
	++IntervalCompleted;
	if (!bNear)
	{
		return; // far seams: the scan-wait window already tells the story; e2e is a near-tier question
	}
	const double CompletedAt = Now();
	if (DirtiedAtSeconds > 0.0)
	{
		NearEndToEndWindow.Add(static_cast<float>((CompletedAt - DirtiedAtSeconds) * 1000.0));
		const double From = (ReadyAtSeconds > 0.0) ? FMath::Max(DirtiedAtSeconds, ReadyAtSeconds) : DirtiedAtSeconds;
		NearReadyToDoneWindow.Add(static_cast<float>((CompletedAt - From) * 1000.0));
	}
	if (ScheduledAtSeconds > 0.0)
	{
		NearPostScheduleWindow.Add(static_cast<float>((CompletedAt - ScheduledAtSeconds) * 1000.0));
		if (DispatchedAtSeconds > 0.0)
		{
			NearQueueWindow.Add(static_cast<float>((DispatchedAtSeconds - ScheduledAtSeconds) * 1000.0));
			NearAsyncWindow.Add(static_cast<float>((CompletedAt - DispatchedAtSeconds) * 1000.0));
			NearWorkerWindow.Add(static_cast<float>(WorkerMs));
		}
	}
}

FVoxelSeamLatencyStats FVoxelSeamRegistry::TakeLatencyStats(const FIntVector& ViewerChunk, int32 NearChunkRadius)
{
	FVoxelSeamLatencyStats S;
	S.DirtyCount = DirtySeams.Num();
	for (const FVoxelSeamKey& Key : DirtySeams)
	{
		if (IsNearViewer(Key.Owner, ViewerChunk, NearChunkRadius))
		{
			++S.NearDirtyCount;
		}
	}

	S.Examined = IntervalExamined;
	S.RequeuedNotResident = IntervalRequeuedNotResident;
	S.RequeuedInFlight = IntervalRequeuedInFlight;
	S.Scheduled = IntervalScheduled;
	S.Completed = IntervalCompleted;
	IntervalExamined = IntervalRequeuedNotResident = IntervalRequeuedInFlight = 0;
	IntervalScheduled = IntervalCompleted = 0;
	S.JobQueueDepth = JobQueue.Num();
	S.RescheduledWithin1s = IntervalRescheduledWithin1s;
	S.DroppedInFlight = IntervalDroppedInFlight;
	S.DroppedParticipant = IntervalDroppedParticipant;
	IntervalRescheduledWithin1s = IntervalDroppedInFlight = IntervalDroppedParticipant = 0;

	S.NearScheduleN = NearScheduleWindow.Count;
	NearScheduleWindow.Percentiles(S.NearScheduleP50Ms, S.NearScheduleP95Ms, S.NearScheduleMaxMs);
	S.FarScheduleN = FarScheduleWindow.Count;
	FarScheduleWindow.Percentiles(S.FarScheduleP50Ms, S.FarScheduleP95Ms, S.FarScheduleMaxMs);
	S.NearScanN = NearScanWindow.Count;
	NearScanWindow.Percentiles(S.NearScanP50Ms, S.NearScanP95Ms, S.NearScanMaxMs);
	S.FarScanN = FarScanWindow.Count;
	FarScanWindow.Percentiles(S.FarScanP50Ms, S.FarScanP95Ms, S.FarScanMaxMs);
	S.NearReadyToDoneN = NearReadyToDoneWindow.Count;
	NearReadyToDoneWindow.Percentiles(S.NearReadyToDoneP50Ms, S.NearReadyToDoneP95Ms, S.NearReadyToDoneMaxMs);
	S.NearEndToEndN = NearEndToEndWindow.Count;
	NearEndToEndWindow.Percentiles(S.NearEndToEndP50Ms, S.NearEndToEndP95Ms, S.NearEndToEndMaxMs);
	S.NearPostScheduleN = NearPostScheduleWindow.Count;
	NearPostScheduleWindow.Percentiles(S.NearPostScheduleP50Ms, S.NearPostScheduleP95Ms, S.NearPostScheduleMaxMs);
	S.NearQueueN = NearQueueWindow.Count;
	NearQueueWindow.Percentiles(S.NearQueueP50Ms, S.NearQueueP95Ms, S.NearQueueMaxMs);
	S.NearAsyncN = NearAsyncWindow.Count;
	NearAsyncWindow.Percentiles(S.NearAsyncP50Ms, S.NearAsyncP95Ms, S.NearAsyncMaxMs);
	S.NearWorkerN = NearWorkerWindow.Count;
	NearWorkerWindow.Percentiles(S.NearWorkerP50Ms, S.NearWorkerP95Ms, S.NearWorkerMaxMs);
	return S;
}

// ==================== Internal helpers ====================

FVoxelSeamState& FVoxelSeamRegistry::FindOrCreateSeam(const FVoxelSeamKey& Key)
{
	if (FVoxelSeamState* Existing = Seams.Find(Key))
	{
		return *Existing;
	}
	FVoxelSeamState New;
	New.Key = Key;
	// Derive the (fixed) participant coordinates now; snapshot values are filled at schedule time.
	TArray<FIntVector> Coords;
	GetParticipants(Key, Coords);
	New.Participants.Reserve(Coords.Num());
	for (const FIntVector& Coord : Coords)
	{
		FVoxelSeamParticipant P;
		P.Coord = Coord;
		New.Participants.Add(P);
	}
	++TotalSeamsCreated;
	return Seams.Add(Key, MoveTemp(New));
}

void FVoxelSeamRegistry::MarkSeamDirty(FVoxelSeamState& Seam)
{
	Seam.bDirty = true;
	bool bAlreadyInSet = false;
	DirtySeams.Add(Seam.Key, &bAlreadyInSet);
	if (!bAlreadyInSet)
	{
		// The wait starts here. A later re-dirty while still waiting must NOT restart the clock —
		// the player has been looking at stale geometry since this moment, not since the latest bump.
		Seam.DirtiedAtSeconds = Now();
	}
	// Readiness: a seam becomes fully resident exactly when its last participant registers, and
	// RegisterChunk/UpdateChunk* dirty every incident seam AFTER inserting the chunk into the mirror,
	// so this is the one place readiness can flip to true. Only a READY seam joins the scan rotation:
	// a seam whose neighbour has not loaded (or never will — the air above / solid below the surface
	// band) would otherwise sit in the rotation for its owner's whole residency, and the scan would
	// spend its per-tick budget re-examining it (measured: 99.6% of the budget, ~300 ms of wait for
	// every near seam before it was even looked at). It stays in DirtySeams meanwhile, and the
	// arrival that makes it buildable re-dirties it through this same path and enqueues it then.
	if (!AreAllParticipantsResident(Seam.Key))
	{
		return;
	}
	if (Seam.ReadyAtSeconds == 0.0)
	{
		Seam.ReadyAtSeconds = Now();
	}
	if (!Seam.bQueued)
	{
		DirtyQueue.Add(Seam.Key);
		Seam.bQueued = true;
	}
}

void FVoxelSeamRegistry::DirtyIncidentSeams(const FIntVector& ChunkCoord)
{
	TArray<FVoxelSeamKey> Incident;
	EnumerateIncidentSeams(ChunkCoord, Incident);
	for (const FVoxelSeamKey& Key : Incident)
	{
		MarkSeamDirty(FindOrCreateSeam(Key));
	}
}

void FVoxelSeamRegistry::RemoveSeam(const FVoxelSeamKey& Key)
{
	Seams.Remove(Key);
	DirtySeams.Remove(Key);
	if (JobQueueSet.Remove(Key) > 0)
	{
		for (int32 i = JobQueue.Num() - 1; i >= 0; --i)
		{
			if (JobQueue[i].Key == Key)
			{
				JobQueue.RemoveAt(i);
				break;
			}
		}
	}
}

bool FVoxelSeamRegistry::AreAllParticipantsResident(const FVoxelSeamKey& Key) const
{
	TArray<FIntVector> Coords;
	GetParticipants(Key, Coords);
	for (const FIntVector& Coord : Coords)
	{
		if (!ChunkMirror.Contains(Coord))
		{
			return false;
		}
	}
	return true;
}

// ==================== Lifecycle hooks ====================

void FVoxelSeamRegistry::RegisterChunk(const FIntVector& ChunkCoord, uint32 ContentVersion, int32 MeshedLODLevel)
{
	if (!bEnabled)
	{
		return;
	}
	FChunkMirrorState& M = ChunkMirror.FindOrAdd(ChunkCoord);
	M.ContentVersion = ContentVersion;
	M.MeshedLODLevel = MeshedLODLevel;
	// A newly resident participant restales every boundary it touches.
	DirtyIncidentSeams(ChunkCoord);
}

void FVoxelSeamRegistry::UpdateChunkContent(const FIntVector& ChunkCoord, uint32 ContentVersion)
{
	if (!bEnabled)
	{
		return;
	}
	FChunkMirrorState& M = ChunkMirror.FindOrAdd(ChunkCoord);
	M.ContentVersion = ContentVersion;
	DirtyIncidentSeams(ChunkCoord);
}

void FVoxelSeamRegistry::UpdateChunkRenderedLOD(const FIntVector& ChunkCoord, int32 MeshedLODLevel)
{
	if (!bEnabled)
	{
		return;
	}
	FChunkMirrorState& M = ChunkMirror.FindOrAdd(ChunkCoord);
	if (M.MeshedLODLevel != MeshedLODLevel)
	{
		M.MeshedLODLevel = MeshedLODLevel;
		// A rendered-LOD change reaches the boundary (transition faces today); restale incident seams.
		DirtyIncidentSeams(ChunkCoord);
	}
}

void FVoxelSeamRegistry::UnregisterChunk(const FIntVector& ChunkCoord)
{
	if (!bEnabled)
	{
		return;
	}
	if (ChunkMirror.Remove(ChunkCoord) == 0)
	{
		return; // was not tracked
	}
	// The chunk left: each incident seam either loses a participant (restale) or, if no participant
	// remains, is pruned entirely so the registry doesn't grow unbounded behind the streaming front.
	TArray<FVoxelSeamKey> Incident;
	EnumerateIncidentSeams(ChunkCoord, Incident);
	for (const FVoxelSeamKey& Key : Incident)
	{
		TArray<FIntVector> Coords;
		GetParticipants(Key, Coords);
		bool bAnyResident = false;
		for (const FIntVector& Coord : Coords)
		{
			if (ChunkMirror.Contains(Coord))
			{
				bAnyResident = true;
				break;
			}
		}
		if (bAnyResident)
		{
			if (FVoxelSeamState* Seam = Seams.Find(Key))
			{
				MarkSeamDirty(*Seam);
			}
		}
		else
		{
			RemoveSeam(Key);
		}
	}
}

// ==================== Per-tick scheduler ====================

int32 FVoxelSeamRegistry::ScheduleReadySeams(const FIntVector& ViewerChunk, int32 NearChunkRadius, int32 MaxToSchedule)
{
	if (!bEnabled || DirtySeams.Num() == 0)
	{
		return 0;
	}

	// The rotation holds READY dirty seams only (MarkSeamDirty enqueues iff all participants are
	// resident), so nearly everything examined here schedules. Two exceptions are handled in the
	// loop: a seam whose previous job is still in flight is re-appended at the BACK and retried
	// next tick; a seam that LOST a participant since it was enqueued is dropped from the rotation
	// (it stays dirty in DirtySeams — that participant's return re-dirties and re-enqueues it).
	// Rotating rather than rescanning from the head keeps a burst of in-flight seams from starving
	// the ones behind them. Scan budget exceeds the schedule budget so ready seams behind in-flight
	// ones are still found within a tick.
	const int32 ScheduleBudget = (MaxToSchedule > 0) ? MaxToSchedule : MAX_int32;
	const int32 ScanBudget = (MaxToSchedule > 0)
		? FMath::Max(64, MaxToSchedule * 4)
		: (DirtyQueue.Num() - DirtyQueueHead);

	int32 Scheduled = 0;
	int32 Examined = 0;
	TArray<FVoxelSeamKey> Requeue;

	while (DirtyQueueHead < DirtyQueue.Num() && Examined < ScanBudget && Scheduled < ScheduleBudget)
	{
		const FVoxelSeamKey Key = DirtyQueue[DirtyQueueHead++];

		// Lazy deletion: entries whose seam was cleaned/scheduled/removed since queuing.
		if (!DirtySeams.Contains(Key))
		{
			if (FVoxelSeamState* Stale = Seams.Find(Key))
			{
				Stale->bQueued = false; // it has left the rotation; a future dirty may enqueue it again
			}
			continue;
		}

		FVoxelSeamState* Seam = Seams.Find(Key);
		if (!Seam || !Seam->bDirty)
		{
			DirtySeams.Remove(Key); // stale set entry — drop it
			if (Seam)
			{
				Seam->bQueued = false;
			}
			continue;
		}

		++Examined;
		++IntervalExamined;

		// Re-dirtied while its previous job is still in flight — retry once the job completes.
		if (Seam->bScheduled)
		{
			Requeue.Add(Key);
			++IntervalRequeuedInFlight;
			continue;
		}
		// A participant left after this seam was enqueued (UnregisterChunk re-dirties survivors but
		// cannot un-enqueue). Drop it from the rotation rather than requeue: it stays in DirtySeams,
		// and the participant's return re-dirties it through MarkSeamDirty, which enqueues it again.
		// The ready stamp is cleared so that return re-stamps it (scan wait must not include the gap).
		if (!AreAllParticipantsResident(Key))
		{
			Seam->bQueued = false;
			Seam->ReadyAtSeconds = 0.0;
			++IntervalRequeuedNotResident; // counts drops now; the log token stays "notResident"
			continue;
		}

		// Latency instrumentation: how long this seam sat dirty before the scan reached it in a
		// schedulable state. Split near/far so the player-visible tier is readable on its own.
		const double ScheduledAt = Now();
		if (Seam->DirtiedAtSeconds > 0.0)
		{
			const bool bNear = IsNearViewer(Key.Owner, ViewerChunk, NearChunkRadius);
			// Total wait since first dirtied (includes any time spent waiting for a participant).
			const float WaitMs = static_cast<float>((ScheduledAt - Seam->DirtiedAtSeconds) * 1000.0);
			(bNear ? NearScheduleWindow : FarScheduleWindow).Add(WaitMs);
			// Pure scan wait: from the moment the seam was buildable. Guard against a missing
			// ready stamp (it is set on the same dirty call that made the seam schedulable).
			const double From = (Seam->ReadyAtSeconds > 0.0) ? FMath::Max(Seam->DirtiedAtSeconds, Seam->ReadyAtSeconds) : Seam->DirtiedAtSeconds;
			const float ScanMs = static_cast<float>((ScheduledAt - From) * 1000.0);
			(bNear ? NearScanWindow : FarScanWindow).Add(ScanMs);
		}

		// Capture the launch-time participant snapshot (mirrors FMeshBoundaryDep per participant).
		for (FVoxelSeamParticipant& P : Seam->Participants)
		{
			const FChunkMirrorState& M = ChunkMirror.FindChecked(P.Coord);
			P.ContentVersion = M.ContentVersion;
			P.MeshedLODLevel = M.MeshedLODLevel;
			P.bResident = true;
		}

		FVoxelSeamJob Job;
		Job.Key = Key;
		Job.Priority = ComputeSeamPriority(Key.Owner, ViewerChunk, NearChunkRadius);
		Job.Participants = Seam->Participants;
		Job.DirtiedAtSeconds = Seam->DirtiedAtSeconds;
		Job.ReadyAtSeconds = Seam->ReadyAtSeconds;
		Job.ScheduledAtSeconds = ScheduledAt;
		// A seam scheduled again within 1 s of its previous build is a rebuild that the old ~300 ms
		// scan rotation would usually have coalesced into one job; count them so the throughput
		// cost of scheduling promptly is visible.
		if (Seam->LastScheduledAtSeconds > 0.0 && (ScheduledAt - Seam->LastScheduledAtSeconds) < 1.0)
		{
			++IntervalRescheduledWithin1s;
		}
		Seam->LastScheduledAtSeconds = ScheduledAt;

		// Insert priority-sorted (ascending; highest priority at the back for O(1) pop) — matches the
		// meshing queue. Dedup is guaranteed by the bScheduled flag below, so no set lookup needed here.
		const int32 InsertIndex = Algo::LowerBound(JobQueue, Job);
		JobQueue.Insert(Job, InsertIndex);
		JobQueueSet.Add(Key);

		Seam->bScheduled = true;
		Seam->bDirty = false;
		Seam->DirtiedAtSeconds = 0.0; // no longer waiting; a re-dirty starts a fresh wait
		Seam->ReadyAtSeconds = 0.0;
		Seam->bQueued = false; // out of the rotation until the next dirty
		DirtySeams.Remove(Key);
		++Scheduled;
		++IntervalScheduled;
		++TotalSeamJobsScheduled;
	}

	// Still-dirty candidates rejoin at the back; compact the consumed head region once it dominates.
	DirtyQueue.Append(Requeue);
	if (DirtyQueueHead > 1024 && DirtyQueueHead * 2 > DirtyQueue.Num())
	{
		DirtyQueue.RemoveAt(0, DirtyQueueHead);
		DirtyQueueHead = 0;
	}
	return Scheduled;
}

int32 FVoxelSeamRegistry::ProcessSeamJobQueue(int32 MaxJobs)
{
	if (!bEnabled || JobQueue.Num() == 0)
	{
		return 0;
	}

	int32 Processed = 0;
	while (JobQueue.Num() > 0 && (MaxJobs <= 0 || Processed < MaxJobs))
	{
		// Pop highest priority (back of the ascending-sorted array).
		FVoxelSeamJob Job = JobQueue.Pop(EAllowShrinking::No);
		JobQueueSet.Remove(Job.Key);

		// -------------------------------------------------------------------------------------------
		// P0 STUB: a seam job produces NO geometry. This is where P1+ will run the actual seam mesher
		// (DC face seam, then mixed-LOD edges/corners, then MC transvoxel) against Job.Participants.
		// For now we only log/count so the scaffolding is observable and the no-op is provable.
		// -------------------------------------------------------------------------------------------
		if (bDebugLogging)
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("[Seam] stub job %s prio=%.1f participants=%d (no geometry — P0)"),
				*Job.Key.ToString(), Job.Priority, Job.Participants.Num());
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Verbose, TEXT("[Seam] stub job %s prio=%.1f participants=%d"),
				*Job.Key.ToString(), Job.Priority, Job.Participants.Num());
		}

		// The stub "completes" immediately: mark the seam not-scheduled (and it stays clean until a
		// participant next changes). P1+ will instead submit a seam mesh section here.
		if (FVoxelSeamState* Seam = Seams.Find(Job.Key))
		{
			Seam->bScheduled = false;
		}
		++Processed;
		++TotalSeamJobsProcessed;
	}
	return Processed;
}

int32 FVoxelSeamRegistry::DrainSeamJobs(TArray<FVoxelSeamJob>& OutJobs, int32 MaxJobs)
{
	if (!bEnabled || JobQueue.Num() == 0 || MaxJobs <= 0)
	{
		return 0;
	}

	int32 Drained = 0;
	while (JobQueue.Num() > 0 && Drained < MaxJobs)
	{
		// Pop highest priority (back of the ascending-sorted array).
		FVoxelSeamJob Job = JobQueue.Pop(EAllowShrinking::No);
		JobQueueSet.Remove(Job.Key);

		// Mark not-scheduled: a participant change during the external job's flight re-dirties and
		// re-schedules the seam through the normal flow (the newer result simply resubmits).
		if (FVoxelSeamState* Seam = Seams.Find(Job.Key))
		{
			Seam->bScheduled = false;
		}

		OutJobs.Add(MoveTemp(Job));
		++Drained;
		++TotalSeamJobsProcessed;
	}
	return Drained;
}

// ==================== Queries ====================

FVoxelSeamRegistryStats FVoxelSeamRegistry::GetStats() const
{
	FVoxelSeamRegistryStats S;
	S.ChunkCount = ChunkMirror.Num();
	S.SeamCount = Seams.Num();
	S.DirtyCount = DirtySeams.Num();
	S.JobQueueDepth = JobQueue.Num();
	S.TotalSeamsCreated = TotalSeamsCreated;
	S.TotalSeamJobsScheduled = TotalSeamJobsScheduled;
	S.TotalSeamJobsProcessed = TotalSeamJobsProcessed;
	return S;
}

bool FVoxelSeamRegistry::IsSeamReady(const FVoxelSeamKey& Key) const
{
	return Seams.Contains(Key) && AreAllParticipantsResident(Key);
}

bool FVoxelSeamRegistry::IsSeamDirty(const FVoxelSeamKey& Key) const
{
	const FVoxelSeamState* Seam = Seams.Find(Key);
	return Seam && Seam->bDirty;
}

void FVoxelSeamRegistry::GetDirtySeamKeys(TArray<FVoxelSeamKey>& OutKeys) const
{
	OutKeys = DirtySeams.Array();
}

void FVoxelSeamRegistry::GetAllSeamKeys(TArray<FVoxelSeamKey>& OutKeys) const
{
	Seams.GetKeys(OutKeys);
}

void FVoxelSeamRegistry::MarkAllSeamsClean()
{
	for (auto& Pair : Seams)
	{
		Pair.Value.bDirty = false;
		Pair.Value.bQueued = false;
	}
	DirtySeams.Reset();
	DirtyQueue.Reset();
	DirtyQueueHead = 0;
}
