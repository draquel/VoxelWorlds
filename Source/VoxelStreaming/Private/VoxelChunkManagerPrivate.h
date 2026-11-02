// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// What the chunk manager's translation units share and nothing outside VoxelStreaming reads: the console variables read
// in more than one of them (defined in VoxelChunkManager.cpp).

/** voxel.Stream.MeshStuckSeconds: the in-flight job watchdog's limit (the watchdog and AuditResidency read it) */
extern TAutoConsoleVariable<float> CVarStreamMeshStuckSeconds;
