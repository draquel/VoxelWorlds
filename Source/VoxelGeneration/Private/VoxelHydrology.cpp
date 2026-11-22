// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelHydrology.h"
#include "IVoxelWorldMode.h"
#include "VoxelBlockSurface.h"
#include "VoxelHash.h"
#include "VoxelRiverCourse.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelPolyline.h"
#include <limits>

namespace VoxelHydrologyPrivate
{
	/** The eight neighbours of a cell, the four sides first. The order decides every tie, so it is part of what a world generates. */
	static const int32 DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int32 DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

	bool CellLess(const FIntPoint& A, const FIntPoint& B)
	{
		return A.X != B.X ? A.X < B.X : A.Y < B.Y;
	}

	/** A cell of a flood: its ground and itself. The lowest first; of two at one height, the lesser cell. */
	struct FFloodEntry
	{
		double Height = 0.0;
		FIntPoint Cell = FIntPoint::ZeroValue;

		bool operator<(const FFloodEntry& Other) const
		{
			return Height != Other.Height ? Height < Other.Height : CellLess(Cell, Other.Cell);
		}
	};

	/** The distance from a place to a region of its world's kind (0 inside it): to a box, or along the datum sphere to a cap. */
	double DistanceToRegion(const FVoxelConditioningRegion& Region, const FVoxelHydrologyGround& Ground, const FVector& Place)
	{
		if (Region.bCap)
		{
			return Ground.PlanetRadius * FMath::Max(0.0, FVoxelPolylineBounds::AngleBetween(Region.Direction, Place) - Region.AngularRadius);
		}
		return FMath::Sqrt(Region.Box.ComputeSquaredDistanceToPoint(FVector2D(Place.X, Place.Y)));
	}

	/** Whether a cell's ground lies under the water of a kept lake: under its level, inside its footprint. */
	bool IsUnderLake(const FVoxelHydrologyLake& Lake, FVoxelHydrologyHeights& Heights, const FIntPoint& Cell)
	{
		if (!Lake.bKept || !Lake.Body.Lake.IsValid())
		{
			return false;
		}
		// A block world's column is dry when its topmost block's top is at the water's.
		const FVoxelHydrologyGround& Ground = Heights.GetGround();
		if ((Ground.bBlocks ? Ground.BlockTopFor(Heights.At(Cell)) : Heights.At(Cell)) >= Lake.WaterLevel)
		{
			return false;
		}
		const FVector Place = Heights.PlaceOf(Cell);
		return Ground.bRadial ? Lake.Body.Lake->CoversDirection(Place) : Lake.Body.Lake->Covers(Place.X, Place.Y);
	}
}

// ==================== The ground ====================

FVoxelHydrologyGround FVoxelHydrologyGround::FromConfig(const UVoxelWorldConfiguration& Config, const TSharedRef<const IVoxelWorldMode>& WorldMode)
{
	FVoxelHydrologyGround Ground;
	const FVoxelNoiseParams Noise = Config.GetNoiseParams();
	Ground.Height = [WorldMode, Noise](double X, double Y)
	{
		return WorldMode->GetSurfaceOnUpLine(FVector(X, Y, 0.0), Noise).Position.Z;
	};
	const FVoxelSeaSurface Sea = FVoxelWaterLevels::FromConfig(Config).GetSea();
	Ground.VoxelSize = Config.VoxelSize;
	Ground.ChunkWorldSize = Config.GetChunkWorldSize();
	Ground.Seed = Noise.Seed;
	if (Config.WorldMode == EWorldMode::SphericalPlanet)
	{
		// The ground along the radial: the planet's own altitude function over its datum sphere.
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(Config);
		Ground.bRadial = true;
		Ground.PlanetCenter = Planet.PlanetCenter;
		Ground.PlanetRadius = FMath::Max(static_cast<double>(Planet.PlanetRadius), 1.0);
		Ground.Height = nullptr;
		Ground.Altitude = [WorldMode, Noise](const FVector& Direction)
		{
			return static_cast<double>(static_cast<const FSphericalPlanetWorldMode&>(WorldMode.Get()).GetTerrainAltitude(Direction, Noise));
		};
		Ground.bSea = Sea.bEnabled && Sea.bRadial;
		Ground.SeaLevel = Sea.Radius - Ground.PlanetRadius;
		// The valleys are in the planet's altitude already (GetTerrainAltitude); the network gives their rivers (V5)
		Ground.Valleys = WorldMode->GetValleyNetwork();
		return Ground;
	}
	Ground.bSea = Sea.bEnabled && !Sea.bRadial;
	Ground.SeaLevel = Sea.PlaneZ;
	Ground.bBlocks = FVoxelBlockSurface::UsesBlockSurface(Config);
	// The valleys are in the world mode's height already; the network gives their rivers (the valleys plan's V3)
	Ground.Valleys = WorldMode->GetValleyNetwork();
	Ground.BlockOrigin = Config.WorldOrigin;
	return Ground;
}

double FVoxelHydrologyGround::BlockTopFor(double InHeight) const
{
	return bBlocks ? FVoxelBlockSurface::BlockTopForHeight(static_cast<float>(InHeight), BlockOrigin.Z, static_cast<float>(VoxelSize)) : InHeight;
}

double FVoxelHydrologyGround::StandingHeightAt(const FVector& Place) const
{
	if (!bBlocks)
	{
		return HeightAt(Place);
	}
	// A block column's height is read at its lower corner, as the generators read it.
	const float Voxel = static_cast<float>(VoxelSize);
	return BlockTopFor(Height(FVoxelBlockSurface::ColumnCorner(Place.X, BlockOrigin.X, Voxel), FVoxelBlockSurface::ColumnCorner(Place.Y, BlockOrigin.Y, Voxel)));
}

FVoxelWaterFrame FVoxelHydrologyGround::GetFrame() const
{
	FVoxelWaterFrame Frame;
	Frame.bRadial = bRadial;
	Frame.Center = PlanetCenter;
	Frame.DatumRadius = PlanetRadius;
	return Frame;
}

FVoxelSeaSurface FVoxelHydrologyGround::GetSea() const
{
	if (!bSea)
	{
		return FVoxelSeaSurface();
	}
	return bRadial ? FVoxelSeaSurface::MakeSphere(PlanetCenter, PlanetRadius + SeaLevel) : FVoxelSeaSurface::MakePlane(SeaLevel);
}

// ==================== The grid ====================

FVoxelHydrologyGrid::FVoxelHydrologyGrid(const FVoxelHydrologyGround& Ground, const FVoxelHydrologySettings& Settings)
	: bRadial(Ground.bRadial)
	, CellSize(FMath::Max(Settings.CellVoxels, 1) * FMath::Max(Ground.VoxelSize, 1.0))
	, PlanetRadius(Ground.PlanetRadius)
	, Spacing(FMath::Max(Settings.SpringSpacingCells, 1))
{
	if (bRadial)
	{
		// As many lattice squares along a face's edge as bring a cell nearest the width asked for, along the face's
		// middle lines (a quarter of the equator), and at least one. The cells are then whole squares on every face.
		const double Quarter = UE_DOUBLE_HALF_PI * FMath::Max(PlanetRadius, 1.0);
		const int32 SquaresPerEdge = FMath::Max(1, FMath::RoundToInt32(Quarter / (CellSize * Spacing)));
		Squares = FVoxelCubeSphereGrid(SquaresPerEdge);
		Cells = FVoxelCubeSphereGrid(SquaresPerEdge * Spacing);
		CellSize = Quarter / (SquaresPerEdge * Spacing);
	}
}

FVector FVoxelHydrologyGrid::PlaceOf(const FIntPoint& Cell) const
{
	return bRadial ? Cells.CellCentreDirection(Cell) : FVector(Cell.X * CellSize, Cell.Y * CellSize, 0.0);
}

FIntPoint FVoxelHydrologyGrid::CellAt(const FVector& Place) const
{
	return bRadial ? Cells.CellOfDirection(Place) : FIntPoint(FMath::RoundToInt32(Place.X / CellSize), FMath::RoundToInt32(Place.Y / CellSize));
}

bool FVoxelHydrologyGrid::GetNeighbour(const FIntPoint& Cell, int32 Index, FIntPoint& OutNeighbour) const
{
	using namespace VoxelHydrologyPrivate;
	if (bRadial)
	{
		return Cells.GetAdjacentCell(Cell, DX[Index], DY[Index], OutNeighbour);
	}
	OutNeighbour = FIntPoint(Cell.X + DX[Index], Cell.Y + DY[Index]);
	return true;
}

double FVoxelHydrologyGrid::GetStepLength(const FIntPoint& A, const FIntPoint& B) const
{
	if (bRadial)
	{
		return PlanetRadius * FVoxelPolylineBounds::AngleBetween(Cells.CellCentreDirection(A), Cells.CellCentreDirection(B));
	}
	return CellSize * FMath::Sqrt(static_cast<double>(FMath::Square(A.X - B.X) + FMath::Square(A.Y - B.Y)));
}

FIntPoint FVoxelHydrologyGrid::GetSquareOrigin(const FIntPoint& Square) const
{
	if (bRadial)
	{
		int32 Face = 0, IU = 0, IV = 0;
		Squares.FromAtlas(Square, Face, IU, IV);
		return Cells.ToAtlas(Face, IU * Spacing, IV * Spacing);
	}
	return FIntPoint(Square.X * Spacing, Square.Y * Spacing);
}

double FVoxelHydrologyGrid::GetDistance(const FVector& A, const FVector& B) const
{
	return bRadial ? PlanetRadius * FVoxelPolylineBounds::AngleBetween(A, B) : FVector2D::Distance(FVector2D(A.X, A.Y), FVector2D(B.X, B.Y));
}

FVoxelHydrologyHeights::FVoxelHydrologyHeights(const FVoxelHydrologyGround& InGround, const FVoxelHydrologySettings& InSettings)
	: Ground(InGround)
	, Grid(InGround, InSettings)
{
}

double FVoxelHydrologyHeights::At(const FIntPoint& Cell)
{
	if (const double* Found = Kept.Find(Cell))
	{
		return *Found;
	}
	++NumSamples;
	const double CellSize = Grid.GetCellSize();
	const double Height = Ground.bRadial ? Ground.Altitude(Grid.PlaceOf(Cell)) : Ground.Height(Cell.X * CellSize, Cell.Y * CellSize);
	Kept.Add(Cell, Height);
	return Height;
}

bool FVoxelHydrologyHeights::GetDownhill(const FIntPoint& Cell, FIntPoint& OutNext)
{
	const double Here = At(Cell);
	double BestFall = 0.0;
	bool bFound = false;
	for (int32 D = 0; D < 8; ++D)
	{
		FIntPoint Neighbour;
		if (!Grid.GetNeighbour(Cell, D, Neighbour))
		{
			continue;
		}
		// The fall a unit of length: a cell's width on a heightmap world (a corner step root two of them), the arc
		// between the two cells on a planet, whose cells are not all one size.
		const double Fall = Ground.bRadial
			? (Here - At(Neighbour)) / FMath::Max(Grid.GetStepLength(Cell, Neighbour), 1.0)
			: (Here - At(Neighbour)) / (D < 4 ? 1.0 : UE_DOUBLE_SQRT_2);
		if (Fall > BestFall)
		{
			BestFall = Fall;
			OutNext = Neighbour;
			bFound = true;
		}
	}
	return bFound;
}

// ==================== The setting ====================

bool FVoxelHydrology::IsEnabled(const UVoxelWorldConfiguration& Config)
{
	return Config.HydrologyConfiguration != nullptr;
}

FString FVoxelHydrology::GetFingerprintTerm(const UVoxelWorldConfiguration& Config)
{
	if (!IsEnabled(Config))
	{
		return FString();
	}
	return FString::Printf(TEXT("Hydrology=%d:%08X"), Version, Config.HydrologyConfiguration->Settings.GetGenerationHash());
}

FVoxelHydrologySettings FVoxelHydrology::Sanitize(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground)
{
	FVoxelHydrologySettings Out = Settings;
	Out.CellVoxels = FMath::Clamp(Out.CellVoxels, 2, 64);
	Out.SpringSpacingCells = FMath::Clamp(Out.SpringSpacingCells, 2, 512);
	Out.MaxRiverLength = FMath::Max(Out.MaxRiverLength, 1000.0f);
	Out.MinRiverLength = FMath::Clamp(Out.MinRiverLength, 0.0f, Out.MaxRiverLength);
	Out.MaxHalfWidthVoxels = FMath::Max(Out.MaxHalfWidthVoxels, Out.HalfWidthVoxels);
	// The LOD window's slab allows a level a chunk and a half over the ground of its column.
	Out.MaxLakeDepth = FMath::Clamp(Out.MaxLakeDepth, 100.0f, static_cast<float>(1.25 * Ground.ChunkWorldSize));
	// A lake no longer than half the longest river's length, corner to corner: a gather over a region then finds
	// every lake that reaches it from the springs within twice that length.
	const double CellSize = Out.CellVoxels * FMath::Max(Ground.VoxelSize, 1.0);
	Out.MaxLakeSpanCells = FMath::Clamp(Out.MaxLakeSpanCells, 8, FMath::Max(8, FMath::FloorToInt32(Out.MaxRiverLength / (2.0 * UE_DOUBLE_SQRT_2 * CellSize))));
	Out.MaxLakeCells = FMath::Clamp(Out.MaxLakeCells, 16, 65536);
	Out.TileCells = FMath::Clamp(Out.TileCells, 16, 512);
	return Out;
}

double FVoxelHydrology::GetDependencyDistance(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground)
{
	const double CellSize = Settings.CellVoxels * FMath::Max(Ground.VoxelSize, 1.0);
	return 2.0 * Settings.MaxRiverLength + 2.0 * CellSize;
}

void FVoxelHydrology::GetSquaresFor(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground, const FVoxelConditioningRegion& Region,
	TArray<FIntPoint>& OutSquares)
{
	OutSquares.Reset();
	const double Distance = GetDependencyDistance(Settings, Ground);
	if (Ground.bRadial)
	{
		if (!Region.bCap)
		{
			return;
		}
		// A square's spring lies anywhere in it: every square whose centre is within the distance and the square's own
		// half width, corner to corner (a cube sphere's cell is no wider than its width at a face's middle).
		const FVoxelHydrologyGrid Grid(Ground, Settings);
		const double Radius = FMath::Max(Ground.PlanetRadius, 1.0);
		const double HalfSquare = UE_DOUBLE_INV_SQRT_2 * Settings.SpringSpacingCells * Grid.GetCellSize();
		Grid.GetSquares().GatherCentresWithinAngle(Region.Direction, Region.AngularRadius + (Distance + 1.1 * HalfSquare) / Radius, OutSquares);
		return;
	}
	if (Region.bCap || Region.bEverywhere || !Region.Box.bIsValid)
	{
		return;
	}
	const double CellSize = Settings.CellVoxels * FMath::Max(Ground.VoxelSize, 1.0);
	const double Square = Settings.SpringSpacingCells * CellSize;
	// A square's spring lies anywhere in it: every square that comes within the distance.
	const FIntRect Rect(
		FMath::FloorToInt32((Region.Box.Min.X - Distance) / Square), FMath::FloorToInt32((Region.Box.Min.Y - Distance) / Square),
		FMath::FloorToInt32((Region.Box.Max.X + Distance) / Square) + 1, FMath::FloorToInt32((Region.Box.Max.Y + Distance) / Square) + 1);
	OutSquares.Reserve(Rect.Width() * Rect.Height());
	for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
	{
		for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
		{
			OutSquares.Add(FIntPoint(X, Y));
		}
	}
}

bool FVoxelHydrology::MeetsRegion(const FVoxelWaterBody& Body, const FVoxelConditioningRegion& Region, const FVoxelHydrologyGround& Ground)
{
	return Region.bCap ? Body.MeetsCap(Region.Direction, Region.AngularRadius, Ground.PlanetRadius) : Body.MeetsRegion(Region.Box);
}

bool FVoxelHydrology::MeetsRegion(const FVoxelConditioningLine& Bed, const FVoxelConditioningRegion& Region, const FVoxelHydrologyGround& Ground)
{
	return Region.bCap ? Bed.MeetsCap(Region.Direction, Region.AngularRadius, Ground.PlanetRadius) : Bed.MeetsRegion(Region.Box);
}

double FVoxelHydrology::GetBlockBedGrade()
{
	// Adjacent columns, diagonals included, are up to a voxel times root two apart along a line: a grade under one over
	// root two keeps the block tops to a block a column (the engine's limit, FVoxelConditioningLine::GetWarnings).
	return FVoxelConditioningLineShape::BoundMargin * UE_DOUBLE_INV_SQRT_2;
}

uint32 FVoxelHydrology::MakeReachId(int32 Seed, const FIntPoint& StartCell)
{
	const uint32 Id = VoxelHash::Lattice(static_cast<uint32>(StartCell.X), static_cast<uint32>(StartCell.Y), 0x52495645u, Seed);
	return Id != FVoxelWaterBody::SeaId ? Id : 1u;
}

uint32 FVoxelHydrology::MakeLakeId(int32 Seed, const FIntPoint& LowestCell, int32 NumCells)
{
	const uint32 Id = VoxelHash::Lattice(static_cast<uint32>(LowestCell.X), static_cast<uint32>(LowestCell.Y), 0x4C414B45u + static_cast<uint32>(NumCells) * 0x9E3779B9u, Seed);
	return Id != FVoxelWaterBody::SeaId ? Id : 1u;
}

int64 FVoxelHydrology::MakeBedLineId(const FIntPoint& StartCell, bool bAtlas)
{
	// 21 bits a coordinate (cells to a million either way of the origin; a planet's atlas coordinate counts up from 0,
	// to two million: six faces of a 1,000 km planet's cells), counted up from -2^60: over every road's and trail's
	// (under -2^61) and under every hand-placed line's of order -2^27 or more.
	const int32 Offset = bAtlas ? 0 : (1 << 20);
	const auto Bits = [Offset](int32 Coordinate) { return static_cast<int64>((Coordinate + Offset) & 0x1FFFFF); };
	return -(static_cast<int64>(1) << 60) + ((Bits(StartCell.X) << 21) | Bits(StartCell.Y));
}

// ==================== Springs ====================

bool FVoxelHydrology::GetSpring(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Square, FIntPoint& OutCell)
{
	const FVoxelHydrologyGround& Ground = Heights.GetGround();
	const int32 Spacing = FMath::Max(Settings.SpringSpacingCells, 1);
	const uint32 X = static_cast<uint32>(Square.X);
	const uint32 Y = static_cast<uint32>(Square.Y);
	const int32 OffsetX = FMath::Min(Spacing - 1, FMath::FloorToInt32(VoxelHash::Unit(VoxelHash::Lattice(X, Y, 0x53505258u, Ground.Seed)) * Spacing));
	const int32 OffsetY = FMath::Min(Spacing - 1, FMath::FloorToInt32(VoxelHash::Unit(VoxelHash::Lattice(X, Y, 0x53505259u, Ground.Seed)) * Spacing));
	// A square's cells lie on one face of a planet, so the offset stays on it.
	const FIntPoint Origin = Heights.GetGrid().GetSquareOrigin(Square);
	OutCell = FIntPoint(Origin.X + OffsetX, Origin.Y + OffsetY);
	if (!Ground.bSea)
	{
		return true;
	}
	return Heights.At(OutCell) >= Ground.SeaLevel + Settings.MinSpringHeight;
}

namespace VoxelHydrologyPrivate
{
	/**
	 * The grid a lake's water is worked out on: finer than the generator's, in world XY on a heightmap world (a block
	 * world: its block columns) and on a planet in the plane tangent to the datum sphere at the lake's lowest cell,
	 * the plane a planet lake's footprint lies in (FVoxelWaterLake::MakeOnSphere).
	 */
	struct FLakeFineGrid
	{
		bool bRadial = false;

		/** A fine cell's width */
		double Fine = 200.0;

		/** A heightmap world: where the fine cell (0, 0) is (a block world's columns are counted from the world's origin) */
		FVector2D Offset = FVector2D::ZeroVector;

		/** A planet: the lake's centre, the tangent plane's two axes and the datum radius */
		FVector Centre = FVector::UpVector;
		FVector AxisX = FVector::ForwardVector;
		FVector AxisY = FVector::RightVector;
		double Radius = 1.0;

		/** @return A fine cell's point on the grid's plane (world units): its middle, or on a block world its column's lower corner */
		FVector2D PointOf(const FIntPoint& FineCell) const
		{
			return FVector2D(Offset.X + FineCell.X * Fine, Offset.Y + FineCell.Y * Fine);
		}

		/** @return The place a point of the grid's plane stands for: the column, or the direction whose datum point lies over it */
		FVector PlaceOf(const FVector2D& Point) const
		{
			if (!bRadial)
			{
				return FVector(Point.X, Point.Y, 0.0);
			}
			const double U = FMath::Clamp(Point.X / Radius, -1.0, 1.0);
			const double V = FMath::Clamp(Point.Y / Radius, -1.0, 1.0);
			return AxisX * U + AxisY * V + Centre * FMath::Sqrt(FMath::Max(0.0, 1.0 - U * U - V * V));
		}
	};

	/**
	 * The water of a lake whose hollow the flood on the generator's grid found: its level, its footprint and its
	 * record, from a second flood on a finer grid (two voxels a cell) inside the hollow and one cell around it.
	 *
	 * The generator's cell is too coarse for a rim: the lowest ground of a pass lies between its cells, and a
	 * footprint of such cells reaches over a narrow rim onto the low ground beyond, where water would stand at the
	 * lake's level. The fine flood starts at the pit under the hollow's lowest cell and takes the lowest cell first
	 * until it leaves the hollow's surroundings: the level it had climbed to then is the lowest ground every way out
	 * crosses, the rim, and the cells it took under that level are the water. The footprint is those cells and one
	 * fine cell around them; the level is a margin under the rim, and under every ground on the footprint's edge, read
	 * a voxel apart.
	 *
	 * On a planet the fine grid lies in the plane tangent at the hollow's lowest cell, and a fine cell belongs to the
	 * generator's cell its place lies in. On a block world a fine cell is a block column and its ground the top of the
	 * column's topmost block: the rim holds at its own height, the water's top is a block layer's (the lowest block top
	 * on the way out, and no higher than the lowest column beside the footprint), and the body's level lies a tenth of
	 * a voxel under it, inside the layer whose blocks are the water's top ones.
	 */
	void MakeLakeBody(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, FVoxelHydrologyLake& Lake)
	{
		const FVoxelHydrologyGround& Ground = Heights.GetGround();
		const FVoxelHydrologyGrid& Grid = Heights.GetGrid();
		const double CellSize = Heights.GetCellSize();
		const double Voxel = FMath::Max(Ground.VoxelSize, 1.0);
		const bool bBlocks = Ground.bBlocks;
		const bool bRadial = Ground.bRadial;
		const double Margin = Settings.LakeLevelMarginVoxels * Voxel;
		const double MinDepth = Settings.MinLakeDepthVoxels * Voxel;
		// The top of the block layer a height lies in or on, from below: a block world's levels are layer tops.
		const auto LayerTopUnder = [&Ground, Voxel](double Height)
		{
			return Ground.BlockOrigin.Z + FMath::FloorToDouble((Height - Ground.BlockOrigin.Z) / Voxel) * Voxel;
		};

		// What the hollow could hold at most, on the generator's grid: too little, and the fine flood is not run.
		Lake.WaterLevel = Lake.Closed == 1 ? Lake.RimLevel : Lake.RimLevel - Margin;
		Lake.Depth = FMath::Max(Lake.RimLevel - Lake.LowestGround, 0.0);
		Lake.Area = Lake.Cells.Num() * CellSize * CellSize;
		if (Lake.Depth < 0.5 * MinDepth || Lake.Area < 0.5 * Settings.MinLakeArea)
		{
			return;
		}

		// The fine grid: two voxels a cell where the generator's cell is a whole number of them; a block world's columns.
		const int32 FineVoxels = (!bBlocks && Settings.CellVoxels >= 4 && Settings.CellVoxels % 2 == 0) ? 2 : 1;
		const int32 PerCell = Settings.CellVoxels / FineVoxels;
		FLakeFineGrid FineGrid;
		FineGrid.bRadial = bRadial;
		FineGrid.Fine = FineVoxels * Voxel;
		const double Fine = FineGrid.Fine;
		if (bRadial)
		{
			// The plane and the axes a planet lake's footprint is read in: an empty footprint at the centre gives them.
			FineGrid.Radius = FMath::Max(Ground.PlanetRadius, 1.0);
			FVoxelWaterLake::MakeOnSphere(Heights.PlaceOf(Lake.LowestCell), FineGrid.Radius, Fine, FIntPoint::ZeroValue, TBitArray<>())
				->GetTangentFrame(FineGrid.AxisX, FineGrid.AxisY, FineGrid.Centre);
		}
		else if (bBlocks)
		{
			// The columns are counted from the world's origin: the fine cell (0, 0) is the column at or after world 0.
			FineGrid.Offset = FVector2D(FMath::Fmod(Ground.BlockOrigin.X, Voxel), FMath::Fmod(Ground.BlockOrigin.Y, Voxel));
		}
		TSet<FIntPoint> Domain;
		Domain.Reserve(Lake.Cells.Num() * 2);
		for (const FIntPoint& Cell : Lake.Cells)
		{
			Domain.Add(Cell);
			for (int32 D = 0; D < 8; ++D)
			{
				FIntPoint Neighbour;
				if (Grid.GetNeighbour(Cell, D, Neighbour))
				{
					Domain.Add(Neighbour);
				}
			}
		}
		const auto InDomain = [&Domain, &Grid, &FineGrid, PerCell, bRadial](const FIntPoint& FineCell)
		{
			if (bRadial)
			{
				return Domain.Contains(Grid.CellAt(FineGrid.PlaceOf(FineGrid.PointOf(FineCell))));
			}
			return Domain.Contains(FIntPoint(FMath::RoundToInt32(static_cast<double>(FineCell.X) / PerCell), FMath::RoundToInt32(static_cast<double>(FineCell.Y) / PerCell)));
		};
		TMap<FIntPoint, double> FineHeights;
		const auto HeightAt = [&FineHeights, &Ground, &FineGrid](const FIntPoint& FineCell)
		{
			if (const double* Found = FineHeights.Find(FineCell))
			{
				return *Found;
			}
			const double Height = Ground.StandingHeightAt(FineGrid.PlaceOf(FineGrid.PointOf(FineCell)));
			FineHeights.Add(FineCell, Height);
			return Height;
		};

		// Down from the hollow's lowest cell to the pit under it. A planet's fine grid is centred on that cell.
		FIntPoint Start = bRadial ? FIntPoint::ZeroValue : FIntPoint(Lake.LowestCell.X * PerCell, Lake.LowestCell.Y * PerCell);
		for (int32 Guard = 4 * PerCell * (Lake.Cells.Num() + 8); Guard > 0; --Guard)
		{
			const double Here = HeightAt(Start);
			double BestFall = 0.0;
			FIntPoint Best = Start;
			for (int32 D = 0; D < 8; ++D)
			{
				const FIntPoint Neighbour(Start.X + DX[D], Start.Y + DY[D]);
				const double Fall = (Here - HeightAt(Neighbour)) / (D < 4 ? 1.0 : UE_DOUBLE_SQRT_2);
				if (Fall > BestFall && InDomain(Neighbour))
				{
					BestFall = Fall;
					Best = Neighbour;
				}
			}
			if (Best == Start)
			{
				break;
			}
			Start = Best;
		}

		TArray<FFloodEntry> Heap;
		TArray<FFloodEntry> Taken;
		TSet<FIntPoint> Seen;
		Heap.HeapPush(FFloodEntry{ HeightAt(Start), Start });
		Seen.Add(Start);
		double Level = HeightAt(Start);
		double Lowest = Level;
		double LowestBeyond = Level;
		int32 BeyondStart = INDEX_NONE;
		int32 NumHollow = 0;
		double Rim = Level;
		bool bDepthCapped = false;
		bool bDone = false;
		while (Heap.Num() > 0)
		{
			FFloodEntry Entry;
			Heap.HeapPop(Entry, EAllowShrinking::No);
			if (!InDomain(Entry.Cell))
			{
				// Out of the hollow's surroundings: the level so far is the lowest ground every way out crosses, or this
				// cell's own when the flood had nowhere lower left to go.
				Rim = FMath::Max(Level, Entry.Height);
				NumHollow = (Entry.Height < Level && BeyondStart != INDEX_NONE) ? BeyondStart : Taken.Num();
				bDone = true;
				break;
			}
			if (Entry.Height > Level)
			{
				if (BeyondStart != INDEX_NONE)
				{
					Lowest = FMath::Min(Lowest, LowestBeyond);
					BeyondStart = INDEX_NONE;
				}
				if (Entry.Height - Lowest > Settings.MaxLakeDepth)
				{
					Rim = Lowest + Settings.MaxLakeDepth;
					NumHollow = Taken.Num();
					bDepthCapped = true;
					bDone = true;
					break;
				}
				Level = Entry.Height;
			}
			else if (Entry.Height < Level)
			{
				if (BeyondStart == INDEX_NONE)
				{
					BeyondStart = Taken.Num();
					LowestBeyond = Entry.Height;
				}
				LowestBeyond = FMath::Min(LowestBeyond, Entry.Height);
			}
			Taken.Add(Entry);
			for (int32 D = 0; D < 8; ++D)
			{
				const FIntPoint Neighbour(Entry.Cell.X + DX[D], Entry.Cell.Y + DY[D]);
				bool bAlready = false;
				Seen.Add(Neighbour, &bAlready);
				if (!bAlready)
				{
					Heap.HeapPush(FFloodEntry{ HeightAt(Neighbour), Neighbour });
				}
			}
		}
		if (!bDone)
		{
			return;
		}
		// Never over the rim the hollow was found with (on a block world, the top of the layer that rim lies in): the
		// cells the river's ways are laid over lie under that one. The water is then what the flood had taken when it
		// first climbed past it.
		const double RimCap = bBlocks ? LayerTopUnder(Lake.RimLevel) + Voxel : Lake.RimLevel;
		if (Rim > RimCap)
		{
			Rim = RimCap;
			bDepthCapped = Lake.Closed == 1;
			for (int32 Index = 0; Index < NumHollow; ++Index)
			{
				if (Taken[Index].Height > Rim)
				{
					NumHollow = Index;
					break;
				}
			}
		}

		// The water: the cells taken under the level. A cell beyond a saddle the flood did not climb back over is not
		// the lake's.
		const double FirstLevel = bBlocks ? LayerTopUnder(Rim) : (bDepthCapped ? Rim : Rim - Margin);
		TSet<FIntPoint> Footprint;
		FIntPoint FootMin(TNumericLimits<int32>::Max(), TNumericLimits<int32>::Max());
		FIntPoint FootMax(TNumericLimits<int32>::Lowest(), TNumericLimits<int32>::Lowest());
		const auto AddFoot = [&](const FIntPoint& Cell)
		{
			Footprint.Add(Cell);
			FootMin = FIntPoint(FMath::Min(FootMin.X, Cell.X), FMath::Min(FootMin.Y, Cell.Y));
			FootMax = FIntPoint(FMath::Max(FootMax.X, Cell.X), FMath::Max(FootMax.Y, Cell.Y));
		};
		double LowestWet = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < NumHollow; ++Index)
		{
			const FFloodEntry& Entry = Taken[Index];
			if (Entry.Height >= FirstLevel)
			{
				continue;
			}
			LowestWet = FMath::Min(LowestWet, Entry.Height);
			AddFoot(Entry.Cell);
			for (int32 D = 0; D < 8; ++D)
			{
				AddFoot(FIntPoint(Entry.Cell.X + DX[D], Entry.Cell.Y + DY[D]));
			}
		}
		if (Footprint.Num() == 0)
		{
			Lake.Depth = 0.0;
			Lake.Area = 0.0;
			return;
		}
		// The edge: each side of a footprint cell's square that no other footprint cell shares, read a voxel apart. A
		// cell's square is centred on its place. On a block world a cell is a column, and the edge the columns beside
		// the footprint: their block tops.
		double LowestEdge = TNumericLimits<double>::Max();
		for (const FIntPoint& Cell : Footprint)
		{
			for (int32 D = 0; D < 4; ++D)
			{
				const FIntPoint Beside(Cell.X + DX[D], Cell.Y + DY[D]);
				if (Footprint.Contains(Beside))
				{
					continue;
				}
				if (bBlocks)
				{
					LowestEdge = FMath::Min(LowestEdge, HeightAt(Beside));
					continue;
				}
				const FVector2D Centre = FineGrid.PointOf(Cell);
				const FVector2D Out(DX[D], DY[D]);
				const FVector2D Along(-Out.Y, Out.X);
				const FVector2D From = Centre + Out * (0.5 * Fine) - Along * (0.5 * Fine);
				for (int32 Step = 0; Step <= FineVoxels; ++Step)
				{
					const FVector2D Point = From + Along * (Fine * Step / FineVoxels);
					LowestEdge = FMath::Min(LowestEdge, Ground.HeightAt(FineGrid.PlaceOf(Point)));
				}
			}
		}
		Lake.WaterLevel = FMath::Min(FirstLevel, bBlocks ? LowestEdge : LowestEdge - Margin);
		Lake.LoweredForEdge = FirstLevel - Lake.WaterLevel;
		Lake.NumWetCells = 0;
		for (int32 Index = 0; Index < NumHollow; ++Index)
		{
			Lake.NumWetCells += Taken[Index].Height < Lake.WaterLevel ? 1 : 0;
		}
		Lake.Area = Lake.NumWetCells * Fine * Fine;
		Lake.Depth = FMath::Max(Lake.WaterLevel - LowestWet, 0.0);
		if (Lake.Depth < MinDepth || Lake.Area < Settings.MinLakeArea)
		{
			return;
		}

		// The body's level: the water's height as a water level. A block world's lies inside the water's top layer.
		const double BodyLevel = (bBlocks ? Lake.WaterLevel - 0.1 * Voxel : Lake.WaterLevel) + Ground.GetLevelBase();
		const uint32 BodyId = FVoxelHydrology::MakeLakeId(Ground.Seed, Lake.LowestCell, Lake.Cells.Num());
		TSharedPtr<const FVoxelWaterLake> Shape;
		if (bRadial)
		{
			// A planet lake's grid is centred on its centre: as many cells each way as the footprint reaches furthest.
			const int32 Half = FMath::Max(FMath::Max(FMath::Abs(FootMin.X), FMath::Abs(FootMax.X)), FMath::Max(FMath::Abs(FootMin.Y), FMath::Abs(FootMax.Y)));
			const FIntPoint Size(2 * Half + 1, 2 * Half + 1);
			TBitArray<> Bits(false, Size.X * Size.Y);
			for (const FIntPoint& Cell : Footprint)
			{
				Bits[(Cell.Y + Half) * Size.X + (Cell.X + Half)] = true;
			}
			Shape = FVoxelWaterLake::MakeOnSphere(FineGrid.Centre, FineGrid.Radius, Fine, Size, MoveTemp(Bits));
		}
		else
		{
			const FIntPoint Size(FootMax.X - FootMin.X + 1, FootMax.Y - FootMin.Y + 1);
			TBitArray<> Bits(false, Size.X * Size.Y);
			for (const FIntPoint& Cell : Footprint)
			{
				Bits[(Cell.Y - FootMin.Y) * Size.X + (Cell.X - FootMin.X)] = true;
			}
			// A cell's square is centred on its place; a block world's cell is the column from its place on.
			const double Lead = bBlocks ? 0.0 : 0.5;
			const FVector2D Origin(FineGrid.Offset.X + (FootMin.X - Lead) * Fine, FineGrid.Offset.Y + (FootMin.Y - Lead) * Fine);
			Shape = FVoxelWaterLake::Make(Origin, Fine, Size, MoveTemp(Bits));
		}
		// The ground at the footprint's cells' corners and centres, for the drawn sheet to follow the shore and not the
		// cells' steps (G229). A block world's cells are its columns, whose squares are the shore already.
		if (!bBlocks)
		{
			const double LevelBase = Ground.GetLevelBase();
			Shape = FVoxelWaterLake::WithGround(*Shape, [&Ground, LevelBase](const FVector& Place) { return Ground.HeightAt(Place) + LevelBase; });
		}
		Lake.Body = FVoxelWaterBody::MakeLake(BodyId, BodyLevel, Shape.ToSharedRef(), Ground.GetFrame(), Lake.Depth + Ground.ChunkWorldSize, Settings.LakeMinCaveDepth);
		Lake.bKept = true;
	}
}

// ==================== A pit's lake ====================

/** One flood from a cell, as MakeLake describes; the lake's body is not made here. */
static TSharedRef<FVoxelHydrologyLake> FloodFromPit(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Pit)
{
	using namespace VoxelHydrologyPrivate;
	const FVoxelHydrologyGrid& Grid = Heights.GetGrid();
	const double DepthCap = Settings.MaxLakeDepth;
	const int32 MaxCells = Settings.MaxLakeCells;
	const int32 MaxSpan = Settings.MaxLakeSpanCells;

	TSharedRef<FVoxelHydrologyLake> Lake = MakeShared<FVoxelHydrologyLake>();
	Lake->Pit = Pit;

	TArray<FFloodEntry> Heap;
	TArray<FFloodEntry> Taken;          // in the order the flood took them
	TSet<FIntPoint> Seen;
	TMap<FIntPoint, FIntPoint> Pusher;  // the cell from which each cell was first reached
	Heap.HeapPush(FFloodEntry{ Heights.At(Pit), Pit });
	Seen.Add(Pit);

	double Level = Heights.At(Pit);     // the level the flood has climbed to
	int32 CountAtRise = 0;              // how many cells were taken when it last climbed: the hollow under that rim
	double LowestOfLake = Level;        // the lowest ground of the hollow so far (the cells that are surely in it)
	int32 BeyondStart = INDEX_NONE;     // the first cell taken beyond a saddle the flood has not climbed back over yet
	double LowestBeyond = 0.0;
	// How wide the flood has grown, in cells along the grid's two axes. A planet's cells have no common axes across its
	// faces: there the width is measured east and north of the pit on the plane tangent at it, in cells' widths.
	FIntPoint MinCell = Pit, MaxCell = Pit;
	FVector PitUp = FVector::UpVector, PitEast = FVector::ForwardVector, PitNorth = FVector::RightVector;
	FVector2D MinPlace = FVector2D::ZeroVector, MaxPlace = FVector2D::ZeroVector;
	if (Grid.IsRadial())
	{
		PitUp = Heights.PlaceOf(Pit);
		FVoxelCubeSphere::GetTangentFrame(PitUp, PitEast, PitNorth);
	}
	const double CellsAcross = Heights.GetGround().PlanetRadius / FMath::Max(Grid.GetCellSize(), 1.0);
	const auto Grow = [&](const FIntPoint& Cell)
	{
		if (Grid.IsRadial())
		{
			const FVector Place = Heights.PlaceOf(Cell);
			const FVector2D At(CellsAcross * FVector::DotProduct(Place, PitEast), CellsAcross * FVector::DotProduct(Place, PitNorth));
			MinPlace = FVector2D(FMath::Min(MinPlace.X, At.X), FMath::Min(MinPlace.Y, At.Y));
			MaxPlace = FVector2D(FMath::Max(MaxPlace.X, At.X), FMath::Max(MaxPlace.Y, At.Y));
			return;
		}
		MinCell = FIntPoint(FMath::Min(MinCell.X, Cell.X), FMath::Min(MinCell.Y, Cell.Y));
		MaxCell = FIntPoint(FMath::Max(MaxCell.X, Cell.X), FMath::Max(MaxCell.Y, Cell.Y));
	};
	const auto IsTooWide = [&]()
	{
		return Grid.IsRadial()
			? (MaxPlace.X - MinPlace.X >= MaxSpan || MaxPlace.Y - MinPlace.Y >= MaxSpan)
			: (MaxCell.X - MinCell.X >= MaxSpan || MaxCell.Y - MinCell.Y >= MaxSpan);
	};
	// The end: how many of Taken are the hollow, its rim, and why it is closed if it is.
	int32 NumHollow = 0;
	bool bDone = false;
	const int32 Budget = 3 * MaxCells + 64;

	while (Heap.Num() > 0)
	{
		FFloodEntry Entry;
		Heap.HeapPop(Entry, EAllowShrinking::No);
		if (Entry.Height > Level)
		{
			// The flood climbs: every cell taken so far lies under this cell, the hollow under this rim. Any ground
			// taken beyond a saddle was one more hollow of the same lake.
			if (BeyondStart != INDEX_NONE)
			{
				LowestOfLake = FMath::Min(LowestOfLake, LowestBeyond);
				BeyondStart = INDEX_NONE;
			}
			if (Taken.Num() > MaxCells || IsTooWide())
			{
				// Wider than a lake may be: the lake is the hollow as it stood under the rim before, with no outlet.
				Lake->Closed = 2;
				NumHollow = CountAtRise;
				Lake->RimLevel = Level;
				bDone = true;
				break;
			}
			if (Entry.Height - LowestOfLake > DepthCap)
			{
				// Deeper than a lake may be: it stands at the cap, with no outlet.
				Lake->Closed = 1;
				NumHollow = Taken.Num();
				Lake->RimLevel = LowestOfLake + DepthCap;
				bDone = true;
				break;
			}
			CountAtRise = Taken.Num();
			Level = Entry.Height;
		}
		else if (Entry.Height < Level)
		{
			// Past a saddle, on lower ground beyond it.
			if (BeyondStart == INDEX_NONE)
			{
				BeyondStart = Taken.Num();
				LowestBeyond = Entry.Height;
				Lake->Outlet = Entry.Cell;
			}
			LowestBeyond = FMath::Min(LowestBeyond, Entry.Height);
			// It runs away when it reaches the sea, falls further than any lake there could stand deep, or is wider than
			// any lake there could be: whatever lake that ground holds stands under this one's rim.
			if (Heights.IsSea(Entry.Cell) || Entry.Height < Level - DepthCap || Taken.Num() - BeyondStart >= MaxCells)
			{
				NumHollow = BeyondStart;
				Lake->RimLevel = Level;
				bDone = true;
				break;
			}
		}
		Taken.Add(Entry);
		Grow(Entry.Cell);
		if (Taken.Num() > Budget)
		{
			break;
		}
		for (int32 D = 0; D < 8; ++D)
		{
			FIntPoint Neighbour;
			if (!Grid.GetNeighbour(Entry.Cell, D, Neighbour))
			{
				continue;
			}
			bool bAlready = false;
			Seen.Add(Neighbour, &bAlready);
			if (!bAlready)
			{
				Pusher.Add(Neighbour, Entry.Cell);
				Heap.HeapPush(FFloodEntry{ Heights.At(Neighbour), Neighbour });
			}
		}
	}
	Lake->NumFlooded = Taken.Num();
	if (!bDone)
	{
		// No end within the budget (or a ground with an edge, in a test): the hollow as far as it is sure, closed.
		Lake->Closed = 3;
		NumHollow = BeyondStart != INDEX_NONE ? BeyondStart : Taken.Num();
		Lake->RimLevel = Level;
	}

	// An open lake's way out: the rim's cell the outlet was reached from, and the lake's lowest cell beside it.
	if (Lake->Closed == 0)
	{
		const FIntPoint* From = Pusher.Find(Lake->Outlet);
		Lake->Saddle = From ? *From : Pit;
	}
	Lake->Cells.Reserve(NumHollow);
	Lake->LowestCell = Pit;
	Lake->LowestGround = Heights.At(Pit);
	for (int32 Index = 0; Index < NumHollow; ++Index)
	{
		const FFloodEntry& Entry = Taken[Index];
		if (Lake->Closed == 0 && Entry.Cell == Lake->Saddle)
		{
			continue;
		}
		Lake->Cells.Add(Entry.Cell);
		if (Entry.Height < Lake->LowestGround || (Entry.Height == Lake->LowestGround && CellLess(Entry.Cell, Lake->LowestCell)))
		{
			Lake->LowestGround = Entry.Height;
			Lake->LowestCell = Entry.Cell;
		}
	}
	Lake->Cells.Add(Pit);
	if (Lake->Closed == 0)
	{
		bool bFound = false;
		double Lowest = 0.0;
		for (int32 D = 0; D < 8; ++D)
		{
			FIntPoint Neighbour;
			if (!Grid.GetNeighbour(Lake->Saddle, D, Neighbour))
			{
				continue;
			}
			if (Lake->Cells.Contains(Neighbour) && (!bFound || Heights.At(Neighbour) < Lowest))
			{
				bFound = true;
				Lowest = Heights.At(Neighbour);
				Lake->PreSaddle = Neighbour;
			}
		}
		if (!bFound)
		{
			Lake->Closed = 3;
		}
		// The way on from the saddle. A descent that comes to the saddle's cell from elsewhere goes on by the steepest
		// fall; where that leads out of the hollow it is the lake's way out too, so the way on from the cell is one,
		// whoever comes to it. (Where it leads into the hollow, a descent comes back to the saddle and leaves by the
		// cell the flood found.)
		FIntPoint Down;
		if (Lake->Closed == 0 && Heights.GetDownhill(Lake->Saddle, Down) && !Lake->Cells.Contains(Down))
		{
			Lake->Outlet = Down;
		}
	}
	if (Lake->Closed == 0)
	{
		// From every cell of the hollow the way to the saddle: breadth first from the cell before it.
		Lake->ToSaddle.Reserve(Lake->Cells.Num());
		TArray<FIntPoint> Queue;
		Queue.Reserve(Lake->Cells.Num());
		Queue.Add(Lake->PreSaddle);
		Lake->ToSaddle.Add(Lake->PreSaddle, Lake->PreSaddle);
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			const FIntPoint Cell = Queue[Head];
			for (int32 D = 0; D < 8; ++D)
			{
				FIntPoint Neighbour;
				if (!Grid.GetNeighbour(Cell, D, Neighbour))
				{
					continue;
				}
				if (Lake->Cells.Contains(Neighbour) && !Lake->ToSaddle.Contains(Neighbour))
				{
					Lake->ToSaddle.Add(Neighbour, Cell);
					Queue.Add(Neighbour);
				}
			}
		}
	}

	return Lake;
}

TSharedRef<const FVoxelHydrologyLake> FVoxelHydrology::MakeLake(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Pit)
{
	// A closed lake is the one the flood from its own lowest cell finds (G233). A hollow wider than a lake may be is
	// closed where the flood found it too wide, and that depends on the pit it started from: two pits of one wide hollow
	// gave two lakes with their own levels, which overlapped. The flood from the lowest cell of what the first flood found
	// finds a lake whose lowest cell is that cell (or a lower one, and the flood goes again from there), so every pit whose
	// flood comes to the same lowest cell gets the same lake. An open lake is one from any of its pits already: it is
	// left as its flood found it.
	constexpr int32 MaxFloods = 4;
	TSharedRef<FVoxelHydrologyLake> Lake = FloodFromPit(Settings, Heights, Pit);
	for (int32 Flood = 1; Flood < MaxFloods && !Lake->IsOpen() && Lake->LowestCell != Lake->Pit; ++Flood)
	{
		Lake = FloodFromPit(Settings, Heights, Lake->LowestCell);
	}
	// Kept by the pit asked for: the cache finds it from there.
	Lake->Pit = Pit;
	VoxelHydrologyPrivate::MakeLakeBody(Settings, Heights, Lake.Get());
	return Lake;
}

// ==================== A spring's descent ====================

TSharedRef<const FVoxelHydrologyTrace> FVoxelHydrology::TraceSpring(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Square,
	IVoxelHydrologyCache* Cache)
{
	using namespace VoxelHydrologyPrivate;
	const FVoxelHydrologyGrid& Grid = Heights.GetGrid();
	TSharedRef<FVoxelHydrologyTrace> Trace = MakeShared<FVoxelHydrologyTrace>();
	Trace->Square = Square;
	if (!GetSpring(Settings, Heights, Square, Trace->Spring))
	{
		return Trace;
	}

	TArray<FIntPoint>& Cells = Trace->Cells;
	TArray<int32>& LakeOf = Trace->LakeOf;
	const auto Push = [&Cells, &LakeOf](const FIntPoint& Cell, int32 Lake)
	{
		Cells.Add(Cell);
		LakeOf.Add(Lake);
	};
	// The valley rivers a descent from this spring may reach (the valleys plan, D179): it ends in the first one whose
	// water it enters, a river like one that ends in the sea
	TArray<TSharedRef<const FVoxelHydrologySystem>> ValleyRivers;
	const FVoxelHydrologyGround& TraceGround = Heights.GetGround();
	if (TraceGround.Valleys.IsValid())
	{
		const double Reach = Settings.MaxRiverLength + Heights.GetCellSize();
		const FVector SpringAt = Heights.PlaceOf(Trace->Spring);
		const FVoxelConditioningRegion Near = TraceGround.bRadial
			? FVoxelConditioningRegion::OfCap(SpringAt, Reach / FMath::Max(TraceGround.PlanetRadius, 1.0))
			: FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(SpringAt) - FVector2D(Reach), FVector2D(SpringAt) + FVector2D(Reach)));
		GatherValleyRivers(Settings, TraceGround, Near, Cache, ValleyRivers);
	}
	const auto InValleyRiver = [&ValleyRivers, &Heights, &TraceGround](const FIntPoint& At) -> bool
	{
		const FVector Place = Heights.PlaceOf(At);
		for (const TSharedRef<const FVoxelHydrologySystem>& River : ValleyRivers)
		{
			for (const FVoxelHydrologyReach& Reach : River->Reaches)
			{
				FVoxelWaterReachSample Sample;
				if (!Reach.Body.Reach.IsValid())
				{
					continue;
				}
				if (TraceGround.bRadial ? Reach.Body.Reach->SampleOnSphere(Place, TraceGround.PlanetRadius, Sample)
					: (Reach.Body.Bounds.IsInsideOrOnXY(Place) && Reach.Body.Reach->SampleAt(Place.X, Place.Y, Sample)))
				{
					return true;
				}
			}
		}
		return false;
	};

	FIntPoint Cell = Trace->Spring;
	int32 DescentStart = 0;   // where the descent in hand began: the spring, or the cell past the last lake's saddle
	double Length = 0.0;
	bool bEndsUnderLake = false;
	Trace->End = EVoxelHydrologyEnd::TooLong;
	for (;;)
	{
		if (Heights.IsSea(Cell))
		{
			Push(Cell, INDEX_NONE);
			Trace->End = EVoxelHydrologyEnd::Sea;
			Trace->Mouth = Cell;
			break;
		}
		if (ValleyRivers.Num() > 0 && InValleyRiver(Cell))
		{
			Push(Cell, INDEX_NONE);
			Trace->End = EVoxelHydrologyEnd::ValleyRiver;
			Trace->Mouth = Cell;
			break;
		}
		if (Length > Settings.MaxRiverLength)
		{
			Push(Cell, INDEX_NONE);
			Trace->End = EVoxelHydrologyEnd::TooLong;
			break;
		}
		FIntPoint Next;
		if (Heights.GetDownhill(Cell, Next))
		{
			Push(Cell, INDEX_NONE);
			Length += Grid.GetStepLength(Cell, Next);
			Cell = Next;
			continue;
		}

		// A pit: its lake. The descent entered the hollow at the first of its cells that lies in it.
		Push(Cell, INDEX_NONE);
		TSharedPtr<const FVoxelHydrologyLake> Lake = Cache ? Cache->FindLake(Cell) : nullptr;
		if (!Lake.IsValid())
		{
			const TSharedRef<const FVoxelHydrologyLake> Made = MakeLake(Settings, Heights, Cell);
			Lake = Cache ? Cache->StoreLake(Made).ToSharedPtr() : Made.ToSharedPtr();
		}
		const int32 LakeIndex = Trace->Lakes.Add(Lake);
		int32 Entry = Cells.Num() - 1;
		while (Entry > DescentStart && Lake->Cells.Contains(Cells[Entry - 1]))
		{
			--Entry;
		}
		// The cell before the descent in hand is the saddle of the lake it left: Entry may be the descent's first cell
		// (the last lake's outlet) and still come over this hollow's saddle (G231).
		if (Lake->IsOpen() && Entry > 0 && Entry >= DescentStart && Cells[Entry - 1] == Lake->Saddle)
		{
			// The descent came to the hollow over its own saddle (the steepest way down from a saddle may lead in, while
			// the lowest ground beside it lies out): water that stands on the saddle leaves by the outlet, as the lake's
			// own water does, so the way on from the saddle is the same whichever descent comes to it. So does a descent
			// that left another lake over the same saddle cell, two hollows meeting in one saddle at one level, the
			// first's outlet the second's cell: from the saddle it goes on by the second's outlet, not through it (G231).
			for (int32 Index = Entry - 1; Index + 1 < Cells.Num(); ++Index)
			{
				Length -= Grid.GetStepLength(Cells[Index], Cells[Index + 1]);
			}
			Cells.SetNum(Entry, EAllowShrinking::No);
			LakeOf.SetNum(Entry, EAllowShrinking::No);
			if (Trace->Lakes.Num() > Settings.MaxLakesPerRiver)
			{
				Trace->End = EVoxelHydrologyEnd::TooManyLakes;
				break;
			}
			Length += Grid.GetStepLength(Lake->Saddle, Lake->Outlet);
			Cell = Lake->Outlet;
			DescentStart = Cells.Num();
			continue;
		}
		if (!Lake->IsOpen())
		{
			// No way out: the river ends where it enters. The cells on toward the pit are kept as far as the second
			// that lies under the lake's water, for the reach's last vertices; a descent that comes to none (the
			// water stands elsewhere in the hollow) does not end in the lake, and is no river.
			int32 LastKept = Entry;
			for (int32 Index = Entry, Wet = 0; Index < Cells.Num() && Wet < 2; ++Index)
			{
				if (IsUnderLake(*Lake, Heights, Cells[Index]))
				{
					LastKept = Index;
					bEndsUnderLake = true;
					++Wet;
				}
			}
			for (int32 Index = Entry; Index + 1 < Cells.Num(); ++Index)
			{
				Length -= Grid.GetStepLength(Cells[Index], Cells[Index + 1]);
			}
			Cells.SetNum(LastKept + 1, EAllowShrinking::No);
			LakeOf.SetNum(LastKept + 1, EAllowShrinking::No);
			for (int32 Index = Entry; Index < Cells.Num(); ++Index)
			{
				LakeOf[Index] = LakeIndex;
			}
			Trace->End = EVoxelHydrologyEnd::ClosedLake;
			Trace->Mouth = Cells[Entry];
			break;
		}
		if (Trace->Lakes.Num() > Settings.MaxLakesPerRiver)
		{
			Trace->End = EVoxelHydrologyEnd::TooManyLakes;
			break;
		}
		// Through the hollow: from the entry the way on is the hollow's own, to its saddle and the cell beyond. A
		// cell's way on is then the cell's alone, whichever descent comes to it.
		for (int32 Index = Entry; Index + 1 < Cells.Num(); ++Index)
		{
			Length -= Grid.GetStepLength(Cells[Index], Cells[Index + 1]);
		}
		Cells.SetNum(Entry + 1, EAllowShrinking::No);
		LakeOf.SetNum(Entry + 1, EAllowShrinking::No);
		LakeOf[Entry] = LakeIndex;
		FIntPoint At = Cells[Entry];
		for (int32 Guard = Lake->Cells.Num() + 1; At != Lake->PreSaddle && Guard > 0; --Guard)
		{
			const FIntPoint* Way = Lake->ToSaddle.Find(At);
			if (!Way)
			{
				break;
			}
			Length += Grid.GetStepLength(At, *Way);
			At = *Way;
			Push(At, LakeIndex);
		}
		Length += Grid.GetStepLength(At, Lake->Saddle);
		Push(Lake->Saddle, INDEX_NONE);
		Length += Grid.GetStepLength(Lake->Saddle, Lake->Outlet);
		Cell = Lake->Outlet;
		DescentStart = Cells.Num();
	}

	Trace->Length = Length;
	Trace->Fall = Cells.Num() > 0 ? Heights.At(Trace->Spring) - Heights.At(Cells.Last()) : 0.0;
	const bool bEndsInWater = Trace->End == EVoxelHydrologyEnd::Sea || Trace->End == EVoxelHydrologyEnd::ValleyRiver
		|| (Trace->End == EVoxelHydrologyEnd::ClosedLake && bEndsUnderLake);
	Trace->bKept = bEndsInWater && Length >= Settings.MinRiverLength && Length <= Settings.MaxRiverLength && Length > 0.0
		&& Trace->Fall / Length <= Settings.MaxMeanGrade;
	return Trace;
}

// ==================== A river system ====================

namespace VoxelHydrologyPrivate
{
	/** One chain of a system's tree, as it is worked on. */
	struct FChain
	{
		TArray<FIntPoint> Cells;
		int32 Flow = 1;
		bool bEndsAtJoin = false;
		/** The chains that end at this chain's first cell */
		TArray<int32> Incoming;
		/** The chain that starts at this chain's last cell, when it ends at a join */
		int32 Downstream = INDEX_NONE;

		// The shaped line and what was read along it
		TArray<FVector> Points;
		TArray<double> Ground;
		TArray<double> BodyLevels;
		/** A planet: the unit direction at each point (empty on a heightmap world) */
		TArray<FVector> Ups;
		bool bRounded = true;
		double HalfWidth = 0.0;

		// The first profile: the surface it arrives with at its end. NaN until made
		double Arrival = std::numeric_limits<double>::quiet_NaN();
		/** The first profile, kept: it is the chain's own when the chain does not end at a join */
		FVoxelConditioningLineShape::FRiverProfile FirstProfile;
		/** The level this chain starts at when other chains end at its first cell: the lowest they arrive with */
		double StartLevel = std::numeric_limits<double>::quiet_NaN();
	};
}

namespace VoxelHydrologyPrivate
{
	/**
	 * A chain's cells as a line: sampled evenly, its corners spread along it until no vertex turns by more than the
	 * bound, its two ends where they were. FVoxelConditioningLineShape::RoundCorners narrows its window toward the
	 * ends, so a corner a cell from an end (a chain of a few cells, the cell before a join) stays sharp; here the line
	 * is carried on past each end by its own mirror image through the end, so a corner near an end is spread across it
	 * and the end stays in place.
	 * @param Raw The cells' places, where distances are measured (a planet: their points on the datum sphere)
	 * @param Spacing The vertices' spacing
	 * @param MaxTurn The bound on a vertex's turn
	 * @param Distance The distance the first try spreads over
	 * @param SphereRadius A planet: the datum radius, to whose sphere every sampled and spread point is brought back (a
	 *                     mean of points on a sphere lies under it); 0 on a heightmap world
	 * @param Out Receives the vertices
	 * @return Whether the line is within the bound (with the shape's margin)
	 */
	bool RoundChain(TConstArrayView<FVector> Raw, double Spacing, double MaxTurn, double Distance, double SphereRadius, TArray<FVector>& Out)
	{
		using FShape = FVoxelConditioningLineShape;
		const auto OntoSphere = [SphereRadius](TArray<FVector>& Points)
		{
			if (SphereRadius > 0.0)
			{
				for (FVector& Point : Points)
				{
					Point = Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) * SphereRadius;
				}
			}
		};
		TArray<FVector> Dense;
		FShape::ResampleEvenly(Raw, Spacing, Dense);
		OntoSphere(Dense);
		Out = Dense;
		const int32 Num = Dense.Num();
		if (Num < 3 || FShape::GetWorstTurn(Out) <= FShape::BoundMargin * MaxTurn)
		{
			return Num >= 2;
		}
		TArray<FVector> Padded;
		TArray<FVector> Spread;
		TArray<FVector> Cut;
		for (int32 Try = 0; Try < 5; ++Try, Distance *= 1.5)
		{
			// Two passes of the average each reach a window's width: the mirror image is as long.
			const int32 Pad = FMath::Min(Num - 1, 2 * FMath::RoundToInt32(Distance / FMath::Max(Spacing, 1.0)) + 1);
			Padded.Reset(Num + 2 * Pad);
			for (int32 Index = Pad; Index >= 1; --Index)
			{
				Padded.Add(2.0 * Dense[0] - Dense[Index]);
			}
			Padded.Append(Dense);
			for (int32 Index = 1; Index <= Pad; ++Index)
			{
				Padded.Add(2.0 * Dense[Num - 1] - Dense[Num - 1 - Index]);
			}
			FShape::SpreadCorners(Padded, Spacing, Distance, Spread);
			Cut.Reset(Num);
			for (int32 Index = 0; Index < Num; ++Index)
			{
				Cut.Add(Spread[Pad + Index]);
			}
			Cut[0] = Dense[0];
			Cut[Num - 1] = Dense[Num - 1];
			OntoSphere(Cut);
			FShape::ResampleEvenly(Cut, Spacing, Out);
			OntoSphere(Out);
			if (FShape::GetWorstTurn(Out) <= FShape::BoundMargin * MaxTurn)
			{
				return true;
			}
		}
		return false;
	}
}

TSharedRef<const FVoxelHydrologySystem> FVoxelHydrology::BuildSystem(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Mouth,
	TConstArrayView<TSharedRef<const FVoxelHydrologyTrace>> Rivers)
{
	using namespace VoxelHydrologyPrivate;
	using FShape = FVoxelConditioningLineShape;
	const FVoxelHydrologyGround& Ground = Heights.GetGround();
	const FVoxelHydrologyGrid& Grid = Heights.GetGrid();
	const bool bRadial = Ground.bRadial;
	const double CellSize = Heights.GetCellSize();
	const double Voxel = FMath::Max(Ground.VoxelSize, 1.0);
	const double Spacing = 2.0 * Voxel;
	const double BankWidth = Settings.BankWidthVoxels * Voxel;
	const double BankMargin = Settings.BankMarginVoxels * Voxel;
	// A water level from a line's height, and a place from a point where distances are measured.
	const double LevelBase = Ground.GetLevelBase();
	const FVoxelWaterFrame Frame = Ground.GetFrame();
	const auto PlaceOfPoint = [bRadial](const FVector& Point) { return bRadial ? Point.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector) : Point; };

	TSharedRef<FVoxelHydrologySystem> System = MakeShared<FVoxelHydrologySystem>();
	System->Mouth = Mouth;
	System->NumSprings = Rivers.Num();

	// The tree: every river's cells outside the kept lakes, each cell's way on, and how many springs drain through it.
	TMap<FIntPoint, FIntPoint> Next;
	TMap<FIntPoint, int32> Flow;
	TMap<FIntPoint, TArray<FIntPoint, TInlineAllocator<2>>> Before;
	TSet<FIntPoint> Starts;
	TMap<uint32, FVoxelWaterBody> LakeBodies;
	for (const TSharedRef<const FVoxelHydrologyTrace>& River : Rivers)
	{
		const FVoxelHydrologyTrace& Trace = River.Get();
		const int32 Num = Trace.Cells.Num();
		System->LongestRiver = FMath::Max(System->LongestRiver, Trace.Length);
		const auto KeptLake = [&Trace](int32 Index) -> const FVoxelHydrologyLake*
		{
			const int32 Lake = Trace.LakeOf[Index];
			return Lake != INDEX_NONE && Trace.Lakes[Lake]->bKept ? Trace.Lakes[Lake].Get() : nullptr;
		};
		const auto IsWet = [&](int32 Index)
		{
			const FVoxelHydrologyLake* Lake = KeptLake(Index);
			return Lake && IsUnderLake(*Lake, Heights, Trace.Cells[Index]);
		};
		for (const TSharedPtr<const FVoxelHydrologyLake>& Lake : Trace.Lakes)
		{
			if (Lake->bKept)
			{
				LakeBodies.Add(Lake->Body.Id, Lake->Body);
			}
		}

		// The pieces of the river between the kept lakes. A piece runs on into the lake it meets until two of its cells
		// lie under the water, and the piece after a lake starts a cell before the last of its cells under the water:
		// the reaches' ends lie in the lake.
		TArray<FIntPoint, TInlineAllocator<8>> Pieces;   // (first, last) indices
		int32 PieceStart = 0;
		for (int32 Index = 0; Index < Num;)
		{
			const FVoxelHydrologyLake* Lake = KeptLake(Index);
			if (!Lake)
			{
				++Index;
				continue;
			}
			int32 SpanEnd = Index;
			while (SpanEnd + 1 < Num && KeptLake(SpanEnd + 1) == Lake)
			{
				++SpanEnd;
			}
			int32 InEnd = SpanEnd;
			for (int32 At = Index, Wet = 0; At <= SpanEnd; ++At)
			{
				Wet += IsWet(At) ? 1 : 0;
				if (Wet >= 2)
				{
					InEnd = At;
					break;
				}
			}
			int32 OutStart = SpanEnd;
			for (int32 At = SpanEnd; At >= Index; --At)
			{
				if (IsWet(At))
				{
					OutStart = (At > Index && IsWet(At - 1)) ? At - 1 : At;
					break;
				}
			}
			const bool bGoesOn = SpanEnd + 1 < Num;
			if (!bGoesOn)
			{
				Pieces.Add(FIntPoint(PieceStart, InEnd));
				PieceStart = Num;
			}
			else if (InEnd + 1 < OutStart)
			{
				Pieces.Add(FIntPoint(PieceStart, InEnd));
				PieceStart = OutStart;
			}
			// else: a lake so small on this river's way that its two ends meet: the river runs through it.
			Index = SpanEnd + 1;
		}
		if (PieceStart < Num)
		{
			Pieces.Add(FIntPoint(PieceStart, Num - 1));
		}

		Starts.Add(Trace.Spring);
		bool bJoinedKnownWay = false;
		for (const FIntPoint& Piece : Pieces)
		{
			if (bJoinedKnownWay)
			{
				break;
			}
			// A piece wholly under a lake's water (a spring that stands in one) is no river.
			bool bDry = false;
			for (int32 Index = Piece.X; Index <= Piece.Y && !bDry; ++Index)
			{
				bDry = !IsWet(Index);
			}
			if (!bDry || Piece.Y <= Piece.X)
			{
				continue;
			}
			Starts.Add(Trace.Cells[Piece.X]);
			for (int32 Index = Piece.X; Index <= Piece.Y; ++Index)
			{
				const FIntPoint& Cell = Trace.Cells[Index];
				++Flow.FindOrAdd(Cell);
				if (Index == Piece.Y)
				{
					break;
				}
				const FIntPoint& Following = Trace.Cells[Index + 1];
				if (const FIntPoint* Known = Next.Find(Cell))
				{
					if (*Known != Following)
					{
						++System->NumConflicts;
						if (System->ConflictNote.IsEmpty())
						{
							const int32 LakeHere = Trace.LakeOf[Index];
							const int32 LakeNext = Trace.LakeOf[Index + 1];
							System->ConflictNote = FString::Printf(
								TEXT("from cell (%d, %d) one descent goes on to (%d, %d) and the descent of square (%d, %d) to (%d, %d) (its cell %d of %d; lake here %d%s, lake next %d; ground %.2f, %.2f, %.2f)"),
								Cell.X, Cell.Y, Known->X, Known->Y, Trace.Square.X, Trace.Square.Y, Following.X, Following.Y, Index, Num, LakeHere,
								LakeHere != INDEX_NONE ? *FString::Printf(TEXT(" (pit (%d, %d), rim %.2f, closed %d, %d cells, saddle (%d, %d), outlet (%d, %d))"),
									Trace.Lakes[LakeHere]->Pit.X, Trace.Lakes[LakeHere]->Pit.Y, Trace.Lakes[LakeHere]->RimLevel, Trace.Lakes[LakeHere]->Closed,
									Trace.Lakes[LakeHere]->Cells.Num(), Trace.Lakes[LakeHere]->Saddle.X, Trace.Lakes[LakeHere]->Saddle.Y,
									Trace.Lakes[LakeHere]->Outlet.X, Trace.Lakes[LakeHere]->Outlet.Y) : TEXT(""),
								LakeNext, Heights.At(Cell), Heights.At(*Known), Heights.At(Following));
						}
						// The way on from a cell is one: this river goes on by the way an earlier river (in the lattice's
						// order, so the same whichever region asks) gave the cell, and its spring drains down that way.
						FIntPoint On = *Known;
						for (int32 Guard = Next.Num() + 1; Guard > 0; --Guard)
						{
							++Flow.FindOrAdd(On);
							const FIntPoint* Way = Next.Find(On);
							if (!Way)
							{
								break;
							}
							On = *Way;
						}
						bJoinedKnownWay = true;
						break;
					}
					continue;
				}
				Next.Add(Cell, Following);
				Before.FindOrAdd(Following).AddUnique(Cell);
			}
		}
	}
	for (const TPair<FIntPoint, TArray<FIntPoint, TInlineAllocator<2>>>& Entry : Before)
	{
		if (Entry.Value.Num() >= 2)
		{
			Starts.Add(Entry.Key);
		}
	}

	// The chains, by their first cell.
	TArray<FIntPoint> StartCells = Starts.Array();
	StartCells.Sort([](const FIntPoint& A, const FIntPoint& B) { return CellLess(A, B); });
	TArray<FChain> Chains;
	TMap<FIntPoint, int32> ChainAt;
	for (const FIntPoint& Start : StartCells)
	{
		if (!Next.Contains(Start))
		{
			continue;
		}
		FChain& Chain = Chains.AddDefaulted_GetRef();
		Chain.Flow = FMath::Max(Flow.FindRef(Start), 1);
		Chain.Cells.Add(Start);
		FIntPoint At = Start;
		for (int32 Guard = Next.Num() + 1; Guard > 0; --Guard)
		{
			const FIntPoint* Way = Next.Find(At);
			if (!Way)
			{
				break;
			}
			At = *Way;
			Chain.Cells.Add(At);
			if (Starts.Contains(At) && Next.Contains(At))
			{
				Chain.bEndsAtJoin = true;
				break;
			}
		}
		ChainAt.Add(Start, Chains.Num() - 1);
	}
	for (int32 Index = 0; Index < Chains.Num(); ++Index)
	{
		if (Chains[Index].bEndsAtJoin)
		{
			if (const int32* Downstream = ChainAt.Find(Chains[Index].Cells.Last()))
			{
				Chains[Index].Downstream = *Downstream;
				Chains[*Downstream].Incoming.Add(Index);
			}
			else
			{
				Chains[Index].bEndsAtJoin = false;
			}
		}
	}
	if (Chains.Num() == 0)
	{
		System->Refused = TEXT("no river reach outside its lakes");
		return System;
	}

	// The water the reaches meet: the sea and the kept lakes on the rivers' ways, and the valley river a system ends in.
	TArray<FVoxelWaterBody> Bodies;
	LakeBodies.GenerateValueArray(Bodies);
	if (Ground.Valleys.IsValid() && Rivers.ContainsByPredicate([](const TSharedRef<const FVoxelHydrologyTrace>& River) { return River->End == EVoxelHydrologyEnd::ValleyRiver; }))
	{
		const FVector MouthAt = Heights.PlaceOf(Mouth);
		const double Near = 4.0 * Heights.GetCellSize();
		const FVoxelConditioningRegion Region = bRadial
			? FVoxelConditioningRegion::OfCap(MouthAt, Near / FMath::Max(Ground.PlanetRadius, 1.0))
			: FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(MouthAt) - FVector2D(Near), FVector2D(MouthAt) + FVector2D(Near)));
		TArray<TSharedRef<const FVoxelHydrologySystem>> ValleyRivers;
		GatherValleyRivers(Settings, Ground, Region, nullptr, ValleyRivers);
		for (const TSharedRef<const FVoxelHydrologySystem>& River : ValleyRivers)
		{
			for (const FVoxelHydrologyReach& Reach : River->Reaches)
			{
				Bodies.Add(Reach.Body);
			}
		}
	}
	const FVoxelWaterLevels Water = FVoxelWaterLevels::Make(Ground.GetSea(), Frame, FVoxelWaterLevels::WholeWorld(), MoveTemp(Bodies));
	const auto GroundAt = [&Ground](const FVector& Place) { return Ground.HeightAt(Place); };
	const auto BodyLevelAt = [&Water](const FVector& Place, double GroundHeight) { return FVoxelRiverCourse::GetBodyLevelAt(Water, Place, GroundHeight); };

	FShape::FRiverProfileParams Params;
	Params.Depth = Settings.DepthVoxels * Voxel;
	Params.SurfaceUnderGround = Settings.SurfaceBelowGroundVoxels * Voxel;
	Params.BankMargin = BankMargin;
	Params.SmoothingDistance = Settings.SmoothingDistance;
	Params.HeadLength = Settings.HeadLength;
	Params.EndLength = Settings.HeadLength;
	Params.FalloffWidth = static_cast<float>(BankWidth);
	Params.VoxelSize = Voxel;
	if (Ground.bSea)
	{
		Params.MinSurface = Ground.SeaLevel - Params.BodyTuck;
	}
	if (Ground.bBlocks)
	{
		// A block world's bed steps a block a column at most, as a road's line does (D151).
		Params.MaxBedGrade = GetBlockBedGrade();
	}

	// The engine's own check of a bed's line: the bend bound above all (on a planet a segment's sag too, on a block
	// world a step of more than a block a column). The cut's cap is the profile's (MaxCutVoxels).
	FVoxelConditioningLineLimits Limits;
	Limits.VoxelSize = static_cast<float>(Voxel);
	Limits.bBlockWorld = Ground.bBlocks;
	Limits.PlanetRadius = bRadial ? Ground.PlanetRadius : 0.0;
	const double NoLevel = std::numeric_limits<double>::quiet_NaN();
	const double MaxCut = Settings.MaxCutVoxels * Voxel;

	// A river both steep and tightly curved breaks the bend bound however its bed is deepened: the system is then made
	// again with its corners spread wider, and refused when three widths do not do.
	constexpr int32 NumAttempts = 3;
	const double CornerCells[NumAttempts] = { 1.0, 2.0, 4.0 };
	const double BendDeepening[NumAttempts] = { 1.0, 1.5, 2.0 };
	for (int32 Attempt = 0; Attempt < NumAttempts; ++Attempt)
	{
	System->Reaches.Reset();
	System->Refused.Reset();
	System->DeepestCut = 0.0;
	FString BendFault;

	// Each chain's line: the cells' places with their corners rounded, a vertex every two voxels, and the ground and
	// the bodies read along it.
	for (FChain& Chain : Chains)
	{
		Chain.Points.Reset();
		Chain.Ground.Reset();
		Chain.BodyLevels.Reset();
		Chain.Arrival = NoLevel;
		Chain.StartLevel = NoLevel;
		Chain.HalfWidth = Voxel * FMath::Min(static_cast<double>(Settings.MaxHalfWidthVoxels),
			Settings.HalfWidthVoxels * (1.0 + Settings.HalfWidthGrowth * FMath::Log2(static_cast<double>(Chain.Flow))));
		TArray<FVector> Raw;
		Raw.Reserve(Chain.Cells.Num());
		for (const FIntPoint& Cell : Chain.Cells)
		{
			Raw.Add(Grid.PointOf(Heights.PlaceOf(Cell)));
		}
		const double MaxTurn = FShape::GetMaxTurnForReach(Spacing, Chain.HalfWidth + BankWidth);
		Chain.bRounded = RoundChain(Raw, Spacing, MaxTurn, CellSize * CornerCells[Attempt], bRadial ? Ground.PlanetRadius : 0.0, Chain.Points);
		Chain.Ground.Reserve(Chain.Points.Num());
		Chain.BodyLevels.Reserve(Chain.Points.Num());
		Chain.Ups.Reset();
		for (const FVector& Point : Chain.Points)
		{
			const FVector Place = PlaceOfPoint(Point);
			const double Height = GroundAt(Place);
			Chain.Ground.Add(Height);
			Chain.BodyLevels.Add(BodyLevelAt(Place, Height));
			if (bRadial)
			{
				Chain.Ups.Add(Place);
			}
		}
	}

	// A chain's profile. A chain other chains end at starts at the lowest surface they arrive with (StartLevel); a chain
	// that ends at a join lies, over its last bank's width, in the reach it joins (EndLevel).
	const auto MakeProfile = [&](const FChain& Chain, double EndLevel)
	{
		const int32 Num = Chain.Points.Num();
		TArray<double> BodyLevels = Chain.BodyLevels;
		if (!FMath::IsNaN(Chain.StartLevel) && Num > 0)
		{
			// The profile holds a vertex in a body a tuck under that body's level: the level given is the tuck over the
			// surface the chain starts at.
			BodyLevels[0] = Chain.StartLevel + Params.BodyTuck;
		}
		if (!FMath::IsNaN(EndLevel))
		{
			double Along = 0.0;
			for (int32 Index = Num - 1; Index >= 1; --Index)
			{
				BodyLevels[Index] = FMath::IsNaN(BodyLevels[Index]) ? EndLevel : FMath::Max(BodyLevels[Index], EndLevel);
				Along += FVector::Dist(Chain.Points[Index], Chain.Points[Index - 1]);
				if (Along > BankWidth)
				{
					break;
				}
			}
		}
		FShape::FRiverProfileParams ChainParams = Params;
		ChainParams.HalfWidth = static_cast<float>(Chain.HalfWidth);
		ChainParams.MaxBendDeepening = BendDeepening[Attempt];
		if (Num > 0 && !FMath::IsNaN(BodyLevels[Num - 1]))
		{
			// The reach comes to the water it ends in at that water's level, as it comes to the sea's: ground a little
			// over a lake's level would else put the surface under the lake's just before it, and the lake's water
			// would stand over the river's where the footprints meet.
			ChainParams.MinSurface = FMath::Max(ChainParams.MinSurface, BodyLevels[Num - 1] - ChainParams.BodyTuck);
		}
		// The banks where the river will have them: the floor narrows at a head, which the taper of a first profile over
		// level banks gives.
		const TArray<double> Taper = FShape::MakeRiverProfile(Chain.Points, Chain.Ground, Chain.Ground, TConstArrayView<double>(), BodyLevels, ChainParams).Taper;
		TArray<double> Floors;
		Floors.Reserve(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Floors.Add(Chain.HalfWidth * (Taper.IsValidIndex(Index) ? Taper[Index] : 1.0));
		}
		TArray<double> BankGround;
		FVoxelRiverCourse::ReadBanks(Chain.Points, Chain.Ups, Floors, BankWidth, Voxel, BankMargin, GroundAt, BodyLevelAt, BankGround);
		return FShape::MakeRiverProfile(Chain.Points, Chain.Ground, BankGround, TConstArrayView<double>(), BodyLevels, ChainParams);
	};

	// First the surfaces the chains arrive with, from the springs down: a chain's start is the lowest of the chains
	// that end at it, so the surface never rises from one reach to the next.
	TArray<int32> Order;
	{
		TArray<int32> Pending;
		TArray<uint8> State;
		State.Init(0, Chains.Num());
		for (int32 Root = 0; Root < Chains.Num(); ++Root)
		{
			if (State[Root] != 0)
			{
				continue;
			}
			Pending.Add(Root);
			while (Pending.Num() > 0)
			{
				const int32 Index = Pending.Last();
				if (State[Index] == 0)
				{
					State[Index] = 1;
					for (const int32 Incoming : Chains[Index].Incoming)
					{
						if (State[Incoming] == 0)
						{
							Pending.Add(Incoming);
						}
					}
				}
				else
				{
					Pending.Pop(EAllowShrinking::No);
					if (State[Index] == 1)
					{
						State[Index] = 2;
						Order.Add(Index);
					}
				}
			}
		}
	}
	for (const int32 Index : Order)
	{
		FChain& Chain = Chains[Index];
		for (const int32 Incoming : Chain.Incoming)
		{
			const double Arrival = Chains[Incoming].Arrival;
			if (!FMath::IsNaN(Arrival))
			{
				Chain.StartLevel = FMath::IsNaN(Chain.StartLevel) ? Arrival : FMath::Min(Chain.StartLevel, Arrival);
			}
		}
		if (Chain.Points.Num() >= 2)
		{
			Chain.FirstProfile = MakeProfile(Chain, NoLevel);
			if (Chain.FirstProfile.Surface.Num() > 0)
			{
				Chain.Arrival = Chain.FirstProfile.Surface.Last();
			}
		}
	}

	// Then each chain's river reach and bed.
	for (const int32 Index : Order)
	{
		const FChain& Chain = Chains[Index];
		if (Chain.Points.Num() < 2)
		{
			System->Refused = TEXT("a river reach of one place");
			break;
		}
		if (!Chain.bRounded)
		{
			BendFault = FString::Printf(TEXT("the river reach from cell (%d, %d) bends more sharply than a bed's line may"), Chain.Cells[0].X, Chain.Cells[0].Y);
			break;
		}
		const double EndLevel = Chain.Downstream != INDEX_NONE ? Chains[Chain.Downstream].StartLevel : NoLevel;
		FVoxelHydrologyReach Reach;
		Reach.StartCell = Chain.Cells[0];
		Reach.Flow = Chain.Flow;
		Reach.bEndsAtJoin = Chain.Downstream != INDEX_NONE;
		// A chain that ends in the sea or a lake was given its whole profile above; one that ends at a join is made
		// again with its last vertices in the reach it joins.
		Reach.Profile = FMath::IsNaN(EndLevel) ? Chain.FirstProfile : MakeProfile(Chain, EndLevel);
		const FShape::FRiverProfile& Profile = Reach.Profile;
		if (Profile.Num < 2)
		{
			System->Refused = TEXT("a river reach with no vertex outside the water it meets");
			break;
		}
		if (Profile.DeepestCut > MaxCut)
		{
			System->Refused = FString::Printf(TEXT("the bed of the river reach from cell (%d, %d) would lie %.0f uu under the ground (the cap is %.0f)"),
				Chain.Cells[0].X, Chain.Cells[0].Y, Profile.DeepestCut, MaxCut);
			break;
		}
		System->DeepestCut = FMath::Max(System->DeepestCut, Profile.DeepestCut);

		// The bed, a lower-only line, and the river reach on the same vertices. A line's half width must be over
		// nothing, so the bed's is a unit where the river narrows to nothing (a head): the footprint there is the bank.
		FVoxelConditioningLineParams& Bed = Reach.BedParams;
		Bed.FalloffWidth = static_cast<float>(BankWidth);
		Bed.Strength = 1.0f;
		Bed.Blend = EVoxelConditioningLineBlend::LowerOnly;
		Bed.Id = MakeBedLineId(Reach.StartCell, bRadial);
		Bed.bRadial = bRadial;
		Bed.Surface.BandMaterialID = Settings.BedMaterialID;
		Bed.Surface.MinCaveDepth = Settings.BedMinCaveDepth;
		Bed.Surface.ClearHalfWidth = static_cast<float>(Chain.HalfWidth + 0.5 * BankWidth);
		Bed.Vertices.Reserve(Profile.Num);
		TArray<FVoxelWaterReachVertex> Vertices;
		Vertices.Reserve(Profile.Num);
		for (int32 Vertex = Profile.First; Vertex < Profile.First + Profile.Num; ++Vertex)
		{
			const double Half = Chain.HalfWidth * Profile.Taper[Vertex];
			// A vertex's position is its place: the column, or on a planet the direction. A bed's target is a line's
			// height, a river reach's levels are water levels.
			const FVector Place = PlaceOfPoint(Chain.Points[Vertex]);
			Bed.Vertices.Add(FVoxelConditioningLineVertex(Place, static_cast<float>(Profile.Bed[Vertex]), static_cast<float>(FMath::Max(Half, 1.0))));
			Vertices.Add(FVoxelWaterReachVertex(Place, Profile.Bed[Vertex] + LevelBase, Profile.Surface[Vertex] + LevelBase, Half + BankWidth));
			if (Vertex > Profile.First)
			{
				Reach.Length += FVector::Dist(Chain.Points[Vertex], Chain.Points[Vertex - 1]);
			}
		}
		const TSharedRef<const FVoxelWaterReach> Shape = FVoxelWaterReach::Make(MoveTemp(Vertices), bRadial, Settings.FlowSpeed);
		if (Shape->NumSegments() < 1)
		{
			System->Refused = TEXT("a river reach of one place");
			break;
		}
		Reach.Bed = FVoxelConditioningLine::Make(Bed);
		for (const FString& Warning : Reach.Bed->GetWarnings(Limits))
		{
			// A head narrows the floor to nothing on purpose.
			if (!Profile.bStartsInBody && Warning.Contains(TEXT("the levelled core is")))
			{
				continue;
			}
			BendFault = FString::Printf(TEXT("the bed of the river reach from cell (%d, %d): %s"), Chain.Cells[0].X, Chain.Cells[0].Y, *Warning);
			break;
		}
		if (!BendFault.IsEmpty())
		{
			break;
		}
		Reach.Body = FVoxelWaterBody::MakeReach(MakeReachId(Ground.Seed, Reach.StartCell), Shape, Frame);
		System->Reaches.Add(MoveTemp(Reach));
	}
	System->NumAttempts = Attempt + 1;
	if (BendFault.IsEmpty())
	{
		break;
	}
	if (Attempt + 1 == NumAttempts)
	{
		System->Refused = BendFault;
	}
	} // the attempts
	if (!System->Refused.IsEmpty())
	{
		// Whole or not at all: a reach left out would leave the ones above it ending on land.
		System->Reaches.Reset();
		System->DeepestCut = 0.0;
	}
	else
	{
		System->Reaches.Sort([](const FVoxelHydrologyReach& A, const FVoxelHydrologyReach& B) { return CellLess(A.StartCell, B.StartCell); });
	}
	return System;
}

// ==================== A region ====================

void FVoxelHydrology::BuildRegion(const FVoxelHydrologySettings& InSettings, const FVoxelHydrologyGround& Ground, const FVoxelConditioningRegion& Region,
	IVoxelHydrologyCache* Cache, FVoxelHydrologyRegion& Out)
{
	using namespace VoxelHydrologyPrivate;
	Out = FVoxelHydrologyRegion();
	// A region of the world's own kind: a cap on a planet, a box on a heightmap world.
	if (!Ground.IsValid() || Region.bEverywhere || Region.bCap != Ground.bRadial || (!Region.bCap && !Region.Box.bIsValid))
	{
		return;
	}
	const FVoxelHydrologySettings Settings = Sanitize(InSettings, Ground);
	FVoxelHydrologyHeights Heights(Ground, Settings);
	const double CellSize = Heights.GetCellSize();
	const double MouthReach = Settings.MaxRiverLength + CellSize;

	// Every spring a body of the region may depend on: its descent, the kept lakes on its way, and, when it is a river
	// whose mouth is near enough for one of its reaches to come here, its system.
	TArray<FIntPoint> Squares;
	GetSquaresFor(Settings, Ground, Region, Squares);
	TMap<FIntPoint, TArray<TSharedRef<const FVoxelHydrologyTrace>>> ByMouth;
	TMap<uint32, TSharedRef<const FVoxelHydrologyLake>> Lakes;
	{
		for (const FIntPoint& Square : Squares)
		{
			++Out.NumSquares;
			TSharedPtr<const FVoxelHydrologyTrace> Trace = Cache ? Cache->FindTrace(Square) : nullptr;
			if (!Trace.IsValid())
			{
				const TSharedRef<const FVoxelHydrologyTrace> Made = TraceSpring(Settings, Heights, Square, Cache);
				Trace = Cache ? Cache->StoreTrace(Made).ToSharedPtr() : Made.ToSharedPtr();
				++Out.NumTraced;
			}
			if (Trace->End == EVoxelHydrologyEnd::NoSpring)
			{
				continue;
			}
			++Out.NumSprings;
			for (const TSharedPtr<const FVoxelHydrologyLake>& Lake : Trace->Lakes)
			{
				if (Lake->bKept && !Lakes.Contains(Lake->Body.Id) && MeetsRegion(Lake->Body, Region, Ground))
				{
					Lakes.Add(Lake->Body.Id, Lake.ToSharedRef());
				}
			}
			if (Trace->bKept)
			{
				++Out.NumRivers;
				if (DistanceToRegion(Region, Ground, Heights.PlaceOf(Trace->Mouth)) <= MouthReach)
				{
					ByMouth.FindOrAdd(Trace->Mouth).Add(Trace.ToSharedRef());
				}
			}
		}
	}

	Lakes.GenerateValueArray(Out.Lakes);
	Out.Lakes.Sort([](const TSharedRef<const FVoxelHydrologyLake>& A, const TSharedRef<const FVoxelHydrologyLake>& B) { return A->Body.Id < B->Body.Id; });

	TArray<FIntPoint> Mouths;
	ByMouth.GenerateKeyArray(Mouths);
	Mouths.Sort([](const FIntPoint& A, const FIntPoint& B) { return CellLess(A, B); });
	for (const FIntPoint& Mouth : Mouths)
	{
		TSharedPtr<const FVoxelHydrologySystem> System = Cache ? Cache->FindSystem(Mouth) : nullptr;
		if (!System.IsValid())
		{
			TArray<TSharedRef<const FVoxelHydrologyTrace>>& Rivers = ByMouth[Mouth];
			Rivers.Sort([](const TSharedRef<const FVoxelHydrologyTrace>& A, const TSharedRef<const FVoxelHydrologyTrace>& B) { return CellLess(A->Square, B->Square); });
			const TSharedRef<const FVoxelHydrologySystem> Made = BuildSystem(Settings, Heights, Mouth, Rivers);
			System = Cache ? Cache->StoreSystem(Made).ToSharedPtr() : Made.ToSharedPtr();
			++Out.NumSystemsBuilt;
		}
		if (!System->Refused.IsEmpty())
		{
			++Out.NumRefused;
			Out.Refused.Add(System.ToSharedRef());
		}
		for (const FVoxelHydrologyReach& Reach : System->Reaches)
		{
			if (MeetsRegion(Reach.Body, Region, Ground) || (Reach.Bed.IsValid() && MeetsRegion(*Reach.Bed, Region, Ground)))
			{
				Out.Systems.Add(System.ToSharedRef());
				break;
			}
		}
	}

	// The valley rivers that reach the region (the valleys plan, D179): a piece a valley link, after the brooks' systems
	if (Ground.Valleys.IsValid())
	{
		TArray<TSharedRef<const FVoxelHydrologySystem>> ValleyRivers;
		GatherValleyRivers(Settings, Ground, Region, Cache, ValleyRivers);
		for (const TSharedRef<const FVoxelHydrologySystem>& River : ValleyRivers)
		{
			for (const FVoxelHydrologyReach& Reach : River->Reaches)
			{
				if (MeetsRegion(Reach.Body, Region, Ground) || (Reach.Bed.IsValid() && MeetsRegion(*Reach.Bed, Region, Ground)))
				{
					Out.Systems.Add(River);
					break;
				}
			}
		}
	}
	Out.NumSamples = Heights.GetNumSamples();
}
