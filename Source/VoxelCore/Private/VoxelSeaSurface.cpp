// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSeaSurface.h"
#include "VoxelWorldConfiguration.h"

FVoxelSeaSurface FVoxelSeaSurface::MakePlane(double Z)
{
	FVoxelSeaSurface Sea;
	Sea.bEnabled = true;
	Sea.PlaneZ = Z;
	return Sea;
}

FVoxelSeaSurface FVoxelSeaSurface::MakeSphere(const FVector& InCenter, double InRadius)
{
	FVoxelSeaSurface Sea;
	Sea.bEnabled = true;
	Sea.bRadial = true;
	Sea.Center = InCenter;
	Sea.Radius = InRadius;
	return Sea;
}

FVoxelSeaSurface FVoxelSeaSurface::FromConfig(const UVoxelWorldConfiguration& Config)
{
	FVoxelSeaSurface Sea = (Config.WorldMode == EWorldMode::SphericalPlanet)
		? MakeSphere(Config.WorldOrigin, Config.WaterRadius)
		: MakePlane(Config.WaterLevel);
	Sea.bEnabled = Config.bEnableWaterLevel;
	return Sea;
}

bool FVoxelSeaSurface::IsAbove(const FBox& Box) const
{
	if (!bRadial)
	{
		return Box.Min.Z > PlaneZ;
	}
	// The box's nearest point to the centre is outside the sphere.
	return Box.ComputeSquaredDistanceToPoint(Center) > Radius * Radius;
}

bool FVoxelSeaSurface::Crosses(const FBox& Box) const
{
	if (!bRadial)
	{
		return Box.Min.Z <= PlaneZ && Box.Max.Z > PlaneZ;
	}
	// Some point inside the sphere (the nearest one) and some outside (the farthest corner).
	const double R2 = Radius * Radius;
	const FVector Far(
		FMath::Abs(Box.Min.X - Center.X) > FMath::Abs(Box.Max.X - Center.X) ? Box.Min.X : Box.Max.X,
		FMath::Abs(Box.Min.Y - Center.Y) > FMath::Abs(Box.Max.Y - Center.Y) ? Box.Min.Y : Box.Max.Y,
		FMath::Abs(Box.Min.Z - Center.Z) > FMath::Abs(Box.Max.Z - Center.Z) ? Box.Min.Z : Box.Max.Z);
	return Box.ComputeSquaredDistanceToPoint(Center) <= R2 && FVector::DistSquared(Far, Center) > R2;
}
