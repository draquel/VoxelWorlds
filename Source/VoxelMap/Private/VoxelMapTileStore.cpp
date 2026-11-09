// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapTileStore.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

void FVoxelMapTileStore::SetBudget(int64 InBudgetBytes)
{
	BudgetBytes = FMath::Max<int64>(InBudgetBytes, 0);
	Enforce();
}

void FVoxelMapTileStore::Store(const FVoxelMapTileId& Id, TArray<FColor>&& Pixels, int32 Resolution)
{
	FEntry& Entry = Entries.FindOrAdd(Id);
	Bytes -= ImageBytes(Entry.Tile);
	Entry.Tile.TileCoord = Id.Coord;
	Entry.Tile.Level = Id.Level;
	Entry.Tile.Resolution = Resolution;
	Entry.Tile.PixelData = MoveTemp(Pixels);
	Entry.Tile.bIsReady = true;
	Entry.bStale = false;
	Entry.LastUse = ++UseClock;
	Bytes += ImageBytes(Entry.Tile);
	Enforce();
}

const FVoxelMapTile* FVoxelMapTileStore::Find(const FVoxelMapTileId& Id, bool bTouch) const
{
	const FEntry* Entry = Entries.Find(Id);
	if (!Entry)
	{
		return nullptr;
	}
	if (bTouch)
	{
		Entry->LastUse = ++UseClock;
	}
	return &Entry->Tile;
}

bool FVoxelMapTileStore::IsStale(const FVoxelMapTileId& Id) const
{
	const FEntry* Entry = Entries.Find(Id);
	return Entry && Entry->bStale;
}

bool FVoxelMapTileStore::MarkStale(const FVoxelMapTileId& Id)
{
	FEntry* Entry = Entries.Find(Id);
	if (!Entry)
	{
		return false;
	}
	Entry->bStale = true;
	return true;
}

int32 FVoxelMapTileStore::NumAtLevel(int32 Level) const
{
	int32 Count = 0;
	for (const TPair<FVoxelMapTileId, FEntry>& Pair : Entries)
	{
		Count += Pair.Key.Level == Level ? 1 : 0;
	}
	return Count;
}

void FVoxelMapTileStore::Remove(const FVoxelMapTileId& Id)
{
	if (const FEntry* Entry = Entries.Find(Id))
	{
		Bytes -= ImageBytes(Entry->Tile);
		Entries.Remove(Id);
	}
}

void FVoxelMapTileStore::Reset()
{
	Entries.Empty();
	UseClock = 0;
	Bytes = 0;
	Evictions = 0;
}

int32 FVoxelMapTileStore::Enforce()
{
	if (BudgetBytes <= 0 || Bytes <= BudgetBytes)
	{
		return 0;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(VoxelMap_StoreEnforce);
	// The least recently used images go, at every level alike: the views read what they show at each fill, so what they
	// show (and the coarser tiles they fall back on) stays.
	TArray<TPair<uint64, FVoxelMapTileId>> Order; // (last use, id)
	Order.Reserve(Entries.Num());
	for (const TPair<FVoxelMapTileId, FEntry>& Pair : Entries)
	{
		Order.Emplace(Pair.Value.LastUse, Pair.Key);
	}
	Order.Sort([](const TPair<uint64, FVoxelMapTileId>& A, const TPair<uint64, FVoxelMapTileId>& B) { return A.Key < B.Key; });
	const int64 Target = BudgetBytes * 9 / 10;
	int32 Dropped = 0;
	for (const TPair<uint64, FVoxelMapTileId>& Item : Order)
	{
		if (Bytes <= Target)
		{
			break;
		}
		Bytes -= ImageBytes(Entries[Item.Value].Tile);
		Entries.Remove(Item.Value);
		++Evictions;
		++Dropped;
	}
	return Dropped;
}
