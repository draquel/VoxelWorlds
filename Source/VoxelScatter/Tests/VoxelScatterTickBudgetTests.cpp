// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Engine/StaticMesh.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterTypes.h"
#include "VoxelTickBudget.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G295: the scatter's distance pass and the applies of its completed extractions within the chunk manager's tick budget.
//
// After a teleport onto fresh ground the scatter's ticks ran 2 to 7.5 ms for half a second: the distance pass released the
// place left's types in one pass (651 releases, 2.2 ms on VoxelDemo) and the applies of the first extractions came on top
// of the HISM writes' own budget. Under a limited budget (FVoxelTickBudget, voxel.Scatter.TickBudget 1) the applies, the
// distance results' applies and the distance pass's releases, hand-offs at the horizon and launches each do one piece a
// tick at least, the nearest a viewer first, and leave the rest for the next tick, where a pass cut short resumes.
//
// Through the manager's own pipeline on the tilted heightmap plane with the test definitions (grass and rocks at
// ScatterRadius 10,000, trees with a SpawnDistance of 20,000), with a budget always spent (its clock moves a second at
// every read), so every part does exactly its first piece a tick:
// - ApplyNearestFirst: four chunks handed off in the order 3, 1, 2, 0 along +X, their extractions all finished before the
//   applies: under the budget one apply a tick, the nearest first (0, 1, 2, 3); without it two a tick (the cap) in the
//   order they arrived; the same spawn points either way. A hand-off of a shared snapshot keeps the snapshot (no copy) and
//   gives the same spawn points as a hand-off of an array.
// - DistanceReleases: six chunks scattered, the viewer gone far: without a budget one pass releases all six; under it the
//   pass queues six and releases one a tick, a chunk unloaded meanwhile has its queued release dropped, a chunk the viewer
//   came back to before its release keeps its types; back near, the same spawn points as before and each drawn once.
// - SameAsUnbudgeted: a walk over a grid of chunks (near, the trees' range, far, near, the other side, a chunk unloaded
//   and loaded again) with the budget and without: at every stop the same spawn points per chunk, each drawn once, every
//   piece of work finished (nothing starved), and under the budget the distance pass cut short and resumed.
// ---------------------------------------------------------------------------

namespace VoxelScatterTickBudgetTestUtils
{
	using namespace VoxelScatterTesting;

	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr float ScatterRadius = 10000.0f;
	constexpr int32 GrassID = 0;
	constexpr int32 RockID = 1;
	constexpr int32 TreeID = 2; // VoxelScatterTesting::SmoothDefinitions' trees (SpawnDistance 20,000)

	FVector ChunkCenter(const FIntVector& Coord) { return (FVector(Coord) + FVector(0.5)) * ChunkWorldSize; }

	/** A clock that moves a second at every read: a budget of 1 ms on it is spent from its first check on. */
	double SteppingClock()
	{
		static double Now = 0.0;
		Now += 1.0;
		return Now;
	}

	/** A chunk of the plane: grass, with every third column dirt (rocks grow there). */
	TArray<FVoxelData> MakeChunk(const FIntVector& Coord)
	{
		return MakeHeightmapChunk(FVector(Coord) * ChunkWorldSize, ChunkSize, VoxelSize,
			[](double X, double Y) { return TiltedPlane().GetTerrainHeightAt(X, Y, FVoxelNoiseParams()); },
			[&Coord](int32 X, int32 Y) { return static_cast<uint8>(((Coord.X * ChunkSize + X) + (Coord.Y * ChunkSize + Y)) % 3 == 0 ? EVoxelMaterial::Dirt : EVoxelMaterial::Grass); });
	}

	FString SpawnKey(const FScatterSpawnPoint& Point)
	{
		return FString::Printf(TEXT("%d %d %d %d %d %d"), Point.ScatterTypeID,
			FMath::RoundToInt32(Point.Position.X * 10.0), FMath::RoundToInt32(Point.Position.Y * 10.0), FMath::RoundToInt32(Point.Position.Z * 10.0),
			FMath::RoundToInt32(Point.RotationYaw * 100.0f), FMath::RoundToInt32(Point.Scale * 1000.0f));
	}

	/** What the manager holds for one chunk: its spawn points (sorted keys) and its instances. */
	struct FChunkState
	{
		TArray<FString> Spawns;
		int32 Grass = 0;
		int32 Rocks = 0;
		int32 Trees = 0;
		int32 Instances = 0;

		bool operator==(const FChunkState& Other) const { return Spawns == Other.Spawns && Instances == Other.Instances; }
	};

	/** A game world and a scatter manager on the plane; budgeted, each Update gets a budget already spent. */
	struct FHarness : VoxelScatterTesting::FScatterTestWorld
	{
		FVector Viewer = FVector::ZeroVector;
		TSet<FIntVector> Loaded;
		bool bBudgeted = false;
		FVoxelTickBudget Budget;
		/** The chunks in the order their extractions were applied (OnChunkScatterReady) */
		TArray<FIntVector> ApplyOrder;
		/** Ticks that left a piece of the distance pass for a later tick, and a pass's hand-off or launch among them */
		int32 TicksWithDistanceHeld = 0;
		/** Applies in one Update at most, and extractions waiting after one at most */
		int32 MaxAppliedInATick = 0;
		int32 MaxReleasedInATick = 0;

		FHarness(const TCHAR* WorldName, bool bInBudgeted, int32 MaxAsyncTasks = 2)
			: FScatterTestWorld(WorldName)
			, bBudgeted(bInBudgeted)
		{
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = ScatterRadius;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Config->MaxAsyncScatterTasks = MaxAsyncTasks;
			StartManager(/*bSmoothDefinitions=*/true);
			Manager->SetSurfacePointSpacing(VoxelSize);
			UseWorldMode(TiltedPlaneMode());
			for (const int32 ID : { GrassID, RockID, TreeID })
			{
				if (const FScatterDefinition* Def = Manager->GetScatterDefinition(ID))
				{
					FScatterDefinition Copy = *Def;
					Copy.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
					Copy.Density = ID == TreeID ? 0.3f : Copy.Density;
					Manager->AddScatterDefinition(Copy);
				}
			}
			// The chunk manager's requester: a loaded chunk is handed off again from its voxels
			Manager->SetChunkHandOffRequester([this](const FIntVector& Coord) -> EVoxelScatterHandOff
			{
				if (!Loaded.Contains(Coord))
				{
					return EVoxelScatterHandOff::Gone;
				}
				Manager->HandOffChunkVoxels(Coord, MakeChunk(Coord), ChunkSize, VoxelSize);
				return EVoxelScatterHandOff::HandedOff;
			});
			Manager->OnChunkScatterReady.AddLambda([this](const FIntVector& Coord, int32) { ApplyOrder.Add(Coord); });
		}

		void Update(float DeltaTime = 0.05f)
		{
			const FVoxelTickBudget* TickBudget = nullptr;
			if (bBudgeted)
			{
				Budget = FVoxelTickBudget(1.0, &SteppingClock);
				TickBudget = &Budget;
			}
			Manager->Update(Viewer, DeltaTime, TickBudget);
			const UVoxelScatterManager::FUpdateTimings& T = Manager->GetLastUpdateTimings();
			MaxAppliedInATick = FMath::Max(MaxAppliedInATick, T.Applied);
			MaxReleasedInATick = FMath::Max(MaxReleasedInATick, T.Released);
			TicksWithDistanceHeld += T.DistanceHeld > 0 ? 1 : 0;
		}

		void Load(const FIntVector& Coord)
		{
			Loaded.Add(Coord);
			Manager->HandOffChunkVoxels(Coord, MakeChunk(Coord), ChunkSize, VoxelSize);
		}

		void Unload(const FIntVector& Coord)
		{
			Loaded.Remove(Coord);
			Manager->OnChunkUnloaded(Coord);
		}

		/** Every piece of the scatter's work done: the manager's and the renderer's queued adds */
		bool IsSettled() const
		{
			const UVoxelScatterRenderer* Renderer = Manager->GetScatterRenderer();
			return Manager->GetOutstandingWorkCount() == 0 && (!Renderer || Renderer->GetPendingInstanceAddCount() == 0);
		}

		/** Move the viewer and pump until every piece of work is done and the distance passes have had a second; false
		 *  when work was still outstanding after 20 s (starved) */
		bool Settle(const FVector& InViewer)
		{
			Viewer = InViewer;
			const double Start = FPlatformTime::Seconds();
			int32 Ticks = 0;
			bool bSettled = false;
			while (FPlatformTime::Seconds() < Start + 20.0)
			{
				Update();
				++Ticks;
				// At least 24 ticks (1.2 s of 0.05 s ticks: four distance passes) and every piece done
				if (Ticks >= 24 && IsSettled())
				{
					bSettled = true;
					break;
				}
				FPlatformProcess::Sleep(0.002f);
			}
			return bSettled;
		}

		FChunkState State(const FIntVector& Coord) const
		{
			FChunkState Out;
			if (const FChunkScatterData* Scatter = Manager->GetChunkScatterData(Coord))
			{
				for (const FScatterSpawnPoint& Point : Scatter->SpawnPoints)
				{
					Out.Spawns.Add(SpawnKey(Point));
					Out.Grass += Point.ScatterTypeID == GrassID ? 1 : 0;
					Out.Rocks += Point.ScatterTypeID == RockID ? 1 : 0;
					Out.Trees += Point.ScatterTypeID == TreeID ? 1 : 0;
				}
				Out.Spawns.Sort();
			}
			const UVoxelScatterRenderer* Renderer = Manager->GetScatterRenderer();
			Out.Instances = Renderer ? Renderer->GetChunkInstanceCount(Coord) : 0;
			return Out;
		}
	};

	FString Describe(const FIntVector& Coord, const FChunkState& S)
	{
		return FString::Printf(TEXT("(%d,%d,%d): %d spawn points (%d grass, %d rocks, %d trees), %d instances"),
			Coord.X, Coord.Y, Coord.Z, S.Spawns.Num(), S.Grass, S.Rocks, S.Trees, S.Instances);
	}

	FString CoordsText(const TArray<FIntVector>& Coords)
	{
		TArray<FString> Parts;
		for (const FIntVector& C : Coords)
		{
			Parts.Add(FString::Printf(TEXT("(%d,%d,%d)"), C.X, C.Y, C.Z));
		}
		return FString::Join(Parts, TEXT(" "));
	}

	/** Wait (no Update) until Expected extractions have arrived; returns the extractions waiting. */
	int32 WaitForArrivals(UVoxelScatterManager& Manager, int32 Expected)
	{
		const double Start = FPlatformTime::Seconds();
		int32 Waiting = Manager.TakeArrivedResults();
		while (Waiting < Expected && FPlatformTime::Seconds() < Start + 20.0)
		{
			FPlatformProcess::Sleep(0.005f);
			Waiting = Manager.TakeArrivedResults();
		}
		return Waiting;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterTickBudgetApplyNearestFirstTest, "VoxelWorlds.Scatter.TickBudget.ApplyNearestFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterTickBudgetApplyNearestFirstTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterTickBudgetTestUtils;

	const TArray<FIntVector> Order = { FIntVector(3, 0, 0), FIntVector(1, 0, 0), FIntVector(2, 0, 0), FIntVector(0, 0, 0) };
	const FVector Viewer = ChunkCenter(FIntVector(0, 0, 0));

	// Hand four chunks off, launch all four extractions in one tick, wait for all of them, then apply tick by tick
	TMap<FIntVector, FChunkState> States[2];
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bBudgeted = Pass == 1;
		FHarness H(bBudgeted ? TEXT("VoxelScatterTickBudgetApplyBudgetedWorld") : TEXT("VoxelScatterTickBudgetApplyFreeWorld"), bBudgeted,
			/*MaxAsyncTasks=*/4);
		H.Manager->SetMaxScatterGenerationsPerFrame(0);
		H.Viewer = Viewer;
		H.Update(0.0f); // the manager's viewer, for the hand-offs' distances
		for (const FIntVector& Coord : Order)
		{
			H.Loaded.Add(Coord);
			if (bBudgeted)
			{
				// The shared snapshot (the chunk manager's hand-off since G295): the queued extraction keeps it, no copy
				const TSharedPtr<const TArray<FVoxelData>> Snapshot = MakeShared<TArray<FVoxelData>>(MakeChunk(Coord));
				H.Manager->HandOffChunkVoxels(Coord, Snapshot, ChunkSize, VoxelSize);
				TestEqual(FString::Printf(TEXT("A snapshot handed off is kept, not copied (%d,%d,%d)"), Coord.X, Coord.Y, Coord.Z),
					Snapshot.GetSharedReferenceCount(), 2);
			}
			else
			{
				H.Manager->HandOffChunkVoxels(Coord, MakeChunk(Coord), ChunkSize, VoxelSize);
			}
		}
		H.Update(0.0f); // launches the four (nothing has arrived to apply)
		TestEqual(TEXT("All four extractions launched in one tick"), static_cast<int32>(H.Manager->GetLastUpdateTimings().Launched), 4);
		const int32 Arrived = WaitForArrivals(*H.Manager, 4);
		TestEqual(TEXT("All four extractions arrived before an apply"), Arrived, 4);

		TArray<int32> AppliedPerTick;
		for (int32 Tick = 0; Tick < 8 && H.ApplyOrder.Num() < 4; ++Tick)
		{
			H.Update(0.0f);
			AppliedPerTick.Add(H.Manager->GetLastUpdateTimings().Applied);
		}
		AddInfo(FString::Printf(TEXT("%s: applied in the order %s, per tick %s"), bBudgeted ? TEXT("budget spent") : TEXT("no budget"),
			*CoordsText(H.ApplyOrder), *FString::JoinBy(AppliedPerTick, TEXT(" "), [](int32 N) { return FString::FromInt(N); })));
		TestEqual(TEXT("Every extraction applied"), H.ApplyOrder.Num(), 4);
		if (bBudgeted)
		{
			const TArray<FIntVector> Nearest = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(2, 0, 0), FIntVector(3, 0, 0) };
			TestTrue(TEXT("Under the budget the nearest applied first"), H.ApplyOrder == Nearest);
			TestEqual(TEXT("Under the budget one apply a tick"), AppliedPerTick.Num() >= 4 ? AppliedPerTick[0] + AppliedPerTick[1] + AppliedPerTick[2] + AppliedPerTick[3] : 0, 4);
			TestEqual(TEXT("Under the budget never more than one apply a tick"), H.MaxAppliedInATick, 1);
		}
		else
		{
			TestEqual(TEXT("Without a budget two applies a tick (the cap)"), H.MaxAppliedInATick, 2);
			TestEqual(TEXT("Without a budget the four in two ticks"), AppliedPerTick.Num(), 2);
		}
		TestTrue(TEXT("Every piece of work finished"), H.Settle(Viewer));
		for (const FIntVector& Coord : Order)
		{
			States[Pass].Add(Coord, H.State(Coord));
			AddInfo(FString::Printf(TEXT("%s %s"), bBudgeted ? TEXT("budget spent") : TEXT("no budget"), *Describe(Coord, States[Pass][Coord])));
		}
	}
	for (const FIntVector& Coord : Order)
	{
		const FChunkState& Free = States[0][Coord];
		const FChunkState& Held = States[1][Coord];
		TestTrue(FString::Printf(TEXT("(%d,%d,%d) has grass and trees"), Coord.X, Coord.Y, Coord.Z), Free.Grass > 0 && Free.Trees > 0);
		TestTrue(FString::Printf(TEXT("(%d,%d,%d): the same spawn points and instances with the budget (and a shared snapshot) as without"),
			Coord.X, Coord.Y, Coord.Z), Free == Held);
		TestEqual(FString::Printf(TEXT("(%d,%d,%d): each spawn point drawn once"), Coord.X, Coord.Y, Coord.Z), Held.Instances, Held.Spawns.Num());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterTickBudgetDistanceReleasesTest, "VoxelWorlds.Scatter.TickBudget.DistanceReleases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterTickBudgetDistanceReleasesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterTickBudgetTestUtils;

	TArray<FIntVector> Chunks;
	for (int32 X = 0; X < 3; ++X)
	{
		for (int32 Y = 0; Y < 2; ++Y)
		{
			Chunks.Add(FIntVector(X, Y, 0));
		}
	}
	const FVector Near = ChunkCenter(FIntVector(1, 0, 0));
	const FVector Far = Near + FVector(60000.0, 0.0, 0.0); // beyond every type's cleanup distance (trees 22,000)

	// The reference: scattered near, without a budget
	TMap<FIntVector, FChunkState> Reference;
	{
		FHarness Free(TEXT("VoxelScatterTickBudgetReleaseFreeWorld"), /*bBudgeted=*/false);
		Free.Viewer = Near;
		for (const FIntVector& C : Chunks)
		{
			Free.Load(C);
		}
		TestTrue(TEXT("No budget: scattered near"), Free.Settle(Near));
		for (const FIntVector& C : Chunks)
		{
			Reference.Add(C, Free.State(C));
			TestTrue(FString::Printf(TEXT("No budget: (%d,%d,%d) scattered"), C.X, C.Y, C.Z), Reference[C].Spawns.Num() > 0);
		}
		// Far: the next pass releases all six chunks at once
		Free.MaxReleasedInATick = 0;
		Free.Viewer = Far;
		Free.Update(0.3f);
		TestEqual(TEXT("No budget: one pass releases all six chunks"), static_cast<int32>(Free.Manager->GetLastUpdateTimings().Released), 6);
		TestEqual(TEXT("No budget: nothing left for a later tick"), static_cast<int32>(Free.Manager->GetLastUpdateTimings().DistanceHeld), 0);
	}

	FHarness H(TEXT("VoxelScatterTickBudgetReleaseWorld"), /*bBudgeted=*/true);
	H.Viewer = Near;
	for (const FIntVector& C : Chunks)
	{
		H.Load(C);
	}
	TestTrue(TEXT("Budget spent: scattered near, every piece finished"), H.Settle(Near));
	for (const FIntVector& C : Chunks)
	{
		TestTrue(FString::Printf(TEXT("Budget spent: (%d,%d,%d) as without a budget"), C.X, C.Y, C.Z), H.State(C) == Reference[C]);
	}

	// Far: the pass queues the six and releases one; each tick after one more
	H.Viewer = Far;
	H.Update(0.3f);
	TestEqual(TEXT("Budget spent: the pass releases one chunk"), static_cast<int32>(H.Manager->GetLastUpdateTimings().Released), 1);
	TestEqual(TEXT("Budget spent: five left for later ticks"), static_cast<int32>(H.Manager->GetLastUpdateTimings().DistanceHeld), 5);
	int32 Released = 0;
	for (const FIntVector& C : Chunks)
	{
		Released += H.State(C).Spawns.Num() == 0 ? 1 : 0;
	}
	TestEqual(TEXT("Budget spent: one chunk's spawn points gone, five still there"), Released, 1);

	// A chunk unloaded with its release queued: its entry goes with it
	FIntVector Unloaded = FIntVector::ZeroValue;
	FIntVector Kept = FIntVector::ZeroValue;
	{
		TArray<FIntVector> Waiting;
		for (const FIntVector& C : Chunks)
		{
			if (H.State(C).Spawns.Num() > 0)
			{
				Waiting.Add(C);
			}
		}
		Unloaded = Waiting[0];
		Kept = Waiting[1];
	}
	H.Unload(Unloaded);
	H.Update(0.01f);
	TestEqual(TEXT("Budget spent: the next tick releases one more"), static_cast<int32>(H.Manager->GetLastUpdateTimings().Released), 1);
	TestEqual(TEXT("Budget spent: the unloaded chunk's queued release dropped (three left)"),
		static_cast<int32>(H.Manager->GetLastUpdateTimings().DistanceHeld), 3);

	// The viewer back near before the rest are made: a queued chunk keeps its types
	const bool bKeptWaiting = H.State(Kept).Spawns.Num() > 0;
	H.Viewer = Near;
	H.Update(0.01f);
	if (bKeptWaiting)
	{
		TestTrue(TEXT("Budget spent: a chunk the viewer came back to before its release keeps its spawn points"),
			H.State(Kept) == Reference[Kept]);
	}
	TestTrue(TEXT("Budget spent: back near, every piece finished"), H.Settle(Near));
	H.Load(Unloaded);
	TestTrue(TEXT("Budget spent: the unloaded chunk loaded again"), H.Settle(Near));
	for (const FIntVector& C : Chunks)
	{
		const FChunkState S = H.State(C);
		AddInfo(FString::Printf(TEXT("back near %s"), *Describe(C, S)));
		TestTrue(FString::Printf(TEXT("Back near: (%d,%d,%d) the same spawn points and instances as before"), C.X, C.Y, C.Z), S == Reference[C]);
	}

	// Far again and settled: every type of every chunk released, a tick at a time
	H.MaxReleasedInATick = 0;
	TestTrue(TEXT("Far again: every piece finished"), H.Settle(Far));
	TestEqual(TEXT("Far again: never more than one release a tick"), H.MaxReleasedInATick, 1);
	for (const FIntVector& C : Chunks)
	{
		const FChunkState S = H.State(C);
		TestEqual(FString::Printf(TEXT("Far again: (%d,%d,%d) no spawn point"), C.X, C.Y, C.Z), S.Spawns.Num(), 0);
		TestEqual(FString::Printf(TEXT("Far again: (%d,%d,%d) no instance"), C.X, C.Y, C.Z), S.Instances, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterTickBudgetSameAsUnbudgetedTest, "VoxelWorlds.Scatter.TickBudget.SameAsUnbudgeted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterTickBudgetSameAsUnbudgetedTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterTickBudgetTestUtils;

	// A grid of 4 x 2 chunks; the stops of a walk (the trees reach 20,000, the radius types 10,000)
	TArray<FIntVector> Chunks;
	for (int32 X = 0; X < 4; ++X)
	{
		for (int32 Y = 0; Y < 2; ++Y)
		{
			Chunks.Add(FIntVector(X, Y, 0));
		}
	}
	const FVector Origin = ChunkCenter(FIntVector(1, 0, 0));
	struct FStop
	{
		const TCHAR* Label;
		FVector Viewer;
	};
	const TArray<FStop> Stops = {
		{ TEXT("loaded far (the window's front)"), Origin + FVector(40000.0, 0.0, 0.0) },
		{ TEXT("the trees' range"), Origin + FVector(15000.0, 0.0, 0.0) },
		{ TEXT("near"), Origin },
		{ TEXT("far"), Origin + FVector(-60000.0, 0.0, 0.0) },
		{ TEXT("the other side, the trees' range"), Origin + FVector(0.0, -16000.0, 0.0) },
		{ TEXT("near again"), Origin + FVector(0.0, 1600.0, 0.0) },
	};

	TArray<TMap<FIntVector, FChunkState>> Runs[2];
	int32 TicksHeld[2] = { 0, 0 };
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bBudgeted = Pass == 1;
		FHarness H(bBudgeted ? TEXT("VoxelScatterTickBudgetWalkBudgetedWorld") : TEXT("VoxelScatterTickBudgetWalkFreeWorld"), bBudgeted);
		H.Viewer = Stops[0].Viewer;
		H.Update();
		for (const FIntVector& C : Chunks)
		{
			H.Load(C);
		}
		for (int32 StopIndex = 0; StopIndex < Stops.Num(); ++StopIndex)
		{
			if (StopIndex == 4)
			{
				// A chunk unloaded and loaded again on the way
				H.Unload(Chunks[3]);
				H.Update();
				H.Load(Chunks[3]);
			}
			TestTrue(FString::Printf(TEXT("%s, %s: every piece of work finished"), bBudgeted ? TEXT("budget spent") : TEXT("no budget"),
				Stops[StopIndex].Label), H.Settle(Stops[StopIndex].Viewer));
			TMap<FIntVector, FChunkState>& AtStop = Runs[Pass].AddDefaulted_GetRef();
			for (const FIntVector& C : Chunks)
			{
				AtStop.Add(C, H.State(C));
			}
		}
		TicksHeld[Pass] = H.TicksWithDistanceHeld;
		AddInfo(FString::Printf(TEXT("%s: %d ticks left a piece of the distance pass for later, at most %d applies and %d releases a tick"),
			bBudgeted ? TEXT("budget spent") : TEXT("no budget"), H.TicksWithDistanceHeld, H.MaxAppliedInATick, H.MaxReleasedInATick));
	}

	TestEqual(TEXT("No budget: nothing left for a later tick"), TicksHeld[0], 0);
	TestTrue(TEXT("Budget spent: the distance pass was cut short and resumed"), TicksHeld[1] > 0);
	for (int32 StopIndex = 0; StopIndex < Stops.Num(); ++StopIndex)
	{
		int32 Points = 0;
		for (const FIntVector& C : Chunks)
		{
			const FChunkState& Free = Runs[0][StopIndex][C];
			const FChunkState& Held = Runs[1][StopIndex][C];
			Points += Free.Spawns.Num();
			if (!(Free == Held))
			{
				AddInfo(FString::Printf(TEXT("%s: no budget %s; budget spent %s"), Stops[StopIndex].Label, *Describe(C, Free), *Describe(C, Held)));
			}
			TestTrue(FString::Printf(TEXT("%s: (%d,%d,%d) the same spawn points and instances with the budget as without"), Stops[StopIndex].Label,
				C.X, C.Y, C.Z), Free == Held);
			TestEqual(FString::Printf(TEXT("%s: (%d,%d,%d) each spawn point drawn once"), Stops[StopIndex].Label, C.X, C.Y, C.Z),
				Held.Instances, Held.Spawns.Num());
		}
		AddInfo(FString::Printf(TEXT("%s: %d spawn points over the grid"), Stops[StopIndex].Label, Points));
		if (StopIndex == 2 || StopIndex == 5)
		{
			TestTrue(FString::Printf(TEXT("%s: the grid is scattered"), Stops[StopIndex].Label), Points > 0);
		}
		if (StopIndex == 0 || StopIndex == 3)
		{
			TestEqual(FString::Printf(TEXT("%s: nothing scattered"), Stops[StopIndex].Label), Points, 0);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
