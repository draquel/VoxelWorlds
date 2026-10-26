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
	Orbit,
	/**
	 * A planet's great circle (spherical Phase 4, SP-I4): from GreatCircleStart along GreatCircleHeading, at
	 * GreatCircleAltitude over the datum sphere of GreatCircleRadius around GreatCircleCenter.
	 */
	GreatCircle
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

	/** GreatCircle only: the planet centre and datum radius, the start direction and the heading there (unit, tangent). */
	FVector GreatCircleCenter = FVector::ZeroVector;
	double GreatCircleRadius = 0.0;
	FVector GreatCircleStart = FVector::UpVector;
	FVector GreatCircleHeading = FVector::ForwardVector;

	/** GreatCircle only: the flight altitude over the datum (FollowTerrainAltitude >= 0 follows the terrain instead). */
	float GreatCircleAltitude = 500.0f;

	/** GreatCircle only: the route as given (latitude, longitude, heading in degrees) for the report. */
	FVector GreatCircleLatLonHeading = FVector::ZeroVector;

	/**
	 * A climb (spherical Phase 6, WM-BP3: climb=lat:lon[:alt], or dir=up): a straight path along local up at the start.
	 * Set by MakeClimb or the dir=up keyword; the report records the route as given (latitude, longitude, start altitude
	 * over the ground; latitude and longitude -999 for dir=up from a start= point). On a heightmap world (WM-MP11) the
	 * site is the ground point's X and Y (bClimbSiteXY), as climb=X:Y[:Alt] gives it.
	 */
	bool bClimb = false;
	FVector ClimbLatLonAltitude = FVector::ZeroVector;
	bool bClimbSiteXY = false;

	/**
	 * A descent (spherical Phase 6, WM-BP10: descend=lat:lon:alt): the climb's path the other way, from StartAltitude over
	 * the ground down along local down (bClimb is set too; the report records the route as path "descend").
	 */
	bool bDescend = false;

	/** Traverse length along the path. Must exceed the initial load radius so new terrain streams.
	 *  <= 0 means the whole route (Route) or one lap (Orbit); a Straight path needs a positive length. */
	float TraverseDistance = 20000.0f;

	/** >= 0: fly this many uu above the analytic surface (UVoxelChunkManager::GetGeneratedSurfaceHeight)
	 *  instead of at a fixed Z; on a planet along local up over UVoxelChunkManager::GetGeneratedSurfacePoint. */
	float FollowTerrainAltitude = -1.0f;

	/** Max time to wait for the initial load to settle before the traverse begins. Raise it to measure
	 *  how long a whole finite world takes to build (the report records the settle time). */
	float WarmupTimeoutSec = 10.0f;

	/** Max time to wait for the queues to drain after the traverse stops. */
	float CatchUpTimeoutSec = 90.0f;

	/** Max time the pawn is held at its start position after the run, waiting for collision under it (G9). */
	float RestoreTimeoutSec = 30.0f;

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
 * completion it releases the benchmark view override and writes the report to Saved/VoxelBench/,
 * then puts the player pawn back where it started and holds it there until there is collision
 * under it (IsDone stays false until the pawn is released).
 *
 * The pawn it flies cannot take damage for the whole run (AActor::SetCanBeDamaged), so a route over
 * the sea cannot drown it (G4, G8). Game damage code must honour AActor::CanBeDamaged for this.
 */
class VOXELSTREAMING_API FVoxelStreamingBenchmark
{
public:
	FVoxelStreamingBenchmark(const FVoxelBenchConfig& InConfig, UVoxelChunkManager* InChunkManager);

	/** Releases a pawn still held (run replaced, or the world shutting down). */
	~FVoxelStreamingBenchmark();

	/** Advance the scenario, drive the streaming origin, and take a sample. */
	void Tick(float DeltaTime);

	/** True once the report is written and the pawn is released. */
	bool IsDone() const { return Phase == EPhase::Done; }

	/** Absolute path of the written CSV once the run finishes (empty until then). */
	const FString& GetReportPath() const { return ReportCsvPath; }

private:
	/** Restore: the report is written; the pawn waits at its start position for collision under it. */
	enum class EPhase : uint8 { Warmup, Traverse, CatchUp, Restore, Done };

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
		float GenLaunchMs, GenPollMs, GenApplyMs, GenStoreMs, GenNotifyMs;
		int32 GenApplyCount;
		float MeshTickMs, MeshLaunchMs, MeshApplyMs;
		float MeshSnapMs, MeshDispMs;
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
		// Far ocean (WM-E1): GT time finding open columns and rebuilding the grid water, rebuilds this tick
		float FarOceanMs;
		int32 FarOceanRebuildN;
		// Memory (MB) and far-compression residency, refreshed every MemorySampleIntervalSec
		float VoxelMB, EditMB, RendCPUMB, RendGPUMB, CollMB, ScatMB, ProcessMB;
		int32 ResidentN, UniformN, CompressedN, EmptyN;
		// LOD evaluation this tick (KI12): candidates found (-1 = no evaluation) and re-meshes queued; and
		// the LOD backlog when the sample was taken (Loaded chunks whose LOD change is not queued, and the
		// subset that should be at LOD 0)
		int32 LODCand, LODQueued, LODStale, LODStaleNear;
		// At scale (spherical Phase 6, WM-BP3): the viewer's altitude over the datum (Z over the origin on a
		// heightmap world); ready chunks per committed LOD (0-3, 4 = 4 and up) and the window's reach, refreshed with
		// the memory sample; mesh batches drawn and skipped since the last sample (every GetDynamicMeshElements call:
		// main views and shadow gathers); chunks generated this tick; the far field's update time and drawn patches
		double Altitude;
		int32 LoadedLOD[5];
		float ReachUU, GroundReachUU;
		int32 BatchesDrawn, BatchesSkipped;
		int32 GenDone;
		float FarFieldMs;
		int32 FarFieldPatches;
		// The far field's memory (MB, CPU and GPU copies, estimated) and patches built this tick (WM-BP9)
		float FarFieldMB;
		int32 FarFieldBuilt;
		// Orbit and back (WM-BP10): the voxel window suspended above the cut-over, the descent prefetch's lead (uu under
		// the camera) and the streaming viewer's altitude over the surface under it
		int32 WindowSuspended;
		float StreamingLeadUU;
		float SurfaceAltitude;
		// Scatter's sub-steps (world modes review G42): edit readers, applying completed extractions, launches, the
		// distance spawn, the renderer's instance flush (ms); extractions applied and launched this tick; HISM instances
		// placed and released since the last sample
		float ScatEditMs, ScatApplyMs, ScatLaunchMs, ScatDistMs, ScatFlushMs;
		int32 ScatApplyN, ScatLaunchN, ScatAddN, ScatRelN;
		// HISM write batches applied and HISMs compacted since the last sample (UVoxelScatterRenderer)
		int32 ScatBatchN, ScatCompactN;
		// The engine's last game thread, render thread and GPU frame times (ms; stat unit's), to place frame time outside
		// the chunk manager
		float GTMs, RTMs, GPUMs;
		// Far-compressed chunks expanded on the game thread since the last sample, and their time (ms; world modes
		// review G65, VoxelResidencyStats)
		int32 DecodeN;
		float DecodeMs;
		// The shared voxel snapshots cached for seam jobs and cooks (MB; not in the voxel systems total; G65)
		float SnapMB;
		// Of which the neighbours' boundary shells for collision cooks (MB; G69)
		float ShellMB;
		// Of BatchesDrawn, the voxel batches drawn into shadow maps (visual polish VP3: voxel.Render.ShadowCullFrustum)
		int32 ShadowBatchesDrawn;
		// Far-field stand-ins over the voxel window since the last sample (visual polish VP4: voxel.World.FarFieldAvoidStandIns)
		int32 FarFieldStandIns;
	};

	void TakeSample(float DeltaTime);
	void SampleMemory();
	/** Position along the configured path at arc length S (before terrain following). */
	FVector EvaluatePath(double S) const;

public:
	/**
	 * Configure a great-circle route (gc=lat:lon:heading[:alt]): the start direction at a latitude and longitude (+Z
	 * north, longitude from +X toward +Y), the heading from north toward east there, a flight altitude over the datum.
	 * @param InOutConfig Config to fill (PathType, the GreatCircle* fields, StartPosition)
	 * @param Center Planet centre
	 * @param Radius Datum radius
	 * @param LatitudeDeg Latitude
	 * @param LongitudeDeg Longitude
	 * @param HeadingDeg Heading (0 = north, 90 = east)
	 * @param Altitude Flight altitude over the datum
	 */
	static void MakeGreatCircle(FVoxelBenchConfig& InOutConfig, const FVector& Center, double Radius, double LatitudeDeg,
		double LongitudeDeg, double HeadingDeg, float Altitude);

	/** A great circle's point at arc length S (at the route's altitude), as EvaluatePath gives it. */
	static FVector EvaluateGreatCircle(const FVoxelBenchConfig& InConfig, double S);

	/**
	 * Configure a climb (spherical Phase 6, WM-BP3: climb=lat:lon[:alt]): a straight path from StartAltitude over the
	 * ground point along its local up (radial from the planet centre), so the altitude grows by the distance flown.
	 * @param InOutConfig Config to fill (PathType Straight, Direction, StartPosition, bClimb)
	 * @param Center Planet centre
	 * @param Ground The ground point the climb starts over (e.g. UVoxelChunkManager::GetGeneratedSurfacePoint)
	 * @param StartAltitude Height of the start over Ground
	 * @param LatLonForReport Latitude and longitude as given, for the report
	 */
	static void MakeClimb(FVoxelBenchConfig& InOutConfig, const FVector& Center, const FVector& Ground, float StartAltitude,
		const FVector2D& LatLonForReport);

	/**
	 * Configure a climb along a given local up (world modes review WM-MP11: climb=X:Y[:Alt] on a heightmap world, where up
	 * is +Z and the report records the ground point's X and Y in place of a latitude and longitude).
	 * @param InOutConfig Config to fill (PathType Straight, Direction, StartPosition, bClimb)
	 * @param Up Local up at the ground point (unit)
	 * @param Ground The ground point the climb starts over
	 * @param StartAltitude Height of the start over Ground
	 * @param SiteForReport Latitude and longitude, or X and Y, as given, for the report
	 */
	static void MakeClimbAlong(FVoxelBenchConfig& InOutConfig, const FVector& Up, const FVector& Ground, float StartAltitude,
		const FVector2D& SiteForReport);

	/**
	 * Configure a descent (spherical Phase 6, WM-BP10: descend=lat:lon:alt): MakeClimb's path flown the other way, from
	 * StartAltitude over the ground point down along its local down, so the altitude falls by the distance flown.
	 * @param InOutConfig Config to fill (PathType Straight, Direction, StartPosition, bClimb and bDescend)
	 * @param Center Planet centre
	 * @param Ground The ground point the descent ends over
	 * @param StartAltitude Height of the start over Ground
	 * @param LatLonForReport Latitude and longitude as given, for the report
	 */
	static void MakeDescent(FVoxelBenchConfig& InOutConfig, const FVector& Center, const FVector& Ground, float StartAltitude,
		const FVector2D& LatLonForReport);

	/** MakeDescent along a given local up (heightmap worlds: descend=X:Y:Alt, WM-MP11). */
	static void MakeDescentAlong(FVoxelBenchConfig& InOutConfig, const FVector& Up, const FVector& Ground, float StartAltitude,
		const FVector2D& SiteForReport);

	/** Unit direction for a latitude and longitude (degrees; +Z north, longitude from +X toward +Y), as gc= and climb= read them. */
	static FVector LatLonToDirection(double LatitudeDeg, double LongitudeDeg);

private:
	/** Apply FollowTerrainAltitude to a path position. */
	FVector ApplyAltitude(const FVector& Pos) const;
	/** Total path length for Route/Orbit (Straight returns TraverseDistance). */
	double GetPathLength() const;
	/** Run metadata (map, config, modes, flags, sizes, path) as a JSON object body. */
	FString BuildRunMetadataJson() const;
	bool QueuesDrained() const;
	void Finish(bool bReachedEquilibrium);
	void WriteReport();

	/** Fly the player pawn along the path (teleport + no gravity, no damage) so it stays on loaded
	 *  terrain, never falls through or drowns, and the rendered view follows the flight. */
	void SetupFlyPawn();
	void UpdateFlyPawn();

	/** After the report: put the pawn back at its start position and hold it there (no gravity) until
	 *  collision under it is ready (G9). Moves to Done when there is no pawn to hold. */
	void BeginRestoreHold();

	/** One tick of the restore hold: keep the collision request alive, release once the ground is there. */
	void TickRestoreHold(float DeltaTime);

	/** End the hold: original movement mode and damage flag back. Logs how the hold ended. */
	void ReleasePawn(bool bGrounded);

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

	// Tracked chunks per layer (Z; radial on a planet since spherical Phase 6), taken with each traverse memory sample:
	// running sum and max (§13.11).
	TMap<int32, int64> LayerLoadedSum;
	TMap<int32, int32> LayerLoadedMax;
	int32 LayerSamples = 0;
	bool bRadialLayers = false;

	// The residency snapshot's per-LOD counts and reach, carried between memory samples (spherical Phase 6, WM-BP3).
	int32 LastLoadedLOD[5] = {};
	float LastReachUU = 0.0f;
	float LastGroundReachUU = 0.0f;

	// Chunks generated when the last sample was taken (the per-tick GenDone column).
	int64 LastTotalGenerated = 0;
	// The far field's stand-ins over the window in all when the last sample was taken (the FarFieldStandInN column, VP4).
	int64 LastFarFieldStandIns = 0;
	// The chunk manager's cut-over transition count when the run started (the report lists the run's own; G49).
	int64 CutoverTransitionsAtStart = 0;
	/** The scatter renderer's instance totals at the last sample (ScatAddN / ScatRelN are the differences) */
	int64 LastScatterAdded = -1;
	int64 LastScatterRemoved = -1;
	int64 LastScatterBatches = -1;
	int64 LastScatterCompactions = -1;

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
	bool bOriginalCanBeDamaged = true;
	bool bPawnConfigured = false;

	// Restore hold (Phase == Restore): time held and the next collision-request nudge.
	float RestoreElapsed = 0.0f;
	float RestoreNextNudge = 0.0f;
};
