// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelPolyline.h"

void FVoxelPolylineBounds::Build(int32 NumVertices, bool bInRadial, TFunctionRef<const FVector&(int32)> PositionOf)
{
	bRadial = bInRadial;
	Segments = FMath::Max(NumVertices - 1, 0);
	Bounds = FBox2D(ForceInit);
	BlockBounds.Reset();
	CapCentre = FVector::UpVector;
	CapChordRadius = 0.0;
	BlockBalls.Reset();
	if (Segments == 0)
	{
		return;
	}
	const int32 Blocks = FMath::DivideAndRoundUp(Segments, BlockSegments);

	if (!bRadial)
	{
		BlockBounds.Reserve(Blocks);
		for (int32 Block = 0; Block < Blocks; ++Block)
		{
			// A block's segments [First, Last) read the vertices [First, Last].
			const int32 First = Block * BlockSegments;
			const int32 Last = FMath::Min(First + BlockSegments, Segments);
			FBox2D Box(ForceInit);
			for (int32 Index = First; Index <= Last; ++Index)
			{
				const FVector& Position = PositionOf(Index);
				Box += FVector2D(Position.X, Position.Y);
			}
			BlockBounds.Add(Box);
			Bounds += Box;
		}
		return;
	}

	// A ball around a run of unit directions: the normalized sum as the centre, the largest chord to a vertex as the radius.
	const auto BallOf = [&PositionOf](int32 FirstVertex, int32 LastVertex)
	{
		FVector Sum = FVector::ZeroVector;
		for (int32 Index = FirstVertex; Index <= LastVertex; ++Index)
		{
			Sum += PositionOf(Index);
		}
		const FVector Centre = Sum.IsNearlyZero() ? PositionOf(FirstVertex) : Sum.GetUnsafeNormal();
		double Radius = 0.0;
		for (int32 Index = FirstVertex; Index <= LastVertex; ++Index)
		{
			Radius = FMath::Max(Radius, (PositionOf(Index) - Centre).Size());
		}
		return FVector4d(Centre.X, Centre.Y, Centre.Z, Radius);
	};
	BlockBalls.Reserve(Blocks);
	for (int32 Block = 0; Block < Blocks; ++Block)
	{
		const int32 First = Block * BlockSegments;
		BlockBalls.Add(BallOf(First, FMath::Min(First + BlockSegments, Segments)));
	}
	const FVector4d Whole = BallOf(0, Segments);
	CapCentre = FVector(Whole.X, Whole.Y, Whole.Z);
	CapChordRadius = Whole.W;
}

FBox2D FVoxelPolylineBounds::GetBounds2D(double Reach) const
{
	if (bRadial || Segments == 0)
	{
		return FBox2D(ForceInit);
	}
	return Bounds.ExpandBy(Reach);
}

bool FVoxelPolylineBounds::GetCap(double PlanetRadius, double Reach, FVector& OutDirection, double& OutAngle) const
{
	if (!bRadial || Segments == 0)
	{
		return false;
	}
	OutDirection = CapCentre;
	OutAngle = 2.0 * FMath::Asin(FMath::Min(1.0, 0.5 * CapChordRadius)) + Reach / FMath::Max(PlanetRadius, 1.0);
	return true;
}

bool FVoxelPolylineBounds::MeetsRegion(const FBox2D& Region, double Reach) const
{
	if (bRadial || Segments == 0 || !Bounds.ExpandBy(Reach).Intersect(Region))
	{
		return false;
	}
	for (const FBox2D& Box : BlockBounds)
	{
		if (Box.ExpandBy(Reach).Intersect(Region))
		{
			return true;
		}
	}
	return false;
}

bool FVoxelPolylineBounds::MeetsCap(const FVector& CapDirection, double CapAngle, double PlanetRadius, double Reach) const
{
	if (!bRadial || Segments == 0)
	{
		return false;
	}
	if (CapAngle >= UE_DOUBLE_PI)
	{
		return true;
	}
	const double ReachAngle = Reach / FMath::Max(PlanetRadius, 1.0);
	const auto BallMeets = [&](const FVector& Centre, double ChordRadius)
	{
		return AngleBetween(CapDirection, Centre) <= CapAngle + 2.0 * FMath::Asin(FMath::Min(1.0, 0.5 * ChordRadius)) + ReachAngle;
	};
	if (!BallMeets(CapCentre, CapChordRadius))
	{
		return false;
	}
	for (const FVector4d& Ball : BlockBalls)
	{
		if (BallMeets(FVector(Ball.X, Ball.Y, Ball.Z), Ball.W))
		{
			return true;
		}
	}
	return false;
}
