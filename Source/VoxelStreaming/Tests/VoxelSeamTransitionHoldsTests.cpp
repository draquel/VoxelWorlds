// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSeamRegistry.h"
#include "VoxelSeamTransitionHolds.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Coordinated LOD transitions (VOXEL_QA_FOLLOWUPS_PLAN.md R24 P2). Pure logic, runs headless.
// An LOD change holds the chunk's interior fade until the seams it rebuilds have landed, then
// the interior and those seam buckets fade together. These tests pin which seams are waited
// for, when the hold lets go, and how overlapping transitions group.
// ---------------------------------------------------------------------------

namespace VoxelSeamHoldTestUtils
{
	static void RegisterCube(FVoxelSeamRegistry& Reg, int32 N)
	{
		for (int32 z = 0; z < N; ++z)
		{
			for (int32 y = 0; y < N; ++y)
			{
				for (int32 x = 0; x < N; ++x)
				{
					Reg.RegisterChunk(FIntVector(x, y, z), /*ContentVersion*/ 1, /*MeshedLODLevel*/ 0);
				}
			}
		}
	}

	static bool Touches(const FVoxelSeamKey& Key, const FIntVector& Chunk)
	{
		return FVoxelSeamRegistry::GetParticipants(Key).Contains(Chunk);
	}
}

/**
 * An LOD change waits for exactly the chunk's buildable incident seams, and the hold empties as
 * those seams land or a participant leaves; then the chunk and every held bucket are released
 * together, not by the timeout.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSeamLODChangeHoldSetTest,
	"VoxelWorlds.Streaming.SeamRegistry.LODChangeHoldSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSeamLODChangeHoldSetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelSeamHoldTestUtils;

	FVoxelSeamRegistry Reg; Reg.SetEnabled(true);
	RegisterCube(Reg, 3);
	const FIntVector Center(1, 1, 1);
	const FIntVector Corner(0, 0, 0);

	// The hold set is the buildable incident seams: all 26 for a chunk with a full neighbourhood,
	// only the 7 inside the cube for a corner chunk (the rest have an absent participant).
	TArray<FVoxelSeamKey> CenterSet;
	Reg.UpdateChunkRenderedLOD(Center, /*lod*/ 1);
	Reg.GetReadyIncidentSeams(Center, CenterSet);
	TestEqual(TEXT("center: every incident seam is buildable"), CenterSet.Num(), 26);
	TestEqual(TEXT("center: the set is exactly the incident seams"),
		TSet<FVoxelSeamKey>(CenterSet).Num(), TSet<FVoxelSeamKey>(FVoxelSeamRegistry::EnumerateIncidentSeams(Center)).Num());
	TArray<FVoxelSeamKey> CornerSet;
	Reg.GetReadyIncidentSeams(Corner, CornerSet);
	TestEqual(TEXT("corner: only seams with every participant resident"), CornerSet.Num(), 7);

	// Begin the transition: the interior is held and all 26 seams are awaited.
	FVoxelSeamTransitionHolds Holds;
	Holds.BeginTransition(Center, CenterSet, /*now*/ 0.0);
	TestTrue(TEXT("interior held"), Holds.IsChunkHeld(Center));
	TestEqual(TEXT("26 seams pending"), Holds.NumPendingSeams(), 26);

	// Results land for every seam not touching the +X neighbour: each holds its owner's bucket.
	const FIntVector Neighbour(2, 1, 1);
	int32 Landed = 0;
	for (const FVoxelSeamKey& Key : CenterSet)
	{
		if (!Touches(Key, Neighbour))
		{
			TestTrue(*FString::Printf(TEXT("result for %s holds its bucket"), *Key.ToString()), Holds.OnSeamResult(Key));
			TestTrue(TEXT("its owner bucket is held"), Holds.IsSeamOwnerHeld(Key.Owner));
			++Landed;
		}
	}
	TestEqual(TEXT("17 seams do not touch the neighbour (26 - 9 shared)"), Landed, 17);
	TestEqual(TEXT("9 still pending"), Holds.NumPendingSeams(), 9);

	FVoxelSeamTransitionHolds::FRelease Early;
	Holds.CollectReleases(/*now*/ 0.05, /*hold*/ 0.15, Early);
	TestEqual(TEXT("nothing released while seams are pending and within the cap"), Early.Groups, 0);
	TestTrue(TEXT("still held"), Holds.IsChunkHeld(Center));

	// The neighbour unloads: the 9 seams it shares can never land, so the hold empties.
	Holds.OnChunkRemoved(Neighbour);
	TestEqual(TEXT("participant left: nothing pending"), Holds.NumPendingSeams(), 0);

	FVoxelSeamTransitionHolds::FRelease Done;
	Holds.CollectReleases(/*now*/ 0.06, /*hold*/ 0.15, Done);
	TestEqual(TEXT("released as one group"), Done.Groups, 1);
	TestEqual(TEXT("released by completion, not the cap"), Done.TimedOut, 0);
	TestTrue(TEXT("the interior is released"), Done.Chunks.Contains(Center));
	TestTrue(TEXT("held buckets are released with it"), Done.SeamOwners.Num() > 0);
	for (const FVoxelSeamKey& Key : CenterSet)
	{
		if (!Touches(Key, Neighbour))
		{
			TestTrue(TEXT("every landed seam's bucket is released"), Done.SeamOwners.Contains(Key.Owner));
		}
	}
	TestFalse(TEXT("no longer held"), Holds.IsChunkHeld(Center));
	TestEqual(TEXT("no open groups"), Holds.NumGroups(), 0);
	if (Done.HoldSeconds.Num() == 1)
	{
		TestEqual(TEXT("hold duration recorded"), Done.HoldSeconds[0], 0.06f, 1e-4f);
	}

	// A result for a seam no transition waits for does not hold anything.
	TestFalse(TEXT("unrelated seam result is not held"), Holds.OnSeamResult(CenterSet[0]));
	return true;
}

/**
 * A seam that never lands cannot hold the chunk past the cap: the group times out and reports
 * what was still pending.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSeamHoldTimesOutTest,
	"VoxelWorlds.Streaming.SeamRegistry.LODChangeHoldTimesOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSeamHoldTimesOutTest::RunTest(const FString& Parameters)
{
	FVoxelSeamTransitionHolds Holds;
	const FIntVector Chunk(4, 0, 2);
	const TArray<FVoxelSeamKey> Pending = FVoxelSeamRegistry::EnumerateIncidentSeams(Chunk);
	Holds.BeginTransition(Chunk, Pending, /*now*/ 10.0);

	FVoxelSeamTransitionHolds::FRelease Before;
	Holds.CollectReleases(/*now*/ 10.1, /*hold*/ 0.15, Before);
	TestEqual(TEXT("within the cap: held"), Before.Groups, 0);

	FVoxelSeamTransitionHolds::FRelease After;
	Holds.CollectReleases(/*now*/ 10.15, /*hold*/ 0.15, After);
	TestEqual(TEXT("cap reached: released"), After.Groups, 1);
	TestEqual(TEXT("released by the cap"), After.TimedOut, 1);
	TestEqual(TEXT("reports the seams still pending"), After.SeamsLeftPending, 26);
	TestTrue(TEXT("chunk released"), After.Chunks.Contains(Chunk));
	TestEqual(TEXT("pending cleared"), Holds.NumPendingSeams(), 0);

	// Hold disabled (<= 0): everything goes straight through.
	Holds.BeginTransition(Chunk, Pending, /*now*/ 20.0);
	FVoxelSeamTransitionHolds::FRelease Disabled;
	Holds.CollectReleases(/*now*/ 20.0, /*hold*/ 0.0, Disabled);
	TestEqual(TEXT("hold 0 releases immediately"), Disabled.Groups, 1);
	return true;
}

/**
 * Adjacent chunks changing LOD together (a moving LOD ring) share seams, so their transitions
 * merge and release together on one clock; a transition with nothing to wait for releases at once.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSeamHoldGroupsMergeTest,
	"VoxelWorlds.Streaming.SeamRegistry.LODChangeHoldGroupsMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSeamHoldGroupsMergeTest::RunTest(const FString& Parameters)
{
	FVoxelSeamTransitionHolds Holds;
	const FIntVector A(0, 0, 0);
	const FIntVector B(1, 0, 0); // shares 9 seams with A
	const FIntVector Far(10, 0, 0);

	Holds.BeginTransition(A, FVoxelSeamRegistry::EnumerateIncidentSeams(A), /*now*/ 0.0);
	Holds.BeginTransition(B, FVoxelSeamRegistry::EnumerateIncidentSeams(B), /*now*/ 0.03);
	Holds.BeginTransition(Far, FVoxelSeamRegistry::EnumerateIncidentSeams(Far), /*now*/ 0.03);
	TestEqual(TEXT("A and B merged; Far separate"), Holds.NumGroups(), 2);
	TestEqual(TEXT("distinct pending seams: 26 + 26 - 9 + 26"), Holds.NumPendingSeams(), 69);

	// Land everything of A and B: they release together, at the earliest start (0.0).
	TSet<FVoxelSeamKey> AB(FVoxelSeamRegistry::EnumerateIncidentSeams(A));
	AB.Append(FVoxelSeamRegistry::EnumerateIncidentSeams(B));
	for (const FVoxelSeamKey& Key : AB)
	{
		Holds.OnSeamResult(Key);
	}
	FVoxelSeamTransitionHolds::FRelease R;
	Holds.CollectReleases(/*now*/ 0.05, /*hold*/ 0.15, R);
	TestEqual(TEXT("one group released"), R.Groups, 1);
	TestTrue(TEXT("A released"), R.Chunks.Contains(A));
	TestTrue(TEXT("B released with A"), R.Chunks.Contains(B));
	TestFalse(TEXT("Far still held"), R.Chunks.Contains(Far));
	if (R.HoldSeconds.Num() == 1)
	{
		TestEqual(TEXT("merged group keeps the earliest clock"), R.HoldSeconds[0], 0.05f, 1e-4f);
	}

	// A transition with no buildable seams has nothing to wait for.
	const FIntVector Lonely(50, 50, 50);
	Holds.BeginTransition(Lonely, TArray<FVoxelSeamKey>(), /*now*/ 1.0);
	FVoxelSeamTransitionHolds::FRelease Immediate;
	Holds.CollectReleases(/*now*/ 1.0, /*hold (long, so only completion releases)*/ 10.0, Immediate);
	TestTrue(TEXT("nothing pending: released at once"), Immediate.Chunks.Contains(Lonely));
	TestFalse(TEXT("Far still waiting within the long cap"), Immediate.Chunks.Contains(Far));

	FVoxelSeamTransitionHolds::FRelease All;
	Holds.ReleaseAll(All);
	TestTrue(TEXT("ReleaseAll lets Far go"), All.Chunks.Contains(Far));
	TestEqual(TEXT("nothing left"), Holds.NumGroups(), 0);
	return true;
}

/**
 * A seam bucket still fading from an earlier submit is not replaced mid-fade (that drops the mesh
 * it fades out of): its submit is deferred until the fade ends, and a transition holding that
 * owner waits for the deferred submit even after all its seams landed, so its interiors and its
 * buckets still start their fade together. An unload drops the deferral.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSeamHoldDeferredBucketTest,
	"VoxelWorlds.Streaming.SeamRegistry.LODChangeHoldDeferredBucket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSeamHoldDeferredBucketTest::RunTest(const FString& Parameters)
{
	FVoxelSeamTransitionHolds Holds;
	const FIntVector A(0, 0, 0);
	const FVoxelSeamKey Face = FVoxelSeamKey(A, EVoxelSeamType::Face, 0); // owner A, +X face

	// A transition waits for one seam; its result lands while A's bucket is still fading (until 0.30).
	Holds.BeginTransition(FIntVector(1, 0, 0), { Face }, /*now*/ 0.0);
	TestTrue(TEXT("the awaited seam holds its owner's bucket"), Holds.OnSeamResult(Face));
	Holds.DeferBucket(A, /*due*/ 0.30);
	Holds.DeferBucket(A, /*due (earlier: the later due time is kept)*/ 0.20);
	TestTrue(TEXT("bucket deferred"), Holds.IsBucketDeferred(A));

	// Nothing pending, but the bucket has not been submitted: the group waits.
	FVoxelSeamTransitionHolds::FRelease Early;
	Holds.CollectReleases(/*now*/ 0.10, /*hold*/ 1.0, Early);
	TestEqual(TEXT("no release while the bucket is deferred"), Early.Groups, 0);
	TArray<FIntVector> NotYet;
	Holds.CollectDueBuckets(/*now*/ 0.25, NotYet);
	TestEqual(TEXT("not due before its fade ends"), NotYet.Num(), 0);

	// The fade ends: the bucket is handed back (the caller submits it, held), then the group releases.
	TArray<FIntVector> Due;
	Holds.CollectDueBuckets(/*now*/ 0.30, Due);
	TestEqual(TEXT("due at fade end"), Due.Num(), 1);
	TestFalse(TEXT("no longer deferred"), Holds.IsBucketDeferred(A));
	TestTrue(TEXT("still held for the release"), Holds.IsSeamOwnerHeld(A));
	FVoxelSeamTransitionHolds::FRelease Released;
	Holds.CollectReleases(/*now*/ 0.30, /*hold*/ 1.0, Released);
	TestEqual(TEXT("released after the submit"), Released.Groups, 1);
	TestEqual(TEXT("by completion, not the cap"), Released.TimedOut, 0);
	TestTrue(TEXT("chunk and bucket released together"),
		Released.Chunks.Contains(FIntVector(1, 0, 0)) && Released.SeamOwners.Contains(A));

	// A deferral with no group (an independent result) just waits for its due time; an unload drops it.
	const FIntVector B(5, 0, 0);
	TestFalse(TEXT("independent result: nothing to hold"), Holds.OnSeamResult(FVoxelSeamKey(B, EVoxelSeamType::Face, 1)));
	Holds.DeferBucket(B, /*due*/ 1.0);
	Holds.OnChunkRemoved(B);
	TestFalse(TEXT("unload drops the deferral"), Holds.IsBucketDeferred(B));

	// The cap still bounds a group whose bucket stays deferred.
	Holds.BeginTransition(FIntVector(7, 0, 0), { FVoxelSeamKey(FIntVector(6, 0, 0), EVoxelSeamType::Face, 0) }, /*now*/ 2.0);
	Holds.OnSeamResult(FVoxelSeamKey(FIntVector(6, 0, 0), EVoxelSeamType::Face, 0));
	Holds.DeferBucket(FIntVector(6, 0, 0), /*due*/ 10.0);
	FVoxelSeamTransitionHolds::FRelease Capped;
	Holds.CollectReleases(/*now*/ 3.0, /*hold*/ 1.0, Capped);
	TestEqual(TEXT("cap releases a group stuck behind a deferred bucket"), Capped.Groups, 1);
	TestEqual(TEXT("counted as a timeout"), Capped.TimedOut, 1);

	FVoxelSeamTransitionHolds::FRelease All;
	Holds.ReleaseAll(All);
	TestEqual(TEXT("ReleaseAll drops deferrals"), Holds.NumDeferredBuckets(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
