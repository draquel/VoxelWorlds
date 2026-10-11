// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelNavigation.h"

DEFINE_LOG_CATEGORY(LogVoxelNavigation);

void FVoxelNavigationModule::StartupModule()
{
	UE_LOG(LogVoxelNavigation, Log, TEXT("VoxelNavigation module started"));
}

void FVoxelNavigationModule::ShutdownModule()
{
	UE_LOG(LogVoxelNavigation, Log, TEXT("VoxelNavigation module shutdown"));
}

IMPLEMENT_MODULE(FVoxelNavigationModule, VoxelNavigation)
