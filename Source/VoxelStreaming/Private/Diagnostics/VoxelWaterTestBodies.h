// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVoxelChunkManager;

/**
 * Lakes and river reaches laid by a script for a look or a measurement (the hydrology plan's H3, until its H4 gives
 * the hand-placed river and lake their actors): the cvar voxel.Water.TestBodies names a text file, and a world that
 * starts while it is set adds the file's bodies to its chunk manager at its start, before the first generation
 * request, as a hand-placed body registers. A river reach comes with its bed, a lower-only conditioning line made from
 * the same vertices. Development builds only.
 */
namespace VoxelWaterTestBodies
{
	/**
	 * Add the bodies of the file voxel.Water.TestBodies names to a chunk manager (nothing with the cvar empty).
	 * Called at the end of UVoxelChunkManager::Initialize.
	 * @param Manager The world's chunk manager
	 */
	void Apply(UVoxelChunkManager& Manager);
}
