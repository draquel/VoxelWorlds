// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelMap, Log, All);

/**
 * VoxelMap module - Map data generation for voxel terrain.
 *
 * Provides a world subsystem that generates 2D tile map data from
 * terrain height and material queries. Tiles are generated asynchronously
 * and cached for consumption by UI widgets (minimap, world map).
 *
 * Key components:
 * - FVoxelMapTile: Serializable tile data (pixel colors for a chunk's XY footprint)
 * - UVoxelMapSubsystem: World subsystem managing tile cache, exploration tracking,
 *   and async tile generation
 *
 * The module is world-mode-agnostic — it queries IVoxelWorldMode interface
 * for terrain height and material data, working with any world mode.
 */
class FVoxelMapModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 * Beware of calling this during the shutdown phase - module may have been unloaded.
	 */
	static inline FVoxelMapModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelMapModule>("VoxelMap");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelMap");
	}
};
