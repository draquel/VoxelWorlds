// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCrossfadeScheduler.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Mesh-swap crossfade timing (VOXEL_QA_FOLLOWUPS_PLAN.md R24). Pure logic, runs headless.
// The complementary dither only hides a swap when the fades that must cover each other run on
// the same alpha, so these tests pin the clock: starts, holds, releases, restarts and the cap.
// ---------------------------------------------------------------------------

namespace VoxelCrossfadeTestUtils
{
	static float AlphaOf(const FVoxelCrossfadeScheduler::FAdvanceResult& R, const FIntVector4& Key, float Missing = -1.0f)
	{
		for (const TPair<FIntVector4, float>& Pair : R.Alphas)
		{
			if (Pair.Key == Key)
			{
				return Pair.Value;
			}
		}
		return Missing;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCrossfadeSwapStartsFadeTest,
	"VoxelWorlds.Rendering.Crossfade.SwapStartsFade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCrossfadeSwapStartsFadeTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossfadeTestUtils;
	FVoxelCrossfadeScheduler S;
	const FIntVector4 Chunk = FVoxelCrossfadeScheduler::ChunkKey(FIntVector(3, -2, 1));
	const FIntVector4 Seam = FVoxelCrossfadeScheduler::SeamKey(FIntVector(3, -2, 1), 0);
	TestTrue(TEXT("chunk and seam keys differ"), Chunk != Seam);
	TestTrue(TEXT("seam key recognised"), FVoxelCrossfadeScheduler::IsSeamKey(Seam) && !FVoxelCrossfadeScheduler::IsSeamKey(Chunk));

	TestTrue(TEXT("fade starts"), S.Start(Chunk, /*now*/ 10.0, /*hold*/ false));
	FVoxelCrossfadeScheduler::FAdvanceResult R;
	S.Advance(10.0, 0.4f, 0.0, R);
	TestEqual(TEXT("alpha 0 at the start"), AlphaOf(R, Chunk), 0.0f, 1e-5f);
	S.Advance(10.1, 0.4f, 0.0, R);
	TestEqual(TEXT("alpha follows elapsed / duration"), AlphaOf(R, Chunk), 0.25f, 1e-4f);
	S.Advance(10.4, 0.4f, 0.0, R);
	TestTrue(TEXT("completes at the duration"), R.Completed.Contains(Chunk));
	TestFalse(TEXT("removed once complete"), S.Contains(Chunk));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCrossfadeHoldFreezesAlphaTest,
	"VoxelWorlds.Rendering.Crossfade.HoldFreezesAlphaUntilRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCrossfadeHoldFreezesAlphaTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossfadeTestUtils;
	FVoxelCrossfadeScheduler S;
	const FIntVector4 Chunk = FVoxelCrossfadeScheduler::ChunkKey(FIntVector(0, 0, 0));
	const FIntVector4 SeamA = FVoxelCrossfadeScheduler::SeamKey(FIntVector(0, 0, 0), 0);
	const FIntVector4 SeamB = FVoxelCrossfadeScheduler::SeamKey(FIntVector(-1, 0, 0), 0);

	// The interior is held first; its seams land (also held) a few frames later.
	S.Start(Chunk, /*now*/ 0.0, /*hold*/ true);
	S.Start(SeamA, /*now*/ 0.03, /*hold*/ true);
	S.Start(SeamB, /*now*/ 0.05, /*hold*/ true);

	FVoxelCrossfadeScheduler::FAdvanceResult R;
	S.Advance(/*now*/ 0.5, 0.4f, /*failsafe*/ 5.0, R);
	TestEqual(TEXT("held interior stays at alpha 0"), AlphaOf(R, Chunk), 0.0f);
	TestEqual(TEXT("held seam stays at alpha 0"), AlphaOf(R, SeamA), 0.0f);
	TestTrue(TEXT("held fades never complete"), R.Completed.Num() == 0);

	// Released together: one clock for all three, whatever order they were held in.
	TestTrue(TEXT("release interior"), S.Release(Chunk, /*now*/ 0.6));
	TestTrue(TEXT("release seam A"), S.Release(SeamA, 0.6));
	TestTrue(TEXT("release seam B"), S.Release(SeamB, 0.6));
	TestFalse(TEXT("a second release is a no-op"), S.Release(SeamB, 0.6));

	S.Advance(/*now*/ 0.8, 0.4f, 5.0, R);
	const float A0 = AlphaOf(R, Chunk);
	TestEqual(TEXT("fading from the release"), A0, 0.5f, 1e-4f);
	TestEqual(TEXT("seam A on the interior's alpha"), AlphaOf(R, SeamA), A0);
	TestEqual(TEXT("seam B on the interior's alpha"), AlphaOf(R, SeamB), A0);

	S.Advance(/*now*/ 1.0, 0.4f, 5.0, R);
	TestEqual(TEXT("all three complete on the same tick"), R.Completed.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCrossfadeHoldTimesOutTest,
	"VoxelWorlds.Rendering.Crossfade.HoldTimesOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCrossfadeHoldTimesOutTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossfadeTestUtils;
	FVoxelCrossfadeScheduler S;
	const FIntVector4 Chunk = FVoxelCrossfadeScheduler::ChunkKey(FIntVector(1, 2, 3));
	S.Start(Chunk, /*now*/ 0.0, /*hold*/ true);

	FVoxelCrossfadeScheduler::FAdvanceResult R;
	S.Advance(/*now*/ 0.9, 0.4f, /*failsafe*/ 1.0, R);
	TestEqual(TEXT("held below the failsafe"), AlphaOf(R, Chunk), 0.0f);
	TestEqual(TEXT("no timeout yet"), R.TimedOutHolds.Num(), 0);

	S.Advance(/*now*/ 1.0, 0.4f, 1.0, R);
	TestEqual(TEXT("failsafe releases the hold"), R.TimedOutHolds.Num(), 1);
	TestFalse(TEXT("no longer held"), S.IsHeld(Chunk));
	TestEqual(TEXT("fades from alpha 0 at the timeout"), AlphaOf(R, Chunk), 0.0f, 1e-5f);

	S.Advance(/*now*/ 1.2, 0.4f, 1.0, R);
	TestEqual(TEXT("then fades normally"), AlphaOf(R, Chunk), 0.5f, 1e-4f);

	// No failsafe (<= 0): a hold lasts until released.
	const FIntVector4 Other = FVoxelCrossfadeScheduler::ChunkKey(FIntVector(9, 9, 9));
	S.Start(Other, 0.0, true);
	S.Advance(/*now*/ 100.0, 0.4f, /*failsafe*/ 0.0, R);
	TestTrue(TEXT("without a failsafe the hold persists"), S.IsHeld(Other));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCrossfadeResubmitRestartsTest,
	"VoxelWorlds.Rendering.Crossfade.ResubmitDuringFadeRestarts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCrossfadeResubmitRestartsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossfadeTestUtils;
	FVoxelCrossfadeScheduler S;
	const FIntVector4 Seam = FVoxelCrossfadeScheduler::SeamKey(FIntVector(4, 4, 0), 0);

	S.Start(Seam, /*now*/ 0.0, /*hold*/ false);
	FVoxelCrossfadeScheduler::FAdvanceResult R;
	S.Advance(/*now*/ 0.2, 0.4f, 0.0, R);
	TestEqual(TEXT("mid-fade"), AlphaOf(R, Seam), 0.5f, 1e-4f);
	// The seam pipeline defers a resubmit by this much (a restart drops the mesh being faded out of).
	TestEqual(TEXT("remaining time of a running fade"), S.GetRemainingSeconds(Seam, 0.2, 0.4f), 0.2f, 1e-4f);
	TestEqual(TEXT("no fade: nothing to wait for"),
		S.GetRemainingSeconds(FVoxelCrossfadeScheduler::SeamKey(FIntVector(9, 9, 9), 0), 0.2, 0.4f), 0.0f);

	// A new mesh arrives mid-fade: the fade restarts from 0 for the new mesh.
	TestTrue(TEXT("restart accepted"), S.Start(Seam, /*now*/ 0.2, /*hold*/ false));
	TestEqual(TEXT("still one fade"), S.Num(), 1);
	S.Advance(/*now*/ 0.2, 0.4f, 0.0, R);
	TestEqual(TEXT("restarted at alpha 0"), AlphaOf(R, Seam), 0.0f, 1e-5f);

	// A held resubmit (part of an LOD transition) holds the restarted fade.
	S.Start(Seam, /*now*/ 0.3, /*hold*/ true);
	S.Advance(/*now*/ 0.6, 0.4f, 5.0, R);
	TestTrue(TEXT("resubmitted held"), S.IsHeld(Seam));
	TestEqual(TEXT("held at 0"), AlphaOf(R, Seam), 0.0f);
	TestEqual(TEXT("a held fade shows only its previous mesh: nothing to wait for"), S.GetRemainingSeconds(Seam, 0.6, 0.4f), 0.0f);

	// An unheld resubmit ends a hold and fades from now.
	S.Start(Seam, /*now*/ 0.7, /*hold*/ false);
	TestFalse(TEXT("unheld resubmit ends the hold"), S.IsHeld(Seam));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCrossfadeCapFallsBackTest,
	"VoxelWorlds.Rendering.Crossfade.ConcurrentFadeCapFallsBackToSwap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCrossfadeCapFallsBackTest::RunTest(const FString& Parameters)
{
	FVoxelCrossfadeScheduler S;
	S.SetMaxConcurrent(2);
	const FIntVector4 A = FVoxelCrossfadeScheduler::ChunkKey(FIntVector(0, 0, 0));
	const FIntVector4 B = FVoxelCrossfadeScheduler::ChunkKey(FIntVector(1, 0, 0));
	const FIntVector4 C = FVoxelCrossfadeScheduler::ChunkKey(FIntVector(2, 0, 0));

	TestTrue(TEXT("first fade"), S.Start(A, 0.0, false));
	TestTrue(TEXT("second fade"), S.Start(B, 0.0, false));
	TestFalse(TEXT("over the cap: caller swaps instantly"), S.Start(C, 0.0, false));
	TestFalse(TEXT("refused fade not tracked"), S.Contains(C));
	TestTrue(TEXT("restarting a running fade is not a new fade"), S.Start(A, 0.1, false));

	FVoxelCrossfadeScheduler::FAdvanceResult R;
	S.Advance(/*now*/ 1.0, 0.4f, 0.0, R);
	TestEqual(TEXT("both complete"), R.Completed.Num(), 2);
	TestTrue(TEXT("capacity freed"), S.Start(C, 1.0, false));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
