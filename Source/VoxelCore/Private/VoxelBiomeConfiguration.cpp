// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBiomeConfiguration.h"
#include "VoxelCore.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelBiomeDefinition.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

UVoxelBiomeConfiguration::UVoxelBiomeConfiguration()
{
	// Start with default biomes
	InitializeDefaults();
}

void UVoxelBiomeConfiguration::PostLoad()
{
	Super::PostLoad();

	// Rebuild caches after deserialization overwrites UPROPERTY arrays.
	// The constructor calls InitializeDefaults() which builds caches for the
	// default biomes/ores. Serialization then overwrites Biomes, GlobalOreVeins,
	// and HeightMaterialRules with the user's data, but the cached arrays
	// (BiomeIDToIndex, SortedGlobalOres, SortedHeightRules) are NOT UPROPERTYs
	// and still hold the stale constructor values. Rebuild them now.
	RebuildBiomeIndexCache();
	RebuildHeightRulesCache();
	RebuildOreVeinsCache();
	RebuildBakedCurves();
}

void UVoxelBiomeConfiguration::InitializeDefaults()
{
	Biomes.Empty();

	// Plains - Temperate, moderate moisture (default/fallback biome)
	// Wide temperature tolerance, low-to-moderate moisture. Flat grasslands.
	FBiomeDefinition Plains;
	Plains.BiomeID = 0;
	Plains.Name = TEXT("Plains");
	Plains.TemperatureRange = FVector2D(-0.3, 0.7);
	Plains.MoistureRange = FVector2D(-0.5, 0.3);
	Plains.ContinentalnessRange = FVector2D(-0.05, 0.8); // Coastal to mid-inland
	Plains.SelectionPriority = 0; // Fallback biome
	Plains.SurfaceMaterial = EVoxelMaterial::Grass;
	Plains.SubsurfaceMaterial = EVoxelMaterial::Dirt;
	Plains.DeepMaterial = EVoxelMaterial::Stone;
	Plains.SurfaceDepth = 1.0f;
	Plains.SubsurfaceDepth = 4.0f;
	Plains.UnderwaterSurfaceMaterial = EVoxelMaterial::Sand;
	Plains.UnderwaterSubsurfaceMaterial = EVoxelMaterial::Sand;
	Biomes.Add(Plains);

	// Forest - Lush, humid areas with dense vegetation
	// Similar temperature to plains but requires higher moisture.
	FBiomeDefinition Forest;
	Forest.BiomeID = 1;
	Forest.Name = TEXT("Forest");
	Forest.TemperatureRange = FVector2D(-0.4, 0.7);
	Forest.MoistureRange = FVector2D(0.2, 1.0);
	Forest.ContinentalnessRange = FVector2D(-0.05, 1.0); // Near-coast to deep inland
	Forest.SelectionPriority = 3;
	Forest.SurfaceMaterial = EVoxelMaterial::Grass;
	Forest.SubsurfaceMaterial = EVoxelMaterial::Dirt;
	Forest.DeepMaterial = EVoxelMaterial::Stone;
	Forest.SurfaceDepth = 1.0f;
	Forest.SubsurfaceDepth = 5.0f; // Thicker soil layer in forests
	Forest.UnderwaterSurfaceMaterial = EVoxelMaterial::Dirt;
	Forest.UnderwaterSubsurfaceMaterial = EVoxelMaterial::Dirt;
	Biomes.Add(Forest);

	// Mountain - Cold, rocky high-altitude terrain
	// Cold temperatures with high continentalness (deep inland / elevated).
	FBiomeDefinition Mountain;
	Mountain.BiomeID = 2;
	Mountain.Name = TEXT("Mountain");
	Mountain.TemperatureRange = FVector2D(-1.0, -0.1);
	Mountain.MoistureRange = FVector2D(-1.0, 1.0);
	Mountain.ContinentalnessRange = FVector2D(0.3, 1.0); // Deep inland only
	Mountain.SelectionPriority = 5;
	Mountain.SurfaceMaterial = EVoxelMaterial::Stone;
	Mountain.SubsurfaceMaterial = EVoxelMaterial::Stone;
	Mountain.DeepMaterial = EVoxelMaterial::Stone;
	Mountain.SurfaceDepth = 1.0f;
	Mountain.SubsurfaceDepth = 3.0f;
	Mountain.UnderwaterSurfaceMaterial = EVoxelMaterial::Stone;
	Mountain.UnderwaterSubsurfaceMaterial = EVoxelMaterial::Stone;
	Biomes.Add(Mountain);

	// Ocean - Deep ocean to near-coast (all temperatures, all moisture)
	FBiomeDefinition Ocean;
	Ocean.BiomeID = 3;
	Ocean.Name = TEXT("Ocean");
	Ocean.TemperatureRange = FVector2D(-1.0, 1.0);
	Ocean.MoistureRange = FVector2D(-1.0, 1.0);
	Ocean.ContinentalnessRange = FVector2D(-1.0, -0.05); // Deep ocean to near-coast
	Ocean.SelectionPriority = 10;
	Ocean.SurfaceMaterial = EVoxelMaterial::Sand;
	Ocean.SubsurfaceMaterial = EVoxelMaterial::Sand;
	Ocean.DeepMaterial = EVoxelMaterial::Stone;
	Ocean.SurfaceDepth = 2.0f; // Thicker sand layer on ocean floor
	Ocean.SubsurfaceDepth = 5.0f;
	Ocean.UnderwaterSurfaceMaterial = EVoxelMaterial::Sand;
	Ocean.UnderwaterSubsurfaceMaterial = EVoxelMaterial::Sand;
	Biomes.Add(Ocean);

	// Setup default height material rules
	HeightMaterialRules.Empty();

	// Snow above snow line (high altitude peaks) - highest priority
	HeightMaterialRules.Add(FHeightMaterialRule(
		4000.0f,              // MinHeight
		MAX_FLT,              // MaxHeight
		EVoxelMaterial::Snow, // Material
		true,                 // bSurfaceOnly
		1.0f,                 // MaxDepthBelowSurface
		100                   // Priority
	));

	// Exposed rock at high altitude (just below snow line)
	HeightMaterialRules.Add(FHeightMaterialRule(
		3000.0f,               // MinHeight
		4000.0f,               // MaxHeight
		EVoxelMaterial::Stone, // Material
		true,                  // bSurfaceOnly
		2.0f,                  // MaxDepthBelowSurface
		50                     // Priority
	));

	// Setup default ore veins (Coal, Iron, Gold)
	GlobalOreVeins.Empty();

	// Coal - Common, medium depth, blob-shaped
	// MinDepth 12+ ensures ores stay below smooth terrain surface sampling range
	// NOTE: Using Stone (2) as placeholder until Coal textures are added to array index 10
	GlobalOreVeins.Add(FOreVeinConfig(
		TEXT("Coal"),
		EVoxelMaterial::Stone,   // MaterialID - use Stone until Coal textures added
		12.0f,                   // MinDepth (must be > 10 for smooth terrain)
		60.0f,                   // MaxDepth
		EOreVeinShape::Blob,     // Shape
		0.08f,                   // Frequency
		0.82f,                   // Threshold
		100,                     // SeedOffset
		10                       // Priority
	));

	// Iron - Moderate rarity, medium-deep, streak-shaped veins
	// NOTE: Using Stone (2) as placeholder until Iron textures are added to array index 11
	GlobalOreVeins.Add(FOreVeinConfig(
		TEXT("Iron"),
		EVoxelMaterial::Stone,   // MaterialID - use Stone until Iron textures added
		15.0f,                   // MinDepth
		100.0f,                  // MaxDepth
		EOreVeinShape::Streak,   // Shape (elongated veins)
		0.06f,                   // Frequency
		0.87f,                   // Threshold
		200,                     // SeedOffset
		20                       // Priority
	));

	// Gold - Rare, deep only, small blobs
	// NOTE: Using Sand (3) as placeholder until Gold textures are added to array index 12
	GlobalOreVeins.Add(FOreVeinConfig(
		TEXT("Gold"),
		EVoxelMaterial::Sand,    // MaterialID - use Sand until Gold textures added
		30.0f,                   // MinDepth (deep only)
		0.0f,                    // MaxDepth (no limit)
		EOreVeinShape::Blob,     // Shape
		0.04f,                   // Frequency
		0.93f,                   // Threshold (rare)
		300,                     // SeedOffset
		30                       // Priority (highest, checked first)
	));

	// Initialize continentalness curves with default keys matching old 3-point piecewise linear
	{
		FRichCurve* HeightCurvePtr = const_cast<FRichCurve*>(ContinentalnessHeightCurve.GetRichCurveConst());
		HeightCurvePtr->Reset();
		HeightCurvePtr->AddKey(-1.0f, -3000.0f);
		HeightCurvePtr->AddKey(0.0f, 0.0f);
		HeightCurvePtr->AddKey(1.0f, 1000.0f);
		for (auto It = HeightCurvePtr->GetKeyHandleIterator(); It; ++It)
		{
			HeightCurvePtr->SetKeyInterpMode(*It, RCIM_Linear);
		}

		FRichCurve* ScaleCurvePtr = const_cast<FRichCurve*>(ContinentalnessHeightScaleCurve.GetRichCurveConst());
		ScaleCurvePtr->Reset();
		ScaleCurvePtr->AddKey(-1.0f, 0.2f);
		ScaleCurvePtr->AddKey(1.0f, 1.0f);
		for (auto It = ScaleCurvePtr->GetKeyHandleIterator(); It; ++It)
		{
			ScaleCurvePtr->SetKeyInterpMode(*It, RCIM_Linear);
		}
	}

	// Eagerly rebuild caches so they're ready for worker threads (not thread-safe to rebuild lazily)
	RebuildBiomeIndexCache();
	RebuildHeightRulesCache();
	RebuildOreVeinsCache();
	RebuildBakedCurves();
}

void UVoxelBiomeConfiguration::RebuildBakedCurves()
{
	const int32 N = CONTINENTALNESS_CURVE_SAMPLE_COUNT;
	BakedHeightCurve.SetNum(N);
	BakedHeightScaleCurve.SetNum(N);

	const FRichCurve* HeightCurve = ContinentalnessHeightCurve.GetRichCurveConst();
	const FRichCurve* ScaleCurve = ContinentalnessHeightScaleCurve.GetRichCurveConst();

	for (int32 i = 0; i < N; ++i)
	{
		// Evenly space samples across [-1, 1]
		const float T = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(N - 1);
		BakedHeightCurve[i] = HeightCurve ? HeightCurve->Eval(T) : 0.0f;
		BakedHeightScaleCurve[i] = ScaleCurve ? ScaleCurve->Eval(T) : 1.0f;
	}
}

void UVoxelBiomeConfiguration::GetContinentalnessTerrainParams(float Continentalness, float& OutHeightOffset, float& OutHeightScaleMultiplier) const
{
	const int32 N = CONTINENTALNESS_CURVE_SAMPLE_COUNT;

	// Fallback if baked arrays are empty (shouldn't happen after proper init)
	if (BakedHeightCurve.Num() < N || BakedHeightScaleCurve.Num() < N)
	{
		OutHeightOffset = 0.0f;
		OutHeightScaleMultiplier = 1.0f;
		return;
	}

	// Map continentalness [-1,1] to float index [0, N-1], lerp between adjacent samples
	const float FIdx = (Continentalness + 1.0f) * 0.5f * static_cast<float>(N - 1);
	const int32 Idx0 = FMath::Clamp(FMath::FloorToInt(FIdx), 0, N - 2);
	const float Frac = FIdx - static_cast<float>(Idx0);

	OutHeightOffset = FMath::Lerp(BakedHeightCurve[Idx0], BakedHeightCurve[Idx0 + 1], Frac);
	OutHeightScaleMultiplier = FMath::Lerp(BakedHeightScaleCurve[Idx0], BakedHeightScaleCurve[Idx0 + 1], Frac);
}

void UVoxelBiomeConfiguration::RebuildBiomeIndexCache() const
{
	BiomeIDToIndex.Empty();
	for (int32 i = 0; i < Biomes.Num(); ++i)
	{
		BiomeIDToIndex.Add(Biomes[i].BiomeID, i);
	}
	bBiomeIndexCacheDirty = false;
}

void UVoxelBiomeConfiguration::RebuildHeightRulesCache() const
{
	SortedHeightRules = HeightMaterialRules;
	SortedHeightRules.Sort([](const FHeightMaterialRule& A, const FHeightMaterialRule& B) {
		return A.Priority > B.Priority; // Higher priority first
	});
	bHeightRulesCacheDirty = false;
}

void UVoxelBiomeConfiguration::RebuildOreVeinsCache() const
{
	SortedGlobalOres = GlobalOreVeins;
	SortedGlobalOres.Sort([](const FOreVeinConfig& A, const FOreVeinConfig& B) {
		return A.Priority > B.Priority; // Higher priority first
	});
	bOreVeinsCacheDirty = false;
}

void UVoxelBiomeConfiguration::GetOreVeinsForBiome(uint8 BiomeID, TArray<FOreVeinConfig>& OutOres) const
{
	OutOres.Empty();

	if (!bEnableOreVeins)
	{
		return;
	}

	// Rebuild cache if needed
	if (bOreVeinsCacheDirty)
	{
		RebuildOreVeinsCache();
	}

	// Get the biome definition
	const FBiomeDefinition* Biome = GetBiome(BiomeID);

	if (Biome && Biome->BiomeOreVeins.Num() > 0)
	{
		// Biome has its own ores
		if (Biome->bAddToGlobalOres)
		{
			// Combine biome ores with global ores
			OutOres = SortedGlobalOres;
			OutOres.Append(Biome->BiomeOreVeins);

			// Re-sort by priority
			OutOres.Sort([](const FOreVeinConfig& A, const FOreVeinConfig& B) {
				return A.Priority > B.Priority;
			});
		}
		else
		{
			// Biome ores replace global ores
			OutOres = Biome->BiomeOreVeins;
			OutOres.Sort([](const FOreVeinConfig& A, const FOreVeinConfig& B) {
				return A.Priority > B.Priority;
			});
		}
	}
	else
	{
		// Use global ores
		OutOres = SortedGlobalOres;
	}
}

const FBiomeDefinition* UVoxelBiomeConfiguration::GetBiome(uint8 BiomeID) const
{
	if (bBiomeIndexCacheDirty)
	{
		RebuildBiomeIndexCache();
	}

	const int32* IndexPtr = BiomeIDToIndex.Find(BiomeID);
	if (IndexPtr && *IndexPtr < Biomes.Num())
	{
		return &Biomes[*IndexPtr];
	}

	return nullptr;
}

const FBiomeDefinition* UVoxelBiomeConfiguration::SelectBiome(float Temperature, float Moisture, float Continentalness) const
{
	if (Biomes.Num() == 0)
	{
		return nullptr;
	}

	// Clamp values to valid range
	Temperature = FMath::Clamp(Temperature, -1.0f, 1.0f);
	Moisture = FMath::Clamp(Moisture, -1.0f, 1.0f);
	Continentalness = FMath::Clamp(Continentalness, -1.0f, 1.0f);

	// Data-driven priority selection: filter by containment, return highest SelectionPriority
	const FBiomeDefinition* BestBiome = nullptr;
	int32 BestPriority = INT_MIN;

	for (const FBiomeDefinition& Biome : Biomes)
	{
		if (Biome.Contains(Temperature, Moisture, Continentalness) && Biome.SelectionPriority > BestPriority)
		{
			BestBiome = &Biome;
			BestPriority = Biome.SelectionPriority;
		}
	}

	// Fallback to first biome if no containment match
	return BestBiome ? BestBiome : &Biomes[0];
}

uint8 UVoxelBiomeConfiguration::SelectBiomeID(float Temperature, float Moisture, float Continentalness) const
{
	const FBiomeDefinition* Biome = SelectBiome(Temperature, Moisture, Continentalness);
	return Biome ? Biome->BiomeID : 0;
}

FBiomeBlend UVoxelBiomeConfiguration::GetBiomeBlend(float Temperature, float Moisture, float Continentalness) const
{
	if (Biomes.Num() == 0)
	{
		return FBiomeBlend(0);
	}

	// Clamp values to valid range
	Temperature = FMath::Clamp(Temperature, -1.0f, 1.0f);
	Moisture = FMath::Clamp(Moisture, -1.0f, 1.0f);
	Continentalness = FMath::Clamp(Continentalness, -1.0f, 1.0f);

	// Ensure minimum blend width
	const float EffectiveBlendWidth = FMath::Max(BiomeBlendWidth, 0.01f);

	// Tiered blending: continentalness is a hard gate with soft edges,
	// temperature/moisture is the selector within the filtered set.
	struct FBiomeWeight
	{
		uint8 BiomeID;
		float Weight;
	};
	TArray<FBiomeWeight> CandidateBiomes;
	CandidateBiomes.Reserve(Biomes.Num());

	for (const FBiomeDefinition& Biome : Biomes)
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
		return FBiomeBlend(Biomes[0].BiomeID);
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

uint8 UVoxelBiomeConfiguration::GetBlendedMaterial(const FBiomeBlend& Blend, float DepthBelowSurface) const
{
	// For single biome or dominant biome, use simple lookup
	if (Blend.BiomeCount == 1 || Blend.Weights[0] > 0.9f)
	{
		const FBiomeDefinition* Biome = GetBiome(Blend.BiomeIDs[0]);
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
			const FBiomeDefinition* Biome = GetBiome(Blend.BiomeIDs[i]);
			return Biome ? Biome->GetMaterialAtDepth(DepthBelowSurface) : 0;
		}
	}

	// Fallback to dominant biome
	const FBiomeDefinition* Biome = GetBiome(Blend.BiomeIDs[0]);
	return Biome ? Biome->GetMaterialAtDepth(DepthBelowSurface) : 0;
}

uint8 UVoxelBiomeConfiguration::GetBlendedMaterialWithWater(const FBiomeBlend& Blend, float DepthBelowSurface,
	float TerrainSurfaceHeight, float WaterLevel) const
{
	// Determine if terrain surface is underwater
	const bool bIsUnderwater = bEnableUnderwaterMaterials && (TerrainSurfaceHeight < WaterLevel);

	// For single biome or dominant biome, use simple lookup
	if (Blend.BiomeCount == 1 || Blend.Weights[0] > 0.9f)
	{
		const FBiomeDefinition* Biome = GetBiome(Blend.BiomeIDs[0]);
		if (Biome)
		{
			return Biome->GetMaterialAtDepth(DepthBelowSurface, bIsUnderwater);
		}
		return bIsUnderwater ? DefaultUnderwaterMaterial : 0;
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
			const FBiomeDefinition* Biome = GetBiome(Blend.BiomeIDs[i]);
			if (Biome)
			{
				return Biome->GetMaterialAtDepth(DepthBelowSurface, bIsUnderwater);
			}
			return bIsUnderwater ? DefaultUnderwaterMaterial : 0;
		}
	}

	// Fallback to dominant biome
	const FBiomeDefinition* Biome = GetBiome(Blend.BiomeIDs[0]);
	if (Biome)
	{
		return Biome->GetMaterialAtDepth(DepthBelowSurface, bIsUnderwater);
	}
	return bIsUnderwater ? DefaultUnderwaterMaterial : 0;
}

uint8 UVoxelBiomeConfiguration::ApplyHeightMaterialRules(uint8 CurrentMaterial, float WorldHeight, float DepthBelowSurface) const
{
	if (!bEnableHeightMaterials || HeightMaterialRules.Num() == 0)
	{
		return CurrentMaterial;
	}

	if (bHeightRulesCacheDirty)
	{
		RebuildHeightRulesCache();
	}

	// Check rules in priority order (cached sorted)
	for (const FHeightMaterialRule& Rule : SortedHeightRules)
	{
		if (Rule.Applies(WorldHeight, DepthBelowSurface))
		{
			return Rule.MaterialID;
		}
	}

	return CurrentMaterial;
}

bool UVoxelBiomeConfiguration::IsValid() const
{
	return Biomes.Num() > 0;
}

void UVoxelBiomeConfiguration::LogConfiguration() const
{
	UE_LOG(LogVoxelCore, Warning, TEXT("========== BiomeConfiguration Dump =========="));
	UE_LOG(LogVoxelCore, Warning, TEXT("Asset: %s"), *GetPathName());
	UE_LOG(LogVoxelCore, Warning, TEXT("BlendWidth=%.3f, HeightMaterials=%s, OreVeins=%s, UnderwaterMaterials=%s"),
		BiomeBlendWidth, bEnableHeightMaterials ? TEXT("ON") : TEXT("OFF"),
		bEnableOreVeins ? TEXT("ON") : TEXT("OFF"), bEnableUnderwaterMaterials ? TEXT("ON") : TEXT("OFF"));
	UE_LOG(LogVoxelCore, Warning, TEXT("TempFreq=%.7f, MoistFreq=%.7f, TempSeed=%d, MoistSeed=%d"),
		TemperatureNoiseFrequency, MoistureNoiseFrequency, TemperatureSeedOffset, MoistureSeedOffset);
	if (bEnableContinentalness)
	{
		const FRichCurve* HC = ContinentalnessHeightCurve.GetRichCurveConst();
		const FRichCurve* SC = ContinentalnessHeightScaleCurve.GetRichCurveConst();
		const int32 HKeys = HC ? HC->GetNumKeys() : 0;
		const int32 SKeys = SC ? SC->GetNumKeys() : 0;
		float HMin = 0.0f, HMax = 0.0f, SMin = 1.0f, SMax = 1.0f;
		if (BakedHeightCurve.Num() > 0)
		{
			HMin = FMath::Min(BakedHeightCurve);
			HMax = FMath::Max(BakedHeightCurve);
		}
		if (BakedHeightScaleCurve.Num() > 0)
		{
			SMin = FMath::Min(BakedHeightScaleCurve);
			SMax = FMath::Max(BakedHeightScaleCurve);
		}
		UE_LOG(LogVoxelCore, Warning, TEXT("Continentalness: Freq=%.7f, Seed=%d, HeightCurve(%d keys, range %.0f..%.0f), ScaleCurve(%d keys, range %.2f..%.2f)"),
			ContinentalnessNoiseFrequency, ContinentalnessSeedOffset,
			HKeys, HMin, HMax, SKeys, SMin, SMax);
	}

	UE_LOG(LogVoxelCore, Warning, TEXT("--- Biomes (%d) ---"), Biomes.Num());
	for (const FBiomeDefinition& B : Biomes)
	{
		UE_LOG(LogVoxelCore, Warning, TEXT("  [%d] %s (Pri=%d): Temp(%.2f..%.2f) Moist(%.2f..%.2f) Cont(%.2f..%.2f)"),
			B.BiomeID, *B.Name, B.SelectionPriority, B.TemperatureRange.X, B.TemperatureRange.Y,
			B.MoistureRange.X, B.MoistureRange.Y,
			B.ContinentalnessRange.X, B.ContinentalnessRange.Y);
		UE_LOG(LogVoxelCore, Warning, TEXT("       Surface=%d Subsurface=%d Deep=%d  SurfDepth=%.1f SubDepth=%.1f"),
			B.SurfaceMaterial, B.SubsurfaceMaterial, B.DeepMaterial,
			B.SurfaceDepth, B.SubsurfaceDepth);
		UE_LOG(LogVoxelCore, Warning, TEXT("       UnderwaterSurf=%d UnderwaterSub=%d  BiomeOres=%d AddToGlobal=%s"),
			B.UnderwaterSurfaceMaterial, B.UnderwaterSubsurfaceMaterial,
			B.BiomeOreVeins.Num(), B.bAddToGlobalOres ? TEXT("Y") : TEXT("N"));
	}

	UE_LOG(LogVoxelCore, Warning, TEXT("--- Height Rules (%d) ---"), HeightMaterialRules.Num());
	for (const FHeightMaterialRule& R : HeightMaterialRules)
	{
		UE_LOG(LogVoxelCore, Warning, TEXT("  Mat=%d Height(%.0f..%.0f) SurfOnly=%s MaxDepth=%.1f Priority=%d"),
			R.MaterialID, R.MinHeight, R.MaxHeight,
			R.bSurfaceOnly ? TEXT("Y") : TEXT("N"), R.MaxDepthBelowSurface, R.Priority);
	}

	UE_LOG(LogVoxelCore, Warning, TEXT("--- Global Ore Veins (%d) ---"), GlobalOreVeins.Num());
	for (const FOreVeinConfig& O : GlobalOreVeins)
	{
		UE_LOG(LogVoxelCore, Warning, TEXT("  %s: Mat=%d Depth(%.0f..%.0f) Shape=%s Freq=%.4f Thresh=%.3f Seed=%d Pri=%d"),
			*O.Name, O.MaterialID, O.MinDepth, O.MaxDepth,
			O.Shape == EOreVeinShape::Blob ? TEXT("Blob") : TEXT("Streak"),
			O.Frequency, O.Threshold, O.SeedOffset, O.Priority);
	}

	UE_LOG(LogVoxelCore, Warning, TEXT("========== End BiomeConfiguration Dump =========="));
}

#if WITH_EDITOR
EDataValidationResult UVoxelBiomeConfiguration::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (Biomes.Num() == 0)
	{
		Context.AddError(FText::FromString(TEXT("No biomes defined. Call InitializeDefaults() or add biomes manually.")));
		Result = EDataValidationResult::Invalid;
	}

	// Check for duplicate BiomeIDs
	TSet<uint8> SeenIDs;
	for (const FBiomeDefinition& Biome : Biomes)
	{
		if (SeenIDs.Contains(Biome.BiomeID))
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Duplicate BiomeID %d found. Each biome must have a unique ID."), Biome.BiomeID)));
			Result = EDataValidationResult::Invalid;
		}
		SeenIDs.Add(Biome.BiomeID);
	}

	// Validate temperature/moisture/continentalness ranges
	for (const FBiomeDefinition& Biome : Biomes)
	{
		if (Biome.TemperatureRange.X > Biome.TemperatureRange.Y)
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("Biome '%s' has invalid temperature range (min > max)."), *Biome.Name)));
		}
		if (Biome.MoistureRange.X > Biome.MoistureRange.Y)
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("Biome '%s' has invalid moisture range (min > max)."), *Biome.Name)));
		}
		if (Biome.ContinentalnessRange.X > Biome.ContinentalnessRange.Y)
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("Biome '%s' has invalid continentalness range (min > max)."), *Biome.Name)));
		}
	}

	// Validate height rules
	for (const FHeightMaterialRule& Rule : HeightMaterialRules)
	{
		if (Rule.MinHeight > Rule.MaxHeight)
		{
			Context.AddWarning(FText::FromString(FString::Printf(
				TEXT("Height rule has invalid range (min %.0f > max %.0f)."), Rule.MinHeight, Rule.MaxHeight)));
		}
	}

	return Result;
}

void UVoxelBiomeConfiguration::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Eagerly rebuild caches so they're ready for worker threads (not thread-safe to rebuild lazily)
	RebuildBiomeIndexCache();
	RebuildHeightRulesCache();
	RebuildOreVeinsCache();
	RebuildBakedCurves();
}
#endif
