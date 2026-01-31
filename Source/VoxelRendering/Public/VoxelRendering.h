// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelRendering, Log, All);

/**
 * VoxelRendering module - Rendering system for VoxelWorlds.
 *
 * Provides pluggable mesh rendering through the IVoxelMeshRenderer interface.
 * Supports two rendering backends:
 * - Custom Vertex Factory (GPU-driven, runtime)
 * - ProceduralMeshComponent (Editor/tools fallback)
 *
 * Dependencies: VoxelCore
 *
 * @see IVoxelMeshRenderer
 * @see Documentation/RENDERING_SYSTEM.md
 */
class FVoxelRenderingModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 */
	static inline FVoxelRenderingModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelRenderingModule>("VoxelRendering");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelRendering");
	}
};
