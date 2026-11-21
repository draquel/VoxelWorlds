// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelConditioningLine.h"
#include "VoxelConditioningLineShape.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A river's profile (the hydrology plan's H4, section 5.3): FVoxelConditioningLineShape::MakeRiverProfile on made
// ground. A line of 61 vertices 150 uu apart down a slope of 2 %, the banks at the line's own height unless a case
// says otherwise.
//
// - Generation.RiverProfile.Rules: a river that follows the ground lies its margin under it, with a head and an end on
//   land that start from and thin to nothing; a low bank lowers the surface there and from there on and nothing
//   upstream; a rise of the ground is cut through; given heights that rise are lowered and counted; a mouth comes down
//   to a tuck under the body's level and keeps a few vertices inside it; a head in a lake starts at the lake's level
//   with no taper; a floor under the surface (the sea's level less the tuck) holds. In every case the surface never
//   rises, lies under the banks by the margin outside another body, and the bed lies under it by the depth times the
//   taper.
// - Generation.RiverProfile.Bends: on a steep slope through a curve the bed's grade at each vertex that turns is
//   within what the bend bound leaves there, by a deeper bed and an unchanged surface; with no voxel size given the
//   bed is left as it was; a line made from the profile passes the engine's own bend check; a bed is deepened by no
//   more than the share of its depth allowed, so a head still starts from nothing.
// ---------------------------------------------------------------------------

namespace VoxelRiverProfileTest
{
	using FShape = FVoxelConditioningLineShape;
	constexpr int32 Num = 61;
	constexpr double Spacing = 150.0;
	constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

	struct FCase
	{
		TArray<FVector> Points;
		TArray<double> Ground;
		TArray<double> Bank;
		TArray<double> Heights;
		TArray<double> Bodies;
		FShape::FRiverProfileParams Params;

		FCase()
		{
			for (int32 Index = 0; Index < Num; ++Index)
			{
				Points.Add(FVector(Index * Spacing, 0.0, 0.0));
				Ground.Add(1000.0 - 0.02 * Index * Spacing);
			}
			Bank = Ground;
		}

		FShape::FRiverProfile Make() const { return FShape::MakeRiverProfile(Points, Ground, Bank, Heights, Bodies, Params); }
	};

	/** The rules every profile keeps, whatever the case. */
	void CheckInvariants(FAutomationTestBase& Test, const TCHAR* Name, const FCase& Case, const FShape::FRiverProfile& Profile)
	{
		int32 Rises = 0, OverBank = 0, BedOver = 0, BadTaper = 0;
		for (int32 Index = 0; Index < Profile.Surface.Num(); ++Index)
		{
			Rises += Index > 0 && Profile.Surface[Index] > Profile.Surface[Index - 1] + 1.0e-6 ? 1 : 0;
			const bool bInBody = Case.Bodies.Num() > 0 && !FMath::IsNaN(Case.Bodies[Index]);
			OverBank += !bInBody && Profile.Surface[Index] > Case.Bank[Index] - Case.Params.BankMargin + 1.0e-6 ? 1 : 0;
			BedOver += Profile.Bed[Index] > Profile.Surface[Index] - Case.Params.Depth * Profile.Taper[Index] + 1.0e-6 ? 1 : 0;
			BadTaper += Profile.Taper[Index] < 0.0 || Profile.Taper[Index] > 1.0 ? 1 : 0;
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: a value per vertex"), Name), Profile.Surface.Num(), Num);
		Test.TestEqual(*FString::Printf(TEXT("%s: the surface never rises downstream (rule 1)"), Name), Rises, 0);
		Test.TestEqual(*FString::Printf(TEXT("%s: the surface lies the margin under both banks outside another body (rule 2)"), Name), OverBank, 0);
		Test.TestEqual(*FString::Printf(TEXT("%s: the bed lies the depth times the taper under the surface, or deeper"), Name), BedOver, 0);
		Test.TestEqual(*FString::Printf(TEXT("%s: the taper is a share"), Name), BadTaper, 0);
		Test.TestTrue(*FString::Printf(TEXT("%s: the vertices kept are the profile's"), Name), Profile.First >= 0 && Profile.Num >= 2 && Profile.First + Profile.Num <= Num);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRiverProfileRulesTest,
	"VoxelWorlds.Generation.RiverProfile.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelRiverProfileRulesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelRiverProfileTest;

	// 1. Following the ground down an even slope: its margin under the ground, a head, an end on land.
	{
		FCase Case;
		const FShape::FRiverProfile Profile = Case.Make();
		CheckInvariants(*this, TEXT("Slope"), Case, Profile);
		TestTrue(TEXT("Slope: the surface lies the margin under the ground along the line"),
			FMath::IsNearlyEqual(Profile.Surface[30], Case.Ground[30] - Case.Params.SurfaceUnderGround, 0.01));
		TestTrue(TEXT("Slope: at full depth the bed lies the depth under the surface"), FMath::IsNearlyEqual(Profile.Bed[30], Profile.Surface[30] - Case.Params.Depth, 0.01));
		TestTrue(TEXT("Slope: the head starts from nothing (rule 4)"), Profile.Taper[0] == 0.0 && FMath::IsNearlyEqual(Profile.Bed[0], Profile.Surface[0], 0.01));
		TestTrue(TEXT("Slope: ... is half way at half the head's length"), FMath::IsNearlyEqual(Profile.Taper[2], 0.5, 0.01));
		TestTrue(TEXT("Slope: ... and full from the head's length on"), Profile.Taper[4] == 1.0 && Profile.Taper[30] == 1.0);
		TestFalse(TEXT("Slope: the river starts in no body"), Profile.bStartsInBody);
		TestFalse(TEXT("Slope: the river ends on land (rule 3)"), Profile.bEndsInBody);
		TestTrue(TEXT("Slope: ... and thins to nothing over its end"), Profile.Taper[Num - 1] == 0.0 && FMath::IsNearlyEqual(Profile.Taper[Num - 3], 0.5, 0.01) && Profile.Taper[Num - 5] == 1.0);
		TestEqual(TEXT("Slope: every vertex is kept"), Profile.Num, Num);
		TestEqual(TEXT("Slope: no rise, on a river that follows the ground"), Profile.NumRises, 0);
		TestEqual(TEXT("Slope: no low bank"), Profile.NumLowBanks, 0);
		TestTrue(TEXT("Slope: the deepest cut is the margin and the depth"), FMath::IsNearlyEqual(Profile.DeepestCut, Case.Params.SurfaceUnderGround + Case.Params.Depth, 0.5));
		TestTrue(TEXT("Slope: the fall is the ground's"), FMath::IsNearlyEqual(Profile.Fall, Case.Ground[0] - Case.Ground[Num - 1], 1.0));
	}

	// 2. A low bank: the ground beside the river 60 under the line's at vertices 20 and 21.
	{
		FCase Plain;
		const FShape::FRiverProfile Before = Plain.Make();
		FCase Case;
		Case.Bank[20] -= 60.0;
		Case.Bank[21] -= 60.0;
		const FShape::FRiverProfile Profile = Case.Make();
		CheckInvariants(*this, TEXT("Low bank"), Case, Profile);
		TestTrue(TEXT("Low bank: the surface is the margin under the low bank there"), FMath::IsNearlyEqual(Profile.Surface[20], Case.Bank[20] - Case.Params.BankMargin, 0.01)
			&& FMath::IsNearlyEqual(Profile.Surface[21], Case.Bank[21] - Case.Params.BankMargin, 0.01));
		TestTrue(TEXT("Low bank: ... and stays there from there on, until the ground has come down to it"),
			FMath::IsNearlyEqual(Profile.Surface[24], Profile.Surface[21], 0.01) && Profile.Surface[24] < Before.Surface[24] - 10.0);
		TestTrue(TEXT("Low bank: downstream of that the river is back under its own ground"), FMath::IsNearlyEqual(Profile.Surface[40], Before.Surface[40], 0.01));
		TestTrue(TEXT("Low bank: nothing upstream of it moved"), FMath::IsNearlyEqual(Profile.Surface[19], Before.Surface[19], 0.01));
		TestTrue(TEXT("Low bank: the vertices lowered for it are counted"), Profile.NumLowBanks == 2 && FMath::IsNearlyEqual(Profile.MostLowBank, Before.Surface[20] - Profile.Surface[20], 0.5));
		TestTrue(TEXT("Low bank: the bed is cut deeper there"), Profile.DeepestCut > Before.DeepestCut + 15.0);
	}

	// 3. A rise of the ground along the line: a hump 400 high over vertices 30 to 35. The river cuts through it.
	{
		FCase Case;
		for (int32 Index = 30; Index <= 35; ++Index)
		{
			Case.Ground[Index] += 400.0;
			Case.Bank[Index] += 400.0;
		}
		Case.Params.SmoothingDistance = 0.0;
		const FShape::FRiverProfile Profile = Case.Make();
		CheckInvariants(*this, TEXT("Rise"), Case, Profile);
		TestTrue(TEXT("Rise: the surface through the hump is no higher than before it"), Profile.Surface[32] <= Profile.Surface[29]);
		TestTrue(TEXT("Rise: the bed is cut through the hump"), Profile.DeepestCut > 400.0 + Case.Params.Depth);
		TestEqual(TEXT("Rise: a river that follows the ground has no rise to report"), Profile.NumRises, 0);
	}

	// 4. Heights given that rise: the ground's, a margin under it, but 100 higher over vertices 25 to 30.
	{
		FCase Case;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Case.Heights.Add(Case.Ground[Index] - 75.0 + (Index >= 25 && Index <= 30 ? 100.0 : 0.0));
		}
		const FShape::FRiverProfile Profile = Case.Make();
		CheckInvariants(*this, TEXT("Heights"), Case, Profile);
		TestEqual(TEXT("Heights: the six vertices that stand over one upstream of them are counted"), Profile.NumRises, 6);
		TestTrue(TEXT("Heights: ... by the most any of them rose"), Profile.MostRise > 80.0 && Profile.MostRise <= 100.0);
		TestTrue(TEXT("Heights: they are lowered to the vertex before the rise"), FMath::IsNearlyEqual(Profile.Surface[25], Profile.Surface[24], 0.01));
		int32 Over = 0;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Over += Profile.Surface[Index] > Case.Heights[Index] + 1.0e-6 ? 1 : 0;
		}
		TestEqual(TEXT("Heights: the surface is never over the height given"), Over, 0);
		TestTrue(TEXT("Heights: off the rise the surface is the height given"), FMath::IsNearlyEqual(Profile.Surface[10], Case.Heights[10], 0.01) && FMath::IsNearlyEqual(Profile.Surface[50], Case.Heights[50], 0.01));
	}

	// 5. Heights given over the banks: lowered, and counted as low banks.
	{
		FCase Case;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Case.Heights.Add(Case.Ground[Index] + 50.0);
		}
		const FShape::FRiverProfile Profile = Case.Make();
		CheckInvariants(*this, TEXT("Over the banks"), Case, Profile);
		TestEqual(TEXT("Over the banks: every vertex is lowered for its bank"), Profile.NumLowBanks, Num);
		TestTrue(TEXT("Over the banks: by the height over the bank and the margin"), FMath::IsNearlyEqual(Profile.MostLowBank, 50.0 + Case.Params.BankMargin, 0.01));
	}

	// 6. A mouth: the last eleven vertices lie in the sea, at level 100.
	{
		FCase Case;
		Case.Bodies.Init(NaN, Num);
		for (int32 Index = 50; Index < Num; ++Index)
		{
			Case.Bodies[Index] = 100.0;
			Case.Ground[Index] = 40.0;
			Case.Bank[Index] = 40.0;
		}
		// The ground comes down to the sea: the last dry vertices stand a little over it.
		for (int32 Index = 40; Index < 50; ++Index)
		{
			Case.Ground[Index] = 100.0 + 12.0 * (50 - Index) + 80.0;
			Case.Bank[Index] = Case.Ground[Index];
		}
		Case.Params.SmoothingDistance = 0.0;
		const FShape::FRiverProfile Profile = Case.Make();
		CheckInvariants(*this, TEXT("Mouth"), Case, Profile);
		TestTrue(TEXT("Mouth: the river ends in a body"), Profile.bEndsInBody);
		TestTrue(TEXT("Mouth: inside the body the surface lies a tuck under its level (rules 5 and 7)"), FMath::IsNearlyEqual(Profile.Surface[50], 100.0 - Case.Params.BodyTuck, 0.01));
		TestEqual(TEXT("Mouth: of the vertices inside the body the few next to the land are kept"), Profile.First + Profile.Num, 50 + Case.Params.BodyVertices);
		TestTrue(TEXT("Mouth: the river does not thin out at a mouth"), Profile.Taper[49] == 1.0 && Profile.Taper[Profile.First + Profile.Num - 1] == 1.0);
		TestTrue(TEXT("Mouth: upstream the river stands over the body's level"), Profile.Surface[45] > 100.0);
		TestTrue(TEXT("Mouth: the cut is measured on land alone"), Profile.DeepestCut < Case.Params.SurfaceUnderGround + Case.Params.Depth + 30.0);

		// With the ground averaged along the line the sea's floor does not pull the river under the sea before the shore:
		// the average takes the sea's level there.
		Case.Params.SmoothingDistance = 600.0;
		const FShape::FRiverProfile Smoothed = Case.Make();
		CheckInvariants(*this, TEXT("Mouth, averaged"), Case, Smoothed);
		TestTrue(TEXT("Mouth, averaged: the last vertex on land stands over the sea's level"), Smoothed.Surface[49] > 100.0);
		TestTrue(TEXT("Mouth, averaged: inside the sea the surface is a tuck under it"), FMath::IsNearlyEqual(Smoothed.Surface[50], 100.0 - Case.Params.BodyTuck, 0.01));
	}

	// 7. A head in a lake at level 900: the first eight vertices lie in it, its bed 200 under its level, its rim over it.
	{
		FCase Case;
		Case.Bodies.Init(NaN, Num);
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Case.Bodies[Index] = 900.0;
			Case.Ground[Index] = 700.0;
			Case.Bank[Index] = 700.0;
		}
		for (int32 Index = 8; Index < 12; ++Index)
		{
			Case.Ground[Index] = 960.0; // the rim
			Case.Bank[Index] = 960.0;
		}
		for (int32 Index = 12; Index < Num; ++Index)
		{
			Case.Ground[Index] = 940.0 - 0.02 * (Index - 12) * Spacing;
			Case.Bank[Index] = Case.Ground[Index];
		}
		Case.Params.SmoothingDistance = 0.0;
		const FShape::FRiverProfile Profile = Case.Make();
		CheckInvariants(*this, TEXT("Lake head"), Case, Profile);
		TestTrue(TEXT("Lake head: the river starts in a body"), Profile.bStartsInBody);
		TestEqual(TEXT("Lake head: of the vertices inside the lake the few next to the rim are kept"), Profile.First, 8 - Case.Params.BodyVertices);
		TestTrue(TEXT("Lake head: it starts a tuck under the lake's level (rule 6)"), FMath::IsNearlyEqual(Profile.Surface[Profile.First], 900.0 - Case.Params.BodyTuck, 0.01));
		TestTrue(TEXT("Lake head: at full width and depth: no head"), Profile.Taper[Profile.First] == 1.0);
		TestTrue(TEXT("Lake head: through the rim the surface stays at or under the lake's"), Profile.Surface[9] <= 900.0 - Case.Params.BodyTuck);
		TestTrue(TEXT("Lake head: ... and the bed notches the rim"), Profile.Bed[9] < 960.0 - 200.0 + 0.01);
	}

	// 8. A floor under the surface (the sea's level less the tuck): a river down a beach goes no lower than it.
	{
		FCase Case;
		Case.Params.MinSurface = 900.0;
		const FShape::FRiverProfile Profile = Case.Make();
		int32 Under = 0, Rises = 0;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Under += Profile.Surface[Index] < 900.0 - 1.0e-6 ? 1 : 0;
			Rises += Index > 0 && Profile.Surface[Index] > Profile.Surface[Index - 1] + 1.0e-6 ? 1 : 0;
		}
		TestEqual(TEXT("Floor: no surface lies under it"), Under, 0);
		TestEqual(TEXT("Floor: the surface still never rises"), Rises, 0);
		TestTrue(TEXT("Floor: over it the river is under its ground as before"), FMath::IsNearlyEqual(Profile.Surface[5], Case.Ground[5] - Case.Params.SurfaceUnderGround, 0.01));
		TestTrue(TEXT("Floor: where the ground has come down past it the surface is the floor (non-vacuous)"), Case.Ground[Num - 1] - Case.Params.SurfaceUnderGround < 900.0 && Profile.Surface[Num - 1] == 900.0);
	}

	// 9. Too little to make a river of.
	{
		FCase Case;
		Case.Points.SetNum(1);
		const FShape::FRiverProfile Profile = Case.Make();
		TestEqual(TEXT("One vertex: no profile"), Profile.Num, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRiverProfileBendsTest,
	"VoxelWorlds.Generation.RiverProfile.Bends",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelRiverProfileBendsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelRiverProfileTest;

	// A line that runs straight, turns 12 degrees a vertex for eight vertices and runs straight again, down a slope of
	// 20 %: steeper than a 12 degree turn leaves a bed 6 voxels wide with banks of 4 (voxels of 75 uu).
	FCase Case;
	Case.Points.Reset();
	Case.Ground.Reset();
	FVector At = FVector::ZeroVector;
	double Heading = 0.0;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		Case.Points.Add(At);
		Case.Ground.Add(4000.0 - 0.2 * Index * Spacing);
		if (Index >= 25 && Index < 33)
		{
			Heading += FMath::DegreesToRadians(12.0);
		}
		At += FVector(FMath::Cos(Heading), FMath::Sin(Heading), 0.0) * Spacing;
	}
	Case.Bank = Case.Ground;
	Case.Params.HalfWidth = 225.0f;
	Case.Params.FalloffWidth = 300.0f;

	const FShape::FRiverProfile Free = Case.Make();
	TestEqual(TEXT("No voxel size: the bed is left as the surface less the depth"), Free.NumBendHolds, 0);

	Case.Params.VoxelSize = 75.0;
	const FShape::FRiverProfile Held = Case.Make();
	CheckInvariants(*this, TEXT("Bends"), Case, Held);
	TestTrue(TEXT("Bends: beds were lowered to hold the grade through the bend (non-vacuous)"), Held.NumBendHolds > 0 && Held.MostBendHold > 10.0);

	int32 Steeper = 0, SurfaceMoved = 0, BedRaised = 0;
	double WorstRatio = 0.0;
	for (int32 Index = 0; Index + 1 < Num; ++Index)
	{
		double Allowed = TNumericLimits<double>::Max();
		for (const int32 Vertex : { Index, Index + 1 })
		{
			const double Turn = FShape::GetTurnAt(Case.Points, Vertex);
			if (Turn > UE_DOUBLE_SMALL_NUMBER)
			{
				Allowed = FMath::Min(Allowed, FShape::GetMaxGradeForTurn(Turn, Case.Params.HalfWidth, Case.Params.FalloffWidth, Case.Params.VoxelSize));
			}
		}
		const double Grade = FMath::Abs(Held.Bed[Index + 1] - Held.Bed[Index]) / Spacing;
		if (Allowed < TNumericLimits<double>::Max())
		{
			WorstRatio = FMath::Max(WorstRatio, Grade / Allowed);
			Steeper += Grade > Allowed + 1.0e-6 ? 1 : 0;
		}
	}
	for (int32 Index = 0; Index < Num; ++Index)
	{
		SurfaceMoved += FMath::IsNearlyEqual(Held.Surface[Index], Free.Surface[Index], 1.0e-6) ? 0 : 1;
		BedRaised += Held.Bed[Index] > Free.Bed[Index] + 1.0e-6 ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("Bends: %d beds lowered, by up to %.0f uu; the steepest segment at a vertex that turns is %.2f of what the bound leaves there"),
		Held.NumBendHolds, Held.MostBendHold, WorstRatio));
	TestEqual(TEXT("Bends: no segment at a vertex that turns is steeper than the bend bound leaves there (rule 8)"), Steeper, 0);
	TestEqual(TEXT("Bends: the surface is where it was: the water there is deeper"), SurfaceMoved, 0);
	TestEqual(TEXT("Bends: a bed is only ever lowered"), BedRaised, 0);

	// The line made from the held bed passes the engine's own check of the bend bound; the one made from the free bed
	// does not.
	const auto BreaksBound = [&](const FShape::FRiverProfile& Profile)
	{
		FVoxelConditioningLineParams Line;
		Line.Blend = EVoxelConditioningLineBlend::LowerOnly;
		Line.FalloffWidth = Case.Params.FalloffWidth;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Line.Vertices.Add(FVoxelConditioningLineVertex(Case.Points[Index], static_cast<float>(Profile.Bed[Index]), Case.Params.HalfWidth));
		}
		FVoxelConditioningLineLimits Limits;
		Limits.VoxelSize = 75.0;
		return FVoxelConditioningLine::Make(Line)->GetWarnings(Limits).ContainsByPredicate([](const FString& Warning) { return Warning.Contains(TEXT("bend bound")); });
	};
	TestTrue(TEXT("Bends: the bed as the surface less the depth breaks the bend bound (non-vacuous)"), BreaksBound(Free));
	TestFalse(TEXT("Bends: the held bed keeps it"), BreaksBound(Held));

	// A bed is deepened by no more than a share of the depth there: with a quarter allowed the bend keeps part of its
	// step, the line says so, and the head (no depth) is not deepened at all.
	Case.Params.MaxBendDeepening = 0.25;
	const FShape::FRiverProfile Capped = Case.Make();
	int32 TooDeep = 0;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		TooDeep += Free.Bed[Index] - Capped.Bed[Index] > 0.25 * Case.Params.Depth * Capped.Taper[Index] + 1.0e-6 ? 1 : 0;
	}
	TestEqual(TEXT("Capped: no bed is deepened by more than the share of its depth"), TooDeep, 0);
	TestTrue(TEXT("Capped: beds were deepened as far as allowed (non-vacuous)"), Capped.NumBendHolds > 0 && FMath::IsNearlyEqual(Capped.MostBendHold, 0.25 * Case.Params.Depth, 0.01));
	TestTrue(TEXT("Capped: the line keeps its bend warning"), BreaksBound(Capped));
	TestTrue(TEXT("Capped: the head still starts from nothing"), Capped.Taper[0] == 0.0 && FMath::IsNearlyEqual(Capped.Bed[0], Capped.Surface[0], 1.0e-6));
	TestTrue(TEXT("Bends: the held bed's head starts from nothing too"), FMath::IsNearlyEqual(Held.Bed[0], Held.Surface[0], 1.0e-6));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
