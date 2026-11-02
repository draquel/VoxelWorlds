// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// What the chunk manager's translation units share and nothing outside VoxelStreaming reads: the console variables read
// in more than one of them (defined in VoxelChunkManager.cpp).

/** voxel.Stream.MeshStuckSeconds: the in-flight job watchdog's limit (the watchdog and AuditResidency read it) */
extern TAutoConsoleVariable<float> CVarStreamMeshStuckSeconds;

/** The tick's section timings (FVoxelTimingStats): the arithmetic on FPlatformTime::Seconds() stamps the tick takes. */
namespace VoxelTickTiming
{
	/** @return Milliseconds from a stamp to now (one clock read) */
	inline float MsSince(double StartSeconds) { return static_cast<float>((FPlatformTime::Seconds() - StartSeconds) * 1000.0); }

	/** @return Milliseconds between two stamps */
	inline float MsBetween(double StartSeconds, double EndSeconds) { return static_cast<float>((EndSeconds - StartSeconds) * 1000.0); }
}
