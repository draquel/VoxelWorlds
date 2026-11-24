// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVoxelChunkManager;

/**
 * The streaming probe (`voxel.Stream.Probe`, G275): while it records, the game thread runs a few fixed pieces of work at
 * the start of each chunk manager tick and times them, and the tick itself, by the clock and by the cycles the thread
 * spent on a processor. The same work taking longer on the clock with the same cycles means the thread was off its
 * processor (blocked or preempted); more cycles means the processor ran it slower (a busy sibling hardware thread, a
 * lower clock rate, a full cache). The chunk manager calls the two functions; both return at once when nothing records.
 */
namespace VoxelStreamProbe
{
	/** Before the tick's work: closes the last frame's row and runs the fixed work. */
	void BeginTick(UVoxelChunkManager& Manager);

	/** After the tick's work: the tick's time and its sections. */
	void EndTick(UVoxelChunkManager& Manager);
}
