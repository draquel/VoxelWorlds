// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelNoiseTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The classic table noise (Ken Perlin's 256-entry permutation, the 0.6 Simplex kernel): what heightmap worlds sampled
 * before the world modes review's MP4 (decision D39; GenerationVersion 3). No generation path reads it; the tests keep it
 * as the control that repeats every 256 lattice units and steps across the simplex boundaries (Noise.PlanetHashNoPeriod,
 * Noise.HeightmapNoise). Moved out of FVoxelCPUNoiseGenerator by the code review's CR1 (CORE-02): test-only, not exported.
 */
namespace VoxelClassicNoiseControl
{
	/**
	 * Classic 3D Perlin noise (the permutation table; a 256-unit period).
	 *
	 * @param Position World position to sample
	 * @param Seed Random seed for permutation table
	 * @return Noise value in range [-1, 1]
	 */
	float Perlin3D(const FVector& Position, int32 Seed = 0);

	/**
	 * Classic 3D Simplex noise (the permutation table, the 0.6 corner kernel).
	 *
	 * @param Position World position to sample
	 * @param Seed Random seed for permutation table
	 * @return Noise value in range [-1, 1]
	 */
	float Simplex3D(const FVector& Position, int32 Seed = 0);

	/**
	 * Classic fBm over Perlin3D / Simplex3D / Cellular3D / Voronoi3D: what FVoxelCPUNoiseGenerator::FBM3D was before MP4.
	 *
	 * @param Position World position to sample
	 * @param Params Noise parameters (octaves, frequency, etc.)
	 * @return Noise value in range approximately [-1, 1]
	 */
	float ClassicFBM3D(const FVector& Position, const FVoxelNoiseParams& Params);

	/**
	 * Classic 3D Cellular (Worley) noise at a position.
	 * Uses 3x3x3 cell search to find distances to nearest two feature points.
	 *
	 * @param Position World position to sample (pre-scaled by frequency)
	 * @param Seed Random seed
	 * @param OutF1 Distance to nearest feature point
	 * @param OutF2 Distance to second nearest feature point
	 */
	void Cellular3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2);

	/**
	 * Classic 3D Voronoi noise at a position.
	 * Same as Cellular but also returns a stable cell ID.
	 *
	 * @param Position World position to sample (pre-scaled by frequency)
	 * @param Seed Random seed
	 * @param OutF1 Distance to nearest feature point
	 * @param OutF2 Distance to second nearest feature point
	 * @param OutCellID Stable hash-based cell identifier [0, 1]
	 */
	void Voronoi3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2, float& OutCellID);
}

#endif // WITH_DEV_AUTOMATION_TESTS
