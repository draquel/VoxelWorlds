// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Testing/VoxelScatterTestDefinitions.h"
#include "VoxelChunkManager.h"
#include "VoxelScatterManager.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G79 (WORLD_MODES_REVIEW_PLAN.md §23.15): the chunk manager hands a chunk off to scatter again, from its
// voxels, when the scatter manager's distance pass asks for it (a chunk that arrived at the window's front beyond every
// spawn distance, come within the scatter horizon). Streamed on a plane with LOD off, so a chunk submits one mesh in its
// life and nothing but this path can extract it after its arrival: a viewer walks 16,000 uu in steps of 800 through a
// window of 6,000 with a scatter horizon of 2,500 (trees; grass and rocks at ScatterRadius 1,500). At the end every
// chunk within the horizon of the viewer has been handed off (none waits there) and no chunk holds spawn points past
// the horizon's release distance; with voxel.Scatter.FirstExtractionAtHorizon 0 no chunk that arrived during the
// walk was ever extracted (the ones from the first load unloaded behind the viewer): the symptom on VoxelDemo, where the
// LOD0 refine at 8,200 uu was the next mesh and every type landed there.
// ---------------------------------------------------------------------------

namespace VoxelScatterHorizonStreamingTestUtils
{
	using FScopedCVar = VoxelTesting::FScopedCVar; // the one scoped cvar (code review CR2 row 17)

	constexpr float ViewDistance = 6000.0f;
	constexpr float Radius = 1500.0f;  // grass and rocks
	constexpr float TreeRange = 2500.0f; // the scatter horizon
	constexpr int32 TreeID = 2;

	/** Tick until the chunk manager's pipeline and the scatter manager's extractions have been idle for 90 ticks. */
	bool SettleWithScatter(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds = 180.0)
	{
		UVoxelScatterManager* Scatter = W.Manager->GetScatterManager();
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			const bool bIdle = W.Manager->GetPendingGenerationCount() == 0 && W.Manager->GetPendingMeshingCount() == 0
				&& W.Manager->GetAsyncGenerationInProgressCount() == 0 && W.Manager->GetAsyncMeshingInProgressCount() == 0
				&& W.Manager->GetPendingMeshUploadCount() == 0 && W.Manager->GetSeamJobsInFlightCount() == 0
				&& (!Scatter || Scatter->GetPendingGenerationCount() == 0);
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 90;
	}

	struct FWalkResult
	{
		bool bSettled = true;
		int32 CachedChunks = 0;         // chunks with a surface cache at the end
		int32 SpawnedBeyondRelease = 0; // chunks holding spawn points farther than the horizon's release distance from the viewer
		int32 AwaitingWithinHorizon = 0; // chunks awaiting their first extraction within the horizon of the viewer
		int32 Awaiting = 0;
		int32 Loaded = 0;
		int32 LoadedWithinHorizon = 0;
		int64 DeferredHandOffs = 0;
		int32 TreeChunks = 0;           // chunks holding tree spawn points at the end
	};

	FWalkResult Walk(FAutomationTestBase& Test, int32 Rule)
	{
		const FScopedCVar RuleVar(TEXT("voxel.Scatter.FirstExtractionAtHorizon"), Rule ? TEXT("1") : TEXT("0"));
		const FScopedCVar FarFieldVar(TEXT("voxel.World.FarField"), TEXT("0"));
		FWalkResult R;

		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ScatterHorizonStreamingPlane"));
		W.ConfigureSmallPlane();
		W.Config->ViewDistance = ViewDistance;
		W.Config->LODBands = { FLODBand(0.0f, ViewDistance, 0, 1) };
		W.Config->bEnableScatter = true;
		W.Config->ScatterRadius = Radius;
		W.StartManager();
		UVoxelScatterManager* Scatter = W.Manager->GetScatterManager();
		if (!Scatter)
		{
			Test.AddError(TEXT("no scatter manager"));
			W.Stop();
			return R;
		}
		VoxelScatterTesting::AddSmoothDefinitions(*Scatter);
		Scatter->SetSurfacePointSpacing(W.Config->VoxelSize * 2.0f);
		if (const FScatterDefinition* Trees = Scatter->GetScatterDefinition(TreeID))
		{
			FScatterDefinition Copy = *Trees;
			Copy.SpawnDistance = TreeRange;
			Copy.LODStartDistance = 1500.0f;
			Copy.CullDistance = 2000.0f;
			Copy.Density = 0.2f;
			Scatter->AddScatterDefinition(Copy);
		}
		Test.AddInfo(FString::Printf(TEXT("rule %d: scatter horizon %.0f"), Rule, Scatter->GetScatterHorizon()));

		auto At = [&W](double X) { return FVector(X, 0.0, W.Manager->GetGeneratedSurfacePoint(FVector(X, 0.0, 0.0)).Z + 300.0); };
		W.SetViewer(At(0.0));
		R.bSettled = SettleWithScatter(W);
		for (double X = 800.0; X <= 16000.0 && R.bSettled; X += 800.0)
		{
			W.SetViewer(At(X));
			R.bSettled = SettleWithScatter(W);
		}
		// A few distance passes more at the end (every 0.25 s of tick time)
		for (int32 i = 0; i < 120 && R.bSettled; ++i)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
		}
		R.bSettled = R.bSettled && SettleWithScatter(W);

		const FVector Viewer = At(16000.0);
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		// The chunk grid is at the world origin (zero here): a chunk's centre is its coordinate times the chunk size, plus half
		auto DistanceOf = [&](const FIntVector& Coord)
		{
			return FVector::Dist(FVector(Coord) * ChunkWorld + FVector(ChunkWorld * 0.5), Viewer);
		};
		const float Release = TreeRange * 1.1f + ChunkWorld; // the horizon's release distance, a chunk of slack for the settle
		TArray<FIntVector> LoadedCoords;
		W.Manager->GetLoadedChunks(LoadedCoords);
		for (const FIntVector& Coord : LoadedCoords)
		{
			++R.Loaded;
			R.LoadedWithinHorizon += DistanceOf(Coord) <= TreeRange ? 1 : 0;
			if (const FChunkScatterData* Data = Scatter->GetChunkScatterData(Coord))
			{
				for (const FScatterSpawnPoint& Point : Data->SpawnPoints)
				{
					if (Point.ScatterTypeID == TreeID)
					{
						++R.TreeChunks;
						break;
					}
				}
			}
		}
		// A chunk keeps its surface and scatter cache until it unloads (7,200 here); past the release distance it holds no spawn point
		for (const auto& Pair : Scatter->GetSpawnCache())
		{
			++R.CachedChunks;
			R.SpawnedBeyondRelease += (Pair.Value.SpawnPoints.Num() > 0 && DistanceOf(Pair.Key) > Release) ? 1 : 0;
		}
		for (const FIntVector& Coord : Scatter->GetChunksAwaitingFirstExtraction())
		{
			++R.Awaiting;
			R.AwaitingWithinHorizon += DistanceOf(Coord) <= TreeRange ? 1 : 0;
		}
		R.DeferredHandOffs = Scatter->GetTotalDeferredFirstExtractions();
		Test.AddInfo(FString::Printf(TEXT("rule %d: %s; loaded %d (%d within the horizon), cached %d (%d with spawn points beyond the release distance), %d with trees, awaiting %d (%d within the horizon), deferred hand-offs %lld"),
			Rule, R.bSettled ? TEXT("settled") : TEXT("NOT settled"), R.Loaded, R.LoadedWithinHorizon, R.CachedChunks, R.SpawnedBeyondRelease, R.TreeChunks,
			R.Awaiting, R.AwaitingWithinHorizon, R.DeferredHandOffs));
		W.Stop();
		return R;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneScatterFirstExtractionAtHorizonTest,
	"VoxelWorlds.Streaming.Plane.ScatterFirstExtractionAtHorizon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneScatterFirstExtractionAtHorizonTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHorizonStreamingTestUtils;

	const FWalkResult With = Walk(*this, 1);
	TestTrue(TEXT("With the rule: the walk settled"), With.bSettled);
	TestTrue(TEXT("Scenario: chunks are loaded within the horizon at the end"), With.LoadedWithinHorizon > 0);
	TestTrue(TEXT("With the rule: the distance pass handed chunks off"), With.DeferredHandOffs > 0);
	TestEqual(TEXT("With the rule: no chunk within the horizon awaits its first extraction"), With.AwaitingWithinHorizon, 0);
	TestTrue(TEXT("With the rule: chunks that arrived during the walk hold scatter"), With.CachedChunks > 0);
	TestTrue(TEXT("With the rule: chunks hold trees"), With.TreeChunks > 0);
	TestEqual(TEXT("With the rule: no chunk holds spawn points beyond the horizon's release distance"), With.SpawnedBeyondRelease, 0);

	const FWalkResult Before = Walk(*this, 0);
	TestTrue(TEXT("Rule before: the walk settled"), Before.bSettled);
	TestEqual(TEXT("Rule before: no chunk that arrived during the walk was ever extracted"), Before.CachedChunks, 0);
	TestEqual(TEXT("Rule before: no deferred hand-off"), Before.DeferredHandOffs, static_cast<int64>(0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
