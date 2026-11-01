// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelRendering, Log, All);

/**
 * VoxelRendering module - Rendering system for VoxelWorlds.
 *
 * Provides mesh rendering through the IVoxelMeshRenderer interface: FVoxelMeshRenderer, one
 * UVoxelRegionComponent per render region, its FVoxelSceneProxy drawing the chunk, seam and water
 * meshes through UE's FLocalVertexFactory. (The ProceduralMeshComponent fallback renderer was
 * deleted in code review CR1, D-CR7.)
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
