// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelMeshingTestFixtures.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "VoxelVertex.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The render vertices a mesh job prepares (G294): a chunk mesh's FChunkMeshData::PrepareRenderVertices, run on a task
// worker as the chunk manager's mesh job runs it, gives the vertices and the bounds the renderer's conversion at the submit
// gives (FChunkMeshData::ConvertToVoxelVertices, which FVoxelMeshRenderer::ConvertToVoxelVertices calls), byte for byte,
// for each mesher: Dual Contouring (with its material blends), Marching Cubes and the cubic mesher, on hilly terrain of
// two materials and two biomes. The prepared mesh keeps its positions and indices (its validity, its counts, the renderer's
// indices), drops the attributes only the conversion reads, and HasRenderVertices holds; without the drop it keeps them.
// An empty mesh prepares nothing that HasRenderVertices takes. Pure CPU; headless.
// ---------------------------------------------------------------------------

namespace VoxelPreparedRenderVerticesTests
{
	constexpr int32 CS = VoxelMeshingTestFixtures::TestChunkSize;

	static FVoxelData TerrainVoxel(int32 X, int32 Y, int32 Z)
	{
		const float Hills = 6.0f * FMath::PerlinNoise2D(FVector2D(X, Y) / 17.0f) + 2.0f * FMath::PerlinNoise2D(FVector2D(X, Y) / 7.0f);
		const float SD = 16.0f + Hills - static_cast<float>(Z);
		const uint8 Density = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(127.5f + 127.5f * FMath::Clamp(SD, -1.0f, 1.0f)), 0, 255));
		FVoxelData Data = (Density > 0) ? FVoxelData::Solid(((X / 6 + Y / 9) & 1) ? 2 : 1, ((X + Y) / 11) & 1) : FVoxelData::Air();
		Data.Density = Density;
		return Data;
	}

	static FVoxelMeshingRequest MakeRequest()
	{
		FVoxelMeshingRequest R;
		R.ChunkCoord = FIntVector::ZeroValue;
		R.LODLevel = 0;
		R.ChunkSize = CS;
		R.VoxelSize = VoxelMeshingTestFixtures::TestVoxelSize;
		R.MeshCellDomain = EVoxelMeshCellDomain::Interior;
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
		return R;
	}

	/** One mesher's mesh, prepared on a worker against converted on this thread. */
	static void CheckMesher(FAutomationTestBase& Test, const TCHAR* Name, IVoxelMesher& Mesher, const FVoxelMeshingConfig& Config)
	{
		Mesher.Initialize();
		Mesher.SetConfig(Config);
		FChunkMeshData Mesh;
		if (!Test.TestTrue(FString::Printf(TEXT("%s: meshed"), Name), Mesher.GenerateMeshCPU(MakeRequest(), Mesh))
			|| !Test.TestTrue(FString::Printf(TEXT("%s: a mesh (non-vacuous)"), Name), Mesh.IsValid() && Mesh.Positions.Num() > 100))
		{
			return;
		}

		// The renderer's conversion at the submit
		TArray<FVoxelVertex> Converted;
		FBox ConvertedBounds(ForceInit);
		FChunkMeshData::ConvertToVoxelVertices(Mesh, Converted, ConvertedBounds);

		// The mesh job's, on a worker
		FChunkMeshData Prepared = Mesh;
		Async(EAsyncExecution::ThreadPool, [&Prepared]() { Prepared.PrepareRenderVertices(/*bDropConverted*/ true); }).Wait();

		Test.TestTrue(FString::Printf(TEXT("%s: the prepared mesh has its render vertices"), Name), Prepared.HasRenderVertices());
		Test.TestEqual(FString::Printf(TEXT("%s: as many vertices"), Name), Prepared.RenderVertices.Num(), Converted.Num());
		Test.TestTrue(FString::Printf(TEXT("%s: the same vertices, byte for byte"), Name), Prepared.RenderVertices.Num() == Converted.Num()
			&& FMemory::Memcmp(Prepared.RenderVertices.GetData(), Converted.GetData(), Converted.Num() * sizeof(FVoxelVertex)) == 0);
		Test.TestTrue(FString::Printf(TEXT("%s: the same bounds"), Name), Prepared.RenderBounds.IsValid == ConvertedBounds.IsValid
			&& Prepared.RenderBounds.Min == ConvertedBounds.Min && Prepared.RenderBounds.Max == ConvertedBounds.Max);
		Test.TestTrue(FString::Printf(TEXT("%s: the positions and indices kept"), Name),
			Prepared.Positions == Mesh.Positions && Prepared.Indices == Mesh.Indices && Prepared.IsValid());
		Test.TestTrue(FString::Printf(TEXT("%s: the converted attributes dropped"), Name), Prepared.Normals.Num() == 0
			&& Prepared.UVs.Num() == 0 && Prepared.UV1s.Num() == 0 && Prepared.Colors.Num() == 0 && Prepared.MaterialBlends.Num() == 0);
		Test.TestTrue(FString::Printf(TEXT("%s: the source mesh had attributes to drop (non-vacuous)"), Name), Mesh.Normals.Num() == Mesh.Positions.Num());

		// Kept when asked
		FChunkMeshData Kept = Mesh;
		Kept.PrepareRenderVertices(/*bDropConverted*/ false);
		Test.TestTrue(FString::Printf(TEXT("%s: without the drop, the attributes kept"), Name), Kept.Normals == Mesh.Normals
			&& Kept.UVs == Mesh.UVs && Kept.Colors == Mesh.Colors && Kept.MaterialBlends == Mesh.MaterialBlends && Kept.HasRenderVertices());

		// A mesh's copy is not prepared again by the renderer, a Reset clears them
		Kept.Reset();
		Test.TestFalse(FString::Printf(TEXT("%s: a reset mesh has no render vertices"), Name), Kept.HasRenderVertices() || Kept.RenderVertices.Num() > 0);

		Test.AddInfo(FString::Printf(TEXT("%s: %d vertices, %d triangles, %d blended vertices"), Name, Mesh.Positions.Num(), Mesh.Indices.Num() / 3,
			Mesh.MaterialBlends.Num()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPreparedRenderVerticesTest,
	"VoxelWorlds.Meshing.PreparedRenderVertices",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPreparedRenderVerticesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPreparedRenderVerticesTests;
	{
		FVoxelCPUDualContourMesher Mesher;
		CheckMesher(*this, TEXT("Dual Contouring"), Mesher, VoxelMeshingTestFixtures::MakeDCConfig());
	}
	{
		FVoxelCPUMarchingCubesMesher Mesher;
		CheckMesher(*this, TEXT("Marching Cubes"), Mesher, VoxelMeshingTestFixtures::MakeMCConfig(/*bUseTransvoxel*/ false));
	}
	{
		FVoxelCPUCubicMesher Mesher;
		FVoxelMeshingConfig Config;
		Config.bGenerateUVs = true;
		CheckMesher(*this, TEXT("Cubic"), Mesher, Config);
	}

	// An empty mesh: nothing the renderer takes (it removes the chunk)
	FChunkMeshData Empty;
	Empty.PrepareRenderVertices(true);
	TestFalse(TEXT("an empty mesh has no render vertices to take"), Empty.HasRenderVertices());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
