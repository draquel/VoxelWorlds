// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
#include "RenderingThread.h"

// ==================== Basic Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInfinitePlaneWorldModeBasicTest, "VoxelWorlds.WorldModes.InfinitePlane.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInfinitePlaneWorldModeBasicTest::RunTest(const FString& Parameters)
{
	// Test default construction
	FInfinitePlaneWorldMode DefaultMode;
	TestEqual(TEXT("Default world mode type"), DefaultMode.GetWorldModeType(), EWorldMode::InfinitePlane);
	TestTrue(TEXT("Should be heightmap based"), DefaultMode.IsHeightmapBased());

	// Test terrain params construction
	FWorldModeTerrainParams Params(1000.0f, 5000.0f, 500.0f);
	FInfinitePlaneWorldMode CustomMode(Params);
	TestEqual(TEXT("Custom sea level"), CustomMode.GetSeaLevel(), 1000.0f);
	TestEqual(TEXT("Custom height scale"), CustomMode.GetHeightScale(), 5000.0f);
	TestEqual(TEXT("Custom base height"), CustomMode.GetBaseHeight(), 500.0f);

	// Test vertical bounds
	TestTrue(TEXT("MinZ should be negative"), DefaultMode.GetMinZ() < 0);
	TestTrue(TEXT("MaxZ should be positive"), DefaultMode.GetMaxZ() > 0);
	TestTrue(TEXT("MinZ < MaxZ"), DefaultMode.GetMinZ() < DefaultMode.GetMaxZ());

	return true;
}

// ==================== Density Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInfinitePlaneWorldModeDensityTest, "VoxelWorlds.WorldModes.InfinitePlane.Density",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInfinitePlaneWorldModeDensityTest::RunTest(const FString& Parameters)
{
	FWorldModeTerrainParams Params(0.0f, 5000.0f, 0.0f);
	FInfinitePlaneWorldMode WorldMode(Params);

	const float VoxelSize = 100.0f;

	// Test: Position well below terrain should be solid
	{
		// NoiseValue = 0 -> TerrainHeight = 0 (at sea level)
		// WorldZ = -1000 -> below terrain
		FVector BelowPos(500.0f, 500.0f, -1000.0f);
		float Density = WorldMode.GetDensityAt(BelowPos, 0, 0.0f);
		TestTrue(TEXT("Below terrain should have positive density (solid)"), Density > 0.0f);
	}

	// Test: Position well above terrain should be air
	{
		// NoiseValue = 0 -> TerrainHeight = 0
		// WorldZ = 1000 -> above terrain
		FVector AbovePos(500.0f, 500.0f, 1000.0f);
		float Density = WorldMode.GetDensityAt(AbovePos, 0, 0.0f);
		TestTrue(TEXT("Above terrain should have negative density (air)"), Density < 0.0f);
	}

	// Test: Position at terrain surface
	{
		// NoiseValue = 0 -> TerrainHeight = 0
		// WorldZ = 0 -> at surface
		FVector SurfacePos(500.0f, 500.0f, 0.0f);
		float Density = WorldMode.GetDensityAt(SurfacePos, 0, 0.0f);
		TestTrue(TEXT("At surface should have near-zero density"), FMath::Abs(Density) < 0.1f);
	}

	// Test signed distance to density conversion
	{
		// Positive signed distance (solid)
		uint8 SolidDensity = FInfinitePlaneWorldMode::SignedDistanceToDensity(VoxelSize, VoxelSize);
		TestTrue(TEXT("Solid density should be >= 127"), SolidDensity >= 127);

		// Negative signed distance (air)
		uint8 AirDensity = FInfinitePlaneWorldMode::SignedDistanceToDensity(-VoxelSize, VoxelSize);
		TestTrue(TEXT("Air density should be < 127"), AirDensity < 127);

		// Zero signed distance (surface)
		uint8 SurfaceDensity = FInfinitePlaneWorldMode::SignedDistanceToDensity(0.0f, VoxelSize);
		TestTrue(TEXT("Surface density should be near 127"), FMath::Abs(static_cast<int32>(SurfaceDensity) - 127) <= 1);
	}

	// Test terrain height calculation
	{
		// With HeightScale = 5000, noise of 0.5 should give height of 2500
		float Height = FInfinitePlaneWorldMode::NoiseToTerrainHeight(0.5f, Params);
		TestTrue(TEXT("Terrain height calculation"), FMath::IsNearlyEqual(Height, 2500.0f, 1.0f));

		// Noise of -0.5 should give height of -2500
		float NegHeight = FInfinitePlaneWorldMode::NoiseToTerrainHeight(-0.5f, Params);
		TestTrue(TEXT("Negative terrain height calculation"), FMath::IsNearlyEqual(NegHeight, -2500.0f, 1.0f));
	}

	return true;
}

// ==================== CPU Generation Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInfinitePlaneWorldModeCPUGenerationTest, "VoxelWorlds.WorldModes.InfinitePlane.CPUGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInfinitePlaneWorldModeCPUGenerationTest::RunTest(const FString& Parameters)
{
	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();

	// Create request with InfinitePlane mode
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(0, 0, 0);
	Request.ChunkSize = 16;
	Request.VoxelSize = 100.0f;
	Request.LODLevel = 0;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Perlin;
	Request.NoiseParams.Seed = 12345;
	Request.NoiseParams.Frequency = 0.001f;
	Request.NoiseParams.Amplitude = 1.0f;
	Request.NoiseParams.Octaves = 4;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;

	// Set world mode parameters
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 5000.0f;
	Request.BaseHeight = 0.0f;

	// Generate chunk
	TArray<FVoxelData> VoxelData;
	bool bSuccess = Generator.GenerateChunkCPU(Request, VoxelData);

	TestTrue(TEXT("CPU generation should succeed"), bSuccess);

	int32 ExpectedSize = Request.ChunkSize * Request.ChunkSize * Request.ChunkSize;
	TestEqual(TEXT("Output should have correct number of voxels"), VoxelData.Num(), ExpectedSize);

	// Analyze the generated data
	int32 SolidCount = 0;
	int32 AirCount = 0;
	int32 SurfaceCount = 0;

	for (const FVoxelData& Voxel : VoxelData)
	{
		if (Voxel.IsSolid())
		{
			SolidCount++;
		}
		else
		{
			AirCount++;
		}
		if (FMath::Abs(static_cast<int32>(Voxel.Density) - 127) <= 10)
		{
			SurfaceCount++;
		}
	}

	// For a chunk around sea level (Z=0), we should have a mix of solid and air
	TestTrue(TEXT("Should have some solid voxels"), SolidCount > 0);
	TestTrue(TEXT("Should have some air voxels"), AirCount > 0);

	AddInfo(FString::Printf(TEXT("InfinitePlane CPU Generation: %d solid, %d air, %d near-surface"),
		SolidCount, AirCount, SurfaceCount));

	// Test chunk above sea level (should be mostly air)
	Request.ChunkCoord = FIntVector(0, 0, 5); // Z=5 means chunk starts at Z = 5 * 16 * 100 = 8000
	TArray<FVoxelData> HighChunkData;
	Generator.GenerateChunkCPU(Request, HighChunkData);

	int32 HighAirCount = 0;
	for (const FVoxelData& Voxel : HighChunkData)
	{
		if (Voxel.IsAir())
		{
			HighAirCount++;
		}
	}

	float HighAirPercent = static_cast<float>(HighAirCount) / HighChunkData.Num() * 100.0f;
	TestTrue(TEXT("High chunk should be mostly air (>90%)"), HighAirPercent > 90.0f);
	AddInfo(FString::Printf(TEXT("High chunk (Z=5): %.1f%% air"), HighAirPercent));

	// Test chunk below sea level (should be mostly solid)
	Request.ChunkCoord = FIntVector(0, 0, -5); // Z=-5 means chunk starts at Z = -8000
	TArray<FVoxelData> LowChunkData;
	Generator.GenerateChunkCPU(Request, LowChunkData);

	int32 LowSolidCount = 0;
	for (const FVoxelData& Voxel : LowChunkData)
	{
		if (Voxel.IsSolid())
		{
			LowSolidCount++;
		}
	}

	float LowSolidPercent = static_cast<float>(LowSolidCount) / LowChunkData.Num() * 100.0f;
	TestTrue(TEXT("Low chunk should be mostly solid (>90%)"), LowSolidPercent > 90.0f);
	AddInfo(FString::Printf(TEXT("Low chunk (Z=-5): %.1f%% solid"), LowSolidPercent));

	Generator.Shutdown();
	return true;
}

// ==================== Coordinate Transform Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInfinitePlaneWorldModeCoordinatesTest, "VoxelWorlds.WorldModes.InfinitePlane.Coordinates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInfinitePlaneWorldModeCoordinatesTest::RunTest(const FString& Parameters)
{
	FInfinitePlaneWorldMode WorldMode;

	const int32 ChunkSize = 32;
	const float VoxelSize = 100.0f;
	const float ChunkWorldSize = ChunkSize * VoxelSize; // 3200

	// Test WorldToChunkCoord
	{
		// Position in chunk (0,0,0)
		FVector Pos1(100.0f, 200.0f, 300.0f);
		FIntVector Chunk1 = WorldMode.WorldToChunkCoord(Pos1, ChunkSize, VoxelSize);
		TestEqual(TEXT("Position (100,200,300) should be in chunk (0,0,0)"), Chunk1, FIntVector(0, 0, 0));

		// Position in chunk (1,0,0)
		FVector Pos2(3500.0f, 200.0f, 300.0f);
		FIntVector Chunk2 = WorldMode.WorldToChunkCoord(Pos2, ChunkSize, VoxelSize);
		TestEqual(TEXT("Position (3500,200,300) should be in chunk (1,0,0)"), Chunk2, FIntVector(1, 0, 0));

		// Negative position
		FVector Pos3(-100.0f, -200.0f, -300.0f);
		FIntVector Chunk3 = WorldMode.WorldToChunkCoord(Pos3, ChunkSize, VoxelSize);
		TestEqual(TEXT("Position (-100,-200,-300) should be in chunk (-1,-1,-1)"), Chunk3, FIntVector(-1, -1, -1));
	}

	// Test ChunkCoordToWorld
	{
		// Chunk (0,0,0) at LOD 0
		FVector World1 = WorldMode.ChunkCoordToWorld(FIntVector(0, 0, 0), ChunkSize, VoxelSize, 0);
		TestTrue(TEXT("Chunk (0,0,0) origin should be at world (0,0,0)"), World1.IsNearlyZero());

		// Chunk (1,2,3) at LOD 0
		FVector World2 = WorldMode.ChunkCoordToWorld(FIntVector(1, 2, 3), ChunkSize, VoxelSize, 0);
		FVector Expected2(ChunkWorldSize, ChunkWorldSize * 2, ChunkWorldSize * 3);
		TestTrue(TEXT("Chunk (1,2,3) origin calculation"), World2.Equals(Expected2, 0.1f));

		// Test LOD scaling (LOD 1 = 2x chunk size)
		FVector World3 = WorldMode.ChunkCoordToWorld(FIntVector(1, 0, 0), ChunkSize, VoxelSize, 1);
		FVector Expected3(ChunkWorldSize * 2, 0, 0); // LOD 1 doubles the effective size
		TestTrue(TEXT("Chunk (1,0,0) at LOD 1 should be at (6400,0,0)"), World3.Equals(Expected3, 0.1f));
	}

	// Test round-trip conversion
	{
		FVector OriginalPos(5000.0f, 7500.0f, 2000.0f);
		FIntVector ChunkCoord = WorldMode.WorldToChunkCoord(OriginalPos, ChunkSize, VoxelSize);
		FVector ChunkOrigin = WorldMode.ChunkCoordToWorld(ChunkCoord, ChunkSize, VoxelSize, 0);

		// The original position should be within the chunk whose origin we calculated
		TestTrue(TEXT("Round-trip X"), OriginalPos.X >= ChunkOrigin.X && OriginalPos.X < ChunkOrigin.X + ChunkWorldSize);
		TestTrue(TEXT("Round-trip Y"), OriginalPos.Y >= ChunkOrigin.Y && OriginalPos.Y < ChunkOrigin.Y + ChunkWorldSize);
		TestTrue(TEXT("Round-trip Z"), OriginalPos.Z >= ChunkOrigin.Z && OriginalPos.Z < ChunkOrigin.Z + ChunkWorldSize);
	}

	return true;
}

// ==================== GPU Consistency Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInfinitePlaneWorldModeGPUConsistencyTest, "VoxelWorlds.WorldModes.InfinitePlane.GPUConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInfinitePlaneWorldModeGPUConsistencyTest::RunTest(const FString& Parameters)
{
	FVoxelGPUNoiseGenerator GPUGenerator;
	GPUGenerator.Initialize();

	// Create request with InfinitePlane mode
	FVoxelNoiseGenerationRequest Request;
	Request.ChunkCoord = FIntVector(0, 0, 0);
	Request.ChunkSize = 8; // Small for quick test
	Request.VoxelSize = 100.0f;
	Request.LODLevel = 0;
	Request.NoiseParams.NoiseType = EVoxelNoiseType::Perlin;
	Request.NoiseParams.Seed = 42;
	Request.NoiseParams.Frequency = 0.001f;
	Request.NoiseParams.Amplitude = 1.0f;
	Request.NoiseParams.Octaves = 2;
	Request.NoiseParams.Lacunarity = 2.0f;
	Request.NoiseParams.Persistence = 0.5f;

	// Set world mode parameters
	Request.WorldMode = EWorldMode::InfinitePlane;
	Request.SeaLevel = 0.0f;
	Request.HeightScale = 5000.0f;
	Request.BaseHeight = 0.0f;

	// Generate on CPU
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
		// Compare values
		int32 MatchCount = 0;
		int32 CloseCount = 0;
		const int32 DensityTolerance = 10; // Allow variance due to FP precision

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

		float MatchPercent = static_cast<float>(MatchCount + CloseCount) / CPUData.Num() * 100.0f;

		AddInfo(FString::Printf(TEXT("InfinitePlane CPU vs GPU: %d exact, %d close (%.1f%% within tolerance)"),
			MatchCount, CloseCount, MatchPercent));

		// Target: at least 85% should match or be close
		TestTrue(TEXT("CPU and GPU results should be similar (85%+ within tolerance)"), MatchPercent >= 85.0f);
	}

	GPUGenerator.ReleaseHandle(Handle);
	GPUGenerator.Shutdown();

	return true;
}

// ==================== Material Assignment Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInfinitePlaneWorldModeMaterialTest, "VoxelWorlds.WorldModes.InfinitePlane.Materials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInfinitePlaneWorldModeMaterialTest::RunTest(const FString& Parameters)
{
	FInfinitePlaneWorldMode WorldMode;

	const float SurfaceHeight = 0.0f;
	FVector TestPos(0.0f, 0.0f, 0.0f);

	// Test material at surface (grass)
	uint8 SurfaceMaterial = WorldMode.GetMaterialAtDepth(TestPos, SurfaceHeight, 50.0f);
	TestEqual(TEXT("Near surface should be grass (0)"), SurfaceMaterial, 0);

	// Test material below surface (dirt)
	uint8 DirtMaterial = WorldMode.GetMaterialAtDepth(TestPos, SurfaceHeight, 200.0f);
	TestEqual(TEXT("Shallow depth should be dirt (1)"), DirtMaterial, 1);

	// Test material deep underground (stone)
	uint8 StoneMaterial = WorldMode.GetMaterialAtDepth(TestPos, SurfaceHeight, 500.0f);
	TestEqual(TEXT("Deep underground should be stone (2)"), StoneMaterial, 2);

	// Test air (above surface)
	uint8 AirMaterial = WorldMode.GetMaterialAtDepth(TestPos, SurfaceHeight, -100.0f);
	TestEqual(TEXT("Above surface should be air material (0)"), AirMaterial, 0);

	return true;
}
