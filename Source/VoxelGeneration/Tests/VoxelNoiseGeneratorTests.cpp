// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
#include "VoxelBiomeConfiguration.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelGPUvsCPUConsistencyTest, "VoxelWorlds.Generation.GPUvsCPUConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelGPUvsCPUConsistencyTest::RunTest(const FString& Parameters)
{
	// This test verifies that GPU and CPU generators produce similar results
	// They may not be bit-exact due to floating point differences, but should be close

	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	// Create generation request with frequency that avoids integer grid positions
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(0, 0, 0);
	Request.ChunkSize = 8; // Small chunk for quick comparison
	Request.VoxelSize = 100.0f;
	Request.LODLevel = 0;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Perlin;
	Request.NoiseParams.Seed = 42;
	Request.NoiseParams.Frequency = 0.013f; // Avoids integer grid positions
	Request.NoiseParams.Amplitude = 1.0f;
	Request.NoiseParams.Octaves = 2;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;

	// Generate on CPU (GPU generator has CPU fallback)
	TArray<FVoxelData> CPUData;
	bool bCPUSuccess = GPUGenerator.GenerateChunkCPU(Request, CPUData);
	TestTrue(TEXT("CPU generation should succeed"), bCPUSuccess);

	// Generate on GPU
	volatile bool bCompleted = false;
	FVoxelGenerationHandle Handle = GPUGenerator.GenerateChunkAsync(Request,
		FOnVoxelGenerationComplete::CreateLambda([&bCompleted](FVoxelGenerationHandle, bool)
		{
			bCompleted = true;
		}));

	// Wait for completion
	double StartTime = FPlatformTime::Seconds();
	while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < 5.0)
	{
		FPlatformProcess::Sleep(0.01f);
		FlushRenderingCommands();
	}

	TestTrue(TEXT("GPU generation should complete"), bCompleted);

	// Readback GPU data
	TArray<FVoxelData> GPUData;
	bool bReadbackSuccess = GPUGenerator.ReadbackToCPU(Handle, GPUData);
	TestTrue(TEXT("GPU readback should succeed"), bReadbackSuccess);

	// Compare sizes
	TestEqual(TEXT("CPU and GPU should produce same number of voxels"), CPUData.Num(), GPUData.Num());

	if (CPUData.Num() == GPUData.Num())
	{
		// Compare values - allow for some floating point variance
		int32 MatchCount = 0;
		int32 CloseCount = 0;
		const int32 DensityTolerance = 5; // Allow 5 units of variance due to FP precision

		for (int32 i = 0; i < CPUData.Num(); ++i)
		{
			int32 CPUDensity = CPUData[i].Density;
			int32 GPUDensity = GPUData[i].Density;

			if (CPUDensity == GPUDensity)
			{
				MatchCount++;
			}
			else if (FMath::Abs(CPUDensity - GPUDensity) <= DensityTolerance)
			{
				CloseCount++;
			}
		}

		float MatchPercent = (float)(MatchCount + CloseCount) / CPUData.Num() * 100.0f;

		AddInfo(FString::Printf(TEXT("CPU vs GPU: %d exact matches, %d close matches (%.1f%% within tolerance)"),
			MatchCount, CloseCount, MatchPercent));

		// At least 90% should match or be close
		TestTrue(TEXT("CPU and GPU results should be similar (90%+ within tolerance)"), MatchPercent >= 90.0f);
	}

	GPUGenerator.ReleaseHandle(Handle);
	GPUGenerator.Shutdown();

	return true;
}

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

	bool bAllPass = true;
	const int32 LODs[3] = { 0, 1, 2 };
	for (int32 li = 0; li < 3; ++li)
	{
		Request.LODLevel = LODs[li];

		TArray<FVoxelData> CPUData;
		GPUGenerator.GenerateChunkCPU(Request, CPUData); // CPU fallback: includes water + underground passes

		volatile bool bCompleted = false;
		FVoxelGenerationHandle Handle = GPUGenerator.GenerateChunkAsync(Request,
			FOnVoxelGenerationComplete::CreateLambda([&bCompleted](FVoxelGenerationHandle, bool) { bCompleted = true; }));
		const double StartTime = FPlatformTime::Seconds();
		while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < 10.0)
		{
			FPlatformProcess::Sleep(0.01f);
			FlushRenderingCommands();
		}
		TArray<FVoxelData> GPUData;
		GPUGenerator.ReadbackToCPU(Handle, GPUData);
		GPUGenerator.ReleaseHandle(Handle);

		// Apply the same post-passes the streaming path runs on GPU readback (water + underground).
		FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(Request, GPUData);

		if (CPUData.Num() != GPUData.Num() || CPUData.Num() == 0)
		{
			AddError(FString::Printf(TEXT("LOD%d: size mismatch/empty CPU=%d GPU=%d"), LODs[li], CPUData.Num(), GPUData.Num()));
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
			if ((C.MaterialID != G.MaterialID || C.BiomeID != G.BiomeID) && Reported < 8)
			{
				AddInfo(FString::Printf(TEXT("LOD%d mismatch @%d: dens C=%d G=%d, mat C=%d G=%d, biome C=%d G=%d, meta C=%d G=%d"),
					LODs[li], i, C.Density, G.Density, C.MaterialID, G.MaterialID, C.BiomeID, G.BiomeID, C.Metadata, G.Metadata));
				Reported++;
			}
		}

		const float DensityClosePct = 100.0f * DensityClose / Total;
		const float MatPct = 100.0f * MatMatch / Total;
		const float BiomePct = 100.0f * BiomeMatch / Total;
		const float MetaPct = 100.0f * MetaMatch / Total;
		const float MatWhenDensEqPct = DensityEqualCount > 0 ? 100.0f * MatMatchWhenDensEqual / DensityEqualCount : 100.0f;

		AddInfo(FString::Printf(TEXT("LOD%d [%d voxels]: density~%.1f%%, material %.1f%%, biome %.1f%%, meta %.1f%%, material(density==) %.2f%%"),
			LODs[li], Total, DensityClosePct, MatPct, BiomePct, MetaPct, MatWhenDensEqPct));

		// Density near-identical (Phase A). Material/biome/meta exact except at density boundaries; when the
		// density is identical, material must match almost exactly (isolates logic errors from FP boundaries).
		if (DensityClosePct < 99.0f) { AddError(FString::Printf(TEXT("LOD%d density parity %.1f%% < 99%%"), LODs[li], DensityClosePct)); bAllPass = false; }
		if (MatPct < 95.0f) { AddError(FString::Printf(TEXT("LOD%d material parity %.1f%% < 95%%"), LODs[li], MatPct)); bAllPass = false; }
		if (BiomePct < 98.0f) { AddError(FString::Printf(TEXT("LOD%d biome parity %.1f%% < 98%%"), LODs[li], BiomePct)); bAllPass = false; }
		if (MetaPct < 98.0f) { AddError(FString::Printf(TEXT("LOD%d metadata parity %.1f%% < 98%%"), LODs[li], MetaPct)); bAllPass = false; }
		if (MatWhenDensEqPct < 99.0f) { AddError(FString::Printf(TEXT("LOD%d material(density==) %.2f%% < 99%%"), LODs[li], MatWhenDensEqPct)); bAllPass = false; }
	}

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

	bool bAllPass = true;
	const int32 LODs[3] = { 0, 1, 2 };
	for (int32 li = 0; li < 3; ++li)
	{
		Request.LODLevel = LODs[li];

		TArray<FVoxelData> CPUData;
		GPUGenerator.GenerateChunkCPU(Request, CPUData);

		volatile bool bCompleted = false;
		FVoxelGenerationHandle Handle = GPUGenerator.GenerateChunkAsync(Request,
			FOnVoxelGenerationComplete::CreateLambda([&bCompleted](FVoxelGenerationHandle, bool) { bCompleted = true; }));
		const double StartTime = FPlatformTime::Seconds();
		while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < 10.0)
		{
			FPlatformProcess::Sleep(0.01f);
			FlushRenderingCommands();
		}
		TArray<FVoxelData> GPUData;
		GPUGenerator.ReadbackToCPU(Handle, GPUData);
		GPUGenerator.ReleaseHandle(Handle);
		FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(Request, GPUData);

		if (CPUData.Num() != GPUData.Num() || CPUData.Num() == 0)
		{
			AddError(FString::Printf(TEXT("IslandBowl LOD%d: size mismatch/empty CPU=%d GPU=%d"), LODs[li], CPUData.Num(), GPUData.Num()));
			bAllPass = false;
			continue;
		}

		const int32 Total = CPUData.Num();
		int32 DensityClose = 0, MatMatch = 0, BiomeMatch = 0, DensityEq = 0, MatWhenEq = 0;
		for (int32 i = 0; i < Total; ++i)
		{
			const FVoxelData& C = CPUData[i];
			const FVoxelData& G = GPUData[i];
			if (FMath::Abs((int32)C.Density - (int32)G.Density) <= 2) { DensityClose++; }
			if (C.MaterialID == G.MaterialID) { MatMatch++; }
			if (C.BiomeID == G.BiomeID) { BiomeMatch++; }
			if (C.Density == G.Density) { DensityEq++; if (C.MaterialID == G.MaterialID) { MatWhenEq++; } }
		}
		const float DC = 100.0f * DensityClose / Total;
		const float MP = 100.0f * MatMatch / Total;
		const float BP = 100.0f * BiomeMatch / Total;
		const float MWE = DensityEq > 0 ? 100.0f * MatWhenEq / DensityEq : 100.0f;
		AddInfo(FString::Printf(TEXT("IslandBowl LOD%d [%d]: density~%.1f%%, material %.1f%%, biome %.1f%%, material(density==) %.2f%%"),
			LODs[li], Total, DC, MP, BP, MWE));

		if (DC < 99.0f) { AddError(FString::Printf(TEXT("IslandBowl LOD%d density %.1f%% < 99%%"), LODs[li], DC)); bAllPass = false; }
		if (MP < 95.0f) { AddError(FString::Printf(TEXT("IslandBowl LOD%d material %.1f%% < 95%%"), LODs[li], MP)); bAllPass = false; }
		if (BP < 98.0f) { AddError(FString::Printf(TEXT("IslandBowl LOD%d biome %.1f%% < 98%%"), LODs[li], BP)); bAllPass = false; }
		if (MWE < 99.0f) { AddError(FString::Printf(TEXT("IslandBowl LOD%d material(density==) %.2f%% < 99%%"), LODs[li], MWE)); bAllPass = false; }
	}

	GPUGenerator.Shutdown();
	BiomeConfig->RemoveFromRoot();
	TestTrue(TEXT("GPU==CPU IslandBowl parity (continentalness + falloff) across LOD 0/1/2"), bAllPass);
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
