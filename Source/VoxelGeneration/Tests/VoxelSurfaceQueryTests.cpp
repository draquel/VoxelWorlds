// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSurfaceQuery.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelNoiseTypes.h"

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
	TestEqual(TEXT("GetSurfaceHeight matches IVoxelWorldMode::GetTerrainHeightAt"), QueryHeight, DirectHeight);

	// 2. SampleSurface aggregates the individual queries consistently.
	const FVoxelSurfaceSample Sample = FVoxelSurfaceQuery::SampleSurface(
		WorldMode, WorldX, WorldY, VoxelSize, NoiseParams,
		/*BiomeConfig*/ nullptr, /*WorldSeed*/ 12345,
		/*bEnableWaterLevel*/ false, /*WaterLevel*/ 0.0f);

	TestEqual(TEXT("SampleSurface.Height matches GetSurfaceHeight"), Sample.Height, QueryHeight);

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
