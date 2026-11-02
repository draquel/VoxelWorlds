// Copyright Daniel Raquel. All Rights Reserved.

// The water's console commands: edit-driven propagation's counters, the water tiles' audit and resend.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelWaterPropagation.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"

// Console: voxel.Water.Stats [x y | pawn]
// World modes review G58 / G59: what edit-driven water propagation has done (UVoxelWaterPropagation::GetStats: edit scans,
// the chunk-arrival face scans and their seeds, floods, flooded voxels), and with a position the water tile's column there:
// the chunk layers whose voxels hold water in it (edit-merged for a chunk with edits since G59), before the tile's dilation.
static FAutoConsoleCommandWithWorldAndArgs GVoxelWaterStatsCmd(
	TEXT("voxel.Water.Stats"),
	TEXT("Log edit-driven water propagation's counters (world modes review G58 / G59); with 'x y' or 'pawn', also the water ")
	TEXT("tile's column there (the chunk layers that mark it wet)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats: no PIE/Game world found (start PIE first)"));
			return;
		}
		bool bColumn = false;
		FVector Column = FVector::ZeroVector;
		if (Args.Num() >= 1 && Args[0].Equals(TEXT("pawn"), ESearchCase::IgnoreCase))
		{
			const APlayerController* PC = TargetWorld->GetFirstPlayerController();
			if (PC && PC->GetPawn())
			{
				Column = PC->GetPawn()->GetActorLocation();
				bColumn = true;
			}
		}
		else if (Args.Num() >= 2)
		{
			Column = FVector(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), 0.0);
			bColumn = true;
		}
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM)
			{
				continue;
			}
			if (const UVoxelWaterPropagation* Water = CM->GetWaterPropagation())
			{
				const UVoxelWaterPropagation::FStats& S = Water->GetStats();
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: edit scans %lld (%lld deferred), faces scanned on arrival %lld, %lld scan reads (%.1f ms, %.2f ms the slowest frame); seeds %lld (%lld on chunk faces); floods %lld started, %lld finished (%lld at the limit); %lld voxels flooded, %lld refused; pending: %s"),
					*TargetWorld->GetName(), S.ScansQueued, S.ScansDeferred, S.FaceScansQueued, S.ScanReads, S.ScanSeconds * 1000.0,
					S.MaxFrameScanSeconds * 1000.0, S.Seeds, S.FaceSeeds,
					S.FloodsStarted, S.FloodsFinished, S.CapHits, S.FloodedVoxels, S.RejectedEdits,
					Water->HasPendingWork() ? TEXT("yes") : TEXT("no"));
			}
			else
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: no water propagation (bEnableWaterLevel off)"), *TargetWorld->GetName());
			}
			const UVoxelChunkManager::FWaterRepullStats R = CM->GetWaterRepullStats();
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: the pull's second passes (G70, voxel.Water.PullAgain): %lld queued, %lld run, %lld flooded (%lld voxels), %lld skipped, %d pending"),
				*TargetWorld->GetName(), R.Queued, R.Run, R.Flooded, R.FloodedVoxels, R.Skipped, R.Pending);
			if (bColumn)
			{
				TArray<int32> WetLayers;
				const bool bTile = CM->GetWaterTileColumn(Column, WetLayers);
				FString Layers;
				for (const int32 Z : WetLayers)
				{
					Layers += FString::Printf(TEXT("%s%d"), Layers.IsEmpty() ? TEXT("") : TEXT(", "), Z);
				}
				const FString State = !bTile ? FString(TEXT("no tile"))
					: WetLayers.Num() == 0 ? FString(TEXT("dry (its tile has no water in this column)"))
					: FString::Printf(TEXT("wet, from chunk layer(s) Z %s"), *Layers);
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: water tile column at (%.0f, %.0f): %s"),
					*TargetWorld->GetName(), Column.X, Column.Y, *State);
			}
		}
	}));

// Console: voxel.Water.TileAudit [maxList=8] [chunkX chunkY]
// The water tiles against the generated surface (UVoxelChunkManager::AuditWaterTiles; G77).
static FAutoConsoleCommandWithWorldAndArgs GVoxelWaterTileAuditCmd(
	TEXT("voxel.Water.TileAudit"),
	TEXT("Audit the 2D water tiles against the generated surface: voxel columns under the water plane that no tile marks ")
	TEXT("wet, the worst chunk columns with their chunks (residency, edits, settled, LOD, the stored and the rebuilt mask). ")
	TEXT("Args: [maxList=8] [chunkX chunkY: a chunk column to detail]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.TileAudit: no PIE/Game world"));
			return;
		}
		const int32 MaxList = Args.Num() > 0 ? FMath::Max(0, FCString::Atoi(*Args[0])) : 8;
		const FIntPoint Detail = Args.Num() >= 3 ? FIntPoint(FCString::Atoi(*Args[1]), FCString::Atoi(*Args[2])) : FIntPoint(MAX_int32, MAX_int32);
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (CM)
			{
				CM->AuditWaterTiles(MaxList, Detail);
			}
		}
	}));

// Console: voxel.Water.ResendTiles
// A diagnostic (G77): mark every water tile dirty so its mesh is generated and sent to the renderer again.
static FAutoConsoleCommandWithWorldAndArgs GVoxelWaterResendTilesCmd(
	TEXT("voxel.Water.ResendTiles"),
	TEXT("Send every 2D water tile's mesh to the renderer again (every tile marked dirty)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			return;
		}
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (CM)
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.ResendTiles: %d tile(s) marked dirty"), CM->ResendWaterTiles());
			}
		}
	}));
