// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCubeSphere.h"
#include "VoxelPolyline.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The cube sphere grid's neighbours by index (Documentation/HYDROLOGY_PLAN.md, H8's first step): a planet grid whose
// cells have the same neighbours from both sides of a cube face's edge.
//
// - Core.CubeSphere.AdjacentCells: on grids of several sizes, every cell's neighbour has the cell among its own
//   neighbours; no cell is its own neighbour or has one twice; a cell has eight, but for the three cells round each of
//   the cube's eight corners, which have seven; a side neighbour's centre is about a cell away and a corner
//   neighbour's under two, across an edge as on a face; how often Neighbour, the step
//   along the face's own plane, gives another cell is counted and logged (none on these grids: near a corner it lands
//   within rounding of a cell's border, which is why the walks that must agree use the index form). And the cap gather:
//   the cells whose centres lie within an angle of a direction, against a count of all cells, round a face's middle,
//   an edge and a corner.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCubeSphereAdjacentCellsTest,
	"VoxelWorlds.Core.CubeSphere.AdjacentCells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCubeSphereAdjacentCellsTest::RunTest(const FString& Parameters)
{
	static const int32 DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int32 DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	for (const int32 N : { 2, 3, 8, 21 })
	{
		const FVoxelCubeSphereGrid Grid(N);
		const double CellAngle = UE_DOUBLE_HALF_PI / N;
		int32 OneSided = 0, Repeated = 0, Self = 0, WithSeven = 0, WithOther = 0, FarSide = 0, FarCorner = 0, CrossEdge = 0;
		for (int32 Face = 0; Face < 6; ++Face)
		{
			for (int32 IV = 0; IV < N; ++IV)
			{
				for (int32 IU = 0; IU < N; ++IU)
				{
					const FIntPoint Cell = Grid.ToAtlas(Face, IU, IV);
					const FVector Centre = Grid.CellCentreDirection(Cell);
					TArray<FIntPoint, TInlineAllocator<8>> Neighbours;
					for (int32 D = 0; D < 8; ++D)
					{
						FIntPoint Other;
						if (!Grid.GetAdjacentCell(Cell, DX[D], DY[D], Other))
						{
							continue;
						}
						Self += Other == Cell ? 1 : 0;
						Repeated += Neighbours.Contains(Other) ? 1 : 0;
						Neighbours.Add(Other);
						CrossEdge += Other.X / N != Face ? 1 : 0;
						// The same relation from the other side.
						bool bBack = false;
						for (int32 Back = 0; Back < 8 && !bBack; ++Back)
						{
							FIntPoint Again;
							bBack = Grid.GetAdjacentCell(Other, DX[Back], DY[Back], Again) && Again == Cell;
						}
						OneSided += bBack ? 0 : 1;
						// A cube sphere's cells are no wider than at a face's middle.
						const double Angle = FVoxelPolylineBounds::AngleBetween(Centre, Grid.CellCentreDirection(Other));
						FarSide += (D < 4 && Angle > 1.05 * CellAngle) ? 1 : 0;
						// A corner step is the diagonal of a cell that is a rhombus near a cube's corner, where the grid's lines meet at 120 degrees.
						FarCorner += (D >= 4 && Angle > 1.75 * CellAngle) ? 1 : 0;
					}
					WithSeven += Neighbours.Num() == 7 ? 1 : 0;
					WithOther += (Neighbours.Num() != 7 && Neighbours.Num() != 8) ? 1 : 0;
				}
			}
		}
		const FString What = FString::Printf(TEXT("%d cells an edge"), N);
		TestEqual(*(What + TEXT(": a neighbour's neighbours hold the cell")), OneSided, 0);
		TestEqual(*(What + TEXT(": no cell is its own neighbour")), Self, 0);
		TestEqual(*(What + TEXT(": no neighbour comes twice")), Repeated, 0);
		TestEqual(*(What + TEXT(": the three cells round each of the cube's eight corners have seven neighbours")), WithSeven, 24);
		TestEqual(*(What + TEXT(": every other cell has eight")), WithOther, 0);
		TestEqual(*(What + TEXT(": a side neighbour is a cell away at most")), FarSide, 0);
		TestEqual(*(What + TEXT(": a corner neighbour is under two cells away")), FarCorner, 0);
		TestTrue(*(What + TEXT(": steps cross edges (non-vacuous)")), CrossEdge >= 24 * N);
	}

	// What the index form is for: Neighbour steps along a face's own plane and takes the cell the direction lands in,
	// which near a cube's corner is the border between two cells to within rounding. Counted here, not judged: on
	// these grids it gives the index form's cell every time; the hydrology's walks use the index form, which is exact.
	for (const int32 N : { 21, 64 })
	{
		const FVoxelCubeSphereGrid Grid(N);
		int32 Steps = 0, Differ = 0, PlaneOneSided = 0;
		for (int32 Face = 0; Face < 6; ++Face)
		{
			for (int32 IV = 0; IV < N; ++IV)
			{
				for (int32 IU = 0; IU < N; ++IU)
				{
					if (IU > 0 && IU < N - 1 && IV > 0 && IV < N - 1)
					{
						continue;
					}
					const FIntPoint Cell = Grid.ToAtlas(Face, IU, IV);
					for (int32 D = 0; D < 8; ++D)
					{
						FIntPoint ByIndex;
						if (!Grid.GetAdjacentCell(Cell, DX[D], DY[D], ByIndex) || ByIndex.X / N == Face)
						{
							continue;
						}
						++Steps;
						const FIntPoint ByPlane = Grid.Neighbour(Cell, DX[D], DY[D]);
						Differ += ByPlane != ByIndex ? 1 : 0;
						bool bBack = false;
						for (int32 Back = 0; Back < 8 && !bBack; ++Back)
						{
							bBack = Grid.Neighbour(ByPlane, DX[Back], DY[Back]) == Cell;
						}
						PlaneOneSided += bBack ? 0 : 1;
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%d cells an edge: of %d steps across an edge, the step along the face's plane gives another cell than the index form for %d, and a cell that does not give the first back for %d"),
			N, Steps, Differ, PlaneOneSided));
	}

	// The cap gather against a count of all cells.
	{
		const FVoxelCubeSphereGrid Grid(13);
		const FVector Places[4] = { FVector(1.0, 0.05, -0.1), FVector(1.0, 1.0, 0.2), FVector(1.0, 1.0, 1.0), FVector(-0.3, -1.0, 0.98) };
		for (const FVector& Raw : Places)
		{
			const FVector Direction = Raw.GetSafeNormal();
			for (const double Angle : { 0.0, 0.07, 0.3, 1.1 })
			{
				TArray<FIntPoint> Gathered;
				Grid.GatherCentresWithinAngle(Direction, Angle, Gathered);
				TSet<FIntPoint> Expected;
				Expected.Add(Grid.CellOfDirection(Direction));
				for (int32 Face = 0; Face < 6; ++Face)
				{
					for (int32 IV = 0; IV < 13; ++IV)
					{
						for (int32 IU = 0; IU < 13; ++IU)
						{
							const FIntPoint Cell = Grid.ToAtlas(Face, IU, IV);
							if (FVector::DotProduct(Grid.CellCentreDirection(Cell), Direction) >= FMath::Cos(Angle))
							{
								Expected.Add(Cell);
							}
						}
					}
				}
				const TSet<FIntPoint> Got(Gathered);
				TestTrue(*FString::Printf(TEXT("a cap of %.2f rad about (%.2f, %.2f, %.2f): the cells whose centres lie in it, each once (%d, expected %d)"),
					Angle, Direction.X, Direction.Y, Direction.Z, Gathered.Num(), Expected.Num()),
					Got.Num() == Gathered.Num() && Got.Num() == Expected.Num() && Got.Includes(Expected));
			}
		}
		TArray<FIntPoint> All;
		Grid.GatherCentresWithinAngle(FVector::UpVector, UE_DOUBLE_PI, All);
		TestEqual(TEXT("a cap of the whole sphere: every cell"), static_cast<int64>(All.Num()), Grid.GetNumCells());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
