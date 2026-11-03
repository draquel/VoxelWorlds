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
 * An immutable table of voxel material definitions in MaterialID order: the built-in defaults (Defaults, what the
 * registry answers from), or a world's atlas's (FromAtlas: the defaults grown to the atlas's highest MaterialID, its
 * configs applied). A world holds its atlas's table (UVoxelChunkManager hands it to its meshers in FVoxelMeshingConfig)
 * and reads it on any thread with no lock; two worlds with different atlases each read their own, and a table goes with
 * the last world holding it (G101).
 */
class VOXELCORE_API FVoxelMaterialTable
{
public:
	/**
	 * The built-in definitions, built once on the first call (thread-safely).
	 * @return The defaults' table (valid for the process)
	 */
	static const FVoxelMaterialTable& Defaults();

	/**
	 * The table of an atlas: the defaults, grown to the atlas's highest MaterialID with placeholders, with each
	 * configured material's atlas position, scales, name and flags from its config (hardness and colour stay the
	 * defaults'). Built on the caller's thread; nothing global changes.
	 * @param Configs The atlas's material texture configurations
	 * @param AtlasColumns Number of columns in the atlas (placeholders' positions)
	 * @param AtlasRows Number of rows in the atlas
	 * @return The new table
	 */
	static TSharedRef<const FVoxelMaterialTable> FromAtlas(const TArray<FVoxelMaterialTextureConfig>& Configs, int32 AtlasColumns, int32 AtlasRows);

	/**
	 * @param MaterialID The material ID to look up
	 * @return The definition (valid while the table lives), or nullptr when the table does not define the ID
	 */
	const FVoxelMaterialDefinition* GetMaterial(uint8 MaterialID) const
	{
		return MaterialID < Materials.Num() ? &Materials[MaterialID] : nullptr;
	}

	/**
	 * @param MaterialID The material ID to check
	 * @return Whether the material is non-occluding (doesn't cull adjacent faces); false for an unknown ID
	 */
	bool IsNonOccluding(uint8 MaterialID) const
	{
		return MaterialID < Materials.Num() && Materials[MaterialID].bNonOccluding;
	}

	/**
	 * @param MaterialID The material ID to check
	 * @return Whether the material uses masked (alpha cutout) blending; false for an unknown ID
	 */
	bool IsMaterialMasked(uint8 MaterialID) const
	{
		return MaterialID < Materials.Num() && Materials[MaterialID].bIsMasked;
	}

	/**
	 * @param MaterialID The material ID to look up
	 * @return The material's colour, or magenta for an unknown ID
	 */
	FColor GetMaterialColor(uint8 MaterialID) const
	{
		return MaterialID < Materials.Num() ? Materials[MaterialID].Color : FColor::Magenta;
	}

	/** @return The number of materials the table defines (IDs 0 to Num() - 1) */
	int32 Num() const { return Materials.Num(); }

	/** @return The definitions in MaterialID order */
	const TArray<FVoxelMaterialDefinition>& GetAll() const { return Materials; }

	/** @return The non-occluding materials as "Name(ID), ...", for logs */
	FString DescribeNonOccluding() const;

	/**
	 * The slope rock's thresholds on the smooth terrain (visual polish VP2-O3), the slope being 1 - N.Up of the surface
	 * normal against local up: where the rock starts (about 23 degrees) and where it is whole (about 36). The shader's are
	 * VOXEL_SLOPE_ROCK_START and _END in Shaders/Private/VoxelSlopeRock.ush; test VoxelWorlds.Rendering.SlopeRock.Thresholds
	 * keeps the two equal.
	 */
	static constexpr float SlopeRockStart = 0.08f;
	static constexpr float SlopeRockEnd = 0.19f;

	/**
	 * The material the smooth terrain's shader draws at a surface point: the material's slope material (SlopeMaterialID)
	 * where the slope rock's weight is at least a half, the slope past the middle of SlopeRockStart and SlopeRockEnd (about
	 * 30 degrees), else the material itself. The shader's noise moves its border by up to 0.04 either way in patches; this
	 * is the border without it. Scatter places by it, so ground cover does not grow on the rock (G106).
	 * @param MaterialID The voxel's material
	 * @param CosToUp The cosine of the angle between the surface normal and local up
	 * @return The material drawn there
	 */
	uint8 GetDrawnMaterial(uint8 MaterialID, float CosToUp) const
	{
		if (MaterialID < Materials.Num() && Materials[MaterialID].SlopeMaterialID >= 0
			&& 1.0f - CosToUp >= 0.5f * (SlopeRockStart + SlopeRockEnd))
		{
			return static_cast<uint8>(Materials[MaterialID].SlopeMaterialID);
		}
		return MaterialID;
	}

	/** @return Whether a material of the table has a slope material (a world whose scatter GetDrawnMaterial can change) */
	bool HasSlopeMaterials() const;

	/** @param InMaterials The definitions in MaterialID order */
	explicit FVoxelMaterialTable(TArray<FVoxelMaterialDefinition>&& InMaterials) : Materials(MoveTemp(InMaterials)) {}

private:
	TArray<FVoxelMaterialDefinition> Materials;
};

/**
 * Static registry of voxel material definitions.
 * Provides lookup of material properties by MaterialID.
 * Answers from the built-in defaults (FVoxelMaterialTable::Defaults), an immutable table built on the first query: any
 * thread may query with no lock (G100). An atlas's materials are its world's table (FVoxelMaterialTable::FromAtlas),
 * never the registry's, so two worlds with different atlases do not share one and no atlas outlives its world (G101).
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
	 * @return The defaults in MaterialID order (valid for the process)
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
};
