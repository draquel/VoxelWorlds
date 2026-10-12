// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelSeamRegistry.h"

/**
 * Coordinated LOD transitions for the seam pipeline (VoxelStreaming, R24 P2).
 *
 * A chunk's LOD change swaps its interior mesh (which crossfades) and dirties every seam it
 * touches; the rebuilt seams land a frame or two later. Until they do, the visible seams match the
 * OLD interior while the fade is already showing the new one, and the old interior's rim no longer
 * meets the new seam ring: a brief opening in the terrain. This tracker holds the chunk's fade at
 * alpha 0 (old interior + old seams fully visible; the new interior is uploaded but dithered out)
 * until the incident seams have landed, holding their new seam buckets the same way, and then
 * releases the whole set together so one shared fade takes old {interior + seams} to new
 * {interior + seams}. Every pixel always shows one complete surface.
 *
 * Transitions whose sets overlap (adjacent chunks changing LOD in the same few frames, which is how
 * an LOD ring moves) share seams and seam buckets, so they merge into one group and release
 * together. A group is released when nothing is pending or when it has waited HoldSeconds,
 * whichever comes first, so a seam that never lands can only delay detail by the cap.
 *
 * Deferred buckets: an owner's seam bucket merges up to 7 slots, so it is fading from an earlier
 * result whenever a later slot lands within the fade. Replacing it mid-fade drops the generation
 * it fades out of — the slots of a transition that is still crossfading snap to its end state and
 * open the same kind of gap. The caller instead defers such a bucket (DeferBucket) until the running
 * fade ends and submits the merged slots then (CollectDueBuckets); a group with a deferred bucket
 * waits for it, so its interiors and buckets still start their fade on the same frame.
 *
 * Pure bookkeeping (no renderer, no world): the chunk manager feeds it events and applies the
 * releases. Game-thread only.
 *
 * @see UVoxelChunkManager (OnChunkMeshingComplete / ProcessCompletedSeamMeshes / TickSeamScheduler)
 * @see Documentation/VOXEL_QA_FOLLOWUPS_PLAN.md (R24)
 */
class VOXELSTREAMING_API FVoxelSeamTransitionHolds
{
public:
	/** What one CollectReleases call let go. */
	struct FRelease
	{
		/** Chunks whose interior fade may start now. */
		TArray<FIntVector> Chunks;
		/** Seam bucket owners whose fade may start now. */
		TArray<FIntVector> SeamOwners;
		/** Groups released this call. */
		int32 Groups = 0;
		/** Of those, released by the HoldSeconds cap rather than by completion. */
		int32 TimedOut = 0;
		/** Seams still pending in the timed-out groups (they will land and fade on their own). */
		int32 SeamsLeftPending = 0;
		/** Hold duration of each released group, seconds. */
		TArray<float> HoldSeconds;
	};

	/**
	 * A chunk's interior was just submitted at a new LOD with its fade held. Waits for
	 * PendingSeams (the seams its LOD change will rebuild — FVoxelSeamRegistry::GetReadyIncidentSeams).
	 * Merges with any open group that shares a seam, a seam bucket owner or the chunk itself.
	 *
	 * @param Chunk        The chunk whose interior swap is held.
	 * @param PendingSeams Seams whose rebuilt results release the hold.
	 * @param NowSeconds   Current time (the group's hold clock starts at its earliest member).
	 */
	void BeginTransition(const FIntVector& Chunk, const TArray<FVoxelSeamKey>& PendingSeams, double NowSeconds);

	/**
	 * A seam result is about to be submitted to the renderer. Stops waiting for it and reports
	 * whether its owner's bucket must be held: true when the seam was pending in a group or the
	 * owner's bucket is already held by one (the bucket merges every slot, so one held slot holds
	 * the bucket).
	 *
	 * @param Key The seam whose result landed.
	 * @return True if the caller must hold the owner's bucket fade until the group releases.
	 */
	bool OnSeamResult(const FVoxelSeamKey& Key);

	/**
	 * The seam will not land (job dropped, result rejected or failed, a participant left): stop
	 * waiting for it. No-op if it is not pending.
	 *
	 * @param Key The abandoned seam.
	 */
	void OnSeamAbandoned(const FVoxelSeamKey& Key);

	/**
	 * A chunk left (unloaded). Its interior fade is gone with it, and seams it participates in
	 * cannot land any more, so they stop being waited for.
	 *
	 * @param Chunk The departed chunk.
	 */
	void OnChunkRemoved(const FIntVector& Chunk);

	/**
	 * The owner's seam bucket is still fading from an earlier submit: submit its merged slots at
	 * DueSeconds instead of now. Repeated calls keep the later due time. A group holding the owner
	 * does not release until the bucket has been submitted.
	 *
	 * @param Owner      The seam bucket owner.
	 * @param DueSeconds When the running fade ends.
	 */
	void DeferBucket(const FIntVector& Owner, double DueSeconds);

	/**
	 * Hand back every deferred bucket that is due; they stop being deferred (re-defer one whose fade
	 * turns out to still be running).
	 *
	 * @param NowSeconds Current time.
	 * @param OutOwners  Receives the owners to submit now (appended).
	 */
	void CollectDueBuckets(double NowSeconds, TArray<FIntVector>& OutOwners);

	/** True while the owner's bucket submit is deferred. */
	bool IsBucketDeferred(const FIntVector& Owner) const { return DeferredBuckets.Contains(Owner); }

	/** Deferred bucket submits outstanding. */
	int32 NumDeferredBuckets() const { return DeferredBuckets.Num(); }

	/**
	 * Release every group that has nothing pending and no deferred bucket, or has waited at least
	 * HoldSeconds.
	 *
	 * @param NowSeconds  Current time.
	 * @param HoldSeconds Cap on any group's wait (<= 0 releases everything immediately).
	 * @param Out         Receives the released chunks and seam owners plus stats (appended).
	 */
	void CollectReleases(double NowSeconds, double HoldSeconds, FRelease& Out);

	/** Release everything at once (shutdown, seam pipeline toggled off). Deferred buckets are dropped. */
	void ReleaseAll(FRelease& Out);

	/** True while the chunk's interior fade is held by an open group. */
	bool IsChunkHeld(const FIntVector& Chunk) const { return ChunkToGroup.Contains(Chunk); }

	/** True while the owner's seam bucket is held by an open group. */
	bool IsSeamOwnerHeld(const FIntVector& Owner) const { return OwnerToGroup.Contains(Owner); }

	/** True while the seam is still awaited by an open group. */
	bool IsSeamPending(const FVoxelSeamKey& Key) const { return PendingToGroup.Contains(Key); }

	/** Open groups. */
	int32 NumGroups() const { return Groups.Num(); }

	/** Seams awaited across all open groups. */
	int32 NumPendingSeams() const { return PendingToGroup.Num(); }

private:
	struct FGroup
	{
		TSet<FIntVector> Chunks;
		TSet<FVoxelSeamKey> Pending;
		TSet<FIntVector> SeamOwners;
		double StartSeconds = 0.0;
	};

	/** Fold group B into group A (all lookups re-pointed); returns A. */
	int32 MergeGroups(int32 A, int32 B);

	/** Remove a group and its lookups, reporting its members into Out (NowSeconds < 0 records no duration). */
	void ReleaseGroup(int32 GroupId, bool bTimedOut, double NowSeconds, FRelease& Out);

	TMap<int32, FGroup> Groups;
	int32 NextGroupId = 1;
	TMap<FIntVector, int32> ChunkToGroup;
	TMap<FVoxelSeamKey, int32> PendingToGroup;
	TMap<FIntVector, int32> OwnerToGroup;
	/** Seam bucket owner -> when its running fade ends and its merged slots may be submitted. */
	TMap<FIntVector, double> DeferredBuckets;
};
