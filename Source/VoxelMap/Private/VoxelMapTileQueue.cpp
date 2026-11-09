// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapTileQueue.h"

void FVoxelMapTileQueue::Release(int32 Requester, const FVoxelMapTileId& Id)
{
	if (uint8* Bits = Waiting.Find(Id))
	{
		*Bits &= ~Bit(Requester);
		if (*Bits == 0)
		{
			Waiting.Remove(Id);
		}
	}
}

void FVoxelMapTileQueue::SetRequests(EVoxelMapRequester Requester, TConstArrayView<FVoxelMapTileId> NearestFirst)
{
	const int32 R = static_cast<int32>(Requester);
	FRequestList& List = Lists[R];
	// The previous request: what StartNext has not taken yet stops waiting for this requester (a view moved on).
	for (int32 Index = List.Cursor; Index < List.Ids.Num(); ++Index)
	{
		Release(R, List.Ids[Index]);
	}
	List.Ids.Reset();
	List.Cursor = 0;
	for (const FVoxelMapTileId& Id : NearestFirst)
	{
		if (IsDrawingCurrent(Id))
		{
			// The running drawing already shows the tile as it is now.
			continue;
		}
		uint8& Bits = Waiting.FindOrAdd(Id);
		if ((Bits & Bit(R)) == 0)
		{
			Bits |= Bit(R);
			List.Ids.Add(Id);
		}
	}
}

void FVoxelMapTileQueue::Add(EVoxelMapRequester Requester, const FVoxelMapTileId& Id)
{
	if (IsDrawingCurrent(Id))
	{
		return;
	}
	const int32 R = static_cast<int32>(Requester);
	uint8& Bits = Waiting.FindOrAdd(Id);
	if ((Bits & Bit(R)) != 0)
	{
		return;
	}
	Bits |= Bit(R);
	FRequestList& List = Lists[R];
	List.Ids.Add(Id);
	// An appending requester's list only grows: drop what StartNext has passed once that is most of it.
	if (List.Cursor > 256 && List.Cursor * 2 > List.Ids.Num())
	{
		List.Ids.RemoveAt(0, List.Cursor, EAllowShrinking::No);
		List.Cursor = 0;
	}
}

void FVoxelMapTileQueue::Invalidate(const FVoxelMapTileId& Id)
{
	++Generations.FindOrAdd(Id);
}

bool FVoxelMapTileQueue::StartNext(FVoxelMapTileId& OutId, uint32& OutGeneration)
{
	return StartNext(OutId, OutGeneration, [](const FVoxelMapTileId&) { return true; });
}

bool FVoxelMapTileQueue::StartNext(FVoxelMapTileId& OutId, uint32& OutGeneration, TFunctionRef<bool(const FVoxelMapTileId& Id)> CanStart)
{
	for (int32 R = 0; R < NumRequesters; ++R)
	{
		FRequestList& List = Lists[R];
		auto WantedHere = [this, R](const FVoxelMapTileId& Id)
		{
			const uint8* Bits = Waiting.Find(Id);
			return Bits && (*Bits & Bit(R)) != 0;
		};
		// Past the tiles this requester no longer waits for (started, or dropped).
		while (List.Cursor < List.Ids.Num() && !WantedHere(List.Ids[List.Cursor]))
		{
			++List.Cursor;
		}
		for (int32 Index = List.Cursor; Index < List.Ids.Num(); ++Index)
		{
			const FVoxelMapTileId& Id = List.Ids[Index];
			// An invalidated tile waits while its stale drawing finishes; it starts again after Complete.
			if (!WantedHere(Id) || Drawing.Contains(Id) || !CanStart(Id))
			{
				continue;
			}
			OutId = Id;
			Waiting.Remove(Id);
			OutGeneration = Generations.FindRef(Id);
			Drawing.Add(Id, OutGeneration);
			return true;
		}
	}
	return false;
}

bool FVoxelMapTileQueue::Complete(const FVoxelMapTileId& Id, uint32 Generation)
{
	Drawing.Remove(Id);
	return Generation == Generations.FindRef(Id);
}

void FVoxelMapTileQueue::Cancel(const FVoxelMapTileId& Id)
{
	Drawing.Remove(Id);
	Waiting.Remove(Id);
}

bool FVoxelMapTileQueue::IsDrawingCurrent(const FVoxelMapTileId& Id) const
{
	const uint32* Started = Drawing.Find(Id);
	return Started && *Started == Generations.FindRef(Id);
}

int32 FVoxelMapTileQueue::NumWaitingFor(EVoxelMapRequester Requester) const
{
	const uint8 Mask = Bit(static_cast<int32>(Requester));
	int32 Count = 0;
	for (const TPair<FVoxelMapTileId, uint8>& Pair : Waiting)
	{
		Count += (Pair.Value & Mask) != 0 ? 1 : 0;
	}
	return Count;
}

void FVoxelMapTileQueue::Reset()
{
	for (FRequestList& List : Lists)
	{
		List.Ids.Empty();
		List.Cursor = 0;
	}
	Waiting.Empty();
	Drawing.Empty();
	Generations.Empty();
}
