// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCaveTypes.generated.h"

/**
 * Type of cave geometry to generate.
 * Each type uses different noise field composition for distinct shapes.
 */
UENUM(BlueprintType)
enum class ECaveType : uint8
{
	/** Large, open caverns created by single noise threshold carving */
	Cheese,

	/** Winding tunnel networks created by dual-noise field intersection */
	Spaghetti,

	/** Thin, narrow passages created by tight dual-noise intersection */
	Noodle
};

/**
 * Configuration for a single cave generation layer.
 * Multiple layers compose to create varied underground cave networks.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FCaveLayerConfig
{
	GENERATED_BODY()

	/** Enable this cave layer */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer")
	bool bEnabled = true;

	/** Type of cave geometry this layer generates */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer")
	ECaveType CaveType = ECaveType::Cheese;

	/** Seed offset added to world seed for this layer's noise (ensures unique patterns per layer) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer")
	int32 SeedOffset = 0;

	// ==================== Noise Parameters ====================

	/** Base frequency of cave noise (lower = larger caves) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Noise", meta = (ClampMin = "0.0001", ClampMax = "0.1"))
	float Frequency = 0.005f;

	/** Number of fBm octaves */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Noise", meta = (ClampMin = "1", ClampMax = "8"))
	int32 Octaves = 3;

	/** Amplitude falloff per octave */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Noise", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Persistence = 0.5f;

	/** Frequency multiplier per octave */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Noise", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float Lacunarity = 2.0f;

	// ==================== Carving Parameters ====================

	/**
	 * Noise threshold for carving.
	 * Cheese: noise above this value is carved (higher = less caves).
	 * Spaghetti/Noodle: both noise fields must be within [-Threshold, Threshold] to carve.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Carving", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float Threshold = 0.5f;

	/** Strength of density subtraction when carving [0, 1] */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Carving", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CarveStrength = 1.0f;

	/** Falloff smoothness at cave edges. Higher = softer edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Carving", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float CarveFalloff = 0.1f;

	// ==================== Depth Constraints ====================

	/** Minimum depth below surface for caves (in voxels). Prevents surface breakout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Depth", meta = (ClampMin = "0"))
	float MinDepth = 5.0f;

	/** Maximum depth below surface for caves (in voxels, 0 = no limit) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Depth")
	float MaxDepth = 0.0f;

	/** Width of depth fade zone at MinDepth and MaxDepth boundaries (in voxels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Depth", meta = (ClampMin = "1.0", ClampMax = "20.0"))
	float DepthFadeWidth = 4.0f;

	// ==================== Shape Control ====================

	/**
	 * Vertical scale factor for cave noise sampling.
	 * Values < 1.0 create more horizontal caves, > 1.0 create more vertical caves.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Shape", meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float VerticalScale = 0.5f;

	// ==================== Dual-Noise (Spaghetti/Noodle only) ====================

	/** Seed offset for the second noise field (Spaghetti/Noodle only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Dual Noise", meta = (EditCondition = "CaveType != ECaveType::Cheese"))
	int32 SecondNoiseSeedOffset = 7777;

	/** Frequency scale multiplier for the second noise field relative to the first */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Layer|Dual Noise", meta = (ClampMin = "0.5", ClampMax = "3.0", EditCondition = "CaveType != ECaveType::Cheese"))
	float SecondNoiseFrequencyScale = 1.2f;

	FCaveLayerConfig() = default;
};

/**
 * Per-biome override for cave generation.
 * Allows scaling or disabling caves in specific biomes.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FBiomeCaveOverride
{
	GENERATED_BODY()

	/** Biome ID to override (index into biome configuration) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Cave Override")
	uint8 BiomeID = 0;

	/** Cave density scale for this biome (0 = no caves, 1 = normal, >1 = more caves) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Cave Override", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float CaveScale = 1.0f;

	/** Override MinDepth for this biome (-1 = use layer default) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Cave Override")
	float MinDepthOverride = -1.0f;

	FBiomeCaveOverride() = default;
};
