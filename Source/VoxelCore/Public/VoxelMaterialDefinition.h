// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMaterialDefinition.generated.h"

/**
 * Definition of a voxel material with visual properties.
 * Used by the material registry to map MaterialIDs to colors.
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

	FVoxelMaterialDefinition() = default;

	FVoxelMaterialDefinition(uint8 InID, const FString& InName, const FColor& InColor)
		: MaterialID(InID)
		, Name(InName)
		, Color(InColor)
	{
	}
};
