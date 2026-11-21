// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * What a gather by bounds is for, which decides how a source answers for the things it may have to work out first: a
 * conditioner's lines (IVoxelTerrainConditioner; a road's route) and a water body source's bodies
 * (IVoxelWaterBodySource; a traced river and the lakes along it). What a source has at hand is always given: a
 * conditioner's zones, a level's hand-placed lines, rivers and lakes.
 */
enum class EVoxelConditioningGather : uint8
{
	/**
	 * Everything that reaches the place, computed then and there if it is not at hand: a chunk's generation request (its
	 * voxels are generated once) and every reader that must agree with the generated voxels at a place (a surface query
	 * or a water sample at a point, a chunk's scatter, the PCG sampler).
	 */
	Complete,

	/**
	 * What is at hand now, without computing any: the readers whose regions are too wide to wait for (the far field, the
	 * map, the drawing). The source works out the rest later and announces it (a conditioner through
	 * UVoxelChunkManager::OnTerrainConditioningChanged, a water body source through OnWaterBodiesChanged), and the
	 * reader draws again.
	 */
	Ready
};
