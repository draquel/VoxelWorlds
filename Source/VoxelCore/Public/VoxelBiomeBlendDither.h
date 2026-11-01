// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelGenerationShared.ush"

/**
 * Where a biome blend's material pick is made: a voxel's key on the world's voxel grid and the world seed. The key is the
 * voxel's index counted from the world's origin of space, so every consumer finds the same one for the same voxel
 * (FVoxelBiomeBlendDither::KeyAt from a world position, GridBase plus the local index in generation).
 */
struct FVoxelBlendDitherKey
{
	/** The voxel's integer position in voxels (round(WorldPos / VoxelSize)). */
	FIntVector Key = FIntVector::ZeroValue;

	/** The world seed (FVoxelNoiseParams::Seed). */
	int32 Seed = 0;

	FVoxelBlendDitherKey() = default;
	FVoxelBlendDitherKey(const FIntVector& InKey, int32 InSeed) : Key(InKey), Seed(InSeed) {}
};

/**
 * The biome blend's dither (world modes review G39). A blend zone picks one biome's material per voxel with probability
 * equal to its weight. The value that decides the pick used to be frac(W0 * 17.3 + W1 * 31.7 + depth * 0.1): across a
 * zone the weights change slowly, so it followed the voxel's depth under the surface, which cycles with the grid on a
 * slope, and drew bands of two materials along the voxel grid.
 *
 * Now it is 3D value noise over a lattice of 4 voxels: the 8 corners' 16-bit integer hashes (the ore Rarity hash's mix, on
 * the lattice coordinates and the world seed), trilinear with smoothstep weights (0, 5, 16, 27 of 32 at the voxel's offset
 * in its cell), all in integers; then equalised through a fixed table of the noise's distribution (Cdf), so the value is
 * uniform on [0, 1) and a biome's share of the voxels stays its weight. The two biomes interleave in patches a few voxels
 * across. Exact multiples of 2^-24, mirrored bit for bit by GpuBiomeBlendDither (BiomeMaterial.ush).
 *
 * The pick walks the blend's biomes in BiomeID order, not by weight, so a patch does not change biome where the dominant
 * biome changes inside a zone. voxel.Biome.BlendDither 0 restores the old value and order (for A/B).
 */
struct VOXELCORE_API FVoxelBiomeBlendDither
{
	/** Lattice spacing: 1 << LatticeShift voxels. */
	static constexpr int32 LatticeShift = 2;

	/** Added to the world seed, so the dither is not correlated with other hashes of it. */
	static constexpr int32 SeedOffset = VOXELGEN_BLEND_DITHER_SEED_OFFSET;

	/** The raw value noise's CDF at b * 2^23 (b = 0..256), in 2^-24 units; Equalise interpolates it. */
	static const uint32 Cdf[257];

	/**
	 * The raw value noise at a voxel: in [0, 65535 * 32768].
	 *
	 * @param Key The voxel's key
	 * @param WorldSeed The world seed
	 * @return The trilinear sum of the lattice hashes
	 */
	static uint32 RawValue(const FIntVector& Key, int32 WorldSeed);

	/**
	 * Equalise a raw value through Cdf: uniform on [0, 2^24) over the lattice.
	 *
	 * @param Raw RawValue
	 * @return The equalised value, in 2^-24 units
	 */
	static uint32 Equalise(uint32 Raw);

	/**
	 * The dither at a voxel: uniform on [0, 1), constant in patches a few voxels across.
	 *
	 * @param Dither The voxel's key and the world seed
	 * @return Equalise(RawValue) / 2^24
	 */
	static float Sample(const FVoxelBlendDitherKey& Dither);

	/**
	 * The key of a chunk's voxel (0, 0, 0) on a grid whose voxel samples lie at WorldOrigin + (ChunkCoord * ChunkSize + i)
	 * * VoxelSize; a voxel's key is this plus its local index. Equal to KeyAt at the voxel's sample point.
	 *
	 * @param ChunkCoord The chunk's coordinate
	 * @param ChunkSize Voxels along a chunk's edge
	 * @param WorldOrigin The world's origin (the grid's)
	 * @param VoxelSize Voxel size (uu)
	 * @return The key of the chunk's first voxel
	 */
	static FIntVector GridBase(const FIntVector& ChunkCoord, int32 ChunkSize, const FVector& WorldOrigin, float VoxelSize);

	/**
	 * The key of the voxel nearest a world position (analytic consumers: the surface query, the map).
	 *
	 * @param WorldPos World position
	 * @param VoxelSize Voxel size (uu)
	 * @return round(WorldPos / VoxelSize)
	 */
	static FIntVector KeyAt(const FVector& WorldPos, float VoxelSize);

	/** voxel.Biome.BlendDither (1: the lattice dither in BiomeID order; 0: the old frac value in weight order). */
	static bool IsEnabled();
};
