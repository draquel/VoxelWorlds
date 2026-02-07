// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelScatter, Log, All);

/**
 * VoxelScatter module - Vegetation and object scatter for voxel terrain.
 *
 * Provides scatter placement system for placing vegetation (trees, grass, rocks)
 * on voxel terrain surfaces. Supports both smooth and cubic terrain modes.
 *
 * Key components:
 * - FVoxelSurfaceExtractor: Extracts surface points from mesh data
 * - FVoxelScatterPlacement: Applies placement rules to generate spawn points
 * - UVoxelScatterManager: Coordinates scatter generation per-chunk
 *
 * @see Documentation/SCATTER_SYSTEM.md
 */
class FVoxelScatterModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 * Beware of calling this during the shutdown phase - module may have been unloaded.
	 */
	static inline FVoxelScatterModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelScatterModule>("VoxelScatter");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelScatter");
	}
};
