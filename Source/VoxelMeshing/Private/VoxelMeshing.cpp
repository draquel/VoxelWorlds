// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMeshing.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FVoxelMeshingModule"

DEFINE_LOG_CATEGORY(LogVoxelMeshing);

void FVoxelMeshingModule::StartupModule()
{
	// Note: Shader directory mapping is handled by VoxelGeneration module
	// Both modules share the same /Plugin/VoxelWorlds shader path
	UE_LOG(LogVoxelMeshing, Log, TEXT("VoxelMeshing module started"));
}

void FVoxelMeshingModule::ShutdownModule()
{
	UE_LOG(LogVoxelMeshing, Log, TEXT("VoxelMeshing module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelMeshingModule, VoxelMeshing)
