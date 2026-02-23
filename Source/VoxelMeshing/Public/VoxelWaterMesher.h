// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"

/**
 * Water surface mesher for voxel chunks.
 *
 * Generates water surface quads wherever water-flagged air voxels have
 * non-water above them (surface boundary). Uses greedy meshing to coalesce
 * adjacent water surface voxels into large quads for efficiency.
 *
 * Supports two modes:
 * - Per-chunk: GenerateWaterMesh() scans a single chunk's voxel data
 * - 2D tile: BuildColumnMask() + GenerateWaterMeshFromMask() for independent
 *   water tiles that aggregate water from multiple Z chunks per XY column
 *
 * Usage (per-chunk):
 *   FChunkMeshData WaterMesh;
 *   FVoxelWaterMesher::GenerateWaterMesh(MeshingRequest, WaterMesh, WaterLevel);
 *
 * Usage (2D tile):
 *   TArray<bool> Mask;
 *   Mask.SetNumZeroed(ChunkSize * ChunkSize);
 *   FVoxelWaterMesher::BuildColumnMask(VoxelData, ChunkSize, Mask);
 *   // OR multiple masks together from different Z chunks...
 *   FVoxelWaterMesher::GenerateWaterMeshFromMask(CombinedMask, ChunkSize, VoxelSize,
 *       TileWorldPos, WaterLevel, OutMeshData);
 */
class VOXELMESHING_API FVoxelWaterMesher
{
public:
	/**
	 * Generate water surface mesh for a chunk.
	 *
	 * Scans all voxels in the chunk and emits top-face quads at water surface
	 * boundaries (water-flagged air voxel with non-water or solid above).
	 * Uses greedy rectangle merging per Z-slice for optimal triangle count.
	 *
	 * @param Request Meshing request with voxel data and chunk info
	 * @param OutMeshData Output mesh data (cleared before use)
	 * @param WaterLevel World-space water level height (used as the Z for all water quads)
	 */
	static void GenerateWaterMesh(const FVoxelMeshingRequest& Request, FChunkMeshData& OutMeshData, float WaterLevel);

	/**
	 * Scan chunk voxel data and build a 2D column mask.
	 *
	 * A column is marked true if ANY voxel in it is air + water-flagged
	 * + not underground (cave interior).
	 *
	 * @param VoxelData Raw voxel data array (ChunkSize^3 elements)
	 * @param ChunkSize Number of voxels per chunk axis
	 * @param OutMask Pre-allocated bool array (ChunkSize * ChunkSize), filled with water presence
	 * @return True if any water was found
	 */
	static bool BuildColumnMask(
		const TArray<FVoxelData>& VoxelData,
		int32 ChunkSize,
		TArray<bool>& OutMask);

	/**
	 * Generate water mesh from a pre-built column mask.
	 *
	 * Takes a combined column mask (OR of multiple chunks' partial masks)
	 * and generates water surface geometry. Applies dilation to extend water
	 * under terrain at shorelines, then greedy-merges into large quads.
	 *
	 * @param ColumnMask Bool array (ChunkSize * ChunkSize) indicating water presence per column
	 * @param ChunkSize Number of voxels per chunk axis
	 * @param VoxelSize World-space size of each voxel
	 * @param TileWorldPosition World-space origin of the tile (for UV calculation)
	 * @param WaterLevel World-space Z height for the water surface
	 * @param OutMeshData Output mesh data (cleared before use)
	 */
	static void GenerateWaterMeshFromMask(
		const TArray<bool>& ColumnMask,
		int32 ChunkSize,
		float VoxelSize,
		const FVector& TileWorldPosition,
		float WaterLevel,
		FChunkMeshData& OutMeshData);

private:
	/** Water material ID used in UV1.x to identify water geometry */
	static constexpr uint8 WATER_MATERIAL_ID = 254;

	/**
	 * Check if a voxel at the given position is a water surface voxel.
	 * A water surface voxel is an air voxel with the water flag set,
	 * where the voxel above is either solid, has no water flag, or is out of bounds.
	 */
	static bool IsWaterSurface(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z);

	/**
	 * Get the voxel above position (Z+1), handling chunk boundaries via neighbor data.
	 * Returns true if the above voxel could be resolved, with the result in OutVoxel.
	 */
	static bool GetVoxelAbove(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z, FVoxelData& OutVoxel);

	/**
	 * Emit a greedy-merged water quad into the mesh data.
	 *
	 * @param MeshData Output mesh data to append to
	 * @param VoxelSize World-space size of each voxel
	 * @param TileWorldPos World-space origin of the tile (for UV calculation)
	 * @param LocalWaterZ Water surface Z in tile-local space
	 * @param U Start position along X axis
	 * @param V Start position along Y axis
	 * @param Width Quad width along X axis (in voxels)
	 * @param Height Quad height along Y axis (in voxels)
	 */
	static void EmitWaterQuad(
		FChunkMeshData& MeshData,
		float VoxelSize,
		const FVector& TileWorldPos,
		float LocalWaterZ,
		int32 U, int32 V,
		int32 Width, int32 Height);
};
