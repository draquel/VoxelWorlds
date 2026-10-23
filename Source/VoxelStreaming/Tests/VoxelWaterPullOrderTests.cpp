// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelCoordinates.h"
#include "VoxelData.h"
#include "VoxelSeaSurface.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G70 (WORLD_MODES_REVIEW_PLAN.md §14.1). A chunk pulls the water flags across its faces from its neighbours once, when
// its mesh lands (UVoxelChunkManager::PropagateWaterFromNeighbors), and a chunk wholly under the sea has no water of its
// own: it gets all of it that way. So water that reaches a chunk only through a neighbour that flooded after it pulled
// never came, and which voxels flooded depended on the order the chunks finished meshing in (a corner of a planet chunk
// flooded in some runs of Streaming.Planet.FarReadsOffGameThread and not in others).
//
// - Streaming.Water.PullOrder.Plane / .Planet: a sea two chunk layers deeper than the seabed's top, streamed from a viewer
//   down at the seabed, so the chunks under the sea mesh from the bottom up, each pulling before the one above it has
//   water. Afterwards the flags must be closed under the flood: no dry air below the sea in a loaded chunk next to a
//   flagged voxel, in its own chunk or across a face. With each neighbour pulling again when a chunk's water rises
//   (voxel.Water.PullAgain 1) there is none; with one pull each (0, the rule before) there are many.
// ---------------------------------------------------------------------------

namespace VoxelWaterPullOrderTestUtils
{
	/** Sets voxel.Water.PullAgain at console priority for a scope. */
	struct FScopedPullAgain
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedPullAgain(int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Water.PullAgain"));
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedPullAgain()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/** What a streamed sea's flags look like against their closure under the flood. */
	struct FPullResult
	{
		int32 Chunks = 0;
		int64 Flagged = 0;
		/** Dry air below the sea reachable from a flagged voxel through air below the sea in loaded chunks. */
		int64 Missed = 0;
		/** Chunks holding missed voxels. */
		int32 ChunksMissed = 0;
		UVoxelChunkManager::FWaterRepullStats Repulls;
	};

	/**
	 * Every loaded chunk's voxels (edit-merged snapshots; no edits here), and the flood's closure of their flags: from each
	 * flagged voxel, its six neighbours (across chunk faces into loaded chunks) that are air below the sea and dry are
	 * missed, and so on from those. The sea test is the pull's own (FVoxelSeaSurface on the voxel's world position).
	 */
	FPullResult Measure(VoxelTesting::FTestChunkWorld& W)
	{
		FPullResult Out;
		const int32 CS = W.Config->ChunkSize;
		const float VS = W.Config->VoxelSize;
		const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*W.Config);
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		TMap<FIntVector, TSharedPtr<const TArray<FVoxelData>>> Voxels;
		for (const FIntVector& Coord : Loaded)
		{
			if (TSharedPtr<const TArray<FVoxelData>> Snapshot = W.Manager->GetEditMergedSnapshot(Coord))
			{
				Voxels.Add(Coord, Snapshot);
			}
		}
		Out.Chunks = Voxels.Num();

		auto Split = [CS](const FIntVector& Global, FIntVector& OutChunk, int32& OutIndex)
		{
			OutChunk = FIntVector(FMath::FloorToInt32(static_cast<float>(Global.X) / CS), FMath::FloorToInt32(static_cast<float>(Global.Y) / CS),
				FMath::FloorToInt32(static_cast<float>(Global.Z) / CS));
			const FIntVector L = Global - OutChunk * CS;
			OutIndex = L.X + L.Y * CS + L.Z * CS * CS;
		};
		auto IsBelowSea = [&](const FIntVector& Global)
		{
			return Sea.IsBelow(W.Config->WorldOrigin + FVector(Global) * VS);
		};
		TSet<FIntVector> Missed;
		TArray<FIntVector> Queue;
		auto Visit = [&](const FIntVector& Global)
		{
			FIntVector Chunk;
			int32 Index;
			Split(Global, Chunk, Index);
			const TSharedPtr<const TArray<FVoxelData>>* Data = Voxels.Find(Chunk);
			if (!Data)
			{
				return;
			}
			const FVoxelData& V = (**Data)[Index];
			if (V.IsSolid() || V.HasWaterFlag() || !IsBelowSea(Global) || Missed.Contains(Global))
			{
				return;
			}
			Missed.Add(Global);
			Queue.Add(Global);
		};
		static const FIntVector Steps[6] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
		for (const TPair<FIntVector, TSharedPtr<const TArray<FVoxelData>>>& Pair : Voxels)
		{
			const TArray<FVoxelData>& Data = *Pair.Value;
			for (int32 Index = 0; Index < Data.Num(); ++Index)
			{
				if (Data[Index].IsSolid() || !Data[Index].HasWaterFlag())
				{
					continue;
				}
				++Out.Flagged;
				const FIntVector Global = Pair.Key * CS + FIntVector(Index % CS, (Index / CS) % CS, Index / (CS * CS));
				for (const FIntVector& Step : Steps)
				{
					Visit(Global + Step);
				}
			}
		}
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			for (const FIntVector& Step : Steps)
			{
				Visit(Queue[Head] + Step);
			}
		}
		Out.Missed = Missed.Num();
		TSet<FIntVector> ChunksMissed;
		for (const FIntVector& Global : Missed)
		{
			FIntVector Chunk;
			int32 Index;
			Split(Global, Chunk, Index);
			ChunksMissed.Add(Chunk);
		}
		Out.ChunksMissed = ChunksMissed.Num();
		Out.Repulls = W.Manager->GetWaterRepullStats();
		return Out;
	}

	/** Stream the world with each setting of voxel.Water.PullAgain and measure; the fix must leave nothing missed. */
	void RunPullOrder(FAutomationTestBase& Test, const TCHAR* Name, TFunctionRef<void(VoxelTesting::FTestChunkWorld&)> Configure,
		TFunctionRef<FVector(VoxelTesting::FTestChunkWorld&)> Viewer)
	{
		FPullResult Results[2];
		for (int32 Mode = 0; Mode < 2; ++Mode)
		{
			FScopedPullAgain Scoped(Mode);
			VoxelTesting::FTestChunkWorld W;
			W.Create(Mode == 0 ? TEXT("WaterPullOrderBefore") : TEXT("WaterPullOrderAfter"));
			Configure(W);
			W.StartManager();
			W.SetViewer(Viewer(W));
			Test.TestTrue(*FString::Printf(TEXT("%s (pull again %d): the pipeline settled"), Name, Mode), W.Settle());
			// The second pulls run about a millisecond a tick: let the queue drain.
			for (int32 Tick = 0; Tick < 600 && W.Manager->GetWaterRepullStats().Pending > 0; ++Tick)
			{
				W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			}
			Results[Mode] = Measure(W);
			W.Stop();
			const FPullResult& R = Results[Mode];
			Test.AddInfo(FString::Printf(TEXT("%s, pull again %d: %d chunks, %lld flagged voxels, %lld dry voxels the flood reaches (in %d chunks); second pulls %lld queued, %lld run, %lld flooded (%lld voxels), %lld skipped, %d pending"),
				Name, Mode, R.Chunks, R.Flagged, R.Missed, R.ChunksMissed, R.Repulls.Queued, R.Repulls.Run, R.Repulls.Flooded,
				R.Repulls.FloodedVoxels, R.Repulls.Skipped, R.Repulls.Pending));
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: the sea floods (non-vacuous)"), Name), Results[1].Flagged > 10000);
		Test.TestEqual(*FString::Printf(TEXT("%s: pulling again, no dry voxel the flood reaches"), Name), Results[1].Missed, static_cast<int64>(0));
		Test.TestEqual(*FString::Printf(TEXT("%s: the queue drained"), Name), Results[1].Repulls.Pending, 0);
		Test.TestTrue(*FString::Printf(TEXT("%s: one pull each (the rule before) leaves the flood short (%lld voxels)"), Name, Results[0].Missed),
			Results[0].Missed > 100);
	}
}

/**
 * G70 on a heightmap world: the small plane (terrain within +-800, chunks of 1,600 uu) under a sea at 2,900, so the chunk
 * layer from 0 to 1,600 is wholly under it and the one below holds the seabed; the viewer 1,200 under the datum.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterPullOrderPlaneTest,
	"VoxelWorlds.Streaming.Water.PullOrder.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterPullOrderPlaneTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterPullOrderTestUtils;
	RunPullOrder(*this, TEXT("Plane"),
		[](VoxelTesting::FTestChunkWorld& W)
		{
			W.ConfigureSmallPlane();
			W.Config->bEnableWaterLevel = true;
			W.Config->WaterLevel = 2900.0f;
		},
		[](VoxelTesting::FTestChunkWorld& W) { return FVector(0.0, 0.0, -1200.0); });
	return true;
}

/**
 * G70 on a planet: the small planet (radius 20,000, terrain within +-1,000) under a sea 2,300 over the datum, the viewer
 * 1,200 under the datum at 45N 20E.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterPullOrderPlanetTest,
	"VoxelWorlds.Streaming.Water.PullOrder.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterPullOrderPlanetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterPullOrderTestUtils;
	RunPullOrder(*this, TEXT("Planet"),
		[](VoxelTesting::FTestChunkWorld& W)
		{
			W.ConfigureSmallPlanet();
			W.Config->bEnableWaterLevel = true;
			W.Config->WaterRadius = W.Config->WorldRadius + 2300.0f;
			W.Config->WaterLevel = 1.0e9f; // the Z plane plays no part on a planet
		},
		[](VoxelTesting::FTestChunkWorld& W)
		{
			const double Lat = FMath::DegreesToRadians(45.0);
			const double Lon = FMath::DegreesToRadians(20.0);
			const FVector Up(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
			return W.Config->WorldOrigin + Up * (W.Config->WorldRadius - 1200.0);
		});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
