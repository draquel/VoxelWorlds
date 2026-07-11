// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVoxelCaveConfiguration;

/**
 * Stateless, thread-safe cave queries against the procedural generator.
 *
 * The cave-layer sibling of FVoxelSurfaceQuery: answers "how carved is world position P" by
 * re-running the CPU generator's cave-carving math (FVoxelCPUNoiseGenerator::CalculateCaveDensity),
 * decoupled from chunk streaming so it works for any region whether or not a chunk is loaded.
 *
 * Deterministic for a given (cave config, seed). Safe to call off the game thread. Used by the
 * editor field-visualization tools (cave-presence heatmap) and available to gameplay/AI that
 * needs "is there a cave here" without a resident chunk.
 *
 * @see FVoxelSurfaceQuery
 * @see FVoxelCPUNoiseGenerator::CalculateCaveDensity
 */
class VOXELGENERATION_API FVoxelCaveQuery
{
public:
	/**
	 * Cave carve density [0,1] at an explicit world position. 0 = solid (no carving),
	 * 1 = fully carved (open cave).
	 *
	 * Mirrors generation-time carving exactly (FVoxelCPUNoiseGenerator::GenerateChunkInfinitePlane):
	 * depth below the surface is measured in voxels, and only positions strictly BELOW the terrain
	 * surface carve — at or above the surface this returns 0 (there is no solid terrain to carve).
	 *
	 * @param WorldPos      World position to sample.
	 * @param SurfaceHeight Terrain surface Z at WorldPos.XY (e.g. FVoxelSurfaceQuery::GetSurfaceHeight).
	 * @param VoxelSize     World units per voxel (depth below surface is measured in voxels).
	 * @param BiomeID       Surface biome ID for per-biome cave overrides; 0 if unknown.
	 * @param CaveConfig    Cave configuration; null => returns 0.
	 * @param WorldSeed     World seed.
	 * @param bIsUnderwater Whether the surface column is submerged (suppresses caves per config).
	 * @return Carve density in [0,1].
	 */
	static float SampleCaveDensityAt(
		const FVector& WorldPos,
		float SurfaceHeight,
		float VoxelSize,
		uint8 BiomeID,
		const UVoxelCaveConfiguration* CaveConfig,
		int32 WorldSeed,
		bool bIsUnderwater = false);
};
