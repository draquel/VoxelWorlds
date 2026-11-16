// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSeaSurface.h"
#include "VoxelWaterLevels.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Lakes and river reaches in the water levels (the hydrology plan's H1, section 5.1): a body is a record with a
// footprint, and the snapshot's sample, range and cheap forms evaluate it.
//
// - Core.WaterLevels.Lake: a lake's footprint (cells, a hole, an outline grown by a cell) on a plane and a planet. The
//   sample answers with the lake inside its footprint and with the sea (or nothing) outside it and in the hole; a pond
//   inside a larger lake answers where it stands higher, and a lake under the sea's level never answers; the cheap
//   forms agree with the sample; the range over a box the footprint reaches is not uniform and holds both levels, and a
//   box away from it is the sea's own, uniform.
// - Core.WaterLevels.Reach: a river reach on a plane (a straight run and a bend) and a planet. Its level is the surface
//   level interpolated along the nearest segment: level across the river, falling along it; outside the half width it
//   does not answer; the normal tilts against the slope and the flow runs downstream at the reach's speed. At a join the
//   higher level answers, and a reach that enters a lake hands over where the lake's level is reached.
// ---------------------------------------------------------------------------

namespace VoxelWaterBodiesTestUtils
{
	/** A square footprint of Cells x Cells cells, all set but the ones Hole says are not. */
	TBitArray<> SquareCells(int32 Cells, TFunctionRef<bool(int32 X, int32 Y)> Hole)
	{
		TBitArray<> Bits(false, Cells * Cells);
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				Bits[Y * Cells + X] = !Hole(X, Y);
			}
		}
		return Bits;
	}

	bool NoHole(int32, int32) { return false; }

	/** The cheap forms against the sample at a point. */
	bool CheapFormsAgree(const FVoxelWaterLevels& Levels, const FVector& P)
	{
		FVoxelWaterSample Sample;
		const bool bWater = Levels.Sample(P, Sample);
		const double Depth = Levels.GetDepth(P);
		return (bWater ? FMath::IsNearlyEqual(Depth, Sample.Depth, 1.0e-6) : Depth == FVoxelWaterSample::NoWaterDepth)
			&& Levels.IsUnder(P) == (bWater && Sample.Depth >= 0.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterLevelsLakeTest,
	"VoxelWorlds.Core.WaterLevels.Lake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterLevelsLakeTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterBodiesTestUtils;

	// ---- A plane: the sea at -1,500, a lake at 400 over a 10 x 10 footprint of 200 uu cells with a hole, a pond at 650
	// inside it, and a lake under the sea's level elsewhere.
	{
		const FVoxelSeaSurface Sea = FVoxelSeaSurface::MakePlane(-1500.0);
		FVoxelWaterFrame Frame;
		const FVector2D LakeOrigin(1000.0, 2000.0);
		const TSharedRef<const FVoxelWaterLake> LakeCells = FVoxelWaterLake::Make(LakeOrigin, 200.0, FIntPoint(10, 10),
			SquareCells(10, [](int32 X, int32 Y) { return X == 7 && Y == 7; }));
		const TSharedRef<const FVoxelWaterLake> PondCells = FVoxelWaterLake::Make(LakeOrigin + FVector2D(400.0, 400.0), 200.0, FIntPoint(2, 2), SquareCells(2, NoHole));
		const TSharedRef<const FVoxelWaterLake> LowCells = FVoxelWaterLake::Make(FVector2D(-5000.0, -5000.0), 200.0, FIntPoint(4, 4), SquareCells(4, NoHole));
		TArray<FVoxelWaterBody> Bodies;
		Bodies.Add(FVoxelWaterBody::MakeLake(7, 400.0, LakeCells, Frame, 300.0));
		Bodies.Add(FVoxelWaterBody::MakeLake(3, 650.0, PondCells, Frame, 100.0));
		Bodies.Add(FVoxelWaterBody::MakeLake(9, -2000.0, LowCells, Frame, 100.0));
		// A body gathered twice is kept once.
		const FVoxelWaterBody GatheredTwice = Bodies[0];
		Bodies.Add(GatheredTwice);
		const FVoxelWaterLevels Levels = FVoxelWaterLevels::Make(Sea, Frame, FVoxelWaterLevels::WholeWorld(), Bodies);

		TestTrue(TEXT("Plane: the snapshot holds the sea and three bodies, by id"),
			Levels.GetBodies().Num() == 4 && !Levels.IsSeaAlone() && Levels.HasWater() && !Levels.IsRadial()
			&& Levels.GetBodies()[0].Kind == EVoxelWaterBodyKind::Sea && Levels.GetBodies()[1].Id == 3 && Levels.GetBodies()[2].Id == 7 && Levels.GetBodies()[3].Id == 9);
		TestEqual(TEXT("Plane: the footprint's cells, less the hole"), LakeCells->NumCells(), 99);
		TestTrue(TEXT("Plane: a lake's bounds are its footprint's box, from its depth under the level up to it"),
			Levels.GetBodies()[2].Bounds.Min.Equals(FVector(1000.0, 2000.0, 100.0)) && Levels.GetBodies()[2].Bounds.Max.Equals(FVector(3000.0, 4000.0, 400.0)));

		FVoxelWaterSample S;
		// In the lake, 150 under its level.
		const FVector InLake(LakeOrigin.X + 1700.0, LakeOrigin.Y + 300.0, 250.0);
		TestTrue(TEXT("Plane: a column of the footprint answers with the lake"),
			Levels.Sample(InLake, S) && S.bWater && S.Kind == EVoxelWaterBodyKind::Lake && S.BodyId == 7 && S.SurfaceLevel == 400.0
			&& FMath::IsNearlyEqual(S.Depth, 150.0) && S.SurfaceNormal.Equals(FVector::UpVector) && S.Flow.IsZero());
		// Above the lake: the lake still covers the column.
		TestTrue(TEXT("Plane: above the lake the column is the lake's, the depth negative"),
			Levels.Sample(InLake + FVector(0.0, 0.0, 500.0), S) && S.Kind == EVoxelWaterBodyKind::Lake && FMath::IsNearlyEqual(S.Depth, -350.0));
		// The hole, and outside the footprint: the sea.
		const FVector InHole(LakeOrigin.X + 7.5 * 200.0, LakeOrigin.Y + 7.5 * 200.0, 0.0);
		TestTrue(TEXT("Plane: the hole in the footprint is the sea's"),
			Levels.Sample(InHole, S) && S.Kind == EVoxelWaterBodyKind::Sea && S.SurfaceLevel == -1500.0 && FMath::IsNearlyEqual(S.Depth, -1500.0));
		TestTrue(TEXT("Plane: a column outside the footprint is the sea's"),
			Levels.Sample(FVector(LakeOrigin.X - 50.0, LakeOrigin.Y + 300.0, -1600.0), S) && S.Kind == EVoxelWaterBodyKind::Sea && FMath::IsNearlyEqual(S.Depth, 100.0));
		// The pond inside the lake stands higher: it answers over its own cells.
		const FVector InPond(LakeOrigin.X + 500.0, LakeOrigin.Y + 700.0, 600.0);
		TestTrue(TEXT("Plane: a pond inside the lake answers where it stands higher"),
			Levels.Sample(InPond, S) && S.BodyId == 3 && S.SurfaceLevel == 650.0 && FMath::IsNearlyEqual(S.Depth, 50.0));
		// A lake under the sea's level never answers.
		TestTrue(TEXT("Plane: a lake under the sea's level leaves the column to the sea"),
			Levels.Sample(FVector(-4900.0, -4900.0, -1800.0), S) && S.Kind == EVoxelWaterBodyKind::Sea && FMath::IsNearlyEqual(S.Depth, 300.0));

		int32 CheapMismatches = 0;
		FRandomStream Random(4411);
		for (int32 i = 0; i < 4000; ++i)
		{
			const FVector P(Random.FRandRange(500.0f, 3500.0f), Random.FRandRange(1500.0f, 4500.0f), Random.FRandRange(-2000.0f, 900.0f));
			CheapMismatches += CheapFormsAgree(Levels, P) ? 0 : 1;
		}
		TestEqual(TEXT("Plane: GetDepth and IsUnder agree with the sample (mismatches)"), CheapMismatches, 0);

		// The range: not uniform where a footprint may reach, with both levels; the sea's own away from every body.
		const FVoxelWaterLevelRange Over = Levels.GetRange(FBox(FVector(2800.0, 3800.0, -100.0), FVector(3400.0, 4400.0, 100.0)));
		TestTrue(TEXT("Plane: the range over a box the lake reaches holds the sea's and the lake's level, not uniform"),
			Over.bAny && !Over.bUniform && Over.MinLevel == -1500.0 && Over.MaxLevel == 400.0);
		const FVoxelWaterLevelRange OverPond = Levels.GetRange(FBox(FVector(1450.0, 2450.0, 0.0), FVector(1550.0, 2550.0, 10.0)));
		TestTrue(TEXT("Plane: the range over the pond reaches the pond's level"), !OverPond.bUniform && OverPond.MaxLevel == 650.0);
		const FVoxelWaterLevelRange Away = Levels.GetRange(FBox(FVector(8000.0, 8000.0, -100.0), FVector(9000.0, 9000.0, 100.0)));
		TestTrue(TEXT("Plane: the range away from every body is the sea's, uniform"), Away.bAny && Away.bUniform && Away.MinLevel == -1500.0 && Away.MaxLevel == -1500.0);
		// A box that only the hole's cell holds: no cell of the footprint overlaps it.
		const FVoxelWaterLevelRange HoleRange = Levels.GetRange(FBox(FVector(InHole.X - 50.0, InHole.Y - 50.0, 0.0), FVector(InHole.X + 50.0, InHole.Y + 50.0, 1.0)));
		TestTrue(TEXT("Plane: the range inside the hole is the sea's, uniform"), HoleRange.bUniform && HoleRange.MaxLevel == -1500.0);

		// The box tests: above the lake's level over the footprint, across it, and over dry columns.
		TestTrue(TEXT("Plane: a box over the lake's level is above the water"), Levels.IsAbove(FBox(FVector(1900.0, 2900.0, 450.0), FVector(2000.0, 3000.0, 500.0))));
		TestFalse(TEXT("Plane: the same heights over the pond, which stands higher, are not"), Levels.IsAbove(FBox(FVector(1500.0, 2500.0, 450.0), FVector(1600.0, 2600.0, 500.0))));
		TestFalse(TEXT("Plane: a box that reaches under the lake's level is not"), Levels.IsAbove(FBox(FVector(1900.0, 2900.0, 350.0), FVector(2000.0, 3000.0, 500.0))));
		TestTrue(TEXT("Plane: ... and the lake's surface crosses it"), Levels.Crosses(FBox(FVector(1900.0, 2900.0, 350.0), FVector(2000.0, 3000.0, 500.0))));
		TestTrue(TEXT("Plane: the same heights away from the lake are above the sea"), Levels.IsAbove(FBox(FVector(8000.0, 8000.0, 350.0), FVector(8100.0, 8100.0, 500.0))));

		// With the sea off the lake is the only water.
		const FVoxelWaterLevels NoSea = FVoxelWaterLevels::Make(FVoxelSeaSurface(), Frame, FVoxelWaterLevels::WholeWorld(), Bodies);
		TestTrue(TEXT("Plane, sea off: the lake answers in its footprint"), NoSea.HasWater() && NoSea.Sample(InLake, S) && S.Kind == EVoxelWaterBodyKind::Lake);
		TestTrue(TEXT("Plane, sea off: no water outside it"),
			!NoSea.Sample(FVector(8000.0, 8000.0, -5000.0), S) && !S.bWater && S.Depth == FVoxelWaterSample::NoWaterDepth && !NoSea.IsUnder(FVector(8000.0, 8000.0, -5000.0)));
		const FVoxelWaterLevelRange NoSeaAway = NoSea.GetRange(FBox(FVector(8000.0, 8000.0, -100.0), FVector(9000.0, 9000.0, 100.0)));
		TestTrue(TEXT("Plane, sea off: an empty range away from the lake"), !NoSeaAway.bAny && NoSeaAway.bUniform);

		// No body gathered: the sea's snapshot, as before.
		const FVoxelWaterLevels SeaOnly = FVoxelWaterLevels::Make(Sea, Frame, FVoxelWaterLevels::WholeWorld(), TArray<FVoxelWaterBody>());
		TestTrue(TEXT("Plane: a gather that found no body is the sea alone"), SeaOnly.IsSeaAlone() && SeaOnly.GetBodies().Num() == 1
			&& SeaOnly.GetDepth(InLake) == Sea.GetDepth(InLake));
	}

	// ---- An outline: the cells whose centres lie inside it, grown by one cell.
	{
		const TArray<FVector2D> Outline = { FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0), FVector2D(1000.0, 600.0), FVector2D(0.0, 600.0) };
		const TSharedRef<const FVoxelWaterLake> Drawn = FVoxelWaterLake::MakeFromOutline(Outline, 100.0);
		// 10 x 6 cells inside, a ring of one cell around them: 12 x 8.
		TestEqual(TEXT("Outline: the cells inside and one cell around them"), Drawn->NumCells(), 96);
		TestTrue(TEXT("Outline: a column inside is covered"), Drawn->Covers(500.0, 300.0));
		TestTrue(TEXT("Outline: a column a cell outside the outline is covered"), Drawn->Covers(-50.0, 300.0) && Drawn->Covers(1050.0, 650.0));
		TestFalse(TEXT("Outline: a column two cells outside is not"), Drawn->Covers(-150.0, 300.0) || Drawn->Covers(500.0, 750.0));
		TestTrue(TEXT("Outline: fewer than three corners make no footprint"),
			FVoxelWaterLake::MakeFromOutline(MakeArrayView(Outline.GetData(), 2), 100.0)->NumCells() == 0);

		// The cells a filter keeps (a hand-placed lake's: the ones under its level): the left half of the outline, grown
		// by one cell. 5 x 6 cells kept, a ring around them: 7 x 8.
		int32 Asked = 0;
		const TSharedRef<const FVoxelWaterLake> Kept = FVoxelWaterLake::MakeFromOutline(Outline, 100.0, [&Asked](const FVector& Place)
		{
			++Asked;
			return Place.X < 500.0;
		});
		TestEqual(TEXT("Outline, kept cells: every cell inside the outline is asked about, at its centre's column"), Asked, 60);
		TestEqual(TEXT("Outline, kept cells: the cells kept and one cell around them"), Kept->NumCells(), 56);
		TestTrue(TEXT("Outline, kept cells: a kept column and its neighbour are covered"), Kept->Covers(450.0, 300.0) && Kept->Covers(550.0, 300.0) && Kept->Covers(-50.0, -50.0));
		TestFalse(TEXT("Outline, kept cells: a column inside the outline two cells from a kept one is not"), Kept->Covers(650.0, 300.0));
	}

	// ---- An outline on a planet: a square of 1,000 uu round a direction, as corners' directions. The footprint lies in
	// the plane tangent at the corners' middle: the cells inside, grown by one.
	{
		const double Radius = 250000.0;
		const FVector Centre = FVector(0.3, -0.5, 0.8).GetSafeNormal();
		const FVector East = FVector::CrossProduct(FVector::UpVector, Centre).GetSafeNormal();
		const FVector North = FVector::CrossProduct(Centre, East);
		const auto Direction = [&](double AlongEast, double AlongNorth) { return (Centre * Radius + East * AlongEast + North * AlongNorth).GetSafeNormal(); };
		const TArray<FVector> Corners = { Direction(-500.0, -500.0), Direction(500.0, -500.0), Direction(500.0, 500.0), Direction(-500.0, 500.0) };
		const TSharedRef<const FVoxelWaterLake> Drawn = FVoxelWaterLake::MakeFromOutlineOnSphere(Corners, Radius, 100.0);
		TestTrue(TEXT("Planet outline: a planet lake"), Drawn->IsRadial());
		// 10 x 10 cells inside and a ring of one cell: 144, give or take the cells the outline's edges run along.
		TestTrue(TEXT("Planet outline: the cells inside and one cell around them"), Drawn->NumCells() >= 121 && Drawn->NumCells() <= 169);
		TestTrue(TEXT("Planet outline: the middle and a place inside are covered"), Drawn->CoversDirection(Centre) && Drawn->CoversDirection(Direction(400.0, -400.0)));
		TestTrue(TEXT("Planet outline: a place half a cell outside is covered"), Drawn->CoversDirection(Direction(550.0, 0.0)) && Drawn->CoversDirection(Direction(0.0, -550.0)));
		TestFalse(TEXT("Planet outline: a place over two cells outside is not"), Drawn->CoversDirection(Direction(750.0, 0.0)) || Drawn->CoversDirection(Direction(0.0, 750.0)));
		TestFalse(TEXT("Planet outline: the far side of the planet is not"), Drawn->CoversDirection(-Centre));
		FVector CapDirection;
		double CapAngle = 0.0;
		TestTrue(TEXT("Planet outline: its cap is round the corners' middle and holds them"), Drawn->GetCap(CapDirection, CapAngle)
			&& CapDirection.Equals(Centre, 1.0e-6) && CapAngle > 700.0 / Radius && CapAngle < 1400.0 / Radius);
		// The corners in the other order give the same footprint.
		const TArray<FVector> Reversed = { Corners[3], Corners[2], Corners[1], Corners[0] };
		TestEqual(TEXT("Planet outline: the corners' order does not matter"), FVoxelWaterLake::MakeFromOutlineOnSphere(Reversed, Radius, 100.0)->NumCells(), Drawn->NumCells());

		// The cells a filter keeps: the eastern half.
		int32 Asked = 0;
		const TSharedRef<const FVoxelWaterLake> Kept = FVoxelWaterLake::MakeFromOutlineOnSphere(Corners, Radius, 100.0, [&](const FVector& Place)
		{
			++Asked;
			return FVector::DotProduct(Place, East) > 0.0;
		});
		TestTrue(TEXT("Planet outline, kept cells: every cell inside is asked about, by its direction"), Asked >= 81 && Asked <= 121);
		TestTrue(TEXT("Planet outline, kept cells: about half the footprint"), Kept->NumCells() > Drawn->NumCells() / 3 && Kept->NumCells() < (2 * Drawn->NumCells()) / 3 + 12);
		TestTrue(TEXT("Planet outline, kept cells: the eastern side is covered, the western is not"), Kept->CoversDirection(Direction(300.0, 0.0)) && !Kept->CoversDirection(Direction(-300.0, 0.0)));
		TestTrue(TEXT("Planet outline: fewer than three corners make no footprint"),
			FVoxelWaterLake::MakeFromOutlineOnSphere(MakeArrayView(Corners.GetData(), 2), Radius, 100.0)->NumCells() == 0);
	}

	// ---- A planet: the sea 500 under the datum, a lake 300 over it on a cap of the surface.
	{
		FVoxelWaterFrame Frame;
		Frame.bRadial = true;
		Frame.Center = FVector(100000.0, -200000.0, 50000.0);
		Frame.DatumRadius = 250000.0;
		const FVoxelSeaSurface Sea = FVoxelSeaSurface::MakeSphere(Frame.Center, Frame.DatumRadius - 500.0);
		const FVector LakeDirection = FVector(0.3, -0.5, 0.8).GetSafeNormal();
		const double LakeLevel = Frame.DatumRadius + 300.0;
		// 12 x 12 cells of 250 uu: 3,000 uu across.
		const TSharedRef<const FVoxelWaterLake> Cells = FVoxelWaterLake::MakeOnSphere(LakeDirection, Frame.DatumRadius, 250.0, FIntPoint(12, 12), SquareCells(12, NoHole));
		TArray<FVoxelWaterBody> Bodies;
		Bodies.Add(FVoxelWaterBody::MakeLake(5, LakeLevel, Cells, Frame, 200.0));
		const FVoxelWaterLevels Levels = FVoxelWaterLevels::Make(Sea, Frame, FVoxelWaterLevels::WholeWorld(), Bodies);
		TestTrue(TEXT("Planet: levels are distances from the centre"), Levels.IsRadial() && !Levels.IsSeaAlone());

		FVoxelWaterSample S;
		const FVector AtCentre = Frame.Center + LakeDirection * (LakeLevel - 120.0);
		TestTrue(TEXT("Planet: the lake answers along its centre's direction, the up radial"),
			Levels.Sample(AtCentre, S) && S.Kind == EVoxelWaterBodyKind::Lake && S.BodyId == 5 && S.SurfaceLevel == LakeLevel
			&& FMath::IsNearlyEqual(S.Depth, 120.0, 1.0e-3) && S.SurfaceNormal.Equals(LakeDirection, 1.0e-6));
		// A direction 1,000 uu of arc from the centre is inside the footprint (1,500 uu to its edge); 2,500 uu is outside.
		const FVector Tangent = FVector::CrossProduct(LakeDirection, FVector::UpVector).GetSafeNormal();
		const auto Along = [&](double Arc) { return (LakeDirection * FMath::Cos(Arc / Frame.DatumRadius) + Tangent * FMath::Sin(Arc / Frame.DatumRadius)).GetSafeNormal(); };
		TestTrue(TEXT("Planet: a direction inside the footprint is the lake's"),
			Levels.Sample(Frame.Center + Along(1000.0) * Frame.DatumRadius, S) && S.Kind == EVoxelWaterBodyKind::Lake && FMath::IsNearlyEqual(S.Depth, 300.0, 1.0e-3));
		TestTrue(TEXT("Planet: a direction outside it is the sea's"),
			Levels.Sample(Frame.Center + Along(2500.0) * Frame.DatumRadius, S) && S.Kind == EVoxelWaterBodyKind::Sea && FMath::IsNearlyEqual(S.Depth, -500.0, 1.0e-3));
		TestTrue(TEXT("Planet: the far side of the planet is the sea's"),
			Levels.Sample(Frame.Center - LakeDirection * Frame.DatumRadius, S) && S.Kind == EVoxelWaterBodyKind::Sea);

		int32 CheapMismatches = 0;
		FRandomStream Random(8123);
		for (int32 i = 0; i < 4000; ++i)
		{
			const FVector Direction = Along(Random.FRandRange(0.0f, 3000.0f));
			CheapMismatches += CheapFormsAgree(Levels, Frame.Center + Direction * (Frame.DatumRadius + Random.FRandRange(-800.0f, 600.0f))) ? 0 : 1;
		}
		TestEqual(TEXT("Planet: GetDepth and IsUnder agree with the sample (mismatches)"), CheapMismatches, 0);

		const FVoxelWaterLevelRange Over = Levels.GetRange(FBox::BuildAABB(Frame.Center + LakeDirection * Frame.DatumRadius, FVector(800.0)));
		TestTrue(TEXT("Planet: the range over a box at the lake holds the sea's and the lake's level, not uniform"),
			Over.bAny && !Over.bUniform && Over.MinLevel == Sea.Radius && Over.MaxLevel == LakeLevel);
		const FVoxelWaterLevelRange Away = Levels.GetRange(FBox::BuildAABB(Frame.Center - LakeDirection * Frame.DatumRadius, FVector(800.0)));
		TestTrue(TEXT("Planet: the range on the far side is the sea's, uniform"), Away.bUniform && Away.MaxLevel == Sea.Radius);
		const FVoxelWaterLevelRange Whole = Levels.GetRange(FVoxelWaterLevels::WholeWorld());
		TestTrue(TEXT("Planet: the range over the whole world reaches the lake"), !Whole.bUniform && Whole.MaxLevel == LakeLevel);
		TestTrue(TEXT("Planet: a box over the lake's level is above the water, one that reaches under it is not"),
			Levels.IsAbove(FBox::BuildAABB(Frame.Center + LakeDirection * (LakeLevel + 400.0), FVector(100.0)))
			&& !Levels.IsAbove(FBox::BuildAABB(Frame.Center + LakeDirection * (LakeLevel - 50.0), FVector(100.0))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterLevelsReachTest,
	"VoxelWorlds.Core.WaterLevels.Reach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterLevelsReachTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterBodiesTestUtils;

	// ---- A plane: no sea. A river reach runs east 2,000 uu falling from 500 to 300, turns north at a bend and falls to
	// 200 over 1,000 uu, into a lake at 200. A tributary joins the first run from the south at its middle.
	{
		FVoxelWaterFrame Frame;
		TArray<FVoxelWaterReachVertex> Main;
		Main.Add(FVoxelWaterReachVertex(FVector(0.0, 0.0, 0.0), 450.0, 500.0, 150.0));
		Main.Add(FVoxelWaterReachVertex(FVector(1000.0, 0.0, 0.0), 350.0, 400.0, 150.0));
		Main.Add(FVoxelWaterReachVertex(FVector(1000.0, 0.0, 0.0), 350.0, 400.0, 150.0)); // the same place twice: merged
		Main.Add(FVoxelWaterReachVertex(FVector(2000.0, 0.0, 0.0), 250.0, 300.0, 150.0));
		Main.Add(FVoxelWaterReachVertex(FVector(2000.0, 1000.0, 0.0), 150.0, 200.0, 250.0));
		const TSharedRef<const FVoxelWaterReach> MainReach = FVoxelWaterReach::Make(Main, false, 120.0f);
		TestTrue(TEXT("Plane: a reach's segments, its levels and its widest footprint"),
			MainReach->NumSegments() == 3 && MainReach->GetMaxLevel() == 500.0 && MainReach->GetMinLevel() == 200.0 && MainReach->GetReach() == 250.0 && !MainReach->IsRadial());

		// The tributary comes up from the south to the main run's middle, where the main surface is 400.
		TArray<FVoxelWaterReachVertex> Side;
		Side.Add(FVoxelWaterReachVertex(FVector(1000.0, -1200.0, 0.0), 560.0, 600.0, 100.0));
		Side.Add(FVoxelWaterReachVertex(FVector(1000.0, 0.0, 0.0), 360.0, 400.0, 100.0));
		const TSharedRef<const FVoxelWaterReach> SideReach = FVoxelWaterReach::Make(Side, false, 80.0f);

		// The lake the main reach runs into: 200, over the columns north of Y 900.
		const TSharedRef<const FVoxelWaterLake> LakeCells = FVoxelWaterLake::Make(FVector2D(1000.0, 900.0), 200.0, FIntPoint(10, 10), SquareCells(10, NoHole));
		TArray<FVoxelWaterBody> Bodies;
		Bodies.Add(FVoxelWaterBody::MakeReach(20, MainReach, Frame));
		Bodies.Add(FVoxelWaterBody::MakeReach(21, SideReach, Frame));
		Bodies.Add(FVoxelWaterBody::MakeLake(10, 200.0, LakeCells, Frame, 200.0));
		const FVoxelWaterLevels Levels = FVoxelWaterLevels::Make(FVoxelSeaSurface(), Frame, FVoxelWaterLevels::WholeWorld(), Bodies);
		TestTrue(TEXT("Plane: the reach's record carries its highest level and bounds around its footprint"),
			Levels.GetBodies().Num() == 3 && Levels.GetBodies()[1].Level == 500.0
			&& Levels.GetBodies()[1].Bounds.Min.Equals(FVector(-250.0, -250.0, 150.0)) && Levels.GetBodies()[1].Bounds.Max.Equals(FVector(2250.0, 1250.0, 500.0)));

		FVoxelWaterSample S;
		// A quarter of the way down the first run, on the centre line and at each bank: one level across the river.
		TestTrue(TEXT("Plane: the level is the surface level interpolated along the nearest segment"),
			Levels.Sample(FVector(250.0, 0.0, 470.0), S) && S.Kind == EVoxelWaterBodyKind::Reach && S.BodyId == 20
			&& FMath::IsNearlyEqual(S.SurfaceLevel, 475.0) && FMath::IsNearlyEqual(S.Depth, 5.0));
		FVoxelWaterSample North, South;
		TestTrue(TEXT("Plane: the surface is level across the river"),
			Levels.Sample(FVector(250.0, 140.0, 0.0), North) && Levels.Sample(FVector(250.0, -140.0, 0.0), South)
			&& FMath::IsNearlyEqual(North.SurfaceLevel, 475.0) && FMath::IsNearlyEqual(South.SurfaceLevel, 475.0));
		TestFalse(TEXT("Plane: past the half width the reach does not answer"), Levels.Sample(FVector(250.0, 160.0, 0.0), S));
		TestFalse(TEXT("Plane: nor past its head"), Levels.Sample(FVector(-160.0, 0.0, 0.0), S));

		// The normal tilts against the fall (0.1 down a unit east), the flow runs east along the surface at the speed.
		Levels.Sample(FVector(250.0, 0.0, 0.0), S);
		const FVector ExpectedNormal = FVector(0.1, 0.0, 1.0).GetSafeNormal();
		const FVector ExpectedFlow = FVector(1.0, 0.0, -0.1).GetSafeNormal() * 120.0;
		TestTrue(TEXT("Plane: the normal is the up tilted along the reach"), S.SurfaceNormal.Equals(ExpectedNormal, 1.0e-6));
		TestTrue(TEXT("Plane: the flow is the direction downstream times the speed"), S.Flow.Equals(ExpectedFlow, 1.0e-3));

		// The bend: inside the corner the nearer segment answers; on the second run the level falls northward.
		TestTrue(TEXT("Plane: past the bend the second run answers"),
			Levels.Sample(FVector(2100.0, 500.0, 0.0), S) && S.BodyId == 20 && FMath::IsNearlyEqual(S.SurfaceLevel, 250.0)
			&& S.Flow.Equals(FVector(0.0, 1.0, -0.1).GetSafeNormal() * 120.0, 1.0e-3));
		TestTrue(TEXT("Plane: at the bend's outside corner the bend's vertex answers at its own level"),
			Levels.Sample(FVector(2100.0, -100.0, 0.0), S) && FMath::IsNearlyEqual(S.SurfaceLevel, 300.0));

		// The join: on the tributary upstream of the join it answers alone; where both cover a column the higher level
		// answers (the tributary's surface comes down to the main run's at the join).
		TestTrue(TEXT("Plane: the tributary answers upstream of the join"),
			Levels.Sample(FVector(1000.0, -600.0, 0.0), S) && S.BodyId == 21 && FMath::IsNearlyEqual(S.SurfaceLevel, 500.0));
		TestTrue(TEXT("Plane: where two reaches cover a column the higher level answers"),
			Levels.Sample(FVector(1050.0, -100.0, 0.0), S) && S.BodyId == 21 && FMath::IsNearlyEqual(S.SurfaceLevel, 400.0 + 100.0 * (200.0 / 1200.0), 1.0e-6));
		// At the join itself both are at 400: the earlier body, the main reach, answers.
		TestTrue(TEXT("Plane: at the join both are at one level and the earlier body answers"),
			Levels.Sample(FVector(1000.0, 0.0, 0.0), S) && S.BodyId == 20 && FMath::IsNearlyEqual(S.SurfaceLevel, 400.0));

		// The mouth: the reach reaches the lake at the lake's level; inside the lake's footprint, upstream of the end, the
		// reach is still higher and answers; at its end the two are at one level and the lake (the lower id) answers.
		TestTrue(TEXT("Plane: upstream of its end the reach stands over the lake it enters"),
			Levels.Sample(FVector(2000.0, 950.0, 0.0), S) && S.BodyId == 20 && FMath::IsNearlyEqual(S.SurfaceLevel, 205.0));
		TestTrue(TEXT("Plane: at its end the reach is at the lake's level and the lake answers"),
			Levels.Sample(FVector(2000.0, 1000.0, 0.0), S) && S.BodyId == 10 && S.Kind == EVoxelWaterBodyKind::Lake && S.SurfaceLevel == 200.0 && S.Flow.IsZero());
		TestTrue(TEXT("Plane: past the reach's footprint the lake is still there"),
			Levels.Sample(FVector(2800.0, 2500.0, 0.0), S) && S.BodyId == 10);

		int32 CheapMismatches = 0;
		FRandomStream Random(977);
		for (int32 i = 0; i < 4000; ++i)
		{
			const FVector P(Random.FRandRange(-400.0f, 3200.0f), Random.FRandRange(-1500.0f, 3000.0f), Random.FRandRange(100.0f, 700.0f));
			CheapMismatches += CheapFormsAgree(Levels, P) ? 0 : 1;
		}
		TestEqual(TEXT("Plane: GetDepth and IsUnder agree with the sample (mismatches)"), CheapMismatches, 0);

		// The range over a box on the first run: the reach's levels there, not uniform; nothing over a box away from all.
		const FVoxelWaterLevelRange Over = Levels.GetRange(FBox(FVector(100.0, -50.0, 0.0), FVector(300.0, 50.0, 10.0)));
		TestTrue(TEXT("Plane: the range over a stretch of the reach holds its levels, not uniform"),
			Over.bAny && !Over.bUniform && Over.MaxLevel == 500.0 && Over.MinLevel <= 475.0 && Over.MinLevel >= 200.0);
		const FVoxelWaterLevelRange Away = Levels.GetRange(FBox(FVector(-9000.0, -9000.0, 0.0), FVector(-8000.0, -8000.0, 10.0)));
		TestTrue(TEXT("Plane: no water over a box away from every body"), !Away.bAny);
	}

	// ---- A planet: a reach along a meridian arc, falling from 400 over the datum to 100 over it.
	{
		FVoxelWaterFrame Frame;
		Frame.bRadial = true;
		Frame.Center = FVector(-50000.0, 30000.0, 10000.0);
		Frame.DatumRadius = 100000.0;
		const double R = Frame.DatumRadius;
		const FVector A = FVector(1.0, 0.0, 0.0);
		const FVector B = FVector(FMath::Cos(0.02), 0.0, FMath::Sin(0.02)); // 2,000 uu of arc toward +Z
		TArray<FVoxelWaterReachVertex> Vertices;
		const int32 Steps = 20;
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const double T = static_cast<double>(Step) / Steps;
			const FVector Direction = FVector(FMath::Cos(0.02 * T), 0.0, FMath::Sin(0.02 * T));
			Vertices.Add(FVoxelWaterReachVertex(Direction * 3.0, R + 350.0 - 300.0 * T, R + 400.0 - 300.0 * T, 200.0)); // not unit: normalized when made
		}
		const TSharedRef<const FVoxelWaterReach> Reach = FVoxelWaterReach::Make(Vertices, true, 90.0f);
		TArray<FVoxelWaterBody> Bodies;
		Bodies.Add(FVoxelWaterBody::MakeReach(30, Reach, Frame));
		const FVoxelWaterLevels Levels = FVoxelWaterLevels::Make(FVoxelSeaSurface::MakeSphere(Frame.Center, R - 500.0), Frame, FVoxelWaterLevels::WholeWorld(), Bodies);
		TestTrue(TEXT("Planet: a planet reach's directions are unit and its segments kept"), Reach->IsRadial() && Reach->NumSegments() == Steps
			&& FMath::IsNearlyEqual(Reach->GetVertices()[3].Position.Size(), 1.0, 1.0e-12));

		FVoxelWaterSample S;
		const FVector Middle = FVector(FMath::Cos(0.01), 0.0, FMath::Sin(0.01));
		TestTrue(TEXT("Planet: the level is interpolated along the arc"),
			Levels.Sample(Frame.Center + Middle * (R + 200.0), S) && S.Kind == EVoxelWaterBodyKind::Reach && S.BodyId == 30
			&& FMath::IsNearlyEqual(S.SurfaceLevel, R + 250.0, 0.05) && FMath::IsNearlyEqual(S.Depth, 50.0, 0.05));
		// 150 uu of arc to the side (along Y): inside the half width, the same level; 250 uu: the sea.
		const FVector Side150 = (Middle + FVector(0.0, 150.0 / R, 0.0)).GetSafeNormal();
		const FVector Side250 = (Middle + FVector(0.0, 250.0 / R, 0.0)).GetSafeNormal();
		TestTrue(TEXT("Planet: the surface is level across the river"),
			Levels.Sample(Frame.Center + Side150 * R, S) && S.Kind == EVoxelWaterBodyKind::Reach && FMath::IsNearlyEqual(S.SurfaceLevel, R + 250.0, 0.05));
		TestTrue(TEXT("Planet: past the half width the sea answers"),
			Levels.Sample(Frame.Center + Side250 * R, S) && S.Kind == EVoxelWaterBodyKind::Sea);
		// The flow runs along the arc toward B (the tangent toward +Z at the middle), down the slope of 300 over 2,000.
		Levels.Sample(Frame.Center + Middle * R, S);
		const FVector TangentDown = FVector(-FMath::Sin(0.01), 0.0, FMath::Cos(0.01));
		TestTrue(TEXT("Planet: the flow runs along the arc at the speed"),
			FMath::IsNearlyEqual(S.Flow.Size(), 90.0, 1.0e-3) && FVector::DotProduct(S.Flow.GetSafeNormal(), TangentDown) > 0.98
			&& FVector::DotProduct(S.Flow, Middle) < 0.0);
		TestTrue(TEXT("Planet: the normal leans downstream of the radial"),
			FVector::DotProduct(S.SurfaceNormal, Middle) > 0.98 && FVector::DotProduct(S.SurfaceNormal, TangentDown) > 0.1);

		const FVoxelWaterLevelRange Over = Levels.GetRange(FBox::BuildAABB(Frame.Center + Middle * R, FVector(300.0)));
		TestTrue(TEXT("Planet: the range over a box on the reach is not uniform and reaches its levels"),
			!Over.bUniform && Over.MaxLevel > R + 250.0 && Over.MaxLevel <= R + 400.0 && Over.MinLevel == R - 500.0);
		const FVoxelWaterLevelRange Away = Levels.GetRange(FBox::BuildAABB(Frame.Center - Middle * R, FVector(300.0)));
		TestTrue(TEXT("Planet: the range on the far side is the sea's, uniform"), Away.bUniform && Away.MaxLevel == R - 500.0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
