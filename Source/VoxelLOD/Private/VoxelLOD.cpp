// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelLOD.h"

#define LOCTEXT_NAMESPACE "FVoxelLODModule"

DEFINE_LOG_CATEGORY(LogVoxelLOD);

void FVoxelLODModule::StartupModule()
{
	UE_LOG(LogVoxelLOD, Log, TEXT("VoxelLOD module started"));
}

void FVoxelLODModule::ShutdownModule()
{
	UE_LOG(LogVoxelLOD, Log, TEXT("VoxelLOD module shutdown"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVoxelLODModule, VoxelLOD)
