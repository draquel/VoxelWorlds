// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelGenerationShaders.h"

IMPLEMENT_GLOBAL_SHADER(FGenerateVoxelDensityCS, "/Plugin/VoxelWorlds/Private/GenerateVoxelDensity.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelWaterColumnSeedCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "WaterColumnSeedCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelWaterFloodSweepCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "WaterFloodSweepCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelUndergroundColumnCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "UndergroundColumnCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelUndergroundMarkCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "UndergroundMarkCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelUndergroundApplyCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "UndergroundApplyCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelWaterFloodSweepRadialCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "WaterFloodSweepRadialCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelWaterLineSeedCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "WaterLineSeedCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelUndergroundLinesCS, "/Plugin/VoxelWorlds/Private/VoxelPostPasses.usf", "UndergroundLinesCS", SF_Compute);
