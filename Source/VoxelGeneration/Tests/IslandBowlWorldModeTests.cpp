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
// edge, the world mode's density query agrees with generation, which writes the seabed there (a slope from
// EdgeHeight since WM-E1; a flat floor 1,000 uu down before); the query used to call everything there air.
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
			// WM-E1: the seabed starts at EdgeHeight, so there is no step at the edge (it was 1,000 uu).
			TestEqual(FString::Printf(TEXT("Type %d shape %d: the seabed beyond it continues from EdgeHeight"), TypeIndex, static_cast<int32>(Shape)),
				FIslandBowlWorldMode::ComposeTerrainHeight(12000.1f, 0.0f, 4000.0f, Island), Island.EdgeHeight, 1.0f);
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
	// A short, deep slope so the test chunk below holds both the slope and the flat open-sea floor (WM-E1).
	Island.SeabedDepth = 2000.0f;
	Island.SeabedSlopeWidth = 1500.0f;
	const FWorldModeTerrainParams Terrain(0.0f, 2000.0f, 0.0f);
	const FIslandBowlWorldMode Mode(Terrain, Island);

	FVoxelNoiseParams Noise;
	Noise.NoiseType = EVoxelNoiseType::Perlin;
	Noise.Seed = 4321;
	Noise.Frequency = 0.002f;
	Noise.Octaves = 3;

	const float Seabed = FIslandBowlWorldMode::GetSeabedHeight(
		FIslandBowlWorldMode::GetDistancePastEdge(4500.0f, 300.0f, Island), Island);
	TestEqual(TEXT("GetTerrainHeightAt beyond the edge is the seabed"), Mode.GetTerrainHeightAt(4500.0f, 300.0f, Noise), Seabed, 1.0e-3f);
	TestTrue(TEXT("The seabed there is on the slope, between EdgeHeight and the open-sea floor"),
		Seabed < Island.EdgeHeight - 10.0f && Seabed > Island.EdgeHeight - Island.SeabedDepth + 10.0f);
	TestTrue(TEXT("GetDensityAt 500 uu under the seabed is solid"), Mode.GetDensityAt(FVector(4500.0f, 300.0f, Seabed - 500.0f), 0, 0.0f) > 0.0f);
	TestTrue(TEXT("GetDensityAt 500 uu over the seabed is air"), Mode.GetDensityAt(FVector(4500.0f, 300.0f, Seabed + 500.0f), 0, 0.0f) < 0.0f);

	// Generation: a chunk wholly beyond the edge (x 4,800-6,400, 800-2,400 uu past it), holding the end of the slope
	// and the flat floor beyond (z -3,200 to -1,600).
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
	TestTrue(TEXT("The chunk holds part of the seabed"), SolidVoxels > 0);
	TestEqual(TEXT("Voxels where GetDensityAt and generation disagree beyond the edge"), Disagreements, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

/**
 * The seabed beyond the edge (WM-E1, KI2): no step at R + F, a smoothstep down by SeabedDepth over SeabedSlopeWidth,
 * flat beyond; circles measure the distance radially, rectangles by the larger axis excess (the falloff's metric); a
 * slope width of 0 is the old step; the height bounds hold the open-sea floor. Pure CPU.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIslandSeabedSlopeTest,
	"VoxelWorlds.Generation.IslandBowl.SeabedSlopesFromEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIslandSeabedSlopeTest::RunTest(const FString& Parameters)
{
	FIslandBowlParams Island(30000.0f, 10000.0f, EIslandFalloffType::Smooth);
	Island.CenterX = 1234.0f;
	Island.CenterY = -4321.0f;
	Island.EdgeHeight = -3500.0f;
	Island.SeabedDepth = 6000.0f;
	Island.SeabedSlopeWidth = 20000.0f;
	const float Edge = Island.GetTotalExtent();

	for (const EIslandShape Shape : { EIslandShape::Circular, EIslandShape::Rectangle })
	{
		Island.Shape = Shape;
		Island.SizeY = Shape == EIslandShape::Rectangle ? 20000.0f : Island.IslandRadius;
		const int32 ShapeIndex = static_cast<int32>(Shape);
		int32 Rising = 0;
		float MaxStepAtEdge = 0.0f;
		for (int32 Ray = 0; Ray < 64; ++Ray)
		{
			const float Angle = UE_TWO_PI * (Ray + 0.25f) / 64.0f;
			const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
			const FVector2D Centre(Island.CenterX, Island.CenterY);
			// Walk out along the ray from just inside the footprint: the height never rises past the edge.
			float Previous = TNumericLimits<float>::Max();
			bool bWasInside = true;
			for (float T = 0.0f; T < 60000.0f; T += 50.0f)
			{
				const FVector2D P = Centre + Dir * T;
				const bool bInside = FIslandBowlWorldMode::IsWithinIslandBounds(P.X, P.Y, Island);
				const float H = FIslandBowlWorldMode::ComposeTerrainHeight(P.X, P.Y, Island.EdgeHeight + 2000.0f, Island);
				if (!bInside)
				{
					if (bWasInside)
					{
						// The first step out: the seabed continues from the falloff's EdgeHeight.
						MaxStepAtEdge = FMath::Max(MaxStepAtEdge, FMath::Abs(H - Island.EdgeHeight));
					}
					Rising += H > Previous + 1.0e-3f ? 1 : 0;
					Previous = H;
				}
				bWasInside = bInside;
			}
		}
		TestEqual(FString::Printf(TEXT("Shape %d: the seabed never rises going out"), ShapeIndex), Rising, 0);
		TestTrue(FString::Printf(TEXT("Shape %d: no step at the edge (max %.2f uu over 50 uu)"), ShapeIndex, MaxStepAtEdge),
			MaxStepAtEdge < 5.0f);

		// The profile: half way down at half the width, the open-sea floor from the width on.
		const float Half = FIslandBowlWorldMode::GetSeabedHeight(0.5f * Island.SeabedSlopeWidth, Island);
		TestEqual(FString::Printf(TEXT("Shape %d: half way down at half the slope width"), ShapeIndex),
			Half, Island.EdgeHeight - 0.5f * Island.SeabedDepth, 0.5f);
		TestEqual(FString::Printf(TEXT("Shape %d: the open-sea floor past the slope"), ShapeIndex),
			FIslandBowlWorldMode::GetSeabedHeight(Island.SeabedSlopeWidth * 3.0f, Island), Island.EdgeHeight - Island.SeabedDepth, 1.0e-3f);
	}

	// The distance past the edge: radial for a circle, the larger axis excess for a rectangle.
	Island.Shape = EIslandShape::Circular;
	TestEqual(TEXT("Circle: radial distance past R + F"),
		FIslandBowlWorldMode::GetDistancePastEdge(Island.CenterX + 3000.0f, Island.CenterY + Edge + 1000.0f, Island),
		FMath::Sqrt(FMath::Square(3000.0f) + FMath::Square(Edge + 1000.0f)) - Edge, 0.5f);
	Island.Shape = EIslandShape::Rectangle;
	Island.SizeY = 20000.0f;
	TestEqual(TEXT("Rectangle: the larger axis excess"),
		FIslandBowlWorldMode::GetDistancePastEdge(Island.CenterX + Island.GetTotalExtentX() + 700.0f, Island.CenterY + Island.GetTotalExtentY() + 300.0f, Island),
		700.0f, 0.5f);

	// A slope width of 0 is a step (the old floor: depth 1,000).
	Island.SeabedDepth = 1000.0f;
	Island.SeabedSlopeWidth = 0.0f;
	TestEqual(TEXT("Width 0: a step down by the depth"), FIslandBowlWorldMode::GetSeabedHeight(1.0f, Island), Island.EdgeHeight - 1000.0f, 1.0e-3f);

	// The height bounds hold the open-sea floor (the streaming window and the walls read them).
	Island.Shape = EIslandShape::Circular;
	Island.SeabedDepth = 30000.0f;
	Island.SeabedSlopeWidth = 20000.0f;
	const FIslandBowlWorldMode Mode(FWorldModeTerrainParams(0.0f, 2000.0f, 0.0f), Island);
	float BoundsMin = 0.0f, BoundsMax = 0.0f;
	Mode.GetTerrainHeightBounds(BoundsMin, BoundsMax);
	TestTrue(TEXT("The height bounds reach the open-sea floor"), BoundsMin <= Island.EdgeHeight - Island.SeabedDepth);
	return true;
}
