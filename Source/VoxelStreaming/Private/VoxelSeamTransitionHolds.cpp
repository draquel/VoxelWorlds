// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSeamTransitionHolds.h"

// See VoxelSeamTransitionHolds.h. Pure bookkeeping: groups of held chunk interiors and seam
// buckets waiting for the seams an LOD change rebuilds.

int32 FVoxelSeamTransitionHolds::MergeGroups(int32 A, int32 B)
{
	if (A == B)
	{
		return A;
	}
	FGroup* GroupA = Groups.Find(A);
	FGroup* GroupB = Groups.Find(B);
	if (!GroupA || !GroupB)
	{
		return GroupA ? A : B;
	}

	for (const FIntVector& Chunk : GroupB->Chunks)
	{
		GroupA->Chunks.Add(Chunk);
		ChunkToGroup.Add(Chunk, A);
	}
	for (const FVoxelSeamKey& Key : GroupB->Pending)
	{
		GroupA->Pending.Add(Key);
		PendingToGroup.Add(Key, A);
	}
	for (const FIntVector& Owner : GroupB->SeamOwners)
	{
		GroupA->SeamOwners.Add(Owner);
		OwnerToGroup.Add(Owner, A);
	}
	// The merged group keeps the earliest clock: nothing waits longer than the cap from the first
	// member's start, however many transitions join it.
	GroupA->StartSeconds = FMath::Min(GroupA->StartSeconds, GroupB->StartSeconds);
	Groups.Remove(B);
	return A;
}

void FVoxelSeamTransitionHolds::BeginTransition(const FIntVector& Chunk, const TArray<FVoxelSeamKey>& PendingSeams, double NowSeconds)
{
	// Every open group this transition touches: the chunk itself, a seam it waits for, or a seam
	// bucket that one of those seams will be merged into.
	TArray<int32, TInlineAllocator<8>> Touched;
	if (const int32* Existing = ChunkToGroup.Find(Chunk))
	{
		Touched.AddUnique(*Existing);
	}
	for (const FVoxelSeamKey& Key : PendingSeams)
	{
		if (const int32* ByPending = PendingToGroup.Find(Key))
		{
			Touched.AddUnique(*ByPending);
		}
		if (const int32* ByOwner = OwnerToGroup.Find(Key.Owner))
		{
			Touched.AddUnique(*ByOwner);
		}
	}

	int32 Target = INDEX_NONE;
	if (Touched.Num() == 0)
	{
		Target = NextGroupId++;
		Groups.Add(Target).StartSeconds = NowSeconds;
	}
	else
	{
		Target = Touched[0];
		for (int32 i = 1; i < Touched.Num(); ++i)
		{
			Target = MergeGroups(Target, Touched[i]);
		}
	}

	FGroup& Group = Groups.FindChecked(Target);
	Group.Chunks.Add(Chunk);
	ChunkToGroup.Add(Chunk, Target);
	for (const FVoxelSeamKey& Key : PendingSeams)
	{
		Group.Pending.Add(Key);
		PendingToGroup.Add(Key, Target);
	}
}

bool FVoxelSeamTransitionHolds::OnSeamResult(const FVoxelSeamKey& Key)
{
	int32 GroupId = INDEX_NONE;
	if (const int32* ByPending = PendingToGroup.Find(Key))
	{
		GroupId = *ByPending;
		if (FGroup* Group = Groups.Find(GroupId))
		{
			Group->Pending.Remove(Key);
		}
		PendingToGroup.Remove(Key);
	}
	if (const int32* ByOwner = OwnerToGroup.Find(Key.Owner))
	{
		GroupId = (GroupId == INDEX_NONE) ? *ByOwner : MergeGroups(GroupId, *ByOwner);
	}
	if (GroupId == INDEX_NONE)
	{
		return false;
	}

	FGroup& Group = Groups.FindChecked(GroupId);
	Group.SeamOwners.Add(Key.Owner);
	OwnerToGroup.Add(Key.Owner, GroupId);
	return true;
}

void FVoxelSeamTransitionHolds::OnSeamAbandoned(const FVoxelSeamKey& Key)
{
	int32 GroupId = INDEX_NONE;
	if (PendingToGroup.RemoveAndCopyValue(Key, GroupId))
	{
		if (FGroup* Group = Groups.Find(GroupId))
		{
			Group->Pending.Remove(Key);
		}
	}
}

void FVoxelSeamTransitionHolds::OnChunkRemoved(const FIntVector& Chunk)
{
	int32 GroupId = INDEX_NONE;
	if (ChunkToGroup.RemoveAndCopyValue(Chunk, GroupId))
	{
		if (FGroup* Group = Groups.Find(GroupId))
		{
			Group->Chunks.Remove(Chunk);
		}
	}
	// Its seam bucket is removed with it (nothing left to release or submit).
	DeferredBuckets.Remove(Chunk);
	if (OwnerToGroup.RemoveAndCopyValue(Chunk, GroupId))
	{
		if (FGroup* Group = Groups.Find(GroupId))
		{
			Group->SeamOwners.Remove(Chunk);
		}
	}
	// Seams it participates in cannot be built any more.
	TArray<FVoxelSeamKey> Incident;
	FVoxelSeamRegistry::EnumerateIncidentSeams(Chunk, Incident);
	for (const FVoxelSeamKey& Key : Incident)
	{
		OnSeamAbandoned(Key);
	}
}

void FVoxelSeamTransitionHolds::DeferBucket(const FIntVector& Owner, double DueSeconds)
{
	double& Due = DeferredBuckets.FindOrAdd(Owner, DueSeconds);
	Due = FMath::Max(Due, DueSeconds);
}

void FVoxelSeamTransitionHolds::CollectDueBuckets(double NowSeconds, TArray<FIntVector>& OutOwners)
{
	for (auto It = DeferredBuckets.CreateIterator(); It; ++It)
	{
		if (NowSeconds >= It.Value())
		{
			OutOwners.Add(It.Key());
			It.RemoveCurrent();
		}
	}
}

void FVoxelSeamTransitionHolds::ReleaseGroup(int32 GroupId, bool bTimedOut, double NowSeconds, FRelease& Out)
{
	FGroup Group;
	if (!Groups.RemoveAndCopyValue(GroupId, Group))
	{
		return;
	}
	for (const FIntVector& Chunk : Group.Chunks)
	{
		ChunkToGroup.Remove(Chunk);
		Out.Chunks.Add(Chunk);
	}
	for (const FVoxelSeamKey& Key : Group.Pending)
	{
		PendingToGroup.Remove(Key);
	}
	for (const FIntVector& Owner : Group.SeamOwners)
	{
		OwnerToGroup.Remove(Owner);
		Out.SeamOwners.Add(Owner);
	}
	++Out.Groups;
	if (bTimedOut)
	{
		++Out.TimedOut;
		Out.SeamsLeftPending += Group.Pending.Num();
	}
	if (NowSeconds >= 0.0)
	{
		Out.HoldSeconds.Add(static_cast<float>(FMath::Max(0.0, NowSeconds - Group.StartSeconds)));
	}
}

void FVoxelSeamTransitionHolds::CollectReleases(double NowSeconds, double HoldSeconds, FRelease& Out)
{
	TArray<TPair<int32, bool>, TInlineAllocator<16>> ToRelease;
	for (const TPair<int32, FGroup>& Pair : Groups)
	{
		// Complete = every awaited seam has landed AND every bucket it holds has been submitted (a
		// deferred one still shows its previous fade; releasing now would start the interiors first).
		bool bBucketsSubmitted = true;
		for (const FIntVector& Owner : Pair.Value.SeamOwners)
		{
			if (DeferredBuckets.Contains(Owner))
			{
				bBucketsSubmitted = false;
				break;
			}
		}
		const bool bComplete = Pair.Value.Pending.Num() == 0 && bBucketsSubmitted;
		const bool bTimedOut = !bComplete && (HoldSeconds <= 0.0 || NowSeconds - Pair.Value.StartSeconds >= HoldSeconds);
		if (bComplete || bTimedOut)
		{
			ToRelease.Emplace(Pair.Key, bTimedOut);
		}
	}
	for (const TPair<int32, bool>& Entry : ToRelease)
	{
		ReleaseGroup(Entry.Key, Entry.Value, NowSeconds, Out);
	}
}

void FVoxelSeamTransitionHolds::ReleaseAll(FRelease& Out)
{
	DeferredBuckets.Empty();
	TArray<int32> Ids;
	Groups.GetKeys(Ids);
	for (const int32 Id : Ids)
	{
		ReleaseGroup(Id, /*bTimedOut*/ false, /*NowSeconds (no duration recorded)*/ -1.0, Out);
	}
}
