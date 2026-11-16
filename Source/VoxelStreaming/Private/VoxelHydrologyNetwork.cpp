// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelHydrologyNetwork.h"
#include "VoxelStreaming.h"
#include "Async/ParallelFor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AssertionMacros.h"
#include "Misc/ScopeLock.h"

// Who asked: a tile worked out on the game thread inside a gather is the one cost of the hydrology a frame can feel,
// and the counter (FVoxelHydrologyStats::SyncTiles) says how many, not which query.
static TAutoConsoleVariable<int32> CVarHydrologyTraceSync(
	TEXT("voxel.Water.Hydrology.TraceSync"), 0,
	TEXT("1: log every gather that works hydrology tiles out on the game thread (its region, the tiles, the call stack: who asked). Diagnostic."),
	ECVF_Default);

namespace VoxelHydrologyNetworkPrivate
{
	/** A Ready gather over more tiles than this takes what is kept and queues nothing (a far-field patch or a map tile above the near levels) */
	constexpr int64 ReadyTileLimit = 64;

	/** A Complete gather over more tiles than this is not worked out (no reader of the generated voxels asks over so wide a region) */
	constexpr int64 CompleteTileLimit = 4096;

	/** The caches' limits: over them the tiles used longest ago go, and the descents, lakes and systems are dropped whole (they are worked out again as tiles need them) */
	constexpr int32 MaxTiles = 32768;
	constexpr int32 MaxSquares = 400000;

	/** Lattice squares traced with one set of kept ground heights */
	constexpr int32 SquaresPerBatch = 16;
}

FVoxelHydrologyNetwork::FVoxelHydrologyNetwork(const FVoxelHydrologyGround& InGround, const FVoxelHydrologySettings& InSettings, double InViewDistance)
	: Ground(InGround)
	, Settings(FVoxelHydrology::Sanitize(InSettings, InGround))
	, ViewDistance(FMath::Max(InViewDistance, 1000.0))
{
	CellSize = Settings.CellVoxels * FMath::Max(Ground.VoxelSize, 1.0);
	TileSize = Settings.TileCells * CellSize;
}

FBox2D FVoxelHydrologyNetwork::BoxOfTile(const FIntPoint& Tile) const
{
	return FBox2D(FVector2D(Tile.X * TileSize, Tile.Y * TileSize), FVector2D((Tile.X + 1) * TileSize, (Tile.Y + 1) * TileSize));
}

FIntRect FVoxelHydrologyNetwork::TilesOf(const FBox2D& Region) const
{
	return FIntRect(
		FMath::FloorToInt32(Region.Min.X / TileSize), FMath::FloorToInt32(Region.Min.Y / TileSize),
		FMath::FloorToInt32(Region.Max.X / TileSize) + 1, FMath::FloorToInt32(Region.Max.Y / TileSize) + 1);
}

double FVoxelHydrologyNetwork::GetWarmUpRadius() const
{
	return Settings.WarmUpViewDistances * ViewDistance + TileSize;
}

double FVoxelHydrologyNetwork::GetFarRadius() const
{
	// What draws far away: the far field's beds, and the lakes out to where they are drawn.
	return FMath::Max3(static_cast<double>(Settings.FarViewDistances) * ViewDistance, GetWarmUpRadius(), static_cast<double>(Settings.LakeDrawDistance));
}

// ==================== The generator's cache ====================

TSharedPtr<const FVoxelHydrologyTrace> FVoxelHydrologyNetwork::FindTrace(const FIntPoint& Square)
{
	FScopeLock Scope(&Lock);
	const TSharedRef<const FVoxelHydrologyTrace>* Found = Traces.Find(Square);
	return Found ? Found->ToSharedPtr() : nullptr;
}

TSharedRef<const FVoxelHydrologyTrace> FVoxelHydrologyNetwork::StoreTrace(const TSharedRef<const FVoxelHydrologyTrace>& Trace)
{
	FScopeLock Scope(&Lock);
	if (const TSharedRef<const FVoxelHydrologyTrace>* Found = Traces.Find(Trace->Square))
	{
		return *Found;
	}
	++Stats.TracesMade;
	return Traces.Add(Trace->Square, Trace);
}

TSharedPtr<const FVoxelHydrologyLake> FVoxelHydrologyNetwork::FindLake(const FIntPoint& Pit)
{
	FScopeLock Scope(&Lock);
	const TSharedRef<const FVoxelHydrologyLake>* Found = Lakes.Find(Pit);
	return Found ? Found->ToSharedPtr() : nullptr;
}

TSharedRef<const FVoxelHydrologyLake> FVoxelHydrologyNetwork::StoreLake(const TSharedRef<const FVoxelHydrologyLake>& Lake)
{
	FScopeLock Scope(&Lock);
	if (const TSharedRef<const FVoxelHydrologyLake>* Found = Lakes.Find(Lake->Pit))
	{
		return *Found;
	}
	++Stats.LakesMade;
	return Lakes.Add(Lake->Pit, Lake);
}

TSharedPtr<const FVoxelHydrologySystem> FVoxelHydrologyNetwork::FindSystem(const FIntPoint& Mouth)
{
	FScopeLock Scope(&Lock);
	const TSharedRef<const FVoxelHydrologySystem>* Found = Systems.Find(Mouth);
	return Found ? Found->ToSharedPtr() : nullptr;
}

TSharedRef<const FVoxelHydrologySystem> FVoxelHydrologyNetwork::StoreSystem(const TSharedRef<const FVoxelHydrologySystem>& System)
{
	FScopeLock Scope(&Lock);
	if (const TSharedRef<const FVoxelHydrologySystem>* Found = Systems.Find(System->Mouth))
	{
		return *Found;
	}
	++Stats.SystemsMade;
	return Systems.Add(System->Mouth, System);
}

// ==================== Working tiles out ====================

double FVoxelHydrologyNetwork::BuildNow(const TArray<FIntPoint>& InTiles, EBuild Who, bool bParallel)
{
	using namespace VoxelHydrologyNetworkPrivate;
	if (InTiles.Num() == 0)
	{
		return 0.0;
	}
	const double Start = FPlatformTime::Seconds();
	const EParallelForFlags Flags = bParallel ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread;

	// The descents first: every lattice square the tiles depend on that is not kept, each traced once, in batches that
	// share the ground heights they read. The tiles' regions then find them all kept.
	TArray<FIntPoint> Squares;
	{
		TSet<FIntPoint> Seen;
		FScopeLock Scope(&Lock);
		for (const FIntPoint& Tile : InTiles)
		{
			const FIntRect Rect = FVoxelHydrology::GetSquaresFor(Settings, Ground, BoxOfTile(Tile));
			for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
			{
				for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
				{
					const FIntPoint Square(X, Y);
					bool bAlready = false;
					Seen.Add(Square, &bAlready);
					if (!bAlready && !Traces.Contains(Square))
					{
						Squares.Add(Square);
					}
				}
			}
		}
	}
	const int32 NumBatches = FMath::DivideAndRoundUp(Squares.Num(), SquaresPerBatch);
	ParallelFor(NumBatches, [this, &Squares](int32 Batch)
	{
		FVoxelHydrologyHeights Heights(Ground, Settings);
		const int32 End = FMath::Min((Batch + 1) * SquaresPerBatch, Squares.Num());
		for (int32 Index = Batch * SquaresPerBatch; Index < End; ++Index)
		{
			StoreTrace(FVoxelHydrology::TraceSpring(Settings, Heights, Squares[Index], this));
		}
	}, Flags);

	TArray<TSharedPtr<FTile, ESPMode::ThreadSafe>> Made;
	Made.SetNum(InTiles.Num());
	ParallelFor(InTiles.Num(), [this, &InTiles, &Made](int32 Index)
	{
		const FBox2D Box = BoxOfTile(InTiles[Index]);
		FVoxelHydrologyRegion Region;
		FVoxelHydrology::BuildRegion(Settings, Ground, Box, this, Region);
		TSharedPtr<FTile, ESPMode::ThreadSafe> Tile = MakeShared<FTile, ESPMode::ThreadSafe>();
		for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Region.Lakes)
		{
			Tile->Bodies.Add(Lake->Body);
		}
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				if (Reach.Body.MeetsRegion(Box))
				{
					Tile->Bodies.Add(Reach.Body);
				}
				if (Reach.Bed.IsValid() && Reach.Bed->MeetsRegion(Box))
				{
					Tile->Beds.Add(Reach.Bed.ToSharedRef());
				}
			}
		}
		Made[Index] = MoveTemp(Tile);
	}, (bParallel && InTiles.Num() > 1) ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread);
	const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;

	FScopeLock Scope(&Lock);
	const bool bGameThread = IsInGameThread();
	switch (Who)
	{
	case EBuild::Gather:
		if (bGameThread && bStarted)
		{
			Stats.SyncTiles += InTiles.Num();
			Stats.SyncMs += Ms;
			Stats.WorstSyncMs = FMath::Max(Stats.WorstSyncMs, Ms);
		}
		else if (bGameThread)
		{
			Stats.StartSyncTiles += InTiles.Num();
			Stats.StartSyncMs += Ms;
		}
		else
		{
			Stats.OffThreadTiles += InTiles.Num();
			Stats.OffThreadMs += Ms;
		}
		break;
	case EBuild::Worker:
		Stats.WorkerTiles += InTiles.Num();
		Stats.WorkerMs += Ms;
		break;
	case EBuild::Warm:
		Stats.WarmTiles += InTiles.Num();
		Stats.WarmMs += Ms;
		break;
	}
	for (int32 Index = 0; Index < InTiles.Num(); ++Index)
	{
		const FIntPoint& Key = InTiles[Index];
		const bool bMissed = ReadyMissed.Remove(Key) > 0;
		if (FTileEntry* Existing = Tiles.Find(Key))
		{
			// Worked out twice (the worker and a gather at once): the same tile, the first is the one kept.
			Existing->LastUsed = ++UseCounter;
			continue;
		}
		const bool bHasBody = Made[Index]->Bodies.Num() > 0 || Made[Index]->Beds.Num() > 0;
		FTileEntry Entry;
		Entry.Tile = Made[Index];
		Entry.LastUsed = ++UseCounter;
		Tiles.Add(Key, MoveTemp(Entry));
		if (bHasBody)
		{
			bBodiesArrived = true;
			if (bMissed)
			{
				Announcements.Add(BoxOfTile(Key));
				++Stats.Announced;
			}
		}
	}
	Trim();
	return Ms;
}

void FVoxelHydrologyNetwork::Trim()
{
	using namespace VoxelHydrologyNetworkPrivate;
	if (Tiles.Num() > MaxTiles)
	{
		TArray<uint64> Used;
		Used.Reserve(Tiles.Num());
		for (const TPair<FIntPoint, FTileEntry>& Entry : Tiles)
		{
			Used.Add(Entry.Value.LastUsed);
		}
		Used.Sort();
		const uint64 Median = Used[Used.Num() / 2];
		for (auto It = Tiles.CreateIterator(); It; ++It)
		{
			if (It->Value.LastUsed < Median)
			{
				It.RemoveCurrent();
			}
		}
		++TrimEpoch;
	}
	if (Traces.Num() > MaxSquares)
	{
		// What a tile is worked out from, not what a gather reads: dropped whole, and worked out again around the next
		// tile that needs it (the same descents, the same bodies).
		Traces.Reset();
		Lakes.Reset();
		Systems.Reset();
	}
}

bool FVoxelHydrologyNetwork::Enqueue(const FIntPoint& Tile)
{
	if (bShutdown || Tiles.Contains(Tile) || QueuedTiles.Contains(Tile))
	{
		return false;
	}
	Queue.Add(Tile);
	QueuedTiles.Add(Tile);
	if (!bWorkerRunning)
	{
		bWorkerRunning = true;
		WorkerTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Self = AsShared()]() { Self->RunWorker(); }, UE::Tasks::ETaskPriority::BackgroundNormal);
	}
	return true;
}

void FVoxelHydrologyNetwork::RunWorker()
{
	for (;;)
	{
		TArray<FIntPoint> One;
		{
			FScopeLock Scope(&Lock);
			if (bShutdown || Queue.Num() == 0)
			{
				bWorkerRunning = false;
				return;
			}
			const FIntPoint Tile = Queue[0];
			Queue.RemoveAt(0, EAllowShrinking::No);
			QueuedTiles.Remove(Tile);
			if (Tiles.Contains(Tile))
			{
				continue; // a gather worked it out while it waited
			}
			One.Add(Tile);
		}
		BuildNow(One, EBuild::Worker, /*bParallel=*/false);
	}
}

// ==================== The gathers ====================

void FVoxelHydrologyNetwork::Gather(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>* OutBodies, TArray<FVoxelConditioningLineRef>* OutLines)
{
	using namespace VoxelHydrologyNetworkPrivate;
	if (!Region.bIsValid)
	{
		return;
	}
	const FIntRect Rect = TilesOf(Region);
	const int64 Count = static_cast<int64>(Rect.Width()) * Rect.Height();
	TArray<TSharedPtr<const FTile, ESPMode::ThreadSafe>, TInlineAllocator<4>> Found;
	TArray<FIntPoint> Missing;
	const auto Collect = [&]()
	{
		// Lock held: the kept tiles of the region, and the ones that are not kept.
		Found.Reset();
		Missing.Reset();
		for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
		{
			for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
			{
				const FIntPoint Key(X, Y);
				if (FTileEntry* Entry = Tiles.Find(Key))
				{
					Entry->LastUsed = ++UseCounter;
					Found.Add(Entry->Tile);
				}
				else
				{
					Missing.Add(Key);
				}
			}
		}
	};
	bool bWarn = false;
	{
		FScopeLock Scope(&Lock);
		const bool bReady = Need == EVoxelConditioningGather::Ready;
		++(bReady ? Stats.ReadyGathers : Stats.CompleteGathers);
		const bool bTooWide = Count > (bReady ? ReadyTileLimit : CompleteTileLimit);
		if (bTooWide)
		{
			// What is at hand over the region, and nothing queued or worked out.
			++(bReady ? Stats.ReadyTooWide : Stats.CompleteTooWide);
			bWarn = !bReady && !bWarnedTooWide;
			bWarnedTooWide |= bWarn;
			for (TPair<FIntPoint, FTileEntry>& Entry : Tiles)
			{
				if (Entry.Key.X >= Rect.Min.X && Entry.Key.X < Rect.Max.X && Entry.Key.Y >= Rect.Min.Y && Entry.Key.Y < Rect.Max.Y)
				{
					Found.Add(Entry.Value.Tile);
				}
			}
		}
		else
		{
			Collect();
			if (bReady)
			{
				for (const FIntPoint& Key : Missing)
				{
					bool bAlready = false;
					ReadyMissed.Add(Key, &bAlready);
					Stats.ReadyMisses += bAlready ? 0 : 1;
					Enqueue(Key);
				}
				Missing.Reset();
			}
		}
	}
	if (bWarn)
	{
		UE_LOG(LogVoxelStreaming, Warning,
			TEXT("Hydrology: a complete gather over (%.0f, %.0f)-(%.0f, %.0f), %lld tiles, is too wide to work out: it gets the bodies at hand. A reader of the generated voxels gathers over a chunk or a few; ask with Ready over a region this wide."),
			Region.Min.X, Region.Min.Y, Region.Max.X, Region.Max.Y, Count);
	}

	// A chunk's voxels are generated once: a tile the network does not hold is worked out then and there.
	if (Missing.Num() > 0)
	{
		const TArray<FIntPoint> ToBuild = Missing;
		// On the game thread in parallel, since the frame waits; a worker's gather (the POI layer's placement and road
		// searches) takes its own time and leaves the task threads to the world's streaming.
		const double Ms = BuildNow(ToBuild, EBuild::Gather, /*bParallel=*/IsInGameThread());
		if (IsInGameThread() && bStarted && CVarHydrologyTraceSync.GetValueOnAnyThread() != 0)
		{
			UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Water.Hydrology.TraceSync: a gather over (%.0f, %.0f)-(%.0f, %.0f) worked out %d tile(s) in %.1f ms; the first is (%d, %d). Who asked:"),
				Region.Min.X, Region.Min.Y, Region.Max.X, Region.Max.Y, ToBuild.Num(), Ms, ToBuild[0].X, ToBuild[0].Y);
			FDebug::DumpStackTraceToLog(ELogVerbosity::Display);
		}
		FScopeLock Scope(&Lock);
		Collect();
	}

	// A body or a bed that reaches several of the tiles is kept by each: once.
	const bool bSeveral = Found.Num() > 1;
	TSet<uint32> SeenBodies;
	TSet<int64> SeenBeds;
	for (const TSharedPtr<const FTile, ESPMode::ThreadSafe>& Tile : Found)
	{
		if (OutBodies)
		{
			for (const FVoxelWaterBody& Body : Tile->Bodies)
			{
				if (!Body.MeetsRegion(Region))
				{
					continue;
				}
				if (bSeveral)
				{
					bool bAlready = false;
					SeenBodies.Add(Body.Id, &bAlready);
					if (bAlready)
					{
						continue;
					}
				}
				OutBodies->Add(Body);
			}
		}
		if (OutLines)
		{
			for (const FVoxelConditioningLineRef& Bed : Tile->Beds)
			{
				if (!Bed->MeetsRegion(Region))
				{
					continue;
				}
				if (bSeveral)
				{
					bool bAlready = false;
					SeenBeds.Add(Bed->GetId(), &bAlready);
					if (bAlready)
					{
						continue;
					}
				}
				OutLines->Add(Bed);
			}
		}
	}
}

void FVoxelHydrologyNetwork::GatherWaterBodies(const FBox2D& Bounds, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& OutBodies) const
{
	const_cast<FVoxelHydrologyNetwork*>(this)->Gather(Bounds, Need, &OutBodies, nullptr);
}

bool FVoxelHydrologyNetwork::HoldsWaterBodies(const FBox2D& Bounds) const
{
	if (!Bounds.bIsValid)
	{
		return true;
	}
	const FIntRect Rect = TilesOf(Bounds);
	if (static_cast<int64>(Rect.Width()) * Rect.Height() > VoxelHydrologyNetworkPrivate::CompleteTileLimit)
	{
		return false;
	}
	FScopeLock Scope(&Lock);
	for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
	{
		for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
		{
			if (!Tiles.Contains(FIntPoint(X, Y)))
			{
				return false;
			}
		}
	}
	return true;
}

void FVoxelHydrologyNetwork::GatherConditioningLines(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelConditioningLineRef>& OutLines) const
{
	const_cast<FVoxelHydrologyNetwork*>(this)->Gather(Region, Need, nullptr, &OutLines);
}

int32 FVoxelHydrologyNetwork::WarmUp(TConstArrayView<FVector2D> Centres, double Radius, bool bWait)
{
	if (Radius <= 0.0 || Centres.Num() == 0)
	{
		return 0;
	}
	// The tiles within the distance of a centre that are not kept, the nearest first.
	TArray<TPair<double, FIntPoint>> Wanted;
	{
		FScopeLock Scope(&Lock);
		TSet<FIntPoint> Seen;
		for (const FVector2D& Centre : Centres)
		{
			if (!bWait)
			{
				// A viewer that has moved less than a quarter of a tile since a disc at least this wide was gone through:
				// its tiles are kept or queued already.
				bool bGoneThrough = false;
				for (const FWarmedDisc& Disc : WarmedDiscs)
				{
					bGoneThrough |= Disc.Epoch == TrimEpoch && Disc.Radius >= Radius && FVector2D::DistSquared(Disc.Centre, Centre) < FMath::Square(0.25 * TileSize);
				}
				if (bGoneThrough)
				{
					continue;
				}
				if (WarmedDiscs.Num() >= 8)
				{
					WarmedDiscs.RemoveAt(0);
				}
				WarmedDiscs.Add(FWarmedDisc{ Centre, Radius, TrimEpoch });
			}
			const FIntRect Rect = TilesOf(FBox2D(Centre - FVector2D(Radius, Radius), Centre + FVector2D(Radius, Radius)));
			for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
			{
				for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
				{
					const FIntPoint Key(X, Y);
					const double Distance = FMath::Sqrt(BoxOfTile(Key).ComputeSquaredDistanceToPoint(Centre));
					if (Distance > Radius)
					{
						continue;
					}
					if (FTileEntry* Entry = Tiles.Find(Key))
					{
						Entry->LastUsed = ++UseCounter; // a tile near a viewer is not the one to drop
						continue;
					}
					bool bAlready = false;
					Seen.Add(Key, &bAlready);
					if (!bAlready)
					{
						Wanted.Emplace(Distance, Key);
					}
				}
			}
		}
		Wanted.Sort([](const TPair<double, FIntPoint>& A, const TPair<double, FIntPoint>& B)
		{
			return A.Key != B.Key ? A.Key < B.Key : (A.Value.X != B.Value.X ? A.Value.X < B.Value.X : A.Value.Y < B.Value.Y);
		});
		if (!bWait)
		{
			int32 Count = 0;
			for (const TPair<double, FIntPoint>& Entry : Wanted)
			{
				Count += Enqueue(Entry.Value) ? 1 : 0;
			}
			return Count;
		}
	}
	TArray<FIntPoint> Missing;
	Missing.Reserve(Wanted.Num());
	for (const TPair<double, FIntPoint>& Entry : Wanted)
	{
		Missing.Add(Entry.Value);
	}
	BuildNow(Missing, EBuild::Warm, /*bParallel=*/true);
	return Missing.Num();
}

void FVoxelHydrologyNetwork::TakeAnnouncements(TArray<FBox2D>& OutRegions)
{
	FScopeLock Scope(&Lock);
	OutRegions.Append(Announcements);
	Announcements.Reset();
}

bool FVoxelHydrologyNetwork::TakeBodiesArrived()
{
	FScopeLock Scope(&Lock);
	const bool bArrived = bBodiesArrived;
	bBodiesArrived = false;
	return bArrived;
}

void FVoxelHydrologyNetwork::MarkStarted()
{
	FScopeLock Scope(&Lock);
	bStarted = true;
}

void FVoxelHydrologyNetwork::Survey(const FBox2D& Region, FVoxelHydrologyRegion& Out)
{
	FVoxelHydrology::BuildRegion(Settings, Ground, Region, this, Out);
}

FVoxelHydrologyStats FVoxelHydrologyNetwork::GetStats() const
{
	FScopeLock Scope(&Lock);
	FVoxelHydrologyStats Out = Stats;
	Out.Tiles = Tiles.Num();
	Out.Queued = Queue.Num();
	for (const TPair<FIntPoint, FTileEntry>& Entry : Tiles)
	{
		Out.TilesWithBodies += (Entry.Value.Tile->Bodies.Num() > 0 || Entry.Value.Tile->Beds.Num() > 0) ? 1 : 0;
	}
	Out.Squares = Traces.Num();
	for (const TPair<FIntPoint, TSharedRef<const FVoxelHydrologyTrace>>& Entry : Traces)
	{
		Out.Springs += Entry.Value->End != EVoxelHydrologyEnd::NoSpring ? 1 : 0;
		Out.Rivers += Entry.Value->bKept ? 1 : 0;
	}
	Out.Lakes = Lakes.Num();
	for (const TPair<FIntPoint, TSharedRef<const FVoxelHydrologyLake>>& Entry : Lakes)
	{
		Out.KeptLakes += Entry.Value->bKept ? 1 : 0;
	}
	Out.Systems = Systems.Num();
	for (const TPair<FIntPoint, TSharedRef<const FVoxelHydrologySystem>>& Entry : Systems)
	{
		Out.RefusedSystems += Entry.Value->Refused.IsEmpty() ? 0 : 1;
		Out.Reaches += Entry.Value->Reaches.Num();
		Out.Conflicts += Entry.Value->NumConflicts;
	}
	return Out;
}

void FVoxelHydrologyNetwork::ResetStats()
{
	FScopeLock Scope(&Lock);
	Stats = FVoxelHydrologyStats();
}

void FVoxelHydrologyNetwork::WaitForWorker()
{
	for (;;)
	{
		UE::Tasks::FTask Task;
		{
			FScopeLock Scope(&Lock);
			if (!bWorkerRunning)
			{
				return;
			}
			Task = WorkerTask;
		}
		Task.Wait();
	}
}

void FVoxelHydrologyNetwork::Shutdown()
{
	FScopeLock Scope(&Lock);
	bShutdown = true;
	Queue.Reset();
	QueuedTiles.Reset();
}
