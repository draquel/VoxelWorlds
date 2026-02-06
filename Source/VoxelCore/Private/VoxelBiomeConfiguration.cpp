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

void UVoxelBiomeConfiguration::InitializeDefaults()
{
	Biomes.Empty();

	// Plains - Temperate, moderate moisture (default/fallback biome)
	FBiomeDefinition Plains;
	Plains.BiomeID = 0;
	Plains.Name = TEXT("Plains");
	Plains.TemperatureRange = FVector2D(-0.3, 0.6);
	Plains.MoistureRange = FVector2D(-0.5, 0.5);
	Plains.SurfaceMaterial = EVoxelMaterial::Grass;
	Plains.SubsurfaceMaterial = EVoxelMaterial::Dirt;
	Plains.DeepMaterial = EVoxelMaterial::Stone;
	Plains.SurfaceDepth = 1.0f;
	Plains.SubsurfaceDepth = 4.0f;
	// Underwater: grass becomes sand
	Plains.UnderwaterSurfaceMaterial = EVoxelMaterial::Sand;
	Plains.UnderwaterSubsurfaceMaterial = EVoxelMaterial::Sand;
	Biomes.Add(Plains);

	// Desert - Hot and dry
	FBiomeDefinition Desert;
	Desert.BiomeID = 1;
	Desert.Name = TEXT("Desert");
	Desert.TemperatureRange = FVector2D(0.5, 1.0);
	Desert.MoistureRange = FVector2D(-1.0, 0.0);
	Desert.SurfaceMaterial = EVoxelMaterial::Sand;
	Desert.SubsurfaceMaterial = EVoxelMaterial::Sandstone;
	Desert.DeepMaterial = EVoxelMaterial::Stone;
	Desert.SurfaceDepth = 1.0f;
	Desert.SubsurfaceDepth = 4.0f;
	// Underwater: stays sand (naturally sandy)
	Desert.UnderwaterSurfaceMaterial = EVoxelMaterial::Sand;
	Desert.UnderwaterSubsurfaceMaterial = EVoxelMaterial::Sandstone;
	Biomes.Add(Desert);

	// Tundra - Cold (any moisture)
	FBiomeDefinition Tundra;
	Tundra.BiomeID = 2;
	Tundra.Name = TEXT("Tundra");
	Tundra.TemperatureRange = FVector2D(-1.0, -0.3);
	Tundra.MoistureRange = FVector2D(-1.0, 1.0);
	Tundra.SurfaceMaterial = EVoxelMaterial::Snow;
	Tundra.SubsurfaceMaterial = EVoxelMaterial::FrozenDirt;
	Tundra.DeepMaterial = EVoxelMaterial::Stone;
	Tundra.SurfaceDepth = 1.0f;
	Tundra.SubsurfaceDepth = 4.0f;
	// Underwater: stone/gravel look (cold water erodes to rock)
	Tundra.UnderwaterSurfaceMaterial = EVoxelMaterial::Stone;
	Tundra.UnderwaterSubsurfaceMaterial = EVoxelMaterial::Stone;
	Biomes.Add(Tundra);

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

	// Mark caches as dirty
	bBiomeIndexCacheDirty = true;
	bHeightRulesCacheDirty = true;
	bOreVeinsCacheDirty = true;
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

const FBiomeDefinition* UVoxelBiomeConfiguration::SelectBiome(float Temperature, float Moisture) const
{
	if (Biomes.Num() == 0)
	{
		return nullptr;
	}

	// Clamp values to valid range
	Temperature = FMath::Clamp(Temperature, -1.0f, 1.0f);
	Moisture = FMath::Clamp(Moisture, -1.0f, 1.0f);

	// Priority-based selection: Tundra (cold) > Desert (hot+dry) > Plains (default)
	// This matches the original FVoxelBiomeRegistry logic
	for (const FBiomeDefinition& Biome : Biomes)
	{
		// Check Tundra first (cold overrides everything)
		if (Biome.Name == TEXT("Tundra") && Temperature <= Biome.TemperatureRange.Y)
		{
			return &Biome;
		}
	}

	for (const FBiomeDefinition& Biome : Biomes)
	{
		// Check Desert (hot and dry)
		if (Biome.Name == TEXT("Desert") &&
			Temperature >= Biome.TemperatureRange.X &&
			Moisture <= Biome.MoistureRange.Y)
		{
			return &Biome;
		}
	}

	// Default to first biome (Plains)
	return &Biomes[0];
}

uint8 UVoxelBiomeConfiguration::SelectBiomeID(float Temperature, float Moisture) const
{
	const FBiomeDefinition* Biome = SelectBiome(Temperature, Moisture);
	return Biome ? Biome->BiomeID : 0;
}

FBiomeBlend UVoxelBiomeConfiguration::GetBiomeBlend(float Temperature, float Moisture) const
{
	if (Biomes.Num() == 0)
	{
		return FBiomeBlend(0);
	}

	// Clamp values to valid range
	Temperature = FMath::Clamp(Temperature, -1.0f, 1.0f);
	Moisture = FMath::Clamp(Moisture, -1.0f, 1.0f);

	// Ensure minimum blend width
	float EffectiveBlendWidth = FMath::Max(BiomeBlendWidth, 0.01f);

	// Calculate weights for all biomes based on distance to their edges
	struct FBiomeWeight
	{
		uint8 BiomeID;
		float Weight;
	};
	TArray<FBiomeWeight> CandidateBiomes;
	CandidateBiomes.Reserve(Biomes.Num());

	for (const FBiomeDefinition& Biome : Biomes)
	{
		// Get signed distance to biome edge (positive = inside, negative = outside)
		float SignedDist = Biome.GetSignedDistanceToEdge(Temperature, Moisture);

		// Calculate weight based on distance
		float Weight = 0.0f;

		if (SignedDist >= EffectiveBlendWidth)
		{
			// Well inside this biome - full weight
			Weight = 1.0f;
		}
		else if (SignedDist > -EffectiveBlendWidth)
		{
			// In the blend zone - smooth falloff using smoothstep
			float T = (SignedDist + EffectiveBlendWidth) / (2.0f * EffectiveBlendWidth);
			Weight = T * T * (3.0f - 2.0f * T);
		}

		if (Weight > 0.001f)
		{
			CandidateBiomes.Add({ Biome.BiomeID, Weight });
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

	// Validate temperature/moisture ranges
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

	// Mark caches as dirty when properties change
	bBiomeIndexCacheDirty = true;
	bHeightRulesCacheDirty = true;
	bOreVeinsCacheDirty = true;
}
#endif
