// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// When FDistanceBandLODStrategy rebuilds its balanced LOD cache (WORLD_MODES_REVIEW_PLAN.md G12, WM-H1).
// Pure logic, runs headless.
//
// The cache (raw band LOD, hysteresis, 2:1 balance over the whole candidate volume) used to be rebuilt by
// every Update, so a viewer standing still paid 0.6-0.7 ms a tick on the demo for a map that could not have
// changed. It is now rebuilt when the viewer enters another chunk or moves 100 uu (the chunk manager's LOD
// evaluation trigger), when the world origin or a balance cvar changes, and after a configuration change.
// ---------------------------------------------------------------------------

namespace LODCacheScheduleTestHelpers
{
	/** The demo's chunk geometry and bands (4,800 uu chunks, view distance 40,000 uu). */
	UVoxelWorldConfiguration* MakeConfig()
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = EWorldMode::InfinitePlane;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->bEnableLOD = true;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->ViewDistance = 40000.0f;
		Config->LODBands = {
			FLODBand(0.0f, 7000.0f, 0, 1),
			FLODBand(7000.0f, 14000.0f, 1, 2),
			FLODBand(14000.0f, 40000.0f, 2, 4) };
		Config->bFarBandSurfaceSlabCulling = false;
		Config->bEnableFrustumCulling = false;
		return Config;
	}

	FLODQueryContext MakeContext(const FVector& ViewerPosition)
	{
		FLODQueryContext Context;
		Context.ViewerPosition = ViewerPosition;
		Context.WorldMode = EWorldMode::InfinitePlane;
		return Context;
	}

	/** Chunk -> LOD for every chunk the strategy reports visible. */
	TMap<FIntVector, int32> VisibleLODs(const FDistanceBandLODStrategy& Strategy, const FLODQueryContext& Context)
	{
		TMap<FIntVector, int32> Result;
		for (const FChunkLODRequest& Request : Strategy.GetVisibleChunks(Context))
		{
			Result.Add(Request.ChunkCoord, Request.LODLevel);
		}
		return Result;
	}

	constexpr float Dt = 1.0f / 60.0f;
}

/**
 * The triggers: a still viewer does not rebuild; movement below 100 uu inside one chunk does not; 100 uu does;
 * entering another chunk does even after a few uu; a configuration change does.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODCacheScheduleTriggersTest,
	"VoxelWorlds.LOD.BalancedCache.RebuildTriggers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODCacheScheduleTriggersTest::RunTest(const FString& Parameters)
{
	using namespace LODCacheScheduleTestHelpers;

	FDistanceBandLODStrategy Strategy;
	Strategy.Initialize(MakeConfig());

	// Centre of chunk (0, 0, 0): 2,400 uu from every chunk face.
	const FVector Start(2400.0f, 2400.0f, 2400.0f);
	Strategy.Update(MakeContext(Start), Dt);
	TestEqual(TEXT("First update rebuilds"), Strategy.GetBalancedLODCacheRebuildCount(), 1);

	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Strategy.Update(MakeContext(Start), Dt);
	}
	TestEqual(TEXT("A still viewer never rebuilds"), Strategy.GetBalancedLODCacheRebuildCount(), 1);

	Strategy.Update(MakeContext(Start + FVector(60.0f, 0.0f, 0.0f)), Dt);
	TestEqual(TEXT("60 uu inside the chunk: no rebuild"), Strategy.GetBalancedLODCacheRebuildCount(), 1);

	// Measured from the last rebuild, not from the last tick.
	Strategy.Update(MakeContext(Start + FVector(110.0f, 0.0f, 0.0f)), Dt);
	TestEqual(TEXT("110 uu from the last rebuild: rebuild"), Strategy.GetBalancedLODCacheRebuildCount(), 2);

	// 30 uu across the +X face into chunk (1, 0, 0).
	const FVector NearFace(4790.0f, 2400.0f, 2400.0f);
	Strategy.Update(MakeContext(NearFace), Dt);
	const int32 BeforeCrossing = Strategy.GetBalancedLODCacheRebuildCount();
	Strategy.Update(MakeContext(NearFace + FVector(30.0f, 0.0f, 0.0f)), Dt);
	TestEqual(TEXT("Entering another chunk rebuilds after 30 uu"), Strategy.GetBalancedLODCacheRebuildCount(), BeforeCrossing + 1);

	const FVector Here = NearFace + FVector(30.0f, 0.0f, 0.0f);
	Strategy.SetViewDistance(30000.0f);
	Strategy.Update(MakeContext(Here), Dt);
	TestEqual(TEXT("SetViewDistance rebuilds"), Strategy.GetBalancedLODCacheRebuildCount(), BeforeCrossing + 2);

	Strategy.SetLODBands(Strategy.GetLODBands());
	Strategy.Update(MakeContext(Here), Dt);
	TestEqual(TEXT("SetLODBands rebuilds"), Strategy.GetBalancedLODCacheRebuildCount(), BeforeCrossing + 3);

	Strategy.Initialize(MakeConfig());
	Strategy.Update(MakeContext(Here), Dt);
	TestEqual(TEXT("Initialize rebuilds"), Strategy.GetBalancedLODCacheRebuildCount(), BeforeCrossing + 4);

	FLODQueryContext Shifted = MakeContext(Here);
	Shifted.WorldOrigin = FVector(12345.0f, 54321.0f, 0.0f);
	Strategy.Update(Shifted, Dt);
	TestEqual(TEXT("A new world origin rebuilds"), Strategy.GetBalancedLODCacheRebuildCount(), BeforeCrossing + 5);

	return true;
}

/**
 * Skipping a rebuild changes no answer: for a viewer standing still, the cached map equals the one a strategy
 * that rebuilds every tick converges to. Hysteresis and the 2:1 balance are both functions of the distances
 * and the previous rebuild, so rebuilding at an unchanged position reproduces the map.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODCacheScheduleStillViewerStableTest,
	"VoxelWorlds.LOD.BalancedCache.StillViewerMatchesEveryTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODCacheScheduleStillViewerStableTest::RunTest(const FString& Parameters)
{
	using namespace LODCacheScheduleTestHelpers;

	// A short walk that leaves chunks sitting inside hysteresis deadbands, then a stop.
	TArray<FVector> Path;
	for (int32 Step = 0; Step <= 40; ++Step)
	{
		Path.Add(FVector(2400.0f + Step * 150.0f, 2400.0f + Step * 40.0f, 2400.0f));
	}
	const FVector Stop = Path.Last();

	FDistanceBandLODStrategy Scheduled;
	FDistanceBandLODStrategy EveryTick;
	Scheduled.Initialize(MakeConfig());
	EveryTick.Initialize(MakeConfig());

	for (const FVector& Position : Path)
	{
		Scheduled.Update(MakeContext(Position), Dt);
		EveryTick.SetViewDistance(EveryTick.GetViewDistance()); // force the rebuild, the old behaviour
		EveryTick.Update(MakeContext(Position), Dt);
	}
	// Standing still: the old behaviour kept rebuilding.
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		Scheduled.Update(MakeContext(Stop), Dt);
		EveryTick.SetViewDistance(EveryTick.GetViewDistance());
		EveryTick.Update(MakeContext(Stop), Dt);
	}

	const TMap<FIntVector, int32> A = VisibleLODs(Scheduled, MakeContext(Stop));
	const TMap<FIntVector, int32> B = VisibleLODs(EveryTick, MakeContext(Stop));
	TestEqual(TEXT("Same visible chunk count"), A.Num(), B.Num());
	int32 Differences = 0;
	for (const TPair<FIntVector, int32>& Pair : A)
	{
		const int32* Other = B.Find(Pair.Key);
		if (!Other || *Other != Pair.Value)
		{
			++Differences;
		}
	}
	TestEqual(TEXT("Chunks whose LOD differs from the every-tick strategy"), Differences, 0);
	TestTrue(TEXT("The scheduled strategy rebuilt less often"),
		Scheduled.GetBalancedLODCacheRebuildCount() < EveryTick.GetBalancedLODCacheRebuildCount());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
