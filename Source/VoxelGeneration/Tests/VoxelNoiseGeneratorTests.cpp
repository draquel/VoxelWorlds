// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
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
