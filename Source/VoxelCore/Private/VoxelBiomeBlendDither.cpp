// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBiomeBlendDither.h"
#include "VoxelHash.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarBiomeBlendDither(
	TEXT("voxel.Biome.BlendDither"),
	1,
	TEXT("Biome blend zones (world modes review G39): 1 = each voxel's material pick by 3D value noise over a 4-voxel lattice, "
	     "equalised, walking the biomes in BiomeID order (patches); 0 = frac(W0 * 17.3 + W1 * 31.7 + depth * 0.1) in weight "
	     "order, as before (bands along the voxel grid on slopes). Read when chunks generate; GPU and CPU alike."),
	ECVF_Default);

namespace VoxelBiomeBlendDitherPrivate
{
	/** The shared hash (VoxelHash::Lattice) on a lattice point: its top 16 bits. */
	FORCEINLINE uint32 LatticeHash16(int32 X, int32 Y, int32 Z, int32 Seed)
	{
		return VoxelHash::Lattice(static_cast<uint32>(X), static_cast<uint32>(Y), static_cast<uint32>(Z), Seed) >> 16;
	}

	/** Smoothstep at a voxel's offset 0..3 in its cell, in 32nds: 3t^2 - 2t^3 at t = 0, 1/4, 1/2, 3/4. */
	constexpr uint32 FarWeight[4] = VOXELGEN_BLEND_DITHER_FAR_WEIGHT;
}

// The raw value noise's CDF (VoxelGenerationShared.ush, the GPU's table too; what it is and how it was made is there).
const uint32 FVoxelBiomeBlendDither::Cdf[257] = VOXELGEN_BLEND_DITHER_CDF;

uint32 FVoxelBiomeBlendDither::RawValue(const FIntVector& Key, int32 WorldSeed)
{
	using namespace VoxelBiomeBlendDitherPrivate;
	const int32 Seed = WorldSeed + SeedOffset;
	// The lattice cell (floor division) and the voxel's offset in it; arithmetic shifts and masks, exact for negatives.
	const int32 LX = Key.X >> LatticeShift, LY = Key.Y >> LatticeShift, LZ = Key.Z >> LatticeShift;
	const uint32 Mask = (1u << LatticeShift) - 1u;
	const uint32 WX1 = FarWeight[static_cast<uint32>(Key.X) & Mask];
	const uint32 WY1 = FarWeight[static_cast<uint32>(Key.Y) & Mask];
	const uint32 WZ1 = FarWeight[static_cast<uint32>(Key.Z) & Mask];
	const uint32 WX[2] = { 32u - WX1, WX1 };
	const uint32 WY[2] = { 32u - WY1, WY1 };
	const uint32 WZ[2] = { 32u - WZ1, WZ1 };
	uint32 Sum = 0u;
	for (int32 DZ = 0; DZ < 2; ++DZ)
	{
		for (int32 DY = 0; DY < 2; ++DY)
		{
			for (int32 DX = 0; DX < 2; ++DX)
			{
				Sum += LatticeHash16(LX + DX, LY + DY, LZ + DZ, Seed) * (WX[DX] * WY[DY] * WZ[DZ]);
			}
		}
	}
	return Sum; // the weights sum to 32^3 = 2^15, so Sum <= 65535 * 2^15 < 2^31
}

uint32 FVoxelBiomeBlendDither::Equalise(uint32 Raw)
{
	const uint32 Bucket = FMath::Min(Raw >> 23, 255u);
	const uint32 Frac14 = (Raw >> 9) & 0x3FFFu;
	const uint32 Lo = Cdf[Bucket];
	const uint32 Hi = Cdf[Bucket + 1];
	return Lo + (((Hi - Lo) * Frac14) >> 14);
}

float FVoxelBiomeBlendDither::Sample(const FVoxelBlendDitherKey& Dither)
{
	return static_cast<float>(Equalise(RawValue(Dither.Key, Dither.Seed))) * (1.0f / 16777216.0f);
}

FIntVector FVoxelBiomeBlendDither::GridBase(const FIntVector& ChunkCoord, int32 ChunkSize, const FVector& WorldOrigin, float VoxelSize)
{
	const double Inv = 1.0 / FMath::Max(static_cast<double>(VoxelSize), 1.0e-6);
	return ChunkCoord * ChunkSize + FIntVector(
		FMath::RoundToInt32(WorldOrigin.X * Inv), FMath::RoundToInt32(WorldOrigin.Y * Inv), FMath::RoundToInt32(WorldOrigin.Z * Inv));
}

FIntVector FVoxelBiomeBlendDither::KeyAt(const FVector& WorldPos, float VoxelSize)
{
	const double Inv = 1.0 / FMath::Max(static_cast<double>(VoxelSize), 1.0e-6);
	return FIntVector(FMath::RoundToInt32(WorldPos.X * Inv), FMath::RoundToInt32(WorldPos.Y * Inv), FMath::RoundToInt32(WorldPos.Z * Inv));
}

bool FVoxelBiomeBlendDither::IsEnabled()
{
	return CVarBiomeBlendDither.GetValueOnAnyThread() != 0;
}
