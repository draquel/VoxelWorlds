// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelWaterBodyMesh.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The drawn lakes and river reaches (the hydrology plan's H3, section 5.6, D147).
//
// - Streaming.Water.BodyMesh.Lake: a lake's sheet against its record, on a plane and a planet. Every vertex lies at
//   the lake's level (world Z, or that distance from the planet centre: the sheet is on its sphere); the sheet covers
//   the footprint's cells and nothing else (its area is the cells' on the plane; a quad a cell on the planet, their
//   corners shared); the top faces up and the underside is the same triangles turned over with the top's normals.
// - Streaming.Water.BodyMesh.Reach: a river reach's ribbon against its record, on a plane and a planet: two vertices a
//   vertex of the reach, at that vertex's surface level, its footprint's half width to either side (more at a bend, to
//   hold the width from both segments), so the head, whose width is nothing, is a point, and the mouth's vertices lie
//   at the record's last level; the second texture coordinate runs downstream by the length, the tangent is the
//   direction downstream, and the speed rides in the second channel.
// - Streaming.Water.BodyMesh.LakeShore: a lake in a bowl whose footprint carries its ground (G229), on a plane and a
//   planet. The ground read back at the corners and centres; the sheet cut to the shore: every point whose ground lies
//   under the level under it, none whose ground stands over the level and its margin, its area the bowl's at the level
//   and the margin (the uncut sheet's is the cells', steps and all), every triangle facing up, a planet's cut points
//   shared by the cells beside them, the underside the top turned over.
// - Streaming.Water.BodyMeshes.Plane / .Planet: a streamed world with a lake and a river reach added at its start.
//   A mesh a body within a tick, a lake with the water's material and a river reach with the river's, and none
//   once the configuration names no water material; each underside shows only while the viewer is under that body's
//   surface; the cvar's switch hides them. On the plane, with a sea: the lake's columns add nothing to the sea's
//   tile though their voxels hold water in the chunk layer the sea's level reaches, and a sea column still does. And
//   the rule a dedicated server keeps: it draws none.
// ---------------------------------------------------------------------------

namespace VoxelWaterBodyMeshTest
{
	/** The sum of a section's triangle areas, and how many of them face along Up (by their winding, UE's front face). */
	void Measure(const FVoxelPlanetOceanMesh& Section, const FVector& Origin, const FVector& PlanetCenter, bool bPlanet, double& OutArea, int32& OutFacingUp,
		int32& OutFacingDown)
	{
		OutArea = 0.0;
		OutFacingUp = OutFacingDown = 0;
		for (int32 i = 0; i + 2 < Section.Triangles.Num(); i += 3)
		{
			const FVector A = Origin + Section.Positions[Section.Triangles[i]];
			const FVector B = Origin + Section.Positions[Section.Triangles[i + 1]];
			const FVector C = Origin + Section.Positions[Section.Triangles[i + 2]];
			const FVector Cross = FVector::CrossProduct(B - A, C - A);
			OutArea += 0.5 * Cross.Size();
			if (Cross.SizeSquared() < 1.0e-6)
			{
				continue; // a point (a reach's head)
			}
			const FVector Up = bPlanet ? (A - PlanetCenter).GetSafeNormal() : FVector::UpVector;
			// Counter-clockwise corners are given as (A, C, B): a top triangle's cross product points down.
			(FVector::DotProduct(Cross, Up) < 0.0 ? OutFacingUp : OutFacingDown) += 1;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterBodyMeshLakeTest, "VoxelWorlds.Streaming.Water.BodyMesh.Lake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterBodyMeshLakeTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterBodyMeshTest;
	// A footprint of 7 x 5 cells with a corner and a hole left out.
	const FIntPoint Size(7, 5);
	TBitArray<> Cells(true, Size.X * Size.Y);
	for (const FIntPoint& Off : { FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(3, 2), FIntPoint(6, 4) })
	{
		Cells[Off.Y * Size.X + Off.X] = false;
	}
	const int32 NumCells = Size.X * Size.Y - 5;
	const double Cell = 300.0;

	for (const bool bPlanet : { false, true })
	{
		const TCHAR* Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		FVoxelWaterFrame Frame;
		Frame.bRadial = bPlanet;
		Frame.Center = FVector(12345.0, -54321.0, 777.0);
		Frame.DatumRadius = 250000.0;
		const FVector Up = FVector(0.4, -0.3, 0.7).GetSafeNormal();
		const double Level = bPlanet ? Frame.DatumRadius + 431.25 : 1234.5;
		const TSharedRef<const FVoxelWaterLake> Footprint = bPlanet
			? FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, Cell, Size, Cells)
			: FVoxelWaterLake::Make(FVector2D(1.0e6 + 150.0, -2.0e6 + 75.0), Cell, Size, Cells);
		const FVoxelWaterBody Body = FVoxelWaterBody::MakeLake(71, Level, Footprint, Frame, 500.0);

		FVoxelWaterBodyMesh Mesh;
		if (!TestTrue(FString::Printf(TEXT("%s: a lake's sheet is built"), Name), FVoxelWaterBodyMesh::Build(Body, Frame, 75.0, Mesh)))
		{
			continue;
		}
		// Every vertex at the level, its normal the local up, the water's id in the second channel; no float of the
		// size of the world's coordinates in the mesh.
		double WorstLevel = 0.0, WorstNormal = 0.0, Farthest = 0.0;
		bool bIds = true;
		for (int32 V = 0; V < Mesh.Top.Positions.Num(); ++V)
		{
			const FVector P = Mesh.GetWorldPosition(V);
			WorstLevel = FMath::Max(WorstLevel, FMath::Abs(Frame.HeightOf(P) - Level));
			WorstNormal = FMath::Max(WorstNormal, (Mesh.Top.Normals[V] - Frame.UpAt(P)).Size());
			Farthest = FMath::Max(Farthest, Mesh.Top.Positions[V].Size());
			bIds &= Mesh.Top.UV1[V].X == FVoxelWaterBodyMesh::WaterMaterialID && Mesh.Top.Colors[V].R == FVoxelWaterBodyMesh::WaterMaterialID;
		}
		TestTrue(FString::Printf(TEXT("%s: every vertex lies at the lake's level (worst %.4f)"), Name, WorstLevel), WorstLevel < 0.01);
		TestTrue(FString::Printf(TEXT("%s: every normal is the local up"), Name), WorstNormal < 1.0e-4);
		TestTrue(FString::Printf(TEXT("%s: the vertices carry the water's id"), Name), bIds);
		TestTrue(FString::Printf(TEXT("%s: positions are measured from the mesh's own origin (farthest %.0f uu)"), Name, Farthest), Farthest < 3000.0);

		// The sheet covers the cells: its area is theirs (on the planet a little more, the sheet standing over the datum).
		double Area = 0.0;
		int32 FacingUp = 0, FacingDown = 0;
		Measure(Mesh.Top, Mesh.Origin, Frame.Center, bPlanet, Area, FacingUp, FacingDown);
		const double CellsArea = NumCells * Cell * Cell;
		AddInfo(FString::Printf(TEXT("%s: %d cells, %d quads, %d vertices, area %.0f against the cells' %.0f"), Name, NumCells, Mesh.Quads,
			Mesh.Top.Positions.Num(), Area, CellsArea));
		TestTrue(FString::Printf(TEXT("%s: the sheet's area is the footprint's"), Name), FMath::Abs(Area - CellsArea) < (bPlanet ? 0.01 : 1.0e-6) * CellsArea);
		TestTrue(FString::Printf(TEXT("%s: every triangle of the top faces up"), Name), FacingUp == Mesh.Quads * 2 && FacingDown == 0);
		// Each cell's centre under the sheet, each left-out cell's not: a point of the sheet's own triangles.
		int32 Covered = 0, WronglyCovered = 0;
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			for (int32 X = 0; X < Size.X; ++X)
			{
				const FVector Centre = Footprint->GetCellCentre(X, Y, Level, Frame.Center);
				bool bInside = false;
				for (int32 i = 0; i + 2 < Mesh.Top.Triangles.Num() && !bInside; i += 3)
				{
					const FVector A = Mesh.GetWorldPosition(Mesh.Top.Triangles[i]);
					const FVector B = Mesh.GetWorldPosition(Mesh.Top.Triangles[i + 1]);
					const FVector C = Mesh.GetWorldPosition(Mesh.Top.Triangles[i + 2]);
					const FVector Bary = FMath::ComputeBaryCentric2D(FVector::PointPlaneProject(Centre, A, B, C), A, B, C);
					bInside = Bary.X >= -1.0e-6 && Bary.Y >= -1.0e-6 && Bary.Z >= -1.0e-6
						&& FVector::Dist(Centre, FVector::PointPlaneProject(Centre, A, B, C)) < 1.0;
				}
				Covered += (bInside && Footprint->HasCell(X, Y)) ? 1 : 0;
				WronglyCovered += (bInside && !Footprint->HasCell(X, Y)) ? 1 : 0;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: every cell of the footprint is under the sheet"), Name), Covered, NumCells);
		TestEqual(FString::Printf(TEXT("%s: no cell left out of the footprint is under it"), Name), WronglyCovered, 0);
		if (bPlanet)
		{
			TestEqual(TEXT("Planet: a quad a cell"), Mesh.Quads, NumCells);
			TestTrue(TEXT("Planet: the cells share their corners"), Mesh.Top.Positions.Num() < NumCells * 4 && Mesh.Top.Positions.Num() >= (Size.X + 1) * (Size.Y + 1) - 6);
		}
		else
		{
			TestTrue(TEXT("Plane: the cells merge into rectangles"), Mesh.Quads < NumCells && Mesh.Quads >= 3);
		}

		// The underside: the same vertices and normals, every triangle turned over.
		double UnderArea = 0.0;
		int32 UnderUp = 0, UnderDown = 0;
		Measure(Mesh.Underside, Mesh.Origin, Frame.Center, bPlanet, UnderArea, UnderUp, UnderDown);
		TestTrue(FString::Printf(TEXT("%s: the underside is the top turned over"), Name),
			Mesh.Underside.Positions.Num() == Mesh.Top.Positions.Num() && Mesh.Underside.Triangles.Num() == Mesh.Top.Triangles.Num()
			&& UnderDown == Mesh.Quads * 2 && UnderUp == 0 && FMath::IsNearlyEqual(UnderArea, Area, 1.0e-6 * Area)
			&& Mesh.Underside.Normals == Mesh.Top.Normals);
	}

	// The sea, and a body of the other kind of world, build nothing.
	FVoxelWaterBodyMesh None;
	FVoxelWaterBody SeaRecord;
	TestFalse(TEXT("The sea's record builds no mesh"), FVoxelWaterBodyMesh::Build(SeaRecord, FVoxelWaterFrame(), 75.0, None));
	FVoxelWaterFrame PlanetFrame;
	PlanetFrame.bRadial = true;
	PlanetFrame.DatumRadius = 250000.0;
	const FVoxelWaterBody PlaneLake = FVoxelWaterBody::MakeLake(72, 100.0, FVoxelWaterLake::Make(FVector2D::ZeroVector, 100.0, FIntPoint(2, 2), TBitArray<>(true, 4)),
		FVoxelWaterFrame(), 100.0);
	TestFalse(TEXT("A heightmap lake builds no mesh on a planet"), FVoxelWaterBodyMesh::Build(PlaneLake, PlanetFrame, 75.0, None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterBodyMeshReachTest, "VoxelWorlds.Streaming.Water.BodyMesh.Reach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterBodyMeshReachTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterBodyMeshTest;
	for (const bool bPlanet : { false, true })
	{
		const TCHAR* Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		FVoxelWaterFrame Frame;
		Frame.bRadial = bPlanet;
		Frame.Center = FVector(-2222.0, 3333.0, 4444.0);
		Frame.DatumRadius = 250000.0;
		const FVector Up = FVector(0.5, 0.6, 0.62).GetSafeNormal();
		const FVector East = FVector::CrossProduct(FVector::UpVector, Up).GetSafeNormal();
		const FVector North = FVector::CrossProduct(Up, East);
		const double Base = bPlanet ? Frame.DatumRadius : 0.0;
		// Along and Across the ground from the reach's start: a world XY place, or a direction on the planet.
		const auto Place = [&](double Along, double Across)
		{
			return bPlanet ? (Up * Frame.DatumRadius + East * Along + North * Across).GetSafeNormal() : FVector(5.0e5 + Along, -3.0e5 + Across, 0.0);
		};
		// Straight for 12 vertices, a bend of 40 degrees, straight for 12 more: a vertex every 150 uu. The width starts
		// from nothing over the first five vertices (the head); the surface falls 4 uu a vertex to the mouth.
		const int32 Num = 25;
		const double Step = 150.0, Width = 525.0, HeadLevel = 900.0;
		const double Turn = FMath::DegreesToRadians(40.0);
		TArray<FVoxelWaterReachVertex> Vertices;
		FVector2D At(0.0, 0.0);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (Index > 0)
			{
				At += Index <= 12 ? FVector2D(Step, 0.0) : FVector2D(Step * FMath::Cos(Turn), Step * FMath::Sin(Turn));
			}
			const double Surface = Base + HeadLevel - 4.0 * Index;
			Vertices.Add(FVoxelWaterReachVertex(Place(At.X, At.Y), Surface - 150.0, Surface, Width * FMath::Min(1.0, Index / 5.0)));
		}
		const TSharedRef<const FVoxelWaterReach> Reach = FVoxelWaterReach::Make(Vertices, bPlanet, 180.0f);
		const FVoxelWaterBody Body = FVoxelWaterBody::MakeReach(81, Reach, Frame);
		FVoxelWaterBodyMesh Mesh;
		if (!TestTrue(FString::Printf(TEXT("%s: a river reach's ribbon is built"), Name), FVoxelWaterBodyMesh::Build(Body, Frame, 75.0, Mesh)))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: two vertices a vertex of the reach, a quad a segment"), Name),
			Mesh.Top.Positions.Num() == 2 * Num && Mesh.Quads == Num - 1);

		double WorstLevel = 0.0, WorstWidth = 0.0, WorstCentre = 0.0, WorstTangent = 0.0, BendWidth = 0.0;
		bool bUVs = true;
		double LastV = -1.0;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const FVector Right = Mesh.GetWorldPosition(2 * Index);
			const FVector Left = Mesh.GetWorldPosition(2 * Index + 1);
			const double Level = Vertices[Index].SurfaceLevel;
			WorstLevel = FMath::Max3(WorstLevel, FMath::Abs(Frame.HeightOf(Right) - Level), FMath::Abs(Frame.HeightOf(Left) - Level));
			// The two edges stand either side of the vertex itself, the footprint's half width away (the chord, on a planet).
			const FVector Centre = bPlanet ? Frame.Center + Vertices[Index].Position.GetSafeNormal() * Level
				: FVector(Vertices[Index].Position.X, Vertices[Index].Position.Y, Level);
			WorstCentre = FMath::Max(WorstCentre, FVector::Dist((Right + Left) * 0.5, Centre));
			const double Across = FVector::Dist(Right, Left) * (bPlanet ? Frame.DatumRadius / Level : 1.0);
			const double Wanted = 2.0 * Vertices[Index].HalfWidth;
			if (Index == 12)
			{
				BendWidth = Across; // the bend's vertex: wider, by one over the cosine of half the turn
			}
			else if (Index != 11 && Index != 13)
			{
				WorstWidth = FMath::Max(WorstWidth, FMath::Abs(Across - Wanted));
			}
			// The tangent is the direction downstream: toward the next vertex's centre (the last one's, from the one before).
			const int32 Before = FMath::Max(Index - 1, 0), After = FMath::Min(Index + 1, Num - 1);
			const FVector Along = ((bPlanet ? Vertices[After].Position.GetSafeNormal() * Frame.DatumRadius : Vertices[After].Position)
				- (bPlanet ? Vertices[Before].Position.GetSafeNormal() * Frame.DatumRadius : Vertices[Before].Position)).GetSafeNormal();
			WorstTangent = FMath::Max(WorstTangent, 1.0 - FVector::DotProduct(Mesh.Top.Tangents[2 * Index], Along));
			// UV0: across in voxels (negative on the right, positive on the left), along by the length; UV1: the id and the speed.
			const FVector2D UVR = Mesh.Top.UV0[2 * Index], UVL = Mesh.Top.UV0[2 * Index + 1];
			bUVs &= FMath::IsNearlyEqual(UVR.X, -Vertices[Index].HalfWidth / 75.0, 1.0e-3) && FMath::IsNearlyEqual(UVL.X, Vertices[Index].HalfWidth / 75.0, 1.0e-3)
				&& UVR.Y == UVL.Y && UVR.Y > LastV && FMath::IsNearlyEqual(UVR.Y, Index * Step / 75.0, 0.05)
				&& Mesh.Top.UV1[2 * Index] == FVector2D(FVoxelWaterBodyMesh::WaterMaterialID, 180.0);
			LastV = UVR.Y;
		}
		AddInfo(FString::Printf(TEXT("%s: worst level %.4f, worst width %.3f, the bend's width %.1f against %.1f on the straight"), Name, WorstLevel,
			WorstWidth, BendWidth, 2.0 * Width));
		TestTrue(FString::Printf(TEXT("%s: each pair lies at its vertex's surface level"), Name), WorstLevel < 0.01);
		TestTrue(FString::Printf(TEXT("%s: each pair stands either side of its vertex"), Name), WorstCentre < 1.0);
		TestTrue(FString::Printf(TEXT("%s: each pair is the footprint's width apart"), Name), WorstWidth < 0.5);
		TestTrue(FString::Printf(TEXT("%s: the bend's pair holds the width from both segments"), Name),
			FMath::IsNearlyEqual(BendWidth, 2.0 * Width / FMath::Cos(Turn / 2.0), 2.0));
		TestTrue(FString::Printf(TEXT("%s: the head is a point"), Name), FVector::Dist(Mesh.GetWorldPosition(0), Mesh.GetWorldPosition(1)) < 1.0e-6);
		TestTrue(FString::Printf(TEXT("%s: the mouth's pair lies at the record's last level"), Name),
			FMath::Abs(Frame.HeightOf(Mesh.GetWorldPosition(2 * Num - 1)) - Vertices.Last().SurfaceLevel) < 0.01);
		TestTrue(FString::Printf(TEXT("%s: the tangent is the direction downstream"), Name), WorstTangent < 0.01);
		TestTrue(FString::Printf(TEXT("%s: the texture coordinates run across and downstream, the speed in the second channel"), Name), bUVs);

		// The ribbon is the reach's own surface: a point of it a little in from an edge samples the level it lies at.
		const FVoxelWaterLevels Levels = FVoxelWaterLevels::Make(FVoxelSeaSurface(), Frame, Body.Bounds, { Body });
		double WorstSample = 0.0;
		int32 Sampled = 0;
		for (int32 Index = 6; Index < Num - 1; ++Index)
		{
			const FVector P = FMath::Lerp(Mesh.GetWorldPosition(2 * Index), Mesh.GetWorldPosition(2 * Index + 1), 0.2);
			FVoxelWaterSample Sample;
			if (Levels.Sample(P, Sample) && Sample.Kind == EVoxelWaterBodyKind::Reach)
			{
				++Sampled;
				WorstSample = FMath::Max(WorstSample, FMath::Abs(Sample.SurfaceLevel - Frame.HeightOf(P)));
			}
		}
		TestTrue(FString::Printf(TEXT("%s: the ribbon lies at the level the reach answers under it (%d points, worst %.2f)"), Name, Sampled, WorstSample),
			Sampled >= 15 && WorstSample < 6.0);

		double Area = 0.0;
		int32 FacingUp = 0, FacingDown = 0;
		// The winding. The head's first quad has a triangle with no area. This bend turns 40 degrees at one vertex with
		// a half width of 3.5 vertex spacings, so the inner edge runs back on itself there and the ribbon folds: the
		// two triangles of the fold face the other way (they lie inside the quads before them, so nothing is left
		// open, and a view from above does not draw them). The straights and the outer edge face up.
		Measure(Mesh.Top, Mesh.Origin, Frame.Center, bPlanet, Area, FacingUp, FacingDown);
		TestTrue(FString::Printf(TEXT("%s: the top faces up but for the fold inside the bend (%d up, %d down of %d)"), Name, FacingUp, FacingDown, 2 * (Num - 1)),
			FacingDown == 2 && FacingUp == 2 * (Num - 1) - 3);
		Measure(Mesh.Underside, Mesh.Origin, Frame.Center, bPlanet, Area, FacingUp, FacingDown);
		TestTrue(FString::Printf(TEXT("%s: the underside is the top turned over (%d up, %d down of %d)"), Name, FacingUp, FacingDown, 2 * (Num - 1)),
			FacingUp == 2 && FacingDown == 2 * (Num - 1) - 3);

		// A bend no tighter than the half width does not fold: the same turn over eight vertices, every triangle up.
		TArray<FVoxelWaterReachVertex> Gentle;
		FVector2D GentleAt(0.0, 0.0);
		double Heading = 0.0;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (Index > 0)
			{
				Heading += (Index >= 9 && Index < 17) ? Turn / 8.0 : 0.0;
				GentleAt += FVector2D(Step * FMath::Cos(Heading), Step * FMath::Sin(Heading));
			}
			const double Surface = Base + HeadLevel - 4.0 * Index;
			Gentle.Add(FVoxelWaterReachVertex(Place(GentleAt.X, GentleAt.Y), Surface - 150.0, Surface, Width * 0.5));
		}
		FVoxelWaterBodyMesh GentleMesh;
		const FVoxelWaterBody GentleBody = FVoxelWaterBody::MakeReach(82, FVoxelWaterReach::Make(Gentle, bPlanet, 180.0f), Frame);
		if (TestTrue(FString::Printf(TEXT("%s: a gently bending reach's ribbon is built"), Name), FVoxelWaterBodyMesh::Build(GentleBody, Frame, 75.0, GentleMesh)))
		{
			Measure(GentleMesh.Top, GentleMesh.Origin, Frame.Center, bPlanet, Area, FacingUp, FacingDown);
			TestTrue(FString::Printf(TEXT("%s: a gentle bend's ribbon faces up everywhere (%d up, %d down)"), Name, FacingUp, FacingDown),
				FacingDown == 0 && FacingUp == 2 * (Num - 1));
			Measure(GentleMesh.Underside, GentleMesh.Origin, Frame.Center, bPlanet, Area, FacingUp, FacingDown);
			TestTrue(FString::Printf(TEXT("%s: and its underside faces down everywhere"), Name), FacingUp == 0 && FacingDown == 2 * (Num - 1));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterBodyMeshLakeShoreTest, "VoxelWorlds.Streaming.Water.BodyMesh.LakeShore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterBodyMeshLakeShoreTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterBodyMeshTest;
	// A lake in a bowl, its ground (Depth under the level at the middle, rising as the square of the distance from it)
	// read into the footprint (G229). Its footprint is the cells whose centres lie under the level, grown by one, as the
	// makers' are. The sheet cut to the shore: it covers every point whose ground lies under the level, no point whose
	// ground stands over the level and its margin, its area is the bowl's at the level and margin and not the cells',
	// every triangle faces up, a planet's cut points are shared by the cells beside them, and the underside is the top
	// turned over. The same footprint without the ground draws every cell, as before. A shallow bowl too, whose ground
	// at the level and its margin lies past the footprint's cell of shore: the sheet does not reach the footprint's
	// outline where the ground there stands over the level (no step of the outline shows).
	const double VoxelSize = 75.0;
	const double Cell = 4.0 * VoxelSize;
	const double ShoreRadius = 2000.0;
	const double Margin = FVoxelWaterBodyMesh::ShoreMarginVoxels * VoxelSize;
	const FIntPoint Size(20, 20);

	for (const int32 Case : { 0, 1, 2, 3 })
	{
		const bool bPlanet = (Case & 1) != 0;
		const bool bShallow = Case >= 2;
		const double Depth = bShallow ? 20.0 : 600.0;
		const double Rise = Depth / (ShoreRadius * ShoreRadius);
		const TCHAR* Name = bShallow ? (bPlanet ? TEXT("Planet, shallow") : TEXT("Plane, shallow")) : (bPlanet ? TEXT("Planet") : TEXT("Plane"));
		FVoxelWaterFrame Frame;
		Frame.bRadial = bPlanet;
		Frame.Center = FVector(12345.0, -54321.0, 777.0);
		Frame.DatumRadius = 250000.0;
		const FVector Up = FVector(-0.2, 0.5, 0.8).GetSafeNormal();
		const FVector2D Middle(3.0e5, -1.5e5);
		const double Level = bPlanet ? Frame.DatumRadius + 812.5 : 812.5;
		// The ground at a place as a water level: the distance from the bowl's middle along the ground (on the planet,
		// along the datum sphere).
		const auto GroundAt = [&](const FVector& Place)
		{
			const double Distance = bPlanet ? Frame.DatumRadius * FMath::Acos(FMath::Clamp(FVector::DotProduct(Place.GetSafeNormal(), Up), -1.0, 1.0))
				: FVector2D::Distance(FVector2D(Place.X, Place.Y), Middle);
			return Level - Depth + Rise * Distance * Distance;
		};
		const FVector2D Origin = Middle - FVector2D(0.5 * Size.X * Cell, 0.5 * Size.Y * Cell);
		const auto MakeShape = [&](TBitArray<> Bits)
		{
			return bPlanet ? FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, Cell, Size, MoveTemp(Bits)) : FVoxelWaterLake::Make(Origin, Cell, Size, MoveTemp(Bits));
		};
		// The cells whose centres lie under the level, grown by one cell.
		const TSharedRef<const FVoxelWaterLake> Grid = MakeShape(TBitArray<>(true, Size.X * Size.Y));
		TBitArray<> Bits(false, Size.X * Size.Y);
		int32 NumWet = 0;
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			for (int32 X = 0; X < Size.X; ++X)
			{
				if (GroundAt(Grid->GetGridPlace(X + 0.5, Y + 0.5)) >= Level)
				{
					continue;
				}
				++NumWet;
				for (int32 J = FMath::Max(Y - 1, 0); J <= FMath::Min(Y + 1, Size.Y - 1); ++J)
				{
					for (int32 I = FMath::Max(X - 1, 0); I <= FMath::Min(X + 1, Size.X - 1); ++I)
					{
						Bits[J * Size.X + I] = true;
					}
				}
			}
		}
		const TSharedRef<const FVoxelWaterLake> Plain = MakeShape(Bits);
		const TSharedRef<const FVoxelWaterLake> Footprint = FVoxelWaterLake::WithGround(*Plain, GroundAt);

		// The copy is the same footprint and carries the ground read at its corners and centres.
		bool bSameCells = Footprint->NumCells() == Plain->NumCells() && Footprint->GetSize() == Plain->GetSize();
		double WorstGround = 0.0;
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			for (int32 X = 0; X < Size.X; ++X)
			{
				bSameCells &= Footprint->HasCell(X, Y) == Plain->HasCell(X, Y);
				if (Footprint->HasCell(X, Y))
				{
					WorstGround = FMath::Max3(WorstGround, FMath::Abs(Footprint->GetCentreGround(X, Y) - GroundAt(Footprint->GetGridPlace(X + 0.5, Y + 0.5))),
						FMath::Abs(Footprint->GetCornerGround(X + 1, Y + 1) - GroundAt(Footprint->GetGridPlace(X + 1, Y + 1))));
				}
			}
		}
		TestTrue(FString::Printf(TEXT("%s: the footprint with its ground has the same cells"), Name), bSameCells && Footprint->HasGround() && !Plain->HasGround());
		TestTrue(FString::Printf(TEXT("%s: the ground read back at the corners and centres (worst %.4f)"), Name, WorstGround), WorstGround < 0.05);

		FVoxelWaterBodyMesh Mesh, Whole;
		const FVoxelWaterBody Body = FVoxelWaterBody::MakeLake(73, Level, Footprint, Frame, 2000.0);
		const FVoxelWaterBody PlainBody = FVoxelWaterBody::MakeLake(73, Level, Plain, Frame, 2000.0);
		if (!TestTrue(FString::Printf(TEXT("%s: the lake's sheet is built"), Name), FVoxelWaterBodyMesh::Build(Body, Frame, VoxelSize, Mesh))
			|| !TestTrue(FString::Printf(TEXT("%s: the sheet without the ground is built"), Name), FVoxelWaterBodyMesh::Build(PlainBody, Frame, VoxelSize, Whole)))
		{
			continue;
		}

		// Every vertex at the level, and where the ground stands no higher than the level and its margin (the bowl's
		// ground lies under the line between two of its points: a cut point read on that line is no higher).
		double WorstLevel = 0.0, HighestGround = -UE_DOUBLE_BIG_NUMBER;
		for (int32 V = 0; V < Mesh.Top.Positions.Num(); ++V)
		{
			const FVector P = Mesh.GetWorldPosition(V);
			WorstLevel = FMath::Max(WorstLevel, FMath::Abs(Frame.HeightOf(P) - Level));
			const FVector Place = bPlanet ? (P - Frame.Center).GetSafeNormal() : FVector(P.X, P.Y, 0.0);
			HighestGround = FMath::Max(HighestGround, GroundAt(Place) - Level);
		}
		TestTrue(FString::Printf(TEXT("%s: every vertex lies at the lake's level (worst %.4f)"), Name, WorstLevel), WorstLevel < 0.01);
		TestTrue(FString::Printf(TEXT("%s: no vertex stands where the ground is over the level and its margin (the highest %.2f over the level, the margin %.1f)"),
			Name, HighestGround, Margin), HighestGround <= Margin + 0.1);

		double Area = 0.0, WholeArea = 0.0;
		int32 FacingUp = 0, FacingDown = 0, WholeUp = 0, WholeDown = 0;
		Measure(Mesh.Top, Mesh.Origin, Frame.Center, bPlanet, Area, FacingUp, FacingDown);
		Measure(Whole.Top, Whole.Origin, Frame.Center, bPlanet, WholeArea, WholeUp, WholeDown);
		// The area of the bowl where the ground lies under the level and its margin: a disk (on the planet, measured
		// on the sphere of the level, a little more than on the datum).
		const double DrawnRadius = FMath::Sqrt((Depth + Margin) / Rise);
		const double BowlArea = UE_DOUBLE_PI * DrawnRadius * DrawnRadius * (bPlanet ? FMath::Square(Level / Frame.DatumRadius) : 1.0);
		AddInfo(FString::Printf(TEXT("%s: %d wet cells, %d of the footprint; the sheet %d quads and %d shore triangles, %d vertices, area %.0f; the bowl's %.0f; the cells' sheet %.0f (%d quads)"),
			Name, NumWet, Footprint->NumCells(), Mesh.Quads, Mesh.ShoreTriangles, Mesh.Top.Positions.Num(), Area, BowlArea, WholeArea, Whole.Quads));
		TestTrue(FString::Printf(TEXT("%s: some cells are whole and some cut"), Name), Mesh.Quads > 0 && Mesh.ShoreTriangles > 0);
		TestTrue(FString::Printf(TEXT("%s: every triangle of the top faces up (%d up, %d down, of %d quads and %d triangles)"), Name, FacingUp, FacingDown, Mesh.Quads, Mesh.ShoreTriangles), FacingUp == Mesh.Quads * 2 + Mesh.ShoreTriangles && FacingDown == 0);
		if (!bShallow)
		{
			TestTrue(FString::Printf(TEXT("%s: the sheet's area is the bowl's under the level and its margin (%.0f against %.0f)"), Name, Area, BowlArea),
				FMath::Abs(Area - BowlArea) < 0.01 * BowlArea);
			TestTrue(FString::Printf(TEXT("%s: without the ground the sheet covers the cells, the steps included (%.0f against %.0f)"), Name, WholeArea, BowlArea),
				WholeArea > 1.2 * BowlArea && WholeUp == Whole.Quads * 2 && Whole.ShoreTriangles == 0);
		}

		// The footprint's outline: no vertex at a corner of it whose ground stands over the level. The shallow bowl has
		// such corners whose ground lies under the level and its margin, where a margin would draw the outline's steps.
		int32 OutlineDry = 0, OutlineInMargin = 0, OutlineDrawn = 0;
		for (int32 GY = 0; GY <= Size.Y; ++GY)
		{
			for (int32 GX = 0; GX <= Size.X; ++GX)
			{
				const int32 Around = (Footprint->HasCell(GX - 1, GY - 1) ? 1 : 0) + (Footprint->HasCell(GX, GY - 1) ? 1 : 0)
					+ (Footprint->HasCell(GX - 1, GY) ? 1 : 0) + (Footprint->HasCell(GX, GY) ? 1 : 0);
				if (Around == 0 || Around == 4 || Footprint->GetCornerGround(GX, GY) <= Level)
				{
					continue;
				}
				++OutlineDry;
				OutlineInMargin += Footprint->GetCornerGround(GX, GY) < Level + Margin ? 1 : 0;
				const FVector Corner = Footprint->GetGridPoint(GX, GY, Level, Frame.Center);
				for (int32 V = 0; V < Mesh.Top.Positions.Num(); ++V)
				{
					if (FVector::DistSquared(Mesh.GetWorldPosition(V), Corner) < 0.25)
					{
						++OutlineDrawn;
						break;
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %d corners of the outline over the level, %d of them under the level and its margin"), Name, OutlineDry, OutlineInMargin));
		TestEqual(FString::Printf(TEXT("%s: the sheet reaches no corner of the outline whose ground stands over the level"), Name), OutlineDrawn, 0);
		if (bShallow)
		{
			TestTrue(FString::Printf(TEXT("%s: the shallow bowl has outline corners under the level and its margin"), Name), OutlineInMargin > 0);
		}

		// A sixth of a cell apart over the grid: a point whose ground lies under the level is under the sheet, and one
		// whose ground stands over the level and its margin is not.
		const auto UnderSheet = [&](const FVector& Point)
		{
			for (int32 i = 0; i + 2 < Mesh.Top.Triangles.Num(); i += 3)
			{
				const FVector A = Mesh.GetWorldPosition(Mesh.Top.Triangles[i]);
				const FVector B = Mesh.GetWorldPosition(Mesh.Top.Triangles[i + 1]);
				const FVector C = Mesh.GetWorldPosition(Mesh.Top.Triangles[i + 2]);
				const FVector OnPlane = FVector::PointPlaneProject(Point, A, B, C);
				if (FVector::Dist(Point, OnPlane) > 1.0)
				{
					continue;
				}
				const FVector Bary = FMath::ComputeBaryCentric2D(OnPlane, A, B, C);
				if (Bary.X >= -1.0e-6 && Bary.Y >= -1.0e-6 && Bary.Z >= -1.0e-6)
				{
					return true;
				}
			}
			return false;
		};
		int32 Wet = 0, WetCovered = 0, Dry = 0, DryCovered = 0;
		for (int32 J = 1; J < 6 * Size.Y; J += 2)
		{
			for (int32 I = 1; I < 6 * Size.X; I += 2)
			{
				const double GX = I / 6.0, GY = J / 6.0;
				const double Ground = GroundAt(Footprint->GetGridPlace(GX, GY));
				if (Ground < Level - 1.0)
				{
					++Wet;
					WetCovered += UnderSheet(Footprint->GetGridPoint(GX, GY, Level, Frame.Center)) ? 1 : 0;
				}
				else if (Ground > Level + Margin + 1.0)
				{
					++Dry;
					DryCovered += UnderSheet(Footprint->GetGridPoint(GX, GY, Level, Frame.Center)) ? 1 : 0;
				}
			}
		}
		TestTrue(FString::Printf(TEXT("%s: the points read"), Name), Wet > 100 && Dry > 100);
		TestEqual(FString::Printf(TEXT("%s: every point whose ground lies under the level (by a unit) is under the sheet"), Name), WetCovered, Wet);
		TestEqual(FString::Printf(TEXT("%s: no point whose ground stands over the level and its margin is under it"), Name), DryCovered, 0);

		if (bPlanet)
		{
			// The cells share their corners and their cut points: no two vertices at one place.
			int32 Doubled = 0;
			for (int32 A = 0; A < Mesh.Top.Positions.Num(); ++A)
			{
				for (int32 B = A + 1; B < Mesh.Top.Positions.Num(); ++B)
				{
					Doubled += FVector::DistSquared(Mesh.Top.Positions[A], Mesh.Top.Positions[B]) < 1.0e-4 ? 1 : 0;
				}
			}
			TestEqual(TEXT("Planet: the cells beside one another share their vertices"), Doubled, 0);
		}

		// The underside: the same vertices and normals, every triangle turned over.
		double UnderArea = 0.0;
		int32 UnderUp = 0, UnderDown = 0;
		Measure(Mesh.Underside, Mesh.Origin, Frame.Center, bPlanet, UnderArea, UnderUp, UnderDown);
		TestTrue(FString::Printf(TEXT("%s: the underside is the top turned over"), Name),
			Mesh.Underside.Triangles.Num() == Mesh.Top.Triangles.Num() && UnderDown == FacingUp && UnderUp == 0
			&& FMath::IsNearlyEqual(UnderArea, Area, 1.0e-6 * Area) && Mesh.Underside.Normals == Mesh.Top.Normals);
	}
	return true;
}

namespace VoxelWaterBodyMeshTest
{
	void RunWorld(FAutomationTestBase& Test, bool bPlanet)
	{
		const FString Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		// The plane's sea is set from its ground, which a first world without one reads: a site for the lake whose
		// ground and whose level (150 over it) lie in one chunk layer with a sea 250 under the ground, so the lake's
		// water is in the layer the sea's level reaches (the one whose tiles could take it), and a place of the sea
		// 150 under the sea's level or more.
		double SeaLevel = -600.0;
		FVector2D PlaneSite = FVector2D::ZeroVector, PlaneSeaSite = FVector2D::ZeroVector;
		if (!bPlanet)
		{
			VoxelTesting::FTestChunkWorld Survey;
			Survey.Create(TEXT("VoxelWaterBodyMeshSurveyWorld"));
			Survey.ConfigureSmallPlane();
			Survey.Config->bEnableWaterLevel = false;
			Survey.StartManager();
			const double Layer = Survey.Config->GetChunkWorldSize();
			TArray<FVector> Ground;
			double Lowest = TNumericLimits<double>::Max(), Highest = TNumericLimits<double>::Lowest();
			for (int32 Y = -10; Y <= 10; ++Y)
			{
				for (int32 X = -10; X <= 10; ++X)
				{
					Ground.Add(Survey.Manager->GetGeneratedSurfacePoint(FVector(X * 250.0, Y * 250.0, 0.0)));
					Lowest = FMath::Min(Lowest, Ground.Last().Z);
					Highest = FMath::Max(Highest, Ground.Last().Z);
				}
			}
			Survey.Stop();
			bool bFound = false;
			for (const FVector& Candidate : Ground)
			{
				const double Sea = Candidate.Z - 250.0;
				const bool bOneLayer = FMath::FloorToDouble(Sea / Layer) == FMath::FloorToDouble((Candidate.Z + 150.0) / Layer);
				const FVector* Under = Ground.FindByPredicate([Sea](const FVector& G) { return G.Z < Sea - 150.0; });
				// Near the middle, so the lake, the river reach beside it and the sea's place all stream.
				if (bOneLayer && Under && FMath::Abs(Candidate.X) <= 1000.0 && FMath::Abs(Candidate.Y) <= 1000.0)
				{
					SeaLevel = Sea;
					PlaneSite = FVector2D(Candidate.X, Candidate.Y);
					PlaneSeaSite = FVector2D(Under->X, Under->Y);
					bFound = true;
					break;
				}
			}
			if (!Test.TestTrue(*FString::Printf(TEXT("Plane: the terrain has a site for a lake over a sea and a place under that sea (ground %.0f to %.0f)"), Lowest, Highest), bFound))
			{
				return;
			}
		}
		VoxelTesting::FTestChunkWorld W;
		W.Create(bPlanet ? TEXT("VoxelWaterBodyMeshPlanetWorld") : TEXT("VoxelWaterBodyMeshPlaneWorld"));
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
			W.Config->WaterRadius = W.Config->WorldRadius - 3000.0f;
		}
		else
		{
			W.ConfigureSmallPlane();
			W.Config->WaterLevel = static_cast<float>(SeaLevel);
		}
		W.Config->bEnableWaterLevel = true;
		W.StartManager();
		UVoxelChunkManager* Manager = W.Manager;
		const FVoxelWaterFrame Frame = Manager->GetWaterLevels().GetFrame();
		const float VS = W.Config->VoxelSize;

		// The lake's site, and on the plane a place of the sea.
		const FVector Site = Manager->GetGeneratedSurfacePoint(bPlanet ? Frame.Center + FVector(0.5, 0.3, 0.8).GetSafeNormal() * Frame.DatumRadius
			: FVector(PlaneSite.X, PlaneSite.Y, 0.0));
		const FVector SeaSite = bPlanet ? FVector::ZeroVector : Manager->GetGeneratedSurfacePoint(FVector(PlaneSeaSite.X, PlaneSeaSite.Y, 0.0));
		const FVector Up = Manager->GetUpVectorAt(Site);
		const FVector East = bPlanet ? FVector::CrossProduct(FVector::UpVector, Up).GetSafeNormal() : FVector::ForwardVector;
		const FVector North = FVector::CrossProduct(Up, East);
		const double SiteHeight = Frame.HeightOf(Site);
		const auto At = [&](double Along, double Across, double Height)
		{
			return bPlanet ? Frame.Center + (Up * Frame.DatumRadius + East * Along + North * Across).GetSafeNormal() * Height
				: FVector(Site.X + Along, Site.Y + Across, Height);
		};

		// A lake of 8 x 8 cells of 100 uu, 150 over the site's ground; a river reach 1,500 uu east of it.
		const double LakeLevel = SiteHeight + 150.0;
		const TSharedRef<const FVoxelWaterLake> Footprint = bPlanet
			? FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, 100.0, FIntPoint(8, 8), TBitArray<>(true, 64))
			: FVoxelWaterLake::Make(FVector2D(Site.X - 400.0, Site.Y - 400.0), 100.0, FIntPoint(8, 8), TBitArray<>(true, 64));
		Manager->AddWaterBody(FVoxelWaterBody::MakeLake(61, LakeLevel, Footprint, Frame, 400.0));
		TArray<FVoxelWaterReachVertex> Vertices;
		for (int32 Step = 0; Step <= 10; ++Step)
		{
			const FVector P = At(1500.0, -500.0 + 100.0 * Step, SiteHeight);
			const double Surface = SiteHeight + 300.0 - 10.0 * Step;
			Vertices.Add(FVoxelWaterReachVertex(bPlanet ? (P - Frame.Center).GetSafeNormal() : FVector(P.X, P.Y, 0.0), Surface - 100.0, Surface, 150.0));
		}
		Manager->AddWaterBody(FVoxelWaterBody::MakeReach(62, FVoxelWaterReach::Make(Vertices, bPlanet, 120.0f), Frame));

		// ---- With a water material: a mesh a body within a tick, each with its material.
		UMaterialInterface* WaterMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		UMaterialInterface* RiverMaterial = UMaterialInstanceDynamic::Create(WaterMaterial, GetTransientPackage());
		W.Config->WaterMeshMaterial = WaterMaterial;
		W.Config->RiverMeshMaterial = RiverMaterial;
		W.SetViewer(Site + Up * 600.0);
		W.Tick();
		const FVoxelWaterBodyMeshOwner& Meshes = Manager->GetWaterBodyMeshes();
		// With the tick budget (G282) the meshes build on the thread pool and get their components in the next ticks.
		for (int32 Ticks = 0; Meshes.Num() < 2 && Ticks < 2000; ++Ticks)
		{
			W.Tick();
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: a mesh a body"), *Name), Meshes.Num(), 2);
		UProceduralMeshComponent* LakeMesh = Meshes.Find(61);
		UProceduralMeshComponent* ReachMesh = Meshes.Find(62);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: the lake and the river reach each have a component"), *Name), LakeMesh && ReachMesh))
		{
			return;
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: each has its surface and its underside"), *Name), LakeMesh->GetNumSections() == 2 && ReachMesh->GetNumSections() == 2);
		Test.TestTrue(*FString::Printf(TEXT("%s: the lake draws with the water's material"), *Name), LakeMesh->GetMaterial(0) == WaterMaterial && LakeMesh->GetMaterial(1) == WaterMaterial);
		Test.TestTrue(*FString::Printf(TEXT("%s: the river reach draws with the river's"), *Name), ReachMesh->GetMaterial(0) == RiverMaterial && ReachMesh->GetMaterial(1) == RiverMaterial);
		Test.TestTrue(*FString::Printf(TEXT("%s: the meshes cast no shadow and collide with nothing"), *Name),
			!LakeMesh->CastShadow && LakeMesh->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
		Test.TestTrue(*FString::Printf(TEXT("%s: the lake's component sits at the lake's level over its site"), *Name),
			FMath::Abs(Frame.HeightOf(LakeMesh->GetComponentLocation()) - LakeLevel) < 0.01 && FVector::Dist(LakeMesh->GetComponentLocation(), At(0.0, 0.0, LakeLevel)) < 1.0);

		// ---- The undersides: hidden from above, each shown only while the viewer is under its own body.
		const auto SectionShown = [](UProceduralMeshComponent* Component, int32 Section)
		{
			const FProcMeshSection* Data = Component->GetProcMeshSection(Section);
			return Data && Data->bSectionVisible;
		};
		Test.TestTrue(*FString::Printf(TEXT("%s: from above the surfaces show and the undersides do not"), *Name),
			SectionShown(LakeMesh, 0) && !SectionShown(LakeMesh, 1) && SectionShown(ReachMesh, 0) && !SectionShown(ReachMesh, 1)
			&& !Meshes.IsUndersideShown(61) && !Meshes.IsUndersideShown(62));
		W.SetViewer(At(0.0, 0.0, LakeLevel - 40.0));
		W.Tick();
		Test.TestTrue(*FString::Printf(TEXT("%s: under the lake's surface its underside shows, the river reach's does not"), *Name),
			SectionShown(LakeMesh, 1) && Meshes.IsUndersideShown(61) && !SectionShown(ReachMesh, 1));
		W.SetViewer(At(1500.0, 0.0, SiteHeight + 250.0 - 60.0));
		W.Tick();
		Test.TestTrue(*FString::Printf(TEXT("%s: under the river reach's surface its underside shows, the lake's no more"), *Name),
			SectionShown(ReachMesh, 1) && !SectionShown(LakeMesh, 1));
		// Under the lake's level but outside its footprint: not under the lake.
		W.SetViewer(At(-900.0, 0.0, LakeLevel - 40.0));
		W.Tick();
		Test.TestTrue(*FString::Printf(TEXT("%s: beside the lake, under its level, no underside shows"), *Name), !SectionShown(LakeMesh, 1) && !SectionShown(ReachMesh, 1));

		// ---- The switch, and the bodies kept across a change that names none of them.
		Manager->SetWaterBodyMeshesVisible(false);
		Test.TestTrue(*FString::Printf(TEXT("%s: the switch hides the meshes"), *Name), !LakeMesh->IsVisible() && !ReachMesh->IsVisible());
		Manager->SetWaterBodyMeshesVisible(true);
		Test.TestTrue(*FString::Printf(TEXT("%s: and shows them again"), *Name), LakeMesh->IsVisible() && ReachMesh->IsVisible());
		Manager->NotifyWaterBodiesChanged(FBox2D(FVector2D(-1.0e7, -1.0e7), FVector2D(-0.9e7, -0.9e7)));
		W.Tick();
		Test.TestTrue(*FString::Printf(TEXT("%s: a change elsewhere keeps the meshes that are drawn"), *Name),
			Manager->GetWaterBodyMeshes().Num() == 2 && Manager->GetWaterBodyMeshes().Find(61) == LakeMesh);

		// ---- The tiles are the sea's: on the plane, the lake's columns add nothing to a tile though their voxels hold
		// water in the layer the sea's level reaches, and a sea column still does.
		if (!bPlanet)
		{
			W.SetViewer(Site + Up * 600.0);
			Test.TestTrue(TEXT("Plane: the world settles"), W.Settle());
			const FVoxelData InLake = Manager->GetVoxelAt(FVector(Site.X, Site.Y, Site.Z + 1.5 * VS));
			const double Layer = W.Config->GetChunkWorldSize();
			Test.TestTrue(TEXT("Plane: the lake's voxels hold water over the sea's level, in the sea's chunk layer"),
				InLake.IsAir() && InLake.HasWaterFlag() && LakeLevel > SeaLevel
				&& FMath::FloorToDouble(SeaLevel / Layer) == FMath::FloorToDouble(Site.Z / Layer));
			TArray<int32> WetLayers;
			Manager->GetWaterTileColumn(FVector(Site.X, Site.Y, 0.0), WetLayers);
			Test.TestEqual(TEXT("Plane: the lake's column marks no chunk layer of the sea's tile wet"), WetLayers.Num(), 0);
			W.SetViewer(SeaSite + FVector(0.0, 0.0, 600.0));
			Test.TestTrue(TEXT("Plane: the world settles over the sea"), W.Settle());
			Manager->GetWaterTileColumn(FVector(SeaSite.X, SeaSite.Y, 0.0), WetLayers);
			Test.TestTrue(TEXT("Plane: a sea column still marks its tile wet (non-vacuous)"), WetLayers.Num() > 0);
		}

		// ---- Without a water material nothing is drawn: the meshes go when it does.
		W.Config->WaterMeshMaterial = nullptr;
		W.Config->RiverMeshMaterial = nullptr;
		W.Tick();
		Test.TestEqual(*FString::Printf(TEXT("%s: without a water material no body is drawn"), *Name), Manager->GetWaterBodyMeshes().Num(), 0);

		// ---- A dedicated server draws none: the rule the meshes are made by.
		Test.TestTrue(*FString::Printf(TEXT("%s: a game world that is not a dedicated server's draws its bodies, a dedicated server's and an editor world do not"), *Name),
			UVoxelChunkManager::DrawsWaterBodies(true, false) && !UVoxelChunkManager::DrawsWaterBodies(true, true) && !UVoxelChunkManager::DrawsWaterBodies(false, false));
		W.Stop();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterBodyMeshesPlaneTest, "VoxelWorlds.Streaming.Water.BodyMeshes.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterBodyMeshesPlaneTest::RunTest(const FString& Parameters)
{
	VoxelWaterBodyMeshTest::RunWorld(*this, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterBodyMeshesPlanetTest, "VoxelWorlds.Streaming.Water.BodyMeshes.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterBodyMeshesPlanetTest::RunTest(const FString& Parameters)
{
	VoxelWaterBodyMeshTest::RunWorld(*this, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
