// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "IVoxelWorldMode.h"
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

// Spherical Phase 2 (SP-B4, SP-I3; WORLD_MODES_REVIEW_PLAN.md §16): GPU==CPU parity for a planet, the full FVoxelData
// at LOD 0/1/2, on a planet like Review_Planet (R 250,000, the demo's noise, continentalness, biomes with underwater
// materials, a height rule by altitude, an ore, caves with biome overrides and the underwater minimum depth, water at
// WaterRadius 500 under the datum), off the world origin like the demo. The params come from FVoxelWorldModeFactory, as
// the chunk manager's requests do. Sites: both poles, the equator, 45 degrees, a cube-face edge, a cube corner (each the
// chunk the surface passes through), a coast (the sea surface and the seabed in one chunk), a cave (carved air under the
// ground) and caves under the seabed that the underwater minimum depth suppresses. The planet's post-passes (the radial
// water fill, the underground scan along local down) run on the GPU in the same graph, so the metadata channel compares
// them with the CPU's.
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
		if (!VoxelParityHarness::RunGpuCpuParity(*this, GPUGenerator, MakeRequest(Site.Chunk), *Label, PlanetThresholds))
		{
			bAllPass = false;
		}
	}
	GPUGenerator.Shutdown();

	Config->RemoveFromRoot();
	CaveConfig->RemoveFromRoot();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU planet parity (radial height, materials, caves, water, underground) at every site across LOD 0/1/2"), bAllPass);
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
