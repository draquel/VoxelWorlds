// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelBiomeConfiguration.generated.h"

/**
 * Data asset for configuring biomes in a voxel world.
 *
 * Defines all biomes with their climate ranges (temperature/moisture),
 * material assignments, and blending parameters.
 *
 * Create this asset in the Content Browser, configure biomes,
 * and assign it to your VoxelWorldConfiguration.
 *
 * @see UVoxelWorldConfiguration
 * @see FBiomeDefinition
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelBiomeConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	UVoxelBiomeConfiguration();

	// ==================== Biome Definitions ====================

	/**
	 * All biome definitions for this world.
	 * Biomes are selected based on temperature and moisture values.
	 * Order matters for priority when ranges overlap.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biomes")
	TArray<FBiomeDefinition> Biomes;

	// ==================== Blending Settings ====================

	/**
	 * Width of biome blend zone in temperature/moisture space.
	 * Higher values create smoother transitions between biomes.
	 * Range: 0.01 (sharp edges) to 0.5 (very gradual blending)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blending", meta = (ClampMin = "0.01", ClampMax = "0.5"))
	float BiomeBlendWidth = 0.15f;

	// ==================== Height Material Overrides ====================

	/**
	 * Enable height-based material overrides (snow at peaks, rock at altitude, etc.)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height Materials")
	bool bEnableHeightMaterials = true;

	/**
	 * Rules for overriding materials based on world height.
	 * Applied after biome selection. Checked in priority order (highest first).
	 * Example: Snow above 5000 units, exposed rock between 3000-4000 units.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height Materials", meta = (EditCondition = "bEnableHeightMaterials"))
	TArray<FHeightMaterialRule> HeightMaterialRules;

	// ==================== Noise Parameters ====================

	/**
	 * Frequency for temperature noise (lower = larger biome regions).
	 * This controls how quickly temperature varies across the world.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "0.00001", ClampMax = "0.001"))
	float TemperatureNoiseFrequency = 0.00005f;

	/**
	 * Frequency for moisture noise (lower = larger biome regions).
	 * This controls how quickly moisture varies across the world.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "0.00001", ClampMax = "0.001"))
	float MoistureNoiseFrequency = 0.00007f;

	/**
	 * Seed offset for temperature noise (added to world seed).
	 * Change this to get different temperature patterns with the same world seed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	int32 TemperatureSeedOffset = 1234;

	/**
	 * Seed offset for moisture noise (added to world seed).
	 * Change this to get different moisture patterns with the same world seed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	int32 MoistureSeedOffset = 5678;

	// ==================== API ====================

	/**
	 * Initialize biomes with default definitions (Plains, Desert, Tundra).
	 * Call this to populate a new configuration with standard biomes.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel|Biome")
	void InitializeDefaults();

	/**
	 * Get the number of configured biomes.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Biome")
	int32 GetBiomeCount() const { return Biomes.Num(); }

	/**
	 * Get a biome definition by ID (C++ only, returns pointer).
	 * @param BiomeID The biome ID to look up
	 * @return Pointer to the biome definition, or nullptr if not found
	 */
	const FBiomeDefinition* GetBiome(uint8 BiomeID) const;

	/**
	 * Select the appropriate biome for given climate values.
	 * Uses simple priority-based selection (first matching biome wins).
	 * @param Temperature Normalized temperature (-1 to 1)
	 * @param Moisture Normalized moisture (-1 to 1)
	 * @return Pointer to the selected biome definition, or first biome as fallback
	 */
	const FBiomeDefinition* SelectBiome(float Temperature, float Moisture) const;

	/**
	 * Select the biome ID for given climate values.
	 * @param Temperature Normalized temperature (-1 to 1)
	 * @param Moisture Normalized moisture (-1 to 1)
	 * @return Biome ID
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Biome")
	uint8 SelectBiomeID(float Temperature, float Moisture) const;

	/**
	 * Calculate blended biome selection for smooth transitions.
	 * Uses distance-based weighting from biome boundaries.
	 * @param Temperature Normalized temperature (-1 to 1)
	 * @param Moisture Normalized moisture (-1 to 1)
	 * @return Blend result with up to MAX_BIOME_BLEND biomes and their weights
	 */
	FBiomeBlend GetBiomeBlend(float Temperature, float Moisture) const;

	/**
	 * Get material ID considering biome blending.
	 * Performs weighted selection based on blend weights.
	 * @param Blend The biome blend result
	 * @param DepthBelowSurface Depth below terrain surface
	 * @return Material ID selected based on blend weights
	 */
	uint8 GetBlendedMaterial(const FBiomeBlend& Blend, float DepthBelowSurface) const;

	/**
	 * Apply height material rules to override a material based on elevation.
	 * @param CurrentMaterial The material ID from biome selection
	 * @param WorldHeight The world Z coordinate
	 * @param DepthBelowSurface Depth below terrain surface
	 * @return Final material ID (may be overridden by height rules)
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Biome")
	uint8 ApplyHeightMaterialRules(uint8 CurrentMaterial, float WorldHeight, float DepthBelowSurface) const;

	/**
	 * Check if this configuration is valid for use.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Biome")
	bool IsValid() const;

#if WITH_EDITOR
	/**
	 * Validate the configuration and report any issues.
	 */
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;

	/**
	 * Called when a property changes in the editor.
	 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Cached mapping from BiomeID to array index for fast lookup */
	mutable TMap<uint8, int32> BiomeIDToIndex;
	mutable bool bBiomeIndexCacheDirty = true;

	/** Rebuild the BiomeID to index cache */
	void RebuildBiomeIndexCache() const;

	/** Cached sorted height rules (sorted by priority descending) */
	mutable TArray<FHeightMaterialRule> SortedHeightRules;
	mutable bool bHeightRulesCacheDirty = true;

	/** Rebuild the sorted height rules cache */
	void RebuildHeightRulesCache() const;
};
