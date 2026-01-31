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
