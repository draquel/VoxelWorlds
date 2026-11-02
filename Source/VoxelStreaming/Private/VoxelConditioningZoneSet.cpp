// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelConditioningZoneSet.h"

void FVoxelConditioningZoneSet::GatherOnSphere(const FVector& Direction, double AngularRadius, double PlanetRadius,
	TArray<FVoxelConditioningZone>& OutZones) const
{
	if (IsEmpty())
	{
		return;
	}
	for (const FVoxelConditioningZone& Zone : Zones)
	{
		if (Zone.MeetsCap(Direction, AngularRadius, PlanetRadius))
		{
			OutZones.Add(Zone);
		}
	}
	if (Conditioner)
	{
		Conditioner->GatherConditioningOnSphere(Direction, AngularRadius, OutZones);
	}
}

void FVoxelConditioningZoneSet::GatherForRegion(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const
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
			OutZones.Add(Zone);
		}
	}

	// Dynamic (game-supplied) zones for this region.
	if (Conditioner)
	{
		Conditioner->GatherConditioning(Region, OutZones);
	}
}
