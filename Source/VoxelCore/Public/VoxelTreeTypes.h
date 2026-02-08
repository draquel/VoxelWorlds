// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTreeTypes.generated.h"

/**
 * Shape of tree canopy for voxel tree injection.
 */
UENUM(BlueprintType)
enum class ETreeCanopyShape : uint8
{
	/** Spherical canopy */
	Sphere,

	/** Conical canopy (wider at bottom) */
	Cone,

	/** Flat disc canopy */
	FlatDisc,

	/** Rounded cube canopy */
	RoundedCube
};

/**
 * How trees are rendered in cubic terrain.
 */
UENUM(BlueprintType)
enum class EVoxelTreeMode : uint8
{
	/** Trees injected into terrain VoxelData (editable, destructible) */
	VoxelData,

	/** Pre-built block-style meshes via HISM (lighter, not editable) */
	HISM,

	/** VoxelData near camera, HISM far away */
	Both
};

/**
 * Template defining a voxel tree's shape and materials.
 * Used by VoxelTreeInjector to stamp tree blocks into VoxelData.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelTreeTemplate
{
	GENERATED_BODY()

	/** Unique template ID (index into Configuration->TreeTemplates) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	int32 TemplateID = 0;

	/** Display name */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	FString Name = TEXT("Oak");

	// ==================== Trunk ====================

	/** Trunk height in voxels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trunk", meta = (ClampMin = "1", ClampMax = "32"))
	int32 TrunkHeight = 6;

	/** Random variance on trunk height (+/-) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trunk", meta = (ClampMin = "0", ClampMax = "8"))
	int32 TrunkHeightVariance = 2;

	/** Trunk radius: 0 = 1x1 column, 1 = 3x3 cross pattern */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trunk", meta = (ClampMin = "0", ClampMax = "2"))
	int32 TrunkRadius = 0;

	/** Material ID for trunk blocks */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trunk")
	uint8 TrunkMaterialID = 20; // EVoxelMaterial::Wood

	// ==================== Canopy ====================

	/** Shape of the canopy */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Canopy")
	ETreeCanopyShape CanopyShape = ETreeCanopyShape::Sphere;

	/** Canopy radius in voxels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Canopy", meta = (ClampMin = "1", ClampMax = "16"))
	int32 CanopyRadius = 3;

	/** Random variance on canopy radius (+/-) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Canopy", meta = (ClampMin = "0", ClampMax = "4"))
	int32 CanopyRadiusVariance = 1;

	/** Material ID for leaf blocks */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Canopy")
	uint8 LeafMaterialID = 21; // EVoxelMaterial::Leaves

	/** Vertical offset of canopy center relative to trunk top */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Canopy", meta = (ClampMin = "-4", ClampMax = "4"))
	int32 CanopyVerticalOffset = 0;

	// ==================== Placement Rules ====================

	/** Allowed surface material IDs for tree placement (empty = all materials allowed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TArray<uint8> AllowedMaterials;

	/** Allowed biome IDs for tree placement (empty = all biomes allowed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TArray<uint8> AllowedBiomes;

	/** Minimum world Z elevation for tree placement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	float MinElevation = -1000000.0f;

	/** Maximum world Z elevation for tree placement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	float MaxElevation = 1000000.0f;

	/** Maximum terrain slope in degrees for tree placement (0 = flat only, 90 = any slope) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MaxSlopeDegrees = 30.0f;

	FVoxelTreeTemplate() = default;

	/** Check if a tree can spawn at the given placement conditions */
	bool CanSpawnAt(float Elevation, float SlopeAngle, uint8 SurfaceMaterial, uint8 BiomeID) const
	{
		// Elevation check
		if (Elevation < MinElevation || Elevation > MaxElevation)
		{
			return false;
		}

		// Slope check
		if (SlopeAngle > MaxSlopeDegrees)
		{
			return false;
		}

		// Material filter
		if (AllowedMaterials.Num() > 0 && !AllowedMaterials.Contains(SurfaceMaterial))
		{
			return false;
		}

		// Biome filter
		if (AllowedBiomes.Num() > 0 && !AllowedBiomes.Contains(BiomeID))
		{
			return false;
		}

		return true;
	}

	/** Get max horizontal extent (for cross-chunk overlap checks) */
	int32 GetMaxHorizontalExtent() const
	{
		return CanopyRadius + CanopyRadiusVariance;
	}

	/** Get max tree height including canopy */
	int32 GetMaxHeight() const
	{
		return (TrunkHeight + TrunkHeightVariance) + (CanopyRadius + CanopyRadiusVariance) * 2 + FMath::Abs(CanopyVerticalOffset);
	}
};
