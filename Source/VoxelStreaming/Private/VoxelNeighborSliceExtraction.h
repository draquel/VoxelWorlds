// Copyright Daniel Raquel. All Rights Reserved.

// Source-agnostic neighbor slice/edge/corner extraction for FVoxelMeshingRequest.
//
// The 26-neighborhood extraction logic (face slices + deep planes, edge strips + deep grids,
// corner voxels + deep boxes, EdgeCornerFlags, NeighborPlaneDepth) is shared between two data
// sources through the two callables:
//   - the game-thread path (UVoxelChunkManager::ExtractNeighborEdgeSlices) reads live chunk
//     states + edit layers;
//   - the collision cook worker path reads pre-grabbed shared voxel snapshots (edit-merged),
//     moving the ~6 ms/cook of cache-missing strided reads off the game thread (P4a).
// One body, two wrappers — behavior stays bit-identical between the paths.

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
