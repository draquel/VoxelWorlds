// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelCaveConfiguration.h"
#include "RenderingThread.h"

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
	Request.NoiseParams.Amplitude = 1.0f;
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
	Request.NoiseParams.Amplitude = 1.0f;
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

		// Test that GetGeneratedBuffer returns valid buffer
		FRHIBuffer* Buffer = Generator.GetGeneratedBuffer(Handle);
		TestNotNull(TEXT("Should be able to get generated buffer"), Buffer);
	}

	// Release handle
	Generator.ReleaseHandle(Handle);

	Generator.Shutdown();

	return true;
}

// ==================== Shared GPU==CPU parity harness (Phase D) ====================
//
// The strict parity tests (InfinitePlane, IslandBowl, caves, noise types, water, ore) all follow the same
// shape: for each LOD, generate the same request on CPU and GPU, run the CPU post-passes on the GPU
// readback (as the streaming path does), then compare the full FVoxelData. This helper is that loop, so
// each scenario is just "build a Request + call RunGpuCpuParity".
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

			// Apply the same post-passes the streaming path runs on GPU readback (water + underground).
			FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(Request, GPUData);

			if (CPUData.Num() != GPUData.Num() || CPUData.Num() == 0)
			{
				Test.AddError(FString::Printf(TEXT("%s LOD%d: size mismatch/empty CPU=%d GPU=%d"), Label, LOD, CPUData.Num(), GPUData.Num()));
				bAllPass = false;
				continue;
			}

			const int32 Total = CPUData.Num();
			int32 DensityClose = 0, MatMatch = 0, BiomeMatch = 0, MetaMatch = 0;
			int32 DensityEqualCount = 0, MatMatchWhenDensEqual = 0;
			int32 Reported = 0;
			for (int32 i = 0; i < Total; ++i)
			{
				const FVoxelData& C = CPUData[i];
				const FVoxelData& G = GPUData[i];
				if (FMath::Abs((int32)C.Density - (int32)G.Density) <= 2) { DensityClose++; }
				if (C.MaterialID == G.MaterialID) { MatMatch++; }
				if (C.BiomeID == G.BiomeID) { BiomeMatch++; }
				if (C.Metadata == G.Metadata) { MetaMatch++; }
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

			Test.AddInfo(FString::Printf(TEXT("%s LOD%d [%d voxels]: density~%.1f%%, material %.1f%%, biome %.1f%%, meta %.1f%%, material(density==) %.2f%%"),
				Label, LOD, Total, DensityClosePct, MatPct, BiomePct, MetaPct, MatWhenDensEqPct));

			if (DensityClosePct < Thr.DensityClose) { Test.AddError(FString::Printf(TEXT("%s LOD%d density parity %.1f%% < %.1f%%"), Label, LOD, DensityClosePct, Thr.DensityClose)); bAllPass = false; }
			if (MatPct < Thr.Mat) { Test.AddError(FString::Printf(TEXT("%s LOD%d material parity %.1f%% < %.1f%%"), Label, LOD, MatPct, Thr.Mat)); bAllPass = false; }
			if (BiomePct < Thr.Biome) { Test.AddError(FString::Printf(TEXT("%s LOD%d biome parity %.1f%% < %.1f%%"), Label, LOD, BiomePct, Thr.Biome)); bAllPass = false; }
			if (Thr.bCheckMeta && MetaPct < Thr.Meta) { Test.AddError(FString::Printf(TEXT("%s LOD%d metadata parity %.1f%% < %.1f%%"), Label, LOD, MetaPct, Thr.Meta)); bAllPass = false; }
			if (MatWhenDensEqPct < Thr.MatWhenDensEq) { Test.AddError(FString::Printf(TEXT("%s LOD%d material(density==) %.2f%% < %.2f%%"), Label, LOD, MatWhenDensEqPct, Thr.MatWhenDensEq)); bAllPass = false; }
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
	Request.NoiseParams.Amplitude = 1.0f;
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
	Request.ConditioningZones.Add(FVoxelConditioningZone(FVector2D(4800.0, 1600.0), 800.0f, 1200.0f, 500.0f, 1.0f));

	// Density near-identical (Phase A). Material/biome/meta exact except at density boundaries; when the
	// density is identical, material must match almost exactly (isolates logic errors from FP boundaries).
	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("InfinitePlane"));

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU material/biome/metadata parity across LOD 0/1/2"), bAllPass);
	return true;
}

// Phase C: GPU==CPU parity for IslandBowl mode (continentalness-modulated height + edge falloff), with
// biomes, at LOD 0/1/2. Verifies the shared heightmap path + the island falloff on the GPU match CPU.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUIslandBowlParityTest, "VoxelWorlds.Generation.GPUvsCPUIslandBowlParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUIslandBowlParityTest::RunTest(const FString& Parameters)
{
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
	Request.NoiseParams.Amplitude = 1.0f;
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
	Request.IslandParams.Shape = 0;
	Request.IslandParams.IslandRadius = 3500.0f;
	Request.IslandParams.SizeY = 3500.0f;
	Request.IslandParams.FalloffWidth = 1500.0f;
	Request.IslandParams.FalloffType = 1; // Smooth
	Request.IslandParams.CenterX = 0.0f;
	Request.IslandParams.CenterY = 0.0f;
	Request.IslandParams.EdgeHeight = -1000.0f;

	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("IslandBowl"));

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU IslandBowl parity (continentalness + falloff) across LOD 0/1/2"), bAllPass);
	return true;
}

// Phase D: GPU==CPU parity for cave carving (cheese + tunnel layers + cave-wall material override), at
// LOD 0/1/2. Verifies the GPU cave block (GenerateVoxelDensity.usf + CaveGeneration.ush) matches the CPU
// CalculateCaveDensity/SampleCaveLayer path. The config uses NO biome cave overrides and no underwater
// suppression (GetBiomeCaveScale==1.0, no min-depth override) — that is exactly the core carving path the
// GPU shader implements; per-biome scale + underwater min-depth are a documented GPU gap, not tested here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUCavesParityTest, "VoxelWorlds.Generation.GPUvsCPUCavesParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUCavesParityTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->BuildGpuData();

	// Global cave config: one cheese + one tunnel (spaghetti) layer, cave-wall override on. No biome
	// overrides / underwater suppression → CPU reduces to the shader's core carving path.
	UVoxelCaveConfiguration* CaveConfig = NewObject<UVoxelCaveConfiguration>();
	CaveConfig->AddToRoot();
	CaveConfig->bEnableCaves = true;
	CaveConfig->BiomeOverrides.Empty();     // GetBiomeCaveScale() == 1.0, GetBiomeMinDepthOverride() == -1
	CaveConfig->UnderwaterMinDepth = 0.0f;  // no underwater min-depth (shader doesn't implement it)
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
	Request.NoiseParams.Amplitude = 1.0f;
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
	{
		FVoxelNoiseGenerationRequest Probe = Request; Probe.LODLevel = 0;
		TArray<FVoxelData> ProbeData;
		GPUGenerator.GenerateChunkCPU(Probe, ProbeData);
		int32 UndergroundAir = 0;
		for (const FVoxelData& V : ProbeData)
		{
			if (V.IsAir() && V.HasUndergroundFlag()) { UndergroundAir++; }
		}
		AddInfo(FString::Printf(TEXT("Caves probe: %d underground-air (cave) voxels of %d"), UndergroundAir, ProbeData.Num()));
		TestTrue(TEXT("Cave config should carve some voxels (non-vacuous test)"), UndergroundAir > 0);
	}

	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Caves"));

	GPUGenerator.Shutdown();
	CaveConfig->RemoveFromRoot();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU caves parity (cheese + tunnel + cave-wall) across LOD 0/1/2"), bAllPass);
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
		Request.NoiseParams.Amplitude = 1.0f;
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
	Request.NoiseParams.Amplitude = 1.0f;
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

	const bool bAllPass = VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, Request, TEXT("Water"));

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU water parity (underwater materials + water fill) across LOD 0/1/2"), bAllPass);
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
	Request.NoiseParams.Amplitude = 1.0f;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNoiseGeneratorPerformanceTest, "VoxelWorlds.Generation.Performance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::MediumPriority)

bool FVoxelNoiseGeneratorPerformanceTest::RunTest(const FString& Parameters)
{
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
	Request.NoiseParams.Amplitude = 1.0f;
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
