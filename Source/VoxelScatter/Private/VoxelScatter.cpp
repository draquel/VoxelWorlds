// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatter.h"

#define LOCTEXT_NAMESPACE "FVoxelScatterModule"

DEFINE_LOG_CATEGORY(LogVoxelScatter);

void FVoxelScatterModule::StartupModule()
{
	UE_LOG(LogVoxelScatter, Log, TEXT("VoxelScatter module started"));
}

void FVoxelScatterModule::ShutdownModule()
{
	UE_LOG(LogVoxelScatter, Log, TEXT("VoxelScatter module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelScatterModule, VoxelScatter)
