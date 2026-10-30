// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelScatterTypes.h"
#include "VoxelScatterConfiguration.generated.h"

/**
 * Configuration data asset for scatter definitions.
 *
 * Create this as a Data Asset in the Content Browser:
 *   Right-click -> Miscellaneous -> Data Asset -> VoxelScatterConfiguration
 *
 * Then assign it to your VoxelWorldConfiguration's ScatterConfiguration property.
 *
 * @see UVoxelWorldConfiguration
 * @see FScatterDefinition
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelScatterConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	UVoxelScatterConfiguration();

	// ==================== Scatter Definitions ====================

	/**
	 * Array of scatter definitions.
	 * Each definition specifies a type of object to scatter (grass, rocks, trees, etc.)
	 * with placement rules, mesh, and variation settings.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	TArray<FScatterDefinition> ScatterDefinitions;

	// ==================== Global Settings ====================

	/**
	 * Target spacing between surface sample points (cm).
	 * Lower = more samples = more potential spawn locations = higher density possible.
	 * Default 100cm gives good coverage without excessive sampling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Sampling", meta = (ClampMin = "10.0", ClampMax = "500.0"))
	float SurfacePointSpacing = 100.0f;

	/**
	 * Use default scatter definitions if array is empty.
	 * When true and ScatterDefinitions is empty, creates Grass/Rocks/Trees defaults.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	bool bUseDefaultsIfEmpty = true;

public:
	/**
	 * Get scatter definition by ID.
	 * @param ScatterID ID to look up
	 * @return Pointer to definition or nullptr if not found
	 */
	const FScatterDefinition* GetScatterDefinition(int32 ScatterID) const;

	/**
	 * Validate configuration and log any issues.
	 * @return True if valid
	 */
	bool ValidateConfiguration() const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
