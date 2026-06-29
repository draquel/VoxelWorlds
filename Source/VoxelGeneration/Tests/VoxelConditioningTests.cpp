// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"

// ---------------------------------------------------------------------------
// Pure blend math
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningBlendTest,
	"VoxelWorlds.Generation.Conditioning.Blend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningBlendTest::RunTest(const FString& Parameters)
{
	// Zone centered at origin: full flatten within 100, falloff over the next 100, none beyond 200.
	TArray<FVoxelConditioningZone> Zones;
	Zones.Add(FVoxelConditioningZone(FVector2D(0.0, 0.0), 100.0f, 100.0f, 500.0f, 1.0f));

	// Center: full weight -> reaches target exactly.
	TestEqual(TEXT("Center blends fully to target"),
		FVoxelTerrainConditioning::ApplyToHeight(0.0, 0.0, 0.0f, Zones), 500.0f);

	// Inside inner radius: still full weight.
	TestEqual(TEXT("Inside inner radius blends fully"),
		FVoxelTerrainConditioning::ApplyToHeight(80.0, 0.0, 0.0f, Zones), 500.0f);

	// Beyond falloff: untouched.
	TestEqual(TEXT("Beyond falloff is untouched"),
		FVoxelTerrainConditioning::ApplyToHeight(250.0, 0.0, 100.0f, Zones), 100.0f);

	// In the falloff ring: strictly between base and target.
	const float Mid = FVoxelTerrainConditioning::ApplyToHeight(150.0, 0.0, 0.0f, Zones);
	TestTrue(TEXT("Falloff ring is partial (0 < H < target)"), Mid > 0.0f && Mid < 500.0f);

	// Strength scales the reach.
	TArray<FVoxelConditioningZone> Weak;
	Weak.Add(FVoxelConditioningZone(FVector2D(0.0, 0.0), 100.0f, 100.0f, 1000.0f, 0.5f));
	TestEqual(TEXT("Strength 0.5 reaches halfway to target"),
		FVoxelTerrainConditioning::ApplyToHeight(0.0, 0.0, 0.0f, Weak), 500.0f);

	return true;
}

// ---------------------------------------------------------------------------
// Generation hook: a flatten zone changes the generated terrain surface
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningGenerationTest,
	"VoxelWorlds.Generation.Conditioning.FlattensTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Highest solid voxel's world Z in a column (chunk origin at Z=0), or -1 if none.
static float FindColumnSurfaceZ(const TArray<FVoxelData>& Data, int32 CS, float VoxelSize, int32 X, int32 Y)
{
	for (int32 Z = CS - 1; Z >= 0; --Z)
	{
		const int32 Index = X + Y * CS + Z * CS * CS;
		if (Data[Index].Density >= VOXEL_SURFACE_THRESHOLD)
		{
			return Z * VoxelSize;
		}
	}
	return -1.0f;
}

bool FVoxelConditioningGenerationTest::RunTest(const FString& Parameters)
{
	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();

	const int32 CS = 16;
	const float VoxelSize = 100.0f;

	// InfinitePlane chunk whose natural surface sits comfortably inside the chunk's Z range
	// (low HeightScale, BaseHeight mid-chunk).
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(0, 0, 0);
	Request.ChunkSize = CS;
	Request.VoxelSize = VoxelSize;
	Request.LODLevel = 0;
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 200.0f;
	Request.BaseHeight = 800.0f;
	Request.bEnableBiomes = false;
	Request.bEnableCaves = false;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Perlin;
	Request.NoiseParams.Seed = 4242;
	Request.NoiseParams.Frequency = 0.013f;
	Request.NoiseParams.Amplitude = 1.0f;
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;

	// --- Baseline (no conditioning) ---
	TArray<FVoxelData> Natural;
	TestTrue(TEXT("Baseline generation succeeds"), Generator.GenerateChunkCPU(Request, Natural));

	const float NaturalInside = FindColumnSurfaceZ(Natural, CS, VoxelSize, 8, 8);   // world (800,800)
	const float NaturalOutside = FindColumnSurfaceZ(Natural, CS, VoxelSize, 0, 0);  // world (0,0)
	TestTrue(TEXT("Baseline has a surface at chunk center"), NaturalInside > 0.0f);
	TestTrue(TEXT("Baseline has a surface at chunk corner"), NaturalOutside > 0.0f);

	// --- Conditioned: flatten a disc at the chunk center to Z=1200, corner untouched ---
	const float TargetHeight = 1200.0f;
	Request.ConditioningZones.Add(FVoxelConditioningZone(FVector2D(800.0, 800.0), 300.0f, 100.0f, TargetHeight, 1.0f));

	TArray<FVoxelData> Conditioned;
	TestTrue(TEXT("Conditioned generation succeeds"), Generator.GenerateChunkCPU(Request, Conditioned));

	const float CondInside = FindColumnSurfaceZ(Conditioned, CS, VoxelSize, 8, 8);
	const float CondOutside = FindColumnSurfaceZ(Conditioned, CS, VoxelSize, 0, 0);

	AddInfo(FString::Printf(TEXT("Inside surface: natural %.0f -> conditioned %.0f (target %.0f). Outside: natural %.0f -> conditioned %.0f"),
		NaturalInside, CondInside, TargetHeight, NaturalOutside, CondOutside));

	// Inside the footprint the surface is flattened to (within one voxel of) the target.
	TestTrue(TEXT("Footprint surface reaches the target height"),
		FMath::Abs(CondInside - TargetHeight) <= VoxelSize);

	// Outside the footprint the terrain is unchanged.
	TestEqual(TEXT("Terrain outside the footprint is unchanged"), CondOutside, NaturalOutside);

	return true;
}
