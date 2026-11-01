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

		explicit FHarness(const TCHAR* WorldName)
			: FScatterTestWorld(WorldName)
		{
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = 100000.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			StartManager(/*bSmoothDefinitions=*/true);
			Manager->SetSurfacePointSpacing(VoxelSize);
			UseWorldMode(&TiltedPlane());
			for (const int32 ID : TypeIDs)
			{
				if (const FScatterDefinition* Def = Manager->GetScatterDefinition(ID))
				{
					FScatterDefinition Copy = *Def;
					Copy.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
					Copy.Density = ID == TreeID ? 0.3f : Copy.Density;
					Manager->AddScatterDefinition(Copy);
				}
			}
		}

		UVoxelScatterRenderer* Renderer() const { return const_cast<UVoxelScatterRenderer*>(Manager->GetScatterRenderer()); }

		void Update() { Manager->Update(Viewer, 0.05f); }

		void Submit(const FIntVector& Coord) { Manager->OnChunkMeshDataReady(Coord, MakeChunk(Coord), ChunkSize, VoxelSize); }

		/** Move the viewer, pump until the extractions are done and the distance spawn and the renderer have run, then land held writes. */
		void Settle(const FVector& InViewer)
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
			if (UVoxelScatterRenderer* R = Renderer())
			{
				R->FlushHISMWrites();
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
	TArray<TArray<FString>> RunScenario(FAutomationTestBase& Test, const TCHAR* Label, TArray<FString>& OutNotes, int64* OutCompactions = nullptr)
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
		};
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
		}
		Harness.Settle(Near);
		Stop(TEXT("near"));
		Harness.Settle(Far);
		Stop(TEXT("far (trees released)"));
		Harness.Manager->OnChunkUnloaded(Target);
		Harness.Settle(Far);
		Stop(TEXT("a chunk unloaded"));
		Harness.Submit(Target);
		Harness.Settle(Near);
		Stop(TEXT("reloaded near"));
		const FVector ClearAt(ChunkCenter(Target).X, ChunkCenter(Target).Y,
			TiltedPlane().GetTerrainHeightAt(ChunkCenter(Target).X, ChunkCenter(Target).Y, FVoxelNoiseParams()));
		Harness.Manager->ClearScatterInRadius(ClearAt, 300.0f);
		Harness.Settle(Near);
		Stop(TEXT("a cleared radius"));
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

#endif // WITH_DEV_AUTOMATION_TESTS
