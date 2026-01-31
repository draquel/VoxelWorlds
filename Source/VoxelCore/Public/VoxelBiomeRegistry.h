// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelBiomeDefinition.h"

/**
 * Biome ID constants for type-safe biome references.
 */
namespace EVoxelBiome
{
	constexpr uint8 Plains = 0;
	constexpr uint8 Desert = 1;
	constexpr uint8 Tundra = 2;

	/** Total number of defined biomes */
	constexpr uint8 Count = 3;
}

/**
 * Static registry of biome definitions.
 * Provides biome selection based on temperature and moisture values.
 */
class VOXELCORE_API FVoxelBiomeRegistry
{
public:
	/**
	 * Select the appropriate biome for given climate values.
	 * @param Temperature Normalized temperature (-1 to 1)
	 * @param Moisture Normalized moisture (-1 to 1)
	 * @return Pointer to the selected biome definition, or Plains as fallback
	 */
	static const FBiomeDefinition* SelectBiome(float Temperature, float Moisture);

	/**
	 * Select the biome ID for given climate values.
	 * @param Temperature Normalized temperature (-1 to 1)
	 * @param Moisture Normalized moisture (-1 to 1)
	 * @return Biome ID
	 */
	static uint8 SelectBiomeID(float Temperature, float Moisture);

	/**
	 * Get a biome definition by ID.
	 * @param BiomeID The biome ID to look up
	 * @return Pointer to the biome definition, or nullptr if invalid
	 */
	static const FBiomeDefinition* GetBiome(uint8 BiomeID);

	/**
	 * Get the total number of registered biomes.
	 */
	static int32 GetBiomeCount();

	/**
	 * Get all registered biome definitions.
	 */
	static const TArray<FBiomeDefinition>& GetAllBiomes();

private:
	/** Array of all biome definitions */
	static TArray<FBiomeDefinition> Biomes;

	/** Whether the registry has been initialized */
	static bool bInitialized;

	/** Initialize the registry with default biomes */
	static void EnsureInitialized();
};
