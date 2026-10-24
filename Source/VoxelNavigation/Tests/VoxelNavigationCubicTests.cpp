// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSurfacePathfinder.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Block-world navigation (world modes review WM-C7, KC7, decision D29).
//
// A cubic world's surface is block tops, 75 uu a block, and a walker steps one block. The smooth rule (a rise of 100 uu
// between cells 150 uu apart, scaled by sqrt(2) on diagonals) both refused a one-block staircase (two blocks between cell
// centres two blocks apart) and let a two-block rise through on a diagonal (141). The block rule: one cell per block
// column on the voxel grid, a step of at most one block either way, no sqrt(2), and a diagonal needs both cells beside it
// within a step of each end. Heights here are whole blocks over a grid origin off zero, as the demo's WorldOrigin is.
// ---------------------------------------------------------------------------

namespace VoxelNavCubicTest
{
	constexpr float Block = 75.f;
	const FVector2D GridOrigin(12345.0, 54321.0);

	FVoxelSurfaceNavParams BlockParams()
	{
		FVoxelSurfaceNavParams P;
		P.CellSize = Block;
		P.GridOrigin = GridOrigin;
		P.MaxStepRise = Block + 1.f;
		P.bBlockSteps = true;
		P.MaxNodes = 20000;
		P.MaxSearchRadius = 100000.f;
		return P;
	}

	/** The block column (i, j) a world position lies in, on the grid the params use. */
	FIntPoint Column(double X, double Y)
	{
		return FIntPoint(FMath::FloorToInt((X - GridOrigin.X) / Block), FMath::FloorToInt((Y - GridOrigin.Y) / Block));
	}

	FVector At(int32 I, int32 J)
	{
		return FVector(GridOrigin.X + (I + 0.5) * Block, GridOrigin.Y + (J + 0.5) * Block, 0.0);
	}

	/** Every column a route's legs pass through, in order (sampled every 5 uu). */
	TArray<FIntPoint> ColumnsAlong(const FVector& From, const TArray<FVector>& Points)
	{
		TArray<FIntPoint> Columns;
		FVector Prev = From;
		for (const FVector& P : Points)
		{
			const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector::Dist2D(Prev, P) / 5.f));
			for (int32 i = 0; i <= Steps; ++i)
			{
				const FVector S = FMath::Lerp(Prev, P, static_cast<float>(i) / Steps);
				const FIntPoint C = Column(S.X, S.Y);
				if (Columns.Num() == 0 || Columns.Last() != C)
				{
					Columns.Add(C);
				}
			}
			Prev = P;
		}
		return Columns;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavCubicBlockStepsTest, "VoxelWorlds.Navigation.Cubic.BlockSteps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelNavCubicBlockStepsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelNavCubicTest;

	// 1. A one-block staircase two columns wide: column i (0..7) of rows 0 and 1 is i blocks high, fenced by a twelve-block
	// wall either side, so the only way up to the landing (seven blocks, from column 8 on) is the stairs.
	{
		auto Height = [](double X, double Y) -> float
		{
			const FIntPoint C = Column(X, Y);
			if ((C.Y == 0 || C.Y == 1) && C.X >= 0 && C.X < 8)
			{
				return C.X * Block;
			}
			if ((C.Y == -1 || C.Y == 2) && C.X >= 0 && C.X < 8)
			{
				return 12 * Block; // walls beside the stairs
			}
			return C.X >= 8 ? 7 * Block : 0.f; // the landing at the top is level with the last step
		};
		FVoxelSurfacePathfinder Nav(BlockParams(), Height);
		TArray<FVector> Points;
		FVoxelSurfacePathStats Stats;
		const FVector From = At(-3, 0), To = At(11, 0);
		TestTrue(TEXT("Stairs: a route up the one-block staircase"), Nav.FindPath(From, To, Points, &Stats));
		TestFalse(TEXT("Stairs: complete"), Stats.bPartial);
		TestTrue(TEXT("Stairs: ends on the landing"), Points.Num() > 0 && Column(Points.Last().X, Points.Last().Y) == FIntPoint(11, 0));
		TestEqual(TEXT("Stairs: the landing's height is seven blocks"), Points.Num() > 0 ? Points.Last().Z : -1.0, static_cast<double>(7 * Block));

		// The smooth rule at its own 150 uu cells samples every other step: two blocks (150) between neighbouring cells,
		// over its 100 uu rise, so it refuses the staircase a walker climbs (the KC7 mismatch).
		FVoxelSurfaceNavParams Smooth = BlockParams();
		Smooth.bBlockSteps = false;
		Smooth.CellSize = 150.f;
		Smooth.MaxStepRise = 100.f;
		FVoxelSurfacePathfinder SmoothNav(Smooth, Height);
		FVoxelSurfacePathStats SmoothStats;
		const bool bSmooth = SmoothNav.FindPath(From, To, Points, &SmoothStats);
		AddInfo(FString::Printf(TEXT("Smooth rule at 150 uu cells: %s"), bSmooth && !SmoothStats.bPartial ? TEXT("climbs") : TEXT("refuses the stairs")));
		TestTrue(TEXT("Smooth rule at 150 uu cells refuses the one-block staircase (the KC7 mismatch)"), !bSmooth || SmoothStats.bPartial);
	}

	// 2. A two-block wall across the map with a one-block gap at j = 5: the route goes through the gap and never climbs.
	{
		auto Height = [](double X, double Y) -> float
		{
			const FIntPoint C = Column(X, Y);
			if (C.X == 4)
			{
				return C.Y == 5 ? Block : 2 * Block;
			}
			return 0.f;
		};
		FVoxelSurfacePathfinder Nav(BlockParams(), Height);
		TArray<FVector> Points;
		FVoxelSurfacePathStats Stats;
		const FVector From = At(0, 0), To = At(8, 0);
		TestTrue(TEXT("Wall: a route"), Nav.FindPath(From, To, Points, &Stats));
		TestFalse(TEXT("Wall: complete"), Stats.bPartial);
		const TArray<FIntPoint> Columns = ColumnsAlong(From, Points);
		bool bCrossedWall = false;
		for (const FIntPoint& C : Columns)
		{
			bCrossedWall |= C.X == 4 && C.Y != 5;
		}
		TestFalse(TEXT("Wall: never on the two-block wall"), bCrossedWall);
		TestTrue(TEXT("Wall: through the gap"), Columns.Contains(FIntPoint(4, 5)));

		// No gap: no complete route (a two-block rise is a wall, diagonals included).
		auto Closed = [](double X, double Y) -> float { return Column(X, Y).X == 4 ? 2 * Block : 0.f; };
		FVoxelSurfaceNavParams Small = BlockParams();
		Small.MaxSearchRadius = 1500.f;
		FVoxelSurfacePathfinder ClosedNav(Small, Closed);
		FVoxelSurfacePathStats ClosedStats;
		const bool bFound = ClosedNav.FindPath(From, To, Points, &ClosedStats);
		TestTrue(TEXT("Closed wall: no complete route"), !bFound || ClosedStats.bPartial);
		TestFalse(TEXT("Closed wall: a straight step onto two blocks is refused"), ClosedNav.CanStep(FIntPoint(3, 0), FIntPoint(4, 0)));
		TestFalse(TEXT("Closed wall: a diagonal step onto two blocks is refused (no sqrt(2))"), ClosedNav.CanStep(FIntPoint(3, 0), FIntPoint(4, 1)));
	}

	// 3. Corners: a pillar two blocks high at (1, 0). The diagonal (0, 0) -> (1, 1) passes the pillar's corner, so it is
	// refused while both its sides are not within a step; the diagonal (0, 1) -> (1, 2) is fine. One-block diagonals pass.
	{
		auto Height = [](double X, double Y) -> float
		{
			const FIntPoint C = Column(X, Y);
			if (C == FIntPoint(1, 0))
			{
				return 2 * Block;
			}
			if (C == FIntPoint(5, 5))
			{
				return Block;
			}
			return 0.f;
		};
		FVoxelSurfacePathfinder Nav(BlockParams(), Height);
		TestFalse(TEXT("Corner: diagonal past a two-block pillar refused"), Nav.CanStep(FIntPoint(0, 0), FIntPoint(1, 1)));
		TestTrue(TEXT("Corner: diagonal clear of the pillar allowed"), Nav.CanStep(FIntPoint(0, 1), FIntPoint(1, 2)));
		TestTrue(TEXT("Corner: diagonal up one block allowed"), Nav.CanStep(FIntPoint(4, 4), FIntPoint(5, 5)));
		TestTrue(TEXT("Corner: straight down two blocks refused, as up"), !Nav.CanStep(FIntPoint(1, 0), FIntPoint(1, 1)));
	}

	// 4. The grid: cell centres are the block columns' centres on the voxel grid, whatever the origin.
	{
		FVoxelSurfacePathfinder Nav(BlockParams(), [](double, double) { return 0.f; });
		const FVector Centre = Nav.CellToWorld(FIntPoint(3, -2));
		TestEqual(TEXT("Grid: cell centre X"), Centre.X, GridOrigin.X + 3.5 * Block);
		TestEqual(TEXT("Grid: cell centre Y"), Centre.Y, GridOrigin.Y - 1.5 * Block);
		TestEqual(TEXT("Grid: the cell of a column's corner"), Nav.WorldToCell(FVector(GridOrigin.X + 3 * Block, GridOrigin.Y - 2 * Block, 0.0)), FIntPoint(3, -2));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
