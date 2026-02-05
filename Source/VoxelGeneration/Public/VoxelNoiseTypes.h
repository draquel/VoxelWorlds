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

	// ==================== World Mode Parameters ====================

	/** World generation mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	EWorldMode WorldMode = EWorldMode::InfinitePlane;

	/** Sea level height for terrain generation (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	float SeaLevel = 0.0f;

	/** Scale factor for noise-to-height conversion */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	float HeightScale = 5000.0f;

	/** Base height offset from sea level */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	float BaseHeight = 0.0f;

	// ==================== Biome Parameters ====================

	/** Whether to enable biome-based material selection */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	bool bEnableBiomes = true;

	/** Frequency for temperature noise (lower = larger biome regions) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	float TemperatureNoiseFrequency = 0.00005f;

	/** Frequency for moisture noise (lower = larger biome regions) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	float MoistureNoiseFrequency = 0.00007f;

	/** Seed offset for temperature noise (added to main seed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	int32 TemperatureSeedOffset = 1234;

	/** Seed offset for moisture noise (added to main seed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	int32 MoistureSeedOffset = 5678;

	// ==================== World Origin ====================

	/** World origin offset - all chunk positions are relative to this */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	FVector WorldOrigin = FVector::ZeroVector;

	FVoxelNoiseGenerationRequest() = default;

	/** Get the world position of this chunk's origin (includes WorldOrigin offset) */
	FVector GetChunkWorldPosition() const
	{
		// All chunks cover the same world area regardless of LOD level
		// LOD only affects voxel resolution within the chunk, not chunk position
		float ChunkWorldSize = ChunkSize * VoxelSize;
		return WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
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
