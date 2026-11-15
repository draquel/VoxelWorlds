// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

namespace VoxelWaterLevelsPrivate
{
	/** A region of the world by column: a heightmap world's XY box, or a planet's cap. */
	struct FColumns
	{
		bool bCap = false;
		FBox2D Region = FBox2D(ForceInit);
		FVector Direction = FVector::UpVector;
		double Angle = 0.0;
		double PlanetRadius = 0.0;
	};

	/** The columns a world box stands over: its XY, or the cap it subtends from the planet centre (the whole sphere when it holds the centre). */
	FColumns ColumnsOf(const FBox& Box, const FVoxelWaterFrame& Frame)
	{
		FColumns Columns;
		if (!Frame.bRadial)
		{
			Columns.Region = FBox2D(FVector2D(Box.Min.X, Box.Min.Y), FVector2D(Box.Max.X, Box.Max.Y));
			return Columns;
		}
		Columns.bCap = true;
		Columns.PlanetRadius = Frame.DatumRadius;
		const FVector Offset = Box.GetCenter() - Frame.Center;
		const double Distance = Offset.Size();
		const double HalfDiagonal = Box.GetExtent().Size();
		Columns.Direction = Distance > UE_KINDA_SMALL_NUMBER ? Offset / Distance : FVector::UpVector;
		Columns.Angle = Distance <= HalfDiagonal ? UE_DOUBLE_PI : FMath::Asin(HalfDiagonal / Distance);
		return Columns;
	}

	/** Whether a lake or a river reach may reach some of the columns, and the lowest and highest level it has there. */
	bool LevelsOver(const FVoxelWaterBody& Body, const FColumns& Columns, double& OutMin, double& OutMax)
	{
		if (Body.Kind == EVoxelWaterBodyKind::Lake && Body.Lake.IsValid())
		{
			OutMin = OutMax = Body.Level;
			return Columns.bCap ? Body.Lake->MeetsCap(Columns.Direction, Columns.Angle) : Body.Lake->MeetsRegion(Columns.Region);
		}
		if (Body.Kind == EVoxelWaterBodyKind::Reach && Body.Reach.IsValid())
		{
			return Columns.bCap
				? Body.Reach->GetLevelRangeOnSphere(Columns.Direction, Columns.Angle, Columns.PlanetRadius, OutMin, OutMax)
				: Body.Reach->GetLevelRange(Columns.Region, OutMin, OutMax);
		}
		return false;
	}

	/** Whether a point lies inside a closed polygon (even-odd). */
	bool IsInsidePolygon(const FVector2D& P, TConstArrayView<FVector2D> Polygon)
	{
		bool bInside = false;
		for (int32 I = 0, J = Polygon.Num() - 1; I < Polygon.Num(); J = I++)
		{
			const FVector2D& A = Polygon[I];
			const FVector2D& B = Polygon[J];
			if ((A.Y > P.Y) != (B.Y > P.Y) && P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X)
			{
				bInside = !bInside;
			}
		}
		return bInside;
	}
}

// ==================== The frame ====================

FVoxelWaterFrame FVoxelWaterFrame::FromConfig(const UVoxelWorldConfiguration& Config)
{
	FVoxelWaterFrame Frame;
	if (Config.WorldMode == EWorldMode::SphericalPlanet)
	{
		Frame.bRadial = true;
		Frame.Center = Config.WorldOrigin;
		Frame.DatumRadius = FMath::Max(static_cast<double>(Config.WorldRadius), 1.0);
	}
	return Frame;
}

// ==================== A river reach ====================

TSharedRef<const FVoxelWaterReach> FVoxelWaterReach::Make(TArray<FVoxelWaterReachVertex> InVertices, bool bInRadial, float InFlowSpeed)
{
	TSharedRef<FVoxelWaterReach> Reach = MakeShared<FVoxelWaterReach>();
	Reach->FlowSpeed = FMath::Max(InFlowSpeed, 0.0f);

	// Two vertices at one place make a segment of no length: merged by the conditioning line's rule
	// (FVoxelConditioningLine::Make), so a reach and its bed made from the same vertices have the same segments.
	const double SamePlace = bInRadial ? 1.0e-10 : 1.0e-3;
	Reach->Vertices.Reserve(InVertices.Num());
	for (const FVoxelWaterReachVertex& In : InVertices)
	{
		FVoxelWaterReachVertex Vertex = In;
		Vertex.HalfWidth = FMath::Max(In.HalfWidth, 0.0);
		if (bInRadial)
		{
			Vertex.Position = In.Position.GetSafeNormal();
			if (Vertex.Position.IsZero())
			{
				continue;
			}
		}
		else
		{
			Vertex.Position.Z = 0.0;
		}
		if (Reach->Vertices.Num() > 0 && Reach->Vertices.Last().Position.Equals(Vertex.Position, SamePlace))
		{
			continue;
		}
		Reach->Vertices.Add(Vertex);
	}
	if (Reach->Vertices.Num() < 2)
	{
		Reach->Vertices.Reset();
	}

	for (int32 Index = 0; Index < Reach->Vertices.Num(); ++Index)
	{
		const FVoxelWaterReachVertex& Vertex = Reach->Vertices[Index];
		Reach->MaxHalfWidth = FMath::Max(Reach->MaxHalfWidth, Vertex.HalfWidth);
		Reach->MinLevel = Index == 0 ? Vertex.SurfaceLevel : FMath::Min(Reach->MinLevel, Vertex.SurfaceLevel);
		Reach->MaxLevel = Index == 0 ? Vertex.SurfaceLevel : FMath::Max(Reach->MaxLevel, Vertex.SurfaceLevel);
	}
	const TArray<FVoxelWaterReachVertex>& Made = Reach->Vertices;
	Reach->Polyline.Build(Made.Num(), bInRadial, [&Made](int32 Index) -> const FVector& { return Made[Index].Position; });
	return Reach;
}

void FVoxelWaterReach::FillSample(int32 Segment, double T, double Distance, double PlanetRadius, FVoxelWaterReachSample& Out) const
{
	const FVoxelWaterReachVertex& A = Vertices[Segment];
	const FVoxelWaterReachVertex& B = Vertices[Segment + 1];
	Out.Segment = Segment;
	Out.T = T;
	Out.Distance = Distance;
	Out.SurfaceLevel = A.SurfaceLevel + T * (B.SurfaceLevel - A.SurfaceLevel);
	Out.BedLevel = A.BedLevel + T * (B.BedLevel - A.BedLevel);
	Out.HalfWidth = A.HalfWidth + T * (B.HalfWidth - A.HalfWidth);

	// Downstream is from A to B: along the segment in the plane, or along the chord's tangent at the nearest place on a
	// planet; the slope is the level's change over the segment's length (its arc on a planet).
	double Length = 0.0;
	if (Polyline.bRadial)
	{
		const FVector Chord = B.Position - A.Position;
		const FVector Nearest = (A.Position + Chord * T).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, A.Position);
		Out.Downstream = (Chord - Nearest * FVector::DotProduct(Chord, Nearest)).GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::ForwardVector);
		Length = PlanetRadius * FVoxelPolylineBounds::AngleBetween(A.Position, B.Position);
	}
	else
	{
		const FVector Along(B.Position.X - A.Position.X, B.Position.Y - A.Position.Y, 0.0);
		Length = Along.Size();
		Out.Downstream = Length > UE_DOUBLE_SMALL_NUMBER ? Along / Length : FVector::ForwardVector;
	}
	Out.Slope = Length > UE_DOUBLE_KINDA_SMALL_NUMBER ? (B.SurfaceLevel - A.SurfaceLevel) / Length : 0.0;
}

bool FVoxelWaterReach::SampleAt(double X, double Y, FVoxelWaterReachSample& Out) const
{
	int32 Segment = 0;
	double T = 0.0;
	double DistSq = 0.0;
	const auto PositionOf = [this](int32 Index) -> const FVector& { return Vertices[Index].Position; };
	// The search finds only what is strictly nearer than its reach: a unit more than the widest footprint, so a place on
	// the footprint's edge is found and then tested against the half width there.
	if (!Polyline.FindNearest(X, Y, MaxHalfWidth + 1.0, PositionOf, Segment, T, DistSq))
	{
		return false;
	}
	const double Distance = FMath::Sqrt(DistSq);
	const FVoxelWaterReachVertex& A = Vertices[Segment];
	const FVoxelWaterReachVertex& B = Vertices[Segment + 1];
	if (Distance > A.HalfWidth + T * (B.HalfWidth - A.HalfWidth))
	{
		return false;
	}
	FillSample(Segment, T, Distance, 0.0, Out);
	return true;
}

bool FVoxelWaterReach::SampleOnSphere(const FVector& Direction, double PlanetRadius, FVoxelWaterReachSample& Out) const
{
	int32 Segment = 0;
	double T = 0.0;
	double DistSq = 0.0;
	const auto PositionOf = [this](int32 Index) -> const FVector& { return Vertices[Index].Position; };
	if (!Polyline.FindNearestOnSphere(Direction, PlanetRadius, MaxHalfWidth + 1.0, PositionOf, Segment, T, DistSq))
	{
		return false;
	}
	const double Distance = FVoxelPolylineBounds::ChordToArc(FMath::Sqrt(DistSq), PlanetRadius);
	const FVoxelWaterReachVertex& A = Vertices[Segment];
	const FVoxelWaterReachVertex& B = Vertices[Segment + 1];
	if (Distance > A.HalfWidth + T * (B.HalfWidth - A.HalfWidth))
	{
		return false;
	}
	FillSample(Segment, T, Distance, PlanetRadius, Out);
	return true;
}

bool FVoxelWaterReach::GetLevelRange(const FBox2D& Region, double& OutMin, double& OutMax) const
{
	if (!Polyline.MeetsRegion(Region, MaxHalfWidth))
	{
		return false;
	}
	bool bAny = false;
	for (int32 Block = 0; Block < Polyline.BlockBounds.Num(); ++Block)
	{
		if (!Polyline.BlockBounds[Block].ExpandBy(MaxHalfWidth).Intersect(Region))
		{
			continue;
		}
		const int32 First = Block * FVoxelPolylineBounds::BlockSegments;
		const int32 Last = FMath::Min(First + FVoxelPolylineBounds::BlockSegments, Polyline.Segments);
		for (int32 Index = First; Index <= Last; ++Index)
		{
			const double Level = Vertices[Index].SurfaceLevel;
			OutMin = bAny ? FMath::Min(OutMin, Level) : Level;
			OutMax = bAny ? FMath::Max(OutMax, Level) : Level;
			bAny = true;
		}
	}
	return bAny;
}

bool FVoxelWaterReach::GetLevelRangeOnSphere(const FVector& CapDirection, double CapAngle, double PlanetRadius, double& OutMin, double& OutMax) const
{
	if (!Polyline.MeetsCap(CapDirection, CapAngle, PlanetRadius, MaxHalfWidth))
	{
		return false;
	}
	const double ReachAngle = MaxHalfWidth / FMath::Max(PlanetRadius, 1.0);
	bool bAny = false;
	for (int32 Block = 0; Block < Polyline.BlockBalls.Num(); ++Block)
	{
		const FVector4d& Ball = Polyline.BlockBalls[Block];
		if (CapAngle < UE_DOUBLE_PI && FVoxelPolylineBounds::AngleBetween(CapDirection, FVector(Ball.X, Ball.Y, Ball.Z))
			> CapAngle + 2.0 * FMath::Asin(FMath::Min(1.0, 0.5 * Ball.W)) + ReachAngle)
		{
			continue;
		}
		const int32 First = Block * FVoxelPolylineBounds::BlockSegments;
		const int32 Last = FMath::Min(First + FVoxelPolylineBounds::BlockSegments, Polyline.Segments);
		for (int32 Index = First; Index <= Last; ++Index)
		{
			const double Level = Vertices[Index].SurfaceLevel;
			OutMin = bAny ? FMath::Min(OutMin, Level) : Level;
			OutMax = bAny ? FMath::Max(OutMax, Level) : Level;
			bAny = true;
		}
	}
	return bAny;
}

// ==================== A lake's footprint ====================

TSharedRef<const FVoxelWaterLake> FVoxelWaterLake::Make(const FVector2D& InOrigin, double InCellSize, const FIntPoint& InSize, TBitArray<> InCells)
{
	TSharedRef<FVoxelWaterLake> Lake = MakeShared<FVoxelWaterLake>();
	Lake->Origin = InOrigin;
	Lake->CellSize = FMath::Max(InCellSize, 1.0);
	Lake->Size = FIntPoint(FMath::Max(InSize.X, 0), FMath::Max(InSize.Y, 0));
	Lake->Cells = MoveTemp(InCells);
	Lake->Finish();
	return Lake;
}

TSharedRef<const FVoxelWaterLake> FVoxelWaterLake::MakeOnSphere(const FVector& InCentre, double InPlanetRadius, double InCellSize,
	const FIntPoint& InSize, TBitArray<> InCells)
{
	TSharedRef<FVoxelWaterLake> Lake = MakeShared<FVoxelWaterLake>();
	Lake->bRadial = true;
	Lake->CellSize = FMath::Max(InCellSize, 1.0);
	Lake->Size = FIntPoint(FMath::Max(InSize.X, 0), FMath::Max(InSize.Y, 0));
	Lake->Cells = MoveTemp(InCells);
	Lake->PlanetRadius = FMath::Max(InPlanetRadius, 1.0);
	Lake->Centre = InCentre.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
	// The tangent plane's axes, the same on every machine: from the world axis the centre is farther from.
	const FVector Reference = FMath::Abs(Lake->Centre.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector;
	Lake->AxisX = FVector::CrossProduct(Reference, Lake->Centre).GetSafeNormal();
	Lake->AxisY = FVector::CrossProduct(Lake->Centre, Lake->AxisX);
	Lake->Origin = FVector2D(-0.5 * Lake->Size.X * Lake->CellSize, -0.5 * Lake->Size.Y * Lake->CellSize);
	Lake->Finish();
	return Lake;
}

TSharedRef<const FVoxelWaterLake> FVoxelWaterLake::MakeFromOutline(TConstArrayView<FVector2D> Outline, double InCellSize)
{
	const double Cell = FMath::Max(InCellSize, 1.0);
	FBox2D Box(ForceInit);
	for (const FVector2D& Corner : Outline)
	{
		Box += Corner;
	}
	if (Outline.Num() < 3 || !Box.bIsValid)
	{
		return Make(FVector2D::ZeroVector, Cell, FIntPoint::ZeroValue, TBitArray<>());
	}
	// The grid is aligned to the world, with a cell of margin for the footprint's growth.
	const FIntPoint MinCell(FMath::FloorToInt32(Box.Min.X / Cell) - 1, FMath::FloorToInt32(Box.Min.Y / Cell) - 1);
	const FIntPoint MaxCell(FMath::FloorToInt32(Box.Max.X / Cell) + 1, FMath::FloorToInt32(Box.Max.Y / Cell) + 1);
	const FIntPoint GridSize(MaxCell.X - MinCell.X + 1, MaxCell.Y - MinCell.Y + 1);
	const FVector2D GridOrigin(MinCell.X * Cell, MinCell.Y * Cell);

	TBitArray<> Inside(false, GridSize.X * GridSize.Y);
	for (int32 Y = 0; Y < GridSize.Y; ++Y)
	{
		for (int32 X = 0; X < GridSize.X; ++X)
		{
			if (VoxelWaterLevelsPrivate::IsInsidePolygon(GridOrigin + FVector2D((X + 0.5) * Cell, (Y + 0.5) * Cell), Outline))
			{
				Inside[Y * GridSize.X + X] = true;
			}
		}
	}
	// Grown by one cell, so the footprint reaches past the outline's own columns.
	TBitArray<> Grown(false, GridSize.X * GridSize.Y);
	for (int32 Y = 0; Y < GridSize.Y; ++Y)
	{
		for (int32 X = 0; X < GridSize.X; ++X)
		{
			bool bNear = false;
			for (int32 DY = -1; DY <= 1 && !bNear; ++DY)
			{
				for (int32 DX = -1; DX <= 1 && !bNear; ++DX)
				{
					const int32 NX = X + DX;
					const int32 NY = Y + DY;
					bNear = NX >= 0 && NY >= 0 && NX < GridSize.X && NY < GridSize.Y && Inside[NY * GridSize.X + NX];
				}
			}
			Grown[Y * GridSize.X + X] = bNear;
		}
	}
	return Make(GridOrigin, Cell, GridSize, MoveTemp(Grown));
}

void FVoxelWaterLake::Finish()
{
	// A list of cells of the wrong length is no footprint.
	if (Cells.Num() != Size.X * Size.Y)
	{
		Cells.Init(false, Size.X * Size.Y);
	}
	CellCount = 0;
	FIntPoint MinCell(TNumericLimits<int32>::Max(), TNumericLimits<int32>::Max());
	FIntPoint MaxCell(TNumericLimits<int32>::Lowest(), TNumericLimits<int32>::Lowest());
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		for (int32 X = 0; X < Size.X; ++X)
		{
			if (Cells[Y * Size.X + X])
			{
				++CellCount;
				MinCell = FIntPoint(FMath::Min(MinCell.X, X), FMath::Min(MinCell.Y, Y));
				MaxCell = FIntPoint(FMath::Max(MaxCell.X, X), FMath::Max(MaxCell.Y, Y));
			}
		}
	}
	Bounds = FBox2D(ForceInit);
	CapAngle = 0.0;
	if (CellCount == 0)
	{
		return;
	}
	const FBox2D CellBox(Origin + FVector2D(MinCell.X * CellSize, MinCell.Y * CellSize),
		Origin + FVector2D((MaxCell.X + 1) * CellSize, (MaxCell.Y + 1) * CellSize));
	if (!bRadial)
	{
		Bounds = CellBox;
		return;
	}
	// A planet lake: the cap that holds the cells' box on the tangent plane (an offset d from the centre's datum point
	// is a direction asin(d / R) from the centre's).
	const double Farthest = FMath::Sqrt(FMath::Max(
		FMath::Max(CellBox.Min.SizeSquared(), CellBox.Max.SizeSquared()),
		FMath::Max(FVector2D(CellBox.Min.X, CellBox.Max.Y).SizeSquared(), FVector2D(CellBox.Max.X, CellBox.Min.Y).SizeSquared())));
	CapAngle = FMath::Asin(FMath::Min(1.0, Farthest / PlanetRadius));
}

bool FVoxelWaterLake::Covers(double X, double Y) const
{
	if (bRadial || CellCount == 0)
	{
		return false;
	}
	return HasCell(FMath::FloorToInt32((X - Origin.X) / CellSize), FMath::FloorToInt32((Y - Origin.Y) / CellSize));
}

bool FVoxelWaterLake::CoversDirection(const FVector& Direction) const
{
	if (!bRadial || CellCount == 0 || FVector::DotProduct(Direction, Centre) <= 0.0)
	{
		return false;
	}
	const double U = PlanetRadius * FVector::DotProduct(Direction, AxisX);
	const double V = PlanetRadius * FVector::DotProduct(Direction, AxisY);
	return HasCell(FMath::FloorToInt32((U - Origin.X) / CellSize), FMath::FloorToInt32((V - Origin.Y) / CellSize));
}

bool FVoxelWaterLake::GetCap(FVector& OutDirection, double& OutAngle) const
{
	if (!bRadial || CellCount == 0)
	{
		return false;
	}
	OutDirection = Centre;
	OutAngle = CapAngle;
	return true;
}

bool FVoxelWaterLake::MeetsRegion(const FBox2D& Region) const
{
	if (bRadial || CellCount == 0 || !Bounds.Intersect(Region))
	{
		return false;
	}
	if (Region.IsInside(Bounds.Min) && Region.IsInside(Bounds.Max))
	{
		return true;
	}
	const int32 X0 = FMath::Clamp(FMath::FloorToInt32((Region.Min.X - Origin.X) / CellSize), 0, Size.X - 1);
	const int32 X1 = FMath::Clamp(FMath::FloorToInt32((Region.Max.X - Origin.X) / CellSize), 0, Size.X - 1);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt32((Region.Min.Y - Origin.Y) / CellSize), 0, Size.Y - 1);
	const int32 Y1 = FMath::Clamp(FMath::FloorToInt32((Region.Max.Y - Origin.Y) / CellSize), 0, Size.Y - 1);
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		for (int32 X = X0; X <= X1; ++X)
		{
			if (Cells[Y * Size.X + X])
			{
				return true;
			}
		}
	}
	return false;
}

bool FVoxelWaterLake::MeetsCap(const FVector& CapDirection, double InCapAngle) const
{
	if (!bRadial || CellCount == 0)
	{
		return false;
	}
	return InCapAngle >= UE_DOUBLE_PI || FVoxelPolylineBounds::AngleBetween(CapDirection, Centre) <= InCapAngle + CapAngle;
}

FVector FVoxelWaterLake::GetCellCentre(int32 CellX, int32 CellY, double Level, const FVector& PlanetCenter) const
{
	const FVector2D P = Origin + FVector2D((CellX + 0.5) * CellSize, (CellY + 0.5) * CellSize);
	if (!bRadial)
	{
		return FVector(P.X, P.Y, Level);
	}
	// The direction whose datum point projects onto the tangent plane at P.
	const double U = FMath::Clamp(P.X / PlanetRadius, -1.0, 1.0);
	const double V = FMath::Clamp(P.Y / PlanetRadius, -1.0, 1.0);
	const double W = FMath::Sqrt(FMath::Max(0.0, 1.0 - U * U - V * V));
	return PlanetCenter + (AxisX * U + AxisY * V + Centre * W) * Level;
}

// ==================== A body ====================

FVoxelWaterBody FVoxelWaterBody::MakeLake(uint32 InId, double InLevel, const TSharedRef<const FVoxelWaterLake>& InLake, const FVoxelWaterFrame& Frame, double MaxDepth,
	float InMinCaveDepth)
{
	FVoxelWaterBody Body;
	Body.Id = InId;
	Body.Kind = EVoxelWaterBodyKind::Lake;
	Body.Level = InLevel;
	Body.Lake = InLake;
	Body.MinCaveDepth = FMath::Max(InMinCaveDepth, 0.0f);
	const double Depth = FMath::Max(MaxDepth, 0.0);
	FVector CapDirection;
	double CapAngle = 0.0;
	if (InLake->GetCap(CapDirection, CapAngle))
	{
		// A planet lake: a box around its cap at the level, down by the depth.
		Body.Bounds = FBox::BuildAABB(Frame.Center + CapDirection * InLevel, FVector(InLevel * FMath::Sin(CapAngle) + Depth));
	}
	else if (InLake->GetBounds2D().bIsValid)
	{
		const FBox2D& Box = InLake->GetBounds2D();
		Body.Bounds = FBox(FVector(Box.Min.X, Box.Min.Y, InLevel - Depth), FVector(Box.Max.X, Box.Max.Y, InLevel));
	}
	return Body;
}

FVoxelWaterBody FVoxelWaterBody::MakeReach(uint32 InId, const TSharedRef<const FVoxelWaterReach>& InReach, const FVoxelWaterFrame& Frame)
{
	FVoxelWaterBody Body;
	Body.Id = InId;
	Body.Kind = EVoxelWaterBodyKind::Reach;
	Body.Level = InReach->GetMaxLevel();
	Body.Reach = InReach;
	FBox Box(ForceInit);
	for (const FVoxelWaterReachVertex& Vertex : InReach->GetVertices())
	{
		if (InReach->IsRadial())
		{
			Box += Frame.Center + Vertex.Position * Vertex.SurfaceLevel;
			Box += Frame.Center + Vertex.Position * Vertex.BedLevel;
		}
		else
		{
			Box += FVector(Vertex.Position.X, Vertex.Position.Y, Vertex.SurfaceLevel);
			Box += FVector(Vertex.Position.X, Vertex.Position.Y, Vertex.BedLevel);
		}
	}
	if (Box.IsValid)
	{
		Body.Bounds = InReach->IsRadial() ? Box.ExpandBy(InReach->GetReach())
			: Box.ExpandBy(FVector(InReach->GetReach(), InReach->GetReach(), 0.0));
	}
	return Body;
}

bool FVoxelWaterBody::MeetsRegion(const FBox2D& Region) const
{
	switch (Kind)
	{
	case EVoxelWaterBodyKind::Sea:
		return true;
	case EVoxelWaterBodyKind::Lake:
		return Lake.IsValid() && Lake->MeetsRegion(Region);
	default:
		return Reach.IsValid() && Reach->GetPolyline().MeetsRegion(Region, Reach->GetReach());
	}
}

bool FVoxelWaterBody::MeetsCap(const FVector& CapDirection, double CapAngle, double PlanetRadius) const
{
	switch (Kind)
	{
	case EVoxelWaterBodyKind::Sea:
		return true;
	case EVoxelWaterBodyKind::Lake:
		return Lake.IsValid() && Lake->MeetsCap(CapDirection, CapAngle);
	default:
		return Reach.IsValid() && Reach->GetPolyline().MeetsCap(CapDirection, CapAngle, PlanetRadius, Reach->GetReach());
	}
}

// ==================== The snapshot ====================

FVoxelWaterLevels::FVoxelWaterLevels(const FVoxelSeaSurface& InSea)
	: Sea(InSea)
	, Bounds(WholeWorld())
{
	Frame.bRadial = Sea.bRadial;
	Frame.Center = Sea.Center;
	// A sea alone does not say where the datum is: its own radius stands in, and nothing reads it without a body.
	Frame.DatumRadius = Sea.Radius;
	if (Sea.bEnabled)
	{
		FVoxelWaterBody& Body = Bodies.AddDefaulted_GetRef();
		Body.Id = FVoxelWaterBody::SeaId;
		Body.Kind = EVoxelWaterBodyKind::Sea;
		Body.Level = Sea.bRadial ? Sea.Radius : Sea.PlaneZ;
		Body.Bounds = WholeWorld();
	}
}

FVoxelWaterLevels FVoxelWaterLevels::FromConfig(const UVoxelWorldConfiguration& Config)
{
	FVoxelWaterLevels Levels(FVoxelSeaSurface::FromConfig(Config));
	Levels.Frame = FVoxelWaterFrame::FromConfig(Config);
	return Levels;
}

FVoxelWaterLevels FVoxelWaterLevels::Make(const FVoxelSeaSurface& InSea, const FVoxelWaterFrame& InFrame, const FBox& InBounds, TArray<FVoxelWaterBody> InBodies)
{
	FVoxelWaterLevels Levels(InSea);
	Levels.Frame = InFrame;
	Levels.Bounds = InBounds;

	// The lakes and river reaches, by id, each once.
	InBodies.RemoveAll([](const FVoxelWaterBody& Body)
	{
		return Body.Kind == EVoxelWaterBodyKind::Sea
			|| (Body.Kind == EVoxelWaterBodyKind::Lake && !Body.Lake.IsValid())
			|| (Body.Kind == EVoxelWaterBodyKind::Reach && !Body.Reach.IsValid());
	});
	InBodies.StableSort([](const FVoxelWaterBody& A, const FVoxelWaterBody& B) { return A.Id < B.Id; });
	for (int32 Index = 0; Index < InBodies.Num(); ++Index)
	{
		if (Index > 0 && InBodies[Index].Id == InBodies[Index - 1].Id)
		{
			continue;
		}
		Levels.Bodies.Add(MoveTemp(InBodies[Index]));
		Levels.bSeaAlone = false;
	}
	return Levels;
}

FBox FVoxelWaterLevels::WholeWorld()
{
	return FBox(FVector(-UE_LARGE_WORLD_MAX), FVector(UE_LARGE_WORLD_MAX));
}

bool FVoxelWaterLevels::SampleSea(const FVector& WorldPos, FVoxelWaterSample& Out) const
{
	Out = FVoxelWaterSample();
	if (!Sea.bEnabled)
	{
		return false;
	}
	Out.bWater = true;
	Out.BodyId = FVoxelWaterBody::SeaId;
	Out.Kind = EVoxelWaterBodyKind::Sea;
	Out.Depth = Sea.GetDepth(WorldPos);
	if (Sea.bRadial)
	{
		Out.SurfaceLevel = Sea.Radius;
		Out.SurfaceNormal = (WorldPos - Sea.Center).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	}
	else
	{
		Out.SurfaceLevel = Sea.PlaneZ;
	}
	return true;
}

bool FVoxelWaterLevels::FindBodyAt(const FVector& WorldPos, FBodyAt& Out) const
{
	// The highest level over the column answers; the sea's stands until a lake or a river reach is strictly higher, and
	// of two bodies at one level the earlier (the lower id) answers.
	bool bFound = false;
	double Best = Sea.bEnabled ? (Sea.bRadial ? Sea.Radius : Sea.PlaneZ) : -TNumericLimits<double>::Max();
	const FVector Direction = Frame.bRadial ? Frame.UpAt(WorldPos) : FVector::UpVector;
	for (const FVoxelWaterBody& Body : Bodies)
	{
		if (Body.Kind == EVoxelWaterBodyKind::Lake)
		{
			if (Body.Level > Best && (Frame.bRadial ? Body.Lake->CoversDirection(Direction) : Body.Lake->Covers(WorldPos.X, WorldPos.Y)))
			{
				Best = Body.Level;
				Out.Body = &Body;
				Out.Level = Body.Level;
				bFound = true;
			}
		}
		else if (Body.Kind == EVoxelWaterBodyKind::Reach)
		{
			FVoxelWaterReachSample At;
			if (Body.Level > Best
				&& (Frame.bRadial ? Body.Reach->SampleOnSphere(Direction, Frame.DatumRadius, At) : Body.Reach->SampleAt(WorldPos.X, WorldPos.Y, At))
				&& At.SurfaceLevel > Best)
			{
				Best = At.SurfaceLevel;
				Out.Body = &Body;
				Out.Level = At.SurfaceLevel;
				Out.Reach = At;
				bFound = true;
			}
		}
	}
	return bFound;
}

bool FVoxelWaterLevels::SampleBodies(const FVector& WorldPos, FVoxelWaterSample& Out) const
{
	FBodyAt At;
	if (!FindBodyAt(WorldPos, At))
	{
		return SampleSea(WorldPos, Out);
	}
	Out = FVoxelWaterSample();
	Out.bWater = true;
	Out.BodyId = At.Body->Id;
	Out.Kind = At.Body->Kind;
	Out.SurfaceLevel = At.Level;
	Out.Depth = At.Level - Frame.HeightOf(WorldPos);
	Out.MinCaveDepth = At.Body->MinCaveDepth;
	const FVector Up = Frame.UpAt(WorldPos);
	Out.SurfaceNormal = Up;
	if (At.Body->Kind == EVoxelWaterBodyKind::Reach)
	{
		// Level across the river, sloped along it: the up tilted against the slope, the current along the surface.
		Out.SurfaceNormal = (Up - At.Reach.Downstream * At.Reach.Slope).GetSafeNormal(UE_SMALL_NUMBER, Up);
		Out.Flow = (At.Reach.Downstream + Up * At.Reach.Slope).GetSafeNormal() * At.Body->Reach->GetFlowSpeed();
	}
	return true;
}

double FVoxelWaterLevels::GetDepthBodies(const FVector& WorldPos) const
{
	FBodyAt At;
	if (FindBodyAt(WorldPos, At))
	{
		return At.Level - Frame.HeightOf(WorldPos);
	}
	return Sea.bEnabled ? Sea.GetDepth(WorldPos) : FVoxelWaterSample::NoWaterDepth;
}

bool FVoxelWaterLevels::IsUnderBodies(const FVector& WorldPos) const
{
	FBodyAt At;
	if (FindBodyAt(WorldPos, At))
	{
		return Frame.HeightOf(WorldPos) <= At.Level;
	}
	return Sea.bEnabled && Sea.IsBelow(WorldPos);
}

void FVoxelWaterLevels::GetBoxHeights(const FBox& Box, double& OutMin, double& OutMax) const
{
	if (!Frame.bRadial)
	{
		OutMin = Box.Min.Z;
		OutMax = Box.Max.Z;
		return;
	}
	const FVector& C = Frame.Center;
	const FVector Far(
		FMath::Abs(Box.Min.X - C.X) > FMath::Abs(Box.Max.X - C.X) ? Box.Min.X : Box.Max.X,
		FMath::Abs(Box.Min.Y - C.Y) > FMath::Abs(Box.Max.Y - C.Y) ? Box.Min.Y : Box.Max.Y,
		FMath::Abs(Box.Min.Z - C.Z) > FMath::Abs(Box.Max.Z - C.Z) ? Box.Min.Z : Box.Max.Z);
	OutMin = FMath::Sqrt(Box.ComputeSquaredDistanceToPoint(C));
	OutMax = FVector::Dist(Far, C);
}

bool FVoxelWaterLevels::IsAboveBodies(const FBox& Box) const
{
	if (Sea.bEnabled && !Sea.IsAbove(Box))
	{
		return false;
	}
	double BoxMin = 0.0, BoxMax = 0.0;
	GetBoxHeights(Box, BoxMin, BoxMax);
	const VoxelWaterLevelsPrivate::FColumns Columns = VoxelWaterLevelsPrivate::ColumnsOf(Box, Frame);
	for (const FVoxelWaterBody& Body : Bodies)
	{
		double Low = 0.0, High = 0.0;
		if (VoxelWaterLevelsPrivate::LevelsOver(Body, Columns, Low, High) && BoxMin <= High)
		{
			return false;
		}
	}
	return true;
}

bool FVoxelWaterLevels::CrossesBodies(const FBox& Box) const
{
	if (Sea.bEnabled && Sea.Crosses(Box))
	{
		return true;
	}
	double BoxMin = 0.0, BoxMax = 0.0;
	GetBoxHeights(Box, BoxMin, BoxMax);
	const VoxelWaterLevelsPrivate::FColumns Columns = VoxelWaterLevelsPrivate::ColumnsOf(Box, Frame);
	for (const FVoxelWaterBody& Body : Bodies)
	{
		double Low = 0.0, High = 0.0;
		if (VoxelWaterLevelsPrivate::LevelsOver(Body, Columns, Low, High) && BoxMin <= High && BoxMax > Low)
		{
			return true;
		}
	}
	return false;
}

FVoxelWaterLevelRange FVoxelWaterLevels::GetRange(const FBox& WorldBounds) const
{
	FVoxelWaterLevelRange Range;
	Range.Bounds = WorldBounds;
	if (Sea.bEnabled)
	{
		// The sea lies over or under every column, at one level.
		Range.bAny = true;
		Range.MinLevel = Range.MaxLevel = Sea.bRadial ? Sea.Radius : Sea.PlaneZ;
	}
	if (bSeaAlone)
	{
		return Range;
	}
	// A lake or a river reach that may reach the box: its levels join the range, and one scalar is no longer the truth
	// over every column (the body covers some and not others, or slopes).
	const VoxelWaterLevelsPrivate::FColumns Columns = VoxelWaterLevelsPrivate::ColumnsOf(WorldBounds, Frame);
	for (const FVoxelWaterBody& Body : Bodies)
	{
		double Low = 0.0, High = 0.0;
		if (!VoxelWaterLevelsPrivate::LevelsOver(Body, Columns, Low, High))
		{
			continue;
		}
		Range.MinLevel = Range.bAny ? FMath::Min(Range.MinLevel, Low) : Low;
		Range.MaxLevel = Range.bAny ? FMath::Max(Range.MaxLevel, High) : High;
		Range.bAny = true;
		Range.bUniform = false;
	}
	return Range;
}
