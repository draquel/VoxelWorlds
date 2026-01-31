// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVoxelStreaming, Log, All);

/**
 * VoxelStreaming module - Chunk streaming and management for VoxelWorlds.
 *
 * Provides the ChunkManager which coordinates:
 * - Chunk lifecycle (load, generate, mesh, unload)
 * - LOD strategy integration
 * - Render system updates
 * - Time-sliced streaming operations
 *
 * Dependencies: VoxelCore, VoxelLOD, VoxelRendering
 *
 * @see UVoxelChunkManager
 * @see Documentation/ARCHITECTURE.md
 */
class FVoxelStreamingModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Singleton-like access to this module's interface.
	 */
	static inline FVoxelStreamingModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVoxelStreamingModule>("VoxelStreaming");
	}

	/**
	 * Checks if this module is loaded and ready.
	 */
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VoxelStreaming");
	}
};
