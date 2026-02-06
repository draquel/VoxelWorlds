// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelData.h"
#include "VoxelEditTypes.generated.h"

/**
 * Brush shape for voxel editing operations.
 */
UENUM(BlueprintType)
enum class EVoxelBrushShape : uint8
{
	/** Spherical brush - affects voxels within a radius */
	Sphere,

	/** Cubic brush - affects voxels within a box */
	Cube,

	/** Cylindrical brush - circular in XY, extends in Z */
	Cylinder
};

/**
 * Falloff type for brush edges.
 */
UENUM(BlueprintType)
enum class EVoxelBrushFalloff : uint8
{
	/** Linear falloff - constant slope */
	Linear,

	/** Smooth (hermite) falloff - gradual edges */
	Smooth,

	/** Sharp falloff - minimal transition */
	Sharp
};

/**
 * Parameters for voxel brush operations.
 *
 * Controls the shape, size, and behavior of editing brushes.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelBrushParams
{
	GENERATED_BODY()

	/** Shape of the brush */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Brush")
	EVoxelBrushShape Shape = EVoxelBrushShape::Sphere;

	/** Radius of the brush in world units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Brush", meta = (ClampMin = "1.0", ClampMax = "10000.0"))
	float Radius = 200.0f;

	/** Strength of the edit operation (0-1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Brush", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Strength = 1.0f;

	/** Falloff type for brush edges */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Brush")
	EVoxelBrushFalloff FalloffType = EVoxelBrushFalloff::Smooth;

	/** Material ID to use for Paint mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Brush")
	uint8 MaterialID = 1;

	/** Density change amount for Add/Subtract modes (0-255) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Brush", meta = (ClampMin = "1", ClampMax = "255"))
	uint8 DensityDelta = 50;

	FVoxelBrushParams() = default;

	/**
	 * Calculate falloff factor for a given normalized distance (0=center, 1=edge).
	 * @param NormalizedDistance Distance from center, 0.0 to 1.0+
	 * @return Falloff factor, 1.0 at center, 0.0 at edge and beyond
	 */
	float GetFalloff(float NormalizedDistance) const
	{
		if (NormalizedDistance >= 1.0f)
		{
			return 0.0f;
		}
		if (NormalizedDistance <= 0.0f)
		{
			return 1.0f;
		}

		switch (FalloffType)
		{
		case EVoxelBrushFalloff::Linear:
			return 1.0f - NormalizedDistance;

		case EVoxelBrushFalloff::Smooth:
			// Hermite interpolation: 3t^2 - 2t^3
			{
				const float t = NormalizedDistance;
				return 1.0f - (3.0f * t * t - 2.0f * t * t * t);
			}

		case EVoxelBrushFalloff::Sharp:
			// Squared falloff - stays strong until near edge
			{
				const float inv = 1.0f - NormalizedDistance;
				return inv * inv;
			}

		default:
			return 1.0f - NormalizedDistance;
		}
	}
};

/**
 * Single voxel edit record.
 *
 * Stores the before/after state of a single voxel modification.
 * Used for undo/redo and sparse edit storage.
 *
 * Memory: ~20 bytes per edit
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelEdit
{
	GENERATED_BODY()

	/** Position within chunk (0 to ChunkSize-1 for each axis) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edit")
	FIntVector LocalPosition = FIntVector::ZeroValue;

	/** New voxel data after edit */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edit")
	FVoxelData NewData;

	/** Original voxel data before edit (for undo) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edit")
	FVoxelData OriginalData;

	/** Type of edit operation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edit")
	EEditMode EditMode = EEditMode::Set;

	/** Timestamp when edit was applied */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edit")
	double Timestamp = 0.0;

	FVoxelEdit() = default;

	FVoxelEdit(const FIntVector& InLocalPos, const FVoxelData& InNewData, const FVoxelData& InOriginal, EEditMode InMode)
		: LocalPosition(InLocalPos)
		, NewData(InNewData)
		, OriginalData(InOriginal)
		, EditMode(InMode)
		, Timestamp(FPlatformTime::Seconds())
	{
	}

	/**
	 * Convert local position to linear index within chunk.
	 * @param ChunkSize Number of voxels per edge
	 * @return Linear index for array access
	 */
	FORCEINLINE int32 GetVoxelIndex(int32 ChunkSize) const
	{
		return LocalPosition.X + LocalPosition.Y * ChunkSize + LocalPosition.Z * ChunkSize * ChunkSize;
	}

	/**
	 * Check if local position is valid for given chunk size.
	 */
	FORCEINLINE bool IsValidPosition(int32 ChunkSize) const
	{
		return LocalPosition.X >= 0 && LocalPosition.X < ChunkSize
			&& LocalPosition.Y >= 0 && LocalPosition.Y < ChunkSize
			&& LocalPosition.Z >= 0 && LocalPosition.Z < ChunkSize;
	}
};

/**
 * Per-chunk sparse edit storage.
 *
 * Stores edits for a single chunk using a sparse TMap.
 * Only modified voxels consume memory, making this efficient
 * for worlds with few edits.
 *
 * Memory: ~32 bytes base + ~32 bytes per edit
 * Thread Safety: Not thread-safe, use external synchronization
 */
USTRUCT()
struct VOXELCORE_API FChunkEditLayer
{
	GENERATED_BODY()

	/** Chunk coordinate this layer belongs to */
	UPROPERTY()
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** Sparse map of linear index to edit record */
	UPROPERTY()
	TMap<int32, FVoxelEdit> Edits;

	/** Chunk size (voxels per edge) for index calculations */
	UPROPERTY()
	int32 ChunkSize = VOXEL_DEFAULT_CHUNK_SIZE;

	FChunkEditLayer() = default;

	explicit FChunkEditLayer(const FIntVector& InChunkCoord, int32 InChunkSize = VOXEL_DEFAULT_CHUNK_SIZE)
		: ChunkCoord(InChunkCoord)
		, ChunkSize(InChunkSize)
	{
	}

	/**
	 * Apply an edit to this layer.
	 * Overwrites any existing edit at the same position.
	 */
	void ApplyEdit(const FVoxelEdit& Edit)
	{
		const int32 Index = Edit.GetVoxelIndex(ChunkSize);
		Edits.Add(Index, Edit);
	}

	/**
	 * Remove an edit at a local position.
	 * @return True if an edit was removed
	 */
	bool RemoveEdit(const FIntVector& LocalPos)
	{
		const int32 Index = LocalPos.X + LocalPos.Y * ChunkSize + LocalPos.Z * ChunkSize * ChunkSize;
		return Edits.Remove(Index) > 0;
	}

	/**
	 * Get edit at a local position.
	 * @return Pointer to edit or nullptr if no edit exists
	 */
	const FVoxelEdit* GetEdit(const FIntVector& LocalPos) const
	{
		const int32 Index = LocalPos.X + LocalPos.Y * ChunkSize + LocalPos.Z * ChunkSize * ChunkSize;
		return Edits.Find(Index);
	}

	/**
	 * Get merged voxel data, preferring edited data over procedural.
	 * @param LocalPos Position within chunk
	 * @param ProceduralData Original procedural voxel data
	 * @return Edited data if present, otherwise procedural data
	 */
	FVoxelData GetMergedVoxel(const FIntVector& LocalPos, const FVoxelData& ProceduralData) const
	{
		if (const FVoxelEdit* Edit = GetEdit(LocalPos))
		{
			return Edit->NewData;
		}
		return ProceduralData;
	}

	/**
	 * Check if this layer has any edits.
	 */
	FORCEINLINE bool IsEmpty() const
	{
		return Edits.Num() == 0;
	}

	/**
	 * Get number of edits in this layer.
	 */
	FORCEINLINE int32 GetEditCount() const
	{
		return Edits.Num();
	}

	/**
	 * Clear all edits from this layer.
	 */
	void Clear()
	{
		Edits.Empty();
	}

	/**
	 * Get approximate memory usage in bytes.
	 */
	SIZE_T GetMemoryUsage() const
	{
		return sizeof(FChunkEditLayer) + Edits.GetAllocatedSize();
	}
};

/**
 * Undo/redo operation containing a batch of edits.
 *
 * Represents a single user action that may affect multiple voxels
 * across multiple chunks. All edits in an operation are undone/redone together.
 *
 * Memory: ~64 bytes base + ~32 bytes per edit
 */
USTRUCT()
struct VOXELCORE_API FVoxelEditOperation
{
	GENERATED_BODY()

	/** Unique identifier for this operation */
	UPROPERTY()
	uint64 OperationId = 0;

	/** All voxel edits in this operation */
	UPROPERTY()
	TArray<FVoxelEdit> Edits;

	/** Chunk coordinates affected by this operation */
	UPROPERTY()
	TArray<FIntVector> AffectedChunks;

	/** Human-readable description of the operation */
	UPROPERTY()
	FString Description;

	/** Timestamp when operation was created */
	UPROPERTY()
	double Timestamp = 0.0;

	FVoxelEditOperation() = default;

	explicit FVoxelEditOperation(uint64 InId, const FString& InDescription = TEXT(""))
		: OperationId(InId)
		, Description(InDescription)
		, Timestamp(FPlatformTime::Seconds())
	{
	}

	/**
	 * Add an edit to this operation.
	 * Also tracks the affected chunk if not already tracked.
	 */
	void AddEdit(const FVoxelEdit& Edit, const FIntVector& ChunkCoord)
	{
		Edits.Add(Edit);
		AffectedChunks.AddUnique(ChunkCoord);
	}

	/**
	 * Check if operation has any edits.
	 */
	FORCEINLINE bool IsEmpty() const
	{
		return Edits.Num() == 0;
	}

	/**
	 * Get number of edits in this operation.
	 */
	FORCEINLINE int32 GetEditCount() const
	{
		return Edits.Num();
	}

	/**
	 * Get approximate memory usage in bytes.
	 */
	SIZE_T GetMemoryUsage() const
	{
		return sizeof(FVoxelEditOperation)
			+ Edits.GetAllocatedSize()
			+ AffectedChunks.GetAllocatedSize()
			+ Description.GetAllocatedSize();
	}
};
