// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

FVoxelWaterLevels::FVoxelWaterLevels(const FVoxelSeaSurface& InSea)
	: Sea(InSea)
	, Bounds(WholeWorld())
{
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
	return FVoxelWaterLevels(FVoxelSeaSurface::FromConfig(Config));
}

FBox FVoxelWaterLevels::WholeWorld()
{
	return FBox(FVector(-UE_LARGE_WORLD_MAX), FVector(UE_LARGE_WORLD_MAX));
}

bool FVoxelWaterLevels::Sample(const FVector& WorldPos, FVoxelWaterSample& Out) const
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
	return Range;
}
