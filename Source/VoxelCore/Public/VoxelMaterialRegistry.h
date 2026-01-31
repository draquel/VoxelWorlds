// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMaterialDefinition.h"

/**
 * Material ID constants for type-safe material references.
 */
namespace EVoxelMaterial
{
	constexpr uint8 Grass = 0;
	constexpr uint8 Dirt = 1;
	constexpr uint8 Stone = 2;
	constexpr uint8 Sand = 3;
	constexpr uint8 Snow = 4;
	constexpr uint8 Sandstone = 5;
	constexpr uint8 FrozenDirt = 6;

	/** Total number of defined materials */
	constexpr uint8 Count = 7;
}

/**
 * Static registry of voxel material definitions.
 * Provides lookup of material properties by MaterialID.
 */
class VOXELCORE_API FVoxelMaterialRegistry
{
public:
	/**
	 * Get the material definition for a given ID.
	 * @param MaterialID The material ID to look up
	 * @return Pointer to the material definition, or nullptr if invalid
	 */
	static const FVoxelMaterialDefinition* GetMaterial(uint8 MaterialID);

	/**
	 * Get the color for a given material ID.
	 * @param MaterialID The material ID to look up
	 * @return The material's color, or magenta for invalid IDs
	 */
	static FColor GetMaterialColor(uint8 MaterialID);

	/**
	 * Get the total number of registered materials.
	 */
	static int32 GetMaterialCount();

	/**
	 * Get all registered material definitions.
	 */
	static const TArray<FVoxelMaterialDefinition>& GetAllMaterials();

private:
	/** Array of all material definitions */
	static TArray<FVoxelMaterialDefinition> Materials;

	/** Whether the registry has been initialized */
	static bool bInitialized;

	/** Initialize the registry with default materials */
	static void EnsureInitialized();
};
