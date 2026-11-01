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
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.AuditNeighbors: %d cooked, %d cooked with a neighbour missing, %d stale (the neighbour has data now, no re-cook pending), %d re-cooks pending, %lld neighbour re-cooks so far, arrivals needing none: %lld air and %lld solid on both sides (voxel.Collision.NeighborRecook %d)%s%s"),
				Cooked, WithMissing, Stale, Awaiting, Collision->GetTotalNeighborRecooks(), Collision->GetTotalNeighborArrivalsSkipped(),
				Collision->GetTotalNeighborArrivalsSkippedSolid(),
				IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Collision.NeighborRecook"))
					? IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Collision.NeighborRecook"))->GetInt() : -1,
				List.IsEmpty() ? TEXT("") : TEXT("; stale:"), *List);
		}
	}));
