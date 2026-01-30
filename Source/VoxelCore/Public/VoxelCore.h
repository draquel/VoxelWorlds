// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelCore, Log, All);

/**
 * VoxelCore module - Foundation module for the VoxelWorlds plugin.
 *
 * Contains core data structures, enums, and utilities shared across all voxel modules.
 * This module has no dependencies on other VoxelWorlds modules.
 *
 * @see Documentation/ARCHITECTURE.md
 */
class FVoxelCoreModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 * Beware of calling this during the shutdown phase - module may have been unloaded.
	 */
	static inline FVoxelCoreModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelCoreModule>("VoxelCore");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelCore");
	}
};
