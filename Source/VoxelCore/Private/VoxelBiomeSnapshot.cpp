// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBiomeSnapshot.h"
#include "VoxelBiomeConfiguration.h"

FVoxelBiomeSnapshot FVoxelBiomeSnapshot::FromConfig(const UVoxelBiomeConfiguration* Config)
{
	FVoxelBiomeSnapshot Snapshot;
	if (Config)
	{
		Snapshot.bIsValid = Config->IsValid();

		// Continentalness (height modulation)
		Snapshot.bEnableContinentalness = Config->bEnableContinentalness;
		Snapshot.ContinentalnessSeedOffset = Config->ContinentalnessSeedOffset;
		Snapshot.ContinentalnessNoiseFrequency = Config->ContinentalnessNoiseFrequency;
		Snapshot.BakedHeightCurve = Config->BakedHeightCurve;
		Snapshot.BakedHeightScaleCurve = Config->BakedHeightScaleCurve;

		// Biome / material selection
		Snapshot.Biomes = Config->Biomes;
		Snapshot.BiomeBlendWidth = Config->BiomeBlendWidth;
		Snapshot.bEnableUnderwaterMaterials = Config->bEnableUnderwaterMaterials;
		Snapshot.DefaultUnderwaterMaterial = Config->DefaultUnderwaterMaterial;
		Snapshot.bEnableHeightMaterials = Config->bEnableHeightMaterials;
		if (Config->bEnableHeightMaterials)
		{
			// Priority-sorted at capture — mirrors RebuildHeightRulesCache so ApplyHeightMaterialRules
			// picks the same winner as the config's cached path when rules overlap.
			Snapshot.SortedHeightRules = Config->HeightMaterialRules;
			Snapshot.SortedHeightRules.Sort([](const FHeightMaterialRule& A, const FHeightMaterialRule& B) {
				return A.Priority > B.Priority;
			});
		}

		// Temperature / moisture noise
		Snapshot.TemperatureSeedOffset = Config->TemperatureSeedOffset;
		Snapshot.TemperatureNoiseFrequency = Config->TemperatureNoiseFrequency;
		Snapshot.MoistureSeedOffset = Config->MoistureSeedOffset;
		Snapshot.MoistureNoiseFrequency = Config->MoistureNoiseFrequency;
	}
	return Snapshot;
}

void FVoxelBiomeSnapshot::EvalBakedCurves(
	const TArray<float>& HeightCurve,
	const TArray<float>& ScaleCurve,
	float Continentalness,
	float& OutHeightOffset,
	float& OutHeightScaleMultiplier)
{
	// Identity modulation when the baked arrays are absent/degenerate (matches the former
	// UVoxelBiomeConfiguration::GetContinentalnessTerrainParams fallback).
	const int32 N = FMath::Min(HeightCurve.Num(), ScaleCurve.Num());
	if (N < 2)
	{
		OutHeightOffset = 0.0f;
		OutHeightScaleMultiplier = 1.0f;
		return;
	}

	// Map continentalness [-1,1] to float index [0, N-1], lerp between adjacent samples.
	// Deliberately no clamp on the input — bit-identical to the original generator math (an
	// out-of-range value extrapolates off the end samples exactly as before).
	const float FIdx = (Continentalness + 1.0f) * 0.5f * static_cast<float>(N - 1);
	const int32 Idx0 = FMath::Clamp(FMath::FloorToInt(FIdx), 0, N - 2);
	const float Frac = FIdx - static_cast<float>(Idx0);

	OutHeightOffset = FMath::Lerp(HeightCurve[Idx0], HeightCurve[Idx0 + 1], Frac);
	OutHeightScaleMultiplier = FMath::Lerp(ScaleCurve[Idx0], ScaleCurve[Idx0 + 1], Frac);
}

// ---------------------------------------------------------------------------
// Biome / material selection cores — moved verbatim from UVoxelBiomeConfiguration
// (which now delegates here). Any algorithm change happens in THIS file only.
// ---------------------------------------------------------------------------

const FBiomeDefinition* FVoxelBiomeSnapshot::FindBiome(const TArray<FBiomeDefinition>& InBiomes, uint8 BiomeID)
{
	for (const FBiomeDefinition& Biome : InBiomes)
	{
		if (Biome.BiomeID == BiomeID)
		{
			return &Biome;
		}
	}
	return nullptr;
}

FBiomeBlend FVoxelBiomeSnapshot::ComputeBiomeBlend(
	const TArray<FBiomeDefinition>& InBiomes, float InBiomeBlendWidth,
	float Temperature, float Moisture, float Continentalness)
{
	if (InBiomes.Num() == 0)
	{
		return FBiomeBlend(0);
	}

	// Clamp values to valid range
	Temperature = FMath::Clamp(Temperature, -1.0f, 1.0f);
	Moisture = FMath::Clamp(Moisture, -1.0f, 1.0f);
	Continentalness = FMath::Clamp(Continentalness, -1.0f, 1.0f);

	// Ensure minimum blend width
	const float EffectiveBlendWidth = FMath::Max(InBiomeBlendWidth, 0.01f);

	// Tiered blending: continentalness is a hard gate with soft edges,
	// temperature/moisture is the selector within the filtered set.
	struct FBiomeWeight
	{
		uint8 BiomeID;
		float Weight;
	};
	TArray<FBiomeWeight> CandidateBiomes;
	CandidateBiomes.Reserve(InBiomes.Num());

	for (const FBiomeDefinition& Biome : InBiomes)
	{
		// Step 1: Continentalness gate — hard exclusion outside blend zone
		const float ContDist = Biome.GetContinentalnessSignedDistance(Continentalness);
		if (ContDist < -EffectiveBlendWidth)
		{
			continue; // Excluded by continentalness gate
		}

		// Step 2: Continentalness factor — smoothstep from 0 at -BlendWidth to 1 at +BlendWidth
		float ContFactor;
		if (ContDist >= EffectiveBlendWidth)
		{
			ContFactor = 1.0f;
		}
		else
		{
			const float T = (ContDist + EffectiveBlendWidth) / (2.0f * EffectiveBlendWidth);
			ContFactor = T * T * (3.0f - 2.0f * T); // smoothstep
		}

		// Step 3: Temperature/Moisture weight — 2D signed distance with smoothstep
		const float TMDist = Biome.GetSignedDistanceToEdge2D(Temperature, Moisture);
		float TMWeight;
		if (TMDist >= EffectiveBlendWidth)
		{
			TMWeight = 1.0f;
		}
		else if (TMDist > -EffectiveBlendWidth)
		{
			const float T = (TMDist + EffectiveBlendWidth) / (2.0f * EffectiveBlendWidth);
			TMWeight = T * T * (3.0f - 2.0f * T); // smoothstep
		}
		else
		{
			TMWeight = 0.0f;
		}

		// Step 4: Combined weight = TM selection * continentalness gate * priority boost
		// Priority boost ensures higher-priority biomes dominate in blend zones.
		// Uses exponential scaling so high-priority biomes strongly outweigh low-priority
		// in contested blend regions. Priority 0 → 1.0x, Priority 10 → 4.0x.
		const float PriorityBoost = FMath::Pow(2.0f, Biome.SelectionPriority * 0.2f);
		const float FinalWeight = TMWeight * ContFactor * PriorityBoost;

		if (FinalWeight > 0.001f)
		{
			CandidateBiomes.Add({ Biome.BiomeID, FinalWeight });
		}
	}

	// If no biomes match, return the first biome
	if (CandidateBiomes.Num() == 0)
	{
		return FBiomeBlend(InBiomes[0].BiomeID);
	}

	// Sort by weight (descending)
	CandidateBiomes.Sort([](const FBiomeWeight& A, const FBiomeWeight& B) {
		return A.Weight > B.Weight;
	});

	// Build the blend result (limited to MAX_BIOME_BLEND)
	FBiomeBlend Result;
	Result.BiomeCount = FMath::Min(CandidateBiomes.Num(), MAX_BIOME_BLEND);

	for (int32 i = 0; i < Result.BiomeCount; ++i)
	{
		Result.BiomeIDs[i] = CandidateBiomes[i].BiomeID;
		Result.Weights[i] = CandidateBiomes[i].Weight;
	}

	// Normalize weights to sum to 1.0
	Result.NormalizeWeights();

	return Result;
}

uint8 FVoxelBiomeSnapshot::SelectBlendedMaterial(
	const TArray<FBiomeDefinition>& InBiomes, const FBiomeBlend& Blend, float DepthBelowSurface)
{
	// For single biome or dominant biome, use simple lookup
	if (Blend.BiomeCount == 1 || Blend.Weights[0] > 0.9f)
	{
		const FBiomeDefinition* Biome = FindBiome(InBiomes, Blend.BiomeIDs[0]);
		return Biome ? Biome->GetMaterialAtDepth(DepthBelowSurface) : 0;
	}

	// For blended biomes, use weighted random selection
	// This creates a dithered blend effect that looks more natural than hard edges
	float RandomValue = FMath::Frac(
		Blend.Weights[0] * 17.3f +
		Blend.Weights[1] * 31.7f +
		DepthBelowSurface * 0.1f
	);

	// Walk through weights to select biome
	float CumulativeWeight = 0.0f;
	for (int32 i = 0; i < Blend.BiomeCount; ++i)
	{
		CumulativeWeight += Blend.Weights[i];
		if (RandomValue < CumulativeWeight)
		{
			const FBiomeDefinition* Biome = FindBiome(InBiomes, Blend.BiomeIDs[i]);
			return Biome ? Biome->GetMaterialAtDepth(DepthBelowSurface) : 0;
		}
	}

	// Fallback to dominant biome
	const FBiomeDefinition* Biome = FindBiome(InBiomes, Blend.BiomeIDs[0]);
	return Biome ? Biome->GetMaterialAtDepth(DepthBelowSurface) : 0;
}

uint8 FVoxelBiomeSnapshot::SelectBlendedMaterialWithWater(
	const TArray<FBiomeDefinition>& InBiomes, const FBiomeBlend& Blend, float DepthBelowSurface,
	float TerrainSurfaceHeight, float WaterLevel,
	bool bInEnableUnderwaterMaterials, uint8 InDefaultUnderwaterMaterial)
{
	// Determine if terrain surface is underwater
	const bool bIsUnderwater = bInEnableUnderwaterMaterials && (TerrainSurfaceHeight < WaterLevel);

	// For single biome or dominant biome, use simple lookup
	if (Blend.BiomeCount == 1 || Blend.Weights[0] > 0.9f)
	{
		const FBiomeDefinition* Biome = FindBiome(InBiomes, Blend.BiomeIDs[0]);
		if (Biome)
		{
			return Biome->GetMaterialAtDepth(DepthBelowSurface, bIsUnderwater);
		}
		return bIsUnderwater ? InDefaultUnderwaterMaterial : 0;
	}

	// For blended biomes, use weighted random selection
	float RandomValue = FMath::Frac(
		Blend.Weights[0] * 17.3f +
		Blend.Weights[1] * 31.7f +
		DepthBelowSurface * 0.1f
	);

	// Walk through weights to select biome
	float CumulativeWeight = 0.0f;
	for (int32 i = 0; i < Blend.BiomeCount; ++i)
	{
		CumulativeWeight += Blend.Weights[i];
		if (RandomValue < CumulativeWeight)
		{
			const FBiomeDefinition* Biome = FindBiome(InBiomes, Blend.BiomeIDs[i]);
			if (Biome)
			{
				return Biome->GetMaterialAtDepth(DepthBelowSurface, bIsUnderwater);
			}
			return bIsUnderwater ? InDefaultUnderwaterMaterial : 0;
		}
	}

	// Fallback to dominant biome
	const FBiomeDefinition* Biome = FindBiome(InBiomes, Blend.BiomeIDs[0]);
	if (Biome)
	{
		return Biome->GetMaterialAtDepth(DepthBelowSurface, bIsUnderwater);
	}
	return bIsUnderwater ? InDefaultUnderwaterMaterial : 0;
}

uint8 FVoxelBiomeSnapshot::ApplyHeightRules(
	const TArray<FHeightMaterialRule>& InSortedRules, uint8 CurrentMaterial,
	float WorldHeight, float DepthBelowSurface)
{
	// Check rules in priority order (caller guarantees the sort)
	for (const FHeightMaterialRule& Rule : InSortedRules)
	{
		if (Rule.Applies(WorldHeight, DepthBelowSurface))
		{
			return Rule.MaterialID;
		}
	}
	return CurrentMaterial;
}
