// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/ParallelFor.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshingTestFixtures.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// What a Dual Contouring chunk mesh costs at the demo's chunk size (G275): a 64-voxel chunk of noise terrain meshed at
// LOD 0 in the Interior domain (a streamed chunk's mesh) and in the WholeChunk domain with its six face planes (a
// collision cook's mesh), one at a time and four at once on the task workers. Logged as "[DCBench]" lines: the mean per
// mesh and per pass, and the mesh's CRC, which a change to the mesher's working memory must leave as it was. The test
// only fails on a mesh that differs between runs of the same request; the times are for reading. Pure CPU; headless.
// ---------------------------------------------------------------------------

namespace VoxelDualContourBench
{
	constexpr int32 CS = 64;
	constexpr float VoxelSize = 75.0f;

	static FVoxelData TerrainVoxel(int32 X, int32 Y, int32 Z)
	{
		const FVector P(X, Y, Z);
		const float Hills = 12.0f * FMath::PerlinNoise2D(FVector2D(P.X, P.Y) / 41.0f)
			+ 4.0f * FMath::PerlinNoise2D(FVector2D(P.X, P.Y) / 13.0f + FVector2D(17.0, 3.0));
		const float Bumps = 1.6f * FMath::PerlinNoise3D(P / 6.0f + FVector(5.0, 11.0, 2.0));
		const float SD = 32.0f + Hills + Bumps - static_cast<float>(Z);
		const uint8 Density = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(127.5f + 127.5f * FMath::Clamp(SD, -1.0f, 1.0f)), 0, 255));
		FVoxelData Data = (Density > 0) ? FVoxelData::Solid((SD > 2.5f) ? 2 : 1, (((X + Y) / 16) & 1) ? 1 : 0) : FVoxelData::Air();
		Data.Density = Density;
		return Data;
	}

	static FVoxelMeshingRequest MakeRequest(EVoxelMeshCellDomain Domain)
	{
		FVoxelMeshingRequest R;
		R.ChunkCoord = FIntVector::ZeroValue;
		R.LODLevel = 0;
		R.ChunkSize = CS;
		R.VoxelSize = VoxelSize;
		R.MeshCellDomain = Domain;
		R.VoxelData.SetNumUninitialized(CS * CS * CS);
		for (int32 Z = 0; Z < CS; ++Z)
		{
			for (int32 Y = 0; Y < CS; ++Y)
			{
				for (int32 X = 0; X < CS; ++X)
				{
					R.VoxelData[X + Y * CS + Z * CS * CS] = TerrainVoxel(X, Y, Z);
				}
			}
		}
		if (Domain == EVoxelMeshCellDomain::Interior)
		{
			return R;
		}
		R.NeighborPlaneDepth = 1;
		auto FillPlane = [](TArray<FVoxelData>& Plane, int32 Axis, int32 At)
		{
			Plane.SetNumUninitialized(CS * CS);
			for (int32 B = 0; B < CS; ++B)
			{
				for (int32 A = 0; A < CS; ++A)
				{
					int32 V[3];
					V[Axis] = At;
					V[(Axis == 0) ? 1 : 0] = A;
					V[(Axis == 2) ? 1 : 2] = B;
					Plane[A + B * CS] = TerrainVoxel(V[0], V[1], V[2]);
				}
			}
		};
		FillPlane(R.NeighborXPos, 0, CS);
		FillPlane(R.NeighborXNeg, 0, -1);
		FillPlane(R.NeighborYPos, 1, CS);
		FillPlane(R.NeighborYNeg, 1, -1);
		FillPlane(R.NeighborZPos, 2, CS);
		FillPlane(R.NeighborZNeg, 2, -1);
		return R;
	}

	static uint32 MeshCrc(const FChunkMeshData& M)
	{
		uint32 Crc = 0;
		auto Add = [&Crc](const void* Data, int64 Bytes) { Crc = FCrc::MemCrc32(Data, static_cast<int32>(Bytes), Crc); };
		Add(M.Positions.GetData(), M.Positions.Num() * M.Positions.GetTypeSize());
		Add(M.Normals.GetData(), M.Normals.Num() * M.Normals.GetTypeSize());
		Add(M.UVs.GetData(), M.UVs.Num() * M.UVs.GetTypeSize());
		Add(M.UV1s.GetData(), M.UV1s.Num() * M.UV1s.GetTypeSize());
		Add(M.Colors.GetData(), M.Colors.Num() * M.Colors.GetTypeSize());
		Add(M.MaterialBlends.GetData(), M.MaterialBlends.Num() * M.MaterialBlends.GetTypeSize());
		Add(M.Indices.GetData(), M.Indices.Num() * M.Indices.GetTypeSize());
		return Crc;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelDualContourBenchTest, "VoxelWorlds.Meshing.DualContour.Bench",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FVoxelDualContourBenchTest::RunTest(const FString& Parameters)
{
	using namespace VoxelDualContourBench;
	FVoxelCPUDualContourMesher Mesher;
	Mesher.Initialize();
	Mesher.SetConfig(VoxelMeshingTestFixtures::MakeDCConfig());

	constexpr int32 Runs = 10;
	constexpr int32 Lanes = 4;
	for (const EVoxelMeshCellDomain Domain : { EVoxelMeshCellDomain::Interior, EVoxelMeshCellDomain::WholeChunk })
	{
		const TCHAR* Name = (Domain == EVoxelMeshCellDomain::Interior) ? TEXT("interior") : TEXT("wholechunk");
		const FVoxelMeshingRequest Request = MakeRequest(Domain);

		// One at a time: the passes' means over the runs after the first.
		FVoxelMeshingStats Sum;
		uint32 Crc = 0;
		uint32 Triangles = 0;
		for (int32 Run = 0; Run <= Runs; ++Run)
		{
			FChunkMeshData Mesh;
			FVoxelMeshingStats Stats;
			TestTrue(TEXT("The chunk meshes"), Mesher.GenerateMeshCPU(Request, Mesh, Stats));
			const uint32 RunCrc = MeshCrc(Mesh);
			if (Run == 0)
			{
				Crc = RunCrc;
				Triangles = Stats.GetTriangleCount();
				continue;
			}
			TestEqual(TEXT("The same request gives the same mesh"), RunCrc, Crc);
			Sum.GenerationTimeMs += Stats.GenerationTimeMs;
			Sum.SetupTimeMs += Stats.SetupTimeMs;
			Sum.CrossingsTimeMs += Stats.CrossingsTimeMs;
			Sum.VerticesTimeMs += Stats.VerticesTimeMs;
			Sum.QuadsTimeMs += Stats.QuadsTimeMs;
		}

		// Four at once, as the collision cooks run.
		double LaneMs[Lanes] = {};
		uint32 LaneCrc[Lanes] = {};
		const double WallStart = FPlatformTime::Seconds();
		ParallelFor(Lanes, [&](int32 Lane)
		{
			for (int32 Run = 0; Run < Runs; ++Run)
			{
				FChunkMeshData Mesh;
				FVoxelMeshingStats Stats;
				Mesher.GenerateMeshCPU(Request, Mesh, Stats);
				LaneMs[Lane] += Stats.GenerationTimeMs;
				LaneCrc[Lane] = MeshCrc(Mesh);
			}
		}, EParallelForFlags::Unbalanced);
		const double WallMs = (FPlatformTime::Seconds() - WallStart) * 1000.0;
		double ConcurrentMs = 0.0;
		for (int32 Lane = 0; Lane < Lanes; ++Lane)
		{
			ConcurrentMs += LaneMs[Lane];
			TestEqual(TEXT("A mesh made beside three others is the same mesh"), LaneCrc[Lane], Crc);
		}

		UE_LOG(LogTemp, Display, TEXT("[DCBench] %s crc=%08X tris=%u alone=%.2fms (setup %.2f, crossings %.2f, vertices %.2f, quads %.2f) four-at-once=%.2fms each, %.1f meshes/s"),
			Name, Crc, Triangles, Sum.GenerationTimeMs / Runs, Sum.SetupTimeMs / Runs, Sum.CrossingsTimeMs / Runs,
			Sum.VerticesTimeMs / Runs, Sum.QuadsTimeMs / Runs, ConcurrentMs / (Lanes * Runs), (Lanes * Runs) * 1000.0 / WallMs);
	}
	Mesher.Shutdown();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
