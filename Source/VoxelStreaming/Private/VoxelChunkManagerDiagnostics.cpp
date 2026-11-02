// Copyright Daniel Raquel. All Rights Reserved.

// UVoxelChunkManager's debug stats, residency audit, voxel memory stats and debug drawing: a second translation
// unit of the class, read-only over its state.

#include "VoxelChunkManager.h"
#include "VoxelChunkManagerPrivate.h"
#include "VoxelStreaming.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "VoxelEditManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelScatterManager.h"
#include "VoxelCoordinates.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"

// ==================== Debug ====================

FString UVoxelChunkManager::GetDebugStats() const
{
	FString Stats = TEXT("=== VoxelChunkManager ===\n");
	Stats += FString::Printf(TEXT("Initialized: %s\n"), bIsInitialized ? TEXT("Yes") : TEXT("No"));
	Stats += FString::Printf(TEXT("Streaming: %s\n"), bStreamingEnabled ? TEXT("Enabled") : TEXT("Disabled"));
	Stats += FString::Printf(TEXT("Frame: %lld\n"), CurrentFrame);
	Stats += TEXT("\n");

	Stats += TEXT("--- Chunk Counts ---\n");
	Stats += FString::Printf(TEXT("Total Tracked: %d\n"), ChunkStates.Num());
	Stats += FString::Printf(TEXT("Loaded: %d\n"), LoadedChunkCoords.Num());
	Stats += FString::Printf(TEXT("Generation Queue: %d\n"), GenerationQueue.Num());
	Stats += FString::Printf(TEXT("Meshing Queue: %d\n"), MeshingQueue.Num());
	Stats += FString::Printf(TEXT("Unload Queue: %d\n"), UnloadQueue.Num());
	Stats += TEXT("\n");

	Stats += TEXT("--- Session Stats ---\n");
	Stats += FString::Printf(TEXT("Total Generated: %lld\n"), TotalChunksGenerated);
	Stats += FString::Printf(TEXT("Total Meshed: %lld\n"), TotalChunksMeshed);
	Stats += FString::Printf(TEXT("Total Unloaded: %lld\n"), TotalChunksUnloaded);
	Stats += TEXT("\n");

	// Count chunks by state
	int32 StateCounts[(int32)EChunkState::PendingUnload + 1] = {0};
	for (const auto& Pair : ChunkStates)
	{
		const int32 StateIndex = (int32)Pair.Value.State;
		if (StateIndex >= 0 && StateIndex < UE_ARRAY_COUNT(StateCounts))
		{
			++StateCounts[StateIndex];
		}
	}

	Stats += TEXT("--- Chunks by State ---\n");
	Stats += FString::Printf(TEXT("Unloaded: %d\n"), StateCounts[(int32)EChunkState::Unloaded]);
	Stats += FString::Printf(TEXT("PendingGeneration: %d\n"), StateCounts[(int32)EChunkState::PendingGeneration]);
	Stats += FString::Printf(TEXT("Generating: %d\n"), StateCounts[(int32)EChunkState::Generating]);
	Stats += FString::Printf(TEXT("PendingMeshing: %d\n"), StateCounts[(int32)EChunkState::PendingMeshing]);
	Stats += FString::Printf(TEXT("Meshing: %d\n"), StateCounts[(int32)EChunkState::Meshing]);
	Stats += FString::Printf(TEXT("Loaded: %d\n"), StateCounts[(int32)EChunkState::Loaded]);
	Stats += FString::Printf(TEXT("PendingUnload: %d\n"), StateCounts[(int32)EChunkState::PendingUnload]);

	// Far-chunk compression residency breakdown + memory reclaimed.
	{
		const FVoxelMemoryStats Mem = GetVoxelMemoryStats();
		Stats += TEXT("\n--- Voxel Data Residency ---\n");
		Stats += FString::Printf(TEXT("Resident: %d\n"), Mem.ResidentChunks);
		Stats += FString::Printf(TEXT("Uniform: %d\n"), Mem.UniformChunks);
		Stats += FString::Printf(TEXT("Compressed: %d\n"), Mem.CompressedChunks);
		Stats += FString::Printf(TEXT("Empty: %d\n"), Mem.EmptyChunks);
		Stats += FString::Printf(TEXT("Resident voxel data: %.1f MB\n"), Mem.VoxelDataBytes / (1024.0 * 1024.0));
		Stats += FString::Printf(TEXT("Reclaimed by compression: %.1f MB\n"), Mem.CompressionSavedBytes / (1024.0 * 1024.0));
	}

	// Seam-ownership registry.
	if (SeamRegistry.IsValid())
	{
		const FVoxelSeamRegistryStats S = SeamRegistry->GetStats();
		Stats += TEXT("\n--- Seam Registry ---\n");
		Stats += FString::Printf(TEXT("Seam types: %s\n"), SeamRegistry->IsFaceSeamsOnly() ? TEXT("faces (cubic)") : TEXT("faces, edges, corners"));
		Stats += FString::Printf(TEXT("Chunks/Seams/Dirty/Queued: %d / %d / %d / %d\n"),
			S.ChunkCount, S.SeamCount, S.DirtyCount, S.JobQueueDepth);
		Stats += FString::Printf(TEXT("Lifetime seams/scheduled/processed: %lld / %lld / %lld\n"),
			S.TotalSeamsCreated, S.TotalSeamJobsScheduled, S.TotalSeamJobsProcessed);
		Stats += FString::Printf(TEXT("Late results rejected (owner left): %lld\n"), S.TotalSeamResultsRejected);
		Stats += FString::Printf(TEXT("LOD transition holds: open %d (seams pending %d) | lifetime %lld, timeouts %lld, max %.0f ms | deferred buckets %d\n"),
			SeamHolds.NumGroups(), SeamHolds.NumPendingSeams(), TotalSeamHoldGroups, TotalSeamHoldTimeouts, SeamHoldSecondsMaxLifetime * 1000.0,
			SeamHolds.NumDeferredBuckets());
	}

	// Async pipeline health (R23c): in-flight counts, worst dispatch->complete, watchdog re-queues.
	Stats += TEXT("\n--- Async Jobs ---\n");
	Stats += FString::Printf(TEXT("In flight gen/mesh/seam: %d / %d / %d\n"),
		AsyncGenerationInProgress.Num(), AsyncMeshingInProgress.Num(), SeamJobsInFlight.Num());
	Stats += FString::Printf(TEXT("Worst dispatch->complete gen/mesh: %.0f / %.0f ms\n"),
		WorstGenerationAsyncSeconds * 1000.0, WorstMeshAsyncSeconds * 1000.0);
	Stats += FString::Printf(TEXT("Stuck re-queues gen/mesh: %lld / %lld\n"),
		TotalStuckGenerationRequeues, TotalStuckMeshRequeues);

	if (LODStrategy)
	{
		Stats += TEXT("\n");
		Stats += LODStrategy->GetDebugInfo();
	}

	if (MeshRenderer)
	{
		Stats += TEXT("\n");
		Stats += MeshRenderer->GetDebugStats();
	}

	return Stats;
}

int32 UVoxelChunkManager::AuditResidency(FString& OutReport) const
{
	OutReport.Reset();
	if (!bIsInitialized || !Configuration)
	{
		OutReport = TEXT("residency audit: chunk manager not initialized");
		return 0;
	}

	static const TCHAR* StateNames[] = {
		TEXT("Unloaded"), TEXT("PendingGeneration"), TEXT("Generating"), TEXT("PendingMeshing"),
		TEXT("Meshing"), TEXT("Loaded"), TEXT("PendingUnload") };
	constexpr int32 NumStates = UE_ARRAY_COUNT(StateNames);
	static_assert(NumStates == static_cast<int32>(EChunkState::PendingUnload) + 1, "StateNames must cover EChunkState");
	auto StateName = [](EChunkState State)
	{
		const int32 Index = static_cast<int32>(State);
		return (Index >= 0 && Index < NumStates) ? StateNames[Index] : TEXT("?");
	};

	constexpr int32 MaxListed = 8;
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	// The strategy's horizon distance (horizontal in heightmap worlds), so "beyond the unload radius" here is the
	// strategy's own unload rule (KI11: the island measured 2D while this audit measured 3D).
	auto DistanceToViewer = [this, ChunkWorldSize](const FIntVector& Coord)
	{
		const FVector Center = Configuration->WorldOrigin + FVector(Coord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
		return LODStrategy ? LODStrategy->GetStreamingHorizon(Center, CurrentViewerPosition) : static_cast<float>(FVector::Dist(Center, CurrentViewerPosition));
	};
	auto ListFarthest = [&](TArray<TPair<float, FIntVector>>& Entries, const TCHAR* Label, TFunctionRef<FString(const FIntVector&)> Extra, FString& Out)
	{
		Entries.Sort([](const TPair<float, FIntVector>& A, const TPair<float, FIntVector>& B) { return A.Key > B.Key; });
		for (int32 i = 0; i < FMath::Min(Entries.Num(), MaxListed); ++i)
		{
			const FIntVector& C = Entries[i].Value;
			Out += FString::Printf(TEXT("\n  %s (%d,%d,%d) dist=%.0f%s"), Label, C.X, C.Y, C.Z, Entries[i].Key, *Extra(C));
		}
	};

	FString Details;

	// 1. Chunks the manager still tracks beyond the unload radius, by state.
	int32 BeyondByState[NumStates] = {};
	int32 BeyondTotal = 0;
	const float UnloadDistance = LODStrategy ? LODStrategy->GetUnloadDistance() : -1.0f;
	if (UnloadDistance > 0.0f)
	{
		TArray<TPair<float, FIntVector>> Beyond;
		for (const TPair<FIntVector, FVoxelChunkState>& Pair : ChunkStates)
		{
			const float Distance = DistanceToViewer(Pair.Key);
			if (Distance > UnloadDistance)
			{
				const int32 StateIndex = static_cast<int32>(Pair.Value.State);
				if (StateIndex >= 0 && StateIndex < NumStates)
				{
					++BeyondByState[StateIndex];
				}
				++BeyondTotal;
				Beyond.Emplace(Distance, Pair.Key);
			}
		}
		ListFarthest(Beyond, TEXT("beyond-unload"), [this, &StateName](const FIntVector& C)
		{
			const FVoxelChunkState& S = ChunkStates.FindChecked(C);
			return FString::Printf(TEXT(" state=%s edited=%d rendered=%d meshInFlight=%d genInFlight=%d"),
				StateName(S.State),
				(EditManager && EditManager->ChunkHasEdits(C)) ? 1 : 0,
				(MeshRenderer && MeshRenderer->IsChunkLoaded(C)) ? 1 : 0,
				AsyncMeshingInProgress.Contains(C) ? 1 : 0,
				AsyncGenerationInProgress.Contains(C) ? 1 : 0);
		}, Details);
	}

	// 2. Renderer chunks the manager no longer tracks (geometry with no owner to remove it).
	int32 RenderOrphans = 0;
	if (MeshRenderer)
	{
		TArray<FIntVector> RenderChunks;
		MeshRenderer->GetLoadedChunks(RenderChunks);
		TArray<TPair<float, FIntVector>> Orphans;
		for (const FIntVector& C : RenderChunks)
		{
			if (!ChunkStates.Contains(C))
			{
				Orphans.Emplace(DistanceToViewer(C), C);
			}
		}
		RenderOrphans = Orphans.Num();
		ListFarthest(Orphans, TEXT("render-orphan"), [](const FIntVector&) { return FString(); }, Details);
	}

	// 3. Seam buckets whose owner is not tracked: the renderer's buckets (what is drawn) and the
	// manager's per-owner slot store (what a later merge would resubmit). R23a's floating strips.
	int32 SeamBucketOrphans = 0;
	if (MeshRenderer)
	{
		TArray<FIntVector> SeamOwners;
		MeshRenderer->GetSeamMeshOwners(SeamOwners);
		TArray<TPair<float, FIntVector>> Orphans;
		for (const FIntVector& C : SeamOwners)
		{
			if (!ChunkStates.Contains(C))
			{
				Orphans.Emplace(DistanceToViewer(C), C);
			}
		}
		SeamBucketOrphans = Orphans.Num();
		ListFarthest(Orphans, TEXT("seam-bucket-orphan"), [](const FIntVector&) { return FString(); }, Details);
	}
	int32 SeamSlotOrphans = 0;
	{
		TArray<TPair<float, FIntVector>> Orphans;
		for (const TPair<FIntVector, FSeamOwnerSlots>& Pair : SeamOwnerSlots)
		{
			if (!ChunkStates.Contains(Pair.Key))
			{
				Orphans.Emplace(DistanceToViewer(Pair.Key), Pair.Key);
			}
		}
		SeamSlotOrphans = Orphans.Num();
		ListFarthest(Orphans, TEXT("seam-slot-orphan"), [](const FIntVector&) { return FString(); }, Details);
	}

	// 4. Async jobs in flight longer than the stuck threshold (the watchdog should have released them).
	const double Now = FPlatformTime::Seconds();
	const float StuckSeconds = CVarStreamMeshStuckSeconds.GetValueOnGameThread();
	const double StuckThreshold = (StuckSeconds > 0.0f) ? StuckSeconds : 10.0;
	auto CountStuck = [&](const TMap<FIntVector, FAsyncJobStamp>& InFlight, const TCHAR* Label) -> int32
	{
		int32 Count = 0;
		for (const TPair<FIntVector, FAsyncJobStamp>& Pair : InFlight)
		{
			const double Age = Now - Pair.Value.DispatchedAtSeconds;
			if (Age > StuckThreshold)
			{
				if (Count < MaxListed)
				{
					const FVoxelChunkState* S = ChunkStates.Find(Pair.Key);
					Details += FString::Printf(TEXT("\n  %s (%d,%d,%d) age=%.1fs state=%s"), Label,
						Pair.Key.X, Pair.Key.Y, Pair.Key.Z, Age, S ? StateName(S->State) : TEXT("untracked"));
				}
				++Count;
			}
		}
		return Count;
	};
	const int32 StuckGen = CountStuck(AsyncGenerationInProgress, TEXT("stuck-gen"));
	const int32 StuckMesh = CountStuck(AsyncMeshingInProgress, TEXT("stuck-mesh"));

	const int32 Findings = BeyondTotal + RenderOrphans + SeamBucketOrphans + SeamSlotOrphans + StuckGen + StuckMesh;

	FString ByState;
	for (int32 i = 0; i < NumStates; ++i)
	{
		if (BeyondByState[i] > 0)
		{
			ByState += FString::Printf(TEXT(" %s=%d"), StateNames[i], BeyondByState[i]);
		}
	}
	const int64 SeamRejected = SeamRegistry.IsValid() ? SeamRegistry->GetStats().TotalSeamResultsRejected : 0;
	OutReport = FString::Printf(
		TEXT("residency audit: findings=%d | beyondUnload=%d (radius %.0f)%s | renderOrphans=%d | seamBucketOrphans=%d seamSlotOrphans=%d")
		TEXT(" | stuckJobs gen=%d mesh=%d (>%.1fs) | viewer=(%.0f,%.0f,%.0f) tracked=%d loaded=%d")
		TEXT(" | inFlight gen=%d mesh=%d seam=%d | worst dispatch->complete gen=%.0fms mesh=%.0fms")
		TEXT(" | stuckRequeues gen=%lld mesh=%lld | lateSeamResultsRejected=%lld"),
		Findings, BeyondTotal, UnloadDistance, *ByState, RenderOrphans, SeamBucketOrphans, SeamSlotOrphans,
		StuckGen, StuckMesh, StuckThreshold,
		CurrentViewerPosition.X, CurrentViewerPosition.Y, CurrentViewerPosition.Z, ChunkStates.Num(), LoadedChunkCoords.Num(),
		AsyncGenerationInProgress.Num(), AsyncMeshingInProgress.Num(), SeamJobsInFlight.Num(),
		WorstGenerationAsyncSeconds * 1000.0, WorstMeshAsyncSeconds * 1000.0,
		TotalStuckGenerationRequeues, TotalStuckMeshRequeues, SeamRejected);
	OutReport += Details;
	return Findings;
}

UVoxelChunkManager::FVoxelMemoryStats UVoxelChunkManager::GetVoxelMemoryStats() const
{
	FVoxelMemoryStats Stats;

	// Voxel data in chunk states
	for (const auto& Pair : ChunkStates)
	{
		const FChunkDescriptor& D = Pair.Value.Descriptor;
		Stats.VoxelDataBytes += D.GetMemoryUsage();

		const int64 FullBytes = static_cast<int64>(D.GetTotalVoxels()) * sizeof(FVoxelData);
		switch (D.Residency)
		{
		case EVoxelDataResidency::Resident:
			++Stats.ResidentChunks;
			break;
		case EVoxelDataResidency::Uniform:
			++Stats.UniformChunks;
			Stats.CompressionSavedBytes += FullBytes; // raw array dropped; UniformValue is in the struct
			break;
		case EVoxelDataResidency::Compressed:
			++Stats.CompressedChunks;
			Stats.CompressionSavedBytes += FullBytes - static_cast<int64>(D.CompressedVoxelData.GetAllocatedSize());
			break;
		case EVoxelDataResidency::Empty:
		default:
			++Stats.EmptyChunks;
			break;
		}
	}

	// Shared snapshots cached for seam jobs and cooks (G65: a far-compressed chunk's is decoded on a worker; counted at its
	// full size while it decodes; G71: a resident chunk's own array it shares is in the voxel data above).
	for (const auto& Pair : VoxelSnapshotCache)
	{
		Stats.SnapshotBytes += GetSnapshotOwnBytes(Pair.Key, Pair.Value);
	}

	// The neighbours' boundary shells for collision cooks (G69), the uniform values' shared shells once each.
	{
		TSet<const FVoxelFaceSlabs*> Shared;
		for (const auto& Pair : UniformFaceSlabs)
		{
			if (Pair.Value.IsValid())
			{
				Shared.Add(Pair.Value.Get());
				Stats.ShellBytes += static_cast<int64>(Pair.Value->GetAllocatedSize());
			}
		}
		const int64 ChunkSizeForShell = Configuration ? Configuration->ChunkSize : 0;
		for (const auto& Pair : FaceSlabsCache)
		{
			const FChunkFaceSlabsEntry& Entry = Pair.Value;
			if (!Entry.Shell.IsValid() || Shared.Contains(Entry.Shell.Get()))
			{
				continue;
			}
			const bool bPending = Entry.Ready.IsValid() && !Entry.Ready.IsCompleted();
			Stats.ShellBytes += bPending
				? 6 * static_cast<int64>(Entry.Depth) * ChunkSizeForShell * ChunkSizeForShell * static_cast<int64>(sizeof(FVoxelData))
				: static_cast<int64>(Entry.Shell->GetAllocatedSize());
		}
		Stats.SnapshotBytes += Stats.ShellBytes;
	}

	// The uniform values' shared snapshot arrays (G69), once each.
	for (const auto& Pair : UniformSnapshotArrays)
	{
		Stats.SnapshotBytes += Pair.Value.IsValid() ? static_cast<int64>(Pair.Value->GetAllocatedSize()) : 0;
	}

	// Edit manager
	if (EditManager)
	{
		Stats.EditDataBytes = static_cast<int64>(EditManager->GetMemoryUsage());
	}

	// Renderer
	if (MeshRenderer)
	{
		Stats.RendererGPUBytes = MeshRenderer->GetGPUMemoryUsage();
	}

	// Collision
	if (CollisionManager)
	{
		Stats.CollisionBytes = CollisionManager->GetTotalMemoryUsage();
	}

	// Scatter
	if (ScatterManager)
	{
		Stats.ScatterBytes = ScatterManager->GetTotalMemoryUsage();
	}

	Stats.TotalBytes = Stats.VoxelDataBytes + Stats.EditDataBytes
		+ Stats.RendererGPUBytes + Stats.CollisionBytes + Stats.ScatterBytes;

	return Stats;
}

void UVoxelChunkManager::DrawDebugVisualization() const
{
#if ENABLE_DRAW_DEBUG
	if (!bIsInitialized || !GetWorld())
	{
		return;
	}

	// Draw LOD strategy visualization
	if (LODStrategy)
	{
		FLODQueryContext Context = BuildStreamingContext();
		LODStrategy->DrawDebugVisualization(GetWorld(), Context);
	}

	// Draw chunk bounds colored by state
	for (const auto& Pair : ChunkStates)
	{
		const FIntVector& ChunkCoord = Pair.Key;
		const FVoxelChunkState& State = Pair.Value;

		FColor Color;
		switch (State.State)
		{
			case EChunkState::Loaded:           Color = FColor::Green; break;
			case EChunkState::PendingGeneration:Color = FColor::Yellow; break;
			case EChunkState::Generating:       Color = FColor::Orange; break;
			case EChunkState::PendingMeshing:   Color = FColor::Cyan; break;
			case EChunkState::Meshing:          Color = FColor::Blue; break;
			case EChunkState::PendingUnload:    Color = FColor::Red; break;
			default:                            Color = FColor::White; break;
		}

		const FBox LocalBounds = FVoxelCoordinates::ChunkToWorldBounds(
			ChunkCoord,
			Configuration->ChunkSize,
			Configuration->VoxelSize
		);
		// Add WorldOrigin offset for correct world-space position
		const FBox Bounds(LocalBounds.Min + Configuration->WorldOrigin, LocalBounds.Max + Configuration->WorldOrigin);

		DrawDebugBox(GetWorld(), Bounds.GetCenter(), Bounds.GetExtent(), Color, false, -1.0f, 0, 2.0f);
	}
#endif
}
