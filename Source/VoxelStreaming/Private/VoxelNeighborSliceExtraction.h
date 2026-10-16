// Copyright Daniel Raquel. All Rights Reserved.

// Source-agnostic neighbor slice/edge/corner extraction for FVoxelMeshingRequest.
//
// The 26-neighborhood extraction logic (face slices + deep planes, edge strips + deep grids,
// corner voxels + deep boxes, EdgeCornerFlags, NeighborPlaneDepth), reading through two callables
// so it is independent of where the voxels live. Its user is the collision cook worker, which reads
// pre-grabbed shared voxel snapshots (edit-merged), off the game thread (P4a): collision is meshed
// whole-chunk, boundary faces included. Render meshing needs no slices: chunk meshes are the Interior
// domain and seam jobs read whole neighbour snapshots (the game-thread path that fed render meshes,
// UVoxelChunkManager::ExtractNeighborEdgeSlices, went with the legacy boundary path in M5).

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

struct FVoxelMeshingRequest;
struct FVoxelData;

namespace VoxelNeighborSlices
{
	/**
	 * Fill OutRequest's neighbor slice/edge/corner arrays + flags from an arbitrary voxel source.
	 *
	 * @param ChunkSize        Voxels per chunk axis.
	 * @param ChunkCoord       The chunk whose neighbors are extracted.
	 * @param bDeepOff         -VoxelDeepOff: single plane only (no deep data).
	 * @param bDeepFull        -VoxelDeepFull: 2*stride deep planes (else stride+1).
	 * @param HasNeighborData  Whether a neighbor chunk has voxel data available.
	 * @param GetNeighborVoxel Read one (edit-merged) voxel from a neighbor chunk.
	 * @param OutRequest       Request to fill (LODLevel must already be set).
	 */
	void Extract(
		int32 ChunkSize,
		const FIntVector& ChunkCoord,
		bool bDeepOff,
		bool bDeepFull,
		TFunctionRef<bool(const FIntVector&)> HasNeighborData,
		TFunctionRef<FVoxelData(const FIntVector&, int32, int32, int32)> GetNeighborVoxel,
		FVoxelMeshingRequest& OutRequest);
}
