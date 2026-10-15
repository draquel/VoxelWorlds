// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldModeFactory.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelNoiseTypes.h"
#include "VoxelWorldConfiguration.h"

FWorldModeTerrainParams FVoxelWorldModeFactory::MakeTerrainParams(const UVoxelWorldConfiguration& Config)
{
	return FWorldModeTerrainParams(Config.SeaLevel, Config.HeightScale, Config.BaseHeight);
}

FWorldModeTerrainParams FVoxelWorldModeFactory::MakePlanetTerrainParams(const UVoxelWorldConfiguration& Config)
{
	return FWorldModeTerrainParams(0.0f, Config.PlanetHeightScale, Config.BaseHeight);
}

FIslandBowlParams FVoxelWorldModeFactory::MakeIslandParams(const UVoxelWorldConfiguration& Config)
{
	FIslandBowlParams Params;
	Params.Shape = static_cast<EIslandShape>(Config.IslandShape);
	Params.IslandRadius = Config.IslandRadius;
	Params.SizeY = Config.IslandSizeY;
	Params.FalloffWidth = Config.IslandFalloffWidth;
	Params.FalloffType = static_cast<EIslandFalloffType>(Config.IslandFalloffType);
	// The config's centre is an offset from WorldOrigin. Resolve it once here, so generation (which tests absolute
	// voxel positions) and the LOD cull put the island in the same place (KI3).
	Params.CenterX = Config.WorldOrigin.X + Config.IslandCenterX;
	Params.CenterY = Config.WorldOrigin.Y + Config.IslandCenterY;
	Params.EdgeHeight = Config.IslandEdgeHeight;
	return Params;
}

FSphericalPlanetParams FVoxelWorldModeFactory::MakePlanetParams(const UVoxelWorldConfiguration& Config)
{
	FSphericalPlanetParams Params;
	Params.PlanetRadius = Config.WorldRadius;
	Params.MaxTerrainHeight = Config.PlanetMaxTerrainHeight;
	Params.MaxTerrainDepth = Config.PlanetMaxTerrainDepth;
	Params.PlanetCenter = Config.WorldOrigin;
	return Params;
}

TSharedRef<IVoxelWorldMode> FVoxelWorldModeFactory::Create(const UVoxelWorldConfiguration& Config)
{
	TSharedPtr<IVoxelWorldMode> Mode;
	switch (Config.WorldMode)
	{
	case EWorldMode::IslandBowl:
		Mode = MakeShared<FIslandBowlWorldMode>(MakeTerrainParams(Config), MakeIslandParams(Config));
		break;
	case EWorldMode::SphericalPlanet:
		Mode = MakeShared<FSphericalPlanetWorldMode>(MakePlanetTerrainParams(Config), MakePlanetParams(Config));
		break;
	case EWorldMode::InfinitePlane:
	default:
		Mode = MakeShared<FInfinitePlaneWorldMode>(MakeTerrainParams(Config));
		break;
	}

	// Generation applies continentalness whenever the config has a biome configuration (bEnableBiomes only gates the
	// biome materials), so the analytic height takes the same context. The mode snapshots it by value: no UObject
	// reference is kept, and the instance stays safe for tasks that outlive the world.
	Mode->SetBiomeContext(Config.BiomeConfiguration);
	return Mode.ToSharedRef();
}

void FVoxelWorldModeFactory::FillGenerationRequest(const UVoxelWorldConfiguration& Config, FVoxelNoiseGenerationRequest& Request)
{
	Request.WorldMode = Config.WorldMode;
	Request.WorldOrigin = Config.WorldOrigin;

	const bool bPlanet = Config.WorldMode == EWorldMode::SphericalPlanet;
	const FWorldModeTerrainParams TerrainParams = bPlanet ? MakePlanetTerrainParams(Config) : MakeTerrainParams(Config);
	Request.SeaLevel = TerrainParams.SeaLevel;
	Request.HeightScale = TerrainParams.HeightScale;
	Request.BaseHeight = TerrainParams.BaseHeight;

	if (Config.WorldMode == EWorldMode::IslandBowl)
	{
		Request.IslandParams = MakeIslandParams(Config);
	}
	if (bPlanet)
	{
		Request.SphericalPlanetParams = MakePlanetParams(Config);
	}
}
