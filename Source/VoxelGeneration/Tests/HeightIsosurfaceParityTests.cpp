// Copyright Daniel Raquel. All Rights Reserved.

// Parity tests for the analytic terrain-height query vs the real generated isosurface.
//
// Background: FInfinitePlaneWorldMode::GetTerrainHeightAt (used for spawn / nav / POI placement) used
// to return the RAW base-noise height, while generation (CPU and GPU) additionally applies
// continentalness height modulation (BaseHeight offset + HeightScale multiplier from the biome config's
// continentalness curves). The analytic height therefore diverged from the real surface, worsening far
// from the origin (measured ~1476 uu at (150k,150k) on the demo config). These tests pin the fix:
// GetTerrainHeightAt now applies the SAME continentalness modulation as generation, so the analytic
// height lands on the real generated voxel surface — including inside conditioning zones.
//
// Mirrors the GT-style parity harness (see the gpu-dc-cpu-parity work): independent oracle
// (real voxel generation) + cross-checks at increasing distances.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "InfinitePlaneWorldMode.h"
#include "IVoxelWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelNoiseTypes.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelData.h"

namespace
{
	// Demo-representative terrain config (matches /Game/PluginTesting/Demo/DemoWorldConfig).
	constexpr int32 kChunkSize = 64;
	constexpr float kVoxelSize = 50.0f;
	constexpr float kHeightScale = 8000.0f;
	constexpr float kBaseHeight = 0.0f;
	constexpr float kSeaLevel = 0.0f;
	constexpr int32 kSeed = 4325;

	FVoxelNoiseParams MakeTerrainNoise()
	{
		FVoxelNoiseParams P;
		P.NoiseType = EVoxelNoiseType::Simplex;
		P.Seed = kSeed;
		P.Frequency = 0.000025f;
		P.Amplitude = 1.0f;
		P.Octaves = 6;
		P.Lacunarity = 2.0f;
		P.Persistence = 0.5f;
		return P;
	}

	FWorldModeTerrainParams MakeBaseParams()
	{
		FWorldModeTerrainParams T(kSeaLevel, kHeightScale, kBaseHeight);
		return T;
	}

	// A biome config with continentalness enabled (default strong curves: offset -3000..+1000,
	// scale 0.2..1.0). Rooted so GC can't reclaim it mid-test; caller must RemoveFromRoot().
	UVoxelBiomeConfiguration* MakeContinentalnessConfig()
	{
		UVoxelBiomeConfiguration* Config = NewObject<UVoxelBiomeConfiguration>();
		Config->AddToRoot();
		Config->bEnableContinentalness = true;
		Config->RebuildBakedCurves();
		return Config;
	}

	// Independent re-implementation of generation's continentalness height math (the oracle for HT1).
	float ReferenceGenerationHeight(
		float X, float Y,
		const FVoxelNoiseParams& NoiseParams,
		const FWorldModeTerrainParams& BaseParams,
		const UVoxelBiomeConfiguration* Config)
	{
		const float NoiseValue = FInfinitePlaneWorldMode::SampleTerrainNoise2D(X, Y, NoiseParams);

		FWorldModeTerrainParams Eff = BaseParams;
		if (Config && Config->bEnableContinentalness)
		{
			FVoxelNoiseParams C;
			C.NoiseType = EVoxelNoiseType::Simplex;
			C.Octaves = 2;
			C.Persistence = 0.5f;
			C.Lacunarity = 2.0f;
			C.Amplitude = 1.0f;
			C.Seed = NoiseParams.Seed + Config->ContinentalnessSeedOffset;
			C.Frequency = Config->ContinentalnessNoiseFrequency;

			const float Cont = FVoxelCPUNoiseGenerator::FBM3D(FVector(X, Y, 0.0f), C);
			float Off = 0.0f, Mult = 1.0f;
			Config->GetContinentalnessTerrainParams(Cont, Off, Mult);
			Eff.BaseHeight += Off;
			Eff.HeightScale *= Mult;
		}
		return FInfinitePlaneWorldMode::NoiseToTerrainHeight(NoiseValue, Eff);
	}

	// Generate a vertical stack of 3 chunks straddling ExpectedZ and return the interpolated real
	// surface Z at world (X,Y), or FLT_MAX if no surface crossing was found in the stack.
	float GenerateRealSurfaceZ(
		FVoxelCPUNoiseGenerator& Generator,
		float X, float Y, float ExpectedZ,
		const UVoxelBiomeConfiguration* Config,
		const TArray<FVoxelConditioningZone>* Zones = nullptr)
	{
		const float ChunkWorldSize = kChunkSize * kVoxelSize;
		const int32 ChunkX = FMath::FloorToInt(X / ChunkWorldSize);
		const int32 ChunkY = FMath::FloorToInt(Y / ChunkWorldSize);
		const int32 CenterChunkZ = FMath::FloorToInt(ExpectedZ / ChunkWorldSize);

		// Local voxel column indices within the chunk for this (X,Y).
		const int32 Lx = FMath::Clamp(FMath::RoundToInt((X - ChunkX * ChunkWorldSize) / kVoxelSize), 0, kChunkSize - 1);
		const int32 Ly = FMath::Clamp(FMath::RoundToInt((Y - ChunkY * ChunkWorldSize) / kVoxelSize), 0, kChunkSize - 1);

		TArray<FVoxelData> Column;
		Column.Reserve(kChunkSize * 3);
		const int32 BaseChunkZ = CenterChunkZ - 1;

		for (int32 Cz = 0; Cz < 3; ++Cz)
		{
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = FIntVector(ChunkX, ChunkY, BaseChunkZ + Cz);
			Request.ChunkSize = kChunkSize;
			Request.VoxelSize = kVoxelSize;
			Request.LODLevel = 0;
			Request.WorldMode = EWorldMode::InfinitePlane;
			Request.SeaLevel = kSeaLevel;
			Request.HeightScale = kHeightScale;
			Request.BaseHeight = kBaseHeight;
			Request.bEnableBiomes = false;      // isolate the height path (continentalness applies regardless)
			Request.bEnableCaves = false;
			Request.BiomeConfiguration = const_cast<UVoxelBiomeConfiguration*>(Config);
			Request.NoiseParams = MakeTerrainNoise();
			if (Zones)
			{
				Request.ConditioningZones = *Zones;
			}

			TArray<FVoxelData> ChunkData;
			if (!Generator.GenerateChunkCPU(Request, ChunkData))
			{
				return FLT_MAX;
			}

			for (int32 Lz = 0; Lz < kChunkSize; ++Lz)
			{
				const int32 Index = Lx + Ly * kChunkSize + Lz * kChunkSize * kChunkSize;
				Column.Add(ChunkData[Index]);
			}
		}

		const float BaseZ = BaseChunkZ * ChunkWorldSize;
		float OutHeight = 0.0f;
		uint8 OutMat = 0, OutBiome = 0;
		if (!FVoxelSurfaceQuery::ExtractSurfaceFromColumn(Column, BaseZ, kVoxelSize, OutHeight, OutMat, OutBiome))
		{
			return FLT_MAX;
		}
		return OutHeight;
	}
}

// ---------------------------------------------------------------------------
// HT1: analytic GetTerrainHeightAt applies continentalness (matches the generation formula), and
// quantifies the pre-fix raw-height gap across distances.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeightContinentalnessParityTest,
	"VoxelWorlds.Generation.HeightParity.Continentalness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHeightContinentalnessParityTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* Config = MakeContinentalnessConfig();
	const FVoxelNoiseParams Noise = MakeTerrainNoise();
	const FWorldModeTerrainParams Base = MakeBaseParams();

	FInfinitePlaneWorldMode Analytic(Base);
	Analytic.SetBiomeContext(Config);

	FInfinitePlaneWorldMode Raw(Base); // no biome context => legacy raw base-noise height

	const float Distances[] = { 0.0f, 10000.0f, 50000.0f, 100000.0f, 150000.0f, 200000.0f };
	float MaxRawGap = 0.0f;

	for (float D : Distances)
	{
		// A couple of directions per distance.
		const FVector2D Points[] = { FVector2D(D, D), FVector2D(D, -D), FVector2D(-D, D * 0.5f) };
		for (const FVector2D& Pt : Points)
		{
			const float A = Analytic.GetTerrainHeightAt(Pt.X, Pt.Y, Noise);
			const float Ref = ReferenceGenerationHeight(Pt.X, Pt.Y, Noise, Base, Config);
			const float RawH = Raw.GetTerrainHeightAt(Pt.X, Pt.Y, Noise);

			// The fixed analytic height must match the generation formula essentially exactly.
			TestTrue(
				FString::Printf(TEXT("Analytic==GenerationFormula at (%.0f,%.0f): |%.3f-%.3f|"), Pt.X, Pt.Y, A, Ref),
				FMath::Abs(A - Ref) < 0.1f);

			MaxRawGap = FMath::Max(MaxRawGap, FMath::Abs(RawH - Ref));
		}

		const float A0 = Analytic.GetTerrainHeightAt(D, D, Noise);
		const float R0 = Raw.GetTerrainHeightAt(D, D, Noise);
		AddInfo(FString::Printf(TEXT("dist=%.0f  analytic=%.1f  raw(legacy)=%.1f  gap=%.1f"),
			D, A0, R0, FMath::Abs(A0 - R0)));
	}

	// The pre-fix (raw) height was off by far more than a voxel somewhere — this WAS the bug.
	AddInfo(FString::Printf(TEXT("Max raw(legacy)-vs-generation gap across samples: %.1f uu (%.2f voxels)"),
		MaxRawGap, MaxRawGap / kVoxelSize));
	TestTrue(TEXT("Legacy raw height diverges > 1 voxel from generation (bug reproduced)"),
		MaxRawGap > kVoxelSize);

	Config->RemoveFromRoot();
	return true;
}

// ---------------------------------------------------------------------------
// HT2: analytic GetTerrainHeightAt lands on the REAL generated voxel surface (independent oracle),
// across distances out to 200k. This is the end-to-end parity guarantee.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeightGeneratedSurfaceParityTest,
	"VoxelWorlds.Generation.HeightParity.GeneratedSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHeightGeneratedSurfaceParityTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* Config = MakeContinentalnessConfig();
	const FVoxelNoiseParams Noise = MakeTerrainNoise();
	const FWorldModeTerrainParams Base = MakeBaseParams();

	FInfinitePlaneWorldMode Analytic(Base);
	Analytic.SetBiomeContext(Config);
	FInfinitePlaneWorldMode Raw(Base);

	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();

	// Voxel-aligned sample points at increasing distance.
	const FVector2D Points[] = {
		FVector2D(0.0f, 0.0f),
		FVector2D(50000.0f, 50000.0f),
		FVector2D(100000.0f, -30000.0f),
		FVector2D(150000.0f, 150000.0f),
		FVector2D(-200000.0f, 80000.0f),
	};

	bool bAnySignificantRawGap = false;

	for (const FVector2D& Pt : Points)
	{
		const float AnalyticH = Analytic.GetTerrainHeightAt(Pt.X, Pt.Y, Noise);
		const float RealZ = GenerateRealSurfaceZ(Generator, Pt.X, Pt.Y, AnalyticH, Config);

		if (!TestTrue(FString::Printf(TEXT("Found a real generated surface near (%.0f,%.0f)"), Pt.X, Pt.Y),
			RealZ != FLT_MAX))
		{
			continue;
		}

		const float RawH = Raw.GetTerrainHeightAt(Pt.X, Pt.Y, Noise);
		AddInfo(FString::Printf(TEXT("(%.0f,%.0f): analytic=%.1f  realSurface=%.1f  |diff|=%.2f voxels ; legacyRaw=%.1f (off by %.1f)"),
			Pt.X, Pt.Y, AnalyticH, RealZ, FMath::Abs(AnalyticH - RealZ) / kVoxelSize, RawH, FMath::Abs(RawH - RealZ)));

		// The FIXED analytic height must sit on the real surface within a voxel.
		TestTrue(
			FString::Printf(TEXT("Analytic height matches real surface at (%.0f,%.0f): |%.1f-%.1f| < 1 voxel"),
				Pt.X, Pt.Y, AnalyticH, RealZ),
			FMath::Abs(AnalyticH - RealZ) < kVoxelSize);

		if (FMath::Abs(RawH - RealZ) > kVoxelSize)
		{
			bAnySignificantRawGap = true;
		}
	}

	// Confirms the fix is load-bearing: the legacy raw height missed the real surface by > 1 voxel.
	TestTrue(TEXT("Legacy raw height missed the real surface by > 1 voxel at some distance"),
		bAnySignificantRawGap);

	Config->RemoveFromRoot();
	return true;
}

// ---------------------------------------------------------------------------
// HT3: inside a conditioning zone, the analytic conditioned height (base+continentalness, then
// FVoxelTerrainConditioning::ApplyToHeight) matches the real generated conditioned surface.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeightConditioningParityTest,
	"VoxelWorlds.Generation.HeightParity.ConditioningZone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHeightConditioningParityTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* Config = MakeContinentalnessConfig();
	const FVoxelNoiseParams Noise = MakeTerrainNoise();
	const FWorldModeTerrainParams Base = MakeBaseParams();

	FInfinitePlaneWorldMode Analytic(Base);
	Analytic.SetBiomeContext(Config);

	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();

	// A flatten zone far from origin (so continentalness is also in play). Target chosen away from the
	// natural height so the flatten is unambiguous.
	const FVector2D Center(150000.0f, 150000.0f);
	const float NaturalAtCenter = Analytic.GetTerrainHeightAt(Center.X, Center.Y, Noise);
	const float Target = NaturalAtCenter + 1500.0f;

	TArray<FVoxelConditioningZone> Zones;
	Zones.Add(FVoxelConditioningZone(Center, 4000.0f, 2000.0f, Target, 1.0f));

	// Analytic conditioned height = base+continentalness, then conditioning blend (what
	// UVoxelChunkManager::GetGeneratedSurfaceHeight composes).
	const float AnalyticConditioned =
		FVoxelTerrainConditioning::ApplyToHeight(Center.X, Center.Y, NaturalAtCenter, Zones);

	// Inside the inner radius the zone fully flattens to Target.
	TestTrue(TEXT("Analytic conditioned height reaches the flatten target at the center"),
		FMath::Abs(AnalyticConditioned - Target) < 0.1f);

	// The real generated conditioned surface at the center must match.
	const float RealZ = GenerateRealSurfaceZ(Generator, Center.X, Center.Y, AnalyticConditioned, Config, &Zones);
	if (TestTrue(TEXT("Found a real conditioned surface at the zone center"), RealZ != FLT_MAX))
	{
		AddInfo(FString::Printf(TEXT("Conditioned center: analytic=%.1f  realSurface=%.1f  target=%.1f  |diff|=%.2f voxels"),
			AnalyticConditioned, RealZ, Target, FMath::Abs(AnalyticConditioned - RealZ) / kVoxelSize));
		TestTrue(TEXT("Analytic conditioned height matches the real conditioned surface within a voxel"),
			FMath::Abs(AnalyticConditioned - RealZ) < kVoxelSize);
	}

	Config->RemoveFromRoot();
	return true;
}

// ---------------------------------------------------------------------------
// HT4: NoiseToTerrainHeight is UNCLAMPED and matches the GPU generator's formula. A former CPU-only
// clamp to +/-10000 clipped peaks the GPU still generated, diverging the analytic height from the real
// surface above ~10000 uu. (CPU generation and the analytic query share this function, so the
// generated-surface tests above can't catch the clamp — only a direct comparison to the GPU formula.)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeightUnclampedParityTest,
	"VoxelWorlds.Generation.HeightParity.Unclamped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHeightUnclampedParityTest::RunTest(const FString& Parameters)
{
	// The GPU generator (Shaders/Private/WorldModeSDF.ush::NoiseToTerrainHeight) computes exactly this,
	// with no clamp:
	auto GpuFormula = [](float N, const FWorldModeTerrainParams& P)
	{
		return P.SeaLevel + P.BaseHeight + N * P.HeightScale;
	};

	// HeightScale 15000 => full-scale noise reaches +/-15000, well past the old +/-10000 CPU clamp.
	const FWorldModeTerrainParams P(/*SeaLevel*/ 0.0f, /*HeightScale*/ 15000.0f, /*BaseHeight*/ 0.0f);

	const float Peak = FInfinitePlaneWorldMode::NoiseToTerrainHeight(1.0f, P);
	const float Valley = FInfinitePlaneWorldMode::NoiseToTerrainHeight(-1.0f, P);

	TestEqual(TEXT("Peak matches the GPU (unclamped) formula"), Peak, GpuFormula(1.0f, P));
	TestEqual(TEXT("Valley matches the GPU (unclamped) formula"), Valley, GpuFormula(-1.0f, P));
	TestTrue(TEXT("Peak exceeds the old +10000 CPU clamp"), Peak > 10000.0f);
	TestTrue(TEXT("Valley is below the old -10000 CPU clamp"), Valley < -10000.0f);

	// BaseHeight + SeaLevel offsets also pass through unclamped (5000 + 8000 = 13000 > 10000).
	const FWorldModeTerrainParams P2(/*SeaLevel*/ 1000.0f, /*HeightScale*/ 8000.0f, /*BaseHeight*/ 4000.0f);
	TestEqual(TEXT("Offset peak matches the GPU (unclamped) formula"),
		FInfinitePlaneWorldMode::NoiseToTerrainHeight(1.0f, P2), GpuFormula(1.0f, P2));

	AddInfo(FString::Printf(TEXT("Unclamped: peak=%.1f valley=%.1f (old clamp would have capped at +/-10000)"), Peak, Valley));
	return true;
}

// ---------------------------------------------------------------------------
// HT5: GetTerrainHeightBounds (the authority for VoxelLOD vertical culling) CONTAINS every terrain
// height the generator can produce — base amplitude AND continentalness. Guards against culling chunks
// that hold real terrain. Also pins the fix to the former inline LOD estimate, whose min side omitted
// the -HeightScale valley depth.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeightBoundsContainmentTest,
	"VoxelWorlds.Generation.HeightParity.Bounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHeightBoundsContainmentTest::RunTest(const FString& Parameters)
{
	const FVoxelNoiseParams Noise = MakeTerrainNoise();
	const FWorldModeTerrainParams Base = MakeBaseParams(); // HeightScale 8000, base 0
	const float Center = Base.SeaLevel + Base.BaseHeight;

	// --- No continentalness: bounds must reach a FULL HeightScale below/above base. The old inline LOD
	// estimate used base - ChunkWorldSize for the min, which sat ~HeightScale above the deepest valley. ---
	{
		float Min = 0.f, Max = 0.f;
		FInfinitePlaneWorldMode::GetTerrainHeightBounds(Base, nullptr, Min, Max);

		TestTrue(TEXT("No-cont min reaches a full HeightScale below base (deep valleys covered)"),
			Min <= Center - Base.HeightScale + 0.1f);
		TestTrue(TEXT("No-cont max reaches a full HeightScale above base"),
			Max >= Center + Base.HeightScale - 0.1f);

		FInfinitePlaneWorldMode WM(Base); // no biome context => raw base noise
		float SMin = FLT_MAX, SMax = -FLT_MAX;
		for (int32 i = -60; i <= 60; ++i)
		{
			for (int32 j = -60; j <= 60; ++j)
			{
				const float H = WM.GetTerrainHeightAt(i * 2000.0f, j * 2000.0f, Noise);
				SMin = FMath::Min(SMin, H);
				SMax = FMath::Max(SMax, H);
			}
		}
		AddInfo(FString::Printf(TEXT("No-cont: bounds [%.0f, %.0f] contain sampled [%.0f, %.0f]"), Min, Max, SMin, SMax));
		TestTrue(TEXT("All sampled no-cont heights are within the bounds"), SMin >= Min && SMax <= Max);
	}

	// --- With continentalness: bounds must contain the modulated heights over a wide grid. ---
	{
		UVoxelBiomeConfiguration* Config = MakeContinentalnessConfig();
		float Min = 0.f, Max = 0.f;
		FInfinitePlaneWorldMode::GetTerrainHeightBounds(Base, Config, Min, Max);

		FInfinitePlaneWorldMode WM(Base);
		WM.SetBiomeContext(Config);
		float SMin = FLT_MAX, SMax = -FLT_MAX;
		for (int32 i = -80; i <= 80; ++i)
		{
			for (int32 j = -80; j <= 80; ++j)
			{
				const float H = WM.GetTerrainHeightAt(i * 2500.0f, j * 2500.0f, Noise);
				SMin = FMath::Min(SMin, H);
				SMax = FMath::Max(SMax, H);
			}
		}
		AddInfo(FString::Printf(TEXT("Cont: bounds [%.0f, %.0f] contain sampled [%.0f, %.0f]"), Min, Max, SMin, SMax));
		TestTrue(TEXT("All sampled continentalness heights are within the bounds"), SMin >= Min && SMax <= Max);

		Config->RemoveFromRoot();
	}

	return true;
}
