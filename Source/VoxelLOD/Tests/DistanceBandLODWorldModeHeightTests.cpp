// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Height through the world mode (WORLD_MODES_REVIEW_PLAN.md §14.2 WM-A3). Pure logic, runs headless.
//
// The far-band slab used InfinitePlane's static height math without continentalness, so with continentalness on its
// per-column range sat off the generated surface (by up to the continentalness height offset). It now samples the
// configured world mode, the height generation produces; the terrain height bounds come from the world mode too.
// ---------------------------------------------------------------------------

namespace LODWorldModeHeightTest
{
	/** Exposes the protected slab sample. */
	class FProbeStrategy : public FDistanceBandLODStrategy
	{
	public:
		using FDistanceBandLODStrategy::GetColumnSurfaceRange;
		float GetTerrainMin() const { return TerrainMinHeight; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDistanceBandSlabUsesWorldModeHeightTest,
	"VoxelWorlds.LOD.SlabUsesWorldModeHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDistanceBandSlabUsesWorldModeHeightTest::RunTest(const FString& Parameters)
{
	using namespace LODWorldModeHeightTest;

	// Continentalness that shifts the ground by -2,000 to +1,000 uu and scales the relief by 0.3 to 1.
	UVoxelBiomeConfiguration* Biome = NewObject<UVoxelBiomeConfiguration>();
	Biome->AddToRoot();
	Biome->InitializeDefaults();
	Biome->bEnableContinentalness = true;
	if (FRichCurve* HC = Biome->ContinentalnessHeightCurve.GetRichCurve())
	{
		HC->Reset(); HC->AddKey(-1.0f, -2000.0f); HC->AddKey(0.0f, 0.0f); HC->AddKey(1.0f, 1000.0f);
	}
	if (FRichCurve* SC = Biome->ContinentalnessHeightScaleCurve.GetRichCurve())
	{
		SC->Reset(); SC->AddKey(-1.0f, 0.3f); SC->AddKey(1.0f, 1.0f);
	}
	Biome->RebuildBakedCurves();

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->MeshingMode = EMeshingMode::DualContouring;
	Config->VoxelSize = 75.0f;
	Config->ChunkSize = 64;
	Config->ViewDistance = 40000.0f;
	Config->LODBands = { FLODBand(0.0f, 7000.0f, 0, 1), FLODBand(7000.0f, 40000.0f, 1, 2) };
	Config->SeaLevel = 0.0f;
	Config->HeightScale = 3000.0f;
	Config->BaseHeight = 0.0f;
	Config->NoiseParams.Seed = 4242;
	Config->NoiseParams.Frequency = 0.0002f;
	Config->bSurfaceSlabCulling = true;
	Config->bEnableWaterLevel = false;
	Config->BiomeConfiguration = Biome;

	FProbeStrategy Strategy;
	Strategy.Initialize(Config);
	const TSharedRef<IVoxelWorldMode> World = FVoxelWorldModeFactory::Create(*Config);
	const FWorldModeTerrainParams Terrain = FVoxelWorldModeFactory::MakeTerrainParams(*Config);
	const float ChunkWorldSize = Config->ChunkSize * Config->VoxelSize;

	int32 Mismatches = 0;
	float MaxStaticOffset = 0.0f;
	for (int32 X = -20; X <= 20; X += 4)
	{
		for (int32 Y = -20; Y <= 20; Y += 4)
		{
			float SlabMin = 0.0f, SlabMax = 0.0f;
			Strategy.GetColumnSurfaceRange(X, Y, Config->WorldOrigin, SlabMin, SlabMax);

			// The same 5-point footprint sample, on the world mode and on the old static math.
			float WorldMin = TNumericLimits<float>::Max(), WorldMax = TNumericLimits<float>::Lowest();
			float StaticMin = TNumericLimits<float>::Max(), StaticMax = TNumericLimits<float>::Lowest();
			for (const FVector2D& Offset : { FVector2D(0, 0), FVector2D(1, 0), FVector2D(0, 1), FVector2D(1, 1), FVector2D(0.5, 0.5) })
			{
				const float SX = (X + Offset.X) * ChunkWorldSize;
				const float SY = (Y + Offset.Y) * ChunkWorldSize;
				const float H = static_cast<float>(World->GetSurfaceOnUpLine(FVector(SX, SY, 0.0), Config->NoiseParams).Position.Z);
				WorldMin = FMath::Min(WorldMin, H);
				WorldMax = FMath::Max(WorldMax, H);
				const float S = FInfinitePlaneWorldMode::NoiseToTerrainHeight(
					FInfinitePlaneWorldMode::SampleTerrainNoise2D(SX, SY, Config->NoiseParams), Terrain);
				StaticMin = FMath::Min(StaticMin, S);
				StaticMax = FMath::Max(StaticMax, S);
			}
			Mismatches += (FMath::IsNearlyEqual(SlabMin, WorldMin, 0.01f) && FMath::IsNearlyEqual(SlabMax, WorldMax, 0.01f)) ? 0 : 1;
			MaxStaticOffset = FMath::Max(MaxStaticOffset, FMath::Max(FMath::Abs(StaticMin - WorldMin), FMath::Abs(StaticMax - WorldMax)));
		}
	}
	AddInfo(FString::Printf(TEXT("The old static slab height was off the generated surface by up to %.0f uu here"), MaxStaticOffset));
	TestEqual(TEXT("The slab's column range is the world mode's height range"), Mismatches, 0);
	TestTrue(TEXT("Continentalness moves the surface here (the test would not tell the two apart otherwise)"), MaxStaticOffset > 100.0f);

	// The terrain bounds come from the world mode: one chunk below its minimum.
	const float BoundsMin = World->GetWorldBounds().MinZ;
	TestEqual(TEXT("Terrain bounds from the world mode"), Strategy.GetTerrainMin(), BoundsMin - ChunkWorldSize, 0.01f);

	Biome->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
