// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBiomeConfiguration.h"
#include "VoxelCore.h"
#include "VoxelMaterialRegistry.h"

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

	// Mark caches as dirty
	bBiomeIndexCacheDirty = true;
	bHeightRulesCacheDirty = true;
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
}
#endif
