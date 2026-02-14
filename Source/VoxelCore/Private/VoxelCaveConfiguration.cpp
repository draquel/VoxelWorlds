// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCaveConfiguration.h"

UVoxelCaveConfiguration::UVoxelCaveConfiguration()
{
	InitializeDefaults();
}

void UVoxelCaveConfiguration::InitializeDefaults()
{
	CaveLayers.Empty();

	// Layer 0: Cheese caves — large open caverns deep underground
	{
		FCaveLayerConfig Cheese;
		Cheese.bEnabled = true;
		Cheese.CaveType = ECaveType::Cheese;
		Cheese.SeedOffset = 3001;
		Cheese.Frequency = 0.00025f;
		Cheese.Threshold = 0.33f;
		Cheese.CarveFalloff = 0.2f;
		Cheese.MinDepth = 27.0f;
		Cheese.DepthFadeWidth = 6.0f;
		Cheese.VerticalScale = 0.6f;
		CaveLayers.Add(Cheese);
	}

	// Layer 1: Spaghetti caves — winding traversable tunnels
	{
		FCaveLayerConfig Spaghetti;
		Spaghetti.bEnabled = true;
		Spaghetti.CaveType = ECaveType::Spaghetti;
		Spaghetti.SeedOffset = 4001;
		Spaghetti.Frequency = 0.0002f;
		Spaghetti.MinDepth = 10.0f;
		Spaghetti.MaxDepth = 30.0f;
		CaveLayers.Add(Spaghetti);
	}

	// Layer 2: Noodle caves — narrow passages with occasional surface openings
	{
		FCaveLayerConfig Noodle;
		Noodle.bEnabled = true;
		Noodle.CaveType = ECaveType::Noodle;
		Noodle.SeedOffset = 5001;
		Noodle.Frequency = 0.0003f;
		Noodle.Octaves = 2;
		Noodle.Threshold = 0.16f;
		Noodle.CarveFalloff = 0.06f;
		Noodle.MinDepth = 2.0f;
		Noodle.MaxDepth = 15.0f;
		Noodle.DepthFadeWidth = 3.0f;
		Noodle.VerticalScale = 0.45f;
		Noodle.SecondNoiseSeedOffset = 8888;
		Noodle.SecondNoiseFrequencyScale = 1.5f;
		CaveLayers.Add(Noodle);
	}
}

float UVoxelCaveConfiguration::GetBiomeCaveScale(uint8 BiomeID) const
{
	for (const FBiomeCaveOverride& Override : BiomeOverrides)
	{
		if (Override.BiomeID == BiomeID)
		{
			return Override.CaveScale;
		}
	}
	return 1.0f;
}

float UVoxelCaveConfiguration::GetBiomeMinDepthOverride(uint8 BiomeID) const
{
	for (const FBiomeCaveOverride& Override : BiomeOverrides)
	{
		if (Override.BiomeID == BiomeID)
		{
			return Override.MinDepthOverride;
		}
	}
	return -1.0f;
}

bool UVoxelCaveConfiguration::HasEnabledLayers() const
{
	for (const FCaveLayerConfig& Layer : CaveLayers)
	{
		if (Layer.bEnabled)
		{
			return true;
		}
	}
	return false;
}
