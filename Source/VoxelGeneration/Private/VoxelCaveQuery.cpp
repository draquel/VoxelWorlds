// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCaveQuery.h"
#include "VoxelCPUNoiseGenerator.h"

float FVoxelCaveQuery::SampleCaveDensityAt(
	const FVector& WorldPos,
	float SurfaceHeight,
	float VoxelSize,
	uint8 BiomeID,
	const UVoxelCaveConfiguration* CaveConfig,
	int32 WorldSeed,
	bool bIsUnderwater)
{
	if (!CaveConfig || VoxelSize <= 0.0f)
	{
		return 0.0f;
	}

	// Depth below the terrain surface, in voxels — identical to what the CPU generator computes
	// per voxel: DepthBelowSurface = (TerrainHeight - WorldPos.Z) / VoxelSize. Caves only carve
	// solid terrain below the surface; at or above the surface there is nothing to carve.
	const float DepthBelowSurface = (SurfaceHeight - static_cast<float>(WorldPos.Z)) / VoxelSize;
	if (DepthBelowSurface <= 0.0f)
	{
		return 0.0f;
	}

	return FVoxelCPUNoiseGenerator::CalculateCaveDensity(
		WorldPos, DepthBelowSurface, BiomeID, CaveConfig, WorldSeed, bIsUnderwater);
}
