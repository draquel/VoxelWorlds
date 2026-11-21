// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMapTileQueue.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// QA refinement R22: the map's tile draw queue. Edits redraw tiles that are already cached and may be drawing, which
// the old queue mishandled: a cached tile queued while all four slots were busy stayed pending for good, and a
// re-queue during a drawing was lost when that drawing completed. The policy lives in FVoxelMapTileQueue so it runs
// without a world. MAP_PYRAMID_PLAN.md P1: requests come from the views, each replacing its last, served by requester
// (the minimap, the world map, the edits) and in each requester's order (ViewRequestsReplace).
// ---------------------------------------------------------------------------

namespace VoxelMapTileQueueTest
{
	FVoxelMapTileId Tile(int32 X, int32 Level = 0)
	{
		return FVoxelMapTileId(FIntPoint(X, 0), Level);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapTileQueueRequeueTest,
	"VoxelWorlds.Map.Tiles.RequeueWhileCachedRedraws",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapTileQueueRequeueTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapTileQueueTest;
	FVoxelMapTileQueue Queue;
	FVoxelMapTileId Id;
	uint32 Generation = 0;
	uint32 Started[5] = {};

	// Four tiles drawing: every slot is busy.
	Queue.SetRequests(EVoxelMapRequester::WorldMap, { Tile(1), Tile(2), Tile(3), Tile(4) });
	for (int32 X = 1; X <= 4; ++X)
	{
		TestTrue(TEXT("a waiting tile starts"), Queue.StartNext(Id, Generation));
		TestTrue(TEXT("in request order"), Id == Tile(X));
		Started[X] = Generation;
	}
	TestEqual(TEXT("four drawing"), Queue.NumDrawing(), 4);

	// Tile 5 is cached and gets edited: the redraw request waits for a slot.
	Queue.Invalidate(Tile(5));
	Queue.Add(EVoxelMapRequester::Edits, Tile(5));
	TestTrue(TEXT("the cached tile waits for a slot"), Queue.IsWaiting(Tile(5)));

	// A slot frees: the cached tile is the next to start (the old drain skipped cached tiles forever).
	TestTrue(TEXT("tile 1 completes current"), Queue.Complete(Tile(1), Started[1]));
	TestTrue(TEXT("something starts after a completion"), Queue.StartNext(Id, Generation));
	TestTrue(TEXT("the re-queued cached tile starts"), Id == Tile(5));
	TestTrue(TEXT("its redraw completes current"), Queue.Complete(Tile(5), Generation));
	TestFalse(TEXT("and leaves the queue"), Queue.IsQueued(Tile(5)));

	// A request for a tile drawing under its current generation is satisfied by that drawing.
	Queue.Add(EVoxelMapRequester::Edits, Tile(2));
	TestFalse(TEXT("no second drawing is queued for a drawing tile"), Queue.IsWaiting(Tile(2)));
	TestTrue(TEXT("its drawing completes current"), Queue.Complete(Tile(2), Started[2]));
	TestFalse(TEXT("nothing left for tile 2"), Queue.IsQueued(Tile(2)));

	// An invalidation while drawing, asked for again, survives that drawing's completion (it used to be lost).
	Queue.Invalidate(Tile(3));
	Queue.Add(EVoxelMapRequester::Edits, Tile(3));
	TestFalse(TEXT("the stale drawing of tile 3 is discarded"), Queue.Complete(Tile(3), Started[3]));
	TestTrue(TEXT("tile 3 waits again"), Queue.IsWaiting(Tile(3)));
	TestTrue(TEXT("and starts again"), Queue.StartNext(Id, Generation) && Id == Tile(3));
	TestTrue(TEXT("its redraw completes current"), Queue.Complete(Tile(3), Generation));

	// A tile the caller cannot start now (an overlay rebuild beyond the per-frame gather budget) keeps waiting.
	Queue.Add(EVoxelMapRequester::Edits, Tile(10));
	Queue.Add(EVoxelMapRequester::Edits, Tile(11));
	auto Not10 = [](const FVoxelMapTileId& Candidate) { return Candidate.Coord.X != 10; };
	TestTrue(TEXT("the predicate skips tile 10"), Queue.StartNext(Id, Generation, Not10) && Id == Tile(11));
	TestTrue(TEXT("the skipped tile still waits"), Queue.IsWaiting(Tile(10)));
	TestFalse(TEXT("nothing else can start under the predicate"), Queue.StartNext(Id, Generation, Not10));
	TestTrue(TEXT("it starts once allowed"), Queue.StartNext(Id, Generation) && Id == Tile(10));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapTileQueueGenerationTest,
	"VoxelWorlds.Map.Tiles.PerTileGenerationDiscardsStale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapTileQueueGenerationTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapTileQueueTest;
	FVoxelMapTileQueue Queue;
	const FVoxelMapTileId TileA = Tile(0xA);
	const FVoxelMapTileId TileB = Tile(0xB, 3);
	FVoxelMapTileId Id;
	uint32 GenA = 0;
	uint32 GenB = 0;

	Queue.SetRequests(EVoxelMapRequester::WorldMap, { TileA, TileB });
	TestTrue(TEXT("A starts"), Queue.StartNext(Id, GenA) && Id == TileA);
	TestTrue(TEXT("B starts"), Queue.StartNext(Id, GenB) && Id == TileB);
	TestTrue(TEXT("A draws current"), Queue.IsDrawingCurrent(TileA));

	// An edit lands on A while both draw.
	Queue.Invalidate(TileA);
	TestTrue(TEXT("A's generation moved on"), Queue.GetGeneration(TileA) != GenA);
	TestEqual(TEXT("B's generation is untouched"), Queue.GetGeneration(TileB), GenB);
	TestFalse(TEXT("A's drawing is no longer current"), Queue.IsDrawingCurrent(TileA));
	Queue.Add(EVoxelMapRequester::Edits, TileA);
	uint32 Unused = 0;
	TestFalse(TEXT("A does not restart while its stale drawing runs"), Queue.StartNext(Id, Unused));

	TestFalse(TEXT("A's stale result is discarded"), Queue.Complete(TileA, GenA));
	TestTrue(TEXT("B's result is kept: invalidation is per tile"), Queue.Complete(TileB, GenB));
	TestFalse(TEXT("B is done"), Queue.IsQueued(TileB));

	uint32 GenA2 = 0;
	TestTrue(TEXT("A starts again"), Queue.StartNext(Id, GenA2) && Id == TileA);
	TestEqual(TEXT("under its new generation"), GenA2, Queue.GetGeneration(TileA));
	TestTrue(TEXT("A's fresh result is kept"), Queue.Complete(TileA, GenA2));
	TestEqual(TEXT("nothing waits"), Queue.NumWaiting(), 0);
	TestEqual(TEXT("nothing draws"), Queue.NumDrawing(), 0);

	// An invalidation nobody asks for again leaves nothing waiting once the stale drawing ends.
	Queue.SetRequests(EVoxelMapRequester::WorldMap, { TileB });
	TestTrue(TEXT("B starts"), Queue.StartNext(Id, GenB) && Id == TileB);
	Queue.Invalidate(TileB);
	TestFalse(TEXT("B's stale result is discarded"), Queue.Complete(TileB, GenB));
	TestFalse(TEXT("... and B is not drawn again unasked"), Queue.IsQueued(TileB));

	// Cancel forgets a tile entirely.
	Queue.SetRequests(EVoxelMapRequester::WorldMap, { TileB });
	TestTrue(TEXT("B starts"), Queue.StartNext(Id, GenB));
	Queue.Cancel(TileB);
	TestFalse(TEXT("a cancelled tile is gone"), Queue.IsQueued(TileB));
	return true;
}

/**
 * MAP_PYRAMID_PLAN.md P1: a view's request replaces its last (what left the view stops waiting, unless another requester
 * wants it), requesters are served minimap, world map, edits, each in its own order (nearest the view's centre first),
 * and the queue holds no more than the views' tiles however many views came and went.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapTileQueueViewRequestsTest,
	"VoxelWorlds.Map.Tiles.ViewRequestsReplace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapTileQueueViewRequestsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapTileQueueTest;
	FVoxelMapTileQueue Queue;
	FVoxelMapTileId Id;
	uint32 Generation = 0;

	// The world map at one zoom, then another: the first view's tiles stop waiting, the second's wait in its order.
	TArray<FVoxelMapTileId> First, Second;
	for (int32 X = 0; X < 300; ++X)
	{
		First.Add(Tile(X, 2));
		Second.Add(Tile(299 - X, 3));
	}
	Queue.SetRequests(EVoxelMapRequester::WorldMap, First);
	TestEqual(TEXT("the first view's tiles wait"), Queue.NumWaiting(), 300);
	TestTrue(TEXT("its nearest starts first"), Queue.StartNext(Id, Generation) && Id == Tile(0, 2));
	Queue.SetRequests(EVoxelMapRequester::WorldMap, Second);
	TestEqual(TEXT("the new view replaces the old: only its tiles wait"), Queue.NumWaiting(), 300);
	TestFalse(TEXT("a tile of the old view no longer waits"), Queue.IsWaiting(Tile(5, 2)));
	TestTrue(TEXT("the old view's drawing in flight runs on"), Queue.IsDrawing(Tile(0, 2)));
	TestTrue(TEXT("the new view's nearest starts next"), Queue.StartNext(Id, Generation) && Id == Tile(299, 3));

	// Many views later the queue still holds one view's tiles.
	for (int32 Pan = 0; Pan < 50; ++Pan)
	{
		TArray<FVoxelMapTileId> View;
		for (int32 X = 0; X < 300; ++X)
		{
			View.Add(FVoxelMapTileId(FIntPoint(Pan * 7 + X, 1), 2));
		}
		Queue.SetRequests(EVoxelMapRequester::WorldMap, View);
		TestTrue(TEXT("the queue never past a view's tiles"), Queue.NumWaiting() <= 300);
	}

	// The minimap before the world map, the edits after both; a tile two requesters want stays while one does.
	Queue.SetRequests(EVoxelMapRequester::WorldMap, { Tile(1000, 2), Tile(1001, 2) });
	Queue.Add(EVoxelMapRequester::Edits, Tile(2000));
	Queue.SetRequests(EVoxelMapRequester::Minimap, { Tile(3000, 1), Tile(1001, 2) });
	TestEqual(TEXT("the minimap's waits for the minimap"), Queue.NumWaitingFor(EVoxelMapRequester::Minimap), 2);
	TestTrue(TEXT("the minimap's first"), Queue.StartNext(Id, Generation) && Id == Tile(3000, 1));
	Queue.SetRequests(EVoxelMapRequester::Minimap, {});
	TestTrue(TEXT("a tile the world map still wants keeps waiting when the minimap lets it go"), Queue.IsWaiting(Tile(1001, 2)));
	TestTrue(TEXT("then the world map's, in its order"), Queue.StartNext(Id, Generation) && Id == Tile(1000, 2));
	TestTrue(TEXT("..."), Queue.StartNext(Id, Generation) && Id == Tile(1001, 2));
	TestTrue(TEXT("the edits last"), Queue.StartNext(Id, Generation) && Id == Tile(2000));
	Queue.SetRequests(EVoxelMapRequester::WorldMap, {});
	TestEqual(TEXT("a closed view leaves nothing waiting"), Queue.NumWaiting(), 0);

	// A view asking for a tile already drawing under its generation does not queue it twice.
	Queue.SetRequests(EVoxelMapRequester::WorldMap, { Tile(1000, 2) });
	TestTrue(TEXT("a drawing tile is not asked for again"), !Queue.IsWaiting(Tile(1000, 2)) && Queue.IsDrawing(Tile(1000, 2)));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
