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
 * Works on 2D tiles: BuildColumnMask() + GenerateWaterMeshFromMask() for independent
 * water tiles that aggregate water from multiple Z chunks per XY column.
 *
 * Usage:
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
