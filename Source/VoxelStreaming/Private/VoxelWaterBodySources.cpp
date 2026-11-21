// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterBodySources.h"

void FVoxelWaterBodySources::AddBody(const FVoxelWaterBody& Body)
{
	if (Body.Kind != EVoxelWaterBodyKind::Sea)
	{
		Bodies.Add(Body);
	}
}

void FVoxelWaterBodySources::AddSource(IVoxelWaterBodySource* Source)
{
	if (Source)
	{
		Sources.AddUnique(Source);
	}
}

void FVoxelWaterBodySources::GatherForRegion(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& Out) const
{
	for (const FVoxelWaterBody& Body : Bodies)
	{
		if (Body.MeetsRegion(Region))
		{
			Out.Add(Body);
		}
	}
	for (const IVoxelWaterBodySource* Source : Sources)
	{
		Source->GatherWaterBodies(Region, Need, Out);
	}
}

void FVoxelWaterBodySources::GatherOnSphere(const FVector& Direction, double AngularRadius, double PlanetRadius, EVoxelConditioningGather Need,
	TArray<FVoxelWaterBody>& Out) const
{
	for (const FVoxelWaterBody& Body : Bodies)
	{
		if (Body.MeetsCap(Direction, AngularRadius, PlanetRadius))
		{
			Out.Add(Body);
		}
	}
	for (const IVoxelWaterBodySource* Source : Sources)
	{
		Source->GatherWaterBodiesOnSphere(Direction, AngularRadius, Need, Out);
	}
}

bool FVoxelWaterBodySources::GetSourcesLevelRangeAtHandOnSphere(const FVector& Direction, double AngularRadius, bool& bOutAny, double& OutMinLevel, double& OutMaxLevel) const
{
	bOutAny = false;
	OutMinLevel = 0.0;
	OutMaxLevel = 0.0;
	if (Sources.Num() == 0)
	{
		return false;
	}
	for (const IVoxelWaterBodySource* Source : Sources)
	{
		bool bAny = false;
		double MinLevel = 0.0, MaxLevel = 0.0;
		if (!Source->GetLevelRangeAtHandOnSphere(Direction, AngularRadius, bAny, MinLevel, MaxLevel))
		{
			return false;
		}
		if (bAny)
		{
			OutMinLevel = bOutAny ? FMath::Min(OutMinLevel, MinLevel) : MinLevel;
			OutMaxLevel = bOutAny ? FMath::Max(OutMaxLevel, MaxLevel) : MaxLevel;
			bOutAny = true;
		}
	}
	return true;
}
