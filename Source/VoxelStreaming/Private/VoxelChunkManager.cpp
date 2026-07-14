// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelCaveConfiguration.h"
#include "Algo/BinarySearch.h"
#include "Async/Async.h"
#include "Misc/ScopeExit.h"
#include "VoxelCoordinates.h"
#include "IVoxelLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "IVoxelWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelNoiseTypes.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelGPUDualContourMesher.h"
#include "VoxelGPUMarchingCubesMesher.h"
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelWaterPropagation.h"
#include "VoxelWaterMesher.h"
#include "VoxelCollisionManager.h"
#include "VoxelScatterManager.h"
#include "VoxelTreeInjector.h"
#include "VoxelTreeTypes.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

// P1 LOD-seam diagnostic (see Documentation/LOD_SEAM_INVESTIGATION.md).
// Logs, per meshed chunk at request-assembly time, which boundary faces carry a
// surface crossing and whether the neighbor data for that face is (a) absent
// (-> GetVoxelAt clamps the duplicate edge plane = the confirmed clamp hazard)
// or (b) present but all-Air (-> the plausible allocated-but-ungenerated race).
// This is what turns the two candidate request-assembly hazards into an
// observation on a live torn chunk. Enable with `voxel.LogBoundaryResidency 1`.
//   0 = off
//   1 = log only chunks with at least one at-risk boundary face
//   2 = log every chunk's boundary faces
static TAutoConsoleVariable<int32> CVarLogBoundaryResidency(
	TEXT("voxel.LogBoundaryResidency"),
	0,
	TEXT("Log per-face neighbor-slice residency + all-Air state for each meshed chunk (LOD seam diagnosis)."),
	ECVF_Default);

// Runtime master-enable for GPU terrain generation. Lets you force CPU generation without editing the
// config asset or restarting the build: 1 = honor bUseGPUGeneration; 0 = force CPU. Read at chunk-manager
// Initialize (generator selection), so re-initialize the voxel world after toggling for it to take effect.
static TAutoConsoleVariable<int32> CVarGPUGenerationEnable(
	TEXT("voxel.GPUGeneration.Enable"),
	1,
	TEXT("Master enable for GPU terrain generation. 1 = use bUseGPUGeneration from config; 0 = force CPU generation."),
	ECVF_Default);

// ==================== High-speed traversal: stream throughput overrides ====================
// The walk-speed fall-through is a collision-cook issue (see VoxelCollisionManager); at high speed the
// limiter shifts UPSTREAM to chunk generation — the pawn outruns the load front, so terrain ahead isn't
// even loaded (feetLd=0) and collision can't cook what doesn't exist. These raise the generation/submit
// throughput so the load front keeps up. Adaptive throttling still applies under frame pressure.

static TAutoConsoleVariable<int32> CVarStreamMaxLoadPerFrame(
	TEXT("voxel.Stream.MaxLoadPerFrame"),
	0,
	TEXT("Override MaxChunksToLoadPerFrame (generation dispatch + queue admission + render submit). "
	     "0 = use configuration. Clamped [1,64]. Raise for fast/far traversal."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarStreamMaxAsyncGenTasks(
	TEXT("voxel.Stream.MaxAsyncGenTasks"),
	0,
	TEXT("Override max concurrent async chunk-generation tasks. 0 = use configuration. Clamped [1,16]."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarStreamSpeedAdaptive(
	TEXT("voxel.Stream.SpeedAdaptive"),
	1,
	TEXT("Auto-raise the per-frame load budget with viewer horizontal speed so the load front leads "
	     "during fast traversal (idle stays at the configured budget). 0 = disable. "
	     "Covers realistic fast movement; extreme speeds (~40 m/s+) still need forward-biased generation."),
	ECVF_Default);

// ==================== Far-chunk voxel-data compression ====================
// Settled far chunks keep a resident ~1MB voxel array purely to serve rare events (neighbor-plane
// reads, LOD upgrades, edits, queries). A budgeted sweep compacts idle far chunks; any access
// transparently re-materializes the array via FChunkDescriptor::EnsureResident(). PR B ships the
// uniform tier only (all-air/all-solid chunks collapse to one value); PR C adds the general codec.

static TAutoConsoleVariable<int32> CVarFarCompression(
	TEXT("voxel.FarCompression"),
	1,
	TEXT("Master enable for far-chunk voxel-data compression sweep. 1 = on, 0 = off. "
	     "Access transparently decompresses regardless; this only gates the compress sweep."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionMinStride(
	TEXT("voxel.FarCompression.MinStride"),
	4,
	TEXT("Minimum LOD stride (1<<LODLevel) a chunk must be at to qualify for compression. "
	     "4 = stride-4 far band only (LOD>=2). 2 = also stride-2 (LOD>=1). Clamped to a power of two."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionIdleFrames(
	TEXT("voxel.FarCompression.IdleFrames"),
	300,
	TEXT("Frames a chunk must sit settled (Loaded, unchanged) before it qualifies for compression. "
	     "Primary thrash guard; ~300 = 5s @ 60fps. Keep >= ~180."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionMaxScansPerTick(
	TEXT("voxel.FarCompression.MaxScansPerTick"),
	4,
	TEXT("Budget: max fresh compress attempts per tick (uniform scan + general encode, each ~0.1-1ms). "
	     "Free re-collapses/re-compresses and skips of already-evaluated chunks are not counted against this."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionCodec(
	TEXT("voxel.FarCompression.Codec"),
	3, // Oodle — bake-off winner (best ratio + fastest encode/decode; de-interleave hurts on
	   // run-heavy far chunks because the thin gradient band leaves long AoS tuple runs Oodle handles well)
	TEXT("Encode codec for non-uniform far chunks (EVoxelChunkCodec): 0=uniform-only (no general "
	     "codec), 2=LZ4, 3=Oodle, 4=LZ4Planar, 5=OodlePlanar. Uniform collapse always applies. "
	     "Buffers written by any codec still decode (codec id is stored in the buffer header)."),
	ECVF_Default);

UVoxelChunkManager::UVoxelChunkManager()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

int32 UVoxelChunkManager::ResolveMaxLoadPerFrame() const
{
	const int32 CvarOverride = CVarStreamMaxLoadPerFrame.GetValueOnGameThread();
	const int32 ConfigBase = Configuration ? Configuration->MaxChunksToLoadPerFrame : 8;
	int32 Budget = (CvarOverride > 0) ? CvarOverride : ConfigBase;

	// Speed-adaptive: the faster the viewer moves, the further the load front must lead, so raise the
	// per-frame budget with horizontal speed (~+1 chunk/frame per 128 uu/s). Idle stays at ConfigBase.
	// Realistic fast movement is covered; extreme speeds still need forward-biased generation.
	if (CVarStreamSpeedAdaptive.GetValueOnGameThread() != 0)
	{
		const int32 SpeedBudget = ConfigBase + FMath::CeilToInt(ViewerHorizSpeed / 128.0f);
		Budget = FMath::Max(Budget, SpeedBudget);
	}

	return FMath::Clamp(Budget, 1, 64);
}

void UVoxelChunkManager::BeginPlay()
{
	Super::BeginPlay();
}

void UVoxelChunkManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Shutdown();
	Super::EndPlay(EndPlayReason);
}

void UVoxelChunkManager::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsInitialized || !bStreamingEnabled)
	{
		return;
	}

	++CurrentFrame;

	// Drive an active streaming benchmark: advances the bench view position + samples this frame,
	// before BuildQueryContext picks the position up below. Cleared when the run completes.
	if (ActiveBenchmark.IsValid())
	{
		ActiveBenchmark->Tick(DeltaTime);
		if (ActiveBenchmark->IsDone())
		{
			ActiveBenchmark.Reset();
		}
	}

	const double TickStartTime = FPlatformTime::Seconds();
	FVoxelTimingStats Timing;

	// === Adaptive Throttling: update smoothed frame time ===
	{
		const float FrameTimeMs = DeltaTime * 1000.0f;
		constexpr float Alpha = 0.1f;
		SmoothedFrameTimeMs = SmoothedFrameTimeMs + Alpha * (FrameTimeMs - SmoothedFrameTimeMs);

		const float TargetFPS = Configuration ? Configuration->TargetFrameRate : 60.0f;
		// -VoxelPinScheduler forces non-adaptive so the override/config limits hold for the run.
		const bool bAdaptive = bSchedPinned ? false : (Configuration ? Configuration->bAdaptiveThrottling : true);
		const int32 CvarAsyncGen = CVarStreamMaxAsyncGenTasks.GetValueOnGameThread();
		const int32 ConfigMaxAsyncGen = (CvarAsyncGen > 0) ? FMath::Clamp(CvarAsyncGen, 1, 16)
			: (SchedOverrideAsyncGen >= 0) ? SchedOverrideAsyncGen : (Configuration ? Configuration->MaxAsyncGenerationTasks : 2);
		const int32 ConfigMaxAsync = (SchedOverrideAsyncMesh >= 0) ? SchedOverrideAsyncMesh : (Configuration ? Configuration->MaxAsyncMeshTasks : 4);
		const int32 ConfigMaxLODRemesh = (SchedOverrideLODRemesh >= 0) ? SchedOverrideLODRemesh : (Configuration ? Configuration->MaxLODRemeshPerFrame : 4);
		const int32 ConfigMaxPending = (SchedOverridePending >= 0) ? SchedOverridePending : (Configuration ? Configuration->MaxPendingMeshes : 4);

		if (bAdaptive && TargetFPS > 0.0f)
		{
			const float TargetMs = 1000.0f / TargetFPS;
			if (SmoothedFrameTimeMs > TargetMs * 1.2f)
			{
				// Over budget: halve effective limits (min 1)
				EffectiveMaxAsyncGenerationTasks = FMath::Max(1, ConfigMaxAsyncGen / 2);
				EffectiveMaxAsyncMeshTasks = FMath::Max(1, ConfigMaxAsync / 2);
				EffectiveMaxLODRemeshPerFrame = FMath::Max(1, ConfigMaxLODRemesh / 2);
				EffectiveMaxPendingMeshes = FMath::Max(2, ConfigMaxPending / 2);
			}
			else if (SmoothedFrameTimeMs < TargetMs * 0.8f)
			{
				// Under budget: restore configured limits
				EffectiveMaxAsyncGenerationTasks = ConfigMaxAsyncGen;
				EffectiveMaxAsyncMeshTasks = ConfigMaxAsync;
				EffectiveMaxLODRemeshPerFrame = ConfigMaxLODRemesh;
				EffectiveMaxPendingMeshes = ConfigMaxPending;
			}
			// else: in the 80-120% band, keep current values
		}
		else
		{
			EffectiveMaxAsyncGenerationTasks = ConfigMaxAsyncGen;
			EffectiveMaxAsyncMeshTasks = ConfigMaxAsync;
			EffectiveMaxLODRemeshPerFrame = ConfigMaxLODRemesh;
			EffectiveMaxPendingMeshes = ConfigMaxPending;
		}
	}

	// Build query context from camera state
	FLODQueryContext Context = BuildQueryContext();

	// Cache the current viewer position for the stale-cull distance test used by the queue
	// processors (which don't build their own context). Reflects the benchmark view override.
	CurrentViewerPosition = Context.ViewerPosition;

	// Track viewer HORIZONTAL speed for the speed-adaptive load budget (ResolveMaxLoadPerFrame).
	// Smoothed so a teleport spike doesn't blow the budget out for a frame.
	if (DeltaTime > SMALL_NUMBER && LastViewerPosForSpeed.X != FLT_MAX)
	{
		const FVector ViewerDelta = CurrentViewerPosition - LastViewerPosForSpeed;
		const float InstHorizSpeed = FVector2D(ViewerDelta.X, ViewerDelta.Y).Size() / DeltaTime;
		ViewerHorizSpeed = FMath::FInterpTo(ViewerHorizSpeed, InstHorizSpeed, DeltaTime, 4.0f);
	}
	LastViewerPosForSpeed = CurrentViewerPosition;

	// Debug: Log viewer position periodically
	static int32 ViewerLogCounter = 0;
	if (++ViewerLogCounter % 120 == 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Viewer position: (%.0f, %.0f, %.0f)"),
			Context.ViewerPosition.X, Context.ViewerPosition.Y, Context.ViewerPosition.Z);
	}

	// Calculate current viewer chunk coordinate
	const FIntVector CurrentViewerChunk = WorldToChunkCoord(Context.ViewerPosition);

	// Determine if we need to update streaming decisions
	const bool bViewerChunkChanged = (CurrentViewerChunk != CachedViewerChunk);
	const bool bNeedStreamingUpdate = bForceStreamingUpdate || bViewerChunkChanged;

	// Update LOD strategy (always update for morph factor interpolation)
	if (LODStrategy)
	{
		LODStrategy->Update(Context, DeltaTime);
	}

	// === Streaming decisions ===
	double SectionStart = FPlatformTime::Seconds();
	if (bNeedStreamingUpdate)
	{
		const bool bWasForced = bForceStreamingUpdate;
		bForceStreamingUpdate = false;

		UpdateLoadDecisions(Context);
		CachedViewerChunk = CurrentViewerChunk;
		LastStreamingUpdatePosition = Context.ViewerPosition;

		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Streaming update: ViewerChunk=(%d,%d,%d), WasForced=%s, ContinueNextFrame=%s"),
			CurrentViewerChunk.X, CurrentViewerChunk.Y, CurrentViewerChunk.Z,
			bWasForced ? TEXT("Yes") : TEXT("No"),
			bForceStreamingUpdate ? TEXT("Yes") : TEXT("No"));
	}

	// ALWAYS update UNLOAD decisions (cheap operation, prevents orphaned chunks)
	UpdateUnloadDecisions(Context);

	// Re-prioritize queues when viewer moves to a new chunk
	// This ensures closest chunks are always processed first during fast movement
	// Also updates LOD levels for queued items so they mesh at the correct LOD
	if (bViewerChunkChanged)
	{
		ReprioritizeQueues(Context);
	}
	Timing.StreamingMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Generation queue (async launch + completed result processing) ===
	// Sub-timed per function so the benchmark CSV can attribute generation-phase cost.
	SectionStart = FPlatformTime::Seconds();
	NeighborRemeshSecondsThisTick = 0.0;
	const float TimeSlice = Configuration ? Configuration->StreamingTimeSliceMS : 2.0f;
	double SubStart = SectionStart;
	ProcessGenerationQueue(TimeSlice * 0.4f);
	double SubEnd = FPlatformTime::Seconds();
	Timing.GenLaunchMs = static_cast<float>((SubEnd - SubStart) * 1000.0);
	SubStart = SubEnd;
	ProcessPendingGPUReadbacks();
	SubEnd = FPlatformTime::Seconds();
	Timing.GenPollMs = static_cast<float>((SubEnd - SubStart) * 1000.0);
	SubStart = SubEnd;
	ProcessCompletedAsyncGenerations(Timing);
	SubEnd = FPlatformTime::Seconds();
	Timing.GenApplyMs = static_cast<float>((SubEnd - SubStart) * 1000.0);
	Timing.GenNeighborMs = static_cast<float>(NeighborRemeshSecondsThisTick * 1000.0);
	Timing.GenerationMs = static_cast<float>((SubEnd - SectionStart) * 1000.0);

	// === Meshing queue ===
	// Sub-timed per function so the benchmark CSV can attribute meshing-phase cost.
	SectionStart = FPlatformTime::Seconds();
	SubStart = SectionStart;
	if (Mesher)
	{
		Mesher->Tick(DeltaTime);
	}
	SubEnd = FPlatformTime::Seconds();
	Timing.MeshTickMs = static_cast<float>((SubEnd - SubStart) * 1000.0);
	SubStart = SubEnd;
	ProcessMeshingQueue(TimeSlice * 0.4f, Timing);
	SubEnd = FPlatformTime::Seconds();
	Timing.MeshLaunchMs = static_cast<float>((SubEnd - SubStart) * 1000.0);
	SubStart = SubEnd;
	ProcessCompletedAsyncMeshes();
	SubEnd = FPlatformTime::Seconds();
	Timing.MeshApplyMs = static_cast<float>((SubEnd - SubStart) * 1000.0);
	Timing.MeshingMs = static_cast<float>((SubEnd - SectionStart) * 1000.0);

	// === Render submit (time-budgeted; sub-timed for the benchmark CSV) ===
	SectionStart = FPlatformTime::Seconds();
	SubmitRendererSecondsThisTick = 0.0;
	SubmitScatterSecondsThisTick = 0.0;
	SubmitWaterSecondsThisTick = 0.0;
	SubmitsThisTick = 0;
	const int32 MaxRenderSubmitsPerFrame = ResolveMaxLoadPerFrame();
	constexpr double RenderSubmitBudgetSec = 0.004; // 4ms budget — leaves headroom for other systems
	if (PendingMeshQueue.Num() > 0)
	{
		int32 RenderSubmitCount = 0;
		while (PendingMeshQueue.Num() > 0 && RenderSubmitCount < MaxRenderSubmitsPerFrame)
		{
			const FIntVector ChunkCoord = PendingMeshQueue.Last().ChunkCoord;
			OnChunkMeshingComplete(ChunkCoord);
			++RenderSubmitCount;

			// Stop early if we've exhausted the frame budget for render submits.
			// Remaining chunks stay in PendingMeshQueue and process next frame.
			if ((FPlatformTime::Seconds() - SectionStart) >= RenderSubmitBudgetSec)
			{
				break;
			}
		}
		SubmitsThisTick = RenderSubmitCount;
	}
	SubEnd = FPlatformTime::Seconds();
	Timing.RenderMeshMs = static_cast<float>((SubEnd - SectionStart) * 1000.0);
	Timing.RenderSubRendererMs = static_cast<float>(SubmitRendererSecondsThisTick * 1000.0);
	Timing.RenderSubScatterMs = static_cast<float>(SubmitScatterSecondsThisTick * 1000.0);
	Timing.RenderSubWaterMs = static_cast<float>(SubmitWaterSecondsThisTick * 1000.0);
	Timing.RenderSubmitCount = SubmitsThisTick;

	SubStart = SubEnd;
	const int32 MaxUnloadsPerFrame = Configuration ? Configuration->MaxChunksToUnloadPerFrame : 8;
	ProcessUnloadQueue(MaxUnloadsPerFrame);
	SubEnd = FPlatformTime::Seconds();
	Timing.RenderUnloadMs = static_cast<float>((SubEnd - SubStart) * 1000.0);

	// === Process dirty water tiles (2D water grid) ===
	SubStart = SubEnd;
	if (Configuration && Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial)
	{
		ProcessDirtyWaterTiles(8);
	}
	SubEnd = FPlatformTime::Seconds();
	Timing.RenderWaterTileMs = static_cast<float>((SubEnd - SubStart) * 1000.0);

	Timing.RenderSubmitMs = static_cast<float>((SubEnd - SectionStart) * 1000.0);

	// === Water propagation (bounded BFS per frame) ===
	if (WaterPropagation && WaterPropagation->HasPendingWork())
	{
		WaterPropagation->ProcessPropagation(512);
	}

	// === LOD level changes and morph factor updates ===
	SectionStart = FPlatformTime::Seconds();
	{
		// Detect when all queues drain — signal that a LOD sweep is needed
		const bool bQueuesEmpty = GenerationQueue.Num() == 0
			&& AsyncGenerationInProgress.Num() == 0
			&& MeshingQueue.Num() == 0
			&& AsyncMeshingInProgress.Num() == 0
			&& PendingMeshQueue.Num() == 0;

		if (bQueuesEmpty && !bPendingLODSweep)
		{
			bPendingLODSweep = true;
		}

		// LOD level evaluation: runs on viewer chunk change OR pending sweep
		if (bViewerChunkChanged || bPendingLODSweep)
		{
			EvaluateLODLevelChanges(Context);

			// Clear sweep flag only if queues are still empty after evaluation
			// (no new work was generated)
			if (bPendingLODSweep && bQueuesEmpty && MeshingQueue.Num() == 0)
			{
				bPendingLODSweep = false;
			}
		}

		// Morph factor updates: gated by movement threshold
		const float PositionDeltaSq = FVector::DistSquared(Context.ViewerPosition, LastLODUpdatePosition);
		if (bViewerChunkChanged || PositionDeltaSq > LODUpdateThresholdSq)
		{
			UpdateLODMorphFactors(Context);
			LastLODUpdatePosition = Context.ViewerPosition;
		}
	}
	Timing.LODMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Far-chunk voxel-data compression sweep ===
	// Runs after LOD evaluation so a chunk just queued for an LOD remesh (now dirty) is excluded.
	ProcessFarCompressionSweep();

	// === Subsystem deferral check ===
	const int32 DeferThreshold = Configuration ? Configuration->DeferSubsystemsThreshold : 20;
	bSubsystemsDeferred = (DeferThreshold > 0 && GenerationQueue.Num() > DeferThreshold);

	// === Collision manager ===
	// Always update collision (drains async results, creates physics bodies).
	// Skipping Update() when deferred blocks the async pipeline and prevents
	// collision from ever being generated during initial load.
	// New task launches are throttled internally by MaxAsyncCollisionTasks.
	SectionStart = FPlatformTime::Seconds();
	if (CollisionManager && Configuration && Configuration->bGenerateCollision)
	{
		CollisionManager->Update(Context.ViewerPosition, DeltaTime);
	}
	Timing.CollisionMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Scatter manager ===
	// Always update scatter (drains async results, updates HISM instances).
	// New task launches are throttled internally; skipping Update() entirely
	// when deferred causes async results to pile up and blocks new generations
	// once the generation queue drains.
	SectionStart = FPlatformTime::Seconds();
	if (ScatterManager && Configuration && Configuration->bEnableScatter)
	{
		ScatterManager->Update(Context.ViewerPosition, DeltaTime);

		if (Configuration->bScatterDebugVisualization)
		{
			ScatterManager->DrawDebugVisualization(GetWorld());
		}
	}
	Timing.ScatterMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// Flush all pending render operations as a single batched command
	if (MeshRenderer)
	{
		const double FlushStart = FPlatformTime::Seconds();
		MeshRenderer->FlushPendingOperations();
		Timing.RenderFlushMs = static_cast<float>((FPlatformTime::Seconds() - FlushStart) * 1000.0);
	}
	Timing.TotalMs = static_cast<float>((FPlatformTime::Seconds() - TickStartTime) * 1000.0);
	LastTimingStats = Timing;
}

// ==================== Initialization ====================

void UVoxelChunkManager::Initialize(
	UVoxelWorldConfiguration* InConfig,
	IVoxelLODStrategy* InLODStrategy,
	IVoxelMeshRenderer* InRenderer)
{
	if (bIsInitialized)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("ChunkManager::Initialize called when already initialized"));
		Shutdown();
	}

	if (!InConfig)
	{
		UE_LOG(LogVoxelStreaming, Error, TEXT("ChunkManager::Initialize called with null configuration"));
		return;
	}

	Configuration = InConfig;
	LODStrategy = InLODStrategy;
	MeshRenderer = InRenderer;

	// Benchmark scheduler overrides (command line) — pin specific concurrency limits for A/B runs.
	// Absent keys leave the members at -1 (use the configuration value).
	FParse::Value(FCommandLine::Get(), TEXT("VoxelMaxAsyncGen="), SchedOverrideAsyncGen);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelMaxAsyncMesh="), SchedOverrideAsyncMesh);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelMaxLODRemesh="), SchedOverrideLODRemesh);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelMaxPending="), SchedOverridePending);
	bSchedPinned = FParse::Param(FCommandLine::Get(), TEXT("VoxelPinScheduler"));
	if (SchedOverrideAsyncGen >= 0 || SchedOverrideAsyncMesh >= 0 || SchedOverrideLODRemesh >= 0 || SchedOverridePending >= 0 || bSchedPinned)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelSched override: gen=%d mesh=%d lodRemesh=%d pending=%d pinned=%d"),
			SchedOverrideAsyncGen, SchedOverrideAsyncMesh, SchedOverrideLODRemesh, SchedOverridePending, bSchedPinned ? 1 : 0);
	}

	// Deep neighbour-data depth override (per-job-cost A/B).
	bDeepDepthOff = FParse::Param(FCommandLine::Get(), TEXT("VoxelDeepOff"));
	bDeepDepthFull = FParse::Param(FCommandLine::Get(), TEXT("VoxelDeepFull"));
	if (bDeepDepthOff || bDeepDepthFull)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelDeepDepth override: %s (default is GEO stride+1)"),
			bDeepDepthOff ? TEXT("OFF (1 plane)") : TEXT("FULL (2*stride)"));
	}

	// Stale-cull (default on): skip meshing chunks the viewer has already moved past.
	bStaleCull = !FParse::Param(FCommandLine::Get(), TEXT("VoxelNoStaleCull"));
	if (!bStaleCull)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("Stale-cull DISABLED (-VoxelNoStaleCull): meshing chunks the viewer has passed"));
	}

	// Pass water material to renderer for per-chunk water mesh sections
	if (MeshRenderer && Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial)
	{
		MeshRenderer->SetWaterMaterial(Configuration->WaterMeshMaterial);
	}

	// Disable LOD morphing for cubic mode - vertices cannot interpolate with hard edges
	// LOD bands and levels still apply (for distance-based loading and potential stride use)
	if (Configuration->MeshingMode == EMeshingMode::Cubic && Configuration->bEnableLODMorphing)
	{
		Configuration->bEnableLODMorphing = false;
		UE_LOG(LogVoxelStreaming, Log, TEXT("LOD morphing disabled for cubic mode (hard-edged vertices cannot interpolate)"));
	}

	// Initialize LOD strategy
	if (LODStrategy)
	{
		LODStrategy->Initialize(Configuration);
	}

	// Clear any existing state
	ChunkStates.Empty();
	LoadedChunkCoords.Empty();
	GenerationQueue.Empty();
	GenerationQueueSet.Empty();
	MeshingQueue.Empty();
	MeshingQueueSet.Empty();
	UnloadQueue.Empty();
	UnloadQueueSet.Empty();

	// Reset statistics
	TotalChunksGenerated = 0;
	TotalChunksMeshed = 0;
	TotalChunksUnloaded = 0;
	CurrentFrame = 0;

	// Reset streaming decision caching (sentinel values force update on first tick)
	CachedViewerChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
	LastStreamingUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	LastLODUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	bForceStreamingUpdate = false;
	bPendingLODSweep = false;

	// Reset adaptive throttle state
	SmoothedFrameTimeMs = 16.67f;
	bSubsystemsDeferred = false;
	EffectiveMaxAsyncGenerationTasks = (SchedOverrideAsyncGen >= 0) ? SchedOverrideAsyncGen : Configuration->MaxAsyncGenerationTasks;
	EffectiveMaxAsyncMeshTasks = (SchedOverrideAsyncMesh >= 0) ? SchedOverrideAsyncMesh : Configuration->MaxAsyncMeshTasks;
	EffectiveMaxLODRemeshPerFrame = (SchedOverrideLODRemesh >= 0) ? SchedOverrideLODRemesh : Configuration->MaxLODRemeshPerFrame;
	EffectiveMaxPendingMeshes = (SchedOverridePending >= 0) ? SchedOverridePending : Configuration->MaxPendingMeshes;
	LastTimingStats = FVoxelTimingStats();

	// Create generation components
	FWorldModeTerrainParams TerrainParams;
	TerrainParams.SeaLevel = Configuration->SeaLevel;
	TerrainParams.HeightScale = Configuration->HeightScale;
	TerrainParams.BaseHeight = Configuration->BaseHeight;
	// Build the analytic world mode matching config->WorldMode so spawn / nav / POI / GetGeneratedSurfaceHeight
	// use the correct terrain math for every mode (was always InfinitePlane — flat for island/planet worlds).
	switch (Configuration->WorldMode)
	{
	case EWorldMode::IslandBowl:
	{
		FIslandBowlParams IslandParams;
		IslandParams.Shape = static_cast<EIslandShape>(Configuration->IslandShape);
		IslandParams.IslandRadius = Configuration->IslandRadius;
		IslandParams.SizeY = Configuration->IslandSizeY;
		IslandParams.FalloffWidth = Configuration->IslandFalloffWidth;
		IslandParams.FalloffType = static_cast<EIslandFalloffType>(Configuration->IslandFalloffType);
		IslandParams.CenterX = Configuration->IslandCenterX;
		IslandParams.CenterY = Configuration->IslandCenterY;
		IslandParams.EdgeHeight = Configuration->IslandEdgeHeight;
		IslandParams.bBowlShape = Configuration->bIslandBowlShape;
		WorldMode = MakeUnique<FIslandBowlWorldMode>(TerrainParams, IslandParams);
		break;
	}
	case EWorldMode::SphericalPlanet:
	{
		FWorldModeTerrainParams PlanetTerrainParams(0.0f, Configuration->PlanetHeightScale, Configuration->BaseHeight);
		FSphericalPlanetParams PlanetParams;
		PlanetParams.PlanetRadius = Configuration->WorldRadius;
		PlanetParams.MaxTerrainHeight = Configuration->PlanetMaxTerrainHeight;
		PlanetParams.MaxTerrainDepth = Configuration->PlanetMaxTerrainDepth;
		PlanetParams.PlanetCenter = Configuration->WorldOrigin;
		WorldMode = MakeUnique<FSphericalPlanetWorldMode>(PlanetTerrainParams, PlanetParams);
		break;
	}
	case EWorldMode::InfinitePlane:
	default:
		WorldMode = MakeUnique<FInfinitePlaneWorldMode>(TerrainParams);
		break;
	}

	// Give the world mode the biome config so the analytic height query (GetTerrainHeightAt) applies the same
	// continentalness modulation as generation (InfinitePlane + IslandBowl; other modes no-op) — spawn / nav /
	// POI heights match the real surface.
	WorldMode->SetBiomeContext(Configuration->BiomeConfiguration);

	// -VoxelForceCPU (headless/benchmark, e.g. under -nullrhi where GPU compute can't dispatch) forces
	// BOTH the CPU generator and the CPU mesher. Evaluated once here and reused for the mesher below.
	const bool bForceCPU = FParse::Param(FCommandLine::Get(), TEXT("VoxelForceCPU"));

	// Select the voxel data generator. bUseGPUGeneration was historically a dead flag — this is where it
	// becomes real. The runtime cvar voxel.GPUGeneration.Enable can force CPU without an editor restart.
	const bool bUseGPUGen = Configuration->bUseGPUGeneration && !bForceCPU
		&& CVarGPUGenerationEnable.GetValueOnGameThread() != 0;
	if (bUseGPUGen)
	{
		TUniquePtr<FVoxelGPUNoiseGenerator> GPUGen = MakeUnique<FVoxelGPUNoiseGenerator>();
		GPUGen->Initialize();
		GPUGeneratorPtr = GPUGen.Get();
		NoiseGenerator = MoveTemp(GPUGen);
		bUseGPUGenerationActive = true;
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using GPU noise generator (bUseGPUGeneration=true)"));
	}
	else
	{
		NoiseGenerator = MakeUnique<FVoxelCPUNoiseGenerator>();
		NoiseGenerator->Initialize();
		GPUGeneratorPtr = nullptr;
		bUseGPUGenerationActive = false;
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using CPU noise generator (bUseGPUGeneration=%s, ForceCPU=%s)"),
			Configuration->bUseGPUGeneration ? TEXT("true") : TEXT("false"),
			bForceCPU ? TEXT("true") : TEXT("false"));
	}
	const bool bUseGPU = Configuration->bUseGPUMeshing && !bForceCPU;
	if (bForceCPU)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelForceCPU: forcing CPU generation + CPU mesher (GPU disabled)"));
	}

	// Create mesher based on configuration
	if (Configuration->MeshingMode == EMeshingMode::MarchingCubes)
	{
		FVoxelMeshingConfig MeshConfig;
		MeshConfig.bUseSmoothMeshing = true;
		MeshConfig.IsoLevel = 0.5f;
		MeshConfig.bCalculateAO = Configuration->bCalculateAO;
		MeshConfig.UVScale = Configuration->UVScale;

		// Marching Cubes is triangle-soup (3 verts/tri, no vertex sharing) — a chunk needs far more
		// vertices than Dual Contouring (~1 vert/cell). Scale the GPU output-buffer budget with chunk
		// volume so cave-dense chunks (lots of interior surface) fit at large chunk sizes (e.g. 64^3)
		// instead of overflowing the default 65536 cap and vanishing (the reported invisible-cave-chunk
		// bug). 32^3 stays at 65536; 64^3 -> 524288. The shader's overflow guard still truncates any
		// pathological chunk safely (visible, no corruption). Soup => index count == vertex count.
		const int64 ChunkCells = (int64)Configuration->ChunkSize * Configuration->ChunkSize * Configuration->ChunkSize;
		MeshConfig.MaxVerticesPerChunk = (uint32)FMath::Clamp<int64>(ChunkCells * 2, 65536, 1048576);
		MeshConfig.MaxIndicesPerChunk = MeshConfig.MaxVerticesPerChunk;

		// Disable LOD seam handling if configured
		if (!Configuration->bEnableLODSeams)
		{
			MeshConfig.bUseTransvoxel = false;
			MeshConfig.bGenerateSkirts = false;
		}

		if (bUseGPU)
		{
			auto GPUMCMesher = MakeUnique<FVoxelGPUMarchingCubesMesher>();
			GPUMCMesher->Initialize();
			GPUMCMesher->SetConfig(MeshConfig);
			Mesher = MoveTemp(GPUMCMesher);
			UE_LOG(LogVoxelStreaming, Log, TEXT("Using GPU MarchingCubes mesher (AO=%s, UVScale=%.2f)"),
				Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
				Configuration->UVScale);
		}
		else
		{
			auto CPUMCMesher = MakeUnique<FVoxelCPUMarchingCubesMesher>();
			CPUMCMesher->Initialize();
			CPUMCMesher->SetConfig(MeshConfig);
			Mesher = MoveTemp(CPUMCMesher);
			UE_LOG(LogVoxelStreaming, Log, TEXT("Using CPU MarchingCubes mesher (AO=%s, UVScale=%.2f)"),
				Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
				Configuration->UVScale);
		}
	}
	else if (Configuration->MeshingMode == EMeshingMode::DualContouring)
	{
		FVoxelMeshingConfig MeshConfig;
		MeshConfig.bUseSmoothMeshing = true;
		MeshConfig.IsoLevel = 0.5f;
		MeshConfig.bCalculateAO = Configuration->bCalculateAO;
		MeshConfig.UVScale = Configuration->UVScale;

		// DC uses its own LOD boundary merging, not Transvoxel transition cells
		MeshConfig.bUseTransvoxel = false;
		MeshConfig.bGenerateSkirts = Configuration->bEnableLODSeams;

		if (bUseGPU)
		{
			auto GPUDCMesher = MakeUnique<FVoxelGPUDualContourMesher>();
			GPUDCMesher->Initialize();
			GPUDCMesher->SetConfig(MeshConfig);
			Mesher = MoveTemp(GPUDCMesher);
			UE_LOG(LogVoxelStreaming, Log, TEXT("Using GPU Dual Contouring mesher (AO=%s, UVScale=%.2f)"),
				Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
				Configuration->UVScale);
		}
		else
		{
			auto DCMesher = MakeUnique<FVoxelCPUDualContourMesher>();
			DCMesher->Initialize();
			DCMesher->SetConfig(MeshConfig);
			Mesher = MoveTemp(DCMesher);
			UE_LOG(LogVoxelStreaming, Log, TEXT("Using CPU Dual Contouring mesher (AO=%s, UVScale=%.2f)"),
				Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
				Configuration->UVScale);
		}
	}
	else
	{
		auto CubicMesher = MakeUnique<FVoxelCPUCubicMesher>();
		CubicMesher->Initialize();

		// Configure cubic meshing parameters from world config
		FVoxelMeshingConfig MeshConfig = CubicMesher->GetConfig();
		MeshConfig.bUseGreedyMeshing = Configuration->bUseGreedyMeshing;
		MeshConfig.bCalculateAO = Configuration->bCalculateAO;
		MeshConfig.UVScale = Configuration->UVScale;
		CubicMesher->SetConfig(MeshConfig);

		Mesher = MoveTemp(CubicMesher);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using Cubic mesher (Greedy=%s, AO=%s, UVScale=%.2f)"),
			Configuration->bUseGreedyMeshing ? TEXT("true") : TEXT("false"),
			Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
			Configuration->UVScale);
	}

	// Clear pending mesh queue
	PendingMeshQueue.Empty();

	// Create edit manager
	EditManager = NewObject<UVoxelEditManager>(this);
	EditManager->Initialize(Configuration);

	// Subscribe to edit events - mark chunks dirty when edited
	EditManager->OnChunkEdited.AddLambda([this](const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius)
	{
		// Mark the edited chunk dirty
		MarkChunkDirty(ChunkCoord);
		if (CollisionManager)
		{
			CollisionManager->MarkChunkDirty(ChunkCoord);
		}
		if (ScatterManager)
		{
			// Handle scatter based on edit source
			if (Source == EEditSource::Player && EditRadius > 0.0f)
			{
				// Player edits: surgically remove scatter in the affected radius immediately, so it
				// disappears the same frame as the dig/build. Pad by half a VoxelSize so scatter on block
				// faces above/around the edit center is also cleared (block-face-snapped scatter sits at
				// the face center, 0.5*VoxelSize from the block center).
				//
				// A live player edit ONLY clears — it never re-extracts, so foliage never regrows or
				// reshuffles under the player while they edit. Correctness against the new terrain is
				// deferred to the chunk's next fresh generation (player leaves and returns / chunk reloads),
				// which reads edit-merged voxels at the scatter hand-off (see OnChunkMeshCompleted), so
				// regrown foliage lands on the edited surface instead of floating over holes.
				const float ScatterClearRadius = EditRadius + Configuration->VoxelSize * 0.6f;
				ScatterManager->ClearScatterInRadius(EditCenter, ScatterClearRadius);
			}
			else if (Source != EEditSource::Player)
			{
				// System / Editor edits (e.g. POI terraforming) re-extract the chunk so scatter matches the
				// new surface. RegenerateChunkScatter keeps the current instances rendered until the
				// re-extraction smoothly replaces them per type, so it adds no flash.
				//
				// TODO(scatter-claims): a POI often wants foliage SUPPRESSED — either across its whole
				// claimed zone or just under a static-asset (building) footprint — rather than re-extracted.
				// Drive that from the claim covering the region once VoxelScatter consumes claims.
				ScatterManager->RegenerateChunkScatter(ChunkCoord);
			}
			// Zero-radius player undo/redo needs no scatter handling — the targeted removal already
			// happened at original-edit time.
		}

		// Notify water propagation system of the edit
		if (WaterPropagation)
		{
			WaterPropagation->OnChunkEdited(ChunkCoord, Source, EditCenter, EditRadius);
		}

		// Also mark neighboring chunks dirty so they re-extract boundary data
		// This ensures seamless edits across chunk borders
		static const FIntVector NeighborOffsets[6] = {
			FIntVector(-1, 0, 0), FIntVector(1, 0, 0),
			FIntVector(0, -1, 0), FIntVector(0, 1, 0),
			FIntVector(0, 0, -1), FIntVector(0, 0, 1)
		};

		for (const FIntVector& Offset : NeighborOffsets)
		{
			const FIntVector NeighborCoord = ChunkCoord + Offset;
			if (ChunkStates.Contains(NeighborCoord))
			{
				MarkChunkDirty(NeighborCoord);
				if (CollisionManager)
				{
					CollisionManager->MarkChunkDirty(NeighborCoord);
				}
				// Note: Scatter for neighbors is already handled by ClearScatterInRadius
				// which affects all chunks within the edit radius, not just the primary chunk
				if (ScatterManager && Source != EEditSource::Player)
				{
					ScatterManager->RegenerateChunkScatter(NeighborCoord);
				}
			}
		}
	});

	UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelEditManager created and initialized"));

	// Create collision manager if enabled
	if (Configuration->bGenerateCollision)
	{
		CollisionManager = NewObject<UVoxelCollisionManager>(this);
		CollisionManager->Initialize(Configuration, this);
		CollisionManager->SetCollisionRadius(Configuration->ViewDistance * 0.5f);
		CollisionManager->SetCollisionLODLevel(Configuration->CollisionLODLevel);

		CollisionManager->SetMaxAsyncCollisionTasks(Configuration->MaxAsyncCollisionTasks);

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelCollisionManager created (Radius=%.0f, LOD=%d, MaxAsyncTasks=%d)"),
			Configuration->ViewDistance * 0.5f, Configuration->CollisionLODLevel, Configuration->MaxAsyncCollisionTasks);
	}

	// Create scatter manager if enabled
	if (Configuration->bEnableScatter)
	{
		ScatterManager = NewObject<UVoxelScatterManager>(this);
		ScatterManager->Initialize(Configuration, GetWorld());
		ScatterManager->SetScatterRadius(Configuration->ScatterRadius);
		ScatterManager->SetWorldSeed(static_cast<uint32>(Configuration->WorldSeed));
		// Give scatter the analytic world mode so surface extraction can classify points covered by
		// terrain in a neighboring chunk (cross-chunk cave floors / overhangs) as underground.
		ScatterManager->SetWorldMode(WorldMode.Get());

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelScatterManager created (Radius=%.0f)"),
			Configuration->ScatterRadius);
	}

	// Create water propagation system if water is enabled
	if (Configuration->bEnableWaterLevel)
	{
		WaterPropagation = NewObject<UVoxelWaterPropagation>(this);
		WaterPropagation->Initialize(this, EditManager, Configuration->WaterLevel);

		UE_LOG(LogVoxelStreaming, Log, TEXT("VoxelWaterPropagation created (WaterLevel=%.0f)"),
			Configuration->WaterLevel);
	}

	bIsInitialized = true;

	// Dump biome configuration for diagnostics
	if (Configuration->BiomeConfiguration)
	{
		Configuration->BiomeConfiguration->LogConfiguration();
	}

	UE_LOG(LogVoxelStreaming, Log, TEXT("ChunkManager initialized with config: VoxelSize=%.1f, ChunkSize=%d"),
		Configuration->VoxelSize, Configuration->ChunkSize);
}

void UVoxelChunkManager::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Clear water tile state
	WaterTiles.Empty();
	DirtyWaterTileQueue.Empty();
	DirtyWaterTileSet.Empty();

	// Clear all chunks and water tiles from renderer
	if (MeshRenderer)
	{
		MeshRenderer->ClearAllWaterTiles();
		MeshRenderer->ClearAllChunks();
	}

	// Clear state
	ChunkStates.Empty();
	LoadedChunkCoords.Empty();
	GenerationQueue.Empty();
	GenerationQueueSet.Empty();
	MeshingQueue.Empty();
	MeshingQueueSet.Empty();
	UnloadQueue.Empty();
	UnloadQueueSet.Empty();

	// Clear async generation state
	// Note: In-flight async tasks will safely no-op due to weak pointer check
	AsyncGenerationInProgress.Empty();
	{
		FAsyncGenerationResult DiscardedGenResult;
		while (CompletedGenerationQueue.Dequeue(DiscardedGenResult)) {}
	}

	// Clear async meshing state
	AsyncMeshingInProgress.Empty();
	PendingMeshQueue.Empty();
	{
		FAsyncMeshResult DiscardedMeshResult;
		while (CompletedMeshQueue.Dequeue(DiscardedMeshResult)) {}
	}

	// Reset streaming decision caching
	CachedViewerChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
	LastStreamingUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	LastLODUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	bForceStreamingUpdate = false;
	bPendingLODSweep = false;
	bSubsystemsDeferred = false;

	// Clean up LOD strategy (we own it)
	if (LODStrategy)
	{
		delete LODStrategy;
		LODStrategy = nullptr;
	}

	// Shutdown and cleanup generation components
	if (Mesher)
	{
		Mesher->Shutdown();
		Mesher.Reset();
	}

	// Drop any in-flight GPU readbacks before the generator's Shutdown (which flushes rendering commands
	// and frees the readback staging buffers). Clearing the map here only releases the tracking handles.
	PendingGPUReadbacks.Empty();
	GPUGeneratorPtr = nullptr;
	bUseGPUGenerationActive = false;

	if (NoiseGenerator)
	{
		NoiseGenerator->Shutdown();
		NoiseGenerator.Reset();
	}

	WorldMode.Reset();

	// Clear pending mesh queue
	PendingMeshQueue.Empty();

	// Shutdown collision manager
	if (CollisionManager)
	{
		CollisionManager->Shutdown();
		CollisionManager = nullptr;
	}

	// Shutdown scatter manager
	if (ScatterManager)
	{
		ScatterManager->Shutdown();
		ScatterManager = nullptr;
	}

	// Shutdown water propagation (before edit manager since it depends on it)
	WaterPropagation = nullptr;

	// Shutdown edit manager
	if (EditManager)
	{
		EditManager->Shutdown();
		EditManager = nullptr;
	}

	// Don't delete renderer - we don't own it
	MeshRenderer = nullptr;
	Configuration = nullptr;

	bIsInitialized = false;

	UE_LOG(LogVoxelStreaming, Log, TEXT("ChunkManager shutdown. Stats: Generated=%lld, Meshed=%lld, Unloaded=%lld"),
		TotalChunksGenerated, TotalChunksMeshed, TotalChunksUnloaded);
}

// ==================== Streaming Control ====================

void UVoxelChunkManager::SetStreamingEnabled(bool bEnabled)
{
	bStreamingEnabled = bEnabled;

	if (bEnabled)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Chunk streaming enabled"));
	}
	else
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Chunk streaming disabled"));
	}
}

void UVoxelChunkManager::ForceStreamingUpdate()
{
	if (!bIsInitialized)
	{
		return;
	}

	// Set the force flag - TickComponent will handle the actual update
	// This ensures updates happen at the proper point in the frame sequence
	bForceStreamingUpdate = true;

	UE_LOG(LogVoxelStreaming, Log, TEXT("Force streaming update requested"));
}

// ==================== Chunk Requests ====================

void UVoxelChunkManager::RequestChunkLoad(const FIntVector& ChunkCoord, float Priority)
{
	if (!bIsInitialized)
	{
		return;
	}

	FVoxelChunkState& State = GetOrCreateChunkState(ChunkCoord);

	if (State.State == EChunkState::Unloaded)
	{
		// Add to generation queue with sorted insertion
		FChunkLODRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.LODLevel = 0; // Will be determined by LOD strategy
		Request.Priority = Priority;

		if (AddToGenerationQueue(Request))
		{
			SetChunkState(ChunkCoord, EChunkState::PendingGeneration);

			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) requested for loading"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		}
	}
}

void UVoxelChunkManager::RequestChunkUnload(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}

	if (ChunkStates.Contains(ChunkCoord))
	{
		const EChunkState CurrentState = ChunkStates[ChunkCoord].State;

		if (CurrentState != EChunkState::Unloaded && CurrentState != EChunkState::PendingUnload)
		{
			if (AddToUnloadQueue(ChunkCoord))
			{
				SetChunkState(ChunkCoord, EChunkState::PendingUnload);

				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) requested for unloading"),
					ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
			}
		}
	}
}

void UVoxelChunkManager::MarkChunkDirty(const FIntVector& ChunkCoord)
{
	if (!bIsInitialized)
	{
		return;
	}

	if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
	{
		if (State->State == EChunkState::Loaded)
		{
			State->Descriptor.bIsDirty = true;

			// Add to meshing queue for remeshing with sorted insertion
			FChunkLODRequest Request;
			Request.ChunkCoord = ChunkCoord;
			Request.LODLevel = State->LODLevel;
			Request.Priority = 100.0f; // High priority for dirty chunks

			if (AddToMeshingQueue(Request, EVoxelRemeshReason::Dirty))
			{
				SetChunkState(ChunkCoord, EChunkState::PendingMeshing);
			}
		}
	}
}

// ==================== Queries ====================

EChunkState UVoxelChunkManager::GetChunkState(const FIntVector& ChunkCoord) const
{
	if (const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
	{
		return State->State;
	}

	return EChunkState::Unloaded;
}

bool UVoxelChunkManager::IsChunkLoaded(const FIntVector& ChunkCoord) const
{
	return LoadedChunkCoords.Contains(ChunkCoord);
}

int32 UVoxelChunkManager::GetLoadedChunkCount() const
{
	return LoadedChunkCoords.Num();
}

void UVoxelChunkManager::GetLoadedChunks(TArray<FIntVector>& OutChunks) const
{
	OutChunks = LoadedChunkCoords.Array();
}

FIntVector UVoxelChunkManager::WorldToChunkCoord(const FVector& WorldPosition) const
{
	if (!Configuration)
	{
		return FIntVector::ZeroValue;
	}

	// Chunk keys are origin-relative: every chunk->world conversion in this class adds
	// Configuration->WorldOrigin, and GetVoxelAtWorldPosition/UVoxelEditManager subtract it.
	// Skipping the subtraction here returned coords shifted by WorldOrigin/ChunkWorldSize
	// chunks on any world with a non-zero origin — callers keying HasCollision /
	// RequestCollision / MarkChunkDirty off this hit phantom chunks (~556 m off in the demo).
	const FVector RelativePos = WorldPosition - Configuration->WorldOrigin;
	return FVoxelCoordinates::WorldToChunk(RelativePos, Configuration->ChunkSize, Configuration->VoxelSize);
}

FVoxelData UVoxelChunkManager::GetVoxelAtWorldPosition(const FVector& WorldPosition) const
{
	if (!bIsInitialized || !Configuration)
	{
		return FVoxelData::Air();
	}

	const FVector RelativePos = WorldPosition - Configuration->WorldOrigin;
	const FIntVector ChunkCoord = FVoxelCoordinates::WorldToChunk(
		RelativePos, Configuration->ChunkSize, Configuration->VoxelSize);

	const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || !State->Descriptor.HasVoxelDataAvailable())
	{
		return FVoxelData::Air();
	}

	const FIntVector LocalPos = FVoxelCoordinates::WorldToLocalVoxel(
		RelativePos, Configuration->ChunkSize, Configuration->VoxelSize);

	return State->Descriptor.GetVoxelResident(LocalPos);
}

FVoxelData UVoxelChunkManager::GetEditMergedVoxelAtWorldPosition(const FVector& WorldPosition) const
{
	if (!bIsInitialized || !Configuration)
	{
		return FVoxelData::Air();
	}

	const FVector RelativePos = WorldPosition - Configuration->WorldOrigin;
	const FIntVector ChunkCoord = FVoxelCoordinates::WorldToChunk(
		RelativePos, Configuration->ChunkSize, Configuration->VoxelSize);

	const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || !State->Descriptor.HasVoxelDataAvailable())
	{
		return FVoxelData::Air();
	}

	const FIntVector LocalPos = FVoxelCoordinates::WorldToLocalVoxel(
		RelativePos, Configuration->ChunkSize, Configuration->VoxelSize);

	FVoxelData Voxel = State->Descriptor.GetVoxelResident(LocalPos);

	// Apply the chunk's edit layer (same index convention as the descriptor / collision merge).
	if (EditManager && EditManager->ChunkHasEdits(ChunkCoord))
	{
		const FChunkEditLayer* EditLayer = EditManager->GetEditLayer(ChunkCoord);
		if (EditLayer && !EditLayer->IsEmpty())
		{
			const int32 Index = State->Descriptor.GetVoxelIndex(LocalPos);
			if (const FVoxelEdit* Edit = EditLayer->Edits.Find(Index))
			{
				Voxel = Edit->ApplyToProceduralData(Voxel);
			}
		}
	}

	return Voxel;
}

bool UVoxelChunkManager::QueryEditMergedSurface(
	double WorldX, double WorldY,
	float& OutHeight, FVector& OutNormal, float& OutSlopeDegrees,
	uint8& OutMaterialID, uint8& OutBiomeID) const
{
	const IVoxelWorldMode* WM = GetWorldMode();
	if (!bIsInitialized || !Configuration || !WM)
	{
		return false;
	}

	const float VoxelSize = Configuration->VoxelSize;
	const int32 ChunkSize = Configuration->ChunkSize;
	if (VoxelSize <= 0.0f || ChunkSize <= 0)
	{
		return false;
	}

	// Estimate the surface Z from the generator, used only to locate the column window + the
	// near/far decision (whether the surface's chunk is loaded).
	const float EstZ = WM->GetTerrainHeightAt(static_cast<float>(WorldX), static_cast<float>(WorldY), Configuration->NoiseParams);

	const FVector RelEst = FVector(WorldX, WorldY, EstZ) - Configuration->WorldOrigin;
	const FIntVector EstChunk = FVoxelCoordinates::WorldToChunk(RelEst, ChunkSize, VoxelSize);
	if (!IsChunkLoaded(EstChunk))
	{
		return false; // far band — caller falls back to the generator
	}

	// Assemble a vertical merged-voxel column over a +/- one-chunk window around the estimate
	// (covers dig-down / build-up that shifts the surface off the analytic height).
	const int32 Window = ChunkSize;
	const float BaseZ = EstZ - Window * VoxelSize;
	const int32 Count = 2 * Window + 1;

	TArray<FVoxelData> Column;
	Column.SetNumUninitialized(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		const float Z = BaseZ + i * VoxelSize;
		Column[i] = GetEditMergedVoxelAtWorldPosition(FVector(WorldX, WorldY, Z));
	}

	if (!FVoxelSurfaceQuery::ExtractSurfaceFromColumn(Column, BaseZ, VoxelSize, OutHeight, OutMaterialID, OutBiomeID))
	{
		return false;
	}

	// Surface normal from the edit-merged density gradient (central differences at the surface).
	auto Density = [this](double X, double Y, double Z) -> float
	{
		return static_cast<float>(GetEditMergedVoxelAtWorldPosition(FVector(X, Y, Z)).Density);
	};
	const float Step = VoxelSize;
	const float Hz = OutHeight;
	const float dX = Density(WorldX + Step, WorldY, Hz) - Density(WorldX - Step, WorldY, Hz);
	const float dY = Density(WorldX, WorldY + Step, Hz) - Density(WorldX, WorldY - Step, Hz);
	const float dZ = Density(WorldX, WorldY, Hz + Step) - Density(WorldX, WorldY, Hz - Step);

	// Density increases into the solid; the outward surface normal is the negated gradient.
	OutNormal = FVector(-dX, -dY, -dZ).GetSafeNormal();
	if (OutNormal.IsNearlyZero())
	{
		OutNormal = FVector::UpVector;
	}
	else if (OutNormal.Z < 0.0f)
	{
		OutNormal = -OutNormal; // keep it up-facing for a top surface
	}

	OutSlopeDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(static_cast<float>(OutNormal.Z), -1.0f, 1.0f)));
	return true;
}

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

	if (LODStrategy)
	{
		Stats += TEXT("\n");
		Stats += LODStrategy->GetDebugInfo();
	}

	return Stats;
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

	// Edit manager
	if (EditManager)
	{
		Stats.EditDataBytes = static_cast<int64>(EditManager->GetMemoryUsage());
	}

	// Renderer
	if (MeshRenderer)
	{
		Stats.RendererCPUBytes = MeshRenderer->GetCPUMemoryUsage();
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

	Stats.TotalBytes = Stats.VoxelDataBytes + Stats.EditDataBytes + Stats.RendererCPUBytes
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
		FLODQueryContext Context = const_cast<UVoxelChunkManager*>(this)->BuildQueryContext();
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

FVoxelCPUMarchingCubesMesher* UVoxelChunkManager::GetMarchingCubesMesher() const
{
	if (!Mesher.IsValid())
	{
		return nullptr;
	}

	// Check mesher type using the virtual GetMesherTypeName method
	// This avoids dynamic_cast which requires RTTI (disabled in UE)
	if (Mesher->GetMesherTypeName() == TEXT("CPU MarchingCubes"))
	{
		return static_cast<FVoxelCPUMarchingCubesMesher*>(Mesher.Get());
	}

	return nullptr;
}

// ==================== Internal Update Methods ====================

FLODQueryContext UVoxelChunkManager::BuildQueryContext() const
{
	FLODQueryContext Context;
	bool bFoundViewer = false;
	FString ViewerSource = TEXT("None");

	// Benchmark override: drive streaming from a fixed, script-controlled position so a run is
	// reproducible (and identical in headless and PIE). Bypasses the camera/viewport entirely.
	if (bBenchmarkViewActive)
	{
		Context.ViewerPosition = BenchmarkViewPosition;
		Context.ViewerForward = FVector::ForwardVector;
		Context.ViewerRight = FVector::RightVector;
		Context.ViewerUp = FVector::UpVector;
		Context.FieldOfView = 90.0f;
		if (const UWorld* World = GetWorld())
		{
			Context.GameTime = World->GetTimeSeconds();
			Context.DeltaTime = World->GetDeltaSeconds();
		}
		if (Configuration)
		{
			Context.ViewDistance = Configuration->ViewDistance;
			Context.WorldOrigin = Configuration->WorldOrigin;
			Context.WorldMode = Configuration->WorldMode;
			Context.WorldRadius = Configuration->WorldRadius;
			Context.MaxChunksToLoadPerFrame = Configuration->MaxChunksToLoadPerFrame;
			Context.MaxChunksToUnloadPerFrame = Configuration->MaxChunksToUnloadPerFrame;
			Context.TimeSliceMS = Configuration->StreamingTimeSliceMS;
		}
		Context.FrameNumber = CurrentFrame;
		return Context;
	}

	// Get viewer state from player controller
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector Location;
			FRotator Rotation;
			PC->GetPlayerViewPoint(Location, Rotation);
			ViewerSource = TEXT("GetPlayerViewPoint");

			// Check if we got a valid position (not at origin when player is elsewhere)
			if (PC->GetPawn())
			{
				// Use pawn location if available - more reliable than GetPlayerViewPoint in some cases
				Location = PC->GetPawn()->GetActorLocation();
				Rotation = PC->GetControlRotation();
				ViewerSource = TEXT("Pawn");
			}

			Context.ViewerPosition = Location;
			Context.ViewerForward = Rotation.Vector();
			Context.ViewerRight = Rotation.RotateVector(FVector::RightVector);
			Context.ViewerUp = Rotation.RotateVector(FVector::UpVector);
			bFoundViewer = true;

			if (PC->PlayerCameraManager)
			{
				Context.FieldOfView = PC->PlayerCameraManager->GetFOVAngle();
				// Camera manager has most accurate view location
				Context.ViewerPosition = PC->PlayerCameraManager->GetCameraLocation();
				Context.ViewerForward = PC->PlayerCameraManager->GetCameraRotation().Vector();
				ViewerSource = TEXT("CameraManager");
			}
		}

#if WITH_EDITOR
		// In editor, try to get the editor viewport camera if no player found
		if (!bFoundViewer)
		{
			if (GEditor)
			{
				for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
				{
					if (ViewportClient && ViewportClient->IsPerspective())
					{
						Context.ViewerPosition = ViewportClient->GetViewLocation();
						Context.ViewerForward = ViewportClient->GetViewRotation().Vector();
						Context.FieldOfView = ViewportClient->ViewFOV;
						bFoundViewer = true;
						ViewerSource = TEXT("EditorViewport");
						break;
					}
				}
			}
		}
#endif

		Context.GameTime = World->GetTimeSeconds();
		Context.DeltaTime = World->GetDeltaSeconds();
	}

	// Fallback: If no viewer found, use the owning actor's location
	if (!bFoundViewer)
	{
		if (AActor* Owner = GetOwner())
		{
			Context.ViewerPosition = Owner->GetActorLocation();
			Context.ViewerForward = Owner->GetActorForwardVector();
			ViewerSource = TEXT("OwnerActor");
			bFoundViewer = true;
		}
	}

	// Debug: Log viewer source and position periodically
	static int32 ContextDebugCounter = 0;
	if (++ContextDebugCounter % 180 == 0)  // Every 3 seconds at 60fps
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("BuildQueryContext: Source=%s, Pos=(%.0f, %.0f, %.0f), Found=%s"),
			*ViewerSource, Context.ViewerPosition.X, Context.ViewerPosition.Y, Context.ViewerPosition.Z,
			bFoundViewer ? TEXT("Yes") : TEXT("No"));
	}

	// Configuration values
	if (Configuration)
	{
		Context.ViewDistance = Configuration->ViewDistance;
		Context.WorldOrigin = Configuration->WorldOrigin;
		Context.WorldMode = Configuration->WorldMode;
		Context.WorldRadius = Configuration->WorldRadius;
		Context.MaxChunksToLoadPerFrame = Configuration->MaxChunksToLoadPerFrame;
		Context.MaxChunksToUnloadPerFrame = Configuration->MaxChunksToUnloadPerFrame;
		Context.TimeSliceMS = Configuration->StreamingTimeSliceMS;
	}

	Context.FrameNumber = CurrentFrame;

	return Context;
}

void UVoxelChunkManager::UpdateLoadDecisions(const FLODQueryContext& Context)
{
	if (!LODStrategy)
	{
		return;
	}

	// Get chunks to load (expensive operation - iterates visible area)
	TArray<FChunkLODRequest> ChunksToLoad;
	LODStrategy->GetChunksToLoad(ChunksToLoad, LoadedChunkCoords, Context);

	// Limit how many chunks we add per frame to prevent overwhelming the queue
	// Use a higher limit to ensure view distance fills in reasonable time
	const int32 MaxChunksToAddPerFrame = ResolveMaxLoadPerFrame() * 4;
	int32 ChunksAddedThisFrame = 0;
	int32 ChunksRemaining = 0;

	// Debug: Log streaming decisions periodically
	static int32 DebugFrameCounter = 0;
	if (++DebugFrameCounter % 60 == 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Streaming: Viewer at (%.0f, %.0f, %.0f), ChunksToLoad=%d, Loaded=%d, GenQueue=%d, MeshQueue=%d"),
			Context.ViewerPosition.X, Context.ViewerPosition.Y, Context.ViewerPosition.Z,
			ChunksToLoad.Num(), LoadedChunkCoords.Num(), GenerationQueue.Num(), MeshingQueue.Num());
	}

	// Add to generation queue with sorted insertion (O(1) duplicate check, O(log n) insert)
	for (const FChunkLODRequest& Request : ChunksToLoad)
	{
		const EChunkState CurrentState = GetChunkState(Request.ChunkCoord);

		if (CurrentState == EChunkState::Unloaded)
		{
			// Respect per-frame limit to prevent overwhelming the queue
			if (ChunksAddedThisFrame >= MaxChunksToAddPerFrame)
			{
				++ChunksRemaining;
				continue;  // Count remaining but don't add yet
			}

			FVoxelChunkState& State = GetOrCreateChunkState(Request.ChunkCoord);
			State.LODLevel = Request.LODLevel;
			State.Priority = Request.Priority;

			if (AddToGenerationQueue(Request))
			{
				SetChunkState(Request.ChunkCoord, EChunkState::PendingGeneration);
				++ChunksAddedThisFrame;
			}
		}
	}

	// If we hit the limit and there are still chunks to add, force an update next frame
	if (ChunksRemaining > 0)
	{
		bForceStreamingUpdate = true;
		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Streaming: %d chunks remaining, will continue next frame"), ChunksRemaining);
	}
}

void UVoxelChunkManager::UpdateUnloadDecisions(const FLODQueryContext& Context)
{
	if (!LODStrategy)
	{
		return;
	}

	// Get chunks to unload (cheap operation - just iterates loaded chunks)
	TArray<FIntVector> ChunksToUnload;
	LODStrategy->GetChunksToUnload(ChunksToUnload, LoadedChunkCoords, Context);

	// Add to unload queue with O(1) duplicate check
	for (const FIntVector& ChunkCoord : ChunksToUnload)
	{
		const EChunkState CurrentState = GetChunkState(ChunkCoord);

		if (CurrentState == EChunkState::Loaded)
		{
			if (AddToUnloadQueue(ChunkCoord))
			{
				SetChunkState(ChunkCoord, EChunkState::PendingUnload);
			}
		}
	}
}

void UVoxelChunkManager::ProcessGenerationQueue(float TimeSliceMS)
{
	if (GenerationQueue.Num() == 0 || !NoiseGenerator || !Configuration)
	{
		return;
	}

	// Throttle: limit concurrent async generation tasks
	if (AsyncGenerationInProgress.Num() >= EffectiveMaxAsyncGenerationTasks)
	{
		return;
	}

	const int32 MaxChunks = ResolveMaxLoadPerFrame();
	int32 ProcessedCount = 0;

	while (GenerationQueue.Num() > 0 && ProcessedCount < MaxChunks &&
	       AsyncGenerationInProgress.Num() < EffectiveMaxAsyncGenerationTasks)
	{
		// Skip chunks already being generated asynchronously
		if (AsyncGenerationInProgress.Contains(GenerationQueue.Last().ChunkCoord))
		{
			GenerationQueueSet.Remove(GenerationQueue.Last().ChunkCoord);
			GenerationQueue.Pop(EAllowShrinking::No);
			continue;
		}

		// Get highest priority chunk (at back) and remove from queue and tracking set
		FChunkLODRequest Request = GenerationQueue.Last();
		GenerationQueue.Pop(EAllowShrinking::No);
		GenerationQueueSet.Remove(Request.ChunkCoord);

		// Skip if state changed
		if (GetChunkState(Request.ChunkCoord) != EChunkState::PendingGeneration)
		{
			continue;
		}

		// Mark as generating
		SetChunkState(Request.ChunkCoord, EChunkState::Generating);

		// Build generation request
		FVoxelNoiseGenerationRequest GenRequest;
		GenRequest.ChunkCoord = Request.ChunkCoord;
		// Force LOD 0 when LOD system is disabled (defense-in-depth)
		GenRequest.LODLevel = (Configuration->bEnableLOD) ? Request.LODLevel : 0;
		GenRequest.ChunkSize = Configuration->ChunkSize;
		GenRequest.VoxelSize = Configuration->VoxelSize;
		GenRequest.NoiseParams = Configuration->NoiseParams;
		GenRequest.WorldMode = Configuration->WorldMode;
		GenRequest.SeaLevel = Configuration->SeaLevel;
		GenRequest.HeightScale = Configuration->HeightScale;
		GenRequest.BaseHeight = Configuration->BaseHeight;
		GenRequest.WorldOrigin = Configuration->WorldOrigin;

		// Biome configuration (contains biome definitions, blend settings, height rules)
		GenRequest.bEnableBiomes = Configuration->bEnableBiomes;
		GenRequest.BiomeConfiguration = Configuration->BiomeConfiguration;

		// Island mode parameters (used when WorldMode == IslandBowl)
		if (Configuration->WorldMode == EWorldMode::IslandBowl)
		{
			GenRequest.IslandParams.Shape = static_cast<uint8>(Configuration->IslandShape);
			GenRequest.IslandParams.IslandRadius = Configuration->IslandRadius;
			GenRequest.IslandParams.SizeY = Configuration->IslandSizeY;
			GenRequest.IslandParams.FalloffWidth = Configuration->IslandFalloffWidth;
			GenRequest.IslandParams.FalloffType = static_cast<uint8>(Configuration->IslandFalloffType);
			GenRequest.IslandParams.CenterX = Configuration->IslandCenterX;
			GenRequest.IslandParams.CenterY = Configuration->IslandCenterY;
			GenRequest.IslandParams.EdgeHeight = Configuration->IslandEdgeHeight;
			GenRequest.IslandParams.bBowlShape = Configuration->bIslandBowlShape;
		}

		// Spherical planet mode parameters (used when WorldMode == SphericalPlanet)
		if (Configuration->WorldMode == EWorldMode::SphericalPlanet)
		{
			GenRequest.SphericalPlanetParams.PlanetRadius = Configuration->WorldRadius;
			GenRequest.SphericalPlanetParams.MaxTerrainHeight = Configuration->PlanetMaxTerrainHeight;
			GenRequest.SphericalPlanetParams.MaxTerrainDepth = Configuration->PlanetMaxTerrainDepth;
			GenRequest.SphericalPlanetParams.PlanetCenter = Configuration->WorldOrigin;
			// Use PlanetHeightScale for terrain generation
			GenRequest.HeightScale = Configuration->PlanetHeightScale;
		}

		// Cave parameters
		GenRequest.bEnableCaves = Configuration->bEnableCaves;
		GenRequest.CaveConfiguration = Configuration->CaveConfiguration;

		// Water level parameters
		GenRequest.bEnableWaterLevel = Configuration->bEnableWaterLevel;
		GenRequest.WaterLevel = Configuration->WaterLevel;
		GenRequest.WaterRadius = Configuration->WaterRadius;

		// Terrain conditioning zones overlapping this chunk (Phase 6c: flatten under POIs/claims)
		GatherConditioningZonesForChunk(Request.ChunkCoord, GenRequest.ConditioningZones);

		// Launch async generation on thread pool
		LaunchAsyncGeneration(Request, MoveTemp(GenRequest));

		++ProcessedCount;
	}
}

void UVoxelChunkManager::AddConditioningZone(const FVoxelConditioningZone& Zone)
{
	ConditioningZones.Add(Zone);
	UE_LOG(LogVoxelStreaming, Log,
		TEXT("Added terrain conditioning zone at (%.0f,%.0f) inner=%.0f falloff=%.0f target=%.0f strength=%.2f; %d total."),
		Zone.Center.X, Zone.Center.Y, Zone.InnerRadius, Zone.FalloffWidth, Zone.TargetHeight, Zone.Strength, ConditioningZones.Num());
}

void UVoxelChunkManager::ClearConditioningZones()
{
	ConditioningZones.Reset();
}

void UVoxelChunkManager::GatherConditioningZonesForChunk(const FIntVector& ChunkCoord, TArray<FVoxelConditioningZone>& OutZones) const
{
	if ((ConditioningZones.Num() == 0 && TerrainConditioner == nullptr) || !Configuration)
	{
		return;
	}

	// Chunk XY footprint in world space (chunks cover the same area at every LOD).
	const float ChunkWorldSize = Configuration->ChunkSize * Configuration->VoxelSize;
	const FVector ChunkOrigin = Configuration->WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
	const FBox2D ChunkRegion(
		FVector2D(ChunkOrigin.X, ChunkOrigin.Y),
		FVector2D(ChunkOrigin.X + ChunkWorldSize, ChunkOrigin.Y + ChunkWorldSize));

	GatherConditioningZonesForRegion(ChunkRegion, OutZones);
}

void UVoxelChunkManager::GatherConditioningZonesForRegion(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const
{
	if (ConditioningZones.Num() == 0 && TerrainConditioner == nullptr)
	{
		return;
	}

	// Static zones whose influence overlaps the region.
	for (const FVoxelConditioningZone& Zone : ConditioningZones)
	{
		if (Zone.GetBounds2D().Intersect(Region))
		{
			OutZones.Add(Zone);
		}
	}

	// Dynamic (game-supplied) zones for this region.
	if (TerrainConditioner)
	{
		TerrainConditioner->GatherConditioning(Region, OutZones);
	}
}

void UVoxelChunkManager::GatherConditioningZonesForPoint(double WorldX, double WorldY, TArray<FVoxelConditioningZone>& OutZones) const
{
	// A degenerate (point) region: zone bounds that contain (X,Y) intersect it. FBox2D::Intersect is
	// inclusive on the boundary, so a point exactly on a zone's outer edge (weight 0) is harmless.
	const FVector2D P(WorldX, WorldY);
	GatherConditioningZonesForRegion(FBox2D(P, P), OutZones);
}

float UVoxelChunkManager::GetGeneratedSurfaceHeight(double WorldX, double WorldY) const
{
	const IVoxelWorldMode* WM = GetWorldMode();
	if (!bIsInitialized || !WM || !Configuration)
	{
		return 0.0f;
	}

	// Base terrain + continentalness (the world mode carries the biome context set at init).
	float Height = WM->GetTerrainHeightAt(
		static_cast<float>(WorldX), static_cast<float>(WorldY), Configuration->NoiseParams);

	// Layer terrain conditioning zones (POI / claim flatten) exactly as generation does.
	if (ConditioningZones.Num() > 0 || TerrainConditioner != nullptr)
	{
		TArray<FVoxelConditioningZone> Zones;
		GatherConditioningZonesForPoint(WorldX, WorldY, Zones);
		if (Zones.Num() > 0)
		{
			Height = FVoxelTerrainConditioning::ApplyToHeight(WorldX, WorldY, Height, Zones);
		}
	}

	return Height;
}

void UVoxelChunkManager::LaunchAsyncGeneration(const FChunkLODRequest& Request, FVoxelNoiseGenerationRequest GenRequest)
{
	// Mark as in-progress
	AsyncGenerationInProgress.Add(Request.ChunkCoord);

	// Capture raw pointer (TUniquePtr, safe because ChunkManager outlives tasks)
	IVoxelNoiseGenerator* GeneratorPtr = NoiseGenerator.Get();
	const FIntVector ChunkCoord = Request.ChunkCoord;

	// GPU generation path: dispatch on the render thread + async readback (no thread-pool worker, no
	// stall). The readback is polled each frame in ProcessPendingGPUReadbacks, where CPU post-passes
	// (tree injection) run before the result is handed to the shared completion queue.
	if (bUseGPUGenerationActive && GPUGeneratorPtr)
	{
		FVoxelGenerationHandle Handle = GPUGeneratorPtr->BeginGenerateChunkGPU(GenRequest);
		if (Handle.IsValid())
		{
			FPendingGPUGeneration Pending;
			Pending.Handle = Handle;
			Pending.GenRequest = MoveTemp(GenRequest);
			PendingGPUReadbacks.Add(ChunkCoord, MoveTemp(Pending));
		}
		else
		{
			// Dispatch failed (generator not initialized) — report failure via the normal completion queue
			// so ProcessCompletedAsyncGenerations clears in-progress tracking and resets the chunk state.
			FAsyncGenerationResult FailResult;
			FailResult.ChunkCoord = ChunkCoord;
			FailResult.bSuccess = false;
			CompletedGenerationQueue.Enqueue(MoveTemp(FailResult));
		}
		return;
	}

	// Tree injection captures (value copies for thread safety)
	const bool bInjectTrees = Configuration &&
		Configuration->MeshingMode == EMeshingMode::Cubic &&
		Configuration->TreeMode != EVoxelTreeMode::HISM &&
		Configuration->TreeTemplates.Num() > 0 &&
		Configuration->TreeDensity > 0.0f;
	TArray<FVoxelTreeTemplate> CapturedTreeTemplates;
	float CapturedTreeDensity = 0.0f;
	int32 CapturedWorldSeed = 0;
	FVector CapturedWorldOrigin = FVector::ZeroVector;
	FVoxelNoiseParams CapturedNoiseParams;
	IVoxelWorldMode* WorldModePtr = nullptr;
	UVoxelBiomeConfiguration* CapturedBiomeConfig = nullptr;
	bool CapturedEnableWaterLevel = false;
	float CapturedWaterLevel = 0.0f;

	if (bInjectTrees)
	{
		CapturedTreeTemplates = Configuration->TreeTemplates;
		CapturedTreeDensity = Configuration->TreeDensity;
		CapturedWorldSeed = Configuration->WorldSeed;
		CapturedWorldOrigin = Configuration->WorldOrigin;
		CapturedNoiseParams = Configuration->NoiseParams;
		WorldModePtr = WorldMode.Get(); // Raw ptr, same lifetime as NoiseGenerator
		CapturedBiomeConfig = Configuration->BiomeConfiguration;
		CapturedEnableWaterLevel = Configuration->bEnableWaterLevel;
		CapturedWaterLevel = Configuration->WaterLevel;
	}

	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakThis, GeneratorPtr, GenRequest = MoveTemp(GenRequest), ChunkCoord,
		bInjectTrees, CapturedTreeTemplates = MoveTemp(CapturedTreeTemplates),
		CapturedTreeDensity, CapturedWorldSeed, CapturedWorldOrigin,
		CapturedNoiseParams, WorldModePtr,
		CapturedBiomeConfig, CapturedEnableWaterLevel, CapturedWaterLevel]() mutable
	{
		// Generate voxel data on background thread
		TArray<FVoxelData> VoxelData;
		const bool bSuccess = GeneratorPtr->GenerateChunkCPU(GenRequest, VoxelData);

		// Inject voxel trees (runs on same thread pool worker, before enqueue)
		if (bSuccess && bInjectTrees && WorldModePtr)
		{
			FVoxelTreeInjector::InjectTrees(
				ChunkCoord,
				GenRequest.ChunkSize,
				GenRequest.VoxelSize,
				CapturedWorldOrigin,
				CapturedWorldSeed,
				CapturedTreeTemplates,
				CapturedNoiseParams,
				*WorldModePtr,
				CapturedTreeDensity,
				CapturedBiomeConfig,
				CapturedEnableWaterLevel,
				CapturedWaterLevel,
				VoxelData);
		}

		// Queue result for game thread
		if (UVoxelChunkManager* This = WeakThis.Get())
		{
			FAsyncGenerationResult Result;
			Result.ChunkCoord = ChunkCoord;
			Result.bSuccess = bSuccess;
			if (bSuccess)
			{
				Result.VoxelData = MoveTemp(VoxelData);
			}
			This->CompletedGenerationQueue.Enqueue(MoveTemp(Result));
		}
	});
}

void UVoxelChunkManager::ProcessCompletedAsyncGenerations(FVoxelTimingStats& Timing)
{
	FAsyncGenerationResult Result;
	int32 ProcessedCount = 0;
	const int32 MaxProcessPerFrame = 8;

	double StoreSeconds = 0.0;
	double NotifySeconds = 0.0;

	while (CompletedGenerationQueue.Dequeue(Result) && ProcessedCount < MaxProcessPerFrame)
	{
		// Remove from in-progress tracking
		AsyncGenerationInProgress.Remove(Result.ChunkCoord);

		// Check if chunk is still in valid state
		const EChunkState CurrentState = GetChunkState(Result.ChunkCoord);
		if (CurrentState != EChunkState::Generating)
		{
			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) async generation discarded - state changed to %d"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z, static_cast<int32>(CurrentState));
			continue;
		}

		if (Result.bSuccess)
		{
			// Store voxel data in chunk state
			FVoxelChunkState* State = ChunkStates.Find(Result.ChunkCoord);
			if (State)
			{
				double T0 = FPlatformTime::Seconds();
				State->Descriptor.SetResidentVoxelData(MoveTemp(Result.VoxelData));
				double T1 = FPlatformTime::Seconds();
				OnChunkGenerationComplete(Result.ChunkCoord);
				NotifySeconds += FPlatformTime::Seconds() - T1;
				StoreSeconds += T1 - T0;
			}
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d,%d,%d) async generation failed"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);

			FVoxelChunkState* State = ChunkStates.Find(Result.ChunkCoord);
			if (State)
			{
				State->Descriptor.ClearVoxelData();
			}
			SetChunkState(Result.ChunkCoord, EChunkState::Unloaded);
		}

		++ProcessedCount;
	}

	Timing.GenStoreMs = static_cast<float>(StoreSeconds * 1000.0);
	Timing.GenNotifyMs = static_cast<float>(NotifySeconds * 1000.0);
	Timing.GenApplyCount = ProcessedCount;
}

void UVoxelChunkManager::ProcessPendingGPUReadbacks()
{
	if (PendingGPUReadbacks.Num() == 0 || !GPUGeneratorPtr)
	{
		return;
	}

	// Poll each in-flight GPU readback. The water/underground post-passes run on the GPU inside the
	// generation graph (AddVoxelPostPassDispatches), so readbacks arrive as finished voxel data;
	// results feed the SAME CompletedGenerationQueue as the CPU path, so
	// ProcessCompletedAsyncGenerations applies the state check + storage uniformly and clears
	// AsyncGenerationInProgress. Only cubic-mode tree injection still needs CPU work, and it runs
	// on a thread-pool worker — never here: dispatches complete in batches, and per-chunk volume
	// work on the game thread stacked into 20-35ms generation spikes during traversal.
	TArray<FIntVector> Finished;
	for (TPair<FIntVector, FPendingGPUGeneration>& Pair : PendingGPUReadbacks)
	{
		const FIntVector ChunkCoord = Pair.Key;
		FPendingGPUGeneration& Pending = Pair.Value;

		TArray<FVoxelData> VoxelData;
		const EVoxelGPUReadbackStatus Status = GPUGeneratorPtr->PollGenerateChunkGPU(Pending.Handle, VoxelData);
		if (Status == EVoxelGPUReadbackStatus::Pending)
		{
			continue;
		}

		const int32 ExpectedVoxels = Pending.GenRequest.ChunkSize * Pending.GenRequest.ChunkSize * Pending.GenRequest.ChunkSize;

		if (Status == EVoxelGPUReadbackStatus::Ready && VoxelData.Num() == ExpectedVoxels)
		{
			LaunchPostReadbackProcessing(ChunkCoord, MoveTemp(Pending.GenRequest), MoveTemp(VoxelData));
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d,%d,%d) GPU generation failed (status=%d, voxels=%d/%d)"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, static_cast<int32>(Status), VoxelData.Num(), ExpectedVoxels);
			FAsyncGenerationResult FailResult;
			FailResult.ChunkCoord = ChunkCoord;
			FailResult.bSuccess = false;
			CompletedGenerationQueue.Enqueue(MoveTemp(FailResult));
		}

		GPUGeneratorPtr->ReleaseHandle(Pending.Handle);
		Finished.Add(ChunkCoord);
	}

	for (const FIntVector& ChunkCoord : Finished)
	{
		PendingGPUReadbacks.Remove(ChunkCoord);
	}
}

void UVoxelChunkManager::LaunchPostReadbackProcessing(const FIntVector& ChunkCoord, FVoxelNoiseGenerationRequest GenRequest, TArray<FVoxelData> VoxelData)
{
	// The water/underground post-passes already ran on the GPU (AddVoxelPostPassDispatches), so the
	// readback data is finished except for cubic-mode voxel-tree injection. Without trees there is
	// nothing left to compute — enqueue directly and skip the worker hop.
	const bool bInjectTrees = Configuration &&
		Configuration->MeshingMode == EMeshingMode::Cubic &&
		Configuration->TreeMode != EVoxelTreeMode::HISM &&
		Configuration->TreeTemplates.Num() > 0 &&
		Configuration->TreeDensity > 0.0f;

	if (!bInjectTrees || !WorldMode)
	{
		FAsyncGenerationResult Result;
		Result.ChunkCoord = ChunkCoord;
		Result.bSuccess = true;
		Result.VoxelData = MoveTemp(VoxelData);
		CompletedGenerationQueue.Enqueue(MoveTemp(Result));
		return;
	}

	// Tree injection captures (value copies for thread safety — same pattern as LaunchAsyncGeneration)
	TArray<FVoxelTreeTemplate> CapturedTreeTemplates = Configuration->TreeTemplates;
	const float CapturedTreeDensity = Configuration->TreeDensity;
	const int32 CapturedWorldSeed = Configuration->WorldSeed;
	const FVector CapturedWorldOrigin = Configuration->WorldOrigin;
	const FVoxelNoiseParams CapturedNoiseParams = Configuration->NoiseParams;
	IVoxelWorldMode* WorldModePtr = WorldMode.Get(); // Raw ptr, same lifetime as NoiseGenerator
	UVoxelBiomeConfiguration* CapturedBiomeConfig = Configuration->BiomeConfiguration;
	const bool CapturedEnableWaterLevel = Configuration->bEnableWaterLevel;
	const float CapturedWaterLevel = Configuration->WaterLevel;

	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakThis, ChunkCoord,
		GenRequest = MoveTemp(GenRequest), VoxelData = MoveTemp(VoxelData),
		CapturedTreeTemplates = MoveTemp(CapturedTreeTemplates),
		CapturedTreeDensity, CapturedWorldSeed, CapturedWorldOrigin,
		CapturedNoiseParams, WorldModePtr,
		CapturedBiomeConfig, CapturedEnableWaterLevel, CapturedWaterLevel]() mutable
	{
		FVoxelTreeInjector::InjectTrees(
			ChunkCoord,
			GenRequest.ChunkSize,
			GenRequest.VoxelSize,
			CapturedWorldOrigin,
			CapturedWorldSeed,
			CapturedTreeTemplates,
			CapturedNoiseParams,
			*WorldModePtr,
			CapturedTreeDensity,
			CapturedBiomeConfig,
			CapturedEnableWaterLevel,
			CapturedWaterLevel,
			VoxelData);

		if (UVoxelChunkManager* This = WeakThis.Get())
		{
			FAsyncGenerationResult Result;
			Result.ChunkCoord = ChunkCoord;
			Result.bSuccess = true;
			Result.VoxelData = MoveTemp(VoxelData);
			This->CompletedGenerationQueue.Enqueue(MoveTemp(Result));
		}
	});
}

void UVoxelChunkManager::ProcessMeshingQueue(float TimeSliceMS, FVoxelTimingStats& Timing)
{
	if (MeshingQueue.Num() == 0 || !Mesher || !Configuration)
	{
		return;
	}

	// Throttle if too many async tasks in flight or pending queue is full
	if (AsyncMeshingInProgress.Num() >= EffectiveMaxAsyncMeshTasks)
	{
		return;
	}
	if (PendingMeshQueue.Num() >= EffectiveMaxPendingMeshes)
	{
		return;
	}

	// Per-launch cost attribution (reported via the benchmark CSV)
	double SnapshotSeconds = 0.0;
	double SlicesSeconds = 0.0;
	double DispatchSeconds = 0.0;

	// Limit new dispatches per frame to avoid flooding the render thread.
	// Each GPU dispatch builds an RDG graph + uploads ~150KB + 4 compute passes.
	// Dispatching too many in one frame causes a render thread stall at frame sync.
	// The in-flight cap (EffectiveMaxAsyncMeshTasks) still controls pipeline depth;
	// this cap just prevents burst-refilling the pipeline in a single frame.
	const int32 MaxNewDispatches = FMath::Min(Configuration->MaxChunksToLoadPerFrame, 2);
	int32 ProcessedCount = 0;

	// P2-A: chunks deferred this frame because a face neighbor's voxel data is
	// still generating. They stay PendingMeshing and are re-queued after the loop
	// so they retry next frame (when the neighbor's data should be resident),
	// instead of meshing their boundary against a clamped duplicate plane.
	// Examined bounds per-frame work so a queue full of deferrable chunks can't
	// spin the whole queue every frame.
	TArray<FChunkLODRequest> DeferredThisFrame;
	int32 Examined = 0;
	const int32 MaxExamined = MaxNewDispatches + 48;

	while (MeshingQueue.Num() > 0 && ProcessedCount < MaxNewDispatches &&
	       Examined < MaxExamined &&
	       AsyncMeshingInProgress.Num() < EffectiveMaxAsyncMeshTasks &&
	       PendingMeshQueue.Num() < EffectiveMaxPendingMeshes)
	{
		++Examined;

		// Skip chunks already being meshed asynchronously
		if (AsyncMeshingInProgress.Contains(MeshingQueue.Last().ChunkCoord))
		{
			MeshingQueueSet.Remove(MeshingQueue.Last().ChunkCoord);
			MeshingQueue.Pop(EAllowShrinking::No);
			continue;
		}

		// Get highest priority chunk (at back) and remove from queue and tracking set
		FChunkLODRequest Request = MeshingQueue.Last();
		MeshingQueue.Pop(EAllowShrinking::No);
		MeshingQueueSet.Remove(Request.ChunkCoord);

		// Skip if state changed
		if (GetChunkState(Request.ChunkCoord) != EChunkState::PendingMeshing)
		{
			continue;
		}

		// Stale-cull: the viewer may have moved far past this chunk while it waited in the mesh
		// backlog. If it is now beyond the unload horizon it would be deleted the instant it
		// finished meshing -- so skip the wasted mesh job and route it straight to unload, freeing
		// the pipeline for chunks the viewer still needs. Safe: only drops chunks that the standard
		// distance-unload would remove on arrival anyway.
		if (bStaleCull && IsChunkBeyondUnloadDistance(Request.ChunkCoord))
		{
			if (AddToUnloadQueue(Request.ChunkCoord))
			{
				SetChunkState(Request.ChunkCoord, EChunkState::PendingUnload);
			}
			continue;
		}

		// Get chunk state for voxel data
		FVoxelChunkState* State = ChunkStates.Find(Request.ChunkCoord);
		if (!State || !State->Descriptor.HasVoxelDataAvailable())
		{
			// No voxel data available - skip
			continue;
		}

		// P2-A: don't mesh this boundary against a face neighbor whose voxel data
		// is still in the generation pipeline. GetVoxelAt would clamp the missing
		// plane to our own edge voxel (duplicate plane), producing the LOD-seam
		// tear that is never refreshed once the neighbor arrives. Defer instead and
		// retry next frame; if a neighbor's data will never arrive (freed/unloading)
		// proceed with a one-line warning so we never stall permanently.
		bool bClampUnavoidable = false;
		if (ShouldDeferMeshForNeighbors(Request.ChunkCoord, bClampUnavoidable))
		{
			// Leave the chunk in PendingMeshing; re-queue after the loop.
			DeferredThisFrame.Add(Request);
			continue;
		}
		if (bClampUnavoidable)
		{
			UE_LOG(LogVoxelStreaming, Warning,
				TEXT("Chunk (%d,%d,%d) meshing against a non-resident face neighbor whose data is not coming - boundary may clamp"),
				Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z);
		}

		// Mark as meshing
		SetChunkState(Request.ChunkCoord, EChunkState::Meshing);

		// Build meshing request
		double SubT0 = FPlatformTime::Seconds();
		FVoxelMeshingRequest MeshRequest;
		MeshRequest.ChunkCoord = Request.ChunkCoord;
		// Force LOD 0 when LOD system is disabled (defense-in-depth)
		MeshRequest.LODLevel = (Configuration->bEnableLOD) ? Request.LODLevel : 0;
		MeshRequest.ChunkSize = Configuration->ChunkSize;
		MeshRequest.VoxelSize = Configuration->VoxelSize;
		MeshRequest.WorldOrigin = Configuration->WorldOrigin;
		MeshRequest.VoxelData = State->Descriptor.GetVoxelDataForRead();

		// Merge edit layer if present
		if (EditManager && EditManager->ChunkHasEdits(Request.ChunkCoord))
		{
			const FChunkEditLayer* EditLayer = EditManager->GetEditLayer(Request.ChunkCoord);
			if (EditLayer && !EditLayer->IsEmpty())
			{
				for (const auto& EditPair : EditLayer->Edits)
				{
					const int32 Index = EditPair.Key;
					const FVoxelEdit& Edit = EditPair.Value;
					if (MeshRequest.VoxelData.IsValidIndex(Index))
					{
						// Apply edit relative to procedural data using edit mode and delta
						const FVoxelData& ProceduralData = MeshRequest.VoxelData[Index];
						MeshRequest.VoxelData[Index] = Edit.ApplyToProceduralData(ProceduralData);
					}
				}
				State->Descriptor.bHasEdits = true;

				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) merged %d edits from edit layer"),
					Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, EditLayer->GetEditCount());
			}
		}
		double SubT1 = FPlatformTime::Seconds();
		SnapshotSeconds += SubT1 - SubT0;

		// Extract neighbor edge slices for seamless boundaries
		ExtractNeighborEdgeSlices(Request.ChunkCoord, MeshRequest);
		SlicesSeconds += FPlatformTime::Seconds() - SubT1;

		// Calculate transition faces for Transvoxel LOD seam handling
		// Per Lengyel's Transvoxel algorithm, transition cells are generated on the FINER chunk
		// at faces that border a coarser neighbor. The transition cell's face corners (0,2,6,8)
		// match the coarser neighbor's MC grid, ensuring shared edge crossings produce identical
		// vertices. The 5 face midpoints add fine detail between coarse corners. The 4 interior
		// corners connect to the finer chunk's own MC grid.
		// A face needs transition cells if the neighbor is at a higher LOD level (coarser).
		MeshRequest.TransitionFaces = 0;
		const int32 CurrentLOD = Request.LODLevel;

		// Check each of 6 faces for coarser (higher LOD level) neighbors
		static const FIntVector NeighborOffsets[6] = {
			FIntVector(-1, 0, 0),  // -X
			FIntVector(1, 0, 0),   // +X
			FIntVector(0, -1, 0),  // -Y
			FIntVector(0, 1, 0),   // +Y
			FIntVector(0, 0, -1),  // -Z
			FIntVector(0, 0, 1),   // +Z
		};
		static const uint8 TransitionFlags[6] = {
			FVoxelMeshingRequest::TRANSITION_XNEG,
			FVoxelMeshingRequest::TRANSITION_XPOS,
			FVoxelMeshingRequest::TRANSITION_YNEG,
			FVoxelMeshingRequest::TRANSITION_YPOS,
			FVoxelMeshingRequest::TRANSITION_ZNEG,
			FVoxelMeshingRequest::TRANSITION_ZPOS,
		};

		// Helper to check if neighbor data was successfully extracted
		const int32 SliceSize = Configuration->ChunkSize * Configuration->ChunkSize;
		const int32 ChunkSize = Configuration->ChunkSize;
		auto HasNeighborData = [&MeshRequest, SliceSize](int32 FaceIndex) -> bool
		{
			switch (FaceIndex)
			{
				case 0: return MeshRequest.NeighborXNeg.Num() == SliceSize;
				case 1: return MeshRequest.NeighborXPos.Num() == SliceSize;
				case 2: return MeshRequest.NeighborYNeg.Num() == SliceSize;
				case 3: return MeshRequest.NeighborYPos.Num() == SliceSize;
				case 4: return MeshRequest.NeighborZNeg.Num() == SliceSize;
				case 5: return MeshRequest.NeighborZPos.Num() == SliceSize;
				default: return false;
			}
		};

		// Helper to check if ALL edge data needed for a transition face is available
		// Transition cells at face edges need diagonal neighbor data
		auto HasAllEdgeDataForFace = [&MeshRequest, ChunkSize](int32 FaceIndex) -> bool
		{
			// For each face, check if the edge neighbors that would be needed exist
			// Face 0 (-X): needs edges with Y and Z neighbors
			// Face 1 (+X): needs edges with Y and Z neighbors
			// Face 2 (-Y): needs edges with X and Z neighbors
			// Face 3 (+Y): needs edges with X and Z neighbors
			// Face 4 (-Z): needs edges with X and Y neighbors
			// Face 5 (+Z): needs edges with X and Y neighbors
			switch (FaceIndex)
			{
				case 0: // -X face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS);
				case 1: // +X face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS);
				case 2: // -Y face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS);
				case 3: // +Y face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS);
				case 4: // -Z face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG);
				case 5: // +Z face
					return MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS) &&
					       MeshRequest.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS);
				default:
					return false;
			}
		};

		// When LOD is disabled, skip all transition/neighbor LOD logic — all chunks are LOD 0
		if (Configuration->bEnableLOD)
		{
			for (int32 i = 0; i < 6; i++)
			{
				const FIntVector NeighborCoord = Request.ChunkCoord + NeighborOffsets[i];
				if (const FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord))
				{
					// Store neighbor LOD level for transition cell stride calculation
					// Use the neighbor's *rendered* LOD (not target LOD) so that
					// MergeLODBoundaryCells and skirts align with the neighbor's actual mesh.
					MeshRequest.NeighborLODLevels[i] = NeighborState->MeshedLODLevel;

					// TransitionFaces drives Transvoxel transition cells, which are ONLY
					// valid when the neighbor is genuinely COARSER (rendering at a higher
					// LOD level). Generating them for a same-LOD (or finer) neighbor builds
					// degenerate transition cells (CoarserStride == Stride) whose geometry
					// replaces regular MC at the boundary (Pass-2 skip) and tears — the
					// root cause of the live LOD-boundary fragments on steep terrain.
					//
					// A neighbor that is merely mid-LOD-transition (rendered != target) but
					// NOT coarser than us must NOT get a transition strip; it is re-meshed
					// via QueueNeighborsForRemesh once its LOD settles.
					const bool bNeighborCoarser = NeighborState->MeshedLODLevel > CurrentLOD;
					if (bNeighborCoarser)
					{
						if (HasNeighborData(i))
						{
							MeshRequest.TransitionFaces |= TransitionFlags[i];
						}
					}
				}
				else
				{
					// No neighbor - mark as -1
					MeshRequest.NeighborLODLevels[i] = -1;
				}
				// If neighbor doesn't exist, no transition needed (will be at chunk boundary anyway)
			}
		}
		// else: LOD disabled — NeighborLODLevels stays all -1 (default), TransitionFaces stays 0

		// P1 LOD-seam diagnostic: classify boundary faces for this chunk before it
		// meshes, distinguishing the confirmed clamp hazard from the Air-fill race.
		if (const int32 DiagLevel = CVarLogBoundaryResidency.GetValueOnGameThread())
		{
			// Per-face: slice array (neighbor) + own boundary plane, in the
			// -X,+X,-Y,+Y,-Z,+Z order used by NeighborLODLevels above.
			const TArray<FVoxelData>* FaceSlices[6] = {
				&MeshRequest.NeighborXNeg, &MeshRequest.NeighborXPos,
				&MeshRequest.NeighborYNeg, &MeshRequest.NeighborYPos,
				&MeshRequest.NeighborZNeg, &MeshRequest.NeighborZPos,
			};
			static const TCHAR* FaceNames[6] = { TEXT("-X"), TEXT("+X"), TEXT("-Y"), TEXT("+Y"), TEXT("-Z"), TEXT("+Z") };

			auto CountSolid = [](const TArray<FVoxelData>& Arr) -> int32
			{
				int32 N = 0;
				for (const FVoxelData& V : Arr) { if (V.IsSolid()) { ++N; } }
				return N;
			};

			// Count solids on this chunk's own boundary plane for a face.
			auto OwnPlaneSolid = [&MeshRequest, ChunkSize](int32 Face) -> int32
			{
				int32 N = 0;
				for (int32 A = 0; A < ChunkSize; ++A)
				{
					for (int32 B = 0; B < ChunkSize; ++B)
					{
						int32 X = 0, Y = 0, Z = 0;
						switch (Face)
						{
						case 0: X = 0;             Y = A; Z = B; break; // -X
						case 1: X = ChunkSize - 1; Y = A; Z = B; break; // +X
						case 2: Y = 0;             X = A; Z = B; break; // -Y
						case 3: Y = ChunkSize - 1; X = A; Z = B; break; // +Y
						case 4: Z = 0;             X = A; Y = B; break; // -Z
						default: Z = ChunkSize - 1; X = A; Y = B; break; // +Z
						}
						if (MeshRequest.GetVoxel(X, Y, Z).IsSolid()) { ++N; }
					}
				}
				return N;
			};

			FString FaceReport;
			int32 ClampRiskFaces = 0;
			int32 AirRaceFaces = 0;
			int32 MissingTransitionFaces = 0;  // active boundary, coarser neighbor, but NO transition set
			for (int32 i = 0; i < 6; ++i)
			{
				const bool bResident = FaceSlices[i]->Num() == SliceSize;
				const int32 SliceSolid = bResident ? CountSolid(*FaceSlices[i]) : 0;
				const int32 OwnSolid = OwnPlaneSolid(i);
				// A face can tear only if its own plane straddles the surface
				// (mix of solid and air). Fully-solid or fully-air planes are safe.
				const bool bActive = OwnSolid > 0 && OwnSolid < SliceSize;
				const bool bClampRisk = bActive && !bResident;
				const bool bAirRaceRisk = bActive && bResident && SliceSolid == 0;
				if (bClampRisk) { ++ClampRiskFaces; }
				if (bAirRaceRisk) { ++AirRaceFaces; }

				// Transvoxel engagement: TransitionFlags[i] == (1<<i). A face SHOULD get
				// a transition strip when it has a surface crossing AND a coarser
				// neighbor. If that's true but the bit is not set, transvoxel is not
				// engaging at this boundary — the suspected root cause of the live seam.
				const bool bNbrCoarser = MeshRequest.NeighborLODLevels[i] > CurrentLOD;
				const bool bTransitionSet = (MeshRequest.TransitionFaces & (1u << i)) != 0;
				const bool bMissingTransition = bActive && bNbrCoarser && !bTransitionSet;
				if (bMissingTransition) { ++MissingTransitionFaces; }

				if (DiagLevel >= 2 || bClampRisk || bAirRaceRisk || bMissingTransition)
				{
					FaceReport += FString::Printf(
						TEXT(" [%s res=%s nbrLOD=%d sliceSolid=%d ownSolid=%d%s%s%s%s]"),
						FaceNames[i], bResident ? TEXT("Y") : TEXT("N"),
						MeshRequest.NeighborLODLevels[i], SliceSolid, OwnSolid,
						bTransitionSet ? TEXT(" TF") : TEXT(""),
						bMissingTransition ? TEXT(" NOTF!") : TEXT(""),
						bClampRisk ? TEXT(" CLAMP") : TEXT(""),
						bAirRaceRisk ? TEXT(" AIRRACE") : TEXT(""));
				}
			}

			if (DiagLevel >= 2 || ClampRiskFaces > 0 || AirRaceFaces > 0 || MissingTransitionFaces > 0)
			{
				UE_LOG(LogVoxelStreaming, Warning,
					TEXT("[BoundaryDiag] Chunk(%d,%d,%d) LOD=%d TF=0x%02X clampRiskFaces=%d airRaceFaces=%d missingTF=%d%s"),
					Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
					CurrentLOD, MeshRequest.TransitionFaces, ClampRiskFaces, AirRaceFaces, MissingTransitionFaces, *FaceReport);
			}
		}

		// Launch async mesh generation instead of blocking. Move the request — it carries the
		// full chunk volume + neighbor slices (~1.3MB at 64^3); copying it into the by-value
		// parameter was a per-launch game-thread cost.
		const double DispatchT0 = FPlatformTime::Seconds();
		LaunchAsyncMeshGeneration(Request, MoveTemp(MeshRequest));
		DispatchSeconds += FPlatformTime::Seconds() - DispatchT0;

		++ProcessedCount;
	}

	// P2-A: re-queue chunks deferred this frame so they retry once their neighbors'
	// voxel data is resident. They are still in PendingMeshing state.
	for (const FChunkLODRequest& Deferred : DeferredThisFrame)
	{
		AddToMeshingQueue(Deferred);
	}

	Timing.MeshSnapshotMs = static_cast<float>(SnapshotSeconds * 1000.0);
	Timing.MeshSlicesMs = static_cast<float>(SlicesSeconds * 1000.0);
	Timing.MeshDispatchMs = static_cast<float>(DispatchSeconds * 1000.0);
	Timing.MeshLaunchCount = ProcessedCount;
}

void UVoxelChunkManager::LaunchAsyncMeshGeneration(const FChunkLODRequest& Request, FVoxelMeshingRequest MeshRequest)
{
	// Mark as in-progress
	AsyncMeshingInProgress.Add(Request.ChunkCoord);

	// Capture mesher pointer (it's a TUniquePtr, so we need raw pointer for lambda)
	IVoxelMesher* MesherPtr = Mesher.Get();
	const FIntVector ChunkCoord = Request.ChunkCoord;
	const int32 LODLevel = Request.LODLevel;

	// Use a weak pointer to safely check if ChunkManager is still valid
	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);

	// Detect GPU meshers by type name — use GenerateMeshAsync() for GPU dispatch
	const bool bUseGPUPath = MesherPtr->GetMesherTypeName().Contains(TEXT("GPU"));

	if (bUseGPUPath)
	{
		// GPU path: async callback (fired from the mesher's game-thread Tick) enqueues the result
		FOnVoxelMeshingComplete OnComplete;
		OnComplete.BindLambda([WeakThis, MesherPtr, ChunkCoord, LODLevel](
			FVoxelMeshingHandle Handle, bool bSuccess)
		{
			UVoxelChunkManager* This = WeakThis.Get();
			if (!This)
			{
				return;
			}

			FAsyncMeshResult AsyncResult;
			AsyncResult.ChunkCoord = ChunkCoord;
			AsyncResult.LODLevel = LODLevel;

			if (bSuccess)
			{
				// Readback GPU data to CPU for the rendering pipeline
				FChunkMeshData MeshData;
				if (MesherPtr->ReadbackToCPU(Handle, MeshData))
				{
					AsyncResult.bSuccess = true;
					AsyncResult.MeshData = MoveTemp(MeshData);
				}
				MesherPtr->ReleaseHandle(Handle);
			}
			else
			{
				AsyncResult.bSuccess = false;
			}

			This->CompletedMeshQueue.Enqueue(MoveTemp(AsyncResult));
		});

		// Dispatch from a thread-pool worker: GenerateMeshAsync's CPU side (packing the chunk
		// volume + neighbor slices into upload buffers, ~1.3MB of copies + allocations per chunk)
		// was a per-launch game-thread cost. The DC/MC meshers are worker-safe here (atomic
		// request ids, lock-guarded result maps, and their render command is self-contained).
		// The GPU cubic mesher is NOT — its dispatch path performs blocking readback flushes,
		// which are game-thread-only — so it keeps the inline dispatch.
		const bool bWorkerDispatch = !MesherPtr->GetMesherTypeName().Contains(TEXT("Cubic"));
		if (bWorkerDispatch)
		{
			Async(EAsyncExecution::ThreadPool, [MesherPtr, MeshRequest = MoveTemp(MeshRequest), OnComplete]() mutable
			{
				MesherPtr->GenerateMeshAsync(MeshRequest, OnComplete);
			});
		}
		else
		{
			MesherPtr->GenerateMeshAsync(MeshRequest, OnComplete);
		}
	}
	else
	{
		// CPU path: existing thread pool dispatch (unchanged)
		Async(EAsyncExecution::ThreadPool, [WeakThis, MesherPtr, MeshRequest = MoveTemp(MeshRequest), ChunkCoord, LODLevel]() mutable
		{
			// Generate mesh on background thread
			FChunkMeshData MeshData;
			const bool bSuccess = MesherPtr->GenerateMeshCPU(MeshRequest, MeshData);

			// Queue result for game thread (thread-safe MPSC queue)
			if (UVoxelChunkManager* This = WeakThis.Get())
			{
				FAsyncMeshResult Result;
				Result.ChunkCoord = ChunkCoord;
				Result.LODLevel = LODLevel;
				Result.bSuccess = bSuccess;
				if (bSuccess)
				{
					Result.MeshData = MoveTemp(MeshData);
				}
				This->CompletedMeshQueue.Enqueue(MoveTemp(Result));
			}
		});
	}
}

void UVoxelChunkManager::ProcessCompletedAsyncMeshes()
{
	FAsyncMeshResult Result;
	int32 ProcessedCount = 0;
	const int32 MaxProcessPerFrame = 8; // Limit how many we process to spread render uploads

	while (CompletedMeshQueue.Dequeue(Result) && ProcessedCount < MaxProcessPerFrame)
	{
		// Remove from in-progress tracking
		AsyncMeshingInProgress.Remove(Result.ChunkCoord);

		// Check if chunk is still in a valid state (might have been unloaded while meshing)
		const EChunkState CurrentState = GetChunkState(Result.ChunkCoord);
		if (CurrentState != EChunkState::Meshing)
		{
			// State changed while we were meshing. Route through PendingMeshQueue instead of
			// submitting directly to the renderer — this ensures OnChunkMeshingComplete handles
			// all mesh submissions, maintaining proper state tracking, LoadedChunkCoords, and
			// neighbor remesh cascades.
			if (Result.bSuccess && CurrentState != EChunkState::PendingUnload &&
				CurrentState != EChunkState::Unloaded)
			{
				// Deduplicate: remove any existing pending entry for this chunk coord
				for (int32 i = PendingMeshQueue.Num() - 1; i >= 0; --i)
				{
					if (PendingMeshQueue[i].ChunkCoord == Result.ChunkCoord)
					{
						PendingMeshQueue.RemoveAtSwap(i, EAllowShrinking::No);
						break;
					}
				}

				// Queue through normal path so OnChunkMeshingComplete processes it
				FPendingMeshData PendingMesh;
				PendingMesh.ChunkCoord = Result.ChunkCoord;
				PendingMesh.LODLevel = Result.LODLevel;
				PendingMesh.MeshData = MoveTemp(Result.MeshData);
				PendingMeshQueue.Add(MoveTemp(PendingMesh));

				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) mesh queued despite state change to %d"),
					Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z, static_cast<int32>(CurrentState));
			}
			else
			{
				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) async mesh discarded - state changed to %d"),
					Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z, static_cast<int32>(CurrentState));
			}
			continue;
		}

		if (Result.bSuccess)
		{
			// Deduplicate: remove stale pending entry for same chunk coord
			for (int32 i = PendingMeshQueue.Num() - 1; i >= 0; --i)
			{
				if (PendingMeshQueue[i].ChunkCoord == Result.ChunkCoord)
				{
					PendingMeshQueue.RemoveAtSwap(i, EAllowShrinking::No);
					break;
				}
			}

			// Store mesh in pending queue (will be submitted later, throttled)
			FPendingMeshData PendingMesh;
			PendingMesh.ChunkCoord = Result.ChunkCoord;
			PendingMesh.LODLevel = Result.LODLevel;
			PendingMesh.MeshData = MoveTemp(Result.MeshData);
			PendingMeshQueue.Add(MoveTemp(PendingMesh));
		}
		else
		{
			// Meshing failed - reset to PendingMeshing to retry
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d,%d,%d) async meshing failed"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
			SetChunkState(Result.ChunkCoord, EChunkState::PendingMeshing);
		}

		++ProcessedCount;
	}
}

bool UVoxelChunkManager::IsChunkBeyondUnloadDistance(const FIntVector& ChunkCoord) const
{
	if (!LODStrategy || !Configuration)
	{
		return false;
	}
	const float UnloadDist = LODStrategy->GetUnloadDistance();
	if (UnloadDist <= 0.0f)
	{
		return false; // strategy defines no unload horizon -> never stale-cull
	}
	const FBox Bounds = FVoxelCoordinates::ChunkToWorldBounds(
		ChunkCoord, Configuration->ChunkSize, Configuration->VoxelSize);
	const FVector ChunkCenter = Bounds.GetCenter() + Configuration->WorldOrigin;
	return FVector::DistSquared(CurrentViewerPosition, ChunkCenter) > FMath::Square(UnloadDist);
}

void UVoxelChunkManager::ProcessUnloadQueue(int32 MaxChunks)
{
	int32 ProcessedCount = 0;

	while (UnloadQueue.Num() > 0 && ProcessedCount < MaxChunks)
	{
		// Remove from queue and tracking set (pop from back for O(1))
		FIntVector ChunkCoord = UnloadQueue.Last();
		UnloadQueue.Pop(EAllowShrinking::No);
		UnloadQueueSet.Remove(ChunkCoord);

		// Skip if state changed
		if (GetChunkState(ChunkCoord) != EChunkState::PendingUnload)
		{
			// Unload cancelled (chunk needed again) — drop the pending stamp, don't count as lag.
			if (bBenchmarkViewActive) { UnloadEnqueueTimeSeconds.Remove(ChunkCoord); }
			continue;
		}

		// Benchmark: record unload latency (enqueue -> actual unload) = the lazy-deletion lag.
		if (bBenchmarkViewActive)
		{
			if (const double* EnqueueTime = UnloadEnqueueTimeSeconds.Find(ChunkCoord))
			{
				const double LagMs = (FPlatformTime::Seconds() - *EnqueueTime) * 1000.0;
				BenchUnloadLagSumMs += LagMs;
				BenchUnloadLagMaxMs = FMath::Max(BenchUnloadLagMaxMs, LagMs);
				++BenchUnloadLagCount;
				// Distance form of the lazy-deletion decision: how far the viewer is from the
				// chunk when it is finally unloaded (large == chunks linger past the active radius).
				if (Configuration)
				{
					const FBox LocalBounds = FVoxelCoordinates::ChunkToWorldBounds(
						ChunkCoord, Configuration->ChunkSize, Configuration->VoxelSize);
					const FVector ChunkCenter = LocalBounds.GetCenter() + Configuration->WorldOrigin;
					const double DistUU = FVector::Dist(BenchmarkViewPosition, ChunkCenter);
					BenchUnloadDistSumUU += DistUU;
					BenchUnloadDistMaxUU = FMath::Max(BenchUnloadDistMaxUU, DistUU);
				}
				UnloadEnqueueTimeSeconds.Remove(ChunkCoord);
			}
		}

		// Remove from renderer
		if (MeshRenderer)
		{
			MeshRenderer->RemoveChunk(ChunkCoord);
		}

		// Remove from loaded set
		LoadedChunkCoords.Remove(ChunkCoord);

		// Remove water tile contribution before state is cleared
		if (Configuration && Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial)
		{
			RemoveWaterTileContribution(ChunkCoord);
		}

		// Notify scatter manager
		if (ScatterManager)
		{
			ScatterManager->OnChunkUnloaded(ChunkCoord);
		}

		// Remove state tracking
		RemoveChunkState(ChunkCoord);

		// Fire event
		OnChunkUnloaded.Broadcast(ChunkCoord);

		++TotalChunksUnloaded;
		++ProcessedCount;

		UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) unloaded"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
	}
}

void UVoxelChunkManager::EvaluateLODLevelChanges(const FLODQueryContext& Context)
{
	if (!LODStrategy || !MeshRenderer)
	{
		return;
	}

	if (Configuration && !Configuration->bEnableLOD)
	{
		return;
	}

	// Use effective (adaptive) throttle value as a true per-frame cap
	// This is how many LOD remeshes we can QUEUE THIS FRAME, regardless of total pending
	const int32 MaxLODRemeshThisFrame = EffectiveMaxLODRemeshPerFrame;

	// Track chunks that need remeshing due to LOD level changes
	struct FLODRemeshCandidate
	{
		FIntVector ChunkCoord;
		int32 NewLODLevel;
		float Distance;
		bool bIsUpgrade;
	};
	TArray<FLODRemeshCandidate> RemeshCandidates;

	for (const FIntVector& ChunkCoord : LoadedChunkCoords)
	{
		const int32 NewLODLevel = LODStrategy->GetLODForChunk(ChunkCoord, Context);

		if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
		{
			if (State->LODLevel != NewLODLevel)
			{
				const float ChunkWorldSize = Configuration ? Configuration->GetChunkWorldSize() : 3200.0f;
				const FVector WorldOrigin = Configuration ? Configuration->WorldOrigin : FVector::ZeroVector;
				const FVector ChunkCenter = WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
				const float Distance = FVector::Dist(ChunkCenter, Context.ViewerPosition);

				FLODRemeshCandidate Candidate;
				Candidate.ChunkCoord = ChunkCoord;
				Candidate.NewLODLevel = NewLODLevel;
				Candidate.Distance = Distance;
				Candidate.bIsUpgrade = NewLODLevel < State->LODLevel;

				RemeshCandidates.Add(Candidate);
			}
		}
	}

	// Sort: upgrades first, then closer chunks first
	RemeshCandidates.Sort([](const FLODRemeshCandidate& A, const FLODRemeshCandidate& B)
	{
		if (A.bIsUpgrade != B.bIsUpgrade)
		{
			return A.bIsUpgrade;
		}
		return A.Distance < B.Distance;
	});

	// Queue limited number of remeshes per frame (true per-frame cap, not limited by existing pending)
	int32 QueuedThisFrame = 0;

	for (const FLODRemeshCandidate& Candidate : RemeshCandidates)
	{
		if (QueuedThisFrame >= MaxLODRemeshThisFrame)
		{
			break;
		}

		if (FVoxelChunkState* State = ChunkStates.Find(Candidate.ChunkCoord))
		{
			if (State->State == EChunkState::Loaded)
			{
				State->LODLevel = Candidate.NewLODLevel;

				FChunkLODRequest Request;
				Request.ChunkCoord = Candidate.ChunkCoord;
				Request.LODLevel = Candidate.NewLODLevel;
				Request.Priority = (Candidate.bIsUpgrade ? 100.0f : 50.0f) + (10000.0f / FMath::Max(Candidate.Distance, 1.0f));

				if (AddToMeshingQueue(Request, EVoxelRemeshReason::LODTransition))
				{
					SetChunkState(Candidate.ChunkCoord, EChunkState::PendingMeshing);
					++QueuedThisFrame;

					UE_LOG(LogVoxelStreaming, Verbose, TEXT("Queued chunk (%d,%d,%d) for LOD %s: %d (dist=%.0f)"),
						Candidate.ChunkCoord.X, Candidate.ChunkCoord.Y, Candidate.ChunkCoord.Z,
						Candidate.bIsUpgrade ? TEXT("upgrade") : TEXT("downgrade"),
						Candidate.NewLODLevel, Candidate.Distance);
				}
			}
		}
	}

	// NOTE: face-neighbour re-meshing on an LOD change is handled in OnChunkMeshingComplete, AFTER
	// the chunk's new mesh exists (so neighbours read the updated MeshedLODLevel). A second pass
	// used to run HERE, at the LOD decision, before the chunk had re-meshed — so neighbours
	// re-meshed against the stale rendered LOD and were redone post-mesh anyway. That premature
	// cascade was pure churn (~the bulk of the LOD-transition thrash) and is removed.
	if (QueuedThisFrame > 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("LOD level changes: %d candidates, queued %d/%d this frame"),
			RemeshCandidates.Num(), QueuedThisFrame, MaxLODRemeshThisFrame);
	}
}

void UVoxelChunkManager::UpdateLODMorphFactors(const FLODQueryContext& Context)
{
	if (!LODStrategy || !MeshRenderer)
	{
		return;
	}

	if (Configuration && !Configuration->bEnableLOD)
	{
		return;
	}

	TArray<TPair<FIntVector, float>> Transitions;

	for (const FIntVector& ChunkCoord : LoadedChunkCoords)
	{
		const float NewMorphFactor = LODStrategy->GetLODMorphFactor(ChunkCoord, Context);

		if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
		{
			if (FMath::Abs(State->MorphFactor - NewMorphFactor) > 0.01f)
			{
				State->MorphFactor = NewMorphFactor;
				Transitions.Add(TPair<FIntVector, float>(ChunkCoord, NewMorphFactor));
			}
		}
	}

	if (Transitions.Num() > 0)
	{
		MeshRenderer->UpdateLODTransitionsBatch(Transitions);
	}
}

// ==================== Chunk State Management ====================

FVoxelChunkState& UVoxelChunkManager::GetOrCreateChunkState(const FIntVector& ChunkCoord)
{
	if (FVoxelChunkState* Existing = ChunkStates.Find(ChunkCoord))
	{
		return *Existing;
	}

	FVoxelChunkState NewState(ChunkCoord);
	NewState.Descriptor.ChunkSize = Configuration ? Configuration->ChunkSize : VOXEL_DEFAULT_CHUNK_SIZE;

	return ChunkStates.Add(ChunkCoord, MoveTemp(NewState));
}

void UVoxelChunkManager::SetChunkState(const FIntVector& ChunkCoord, EChunkState NewState)
{
	if (FVoxelChunkState* State = ChunkStates.Find(ChunkCoord))
	{
		State->State = NewState;
		State->Descriptor.State = NewState;
		State->LastStateChangeFrame = CurrentFrame;
	}
}

void UVoxelChunkManager::RemoveChunkState(const FIntVector& ChunkCoord)
{
	ChunkStates.Remove(ChunkCoord);
}

void UVoxelChunkManager::ProcessFarCompressionSweep()
{
	if (CVarFarCompression.GetValueOnGameThread() == 0)
	{
		return;
	}

	const int32 MinStride = FMath::Max(1, CVarFarCompressionMinStride.GetValueOnGameThread());
	const int64 IdleFrames = FMath::Max(0, CVarFarCompressionIdleFrames.GetValueOnGameThread());
	const int32 MaxScans = FMath::Max(1, CVarFarCompressionMaxScansPerTick.GetValueOnGameThread());
	const EVoxelChunkCodec Codec = static_cast<EVoxelChunkCodec>(FMath::Clamp(CVarFarCompressionCodec.GetValueOnGameThread(), 0, 5));

	int32 Budget = 0;
	for (auto& Pair : ChunkStates)
	{
		FVoxelChunkState& S = Pair.Value;
		FChunkDescriptor& D = S.Descriptor;

		// Only settled, currently-resident far chunks that aren't pending a remesh.
		if (S.State != EChunkState::Loaded) continue;
		if (D.Residency != EVoxelDataResidency::Resident) continue;
		if (D.bIsDirty) continue;
		if ((1 << FMath::Clamp(S.LODLevel, 0, 7)) < MinStride) continue;
		// Idle: LastStateChangeFrame is the frame the chunk entered Loaded; it stays fixed while
		// settled and resets on any remesh (Loaded -> PendingMeshing -> ... -> Loaded).
		if ((CurrentFrame - S.LastStateChangeFrame) < IdleFrames) continue;

		if (D.bUniformValueValid || D.CompressedVoxelData.Num() > 0)
		{
			// A compact form is already cached (the chunk was expanded by access); re-apply it for
			// free — no scan, no re-encode. Not budgeted.
			D.TryCompress(Codec);
			continue;
		}
		if (D.bCompressionEvaluated)
		{
			// Already attempted; neither uniform nor the codec beat raw. Skip until content changes.
			continue;
		}
		if (Budget >= MaxScans)
		{
			// Out of budget this tick; retry next tick (bCompressionEvaluated stays false).
			continue;
		}
		++Budget;
		D.TryCompress(Codec);
	}
}

// ==================== Generation/Meshing Callbacks ====================

void UVoxelChunkManager::OnChunkGenerationComplete(const FIntVector& ChunkCoord)
{
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || State->State != EChunkState::Generating)
	{
		return;
	}

	++TotalChunksGenerated;

	// Queue for meshing with sorted insertion
	FChunkLODRequest Request;
	Request.ChunkCoord = ChunkCoord;
	Request.LODLevel = State->LODLevel;
	Request.Priority = State->Priority;

	AddToMeshingQueue(Request);
	SetChunkState(ChunkCoord, EChunkState::PendingMeshing);

	// Queue neighbors for remeshing so they can incorporate this chunk's edge data
	// This ensures seamless boundaries when chunks load in different orders
	QueueNeighborsForRemesh(ChunkCoord);

	// Fire event
	OnChunkGenerated.Broadcast(ChunkCoord);

	UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) generation complete"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
}

void UVoxelChunkManager::OnChunkMeshingComplete(const FIntVector& ChunkCoord)
{
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || State->State != EChunkState::Meshing)
	{
		// State changed while mesh was in PendingMeshQueue (e.g., chunk re-queued for remesh
		// or marked for unload). Still submit the mesh to the renderer to keep the chunk visible,
		// but don't change chunk state — the new mesh cycle will handle the state transition.
		bool bMeshSubmitted = false;
		int32 SubmittedLOD = -1;
		int32 PreviousMeshedLOD = State ? State->MeshedLODLevel : -1;

		for (int32 i = PendingMeshQueue.Num() - 1; i >= 0; --i)
		{
			if (PendingMeshQueue[i].ChunkCoord == ChunkCoord)
			{
				if (MeshRenderer && State)
				{
					// Entry is discarded right after — move the mesh data into the renderer
					const double RendT0 = FPlatformTime::Seconds();
					MeshRenderer->UpdateChunkMeshFromCPU(
						ChunkCoord,
						PendingMeshQueue[i].LODLevel,
						MoveTemp(PendingMeshQueue[i].MeshData));
					SubmitRendererSecondsThisTick += FPlatformTime::Seconds() - RendT0;
					State->MeshedLODLevel = PendingMeshQueue[i].LODLevel;
					if (bBenchmarkViewActive) { BenchEverMeshed.Add(ChunkCoord); }
					SubmittedLOD = PendingMeshQueue[i].LODLevel;
					bMeshSubmitted = true;
				}
				PendingMeshQueue.RemoveAtSwap(i, EAllowShrinking::No);
				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) mesh submitted despite state change (state=%d)"),
					ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, State ? static_cast<int32>(State->State) : -1);
				break;
			}
		}

		// If the chunk is already Loaded (e.g., neighbor remesh submitted a new mesh for it),
		// still trigger neighbor remesh cascade since MeshedLODLevel may have changed.
		if (bMeshSubmitted && State && State->State == EChunkState::Loaded)
		{
			if (Configuration && Configuration->bEnableLOD && PreviousMeshedLOD != State->MeshedLODLevel)
			{
				static const FIntVector FaceOffsets[6] = {
					FIntVector(1, 0, 0),  FIntVector(-1, 0, 0),
					FIntVector(0, 1, 0),  FIntVector(0, -1, 0),
					FIntVector(0, 0, 1),  FIntVector(0, 0, -1),
				};
				for (const FIntVector& Offset : FaceOffsets)
				{
					const FIntVector NeighborCoord = ChunkCoord + Offset;
					if (FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord))
					{
						if (NeighborState->State == EChunkState::Loaded)
						{
							FChunkLODRequest Request;
							Request.ChunkCoord = NeighborCoord;
							Request.LODLevel = NeighborState->LODLevel;
							Request.Priority = 20.0f;
							if (AddToMeshingQueue(Request, EVoxelRemeshReason::LODTransition))
							{
								SetChunkState(NeighborCoord, EChunkState::PendingMeshing);
							}
						}
					}
				}
			}
		}
		// Safety net: if chunk is in an unexpected state (not actively transitioning and not Loaded),
		// force it to Loaded to avoid permanently invisible chunks.
		else if (bMeshSubmitted && State &&
			State->State != EChunkState::PendingMeshing &&
			State->State != EChunkState::Meshing &&
			State->State != EChunkState::Loaded &&
			State->State != EChunkState::PendingUnload &&
			State->State != EChunkState::Unloaded)
		{
			UE_LOG(LogVoxelStreaming, Warning,
				TEXT("Chunk (%d,%d,%d) mesh submitted but chunk in unexpected state %d — forcing to Loaded"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, static_cast<int32>(State->State));
			LoadedChunkCoords.Add(ChunkCoord);
			State->Descriptor.bIsDirty = false;
			SetChunkState(ChunkCoord, EChunkState::Loaded);
			OnChunkLoaded.Broadcast(ChunkCoord);
		}

		return;
	}

	++TotalChunksMeshed;

	// Find mesh in pending queue (search from back since we pop from back)
	int32 PendingIndex = INDEX_NONE;
	for (int32 i = PendingMeshQueue.Num() - 1; i >= 0; --i)
	{
		if (PendingMeshQueue[i].ChunkCoord == ChunkCoord)
		{
			PendingIndex = i;
			break;
		}
	}

	if (PendingIndex != INDEX_NONE && MeshRenderer)
	{
		FPendingMeshData& PendingMesh = PendingMeshQueue[PendingIndex];
		const int32 PreviousMeshedLOD = State->MeshedLODLevel;

		// Notify scatter manager with voxel data for LOD-independent surface extraction.
		// Runs BEFORE the renderer submit: scatter reads MeshData, and the submit below
		// moves it into the renderer.
		if (ScatterManager && Configuration && Configuration->bEnableScatter)
		{
			const double ScatT0 = FPlatformTime::Seconds();

			// Scatter must classify against the terrain the player actually sees. When the
			// chunk has edits (player dig/build, POI, editor), merge them into a scratch copy
			// so scatter extracts from edit-merged voxels — otherwise grass can cover a dug
			// cave opening or mushrooms can appear on a carved-flat surface.
			if (EditManager && EditManager->ChunkHasEdits(ChunkCoord))
			{
				TArray<FVoxelData> EditMergedVoxels = State->Descriptor.GetVoxelDataForRead();
				EditManager->ApplyEditsToVoxelData(ChunkCoord, EditMergedVoxels);
				ScatterManager->OnChunkMeshDataReady(ChunkCoord, PendingMesh.LODLevel, PendingMesh.MeshData,
					EditMergedVoxels, State->Descriptor.ChunkSize, Configuration->VoxelSize);
			}
			else
			{
				ScatterManager->OnChunkMeshDataReady(ChunkCoord, PendingMesh.LODLevel, PendingMesh.MeshData,
					State->Descriptor.GetVoxelDataForRead(), State->Descriptor.ChunkSize, Configuration->VoxelSize);
			}
			SubmitScatterSecondsThisTick += FPlatformTime::Seconds() - ScatT0;
		}

		// Send mesh to renderer (entry is discarded below — move the mesh data in)
		const double RendT0 = FPlatformTime::Seconds();
		MeshRenderer->UpdateChunkMeshFromCPU(
			ChunkCoord,
			PendingMesh.LODLevel,
			MoveTemp(PendingMesh.MeshData)
		);
		SubmitRendererSecondsThisTick += FPlatformTime::Seconds() - RendT0;

		// Track which LOD is actually rendered so neighbors can read the correct
		// LOD level for MergeLODBoundaryCells and transition face setup.
		State->MeshedLODLevel = PendingMesh.LODLevel;
		if (bBenchmarkViewActive) { BenchEverMeshed.Add(ChunkCoord); }

		// Propagate water flags from loaded neighbors into this chunk's voxel data
		// so caves connected across chunk boundaries receive consistent water flags.
		if (Configuration && Configuration->bEnableWaterLevel)
		{
			const double WaterT0 = FPlatformTime::Seconds();
			PropagateWaterFromNeighbors(ChunkCoord);

			// Update this chunk's contribution to its 2D water tile
			if (Configuration->WaterMeshMaterial)
			{
				UpdateWaterTileContribution(ChunkCoord);
			}
			SubmitWaterSecondsThisTick += FPlatformTime::Seconds() - WaterT0;
		}

		// Remove from pending queue — O(1) swap since order doesn't matter (accessed by coord)
		PendingMeshQueue.RemoveAtSwap(PendingIndex, EAllowShrinking::No);

		// When MeshedLODLevel changed, face neighbors need to remesh so their
		// MergeLODBoundaryCells and TransitionFaces align with our new rendered LOD.
		// This closes the "break" phase of LOD transitions: without this, neighbors
		// retain stale boundary alignment until some other trigger re-meshes them.
		if (Configuration && Configuration->bEnableLOD && PreviousMeshedLOD != State->MeshedLODLevel)
		{
			static const FIntVector FaceOffsets[6] = {
				FIntVector(1, 0, 0),  FIntVector(-1, 0, 0),
				FIntVector(0, 1, 0),  FIntVector(0, -1, 0),
				FIntVector(0, 0, 1),  FIntVector(0, 0, -1),
			};
			for (const FIntVector& Offset : FaceOffsets)
			{
				const FIntVector NeighborCoord = ChunkCoord + Offset;
				if (FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord))
				{
					if (NeighborState->State == EChunkState::Loaded)
					{
						FChunkLODRequest Request;
						Request.ChunkCoord = NeighborCoord;
						Request.LODLevel = NeighborState->LODLevel;
						Request.Priority = 20.0f; // Low priority — refinement pass
						if (AddToMeshingQueue(Request, EVoxelRemeshReason::LODTransition))
						{
							SetChunkState(NeighborCoord, EChunkState::PendingMeshing);
						}
					}
				}
			}
		}
	}
	else if (PendingIndex == INDEX_NONE)
	{
		// Safety net: chunk reached OnChunkMeshingComplete but had no pending mesh data.
		// Re-queue for meshing to avoid an invisible loaded chunk.
		UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d,%d,%d) completed meshing with no pending mesh data — re-queueing"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
		FChunkLODRequest Request;
		Request.ChunkCoord = ChunkCoord;
		Request.LODLevel = State->LODLevel;
		Request.Priority = 80.0f;
		if (AddToMeshingQueue(Request))
		{
			SetChunkState(ChunkCoord, EChunkState::PendingMeshing);
		}
		return; // Don't set to Loaded — it needs to remesh first
	}

	// Mark as loaded
	LoadedChunkCoords.Add(ChunkCoord);
	State->Descriptor.bIsDirty = false;
	SetChunkState(ChunkCoord, EChunkState::Loaded);

	// Fire event
	OnChunkLoaded.Broadcast(ChunkCoord);

	UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) loaded"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
}

bool UVoxelChunkManager::ShouldDeferMeshForNeighbors(const FIntVector& ChunkCoord, bool& bOutClampUnavoidable) const
{
	bOutClampUnavoidable = false;

	if (!Configuration)
	{
		return false;
	}

	const int32 VolumeSize = Configuration->ChunkSize * Configuration->ChunkSize * Configuration->ChunkSize;

	static const FIntVector FaceOffsets[6] = {
		FIntVector(1, 0, 0),  FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0),  FIntVector(0, -1, 0),
		FIntVector(0, 0, 1),  FIntVector(0, 0, -1),
	};

	bool bDefer = false;
	for (const FIntVector& Offset : FaceOffsets)
	{
		const FVoxelChunkState* Neighbor = ChunkStates.Find(ChunkCoord + Offset);
		if (!Neighbor)
		{
			// No neighbor tracked here — this is the loaded-region edge, not a tear
			// (the chunk will be re-meshed via QueueNeighborsForRemesh when/if the
			// neighbor is later generated). Nothing to wait for.
			continue;
		}

		if (Neighbor->Descriptor.HasVoxelDataAvailable())
		{
			continue; // data available (resident or decompressable) — fine to mesh against
		}

		// Neighbor exists but its voxel data is not available.
		if (Neighbor->State == EChunkState::PendingGeneration ||
			Neighbor->State == EChunkState::Generating)
		{
			// Data is in the pipeline and will arrive — wait for it.
			bDefer = true;
		}
		else
		{
			// Loaded-but-freed, unloading, etc. — data is not coming. Don't stall
			// forever; let the caller mesh (it will clamp) but flag it.
			bOutClampUnavoidable = true;
		}
	}

	return bDefer;
}

void UVoxelChunkManager::QueueNeighborsForRemesh(const FIntVector& ChunkCoord)
{
	// Attribution timer: accumulated across the tick, reported as Timing.GenNeighborMs
	const double QnStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT { NeighborRemeshSecondsThisTick += FPlatformTime::Seconds() - QnStart; };

	// For Marching Cubes, we need to remesh all 26 neighbors (faces, edges, corners)
	// because diagonal chunks may use our voxel data at their boundaries.
	// Build list of all neighbor offsets: 6 faces + 12 edges + 8 corners = 26 total
	static const FIntVector NeighborOffsets[26] = {
		// 6 Face neighbors
		FIntVector(1, 0, 0),   FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0),   FIntVector(0, -1, 0),
		FIntVector(0, 0, 1),   FIntVector(0, 0, -1),
		// 12 Edge neighbors
		FIntVector(1, 1, 0),   FIntVector(1, -1, 0),   FIntVector(-1, 1, 0),   FIntVector(-1, -1, 0),
		FIntVector(1, 0, 1),   FIntVector(1, 0, -1),   FIntVector(-1, 0, 1),   FIntVector(-1, 0, -1),
		FIntVector(0, 1, 1),   FIntVector(0, 1, -1),   FIntVector(0, -1, 1),   FIntVector(0, -1, -1),
		// 8 Corner neighbors
		FIntVector(1, 1, 1),   FIntVector(1, 1, -1),   FIntVector(1, -1, 1),   FIntVector(1, -1, -1),
		FIntVector(-1, 1, 1),  FIntVector(-1, 1, -1),  FIntVector(-1, -1, 1),  FIntVector(-1, -1, -1)
	};

	// Check each neighbor
	for (int32 i = 0; i < 26; ++i)
	{
		const FIntVector NeighborCoord = ChunkCoord + NeighborOffsets[i];

		FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord);
		if (!NeighborState)
		{
			continue;
		}

		// Only remesh neighbors that are already in Loaded state
		// Neighbors in earlier states will get correct data during their initial meshing
		if (NeighborState->State == EChunkState::Loaded)
		{
			// Queue for remeshing - lower priority since it's a refinement
			FChunkLODRequest Request;
			Request.ChunkCoord = NeighborCoord;
			Request.LODLevel = NeighborState->LODLevel;
			Request.Priority = NeighborState->Priority * 0.5f; // Lower priority than new chunks

			// O(1) duplicate check + sorted insertion
			if (AddToMeshingQueue(Request, EVoxelRemeshReason::NeighborRemesh))
			{
				SetChunkState(NeighborCoord, EChunkState::PendingMeshing);

				UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) queued for remesh (neighbor of %d, %d, %d)"),
					NeighborCoord.X, NeighborCoord.Y, NeighborCoord.Z,
					ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
			}
		}
	}
}

bool UVoxelChunkManager::PropagateWaterFromNeighbors(const FIntVector& ChunkCoord)
{
	if (!Configuration || !Configuration->bEnableWaterLevel)
	{
		return false;
	}

	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || !State->Descriptor.HasVoxelDataAvailable())
	{
		return false;
	}

	const int32 CS = Configuration->ChunkSize;
	const float VS = Configuration->VoxelSize;
	const int32 VolumeSize = CS * CS * CS;
	const int32 SliceSize = CS * CS;
	const float WaterLevel = Configuration->WaterLevel;
	const FVector ChunkWorldPos = FVoxelCoordinates::ChunkToWorld(ChunkCoord, CS, VS);

	if (!State->Descriptor.HasVoxelDataAvailable())
	{
		return false;
	}

	// Entirely above the water level — no voxel can qualify as a seed (the per-voxel check
	// below rejects everything), so skip the 6-face boundary scan outright. This runs per
	// mesh submit and most submitted chunks are above the water level.
	if (ChunkWorldPos.Z > WaterLevel)
	{
		return false;
	}

	// Voxels above the water level never seed — clamp the Z iteration up front instead of
	// rejecting them one by one. For the X/Y faces, loop axis B is Z.
	const int32 MaxSeedZ = FMath::Clamp(FMath::FloorToInt32((WaterLevel - ChunkWorldPos.Z) / VS), 0, CS - 1);

	TArray<FVoxelData>& VoxelData = State->Descriptor.GetVoxelDataMutable();

	// Collect seed voxels from 6 face neighbors
	TArray<int32> Seeds;

	static const FIntVector FaceOffsets[6] = {
		{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
	};

	for (int32 F = 0; F < 6; ++F)
	{
		const FIntVector NeighborCoord = ChunkCoord + FaceOffsets[F];
		const FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord);
		if (!NeighborState || !NeighborState->Descriptor.HasVoxelDataAvailable())
		{
			continue;
		}

		const TArray<FVoxelData>& NeighborData = NeighborState->Descriptor.GetVoxelDataForRead();

		// Determine which boundary face to check on each chunk
		// For face direction F, check our boundary face and the neighbor's opposite face
		// e.g. +X neighbor: our X=CS-1 face vs neighbor's X=0 face
		const int32 MaxIdx = CS - 1;

		// +Z face: the whole plane sits at Z=MaxIdx — skip it when that plane is above water
		if (F == 4 && (ChunkWorldPos.Z + MaxIdx * VS) > WaterLevel)
		{
			continue;
		}

		// X/Y faces iterate Z on axis B — only the underwater band can seed
		const int32 MaxB = (F < 4) ? MaxSeedZ : MaxIdx;

		// Iterate boundary face voxels (2D loop over the two axes perpendicular to this face)
		for (int32 A = 0; A < CS; ++A)
		{
			for (int32 B = 0; B <= MaxB; ++B)
			{
				int32 OurX, OurY, OurZ;
				int32 NbrX, NbrY, NbrZ;

				switch (F)
				{
				case 0: // +X neighbor: our X=MaxIdx, neighbor X=0
					OurX = MaxIdx; OurY = A; OurZ = B;
					NbrX = 0;      NbrY = A; NbrZ = B;
					break;
				case 1: // -X neighbor: our X=0, neighbor X=MaxIdx
					OurX = 0;      OurY = A; OurZ = B;
					NbrX = MaxIdx; NbrY = A; NbrZ = B;
					break;
				case 2: // +Y neighbor: our Y=MaxIdx, neighbor Y=0
					OurX = A; OurY = MaxIdx; OurZ = B;
					NbrX = A; NbrY = 0;      NbrZ = B;
					break;
				case 3: // -Y neighbor: our Y=0, neighbor Y=MaxIdx
					OurX = A; OurY = 0;      OurZ = B;
					NbrX = A; NbrY = MaxIdx; NbrZ = B;
					break;
				case 4: // +Z neighbor: our Z=MaxIdx, neighbor Z=0
					OurX = A; OurY = B; OurZ = MaxIdx;
					NbrX = A; NbrY = B; NbrZ = 0;
					break;
				case 5: // -Z neighbor: our Z=0, neighbor Z=MaxIdx
					OurX = A; OurY = B; OurZ = 0;
					NbrX = A; NbrY = B; NbrZ = MaxIdx;
					break;
				default:
					continue;
				}

				const int32 NbrIdx = NbrX + NbrY * CS + NbrZ * SliceSize;
				const FVoxelData& NbrVoxel = NeighborData[NbrIdx];

				const int32 OurIdx = OurX + OurY * CS + OurZ * SliceSize;
				FVoxelData& OurVoxel = VoxelData[OurIdx];

				// Our voxel must be dry air below water level
				if (OurVoxel.IsSolid() || OurVoxel.HasWaterFlag())
				{
					continue;
				}

				const float OurWorldZ = ChunkWorldPos.Z + OurZ * VS;
				if (OurWorldZ > WaterLevel)
				{
					continue;
				}

				// Only propagate water from neighbors that already have water flags.
				// No special-casing for +Z face — the column scan in
				// ApplyWaterFillPass handles seeding for the water-level chunk.
				if (!NbrVoxel.HasWaterFlag())
				{
					continue;
				}

				// Seed this voxel
				OurVoxel.SetWaterFlag(true);
				Seeds.Add(OurIdx);
			}
		}
	}

	if (Seeds.Num() == 0)
	{
		return false;
	}

	// BFS flood fill from seeds (same algorithm as ApplyWaterFillPass Phase 2)
	const int32 NumSeeds = Seeds.Num();
	TArray<int32> BFSQueue = MoveTemp(Seeds);
	int32 QueueHead = 0;

	static constexpr int32 DX[6] = { 1, -1,  0,  0,  0,  0 };
	static constexpr int32 DY[6] = { 0,  0,  1, -1,  0,  0 };
	static constexpr int32 DZ[6] = { 0,  0,  0,  0,  1, -1 };

	while (QueueHead < BFSQueue.Num())
	{
		const int32 CurrentIdx = BFSQueue[QueueHead++];
		const int32 CZ = CurrentIdx / SliceSize;
		const int32 CY = (CurrentIdx - CZ * SliceSize) / CS;
		const int32 CX = CurrentIdx - CZ * SliceSize - CY * CS;

		for (int32 D = 0; D < 6; ++D)
		{
			const int32 NX = CX + DX[D];
			const int32 NY = CY + DY[D];
			const int32 NZ = CZ + DZ[D];

			if (NX < 0 || NX >= CS || NY < 0 || NY >= CS || NZ < 0 || NZ >= CS)
			{
				continue;
			}

			const int32 NeighborIdx = NX + NY * CS + NZ * SliceSize;
			FVoxelData& Neighbor = VoxelData[NeighborIdx];

			if (Neighbor.IsSolid() || Neighbor.HasWaterFlag())
			{
				continue;
			}

			const float NeighborWorldZ = ChunkWorldPos.Z + NZ * VS;
			if (NeighborWorldZ > WaterLevel)
			{
				continue;
			}

			Neighbor.SetWaterFlag(true);
			BFSQueue.Add(NeighborIdx);
		}
	}

	UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d): PropagateWaterFromNeighbors — %d seeds, %d total propagated"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, NumSeeds, BFSQueue.Num());

	return true;
}

// ==================== 2D Water Tile System ====================

void UVoxelChunkManager::UpdateWaterTileContribution(const FIntVector& ChunkCoord)
{
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || !State->Descriptor.HasVoxelDataAvailable())
	{
		return;
	}

	const int32 CS = State->Descriptor.ChunkSize;

	// Chunks entirely above the water level cannot hold water flags — skip the full-volume
	// column scan (this runs per mesh submit) and just clear any stale contribution.
	if (Configuration)
	{
		const FVector ChunkWorldPos = FVoxelCoordinates::ChunkToWorld(ChunkCoord, CS, Configuration->VoxelSize);
		if (ChunkWorldPos.Z > Configuration->WaterLevel)
		{
			RemoveWaterTileContribution(ChunkCoord);
			return;
		}
	}

	TArray<bool> PartialMask;
	PartialMask.SetNumZeroed(CS * CS);

	bool bAnyWater = FVoxelWaterMesher::BuildColumnMask(
		State->Descriptor.GetVoxelDataForRead(), CS, PartialMask);

	FIntVector2 TileCoord(ChunkCoord.X, ChunkCoord.Y);

	if (bAnyWater)
	{
		FWaterTileState& Tile = WaterTiles.FindOrAdd(TileCoord);
		Tile.PartialMasks.Add(ChunkCoord.Z, MoveTemp(PartialMask));

		// Mark dirty and queue
		Tile.bDirty = true;
		if (!DirtyWaterTileSet.Contains(TileCoord))
		{
			DirtyWaterTileQueue.Add(TileCoord);
			DirtyWaterTileSet.Add(TileCoord);
		}
	}
	else
	{
		// No water in this chunk — only touch the tile if a stale contribution needs removing.
		// (Previously this dirtied the tile unconditionally, so every dry-chunk remesh forced a
		// spurious water tile rebuild in ProcessDirtyWaterTiles.)
		RemoveWaterTileContribution(ChunkCoord);
	}
}

void UVoxelChunkManager::RemoveWaterTileContribution(const FIntVector& ChunkCoord)
{
	FIntVector2 TileCoord(ChunkCoord.X, ChunkCoord.Y);
	FWaterTileState* Tile = WaterTiles.Find(TileCoord);
	if (!Tile)
	{
		return;
	}

	if (Tile->PartialMasks.Remove(ChunkCoord.Z) == 0)
	{
		// This chunk contributed nothing — the tile's combined mask is unchanged
		return;
	}

	if (Tile->PartialMasks.IsEmpty())
	{
		// No more contributors — remove the tile entirely
		if (MeshRenderer)
		{
			MeshRenderer->RemoveWaterTile(TileCoord);
		}
		WaterTiles.Remove(TileCoord);
		DirtyWaterTileSet.Remove(TileCoord);
	}
	else
	{
		// Still has contributors — mark dirty for re-evaluation
		Tile->bDirty = true;
		if (!DirtyWaterTileSet.Contains(TileCoord))
		{
			DirtyWaterTileQueue.Add(TileCoord);
			DirtyWaterTileSet.Add(TileCoord);
		}
	}
}

void UVoxelChunkManager::ProcessDirtyWaterTiles(int32 MaxTilesPerFrame)
{
	if (!Configuration || !MeshRenderer)
	{
		return;
	}

	int32 Processed = 0;
	while (DirtyWaterTileQueue.Num() > 0 && Processed < MaxTilesPerFrame)
	{
		FIntVector2 TileCoord = DirtyWaterTileQueue.Last();
		DirtyWaterTileQueue.Pop();
		DirtyWaterTileSet.Remove(TileCoord);

		FWaterTileState* Tile = WaterTiles.Find(TileCoord);
		if (!Tile || !Tile->bDirty)
		{
			continue;
		}
		Tile->bDirty = false;

		// Combine all partial masks via OR
		const int32 CS = Configuration->ChunkSize;
		TArray<bool> CombinedMask;
		CombinedMask.SetNumZeroed(CS * CS);

		for (auto& Pair : Tile->PartialMasks)
		{
			for (int32 i = 0; i < CS * CS; i++)
			{
				if (Pair.Value[i])
				{
					CombinedMask[i] = true;
				}
			}
		}

		// Generate water mesh from combined mask
		const float ChunkExtent = static_cast<float>(CS) * Configuration->VoxelSize;
		const FVector TileWorldPos(
			Configuration->WorldOrigin.X + TileCoord.X * ChunkExtent,
			Configuration->WorldOrigin.Y + TileCoord.Y * ChunkExtent,
			Configuration->WorldOrigin.Z);

		FChunkMeshData WaterMeshData;
		FVoxelWaterMesher::GenerateWaterMeshFromMask(
			CombinedMask, CS, Configuration->VoxelSize,
			TileWorldPos, Configuration->WaterLevel, WaterMeshData);

		if (WaterMeshData.IsValid())
		{
			MeshRenderer->UpdateWaterTileMesh(TileCoord, WaterMeshData);
			UE_LOG(LogVoxelStreaming, Log, TEXT("Water tile (%d,%d): mesh submitted — %d verts, %d tris"),
				TileCoord.X, TileCoord.Y,
				WaterMeshData.GetVertexCount(), WaterMeshData.GetTriangleCount());
		}
		else
		{
			MeshRenderer->RemoveWaterTile(TileCoord);
		}
		++Processed;
	}
}

void UVoxelChunkManager::ExtractNeighborEdgeSlices(const FIntVector& ChunkCoord, FVoxelMeshingRequest& OutRequest)
{
	if (!Configuration)
	{
		return;
	}

	const int32 ChunkSize = Configuration->ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;
	const int32 VolumeSize = ChunkSize * ChunkSize * ChunkSize;

	// Reset edge/corner flags
	OutRequest.EdgeCornerFlags = 0;

	// Cache structure for neighbor chunk data to avoid repeated TMap lookups
	struct FNeighborCache
	{
		const FVoxelChunkState* State = nullptr;
		const FChunkEditLayer* EditLayer = nullptr;
		bool bHasData = false;
	};

	// Pre-cache neighbor data (26 potential neighbors: 6 faces + 12 edges + 8 corners)
	// Use a simple lambda to initialize cache on first access per neighbor
	TMap<FIntVector, FNeighborCache> NeighborCaches;
	NeighborCaches.Reserve(26);

	auto GetNeighborCache = [this, VolumeSize, &NeighborCaches](const FIntVector& NeighborCoord) -> const FNeighborCache&
	{
		if (const FNeighborCache* Cached = NeighborCaches.Find(NeighborCoord))
		{
			return *Cached;
		}

		FNeighborCache Cache;
		Cache.State = ChunkStates.Find(NeighborCoord);
		if (Cache.State && Cache.State->Descriptor.HasVoxelDataAvailable())
		{
			Cache.bHasData = true;
			// Materialize the neighbor's raw array ONCE here (const overload; no-op when already
			// resident). The per-voxel GetNeighborVoxel reads below then hit a guaranteed-resident
			// array — never decompress on the ~25-125k-voxel per-launch hot path.
			Cache.State->Descriptor.GetVoxelDataForRead();
			// Cache edit layer lookup (only once per neighbor)
			if (EditManager)
			{
				Cache.EditLayer = EditManager->GetEditLayer(NeighborCoord);
			}
		}
		return NeighborCaches.Add(NeighborCoord, Cache);
	};

	// Optimized helper lambda to get a single voxel from a neighbor chunk.
	// Every extraction loop below reads from ONE neighbor at a time, so memoize the
	// last-resolved cache entry: without this, the per-voxel TMap::Find (FIntVector hash +
	// probe) across the ~25-125k slice/deep voxels per launch dominated the whole meshing
	// section (~3.4ms/frame of the 4ms measured by MeshSliceMs). The memoized pointer is
	// refreshed whenever the coord changes, so it never outlives a map rehash from Add().
	FIntVector MemoCoord(INT32_MAX, INT32_MAX, INT32_MAX);
	const FNeighborCache* MemoCache = nullptr;
	auto GetNeighborVoxel = [ChunkSize, &GetNeighborCache, &MemoCoord, &MemoCache](const FIntVector& NeighborCoord, int32 X, int32 Y, int32 Z) -> FVoxelData
	{
		if (NeighborCoord != MemoCoord)
		{
			MemoCache = &GetNeighborCache(NeighborCoord);
			MemoCoord = NeighborCoord;
		}
		const FNeighborCache& Cache = *MemoCache;
		if (!Cache.bHasData)
		{
			return FVoxelData::Air();
		}

		const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
		// Residency was forced once at cache fill (bHasData ⇒ resident), so read the raw array
		// directly here — the memoized per-voxel hot path must not route through an accessor.
		const TArray<FVoxelData>& Voxels = Cache.State->Descriptor.VoxelData;
		if (!Voxels.IsValidIndex(Index))
		{
			return FVoxelData::Air();
		}

		FVoxelData Result = Voxels[Index];

		// Apply edit if present (using cached edit layer)
		if (Cache.EditLayer)
		{
			if (const FVoxelEdit* Edit = Cache.EditLayer->GetEdit(FIntVector(X, Y, Z)))
			{
				Result = Edit->ApplyToProceduralData(Result);
			}
		}

		return Result;
	};

	// Helper to check if neighbor chunk exists and has valid data (uses cache)
	auto HasNeighborData = [&GetNeighborCache](const FIntVector& NeighborCoord) -> bool
	{
		return GetNeighborCache(NeighborCoord).bHasData;
	};

	// Helper to get voxel index
	auto GetIndex = [ChunkSize](int32 X, int32 Y, int32 Z) -> int32
	{
		return X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
	};

	// ---- Deep neighbor planes (smooth meshers at LOD > 0) ----
	// A strided boundary cell reaches `stride` voxels into the neighbor and its
	// gradient normals reach ~2*stride; one face plane only suffices at stride 1.
	// For LOD > 0 we additionally supply (2*stride - 1) deeper planes per face so
	// Dual Contouring's outward boundary cell computes identically to the inward
	// neighbor (watertight), and Marching Cubes gets correct boundary normals. LOD 0
	// keeps a single plane (no extra cost). Capped so the source index stays in range.
	const int32 MeshStride = 1 << FMath::Clamp(OutRequest.LODLevel, 0, 7);
	// Total deep planes incl. plane 0. Default stride+1 = geometry-only: the outward DC boundary
	// cell reaches `stride` voxels deep (watertight), plus one plane for one-sided boundary normals.
	// -VoxelDeepFull restores 2*stride (adds central-difference normal reach at higher per-job cost,
	// ~14% slower catch-up at v6000); -VoxelDeepOff drops to 1 plane (no deep data, loses the seam fix).
	int32 DeepDepth;
	if (MeshStride <= 1 || bDeepDepthOff) { DeepDepth = 1; }
	else if (bDeepDepthFull)             { DeepDepth = 2 * MeshStride; }
	else                                 { DeepDepth = MeshStride + 1; }
	const int32 ExtraPlanes = FMath::Clamp(DeepDepth - 1, 0, ChunkSize - 1);
	OutRequest.NeighborPlaneDepth = ExtraPlanes + 1;

	// Fill a Deep array with planes one voxel deeper than the face slice (plane 0).
	// Axis: 0=X face, 1=Y face, 2=Z face; bNeg selects the -axis neighbor. Only fills
	// when the neighbor has data (caller guards) and ExtraPlanes > 0. In-plane index
	// (a + b*ChunkSize) matches the plane-0 layout for that face.
	auto FillDeep = [ChunkSize, SliceSize, ExtraPlanes, &GetNeighborVoxel]
		(TArray<FVoxelData>& DeepArr, const FIntVector& NCoord, int32 Axis, bool bNeg)
	{
		if (ExtraPlanes <= 0) { return; }
		DeepArr.SetNumUninitialized(ExtraPlanes * SliceSize);
		for (int32 k = 0; k < ExtraPlanes; ++k)
		{
			const int32 D = bNeg ? (ChunkSize - 2 - k) : (1 + k);
			for (int32 b = 0; b < ChunkSize; ++b)
			{
				for (int32 a = 0; a < ChunkSize; ++a)
				{
					const int32 NX = (Axis == 0) ? D : a;
					const int32 NY = (Axis == 1) ? D : ((Axis == 0) ? a : b);
					const int32 NZ = (Axis == 2) ? D : b;
					DeepArr[k * SliceSize + a + b * ChunkSize] = GetNeighborVoxel(NCoord, NX, NY, NZ);
				}
			}
		}
	};

	// Deep edge data: the 2-axis outward DC boundary cell reaches diagonally into the edge
	// neighbor, so fill the full NeighborPlaneDepth^2 grid of free-axis strips. AxisA/AxisB
	// are the two pinned axes (0=X,1=Y,2=Z), bNegA/bNegB select the negative neighbor, and
	// FreeAxis is the remaining (in-range) axis. Layout matches FVoxelMeshingRequest::EdgeDeepVoxel:
	// ((dA * D) + dB) * ChunkSize + free, with (0,0) == the base Edge* strip.
	const int32 DeepN = OutRequest.NeighborPlaneDepth; // == ExtraPlanes + 1
	auto FillEdgeDeep = [ChunkSize, DeepN, &GetNeighborVoxel]
		(TArray<FVoxelData>& DeepArr, const FIntVector& NCoord,
		 int32 AxisA, bool bNegA, int32 AxisB, bool bNegB, int32 FreeAxis)
	{
		if (DeepN <= 1) { return; } // LOD0: the single base strip is sufficient
		const int32 D = DeepN;
		DeepArr.SetNumUninitialized(D * D * ChunkSize);
		for (int32 a = 0; a < D; ++a)
		{
			const int32 CA = bNegA ? (ChunkSize - 1 - a) : a;
			for (int32 b = 0; b < D; ++b)
			{
				const int32 CB = bNegB ? (ChunkSize - 1 - b) : b;
				for (int32 f = 0; f < ChunkSize; ++f)
				{
					int32 C[3] = { 0, 0, 0 };
					C[AxisA] = CA; C[AxisB] = CB; C[FreeAxis] = f;
					DeepArr[(a * D + b) * ChunkSize + f] = GetNeighborVoxel(NCoord, C[0], C[1], C[2]);
				}
			}
		}
	};

	// Deep corner data: the 3-axis outward DC boundary cell reaches along the body diagonal
	// into the corner neighbor, so fill the full NeighborPlaneDepth^3 box. Layout matches
	// FVoxelMeshingRequest::CornerDeepVoxel: ((dX * D) + dY) * D + dZ, with (0,0,0) == the
	// scalar Corner* voxel.
	auto FillCornerDeep = [ChunkSize, DeepN, &GetNeighborVoxel]
		(TArray<FVoxelData>& DeepArr, const FIntVector& NCoord, bool bNegX, bool bNegY, bool bNegZ)
	{
		if (DeepN <= 1) { return; }
		const int32 D = DeepN;
		DeepArr.SetNumUninitialized(D * D * D);
		for (int32 a = 0; a < D; ++a)
		{
			const int32 CX = bNegX ? (ChunkSize - 1 - a) : a;
			for (int32 b = 0; b < D; ++b)
			{
				const int32 CY = bNegY ? (ChunkSize - 1 - b) : b;
				for (int32 c = 0; c < D; ++c)
				{
					const int32 CZ = bNegZ ? (ChunkSize - 1 - c) : c;
					DeepArr[(a * D + b) * D + c] = GetNeighborVoxel(NCoord, CX, CY, CZ);
				}
			}
		}
	};

	// ==================== Extract Face Neighbors ====================

	// +X neighbor (extract X=0 slice from neighbor)
	FIntVector NeighborXPosCoord = ChunkCoord + FIntVector(1, 0, 0);
	if (HasNeighborData(NeighborXPosCoord))
	{
		OutRequest.NeighborXPos.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				OutRequest.NeighborXPos[Y + Z * ChunkSize] = GetNeighborVoxel(NeighborXPosCoord, 0, Y, Z);
			}
		}
		FillDeep(OutRequest.NeighborXPosDeep, NeighborXPosCoord, 0, false);
	}

	// -X neighbor (extract X=ChunkSize-1 slice from neighbor)
	FIntVector NeighborXNegCoord = ChunkCoord + FIntVector(-1, 0, 0);
	if (HasNeighborData(NeighborXNegCoord))
	{
		OutRequest.NeighborXNeg.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				OutRequest.NeighborXNeg[Y + Z * ChunkSize] = GetNeighborVoxel(NeighborXNegCoord, ChunkSize - 1, Y, Z);
			}
		}
		FillDeep(OutRequest.NeighborXNegDeep, NeighborXNegCoord, 0, true);
	}

	// +Y neighbor (extract Y=0 slice from neighbor)
	FIntVector NeighborYPosCoord = ChunkCoord + FIntVector(0, 1, 0);
	if (HasNeighborData(NeighborYPosCoord))
	{
		OutRequest.NeighborYPos.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborYPos[X + Z * ChunkSize] = GetNeighborVoxel(NeighborYPosCoord, X, 0, Z);
			}
		}
		FillDeep(OutRequest.NeighborYPosDeep, NeighborYPosCoord, 1, false);
	}

	// -Y neighbor (extract Y=ChunkSize-1 slice from neighbor)
	FIntVector NeighborYNegCoord = ChunkCoord + FIntVector(0, -1, 0);
	if (HasNeighborData(NeighborYNegCoord))
	{
		OutRequest.NeighborYNeg.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborYNeg[X + Z * ChunkSize] = GetNeighborVoxel(NeighborYNegCoord, X, ChunkSize - 1, Z);
			}
		}
		FillDeep(OutRequest.NeighborYNegDeep, NeighborYNegCoord, 1, true);
	}

	// +Z neighbor (extract Z=0 slice from neighbor)
	FIntVector NeighborZPosCoord = ChunkCoord + FIntVector(0, 0, 1);
	if (HasNeighborData(NeighborZPosCoord))
	{
		OutRequest.NeighborZPos.SetNumUninitialized(SliceSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborZPos[X + Y * ChunkSize] = GetNeighborVoxel(NeighborZPosCoord, X, Y, 0);
			}
		}
		FillDeep(OutRequest.NeighborZPosDeep, NeighborZPosCoord, 2, false);
	}

	// -Z neighbor (extract Z=ChunkSize-1 slice from neighbor)
	FIntVector NeighborZNegCoord = ChunkCoord + FIntVector(0, 0, -1);
	if (HasNeighborData(NeighborZNegCoord))
	{
		OutRequest.NeighborZNeg.SetNumUninitialized(SliceSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborZNeg[X + Y * ChunkSize] = GetNeighborVoxel(NeighborZNegCoord, X, Y, ChunkSize - 1);
			}
		}
		FillDeep(OutRequest.NeighborZNegDeep, NeighborZNegCoord, 2, true);
	}

	// ==================== Extract Edge Neighbors (for Marching Cubes) ====================

	// Edge X+Y+ (diagonal chunk at +X+Y, extract X=0, Y=0, Z varies)
	FIntVector EdgeXPosYPos = ChunkCoord + FIntVector(1, 1, 0);
	if (HasNeighborData(EdgeXPosYPos))
	{
		OutRequest.EdgeXPosYPos.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXPosYPos[Z] = GetNeighborVoxel(EdgeXPosYPos, 0, 0, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_YPOS;
		FillEdgeDeep(OutRequest.EdgeXPosYPosDeep, EdgeXPosYPos, 0, false, 1, false, 2);
	}

	// Edge X+Y- (diagonal chunk at +X-Y, extract X=0, Y=ChunkSize-1, Z varies)
	FIntVector EdgeXPosYNeg = ChunkCoord + FIntVector(1, -1, 0);
	if (HasNeighborData(EdgeXPosYNeg))
	{
		OutRequest.EdgeXPosYNeg.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXPosYNeg[Z] = GetNeighborVoxel(EdgeXPosYNeg, 0, ChunkSize - 1, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_YNEG;
		FillEdgeDeep(OutRequest.EdgeXPosYNegDeep, EdgeXPosYNeg, 0, false, 1, true, 2);
	}

	// Edge X-Y+ (diagonal chunk at -X+Y, extract X=ChunkSize-1, Y=0, Z varies)
	FIntVector EdgeXNegYPos = ChunkCoord + FIntVector(-1, 1, 0);
	if (HasNeighborData(EdgeXNegYPos))
	{
		OutRequest.EdgeXNegYPos.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXNegYPos[Z] = GetNeighborVoxel(EdgeXNegYPos, ChunkSize - 1, 0, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_YPOS;
		FillEdgeDeep(OutRequest.EdgeXNegYPosDeep, EdgeXNegYPos, 0, true, 1, false, 2);
	}

	// Edge X-Y- (diagonal chunk at -X-Y, extract X=ChunkSize-1, Y=ChunkSize-1, Z varies)
	FIntVector EdgeXNegYNeg = ChunkCoord + FIntVector(-1, -1, 0);
	if (HasNeighborData(EdgeXNegYNeg))
	{
		OutRequest.EdgeXNegYNeg.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXNegYNeg[Z] = GetNeighborVoxel(EdgeXNegYNeg, ChunkSize - 1, ChunkSize - 1, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_YNEG;
		FillEdgeDeep(OutRequest.EdgeXNegYNegDeep, EdgeXNegYNeg, 0, true, 1, true, 2);
	}

	// Edge X+Z+ (diagonal chunk at +X+Z, extract X=0, Z=0, Y varies)
	FIntVector EdgeXPosZPos = ChunkCoord + FIntVector(1, 0, 1);
	if (HasNeighborData(EdgeXPosZPos))
	{
		OutRequest.EdgeXPosZPos.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXPosZPos[Y] = GetNeighborVoxel(EdgeXPosZPos, 0, Y, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_ZPOS;
		FillEdgeDeep(OutRequest.EdgeXPosZPosDeep, EdgeXPosZPos, 0, false, 2, false, 1);
	}

	// Edge X+Z- (diagonal chunk at +X-Z, extract X=0, Z=ChunkSize-1, Y varies)
	FIntVector EdgeXPosZNeg = ChunkCoord + FIntVector(1, 0, -1);
	if (HasNeighborData(EdgeXPosZNeg))
	{
		OutRequest.EdgeXPosZNeg.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXPosZNeg[Y] = GetNeighborVoxel(EdgeXPosZNeg, 0, Y, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_ZNEG;
		FillEdgeDeep(OutRequest.EdgeXPosZNegDeep, EdgeXPosZNeg, 0, false, 2, true, 1);
	}

	// Edge X-Z+ (diagonal chunk at -X+Z, extract X=ChunkSize-1, Z=0, Y varies)
	FIntVector EdgeXNegZPos = ChunkCoord + FIntVector(-1, 0, 1);
	if (HasNeighborData(EdgeXNegZPos))
	{
		OutRequest.EdgeXNegZPos.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXNegZPos[Y] = GetNeighborVoxel(EdgeXNegZPos, ChunkSize - 1, Y, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_ZPOS;
		FillEdgeDeep(OutRequest.EdgeXNegZPosDeep, EdgeXNegZPos, 0, true, 2, false, 1);
	}

	// Edge X-Z- (diagonal chunk at -X-Z, extract X=ChunkSize-1, Z=ChunkSize-1, Y varies)
	FIntVector EdgeXNegZNeg = ChunkCoord + FIntVector(-1, 0, -1);
	if (HasNeighborData(EdgeXNegZNeg))
	{
		OutRequest.EdgeXNegZNeg.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXNegZNeg[Y] = GetNeighborVoxel(EdgeXNegZNeg, ChunkSize - 1, Y, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_ZNEG;
		FillEdgeDeep(OutRequest.EdgeXNegZNegDeep, EdgeXNegZNeg, 0, true, 2, true, 1);
	}

	// Edge Y+Z+ (diagonal chunk at +Y+Z, extract Y=0, Z=0, X varies)
	FIntVector EdgeYPosZPos = ChunkCoord + FIntVector(0, 1, 1);
	if (HasNeighborData(EdgeYPosZPos))
	{
		OutRequest.EdgeYPosZPos.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYPosZPos[X] = GetNeighborVoxel(EdgeYPosZPos, X, 0, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YPOS_ZPOS;
		FillEdgeDeep(OutRequest.EdgeYPosZPosDeep, EdgeYPosZPos, 1, false, 2, false, 0);
	}

	// Edge Y+Z- (diagonal chunk at +Y-Z, extract Y=0, Z=ChunkSize-1, X varies)
	FIntVector EdgeYPosZNeg = ChunkCoord + FIntVector(0, 1, -1);
	if (HasNeighborData(EdgeYPosZNeg))
	{
		OutRequest.EdgeYPosZNeg.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYPosZNeg[X] = GetNeighborVoxel(EdgeYPosZNeg, X, 0, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YPOS_ZNEG;
		FillEdgeDeep(OutRequest.EdgeYPosZNegDeep, EdgeYPosZNeg, 1, false, 2, true, 0);
	}

	// Edge Y-Z+ (diagonal chunk at -Y+Z, extract Y=ChunkSize-1, Z=0, X varies)
	FIntVector EdgeYNegZPos = ChunkCoord + FIntVector(0, -1, 1);
	if (HasNeighborData(EdgeYNegZPos))
	{
		OutRequest.EdgeYNegZPos.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYNegZPos[X] = GetNeighborVoxel(EdgeYNegZPos, X, ChunkSize - 1, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YNEG_ZPOS;
		FillEdgeDeep(OutRequest.EdgeYNegZPosDeep, EdgeYNegZPos, 1, true, 2, false, 0);
	}

	// Edge Y-Z- (diagonal chunk at -Y-Z, extract Y=ChunkSize-1, Z=ChunkSize-1, X varies)
	FIntVector EdgeYNegZNeg = ChunkCoord + FIntVector(0, -1, -1);
	if (HasNeighborData(EdgeYNegZNeg))
	{
		OutRequest.EdgeYNegZNeg.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYNegZNeg[X] = GetNeighborVoxel(EdgeYNegZNeg, X, ChunkSize - 1, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YNEG_ZNEG;
		FillEdgeDeep(OutRequest.EdgeYNegZNegDeep, EdgeYNegZNeg, 1, true, 2, true, 0);
	}

	// ==================== Extract Corner Neighbors (for Marching Cubes) ====================

	// Corner X+Y+Z+ (diagonal chunk at +X+Y+Z, extract voxel at 0,0,0)
	FIntVector CornerPPP = ChunkCoord + FIntVector(1, 1, 1);
	if (HasNeighborData(CornerPPP))
	{
		OutRequest.CornerXPosYPosZPos = GetNeighborVoxel(CornerPPP, 0, 0, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS;
		FillCornerDeep(OutRequest.CornerXPosYPosZPosDeep, CornerPPP, false, false, false);
	}

	// Corner X+Y+Z- (diagonal chunk at +X+Y-Z, extract voxel at 0,0,ChunkSize-1)
	FIntVector CornerPPN = ChunkCoord + FIntVector(1, 1, -1);
	if (HasNeighborData(CornerPPN))
	{
		OutRequest.CornerXPosYPosZNeg = GetNeighborVoxel(CornerPPN, 0, 0, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG;
		FillCornerDeep(OutRequest.CornerXPosYPosZNegDeep, CornerPPN, false, false, true);
	}

	// Corner X+Y-Z+ (diagonal chunk at +X-Y+Z, extract voxel at 0,ChunkSize-1,0)
	FIntVector CornerPNP = ChunkCoord + FIntVector(1, -1, 1);
	if (HasNeighborData(CornerPNP))
	{
		OutRequest.CornerXPosYNegZPos = GetNeighborVoxel(CornerPNP, 0, ChunkSize - 1, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS;
		FillCornerDeep(OutRequest.CornerXPosYNegZPosDeep, CornerPNP, false, true, false);
	}

	// Corner X+Y-Z- (diagonal chunk at +X-Y-Z, extract voxel at 0,ChunkSize-1,ChunkSize-1)
	FIntVector CornerPNN = ChunkCoord + FIntVector(1, -1, -1);
	if (HasNeighborData(CornerPNN))
	{
		OutRequest.CornerXPosYNegZNeg = GetNeighborVoxel(CornerPNN, 0, ChunkSize - 1, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG;
		FillCornerDeep(OutRequest.CornerXPosYNegZNegDeep, CornerPNN, false, true, true);
	}

	// Corner X-Y+Z+ (diagonal chunk at -X+Y+Z, extract voxel at ChunkSize-1,0,0)
	FIntVector CornerNPP = ChunkCoord + FIntVector(-1, 1, 1);
	if (HasNeighborData(CornerNPP))
	{
		OutRequest.CornerXNegYPosZPos = GetNeighborVoxel(CornerNPP, ChunkSize - 1, 0, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS;
		FillCornerDeep(OutRequest.CornerXNegYPosZPosDeep, CornerNPP, true, false, false);
	}

	// Corner X-Y+Z- (diagonal chunk at -X+Y-Z, extract voxel at ChunkSize-1,0,ChunkSize-1)
	FIntVector CornerNPN = ChunkCoord + FIntVector(-1, 1, -1);
	if (HasNeighborData(CornerNPN))
	{
		OutRequest.CornerXNegYPosZNeg = GetNeighborVoxel(CornerNPN, ChunkSize - 1, 0, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG;
		FillCornerDeep(OutRequest.CornerXNegYPosZNegDeep, CornerNPN, true, false, true);
	}

	// Corner X-Y-Z+ (diagonal chunk at -X-Y+Z, extract voxel at ChunkSize-1,ChunkSize-1,0)
	FIntVector CornerNNP = ChunkCoord + FIntVector(-1, -1, 1);
	if (HasNeighborData(CornerNNP))
	{
		OutRequest.CornerXNegYNegZPos = GetNeighborVoxel(CornerNNP, ChunkSize - 1, ChunkSize - 1, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS;
		FillCornerDeep(OutRequest.CornerXNegYNegZPosDeep, CornerNNP, true, true, false);
	}

	// Corner X-Y-Z- (diagonal chunk at -X-Y-Z, extract voxel at ChunkSize-1,ChunkSize-1,ChunkSize-1)
	FIntVector CornerNNN = ChunkCoord + FIntVector(-1, -1, -1);
	if (HasNeighborData(CornerNNN))
	{
		OutRequest.CornerXNegYNegZNeg = GetNeighborVoxel(CornerNNN, ChunkSize - 1, ChunkSize - 1, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG;
		FillCornerDeep(OutRequest.CornerXNegYNegZNegDeep, CornerNNN, true, true, true);
	}
}

// ==================== Queue Management ====================

bool UVoxelChunkManager::AddToGenerationQueue(const FChunkLODRequest& Request)
{
	// O(1) duplicate check
	if (GenerationQueueSet.Contains(Request.ChunkCoord))
	{
		return false;
	}

	// Add to tracking set
	GenerationQueueSet.Add(Request.ChunkCoord);

	// Binary search for sorted insertion (ascending — highest priority at back for O(1) pop)
	int32 InsertIndex = Algo::LowerBound(GenerationQueue, Request);
	GenerationQueue.Insert(Request, InsertIndex);

	return true;
}

bool UVoxelChunkManager::AddToMeshingQueue(const FChunkLODRequest& Request, EVoxelRemeshReason Reason)
{
	// O(1) duplicate check
	if (MeshingQueueSet.Contains(Request.ChunkCoord))
	{
		return false;
	}

	// Benchmark thrash: a chunk re-entering the meshing queue after it already had a mesh is churn.
	// Attribute it to its source so we know which driver to attack.
	if (bBenchmarkViewActive && BenchEverMeshed.Contains(Request.ChunkCoord))
	{
		++BenchRemeshCount;
		++BenchRemeshByReason[static_cast<int32>(Reason)];
	}

	// Add to tracking set
	MeshingQueueSet.Add(Request.ChunkCoord);

	// Binary search for sorted insertion (ascending — highest priority at back for O(1) pop)
	int32 InsertIndex = Algo::LowerBound(MeshingQueue, Request);
	MeshingQueue.Insert(Request, InsertIndex);

	return true;
}

bool UVoxelChunkManager::AddToUnloadQueue(const FIntVector& ChunkCoord)
{
	// O(1) duplicate check
	if (UnloadQueueSet.Contains(ChunkCoord))
	{
		return false;
	}

	// Add to tracking set and queue
	UnloadQueueSet.Add(ChunkCoord);
	UnloadQueue.Add(ChunkCoord);

	// Benchmark: stamp enqueue time to measure unload latency (lazy-deletion lag).
	if (bBenchmarkViewActive)
	{
		UnloadEnqueueTimeSeconds.Add(ChunkCoord, FPlatformTime::Seconds());
	}

	return true;
}

void UVoxelChunkManager::RemoveFromGenerationQueue(const FIntVector& ChunkCoord)
{
	// Remove from tracking set
	GenerationQueueSet.Remove(ChunkCoord);

	// Remove from queue (linear search, but only called when processing)
	for (int32 i = 0; i < GenerationQueue.Num(); ++i)
	{
		if (GenerationQueue[i].ChunkCoord == ChunkCoord)
		{
			GenerationQueue.RemoveAt(i);
			return;
		}
	}
}

void UVoxelChunkManager::RemoveFromMeshingQueue(const FIntVector& ChunkCoord)
{
	// Remove from tracking set
	MeshingQueueSet.Remove(ChunkCoord);

	// Remove from queue (linear search, but only called when processing)
	for (int32 i = 0; i < MeshingQueue.Num(); ++i)
	{
		if (MeshingQueue[i].ChunkCoord == ChunkCoord)
		{
			MeshingQueue.RemoveAt(i);
			return;
		}
	}
}

void UVoxelChunkManager::RemoveFromUnloadQueue(const FIntVector& ChunkCoord)
{
	// Remove from tracking set
	UnloadQueueSet.Remove(ChunkCoord);

	// Remove from queue
	UnloadQueue.Remove(ChunkCoord);
}

// ==================== Streaming Benchmark ====================

void UVoxelChunkManager::StartBenchmark(const FVoxelBenchConfig& InConfig)
{
	ActiveBenchmark = MakeUnique<FVoxelStreamingBenchmark>(InConfig, this);
}

// Console: voxel.Bench.Run [tag] [velocityUU] [distanceUU]
// Starts a deterministic fixed-velocity traverse from the current view position (kept near the
// ground so the leading edge streams at LOD0) and writes a report under Saved/VoxelBench/.
static FAutoConsoleCommandWithWorldAndArgs GVoxelBenchRunCmd(
	TEXT("voxel.Bench.Run"),
	TEXT("Run a streaming benchmark traverse: voxel.Bench.Run [tag] [velocityUU] [distanceUU]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		// Target the ticking game/PIE world, not the editor world the command may arrive on.
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
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run: no PIE/Game world found (start PIE first)"));
			return;
		}

		UVoxelChunkManager* CM = nullptr;
		for (TActorIterator<AActor> It(TargetWorld); It; ++It)
		{
			if (UVoxelChunkManager* Found = It->FindComponentByClass<UVoxelChunkManager>())
			{
				CM = Found;
				break;
			}
		}
		if (!CM)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run: no UVoxelChunkManager found in the game world"));
			return;
		}

		FVoxelBenchConfig Config;
		if (Args.Num() > 0) { Config.Tag = Args[0]; }
		if (Args.Num() > 1) { Config.VelocityUU = FCString::Atof(*Args[1]); }
		if (Args.Num() > 2) { Config.TraverseDistance = FCString::Atof(*Args[2]); }

		// Start from the player view (on the ground -> leading edge stays LOD0); fall back to the
		// chunk-manager owner if no valid player view is available.
		FVector Start = FVector::ZeroVector;
		if (APlayerController* PC = TargetWorld->GetFirstPlayerController())
		{
			FVector Loc; FRotator Rot;
			PC->GetPlayerViewPoint(Loc, Rot);
			Start = Loc;
		}
		if (Start.IsNearlyZero())
		{
			if (const AActor* Owner = CM->GetOwner()) { Start = Owner->GetActorLocation(); }
			Start.Z += 500.0f; // keep slightly above the surface
		}
		Config.StartPosition = Start;
		CM->StartBenchmark(Config);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run '%s': world=%s start=(%.0f,%.0f,%.0f) vel=%.0f dist=%.0f"),
			*Config.Tag, *TargetWorld->GetName(), Start.X, Start.Y, Start.Z, Config.VelocityUU, Config.TraverseDistance);
	}));

// Console: voxel.RemeshAll
// Re-mesh every currently-loaded chunk IN PLACE (marks them dirty without changing LOD), so a live
// mesher CVar change (e.g. voxel.MCBoundaryMorph) takes effect immediately while the LOD rings /
// seams stay exactly where they are. Handy for A/B-ing meshing changes while detached and flying.
static FAutoConsoleCommandWithWorldAndArgs GVoxelRemeshAllCmd(
	TEXT("voxel.RemeshAll"),
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
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.RemeshAll: no PIE/Game world found (start PIE first)"));
			return;
		}

		int32 Managers = 0;
		int32 TotalDirtied = 0;
		for (TActorIterator<AActor> It(TargetWorld); It; ++It)
		{
			UVoxelChunkManager* CM = It->FindComponentByClass<UVoxelChunkManager>();
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
		UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.RemeshAll: re-meshing %d chunk(s) across %d manager(s)"), TotalDirtied, Managers);
	}));

// Console: voxel.Scatter.Stats
// Dump scatter manager + renderer statistics (HISM counts, pool active/pooled/util, pending
// adds, chunk/surface/spawn totals, memory) for perf and config profiling. Read the output
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
		for (TActorIterator<AActor> It(TargetWorld); It; ++It)
		{
			UVoxelChunkManager* CM = It->FindComponentByClass<UVoxelChunkManager>();
			if (!CM) { continue; }
			UVoxelScatterManager* ScatterMgr = CM->GetScatterManager();
			if (!ScatterMgr) { continue; }
			++Managers;
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Stats:\n%s"), *ScatterMgr->GetDebugStats());
		}
		if (Managers == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.Stats: no UVoxelChunkManager with a scatter manager found"));
		}
	}));

// Console: voxel.FarCompression.Stats
// Dump the chunk manager's voxel-data residency breakdown (Resident/Uniform/Compressed/Empty
// chunk counts + resident MB + MB reclaimed by compression). Read with unreal-mcp
// LogsToolset.GetLogEntries (category LogVoxelStreaming) since Claudius get_output_log fails
// while PIE holds the log file open.
static FAutoConsoleCommandWithWorldAndArgs GVoxelFarCompressionStatsCmd(
	TEXT("voxel.FarCompression.Stats"),
	TEXT("Log voxel-data residency + compression memory stats for the running game world."),
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
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Stats: no PIE/Game world found (start PIE first)"));
			return;
		}

		int32 Managers = 0;
		for (TActorIterator<AActor> It(TargetWorld); It; ++It)
		{
			UVoxelChunkManager* CM = It->FindComponentByClass<UVoxelChunkManager>();
			if (!CM) { continue; }
			++Managers;
			const UVoxelChunkManager::FVoxelMemoryStats M = CM->GetVoxelMemoryStats();
			UE_LOG(LogVoxelStreaming, Warning,
				TEXT("voxel.FarCompression.Stats: Resident=%d Uniform=%d Compressed=%d Empty=%d | ResidentVoxelData=%.1fMB Reclaimed=%.1fMB"),
				M.ResidentChunks, M.UniformChunks, M.CompressedChunks, M.EmptyChunks,
				M.VoxelDataBytes / (1024.0 * 1024.0), M.CompressionSavedBytes / (1024.0 * 1024.0));
		}
		if (Managers == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Stats: no UVoxelChunkManager found"));
		}
	}));

// ==================== Queue Re-Prioritization ====================

void UVoxelChunkManager::ReprioritizeQueues(const FLODQueryContext& Context)
{
	if (!Configuration)
	{
		return;
	}

	const FVector& ViewerPosition = Context.ViewerPosition;
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	const FVector WorldOrigin = Configuration->WorldOrigin;
	const float ViewDistance = Configuration->ViewDistance;
	// Add 20% buffer to prevent flip-flopping at ViewDistance boundary
	const float EvictDistanceSq = FMath::Square(ViewDistance * 1.2f);

	int32 EvictedCount = 0;
	int32 LODUpdatedCount = 0;

	// Re-prioritize, update LOD levels, and evict stale items from generation queue
	for (int32 i = GenerationQueue.Num() - 1; i >= 0; --i)
	{
		FChunkLODRequest& Request = GenerationQueue[i];
		const FVector ChunkCenter = WorldOrigin + FVector(Request.ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
		const float DistSq = FVector::DistSquared(ChunkCenter, ViewerPosition);

		if (DistSq > EvictDistanceSq)
		{
			// Beyond view distance — evict from queue and reset chunk state
			GenerationQueueSet.Remove(Request.ChunkCoord);
			SetChunkState(Request.ChunkCoord, EChunkState::Unloaded);
			RemoveChunkState(Request.ChunkCoord);
			GenerationQueue.RemoveAtSwap(i);
			++EvictedCount;
		}
		else
		{
			// Update priority: closer = higher priority
			Request.Priority = 1.0f / FMath::Max(FMath::Sqrt(DistSq), 1.0f);

			// Update LOD level based on current viewer position
			// This prevents chunks from being generated at a stale LOD level
			if (LODStrategy)
			{
				const int32 NewLOD = LODStrategy->GetLODForChunk(Request.ChunkCoord, Context);
				if (Request.LODLevel != NewLOD)
				{
					Request.LODLevel = NewLOD;
					// Also update the chunk state's LOD level
					if (FVoxelChunkState* State = ChunkStates.Find(Request.ChunkCoord))
					{
						State->LODLevel = NewLOD;
					}
					++LODUpdatedCount;
				}
			}
		}
	}

	// Re-sort generation queue (ascending — highest priority at back)
	if (GenerationQueue.Num() > 1)
	{
		GenerationQueue.Sort();
	}

	// Re-prioritize and update LOD levels in meshing queue (don't evict — generation data already computed)
	for (FChunkLODRequest& Request : MeshingQueue)
	{
		const FVector ChunkCenter = WorldOrigin + FVector(Request.ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
		const float Dist = FVector::Dist(ChunkCenter, ViewerPosition);
		Request.Priority = 1.0f / FMath::Max(Dist, 1.0f);

		// Update LOD level for meshing queue items too
		if (LODStrategy)
		{
			const int32 NewLOD = LODStrategy->GetLODForChunk(Request.ChunkCoord, Context);
			if (Request.LODLevel != NewLOD)
			{
				Request.LODLevel = NewLOD;
				if (FVoxelChunkState* State = ChunkStates.Find(Request.ChunkCoord))
				{
					State->LODLevel = NewLOD;
				}
				++LODUpdatedCount;
			}
		}
	}

	if (MeshingQueue.Num() > 1)
	{
		MeshingQueue.Sort();
	}

	if (EvictedCount > 0 || LODUpdatedCount > 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("ReprioritizeQueues: Evicted %d gen items, updated %d LOD levels, Gen=%d Mesh=%d remaining"),
			EvictedCount, LODUpdatedCount, GenerationQueue.Num(), MeshingQueue.Num());
	}
}

// ==================== Collision Mesh Generation ====================

bool UVoxelChunkManager::PrepareCollisionMeshRequest(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	FVoxelMeshingRequest& OutMeshRequest)
{
	if (!bIsInitialized || !Configuration || !Mesher)
	{
		return false;
	}

	// Get chunk state
	const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || State->State == EChunkState::Unloaded)
	{
		return false;
	}

	// Verify we have voxel data
	const int32 ChunkSize = Configuration->ChunkSize;
	const int32 VolumeSize = ChunkSize * ChunkSize * ChunkSize;
	if (!State->Descriptor.HasVoxelDataAvailable())
	{
		return false;
	}

	// Build meshing request for collision LOD
	OutMeshRequest.ChunkCoord = ChunkCoord;
	OutMeshRequest.LODLevel = LODLevel;
	OutMeshRequest.ChunkSize = ChunkSize;
	OutMeshRequest.VoxelSize = Configuration->VoxelSize;
	OutMeshRequest.WorldOrigin = Configuration->WorldOrigin;

	// Copy voxel data
	OutMeshRequest.VoxelData = State->Descriptor.GetVoxelDataForRead();

	// Merge edit layer if present
	if (EditManager && EditManager->ChunkHasEdits(ChunkCoord))
	{
		const FChunkEditLayer* EditLayer = EditManager->GetEditLayer(ChunkCoord);
		if (EditLayer && !EditLayer->IsEmpty())
		{
			for (const auto& EditPair : EditLayer->Edits)
			{
				const int32 Index = EditPair.Key;
				const FVoxelEdit& Edit = EditPair.Value;
				if (OutMeshRequest.VoxelData.IsValidIndex(Index))
				{
					const FVoxelData& ProceduralData = OutMeshRequest.VoxelData[Index];
					OutMeshRequest.VoxelData[Index] = Edit.ApplyToProceduralData(ProceduralData);
				}
			}
		}
	}

	// Extract neighbor data for seamless boundaries
	ExtractNeighborEdgeSlices(ChunkCoord, OutMeshRequest);

	// For collision, we don't need transition cells
	OutMeshRequest.TransitionFaces = 0;
	for (int32 i = 0; i < 6; ++i)
	{
		OutMeshRequest.NeighborLODLevels[i] = LODLevel;
	}

	return true;
}

bool UVoxelChunkManager::GetChunkCollisionMesh(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	FChunkMeshData& OutMeshData)
{
	FVoxelMeshingRequest MeshRequest;
	if (!PrepareCollisionMeshRequest(ChunkCoord, LODLevel, MeshRequest))
	{
		return false;
	}

	OutMeshData.Reset();
	return Mesher->GenerateMeshCPU(MeshRequest, OutMeshData);
}
