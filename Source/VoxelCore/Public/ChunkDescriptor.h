// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelData.h"
#include "ChunkDescriptor.generated.h"

/**
 * Chunk metadata and voxel storage.
 *
 * Contains all data needed to describe a chunk's state and contents.
 * VoxelData array is sized to ChunkSize^3 elements.
 *
 * Memory: ~128 KB for 32^3 chunk (voxel data only)
 * Thread Safety: Not thread-safe, use external synchronization
 *
 * @see Documentation/DATA_STRUCTURES.md
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FChunkDescriptor
{
	GENERATED_BODY()

	/** Chunk position in chunk coordinate space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** Current LOD level (0 = finest detail) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	int32 LODLevel = 0;

	/** Number of voxels per edge (32, 64, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	int32 ChunkSize = VOXEL_DEFAULT_CHUNK_SIZE;

	/** Voxel data array (ChunkSize^3 elements) */
	UPROPERTY()
	TArray<FVoxelData> VoxelData;

	/** World-space bounding box */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	FBox Bounds = FBox(ForceInit);

	/** Chunk needs mesh regeneration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	bool bIsDirty = false;

	/** Chunk has player edits applied */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	bool bHasEdits = false;

	/** LOD transition blend factor (0 = this LOD, 1 = next LOD) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	float MorphFactor = 0.0f;

	/** Seed used for procedural generation (stored as int32 for Blueprint compatibility) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	int32 GenerationSeed = 0;

	/** Current streaming state */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chunk")
	EChunkState State = EChunkState::Unloaded;

	/** Default constructor */
	FChunkDescriptor() = default;

	/** Construct with coordinates and size */
	FChunkDescriptor(const FIntVector& InChunkCoord, int32 InChunkSize = VOXEL_DEFAULT_CHUNK_SIZE, int32 InLODLevel = 0)
		: ChunkCoord(InChunkCoord)
		, LODLevel(InLODLevel)
		, ChunkSize(InChunkSize)
	{
	}

	/** Allocate voxel data array for current chunk size */
	void AllocateVoxelData()
	{
		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		VoxelData.SetNumZeroed(TotalVoxels);
	}

	/** Clear voxel data to free memory */
	void ClearVoxelData()
	{
		VoxelData.Empty();
	}

	/** Get total number of voxels in this chunk */
	FORCEINLINE int32 GetTotalVoxels() const
	{
		return ChunkSize * ChunkSize * ChunkSize;
	}

	/** Convert local voxel position to linear index */
	FORCEINLINE int32 GetVoxelIndex(const FIntVector& LocalPos) const
	{
		check(LocalPos.X >= 0 && LocalPos.X < ChunkSize);
		check(LocalPos.Y >= 0 && LocalPos.Y < ChunkSize);
		check(LocalPos.Z >= 0 && LocalPos.Z < ChunkSize);
		return LocalPos.X + LocalPos.Y * ChunkSize + LocalPos.Z * ChunkSize * ChunkSize;
	}

	/** Convert linear index to local voxel position */
	FORCEINLINE FIntVector GetVoxelPosition(int32 Index) const
	{
		check(Index >= 0 && Index < GetTotalVoxels());
		return FIntVector(
			Index % ChunkSize,
			(Index / ChunkSize) % ChunkSize,
			Index / (ChunkSize * ChunkSize)
		);
	}

	/** Get voxel at local position */
	FORCEINLINE FVoxelData GetVoxel(const FIntVector& LocalPos) const
	{
		const int32 Index = GetVoxelIndex(LocalPos);
		return VoxelData.IsValidIndex(Index) ? VoxelData[Index] : FVoxelData::Air();
	}

	/** Set voxel at local position */
	FORCEINLINE void SetVoxel(const FIntVector& LocalPos, const FVoxelData& Data)
	{
		const int32 Index = GetVoxelIndex(LocalPos);
		if (VoxelData.IsValidIndex(Index))
		{
			VoxelData[Index] = Data;
			bIsDirty = true;
		}
	}

	/** Get voxel by linear index */
	FORCEINLINE FVoxelData GetVoxelByIndex(int32 Index) const
	{
		return VoxelData.IsValidIndex(Index) ? VoxelData[Index] : FVoxelData::Air();
	}

	/** Set voxel by linear index */
	FORCEINLINE void SetVoxelByIndex(int32 Index, const FVoxelData& Data)
	{
		if (VoxelData.IsValidIndex(Index))
		{
			VoxelData[Index] = Data;
			bIsDirty = true;
		}
	}

	/** Check if local position is within chunk bounds */
	FORCEINLINE bool IsValidLocalPosition(const FIntVector& LocalPos) const
	{
		return LocalPos.X >= 0 && LocalPos.X < ChunkSize
			&& LocalPos.Y >= 0 && LocalPos.Y < ChunkSize
			&& LocalPos.Z >= 0 && LocalPos.Z < ChunkSize;
	}

	/** Check if voxel data is allocated */
	FORCEINLINE bool HasVoxelData() const
	{
		return VoxelData.Num() == GetTotalVoxels();
	}

	/** Get memory usage in bytes */
	SIZE_T GetMemoryUsage() const
	{
		return sizeof(FChunkDescriptor) + VoxelData.GetAllocatedSize();
	}

	/** Unique identifier combining coords and LOD */
	FORCEINLINE uint64 GetUniqueID() const
	{
		// Pack coords (16 bits each, signed) and LOD (8 bits) into 64 bits
		uint64 X = static_cast<uint16>(ChunkCoord.X);
		uint64 Y = static_cast<uint16>(ChunkCoord.Y);
		uint64 Z = static_cast<uint16>(ChunkCoord.Z);
		uint64 LOD = static_cast<uint8>(LODLevel);
		return (X) | (Y << 16) | (Z << 32) | (LOD << 48);
	}
};
