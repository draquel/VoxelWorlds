// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRendering.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FVoxelRenderingModule"

DEFINE_LOG_CATEGORY(LogVoxelRendering);

void FVoxelRenderingModule::StartupModule()
{
	// Register shader directory for custom vertex factory
	FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("VoxelWorlds"))->GetBaseDir(),
		TEXT("Shaders")
	);

	AddShaderSourceDirectoryMapping(TEXT("/Plugin/VoxelWorlds"), PluginShaderDir);

	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelRendering module started - Shader directory: %s"), *PluginShaderDir);
}

void FVoxelRenderingModule::ShutdownModule()
{
	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelRendering module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelRenderingModule, VoxelRendering)
