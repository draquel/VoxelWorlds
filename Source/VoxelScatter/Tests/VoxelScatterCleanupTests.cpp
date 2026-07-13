// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterExclusionSubsystem.h"
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

// ---------------------------------------------------------------------------
// Edit-aware coverage is CHUNK-level, not per-point: a chunk whose TOP sits above the
// analytic terrain height is a surface chunk and must NOT be classified underground,
// even when its local (meshed) surface dips below that analytic height. The old
// per-point test buried such points, which pinned edit-exposed surfaces (a cave floor
// uncovered by carving the ceiling away) as underground against an edit-blind analytic
// height. Only a chunk entirely below the analytic surface is buried.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterSurfaceChunkNotBuriedTest,
	"VoxelWorlds.Scatter.Classification.SurfaceChunkNotBuriedByLocalHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterSurfaceChunkNotBuriedTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	// Analytic height above the flat chunk's ~800 surface (would trip the OLD per-point cover test:
	// 800 < 1000 - 150) but well below the chunk top (16 * 100 = 1600). Chunk-level test: not buried.
	FFixedHeightWorldMode WorldMode(1000.0f);

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
		Harness.Manager->SetWorldMode(nullptr);
		return true;
	}

	int32 UndergroundCount = 0;
	for (const FVoxelSurfacePoint& P : Surface->SurfacePoints)
	{
		if (P.bIsUnderground) { ++UndergroundCount; }
	}
	// The chunk top (1600) is above the analytic height (1000): the chunk is not buried, so its
	// open-sky surface stays surface despite sitting below the analytic height locally.
	TestEqual(TEXT("Surface chunk not buried by an analytic height below its top"), UndergroundCount, 0);

	Harness.Manager->SetWorldMode(nullptr);
	return true;
}

// ---------------------------------------------------------------------------
// Edit reclassification through the extractor: carving a cave ceiling away (modeled by
// removing the upper solid layer of the covered-cave chunk before extraction, exactly
// as edit-merged voxels would present it) leaves the former cave floor as the topmost
// transition. With the chunk-level coverage test it reclassifies to SURFACE instead of
// staying underground, so underground-only scatter (mushrooms) on a now-open floor is
// dropped. Guards the "mushrooms appear on the surface after an edit" report.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterExposedFloorTest,
	"VoxelWorlds.Scatter.Classification.ExposedFloorReclassifiesToSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterExposedFloorTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	// Analytic height above the cave floor (~350) but below the chunk top (1600): the edit-blind
	// procedural height a world reports where a cave once had a roof — what the old per-point test
	// wrongly buried the floor against.
	FFixedHeightWorldMode WorldMode(1000.0f);

	FScatterTestHarness Harness(2);
	Harness.Manager->SetWorldMode(&WorldMode);

	// Covered-cave chunk with its UPPER solid layer (Z=[8,12)) removed — the edit-merged voxels a
	// player sees after carving the ceiling away. The lower floor (top at Z=3) is now open sky.
	TArray<FVoxelData> Voxels = MakeCoveredCaveChunk();
	for (int32 Z = 8; Z < 12; ++Z)
	{
		for (int32 Y = 0; Y < TestChunkSize; ++Y)
		{
			for (int32 X = 0; X < TestChunkSize; ++X)
			{
				Voxels[X + Y * TestChunkSize + Z * TestChunkSize * TestChunkSize] = FVoxelData::Air();
			}
		}
	}

	const FIntVector Coord(0, 0, 0);
	const FChunkMeshData EmptyMesh;
	Harness.Manager->OnChunkMeshDataReady(Coord, 0, EmptyMesh, Voxels, TestChunkSize, TestVoxelSize);

	const bool bReady = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		return Harness.Manager->GetPendingGenerationCount() == 0
			&& Harness.Manager->GetChunkSurfaceData(Coord) != nullptr;
	});
	TestTrue(TEXT("Surface data extracted"), bReady);

	const FChunkSurfaceData* Surface = Harness.Manager->GetChunkSurfaceData(Coord);
	if (!Surface || Surface->SurfacePoints.Num() == 0)
	{
		AddError(TEXT("No surface points extracted from exposed-floor chunk"));
		Harness.Manager->SetWorldMode(nullptr);
		return true;
	}

	int32 UndergroundCount = 0;
	for (const FVoxelSurfacePoint& P : Surface->SurfacePoints)
	{
		if (P.bIsUnderground) { ++UndergroundCount; }
	}
	// The ceiling is gone: the former cave floor is now the topmost, open-sky transition in every
	// column and the chunk is not buried, so nothing is classified underground.
	TestEqual(TEXT("Exposed cave floor reclassifies to surface after the ceiling is carved away"),
		UndergroundCount, 0);

	Harness.Manager->SetWorldMode(nullptr);
	return true;
}

// ---------------------------------------------------------------------------
// Exclusion volumes: oriented-box containment. A yaw-rotated volume must accept
// points inside the rotated box and reject points that are inside its AABB but
// outside the oriented box (the case an AABB test would get wrong).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterExclusionOrientedBoxTest,
	"VoxelWorlds.Scatter.Exclusion.OrientedBoxContainment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterExclusionOrientedBoxTest::RunTest(const FString& Parameters)
{
	FScatterExclusionVolume Volume;
	Volume.Id = FGuid::NewGuid();
	Volume.Frame = FTransform(FRotator(0.0f, 45.0f, 0.0f), FVector(1000.0f, 1000.0f, 0.0f));
	Volume.HalfExtent = FVector(200.0f, 100.0f, 50.0f);

	TestTrue(TEXT("Volume with valid Id and extent is usable"), Volume.IsUsable());
	TestTrue(TEXT("Centre is inside"), Volume.ContainsPoint(FVector(1000.0f, 1000.0f, 0.0f)));

	// Along the rotated local +X axis (45° yaw): world dir (0.707, 0.707, 0). 190 units out stays
	// inside (|localX| = 190 <= 200); 210 units out is outside.
	const FVector DiagX = FVector(FMath::Sqrt(0.5f), FMath::Sqrt(0.5f), 0.0f);
	TestTrue(TEXT("Point 190 along rotated +X is inside"),
		Volume.ContainsPoint(FVector(1000.0f, 1000.0f, 0.0f) + DiagX * 190.0f));
	TestFalse(TEXT("Point 210 along rotated +X is outside"),
		Volume.ContainsPoint(FVector(1000.0f, 1000.0f, 0.0f) + DiagX * 210.0f));

	// A point along world +X at 190: local coords are (134, -134, 0) — inside the 200x100 box? No:
	// |localY| = 134 > 100, so the ORIENTED test rejects it even though it is inside the AABB.
	TestFalse(TEXT("AABB-inside but oriented-outside point is rejected"),
		Volume.ContainsPoint(FVector(1190.0f, 1000.0f, 0.0f)));

	// Z extent respected
	TestFalse(TEXT("Point above the box is outside"), Volume.ContainsPoint(FVector(1000.0f, 1000.0f, 60.0f)));

	// AABB encloses the rotated box: rotated corners reach ±(150+70.7...) ≈ ±212 on X/Y
	const FBox Bounds = Volume.GetWorldBounds();
	TestTrue(TEXT("AABB contains a rotated corner"),
		Bounds.IsInside(FVector(1000.0f, 1000.0f, 0.0f) + DiagX * 199.0f));

	// Invalid configurations are rejected
	FScatterExclusionVolume NoId = Volume;
	NoId.Id.Invalidate();
	TestFalse(TEXT("Volume without Id is unusable"), NoId.IsUsable());
	FScatterExclusionVolume FlatExtent = Volume;
	FlatExtent.HalfExtent.Z = 0.0f;
	TestFalse(TEXT("Volume with zero extent is unusable"), FlatExtent.IsUsable());

	return true;
}

// ---------------------------------------------------------------------------
// Exclusion volumes: a chunk generated WHILE a volume covers it is born bare —
// the fresh async result's spawn points are filtered at consumption, while its
// surface points are cached untouched (so a later unregister could regrow).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterExclusionBornBareTest,
	"VoxelWorlds.Scatter.Exclusion.ChunkBornBareInsideVolume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterExclusionBornBareTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);
	const FIntVector Coord(0, 0, 0);

	// Volume covering the whole test chunk (world extent [0,1600] per axis)
	FScatterExclusionVolume Volume;
	Volume.Id = FGuid::NewGuid();
	Volume.Frame = FTransform(FVector(800.0f, 800.0f, 800.0f));
	Volume.HalfExtent = FVector(800.0f, 800.0f, 800.0f);
	TestTrue(TEXT("Volume registered"), Harness.Manager->RegisterScatterExclusionVolume(Volume));

	// Submit the chunk AFTER the volume exists — the fresh generation must come out bare
	const FChunkMeshData EmptyMesh;
	Harness.Manager->OnChunkMeshDataReady(Coord, 0, EmptyMesh, MakeFlatGrassChunk(), TestChunkSize, TestVoxelSize);

	const bool bReady = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		return Harness.Manager->GetPendingGenerationCount() == 0
			&& Harness.Manager->GetChunkSurfaceData(Coord) != nullptr;
	});
	TestTrue(TEXT("Surface data extracted"), bReady);

	const FChunkSurfaceData* Surface = Harness.Manager->GetChunkSurfaceData(Coord);
	TestTrue(TEXT("Surface points cached untouched (exclusion never strips surface data)"),
		Surface && Surface->SurfacePoints.Num() > 0);

	const FChunkScatterData* Scatter = Harness.Manager->GetChunkScatterData(Coord);
	const int32 SpawnCount = Scatter ? Scatter->SpawnPoints.Num() : 0;
	TestEqual(TEXT("Chunk born bare: no spawn points inside the volume"), SpawnCount, 0);

	return true;
}

// ---------------------------------------------------------------------------
// Exclusion volumes: registering over an already-populated chunk clears its
// cached spawn points immediately (live suppression), and unregistering regrows
// them from the cached surface data. Deterministic chunk seeds make the regrown
// set identical to the original — the count must match exactly.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterExclusionClearRegrowTest,
	"VoxelWorlds.Scatter.Exclusion.RegisterClearsUnregisterRegrows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterExclusionClearRegrowTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);
	const FIntVector Coord(0, 0, 0);

	// Generate scatter normally first
	SubmitChunk(Harness.Manager, Coord);
	const bool bReady = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		return Harness.Manager->GetPendingGenerationCount() == 0
			&& Harness.Manager->HasScatterData(Coord);
	});
	TestTrue(TEXT("Initial scatter generated"), bReady);

	const FChunkScatterData* Scatter = Harness.Manager->GetChunkScatterData(Coord);
	const int32 OriginalCount = Scatter ? Scatter->SpawnPoints.Num() : 0;
	if (OriginalCount == 0)
	{
		AddError(TEXT("Fixture produced no spawn points — cannot exercise clear/regrow"));
		return true;
	}

	// Register a whole-chunk volume: cached spawn points must clear synchronously
	FScatterExclusionVolume Volume;
	Volume.Id = FGuid::NewGuid();
	Volume.Frame = FTransform(FVector(800.0f, 800.0f, 800.0f));
	Volume.HalfExtent = FVector(800.0f, 800.0f, 800.0f);
	TestTrue(TEXT("Volume registered"), Harness.Manager->RegisterScatterExclusionVolume(Volume));
	TestTrue(TEXT("Manager reports the volume"), Harness.Manager->HasScatterExclusionVolume(Volume.Id));

	Scatter = Harness.Manager->GetChunkScatterData(Coord);
	TestEqual(TEXT("Register cleared the chunk's spawn points immediately"),
		Scatter ? Scatter->SpawnPoints.Num() : 0, 0);

	// Surface data must survive the clear — it is what regrow replays from
	const FChunkSurfaceData* Surface = Harness.Manager->GetChunkSurfaceData(Coord);
	TestTrue(TEXT("Surface cache survives the clear"), Surface && Surface->SurfacePoints.Num() > 0);

	// Unregister: the chunk regrows via async placement from the cached surface data.
	// Deterministic seeds → the regrown count matches the original exactly.
	TestTrue(TEXT("Volume unregistered"), Harness.Manager->UnregisterScatterExclusionVolume(Volume.Id));
	TestFalse(TEXT("Manager no longer reports the volume"), Harness.Manager->HasScatterExclusionVolume(Volume.Id));

	const bool bRegrown = PumpUntil(Harness.Manager, 15.0f, [&]()
	{
		const FChunkScatterData* Data = Harness.Manager->GetChunkScatterData(Coord);
		return Data && Data->SpawnPoints.Num() == OriginalCount;
	});
	TestTrue(FString::Printf(TEXT("Foliage regrew to the original %d spawn points"), OriginalCount), bRegrown);

	return true;
}

// ---------------------------------------------------------------------------
// Exclusion volumes: the world-subsystem facade forwards to live managers and
// REPLAYS its cached volumes to managers that initialize later — registration
// order between volume producers (claims) and the voxel world must not matter.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterExclusionSubsystemReplayTest,
	"VoxelWorlds.Scatter.Exclusion.SubsystemForwardsAndReplays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterExclusionSubsystemReplayTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterCleanupTestUtils;

	FScatterTestHarness Harness(2);

	UVoxelScatterExclusionSubsystem* Subsystem = Harness.World->GetSubsystem<UVoxelScatterExclusionSubsystem>();
	if (!Subsystem)
	{
		AddError(TEXT("World has no UVoxelScatterExclusionSubsystem"));
		return true;
	}

	FScatterExclusionVolume Volume;
	Volume.Id = FGuid::NewGuid();
	Volume.Frame = FTransform(FVector(800.0f, 800.0f, 800.0f));
	Volume.HalfExtent = FVector(800.0f, 800.0f, 800.0f);

	// Forward: the harness manager registered itself on Initialize, so it receives the volume live
	TestTrue(TEXT("Subsystem accepted the volume"), Subsystem->RegisterVolume(Volume));
	TestTrue(TEXT("Live manager received the forwarded volume"),
		Harness.Manager->HasScatterExclusionVolume(Volume.Id));

	// Replay: a manager that initializes AFTER the volume was registered receives it on init
	UVoxelScatterManager* LateManager = NewObject<UVoxelScatterManager>();
	LateManager->AddToRoot();
	LateManager->Initialize(Harness.Config, Harness.World);
	TestTrue(TEXT("Late-initializing manager received the replayed volume"),
		LateManager->HasScatterExclusionVolume(Volume.Id));

	// Unregister reaches both managers
	TestTrue(TEXT("Subsystem removed the volume"), Subsystem->UnregisterVolume(Volume.Id));
	TestFalse(TEXT("Live manager volume removed"), Harness.Manager->HasScatterExclusionVolume(Volume.Id));
	TestFalse(TEXT("Late manager volume removed"), LateManager->HasScatterExclusionVolume(Volume.Id));

	LateManager->Shutdown();
	LateManager->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
