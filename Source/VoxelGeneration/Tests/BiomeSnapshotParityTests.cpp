// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelSurfaceQuery.h"

// ==================== FVoxelBiomeSnapshot Parity Tests ====================
//
// FVoxelBiomeSnapshot::FromConfig must capture EVERYTHING the shared biome/material pipeline
// needs: for any input, the snapshot's blend / material / height-rule results must equal the
// UVoxelBiomeConfiguration's (both delegate to the same static cores, so the algorithms cannot
// diverge — what this test guards is the CAPTURE: a field FromConfig forgets to copy, or a
// sort it forgets to apply, shows up here as a mismatch).

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeSnapshotParityTest, "VoxelWorlds.Biome.SnapshotParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeSnapshotParityTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* Config = NewObject<UVoxelBiomeConfiguration>(GetTransientPackage());
	Config->AddToRoot();
	// Ctor defaults give Plains/Forest/Mountain/Ocean + snow/stone height rules + underwater
	// materials. Enable continentalness (curves are baked by the ctor's default keys).
	Config->bEnableContinentalness = true;

	const FVoxelBiomeSnapshot Snapshot = FVoxelBiomeSnapshot::FromConfig(Config);
	TestTrue(TEXT("Snapshot of a valid config is valid"), Snapshot.bIsValid);

	// --- 1. Biome blend parity over a T/M/C grid ---
	int32 BlendMismatches = 0;
	for (float T = -1.0f; T <= 1.0f; T += 0.25f)
	{
		for (float M = -1.0f; M <= 1.0f; M += 0.25f)
		{
			for (float C = -1.0f; C <= 1.0f; C += 0.25f)
			{
				const FBiomeBlend A = Config->GetBiomeBlend(T, M, C);
				const FBiomeBlend B = Snapshot.GetBiomeBlend(T, M, C);

				bool bEqual = (A.BiomeCount == B.BiomeCount);
				for (int32 i = 0; bEqual && i < A.BiomeCount; ++i)
				{
					bEqual = (A.BiomeIDs[i] == B.BiomeIDs[i])
						&& FMath::IsNearlyEqual(A.Weights[i], B.Weights[i], KINDA_SMALL_NUMBER);
				}
				if (!bEqual)
				{
					++BlendMismatches;
				}

				// --- 2. Material parity for this blend (surface + at depth, dry + underwater) ---
				if (Config->GetBlendedMaterial(A, 0.0f) != Snapshot.GetBlendedMaterial(B, 0.0f) ||
					Config->GetBlendedMaterial(A, 5.0f) != Snapshot.GetBlendedMaterial(B, 5.0f) ||
					Config->GetBlendedMaterialWithWater(A, 0.0f, -500.0f, 0.0f) != Snapshot.GetBlendedMaterialWithWater(B, 0.0f, -500.0f, 0.0f) ||
					Config->GetBlendedMaterialWithWater(A, 0.0f, 500.0f, 0.0f) != Snapshot.GetBlendedMaterialWithWater(B, 0.0f, 500.0f, 0.0f))
				{
					++BlendMismatches;
				}
			}
		}
	}
	TestEqual(TEXT("Biome blend + blended material parity across T/M/C grid (mismatches)"), BlendMismatches, 0);

	// --- 3. Height-rule parity across altitudes ---
	int32 RuleMismatches = 0;
	for (float H = -2000.0f; H <= 8000.0f; H += 250.0f)
	{
		for (uint8 Mat = 0; Mat < 4; ++Mat)
		{
			if (Config->ApplyHeightMaterialRules(Mat, H, 0.0f) != Snapshot.ApplyHeightMaterialRules(Mat, H, 0.0f))
			{
				++RuleMismatches;
			}
		}
	}
	TestEqual(TEXT("Height material rule parity across altitudes (mismatches)"), RuleMismatches, 0);

	// --- 4. Full surface pipeline: QuerySurfaceConditions(snapshot) equals a config-driven
	//        reference built the same way the pre-snapshot implementation did. ---
	const int32 WorldSeed = 13371337;
	FVoxelNoiseParams TempNP, MoistNP, ContNP;
	auto InitClimate = [](FVoxelNoiseParams& P)
	{
		P.NoiseType = EVoxelNoiseType::Simplex;
		P.Octaves = 2;
		P.Persistence = 0.5f;
		P.Lacunarity = 2.0f;
		P.Amplitude = 1.0f;
	};
	InitClimate(TempNP);   TempNP.Seed = WorldSeed + Config->TemperatureSeedOffset;    TempNP.Frequency = Config->TemperatureNoiseFrequency;
	InitClimate(MoistNP);  MoistNP.Seed = WorldSeed + Config->MoistureSeedOffset;      MoistNP.Frequency = Config->MoistureNoiseFrequency;
	InitClimate(ContNP);   ContNP.Seed = WorldSeed + Config->ContinentalnessSeedOffset; ContNP.Frequency = Config->ContinentalnessNoiseFrequency;

	int32 PipelineMismatches = 0;
	FRandomStream Rand(4242);
	for (int32 i = 0; i < 64; ++i)
	{
		const float X = Rand.FRandRange(-200000.0f, 200000.0f);
		const float Y = Rand.FRandRange(-200000.0f, 200000.0f);
		const float Height = Rand.FRandRange(-3000.0f, 6000.0f);
		const bool bWater = (i % 2) == 0;
		const float WaterLevel = 0.0f;

		uint8 SnapMat = 0, SnapBiome = 0;
		FVoxelSurfaceQuery::QuerySurfaceConditions(X, Y, Height, 100.0f,
			Snapshot, WorldSeed, bWater, WaterLevel, SnapMat, SnapBiome);

		// Config-driven reference (identical noise pipeline through the UObject methods)
		const FVector Pos(X, Y, 0.0f);
		const float T = FVoxelCPUNoiseGenerator::FBM3D(Pos, TempNP);
		const float M = FVoxelCPUNoiseGenerator::FBM3D(Pos, MoistNP);
		const float C = FVoxelCPUNoiseGenerator::FBM3D(Pos, ContNP);
		const FBiomeBlend Blend = Config->GetBiomeBlend(T, M, C);
		uint8 RefMat = (bWater && Height < WaterLevel)
			? Config->GetBlendedMaterialWithWater(Blend, 0.0f, Height, WaterLevel)
			: Config->GetBlendedMaterial(Blend, 0.0f);
		RefMat = Config->ApplyHeightMaterialRules(RefMat, Height, 0.0f);
		const uint8 RefBiome = Blend.GetDominantBiome();

		if (SnapMat != RefMat || SnapBiome != RefBiome)
		{
			++PipelineMismatches;
		}
	}
	TestEqual(TEXT("QuerySurfaceConditions(snapshot) matches config-driven reference (mismatches)"), PipelineMismatches, 0);

	// --- 5. Invalid snapshot behaves like the former null-config path ---
	uint8 Mat = 99, Biome = 99;
	FVoxelSurfaceQuery::QuerySurfaceConditions(0.0f, 0.0f, 100.0f, 100.0f,
		FVoxelBiomeSnapshot(), WorldSeed, false, 0.0f, Mat, Biome);
	TestEqual(TEXT("Invalid snapshot yields material 0"), (int32)Mat, 0);
	TestEqual(TEXT("Invalid snapshot yields biome 0"), (int32)Biome, 0);

	Config->RemoveFromRoot();
	return true;
}
