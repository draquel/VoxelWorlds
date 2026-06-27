// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelPCG.h"

#define LOCTEXT_NAMESPACE "FVoxelPCGModule"

DEFINE_LOG_CATEGORY(LogVoxelPCG);

void FVoxelPCGModule::StartupModule()
{
	UE_LOG(LogVoxelPCG, Log, TEXT("VoxelPCG module started"));
}

void FVoxelPCGModule::ShutdownModule()
{
	UE_LOG(LogVoxelPCG, Log, TEXT("VoxelPCG module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelPCGModule, VoxelPCG)
