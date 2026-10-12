// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"

/**
 * Bounded drain of an async-result queue (VoxelStreaming).
 *
 * The game thread drains worker results (chunk generation, chunk meshing, seam meshing, collision
 * cooks) with a per-tick budget. The budget must be tested BEFORE the dequeue:
 * `while (Queue.Dequeue(Item) && Count < Max)` pops the (Max+1)th item, then fails the budget test
 * and drops it. The chunk that result belonged to waits forever for it — the R23 "chunk stranded in
 * Meshing" defect (VOXEL_QA_FOLLOWUPS_PLAN.md R23b). Every drain goes through this helper so that
 * operand order cannot come back.
 *
 * Thread safety: call from the queue's single consumer thread (the game thread for every caller).
 *
 * @param Queue      Result queue (any TQueue mode).
 * @param MaxCounted Budget: stop once Process has returned true this many times. <= 0 drains nothing.
 * @param Process    Called with each dequeued item by mutable reference (it may move from it).
 *                   Returns true when the item counts against the budget; cheap discards (stale or
 *                   superseded results) may return false so they never starve real work.
 * @return Number of items dequeued, counted or not. Items beyond the budget stay in the queue.
 */
template <typename ItemType, EQueueMode Mode, typename ProcessFnType>
int32 VoxelDrainBounded(TQueue<ItemType, Mode>& Queue, int32 MaxCounted, ProcessFnType&& Process)
{
	int32 Counted = 0;
	int32 Dequeued = 0;
	ItemType Item;
	while (Counted < MaxCounted && Queue.Dequeue(Item))
	{
		++Dequeued;
		if (Process(Item))
		{
			++Counted;
		}
	}
	return Dequeued;
}
