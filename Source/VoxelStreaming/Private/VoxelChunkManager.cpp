// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelChunkManager.h"
#include "VoxelStreaming.h"
#include "VoxelSeamRegistry.h"
#include "VoxelBoundedDrain.h"
#include "VoxelSeamTransitionHolds.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelCaveConfiguration.h"
#include "Algo/BinarySearch.h"
#include "Async/Async.h"
#include "Tasks/Task.h"
#include "Misc/ScopeExit.h"
#include "VoxelCoordinates.h"
#include "VoxelDigitalLine.h"
#include "IVoxelLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "IVoxelWorldMode.h"
#include "VoxelWorldModeFactory.h"
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
#include "VoxelSeaSurface.h"
#include "VoxelWaterMesher.h"
#include "VoxelCollisionManager.h"
#include "VoxelScatterManager.h"
#include "VoxelTreeInjector.h"
#include "VoxelTreeTypes.h"
#include "VoxelWorldBoundary.h"
#include "VoxelFarOcean.h"
#include "IslandBowlWorldMode.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelBiomeSnapshot.h"
#include "ProceduralMeshComponent.h"
#include "Components/BoxComponent.h"
#include "UObject/UObjectIterator.h"
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

// Runtime master-enable for GPU terrain generation. Lets you force CPU generation without editing the
// config asset or restarting the build: 1 = honor bUseGPUGeneration; 0 = force CPU. Read at chunk-manager
// Initialize (generator selection), so re-initialize the voxel world after toggling for it to take effect.
// World boundary (world modes review WM-I5): the edge walls' collision, for every live chunk manager.
static TAutoConsoleVariable<int32> CVarWorldBoundary(
	TEXT("voxel.World.Boundary"),
	1,
	TEXT("Invisible walls at a bounded world's edge (an island's R + falloff).\n")
	TEXT("  1 = block pawns, NPCs and physics bodies there (default)\n")
	TEXT("  0 = walls without collision (the world's edge is open; for repros)"),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Var)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			It->SetWorldBoundaryCollisionEnabled(Var->GetInt() != 0);
		}
	}),
	ECVF_Default);

// Far ocean (world modes review WM-E1): its visibility, for every live chunk manager.
static TAutoConsoleVariable<int32> CVarFarOcean(
	TEXT("voxel.World.FarOcean"),
	1,
	TEXT("The open sea and the seabed past a bounded world's streamed columns (an island's far ocean).\n")
	TEXT("  1 = drawn (default)\n")
	TEXT("  0 = hidden (the island floats in the void again; for before/after views)"),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Var)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			It->SetFarOceanVisible(Var->GetInt() != 0);
		}
	}),
	ECVF_Default);

// A planet's sea surface (spherical SP-E2): its visibility, for every live chunk manager.
static TAutoConsoleVariable<int32> CVarPlanetOcean(
	TEXT("voxel.World.PlanetOcean"),
	1,
	TEXT("A planet's sea surface: a cube sphere at WaterRadius with the configuration's water material.\n")
	TEXT("  1 = drawn (default)\n")
	TEXT("  0 = hidden (for before/after views)"),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Var)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			It->SetPlanetOceanVisible(Var->GetInt() != 0);
		}
	}),
	ECVF_Default);

// How far inside the edge the far ocean's water stands in for columns the streaming has not loaded. Read when the
// far ocean is built.
static TAutoConsoleVariable<float> CVarFarOceanInland(
	TEXT("voxel.World.FarOceanInland"),
	-1.0f,
	TEXT("How far inside a bounded world's edge (uu) the far ocean's water covers chunk columns that are not loaded.\n")
	TEXT("  < 0 = every unloaded column (default): no void anywhere around the loaded region\n")
	TEXT("  >= 0 = only columns reaching within this distance of the edge; deeper unloaded land stays empty, as on a plane\n")
	TEXT("Read when the world initializes."),
	ECVF_Default);

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

// ==================== Mesh-consumer throughput (A/B ceiling knobs) ====================
// The mesh consumer is bounded by three limits: (1) a hardcoded per-frame GPU mesh-DISPATCH cap
// (min(MaxChunksToLoadPerFrame, 2)), (2) the in-flight async-mesh cap, and (3) the pending-mesh
// (awaiting-render-submit) cap. During sustained traversal the mesh queue does not drain because
// mesh work arrives faster than this consumer completes it. These cvars expose all three so the
// true throughput ceiling can be found live: raising the dispatch cap trades render-thread frame
// time (each dispatch = an RDG graph + ~150KB upload + 4 compute passes) and game-thread launch
// cost (26-neighbor slice extraction) for a higher drain rate. Raise all three together — the
// dispatch loop also stops at the in-flight and pending caps. 0 = keep the shipped/config value.
static TAutoConsoleVariable<int32> CVarStreamMaxMeshDispatch(
	TEXT("voxel.Stream.MaxMeshDispatchPerFrame"),
	0,
	TEXT("Override the per-frame GPU mesh-dispatch cap (shipped: min(MaxChunksToLoadPerFrame, 2)). "
	     "0 = shipped behavior. Clamped [1,32]. Raising trades render/game-thread frame time for mesh "
	     "drain rate; raise MaxAsyncMeshTasks + MaxPendingMeshes alongside it or the in-flight caps bind."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarStreamMaxAsyncMeshTasks(
	TEXT("voxel.Stream.MaxAsyncMeshTasks"),
	0,
	TEXT("Override max concurrent async chunk-meshing tasks (mesh pipeline depth). 0 = use "
	     "configuration/-VoxelMaxAsyncMesh. Clamped [1,32]. Adaptive throttling still halves it under "
	     "frame pressure (disable with voxel.Stream.SpeedAdaptive 0 / -VoxelPinScheduler for a clean A/B)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarStreamMaxPendingMeshes(
	TEXT("voxel.Stream.MaxPendingMeshes"),
	0,
	TEXT("Override max meshes awaiting render submit before mesh dispatch stalls. 0 = use "
	     "configuration/-VoxelMaxPending. Clamped [2,64]. Raise together with MaxMeshDispatchPerFrame."),
	ECVF_Default);

// ==================== Stranded-job safety net (R23c) ====================
// An async generation or mesh job whose result never comes back leaves its chunk in Generating /
// Meshing for the rest of the session: it never unloads (only Loaded chunks unload), never remeshes
// (every later request for it is skipped while it is "in flight"), and holds one async slot. That is
// how a result dropped by the old drain loop produced a terrain chunk that stayed visible wherever
// the player went (VOXEL_QA_FOLLOWUPS_PLAN.md R23). The drains are fixed; this watchdog makes sure
// any other lost request (a GPU readback, a mesher bug) cannot strand a chunk either.
static TAutoConsoleVariable<float> CVarStreamMeshStuckSeconds(
	TEXT("voxel.Stream.MeshStuckSeconds"),
	10.0f,
	TEXT("Seconds an async chunk mesh or generation job may stay in flight before the watchdog logs a "
	     "warning, frees its slot and re-queues the chunk (a late result is then discarded). Must stay "
	     "well above real dispatch-to-complete times, including hitches and GPU readback queues; "
	     "voxel.Stream.AuditResidency reports the worst seen this session. 0 = disable the watchdog."),
	ECVF_Default);

// ==================== Seam-ownership pipeline ====================
// Every mesher meshes chunk interiors only and gives each chunk boundary to one single-owner seam job
// (Documentation/Research/SEAM_OWNERSHIP_ARCHITECTURE.md §2). FVoxelSeamRegistry tracks canonical
// (min-coordinate) ownership of every boundary, dirty-tracks seams off content-version and rendered-LOD
// changes, and schedules seam jobs by priority; the chunk manager runs them on workers and submits the
// results as per-owner seam buckets. Cubic joined in world modes review M5 (WM-C1), which deleted the
// legacy whole-chunk boundary path (neighbour slices, deferral, revalidation, neighbour re-mesh cascades).

static TAutoConsoleVariable<int32> CVarSeamNearChunkRadius(
	TEXT("voxel.Seam.NearChunkRadius"),
	3,
	TEXT("Chebyshev chunk radius within which a seam is prioritised at the near-correction tier (65). "
	     "Mirrors the near-field boundary-correction fast path in seam space."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSeamMaxSchedulePerTick(
	TEXT("voxel.Seam.MaxSchedulePerTick"),
	64,
	TEXT("Max dirty seams EXAMINED for scheduling per tick (0 = all). Bounds the per-tick seam "
	     "scheduler cost under a heavy streaming frontier; unexamined/not-yet-ready seams retry later."),
	ECVF_Default);

// Latency instrumentation for the seam scheduler. It was built to test (and confirmed) that the
// dirty-seam scan rotation, which used to hold every dirty seam, made a freshly-ready NEAR seam
// wait ~dirty/scan-rate (~300 ms) behind never-schedulable far ones; the rotation now holds only
// ready seams (scan wait p50 318 -> 18 ms) and the remaining near ready->done latency sits in the
// job queue / in-flight stage. It logs, every N ticks, dirty/ready->scheduled->completed windows
// for near seams alongside the dirty-set size and per-interval counters. Parse [SeamLat] lines
// from LogVoxelStreaming.
static TAutoConsoleVariable<int32> CVarSeamLogLatency(
	TEXT("voxel.Seam.LogLatency"),
	0,
	TEXT("Log seam-scheduler latency stats every N ticks (0 = off). Fields: dirty/nearDirty counts, "
	     "per-interval examined/requeued/scheduled/completed, and rolling p50/p95/max (ms) of "
	     "dirty->scheduled (near and far), dirty->completed and scheduled->completed (near)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSeamMaxJobsPerTick(
	TEXT("voxel.Seam.MaxJobsPerTick"),
	64,
	TEXT("Max seam jobs dispatched per tick (0 = only the in-flight cap, voxel.Seam.MaxInFlight, limits it)."),
	ECVF_Default);

// Seam-ownership meshing is UNCONDITIONAL (P4b retired the voxel.Seam.Meshing rollback flag; M5 brought cubic
// onto it): every configuration runs interior-only chunk meshes + single-owner seam jobs.
// Qualified vs legacy: thrash -96/-98%, catch-up 31.9->11.8s, GT MeshMs 8.5->0.6ms on GPU RHI
// (SEAM_OWNERSHIP_ARCHITECTURE.md §7.1); cubic in WORLD_MODES_REVIEW_PLAN.md §14.9.

// With a GPU DC main mesher, seam junctions are only bit-exact if interiors are CPU-meshed too
// (GPU QEF solves diverge from the CPU seam rings — visible cracks). 1 = route interior chunk
// meshes to the CPU SeamMesher while seam meshing is active (watertight). 0 = keep GPU interior
// meshes (junction cracks; A/B lever for measuring the GPU interior-meshing advantage).
static TAutoConsoleVariable<int32> CVarSeamCPUInteriorRouting(
	TEXT("voxel.Seam.CPUInteriorRouting"),
	1,
	TEXT("When seam meshing is active with a GPU DC mesher: 1 = interior chunk meshes run on the "
	     "CPU DC mesher (bit-exact seam junctions, default and the only supported setting).\n"
	     "0 is RETIRED and NON-FUNCTIONAL: the GPU DC interior path produces zero geometry "
	     "(Pass 3 readback race), so setting 0 leaves only the CPU seam ribbons and the terrain "
	     "disappears. Kept solely so the setting is discoverable rather than silently missing. "
	     "See SEAM_OWNERSHIP_ARCHITECTURE.md 7.2 option C."),
	ECVF_Default);

// Job-stage caps. Seam jobs are strip-sized and finish within a frame (worker p50 ~0.2-0.7 ms), so
// the in-flight cap behaves as a PER-TICK DRAIN BUDGET rather than a concurrency limit: a near
// chunk-arrival / LOD-shift burst of hundreds of ready seams drains at MaxInFlight jobs per tick.
// Measured with voxel.Seam.LogLatency (VoxelWorldsTest, 600 uu/s, 5 min each), near ready->visible:
//   caps  8/16: p50 355 / p95 624 ms, 22% of ticks drain-capped   (the old constexpr values)
//   caps 32/32: p50  70 / p95 116 ms,  3% capped
//   caps 64/64: p50  35 / p95  68 ms,  0% capped
// at an unchanged 17 ms average frame; the submit pass costs 0.011 ms per mesh (worst tick 2.5 ms).
// 64/64 is the default; both stay cvars so the trade can be re-measured on other hardware.
static TAutoConsoleVariable<int32> CVarSeamMaxInFlight(
	TEXT("voxel.Seam.MaxInFlight"),
	64,
	TEXT("Max async seam-mesh jobs in flight. Jobs finish within a frame, so this is effectively the "
	     "number of seam jobs dispatched per tick; it bounds how fast a burst of newly buildable "
	     "boundaries lands (8 = ~350 ms p50 near the player, 64 = ~35 ms, same frame time)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSeamMaxSubmitsPerTick(
	TEXT("voxel.Seam.MaxSubmitsPerTick"),
	64,
	TEXT("Max completed seam meshes submitted to the renderer per tick (each is a game-thread vertex "
	     "convert + render command, ~0.01 ms). Leftovers keep their in-flight slot, backpressuring dispatch."),
	ECVF_Default);

// Coordinated LOD transitions (R24): an LOD change swaps the interior with a crossfade, but its
// rebuilt seams land later (a few frames for one chunk, up to ~0.6 s while an LOD ring sweeps and
// its transitions merge) and, until then, the seams shown match the OLD interior while the fade is
// already showing the new one — the old rim and the new seam ring do not meet, and the player sees
// a brief opening at the chunk boundary. Holding the interior's fade at alpha 0 until its incident
// seams land, then fading interior and seams together, closes it. This caps the wait so a seam that
// never lands only delays detail.
static TAutoConsoleVariable<int32> CVarSeamHoldMs(
	TEXT("voxel.MeshFade.SeamHoldMs"),
	1000,
	TEXT("Max milliseconds an LOD-change interior swap waits (fully showing the old mesh) for its rebuilt "
	     "seams before fading in with them. A failsafe, not the normal release: a transition releases as soon "
	     "as its seams land (measured 0.2-0.6 s while an LOD ring sweeps, since the transitions of a sweep "
	     "merge). A release by this cap fades the interior before its seams and can open a gap. "
	     "0 = no hold (the interior fades at once and seams fade as they land)."),
	ECVF_Default);

// First appearances (R24 P3): beyond voxel.MeshFade.AppearMinDistance a new chunk fades in from
// nothing. Its seams land a tick or more later and would fade in behind it, leaving its boundary
// open while the interior is already showing; holding the new interior (invisible) until its ready
// seams land, under the same voxel.MeshFade.SeamHoldMs cap, fades them in together.
static TAutoConsoleVariable<int32> CVarAppearHold(
	TEXT("voxel.MeshFade.AppearHold"),
	1,
	TEXT("Hold a new chunk that fades in (voxel.MeshFade.Appear) until its ready seams land, then fade them in together "
	     "(capped by voxel.MeshFade.SeamHoldMs). 1 = on (default), 0 = the interior fades in at once and its seams follow."),
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
	TEXT("Minimum stride (1<<LODLevel) of a chunk's distance band for it to qualify for compression: its LOD "
	     "level, or with the LOD system off (cubic) the band its distance falls in. "
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
	TEXT("Budget: max fresh compress attempts per tick (uniform scan + general encode, ~3.5ms per 1MB chunk "
	     "in PIE). Async: encodes handed to workers; sync: encodes run on the game thread. "
	     "Free re-collapses/re-compresses and skips of already-evaluated chunks are not counted against this."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionAsync(
	TEXT("voxel.FarCompression.Async"),
	1,
	TEXT("1 = encode on a background worker from a copy of the chunk's array; the game thread installs the "
	     "result only if the array is unchanged (FChunkDescriptor::AsyncCompactTicket). 0 = encode on the game "
	     "thread inside the sweep, as before (for A/B)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionMaxInFlight(
	TEXT("voxel.FarCompression.MaxInFlight"),
	8,
	TEXT("Async encodes in flight at most. Each holds a copy of its chunk's array (1MB for a 64^3 chunk), "
	     "so this also caps the copies' memory."),
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

	// Seam-ownership registry: a plain C++ helper, always on (every mesher meshes through seam jobs).
	SeamRegistry = MakeUnique<FVoxelSeamRegistry>();
	SeamRegistry->SetEnabled(true);

	FarCompressResults = MakeShared<FFarCompressResultQueue, ESPMode::ThreadSafe>();
}

// Defaulted here (not in the header) so ~TUniquePtr<FVoxelSeamRegistry> sees the complete type.
UVoxelChunkManager::~UVoxelChunkManager() = default;

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

	RecoverLostRenderData();

	const double TickStartTime = FPlatformTime::Seconds();
	FVoxelTimingStats Timing;

	// === Adaptive Throttling: update smoothed frame time ===
	{
		const float FrameTimeMs = DeltaTime * 1000.0f;
		constexpr float Alpha = 0.1f;
		SmoothedFrameTimeMs = SmoothedFrameTimeMs + Alpha * (FrameTimeMs - SmoothedFrameTimeMs);

		// Track the lowest frame time actually observed, drifting slowly back up so a single
		// unusually fast frame can't pin the floor for the session. On a vsync-capped display
		// this settles at the present interval -- the fastest this loop can ever see.
		ObservedFrameFloorMs += DeltaTime * 0.5f;
		if (FrameTimeMs > 0.0f)
		{
			ObservedFrameFloorMs = FMath::Min(ObservedFrameFloorMs, FrameTimeMs);
		}
		ObservedFrameFloorMs = FMath::Clamp(ObservedFrameFloorMs, 1.0f, 100.0f);

		const float TargetFPS = Configuration ? Configuration->TargetFrameRate : 60.0f;
		// -VoxelPinScheduler forces non-adaptive so the override/config limits hold for the run.
		const bool bAdaptive = bSchedPinned ? false : (Configuration ? Configuration->bAdaptiveThrottling : true);
		const int32 CvarAsyncGen = CVarStreamMaxAsyncGenTasks.GetValueOnGameThread();
		const int32 ConfigMaxAsyncGen = (CvarAsyncGen > 0) ? FMath::Clamp(CvarAsyncGen, 1, 16)
			: (SchedOverrideAsyncGen >= 0) ? SchedOverrideAsyncGen : (Configuration ? Configuration->MaxAsyncGenerationTasks : 2);
		const int32 CvarAsyncMesh = CVarStreamMaxAsyncMeshTasks.GetValueOnGameThread();
		const int32 ConfigMaxAsync = (CvarAsyncMesh > 0) ? FMath::Clamp(CvarAsyncMesh, 1, 32)
			: (SchedOverrideAsyncMesh >= 0) ? SchedOverrideAsyncMesh : (Configuration ? Configuration->MaxAsyncMeshTasks : 4);
		const int32 ConfigMaxLODRemesh = (SchedOverrideLODRemesh >= 0) ? SchedOverrideLODRemesh : (Configuration ? Configuration->MaxLODRemeshPerFrame : 4);
		const int32 CvarPending = CVarStreamMaxPendingMeshes.GetValueOnGameThread();
		const int32 ConfigMaxPending = (CvarPending > 0) ? FMath::Clamp(CvarPending, 2, 64)
			: (SchedOverridePending >= 0) ? SchedOverridePending : (Configuration ? Configuration->MaxPendingMeshes : 4);

		if (bAdaptive && TargetFPS > 0.0f)
		{
			const float TargetMs = 1000.0f / TargetFPS;

			// Both thresholds are measured against a budget that can never be faster than the
			// frame times this machine actually produces. A target above the present rate makes
			// the RELEASE test unreachable and the throttle latches on for the rest of the
			// session: measured on a 60 Hz display, the frame-time floor is 16.667 ms while the
			// old release test demanded < 13.333 ms -- 0 of 339 sampled frames could satisfy it,
			// so one loading hitch halved generation permanently while FPS looked perfectly fine.
			const float BudgetMs = FMath::Max(TargetMs, ObservedFrameFloorMs);

			if (SmoothedFrameTimeMs > BudgetMs * 1.2f)
			{
				// Over budget: reduce generation INFLOW to relieve frame pressure. Two caps are
				// deliberately NOT reduced here:
				//   - the mesh CONSUMER caps (async-mesh / pending): A/B showed halving them
				//     throttles the drain exactly when the backlog is worst (an inverted
				//     throttle), slowing catch-up with no frame benefit.
				//   - MaxLODRemeshPerFrame: LOD remeshes are the visible-detail consumer for
				//     chunks that are already resident. Halving it delays terrain reaching full
				//     detail underneath the player without meaningfully reducing frame cost.
				EffectiveMaxAsyncGenerationTasks = FMath::Max(1, ConfigMaxAsyncGen / 2);
				EffectiveMaxAsyncMeshTasks = ConfigMaxAsync;
				EffectiveMaxLODRemeshPerFrame = ConfigMaxLODRemesh;
				EffectiveMaxPendingMeshes = ConfigMaxPending;
			}
			else if (SmoothedFrameTimeMs < BudgetMs * 1.05f)
			{
				// Back within budget: restore configured limits. The 1.05 margin sits above the
				// achievable floor (unlike the old 0.8), so this branch is actually reachable.
				EffectiveMaxAsyncGenerationTasks = ConfigMaxAsyncGen;
				EffectiveMaxAsyncMeshTasks = ConfigMaxAsync;
				EffectiveMaxLODRemeshPerFrame = ConfigMaxLODRemesh;
				EffectiveMaxPendingMeshes = ConfigMaxPending;
			}
			// else: between 105% and 120% of budget, keep current values (hysteresis band)
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
	// R24 P3: the renderer fades new geometry in only away from the viewer.
	if (MeshRenderer)
	{
		MeshRenderer->SetViewerPosition(CurrentViewerPosition);
	}

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

	// Update LOD strategy (always update for morph factor interpolation). Timed on its own: it rebuilds
	// the balanced LOD cache over the whole candidate volume and sat outside every section (§13.11).
	if (LODStrategy)
	{
		const double LODUpdateStart = FPlatformTime::Seconds();
		LODStrategy->Update(Context, DeltaTime);
		Timing.LODUpdateMs = static_cast<float>((FPlatformTime::Seconds() - LODUpdateStart) * 1000.0);
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
	// After both drains, so a result that landed this tick is never mistaken for a stuck job.
	CheckStuckAsyncJobs();
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
	if (UsesWaterTiles())
	{
		ProcessDirtyWaterTiles(8);
	}
	SubEnd = FPlatformTime::Seconds();
	Timing.RenderWaterTileMs = static_cast<float>((SubEnd - SubStart) * 1000.0);

	Timing.RenderSubmitMs = static_cast<float>((SubEnd - SectionStart) * 1000.0);

	// === Far ocean (WM-E1) ===
	// After this tick's generation results, unloads and water tiles: a column that just got data or a water tile
	// leaves the far ocean in the same tick, so the two waters never overlap.
	if (FarOceanGrid)
	{
		const double FarOceanStart = FPlatformTime::Seconds();
		UpdateFarOceanUnderside(CurrentViewerPosition);
		Timing.FarOceanRebuildCount = UpdateFarOceanGrid(false) ? 1 : 0;
		Timing.FarOceanMs = static_cast<float>((FPlatformTime::Seconds() - FarOceanStart) * 1000.0);
	}
	// A planet's sea surface shows its underside only while the viewer is in the sea (SP-E2).
	UpdatePlanetOceanUnderside(CurrentViewerPosition);

	// === Water propagation (bounded BFS per frame) ===
	if (WaterPropagation && WaterPropagation->HasPendingWork())
	{
		WaterPropagation->ProcessPropagation(512);
	}

	// === LOD level changes and morph factor updates ===
	SectionStart = FPlatformTime::Seconds();
	{
		// Pipeline idle: no generation or meshing work queued, in flight or awaiting upload.
		const bool bQueuesEmpty = GenerationQueue.Num() == 0
			&& AsyncGenerationInProgress.Num() == 0
			&& MeshingQueue.Num() == 0
			&& AsyncMeshingInProgress.Num() == 0
			&& PendingMeshQueue.Num() == 0;

		// LOD level evaluation (KI12): viewer movement, left-overs from the per-frame cap, or an idle
		// sweep. See FVoxelLODEvalSchedule for why it is not tied to queue state.
		if (LODEvalSchedule.ShouldEvaluate(bViewerChunkChanged, Context.ViewerPosition, bQueuesEmpty))
		{
			int32 Queued = 0;
			bool bCapReached = false;
			Timing.LODEvalCandidates = EvaluateLODLevelChanges(Context, Queued, bCapReached);
			Timing.LODEvalQueued = Queued;
			LODEvalSchedule.OnEvaluated(Context.ViewerPosition, Queued, bCapReached);
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

	// === Seam-ownership scheduler ===
	// Runs after the generation/meshing/LOD sections so this-tick residency and rendered-LOD changes
	// are already pushed into the registry. Timed into its own bench column (SeamMs) — the flip
	// qualification found the seam pipeline's GT cost was invisible inside TotalMs.
	SectionStart = FPlatformTime::Seconds();
	TickSeamScheduler();
	Timing.SeamMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

	// === Far-chunk voxel-data compression sweep ===
	// Runs after LOD evaluation so a chunk just queued for an LOD remesh (now dirty) is excluded.
	// Timed into its own bench column (FarCompressMs): it used to show only as unattributed TotalMs (G6).
	SectionStart = FPlatformTime::Seconds();
	ProcessFarCompressionSweep(Context, Timing);
	Timing.FarCompressMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);

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
		Timing.CollisionPrepMs = CollisionManager->LastPrepMs;
		Timing.CollisionApplyMs = CollisionManager->LastApplyMs;
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

	// Settings that run but give wrong results (tree template material ids, ...): say so at world start too,
	// since an editor session loads the asset long before Play.
	for (const FString& Warning : Configuration->GetSettingsWarnings())
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("World configuration %s: %s"), *Configuration->GetName(), *Warning);
	}

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
	if (MeshRenderer && UsesWaterTiles())
	{
		MeshRenderer->SetWaterMaterial(Configuration->WaterMeshMaterial);
	}

	// Disable LOD morphing for cubic mode - vertices cannot interpolate with hard edges
	if (Configuration->MeshingMode == EMeshingMode::Cubic && Configuration->bEnableLODMorphing)
	{
		Configuration->bEnableLODMorphing = false;
		UE_LOG(LogVoxelStreaming, Log, TEXT("LOD morphing disabled for cubic mode (hard-edged vertices cannot interpolate)"));
	}

	// Cubic runs without the LOD system (decision D7): greedy meshing is its answer to distance and
	// the cubic mesher never reads the LOD level, so LOD changes would only re-mesh and crossfade
	// identical geometry. IsLODEnabled() applies the rule for every reader, including the LOD
	// strategy below; the asset is not written, so the same config meshed smooth keeps its LOD.
	// The bands still apply as distances (far-band slab, far-chunk compression).
	if (Configuration->MeshingMode == EMeshingMode::Cubic && Configuration->bEnableLOD)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("LOD system disabled for cubic mode (bEnableLOD is ignored; greedy meshing replaces LOD)"));
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
	LODEvalSchedule.Reset();

	// Reset adaptive throttle state
	SmoothedFrameTimeMs = 16.67f;
	ObservedFrameFloorMs = 16.67f;
	bSubsystemsDeferred = false;
	EffectiveMaxAsyncGenerationTasks = (SchedOverrideAsyncGen >= 0) ? SchedOverrideAsyncGen : Configuration->MaxAsyncGenerationTasks;
	EffectiveMaxAsyncMeshTasks = (SchedOverrideAsyncMesh >= 0) ? SchedOverrideAsyncMesh : Configuration->MaxAsyncMeshTasks;
	EffectiveMaxLODRemeshPerFrame = (SchedOverrideLODRemesh >= 0) ? SchedOverrideLODRemesh : Configuration->MaxLODRemeshPerFrame;
	EffectiveMaxPendingMeshes = (SchedOverridePending >= 0) ? SchedOverridePending : Configuration->MaxPendingMeshes;
	LastTimingStats = FVoxelTimingStats();

	// Create generation components.
	// The analytic world mode matching config->WorldMode, so spawn / nav / POI / GetGeneratedSurfaceHeight use the
	// right terrain math for every mode. Built by the one factory every other consumer uses (WM-A1), with the biome
	// context set, so its continentalness-modulated height matches generation.
	WorldMode = FVoxelWorldModeFactory::Create(*Configuration);

	// -VoxelForceCPU (headless/benchmark, e.g. under -nullrhi where GPU compute can't dispatch) forces
	// BOTH the CPU generator and the CPU mesher. Evaluated once here and reused for the mesher below.
	const bool bForceCPU = FParse::Param(FCommandLine::Get(), TEXT("VoxelForceCPU"));

	// Select the voxel data generator. bUseGPUGeneration was historically a dead flag — this is where it
	// becomes real. The runtime cvar voxel.GPUGeneration.Enable can force CPU without an editor restart.
	// Every world mode generates on the GPU when allowed: planets too since spherical Phase 2 (SP-B4).
	const bool bUseGPUGen = Configuration->ShouldUseGPUGeneration() && !bForceCPU
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

	// Cubic boundary geometry lies entirely in face planes (WM-C1): its edge and corner seams would
	// only schedule jobs that mesh nothing, so the registry tracks face seams alone.
	if (SeamRegistry.IsValid())
	{
		SeamRegistry->SetFaceSeamsOnly(Configuration->MeshingMode == EMeshingMode::Cubic);
	}
	if (Mesher->GetMesherTypeName().Contains(TEXT("GPU")) && CVarSeamCPUInteriorRouting.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogVoxelStreaming, Log,
			TEXT("Seam meshing: chunk meshes route to the CPU seam mesher (GPU results are not bit-exact with the CPU seam geometry — junction cracks); the GPU mesher idles."));
	}

	// Clear pending mesh queue
	PendingMeshQueue.Empty();

	// Create edit manager
	EditManager = NewObject<UVoxelEditManager>(this);
	EditManager->Initialize(Configuration);

	// Subscribe to edit events - mark chunks dirty when edited
	EditManager->OnChunkEdited.AddLambda([this](const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius)
	{
		// The edit changed this chunk's LOGICAL voxel content, but it lives in the edit-manager
		// layer — the descriptor array is untouched, so nothing else advances ContentVersion.
		// Bump it before MarkChunkDirty so the seam registry restales against the NEW version and
		// the shared voxel-snapshot cache (seam jobs + async collision cooks key edit-merged
		// snapshots off ContentVersion) rebuilds instead of re-serving the pre-edit snapshot.
		if (FVoxelChunkState* EditedState = ChunkStates.Find(ChunkCoord))
		{
			EditedState->Descriptor.BumpContentVersion();
		}
		else
		{
			// Edit landed in an UNTRACKED chunk (e.g. building up into the all-air chunk above the
			// analytic streaming band). Force a streaming update so the edit-pinned load pass in
			// UpdateLoadDecisions picks it up this tick instead of waiting for viewer movement.
			bForceStreamingUpdate = true;
		}

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
	}

	bIsInitialized = true;

	// The hard boundary at the world's edge (WM-I5, D9), and the sea beyond it (WM-E1); a planet's sea surface (SP-E2)
	// and KillZ under it (SP-F5).
	RebuildWorldBoundary();
	RebuildFarOcean();
	RebuildPlanetOcean();
	ApplyPlanetKillZ();

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

	// Clear all chunks, water tiles, and seam meshes from renderer
	if (MeshRenderer)
	{
		MeshRenderer->ClearAllWaterTiles();
		MeshRenderer->ClearAllSeamMeshes();
		MeshRenderer->ClearAllChunks();
	}

	// Clear state
	ChunkStates.Empty();
	LoadedChunkCoords.Empty();
	// Seam-ownership: drop all tracked seams/jobs alongside the chunk state they mirror, and
	// discard the async seam pipeline's transient state (in-flight results will no-op via the
	// weak manager pointer; late results drain harmlessly next session).
	if (SeamRegistry.IsValid())
	{
		SeamRegistry->Reset();
	}
	SeamJobsInFlight.Empty();
	SeamSnapshotCache.Empty();
	SeamOwnerSlots.Empty();
	{
		FVoxelSeamTransitionHolds::FRelease Discarded; // the renderer is cleared above
		SeamHolds.ReleaseAll(Discarded);
	}
	SeamRedirtyAfterFlight.Empty();
	{
		FCompletedSeamMesh DiscardedSeamResult;
		while (CompletedSeamMeshQueue.Dequeue(DiscardedSeamResult)) {}
	}
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

	// Far compression: encodes still running finish into the old queue, which is freed with the last
	// of them. The descriptors that carried their tickets are gone with ChunkStates.
	FarCompressResults = MakeShared<FFarCompressResultQueue, ESPMode::ThreadSafe>();
	FarCompressJobsInFlight = 0;
	FarCompressBytesInFlight = 0;
	FarCompressScratchPool.Empty();

	// Reset streaming decision caching
	CachedViewerChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
	LastStreamingUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	LastLODUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	bForceStreamingUpdate = false;
	LODEvalSchedule.Reset();
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
	if (SeamMesher)
	{
		SeamMesher->Shutdown();
		SeamMesher.Reset();
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
	DestroyWorldBoundary();
	DestroyFarOcean();
	DestroyPlanetOcean();

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

			// Seam-ownership P0: an explicit dirty (voxel edit / forced remesh) may have changed the
			// chunk's content — restale its incident seams. ContentVersion is bumped by the edit itself.
			if (SeamRegistry.IsValid())
			{
				SeamRegistry->UpdateChunkContent(ChunkCoord, State->Descriptor.ContentVersion);
			}

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

TSharedPtr<const TArray<FVoxelData>> UVoxelChunkManager::GetEditMergedSnapshot(const FIntVector& ChunkCoord)
{
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!bIsInitialized || !State || !State->Descriptor.HasVoxelDataAvailable())
	{
		return nullptr;
	}
	return GetSeamVoxelSnapshot(ChunkCoord, *State);
}

bool UVoxelChunkManager::HasChunkVoxelData(const FIntVector& ChunkCoord) const
{
	const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	return State && State->Descriptor.HasVoxelDataAvailable();
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

bool UVoxelChunkManager::QueryEditMergedSurfacePoint(const FVector& WorldPos, FVoxelSurfaceSample& OutSample) const
{
	const IVoxelWorldMode* WM = GetWorldMode();
	if (!bIsInitialized || !Configuration || !WM)
	{
		return false;
	}

	if (WM->GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		// Heightmap modes: the column at (X, Y), unchanged.
		float Height = 0.0f;
		if (!QueryEditMergedSurface(WorldPos.X, WorldPos.Y, Height, OutSample.Normal, OutSample.SlopeDegrees, OutSample.MaterialID, OutSample.BiomeID))
		{
			return false;
		}
		OutSample.Height = Height;
		OutSample.Position = FVector(WorldPos.X, WorldPos.Y, Height);
		OutSample.Up = FVector::UpVector;
		OutSample.Altitude = WM->GetAltitude(OutSample.Position);
		return true;
	}

	const float VoxelSize = Configuration->VoxelSize;
	const int32 ChunkSize = Configuration->ChunkSize;
	if (VoxelSize <= 0.0f || ChunkSize <= 0)
	{
		return false;
	}

	// The analytic surface along the direction locates the line and makes the near/far decision, as the analytic height
	// does for the column.
	const FVector& Origin = Configuration->WorldOrigin;
	const FVoxelTerrainSurfacePoint Estimate = WM->GetSurfacePoint(WorldPos, Configuration->NoiseParams);
	const FVector Up = WM->GetUpVector(Estimate.Position);
	if (!IsChunkLoaded(FVoxelCoordinates::WorldToChunk(Estimate.Position - Origin, ChunkSize, VoxelSize)))
	{
		return false;
	}

	// A digital line of edit-merged voxels along local up through the estimate's voxel, one chunk either side (covers
	// dig-down / build-up). Voxel I's density sample sits at Origin + I * VoxelSize; read it from inside its cell.
	const FVector RelVoxel = (Estimate.Position - Origin) / VoxelSize;
	const FIntVector Start(FMath::FloorToInt32(RelVoxel.X), FMath::FloorToInt32(RelVoxel.Y), FMath::FloorToInt32(RelVoxel.Z));
	const FVoxelDigitalLine Line = FVoxelDigitalLine::Make(Up);
	const int32 Window = ChunkSize;
	const int32 Count = 2 * Window + 1;
	TArray<FIntVector, TInlineAllocator<160>> Indices;
	TArray<FVoxelData, TInlineAllocator<160>> Voxels;
	Indices.SetNumUninitialized(Count);
	Voxels.SetNumUninitialized(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		Indices[i] = Line.Step(Start, i - Window);
		Voxels[i] = GetEditMergedVoxelAtWorldPosition(Origin + (FVector(Indices[i]) + FVector(0.5)) * VoxelSize);
	}

	// Coarse, from the top down: the highest solid voxel with air above it along the line (ExtractSurfaceFromColumn's
	// rule), one voxel read per step.
	int32 Found = INDEX_NONE;
	for (int32 i = Count - 2; i >= 0; --i)
	{
		if (Voxels[i].IsSolid() && Voxels[i + 1].IsAir())
		{
			Found = i;
			break;
		}
	}
	if (Found == INDEX_NONE)
	{
		return false;
	}

	// Fine: the trilinear edit-merged density on the true up-line through the queried direction, around the coarse
	// crossing. The digital line strays up to a voxel sideways, which on a slope moves the crossing; the up-line does not.
	auto VoxelDensity = [this, &Origin, VoxelSize](const FIntVector& I) -> float
	{
		return static_cast<float>(GetEditMergedVoxelAtWorldPosition(Origin + (FVector(I) + FVector(0.5)) * VoxelSize).Density);
	};
	auto Trilinear = [&VoxelDensity, &Origin, VoxelSize](const FVector& P) -> float
	{
		const FVector F = (P - Origin) / VoxelSize;
		const FIntVector I0(FMath::FloorToInt32(F.X), FMath::FloorToInt32(F.Y), FMath::FloorToInt32(F.Z));
		const FVector T = F - FVector(I0);
		float Result = 0.0f;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FIntVector O(Corner & 1, (Corner >> 1) & 1, (Corner >> 2) & 1);
			const double W = (O.X ? T.X : 1.0 - T.X) * (O.Y ? T.Y : 1.0 - T.Y) * (O.Z ? T.Z : 1.0 - T.Z);
			Result += static_cast<float>(W) * VoxelDensity(I0 + O);
		}
		return Result;
	};
	const float Iso = static_cast<float>(VOXEL_SURFACE_THRESHOLD);
	const double TopT = FVector::DotProduct(Origin + FVector(Indices[Found + 1]) * VoxelSize - Estimate.Position, Up) + 1.5 * VoxelSize;
	const double BottomT = FVector::DotProduct(Origin + FVector(Indices[Found]) * VoxelSize - Estimate.Position, Up) - 1.5 * VoxelSize;
	const double Step = 0.25 * VoxelSize;
	double HitT = TNumericLimits<double>::Max();
	float AboveDensity = Trilinear(Estimate.Position + Up * TopT);
	for (double T = TopT - Step; T >= BottomT && AboveDensity < Iso; T -= Step)
	{
		const float Density = Trilinear(Estimate.Position + Up * T);
		if (Density >= Iso)
		{
			HitT = T + Step * (Density - Iso) / FMath::Max(Density - AboveDensity, KINDA_SMALL_NUMBER);
			break;
		}
		AboveDensity = Density;
	}
	if (HitT == TNumericLimits<double>::Max())
	{
		// The fine walk found no crossing (an edge case of the bracket): keep the coarse crossing, put on the up-line.
		const float D0 = static_cast<float>(Voxels[Found].Density);
		const float D1 = static_cast<float>(Voxels[Found + 1].Density);
		const float Denom = D1 - D0;
		const float T = FMath::Abs(Denom) > KINDA_SMALL_NUMBER ? FMath::Clamp((Iso - D0) / Denom, 0.0f, 1.0f) : 0.5f;
		const FVector Crossing = Origin + FMath::Lerp(FVector(Indices[Found]), FVector(Indices[Found + 1]), static_cast<double>(T)) * VoxelSize;
		HitT = FVector::DotProduct(Crossing - Estimate.Position, Up);
	}

	OutSample.Position = Estimate.Position + Up * HitT;
	OutSample.Up = Up;
	OutSample.MaterialID = Voxels[Found].MaterialID;
	OutSample.BiomeID = Voxels[Found].BiomeID;

	// The normal from the trilinear edit-merged density gradient at the surface (central differences half a voxel either
	// side on the world axes), turned toward local up for a top surface.
	const FVector& P = OutSample.Position;
	const double H = 0.5 * VoxelSize;
	const FVector Gradient(
		Trilinear(P + FVector(H, 0.0, 0.0)) - Trilinear(P - FVector(H, 0.0, 0.0)),
		Trilinear(P + FVector(0.0, H, 0.0)) - Trilinear(P - FVector(0.0, H, 0.0)),
		Trilinear(P + FVector(0.0, 0.0, H)) - Trilinear(P - FVector(0.0, 0.0, H)));
	OutSample.Normal = (-Gradient).GetSafeNormal();
	if (OutSample.Normal.IsNearlyZero())
	{
		OutSample.Normal = Up;
	}
	else if (FVector::DotProduct(OutSample.Normal, Up) < 0.0)
	{
		OutSample.Normal = -OutSample.Normal;
	}
	OutSample.SlopeDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(OutSample.Normal, Up), -1.0, 1.0)));
	OutSample.Altitude = WM->GetAltitude(OutSample.Position);
	OutSample.Height = OutSample.Altitude;
	return true;
}

FVector UVoxelChunkManager::GetChunkLocalUp(const FIntVector& ChunkCoord) const
{
	if (!WorldMode || !Configuration || Configuration->WorldMode != EWorldMode::SphericalPlanet)
	{
		return FVector::UpVector;
	}
	const double ChunkWorldSize = static_cast<double>(Configuration->ChunkSize) * Configuration->VoxelSize;
	return WorldMode->GetUpVector(Configuration->WorldOrigin + (FVector(ChunkCoord) + FVector(0.5)) * ChunkWorldSize);
}

FVector UVoxelChunkManager::GetGeneratedSurfacePoint(const FVector& WorldPos) const
{
	const IVoxelWorldMode* WM = GetWorldMode();
	if (!bIsInitialized || !WM || !Configuration)
	{
		return WorldPos;
	}
	if (WM->GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		return FVector(WorldPos.X, WorldPos.Y, GetGeneratedSurfaceHeight(WorldPos.X, WorldPos.Y));
	}
	return WM->GetSurfacePoint(WorldPos, Configuration->NoiseParams).Position;
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
		return LODStrategy ? LODStrategy->GetHorizonDistance(Center, CurrentViewerPosition) : static_cast<float>(FVector::Dist(Center, CurrentViewerPosition));
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

	// Copies held by async far-compression encodes, in flight or pooled for the next dispatch.
	Stats.VoxelDataBytes += FarCompressBytesInFlight;
	for (const TArray<FVoxelData>& Scratch : FarCompressScratchPool)
	{
		Stats.VoxelDataBytes += static_cast<int64>(Scratch.GetAllocatedSize());
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
		UE_LOG(LogVoxelStreaming, Verbose, TEXT("BuildQueryContext: Source=%s, Pos=(%.0f, %.0f, %.0f), Found=%s"),
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

	// Edit-pinned loading: chunks whose only content is EDITS are invisible to the LOD strategy —
	// its vertical range and surface-slab culls derive from the ANALYTIC terrain, so a chunk holding
	// a player build above the surface band never qualifies on its own. Left unloaded, its edits
	// never generate/mesh and its incident seams can't schedule (participant not resident): build
	// tops get cut off flat at the chunk boundary. Pin-load every edited chunk within the LOAD
	// radius (the view edge) — pinning out to the unload radius loaded an edited chunk on its own
	// beyond the rest of the terrain (R23e). UpdateUnloadDecisions keeps a pinned chunk resident out
	// to the unload radius, so the 1.0-1.2x band is hysteresis, as for every other chunk.
	if (EditManager)
	{
		TArray<FIntVector> EditedChunks;
		EditManager->GetEditedChunkCoords(EditedChunks);
		if (EditedChunks.Num() > 0)
		{
			const float LoadDistance = LODStrategy->GetLoadDistance();
			const float PinDistance = (LoadDistance > 0.0f) ? LoadDistance : LODStrategy->GetUnloadDistance();
			const float ChunkWorldSize = Configuration->GetChunkWorldSize();
			for (const FIntVector& Coord : EditedChunks)
			{
				if (GetChunkState(Coord) != EChunkState::Unloaded)
				{
					continue;
				}
				const FVector ChunkCenter = Configuration->WorldOrigin
					+ FVector(Coord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
				const float PinCandidateDistance = LODStrategy->GetHorizonDistance(ChunkCenter, Context.ViewerPosition);
				if (PinCandidateDistance > PinDistance)
				{
					continue; // Too far — re-pins automatically when the viewer returns.
				}
				if (ChunksAddedThisFrame >= MaxChunksToAddPerFrame)
				{
					++ChunksRemaining;
					continue;
				}

				FChunkLODRequest Request;
				Request.ChunkCoord = Coord;
				Request.LODLevel = LODStrategy->GetLODForChunk(Coord, Context);
				Request.Priority = 900.0f; // The player is actively building here — near-dirty priority.

				FVoxelChunkState& State = GetOrCreateChunkState(Coord);
				State.LODLevel = Request.LODLevel;
				State.Priority = Request.Priority;

				if (AddToGenerationQueue(Request))
				{
					SetChunkState(Coord, EChunkState::PendingGeneration);
					++ChunksAddedThisFrame;
					UE_LOG(LogVoxelStreaming, Log, TEXT("Edit-pinned chunk (%d,%d,%d) queued for load at %.0f uu (pin radius %.0f; outside analytic streaming bounds)"),
						Coord.X, Coord.Y, Coord.Z, PinCandidateDistance, PinDistance);
				}
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

	// Every out-of-range chunk, farthest first (cheap operation - just iterates loaded chunks).
	TArray<FIntVector> Candidates;
	LODStrategy->GetChunksToUnload(Candidates, LoadedChunkCoords, Context);
	if (Candidates.Num() == 0)
	{
		return;
	}

	// Veto first, cap second (R23d): a candidate this manager refuses must not use up the per-frame
	// budget, or a few refused chunks early in the loaded set's stable order would stop every later
	// chunk from ever unloading.
	const float EditPinDistance = LODStrategy->GetUnloadDistance();
	const float ChunkWorldSize = Configuration ? Configuration->GetChunkWorldSize() : 0.0f;
	auto CanUnload = [this, &Context, EditPinDistance, ChunkWorldSize](const FIntVector& ChunkCoord) -> bool
	{
		// Only settled chunks unload here: anything mid-pipeline finishes first, and PendingUnload
		// chunks are already queued.
		if (GetChunkState(ChunkCoord) != EChunkState::Loaded)
		{
			return false;
		}

		// Edit-pinned: the strategy's analytic culls (vertical band / surface slab) would evict a
		// chunk that exists only because of its edit layer, immediately undoing the edit-pinned
		// load in UpdateLoadDecisions — the two would flap load/unload forever. Keep edited chunks
		// resident until they leave the distance-based unload radius.
		if (EditManager && ChunkWorldSize > 0.0f && EditManager->ChunkHasEdits(ChunkCoord))
		{
			const FVector ChunkCenter = Configuration->WorldOrigin
				+ FVector(ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
			if (LODStrategy->GetHorizonDistance(ChunkCenter, Context.ViewerPosition) <= EditPinDistance)
			{
				return false;
			}
		}
		return true;
	};

	TArray<FIntVector> ChunksToUnload;
	IVoxelLODStrategy::SelectUnloadCandidates(Candidates, CanUnload, Context.MaxChunksToUnloadPerFrame, ChunksToUnload);

	// Add to unload queue with O(1) duplicate check
	for (const FIntVector& ChunkCoord : ChunksToUnload)
	{
		if (AddToUnloadQueue(ChunkCoord))
		{
			SetChunkState(ChunkCoord, EChunkState::PendingUnload);
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

	// Requests for a chunk that still owns an in-flight generation slot, re-queued after the loop.
	// Dropping them (the old behaviour) left the chunk in PendingGeneration with no queue entry for
	// good. Unloads and the stuck-job watchdog release the slot, so this should not occur; it is a
	// guard, bounded per frame so a queue of such requests cannot spin.
	TArray<FChunkLODRequest> DeferredInFlight;
	constexpr int32 MaxDeferredInFlight = 48;

	while (GenerationQueue.Num() > 0 && ProcessedCount < MaxChunks && DeferredInFlight.Num() < MaxDeferredInFlight &&
	       AsyncGenerationInProgress.Num() < EffectiveMaxAsyncGenerationTasks)
	{
		// Skip chunks already being generated asynchronously
		if (AsyncGenerationInProgress.Contains(GenerationQueue.Last().ChunkCoord))
		{
			const FChunkLODRequest InFlightRequest = GenerationQueue.Last();
			GenerationQueueSet.Remove(InFlightRequest.ChunkCoord);
			GenerationQueue.Pop(EAllowShrinking::No);
			if (GetChunkState(InFlightRequest.ChunkCoord) == EChunkState::PendingGeneration)
			{
				DeferredInFlight.Add(InFlightRequest);
			}
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
		GenRequest.LODLevel = (Configuration->IsLODEnabled()) ? Request.LODLevel : 0;
		GenRequest.ChunkSize = Configuration->ChunkSize;
		GenRequest.VoxelSize = Configuration->VoxelSize;
		GenRequest.NoiseParams = Configuration->NoiseParams;
		// World mode, origin, terrain params and the island / planet params, from the one builder (WM-A1).
		FVoxelWorldModeFactory::FillGenerationRequest(*Configuration, GenRequest);

		// Biome configuration (contains biome definitions, blend settings, height rules)
		GenRequest.bEnableBiomes = Configuration->bEnableBiomes;
		GenRequest.BiomeConfiguration = Configuration->BiomeConfiguration;

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

	for (const FChunkLODRequest& Deferred : DeferredInFlight)
	{
		AddToGenerationQueue(Deferred);
	}
}

// ==================== World Bounds and Local Frame (WM-A2) ====================

bool UVoxelChunkManager::IsInsideWorld(double WorldX, double WorldY) const
{
	return WorldMode ? WorldMode->IsInsideWorld(WorldX, WorldY) : true;
}

FVoxelWorldBounds UVoxelChunkManager::GetWorldBounds() const
{
	return WorldMode ? WorldMode->GetWorldBounds() : FVoxelWorldBounds();
}

FVector UVoxelChunkManager::GetUpVectorAt(const FVector& WorldPos) const
{
	return WorldMode ? WorldMode->GetUpVector(WorldPos) : FVector::UpVector;
}

float UVoxelChunkManager::GetAltitudeAt(const FVector& WorldPos) const
{
	return WorldMode ? WorldMode->GetAltitude(WorldPos) : static_cast<float>(WorldPos.Z);
}

FVoxelSurfaceCoord UVoxelChunkManager::WorldToSurface(const FVector& WorldPos) const
{
	return WorldMode ? WorldMode->WorldToSurface(WorldPos) : FVoxelSurfaceCoord(0, FVector2D(WorldPos.X, WorldPos.Y));
}

FVector UVoxelChunkManager::SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const
{
	return WorldMode ? WorldMode->SurfaceToWorld(Coord, Altitude) : FVector(Coord.UV.X, Coord.UV.Y, Altitude);
}

FVoxelTerrainSurfacePoint UVoxelChunkManager::GetSurfacePointAt(const FVector& WorldPos) const
{
	if (!WorldMode || !Configuration)
	{
		FVoxelTerrainSurfacePoint Point;
		Point.Position = WorldPos;
		return Point;
	}
	return WorldMode->GetSurfacePoint(WorldPos, Configuration->NoiseParams);
}

FVector UVoxelChunkManager::GetPlanetSpawnPosition() const
{
	if (!Configuration)
	{
		return FVector::ZeroVector;
	}
	if (!WorldMode || Configuration->WorldMode != EWorldMode::SphericalPlanet)
	{
		return Configuration->WorldOrigin;
	}
	// The configuration's point gives the direction; the terrain along it is the world mode's (SP-B11).
	const FVoxelTerrainSurfacePoint Surface = GetSurfacePointAt(Configuration->GetPlanetSpawnPosition());
	return Surface.Position + GetUpVectorAt(Surface.Position) * Configuration->PlanetSpawnAltitude;
}

FVoxelSeaSurface UVoxelChunkManager::GetSeaSurface(bool bOriginRelativePlane) const
{
	if (!Configuration)
	{
		return FVoxelSeaSurface();
	}
	return bOriginRelativePlane ? FVoxelSeaSurface::FromConfigOriginRelative(*Configuration) : FVoxelSeaSurface::FromConfig(*Configuration);
}

float UVoxelChunkManager::GetSeaDepthAt(const FVector& WorldPos) const
{
	const FVoxelSeaSurface Sea = GetSeaSurface(/*bOriginRelativePlane=*/true);
	return Sea.bEnabled ? static_cast<float>(Sea.GetDepth(WorldPos)) : -1.0e9f;
}

double UVoxelChunkManager::ComputePlanetKillZ(double CurrentKillZ, const FVoxelWorldBounds& Bounds, double Margin)
{
	if (Bounds.Shape != EVoxelWorldBoundsShape::Shell)
	{
		return CurrentKillZ;
	}
	const double Below = Bounds.Center.Z - Bounds.OuterRadius - FMath::Max(Margin, 0.0);
	return FMath::Min(CurrentKillZ, Below);
}

void UVoxelChunkManager::ApplyPlanetKillZ()
{
	UWorld* World = GetWorld();
	AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
	if (!Settings || !WorldMode || !Configuration || Configuration->WorldMode != EWorldMode::SphericalPlanet)
	{
		return;
	}
	// The engine's KillZ is a Z plane: a planet bigger than it reaches would lose its southern hemisphere to it. Below the
	// planet it never touches the surface, and bodies that leave the planet along world -Z are still removed.
	const double NewKillZ = ComputePlanetKillZ(Settings->KillZ, WorldMode->GetWorldBounds(), Configuration->GetChunkWorldSize());
	if (NewKillZ < Settings->KillZ)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Planet: KillZ moved from %.0f to %.0f, below the planet (centre Z %.0f, outer radius %.0f)."),
			Settings->KillZ, NewKillZ, WorldMode->GetWorldBounds().Center.Z, WorldMode->GetWorldBounds().OuterRadius);
		Settings->KillZ = static_cast<float>(NewKillZ);
	}
}

FVoxelWorldFingerprint UVoxelChunkManager::GetWorldFingerprint() const
{
	return Configuration ? FVoxelWorldModeFactory::MakeFingerprint(*Configuration) : FVoxelWorldFingerprint();
}

double UVoxelChunkManager::GetDistanceToWorldEdge(double WorldX, double WorldY) const
{
	return WorldMode ? WorldMode->GetWorldBounds().GetDistanceToEdgeXY(WorldX, WorldY) : TNumericLimits<double>::Max();
}

// ==================== World Boundary (WM-I5) ====================

namespace VoxelWorldBoundaryPrivate
{
	/** Walls 5 m thick: a sweep cannot tunnel through, and the outer side never matters */
	constexpr double WallThickness = 500.0;

	/** A circle's chords stay within 50 uu of R + F (63 walls for island S, 158 for M, 223 for L) */
	constexpr double MaxChordError = 50.0;

	/** Height of the walls above the highest terrain: higher than a pawn can climb, build or jump */
	constexpr double HeightAboveTerrain = 50000.0;

	/** Keep a mode's "no bound" height range (IVoxelWorldMode's default) from making boxes millions of km tall */
	constexpr double MaxAbsZ = 2.0e6;
}

void UVoxelChunkManager::RebuildWorldBoundary()
{
	using namespace VoxelWorldBoundaryPrivate;
	DestroyWorldBoundary();

	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!Configuration || !WorldMode || !World || !World->IsGameWorld() || !Owner || !Configuration->bWorldBoundary)
	{
		return;
	}
	const FVoxelWorldBounds Bounds = WorldMode->GetWorldBounds();
	const double BottomZ = FMath::Max(static_cast<double>(Bounds.MinZ) - Configuration->GetChunkWorldSize(), -MaxAbsZ);
	const double TopZ = FMath::Min(static_cast<double>(Bounds.MaxZ) + HeightAboveTerrain, MaxAbsZ);
	TArray<FVoxelWorldBoundaryWall> Walls;
	FVoxelWorldBoundary::BuildWalls(Bounds, WallThickness, MaxChordError, BottomZ, TopZ, Walls);
	if (Walls.Num() == 0)
	{
		return;
	}

	const bool bCollision = CVarWorldBoundary.GetValueOnGameThread() != 0;
	WorldBoundaryWalls.Reserve(Walls.Num());
	for (int32 i = 0; i < Walls.Num(); ++i)
	{
		const FVoxelWorldBoundaryWall& Wall = Walls[i];
		UBoxComponent* Box = NewObject<UBoxComponent>(Owner, *FString::Printf(TEXT("VoxelWorldBoundary_%d"), i), RF_Transient);
		Box->SetUsingAbsoluteLocation(true);
		Box->SetUsingAbsoluteRotation(true);
		Box->SetUsingAbsoluteScale(true);
		Box->SetupAttachment(Owner->GetRootComponent());
		Box->SetBoxExtent(Wall.HalfExtent, /*bUpdateOverlaps=*/false);
		Box->SetHiddenInGame(true);
		Box->SetCanEverAffectNavigation(false);
		Box->SetGenerateOverlapEvents(false);
		// Blocks everything that moves (pawns, NPCs, ragdolls, dropped items); traces for sight, interaction and the
		// camera pass, so nothing aimed at the sea stops on an invisible wall.
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		Box->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		Box->SetCollisionEnabled(bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		Box->SetWorldLocationAndRotation(Wall.Center, Wall.Rotation);
		Box->RegisterComponent();
		WorldBoundaryWalls.Add(Box);
	}

	UE_LOG(LogVoxelStreaming, Log,
		TEXT("World boundary: %d wall(s) at the world's edge (%s, centre (%.0f, %.0f), half extent (%.0f, %.0f)), Z %.0f to %.0f, collision %s"),
		WorldBoundaryWalls.Num(), *StaticEnum<EVoxelWorldBoundsShape>()->GetNameStringByValue(static_cast<int64>(Bounds.Shape)),
		Bounds.Center.X, Bounds.Center.Y, Bounds.HalfExtent.X, Bounds.HalfExtent.Y, BottomZ, TopZ, bCollision ? TEXT("on") : TEXT("off"));
}

void UVoxelChunkManager::DestroyWorldBoundary()
{
	for (UBoxComponent* Box : WorldBoundaryWalls)
	{
		if (IsValid(Box))
		{
			Box->DestroyComponent();
		}
	}
	WorldBoundaryWalls.Reset();
}

void UVoxelChunkManager::SetWorldBoundaryCollisionEnabled(bool bEnabled)
{
	for (UBoxComponent* Box : WorldBoundaryWalls)
	{
		if (IsValid(Box))
		{
			Box->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
		}
	}
	if (WorldBoundaryWalls.Num() > 0)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("World boundary: %d wall(s), collision %s"), WorldBoundaryWalls.Num(), bEnabled ? TEXT("on") : TEXT("off"));
	}
}

// ==================== Far Ocean (WM-E1) ====================

namespace VoxelFarOceanManagerPrivate
{
	/** Hand a mesh to a procedural mesh section (no collision). Clears the section when the mesh is empty. */
	void SetSection(UProceduralMeshComponent* Component, int32 Section, const FChunkMeshData& Mesh, UMaterialInterface* Material)
	{
		if (!Component)
		{
			return;
		}
		if (Mesh.Positions.Num() == 0 || Mesh.Indices.Num() == 0)
		{
			Component->ClearMeshSection(Section);
			return;
		}
		const int32 N = Mesh.Positions.Num();
		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FVector2D> UV1;
		TArray<FProcMeshTangent> Tangents;
		Vertices.SetNumUninitialized(N);
		Normals.SetNumUninitialized(N);
		UV0.SetNumUninitialized(N);
		UV1.SetNumUninitialized(N);
		Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), N);
		for (int32 i = 0; i < N; ++i)
		{
			Vertices[i] = FVector(Mesh.Positions[i]);
			Normals[i] = FVector(Mesh.Normals[i]);
			UV0[i] = FVector2D(Mesh.UVs[i]);
			UV1[i] = FVector2D(Mesh.UV1s[i]);
		}
		TArray<int32> Triangles;
		Triangles.SetNumUninitialized(Mesh.Indices.Num());
		for (int32 i = 0; i < Mesh.Indices.Num(); ++i)
		{
			Triangles[i] = static_cast<int32>(Mesh.Indices[i]);
		}
		const TArray<FVector2D> NoUV;
		Component->CreateMeshSection(Section, Vertices, Triangles, Normals, UV0, UV1, NoUV, NoUV, Mesh.Colors, Tangents,
			/*bCreateCollision=*/false);
		if (Material)
		{
			Component->SetMaterial(Section, Material);
		}
	}

	UProceduralMeshComponent* MakeComponent(AActor* Owner, const TCHAR* Name, const FVector& Location)
	{
		UProceduralMeshComponent* Component = NewObject<UProceduralMeshComponent>(Owner, Name, RF_Transient);
		Component->SetUsingAbsoluteLocation(true);
		Component->SetUsingAbsoluteRotation(true);
		Component->SetUsingAbsoluteScale(true);
		Component->SetupAttachment(Owner->GetRootComponent());
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCastShadow(false);
		Component->bUseAsyncCooking = false;
		Component->SetWorldLocationAndRotation(Location, FRotator::ZeroRotator);
		Component->RegisterComponent();
		return Component;
	}
}

uint8 UVoxelChunkManager::ResolveFarSeabedMaterial(const FIslandBowlParams& Island) const
{
	// The material generation gives the seabed a little past the edge, the most common one around the island: the
	// same surface pipeline as the map and scatter (FVoxelSurfaceQuery).
	if (!Configuration)
	{
		return 0;
	}
	const FVoxelBiomeSnapshot Snapshot = FVoxelBiomeSnapshot::FromConfig(Configuration->BiomeConfiguration);
	const float Distance = FMath::Clamp(Island.SeabedSlopeWidth * 0.5f, 500.0f, 20000.0f);
	TMap<uint8, int32> Votes;
	constexpr int32 Samples = 48;
	for (int32 i = 0; i < Samples; ++i)
	{
		const float Angle = UE_TWO_PI * (i + 0.5f) / Samples;
		const FVector2D Dir(FMath::Cos(Angle), FMath::Sin(Angle));
		// Out from the centre to the footprint's edge (a rectangle's too: its larger-axis distance grows along the ray).
		float Edge = Island.GetTotalExtent();
		if (Island.Shape == EIslandShape::Rectangle)
		{
			const float SX = FMath::Abs(Dir.X) > KINDA_SMALL_NUMBER ? Island.GetTotalExtentX() / FMath::Abs(Dir.X) : TNumericLimits<float>::Max();
			const float SY = FMath::Abs(Dir.Y) > KINDA_SMALL_NUMBER ? Island.GetTotalExtentY() / FMath::Abs(Dir.Y) : TNumericLimits<float>::Max();
			Edge = FMath::Min(SX, SY);
		}
		const FVector2D P = FVector2D(Island.CenterX, Island.CenterY) + Dir * (Edge + Distance);
		const float Height = FIslandBowlWorldMode::GetSeabedHeight(
			FIslandBowlWorldMode::GetDistancePastEdge(P.X, P.Y, Island), Island);
		uint8 Material = 0;
		if (Configuration->bEnableBiomes && Snapshot.bIsValid)
		{
			uint8 Biome = 0;
			FVoxelSurfaceQuery::QuerySurfaceConditions(P.X, P.Y, Height, Configuration->VoxelSize, Snapshot,
				Configuration->NoiseParams.Seed, Configuration->bEnableWaterLevel, Configuration->WaterLevel, Material, Biome);
		}
		else if (WorldMode)
		{
			Material = WorldMode->GetMaterialAtDepth(FVector(P.X, P.Y, Height), Height, 0.0f);
		}
		++Votes.FindOrAdd(Material);
	}
	uint8 Best = 0;
	int32 BestVotes = -1;
	for (const TPair<uint8, int32>& Vote : Votes)
	{
		if (Vote.Value > BestVotes)
		{
			Best = Vote.Key;
			BestVotes = Vote.Value;
		}
	}
	return Best;
}

void UVoxelChunkManager::RebuildFarOcean()
{
	using namespace VoxelFarOceanManagerPrivate;
	DestroyFarOcean();

	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	// Visual only: a dedicated server draws nothing.
	if (!Configuration || !WorldMode || !World || !World->IsGameWorld() || !Owner || World->GetNetMode() == NM_DedicatedServer
		|| Configuration->WorldMode != EWorldMode::IslandBowl || !Configuration->bIslandFarOcean)
	{
		return;
	}
	const FVoxelWorldBounds Bounds = WorldMode->GetWorldBounds();
	if (Bounds.Shape != EVoxelWorldBoundsShape::Circle && Bounds.Shape != EVoxelWorldBoundsShape::Rectangle)
	{
		return;
	}
	const double StartTime = FPlatformTime::Seconds();
	const FIslandBowlParams Island = FVoxelWorldModeFactory::MakeIslandParams(*Configuration);
	FarOceanSettings = FVoxelFarOceanSettings();
	FarOceanSettings.Bounds = Bounds;
	FarOceanSettings.GridOrigin = FVector2D(Configuration->WorldOrigin.X, Configuration->WorldOrigin.Y);
	FarOceanSettings.ColumnSize = Configuration->GetChunkWorldSize();
	FarOceanSettings.VoxelSize = Configuration->VoxelSize;
	FarOceanSettings.WaterZ = Configuration->WaterLevel; // where FVoxelWaterMesher puts the water tiles
	FarOceanSettings.OuterDistance = Configuration->IslandFarOceanDistance;
	FarOceanSettings.SeabedSlopeWidth = Island.SeabedSlopeWidth;
	// Half a voxel under the profile: the voxel seabed past the edge is the profile itself (no noise there), which
	// Marching Cubes and Dual Contouring follow within a few uu, so the drawn seabed stays hidden wherever the real one
	// is loaded and shows where it is not. Kept small because the loaded seabed shadows it along its edge in a strip
	// about this wide (1.5 voxels drew a dark line there).
	FarOceanSettings.SeabedOffset = 0.5 * Configuration->VoxelSize;
	FarOceanSettings.FloorDrop = Configuration->VoxelSize;
	FarOceanSettings.EdgeZ = Island.EdgeHeight;
	FarOceanSettings.SeabedHeight = [Island](double Distance)
	{
		return static_cast<double>(FIslandBowlWorldMode::GetSeabedHeight(static_cast<float>(Distance), Island));
	};
	FarOceanSettings.SeabedMaterialID = ResolveFarSeabedMaterial(Island);
	FarOceanSettings.InlandCoverage = CVarFarOceanInland.GetValueOnGameThread();

	const FVector Origin = FarOceanSettings.GetMeshOrigin();
	const bool bVisible = CVarFarOcean.GetValueOnGameThread() != 0;

	// The seabed and the open sea never change: one component. The seabed takes the terrain's own material instance
	// (the atlas parameters), so it reads as the voxel seabed it continues.
	FarOceanStatic = MakeComponent(Owner, TEXT("VoxelFarOceanStatic"), Origin);
	FChunkMeshData Mesh;
	const int32 SeabedTriangles = FVoxelFarOcean::BuildSeabed(FarOceanSettings, Mesh);
	SetSection(FarOceanStatic, 0, Mesh, MeshRenderer ? MeshRenderer->GetSurfaceMaterial() : nullptr);

	// The water needs the water tiles' material (without it the world actor draws its fallback plane).
	const bool bWater = Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial;
	int32 OpenSeaQuads = 0;
	if (bWater)
	{
		FChunkMeshData Underside;
		OpenSeaQuads = FVoxelFarOcean::BuildOpenSeaWater(FarOceanSettings, Mesh, &Underside);
		SetSection(FarOceanStatic, 1, Mesh, Configuration->WaterMeshMaterial);
		SetSection(FarOceanStatic, 2, Underside, Configuration->WaterMeshMaterial);
		FarOceanStatic->SetTranslucentSortPriority(0);

		FarOceanGrid = MakeComponent(Owner, TEXT("VoxelFarOceanGrid"), Origin);
		FarOceanWindow = MakeComponent(Owner, TEXT("VoxelFarOceanWindow"), Origin);
		UpdateFarOceanGrid(true);
	}
	for (UProceduralMeshComponent* Component : { FarOceanStatic.Get(), FarOceanGrid.Get(), FarOceanWindow.Get() })
	{
		if (Component)
		{
			Component->SetVisibility(bVisible);
		}
	}
	bFarOceanUndersideShown = true; // force the first update to hide them
	UpdateFarOceanUnderside(CurrentViewerPosition);

	FIntPoint GridMin, GridMax;
	FVoxelFarOcean::GetGridColumns(FarOceanSettings, GridMin, GridMax);
	UE_LOG(LogVoxelStreaming, Log,
		TEXT("Far ocean: seabed %d triangle(s) (material %d, %.0f uu under the profile), %s; grid %dx%d column(s), %d open; reaches %.0f uu past the edge; built in %.2f ms%s"),
		SeabedTriangles, FarOceanSettings.SeabedMaterialID, FarOceanSettings.SeabedOffset,
		bWater ? *FString::Printf(TEXT("open sea %d quad(s), grid %d quad(s) (%d in the window)"), OpenSeaQuads, FarOceanGridQuads,
			FarOceanGridQuads - FarOceanFrameQuads)
			: TEXT("no water (needs bEnableWaterLevel and WaterMeshMaterial)"),
		GridMax.X - GridMin.X + 1, GridMax.Y - GridMin.Y + 1, FarOceanOpenColumns.Num(), FarOceanSettings.OuterDistance,
		(FPlatformTime::Seconds() - StartTime) * 1000.0, bVisible ? TEXT("") : TEXT(" (hidden: voxel.World.FarOcean 0)"));
}

void UVoxelChunkManager::DestroyFarOcean()
{
	for (UProceduralMeshComponent* Component : { FarOceanGrid.Get(), FarOceanWindow.Get(), FarOceanStatic.Get() })
	{
		if (IsValid(Component))
		{
			Component->DestroyComponent();
		}
	}
	FarOceanGrid = nullptr;
	FarOceanWindow = nullptr;
	FarOceanStatic = nullptr;
	FarOceanOpenColumns.Reset();
	FarOceanWindowColumns = FIntRect();
	FarOceanFrameQuads = 0;
	FarOceanGridQuads = 0;
	bFarOceanUndersideShown = false;
}

bool UVoxelChunkManager::UpdateFarOceanGrid(bool bForce)
{
	if (!FarOceanGrid)
	{
		return false;
	}
	// Open: every column holding a chunk with voxel data (its terrain and water tile may render) or a water tile.
	TSet<FIntPoint> Open;
	Open.Reserve(FarOceanOpenColumns.Num() + 16);
	for (const TPair<FIntVector, FVoxelChunkState>& Pair : ChunkStates)
	{
		switch (Pair.Value.State)
		{
		case EChunkState::PendingMeshing:
		case EChunkState::Meshing:
		case EChunkState::Loaded:
		case EChunkState::PendingUnload:
			Open.Add(FIntPoint(Pair.Key.X, Pair.Key.Y));
			break;
		default:
			break;
		}
	}
	for (const TPair<FIntVector2, FWaterTileState>& Pair : WaterTiles)
	{
		Open.Add(FIntPoint(Pair.Key.X, Pair.Key.Y));
	}
	if (!bForce && Open.Num() == FarOceanOpenColumns.Num() && Open.Includes(FarOceanOpenColumns))
	{
		return false;
	}
	FarOceanOpenColumns = MoveTemp(Open);

	// The window follows the open columns: when they leave it, it moves to their bounds plus a margin and the frame
	// outside it is rebuilt; otherwise only the window is (a few hundred columns instead of the whole grid).
	bool bMoveWindow = bForce;
	if (FarOceanOpenColumns.Num() > 0)
	{
		FIntPoint OpenMin(TNumericLimits<int32>::Max()), OpenMax(TNumericLimits<int32>::Lowest());
		for (const FIntPoint& Column : FarOceanOpenColumns)
		{
			OpenMin = OpenMin.ComponentMin(Column);
			OpenMax = OpenMax.ComponentMax(Column);
		}
		const FIntRect& W = FarOceanWindowColumns;
		const bool bInside = OpenMin.X >= W.Min.X && OpenMin.Y >= W.Min.Y && OpenMax.X < W.Max.X && OpenMax.Y < W.Max.Y;
		if (!bInside || bForce)
		{
			FarOceanWindowColumns = FIntRect(OpenMin - FIntPoint(FarOceanWindowMarginColumns),
				OpenMax + FIntPoint(1 + FarOceanWindowMarginColumns));
			bMoveWindow = true;
		}
	}

	using VoxelFarOceanManagerPrivate::SetSection;
	UMaterialInterface* WaterMaterial = Configuration ? Configuration->WaterMeshMaterial.Get() : nullptr;
	UMaterialInterface* FloorMaterial = MeshRenderer ? MeshRenderer->GetSurfaceMaterial() : nullptr;
	FChunkMeshData Mesh;
	FChunkMeshData Underside;
	auto BuildPart = [&](UProceduralMeshComponent* Component, EVoxelFarOceanGridPart Part)
	{
		const int32 Quads = FVoxelFarOcean::BuildGridWater(FarOceanSettings, FarOceanOpenColumns, Mesh, &Underside, Part,
			FarOceanWindowColumns);
		SetSection(Component, 0, Mesh, WaterMaterial);
		SetSection(Component, 2, Underside, WaterMaterial);
		if (Component->GetNumSections() > 2)
		{
			Component->SetMeshSectionVisible(2, bFarOceanUndersideShown);
		}
		// The floor under the grid water's columns inside the footprint uses the terrain material, as the seabed.
		FVoxelFarOcean::BuildGridFloor(FarOceanSettings, FarOceanOpenColumns, Mesh, Part, FarOceanWindowColumns);
		SetSection(Component, 1, Mesh, FloorMaterial);
		return Quads;
	};
	if (bMoveWindow)
	{
		FarOceanFrameQuads = BuildPart(FarOceanGrid, EVoxelFarOceanGridPart::Frame);
		++FarOceanFrameRebuilds;
	}
	FarOceanGridQuads = FarOceanFrameQuads + (FarOceanWindow ? BuildPart(FarOceanWindow, EVoxelFarOceanGridPart::Window) : 0);
	++FarOceanRebuilds;
	return true;
}

void UVoxelChunkManager::UpdateFarOceanUnderside(const FVector& ViewerPosition)
{
	const bool bBelow = ViewerPosition.Z < FarOceanSettings.WaterZ;
	if (bBelow == bFarOceanUndersideShown)
	{
		return;
	}
	bFarOceanUndersideShown = bBelow;
	for (UProceduralMeshComponent* Component : { FarOceanGrid.Get(), FarOceanWindow.Get(), FarOceanStatic.Get() })
	{
		if (IsValid(Component) && Component->GetNumSections() > 2)
		{
			Component->SetMeshSectionVisible(2, bBelow);
		}
	}
}

void UVoxelChunkManager::SetFarOceanVisible(bool bVisible)
{
	for (UProceduralMeshComponent* Component : { FarOceanGrid.Get(), FarOceanWindow.Get(), FarOceanStatic.Get() })
	{
		if (IsValid(Component))
		{
			Component->SetVisibility(bVisible);
		}
	}
	if (FarOceanStatic)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Far ocean %s"), bVisible ? TEXT("shown") : TEXT("hidden"));
	}
}

// ==================== Planet Ocean (spherical SP-E2) ====================

void UVoxelChunkManager::RebuildPlanetOcean()
{
	DestroyPlanetOcean();

	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	// Visual only: a dedicated server draws nothing.
	if (!Configuration || !World || !World->IsGameWorld() || !Owner || World->GetNetMode() == NM_DedicatedServer
		|| Configuration->WorldMode != EWorldMode::SphericalPlanet || !Configuration->bEnableWaterLevel)
	{
		return;
	}
	if (!Configuration->WaterMeshMaterial)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("Planet ocean: not drawn, the configuration has no WaterMeshMaterial."));
		return;
	}
	const double StartTime = FPlatformTime::Seconds();
	PlanetOceanSettings = FVoxelPlanetOceanSettings();
	PlanetOceanSettings.Center = Configuration->WorldOrigin; // the planet centre (FVoxelWorldModeFactory::MakePlanetParams)
	PlanetOceanSettings.Radius = Configuration->WaterRadius;
	PlanetOceanSettings.VoxelSize = Configuration->VoxelSize;

	FVoxelPlanetOceanMesh Top;
	FVoxelPlanetOceanMesh Underside;
	int32 Segments = 0;
	const int32 Quads = FVoxelPlanetOcean::Build(PlanetOceanSettings, Top, &Underside, Segments);
	if (Quads == 0)
	{
		return;
	}

	PlanetOcean = VoxelFarOceanManagerPrivate::MakeComponent(Owner, TEXT("VoxelPlanetOcean"), PlanetOceanSettings.Center);
	auto MakeTangents = [](const TArray<FVector>& In)
	{
		TArray<FProcMeshTangent> Out;
		Out.Reserve(In.Num());
		for (const FVector& T : In)
		{
			Out.Add(FProcMeshTangent(T, false));
		}
		return Out;
	};
	const TArray<FVector2D> NoUV;
	PlanetOcean->CreateMeshSection(0, Top.Positions, Top.Triangles, Top.Normals, Top.UV0, Top.UV1, NoUV, NoUV, Top.Colors,
		MakeTangents(Top.Tangents), /*bCreateCollision=*/false);
	PlanetOcean->CreateMeshSection(1, Underside.Positions, Underside.Triangles, Underside.Normals, Underside.UV0, Underside.UV1,
		NoUV, NoUV, Underside.Colors, MakeTangents(Underside.Tangents), /*bCreateCollision=*/false);
	PlanetOcean->SetMaterial(0, Configuration->WaterMeshMaterial);
	PlanetOcean->SetMaterial(1, Configuration->WaterMeshMaterial);
	PlanetOcean->SetVisibility(CVarPlanetOcean.GetValueOnGameThread() != 0);
	bPlanetOceanUndersideShown = true; // force the first update to decide
	UpdatePlanetOceanUnderside(CurrentViewerPosition);

	UE_LOG(LogVoxelStreaming, Log,
		TEXT("Planet ocean: radius %.0f, %d quads a face edge (%d quads, sag at most %.1f uu), built in %.2f ms%s"),
		PlanetOceanSettings.Radius, Segments, Quads, FVoxelPlanetOcean::MeasureMaxSag(PlanetOceanSettings.Radius, Segments),
		(FPlatformTime::Seconds() - StartTime) * 1000.0,
		CVarPlanetOcean.GetValueOnGameThread() != 0 ? TEXT("") : TEXT(" (hidden: voxel.World.PlanetOcean 0)"));
}

void UVoxelChunkManager::DestroyPlanetOcean()
{
	if (IsValid(PlanetOcean))
	{
		PlanetOcean->DestroyComponent();
	}
	PlanetOcean = nullptr;
	bPlanetOceanUndersideShown = false;
}

void UVoxelChunkManager::UpdatePlanetOceanUnderside(const FVector& ViewerPosition)
{
	if (!PlanetOcean)
	{
		return;
	}
	const bool bInside = FVector::DistSquared(ViewerPosition, PlanetOceanSettings.Center)
		< PlanetOceanSettings.Radius * PlanetOceanSettings.Radius;
	if (bInside == bPlanetOceanUndersideShown)
	{
		return;
	}
	bPlanetOceanUndersideShown = bInside;
	if (PlanetOcean->GetNumSections() > 1)
	{
		PlanetOcean->SetMeshSectionVisible(1, bInside);
	}
}

void UVoxelChunkManager::SetPlanetOceanVisible(bool bVisible)
{
	if (IsValid(PlanetOcean))
	{
		PlanetOcean->SetVisibility(bVisible);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Planet ocean %s"), bVisible ? TEXT("shown") : TEXT("hidden"));
	}
}

FString UVoxelChunkManager::GetFarOceanDebugString() const
{
	if (!FarOceanStatic)
	{
		return TEXT("no far ocean (not an island, bIslandFarOcean off, or not a game world)");
	}
	FIntPoint GridMin, GridMax;
	FVoxelFarOcean::GetGridColumns(FarOceanSettings, GridMin, GridMax);
	const FIntRect& W = FarOceanWindowColumns;
	return FString::Printf(TEXT("visible=%d open columns=%d grid %dx%d quads=%d (window %dx%d: %d) rebuilds=%d (frame %d) water=%s seabed material=%d reaches %.0f uu past the edge, inland coverage %.0f"),
		FarOceanStatic->IsVisible() ? 1 : 0, FarOceanOpenColumns.Num(), GridMax.X - GridMin.X + 1, GridMax.Y - GridMin.Y + 1,
		FarOceanGridQuads, W.Width(), W.Height(), FarOceanGridQuads - FarOceanFrameQuads, FarOceanRebuilds, FarOceanFrameRebuilds,
		FarOceanGrid ? TEXT("yes") : TEXT("no"), FarOceanSettings.SeabedMaterialID,
		FarOceanSettings.OuterDistance, FarOceanSettings.InlandCoverage);
}

void UVoxelChunkManager::AddConditioningZone(const FVoxelConditioningZone& Zone)
{
	ConditioningZones.Add(Zone);
	UE_LOG(LogVoxelStreaming, Log,
		TEXT("Added terrain conditioning zone at (%.0f,%.0f) inner=%.0f falloff=%.0f target=%.0f strength=%.2f; %d total."),
		Zone.Center.X, Zone.Center.Y, Zone.InnerRadius, Zone.FalloffWidth, Zone.TargetHeight, Zone.Strength, ConditioningZones.Num());
	OnTerrainConditioningChanged.Broadcast();
}

void UVoxelChunkManager::ClearConditioningZones()
{
	ConditioningZones.Reset();
	OnTerrainConditioningChanged.Broadcast();
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
	// Mark as in-progress (stamped: the serial travels with the result, see FAsyncJobStamp)
	const uint32 DispatchSerial = StampAsyncDispatch(AsyncGenerationInProgress, Request.ChunkCoord);

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
			Pending.DispatchSerial = DispatchSerial;
			PendingGPUReadbacks.Add(ChunkCoord, MoveTemp(Pending));
		}
		else
		{
			// Dispatch failed (generator not initialized) — report failure via the normal completion queue
			// so ProcessCompletedAsyncGenerations clears in-progress tracking and resets the chunk state.
			FAsyncGenerationResult FailResult;
			FailResult.ChunkCoord = ChunkCoord;
			FailResult.bSuccess = false;
			FailResult.DispatchSerial = DispatchSerial;
			CompletedGenerationQueue.Enqueue(MoveTemp(FailResult));
		}
		return;
	}

	// Tree injection captures (value copies for thread safety). Voxel trees are a heightmap feature (cubic, which planets
	// do not use: spherical SD3, SP-B9).
	const bool bInjectTrees = Configuration &&
		Configuration->WorldMode != EWorldMode::SphericalPlanet &&
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

	Async(EAsyncExecution::ThreadPool, [WeakThis, GeneratorPtr, GenRequest = MoveTemp(GenRequest), ChunkCoord, DispatchSerial,
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
			Result.DispatchSerial = DispatchSerial;
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
	int32 ProcessedCount = 0;
	const int32 MaxProcessPerFrame = 8;

	double StoreSeconds = 0.0;
	double NotifySeconds = 0.0;

	// Bounded drain: results beyond this tick's budget stay queued (see VoxelDrainBounded).
	VoxelDrainBounded(CompletedGenerationQueue, MaxProcessPerFrame, [&](FAsyncGenerationResult& Result) -> bool
	{
		// Only the dispatch that owns the chunk's in-flight slot may complete it; this also removes
		// the slot. A superseded result (unloaded chunk, watchdog re-queue) is discarded.
		if (!AcceptAsyncResult(AsyncGenerationInProgress, Result.ChunkCoord, Result.DispatchSerial, WorstGenerationAsyncSeconds))
		{
			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) async generation discarded - superseded dispatch"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
			return false;
		}

		// Check if chunk is still in valid state
		const EChunkState CurrentState = GetChunkState(Result.ChunkCoord);
		if (CurrentState != EChunkState::Generating)
		{
			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) async generation discarded - state changed to %d"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z, static_cast<int32>(CurrentState));
			return false;
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
		return true;
	});

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
			LaunchPostReadbackProcessing(ChunkCoord, MoveTemp(Pending.GenRequest), MoveTemp(VoxelData), Pending.DispatchSerial);
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Chunk (%d,%d,%d) GPU generation failed (status=%d, voxels=%d/%d)"),
				ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, static_cast<int32>(Status), VoxelData.Num(), ExpectedVoxels);
			FAsyncGenerationResult FailResult;
			FailResult.ChunkCoord = ChunkCoord;
			FailResult.bSuccess = false;
			FailResult.DispatchSerial = Pending.DispatchSerial;
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

void UVoxelChunkManager::LaunchPostReadbackProcessing(const FIntVector& ChunkCoord, FVoxelNoiseGenerationRequest GenRequest, TArray<FVoxelData> VoxelData, uint32 DispatchSerial)
{
	// The water/underground post-passes already ran on the GPU (AddVoxelPostPassDispatches), so the
	// readback data is finished except for cubic-mode voxel-tree injection. Without trees there is
	// nothing left to compute — enqueue directly and skip the worker hop. Never on a planet (SP-B9).
	const bool bInjectTrees = Configuration &&
		Configuration->WorldMode != EWorldMode::SphericalPlanet &&
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
		Result.DispatchSerial = DispatchSerial;
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

	Async(EAsyncExecution::ThreadPool, [WeakThis, ChunkCoord, DispatchSerial,
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
			Result.DispatchSerial = DispatchSerial;
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
	double DispatchSeconds = 0.0;

	// Cap new mesh dispatches per frame — the mesh-consumer drain rate. Each GPU dispatch builds
	// an RDG graph + uploads ~150KB + 4 compute passes, so a very high cap can stall the render
	// thread at frame sync; the in-flight cap (EffectiveMaxAsyncMeshTasks) also bounds pipeline
	// depth. This was hardcoded to min(MaxChunksToLoadPerFrame, 2), which A/B showed was far too
	// conservative (2->6 cut post-traverse drain ~2.5x and peak backlog ~35% at zero frame cost at
	// moderate traversal). Now config-driven (MaxMeshDispatchPerFrame, default 6) with a runtime
	// override (voxel.Stream.MaxMeshDispatchPerFrame, 0 = use config).
	const int32 CvarMeshDispatch = CVarStreamMaxMeshDispatch.GetValueOnGameThread();
	const int32 MaxNewDispatches = (CvarMeshDispatch > 0)
		? FMath::Clamp(CvarMeshDispatch, 1, 32)
		: FMath::Max(1, Configuration->MaxMeshDispatchPerFrame);
	int32 ProcessedCount = 0;

	// Chunks whose previous mesh job is still on a worker. They stay PendingMeshing and are re-queued
	// after the loop so they retry next frame. Examined bounds per-frame work so a queue full of them
	// can't spin the whole queue every frame.
	TArray<FChunkLODRequest> DeferredThisFrame;
	int32 Examined = 0;
	const int32 MaxExamined = MaxNewDispatches + 48;

	while (MeshingQueue.Num() > 0 && ProcessedCount < MaxNewDispatches &&
	       Examined < MaxExamined &&
	       AsyncMeshingInProgress.Num() < EffectiveMaxAsyncMeshTasks &&
	       PendingMeshQueue.Num() < EffectiveMaxPendingMeshes)
	{
		++Examined;

		// Skip chunks already being meshed asynchronously. A chunk waiting to mesh (PendingMeshing)
		// keeps its request for a later frame instead of losing it: dropping it left the chunk in
		// PendingMeshing with no queue entry, so it never meshed again (R23). Unloads and the
		// stuck-job watchdog release the slot, so the wait is bounded.
		if (AsyncMeshingInProgress.Contains(MeshingQueue.Last().ChunkCoord))
		{
			const FChunkLODRequest InFlightRequest = MeshingQueue.Last();
			MeshingQueueSet.Remove(InFlightRequest.ChunkCoord);
			MeshingQueue.Pop(EAllowShrinking::No);
			if (GetChunkState(InFlightRequest.ChunkCoord) == EChunkState::PendingMeshing)
			{
				DeferredThisFrame.Add(InFlightRequest);
			}
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

		// Mark as meshing
		SetChunkState(Request.ChunkCoord, EChunkState::Meshing);

		// Build meshing request
		double SubT0 = FPlatformTime::Seconds();
		FVoxelMeshingRequest MeshRequest;
		MeshRequest.ChunkCoord = Request.ChunkCoord;
		// Force LOD 0 when LOD system is disabled (defense-in-depth)
		MeshRequest.LODLevel = (Configuration->IsLODEnabled()) ? Request.LODLevel : 0;
		MeshRequest.ChunkSize = Configuration->ChunkSize;
		MeshRequest.VoxelSize = Configuration->VoxelSize;
		MeshRequest.WorldOrigin = Configuration->WorldOrigin;
		MeshRequest.LocalUp = GetChunkLocalUp(Request.ChunkCoord);
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

		// Seam ownership: a chunk mesh is the Interior domain, a pure function of the chunk's own voxels, so
		// it never waits for a neighbour and never re-meshes when one arrives. Boundary geometry comes from
		// the single-owner seam jobs (TickSeamScheduler). The request deliberately carries no neighbour data.
		MeshRequest.MeshCellDomain = EVoxelMeshCellDomain::Interior;

		// Benchmark thrash: a mesh job for a chunk that already had a mesh is churn. Counted here, where the
		// job is launched, and attributed to the reason the chunk was queued for (G5).
		if (bBenchmarkViewActive)
		{
			EVoxelRemeshReason QueuedReason = EVoxelRemeshReason::Other;
			BenchQueuedRemeshReason.RemoveAndCopyValue(Request.ChunkCoord, QueuedReason);
			if (BenchEverMeshed.Contains(Request.ChunkCoord))
			{
				++BenchRemeshCount;
				++BenchRemeshByReason[static_cast<int32>(QueuedReason)];
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

	// Re-queue chunks whose previous mesh job was still in flight. They are still in PendingMeshing state.
	for (const FChunkLODRequest& Deferred : DeferredThisFrame)
	{
		AddToMeshingQueue(Deferred);
	}

	Timing.MeshSnapshotMs = static_cast<float>(SnapshotSeconds * 1000.0);
	Timing.MeshDispatchMs = static_cast<float>(DispatchSeconds * 1000.0);
	Timing.MeshLaunchCount = ProcessedCount;
}

IVoxelMesher* UVoxelChunkManager::EnsureSeamMesher()
{
	if (!SeamMesher.IsValid())
	{
		// Match the configured meshing mode: seam geometry must terminate exactly on the
		// interior meshes, which requires the same algorithm family AND the same config
		// (iso level + QEF parameters for DC; transvoxel/morph settings for MC).
		if (Configuration && Configuration->MeshingMode == EMeshingMode::MarchingCubes)
		{
			SeamMesher = MakeUnique<FVoxelCPUMarchingCubesMesher>();
		}
		else if (Configuration && Configuration->MeshingMode == EMeshingMode::Cubic)
		{
			SeamMesher = MakeUnique<FVoxelCPUCubicMesher>();
		}
		else
		{
			SeamMesher = MakeUnique<FVoxelCPUDualContourMesher>();
		}
		SeamMesher->Initialize();
		SeamMesher->SetConfig(Mesher->GetConfig());
	}
	return SeamMesher.Get();
}

void UVoxelChunkManager::LaunchAsyncMeshGeneration(const FChunkLODRequest& Request, FVoxelMeshingRequest MeshRequest)
{
	// Mark as in-progress (stamped: the serial travels with the result, see FAsyncJobStamp)
	const uint32 DispatchSerial = StampAsyncDispatch(AsyncMeshingInProgress, Request.ChunkCoord);

	// Capture mesher pointer (it's a TUniquePtr, so we need raw pointer for lambda)
	IVoxelMesher* MesherPtr = Mesher.Get();

	// Seam-ownership junction exactness: interior chunk meshes run on the same CPU instance that
	// meshes the seams. Seam rings recompute rim vertices bit-exactly only against the CPU op
	// sequence — the GPU QEF solve diverges (cell-relative coordinate formulation, and SVD threshold
	// flips that can move a vertex by up to a cell), which rendered as boundary cracks growing with
	// LOD. One mesher computes both sides; a GPU main mesher idles for chunk meshes.
	if (MesherPtr->GetMesherTypeName().Contains(TEXT("GPU"))
		&& CVarSeamCPUInteriorRouting.GetValueOnGameThread() != 0)
	{
		MesherPtr = EnsureSeamMesher();
	}
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
		OnComplete.BindLambda([WeakThis, MesherPtr, ChunkCoord, LODLevel, DispatchSerial](
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
			AsyncResult.DispatchSerial = DispatchSerial;

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
		// volume into upload buffers, copies + allocations per chunk) was a per-launch game-thread
		// cost. The DC/MC meshers are worker-safe here (atomic request ids, lock-guarded result
		// maps, and their render command is self-contained).
		Async(EAsyncExecution::ThreadPool, [MesherPtr, MeshRequest = MoveTemp(MeshRequest), OnComplete]() mutable
		{
			MesherPtr->GenerateMeshAsync(MeshRequest, OnComplete);
		});
	}
	else
	{
		// CPU path: existing thread pool dispatch (unchanged)
		Async(EAsyncExecution::ThreadPool, [WeakThis, MesherPtr, MeshRequest = MoveTemp(MeshRequest), ChunkCoord, LODLevel, DispatchSerial]() mutable
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
				Result.DispatchSerial = DispatchSerial;
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
	const int32 MaxProcessPerFrame = 8; // Limit how many we process to spread render uploads

	// Bounded drain: results beyond this tick's budget stay queued. The old
	// `while (Dequeue(Result) && Count < Max)` popped the 9th result and dropped it, stranding its
	// chunk in Meshing for the session (R23b; the demo allows 16 async meshes in flight).
	VoxelDrainBounded(CompletedMeshQueue, MaxProcessPerFrame, [this](FAsyncMeshResult& Result) -> bool
	{
		// Only the dispatch that owns the chunk's in-flight slot may complete it; this also removes
		// the slot. A superseded result (unloaded chunk, watchdog re-queue) is discarded.
		if (!AcceptAsyncResult(AsyncMeshingInProgress, Result.ChunkCoord, Result.DispatchSerial, WorstMeshAsyncSeconds))
		{
			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d,%d,%d) async mesh discarded - superseded dispatch"),
				Result.ChunkCoord.X, Result.ChunkCoord.Y, Result.ChunkCoord.Z);
			return false;
		}

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
			return false;
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

		return true;
	});
}

// ==================== In-flight async job tracking (R23c) ====================

uint32 UVoxelChunkManager::StampAsyncDispatch(TMap<FIntVector, FAsyncJobStamp>& InFlight, const FIntVector& ChunkCoord)
{
	FAsyncJobStamp& Stamp = InFlight.FindOrAdd(ChunkCoord);
	Stamp.DispatchedAtSeconds = FPlatformTime::Seconds();
	Stamp.DispatchFrame = CurrentFrame;
	Stamp.Serial = NextAsyncJobSerial++;
	if (NextAsyncJobSerial == 0)
	{
		NextAsyncJobSerial = 1; // 0 is never issued
	}
	return Stamp.Serial;
}

bool UVoxelChunkManager::AcceptAsyncResult(TMap<FIntVector, FAsyncJobStamp>& InFlight, const FIntVector& ChunkCoord, uint32 Serial, double& WorstSeconds)
{
	const FAsyncJobStamp* Stamp = InFlight.Find(ChunkCoord);
	if (!Stamp || Stamp->Serial != Serial)
	{
		return false;
	}
	WorstSeconds = FMath::Max(WorstSeconds, FPlatformTime::Seconds() - Stamp->DispatchedAtSeconds);
	InFlight.Remove(ChunkCoord);
	return true;
}

void UVoxelChunkManager::CancelInFlightGeneration(const FIntVector& ChunkCoord)
{
	AsyncGenerationInProgress.Remove(ChunkCoord);
	// A GPU dispatch also holds a readback handle polled each tick; release it so the poll stops
	// (ReleaseHandle is safe while the render thread still holds the result: it is shared).
	if (FPendingGPUGeneration* Pending = PendingGPUReadbacks.Find(ChunkCoord))
	{
		if (GPUGeneratorPtr)
		{
			GPUGeneratorPtr->ReleaseHandle(Pending->Handle);
		}
		PendingGPUReadbacks.Remove(ChunkCoord);
	}
}

void UVoxelChunkManager::CancelInFlightMesh(const FIntVector& ChunkCoord)
{
	AsyncMeshingInProgress.Remove(ChunkCoord);
}

void UVoxelChunkManager::CheckStuckAsyncJobs()
{
	const float StuckSeconds = CVarStreamMeshStuckSeconds.GetValueOnGameThread();
	if (StuckSeconds <= 0.0f)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	// Wall time alone would flag every in-flight job after a PIE pause or a debugger break (the
	// manager did not tick, results sat in the queue); a stranded job also misses many ticks.
	constexpr int64 MinStuckTicks = 60;
	auto IsStuck = [this, Now, StuckSeconds](const FAsyncJobStamp& Stamp, double& OutAge)
	{
		OutAge = Now - Stamp.DispatchedAtSeconds;
		return OutAge > StuckSeconds && (CurrentFrame - Stamp.DispatchFrame) >= MinStuckTicks;
	};

	// Meshing: give the slot back and send the chunk round again. A late result for the old
	// dispatch is rejected by its serial.
	TArray<TPair<FIntVector, double>> Stuck;
	for (const TPair<FIntVector, FAsyncJobStamp>& Pair : AsyncMeshingInProgress)
	{
		double Age = 0.0;
		if (IsStuck(Pair.Value, Age))
		{
			Stuck.Emplace(Pair.Key, Age);
		}
	}
	for (const TPair<FIntVector, double>& Entry : Stuck)
	{
		const FIntVector& ChunkCoord = Entry.Key;
		CancelInFlightMesh(ChunkCoord);
		++TotalStuckMeshRequeues;

		FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
		const bool bRequeue = State && State->State == EChunkState::Meshing;
		UE_LOG(LogVoxelStreaming, Warning,
			TEXT("Chunk (%d,%d,%d) async mesh in flight %.1f s (> voxel.Stream.MeshStuckSeconds %.1f): slot released%s"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, Entry.Value, StuckSeconds,
			bRequeue ? TEXT(", re-queued for meshing") : TEXT(""));
		if (bRequeue)
		{
			FChunkLODRequest Request;
			Request.ChunkCoord = ChunkCoord;
			Request.LODLevel = State->LODLevel;
			Request.Priority = 80.0f; // the invisible-chunk safety-net tier
			SetChunkState(ChunkCoord, EChunkState::PendingMeshing);
			AddToMeshingQueue(Request);
		}
	}

	// Generation: same treatment (a lost GPU readback would otherwise hold the chunk in Generating).
	Stuck.Reset();
	for (const TPair<FIntVector, FAsyncJobStamp>& Pair : AsyncGenerationInProgress)
	{
		double Age = 0.0;
		if (IsStuck(Pair.Value, Age))
		{
			Stuck.Emplace(Pair.Key, Age);
		}
	}
	for (const TPair<FIntVector, double>& Entry : Stuck)
	{
		const FIntVector& ChunkCoord = Entry.Key;
		CancelInFlightGeneration(ChunkCoord);
		++TotalStuckGenerationRequeues;

		FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
		const bool bRequeue = State && State->State == EChunkState::Generating;
		UE_LOG(LogVoxelStreaming, Warning,
			TEXT("Chunk (%d,%d,%d) async generation in flight %.1f s (> voxel.Stream.MeshStuckSeconds %.1f): slot released%s"),
			ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z, Entry.Value, StuckSeconds,
			bRequeue ? TEXT(", re-queued for generation") : TEXT(""));
		if (bRequeue)
		{
			FChunkLODRequest Request;
			Request.ChunkCoord = ChunkCoord;
			Request.LODLevel = State->LODLevel;
			Request.Priority = State->Priority;
			SetChunkState(ChunkCoord, EChunkState::PendingGeneration);
			AddToGenerationQueue(Request);
		}
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
	return LODStrategy->GetHorizonDistance(ChunkCenter, CurrentViewerPosition) > UnloadDist;
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
			// A chunk loaded again later meshes for the first time again: not a re-mesh. Without this a
			// route that revisits terrain counted every reload as thrash (the I9 soak's lap 2, §13.10).
			BenchEverMeshed.Remove(ChunkCoord);
			BenchQueuedRemeshReason.Remove(ChunkCoord);
		}

		// Remove from renderer
		if (MeshRenderer)
		{
			MeshRenderer->RemoveChunk(ChunkCoord);
			// Seam-ownership: drop this chunk's owned seam buckets with it — SeamSlots 0-2 (faces
			// +X/+Y/+Z), 3-5 (edges parallel to X/Y/Z), 6 (corner). Cheap no-ops when absent.
			for (uint8 SeamSlot = 0; SeamSlot <= 6; ++SeamSlot)
			{
				MeshRenderer->RemoveSeamMesh(ChunkCoord, SeamSlot);
			}
		}
		// Drop the chunk's shared seam snapshot and its stored slot meshes with it
		// (workers holding snapshot refs stay safe).
		SeamSnapshotCache.Remove(ChunkCoord);
		SeamOwnerSlots.Remove(ChunkCoord);

		// Remove from loaded set
		LoadedChunkCoords.Remove(ChunkCoord);

		// Remove water tile contribution before state is cleared
		if (UsesWaterTiles())
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

int32 UVoxelChunkManager::EvaluateLODLevelChanges(const FLODQueryContext& Context, int32& OutQueued, bool& OutCapReached)
{
	OutQueued = 0;
	OutCapReached = false;

	if (!LODStrategy || !MeshRenderer)
	{
		return 0;
	}

	if (Configuration && !Configuration->IsLODEnabled())
	{
		return 0;
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
			// Candidates left over: the schedule evaluates again next tick instead of waiting for the
			// viewer to move (KI12). A cap of 0 disables LOD re-meshing, so it leaves nothing "over".
			OutCapReached = MaxLODRemeshThisFrame > 0;
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

	OutQueued = QueuedThisFrame;
	return RemeshCandidates.Num();
}

void UVoxelChunkManager::GetBenchLoadedByLayer(TMap<int32, int32>& OutByLayer) const
{
	OutByLayer.Reset();
	for (const FIntVector& ChunkCoord : LoadedChunkCoords)
	{
		++OutByLayer.FindOrAdd(ChunkCoord.Z);
	}
}

void UVoxelChunkManager::GetBenchLODBacklog(int32& OutStale, int32& OutStaleNear) const
{
	OutStale = 0;
	OutStaleNear = 0;
	if (!LODStrategy || (Configuration && !Configuration->IsLODEnabled()))
	{
		return;
	}

	const FLODQueryContext Context = BuildQueryContext();
	for (const FIntVector& ChunkCoord : LoadedChunkCoords)
	{
		const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
		if (!State || State->State != EChunkState::Loaded)
		{
			continue; // queued or in flight: its LOD is already on its way
		}
		const int32 TargetLOD = LODStrategy->GetLODForChunk(ChunkCoord, Context);
		if (State->LODLevel != TargetLOD)
		{
			++OutStale;
			if (TargetLOD == 0)
			{
				++OutStaleNear;
			}
		}
	}
}

void UVoxelChunkManager::UpdateLODMorphFactors(const FLODQueryContext& Context)
{
	if (!LODStrategy || !MeshRenderer)
	{
		return;
	}

	if (Configuration && !Configuration->IsLODEnabled())
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
	// Release any in-flight generation / mesh slot: its result belongs to the residency that just
	// ended and is rejected on arrival (serial mismatch), so a reload dispatches fresh work instead
	// of waiting on — or being completed by — the old job.
	CancelInFlightGeneration(ChunkCoord);
	CancelInFlightMesh(ChunkCoord);
	// R24: a departed chunk cannot finish an LOD transition. Drop any hold request it left in the
	// renderer (its meshes are gone) and stop waiting for seams it participates in.
	if (MeshRenderer && (SeamHolds.IsChunkHeld(ChunkCoord) || SeamHolds.IsSeamOwnerHeld(ChunkCoord)))
	{
		MeshRenderer->SetChunkFadeHold(ChunkCoord, false);
		MeshRenderer->SetSeamFadeHold(ChunkCoord, false);
	}
	SeamHolds.OnChunkRemoved(ChunkCoord);
	// Seam-ownership P0: this chunk's voxel data is gone — restale/prune its incident seams.
	if (SeamRegistry.IsValid())
	{
		SeamRegistry->UnregisterChunk(ChunkCoord);
	}
}

void UVoxelChunkManager::TickSeamScheduler()
{
	if (!SeamRegistry.IsValid())
	{
		return;
	}

	// Snapshot-cache age sweep: entries are only useful while a chunk's seam wave dispatches;
	// drop anything unused for ~2s. Workers keep in-flight data alive via their own refs, so
	// eviction is always safe. Bounds the cache to the active streaming front (tens of MB peak).
	++SeamTickCounter;
	if ((SeamTickCounter & 63) == 0)
	{
		for (auto It = SeamSnapshotCache.CreateIterator(); It; ++It)
		{
			if (SeamTickCounter - It->Value.LastUsedTick > 128)
			{
				It.RemoveCurrent();
			}
		}
	}

	// Instrumentation: the frame time this tick runs in, so cap sweeps can see their frame cost.
	if (const UWorld* World = GetWorld())
	{
		const double FrameMs = World->GetDeltaSeconds() * 1000.0;
		SeamFrameMsAccum += FrameMs;
		SeamFrameMsMax = FMath::Max(SeamFrameMsMax, FrameMs);
		++SeamFrameTicks;
	}

	// Drain finished async seam meshes first — frees in-flight slots for this tick's dispatches.
	ProcessCompletedSeamMeshes();

	// Enable/debug flags were synced early in TickComponent so this tick's generation/meshing
	// completions already registered into the registry. Here we only schedule + process.
	// Move dirty+all-resident seams into the priority-sorted job queue (near-viewer first), then run
	// the job processor. Both are budget-capped so the per-tick cost stays bounded.
	const FIntVector ViewerChunk = WorldToChunkCoord(CurrentViewerPosition);
	const int32 NearRadius = FMath::Max(0, CVarSeamNearChunkRadius.GetValueOnGameThread());
	SeamRegistry->ScheduleReadySeams(ViewerChunk, NearRadius, FMath::Max(0, CVarSeamMaxSchedulePerTick.GetValueOnGameThread()));

	const int32 MaxJobs = FMath::Max(0, CVarSeamMaxJobsPerTick.GetValueOnGameThread());
	// Pop jobs (bounded by the free in-flight slots) and run the mode-matched seam mesher on the
	// worker pool.
	const int32 FreeSlots = FMath::Max(1, CVarSeamMaxInFlight.GetValueOnGameThread()) - SeamJobsInFlight.Num();
	const int32 DrainBudget = (MaxJobs > 0) ? FMath::Min(MaxJobs, FreeSlots) : FreeSlots;
	// Ticks on which the per-tick drain (in-flight cap minus what is still on workers) could not
	// empty the queue — i.e. the cap, not job supply, bounded throughput. Seam jobs finish within
	// a frame (worker p50 ~0.2 ms), so "no free slot" never happens; the cap binds as 8 jobs/tick.
	if (SeamRegistry->GetJobQueueDepth() > DrainBudget)
	{
		++SeamSlotStarvedTicks;
	}
	if (DrainBudget > 0)
	{
		TArray<FVoxelSeamJob> Jobs;
		SeamRegistry->DrainSeamJobs(Jobs, DrainBudget);
		for (const FVoxelSeamJob& Job : Jobs)
		{
			DispatchSeamJob(Job);
		}
	}

	// R24: release LOD transitions whose seams have all landed (results were submitted at the top
	// of this tick, deferred buckets just below), or that hit the hold cap. Everything released here
	// starts fading on one clock.
	FlushDueSeamBuckets();
	{
		FVoxelSeamTransitionHolds::FRelease Released;
		const double HoldSeconds = FMath::Max(0, CVarSeamHoldMs.GetValueOnGameThread()) / 1000.0;
		SeamHolds.CollectReleases(SeamRegistry->NowSeconds(), HoldSeconds, Released);
		ApplySeamHoldReleases(Released);
	}

	// Latency instrumentation readout (see CVarSeamLogLatency). Taken AFTER drain so this tick's
	// completions are included in the interval counters.
	const int32 LogEvery = CVarSeamLogLatency.GetValueOnGameThread();
	if (LogEvery > 0 && (SeamTickCounter % LogEvery) == 0)
	{
		const FVoxelSeamLatencyStats L = SeamRegistry->TakeLatencyStats(ViewerChunk, NearRadius);
		const float T = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		UE_LOG(LogVoxelStreaming, Log,
			TEXT("[SeamLat] t=%.1f dirty=%d nearDirty=%d | examined=%d notResident=%d inFlight=%d scheduled=%d completed=%d")
			TEXT(" | nearSched n=%d p50=%.1f p95=%.1f max=%.1f | farSched n=%d p50=%.1f p95=%.1f max=%.1f")
			TEXT(" | nearScan n=%d p50=%.1f p95=%.1f max=%.1f | farScan n=%d p50=%.1f p95=%.1f max=%.1f")
			TEXT(" | nearE2E n=%d p50=%.1f p95=%.1f max=%.1f | nearReadyDone n=%d p50=%.1f p95=%.1f max=%.1f")
			TEXT(" | nearPostSched n=%d p50=%.1f p95=%.1f max=%.1f")
			TEXT(" | jobQ=%d inFlight=%d peak=%d/%d capped=%d resched1s=%d dropInFlight=%d dropPart=%d rejectDeparted=%d")
			TEXT(" | nearQueue n=%d p50=%.1f p95=%.1f max=%.1f | nearAsync n=%d p50=%.1f p95=%.1f max=%.1f")
			TEXT(" | nearWorker n=%d p50=%.1f p95=%.1f max=%.1f")
			TEXT(" | submit n=%d ticks=%d avgMs=%.2f maxMs=%.2f | frame avgMs=%.2f maxMs=%.2f caps=%d/%d")
			TEXT(" | lodHold groups=%d avgMs=%.1f maxMs=%.1f timeouts=%d leftPending=%d open=%d pending=%d | bucketDefer n=%d open=%d"),
			T, L.DirtyCount, L.NearDirtyCount,
			L.Examined, L.RequeuedNotResident, L.RequeuedInFlight, L.Scheduled, L.Completed,
			L.NearScheduleN, L.NearScheduleP50Ms, L.NearScheduleP95Ms, L.NearScheduleMaxMs,
			L.FarScheduleN, L.FarScheduleP50Ms, L.FarScheduleP95Ms, L.FarScheduleMaxMs,
			L.NearScanN, L.NearScanP50Ms, L.NearScanP95Ms, L.NearScanMaxMs,
			L.FarScanN, L.FarScanP50Ms, L.FarScanP95Ms, L.FarScanMaxMs,
			L.NearEndToEndN, L.NearEndToEndP50Ms, L.NearEndToEndP95Ms, L.NearEndToEndMaxMs,
			L.NearReadyToDoneN, L.NearReadyToDoneP50Ms, L.NearReadyToDoneP95Ms, L.NearReadyToDoneMaxMs,
			L.NearPostScheduleN, L.NearPostScheduleP50Ms, L.NearPostScheduleP95Ms, L.NearPostScheduleMaxMs,
			L.JobQueueDepth, SeamJobsInFlight.Num(), SeamInFlightPeak, CVarSeamMaxInFlight.GetValueOnGameThread(), SeamSlotStarvedTicks,
			L.RescheduledWithin1s, L.DroppedInFlight, L.DroppedParticipant, L.RejectedDepartedOwner,
			L.NearQueueN, L.NearQueueP50Ms, L.NearQueueP95Ms, L.NearQueueMaxMs,
			L.NearAsyncN, L.NearAsyncP50Ms, L.NearAsyncP95Ms, L.NearAsyncMaxMs,
			L.NearWorkerN, L.NearWorkerP50Ms, L.NearWorkerP95Ms, L.NearWorkerMaxMs,
			SeamSubmitCount, SeamSubmitTicks, (SeamSubmitTicks > 0) ? SeamSubmitMsAccum / SeamSubmitTicks : 0.0, SeamSubmitMsMax,
			(SeamFrameTicks > 0) ? SeamFrameMsAccum / SeamFrameTicks : 0.0, SeamFrameMsMax,
			CVarSeamMaxInFlight.GetValueOnGameThread(), CVarSeamMaxSubmitsPerTick.GetValueOnGameThread(),
			SeamHoldGroupsInterval, (SeamHoldGroupsInterval > 0) ? SeamHoldSecondsSumInterval * 1000.0 / SeamHoldGroupsInterval : 0.0,
			SeamHoldSecondsMaxInterval * 1000.0, SeamHoldTimeoutsInterval, SeamHoldLeftPendingInterval,
			SeamHolds.NumGroups(), SeamHolds.NumPendingSeams(),
			SeamBucketDeferralsInterval, SeamHolds.NumDeferredBuckets());
		SeamInFlightPeak = SeamJobsInFlight.Num();
		SeamSlotStarvedTicks = 0;
		SeamSubmitMsAccum = SeamSubmitMsMax = 0.0;
		SeamSubmitTicks = SeamSubmitCount = 0;
		SeamFrameMsAccum = SeamFrameMsMax = 0.0;
		SeamFrameTicks = 0;
		SeamHoldGroupsInterval = SeamHoldTimeoutsInterval = SeamHoldLeftPendingInterval = 0;
		SeamBucketDeferralsInterval = 0;
		SeamHoldSecondsSumInterval = SeamHoldSecondsMaxInterval = 0.0;
	}
}

// ==================== Coordinated LOD transitions (R24) ====================

bool UVoxelChunkManager::ShouldHoldForSeams(const FIntVector& ChunkCoord, int32 PreviousMeshedLOD, int32 NewLOD, const FChunkMeshData& NewMesh) const
{
	if (!MeshRenderer || !SeamRegistry.IsValid() || CVarSeamHoldMs.GetValueOnGameThread() <= 0 || !NewMesh.IsValid())
	{
		return false;
	}
	// An LOD change of a chunk that is on screen: the interior crossfades from its old mesh and every
	// incident seam is rebuilt for the new LOD. Edits (same LOD) never hold — a dig must show at once.
	if (MeshRenderer->IsChunkLoaded(ChunkCoord))
	{
		return PreviousMeshedLOD >= 0 && PreviousMeshedLOD != NewLOD;
	}
	// A first appearance that the renderer fades in (R24 P3, beyond voxel.MeshFade.AppearMinDistance):
	// it stays invisible until its ready seams land, then fades in with them. Near the viewer a new
	// chunk appears at once and never waits.
	return CVarAppearHold.GetValueOnGameThread() != 0 && MeshRenderer->WillFadeInChunk(ChunkCoord);
}

void UVoxelChunkManager::BeginSeamHold(const FIntVector& ChunkCoord)
{
	// The LOD change or first mesh (UpdateChunkRenderedLOD) just dirtied every incident seam; the
	// buildable ones are rebuilt within a few ticks. Seams with an absent participant cannot land, so
	// they are not waited for, and one whose participant has no mesh yet is abandoned at dispatch.
	TArray<FVoxelSeamKey> Rebuilding;
	SeamRegistry->GetReadyIncidentSeams(ChunkCoord, Rebuilding);
	SeamHolds.BeginTransition(ChunkCoord, Rebuilding, SeamRegistry->NowSeconds());
}

void UVoxelChunkManager::ApplySeamHoldReleases(const FVoxelSeamTransitionHolds::FRelease& Release)
{
	if (MeshRenderer)
	{
		for (const FIntVector& Chunk : Release.Chunks)
		{
			MeshRenderer->SetChunkFadeHold(Chunk, false);
		}
		for (const FIntVector& Owner : Release.SeamOwners)
		{
			MeshRenderer->SetSeamFadeHold(Owner, false);
		}
	}

	SeamHoldGroupsInterval += Release.HoldSeconds.Num();
	TotalSeamHoldGroups += Release.HoldSeconds.Num();
	SeamHoldTimeoutsInterval += Release.TimedOut;
	TotalSeamHoldTimeouts += Release.TimedOut;
	SeamHoldLeftPendingInterval += Release.SeamsLeftPending;
	for (const float Seconds : Release.HoldSeconds)
	{
		SeamHoldSecondsSumInterval += Seconds;
		SeamHoldSecondsMaxInterval = FMath::Max(SeamHoldSecondsMaxInterval, static_cast<double>(Seconds));
		SeamHoldSecondsMaxLifetime = FMath::Max(SeamHoldSecondsMaxLifetime, static_cast<double>(Seconds));
	}
}

void UVoxelChunkManager::FinishSeamJob(const FVoxelSeamKey& Key, bool bAbandoned)
{
	if (SeamRegistry.IsValid())
	{
		SeamRegistry->NotifySeamJobFinished(Key);
		if (SeamRedirtyAfterFlight.Remove(Key) > 0)
		{
			SeamRegistry->RedirtySeam(Key);
		}
	}
	if (bAbandoned)
	{
		SeamHolds.OnSeamAbandoned(Key);
	}
}

void UVoxelChunkManager::DispatchSeamJob(const FVoxelSeamJob& Job)
{
	// P2 scope: same-LOD FACE (2), EDGE (4), and CORNER (8 participants) seams. Mixed-LOD is
	// P2c/d — dropped jobs stay clean until a participant changes, which re-schedules them
	// (and the filter drops them again until the missing mesher lands).
	const int32 ExpectedParticipants =
		(Job.Key.Type == EVoxelSeamType::Face) ? 2 :
		(Job.Key.Type == EVoxelSeamType::Edge) ? 4 :
		(Job.Key.Type == EVoxelSeamType::Corner) ? 8 : -1;
	if (ExpectedParticipants < 0 || Job.Participants.Num() != ExpectedParticipants)
	{
		SeamRegistry->RecordSeamJobDropped(/*bOlderJobInFlight*/ false);
		FinishSeamJob(Job.Key, /*bAbandoned*/ true);
		return;
	}
	if (SeamJobsInFlight.Contains(Job.Key))
	{
		// An older build of this seam is still on a worker. The registry keeps a seam scheduled until
		// its job finishes, so this only happens after a registry reset; rebuild the seam once the
		// older job lands rather than leaving its geometry stale (see [SeamLat] dropInFlight).
		SeamRegistry->RecordSeamJobDropped(/*bOlderJobInFlight*/ true);
		SeamRedirtyAfterFlight.Add(Job.Key);
		return;
	}
	if (!SeamRegistry->IsResidencyCurrent(Job.Key.Owner, Job.OwnerResidencyEpoch))
	{
		// The owner left (or left and reloaded) while the job waited in the queue. Its result could
		// never be applied (ProcessCompletedSeamMeshes rejects it), and a reload's RegisterChunk has
		// already re-dirtied the seam for the new residency.
		SeamRegistry->RecordSeamJobDropped(/*bOlderJobInFlight*/ false);
		FinishSeamJob(Job.Key, /*bAbandoned*/ true);
		return;
	}

	// Gather live states: every participant must have voxel data and share one rendered LOD.
	// (Mixed-LOD tuples wait for P2c/d; a later LOD change re-dirties and re-schedules.)
	FVoxelChunkState* States[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	for (int32 i = 0; i < ExpectedParticipants; ++i)
	{
		States[i] = ChunkStates.Find(Job.Participants[i].Coord);
		if (!States[i] || !States[i]->Descriptor.HasVoxelDataAvailable())
		{
			SeamRegistry->RecordSeamJobDropped(/*bOlderJobInFlight*/ false);
			FinishSeamJob(Job.Key, /*bAbandoned*/ true);
			return; // a participant vanished between schedule and drain; the unregister path re-dirties
		}
	}
	// Every participant needs a rendered mesh. Participants may differ in LOD — the P2c/P2d
	// mixed-LOD meshers stitch across resolutions; the same-LOD fast paths run when uniform.
	int32 MinParticipantLOD = TNumericLimits<int32>::Max();
	for (int32 i = 0; i < ExpectedParticipants; ++i)
	{
		if (States[i]->MeshedLODLevel < 0)
		{
			SeamRegistry->RecordSeamJobDropped(/*bOlderJobInFlight*/ false);
			FinishSeamJob(Job.Key, /*bAbandoned*/ true);
			return;
		}
		MinParticipantLOD = FMath::Min(MinParticipantLOD, States[i]->MeshedLODLevel);
	}
	const int32 SharedLOD = States[0]->MeshedLODLevel;

	// Worker dispatch mirrors the chunk-mesh CPU path: raw mesher pointer + weak manager for the
	// result enqueue. The seam meshers read only the request + Config (thread-safe). Voxel data
	// is attached as SHARED snapshots (built at most once per chunk content version) — dispatch
	// itself no longer copies voxel arrays. Seam jobs ALWAYS run on the CPU via the dedicated
	// SeamMesher (mode-matched: CPU DC, CPU MC or CPU cubic) — a GPU main mesher cannot serve them.
	IVoxelMesher* SeamMesherPtr = EnsureSeamMesher();
	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);
	const FVoxelSeamKey Key = Job.Key;
	const double DispatchedAt = SeamRegistry->NowSeconds(); // same clock as the job's schedule stamp

	if (Job.Key.Type == EVoxelSeamType::Face)
	{
		FVoxelFaceSeamRequest SeamRequest;
		SeamRequest.OwnerChunkCoord = Key.Owner;
		SeamRequest.Axis = Key.Axis;
		SeamRequest.LODLevel = States[0]->MeshedLODLevel;
		SeamRequest.LODLevelB = States[1]->MeshedLODLevel; // may differ (P2c mixed-LOD path)
		SeamRequest.ChunkSize = Configuration->ChunkSize;
		SeamRequest.VoxelSize = Configuration->VoxelSize;
		SeamRequest.WorldOrigin = Configuration->WorldOrigin;
		SeamRequest.LocalUp = GetChunkLocalUp(Key.Owner);
		SeamRequest.VoxelDataA = GetSeamVoxelSnapshot(Job.Participants[0].Coord, *States[0]);
		SeamRequest.VoxelDataB = GetSeamVoxelSnapshot(Job.Participants[1].Coord, *States[1]);

		const int32 FaceLODLevel = FMath::Min(States[0]->MeshedLODLevel, States[1]->MeshedLODLevel);
		SeamJobsInFlight.Add(Job.Key);
		SeamInFlightPeak = FMath::Max(SeamInFlightPeak, SeamJobsInFlight.Num());
		Async(EAsyncExecution::ThreadPool, [WeakThis, SeamMesherPtr, SeamRequest = MoveTemp(SeamRequest), Key, LODLevel = FaceLODLevel, DirtiedAt = Job.DirtiedAtSeconds, ReadyAt = Job.ReadyAtSeconds, ScheduledAt = Job.ScheduledAtSeconds, DispatchedAt, OwnerEpoch = Job.OwnerResidencyEpoch]() mutable
		{
			FChunkMeshData MeshData;
			const double WorkerStart = FPlatformTime::Seconds();
			const bool bSuccess = SeamMesherPtr->GenerateFaceSeamMeshCPU(SeamRequest, MeshData);
			const double WorkerMs = (FPlatformTime::Seconds() - WorkerStart) * 1000.0;
			if (UVoxelChunkManager* This = WeakThis.Get())
			{
				FCompletedSeamMesh Result;
				Result.Key = Key;
				Result.LODLevel = LODLevel;
				Result.bSuccess = bSuccess;
				Result.DirtiedAtSeconds = DirtiedAt;
				Result.ReadyAtSeconds = ReadyAt;
				Result.ScheduledAtSeconds = ScheduledAt;
				Result.DispatchedAtSeconds = DispatchedAt;
				Result.WorkerMs = WorkerMs;
				Result.OwnerResidencyEpoch = OwnerEpoch;
				if (bSuccess)
				{
					Result.MeshData = MoveTemp(MeshData);
				}
				This->CompletedSeamMeshQueue.Enqueue(MoveTemp(Result));
			}
		});
	}
	else if (Job.Key.Type == EVoxelSeamType::Edge)
	{
		FVoxelEdgeSeamRequest SeamRequest;
		SeamRequest.OwnerChunkCoord = Key.Owner;
		SeamRequest.EdgeAxis = Key.Axis;
		SeamRequest.LODLevel = SharedLOD;
		SeamRequest.ChunkSize = Configuration->ChunkSize;
		SeamRequest.VoxelSize = Configuration->VoxelSize;
		SeamRequest.WorldOrigin = Configuration->WorldOrigin;
		SeamRequest.LocalUp = GetChunkLocalUp(Key.Owner);
		// Registry participant order [Owner, +PerpA, +PerpB, +PerpA+PerpB] (ascending perps)
		// matches the request's quadrant order [(0,0),(1,0),(0,1),(1,1)] by construction.
		for (int32 i = 0; i < 4; ++i)
		{
			SeamRequest.LODLevels[i] = States[i]->MeshedLODLevel; // may differ (P2d mixed path)
			SeamRequest.VoxelData[i] = GetSeamVoxelSnapshot(Job.Participants[i].Coord, *States[i]);
		}

		SeamJobsInFlight.Add(Job.Key);
		SeamInFlightPeak = FMath::Max(SeamInFlightPeak, SeamJobsInFlight.Num());
		Async(EAsyncExecution::ThreadPool, [WeakThis, SeamMesherPtr, SeamRequest = MoveTemp(SeamRequest), Key, LODLevel = MinParticipantLOD, DirtiedAt = Job.DirtiedAtSeconds, ReadyAt = Job.ReadyAtSeconds, ScheduledAt = Job.ScheduledAtSeconds, DispatchedAt, OwnerEpoch = Job.OwnerResidencyEpoch]() mutable
		{
			FChunkMeshData MeshData;
			const double WorkerStart = FPlatformTime::Seconds();
			const bool bSuccess = SeamMesherPtr->GenerateEdgeSeamMeshCPU(SeamRequest, MeshData);
			const double WorkerMs = (FPlatformTime::Seconds() - WorkerStart) * 1000.0;
			if (UVoxelChunkManager* This = WeakThis.Get())
			{
				FCompletedSeamMesh Result;
				Result.Key = Key;
				Result.LODLevel = LODLevel;
				Result.bSuccess = bSuccess;
				Result.DirtiedAtSeconds = DirtiedAt;
				Result.ReadyAtSeconds = ReadyAt;
				Result.ScheduledAtSeconds = ScheduledAt;
				Result.DispatchedAtSeconds = DispatchedAt;
				Result.WorkerMs = WorkerMs;
				Result.OwnerResidencyEpoch = OwnerEpoch;
				if (bSuccess)
				{
					Result.MeshData = MoveTemp(MeshData);
				}
				This->CompletedSeamMeshQueue.Enqueue(MoveTemp(Result));
			}
		});
	}
	else // EVoxelSeamType::Corner
	{
		FVoxelCornerSeamRequest SeamRequest;
		SeamRequest.OwnerChunkCoord = Key.Owner;
		SeamRequest.LODLevel = SharedLOD;
		SeamRequest.ChunkSize = Configuration->ChunkSize;
		SeamRequest.VoxelSize = Configuration->VoxelSize;
		SeamRequest.WorldOrigin = Configuration->WorldOrigin;
		SeamRequest.LocalUp = GetChunkLocalUp(Key.Owner);
		// Registry participant order (X innermost: i + j*2 + k*4) matches the request's octant
		// order dx + dy*2 + dz*4 by construction.
		for (int32 i = 0; i < 8; ++i)
		{
			SeamRequest.LODLevels[i] = States[i]->MeshedLODLevel; // may differ (P2d mixed path)
			SeamRequest.VoxelData[i] = GetSeamVoxelSnapshot(Job.Participants[i].Coord, *States[i]);
		}

		SeamJobsInFlight.Add(Job.Key);
		SeamInFlightPeak = FMath::Max(SeamInFlightPeak, SeamJobsInFlight.Num());
		Async(EAsyncExecution::ThreadPool, [WeakThis, SeamMesherPtr, SeamRequest = MoveTemp(SeamRequest), Key, LODLevel = MinParticipantLOD, DirtiedAt = Job.DirtiedAtSeconds, ReadyAt = Job.ReadyAtSeconds, ScheduledAt = Job.ScheduledAtSeconds, DispatchedAt, OwnerEpoch = Job.OwnerResidencyEpoch]() mutable
		{
			FChunkMeshData MeshData;
			const double WorkerStart = FPlatformTime::Seconds();
			const bool bSuccess = SeamMesherPtr->GenerateCornerSeamMeshCPU(SeamRequest, MeshData);
			const double WorkerMs = (FPlatformTime::Seconds() - WorkerStart) * 1000.0;
			if (UVoxelChunkManager* This = WeakThis.Get())
			{
				FCompletedSeamMesh Result;
				Result.Key = Key;
				Result.LODLevel = LODLevel;
				Result.bSuccess = bSuccess;
				Result.DirtiedAtSeconds = DirtiedAt;
				Result.ReadyAtSeconds = ReadyAt;
				Result.ScheduledAtSeconds = ScheduledAt;
				Result.DispatchedAtSeconds = DispatchedAt;
				Result.WorkerMs = WorkerMs;
				Result.OwnerResidencyEpoch = OwnerEpoch;
				if (bSuccess)
				{
					Result.MeshData = MoveTemp(MeshData);
				}
				This->CompletedSeamMeshQueue.Enqueue(MoveTemp(Result));
			}
		});
	}
}

void UVoxelChunkManager::ProcessCompletedSeamMeshes()
{
	// Budget the per-tick submits: each one converts vertices on the game thread and enqueues a
	// render command. Leftovers keep their in-flight slots, so dispatch backpressures naturally
	// and the work spreads across ticks instead of spiking the traverse frame.
	const int32 MaxSeamSubmitsPerTick = FMath::Max(1, CVarSeamMaxSubmitsPerTick.GetValueOnGameThread());
	int32 Processed = 0;
	const double SubmitStart = FPlatformTime::Seconds();

	VoxelDrainBounded(CompletedSeamMeshQueue, MaxSeamSubmitsPerTick, [&](FCompletedSeamMesh& Result) -> bool
	{
		++Processed;
		SeamJobsInFlight.Remove(Result.Key);

		if (!MeshRenderer)
		{
			FinishSeamJob(Result.Key, /*bAbandoned*/ true);
			return true;
		}
		// R23a: a result applies only to the owner residency it was scheduled against. A job
		// dispatched just before its owner unloaded completes afterwards; without this check the
		// FindOrAdd below recreated the owner's slot entry and submitted a bucket for a chunk that no
		// longer exists. The unload had already removed the owner's buckets, so nothing ever removed
		// that one: seam strips floating over empty terrain after a teleport. The epoch also rejects
		// a result from an EARLIER residency of an owner that has since reloaded; that reload's
		// RegisterChunk re-dirtied every incident seam, so the new residency rebuilds them.
		if (!SeamRegistry.IsValid() || !SeamRegistry->IsResidencyCurrent(Result.Key.Owner, Result.OwnerResidencyEpoch))
		{
			if (SeamRegistry.IsValid())
			{
				SeamRegistry->RecordSeamResultRejected();
			}
			UE_LOG(LogVoxelStreaming, Verbose, TEXT("Seam result %s rejected: owner left while the job was in flight"), *Result.Key.ToString());
			FinishSeamJob(Result.Key, /*bAbandoned*/ true);
			return true;
		}
		if (!Result.bSuccess)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Seam job %s failed to mesh"), *Result.Key.ToString());
			FinishSeamJob(Result.Key, /*bAbandoned*/ true);
			return true;
		}

		// Latency instrumentation: this is the moment the seam's geometry becomes visible, so
		// report dirty->completed for the near tier (near-ness judged against the viewer NOW).
		if (SeamRegistry.IsValid())
		{
			const bool bNear = FVoxelSeamRegistry::IsNearViewer(
				Result.Key.Owner, WorldToChunkCoord(CurrentViewerPosition),
				FMath::Max(0, CVarSeamNearChunkRadius.GetValueOnGameThread()));
			SeamRegistry->RecordSeamCompleted(Result.DirtiedAtSeconds, Result.ReadyAtSeconds, Result.ScheduledAtSeconds, bNear,
				Result.DispatchedAtSeconds, Result.WorkerMs);
		}

		// Store the slot mesh, then submit ONE merged bucket per owner. Per-slot render buckets
		// (up to 7/chunk) exhausted the scene proxy's per-frame batch cap on near geometry, so
		// far seams never drew ("seams vanish at distance"). Merging is a cheap strip concat;
		// an all-empty merge removes the bucket. Slot: 0-2 face +X/+Y/+Z, 3-5 edge, 6 corner.
		const uint8 SeamSlot =
			(Result.Key.Type == EVoxelSeamType::Face) ? Result.Key.Axis :
			(Result.Key.Type == EVoxelSeamType::Edge) ? static_cast<uint8>(3 + Result.Key.Axis) : static_cast<uint8>(6);
		FSeamOwnerSlots& OwnerSlots = SeamOwnerSlots.FindOrAdd(Result.Key.Owner);
		OwnerSlots.Slots[SeamSlot] = MoveTemp(Result.MeshData); // may be empty = seam dissolved
		OwnerSlots.SlotLOD[SeamSlot] = Result.LODLevel;

		// R24: the bucket crossfades from the one it replaces. If an LOD transition is waiting for
		// this seam (or already holds this owner's bucket), the submit holds the new bucket at alpha 0
		// so it fades in with the interior it meets when the transition releases.
		SeamHolds.OnSeamResult(Result.Key);

		// A bucket still fading from an earlier submit would lose the mesh it fades out of if replaced
		// now (one retained generation), snapping the other slots' transitions to their end state. Its
		// slots are stored; submit the merged bucket once that fade ends (FlushDueSeamBuckets), which
		// also folds any further results that land meanwhile into the same swap.
		const float FadeRemaining = MeshRenderer->GetSeamFadeRemainingSeconds(Result.Key.Owner);
		if (FadeRemaining > 0.0f || SeamHolds.IsBucketDeferred(Result.Key.Owner))
		{
			SeamHolds.DeferBucket(Result.Key.Owner, SeamRegistry->NowSeconds() + FadeRemaining);
			++SeamBucketDeferralsInterval;
		}
		else
		{
			SubmitSeamOwnerBucket(Result.Key.Owner);
		}
		if (bBenchmarkViewActive)
		{
			++BenchSeamJobsCompleted;
		}
		FinishSeamJob(Result.Key, /*bAbandoned*/ false);
		return true;
	});

	// Instrumentation: the game-thread cost of this tick's submits (only ticks that submitted).
	if (Processed > 0)
	{
		const double Ms = (FPlatformTime::Seconds() - SubmitStart) * 1000.0;
		SeamSubmitMsAccum += Ms;
		SeamSubmitMsMax = FMath::Max(SeamSubmitMsMax, Ms);
		++SeamSubmitTicks;
		SeamSubmitCount += Processed;
	}
}

void UVoxelChunkManager::SubmitSeamOwnerBucket(const FIntVector& Owner)
{
	const FSeamOwnerSlots* OwnerSlots = SeamOwnerSlots.Find(Owner);
	if (!OwnerSlots || !MeshRenderer)
	{
		return;
	}

	FChunkMeshData Merged;
	int32 MinLOD = TNumericLimits<int32>::Max();
	for (int32 s = 0; s < 7; ++s)
	{
		const FChunkMeshData& SlotMesh = OwnerSlots->Slots[s];
		if (!SlotMesh.IsValid())
		{
			continue;
		}
		MinLOD = FMath::Min(MinLOD, FMath::Max(OwnerSlots->SlotLOD[s], 0));
		const uint32 Base = static_cast<uint32>(Merged.Positions.Num());
		Merged.Positions.Append(SlotMesh.Positions);
		Merged.Normals.Append(SlotMesh.Normals);
		Merged.UVs.Append(SlotMesh.UVs);
		Merged.UV1s.Append(SlotMesh.UV1s);
		Merged.Colors.Append(SlotMesh.Colors);
		Merged.Indices.Reserve(Merged.Indices.Num() + SlotMesh.Indices.Num());
		for (const uint32 Idx : SlotMesh.Indices)
		{
			Merged.Indices.Add(Base + Idx);
		}
	}
	if (MinLOD == TNumericLimits<int32>::Max())
	{
		MinLOD = 0; // all slots empty: the submit removes the bucket, the LOD is unused
	}

	if (SeamHolds.IsSeamOwnerHeld(Owner))
	{
		MeshRenderer->SetSeamFadeHold(Owner, true);
	}
	MeshRenderer->UpdateSeamMeshFromCPU(Owner, 0, MinLOD, MoveTemp(Merged), /*bCrossfade*/ true);
}

void UVoxelChunkManager::FlushDueSeamBuckets()
{
	if (SeamHolds.NumDeferredBuckets() == 0)
	{
		return;
	}
	const double Now = SeamRegistry->NowSeconds();
	TArray<FIntVector> Due;
	SeamHolds.CollectDueBuckets(Now, Due);
	for (const FIntVector& Owner : Due)
	{
		// The renderer's fade clock is authoritative: if it has not quite finished (clock skew of a
		// frame), wait for it rather than replace the bucket at alpha 0.99.
		const float Remaining = MeshRenderer ? MeshRenderer->GetSeamFadeRemainingSeconds(Owner) : 0.0f;
		if (Remaining > 0.0f)
		{
			SeamHolds.DeferBucket(Owner, Now + Remaining);
			continue;
		}
		SubmitSeamOwnerBucket(Owner);
	}
}

TSharedPtr<const TArray<FVoxelData>> UVoxelChunkManager::GetSeamVoxelSnapshot(const FIntVector& Coord, FVoxelChunkState& State)
{
	const uint32 Version = State.Descriptor.ContentVersion;
	if (FSeamVoxelSnapshot* Cached = SeamSnapshotCache.Find(Coord))
	{
		if (Cached->ContentVersion == Version && Cached->Data.IsValid())
		{
			Cached->LastUsedTick = SeamTickCounter;
			return Cached->Data;
		}
	}

	// Build once per (chunk, content version) — the only remaining game-thread voxel copy in the
	// seam pipeline. ContentVersion covers edits (bumped on explicit voxel edits), so the merged
	// snapshot stays valid exactly as long as the seams that would read it.
	TSharedPtr<TArray<FVoxelData>> Built = MakeShared<TArray<FVoxelData>>(State.Descriptor.GetVoxelDataForRead());
	if (EditManager && EditManager->ChunkHasEdits(Coord))
	{
		EditManager->ApplyEditsToVoxelData(Coord, *Built);
	}

	FSeamVoxelSnapshot& Entry = SeamSnapshotCache.FindOrAdd(Coord);
	Entry.ContentVersion = Version;
	Entry.LastUsedTick = SeamTickCounter;
	Entry.Data = Built;
	return Entry.Data;
}

void UVoxelChunkManager::ProcessFarCompressionSweep(const FLODQueryContext& Context, FVoxelTimingStats& Timing)
{
	const bool bEnabled = CVarFarCompression.GetValueOnGameThread() != 0;
	const bool bAsync = CVarFarCompressionAsync.GetValueOnGameThread() != 0;
	const int32 MinStride = FMath::Max(1, CVarFarCompressionMinStride.GetValueOnGameThread());
	const int64 IdleFrames = FMath::Max(0, CVarFarCompressionIdleFrames.GetValueOnGameThread());
	const int32 MaxScans = FMath::Max(1, CVarFarCompressionMaxScansPerTick.GetValueOnGameThread());
	const int32 MaxInFlight = FMath::Max(1, CVarFarCompressionMaxInFlight.GetValueOnGameThread());
	const EVoxelChunkCodec Codec = static_cast<EVoxelChunkCodec>(FMath::Clamp(CVarFarCompressionCodec.GetValueOnGameThread(), 0, 5));

	// "Far" is keyed on distance: the chunk's distance band must have reached MinStride. With the
	// LOD system running, the chunk's LOD level is that band (distance plus hysteresis and 2:1
	// balance). With it off, as in every cubic world, all chunks are LOD 0, so the strategy maps the
	// chunk's distance onto the same bands instead; without that, nothing would ever compress.
	const bool bLODActive = Configuration && Configuration->IsLODEnabled();

	// Only settled, currently-resident far chunks that aren't pending a remesh.
	auto IsFarCandidate = [&](const FIntVector& Coord, const FVoxelChunkState& S) -> bool
	{
		if (S.State != EChunkState::Loaded) return false;
		if (S.Descriptor.Residency != EVoxelDataResidency::Resident) return false;
		if (S.Descriptor.bIsDirty) return false;
		// Idle: LastStateChangeFrame is the frame the chunk entered Loaded; it stays fixed while
		// settled and resets on any remesh (Loaded -> PendingMeshing -> ... -> Loaded).
		if ((CurrentFrame - S.LastStateChangeFrame) < IdleFrames) return false;
		const int32 BandLOD = (bLODActive || !LODStrategy) ? S.LODLevel : LODStrategy->GetDistanceBandLOD(Coord, Context);
		return (1 << FMath::Clamp(BandLOD, 0, 7)) >= MinStride;
	};

	// Install finished async encodes. Drained even with the sweep off, so their copies come back.
	int32 Applied = 0;
	FFarCompressResult Result;
	while (FarCompressResults->Dequeue(Result))
	{
		--FarCompressJobsInFlight;
		FarCompressBytesInFlight -= Result.Scratch.GetAllocatedSize();
		if (FarCompressScratchPool.Num() + FarCompressJobsInFlight < MaxInFlight)
		{
			FarCompressScratchPool.Add(MoveTemp(Result.Scratch));
		}
		Result.Scratch.Empty(); // frees a copy the pool had no room for

		// A mismatched ticket means the chunk unloaded, or its array was written or changed residency
		// after the copy was taken (every write path clears the ticket): the form no longer describes it.
		FVoxelChunkState* S = ChunkStates.Find(Result.ChunkCoord);
		if (!S || S->Descriptor.AsyncCompactTicket != Result.Ticket)
		{
			++FarCompressStale;
			continue;
		}
		// The form matches the array, so keep it either way. Drop the raw array only if the chunk still
		// qualifies; one that started a re-mesh or came closer meanwhile keeps it, and the sweep
		// re-applies the cached form for free once it qualifies again.
		const bool bDrop = bEnabled && IsFarCandidate(Result.ChunkCoord, *S);
		S->Descriptor.InstallCompactForm(MoveTemp(Result.Form), bDrop);
		++FarCompressInstalled;
		++Applied;
	}
	Timing.FarCompressApplyCount = Applied;

	if (!bEnabled)
	{
		if (FarCompressJobsInFlight == 0)
		{
			FarCompressScratchPool.Empty();
		}
		return;
	}

	int32 Budget = 0;
	for (auto& Pair : ChunkStates)
	{
		FVoxelChunkState& S = Pair.Value;
		FChunkDescriptor& D = S.Descriptor;

		if (!IsFarCandidate(Pair.Key, S)) continue;

		if (D.bUniformValueValid || D.CompressedVoxelData.Num() > 0)
		{
			// A compact form is already cached (the chunk was expanded by access); re-apply it for
			// free — no scan, no re-encode. Not budgeted. The seam pipeline reads neighbours through
			// its snapshot cache (one decode per content version), so re-compacting at once is safe.
			D.TryCompress(Codec);
			continue;
		}
		if (D.bCompressionEvaluated)
		{
			// Already attempted; neither uniform nor the codec beat raw. Skip until content changes.
			continue;
		}
		if (D.AsyncCompactTicket != 0)
		{
			// An encode of exactly this array is in flight.
			continue;
		}
		if (Budget >= MaxScans || (bAsync && FarCompressJobsInFlight >= MaxInFlight))
		{
			// Out of budget this tick; retry next tick (bCompressionEvaluated stays false).
			continue;
		}
		++Budget;
		++FarCompressEncodes;
		if (bAsync)
		{
			DispatchFarCompression(Pair.Key, D, Codec);
		}
		else
		{
			D.TryCompress(Codec);
		}
	}
	Timing.FarCompressDispatchCount = Budget;

	// Keep the scratch arrays while encodes keep coming; release them once the sweep has been idle.
	if (Budget > 0 || FarCompressJobsInFlight > 0)
	{
		FarCompressLastBusyFrame = CurrentFrame;
	}
	else if (FarCompressScratchPool.Num() > 0 && (CurrentFrame - FarCompressLastBusyFrame) >= IdleFrames)
	{
		FarCompressScratchPool.Empty();
	}
}

void UVoxelChunkManager::DispatchFarCompression(const FIntVector& ChunkCoord, FChunkDescriptor& Descriptor, EVoxelChunkCodec Codec)
{
	if (++NextFarCompressTicket == 0)
	{
		++NextFarCompressTicket; // 0 means "no encode in flight"
	}
	const uint32 Ticket = NextFarCompressTicket;
	Descriptor.AsyncCompactTicket = Ticket;

	// Encode from a copy: the resident array stays readable (neighbour reads, seam snapshots, point
	// queries) and writable while the worker runs, and a write clears the ticket, which voids the result.
	// The game-thread cost is one memcpy into a pooled array, against ~3.5 ms for the encode.
	TArray<FVoxelData> Copy = FarCompressScratchPool.Num() > 0 ? FarCompressScratchPool.Pop(EAllowShrinking::No) : TArray<FVoxelData>();
	const int32 Num = Descriptor.VoxelData.Num();
	Copy.SetNumUninitialized(Num, EAllowShrinking::No);
	FMemory::Memcpy(Copy.GetData(), Descriptor.VoxelData.GetData(), static_cast<SIZE_T>(Num) * sizeof(FVoxelData));
	++FarCompressJobsInFlight;
	FarCompressBytesInFlight += Copy.GetAllocatedSize();

	// Background priority, outside the thread pool that runs generation, meshing and seam jobs:
	// compression only saves memory, so it never delays the streaming front.
	UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[Results = FarCompressResults, ChunkCoord, Ticket, Codec, ChunkSize = Descriptor.ChunkSize, Copy = MoveTemp(Copy)]() mutable
		{
			FFarCompressResult Done;
			Done.ChunkCoord = ChunkCoord;
			Done.Ticket = Ticket;
			Done.Form = FChunkDescriptor::ComputeCompactForm(Copy, Codec, ChunkSize);
			Done.Scratch = MoveTemp(Copy);
			Results->Enqueue(MoveTemp(Done));
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);
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

	// No neighbour re-meshes: chunk meshes never read neighbour data. This chunk's arrival restales
	// the seams it participates in, through the registry below.

	// Seam-ownership: this chunk's voxel data is now available — register it (restales its incident
	// seams). ContentVersion is the seam-invalidation key; rendered LOD is not set yet (mesh follows).
	if (SeamRegistry.IsValid())
	{
		SeamRegistry->RegisterChunk(ChunkCoord, State->Descriptor.ContentVersion, State->MeshedLODLevel);
	}

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
					// R24: an LOD change (or a faded-in first appearance, P3) holds the swap until its seams land (see BeginSeamHold).
					const bool bHoldForSeams = ShouldHoldForSeams(ChunkCoord, PreviousMeshedLOD,
						PendingMeshQueue[i].LODLevel, PendingMeshQueue[i].MeshData);
					if (bHoldForSeams)
					{
						MeshRenderer->SetChunkFadeHold(ChunkCoord, true);
					}

					// Entry is discarded right after — move the mesh data into the renderer
					const double RendT0 = FPlatformTime::Seconds();
					MeshRenderer->UpdateChunkMeshFromCPU(
						ChunkCoord,
						PendingMeshQueue[i].LODLevel,
						MoveTemp(PendingMeshQueue[i].MeshData));
					SubmitRendererSecondsThisTick += FPlatformTime::Seconds() - RendT0;
					State->MeshedLODLevel = PendingMeshQueue[i].LODLevel;
					// Seam-ownership P0: rendered LOD known — refresh the seam registry (no-op if unchanged).
					if (SeamRegistry.IsValid())
					{
						SeamRegistry->UpdateChunkRenderedLOD(ChunkCoord, State->MeshedLODLevel);
					}
					if (bHoldForSeams)
					{
						BeginSeamHold(ChunkCoord);
					}
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

		// Safety net: if chunk is in an unexpected state (not actively transitioning and not Loaded),
		// force it to Loaded to avoid permanently invisible chunks. (A chunk already Loaded needs
		// nothing more: the seam registry restaled its seams through UpdateChunkRenderedLOD above.)
		if (bMeshSubmitted && State &&
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

		// R24: an LOD change (or a faded-in first appearance, P3) holds the swap until its seams land (see BeginSeamHold).
		const bool bHoldForSeams = ShouldHoldForSeams(ChunkCoord, PreviousMeshedLOD, PendingMesh.LODLevel, PendingMesh.MeshData);
		if (bHoldForSeams)
		{
			MeshRenderer->SetChunkFadeHold(ChunkCoord, true);
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
		// Seam-ownership P0: rendered LOD known — refresh the seam registry (no-op if unchanged).
		if (SeamRegistry.IsValid())
		{
			SeamRegistry->UpdateChunkRenderedLOD(ChunkCoord, State->MeshedLODLevel);
		}
		if (bHoldForSeams)
		{
			BeginSeamHold(ChunkCoord);
		}
		if (bBenchmarkViewActive) { BenchEverMeshed.Add(ChunkCoord); }

		// Propagate water flags from loaded neighbors into this chunk's voxel data
		// so caves connected across chunk boundaries receive consistent water flags.
		if (Configuration && Configuration->bEnableWaterLevel)
		{
			const double WaterT0 = FPlatformTime::Seconds();
			PropagateWaterFromNeighbors(ChunkCoord);

			// Update this chunk's contribution to its 2D water tile
			if (UsesWaterTiles())
			{
				UpdateWaterTileContribution(ChunkCoord);
			}
			SubmitWaterSecondsThisTick += FPlatformTime::Seconds() - WaterT0;
		}

		// Remove from pending queue — O(1) swap since order doesn't matter (accessed by coord)
		PendingMeshQueue.RemoveAtSwap(PendingIndex, EAllowShrinking::No);
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

bool UVoxelChunkManager::UsesWaterTiles() const
{
	// The tiles are XY columns meshed at the WaterLevel plane; a planet's sea is a sphere (its surface is the world
	// actor's water sphere until SP-E2), so the tiles would draw in the wrong place there.
	return Configuration && Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial
		&& Configuration->WorldMode != EWorldMode::SphericalPlanet;
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
	const FVector ChunkWorldPos = FVoxelCoordinates::ChunkToWorld(ChunkCoord, CS, VS);

	if (!State->Descriptor.HasVoxelDataAvailable())
	{
		return false;
	}

	// "Below the sea" as generation's fill pass tests it: the WaterLevel plane, or a planet's WaterRadius sphere (SP-B8).
	// The sphere is tested in world space (the chunk grid is relative to WorldOrigin, the planet centre); the plane keeps
	// its Z test on the grid position.
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*Configuration);
	const FVector VoxelOrigin = Sea.bRadial ? ChunkWorldPos + Configuration->WorldOrigin : ChunkWorldPos;
	auto IsBelowSea = [&Sea, &VoxelOrigin, VS](int32 X, int32 Y, int32 Z)
	{
		return Sea.IsBelow(VoxelOrigin + FVector(X, Y, Z) * VS);
	};

	// Entirely above the sea — no voxel can qualify as a seed (the per-voxel check
	// below rejects everything), so skip the 6-face boundary scan outright. This runs per
	// mesh submit and most submitted chunks are above the sea.
	if (Sea.IsAbove(FBox(VoxelOrigin, VoxelOrigin + FVector((CS - 1) * VS))))
	{
		return false;
	}

	// Plane: voxels above the water level never seed — clamp the Z iteration up front instead of
	// rejecting them one by one. For the X/Y faces, loop axis B is Z. A sphere checks every voxel.
	const int32 MaxSeedZ = Sea.bRadial ? CS - 1 : FMath::Clamp(FMath::FloorToInt32((Sea.PlaneZ - ChunkWorldPos.Z) / VS), 0, CS - 1);

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
		if (!Sea.bRadial && F == 4 && (ChunkWorldPos.Z + MaxIdx * VS) > Sea.PlaneZ)
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

				// Our voxel must be dry air below the sea
				if (OurVoxel.IsSolid() || OurVoxel.HasWaterFlag())
				{
					continue;
				}

				if (!IsBelowSea(OurX, OurY, OurZ))
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

			if (!IsBelowSea(NX, NY, NZ))
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

void UVoxelChunkManager::RecoverLostRenderData()
{
	TArray<FIntVector> LostChunks;
	if (!MeshRenderer || !MeshRenderer->ConsumeLostRenderData(LostChunks))
	{
		return;
	}

	// Chunks still in the pipeline submit to the new proxy on their own; a Loaded chunk's mesh is the one lost.
	int32 Remeshed = 0;
	for (const FIntVector& Coord : LostChunks)
	{
		if (GetChunkState(Coord) == EChunkState::Loaded)
		{
			MarkChunkDirty(Coord);
			++Remeshed;
		}
	}

	// The merged seam buckets are rebuilt from the slots kept here; no seam job has to run again.
	TArray<FIntVector> SeamOwners;
	SeamOwnerSlots.GetKeys(SeamOwners);
	for (const FIntVector& Owner : SeamOwners)
	{
		SubmitSeamOwnerBucket(Owner);
	}

	for (TPair<FIntVector2, FWaterTileState>& Tile : WaterTiles)
	{
		Tile.Value.bDirty = true;
		if (!DirtyWaterTileSet.Contains(Tile.Key))
		{
			DirtyWaterTileQueue.Add(Tile.Key);
			DirtyWaterTileSet.Add(Tile.Key);
		}
	}

	UE_LOG(LogVoxelStreaming, Log, TEXT("Renderer lost its geometry (scene proxy recreated): re-meshing %d chunk(s), re-sending %d seam bucket(s) and %d water tile(s)"),
		Remeshed, SeamOwners.Num(), WaterTiles.Num());
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

	// Benchmark thrash is counted when the job launches (ProcessMeshingQueue); remember why it was queued.
	if (bBenchmarkViewActive)
	{
		BenchQueuedRemeshReason.FindOrAdd(Request.ChunkCoord, Reason);
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

// Console: voxel.Bench.Run [tag] [velocityUU] [distanceUU] [key=value ...]
// Starts a deterministic traverse from the current view position and writes a report under
// Saved/VoxelBench/. Optional key=value arguments (':' separates components and '/' separates route
// points, because -ExecCmds splits commands on commas):
//   dir=X:Y[:Z]            straight-path direction (default +X)
//   start=X:Y[:Z]          start point instead of the player view; Z defaults to 500 above the surface
//   route=X:Y[:Z]/X:Y/...  fly a polyline from the start through these points (distance 0 = whole route)
//   orbit=CX:CY:R          fly a circle around (CX, CY) (distance 0 = one lap)
//   follow=Alt             fly Alt uu above the analytic surface (heightmap world modes)
//   warmup=Sec             max settle time before the traverse (record how long a world takes to build)
//   catchup=Sec            max drain time after the traverse
//   build=Label            free-form build label for the report (the sweep script passes the commit)
static bool ParseBenchVector(const FString& Value, FVector& Out, bool& bOutHasZ)
{
	TArray<FString> Parts;
	Value.ParseIntoArray(Parts, TEXT(":"), true);
	if (Parts.Num() < 2 || Parts.Num() > 3)
	{
		return false;
	}
	Out = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), Parts.Num() == 3 ? FCString::Atod(*Parts[2]) : 0.0);
	bOutHasZ = Parts.Num() == 3;
	return true;
}

static FAutoConsoleCommandWithWorldAndArgs GVoxelBenchRunCmd(
	TEXT("voxel.Bench.Run"),
	TEXT("Run a streaming benchmark: voxel.Bench.Run [tag] [velocityUU] [distanceUU] [dir=X:Y[:Z]] [start=X:Y[:Z]] ")
	TEXT("[route=X:Y[:Z]/X:Y[:Z]/...] [orbit=CX:CY:R] [gc=Lat:Lon:Heading[:Alt]] [follow=AltUU] [warmup=Sec] [catchup=Sec] [build=Label]"),
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
		TArray<FString> Positional;
		bool bHasStart = false, bStartHasZ = false, bDistanceGiven = false, bHasGreatCircle = false;
		FVector StartArg = FVector::ZeroVector;
		for (const FString& Arg : Args)
		{
			FString Key, Value;
			if (!Arg.Split(TEXT("="), &Key, &Value))
			{
				Positional.Add(Arg);
				continue;
			}
			Key = Key.ToLower();
			bool bHasZ = false;
			FVector Vec;
			if (Key == TEXT("dir") && ParseBenchVector(Value, Vec, bHasZ))
			{
				Config.Direction = Vec;
			}
			else if (Key == TEXT("start") && ParseBenchVector(Value, StartArg, bStartHasZ))
			{
				bHasStart = true;
			}
			else if (Key == TEXT("route"))
			{
				TArray<FString> Points;
				Value.ParseIntoArray(Points, TEXT("/"), true);
				for (const FString& Point : Points)
				{
					if (ParseBenchVector(Point, Vec, bHasZ))
					{
						// No Z given: a sentinel, replaced by the start altitude once the start is known.
						Config.RoutePoints.Add(FVector(Vec.X, Vec.Y, bHasZ ? Vec.Z : TNumericLimits<double>::Max()));
					}
				}
				Config.PathType = EVoxelBenchPath::Route;
			}
			else if (Key == TEXT("orbit"))
			{
				TArray<FString> Parts;
				Value.ParseIntoArray(Parts, TEXT(":"), true);
				if (Parts.Num() == 3)
				{
					Config.OrbitCenter = FVector2D(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]));
					Config.OrbitRadius = FCString::Atof(*Parts[2]);
					Config.PathType = EVoxelBenchPath::Orbit;
				}
			}
			else if (Key == TEXT("gc"))
			{
				// A planet's great circle (spherical Phase 4, SP-I4): latitude, longitude, heading from north, flight altitude.
				TArray<FString> Parts;
				Value.ParseIntoArray(Parts, TEXT(":"), true);
				const UVoxelWorldConfiguration* Cfg = CM->GetConfiguration();
				if (Parts.Num() >= 3 && Cfg && Cfg->WorldMode == EWorldMode::SphericalPlanet)
				{
					FVoxelStreamingBenchmark::MakeGreatCircle(Config, Cfg->WorldOrigin, Cfg->WorldRadius, FCString::Atod(*Parts[0]),
						FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]), Parts.Num() > 3 ? FCString::Atof(*Parts[3]) : 500.0f);
					bHasGreatCircle = true;
				}
				else
				{
					UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run: gc=Lat:Lon:Heading[:Alt] needs a planet world and three values"));
				}
			}
			else if (Key == TEXT("follow")) { Config.FollowTerrainAltitude = FCString::Atof(*Value); }
			else if (Key == TEXT("warmup")) { Config.WarmupTimeoutSec = FCString::Atof(*Value); }
			else if (Key == TEXT("catchup")) { Config.CatchUpTimeoutSec = FCString::Atof(*Value); }
			else if (Key == TEXT("build")) { Config.BuildLabel = Value; }
			else
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run: ignoring unrecognised argument '%s'"), *Arg);
			}
		}
		if (Positional.Num() > 0) { Config.Tag = Positional[0]; }
		if (Positional.Num() > 1) { Config.VelocityUU = FCString::Atof(*Positional[1]); }
		if (Positional.Num() > 2) { Config.TraverseDistance = FCString::Atof(*Positional[2]); bDistanceGiven = true; }
		// Route and orbit default to the whole path; FVoxelStreamingBenchmark resolves <= 0 to its length.
		if (Config.PathType != EVoxelBenchPath::Straight && !bDistanceGiven)
		{
			Config.TraverseDistance = 0.0f;
		}

		// Start: an explicit start= (Z defaults to 500 above the analytic surface), else the player view
		// (on the ground, so the leading edge stays at LOD0), else the chunk-manager owner.
		FVector Start = FVector::ZeroVector;
		if (bHasGreatCircle)
		{
			Start = Config.StartPosition; // the great circle's own start
		}
		else if (bHasStart)
		{
			Start = StartArg;
			if (!bStartHasZ)
			{
				// 500 above the analytic surface (along local up on a planet).
				if (CM->GetWorldBounds().Shape == EVoxelWorldBoundsShape::Shell)
				{
					const FVector Surface = CM->GetGeneratedSurfacePoint(Start);
					Start = Surface + CM->GetUpVectorAt(Surface) * 500.0;
				}
				else
				{
					Start.Z = CM->GetGeneratedSurfaceHeight(Start.X, Start.Y) + 500.0;
				}
			}
		}
		else if (APlayerController* PC = TargetWorld->GetFirstPlayerController())
		{
			FVector Loc; FRotator Rot;
			PC->GetPlayerViewPoint(Loc, Rot);
			Start = Loc;
		}
		if (!bHasStart && Start.IsNearlyZero())
		{
			if (const AActor* Owner = CM->GetOwner()) { Start = Owner->GetActorLocation(); }
			Start.Z += 500.0f; // keep slightly above the surface
		}
		Config.StartPosition = Start;
		for (FVector& Point : Config.RoutePoints)
		{
			if (Point.Z == TNumericLimits<double>::Max()) { Point.Z = Start.Z; }
		}

		CM->StartBenchmark(Config);
		UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run '%s': world=%s start=(%.0f,%.0f,%.0f) vel=%.0f dist=%.0f path=%d follow=%.0f"),
			*Config.Tag, *TargetWorld->GetName(), Start.X, Start.Y, Start.Z, Config.VelocityUU, Config.TraverseDistance,
			static_cast<int32>(Config.PathType), Config.FollowTerrainAltitude);
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
			int64 Encodes = 0, Installed = 0, Stale = 0;
			CM->GetBenchFarCompressionCounts(Encodes, Installed, Stale);
			UE_LOG(LogVoxelStreaming, Warning,
				TEXT("voxel.FarCompression.Stats: Resident=%d Uniform=%d Compressed=%d Empty=%d | ResidentVoxelData=%.1fMB Reclaimed=%.1fMB | encodes=%lld installed=%lld stale=%lld"),
				M.ResidentChunks, M.UniformChunks, M.CompressedChunks, M.EmptyChunks,
				M.VoxelDataBytes / (1024.0 * 1024.0), M.CompressionSavedBytes / (1024.0 * 1024.0),
				Encodes, Installed, Stale);
		}
		if (Managers == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Stats: no UVoxelChunkManager found"));
		}
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
		for (TActorIterator<AActor> It(TargetWorld); It; ++It)
		{
			const UVoxelChunkManager* CM = It->FindComponentByClass<UVoxelChunkManager>();
			if (!CM) { continue; }
			++Managers;
			FString Report;
			CM->AuditResidency(Report);
			TArray<FString> Lines;
			Report.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.AuditResidency [%s] %s"), *It->GetName(), *Line);
			}
		}
		if (Managers == 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Stream.AuditResidency: no UVoxelChunkManager found"));
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
		// In range by the strategy's horizon (horizontal in heightmap worlds), not 3D: a viewer high above the terrain
		// would otherwise evict the ground it just requested.
		const float HorizonDistance = LODStrategy ? LODStrategy->GetHorizonDistance(ChunkCenter, ViewerPosition) : FMath::Sqrt(DistSq);

		if (FMath::Square(HorizonDistance) > EvictDistanceSq)
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
	FVoxelMeshingRequest& OutMeshRequest,
	TSharedPtr<const TArray<FVoxelData>>& OutSharedVoxels,
	TMap<FIntVector, TSharedPtr<const TArray<FVoxelData>>>& OutNeighborSnapshots)
{
	if (!bIsInitialized || !Configuration || !Mesher)
	{
		return false;
	}

	// Get chunk state (non-const: the snapshot path updates the cache entry's last-used tick)
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || State->State == EChunkState::Unloaded)
	{
		return false;
	}

	// Verify we have voxel data
	const int32 ChunkSize = Configuration->ChunkSize;
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
	OutMeshRequest.LocalUp = GetChunkLocalUp(ChunkCoord);

	// Hand out the shared edit-merged snapshot (built at most once per chunk content version, usually
	// already cached by the seam pipeline). The worker copies it into the request off the game thread.
	OutSharedVoxels = GetSeamVoxelSnapshot(ChunkCoord, *State);
	if (!OutSharedVoxels.IsValid() || OutSharedVoxels->Num() == 0)
	{
		return false;
	}

	// Shared snapshots for the 26 neighbours that have data (map lookups; builds only on cache miss,
	// which also seeds the seam pipeline's cache). The worker runs the slice extraction from these off
	// the game thread: collision is cooked whole-chunk from the main mesher, boundary faces included.
	OutNeighborSnapshots.Reserve(26);
	for (int32 DZ = -1; DZ <= 1; ++DZ)
	for (int32 DY = -1; DY <= 1; ++DY)
	for (int32 DX = -1; DX <= 1; ++DX)
	{
		if (DX == 0 && DY == 0 && DZ == 0)
		{
			continue;
		}
		const FIntVector NCoord = ChunkCoord + FIntVector(DX, DY, DZ);
		if (FVoxelChunkState* NState = ChunkStates.Find(NCoord))
		{
			if (NState->Descriptor.HasVoxelDataAvailable())
			{
				TSharedPtr<const TArray<FVoxelData>> Snap = GetSeamVoxelSnapshot(NCoord, *NState);
				if (Snap.IsValid() && Snap->Num() > 0)
				{
					OutNeighborSnapshots.Add(NCoord, MoveTemp(Snap));
				}
			}
		}
	}

	// For collision, we don't need transition cells
	OutMeshRequest.TransitionFaces = 0;
	for (int32 i = 0; i < 6; ++i)
	{
		OutMeshRequest.NeighborLODLevels[i] = LODLevel;
	}

	return true;
}
