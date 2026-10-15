// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldBoundary.h"

int32 FVoxelWorldBoundary::GetCircleSegmentCount(double Radius, double MaxChordError)
{
	if (Radius <= 0.0)
	{
		return MinCircleSegments;
	}
	// An inscribed N-gon's chords run Radius * (1 - cos(pi / N)) inside the circle at their middles.
	const double Error = FMath::Clamp(MaxChordError, 1.0, Radius);
	const double HalfAngle = FMath::Acos(1.0 - Error / Radius);
	const int32 Count = HalfAngle > UE_SMALL_NUMBER ? FMath::CeilToInt32(UE_DOUBLE_PI / HalfAngle) : MaxCircleSegments;
	return FMath::Clamp(Count, MinCircleSegments, MaxCircleSegments);
}

void FVoxelWorldBoundary::BuildWalls(const FVoxelWorldBounds& Bounds, double Thickness, double MaxChordError, double BottomZ, double TopZ,
	TArray<FVoxelWorldBoundaryWall>& OutWalls)
{
	OutWalls.Reset();
	const double HalfThickness = FMath::Max(Thickness, 1.0) * 0.5;
	const double CentreZ = (BottomZ + TopZ) * 0.5;
	const double HalfHeight = FMath::Max(TopZ - BottomZ, 1.0) * 0.5;

	switch (Bounds.Shape)
	{
	case EVoxelWorldBoundsShape::Circle:
	{
		const double Radius = Bounds.OuterRadius;
		if (Radius <= 0.0)
		{
			return;
		}
		const int32 Count = GetCircleSegmentCount(Radius, MaxChordError);
		const double Step = UE_DOUBLE_TWO_PI / Count;
		// Chord i joins the polygon's vertices at angles i * Step and (i + 1) * Step on the circle; its middle sits
		// Radius * cos(Step / 2) from the centre. Each wall is a little longer than its chord so neighbours overlap at
		// the vertices (outside the circle) and leave no gap on the outer side.
		const double ChordDistance = Radius * FMath::Cos(Step * 0.5);
		const double HalfChord = Radius * FMath::Sin(Step * 0.5);
		OutWalls.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			const double Angle = (i + 0.5) * Step;
			const FVector2D Normal(FMath::Cos(Angle), FMath::Sin(Angle));
			FVoxelWorldBoundaryWall& Wall = OutWalls.AddDefaulted_GetRef();
			Wall.Center = FVector(Bounds.Center.X + Normal.X * (ChordDistance + HalfThickness),
				Bounds.Center.Y + Normal.Y * (ChordDistance + HalfThickness), CentreZ);
			Wall.Rotation = FRotator(0.0, FMath::RadiansToDegrees(Angle), 0.0);
			Wall.HalfExtent = FVector(HalfThickness, HalfChord + HalfThickness, HalfHeight);
		}
		return;
	}
	case EVoxelWorldBoundsShape::Rectangle:
	{
		const double HX = Bounds.HalfExtent.X;
		const double HY = Bounds.HalfExtent.Y;
		if (HX <= 0.0 || HY <= 0.0)
		{
			return;
		}
		// One wall per side, its inner face on the side, long enough to close the corners.
		auto Add = [&](double CX, double CY, double Yaw, double HalfLength)
		{
			FVoxelWorldBoundaryWall& Wall = OutWalls.AddDefaulted_GetRef();
			Wall.Center = FVector(Bounds.Center.X + CX, Bounds.Center.Y + CY, CentreZ);
			Wall.Rotation = FRotator(0.0, Yaw, 0.0);
			Wall.HalfExtent = FVector(HalfThickness, HalfLength + 2.0 * HalfThickness, HalfHeight);
		};
		Add(HX + HalfThickness, 0.0, 0.0, HY);
		Add(-(HX + HalfThickness), 0.0, 180.0, HY);
		Add(0.0, HY + HalfThickness, 90.0, HX);
		Add(0.0, -(HY + HalfThickness), -90.0, HX);
		return;
	}
	case EVoxelWorldBoundsShape::Shell:      // a planet: gravity holds the player; no walls
	case EVoxelWorldBoundsShape::Unbounded:
	default:
		return;
	}
}
