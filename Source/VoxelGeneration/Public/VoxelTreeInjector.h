// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"
#include "VoxelTreeTypes.h"

struct FVoxelNoiseParams;
class IVoxelWorldMode;
class UVoxelBiomeConfiguration;

/**
 * One voxel tree as the injector places it (world modes review WM-C8, KC9): recomputed from the same seeded positions the
 * stamp uses, so gameplay can find and fell the tree whose voxels a chunk holds.
 */
struct FVoxelTreeInstance
{
	/** Global voxel coordinate of the trunk's lowest block (the ground's top voxel, which the trunk replaces). */
	FIntVector BaseVoxel = FIntVector::ZeroValue;

	/** Index into the templates the trees were placed with. */
	int32 TemplateIndex = INDEX_NONE;

	/** The tree's own seed (height and canopy variation). */
	uint32 Seed = 0;

	bool IsValid() const { return TemplateIndex != INDEX_NONE; }
};

/** One voxel a tree's stamp writes (FVoxelTreeInjector::CollectTreeVoxels). */
struct FVoxelTreeVoxel
{
	/** Global voxel coordinate (above WorldOrigin, in voxels). */
	FIntVector GlobalVoxel = FIntVector::ZeroValue;

	/** The material the stamp writes there (the template's trunk or leaf material). */
	uint8 MaterialID = 0;

	/** A trunk block (written over anything) rather than a leaf (written only over air or other trees' blocks). */
	bool bTrunk = false;
};

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

	/**
	 * Every voxel one tree's stamp writes, in the order the stamp writes them (canopy, then trunk), the trunk's base block
	 * included. The same shape enumeration as InjectTrees, so the result equals what the stamp wrote wherever nothing
	 * stamped later (a neighbouring tree, or terrain under a leaf) overrode it.
	 *
	 * @param BaseGlobalVoxel The tree's base (FVoxelTreeInstance::BaseVoxel)
	 * @param Template The tree's template
	 * @param TreeSeed The tree's seed
	 * @param OutVoxels Receives the voxels (appended)
	 */
	static void CollectTreeVoxels(const FIntVector& BaseGlobalVoxel, const FVoxelTreeTemplate& Template, uint32 TreeSeed,
		TArray<FVoxelTreeVoxel>& OutVoxels);

	/**
	 * The trees whose trunk passes through a voxel (world modes review WM-C8): recomputed from the seeded positions of the
	 * chunks around it, as InjectTrees places them. A tree the stamp skipped (its base under an overhang) is still listed;
	 * callers check the voxels are there. Arguments as InjectTrees.
	 *
	 * @param GlobalVoxel The voxel (global voxel coordinate)
	 * @param OutTrees Receives the trees (appended); usually one, two only where trunks touch
	 */
	static void FindTreesWithTrunkAt(
		const FIntVector& GlobalVoxel,
		int32 ChunkSize, float VoxelSize,
		const FVector& WorldOrigin, int32 WorldSeed,
		const TArray<FVoxelTreeTemplate>& Templates,
		const FVoxelNoiseParams& NoiseParams,
		const IVoxelWorldMode& WorldMode,
		float TreeDensity,
		const UVoxelBiomeConfiguration* BiomeConfig,
		bool bEnableWaterLevel, float WaterLevel,
		TArray<FVoxelTreeInstance>& OutTrees);

private:
	/**
	 * Visit every voxel a tree's stamp writes, canopy first then trunk (the trunk punches through the canopy): the shape
	 * StampTree writes and CollectTreeVoxels lists.
	 */
	static void ForEachTreeVoxel(const FIntVector& BaseGlobalVoxel, const FVoxelTreeTemplate& Template, uint32 TreeSeed,
		TFunctionRef<void(int32 GX, int32 GY, int32 GZ, uint8 MaterialID, bool bTrunk)> Visit);

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

	// Surface slope and biome/material queries are provided by the shared
	// FVoxelSurfaceQuery utility (VoxelGeneration/Public/VoxelSurfaceQuery.h),
	// reused by both this injector and the VoxelPCG surface sampler.
};
