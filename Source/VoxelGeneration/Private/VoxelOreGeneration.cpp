// Copyright Daniel Raquel. All Rights Reserved.

// FVoxelCPUNoiseGenerator's ore veins (the vein noise, a planet's streak frame, the rarity hash and the placement check): a translation unit of the class.

#include "VoxelCPUNoiseGenerator.h"
#include "VoxelHash.h"
#include "VoxelGeneration.h"
#include "VoxelTerrainConditioning.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelSeaSurface.h"
#include "VoxelDownLines.h"
#include "VoxelNoise.h"
#include "Async/Async.h"

// ==================== Ore Vein Helpers ====================

float FVoxelCPUNoiseGenerator::SampleOreVeinNoise(const FVector& WorldPos, const FOreVeinConfig& OreConfig, int32 WorldSeed)
{
	// Create ore-specific noise params
	FVoxelNoiseParams OreNoiseParams;
	OreNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	OreNoiseParams.Seed = WorldSeed + OreConfig.SeedOffset;
	OreNoiseParams.Frequency = OreConfig.Frequency;
	OreNoiseParams.Octaves = 2;  // Simple noise for ores
	OreNoiseParams.Persistence = 0.5f;
	OreNoiseParams.Lacunarity = 2.0f;
	OreNoiseParams.Amplitude = 1.0f;

	FVector SamplePos = WorldPos;

	// A streak squashes the position along one direction per ore type (GetPlanetOreSamplePoint, the planet's linear map;
	// MP4 gives heightmap worlds the same: the direction formula that varied with the absolute position turned into
	// white noise far from the origin, and a linear map lets the GPU sample the ore at a chunk origin + the voxel's offset).
	SamplePos = GetPlanetOreSamplePoint(WorldPos, OreConfig);

	// Sample noise
	float NoiseValue = FBM3D(SamplePos, OreNoiseParams);

	// Normalize from [-1, 1] to [0, 1]
	return (NoiseValue + 1.0f) * 0.5f;
}

FVector FVoxelCPUNoiseGenerator::GetPlanetOreStreakDirection(const FOreVeinConfig& OreConfig)
{
	// The heightmap streak's direction formula at the origin: one direction per ore type (WM-BP5). A direction that varies
	// with the absolute position (the heightmap form) turns into white noise R-sized distances from the origin.
	const double StreakSeed = static_cast<double>(OreConfig.SeedOffset) * 0.1;
	return FVector(FMath::Sin(StreakSeed), FMath::Cos(StreakSeed * 1.5), FMath::Sin(StreakSeed * 2.0)).GetSafeNormal();
}

FVector FVoxelCPUNoiseGenerator::GetPlanetOreSamplePoint(const FVector& PlanetRelativePos, const FOreVeinConfig& OreConfig)
{
	if (OreConfig.Shape != EOreVeinShape::Streak)
	{
		return PlanetRelativePos;
	}
	// Squash the position along the streak direction by StreakStretch (the heightmap streak's projection): a linear map, so
	// a chunk's origin and its voxels' offsets map separately (the GPU's split origin).
	const FVector Dir = GetPlanetOreStreakDirection(OreConfig);
	const double Along = FVector::DotProduct(PlanetRelativePos, Dir);
	return PlanetRelativePos - Dir * (Along * (1.0 - 1.0 / FMath::Max(OreConfig.StreakStretch, 1.0f)));
}

float FVoxelCPUNoiseGenerator::SamplePlanetOreVeinNoise(const FVector& PlanetRelativePos, const FOreVeinConfig& OreConfig, int32 WorldSeed)
{
	// The heightmap ore field (2-octave Simplex at the ore's frequency and seed) by the planet noise, relative to the centre
	const FVoxelNoiseParams Params = FVoxelNoise::MakeFieldParams(WorldSeed + OreConfig.SeedOffset, OreConfig.Frequency);
	return (FVoxelNoise::FBM3D(GetPlanetOreSamplePoint(PlanetRelativePos, OreConfig), Params) + 1.0f) * 0.5f;
}

FIntVector FVoxelCPUNoiseGenerator::OreRarityKey(const FVector& PlanetRelativePos)
{
	return FIntVector(
		FMath::FloorToInt32(PlanetRelativePos.X + 0.5),
		FMath::FloorToInt32(PlanetRelativePos.Y + 0.5),
		FMath::FloorToInt32(PlanetRelativePos.Z + 0.5));
}

float FVoxelCPUNoiseGenerator::OreRarityHash(const FIntVector& Key, int32 Seed)
{
	// Mirrored bit for bit by GpuOreRarityHashKey (BiomeMaterial.ush): the shared hash's top 24 bits as a unit float.
	return VoxelHash::Unit(VoxelHash::Lattice(static_cast<uint32>(Key.X), static_cast<uint32>(Key.Y), static_cast<uint32>(Key.Z), Seed));
}

bool FVoxelCPUNoiseGenerator::CheckOreVeinPlacement(
	const FVector& WorldPos,
	float DepthBelowSurface,
	const TArray<FOreVeinConfig>& OreConfigs,
	int32 WorldSeed,
	uint8& OutMaterialID,
	const FVector* PlanetRelativePos)
{
	// Check each ore type in priority order (assumed to be pre-sorted)
	for (const FOreVeinConfig& OreConfig : OreConfigs)
	{
		// Check depth constraints
		if (!OreConfig.IsValidDepth(DepthBelowSurface))
		{
			continue;
		}

		// Sample ore noise at this position: on a planet the planet noise relative to its centre (WM-BP5)
		float OreNoise = PlanetRelativePos
			? SamplePlanetOreVeinNoise(*PlanetRelativePos, OreConfig, WorldSeed)
			: SampleOreVeinNoise(WorldPos, OreConfig, WorldSeed);

		// Check against threshold
		if (OreNoise >= OreConfig.Threshold)
		{
			// Apply rarity check (if rarity < 1, randomly skip some valid placements)
			if (OreConfig.Rarity < 1.0f)
			{
				// A deterministic value per voxel: an exact integer hash of the position rounded to whole uu, relative to
				// the planet centre on a planet (SP-H3) and absolute on a heightmap world (MP4; before it, frac(sin(p . k)),
				// which the GPU's float and the CPU's double rounded differently)
				const float RandomValue = OreRarityHash(OreRarityKey(PlanetRelativePos ? *PlanetRelativePos : WorldPos), WorldSeed);
				if (RandomValue > OreConfig.Rarity)
				{
					continue;
				}
			}

			OutMaterialID = OreConfig.MaterialID;
			return true;
		}
	}

	return false;
}
