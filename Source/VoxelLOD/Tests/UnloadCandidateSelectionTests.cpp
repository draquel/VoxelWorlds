// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"

#if WITH_DEV_AUTOMATION_TESTS

// ==================== Helpers ====================

namespace UnloadCandidateTestHelpers
{
	/** Exposes the cached geometry the unload query reads, so it runs without a world config. */
	struct FTestableLODStrategy : public FDistanceBandLODStrategy
	{
		using FDistanceBandLODStrategy::BaseChunkSize;
		using FDistanceBandLODStrategy::VoxelSize;
		using FDistanceBandLODStrategy::TerrainMinHeight;
		using FDistanceBandLODStrategy::TerrainMaxHeight;
	};

	/** Demo geometry: 64-voxel chunks at 75 uu (4,800 uu wide), 40,000 view => 48,000 unload radius. */
	void SetupDemo(FTestableLODStrategy& Strategy)
	{
		Strategy.BaseChunkSize = 64;
		Strategy.VoxelSize = 75.0f;
		// A height band that keeps every chunk at Z = 0, so only distance decides.
		Strategy.TerrainMinHeight = -100000.0f;
		Strategy.TerrainMaxHeight = 100000.0f;
		Strategy.SetViewDistance(40000.0f);
		Strategy.SetUnloadDistanceMultiplier(1.2f);
	}
}

// ==================== Tests ====================

/**
 * R23d (VOXEL_QA_FOLLOWUPS_PLAN.md): the per-frame unload cap must be applied AFTER the chunk
 * manager's veto. The strategy used to stop collecting candidates at MaxChunksToUnloadPerFrame and
 * the manager then refused some (chunks still meshing, edit-pinned chunks) — but they had already
 * used the budget, and the loaded set iterates in a stable order, so the same refused chunks came
 * first every frame and a farther-down chunk never unloaded.
 *
 * 20 refused candidates sit ahead of one far chunk the manager accepts; the far chunk must still be
 * selected within a cap of 16 (and even a cap of 1).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnloadCandidatesVetoDoesNotStarveTest,
	"VoxelWorlds.LOD.UnloadCandidates.VetoDoesNotStarve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnloadCandidatesVetoDoesNotStarveTest::RunTest(const FString& Parameters)
{
	using namespace UnloadCandidateTestHelpers;
	FTestableLODStrategy Strategy;
	SetupDemo(Strategy);

	FLODQueryContext Context;
	Context.ViewerPosition = FVector(2400.0f, 2400.0f, 2400.0f); // centre of chunk (0,0,0)
	Context.ViewDistance = 40000.0f;
	Context.MaxChunksToUnloadPerFrame = 16;

	// 20 refused chunks (think: stranded in Meshing) farther out than the one acceptable far chunk,
	// so they come first in farthest-first order; plus some in-range chunks that are not candidates.
	TSet<FIntVector> Loaded;
	TSet<FIntVector> Refused;
	for (int32 i = 0; i < 20; ++i)
	{
		const FIntVector Coord(20 + i, 0, 0); // 96,000 uu and beyond
		Loaded.Add(Coord);
		Refused.Add(Coord);
	}
	const FIntVector FarChunk(12, 0, 0); // 57,600 uu: beyond the 48,000 unload radius
	Loaded.Add(FarChunk);
	for (int32 i = 0; i < 5; ++i)
	{
		Loaded.Add(FIntVector(i, 1, 0)); // well inside the view distance
	}

	// The strategy reports EVERY out-of-range chunk, uncapped, farthest first.
	TArray<FIntVector> Candidates;
	Strategy.GetChunksToUnload(Candidates, Loaded, Context);
	TestEqual(TEXT("strategy returns all 21 out-of-range chunks (no per-frame cap)"), Candidates.Num(), 21);
	TestFalse(TEXT("in-range chunks are not candidates"), Candidates.Contains(FIntVector(0, 1, 0)));
	if (Candidates.Num() == 21)
	{
		TestEqual(TEXT("farthest first"), Candidates[0], FIntVector(39, 0, 0));
		TestEqual(TEXT("the acceptable far chunk is the nearest candidate, behind all refused ones"), Candidates.Last(), FarChunk);
	}

	auto CanUnload = [&Refused](const FIntVector& Coord) { return !Refused.Contains(Coord); };

	// The manager's selection: refusals cost nothing, so the far chunk is picked within the cap.
	TArray<FIntVector> Selected;
	const int32 Count = IVoxelLODStrategy::SelectUnloadCandidates(Candidates, CanUnload, Context.MaxChunksToUnloadPerFrame, Selected);
	TestEqual(TEXT("exactly the one acceptable chunk is selected"), Count, 1);
	TestTrue(TEXT("the far chunk unloads this frame"), Selected.Contains(FarChunk));

	// Even with a cap of 1 it is not starved.
	IVoxelLODStrategy::SelectUnloadCandidates(Candidates, CanUnload, 1, Selected);
	TestEqual(TEXT("cap 1 still selects the far chunk"), Selected.Num(), 1);
	TestTrue(TEXT("cap 1: far chunk"), Selected.Num() == 1 && Selected[0] == FarChunk);

	// The cap still binds when everything is acceptable, keeping farthest-first order.
	IVoxelLODStrategy::SelectUnloadCandidates(Candidates, [](const FIntVector&) { return true; }, 16, Selected);
	TestEqual(TEXT("cap binds on acceptable candidates"), Selected.Num(), 16);
	TestTrue(TEXT("selection keeps farthest-first order"), Selected.Num() > 0 && Selected[0] == FIntVector(39, 0, 0));

	// <= 0 means no cap.
	IVoxelLODStrategy::SelectUnloadCandidates(Candidates, [](const FIntVector&) { return true; }, 0, Selected);
	TestEqual(TEXT("cap 0 = unlimited"), Selected.Num(), 21);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
