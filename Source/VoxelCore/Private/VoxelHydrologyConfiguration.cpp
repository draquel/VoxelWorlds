// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelHydrologyConfiguration.h"
#include "Misc/Crc.h"

FString FVoxelHydrologySettings::DescribeGeneration() const
{
	// Every value a body depends on, in a fixed order and a fixed format: two worlds whose lines are equal have the
	// same bodies. TileCells and the two view distances cut up the work and are left out.
	return FString::Printf(
		TEXT("cell=%d;spring=%d,%.3f;river=%.3f,%.3f,%.5f,%d;bed=%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.4f,%.3f,%d,%.3f;lake=%.4f,%.3f,%.4f,%.3f,%d,%d,%.3f"),
		CellVoxels, SpringSpacingCells, MinSpringHeight,
		MinRiverLength, MaxRiverLength, MaxMeanGrade, MaxLakesPerRiver,
		HalfWidthVoxels, HalfWidthGrowth, MaxHalfWidthVoxels, BankWidthVoxels, DepthVoxels, SurfaceBelowGroundVoxels, BankMarginVoxels,
		HeadLength, SmoothingDistance, MaxCutVoxels, FlowSpeed, BedMaterialID, BedMinCaveDepth,
		MinLakeDepthVoxels, MinLakeArea, LakeLevelMarginVoxels, MaxLakeDepth, MaxLakeCells, MaxLakeSpanCells, LakeMinCaveDepth);
}

uint32 FVoxelHydrologySettings::GetGenerationHash() const
{
	return FCrc::StrCrc32(*DescribeGeneration());
}
