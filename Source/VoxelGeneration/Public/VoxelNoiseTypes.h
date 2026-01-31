// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelNoiseTypes.generated.h"

// Re-export EVoxelNoiseType and FVoxelNoiseParams from VoxelCoreTypes.h
// These are defined in VoxelCore to avoid circular dependencies with VoxelWorldConfiguration

/**
 * Request for generating voxel data for a chunk.
 */
USTRUCT(BlueprintType)
struct VOXELGENERATION_API FVoxelNoiseGenerationRequest
{
	GENERATED_BODY()

	/** Chunk coordinate in chunk space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** LOD level for this chunk (0 = highest detail) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	int32 LODLevel = 0;

	/** Number of voxels per chunk edge */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	int32 ChunkSize = 32;

	/** Size of each voxel in world units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	float VoxelSize = 100.0f;

	/** Noise parameters for generation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	FVoxelNoiseParams NoiseParams;

	FVoxelNoiseGenerationRequest() = default;

	/** Get the world position of this chunk's origin */
	FVector GetChunkWorldPosition() const
	{
		float ChunkWorldSize = ChunkSize * VoxelSize * FMath::Pow(2.0f, static_cast<float>(LODLevel));
		return FVector(ChunkCoord) * ChunkWorldSize;
	}

	/** Get the effective voxel size at this LOD level */
	float GetEffectiveVoxelSize() const
	{
		return VoxelSize * FMath::Pow(2.0f, static_cast<float>(LODLevel));
	}
};

/**
 * Handle for tracking async generation operations.
 */
struct VOXELGENERATION_API FVoxelGenerationHandle
{
	/** Unique identifier for this generation request */
	uint64 RequestId = 0;

	/** Whether generation has completed */
	bool bIsComplete = false;

	/** Whether generation succeeded */
	bool bWasSuccessful = false;

	/** Error message if generation failed */
	FString ErrorMessage;

	FVoxelGenerationHandle() = default;
	explicit FVoxelGenerationHandle(uint64 InRequestId) : RequestId(InRequestId) {}

	bool IsValid() const { return RequestId != 0; }
	bool IsComplete() const { return bIsComplete; }
	bool WasSuccessful() const { return bIsComplete && bWasSuccessful; }
};

/** Delegate called when async generation completes */
DECLARE_DELEGATE_TwoParams(FOnVoxelGenerationComplete, FVoxelGenerationHandle /*Handle*/, bool /*bSuccess*/);
