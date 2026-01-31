// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelBiomeDefinition.generated.h"

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
	 * Check if the given temperature and moisture values fall within this biome's ranges.
	 */
	bool Contains(float Temperature, float Moisture) const
	{
		return Temperature >= TemperatureRange.X && Temperature <= TemperatureRange.Y
			&& Moisture >= MoistureRange.X && Moisture <= MoistureRange.Y;
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
};
