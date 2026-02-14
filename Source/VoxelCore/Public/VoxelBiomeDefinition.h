// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelBiomeDefinition.generated.h"

/**
 * Maximum number of biomes that can be blended at once.
 */
constexpr int32 MAX_BIOME_BLEND = 4;

/**
 * Maximum number of ore veins that can be configured.
 */
constexpr int32 MAX_ORE_VEINS = 16;

/**
 * Shape type for ore vein generation.
 */
UENUM(BlueprintType)
enum class EOreVeinShape : uint8
{
	/** Blobby, rounded clusters using 3D noise threshold */
	Blob,

	/** Elongated, streak-like veins using anisotropic/directional noise */
	Streak
};

/**
 * Configuration for a single ore vein type.
 * Defines where and how ore deposits spawn in the terrain.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FOreVeinConfig
{
	GENERATED_BODY()

	/** Display name for this ore type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein")
	FString Name;

	/** Material ID for this ore (index into material atlas) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein")
	uint8 MaterialID = 0;

	/** Minimum depth below surface for ore to spawn (in voxels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein", meta = (ClampMin = "0"))
	float MinDepth = 3.0f;

	/** Maximum depth below surface for ore to spawn (in voxels, 0 = no limit) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein")
	float MaxDepth = 0.0f;

	/** Shape of ore deposits */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein")
	EOreVeinShape Shape = EOreVeinShape::Blob;

	/** Frequency of ore noise (lower = larger deposits, higher = smaller deposits) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float Frequency = 0.05f;

	/** Noise threshold for ore placement (higher = rarer ore, 0.8-0.95 typical) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Threshold = 0.85f;

	/** Seed offset for this ore type (added to world seed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein")
	int32 SeedOffset = 0;

	/** Rarity multiplier (0-1, lower = rarer). Applied after threshold check. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Rarity = 1.0f;

	/**
	 * Stretch factor for streak-shaped veins.
	 * Values > 1 create elongated deposits along random directions.
	 * Only used when Shape == Streak.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein", meta = (ClampMin = "1.0", ClampMax = "10.0", EditCondition = "Shape == EOreVeinShape::Streak"))
	float StreakStretch = 4.0f;

	/** Priority for ore placement (higher = checked first, can override other ores) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ore Vein")
	int32 Priority = 0;

	FOreVeinConfig() = default;

	FOreVeinConfig(const FString& InName, uint8 InMaterialID, float InMinDepth, float InMaxDepth,
		EOreVeinShape InShape, float InFrequency, float InThreshold, int32 InSeedOffset, int32 InPriority = 0)
		: Name(InName)
		, MaterialID(InMaterialID)
		, MinDepth(InMinDepth)
		, MaxDepth(InMaxDepth)
		, Shape(InShape)
		, Frequency(InFrequency)
		, Threshold(InThreshold)
		, SeedOffset(InSeedOffset)
		, Priority(InPriority)
	{
	}

	/** Check if ore can spawn at this depth */
	bool IsValidDepth(float DepthBelowSurface) const
	{
		if (DepthBelowSurface < MinDepth)
		{
			return false;
		}
		if (MaxDepth > 0.0f && DepthBelowSurface > MaxDepth)
		{
			return false;
		}
		return true;
	}
};

/**
 * Definition of a biome with climate ranges and material assignments.
 * Biomes are selected based on temperature and moisture values.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FBiomeDefinition
{
	GENERATED_BODY()

	/** Unique identifier for this biome (0-255) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	uint8 BiomeID = 0;

	/** Display name for this biome */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	FString Name;

	/** Temperature range (X=min, Y=max) in normalized -1 to 1 space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	FVector2D TemperatureRange = FVector2D(-1.0, 1.0);

	/** Moisture range (X=min, Y=max) in normalized -1 to 1 space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	FVector2D MoistureRange = FVector2D(-1.0, 1.0);

	/** Continentalness range (X=min, Y=max) in normalized -1 to 1 space.
	 *  -1 = deep ocean, 0 = coastline, 1 = continental interior.
	 *  Default full range (-1,1) for backward compatibility. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	FVector2D ContinentalnessRange = FVector2D(-1.0, 1.0);

	/** Material ID for surface voxels (depth 0-1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	uint8 SurfaceMaterial = 0;

	/** Material ID for subsurface voxels (depth 1-4) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	uint8 SubsurfaceMaterial = 0;

	/** Material ID for deep voxels (depth 4+) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	uint8 DeepMaterial = 0;

	/** Depth threshold between surface and subsurface (in voxels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	float SurfaceDepth = 1.0f;

	/** Depth threshold between subsurface and deep (in voxels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome")
	float SubsurfaceDepth = 4.0f;

	// ==================== Underwater Materials ====================

	/** Material ID for surface voxels when terrain is below water level */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome|Underwater")
	uint8 UnderwaterSurfaceMaterial = 3; // Default: Sand

	/** Material ID for subsurface voxels when terrain is below water level */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome|Underwater")
	uint8 UnderwaterSubsurfaceMaterial = 3; // Default: Sand

	/**
	 * Biome-specific ore veins (optional).
	 * If populated, these override global ore veins for this biome.
	 * If empty, global ore veins are used instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome|Ore")
	TArray<FOreVeinConfig> BiomeOreVeins;

	/** If true, biome ores ADD to global ores. If false, biome ores REPLACE global ores. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Biome|Ore")
	bool bAddToGlobalOres = false;

	FBiomeDefinition() = default;

	FBiomeDefinition(
		uint8 InID,
		const FString& InName,
		const FVector2D& InTempRange,
		const FVector2D& InMoistureRange,
		uint8 InSurface,
		uint8 InSubsurface,
		uint8 InDeep)
		: BiomeID(InID)
		, Name(InName)
		, TemperatureRange(InTempRange)
		, MoistureRange(InMoistureRange)
		, SurfaceMaterial(InSurface)
		, SubsurfaceMaterial(InSubsurface)
		, DeepMaterial(InDeep)
	{
	}

	/**
	 * Check if the given climate values fall within this biome's ranges.
	 * @param Temperature Normalized temperature (-1 to 1)
	 * @param Moisture Normalized moisture (-1 to 1)
	 * @param Continentalness Normalized continentalness (-1 to 1), default 0 for backward compat
	 */
	bool Contains(float Temperature, float Moisture, float Continentalness = 0.0f) const
	{
		return Temperature >= TemperatureRange.X && Temperature <= TemperatureRange.Y
			&& Moisture >= MoistureRange.X && Moisture <= MoistureRange.Y
			&& Continentalness >= ContinentalnessRange.X && Continentalness <= ContinentalnessRange.Y;
	}

	/**
	 * Get the appropriate material ID for the given depth below surface.
	 * @param DepthBelowSurface How many voxels below the terrain surface (0 = surface)
	 * @return Material ID for this depth
	 */
	uint8 GetMaterialAtDepth(float DepthBelowSurface) const
	{
		if (DepthBelowSurface <= SurfaceDepth)
		{
			return SurfaceMaterial;
		}
		else if (DepthBelowSurface <= SubsurfaceDepth)
		{
			return SubsurfaceMaterial;
		}
		else
		{
			return DeepMaterial;
		}
	}

	/**
	 * Get the appropriate material ID considering underwater state.
	 * @param DepthBelowSurface How many voxels below the terrain surface (0 = surface)
	 * @param bIsUnderwater Whether the terrain surface is below water level
	 * @return Material ID for this depth (uses underwater materials if bIsUnderwater)
	 */
	uint8 GetMaterialAtDepth(float DepthBelowSurface, bool bIsUnderwater) const
	{
		if (bIsUnderwater)
		{
			// Underwater uses separate material set (no deep material distinction)
			if (DepthBelowSurface <= SurfaceDepth)
			{
				return UnderwaterSurfaceMaterial;
			}
			else if (DepthBelowSurface <= SubsurfaceDepth)
			{
				return UnderwaterSubsurfaceMaterial;
			}
			else
			{
				return DeepMaterial; // Deep material stays the same (stone)
			}
		}
		return GetMaterialAtDepth(DepthBelowSurface);
	}

	/**
	 * Calculate the distance from a point in temperature/moisture space to this biome's center.
	 * Used for blending weight calculations.
	 */
	float GetDistanceToCenter(float Temperature, float Moisture) const
	{
		const float CenterTemp = (TemperatureRange.X + TemperatureRange.Y) * 0.5f;
		const float CenterMoist = (MoistureRange.X + MoistureRange.Y) * 0.5f;
		const float DeltaT = Temperature - CenterTemp;
		const float DeltaM = Moisture - CenterMoist;
		return FMath::Sqrt(DeltaT * DeltaT + DeltaM * DeltaM);
	}

	/**
	 * Calculate the distance from a point to the edge of this biome's range.
	 * Returns positive if inside, negative if outside.
	 * @param Continentalness Default 0 for backward compatibility
	 */
	float GetSignedDistanceToEdge(float Temperature, float Moisture, float Continentalness = 0.0f) const
	{
		// Distance to each edge (positive = inside, negative = outside)
		const float DistTempMin = Temperature - TemperatureRange.X;
		const float DistTempMax = TemperatureRange.Y - Temperature;
		const float DistMoistMin = Moisture - MoistureRange.X;
		const float DistMoistMax = MoistureRange.Y - Moisture;
		const float DistContMin = Continentalness - ContinentalnessRange.X;
		const float DistContMax = ContinentalnessRange.Y - Continentalness;

		// Minimum distance to any edge (positive if inside all edges)
		return FMath::Min(
			FMath::Min(FMath::Min(DistTempMin, DistTempMax), FMath::Min(DistMoistMin, DistMoistMax)),
			FMath::Min(DistContMin, DistContMax));
	}
};

/**
 * Rule for overriding material based on world height.
 * Applied after biome-based material selection for elevation-dependent effects
 * like snow on mountain peaks or exposed rock at high altitude.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FHeightMaterialRule
{
	GENERATED_BODY()

	/** Minimum world height (Z) for this rule to apply (in world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height Rule")
	float MinHeight = 0.0f;

	/** Maximum world height (Z) for this rule to apply (in world units). Set to MAX_FLT for no upper limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height Rule")
	float MaxHeight = MAX_FLT;

	/** Material ID to use when this rule applies */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height Rule")
	uint8 MaterialID = 0;

	/** Only apply to surface voxels (depth below surface < threshold) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height Rule")
	bool bSurfaceOnly = true;

	/** Maximum depth below surface for this rule to apply (when bSurfaceOnly is true) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height Rule", meta = (EditCondition = "bSurfaceOnly"))
	float MaxDepthBelowSurface = 2.0f;

	/** Priority for rule ordering (higher = checked first) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Height Rule")
	int32 Priority = 0;

	FHeightMaterialRule() = default;

	FHeightMaterialRule(float InMinHeight, float InMaxHeight, uint8 InMaterialID, bool bInSurfaceOnly = true, float InMaxDepth = 2.0f, int32 InPriority = 0)
		: MinHeight(InMinHeight)
		, MaxHeight(InMaxHeight)
		, MaterialID(InMaterialID)
		, bSurfaceOnly(bInSurfaceOnly)
		, MaxDepthBelowSurface(InMaxDepth)
		, Priority(InPriority)
	{
	}

	/** Check if this rule applies at the given height and depth */
	bool Applies(float WorldHeight, float DepthBelowSurface) const
	{
		if (WorldHeight < MinHeight || WorldHeight > MaxHeight)
		{
			return false;
		}
		if (bSurfaceOnly && DepthBelowSurface > MaxDepthBelowSurface)
		{
			return false;
		}
		return true;
	}
};

/**
 * Blend result containing multiple biomes with weights.
 * Used for smooth transitions between biome regions.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FBiomeBlend
{
	GENERATED_BODY()

	/** Biome IDs participating in the blend (up to MAX_BIOME_BLEND) */
	uint8 BiomeIDs[MAX_BIOME_BLEND] = { 0, 0, 0, 0 };

	/** Blend weights for each biome (sum should equal 1.0) */
	float Weights[MAX_BIOME_BLEND] = { 1.0f, 0.0f, 0.0f, 0.0f };

	/** Number of biomes in this blend (1-4) */
	UPROPERTY(BlueprintReadOnly, Category = "Voxel Biome")
	int32 BiomeCount = 1;

	FBiomeBlend() = default;

	/** Create a single-biome blend (no blending) */
	explicit FBiomeBlend(uint8 SingleBiomeID)
		: BiomeCount(1)
	{
		BiomeIDs[0] = SingleBiomeID;
		Weights[0] = 1.0f;
	}

	/** Get the dominant biome ID (highest weight) */
	uint8 GetDominantBiome() const
	{
		return BiomeIDs[0]; // Sorted by weight, so index 0 is dominant
	}

	/** Check if blending is occurring (more than one biome with significant weight) */
	bool IsBlending() const
	{
		return BiomeCount > 1 && Weights[1] > 0.01f;
	}

	/** Normalize weights to sum to 1.0 */
	void NormalizeWeights()
	{
		float TotalWeight = 0.0f;
		for (int32 i = 0; i < BiomeCount; ++i)
		{
			TotalWeight += Weights[i];
		}
		if (TotalWeight > KINDA_SMALL_NUMBER)
		{
			for (int32 i = 0; i < BiomeCount; ++i)
			{
				Weights[i] /= TotalWeight;
			}
		}
	}
};
