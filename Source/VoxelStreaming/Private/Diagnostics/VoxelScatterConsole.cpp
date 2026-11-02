// Copyright Daniel Raquel. All Rights Reserved.

// Scatter's console commands that read the chunk manager (voxel.Scatter.Stats, voxel.Scatter.EditAudit). They live in
// VoxelStreaming because VoxelScatter does not depend on it.

#include "VoxelChunkManager.h"
#include "Diagnostics/VoxelConsoleHelpers.h"
#include "VoxelScatterRenderer.h"
#include "VoxelStreaming.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "VoxelEditManager.h"
#include "VoxelScatterManager.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Console: voxel.Scatter.Stats [hism|reset]
// Dump scatter manager + renderer statistics (HISM counts, pool active/pooled/util, pending
// adds, chunk/surface/spawn totals, memory, worker extraction timing) for perf and config profiling. Read the output
// with unreal-mcp LogsToolset.GetLogEntries (category LogVoxelStreaming) since Claudius
// get_output_log fails while PIE holds the log file open.
static FAutoConsoleCommandWithWorldAndArgs GVoxelScatterStatsCmd(
	TEXT("voxel.Scatter.Stats"),
	TEXT("Log scatter manager + renderer statistics for the running game world."),
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
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Stats: no PIE/Game world found (start PIE first)"));
			return;
		}

		int32 Managers = 0;
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM) { continue; }
			UVoxelScatterManager* ScatterMgr = CM->GetScatterManager();
			if (!ScatterMgr) { continue; }
			++Managers;
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Stats:\n%s"), *ScatterMgr->GetDebugStats());
			// "voxel.Scatter.Stats reset": restart the worker extraction timing (world modes review G28)
			if (Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase))
			{
				ScatterMgr->ResetExtractionTiming();
			}
			// "voxel.Scatter.Stats hism": the HISMs by size (world modes review G42)
			if (Args.Num() > 0 && Args[0].Equals(TEXT("hism"), ESearchCase::IgnoreCase) && ScatterMgr->GetScatterRenderer())
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Stats hism:\n%s"), *ScatterMgr->GetScatterRenderer()->GetHISMBreakdown(12));
			}
		}
		if (Managers == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Stats: no UVoxelChunkManager with a scatter manager found"));
		}
	}));

// Console: voxel.Scatter.EditAudit [maxLines=20]
// World modes review G33: whether every cached scatter chunk's surface points are what a fresh extraction gives now
// (UVoxelScatterManager::CompareWithFreshExtraction: the chunk's edit-merged voxels and a context made now, so the edits of
// the chunks it reads past its faces and the conditioning zones as they are now). A chunk that differs holds a stale view of
// something it reads; the audit says whether it reads an edited chunk (a heightmap chunk the chunk above, a planet chunk the
// seven at +X / +Y / +Z: a system edit it was not re-extracted for, G33), has edits itself, or neither (a conditioning zone
// registered after it was extracted, for instance). Chunks queued or in flight are skipped, and so are chunks with a cleared
// volume (a player edit only clears scatter, by design). Also logs what system and editor edits did to scatter
// (UVoxelChunkManager::GetEditScatterStats) and voxel.Scatter.EditReaders. Logs the stale chunks with the most changed
// points with a vox.TP to each and writes Saved/VoxelAudit/EditAudit_<world>_<time>.csv.
static FAutoConsoleCommandWithWorldAndArgs GVoxelScatterEditAuditCmd(
	TEXT("voxel.Scatter.EditAudit"),
	TEXT("Compare every cached scatter chunk's surface points with a fresh extraction now (stale ")
	TEXT("scatter next to a system edit). Optional arg: max chunk lines to log."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.EditAudit: no PIE/Game world found (start PIE first)"));
			return;
		}
		const int32 MaxLines = Args.Num() > 0 ? FMath::Max(0, FCString::Atoi(*Args[0])) : 20;
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			const UVoxelScatterManager* ScatterMgr = CM ? CM->GetScatterManager() : nullptr;
			const UVoxelWorldConfiguration* Config = CM ? CM->GetConfiguration() : nullptr;
			const UVoxelEditManager* EditMgr = CM ? CM->GetEditManager() : nullptr;
			const IVoxelWorldMode* Mode = CM ? CM->GetWorldMode() : nullptr;
			if (!ScatterMgr || !Config || !EditMgr || !Mode)
			{
				continue;
			}
			const double T0 = FPlatformTime::Seconds();
			TArray<UVoxelScatterManager::FFreshExtractionComparison> Stale;
			const UVoxelScatterManager::FFreshExtractionStats Stats = ScatterMgr->CompareWithFreshExtraction(
				[CM](const FIntVector& Coord) { return CM->GetVoxelSnapshot(Coord); }, Stale);
			const double Seconds = FPlatformTime::Seconds() - T0;

			// The chunks a chunk reads past its faces: the inverse of the reader offsets.
			TArray<FIntVector> ReaderOffsets;
			UVoxelScatterManager::GetEditReaderOffsets(Mode->GetWorldModeType() == EWorldMode::SphericalPlanet, nullptr, ReaderOffsets);
			int32 ReadsEdited = 0, OwnEdits = 0, Other = 0, ChangedPoints = 0;
			TArray<FString> Kinds;
			for (const UVoxelScatterManager::FFreshExtractionComparison& C : Stale)
			{
				bool bReadsEdited = false;
				for (const FIntVector& Offset : ReaderOffsets)
				{
					bReadsEdited |= EditMgr->ChunkHasEdits(C.ChunkCoord - Offset);
				}
				const bool bOwn = EditMgr->ChunkHasEdits(C.ChunkCoord);
				ReadsEdited += bReadsEdited ? 1 : 0;
				OwnEdits += bOwn ? 1 : 0;
				Other += (!bReadsEdited && !bOwn) ? 1 : 0;
				ChangedPoints += C.ChangedPoints;
				Kinds.Add(FString::Printf(TEXT("%s%s"), bReadsEdited ? TEXT("readsEdited") : TEXT(""), bOwn ? (bReadsEdited ? TEXT("+own") : TEXT("own")) : (bReadsEdited ? TEXT("") : TEXT("neither"))));
			}
			const UVoxelChunkManager::FEditScatterStats& E = CM->GetEditScatterStats();
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.EditAudit [%s]: compared %d cached chunk(s) in %.2f s: stale %d (%d changed points; reading an edited chunk %d, edited themselves %d, neither %d); skipped %d pending, %d with a cleared volume, %d without voxels; edits: %lld system broadcasts, readers re-extracted %lld, folded %lld, without data %lld; scatter regenerations %lld, extractions launched %lld"),
				*TargetWorld->GetName(), Stats.Compared, Seconds, Stats.Stale, ChangedPoints, ReadsEdited, OwnEdits, Other, Stats.SkippedPending,
				Stats.SkippedCleared, Stats.SkippedNoVoxels, E.Broadcasts, E.ReadersReextracted, E.ReadersFolded,
				E.ReadersWithoutData, ScatterMgr->GetTotalRegenerations(), ScatterMgr->GetTotalExtractionsLaunched());
			const double ChunkWorldSize = Config->GetChunkWorldSize();
			for (int32 i = 0; i < FMath::Min(MaxLines, Stale.Num()); ++i)
			{
				const UVoxelScatterManager::FFreshExtractionComparison& C = Stale[i];
				const FVector Centre = Config->WorldOrigin + (FVector(C.ChunkCoord) + FVector(0.5)) * ChunkWorldSize;
				UE_LOG(LogVoxelStreaming, Warning, TEXT("  chunk (%d,%d,%d) %s: cached %d, fresh %d, changed %d  ->  vox.TP %.0f %.0f %.0f"),
					C.ChunkCoord.X, C.ChunkCoord.Y, C.ChunkCoord.Z, *Kinds[i], C.CachedPoints, C.FreshPoints, C.ChangedPoints, Centre.X, Centre.Y, Centre.Z);
			}
			TArray<FString> CsvLines = { TEXT("X,Y,Z,kind,cached,fresh,changed") };
			for (int32 i = 0; i < Stale.Num(); ++i)
			{
				const UVoxelScatterManager::FFreshExtractionComparison& C = Stale[i];
				CsvLines.Add(FString::Printf(TEXT("%d,%d,%d,%s,%d,%d,%d"), C.ChunkCoord.X, C.ChunkCoord.Y, C.ChunkCoord.Z, *Kinds[i], C.CachedPoints,
					C.FreshPoints, C.ChangedPoints));
			}
			const FString Path = FPaths::ProjectSavedDir() / TEXT("VoxelAudit") / FString::Printf(TEXT("EditAudit_%s_%s.csv"),
				*TargetWorld->GetName(), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
			if (FFileHelper::SaveStringArrayToFile(CsvLines, *Path))
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.EditAudit: %d stale chunk(s) written to %s"), Stale.Num(), *FPaths::ConvertRelativePathToFull(Path));
			}
		}
	}));
