// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelScatterTypes.h"

struct FChunkMeshData;

/**
 * Extracts surface points from chunk mesh data for scatter placement.
 *
 * Downsamples mesh vertices to a manageable set of surface points
 * representing the terrain surface. Uses spatial hashing for deduplication
 * and filtering to ensure even distribution.
 *
 * Thread Safety: All methods are stateless and thread-safe.
 */
class VOXELSCATTER_API FVoxelSurfaceExtractor
{
public:
	/**
	 * Extract surface points from mesh data.
	 *
	 * Samples mesh vertices at approximately TargetPointSpacing intervals,
	 * filtering duplicates using spatial hashing.
	 *
	 * @param MeshData Source mesh with vertices
	 * @param ChunkCoord Chunk coordinate for output
	 * @param ChunkWorldOrigin World position of chunk origin (0,0,0 corner)
	 * @param TargetPointSpacing Approximate spacing between points (cm)
	 * @param LODLevel LOD level of the mesh
	 * @param OutSurfaceData Extracted surface points
	 */
	static void ExtractSurfacePoints(
		const FChunkMeshData& MeshData,
		const FIntVector& ChunkCoord,
		const FVector& ChunkWorldOrigin,
		float TargetPointSpacing,
		int32 LODLevel,
		FChunkSurfaceData& OutSurfaceData);

	/**
	 * Extract surface points with filtering by face type.
	 *
	 * @param MeshData Source mesh
	 * @param ChunkCoord Chunk coordinate
	 * @param ChunkWorldOrigin World position of chunk origin
	 * @param TargetPointSpacing Spacing between points
	 * @param LODLevel LOD level
	 * @param bTopFacesOnly Only extract top-facing surfaces
	 * @param OutSurfaceData Extracted surface points
	 */
	static void ExtractSurfacePointsFiltered(
		const FChunkMeshData& MeshData,
		const FIntVector& ChunkCoord,
		const FVector& ChunkWorldOrigin,
		float TargetPointSpacing,
		int32 LODLevel,
		bool bTopFacesOnly,
		FChunkSurfaceData& OutSurfaceData);

private:
	/**
	 * Decode material ID and face type from UV1.
	 */
	static void DecodeUV1Data(const FVector2f& UV1, uint8& OutMaterialID, EVoxelFaceType& OutFaceType);

	/**
	 * Decode biome ID and AO from vertex color.
	 */
	static void DecodeColorData(const FColor& Color, uint8& OutBiomeID, uint8& OutAO);

	/**
	 * Compute spatial hash for a position.
	 * Used for deduplication within grid cells.
	 */
	static uint32 ComputeSpatialHash(const FVector& Position, float CellSize);

	/**
	 * Get hash key for grid cell.
	 */
	static FIntVector GetGridCell(const FVector& Position, float CellSize);
};
