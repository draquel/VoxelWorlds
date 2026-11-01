// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The one integer hash mix generation shares (code review CR2, CORE-21), the twin of VoxelHash.ush bit for bit: 32-bit
 * wrapping multiplies, xors and shifts only. The noise lattice (FVoxelPlanetNoise), the ore Rarity
 * (FVoxelCPUNoiseGenerator::OreRarityHash) and the biome blend dither (FVoxelBiomeBlendDither) call it.
 */
namespace VoxelHash
{
	/**
	 * A 32-bit hash of a lattice corner and a seed.
	 *
	 * @param X Corner X (wrapping)
	 * @param Y Corner Y
	 * @param Z Corner Z
	 * @param Seed The field's seed
	 * @return The hash
	 */
	FORCEINLINE uint32 Lattice(uint32 X, uint32 Y, uint32 Z, int32 Seed)
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

	/** The hash as a float in [0, 1): its top 24 bits. */
	FORCEINLINE float Unit(uint32 H)
	{
		return static_cast<float>(H >> 8) * (1.0f / 16777216.0f);
	}
}
