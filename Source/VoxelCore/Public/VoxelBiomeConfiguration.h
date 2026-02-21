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

	/** Rebuild caches after deserialization loads actual property values */
	virtual void PostLoad() override;

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

	// ==================== Ore Vein Settings ====================

	/**
	 * Enable ore vein generation.
	 * When enabled, ore deposits are placed using 3D noise patterns.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Veins")
	bool bEnableOreVeins = true;

	/**
	 * Global ore vein configurations.
	 * These ores spawn in all biomes (unless overridden by biome-specific ores).
	 * Checked in priority order (highest first).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Veins", meta = (EditCondition = "bEnableOreVeins"))
	TArray<FOreVeinConfig> GlobalOreVeins;

	// ==================== Underwater Material Settings ====================

	/**
	 * Enable underwater material overrides.
	 * When enabled and terrain is below water level, use biome's underwater materials.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Underwater")
	bool bEnableUnderwaterMaterials = true;

	/**
	 * Default underwater material ID (used when biome doesn't specify one).
	 * Typically Sand (3) for a beach-like underwater appearance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Underwater", meta = (EditCondition = "bEnableUnderwaterMaterials"))
	uint8 DefaultUnderwaterMaterial = 3; // EVoxelMaterial::Sand

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

	// ==================== Continentalness ====================

	/**
	 * Enable continentalness as a biome selection axis and terrain height modulator.
	 * When enabled, a third noise axis controls large-scale land/water distribution.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continentalness")
	bool bEnableContinentalness = false;

	/**
	 * Frequency for continentalness noise (lower = larger land masses).
	 * Default is very low for continental-scale features.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "0.000001", ClampMax = "0.001", EditCondition = "bEnableContinentalness"))
	float ContinentalnessNoiseFrequency = 0.00002f;

	/**
	 * Seed offset for continentalness noise (added to world seed).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (EditCondition = "bEnableContinentalness"))
	int32 ContinentalnessSeedOffset = 9012;

	/**
	 * Height offset at continentalness = -1 (deep ocean), in world units.
	 * Should be well below WaterLevel to create ocean floor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continentalness", meta = (EditCondition = "bEnableContinentalness"))
	float ContinentalnessHeightMin = -3000.0f;

	/**
	 * Height offset at continentalness = 0 (coast).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continentalness", meta = (EditCondition = "bEnableContinentalness"))
	float ContinentalnessHeightMid = 0.0f;

	/**
	 * Height offset at continentalness = +1 (continental interior).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continentalness", meta = (EditCondition = "bEnableContinentalness"))
	float ContinentalnessHeightMax = 1000.0f;

	/**
	 * HeightScale multiplier at continentalness = -1 (ocean: flat seabed).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continentalness", meta = (ClampMin = "0.0", ClampMax = "2.0", EditCondition = "bEnableContinentalness"))
	float ContinentalnessHeightScaleMin = 0.2f;

	/**
	 * HeightScale multiplier at continentalness = +1 (inland: full terrain variation).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Continentalness", meta = (ClampMin = "0.0", ClampMax = "2.0", EditCondition = "bEnableContinentalness"))
	float ContinentalnessHeightScaleMax = 1.0f;

	/**
	 * Map continentalness value to terrain height offset and height scale multiplier.
	 * Uses piecewise linear interpolation: [-1,0] maps HeightMin->HeightMid, [0,1] maps HeightMid->HeightMax.
	 * @param Continentalness Input value in [-1, 1]
	 * @param OutHeightOffset Output height offset in world units
	 * @param OutHeightScaleMultiplier Output multiplier for HeightScale
	 */
	void GetContinentalnessTerrainParams(float Continentalness, float& OutHeightOffset, float& OutHeightScaleMultiplier) const;

	// ==================== API ====================

	/**
	 * Initialize biomes with default definitions (Plains, Forest, Mountain, Ocean).
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
	 * @param Continentalness Normalized continentalness (-1 to 1), default 0 for backward compat
	 * @return Pointer to the selected biome definition, or first biome as fallback
	 */
	const FBiomeDefinition* SelectBiome(float Temperature, float Moisture, float Continentalness = 0.0f) const;

	/**
	 * Select the biome ID for given climate values.
	 * @param Temperature Normalized temperature (-1 to 1)
	 * @param Moisture Normalized moisture (-1 to 1)
	 * @param Continentalness Normalized continentalness (-1 to 1), default 0 for backward compat
	 * @return Biome ID
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Biome")
	uint8 SelectBiomeID(float Temperature, float Moisture, float Continentalness = 0.0f) const;

	/**
	 * Calculate blended biome selection for smooth transitions.
	 * Uses distance-based weighting from biome boundaries.
	 * @param Temperature Normalized temperature (-1 to 1)
	 * @param Moisture Normalized moisture (-1 to 1)
	 * @param Continentalness Normalized continentalness (-1 to 1), default 0 for backward compat
	 * @return Blend result with up to MAX_BIOME_BLEND biomes and their weights
	 */
	FBiomeBlend GetBiomeBlend(float Temperature, float Moisture, float Continentalness = 0.0f) const;

	/**
	 * Get material ID considering biome blending.
	 * Performs weighted selection based on blend weights.
	 * @param Blend The biome blend result
	 * @param DepthBelowSurface Depth below terrain surface
	 * @return Material ID selected based on blend weights
	 */
	uint8 GetBlendedMaterial(const FBiomeBlend& Blend, float DepthBelowSurface) const;

	/**
	 * Get material ID considering biome blending and water level.
	 * Uses underwater materials when terrain surface is below water level.
	 * @param Blend The biome blend result
	 * @param DepthBelowSurface Depth below terrain surface
	 * @param TerrainSurfaceHeight The height of the terrain surface at this X,Y
	 * @param WaterLevel The water level height (or radius for spherical)
	 * @return Material ID selected based on blend weights and water state
	 */
	uint8 GetBlendedMaterialWithWater(const FBiomeBlend& Blend, float DepthBelowSurface,
		float TerrainSurfaceHeight, float WaterLevel) const;

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
	 * Get the applicable ore veins for a biome.
	 * Returns biome-specific ores if configured, otherwise global ores.
	 * @param BiomeID The biome to get ores for
	 * @param OutOres Output array of applicable ore configs
	 */
	void GetOreVeinsForBiome(uint8 BiomeID, TArray<FOreVeinConfig>& OutOres) const;

	/**
	 * Check if ore veins are enabled and configured.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Ore")
	bool HasOreVeins() const { return bEnableOreVeins && GlobalOreVeins.Num() > 0; }

	/**
	 * Check if this configuration is valid for use.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Biome")
	bool IsValid() const;

	/**
	 * Log all configuration values to the output log for debugging.
	 * Dumps biome definitions, height rules, and ore vein configs.
	 */
	void LogConfiguration() const;

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

	/** Cached sorted global ore veins (sorted by priority descending) */
	mutable TArray<FOreVeinConfig> SortedGlobalOres;
	mutable bool bOreVeinsCacheDirty = true;

	/** Rebuild the sorted ore veins cache */
	void RebuildOreVeinsCache() const;
};
