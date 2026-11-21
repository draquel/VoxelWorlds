// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelConditioningLine.h"
#include "VoxelChunkLineSegments.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
#include "VoxelBlockSurface.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelConditioningLineTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The conditioning line (Documentation/TERRAIN_LINES_PLAN.md, L2): a polyline that levels, lowers or raises the
// generated terrain along it.
//
// - Arithmetic: the weight's profile, the target and half width interpolated along the nearest segment, the three
//   blends, one blend per line, lines in id order and then the zones.
// - Bend: a line that keeps the bend bound has no step over a tenth of a voxel across a bisector inside its reach, and
//   has no warning; a line that breaks it steps by more and is warned about.
// - Warnings: each limit of the plan's section 4.1 names itself; a line that keeps them all has none.
// - SegmentList: a chunk's list holds the segments within reach of the chunk and no others, in order, in the chunk's
//   frame, and evaluating it gives what the line itself gives, on a plane and on a planet; a chunk no line reaches has
//   an empty list.
// - GeneratedChunk: on the plane, the island and a planet, a generated chunk is solid exactly where the analytic
//   conditioned height says (the core at the target, the three blends, two lines and a zone in order), and its voxels
//   beyond the reach are byte for byte the chunk's without the line.
// - BlockSurface: on a block world the block tops along a levelled line are the target's block top.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineArithmeticTest,
	"VoxelWorlds.Generation.ConditioningLine.Arithmetic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineArithmeticTest::RunTest(const FString& Parameters)
{
	using namespace VoxelConditioningLineTestUtils;

	// A line along X from 0 to 1,000 at Y 0: the target rises from 100 to 300, the half width from 100 to 200, the
	// falloff is 200 wide.
	FVoxelConditioningLineParams Params;
	Params.FalloffWidth = 200.0f;
	Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(0.0, 0.0, 0.0), 100.0f, 100.0f));
	Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(1000.0, 0.0, 0.0), 300.0f, 200.0f));
	const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(Params);
	TestEqual(TEXT("Two vertices make one segment"), Line->NumSegments(), 1);
	TestEqual(TEXT("The reach is the largest half width plus the falloff"), Line->GetReach(), 400.0);

	float Target = 0.0f;
	float Weight = 0.0f;
	TestTrue(TEXT("On the line the weight is the strength"), Line->Evaluate(500.0, 0.0, Target, Weight) && Weight == 1.0f);
	TestEqual(TEXT("The target is interpolated along the segment"), Target, 200.0f, 1e-3f);
	// At X 500 the half width is 150: Y 140 is on the core, Y 250 is half way across the falloff, Y 360 is beyond it.
	TestTrue(TEXT("Inside the interpolated half width the weight is the strength"), Line->Evaluate(500.0, 140.0, Target, Weight) && Weight == 1.0f);
	TestTrue(TEXT("Half way across the falloff the weight is a half (smoothstep)"), Line->Evaluate(500.0, -250.0, Target, Weight) && FMath::IsNearlyEqual(Weight, 0.5f, 1e-4f));
	TestFalse(TEXT("Beyond the falloff the line has no weight"), Line->Evaluate(500.0, 360.0, Target, Weight));
	TestFalse(TEXT("Beyond the reach the line has no weight"), Line->Evaluate(500.0, 5000.0, Target, Weight));
	// Past an end the nearest place is the end vertex: its target and its half width.
	TestTrue(TEXT("Past the end the end vertex answers"), Line->Evaluate(1100.0, 0.0, Target, Weight) && Target == 300.0f && Weight == 1.0f);
	TestTrue(TEXT("Past the start the start vertex answers"), Line->Evaluate(-150.0, 0.0, Target, Weight) && Target == 100.0f && Weight > 0.0f && Weight < 1.0f);

	// The three blends, at full weight and at half.
	using EBlend = EVoxelConditioningLineBlend;
	TestEqual(TEXT("Level reaches the target from below"), FVoxelConditioningLine::BlendHeight(EBlend::Level, 0.0f, 200.0f, 1.0f), 200.0f);
	TestEqual(TEXT("Level reaches the target from above"), FVoxelConditioningLine::BlendHeight(EBlend::Level, 500.0f, 200.0f, 1.0f), 200.0f);
	TestEqual(TEXT("Level at half weight is half way"), FVoxelConditioningLine::BlendHeight(EBlend::Level, 0.0f, 200.0f, 0.5f), 100.0f);
	TestEqual(TEXT("LowerOnly cuts ground over the target"), FVoxelConditioningLine::BlendHeight(EBlend::LowerOnly, 500.0f, 200.0f, 1.0f), 200.0f);
	TestEqual(TEXT("LowerOnly leaves ground under the target"), FVoxelConditioningLine::BlendHeight(EBlend::LowerOnly, 50.0f, 200.0f, 1.0f), 50.0f);
	TestEqual(TEXT("RaiseOnly fills ground under the target"), FVoxelConditioningLine::BlendHeight(EBlend::RaiseOnly, 50.0f, 200.0f, 1.0f), 200.0f);
	TestEqual(TEXT("RaiseOnly leaves ground over the target"), FVoxelConditioningLine::BlendHeight(EBlend::RaiseOnly, 500.0f, 200.0f, 1.0f), 500.0f);
	TestEqual(TEXT("LowerOnly at half weight is half way down"), FVoxelConditioningLine::BlendHeight(EBlend::LowerOnly, 500.0f, 200.0f, 0.5f), 350.0f);

	// Strength scales the reach toward the target; a strength of 0 leaves the height alone.
	Params.Strength = 0.5f;
	FVoxelConditioning Half;
	Half.Lines.Add(FVoxelConditioningLine::Make(Params));
	TestEqual(TEXT("Strength 0.5 goes half way on the core"), Half.ApplyToHeight(500.0, 0.0, 0.0f), 100.0f, 1e-3f);
	Params.Strength = 0.0f;
	FVoxelConditioning Paint;
	Paint.Lines.Add(FVoxelConditioningLine::Make(Params));
	TestEqual(TEXT("Strength 0 leaves the height alone"), Paint.ApplyToHeight(500.0, 0.0, 37.0f), 37.0f);
	Params.Strength = 1.0f;

	// One blend per line: a point in the falloff of two segments of one line takes one blend, from the nearest. A line
	// with a vertex in the middle gives what the one-segment line gives.
	{
		FVoxelConditioningLineParams Split = Params;
		Split.Vertices.Insert(FVoxelConditioningLineVertex(FVector(500.0, 0.0, 0.0), 200.0f, 150.0f), 1);
		FVoxelConditioning One;
		One.Lines.Add(Line);
		FVoxelConditioning Two;
		Two.Lines.Add(FVoxelConditioningLine::Make(Split));
		for (const FVector2D& P : { FVector2D(500.0, 250.0), FVector2D(480.0, -230.0), FVector2D(520.0, 300.0), FVector2D(250.0, 200.0) })
		{
			TestEqual(FString::Printf(TEXT("A vertex in the middle changes nothing at (%.0f, %.0f)"), P.X, P.Y),
				Two.ApplyToHeight(P.X, P.Y, 0.0f), One.ApplyToHeight(P.X, P.Y, 0.0f), 1e-3f);
		}
	}

	// Lines in id order, then the zones: where two lines cross the later id blends on top, whatever order they were
	// gathered in, and a zone blends on top of both, so a pad wins where a road arrives at it.
	{
		const FVoxelConditioningLineRef First = FVoxelConditioningLine::Make(MakeStraight(FVector2D(-1000.0, 0.0), FVector2D(1000.0, 0.0), 100.0f, 100.0f, 100.0f, 200.0f, EBlend::Level, 5));
		const FVoxelConditioningLineRef Second = FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, -1000.0), FVector2D(0.0, 1000.0), 400.0f, 400.0f, 100.0f, 200.0f, EBlend::Level, 9));
		FVoxelConditioning Crossing;
		Crossing.Lines.Add(Second);
		Crossing.Lines.Add(First);
		Crossing.Lines.Add(Second);
		Crossing.SortLines();
		TestEqual(TEXT("A line gathered twice counts once"), Crossing.Lines.Num(), 2);
		TestTrue(TEXT("The lines are in id order"), Crossing.Lines.Num() == 2 && Crossing.Lines[0]->GetId() == 5 && Crossing.Lines[1]->GetId() == 9);
		TestEqual(TEXT("Where two lines cross the later id wins on its core"), Crossing.ApplyToHeight(0.0, 0.0, 0.0f), 400.0f);
		// In the second line's falloff, over the first's core: the first reaches 100, the second blends half way to 400.
		TestEqual(TEXT("The later line blends on top of the earlier one"), Crossing.ApplyToHeight(200.0, 0.0, 0.0f), 250.0f, 1e-2f);
		Crossing.Zones.Add(FVoxelConditioningZone(FVector2D(0.0, 0.0), 300.0f, 100.0f, 700.0f));
		TestEqual(TEXT("A zone blends on top of the lines"), Crossing.ApplyToHeight(0.0, 0.0, 0.0f), 700.0f);
	}

	// A planet line: the same profile over the distance along the surface.
	{
		const double R = 100000.0;
		const FVector A = FVector(1.0, 0.0, 0.0);
		const FVector B = FVector(FMath::Cos(3000.0 / R), FMath::Sin(3000.0 / R), 0.0);
		const FVoxelConditioningLineRef Arc = FVoxelConditioningLine::Make(MakeArc(A, B, R, 50.0f, 250.0f, 150.0f, 200.0f));
		const auto At = [&](double AlongUU, double AcrossUU)
		{
			return FVector(FMath::Cos(AlongUU / R) * FMath::Cos(AcrossUU / R), FMath::Sin(AlongUU / R) * FMath::Cos(AcrossUU / R), FMath::Sin(AcrossUU / R));
		};
		TestTrue(TEXT("Planet: on the line the weight is the strength"), Arc->EvaluateOnSphere(At(1500.0, 0.0), R, Target, Weight) && Weight == 1.0f);
		TestEqual(TEXT("Planet: the target is interpolated along the line"), Target, 150.0f, 0.1f);
		TestTrue(TEXT("Planet: on the core the weight is the strength"), Arc->EvaluateOnSphere(At(1500.0, 140.0), R, Target, Weight) && Weight == 1.0f);
		TestTrue(TEXT("Planet: half way across the falloff the weight is a half"), Arc->EvaluateOnSphere(At(1500.0, -250.0), R, Target, Weight) && FMath::IsNearlyEqual(Weight, 0.5f, 2e-3f));
		TestFalse(TEXT("Planet: beyond the falloff the line has no weight"), Arc->EvaluateOnSphere(At(1500.0, 360.0), R, Target, Weight));
		TestFalse(TEXT("Planet: on the far side the line has no weight"), Arc->EvaluateOnSphere(FVector(-1.0, 0.0, 0.0), R, Target, Weight));
		// Each kind is ignored by the other's functions.
		TestFalse(TEXT("A planet line has no heightmap weight"), Arc->Evaluate(0.0, 0.0, Target, Weight));
		TestFalse(TEXT("A heightmap line has no planet weight"), Line->EvaluateOnSphere(A, R, Target, Weight));
		FVoxelConditioning Mixed;
		Mixed.Lines.Add(Arc);
		TestEqual(TEXT("A planet line changes no heightmap height"), Mixed.ApplyToHeight(0.0, 0.0, 12.0f), 12.0f);
		TestEqual(TEXT("A planet line changes the altitude along it"), Mixed.ApplyToAltitude(At(1500.0, 0.0), R, 0.0f), 150.0f, 0.1f);
	}

	// A line with fewer than two distinct vertices conditions nothing.
	{
		FVoxelConditioningLineParams Single;
		Single.Vertices.Add(FVoxelConditioningLineVertex(FVector(10.0, 10.0, 0.0), 500.0f, 100.0f));
		Single.Vertices.Add(FVoxelConditioningLineVertex(FVector(10.0, 10.0, 0.0), 500.0f, 100.0f));
		const FVoxelConditioningLineRef Empty = FVoxelConditioningLine::Make(Single);
		TestEqual(TEXT("Two vertices at one place make no segment"), Empty->NumSegments(), 0);
		TestFalse(TEXT("A line with no segment has no weight"), Empty->Evaluate(10.0, 10.0, Target, Weight));
		TestFalse(TEXT("A line with no segment meets no region"), Empty->MeetsRegion(FBox2D(FVector2D(-100.0, -100.0), FVector2D(100.0, 100.0))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineBendTest,
	"VoxelWorlds.Generation.ConditioningLine.Bend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineBendTest::RunTest(const FString& Parameters)
{
	using namespace VoxelConditioningLineTestUtils;

	// The road class: half width 3 voxels, falloff 4 voxels (a reach of 525 uu), a vertex every 2 voxels. A curve of a
	// given radius climbing at a given grade, through 120 degrees.
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;
	const double Spacing = 2.0 * VoxelSize;
	const auto MakeCurve = [&](double Radius, double Grade)
	{
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		const double Turn = Spacing / Radius;
		const int32 Steps = FMath::CeilToInt32(FMath::DegreesToRadians(120.0) / Turn);
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const double Angle = Step * Turn;
			Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(Radius * FMath::Sin(Angle), Radius * (1.0 - FMath::Cos(Angle)), 0.0),
				static_cast<float>(Grade * Step * Spacing), HalfWidth));
		}
		return Params;
	};
	// The largest change of the conditioned height across a bisector inside the reach: two points a hundredth of a unit
	// either side of each inner vertex's bisector, on the inside of the bend, at every distance from the line.
	const auto LargestStep = [&](const FVoxelConditioningLineRef& Line)
	{
		FVoxelConditioning Conditioning;
		Conditioning.Lines.Add(Line);
		const TArray<FVoxelConditioningLineVertex>& V = Line->GetVertices();
		float Largest = 0.0f;
		for (int32 Index = 1; Index + 1 < V.Num(); ++Index)
		{
			const FVector2D P(V[Index].Position.X, V[Index].Position.Y);
			const FVector2D In = (P - FVector2D(V[Index - 1].Position.X, V[Index - 1].Position.Y)).GetSafeNormal();
			const FVector2D Out = (FVector2D(V[Index + 1].Position.X, V[Index + 1].Position.Y) - P).GetSafeNormal();
			const FVector2D Along = (In + Out).GetSafeNormal();
			// The inside of the bend is the side the line turns toward.
			const FVector2D Inside = FVector2D(-Along.Y, Along.X) * (FVector2D::CrossProduct(In, Out) >= 0.0 ? 1.0 : -1.0);
			for (double Dist = 10.0; Dist < Line->GetReach(); Dist += 10.0)
			{
				const FVector2D OnBisector = P + Inside * Dist;
				const float Before = Conditioning.ApplyToHeight(OnBisector.X - Along.X * 0.01, OnBisector.Y - Along.Y * 0.01, 0.0f);
				const float After = Conditioning.ApplyToHeight(OnBisector.X + Along.X * 0.01, OnBisector.Y + Along.Y * 0.01, 0.0f);
				Largest = FMath::Max(Largest, FMath::Abs(After - Before));
			}
		}
		return Largest;
	};
	FVoxelConditioningLineLimits Limits;
	Limits.VoxelSize = VoxelSize;
	const float Tenth = 0.1f * VoxelSize;

	// The plan's road numbers: 16 degrees a vertex up to a grade of 10 % (a radius of the reach), 6.7 degrees at 25 %
	// (1,280 uu). Each keeps the bound: no step over a tenth of a voxel, and no warning.
	for (const TPair<double, double>& Case : { TPair<double, double>(540.0, 0.10), TPair<double, double>(1300.0, 0.25), TPair<double, double>(800.0, 0.15) })
	{
		const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(MakeCurve(Case.Key, Case.Value));
		const float Step = LargestStep(Line);
		AddInfo(FString::Printf(TEXT("Radius %.0f at %.0f %%: the largest step across a bisector is %.2f uu (a tenth of a voxel is %.1f)"), Case.Key, 100.0 * Case.Value, Step, Tenth));
		TestTrue(FString::Printf(TEXT("Radius %.0f at %.0f %% steps under a tenth of a voxel (%.2f)"), Case.Key, 100.0 * Case.Value, Step), Step <= Tenth);
		const TArray<FString> Warnings = Line->GetWarnings(Limits);
		TestEqual(FString::Printf(TEXT("Radius %.0f at %.0f %% has no warning (%s)"), Case.Key, 100.0 * Case.Value, Warnings.Num() > 0 ? *Warnings[0] : TEXT("none")), Warnings.Num(), 0);
	}

	// A curve too tight for its grade breaks the bound: the step is over a tenth of a voxel, and the line says so.
	{
		const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(MakeCurve(540.0, 0.25));
		const float Step = LargestStep(Line);
		AddInfo(FString::Printf(TEXT("Radius 540 at 25 %%: the largest step across a bisector is %.2f uu"), Step));
		TestTrue(TEXT("A tight curve at 25 % steps over a tenth of a voxel"), Step > Tenth);
		const TArray<FString> Warnings = Line->GetWarnings(Limits);
		TestTrue(TEXT("A tight curve at 25 % is warned about (the bend bound)"), Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("bend bound")); }));
	}

	// A level line may turn as tightly as the reach allows: with no grade there is no step.
	{
		const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(MakeCurve(540.0, 0.0));
		TestTrue(TEXT("A level curve has no step"), LargestStep(Line) < 0.01f);
		TestEqual(TEXT("A level curve of the reach's radius has no warning"), Line->GetWarnings(Limits).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineWarningsTest,
	"VoxelWorlds.Generation.ConditioningLine.Warnings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineWarningsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelConditioningLineTestUtils;
	FVoxelConditioningLineLimits Limits;
	Limits.VoxelSize = VoxelSize;
	Limits.MaxCutFill = 1200.0f;
	Limits.NaturalHeight = [](const FVector&) { return 0.0f; };
	const auto Has = [](const TArray<FString>& Warnings, const TCHAR* Text)
	{
		return Warnings.ContainsByPredicate([Text](const FString& W) { return W.Contains(Text); });
	};
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;

	// A straight road within the cap keeps every limit.
	{
		const TArray<FString> Warnings = FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, 0.0), FVector2D(6000.0, 2000.0), 100.0f, 500.0f, HalfWidth, Falloff))->GetWarnings(Limits);
		TestEqual(FString::Printf(TEXT("A straight road has no warning (%s)"), Warnings.Num() > 0 ? *Warnings[0] : TEXT("none")), Warnings.Num(), 0);
	}
	// No segment.
	{
		FVoxelConditioningLineParams Params;
		Params.Vertices.Add(FVoxelConditioningLineVertex(FVector::ZeroVector, 0.0f, HalfWidth));
		TestTrue(TEXT("One vertex: no segment"), Has(FVoxelConditioningLine::Make(Params)->GetWarnings(Limits), TEXT("fewer than two")));
	}
	// The cut and fill cap.
	TestTrue(TEXT("A target far from the natural surface breaks the cut and fill cap"),
		Has(FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), 2000.0f, 2000.0f, HalfWidth, Falloff))->GetWarnings(Limits), TEXT("cut and fill cap")));
	// The widths.
	TestTrue(TEXT("A core under two voxels is warned about"),
		Has(FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), 0.0f, 0.0f, 0.5f * VoxelSize, Falloff))->GetWarnings(Limits), TEXT("levelled core")));
	TestTrue(TEXT("A falloff under two voxels is warned about"),
		Has(FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), 0.0f, 0.0f, HalfWidth, VoxelSize))->GetWarnings(Limits), TEXT("falloff")));
	// A line that only paints (strength 0) has no target: the widths and the cap do not apply.
	{
		FVoxelConditioningLineParams Paint = MakeStraight(FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), 5000.0f, 5000.0f, 0.5f * VoxelSize, VoxelSize);
		Paint.Strength = 0.0f;
		TestEqual(TEXT("A paint-only line is not held to the cap or the widths"), FVoxelConditioningLine::Make(Paint)->GetWarnings(Limits).Num(), 0);
	}
	// A right-angle corner: the bisectors meet inside the reach, and with a grade the target steps.
	{
		FVoxelConditioningLineParams Corner = MakeStraight(FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), 0.0f, 600.0f, HalfWidth, Falloff);
		const FVoxelConditioningLineParams Leg = MakeStraight(FVector2D(3000.0, 0.0), FVector2D(3000.0, 3000.0), 600.0f, 1200.0f, HalfWidth, Falloff);
		Corner.Vertices.Append(Leg.Vertices);
		const TArray<FString> Warnings = FVoxelConditioningLine::Make(Corner)->GetWarnings(Limits);
		TestTrue(TEXT("A right-angle corner turns too sharply"), Has(Warnings, TEXT("turn so sharply")));
		TestTrue(TEXT("A right-angle corner on a grade breaks the bend bound"), Has(Warnings, TEXT("bend bound")));
	}
	// A hairpin: the line comes back within twice its reach of itself.
	{
		FVoxelConditioningLineParams Hairpin = MakeStraight(FVector2D(0.0, 0.0), FVector2D(4000.0, 0.0), 0.0f, 0.0f, HalfWidth, Falloff);
		const double Radius = 300.0;
		for (int32 Step = 1; Step < 12; ++Step)
		{
			const double Angle = UE_DOUBLE_PI * Step / 12.0;
			Hairpin.Vertices.Add(FVoxelConditioningLineVertex(FVector(4000.0 + Radius * FMath::Sin(Angle), Radius * (1.0 - FMath::Cos(Angle)), 0.0), 0.0f, HalfWidth));
		}
		Hairpin.Vertices.Append(MakeStraight(FVector2D(4000.0, 2.0 * Radius), FVector2D(0.0, 2.0 * Radius), 0.0f, 0.0f, HalfWidth, Falloff).Vertices);
		TestTrue(TEXT("A hairpin comes back on itself"), Has(FVoxelConditioningLine::Make(Hairpin)->GetWarnings(Limits), TEXT("within twice the reach")));
	}
	// A line that crosses itself.
	{
		FVoxelConditioningLineParams Loop = MakeStraight(FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), 0.0f, 0.0f, HalfWidth, Falloff);
		Loop.Vertices.Append(MakeStraight(FVector2D(6000.0, 0.0), FVector2D(6000.0, 6000.0), 0.0f, 0.0f, HalfWidth, Falloff).Vertices);
		Loop.Vertices.Append(MakeStraight(FVector2D(6000.0, 6000.0), FVector2D(3000.0, -3000.0), 0.0f, 0.0f, HalfWidth, Falloff).Vertices);
		TestTrue(TEXT("A line that crosses itself is warned about"), Has(FVoxelConditioningLine::Make(Loop)->GetWarnings(Limits), TEXT("within twice the reach")));
	}
	// A block world: a target steeper than one block a column.
	{
		FVoxelConditioningLineLimits Block = Limits;
		Block.bBlockWorld = true;
		Block.MaxCutFill = 0.0f;
		TestTrue(TEXT("A block world: a slope over one block a column is warned about"),
			Has(FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), 0.0f, 2700.0f, HalfWidth, Falloff))->GetWarnings(Block), TEXT("one block a column")));
		TestEqual(TEXT("A block world: a gentle slope has no warning"),
			FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), 0.0f, 900.0f, HalfWidth, Falloff))->GetWarnings(Block).Num(), 0);
		TestFalse(TEXT("A smooth world: the same slope is not a block warning"),
			Has(FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), 0.0f, 2700.0f, HalfWidth, Falloff))->GetWarnings(Limits), TEXT("one block a column")));
	}
	// A planet: a long segment's chord sags under the datum sphere.
	{
		FVoxelConditioningLineLimits Planet = Limits;
		Planet.PlanetRadius = 100000.0;
		const FVector A(1.0, 0.0, 0.0);
		const FVector B(FMath::Cos(0.1), FMath::Sin(0.1), 0.0);
		TestTrue(TEXT("A planet: a 10,000 uu segment sags"), Has(FVoxelConditioningLine::Make(MakeArc(A, B, 100000.0, 0.0f, 0.0f, HalfWidth, Falloff, EVoxelConditioningLineBlend::Level, 1, 20000.0))->GetWarnings(Planet), TEXT("sag")));
		TestEqual(TEXT("A planet: the same arc with a vertex every 2 voxels has no warning"),
			FVoxelConditioningLine::Make(MakeArc(A, B, 100000.0, 0.0f, 0.0f, HalfWidth, Falloff))->GetWarnings(Planet).Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineSegmentListTest,
	"VoxelWorlds.Generation.ConditioningLine.SegmentList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineSegmentListTest::RunTest(const FString& Parameters)
{
	using namespace VoxelConditioningLineTestUtils;
	const double ChunkWorldSize = ChunkSize * VoxelSize;
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;

	// A plane: a long diagonal line that climbs, and a second line across it, far from the world's origin so a float
	// world position would be coarse (the list is in the chunk's frame).
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::InfinitePlane);
		const FVector2D Base(3.0e6, -2.0e6);
		const FVoxelConditioningLineRef Diagonal = FVoxelConditioningLine::Make(MakeStraight(Base + FVector2D(-9000.0, -7000.0), Base + FVector2D(9000.0, 8000.0), -300.0f, 900.0f, HalfWidth, Falloff, EVoxelConditioningLineBlend::Level, 1));
		const FVoxelConditioningLineRef Across = FVoxelConditioningLine::Make(MakeStraight(Base + FVector2D(-4000.0, 5000.0), Base + FVector2D(5000.0, -5000.0), 500.0f, 200.0f, 2.0f * VoxelSize, Falloff, EVoxelConditioningLineBlend::LowerOnly, 2));
		FVoxelConditioning Conditioning;
		Conditioning.Lines = { Diagonal, Across };

		int32 ChunksWithSegments = 0;
		int32 ChunksWithout = 0;
		int32 ColumnsChecked = 0;
		int32 ColumnsWithWeight = 0;
		float LargestDifference = 0.0f;
		bool bOrdered = true;
		bool bOnlyNear = true;
		const FIntVector Centre = ChunkAt(*Config, FVector(Base.X, Base.Y, 0.0));
		for (int32 CY = -4; CY <= 4; ++CY)
		{
			for (int32 CX = -4; CX <= 4; ++CX)
			{
				FVoxelNoiseGenerationRequest Request = MakeRequest(*Config, Centre + FIntVector(CX, CY, 0));
				Request.Conditioning = Conditioning;
				FVoxelChunkLineSegments List;
				FVoxelChunkLineSegments::Build(Request, List);
				(List.Num() > 0 ? ChunksWithSegments : ChunksWithout)++;
				const FVector Origin = Request.GetChunkWorldPosition();

				// The list's lines in order, and every segment within its line's reach of the chunk's box.
				int32 LastLine = -1;
				for (int32 Segment = 0; Segment < List.Num(); ++Segment)
				{
					const FVector4f& S0 = List.Data[Segment * FVoxelConditioningLine::SegmentStride + 0];
					const FVector4f& S1 = List.Data[Segment * FVoxelConditioningLine::SegmentStride + 1];
					const int32 LineIndex = static_cast<int32>(List.Data[Segment * FVoxelConditioningLine::SegmentStride + 3].X);
					bOrdered &= LineIndex >= LastLine;
					LastLine = LineIndex;
					const double Reach = Conditioning.Lines[LineIndex]->GetReach();
					const FBox2D Box(FVector2D(-Reach, -Reach), FVector2D(ChunkWorldSize + Reach, ChunkWorldSize + Reach));
					bOnlyNear &= FBox2D(FVector2D(FMath::Min(S0.X, S1.X), FMath::Min(S0.Y, S1.Y)), FVector2D(FMath::Max(S0.X, S1.X), FMath::Max(S0.Y, S1.Y))).Intersect(Box);
				}

				// Each column: the list's answer against the lines' own, at the position the generators give it.
				for (int32 Y = 0; Y < ChunkSize; ++Y)
				{
					for (int32 X = 0; X < ChunkSize; ++X)
					{
						const FVector3f Local(X * VoxelSize, Y * VoxelSize, 0.0f);
						const float FromList = List.Num() > 0 ? List.Apply(Local, 100.0f) : 100.0f;
						const float FromLines = Conditioning.ApplyToHeight(Origin.X + Local.X, Origin.Y + Local.Y, 100.0f);
						LargestDifference = FMath::Max(LargestDifference, FMath::Abs(FromList - FromLines));
						++ColumnsChecked;
						ColumnsWithWeight += FromLines != 100.0f ? 1 : 0;
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("Plane: %d chunks with segments, %d without; %d columns, %d with a weight; the list and the lines differ by at most %.4f uu"),
			ChunksWithSegments, ChunksWithout, ColumnsChecked, ColumnsWithWeight, LargestDifference));
		TestTrue(TEXT("Plane: some chunks have segments and some have none"), ChunksWithSegments > 5 && ChunksWithout > 5);
		TestTrue(TEXT("Plane: the lines reach some columns"), ColumnsWithWeight > 1000);
		TestTrue(TEXT("Plane: a list's lines are in the order they are applied"), bOrdered);
		TestTrue(TEXT("Plane: a list holds only segments within reach of its chunk"), bOnlyNear);
		TestTrue(FString::Printf(TEXT("Plane: the chunk's list gives what the lines give (largest difference %.4f uu)"), LargestDifference), LargestDifference < 0.05f);

		// A chunk no line reaches: an empty list. A request with no line: an empty list.
		FVoxelNoiseGenerationRequest Far = MakeRequest(*Config, Centre + FIntVector(40, 0, 0));
		Far.Conditioning = Conditioning;
		FVoxelChunkLineSegments FarList;
		FVoxelChunkLineSegments::Build(Far, FarList);
		TestEqual(TEXT("Plane: a chunk no line reaches has an empty list"), FarList.Num(), 0);
		Far.Conditioning.Reset();
		FVoxelChunkLineSegments::Build(Far, FarList);
		TestEqual(TEXT("Plane: a request with no line has an empty list"), FarList.Num(), 0);
		Config->RemoveFromRoot();
	}

	// A planet: an arc across a cube-face edge, the list in the frame of each chunk's anchor.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::SphericalPlanet);
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(*Config);
		const double R = Planet.PlanetRadius;
		const FVector A = FVector(1.0, 0.92, 0.3).GetSafeNormal();
		const FVector B = FVector(0.92, 1.0, 0.36).GetSafeNormal();
		const FVoxelConditioningLineRef Arc = FVoxelConditioningLine::Make(MakeArc(A, B, R, 200.0f, -100.0f, HalfWidth, Falloff));
		FVoxelConditioning Conditioning;
		Conditioning.Lines.Add(Arc);
		const FVector Middle = (A + B).GetSafeNormal();
		const FIntVector Centre = ChunkAt(*Config, Planet.PlanetCenter + Middle * R);

		int32 ChunksWithSegments = 0;
		int32 VoxelsChecked = 0;
		int32 VoxelsWithWeight = 0;
		float LargestDifference = 0.0f;
		for (int32 CZ = -2; CZ <= 2; ++CZ)
		{
			for (int32 CY = -2; CY <= 2; ++CY)
			{
				for (int32 CX = -2; CX <= 2; ++CX)
				{
					FVoxelNoiseGenerationRequest Request = MakeRequest(*Config, Centre + FIntVector(CX, CY, CZ));
					Request.Conditioning = Conditioning;
					FVoxelChunkLineSegments List;
					FVoxelChunkLineSegments::Build(Request, List);
					ChunksWithSegments += List.Num() > 0 ? 1 : 0;
					const FVector AnchorDirection = FSphericalPlanetWorldMode::GetDirectionFromCenter(Request.GetPlanetAnchorRelative(), FVector::ZeroVector);
					const FVector Origin = Request.GetChunkWorldPosition();
					for (int32 Z = 0; Z < ChunkSize; Z += 3)
					{
						for (int32 Y = 0; Y < ChunkSize; Y += 3)
						{
							for (int32 X = 0; X < ChunkSize; X += 3)
							{
								const FVector Direction = (Origin + FVector(X, Y, Z) * VoxelSize - Planet.PlanetCenter).GetSafeNormal();
								const FVector3f SurfaceOffset((Direction - AnchorDirection) * R);
								const float FromList = (List.Num() > 0 && List.MayReach(SurfaceOffset)) ? List.Apply(SurfaceOffset, 40.0f) : 40.0f;
								const float FromLines = Conditioning.ApplyToAltitude(Direction, R, 40.0f);
								LargestDifference = FMath::Max(LargestDifference, FMath::Abs(FromList - FromLines));
								++VoxelsChecked;
								VoxelsWithWeight += FromLines != 40.0f ? 1 : 0;
							}
						}
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("Planet: %d of 125 chunks with segments; %d voxels, %d with a weight; the list and the line differ by at most %.4f uu"),
			ChunksWithSegments, VoxelsChecked, VoxelsWithWeight, LargestDifference));
		TestTrue(TEXT("Planet: some chunks have segments and some have none"), ChunksWithSegments > 5 && ChunksWithSegments < 125);
		TestTrue(TEXT("Planet: the line reaches some voxels"), VoxelsWithWeight > 1000);
		TestTrue(FString::Printf(TEXT("Planet: the chunk's list gives what the line gives (largest difference %.4f uu)"), LargestDifference), LargestDifference < 0.1f);
		Config->RemoveFromRoot();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineGeneratedChunkTest,
	"VoxelWorlds.Generation.ConditioningLine.GeneratedChunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineGeneratedChunkTest::RunTest(const FString& Parameters)
{
	using namespace VoxelConditioningLineTestUtils;
	using EBlend = EVoxelConditioningLineBlend;
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;

	// The plane and the island: a diagonal line through a site, its target a set offset from the natural height there.
	for (const EWorldMode WorldMode : { EWorldMode::InfinitePlane, EWorldMode::IslandBowl })
	{
		const TCHAR* Name = WorldMode == EWorldMode::IslandBowl ? TEXT("Island") : TEXT("Plane");
		UVoxelWorldConfiguration* Config = MakeConfig(WorldMode);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FVector2D Site(2600.0, 3100.0);
		const float NaturalAtSite = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector(Site.X, Site.Y, 0.0), Config->NoiseParams).Position.Z);
		const auto Along = [&](float TargetA, float TargetB, EBlend Blend, int64 Id)
		{
			return FVoxelConditioningLine::Make(MakeStraight(Site + FVector2D(-5000.0, -3500.0), Site + FVector2D(5000.0, 3500.0), TargetA, TargetB, HalfWidth, Falloff, Blend, Id));
		};
		struct FCase
		{
			const TCHAR* Name;
			FVoxelConditioning Conditioning;
			float SiteHeight;
		};
		TArray<FCase> Cases;
		{
			FCase& Level = Cases.AddDefaulted_GetRef();
			Level.Name = TEXT("a levelled line");
			Level.Conditioning.Lines.Add(Along(NaturalAtSite + 240.0f, NaturalAtSite + 240.0f, EBlend::Level, 1));
			Level.SiteHeight = NaturalAtSite + 240.0f;
			FCase& Sloped = Cases.AddDefaulted_GetRef();
			Sloped.Name = TEXT("a levelled line on a grade");
			Sloped.Conditioning.Lines.Add(Along(NaturalAtSite - 700.0f, NaturalAtSite + 700.0f, EBlend::Level, 1));
			Sloped.SiteHeight = NaturalAtSite;
			FCase& Lower = Cases.AddDefaulted_GetRef();
			Lower.Name = TEXT("a lower-only line");
			Lower.Conditioning.Lines.Add(Along(NaturalAtSite - 260.0f, NaturalAtSite - 260.0f, EBlend::LowerOnly, 1));
			Lower.SiteHeight = NaturalAtSite - 260.0f;
			FCase& Raise = Cases.AddDefaulted_GetRef();
			Raise.Name = TEXT("a raise-only line");
			Raise.Conditioning.Lines.Add(Along(NaturalAtSite + 260.0f, NaturalAtSite + 260.0f, EBlend::RaiseOnly, 1));
			Raise.SiteHeight = NaturalAtSite + 260.0f;
			// Two lines that cross at the site and a zone on top: lines in id order, then the zone.
			FCase& Order = Cases.AddDefaulted_GetRef();
			Order.Name = TEXT("two lines and a zone");
			Order.Conditioning.Lines.Add(Along(NaturalAtSite + 240.0f, NaturalAtSite + 240.0f, EBlend::Level, 1));
			Order.Conditioning.Lines.Add(FVoxelConditioningLine::Make(MakeStraight(Site + FVector2D(-3000.0, 3000.0), Site + FVector2D(3000.0, -3000.0),
				NaturalAtSite - 150.0f, NaturalAtSite - 150.0f, HalfWidth, Falloff, EBlend::Level, 2)));
			Order.Conditioning.Zones.Add(FVoxelConditioningZone(Site + FVector2D(600.0, 0.0), 300.0f, 400.0f, NaturalAtSite + 90.0f));
			Order.SiteHeight = Order.Conditioning.ApplyToHeight(Site.X, Site.Y, NaturalAtSite);
		}
		for (const FCase& Case : Cases)
		{
			// The chunk the conditioned surface passes through at the site.
			const FIntVector Chunk = ChunkAt(*Config, FVector(Site.X, Site.Y, Case.SiteHeight));
			const FChunkCheck Check = CheckChunk(*Config, *Mode, Chunk, Case.Conditioning, 1.0f);
			AddInfo(FString::Printf(TEXT("%s, %s: chunk (%d, %d, %d): %d voxels, %d wrong; %d columns moved over a voxel; %d voxels untouched, %d of them differing; the CPU generator %.2f ms without the conditioning, %.2f ms with it"),
				Name, Case.Name, Chunk.X, Chunk.Y, Chunk.Z, Check.Voxels, Check.Wrong, Check.Moved, Check.Untouched, Check.UntouchedDiffering, Check.NaturalMs, Check.ConditionedMs));
			TestEqual(FString::Printf(TEXT("%s, %s: the chunk is solid where the analytic height says"), Name, Case.Name), Check.Wrong, 0);
			TestTrue(FString::Printf(TEXT("%s, %s: the line moves some columns by more than a voxel"), Name, Case.Name), Check.Moved > 20);
			TestTrue(FString::Printf(TEXT("%s, %s: part of the chunk is beyond the reach"), Name, Case.Name), Check.Untouched > 1000);
			TestEqual(FString::Printf(TEXT("%s, %s: beyond the reach the chunk is byte for byte the natural one"), Name, Case.Name), Check.UntouchedDiffering, 0);
		}
		// On the core the conditioned height is the target (the first case, at the site).
		TestEqual(FString::Printf(TEXT("%s: the levelled line reaches its target on the core"), Name),
			Cases[0].Conditioning.ApplyToHeight(Site.X + 40.0, Site.Y - 30.0, NaturalAtSite), NaturalAtSite + 240.0f, 1e-2f);
		Config->RemoveFromRoot();
	}

	// A planet: an arc through a site across a cube-face edge.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::SphericalPlanet);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FSphericalPlanetWorldMode& PlanetMode = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(*Config);
		const double R = Planet.PlanetRadius;
		const FVector A = FVector(1.0, 0.95, 0.3).GetSafeNormal();
		const FVector B = FVector(0.95, 1.0, 0.36).GetSafeNormal();
		const FVector Site = (A + B).GetSafeNormal();
		const float NaturalAtSite = PlanetMode.GetTerrainAltitude(Site, Config->NoiseParams);
		struct FCase
		{
			const TCHAR* Name;
			float Offset;
			EBlend Blend;
		};
		for (const FCase& Case : { FCase{ TEXT("a levelled line"), 240.0f, EBlend::Level }, FCase{ TEXT("a lower-only line"), -260.0f, EBlend::LowerOnly },
			FCase{ TEXT("a raise-only line"), 260.0f, EBlend::RaiseOnly } })
		{
			FVoxelConditioning Conditioning;
			Conditioning.Lines.Add(FVoxelConditioningLine::Make(MakeArc(A, B, R, NaturalAtSite + Case.Offset, NaturalAtSite + Case.Offset, HalfWidth, Falloff, Case.Blend)));
			const FIntVector Chunk = ChunkAt(*Config, Planet.PlanetCenter + Site * (R + NaturalAtSite + Case.Offset));
			const FChunkCheck Check = CheckChunk(*Config, *Mode, Chunk, Conditioning, 2.0f);
			AddInfo(FString::Printf(TEXT("Planet, %s: chunk (%d, %d, %d): %d voxels, %d wrong; %d voxels moved over a voxel; %d untouched, %d of them differing; the CPU generator %.2f ms without the conditioning, %.2f ms with it"),
				Case.Name, Chunk.X, Chunk.Y, Chunk.Z, Check.Voxels, Check.Wrong, Check.Moved, Check.Untouched, Check.UntouchedDiffering, Check.NaturalMs, Check.ConditionedMs));
			TestEqual(FString::Printf(TEXT("Planet, %s: the chunk is solid where the analytic altitude says"), Case.Name), Check.Wrong, 0);
			TestTrue(FString::Printf(TEXT("Planet, %s: the line moves some voxels' ground by more than a voxel"), Case.Name), Check.Moved > 200);
			TestTrue(FString::Printf(TEXT("Planet, %s: part of the chunk is beyond the reach"), Case.Name), Check.Untouched > 1000);
			TestEqual(FString::Printf(TEXT("Planet, %s: beyond the reach the chunk is byte for byte the natural one"), Case.Name), Check.UntouchedDiffering, 0);
			TestEqual(FString::Printf(TEXT("Planet, %s: on the core the altitude is the blend's"), Case.Name),
				Conditioning.ApplyToAltitude(Site, R, NaturalAtSite), NaturalAtSite + Case.Offset, 1e-2f);
		}
		Config->RemoveFromRoot();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineBlockSurfaceTest,
	"VoxelWorlds.Generation.ConditioningLine.BlockSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineBlockSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelConditioningLineTestUtils;

	// A block world (cubic meshing on a plane): a line along X whose target climbs one block every four columns. The
	// block tops on its core are the target's block tops, column by column: the topmost solid voxel of the generated
	// chunk is the layer FVoxelBlockSurface gives for the target, and adjacent columns differ by at most one block.
	UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::InfinitePlane);
	Config->MeshingMode = EMeshingMode::Cubic;
	TestTrue(TEXT("The world is a block world"), FVoxelBlockSurface::UsesBlockSurface(*Config));
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVector Origin = Config->WorldOrigin;
	const double ChunkWorldSize = ChunkSize * VoxelSize;
	const FIntVector Chunk(1, 2, 0);
	const FVector ChunkOrigin = Origin + FVector(Chunk) * ChunkWorldSize;
	const double LineY = ChunkOrigin.Y + 0.5 * ChunkWorldSize;
	const float TargetAtStart = static_cast<float>(ChunkOrigin.Z) + 8.4f * VoxelSize;
	const float Grade = 0.25f;
	FVoxelConditioning Conditioning;
	Conditioning.Lines.Add(FVoxelConditioningLine::Make(MakeStraight(FVector2D(ChunkOrigin.X - 600.0, LineY), FVector2D(ChunkOrigin.X + ChunkWorldSize + 600.0, LineY),
		TargetAtStart - 600.0f * Grade, TargetAtStart + static_cast<float>(ChunkWorldSize + 600.0) * Grade, 3.0f * VoxelSize, 4.0f * VoxelSize)));

	FVoxelConditioningLineLimits Limits;
	Limits.VoxelSize = VoxelSize;
	Limits.bBlockWorld = true;
	TestEqual(TEXT("The line keeps the block world's limit"), Conditioning.Lines[0]->GetWarnings(Limits).Num(), 0);

	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();
	FVoxelNoiseGenerationRequest Request = MakeRequest(*Config, Chunk);
	Request.Conditioning = Conditioning;
	TArray<FVoxelData> Data;
	Generator.GenerateChunkCPU(Request, Data);
	Generator.Shutdown();

	int32 Columns = 0;
	int32 WrongTops = 0;
	int32 LargestStep = 0;
	const int32 CoreY0 = ChunkSize / 2 - 2;
	const int32 CoreY1 = ChunkSize / 2 + 2;
	TArray<int32> Tops;
	for (int32 Y = CoreY0; Y <= CoreY1; ++Y)
	{
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			// The column's target, sampled where the generator samples the column: its lower corner.
			const double WorldX = ChunkOrigin.X + X * VoxelSize;
			const double WorldY = ChunkOrigin.Y + Y * VoxelSize;
			const float Natural = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector(WorldX, WorldY, 0.0), Config->NoiseParams).Position.Z);
			const float Height = Conditioning.ApplyToHeight(WorldX, WorldY, Natural);
			const float Target = TargetAtStart + static_cast<float>(X * VoxelSize) * Grade;
			TestEqual(FString::Printf(TEXT("Column (%d, %d) is on the core: its height is the target"), X, Y), Height, Target, 0.05f);
			const int32 ExpectedTop = static_cast<int32>(FVoxelBlockSurface::TopSolidLayerForHeight(Height, ChunkOrigin.Z, VoxelSize));
			int32 Top = -1;
			for (int32 Z = ChunkSize - 1; Z >= 0; --Z)
			{
				if (Data[X + Y * ChunkSize + Z * ChunkSize * ChunkSize].Density >= VOXEL_SURFACE_THRESHOLD)
				{
					Top = Z;
					break;
				}
			}
			++Columns;
			WrongTops += Top != ExpectedTop ? 1 : 0;
			Tops.Add(Top);
		}
	}
	// Adjacent columns, diagonals included.
	const int32 Rows = CoreY1 - CoreY0 + 1;
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		for (int32 X = 0; X + 1 < ChunkSize; ++X)
		{
			LargestStep = FMath::Max(LargestStep, FMath::Abs(Tops[Row * ChunkSize + X + 1] - Tops[Row * ChunkSize + X]));
			if (Row + 1 < Rows)
			{
				LargestStep = FMath::Max(LargestStep, FMath::Abs(Tops[(Row + 1) * ChunkSize + X + 1] - Tops[Row * ChunkSize + X]));
				LargestStep = FMath::Max(LargestStep, FMath::Abs(Tops[(Row + 1) * ChunkSize + X] - Tops[Row * ChunkSize + X + 1]));
			}
		}
	}
	AddInfo(FString::Printf(TEXT("Block world: %d core columns, %d with another top than the target's; tops from layer %d to %d; the largest step between adjacent columns is %d block(s)"),
		Columns, WrongTops, Tops[0], Tops.Last(), LargestStep));
	TestEqual(TEXT("The block tops along the line are the target's block tops"), WrongTops, 0);
	TestTrue(TEXT("The line climbs through the chunk"), Tops.Last() - Tops[0] >= 6);
	TestTrue(TEXT("Adjacent block tops along the line differ by at most one block"), LargestStep <= 1);
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
