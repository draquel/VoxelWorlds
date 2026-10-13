// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapTileQueue.h"

void FVoxelMapTileQueue::Request(uint64 Key, bool bInvalidate, bool bPriority)
{
	const bool bDrawing = Drawing.Contains(Key);
	if (bInvalidate)
	{
		++Generations.FindOrAdd(Key);
	}
	else if (bDrawing)
	{
		// The running drawing already shows the tile as it is now.
		return;
	}
	Waiting.Add(Key);
	if (bPriority)
	{
		PriorityWaiting.Add(Key);
	}
}

bool FVoxelMapTileQueue::StartNext(uint64& OutKey, uint32& OutGeneration)
{
	return StartNext(OutKey, OutGeneration, [](uint64) { return true; });
}

bool FVoxelMapTileQueue::StartNext(uint64& OutKey, uint32& OutGeneration, TFunctionRef<bool(uint64 Key)> CanStart)
{
	for (const TSet<uint64>* Set : { &PriorityWaiting, &Waiting })
	{
		bool bFound = false;
		for (const uint64 Key : *Set)
		{
			// An invalidated tile waits while its stale drawing finishes; it starts again after Complete.
			if (!Drawing.Contains(Key) && CanStart(Key))
			{
				OutKey = Key;
				bFound = true;
				break;
			}
		}
		if (bFound)
		{
			return Start(OutKey, OutGeneration);
		}
	}
	return false;
}

bool FVoxelMapTileQueue::Start(uint64 Key, uint32& OutGeneration)
{
	if (!Waiting.Contains(Key) || Drawing.Contains(Key))
	{
		return false;
	}
	Waiting.Remove(Key);
	PriorityWaiting.Remove(Key);
	OutGeneration = Generations.FindRef(Key);
	Drawing.Add(Key, OutGeneration);
	return true;
}

bool FVoxelMapTileQueue::Complete(uint64 Key, uint32 Generation)
{
	Drawing.Remove(Key);
	if (Generation != Generations.FindRef(Key))
	{
		// Invalidated while drawing: the request that bumped the generation already queued the tile again.
		Waiting.Add(Key);
		return false;
	}
	return true;
}

void FVoxelMapTileQueue::Cancel(uint64 Key)
{
	Drawing.Remove(Key);
	Waiting.Remove(Key);
	PriorityWaiting.Remove(Key);
}

void FVoxelMapTileQueue::Reset()
{
	Waiting.Empty();
	PriorityWaiting.Empty();
	Drawing.Empty();
	Generations.Empty();
}
