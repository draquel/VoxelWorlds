// Copyright Daniel Raquel. All Rights Reserved.

// Review B probes (Documentation/WORLD_MODES_REVIEW_PLAN.md §5 I1): island height queries against the real generated
// surface, in the island, in the falloff band and beyond it. They load the review configs
// (/Game/PluginTesting/Review/Review_Island_*) so the numbers are the demo's seed, biomes and water line.
//
// These tests record what the code does today and pass; the numbers go to the review (KI1, KI2, KI4). The checks that
// would fail today are logged as "PROBE" lines instead of being asserted, so the suite stays green until the findings
// are fixed. Turn them into assertions with the fix. KI4's are asserted since WM-H6 (2026-10-14): no falloff curve
// leaves a step at R+F, and GetDensityAt agrees with the seabed beyond R+F; since WM-E1 (KI2) the seabed continues
// from the falloff with no step at all (it was a 1,000 uu cliff to a flat floor). The InfinitePlane
// height column is what POI placement sampled before WM-A3 (2026-10-15); placement now samples the world mode, whose
// height the "island analytic" column checks.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "IslandBowlWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace IslandProbe
{
	UVoxelWorldConfiguration* LoadReviewConfig(const TCHAR* Name)
	{
		return LoadObject<UVoxelWorldConfiguration>(nullptr, *FString::Printf(TEXT("/Game/PluginTesting/Review/%s.%s"), Name, Name));
	}

	/** The surface generation produces at (X, Y): a CPU island chunk stack around ExpectedZ, caves off. FLT_MAX if none. */
	float GenerateIslandSurfaceZ(FVoxelCPUNoiseGenerator& Generator, const UVoxelWorldConfiguration& Config,
		const FIslandBowlParams& Island, float X, float Y, float ExpectedZ)
	{
		const int32 ChunkSize = Config.ChunkSize;
		const float VoxelSize = Config.VoxelSize;
		const float ChunkWorldSize = ChunkSize * VoxelSize;
		const FVector Origin = Config.WorldOrigin;
		const int32 ChunkX = FMath::FloorToInt((X - Origin.X) / ChunkWorldSize);
		const int32 ChunkY = FMath::FloorToInt((Y - Origin.Y) / ChunkWorldSize);
		const int32 CentreChunkZ = FMath::FloorToInt((ExpectedZ - Origin.Z) / ChunkWorldSize);
		const int32 Lx = FMath::Clamp(FMath::RoundToInt((X - Origin.X - ChunkX * ChunkWorldSize) / VoxelSize), 0, ChunkSize - 1);
		const int32 Ly = FMath::Clamp(FMath::RoundToInt((Y - Origin.Y - ChunkY * ChunkWorldSize) / VoxelSize), 0, ChunkSize - 1);

		constexpr int32 StackHalf = 2;
		const int32 BaseChunkZ = CentreChunkZ - StackHalf;
		TArray<FVoxelData> Column;
		Column.Reserve(ChunkSize * (2 * StackHalf + 1));
		for (int32 Cz = 0; Cz <= 2 * StackHalf; ++Cz)
		{
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = FIntVector(ChunkX, ChunkY, BaseChunkZ + Cz);
			Request.ChunkSize = ChunkSize;
			Request.VoxelSize = VoxelSize;
			Request.LODLevel = 0;
			FVoxelWorldModeFactory::FillGenerationRequest(Config, Request);
			Request.bEnableBiomes = false; // the height path only; continentalness applies regardless
			Request.bEnableCaves = false;
			Request.BiomeConfiguration = Config.BiomeConfiguration;
			Request.NoiseParams = Config.NoiseParams;
			Request.IslandParams = Island; // the config's, or a variant of it

			TArray<FVoxelData> ChunkData;
			if (!Generator.GenerateChunkCPU(Request, ChunkData))
			{
				return FLT_MAX;
			}
			for (int32 Lz = 0; Lz < ChunkSize; ++Lz)
			{
				Column.Add(ChunkData[Lx + Ly * ChunkSize + Lz * ChunkSize * ChunkSize]);
			}
		}

		float Height = 0.0f;
		uint8 Material = 0, Biome = 0;
		const float BaseZ = Origin.Z + BaseChunkZ * ChunkWorldSize;
		return FVoxelSurfaceQuery::ExtractSurfaceFromColumn(Column, BaseZ, VoxelSize, Height, Material, Biome) ? Height : FLT_MAX;
	}

	struct FZoneStats
	{
		int32 Samples = 0;
		float MaxIslandGap = 0.0f;      // |island analytic - generated|
		float MaxPlaneGap = 0.0f;       // |InfinitePlane analytic (POI placement) - generated|
		float MeanPlaneGap = 0.0f;
		int32 PlaneDryIslandWet = 0;    // POI math says land, the island is under water there
	};
}

/**
 * KI1, KI2, KI4: along 16 rays from the island centre, compare the island's analytic height
 * (FIslandBowlWorldMode::GetTerrainHeightAt, used by the chunk manager's surface queries) and the InfinitePlane height
 * POI placement samples (FInfinitePlaneWorldMode::SampleTerrainHeight) with the surface CPU generation produces.
 * Zones: inside IslandRadius, the falloff band, and the generated ring beyond R+F.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIslandReviewHeightProbeTest,
	"VoxelWorlds.Generation.Island.HeightProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIslandReviewHeightProbeTest::RunTest(const FString& Parameters)
{
	using namespace IslandProbe;

	for (const TCHAR* Name : { TEXT("Review_Island_S"), TEXT("Review_Island_M") })
	{
		UVoxelWorldConfiguration* Config = LoadReviewConfig(Name);
		if (!Config)
		{
			AddWarning(FString::Printf(TEXT("%s not found: run Scripts/setup_world_modes_review.py"), Name));
			continue;
		}
		const FIslandBowlParams Island = FVoxelWorldModeFactory::MakeIslandParams(*Config);
		const FWorldModeTerrainParams Terrain(Config->SeaLevel, Config->HeightScale, Config->BaseHeight);
		FIslandBowlWorldMode IslandMode(Terrain, Island);
		IslandMode.SetBiomeContext(Config->BiomeConfiguration);
		const FVoxelBiomeSnapshot Biome = FVoxelBiomeSnapshot::FromConfig(Config->BiomeConfiguration);
		const float WaterLine = Config->bEnableWaterLevel ? Config->WaterLevel : Config->SeaLevel;

		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();

		const float R = Island.IslandRadius;
		const float F = Island.FalloffWidth;
		// Fractions of the distance from the centre: inner area, falloff band, beyond R+F (still inside the
		// one-chunk-diagonal ring the LOD cull keeps).
		const float ChunkDiagonal = Config->ChunkSize * Config->VoxelSize * UE_SQRT_2;
		struct FZone { const TCHAR* Key; TArray<float> Value; };
		const FZone Zones[] = {
			{ TEXT("inner"), { 0.1f * R, 0.4f * R, 0.7f * R, 0.95f * R } },
			{ TEXT("band"), { R + 0.1f * F, R + 0.3f * F, R + 0.5f * F, R + 0.7f * F, R + 0.9f * F } },
			{ TEXT("beyond"), { R + F + 300.0f, R + F + 0.5f * ChunkDiagonal } },
		};

		for (const FZone& Zone : Zones)
		{
			FZoneStats Stats;
			for (int32 Ray = 0; Ray < 16; ++Ray)
			{
				const float Angle = UE_TWO_PI * Ray / 16.0f + 0.1f;
				for (const float Distance : Zone.Value)
				{
					const float X = Island.CenterX + Distance * FMath::Cos(Angle);
					const float Y = Island.CenterY + Distance * FMath::Sin(Angle);
					const float IslandH = IslandMode.GetTerrainHeightAt(X, Y, Config->NoiseParams);
					const float PlaneH = FInfinitePlaneWorldMode::SampleTerrainHeight(X, Y, Terrain, Config->NoiseParams, &Biome);
					const float GenH = GenerateIslandSurfaceZ(Generator, *Config, Island, X, Y, IslandH);
					if (GenH == FLT_MAX)
					{
						AddWarning(FString::Printf(TEXT("%s %s: no generated surface at (%.0f, %.0f), analytic %.0f"), Name, Zone.Key, X, Y, IslandH));
						continue;
					}
					++Stats.Samples;
					Stats.MaxIslandGap = FMath::Max(Stats.MaxIslandGap, FMath::Abs(IslandH - GenH));
					Stats.MaxPlaneGap = FMath::Max(Stats.MaxPlaneGap, FMath::Abs(PlaneH - GenH));
					Stats.MeanPlaneGap += FMath::Abs(PlaneH - GenH);
					Stats.PlaneDryIslandWet += (PlaneH >= WaterLine && GenH < WaterLine) ? 1 : 0;
				}
			}
			if (Stats.Samples > 0)
			{
				Stats.MeanPlaneGap /= Stats.Samples;
			}
			AddInfo(FString::Printf(TEXT("PROBE %s %s: %d samples | island analytic vs generated max %.1f uu | InfinitePlane height (POI placement before WM-A3) vs generated max %.0f mean %.0f uu | it says land where the island is under water: %d"),
				Name, Zone.Key, Stats.Samples, Stats.MaxIslandGap, Stats.MaxPlaneGap, Stats.MeanPlaneGap, Stats.PlaneDryIslandWet));

			// The island's own analytic height must match generation everywhere it generates (one voxel of slack).
			TestTrue(FString::Printf(TEXT("%s %s: island analytic height within one voxel of the generated surface (max %.1f)"), Name, Zone.Key, Stats.MaxIslandGap),
				Stats.MaxIslandGap <= Config->VoxelSize);
		}

		// KI2 / KI4: the step at R+F. Just inside, the falloff leaves EdgeHeight + factor * relief; just outside,
		// generation writes the seabed, which starts at EdgeHeight and slopes down (WM-E1). Every curve reaches
		// EdgeHeight at the edge, so nothing steps there: the floor beyond used to drop 1,000 uu (KI2), and Exponential
		// added ~5% of the relief on top (KI4).
		for (const EIslandFalloffType Falloff : { Island.FalloffType, EIslandFalloffType::Exponential })
		{
			FIslandBowlParams Variant = Island;
			Variant.FalloffType = Falloff;
			FIslandBowlWorldMode VariantMode(Terrain, Variant);
			VariantMode.SetBiomeContext(Config->BiomeConfiguration);
			float MaxStep = 0.0f, MinStep = FLT_MAX;
			for (int32 Ray = 0; Ray < 16; ++Ray)
			{
				const float Angle = UE_TWO_PI * Ray / 16.0f + 0.1f;
				const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
				const FVector2D Inside = FVector2D(Variant.CenterX, Variant.CenterY) + Dir * (R + F - 40.0f);
				const FVector2D Outside = FVector2D(Variant.CenterX, Variant.CenterY) + Dir * (R + F + 40.0f);
				const float InsideGen = GenerateIslandSurfaceZ(Generator, *Config, Variant, Inside.X, Inside.Y,
					VariantMode.GetTerrainHeightAt(Inside.X, Inside.Y, Config->NoiseParams));
				const float OutsideGen = GenerateIslandSurfaceZ(Generator, *Config, Variant, Outside.X, Outside.Y,
					VariantMode.GetTerrainHeightAt(Outside.X, Outside.Y, Config->NoiseParams));
				if (InsideGen != FLT_MAX && OutsideGen != FLT_MAX)
				{
					MaxStep = FMath::Max(MaxStep, InsideGen - OutsideGen);
					MinStep = FMath::Min(MinStep, InsideGen - OutsideGen);
				}
			}
			AddInfo(FString::Printf(TEXT("PROBE %s edge step at R+F (falloff type %d): generated surface drops %.0f to %.0f uu across 80 uu (EdgeHeight %.0f, seabed slopes %.0f uu down over %.0f uu beyond)"),
				Name, static_cast<int32>(Falloff), MinStep, MaxStep, Variant.EdgeHeight, Variant.SeabedDepth, Variant.SeabedSlopeWidth));
			TestTrue(FString::Printf(TEXT("%s falloff type %d: no step at R+F (max %.0f uu across 80 uu)"),
				Name, static_cast<int32>(Falloff), MaxStep),
				MaxStep <= 1.5f * Config->VoxelSize);
		}

		// KI4: the world mode's height bounds leave out the floor generation writes beyond R+F.
		float BoundsMin = 0.0f, BoundsMax = 0.0f;
		IslandMode.GetTerrainHeightBounds(BoundsMin, BoundsMax);
		const float OpenSeaFloor = Island.EdgeHeight - Island.SeabedDepth;
		AddInfo(FString::Printf(TEXT("PROBE %s GetTerrainHeightBounds [%.0f, %.0f]; the open-sea floor beyond R+F at %.0f is %s the bounds"),
			Name, BoundsMin, BoundsMax, OpenSeaFloor, OpenSeaFloor < BoundsMin ? TEXT("below") : TEXT("inside")));
		TestTrue(FString::Printf(TEXT("%s: the open-sea floor is inside the height bounds"), Name), OpenSeaFloor >= BoundsMin);

		// The island world mode's own density query agrees with the seabed generation writes beyond R+F (it used to
		// call everything there air).
		const float BeyondX = Island.CenterX + R + F + 300.0f;
		const float SeabedZ = IslandMode.GetTerrainHeightAt(BeyondX, Island.CenterY, Config->NoiseParams);
		const float BeyondDensity = IslandMode.GetDensityAt(FVector(BeyondX, Island.CenterY, SeabedZ - 500.0f), 0, 0.0f);
		AddInfo(FString::Printf(TEXT("PROBE %s GetDensityAt 500 uu under the generated seabed beyond R+F (at %.0f) = %.0f (negative = air)"),
			Name, SeabedZ, BeyondDensity));
		TestTrue(FString::Printf(TEXT("%s GetDensityAt is solid under the seabed beyond R+F"), Name), BeyondDensity > 0.0f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
