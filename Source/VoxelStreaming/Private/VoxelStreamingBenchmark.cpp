#include "VoxelStreamingBenchmark.h"
#include "VoxelChunkManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelBench, Log, All);

FVoxelStreamingBenchmark::FVoxelStreamingBenchmark(const FVoxelBenchConfig& InConfig, UVoxelChunkManager* InChunkManager)
	: Config(InConfig)
	, ChunkManager(InChunkManager)
{
	Config.Direction = Config.Direction.GetSafeNormal();
	if (Config.Direction.IsNearlyZero())
	{
		Config.Direction = FVector::ForwardVector;
	}
	CurrentPos = Config.StartPosition;

	if (ChunkManager)
	{
		ChunkManager->ResetBenchCounters();
		ChunkManager->SetBenchmarkView(true, CurrentPos);
	}
	Samples.Reserve(4096);
	SetupFlyPawn();
	UE_LOG(LogVoxelBench, Log, TEXT("Benchmark '%s' started: start=(%.0f,%.0f,%.0f) vel=%.0f dist=%.0f"),
		*Config.Tag, CurrentPos.X, CurrentPos.Y, CurrentPos.Z, Config.VelocityUU, Config.TraverseDistance);
}

void FVoxelStreamingBenchmark::SetupFlyPawn()
{
	if (!ChunkManager) { return; }
	UWorld* World = ChunkManager->GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn) { return; }
	FlyPawn = Pawn;
	OriginalPawnLocation = Pawn->GetActorLocation();
	bPawnConfigured = true;
	// A character would walk + fall through unloaded terrain; switch it to flying so it tracks
	// the bench path cleanly. Non-character pawns just get teleported each tick.
	if (ACharacter* Char = Cast<ACharacter>(Pawn))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			OriginalMovementMode = static_cast<uint8>(Move->MovementMode.GetValue());
			Move->SetMovementMode(MOVE_Flying);
			Move->StopMovementImmediately();
		}
	}
}

void FVoxelStreamingBenchmark::UpdateFlyPawn()
{
	if (APawn* Pawn = FlyPawn.Get())
	{
		Pawn->SetActorLocation(CurrentPos, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void FVoxelStreamingBenchmark::RestoreFlyPawn()
{
	if (APawn* Pawn = FlyPawn.Get())
	{
		Pawn->SetActorLocation(OriginalPawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
		if (ACharacter* Char = Cast<ACharacter>(Pawn))
		{
			if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
			{
				Move->SetMovementMode(static_cast<EMovementMode>(OriginalMovementMode));
			}
		}
	}
	FlyPawn = nullptr;
}

bool FVoxelStreamingBenchmark::QueuesDrained() const
{
	if (!ChunkManager) { return true; }
	// "Caught up" = no pending mesh-producing work. Unload backlog is tracked separately
	// (lazy deletion) so a slow unloader doesn't hold the catch-up metric hostage.
	const int32 Backlog =
		ChunkManager->GetPendingGenerationCount() +
		ChunkManager->GetPendingMeshingCount() +
		ChunkManager->GetAsyncGenerationInProgressCount() +
		ChunkManager->GetPendingMeshUploadCount();
	return Backlog <= Config.DrainedThreshold;
}

void FVoxelStreamingBenchmark::TakeSample(float DeltaTime)
{
	if (!ChunkManager) { return; }
	const auto& T = ChunkManager->GetTimingStats();

	FSample S;
	S.SimTime = SimTime;
	S.Phase = static_cast<uint8>(Phase);
	S.PosX = CurrentPos.X; S.PosY = CurrentPos.Y; S.PosZ = CurrentPos.Z;
	S.GenQueue = ChunkManager->GetPendingGenerationCount();
	S.MeshQueue = ChunkManager->GetPendingMeshingCount();
	S.UnloadQueue = ChunkManager->GetPendingUnloadCount();
	S.PendingUpload = ChunkManager->GetPendingMeshUploadCount();
	S.GenInFlight = ChunkManager->GetAsyncGenerationInProgressCount();
	S.LoadedChunks = ChunkManager->GetLoadedChunkCount();
	S.TotalChunks = ChunkManager->GetTotalChunkCount();
	S.FrameMs = DeltaTime * 1000.0f;
	S.GenMs = T.GenerationMs;
	S.MeshMs = T.MeshingMs;
	S.LODMs = T.LODMs;
	S.StreamMs = T.StreamingMs;
	S.TotalMs = T.TotalMs;
	S.RenderMs = T.RenderSubmitMs;
	S.CollMs = T.CollisionMs;
	S.ScatMs = T.ScatterMs;
	S.GenLaunchMs = T.GenLaunchMs;
	S.GenPollMs = T.GenPollMs;
	S.GenApplyMs = T.GenApplyMs;
	S.GenStoreMs = T.GenStoreMs;
	S.GenNotifyMs = T.GenNotifyMs;
	S.GenNeighborMs = T.GenNeighborMs;
	S.GenApplyCount = T.GenApplyCount;
	S.MeshTickMs = T.MeshTickMs;
	S.MeshLaunchMs = T.MeshLaunchMs;
	S.MeshApplyMs = T.MeshApplyMs;
	S.MeshSnapMs = T.MeshSnapshotMs;
	S.MeshSliceMs = T.MeshSlicesMs;
	S.MeshDispMs = T.MeshDispatchMs;
	S.MeshLaunchCount = T.MeshLaunchCount;
	S.RemeshCount = ChunkManager->GetBenchRemeshCount();
	Samples.Add(S);
}

void FVoxelStreamingBenchmark::Tick(float DeltaTime)
{
	if (Phase == EPhase::Done || !ChunkManager || DeltaTime <= 0.0f) { return; }

	SimTime += DeltaTime;
	PhaseElapsed += DeltaTime;

	// Advance the streaming origin before sampling so the sample reflects the current position.
	if (Phase == EPhase::Traverse)
	{
		DistanceTraversed += Config.VelocityUU * DeltaTime;
		CurrentPos = Config.StartPosition + Config.Direction * DistanceTraversed;
	}
	ChunkManager->SetBenchmarkView(true, CurrentPos);
	UpdateFlyPawn();

	TakeSample(DeltaTime);

	switch (Phase)
	{
	case EPhase::Warmup:
		DrainedStreak = QueuesDrained() ? (DrainedStreak + 1) : 0;
		if (DrainedStreak >= Config.EquilibriumStreak || PhaseElapsed >= Config.WarmupTimeoutSec)
		{
			Phase = EPhase::Traverse;
			PhaseElapsed = 0.0f;
			DistanceTraversed = 0.0f;
			DrainedStreak = 0;
			TraverseStartSimTime = SimTime;
			UE_LOG(LogVoxelBench, Log, TEXT("Benchmark '%s': warmup done at t=%.1fs, traversing %.0f uu"),
				*Config.Tag, SimTime, Config.TraverseDistance);
		}
		break;

	case EPhase::Traverse:
		if (DistanceTraversed >= Config.TraverseDistance)
		{
			Phase = EPhase::CatchUp;
			PhaseElapsed = 0.0f;
			DrainedStreak = 0;
			CatchUpStartSimTime = SimTime;
			UE_LOG(LogVoxelBench, Log, TEXT("Benchmark '%s': traverse done at t=%.1fs (%.0f uu), measuring catch-up"),
				*Config.Tag, SimTime, DistanceTraversed);
		}
		break;

	case EPhase::CatchUp:
		DrainedStreak = QueuesDrained() ? (DrainedStreak + 1) : 0;
		if (DrainedStreak >= Config.EquilibriumStreak)
		{
			Finish(true);
		}
		else if (PhaseElapsed >= Config.CatchUpTimeoutSec)
		{
			Finish(false);
		}
		break;

	default:
		break;
	}
}

void FVoxelStreamingBenchmark::Finish(bool bReachedEquilibrium)
{
	CatchUpDurationSec = bReachedEquilibrium ? (SimTime - CatchUpStartSimTime) : -1.0;
	Phase = EPhase::Done;
	if (ChunkManager)
	{
		ChunkManager->SetBenchmarkView(false, FVector::ZeroVector);
	}
	RestoreFlyPawn();
	WriteReport();
}

// ---- percentile over an arbitrary float projection of the traverse-phase samples ----
static float Percentile(TArray<float>& Values, float P)
{
	if (Values.Num() == 0) { return 0.0f; }
	Values.Sort();
	const int32 Idx = FMath::Clamp(FMath::RoundToInt(P * (Values.Num() - 1)), 0, Values.Num() - 1);
	return Values[Idx];
}

void FVoxelStreamingBenchmark::WriteReport()
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VoxelBench");
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	PF.CreateDirectoryTree(*Dir);
	const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString Base = Dir / FString::Printf(TEXT("%s_%s"), *Stamp, *Config.Tag);
	ReportCsvPath = Base + TEXT(".csv");

	// ---- CSV time-series ----
	FString Csv = TEXT("SimTime,Phase,PosX,PosY,PosZ,GenQ,MeshQ,UnloadQ,UploadQ,GenInFlight,Loaded,Total,FrameMs,GenMs,MeshMs,LODMs,StreamMs,TotalMs,RenderMs,CollMs,ScatMs,GenLaunchMs,GenPollMs,GenApplyMs,GenStoreMs,GenNotifyMs,GenNeighborMs,GenApplyN,MeshTickMs,MeshLaunchMs,MeshApplyMs,MeshSnapMs,MeshSliceMs,MeshDispMs,MeshLaunchN,Remesh\n");
	for (const FSample& S : Samples)
	{
		Csv += FString::Printf(TEXT("%.3f,%d,%.0f,%.0f,%.0f,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%lld\n"),
			S.SimTime, S.Phase, S.PosX, S.PosY, S.PosZ, S.GenQueue, S.MeshQueue, S.UnloadQueue, S.PendingUpload,
			S.GenInFlight, S.LoadedChunks, S.TotalChunks, S.FrameMs, S.GenMs, S.MeshMs, S.LODMs, S.StreamMs, S.TotalMs,
			S.RenderMs, S.CollMs, S.ScatMs,
			S.GenLaunchMs, S.GenPollMs, S.GenApplyMs, S.GenStoreMs, S.GenNotifyMs, S.GenNeighborMs, S.GenApplyCount,
			S.MeshTickMs, S.MeshLaunchMs, S.MeshApplyMs,
			S.MeshSnapMs, S.MeshSliceMs, S.MeshDispMs, S.MeshLaunchCount,
			S.RemeshCount);
	}
	FFileHelper::SaveStringToFile(Csv, *ReportCsvPath);

	// ---- summary over the traverse phase (the loaded window) ----
	int32 PeakGen = 0, PeakMesh = 0, PeakUnload = 0, MaxLoaded = 0;
	TArray<float> FrameMsArr, TotalMsArr;
	for (const FSample& S : Samples)
	{
		MaxLoaded = FMath::Max(MaxLoaded, S.LoadedChunks); // retention peak across all phases
		if (S.Phase != static_cast<uint8>(EPhase::Traverse)) { continue; }
		PeakGen = FMath::Max(PeakGen, S.GenQueue);
		PeakMesh = FMath::Max(PeakMesh, S.MeshQueue);
		PeakUnload = FMath::Max(PeakUnload, S.UnloadQueue);
		FrameMsArr.Add(S.FrameMs);
		TotalMsArr.Add(S.TotalMs);
	}

	double UnloadLagMean = 0.0, UnloadLagMax = 0.0; int64 UnloadLagCount = 0;
	ChunkManager->GetBenchUnloadLagStats(UnloadLagMean, UnloadLagMax, UnloadLagCount);
	double UnloadDistMean = 0.0, UnloadDistMax = 0.0;
	ChunkManager->GetBenchUnloadDistStats(UnloadDistMean, UnloadDistMax);
	const int64 Thrash = ChunkManager->GetBenchRemeshCount();
	const int64 ThrashNeighbor = ChunkManager->GetBenchRemeshByReason(EVoxelRemeshReason::NeighborRemesh);
	const int64 ThrashLOD = ChunkManager->GetBenchRemeshByReason(EVoxelRemeshReason::LODTransition);
	const int64 ThrashDirty = ChunkManager->GetBenchRemeshByReason(EVoxelRemeshReason::Dirty);
	const int64 ThrashOther = ChunkManager->GetBenchRemeshByReason(EVoxelRemeshReason::Other);
	const double TraverseDur = CatchUpStartSimTime - TraverseStartSimTime;

	FString Json;
	Json += TEXT("{\n");
	Json += FString::Printf(TEXT("  \"tag\": \"%s\",\n"), *Config.Tag);
	Json += FString::Printf(TEXT("  \"velocityUU\": %.1f,\n"), Config.VelocityUU);
	Json += FString::Printf(TEXT("  \"traverseDistance\": %.1f,\n"), Config.TraverseDistance);
	Json += FString::Printf(TEXT("  \"samples\": %d,\n"), Samples.Num());
	Json += FString::Printf(TEXT("  \"traverseDurationSec\": %.2f,\n"), TraverseDur);
	Json += FString::Printf(TEXT("  \"catchUpSec\": %.2f,\n"), CatchUpDurationSec);
	Json += FString::Printf(TEXT("  \"reachedEquilibrium\": %s,\n"), CatchUpDurationSec >= 0.0 ? TEXT("true") : TEXT("false"));
	Json += FString::Printf(TEXT("  \"peakGenQueue\": %d,\n"), PeakGen);
	Json += FString::Printf(TEXT("  \"peakMeshQueue\": %d,\n"), PeakMesh);
	Json += FString::Printf(TEXT("  \"peakUnloadQueue\": %d,\n"), PeakUnload);
	Json += FString::Printf(TEXT("  \"peakLoadedChunks\": %d,\n"), MaxLoaded);
	Json += FString::Printf(TEXT("  \"frameMsP50\": %.3f,\n"), Percentile(FrameMsArr, 0.50f));
	Json += FString::Printf(TEXT("  \"frameMsP95\": %.3f,\n"), Percentile(FrameMsArr, 0.95f));
	Json += FString::Printf(TEXT("  \"frameMsP99\": %.3f,\n"), Percentile(FrameMsArr, 0.99f));
	Json += FString::Printf(TEXT("  \"totalMsP50\": %.3f,\n"), Percentile(TotalMsArr, 0.50f));
	Json += FString::Printf(TEXT("  \"totalMsP95\": %.3f,\n"), Percentile(TotalMsArr, 0.95f));
	Json += FString::Printf(TEXT("  \"totalMsP99\": %.3f,\n"), Percentile(TotalMsArr, 0.99f));
	Json += FString::Printf(TEXT("  \"thrashRemeshCount\": %lld,\n"), Thrash);
	Json += FString::Printf(TEXT("  \"thrashNeighbor\": %lld,\n"), ThrashNeighbor);
	Json += FString::Printf(TEXT("  \"thrashLOD\": %lld,\n"), ThrashLOD);
	Json += FString::Printf(TEXT("  \"thrashDirty\": %lld,\n"), ThrashDirty);
	Json += FString::Printf(TEXT("  \"thrashOther\": %lld,\n"), ThrashOther);
	Json += FString::Printf(TEXT("  \"unloadLagMeanMs\": %.1f,\n"), UnloadLagMean);
	Json += FString::Printf(TEXT("  \"unloadLagMaxMs\": %.1f,\n"), UnloadLagMax);
	Json += FString::Printf(TEXT("  \"unloadCount\": %lld,\n"), UnloadLagCount);
	Json += FString::Printf(TEXT("  \"unloadDistMeanUU\": %.0f,\n"), UnloadDistMean);
	Json += FString::Printf(TEXT("  \"unloadDistMaxUU\": %.0f,\n"), UnloadDistMax);
	Json += FString::Printf(TEXT("  \"effMaxAsyncGen\": %d,\n"), ChunkManager->GetEffectiveMaxAsyncGenerationTasks());
	Json += FString::Printf(TEXT("  \"effMaxAsyncMesh\": %d,\n"), ChunkManager->GetEffectiveMaxAsyncMeshTasks());
	Json += FString::Printf(TEXT("  \"effMaxLODRemeshPerFrame\": %d,\n"), ChunkManager->GetEffectiveMaxLODRemeshPerFrame());
	Json += FString::Printf(TEXT("  \"effMaxPendingMeshes\": %d\n"), ChunkManager->GetEffectiveMaxPendingMeshes());
	Json += TEXT("}\n");
	FFileHelper::SaveStringToFile(Json, *(Base + TEXT(".json")));

	UE_LOG(LogVoxelBench, Warning,
		TEXT("Benchmark '%s' DONE: traverse=%.1fs catchUp=%.1fs peakMeshQ=%d peakGenQ=%d peakUnloadQ=%d frameP95=%.1fms totalP95=%.1fms thrash=%lld(nbr=%lld lod=%lld dirty=%lld other=%lld) unloadLag(mean/max)=%.0f/%.0fms unloadDist(mean/max)=%.0f/%.0fuu -> %s"),
		*Config.Tag, TraverseDur, CatchUpDurationSec, PeakMesh, PeakGen, PeakUnload,
		Percentile(FrameMsArr, 0.95f), Percentile(TotalMsArr, 0.95f), Thrash, ThrashNeighbor, ThrashLOD, ThrashDirty, ThrashOther, UnloadLagMean, UnloadLagMax, UnloadDistMean, UnloadDistMax, *ReportCsvPath);
}
