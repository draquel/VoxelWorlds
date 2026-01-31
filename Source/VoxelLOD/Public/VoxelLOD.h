// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelLOD, Log, All);

/**
 * VoxelLOD module - Level of Detail system for VoxelWorlds.
 *
 * Provides pluggable LOD strategies for determining chunk visibility
 * and detail levels based on viewer position.
 *
 * Dependencies: VoxelCore
 *
 * @see Documentation/LOD_SYSTEM.md
 */
class FVoxelLODModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 */
	static inline FVoxelLODModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelLODModule>("VoxelLOD");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelLOD");
	}
};
