// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelConditioningLine.h"
#include "VoxelConditioningLineShape.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// From a polyline to a line's vertices and targets (Documentation/TERRAIN_LINES_PLAN.md, R1 and D134;
// FVoxelConditioningLineShape): the corners rounded to the bend bound's turn, and a levelled profile held to a grade
// and flatter through a bend. What a road between POIs is made with; the hand-placed line's component calls MakeProfile
// (L5) and rounds its corners with FitCornerArcs (G225, the next test).
//
// - The sampling: even steps, the two ends kept.
// - The bound's two forms agree with the plan's road numbers (16 degrees a vertex at 10 %, 11 at 15 %, 6.7 at 25 %),
//   and the grade a turn leaves is the inverse of the turn a grade leaves.
// - RoundCorners: a right-angle corner comes out within the bound, its ends where they were, no point further from the
//   polyline than the move it reports, the straight runs away from the corner untouched; a straight line and a gentle
//   curve are not spread at all; a line that comes straight back on itself is reported as outside the bound.
// - MakeProfile: on a rounded corner across a slope the made line has no bend warning where the ground's own heights
//   have one; no segment is steeper than the grade; given end heights are met, and heights too far apart are reported;
//   with no end heights and no bend it is the hand-placed line's FollowTerrain arithmetic, value for value.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineShapeTest,
	"VoxelWorlds.Generation.ConditioningLine.Shape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineShapeTest::RunTest(const FString& Parameters)
{
	using FShape = FVoxelConditioningLineShape;
	// The road class on the demo: voxels of 75, a core 6 voxels wide, a falloff of 4, a vertex every 2 voxels.
	const double Voxel = 75.0;
	const float HalfWidth = 225.0f;
	const float Falloff = 300.0f;
	const double Spacing = 150.0;
	const double Reach = HalfWidth + Falloff;

	auto DistanceToPolyline = [](const FVector& Point, TConstArrayView<FVector> Line)
	{
		double Best = TNumericLimits<double>::Max();
		for (int32 Index = 1; Index < Line.Num(); ++Index)
		{
			Best = FMath::Min(Best, FVector::Dist(Point, FMath::ClosestPointOnSegment(Point, Line[Index - 1], Line[Index])));
		}
		return Best;
	};
	auto MakeLine = [&](TConstArrayView<FVector> Points, TConstArrayView<double> Targets)
	{
		FVoxelConditioningLineParams Params;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			Params.Vertices.Add(FVoxelConditioningLineVertex(Points[Index], static_cast<float>(Targets[Index]), HalfWidth));
		}
		Params.FalloffWidth = Falloff;
		return FVoxelConditioningLine::Make(Params);
	};
	auto BendWarnings = [&](const FVoxelConditioningLineRef& Line)
	{
		FVoxelConditioningLineLimits Limits;
		Limits.VoxelSize = static_cast<float>(Voxel);
		int32 Count = 0;
		for (const FString& Warning : Line->GetWarnings(Limits))
		{
			Count += (Warning.Contains(TEXT("bend bound")) || Warning.Contains(TEXT("turn so sharply"))) ? 1 : 0;
		}
		return Count;
	};

	// ---- The sampling.
	const TArray<FVector> Corner = { FVector(0.0, 0.0, 0.0), FVector(6000.0, 0.0, 0.0), FVector(6000.0, 5000.0, 0.0) };
	{
		TArray<FVector> Even;
		FShape::ResampleEvenly(Corner, Spacing, Even);
		TestEqual(TEXT("The polyline's length"), FShape::GetLength(Corner), 11000.0, 1.0e-9);
		TestEqual(TEXT("11,000 uu in steps of 150 or a little less: 74 steps"), Even.Num(), 75);
		TestTrue(TEXT("The ends are kept"), Even[0] == Corner[0] && Even.Last() == Corner.Last());
		double Shortest = TNumericLimits<double>::Max(), Longest = 0.0;
		for (int32 Index = 1; Index < Even.Num(); ++Index)
		{
			// Along the polyline: across its corner a step's chord is shorter.
			const double Step = FVector::Dist(Even[Index - 1], Even[Index]);
			Shortest = FMath::Min(Shortest, Step);
			Longest = FMath::Max(Longest, Step);
		}
		TestTrue(TEXT("The steps are even, a spacing or a little less"), Longest <= Spacing + 1.0e-6 && Longest > 148.0 && Shortest > 100.0);
		TArray<FVector> One;
		FShape::ResampleEvenly(TConstArrayView<FVector>(Corner.GetData(), 1), Spacing, One);
		TestEqual(TEXT("One point stays one point"), One.Num(), 1);
	}

	// ---- The bound's two forms.
	{
		const double ForReach = FMath::RadiansToDegrees(FShape::GetMaxTurnForReach(Spacing, Reach));
		TestTrue(TEXT("The reach's bound for a road sampled every 2 voxels is about 16 degrees"), ForReach > 16.0 && ForReach < 16.6);
		const double At10 = FMath::RadiansToDegrees(FShape::GetMaxTurnForGrade(0.10, HalfWidth, Falloff, Voxel));
		const double At15 = FMath::RadiansToDegrees(FShape::GetMaxTurnForGrade(0.15, HalfWidth, Falloff, Voxel));
		const double At25 = FMath::RadiansToDegrees(FShape::GetMaxTurnForGrade(0.25, HalfWidth, Falloff, Voxel));
		AddInfo(FString::Printf(TEXT("The largest turn a vertex: %.1f degrees by the reach; by the grade %.1f at 10 %%, %.1f at 15 %%, %.1f at 25 %%"), ForReach, At10, At15, At25));
		TestTrue(TEXT("The plan's road numbers: about 16 degrees at 10 %, 11 at 15 %, 6.7 at 25 %"),
			At10 > 16.0 && At10 < 17.5 && At15 > 10.5 && At15 < 11.7 && At25 > 6.4 && At25 < 7.1);
		for (const double Grade : { 0.05, 0.2, 0.35 })
		{
			const double Turn = FShape::GetMaxTurnForGrade(Grade, HalfWidth, Falloff, Voxel);
			TestEqual(TEXT("The grade a turn leaves is the inverse of the turn a grade leaves"), FShape::GetMaxGradeForTurn(Turn, HalfWidth, Falloff, Voxel), Grade, 1.0e-9);
		}
		TestEqual(TEXT("A level line may turn by any angle"), FShape::GetMaxTurnForGrade(0.0, HalfWidth, Falloff, Voxel), UE_DOUBLE_PI);
		TestEqual(TEXT("A line that moves no height too"), FShape::GetMaxTurnForGrade(0.3, HalfWidth, Falloff, Voxel, 0.0f), UE_DOUBLE_PI);
		TestTrue(TEXT("A straight run bounds no grade"), FShape::GetMaxGradeForTurn(0.0, HalfWidth, Falloff, Voxel) > 1.0e30);
	}

	// ---- RoundCorners.
	const double MaxTurn = FShape::GetMaxTurnForReach(Spacing, Reach);
	TArray<FVector> Rounded;
	{
		TArray<FVector> Even;
		FShape::ResampleEvenly(Corner, Spacing, Even);
		TestTrue(TEXT("The corner as drawn breaks the bound (non-vacuous)"), FShape::GetWorstTurn(Even) > MaxTurn);
		const FShape::FRounding Result = FShape::RoundCorners(Corner, Spacing, MaxTurn, 900.0, Rounded);
		AddInfo(FString::Printf(TEXT("A right angle rounded: %d tries, the corners spread over %.0f uu each way, the worst turn %.1f degrees (the bound %.1f), no point moved more than %.0f uu, %d vertices"),
			Result.Tries, Result.CornerDistance, FMath::RadiansToDegrees(Result.WorstTurn), FMath::RadiansToDegrees(MaxTurn), Result.FarthestMove, Rounded.Num()));
		TestTrue(TEXT("A right angle is rounded to within the bound"), Result.bWithinBound && Result.Tries >= 1
			&& Result.WorstTurn <= FShape::BoundMargin * MaxTurn && FMath::IsNearlyEqual(Result.WorstTurn, FShape::GetWorstTurn(Rounded), 1.0e-12));
		TestTrue(TEXT("The two ends stay where they are"), Rounded[0] == Corner[0] && Rounded.Last() == Corner.Last());
		TestTrue(TEXT("A point moves by no more than twice the distance the corners were spread over"), Result.FarthestMove > 100.0 && Result.FarthestMove <= 2.0 * Result.CornerDistance);
		double Farthest = 0.0;
		int32 OffTheRuns = 0;
		for (const FVector& Point : Rounded)
		{
			const double Off = DistanceToPolyline(Point, Corner);
			Farthest = FMath::Max(Farthest, Off);
			// Further from the corner than the spread reaches, twice over: on the straight runs as drawn.
			if (FVector::Dist(Point, Corner[1]) > 2.0 * Result.CornerDistance + Spacing)
			{
				OffTheRuns += Off > 1.0e-6 ? 1 : 0;
			}
		}
		TestTrue(TEXT("No vertex is further from the polyline than the move reported"), Farthest <= Result.FarthestMove + 1.0e-6);
		TestEqual(TEXT("The straight runs away from the corner are untouched"), OffTheRuns, 0);
		for (int32 Index = 1; Index < Rounded.Num(); ++Index)
		{
			const double Step = FVector::Dist(Rounded[Index - 1], Rounded[Index]);
			if (Step > Spacing + 1.0e-6 || Step < 0.9 * Spacing)
			{
				AddError(FString::Printf(TEXT("The rounded line's step %d is %.2f uu"), Index, Step));
				break;
			}
		}

		// A straight line and a gentle curve are within the bound as sampled: not spread.
		TArray<FVector> Out;
		const TArray<FVector> Straight = { FVector(100.0, -50.0, 0.0), FVector(4100.0, 2950.0, 0.0) };
		const FShape::FRounding StraightResult = FShape::RoundCorners(Straight, Spacing, MaxTurn, 900.0, Out);
		TestTrue(TEXT("A straight line is not spread"), StraightResult.Tries == 0 && StraightResult.bWithinBound && StraightResult.FarthestMove == 0.0
			&& Out.Num() == 35 && DistanceToPolyline(Out[17], Straight) < 1.0e-6);
		TArray<FVector> Arc;
		for (int32 Index = 0; Index <= 40; ++Index)
		{
			const double Angle = Index * (Spacing / 2000.0);
			Arc.Add(FVector(2000.0 * FMath::Sin(Angle), 2000.0 * (1.0 - FMath::Cos(Angle)), 0.0));
		}
		const FShape::FRounding ArcResult = FShape::RoundCorners(Arc, Spacing, MaxTurn, 900.0, Out);
		TestTrue(TEXT("A curve within the bound is not spread: it comes back as it was sampled"), ArcResult.Tries == 0 && ArcResult.bWithinBound && Out.Num() == Arc.Num()
			&& FVector::Dist(Out[20], Arc[20]) < 0.5);

		// A line that comes straight back on itself: no spreading rounds it, and the result says so.
		const TArray<FVector> Hairpin = { FVector(0.0, 0.0, 0.0), FVector(5000.0, 0.0, 0.0), FVector(0.0, 40.0, 0.0) };
		const FShape::FRounding HairpinResult = FShape::RoundCorners(Hairpin, Spacing, MaxTurn, 900.0, Out);
		TestTrue(TEXT("A line that comes back on itself is reported as outside the bound"), !HairpinResult.bWithinBound && HairpinResult.Tries == 3
			&& HairpinResult.WorstTurn > MaxTurn);
	}

	// ---- MakeProfile.
	{
		// The rounded corner across a slope of 30 % (the ground rises along x + y).
		auto GroundAt = [](const FVector& Point) { return 1000.0 + 0.3 * (Point.X + Point.Y) / UE_DOUBLE_SQRT_2; };
		TArray<double> Ground;
		for (const FVector& Point : Rounded)
		{
			Ground.Add(GroundAt(Point));
		}
		TestTrue(TEXT("A line at the ground's own heights breaks the bend bound on this slope (non-vacuous)"), BendWarnings(MakeLine(Rounded, Ground)) > 0);

		FShape::FProfileParams Params;
		Params.SmoothingDistance = 1200.0;
		Params.MaxGrade = 0.35;
		Params.HalfWidth = HalfWidth;
		Params.FalloffWidth = Falloff;
		Params.VoxelSize = Voxel;
		TArray<double> Targets;
		const FShape::FProfile Made = FShape::MakeProfile(Rounded, Ground, Params, Targets);
		TestEqual(TEXT("A target a vertex"), Targets.Num(), Rounded.Num());
		TestEqual(TEXT("The made profile has no bend warning: flatter through the bend"), BendWarnings(MakeLine(Rounded, Targets)), 0);
		double WorstStep = 0.0, Steepest = 0.0, MostMoved = 0.0;
		const double Peak = FVoxelConditioningLine::PeakOfDistanceTimesWeight(HalfWidth, Falloff);
		for (int32 Index = 0; Index + 1 < Rounded.Num(); ++Index)
		{
			const double Grade = FMath::Abs(Targets[Index + 1] - Targets[Index]) / FVector::Dist(Rounded[Index], Rounded[Index + 1]);
			Steepest = FMath::Max(Steepest, Grade);
			MostMoved = FMath::Max(MostMoved, FMath::Abs(Targets[Index] - Ground[Index]));
			if (Index > 0)
			{
				const double Before = FMath::Abs(Targets[Index] - Targets[Index - 1]) / FVector::Dist(Rounded[Index - 1], Rounded[Index]);
				WorstStep = FMath::Max(WorstStep, 2.0 * FMath::Max(Grade, Before) * FMath::Tan(0.5 * FShape::GetTurnAt(Rounded, Index)) * Peak);
			}
		}
		AddInfo(FString::Printf(TEXT("The profile through the corner on a 30 %% slope: the steepest segment %.1f %%, the largest step across a bisector %.2f uu (a tenth of a voxel is %.1f), the ground moved by up to %.0f uu"),
			100.0 * Steepest, WorstStep, 0.1 * Voxel, MostMoved));
		TestTrue(TEXT("No step across a bisector over a tenth of a voxel"), WorstStep <= 0.1 * Voxel);
		TestTrue(TEXT("No segment steeper than the grade, and the steepest is the one reported"), Steepest <= Params.MaxGrade + 1.0e-9 && FMath::IsNearlyEqual(Steepest, Made.WorstGrade, 1.0e-9));
		TestTrue(TEXT("With no end heights the line starts on the ground, and no end is missed"), Made.bEndsMet && Made.EndMiss == 0.0
			&& FMath::IsNearlyEqual(Targets[0], Ground[0], 1.0e-9));

		// End heights: met when the grades let the line reach them, reported when they do not.
		Params.StartHeight = Ground[0] + 120.0;
		Params.EndHeight = Ground.Last() - 200.0;
		const FShape::FProfile Pinned = FShape::MakeProfile(Rounded, Ground, Params, Targets);
		TestTrue(TEXT("Given end heights are met"), Pinned.bEndsMet && FMath::IsNearlyEqual(Targets[0], Ground[0] + 120.0, 1.0e-9)
			&& FMath::IsNearlyEqual(Targets.Last(), Ground.Last() - 200.0, 1.0e-9) && Pinned.WorstGrade <= Params.MaxGrade + 1.0e-9);
		TestEqual(TEXT("... with no bend warning either"), BendWarnings(MakeLine(Rounded, Targets)), 0);
		Params.EndHeight = Ground.Last() + 9000.0;
		const FShape::FProfile TooFar = FShape::MakeProfile(Rounded, Ground, Params, Targets);
		TestTrue(TEXT("End heights further apart than the line can climb are reported, with the miss"), !TooFar.bEndsMet && TooFar.EndMiss > 1000.0
			&& FMath::IsNearlyEqual(Targets.Last(), Ground.Last() + 9000.0, 1.0e-9) && TooFar.WorstGrade <= Params.MaxGrade + 1.0e-9);

		// A straight line over rough ground that climbs faster than the grade, no end heights: the hand-placed line's
		// FollowTerrain, value for value (the ground averaged over the distance each way, the window narrowing toward the
		// ends, then held to the grade from both ends, which leaves the end where the grade brought it), so the component
		// can call this in its place.
		TArray<FVector> Straight;
		TArray<double> Rough;
		FRandomStream Random(11);
		for (int32 Index = 0; Index <= 60; ++Index)
		{
			Straight.Add(FVector(150.0 * Index, 40.0 * Index, 0.0));
			Rough.Add(500.0 + 0.4 * 150.0 * Index + Random.FRandRange(-90.0f, 90.0f));
		}
		FShape::FProfileParams Follow;
		Follow.SmoothingDistance = 1200.0;
		Follow.MaxGrade = 0.15;
		Follow.HalfWidth = HalfWidth;
		Follow.FalloffWidth = Falloff;
		Follow.VoxelSize = Voxel;
		FShape::MakeProfile(Straight, Rough, Follow, Targets);
		const double Step = FShape::GetLength(Straight) / 60.0;
		const int32 Window = FMath::Max(0, FMath::RoundToInt32(Follow.SmoothingDistance / Step));
		TArray<double> Expected;
		Expected.SetNum(Rough.Num());
		for (int32 Index = 0; Index < Rough.Num(); ++Index)
		{
			const int32 Half = FMath::Min3(Window, Index, Rough.Num() - 1 - Index);
			double Sum = 0.0;
			for (int32 Other = Index - Half; Other <= Index + Half; ++Other)
			{
				Sum += Rough[Other];
			}
			Expected[Index] = Sum / (2 * Half + 1);
		}
		const double MaxStep = Follow.MaxGrade * Step;
		for (int32 Index = 1; Index < Expected.Num(); ++Index)
		{
			Expected[Index] = FMath::Clamp(Expected[Index], Expected[Index - 1] - MaxStep, Expected[Index - 1] + MaxStep);
		}
		for (int32 Index = Expected.Num() - 2; Index >= 0; --Index)
		{
			Expected[Index] = FMath::Clamp(Expected[Index], Expected[Index + 1] - MaxStep, Expected[Index + 1] + MaxStep);
		}
		double Off = 0.0;
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			Off = FMath::Max(Off, FMath::Abs(Expected[Index] - Targets[Index]));
		}
		TestTrue(TEXT("On a straight line with no end heights the profile is the hand-placed line's FollowTerrain"), Off < 1.0e-6);
		TestTrue(TEXT("... which ends under the ground it could not keep up with (non-vacuous: an end pinned to the ground would differ)"),
			Targets.Last() < Rough.Last() - 500.0);

		// No grade asked for: the averaged ground, unclamped on a straight line.
		Follow.MaxGrade = 0.0;
		const FShape::FProfile Free = FShape::MakeProfile(Straight, Rough, Follow, Targets);
		TestTrue(TEXT("With no grade a straight line's profile is the averaged ground, steeper than any limit here"), Free.WorstGrade > 0.3 && Free.bEndsMet);
	}
	return true;
}

// ---------------------------------------------------------------------------
// FitCornerArcs (G225): a corner rounded with an arc of the radius the bound asks for, which turns evenly, where
// SpreadCorners' average turns most at a corner's middle.
//
// - Corners of 60, 90, 120 and 150 degrees on long legs come out within the bound (every vertex, and the engine's own
//   check on a level line), the corner moved by the arc's own distance, radius x (1 / cos(turn / 2) - 1): a reach at 120
//   degrees; the ends where they were, a vertex a spacing apart, the straight runs away from the corner untouched, the
//   places along the polyline in order. R1's spreading over twice the reach leaves the 120 degree corner outside.
// - Near an end: a right angle on legs of five vertices, a right angle 400 uu from an end, a corner of 120 degrees on
//   legs shorter than its arc on both sides: within the bound, the ends kept.
// - Left as they were: a polyline within the bound (point for point), a hairpin of 170 degrees, an S too tight for two
//   arcs; two corners far enough apart get an arc each.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineCornerArcsTest,
	"VoxelWorlds.Generation.ConditioningLine.CornerArcs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineCornerArcsTest::RunTest(const FString& Parameters)
{
	using FShape = FVoxelConditioningLineShape;
	// The road class on the demo: voxels of 75, a core 6 voxels wide, a falloff of 4 (a reach of 525), a vertex every 2 voxels.
	const double Voxel = 75.0;
	const float HalfWidth = 225.0f;
	const float Falloff = 300.0f;
	const double Spacing = 150.0;
	const double Reach = HalfWidth + Falloff;
	const double Radius = FShape::GetCornerRadius(Spacing, FShape::GetMaxTurnForReach(Spacing, Reach));
	TestTrue(TEXT("The radius is about the reach over the margin"), Radius > Reach / FShape::BoundMargin && Radius < 1.01 * Reach / FShape::BoundMargin);

	// The worst vertex's turn over what the bound's first part allows it, on its shorter segment.
	auto BendRatio = [&](TConstArrayView<FVector> Points)
	{
		double Worst = 0.0;
		for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
		{
			const double Chord = FMath::Min(FVector::Dist(Points[Index - 1], Points[Index]), FVector::Dist(Points[Index], Points[Index + 1]));
			Worst = FMath::Max(Worst, FShape::GetTurnAt(Points, Index) / FShape::GetMaxTurnForReach(Chord, Reach));
		}
		return Worst;
	};
	// The engine's check on the polyline as a level line: its bend warnings.
	auto BendWarnings = [&](TConstArrayView<FVector> Points)
	{
		FVoxelConditioningLineParams Params;
		for (const FVector& Point : Points)
		{
			Params.Vertices.Add(FVoxelConditioningLineVertex(Point, 0.0f, HalfWidth));
		}
		Params.FalloffWidth = Falloff;
		FVoxelConditioningLineLimits Limits;
		Limits.VoxelSize = static_cast<float>(Voxel);
		int32 Count = 0;
		for (const FString& Warning : FVoxelConditioningLine::Make(Params)->GetWarnings(Limits))
		{
			Count += (Warning.Contains(TEXT("bend bound")) || Warning.Contains(TEXT("turn so sharply"))) ? 1 : 0;
		}
		return Count;
	};
	auto DistanceToPolyline = [](const FVector& Point, TConstArrayView<FVector> Line)
	{
		double Best = TNumericLimits<double>::Max();
		for (int32 Index = 1; Index < Line.Num(); ++Index)
		{
			Best = FMath::Min(Best, FVector::Dist(Point, FMath::ClosestPointOnSegment(Point, Line[Index - 1], Line[Index])));
		}
		return Best;
	};
	// Two straight legs that meet at the origin, the second turning Degrees to the left, sampled evenly.
	auto Corner = [&](double Degrees, double LegIn, double LegOut)
	{
		const double Turn = FMath::DegreesToRadians(Degrees);
		const TArray<FVector> Drawn = { FVector(-LegIn, 0.0, 0.0), FVector::ZeroVector, FVector(FMath::Cos(Turn), FMath::Sin(Turn), 0.0) * LegOut };
		TArray<FVector> Even;
		FShape::ResampleEvenly(Drawn, Spacing, Even);
		return Even;
	};
	// What every made polyline keeps: its ends, a vertex a spacing apart at most, its places in order and within the polyline given.
	auto CheckMade = [&](const TCHAR* Name, TConstArrayView<FVector> Drawn, TConstArrayView<FVector> Made, TConstArrayView<double> Places)
	{
		TestTrue(FString::Printf(TEXT("%s: the ends stay where they are"), Name), Made.Num() > 2 && Made[0] == Drawn[0] && Made.Last() == Drawn.Last());
		double Longest = 0.0;
		bool bInOrder = Places.Num() == Made.Num() && Places[0] == 0.0 && Places.Last() == Drawn.Num() - 1;
		for (int32 Index = 1; Index < Made.Num(); ++Index)
		{
			Longest = FMath::Max(Longest, FVector::Dist(Made[Index - 1], Made[Index]));
			bInOrder = bInOrder && Index < Places.Num() && Places[Index] >= Places[Index - 1];
		}
		TestTrue(FString::Printf(TEXT("%s: a vertex a spacing apart at most"), Name), Longest <= Spacing + 1.0e-6);
		TestTrue(FString::Printf(TEXT("%s: the places along the polyline given are in order"), Name), bInOrder);
	};

	// ---- Corners on long legs.
	for (const double Degrees : { 60.0, 90.0, 120.0, 150.0 })
	{
		const FString Name = FString::Printf(TEXT("%.0f degrees"), Degrees);
		const TArray<FVector> Drawn = Corner(Degrees, 4000.0, 4000.0);
		TArray<FVector> Made;
		TArray<double> Places;
		const FShape::FCornerArcs Arcs = FShape::FitCornerArcs(Drawn, Spacing, Radius, Made, &Places);
		const double Moved = DistanceToPolyline(FVector::ZeroVector, Made);
		const double ArcMove = Radius * (1.0 / FMath::Cos(0.5 * FMath::DegreesToRadians(Degrees)) - 1.0);
		AddInfo(FString::Printf(TEXT("%s: as drawn %.1f times what the bound allows, as made %.2f; the corner moved %.0f uu (the arc's %.0f; the reach %.0f), the farthest move reported %.0f, %d vertices"),
			*Name, BendRatio(Drawn), BendRatio(Made), Moved, ArcMove, Reach, Arcs.FarthestMove, Made.Num()));
		TestTrue(*(Name + TEXT(": as drawn it breaks the bound (non-vacuous)")), BendRatio(Drawn) > 2.0 && BendWarnings(Drawn) > 0);
		TestTrue(*(Name + TEXT(": one arc, nothing left")), Arcs.NumArcs == 1 && Arcs.NumLeft == 0);
		TestTrue(*(Name + TEXT(": every vertex is within the bound, with the margin")), BendRatio(Made) <= 0.92 && FMath::IsNearlyEqual(Arcs.WorstTurn, FShape::GetWorstTurn(Made), 1.0e-12));
		TestEqual(*(Name + TEXT(": the engine's check has no bend warning")), BendWarnings(Made), 0);
		TestTrue(*(Name + TEXT(": the corner moves by the arc's own distance")), FMath::Abs(Moved - ArcMove) <= 0.02 * ArcMove + 5.0);
		TestTrue(*(Name + TEXT(": the move reported is the corner's")), FMath::Abs(Arcs.FarthestMove - Moved) <= 0.02 * Moved + 5.0);
		if (Degrees == 120.0)
		{
			TestTrue(TEXT("120 degrees: the corner moves by about a reach"), Moved <= 1.15 * Reach);
			// R1's spreading, as L5's component took it, stopped at twice the reach: a 120 degree corner stays outside the bound.
			TArray<FVector> Spread, Resampled;
			FShape::SpreadCorners(Drawn, Spacing, 2.0 * Reach, Spread);
			FShape::ResampleEvenly(Spread, Spacing, Resampled);
			AddInfo(FString::Printf(TEXT("120 degrees spread over twice the reach: %.2f times what the bound allows"), BendRatio(Resampled)));
			TestTrue(TEXT("120 degrees: spread over twice the reach it stays outside the bound (what G225 found)"), BendRatio(Resampled) > 1.0);
		}
		CheckMade(*Name, Drawn, Made, Places);
		int32 OffTheRuns = 0;
		const double Tangent = Radius * FMath::Tan(0.5 * FMath::DegreesToRadians(Degrees));
		for (const FVector& Point : Made)
		{
			// Further from the corner than the arc's tangent points (and a vertex more): on the straight runs as drawn.
			if (Point.Size() > Tangent + Spacing)
			{
				OffTheRuns += DistanceToPolyline(Point, Drawn) > 1.0e-6 ? 1 : 0;
			}
		}
		TestEqual(*(Name + TEXT(": the straight runs away from the arc are untouched")), OffTheRuns, 0);
	}

	// ---- Corners near an end.
	struct FNearEnd
	{
		const TCHAR* Name;
		double Degrees;
		double LegIn;
		double LegOut;
	};
	for (const FNearEnd& Case : { FNearEnd{ TEXT("A right angle on legs of five vertices"), 90.0, 750.0, 750.0 },
		FNearEnd{ TEXT("A right angle 400 uu from the end"), 90.0, 3000.0, 400.0 },
		FNearEnd{ TEXT("A right angle 400 uu from the start"), 90.0, 400.0, 3000.0 },
		FNearEnd{ TEXT("120 degrees on legs of 500 uu"), 120.0, 500.0, 500.0 } })
	{
		const TArray<FVector> Drawn = Corner(Case.Degrees, Case.LegIn, Case.LegOut);
		TArray<FVector> Made;
		TArray<double> Places;
		const FShape::FCornerArcs Arcs = FShape::FitCornerArcs(Drawn, Spacing, Radius, Made, &Places);
		AddInfo(FString::Printf(TEXT("%s: as drawn %.1f times what the bound allows, as made %.2f; the corner moved %.0f uu, %d vertices"),
			Case.Name, BendRatio(Drawn), BendRatio(Made), DistanceToPolyline(FVector::ZeroVector, Made), Made.Num()));
		TestTrue(FString::Printf(TEXT("%s: as drawn it breaks the bound (non-vacuous)"), Case.Name), BendRatio(Drawn) > 2.0);
		TestTrue(FString::Printf(TEXT("%s: rounded, every vertex within the bound"), Case.Name), Arcs.NumArcs == 1 && BendRatio(Made) <= 0.92 && BendWarnings(Made) == 0);
		CheckMade(Case.Name, Drawn, Made, Places);
	}

	// ---- Left as they were.
	{
		TArray<FVector> Made;
		TArray<double> Places;
		TArray<FVector> Gentle;
		for (int32 Index = 0; Index <= 40; ++Index)
		{
			const double Angle = Index * (Spacing / 2000.0);
			Gentle.Add(FVector(2000.0 * FMath::Sin(Angle), 2000.0 * (1.0 - FMath::Cos(Angle)), 0.0));
		}
		const FShape::FCornerArcs GentleArcs = FShape::FitCornerArcs(Gentle, Spacing, Radius, Made, &Places);
		bool bSame = Made.Num() == Gentle.Num();
		for (int32 Index = 0; bSame && Index < Made.Num(); ++Index)
		{
			bSame = Made[Index] == Gentle[Index] && Places[Index] == Index;
		}
		TestTrue(TEXT("A curve within the bound comes back as it was, point for point"), bSame && GentleArcs.NumArcs == 0 && GentleArcs.FarthestMove == 0.0);

		const TArray<FVector> Hairpin = Corner(170.0, 3000.0, 3000.0);
		const FShape::FCornerArcs HairpinArcs = FShape::FitCornerArcs(Hairpin, Spacing, Radius, Made);
		TestTrue(TEXT("A hairpin of 170 degrees is left as it was"), HairpinArcs.NumArcs == 0 && HairpinArcs.NumLeft == 1 && Made == Hairpin);

		// A left turn and a right turn, each a right angle: 600 uu apart they are too close for an arc each (an arc of the
		// radius needs about its radius on each leg), 1,500 uu apart they get one each.
		for (const double Gap : { 600.0, 1500.0 })
		{
			TArray<FVector> S;
			FShape::ResampleEvenly(TArray<FVector>{ FVector(-3000.0, 0.0, 0.0), FVector::ZeroVector, FVector(0.0, Gap, 0.0), FVector(3000.0, Gap, 0.0) }, Spacing, S);
			const FShape::FCornerArcs SArcs = FShape::FitCornerArcs(S, Spacing, Radius, Made);
			AddInfo(FString::Printf(TEXT("An S of two right angles %.0f uu apart: %d arcs, %d left, as made %.2f times what the bound allows"), Gap, SArcs.NumArcs, SArcs.NumLeft, BendRatio(Made)));
			if (Gap < 1000.0)
			{
				TestTrue(TEXT("An S too tight for two arcs is left as it was"), SArcs.NumArcs == 0 && SArcs.NumLeft == 1 && Made == S);
			}
			else
			{
				TestTrue(TEXT("Two corners far enough apart get an arc each, within the bound"), SArcs.NumArcs == 2 && SArcs.NumLeft == 0 && BendRatio(Made) <= 0.92);
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
