// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelWorldBoundary.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The hard boundary at a bounded world's edge (WORLD_MODES_REVIEW_PLAN.md §14.2 WM-I5, decision D9). Pure geometry,
// runs headless; the chunk manager turns each wall into an invisible collision box.
//
// A circle's walls are an inscribed polygon: every point past R + F is behind some wall's inner face, every point more
// than the chord error inside R + F is in front of all of them, and neighbouring walls overlap so the ring has no gap.
// ---------------------------------------------------------------------------

namespace VoxelWorldBoundaryTest
{
	/** How far (X, Y) is past a wall's inner face, along the wall's outward axis (positive = behind the face). */
	double DepthPastInnerFace(const FVoxelWorldBoundaryWall& Wall, double X, double Y)
	{
		const FVector Out = Wall.Rotation.Vector();
		const FVector InnerFaceCentre = Wall.Center - Out * Wall.HalfExtent.X;
		return FVector::DotProduct(FVector(X, Y, Wall.Center.Z) - InnerFaceCentre, Out);
	}

	/** Whether (X, Y) is inside a wall's box footprint (within its length and thickness). */
	bool IsInsideWallBox(const FVoxelWorldBoundaryWall& Wall, double X, double Y)
	{
		const FVector Local = Wall.Rotation.UnrotateVector(FVector(X, Y, Wall.Center.Z) - Wall.Center);
		return FMath::Abs(Local.X) <= Wall.HalfExtent.X && FMath::Abs(Local.Y) <= Wall.HalfExtent.Y;
	}

	FVoxelWorldBounds Circle(double CX, double CY, double Radius)
	{
		FVoxelWorldBounds Bounds;
		Bounds.Shape = EVoxelWorldBoundsShape::Circle;
		Bounds.Center = FVector(CX, CY, 0.0);
		Bounds.OuterRadius = static_cast<float>(Radius);
		Bounds.HalfExtent = FVector2D(Radius, Radius);
		return Bounds;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWorldBoundaryCircleTest,
	"VoxelWorlds.WorldModes.Boundary.CircleWalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWorldBoundaryCircleTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWorldBoundaryTest;
	constexpr double Thickness = 500.0;
	constexpr double ChordError = 50.0;

	// The review islands' R + F (S, M, L), centred off the origin like an island with a world origin.
	for (const double Radius : { 40000.0, 250000.0, 500000.0 })
	{
		const FVoxelWorldBounds Bounds = Circle(12345.0, -54321.0, Radius);
		TArray<FVoxelWorldBoundaryWall> Walls;
		FVoxelWorldBoundary::BuildWalls(Bounds, Thickness, ChordError, -20000.0, 60000.0, Walls);
		const int32 Expected = FVoxelWorldBoundary::GetCircleSegmentCount(Radius, ChordError);
		TestEqual(FString::Printf(TEXT("R+F %.0f: one wall per segment"), Radius), Walls.Num(), Expected);
		TestTrue(FString::Printf(TEXT("R+F %.0f: chord error within %.0f uu"), Radius, ChordError),
			Radius * (1.0 - FMath::Cos(UE_DOUBLE_PI / Expected)) <= ChordError + 0.01);
		AddInfo(FString::Printf(TEXT("R+F %.0f: %d walls"), Radius, Walls.Num()));
		if (Walls.Num() == 0)
		{
			continue;
		}
		TestTrue(TEXT("Walls span the requested height"), FMath::IsNearlyEqual(Walls[0].Center.Z, 20000.0)
			&& FMath::IsNearlyEqual(Walls[0].HalfExtent.Z, 40000.0));

		int32 Escapes = 0;      // a point past R + F in front of every wall's inner face: a gap
		int32 Intrusions = 0;   // a point well inside behind some wall's inner face: the wall cuts the island
		int32 Gaps = 0;         // a point just past R + F inside no wall box: the ring is not closed
		for (int32 i = 0; i < 3600; ++i)
		{
			const double Angle = UE_DOUBLE_TWO_PI * (i + 0.37) / 3600.0;
			const double CosA = FMath::Cos(Angle), SinA = FMath::Sin(Angle);
			auto At = [&](double Distance) { return FVector2D(Bounds.Center.X + Distance * CosA, Bounds.Center.Y + Distance * SinA); };

			const FVector2D Past = At(Radius + 1.0);
			const FVector2D InWall = At(Radius + 10.0);
			const FVector2D Inside = At(Radius - ChordError - 1.0);
			bool bBlocked = false, bInWall = false;
			for (const FVoxelWorldBoundaryWall& Wall : Walls)
			{
				// Only the wall facing this direction can block it; the others face elsewhere.
				if (FVector2D::DotProduct(FVector2D(Wall.Rotation.Vector()), FVector2D(CosA, SinA)) < FMath::Cos(UE_DOUBLE_TWO_PI / Walls.Num()))
				{
					continue;
				}
				bBlocked |= DepthPastInnerFace(Wall, Past.X, Past.Y) >= 0.0;
				bInWall |= IsInsideWallBox(Wall, InWall.X, InWall.Y);
				Intrusions += DepthPastInnerFace(Wall, Inside.X, Inside.Y) >= 0.0 && IsInsideWallBox(Wall, Inside.X, Inside.Y) ? 1 : 0;
			}
			Escapes += bBlocked ? 0 : 1;
			Gaps += bInWall ? 0 : 1;
		}
		TestEqual(FString::Printf(TEXT("R+F %.0f: every point past the edge is behind a wall"), Radius), Escapes, 0);
		TestEqual(FString::Printf(TEXT("R+F %.0f: the walls form a closed ring just past the edge"), Radius), Gaps, 0);
		TestEqual(FString::Printf(TEXT("R+F %.0f: no wall reaches inside the chord error"), Radius), Intrusions, 0);
	}

	// Segment counts: never below the minimum, more for bigger circles, capped.
	TestEqual(TEXT("A tiny circle still gets the minimum"), FVoxelWorldBoundary::GetCircleSegmentCount(100.0, 50.0), FVoxelWorldBoundary::MinCircleSegments);
	TestTrue(TEXT("Bigger circles get more walls"), FVoxelWorldBoundary::GetCircleSegmentCount(500000.0, 50.0) > FVoxelWorldBoundary::GetCircleSegmentCount(40000.0, 50.0));
	TestEqual(TEXT("Capped"), FVoxelWorldBoundary::GetCircleSegmentCount(1.0e9, 50.0), FVoxelWorldBoundary::MaxCircleSegments);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWorldBoundaryShapesTest,
	"VoxelWorlds.WorldModes.Boundary.OtherShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWorldBoundaryShapesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWorldBoundaryTest;
	TArray<FVoxelWorldBoundaryWall> Walls;

	// Rectangle: four walls whose inner faces are the sides, closed at the corners.
	FVoxelWorldBounds Rect;
	Rect.Shape = EVoxelWorldBoundsShape::Rectangle;
	Rect.Center = FVector(1000.0, -2000.0, 0.0);
	Rect.HalfExtent = FVector2D(40000.0, 30000.0);
	FVoxelWorldBoundary::BuildWalls(Rect, 500.0, 50.0, -10000.0, 10000.0, Walls);
	TestEqual(TEXT("Rectangle: four walls"), Walls.Num(), 4);
	int32 Escapes = 0, Intrusions = 0;
	for (int32 i = 0; i < 400; ++i)
	{
		// Points along the four sides, the corners included, just outside and just inside.
		const double T = -1.0 + 2.0 * i / 399.0;
		const FVector2D C(Rect.Center.X, Rect.Center.Y);
		const FVector2D Outside[4] = { C + FVector2D(40001.0, T * 30001.0), C + FVector2D(-40001.0, T * 30001.0),
			C + FVector2D(T * 40001.0, 30001.0), C + FVector2D(T * 40001.0, -30001.0) };
		const FVector2D Inside[4] = { C + FVector2D(39999.0, T * 29999.0), C + FVector2D(-39999.0, T * 29999.0),
			C + FVector2D(T * 39999.0, 29999.0), C + FVector2D(T * 39999.0, -29999.0) };
		for (int32 Side = 0; Side < 4; ++Side)
		{
			bool bBlocked = false;
			for (const FVoxelWorldBoundaryWall& Wall : Walls)
			{
				bBlocked |= IsInsideWallBox(Wall, Outside[Side].X, Outside[Side].Y);
				Intrusions += IsInsideWallBox(Wall, Inside[Side].X, Inside[Side].Y) ? 1 : 0;
			}
			Escapes += bBlocked ? 0 : 1;
		}
	}
	TestEqual(TEXT("Rectangle: just past every side and corner is inside a wall"), Escapes, 0);
	TestEqual(TEXT("Rectangle: nothing inside is"), Intrusions, 0);

	// No walls for a plane or a planet.
	FVoxelWorldBoundary::BuildWalls(FVoxelWorldBounds(), 500.0, 50.0, -10000.0, 10000.0, Walls);
	TestEqual(TEXT("Unbounded: no walls"), Walls.Num(), 0);
	FVoxelWorldBounds Shell;
	Shell.Shape = EVoxelWorldBoundsShape::Shell;
	Shell.OuterRadius = 255000.0f;
	Shell.InnerRadius = 247000.0f;
	FVoxelWorldBoundary::BuildWalls(Shell, 500.0, 50.0, -10000.0, 10000.0, Walls);
	TestEqual(TEXT("Shell: no walls"), Walls.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
