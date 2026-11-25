// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSurfacePathfinder.h"
#include "VoxelWorldModeTypes.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

#define VOXEL_NAV_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VoxelNavTest
{
	FVoxelSurfaceNavParams Params()
	{
		FVoxelSurfaceNavParams P;
		P.CellSize = 100.f;
		P.MaxStepRise = 100.f;
		P.MaxNodes = 20000;
		P.MaxSearchRadius = 100000.f;
		return P;
	}

	bool PathTouches(const TArray<FVector>& Points, const FVector& From, TFunctionRef<bool(const FVector&)> Pred)
	{
		// Sample each leg every 25 units.
		FVector Prev = From;
		for (const FVector& P : Points)
		{
			const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector::Dist2D(Prev, P) / 25.f));
			for (int32 i = 0; i <= Steps; ++i)
			{
				if (Pred(FMath::Lerp(Prev, P, static_cast<float>(i) / Steps)))
				{
					return true;
				}
			}
			Prev = P;
		}
		return false;
	}
}

// ---------------------------------------------------------------------------
// Flat ground: a straight route with a handful of waypoints that ends at the goal.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavFlatTest, "VoxelWorlds.Navigation.Flat", VOXEL_NAV_TEST_FLAGS)
bool FVoxelNavFlatTest::RunTest(const FString& Parameters)
{
	FVoxelSurfacePathfinder Nav(VoxelNavTest::Params(), [](double, double) { return 0.f; });
	TArray<FVector> Points;
	FVoxelSurfacePathStats Stats;
	TestTrue(TEXT("Path found"), Nav.FindPath(FVector(50, 50, 0), FVector(3050, 1050, 0), Points, &Stats));
	TestTrue(TEXT("Ends at the goal cell"), Points.Num() > 0 && FVector::Dist2D(Points.Last(), FVector(3050, 1050, 0)) < 1.f);
	TestTrue(TEXT("Simplified to few points"), Points.Num() <= 3);
	TestFalse(TEXT("Not partial"), Stats.bPartial);
	TestTrue(TEXT("Expanded a modest number of nodes"), Stats.NodesExpanded < 200);
	TestTrue(TEXT("Zero height reported"), FMath::IsNearlyZero(Points.Last().Z));

	// Start == goal cell.
	TestTrue(TEXT("Same cell"), Nav.FindPath(FVector(10, 10, 0), FVector(20, 20, 0), Points));
	TestEqual(TEXT("One point"), Points.Num(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// A cliff band blocks the direct line; the route detours around its end and never crosses it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavCliffTest, "VoxelWorlds.Navigation.CliffDetour", VOXEL_NAV_TEST_FLAGS)
bool FVoxelNavCliffTest::RunTest(const FString& Parameters)
{
	// Wall: x in [1000, 1300), y in (-inf, 1500) is 1000 high; everything else flat.
	auto Height = [](double X, double Y) { return (X >= 1000.0 && X < 1300.0 && Y < 1500.0) ? 1000.f : 0.f; };
	FVoxelSurfacePathfinder Nav(VoxelNavTest::Params(), Height);
	TArray<FVector> Points;
	FVoxelSurfacePathStats Stats;
	const FVector From(50, 50, 0), To(2550, 50, 0);
	TestTrue(TEXT("Path found"), Nav.FindPath(From, To, Points, &Stats));
	TestFalse(TEXT("Complete"), Stats.bPartial);
	TestFalse(TEXT("Never on the wall"), VoxelNavTest::PathTouches(Points, From, [](const FVector& P) { return P.X >= 1000.f && P.X < 1300.f && P.Y < 1500.f; }));
	TestTrue(TEXT("Goes around the end"), VoxelNavTest::PathTouches(Points, From, [](const FVector& P) { return P.Y >= 1500.f; }));
	TestTrue(TEXT("Ends at the goal"), FVector::Dist2D(Points.Last(), To) < 1.f);

	// Short cliff = step: a 90-unit step is walkable, a 110-unit one is not.
	auto Step = [](double X, double) { return X >= 500.0 ? 90.f : 0.f; };
	FVoxelSurfacePathfinder StepNav(VoxelNavTest::Params(), Step);
	TestTrue(TEXT("Small step ok"), StepNav.FindPath(FVector(50, 50, 0), FVector(950, 50, 0), Points, &Stats) && !Stats.bPartial);
	auto Tall = [](double X, double) { return X >= 500.0 ? 110.f : 0.f; };
	FVoxelSurfaceNavParams Strict = VoxelNavTest::Params();
	Strict.bAllowPartial = false;
	FVoxelSurfacePathfinder TallNav(Strict, Tall);
	TestFalse(TEXT("Tall step is a wall"), TallNav.FindPath(FVector(50, 50, 0), FVector(950, 50, 0), Points, &Stats));
	return true;
}

// ---------------------------------------------------------------------------
// Water: cells below the wade depth are not walkable; a goal in a lake snaps to its shore or fails.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavWaterTest, "VoxelWorlds.Navigation.Water", VOXEL_NAV_TEST_FLAGS)
bool FVoxelNavWaterTest::RunTest(const FString& Parameters)
{
	// Lake: y >= 1000 is 500 below the water surface (0); a 40-deep ford at x in [2000, 2200).
	auto Height = [](double X, double Y)
	{
		if (Y < 1000.0) { return 0.f; }
		return (X >= 2000.0 && X < 2200.0) ? -40.f : -500.f;
	};
	FVoxelSurfaceNavParams P = VoxelNavTest::Params();
	P.bHasWater = true;
	P.WaterLevel = 0.f;
	P.MaxWadeDepth = 60.f;
	P.GoalSnapCells = 0;
	P.bAllowPartial = false;
	FVoxelSurfacePathfinder Nav(P, Height);
	TArray<FVector> Points;
	FVoxelSurfacePathStats Stats;
	TestFalse(TEXT("Deep water goal fails"), Nav.FindPath(FVector(50, 50, 0), FVector(50, 2050, 0), Points, &Stats));
	TestTrue(TEXT("Ford is walkable"), Nav.FindPath(FVector(50, 50, 0), FVector(2050, 2050, 0), Points, &Stats) && !Stats.bPartial);
	TestFalse(TEXT("Never in deep water"), VoxelNavTest::PathTouches(Points, FVector(50, 50, 0), [&](const FVector& Pt) { return Height(Pt.X, Pt.Y) < -60.f; }));

	// With goal snapping the lake goal lands on the shore.
	P.GoalSnapCells = 3;
	FVoxelSurfacePathfinder Snap(P, Height);
	TestTrue(TEXT("Snapped to the shore"), Snap.FindPath(FVector(50, 50, 0), FVector(50, 1150, 0), Points, &Stats) && Stats.bGoalSnapped);
	TestTrue(TEXT("Shore point is dry"), Points.Last().Y < 1000.f);
	return true;
}

// ---------------------------------------------------------------------------
// Budget + blocks: a tiny node budget yields a partial route toward the goal; blocked cells are avoided.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavBudgetBlockTest, "VoxelWorlds.Navigation.BudgetAndBlocks", VOXEL_NAV_TEST_FLAGS)
bool FVoxelNavBudgetBlockTest::RunTest(const FString& Parameters)
{
	FVoxelSurfaceNavParams P = VoxelNavTest::Params();
	P.MaxNodes = 16;
	FVoxelSurfacePathfinder Budget(P, [](double, double) { return 0.f; });
	TArray<FVector> Points;
	FVoxelSurfacePathStats Stats;
	TestTrue(TEXT("Partial route returned"), Budget.FindPath(FVector(50, 50, 0), FVector(9050, 50, 0), Points, &Stats));
	TestTrue(TEXT("Marked partial"), Stats.bPartial);
	TestTrue(TEXT("Progress toward the goal"), Points.Last().X > 500.f && Points.Last().X < 9000.f);
	P.bAllowPartial = false;
	FVoxelSurfacePathfinder Strict(P, [](double, double) { return 0.f; });
	TestFalse(TEXT("Strict budget fails"), Strict.FindPath(FVector(50, 50, 0), FVector(9050, 50, 0), Points, &Stats));

	// Blocked column x cells [5, 7] for y cells < 10: the route goes around the top.
	FVoxelSurfacePathfinder Blocked(VoxelNavTest::Params(), [](double, double) { return 0.f; },
		[](const FIntPoint& Cell) { return Cell.X >= 5 && Cell.X <= 7 && Cell.Y < 10; });
	const FVector From(50, 50, 0), To(1250, 50, 0);
	TestTrue(TEXT("Path around the block"), Blocked.FindPath(From, To, Points, &Stats) && !Stats.bPartial);
	TestFalse(TEXT("Never inside the block"), VoxelNavTest::PathTouches(Points, From, [](const FVector& Pt)
	{
		const int32 CX = FMath::FloorToInt(Pt.X / 100.f), CY = FMath::FloorToInt(Pt.Y / 100.f);
		return CX >= 5 && CX <= 7 && CY < 10;
	}));
	return true;
}

// ---------------------------------------------------------------------------
// World bounds (world modes review WM-I2): the subsystem blocks every cell outside the world mode's bounds, water on
// or off. With water off an island's floor beyond R + falloff is flat, dry ground the walker must not take: a goal out
// there fails (one just past the edge snaps back onto the island), and no route leaves the footprint.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavWorldBoundsTest, "VoxelWorlds.Navigation.WorldBounds", VOXEL_NAV_TEST_FLAGS)
bool FVoxelNavWorldBoundsTest::RunTest(const FString& Parameters)
{
	FVoxelWorldBounds Island;
	Island.Shape = EVoxelWorldBoundsShape::Circle;
	Island.Center = FVector(0.0, 0.0, 0.0);
	Island.OuterRadius = 2000.0f;
	Island.HalfExtent = FVector2D(2000.0, 2000.0);

	// Water off: the island and the floor beyond it are the same flat ground.
	auto Flat = [](double, double) { return 0.f; };
	auto Outside = [&Island](const FIntPoint& Cell)
	{
		return !Island.ContainsXY((Cell.X + 0.5) * 100.0, (Cell.Y + 0.5) * 100.0);
	};
	FVoxelSurfaceNavParams P = VoxelNavTest::Params();
	P.GoalSnapCells = 0;
	P.bAllowPartial = false;
	TArray<FVector> Points;
	FVoxelSurfacePathStats Stats;

	FVoxelSurfacePathfinder Strict(P, Flat, Outside);
	TestFalse(TEXT("A goal on the floor beyond the edge fails"), Strict.FindPath(FVector(50, 50, 0), FVector(3050, 50, 0), Points, &Stats));
	TestTrue(TEXT("A goal across the island succeeds"), Strict.FindPath(FVector(-1450, 50, 0), FVector(1450, 50, 0), Points, &Stats) && !Stats.bPartial);
	TestFalse(TEXT("Never past the edge"), VoxelNavTest::PathTouches(Points, FVector(-1450, 50, 0),
		[&Island](const FVector& Pt) { return !Island.ContainsXY(Pt.X, Pt.Y); }));

	// A goal just past the edge snaps back onto the island, like a lake goal onto its shore.
	P.GoalSnapCells = 3;
	FVoxelSurfacePathfinder Snap(P, Flat, Outside);
	TestTrue(TEXT("A goal just past the edge snaps inside"), Snap.FindPath(FVector(50, 50, 0), FVector(2150, 50, 0), Points, &Stats) && Stats.bGoalSnapped);
	TestTrue(TEXT("The snapped goal is inside the world"), Points.Num() > 0 && Island.ContainsXY(Points.Last().X, Points.Last().Y));
	return true;
}

// ---------------------------------------------------------------------------
// A goal out of reach (G73): on a pillar three steps high the search stops once the check from the goal's side finds the
// pillar top enclosed, where it used to spend its whole budget; a goal behind a long wall is not a pocket and is reached.
// Cells the sampler has no height for (NaN: the subsystem's per-search sampling cap) are left out of the search.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavPocketTest, "VoxelWorlds.Navigation.PocketAndSampleCap", VOXEL_NAV_TEST_FLAGS)
bool FVoxelNavPocketTest::RunTest(const FString& Parameters)
{
	// A 3 x 3 pillar 300 high at cells x [20, 22], y [0, 2] on flat ground.
	auto Pillar = [](double X, double Y)
	{
		const int32 CX = FMath::FloorToInt(X / 100.0), CY = FMath::FloorToInt(Y / 100.0);
		return CX >= 20 && CX <= 22 && CY >= 0 && CY <= 2 ? 300.f : 0.f;
	};
	FVoxelSurfaceNavParams P = VoxelNavTest::Params();
	P.MaxSearchRadius = 6000.f;
	TArray<FVector> Points;
	FVoxelSurfacePathStats Stats;
	const FVector From(50, 150, 0), OnPillar(2150, 150, 0);

	FVoxelSurfacePathfinder Nav(P, Pillar);
	TestTrue(TEXT("Pillar: a partial route"), Nav.FindPath(From, OnPillar, Points, &Stats) && Stats.bPartial);
	TestTrue(TEXT("Pillar: the goal is in a pocket"), Stats.bGoalInPocket);
	TestEqual(TEXT("Pillar: the search stops at the check"), Stats.NodesExpanded, P.PocketCheckNodes);
	TestTrue(TEXT("Pillar: the route ends at the pillar's foot"), Points.Num() > 0 && FVector::Dist2D(Points.Last(), OnPillar) < 300.f);

	FVoxelSurfaceNavParams NoCheck = P;
	NoCheck.PocketCheckNodes = 0;
	FVoxelSurfacePathfinder Exhaust(NoCheck, Pillar);
	FVoxelSurfacePathStats ExhaustStats;
	Exhaust.FindPath(From, OnPillar, Points, &ExhaustStats);
	TestTrue(TEXT("Pillar without the check: the search runs on (to its radius or budget)"), ExhaustStats.NodesExpanded > 4 * P.PocketCheckNodes);

	// A wall 100 cells long at x = 10 (y in [-50, 50]): the goal behind it is reached after more nodes than the check's.
	auto Wall = [](double X, double Y)
	{
		const int32 CX = FMath::FloorToInt(X / 100.0), CY = FMath::FloorToInt(Y / 100.0);
		return CX == 10 && CY >= -50 && CY <= 50 ? 1000.f : 0.f;
	};
	FVoxelSurfacePathfinder Around(P, Wall);
	TestTrue(TEXT("Wall: a complete route"), Around.FindPath(FVector(550, 50, 0), FVector(1550, 50, 0), Points, &Stats) && !Stats.bPartial);
	TestFalse(TEXT("Wall: the goal is not in a pocket"), Stats.bGoalInPocket);
	TestTrue(TEXT("Wall: the route took more nodes than the check's threshold"), Stats.NodesExpanded > P.PocketCheckNodes);

	// No height for a band of cells across the way (x = 15, y in [-3, 3]): the route goes around it, marked capped; a goal
	// with no height (and no snapping) gets no route.
	auto Band = [](double X, double Y)
	{
		const int32 CX = FMath::FloorToInt(X / 100.0), CY = FMath::FloorToInt(Y / 100.0);
		return CX == 15 && CY >= -3 && CY <= 3 ? std::numeric_limits<float>::quiet_NaN() : 0.f;
	};
	FVoxelSurfacePathfinder Cap(P, Band);
	const FVector CapFrom(550, 50, 0);
	TestTrue(TEXT("Cap: a complete route around the band"), Cap.FindPath(CapFrom, FVector(2550, 50, 0), Points, &Stats) && !Stats.bPartial);
	TestTrue(TEXT("Cap: marked"), Stats.bSamplesCapped);
	TestFalse(TEXT("Cap: never through the band"), VoxelNavTest::PathTouches(Points, CapFrom, [](const FVector& Pt)
	{
		const int32 CX = FMath::FloorToInt(Pt.X / 100.f), CY = FMath::FloorToInt(Pt.Y / 100.f);
		return CX == 15 && CY >= -3 && CY <= 3;
	}));
	FVoxelSurfaceNavParams NoSnap = P;
	NoSnap.GoalSnapCells = 0;
	FVoxelSurfacePathfinder CapNoSnap(NoSnap, Band);
	TestFalse(TEXT("Cap: a goal with no height gets no route"), CapNoSnap.FindPath(CapFrom, FVector(1550, 50, 0), Points, &Stats));
	return true;
}

// ---------------------------------------------------------------------------
// A cost query (the hydrology plan's H5: a trail crosses a river reach at a cost): a strip of cells that cost more to
// enter is gone round when the way round is cheaper and crossed by the fewest cells when it is not; the string-pulling
// never straightens a route through it; with no query a search is what it was.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavCellCostTest, "VoxelWorlds.Navigation.CellCost", VOXEL_NAV_TEST_FLAGS)
bool FVoxelNavCellCostTest::RunTest(const FString& Parameters)
{
	// Flat ground; a strip two cells wide (x cells 10 and 11) from far below up to y cell 14: it ends at y = 1500.
	auto Flat = [](double, double) { return 0.f; };
	auto InStrip = [](const FIntPoint& Cell) { return Cell.X >= 10 && Cell.X <= 11 && Cell.Y < 15; };
	auto PointInStrip = [](const FVector& P) { return P.X >= 1000.0 && P.X < 1200.0 && P.Y < 1500.0; };
	const FVoxelSurfaceNavParams P = VoxelNavTest::Params();
	const FVector From(50, 50, 0), To(2550, 50, 0);
	TArray<FVector> Points;
	FVoxelSurfacePathStats Stats;

	// Dear: 2,000 a cell, 4,000 to cross, against a way round the end some 1,700 longer.
	int32 Asked = 0;
	FVoxelSurfacePathfinder Dear(P, Flat, nullptr, [&](const FIntPoint& Cell) { ++Asked; return InStrip(Cell) ? 2000.f : 0.f; });
	TestTrue(TEXT("Dear: a complete route"), Dear.FindPath(From, To, Points, &Stats) && !Stats.bPartial);
	TestTrue(TEXT("Dear: the query was asked"), Asked > 0);
	TestFalse(TEXT("Dear: never through the strip"), VoxelNavTest::PathTouches(Points, From, PointInStrip));
	TestTrue(TEXT("Dear: round the strip's end"), VoxelNavTest::PathTouches(Points, From, [](const FVector& Pt) { return Pt.Y >= 1500.0; }));
	TestTrue(TEXT("Dear: ends at the goal"), Points.Num() > 0 && FVector::Dist2D(Points.Last(), To) < 1.f);

	// Cheap: 300 a cell, 600 to cross: the route crosses, by one cell a column, and enters the strip once.
	FVoxelSurfacePathfinder Cheap(P, Flat, nullptr, [&](const FIntPoint& Cell) { return InStrip(Cell) ? 300.f : 0.f; });
	TestTrue(TEXT("Cheap: a complete route"), Cheap.FindPath(From, To, Points, &Stats) && !Stats.bPartial);
	int32 InStripPoints = 0, Entries = 0;
	bool bWasIn = false, bBackBefore = false;
	for (const FVector& Point : Points)
	{
		const bool bIn = PointInStrip(Point);
		InStripPoints += bIn ? 1 : 0;
		Entries += (bIn && !bWasIn) ? 1 : 0;
		// Past the strip a route never comes back to its near side.
		bBackBefore |= Entries > 0 && !bIn && Point.X < 1000.0;
		bWasIn = bIn;
	}
	TestEqual(TEXT("Cheap: the strip is crossed by its two cells, each a waypoint the string-pulling kept"), InStripPoints, 2);
	TestEqual(TEXT("Cheap: entered once"), Entries, 1);
	TestFalse(TEXT("Cheap: no way back across"), bBackBefore);
	TestTrue(TEXT("Cheap: ends at the goal"), Points.Num() > 0 && FVector::Dist2D(Points.Last(), To) < 1.f);
	TestFalse(TEXT("Cheap: a straight run through the strip is not taken"), Cheap.IsStraightRunWalkable(FIntPoint(5, 0), FIntPoint(20, 0)));
	TestTrue(TEXT("Cheap: a straight run beside it is"), Cheap.IsStraightRunWalkable(FIntPoint(5, 20), FIntPoint(20, 20)));

	// A query that answers nothing, and none at all: the same route, point for point.
	TArray<FVector> Free, None;
	FVoxelSurfacePathfinder NoCost(P, Flat, nullptr, [](const FIntPoint&) { return 0.f; });
	FVoxelSurfacePathfinder Plain(P, Flat);
	TestTrue(TEXT("Free: found"), NoCost.FindPath(From, To, Free) && Plain.FindPath(From, To, None));
	TestTrue(TEXT("Free: the route of a search with no query"), Free == None);
	TestTrue(TEXT("Plain: a straight run anywhere"), Plain.IsStraightRunWalkable(FIntPoint(5, 0), FIntPoint(20, 0)));
	return true;
}

// ---------------------------------------------------------------------------
// A search in steps (G281): stopped after every node, or every few, and taken up again, it gives the route and the stats of
// the same search run at once, on open ground, round a wall, to a goal in a pocket, to a goal past the node budget (a
// partial route), across cells with no height, and with a cost query; a trivial search ends at its beginning; a new
// search drops one in progress.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavStepsTest, "VoxelWorlds.Navigation.SearchInSteps", VOXEL_NAV_TEST_FLAGS)
bool FVoxelNavStepsTest::RunTest(const FString& Parameters)
{
	// Hills, a wall with a gap, a pillar and a band with no height, all on one field.
	auto Field = [](double X, double Y)
	{
		const int32 CX = FMath::FloorToInt(X / 100.0), CY = FMath::FloorToInt(Y / 100.0);
		if (CX == 10 && CY >= -40 && CY <= 40 && CY != 25)
		{
			return 1000.f; // a wall with a gap at y = 25
		}
		if (CX >= 30 && CX <= 32 && CY >= 0 && CY <= 2)
		{
			return 300.f; // a pillar
		}
		if (CX == 18 && CY >= -5 && CY <= 5)
		{
			return std::numeric_limits<float>::quiet_NaN(); // no height
		}
		return static_cast<float>(40.0 * FMath::Sin(X * 0.002) * FMath::Cos(Y * 0.0017));
	};
	auto Cost = [](const FIntPoint& Cell) { return Cell.X == 22 && Cell.Y > -10 && Cell.Y < 10 ? 500.f : 0.f; };
	FVoxelSurfaceNavParams P = VoxelNavTest::Params();
	P.MaxSearchRadius = 8000.f;
	FVoxelSurfaceNavParams Small = P;
	Small.MaxNodes = 300;

	struct FCase
	{
		const TCHAR* Name;
		FVector From;
		FVector To;
		bool bSmallBudget;
		bool bCost;
	};
	const FCase Cases[] = {
		{ TEXT("open ground"), FVector(-2950, -1950, 0), FVector(-450, 1550, 0), false, false },
		{ TEXT("round the wall"), FVector(550, 50, 0), FVector(1550, 50, 0), false, false },
		{ TEXT("a goal in a pocket"), FVector(1550, 150, 0), FVector(3150, 150, 0), false, false },
		{ TEXT("past the node budget"), FVector(550, 50, 0), FVector(4550, -2050, 0), true, false },
		{ TEXT("across cells with no height"), FVector(1250, 50, 0), FVector(2550, 50, 0), false, false },
		{ TEXT("with a cost query"), FVector(1250, 50, 0), FVector(2950, -50, 0), false, true },
		{ TEXT("in one cell"), FVector(1210, 20, 0), FVector(1260, 70, 0), false, false },
		{ TEXT("from a wall's top"), FVector(1050, 0, 0), FVector(1550, 50, 0), false, false },
	};
	for (const FCase& Case : Cases)
	{
		const FVoxelSurfaceNavParams& CaseParams = Case.bSmallBudget ? Small : P;
		FVoxelSurfacePathfinder::FCostQuery CaseCost = Case.bCost ? FVoxelSurfacePathfinder::FCostQuery(Cost) : nullptr;
		FVoxelSurfacePathfinder AtOnce(CaseParams, Field, nullptr, CaseCost);
		TArray<FVector> Expected;
		FVoxelSurfacePathStats ExpectedStats;
		const bool bExpected = AtOnce.FindPath(Case.From, Case.To, Expected, &ExpectedStats);

		for (const int32 Every : { 1, 7 })
		{
			FVoxelSurfacePathfinder InSteps(CaseParams, Field, nullptr, CaseCost);
			InSteps.BeginSearch(Case.From, Case.To);
			int32 Calls = 0;
			int32 Nodes = 0;
			bool bEnded = InSteps.IsSearchDone();
			while (!bEnded && Calls < 100000)
			{
				++Calls;
				bEnded = InSteps.AdvanceSearch([&Nodes, Every] { return ++Nodes % Every == 0; });
			}
			TArray<FVector> Got;
			FVoxelSurfacePathStats GotStats;
			const bool bGot = InSteps.GetSearchResult(Got, &GotStats);
			const FString Name = FString::Printf(TEXT("%s, every %d node(s)"), Case.Name, Every);
			TestEqual(*(Name + TEXT(": found as at once")), bGot, bExpected);
			TestTrue(*(Name + TEXT(": the same route, point for point")), Got == Expected);
			TestEqual(*(Name + TEXT(": the same nodes")), GotStats.NodesExpanded, ExpectedStats.NodesExpanded);
			TestEqual(*(Name + TEXT(": the same samples")), GotStats.HeightSamples, ExpectedStats.HeightSamples);
			TestEqual(*(Name + TEXT(": the same raw cells")), GotStats.RawCells, ExpectedStats.RawCells);
			TestEqual(*(Name + TEXT(": partial as at once")), GotStats.bPartial, ExpectedStats.bPartial);
			TestEqual(*(Name + TEXT(": pocket as at once")), GotStats.bGoalInPocket, ExpectedStats.bGoalInPocket);
			TestEqual(*(Name + TEXT(": capped as at once")), GotStats.bSamplesCapped, ExpectedStats.bSamplesCapped);
			TestEqual(*(Name + TEXT(": snapped as at once")), GotStats.bGoalSnapped, ExpectedStats.bGoalSnapped);
			if (Every == 1 && ExpectedStats.NodesExpanded > 2)
			{
				TestTrue(*(Name + TEXT(": it did stop between nodes")), Calls > 1);
			}
		}
	}

	// The cases above meant what they say.
	{
		FVoxelSurfacePathfinder Check(P, Field);
		TArray<FVector> Points;
		FVoxelSurfacePathStats Stats;
		Check.FindPath(FVector(1550, 150, 0), FVector(3150, 150, 0), Points, &Stats);
		TestTrue(TEXT("The pillar's goal is in a pocket"), Stats.bGoalInPocket);
		FVoxelSurfacePathfinder CheckSmall(Small, Field);
		CheckSmall.FindPath(FVector(550, 50, 0), FVector(4550, -2050, 0), Points, &Stats);
		TestTrue(TEXT("The far goal is past the small budget"), Stats.bPartial && Stats.NodesExpanded >= Small.MaxNodes);
		Check.FindPath(FVector(1250, 50, 0), FVector(2550, 50, 0), Points, &Stats);
		TestTrue(TEXT("The band has no height"), Stats.bSamplesCapped);
	}

	// A search not ended has no route; a new one drops it.
	FVoxelSurfacePathfinder Dropped(P, Field);
	Dropped.BeginSearch(FVector(-2950, -1950, 0), FVector(-450, 1550, 0));
	Dropped.AdvanceSearch([] { return true; });
	TArray<FVector> None;
	TestFalse(TEXT("Not ended: no route yet"), Dropped.IsSearchDone() || Dropped.GetSearchResult(None));
	TArray<FVector> Again, Fresh;
	Dropped.BeginSearch(FVector(550, 50, 0), FVector(1550, 50, 0));
	Dropped.AdvanceSearch([] { return false; });
	FVoxelSurfacePathfinder Clean(P, Field);
	TestTrue(TEXT("Begun again: the new search's route"), Dropped.GetSearchResult(Again) && Clean.FindPath(FVector(550, 50, 0), FVector(1550, 50, 0), Fresh)
		&& Again == Fresh);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
