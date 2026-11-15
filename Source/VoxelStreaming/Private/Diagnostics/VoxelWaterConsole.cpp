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
#include "UObject/UObjectIterator.h"

// Console: voxel.Water.Stats [x y | pawn]
// The world's water levels (the water plan's W2: the bodies, and with a position the sample there), then
// world modes review G58 / G59: what edit-driven water propagation has done (UVoxelWaterPropagation::GetStats: edit scans,
// the chunk-arrival face scans and their seeds, floods, flooded voxels), and with a position the water tile's column there:
// the chunk layers whose voxels hold water in it (edit-merged for a chunk with edits since G59), before the tile's dilation.
static FAutoConsoleCommandWithWorldAndArgs GVoxelWaterStatsCmd(
	TEXT("voxel.Water.Stats"),
	TEXT("Log the world's water bodies (the water levels: each body's id, kind and level) and edit-driven water propagation's ")
	TEXT("counters (world modes review G58 / G59); with 'x y' or 'pawn', also the water levels' sample at that position and the ")
	TEXT("water tile's column there (the chunk layers that mark it wet)."),
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
			// The bodies of the world's water: the sea's record, the lakes and river reaches added to the world, and how
			// many sources may hand out more; then the sample at the position (the point gather).
			const FVoxelWaterLevels Levels = CM->GetWaterLevels();
			auto KindName = [](EVoxelWaterBodyKind Kind)
			{
				return Kind == EVoxelWaterBodyKind::Sea ? TEXT("sea") : Kind == EVoxelWaterBodyKind::Lake ? TEXT("lake") : TEXT("reach");
			};
			const TCHAR* LevelTerms = Levels.IsRadial() ? TEXT("from the planet centre") : TEXT("world Z");
			TArray<FVoxelWaterBody> Bodies(Levels.GetBodies());
			Bodies.Append(CM->GetStaticWaterBodies());
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: %d water bod%s (levels: %s)"),
				*TargetWorld->GetName(), Bodies.Num(), Bodies.Num() == 1 ? TEXT("y") : TEXT("ies"), LevelTerms);
			for (const FVoxelWaterBody& Body : Bodies)
			{
				const bool bEverywhere = Body.Bounds == FVoxelWaterLevels::WholeWorld();
				FString Footprint;
				if (Body.Kind == EVoxelWaterBodyKind::Lake && Body.Lake.IsValid())
				{
					Footprint = FString::Printf(TEXT(", %d cells of %.0f uu"), Body.Lake->NumCells(), Body.Lake->GetCellSize());
				}
				else if (Body.Kind == EVoxelWaterBodyKind::Reach && Body.Reach.IsValid())
				{
					Footprint = FString::Printf(TEXT(", %d segments down to level %.1f, footprint to %.0f uu, %.0f uu/s"),
						Body.Reach->NumSegments(), Body.Reach->GetMinLevel(), Body.Reach->GetReach(), Body.Reach->GetFlowSpeed());
				}
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]:   body %u: %s, level %.1f%s, %s"),
					*TargetWorld->GetName(), Body.Id, KindName(Body.Kind), Body.Level, *Footprint,
					bEverywhere ? TEXT("over the whole world") : *FString::Printf(TEXT("bounds %s"), *Body.Bounds.ToString()));
			}
			const UVoxelChunkManager::FWaterGatherStats G = CM->GetWaterGatherStats();
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: %s; the point gather: %lld samples, %lld gathered a place (%d places kept); %lld gathers by bounds; %lld generation requests carried bodies (water columns); %lld chunks' columns read by the pull or the flood"),
				*TargetWorld->GetName(), CM->HasWaterBodies() ? TEXT("lakes or river reaches may lie in the world (a body or a source is registered)") : TEXT("the sea alone (no lake, no river reach, no source)"),
				G.PointSamples, G.PointGathers, G.PointSnapshots, G.BoxGathers, G.ColumnRequests, G.ChunkReaders);
			// The lakes and river reaches drawn, in every play world (a dedicated server's and each client's): one mesh a
			// body within the far field's distance of the viewer, none on a dedicated server or without a water material.
			for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
			{
				const UWorld* PlayWorld = It->GetWorld();
				if (PlayWorld && PlayWorld->IsGameWorld())
				{
					const TCHAR* Net = PlayWorld->GetNetMode() == NM_DedicatedServer ? TEXT("Server") : PlayWorld->GetNetMode() == NM_ListenServer ? TEXT("Listen")
						: PlayWorld->GetNetMode() == NM_Client ? TEXT("Client") : TEXT("Standalone");
					UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: net=%s: %d lake(s) and river reach(es) added to the world, %d drawn (%d quads)"),
						*PlayWorld->GetName(), Net, It->GetStaticWaterBodies().Num(), It->GetWaterBodyMeshes().Num(), It->GetWaterBodyMeshes().GetNumQuads());
				}
			}
			if (bColumn)
			{
				FVoxelWaterSample Sample;
				if (CM->SampleWater(Column, Sample))
				{
					UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: at (%.0f, %.0f, %.0f): body %u (%s), surface level %.1f, depth %.1f (positive under the surface)"),
						*TargetWorld->GetName(), Column.X, Column.Y, Column.Z, Sample.BodyId, KindName(Sample.Kind), Sample.SurfaceLevel, Sample.Depth);
				}
				else
				{
					UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: at (%.0f, %.0f, %.0f): no water over this position"),
						*TargetWorld->GetName(), Column.X, Column.Y, Column.Z);
				}
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
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: no water propagation (the world has no water)"), *TargetWorld->GetName());
			}
			const UVoxelChunkManager::FWaterRepullStats R = CM->GetWaterRepullStats();
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: the pull's second passes (G70): %lld queued, %lld run, %lld flooded (%lld voxels), %lld skipped, %d pending"),
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
