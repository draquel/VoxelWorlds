// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Testing/VoxelScatterTestDefinitions.h"
#include "Engine/StaticMesh.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterTypes.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelData.h"
#include "Containers/Ticker.h"
#include "PhysicsEngine/BodyInstance.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G42 (WORLD_MODES_REVIEW_PLAN.md §14.1): scatter's HISM writes. A UE 5.8 HISM rebuilds its whole
// cluster tree and recreates its scene proxy (every instance registered again with the distance-field and ray-tracing
// scenes) for each change that is not in place, so the renderer holds each HISM's writes and applies them at most once
// per voxel.Scatter.HISMBatchSeconds, releases zero-scaled where each instance stands; and voxel.Scatter.RegionChunks
// sizes the regions HISMs are kept per.
//
// Through the scatter manager's own pipeline on a tilted heightmap plane of grass and dirt (the test definitions given
// an engine mesh, as the reload tests):
// - SameInstances: the same scenario with every write made at once (HISMBatchSeconds 0, the behaviour before G42) and
//   batched: four chunks near the viewer, the viewer far (the trees released), a chunk unloaded, reloaded near, and a
//   cleared radius. At every stop the instances drawn (scale not zero, every HISM of every type) are the same transforms
//   in both, each type's as many as its spawn points.
// - InPlaceRelease: batched, the trees the distance cleanup releases keep their location with a zero scale, and one tick
//   applies one batch per HISM however many chunks released.
// - Regions: with regions of one chunk (a HISM per chunk and type) the instances drawn are the transforms of one region
//   per type, to 0.01 uu.
// - WriteBudget (G281): with a write budget so small that each tick writes one batch (voxel.Scatter.HISMWriteBudgetMs) and
//   regions of one chunk (twelve HISMs), the scenario pumped by ticks alone (no flush: the batches left wait for later
//   ticks) draws at every stop the instances written at once, a chunk unloaded and a radius cleared while their writes
//   wait included; so does the budget 0 (every due batch in its tick, before G281); of four HISMs due in one tick (the
//   trees released) the tick writes the nearest the viewer, the others drawing their trees until later ticks.
// ---------------------------------------------------------------------------

namespace VoxelScatterHISMBatchTestUtils
{
	using namespace VoxelScatterTesting;

	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr int32 TreeID = 2; // VoxelScatterTesting::SmoothDefinitions' trees (SpawnDistance 20,000)
	const int32 TypeIDs[] = { 0, 1, TreeID };

	FVector ChunkCenter(const FIntVector& Coord) { return (FVector(Coord) + FVector(0.5)) * ChunkWorldSize; }

	/** A chunk of the plane with the heightmap generator's SDF: grass, with every third column dirt (rocks grow there). */
	TArray<FVoxelData> MakeChunk(const FIntVector& Coord)
	{
		// Every third column of the lattice is dirt, the rest grass.
		return MakeHeightmapChunk(FVector(Coord) * ChunkWorldSize, ChunkSize, VoxelSize,
			[](double X, double Y) { return TiltedPlane().GetTerrainHeightAt(X, Y, FVoxelNoiseParams()); },
			[&Coord](int32 X, int32 Y) { return static_cast<uint8>(((Coord.X * ChunkSize + X) + (Coord.Y * ChunkSize + Y)) % 3 == 0 ? EVoxelMaterial::Dirt : EVoxelMaterial::Grass); });
	}

	/** Sets console variables for the test's scope and restores them after. */
	struct FScopedCVars
	{
		TArray<TPair<IConsoleVariable*, FString>> Saved;
		void Set(const TCHAR* Name, const TCHAR* Value)
		{
			if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				Saved.Emplace(Var, Var->GetString());
				Var->Set(Value, ECVF_SetByConsole);
			}
		}
		~FScopedCVars()
		{
			for (int32 i = Saved.Num() - 1; i >= 0; --i)
			{
				Saved[i].Key->Set(*Saved[i].Value, ECVF_SetByConsole);
			}
		}
	};

	/** One drawn instance as a key: type, world position to 0.01 uu, rotation and scale to 1e-4. */
	FString InstanceKey(int32 TypeID, const FTransform& Xf)
	{
		const FVector P = Xf.GetLocation();
		const FQuat Q = Xf.GetRotation().GetNormalized();
		const FVector S = Xf.GetScale3D();
		// The quaternion's sign is arbitrary; key it with W >= 0
		const double Sign = Q.W < 0.0 ? -1.0 : 1.0;
		return FString::Printf(TEXT("%d %lld %lld %lld %d %d %d %d %d %d %d"), TypeID,
			FMath::RoundToInt64(P.X * 100.0), FMath::RoundToInt64(P.Y * 100.0), FMath::RoundToInt64(P.Z * 100.0),
			FMath::RoundToInt32(Q.X * Sign * 10000.0), FMath::RoundToInt32(Q.Y * Sign * 10000.0), FMath::RoundToInt32(Q.Z * Sign * 10000.0),
			FMath::RoundToInt32(Q.W * Sign * 10000.0), FMath::RoundToInt32(S.X * 10000.0), FMath::RoundToInt32(S.Y * 10000.0),
			FMath::RoundToInt32(S.Z * 10000.0));
	}

	/** Private game world + initialized scatter manager for a smooth world, torn down on destruction. */
	struct FHarness : VoxelScatterTesting::FScatterTestWorld
	{
		FVector Viewer = FVector::ZeroVector;

		explicit FHarness(const TCHAR* WorldName, bool bTreeCollision = false)
			: FScatterTestWorld(WorldName)
		{
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = 100000.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			StartManager(/*bSmoothDefinitions=*/true);
			Manager->SetSurfacePointSpacing(VoxelSize);
			UseWorldMode(TiltedPlaneMode());
			for (const int32 ID : TypeIDs)
			{
				if (const FScatterDefinition* Def = Manager->GetScatterDefinition(ID))
				{
					FScatterDefinition Copy = *Def;
					Copy.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
					Copy.Density = ID == TreeID ? 0.3f : Copy.Density;
					Copy.bEnableCollision = ID == TreeID ? bTreeCollision : Copy.bEnableCollision;
					Manager->AddScatterDefinition(Copy);
				}
			}
		}

		UVoxelScatterRenderer* Renderer() const { return const_cast<UVoxelScatterRenderer*>(Manager->GetScatterRenderer()); }

		void Update() { Manager->Update(Viewer, 0.05f); }

		void Submit(const FIntVector& Coord) { Manager->HandOffChunkVoxels(Coord, MakeChunk(Coord), ChunkSize, VoxelSize); }

		/**
		 * Move the viewer, pump until the extractions are done and the distance spawn and the renderer have run, then land
		 * held writes: at once (FlushHISMWrites), or with bFlush false by more ticks until none is held (each tick within the
		 * write budget, as in play).
		 */
		void Settle(const FVector& InViewer, bool bFlush = true)
		{
			Viewer = InViewer;
			const double Start = FPlatformTime::Seconds();
			while (FPlatformTime::Seconds() < Start + 15.0)
			{
				Update();
				if (Manager->GetPendingGenerationCount() == 0 && FPlatformTime::Seconds() >= Start + 1.0)
				{
					break;
				}
				FPlatformProcess::Sleep(0.005f);
			}
			for (int32 i = 0; i < 12; ++i)
			{
				Update();
				FPlatformProcess::Sleep(0.005f);
			}
			UVoxelScatterRenderer* R = Renderer();
			if (R && bFlush)
			{
				R->FlushHISMWrites();
			}
			const double TicksStart = FPlatformTime::Seconds();
			while (R && !bFlush && (R->GetPendingHISMWriteCount() > 0 || R->GetPendingInstanceAddCount() > 0)
				&& FPlatformTime::Seconds() < TicksStart + 30.0)
			{
				Update();
				FPlatformProcess::Sleep(0.005f);
			}
		}

		/** Every instance drawn (scale not zero) in every HISM of the test's types, as sorted keys; also counts per type. */
		TArray<FString> Drawn(TMap<int32, int32>* OutPerType = nullptr) const
		{
			TArray<FString> Keys;
			const UVoxelScatterRenderer* R = Manager->GetScatterRenderer();
			if (!R)
			{
				return Keys;
			}
			for (const int32 ID : TypeIDs)
			{
				TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> HISMs;
				R->GetTypeHISMs(ID, HISMs);
				for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : HISMs)
				{
					for (int32 i = 0; i < Pair.Value->GetInstanceCount(); ++i)
					{
						FTransform Xf;
						if (Pair.Value->GetInstanceTransform(i, Xf, /*bWorldSpace=*/true) && !Xf.GetScale3D().IsNearlyZero())
						{
							Keys.Add(InstanceKey(ID, Xf));
							if (OutPerType)
							{
								++OutPerType->FindOrAdd(ID);
							}
						}
					}
				}
			}
			Keys.Sort();
			return Keys;
		}

		/** Every instance drawn (scale not zero) in every HISM of the test's types: type and world transform. */
		TArray<TPair<int32, FTransform>> DrawnTransforms() const
		{
			TArray<TPair<int32, FTransform>> Out;
			const UVoxelScatterRenderer* R = Manager->GetScatterRenderer();
			for (const int32 ID : TypeIDs)
			{
				TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> HISMs;
				if (R)
				{
					R->GetTypeHISMs(ID, HISMs);
				}
				for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : HISMs)
				{
					for (int32 i = 0; i < Pair.Value->GetInstanceCount(); ++i)
					{
						FTransform Xf;
						if (Pair.Value->GetInstanceTransform(i, Xf, /*bWorldSpace=*/true) && !Xf.GetScale3D().IsNearlyZero())
						{
							Out.Emplace(ID, Xf);
						}
					}
				}
			}
			return Out;
		}

		/** The cached spawn points of every chunk, counted per type. */
		TMap<int32, int32> SpawnsPerType(const TArray<FIntVector>& Coords) const
		{
			TMap<int32, int32> Out;
			for (const FIntVector& Coord : Coords)
			{
				if (const FChunkScatterData* Scatter = Manager->GetChunkScatterData(Coord))
				{
					for (const FScatterSpawnPoint& Point : Scatter->SpawnPoints)
					{
						++Out.FindOrAdd(Point.ScatterTypeID);
					}
				}
			}
			return Out;
		}
	};

	/**
	 * Instances of A with no instance of B of the same type within Tolerance (position) and 1e-3 (rotation, scale), each B
	 * instance matched once.
	 */
	int32 CountUnmatched(const TArray<TPair<int32, FTransform>>& A, const TArray<TPair<int32, FTransform>>& B, double Tolerance)
	{
		TMap<FIntVector, TArray<int32>> Cells;
		auto CellOf = [](const FVector& P) { return FIntVector(FMath::FloorToInt32(P.X), FMath::FloorToInt32(P.Y), FMath::FloorToInt32(P.Z)); };
		for (int32 i = 0; i < B.Num(); ++i)
		{
			Cells.FindOrAdd(CellOf(B[i].Value.GetLocation())).Add(i);
		}
		TArray<bool> Used;
		Used.Init(false, B.Num());
		int32 Unmatched = 0;
		for (const TPair<int32, FTransform>& Inst : A)
		{
			const FVector P = Inst.Value.GetLocation();
			const FIntVector C = CellOf(P);
			bool bFound = false;
			for (int32 dx = -1; dx <= 1 && !bFound; ++dx)
			{
				for (int32 dy = -1; dy <= 1 && !bFound; ++dy)
				{
					for (int32 dz = -1; dz <= 1 && !bFound; ++dz)
					{
						const TArray<int32>* Candidates = Cells.Find(C + FIntVector(dx, dy, dz));
						for (int32 j = 0; Candidates && j < Candidates->Num() && !bFound; ++j)
						{
							const int32 k = (*Candidates)[j];
							const FTransform& O = B[k].Value;
							if (!Used[k] && B[k].Key == Inst.Key && O.GetLocation().Equals(P, Tolerance)
								&& O.GetRotation().Equals(Inst.Value.GetRotation(), 1e-3) && O.GetScale3D().Equals(Inst.Value.GetScale3D(), 1e-3))
							{
								Used[k] = true;
								bFound = true;
							}
						}
					}
				}
			}
			Unmatched += bFound ? 0 : 1;
		}
		return Unmatched;
	}

	/** The SameInstances scenario: the drawn instances at each stop, and whether they matched the spawn points per type. */
	TArray<TArray<FString>> RunScenario(FAutomationTestBase& Test, const TCHAR* Label, TArray<FString>& OutNotes, int64* OutCompactions = nullptr,
		bool bFlush = true, int64* OutLeft = nullptr)
	{
		const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
		const FIntVector Target(1, 0, 0);
		const FVector Near = ChunkCenter(FIntVector(1, 1, 0));
		const FVector Far = Near + FVector(40000.0, 0.0, 0.0);
		TArray<TArray<FString>> Stops;
		FHarness Harness(TEXT("VoxelScatterHISMBatchTestWorld"));
		auto Stop = [&](const TCHAR* Name)
		{
			TMap<int32, int32> PerType;
			Stops.Add(Harness.Drawn(&PerType));
			const TMap<int32, int32> Spawns = Harness.SpawnsPerType(Coords);
			for (const int32 ID : TypeIDs)
			{
				Test.TestEqual(FString::Printf(TEXT("%s, %s: type %d draws one instance per spawn point"), Label, Name, ID),
					PerType.FindRef(ID), Spawns.FindRef(ID));
			}
			OutNotes.Add(FString::Printf(TEXT("%s, %s: %d instances drawn (trees %d) of %d in %d HISMs, %d batches, %d compactions, %d writes held"),
				Label, Name, Stops.Last().Num(), PerType.FindRef(TreeID), Harness.Renderer()->GetTotalInstanceCount(), Harness.Renderer()->GetHISMCount(),
				static_cast<int32>(Harness.Renderer()->GetTotalHISMWriteBatches()), static_cast<int32>(Harness.Renderer()->GetTotalHISMCompactions()),
				Harness.Renderer()->GetPendingHISMWriteCount()));
			if (OutCompactions)
			{
				*OutCompactions = Harness.Renderer()->GetTotalHISMCompactions();
			}
			if (OutLeft)
			{
				*OutLeft = Harness.Renderer()->GetTotalHISMBatchesLeft();
			}
		};
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
		}
		Harness.Settle(Near, bFlush);
		Stop(TEXT("near"));
		Harness.Settle(Far, bFlush);
		Stop(TEXT("far (trees released)"));
		Harness.Manager->OnChunkUnloaded(Target);
		Harness.Settle(Far, bFlush);
		Stop(TEXT("a chunk unloaded"));
		Harness.Submit(Target);
		Harness.Settle(Near, bFlush);
		Stop(TEXT("reloaded near"));
		const FVector ClearAt(ChunkCenter(Target).X, ChunkCenter(Target).Y,
			TiltedPlane().GetTerrainHeightAt(ChunkCenter(Target).X, ChunkCenter(Target).Y, FVoxelNoiseParams()));
		Harness.Manager->ClearScatterInRadius(ClearAt, 300.0f);
		Harness.Settle(Near, bFlush);
		Stop(TEXT("a cleared radius"));
		// A chunk unloaded and another cleared while their writes wait for the budget: the reload's placements still held
		Harness.Submit(Target);
		for (int32 i = 0; i < 40; ++i)
		{
			Harness.Update();
			FPlatformProcess::Sleep(0.005f);
		}
		Harness.Manager->OnChunkUnloaded(Target);
		Harness.Manager->ClearScatterInRadius(ClearAt + FVector(-ChunkWorldSize, 0.0, 0.0), 300.0f);
		Harness.Settle(Near, bFlush);
		Stop(TEXT("unloaded and cleared while held"));
		return Stops;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterHISMBatchSameInstancesTest,
	"VoxelWorlds.Scatter.HISMBatch.SameInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterHISMBatchSameInstancesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHISMBatchTestUtils;

	TArray<FString> Notes;
	TArray<TArray<FString>> Immediate;
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0"));
		Immediate = RunScenario(*this, TEXT("written at once"), Notes);
	}
	TArray<TArray<FString>> Batched;
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0.1"));
		Batched = RunScenario(*this, TEXT("batched"), Notes);
	}
	// Batched, every HISM taking a batch with any pooled instance outnumbering its live ones rewritten without them
	TArray<TArray<FString>> Compacted;
	int64 Compactions = 0;
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0.1"));
		CVars.Set(TEXT("voxel.Scatter.HISMCompactMinPooled"), TEXT("1"));
		Compacted = RunScenario(*this, TEXT("compacted"), Notes, &Compactions);
	}
	for (const FString& Note : Notes)
	{
		AddInfo(Note);
	}
	TestTrue(TEXT("Scenario: instances are drawn near"), Immediate.Num() > 0 && Immediate[0].Num() > 0);
	TestTrue(TEXT("Scenario: the compacted run rewrote HISMs"), Compactions > 0);
	auto Compare = [&](const TCHAR* Label, const TArray<TArray<FString>>& Run)
	{
		TestEqual(FString::Printf(TEXT("%s: stops as often as the run written at once"), Label), Run.Num(), Immediate.Num());
		for (int32 i = 0; i < FMath::Min(Run.Num(), Immediate.Num()); ++i)
		{
			int32 Differ = 0;
			for (int32 j = 0; j < FMath::Max(Run[i].Num(), Immediate[i].Num()); ++j)
			{
				Differ += (!Run[i].IsValidIndex(j) || !Immediate[i].IsValidIndex(j) || Run[i][j] != Immediate[i][j]) ? 1 : 0;
			}
			TestEqual(FString::Printf(TEXT("%s, stop %d: draws the instances written at once (%d against %d, %d keys differ)"), Label, i,
				Run[i].Num(), Immediate[i].Num(), Differ), Differ, 0);
		}
	};
	Compare(TEXT("Batched"), Batched);
	Compare(TEXT("Compacted"), Compacted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterHISMBatchInPlaceReleaseTest,
	"VoxelWorlds.Scatter.HISMBatch.InPlaceRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterHISMBatchInPlaceReleaseTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHISMBatchTestUtils;

	FScopedCVars CVars;
	CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0.1"));
	CVars.Set(TEXT("voxel.Scatter.HISMCompactMinPooled"), TEXT("0")); // the released trees stay in their HISM
	const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
	const FVector Near = ChunkCenter(FIntVector(1, 1, 0));
	const FVector Far = Near + FVector(40000.0, 0.0, 0.0);

	FHarness Harness(TEXT("VoxelScatterHISMBatchTestWorld"));
	for (const FIntVector& Coord : Coords)
	{
		Harness.Submit(Coord);
	}
	Harness.Settle(Near);

	// The trees as drawn: their HISM's index -> world transform
	TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> TreeHISMs;
	Harness.Renderer()->GetTypeHISMs(TreeID, TreeHISMs);
	TestEqual(TEXT("Scenario: one tree HISM (a heightmap world keeps one region)"), TreeHISMs.Num(), 1);
	if (TreeHISMs.Num() != 1)
	{
		return false;
	}
	UHierarchicalInstancedStaticMeshComponent* Trees = TreeHISMs[0].Value;
	TMap<int32, FVector> Drawn;
	for (int32 i = 0; i < Trees->GetInstanceCount(); ++i)
	{
		FTransform Xf;
		if (Trees->GetInstanceTransform(i, Xf, /*bWorldSpace=*/false) && !Xf.GetScale3D().IsNearlyZero())
		{
			Drawn.Add(i, Xf.GetLocation());
		}
	}
	TestTrue(TEXT("Scenario: trees are drawn near"), Drawn.Num() > 0);

	// Far: one distance pass releases the trees of all four chunks; the tick that takes them applies one batch per HISM.
	Harness.Viewer = Far;
	const int64 BatchesBefore = Harness.Renderer()->GetTotalHISMWriteBatches();
	Harness.Manager->Update(Far, 0.3f); // past the distance interval: the cleanup runs, then the renderer ticks
	const int64 Batches = Harness.Renderer()->GetTotalHISMWriteBatches() - BatchesBefore;
	AddInfo(FString::Printf(TEXT("released %d trees of 4 chunks; %lld batches in that tick, %d writes held after it"), Drawn.Num(), Batches,
		Harness.Renderer()->GetPendingHISMWriteCount()));
	TestTrue(TEXT("The release tick applies at most one batch per HISM of the three types"), Batches <= 3);
	Harness.Renderer()->FlushHISMWrites();

	int32 InPlace = 0;
	int32 Moved = 0;
	int32 StillDrawn = 0;
	for (const TPair<int32, FVector>& Pair : Drawn)
	{
		FTransform Xf;
		Trees->GetInstanceTransform(Pair.Key, Xf, /*bWorldSpace=*/false);
		StillDrawn += Xf.GetScale3D().IsNearlyZero() ? 0 : 1;
		(Xf.GetLocation().Equals(Pair.Value, 0.0) ? InPlace : Moved) += 1;
	}
	AddInfo(FString::Printf(TEXT("after the release: %d in place, %d moved, %d still drawn"), InPlace, Moved, StillDrawn));
	TestEqual(TEXT("No released tree is drawn"), StillDrawn, 0);
	TestEqual(TEXT("Every released tree keeps its location (an in-place change, no tree rebuild)"), InPlace, Drawn.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterHISMBatchRegionsTest,
	"VoxelWorlds.Scatter.HISMBatch.Regions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterHISMBatchRegionsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHISMBatchTestUtils;

	const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
	const FVector Near = ChunkCenter(FIntVector(1, 1, 0));
	auto Run = [&](const TCHAR* Regions, int32& OutHISMs) -> TArray<TPair<int32, FTransform>>
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.RegionChunks"), Regions);
		FHarness Harness(TEXT("VoxelScatterHISMBatchTestWorld"));
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
		}
		Harness.Settle(Near);
		OutHISMs = Harness.Renderer()->GetHISMCount();
		return Harness.DrawnTransforms();
	};
	int32 OneRegionHISMs = 0;
	int32 SplitHISMs = 0;
	// The default (-1: regions of 4 chunks in every world mode since WM-MP5) puts the four chunks in one region.
	const TArray<TPair<int32, FTransform>> OneRegion = Run(TEXT("-1"), OneRegionHISMs);
	const TArray<TPair<int32, FTransform>> Split = Run(TEXT("1"), SplitHISMs);
	AddInfo(FString::Printf(TEXT("one region (the default's four chunks): %d HISMs, %d instances; regions of one chunk: %d HISMs, %d instances"), OneRegionHISMs,
		OneRegion.Num(), SplitHISMs, Split.Num()));
	TestEqual(TEXT("One region: a HISM per type"), OneRegionHISMs, 3);
	TestEqual(TEXT("Regions of one chunk: a HISM per chunk and type"), SplitHISMs, 12);
	TestTrue(TEXT("Scenario: instances are drawn"), OneRegion.Num() > 0);
	TestEqual(TEXT("As many instances drawn"), Split.Num(), OneRegion.Num());
	TestEqual(TEXT("Every instance of one region drawn by the split regions, to 0.01 uu"), CountUnmatched(OneRegion, Split, 0.01), 0);
	TestEqual(TEXT("and every instance of the split regions by one region"), CountUnmatched(Split, OneRegion, 0.01), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterHISMBatchWriteBudgetTest,
	"VoxelWorlds.Scatter.HISMBatch.WriteBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterHISMBatchWriteBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHISMBatchTestUtils;

	TArray<FString> Notes;
	// Regions of one chunk: twelve HISMs, so a tick has several batches due and the budget has something to order.
	TArray<TArray<FString>> Immediate;
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.RegionChunks"), TEXT("1"));
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0"));
		Immediate = RunScenario(*this, TEXT("written at once"), Notes);
	}
	TArray<TArray<FString>> Unbudgeted;
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.RegionChunks"), TEXT("1"));
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0.1"));
		CVars.Set(TEXT("voxel.Scatter.HISMWriteBudgetMs"), TEXT("0"));
		Unbudgeted = RunScenario(*this, TEXT("budget 0, by ticks"), Notes, nullptr, /*bFlush=*/false);
	}
	TArray<TArray<FString>> Budgeted;
	int64 Left = 0;
	int64 Compactions = 0;
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.RegionChunks"), TEXT("1"));
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0.1"));
		CVars.Set(TEXT("voxel.Scatter.HISMWriteBudgetMs"), TEXT("0.0001"));
		CVars.Set(TEXT("voxel.Scatter.HISMCompactMinPooled"), TEXT("1"));
		Budgeted = RunScenario(*this, TEXT("one batch a tick, by ticks"), Notes, &Compactions, /*bFlush=*/false, &Left);
	}
	for (const FString& Note : Notes)
	{
		AddInfo(Note);
	}
	TestTrue(TEXT("Scenario: instances are drawn near"), Immediate.Num() > 0 && Immediate[0].Num() > 0);
	TestTrue(TEXT("Scenario: the budget left batches for later ticks"), Left > 0);
	TestTrue(TEXT("Scenario: the budgeted run rewrote HISMs"), Compactions > 0);
	auto Compare = [&](const TCHAR* Label, const TArray<TArray<FString>>& Run)
	{
		TestEqual(FString::Printf(TEXT("%s: stops as often as the run written at once"), Label), Run.Num(), Immediate.Num());
		for (int32 i = 0; i < FMath::Min(Run.Num(), Immediate.Num()); ++i)
		{
			int32 Differ = 0;
			for (int32 j = 0; j < FMath::Max(Run[i].Num(), Immediate[i].Num()); ++j)
			{
				Differ += (!Run[i].IsValidIndex(j) || !Immediate[i].IsValidIndex(j) || Run[i][j] != Immediate[i][j]) ? 1 : 0;
			}
			TestEqual(FString::Printf(TEXT("%s, stop %d: draws the instances written at once (%d against %d, %d keys differ)"), Label, i,
				Run[i].Num(), Immediate[i].Num(), Differ), Differ, 0);
		}
	};
	Compare(TEXT("Budget 0"), Unbudgeted);
	Compare(TEXT("One batch a tick"), Budgeted);

	// Tick by tick: the four chunks' trees released in one tick (the viewer gone far east) are four HISMs due at once; a
	// tick writes one, the nearest the viewer (an east chunk's), and the others draw their trees until their tick comes.
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.RegionChunks"), TEXT("1"));
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0.1"));
		CVars.Set(TEXT("voxel.Scatter.HISMCompactMinPooled"), TEXT("0"));
		CVars.Set(TEXT("voxel.Scatter.HISMWriteBudgetMs"), TEXT("0"));
		const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
		const FVector Near = ChunkCenter(FIntVector(1, 1, 0));
		const FVector Far = Near + FVector(40000.0, 0.0, 0.0);
		FHarness Harness(TEXT("VoxelScatterHISMBatchTestWorld"));
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
		}
		Harness.Settle(Near);
		UVoxelScatterRenderer* R = Harness.Renderer();
		TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> TreeHISMs;
		R->GetTypeHISMs(TreeID, TreeHISMs);
		auto DrawsTrees = [](const UHierarchicalInstancedStaticMeshComponent* HISM)
		{
			for (int32 i = 0; i < HISM->GetInstanceCount(); ++i)
			{
				FTransform Xf;
				if (HISM->GetInstanceTransform(i, Xf, /*bWorldSpace=*/false) && !Xf.GetScale3D().IsNearlyZero())
				{
					return true;
				}
			}
			return false;
		};
		TSet<FIntVector> HadTrees;
		for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : TreeHISMs)
		{
			if (DrawsTrees(Pair.Value))
			{
				HadTrees.Add(Pair.Key);
			}
		}
		const int32 WithTrees = HadTrees.Num();
		TestTrue(TEXT("Tick by tick: trees in more than one region"), WithTrees >= 2);

		CVars.Set(TEXT("voxel.Scatter.HISMWriteBudgetMs"), TEXT("0.0001"));
		FPlatformProcess::Sleep(0.25f); // past the batch interval since the HISMs' last writes
		Harness.Viewer = Far;
		const int64 BatchesBefore = R->GetTotalHISMWriteBatches();
		Harness.Manager->Update(Far, 0.3f); // past the distance interval: the cleanup releases every chunk's trees, then the renderer ticks
		const int32 InTick = static_cast<int32>(R->GetTotalHISMWriteBatches() - BatchesBefore);
		TArray<FIntVector> Written;
		int32 StillDrawn = 0;
		for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : TreeHISMs)
		{
			if (!HadTrees.Contains(Pair.Key))
			{
				continue;
			}
			if (DrawsTrees(Pair.Value))
			{
				++StillDrawn;
			}
			else
			{
				Written.Add(Pair.Key);
			}
		}
		AddInfo(FString::Printf(TEXT("tick by tick: %d tree HISMs with trees before; the release tick wrote %d batch(es), %d tree HISM(s) cleared, %d still drawn; %lld batches left so far"),
			WithTrees, InTick, Written.Num(), StillDrawn, R->GetTotalHISMBatchesLeft()));
		TestEqual(TEXT("Tick by tick: the release tick writes one batch"), InTick, 1);
		TestEqual(TEXT("Tick by tick: one tree HISM cleared in it"), Written.Num(), 1);
		const int32 EastX = R->GetRenderRegions().GetRegion(FIntVector(1, 0, 0)).X;
		bool bEastHadTrees = false;
		for (const FIntVector& Region : HadTrees)
		{
			bEastHadTrees |= Region.X == EastX;
		}
		TestTrue(TEXT("Scenario: an east region had trees"), bEastHadTrees);
		TestTrue(TEXT("Tick by tick: the nearest the viewer (an east chunk's region)"), Written.Num() == 1 && Written[0].X == EastX);
		TestTrue(TEXT("Tick by tick: the others still draw their trees, held"), StillDrawn == WithTrees - 1 && R->GetPendingHISMWriteCount() > 0);
		Harness.Settle(Far, /*bFlush=*/false);
		int32 AfterAll = 0;
		for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : TreeHISMs)
		{
			AfterAll += DrawsTrees(Pair.Value) ? 1 : 0;
		}
		TestEqual(TEXT("Tick by tick: later ticks release the rest"), AfterAll, 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// G292: one HISM's write. A UE 5.8 HISM's own UpdateInstanceTransform starts a tree build at the batch's first placement
// (its set-up copies every instance on the game thread) and each later change of the batch throws that build away, which
// starts another when it lands; a compaction cleared and added every live instance. With voxel.Scatter.HISMWritePath 1:
// - OneTreeBuild: a batch of placements and new instances starts one tree build, not thrown away (the HISM building and
//   no change made during the build); with 0 a batch of several placements throws its first build away. Releases (zero
//   scale where each instance stands) start none on either path.
// - CompactInPlace: three of four chunks unloaded (the kept chunk's trees at the HISM's highest indices), one cleared and
//   another reloaded while the writes wait, then the compacting batch: the tree HISM holds exactly its live instances,
//   none zero-scaled, the same transforms the path before G292 draws, each chunk's count its spawn points, one tree build
//   started and not thrown away; with the trees' collision on, a body per live instance at its transform.
// - EstimateBudget: with the write estimate set far over the budget, a tick writes its first batch and leaves every other
//   batch that would start a build (they are written first in later ticks), while batches of releases alone are written
//   in the same tick; the instances drawn at the end are the ones written at once.
// ---------------------------------------------------------------------------

namespace VoxelScatterHISMBatchTestUtils
{
	/** Every HISM of the test's types. */
	TArray<UHierarchicalInstancedStaticMeshComponent*> AllHISMs(const UVoxelScatterRenderer* R)
	{
		TArray<UHierarchicalInstancedStaticMeshComponent*> Out;
		for (const int32 ID : TypeIDs)
		{
			TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> HISMs;
			R->GetTypeHISMs(ID, HISMs);
			for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : HISMs)
			{
				Out.Add(Pair.Value);
			}
		}
		return Out;
	}

	/**
	 * Let the HISMs' tree builds land (20 s at most): a build's result is applied on the game thread by the core ticker
	 * (ExecuteOnGameThread), which nothing ticks while a test runs, so the ticker is ticked here.
	 */
	bool WaitForTreeBuilds(const UVoxelScatterRenderer* R)
	{
		const double Start = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() < Start + 20.0)
		{
			FTSTicker::GetCoreTicker().Tick(0.0f);
			bool bBuilding = false;
			for (const UHierarchicalInstancedStaticMeshComponent* HISM : AllHISMs(R))
			{
				bBuilding |= HISM->IsAsyncBuilding();
			}
			if (!bBuilding)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterHISMBatchOneTreeBuildTest,
	"VoxelWorlds.Scatter.HISMBatch.OneTreeBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterHISMBatchOneTreeBuildTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHISMBatchTestUtils;

	const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
	const FVector Near = ChunkCenter(FIntVector(1, 1, 0));
	const FVector Far = Near + FVector(40000.0, 0.0, 0.0);
	// Per path: the HISMs that started a build at the release flush, and at the placement flush those building and those
	// whose build was thrown away (a change made while it ran)
	struct FResult
	{
		int32 BuildingAfterRelease = 0;
		int32 Building = 0;
		int32 ThrownAway = 0;
		int64 BuildsCounted = 0;
		TArray<FString> Drawn;
	};
	auto Run = [&](const TCHAR* Path) -> FResult
	{
		FResult Result;
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.HISMWritePath"), Path);
		// Batched, but no tick writes a batch (the interval and the longest hold far away): only the flushes below do, so
		// each HISM takes each stop's writes as one batch
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("1000"));
		CVars.Set(TEXT("voxel.Scatter.HISMMaxHoldSeconds"), TEXT("1000"));
		CVars.Set(TEXT("voxel.Scatter.HISMCompactMinPooled"), TEXT("0"));
		FHarness Harness(TEXT("VoxelScatterHISMBatchTestWorld"));
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
		}
		Harness.Settle(Near);
		UVoxelScatterRenderer* R = Harness.Renderer();
		TestTrue(FString::Printf(TEXT("Path %s: the first builds land"), Path), WaitForTreeBuilds(R));
		Harness.Settle(Far); // the trees released: in place
		for (const UHierarchicalInstancedStaticMeshComponent* HISM : AllHISMs(R))
		{
			Result.BuildingAfterRelease += HISM->IsAsyncBuilding() ? 1 : 0;
		}
		TestTrue(FString::Printf(TEXT("Path %s: the release's builds land"), Path), WaitForTreeBuilds(R));
		const int64 BuildsBefore = R->GetTotalHISMTreeBuilds();
		Harness.Settle(Near); // the trees placed again: recycled indices, each a placement
		for (const UHierarchicalInstancedStaticMeshComponent* HISM : AllHISMs(R))
		{
			Result.Building += HISM->IsAsyncBuilding() ? 1 : 0;
			Result.ThrownAway += HISM->IsAsyncBuilding() && HISM->bConcurrentChanges ? 1 : 0;
		}
		Result.BuildsCounted = R->GetTotalHISMTreeBuilds() - BuildsBefore;
		WaitForTreeBuilds(R);
		Result.Drawn = Harness.Drawn();
		return Result;
	};
	const FResult Before = Run(TEXT("0"));
	const FResult After = Run(TEXT("1"));
	AddInfo(FString::Printf(TEXT("before G292: %d HISM(s) building after the release, %d after the placements, %d of them thrown away; G292: %d, %d, %d (%lld builds counted); %d and %d instances drawn"),
		Before.BuildingAfterRelease, Before.Building, Before.ThrownAway, After.BuildingAfterRelease, After.Building, After.ThrownAway,
		After.BuildsCounted, Before.Drawn.Num(), After.Drawn.Num()));
	TestEqual(TEXT("A release (in place) starts no build"), After.BuildingAfterRelease, 0);
	TestTrue(TEXT("Scenario: the placements started a build"), After.Building > 0);
	TestTrue(TEXT("Scenario: before G292 a batch of placements throws its first build away"), Before.ThrownAway > 0);
	TestEqual(TEXT("G292: no build thrown away"), After.ThrownAway, 0);
	TestEqual(TEXT("G292: the builds counted are the HISMs building"), static_cast<int32>(After.BuildsCounted), After.Building);
	TestTrue(TEXT("Scenario: instances drawn"), After.Drawn.Num() > 0);
	TestTrue(TEXT("The same instances drawn on both paths"), After.Drawn == Before.Drawn);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterHISMBatchCompactInPlaceTest,
	"VoxelWorlds.Scatter.HISMBatch.CompactInPlace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterHISMBatchCompactInPlaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHISMBatchTestUtils;

	const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
	const FIntVector Kept(1, 1, 0);
	const FIntVector Cleared(1, 0, 0);
	const FVector Near = ChunkCenter(Kept);
	struct FResult
	{
		TArray<FString> Drawn;
		int32 TreeInstances = 0;
		int32 TreeZeroScaled = 0;
		int32 LiveTrees = 0;
		int64 Compactions = 0;
		int32 ThrownAway = 0;
		int64 Builds = 0;
		bool bPhysics = false;
		int32 Bodies = 0;
		int32 BodiesMisplaced = 0;
		TMap<FIntVector, int32> ChunkCounts;
		TMap<FIntVector, int32> ChunkSpawns;
	};
	auto Run = [&](const TCHAR* Path) -> FResult
	{
		FResult Result;
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.HISMWritePath"), Path);
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("1000"));
		CVars.Set(TEXT("voxel.Scatter.HISMMaxHoldSeconds"), TEXT("1000"));
		CVars.Set(TEXT("voxel.Scatter.HISMCompactMinPooled"), TEXT("1"));
		FHarness Harness(TEXT("VoxelScatterHISMBatchTestWorld"), /*bTreeCollision=*/true);
		// One at a time, so the chunks' instances take the HISMs' indices in this order: the kept chunk's last
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
			Harness.Settle(Near);
		}
		UVoxelScatterRenderer* R = Harness.Renderer();
		WaitForTreeBuilds(R);
		const int64 CompactionsBefore = R->GetTotalHISMCompactions();
		const int64 BuildsBefore = R->GetTotalHISMTreeBuilds();
		// While the writes wait (no tick writes them): two chunks unloaded, the third's ground cleared round its centre, the
		// first unloaded and reloaded (its placements held); then one flush, whose batches compact the HISMs
		Harness.Manager->OnChunkUnloaded(FIntVector(0, 0, 0));
		Harness.Manager->OnChunkUnloaded(FIntVector(0, 1, 0));
		const FVector ClearAt(ChunkCenter(Cleared).X, ChunkCenter(Cleared).Y,
			TiltedPlane().GetTerrainHeightAt(ChunkCenter(Cleared).X, ChunkCenter(Cleared).Y, FVoxelNoiseParams()));
		Harness.Manager->ClearScatterInRadius(ClearAt, 600.0f);
		Harness.Manager->OnChunkUnloaded(Cleared);
		Harness.Submit(FIntVector(0, 0, 0));
		Harness.Settle(Near);
		Result.Compactions = R->GetTotalHISMCompactions() - CompactionsBefore;
		Result.Builds = R->GetTotalHISMTreeBuilds() - BuildsBefore;
		for (const UHierarchicalInstancedStaticMeshComponent* HISM : AllHISMs(R))
		{
			Result.ThrownAway += HISM->IsAsyncBuilding() && HISM->bConcurrentChanges ? 1 : 0;
		}
		WaitForTreeBuilds(R);
		const TArray<FIntVector> Live = { FIntVector(0, 0, 0), Kept };
		for (const FIntVector& Coord : Live)
		{
			Result.ChunkCounts.Add(Coord, R->GetChunkInstanceCount(Coord, TreeID));
			Result.ChunkSpawns.Add(Coord, Harness.SpawnsPerType({ Coord }).FindRef(TreeID));
			Result.LiveTrees += Result.ChunkSpawns[Coord];
		}
		TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> TreeHISMs;
		R->GetTypeHISMs(TreeID, TreeHISMs);
		for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : TreeHISMs)
		{
			UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;
			Result.TreeInstances += HISM->GetInstanceCount();
			Result.bPhysics |= HISM->IsPhysicsStateCreated();
			const TArray<FBodyInstance*>& Bodies = HISM->GetInstanceBodies();
			for (int32 i = 0; i < HISM->GetInstanceCount(); ++i)
			{
				FTransform Xf;
				HISM->GetInstanceTransform(i, Xf, /*bWorldSpace=*/true);
				if (Xf.GetScale3D().IsNearlyZero())
				{
					++Result.TreeZeroScaled;
					continue;
				}
				if (Bodies.IsValidIndex(i) && Bodies[i] && Bodies[i]->IsValidBodyInstance())
				{
					++Result.Bodies;
					Result.BodiesMisplaced += Bodies[i]->GetUnrealWorldTransform().GetLocation().Equals(Xf.GetLocation(), 0.1) ? 0 : 1;
				}
			}
		}
		Result.Drawn = Harness.Drawn();
		return Result;
	};
	const FResult Before = Run(TEXT("0"));
	const FResult After = Run(TEXT("1"));
	for (const FResult* Res : { &Before, &After })
	{
		AddInfo(FString::Printf(TEXT("%s: %lld compaction(s), %lld build(s) counted, %d thrown away; trees: %d instances in the HISM (%d zero-scaled), %d live, %d bodies (%d misplaced, physics %d); %d instances drawn"),
			Res == &Before ? TEXT("before G292") : TEXT("G292"), Res->Compactions, Res->Builds, Res->ThrownAway, Res->TreeInstances, Res->TreeZeroScaled,
			Res->LiveTrees, Res->Bodies, Res->BodiesMisplaced, Res->bPhysics ? 1 : 0, Res->Drawn.Num()));
	}
	TestTrue(TEXT("Scenario: the batch compacted HISMs"), After.Compactions > 0 && Before.Compactions > 0);
	TestTrue(TEXT("Scenario: trees live"), After.LiveTrees > 0);
	TestEqual(TEXT("G292: the tree HISM holds its live instances only"), After.TreeInstances, After.LiveTrees);
	TestEqual(TEXT("G292: none zero-scaled"), After.TreeZeroScaled, 0);
	TestEqual(TEXT("G292: no build thrown away"), After.ThrownAway, 0);
	TestTrue(TEXT("The same instances drawn on both paths"), After.Drawn == Before.Drawn);
	for (const TPair<FIntVector, int32>& Pair : After.ChunkSpawns)
	{
		TestEqual(FString::Printf(TEXT("G292: chunk (%d,%d,%d) holds a tree per spawn point"), Pair.Key.X, Pair.Key.Y, Pair.Key.Z),
			After.ChunkCounts.FindRef(Pair.Key), Pair.Value);
	}
	if (After.bPhysics)
	{
		TestEqual(TEXT("G292: a body per live tree"), After.Bodies, After.LiveTrees);
		TestEqual(TEXT("G292: each body at its tree"), After.BodiesMisplaced, 0);
		TestEqual(TEXT("As many bodies as before G292"), After.Bodies, Before.Bodies);
	}
	else
	{
		AddInfo(TEXT("The test world created no physics state: the bodies are not checked"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterHISMBatchEstimateBudgetTest,
	"VoxelWorlds.Scatter.HISMBatch.EstimateBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterHISMBatchEstimateBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHISMBatchTestUtils;

	TArray<FString> Notes;
	TArray<TArray<FString>> Immediate;
	{
		FScopedCVars CVars;
		CVars.Set(TEXT("voxel.Scatter.RegionChunks"), TEXT("1"));
		CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0"));
		Immediate = RunScenario(*this, TEXT("written at once"), Notes);
	}

	FScopedCVars CVars;
	CVars.Set(TEXT("voxel.Scatter.RegionChunks"), TEXT("1"));
	CVars.Set(TEXT("voxel.Scatter.HISMWritePath"), TEXT("1"));
	CVars.Set(TEXT("voxel.Scatter.HISMBatchSeconds"), TEXT("0.1"));
	CVars.Set(TEXT("voxel.Scatter.HISMCompactMinPooled"), TEXT("0"));
	CVars.Set(TEXT("voxel.Scatter.HISMWriteBudgetMs"), TEXT("1000"));
	const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
	const FVector Near = ChunkCenter(FIntVector(1, 1, 0));
	const FVector Far = Near + FVector(40000.0, 0.0, 0.0);
	FHarness Harness(TEXT("VoxelScatterHISMBatchTestWorld"));
	UVoxelScatterRenderer* R = Harness.Renderer();
	// A write that starts a build estimated at a second an instance: over the 1,000 ms budget whatever is first
	R->SetEstimatedWriteNsPerInstance(1e9);
	for (const FIntVector& Coord : Coords)
	{
		Harness.Submit(Coord);
	}
	// Tick by tick until nothing is held: per tick the batches written and left
	TArray<int32> WrittenPerTick;
	TArray<int32> LeftPerTick;
	auto Pump = [&](const FVector& Viewer)
	{
		Harness.Viewer = Viewer;
		const double Start = FPlatformTime::Seconds();
		int32 Quiet = 0;
		while (FPlatformTime::Seconds() < Start + 30.0 && Quiet < 40)
		{
			const int64 Batches = R->GetTotalHISMWriteBatches();
			const int64 Left = R->GetTotalHISMBatchesLeft();
			Harness.Update();
			WrittenPerTick.Add(static_cast<int32>(R->GetTotalHISMWriteBatches() - Batches));
			LeftPerTick.Add(static_cast<int32>(R->GetTotalHISMBatchesLeft() - Left));
			const bool bBusy = R->GetPendingHISMWriteCount() > 0 || R->GetPendingInstanceAddCount() > 0 || Harness.Manager->GetPendingGenerationCount() > 0;
			Quiet = bBusy ? 0 : Quiet + 1;
			FPlatformProcess::Sleep(0.005f);
		}
	};
	Pump(Near);
	int32 MostWrittenNear = 0;
	int32 LeftNear = 0;
	for (int32 i = 0; i < WrittenPerTick.Num(); ++i)
	{
		MostWrittenNear = FMath::Max(MostWrittenNear, WrittenPerTick[i]);
		LeftNear += LeftPerTick[i];
	}
	const TArray<FString> DrawnNear = Harness.Drawn();
	// Far: one distance pass releases every chunk's trees, batches of releases alone (no build), each tree HISM's due at
	// once (their builds landed, the interval past): the tick writes them all, whatever the estimate
	WaitForTreeBuilds(R);
	FPlatformProcess::Sleep(0.25f);
	Harness.Viewer = Far;
	const int64 BatchesBefore = R->GetTotalHISMWriteBatches();
	const int64 LeftBefore = R->GetTotalHISMBatchesLeft();
	Harness.Manager->Update(Far, 0.3f); // past the distance interval: the cleanup runs, then the renderer ticks
	const int32 WrittenFar = static_cast<int32>(R->GetTotalHISMWriteBatches() - BatchesBefore);
	const int32 LeftFar = static_cast<int32>(R->GetTotalHISMBatchesLeft() - LeftBefore);
	WrittenPerTick.Reset();
	LeftPerTick.Reset();
	Pump(Far);
	const TArray<FString> DrawnFar = Harness.Drawn();
	AddInfo(FString::Printf(TEXT("near: at most %d batch(es) a tick, %d left; the release tick far: %d written, %d left; %d and %d instances drawn"),
		MostWrittenNear, LeftNear, WrittenFar, LeftFar, DrawnNear.Num(), DrawnFar.Num()));
	for (const FString& Note : Notes)
	{
		AddInfo(Note);
	}
	TestEqual(TEXT("Near: a tick writes one batch that builds (the first), the others wait"), MostWrittenNear, 1);
	TestTrue(TEXT("Near: batches were left for later ticks"), LeftNear > 0);
	TestTrue(TEXT("Far: the releases' batches (no build) are written in one tick"), WrittenFar > 1 && LeftFar == 0);
	TestTrue(TEXT("Scenario: the written-at-once run stopped near and far"), Immediate.Num() >= 2);
	if (Immediate.Num() >= 2)
	{
		TestTrue(TEXT("Near: the instances written at once"), DrawnNear == Immediate[0]);
		TestTrue(TEXT("Far: the instances written at once"), DrawnFar == Immediate[1]);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
