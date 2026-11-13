// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
#include "VoxelSeaSurface.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelChunkLineSegments.h"
#include "RenderingThread.h"
#include "RHI.h"             // GUsingNullRHI: GPU tests skip under -nullrhi

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCPUNoiseGeneratorTest, "VoxelWorlds.Generation.CPUNoiseGenerator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCPUNoiseGeneratorTest::RunTest(const FString& Parameters)
{
	// Create CPU generator
	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();

	TestTrue(TEXT("Generator should be initialized"), Generator.IsInitialized());

	// Create generation request
	// Use a frequency that doesn't produce integer coordinates when scaled
	// With VoxelSize=100 and Frequency=0.1, positions become multiples of 10 (integers)
	// Perlin noise returns 0 on integer grid corners, so use 0.013 instead
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(0, 0, 0);
	Request.ChunkSize = 16; // Use smaller chunk for faster test
	Request.VoxelSize = 100.0f;
	Request.LODLevel = 0;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Perlin;
	Request.NoiseParams.Seed = 12345;
	Request.NoiseParams.Frequency = 0.013f; // Avoids integer grid positions
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;

	// Generate chunk data
	TArray<FVoxelData> VoxelData;
	bool bSuccess = Generator.GenerateChunkCPU(Request, VoxelData);

	TestTrue(TEXT("CPU generation should succeed"), bSuccess);

	// Verify output size
	int32 ExpectedSize = Request.ChunkSize * Request.ChunkSize * Request.ChunkSize;
	TestEqual(TEXT("Output should have correct number of voxels"), VoxelData.Num(), ExpectedSize);

	// Verify data has variation (noise should produce different density values)
	int32 NonZeroDensityCount = 0;
	int32 SolidVoxelCount = 0;
	int32 AirVoxelCount = 0;
	TSet<uint8> UniqueDensities;

	for (const FVoxelData& Voxel : VoxelData)
	{
		UniqueDensities.Add(Voxel.Density);
		if (Voxel.Density > 0)
		{
			NonZeroDensityCount++;
		}
		if (Voxel.Density >= 127)
		{
			SolidVoxelCount++;
		}
		else
		{
			AirVoxelCount++;
		}
	}

	TestTrue(TEXT("Should have some non-zero density voxels"), NonZeroDensityCount > 0);
	// With higher frequency, we should get good variation. Check for multiple unique density values.
	TestTrue(TEXT("Should have density variation (multiple unique values)"), UniqueDensities.Num() > 10);

	AddInfo(FString::Printf(TEXT("Density stats: %d solid, %d air, %d unique densities"),
		SolidVoxelCount, AirVoxelCount, UniqueDensities.Num()));

	// Test single point sampling
	// Use positions that won't land on integer grid corners after frequency scaling
	FVector TestPosition(537.0f, 523.0f, 117.0f);
	float NoiseValue = Generator.SampleNoiseAt(TestPosition, Request.NoiseParams);
	TestTrue(TEXT("Noise value should be in valid range"), NoiseValue >= -1.0f && NoiseValue <= 1.0f);

	// Test determinism - same seed should produce same results
	TArray<FVoxelData> VoxelData2;
	bool bSuccess2 = Generator.GenerateChunkCPU(Request, VoxelData2);
	TestTrue(TEXT("Second generation should succeed"), bSuccess2);
	TestEqual(TEXT("Same request should produce same results"), VoxelData[0].Density, VoxelData2[0].Density);

	// Test different seeds produce different noise values
	// Compare raw noise samples rather than density (which may round to same value)
	// Use positions that won't land on integer grid corners after frequency scaling
	FVector TestPos1(137.5f, 243.7f, 318.2f);
	FVector TestPos2(512.3f, 627.8f, 741.1f);

	float Noise1Seed1 = Generator.SampleNoiseAt(TestPos1, Request.NoiseParams);
	float Noise2Seed1 = Generator.SampleNoiseAt(TestPos2, Request.NoiseParams);

	FVoxelNoiseParams DifferentSeedParams = Request.NoiseParams;
	DifferentSeedParams.Seed = 54321;

	float Noise1Seed2 = Generator.SampleNoiseAt(TestPos1, DifferentSeedParams);
	float Noise2Seed2 = Generator.SampleNoiseAt(TestPos2, DifferentSeedParams);

	// Different seeds should produce different raw noise values
	bool bDifferentNoise = !FMath::IsNearlyEqual(Noise1Seed1, Noise1Seed2, 0.001f) ||
	                       !FMath::IsNearlyEqual(Noise2Seed1, Noise2Seed2, 0.001f);

	AddInfo(FString::Printf(TEXT("Noise seed test: Seed1=[%.4f, %.4f] Seed2=[%.4f, %.4f]"),
		Noise1Seed1, Noise2Seed1, Noise1Seed2, Noise2Seed2));

	TestTrue(TEXT("Different seeds should produce different noise values"), bDifferentNoise);

	Generator.Shutdown();
	TestFalse(TEXT("Generator should not be initialized after shutdown"), Generator.IsInitialized());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUNoiseGeneratorAsyncTest, "VoxelWorlds.Generation.GPUNoiseGeneratorAsync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUNoiseGeneratorAsyncTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	// Create GPU generator
	FVoxelGPUNoiseGenerator Generator;
	Generator.Initialize();

	TestTrue(TEXT("GPU Generator should be initialized"), Generator.IsInitialized());

	// Create generation request with frequency that avoids integer grid positions
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(0, 0, 0);
	Request.ChunkSize = 16; // Use smaller chunk for faster test
	Request.VoxelSize = 100.0f;
	Request.LODLevel = 0;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Perlin;
	Request.NoiseParams.Seed = 12345;
	Request.NoiseParams.Frequency = 0.013f; // Avoids integer grid positions
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;

	// Track completion
	volatile bool bCompleted = false;
	volatile bool bSucceeded = false;
	FVoxelGenerationHandle ResultHandle;

	// Generate chunk asynchronously
	FVoxelGenerationHandle Handle = Generator.GenerateChunkAsync(Request,
		FOnVoxelGenerationComplete::CreateLambda([&bCompleted, &bSucceeded, &ResultHandle](FVoxelGenerationHandle InHandle, bool bSuccess)
		{
			ResultHandle = InHandle;
			bSucceeded = bSuccess;
			bCompleted = true;
		}));

	TestTrue(TEXT("Handle should be valid"), Handle.IsValid());

	// Wait for completion (with timeout)
	double StartTime = FPlatformTime::Seconds();
	double TimeoutSeconds = 5.0;

	while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < TimeoutSeconds)
	{
		FPlatformProcess::Sleep(0.01f);
		// Tick the render thread to process commands
		FlushRenderingCommands();
	}

	TestTrue(TEXT("Generation should complete within timeout"), bCompleted);
	TestTrue(TEXT("Generation should succeed"), bSucceeded);

	if (bCompleted && bSucceeded)
	{
		// Test readback
		TArray<FVoxelData> VoxelData;
		bool bReadbackSuccess = Generator.ReadbackToCPU(Handle, VoxelData);

		TestTrue(TEXT("Readback should succeed"), bReadbackSuccess);

		// Verify output size
		int32 ExpectedSize = Request.ChunkSize * Request.ChunkSize * Request.ChunkSize;
		TestEqual(TEXT("Output should have correct number of voxels"), VoxelData.Num(), ExpectedSize);

		// Verify data has variation (noise should produce variation)
		int32 NonZeroDensityCount = 0;
		for (const FVoxelData& Voxel : VoxelData)
		{
			if (Voxel.Density > 0)
			{
				NonZeroDensityCount++;
			}
		}
		TestTrue(TEXT("Should have some non-zero density voxels from GPU"), NonZeroDensityCount > 0);
	}

	// Release handle
	Generator.ReleaseHandle(Handle);

	Generator.Shutdown();

	return true;
}

// ==================== Shared GPU==CPU parity harness (Phase D) ====================
//
// The strict parity tests (InfinitePlane, IslandBowl, caves, noise types, water, ore) all follow the same
// shape: for each LOD, generate the same request on CPU and GPU, then compare the full FVoxelData.
// The GPU path runs the water/underground post-passes as compute shaders inside the generation graph
// (AddVoxelPostPassDispatches), the CPU path runs them inside GenerateChunkCPU — so this comparison
// validates the shader ports bit-for-bit. Each scenario is just "build a Request + call RunGpuCpuParity".
namespace VoxelParityHarness
{
	/** Per-channel pass thresholds (percent). Defaults match the Phase B/C parity acceptance. */
	struct FParityThresholds
	{
		float DensityClose = 99.0f;    // |ΔDensity| <= 2 (FP-boundary tolerant)
		float Mat = 95.0f;             // material exact (boundary voxels may frac-dither)
		float Biome = 98.0f;           // biome exact
		float Meta = 98.0f;            // metadata exact (cave/underground/water flags)
		float MatWhenDensEq = 99.0f;   // material exact among density-identical voxels (isolates logic from FP)
		bool bCheckMeta = true;        // some scenarios opt out of the metadata gate
		int32 WatchMaterialID = -1;    // >= 0: also require agreement on the voxels where either side has this material
		float WatchMaterialMatch = 0.0f; // ... at least this share of them (percent)
	};

	// Runs GPU-vs-CPU parity for a configured request across the given LODs. Emits per-LOD stats via AddInfo
	// and per-channel failures via AddError. Returns true iff every channel passed at every LOD.
	// Request is taken by value: the helper owns LODLevel across the sweep.
	static bool RunGpuCpuParity(
		FAutomationTestBase& Test,
		FVoxelGPUNoiseGenerator& Generator,
		FVoxelNoiseGenerationRequest Request,
		const TCHAR* Label,
		const FParityThresholds& Thr = FParityThresholds(),
		const TArray<int32>& LODs = { 0, 1, 2 })
	{
		bool bAllPass = true;
		for (int32 LOD : LODs)
		{
			Request.LODLevel = LOD;

			TArray<FVoxelData> CPUData;
			Generator.GenerateChunkCPU(Request, CPUData); // CPU fallback: includes water + underground passes

			volatile bool bCompleted = false;
			FVoxelGenerationHandle Handle = Generator.GenerateChunkAsync(Request,
				FOnVoxelGenerationComplete::CreateLambda([&bCompleted](FVoxelGenerationHandle, bool) { bCompleted = true; }));
			const double StartTime = FPlatformTime::Seconds();
			while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < 10.0)
			{
				FPlatformProcess::Sleep(0.01f);
				FlushRenderingCommands();
			}
			TArray<FVoxelData> GPUData;
			Generator.ReadbackToCPU(Handle, GPUData);
			Generator.ReleaseHandle(Handle);

			// No CPU post-passes here: the GPU graph already ran them (AddVoxelPostPassDispatches),
			// so GPUData arrives finished and the comparison validates the shader ports.

			if (CPUData.Num() != GPUData.Num() || CPUData.Num() == 0)
			{
				Test.AddError(FString::Printf(TEXT("%s LOD%d: size mismatch/empty CPU=%d GPU=%d"), Label, LOD, CPUData.Num(), GPUData.Num()));
				bAllPass = false;
				continue;
			}

			const int32 Total = CPUData.Num();
			int32 DensityClose = 0, MatMatch = 0, BiomeMatch = 0, MetaMatch = 0;
			int32 DensityEqualCount = 0, MatMatchWhenDensEqual = 0;
			int32 WatchedEither = 0, WatchedBoth = 0;
			int32 Reported = 0;
			for (int32 i = 0; i < Total; ++i)
			{
				const FVoxelData& C = CPUData[i];
				const FVoxelData& G = GPUData[i];
				if (FMath::Abs((int32)C.Density - (int32)G.Density) <= 2) { DensityClose++; }
				if (C.MaterialID == G.MaterialID) { MatMatch++; }
				if (C.BiomeID == G.BiomeID) { BiomeMatch++; }
				if (C.Metadata == G.Metadata) { MetaMatch++; }
				if (Thr.WatchMaterialID >= 0 && (C.MaterialID == Thr.WatchMaterialID || G.MaterialID == Thr.WatchMaterialID))
				{
					WatchedEither++;
					WatchedBoth += (C.MaterialID == G.MaterialID) ? 1 : 0;
				}
				if (C.Density == G.Density)
				{
					DensityEqualCount++;
					if (C.MaterialID == G.MaterialID) { MatMatchWhenDensEqual++; }
				}
				if ((C.MaterialID != G.MaterialID || C.BiomeID != G.BiomeID || C.Metadata != G.Metadata) && Reported < 8)
				{
					Test.AddInfo(FString::Printf(TEXT("%s LOD%d mismatch @%d: dens C=%d G=%d, mat C=%d G=%d, biome C=%d G=%d, meta C=%d G=%d"),
						Label, LOD, i, C.Density, G.Density, C.MaterialID, G.MaterialID, C.BiomeID, G.BiomeID, C.Metadata, G.Metadata));
					Reported++;
				}
			}

			const float DensityClosePct = 100.0f * DensityClose / Total;
			const float MatPct = 100.0f * MatMatch / Total;
			const float BiomePct = 100.0f * BiomeMatch / Total;
			const float MetaPct = 100.0f * MetaMatch / Total;
			const float MatWhenDensEqPct = DensityEqualCount > 0 ? 100.0f * MatMatchWhenDensEqual / DensityEqualCount : 100.0f;

			// A CRC of the GPU output, so a change to the shader that must not move a heightmap's voxels can be checked
			// bit for bit against the previous build's log (spherical Phase 2 refactored the heightmap branch's helpers).
			const uint32 GPUCrc = FCrc::MemCrc32(GPUData.GetData(), GPUData.Num() * sizeof(FVoxelData));
			Test.AddInfo(FString::Printf(TEXT("%s LOD%d [%d voxels]: density~%.1f%%, material %.1f%%, biome %.1f%%, meta %.1f%%, material(density==) %.2f%% (density %d (%d by more than 2), material %d, biome %d, meta %d differ; GPU crc %08x)"),
				Label, LOD, Total, DensityClosePct, MatPct, BiomePct, MetaPct, MatWhenDensEqPct,
				Total - DensityEqualCount, Total - DensityClose, Total - MatMatch, Total - BiomeMatch, Total - MetaMatch, GPUCrc));

			if (DensityClosePct < Thr.DensityClose) { Test.AddError(FString::Printf(TEXT("%s LOD%d density parity %.1f%% < %.1f%%"), Label, LOD, DensityClosePct, Thr.DensityClose)); bAllPass = false; }
			if (MatPct < Thr.Mat) { Test.AddError(FString::Printf(TEXT("%s LOD%d material parity %.1f%% < %.1f%%"), Label, LOD, MatPct, Thr.Mat)); bAllPass = false; }
			if (BiomePct < Thr.Biome) { Test.AddError(FString::Printf(TEXT("%s LOD%d biome parity %.1f%% < %.1f%%"), Label, LOD, BiomePct, Thr.Biome)); bAllPass = false; }
			if (Thr.bCheckMeta && MetaPct < Thr.Meta) { Test.AddError(FString::Printf(TEXT("%s LOD%d metadata parity %.1f%% < %.1f%%"), Label, LOD, MetaPct, Thr.Meta)); bAllPass = false; }
			if (MatWhenDensEqPct < Thr.MatWhenDensEq) { Test.AddError(FString::Printf(TEXT("%s LOD%d material(density==) %.2f%% < %.2f%%"), Label, LOD, MatWhenDensEqPct, Thr.MatWhenDensEq)); bAllPass = false; }
			if (Thr.WatchMaterialID >= 0)
			{
				const float WatchedPct = WatchedEither > 0 ? 100.0f * WatchedBoth / WatchedEither : 100.0f;
				Test.AddInfo(FString::Printf(TEXT("%s LOD%d material %d: %d voxels on either side, %.2f%% on both"), Label, LOD, Thr.WatchMaterialID, WatchedEither, WatchedPct));
				if (WatchedPct < Thr.WatchMaterialMatch) { Test.AddError(FString::Printf(TEXT("%s LOD%d material %d agreement %.2f%% < %.2f%%"), Label, LOD, Thr.WatchMaterialID, WatchedPct, Thr.WatchMaterialMatch)); bAllPass = false; }
			}
		}
		return bAllPass;
	}
} // namespace VoxelParityHarness

// Phase B: strict GPU==CPU parity for the FULL FVoxelData (density + material + biome + metadata) with a
// biome configuration (biomes + height rule + ore vein) + a conditioning zone, at LOD 0/1/2. Runs the CPU
// post-passes on the GPU readback (as the streaming path does) so water/underground metadata is comparable.
// The LOD>0 cases guard the Phase A LOD-stride regression (generation must be LOD-independent).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUMaterialParityTest, "VoxelWorlds.Generation.GPUvsCPUMaterialParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUMaterialParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults(); // Plains / Forest / Mountain / Ocean

	// Height rule: exercise ApplyHeightMaterialRules (snow-like override above a height, on the surface).
	BiomeConfig->bEnableHeightMaterials = true;
	BiomeConfig->HeightMaterialRules.Empty();
	{
		FHeightMaterialRule Rule;
		Rule.MinHeight = 2500.0f; Rule.MaxHeight = MAX_FLT; Rule.MaterialID = 6;
		Rule.bSurfaceOnly = true; Rule.MaxDepthBelowSurface = 2.0f; Rule.Priority = 10;
		BiomeConfig->HeightMaterialRules.Add(Rule);
	}

	// Global ore vein (Rarity 1.0 so the precision-sensitive rarity hash is not exercised).
	BiomeConfig->bEnableOreVeins = true;
	BiomeConfig->GlobalOreVeins.Empty();
	{
		FOreVeinConfig Ore;
		Ore.Name = TEXT("Iron"); Ore.MaterialID = 8; Ore.MinDepth = 12.0f; Ore.MaxDepth = 0.0f;
		Ore.Shape = EOreVeinShape::Blob; Ore.Frequency = 0.02f; Ore.Threshold = 0.55f;
		Ore.SeedOffset = 111; Ore.Rarity = 1.0f; Ore.Priority = 0;
		BiomeConfig->GlobalOreVeins.Add(Ore);
	}
	BiomeConfig->BuildGpuData(); // bake GPU arrays (also triggers the sort/index cache rebuilds it needs)

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(3, 1, 0);
	Request.ChunkSize = 16;
	Request.VoxelSize = 100.0f;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Request.NoiseParams.Seed = 4325;
	Request.NoiseParams.Frequency = 0.0025f;
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 4000.0f;
	Request.BaseHeight = 0.0f;
	Request.bEnableBiomes = true;
	Request.BiomeConfiguration = BiomeConfig;
	// Conditioning zone overlapping the chunk (exercise the GPU height-flatten before the SDF).
	Request.Conditioning.Zones.Add(FVoxelConditioningZone(FVector2D(4800.0, 1600.0), 800.0f, 1200.0f, 500.0f, 1.0f));

	// Density near-identical (Phase A). Material/biome/meta exact except at density boundaries; when the
	// density is identical, material must match almost exactly (isolates logic errors from FP boundaries).
	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("InfinitePlane"));

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU material/biome/metadata parity across LOD 0/1/2"), bAllPass);
	return true;
}

// G39 (WORLD_MODES_REVIEW_PLAN.md §14.1): the blend zones' dither on the GPU (GpuBiomeBlendDither, GpuBlendedMaterial in
// BiomeMaterial.ush) equals the CPU's (FVoxelBiomeBlendDither, FVoxelBiomeSnapshot::PickBlendIndex). The default biomes
// with a wide blend (BiomeBlendWidth 0.5), so much of the chunk is a blend zone, on a grid whose origin is off the voxel
// grid, at LOD 0/1/2.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUBlendDitherParityTest, "VoxelWorlds.Generation.GPUvsCPUBlendDitherParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUBlendDitherParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults(); // Plains / Forest / Mountain / Ocean
	BiomeConfig->BiomeBlendWidth = 0.5f;
	BiomeConfig->BuildGpuData();

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(-2, 5, 0);
	Request.ChunkSize = 32;
	Request.VoxelSize = 75.0f;
	Request.WorldOrigin = FVector(12345.0, 54321.0, -2187.5);
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Request.NoiseParams.Seed = 9177;
	Request.NoiseParams.Frequency = 0.0004f;
	Request.NoiseParams.Octaves = 3;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 800.0f;
	Request.BaseHeight = 0.0f;
	Request.bEnableBiomes = true;
	Request.BiomeConfiguration = BiomeConfig;
	// The chunk layer holding the surface (terrain within +-800 of Z 0)
	Request.ChunkCoord.Z = FMath::FloorToInt32((0.0 - Request.WorldOrigin.Z) / (Request.ChunkSize * Request.VoxelSize));

	// The chunk lies in a blend zone: two or more biomes' materials among its solid voxels (non-vacuous).
	TArray<FVoxelData> Cpu;
	GPUGenerator.GenerateChunkCPU(Request, Cpu);
	TSet<uint8> Materials;
	int32 Solid = 0;
	for (const FVoxelData& Voxel : Cpu)
	{
		if (Voxel.IsSolid())
		{
			++Solid;
			Materials.Add(Voxel.MaterialID);
		}
	}
	AddInfo(FString::Printf(TEXT("CPU, %d voxels (%d solid), %d materials"), Cpu.Num(), Solid, Materials.Num()));
	TestTrue(TEXT("The chunk holds several materials (non-vacuous)"), Materials.Num() >= 2);

	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("BlendDither"));

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU material parity in a blend zone, LOD 0/1/2"), bAllPass);
	return true;
}

// Phase C: GPU==CPU parity for IslandBowl mode (continentalness-modulated height + edge falloff), with
// biomes, at LOD 0/1/2. Verifies the shared heightmap path + the island falloff on the GPU match CPU.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUIslandBowlParityTest, "VoxelWorlds.Generation.GPUvsCPUIslandBowlParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUIslandBowlParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();

	// Enable continentalness (internal oceans/mountains) so we exercise the continentalness->height +
	// island-falloff composition. Give it non-trivial curves so the modulation is meaningful.
	BiomeConfig->bEnableContinentalness = true;
	if (FRichCurve* HC = BiomeConfig->ContinentalnessHeightCurve.GetRichCurve())
	{
		HC->Reset(); HC->AddKey(-1.0f, -2000.0f); HC->AddKey(0.0f, 0.0f); HC->AddKey(1.0f, 1000.0f);
	}
	if (FRichCurve* SC = BiomeConfig->ContinentalnessHeightScaleCurve.GetRichCurve())
	{
		SC->Reset(); SC->AddKey(-1.0f, 0.3f); SC->AddKey(1.0f, 1.0f);
	}
	BiomeConfig->RebuildBakedCurves();
	BiomeConfig->BuildGpuData();

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(2, 0, 0);
	Request.ChunkSize = 16;
	Request.VoxelSize = 100.0f;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Request.NoiseParams.Seed = 4325;
	Request.NoiseParams.Frequency = 0.0025f;
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;
	Request.WorldMode = EWorldMode::IslandBowl;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 3000.0f;
	Request.BaseHeight = 0.0f;
	Request.bEnableBiomes = true;
	Request.BiomeConfiguration = BiomeConfig;
	// Circular island whose falloff edge (radius 3500 + falloff 1500) crosses the test chunk (world ~3200-4800).
	Request.IslandParams.Shape = EIslandShape::Circular;
	Request.IslandParams.IslandRadius = 3500.0f;
	Request.IslandParams.SizeY = 3500.0f;
	Request.IslandParams.FalloffWidth = 1500.0f;
	Request.IslandParams.FalloffType = EIslandFalloffType::Smooth;
	Request.IslandParams.CenterX = 0.0f;
	Request.IslandParams.CenterY = 0.0f;
	Request.IslandParams.EdgeHeight = -1000.0f;

	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("IslandBowl"));

	// The Exponential curve (rescaled to reach 0 at the edge, KI4) has its own branch in IslandFalloff.ush.
	Request.IslandParams.FalloffType = EIslandFalloffType::Exponential;
	const bool bExponentialPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("IslandBowl Exponential"));

	// The seabed beyond the edge (WM-E1, IslandSeabedHeight in IslandFalloff.ush): a chunk past R + F (x 4,800-6,400,
	// 0-1,400 uu past the edge for y 0-1,600) at z -1,600 to 0, where the seabed slopes from EdgeHeight -1,000 toward
	// -2,200, with the slope ending inside the chunk. Then the rectangle's larger-axis distance.
	Request.IslandParams.FalloffType = EIslandFalloffType::Smooth;
	Request.IslandParams.SeabedDepth = 1200.0f;
	Request.IslandParams.SeabedSlopeWidth = 1000.0f;
	Request.ChunkCoord = FIntVector(3, 0, -1);
	const bool bSeabedPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("IslandBowl seabed"));
	Request.IslandParams.Shape = EIslandShape::Rectangle;
	Request.IslandParams.SizeY = 2000.0f;
	Request.ChunkCoord = FIntVector(3, 1, -1);
	const bool bSeabedRectPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("IslandBowl seabed rectangle"));

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU IslandBowl parity (continentalness + falloff) across LOD 0/1/2"), bAllPass);
	TestTrue(TEXT("GPU==CPU IslandBowl parity with Exponential falloff across LOD 0/1/2"), bExponentialPass);
	TestTrue(TEXT("GPU==CPU IslandBowl parity on the seabed beyond the edge across LOD 0/1/2"), bSeabedPass);
	TestTrue(TEXT("GPU==CPU IslandBowl parity on a rectangle's seabed across LOD 0/1/2"), bSeabedRectPass);
	return true;
}

// Phase D + G18: GPU==CPU parity for cave carving (cheese + tunnel layers + cave-wall material override), at
// LOD 0/1/2: the core carving (no biome overrides, no water), the cave configuration's biome overrides (a cave
// scale above 1 with a minimum-depth override, a scale of 0, a scale of 0.5), and a chunk under the sea with the
// underwater minimum depth. Verifies the GPU cave block (GpuCaveCarve in GenerateVoxelDensity.usf + CaveGeneration.ush)
// matches the CPU's CalculateCaveDensity/SampleCaveLayer path in the heightmap branch, overrides and underwater
// suppression included (G18, WORLD_MODES_REVIEW_PLAN.md §14.1).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUCavesParityTest, "VoxelWorlds.Generation.GPUvsCPUCavesParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUCavesParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->BuildGpuData();

	// Global cave config: one cheese + one tunnel (spaghetti) layer, cave-wall override on. First no biome
	// overrides and no underwater minimum depth: the core carving path.
	UVoxelCaveConfiguration* CaveConfig = NewObject<UVoxelCaveConfiguration>();
	CaveConfig->AddToRoot();
	CaveConfig->bEnableCaves = true;
	CaveConfig->BiomeOverrides.Empty();     // GetBiomeCaveScale() == 1.0, GetBiomeMinDepthOverride() == -1
	CaveConfig->UnderwaterMinDepth = 0.0f;
	CaveConfig->bOverrideCaveWallMaterial = true;
	CaveConfig->CaveWallMaterialID = 2;
	CaveConfig->CaveWallMaterialMinDepth = 8.0f;
	CaveConfig->CaveLayers.Empty();
	{
		FCaveLayerConfig Cheese;
		Cheese.bEnabled = true;
		Cheese.CaveType = ECaveType::Cheese;
		Cheese.SeedOffset = 1234;
		Cheese.Frequency = 0.010f; Cheese.Octaves = 3; Cheese.Persistence = 0.5f; Cheese.Lacunarity = 2.0f;
		Cheese.Threshold = 0.30f; Cheese.CarveStrength = 1.0f; Cheese.CarveFalloff = 0.25f;
		Cheese.MinDepth = 4.0f; Cheese.MaxDepth = 0.0f; Cheese.DepthFadeWidth = 4.0f; Cheese.VerticalScale = 0.6f;
		CaveConfig->CaveLayers.Add(Cheese);

		FCaveLayerConfig Tunnel;
		Tunnel.bEnabled = true;
		Tunnel.CaveType = ECaveType::Spaghetti;
		Tunnel.SeedOffset = 5678;
		Tunnel.Frequency = 0.012f; Tunnel.Octaves = 3; Tunnel.Persistence = 0.5f; Tunnel.Lacunarity = 2.0f;
		Tunnel.Threshold = 0.30f; Tunnel.CarveStrength = 1.0f; Tunnel.CarveFalloff = 0.20f;
		Tunnel.MinDepth = 4.0f; Tunnel.MaxDepth = 0.0f; Tunnel.DepthFadeWidth = 4.0f; Tunnel.VerticalScale = 0.7f;
		Tunnel.SecondNoiseSeedOffset = 7777; Tunnel.SecondNoiseFrequencyScale = 1.2f;
		CaveConfig->CaveLayers.Add(Tunnel);
	}

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(3, 1, 0); // same chunk as the InfinitePlane material test (has real terrain)
	Request.ChunkSize = 16;
	Request.VoxelSize = 100.0f;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Request.NoiseParams.Seed = 4325;
	Request.NoiseParams.Frequency = 0.0025f;
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 4000.0f;
	Request.BaseHeight = 0.0f;
	Request.bEnableBiomes = true;
	Request.BiomeConfiguration = BiomeConfig;
	Request.bEnableCaves = true;
	Request.CaveConfiguration = CaveConfig;
	Request.bEnableWaterLevel = false; // terrain above water → bUnderwater=false (no underwater suppression)

	// Guard against a vacuous pass: confirm caves actually carved voxels. The CAVE flag is temporary
	// (cleared by the water-fill pass), so count the persistent UNDERGROUND flag on air voxels — in a
	// terrain chunk, underground air comes from cave carving.
	auto GenerateProbe = [&GPUGenerator](const FVoxelNoiseGenerationRequest& InRequest)
	{
		FVoxelNoiseGenerationRequest Probe = InRequest; Probe.LODLevel = 0;
		TArray<FVoxelData> ProbeData;
		GPUGenerator.GenerateChunkCPU(Probe, ProbeData);
		return ProbeData;
	};
	auto CountCaveAir = [&GenerateProbe](const FVoxelNoiseGenerationRequest& InRequest)
	{
		int32 UndergroundAir = 0;
		for (const FVoxelData& V : GenerateProbe(InRequest))
		{
			if (V.IsAir() && V.HasUndergroundFlag()) { UndergroundAir++; }
		}
		return UndergroundAir;
	};
	const int32 CoreCaveAir = CountCaveAir(Request);
	AddInfo(FString::Printf(TEXT("Caves probe: %d underground-air (cave) voxels of %d"), CoreCaveAir, Request.ChunkSize * Request.ChunkSize * Request.ChunkSize));
	TestTrue(TEXT("Cave config should carve some voxels (non-vacuous test)"), CoreCaveAir > 0);

	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Caves"));

	// The biome overrides. The chunk's biome: with continentalness off every column samples continentalness 0, where
	// Ocean (range ending at -0.05, inside the blend width, and priority 10) wins the blend, so the chunk is Ocean. Each
	// override set gives the chunk's biome its values, every other default biome different ones, and then a second entry
	// for the chunk's biome that the lookup must skip (the first match wins), so a GPU that read the wrong entry would
	// carve differently.
	uint8 ChunkBiome = 0;
	{
		TMap<uint8, int32> BiomeVotes;
		for (const FVoxelData& V : GenerateProbe(Request))
		{
			++BiomeVotes.FindOrAdd(V.BiomeID);
		}
		for (const TPair<uint8, int32>& Vote : BiomeVotes)
		{
			if (Vote.Value > BiomeVotes.FindRef(ChunkBiome)) { ChunkBiome = Vote.Key; }
		}
	}
	auto SetOverrides = [CaveConfig, ChunkBiome](float CaveScale, float MinDepthOverride)
	{
		auto AddOverride = [CaveConfig](uint8 BiomeID, float Scale, float MinDepth)
		{
			FBiomeCaveOverride Override;
			Override.BiomeID = BiomeID; Override.CaveScale = Scale; Override.MinDepthOverride = MinDepth;
			CaveConfig->BiomeOverrides.Add(Override);
		};
		const float OtherScale = (CaveScale > 0.0f) ? 0.0f : 1.5f;
		CaveConfig->BiomeOverrides.Empty();
		for (uint8 BiomeID = 0; BiomeID < 4; ++BiomeID) // Plains, Forest, Mountain, Ocean
		{
			AddOverride(BiomeID, (BiomeID == ChunkBiome) ? CaveScale : OtherScale, (BiomeID == ChunkBiome) ? MinDepthOverride : -1.0f);
		}
		AddOverride(ChunkBiome, OtherScale, -1.0f); // never read
	};

	// More caves from a different depth: a scale above 1 and a minimum-depth override.
	SetOverrides(1.25f, 8.0f);
	const int32 OverrideCaveAir = CountCaveAir(Request);
	AddInfo(FString::Printf(TEXT("Biome overrides probe: chunk biome %d, %d underground-air voxels with scale 1.25 and minimum depth 8, %d without"), ChunkBiome, OverrideCaveAir, CoreCaveAir));
	TestTrue(TEXT("The biome overrides change the carving in the chunk (non-vacuous)"), OverrideCaveAir > 0 && OverrideCaveAir != CoreCaveAir);
	const bool bOverridesPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Caves biome overrides"));

	// No caves in the biome (the demo's Ocean, CaveScale 0).
	SetOverrides(0.0f, -1.0f);
	const int32 NoCaveAir = CountCaveAir(Request);
	AddInfo(FString::Printf(TEXT("Biome scale 0 probe: %d underground-air voxels"), NoCaveAir));
	TestTrue(TEXT("A biome cave scale of 0 carves nothing where the core config carves (non-vacuous)"), NoCaveAir == 0 && CoreCaveAir > 0);
	const bool bZeroScalePass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Caves biome scale 0"));

	// A scale below 1 caps the carve (half of a full carve leaves a fully solid voxel at 127, still solid), so it mostly
	// softens the density and widens the cave-wall band: the scale and its clamp must match.
	SetOverrides(0.5f, -1.0f);
	{
		FVoxelNoiseGenerationRequest NoCaves = Request;
		NoCaves.bEnableCaves = false;
		const TArray<FVoxelData> Uncarved = GenerateProbe(NoCaves);
		const TArray<FVoxelData> Softened = GenerateProbe(Request);
		int32 SoftenedCount = 0;
		for (int32 i = 0; i < Uncarved.Num() && i < Softened.Num(); ++i)
		{
			SoftenedCount += (Uncarved[i].Density != Softened[i].Density) ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("Biome scale 0.5 probe: %d voxels softened by the caves, %d underground-air"), SoftenedCount, CountCaveAir(Request)));
		TestTrue(TEXT("The scale-0.5 caves soften some voxels (non-vacuous)"), SoftenedCount > 0);
	}
	const bool bHalfScalePass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Caves biome scale 0.5"));

	// Under the sea: a chunk whose columns' terrain is under the water level, where caves carve without the underwater
	// minimum depth and most are suppressed with it, so the GPU's suppression is exercised (a GPU without it would carve
	// here). The overrides that carve are back on: their minimum depth (8) gives way to the larger underwater one.
	// Searched like the planet's "Under the seabed" site, with the water level of the water test.
	SetOverrides(1.25f, 8.0f);
	constexpr float UnderwaterMinDepth = 25.0f;
	Request.bEnableWaterLevel = true;
	Request.WaterLevel = 800.0f;
	bool bFoundUnderSea = false;
	for (int32 Z = 0; Z >= -2 && !bFoundUnderSea; --Z)
	{
		for (int32 Y = 0; Y < 4 && !bFoundUnderSea; ++Y)
		{
			for (int32 X = 0; X < 8 && !bFoundUnderSea; ++X)
			{
				Request.ChunkCoord = FIntVector(X, Y, Z);
				CaveConfig->UnderwaterMinDepth = 0.0f;
				const int32 Unsuppressed = CountCaveAir(Request);
				CaveConfig->UnderwaterMinDepth = UnderwaterMinDepth;
				const int32 Suppressed = CountCaveAir(Request);
				// The caves deeper than the underwater minimum depth stay, so at most half survive rather than none.
				if (Unsuppressed > 200 && Suppressed * 2 < Unsuppressed)
				{
					AddInfo(FString::Printf(TEXT("Under the sea: chunk (%d, %d, %d), water level %.0f, underground air %d without the underwater minimum depth, %d with it"),
						X, Y, Z, Request.WaterLevel, Unsuppressed, Suppressed));
					bFoundUnderSea = true;
				}
			}
		}
	}
	TestTrue(TEXT("A chunk under the sea where the underwater minimum depth suppresses caves (non-vacuous)"), bFoundUnderSea);
	const bool bUnderSeaPass = bFoundUnderSea
		&& VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Caves under the sea"));

	GPUGenerator.Shutdown();
	CaveConfig->RemoveFromRoot();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU caves parity (cheese + tunnel + cave-wall) across LOD 0/1/2"), bAllPass);
	TestTrue(TEXT("GPU==CPU caves parity with the biome overrides across LOD 0/1/2"), bOverridesPass);
	TestTrue(TEXT("GPU==CPU caves parity with a biome cave scale of 0 across LOD 0/1/2"), bZeroScalePass);
	TestTrue(TEXT("GPU==CPU caves parity with a biome cave scale of 0.5 across LOD 0/1/2"), bHalfScalePass);
	TestTrue(TEXT("GPU==CPU caves parity under the sea with the underwater minimum depth across LOD 0/1/2"), bUnderSeaPass);
	return true;
}

// Phase D: GPU==CPU parity across all four noise types (Perlin, Simplex, Cellular, Voronoi) driving the
// terrain height field, with biomes on, at LOD 0/1/2. Perlin+Simplex were already covered; Cellular+Voronoi
// are the new coverage. CPU and GPU share the same Perlin permutation table + Hash3D, the 3x3x3 cell search,
// and the F1 / (F2-F1) mappings — this locks them in lockstep. The density~% channel is the primary
// noise-type signal (the terrain surface derives directly from the chosen noise type).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUNoiseTypeParityTest, "VoxelWorlds.Generation.GPUvsCPUNoiseTypeParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUNoiseTypeParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->BuildGpuData();

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	const EVoxelNoiseType Types[4] = { EVoxelNoiseType::Perlin, EVoxelNoiseType::Simplex, EVoxelNoiseType::Cellular, EVoxelNoiseType::Voronoi };
	const TCHAR* Names[4] = { TEXT("Perlin"), TEXT("Simplex"), TEXT("Cellular"), TEXT("Voronoi") };

	bool bAllPass = true;
	for (int32 ti = 0; ti < 4; ++ti)
	{
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = FIntVector(3, 1, 0);
		Request.ChunkSize = 16;
		Request.VoxelSize = 100.0f;
		Request.NoiseParams.NoiseType = Types[ti];
		Request.NoiseParams.Seed = 1337;
		Request.NoiseParams.Frequency = 0.0025f;
		Request.NoiseParams.Octaves = 3;
		Request.NoiseParams.Lacunarity = 2.0f;
		Request.NoiseParams.Persistence = 0.5f;
		Request.WorldMode = EWorldMode::InfinitePlane;
		Request.SeaLevel = 0.0f;
		Request.HeightScale = 3000.0f;
		Request.BaseHeight = 0.0f;
		Request.bEnableBiomes = true;
		Request.BiomeConfiguration = BiomeConfig;

		const FString Label = FString::Printf(TEXT("Noise-%s"), Names[ti]);
		if (!VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, *Label))
		{
			bAllPass = false;
		}
	}

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU parity across Perlin/Simplex/Cellular/Voronoi at LOD 0/1/2"), bAllPass);
	return true;
}

// Phase D: GPU==CPU parity with water ON — underwater material selection + the water-fill post-pass, at
// LOD 0/1/2. Phase B/C only ran with water off; this covers the underwater branch (GpuMaterialAtDepth's
// UwSurf/UwSub bands, gated by WaterLevelEnabled && EnableUnderwaterMaterials && TerrainHeight < WaterLevel)
// and the shared ApplyWaterFillPass (WATER flag). WaterLevel is set above the terrain so surface voxels are
// underwater; distinct underwater materials make the branch observable (vs the surface materials Phase B
// already covered).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUWaterParityTest, "VoxelWorlds.Generation.GPUvsCPUWaterParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUWaterParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->bEnableUnderwaterMaterials = true;
	// Distinct underwater materials so the underwater branch is observably different from the surface path
	// (which Phase B already covered). Any solid surface/subsurface voxel below water becomes 20/21.
	for (FBiomeDefinition& Biome : BiomeConfig->Biomes)
	{
		Biome.UnderwaterSurfaceMaterial = 20;
		Biome.UnderwaterSubsurfaceMaterial = 21;
	}
	BiomeConfig->BuildGpuData();

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(3, 1, 0);
	Request.ChunkSize = 16;
	Request.VoxelSize = 100.0f;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Request.NoiseParams.Seed = 4325;
	Request.NoiseParams.Frequency = 0.0025f;
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 3000.0f;
	Request.BaseHeight = 0.0f;
	Request.bEnableBiomes = true;
	Request.BiomeConfiguration = BiomeConfig;
	Request.bEnableWaterLevel = true;
	// Water level must sit inside this chunk's Z span [0,1600): ApplyWaterFillPass only column-scans the
	// chunk that CONTAINS the water level (bChunkContainsWaterLevel). Most columns' terrain surface is below
	// 800 (HeightScale 3000, mean ~0) → their in-chunk surface voxels are underwater.
	Request.WaterLevel = 800.0f;

	// Guard against a vacuous pass: confirm water actually filled voxels AND underwater materials appeared.
	{
		FVoxelNoiseGenerationRequest Probe = Request; Probe.LODLevel = 0;
		TArray<FVoxelData> ProbeData;
		GPUGenerator.GenerateChunkCPU(Probe, ProbeData);
		int32 WaterCount = 0, UwMatCount = 0;
		for (const FVoxelData& V : ProbeData)
		{
			if (V.HasWaterFlag()) { WaterCount++; }
			if (V.MaterialID == 20 || V.MaterialID == 21) { UwMatCount++; }
		}
		AddInfo(FString::Printf(TEXT("Water probe: %d water-flagged, %d underwater-material of %d"), WaterCount, UwMatCount, ProbeData.Num()));
		TestTrue(TEXT("Water config should fill some water voxels (non-vacuous)"), WaterCount > 0);
		TestTrue(TEXT("Underwater materials should appear (non-vacuous)"), UwMatCount > 0);
	}

	bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Water"));

	// G129: the chunk below, wholly under the water level: both paths seed its open sea from its top layer
	FVoxelNoiseGenerationRequest Submerged = Request;
	Submerged.ChunkCoord = FIntVector(3, 1, -1);
	{
		TArray<FVoxelData> ProbeData;
		GPUGenerator.GenerateChunkCPU(Submerged, ProbeData);
		int32 WaterCount = 0;
		for (const FVoxelData& V : ProbeData)
		{
			WaterCount += V.HasWaterFlag() ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("Submerged probe: %d water-flagged of %d"), WaterCount, ProbeData.Num()));
		TestTrue(TEXT("The submerged chunk holds water of its own (non-vacuous)"), WaterCount > 0);
	}
	bAllPass &= VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Submerged, TEXT("WaterSubmerged"));

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU water parity (underwater materials + water fill, a chunk holding the level and one under it) across LOD 0/1/2"), bAllPass);
	return true;
}

// Phase D: GPU==CPU parity for ore vein variants — a STREAK-shape global ore (exercises the streak-stretch
// branch of SampleGpuOreVeinNoise) and a distinct PER-BIOME ore range (exercises the per-biome
// OreStart/OreCount path, where different biomes bake different ore lists). The chunk is pushed fully
// underground (raised BaseHeight) so every voxel is deep solid → the material(density==) channel compares
// ore selection on the whole chunk. Rarity stays 1 (rarity<1 is a documented double-vs-float precision gap).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUOreVariantParityTest, "VoxelWorlds.Generation.GPUvsCPUOreVariantParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUOreVariantParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->bEnableOreVeins = true;

	// Use PER-BIOME ores (Biome.BiomeOreVeins is read fresh by GetOreVeinsForBiome — no dependency on the
	// lazily-rebuilt global-ore cache, which a direct GlobalOreVeins edit wouldn't mark dirty). Every biome
	// gets a STREAK ore (exercises SampleGpuOreVeinNoise's streak-stretch branch); biome 0 additionally gets
	// a blob "gem" so it bakes a distinct 2-ore range vs the others. bAddToGlobalOres=false → the biome ores
	// replace globals, so GetOreVeinsForBiome returns exactly this list (CPU per-voxel + GPU bake agree).
	for (int32 bi = 0; bi < BiomeConfig->Biomes.Num(); ++bi)
	{
		FBiomeDefinition& Biome = BiomeConfig->Biomes[bi];
		Biome.bAddToGlobalOres = false;
		Biome.BiomeOreVeins.Empty();

		FOreVeinConfig Streak;
		Streak.Name = TEXT("StreakOre"); Streak.MaterialID = 9; Streak.MinDepth = 12.0f; Streak.MaxDepth = 0.0f;
		Streak.Shape = EOreVeinShape::Streak; Streak.StreakStretch = 3.0f;
		Streak.Frequency = 0.03f; Streak.Threshold = 0.50f; Streak.SeedOffset = 222; Streak.Rarity = 1.0f; Streak.Priority = 0;
		Biome.BiomeOreVeins.Add(Streak);

		if (bi == 0)
		{
			FOreVeinConfig Gem;
			Gem.Name = TEXT("BiomeGem"); Gem.MaterialID = 12; Gem.MinDepth = 12.0f; Gem.MaxDepth = 0.0f;
			Gem.Shape = EOreVeinShape::Blob; Gem.Frequency = 0.025f; Gem.Threshold = 0.50f; Gem.SeedOffset = 333; Gem.Rarity = 1.0f; Gem.Priority = 5;
			Biome.BiomeOreVeins.Add(Gem);
		}
	}

	BiomeConfig->BuildGpuData();

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(3, 1, 0);
	Request.ChunkSize = 16;
	Request.VoxelSize = 100.0f;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Request.NoiseParams.Seed = 4325;
	Request.NoiseParams.Frequency = 0.0025f;
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 2000.0f;
	Request.BaseHeight = 5000.0f; // raise the surface above the chunk → the whole chunk is deep solid
	Request.bEnableBiomes = true;
	Request.BiomeConfiguration = BiomeConfig;

	// Guard against a vacuous pass: confirm some ore voxels (streak mat 9 or gem mat 12) actually placed.
	{
		FVoxelNoiseGenerationRequest Probe = Request; Probe.LODLevel = 0;
		TArray<FVoxelData> ProbeData;
		GPUGenerator.GenerateChunkCPU(Probe, ProbeData);
		int32 OreCount = 0;
		for (const FVoxelData& V : ProbeData)
		{
			if (V.MaterialID == 9 || V.MaterialID == 12) { OreCount++; }
		}
		AddInfo(FString::Printf(TEXT("Ore probe: %d ore-material voxels of %d"), OreCount, ProbeData.Num()));
		TestTrue(TEXT("Ore config should place some ore voxels (non-vacuous)"), OreCount > 0);
	}

	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("OreVariant"));

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU ore-variant parity (streak + per-biome) across LOD 0/1/2"), bAllPass);
	return true;
}

// Spherical Phase 2 (SP-B4, SP-I3; WORLD_MODES_REVIEW_PLAN.md §16): GPU==CPU parity for a planet, the full FVoxelData
// at LOD 0/1/2, on a planet like Review_Planet (R 250,000, the demo's noise, continentalness, biomes with underwater
// materials, a height rule by altitude, an ore, caves with biome overrides and the underwater minimum depth, water at
// WaterRadius 500 under the datum), off the world origin like the demo. The params come from FVoxelWorldModeFactory, as
// the chunk manager's requests do. Sites: both poles, the equator, 45 degrees, a cube-face edge, a cube corner (each the
// chunk the surface passes through), a coast (the sea surface and the seabed in one chunk), a cave (carved air under the
// ground) and caves under the seabed that the underwater minimum depth suppresses. The planet's post-passes (the radial
// water fill, the underground scan along local down) run on the GPU in the same graph, so the metadata channel compares
// them with the CPU's. Spherical Phase 6 (WM-BP6) adds Review_Planet_L's scale: R 1e8 with L's terrain, sites at both
// poles' north, 45 degrees, the equator, a cube corner, a conditioned pad and a coast ("Planet L ..."), where a float SDF
// or a float noise position would differ by 13 density steps.
namespace VoxelSphericalParity
{
	constexpr float Radius = 250000.0f;
	constexpr float WaterRadius = Radius - 500.0f;

	/** Unit directions spread over the sphere (a Fibonacci lattice), for searching sites. */
	static FVector FibonacciDirection(int32 Index, int32 Count)
	{
		const double Golden = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));
		const double Z = 1.0 - (2.0 * Index + 1.0) / Count;
		const double R = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
		return FVector(FMath::Cos(Golden * Index) * R, FMath::Sin(Golden * Index) * R, Z);
	}

	/**
	 * Away from the poles and the cube's face centres: no component of the direction above 0.8, so a chunk's lines along
	 * local down shear by at least half a voxel per step on one axis (FVoxelDownLines), and the GPU line passes are tested
	 * on sheared lines, not on columns.
	 */
	static bool IsSheared(const FVector& Direction)
	{
		return Direction.GetAbs().GetMax() <= 0.8;
	}

	static double LatitudeDeg(const FVector& Direction)
	{
		return FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Direction.Z, -1.0, 1.0)));
	}

	static double LongitudeDeg(const FVector& Direction)
	{
		return FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));
	}

	static FVector LatLonDirection(double LatDeg, double LonDeg)
	{
		const double Lat = FMath::DegreesToRadians(LatDeg);
		const double Lon = FMath::DegreesToRadians(LonDeg);
		return FVector(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	}

	struct FCounts
	{
		int32 Solid = 0;
		int32 Air = 0;
		int32 Water = 0;
		int32 UnderwaterMaterial = 0;
		int32 UndergroundAir = 0;
	};

	static FCounts Count(const TArray<FVoxelData>& Data)
	{
		FCounts C;
		for (const FVoxelData& V : Data)
		{
			V.IsSolid() ? ++C.Solid : ++C.Air;
			C.Water += V.HasWaterFlag() ? 1 : 0;
			C.UnderwaterMaterial += (V.IsSolid() && (V.MaterialID == 20 || V.MaterialID == 21)) ? 1 : 0;
			C.UndergroundAir += (V.IsAir() && V.HasUndergroundFlag()) ? 1 : 0;
		}
		return C;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUSphericalParityTest, "VoxelWorlds.Generation.GPUvsCPUSphericalParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUSphericalParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }
	using namespace VoxelSphericalParity;

	// Biomes: the defaults with continentalness (the default curves), distinct underwater materials so the underwater
	// branch is observable, a surface height rule above 2,500 uu of altitude, two global ores: iron (Rarity 1) and gold
	// at Rarity 0.5, whose Rarity hash is exact on both sides since spherical Phase 4 (SP-H3; the old frac(sin) hash put
	// about half the gold voxels on one side only).
	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->bEnableContinentalness = true;
	BiomeConfig->bEnableUnderwaterMaterials = true;
	for (FBiomeDefinition& Biome : BiomeConfig->Biomes)
	{
		Biome.UnderwaterSurfaceMaterial = 20;
		Biome.UnderwaterSubsurfaceMaterial = 21;
	}
	BiomeConfig->bEnableHeightMaterials = true;
	BiomeConfig->HeightMaterialRules.Empty();
	{
		FHeightMaterialRule Rule;
		Rule.MinHeight = 2500.0f; Rule.MaxHeight = MAX_FLT; Rule.MaterialID = 6;
		Rule.bSurfaceOnly = true; Rule.MaxDepthBelowSurface = 2.0f; Rule.Priority = 10;
		BiomeConfig->HeightMaterialRules.Add(Rule);
	}
	BiomeConfig->bEnableOreVeins = true;
	BiomeConfig->GlobalOreVeins.Empty();
	{
		FOreVeinConfig Ore;
		Ore.Name = TEXT("Iron"); Ore.MaterialID = 8; Ore.MinDepth = 12.0f; Ore.MaxDepth = 0.0f;
		Ore.Shape = EOreVeinShape::Blob; Ore.Frequency = 0.004f; Ore.Threshold = 0.6f;
		Ore.SeedOffset = 111; Ore.Rarity = 1.0f; Ore.Priority = 0;
		BiomeConfig->GlobalOreVeins.Add(Ore);
		FOreVeinConfig Gold;
		Gold.Name = TEXT("Gold"); Gold.MaterialID = 9; Gold.MinDepth = 12.0f; Gold.MaxDepth = 0.0f;
		Gold.Shape = EOreVeinShape::Blob; Gold.Frequency = 0.004f; Gold.Threshold = 0.55f;
		Gold.SeedOffset = 222; Gold.Rarity = 0.5f; Gold.Priority = 1;
		BiomeConfig->GlobalOreVeins.Add(Gold);
	}
	// The derived caches (sorted ores, height rules): without this the test generated InitializeDefaults' ores.
	BiomeConfig->RebuildCaches();

	// Caves: a cheese and a tunnel layer flattened along local up, the cave-wall override, biome overrides (Plains more
	// and shallower, Ocean none) and the underwater minimum depth, which the planet branch applies as the CPU does.
	UVoxelCaveConfiguration* CaveConfig = NewObject<UVoxelCaveConfiguration>();
	CaveConfig->AddToRoot();
	CaveConfig->bEnableCaves = true;
	CaveConfig->UnderwaterMinDepth = 60.0f;
	CaveConfig->bOverrideCaveWallMaterial = true;
	CaveConfig->CaveWallMaterialID = 2;
	CaveConfig->CaveWallMaterialMinDepth = 8.0f;
	CaveConfig->BiomeOverrides.Empty();
	{
		FBiomeCaveOverride Plains;
		Plains.BiomeID = 0; Plains.CaveScale = 1.5f; Plains.MinDepthOverride = 6.0f;
		CaveConfig->BiomeOverrides.Add(Plains);
		FBiomeCaveOverride Ocean;
		Ocean.BiomeID = 3; Ocean.CaveScale = 0.0f;
		CaveConfig->BiomeOverrides.Add(Ocean);
	}
	CaveConfig->CaveLayers.Empty();
	{
		FCaveLayerConfig Cheese;
		Cheese.bEnabled = true;
		Cheese.CaveType = ECaveType::Cheese;
		Cheese.SeedOffset = 1234;
		Cheese.Frequency = 0.0025f; Cheese.Octaves = 3; Cheese.Persistence = 0.5f; Cheese.Lacunarity = 2.0f;
		Cheese.Threshold = 0.25f; Cheese.CarveStrength = 1.0f; Cheese.CarveFalloff = 0.25f;
		Cheese.MinDepth = 4.0f; Cheese.MaxDepth = 0.0f; Cheese.DepthFadeWidth = 4.0f; Cheese.VerticalScale = 0.6f;
		CaveConfig->CaveLayers.Add(Cheese);

		FCaveLayerConfig Tunnel;
		Tunnel.bEnabled = true;
		Tunnel.CaveType = ECaveType::Spaghetti;
		Tunnel.SeedOffset = 5678;
		Tunnel.Frequency = 0.003f; Tunnel.Octaves = 2; Tunnel.Persistence = 0.5f; Tunnel.Lacunarity = 2.0f;
		Tunnel.Threshold = 0.15f; Tunnel.CarveStrength = 1.0f; Tunnel.CarveFalloff = 0.20f;
		Tunnel.MinDepth = 4.0f; Tunnel.MaxDepth = 0.0f; Tunnel.DepthFadeWidth = 4.0f; Tunnel.VerticalScale = 0.7f;
		Tunnel.SecondNoiseSeedOffset = 7777; Tunnel.SecondNoiseFrequencyScale = 1.2f;
		CaveConfig->CaveLayers.Add(Tunnel);
	}

	// The planet, like Review_Planet, off the world origin like the demo.
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::SphericalPlanet;
	Config->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
	Config->WorldRadius = Radius;
	Config->PlanetHeightScale = 6000.0f;
	Config->BaseHeight = 0.0f;
	Config->PlanetMaxTerrainHeight = 6500.0f;
	Config->PlanetMaxTerrainDepth = 6500.0f;
	Config->VoxelSize = 75.0f;
	Config->ChunkSize = 32;
	Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Config->NoiseParams.Seed = 248675;
	Config->NoiseParams.Octaves = 3;
	Config->NoiseParams.Frequency = 0.000025f;
	Config->NoiseParams.Lacunarity = 1.681f;
	Config->NoiseParams.Persistence = 0.5f;
	Config->BiomeConfiguration = BiomeConfig;
	Config->bEnableBiomes = true;
	Config->CaveConfiguration = CaveConfig;
	Config->bEnableCaves = true;
	Config->bEnableWaterLevel = true;
	Config->WaterRadius = WaterRadius;

	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVector Origin = Config->WorldOrigin;
	const double ChunkWorldSize = Config->ChunkSize * Config->VoxelSize;

	// A request as UVoxelChunkManager builds it: the world-mode fields from the factory, the rest from the config.
	auto MakeRequest = [&](const FIntVector& ChunkCoord)
	{
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.ChunkSize = Config->ChunkSize;
		Request.VoxelSize = Config->VoxelSize;
		Request.NoiseParams = Config->NoiseParams;
		FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
		Request.bEnableBiomes = Config->bEnableBiomes;
		Request.BiomeConfiguration = Config->BiomeConfiguration;
		Request.bEnableCaves = Config->bEnableCaves;
		Request.CaveConfiguration = Config->CaveConfiguration;
		Request.bEnableWaterLevel = Config->bEnableWaterLevel;
		Request.WaterLevel = Config->WaterLevel;
		Request.WaterRadius = Config->WaterRadius;
		return Request;
	};
	auto ChunkAt = [&](const FVector& WorldPos)
	{
		const FVector Local = (WorldPos - Origin) / ChunkWorldSize;
		return FIntVector(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), FMath::FloorToInt(Local.Z));
	};
	auto SurfaceRadius = [&](const FVector& Direction)
	{
		return (Mode->GetSurfacePoint(Origin + Direction * Radius, Config->NoiseParams).Position - Origin).Size();
	};
	FVoxelCPUNoiseGenerator CPUGenerator;
	CPUGenerator.Initialize();
	auto GenerateCPU = [&](const FIntVector& ChunkCoord)
	{
		TArray<FVoxelData> Data;
		CPUGenerator.GenerateChunkCPU(MakeRequest(ChunkCoord), Data);
		return Data;
	};

	struct FSite
	{
		FString Name;
		FIntVector Chunk;
		/** Planet conditioning zones on the request (spherical Phase 5, SP-B10); none for the earlier sites. */
		FVoxelConditioning Zones;
	};
	TArray<FSite> Sites;

	// The surface sites: the chunk the surface passes through along each direction.
	const TPair<const TCHAR*, FVector> SurfaceSites[] = {
		{ TEXT("North pole"), FVector(0, 0, 1) },
		{ TEXT("South pole"), FVector(0, 0, -1) },
		{ TEXT("Equator"), FVector(1, 0, 0) },
		{ TEXT("Latitude 45"), LatLonDirection(45.0, 3.0) },
		{ TEXT("Cube-face edge"), LatLonDirection(0.0, 45.0) },
		{ TEXT("Cube corner"), FVector(1, 1, 1).GetSafeNormal() },
	};
	for (const TPair<const TCHAR*, FVector>& Site : SurfaceSites)
	{
		const FVector Surface = Origin + Site.Value * SurfaceRadius(Site.Value);
		Sites.Add({ Site.Key, ChunkAt(Surface) });
		const FCounts C = Count(GenerateCPU(Sites.Last().Chunk));
		AddInfo(FString::Printf(TEXT("%s: chunk (%d, %d, %d), surface radius %.0f, solid %d, air %d, water %d"), Site.Key,
			Sites.Last().Chunk.X, Sites.Last().Chunk.Y, Sites.Last().Chunk.Z, (Surface - Origin).Size(), C.Solid, C.Air, C.Water));
		TestTrue(FString::Printf(TEXT("%s: the chunk crosses the surface (non-vacuous)"), Site.Key), C.Solid > 0 && C.Air > 0);
	}

	// The coast: a seabed just under the sea, so one chunk holds the sea surface, water and underwater materials.
	constexpr int32 SearchCount = 2000;
	{
		bool bFound = false;
		for (int32 i = 0; i < SearchCount && !bFound; ++i)
		{
			const FVector Dir = FibonacciDirection(i, SearchCount);
			if (!IsSheared(Dir))
			{
				continue;
			}
			const double SurfaceR = SurfaceRadius(Dir);
			if (SurfaceR > WaterRadius - 600.0 && SurfaceR < WaterRadius - 150.0)
			{
				const FIntVector Chunk = ChunkAt(Origin + Dir * (WaterRadius - 100.0));
				const FCounts C = Count(GenerateCPU(Chunk));
				if (C.Water > 0 && C.UnderwaterMaterial > 0 && C.Solid > 0)
				{
					Sites.Add({ TEXT("Coast"), Chunk });
					AddInfo(FString::Printf(TEXT("Coast: chunk (%d, %d, %d) at latitude %.1f, longitude %.1f, seabed radius %.0f, water %d, underwater material %d"),
						Chunk.X, Chunk.Y, Chunk.Z, LatitudeDeg(Dir), LongitudeDeg(Dir), SurfaceR, C.Water, C.UnderwaterMaterial));
					bFound = true;
				}
			}
		}
		TestTrue(TEXT("A coast chunk with water flags and underwater materials (non-vacuous)"), bFound);
	}

	// A cave: carved underground air 1,200 uu under dry land.
	{
		bool bFound = false;
		for (int32 i = 0; i < SearchCount && !bFound; i += 7)
		{
			const FVector Dir = FibonacciDirection(i, SearchCount);
			if (!IsSheared(Dir))
			{
				continue;
			}
			const double SurfaceR = SurfaceRadius(Dir);
			if (SurfaceR < WaterRadius + 300.0)
			{
				continue;
			}
			const FIntVector Chunk = ChunkAt(Origin + Dir * (SurfaceR - 1200.0));
			const FCounts C = Count(GenerateCPU(Chunk));
			if (C.UndergroundAir > 200)
			{
				Sites.Add({ TEXT("Cave"), Chunk });
				AddInfo(FString::Printf(TEXT("Cave: chunk (%d, %d, %d) at latitude %.1f, longitude %.1f, surface radius %.0f, underground air %d"),
					Chunk.X, Chunk.Y, Chunk.Z, LatitudeDeg(Dir), LongitudeDeg(Dir), SurfaceR, C.UndergroundAir));
				bFound = true;
			}
		}
		TestTrue(TEXT("A cave chunk with carved underground air (non-vacuous)"), bFound);
	}

	// Under the seabed: a chunk where caves carve without the underwater minimum depth and are suppressed with it, so
	// the GPU's suppression is exercised (a GPU without it would carve here).
	{
		bool bFound = false;
		for (int32 i = 0; i < SearchCount && !bFound; i += 3)
		{
			const FVector Dir = FibonacciDirection(i, SearchCount);
			if (!IsSheared(Dir))
			{
				continue;
			}
			const double SurfaceR = SurfaceRadius(Dir);
			if (SurfaceR > WaterRadius - 400.0)
			{
				continue;
			}
			const FIntVector Chunk = ChunkAt(Origin + Dir * (SurfaceR - 900.0));
			CaveConfig->UnderwaterMinDepth = 0.0f;
			const FCounts Unsuppressed = Count(GenerateCPU(Chunk));
			CaveConfig->UnderwaterMinDepth = 60.0f;
			const FCounts Suppressed = Count(GenerateCPU(Chunk));
			if (Unsuppressed.UndergroundAir > 200 && Suppressed.UndergroundAir < Unsuppressed.UndergroundAir / 4)
			{
				Sites.Add({ TEXT("Under the seabed"), Chunk });
				AddInfo(FString::Printf(TEXT("Under the seabed: chunk (%d, %d, %d) at latitude %.1f, longitude %.1f, seabed radius %.0f, underground air %d without the underwater minimum depth, %d with it"),
					Chunk.X, Chunk.Y, Chunk.Z, LatitudeDeg(Dir), LongitudeDeg(Dir), SurfaceR, Unsuppressed.UndergroundAir, Suppressed.UndergroundAir));
				bFound = true;
			}
		}
		TestTrue(TEXT("A chunk under the seabed where the underwater minimum depth suppresses caves (non-vacuous)"), bFound);
	}

	// The open sea (G132): a chunk wholly under the sea, which its lines seed from where they enter it (before G132 such a
	// chunk had no water of its own until its neighbours flooded it at load). One holding the seabed under its water if the
	// sea is deep enough somewhere, else one of open water.
	{
		const FVoxelSeaSurface Sea = FVoxelSeaSurface::MakeSphere(Origin, WaterRadius);
		FIntVector WaterOnly(0, 0, 0);
		bool bWaterOnly = false;
		bool bFound = false;
		int32 Generated = 0; // chunks generated by the search: at most 200
		for (int32 i = 0; i < SearchCount && !bFound && Generated < 200; ++i)
		{
			const FVector Dir = FibonacciDirection(i, SearchCount);
			const double SurfaceR = SurfaceRadius(Dir);
			for (double Up = 0.1 * ChunkWorldSize; !bFound && SurfaceR + Up < WaterRadius; Up += 0.25 * ChunkWorldSize)
			{
				const FIntVector Chunk = ChunkAt(Origin + Dir * (SurfaceR + Up));
				const FVector Min = Origin + FVector(Chunk) * ChunkWorldSize;
				const FBox Box(Min, Min + FVector(ChunkWorldSize));
				if (Sea.IsAbove(Box) || Sea.Crosses(Box) || Generated >= 200)
				{
					continue;
				}
				++Generated;
				const FCounts C = Count(GenerateCPU(Chunk));
				if (C.Water > 1000 && C.Solid > 0)
				{
					Sites.Add({ TEXT("Open sea"), Chunk });
					AddInfo(FString::Printf(TEXT("Open sea: chunk (%d, %d, %d) at latitude %.1f, longitude %.1f, seabed radius %.0f, water %d, solid %d"),
						Chunk.X, Chunk.Y, Chunk.Z, LatitudeDeg(Dir), LongitudeDeg(Dir), SurfaceR, C.Water, C.Solid));
					bFound = true;
				}
				else if (!bWaterOnly && C.Water > 1000)
				{
					WaterOnly = Chunk;
					bWaterOnly = true;
				}
			}
		}
		if (!bFound && bWaterOnly)
		{
			Sites.Add({ TEXT("Open sea"), WaterOnly });
			AddInfo(FString::Printf(TEXT("Open sea: chunk (%d, %d, %d), open water only (the sea is nowhere a chunk deeper than its seabed)"),
				WaterOnly.X, WaterOnly.Y, WaterOnly.Z));
		}
		TestTrue(TEXT("A chunk wholly under the sea with water of its own (non-vacuous)"), bFound || bWaterOnly);
	}

	// Conditioned pads (spherical Phase 5, WM-PL3): planet zones raise the ground at 45 degrees and lower it across a
	// cube-face edge; the sites are the chunk the pad's surface passes through and one on the falloff ring's middle.
	{
		const float Inner = 1500.0f;
		const float Falloff = 1000.0f;
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		auto AlongSurface = [](const FVector& From, const FVector& Toward, double Distance)
		{
			const FVector Tangent = (Toward - From * FVector::DotProduct(Toward, From)).GetSafeNormal();
			return (From * FMath::Cos(Distance / Radius) + Tangent * FMath::Sin(Distance / Radius)).GetSafeNormal();
		};
		struct FPad
		{
			const TCHAR* Key;
			FVector Direction;
			float Shift;
		};
		const FPad Pads[] = {
			{ TEXT("Pad raised at latitude 45"), LatLonDirection(45.0, 3.0), 300.0f },
			{ TEXT("Pad lowered across a cube-face edge"), LatLonDirection(0.0, 45.0), -300.0f },
		};
		for (const FPad& Pad : Pads)
		{
			const FVector Dir = Pad.Direction;
			const float Target = Planet.GetTerrainAltitude(Dir, Config->NoiseParams) + Pad.Shift;
			const FVoxelConditioning Zones = FVoxelConditioning::OfZones({ FVoxelConditioningZone::MakeRadial(Dir, Inner, Falloff, Target) });
			Sites.Add({ Pad.Key, ChunkAt(Origin + Dir * (Radius + Target)), Zones });
			const FVector RingDir = AlongSurface(Dir, FVector::CrossProduct(FVector(0, 0, 1), Dir).IsNearlyZero() ? FVector(1, 0, 0) : FVector::CrossProduct(FVector(0, 0, 1), Dir), Inner + 0.5 * Falloff);
			const float RingAltitude = FMath::Lerp(Planet.GetTerrainAltitude(RingDir, Config->NoiseParams), Target, 0.5f);
			Sites.Add({ FString::Printf(TEXT("%s, ring"), Pad.Key), ChunkAt(Origin + RingDir * (Radius + RingAltitude)), Zones });
			AddInfo(FString::Printf(TEXT("%s: pad altitude %.0f, chunk (%d, %d, %d)"), Pad.Key, Target,
				Sites[Sites.Num() - 2].Chunk.X, Sites[Sites.Num() - 2].Chunk.Y, Sites[Sites.Num() - 2].Chunk.Z));
		}
	}
	CPUGenerator.Shutdown();

	// Thresholds, tighter than the plane's (its chunks are 16^3, where one voxel is 0.024%). Measured on the first run
	// (WORLD_MODES_REVIEW_PLAN.md §16.2), every site at every LOD: density within 2 on >= 99.9%, material >= 99.98%, biome
	// 100%, metadata >= 99.997% (one voxel in 32,768), material where the density matches >= 99.98%. What differs is float
	// against double at FP boundaries: a density of 126 against 127 at the surface gates the cave carve (the voxels off by
	// more than 2) and decides solid or air for the water and underground flags beside it; depth-band edges flip a
	// material. A flood that failed to converge, a line walked in the wrong order or a missing branch moves hundreds of
	// voxels and fails these.
	VoxelParityHarness::FParityThresholds PlanetThresholds;
	PlanetThresholds.DensityClose = 99.5f;
	PlanetThresholds.Mat = 99.0f;
	PlanetThresholds.Biome = 99.9f;
	PlanetThresholds.Meta = 99.9f;
	PlanetThresholds.MatWhenDensEq = 99.5f;
	PlanetThresholds.WatchMaterialID = 9; // gold, Rarity 0.5 (SP-H3)
	PlanetThresholds.WatchMaterialMatch = 99.0f;

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();
	bool bAllPass = true;
	for (const FSite& Site : Sites)
	{
		const FString Label = FString::Printf(TEXT("Planet %s"), *Site.Name);
		FVoxelNoiseGenerationRequest Request = MakeRequest(Site.Chunk);
		Request.Conditioning = Site.Zones;
		if (!VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, *Label, PlanetThresholds))
		{
			bAllPass = false;
		}
	}
	GPUGenerator.Shutdown();

	// Review_Planet_L's scale (spherical Phase 6, WM-BP6): R 1e8 (1,000 km), L's terrain (the demo's base wavelength and
	// height scale 64 times larger, 6 more octaves), the sea 83,333 under the datum, the same biomes, ore and caves, off the
	// world origin. The GPU computes its altitudes, conditioning chords, sea test and noise offsets in the chunk's frame
	// (PlanetFrame.ush, VoxelNoise.ush) and must match the CPU's double math.
	{
		constexpr double RL = 1.0e8;
		UVoxelWorldConfiguration* LConfig = NewObject<UVoxelWorldConfiguration>();
		LConfig->AddToRoot();
		LConfig->WorldMode = EWorldMode::SphericalPlanet;
		LConfig->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
		LConfig->WorldRadius = static_cast<float>(RL);
		LConfig->PlanetHeightScale = 384000.0f;
		LConfig->BaseHeight = 0.0f;
		LConfig->PlanetMaxTerrainHeight = 762000.0f;
		LConfig->PlanetMaxTerrainDepth = 762000.0f;
		LConfig->VoxelSize = 75.0f;
		LConfig->ChunkSize = 32;
		LConfig->NoiseParams = Config->NoiseParams;
		LConfig->NoiseParams.Frequency = 2.5e-5f / 64.0f;
		LConfig->NoiseParams.Octaves = 9;
		LConfig->NoiseParams.Lacunarity = 2.0f;
		LConfig->BiomeConfiguration = BiomeConfig;
		LConfig->bEnableBiomes = true;
		LConfig->CaveConfiguration = CaveConfig;
		LConfig->bEnableCaves = true;
		LConfig->bEnableWaterLevel = true;
		LConfig->WaterRadius = static_cast<float>(RL - 83333.0);

		const TSharedRef<IVoxelWorldMode> LMode = FVoxelWorldModeFactory::Create(*LConfig);
		const FSphericalPlanetWorldMode& LPlanet = static_cast<const FSphericalPlanetWorldMode&>(*LMode);
		const FVector LOrigin = LConfig->WorldOrigin;
		const double LChunkWorldSize = LConfig->ChunkSize * LConfig->VoxelSize;
		const double LWaterRadius = LConfig->WaterRadius;
		auto MakeLRequest = [&](const FIntVector& ChunkCoord)
		{
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = ChunkCoord;
			Request.ChunkSize = LConfig->ChunkSize;
			Request.VoxelSize = LConfig->VoxelSize;
			Request.NoiseParams = LConfig->NoiseParams;
			FVoxelWorldModeFactory::FillGenerationRequest(*LConfig, Request);
			Request.bEnableBiomes = true;
			Request.BiomeConfiguration = BiomeConfig;
			Request.bEnableCaves = true;
			Request.CaveConfiguration = CaveConfig;
			Request.bEnableWaterLevel = true;
			Request.WaterLevel = LConfig->WaterLevel;
			Request.WaterRadius = LConfig->WaterRadius;
			return Request;
		};
		auto LChunkAt = [&](const FVector& WorldPos)
		{
			const FVector Local = (WorldPos - LOrigin) / LChunkWorldSize;
			return FIntVector(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), FMath::FloorToInt(Local.Z));
		};
		auto LSurfaceRadius = [&](const FVector& Direction)
		{
			return RL + LPlanet.GetTerrainAltitude(Direction, LConfig->NoiseParams);
		};
		FVoxelCPUNoiseGenerator LCPU;
		LCPU.Initialize();
		auto LCount = [&](const FIntVector& Chunk, const FVoxelConditioning& Zones = FVoxelConditioning())
		{
			FVoxelNoiseGenerationRequest Request = MakeLRequest(Chunk);
			Request.Conditioning = Zones;
			TArray<FVoxelData> Data;
			LCPU.GenerateChunkCPU(Request, Data);
			return Count(Data);
		};

		TArray<FSite> LSites;
		const TPair<const TCHAR*, FVector> LSurfaceSites[] = {
			{ TEXT("L North pole"), FVector(0, 0, 1) },
			{ TEXT("L Latitude 45"), LatLonDirection(45.0, 3.0) },
			{ TEXT("L Equator"), FVector(1, 0, 0) },
			{ TEXT("L Cube corner"), FVector(1, 1, 1).GetSafeNormal() },
		};
		for (const TPair<const TCHAR*, FVector>& Site : LSurfaceSites)
		{
			const FVector Surface = LOrigin + Site.Value * LSurfaceRadius(Site.Value);
			LSites.Add({ Site.Key, LChunkAt(Surface) });
			const FCounts C = LCount(LSites.Last().Chunk);
			AddInfo(FString::Printf(TEXT("%s: chunk (%d, %d, %d), surface altitude %.0f, solid %d, air %d, water %d"), Site.Key,
				LSites.Last().Chunk.X, LSites.Last().Chunk.Y, LSites.Last().Chunk.Z, LSurfaceRadius(Site.Value) - RL, C.Solid, C.Air, C.Water));
			TestTrue(FString::Printf(TEXT("%s: the chunk crosses the surface (non-vacuous)"), Site.Key), C.Solid > 0 && C.Air > 0);
		}

		// A conditioned pad at 45 degrees: the ground raised 300 uu over 1,500 uu with a 1,000 uu falloff (POI-sized), the
		// pad's chunk and one on its ring.
		{
			const FVector Dir = LatLonDirection(45.0, 3.0);
			const float Target = LPlanet.GetTerrainAltitude(Dir, LConfig->NoiseParams) + 300.0f;
			const FVoxelConditioning Zones = FVoxelConditioning::OfZones({ FVoxelConditioningZone::MakeRadial(Dir, 1500.0f, 1000.0f, Target) });
			LSites.Add({ TEXT("L Pad raised at latitude 45"), LChunkAt(LOrigin + Dir * (RL + Target)), Zones });
			const FVector Tangent = FVector::CrossProduct(FVector(0, 0, 1), Dir).GetSafeNormal();
			const FVector RingDir = (Dir * FMath::Cos(2000.0 / RL) + Tangent * FMath::Sin(2000.0 / RL)).GetSafeNormal();
			const float RingAltitude = FMath::Lerp(LPlanet.GetTerrainAltitude(RingDir, LConfig->NoiseParams), Target, 0.5f);
			LSites.Add({ TEXT("L Pad raised at latitude 45, ring"), LChunkAt(LOrigin + RingDir * (RL + RingAltitude)), Zones });
			AddInfo(FString::Printf(TEXT("L Pad raised at latitude 45: pad altitude %.0f, chunk (%d, %d, %d)"), Target,
				LSites[LSites.Num() - 2].Chunk.X, LSites[LSites.Num() - 2].Chunk.Y, LSites[LSites.Num() - 2].Chunk.Z));
		}

		// A coast: along a great circle from 45 degrees, bisect the first crossing of the sea for a seabed 300 uu under it,
		// the chunk 100 uu under the sea holding the surface, water and the seabed.
		{
			bool bFound = false;
			const FVector Start = LatLonDirection(45.0, 3.0);
			const FVector Axis = FVector::CrossProduct(Start, FVector(0, 0, 1)).GetSafeNormal();
			const double Step = 0.002; // 200 km of arc
			auto SeabedMinusTarget = [&](double Angle)
			{
				return LSurfaceRadius(Start.RotateAngleAxisRad(Angle, Axis)) - (LWaterRadius - 300.0);
			};
			for (int32 i = 0; i < 3000 && !bFound; ++i)
			{
				const double A0 = i * Step;
				const double A1 = A0 + Step;
				if ((SeabedMinusTarget(A0) > 0.0) != (SeabedMinusTarget(A1) > 0.0))
				{
					double Lo = A0, Hi = A1;
					for (int32 b = 0; b < 60; ++b)
					{
						const double Mid = 0.5 * (Lo + Hi);
						((SeabedMinusTarget(Mid) > 0.0) == (SeabedMinusTarget(Lo) > 0.0) ? Lo : Hi) = Mid;
					}
					const FVector Dir = Start.RotateAngleAxisRad(0.5 * (Lo + Hi), Axis);
					const FIntVector Chunk = LChunkAt(LOrigin + Dir * (LWaterRadius - 100.0));
					const FCounts C = LCount(Chunk);
					if (C.Water > 0 && C.UnderwaterMaterial > 0 && C.Solid > 0)
					{
						LSites.Add({ TEXT("L Coast"), Chunk });
						AddInfo(FString::Printf(TEXT("L Coast: chunk (%d, %d, %d) at latitude %.2f, longitude %.2f, water %d, underwater material %d"),
							Chunk.X, Chunk.Y, Chunk.Z, LatitudeDeg(Dir), LongitudeDeg(Dir), C.Water, C.UnderwaterMaterial));
						bFound = true;
					}
				}
			}
			TestTrue(TEXT("L: a coast chunk with water flags and underwater materials (non-vacuous)"), bFound);
		}
		LCPU.Shutdown();

		FVoxelGPUNoiseGenerator LGPU;
		LGPU.Initialize();
		for (const FSite& Site : LSites)
		{
			const FString Label = FString::Printf(TEXT("Planet %s"), *Site.Name);
			FVoxelNoiseGenerationRequest Request = MakeLRequest(Site.Chunk);
			Request.Conditioning = Site.Zones;
			if (!VoxelParityHarness::RunGpuCpuParity(*this, LGPU, Request, *Label, PlanetThresholds))
			{
				bAllPass = false;
			}
		}
		LGPU.Shutdown();
		LConfig->RemoveFromRoot();
	}

	Config->RemoveFromRoot();
	CaveConfig->RemoveFromRoot();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU planet parity (radial height, materials, caves, water, underground) at every site across LOD 0/1/2, R 2.5e5 and 1e8"), bAllPass);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNoiseGeneratorPerformanceTest, "VoxelWorlds.Generation.Performance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::MediumPriority)

bool FVoxelNoiseGeneratorPerformanceTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }

	FVoxelGPUNoiseGenerator Generator;
	Generator.Initialize();

	// Create generation request for 32^3 chunk (standard size)
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(0, 0, 0);
	Request.ChunkSize = 32;
	Request.VoxelSize = 100.0f;
	Request.LODLevel = 0;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Perlin;
	Request.NoiseParams.Seed = 12345;
	Request.NoiseParams.Frequency = 0.013f; // Avoids integer grid positions
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;

	const int32 NumIterations = 10;

	// Benchmark CPU generation
	double CPUTotalTime = 0.0;
	for (int32 i = 0; i < NumIterations; ++i)
	{
		Request.ChunkCoord = FIntVector(i, 0, 0);
		TArray<FVoxelData> VoxelData;

		double StartTime = FPlatformTime::Seconds();
		Generator.GenerateChunkCPU(Request, VoxelData);
		CPUTotalTime += FPlatformTime::Seconds() - StartTime;
	}
	double CPUAvgMs = (CPUTotalTime / NumIterations) * 1000.0;

	// Benchmark GPU generation (including readback for fair comparison)
	double GPUTotalTime = 0.0;
	for (int32 i = 0; i < NumIterations; ++i)
	{
		Request.ChunkCoord = FIntVector(i + 100, 0, 0);

		volatile bool bCompleted = false;
		FVoxelGenerationHandle Handle;

		double StartTime = FPlatformTime::Seconds();

		Handle = Generator.GenerateChunkAsync(Request,
			FOnVoxelGenerationComplete::CreateLambda([&bCompleted](FVoxelGenerationHandle, bool)
			{
				bCompleted = true;
			}));

		while (!bCompleted)
		{
			FPlatformProcess::Sleep(0.001f);
			FlushRenderingCommands();
		}

		TArray<FVoxelData> VoxelData;
		Generator.ReadbackToCPU(Handle, VoxelData);

		GPUTotalTime += FPlatformTime::Seconds() - StartTime;
		Generator.ReleaseHandle(Handle);
	}
	double GPUAvgMs = (GPUTotalTime / NumIterations) * 1000.0;

	AddInfo(FString::Printf(TEXT("32^3 chunk generation performance:")));
	AddInfo(FString::Printf(TEXT("  CPU average: %.2f ms"), CPUAvgMs));
	AddInfo(FString::Printf(TEXT("  GPU average (with readback): %.2f ms"), GPUAvgMs));

	// Performance targets from GPU_PIPELINE.md: 0.3-0.5ms for generation
	// With readback overhead, we expect more, but should still be reasonable
	TestTrue(TEXT("CPU generation should complete in reasonable time (< 50ms)"), CPUAvgMs < 50.0);
	TestTrue(TEXT("GPU generation should complete in reasonable time (< 20ms with readback)"), GPUAvgMs < 20.0);

	Generator.Shutdown();

	return true;
}

// The climate options and the ridged terrain fBm (ClimateShaping.ush, NoiseFBM3DRidged against FVoxelBiomeSnapshot's
// Shape* functions and FVoxelNoise's ridged loop), every one on at once: on the plane equalised temperature, moisture
// and continentalness, the lapse and the ridged terrain; on a planet also the latitude blend, at the poles, the equator
// and 45 degrees. Climate fields a few hundred uu across, so a chunk crosses several biome borders, and a CPU run with
// the options off shows they move biomes in these chunks (non-vacuous).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUClimateShapingParityTest, "VoxelWorlds.Generation.GPUvsCPUClimateShapingParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUClimateShapingParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }
	using namespace VoxelSphericalParity;

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->bEnableContinentalness = true;
	BiomeConfig->TemperatureNoiseFrequency = 0.0012f;
	BiomeConfig->MoistureNoiseFrequency = 0.0015f;
	BiomeConfig->ContinentalnessNoiseFrequency = 0.0008f;
	BiomeConfig->RebuildCaches();
	BiomeConfig->BuildGpuData();

	auto SetOptions = [BiomeConfig](bool bOn, float LatitudeWeight)
	{
		BiomeConfig->bEqualizeClimate = bOn;
		BiomeConfig->bEqualizeContinentalness = bOn;
		BiomeConfig->TemperatureLatitudeWeight = bOn ? LatitudeWeight : 0.0f;
		BiomeConfig->TemperatureLapseRate = bOn ? 1.5f : 0.0f;
	};
	// How many voxels change biome when the options go off: the options must act in the chunks compared.
	FVoxelCPUNoiseGenerator CPUGenerator;
	CPUGenerator.Initialize();
	auto BiomesMoved = [&](const FVoxelNoiseGenerationRequest& Request, float LatitudeWeight)
	{
		TArray<FVoxelData> On, Off;
		SetOptions(true, LatitudeWeight);
		CPUGenerator.GenerateChunkCPU(Request, On);
		SetOptions(false, LatitudeWeight);
		FVoxelNoiseGenerationRequest Plain = Request;
		Plain.NoiseParams.bRidged = false;
		CPUGenerator.GenerateChunkCPU(Plain, Off);
		SetOptions(true, LatitudeWeight);
		int32 Moved = 0;
		for (int32 i = 0; i < FMath::Min(On.Num(), Off.Num()); ++i)
		{
			Moved += On[i].BiomeID != Off[i].BiomeID;
		}
		return Moved;
	};

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();
	bool bAllPass = true;

	// The plane
	{
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = FIntVector(3, 1, 0);
		Request.ChunkSize = 16;
		Request.VoxelSize = 100.0f;
		Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Request.NoiseParams.Seed = 4325;
		Request.NoiseParams.Frequency = 0.0025f;
		Request.NoiseParams.Octaves = 4;
		Request.NoiseParams.Lacunarity = 2.0f;
		Request.NoiseParams.Persistence = 0.5f;
		Request.NoiseParams.bRidged = true;
		Request.WorldMode = EWorldMode::InfinitePlane;
		Request.SeaLevel = 0.0f;
		Request.HeightScale = 4000.0f;
		Request.BaseHeight = 0.0f;
		Request.bEnableBiomes = true;
		Request.BiomeConfiguration = BiomeConfig;
		const int32 Moved = BiomesMoved(Request, 0.0f);
		AddInfo(FString::Printf(TEXT("Plane: %d voxels change biome with the options off"), Moved));
		TestTrue(TEXT("Plane: the options move biomes in the chunk (non-vacuous)"), Moved > 0);
		bAllPass &= VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Climate shaping, plane"));
	}

	// A planet: Review_Planet's size, the latitude blend on
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
		Config->WorldRadius = Radius;
		Config->PlanetHeightScale = 6000.0f;
		Config->PlanetMaxTerrainHeight = 6500.0f;
		Config->PlanetMaxTerrainDepth = 6500.0f;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 32;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 248675;
		Config->NoiseParams.Octaves = 3;
		Config->NoiseParams.Frequency = 0.000025f;
		Config->NoiseParams.Lacunarity = 1.681f;
		Config->NoiseParams.Persistence = 0.5f;
		Config->NoiseParams.bRidged = true;
		Config->BiomeConfiguration = BiomeConfig;
		Config->bEnableBiomes = true;
		SetOptions(true, 0.6f);

		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FVector Origin = Config->WorldOrigin;
		const double ChunkWorldSize = Config->ChunkSize * Config->VoxelSize;
		VoxelParityHarness::FParityThresholds PlanetThresholds;
		PlanetThresholds.DensityClose = 99.5f;
		PlanetThresholds.Mat = 99.0f;
		PlanetThresholds.Biome = 99.9f;
		PlanetThresholds.Meta = 99.9f;
		PlanetThresholds.MatWhenDensEq = 99.5f;

		const TPair<const TCHAR*, FVector> SurfaceSites[] = {
			{ TEXT("north pole"), FVector(0, 0, 1) },
			{ TEXT("south pole"), FVector(0, 0, -1) },
			{ TEXT("equator"), LatLonDirection(0.0, 20.0) },
			{ TEXT("latitude 45"), LatLonDirection(45.0, 3.0) },
		};
		int32 MovedTotal = 0;
		for (const TPair<const TCHAR*, FVector>& Site : SurfaceSites)
		{
			const FVector Surface = Mode->GetSurfacePoint(Origin + Site.Value * Radius, Config->NoiseParams).Position;
			const FVector Local = (Surface - Origin) / ChunkWorldSize;
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = FIntVector(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), FMath::FloorToInt(Local.Z));
			Request.ChunkSize = Config->ChunkSize;
			Request.VoxelSize = Config->VoxelSize;
			Request.NoiseParams = Config->NoiseParams;
			FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
			Request.bEnableBiomes = true;
			Request.BiomeConfiguration = BiomeConfig;
			MovedTotal += BiomesMoved(Request, 0.6f);
			bAllPass &= VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request,
				*FString::Printf(TEXT("Climate shaping, planet %s"), Site.Key), PlanetThresholds);
		}
		AddInfo(FString::Printf(TEXT("Planet: %d voxels change biome with the options off"), MovedTotal));
		TestTrue(TEXT("Planet: the options move biomes in the chunks (non-vacuous)"), MovedTotal > 0);
		Config->RemoveFromRoot();
	}

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU parity with the climate options and the ridged terrain on, plane and planet, LOD 0/1/2"), bAllPass);
	return true;
}

// G129: the heightmap water fill in a chunk wholly under the water level. Until G129 only the chunk holding the level
// was seeded, so a chunk under it had no water of its own and the chunk manager flooded it on the game thread when its
// mesh landed (PropagateWaterFromNeighbors, a flood over the whole chunk). Now its columns are scanned from its top layer:
// the open sea over the ground takes the flag, the ground and a cave in it stay dry, the cave flags clear; a chunk whose
// top meets the water level is under it too, and a chunk above the level takes none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterFillSubmergedChunkTest, "VoxelWorlds.Generation.WaterFill.SubmergedChunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterFillSubmergedChunkTest::RunTest(const FString& Parameters)
{
	constexpr int32 N = 16;
	auto Index = [](int32 X, int32 Y, int32 Z) { return X + Y * N + Z * N * N; };
	// Ground below Z 4 + (X + Y) % 5 in voxels, a cave (cave-flagged, as carving leaves it) in it
	auto Build = [&](TArray<FVoxelData>& Data)
	{
		Data.SetNum(N * N * N);
		for (int32 Z = 0; Z < N; ++Z)
		for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			const bool bGround = Z < 4 + (X + Y) % 5;
			FVoxelData Voxel = bGround ? FVoxelData::Solid(1, 0) : FVoxelData::Air();
			if (bGround && FVector::Dist(FVector(X, Y, Z), FVector(8.0, 8.0, 1.5)) < 1.6)
			{
				Voxel = FVoxelData::Air();
				Voxel.SetCaveFlag(true);
			}
			Data[Index(X, Y, Z)] = Voxel;
		}
	};
	auto Count = [&](const TArray<FVoxelData>& Data, int32& OutSeaAir, int32& OutSeaWet, int32& OutCaveAir, int32& OutCaveWet,
		int32& OutSolidWet, int32& OutCaveFlags)
	{
		OutSeaAir = OutSeaWet = OutCaveAir = OutCaveWet = OutSolidWet = OutCaveFlags = 0;
		for (int32 Z = 0; Z < N; ++Z)
		for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			const FVoxelData& Voxel = Data[Index(X, Y, Z)];
			OutCaveFlags += Voxel.HasCaveFlag() ? 1 : 0;
			if (Voxel.IsSolid())
			{
				OutSolidWet += Voxel.HasWaterFlag() ? 1 : 0;
			}
			else if (Z < 4 + (X + Y) % 5)
			{
				++OutCaveAir;
				OutCaveWet += Voxel.HasWaterFlag() ? 1 : 0;
			}
			else
			{
				++OutSeaAir;
				OutSeaWet += Voxel.HasWaterFlag() ? 1 : 0;
			}
		}
	};

	FVoxelNoiseGenerationRequest Request;
	Request.ChunkSize = N;
	Request.VoxelSize = 100.0f;
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.bEnableWaterLevel = true;
	Request.ChunkCoord = FIntVector(2, -1, -2); // the chunk spans Z -3,200 to -1,600, its voxels -3,200 to -1,700

	// Wholly under the level (the level 2,000 above the chunk's top), and with the level at the chunk's top
	for (const float WaterLevel : { 400.0f, -1600.0f })
	{
		Request.WaterLevel = WaterLevel;
		TArray<FVoxelData> Data;
		Build(Data);
		FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(Request, Data);
		int32 SeaAir, SeaWet, CaveAir, CaveWet, SolidWet, CaveFlags;
		Count(Data, SeaAir, SeaWet, CaveAir, CaveWet, SolidWet, CaveFlags);
		AddInfo(FString::Printf(TEXT("Water level %.0f: sea %d of %d air voxels wet, cave %d of %d, solid %d wet"),
			WaterLevel, SeaWet, SeaAir, CaveWet, CaveAir, SolidWet));
		TestTrue(TEXT("The chunk has open sea and a cave"), SeaAir > 1000 && CaveAir > 5);
		TestEqual(TEXT("All open sea over the ground is wet"), SeaWet, SeaAir);
		TestEqual(TEXT("The cave stays dry"), CaveWet, 0);
		TestEqual(TEXT("No solid voxel is wet"), SolidWet, 0);
		TestEqual(TEXT("Cave flags are cleared after the fill"), CaveFlags, 0);
	}

	// Above the level: nothing to seed
	Request.WaterLevel = -3300.0f;
	TArray<FVoxelData> Dry;
	Build(Dry);
	FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(Request, Dry);
	int32 SeaAir, SeaWet, CaveAir, CaveWet, SolidWet, CaveFlags;
	Count(Dry, SeaAir, SeaWet, CaveAir, CaveWet, SolidWet, CaveFlags);
	TestEqual(TEXT("A chunk above the water level takes no water"), SeaWet + CaveWet + SolidWet, 0);
	return true;
}

// Conditioning lines on the GPU (Documentation/TERRAIN_LINES_PLAN.md, L2): the chunk's segment list
// (FVoxelChunkLineSegments) is uploaded as ConditioningLineData and walked by GpuApplyConditioningLines
// (ConditioningLines.ush) before the zones, in the heightmap branch and in the planet branch, and gives what the CPU
// generator gives from the same list, at LOD 0/1/2. On a plane far from the world's origin: a diagonal line on a grade,
// a line through a chunk corner (the four chunks around it), and a lower-only and a raise-only line that cross under a
// zone. On a planet: a diagonal line at latitude 45, a line across a cube-face edge, and a line through a chunk corner
// (those of the eight chunks around it that the surface passes through). A CPU run without the lines shows they move
// the voxels of each chunk (non-vacuous).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPULineParityTest, "VoxelWorlds.Generation.GPUvsCPULineParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPULineParityTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: GPU test requires a real RHI (run without -nullrhi)")); return true; }
	using namespace VoxelSphericalParity;
	using EBlend = EVoxelConditioningLineBlend;

	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->BuildGpuData();

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();
	FVoxelCPUNoiseGenerator CPUGenerator;
	CPUGenerator.Initialize();

	struct FSite
	{
		FString Name;
		FVoxelNoiseGenerationRequest Request;
	};
	TArray<FSite> Sites;
	const float VoxelSize = 75.0f;
	const int32 ChunkSize = 32;
	const double ChunkWorldSize = ChunkSize * VoxelSize;
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;
	const double Spacing = 2.0 * VoxelSize;

	// ---- The plane, its grid origin 1,000 km from the world's: a float world position is 0.06 uu coarse there, and
	// the segment list is in the chunk's frame.
	const FVector PlaneOrigin(1.0e6 + 37.5, -2.0e6 + 112.5, 0.0);
	auto MakePlaneRequest = [&](const FIntVector& ChunkCoord)
	{
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = VoxelSize;
		Request.WorldOrigin = PlaneOrigin;
		Request.NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Request.NoiseParams.Seed = 4325;
		Request.NoiseParams.Frequency = 0.0006f;
		Request.NoiseParams.Octaves = 4;
		Request.NoiseParams.Lacunarity = 2.0f;
		Request.NoiseParams.Persistence = 0.5f;
		Request.WorldMode = EWorldMode::InfinitePlane;
		Request.SeaLevel = 0.0f;
		Request.HeightScale = 1400.0f;
		Request.BaseHeight = 1200.0f;
		Request.bEnableBiomes = true;
		Request.BiomeConfiguration = BiomeConfig;
		return Request;
	};
	// A straight line between two points given relative to the grid origin.
	auto PlaneLine = [&](const FVector2D& A, const FVector2D& B, float TargetA, float TargetB, EBlend Blend, int64 Id)
	{
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		Params.Blend = Blend;
		Params.Id = Id;
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32((B - A).Size() / Spacing));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const double T = static_cast<double>(Step) / Steps;
			const FVector2D P = FMath::Lerp(A, B, T);
			Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(PlaneOrigin.X + P.X, PlaneOrigin.Y + P.Y, 0.0), FMath::Lerp(TargetA, TargetB, static_cast<float>(T)), HalfWidth));
		}
		return FVoxelConditioningLine::Make(Params);
	};
	{
		// Chunk (3, 1, 0): X 7,200 to 9,600, Y 2,400 to 4,800, Z 0 to 2,400 from the grid origin.
		FSite& Diagonal = Sites.AddDefaulted_GetRef();
		Diagonal.Name = TEXT("Line plane diagonal on a grade");
		Diagonal.Request = MakePlaneRequest(FIntVector(3, 1, 0));
		Diagonal.Request.Conditioning.Lines.Add(PlaneLine(FVector2D(6600.0, 1900.0), FVector2D(10200.0, 5300.0), 700.0f, 1700.0f, EBlend::Level, 1));

		// A line through the corner (7,200, 2,400) the four chunks (2..3, 0..1, 0) share.
		const FVoxelConditioningLineRef Corner = PlaneLine(FVector2D(5700.0, 3650.0), FVector2D(8700.0, 1150.0), 1250.0f, 1250.0f, EBlend::Level, 1);
		for (const FIntVector& Chunk : { FIntVector(3, 1, 0), FIntVector(2, 0, 0), FIntVector(2, 1, 0), FIntVector(3, 0, 0) })
		{
			FSite& Site = Sites.AddDefaulted_GetRef();
			Site.Name = FString::Printf(TEXT("Line plane through a chunk corner, chunk (%d, %d)"), Chunk.X, Chunk.Y);
			Site.Request = MakePlaneRequest(Chunk);
			Site.Request.Conditioning.Lines.Add(Corner);
		}

		// A lower-only and a raise-only line that cross, and a zone on top: lines in id order, then the zone.
		FSite& Blends = Sites.AddDefaulted_GetRef();
		Blends.Name = TEXT("Line plane lower-only and raise-only under a zone");
		Blends.Request = MakePlaneRequest(FIntVector(3, 1, 0));
		Blends.Request.Conditioning.Lines.Add(PlaneLine(FVector2D(6900.0, 3000.0), FVector2D(9900.0, 4300.0), 900.0f, 900.0f, EBlend::LowerOnly, 1));
		Blends.Request.Conditioning.Lines.Add(PlaneLine(FVector2D(7500.0, 5100.0), FVector2D(9300.0, 2100.0), 1500.0f, 1500.0f, EBlend::RaiseOnly, 2));
		Blends.Request.Conditioning.Zones.Add(FVoxelConditioningZone(FVector2D(PlaneOrigin.X + 8400.0, PlaneOrigin.Y + 3600.0), 300.0f, 500.0f, 1200.0f, 1.0f));
	}

	// ---- The planet: Review_Planet's size and terrain, off the world origin, biomes and a sea.
	UVoxelBiomeConfiguration* PlanetBiomes = NewObject<UVoxelBiomeConfiguration>();
	PlanetBiomes->AddToRoot();
	PlanetBiomes->InitializeDefaults();
	PlanetBiomes->bEnableContinentalness = true;
	PlanetBiomes->RebuildCaches();
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::SphericalPlanet;
	Config->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
	Config->WorldRadius = Radius;
	Config->PlanetHeightScale = 6000.0f;
	Config->BaseHeight = 0.0f;
	Config->PlanetMaxTerrainHeight = 6500.0f;
	Config->PlanetMaxTerrainDepth = 6500.0f;
	Config->VoxelSize = VoxelSize;
	Config->ChunkSize = ChunkSize;
	Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Config->NoiseParams.Seed = 248675;
	Config->NoiseParams.Octaves = 3;
	Config->NoiseParams.Frequency = 0.000025f;
	Config->NoiseParams.Lacunarity = 1.681f;
	Config->NoiseParams.Persistence = 0.5f;
	Config->BiomeConfiguration = PlanetBiomes;
	Config->bEnableBiomes = true;
	Config->bEnableCaves = false;
	Config->bEnableWaterLevel = true;
	Config->WaterRadius = WaterRadius;
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
	const FVector Origin = Config->WorldOrigin;
	auto MakePlanetRequest = [&](const FIntVector& ChunkCoord)
	{
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.ChunkSize = Config->ChunkSize;
		Request.VoxelSize = Config->VoxelSize;
		Request.NoiseParams = Config->NoiseParams;
		FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
		Request.bEnableBiomes = true;
		Request.BiomeConfiguration = Config->BiomeConfiguration;
		Request.bEnableWaterLevel = true;
		Request.WaterRadius = Config->WaterRadius;
		return Request;
	};
	auto ChunkAt = [&](const FVector& WorldPos)
	{
		const FVector Local = (WorldPos - Origin) / ChunkWorldSize;
		return FIntVector(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), FMath::FloorToInt(Local.Z));
	};
	// An arc through a direction, 2,400 uu each way along a heading given as a second direction, its target an offset
	// from the natural altitude along it at each end.
	auto PlanetLine = [&](const FVector& Through, const FVector& Toward, float OffsetA, float OffsetB, EBlend Blend)
	{
		const FVector Tangent = (Toward - Through * FVector::DotProduct(Toward, Through)).GetSafeNormal();
		const float Natural = Planet.GetTerrainAltitude(Through, Config->NoiseParams);
		FVoxelConditioningLineParams Params;
		Params.bRadial = true;
		Params.FalloffWidth = Falloff;
		Params.Blend = Blend;
		const int32 HalfSteps = 16;
		for (int32 Step = -HalfSteps; Step <= HalfSteps; ++Step)
		{
			const double Angle = Step * Spacing / Radius;
			const FVector Direction = (Through * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal();
			Params.Vertices.Add(FVoxelConditioningLineVertex(Direction, Natural + FMath::Lerp(OffsetA, OffsetB, 0.5f + 0.5f * Step / HalfSteps), HalfWidth));
		}
		return FVoxelConditioningLine::Make(Params);
	};
	{
		// Latitude 45: a line diagonal to the chunk grid, climbing.
		const FVector Dir45 = LatLonDirection(45.0, 3.0);
		FSite& Diagonal = Sites.AddDefaulted_GetRef();
		Diagonal.Name = TEXT("Line planet diagonal at latitude 45");
		const FVoxelConditioningLineRef DiagonalLine = PlanetLine(Dir45, FVector(0.3, 1.0, 0.6).GetSafeNormal(), 0.0f, 500.0f, EBlend::Level);
		Diagonal.Request = MakePlanetRequest(ChunkAt(Origin + Dir45 * (Radius + Planet.GetTerrainAltitude(Dir45, Config->NoiseParams) + 250.0f)));
		Diagonal.Request.Conditioning.Lines.Add(DiagonalLine);

		// Across a cube-face edge, lowered.
		const FVector DirEdge = LatLonDirection(0.0, 45.0);
		FSite& Edge = Sites.AddDefaulted_GetRef();
		Edge.Name = TEXT("Line planet lower-only across a cube-face edge");
		Edge.Request = MakePlanetRequest(ChunkAt(Origin + DirEdge * (Radius + Planet.GetTerrainAltitude(DirEdge, Config->NoiseParams) - 300.0f)));
		Edge.Request.Conditioning.Lines.Add(PlanetLine(DirEdge, FVector(1.0, -1.0, 0.7).GetSafeNormal(), -300.0f, -300.0f, EBlend::LowerOnly));

		// Through a chunk corner: the corner of the chunk under the surface at a site, its direction the line's middle.
		const FVector DirSite = LatLonDirection(20.0, 100.0);
		const FIntVector SiteChunk = ChunkAt(Origin + DirSite * (Radius + Planet.GetTerrainAltitude(DirSite, Config->NoiseParams)));
		const FVector Corner = Origin + FVector(SiteChunk) * ChunkWorldSize;
		const FVector DirCorner = (Corner - Origin).GetSafeNormal();
		const float CornerAltitude = static_cast<float>((Corner - Origin).Size() - Radius);
		// Level with the corner itself, so the surface passes through it in each chunk around it.
		const FVoxelConditioningLineRef CornerLine = PlanetLine(DirCorner, FVector(-1.0, 0.2, 1.0).GetSafeNormal(),
			CornerAltitude - Planet.GetTerrainAltitude(DirCorner, Config->NoiseParams), CornerAltitude - Planet.GetTerrainAltitude(DirCorner, Config->NoiseParams), EBlend::Level);
		// Of the eight chunks that share the corner, those the surface passes through: where the line moves 50 voxels
		// or more (the others hold the corner in the air or under the ground).
		int32 CornerChunks = 0;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			const FIntVector Offset(-(Index & 1), -((Index >> 1) & 1), -((Index >> 2) & 1));
			FVoxelNoiseGenerationRequest Request = MakePlanetRequest(SiteChunk + Offset);
			TArray<FVoxelData> With, Without;
			CPUGenerator.GenerateChunkCPU(Request, Without);
			Request.Conditioning.Lines.Add(CornerLine);
			CPUGenerator.GenerateChunkCPU(Request, With);
			int32 Differ = 0;
			for (int32 Voxel = 0; Voxel < With.Num() && Voxel < Without.Num(); ++Voxel)
			{
				Differ += With[Voxel].Density != Without[Voxel].Density ? 1 : 0;
			}
			if (Differ < 50)
			{
				continue;
			}
			++CornerChunks;
			FSite& Site = Sites.AddDefaulted_GetRef();
			Site.Name = FString::Printf(TEXT("Line planet through a chunk corner, chunk offset (%d, %d, %d)"), Offset.X, Offset.Y, Offset.Z);
			Site.Request = Request;
		}
		TestTrue(TEXT("The surface passes through three or more of the chunks around the planet's corner"), CornerChunks >= 3);
	}

	// Non-vacuous: the lines move each chunk's voxels on the CPU, and each chunk's segment list is not empty.
	bool bAllMoved = true;
	for (const FSite& Site : Sites)
	{
		FVoxelNoiseGenerationRequest Natural = Site.Request;
		Natural.Conditioning.Reset();
		TArray<FVoxelData> With, Without;
		CPUGenerator.GenerateChunkCPU(Site.Request, With);
		CPUGenerator.GenerateChunkCPU(Natural, Without);
		int32 Differ = 0;
		for (int32 Index = 0; Index < With.Num() && Index < Without.Num(); ++Index)
		{
			Differ += With[Index].Density != Without[Index].Density ? 1 : 0;
		}
		FVoxelChunkLineSegments Segments;
		FVoxelChunkLineSegments::Build(Site.Request, Segments);
		AddInfo(FString::Printf(TEXT("%s: chunk (%d, %d, %d), %d segments in its list, %d voxels' density moved by the conditioning"),
			*Site.Name, Site.Request.ChunkCoord.X, Site.Request.ChunkCoord.Y, Site.Request.ChunkCoord.Z, Segments.Num(), Differ));
		if (Segments.Num() == 0 || Differ < 50)
		{
			AddError(FString::Printf(TEXT("%s: the line does not move the chunk (%d segments, %d voxels)"), *Site.Name, Segments.Num(), Differ));
			bAllMoved = false;
		}
	}
	CPUGenerator.Shutdown();

	// The planet's thresholds are the spherical parity test's; the plane's 32^3 chunks take them too (a voxel is 0.003 %).
	VoxelParityHarness::FParityThresholds Thresholds;
	Thresholds.DensityClose = 99.5f;
	Thresholds.Mat = 99.0f;
	Thresholds.Biome = 99.9f;
	Thresholds.Meta = 99.9f;
	Thresholds.MatWhenDensEq = 99.5f;
	bool bAllPass = true;
	for (const FSite& Site : Sites)
	{
		if (!VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Site.Request, *Site.Name, Thresholds))
		{
			bAllPass = false;
		}
	}

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	PlanetBiomes->RemoveFromRoot();
	Config->RemoveFromRoot();
	TestTrue(TEXT("The lines move every test chunk"), bAllMoved);
	TestTrue(TEXT("GPU==CPU parity with conditioning lines, plane and planet, across LOD 0/1/2"), bAllPass);
	return true;
}
