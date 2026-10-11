// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSurfacePathfinder.h"
#include "VoxelNavigation.h"
#include "Algo/Reverse.h"

namespace
{
	const FIntPoint GNeighbourOffsets[8] = {
		FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1),
		FIntPoint(1, 1), FIntPoint(1, -1), FIntPoint(-1, 1), FIntPoint(-1, -1)
	};

	struct FOpenNode
	{
		FIntPoint Cell;
		float F;
		bool operator<(const FOpenNode& Other) const { return F < Other.F; }
	};

	struct FNodeRecord
	{
		float G = TNumericLimits<float>::Max();
		FIntPoint Parent = FIntPoint(TNumericLimits<int32>::Min(), 0);
		bool bClosed = false;
		bool HasParent() const { return Parent.X != TNumericLimits<int32>::Min(); }
	};
}

FVoxelSurfacePathfinder::FVoxelSurfacePathfinder(const FVoxelSurfaceNavParams& InParams, FHeightSampler InSampler, FBlockedQuery InBlocked)
	: Params(InParams)
	, Sampler(MoveTemp(InSampler))
	, Blocked(MoveTemp(InBlocked))
{
	Params.CellSize = FMath::Max(Params.CellSize, 1.f);
}

FIntPoint FVoxelSurfacePathfinder::WorldToCell(const FVector& World) const
{
	return FIntPoint(FMath::FloorToInt(World.X / Params.CellSize), FMath::FloorToInt(World.Y / Params.CellSize));
}

FVector FVoxelSurfacePathfinder::CellToWorld(const FIntPoint& Cell) const
{
	const double X = (Cell.X + 0.5) * Params.CellSize;
	const double Y = (Cell.Y + 0.5) * Params.CellSize;
	return FVector(X, Y, HeightAt(Cell));
}

float FVoxelSurfacePathfinder::HeightAt(const FIntPoint& Cell) const
{
	if (const float* Cached = HeightCache.Find(Cell))
	{
		return *Cached;
	}
	const float H = Sampler ? Sampler((Cell.X + 0.5) * Params.CellSize, (Cell.Y + 0.5) * Params.CellSize) : 0.f;
	HeightCache.Add(Cell, H);
	++SampleCount;
	return H;
}

bool FVoxelSurfacePathfinder::IsCellWalkable(const FIntPoint& Cell) const
{
	if (Blocked && Blocked(Cell))
	{
		return false;
	}
	if (Params.bHasWater && HeightAt(Cell) < Params.WaterLevel - Params.MaxWadeDepth)
	{
		return false;
	}
	return true;
}

bool FVoxelSurfacePathfinder::CanStep(const FIntPoint& From, const FIntPoint& To) const
{
	if (!IsCellWalkable(From) || !IsCellWalkable(To))
	{
		return false;
	}
	const bool bDiagonal = From.X != To.X && From.Y != To.Y;
	const float Limit = Params.MaxStepRise * (bDiagonal ? UE_SQRT_2 : 1.f);
	return FMath::Abs(HeightAt(From) - HeightAt(To)) <= Limit;
}

float FVoxelSurfacePathfinder::StepCost(const FIntPoint& From, const FIntPoint& To) const
{
	const bool bDiagonal = From.X != To.X && From.Y != To.Y;
	const float Run = Params.CellSize * (bDiagonal ? UE_SQRT_2 : 1.f);
	const float Rise = FMath::Abs(HeightAt(From) - HeightAt(To));
	return Run * (1.f + Params.SlopeCostScale * (Rise / Run));
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

void FVoxelSurfacePathfinder::SimplifyPath(TArray<FIntPoint>& Cells) const
{
	if (Cells.Num() < 3)
	{
		return;
	}
	TArray<FIntPoint> Kept;
	Kept.Add(Cells[0]);
	int32 Anchor = 0;
	while (Anchor < Cells.Num() - 1)
	{
		// Furthest cell reachable in a straight run from the anchor.
		int32 Best = Anchor + 1;
		for (int32 i = Cells.Num() - 1; i > Anchor + 1; --i)
		{
			if (IsStraightRunWalkable(Cells[Anchor], Cells[i]))
			{
				Best = i;
				break;
			}
		}
		Kept.Add(Cells[Best]);
		Anchor = Best;
	}
	Cells = MoveTemp(Kept);
}

bool FVoxelSurfacePathfinder::FindPath(const FVector& From, const FVector& To, TArray<FVector>& OutPoints, FVoxelSurfacePathStats* OutStats) const
{
	OutPoints.Reset();
	HeightCache.Reset();
	SampleCount = 0;
	FVoxelSurfacePathStats Stats;

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

	TMap<FIntPoint, FNodeRecord> Records;
	TArray<FOpenNode> Open; // binary heap by F
	FNodeRecord& StartRec = Records.Add(Start);
	StartRec.G = 0.f;
	Open.HeapPush(FOpenNode{Start, Heuristic(Start, Goal, Params.CellSize)});

	FIntPoint Closest = Start;
	float ClosestH = Heuristic(Start, Goal, Params.CellSize);
	bool bReached = false;

	while (Open.Num() > 0)
	{
		FOpenNode Node;
		Open.HeapPop(Node, EAllowShrinking::No);
		FNodeRecord& Rec = Records.FindChecked(Node.Cell);
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

		const float G = Rec.G;
		for (const FIntPoint& Offset : GNeighbourOffsets)
		{
			const FIntPoint Next = Node.Cell + Offset;
			const float DX = static_cast<float>(Next.X - Start.X);
			const float DY = static_cast<float>(Next.Y - Start.Y);
			if (DX * DX + DY * DY > RadiusCellsSq)
			{
				continue;
			}
			FNodeRecord* NextRec = Records.Find(Next);
			if (NextRec && NextRec->bClosed)
			{
				continue;
			}
			if (!CanStep(Node.Cell, Next))
			{
				continue;
			}
			// Diagonals may not cut a corner: both orthogonal neighbours must be steppable.
			if (Offset.X != 0 && Offset.Y != 0)
			{
				if (!CanStep(Node.Cell, FIntPoint(Next.X, Node.Cell.Y)) || !CanStep(Node.Cell, FIntPoint(Node.Cell.X, Next.Y)))
				{
					continue;
				}
			}
			const float NewG = G + StepCost(Node.Cell, Next);
			if (!NextRec)
			{
				NextRec = &Records.Add(Next);
			}
			if (NewG < NextRec->G)
			{
				NextRec->G = NewG;
				NextRec->Parent = Node.Cell;
				const float H = Heuristic(Next, Goal, Params.CellSize);
				Open.HeapPush(FOpenNode{Next, NewG + H});
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
	TArray<FIntPoint> Cells;
	for (FIntPoint Cell = Closest;;)
	{
		Cells.Add(Cell);
		const FNodeRecord* Rec = Records.Find(Cell);
		if (!Rec || !Rec->HasParent())
		{
			break;
		}
		Cell = Rec->Parent;
	}
	Algo::Reverse(Cells);
	Stats.RawCells = Cells.Num();
	SimplifyPath(Cells);

	// The start cell is where the walker already stands; emit the rest.
	for (int32 i = 1; i < Cells.Num(); ++i)
	{
		OutPoints.Add(CellToWorld(Cells[i]));
	}
	if (OutPoints.Num() == 0)
	{
		OutPoints.Add(CellToWorld(Closest));
	}
	if (OutStats) { *OutStats = Stats; }
	return true;
}
