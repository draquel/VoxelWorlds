// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMaterialDefinition.h"

/**
 * Material ID constants for type-safe material references.
 */
namespace EVoxelMaterial
{
	// Base terrain materials
	constexpr uint8 Grass = 0;
	constexpr uint8 Dirt = 1;
	constexpr uint8 Stone = 2;
	constexpr uint8 Sand = 3;
	constexpr uint8 Snow = 4;
	constexpr uint8 Sandstone = 5;
	constexpr uint8 FrozenDirt = 6;

	// Ore materials (10-19 range)
	constexpr uint8 Coal = 10;
	constexpr uint8 Iron = 11;
	constexpr uint8 Gold = 12;
	constexpr uint8 Copper = 13;
	constexpr uint8 Diamond = 14;

	// Vegetation materials (20-29 range)
	constexpr uint8 Wood = 20;
	constexpr uint8 Leaves = 21;

	/** Total number of defined materials (base + ores + vegetation) */
	constexpr uint8 Count = 22;
}

struct FVoxelMaterialTextureConfig;

/**
 * Static registry of voxel material definitions.
 * Provides lookup of material properties by MaterialID.
 * The definitions are an immutable table: the defaults, built on the first query, or the table an atlas published
 * (SetAtlasPositions). Any thread may query with no lock and sees one table whole; no table is changed or freed once
 * published, so the pointers and references the queries return stay valid (G100).
 */
class VOXELCORE_API FVoxelMaterialRegistry
{
public:
	/**
	 * Get the material definition for a given ID.
	 * @param MaterialID The material ID to look up
	 * @return Pointer to the material definition (valid for the process), or nullptr if invalid
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
	 * @return The table published now, in MaterialID order (valid for the process; a later atlas publishes another)
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
	 * Check if a material uses masked (alpha cutout) blending.
	 * @param MaterialID The material ID to check
	 * @return true if the material has bIsMasked set
	 */
	static bool IsMaterialMasked(uint8 MaterialID);

	/**
	 * Check if a material is non-occluding (doesn't cull adjacent faces).
	 * @param MaterialID The material ID to check
	 * @return true if the material has bNonOccluding set
	 */
	static bool IsNonOccluding(uint8 MaterialID);

	/**
	 * How hard a block of a material is to break (world modes review WM-C14, D31): seconds bare-handed.
	 * @param MaterialID The material ID to look up
	 * @return The material's hardness (DefaultHardness for an unknown ID)
	 */
	static float GetHardness(uint8 MaterialID);

	/** The hardness of a material the registry does not know (and of the reserved slots). */
	static constexpr float DefaultHardness = 1.0f;

	/**
	 * Get the set of all MaterialIDs that use masked blending.
	 */
	static TSet<uint8> GetMaskedMaterialIDs();

	/**
	 * Publish the table of an atlas: the defaults, grown to the atlas's highest MaterialID with placeholders, with each
	 * configured material's atlas position, scales, name and flags from its config. A table is built and published whole,
	 * never changed in place (queries on other threads see the old table or the new one); an atlas whose table was
	 * published before republishes that table. Called by UVoxelRegionComponent::SetMaterialAtlas on the game thread.
	 * @param Configs Array of material texture configurations
	 * @param AtlasColumns Number of columns in the atlas
	 * @param AtlasRows Number of rows in the atlas
	 */
	static void SetAtlasPositions(const TArray<FVoxelMaterialTextureConfig>& Configs, int32 AtlasColumns, int32 AtlasRows);

private:
	/**
	 * The table published now: the last atlas's, or the defaults (built once on the first call, thread-safely).
	 * @return The material definitions in MaterialID order
	 */
	static const TArray<FVoxelMaterialDefinition>& Table();
};
