// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelMeshing, Log, All);

/**
 * VoxelMeshing module - GPU and CPU mesh generation for VoxelWorlds.
 *
 * Provides compute shader-based cubic meshing with face culling
 * and CPU fallback for testing and editor scenarios.
 *
 * Dependencies: VoxelCore, RenderCore, RHI
 *
 * @see IVoxelMesher
 * @see Documentation/ARCHITECTURE.md
 */
class FVoxelMeshingModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 */
	static inline FVoxelMeshingModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelMeshingModule>("VoxelMeshing");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelMeshing");
	}
};
