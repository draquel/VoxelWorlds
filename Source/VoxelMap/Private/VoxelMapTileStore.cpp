// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMapTileStore.h"

TArray<FColor> FVoxelMapTileStore::Downsample(const TArray<FColor>& Pixels, int32 Resolution, int32 Factor)
{
	TArray<FColor> Out;
	if (Factor <= 0 || Resolution <= 0 || Resolution % Factor != 0 || Pixels.Num() != Resolution * Resolution)
	{
		return Out;
	}
	const int32 Small = Resolution / Factor;
	const int32 Count = Factor * Factor;
	Out.SetNumUninitialized(Small * Small);
	for (int32 Y = 0; Y < Small; ++Y)
	{
		for (int32 X = 0; X < Small; ++X)
		{
			int32 R = 0, G = 0, B = 0, A = 0;
			for (int32 DY = 0; DY < Factor; ++DY)
			{
				const FColor* Row = &Pixels[(Y * Factor + DY) * Resolution + X * Factor];
				for (int32 DX = 0; DX < Factor; ++DX)
				{
					R += Row[DX].R;
					G += Row[DX].G;
					B += Row[DX].B;
					A += Row[DX].A;
				}
			}
			Out[Y * Small + X] = FColor(
				static_cast<uint8>((R + Count / 2) / Count),
				static_cast<uint8>((G + Count / 2) / Count),
				static_cast<uint8>((B + Count / 2) / Count),
				static_cast<uint8>((A + Count / 2) / Count));
		}
	}
	return Out;
}

void FVoxelMapTileStore::SetBudgets(int64 InDetailBudgetBytes, int64 InOverviewBudgetBytes)
{
	DetailBudgetBytes = FMath::Max<int64>(InDetailBudgetBytes, 0);
	OverviewBudgetBytes = FMath::Max<int64>(InOverviewBudgetBytes, 0);
	Enforce();
}

void FVoxelMapTileStore::Store(uint64 Key, FIntPoint Coord, TArray<FColor>&& Pixels, int32 Resolution, bool bKeepDetail)
{
	FEntry& Entry = Entries.FindOrAdd(Key);
	const bool bHadDetail = Entry.Detail.PixelData.Num() > 0;
	DetailBytes -= ImageBytes(Entry.Detail);
	OverviewBytes -= ImageBytes(Entry.Overview);
	DetailCount -= bHadDetail ? 1 : 0;

	// The overview, from the new image (a size that does not divide keeps the image itself as its overview).
	TArray<FColor> Small = Downsample(Pixels, Resolution, OverviewDownsample);
	Entry.Overview.TileCoord = Coord;
	Entry.Overview.bIsReady = true;
	if (Small.Num() > 0)
	{
		Entry.Overview.Resolution = Resolution / OverviewDownsample;
		Entry.Overview.PixelData = MoveTemp(Small);
	}
	else
	{
		Entry.Overview.Resolution = Resolution;
		Entry.Overview.PixelData = Pixels;
	}

	Entry.Detail.TileCoord = Coord;
	Entry.Detail.Resolution = Resolution;
	Entry.Detail.bIsReady = true;
	if (bKeepDetail || bHadDetail)
	{
		Entry.Detail.PixelData = MoveTemp(Pixels);
	}
	else
	{
		Entry.Detail.PixelData.Empty();
	}

	DetailBytes += ImageBytes(Entry.Detail);
	OverviewBytes += ImageBytes(Entry.Overview);
	DetailCount += Entry.Detail.PixelData.Num() > 0 ? 1 : 0;
	Entry.LastUse = ++UseClock;
	Enforce();
}

const FVoxelMapTile* FVoxelMapTileStore::Find(uint64 Key, bool bTouch) const
{
	const FEntry* Entry = Entries.Find(Key);
	if (!Entry)
	{
		return nullptr;
	}
	if (bTouch)
	{
		Entry->LastUse = ++UseClock;
	}
	return Entry->Detail.PixelData.Num() > 0 ? &Entry->Detail : &Entry->Overview;
}

bool FVoxelMapTileStore::HasDetail(uint64 Key) const
{
	const FEntry* Entry = Entries.Find(Key);
	return Entry && Entry->Detail.PixelData.Num() > 0;
}

void FVoxelMapTileStore::Remove(uint64 Key)
{
	if (const FEntry* Entry = Entries.Find(Key))
	{
		DetailBytes -= ImageBytes(Entry->Detail);
		OverviewBytes -= ImageBytes(Entry->Overview);
		DetailCount -= Entry->Detail.PixelData.Num() > 0 ? 1 : 0;
		Entries.Remove(Key);
	}
}

void FVoxelMapTileStore::Reset()
{
	Entries.Empty();
	UseClock = 0;
	DetailBytes = 0;
	OverviewBytes = 0;
	DetailCount = 0;
	DetailEvictions = 0;
	OverviewEvictions = 0;
}

int32 FVoxelMapTileStore::Enforce()
{
	int32 Dropped = 0;

	// Detail first: the least recently used detail images go; their tiles keep the overview.
	if (DetailBudgetBytes > 0 && DetailBytes > DetailBudgetBytes)
	{
		TArray<TPair<uint64, uint64>> Order; // (last use, key)
		Order.Reserve(DetailCount);
		for (const TPair<uint64, FEntry>& Pair : Entries)
		{
			if (Pair.Value.Detail.PixelData.Num() > 0)
			{
				Order.Emplace(Pair.Value.LastUse, Pair.Key);
			}
		}
		Order.Sort([](const TPair<uint64, uint64>& A, const TPair<uint64, uint64>& B) { return A.Key < B.Key; });
		const int64 Target = DetailBudgetBytes * 9 / 10;
		for (const TPair<uint64, uint64>& Item : Order)
		{
			if (DetailBytes <= Target)
			{
				break;
			}
			FEntry& Entry = Entries[Item.Value];
			DetailBytes -= ImageBytes(Entry.Detail);
			--DetailCount;
			Entry.Detail.PixelData.Empty();
			++DetailEvictions;
			++Dropped;
		}
	}

	// Then overviews: the least recently used tiles without a detail image lose their images altogether.
	if (OverviewBudgetBytes > 0 && OverviewBytes > OverviewBudgetBytes)
	{
		TArray<TPair<uint64, uint64>> Order;
		Order.Reserve(Entries.Num());
		for (const TPair<uint64, FEntry>& Pair : Entries)
		{
			if (Pair.Value.Detail.PixelData.Num() == 0)
			{
				Order.Emplace(Pair.Value.LastUse, Pair.Key);
			}
		}
		Order.Sort([](const TPair<uint64, uint64>& A, const TPair<uint64, uint64>& B) { return A.Key < B.Key; });
		const int64 Target = OverviewBudgetBytes * 9 / 10;
		for (const TPair<uint64, uint64>& Item : Order)
		{
			if (OverviewBytes <= Target)
			{
				break;
			}
			OverviewBytes -= ImageBytes(Entries[Item.Value].Overview);
			Entries.Remove(Item.Value);
			++OverviewEvictions;
			++Dropped;
		}
	}
	return Dropped;
}
