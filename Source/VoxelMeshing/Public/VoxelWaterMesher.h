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
 * This is a standalone utility class — it does NOT implement IVoxelMesher
 * since water meshing is simpler and doesn't need GPU dispatch or LOD support.
 *
 * Usage:
 *   FChunkMeshData WaterMesh;
 *   FVoxelWaterMesher::GenerateWaterMesh(MeshingRequest, WaterMesh);
 *   if (WaterMesh.IsValid()) { WaterRenderer->UpdateChunkMeshFromCPU(...); }
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
	 */
	static void GenerateWaterMesh(const FVoxelMeshingRequest& Request, FChunkMeshData& OutMeshData);

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
	 * @param Request Meshing request for coordinate/size info
	 * @param SliceZ Z-level of the water surface (quad emitted at Z+1 face)
	 * @param U Start position along X axis
	 * @param V Start position along Y axis
	 * @param Width Quad width along X axis (in voxels)
	 * @param Height Quad height along Y axis (in voxels)
	 */
	static void EmitWaterQuad(
		FChunkMeshData& MeshData,
		const FVoxelMeshingRequest& Request,
		int32 SliceZ,
		int32 U, int32 V,
		int32 Width, int32 Height);
};
