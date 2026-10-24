// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G14 (WORLD_MODES_REVIEW_PLAN.md section 14.1, section 20.26). The streaming bench counts a re-mesh when a mesh job
// launches for a chunk that already had a mesh, and it knew that only of chunks meshed since the bench started: starting
// a run emptied the set. A bench started after the editor's first, cold PIE had meshed fewer chunks by then than one
// started in a later PIE, so it counted about twice the LOD re-meshes on the same route (499 against 257-259 on the
// control's chord) while the LOD evaluation queued the same number in both.
//
// - Streaming.Bench.PreBenchMeshes: a small plane with three LOD bands streams around a viewer and settles; the counters
//   are reset as a bench start resets them, the viewer moves 3,000 uu and the world settles again. Every chunk that
//   re-meshed for its LOD was meshed before the reset. With voxel.Bench.CountPreBenchMeshes 1 the bench's re-mesh count
//   (and its LOD share) equals the re-meshes the renderer received after the reset; with 0 (the rule before) it counts
//   none of them.
// ---------------------------------------------------------------------------

namespace VoxelBenchRemeshCountTest
{
	struct FRunResult
	{
		bool bSettled = false;
		int64 Truth = 0;      // chunk meshes submitted after the reset for a chunk that had one
		int64 Counted = 0;    // the bench's re-mesh count
		int64 CountedLOD = 0; // its LOD-transition share
		int32 Loaded = 0;
	};

	FRunResult Run(int32 CountPreBenchMeshes)
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Bench.CountPreBenchMeshes"));
		const int32 Previous = CVar ? CVar->GetInt() : 1;
		if (CVar) { CVar->Set(CountPreBenchMeshes, ECVF_SetByCode); }

		FRunResult Result;
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("VoxelBenchPreBenchMeshes"));
		W.ConfigureCommon(100.0f, 16, 6000.0f);
		W.Config->WorldMode = EWorldMode::InfinitePlane;
		W.Config->WorldOrigin = FVector::ZeroVector;
		W.Config->HeightScale = 800.0f;
		W.Config->BaseHeight = 0.0f;
		W.Config->bEnableLOD = true;
		W.Config->LODBands = {
			FLODBand(0.0f, 2000.0f, 0, 1),
			FLODBand(2000.0f, 4000.0f, 1, 2),
			FLODBand(4000.0f, 6000.0f, 2, 4) };
		W.StartManager();

		const float SurfaceA = W.Manager->GetGeneratedSurfaceHeight(0.0f, 0.0f);
		W.SetViewer(FVector(0.0, 0.0, SurfaceA + 200.0));
		const bool bSettledA = W.Settle();

		// A bench starts here: counters reset, then its view moves.
		const int64 ResubmitsBefore = W.Renderer.ChunkResubmits;
		const float SurfaceB = W.Manager->GetGeneratedSurfaceHeight(3000.0f, 0.0f);
		W.SetViewer(FVector(3000.0, 0.0, SurfaceB + 200.0));
		const bool bSettledB = W.Settle();

		Result.bSettled = bSettledA && bSettledB;
		Result.Truth = W.Renderer.ChunkResubmits - ResubmitsBefore;
		Result.Counted = W.Manager->GetBenchRemeshCount();
		Result.CountedLOD = W.Manager->GetBenchRemeshByReason(EVoxelRemeshReason::LODTransition);
		Result.Loaded = W.Renderer.ChunkMeshes.Num();
		W.Stop();

		if (CVar) { CVar->Set(Previous, ECVF_SetByCode); }
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBenchPreBenchMeshesTest, "VoxelWorlds.Streaming.Bench.PreBenchMeshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelBenchPreBenchMeshesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelBenchRemeshCountTest;

	const FRunResult After = Run(1);
	const FRunResult Before = Run(0);

	AddInfo(FString::Printf(TEXT("CountPreBenchMeshes 1: re-meshes received %lld, counted %lld (LOD %lld), %d chunk meshes"),
		After.Truth, After.Counted, After.CountedLOD, After.Loaded));
	AddInfo(FString::Printf(TEXT("CountPreBenchMeshes 0 (before): re-meshes received %lld, counted %lld (LOD %lld), %d chunk meshes"),
		Before.Truth, Before.Counted, Before.CountedLOD, Before.Loaded));

	TestTrue(TEXT("Both runs settled at both viewers"), After.bSettled && Before.bSettled);
	TestTrue(TEXT("The move re-meshed chunks meshed before the reset"), After.Truth > 0 && Before.Truth > 0);
	TestEqual(TEXT("1: the bench counts every re-mesh the renderer received"), After.Counted, After.Truth);
	TestEqual(TEXT("1: they are all LOD transitions"), After.CountedLOD, After.Truth);
	TestEqual(TEXT("0 (the rule before): none of them counted"), Before.Counted, (int64)0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
