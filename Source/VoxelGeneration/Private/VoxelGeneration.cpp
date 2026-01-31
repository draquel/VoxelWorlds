// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGeneration.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FVoxelGenerationModule"

DEFINE_LOG_CATEGORY(LogVoxelGeneration);

void FVoxelGenerationModule::StartupModule()
{
	// Register the shader directory for the VoxelWorlds plugin
	FString ShaderDir = GetShaderDirectory();
	if (FPaths::DirectoryExists(ShaderDir))
	{
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/VoxelWorlds"), ShaderDir);
		UE_LOG(LogVoxelGeneration, Log, TEXT("VoxelGeneration module started - Shader directory: %s"), *ShaderDir);
	}
	else
	{
		UE_LOG(LogVoxelGeneration, Warning, TEXT("VoxelGeneration shader directory not found: %s"), *ShaderDir);
	}
}

void FVoxelGenerationModule::ShutdownModule()
{
	UE_LOG(LogVoxelGeneration, Log, TEXT("VoxelGeneration module shutdown"));
}

FString FVoxelGenerationModule::GetShaderDirectory()
{
	// Get the plugin base directory
	FString PluginDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("VoxelWorlds"));
	if (!FPaths::DirectoryExists(PluginDir))
	{
		// Try engine plugins directory as fallback
		PluginDir = FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("VoxelWorlds"));
	}

	return FPaths::Combine(PluginDir, TEXT("Shaders"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelGenerationModule, VoxelGeneration)
