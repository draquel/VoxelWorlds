// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSurfacePathfinder.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
