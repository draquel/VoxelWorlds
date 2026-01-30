// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCore.h"

#define LOCTEXT_NAMESPACE "FVoxelCoreModule"

DEFINE_LOG_CATEGORY(LogVoxelCore);

void FVoxelCoreModule::StartupModule()
{
	UE_LOG(LogVoxelCore, Log, TEXT("VoxelCore module started"));
}

void FVoxelCoreModule::ShutdownModule()
{
	UE_LOG(LogVoxelCore, Log, TEXT("VoxelCore module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelCoreModule, VoxelCore)
