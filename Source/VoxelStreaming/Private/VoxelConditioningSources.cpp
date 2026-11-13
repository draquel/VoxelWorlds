// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelConditioningSources.h"

void FVoxelConditioningSources::GatherOnSphere(const FVector& Direction, double AngularRadius, double PlanetRadius,
	EVoxelConditioningGather Need, FVoxelConditioning& Out) const
{
	if (IsEmpty())
	{
		return;
	}
	for (const FVoxelConditioningZone& Zone : Zones)
	{
		if (Zone.MeetsCap(Direction, AngularRadius, PlanetRadius))
		{
			Out.Zones.Add(Zone);
		}
	}
	if (Conditioner)
	{
		Conditioner->GatherConditioningOnSphere(Direction, AngularRadius, Out.Zones);
		Conditioner->GatherConditioningLinesOnSphere(Direction, AngularRadius, Need, Out.Lines);
	}
}

void FVoxelConditioningSources::GatherForRegion(const FBox2D& Region, EVoxelConditioningGather Need, FVoxelConditioning& Out) const
{
	if (IsEmpty())
	{
		return;
	}

	// Static zones whose influence overlaps the region (heightmap zones: planet zones apply only on a planet).
	for (const FVoxelConditioningZone& Zone : Zones)
	{
		if (!Zone.IsRadial() && Zone.GetBounds2D().Intersect(Region))
		{
			Out.Zones.Add(Zone);
		}
	}

	// Dynamic (game-supplied) zones and lines for this region.
	if (Conditioner)
	{
		Conditioner->GatherConditioning(Region, Out.Zones);
		Conditioner->GatherConditioningLines(Region, Need, Out.Lines);
	}
}
