// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelStreaming.h"

#define LOCTEXT_NAMESPACE "FVoxelStreamingModule"

DEFINE_LOG_CATEGORY(LogVoxelStreaming);

void FVoxelStreamingModule::StartupModule()
{
	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelStreaming module started"));
}

void FVoxelStreamingModule::ShutdownModule()
{
	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelStreaming module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelStreamingModule, VoxelStreaming)
