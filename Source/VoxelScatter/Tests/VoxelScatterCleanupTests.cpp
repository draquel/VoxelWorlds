// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "VoxelScatterManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelData.h"
#include "VoxelMaterialRegistry.h"
#include "ChunkRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Scatter cleanup regression tests
//
// Cover the two scatter-cleanup bugs fixed on fix/scatter-cleanup:
//  - stale async results resurrecting scatter for unloaded chunks
//    ("trail in the void" + leaked HISM instances)
//  - player edits not removing scatter (cleared volumes ignored by
//    in-flight results and stale cached surface data)
//  - completion queues dropping a result at the per-frame budget boundary
//    (permanently leaking AsyncScatterInProgress entries)
//
// The scatter async pipeline runs on the thread pool but results are only
// consumed inside Update() on the game thread, so tests can deterministically
// interleave unload/clear calls between launch and consumption.
// ---------------------------------------------------------------------------

namespace VoxelScatterCleanupTestUtils
{
	constexpr int32 TestChunkSize = 16;
	constexpr float TestVoxelSize = 100.0f;

	/** Private game world + initialized scatter manager, torn down on destruction. */
	struct FScatterTestHarness
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelScatterManager* Manager = nullptr;

		explicit FScatterTestHarness(int32 MaxAsyncTasks)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelScatterCleanupTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);

			Config = NewObject<UVoxelWorldConfiguration>();
			Config->ChunkSize = TestChunkSize;
			Config->VoxelSize = TestVoxelSize;
			Config->ScatterRadius = 100000.0f; // keep every test chunk in range of the origin viewer
			Config->MaxAsyncScatterTasks = MaxAsyncTasks;
			Config->bUseGPUScatterExtraction = false;

			Manager = NewObject<UVoxelScatterManager>();
			Manager->AddToRoot();
			Manager->Initialize(Config, World);
		}

		~FScatterTestHarness()
		{
			if (Manager)
			{
				Manager->Shutdown();
				Manager->RemoveFromRoot();
			}
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}
	};

	/** Flat grass slab filling the bottom half of the chunk — guarantees surface + spawn points. */
	static TArray<FVoxelData> MakeFlatGrassChunk()
	{
		TArray<FVoxelData> Voxels;
		Voxels.Init(FVoxelData::Air(), TestChunkSize * TestChunkSize * TestChunkSize);

		const int32 SurfaceZ = TestChunkSize / 2;
		for (int32 Z = 0; Z < SurfaceZ; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					Voxels[X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize] =
						FVoxelData::Solid(EVoxelMaterial::Grass);
				}
			}
		}
		return Voxels;
	}

	/**
	 * Flat grass slab whose air-above voxels carry the water and/or underground flags.
	 * The surface extractor reads these flags off the air voxel directly above each column
	 * to classify a surface point as underwater / underground.
	 */
	static TArray<FVoxelData> MakeFlaggedGrassChunk(bool bWater, bool bUnderground)
	{
		TArray<FVoxelData> Voxels;
		Voxels.Init(FVoxelData::Air(), TestChunkSize * TestChunkSize * TestChunkSize);

		const int32 SurfaceZ = TestChunkSize / 2;
		for (int32 Z = 0; Z < TestChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					const int32 Idx = X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize;
					if (Z < SurfaceZ)
					{
						Voxels[Idx] = FVoxelData::Solid(EVoxelMaterial::Grass);
					}
					else
					{
						if (bWater) { Voxels[Idx].SetWaterFlag(true); }
						if (bUnderground) { Voxels[Idx].SetUndergroundFlag(true); }
					}
				}
			}
		}
		return Voxels;
	}

	/** Submit a chunk to the scatter manager as if its mesh just arrived. */
	static void SubmitChunk(UVoxelScatterManager* Manager, const FIntVector& ChunkCoord)
	{
		const FChunkMeshData EmptyMesh; // CPU voxel-based extraction ignores mesh data
		Manager->OnChunkMeshDataReady(ChunkCoord, 0, EmptyMesh, MakeFlatGrassChunk(), TestChunkSize, TestVoxelSize);
	}

	/** Pump Update until Condition is met or timeout. Returns whether the condition was met. */
	static bool PumpUntil(UVoxelScatterManager* Manager, float TimeoutSeconds, TFunctionRef<bool()> Condition)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Manager->Update(FVector::ZeroVector, 0.05f);
			if (Condition())
			{
				return true;
			}
			FPlatformProcess::Sleep(0.02f);
		}
		return false;
	}

	/** Pump Update for a fixed duration — used to assert something does NOT happen. */
	static void PumpFor(UVoxelScatterManager* Manager, float Seconds)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Manager->Update(FVector::ZeroVector, 0.05f);
			FPlatformProcess::Sleep(0.02f);
		}
	}

	/** World-space center of a test chunk. */
	static FVector ChunkCenter(const FIntVector& ChunkCoord)
	{
		const float ChunkWorldSize = TestChunkSize * TestVoxelSize;
		return FVector(ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
	}
}

// ---------------------------------------------------------------------------
// Positive control: the async pipeline produces scatter data for a submitted
// chunk and the pending count returns to zero. The later tests' fixed-duration
// pumps are only meaningful because this proves tasks complete well inside them.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterAsyncPipelineTest,
	"VoxelWorlds.Scatter.Cleanup.AsyncPipelineGeneratesScatter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterAsyncPipelineTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);
	const FIntVector Coord(0, 0, 0);

	SubmitChunk(Harness.Manager, Coord);

	TestTrue(TEXT("Scatter data generated within timeout"),
		PumpUntil(Harness.Manager, 10.0f, [&]() { return Harness.Manager->HasScatterData(Coord); }));

	TestTrue(TEXT("Pending generation count returns to zero"),
		PumpUntil(Harness.Manager, 5.0f, [&]() { return Harness.Manager->GetPendingGenerationCount() == 0; }));

	const FChunkScatterData* ScatterData = Harness.Manager->GetChunkScatterData(Coord);
	TestNotNull(TEXT("Scatter data cached"), ScatterData);
	if (ScatterData)
	{
		TestTrue(TEXT("Spawn points generated on flat grass"), ScatterData->SpawnPoints.Num() > 0);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Unload while the async task is in flight: the arriving result must be
// discarded, not cached. Pre-fix this repopulated ScatterDataCache/
// SurfaceDataCache for a chunk the world no longer tracks (trail in the void).
//
// Deterministic interleave: Update() consumes results BEFORE launching new
// tasks, so a task launched by Update N cannot be consumed until Update N+1 —
// OnChunkUnloaded between them always races ahead of the result.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterUnloadDiscardTest,
	"VoxelWorlds.Scatter.Cleanup.UnloadDiscardsInFlightResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterUnloadDiscardTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);
	const FIntVector Coord(0, 0, 0);

	SubmitChunk(Harness.Manager, Coord);

	// Single Update: launches the async task (no result can be consumed yet)
	Harness.Manager->Update(FVector::ZeroVector, 0.05f);
	TestEqual(TEXT("Task is in flight after first Update"),
		Harness.Manager->GetPendingGenerationCount(), 1);

	// Unload while in flight
	Harness.Manager->OnChunkUnloaded(Coord);

	// Give the result ample time to arrive and be (discarded on) consumption
	PumpFor(Harness.Manager, 3.0f);

	TestFalse(TEXT("No scatter data for unloaded chunk"), Harness.Manager->HasScatterData(Coord));
	TestNull(TEXT("Scatter cache empty for unloaded chunk"), Harness.Manager->GetChunkScatterData(Coord));
	TestNull(TEXT("Surface cache empty for unloaded chunk"), Harness.Manager->GetChunkSurfaceData(Coord));
	TestEqual(TEXT("No in-progress leak"), Harness.Manager->GetPendingGenerationCount(), 0);
	TestEqual(TEXT("Discarded result contributed no spawn points"),
		Harness.Manager->GetStatistics().TotalSpawnPoints, (int64)0);

	return true;
}

// ---------------------------------------------------------------------------
// ClearScatterInRadius while the async task is in flight: the cleared volume
// is registered AFTER the task snapshotted its cleared volumes, so the result
// must be filtered at consumption time. Pre-fix the result re-added spawn
// points inside the just-dug area (floating scatter).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterClearInFlightTest,
	"VoxelWorlds.Scatter.Cleanup.ClearFiltersInFlightResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterClearInFlightTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);
	const FIntVector Coord(0, 0, 0);
	const float ChunkWorldSize = TestChunkSize * TestVoxelSize;

	SubmitChunk(Harness.Manager, Coord);

	// Launch the async task, then clear a volume covering the whole chunk
	Harness.Manager->Update(FVector::ZeroVector, 0.05f);
	Harness.Manager->ClearScatterInRadius(ChunkCenter(Coord), ChunkWorldSize * 2.0f);

	PumpFor(Harness.Manager, 3.0f);

	const FChunkScatterData* ScatterData = Harness.Manager->GetChunkScatterData(Coord);
	TestTrue(TEXT("No spawn points survive inside the cleared volume"),
		ScatterData == nullptr || ScatterData->SpawnPoints.Num() == 0);

	const FChunkSurfaceData* SurfaceData = Harness.Manager->GetChunkSurfaceData(Coord);
	TestTrue(TEXT("No surface points survive inside the cleared volume"),
		SurfaceData == nullptr || SurfaceData->SurfacePoints.Num() == 0);

	TestEqual(TEXT("No in-progress leak"), Harness.Manager->GetPendingGenerationCount(), 0);
	TestTrue(TEXT("Cleared volume registered for the chunk"),
		Harness.Manager->IsPointInClearedVolume(Coord, ChunkCenter(Coord)));

	return true;
}

// ---------------------------------------------------------------------------
// ClearScatterInRadius on ALREADY-CACHED scatter: spawn points AND cached
// surface points inside the radius are removed synchronously. The surface
// scrub is what stops distance streaming from resurrecting scatter in the
// dug area from pre-edit surface data.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterClearCachedTest,
	"VoxelWorlds.Scatter.Cleanup.ClearRemovesCachedPoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterClearCachedTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);
	const FIntVector Coord(0, 0, 0);
	const float ChunkWorldSize = TestChunkSize * TestVoxelSize;

	SubmitChunk(Harness.Manager, Coord);

	if (!PumpUntil(Harness.Manager, 10.0f, [&]()
		{ return Harness.Manager->HasScatterData(Coord) && Harness.Manager->GetPendingGenerationCount() == 0; }))
	{
		AddError(TEXT("Scatter generation did not complete - cannot test clearing"));
		return true;
	}

	const FChunkScatterData* ScatterData = Harness.Manager->GetChunkScatterData(Coord);
	const FChunkSurfaceData* SurfaceData = Harness.Manager->GetChunkSurfaceData(Coord);
	TestTrue(TEXT("Precondition: spawn points exist"), ScatterData && ScatterData->SpawnPoints.Num() > 0);
	TestTrue(TEXT("Precondition: surface points exist"), SurfaceData && SurfaceData->SurfacePoints.Num() > 0);

	// Clear everything - no pumping afterwards: removal must be synchronous
	Harness.Manager->ClearScatterInRadius(ChunkCenter(Coord), ChunkWorldSize * 2.0f);

	ScatterData = Harness.Manager->GetChunkScatterData(Coord);
	TestTrue(TEXT("All cached spawn points removed synchronously"),
		ScatterData == nullptr || ScatterData->SpawnPoints.Num() == 0);

	SurfaceData = Harness.Manager->GetChunkSurfaceData(Coord);
	TestTrue(TEXT("All cached surface points scrubbed synchronously"),
		SurfaceData == nullptr || SurfaceData->SurfacePoints.Num() == 0);

	return true;
}

// ---------------------------------------------------------------------------
// Completion queue drains without dropping results at max concurrency.
// Pre-fix, the dequeue-before-budget-check loop popped and dropped one result
// whenever 3+ results were queued in a single frame (possible at
// MaxAsyncScatterTasks 3-4), leaking its AsyncScatterInProgress entry forever:
// the pending count never returned to zero and that chunk never got scatter.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterQueueDrainTest,
	"VoxelWorlds.Scatter.Cleanup.CompletionQueueDrainsWithoutDrop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterQueueDrainTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(4); // max concurrency: 4 results can queue between Updates
	constexpr int32 NumChunks = 6;

	TArray<FIntVector> Coords;
	for (int32 i = 0; i < NumChunks; ++i)
	{
		Coords.Add(FIntVector(i, 0, 0));
		SubmitChunk(Harness.Manager, Coords.Last());
	}

	// Launch up to 4 tasks (2 per Update), then let them all finish so 4 results
	// sit in the completion queue when the next Update dequeues - the exact
	// boundary the pre-fix loop shape dropped a result at.
	Harness.Manager->Update(FVector::ZeroVector, 0.05f);
	Harness.Manager->Update(FVector::ZeroVector, 0.05f);
	FPlatformProcess::Sleep(0.5f);

	const bool bAllGenerated = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		if (Harness.Manager->GetPendingGenerationCount() != 0)
		{
			return false;
		}
		for (const FIntVector& Coord : Coords)
		{
			if (!Harness.Manager->HasScatterData(Coord))
			{
				return false;
			}
		}
		return true;
	});

	TestTrue(TEXT("All chunks generated scatter and the pipeline fully drained (no dropped results)"),
		bAllGenerated);

	return true;
}

// ---------------------------------------------------------------------------
// Placement filter: FScatterDefinition::SurfaceLocationMask selects which of the
// three surface categories (dry Surface / Underwater / Underground) a definition
// spawns on. Categories are mutually exclusive per point; the mask opts into any
// combination. Pure logic — no world needed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterMaskFilterTest,
	"VoxelWorlds.Scatter.Placement.SurfaceLocationMaskFilters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterMaskFilterTest::RunTest(const FString& Parameters)
{
	// Flat top-facing point in each category (constructor computes slope 0 for an up normal,
	// so only the surface-location mask differentiates these under a default definition).
	auto MakePoint = [](bool bUnderground, bool bUnderwater)
	{
		FVoxelSurfacePoint P(FVector::ZeroVector, FVector::UpVector, /*Material=*/0, /*Biome=*/0, EVoxelFaceType::Top);
		P.bIsUnderground = bUnderground;
		P.bIsUnderwater = bUnderwater;
		return P;
	};
	const FVoxelSurfacePoint Dry = MakePoint(false, false);
	const FVoxelSurfacePoint Underwater = MakePoint(false, true);
	const FVoxelSurfacePoint Underground = MakePoint(true, false);

	auto DefWithMask = [](int32 Mask)
	{
		FScatterDefinition Def;
		Def.SurfaceLocationMask = Mask;
		return Def;
	};

	const int32 SurfaceBit = static_cast<int32>(EScatterSurfaceLocationFlags::Surface);
	const int32 UnderwaterBit = static_cast<int32>(EScatterSurfaceLocationFlags::Underwater);
	const int32 UndergroundBit = static_cast<int32>(EScatterSurfaceLocationFlags::Underground);

	// Surface-only (the default mask)
	{
		const FScatterDefinition Def = DefWithMask(SurfaceBit);
		TestTrue(TEXT("Surface mask accepts dry"), Def.CanSpawnAt(Dry));
		TestFalse(TEXT("Surface mask rejects underwater"), Def.CanSpawnAt(Underwater));
		TestFalse(TEXT("Surface mask rejects underground"), Def.CanSpawnAt(Underground));
	}
	// Underwater-only
	{
		const FScatterDefinition Def = DefWithMask(UnderwaterBit);
		TestFalse(TEXT("Underwater mask rejects dry"), Def.CanSpawnAt(Dry));
		TestTrue(TEXT("Underwater mask accepts underwater"), Def.CanSpawnAt(Underwater));
		TestFalse(TEXT("Underwater mask rejects underground"), Def.CanSpawnAt(Underground));
	}
	// Underground-only
	{
		const FScatterDefinition Def = DefWithMask(UndergroundBit);
		TestFalse(TEXT("Underground mask rejects dry"), Def.CanSpawnAt(Dry));
		TestFalse(TEXT("Underground mask rejects underwater"), Def.CanSpawnAt(Underwater));
		TestTrue(TEXT("Underground mask accepts underground"), Def.CanSpawnAt(Underground));
	}
	// Surface + Underwater (e.g. an amphibious rock)
	{
		const FScatterDefinition Def = DefWithMask(SurfaceBit | UnderwaterBit);
		TestTrue(TEXT("Surface|Underwater accepts dry"), Def.CanSpawnAt(Dry));
		TestTrue(TEXT("Surface|Underwater accepts underwater"), Def.CanSpawnAt(Underwater));
		TestFalse(TEXT("Surface|Underwater rejects underground"), Def.CanSpawnAt(Underground));
	}
	// All three
	{
		const FScatterDefinition Def = DefWithMask(SurfaceBit | UnderwaterBit | UndergroundBit);
		TestTrue(TEXT("All accepts dry"), Def.CanSpawnAt(Dry));
		TestTrue(TEXT("All accepts underwater"), Def.CanSpawnAt(Underwater));
		TestTrue(TEXT("All accepts underground"), Def.CanSpawnAt(Underground));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Classification: the CPU voxel extractor tags each surface point as underwater
// or underground from the water/underground flag on the air voxel above it, with
// underground taking precedence. Drives the real async pipeline and inspects the
// cached surface data (independent of whether any definition spawns there).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterClassificationTest,
	"VoxelWorlds.Scatter.Classification.WaterAndUndergroundFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterClassificationTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);

	struct FCase { FIntVector Coord; bool bWater; bool bUnderground; const TCHAR* Name; };
	const FCase Cases[] = {
		{ FIntVector(0, 0, 0), false, false, TEXT("dry") },
		{ FIntVector(1, 0, 0), true,  false, TEXT("submerged") },
		{ FIntVector(2, 0, 0), false, true,  TEXT("underground") },
	};

	for (const FCase& C : Cases)
	{
		const FChunkMeshData EmptyMesh;
		Harness.Manager->OnChunkMeshDataReady(C.Coord, 0, EmptyMesh,
			MakeFlaggedGrassChunk(C.bWater, C.bUnderground), TestChunkSize, TestVoxelSize);
	}

	const bool bReady = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		if (Harness.Manager->GetPendingGenerationCount() != 0)
		{
			return false;
		}
		for (const FCase& C : Cases)
		{
			if (Harness.Manager->GetChunkSurfaceData(C.Coord) == nullptr)
			{
				return false;
			}
		}
		return true;
	});
	TestTrue(TEXT("All classification chunks extracted surface data"), bReady);

	for (const FCase& C : Cases)
	{
		const FChunkSurfaceData* Surface = Harness.Manager->GetChunkSurfaceData(C.Coord);
		if (!Surface || Surface->SurfacePoints.Num() == 0)
		{
			AddError(FString::Printf(TEXT("No surface points for %s chunk"), C.Name));
			continue;
		}

		int32 UnderwaterCount = 0;
		int32 UndergroundCount = 0;
		for (const FVoxelSurfacePoint& P : Surface->SurfacePoints)
		{
			if (P.bIsUnderwater) { ++UnderwaterCount; }
			if (P.bIsUnderground) { ++UndergroundCount; }
		}
		const int32 Total = Surface->SurfacePoints.Num();

		if (C.bUnderground)
		{
			TestEqual(FString::Printf(TEXT("%s: all points underground"), C.Name), UndergroundCount, Total);
			TestEqual(FString::Printf(TEXT("%s: underground wins over water"), C.Name), UnderwaterCount, 0);
		}
		else if (C.bWater)
		{
			TestEqual(FString::Printf(TEXT("%s: all points underwater"), C.Name), UnderwaterCount, Total);
			TestEqual(FString::Printf(TEXT("%s: no points underground"), C.Name), UndergroundCount, 0);
		}
		else
		{
			TestEqual(FString::Printf(TEXT("%s: no points underwater"), C.Name), UnderwaterCount, 0);
			TestEqual(FString::Printf(TEXT("%s: no points underground"), C.Name), UndergroundCount, 0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
