// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelCaveTypes.h"
#include "VoxelCaveConfiguration.generated.h"

/**
 * Data asset for configuring cave generation in a voxel world.
 *
 * Defines cave layers (cheese caverns, spaghetti tunnels, noodle passages),
 * per-biome overrides, and cave wall material settings.
 *
 * Create this asset in the Content Browser:
 *   Right-click -> Miscellaneous -> Data Asset -> VoxelCaveConfiguration
 *
 * @see UVoxelWorldConfiguration
 * @see FCaveLayerConfig
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelCaveConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	UVoxelCaveConfiguration();

	// ==================== Cave Layers ====================

	/** Enable cave generation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves")
	bool bEnableCaves = true;

	/**
	 * Cave layers to compose. Each layer generates a different type of cave.
	 * Layers are composited via union (max carve density) — overlapping caves merge naturally.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves")
	TArray<FCaveLayerConfig> CaveLayers;

	// ==================== Biome Overrides ====================

	/**
	 * Per-biome cave scaling and depth overrides.
	 * Use to disable caves in deserts, increase in mountains, etc.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Biome Overrides")
	TArray<FBiomeCaveOverride> BiomeOverrides;

	// ==================== Underwater Suppression ====================

	/**
	 * Minimum depth below seabed for caves when terrain surface is below water level.
	 * Applied regardless of biome — catches transition zones where ocean biome
	 * isn't assigned but terrain is still underwater. Set to 0 to disable.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Underwater", meta = (ClampMin = "0"))
	float UnderwaterMinDepth = 0.0f;

	// ==================== Cave Wall Material ====================

	/** Override material on cave wall surfaces */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Materials")
	bool bOverrideCaveWallMaterial = false;

	/** Material ID to apply on cave walls (index into material atlas) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Materials", meta = (EditCondition = "bOverrideCaveWallMaterial"))
	uint8 CaveWallMaterialID = 2;

	/** Minimum depth below surface for cave wall material override (in voxels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Materials", meta = (ClampMin = "0", EditCondition = "bOverrideCaveWallMaterial"))
	float CaveWallMaterialMinDepth = 10.0f;

	// ==================== Methods ====================

	/**
	 * Initialize default cave layers (Cheese, Spaghetti, Noodle).
	 * Called by constructor. Can be called manually to reset to defaults.
	 */
	void InitializeDefaults();

	/**
	 * Get the biome cave scale for a given biome ID.
	 * @param BiomeID Biome to look up
	 * @return Cave scale factor (0 = no caves, 1 = normal)
	 */
	float GetBiomeCaveScale(uint8 BiomeID) const;

	/**
	 * Get the minimum depth override for a given biome ID.
	 * @param BiomeID Biome to look up
	 * @return MinDepth override, or -1 if no override
	 */
	float GetBiomeMinDepthOverride(uint8 BiomeID) const;

	/** Check if any cave layers are enabled */
	bool HasEnabledLayers() const;
};
