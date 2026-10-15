// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "IslandBowlWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Island shape parameters (WORLD_MODES_REVIEW_PLAN.md KI4, WM-H6). Pure CPU, runs headless.
//
// Every falloff curve runs from 1 at the island radius to exactly 0 at its edge (R + falloff width), so the
// terrain meets EdgeHeight there; the Exponential curve used to stop at e^-3 and leave a step. Beyond the
// edge, the world mode's density query agrees with generation, which writes a flat floor 1,000 uu below
// EdgeHeight; the query used to call everything there air.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIslandFalloffReachesEdgeTest,
	"VoxelWorlds.Generation.IslandBowl.FalloffReachesEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIslandFalloffReachesEdgeTest::RunTest(const FString& Parameters)
{
	for (const EIslandFalloffType Type : { EIslandFalloffType::Linear, EIslandFalloffType::Smooth,
		EIslandFalloffType::Squared, EIslandFalloffType::Exponential })
	{
		const int32 TypeIndex = static_cast<int32>(Type);
		TestEqual(FString::Printf(TEXT("Type %d: 1 at the island radius"), TypeIndex),
			FIslandBowlWorldMode::ApplyFalloffCurve(0.0f, Type), 1.0f, 1.0e-6f);
		TestEqual(FString::Printf(TEXT("Type %d: 0 at the edge"), TypeIndex),
			FIslandBowlWorldMode::ApplyFalloffCurve(1.0f, Type), 0.0f, 1.0e-6f);

		float Previous = 1.0f;
		bool bMonotonic = true;
		for (int32 Step = 1; Step <= 100; ++Step)
		{
			const float Value = FIslandBowlWorldMode::ApplyFalloffCurve(Step / 100.0f, Type);
			bMonotonic &= Value <= Previous + 1.0e-6f && Value >= -1.0e-6f;
			Previous = Value;
		}
		TestTrue(FString::Printf(TEXT("Type %d: falls monotonically from 1 to 0"), TypeIndex), bMonotonic);

		// Both shapes reach EdgeHeight just inside the edge, whatever the base terrain.
		FIslandBowlParams Island(10000.0f, 2000.0f, Type);
		Island.EdgeHeight = -1500.0f;
		for (const EIslandShape Shape : { EIslandShape::Circular, EIslandShape::Rectangle })
		{
			Island.Shape = Shape;
			const float JustInside = FIslandBowlWorldMode::ComposeTerrainHeight(11999.9f, 0.0f, 4000.0f, Island);
			TestEqual(FString::Printf(TEXT("Type %d shape %d: EdgeHeight at the edge"), TypeIndex, static_cast<int32>(Shape)),
				JustInside, Island.EdgeHeight, 1.0f);
			TestEqual(FString::Printf(TEXT("Type %d shape %d: the floor beyond it"), TypeIndex, static_cast<int32>(Shape)),
				FIslandBowlWorldMode::ComposeTerrainHeight(12000.1f, 0.0f, 4000.0f, Island),
				Island.EdgeHeight - FIslandBowlWorldMode::OutsideFloorDepth, 1.0e-3f);
		}
	}
	return true;
}

/**
 * GetDensityAt, GetTerrainHeightAt and CPU generation agree beyond the edge: solid under the floor, air above it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIslandDensityBeyondEdgeTest,
	"VoxelWorlds.Generation.IslandBowl.DensityMatchesGenerationBeyondEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIslandDensityBeyondEdgeTest::RunTest(const FString& Parameters)
{
	FIslandBowlParams Island(3000.0f, 1000.0f, EIslandFalloffType::Smooth);
	Island.EdgeHeight = -1000.0f;
	const FWorldModeTerrainParams Terrain(0.0f, 2000.0f, 0.0f);
	const FIslandBowlWorldMode Mode(Terrain, Island);

	FVoxelNoiseParams Noise;
	Noise.NoiseType = EVoxelNoiseType::Perlin;
	Noise.Seed = 4321;
	Noise.Frequency = 0.002f;
	Noise.Octaves = 3;

	const float Floor = Island.EdgeHeight - FIslandBowlWorldMode::OutsideFloorDepth;
	TestEqual(TEXT("GetTerrainHeightAt beyond the edge is the floor"), Mode.GetTerrainHeightAt(4500.0f, 300.0f, Noise), Floor, 1.0e-3f);
	TestTrue(TEXT("GetDensityAt 500 uu under the floor is solid"), Mode.GetDensityAt(FVector(4500.0f, 300.0f, Floor - 500.0f), 0, 0.0f) > 0.0f);
	TestTrue(TEXT("GetDensityAt 500 uu over the floor is air"), Mode.GetDensityAt(FVector(4500.0f, 300.0f, Floor + 500.0f), 0, 0.0f) < 0.0f);

	// Generation: a chunk wholly beyond the edge (x 4,800-6,400), straddling the floor.
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(3, 0, -2);
	Request.ChunkSize = 16;
	Request.VoxelSize = 100.0f;
	Request.WorldMode = EWorldMode::IslandBowl;
	Request.SeaLevel = Terrain.SeaLevel;
	Request.HeightScale = Terrain.HeightScale;
	Request.BaseHeight = Terrain.BaseHeight;
	Request.bEnableBiomes = false;
	Request.bEnableCaves = false;
	Request.NoiseParams = Noise;
	Request.IslandParams = Island;

	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();
	TArray<FVoxelData> Voxels;
	if (!TestTrue(TEXT("Generated the chunk"), Generator.GenerateChunkCPU(Request, Voxels)))
	{
		return true;
	}

	const FVector ChunkOrigin = Request.GetChunkWorldPosition();
	int32 Disagreements = 0;
	int32 SolidVoxels = 0;
	for (int32 Index = 0; Index < Voxels.Num(); ++Index)
	{
		const int32 X = Index % Request.ChunkSize;
		const int32 Y = (Index / Request.ChunkSize) % Request.ChunkSize;
		const int32 Z = Index / (Request.ChunkSize * Request.ChunkSize);
		const FVector Position = ChunkOrigin + FVector(X, Y, Z) * Request.VoxelSize;
		const float Density = Mode.GetDensityAt(Position, 0, 0.0f);
		// Half a voxel either side of the surface is the generator's density ramp; compare clear of it.
		if (FMath::Abs(Density) < Request.VoxelSize)
		{
			continue;
		}
		SolidVoxels += Voxels[Index].IsSolid() ? 1 : 0;
		Disagreements += (Density > 0.0f) != Voxels[Index].IsSolid() ? 1 : 0;
	}
	TestTrue(TEXT("The chunk holds part of the floor"), SolidVoxels > 0);
	TestEqual(TEXT("Voxels where GetDensityAt and generation disagree beyond the edge"), Disagreements, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
