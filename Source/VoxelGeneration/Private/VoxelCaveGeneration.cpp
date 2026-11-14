// Copyright Daniel Raquel. All Rights Reserved.

// FVoxelCPUNoiseGenerator's the cave carve (a cave layer's density and the carve at a voxel; GPU twin GpuCaveCarve in GenerateVoxelDensity.usf): a translation unit of the class.

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

// ==================== Cave Generation Helpers ====================

float FVoxelCPUNoiseGenerator::SampleCaveLayer(const FVector& WorldPos, const FCaveLayerConfig& LayerConfig, int32 WorldSeed,
	const FSphericalPlanetParams* Planet)
{
	// Apply vertical scale to flatten caves horizontally: along world Z in heightmap modes, along local up on a planet
	// (the altitude above the datum is scaled, the direction kept; SP-B6). A planet's caves sample the planet noise
	// relative to its centre (WM-BP5, D22): exact at any radius, no period, and they move with the planet.
	const FVector ScaledPos = Planet
		? FSphericalPlanetWorldMode::ScaleAltitudeRelative(WorldPos - Planet->PlanetCenter, Planet->PlanetRadius, LayerConfig.VerticalScale)
		: FVector(WorldPos.X, WorldPos.Y, WorldPos.Z * LayerConfig.VerticalScale);
	auto SampleField = [Planet, &ScaledPos](const FVoxelNoiseParams& Params)
	{
		return Planet ? FVoxelNoise::FBM3D(ScaledPos, Params) : FBM3D(ScaledPos, Params);
	};

	// Build noise params for first noise field
	FVoxelNoiseParams CaveNoiseParams;
	CaveNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	CaveNoiseParams.Seed = WorldSeed + LayerConfig.SeedOffset;
	CaveNoiseParams.Frequency = LayerConfig.Frequency;
	CaveNoiseParams.Octaves = LayerConfig.Octaves;
	CaveNoiseParams.Persistence = LayerConfig.Persistence;
	CaveNoiseParams.Lacunarity = LayerConfig.Lacunarity;

	if (LayerConfig.CaveType == ECaveType::Cheese)
	{
		// Cheese caves: single noise field, carve where noise > threshold
		float Noise = SampleField(CaveNoiseParams);

		// Noise is in [-1, 1], map threshold to that range
		if (Noise <= LayerConfig.Threshold)
		{
			return 0.0f;
		}

		// Smooth falloff above threshold
		float Excess = Noise - LayerConfig.Threshold;
		float FalloffRange = FMath::Max(LayerConfig.CarveFalloff, 0.01f);
		float CarveDensity = FMath::Clamp(Excess / FalloffRange, 0.0f, 1.0f);

		return CarveDensity * LayerConfig.CarveStrength;
	}
	else
	{
		// Spaghetti and Noodle: dual-noise intersection
		// Tunnel forms where BOTH noise fields are near zero simultaneously
		float Noise1 = SampleField(CaveNoiseParams);

		// Second noise field with offset seed and scaled frequency
		FVoxelNoiseParams SecondNoiseParams = CaveNoiseParams;
		SecondNoiseParams.Seed = WorldSeed + LayerConfig.SecondNoiseSeedOffset;
		SecondNoiseParams.Frequency = LayerConfig.Frequency * LayerConfig.SecondNoiseFrequencyScale;

		float Noise2 = SampleField(SecondNoiseParams);

		// Both noise fields must be within [-Threshold, Threshold] for a tunnel
		float AbsNoise1 = FMath::Abs(Noise1);
		float AbsNoise2 = FMath::Abs(Noise2);

		if (AbsNoise1 >= LayerConfig.Threshold || AbsNoise2 >= LayerConfig.Threshold)
		{
			return 0.0f;
		}

		// Carve density is stronger as both approach zero
		float FalloffRange = FMath::Max(LayerConfig.CarveFalloff, 0.01f);
		float Carve1 = FMath::Clamp(1.0f - (AbsNoise1 / LayerConfig.Threshold), 0.0f, 1.0f);
		float Carve2 = FMath::Clamp(1.0f - (AbsNoise2 / LayerConfig.Threshold), 0.0f, 1.0f);

		// Multiply for intersection — both must be near zero
		float CarveDensity = Carve1 * Carve2;

		// Apply smooth falloff
		CarveDensity = FMath::SmoothStep(0.0f, FalloffRange, CarveDensity);

		return CarveDensity * LayerConfig.CarveStrength;
	}
}

float FVoxelCPUNoiseGenerator::CalculateCaveDensity(
	const FVector& WorldPos,
	float DepthBelowSurface,
	uint8 BiomeID,
	const UVoxelCaveConfiguration* CaveConfig,
	int32 WorldSeed,
	bool bIsUnderwater,
	const FSphericalPlanetParams* Planet,
	float MinDepthUnderLines)
{
	if (!CaveConfig || !CaveConfig->bEnableCaves)
	{
		return 0.0f;
	}

	// Get biome scaling
	float BiomeCaveScale = CaveConfig->GetBiomeCaveScale(BiomeID);
	if (BiomeCaveScale <= 0.0f)
	{
		return 0.0f;
	}

	float BiomeMinDepthOverride = CaveConfig->GetBiomeMinDepthOverride(BiomeID);

	// Apply underwater min depth — catches biome transition zones where the
	// ocean biome isn't assigned but terrain is still below water level
	if (bIsUnderwater && CaveConfig->UnderwaterMinDepth > 0.0f)
	{
		if (BiomeMinDepthOverride < 0.0f)
		{
			BiomeMinDepthOverride = CaveConfig->UnderwaterMinDepth;
		}
		else
		{
			BiomeMinDepthOverride = FMath::Max(BiomeMinDepthOverride, CaveConfig->UnderwaterMinDepth);
		}
	}

	float MaxCarveDensity = 0.0f;

	for (const FCaveLayerConfig& Layer : CaveConfig->CaveLayers)
	{
		if (!Layer.bEnabled)
		{
			continue;
		}

		// Determine effective min depth (biome override or layer default)
		float EffectiveMinDepth = (BiomeMinDepthOverride >= 0.0f) ? BiomeMinDepthOverride : Layer.MinDepth;

		// Under a conditioning line no cave opens above the line's minimum depth: it only ever deepens the layer's.
		if (MinDepthUnderLines > 0.0f)
		{
			EffectiveMinDepth = FMath::Max(EffectiveMinDepth, MinDepthUnderLines);
		}

		// Check depth constraints
		if (DepthBelowSurface < EffectiveMinDepth - Layer.DepthFadeWidth)
		{
			continue;
		}

		if (Layer.MaxDepth > 0.0f && DepthBelowSurface > Layer.MaxDepth + Layer.DepthFadeWidth)
		{
			continue;
		}

		// Sample this cave layer
		float LayerCarve = SampleCaveLayer(WorldPos, Layer, WorldSeed, Planet);

		if (LayerCarve <= 0.0f)
		{
			continue;
		}

		// Apply depth fade at MinDepth boundary
		if (DepthBelowSurface < EffectiveMinDepth)
		{
			float FadeT = (DepthBelowSurface - (EffectiveMinDepth - Layer.DepthFadeWidth)) / Layer.DepthFadeWidth;
			LayerCarve *= FMath::SmoothStep(0.0f, 1.0f, FadeT);
		}

		// Apply depth fade at MaxDepth boundary
		if (Layer.MaxDepth > 0.0f && DepthBelowSurface > Layer.MaxDepth)
		{
			float FadeT = 1.0f - (DepthBelowSurface - Layer.MaxDepth) / Layer.DepthFadeWidth;
			LayerCarve *= FMath::SmoothStep(0.0f, 1.0f, FadeT);
		}

		// Union composition: take the maximum carve from any layer
		MaxCarveDensity = FMath::Max(MaxCarveDensity, LayerCarve);
	}

	// Apply biome scaling
	return FMath::Clamp(MaxCarveDensity * BiomeCaveScale, 0.0f, 1.0f);
}
