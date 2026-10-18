// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterTypes.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelSeaSurface.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 4: scatter along local up (WORLD_MODES_REVIEW_PLAN.md §18, WM-SY6; SP-G1). Real planet chunks from
// the CPU generator (a 250,000 uu planet like Review_Planet, voxel 75, chunk 32), extracted by
// UVoxelScatterManager::ExtractPlanetSurfacePoints (ExtractSurfacePointsAlongUp) at the poles, 45 degrees, the equator and a cube corner.
//
// - AlongLocalUp: the open points lie on the analytic surface along their direction; local up is radial; slope and
//   face type are measured against it; an instance not aligned to the normal stands on local up.
// - DensityPerArea: the weighted points per area of the tangent plane around each site match the column scan's one
//   per stride square, within 10%, also at the cube corner where the lines meet the ground 1.73 times as densely.
// ---------------------------------------------------------------------------

namespace VoxelScatterPlanetTestUtils
{
	constexpr float Radius = 250000.0f;
	constexpr float VoxelSize = 75.0f;
	constexpr int32 ChunkSize = 32;

	UVoxelWorldConfiguration* MakePlanetConfig()
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector(1000.0, -2000.0, 500.0);
		Config->WorldRadius = Radius;
		Config->PlanetHeightScale = 3000.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = 4000.0f;
		Config->PlanetMaxTerrainDepth = 4000.0f;
		Config->VoxelSize = VoxelSize;
		Config->ChunkSize = ChunkSize;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 248675;
		Config->NoiseParams.Octaves = 3;
		Config->NoiseParams.Frequency = 0.000025f;
		Config->NoiseParams.Lacunarity = 1.681f;
		Config->NoiseParams.Persistence = 0.5f;
		return Config;
	}

	/**
	 * The surface points of the 3x3x3 chunks around a site, extracted along local up at one point per voxel. With
	 * GenerationZones the chunks generate conditioned (spherical Phase 5); ContextZones go to the extraction context.
	 */
	TArray<FVoxelSurfacePoint> ExtractAround(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const FVector& Site,
		const TArray<FVoxelConditioningZone>& GenerationZones = {}, const TArray<FVoxelConditioningZone>& ContextZones = {})
	{
		FScatterExtractionContext Context;
		Context.WorldMode = &Mode;
		Context.NoiseParams = Config->NoiseParams;
		Context.Sea = FVoxelSeaSurface::FromConfig(*Config);
		if (ContextZones.Num() > 0)
		{
			Context.ConditioningZones = MakeShared<const TArray<FVoxelConditioningZone>>(ContextZones);
		}
		const double ChunkWorld = ChunkSize * VoxelSize;
		const FVector Local = (Site - Config->WorldOrigin) / ChunkWorld;
		const FIntVector Center(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), FMath::FloorToInt(Local.Z));
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		TArray<FVoxelSurfacePoint> Out;
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			const FIntVector Coord = Center + FIntVector(DX, DY, DZ);
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = Coord;
			Request.ChunkSize = ChunkSize;
			Request.VoxelSize = VoxelSize;
			Request.NoiseParams = Config->NoiseParams;
			FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
			Request.ConditioningZones = GenerationZones;
			TArray<FVoxelData> Voxels;
			Generator.GenerateChunkCPU(Request, Voxels);
			FChunkSurfaceData Surface;
			UVoxelScatterManager::ExtractPlanetSurfacePoints(Voxels, Coord, Config->WorldOrigin + FVector(Coord) * ChunkWorld,
				ChunkSize, VoxelSize, VoxelSize, Context, Surface);
			Out.Append(Surface.SurfacePoints);
		}
		Generator.Shutdown();
		return Out;
	}

	TArray<TPair<const TCHAR*, FVector>> Sites()
	{
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		return {
			{ TEXT("north pole"), FVector(0, 0, 1) },
			{ TEXT("south pole"), FVector(0, 0, -1) },
			{ TEXT("45 degrees"), FVector(C45, 0, C45) },
			{ TEXT("equator"), FVector(0, 1, 0) },
			{ TEXT("cube corner"), FVector(1, 1, 1).GetSafeNormal() },
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPlanetAlongLocalUpTest,
	"VoxelWorlds.Scatter.Planet.AlongLocalUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPlanetAlongLocalUpTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterPlanetTestUtils;
	UVoxelWorldConfiguration* Config = MakePlanetConfig();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVector Center = Config->WorldOrigin;

	for (const TPair<const TCHAR*, FVector>& Site : Sites())
	{
		const FVoxelTerrainSurfacePoint SiteSurface = Mode->GetSurfacePoint(Center + Site.Value, Config->NoiseParams);
		const TArray<FVoxelSurfacePoint> Points = ExtractAround(Config, *Mode, SiteSurface.Position);
		int32 Open = 0, Underground = 0, OffSurface = 0, BadUp = 0, BadSlope = 0, BadFace = 0, BadInstance = 0;
		double WorstOff = 0.0;
		for (const FVoxelSurfacePoint& Point : Points)
		{
			if (Point.bIsUnderground)
			{
				++Underground;
				continue;
			}
			++Open;
			const FVector Direction = (Point.Position - Center).GetSafeNormal();
			const FVoxelTerrainSurfacePoint Analytic = Mode->GetSurfacePoint(Point.Position, Config->NoiseParams);
			const double Off = FMath::Abs(Point.Altitude - Analytic.Altitude);
			WorstOff = FMath::Max(WorstOff, Off);
			OffSurface += Off > VoxelSize ? 1 : 0;
			BadUp += (Point.LocalUp - Direction).Size() > 1.0e-6 ? 1 : 0;
			const float Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(static_cast<float>(FVector::DotProduct(Point.Normal, Direction)), -1.0f, 1.0f)));
			BadSlope += FMath::Abs(Slope - Point.SlopeAngle) > 0.01f ? 1 : 0;
			const double Along = FVector::DotProduct(Point.Normal, Direction);
			const EVoxelFaceType Expected = Along > 0.5 ? EVoxelFaceType::Top : (Along < -0.5 ? EVoxelFaceType::Bottom : EVoxelFaceType::Side);
			BadFace += Point.FaceType != Expected ? 1 : 0;

			FScatterSpawnPoint Spawn;
			Spawn.Position = Point.Position;
			Spawn.Normal = Point.Normal;
			Spawn.LocalUp = Point.LocalUp;
			Spawn.RotationYaw = 137.0f;
			const FVector InstanceUp = Spawn.GetTransform(false).GetRotation().GetUpVector();
			BadInstance += FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(InstanceUp, Direction), -1.0, 1.0))) > 0.01 ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("%s: %d open points, %d underground; worst %.1f uu off the analytic surface (%d over a voxel)"),
			Site.Key, Open, Underground, WorstOff, OffSurface));
		TestTrue(*FString::Printf(TEXT("%s: the surface has points (non-vacuous)"), Site.Key), Open >= 2000);
		TestEqual(*FString::Printf(TEXT("%s: no cave or overhang, so nothing underground"), Site.Key), Underground, 0);
		TestEqual(*FString::Printf(TEXT("%s: every open point within a voxel of the analytic surface"), Site.Key), OffSurface, 0);
		TestEqual(*FString::Printf(TEXT("%s: local up is radial"), Site.Key), BadUp, 0);
		TestEqual(*FString::Printf(TEXT("%s: the slope is against local up"), Site.Key), BadSlope, 0);
		TestEqual(*FString::Printf(TEXT("%s: the face type is by local up"), Site.Key), BadFace, 0);
		TestEqual(*FString::Printf(TEXT("%s: instances stand on local up (within 0.01 degrees)"), Site.Key), BadInstance, 0);
	}
	Config->RemoveFromRoot();
	return true;
}

// Spherical Phase 5 (WM-PL4; SP-B10): on a planet chunk generated with a raised pad, the lines that leave a chunk through
// its upper face put their crossing on the generated (conditioned) surface: with the chunk's zones in the context every open
// point lies within a voxel of it; without them the analytic test reads the natural ground and loses or misplaces points.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPlanetConditionedLineTopTest,
	"VoxelWorlds.Scatter.Planet.ConditionedLineTop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPlanetConditionedLineTopTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterPlanetTestUtils;
	UVoxelWorldConfiguration* Config = MakePlanetConfig();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
	const FVector Center = Config->WorldOrigin;
	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const FVector Site(C45, 0.0, C45);
	const float Target = Planet.GetTerrainAltitude(Site, Config->NoiseParams) + 300.0f;
	const TArray<FVoxelConditioningZone> Zones = { FVoxelConditioningZone::MakeRadial(Site, 1500.0f, 1000.0f, Target) };
	const FVector PadPoint = Center + Site * (Radius + Target);

	auto Judge = [&](const TArray<FVoxelSurfacePoint>& Points, int32& OutOpen, int32& OutOff)
	{
		OutOpen = 0;
		OutOff = 0;
		for (const FVoxelSurfacePoint& Point : Points)
		{
			if (Point.bIsUnderground)
			{
				continue;
			}
			++OutOpen;
			const FVoxelTerrainSurfacePoint Generated = Planet.GetConditionedSurfacePoint(Point.Position, Config->NoiseParams, Zones);
			OutOff += FMath::Abs(Point.Altitude - Generated.Altitude) > VoxelSize ? 1 : 0;
		}
	};
	int32 Open = 0, Off = 0, OpenWithout = 0, OffWithout = 0;
	Judge(ExtractAround(Config, *Mode, PadPoint, Zones, Zones), Open, Off);
	Judge(ExtractAround(Config, *Mode, PadPoint, Zones, {}), OpenWithout, OffWithout);
	AddInfo(FString::Printf(TEXT("With the zones: %d open points, %d off the generated surface; without: %d open, %d off"), Open, Off, OpenWithout, OffWithout));
	TestTrue(TEXT("The pad has points (non-vacuous)"), Open >= 2000);
	TestEqual(TEXT("Every open point within a voxel of the generated surface"), Off, 0);
	TestTrue(TEXT("Without the zones the line tops read the natural ground (the test is sensitive)"), OpenWithout < Open || OffWithout > 0);
	Config->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPlanetDensityTest,
	"VoxelWorlds.Scatter.Planet.DensityPerArea",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPlanetDensityTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterPlanetTestUtils;
	UVoxelWorldConfiguration* Config = MakePlanetConfig();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVector Center = Config->WorldOrigin;

	// The column scan puts one point per voxel column, i.e. one per VoxelSize^2 of horizontal area. On a planet the
	// weighted points whose projection onto the site's tangent plane falls in a disc of radius R should number
	// pi R^2 / VoxelSize^2.
	constexpr double DiscRadius = 1500.0;
	const double Expected = UE_DOUBLE_PI * DiscRadius * DiscRadius / (VoxelSize * VoxelSize);
	for (const TPair<const TCHAR*, FVector>& Site : Sites())
	{
		const FVoxelTerrainSurfacePoint SiteSurface = Mode->GetSurfacePoint(Center + Site.Value, Config->NoiseParams);
		const TArray<FVoxelSurfacePoint> Points = ExtractAround(Config, *Mode, SiteSurface.Position);
		double Weighted = 0.0;
		int32 Raw = 0;
		for (const FVoxelSurfacePoint& Point : Points)
		{
			const FVector Offset = Point.Position - SiteSurface.Position;
			const FVector Across = Offset - Site.Value * FVector::DotProduct(Offset, Site.Value);
			if (!Point.bIsUnderground && Across.Size() <= DiscRadius)
			{
				Weighted += Point.SpawnWeight;
				++Raw;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: weighted %.0f points in the disc (raw %d) against %.0f expected (%.1f%%)"),
			Site.Key, Weighted, Raw, Expected, 100.0 * Weighted / Expected));
		TestTrue(*FString::Printf(TEXT("%s: density per area within 10%% of the column scan's"), Site.Key), FMath::Abs(Weighted / Expected - 1.0) <= 0.10);
	}
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
