// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBiomeRegistry.h"
#include "VoxelMaterialRegistry.h"

TArray<FBiomeDefinition> FVoxelBiomeRegistry::Biomes;
bool FVoxelBiomeRegistry::bInitialized = false;

void FVoxelBiomeRegistry::EnsureInitialized()
{
	if (bInitialized)
	{
		return;
	}

	Biomes.Empty();
	Biomes.Reserve(EVoxelBiome::Count);

	// Define biomes with their climate ranges and materials
	// Biomes are checked in order, so more specific ranges should come first

	// Plains - Temperate, moderate moisture (default/fallback biome)
	// Temperature: -0.3 to 0.7 (cool to warm)
	// Moisture: -0.5 to 0.3 (semi-arid to moderate)
	Biomes.Add(FBiomeDefinition(
		EVoxelBiome::Plains,
		TEXT("Plains"),
		FVector2D(-0.3, 0.7),    // Temperature range
		FVector2D(-0.5, 0.3),    // Moisture range
		EVoxelMaterial::Grass,    // Surface
		EVoxelMaterial::Dirt,     // Subsurface
		EVoxelMaterial::Stone     // Deep
	));

	// Forest - Lush, humid areas with dense vegetation
	// Temperature: -0.4 to 0.7 (wide range — forests in many climates)
	// Moisture: 0.2 to 1.0 (humid — forests need moisture)
	Biomes.Add(FBiomeDefinition(
		EVoxelBiome::Forest,
		TEXT("Forest"),
		FVector2D(-0.4, 0.7),     // Temperature range
		FVector2D(0.2, 1.0),      // Moisture range
		EVoxelMaterial::Grass,    // Surface (forest floor)
		EVoxelMaterial::Dirt,     // Subsurface
		EVoxelMaterial::Stone     // Deep
	));

	// Mountain - Cold, rocky high-altitude terrain
	// Temperature: -1.0 to -0.1 (cold — high elevation)
	// Moisture: -1.0 to 1.0 (any)
	Biomes.Add(FBiomeDefinition(
		EVoxelBiome::Mountain,
		TEXT("Mountain"),
		FVector2D(-1.0, -0.1),    // Temperature range
		FVector2D(-1.0, 1.0),     // Moisture range (any)
		EVoxelMaterial::Stone,    // Surface (rocky peaks)
		EVoxelMaterial::Stone,    // Subsurface
		EVoxelMaterial::Stone     // Deep
	));

	// Ocean - Deep water (placeholder for registry, full support via BiomeConfiguration)
	// Temperature: -1.0 to 1.0 (any)
	// Moisture: -1.0 to 1.0 (any)
	Biomes.Add(FBiomeDefinition(
		EVoxelBiome::Ocean,
		TEXT("Ocean"),
		FVector2D(-1.0, 1.0),     // Temperature range (any)
		FVector2D(-1.0, 1.0),     // Moisture range (any)
		EVoxelMaterial::Sand,     // Surface
		EVoxelMaterial::Sand,     // Subsurface
		EVoxelMaterial::Stone     // Deep
	));

	bInitialized = true;
}

const FBiomeDefinition* FVoxelBiomeRegistry::SelectBiome(float Temperature, float Moisture)
{
	EnsureInitialized();

	// Clamp values to valid range
	Temperature = FMath::Clamp(Temperature, -1.0f, 1.0f);
	Moisture = FMath::Clamp(Moisture, -1.0f, 1.0f);

	// Check biomes in priority order
	// Mountain first (cold overrides everything — high elevation)
	if (Temperature <= -0.1f)
	{
		return &Biomes[EVoxelBiome::Mountain];
	}

	// Forest (humid areas)
	if (Moisture >= 0.2f)
	{
		return &Biomes[EVoxelBiome::Forest];
	}

	// Default to Plains
	return &Biomes[EVoxelBiome::Plains];
}

uint8 FVoxelBiomeRegistry::SelectBiomeID(float Temperature, float Moisture)
{
	const FBiomeDefinition* Biome = SelectBiome(Temperature, Moisture);
	return Biome ? Biome->BiomeID : EVoxelBiome::Plains;
}

const FBiomeDefinition* FVoxelBiomeRegistry::GetBiome(uint8 BiomeID)
{
	EnsureInitialized();

	if (BiomeID < Biomes.Num())
	{
		return &Biomes[BiomeID];
	}

	return nullptr;
}

int32 FVoxelBiomeRegistry::GetBiomeCount()
{
	EnsureInitialized();
	return Biomes.Num();
}

const TArray<FBiomeDefinition>& FVoxelBiomeRegistry::GetAllBiomes()
{
	EnsureInitialized();
	return Biomes;
}

FBiomeBlend FVoxelBiomeRegistry::GetBiomeBlend(float Temperature, float Moisture, float BlendWidth)
{
	EnsureInitialized();

	// Clamp values to valid range
	Temperature = FMath::Clamp(Temperature, -1.0f, 1.0f);
	Moisture = FMath::Clamp(Moisture, -1.0f, 1.0f);

	// Ensure minimum blend width
	BlendWidth = FMath::Max(BlendWidth, 0.01f);

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
		// Inside biome: full weight if well inside, ramping down near edges
		// Outside biome: weight ramps down based on distance outside
		float Weight = 0.0f;

		if (SignedDist >= BlendWidth)
		{
			// Well inside this biome - full weight
			Weight = 1.0f;
		}
		else if (SignedDist > -BlendWidth)
		{
			// In the blend zone - smooth falloff using smoothstep
			float T = (SignedDist + BlendWidth) / (2.0f * BlendWidth);
			// Smoothstep for smoother transitions
			Weight = T * T * (3.0f - 2.0f * T);
		}
		// else: too far outside, weight = 0

		if (Weight > 0.001f)
		{
			CandidateBiomes.Add({ Biome.BiomeID, Weight });
		}
	}

	// If no biomes match, return the default (Plains)
	if (CandidateBiomes.Num() == 0)
	{
		return FBiomeBlend(EVoxelBiome::Plains);
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

uint8 FVoxelBiomeRegistry::GetBlendedMaterial(const FBiomeBlend& Blend, float DepthBelowSurface)
{
	EnsureInitialized();

	// For single biome or dominant biome, use simple lookup
	if (Blend.BiomeCount == 1 || Blend.Weights[0] > 0.9f)
	{
		const FBiomeDefinition* Biome = GetBiome(Blend.BiomeIDs[0]);
		return Biome ? Biome->GetMaterialAtDepth(DepthBelowSurface) : 0;
	}

	// For blended biomes, use weighted random selection
	// This creates a dithered blend effect that looks more natural than hard edges
	// Use a deterministic "random" based on the blend state for consistency
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
