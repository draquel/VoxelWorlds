// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Containers/Queue.h"
#include "VoxelBoundedDrain.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Chunk residency (VOXEL_QA_FOLLOWUPS_PLAN.md R23). Pure logic, runs headless.
//
// The game thread drains async results (generation, meshing, seams, collision cooks) with a
// per-tick budget. `while (Queue.Dequeue(R) && Count < Max)` popped the (Max+1)th result and then
// dropped it; the chunk it belonged to stayed in Meshing (or Generating) for the rest of the
// session — never unloaded, never remeshed, and held an async slot. VoxelDrainBounded checks the
// budget first; these tests pin that contract.
// ---------------------------------------------------------------------------

/**
 * 20 queued results, budget 8: exactly 8 are processed (in FIFO order) and the other 12 are still
 * in the queue afterwards, in order. Nothing is lost.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelResidencyBoundedDrainKeepsOverflowTest,
	"VoxelWorlds.Streaming.Residency.BoundedDrainKeepsOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelResidencyBoundedDrainKeepsOverflowTest::RunTest(const FString& Parameters)
{
	constexpr int32 NumQueued = 20;
	constexpr int32 Budget = 8;

	// --- Every item counts against the budget ---
	{
		TQueue<int32, EQueueMode::Mpsc> Queue;
		for (int32 i = 0; i < NumQueued; ++i)
		{
			Queue.Enqueue(i);
		}

		TArray<int32> Processed;
		const int32 Dequeued = VoxelDrainBounded(Queue, Budget, [&Processed](int32& Item)
		{
			Processed.Add(Item);
			return true;
		});

		TestEqual(TEXT("Drain dequeues exactly the budget"), Dequeued, Budget);
		TestEqual(TEXT("Drain processes exactly the budget"), Processed.Num(), Budget);
		for (int32 i = 0; i < Processed.Num(); ++i)
		{
			TestEqual(FString::Printf(TEXT("Processed item %d in FIFO order"), i), Processed[i], i);
		}

		TArray<int32> Left;
		int32 Item = 0;
		while (Queue.Dequeue(Item))
		{
			Left.Add(Item);
		}
		TestEqual(TEXT("Overflow stays queued (12 left)"), Left.Num(), NumQueued - Budget);
		for (int32 i = 0; i < Left.Num(); ++i)
		{
			TestEqual(FString::Printf(TEXT("Left-over item %d intact"), i), Left[i], Budget + i);
		}
		TestEqual(TEXT("Nothing lost"), Processed.Num() + Left.Num(), NumQueued);
	}

	// --- Uncounted discards do not use the budget, and still lose nothing ---
	{
		TQueue<int32, EQueueMode::Mpsc> Queue;
		for (int32 i = 0; i < NumQueued; ++i)
		{
			Queue.Enqueue(i);
		}

		// Odd items are cheap discards (a superseded or stale result): they are consumed but do not
		// count, so the drain keeps going until 8 real results have been processed.
		int32 Counted = 0;
		int32 Discarded = 0;
		const int32 Dequeued = VoxelDrainBounded(Queue, Budget, [&Counted, &Discarded](int32& Item)
		{
			if (Item % 2 == 1)
			{
				++Discarded;
				return false;
			}
			++Counted;
			return true;
		});

		TestEqual(TEXT("Counted results stop at the budget"), Counted, Budget);
		TestEqual(TEXT("Discards were consumed along the way"), Discarded, Budget - 1);
		TestEqual(TEXT("Dequeued = counted + discarded"), Dequeued, Counted + Discarded);

		int32 Left = 0;
		int32 Item = 0;
		int32 FirstLeft = -1;
		while (Queue.Dequeue(Item))
		{
			FirstLeft = (Left == 0) ? Item : FirstLeft;
			++Left;
		}
		TestEqual(TEXT("Rest stays queued"), Left, NumQueued - Dequeued);
		TestEqual(TEXT("The first left-over item is the one after the last processed"), FirstLeft, Dequeued);
	}

	// --- A zero budget drains nothing ---
	{
		TQueue<int32, EQueueMode::Mpsc> Queue;
		Queue.Enqueue(7);
		int32 Calls = 0;
		const int32 Dequeued = VoxelDrainBounded(Queue, 0, [&Calls](int32&) { ++Calls; return true; });
		TestEqual(TEXT("Zero budget dequeues nothing"), Dequeued, 0);
		TestEqual(TEXT("Zero budget never calls Process"), Calls, 0);
		TestFalse(TEXT("Item still queued"), Queue.IsEmpty());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
