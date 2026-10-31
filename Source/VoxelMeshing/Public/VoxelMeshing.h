// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelMeshing, Log, All);

/**
 * VoxelMeshing module - CPU mesh generation for VoxelWorlds.
 *
 * The cubic, Marching Cubes and Dual Contouring meshers behind IVoxelMesher: interior chunk meshes
 * and the seam jobs between chunks, run on the chunk manager's worker tasks. Nothing in the module
 * touches the GPU (the GPU meshers were deleted in code review CR1).
 *
 * Dependencies: VoxelCore
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
