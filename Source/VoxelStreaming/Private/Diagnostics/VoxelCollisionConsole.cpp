// Copyright Daniel Raquel. All Rights Reserved.

// The collision cooks' console audit (voxel.Collision.AuditNeighbors).

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelCollisionManager.h"
#include "UObject/UObjectIterator.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

static FAutoConsoleCommandWithWorldAndArgs GVoxelCollisionAuditNeighborsCmd(
	TEXT("voxel.Collision.AuditNeighbors"),
	TEXT("G32: log the collision cooks that read a neighbour as air (it had no voxel data then): how many cooked chunks, how many "
	     "were cooked with a neighbour missing, how many of those have the neighbour's data now with no re-cook pending (stale), "
	     "how many re-cooks are pending, and the neighbour re-cooks so far. Optional arg: max stale chunks to list (default 8)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
	{
		const int32 MaxList = Args.Num() > 0 ? FMath::Max(0, FCString::Atoi(*Args[0])) : 8;
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			UVoxelCollisionManager* Collision = It->GetCollisionManager();
			if (!It->GetWorld() || !It->GetWorld()->IsGameWorld() || !Collision || !Collision->IsInitialized())
			{
				continue;
			}
			int32 Cooked = 0, WithMissing = 0, Stale = 0, Awaiting = 0;
			TArray<FIntVector> StaleChunks;
			Collision->AuditNeighborCooks(Cooked, WithMissing, Stale, Awaiting, &StaleChunks);
			FString List;
			for (int32 Index = 0; Index < FMath::Min(MaxList, StaleChunks.Num()); ++Index)
			{
				List += FString::Printf(TEXT(" (%d,%d,%d)"), StaleChunks[Index].X, StaleChunks[Index].Y, StaleChunks[Index].Z);
			}
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.AuditNeighbors: %d cooked, %d cooked with a neighbour missing, %d stale (the neighbour has data now, no re-cook pending), %d re-cooks pending, %lld neighbour re-cooks so far, arrivals needing none: %lld air and %lld solid on both sides%s%s"),
				Cooked, WithMissing, Stale, Awaiting, Collision->GetTotalNeighborRecooks(), Collision->GetTotalNeighborArrivalsSkipped(),
				Collision->GetTotalNeighborArrivalsSkippedSolid(),
				List.IsEmpty() ? TEXT("") : TEXT("; stale:"), *List);
		}
	}));

static FAutoConsoleCommandWithWorldAndArgs GVoxelCollisionCookStatsCmd(
	TEXT("voxel.Collision.CookStats"),
	TEXT("G275, G283: log what the collision cooks cost on their workers since the last reset, a mean per cook and per stage (the wait for "
	     "far-compressed snapshots, the voxel copy, the neighbour slices, the mesh at the collision LOD, the Chaos triangle mesh); the "
	     "cooks by kind (a chunk's first, a neighbour's re-cook, an edit's), the chunks found to have no surface (skipped at the launch or "
	     "on the worker), the neighbour wait's holds (D196), the audit's counts "
	     "(voxel.Collision.CookAudit: predictions against meshes, re-cooks that built the same triangles), and per distance band from the "
	     "nearest focus (in chunk widths) the chunks whose first collision arrived and when (mean and last, seconds after the reset); then "
	     "reset. `voxel.Collision.CookStats keep` logs without the reset."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
	{
		const bool bKeep = Args.Num() > 0 && Args[0] == TEXT("keep");
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			UVoxelCollisionManager* Collision = It->GetCollisionManager();
			if (!It->GetWorld() || !It->GetWorld()->IsGameWorld() || !Collision || !Collision->IsInitialized())
			{
				continue;
			}
			const FVoxelCollisionCookStats& Stats = Collision->GetCookStats();
			const double PerCook = Stats.Cooks > 0 ? 1.0 / static_cast<double>(Stats.Cooks) : 0.0;
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.CookStats: %lld cooks (%lld empty), %.0f triangles a cook; a cook %.2f ms (the longest %.1f): wait %.2f, copy %.2f, slices %.2f, mesh %.2f, triangle mesh %.2f; %.1f thread-seconds in all"),
				Stats.Cooks, Stats.EmptyCooks, Stats.Triangles * PerCook, Stats.TotalMs * PerCook, Stats.MaxTotalMs, Stats.WaitMs * PerCook,
				Stats.CopyMs * PerCook, Stats.SlicesMs * PerCook, Stats.MeshMs * PerCook, Stats.TriMeshMs * PerCook, Stats.TotalMs / 1000.0);
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.CookStats kinds: first %lld (%lld empty), neighbour re-cooks %lld, other re-cooks %lld; no surface: %lld skipped at the launch, %lld predicted on the worker (%lld not meshed); neighbour wait %lld holds of %lld chunks"),
				Stats.FirstCooks, Stats.FirstEmpty, Stats.NeighborRecooks, Stats.OtherRecooks, Stats.SkippedAtLaunch, Stats.PredictedEmpty,
				Stats.SkippedOnWorker, Stats.NeighborWaitHolds, Stats.NeighborWaitChunks);
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.CookStats audit: predicted empty but meshed triangles %lld, empty but not predicted %lld; re-cooks that built the same triangles: neighbour %lld of %lld, other %lld of %lld"),
				Stats.PredictedEmptyWrong, Stats.EmptyNotPredicted, Stats.NeighborRecooksSame, Stats.NeighborRecooks, Stats.OtherRecooksSame,
				Stats.OtherRecooks);
			static const TCHAR* BandNames[FVoxelCollisionCookStats::NumBands] = { TEXT("0-1"), TEXT("1-2"), TEXT("2-3"), TEXT("3-5"), TEXT("5+") };
			FString Bands;
			for (int32 Band = 0; Band < FVoxelCollisionCookStats::NumBands; ++Band)
			{
				const int64 Count = Stats.BandFirstReady[Band];
				Bands += FString::Printf(TEXT(" %s: %lld at %.2f s (last %.2f);"), BandNames[Band], Count,
					Count > 0 ? Stats.BandFirstReadySumSeconds[Band] / static_cast<double>(Count) : 0.0, Stats.BandFirstReadyLastSeconds[Band]);
			}
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.CookStats first collision by chunk widths from the nearest focus:%s"), *Bands);
			// G296's measure: for the cooks that found a surface, where the chunk's drawn mesh and its neighbours' stood at the launch.
			static const TCHAR* KindNames[2] = { TEXT("first"), TEXT("re-cooks") };
			for (int32 K = 0; K < 2; ++K)
			{
				const int64 N = Stats.ReachCooks[K];
				const int64 Drawn = Stats.ReachOwn[K][static_cast<int32>(EVoxelCollisionOwnMesh::DrawnAtLOD)];
				UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.CookStats reach %s: %lld with a surface; own mesh drawn at the cook's LOD %lld (aged %.2f s, the oldest %.1f), coming %lld, other LOD %lld, none %lld; with every neighbour that has voxels drawn there %lld, to be drawn there %lld; queued %.2f s (the longest %.2f)"),
					KindNames[K], N, Drawn, Drawn > 0 ? Stats.ReachOwnAgeSumSeconds[K] / static_cast<double>(Drawn) : 0.0, Stats.ReachOwnAgeMaxSeconds[K],
					Stats.ReachOwn[K][static_cast<int32>(EVoxelCollisionOwnMesh::ComingAtLOD)], Stats.ReachOwn[K][static_cast<int32>(EVoxelCollisionOwnMesh::OtherLOD)],
					Stats.ReachOwn[K][static_cast<int32>(EVoxelCollisionOwnMesh::None)], Stats.ReachAllDrawn[K], Stats.ReachAllTarget[K],
					N > 0 ? Stats.QueueWaitSumSeconds[K] / static_cast<double>(N) : 0.0, Stats.QueueWaitMaxSeconds[K]);
			}
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.CookStats reach: %lld cooks launched before their mesh at the cook's LOD, which came %.2f s after on average"),
				Stats.MeshAfterCook, Stats.MeshAfterCook > 0 ? Stats.MeshAfterCookSumSeconds / static_cast<double>(Stats.MeshAfterCook) : 0.0);
			if (!bKeep)
			{
				Collision->ResetCookStats();
			}
		}
	}));
