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

float FVoxelSeamRegistry::ComputeSeamPriority(const FIntVector& OwnerChunk, const FIntVector& ViewerChunk, int32 NearChunkRadius)
{
	const FIntVector Delta = OwnerChunk - ViewerChunk;
	const int32 Cheb = FMath::Max3(FMath::Abs(Delta.X), FMath::Abs(Delta.Y), FMath::Abs(Delta.Z));
	if (Cheb <= NearChunkRadius)
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
		// First transition to dirty: join the round-robin scan queue. Re-dirtying while already
		// queued adds nothing (the existing entry still gets examined).
		DirtyQueue.Add(Seam.Key);
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

	// Round-robin scan: pop candidates from the head of DirtyQueue; a candidate that cannot
	// schedule yet (participant missing, or still in flight) is re-appended at the BACK so the
	// next tick examines DIFFERENT seams. A bounded scan from a fixed starting point would
	// re-examine the same not-yet-ready frontier every tick and starve everything behind it
	// (observed live: thousands dirty, 2 scheduled). Scan budget exceeds the schedule budget so
	// ready seams hiding behind unready ones are still found within a tick.
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
			continue;
		}

		FVoxelSeamState* Seam = Seams.Find(Key);
		if (!Seam || !Seam->bDirty)
		{
			DirtySeams.Remove(Key); // stale set entry — drop it
			continue;
		}

		++Examined;

		// Re-dirtied while its previous job is still in flight — retry once the job completes.
		if (Seam->bScheduled)
		{
			Requeue.Add(Key);
			continue;
		}
		// Gate: only schedule a seam once ALL its participants are resident (both/all sides' data
		// present) — the same "wait for neighbours" the meshing scheduler enforces.
		if (!AreAllParticipantsResident(Key))
		{
			Requeue.Add(Key); // stays dirty; back of the rotation
			continue;
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

		// Insert priority-sorted (ascending; highest priority at the back for O(1) pop) — matches the
		// meshing queue. Dedup is guaranteed by the bScheduled flag below, so no set lookup needed here.
		const int32 InsertIndex = Algo::LowerBound(JobQueue, Job);
		JobQueue.Insert(Job, InsertIndex);
		JobQueueSet.Add(Key);

		Seam->bScheduled = true;
		Seam->bDirty = false;
		DirtySeams.Remove(Key);
		++Scheduled;
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
	}
	DirtySeams.Reset();
	DirtyQueue.Reset();
	DirtyQueueHead = 0;
}
