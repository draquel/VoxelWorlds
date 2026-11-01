// Copyright Daniel Raquel. All Rights Reserved.

// The voxel data's console commands: far compression's residency and decodes, the voxel snapshot cache.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelResidencyStats.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"

// Console: voxel.FarCompression.Stats
// Dump the chunk manager's voxel-data residency breakdown (Resident/Uniform/Compressed/Empty
// chunk counts + resident MB + MB reclaimed by compression). Read with unreal-mcp
// LogsToolset.GetLogEntries (category LogVoxelStreaming) since Claudius get_output_log fails
// while PIE holds the log file open.
static FAutoConsoleCommandWithWorldAndArgs GVoxelFarCompressionStatsCmd(
	TEXT("voxel.FarCompression.Stats"),
	TEXT("Log voxel-data residency + compression memory stats for the running game world."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game)) ? World : nullptr;
		if (!TargetWorld && GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
				{
					TargetWorld = Ctx.World();
					break;
				}
			}
		}
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Stats: no PIE/Game world found (start PIE first)"));
			return;
		}

		int32 Managers = 0;
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM) { continue; }
			++Managers;
			const UVoxelChunkManager::FVoxelMemoryStats M = CM->GetVoxelMemoryStats();
			int64 Encodes = 0, Installed = 0, Stale = 0;
			CM->GetBenchFarCompressionCounts(Encodes, Installed, Stale);
			UE_LOG(LogVoxelStreaming, Warning,
				TEXT("voxel.FarCompression.Stats: Resident=%d Uniform=%d Compressed=%d Empty=%d | ResidentVoxelData=%.1fMB Reclaimed=%.1fMB | encodes=%lld installed=%lld stale=%lld"),
				M.ResidentChunks, M.UniformChunks, M.CompressedChunks, M.EmptyChunks,
				M.VoxelDataBytes / (1024.0 * 1024.0), M.CompressionSavedBytes / (1024.0 * 1024.0),
				Encodes, Installed, Stale);
		}
		if (Managers == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Stats: no UVoxelChunkManager found"));
		}
	}));

// Console: voxel.FarCompression.Decodes [reset]
// Where far-compressed chunks were expanded since the last reset (G65): per reader (the FReadScope tags) and per thread
// kind, the count (uniform / compressed) and the time, game-thread waits for worker decodes, and each running chunk
// manager's snapshot decodes this session.
static FAutoConsoleCommand GVoxelFarCompressionDecodesCmd(
	TEXT("voxel.FarCompression.Decodes"),
	TEXT("Log where far-compressed chunk data was expanded (per reader, game thread or worker, and game-thread waits) since "
	     "the last reset. 'reset' clears the statistics."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase))
		{
			VoxelResidencyStats::Reset();
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Decodes: reset"));
			return;
		}
		if (GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				UWorld* World = Ctx.World();
				if (!World || (Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Game))
				{
					continue;
				}
				for (const UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(World))
				{
					if (CM)
					{
						UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Decodes: snapshot decodes on workers (session): %lld"),
							CM->GetSnapshotDecodeCount());
					}
				}
			}
		}
		TArray<FString> Lines;
		VoxelResidencyStats::Describe().ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Decodes: %s"), *Line);
		}
	}));

// Console: voxel.Snapshot.Stats [reset]
// The seam snapshot cache (world modes review G69): what it holds (by how each entry was made, the reader that made it
// and the last that read it, its age and reads, the chunk's distance and state) and the session's counters.
static FAutoConsoleCommand GVoxelSnapshotStatsCmd(
	TEXT("voxel.Snapshot.Stats"),
	TEXT("Log the voxel snapshot cache's contents and session counters. 'reset' clears the counters."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const bool bReset = Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase);
		if (!GEngine)
		{
			return;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Game))
			{
				continue;
			}
			for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(World))
			{
				if (CM)
				{
					if (bReset)
					{
						CM->ResetSnapshotCacheCounters();
						UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Snapshot.Stats: reset"));
						continue;
					}
					TArray<FString> Lines;
					CM->DescribeSnapshotCache().ParseIntoArrayLines(Lines);
					for (const FString& Line : Lines)
					{
						UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Snapshot.Stats: %s"), *Line);
					}
				}
			}
		}
	}));
