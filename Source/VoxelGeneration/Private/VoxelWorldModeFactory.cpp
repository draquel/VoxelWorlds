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
	Params.SeabedDepth = Config.IslandSeabedDepth;
	Params.SeabedSlopeWidth = Config.IslandSeabedSlopeWidth;
	return Params;
}

FSphericalPlanetParams FVoxelWorldModeFactory::MakePlanetParams(const UVoxelWorldConfiguration& Config)
{
	FSphericalPlanetParams Params;
	Params.PlanetRadius = Config.WorldRadius;
	Params.MaxTerrainHeight = Config.PlanetMaxTerrainHeight;
	Params.MaxTerrainDepth = Config.PlanetMaxTerrainDepth;
	Params.PlanetCenter = Config.WorldOrigin;
	Params.OrbitCeiling = Config.PlanetOrbitCeiling;
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

namespace VoxelWorldModeFactoryPrivate
{
	FString Num(double Value)
	{
		return FString::SanitizeFloat(Value);
	}

	FString Vec2(double X, double Y)
	{
		return FString::Printf(TEXT("(%s,%s)"), *Num(X), *Num(Y));
	}

	FString Vec3(const FVector& V)
	{
		return FString::Printf(TEXT("(%s,%s,%s)"), *Num(V.X), *Num(V.Y), *Num(V.Z));
	}

	const TCHAR* FalloffName(EIslandFalloffType Type)
	{
		switch (Type)
		{
		case EIslandFalloffType::Linear: return TEXT("Linear");
		case EIslandFalloffType::Smooth: return TEXT("Smooth");
		case EIslandFalloffType::Squared: return TEXT("Squared");
		case EIslandFalloffType::Exponential: return TEXT("Exponential");
		default: return TEXT("Unknown");
		}
	}
}

FVoxelWorldFingerprint FVoxelWorldModeFactory::MakeFingerprint(const UVoxelWorldConfiguration& Config)
{
	using namespace VoxelWorldModeFactoryPrivate;

	TArray<FString> Params;
	if (Config.WorldMode == EWorldMode::IslandBowl)
	{
		const FIslandBowlParams Island = MakeIslandParams(Config);
		const bool bRectangle = Island.Shape == EIslandShape::Rectangle;
		Params.Add(FString::Printf(TEXT("IslandShape=%s"), bRectangle ? TEXT("Rectangle") : TEXT("Circular")));
		Params.Add(TEXT("IslandRadius=") + Num(Island.IslandRadius));
		if (bRectangle)
		{
			Params.Add(TEXT("IslandSizeY=") + Num(Island.SizeY)); // unread by a circular island
		}
		Params.Add(TEXT("IslandFalloffWidth=") + Num(Island.FalloffWidth));
		Params.Add(FString::Printf(TEXT("IslandFalloff=%s"), FalloffName(Island.FalloffType)));
		Params.Add(TEXT("IslandCenter=") + Vec2(Island.CenterX, Island.CenterY));
		Params.Add(TEXT("IslandEdgeHeight=") + Num(Island.EdgeHeight));
		// The seabed (SeabedDepth, SeabedSlopeWidth; WM-E1) is left out on purpose: it shapes only the terrain past
		// R + F, where a save holds nothing (POIs, map tiles and edits stay inside the walls), so a seabed change
		// must not refuse a save. GenerationVersion did not move for it either (WORLD_MODES_REVIEW_PLAN.md §14.10).
	}
	else if (Config.WorldMode == EWorldMode::SphericalPlanet)
	{
		const FSphericalPlanetParams Planet = MakePlanetParams(Config);
		Params.Add(TEXT("PlanetRadius=") + Num(Planet.PlanetRadius));
		Params.Add(TEXT("PlanetMaxTerrainHeight=") + Num(Planet.MaxTerrainHeight));
		Params.Add(TEXT("PlanetMaxTerrainDepth=") + Num(Planet.MaxTerrainDepth));
		Params.Add(TEXT("PlanetCenter=") + Vec3(Planet.PlanetCenter));
		Params.Add(FString::Printf(TEXT("PlanetGeneration=%d"), PlanetGenerationVersion));
	}
	Params.Add(TEXT("VoxelSize=") + Num(Config.VoxelSize));
	Params.Add(FString::Printf(TEXT("ChunkSize=%d"), Config.ChunkSize));
	Params.Add(TEXT("WorldOrigin=") + Vec3(Config.WorldOrigin));
	// The meshing mode (a cubic and a smooth configuration of one map and seed shared a save key) and the noise seed in use
	// (WorldSeed when the noise seed is 0; code review CR2, D-CR19 / D-CR20).
	Params.Add(TEXT("MeshingMode=") + StaticEnum<EMeshingMode>()->GetNameStringByValue(static_cast<int64>(Config.MeshingMode)));
	Params.Add(FString::Printf(TEXT("NoiseSeed=%d"), Config.GetNoiseParams().Seed));

	FVoxelWorldFingerprint Fingerprint;
	Fingerprint.WorldMode = Config.WorldMode;
	Fingerprint.Shape = FString::Join(Params, TEXT(";"));
	Fingerprint.GenerationVersion = GenerationVersion;
	return Fingerprint;
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
