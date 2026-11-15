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
