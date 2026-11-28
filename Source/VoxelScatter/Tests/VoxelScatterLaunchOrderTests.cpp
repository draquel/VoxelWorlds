// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Testing/VoxelScatterTestDefinitions.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterTypes.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G299: the pending queue's launches go to the chunk nearest a viewer now (voxel.Scatter.LaunchNearest).
//
// A chunk handed to scatter waits in the pending queue, sorted by its distance from the viewer when it was queued, and
// at most MaxAsyncScatterTasks extractions run at once. A machine whose extractions lag (a dedicated server process: four
// workers, the CPU generating its window) worked through the chunks queued around where its player had been while the
// chunks under the walking player waited for minutes; a client on the GPU had their trees, and its pawn stood on a tree
// the server did not have (corrections of 20 to 80 uu, Scripts/qa/server_process.py).
//
// Through the manager's own pipeline on a tilted heightmap plane: a row of chunks handed off with the viewer at the row's
// first chunk (the queue's order: the first chunk nearest), then the viewer at the row's last chunk and one update.
// - With the switch on, the update launches the chunks nearest the viewer now (the row's end); the row's start waits.
// - With it off (the path before G299), the update launches the row's start, where the viewer was, and the end waits:
//   the stale order the server process walked through.
// - Either way, once settled, the row's chunks hold the same spawn points: the order is all that changes.
// ---------------------------------------------------------------------------

namespace VoxelScatterLaunchOrderTestUtils
{
	using namespace VoxelScatterTesting;

	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr int32 RowLength = 8;

	FVector ChunkCenter(const FIntVector& Coord) { return (FVector(Coord) + FVector(0.5)) * ChunkWorldSize; }

	TArray<FVoxelData> MakeChunk(const FIntVector& Coord)
	{
		return MakeHeightmapChunk(FVector(Coord) * ChunkWorldSize, ChunkSize, VoxelSize,
			[](double X, double Y) { return TiltedPlane().GetTerrainHeightAt(X, Y, FVoxelNoiseParams()); },
			[](int32, int32) { return static_cast<uint8>(EVoxelMaterial::Grass); });
	}

	FString SpawnKey(const FScatterSpawnPoint& Point)
	{
		return FString::Printf(TEXT("%d %d %d %d"), Point.ScatterTypeID, FMath::RoundToInt32(Point.Position.X * 10.0),
			FMath::RoundToInt32(Point.Position.Y * 10.0), FMath::RoundToInt32(Point.Position.Z * 10.0));
	}

	/** One row's run: which chunks the first update after the viewer moved launched, and the row's spawn points settled. */
	struct FRun
	{
		TArray<int32> LaunchedFirst; // row indices launched by the update after the move
		TArray<FString> Spawns;      // every row chunk's spawn points once settled, sorted
		int64 OutOfOrder = 0;
		bool bQueuedBehind = false;  // the scenario: more chunks queued than one update launches
	};

	FRun RunRow(int32 LaunchNearest)
	{
		VoxelTesting::FScopedCVar Switch(TEXT("voxel.Scatter.LaunchNearest"), LaunchNearest);
		FRun Out;
		FScatterTestWorld W(TEXT("VoxelScatterLaunchOrderWorld"));
		W.Config->ChunkSize = ChunkSize;
		W.Config->VoxelSize = VoxelSize;
		// Every row chunk within the radius types' reach from either end of the row: all are queued at their hand-off.
		W.Config->ScatterRadius = 2.0f * RowLength * ChunkWorldSize;
		W.Config->MeshingMode = EMeshingMode::DualContouring;
		W.StartManager(/*bSmoothDefinitions=*/true);
		W.Manager->SetSurfacePointSpacing(VoxelSize);
		W.UseWorldMode(TiltedPlaneMode());

		// Handed off with the viewer at the row's start, without an update between: all queued, sorted from there.
		const FVector Start = ChunkCenter(FIntVector(0, 0, 0));
		const FVector End = ChunkCenter(FIntVector(RowLength - 1, 0, 0));
		W.Manager->Update(Start, 0.05f);
		for (int32 I = 0; I < RowLength; ++I)
		{
			W.Manager->HandOffChunkVoxels(FIntVector(I, 0, 0), MakeChunk(FIntVector(I, 0, 0)), ChunkSize, VoxelSize);
		}
		int32 QueuedBefore = 0;
		for (int32 I = 0; I < RowLength; ++I)
		{
			QueuedBefore += W.Manager->IsChunkQueuedForExtraction(FIntVector(I, 0, 0)) ? 1 : 0;
		}

		// The viewer walks to the row's end; one update launches what it launches.
		W.Manager->Update(End, 0.05f);
		for (int32 I = 0; I < RowLength; ++I)
		{
			if (!W.Manager->IsChunkQueuedForExtraction(FIntVector(I, 0, 0)))
			{
				Out.LaunchedFirst.Add(I);
			}
		}
		Out.bQueuedBehind = QueuedBefore == RowLength && Out.LaunchedFirst.Num() > 0 && Out.LaunchedFirst.Num() < RowLength;
		Out.OutOfOrder = W.Manager->GetLaunchedOutOfQueueOrder();

		// Settle at the end and read the row.
		const double Deadline = FPlatformTime::Seconds() + 20.0;
		while (W.Manager->GetPendingGenerationCount() > 0 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->Update(End, 0.05f);
			FPlatformProcess::Sleep(0.005f);
		}
		for (int32 I = 0; I < 4; ++I)
		{
			W.Manager->Update(End, 0.05f);
		}
		for (int32 I = 0; I < RowLength; ++I)
		{
			if (const FChunkScatterData* Scatter = W.Manager->GetChunkScatterData(FIntVector(I, 0, 0)))
			{
				for (const FScatterSpawnPoint& Point : Scatter->SpawnPoints)
				{
					Out.Spawns.Add(FString::Printf(TEXT("%d: "), I) + SpawnKey(Point));
				}
			}
		}
		Out.Spawns.Sort();
		return Out;
	}

	FString Indices(const TArray<int32>& Values)
	{
		TArray<FString> Parts;
		for (const int32 V : Values)
		{
			Parts.Add(FString::FromInt(V));
		}
		return FString::Join(Parts, TEXT(", "));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterLaunchNearestTest, "VoxelWorlds.Scatter.LaunchNearest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterLaunchNearestTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterLaunchOrderTestUtils;

	const FRun Nearest = RunRow(1);
	const FRun Queued = RunRow(0);
	AddInfo(FString::Printf(TEXT("Row of %d chunks, the viewer moved from chunk 0 to chunk %d: with the switch the update launched [%s] (%lld out of the queue's order), without it [%s]; %d and %d spawn points settled"),
		RowLength, RowLength - 1, *Indices(Nearest.LaunchedFirst), Nearest.OutOfOrder, *Indices(Queued.LaunchedFirst), Nearest.Spawns.Num(), Queued.Spawns.Num()));

	TestTrue(TEXT("Scenario: every row chunk was queued and one update launched some but not all (with the switch)"), Nearest.bQueuedBehind);
	TestTrue(TEXT("Scenario: the same without it"), Queued.bQueuedBehind);
	TestTrue(TEXT("Scenario: the row has spawn points"), Nearest.Spawns.Num() > 0);

	// With the switch: the chunks nearest the viewer now, the row's end first.
	bool bEndFirst = Nearest.LaunchedFirst.Num() > 0;
	for (int32 K = 0; K < Nearest.LaunchedFirst.Num(); ++K)
	{
		bEndFirst &= Nearest.LaunchedFirst.Contains(RowLength - 1 - K);
	}
	TestTrue(TEXT("With voxel.Scatter.LaunchNearest 1 the update launches the chunks nearest the viewer now (the row's end)"), bEndFirst);
	TestFalse(TEXT("... and the row's start still waits"), Nearest.LaunchedFirst.Contains(0));
	TestTrue(TEXT("... out of the queue's order, counted"), Nearest.OutOfOrder > 0);

	// Without it: the queue's order, the row's start where the viewer had been.
	bool bStartFirst = Queued.LaunchedFirst.Num() > 0;
	for (int32 K = 0; K < Queued.LaunchedFirst.Num(); ++K)
	{
		bStartFirst &= Queued.LaunchedFirst.Contains(K);
	}
	TestTrue(TEXT("With voxel.Scatter.LaunchNearest 0 (before G299) the update launches the row's start, where the viewer was"), bStartFirst);
	TestEqual(TEXT("... and none out of the queue's order"), Queued.OutOfOrder, static_cast<int64>(0));

	TestTrue(TEXT("Either way the settled row holds the same spawn points"), Nearest.Spawns == Queued.Spawns);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
