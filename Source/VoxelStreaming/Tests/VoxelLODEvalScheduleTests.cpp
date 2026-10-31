// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelLODEvalSchedule.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// When the chunk manager re-evaluates LOD levels (WORLD_MODES_REVIEW_PLAN.md §13.10, KI12). Pure logic,
// runs headless.
//
// The old rule evaluated on a viewer-chunk change or while a sweep flag was set; the flag was set when
// every queue was empty and cleared only by an evaluation that ran with every queue empty and queued
// nothing. Once streaming kept the queues busy the flag froze in whatever state it had, so identical
// island benches landed in two regimes (12,500 or 1,341 LOD re-meshes) depending on whether an LOD
// re-mesh was in flight when the queues last emptied. These tests pin the replacement.
// ---------------------------------------------------------------------------

/** Candidates left over by the per-frame cap are picked up next tick with no movement and a busy pipeline. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelLODEvalScheduleCapLeftoversTest,
	"VoxelWorlds.Streaming.LODEvalSchedule.CapLeftoversEvaluateNextTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelLODEvalScheduleCapLeftoversTest::RunTest(const FString& Parameters)
{
	FVoxelLODEvalSchedule Schedule;
	const FVector Position(1000.0f, 2000.0f, 300.0f);

	TestTrue(TEXT("First tick evaluates"), Schedule.ShouldEvaluate(false, Position, false));
	Schedule.OnEvaluated(Position, /*NumQueued*/ 8, /*bCapReached*/ true);
	TestTrue(TEXT("Cap reached is recorded"), Schedule.IsCapReached());

	// Busy pipeline, viewer still: keeps evaluating while each batch fills the cap.
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		TestTrue(FString::Printf(TEXT("Left-overs evaluate on tick %d"), Tick), Schedule.ShouldEvaluate(false, Position, false));
		Schedule.OnEvaluated(Position, 8, true);
	}

	// The last batch fits under the cap: nothing is left, so the schedule goes quiet.
	TestTrue(TEXT("Final batch evaluates"), Schedule.ShouldEvaluate(false, Position, false));
	Schedule.OnEvaluated(Position, 3, false);
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		TestFalse(FString::Printf(TEXT("Quiet on tick %d after the backlog cleared"), Tick), Schedule.ShouldEvaluate(false, Position, false));
	}
	return true;
}

/** Movement, chunk changes, the idle edge and Reset each trigger an evaluation; a settled, still viewer does not. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelLODEvalScheduleTriggersTest,
	"VoxelWorlds.Streaming.LODEvalSchedule.Triggers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelLODEvalScheduleTriggersTest::RunTest(const FString& Parameters)
{
	FVoxelLODEvalSchedule Schedule;
	FVector Position(0.0f, 0.0f, 0.0f);

	TestTrue(TEXT("Nothing evaluated yet: evaluate"), Schedule.ShouldEvaluate(false, Position, false));
	Schedule.OnEvaluated(Position, 0, false);

	// --- Movement accumulates from the last evaluation, not from the last tick ---
	Position.X += 60.0f;
	TestFalse(TEXT("60 uu: below the threshold"), Schedule.ShouldEvaluate(false, Position, false));
	Position.X += 60.0f;
	TestTrue(TEXT("120 uu since the last evaluation: evaluate"), Schedule.ShouldEvaluate(false, Position, false));
	Schedule.OnEvaluated(Position, 0, false);
	Position.Z += 99.0f;
	TestFalse(TEXT("99 uu vertical: below the threshold"), Schedule.ShouldEvaluate(false, Position, false));

	// --- A chunk change evaluates regardless of distance ---
	TestTrue(TEXT("Chunk change: evaluate"), Schedule.ShouldEvaluate(true, Position, false));
	Schedule.OnEvaluated(Position, 0, false);

	// --- The idle sweep fires on the busy -> idle edge, once ---
	TestTrue(TEXT("Pipeline goes idle: sweep"), Schedule.ShouldEvaluate(false, Position, true));
	Schedule.OnEvaluated(Position, 0, false);
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		TestFalse(FString::Printf(TEXT("Still idle, viewer still, tick %d: nothing to do"), Tick), Schedule.ShouldEvaluate(false, Position, true));
	}
	TestFalse(TEXT("Pipeline busy again, viewer still"), Schedule.ShouldEvaluate(false, Position, false));
	TestTrue(TEXT("Idle again: one more sweep"), Schedule.ShouldEvaluate(false, Position, true));
	Schedule.OnEvaluated(Position, 0, false);

	// --- A sweep that queued work runs again next tick, then ends ---
	TestFalse(TEXT("Settled"), Schedule.ShouldEvaluate(false, Position, true));
	TestFalse(TEXT("Busy again, viewer still"), Schedule.ShouldEvaluate(false, Position, false));
	TestTrue(TEXT("Idle edge"), Schedule.ShouldEvaluate(false, Position, true));
	Schedule.OnEvaluated(Position, 4, false);
	TestTrue(TEXT("Sweep that queued work evaluates again (pipeline now busy)"), Schedule.ShouldEvaluate(false, Position, false));
	Schedule.OnEvaluated(Position, 0, false);
	TestFalse(TEXT("Sweep over"), Schedule.ShouldEvaluate(false, Position, false));

	// --- Reset forgets everything ---
	Schedule.Reset();
	TestTrue(TEXT("After Reset: evaluate"), Schedule.ShouldEvaluate(false, Position, false));
	TestFalse(TEXT("After Reset: no stale cap flag"), Schedule.IsCapReached());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
