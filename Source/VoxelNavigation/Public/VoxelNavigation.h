// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelNavigation, Log, All);

/**
 * VoxelNavigation module — surface pathfinding over voxel terrain without an engine navmesh.
 *
 * FVoxelSurfacePathfinder is the pure A* over a 2.5-D cell grid (height-sampler driven, unit-testable);
 * UVoxelSurfaceNavigationSubsystem wraps it for a world: tile cache over the chunk manager's edit-merged /
 * analytic surface, blocked boxes, budgets, cvars and debug drawing.
 */
class FVoxelNavigationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
