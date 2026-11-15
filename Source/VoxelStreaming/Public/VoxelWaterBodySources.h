// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelWaterLevels.h"

/**
 * Where a chunk manager's lakes and river reaches come from, the twin of FVoxelConditioningSources (the hydrology
 * plan's section 5.2): the static bodies added to it (UVoxelChunkManager::AddWaterBody: a hand-placed river or lake)
 * and the sources registered on it (AddWaterBodySource, not owned: the hydrology generator's network). The manager
 * picks the XY region or the cap a box, a chunk or a point takes from the world mode and asks these sources for the
 * bodies there, which it hands out as a snapshot of the water levels (FVoxelWaterLevels). Game thread.
 */
class VOXELSTREAMING_API FVoxelWaterBodySources
{
public:
	/** Add a static body (a lake or a river reach; a sea record is ignored). */
	void AddBody(const FVoxelWaterBody& Body);

	/** Register a source. Not owned: the caller keeps it alive until RemoveSource. */
	void AddSource(IVoxelWaterBodySource* Source);

	/** Take a source out. */
	void RemoveSource(IVoxelWaterBodySource* Source) { Sources.Remove(Source); }

	/** @return Whether there is nothing to gather: no static body and no source (the sea alone) */
	bool IsEmpty() const { return Bodies.Num() == 0 && Sources.Num() == 0; }

	/** @return The static bodies, in the order they were added */
	const TArray<FVoxelWaterBody>& GetBodies() const { return Bodies; }

	/** @return How many sources are registered */
	int32 NumSources() const { return Sources.Num(); }

	/**
	 * Append the bodies whose footprints may reach a world XY region of a heightmap world: the static ones, then each
	 * source's.
	 * @param Region The region (a point region is a point query)
	 * @param Need   What the gather is for: every body (Complete) or the bodies at hand (Ready); static bodies are always at hand
	 * @param Out    Appended to
	 */
	void GatherForRegion(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& Out) const;

	/**
	 * Append the bodies whose footprints may reach a cap of a planet, as GatherForRegion.
	 * @param Direction      The cap's centre direction from the planet centre
	 * @param AngularRadius  The cap's half-angle (radians; pi or more is the whole sphere)
	 * @param PlanetRadius   The planet's datum radius
	 * @param Need           Complete or Ready
	 * @param Out            Appended to
	 */
	void GatherOnSphere(const FVector& Direction, double AngularRadius, double PlanetRadius, EVoxelConditioningGather Need,
		TArray<FVoxelWaterBody>& Out) const;

private:
	/** Static bodies: lakes and river reaches */
	TArray<FVoxelWaterBody> Bodies;

	/** Registered sources (not owned) */
	TArray<IVoxelWaterBodySource*> Sources;
};
