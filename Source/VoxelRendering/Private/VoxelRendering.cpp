// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRendering.h"

#define LOCTEXT_NAMESPACE "FVoxelRenderingModule"

DEFINE_LOG_CATEGORY(LogVoxelRendering);

void FVoxelRenderingModule::StartupModule()
{
	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelRendering module started"));
}

void FVoxelRenderingModule::ShutdownModule()
{
	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelRendering module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelRenderingModule, VoxelRendering)
