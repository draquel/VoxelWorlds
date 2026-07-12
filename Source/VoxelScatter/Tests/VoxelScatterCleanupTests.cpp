// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "VoxelScatterManager.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
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

	/**
	 * Two solid layers with an air gap (a cave) between them and NO underground flags set:
	 * an upper terrain surface (open sky) and a lower "cave floor" that is covered by the
	 * upper layer. Exercises the geometric "not the topmost transition in the column =
	 * underground" rule that classifies covered floors correctly even when
	 * ApplyUndergroundClassificationPass left them unflagged (thin/cross-chunk ceilings).
	 */
	static TArray<FVoxelData> MakeCoveredCaveChunk()
	{
		TArray<FVoxelData> Voxels;
		Voxels.Init(FVoxelData::Air(), TestChunkSize * TestChunkSize * TestChunkSize);

		auto SetSolidLayer = [&](int32 Z)
		{
			for (int32 Y = 0; Y < TestChunkSize; ++Y)
			{
				for (int32 X = 0; X < TestChunkSize; ++X)
				{
					Voxels[X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize] =
						FVoxelData::Solid(EVoxelMaterial::Grass);
				}
			}
		};

		// Lower layer (cave floor top at Z=3); air gap Z=[4,8) with NO flags; upper terrain
		// layer (surface top at Z=11); sky above. Assumes TestChunkSize >= 12.
		for (int32 Z = 0; Z < 4; ++Z)  { SetSolidLayer(Z); }
		for (int32 Z = 8; Z < 12; ++Z) { SetSolidLayer(Z); }
		return Voxels;
	}

	/**
	 * Minimal heightmap world mode that reports a fixed terrain-surface height. Lets a test chunk's
	 * local surface sit BELOW the analytic terrain height (as if the real surface / cave roof is in a
	 * neighboring chunk above), exercising the cross-chunk bCoveredByTerrain classification.
	 */
	struct FFixedHeightWorldMode : public IVoxelWorldMode
	{
		float Height = 0.0f;
		explicit FFixedHeightWorldMode(float InHeight) : Height(InHeight) {}
		virtual float GetTerrainHeightAt(float, float, const FVoxelNoiseParams&) const override { return Height; }
		virtual bool IsHeightmapBased() const override { return true; }
		virtual float GetDensityAt(const FVector&, int32, float) const override { return 0.0f; }
		virtual FIntVector WorldToChunkCoord(const FVector&, int32, float) const override { return FIntVector::ZeroValue; }
		virtual FVector ChunkCoordToWorld(const FIntVector&, int32, float, int32) const override { return FVector::ZeroVector; }
		virtual int32 GetMinZ() const override { return -100000; }
		virtual int32 GetMaxZ() const override { return 100000; }
		virtual EWorldMode GetWorldModeType() const override { return EWorldMode::InfinitePlane; }
		virtual uint8 GetMaterialAtDepth(const FVector&, float, float) const override { return 0; }
	};

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

// ---------------------------------------------------------------------------
// Covered-floor classification: a cave floor that has solid terrain above it is
// classified underground even when no underground FLAG is set on the cave air
// (the gap ApplyUndergroundClassificationPass leaves for thin/cross-chunk
// ceilings). The extractor's geometric "not the topmost transition = covered"
// rule is what prevents tall surface scatter (trees) from being placed on those
// floors and poking up through the roof.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterCoveredFloorTest,
	"VoxelWorlds.Scatter.Classification.CoveredFloorIsUnderground",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterCoveredFloorTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);
	const FIntVector Coord(0, 0, 0);

	const FChunkMeshData EmptyMesh;
	Harness.Manager->OnChunkMeshDataReady(Coord, 0, EmptyMesh, MakeCoveredCaveChunk(), TestChunkSize, TestVoxelSize);

	const bool bReady = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		return Harness.Manager->GetPendingGenerationCount() == 0
			&& Harness.Manager->GetChunkSurfaceData(Coord) != nullptr;
	});
	TestTrue(TEXT("Surface data extracted"), bReady);

	const FChunkSurfaceData* Surface = Harness.Manager->GetChunkSurfaceData(Coord);
	if (!Surface || Surface->SurfacePoints.Num() == 0)
	{
		AddError(TEXT("No surface points extracted from covered-cave chunk"));
		return true;
	}

	int32 SurfaceCount = 0;
	int32 UndergroundCount = 0;
	float MaxSurfaceZ = -FLT_MAX;
	float MaxUndergroundZ = -FLT_MAX;
	for (const FVoxelSurfacePoint& P : Surface->SurfacePoints)
	{
		if (P.bIsUnderground)
		{
			++UndergroundCount;
			MaxUndergroundZ = FMath::Max(MaxUndergroundZ, P.Position.Z);
		}
		else
		{
			++SurfaceCount;
			MaxSurfaceZ = FMath::Max(MaxSurfaceZ, P.Position.Z);
		}
	}

	// The upper terrain top is open sky (surface); the lower cave floor is covered
	// (underground) — even though no underground flag was ever set on the cave air.
	TestTrue(TEXT("Open-sky upper surface points exist"), SurfaceCount > 0);
	TestTrue(TEXT("Covered lower floor classified underground with no flag set"), UndergroundCount > 0);
	if (SurfaceCount > 0 && UndergroundCount > 0)
	{
		TestTrue(TEXT("The underground floor sits below the open-sky surface"), MaxUndergroundZ < MaxSurfaceZ);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Water-level classification: a terrain surface below the world water level is
// classified underwater even when the per-voxel water flag never propagated to
// it — keeping Surface-only scatter (e.g. trees) off submerged seabeds so it can't
// poke up through the water. Uses config WaterLevel only (no world mode needed).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterBelowWaterTest,
	"VoxelWorlds.Scatter.Classification.BelowWaterLevelIsUnderwater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterBelowWaterTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);
	// Water surface far above the flat-grass chunk's ~800 surface; the grass carries NO water flag.
	Harness.Config->bEnableWaterLevel = true;
	Harness.Config->WaterLevel = 100000.0f;

	const FIntVector Coord(0, 0, 0);
	const FChunkMeshData EmptyMesh;
	Harness.Manager->OnChunkMeshDataReady(Coord, 0, EmptyMesh, MakeFlatGrassChunk(), TestChunkSize, TestVoxelSize);

	const bool bReady = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		return Harness.Manager->GetPendingGenerationCount() == 0
			&& Harness.Manager->GetChunkSurfaceData(Coord) != nullptr;
	});
	TestTrue(TEXT("Surface data extracted"), bReady);

	const FChunkSurfaceData* Surface = Harness.Manager->GetChunkSurfaceData(Coord);
	if (!Surface || Surface->SurfacePoints.Num() == 0)
	{
		AddError(TEXT("No surface points extracted"));
		return true;
	}

	int32 UnderwaterCount = 0;
	int32 DryCount = 0;
	int32 UndergroundCount = 0;
	for (const FVoxelSurfacePoint& P : Surface->SurfacePoints)
	{
		if (P.bIsUnderground) { ++UndergroundCount; }
		else if (P.bIsUnderwater) { ++UnderwaterCount; }
		else { ++DryCount; }
	}
	TestEqual(TEXT("All submerged surface points are underwater"), UnderwaterCount, Surface->SurfacePoints.Num());
	TestEqual(TEXT("No dry surface points below the water line"), DryCount, 0);
	TestEqual(TEXT("Nothing classified underground"), UndergroundCount, 0);

	return true;
}

// ---------------------------------------------------------------------------
// Cross-chunk coverage: a surface point well below the ANALYTIC terrain height at
// its (X,Y) is covered by terrain living in a neighboring chunk, so it is
// classified underground even though it is the topmost transition in its own chunk
// and carries no underground flag. This is the fix for surface scatter on
// cross-chunk cave floors poking up through the terrain / water above.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterCrossChunkCoverTest,
	"VoxelWorlds.Scatter.Classification.CoveredByNeighborChunkIsUnderground",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterCrossChunkCoverTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	// Declared before the harness so it outlives the manager that holds a raw pointer to it.
	// Analytic terrain surface far ABOVE this chunk's local ~800 surface: the chunk is effectively
	// buried, so its surface points are covered by terrain in the chunk(s) above.
	FFixedHeightWorldMode WorldMode(100000.0f);

	FScatterTestHarness Harness(2);
	Harness.Manager->SetWorldMode(&WorldMode);

	const FIntVector Coord(0, 0, 0);
	const FChunkMeshData EmptyMesh;
	Harness.Manager->OnChunkMeshDataReady(Coord, 0, EmptyMesh, MakeFlatGrassChunk(), TestChunkSize, TestVoxelSize);

	const bool bReady = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		return Harness.Manager->GetPendingGenerationCount() == 0
			&& Harness.Manager->GetChunkSurfaceData(Coord) != nullptr;
	});
	TestTrue(TEXT("Surface data extracted"), bReady);

	const FChunkSurfaceData* Surface = Harness.Manager->GetChunkSurfaceData(Coord);
	if (!Surface || Surface->SurfacePoints.Num() == 0)
	{
		AddError(TEXT("No surface points extracted"));
		return true;
	}

	int32 UndergroundCount = 0;
	for (const FVoxelSurfacePoint& P : Surface->SurfacePoints)
	{
		if (P.bIsUnderground) { ++UndergroundCount; }
	}
	// Every topmost-in-chunk surface point is below the analytic surface → covered → underground.
	TestEqual(TEXT("Points below the analytic terrain surface are classified underground"),
		UndergroundCount, Surface->SurfacePoints.Num());

	Harness.Manager->SetWorldMode(nullptr);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
