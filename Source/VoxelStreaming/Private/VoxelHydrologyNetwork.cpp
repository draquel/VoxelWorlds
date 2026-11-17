// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelHydrologyNetwork.h"
#include "VoxelStreaming.h"
#include "Async/ParallelFor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AssertionMacros.h"
#include "Misc/ScopeLock.h"
#include "VoxelPolyline.h"

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
	, Grid(Ground, Settings)
{
	CellSize = Settings.CellVoxels * FMath::Max(Ground.VoxelSize, 1.0);
	TileSize = Settings.TileCells * CellSize;
	if (Ground.bRadial)
	{
		// A planet's tiles: as many along a face's edge as bring a tile nearest the width asked for. A tile is a region
		// the bodies are kept by, so it need not be whole cells.
		const double Quarter = UE_DOUBLE_HALF_PI * FMath::Max(Ground.PlanetRadius, 1.0);
		const int32 TilesPerEdge = FMath::Max(1, FMath::RoundToInt32(Quarter / TileSize));
		TileGrid = FVoxelCubeSphereGrid(TilesPerEdge);
		TileSize = Quarter / TilesPerEdge;
		TileCapAngle = UE_DOUBLE_HALF_PI / TilesPerEdge;
	}
}

FVoxelConditioningRegion FVoxelHydrologyNetwork::RegionOfTile(const FIntPoint& Tile) const
{
	if (Ground.bRadial)
	{
		// The cap around the tile: its middle, out to a bound on its farthest corner (a cube sphere's cell is no wider
		// than at a face's middle, and a corner lies half a cell along each of the tile's two middle lines).
		return FVoxelConditioningRegion::OfCap(TileGrid.CellCentreDirection(Tile), TileCapAngle);
	}
	return FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(Tile.X * TileSize, Tile.Y * TileSize), FVector2D((Tile.X + 1) * TileSize, (Tile.Y + 1) * TileSize)));
}

bool FVoxelHydrologyNetwork::IsRegionOfWorld(const FVoxelConditioningRegion& Region) const
{
	return !Region.bEverywhere && Region.bCap == Ground.bRadial && (Region.bCap || Region.Box.bIsValid);
}

bool FVoxelHydrologyNetwork::VisitTiles(const FVoxelConditioningRegion& Region, int64 Limit, TFunctionRef<bool(const FIntPoint&)> Visit) const
{
	if (Ground.bRadial)
	{
		// More tiles than the limit, without counting them: a tile is no larger than a square of its width at a face's
		// middle, so a cap holds at least its own area over that. (A reader that takes what is at hand asks over caps
		// of any size, thousands of times at a world's start: this must cost nothing for a wide one.)
		const double Angle = FMath::Clamp(Region.AngularRadius, 0.0, UE_DOUBLE_PI);
		if (UE_DOUBLE_TWO_PI * (1.0 - FMath::Cos(Angle)) > static_cast<double>(Limit) * TileCapAngle * TileCapAngle)
		{
			return false;
		}
		// The tiles whose caps meet the cap: a flood through the side neighbours from the tile the cap's middle lies in.
		// (A tile's cap holds the tile, so the tiles that meet a cap are joined through their sides.)
		const double MinDot = FMath::Cos(FMath::Min(Angle + TileCapAngle, UE_DOUBLE_PI));
		static const int32 StepX[4] = { 1, -1, 0, 0 };
		static const int32 StepY[4] = { 0, 0, 1, -1 };
		TSet<FIntPoint> Visited;
		TArray<FIntPoint, TInlineAllocator<64>> Found;
		const FIntPoint Start = TileGrid.CellOfDirection(Region.Direction);
		Visited.Add(Start);
		Found.Add(Start);
		for (int32 Head = 0; Head < Found.Num(); ++Head)
		{
			if (Found.Num() > Limit || !Visit(Found[Head]))
			{
				return false;
			}
			const FIntPoint Tile = Found[Head];
			for (int32 Step = 0; Step < 4; ++Step)
			{
				FIntPoint Next;
				if (!TileGrid.GetAdjacentCell(Tile, StepX[Step], StepY[Step], Next))
				{
					continue;
				}
				bool bAlready = false;
				Visited.Add(Next, &bAlready);
				if (!bAlready && FVector::DotProduct(TileGrid.CellCentreDirection(Next), Region.Direction) >= MinDot)
				{
					Found.Add(Next);
				}
			}
		}
		return true;
	}
	const FIntRect Rect(
		FMath::FloorToInt32(Region.Box.Min.X / TileSize), FMath::FloorToInt32(Region.Box.Min.Y / TileSize),
		FMath::FloorToInt32(Region.Box.Max.X / TileSize) + 1, FMath::FloorToInt32(Region.Box.Max.Y / TileSize) + 1);
	if (static_cast<int64>(Rect.Width()) * Rect.Height() > Limit)
	{
		return false;
	}
	for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
	{
		for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
		{
			if (!Visit(FIntPoint(X, Y)))
			{
				return false;
			}
		}
	}
	return true;
}

bool FVoxelHydrologyNetwork::TilesOf(const FVoxelConditioningRegion& Region, int64 Limit, TArray<FIntPoint>& OutTiles) const
{
	OutTiles.Reset();
	return VisitTiles(Region, Limit, [&OutTiles](const FIntPoint& Tile)
	{
		OutTiles.Add(Tile);
		return true;
	});
}

double FVoxelHydrologyNetwork::DistanceToTile(const FIntPoint& Tile, const FVector& Place) const
{
	const FVoxelConditioningRegion Region = RegionOfTile(Tile);
	if (Region.bCap)
	{
		return Ground.PlanetRadius * FMath::Max(0.0, FVoxelPolylineBounds::AngleBetween(Region.Direction, Place) - Region.AngularRadius);
	}
	return FMath::Sqrt(Region.Box.ComputeSquaredDistanceToPoint(FVector2D(Place.X, Place.Y)));
}

FVector FVoxelHydrologyNetwork::GetPlaceOf(const FVector& WorldPosition) const
{
	return Ground.bRadial
		? (WorldPosition - Ground.PlanetCenter).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector)
		: FVector(WorldPosition.X, WorldPosition.Y, 0.0);
}

FVoxelConditioningRegion FVoxelHydrologyNetwork::GetRegionAround(const FVector& Place, double Radius) const
{
	if (Ground.bRadial)
	{
		return FVoxelConditioningRegion::OfCap(Place, FMath::Min(Radius / FMath::Max(Ground.PlanetRadius, 1.0), UE_DOUBLE_PI));
	}
	const FVector2D Centre(Place.X, Place.Y);
	return FVoxelConditioningRegion::OfBox(FBox2D(Centre - FVector2D(Radius, Radius), Centre + FVector2D(Radius, Radius)));
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
		TArray<FIntPoint> OfTile;
		for (const FIntPoint& Tile : InTiles)
		{
			FVoxelHydrology::GetSquaresFor(Settings, Ground, RegionOfTile(Tile), OfTile);
			FScopeLock Scope(&Lock);
			for (const FIntPoint& Square : OfTile)
			{
				bool bAlready = false;
				Seen.Add(Square, &bAlready);
				if (!bAlready && !Traces.Contains(Square))
				{
					Squares.Add(Square);
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
		const FVoxelConditioningRegion Box = RegionOfTile(InTiles[Index]);
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
				if (FVoxelHydrology::MeetsRegion(Reach.Body, Box, Ground))
				{
					Tile->Bodies.Add(Reach.Body);
				}
				if (Reach.Bed.IsValid() && FVoxelHydrology::MeetsRegion(*Reach.Bed, Box, Ground))
				{
					Tile->Beds.Add(Reach.Bed.ToSharedRef());
				}
			}
		}
		// The bodies' levels as one range, and the tile's middle: what a reader of the range alone takes (GetLevelRangeAtHandOnSphere).
		for (int32 Body = 0; Body < Tile->Bodies.Num(); ++Body)
		{
			const FVoxelWaterBody& Record = Tile->Bodies[Body];
			const double Low = Record.Reach.IsValid() ? Record.Reach->GetMinLevel() : Record.Level;
			const double High = Record.Reach.IsValid() ? Record.Reach->GetMaxLevel() : Record.Level;
			Tile->MinLevel = Body == 0 ? Low : FMath::Min(Tile->MinLevel, Low);
			Tile->MaxLevel = Body == 0 ? High : FMath::Max(Tile->MaxLevel, High);
		}
		if (Box.bCap)
		{
			Tile->Centre = Box.Direction;
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
				Announcements.Add(RegionOfTile(Key));
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
		FWarmedDisc Disc;
		bool bDisc = false;
		{
			FScopeLock Scope(&Lock);
			if (!bShutdown && PendingDiscs.Num() > 0)
			{
				Disc = PendingDiscs[0];
				PendingDiscs.RemoveAt(0, EAllowShrinking::No);
				bDisc = true;
			}
		}
		if (bDisc)
		{
			// A planet's disc a warm-up asked for: its tiles that are neither kept nor queued, the nearest first, after
			// what is queued already.
			TArray<TPair<double, FIntPoint>> Near;
			VisitTiles(GetRegionAround(Disc.Centre, Disc.Radius), TNumericLimits<int32>::Max(), [&](const FIntPoint& Key)
			{
				const double Distance = DistanceToTile(Key, Disc.Centre);
				if (Distance <= Disc.Radius)
				{
					Near.Emplace(Distance, Key);
				}
				return true;
			});
			Near.Sort([](const TPair<double, FIntPoint>& A, const TPair<double, FIntPoint>& B)
			{
				return A.Key != B.Key ? A.Key < B.Key : (A.Value.X != B.Value.X ? A.Value.X < B.Value.X : A.Value.Y < B.Value.Y);
			});
			FScopeLock Scope(&Lock);
			for (const TPair<double, FIntPoint>& Entry : Near)
			{
				if (FTileEntry* Kept = Tiles.Find(Entry.Value))
				{
					Kept->LastUsed = ++UseCounter; // a tile near a viewer is not the one to drop
				}
				else if (!bShutdown && !QueuedTiles.Contains(Entry.Value))
				{
					Queue.Add(Entry.Value);
					QueuedTiles.Add(Entry.Value);
				}
			}
			continue;
		}
		FRequestedRegion Requested;
		bool bRequested = false;
		{
			FScopeLock Scope(&Lock);
			if (!bShutdown && PendingRegions.Num() > 0)
			{
				Requested = PendingRegions[0];
				PendingRegions.RemoveAt(0, EAllowShrinking::No);
				bRequested = true;
			}
		}
		if (bRequested)
		{
			// A region RequestRegion asked for (G232): its tiles that are neither kept nor queued, the nearest to its place
			// first, after what is queued already.
			TArray<TPair<double, FIntPoint>> Near;
			VisitTiles(Requested.Region, TNumericLimits<int32>::Max(), [&](const FIntPoint& Key)
			{
				Near.Emplace(DistanceToTile(Key, Requested.Place), Key);
				return true;
			});
			Near.Sort([](const TPair<double, FIntPoint>& A, const TPair<double, FIntPoint>& B)
			{
				return A.Key != B.Key ? A.Key < B.Key : (A.Value.X != B.Value.X ? A.Value.X < B.Value.X : A.Value.Y < B.Value.Y);
			});
			FScopeLock Scope(&Lock);
			int32 Count = 0;
			for (const TPair<double, FIntPoint>& Entry : Near)
			{
				if (Count >= Requested.MaxTiles)
				{
					break;
				}
				Count += Enqueue(Entry.Value) ? 1 : 0;
			}
			Stats.RequestedTiles += Count;
			continue;
		}
		{
			FScopeLock Scope(&Lock);
			if ((PendingDiscs.Num() > 0 || PendingRegions.Num() > 0) && !bShutdown)
			{
				continue; // a disc or a region was asked for meanwhile
			}
			if (bShutdown || Queue.Num() == 0)
			{
				bWorkerRunning = false;
				// Nothing waits: every disc a warm-up went through is whole.
				for (FWarmedDisc& Warmed : WarmedDiscs)
				{
					Warmed.bComplete = true;
				}
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

void FVoxelHydrologyNetwork::Gather(const FVoxelConditioningRegion& Region, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>* OutBodies, TArray<FVoxelConditioningLineRef>* OutLines)
{
	using namespace VoxelHydrologyNetworkPrivate;
	if (!IsRegionOfWorld(Region))
	{
		return;
	}
	const bool bReady = Need == EVoxelConditioningGather::Ready;
	TArray<FIntPoint> Keys;
	const bool bTooWide = !TilesOf(Region, bReady ? ReadyTileLimit : CompleteTileLimit, Keys);
	TArray<TSharedPtr<const FTile, ESPMode::ThreadSafe>, TInlineAllocator<4>> Found;
	TArray<FIntPoint> Missing;
	const auto Collect = [&]()
	{
		// Lock held: the kept tiles of the region, and the ones that are not kept.
		Found.Reset();
		Missing.Reset();
		for (const FIntPoint& Key : Keys)
		{
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
	};
	bool bWarn = false;
	{
		FScopeLock Scope(&Lock);
		++(bReady ? Stats.ReadyGathers : Stats.CompleteGathers);
		if (bTooWide)
		{
			// What is at hand over the region, and nothing queued or worked out. The bodies are told from the region's
			// below, each by its own footprint.
			++(bReady ? Stats.ReadyTooWide : Stats.CompleteTooWide);
			bWarn = !bReady && !bWarnedTooWide;
			bWarnedTooWide |= bWarn;
			if (Region.bCap)
			{
				// The kept tiles whose caps meet the cap, each by its middle.
				const double MinDot = FMath::Cos(FMath::Min(Region.AngularRadius + TileCapAngle, UE_DOUBLE_PI));
				for (TPair<FIntPoint, FTileEntry>& Entry : Tiles)
				{
					if (FVector::DotProduct(Entry.Value.Tile->Centre, Region.Direction) >= MinDot)
					{
						Found.Add(Entry.Value.Tile);
					}
				}
			}
			else
			{
				const FIntRect Rect(
					FMath::FloorToInt32(Region.Box.Min.X / TileSize), FMath::FloorToInt32(Region.Box.Min.Y / TileSize),
					FMath::FloorToInt32(Region.Box.Max.X / TileSize) + 1, FMath::FloorToInt32(Region.Box.Max.Y / TileSize) + 1);
				for (TPair<FIntPoint, FTileEntry>& Entry : Tiles)
				{
					if (Entry.Key.X >= Rect.Min.X && Entry.Key.X < Rect.Max.X && Entry.Key.Y >= Rect.Min.Y && Entry.Key.Y < Rect.Max.Y)
					{
						Found.Add(Entry.Value.Tile);
					}
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
	if (bWarn && Region.bCap)
	{
		UE_LOG(LogVoxelStreaming, Warning,
			TEXT("Hydrology: a complete gather over a cap of %.2f degrees, more than %lld tiles, is too wide to work out: it gets the bodies at hand. A reader of the generated voxels gathers over a chunk or a few; ask with Ready over a region this wide."),
			FMath::RadiansToDegrees(Region.AngularRadius), CompleteTileLimit);
	}
	else if (bWarn)
	{
		UE_LOG(LogVoxelStreaming, Warning,
			TEXT("Hydrology: a complete gather over (%.0f, %.0f)-(%.0f, %.0f), more than %lld tiles, is too wide to work out: it gets the bodies at hand. A reader of the generated voxels gathers over a chunk or a few; ask with Ready over a region this wide."),
			Region.Box.Min.X, Region.Box.Min.Y, Region.Box.Max.X, Region.Box.Max.Y, CompleteTileLimit);
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
			const FString Where = Region.bCap
				? FString::Printf(TEXT("a cap of %.3f degrees round (%.4f, %.4f, %.4f)"), FMath::RadiansToDegrees(Region.AngularRadius), Region.Direction.X, Region.Direction.Y, Region.Direction.Z)
				: FString::Printf(TEXT("(%.0f, %.0f)-(%.0f, %.0f)"), Region.Box.Min.X, Region.Box.Min.Y, Region.Box.Max.X, Region.Box.Max.Y);
			UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Water.Hydrology.TraceSync: a gather over %s worked out %d tile(s) in %.1f ms; the first is (%d, %d). Who asked:"),
				*Where, ToBuild.Num(), Ms, ToBuild[0].X, ToBuild[0].Y);
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
				if (!FVoxelHydrology::MeetsRegion(Body, Region, Ground))
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
				if (!FVoxelHydrology::MeetsRegion(*Bed, Region, Ground))
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
	const_cast<FVoxelHydrologyNetwork*>(this)->Gather(FVoxelConditioningRegion::OfBox(Bounds), Need, &OutBodies, nullptr);
}

void FVoxelHydrologyNetwork::GatherWaterBodiesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need,
	TArray<FVoxelWaterBody>& OutBodies) const
{
	const_cast<FVoxelHydrologyNetwork*>(this)->Gather(FVoxelConditioningRegion::OfCap(Direction, AngularRadius), Need, &OutBodies, nullptr);
}

bool FVoxelHydrologyNetwork::Holds(const FVoxelConditioningRegion& Region) const
{
	if (!IsRegionOfWorld(Region))
	{
		return true;
	}
	FScopeLock Scope(&Lock);
	if (Region.bCap)
	{
		// A cap inside a disc a warm-up has gone through whole: every tile it meets is kept.
		const auto Inside = [this, &Region](const FWarmedDisc& Disc)
		{
			return Disc.bComplete && Disc.Epoch == TrimEpoch && Grid.GetDistance(Disc.Centre, Region.Direction) + Region.AngularRadius * Ground.PlanetRadius <= Disc.Radius;
		};
		for (const FWarmedDisc& Disc : HeldDiscs)
		{
			if (Inside(Disc))
			{
				return true;
			}
		}
		for (const FWarmedDisc& Disc : WarmedDiscs)
		{
			if (Inside(Disc))
			{
				return true;
			}
		}
	}
	// Else tile by tile, as far as the first that is not kept.
	return VisitTiles(Region, VoxelHydrologyNetworkPrivate::CompleteTileLimit, [this](const FIntPoint& Key) { return Tiles.Contains(Key); });
}

bool FVoxelHydrologyNetwork::GetLevelRangeAtHandOnSphere(const FVector& Direction, double AngularRadius, bool& bOutAny, double& OutMinLevel, double& OutMaxLevel) const
{
	bOutAny = false;
	OutMinLevel = 0.0;
	OutMaxLevel = 0.0;
	if (!Ground.bRadial)
	{
		return true;
	}
	// Every kept tile with a body whose cap meets the cap: a tile holds the bodies that may reach it, so its range is
	// at least the range of the bodies over the cap.
	const double MinDot = FMath::Cos(FMath::Min(FMath::Max(AngularRadius, 0.0) + TileCapAngle, UE_DOUBLE_PI));
	FScopeLock Scope(&Lock);
	for (const TPair<FIntPoint, FTileEntry>& Entry : Tiles)
	{
		const FTile& Tile = *Entry.Value.Tile;
		if (Tile.Bodies.Num() == 0 || FVector::DotProduct(Tile.Centre, Direction) < MinDot)
		{
			continue;
		}
		OutMinLevel = bOutAny ? FMath::Min(OutMinLevel, Tile.MinLevel) : Tile.MinLevel;
		OutMaxLevel = bOutAny ? FMath::Max(OutMaxLevel, Tile.MaxLevel) : Tile.MaxLevel;
		bOutAny = true;
	}
	return true;
}

bool FVoxelHydrologyNetwork::HoldsWaterBodies(const FBox2D& Bounds) const
{
	return Holds(FVoxelConditioningRegion::OfBox(Bounds));
}

bool FVoxelHydrologyNetwork::HoldsWaterBodiesOnSphere(const FVector& Direction, double AngularRadius) const
{
	return Holds(FVoxelConditioningRegion::OfCap(Direction, AngularRadius));
}

void FVoxelHydrologyNetwork::GatherConditioningLines(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelConditioningLineRef>& OutLines) const
{
	const_cast<FVoxelHydrologyNetwork*>(this)->Gather(FVoxelConditioningRegion::OfBox(Region), Need, nullptr, &OutLines);
}

void FVoxelHydrologyNetwork::GatherConditioningLinesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need,
	TArray<FVoxelConditioningLineRef>& OutLines) const
{
	const_cast<FVoxelHydrologyNetwork*>(this)->Gather(FVoxelConditioningRegion::OfCap(Direction, AngularRadius), Need, nullptr, &OutLines);
}

int32 FVoxelHydrologyNetwork::WarmUp(TConstArrayView<FVector2D> Centres, double Radius, bool bWait)
{
	TArray<FVector, TInlineAllocator<4>> Places;
	for (const FVector2D& Centre : Centres)
	{
		Places.Add(FVector(Centre.X, Centre.Y, 0.0));
	}
	return WarmUpAround(Places, Radius, bWait);
}

int32 FVoxelHydrologyNetwork::WarmUpAround(TConstArrayView<FVector> Centres, double Radius, bool bWait)
{
	if (Radius <= 0.0 || Centres.Num() == 0)
	{
		return 0;
	}
	const double StartTime = FPlatformTime::Seconds();
	// The centres whose discs are to be gone through. A warm-up that does not wait skips a viewer that has moved less
	// than a quarter of a tile since a disc at least this wide was gone through: its tiles are kept or queued already.
	TArray<FVector, TInlineAllocator<4>> Asked;
	{
		FScopeLock Scope(&Lock);
		for (const FVector& Centre : Centres)
		{
			bool bGoneThrough = false;
			for (const FWarmedDisc& Disc : WarmedDiscs)
			{
				bGoneThrough |= !bWait && Disc.Epoch == TrimEpoch && Disc.Radius >= Radius && Grid.GetDistance(Disc.Centre, Centre) < 0.25 * TileSize;
			}
			if (!bGoneThrough)
			{
				Asked.Add(Centre);
			}
		}
	}
	if (Asked.Num() == 0)
	{
		return 0;
	}
	if (!bWait && Ground.bRadial)
	{
		// A planet's disc is found by a flood over its tiles (thousands of them round a viewer of a large planet): the
		// worker goes through it, before the tiles it has queued. The disc is whole when the worker has nothing left.
		FScopeLock Scope(&Lock);
		if (bShutdown)
		{
			return 0;
		}
		for (const FVector& Centre : Asked)
		{
			if (WarmedDiscs.Num() >= 8)
			{
				WarmedDiscs.RemoveAt(0);
			}
			WarmedDiscs.Add(FWarmedDisc{ Centre, Radius, TrimEpoch, false });
			PendingDiscs.Add(FWarmedDisc{ Centre, Radius, TrimEpoch, false });
		}
		if (!bWorkerRunning)
		{
			bWorkerRunning = true;
			WorkerTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Self = AsShared()]() { Self->RunWorker(); }, UE::Tasks::ETaskPriority::BackgroundNormal);
		}
		const double Ms = (FPlatformTime::Seconds() - StartTime) * 1000.0;
		++Stats.WarmUpAsks;
		Stats.WarmUpMs += Ms;
		Stats.WorstWarmUpMs = FMath::Max(Stats.WorstWarmUpMs, Ms);
		return 0;
	}
	// The tiles within the distance of a centre, with how far each lies: found without the lock (on a planet by a flood).
	TArray<TPair<double, FIntPoint>> Near;
	{
		TSet<FIntPoint> Seen;
		for (const FVector& Centre : Asked)
		{
			VisitTiles(GetRegionAround(Centre, Radius), TNumericLimits<int32>::Max(), [&](const FIntPoint& Key)
			{
				const double Distance = DistanceToTile(Key, Centre);
				bool bAlready = false;
				if (Distance <= Radius)
				{
					Seen.Add(Key, &bAlready);
					if (!bAlready)
					{
						Near.Emplace(Distance, Key);
					}
				}
				return true;
			});
		}
	}
	// The ones that are not kept, the nearest first.
	TArray<TPair<double, FIntPoint>> Wanted;
	{
		FScopeLock Scope(&Lock);
		for (const TPair<double, FIntPoint>& Entry : Near)
		{
			if (FTileEntry* Kept = Tiles.Find(Entry.Value))
			{
				Kept->LastUsed = ++UseCounter; // a tile near a viewer is not the one to drop
				continue;
			}
			Wanted.Add(Entry);
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
			// The discs gone through: whole once the worker has emptied its queue (at once, when nothing waits).
			const bool bIdle = Queue.Num() == 0 && !bWorkerRunning;
			for (const FVector& Centre : Asked)
			{
				if (WarmedDiscs.Num() >= 8)
				{
					WarmedDiscs.RemoveAt(0);
				}
				WarmedDiscs.Add(FWarmedDisc{ Centre, Radius, TrimEpoch, bIdle });
			}
			const double Ms = (FPlatformTime::Seconds() - StartTime) * 1000.0;
			++Stats.WarmUpAsks;
			Stats.WarmUpMs += Ms;
			Stats.WorstWarmUpMs = FMath::Max(Stats.WorstWarmUpMs, Ms);
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
	{
		// Every tile of these discs is kept now: a region inside one is answered at once (Holds).
		FScopeLock Scope(&Lock);
		for (const FVector& Centre : Asked)
		{
			if (HeldDiscs.Num() >= 8)
			{
				HeldDiscs.RemoveAt(0);
			}
			HeldDiscs.Add(FWarmedDisc{ Centre, Radius, TrimEpoch, true });
		}
	}
	return Missing.Num();
}

bool FVoxelHydrologyNetwork::RequestRegion(const FVoxelConditioningRegion& Region, const FVector& Place, int32 MaxTiles)
{
	if (MaxTiles <= 0 || !IsRegionOfWorld(Region))
	{
		return false;
	}
	FScopeLock Scope(&Lock);
	if (bShutdown)
	{
		return false;
	}
	PendingRegions.Add(FRequestedRegion{ Region, Place, MaxTiles });
	if (!bWorkerRunning)
	{
		bWorkerRunning = true;
		WorkerTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Self = AsShared()]() { Self->RunWorker(); }, UE::Tasks::ETaskPriority::BackgroundNormal);
	}
	return true;
}

void FVoxelHydrologyNetwork::GetLoad(int32& OutKept, int32& OutQueued, int32& OutLimit) const
{
	FScopeLock Scope(&Lock);
	OutKept = Tiles.Num();
	OutQueued = Queue.Num() + PendingDiscs.Num() + PendingRegions.Num();
	OutLimit = VoxelHydrologyNetworkPrivate::MaxTiles;
}

void FVoxelHydrologyNetwork::TakeAnnouncements(TArray<FVoxelConditioningRegion>& OutRegions)
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

void FVoxelHydrologyNetwork::Survey(const FVoxelConditioningRegion& Region, FVoxelHydrologyRegion& Out)
{
	FVoxelHydrology::BuildRegion(Settings, Ground, Region, this, Out);
}

FVoxelHydrologyStats FVoxelHydrologyNetwork::GetStats() const
{
	FScopeLock Scope(&Lock);
	FVoxelHydrologyStats Out = Stats;
	Out.Tiles = Tiles.Num();
	Out.Queued = Queue.Num() + PendingDiscs.Num() + PendingRegions.Num();
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
	PendingDiscs.Reset();
	PendingRegions.Reset();
}
