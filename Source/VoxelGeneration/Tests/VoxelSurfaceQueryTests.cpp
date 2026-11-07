// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelBiomeSnapshot.h"
#include "InfinitePlaneWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelNoiseTypes.h"
#include "VoxelSeaSurface.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

// ==================== FVoxelSurfaceQuery Tests ====================
//
// Verifies the shared surface-query utility (used by both FVoxelTreeInjector and the
// VoxelPCG surface sampler) delegates to the generator correctly and that its
// height/slope/normal derivations are internally consistent.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSurfaceQueryConsistencyTest, "VoxelWorlds.Generation.SurfaceQuery.Consistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSurfaceQueryConsistencyTest::RunTest(const FString& Parameters)
{
	// Known generator: infinite-plane world mode with concrete terrain params.
	FWorldModeTerrainParams TerrainParams;
	TerrainParams.SeaLevel = 0.0f;
	TerrainParams.HeightScale = 2000.0f;
	TerrainParams.BaseHeight = 500.0f;
	const FInfinitePlaneWorldMode WorldMode(TerrainParams);

	FVoxelNoiseParams NoiseParams;
	NoiseParams.Frequency = 0.0005f;

	const float VoxelSize = 100.0f;
	const float WorldX = 1234.0f;
	const float WorldY = -567.0f;

	// 1. GetSurfaceHeight must match the world mode's own height sample (the "known sample").
	const float DirectHeight = WorldMode.GetTerrainHeightAt(WorldX, WorldY, NoiseParams);
	const float QueryHeight = FVoxelSurfaceQuery::GetSurfaceHeight(WorldMode, WorldX, WorldY, NoiseParams);
	TestEqual(TEXT("GetSurfaceHeight matches the mode's own height"), QueryHeight, DirectHeight);

	// 2. SampleSurface aggregates the individual queries consistently.
	// Default (invalid) snapshot = the former null-BiomeConfig case.
	const FVoxelSurfaceSample Sample = FVoxelSurfaceQuery::SampleSurface(
		WorldMode, WorldX, WorldY, VoxelSize, NoiseParams,
		FVoxelBiomeSnapshot(), /*WorldSeed*/ 12345,
		/*bEnableWaterLevel*/ false, /*WaterLevel*/ 0.0f);

	TestEqual(TEXT("SampleSurface Position.Z matches GetSurfaceHeight"), static_cast<float>(Sample.Position.Z), QueryHeight);

	const float StandaloneSlope = FVoxelSurfaceQuery::ComputeSlopeDegrees(WorldMode, WorldX, WorldY, VoxelSize, NoiseParams);
	TestEqual(TEXT("SampleSurface.SlopeDegrees matches ComputeSlopeDegrees"), Sample.SlopeDegrees, StandaloneSlope);

	const FVector StandaloneNormal = FVoxelSurfaceQuery::ComputeSurfaceNormal(WorldMode, WorldX, WorldY, VoxelSize, NoiseParams);
	TestTrue(TEXT("SampleSurface.Normal matches ComputeSurfaceNormal"), Sample.Normal.Equals(StandaloneNormal, KINDA_SMALL_NUMBER));

	// 3. Invariants: normal is normalized and up-facing; slope is a sane angle.
	TestTrue(TEXT("Normal is normalized"), FMath::IsNearlyEqual(Sample.Normal.Size(), 1.0f, 0.001f));
	TestTrue(TEXT("Normal points upward (Z > 0)"), Sample.Normal.Z > 0.0f);
	TestTrue(TEXT("Slope is within [0, 90] degrees"), Sample.SlopeDegrees >= 0.0f && Sample.SlopeDegrees <= 90.0f);

	// 4. With no biome config, material/biome default to 0 (no crash on null config).
	TestEqual(TEXT("MaterialID is 0 without biome config"), (int32)Sample.MaterialID, 0);
	TestEqual(TEXT("BiomeID is 0 without biome config"), (int32)Sample.BiomeID, 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSurfaceQueryColumnTest, "VoxelWorlds.Generation.SurfaceQuery.ExtractColumn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSurfaceQueryColumnTest::RunTest(const FString& Parameters)
{
	const float BaseZ = 0.0f;
	const float VoxelSize = 100.0f;

	// Column (low -> high): solid, solid, solid(d=200), air, air.
	// Surface is between index 2 (d=200) and index 3 (d=0): t = (127-200)/(0-200) = 0.365.
	TArray<FVoxelData> Column;
	Column.Add(FVoxelData(/*Mat*/ 5, /*Density*/ 255, /*Biome*/ 3));
	Column.Add(FVoxelData(5, 255, 3));
	Column.Add(FVoxelData(5, 200, 3));
	Column.Add(FVoxelData::Air());
	Column.Add(FVoxelData::Air());

	float Height = 0.0f; uint8 Mat = 0; uint8 Biome = 0;
	const bool bFound = FVoxelSurfaceQuery::ExtractSurfaceFromColumn(Column, BaseZ, VoxelSize, Height, Mat, Biome);
	TestTrue(TEXT("Surface found in column"), bFound);
	TestTrue(TEXT("Interpolated height ~236.5"), FMath::IsNearlyEqual(Height, 236.5f, 0.5f));
	TestEqual(TEXT("Surface material from solid voxel"), (int32)Mat, 5);
	TestEqual(TEXT("Surface biome from solid voxel"), (int32)Biome, 3);

	// All air -> no surface.
	TArray<FVoxelData> AllAir; AllAir.Init(FVoxelData::Air(), 5);
	TestFalse(TEXT("All-air column has no surface"), FVoxelSurfaceQuery::ExtractSurfaceFromColumn(AllAir, BaseZ, VoxelSize, Height, Mat, Biome));

	// All solid -> no air-over-solid transition -> no surface (surface is above this column).
	TArray<FVoxelData> AllSolid; AllSolid.Init(FVoxelData::Solid(5), 5);
	TestFalse(TEXT("All-solid column has no surface"), FVoxelSurfaceQuery::ExtractSurfaceFromColumn(AllSolid, BaseZ, VoxelSize, Height, Mat, Biome));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSurfaceQueryFlatTerrainTest, "VoxelWorlds.Generation.SurfaceQuery.FlatTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSurfaceQueryFlatTerrainTest::RunTest(const FString& Parameters)
{
	// Zero-amplitude noise => flat terrain => zero slope, straight-up normal.
	FWorldModeTerrainParams TerrainParams;
	TerrainParams.SeaLevel = 0.0f;
	TerrainParams.HeightScale = 0.0f; // No height variation
	TerrainParams.BaseHeight = 1000.0f;
	const FInfinitePlaneWorldMode WorldMode(TerrainParams);

	FVoxelNoiseParams NoiseParams;
	NoiseParams.Frequency = 0.0005f;

	const float VoxelSize = 100.0f;

	const float Slope = FVoxelSurfaceQuery::ComputeSlopeDegrees(WorldMode, 0.0f, 0.0f, VoxelSize, NoiseParams);
	TestTrue(TEXT("Flat terrain slope is ~0"), FMath::IsNearlyEqual(Slope, 0.0f, 0.01f));

	const FVector Normal = FVoxelSurfaceQuery::ComputeSurfaceNormal(WorldMode, 0.0f, 0.0f, VoxelSize, NoiseParams);
	TestTrue(TEXT("Flat terrain normal is ~UpVector"), Normal.Equals(FVector::UpVector, 0.001f));

	return true;
}

// ---------------------------------------------------------------------------
// The climate query (the weather plan's P2): the fields QueryClimateAt returns are the ones the biome came from, on a
// heightmap world and on a planet (the latitude and the lapse rate on), with the frame of the place.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSurfaceQueryClimateTest, "VoxelWorlds.Generation.SurfaceQuery.ClimateMatchesBiome",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSurfaceQueryClimateTest::RunTest(const FString& Parameters)
{
	// A biome configuration with every shaping on, so the query has to reproduce all of it.
	UVoxelBiomeConfiguration* Biomes = NewObject<UVoxelBiomeConfiguration>();
	Biomes->AddToRoot();
	Biomes->bEnableContinentalness = true;
	Biomes->bEqualizeClimate = true;
	Biomes->TemperatureLatitudeWeight = 0.7f;
	Biomes->TemperatureLapseRate = 0.5f;
	Biomes->RebuildBakedCurves();
	const FVoxelBiomeSnapshot Snapshot = FVoxelBiomeSnapshot::FromConfig(Biomes);
	TestTrue(TEXT("The snapshot is valid"), Snapshot.bIsValid);

	// 1. A heightmap world: the sample's biome is SampleSurface's at every column, and the fields are in range.
	{
		FWorldModeTerrainParams TerrainParams;
		TerrainParams.SeaLevel = 0.0f;
		TerrainParams.HeightScale = 4000.0f;
		TerrainParams.BaseHeight = 500.0f;
		const FInfinitePlaneWorldMode WorldMode(TerrainParams);
		FVoxelNoiseParams NoiseParams;
		NoiseParams.Frequency = 0.0005f;
		NoiseParams.Seed = 4711;
		int32 Checked = 0, Distinct = 0;
		uint8 FirstBiome = 0;
		for (int32 i = 0; i < 9; ++i)
		{
			const float X = -200000.0f + 50000.0f * i, Y = 130000.0f - 40000.0f * i;
			FVoxelClimateSample Climate;
			if (!TestTrue(TEXT("QueryClimateAt succeeds on a plane"), FVoxelSurfaceQuery::QueryClimateAt(WorldMode, FVector(X, Y, 0.0), NoiseParams, Snapshot, nullptr, Climate)))
			{
				return false;
			}
			const FVoxelSurfaceSample Surface = FVoxelSurfaceQuery::SampleSurface(WorldMode, X, Y, 100.0f, NoiseParams, Snapshot, NoiseParams.Seed, false, 0.0f);
			TestEqual(*FString::Printf(TEXT("The climate's biome is the surface's at (%.0f, %.0f)"), X, Y), static_cast<int32>(Climate.BiomeID), static_cast<int32>(Surface.BiomeID));
			TestEqual(TEXT("The climate's surface altitude is the surface's"), Climate.SurfaceAltitude, Surface.Altitude);
			TestTrue(TEXT("Up is +Z on a plane"), Climate.Up.Equals(FVector::UpVector));
			TestEqual(TEXT("No latitude on a plane"), Climate.SinLatitude, 0.0f);
			TestTrue(TEXT("Temperature in [-1.5, 1.5]"), Climate.Temperature >= -1.5f && Climate.Temperature <= 1.5f);
			TestTrue(TEXT("Moisture in [-1.5, 1.5]"), Climate.Moisture >= -1.5f && Climate.Moisture <= 1.5f);
			TestTrue(TEXT("Continentalness in [-1, 1]"), Climate.Continentalness >= -1.0f && Climate.Continentalness <= 1.0f);
			if (Checked == 0)
			{
				FirstBiome = Climate.BiomeID;
			}
			else if (Climate.BiomeID != FirstBiome)
			{
				++Distinct;
			}
			++Checked;
		}
		TestTrue(TEXT("The columns are not all one biome (the fields vary)"), Distinct > 0);
	}

	// 2. A planet: the sample's biome is SampleSurfacePoint's along every direction; the frame is radial; the poles are
	// colder than the equator with the latitude weight on.
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector(5000.0, -3000.0, 200.0);
		Config->WorldRadius = 250000.0f;
		Config->PlanetHeightScale = 6000.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = 6500.0f;
		Config->PlanetMaxTerrainDepth = 6500.0f;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 32;
		Config->BiomeConfiguration = Biomes;
		Config->bEnableBiomes = true;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 248675;
		Config->NoiseParams.Octaves = 3;
		Config->NoiseParams.Frequency = 0.000025f;
		Config->NoiseParams.Lacunarity = 1.681f;
		Config->NoiseParams.Persistence = 0.5f;
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FVoxelNoiseParams NoiseParams = Config->GetNoiseParams();
		const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*Config);
		const FVector Directions[] = {
			FVector(1, 0, 0), FVector(0, 1, 0), FVector(-1, 0, 0), FVector(0, -1, 0), FVector(0, 0, 1), FVector(0, 0, -1),
			FVector(1, 1, 0).GetSafeNormal(), FVector(1, 0, 1).GetSafeNormal(), FVector(0, 1, -1).GetSafeNormal(),
			FVector(-1, 1, 1).GetSafeNormal(), FVector(0.3, -0.8, 0.5).GetSafeNormal(), FVector(-0.6, -0.2, -0.75).GetSafeNormal() };
		float EquatorTemperature = 0.0f, PoleTemperature = 0.0f;
		int32 EquatorSamples = 0, PoleSamples = 0;
		for (const FVector& Direction : Directions)
		{
			const FVector WorldPos = Config->WorldOrigin + Direction * (Config->WorldRadius + 1000.0);
			FVoxelClimateSample Climate;
			if (!TestTrue(TEXT("QueryClimateAt succeeds on a planet"), FVoxelSurfaceQuery::QueryClimateAt(*Mode, WorldPos, NoiseParams, Snapshot, nullptr, Climate)))
			{
				return false;
			}
			const FVoxelSurfaceSample Surface = FVoxelSurfaceQuery::SampleSurfacePoint(*Mode, WorldPos, Config->VoxelSize, NoiseParams, Snapshot, Sea, nullptr);
			TestEqual(*FString::Printf(TEXT("The climate's biome is the surface's along (%.2f, %.2f, %.2f)"), Direction.X, Direction.Y, Direction.Z),
				static_cast<int32>(Climate.BiomeID), static_cast<int32>(Surface.BiomeID));
			TestTrue(TEXT("The climate's surface altitude is the surface's"), FMath::IsNearlyEqual(Climate.SurfaceAltitude, Surface.Altitude, 0.01f));
			TestTrue(TEXT("Up is radial on a planet"), Climate.Up.Equals(Direction, 1.0e-4));
			TestTrue(TEXT("The latitude is the direction's Z"), FMath::IsNearlyEqual(Climate.SinLatitude, static_cast<float>(Direction.Z), 1.0e-4f));
			if (FMath::Abs(Direction.Z) < 0.01)
			{
				EquatorTemperature += Climate.Temperature;
				++EquatorSamples;
			}
			else if (FMath::Abs(Direction.Z) > 0.99)
			{
				PoleTemperature += Climate.Temperature;
				++PoleSamples;
			}
		}
		TestTrue(TEXT("The poles are colder than the equator with the latitude weight on"),
			PoleSamples > 0 && EquatorSamples > 0 && PoleTemperature / PoleSamples < EquatorTemperature / EquatorSamples);

		// 3. Without a valid snapshot the sample is invalid, its fields 0, the frame still filled.
		FVoxelClimateSample Invalid;
		TestFalse(TEXT("QueryClimateAt fails without a biome snapshot"), FVoxelSurfaceQuery::QueryClimateAt(*Mode, Config->WorldOrigin + Directions[0] * Config->WorldRadius, NoiseParams, FVoxelBiomeSnapshot(), nullptr, Invalid));
		TestFalse(TEXT("The sample is invalid"), Invalid.bValid);
		TestEqual(TEXT("The temperature is 0"), Invalid.Temperature, 0.0f);
		TestTrue(TEXT("The up is still radial"), Invalid.Up.Equals(Directions[0], 1.0e-4));
		Config->RemoveFromRoot();
	}
	Biomes->RemoveFromRoot();
	return true;
}
