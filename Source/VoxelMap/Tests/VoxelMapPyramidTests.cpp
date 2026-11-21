// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMapTileFrame.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// MAP_PYRAMID_PLAN.md P1: the tile pyramid's arithmetic (FVoxelMapTileFrame), pure, on a plane and on a planet whose face
// edge is not a power of two (37 tiles: the last tile of a face edge partly empty at every coarse level).
// - A level's tile holds its level-0 tiles: LevelTileOf, ParentOf and FirstLevel0Tile agree, at every level, on every
//   planet cell and on plane tiles either side of 0.
// - A coarse tile's grid point samples where the level-0 pixel SourcePixel names lies (a plane's corner exactly; a
//   planet's direction in that level-0 cell and pixel), so the edit overlays under coarse pixels are the right ones.
// - LevelForTexel picks the coarsest level whose pixels are at most a texel apart.
// ---------------------------------------------------------------------------

namespace VoxelMapPyramidTest
{
	FVoxelMapTileFrame PlaneFrame()
	{
		FVoxelMapTileFrame Frame;
		Frame.bPlanet = false;
		Frame.ChunkSize = 16;
		Frame.VoxelSize = 100.0f;
		Frame.WorldOrigin = FVector(1000.0, -500.0, 0.0);
		return Frame;
	}

	FVoxelMapTileFrame PlanetFrame()
	{
		FVoxelMapTileFrame Frame;
		Frame.bPlanet = true;
		Frame.Grid = FVoxelCubeSphereGrid(37);
		Frame.Center = FVector(100.0, 200.0, -300.0);
		Frame.Radius = 37 * 1600.0 / (0.5 * UE_DOUBLE_PI);
		Frame.ChunkSize = 16;
		Frame.VoxelSize = 100.0f;
		return Frame;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapPyramidLevelsTest,
	"VoxelWorlds.Map.Pyramid.Levels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapPyramidLevelsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapPyramidTest;

	// ---- A plane ----
	{
		const FVoxelMapTileFrame Frame = PlaneFrame();
		TestTrue(TEXT("Plane: (-5, 7) at level 2 is (-2, 1)"), Frame.LevelTileOf(FIntPoint(-5, 7), 2) == FVoxelMapTileId(FIntPoint(-2, 1), 2));
		TestTrue(TEXT("Plane: level 0 is the tile itself"), Frame.LevelTileOf(FIntPoint(-5, 7), 0) == FVoxelMapTileId(FIntPoint(-5, 7), 0));
		int32 Inconsistent = 0, OutsideFootprint = 0, SampleMismatch = 0;
		for (int32 Y = -9; Y <= 9; Y += 3)
		{
			for (int32 X = -17; X <= 17; X += 2)
			{
				const FIntPoint Tile0(X, Y);
				for (int32 Level = 0; Level < 6; ++Level)
				{
					const FVoxelMapTileId Id = Frame.LevelTileOf(Tile0, Level);
					Inconsistent += Frame.ParentOf(Id) == Frame.LevelTileOf(Tile0, Level + 1) ? 0 : 1;
					const FIntPoint First = Frame.FirstLevel0Tile(Id);
					const int32 Span = 1 << Level;
					OutsideFootprint += (X >= First.X && X < First.X + Span && Y >= First.Y && Y < First.Y + Span
						&& Frame.CoversLevel0Tile(Id, Tile0, 0)) ? 0 : 1;
				}
			}
		}
		// A coarse grid point samples its level-0 pixel's corner, the apron included.
		for (int32 Level = 1; Level <= 4; ++Level)
		{
			const FVoxelMapTileId Id(FIntPoint(-3, 2), Level);
			for (const FIntPoint G : { FIntPoint(0, 0), FIntPoint(1, 1), FIntPoint(9, 4), FIntPoint(16, 16), FIntPoint(17, 17) })
			{
				const FVoxelMapTileSample Coarse = Frame.SampleAt(Id, G.X, G.Y);
				FIntPoint Tile0;
				int32 PX = 0, PY = 0;
				Frame.SourcePixel(Id, G.X, G.Y, Coarse, Tile0, PX, PY);
				const FVoxelMapTileSample Fine = Frame.SampleAt(Tile0, PX + 1, PY + 1);
				const bool bInRange = PX >= 0 && PY >= 0 && PX < Frame.ChunkSize && PY < Frame.ChunkSize;
				SampleMismatch += (bInRange && FMath::IsNearlyEqual(Coarse.X, Fine.X, 1.0f) && FMath::IsNearlyEqual(Coarse.Y, Fine.Y, 1.0f)) ? 0 : 1;
			}
		}
		TestEqual(TEXT("Plane: a tile's parent is its level-0 tile's tile a level up"), Inconsistent, 0);
		TestEqual(TEXT("Plane: a level's tile holds its level-0 tiles"), OutsideFootprint, 0);
		TestEqual(TEXT("Plane: a coarse grid point samples its level-0 pixel's corner"), SampleMismatch, 0);
		TestTrue(TEXT("Plane: level 0's sample is SampleAt's"), Frame.SampleAt(FVoxelMapTileId(FIntPoint(4, -2)), 3, 5).X == Frame.SampleAt(FIntPoint(4, -2), 3, 5).X);
		const FVoxelMapTileSample A = Frame.SampleAt(FVoxelMapTileId(FIntPoint(0, 0), 3), 1, 1);
		const FVoxelMapTileSample B = Frame.SampleAt(FVoxelMapTileId(FIntPoint(0, 0), 3), 2, 1);
		TestTrue(TEXT("Plane: a level-3 tile's pixels are 8 voxels apart"), FMath::IsNearlyEqual(B.X - A.X, 800.0f, 0.01f));
		TestEqual(TEXT("Plane: the pyramid has no top"), Frame.GetMaxLevel(), FVoxelMapTileFrame::MaxPlaneLevel);
		TestTrue(TEXT("Plane: an unbounded plane's tiles are valid at every level"), Frame.IsValidTile(FVoxelMapTileId(FIntPoint(-1000, 50), 7)));

		// The level matching a texel: 100 uu pixels.
		TestEqual(TEXT("Texels finer than pixels: level 0"), Frame.LevelForTexel(60.0), 0);
		TestEqual(TEXT("A texel of 1.9 pixels: level 0"), Frame.LevelForTexel(190.0), 0);
		TestEqual(TEXT("A texel of 2 pixels: level 1"), Frame.LevelForTexel(200.0), 1);
		TestEqual(TEXT("A texel of 16 tiles: level 8"), Frame.LevelForTexel(25600.0), 8);
	}

	// ---- A planet of 37 tiles a face edge ----
	{
		const FVoxelMapTileFrame Frame = PlanetFrame();
		const int32 N = Frame.Grid.GetCellsPerEdge();
		TestEqual(TEXT("Planet: 37 tiles a face edge: levels 0 to 6"), Frame.GetMaxLevel(), 6);
		TestEqual(TEXT("Planet: 19 tiles a face edge at level 1"), Frame.TilesPerFaceEdgeAt(1), 19);
		TestEqual(TEXT("Planet: 5 at level 3"), Frame.TilesPerFaceEdgeAt(3), 5);
		TestEqual(TEXT("Planet: one at the top"), Frame.TilesPerFaceEdgeAt(6), 1);
		int32 Inconsistent = 0, Invalid = 0, OutsideFootprint = 0, WrongFace = 0;
		for (int32 V = 0; V < N; ++V)
		{
			for (int32 X = 0; X < 6 * N; ++X)
			{
				const FIntPoint Tile0(X, V);
				const int32 Face = X / N;
				for (int32 Level = 0; Level <= Frame.GetMaxLevel(); ++Level)
				{
					const FVoxelMapTileId Id = Frame.LevelTileOf(Tile0, Level);
					Invalid += Frame.IsValidTile(Id) && Frame.CanDrawTile(Id) ? 0 : 1;
					WrongFace += Id.Coord.X / Frame.TilesPerFaceEdgeAt(Level) == Face ? 0 : 1;
					if (Level < Frame.GetMaxLevel())
					{
						Inconsistent += Frame.ParentOf(Id) == Frame.LevelTileOf(Tile0, Level + 1) ? 0 : 1;
					}
					const FIntPoint First = Frame.FirstLevel0Tile(Id);
					OutsideFootprint += (Frame.Grid.IsValidCell(First) && Frame.CoversLevel0Tile(Id, Tile0, 0)) ? 0 : 1;
				}
			}
		}
		TestEqual(TEXT("Planet: every level's tile is valid"), Invalid, 0);
		TestEqual(TEXT("Planet: a tile stays on its face"), WrongFace, 0);
		TestEqual(TEXT("Planet: a tile's parent is its level-0 tile's tile a level up"), Inconsistent, 0);
		TestEqual(TEXT("Planet: a level's tile holds its level-0 tiles, from a valid first cell"), OutsideFootprint, 0);
		TestFalse(TEXT("Planet: past a level's atlas is no tile"), Frame.IsValidTile(FVoxelMapTileId(FIntPoint(6 * 5, 0), 3)));
		TestFalse(TEXT("Planet: past the top is no tile"), Frame.IsValidTile(FVoxelMapTileId(FIntPoint(0, 0), 7)));

		// A coarse grid point's direction falls in the level-0 cell and pixel SourcePixel names (interior points on the
		// face; the apron and the empty part of an edge tile by direction).
		int32 Checked = 0, Wrong = 0;
		for (int32 Level = 1; Level <= 4; ++Level)
		{
			const int32 NL = Frame.TilesPerFaceEdgeAt(Level);
			for (const FIntPoint TileAt : { FIntPoint(0, 0), FIntPoint(NL + 1, 1), FIntPoint(3 * NL - 1, NL - 1), FIntPoint(5 * NL + NL / 2, NL / 2) })
			{
				const FVoxelMapTileId Id(TileAt, Level);
				for (int32 GY = 0; GY < Frame.ChunkSize + 2; GY += 3)
				{
					for (int32 GX = 0; GX < Frame.ChunkSize + 2; GX += 3)
					{
						const FVoxelMapTileSample Sample = Frame.SampleAt(Id, GX, GY);
						FIntPoint Tile0;
						int32 PX = 0, PY = 0;
						Frame.SourcePixel(Id, GX, GY, Sample, Tile0, PX, PY);
						FVector2D Fraction;
						const FIntPoint Cell = Frame.Grid.CellOfDirection(Sample.Direction, &Fraction);
						// Both in level-0 pixels along the face. The coarse sample lies on a level-0 pixel's edge (2^(L-1)
						// pixels into the coarse pixel), so rounding may put it a pixel (or a cell) lower.
						int32 Face = 0, IU = 0, IV = 0, Face0 = 0, IU0 = 0, IV0 = 0;
						Frame.Grid.FromAtlas(Cell, Face, IU, IV);
						Frame.Grid.FromAtlas(Tile0, Face0, IU0, IV0);
						const double GU = IU * Frame.ChunkSize + Fraction.X * Frame.ChunkSize;
						const double GV = IV * Frame.ChunkSize + Fraction.Y * Frame.ChunkSize;
						const double PU = IU0 * Frame.ChunkSize + PX + 0.5;
						const double PV = IV0 * Frame.ChunkSize + PY + 0.5;
						++Checked;
						Wrong += (Face == Face0 && FMath::Abs(GU - PU) <= 1.01 && FMath::Abs(GV - PV) <= 1.01) ? 0 : 1;
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("Planet: %d coarse grid points, %d off their level-0 pixel"), Checked, Wrong));
		TestEqual(TEXT("Planet: a coarse grid point lies in the level-0 pixel SourcePixel names"), Wrong, 0);

		const double Pixel = Frame.GetPixelWorldSize();
		TestEqual(TEXT("Planet: a texel of a pixel: level 0"), Frame.LevelForTexel(Pixel * 1.5), 0);
		TestEqual(TEXT("Planet: a texel of 4 pixels: level 2"), Frame.LevelForTexel(Pixel * 4.0), 2);
		TestEqual(TEXT("Planet: a texel of a face: the top"), Frame.LevelForTexel(Pixel * 1.0e6), 6);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
