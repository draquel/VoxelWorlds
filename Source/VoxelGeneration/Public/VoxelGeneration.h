// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelGeneration, Log, All);

/**
 * VoxelGeneration module - GPU and CPU noise generation for VoxelWorlds.
 *
 * Provides compute shader-based terrain generation using Perlin and Simplex noise
 * with CPU fallback for testing and editor scenarios.
 *
 * Dependencies: VoxelCore, RenderCore, RHI
 *
 * @see IVoxelNoiseGenerator
 * @see Documentation/ARCHITECTURE.md
 */
class FVoxelGenerationModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 */
	static inline FVoxelGenerationModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelGenerationModule>("VoxelGeneration");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelGeneration");
	}
};
