// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVoxelWorldConfiguration;

/**
 * The sea's surface as the voxel water rules see it: the Z plane at WaterLevel in heightmap modes, the sphere of
 * WaterRadius around the planet centre on a planet (spherical requirement SP-B8). Generation's water fill, the chunk
 * manager's cross-chunk propagation and the edit-time propagation (UVoxelWaterPropagation) all test "below the sea"
 * with it, so the three agree in every world mode.
 *
 * Plain data, cheap to copy; safe on any thread.
 */
struct VOXELCORE_API FVoxelSeaSurface
{
	/** Water is on (bEnableWaterLevel) */
	bool bEnabled = false;

	/** The surface is a sphere (a planet) rather than a Z plane */
	bool bRadial = false;

	/** Sphere: the planet centre */
	FVector Center = FVector::ZeroVector;

	/** Sphere: the sea radius */
	double Radius = 0.0;

	/** Plane: the water level (world Z) */
	double PlaneZ = 0.0;

	/** A Z plane at Z. */
	static FVoxelSeaSurface MakePlane(double Z);

	/** A sphere of Radius around Center. */
	static FVoxelSeaSurface MakeSphere(const FVector& InCenter, double InRadius);

	/**
	 * The sea of a world configuration: WaterLevel's plane, or on a planet the sphere of WaterRadius around WorldOrigin
	 * (the planet centre, FVoxelWorldModeFactory::MakePlanetParams). bEnabled follows bEnableWaterLevel. The one sea
	 * every consumer reads: the plane is world Z, as the terrain's heights are (WorldOrigin moves the chunk grid, not the
	 * ground), so it is where generation floods and the water tiles draw, wherever WorldOrigin is (review plan G19).
	 *
	 * @param Config World configuration
	 * @return The sea surface
	 */
	static FVoxelSeaSurface FromConfig(const UVoxelWorldConfiguration& Config);

	/**
	 * Signed depth of a world position below the surface: PlaneZ - Z on the plane, Radius - |WorldPos - Center| on a
	 * sphere (spherical requirement SP-E1). Positive below the sea, negative above, 0 on it; the sign alone answers
	 * "strictly below" exactly as the old Z < WaterLevel tests did.
	 *
	 * @param WorldPos World position
	 * @return Depth below the surface (world units)
	 */
	double GetDepth(const FVector& WorldPos) const
	{
		return bRadial
			? Radius - FVector::Dist(WorldPos, Center)
			: PlaneZ - WorldPos.Z;
	}

	/**
	 * Whether a world position is at or below the surface: Z <= PlaneZ, or within Radius of Center. (The water rules
	 * give such air the water flag when the open sea reaches it.)
	 *
	 * @param WorldPos World position
	 * @return True at or below the surface
	 */
	bool IsBelow(const FVector& WorldPos) const
	{
		return bRadial
			? FVector::DistSquared(WorldPos, Center) <= Radius * Radius
			: WorldPos.Z <= PlaneZ;
	}

	/**
	 * Whether every point of a box is above the surface, so nothing in it can take water.
	 *
	 * @param Box World-space box
	 * @return True when the box lies wholly above the sea
	 */
	bool IsAbove(const FBox& Box) const;

	/**
	 * Whether the surface passes through a box: some of it at or below the sea, some above.
	 *
	 * @param Box World-space box
	 * @return True when the box straddles the surface
	 */
	bool Crosses(const FBox& Box) const;
};
