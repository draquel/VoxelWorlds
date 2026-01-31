// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"
#include "VoxelMeshingTypes.generated.h"

/**
 * Request structure for mesh generation.
 *
 * Contains all data needed to generate a mesh from voxel data.
 * Supports neighbor data for seamless chunk boundaries.
 *
 * @see IVoxelMesher
 */
USTRUCT(BlueprintType)
struct VOXELMESHING_API FVoxelMeshingRequest
{
	GENERATED_BODY()

	/** Chunk position in chunk coordinate space */
	UPROPERTY()
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** LOD level for this mesh */
	UPROPERTY()
	int32 LODLevel = 0;

	/** Size of the chunk in voxels per axis */
	UPROPERTY()
	int32 ChunkSize = 32;

	/** World-space size of each voxel */
	UPROPERTY()
	float VoxelSize = 100.0f;

	/** Input voxel data (ChunkSize^3 elements) */
	UPROPERTY()
	TArray<FVoxelData> VoxelData;

	/**
	 * Neighbor chunk data for seamless boundaries.
	 * Each array contains ChunkSize^2 voxels representing the edge slice.
	 * Empty arrays mean boundary faces will be generated (chunk edge).
	 */
	UPROPERTY()
	TArray<FVoxelData> NeighborXPos;  // +X neighbor (East)

	UPROPERTY()
	TArray<FVoxelData> NeighborXNeg;  // -X neighbor (West)

	UPROPERTY()
	TArray<FVoxelData> NeighborYPos;  // +Y neighbor (North)

	UPROPERTY()
	TArray<FVoxelData> NeighborYNeg;  // -Y neighbor (South)

	UPROPERTY()
	TArray<FVoxelData> NeighborZPos;  // +Z neighbor (Top)

	UPROPERTY()
	TArray<FVoxelData> NeighborZNeg;  // -Z neighbor (Bottom)

	/** Get voxel at local position */
	FORCEINLINE const FVoxelData& GetVoxel(int32 X, int32 Y, int32 Z) const
	{
		const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
		return VoxelData[Index];
	}

	/** Check if request has valid voxel data */
	FORCEINLINE bool IsValid() const
	{
		return VoxelData.Num() == ChunkSize * ChunkSize * ChunkSize;
	}

	/** Get expected neighbor slice size */
	FORCEINLINE int32 GetNeighborSliceSize() const
	{
		return ChunkSize * ChunkSize;
	}

	/** Check if a neighbor slice is present */
	FORCEINLINE bool HasNeighbor(int32 Face) const
	{
		switch (Face)
		{
		case 0: return NeighborXPos.Num() == GetNeighborSliceSize();
		case 1: return NeighborXNeg.Num() == GetNeighborSliceSize();
		case 2: return NeighborYPos.Num() == GetNeighborSliceSize();
		case 3: return NeighborYNeg.Num() == GetNeighborSliceSize();
		case 4: return NeighborZPos.Num() == GetNeighborSliceSize();
		case 5: return NeighborZNeg.Num() == GetNeighborSliceSize();
		default: return false;
		}
	}
};

/**
 * Handle for tracking async meshing operations.
 *
 * Returned by async meshing calls, used to query status
 * and retrieve results.
 */
USTRUCT(BlueprintType)
struct VOXELMESHING_API FVoxelMeshingHandle
{
	GENERATED_BODY()

	/** Unique identifier for this request */
	UPROPERTY()
	uint64 RequestId = 0;

	/** Whether the meshing operation has completed */
	UPROPERTY()
	bool bIsComplete = false;

	/** Whether the operation completed successfully */
	UPROPERTY()
	bool bWasSuccessful = false;

	/** Chunk coordinate this handle refers to */
	UPROPERTY()
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** Default constructor */
	FVoxelMeshingHandle() = default;

	/** Construct with request ID */
	explicit FVoxelMeshingHandle(uint64 InRequestId, const FIntVector& InChunkCoord)
		: RequestId(InRequestId)
		, bIsComplete(false)
		, bWasSuccessful(false)
		, ChunkCoord(InChunkCoord)
	{
	}

	/** Check if handle is valid */
	FORCEINLINE bool IsValid() const
	{
		return RequestId != 0;
	}

	/** Reset handle to invalid state */
	void Reset()
	{
		RequestId = 0;
		bIsComplete = false;
		bWasSuccessful = false;
		ChunkCoord = FIntVector::ZeroValue;
	}
};

/**
 * Delegate called when an async meshing operation completes.
 *
 * @param Handle The handle for the completed operation
 * @param bSuccess Whether the operation completed successfully
 */
DECLARE_DELEGATE_TwoParams(FOnVoxelMeshingComplete, FVoxelMeshingHandle /*Handle*/, bool /*bSuccess*/);

/**
 * Statistics for a meshing operation.
 */
USTRUCT(BlueprintType)
struct VOXELMESHING_API FVoxelMeshingStats
{
	GENERATED_BODY()

	/** Number of vertices generated */
	UPROPERTY()
	uint32 VertexCount = 0;

	/** Number of indices generated */
	UPROPERTY()
	uint32 IndexCount = 0;

	/** Number of faces generated */
	UPROPERTY()
	uint32 FaceCount = 0;

	/** Time taken to generate mesh in milliseconds */
	UPROPERTY()
	float GenerationTimeMs = 0.0f;

	/** Number of solid voxels processed */
	UPROPERTY()
	uint32 SolidVoxelCount = 0;

	/** Number of faces culled due to neighbors */
	UPROPERTY()
	uint32 CulledFaceCount = 0;

	/** Get triangle count */
	FORCEINLINE uint32 GetTriangleCount() const
	{
		return IndexCount / 3;
	}
};

/**
 * Configuration for mesh generation.
 */
USTRUCT(BlueprintType)
struct VOXELMESHING_API FVoxelMeshingConfig
{
	GENERATED_BODY()

	/** Maximum vertices per mesh (for buffer pre-allocation) */
	UPROPERTY()
	uint32 MaxVerticesPerChunk = 65536;

	/** Maximum indices per mesh (for buffer pre-allocation) */
	UPROPERTY()
	uint32 MaxIndicesPerChunk = 196608;

	/** Whether to generate UVs */
	UPROPERTY()
	bool bGenerateUVs = true;

	/** Whether to calculate ambient occlusion */
	UPROPERTY()
	bool bCalculateAO = false;

	/** UV scale for texture mapping */
	UPROPERTY()
	float UVScale = 1.0f;
};
