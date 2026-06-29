// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelPCG, Log, All);

/**
 * VoxelPCG module - PCG (Procedural Content Generation) integration for voxel terrain.
 *
 * Bridges UE's PCG framework to the runtime-generated voxel world so PCG graphs can
 * place scatter/foliage/props on the terrain via the "Generate at Runtime" path,
 * alongside the bespoke VoxelScatter system.
 *
 * The PCG engine dependency is isolated to this module. The core bridge is a custom
 * PCG node (Voxel Surface Sampler) that re-samples the procedural generator
 * (IVoxelWorldMode height + biome/material query) to emit terrain surface points as
 * PCG point data — decoupled from chunk streaming, mirroring how FVoxelTreeInjector
 * and UVoxelMapSubsystem already query the generator independently of loaded chunks.
 */
class FVoxelPCGModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 * Beware of calling this during the shutdown phase - module may have been unloaded.
	 */
	static inline FVoxelPCGModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelPCGModule>("VoxelPCG");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelPCG");
	}
};
