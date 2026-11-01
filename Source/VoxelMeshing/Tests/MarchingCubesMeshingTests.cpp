// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestScopedCVar.h"

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

// ==================== LOD Vertex Normals (G61) ====================

namespace MarchingCubesMeshingTestHelpers
{
	/**
	 * A tilted plane through the chunk's centre (solid below, Normal pointing into the air) whose density saturates a
	 * voxel either side of the surface, as generated terrain's does (SignedDistanceToDensity), at a given LOD.
	 */
	FVoxelMeshingRequest CreateSaturatedPlaneRequest(int32 ChunkSize, int32 LODLevel, const FVector3f& Normal)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkCoord = FIntVector(0, 0, 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = 100.0f;
		Request.LODLevel = LODLevel;
		Request.VoxelData.SetNum(ChunkSize * ChunkSize * ChunkSize);
		const FVector3f Centre(ChunkSize * 0.5f + 0.3f, ChunkSize * 0.5f - 0.2f, ChunkSize * 0.5f + 0.15f);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const float Above = FVector3f::DotProduct(FVector3f(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z)) - Centre, Normal); // voxels into the air
					const float Density = (FMath::Clamp(-Above, -1.0f, 1.0f) + 1.0f) * 127.5f;
					Request.VoxelData[X + Y * ChunkSize + Z * ChunkSize * ChunkSize] =
						FVoxelData(1, static_cast<uint8>(FMath::Clamp(Density, 0.0f, 255.0f)));
				}
			}
		}
		return Request;
	}

	/** Mean angle (degrees) between a mesh's vertex normals and Normal, over vertices more than Margin voxels inside. */
	double MeanAngleInside(const FChunkMeshData& Mesh, const FVector3f& Normal, float VoxelSize, int32 ChunkSize, float Margin, int32& OutCount)
	{
		double Sum = 0.0;
		OutCount = 0;
		for (int32 i = 0; i < Mesh.Positions.Num() && i < Mesh.Normals.Num(); ++i)
		{
			const FVector3f V = Mesh.Positions[i] / VoxelSize;
			if (V.GetMin() < Margin || V.GetMax() > ChunkSize - Margin)
			{
				continue;
			}
			Sum += FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector3f::DotProduct(Mesh.Normals[i].GetSafeNormal(), Normal), -1.0f, 1.0f)));
			++OutCount;
		}
		return OutCount > 0 ? Sum / OutCount : 0.0;
	}
}

/**
 * G61 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.9): on a tilted plane whose density saturates a voxel either side of the
 * surface, Marching Cubes' LOD 1 and 2 vertex normals are as close to the plane's as LOD 0's (voxel.Mesh.VoxelShadingNormals
 * 1), where the stride-scale gradient before G61 (0) snaps them toward the cube directions; positions are the same either
 * way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMarchingCubesMeshingLODNormalsTest, "VoxelWorlds.Meshing.MarchingCubes.LODNormals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMarchingCubesMeshingLODNormalsTest::RunTest(const FString& Parameters)
{
	constexpr int32 ChunkSize = 32;
	const FVector3f Normal = FVector3f(0.35f, 0.2f, 0.92f).GetSafeNormal();

	FVoxelCPUMarchingCubesMesher CPUMesher;
	CPUMesher.Initialize();
	CPUMesher.SetConfig(CreateMCConfig());

	double Angle[2][3] = {};
	for (int32 Mode = 0; Mode < 2; ++Mode)
	{
		VoxelTesting::FScopedCVar Scoped(TEXT("voxel.Mesh.VoxelShadingNormals"), Mode);
		for (int32 LOD = 0; LOD < 3; ++LOD)
		{
			const FVoxelMeshingRequest Request = CreateSaturatedPlaneRequest(ChunkSize, LOD, Normal);
			FChunkMeshData CPUMesh;
			TestTrue(*FString::Printf(TEXT("Normals %d LOD %d: CPU meshing succeeds"), Mode, LOD), CPUMesher.GenerateMeshCPU(Request, CPUMesh));
			int32 Count = 0;
			// Away from the chunk's faces, where the taps clamp (no neighbour data here).
			Angle[Mode][LOD] = MeanAngleInside(CPUMesh, Normal, Request.VoxelSize, ChunkSize, 1.5f * (1 << LOD) + 1.0f, Count);
			TestTrue(*FString::Printf(TEXT("Normals %d LOD %d: vertices measured (%d)"), Mode, LOD, Count), Count > 50);
		}
	}
	AddInfo(FString::Printf(TEXT("Mean angle from the plane's normal, LOD 0/1/2: before G61 %.2f / %.2f / %.2f deg, after %.2f / %.2f / %.2f deg"),
		Angle[0][0], Angle[0][1], Angle[0][2], Angle[1][0], Angle[1][1], Angle[1][2]));
	TestTrue(TEXT("LOD 0 is the same in either mode"), FMath::IsNearlyEqual(Angle[0][0], Angle[1][0], 1e-6));
	for (int32 LOD = 1; LOD < 3; ++LOD)
	{
		TestTrue(*FString::Printf(TEXT("LOD %d's normals within a degree of LOD 0's error (%.2f against %.2f)"), LOD, Angle[1][LOD], Angle[1][0]),
			Angle[1][LOD] <= Angle[1][0] + 1.0);
	}
	TestTrue(*FString::Printf(TEXT("The stride-scale normals before G61 snap at LOD 2 (%.2f deg against LOD 0's %.2f)"), Angle[0][2], Angle[0][0]),
		Angle[0][2] > Angle[0][0] + 3.0);

	CPUMesher.Shutdown();
	return true;
}
