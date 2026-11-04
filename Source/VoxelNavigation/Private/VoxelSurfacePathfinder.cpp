// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSurfacePathfinder.h"
#include "VoxelNavigation.h"
#include "Algo/Reverse.h"
#include "Misc/ScopeExit.h"

namespace
{
	/** The four orthogonal neighbours, then the four diagonals; diagonal k passes orthogonals GDiagonalSides[k]. */
	const FIntPoint GNeighbourOffsets[8] = {
		FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1),
		FIntPoint(1, 1), FIntPoint(1, -1), FIntPoint(-1, 1), FIntPoint(-1, -1)
	};
	const int32 GDiagonalSides[4][2] = { { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 } };

	/**
	 * The heuristic's weight for ordering the open list: a hair over 1, so of two nodes with the same cost estimate the
	 * one nearer the goal comes first (on open ground every node of the band between start and goal ties otherwise, and
	 * all of them were expanded; G73). A route is at most 0.1% longer than the cheapest.
	 */
	constexpr float GTieBreakWeight = 1.001f;

	struct FOpenNode
	{
		FIntPoint Cell;
		float F;
		bool operator<(const FOpenNode& Other) const { return F < Other.F; }
	};

	using FPagePool = TArray<TUniquePtr<uint8[]>>;

	/** Pages the grids of this thread have released, for the next search to reuse. */
	FPagePool& PagePool()
	{
		static thread_local FPagePool Pool;
		return Pool;
	}
}

// ---------------------------------------------------------------------------
// The search's cell grid
// ---------------------------------------------------------------------------

FVoxelSurfacePathfinder::FCellGrid::~FCellGrid()
{
	Reset();
}

FVoxelSurfacePathfinder::FCell& FVoxelSurfacePathfinder::FCellGrid::Get(const FIntPoint& Cell)
{
	// Arithmetic shifts floor negative cells too.
	const FIntPoint Key(Cell.X >> PageShift, Cell.Y >> PageShift);
	if (Key != LastKey)
	{
		FPage*& Page = Pages.FindOrAdd(Key);
		if (!Page)
		{
			FPagePool& Pool = PagePool();
			uint8* Memory = Pool.Num() > 0 ? Pool.Pop(EAllowShrinking::No).Release() : new uint8[sizeof(FPage)];
			Page = new (Memory) FPage();
		}
		LastKey = Key;
		LastPage = Page;
	}
	return LastPage->Cells[((Cell.Y & (PageSize - 1)) << PageShift) | (Cell.X & (PageSize - 1))];
}

void FVoxelSurfacePathfinder::FCellGrid::Reset()
{
	FPagePool& Pool = PagePool();
	for (TPair<FIntPoint, FPage*>& Pair : Pages)
	{
		static_assert(std::is_trivially_destructible_v<FPage>, "pages are returned to the pool as raw memory");
		Pool.Emplace(reinterpret_cast<uint8*>(Pair.Value));
	}
	Pages.Reset();
	LastKey = FIntPoint(TNumericLimits<int32>::Max(), 0);
	LastPage = nullptr;
}

// ---------------------------------------------------------------------------
// Pathfinder
// ---------------------------------------------------------------------------

FVoxelSurfacePathfinder::FVoxelSurfacePathfinder(const FVoxelSurfaceNavParams& InParams, FHeightSampler InSampler, FBlockedQuery InBlocked)
	: Params(InParams)
	, Sampler(MoveTemp(InSampler))
	, Blocked(MoveTemp(InBlocked))
{
	Params.CellSize = FMath::Max(Params.CellSize, 1.f);
}

void FVoxelSurfacePathfinder::ClearCache() const
{
	Cells.Reset();
}

FIntPoint FVoxelSurfacePathfinder::WorldToCell(const FVector& World) const
{
	return Params.CellOf(World.X, World.Y);
}

FVector FVoxelSurfacePathfinder::CellToWorld(const FIntPoint& Cell) const
{
	const FVector2D Centre = Params.CellCentre(Cell);
	return FVector(Centre.X, Centre.Y, HeightAt(Cell));
}

const FVoxelSurfacePathfinder::FCell& FVoxelSurfacePathfinder::Sampled(const FIntPoint& Cell) const
{
	FCell& Record = Cells.Get(Cell);
	if (!Record.bSampled)
	{
		// One sample and one walkability decision per cell per search: above water (minus wading) and not blocked.
		const FVector2D Centre = Params.CellCentre(Cell);
		const float Height = Sampler ? Sampler(Centre.X, Centre.Y) : 0.f;
		Record.bSampled = true;
		if (FMath::IsNaN(Height))
		{
			// No height for it in this search (the subsystem's sampling cap): left out of the search.
			Record.Height = 0.f;
			Record.bUnknown = true;
			Record.bWalkable = false;
			bSamplesCapped = true;
			return Record;
		}
		Record.Height = Height;
		Record.bWalkable = !(Blocked && Blocked(Cell)) && !Params.IsUnderwater(Centre.X, Centre.Y, Record.Height);
		++SampleCount;
	}
	return Record;
}

float FVoxelSurfacePathfinder::HeightAt(const FIntPoint& Cell) const
{
	return Sampled(Cell).Height;
}

bool FVoxelSurfacePathfinder::IsCellWalkable(const FIntPoint& Cell) const
{
	return Sampled(Cell).bWalkable;
}

bool FVoxelSurfacePathfinder::CanStepBetween(const FCell& From, const FCell& To, bool bDiagonal) const
{
	// Whole blocks (WM-C7) take one block whichever way a walker goes; the smooth rule scales the rise with the run.
	const float Limit = Params.MaxStepRise * (bDiagonal && !Params.bBlockSteps ? UE_SQRT_2 : 1.f);
	return From.bWalkable && To.bWalkable && FMath::Abs(From.Height - To.Height) <= Limit;
}

bool FVoxelSurfacePathfinder::IsGoalInPocket(const FIntPoint& Goal, const FIntPoint& Start) const
{
	// A flood from the goal over the cells a walker could step between in either direction by the rise alone (no corner
	// rules): it reaches every cell from which the goal can be reached, and maybe more, so when it runs out small without
	// the start the goal is out of reach. A cell the sampler had no height for makes the answer unknown: no pocket.
	TArray<FIntPoint, TInlineAllocator<256>> Queue;
	Queue.Add(Goal);
	Cells.Get(Goal).bInGoalPocket = true;
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const FIntPoint Cell = Queue[Head];
		const FCell& Here = Sampled(Cell);
		for (int32 k = 0; k < 8; ++k)
		{
			const FIntPoint Next = Cell + GNeighbourOffsets[k];
			const FCell& There = Sampled(Next);
			if (There.bUnknown)
			{
				return false;
			}
			if (There.bInGoalPocket || !CanStepBetween(Here, There, k >= 4))
			{
				continue;
			}
			if (Next == Start || Queue.Num() >= Params.PocketCells)
			{
				return false;
			}
			const_cast<FCell&>(There).bInGoalPocket = true;
			Queue.Add(Next);
		}
	}
	return true;
}

bool FVoxelSurfacePathfinder::CanStep(const FIntPoint& From, const FIntPoint& To) const
{
	const FCell& A = Sampled(From);
	const FCell& B = Sampled(To);
	const bool bDiagonal = From.X != To.X && From.Y != To.Y;
	if (!CanStepBetween(A, B, bDiagonal))
	{
		return false;
	}
	if (Params.bBlockSteps && bDiagonal)
	{
		// A diagonal passes the corner the two cells beside it share, so each of them must be walkable and within a step
		// of both ends.
		for (const FIntPoint& Side : { FIntPoint(To.X, From.Y), FIntPoint(From.X, To.Y) })
		{
			const FCell& S = Sampled(Side);
			if (!CanStepBetween(A, S, false) || !CanStepBetween(S, B, false))
			{
				return false;
			}
		}
	}
	return true;
}

float FVoxelSurfacePathfinder::Heuristic(const FIntPoint& A, const FIntPoint& B, float CellSize)
{
	// Octile distance: exact for 8-connected grids with √2 diagonals, so A* stays admissible on flat ground.
	const int32 DX = FMath::Abs(A.X - B.X);
	const int32 DY = FMath::Abs(A.Y - B.Y);
	return CellSize * (FMath::Max(DX, DY) + (UE_SQRT_2 - 1.f) * FMath::Min(DX, DY));
}

bool FVoxelSurfacePathfinder::SnapToWalkable(const FIntPoint& Cell, FIntPoint& OutCell) const
{
	if (IsCellWalkable(Cell))
	{
		OutCell = Cell;
		return true;
	}
	for (int32 R = 1; R <= Params.GoalSnapCells; ++R)
	{
		for (int32 DX = -R; DX <= R; ++DX)
		{
			for (int32 DY = -R; DY <= R; ++DY)
			{
				if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != R)
				{
					continue; // ring only
				}
				const FIntPoint Candidate(Cell.X + DX, Cell.Y + DY);
				if (IsCellWalkable(Candidate))
				{
					OutCell = Candidate;
					return true;
				}
			}
		}
	}
	return false;
}

bool FVoxelSurfacePathfinder::IsStraightRunWalkable(const FIntPoint& From, const FIntPoint& To) const
{
	// Supercover line: visit every cell the segment between the centres touches, checking each step.
	const int32 Steps = FMath::Max(FMath::Abs(To.X - From.X), FMath::Abs(To.Y - From.Y));
	if (Steps == 0)
	{
		return IsCellWalkable(From);
	}
	FIntPoint Prev = From;
	for (int32 i = 1; i <= Steps; ++i)
	{
		const float T = static_cast<float>(i) / Steps;
		const FIntPoint Cell(FMath::RoundToInt(From.X + (To.X - From.X) * T), FMath::RoundToInt(From.Y + (To.Y - From.Y) * T));
		if (Cell == Prev)
		{
			continue;
		}
		// A move that changes both axes passes a corner: both orthogonal neighbours must be steppable too,
		// otherwise a wall tip would be cut.
		if (Cell.X != Prev.X && Cell.Y != Prev.Y)
		{
			if (!CanStep(Prev, FIntPoint(Cell.X, Prev.Y)) || !CanStep(Prev, FIntPoint(Prev.X, Cell.Y)))
			{
				return false;
			}
		}
		if (!CanStep(Prev, Cell))
		{
			return false;
		}
		Prev = Cell;
	}
	return true;
}

void FVoxelSurfacePathfinder::SimplifyPath(TArray<FIntPoint>& Path) const
{
	if (Path.Num() < 3)
	{
		return;
	}
	TArray<FIntPoint> Kept;
	Kept.Add(Path[0]);
	int32 Anchor = 0;
	while (Anchor < Path.Num() - 1)
	{
		// Furthest cell reachable in a straight run from the anchor.
		int32 Best = Anchor + 1;
		for (int32 i = Path.Num() - 1; i > Anchor + 1; --i)
		{
			if (IsStraightRunWalkable(Path[Anchor], Path[i]))
			{
				Best = i;
				break;
			}
		}
		Kept.Add(Path[Best]);
		Anchor = Best;
	}
	Path = MoveTemp(Kept);
}

bool FVoxelSurfacePathfinder::FindPath(const FVector& From, const FVector& To, TArray<FVector>& OutPoints, FVoxelSurfacePathStats* OutStats) const
{
	OutPoints.Reset();
	Cells.Reset();
	SampleCount = 0;
	bSamplesCapped = false;
	FVoxelSurfacePathStats Stats;
	ON_SCOPE_EXIT
	{
		// Whichever way the search ends (the start or goal could be the cells left out).
		if (OutStats)
		{
			OutStats->bSamplesCapped = bSamplesCapped;
		}
	};

	FIntPoint Start, Goal;
	if (!SnapToWalkable(WorldToCell(From), Start))
	{
		if (OutStats) { *OutStats = Stats; }
		return false;
	}
	const FIntPoint RawGoal = WorldToCell(To);
	if (!SnapToWalkable(RawGoal, Goal))
	{
		Stats.HeightSamples = SampleCount;
		if (OutStats) { *OutStats = Stats; }
		return false;
	}
	Stats.bGoalSnapped = Goal != RawGoal;

	if (Start == Goal)
	{
		OutPoints.Add(CellToWorld(Goal));
		Stats.RawCells = 1;
		Stats.HeightSamples = SampleCount;
		if (OutStats) { *OutStats = Stats; }
		return true;
	}

	const float RadiusCells = Params.MaxSearchRadius / Params.CellSize;
	const float RadiusCellsSq = RadiusCells * RadiusCells;

	TArray<FOpenNode> Open; // binary heap by F
	FCell& StartRec = Cells.Get(Start);
	StartRec.G = 0.f;
	Open.HeapPush(FOpenNode{Start, GTieBreakWeight * Heuristic(Start, Goal, Params.CellSize)});

	FIntPoint Closest = Start;
	float ClosestH = Heuristic(Start, Goal, Params.CellSize);
	bool bReached = false;

	while (Open.Num() > 0)
	{
		FOpenNode Node;
		Open.HeapPop(Node, EAllowShrinking::No);
		FCell& Rec = Cells.Get(Node.Cell);
		if (Rec.bClosed)
		{
			continue; // stale heap entry
		}
		Rec.bClosed = true;
		++Stats.NodesExpanded;

		if (Node.Cell == Goal)
		{
			bReached = true;
			Closest = Goal;
			break;
		}
		if (Stats.NodesExpanded >= Params.MaxNodes)
		{
			break;
		}
		if (Stats.NodesExpanded == Params.PocketCheckNodes && IsGoalInPocket(Goal, Start))
		{
			// Out of reach: the rest of the budget would be spent on cells that cannot lead there.
			Stats.bGoalInPocket = true;
			break;
		}

		// The neighbours, each sampled once: the orthogonals first, since every diagonal needs the two beside it.
		const FCell* Around[8];
		bool bCanStep[8];
		for (int32 k = 0; k < 4; ++k)
		{
			Around[k] = &Sampled(Node.Cell + GNeighbourOffsets[k]);
			bCanStep[k] = CanStepBetween(Rec, *Around[k], false);
		}
		for (int32 k = 4; k < 8; ++k)
		{
			const int32 SideA = GDiagonalSides[k - 4][0];
			const int32 SideB = GDiagonalSides[k - 4][1];
			// Diagonals may not cut a corner: both orthogonal neighbours must be steppable, and on a block world the
			// diagonal's end must be within a step of each of them too.
			bCanStep[k] = false;
			Around[k] = nullptr;
			if (!bCanStep[SideA] || !bCanStep[SideB])
			{
				continue;
			}
			const FCell& Diagonal = Sampled(Node.Cell + GNeighbourOffsets[k]);
			Around[k] = &Diagonal;
			bCanStep[k] = CanStepBetween(Rec, Diagonal, true)
				&& (!Params.bBlockSteps || (CanStepBetween(*Around[SideA], Diagonal, false) && CanStepBetween(*Around[SideB], Diagonal, false)));
		}

		const float G = Rec.G;
		for (int32 k = 0; k < 8; ++k)
		{
			if (!bCanStep[k] || Around[k]->bClosed)
			{
				continue;
			}
			const FIntPoint Next = Node.Cell + GNeighbourOffsets[k];
			const float DX = static_cast<float>(Next.X - Start.X);
			const float DY = static_cast<float>(Next.Y - Start.Y);
			if (DX * DX + DY * DY > RadiusCellsSq)
			{
				continue;
			}
			// Cost = run x (1 + SlopeCostScale x rise / run).
			const float Run = Params.CellSize * (k >= 4 ? UE_SQRT_2 : 1.f);
			const float NewG = G + Run + Params.SlopeCostScale * FMath::Abs(Rec.Height - Around[k]->Height);
			FCell& NextRec = const_cast<FCell&>(*Around[k]);
			if (NewG < NextRec.G)
			{
				NextRec.G = NewG;
				NextRec.Parent = Node.Cell;
				NextRec.bHasParent = true;
				const float H = Heuristic(Next, Goal, Params.CellSize);
				Open.HeapPush(FOpenNode{Next, NewG + GTieBreakWeight * H});
				if (H < ClosestH)
				{
					ClosestH = H;
					Closest = Next;
				}
			}
		}
	}

	Stats.HeightSamples = SampleCount;
	if (!bReached)
	{
		Stats.bPartial = true;
		if (!Params.bAllowPartial || Closest == Start)
		{
			if (OutStats) { *OutStats = Stats; }
			return false;
		}
	}

	// Walk the parents back from the end cell.
	TArray<FIntPoint> Path;
	for (FIntPoint Cell = Closest;;)
	{
		Path.Add(Cell);
		const FCell& Rec = Cells.Get(Cell);
		if (!Rec.bHasParent)
		{
			break;
		}
		Cell = Rec.Parent;
	}
	Algo::Reverse(Path);
	Stats.RawCells = Path.Num();
	SimplifyPath(Path);

	// The start cell is where the walker already stands; emit the rest.
	for (int32 i = 1; i < Path.Num(); ++i)
	{
		OutPoints.Add(CellToWorld(Path[i]));
	}
	if (OutPoints.Num() == 0)
	{
		OutPoints.Add(CellToWorld(Closest));
	}
	if (OutStats) { *OutStats = Stats; }
	return true;
}
