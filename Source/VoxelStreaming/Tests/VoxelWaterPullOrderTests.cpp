// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelCoordinates.h"
#include "VoxelData.h"
#include "VoxelSeaSurface.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "Misc/Crc.h"

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
//   flagged voxel, in its own chunk or across a face. With each neighbour pulling again when a chunk's water rises there
//   is none (with one pull each, before G70, there were many).
// - Streaming.Water.PullOrder.LakePlane / .LakePlanet (the hydrology plan's H2): the same with the sea off and a lake
//   in its place, a footprint 4,800 uu across at the level the sea stood at, so the pull and the flood test a voxel
//   against the level over its own column (the chunk's water columns): the flags are closed under that flood, and no
//   voxel holds the flag over its column's level or outside the footprint.
// - Streaming.Water.PullOrder.StridedScansSame (G294): the plane's sea and the lake's streamed twice, the pull's face scan
//   in the array's order (the default) and along each face's columns as before G294 (voxel.Water.StridedScans 1): every
//   chunk loaded in both holds the same water flags, voxel for voxel, and the same chunks are loaded.
// ---------------------------------------------------------------------------

namespace VoxelWaterPullOrderTestUtils
{
	/** What a streamed sea's flags look like against their closure under the flood. */
	struct FPullResult
	{
		int32 Chunks = 0;
		int64 Flagged = 0;
		/** Dry air below the sea reachable from a flagged voxel through air below the sea in loaded chunks. */
		int64 Missed = 0;
		/** Chunks holding missed voxels. */
		int32 ChunksMissed = 0;
		/** Flagged voxels that are not at or under the water over their own column. */
		int64 Stray = 0;
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
			if (TSharedPtr<const TArray<FVoxelData>> Snapshot = W.Manager->GetVoxelSnapshot(Coord))
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
		// With a lake or a river reach in the world, the level over the voxel's own column (the pull's test there).
		const bool bBodies = W.Manager->HasWaterBodies();
		auto IsBelowSea = [&](const FIntVector& Global)
		{
			if (bBodies)
			{
				FIntVector Chunk;
				int32 Index;
				Split(Global, Chunk, Index);
				return W.Manager->IsVoxelUnderWater(Chunk, Global - Chunk * CS);
			}
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
				Out.Stray += IsBelowSea(Global) ? 0 : 1;
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

	/** Stream the world and measure: the flood must miss nothing. */
	void RunPullOrder(FAutomationTestBase& Test, const TCHAR* Name, TFunctionRef<void(VoxelTesting::FTestChunkWorld&)> Configure,
		TFunctionRef<FVector(VoxelTesting::FTestChunkWorld&)> Viewer, TFunction<void(VoxelTesting::FTestChunkWorld&)> AddBodies = nullptr)
	{
		FPullResult Result;
		{
			VoxelTesting::FTestChunkWorld W;
			W.Create(TEXT("WaterPullOrder"));
			Configure(W);
			W.StartManager();
			if (AddBodies)
			{
				// At the world's start, before the first generation request.
				AddBodies(W);
			}
			W.SetViewer(Viewer(W));
			Test.TestTrue(*FString::Printf(TEXT("%s: the pipeline settled"), Name), W.Settle());
			// The second pulls run about a millisecond a tick: let the queue drain.
			for (int32 Tick = 0; Tick < 600 && W.Manager->GetWaterRepullStats().Pending > 0; ++Tick)
			{
				W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			}
			Result = Measure(W);
			W.Stop();
			const FPullResult& R = Result;
			Test.AddInfo(FString::Printf(TEXT("%s: %d chunks, %lld flagged voxels, %lld dry voxels the flood reaches (in %d chunks); second pulls %lld queued, %lld run, %lld flooded (%lld voxels), %lld skipped, %d pending"),
				Name, R.Chunks, R.Flagged, R.Missed, R.ChunksMissed, R.Repulls.Queued, R.Repulls.Run, R.Repulls.Flooded,
				R.Repulls.FloodedVoxels, R.Repulls.Skipped, R.Repulls.Pending));
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: the sea floods (non-vacuous)"), Name), Result.Flagged > 10000);
		Test.TestEqual(*FString::Printf(TEXT("%s: no dry voxel the flood reaches"), Name), Result.Missed, static_cast<int64>(0));
		Test.TestEqual(*FString::Printf(TEXT("%s: no flagged voxel over the water over its column"), Name), Result.Stray, static_cast<int64>(0));
		Test.TestEqual(*FString::Printf(TEXT("%s: the queue drained"), Name), Result.Repulls.Pending, 0);
	}
}

namespace VoxelWaterPullOrderTestUtils
{
	/** Per loaded chunk, a CRC of its water flags (one bit a voxel) and how many it holds. */
	TMap<FIntVector, TPair<uint32, int32>> FlagDigests(VoxelTesting::FTestChunkWorld& W)
	{
		TMap<FIntVector, TPair<uint32, int32>> Out;
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		for (const FIntVector& Coord : Loaded)
		{
			const TSharedPtr<const TArray<FVoxelData>> Snapshot = W.Manager->GetVoxelSnapshot(Coord);
			if (!Snapshot.IsValid())
			{
				continue;
			}
			TArray<uint8> Bits;
			Bits.SetNumZeroed((Snapshot->Num() + 7) / 8);
			int32 Flagged = 0;
			for (int32 Index = 0; Index < Snapshot->Num(); ++Index)
			{
				if ((*Snapshot)[Index].HasWaterFlag())
				{
					Bits[Index / 8] |= static_cast<uint8>(1u << (Index % 8));
					++Flagged;
				}
			}
			Out.Add(Coord, TPair<uint32, int32>(FCrc::MemCrc32(Bits.GetData(), Bits.Num()), Flagged));
		}
		return Out;
	}

	/** Stream the world with the pull's scans in one order and take its flags. */
	TMap<FIntVector, TPair<uint32, int32>> StreamFlags(FAutomationTestBase& Test, const FString& Name, int32 StridedScans,
		TFunctionRef<void(VoxelTesting::FTestChunkWorld&)> Configure, TFunction<void(VoxelTesting::FTestChunkWorld&)> AddBodies)
	{
		const VoxelTesting::FScopedCVar Scans(TEXT("voxel.Water.StridedScans"), StridedScans);
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("WaterPullScans"));
		Configure(W);
		W.StartManager();
		if (AddBodies)
		{
			AddBodies(W);
		}
		W.SetViewer(FVector(0.0, 0.0, -1200.0));
		Test.TestTrue(*FString::Printf(TEXT("%s, strided scans %d: the pipeline settled"), *Name, StridedScans), W.Settle());
		for (int32 Tick = 0; Tick < 600 && W.Manager->GetWaterRepullStats().Pending > 0; ++Tick)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
		}
		TMap<FIntVector, TPair<uint32, int32>> Flags = FlagDigests(W);
		W.Stop();
		return Flags;
	}

	/** The same flags from both scan orders. */
	void RunScansSame(FAutomationTestBase& Test, const FString& Name, TFunctionRef<void(VoxelTesting::FTestChunkWorld&)> Configure,
		TFunction<void(VoxelTesting::FTestChunkWorld&)> AddBodies = nullptr)
	{
		const TMap<FIntVector, TPair<uint32, int32>> InOrder = StreamFlags(Test, Name, 0, Configure, AddBodies);
		const TMap<FIntVector, TPair<uint32, int32>> Strided = StreamFlags(Test, Name, 1, Configure, AddBodies);
		int32 Common = 0;
		int32 Differ = 0;
		int64 Flagged = 0;
		for (const TPair<FIntVector, TPair<uint32, int32>>& Pair : InOrder)
		{
			if (const TPair<uint32, int32>* Other = Strided.Find(Pair.Key))
			{
				++Common;
				Flagged += Pair.Value.Value;
				if (*Other != Pair.Value)
				{
					++Differ;
					Test.AddError(FString::Printf(TEXT("%s: chunk %s holds %d flags in order, %d strided (CRC %08x, %08x)"), *Name,
						*Pair.Key.ToString(), Pair.Value.Value, Other->Value, Pair.Value.Key, Other->Key));
				}
			}
		}
		Test.AddInfo(FString::Printf(TEXT("%s: %d chunks in order, %d strided, %d in both, %lld flagged voxels in them, %d differ"),
			*Name, InOrder.Num(), Strided.Num(), Common, Flagged, Differ));
		Test.TestEqual(*FString::Printf(TEXT("%s: the same chunks loaded"), *Name), Common, InOrder.Num());
		Test.TestEqual(*FString::Printf(TEXT("%s: the same chunks loaded (strided)"), *Name), Common, Strided.Num());
		Test.TestTrue(*FString::Printf(TEXT("%s: the sea floods (non-vacuous)"), *Name), Flagged > 10000);
		Test.TestEqual(*FString::Printf(TEXT("%s: no chunk's flags differ"), *Name), Differ, 0);
	}
}

/**
 * G294: the pull's face scan in either order floods the same voxels, on the plane under the sea (the PullOrder.Plane
 * world) and under a lake (PullOrder.LakePlane's).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterPullStridedScansSameTest,
	"VoxelWorlds.Streaming.Water.PullOrder.StridedScansSame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterPullStridedScansSameTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterPullOrderTestUtils;
	RunScansSame(*this, TEXT("Sea on a plane"),
		[](VoxelTesting::FTestChunkWorld& W)
		{
			W.ConfigureSmallPlane();
			W.Config->bEnableWaterLevel = true;
			W.Config->WaterLevel = 2900.0f;
		});
	RunScansSame(*this, TEXT("Lake on a plane"),
		[](VoxelTesting::FTestChunkWorld& W)
		{
			W.ConfigureSmallPlane();
			W.Config->bEnableWaterLevel = false;
		},
		[](VoxelTesting::FTestChunkWorld& W)
		{
			const FVoxelWaterFrame Frame = W.Manager->GetWaterLevels().GetFrame();
			W.Manager->AddWaterBody(FVoxelWaterBody::MakeLake(53, 2900.0,
				FVoxelWaterLake::Make(FVector2D(-2400.0, -2400.0), 400.0, FIntPoint(12, 12), TBitArray<>(true, 144)), Frame, 6000.0));
		});
	return true;
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

/**
 * The plane's pull order with a lake in the sea's place (the sea off): a footprint of 12 x 12 cells of 400 uu around the
 * origin at level 2,900, the viewer 1,200 under the datum.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterPullOrderLakePlaneTest,
	"VoxelWorlds.Streaming.Water.PullOrder.LakePlane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterPullOrderLakePlaneTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterPullOrderTestUtils;
	RunPullOrder(*this, TEXT("Lake on a plane"),
		[](VoxelTesting::FTestChunkWorld& W)
		{
			W.ConfigureSmallPlane();
			W.Config->bEnableWaterLevel = false;
		},
		[](VoxelTesting::FTestChunkWorld& W) { return FVector(0.0, 0.0, -1200.0); },
		[](VoxelTesting::FTestChunkWorld& W)
		{
			const FVoxelWaterFrame Frame = W.Manager->GetWaterLevels().GetFrame();
			W.Manager->AddWaterBody(FVoxelWaterBody::MakeLake(51, 2900.0,
				FVoxelWaterLake::Make(FVector2D(-2400.0, -2400.0), 400.0, FIntPoint(12, 12), TBitArray<>(true, 144)), Frame, 6000.0));
		});
	return true;
}

/**
 * The planet's pull order with a lake in the sea's place (the sea off): a footprint of 12 x 12 cells of 400 uu around
 * 45N 20E, 2,300 over the datum, the viewer 1,200 under the datum there.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterPullOrderLakePlanetTest,
	"VoxelWorlds.Streaming.Water.PullOrder.LakePlanet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterPullOrderLakePlanetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterPullOrderTestUtils;
	const double Lat = FMath::DegreesToRadians(45.0);
	const double Lon = FMath::DegreesToRadians(20.0);
	const FVector Up(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
	RunPullOrder(*this, TEXT("Lake on a planet"),
		[](VoxelTesting::FTestChunkWorld& W)
		{
			W.ConfigureSmallPlanet();
			W.Config->bEnableWaterLevel = false;
		},
		[Up](VoxelTesting::FTestChunkWorld& W) { return W.Config->WorldOrigin + Up * (W.Config->WorldRadius - 1200.0); },
		[Up](VoxelTesting::FTestChunkWorld& W)
		{
			const FVoxelWaterFrame Frame = W.Manager->GetWaterLevels().GetFrame();
			W.Manager->AddWaterBody(FVoxelWaterBody::MakeLake(52, Frame.DatumRadius + 2300.0,
				FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, 400.0, FIntPoint(12, 12), TBitArray<>(true, 144)), Frame, 6000.0));
		});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
