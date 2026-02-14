// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMap.h"

#define LOCTEXT_NAMESPACE "FVoxelMapModule"

DEFINE_LOG_CATEGORY(LogVoxelMap);

void FVoxelMapModule::StartupModule()
{
	UE_LOG(LogVoxelMap, Log, TEXT("VoxelMap module started"));
}

void FVoxelMapModule::ShutdownModule()
{
	UE_LOG(LogVoxelMap, Log, TEXT("VoxelMap module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelMapModule, VoxelMap)
