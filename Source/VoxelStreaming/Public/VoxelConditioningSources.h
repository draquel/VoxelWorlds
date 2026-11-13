// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTerrainConditioning.h"

/**
 * Where a chunk manager's terrain conditioning comes from (gen-time flattening under POIs and claims, levelling along
 * lines): the zones and the lines added to it (AddConditioningZone, AddConditioningLine) and the game's dynamic
 * conditioner (SetTerrainConditioner, not owned), which supplies zones and lines. The manager picks the cap or the XY region a box or a chunk takes from the world mode and asks these sources
 * for the conditioning there (FVoxelConditioning), which generation and the surface queries apply. Game thread.
 */
class VOXELSTREAMING_API FVoxelConditioningSources
{
public:
	/** Add a static zone. */
	void AddZone(const FVoxelConditioningZone& Zone) { Zones.Add(Zone); }

	/** Set (or clear, with null) the game's dynamic conditioner. Not owned: the caller keeps it alive. */
	void SetConditioner(IVoxelTerrainConditioner* InConditioner) { Conditioner = InConditioner; }

	/** Add a static line (shared: gathered by reference). */
	void AddLine(const FVoxelConditioningLineRef& Line) { Lines.Add(Line); }

	/** @return Whether there is nothing to gather: no static zone, no static line and no conditioner */
	bool IsEmpty() const { return Zones.Num() == 0 && Lines.Num() == 0 && Conditioner == nullptr; }

	/** @return How many static zones there are */
	int32 NumZones() const { return Zones.Num(); }

	/** @return Whether the game's conditioner is set */
	bool HasConditioner() const { return Conditioner != nullptr; }

	/** @return How many static lines there are */
	int32 NumLines() const { return Lines.Num(); }

	/** @return The static lines, in the order they were added */
	const TArray<FVoxelConditioningLineRef>& GetLines() const { return Lines; }

	/**
	 * Append the conditioning a cap of a planet meets: the static planet zones (FVoxelConditioningZone::MeetsCap) and
	 * lines (FVoxelConditioningLine::MeetsCap), then the conditioner's zones and lines; the lines end in the order they
	 * are applied (FVoxelConditioning::SortLines).
	 * @param Direction      The cap's centre direction from the planet centre
	 * @param AngularRadius  The cap's angular radius (radians)
	 * @param PlanetRadius   The planet's datum radius
	 * @param Need           What the gather is for: every line (Complete) or the lines at hand (Ready); zones are always complete
	 * @param Out            Appended to
	 */
	void GatherOnSphere(const FVector& Direction, double AngularRadius, double PlanetRadius, EVoxelConditioningGather Need,
		FVoxelConditioning& Out) const;

	/**
	 * Append the conditioning whose influence overlaps a world XY region: the static heightmap zones and lines
	 * (FVoxelConditioningLine::MeetsRegion), then the conditioner's zones and lines; the lines end in the order they are
	 * applied (FVoxelConditioning::SortLines).
	 * @param Region  The region
	 * @param Need    What the gather is for: every line (Complete) or the lines at hand (Ready); zones are always complete
	 * @param Out     Appended to
	 */
	void GatherForRegion(const FBox2D& Region, EVoxelConditioningGather Need, FVoxelConditioning& Out) const;

private:
	/** Static terrain conditioning zones */
	TArray<FVoxelConditioningZone> Zones;

	/** Static terrain conditioning lines */
	TArray<FVoxelConditioningLineRef> Lines;

	/** Optional game-supplied dynamic conditioner queried per chunk (not owned) */
	IVoxelTerrainConditioner* Conditioner = nullptr;
};
