// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A digital line through the voxel grid along a direction: each step moves one voxel along the axis nearest the
 * direction (AxisA) and the sideways offset on the other two axes is the rounded multiple of the direction's slope, so
 * consecutive voxels touch and the line follows the direction to within half a voxel. With the direction +Z the line is
 * a voxel column. The same geometry as the planet post-passes' FVoxelDownLines (VoxelGeneration), for consumers that
 * walk the surface along local up from one voxel: the edit-merged surface query and scatter extraction on a planet
 * (spherical Phase 4, WORLD_MODES_REVIEW_PLAN.md §18).
 *
 * Plain data; build once per query or per chunk on any thread.
 */
struct FVoxelDigitalLine
{
	/** The axis nearest the direction (0 = X, 1 = Y, 2 = Z); the line steps along it */
	int32 AxisA = 2;

	/** +1 when the direction points along +AxisA, else -1 */
	int32 SignA = 1;

	/** Sideways offset per step on AxisB = (AxisA + 1) % 3 and AxisC = (AxisA + 2) % 3 */
	double SlopeB = 0.0;
	double SlopeC = 0.0;

	/**
	 * The line geometry along a direction.
	 *
	 * @param Direction Unit direction (need not be normalised; only its ratios matter)
	 * @return The line
	 */
	static FVoxelDigitalLine Make(const FVector& Direction)
	{
		FVoxelDigitalLine Line;
		const FVector Abs = Direction.GetAbs();
		Line.AxisA = (Abs.X >= Abs.Y && Abs.X >= Abs.Z) ? 0 : (Abs.Y >= Abs.Z ? 1 : 2);
		Line.SignA = Direction[Line.AxisA] >= 0.0 ? 1 : -1;
		const double Major = FMath::Max(Abs[Line.AxisA], UE_DOUBLE_SMALL_NUMBER);
		Line.SlopeB = Direction[(Line.AxisA + 1) % 3] / Major;
		Line.SlopeC = Direction[(Line.AxisA + 2) % 3] / Major;
		return Line;
	}

	/**
	 * The voxel K steps from Start along the line (negative K steps backwards).
	 *
	 * @param Start Voxel index the line passes through
	 * @param K Step index
	 * @return Voxel index
	 */
	FIntVector Step(const FIntVector& Start, int32 K) const
	{
		FIntVector Result = Start;
		Result[AxisA] += SignA * K;
		Result[(AxisA + 1) % 3] += FMath::RoundToInt32(K * SlopeB);
		Result[(AxisA + 2) % 3] += FMath::RoundToInt32(K * SlopeC);
		return Result;
	}
};
