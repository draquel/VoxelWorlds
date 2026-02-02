// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGeneration.h"

#define LOCTEXT_NAMESPACE "FVoxelGenerationModule"

DEFINE_LOG_CATEGORY(LogVoxelGeneration);

void FVoxelGenerationModule::StartupModule()
{
	// Shader directory registration is handled by VoxelRendering module
	UE_LOG(LogVoxelGeneration, Log, TEXT("VoxelGeneration module started"));
}

void FVoxelGenerationModule::ShutdownModule()
{
	UE_LOG(LogVoxelGeneration, Log, TEXT("VoxelGeneration module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelGenerationModule, VoxelGeneration)
