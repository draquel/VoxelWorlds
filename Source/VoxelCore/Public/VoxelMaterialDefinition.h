// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMaterialDefinition.generated.h"

/**
 * Definition of a voxel material with visual properties.
 * Used by the material registry to map MaterialIDs to colors and atlas positions.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelMaterialDefinition
{
	GENERATED_BODY()

	/** Unique identifier for this material (0-255) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Material")
	uint8 MaterialID = 0;

	/** Display name for this material */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Material")
	FString Name;

	/** Base color for vertex color visualization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Material")
	FColor Color = FColor::White;

	// ===== Atlas Properties =====

	/** Column position in packed atlas (0-based, for cubic terrain) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Material|Atlas")
	int32 AtlasColumn = 0;

	/** Row position in packed atlas (0-based, for cubic terrain) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Material|Atlas")
	int32 AtlasRow = 0;

	/** Index into Texture2DArray (for smooth terrain, -1 if not set) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Material|Atlas")
	int32 ArrayIndex = -1;

	/** Scale for triplanar projection (smooth terrain) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Material|Atlas", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float TriplanarScale = 1.0f;

	/** UV scale multiplier for packed atlas sampling */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel Material|Atlas", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float UVScale = 1.0f;

	FVoxelMaterialDefinition() = default;

	FVoxelMaterialDefinition(uint8 InID, const FString& InName, const FColor& InColor)
		: MaterialID(InID)
		, Name(InName)
		, Color(InColor)
		, AtlasColumn(0)
		, AtlasRow(0)
		, ArrayIndex(-1)
		, TriplanarScale(1.0f)
		, UVScale(1.0f)
	{
	}

	FVoxelMaterialDefinition(uint8 InID, const FString& InName, const FColor& InColor, int32 InAtlasColumn, int32 InAtlasRow)
		: MaterialID(InID)
		, Name(InName)
		, Color(InColor)
		, AtlasColumn(InAtlasColumn)
		, AtlasRow(InAtlasRow)
		, ArrayIndex(InID)
		, TriplanarScale(1.0f)
		, UVScale(1.0f)
	{
	}
};
