// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMapTileQueue.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// QA refinement R22: the map's tile draw queue. Edits redraw tiles that are already cached and may be drawing, which
// the old queue mishandled: a cached tile queued while all four slots were busy stayed pending for good, and a
// re-queue during a drawing was lost when that drawing completed. The policy lives in FVoxelMapTileQueue so it runs
// without a world.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapTileQueueRequeueTest,
	"VoxelWorlds.Map.Tiles.RequeueWhileCachedRedraws",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapTileQueueRequeueTest::RunTest(const FString& Parameters)
{
	FVoxelMapTileQueue Queue;
	uint64 Key = 0;
	uint32 Generation = 0;
	uint32 Started[5] = {};

	// Four tiles drawing: every slot is busy.
	for (uint64 Tile = 1; Tile <= 4; ++Tile)
	{
		Queue.Request(Tile, false);
		TestTrue(TEXT("a waiting tile starts"), Queue.StartNext(Key, Generation));
		TestEqual(TEXT("in request order"), Key, Tile);
		Started[Tile] = Generation;
	}
	TestEqual(TEXT("four drawing"), Queue.NumDrawing(), 4);

	// Tile 5 is cached and gets edited: the redraw request waits for a slot.
	Queue.Request(5, /*bInvalidate*/ true, /*bPriority*/ true);
	TestTrue(TEXT("the cached tile waits for a slot"), Queue.IsWaiting(5));

	// A slot frees: the cached tile is the next to start (the old drain skipped cached tiles forever).
	TestTrue(TEXT("tile 1 completes current"), Queue.Complete(1, Started[1]));
	TestTrue(TEXT("something starts after a completion"), Queue.StartNext(Key, Generation));
	TestEqual(TEXT("the re-queued cached tile starts"), Key, static_cast<uint64>(5));
	TestTrue(TEXT("its redraw completes current"), Queue.Complete(5, Generation));
	TestFalse(TEXT("and leaves the queue"), Queue.IsQueued(5));

	// A non-invalidating request for a tile that is drawing is satisfied by that drawing.
	Queue.Request(2, false);
	TestFalse(TEXT("no second drawing is queued for a drawing tile"), Queue.IsWaiting(2));
	TestTrue(TEXT("its drawing completes current"), Queue.Complete(2, Started[2]));
	TestFalse(TEXT("nothing left for tile 2"), Queue.IsQueued(2));

	// An invalidating request for a drawing tile survives that drawing's completion (it used to be lost).
	Queue.Request(3, true);
	TestFalse(TEXT("the stale drawing of tile 3 is discarded"), Queue.Complete(3, Started[3]));
	TestTrue(TEXT("tile 3 waits again"), Queue.IsWaiting(3));
	TestTrue(TEXT("and starts again"), Queue.StartNext(Key, Generation) && Key == 3);
	TestTrue(TEXT("its redraw completes current"), Queue.Complete(3, Generation));

	// Priority requests start before the backlog.
	Queue.Request(10, false);
	Queue.Request(11, false);
	Queue.Request(12, false, /*bPriority*/ true);
	TestTrue(TEXT("a priority tile starts first"), Queue.StartNext(Key, Generation) && Key == 12);

	// A tile the caller cannot start now (an overlay rebuild beyond the per-frame gather budget) keeps waiting.
	TestTrue(TEXT("the predicate skips tile 10"), Queue.StartNext(Key, Generation, [](uint64 K) { return K != 10; }) && Key == 11);
	TestTrue(TEXT("the skipped tile still waits"), Queue.IsWaiting(10));
	TestFalse(TEXT("nothing else can start under the predicate"), Queue.StartNext(Key, Generation, [](uint64 K) { return K != 10; }));
	TestTrue(TEXT("it starts once allowed"), Queue.StartNext(Key, Generation) && Key == 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapTileQueueGenerationTest,
	"VoxelWorlds.Map.Tiles.PerTileGenerationDiscardsStale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapTileQueueGenerationTest::RunTest(const FString& Parameters)
{
	FVoxelMapTileQueue Queue;
	constexpr uint64 TileA = 0xA;
	constexpr uint64 TileB = 0xB;
	uint64 Key = 0;
	uint32 GenA = 0;
	uint32 GenB = 0;

	Queue.Request(TileA, false);
	Queue.Request(TileB, false);
	TestTrue(TEXT("A starts"), Queue.Start(TileA, GenA));
	TestTrue(TEXT("B starts"), Queue.Start(TileB, GenB));
	TestFalse(TEXT("a drawing tile cannot start twice"), Queue.Start(TileA, GenA));

	// An edit lands on A while both draw.
	Queue.Request(TileA, /*bInvalidate*/ true);
	TestTrue(TEXT("A's generation moved on"), Queue.GetGeneration(TileA) != GenA);
	TestEqual(TEXT("B's generation is untouched"), Queue.GetGeneration(TileB), GenB);
	uint32 Unused = 0;
	TestFalse(TEXT("A does not restart while its stale drawing runs"), Queue.StartNext(Key, Unused));

	TestFalse(TEXT("A's stale result is discarded"), Queue.Complete(TileA, GenA));
	TestTrue(TEXT("B's result is kept: invalidation is per tile"), Queue.Complete(TileB, GenB));
	TestFalse(TEXT("B is done"), Queue.IsQueued(TileB));

	uint32 GenA2 = 0;
	TestTrue(TEXT("A starts again"), Queue.StartNext(Key, GenA2) && Key == TileA);
	TestEqual(TEXT("under its new generation"), GenA2, Queue.GetGeneration(TileA));
	TestTrue(TEXT("A's fresh result is kept"), Queue.Complete(TileA, GenA2));
	TestEqual(TEXT("nothing waits"), Queue.NumWaiting(), 0);
	TestEqual(TEXT("nothing draws"), Queue.NumDrawing(), 0);

	// Cancel forgets a tile entirely.
	Queue.Request(TileB, false);
	TestTrue(TEXT("B starts"), Queue.Start(TileB, GenB));
	Queue.Cancel(TileB);
	TestFalse(TEXT("a cancelled tile is gone"), Queue.IsQueued(TileB));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
