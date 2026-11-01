// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTerrainConditioning.h"

/**
 * The terrain conditioning zones a chunk manager's generation and surface queries apply (gen-time flattening under POIs and
 * claims): the zones added to it (AddConditioningZone) and the game's dynamic conditioner (SetTerrainConditioner, not
 * owned). The manager picks the cap or the XY region a box or a chunk takes from the world mode and asks this set for the
 * zones there. Game thread.
 */
class VOXELSTREAMING_API FVoxelConditioningZoneSet
{
public:
	/** Add a static zone. */
	void Add(const FVoxelConditioningZone& Zone) { Zones.Add(Zone); }

	/** Set (or clear, with null) the game's dynamic conditioner. Not owned: the caller keeps it alive. */
	void SetConditioner(IVoxelTerrainConditioner* InConditioner) { Conditioner = InConditioner; }

	/** @return Whether there is nothing to gather: no static zone and no conditioner */
	bool IsEmpty() const { return Zones.Num() == 0 && Conditioner == nullptr; }

	/** @return How many static zones there are */
	int32 Num() const { return Zones.Num(); }

	/**
	 * Append the zones a cap of a planet meets: the static planet zones (FVoxelConditioningZone::MeetsCap) and the
	 * conditioner's.
	 * @param Direction      The cap's centre direction from the planet centre
	 * @param AngularRadius  The cap's angular radius (radians)
	 * @param PlanetRadius   The planet's datum radius
	 * @param OutZones       Appended to
	 */
	void GatherOnSphere(const FVector& Direction, double AngularRadius, double PlanetRadius, TArray<FVoxelConditioningZone>& OutZones) const;

	/**
	 * Append the zones whose influence overlaps a world XY region: the static heightmap zones and the conditioner's.
	 * @param Region    The region
	 * @param OutZones  Appended to
	 */
	void GatherForRegion(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const;

private:
	/** Static terrain conditioning zones */
	TArray<FVoxelConditioningZone> Zones;

	/** Optional game-supplied dynamic conditioner queried per chunk (not owned) */
	IVoxelTerrainConditioner* Conditioner = nullptr;
};
