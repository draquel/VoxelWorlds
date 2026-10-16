// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"
#include "ChunkRenderData.h"

// ==================== Helper Functions ====================

namespace CubicMeshingTestHelpers
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
} // namespace CubicMeshingTestHelpers
using namespace CubicMeshingTestHelpers;

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

	// Disable greedy meshing to test face culling specifically
	FVoxelMeshingConfig Config = Mesher.GetConfig();
	Config.bUseGreedyMeshing = false;
	Mesher.SetConfig(Config);

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

	// Disable greedy meshing to test per-face output
	FVoxelMeshingConfig Config = Mesher.GetConfig();
	Config.bUseGreedyMeshing = false;
	Mesher.SetConfig(Config);

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

// ==================== Performance Test ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingPerformanceTest, "VoxelWorlds.Meshing.Cubic.Performance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::MediumPriority)

bool FCubicMeshingPerformanceTest::RunTest(const FString& Parameters)
{
	FVoxelCPUCubicMesher CPUMesher;
	CPUMesher.Initialize();

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

	AddInfo(FString::Printf(TEXT("32^3 chunk meshing performance:")));
	AddInfo(FString::Printf(TEXT("  CPU average: %.2f ms"), CPUAvgMs));

	// Performance target: CPU < 50ms
	TestTrue(TEXT("CPU meshing should complete in < 50ms"), CPUAvgMs < 50.0);

	CPUMesher.Shutdown();

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

// ==================== Seam ownership (world modes review WM-C1) ====================
//
// Streaming meshes a cubic chunk in the Interior domain (every face except those on its six boundary
// planes, reading nothing outside the chunk) and gives each shared plane to the pair's face seam job,
// which culls it against both chunks. These tests check the decomposition against meshing the whole
// region as one chunk: interiors + face seams must reproduce exactly the reference faces inside the
// region (none missing, none twice), for greedy and simple meshing, with non-occluding leaves.

namespace CubicSeamTestHelpers
{
	/** A unit face: (min corner of the unit square in region voxel units, Dir * 256 + MaterialID). */
	using FUnitFace = FIntVector4;

	/**
	 * Region voxel content (Size^3): terrain with a sloped heightfield, steps, a tunnel, floating
	 * blocks and a leaves blob, so faces land on every chunk plane in every direction.
	 */
	FVoxelData RegionVoxel(int32 X, int32 Y, int32 Z, int32 Size)
	{
		const int32 Height = Size / 2 + ((X * 3 + Y * 5) % 7) - 3 + ((X / 3 + Y / 2) % 2);
		if (Z <= Height)
		{
			// A tunnel along X through the middle, a few voxels under the surface.
			if (FMath::Abs(Y - Size / 2) <= 1 && Z >= Height - 4 && Z <= Height - 2)
			{
				return FVoxelData::Air();
			}
			const uint8 Mat = (Z == Height) ? EVoxelMaterial::Grass : ((Z + X) % 5 == 0 ? EVoxelMaterial::Stone : EVoxelMaterial::Dirt);
			return FVoxelData::Solid(Mat, static_cast<uint8>((X / 4) % 2));
		}
		// A leaves blob straddling the region's centre planes, and a few floating blocks.
		const FIntVector Centre(Size / 2, Size / 2, Size / 2 + 4);
		if ((FIntVector(X, Y, Z) - Centre).GetAbsMax() <= 2)
		{
			return FVoxelData::Solid(EVoxelMaterial::Leaves);
		}
		if ((X * 7 + Y * 11 + Z * 13) % 29 == 0 && Z < Size - 1)
		{
			return FVoxelData::Solid(EVoxelMaterial::Wood);
		}
		return FVoxelData::Air();
	}

	/** One chunk (N^3) of the region at chunk offset Offset (in chunks). */
	TArray<FVoxelData> ChunkOf(const FIntVector& Offset, int32 N, int32 RegionSize)
	{
		TArray<FVoxelData> Voxels;
		Voxels.SetNum(N * N * N);
		for (int32 Z = 0; Z < N; ++Z)
		for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			Voxels[X + Y * N + Z * N * N] = RegionVoxel(Offset.X * N + X, Offset.Y * N + Y, Offset.Z * N + Z, RegionSize);
		}
		return Voxels;
	}

	/**
	 * Split each emitted quad (4 vertices each) into unit faces, shifted by OffsetVoxels, and count them.
	 * Returns false if a quad is not an axis-aligned rectangle on the voxel grid.
	 */
	bool CollectUnitFaces(const FChunkMeshData& Mesh, float VoxelSize, const FIntVector& OffsetVoxels, TMap<FUnitFace, int32>& OutFaces)
	{
		if (Mesh.Positions.Num() % 4 != 0)
		{
			return false;
		}
		for (int32 Base = 0; Base < Mesh.Positions.Num(); Base += 4)
		{
			FIntVector Min(INT32_MAX), Max(INT32_MIN);
			for (int32 i = 0; i < 4; ++i)
			{
				const FVector3f& P = Mesh.Positions[Base + i];
				const FIntVector V(FMath::RoundToInt(P.X / VoxelSize), FMath::RoundToInt(P.Y / VoxelSize), FMath::RoundToInt(P.Z / VoxelSize));
				Min = FIntVector(FMath::Min(Min.X, V.X), FMath::Min(Min.Y, V.Y), FMath::Min(Min.Z, V.Z));
				Max = FIntVector(FMath::Max(Max.X, V.X), FMath::Max(Max.Y, V.Y), FMath::Max(Max.Z, V.Z));
			}
			const FVector3f& Nrm = Mesh.Normals[Base];
			const int32 Axis = FMath::Abs(Nrm.X) > 0.5f ? 0 : (FMath::Abs(Nrm.Y) > 0.5f ? 1 : 2);
			const bool bPositive = Nrm[Axis] > 0.0f;
			const int32 Dir = Axis * 2 + (bPositive ? 0 : 1);
			if (Min[Axis] != Max[Axis])
			{
				return false;
			}
			const int32 U = (Axis + 1) % 3;
			const int32 V = (Axis + 2) % 3;
			const int32 Mat = FMath::RoundToInt(Mesh.UV1s[Base].X);
			for (int32 a = Min[U]; a < Max[U]; ++a)
			for (int32 b = Min[V]; b < Max[V]; ++b)
			{
				FIntVector Cell;
				Cell[Axis] = Min[Axis];
				Cell[U] = a;
				Cell[V] = b;
				Cell += OffsetVoxels;
				OutFaces.FindOrAdd(FUnitFace(Cell.X, Cell.Y, Cell.Z, Dir * 256 + Mat))++;
			}
		}
		return true;
	}

	/** Mesh one chunk in a domain (no neighbour slices). */
	FChunkMeshData MeshChunk(FVoxelCPUCubicMesher& Mesher, TArray<FVoxelData> Voxels, int32 N, EVoxelMeshCellDomain Domain)
	{
		FVoxelMeshingRequest Request;
		Request.ChunkSize = N;
		Request.VoxelSize = 100.0f;
		Request.VoxelData = MoveTemp(Voxels);
		Request.MeshCellDomain = Domain;
		FChunkMeshData Mesh;
		Mesher.GenerateMeshCPU(Request, Mesh);
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingInteriorDomainTest, "VoxelWorlds.Meshing.Cubic.InteriorDomain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingInteriorDomainTest::RunTest(const FString& Parameters)
{
	using namespace CubicSeamTestHelpers;
	FVoxelCPUCubicMesher Mesher;
	Mesher.Initialize();
	const int32 N = 8;

	// A solid chunk has only boundary faces: its interior mesh is empty.
	FChunkMeshData Full = MeshChunk(Mesher, CreateFullChunkRequest(N).VoxelData, N, EVoxelMeshCellDomain::Full);
	FChunkMeshData Interior = MeshChunk(Mesher, CreateFullChunkRequest(N).VoxelData, N, EVoxelMeshCellDomain::Interior);
	TestTrue(TEXT("Solid chunk, Full domain: boundary walls"), Full.Positions.Num() > 0);
	TestEqual(TEXT("Solid chunk, Interior domain: nothing"), Interior.Positions.Num(), 0);

	// Interior = Full minus the faces on the chunk's boundary planes, and it never reads neighbour slices.
	for (const bool bGreedy : { false, true })
	{
		FVoxelMeshingConfig Config = Mesher.GetConfig();
		Config.bUseGreedyMeshing = bGreedy;
		Mesher.SetConfig(Config);

		const TArray<FVoxelData> Voxels = ChunkOf(FIntVector(0, 0, 0), N, 2 * N);
		TMap<FUnitFace, int32> FullFaces, InteriorFaces;
		TestTrue(TEXT("Full quads on grid"), CollectUnitFaces(MeshChunk(Mesher, Voxels, N, EVoxelMeshCellDomain::Full), 100.0f, FIntVector::ZeroValue, FullFaces));
		TestTrue(TEXT("Interior quads on grid"), CollectUnitFaces(MeshChunk(Mesher, Voxels, N, EVoxelMeshCellDomain::Interior), 100.0f, FIntVector::ZeroValue, InteriorFaces));

		int32 Expected = 0, Missing = 0, Extra = 0;
		for (const TPair<FUnitFace, int32>& F : FullFaces)
		{
			const int32 Dir = F.Key.W / 256;
			const int32 Axis = Dir / 2;
			const int32 Plane = FIntVector(F.Key.X, F.Key.Y, F.Key.Z)[Axis];
			if (Plane == 0 || Plane == N)
			{
				continue; // on a boundary plane: the face seam's
			}
			++Expected;
			Missing += InteriorFaces.Contains(F.Key) ? 0 : 1;
		}
		for (const TPair<FUnitFace, int32>& F : InteriorFaces)
		{
			Extra += FullFaces.Contains(F.Key) && F.Value == 1 ? 0 : 1;
		}
		TestTrue(*FString::Printf(TEXT("Interior has faces to compare (greedy=%d)"), bGreedy), Expected > 0);
		TestEqual(*FString::Printf(TEXT("Interior misses no off-boundary face (greedy=%d)"), bGreedy), Missing, 0);
		TestEqual(*FString::Printf(TEXT("Interior adds nothing (greedy=%d)"), bGreedy), Extra, 0);

		// Neighbour slices must not change the interior mesh.
		FVoxelMeshingRequest WithSlices;
		WithSlices.ChunkSize = N;
		WithSlices.VoxelSize = 100.0f;
		WithSlices.VoxelData = Voxels;
		WithSlices.MeshCellDomain = EVoxelMeshCellDomain::Interior;
		WithSlices.NeighborXPos.Init(FVoxelData::Solid(EVoxelMaterial::Stone), N * N);
		WithSlices.NeighborZPos.Init(FVoxelData::Solid(EVoxelMaterial::Stone), N * N);
		FChunkMeshData SliceMesh;
		Mesher.GenerateMeshCPU(WithSlices, SliceMesh);
		const FChunkMeshData NoSliceMesh = MeshChunk(Mesher, Voxels, N, EVoxelMeshCellDomain::Interior);
		TestTrue(*FString::Printf(TEXT("Interior ignores neighbour slices (greedy=%d)"), bGreedy),
			SliceMesh.Positions == NoSliceMesh.Positions && SliceMesh.Indices == NoSliceMesh.Indices && SliceMesh.Colors == NoSliceMesh.Colors);
	}

	// Edge and corner seams carry no cubic geometry.
	FChunkMeshData EdgeMesh, CornerMesh;
	TestTrue(TEXT("Edge seam succeeds"), Mesher.GenerateEdgeSeamMeshCPU(FVoxelEdgeSeamRequest(), EdgeMesh));
	TestTrue(TEXT("Corner seam succeeds"), Mesher.GenerateCornerSeamMeshCPU(FVoxelCornerSeamRequest(), CornerMesh));
	TestEqual(TEXT("Edge seam is empty"), EdgeMesh.Positions.Num(), 0);
	TestEqual(TEXT("Corner seam is empty"), CornerMesh.Positions.Num(), 0);

	Mesher.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCubicMeshingSeamClosureTest, "VoxelWorlds.Meshing.Cubic.SeamClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCubicMeshingSeamClosureTest::RunTest(const FString& Parameters)
{
	using namespace CubicSeamTestHelpers;
	FVoxelCPUCubicMesher Mesher;
	Mesher.Initialize();
	const int32 N = 8;
	const int32 R = 2 * N;
	const float VoxelSize = 100.0f;

	TArray<FVoxelData> Region;
	Region.SetNum(R * R * R);
	for (int32 Z = 0; Z < R; ++Z)
	for (int32 Y = 0; Y < R; ++Y)
	for (int32 X = 0; X < R; ++X)
	{
		Region[X + Y * R + Z * R * R] = RegionVoxel(X, Y, Z, R);
	}

	TMap<FIntVector, TSharedPtr<const TArray<FVoxelData>>> Chunks;
	for (int32 k = 0; k < 2; ++k)
	for (int32 j = 0; j < 2; ++j)
	for (int32 i = 0; i < 2; ++i)
	{
		Chunks.Add(FIntVector(i, j, k), MakeShared<const TArray<FVoxelData>>(ChunkOf(FIntVector(i, j, k), N, R)));
	}

	for (const bool bGreedy : { false, true })
	{
		FVoxelMeshingConfig Config = Mesher.GetConfig();
		Config.bUseGreedyMeshing = bGreedy;
		Mesher.SetConfig(Config);

		// Reference: the whole 2x2x2 region meshed as one chunk; keep the faces strictly inside it.
		TMap<FUnitFace, int32> Reference;
		TestTrue(TEXT("Reference quads on grid"), CollectUnitFaces(MeshChunk(Mesher, Region, R, EVoxelMeshCellDomain::Full), VoxelSize, FIntVector::ZeroValue, Reference));
		TSet<FUnitFace> Inside;
		int32 OnChunkPlanes = 0;
		for (const TPair<FUnitFace, int32>& F : Reference)
		{
			const int32 Axis = (F.Key.W / 256) / 2;
			const int32 Plane = FIntVector(F.Key.X, F.Key.Y, F.Key.Z)[Axis];
			if (Plane != 0 && Plane != R)
			{
				Inside.Add(F.Key);
				OnChunkPlanes += (Plane == N) ? 1 : 0;
			}
		}

		// Decomposed: 8 interiors + 12 face seams, each moved into the region's frame.
		TMap<FUnitFace, int32> Decomposed;
		bool bOnGrid = true;
		int32 SeamFaces = 0;
		for (const TPair<FIntVector, TSharedPtr<const TArray<FVoxelData>>>& Chunk : Chunks)
		{
			bOnGrid &= CollectUnitFaces(MeshChunk(Mesher, *Chunk.Value, N, EVoxelMeshCellDomain::Interior), VoxelSize, Chunk.Key * N, Decomposed);
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				FIntVector BCoord = Chunk.Key;
				BCoord[Axis] += 1;
				if (!Chunks.Contains(BCoord))
				{
					continue;
				}
				FVoxelFaceSeamRequest Seam;
				Seam.OwnerChunkCoord = Chunk.Key;
				Seam.Axis = static_cast<uint8>(Axis);
				Seam.ChunkSize = N;
				Seam.VoxelSize = VoxelSize;
				Seam.VoxelDataA = Chunk.Value;
				Seam.VoxelDataB = Chunks[BCoord];
				FChunkMeshData SeamMesh;
				TestTrue(TEXT("Face seam succeeds"), Mesher.GenerateFaceSeamMeshCPU(Seam, SeamMesh));
				SeamFaces += SeamMesh.Positions.Num() / 4;
				bOnGrid &= CollectUnitFaces(SeamMesh, VoxelSize, Chunk.Key * N, Decomposed);
			}
		}
		TestTrue(TEXT("Decomposed quads on grid"), bOnGrid);

		int32 Missing = 0, Extra = 0, Twice = 0;
		for (const FUnitFace& F : Inside)
		{
			Missing += Decomposed.Contains(F) ? 0 : 1;
		}
		for (const TPair<FUnitFace, int32>& F : Decomposed)
		{
			Extra += Inside.Contains(F.Key) ? 0 : 1;
			Twice += F.Value > 1 ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("greedy=%d: %d unit faces inside the region, %d on the chunk planes, %d seam quads"),
			bGreedy, Inside.Num(), OnChunkPlanes, SeamFaces));
		TestTrue(*FString::Printf(TEXT("The chunk planes carry faces to check (greedy=%d)"), bGreedy), OnChunkPlanes > 20 && SeamFaces > 0);
		TestEqual(*FString::Printf(TEXT("No face missing (greedy=%d)"), bGreedy), Missing, 0);
		TestEqual(*FString::Printf(TEXT("No extra face (greedy=%d)"), bGreedy), Extra, 0);
		TestEqual(*FString::Printf(TEXT("No face emitted twice (greedy=%d)"), bGreedy), Twice, 0);
	}

	// Seam positions are in the owner's frame: the shared plane sits at ChunkSize along the axis.
	{
		TArray<FVoxelData> A, B;
		A.Init(FVoxelData::Air(), N * N * N);
		B.Init(FVoxelData::Air(), N * N * N);
		A[(N - 1) + 2 * N + 3 * N * N] = FVoxelData::Solid(EVoxelMaterial::Stone); // touches the +X plane, B side air
		FVoxelFaceSeamRequest Seam;
		Seam.Axis = 0;
		Seam.ChunkSize = N;
		Seam.VoxelSize = VoxelSize;
		Seam.VoxelDataA = MakeShared<const TArray<FVoxelData>>(A);
		Seam.VoxelDataB = MakeShared<const TArray<FVoxelData>>(B);
		FChunkMeshData SeamMesh;
		Mesher.GenerateFaceSeamMeshCPU(Seam, SeamMesh);
		TestEqual(TEXT("One block against air across the plane: one quad"), SeamMesh.Positions.Num(), 4);
		bool bOnPlane = SeamMesh.Positions.Num() == 4;
		for (const FVector3f& P : SeamMesh.Positions)
		{
			bOnPlane &= FMath::IsNearlyEqual(P.X, N * VoxelSize);
		}
		TestTrue(TEXT("The quad lies on the shared plane in the owner's frame"), bOnPlane);
		TestTrue(TEXT("It faces +X"), SeamMesh.Normals.Num() == 4 && SeamMesh.Normals[0].X > 0.5f);

		// The same block with a solid voxel across the plane is culled; B's face toward A is not emitted.
		B[0 + 2 * N + 3 * N * N] = FVoxelData::Solid(EVoxelMaterial::Dirt);
		Seam.VoxelDataB = MakeShared<const TArray<FVoxelData>>(B);
		Mesher.GenerateFaceSeamMeshCPU(Seam, SeamMesh);
		TestEqual(TEXT("Solid against solid across the plane: no face"), SeamMesh.Positions.Num(), 0);
	}

	Mesher.Shutdown();
	return true;
}
