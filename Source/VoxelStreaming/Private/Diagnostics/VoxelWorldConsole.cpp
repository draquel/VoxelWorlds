// Copyright Daniel Raquel. All Rights Reserved.

// The voxel world's console commands: the world, far-field and far-ocean reports, the residency audit and the
// in-place re-mesh. Each reads a chunk manager through its public API; none holds state of its own.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelTreeInjector.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "UObject/UObjectIterator.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"

static FAutoConsoleCommandWithWorld GVoxelFarFieldStatsCmd(
	TEXT("voxel.World.FarFieldStats"),
	TEXT("Log the far field's state for every live chunk manager (patches, drawn, hidden by the voxel window, builds, memory)."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld*)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			if (It->GetWorld() && It->GetWorld()->IsGameWorld())
			{
				UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.FarFieldStats: %s"), *It->GetFarFieldDebugString());
			}
		}
	}));

static FAutoConsoleCommandWithWorld GVoxelWorldStatsCmd(
	TEXT("voxel.World.Stats"),
	TEXT("Log the voxel world's report for every live chunk manager: the world's bounds and sea (a planet's radius, shell, orbit radius and sea sphere; a heightmap world's mode, bounds, terrain Z and far ocean), the voxel window's reach from the streaming viewer with the far field, and the cut-over and descent prefetch. vox.Planet / vox.World print the same lines and add the pawn's."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld*)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			if (It->GetWorld() && It->GetWorld()->IsGameWorld())
			{
				TArray<FString> Lines;
				It->GetWorldReport(FVoxelWorldReportNotes(), Lines);
				for (const FString& Line : Lines)
				{
					UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.Stats: %s"), *Line);
				}
			}
		}
	}));

static FAutoConsoleCommandWithWorld GVoxelFarOceanStatsCmd(
	TEXT("voxel.World.FarOceanStats"),
	TEXT("Log the far ocean's state for every live chunk manager (open columns, grid quads, rebuilds, extent)."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld*)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			if (It->GetWorld() && It->GetWorld()->IsGameWorld())
			{
				UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.FarOceanStats: %s"), *It->GetFarOceanDebugString());
			}
		}
	}));

// Console: voxel.Mesh.RemeshAll
// Re-mesh every currently-loaded chunk IN PLACE (marks them dirty without changing LOD), so a live
// mesher CVar change (e.g. voxel.Mesh.MCBoundaryMorphWidth) takes effect immediately while the LOD rings /
// seams stay exactly where they are. Handy for A/B-ing meshing changes while detached and flying.
static FAutoConsoleCommandWithWorldAndArgs GVoxelRemeshAllCmd(
	TEXT("voxel.Mesh.RemeshAll"),
	TEXT("Re-mesh all loaded voxel chunks in place (apply a live mesher CVar change without moving LOD)."),
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
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Mesh.RemeshAll: no PIE/Game world found (start PIE first)"));
			return;
		}

		int32 Managers = 0;
		int32 TotalDirtied = 0;
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM) { continue; }
			++Managers;
			TArray<FIntVector> Loaded;
			CM->GetLoadedChunks(Loaded);
			for (const FIntVector& Coord : Loaded)
			{
				CM->MarkChunkDirty(Coord);
			}
			TotalDirtied += Loaded.Num();
		}
		UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Mesh.RemeshAll: re-meshing %d chunk(s) across %d manager(s)"), TotalDirtied, Managers);
	}));

// Console: voxel.Stream.AuditResidency
// One-line QA check for "a distant chunk that should have unloaded is still visible" (R23): chunks
// tracked beyond the unload radius (by state), renderer chunks and seam buckets with no tracked
// owner, and async jobs stuck past voxel.Stream.MeshStuckSeconds. findings=0 after streaming has
// settled = healthy. Logged at Warning on LogVoxelStreaming; read it with unreal-mcp
// LogsToolset.GetLogEntries (Claudius get_output_log fails while PIE holds the log file open).
static FAutoConsoleCommandWithWorldAndArgs GVoxelAuditResidencyCmd(
	TEXT("voxel.Stream.AuditResidency"),
	TEXT("Audit voxel chunk residency: tracked chunks beyond the unload radius by state, renderer chunks and seam buckets ")
	TEXT("with no tracked owner, and stuck async jobs. findings=0 after streaming settles = healthy."),
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
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.AuditResidency: no PIE/Game world found (start PIE first)"));
			return;
		}

		int32 Managers = 0;
		for (const UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM) { continue; }
			++Managers;
			FString Report;
			CM->AuditResidency(Report);
			TArray<FString> Lines;
			Report.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.AuditResidency [%s] %s"), *GetNameSafe(CM->GetOwner()), *Line);
			}
		}
		if (Managers == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.AuditResidency: no UVoxelChunkManager found"));
		}
	}));

// Console: voxel.Trees.List [metres=40]
// A cubic world's standing voxel trees near the player (code review CR4, FW-30: was the game module's vox.Trees, which runs
// this now): each tree's template, base and trunk / leaf blocks left.
static FAutoConsoleCommandWithWorldAndArgs GVoxelTreesListCmd(
	TEXT("voxel.Trees.List"),
	TEXT("List a cubic world's standing voxel trees near the player: voxel.Trees.List [metres=40] (template, base, trunk and leaf blocks)"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = VoxelEditAuditHelpers::FindGameWorld(InWorld);
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		APawn* Player = PC ? PC->GetPawn() : nullptr;
		UVoxelChunkManager* ChunkMgr = World ? UVoxelChunkManager::Find(World) : nullptr;
		if (!Player || !ChunkMgr)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Trees.List: need a player pawn and a chunk manager."));
			return;
		}
		if (!ChunkMgr->HasVoxelTrees())
		{
			UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Trees.List: this world has no voxel trees (cubic meshing, TreeMode VoxelData or Both, templates)."));
			return;
		}
		const float Metres = Args.Num() >= 1 ? FCString::Atof(*Args[0]) : 40.f;
		TArray<FVoxelTreeInstance> Trees;
		ChunkMgr->FindVoxelTreesNear(Player->GetActorLocation(), Metres * 100.f, Trees);
		UE_LOG(LogVoxelStreaming, Display, TEXT("voxel.Trees.List: %d standing voxel tree(s) within %.0f m:"), Trees.Num(), Metres);
		TArray<FVoxelTreeVoxel> Blocks;
		for (int32 i = 0; i < FMath::Min(Trees.Num(), 20); ++i)
		{
			ChunkMgr->GetVoxelTreeBlocks(Trees[i], Blocks);
			const int32 Trunk = Blocks.FilterByPredicate([](const FVoxelTreeVoxel& B) { return B.bTrunk; }).Num();
			const FVector Base = ChunkMgr->GlobalVoxelToWorldCentre(Trees[i].BaseVoxel);
			UE_LOG(LogVoxelStreaming, Display, TEXT("  %d. template %d at %s (base voxel %s), %.0f m: %d trunk, %d leaf blocks"), i + 1,
				Trees[i].TemplateIndex, *Base.ToCompactString(), *Trees[i].BaseVoxel.ToString(), FVector::Dist2D(Base, Player->GetActorLocation()) / 100.f,
				Trunk, Blocks.Num() - Trunk);
		}
	}));

// Console: voxel.Stream.Enabled 0|1
// Pause or resume a game world's chunk streaming (UVoxelChunkManager::SetStreamingEnabled; code review CR4, FW-30: the
// plugin's own switch for what vox.Freeze's streaming flag does).
static FAutoConsoleCommandWithWorldAndArgs GVoxelStreamEnabledCmd(
	TEXT("voxel.Stream.Enabled"),
	TEXT("Pause (0) or resume (1) the game world's chunk streaming: voxel.Stream.Enabled 0|1"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* InWorld)
	{
		UWorld* World = VoxelEditAuditHelpers::FindGameWorld(InWorld);
		UVoxelChunkManager* ChunkMgr = World ? UVoxelChunkManager::Find(World) : nullptr;
		if (!ChunkMgr || Args.Num() < 1)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.Enabled: usage voxel.Stream.Enabled 0|1 (needs a game world with a chunk manager)"));
			return;
		}
		const bool bEnabled = FCString::Atoi(*Args[0]) != 0;
		ChunkMgr->SetStreamingEnabled(bEnabled);
		UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Stream.Enabled: streaming %s."), bEnabled ? TEXT("on") : TEXT("paused"));
	}));
