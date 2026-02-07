// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelScatterTypes.h"

/**
 * Generates scatter spawn points from surface data.
 *
 * Applies scatter definitions to surface points to determine
 * where instances should be placed. Uses deterministic randomness
 * so the same seed produces the same results.
 *
 * Thread Safety: All methods are stateless and thread-safe.
 */
class VOXELSCATTER_API FVoxelScatterPlacement
{
public:
	/**
	 * Generate spawn points for a chunk.
	 *
	 * Iterates over all surface points, evaluates each scatter definition,
	 * and probabilistically spawns instances based on density.
	 *
	 * @param SurfaceData Extracted surface points
	 * @param Definitions Scatter types to evaluate
	 * @param ChunkSeed Base seed for deterministic randomness
	 * @param OutScatterData Generated scatter data with spawn points
	 */
	static void GenerateSpawnPoints(
		const FChunkSurfaceData& SurfaceData,
		const TArray<FScatterDefinition>& Definitions,
		uint32 ChunkSeed,
		FChunkScatterData& OutScatterData);

	/**
	 * Generate spawn points for a single scatter type.
	 *
	 * @param SurfaceData Extracted surface points
	 * @param Definition Scatter type to evaluate
	 * @param ChunkSeed Base seed
	 * @param OutSpawnPoints Generated spawn points (appended to)
	 * @return Number of points generated
	 */
	static int32 GenerateSpawnPointsForType(
		const FChunkSurfaceData& SurfaceData,
		const FScatterDefinition& Definition,
		uint32 ChunkSeed,
		TArray<FScatterSpawnPoint>& OutSpawnPoints);

	/**
	 * Compute a chunk seed from chunk coordinate and world seed.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param WorldSeed World generation seed
	 * @return Deterministic seed for this chunk
	 */
	static uint32 ComputeChunkSeed(const FIntVector& ChunkCoord, uint32 WorldSeed);

private:
	/**
	 * Hash a position to generate deterministic seed.
	 */
	static uint32 HashPosition(const FVector& Position, uint32 BaseSeed);

	/**
	 * Generate random float 0-1 from seed.
	 * Uses LCG (Linear Congruential Generator) for speed.
	 */
	static float RandomFromSeed(uint32& Seed);

	/**
	 * Generate random float in range from seed.
	 */
	static float RandomInRange(uint32& Seed, float Min, float Max);
};
