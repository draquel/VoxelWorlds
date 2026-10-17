// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SphericalPlanetWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelSeaSurface.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelNoiseTypes.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 4, systems: the terrain queries (WORLD_MODES_REVIEW_PLAN.md §18, WM-SY1, WM-SY2;
// SPHERICAL_PLANET_REQUIREMENTS.md SP-A3, SP-A4). Pure CPU, runs headless.
//
// - The legacy GetTerrainHeightAt on a planet returns the Z where the vertical line meets the terrain on the upper
//   hemisphere (a fallback for stray callers), not a radius.
// - GetAltitudeBounds: the height bounds as altitudes on the plane and the island, the shell on a planet.
// - The planet's surface conditions (material, biome) equal what generation writes in the surface voxel.
// ---------------------------------------------------------------------------

namespace PlanetSystemsTestUtils
{
	constexpr float Radius = 250000.0f;
	constexpr float VoxelSize = 75.0f;
	constexpr int32 ChunkSize = 32;

	/** A planet like Review_Planet (R 250,000, PlanetHeightScale 6,000, the demo's terrain noise). Rooted. */
	UVoxelWorldConfiguration* MakePlanetConfig(UVoxelBiomeConfiguration* Biomes, const FVector& Origin = FVector::ZeroVector)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = Origin;
		Config->WorldRadius = Radius;
		Config->PlanetHeightScale = 6000.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = 6500.0f;
		Config->PlanetMaxTerrainDepth = 6500.0f;
		Config->VoxelSize = VoxelSize;
		Config->ChunkSize = ChunkSize;
		Config->BiomeConfiguration = Biomes;
		Config->bEnableBiomes = Biomes != nullptr;
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

	/** Directions spread over the sphere (a Fibonacci lattice). */
	TArray<FVector> FibonacciDirections(int32 Count)
	{
		TArray<FVector> Out;
		const double Golden = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));
		for (int32 i = 0; i < Count; ++i)
		{
			const double Z = 1.0 - (i + 0.5) * 2.0 / Count;
			const double Rho = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
			Out.Add(FVector(Rho * FMath::Cos(Golden * i), Rho * FMath::Sin(Golden * i), Z));
		}
		return Out;
	}

	/** Generates planet chunks on demand through the CPU generator and reads voxels by global voxel index. */
	struct FPlanetVoxels
	{
		const UVoxelWorldConfiguration* Config = nullptr;
		FVoxelCPUNoiseGenerator Generator;
		TMap<FIntVector, TArray<FVoxelData>> Chunks;

		explicit FPlanetVoxels(const UVoxelWorldConfiguration* InConfig) : Config(InConfig) {}

		const FVoxelData& Voxel(const FIntVector& Global)
		{
			const int32 N = Config->ChunkSize;
			const FIntVector ChunkCoord(FMath::FloorToInt(Global.X / static_cast<float>(N)), FMath::FloorToInt(Global.Y / static_cast<float>(N)),
				FMath::FloorToInt(Global.Z / static_cast<float>(N)));
			TArray<FVoxelData>* Data = Chunks.Find(ChunkCoord);
			if (!Data)
			{
				FVoxelNoiseGenerationRequest Request;
				Request.ChunkCoord = ChunkCoord;
				Request.ChunkSize = N;
				Request.VoxelSize = Config->VoxelSize;
				Request.LODLevel = 0;
				Request.NoiseParams = Config->NoiseParams;
				FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
				Request.bEnableBiomes = Config->bEnableBiomes;
				Request.BiomeConfiguration = Config->BiomeConfiguration;
				Request.bEnableCaves = Config->bEnableCaves;
				Request.CaveConfiguration = Config->CaveConfiguration;
				Request.bEnableWaterLevel = Config->bEnableWaterLevel;
				Request.WaterLevel = Config->WaterLevel;
				Request.WaterRadius = Config->WaterRadius;
				Data = &Chunks.Add(ChunkCoord);
				Generator.GenerateChunkCPU(Request, *Data);
			}
			const FIntVector Local = Global - ChunkCoord * N;
			return (*Data)[Local.X + Local.Y * N + Local.Z * N * N];
		}
	};
}

/**
 * SP-A3: the legacy height query on a planet. At points spread over the northern hemisphere (out to 0.95 of the radius
 * from the axis), the returned Z is where the vertical line through (X, Y) meets the terrain: the point (X, Y, Z) lies
 * on the surface along its own direction to within 1 uu. Past the terrain's radius the line misses and the centre's Z
 * comes back.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetLegacyHeightTest,
	"VoxelWorlds.WorldModes.Planet.LegacyHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetLegacyHeightTest::RunTest(const FString& Parameters)
{
	using namespace PlanetSystemsTestUtils;

	AddExpectedMessagePlain(TEXT("GetTerrainHeightAt called on a planet"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

	UVoxelBiomeConfiguration* Biomes = NewObject<UVoxelBiomeConfiguration>();
	Biomes->AddToRoot();
	Biomes->bEnableContinentalness = true;
	Biomes->RebuildBakedCurves();
	UVoxelWorldConfiguration* Config = MakePlanetConfig(Biomes, FVector(12345.0, -6789.0, 1000.0));
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVector Center = Config->WorldOrigin;

	double WorstError = 0.0;
	int32 Samples = 0;
	for (int32 Ring = 0; Ring < 6; ++Ring)
	{
		const double Rho = Radius * 0.95 * Ring / 5.0;
		for (int32 Bearing = 0; Bearing < 4; ++Bearing)
		{
			const double Angle = UE_DOUBLE_PI * 0.5 * Bearing + 0.3 * Ring;
			const double X = Center.X + Rho * FMath::Cos(Angle);
			const double Y = Center.Y + Rho * FMath::Sin(Angle);
			const float Z = Mode->GetTerrainHeightAt(static_cast<float>(X), static_cast<float>(Y), Config->NoiseParams);
			const FVector OnLine(X, Y, Z);
			const FVoxelTerrainSurfacePoint Surface = Mode->GetSurfacePoint(OnLine, Config->NoiseParams);
			const double Error = FVector::Dist(OnLine, Surface.Position);
			WorstError = FMath::Max(WorstError, Error);
			TestTrue(*FString::Printf(TEXT("The point is on the upper hemisphere (rho %.0f)"), Rho), Z > Center.Z);
			++Samples;
		}
	}
	AddInfo(FString::Printf(TEXT("%d points on the northern hemisphere: worst distance from the surface %.4f uu"), Samples, WorstError));
	TestTrue(TEXT("Every (X, Y, GetTerrainHeightAt) lies on the terrain within 1 uu"), WorstError <= 1.0);

	const float Missed = Mode->GetTerrainHeightAt(static_cast<float>(Center.X + Radius * 1.2), static_cast<float>(Center.Y), Config->NoiseParams);
	TestEqual(TEXT("A line past the terrain returns the centre's Z"), Missed, static_cast<float>(Center.Z));

	Config->RemoveFromRoot();
	Biomes->RemoveFromRoot();
	return true;
}

/**
 * SP-A4: GetAltitudeBounds. On the plane and the island the height bounds become altitudes (Z - SeaLevel); on a planet
 * they are the shell, and every surface the planet generates lies inside them. The planet's GetTerrainHeightBounds is
 * the shell's world-Z extent, as its GetWorldBounds reports.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldModeAltitudeBoundsTest,
	"VoxelWorlds.WorldModes.Api.AltitudeBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldModeAltitudeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace PlanetSystemsTestUtils;

	// The plane, with a SeaLevel off zero so altitude and Z differ.
	{
		FWorldModeTerrainParams Terrain(250.0f, 4000.0f, 100.0f);
		const FInfinitePlaneWorldMode Plane(Terrain);
		float MinZ = 0.0f, MaxZ = 0.0f, MinAlt = 0.0f, MaxAlt = 0.0f;
		Plane.GetTerrainHeightBounds(MinZ, MaxZ);
		Plane.GetAltitudeBounds(MinAlt, MaxAlt);
		TestEqual(TEXT("Plane: lowest altitude = lowest height - SeaLevel"), MinAlt, MinZ - 250.0f);
		TestEqual(TEXT("Plane: highest altitude = highest height - SeaLevel"), MaxAlt, MaxZ - 250.0f);
	}

	// The island.
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::IslandBowl;
		Config->SeaLevel = -300.0f;
		const TSharedRef<IVoxelWorldMode> Island = FVoxelWorldModeFactory::Create(*Config);
		float MinZ = 0.0f, MaxZ = 0.0f, MinAlt = 0.0f, MaxAlt = 0.0f;
		Island->GetTerrainHeightBounds(MinZ, MaxZ);
		Island->GetAltitudeBounds(MinAlt, MaxAlt);
		TestEqual(TEXT("Island: lowest altitude = lowest height - SeaLevel"), MinAlt, Island->GetAltitude(FVector(0.0, 0.0, MinZ)));
		TestEqual(TEXT("Island: highest altitude = highest height - SeaLevel"), MaxAlt, Island->GetAltitude(FVector(0.0, 0.0, MaxZ)));
		TestEqual(TEXT("Island: the altitude is Z - SeaLevel"), Island->GetAltitude(FVector(0.0, 0.0, 1000.0)), 1300.0f);
		Config->RemoveFromRoot();
	}

	// The planet.
	{
		UVoxelBiomeConfiguration* Biomes = NewObject<UVoxelBiomeConfiguration>();
		Biomes->AddToRoot();
		Biomes->bEnableContinentalness = true;
		Biomes->RebuildBakedCurves();
		UVoxelWorldConfiguration* Config = MakePlanetConfig(Biomes, FVector(0.0, 0.0, 5000.0));
		Config->bEnableBiomes = false;
		const TSharedRef<IVoxelWorldMode> Planet = FVoxelWorldModeFactory::Create(*Config);
		float MinAlt = 0.0f, MaxAlt = 0.0f, MinZ = 0.0f, MaxZ = 0.0f;
		Planet->GetAltitudeBounds(MinAlt, MaxAlt);
		Planet->GetTerrainHeightBounds(MinZ, MaxZ);
		TestEqual(TEXT("Planet: lowest altitude = -MaxTerrainDepth"), MinAlt, -6500.0f);
		TestEqual(TEXT("Planet: highest altitude = MaxTerrainHeight"), MaxAlt, 6500.0f);
		const FVoxelWorldBounds Bounds = Planet->GetWorldBounds();
		TestEqual(TEXT("Planet: height bounds = the shell's world-Z extent (low)"), MinZ, Bounds.MinZ);
		TestEqual(TEXT("Planet: height bounds = the shell's world-Z extent (high)"), MaxZ, Bounds.MaxZ);

		float Lowest = TNumericLimits<float>::Max(), Highest = TNumericLimits<float>::Lowest();
		for (const FVector& Direction : FibonacciDirections(500))
		{
			const FVoxelTerrainSurfacePoint Surface = Planet->GetSurfacePoint(Config->WorldOrigin + Direction, Config->NoiseParams);
			Lowest = FMath::Min(Lowest, Surface.Altitude);
			Highest = FMath::Max(Highest, Surface.Altitude);
		}
		AddInfo(FString::Printf(TEXT("500 surface points: altitude %.0f to %.0f inside [%.0f, %.0f]"), Lowest, Highest, MinAlt, MaxAlt));
		TestTrue(TEXT("Every surface point is inside the altitude bounds"), Lowest >= MinAlt && Highest <= MaxAlt);
		Config->RemoveFromRoot();
		Biomes->RemoveFromRoot();
	}
	return true;
}

/**
 * SP-A3: material and biome on a planet (FVoxelSurfaceQuery::QueryPlanetSurfaceConditions) equal what
 * GenerateChunkSphericalPlanet writes: surface voxels (0.05 to 0.95 voxels deep) around 300 surface points spread over
 * the sphere, with the default biomes and height rules and a sea 600 uu over the datum (so some surfaces take the
 * underwater variant). The query at the voxel's position reads the generator's depth and altitude, so the dithered
 * biome blend and the height rules pick the same material.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetSurfaceConditionsTest,
	"VoxelWorlds.Generation.Planet.SurfaceConditions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetSurfaceConditionsTest::RunTest(const FString& Parameters)
{
	using namespace PlanetSystemsTestUtils;

	UVoxelBiomeConfiguration* Biomes = NewObject<UVoxelBiomeConfiguration>(); // default biomes and height rules
	Biomes->AddToRoot();
	Biomes->bEnableContinentalness = true;
	Biomes->RebuildBakedCurves();
	UVoxelWorldConfiguration* Config = MakePlanetConfig(Biomes);
	Config->bEnableBiomes = true;
	Config->bEnableWaterLevel = true;
	Config->WaterRadius = Radius + 600.0f;
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
	const FWorldModeTerrainParams Terrain = FVoxelWorldModeFactory::MakePlanetTerrainParams(*Config);
	const FVoxelBiomeSnapshot Snapshot = FVoxelBiomeSnapshot::FromConfig(Biomes);
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*Config);
	const FVector Center = Config->WorldOrigin;
	FPlanetVoxels Voxels(Config);

	int32 Compared = 0, MaterialMatches = 0, BiomeMatches = 0, Underwater = 0;
	for (const FVector& Direction : FibonacciDirections(300))
	{
		const FVoxelTerrainSurfacePoint Surface = Mode->GetSurfacePoint(Center + Direction, Config->NoiseParams);
		const FIntVector Base((Surface.Position - Center) / VoxelSize);
		for (int32 dz = -1; dz <= 1; ++dz)
		for (int32 dy = -1; dy <= 1; ++dy)
		for (int32 dx = -1; dx <= 1; ++dx)
		{
			const FIntVector Index = Base + FIntVector(dx, dy, dz);
			const FVector Position = Center + FVector(Index) * VoxelSize;
			const FVector VoxelDir = (Position - Center).GetSafeNormal();
			const double TerrainRadius = Radius + FSphericalPlanetWorldMode::SampleRadialDisplacement(
				VoxelDir, Radius, Terrain, Config->NoiseParams, &Snapshot);
			const double Depth = (TerrainRadius - FVector::Dist(Position, Center)) / VoxelSize;
			if (Depth < 0.05 || Depth > 0.95)
			{
				continue;
			}
			const FVoxelData& Generated = Voxels.Voxel(Index);
			uint8 Material = 0, Biome = 0;
			FVoxelSurfaceQuery::QueryPlanetSurfaceConditions(Planet, Position, VoxelSize, Config->NoiseParams, Snapshot, Sea,
				Material, Biome);
			++Compared;
			MaterialMatches += Material == Generated.MaterialID ? 1 : 0;
			BiomeMatches += Biome == Generated.BiomeID ? 1 : 0;
			Underwater += TerrainRadius < Config->WaterRadius ? 1 : 0;
			if (Material != Generated.MaterialID && Compared - MaterialMatches <= 3)
			{
				AddInfo(FString::Printf(TEXT("Mismatch at depth %.2f, altitude %.0f: query %d, generated %d"),
					Depth, TerrainRadius - Radius, Material, Generated.MaterialID));
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d surface voxels (%d under the sea): material equal %d, biome equal %d (%d chunks)"),
		Compared, Underwater, MaterialMatches, BiomeMatches, Voxels.Chunks.Num()));
	TestTrue(TEXT("Enough surface voxels"), Compared >= 300);
	TestTrue(TEXT("Some surfaces lie under the sea"), Underwater >= 20);
	TestEqual(TEXT("Every surface voxel's biome equals the query's"), BiomeMatches, Compared);
	TestEqual(TEXT("Every surface voxel's material equals the query's"), MaterialMatches, Compared);

	// SampleSurfacePoint on a planet: on the surface along the direction, slope against local up.
	const FVector Probe = (FVector(0.3, -0.5, 0.8)).GetSafeNormal();
	const FVoxelSurfaceSample Sample = FVoxelSurfaceQuery::SampleSurfacePoint(*Mode, Center + Probe * 1000.0, VoxelSize,
		Config->NoiseParams, Snapshot, Sea);
	const FVoxelTerrainSurfacePoint Expected = Mode->GetSurfacePoint(Center + Probe, Config->NoiseParams);
	TestTrue(TEXT("SampleSurfacePoint: the analytic surface point"), FVector::Dist(Sample.Position, Expected.Position) < 0.01);
	TestTrue(TEXT("SampleSurfacePoint: local up is radial"), FVector::Dist(Sample.Up, Probe) < 1.0e-6);
	TestTrue(TEXT("SampleSurfacePoint: the slope against local up"),
		FMath::IsNearlyEqual(Sample.SlopeDegrees, FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(Expected.Normal, Probe))), 0.01));

	Config->RemoveFromRoot();
	Biomes->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
