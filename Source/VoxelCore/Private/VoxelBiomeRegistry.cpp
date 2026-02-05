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

	// Plains - Temperate, moderate moisture
	// Temperature: -0.3 to 0.6 (cool to warm)
	// Moisture: -0.5 to 0.5 (semi-arid to semi-humid)
	Biomes.Add(FBiomeDefinition(
		EVoxelBiome::Plains,
		TEXT("Plains"),
		FVector2D(-0.3, 0.6),    // Temperature range
		FVector2D(-0.5, 0.5),    // Moisture range
		EVoxelMaterial::Grass,    // Surface
		EVoxelMaterial::Dirt,     // Subsurface
		EVoxelMaterial::Stone     // Deep
	));

	// Desert - Hot and dry
	// Temperature: 0.5 to 1.0 (hot)
	// Moisture: -1.0 to 0.0 (arid)
	Biomes.Add(FBiomeDefinition(
		EVoxelBiome::Desert,
		TEXT("Desert"),
		FVector2D(0.5, 1.0),      // Temperature range
		FVector2D(-1.0, 0.0),     // Moisture range
		EVoxelMaterial::Sand,     // Surface
		EVoxelMaterial::Sandstone,// Subsurface
		EVoxelMaterial::Stone     // Deep
	));

	// Tundra - Cold (any moisture)
	// Temperature: -1.0 to -0.3 (freezing to cold)
	// Moisture: -1.0 to 1.0 (any)
	Biomes.Add(FBiomeDefinition(
		EVoxelBiome::Tundra,
		TEXT("Tundra"),
		FVector2D(-1.0, -0.3),    // Temperature range
		FVector2D(-1.0, 1.0),     // Moisture range (any)
		EVoxelMaterial::Snow,     // Surface
		EVoxelMaterial::FrozenDirt,// Subsurface
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
	// Tundra first (cold overrides everything)
	if (Temperature <= -0.3f)
	{
		return &Biomes[EVoxelBiome::Tundra];
	}

	// Desert (hot and dry)
	if (Temperature >= 0.5f && Moisture <= 0.0f)
	{
		return &Biomes[EVoxelBiome::Desert];
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
