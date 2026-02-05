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

struct FVoxelMaterialTextureConfig;

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

	// ===== Atlas Support =====

	/**
	 * Get UV offset for a material in the packed atlas.
	 * @param MaterialID The material ID to look up
	 * @param Columns Number of columns in the atlas grid
	 * @param Rows Number of rows in the atlas grid
	 * @return UV offset (0-1 range) for the material's tile
	 */
	static FVector2D GetAtlasUVOffset(uint8 MaterialID, int32 Columns, int32 Rows);

	/**
	 * Get the texture array index for a material.
	 * @param MaterialID The material ID to look up
	 * @return Index into the texture arrays (-1 if not found)
	 */
	static int32 GetArrayIndex(uint8 MaterialID);

	/**
	 * Get the triplanar scale for a material.
	 * @param MaterialID The material ID to look up
	 * @return Triplanar scale (defaults to 1.0 if not found)
	 */
	static float GetTriplanarScale(uint8 MaterialID);

	/**
	 * Get the UV scale for a material.
	 * @param MaterialID The material ID to look up
	 * @return UV scale (defaults to 1.0 if not found)
	 */
	static float GetUVScale(uint8 MaterialID);

	/**
	 * Update material atlas positions from texture configs.
	 * @param Configs Array of material texture configurations
	 * @param AtlasColumns Number of columns in the atlas
	 * @param AtlasRows Number of rows in the atlas
	 */
	static void SetAtlasPositions(const TArray<FVoxelMaterialTextureConfig>& Configs, int32 AtlasColumns, int32 AtlasRows);

private:
	/** Array of all material definitions */
	static TArray<FVoxelMaterialDefinition> Materials;

	/** Whether the registry has been initialized */
	static bool bInitialized;

	/** Initialize the registry with default materials */
	static void EnsureInitialized();
};
