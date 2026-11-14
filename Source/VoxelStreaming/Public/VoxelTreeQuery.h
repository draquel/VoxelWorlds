// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelTreeInjector.h"
#include "VoxelWaterLevels.h"

class UVoxelWorldConfiguration;
class IVoxelWorldMode;

/**
 * A cubic world's voxel trees as they stand (world modes review §21, WM-C8): the trees generation injects
 * (FVoxelTreeInjector), recomputed from the configuration and the world mode, and which of their blocks are still there in
 * the voxels as the reader gives them (the edit-merged voxels: a felled tree has none). The chunk manager answers with it
 * (UVoxelChunkManager::FindVoxelTreeAt, FindVoxelTreesNear, GetVoxelTreeBlocks) once it has checked the world has trees.
 */
struct VOXELSTREAMING_API FVoxelTreeQuery
{
	/** Reads the voxel at a world position (the chunk manager's edit-merged GetVoxelAt) */
	using FReadVoxel = TFunctionRef<FVoxelData(const FVector& WorldPosition)>;

	/**
	 * @param Config       The world's configuration (null: a voxel size of 100 at the origin)
	 * @param GlobalVoxel  A voxel on the world's grid
	 * @return The world position of the voxel's centre
	 */
	static FVector GlobalVoxelToWorldCentre(const UVoxelWorldConfiguration* Config, const FIntVector& GlobalVoxel);

	/**
	 * The standing tree whose trunk holds the voxel at a world position (where two trunks touch, the one with more trunk left).
	 * @param Conditioning The conditioning the trees there were injected with (its lines: a complete gather over the
	 *        position's chunk and the chunks beside it), or null
	 * @return False when no tree's trunk block is there
	 */
	static bool FindAt(const UVoxelWorldConfiguration& Config, const IVoxelWorldMode& WorldMode, const FVoxelWaterLevels& Water, const FVector& WorldPos,
		FReadVoxel ReadVoxel, FVoxelTreeInstance& OutTree, const struct FVoxelConditioning* Conditioning = nullptr);

	/**
	 * The standing trees (a trunk block over their base left) within Radius of a world position in XY, nearest first.
	 * @param Conditioning The conditioning the trees there were injected with (its lines: a complete gather over the
	 *        chunks the radius touches), or null
	 */
	static void FindNear(const UVoxelWorldConfiguration& Config, const IVoxelWorldMode& WorldMode, const FVoxelWaterLevels& Water, const FVector& WorldPos, float Radius,
		FReadVoxel ReadVoxel, TArray<FVoxelTreeInstance>& OutTrees, const struct FVoxelConditioning* Conditioning = nullptr);

	/** A tree's blocks still in the voxels (each block once, the trunk's over the canopy's where they overlap). */
	static void GetBlocks(const UVoxelWorldConfiguration& Config, const FVoxelTreeInstance& Tree, FReadVoxel ReadVoxel,
		TArray<FVoxelTreeVoxel>& OutBlocks);
};
