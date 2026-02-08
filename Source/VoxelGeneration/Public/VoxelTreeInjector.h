// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"
#include "VoxelTreeTypes.h"

struct FVoxelNoiseParams;
class IVoxelWorldMode;
class UVoxelBiomeConfiguration;

/**
 * Stamps voxel tree blocks into chunk VoxelData during generation.
 *
 * Deterministic: same seed + templates = same trees every time.
 * Cross-chunk safe: trees near chunk borders are computed by checking
 * neighboring chunk tree positions and only writing voxels within bounds.
 *
 * Thread Safety: All methods are stateless and thread-safe.
 *
 * @see FVoxelTreeTemplate
 */
class VOXELGENERATION_API FVoxelTreeInjector
{
public:
	/**
	 * Inject all trees that affect a given chunk.
	 *
	 * Computes tree positions for this chunk AND neighboring chunks (within tree extent range),
	 * then stamps tree blocks only within this chunk's voxel bounds.
	 * Tree positions are filtered by per-template placement rules (material, biome, slope, elevation).
	 *
	 * @param ChunkCoord This chunk's coordinate
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @param WorldOrigin World origin offset
	 * @param WorldSeed World generation seed
	 * @param Templates Available tree templates
	 * @param NoiseParams Noise parameters for height sampling
	 * @param WorldMode World mode interface for height queries
	 * @param TreeDensity Trees per chunk (fractional = probability)
	 * @param BiomeConfig Biome configuration for material/biome queries (can be nullptr to skip biome filtering)
	 * @param bEnableWaterLevel Whether water level is active
	 * @param WaterLevel Water level height (trees below this are skipped)
	 * @param InOutVoxelData Voxel data to modify (must be ChunkSize^3 elements)
	 */
	static void InjectTrees(
		const FIntVector& ChunkCoord,
		int32 ChunkSize, float VoxelSize,
		const FVector& WorldOrigin, int32 WorldSeed,
		const TArray<FVoxelTreeTemplate>& Templates,
		const FVoxelNoiseParams& NoiseParams,
		const IVoxelWorldMode& WorldMode,
		float TreeDensity,
		const UVoxelBiomeConfiguration* BiomeConfig,
		bool bEnableWaterLevel, float WaterLevel,
		TArray<FVoxelData>& InOutVoxelData);

	/**
	 * Compute deterministic tree positions for a specific source chunk.
	 * Positions are filtered by per-template placement rules.
	 *
	 * @param SourceChunkCoord The chunk to compute tree positions for
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @param WorldOrigin World origin offset
	 * @param WorldSeed World generation seed
	 * @param NoiseParams Noise parameters for height sampling
	 * @param WorldMode World mode interface for height queries
	 * @param TreeDensity Trees per chunk
	 * @param Templates Available templates (for random selection)
	 * @param BiomeConfig Biome configuration for material/biome queries (can be nullptr)
	 * @param bEnableWaterLevel Whether water level filtering is active
	 * @param WaterLevel Water level height
	 * @param OutGlobalVoxelPositions Global voxel coordinates of tree base positions
	 * @param OutTemplateIDs Template ID for each tree
	 * @param OutSeeds Per-tree random seed
	 */
	static void ComputeTreePositionsForChunk(
		const FIntVector& SourceChunkCoord,
		int32 ChunkSize, float VoxelSize,
		const FVector& WorldOrigin, int32 WorldSeed,
		const FVoxelNoiseParams& NoiseParams,
		const IVoxelWorldMode& WorldMode,
		float TreeDensity,
		const TArray<FVoxelTreeTemplate>& Templates,
		const UVoxelBiomeConfiguration* BiomeConfig,
		bool bEnableWaterLevel, float WaterLevel,
		TArray<FIntVector>& OutGlobalVoxelPositions,
		TArray<int32>& OutTemplateIDs,
		TArray<uint32>& OutSeeds);

private:
	/**
	 * Stamp a single tree, writing only voxels that fall within the target chunk bounds.
	 */
	static void StampTree(
		const FIntVector& BaseGlobalVoxel,
		const FVoxelTreeTemplate& Template,
		uint32 TreeSeed,
		const FIntVector& ChunkCoord,
		int32 ChunkSize,
		TArray<FVoxelData>& InOutVoxelData);

	/** Compute a deterministic seed for a chunk's trees */
	static uint32 ComputeTreeChunkSeed(const FIntVector& ChunkCoord, int32 WorldSeed);

	/** LCG random in [0,1) */
	static float RandomFromSeed(uint32& Seed);

	/** LCG random integer in [Min, Max] */
	static int32 RandomIntFromSeed(uint32& Seed, int32 Min, int32 Max);

	/**
	 * Compute terrain slope angle at a world position by sampling neighboring heights.
	 * @return Slope angle in degrees (0 = flat, 90 = vertical)
	 */
	static float ComputeSlopeAt(
		float WorldX, float WorldY, float VoxelSize,
		const IVoxelWorldMode& WorldMode,
		const FVoxelNoiseParams& NoiseParams);

	/**
	 * Query surface material and biome at a world position.
	 * Uses biome noise sampling (temperature/moisture → biome selection → surface material).
	 * @param OutSurfaceMaterial The surface material ID at this position
	 * @param OutBiomeID The biome ID at this position
	 */
	static void QuerySurfaceConditions(
		float WorldX, float WorldY, float TerrainHeight, float VoxelSize,
		const UVoxelBiomeConfiguration* BiomeConfig,
		int32 WorldSeed,
		bool bEnableWaterLevel, float WaterLevel,
		uint8& OutSurfaceMaterial, uint8& OutBiomeID);
};
