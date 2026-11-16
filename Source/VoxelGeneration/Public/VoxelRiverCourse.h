// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelWaterLevels.h"

/**
 * What a river reads along its course before its profile is made (FVoxelConditioningLineShape::MakeRiverProfile;
 * Documentation/HYDROLOGY_PLAN.md, section 5.3): the level of the body a place lies in, and the lowest ground of the
 * two banks at each vertex. The hand-placed river (UVoxelRiverComponent) and the hydrology generator (FVoxelHydrology)
 * read the same way, so a generated river keeps the rules a drawn one keeps. Pure and thread-safe: the ground is a
 * function the caller gives.
 *
 * A place is a column: world (X, Y) on a heightmap world (Z is not read), the unit direction from the planet's centre
 * on a planet. A height is a line's: a world Z, or an altitude over the datum on a planet.
 */
struct VOXELGENERATION_API FVoxelRiverCourse
{
	/**
	 * The level of the body a place lies in, in a line's terms: the sea or a lake where the ground lies under its
	 * level, a river reach wherever its footprint covers the place (its bed will be there); the highest, where several
	 * do. Every body is asked, not the one the world's sample answers with: under a river that has come down to the
	 * sea's level the sample is the sea's.
	 * @param Water The water levels gathered over the river
	 * @param Place The column
	 * @param GroundHeight The natural ground's height there
	 * @return The level, or NaN where the place lies in no body
	 */
	static double GetBodyLevelAt(const FVoxelWaterLevels& Water, const FVector& Place, double GroundHeight);

	/**
	 * The lowest natural ground of each vertex's two banks: from the floor's edge out to the footprint's, a voxel
	 * apart, on both sides of the line. Where a bank lies in another body (the shore of the sea beside a mouth, a
	 * lake's, a river's) that body's surface, and the margin over it, is the bank: the river may stand as high as the
	 * water beside it.
	 * @param Points The line's vertices where distances are measured: world X and Y, or the points on the datum sphere
	 * @param Ups A planet: the unit direction at each vertex (the vertex's own position). Empty on a heightmap world
	 * @param Floors Half the floor's width at each vertex (the bed's half width times the taper there)
	 * @param BankWidth The bank's width, past the floor's edge
	 * @param VoxelSize The world's voxel size (the banks are read a voxel apart)
	 * @param BankMargin How far over another body's level its bank counts as standing
	 * @param Ground The natural ground's height at a place
	 * @param BodyLevel The level of the body a place lies in given its ground, or NaN (GetBodyLevelAt)
	 * @param OutBankGround Receives the lowest bank ground per vertex (emptied first)
	 */
	static void ReadBanks(TConstArrayView<FVector> Points, TConstArrayView<FVector> Ups, TConstArrayView<double> Floors, double BankWidth, double VoxelSize,
		double BankMargin, TFunctionRef<double(const FVector& Place)> Ground, TFunctionRef<double(const FVector& Place, double GroundHeight)> BodyLevel,
		TArray<double>& OutBankGround);
};
