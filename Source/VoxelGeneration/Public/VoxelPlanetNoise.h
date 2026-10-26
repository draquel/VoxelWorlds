// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"

/**
 * A noise-space point split for precision (spherical Phase 6, WM-BP5): the integer lattice cell holding it and the
 * fraction within that cell. For Simplex the split is in the skewed lattice; for Perlin, Cellular and Voronoi in the
 * cube lattice. The cell wraps mod 2^32 (the lattice hash reads only its 32 bits), so any double position splits.
 */
struct VOXELGENERATION_API FVoxelNoiseSplit
{
	/** Lattice cell (wrapping). */
	FIntVector Cell = FIntVector::ZeroValue;

	/** Fraction within the cell, in [0, 1) on each axis. */
	FVector3f Frac = FVector3f::ZeroVector;
};

/**
 * The terrain noise (spherical Phase 6, WM-BP5, decision D22; every world mode since the world modes review's MP4, decision
 * D39): the noise every field samples (terrain, continentalness, temperature, moisture, caves and ores), on the CPU and, bit
 * for bit in its algebra, on the GPU (PlanetNoise.ush). The name is from spherical Phase 6, when planets alone had it;
 * FVoxelCPUNoiseGenerator::FBM3D, the heightmap entry point, has come through here since MP4 (GenerationVersion 3).
 *
 * Two things differ from the classic table noise heightmap worlds sampled before MP4 (FVoxelCPUNoiseGenerator::ClassicFBM3D):
 *
 * - A hashed integer lattice (LatticeHash, the ore Rarity hash's mixing) instead of Ken Perlin's 256-entry permutation
 *   table, so the noise has no period: the old table repeated every field every 256 lattice units, a 1,000 km planet's
 *   terrain 61 times around its equator.
 * - Split-origin sampling: a sample point is an origin, split in double into an integer lattice cell and a float fraction
 *   per octave (Split), plus a small float offset in noise units. Every float operation sees only values of the size of
 *   a cell, so the noise is exact to float precision at any distance from the centre (a float position at 1e8 uu is 8 uu
 *   coarse: 10 uu terraces in the terrain's top octave on Review_Planet_L). The CPU splits each sample at the point
 *   itself (FBM3D(Position)); the GPU splits a per-chunk origin on the CPU and adds each voxel's offset (FBM3D(Origin,
 *   Offset) is that form on the CPU, for parity and tests).
 *
 * - A continuous Simplex kernel (G43, PlanetGenerationVersion 6): the classic (0.6 - d^2)^4 corner weight stepped where
 *   a sample crossed into the next simplex (up to 136 uu along Review_Planet_L's cube face edges, where every octave's
 *   boundary lies); it is now that weight less its value and slope at d^2 = 0.5. voxel.Noise.ContinuousSimplex 0
 *   restores the classic one (A/B; voxel.Planet.ContinuousSimplex before MP4).
 *
 * The octave loop (frequencies multiplied in float, amplitudes, normalisation) is FVoxelCPUNoiseGenerator::FBM3D's.
 *
 * Thread safety: stateless, any thread.
 * @see WORLD_MODES_REVIEW_PLAN.md §20.1 (WM-BP5, D22), Shaders/Private/PlanetNoise.ush
 */
class VOXELGENERATION_API FVoxelPlanetNoise
{
public:
	/** Most octaves a field has (FVoxelNoiseParams::Octaves' ClampMax). */
	static constexpr int32 MaxOctaves = 16;

	/**
	 * The lattice hash: a 32-bit value from a lattice corner and a seed, with wrapping multiplies, xors and shifts only
	 * (the mixing of FVoxelCPUNoiseGenerator::OreRarityHash), mirrored by PlanetLatticeHash in PlanetNoise.ush.
	 *
	 * @param X Corner X (wrapping)
	 * @param Y Corner Y
	 * @param Z Corner Z
	 * @param Seed The field's seed
	 * @return The hash
	 */
	static uint32 LatticeHash(uint32 X, uint32 Y, uint32 Z, int32 Seed)
	{
		uint32 H = X * 0x8DA6B343u;
		H ^= Y * 0xD8163841u;
		H ^= Z * 0xCB1AB31Fu;
		H ^= static_cast<uint32>(Seed) * 0x9E3779B9u;
		H ^= H >> 16;
		H *= 0x7FEB352Du;
		H ^= H >> 15;
		H *= 0x846CA68Bu;
		H ^= H >> 16;
		return H;
	}

	/**
	 * Split a noise-space point (a position already multiplied by the octave's frequency) into its lattice cell and
	 * fraction, in double: in the skewed lattice for Simplex, the cube lattice otherwise.
	 *
	 * @param NoisePoint The point in noise units
	 * @param Type Noise type
	 * @return The split
	 */
	static FVoxelNoiseSplit Split(const FVector& NoisePoint, EVoxelNoiseType Type);

	/**
	 * One octave of noise at Origin + Offset.
	 *
	 * @param Origin The split origin
	 * @param Offset The sample's offset from the origin in noise units (unskewed; small: a few cells for float precision)
	 * @param Type Noise type (Cellular: F1 mapped to [-1, 1]; Voronoi: F2 - F1 mapped to [-1, 1], as the heightmap FBM)
	 * @param Seed The field's seed
	 * @return Noise in about [-1, 1]
	 */
	static float Sample(const FVoxelNoiseSplit& Origin, const FVector3f& Offset, EVoxelNoiseType Type, int32 Seed);

	/** Simplex noise at Origin + Offset (Origin split in the skewed lattice): about [-1, 1], continuous across simplices. */
	static float Simplex(const FVoxelNoiseSplit& Origin, const FVector3f& Offset, int32 Seed);

	/**
	 * voxel.Noise.ContinuousSimplex: whether Simplex uses the continuous corner kernel (G43, the default) or the classic one.
	 *
	 * @return True for the continuous kernel
	 */
	static bool IsContinuousSimplex();

	/** Perlin (improved gradient) noise at Origin + Offset: about [-1, 1]. */
	static float Perlin(const FVoxelNoiseSplit& Origin, const FVector3f& Offset, int32 Seed);

	/**
	 * Cellular (Worley) noise at Origin + Offset: the distances to the nearest two feature points over the 3x3x3 cells
	 * around the sample, one feature point per cell from the hash.
	 *
	 * @param OutF1 Nearest distance
	 * @param OutF2 Second nearest distance
	 * @param OutCellID Optional: a [0, 1) value per nearest cell (Voronoi)
	 */
	static void Cellular(const FVoxelNoiseSplit& Origin, const FVector3f& Offset, int32 Seed, float& OutF1, float& OutF2, float* OutCellID = nullptr);

	/**
	 * Each octave's frequency as the FBM loop computes it: Frequency, then multiplied by Lacunarity in float per octave.
	 *
	 * @param Params Noise parameters
	 * @param OutFrequencies MaxOctaves floats
	 * @return The number of octaves (Params.Octaves clamped to [1, MaxOctaves])
	 */
	static int32 OctaveFrequencies(const FVoxelNoiseParams& Params, float* OutFrequencies);

	/**
	 * fBm at a position, each octave split at the point itself (the CPU planet paths: generation, the surface function,
	 * the surface queries, the map).
	 *
	 * @param Position Sample position in uu, relative to the planet centre (double)
	 * @param Params Noise parameters
	 * @return Noise in about [-1, 1]
	 */
	static float FBM3D(const FVector& Position, const FVoxelNoiseParams& Params);

	/**
	 * fBm at Origin + Offset: Origin split per octave (SplitOctaves), Offset scaled by each octave's frequency in float.
	 * The GPU's form (PlanetFBM3D), on the CPU for parity and tests.
	 *
	 * @param Origin Origin in uu (double)
	 * @param Offset The sample's offset from Origin in uu (float; within a chunk or so)
	 * @param Params Noise parameters
	 * @return Noise in about [-1, 1]
	 */
	static float FBM3D(const FVector& Origin, const FVector3f& Offset, const FVoxelNoiseParams& Params);

	/**
	 * The per-octave splits of an origin, what the GPU generator uploads for a chunk (2 float4s each).
	 *
	 * @param Origin Origin in uu (double)
	 * @param Params Noise parameters (type, frequency, lacunarity, octaves)
	 * @param OutSplits MaxOctaves splits
	 * @return The number of octaves written
	 */
	static int32 SplitOctaves(const FVector& Origin, const FVoxelNoiseParams& Params, FVoxelNoiseSplit* OutSplits);

	/**
	 * The 2-octave Simplex field the planet's continentalness, temperature and moisture use (the heightmap modes' climate
	 * parameters).
	 *
	 * @param Seed The field's seed
	 * @param Frequency The field's frequency
	 * @return Noise parameters
	 */
	static FVoxelNoiseParams MakeFieldParams(int32 Seed, float Frequency);

	// ==================== Double reference (tests) ====================

	/** One octave with the same lattice and hash, every step in double (Noise.SplitOrigin's reference). */
	static double ReferenceSample(const FVector& NoisePoint, EVoxelNoiseType Type, int32 Seed);

	/** fBm with ReferenceSample, the frequencies as OctaveFrequencies gives them. */
	static double ReferenceFBM3D(const FVector& Position, const FVoxelNoiseParams& Params);
};
