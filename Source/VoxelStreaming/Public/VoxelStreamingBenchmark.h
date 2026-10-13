// Streaming performance benchmark: drives a deterministic fixed-velocity traverse + catch-up
// scenario and samples the chunk manager's queue/scheduler dynamics each frame, then writes a
// comparable CSV time-series + JSON summary. Shared by the PIE console command and the headless
// automation driver — both just construct it and call Tick() each frame via the chunk manager.

#pragma once

#include "CoreMinimal.h"

class UVoxelChunkManager;
class APawn;

/** Shape of the path the benchmark viewer flies during the traverse phase. */
enum class EVoxelBenchPath : uint8
{
	/** StartPosition + Direction * distance. */
	Straight,
	/** StartPosition, then each of RoutePoints in order (a polyline). */
	Route,
	/** A circle of OrbitRadius around OrbitCenter, starting at the point nearest StartPosition. */
	Orbit
};

/** Scenario + equilibrium parameters for a benchmark run. */
struct FVoxelBenchConfig
{
	/** Label written into the report filename + header (use to tag A/B runs). */
	FString Tag = TEXT("bench");

	/** Where the traverse starts. Its Z is the flight altitude unless FollowTerrainAltitude is set. */
	FVector StartPosition = FVector::ZeroVector;

	/** Path shape; see EVoxelBenchPath. */
	EVoxelBenchPath PathType = EVoxelBenchPath::Straight;

	/** Straight traverse direction (normalized) and speed along any path. */
	FVector Direction = FVector::ForwardVector;
	float VelocityUU = 1500.0f;

	/** Route waypoints, visited after StartPosition (Route only). Z is used unless FollowTerrainAltitude is set. */
	TArray<FVector> RoutePoints;

	/** Orbit centre (XY) and radius (Orbit only). */
	FVector2D OrbitCenter = FVector2D::ZeroVector;
	float OrbitRadius = 0.0f;

	/** Traverse length along the path. Must exceed the initial load radius so new terrain streams.
	 *  <= 0 means the whole route (Route) or one lap (Orbit); a Straight path needs a positive length. */
	float TraverseDistance = 20000.0f;

	/** >= 0: fly this many uu above the analytic surface (UVoxelChunkManager::GetGeneratedSurfaceHeight)
	 *  instead of at a fixed Z. Heightmap world modes only (InfinitePlane, IslandBowl). */
	float FollowTerrainAltitude = -1.0f;

	/** Max time to wait for the initial load to settle before the traverse begins. Raise it to measure
	 *  how long a whole finite world takes to build (the report records the settle time). */
	float WarmupTimeoutSec = 10.0f;

	/** Max time to wait for the queues to drain after the traverse stops. */
	float CatchUpTimeoutSec = 90.0f;

	/** Queues are "drained" when gen+mesh+unload+in-flight+upload <= this for EquilibriumStreak samples. */
	int32 DrainedThreshold = 0;
	int32 EquilibriumStreak = 10;

	/** Seconds between memory samples. GetVoxelMemoryStats walks every tracked chunk, so it is not
	 *  taken every frame; samples in between repeat the last value. */
	float MemorySampleIntervalSec = 0.25f;

	/** Free-form build label recorded in the report (the sweep script passes the VoxelWorlds commit). */
	FString BuildLabel;
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
		float GenMs, MeshMs, LODMs, LODUpdateMs, StreamMs, TotalMs;
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
		// Seam-ownership pipeline (TickSeamScheduler total: scheduling + dispatch + submits)
		float SeamMs;
		// Collision sub-phases (P4a): cook-launch prep vs completed-cook apply (both GT)
		float CollPrepMs, CollApplyMs;
		// Far-compression sweep (G6): GT time, encodes started, async results installed this tick
		float FarCompMs;
		int32 FarCompDispatchN, FarCompApplyN;
		// Memory (MB) and far-compression residency, refreshed every MemorySampleIntervalSec
		float VoxelMB, EditMB, RendCPUMB, RendGPUMB, CollMB, ScatMB, ProcessMB;
		int32 ResidentN, UniformN, CompressedN, EmptyN;
		// LOD evaluation this tick (KI12): candidates found (-1 = no evaluation) and re-meshes queued; and
		// the LOD backlog when the sample was taken (Loaded chunks whose LOD change is not queued, and the
		// subset that should be at LOD 0)
		int32 LODCand, LODQueued, LODStale, LODStaleNear;
	};

	void TakeSample(float DeltaTime);
	void SampleMemory();
	/** Position along the configured path at arc length S (before terrain following). */
	FVector EvaluatePath(double S) const;
	/** Apply FollowTerrainAltitude to a path position. */
	FVector ApplyAltitude(const FVector& Pos) const;
	/** Total path length for Route/Orbit (Straight returns TraverseDistance). */
	double GetPathLength() const;
	/** Run metadata (map, config, modes, flags, sizes, path) as a JSON object body. */
	FString BuildRunMetadataJson() const;
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
	bool bWarmupDrained = false;      // warmup ended because the queues drained (not on timeout)

	// Route: cumulative arc length at each vertex (StartPosition first). Orbit: start angle.
	TArray<FVector> RouteVertices;
	TArray<double> RouteCumulative;
	double OrbitStartAngle = 0.0;

	// Loaded chunks per layer (Z), taken with each traverse memory sample: running sum and max (§13.11).
	TMap<int32, int64> LayerLoadedSum;
	TMap<int32, int32> LayerLoadedMax;
	int32 LayerSamples = 0;

	// Latest memory sample (carried between MemorySampleIntervalSec refreshes) and its timer.
	FSample LastMemory = {};
	float MemorySampleTimer = 0.0f;

	// Residency audit taken when the run finishes (after catch-up).
	int32 AuditFindings = -1;
	FString AuditReport;

	TArray<FSample> Samples;
	FString ReportCsvPath;

	// Fly-pawn state (so the viewer flies the path instead of a character running + falling through).
	TWeakObjectPtr<APawn> FlyPawn;
	FVector OriginalPawnLocation = FVector::ZeroVector;
	uint8 OriginalMovementMode = 0;
	bool bPawnConfigured = false;
};
