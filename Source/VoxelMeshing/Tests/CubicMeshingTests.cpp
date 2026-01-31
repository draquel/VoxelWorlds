// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelGPUCubicMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"
#include "RenderingThread.h"

// ==================== Helper Functions ====================

namespace
{
	/**
	 * Create a meshing request with all air voxels.
	 */
	FVoxelMeshingRequest CreateEmptyChunkRequest(int32 ChunkSize = 16)
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
	 * Create a meshing request with all solid voxels.
	 */
	FVoxelMeshingRequest CreateFullChunkRequest(int32 ChunkSize = 16)
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
	 * Create a meshing request with a single solid voxel at center.
	 */
	FVoxelMeshingRequest CreateSingleVoxelRequest(int32 ChunkSize = 16)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = 0;

		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		Request.VoxelData.SetNum(TotalVoxels);

		// All air
		for (int32 i = 0; i < TotalVoxels; ++i)
		{
			Request.VoxelData[i] = FVoxelData::Air();
		}

		// Single solid at center
		const int32 Center = ChunkSize / 2;
		const int32 CenterIndex = Center + Center * ChunkSize + Center * ChunkSize * ChunkSize;
		Request.VoxelData[CenterIndex] = FVoxelData::Solid(1);

		return Request;
	}

	/**
	 * Create a meshing request with two adjacent solid voxels (to test face culling).
	 */
	FVoxelMeshingRequest CreateAdjacentVoxelsRequest(int32 ChunkSize = 16)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = 0;

		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		Request.VoxelData.SetNum(TotalVoxels);

		// All air
		for (int32 i = 0; i < TotalVoxels; ++i)
		{
			Request.VoxelData[i] = FVoxelData::Air();
		}

		// Two adjacent solid voxels at center
		const int32 Center = ChunkSize / 2;
		const int32 Index1 = Center + Center * ChunkSize + Center * ChunkSize * ChunkSize;
		const int32 Index2 = (Center + 1) + Center * ChunkSize + Center * ChunkSize * ChunkSize;
		Request.VoxelData[Index1] = FVoxelData::Solid(1);
		Request.VoxelData[Index2] = FVoxelData::Solid(1);

		return Request;
	}

	/**
	 * Create a meshing request with terrain-like data (half solid, half air).
	 */
	FVoxelMeshingRequest CreateTerrainLikeRequest(int32 ChunkSize = 32)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = 0;

		const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
		Request.VoxelData.SetNum(TotalVoxels);

		// Lower half solid, upper half air
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
}

// ==================== CPU Mesher Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingEmptyChunkTest, "VoxelWorlds.Meshing.Cubic.EmptyChunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingEmptyChunkTest::RunTest(const FString& Parameters)
{
	FVoxelCPUCubicMesher Mesher;
	Mesher.Initialize();

	TestTrue(TEXT("Mesher should be initialized"), Mesher.IsInitialized());

	// Create empty chunk request
	FVoxelMeshingRequest Request = CreateEmptyChunkRequest(8);

	// Generate mesh
	FChunkMeshData MeshData;
	FVoxelMeshingStats Stats;
	bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData, Stats);

	TestTrue(TEXT("Empty chunk meshing should succeed"), bSuccess);
	TestEqual(TEXT("Empty chunk should produce 0 vertices"), MeshData.GetVertexCount(), 0);
	TestEqual(TEXT("Empty chunk should produce 0 indices"), MeshData.Indices.Num(), 0);
	TestEqual(TEXT("Empty chunk should produce 0 faces"), Stats.FaceCount, 0u);

	Mesher.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingSingleVoxelTest, "VoxelWorlds.Meshing.Cubic.SingleVoxel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingSingleVoxelTest::RunTest(const FString& Parameters)
{
	FVoxelCPUCubicMesher Mesher;
	Mesher.Initialize();

	// Create single voxel request
	FVoxelMeshingRequest Request = CreateSingleVoxelRequest(8);

	// Generate mesh
	FChunkMeshData MeshData;
	FVoxelMeshingStats Stats;
	bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData, Stats);

	TestTrue(TEXT("Single voxel meshing should succeed"), bSuccess);

	// Single voxel = 6 faces, 4 vertices per face, 6 indices per face
	const int32 ExpectedVertices = 6 * 4;  // 24 vertices
	const int32 ExpectedIndices = 6 * 6;   // 36 indices (6 faces * 2 triangles * 3 indices)

	TestEqual(TEXT("Single voxel should produce 24 vertices"), MeshData.GetVertexCount(), ExpectedVertices);
	TestEqual(TEXT("Single voxel should produce 36 indices"), MeshData.Indices.Num(), ExpectedIndices);
	TestEqual(TEXT("Single voxel should produce 6 faces"), Stats.FaceCount, 6u);
	TestEqual(TEXT("Single voxel should report 1 solid voxel"), Stats.SolidVoxelCount, 1u);

	// Verify normals are unit vectors
	for (int32 i = 0; i < MeshData.Normals.Num(); ++i)
	{
		float Length = MeshData.Normals[i].Size();
		TestTrue(FString::Printf(TEXT("Normal %d should be unit length"), i),
			FMath::IsNearlyEqual(Length, 1.0f, 0.01f));
	}

	Mesher.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingFaceCullingTest, "VoxelWorlds.Meshing.Cubic.FaceCulling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingFaceCullingTest::RunTest(const FString& Parameters)
{
	FVoxelCPUCubicMesher Mesher;
	Mesher.Initialize();

	// Create adjacent voxels request
	FVoxelMeshingRequest Request = CreateAdjacentVoxelsRequest(8);

	// Generate mesh
	FChunkMeshData MeshData;
	FVoxelMeshingStats Stats;
	bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData, Stats);

	TestTrue(TEXT("Adjacent voxels meshing should succeed"), bSuccess);

	// Two adjacent voxels should have:
	// - 2 voxels * 6 faces = 12 potential faces
	// - 2 shared faces (one between them, culled from each voxel's perspective)
	// - 12 - 2 = 10 actual faces
	const int32 ExpectedFaces = 10;
	const int32 ExpectedVertices = ExpectedFaces * 4;  // 40 vertices
	const int32 ExpectedIndices = ExpectedFaces * 6;   // 60 indices

	TestEqual(TEXT("Two adjacent voxels should produce 40 vertices"), MeshData.GetVertexCount(), ExpectedVertices);
	TestEqual(TEXT("Two adjacent voxels should produce 60 indices"), MeshData.Indices.Num(), ExpectedIndices);
	TestEqual(TEXT("Two adjacent voxels should produce 10 faces"), Stats.FaceCount, static_cast<uint32>(ExpectedFaces));
	TestEqual(TEXT("Should report 2 culled faces"), Stats.CulledFaceCount, 2u);

	Mesher.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingFullChunkTest, "VoxelWorlds.Meshing.Cubic.FullChunk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingFullChunkTest::RunTest(const FString& Parameters)
{
	FVoxelCPUCubicMesher Mesher;
	Mesher.Initialize();

	// Create full chunk request (small size for test)
	const int32 ChunkSize = 4;
	FVoxelMeshingRequest Request = CreateFullChunkRequest(ChunkSize);

	// Generate mesh
	FChunkMeshData MeshData;
	FVoxelMeshingStats Stats;
	bool bSuccess = Mesher.GenerateMeshCPU(Request, MeshData, Stats);

	TestTrue(TEXT("Full chunk meshing should succeed"), bSuccess);

	// Full chunk should only have exterior faces
	// For a 4^3 chunk, that's 6 faces * 4^2 = 96 faces
	const int32 ExpectedFaces = 6 * ChunkSize * ChunkSize;  // 96
	const int32 ExpectedVertices = ExpectedFaces * 4;       // 384
	const int32 ExpectedIndices = ExpectedFaces * 6;        // 576

	TestEqual(TEXT("Full chunk should produce exterior faces only"), Stats.FaceCount, static_cast<uint32>(ExpectedFaces));
	TestEqual(TEXT("Full chunk vertices"), MeshData.GetVertexCount(), ExpectedVertices);

	// Verify all interior faces were culled
	const int32 TotalSolidVoxels = ChunkSize * ChunkSize * ChunkSize;
	const int32 TotalPotentialFaces = TotalSolidVoxels * 6;
	TestEqual(TEXT("Culled faces should be total - exterior"),
		Stats.CulledFaceCount, static_cast<uint32>(TotalPotentialFaces - ExpectedFaces));

	Mesher.Shutdown();
	return true;
}

// ==================== GPU Mesher Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingGPUAsyncTest, "VoxelWorlds.Meshing.Cubic.GPUAsync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingGPUAsyncTest::RunTest(const FString& Parameters)
{
	FVoxelGPUCubicMesher Mesher;
	Mesher.Initialize();

	TestTrue(TEXT("GPU Mesher should be initialized"), Mesher.IsInitialized());

	// Create single voxel request
	FVoxelMeshingRequest Request = CreateSingleVoxelRequest(8);

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

	TestTrue(TEXT("GPU meshing should complete within timeout"), bCompleted);
	TestTrue(TEXT("GPU meshing should succeed"), bSucceeded);

	if (bCompleted && bSucceeded)
	{
		// Get buffer counts
		uint32 VertexCount, IndexCount;
		bool bGotCounts = Mesher.GetBufferCounts(Handle, VertexCount, IndexCount);
		TestTrue(TEXT("Should be able to get buffer counts"), bGotCounts);

		// Single voxel = 24 vertices, 36 indices
		TestEqual(TEXT("GPU single voxel should produce 24 vertices"), VertexCount, 24u);
		TestEqual(TEXT("GPU single voxel should produce 36 indices"), IndexCount, 36u);

		// Test vertex buffer access
		FRHIBuffer* VertexBuffer = Mesher.GetVertexBuffer(Handle);
		TestNotNull(TEXT("Should be able to get vertex buffer"), VertexBuffer);

		FRHIBuffer* IndexBuffer = Mesher.GetIndexBuffer(Handle);
		TestNotNull(TEXT("Should be able to get index buffer"), IndexBuffer);
	}

	Mesher.ReleaseHandle(Handle);
	Mesher.Shutdown();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingGPUReadbackTest, "VoxelWorlds.Meshing.Cubic.GPUReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingGPUReadbackTest::RunTest(const FString& Parameters)
{
	FVoxelGPUCubicMesher Mesher;
	Mesher.Initialize();

	// Create single voxel request
	FVoxelMeshingRequest Request = CreateSingleVoxelRequest(8);

	// Generate mesh asynchronously
	volatile bool bCompleted = false;
	FVoxelMeshingHandle Handle = Mesher.GenerateMeshAsync(Request,
		FOnVoxelMeshingComplete::CreateLambda([&bCompleted](FVoxelMeshingHandle, bool)
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

	TestTrue(TEXT("GPU meshing should complete"), bCompleted);

	// Readback to CPU
	FChunkMeshData MeshData;
	bool bReadbackSuccess = Mesher.ReadbackToCPU(Handle, MeshData);

	TestTrue(TEXT("Readback should succeed"), bReadbackSuccess);
	TestEqual(TEXT("Readback should have 24 vertices"), MeshData.GetVertexCount(), 24);
	TestEqual(TEXT("Readback should have 36 indices"), MeshData.Indices.Num(), 36);

	// Verify data integrity
	TestEqual(TEXT("Positions and normals should match"), MeshData.Positions.Num(), MeshData.Normals.Num());
	TestEqual(TEXT("Positions and UVs should match"), MeshData.Positions.Num(), MeshData.UVs.Num());

	Mesher.ReleaseHandle(Handle);
	Mesher.Shutdown();

	return true;
}

// ==================== CPU vs GPU Consistency Test ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingCPUvsGPUTest, "VoxelWorlds.Meshing.Cubic.CPUvsGPU",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingCPUvsGPUTest::RunTest(const FString& Parameters)
{
	FVoxelCPUCubicMesher CPUMesher;
	FVoxelGPUCubicMesher GPUMesher;

	CPUMesher.Initialize();
	GPUMesher.Initialize();

	// Create terrain-like request for comparison
	FVoxelMeshingRequest Request = CreateTerrainLikeRequest(8);

	// Generate on CPU
	FChunkMeshData CPUMeshData;
	FVoxelMeshingStats CPUStats;
	bool bCPUSuccess = CPUMesher.GenerateMeshCPU(Request, CPUMeshData, CPUStats);
	TestTrue(TEXT("CPU meshing should succeed"), bCPUSuccess);

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
	TestTrue(TEXT("GPU meshing should complete"), bCompleted);

	// Readback GPU data
	FChunkMeshData GPUMeshData;
	bool bReadbackSuccess = GPUMesher.ReadbackToCPU(Handle, GPUMeshData);
	TestTrue(TEXT("GPU readback should succeed"), bReadbackSuccess);

	// Compare vertex counts
	AddInfo(FString::Printf(TEXT("CPU: %d vertices, %d indices"),
		CPUMeshData.GetVertexCount(), CPUMeshData.Indices.Num()));
	AddInfo(FString::Printf(TEXT("GPU: %d vertices, %d indices"),
		GPUMeshData.GetVertexCount(), GPUMeshData.Indices.Num()));

	// Counts should match exactly for deterministic meshing
	TestEqual(TEXT("Vertex counts should match"), CPUMeshData.GetVertexCount(), GPUMeshData.GetVertexCount());
	TestEqual(TEXT("Index counts should match"), CPUMeshData.Indices.Num(), GPUMeshData.Indices.Num());

	// Compare vertex positions as SETS (GPU uses atomic counters, so order is non-deterministic)
	// We check that each GPU vertex exists somewhere in the CPU vertex set
	if (CPUMeshData.GetVertexCount() == GPUMeshData.GetVertexCount() && CPUMeshData.GetVertexCount() > 0)
	{
		int32 MatchCount = 0;
		const float Tolerance = 0.1f;

		// For each GPU vertex, search for a matching CPU vertex
		for (int32 GPUIdx = 0; GPUIdx < GPUMeshData.GetVertexCount(); ++GPUIdx)
		{
			const FVector3f& GPUPos = GPUMeshData.Positions[GPUIdx];

			for (int32 CPUIdx = 0; CPUIdx < CPUMeshData.GetVertexCount(); ++CPUIdx)
			{
				if (GPUPos.Equals(CPUMeshData.Positions[CPUIdx], Tolerance))
				{
					MatchCount++;
					break;  // Found a match, move to next GPU vertex
				}
			}
		}

		float MatchPercent = (float)MatchCount / GPUMeshData.GetVertexCount() * 100.0f;
		AddInfo(FString::Printf(TEXT("Vertex set match: %.1f%% (%d/%d GPU vertices found in CPU set)"),
			MatchPercent, MatchCount, GPUMeshData.GetVertexCount()));

		// At least 85% of GPU vertices should exist in the CPU vertex set
		TestTrue(TEXT("At least 85% of GPU vertices should match CPU vertices"), MatchPercent >= 85.0f);
	}

	GPUMesher.ReleaseHandle(Handle);
	CPUMesher.Shutdown();
	GPUMesher.Shutdown();

	return true;
}

// ==================== Performance Test ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingPerformanceTest, "VoxelWorlds.Meshing.Cubic.Performance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::MediumPriority)

bool FCubicMeshingPerformanceTest::RunTest(const FString& Parameters)
{
	FVoxelCPUCubicMesher CPUMesher;
	FVoxelGPUCubicMesher GPUMesher;

	CPUMesher.Initialize();
	GPUMesher.Initialize();

	// Create 32^3 terrain request (standard chunk size)
	FVoxelMeshingRequest Request = CreateTerrainLikeRequest(32);

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

	AddInfo(FString::Printf(TEXT("32^3 chunk meshing performance:")));
	AddInfo(FString::Printf(TEXT("  CPU average: %.2f ms"), CPUAvgMs));
	AddInfo(FString::Printf(TEXT("  GPU average (with count readback): %.2f ms"), GPUAvgMs));

	// Performance targets: CPU < 50ms, GPU < 5ms (meshing is simpler than noise generation)
	TestTrue(TEXT("CPU meshing should complete in < 50ms"), CPUAvgMs < 50.0);
	TestTrue(TEXT("GPU meshing should complete in < 5ms"), GPUAvgMs < 5.0);

	CPUMesher.Shutdown();
	GPUMesher.Shutdown();

	return true;
}

// ==================== Boundary/Neighbor Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingChunkBoundaryTest, "VoxelWorlds.Meshing.Cubic.ChunkBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingChunkBoundaryTest::RunTest(const FString& Parameters)
{
	FVoxelCPUCubicMesher Mesher;
	Mesher.Initialize();

	const int32 ChunkSize = 4;

	// Create request with solid voxels at +X edge
	FVoxelMeshingRequest Request;
	Request.ChunkCoord = FIntVector(0, 0, 0);
	Request.ChunkSize = ChunkSize;
	Request.VoxelSize = 100.0f;

	const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
	Request.VoxelData.SetNum(TotalVoxels);

	// All air except for voxels at X = ChunkSize-1 edge
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

	// First, mesh without neighbor data (should render +X boundary faces)
	FChunkMeshData MeshDataNoNeighbor;
	bool bSuccess1 = Mesher.GenerateMeshCPU(Request, MeshDataNoNeighbor);
	TestTrue(TEXT("Meshing without neighbor should succeed"), bSuccess1);

	int32 FacesWithoutNeighbor = MeshDataNoNeighbor.Indices.Num() / 6;

	// Now add neighbor data showing solid voxels adjacent (should cull +X faces)
	const int32 SliceSize = ChunkSize * ChunkSize;
	Request.NeighborXPos.SetNum(SliceSize);
	for (int32 i = 0; i < SliceSize; ++i)
	{
		Request.NeighborXPos[i] = FVoxelData::Solid(1);  // Adjacent voxels are solid
	}

	FChunkMeshData MeshDataWithNeighbor;
	bool bSuccess2 = Mesher.GenerateMeshCPU(Request, MeshDataWithNeighbor);
	TestTrue(TEXT("Meshing with neighbor should succeed"), bSuccess2);

	int32 FacesWithNeighbor = MeshDataWithNeighbor.Indices.Num() / 6;

	AddInfo(FString::Printf(TEXT("Faces without neighbor: %d, with neighbor: %d"),
		FacesWithoutNeighbor, FacesWithNeighbor));

	// With neighbor data showing solid, +X faces should be culled
	// Original: each edge voxel has 5 exposed faces (not -X which faces interior)
	// With solid neighbor: each edge voxel has 4 exposed faces (+X also culled)
	TestTrue(TEXT("Neighbor data should reduce face count"),
		FacesWithNeighbor < FacesWithoutNeighbor);

	Mesher.Shutdown();
	return true;
}
