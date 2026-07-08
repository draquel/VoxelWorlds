// Streaming performance benchmark: drives a deterministic fixed-velocity traverse + catch-up
// scenario and samples the chunk manager's queue/scheduler dynamics each frame, then writes a
// comparable CSV time-series + JSON summary. Shared by the PIE console command and the headless
// automation driver — both just construct it and call Tick() each frame via the chunk manager.

#pragma once

#include "CoreMinimal.h"

class UVoxelChunkManager;
class APawn;

/** Scenario + equilibrium parameters for a benchmark run. */
struct FVoxelBenchConfig
{
	/** Label written into the report filename + header (use to tag A/B runs). */
	FString Tag = TEXT("bench");

	/** Where the traverse starts (the driver lowers Z to keep the leading edge at LOD0). */
	FVector StartPosition = FVector::ZeroVector;

	/** Straight traverse direction (normalized) and speed. */
	FVector Direction = FVector::ForwardVector;
	float VelocityUU = 1500.0f;

	/** Traverse length — should exceed the initial load radius so new terrain must stream. */
	float TraverseDistance = 20000.0f;

	/** Max time to wait for the initial load to settle before the traverse begins. */
	float WarmupTimeoutSec = 10.0f;

	/** Max time to wait for the queues to drain after the traverse stops. */
	float CatchUpTimeoutSec = 90.0f;

	/** Queues are "drained" when gen+mesh+unload+in-flight+upload <= this for EquilibriumStreak samples. */
	int32 DrainedThreshold = 0;
	int32 EquilibriumStreak = 10;
};

/**
 * Deterministic streaming benchmark. Construct with a config + chunk manager, then call Tick()
 * once per frame (the chunk manager does this from TickComponent while a run is active). On
 * completion it releases the benchmark view override and writes the report to Saved/VoxelBench/.
 */
class VOXELSTREAMING_API FVoxelStreamingBenchmark
{
public:
	FVoxelStreamingBenchmark(const FVoxelBenchConfig& InConfig, UVoxelChunkManager* InChunkManager);

	/** Advance the scenario, drive the streaming origin, and take a sample. */
	void Tick(float DeltaTime);

	bool IsDone() const { return Phase == EPhase::Done; }

	/** Absolute path of the written CSV once the run finishes (empty until then). */
	const FString& GetReportPath() const { return ReportCsvPath; }

private:
	enum class EPhase : uint8 { Warmup, Traverse, CatchUp, Done };

	struct FSample
	{
		double SimTime;
		uint8 Phase;
		double PosX, PosY, PosZ;
		int32 GenQueue, MeshQueue, UnloadQueue, PendingUpload, GenInFlight;
		int32 LoadedChunks, TotalChunks;
		float FrameMs;
		float GenMs, MeshMs, LODMs, StreamMs, TotalMs;
		// Previously-unsampled tick sections + generation/meshing sub-phases (spike attribution)
		float RenderMs, CollMs, ScatMs;
		float GenLaunchMs, GenPollMs, GenApplyMs, GenStoreMs, GenNotifyMs, GenNeighborMs;
		int32 GenApplyCount;
		float MeshTickMs, MeshLaunchMs, MeshApplyMs;
		float MeshSnapMs, MeshSliceMs, MeshDispMs;
		int32 MeshLaunchCount;
		float RendMeshMs, RendSubRendMs, RendSubScatMs, RendSubWatMs;
		float RendUnloadMs, RendWTileMs, RendFlushMs;
		int32 RendSubmitCount;
		int64 RemeshCount;
	};

	void TakeSample(float DeltaTime);
	bool QueuesDrained() const;
	void Finish(bool bReachedEquilibrium);
	void WriteReport();

	/** Fly the player pawn along the path (teleport + no gravity) so it stays on loaded terrain
	 *  and never falls through, and the rendered view follows the flight. Restored on finish. */
	void SetupFlyPawn();
	void UpdateFlyPawn();
	void RestoreFlyPawn();

	FVoxelBenchConfig Config;
	UVoxelChunkManager* ChunkManager = nullptr;

	EPhase Phase = EPhase::Warmup;
	double SimTime = 0.0;
	float PhaseElapsed = 0.0f;
	float DistanceTraversed = 0.0f;
	int32 DrainedStreak = 0;
	FVector CurrentPos = FVector::ZeroVector;

	double TraverseStartSimTime = 0.0;
	double CatchUpStartSimTime = 0.0;
	double CatchUpDurationSec = -1.0; // <0 == did not reach equilibrium before timeout

	TArray<FSample> Samples;
	FString ReportCsvPath;

	// Fly-pawn state (so the viewer flies the path instead of a character running + falling through).
	TWeakObjectPtr<APawn> FlyPawn;
	FVector OriginalPawnLocation = FVector::ZeroVector;
	uint8 OriginalMovementMode = 0;
	bool bPawnConfigured = false;
};
