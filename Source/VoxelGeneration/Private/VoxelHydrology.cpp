// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelHydrology.h"
#include "IVoxelWorldMode.h"
#include "VoxelBlockSurface.h"
#include "VoxelHash.h"
#include "VoxelRiverCourse.h"
#include "VoxelWorldConfiguration.h"
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

	double StepLength(const FIntPoint& A, const FIntPoint& B, double CellSize)
	{
		return CellSize * FMath::Sqrt(static_cast<double>(FMath::Square(A.X - B.X) + FMath::Square(A.Y - B.Y)));
	}

	/** The distance from a place to a box (0 inside it). */
	double DistanceToBox(const FBox2D& Box, const FVector2D& Place)
	{
		return FMath::Sqrt(Box.ComputeSquaredDistanceToPoint(Place));
	}

	/** Whether a cell's ground lies under the water of a kept lake: under its level, inside its footprint. */
	bool IsUnderLake(const FVoxelHydrologyLake& Lake, FVoxelHydrologyHeights& Heights, const FIntPoint& Cell)
	{
		if (!Lake.bKept || !Lake.Body.Lake.IsValid() || Heights.At(Cell) >= Lake.WaterLevel)
		{
			return false;
		}
		const FVector2D Place = Heights.PlaceOf(Cell);
		return Lake.Body.Lake->Covers(Place.X, Place.Y);
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
	Ground.bSea = Sea.bEnabled && !Sea.bRadial;
	Ground.SeaLevel = Sea.PlaneZ;
	Ground.VoxelSize = Config.VoxelSize;
	Ground.ChunkWorldSize = Config.GetChunkWorldSize();
	Ground.Seed = Noise.Seed;
	return Ground;
}

FVoxelHydrologyHeights::FVoxelHydrologyHeights(const FVoxelHydrologyGround& InGround, const FVoxelHydrologySettings& InSettings)
	: Ground(InGround)
	, CellSize(FMath::Max(InSettings.CellVoxels, 1) * FMath::Max(InGround.VoxelSize, 1.0))
{
}

double FVoxelHydrologyHeights::At(const FIntPoint& Cell)
{
	if (const double* Found = Kept.Find(Cell))
	{
		return *Found;
	}
	++NumSamples;
	const double Height = Ground.Height(Cell.X * CellSize, Cell.Y * CellSize);
	Kept.Add(Cell, Height);
	return Height;
}

bool FVoxelHydrologyHeights::GetDownhill(const FIntPoint& Cell, FIntPoint& OutNext)
{
	using namespace VoxelHydrologyPrivate;
	const double Here = At(Cell);
	double BestFall = 0.0;
	bool bFound = false;
	for (int32 D = 0; D < 8; ++D)
	{
		const FIntPoint Neighbour(Cell.X + DX[D], Cell.Y + DY[D]);
		const double Fall = (Here - At(Neighbour)) / (D < 4 ? 1.0 : UE_DOUBLE_SQRT_2);
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
	return Config.HydrologyConfiguration != nullptr
		&& (Config.WorldMode == EWorldMode::InfinitePlane || Config.WorldMode == EWorldMode::IslandBowl)
		&& !FVoxelBlockSurface::UsesBlockSurface(Config);
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

FIntRect FVoxelHydrology::GetSquaresFor(const FVoxelHydrologySettings& Settings, const FVoxelHydrologyGround& Ground, const FBox2D& Region)
{
	const double CellSize = Settings.CellVoxels * FMath::Max(Ground.VoxelSize, 1.0);
	const double Square = Settings.SpringSpacingCells * CellSize;
	const double Distance = GetDependencyDistance(Settings, Ground);
	// A square's spring lies anywhere in it: every square that comes within the distance.
	return FIntRect(
		FMath::FloorToInt32((Region.Min.X - Distance) / Square), FMath::FloorToInt32((Region.Min.Y - Distance) / Square),
		FMath::FloorToInt32((Region.Max.X + Distance) / Square) + 1, FMath::FloorToInt32((Region.Max.Y + Distance) / Square) + 1);
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

int64 FVoxelHydrology::MakeBedLineId(const FIntPoint& StartCell)
{
	// 21 bits a coordinate (cells to a million either way of the origin), counted up from -2^60: over every road's and
	// trail's (under -2^61) and under every hand-placed line's of order -2^27 or more.
	const auto Bits = [](int32 Coordinate) { return static_cast<int64>((Coordinate + (1 << 20)) & 0x1FFFFF); };
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
	OutCell = FIntPoint(Square.X * Spacing + OffsetX, Square.Y * Spacing + OffsetY);
	if (!Ground.bSea)
	{
		return true;
	}
	return Heights.At(OutCell) >= Ground.SeaLevel + Settings.MinSpringHeight;
}

namespace VoxelHydrologyPrivate
{
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
	 */
	void MakeLakeBody(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, FVoxelHydrologyLake& Lake)
	{
		const FVoxelHydrologyGround& Ground = Heights.GetGround();
		const double CellSize = Heights.GetCellSize();
		const double Voxel = FMath::Max(Ground.VoxelSize, 1.0);
		const double Margin = Settings.LakeLevelMarginVoxels * Voxel;
		const double MinDepth = Settings.MinLakeDepthVoxels * Voxel;

		// What the hollow could hold at most, on the generator's grid: too little, and the fine flood is not run.
		Lake.WaterLevel = Lake.Closed == 1 ? Lake.RimLevel : Lake.RimLevel - Margin;
		Lake.Depth = FMath::Max(Lake.RimLevel - Lake.LowestGround, 0.0);
		Lake.Area = Lake.Cells.Num() * CellSize * CellSize;
		if (Lake.Depth < 0.5 * MinDepth || Lake.Area < 0.5 * Settings.MinLakeArea)
		{
			return;
		}

		// The fine grid: two voxels a cell where the generator's cell is a whole number of them.
		const int32 FineVoxels = (Settings.CellVoxels >= 4 && Settings.CellVoxels % 2 == 0) ? 2 : 1;
		const int32 PerCell = Settings.CellVoxels / FineVoxels;
		const double Fine = FineVoxels * Voxel;
		TSet<FIntPoint> Domain;
		Domain.Reserve(Lake.Cells.Num() * 2);
		for (const FIntPoint& Cell : Lake.Cells)
		{
			Domain.Add(Cell);
			for (int32 D = 0; D < 8; ++D)
			{
				Domain.Add(FIntPoint(Cell.X + DX[D], Cell.Y + DY[D]));
			}
		}
		const auto InDomain = [&Domain, PerCell](const FIntPoint& FineCell)
		{
			return Domain.Contains(FIntPoint(FMath::RoundToInt32(static_cast<double>(FineCell.X) / PerCell), FMath::RoundToInt32(static_cast<double>(FineCell.Y) / PerCell)));
		};
		TMap<FIntPoint, double> FineHeights;
		const auto HeightAt = [&FineHeights, &Ground, Fine](const FIntPoint& FineCell)
		{
			if (const double* Found = FineHeights.Find(FineCell))
			{
				return *Found;
			}
			const double Height = Ground.Height(FineCell.X * Fine, FineCell.Y * Fine);
			FineHeights.Add(FineCell, Height);
			return Height;
		};

		// Down from the hollow's lowest cell to the pit under it.
		FIntPoint Start(Lake.LowestCell.X * PerCell, Lake.LowestCell.Y * PerCell);
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
		if (Rim > Lake.RimLevel)
		{
			// Never over the rim the hollow was found with: the cells the river's ways are laid over lie under that one.
			// The water is then what the flood had taken when it first climbed past it.
			Rim = Lake.RimLevel;
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
		const double FirstLevel = bDepthCapped ? Rim : Rim - Margin;
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
		// cell's square is centred on its place.
		double LowestEdge = TNumericLimits<double>::Max();
		for (const FIntPoint& Cell : Footprint)
		{
			for (int32 D = 0; D < 4; ++D)
			{
				if (Footprint.Contains(FIntPoint(Cell.X + DX[D], Cell.Y + DY[D])))
				{
					continue;
				}
				const FVector2D Centre(Cell.X * Fine, Cell.Y * Fine);
				const FVector2D Out(DX[D], DY[D]);
				const FVector2D Along(-Out.Y, Out.X);
				const FVector2D From = Centre + Out * (0.5 * Fine) - Along * (0.5 * Fine);
				for (int32 Step = 0; Step <= FineVoxels; ++Step)
				{
					const FVector2D Place = From + Along * (Fine * Step / FineVoxels);
					LowestEdge = FMath::Min(LowestEdge, Ground.Height(Place.X, Place.Y));
				}
			}
		}
		Lake.WaterLevel = FMath::Min(FirstLevel, LowestEdge - Margin);
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

		const FIntPoint Size(FootMax.X - FootMin.X + 1, FootMax.Y - FootMin.Y + 1);
		TBitArray<> Bits(false, Size.X * Size.Y);
		for (const FIntPoint& Cell : Footprint)
		{
			Bits[(Cell.Y - FootMin.Y) * Size.X + (Cell.X - FootMin.X)] = true;
		}
		const FVector2D Origin((FootMin.X - 0.5) * Fine, (FootMin.Y - 0.5) * Fine);
		const TSharedRef<const FVoxelWaterLake> Shape = FVoxelWaterLake::Make(Origin, Fine, Size, MoveTemp(Bits));
		Lake.Body = FVoxelWaterBody::MakeLake(FVoxelHydrology::MakeLakeId(Ground.Seed, Lake.LowestCell, Lake.Cells.Num()), Lake.WaterLevel, Shape, FVoxelWaterFrame(),
			Lake.Depth + Ground.ChunkWorldSize, Settings.LakeMinCaveDepth);
		Lake.bKept = true;
	}
}

// ==================== A pit's lake ====================

TSharedRef<const FVoxelHydrologyLake> FVoxelHydrology::MakeLake(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Pit)
{
	using namespace VoxelHydrologyPrivate;
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
	FIntPoint MinCell = Pit, MaxCell = Pit;
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
			if (Taken.Num() > MaxCells || MaxCell.X - MinCell.X >= MaxSpan || MaxCell.Y - MinCell.Y >= MaxSpan)
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
		MinCell = FIntPoint(FMath::Min(MinCell.X, Entry.Cell.X), FMath::Min(MinCell.Y, Entry.Cell.Y));
		MaxCell = FIntPoint(FMath::Max(MaxCell.X, Entry.Cell.X), FMath::Max(MaxCell.Y, Entry.Cell.Y));
		if (Taken.Num() > Budget)
		{
			break;
		}
		for (int32 D = 0; D < 8; ++D)
		{
			const FIntPoint Neighbour(Entry.Cell.X + DX[D], Entry.Cell.Y + DY[D]);
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
			const FIntPoint Neighbour(Lake->Saddle.X + DX[D], Lake->Saddle.Y + DY[D]);
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
				const FIntPoint Neighbour(Cell.X + DX[D], Cell.Y + DY[D]);
				if (Lake->Cells.Contains(Neighbour) && !Lake->ToSaddle.Contains(Neighbour))
				{
					Lake->ToSaddle.Add(Neighbour, Cell);
					Queue.Add(Neighbour);
				}
			}
		}
	}

	VoxelHydrologyPrivate::MakeLakeBody(Settings, Heights, Lake.Get());
	return Lake;
}

// ==================== A spring's descent ====================

TSharedRef<const FVoxelHydrologyTrace> FVoxelHydrology::TraceSpring(const FVoxelHydrologySettings& Settings, FVoxelHydrologyHeights& Heights, const FIntPoint& Square,
	IVoxelHydrologyCache* Cache)
{
	using namespace VoxelHydrologyPrivate;
	const double CellSize = Heights.GetCellSize();
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
			Length += StepLength(Cell, Next, CellSize);
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
		if (Lake->IsOpen() && Entry > DescentStart && Cells[Entry - 1] == Lake->Saddle)
		{
			// The descent came to the hollow over its own saddle (the steepest way down from a saddle may lead in, while
			// the lowest ground beside it lies out): water that stands on the saddle leaves by the outlet, as the lake's
			// own water does, so the way on from the saddle is the same whichever descent comes to it.
			for (int32 Index = Entry - 1; Index + 1 < Cells.Num(); ++Index)
			{
				Length -= StepLength(Cells[Index], Cells[Index + 1], CellSize);
			}
			Cells.SetNum(Entry, EAllowShrinking::No);
			LakeOf.SetNum(Entry, EAllowShrinking::No);
			if (Trace->Lakes.Num() > Settings.MaxLakesPerRiver)
			{
				Trace->End = EVoxelHydrologyEnd::TooManyLakes;
				break;
			}
			Length += StepLength(Lake->Saddle, Lake->Outlet, CellSize);
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
				Length -= StepLength(Cells[Index], Cells[Index + 1], CellSize);
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
			Length -= StepLength(Cells[Index], Cells[Index + 1], CellSize);
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
			Length += StepLength(At, *Way, CellSize);
			At = *Way;
			Push(At, LakeIndex);
		}
		Length += StepLength(At, Lake->Saddle, CellSize);
		Push(Lake->Saddle, INDEX_NONE);
		Length += StepLength(Lake->Saddle, Lake->Outlet, CellSize);
		Cell = Lake->Outlet;
		DescentStart = Cells.Num();
	}

	Trace->Length = Length;
	Trace->Fall = Cells.Num() > 0 ? Heights.At(Trace->Spring) - Heights.At(Cells.Last()) : 0.0;
	const bool bEndsInWater = Trace->End == EVoxelHydrologyEnd::Sea || (Trace->End == EVoxelHydrologyEnd::ClosedLake && bEndsUnderLake);
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
	 * @param Raw The cells' places
	 * @param Spacing The vertices' spacing
	 * @param MaxTurn The bound on a vertex's turn
	 * @param Distance The distance the first try spreads over
	 * @param Out Receives the vertices
	 * @return Whether the line is within the bound (with the shape's margin)
	 */
	bool RoundChain(TConstArrayView<FVector> Raw, double Spacing, double MaxTurn, double Distance, TArray<FVector>& Out)
	{
		using FShape = FVoxelConditioningLineShape;
		TArray<FVector> Dense;
		FShape::ResampleEvenly(Raw, Spacing, Dense);
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
			FShape::ResampleEvenly(Cut, Spacing, Out);
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
	const double CellSize = Heights.GetCellSize();
	const double Voxel = FMath::Max(Ground.VoxelSize, 1.0);
	const double Spacing = 2.0 * Voxel;
	const double BankWidth = Settings.BankWidthVoxels * Voxel;
	const double BankMargin = Settings.BankMarginVoxels * Voxel;

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

	// The water the reaches meet: the sea and the kept lakes on the rivers' ways.
	TArray<FVoxelWaterBody> Bodies;
	LakeBodies.GenerateValueArray(Bodies);
	const FVoxelSeaSurface Sea = Ground.bSea ? FVoxelSeaSurface::MakePlane(Ground.SeaLevel) : FVoxelSeaSurface();
	const FVoxelWaterLevels Water = FVoxelWaterLevels::Make(Sea, FVoxelWaterFrame(), FVoxelWaterLevels::WholeWorld(), MoveTemp(Bodies));
	const auto GroundAt = [&Ground](const FVector& Place) { return Ground.Height(Place.X, Place.Y); };
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

	// The engine's own check of a bed's line: the bend bound above all. The cut's cap is the profile's (MaxCutVoxels).
	FVoxelConditioningLineLimits Limits;
	Limits.VoxelSize = static_cast<float>(Voxel);
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
			const FVector2D Place = Heights.PlaceOf(Cell);
			Raw.Add(FVector(Place.X, Place.Y, 0.0));
		}
		const double MaxTurn = FShape::GetMaxTurnForReach(Spacing, Chain.HalfWidth + BankWidth);
		Chain.bRounded = RoundChain(Raw, Spacing, MaxTurn, CellSize * CornerCells[Attempt], Chain.Points);
		Chain.Ground.Reserve(Chain.Points.Num());
		Chain.BodyLevels.Reserve(Chain.Points.Num());
		for (const FVector& Point : Chain.Points)
		{
			const double Height = GroundAt(Point);
			Chain.Ground.Add(Height);
			Chain.BodyLevels.Add(BodyLevelAt(Point, Height));
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
		FVoxelRiverCourse::ReadBanks(Chain.Points, TConstArrayView<FVector>(), Floors, BankWidth, Voxel, BankMargin, GroundAt, BodyLevelAt, BankGround);
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
		Bed.Id = MakeBedLineId(Reach.StartCell);
		Bed.bRadial = false;
		Bed.Surface.BandMaterialID = Settings.BedMaterialID;
		Bed.Surface.MinCaveDepth = Settings.BedMinCaveDepth;
		Bed.Surface.ClearHalfWidth = static_cast<float>(Chain.HalfWidth + 0.5 * BankWidth);
		Bed.Vertices.Reserve(Profile.Num);
		TArray<FVoxelWaterReachVertex> Vertices;
		Vertices.Reserve(Profile.Num);
		for (int32 Vertex = Profile.First; Vertex < Profile.First + Profile.Num; ++Vertex)
		{
			const double Half = Chain.HalfWidth * Profile.Taper[Vertex];
			Bed.Vertices.Add(FVoxelConditioningLineVertex(Chain.Points[Vertex], static_cast<float>(Profile.Bed[Vertex]), static_cast<float>(FMath::Max(Half, 1.0))));
			Vertices.Add(FVoxelWaterReachVertex(Chain.Points[Vertex], Profile.Bed[Vertex], Profile.Surface[Vertex], Half + BankWidth));
			if (Vertex > Profile.First)
			{
				Reach.Length += FVector::Dist(Chain.Points[Vertex], Chain.Points[Vertex - 1]);
			}
		}
		const TSharedRef<const FVoxelWaterReach> Shape = FVoxelWaterReach::Make(MoveTemp(Vertices), false, Settings.FlowSpeed);
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
		Reach.Body = FVoxelWaterBody::MakeReach(MakeReachId(Ground.Seed, Reach.StartCell), Shape, FVoxelWaterFrame());
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

void FVoxelHydrology::BuildRegion(const FVoxelHydrologySettings& InSettings, const FVoxelHydrologyGround& Ground, const FBox2D& Region, IVoxelHydrologyCache* Cache,
	FVoxelHydrologyRegion& Out)
{
	using namespace VoxelHydrologyPrivate;
	Out = FVoxelHydrologyRegion();
	if (!Ground.Height || !Region.bIsValid)
	{
		return;
	}
	const FVoxelHydrologySettings Settings = Sanitize(InSettings, Ground);
	FVoxelHydrologyHeights Heights(Ground, Settings);
	const double CellSize = Heights.GetCellSize();
	const double MouthReach = Settings.MaxRiverLength + CellSize;

	// Every spring a body of the region may depend on: its descent, the kept lakes on its way, and, when it is a river
	// whose mouth is near enough for one of its reaches to come here, its system.
	const FIntRect Squares = GetSquaresFor(Settings, Ground, Region);
	TMap<FIntPoint, TArray<TSharedRef<const FVoxelHydrologyTrace>>> ByMouth;
	TMap<uint32, TSharedRef<const FVoxelHydrologyLake>> Lakes;
	for (int32 Y = Squares.Min.Y; Y < Squares.Max.Y; ++Y)
	{
		for (int32 X = Squares.Min.X; X < Squares.Max.X; ++X)
		{
			++Out.NumSquares;
			const FIntPoint Square(X, Y);
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
				if (Lake->bKept && !Lakes.Contains(Lake->Body.Id) && Lake->Body.MeetsRegion(Region))
				{
					Lakes.Add(Lake->Body.Id, Lake.ToSharedRef());
				}
			}
			if (Trace->bKept)
			{
				++Out.NumRivers;
				if (DistanceToBox(Region, Heights.PlaceOf(Trace->Mouth)) <= MouthReach)
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
			if (Reach.Body.MeetsRegion(Region) || (Reach.Bed.IsValid() && Reach.Bed->MeetsRegion(Region)))
			{
				Out.Systems.Add(System.ToSharedRef());
				break;
			}
		}
	}
	Out.NumSamples = Heights.GetNumSamples();
}
