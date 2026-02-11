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

	/** World origin offset - all chunk positions are relative to this */
	UPROPERTY()
	FVector WorldOrigin = FVector::ZeroVector;

	/** Input voxel data (ChunkSize^3 elements) */
	UPROPERTY()
	TArray<FVoxelData> VoxelData;

	/**
	 * Face neighbor chunk data for seamless boundaries.
	 * Each array contains ChunkSize^2 voxels representing the face slice.
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

	/**
	 * Edge neighbor data for diagonal chunk boundaries (Marching Cubes).
	 * Each array contains ChunkSize voxels representing an edge strip.
	 * Named by the two positive/negative axes involved.
	 */
	UPROPERTY()
	TArray<FVoxelData> EdgeXPosYPos;  // +X+Y edge (Z varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeXPosYNeg;  // +X-Y edge (Z varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeXNegYPos;  // -X+Y edge (Z varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeXNegYNeg;  // -X-Y edge (Z varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeXPosZPos;  // +X+Z edge (Y varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeXPosZNeg;  // +X-Z edge (Y varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeXNegZPos;  // -X+Z edge (Y varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeXNegZNeg;  // -X-Z edge (Y varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeYPosZPos;  // +Y+Z edge (X varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeYPosZNeg;  // +Y-Z edge (X varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeYNegZPos;  // -Y+Z edge (X varies)

	UPROPERTY()
	TArray<FVoxelData> EdgeYNegZNeg;  // -Y-Z edge (X varies)

	/**
	 * Corner neighbor data for diagonal chunk boundaries (Marching Cubes).
	 * Single voxel at each of the 8 chunk corners.
	 */
	UPROPERTY()
	FVoxelData CornerXPosYPosZPos;  // +X+Y+Z corner

	UPROPERTY()
	FVoxelData CornerXPosYPosZNeg;  // +X+Y-Z corner

	UPROPERTY()
	FVoxelData CornerXPosYNegZPos;  // +X-Y+Z corner

	UPROPERTY()
	FVoxelData CornerXPosYNegZNeg;  // +X-Y-Z corner

	UPROPERTY()
	FVoxelData CornerXNegYPosZPos;  // -X+Y+Z corner

	UPROPERTY()
	FVoxelData CornerXNegYPosZNeg;  // -X+Y-Z corner

	UPROPERTY()
	FVoxelData CornerXNegYNegZPos;  // -X-Y+Z corner

	UPROPERTY()
	FVoxelData CornerXNegYNegZNeg;  // -X-Y-Z corner

	/** Flags indicating which edge/corner data is valid */
	UPROPERTY()
	uint32 EdgeCornerFlags = 0;

	/**
	 * Flags indicating which faces border coarser (higher LOD level) neighbors.
	 * Per Lengyel's Transvoxel: the FINER chunk generates transition cells
	 * so the face corners match the coarser neighbor's MC grid exactly.
	 * Bit 0: -X, Bit 1: +X, Bit 2: -Y, Bit 3: +Y, Bit 4: -Z, Bit 5: +Z
	 */
	UPROPERTY()
	uint8 TransitionFaces = 0;

	/**
	 * LOD levels of neighbor chunks for each face.
	 * Used by Transvoxel to determine transition cell stride.
	 * Order: -X, +X, -Y, +Y, -Z, +Z
	 * Value of -1 means no neighbor (chunk at world boundary).
	 */
	UPROPERTY()
	int32 NeighborLODLevels[6] = {-1, -1, -1, -1, -1, -1};

	// Transition face flag bits
	static constexpr uint8 TRANSITION_XNEG = 1 << 0;
	static constexpr uint8 TRANSITION_XPOS = 1 << 1;
	static constexpr uint8 TRANSITION_YNEG = 1 << 2;
	static constexpr uint8 TRANSITION_YPOS = 1 << 3;
	static constexpr uint8 TRANSITION_ZNEG = 1 << 4;
	static constexpr uint8 TRANSITION_ZPOS = 1 << 5;

	// Edge flag bits (0-11)
	static constexpr uint32 EDGE_XPOS_YPOS = 1 << 0;
	static constexpr uint32 EDGE_XPOS_YNEG = 1 << 1;
	static constexpr uint32 EDGE_XNEG_YPOS = 1 << 2;
	static constexpr uint32 EDGE_XNEG_YNEG = 1 << 3;
	static constexpr uint32 EDGE_XPOS_ZPOS = 1 << 4;
	static constexpr uint32 EDGE_XPOS_ZNEG = 1 << 5;
	static constexpr uint32 EDGE_XNEG_ZPOS = 1 << 6;
	static constexpr uint32 EDGE_XNEG_ZNEG = 1 << 7;
	static constexpr uint32 EDGE_YPOS_ZPOS = 1 << 8;
	static constexpr uint32 EDGE_YPOS_ZNEG = 1 << 9;
	static constexpr uint32 EDGE_YNEG_ZPOS = 1 << 10;
	static constexpr uint32 EDGE_YNEG_ZNEG = 1 << 11;

	// Corner flag bits (12-19)
	static constexpr uint32 CORNER_XPOS_YPOS_ZPOS = 1 << 12;
	static constexpr uint32 CORNER_XPOS_YPOS_ZNEG = 1 << 13;
	static constexpr uint32 CORNER_XPOS_YNEG_ZPOS = 1 << 14;
	static constexpr uint32 CORNER_XPOS_YNEG_ZNEG = 1 << 15;
	static constexpr uint32 CORNER_XNEG_YPOS_ZPOS = 1 << 16;
	static constexpr uint32 CORNER_XNEG_YPOS_ZNEG = 1 << 17;
	static constexpr uint32 CORNER_XNEG_YNEG_ZPOS = 1 << 18;
	static constexpr uint32 CORNER_XNEG_YNEG_ZNEG = 1 << 19;

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

	/** Get the world-space position of this chunk's origin (includes WorldOrigin offset) */
	FORCEINLINE FVector GetChunkWorldPosition() const
	{
		// All chunks cover the same world area regardless of LOD level
		// LOD only affects voxel resolution within the chunk, not chunk position
		return WorldOrigin + FVector(ChunkCoord) * static_cast<float>(ChunkSize) * VoxelSize;
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

	/** Check if an edge strip is present */
	FORCEINLINE bool HasEdge(uint32 EdgeFlag) const
	{
		return (EdgeCornerFlags & EdgeFlag) != 0;
	}

	/** Check if a corner is present */
	FORCEINLINE bool HasCorner(uint32 CornerFlag) const
	{
		return (EdgeCornerFlags & CornerFlag) != 0;
	}

	/** Get expected edge strip size */
	FORCEINLINE int32 GetEdgeStripSize() const
	{
		return ChunkSize;
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
	bool bCalculateAO = true;

	/** UV scale for texture mapping */
	UPROPERTY()
	float UVScale = 1.0f;

	/**
	 * Whether to use greedy meshing algorithm.
	 * Greedy meshing merges adjacent coplanar faces with the same material
	 * into larger quads, significantly reducing triangle count (typically 40-60%).
	 * Disable for debugging or when per-voxel face data is needed.
	 * Only applies to cubic meshing.
	 */
	UPROPERTY()
	bool bUseGreedyMeshing = true;

	/**
	 * Use smooth (Marching Cubes) meshing instead of cubic.
	 * Smooth meshing interpolates vertex positions along cube edges where the
	 * density field crosses the isosurface, producing organic curved surfaces
	 * instead of blocky voxel geometry.
	 */
	UPROPERTY(EditAnywhere, Category = "Meshing")
	bool bUseSmoothMeshing = false;

	/**
	 * ISO surface threshold for smooth meshing (0.0-1.0).
	 * The isosurface is generated where density equals this value.
	 * Default 0.5 corresponds to density threshold 127 (VOXEL_SURFACE_THRESHOLD).
	 * Lower values produce larger/more solid meshes, higher values produce smaller.
	 * Only applies when bUseSmoothMeshing is true.
	 */
	UPROPERTY(EditAnywhere, Category = "Meshing", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bUseSmoothMeshing"))
	float IsoLevel = 0.5f;

	/**
	 * Use Transvoxel algorithm for seamless LOD transitions.
	 * Transvoxel generates special transition cells at LOD boundaries that
	 * properly connect high-resolution and low-resolution meshes without seams.
	 * Uses Eric Lengyel's official lookup tables for correct triangulation.
	 * Only applies when bUseSmoothMeshing is true.
	 *
	 * When disabled, falls back to skirt generation for LOD seam hiding.
	 */
	UPROPERTY(EditAnywhere, Category = "Meshing", meta = (EditCondition = "bUseSmoothMeshing"))
	bool bUseTransvoxel = true;

	/**
	 * Generate skirts along chunk boundaries to hide LOD seams.
	 * Skirts extend boundary edges outward to overlap with neighboring chunks,
	 * covering gaps between chunks at different LOD levels.
	 * Only applies when bUseSmoothMeshing is true and Transvoxel is disabled.
	 */
	UPROPERTY(EditAnywhere, Category = "Meshing", meta = (EditCondition = "bUseSmoothMeshing && !bUseTransvoxel"))
	bool bGenerateSkirts = true;

	/**
	 * Depth of skirts in voxel units.
	 * Larger values better hide LOD transitions but add more geometry.
	 * Default 2.0 provides good coverage for most LOD transitions.
	 */
	UPROPERTY(EditAnywhere, Category = "Meshing", meta = (ClampMin = "0.5", ClampMax = "8.0", EditCondition = "bUseSmoothMeshing && !bUseTransvoxel && bGenerateSkirts"))
	float SkirtDepth = 2.0f;
};
