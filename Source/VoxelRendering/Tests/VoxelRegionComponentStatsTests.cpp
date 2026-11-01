// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VoxelRegionComponent.h"
#include "VoxelVertex.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// UVoxelRegionComponent's vertex, triangle and GPU memory statistics report the meshes it holds now
// (WORLD_MODES_REVIEW_PLAN.md G1, WM-H2). They used to add every upload and never subtract, so a session
// that re-meshed and unloaded chunks reported many times what the renderer held. Runs headless: the
// component is never registered, so meshes stay in its pending batch and no render command is sent.
// ---------------------------------------------------------------------------

namespace VoxelRegionComponentStatsTestUtils
{
	/** Submit a CPU mesh with the given vertex and index counts (geometry content is irrelevant here). */
	void Submit(UVoxelRegionComponent& Component, const FIntVector& Chunk, int32 NumVertices, int32 NumIndices)
	{
		TArray<FVoxelVertex> Vertices;
		Vertices.SetNum(NumVertices);
		TArray<uint32> Indices;
		Indices.SetNumZeroed(NumIndices);
		Component.UpdateChunkBuffersFromCPUData(Chunk, MoveTemp(Vertices), MoveTemp(Indices), /*LODLevel*/ 0,
			FBox(FVector::ZeroVector, FVector(100.0)));
	}

	int64 MeshBytes(int64 NumVertices, int64 NumIndices)
	{
		return NumVertices * sizeof(FVoxelVertex) + NumIndices * sizeof(uint32);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRegionComponentStatsCurrentTest,
	"VoxelWorlds.Rendering.WorldComponent.StatsReportCurrentMeshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelRegionComponentStatsCurrentTest::RunTest(const FString& Parameters)
{
	using namespace VoxelRegionComponentStatsTestUtils;

	UVoxelRegionComponent* Component = NewObject<UVoxelRegionComponent>(GetTransientPackage());
	const FIntVector A(0, 0, 0);
	const FIntVector B(1, 0, 0);

	Submit(*Component, A, 100, 300);
	TestEqual(TEXT("One mesh: vertices"), Component->GetTotalVertexCount(), int64(100));
	TestEqual(TEXT("One mesh: triangles"), Component->GetTotalTriangleCount(), int64(100));
	TestEqual(TEXT("One mesh: GPU bytes"), Component->GetGPUMemoryUsage(), MeshBytes(100, 300));

	// A re-mesh replaces the chunk's mesh: the old one no longer counts.
	Submit(*Component, A, 40, 60);
	TestEqual(TEXT("Re-meshed: vertices"), Component->GetTotalVertexCount(), int64(40));
	TestEqual(TEXT("Re-meshed: triangles"), Component->GetTotalTriangleCount(), int64(20));
	TestEqual(TEXT("Re-meshed: GPU bytes"), Component->GetGPUMemoryUsage(), MeshBytes(40, 60));

	Submit(*Component, B, 10, 30);
	TestEqual(TEXT("Two chunks: vertices"), Component->GetTotalVertexCount(), int64(50));
	TestEqual(TEXT("Two chunks: triangles"), Component->GetTotalTriangleCount(), int64(30));

	// Unloading takes the chunk out.
	Component->RemoveChunk(A);
	TestEqual(TEXT("A unloaded: vertices"), Component->GetTotalVertexCount(), int64(10));
	TestEqual(TEXT("A unloaded: triangles"), Component->GetTotalTriangleCount(), int64(10));
	TestEqual(TEXT("A unloaded: GPU bytes"), Component->GetGPUMemoryUsage(), MeshBytes(10, 30));

	// Removing a chunk that is not there changes nothing; an empty mesh removes the chunk.
	Component->RemoveChunk(A);
	TestEqual(TEXT("Removing A twice: vertices"), Component->GetTotalVertexCount(), int64(10));
	Submit(*Component, B, 0, 0);
	TestEqual(TEXT("Empty mesh for B: vertices"), Component->GetTotalVertexCount(), int64(0));
	TestEqual(TEXT("Empty mesh for B: triangles"), Component->GetTotalTriangleCount(), int64(0));
	TestEqual(TEXT("Empty mesh for B: GPU bytes"), Component->GetGPUMemoryUsage(), int64(0));

	Submit(*Component, A, 7, 9);
	Component->ClearAllChunks();
	TestEqual(TEXT("ClearAllChunks: vertices"), Component->GetTotalVertexCount(), int64(0));

	Component->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
