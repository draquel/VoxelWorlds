// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelGPUMarchingCubesMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"
#include "RenderingThread.h"

// ==================== Helper Functions ====================

namespace MarchingCubesMeshingTestHelpers
{
	/**
	 * Create a meshing request with all air voxels.
	 */
	FVoxelMeshingRequest CreateMCEmptyRequest(int32 ChunkSize = 16)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = 0;

		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		Request.VoxelData.SetNum(TotalVoxels);

		for (int32 i = 0; i < TotalVoxels; ++i)
		{
			Request.VoxelData[i] = FVoxelData::Air();
		}

		return Request;
	}

	/**
	 * Create a meshing request with all solid voxels (density = 255).
	 */
	FVoxelMeshingRequest CreateSolidChunkRequest(int32 ChunkSize = 16)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = 0;

		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		Request.VoxelData.SetNum(TotalVoxels);

		for (int32 i = 0; i < TotalVoxels; ++i)
		{
			Request.VoxelData[i] = FVoxelData::Solid(1);
		}

		return Request;
	}

	/**
	 * Create a meshing request with half solid (lower half) terrain.
	 * Creates a horizontal plane at Z = ChunkSize/2.
	 */
	FVoxelMeshingRequest CreateHalfSolidRequest(int32 ChunkSize = 16)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = 0;

		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		Request.VoxelData.SetNum(TotalVoxels);

		// Lower half solid (Z < ChunkSize/2), upper half air
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
					if (Z < ChunkSize / 2)
					{
						Request.VoxelData[Index] = FVoxelData::Solid(1);
					}
					else
					{
						Request.VoxelData[Index] = FVoxelData::Air();
					}
				}
			}
		}

		return Request;
	}

	/**
	 * Create a meshing request with a sphere SDF.
	 * Density is based on distance from center, with gradient at surface.
	 */
	FVoxelMeshingRequest CreateSphereSdfRequest(int32 ChunkSize = 16, float Radius = 5.0f)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = 0;

		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		Request.VoxelData.SetNum(TotalVoxels);

		const float CenterX = ChunkSize * 0.5f;
		const float CenterY = ChunkSize * 0.5f;
		const float CenterZ = ChunkSize * 0.5f;

		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;

					// Calculate distance from center
					const float DX = X - CenterX;
					const float DY = Y - CenterY;
					const float DZ = Z - CenterZ;
					const float Distance = FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);

					// Create smooth density gradient
					// Inside sphere: density = 255, outside: density = 0
					// Gradient over 1 voxel at surface
					float NormalizedDensity;
					if (Distance <= Radius - 0.5f)
					{
						NormalizedDensity = 1.0f;
					}
					else if (Distance >= Radius + 0.5f)
					{
						NormalizedDensity = 0.0f;
					}
					else
					{
						// Linear interpolation at surface
						NormalizedDensity = 0.5f - (Distance - Radius);
					}

					FVoxelData Voxel;
					Voxel.Density = static_cast<uint8>(FMath::Clamp(NormalizedDensity * 255.0f, 0.0f, 255.0f));
					Voxel.MaterialID = 1;
					Voxel.BiomeID = 0;
					Voxel.Metadata = 0;
					Request.VoxelData[Index] = Voxel;
				}
			}
		}

		return Request;
	}

	/**
	 * Create a meshing request with terrain for boundary testing.
	 * Solid voxels at the +X edge to test neighbor data handling.
	 */
	FVoxelMeshingRequest CreateBoundaryTestRequest(int32 ChunkSize = 8)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = 0;

		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		Request.VoxelData.SetNum(TotalVoxels);

		// Solid voxels only at X = ChunkSize - 1 edge
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
					if (X == ChunkSize - 1)
					{
						Request.VoxelData[Index] = FVoxelData::Solid(1);
					}
					else
					{
						Request.VoxelData[Index] = FVoxelData::Air();
					}
				}
			}
		}

		return Request;
	}

	/**
	 * Configure a mesher for smooth meshing.
	 */
	FVoxelMeshingConfig CreateMCConfig()
	{
		FVoxelMeshingConfig Config;
		Config.bUseSmoothMeshing = true;
		Config.IsoLevel = 0.5f;
		Config.bGenerateUVs = true;
		Config.bCalculateAO = false;  // MarchingCubes meshing doesn't use AO
		return Config;
	}
} // namespace MarchingCubesMeshingTestHelpers
using namespace MarchingCubesMeshingTestHelpers;

// ==================== CPU MarchingCubes Mesher Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingEmptyChunkTest, "VoxelWorlds.Meshing.MarchingCubes.EmptyChunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesMeshingEmptyChunkTest::RunTest(const FString& Parameters)
{
	FVoxelCPUMarchingCubesMesher Mesher;
	Mesher.Initialize();
	Mesher.SetConfig(CreateMCConfig());

	TestTrue(TEXT("Mesher should be initialized"), Mesher.IsInitialized());

	// Create empty chunk request
	FVoxelMeshingRequest Request = CreateMCEmptyRequest(8);

	// Generate mesh
	FChunkMeshData MeshData;
	FVoxelMeshingStats Stats;
	bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData, Stats);

	TestTrue(TEXT("Empty chunk meshing should succeed"), bSuccess);
	TestEqual(TEXT("Empty chunk should produce 0 vertices"), MeshData.GetVertexCount(), 0);
	TestEqual(TEXT("Empty chunk should produce 0 indices"), MeshData.Indices.Num(), 0);
	TestEqual(TEXT("Empty chunk should produce 0 triangles"), Stats.FaceCount, 0u);

	Mesher.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingSolidChunkTest, "VoxelWorlds.Meshing.MarchingCubes.SolidChunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesMeshingSolidChunkTest::RunTest(const FString& Parameters)
{
	FVoxelCPUMarchingCubesMesher Mesher;
	Mesher.Initialize();
	Mesher.SetConfig(CreateMCConfig());

	// Create fully solid chunk (all corners inside)
	FVoxelMeshingRequest Request = CreateSolidChunkRequest(8);

	// Generate mesh
	FChunkMeshData MeshData;
	FVoxelMeshingStats Stats;
	bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData, Stats);

	TestTrue(TEXT("Solid chunk meshing should succeed"), bSuccess);

	// Fully solid chunk should only produce geometry at boundaries (outer cubes)
	// Interior cubes are all-solid (cubeIndex = 255) which produces no triangles
	AddInfo(FString::Printf(TEXT("Solid chunk: %d verts, %d tris"),
		MeshData.GetVertexCount(), Stats.FaceCount));

	// Should have some boundary triangles but many fewer than cubic meshing
	// (boundary cubes will have some corners outside if no neighbor data)

	Mesher.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingHalfSolidTest, "VoxelWorlds.Meshing.MarchingCubes.HalfSolid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesMeshingHalfSolidTest::RunTest(const FString& Parameters)
{
	FVoxelCPUMarchingCubesMesher Mesher;
	Mesher.Initialize();
	Mesher.SetConfig(CreateMCConfig());

	// Create half-solid request (horizontal plane)
	FVoxelMeshingRequest Request = CreateHalfSolidRequest(8);

	// Generate mesh
	FChunkMeshData MeshData;
	FVoxelMeshingStats Stats;
	bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData, Stats);

	TestTrue(TEXT("Half-solid meshing should succeed"), bSuccess);
	TestTrue(TEXT("Half-solid should produce vertices"), MeshData.GetVertexCount() > 0);
	TestTrue(TEXT("Half-solid should produce indices"), MeshData.Indices.Num() > 0);
	TestTrue(TEXT("Half-solid should produce triangles"), Stats.FaceCount > 0);

	// With binary solid/air voxels, the surface should be at Z = ChunkSize/2 - 0.5
	// Check that all vertices are near the expected Z level
	const float ExpectedZ = (8 / 2 - 0.5f) * Request.VoxelSize;
	int32 VerticesNearSurface = 0;
	const float Tolerance = Request.VoxelSize;

	for (const FVector3f& Pos : MeshData.Positions)
	{
		if (FMath::Abs(Pos.Z - ExpectedZ) < Tolerance)
		{
			VerticesNearSurface++;
		}
	}

	// Most vertices should be near the expected surface height
	float PercentNearSurface = static_cast<float>(VerticesNearSurface) / MeshData.GetVertexCount() * 100.0f;
	AddInfo(FString::Printf(TEXT("Half-solid: %d verts, %d tris, %.1f%% near expected surface"),
		MeshData.GetVertexCount(), Stats.FaceCount, PercentNearSurface));

	TestTrue(TEXT("Most vertices should be near expected surface"),
		PercentNearSurface > 50.0f);

	// Verify normals are unit vectors
	for (int32 i = 0; i < MeshData.Normals.Num(); ++i)
	{
		float Length = MeshData.Normals[i].Size();
		TestTrue(FString::Printf(TEXT("Normal %d should be unit length (got %.2f)"), i, Length),
			FMath::IsNearlyEqual(Length, 1.0f, 0.1f));
	}

	Mesher.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingSphereSdfTest, "VoxelWorlds.Meshing.MarchingCubes.SphereSDF",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesMeshingSphereSdfTest::RunTest(const FString& Parameters)
{
	FVoxelCPUMarchingCubesMesher Mesher;
	Mesher.Initialize();
	Mesher.SetConfig(CreateMCConfig());

	// Create sphere SDF request
	const int32 ChunkSize = 16;
	const float Radius = 5.0f;
	FVoxelMeshingRequest Request = CreateSphereSdfRequest(ChunkSize, Radius);

	// Generate mesh
	FChunkMeshData MeshData;
	FVoxelMeshingStats Stats;
	bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData, Stats);

	TestTrue(TEXT("Sphere SDF meshing should succeed"), bSuccess);
	TestTrue(TEXT("Sphere should produce vertices"), MeshData.GetVertexCount() > 0);
	TestTrue(TEXT("Sphere should produce triangles"), Stats.FaceCount > 0);

	// Verify the mesh forms a closed surface (or nearly closed)
	// Check that vertices are approximately at the expected radius
	const float CenterX = ChunkSize * 0.5f * Request.VoxelSize;
	const float CenterY = ChunkSize * 0.5f * Request.VoxelSize;
	const float CenterZ = ChunkSize * 0.5f * Request.VoxelSize;
	const float ExpectedRadius = Radius * Request.VoxelSize;

	int32 VerticesNearRadius = 0;
	const float RadiusTolerance = Request.VoxelSize * 1.5f;

	for (const FVector3f& Pos : MeshData.Positions)
	{
		float DX = Pos.X - CenterX;
		float DY = Pos.Y - CenterY;
		float DZ = Pos.Z - CenterZ;
		float Distance = FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);

		if (FMath::Abs(Distance - ExpectedRadius) < RadiusTolerance)
		{
			VerticesNearRadius++;
		}
	}

	float PercentNearRadius = static_cast<float>(VerticesNearRadius) / MeshData.GetVertexCount() * 100.0f;
	AddInfo(FString::Printf(TEXT("Sphere: %d verts, %d tris, %.1f%% near expected radius"),
		MeshData.GetVertexCount(), Stats.FaceCount, PercentNearRadius));

	// Most vertices should be near the expected radius
	TestTrue(TEXT("Most vertices should be near expected sphere radius"),
		PercentNearRadius > 70.0f);

	Mesher.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingChunkBoundaryTest, "VoxelWorlds.Meshing.MarchingCubes.ChunkBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesMeshingChunkBoundaryTest::RunTest(const FString& Parameters)
{
	FVoxelCPUMarchingCubesMesher Mesher;
	Mesher.Initialize();
	Mesher.SetConfig(CreateMCConfig());

	const int32 ChunkSize = 8;

	// Create boundary test request (solid at +X edge)
	FVoxelMeshingRequest Request = CreateBoundaryTestRequest(ChunkSize);

	// First, mesh without neighbor data
	FChunkMeshData MeshDataNoNeighbor;
	bool bSuccess1 = Mesher.GenerateMeshCPU(Request, MeshDataNoNeighbor);
	TestTrue(TEXT("Meshing without neighbor should succeed"), bSuccess1);

	int32 TrisWithoutNeighbor = MeshDataNoNeighbor.Indices.Num() / 3;

	// Now add neighbor data showing AIR voxels adjacent
	// Without neighbor data: X=ChunkSize falls back to edge voxel (solid at X=ChunkSize-1)
	// With AIR neighbor data: X=ChunkSize reads air from neighbor, creating a solid/air boundary
	// This should result in MORE triangles at the +X face
	const int32 SliceSize = ChunkSize * ChunkSize;
	Request.NeighborXPos.SetNum(SliceSize);
	for (int32 i = 0; i < SliceSize; ++i)
	{
		Request.NeighborXPos[i] = FVoxelData::Air();
	}

	FChunkMeshData MeshDataWithNeighbor;
	bool bSuccess2 = Mesher.GenerateMeshCPU(Request, MeshDataWithNeighbor);
	TestTrue(TEXT("Meshing with neighbor should succeed"), bSuccess2);

	int32 TrisWithNeighbor = MeshDataWithNeighbor.Indices.Num() / 3;

	AddInfo(FString::Printf(TEXT("Boundary test: %d tris without neighbor, %d with neighbor (air)"),
		TrisWithoutNeighbor, TrisWithNeighbor));

	// With AIR neighbor data, the +X boundary cubes have a solid/air transition
	// This creates surface triangles at the chunk boundary
	// Without neighbor data, fallback samples the edge voxel (solid), so no surface there
	TestTrue(TEXT("AIR neighbor data should create more triangles at boundary"),
		TrisWithNeighbor > TrisWithoutNeighbor);

	Mesher.Shutdown();
	return true;
}

// ==================== GPU MarchingCubes Mesher Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingGPUAsyncTest, "VoxelWorlds.Meshing.MarchingCubes.GPUAsync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesMeshingGPUAsyncTest::RunTest(const FString& Parameters)
{
	FVoxelGPUMarchingCubesMesher Mesher;
	Mesher.Initialize();
	Mesher.SetConfig(CreateMCConfig());

	TestTrue(TEXT("GPU MarchingCubes Mesher should be initialized"), Mesher.IsInitialized());

	// Create half-solid request for testing
	FVoxelMeshingRequest Request = CreateHalfSolidRequest(8);

	// Track completion
	volatile bool bCompleted = false;
	volatile bool bSucceeded = false;
	FVoxelMeshingHandle ResultHandle;

	// Generate mesh asynchronously
	FVoxelMeshingHandle Handle = Mesher.GenerateMeshAsync(Request,
		FOnVoxelMeshingComplete::CreateLambda([&bCompleted, &bSucceeded, &ResultHandle](FVoxelMeshingHandle InHandle, bool bSuccess)
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
		FlushRenderingCommands();
	}

	TestTrue(TEXT("GPU smooth meshing should complete within timeout"), bCompleted);
	TestTrue(TEXT("GPU smooth meshing should succeed"), bSucceeded);

	if (bCompleted && bSucceeded)
	{
		// Get buffer counts
		uint32 VertexCount, IndexCount;
		bool bGotCounts = Mesher.GetBufferCounts(Handle, VertexCount, IndexCount);
		TestTrue(TEXT("Should be able to get buffer counts"), bGotCounts);

		AddInfo(FString::Printf(TEXT("GPU smooth mesh: %d vertices, %d indices"),
			VertexCount, IndexCount));

		TestTrue(TEXT("GPU smooth mesh should have vertices"), VertexCount > 0);
		TestTrue(TEXT("GPU smooth mesh should have indices"), IndexCount > 0);

		// Test buffer access
		FRHIBuffer* VertexBuffer = Mesher.GetVertexBuffer(Handle);
		TestNotNull(TEXT("Should be able to get vertex buffer"), VertexBuffer);

		FRHIBuffer* IndexBuffer = Mesher.GetIndexBuffer(Handle);
		TestNotNull(TEXT("Should be able to get index buffer"), IndexBuffer);
	}

	Mesher.ReleaseHandle(Handle);
	Mesher.Shutdown();

	return true;
}

// ==================== CPU vs GPU Consistency Test ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingCPUvsGPUTest, "VoxelWorlds.Meshing.MarchingCubes.CPUvsGPU",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesMeshingCPUvsGPUTest::RunTest(const FString& Parameters)
{
	FVoxelCPUMarchingCubesMesher CPUMesher;
	FVoxelGPUMarchingCubesMesher GPUMesher;

	FVoxelMeshingConfig MCConfig = CreateMCConfig();

	CPUMesher.Initialize();
	CPUMesher.SetConfig(MCConfig);
	GPUMesher.Initialize();
	GPUMesher.SetConfig(MCConfig);

	// Create half-solid request for comparison
	FVoxelMeshingRequest Request = CreateHalfSolidRequest(8);

	// Generate on CPU
	FChunkMeshData CPUMeshData;
	FVoxelMeshingStats CPUStats;
	bool bCPUSuccess = CPUMesher.GenerateMeshCPU(Request, CPUMeshData, CPUStats);
	TestTrue(TEXT("CPU smooth meshing should succeed"), bCPUSuccess);

	// Generate on GPU
	volatile bool bCompleted = false;
	FVoxelMeshingHandle Handle = GPUMesher.GenerateMeshAsync(Request,
		FOnVoxelMeshingComplete::CreateLambda([&bCompleted](FVoxelMeshingHandle, bool)
		{
			bCompleted = true;
		}));

	double StartTime = FPlatformTime::Seconds();
	while (!bCompleted && (FPlatformTime::Seconds() - StartTime) < 5.0)
	{
		FPlatformProcess::Sleep(0.01f);
		FlushRenderingCommands();
	}
	TestTrue(TEXT("GPU smooth meshing should complete"), bCompleted);

	// Readback GPU data
	FChunkMeshData GPUMeshData;
	bool bReadbackSuccess = GPUMesher.ReadbackToCPU(Handle, GPUMeshData);
	TestTrue(TEXT("GPU readback should succeed"), bReadbackSuccess);

	// Compare vertex counts
	AddInfo(FString::Printf(TEXT("CPU: %d vertices, %d indices"),
		CPUMeshData.GetVertexCount(), CPUMeshData.Indices.Num()));
	AddInfo(FString::Printf(TEXT("GPU: %d vertices, %d indices"),
		GPUMeshData.GetVertexCount(), GPUMeshData.Indices.Num()));

	// Due to atomic counter ordering, GPU vertex order may differ, but counts should be similar
	// Allow some tolerance since floating point calculations may differ slightly
	const int32 VertexDiff = FMath::Abs(CPUMeshData.GetVertexCount() - GPUMeshData.GetVertexCount());
	const int32 IndexDiff = FMath::Abs(CPUMeshData.Indices.Num() - GPUMeshData.Indices.Num());

	// Counts should be identical or very close
	TestTrue(TEXT("Vertex counts should be similar"),
		VertexDiff < CPUMeshData.GetVertexCount() * 0.1f);
	TestTrue(TEXT("Index counts should be similar"),
		IndexDiff < CPUMeshData.Indices.Num() * 0.1f);

	// Compare vertex positions as sets
	if (CPUMeshData.GetVertexCount() > 0 && GPUMeshData.GetVertexCount() > 0)
	{
		int32 MatchCount = 0;
		const float Tolerance = 1.0f;  // 1 unit tolerance

		for (int32 GPUIdx = 0; GPUIdx < GPUMeshData.GetVertexCount(); ++GPUIdx)
		{
			const FVector3f& GPUPos = GPUMeshData.Positions[GPUIdx];

			for (int32 CPUIdx = 0; CPUIdx < CPUMeshData.GetVertexCount(); ++CPUIdx)
			{
				if (GPUPos.Equals(CPUMeshData.Positions[CPUIdx], Tolerance))
				{
					MatchCount++;
					break;
				}
			}
		}

		float MatchPercent = (float)MatchCount / GPUMeshData.GetVertexCount() * 100.0f;
		AddInfo(FString::Printf(TEXT("Vertex set match: %.1f%% (%d/%d GPU vertices found in CPU set)"),
			MatchPercent, MatchCount, GPUMeshData.GetVertexCount()));

		// At least 85% of GPU vertices should exist in the CPU vertex set
		TestTrue(TEXT("At least 85% of GPU vertices should match CPU vertices"),
			MatchPercent >= 85.0f);
	}

	GPUMesher.ReleaseHandle(Handle);
	CPUMesher.Shutdown();
	GPUMesher.Shutdown();

	return true;
}

// ==================== Performance Test ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingPerformanceTest, "VoxelWorlds.Meshing.MarchingCubes.Performance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::MediumPriority)

bool FMarchingCubesMeshingPerformanceTest::RunTest(const FString& Parameters)
{
	FVoxelCPUMarchingCubesMesher CPUMesher;
	FVoxelGPUMarchingCubesMesher GPUMesher;

	FVoxelMeshingConfig MCConfig = CreateMCConfig();

	CPUMesher.Initialize();
	CPUMesher.SetConfig(MCConfig);
	GPUMesher.Initialize();
	GPUMesher.SetConfig(MCConfig);

	// Create 32^3 terrain request (standard chunk size)
	FVoxelMeshingRequest Request = CreateHalfSolidRequest(32);

	const int32 NumIterations = 5;

	// Benchmark CPU meshing
	double CPUTotalTime = 0.0;
	for (int32 i = 0; i < NumIterations; ++i)
	{
		Request.ChunkCoord = FIntVector(i, 0, 0);
		FChunkMeshData MeshData;
		FVoxelMeshingStats Stats;

		double StartTime = FPlatformTime::Seconds();
		CPUMesher.GenerateMeshCPU(Request, MeshData, Stats);
		CPUTotalTime += FPlatformTime::Seconds() - StartTime;
	}
	double CPUAvgMs = (CPUTotalTime / NumIterations) * 1000.0;

	// Benchmark GPU meshing (including count readback)
	double GPUTotalTime = 0.0;
	for (int32 i = 0; i < NumIterations; ++i)
	{
		Request.ChunkCoord = FIntVector(i + 100, 0, 0);

		volatile bool bCompleted = false;
		double StartTime = FPlatformTime::Seconds();

		FVoxelMeshingHandle Handle = GPUMesher.GenerateMeshAsync(Request,
			FOnVoxelMeshingComplete::CreateLambda([&bCompleted](FVoxelMeshingHandle, bool)
			{
				bCompleted = true;
			}));

		while (!bCompleted)
		{
			FPlatformProcess::Sleep(0.001f);
			FlushRenderingCommands();
		}

		// Read counts to ensure we time the full operation
		uint32 VertexCount, IndexCount;
		GPUMesher.GetBufferCounts(Handle, VertexCount, IndexCount);

		GPUTotalTime += FPlatformTime::Seconds() - StartTime;
		GPUMesher.ReleaseHandle(Handle);
	}
	double GPUAvgMs = (GPUTotalTime / NumIterations) * 1000.0;

	AddInfo(FString::Printf(TEXT("32^3 chunk smooth meshing performance:")));
	AddInfo(FString::Printf(TEXT("  CPU average: %.2f ms"), CPUAvgMs));
	AddInfo(FString::Printf(TEXT("  GPU average (with count readback): %.2f ms"), GPUAvgMs));

	// Performance targets: CPU < 100ms, GPU < 5ms
	// MarchingCubes meshing is more complex than cubic meshing
	TestTrue(TEXT("CPU smooth meshing should complete in < 100ms"), CPUAvgMs < 100.0);
	TestTrue(TEXT("GPU smooth meshing should complete in < 5ms"), GPUAvgMs < 5.0);

	CPUMesher.Shutdown();
	GPUMesher.Shutdown();

	return true;
}
