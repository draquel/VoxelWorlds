// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelValleyConfiguration.h"
#include "Misc/Crc.h"

FString FVoxelValleySettings::DescribeGeneration() const
{
	// Every value a valley depends on, in a fixed order and a fixed format: two worlds whose lines are equal have the
	// same valleys. TileCells cuts up the work and is left out.
	return FString::Printf(
		TEXT("lattice=%.3f,%.4f,%.3f;breach=%d;drainage=%d,%d,%d;shape=%.4f,%.3f,%.3f,%.3f,%.3f,%.3f;river=%.3f,%.3f,%.4f,%d"),
		CellSize, Jitter, MinLandHeight,
		BreachCells,
		DrainageReach, ValleyDrainage, FullWidthDrainage,
		FloorFraction, MinFloorHeight, FloorHalfWidthMin, FloorHalfWidthMax, ValleyHalfWidthMin, ValleyHalfWidthMax,
		RiverHalfWidthMin, RiverHalfWidthMax, RiverWinding, RiverHalfWaves);
}

uint32 FVoxelValleySettings::GetGenerationHash() const
{
	return FCrc::StrCrc32(*DescribeGeneration());
}
