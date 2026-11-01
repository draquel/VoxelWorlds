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
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "IVoxelWorldMode.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelBlockSurface.h"
#include "VoxelNoiseTypes.h"
#include "VoxelCPUCubicMesher.h"
#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelCPUDualContourMesher.h"
#include "VoxelResidencyStats.h"
#include "Async/ParallelFor.h"
#include "VoxelNeighborSliceExtraction.h"
#include "VoxelCPUNoiseGenerator.h"
#include "RenderGraphResources.h" // FRDGPooledBuffer: the GPU generator and meshers hold TRefCountPtrs to it
#include "VoxelGPUNoiseGenerator.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelWaterPropagation.h"
#include "VoxelSeaSurface.h"
#include "VoxelWaterMesher.h"
#include "VoxelCollisionManager.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelTreeInjector.h"
#include "VoxelTreeTypes.h"
#include "VoxelWorldBoundary.h"
#include "VoxelFarOcean.h"
#include "VoxelPlanetFarField.h"
#include "VoxelPlanetOcean.h"
#include "Engine/GameViewportClient.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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

// A planet's far field (spherical Phase 6, WM-BP9): on or off, for every live chunk manager (A/B against the ocean shell
// alone, which is what a planet drew beyond its voxel window before P6.4).
static TAutoConsoleVariable<int32> CVarFarField(
	TEXT("voxel.World.FarField"),
	1,
	TEXT("The far field: the analytic surface drawn beyond the voxel window to the horizon (FVoxelPlanetFarField). On a planet six\n")
	TEXT("cube-face quadtrees; on a heightmap world (world modes review WM-MP1) a quadtree of root tiles over the XY plane out to\n")
	TEXT("voxel.World.FarFieldDistance, with the sea at WaterLevel (an island's far ocean draws its open sea and seabed past the box).\n")
	TEXT("  1 = drawn (default); at scale a planet's patches also draw the sea in place of the ocean shell\n")
	TEXT("  0 = off, a development switch (code review CR1, D-CR13): nothing is drawn past the voxel window but a small planet's\n")
	TEXT("  ocean shell or an island's open sea and seabed"),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Var)
	{
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			if (It->GetWorld() && It->GetWorld()->IsGameWorld())
			{
				It->SetFarFieldEnabled(Var->GetInt() != 0);
			}
		}
	}),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarFarFieldError(
	TEXT("voxel.World.FarFieldError"),
	2.0f,
	TEXT("The far field's screen-space error budget (pixels): a patch splits while its geometric error projects to more\n")
	TEXT("than this from the camera (default 2)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarFarFieldHeightmapError(
	TEXT("voxel.World.FarFieldHeightmapError"),
	4.0f,
	TEXT("Heightmap worlds (world modes review WM-MP1): the far field's screen-space error budget (pixels) in place of\n")
	TEXT("voxel.World.FarFieldError (default 4; a plane has no horizon, so at 2 px VoxelDemo's far field held about 1,500\n")
	TEXT("patches and 300 MB from the ground, a planet's a tenth of that). <= 0 = voxel.World.FarFieldError."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarFieldQuads(
	TEXT("voxel.World.FarFieldQuads"),
	16,
	TEXT("Quads along a far-field patch's edge, a power of two from 4 to 64 (default 16). Read when the far field is built\n")
	TEXT("(voxel.World.FarField 0 then 1 rebuilds it)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarFieldMaxBuilds(
	TEXT("voxel.World.FarFieldMaxBuilds"),
	6,
	TEXT("Far-field patch builds in flight on the thread pool at once (default 6)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarFieldRootChunks(
	TEXT("voxel.World.FarFieldRootChunks"),
	0,
	TEXT("Heightmap worlds (world modes review WM-MP1, D36): chunks along a far-field root tile's edge, a power of two from 4 to\n")
	TEXT("4096. 0 (default) = about a quarter of the far distance (or an island's half extent) in chunks, rounded to a power of two\n")
	TEXT("(64 with 6,400 uu chunks and the 2,000,000 uu default: 409,600 uu tiles, a few rows of roots about the camera). Band\n")
	TEXT("patches are one chunk column. Read when the far field is built (voxel.World.FarField 0 then 1 rebuilds it)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarFarFieldDistance(
	TEXT("voxel.World.FarFieldDistance"),
	2000000.0f,
	TEXT("Heightmap worlds (world modes review WM-MP1): how far from the camera the far field draws (uu; default 2,000,000, the\n")
	TEXT("island far ocean's reach). Root tiles exist within it and patches past it are culled; a bounded world (an island) also\n")
	TEXT("stops at its bounds. 0 = no limit on a bounded world, the tile under the camera alone on a plane. Read when the far\n")
	TEXT("field is built."),
	ECVF_Default);

// Visual polish VP4: the terrain flicker on planets. A far-field patch that needed a build while nothing showed of it (a
// band patch the voxel chunks covered, past the prebuild radius or with a chunk under it re-meshing an empty mesh; a patch
// coming up over the horizon) was stood in for by its nearest built ancestor, often a cube face's root, whose coarse
// surface replaced the horizon and rose through the voxel terrain for the frame or more the build took.
static TAutoConsoleVariable<int32> CVarFarFieldAvoidStandIns(
	TEXT("voxel.World.FarFieldAvoidStandIns"),
	1,
	TEXT("Planets (visual polish VP4): no far-field stand-in where nothing would show.\n")
	TEXT("  1 = a band patch whose chunks are all drawn draws nothing, built or not (one kept built for the window's edge builds\n")
	TEXT("      meanwhile); a chunk re-meshing counts as drawn as it was; a patch coming up over the horizon waits for its build\n")
	TEXT("      drawing nothing. No coarse ancestor is drawn over the voxel terrain or the horizon for them (default)\n")
	TEXT("  0 = as before: each needed a build, and its nearest built ancestor (often a cube face's root) flashed in its place,\n")
	TEXT("      over everything under it, until the build landed"),
	ECVF_Default);

// Visual polish F4.2: a cube face's far-field root was built only once some of the face came into view, so after a teleport
// onto a face the camera had never seen nothing of the far field was there to stand in, and the first update drew none.
static TAutoConsoleVariable<int32> CVarFarFieldBuildAllRoots(
	TEXT("voxel.World.FarFieldBuildAllRoots"),
	1,
	TEXT("Planets (visual polish F4.2): build all six cube faces' far-field roots, also faces under the horizon.\n")
	TEXT("  1 = every root is built in the first updates and kept (a patch each), so after a teleport onto a face the camera\n")
	TEXT("      never saw its root stands in from the first update (default)\n")
	TEXT("  0 = as before: a root is built once some of its face comes into view; a teleport onto a face never seen drew no\n")
	TEXT("      far field there until builds landed"),
	ECVF_Default);

// Visual polish F4.3: the tick (TG_PrePhysics) reads the player camera manager's camera before the camera managers
// update (after TG_PostPhysics), so the far field lagged the rendered camera a frame: the first frame after a teleport
// was drawn with the selection for the old position.
/**
 * Counts a worker job that holds a raw pointer to one of the chunk manager's meshers (a chunk mesh, a seam job) from its
 * dispatch to its end, so Shutdown can wait for them before the meshers go (world modes review G76: a seam job finishing
 * after Shutdown called into a freed mesher). Copies count too: a lambda moved into its task keeps one.
 */
struct FMesherWorkerGuard
{
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Count;
	explicit FMesherWorkerGuard(const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe>& InCount) : Count(InCount) { ++(*Count); }
	FMesherWorkerGuard(const FMesherWorkerGuard& Other) : Count(Other.Count) { ++(*Count); }
	FMesherWorkerGuard& operator=(const FMesherWorkerGuard&) = delete;
	~FMesherWorkerGuard() { --(*Count); }
};

static TAutoConsoleVariable<int32> CVarFarFieldCameraUpdate(
	TEXT("voxel.World.FarFieldCameraUpdate"),
	1,
	TEXT("Planets (visual polish F4.3): after a cut, update the far field again for the camera the frame renders with.\n")
	TEXT("  1 = in TG_PostUpdateWork, after the camera managers, when the camera lies more than a chunk from the camera of\n")
	TEXT("      the tick's update (a teleport), the far field updates for it: the first frame after a teleport draws the\n")
	TEXT("      far field there (default)\n")
	TEXT("  0 = as before: the tick's update only, for the frame before's camera; the first frame after a teleport drew the\n")
	TEXT("      far field for the old position (nothing where the camera arrived)"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarStreamCameraCutUpdate(
	TEXT("voxel.Stream.CameraCutUpdate"),
	1,
	TEXT("Every world mode (world modes review WM-MP4): after a cut, run the streaming decisions again for the camera the\n")
	TEXT("frame renders with.\n")
	TEXT("  1 = in TG_PostUpdateWork, after the camera managers, when the camera lies more than a chunk from the camera of\n")
	TEXT("      the tick's update (a teleport, a respawn, a switch of view target), the load and unload decisions, the queues'\n")
	TEXT("      order and the first generation launches run again for it: the chunks under the new position are requested\n")
	TEXT("      in the frame of the cut (default)\n")
	TEXT("  0 = as before: the tick's decisions only, for the frame before's camera; the frame of a teleport streamed for the\n")
	TEXT("      old position and the next frame for the new one"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarFieldTrace(
	TEXT("voxel.World.FarFieldTrace"),
	0,
	TEXT("Planets (visual polish VP4): log every far-field update with a stand-in over the voxel window (a coarser patch drawn\n")
	TEXT("because a patch under it was not ready, over band patches the chunks cover): the frame, the stand-ins and the coarsest's\n")
	TEXT("level, the band patches not ready (covered by drawn chunks or not) and a chunk not drawn under one, with its state.\n")
	TEXT("0 = off (default)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarStreamDescentLead(
	TEXT("voxel.Stream.DescentLead"),
	3.0f,
	TEXT("Seconds ahead of a descending camera the streaming viewer goes along local down (the descent prefetch, spherical\n")
	TEXT("Phase 6 WM-BP10; every world mode since world modes review WM-MP3): the voxel window resumes from the cut-over and\n")
	TEXT("refines its LOD before the camera gets there. 0 = off (the streaming viewer is the camera)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarStreamDescentMinSpeed(
	TEXT("voxel.Stream.DescentMinSpeed"),
	1500.0f,
	TEXT("The descent prefetch leads only a camera descending faster than this (uu/s), not a jump or a walk downhill."),
	ECVF_Default);

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

static FAutoConsoleCommandWithWorldAndArgs GVoxelCollisionAuditNeighborsCmd(
	TEXT("voxel.Collision.AuditNeighbors"),
	TEXT("G32: log the collision cooks that read a neighbour as air (it had no voxel data then): how many cooked chunks, how many "
	     "were cooked with a neighbour missing, how many of those have the neighbour's data now with no re-cook pending (stale), "
	     "how many re-cooks are pending, and the neighbour re-cooks so far. Optional arg: max stale chunks to list (default 8)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*)
	{
		const int32 MaxList = Args.Num() > 0 ? FMath::Max(0, FCString::Atoi(*Args[0])) : 8;
		for (TObjectIterator<UVoxelChunkManager> It; It; ++It)
		{
			UVoxelCollisionManager* Collision = It->GetCollisionManager();
			if (!It->GetWorld() || !It->GetWorld()->IsGameWorld() || !Collision || !Collision->IsInitialized())
			{
				continue;
			}
			int32 Cooked = 0, WithMissing = 0, Stale = 0, Awaiting = 0;
			TArray<FIntVector> StaleChunks;
			Collision->AuditNeighborCooks(Cooked, WithMissing, Stale, Awaiting, &StaleChunks);
			FString List;
			for (int32 Index = 0; Index < FMath::Min(MaxList, StaleChunks.Num()); ++Index)
			{
				List += FString::Printf(TEXT(" (%d,%d,%d)"), StaleChunks[Index].X, StaleChunks[Index].Y, StaleChunks[Index].Z);
			}
			UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.Collision.AuditNeighbors: %d cooked, %d cooked with a neighbour missing, %d stale (the neighbour has data now, no re-cook pending), %d re-cooks pending, %lld neighbour re-cooks so far, arrivals needing none: %lld air and %lld solid on both sides (voxel.Collision.NeighborRecook %d)%s%s"),
				Cooked, WithMissing, Stale, Awaiting, Collision->GetTotalNeighborRecooks(), Collision->GetTotalNeighborArrivalsSkipped(),
				Collision->GetTotalNeighborArrivalsSkippedSolid(),
				IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Collision.NeighborRecook"))
					? IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Collision.NeighborRecook"))->GetInt() : -1,
				List.IsEmpty() ? TEXT("") : TEXT("; stale:"), *List);
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
	TEXT("Auto-raise the per-frame load budget with viewer speed (horizontal; 3D on a planet) so the load front leads "
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
	TEXT("Hold a new chunk that fades in (beyond voxel.MeshFade.AppearMinDistance) until its ready seams land, then fade them in together "
	     "(capped by voxel.MeshFade.SeamHoldMs). 1 = on (default), 0 = the interior fades in at once and its seams follow."),
	ECVF_Default);

// World modes review G33: a system or editor edit re-extracts the scatter of the chunks whose extraction reads the voxels it
// changed (UVoxelScatterManager::GetEditReaders: the chunk below on a heightmap, the seven chunks at -X / -Y / -Z on a
// planet, those the edit's bounds reach), once each per tick. The rule before regenerated the edited chunk's six face
// neighbours, which missed a planet's edge and corner readers and re-extracted the rest for nothing.
static TAutoConsoleVariable<int32> CVarScatterEditReaders(
	TEXT("voxel.Scatter.EditReaders"),
	1,
	TEXT("System / editor edits re-extract the scatter of the chunks whose extraction reads the edited voxels (world modes review "
	     "G33). 1 = on (default), 0 = the rule before: the edited chunk's six face neighbours, for A/B."),
	ECVF_Default);

// World modes review G56: an edit re-meshes the chunks it changed, and their seams restale (MarkChunkDirty). The rule before
// also re-meshed each edited chunk's six face neighbours ("so they re-extract boundary data"), work for nothing since seam
// ownership: a chunk mesh reads only its own voxels, and a seam job only its participants', which the edited chunk's seams
// include. voxel.Scatter.EditReaders 0 keeps the re-mesh: that rule's face neighbours re-extract their scatter through it.
static TAutoConsoleVariable<int32> CVarEditNeighbourRemesh(
	TEXT("voxel.Edit.NeighbourRemesh"),
	0,
	TEXT("Re-mesh an edited chunk's six face neighbours too (world modes review G56). 0 = off (default): an edit re-meshes the ")
	TEXT("chunks it changed and restales their seams; 1 = the rule before, for A/B."),
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
	     "in PIE), the encodes handed to workers. "
	     "Free re-collapses/re-compresses and skips of already-evaluated chunks are not counted against this."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionMaxInFlight(
	TEXT("voxel.FarCompression.MaxInFlight"),
	8,
	TEXT("Async encodes in flight at most. Each holds a copy of its chunk's array (1MB for a 64^3 chunk), "
	     "so this also caps the copies' memory."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionUniformAtGeneration(
	TEXT("voxel.FarCompression.UniformAtGeneration"),
	1,
	TEXT("World modes review G67: 1 = a chunk whose every voxel is the same (all air, all rock, open sea) is collapsed to that "
	     "value as it is generated (found by the generation worker, or as a GPU readback lands); 0 = it stays resident until the "
	     "far-compression sweep collapses it (IdleFrames after it went idle), as before."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarBenchCountPreBenchMeshes(
	TEXT("voxel.Bench.CountPreBenchMeshes"),
	1,
	TEXT("The streaming bench's re-mesh count (world modes review G14). 1 = a re-mesh counts for every chunk that had a mesh "
	     "when its job launched, also one meshed before the bench started. 0 = only chunks meshed since the bench started "
	     "count (the rule before): a bench started after a cold first PIE, which had meshed fewer chunks by then, counted "
	     "about twice the LOD re-meshes of a later one."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionSnapshotReads(
	TEXT("voxel.FarCompression.SnapshotReads"),
	1,
	TEXT("Far-compressed chunk data off the game thread's reads (world modes review G65). 1 = the snapshot of a "
	     "far-compressed chunk without edits is decoded by a worker task, started when a chunk in its neighbourhood is "
	     "queued for a mesh or a collision cook; mesh jobs, seam jobs and cooks wait for it on their own workers, and the "
	     "water pull, water tile and scatter hand-off read it. The chunks stay compressed. 0 = every read expands the chunk "
	     "in place on the game thread, as before (for A/B)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarWaterPullAgain(
	TEXT("voxel.Water.PullAgain"),
	1,
	TEXT("The water pull across chunk faces (world modes review G70): 1 = when a chunk's water rises after a neighbour pulled "
	     "(its own pull floods voxels on the face they share, or its generated water arrives), that neighbour pulls again, "
	     "so the flood reaches the same voxels whatever order the chunks finish meshing in; 0 = each chunk pulls once when "
	     "its mesh lands, as before (a flood that reached it only through a neighbour that flooded later never came)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarCollisionNeighborShells(
	TEXT("voxel.Collision.NeighborShells"),
	1,
	TEXT("Collision cooks (world modes review G69): 1 = a cook reads each of its 26 neighbours from a cached boundary shell "
	     "(the voxels within the cook's neighbour depth of each face: one plane at collision LOD 0), a far-compressed one "
	     "decoded on a worker and only its shell kept; 0 = from the neighbour's whole cached snapshot, as before."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSnapshotShareUniform(
	TEXT("voxel.Snapshot.ShareUniform"),
	1,
	TEXT("Seam snapshot cache (world modes review G69): 1 = the snapshot of a uniform chunk (all one voxel: air, rock, sea) "
	     "shares one array per value; 0 = each is expanded into its own array on a worker, as before."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSnapshotShareResident(
	TEXT("voxel.Snapshot.ShareResident"),
	1,
	TEXT("Resident chunks (world modes review G71): 1 = the snapshot of a resident chunk without edits (seam jobs, collision "
	     "cooks, the water pull) and the far-compression encode's input share the chunk's own voxel array, copy-on-write "
	     "(a write to the chunk while one holds it clones the array first); 0 = each takes a copy, as before."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarSnapshotMaxAgeTicks(
	TEXT("voxel.Snapshot.MaxAgeTicks"),
	128,
	TEXT("Seam snapshot cache: an entry not read for this many seam ticks (one a frame) is evicted by the sweep, which runs "
	     "every 64 ticks (world modes review G69)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarFarCompressionCodec(
	TEXT("voxel.FarCompression.Codec"),
	3, // Oodle — bake-off winner (best ratio + fastest encode/decode; de-interleave hurts on
	   // run-heavy far chunks because the thin gradient band leaves long AoS tuple runs Oodle handles well)
	TEXT("Encode codec for non-uniform far chunks (EVoxelChunkCodec): 0=uniform-only (no general "
	     "codec), 2=LZ4, 3=Oodle, 4=LZ4Planar, 5=OodlePlanar. Uniform collapse always applies. "
	     "Buffers written by any codec still decode (codec id is stored in the buffer header)."),
	ECVF_Default);

/** A mesh job's own far-compressed chunk: its snapshot, decoded on a worker, copied into the request on the job's worker (G65). */
static void TakeDeferredMeshVoxels(TSharedPtr<const TArray<FVoxelData>>& DeferredVoxels, UE::Tasks::FTask& DeferredDecode,
	TArray<FVoxelData>& OutVoxels)
{
	if (!DeferredVoxels.IsValid())
	{
		return;
	}
	if (DeferredDecode.IsValid())
	{
		DeferredDecode.Wait();
	}
	OutVoxels = *DeferredVoxels;
	DeferredVoxels.Reset();
}

/** A seam job or cook waits on its worker for the snapshot decodes it reads (G65). */
static void WaitForSnapshotDecodes(TArray<UE::Tasks::FTask>& Decodes)
{
	for (UE::Tasks::FTask& Decode : Decodes)
	{
		Decode.Wait();
	}
	Decodes.Reset();
}

UVoxelChunkManager::UVoxelChunkManager()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	// The far field again for the frame's camera after a cut (F4.3): after the camera managers update.
	FarFieldCameraTick.bCanEverTick = true;
	FarFieldCameraTick.bStartWithTickEnabled = true;
	FarFieldCameraTick.TickGroup = TG_PostUpdateWork;
	FarFieldCameraTick.Target = this;

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
	// per-frame budget with the viewer's speed (~+1 chunk/frame per 128 uu/s; see ViewerLoadSpeed). Idle stays at
	// ConfigBase. Realistic fast movement is covered; extreme speeds still need forward-biased generation.
	if (CVarStreamSpeedAdaptive.GetValueOnGameThread() != 0)
	{
		const int32 SpeedBudget = ConfigBase + FMath::CeilToInt(ViewerLoadSpeed / 128.0f);
		Budget = FMath::Max(Budget, SpeedBudget);
	}

	return FMath::Clamp(Budget, 1, 64);
}

void UVoxelChunkManager::BeginPlay()
{
	Super::BeginPlay();
}

void UVoxelChunkManager::RegisterComponentTickFunctions(bool bRegister)
{
	Super::RegisterComponentTickFunctions(bRegister);
	if (bRegister)
	{
		if (SetupActorComponentTickFunction(&FarFieldCameraTick))
		{
			FarFieldCameraTick.Target = this;
			FarFieldCameraTick.AddPrerequisite(this, PrimaryComponentTick);
		}
	}
	else if (FarFieldCameraTick.IsTickFunctionRegistered())
	{
		FarFieldCameraTick.UnRegisterTickFunction();
	}
}

void FVoxelFarFieldCameraTickFunction::ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	FActorComponentTickFunction::ExecuteTickHelper(Target, /*bTickInEditor=*/ false, DeltaTime, TickType, [this](float DilatedTime)
	{
		Target->TickAfterCameraUpdate();
	});
}

FString FVoxelFarFieldCameraTickFunction::DiagnosticMessage()
{
	return GetFullNameSafe(Target) + TEXT("[UVoxelChunkManager::TickAfterCameraUpdate]");
}

FName FVoxelFarFieldCameraTickFunction::DiagnosticContext(bool bDetailed)
{
	if (bDetailed)
	{
		return FName(*FString::Printf(TEXT("VoxelFarFieldCameraTick/%s"), *GetFullNameSafe(Target)));
	}
	return FName(TEXT("VoxelFarFieldCameraTick"));
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

	// The camera; the streaming viewer goes ahead of it under a descending camera (spherical Phase 6, WM-BP10: the descent
	// prefetch; every world mode since WM-MP3), so the window resumes from the cut-over and refines before the camera arrives.
	CurrentCameraPosition = Context.ViewerPosition;
	ApplyDescentPrefetch(Context, DeltaTime);
	CutoverClock += DeltaTime;

	// Cache the current viewer position for the stale-cull distance test used by the queue
	// processors (which don't build their own context). Reflects the benchmark view override.
	CurrentViewerPosition = Context.ViewerPosition;
	// R24 P3: the renderer fades new geometry in only away from the viewer: the camera, what the player sees from.
	if (MeshRenderer)
	{
		MeshRenderer->SetViewerPosition(CurrentCameraPosition);
	}

	// Track the viewer's speed for the speed-adaptive load budget (ResolveMaxLoadPerFrame): horizontal in heightmap
	// worlds (their window is a column per XY), 3D on a planet (its window is a shell around the viewer, so a climb at a
	// pole or a flight north streams as much as a walk east; spherical Phase 6, WM-BP2). Smoothed so a teleport spike
	// doesn't blow the budget out for a frame.
	if (DeltaTime > SMALL_NUMBER && LastViewerPosForSpeed.X != FLT_MAX)
	{
		const FVector ViewerDelta = CurrentViewerPosition - LastViewerPosForSpeed;
		const bool bPlanet = Configuration && Configuration->WorldMode == EWorldMode::SphericalPlanet;
		const float InstSpeed = static_cast<float>((bPlanet ? ViewerDelta.Size() : FVector2D(ViewerDelta.X, ViewerDelta.Y).Size()) / DeltaTime);
		ViewerLoadSpeed = FMath::FInterpTo(ViewerLoadSpeed, InstSpeed, DeltaTime, 4.0f);
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

	// The voxel cut-over (spherical Phase 6, WM-BP10): when the window suspends, the queues let go of everything they held
	// for it (nothing is wanted up there) and every chunk unloads; when it resumes, the next streaming update asks for the
	// window again.
	const bool bWindowSuspended = IsVoxelWindowSuspended();
	if (bWindowSuspended != bWasWindowSuspended)
	{
		bWasWindowSuspended = bWindowSuspended;
		bForceStreamingUpdate = true;
		FVoxelCutoverTransition Transition;
		Transition.Time = CutoverClock;
		Transition.bSuspended = bWindowSuspended;
		Transition.ViewerOverSurface = LODStrategy ? LODStrategy->GetViewerSurfaceAltitude() : 0.0;
		Transition.Lead = StreamingViewerLead;
		Transition.CameraOverSurface = Transition.ViewerOverSurface + StreamingViewerLead;
		if (bWindowSuspended)
		{
			Transition.ChunksHeld = LoadedChunkCoords.Num();
			Transition.GenerationsDropped = ReprioritizeQueues(Context);
			Transition.ActiveSeconds = LastWindowResumeTime >= 0.0 ? Transition.Time - LastWindowResumeTime : -1.0;
			Transition.GeneratedSinceResume = TotalChunksGenerated - GeneratedAtWindowResume;
			Transition.MeshedSinceResume = TotalChunksMeshed - MeshedAtWindowResume;
		}
		else
		{
			LastWindowResumeTime = Transition.Time;
			GeneratedAtWindowResume = TotalChunksGenerated;
			MeshedAtWindowResume = TotalChunksMeshed;
		}
		if (CutoverTransitions.Num() >= MaxCutoverTransitions)
		{
			CutoverTransitions.RemoveAt(0);
		}
		CutoverTransitions.Add(Transition);
		++CutoverTransitionCount;
		if (bWindowSuspended)
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("Voxel window suspended above the cut-over (unloading; the far field draws the world): camera %.0f over the surface, streaming viewer %.0f (lead %.0f); %d chunks held, %d queued generations dropped; %s, %lld chunks generated and %lld meshed in it."),
				Transition.CameraOverSurface, Transition.ViewerOverSurface, Transition.Lead, Transition.ChunksHeld, Transition.GenerationsDropped,
				Transition.ActiveSeconds >= 0.0 ? *FString::Printf(TEXT("active %.2f s since it resumed"), Transition.ActiveSeconds) : TEXT("active since the world started"),
				Transition.GeneratedSinceResume, Transition.MeshedSinceResume);
		}
		else
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("Voxel window resumed (streaming back): camera %.0f over the surface, streaming viewer %.0f (lead %.0f), %d chunks held."),
				Transition.CameraOverSurface, Transition.ViewerOverSurface, Transition.Lead, LoadedChunkCoords.Num());
		}
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
	// The open sea's underside shows only while the viewer is under the water plane.
	if (FarOceanStatic)
	{
		UpdateFarOceanUnderside(CurrentViewerPosition);
	}
// A planet's sea surface shows its underside only while the viewer is in the sea (SP-E2).
	UpdatePlanetOceanUnderside(CurrentViewerPosition);

	// === Planet far field (spherical Phase 6, WM-BP9) ===
	// After this tick's generation results, meshes and unloads: a band patch shows in the tick its chunks leave and
	// hides once they have been drawn a moment. Timed into FarFieldStats.UpdateMs (the bench's FarFieldMs).
	if (FarField)
	{
		UpdateFarField(CurrentCameraPosition, CurrentViewerPosition, Context.FieldOfView);
	}

	// === Water propagation (bounded BFS per frame) ===
	if (WaterPropagation && WaterPropagation->HasPendingWork())
	{
		WaterPropagation->ProcessPropagation(512);
	}

	// === The water pull's second passes (G70), within about a millisecond ===
	if (WaterRepullQueue.Num() > 0)
	{
		ProcessWaterRepulls();
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
		ProcessEditScatterReaders();
		Timing.ScatterEditMs = static_cast<float>((FPlatformTime::Seconds() - SectionStart) * 1000.0);
		ScatterManager->Update(Context.ViewerPosition, DeltaTime);
		const UVoxelScatterManager::FUpdateTimings& ScatterSteps = ScatterManager->GetLastUpdateTimings();
		Timing.ScatterApplyMs = ScatterSteps.ApplyMs;
		Timing.ScatterLaunchMs = ScatterSteps.RegrowMs + ScatterSteps.LaunchMs;
		Timing.ScatterDistanceMs = ScatterSteps.DistanceMs;
		Timing.ScatterFlushMs = ScatterSteps.FlushMs;
		Timing.ScatterApplied = ScatterSteps.Applied;
		Timing.ScatterLaunched = ScatterSteps.Launched;

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
	// The voxel cut-over and the descent prefetch (spherical Phase 6, WM-BP10) start over with the world.
	bWasWindowSuspended = false;
	StreamingViewerLead = 0.0;
	CameraRadialSpeed = 0.0;
	LastPrefetchCamera = FVector(FLT_MAX);
	CutoverTransitions.Reset();
	CutoverTransitionCount = 0;
	CutoverClock = 0.0;
	LastWindowResumeTime = -1.0;
	GeneratedAtWindowResume = 0;
	MeshedAtWindowResume = 0;

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

// Pass water material to renderer for per-chunk water mesh sections
	if (MeshRenderer && UsesWaterTiles())
	{
		MeshRenderer->SetWaterMaterial(Configuration->WaterMeshMaterial);
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
	++ChunkStateVersion;
	BenchEverMeshed.Empty();
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
	MeshWorkStats = FMeshWorkStats();
	CurrentFrame = 0;

	// Reset streaming decision caching (sentinel values force update on first tick)
	CachedViewerChunk = FIntVector(INT32_MAX, INT32_MAX, INT32_MAX);
	LastStreamingUpdatePosition = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	bForceStreamingUpdate = false;
	LODEvalSchedule.Reset();

	// Reset adaptive throttle state
	SmoothedFrameTimeMs = 16.67f;
	ObservedFrameFloorMs = 16.67f;
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
	if (bForceCPU)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("VoxelForceCPU: forcing CPU generation"));
	}

	// Create mesher based on configuration
	if (Configuration->MeshingMode == EMeshingMode::MarchingCubes)
	{
		FVoxelMeshingConfig MeshConfig;
		MeshConfig.IsoLevel = 0.5f;
		MeshConfig.bCalculateAO = Configuration->bCalculateAO;
		MeshConfig.UVScale = Configuration->UVScale;

		// Disable LOD seam handling if configured
		if (!Configuration->bEnableLODSeams)
		{
			MeshConfig.bUseTransvoxel = false;
		}

		auto CPUMCMesher = MakeUnique<FVoxelCPUMarchingCubesMesher>();
		CPUMCMesher->Initialize();
		CPUMCMesher->SetConfig(MeshConfig);
		Mesher = MoveTemp(CPUMCMesher);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using CPU MarchingCubes mesher (AO=%s, UVScale=%.2f)"),
			Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
			Configuration->UVScale);
	}
	else if (Configuration->MeshingMode == EMeshingMode::DualContouring)
	{
		FVoxelMeshingConfig MeshConfig;
		MeshConfig.IsoLevel = 0.5f;
		MeshConfig.bCalculateAO = Configuration->bCalculateAO;
		MeshConfig.UVScale = Configuration->UVScale;

		// DC uses its own LOD boundary merging, not Transvoxel transition cells
		MeshConfig.bUseTransvoxel = false;

		auto DCMesher = MakeUnique<FVoxelCPUDualContourMesher>();
		DCMesher->Initialize();
		DCMesher->SetConfig(MeshConfig);
		Mesher = MoveTemp(DCMesher);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Using CPU Dual Contouring mesher (AO=%s, UVScale=%.2f)"),
			Configuration->bCalculateAO ? TEXT("true") : TEXT("false"),
			Configuration->UVScale);
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

		// Mark the edited chunk dirty: it re-meshes and its seams restale (on its way to a mesh as well, G57).
		++MeshWorkStats.EditBroadcasts;
		const EChunkState EditedChunkState = GetChunkState(ChunkCoord);
		MeshWorkStats.EditedRemeshes += EditedChunkState == EChunkState::Loaded ? 1 : 0;
		MeshWorkStats.EditedOnTheWay += (EditedChunkState == EChunkState::PendingMeshing || EditedChunkState == EChunkState::Meshing) ? 1 : 0;
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
				// re-extraction smoothly replaces them per type, so it adds no flash. The chunk's re-mesh
				// (MarkChunkDirty above) hands its edit-merged voxels to scatter.
				//
				// TODO(scatter-claims): a POI often wants foliage SUPPRESSED — either across its whole
				// claimed zone or just under a static-asset (building) footprint — rather than re-extracted.
				// Drive that from the claim covering the region once VoxelScatter consumes claims.
				ScatterManager->RegenerateChunkScatter(ChunkCoord);
				++EditScatterStats.Broadcasts;
				if (CVarScatterEditReaders.GetValueOnGameThread() != 0)
				{
					// The chunks whose extraction reads the voxels the edit changed here (G33): the chunk below on a
					// heightmap, the -X / -Y / -Z chunks on a planet, as far as the edit's bounds reach. Re-extracted once
					// each in the next tick (ProcessEditScatterReaders), so an edit across many chunks folds them.
					FIntVector EditMin, EditMax;
					const bool bBounds = EditManager->GetBroadcastEditBounds(EditMin, EditMax);
					TArray<FIntVector> Readers;
					ScatterManager->GetEditReaders(ChunkCoord, bBounds ? &EditMin : nullptr, Readers);
					EditScatterReaderChunks.Append(Readers);
					EditScatterEditedChunks.Add(ChunkCoord);
				}
			}
			// A zero-radius player edit has no sphere to clear. Undo broadcasts System (the restored voxels regenerate),
			// and redo the operation's own source, centre and radius (G60).
		}

		// Notify water propagation system of the edit
		if (WaterPropagation)
		{
			WaterPropagation->OnChunkEdited(ChunkCoord, Source, EditCenter, EditRadius);
		}

		// The face neighbours re-cook their collision: a cook reads its 26 neighbours' voxels (G32). Their meshes read none of
		// the edited chunk's voxels (a chunk mesh is a function of its own; the boundary is in the edited chunk's seams,
		// restaled above), so they re-mesh only under the rule before (voxel.Edit.NeighbourRemesh 1, G56), or for a system edit
		// under voxel.Scatter.EditReaders 0, whose face neighbours re-extract their scatter through that re-mesh.
		static const FIntVector NeighborOffsets[6] = {
			FIntVector(-1, 0, 0), FIntVector(1, 0, 0),
			FIntVector(0, -1, 0), FIntVector(0, 1, 0),
			FIntVector(0, 0, -1), FIntVector(0, 0, 1)
		};
		const bool bNeighbourRemesh = CVarEditNeighbourRemesh.GetValueOnGameThread() != 0
			|| (Source != EEditSource::Player && CVarScatterEditReaders.GetValueOnGameThread() == 0);

		for (const FIntVector& Offset : NeighborOffsets)
		{
			const FIntVector NeighborCoord = ChunkCoord + Offset;
			if (ChunkStates.Contains(NeighborCoord))
			{
				if (bNeighbourRemesh)
				{
					MeshWorkStats.NeighbourRemeshes += GetChunkState(NeighborCoord) == EChunkState::Loaded ? 1 : 0;
					MarkChunkDirty(NeighborCoord);
				}
				if (CollisionManager)
				{
					CollisionManager->MarkChunkDirty(NeighborCoord);
				}
				// Player edits: scatter for neighbours is handled by ClearScatterInRadius, which affects every chunk
				// within the edit radius. System / Editor edits: the chunks whose extraction reads the edited voxels
				// re-extract (G33, above); voxel.Scatter.EditReaders 0 restores the rule before, these six.
				if (ScatterManager && Source != EEditSource::Player && CVarScatterEditReaders.GetValueOnGameThread() == 0)
				{
					ScatterManager->RegenerateChunkScatter(NeighborCoord);
					++EditScatterStats.LegacyFaceNeighbours;
				}
			}
		}

		// A collision cook reads the edge and corner neighbours' voxels too: re-cook those the edit reaches (G32).
		if (CollisionManager)
		{
			CollisionManager->MarkNeighborsDirtyForEdit(ChunkCoord, EditCenter, EditRadius);
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
		// The chunk's conditioning zones, the ones generation layered onto it, so scatter's analytic surface is the
		// generated ground: the buried-chunk test's height on a heightmap (its XY footprint, world modes review G23), a
		// line's crossing at a planet chunk's upper face (the cap its box subtends, SP-B10). The scatter manager is ours
		// and dies with us; it asks on the game thread.
		ScatterManager->SetConditioningGatherer([this](const FBox& ChunkBox, TArray<FVoxelConditioningZone>& OutZones)
		{
			FVector CapDirection;
			double CapAngle = 0.0;
			if (GetPlanetCapOfBox(ChunkBox, CapDirection, CapAngle))
			{
				GatherConditioningZonesOnSphere(CapDirection, CapAngle, OutZones);
			}
			else
			{
				GatherConditioningZonesForRegion(FBox2D(FVector2D(ChunkBox.Min.X, ChunkBox.Min.Y), FVector2D(ChunkBox.Max.X, ChunkBox.Max.Y)), OutZones);
			}
		});
		// The edit layers, so a heightmap chunk's extraction knows the voxel over its top layer, the chunk above's bottom
		// voxel with that chunk's edits applied (world modes review G26).
		ScatterManager->SetEditLayerSource([this](const FIntVector& ChunkCoord) -> const FChunkEditLayer*
		{
			return EditManager ? EditManager->GetEditLayer(ChunkCoord) : nullptr;
		});
		// A chunk handed off with every type out of range (it arrived at the window's front) is handed off again by the
		// scatter manager's distance pass once within the scatter horizon (world modes review G79), from its voxels as the
		// mesh submit hands them off.
		ScatterManager->SetChunkHandOffRequester([this](const FIntVector& ChunkCoord) -> bool
		{
			FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
			if (!State || !State->Descriptor.HasVoxelDataAvailable()
				|| State->State == EChunkState::PendingUnload || State->State == EChunkState::Unloaded)
			{
				return false;
			}
			HandOffChunkScatter(ChunkCoord, *State);
			return true;
		});

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
	RebuildFarField(CVarFarField.GetValueOnGameThread() != 0);
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

// ==================== Finding the manager ====================

namespace
{
	// The last manager Find returned (code review CR2): validated against the caller's world and the initialized
	// requirement on every hit, dropped by that manager's Shutdown.
	TWeakObjectPtr<UVoxelChunkManager> GLastFoundChunkManager;
}

UVoxelChunkManager* UVoxelChunkManager::Find(const UWorld* World, bool bRequireInitialized)
{
	if (!World)
	{
		return nullptr;
	}
	if (UVoxelChunkManager* Cached = GLastFoundChunkManager.Get())
	{
		if (Cached->GetWorld() == World && (!bRequireInitialized || Cached->IsInitialized()))
		{
			return Cached;
		}
	}
	GLastFoundChunkManager.Reset();
	for (TActorIterator<AActor> It(const_cast<UWorld*>(World)); It; ++It)
	{
		UVoxelChunkManager* Candidate = It->FindComponentByClass<UVoxelChunkManager>();
		if (Candidate && (!bRequireInitialized || Candidate->IsInitialized()))
		{
			GLastFoundChunkManager = Candidate;
			return Candidate;
		}
	}
	return nullptr;
}

TArray<UVoxelChunkManager*> UVoxelChunkManager::FindAll(const UWorld* World)
{
	TArray<UVoxelChunkManager*> Managers;
	if (!World)
	{
		return Managers;
	}
	for (TActorIterator<AActor> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (UVoxelChunkManager* Candidate = It->FindComponentByClass<UVoxelChunkManager>())
		{
			Managers.Add(Candidate);
		}
	}
	return Managers;
}

void UVoxelChunkManager::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}
	if (GLastFoundChunkManager.Get() == this)
	{
		GLastFoundChunkManager.Reset();
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
	++ChunkStateVersion;
	BenchEverMeshed.Empty();
	LoadedChunkCoords.Empty();
	EditScatterReaderChunks.Empty();
	EditScatterEditedChunks.Empty();
	EditScatterStats = FEditScatterStats();
	MeshWorkStats = FMeshWorkStats();
	UniformSnapshotArrays.Empty();
	RecentlyEvictedSnapshots.Empty();
	ShellCache.Empty();
	UniformShells.Empty();
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
	bForceStreamingUpdate = false;
	LODEvalSchedule.Reset();

	// Clean up LOD strategy (we own it)
	if (LODStrategy)
	{
		delete LODStrategy;
		LODStrategy = nullptr;
	}

	// Chunk meshes and seam jobs on workers hold raw pointers to the meshers (G76): wait for them before the meshers go.
	// Their results land in queues this Shutdown has emptied, or no-op through the weak manager pointer.
	{
		const double WaitStart = FPlatformTime::Seconds();
		while (MesherWorkerJobs->load() > 0 && FPlatformTime::Seconds() - WaitStart < 10.0)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		const double Waited = FPlatformTime::Seconds() - WaitStart;
		if (MesherWorkerJobs->load() > 0)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("Shutdown: %d mesh or seam jobs still on workers after %.1f s; the meshers go regardless."), MesherWorkerJobs->load(), Waited);
		}
		else if (Waited > 0.05)
		{
			UE_LOG(LogVoxelStreaming, Log, TEXT("Shutdown: waited %.0f ms for the mesh and seam jobs on workers."), Waited * 1000.0);
		}
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
	DestroyFarField();
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
	WaterRepullQueue.Reset();
	WaterRepullSet.Reset();

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
		else if (State->State == EChunkState::PendingMeshing || State->State == EChunkState::Meshing)
		{
			// On its way to a mesh (world modes review G57): its seams restale too, as a seam job reads the chunk's edit-merged
			// snapshot when it is dispatched. A queued mesh reads the edit layer when it is dispatched; a mesh on a worker was
			// built before this change, so the chunk re-meshes once it lands (OnChunkMeshingComplete).
			if (SeamRegistry.IsValid())
			{
				SeamRegistry->UpdateChunkContent(ChunkCoord, State->Descriptor.ContentVersion);
			}
			if (State->State == EChunkState::Meshing)
			{
				State->bRemeshAfterMeshing = true;
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
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("point query"));
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
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("point query"));
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
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("snapshot"));
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

bool UVoxelChunkManager::IsChunkWaterSettled(const FIntVector& ChunkCoord) const
{
	const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	return State && State->bWaterSettled && State->Descriptor.HasVoxelDataAvailable();
}

bool UVoxelChunkManager::QueryEditMergedSurface(
	double WorldX, double WorldY,
	float& OutHeight, FVector& OutNormal, float& OutSlopeDegrees,
	uint8& OutMaterialID, uint8& OutBiomeID) const
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("surface query"));
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
	const float EstZ = static_cast<float>(WM->GetSurfaceOnUpLine(FVector(WorldX, WorldY, 0.0), Configuration->NoiseParams).Position.Z);

	const FVector RelEst = FVector(WorldX, WorldY, EstZ) - Configuration->WorldOrigin;
	const FIntVector EstChunk = FVoxelCoordinates::WorldToChunk(RelEst, ChunkSize, VoxelSize);
	if (!IsChunkLoaded(EstChunk))
	{
		return false; // far band — caller falls back to the generator
	}

	// A block world's surface is the block tops it draws, not the density isosurface under them (WM-C5).
	if (FVoxelBlockSurface::UsesBlockSurface(*Configuration))
	{
		return QueryEditMergedBlockSurface(WorldX, WorldY, EstZ, OutHeight, OutNormal, OutSlopeDegrees, OutMaterialID, OutBiomeID);
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

bool UVoxelChunkManager::QueryEditMergedBlockSurface(double WorldX, double WorldY, float EstimateZ,
	float& OutHeight, FVector& OutNormal, float& OutSlopeDegrees, uint8& OutMaterialID, uint8& OutBiomeID) const
{
	const float VoxelSize = Configuration->VoxelSize;
	const int32 Window = Configuration->ChunkSize;
	const FVector& Origin = Configuration->WorldOrigin;

	// The voxel trees' leaves are not ground: a canopy is walked under, so the surface under it is the ground's top.
	uint64 LeafMask[4] = { 0, 0, 0, 0 };
	if (Configuration->TreeMode != EVoxelTreeMode::HISM && Configuration->TreeDensity > 0.0f)
	{
		for (const FVoxelTreeTemplate& Template : Configuration->TreeTemplates)
		{
			LeafMask[Template.LeafMaterialID >> 6] |= uint64(1) << (Template.LeafMaterialID & 63);
		}
	}
	auto IsGround = [&LeafMask](const FVoxelData& Voxel)
	{
		return Voxel.IsSolid() && (LeafMask[Voxel.MaterialID >> 6] & (uint64(1) << (Voxel.MaterialID & 63))) == 0;
	};

	// The block column containing (X, Y), read at its cells' centres so the position-to-voxel floor is unambiguous.
	const double CX = FVoxelBlockSurface::ColumnCorner(WorldX, Origin.X, VoxelSize) + 0.5 * VoxelSize;
	const double CY = FVoxelBlockSurface::ColumnCorner(WorldY, Origin.Y, VoxelSize) + 0.5 * VoxelSize;
	const int64 EstLayer = static_cast<int64>(FMath::FloorToDouble((static_cast<double>(EstimateZ) - Origin.Z) / VoxelSize));
	auto LayerCentre = [&Origin, VoxelSize](int64 Layer) { return Origin.Z + (static_cast<double>(Layer) + 0.5) * VoxelSize; };

	// Scan down from the top of a +/- one-chunk window around the estimate for the topmost ground voxel with no ground above.
	bool bAboveGround = IsGround(GetEditMergedVoxelAtWorldPosition(FVector(CX, CY, LayerCentre(EstLayer + Window + 1))));
	int64 TopLayer = TNumericLimits<int64>::Lowest();
	FVoxelData TopVoxel;
	for (int64 Layer = EstLayer + Window; Layer >= EstLayer - Window; --Layer)
	{
		const FVoxelData Voxel = GetEditMergedVoxelAtWorldPosition(FVector(CX, CY, LayerCentre(Layer)));
		const bool bGround = IsGround(Voxel);
		if (bGround && !bAboveGround)
		{
			TopLayer = Layer;
			TopVoxel = Voxel;
			break;
		}
		bAboveGround = bGround;
	}
	if (TopLayer == TNumericLimits<int64>::Lowest())
	{
		return false;
	}

	OutHeight = static_cast<float>(FVoxelBlockSurface::LayerTop(TopLayer, Origin.Z, VoxelSize));
	OutMaterialID = TopVoxel.MaterialID;
	OutBiomeID = TopVoxel.BiomeID;

	// The terrain's slope from the edit-merged density gradient, sampled inside the top block (a block's own face is flat).
	auto Density = [this](double X, double Y, double Z) -> float
	{
		return static_cast<float>(GetEditMergedVoxelAtWorldPosition(FVector(X, Y, Z)).Density);
	};
	const double Hz = LayerCentre(TopLayer);
	const float dX = Density(CX + VoxelSize, CY, Hz) - Density(CX - VoxelSize, CY, Hz);
	const float dY = Density(CX, CY + VoxelSize, Hz) - Density(CX, CY - VoxelSize, Hz);
	const float dZ = Density(CX, CY, Hz + VoxelSize) - Density(CX, CY, Hz - VoxelSize);
	OutNormal = FVector(-dX, -dY, -dZ).GetSafeNormal();
	if (OutNormal.IsNearlyZero())
	{
		OutNormal = FVector::UpVector;
	}
	else if (OutNormal.Z < 0.0f)
	{
		OutNormal = -OutNormal;
	}
	OutSlopeDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(static_cast<float>(OutNormal.Z), -1.0f, 1.0f)));
	return true;
}

bool UVoxelChunkManager::QueryEditMergedSurfacePoint(const FVector& WorldPos, FVoxelSurfaceSample& OutSample) const
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("surface query"));
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

	// The generated surface along the direction (the analytic one with the conditioning zones there, SP-B10) locates the
	// line and makes the near/far decision, as the analytic height does for the column.
	const FVector& Origin = Configuration->WorldOrigin;
	const FVector EstimatePosition = GetGeneratedSurfacePoint(WorldPos);
	const FVector Up = WM->GetUpVector(EstimatePosition);
	if (!IsChunkLoaded(FVoxelCoordinates::WorldToChunk(EstimatePosition - Origin, ChunkSize, VoxelSize)))
	{
		return false;
	}

	// A digital line of edit-merged voxels along local up through the estimate's voxel, one chunk either side (covers
	// dig-down / build-up). Voxel I's density sample sits at Origin + I * VoxelSize; read it from inside its cell.
	const FVector RelVoxel = (EstimatePosition - Origin) / VoxelSize;
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
	const double TopT = FVector::DotProduct(Origin + FVector(Indices[Found + 1]) * VoxelSize - EstimatePosition, Up) + 1.5 * VoxelSize;
	const double BottomT = FVector::DotProduct(Origin + FVector(Indices[Found]) * VoxelSize - EstimatePosition, Up) - 1.5 * VoxelSize;
	const double Step = 0.25 * VoxelSize;
	double HitT = TNumericLimits<double>::Max();
	float AboveDensity = Trilinear(EstimatePosition + Up * TopT);
	for (double T = TopT - Step; T >= BottomT && AboveDensity < Iso; T -= Step)
	{
		const float Density = Trilinear(EstimatePosition + Up * T);
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
		HitT = FVector::DotProduct(Crossing - EstimatePosition, Up);
	}

	OutSample.Position = EstimatePosition + Up * HitT;
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
	// A planet: the natural surface along the direction, with the planet conditioning zones there (SP-B10).
	if (ConditioningZones.Num() > 0 || TerrainConditioner != nullptr)
	{
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*WM);
		const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
		const FVector Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, Params.PlanetCenter);
		TArray<FVoxelConditioningZone> Zones;
		GatherConditioningZonesOnSphere(Direction, 0.0, Zones);
		if (Zones.Num() > 0)
		{
			return Params.PlanetCenter + Direction * (Params.PlanetRadius + Planet.GetConditionedTerrainAltitude(Direction, Configuration->NoiseParams, Zones));
		}
	}
	return WM->GetSurfacePoint(WorldPos, Configuration->NoiseParams).Position;
}

float UVoxelChunkManager::GetGeneratedPlanetAltitude(const FVector& Direction) const
{
	const IVoxelWorldMode* WM = GetWorldMode();
	if (!WM || !Configuration || WM->GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		return 0.0f;
	}
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*WM);
	const float Natural = Planet.GetTerrainAltitude(Direction, Configuration->NoiseParams);
	if (ConditioningZones.Num() == 0 && TerrainConditioner == nullptr)
	{
		return Natural;
	}
	// The zones at this direction (a point cap), blended as GenerateChunkSphericalPlanet blends them.
	TArray<FVoxelConditioningZone> Zones;
	GatherConditioningZonesOnSphere(Direction, 0.0, Zones);
	return Zones.Num() > 0
		? FVoxelTerrainConditioning::ApplyToAltitude(Direction, Planet.GetPlanetParams().PlanetRadius, Natural, Zones)
		: Natural;
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

	// Shared snapshots cached for seam jobs and cooks (G65: a far-compressed chunk's is decoded on a worker; counted at its
	// full size while it decodes; G71: a resident chunk's own array it shares is in the voxel data above).
	for (const auto& Pair : SeamSnapshotCache)
	{
		Stats.SnapshotBytes += GetSnapshotOwnBytes(Pair.Key, Pair.Value);
	}

	// The neighbours' boundary shells for collision cooks (G69), the uniform values' shared shells once each.
	{
		TSet<const FVoxelChunkShell*> Shared;
		for (const auto& Pair : UniformShells)
		{
			if (Pair.Value.IsValid())
			{
				Shared.Add(Pair.Value.Get());
				Stats.ShellBytes += static_cast<int64>(Pair.Value->GetAllocatedSize());
			}
		}
		const int64 ChunkSizeForShell = Configuration ? Configuration->ChunkSize : 0;
		for (const auto& Pair : ShellCache)
		{
			const FChunkShellEntry& Entry = Pair.Value;
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

UVoxelMaterialAtlas* UVoxelChunkManager::GetMaterialAtlas() const
{
	return MeshRenderer ? MeshRenderer->GetMaterialAtlas() : nullptr;
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
		Context.FieldOfView = 90.0f;
		if (Configuration)
		{
			Context.WorldOrigin = Configuration->WorldOrigin;
			Context.WorldMode = Configuration->WorldMode;
			Context.MaxChunksToUnloadPerFrame = Configuration->MaxChunksToUnloadPerFrame;
		}
		Context.bFarFieldActive = FarField.IsValid();
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
		Context.WorldOrigin = Configuration->WorldOrigin;
		Context.WorldMode = Configuration->WorldMode;
		Context.MaxChunksToUnloadPerFrame = Configuration->MaxChunksToUnloadPerFrame;
	}
	// The cut-over applies only with a far field (WM-MP2, D38; every mode since G78).
	Context.bFarFieldActive = FarField.IsValid();

	return Context;
}

FLODQueryContext UVoxelChunkManager::BuildStreamingContext() const
{
	FLODQueryContext Context = BuildQueryContext();
	if (StreamingViewerLead > 0.0)
	{
		// Under the camera by this tick's lead, along the local down the prefetch found there (WM-MP3: every world mode).
		Context.ViewerPosition += StreamingViewerDown * StreamingViewerLead;
		Context.ViewerLead = StreamingViewerLead;
	}
	return Context;
}

double UVoxelChunkManager::ComputeDescentLead(double RadialSpeed, double AltitudeOverSurface, double LeadSeconds, double MinDescentSpeed,
	double Clearance)
{
	const double Descent = -RadialSpeed;
	if (LeadSeconds <= 0.0 || Descent <= FMath::Max(MinDescentSpeed, 0.0))
	{
		return 0.0;
	}
	// At most half the altitude: a descent that slows toward the ground (vox.FlyTo's falls by the same factor every
	// second) would otherwise put the viewer at the ground from any height and resume the window far above the cut-over.
	const double MaxLead = FMath::Min(0.5 * AltitudeOverSurface, AltitudeOverSurface - Clearance);
	return FMath::Clamp(Descent * LeadSeconds, 0.0, FMath::Max(MaxLead, 0.0));
}

void UVoxelChunkManager::ApplyDescentPrefetch(FLODQueryContext& Context, float DeltaTime)
{
	const double LeadSeconds = CVarStreamDescentLead.GetValueOnGameThread();
	if (!Configuration || !WorldMode || LeadSeconds <= 0.0)
	{
		StreamingViewerLead = 0.0;
		CameraRadialSpeed = 0.0;
		LastPrefetchCamera = FVector(FLT_MAX);
		return;
	}

	// Local up at the camera and its height along it: on a planet the direction from the centre and the distance to it;
	// on a heightmap world +Z and Z (world modes review WM-MP3: the prefetch in every world mode).
	const bool bPlanet = Configuration->WorldMode == EWorldMode::SphericalPlanet;
	const FVector Centre = Configuration->WorldOrigin;
	const FVector Camera = Context.ViewerPosition;
	const FVector Up = bPlanet ? (Camera - Centre).GetSafeNormal() : FVector::UpVector;
	if (Up.IsNearlyZero())
	{
		StreamingViewerLead = 0.0;
		return;
	}
	auto HeightOf = [bPlanet, &Centre](const FVector& Position) { return bPlanet ? FVector::Dist(Position, Centre) : Position.Z; };

	// The camera's speed along local up, smoothed. A jump of more than ten chunks in a frame is a teleport (or a switch of
	// view target), not motion: the speed starts over.
	const double ChunkWorldSize = Configuration->GetChunkWorldSize();
	const double CameraHeight = HeightOf(Camera);
	if (DeltaTime > SMALL_NUMBER && LastPrefetchCamera.X != FLT_MAX && FVector::DistSquared(Camera, LastPrefetchCamera) < FMath::Square(10.0 * ChunkWorldSize))
	{
		const double Instant = (CameraHeight - HeightOf(LastPrefetchCamera)) / DeltaTime;
		CameraRadialSpeed = FMath::FInterpTo(CameraRadialSpeed, Instant, static_cast<double>(DeltaTime), 4.0);
	}
	else
	{
		CameraRadialSpeed = 0.0;
	}
	LastPrefetchCamera = Camera;

	// Ahead of the camera along local down by what it covers in LeadSeconds, stopping two chunks over the surface under it
	// (the generated terrain under the camera: along its direction on a planet, at its (X, Y) on a heightmap world; or the
	// sea where that is higher): the near window's layers stay over the ground.
	double SurfaceHeight;
	if (bPlanet)
	{
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*WorldMode);
		SurfaceHeight = Configuration->WorldRadius + static_cast<double>(Planet.GetTerrainAltitude(Up, Configuration->NoiseParams));
		if (Configuration->bEnableWaterLevel)
		{
			SurfaceHeight = FMath::Max(SurfaceHeight, static_cast<double>(Configuration->WaterRadius));
		}
	}
	else
	{
		SurfaceHeight = GetGeneratedSurfaceHeight(Camera.X, Camera.Y);
		const FVoxelSeaSurface Sea = GetSeaSurface();
		if (Sea.bEnabled)
		{
			SurfaceHeight = FMath::Max(SurfaceHeight, Sea.PlaneZ);
		}
	}
	StreamingViewerLead = ComputeDescentLead(CameraRadialSpeed, CameraHeight - SurfaceHeight, LeadSeconds,
		CVarStreamDescentMinSpeed.GetValueOnGameThread(), 2.0 * ChunkWorldSize);
	StreamingViewerDown = -Up;
	if (StreamingViewerLead > 0.0)
	{
		Context.ViewerPosition = Camera + StreamingViewerDown * StreamingViewerLead;
		// The cut-over reads the camera's altitude from it (G49: a suspended window resumes only with the camera under it).
		Context.ViewerLead = StreamingViewerLead;
	}
}

bool UVoxelChunkManager::IsVoxelWindowSuspended() const
{
	return LODStrategy && LODStrategy->IsWindowSuspended();
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
	// to the unload radius, so the 1.0-1.2x band is hysteresis, as for every other chunk. None above
	// a planet's voxel cut-over, where the window holds nothing (WM-BP10): the edits stay in their
	// layers and load with the window when it resumes.
	if (EditManager && !LODStrategy->IsWindowSuspended())
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
	// Above a planet's voxel cut-over (WM-BP10) edited chunks unload too: their edits stay in the edit layers.
	const bool bWindowSuspended = LODStrategy->IsWindowSuspended();
	auto CanUnload = [this, &Context, EditPinDistance, ChunkWorldSize, bWindowSuspended](const FIntVector& ChunkCoord) -> bool
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
		if (!bWindowSuspended && EditManager && ChunkWorldSize > 0.0f && EditManager->ChunkHasEdits(ChunkCoord))
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

		FVoxelNoiseGenerationRequest GenRequest;
		BuildChunkGenerationRequest(Request.ChunkCoord, Request.LODLevel, GenRequest);

		// Launch async generation on thread pool
		LaunchAsyncGeneration(Request, MoveTemp(GenRequest));

		++ProcessedCount;
	}

	for (const FChunkLODRequest& Deferred : DeferredInFlight)
	{
		AddToGenerationQueue(Deferred);
	}
}

void UVoxelChunkManager::BuildChunkGenerationRequest(const FIntVector& ChunkCoord, int32 LODLevel, FVoxelNoiseGenerationRequest& OutRequest) const
{
	OutRequest.ChunkCoord = ChunkCoord;
	// Force LOD 0 when LOD system is disabled (defense-in-depth)
	OutRequest.LODLevel = (Configuration->IsLODEnabled()) ? LODLevel : 0;
	OutRequest.ChunkSize = Configuration->ChunkSize;
	OutRequest.VoxelSize = Configuration->VoxelSize;
	OutRequest.NoiseParams = Configuration->NoiseParams;
	// World mode, origin, terrain params and the island / planet params, from the one builder (WM-A1).
	FVoxelWorldModeFactory::FillGenerationRequest(*Configuration, OutRequest);

	// Biome configuration (contains biome definitions, blend settings, height rules)
	OutRequest.bEnableBiomes = Configuration->bEnableBiomes;
	OutRequest.BiomeConfiguration = Configuration->BiomeConfiguration;

	// Cave parameters
	OutRequest.bEnableCaves = Configuration->bEnableCaves;
	OutRequest.CaveConfiguration = Configuration->CaveConfiguration;

	// Water level parameters
	OutRequest.bEnableWaterLevel = Configuration->bEnableWaterLevel;
	OutRequest.WaterLevel = Configuration->WaterLevel;
	OutRequest.WaterRadius = Configuration->WaterRadius;

	// Terrain conditioning zones overlapping this chunk (Phase 6c: flatten under POIs/claims)
	GatherConditioningZonesForChunk(ChunkCoord, OutRequest.ConditioningZones);
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

bool UVoxelChunkManager::IsBlockSurface() const
{
	return Configuration && FVoxelBlockSurface::UsesBlockSurface(*Configuration);
}

float UVoxelChunkManager::SnapToBlockTop(float Height) const
{
	if (!IsBlockSurface())
	{
		return Height;
	}
	return static_cast<float>(FVoxelBlockSurface::BlockTopForHeight(Height, Configuration->WorldOrigin.Z, Configuration->VoxelSize));
}

bool UVoxelChunkManager::HasVoxelTrees() const
{
	// The same gate as the generation request's tree injection.
	return bIsInitialized && Configuration && WorldMode
		&& Configuration->WorldMode != EWorldMode::SphericalPlanet
		&& Configuration->MeshingMode == EMeshingMode::Cubic
		&& Configuration->TreeMode != EVoxelTreeMode::HISM
		&& Configuration->TreeTemplates.Num() > 0
		&& Configuration->TreeDensity > 0.0f;
}

FVector UVoxelChunkManager::GlobalVoxelToWorldCentre(const FIntVector& GlobalVoxel) const
{
	const float VoxelSize = Configuration ? Configuration->VoxelSize : 100.0f;
	const FVector Origin = Configuration ? Configuration->WorldOrigin : FVector::ZeroVector;
	return Origin + (FVector(GlobalVoxel) + FVector(0.5)) * VoxelSize;
}

bool UVoxelChunkManager::FindVoxelTreeAt(const FVector& WorldPos, FVoxelTreeInstance& OutTree) const
{
	if (!HasVoxelTrees())
	{
		return false;
	}
	const float VoxelSize = Configuration->VoxelSize;
	const FVector Relative = (WorldPos - Configuration->WorldOrigin) / VoxelSize;
	const FIntVector GlobalVoxel(FMath::FloorToInt32(Relative.X), FMath::FloorToInt32(Relative.Y), FMath::FloorToInt32(Relative.Z));

	TArray<FVoxelTreeInstance> Candidates;
	FVoxelTreeInjector::FindTreesWithTrunkAt(GlobalVoxel, Configuration->ChunkSize, VoxelSize, Configuration->WorldOrigin,
		Configuration->WorldSeed, Configuration->TreeTemplates, Configuration->NoiseParams, *WorldMode, Configuration->TreeDensity,
		Configuration->BiomeConfiguration, Configuration->bEnableWaterLevel, Configuration->WaterLevel, Candidates);

	// The voxel must still be that tree's trunk block; where two trunks touch, the tree with more of its trunk left wins.
	const FVoxelData Voxel = GetEditMergedVoxelAtWorldPosition(GlobalVoxelToWorldCentre(GlobalVoxel));
	int32 BestTrunk = 0;
	TArray<FVoxelTreeVoxel> Blocks;
	for (const FVoxelTreeInstance& Candidate : Candidates)
	{
		const FVoxelTreeTemplate& Template = Configuration->TreeTemplates[Candidate.TemplateIndex];
		if (!Voxel.IsSolid() || Voxel.MaterialID != Template.TrunkMaterialID)
		{
			continue;
		}
		GetVoxelTreeBlocks(Candidate, Blocks);
		const int32 Trunk = Blocks.FilterByPredicate([](const FVoxelTreeVoxel& B) { return B.bTrunk; }).Num();
		if (Trunk > BestTrunk)
		{
			BestTrunk = Trunk;
			OutTree = Candidate;
		}
	}
	return BestTrunk > 0;
}

void UVoxelChunkManager::FindVoxelTreesNear(const FVector& WorldPos, float Radius, TArray<FVoxelTreeInstance>& OutTrees) const
{
	OutTrees.Reset();
	if (!HasVoxelTrees())
	{
		return;
	}
	const float VoxelSize = Configuration->VoxelSize;
	const int32 ChunkSize = Configuration->ChunkSize;
	const double ChunkWorldSize = static_cast<double>(ChunkSize) * VoxelSize;
	const FVector Relative = WorldPos - Configuration->WorldOrigin;
	const int32 CX0 = FMath::FloorToInt32((Relative.X - Radius) / ChunkWorldSize);
	const int32 CX1 = FMath::FloorToInt32((Relative.X + Radius) / ChunkWorldSize);
	const int32 CY0 = FMath::FloorToInt32((Relative.Y - Radius) / ChunkWorldSize);
	const int32 CY1 = FMath::FloorToInt32((Relative.Y + Radius) / ChunkWorldSize);
	TArray<TPair<double, FVoxelTreeInstance>> Found;
	TArray<FVoxelTreeVoxel> Blocks;
	for (int32 CX = CX0; CX <= CX1; ++CX)
	{
		for (int32 CY = CY0; CY <= CY1; ++CY)
		{
			TArray<FIntVector> Positions;
			TArray<int32> Templates;
			TArray<uint32> Seeds;
			FVoxelTreeInjector::ComputeTreePositionsForChunk(FIntVector(CX, CY, 0), ChunkSize, VoxelSize, Configuration->WorldOrigin,
				Configuration->WorldSeed, Configuration->NoiseParams, *WorldMode, Configuration->TreeDensity, Configuration->TreeTemplates,
				Configuration->BiomeConfiguration, Configuration->bEnableWaterLevel, Configuration->WaterLevel, Positions, Templates, Seeds);
			for (int32 i = 0; i < Positions.Num(); ++i)
			{
				FVoxelTreeInstance Tree;
				Tree.BaseVoxel = Positions[i];
				Tree.TemplateIndex = Templates[i];
				Tree.Seed = Seeds[i];
				const double Distance = FVector::Dist2D(GlobalVoxelToWorldCentre(Tree.BaseVoxel), WorldPos);
				if (Distance > Radius)
				{
					continue;
				}
				GetVoxelTreeBlocks(Tree, Blocks);
				if (Blocks.ContainsByPredicate([&Tree](const FVoxelTreeVoxel& B) { return B.bTrunk && B.GlobalVoxel.Z > Tree.BaseVoxel.Z; }))
				{
					Found.Emplace(Distance, Tree);
				}
			}
		}
	}
	Found.Sort([](const TPair<double, FVoxelTreeInstance>& A, const TPair<double, FVoxelTreeInstance>& B) { return A.Key < B.Key; });
	for (const TPair<double, FVoxelTreeInstance>& Entry : Found)
	{
		OutTrees.Add(Entry.Value);
	}
}

void UVoxelChunkManager::GetVoxelTreeBlocks(const FVoxelTreeInstance& Tree, TArray<FVoxelTreeVoxel>& OutBlocks) const
{
	OutBlocks.Reset();
	if (!Configuration || !Configuration->TreeTemplates.IsValidIndex(Tree.TemplateIndex))
	{
		return;
	}
	TArray<FVoxelTreeVoxel> Stamp;
	FVoxelTreeInjector::CollectTreeVoxels(Tree.BaseVoxel, Configuration->TreeTemplates[Tree.TemplateIndex], Tree.Seed, Stamp);
	TSet<FIntVector> Seen;
	// Later entries win (the trunk is stamped after the canopy and punches through it), so walk the stamp backwards.
	for (int32 i = Stamp.Num() - 1; i >= 0; --i)
	{
		const FVoxelTreeVoxel& Block = Stamp[i];
		bool bAlreadySeen = false;
		Seen.Add(Block.GlobalVoxel, &bAlreadySeen);
		if (bAlreadySeen)
		{
			continue;
		}
		const FVoxelData Voxel = GetEditMergedVoxelAtWorldPosition(GlobalVoxelToWorldCentre(Block.GlobalVoxel));
		if (Voxel.IsSolid() && Voxel.MaterialID == Block.MaterialID)
		{
			OutBlocks.Add(Block);
		}
	}
}

int32 UVoxelChunkManager::FellVoxelTree(const FVoxelTreeInstance& Tree, bool bKeepStump)
{
	if (!EditManager || !Tree.IsValid())
	{
		return 0;
	}
	TArray<FVoxelTreeVoxel> Blocks;
	GetVoxelTreeBlocks(Tree, Blocks);
	// Felling is gameplay, as chopping a scatter tree is: it stays out of the undo history (as POI stamps and the flood do).
	EditManager->BeginEditOperation(TEXT("Fell voxel tree"), /*bAddToUndoHistory=*/false);
	int32 Removed = 0;
	for (const FVoxelTreeVoxel& Block : Blocks)
	{
		if (bKeepStump && Block.bTrunk && Block.GlobalVoxel.Z == Tree.BaseVoxel.Z)
		{
			continue;
		}
		EditManager->ApplyEdit(GlobalVoxelToWorldCentre(Block.GlobalVoxel), FVoxelData::Air(), EEditMode::Set);
		++Removed;
	}
	EditManager->EndEditOperation();
	return Removed;
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
	// The configuration's point gives the direction; the terrain along it is the generated one, a POI's pad included (SP-B11,
	// SP-B10).
	const FVector Surface = GetGeneratedSurfacePoint(Configuration->GetPlanetSpawnPosition());
	return Surface + GetUpVectorAt(Surface) * Configuration->PlanetSpawnAltitude;
}

FVoxelSeaSurface UVoxelChunkManager::GetSeaSurface() const
{
	if (!Configuration)
	{
		return FVoxelSeaSurface();
	}
	return FVoxelSeaSurface::FromConfig(*Configuration);
}

float UVoxelChunkManager::GetSeaDepthAt(const FVector& WorldPos) const
{
	const FVoxelSeaSurface Sea = GetSeaSurface();
	return Sea.bEnabled ? static_cast<float>(Sea.GetDepth(WorldPos)) : -1.0e9f;
}

double UVoxelChunkManager::ComputePlanetKillZ(double CurrentKillZ, const FVoxelWorldBounds& Bounds, double Margin)
{
	if (Bounds.Shape != EVoxelWorldBoundsShape::Shell)
	{
		return CurrentKillZ;
	}
	// The world reaches up to the orbit ceiling (spherical Phase 6): KillZ goes below that, not below the terrain.
	const double Reach = FMath::Max(Bounds.OrbitRadius, static_cast<double>(Bounds.OuterRadius));
	const double Below = Bounds.Center.Z - Reach - FMath::Max(Margin, 0.0);
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
	// The engine's KillZ is a Z plane: a planet bigger than it reaches would lose its southern hemisphere to it, and a pawn
	// flying over that hemisphere would die at any altitude. Below the orbit ceiling it never touches the planet or the
	// space around it, and bodies that leave the world along world -Z are still removed.
	const FVoxelWorldBounds Bounds = WorldMode->GetWorldBounds();
	const double NewKillZ = ComputePlanetKillZ(Settings->KillZ, Bounds, Configuration->GetChunkWorldSize());
	if (NewKillZ < Settings->KillZ)
	{
		UE_LOG(LogVoxelStreaming, Log, TEXT("Planet: KillZ moved from %.0f to %.0f, below the planet's world (centre Z %.0f, orbit radius %.0f)."),
			Settings->KillZ, NewKillZ, Bounds.Center.Z, Bounds.OrbitRadius);
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
	FarOceanSettings.SeabedHeight = [Island](double Distance)
	{
		return static_cast<double>(FIslandBowlWorldMode::GetSeabedHeight(static_cast<float>(Distance), Island));
	};
	FarOceanSettings.SeabedMaterialID = ResolveFarSeabedMaterial(Island);

	const FVector Origin = FarOceanSettings.GetMeshOrigin();
	const bool bVisible = CVarFarOcean.GetValueOnGameThread() != 0;

	// The seabed and the open sea never change: one component. The seabed takes the terrain's own material instance
	// (the atlas parameters), so it reads as the voxel seabed it continues.
	FarOceanStatic = MakeComponent(Owner, TEXT("VoxelFarOceanStatic"), Origin);
	FChunkMeshData Mesh;
	const int32 SeabedTriangles = FVoxelFarOcean::BuildSeabed(FarOceanSettings, Mesh);
	SetSection(FarOceanStatic, 0, Mesh, MeshRenderer ? MeshRenderer->GetSurfaceMaterial() : nullptr);

	// The water needs the water tiles' material (without it the world actor draws its fallback plane). Inside the bounds box
	// the far field draws the terrain and the sea (WM-MP1, D37); the open sea starts at the box.
	const bool bWater = Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial;
	int32 OpenSeaQuads = 0;
	if (bWater)
	{
		FChunkMeshData Underside;
		OpenSeaQuads = FVoxelFarOcean::BuildOpenSeaWater(FarOceanSettings, Mesh, &Underside);
		SetSection(FarOceanStatic, 1, Mesh, Configuration->WaterMeshMaterial);
		SetSection(FarOceanStatic, 2, Underside, Configuration->WaterMeshMaterial);
		FarOceanStatic->SetTranslucentSortPriority(0);
	}
	FarOceanStatic->SetVisibility(bVisible);
	bFarOceanUndersideShown = true; // force the first update to hide it
	UpdateFarOceanUnderside(CurrentViewerPosition);

	FIntPoint GridMin, GridMax;
	FVoxelFarOcean::GetGridColumns(FarOceanSettings, GridMin, GridMax);
	UE_LOG(LogVoxelStreaming, Log,
		TEXT("Far ocean: seabed %d triangle(s) (material %d, %.0f uu under the profile), %s from the %dx%d column box; reaches %.0f uu past the edge; built in %.2f ms%s"),
		SeabedTriangles, FarOceanSettings.SeabedMaterialID, FarOceanSettings.SeabedOffset,
		bWater ? *FString::Printf(TEXT("open sea %d quad(s)"), OpenSeaQuads) : TEXT("no water (needs bEnableWaterLevel and WaterMeshMaterial)"),
		GridMax.X - GridMin.X + 1, GridMax.Y - GridMin.Y + 1, FarOceanSettings.OuterDistance,
		(FPlatformTime::Seconds() - StartTime) * 1000.0, bVisible ? TEXT("") : TEXT(" (hidden: voxel.World.FarOcean 0)"));
}

void UVoxelChunkManager::DestroyFarOcean()
{
	if (IsValid(FarOceanStatic))
	{
		FarOceanStatic->DestroyComponent();
	}
	FarOceanStatic = nullptr;
	bFarOceanUndersideShown = false;
}

void UVoxelChunkManager::UpdateUnderside(bool bBelow, bool& bShown, TFunctionRef<void(bool)> Apply)
{
	// A sea's underside shows only while the viewer is under its surface (code review CR2, CM-19: the one rule for the
	// far ocean, the planet ocean and the far field's sea).
	if (bBelow == bShown)
	{
		return;
	}
	bShown = bBelow;
	Apply(bBelow);
}

void UVoxelChunkManager::UpdateFarOceanUnderside(const FVector& ViewerPosition)
{
	UpdateUnderside(ViewerPosition.Z < FarOceanSettings.WaterZ, bFarOceanUndersideShown, [this](bool bBelow)
	{
		if (IsValid(FarOceanStatic) && FarOceanStatic->GetNumSections() > 2)
		{
			FarOceanStatic->SetMeshSectionVisible(2, bBelow);
		}
	});
}

void UVoxelChunkManager::SetFarOceanVisible(bool bVisible)
{
	if (IsValid(FarOceanStatic))
	{
		FarOceanStatic->SetVisibility(bVisible);
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
	if (FVoxelPlanetFarField::ShouldDrawSea(Configuration->WaterRadius, PlanetOceanSettings.MaxSag, PlanetOceanSettings.MinSegments,
		PlanetOceanSettings.MaxSegments))
	{
		// At scale the shell's quads sag past its budget (G40: up to 3,189 uu on a 1,000 km planet, its vertices floats of
		// size R): the far field's sea patches draw the sea instead, and no shell is built (with voxel.World.FarField 0,
		// a development switch, a big planet has no sea: code review CR1, D-CR13).
		UE_LOG(LogVoxelStreaming, Log, TEXT("Planet ocean: drawn by the far field (radius %.0f: the shell would sag %.0f uu at its %d quads a face edge)."),
			Configuration->WaterRadius, FVoxelPlanetOcean::MeasureMaxSag(Configuration->WaterRadius,
				FVoxelPlanetOcean::ChooseSegments(Configuration->WaterRadius, PlanetOceanSettings.MaxSag, PlanetOceanSettings.MinSegments,
					PlanetOceanSettings.MaxSegments)), PlanetOceanSettings.MaxSegments);
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
	UpdateUnderside(bInside, bPlanetOceanUndersideShown, [this](bool bBelow)
	{
		if (PlanetOcean->GetNumSections() > 1)
		{
			PlanetOcean->SetMeshSectionVisible(1, bBelow);
		}
	});
}

void UVoxelChunkManager::SetPlanetOceanVisible(bool bVisible)
{
	if (IsValid(PlanetOcean))
	{
		PlanetOcean->SetVisibility(bVisible);
		UE_LOG(LogVoxelStreaming, Log, TEXT("Planet ocean %s"), bVisible ? TEXT("shown") : TEXT("hidden"));
	}
}

// ==================== Planet Far Field (spherical Phase 6, WM-BP9) ====================

namespace VoxelFarFieldManagerPrivate
{
	/** A procedural mesh component for one far-field patch at its origin (no collision, no shadow, nothing to navigate). */
	UProceduralMeshComponent* MakePatchComponent(AActor* Owner, const FVector& Location)
	{
		UProceduralMeshComponent* Component = NewObject<UProceduralMeshComponent>(Owner, NAME_None, RF_Transient);
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

	/** Hand a far-field section to a procedural mesh section, its triangles reversed for an underside; clears it when empty. */
	void SetPatchSection(UProceduralMeshComponent* Component, int32 Section, const FVoxelFarFieldSection& Mesh, bool bReverse,
		UMaterialInterface* Material)
	{
		if (Mesh.IsEmpty())
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
		Tangents.Reserve(N);
		for (int32 i = 0; i < N; ++i)
		{
			Vertices[i] = FVector(Mesh.Positions[i]);
			Normals[i] = FVector(Mesh.Normals[i]);
			UV0[i] = FVector2D(Mesh.UV0[i]);
			UV1[i] = FVector2D(Mesh.UV1[i]);
			Tangents.Add(FProcMeshTangent(FVector(Mesh.Tangents[i]), false));
		}
		TArray<int32> Triangles;
		if (bReverse)
		{
			Triangles.Reserve(Mesh.Triangles.Num());
			for (int32 i = 0; i + 2 < Mesh.Triangles.Num(); i += 3)
			{
				Triangles.Append({ Mesh.Triangles[i], Mesh.Triangles[i + 2], Mesh.Triangles[i + 1] });
			}
		}
		else
		{
			Triangles = Mesh.Triangles;
		}
		const TArray<FVector2D> NoUV;
		Component->CreateMeshSection(Section, Vertices, Triangles, Normals, UV0, UV1, NoUV, NoUV, Mesh.Colors, Tangents,
			/*bCreateCollision=*/false);
		if (Material)
		{
			Component->SetMaterial(Section, Material);
		}
	}

	/** What a component's sections hold on the CPU and the GPU (an estimate: the proc-mesh vertex and a GPU vertex). */
	int64 EstimateBytes(int64 Vertices, int64 Indices)
	{
		return Vertices * (static_cast<int64>(sizeof(FProcMeshVertex)) + 64) + Indices * 8;
	}
}

void UVoxelChunkManager::RebuildFarField(bool bEnabled)
{
	DestroyFarField();

	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	// Visual only: a dedicated server draws nothing.
	if (!bEnabled || !Configuration || !WorldMode || !World || !World->IsGameWorld() || !Owner || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	const bool bPlanet = Configuration->WorldMode == EWorldMode::SphericalPlanet;
	const bool bHeightmap = Configuration->WorldMode == EWorldMode::InfinitePlane || Configuration->WorldMode == EWorldMode::IslandBowl;
	if (!bPlanet && !bHeightmap)
	{
		return;
	}
	TSharedPtr<const FVoxelFarFieldSurface> Surface = FVoxelFarFieldSurface::Make(*Configuration, WorldMode);
	if (!Surface.IsValid())
	{
		return;
	}

	FVoxelFarFieldSettings Settings;
	Settings.GridOrigin = Configuration->WorldOrigin;
	Settings.ChunkWorldSize = Configuration->GetChunkWorldSize();
	Settings.VoxelSize = Configuration->VoxelSize;
	// Quads along a patch's edge (16: a band patch's error stays a fraction of the sink on the review planets, and the
	// procedural mesh components hold about 200 bytes a vertex, CPU and GPU copies).
	Settings.PatchQuads = static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::Clamp(CVarFarFieldQuads.GetValueOnGameThread(), 4, 64))));
	// A voxel under the analytic surface: the voxel surface (which DC and MC follow within a few uu) hides it wherever a
	// chunk is drawn over it.
	Settings.Sink = Configuration->VoxelSize;
	Settings.SeaMargin = 2.0 * Configuration->VoxelSize;
	if (bPlanet)
	{
		const FSphericalPlanetWorldMode& Planet = Surface->GetPlanet();
		const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
		Settings.Center = Params.PlanetCenter;
		Settings.Radius = Params.PlanetRadius;
		// The terrain's real bounds within the configured shell (terrain past it is cut off), as the streaming window's (WM-BP8).
		float MinAltitude = 0.0f;
		float MaxAltitude = 0.0f;
		FSphericalPlanetWorldMode::GetDisplacementBounds(Planet.GetTerrainParams(), &Planet.GetBiomeSnapshot(), MinAltitude, MaxAltitude);
		Settings.MinAltitude = FMath::Max(MinAltitude, -Params.MaxTerrainDepth);
		Settings.MaxAltitude = FMath::Min(MaxAltitude, Params.MaxTerrainHeight);
		// The sea is the far field's only at scale, where the ocean shell would sag past its budget (G40); the small planet
		// keeps its shell.
		const FVoxelPlanetOceanSettings OceanDefaults;
		Settings.bSea = Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial
			&& FVoxelPlanetFarField::ShouldDrawSea(Configuration->WaterRadius, OceanDefaults.MaxSag, OceanDefaults.MinSegments, OceanDefaults.MaxSegments);
		Settings.SeaRadius = Configuration->WaterRadius;
		Settings.BandLevel = FVoxelFarFieldSettings::ComputeBandLevel(Settings.Radius, Settings.ChunkWorldSize);
		// Zones (POI pads, a few chunks across) only for patches up to 32 chunks in radius: past that they do not show, and a
		// wide cap would ask the POI system for every POI in it (G37).
		Settings.ZoneGatherMax = 32.0 * Settings.ChunkWorldSize / FMath::Max(Settings.Radius, 1.0);
	}
	else
	{
		// A heightmap world (world modes review WM-MP1, D36): root tiles of RootChunks chunks on the chunk grid, split down
		// to one chunk column at the voxel window's edge, out to FarDistance from the camera (and an island's bounds).
		Settings.bHeightmap = true;
		Settings.FarDistance = FMath::Max(0.0f, CVarFarFieldDistance.GetValueOnGameThread());
		Settings.Bounds = WorldMode->GetWorldBounds();
		const int32 RootChunksCVar = CVarFarFieldRootChunks.GetValueOnGameThread();
		if (RootChunksCVar > 0)
		{
			Settings.RootChunks = static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::Clamp(RootChunksCVar, 4, 4096))));
		}
		else
		{
			const double Reach = Settings.FarDistance > 0.0 ? Settings.FarDistance
				: (Settings.Bounds.IsBounded() ? FMath::Max(Settings.Bounds.HalfExtent.X, Settings.Bounds.HalfExtent.Y) : 2.0e6);
			Settings.RootChunks = FVoxelFarFieldSettings::ComputeHeightmapRootChunks(Reach, Settings.ChunkWorldSize);
		}
		const FVoxelWorldBounds HeightBounds = WorldMode->GetWorldBounds();
		Settings.MinAltitude = HeightBounds.MinZ;
		Settings.MaxAltitude = HeightBounds.MaxZ;
		// The sea at WaterLevel wherever the terrain dips near or under it, as the water tiles draw it over the streamed
		// columns (D37: an island's far ocean keeps its open sea past the bounds and its seabed; its grid water and floor
		// inside the bounds are the far field's now).
		Settings.bSea = Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial;
		Settings.SeaZ = Configuration->WaterLevel;
		Settings.BandLevel = FVoxelFarFieldSettings::ComputeHeightmapBandLevel(Settings.RootChunks);
		// Zones for patches up to 64 chunks across (a root tile of 64 included): the gather is a box query over the zones.
		Settings.ZoneGatherMax = 64.0 * Settings.ChunkWorldSize + 1.0;
	}

	FarField = MakeUnique<FVoxelPlanetFarField>();
	FarField->Initialize(Settings, Surface);
	FarField->SetMaxBuildsInFlight(CVarFarFieldMaxBuilds.GetValueOnGameThread());
	// The cut-over is in force only with a far field (D38, every mode since G78): told now, not at the next Update.
	if (LODStrategy)
	{
		LODStrategy->SetFarFieldActive(true);
	}
	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);
	// A build's zones: a cap's on the sphere, a box's in the plane (the region is the patch's footprint, FVoxelFarFieldFrame).
	FarField->SetZoneGatherer([WeakThis](const FVoxelFarFieldRegion& Region, TArray<FVoxelConditioningZone>& OutZones)
	{
		if (const UVoxelChunkManager* Manager = WeakThis.Get())
		{
			if (Region.bCap)
			{
				Manager->GatherConditioningZonesOnSphere(Region.Direction, Region.AngularRadius, OutZones);
			}
			else
			{
				Manager->GatherConditioningZonesForRegion(Region.Box, OutZones);
			}
		}
	});
	FarField->SetWaterTileQuery([WeakThis](const FIntPoint& Column)
	{
		const UVoxelChunkManager* Manager = WeakThis.Get();
		return Manager && Manager->HasWaterTile(Column);
	});
	FarFieldStats = FVoxelFarFieldStats();
	FarFieldStats.bActive = true;
	FarFieldStats.bDrawsSea = Settings.bSea;
	bFarFieldUndersideShown = false;
	FarFieldGroundViewer = FVector(TNumericLimits<double>::Max());

	if (bPlanet)
	{
		UE_LOG(LogVoxelStreaming, Log,
			TEXT("Far field: radius %.0f, %d quads a patch, band level %d (patches %.0f uu across near the voxel window), drawn %.0f uu under the surface, altitudes %.0f to %.0f, sea %s"),
			Settings.Radius, Settings.PatchQuads, Settings.BandLevel, Settings.GetPatchArc(Settings.BandLevel), Settings.Sink,
			Settings.MinAltitude, Settings.MaxAltitude,
			Settings.bSea ? *FString::Printf(TEXT("drawn by the far field (radius %.0f)"), Settings.SeaRadius)
				: (Configuration->bEnableWaterLevel ? TEXT("drawn by the ocean shell") : TEXT("off")));
	}
	else
	{
		UE_LOG(LogVoxelStreaming, Log,
			TEXT("Far field (heightmap): root tiles of %d chunks (%.0f uu), %d quads a patch, band level %d (patches %.0f uu across near the voxel window), out to %.0f uu from the camera%s, drawn %.0f uu under the surface, heights %.0f to %.0f, sea %s%s"),
			Settings.RootChunks, Settings.GetRootTileSize(), Settings.PatchQuads, Settings.BandLevel, Settings.GetPatchArc(Settings.BandLevel),
			Settings.FarDistance, Settings.Bounds.IsBounded() ? TEXT(" and within the world's bounds") : TEXT(""), Settings.Sink,
			Settings.MinAltitude, Settings.MaxAltitude,
			Settings.bSea ? *FString::Printf(TEXT("drawn by the far field at Z %.0f"), Settings.SeaZ) : TEXT("off"),
			Surface->bBlockSurface ? TEXT("; block tops (cubic)") : TEXT(""));
	}
}

void UVoxelChunkManager::DestroyFarField()
{
	for (TPair<uint64, TObjectPtr<UProceduralMeshComponent>>& Pair : FarFieldComponents)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->DestroyComponent();
		}
	}
	FarFieldComponents.Reset();
	FarFieldComponentSizes.Reset();
	FarFieldComponentVertices = 0;
	FarFieldComponentIndices = 0;
	FarField.Reset();
	FarFieldStats = FVoxelFarFieldStats();
	bFarFieldUndersideShown = false;
	if (LODStrategy)
	{
		LODStrategy->SetFarFieldActive(false);
	}
}

bool UVoxelChunkManager::DoesFarFieldDrawSea() const
{
	return FarField.IsValid() && FarField->GetSettings().bSea;
}

UProceduralMeshComponent* UVoxelChunkManager::GetFarFieldComponent(uint64 PackedPatchKey) const
{
	const TObjectPtr<UProceduralMeshComponent>* Found = FarFieldComponents.Find(PackedPatchKey);
	return Found ? Found->Get() : nullptr;
}

void UVoxelChunkManager::SetFarFieldEnabled(bool bEnabled)
{
	DestroyFarField();
	RebuildFarField(bEnabled);
	UE_LOG(LogVoxelStreaming, Log, TEXT("Far field %s"), FarField ? TEXT("on") : TEXT("off"));
}

bool UVoxelChunkManager::IsChunkDrawnForFarField(const FIntVector& ChunkCoord) const
{
	const FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State)
	{
		return false;
	}
	switch (State->State)
	{
	case EChunkState::Loaded:
	case EChunkState::PendingUnload:
		return true;
	case EChunkState::PendingMeshing:
	case EChunkState::Meshing:
		// Re-meshing (an LOD change, an edit): its old mesh is still drawn. A chunk loaded before is drawn as it was, also
		// with an empty mesh the renderer holds nothing for (VP4: such a chunk under a band patch uncovered the patch for its
		// re-mesh, and the patch's ancestor flashed over the terrain).
		return (CVarFarFieldAvoidStandIns.GetValueOnGameThread() != 0 && LoadedChunkCoords.Contains(ChunkCoord))
			|| (MeshRenderer && MeshRenderer->IsChunkLoaded(ChunkCoord));
	default:
		return false;
	}
}

void UVoxelChunkManager::UpdateFarFieldForViewer(const FVector& ViewerPosition, float FieldOfView)
{
	if (FarField)
	{
		UpdateFarField(ViewerPosition, ViewerPosition, FieldOfView);
	}
}

bool UVoxelChunkManager::UpdateFarFieldForCamera(const FVector& CameraPosition, float FieldOfView)
{
	if (!FarField || !Configuration || !bFarFieldHasLastCamera)
	{
		return false;
	}
	// Up to a chunk a frame the tick's selection holds (its hysteresis covers the motion); more is a cut, as the far field
	// itself tells one from motion (VP4's horizon rule).
	if (FVector::DistSquared(CameraPosition, FarFieldLastCamera) <= FMath::Square(FarField->GetSettings().ChunkWorldSize))
	{
		return false;
	}
	// The window has not moved yet (the next tick moves it): the hole stays where its chunks are. The frame's figures
	// cover both updates.
	const float TickUpdateMs = FarFieldStats.UpdateMs;
	const int32 TickPatchesBuilt = FarFieldStats.PatchesBuilt;
	UpdateFarField(CameraPosition, FarFieldLastViewer, FieldOfView);
	FarFieldStats.UpdateMs += TickUpdateMs;
	FarFieldStats.PatchesBuilt += TickPatchesBuilt;
	FarFieldStats.CameraUpdates = ++FarFieldCameraUpdates;
	return true;
}

void UVoxelChunkManager::TickAfterCameraUpdate()
{
	if (!bIsInitialized || !bStreamingEnabled || bBenchmarkViewActive)
	{
		return;
	}
	// The camera the tick read (BuildQueryContext), now updated for this frame.
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Camera = PC->PlayerCameraManager->GetCameraLocation();
	if (FarField && CVarFarFieldCameraUpdate.GetValueOnGameThread() != 0)
	{
		UpdateFarFieldForCamera(Camera, PC->PlayerCameraManager->GetFOVAngle());
	}
	if (CVarStreamCameraCutUpdate.GetValueOnGameThread() != 0)
	{
		UpdateStreamingForCamera(Camera);
	}
}

bool UVoxelChunkManager::UpdateStreamingForCamera(const FVector& CameraPosition)
{
	if (!Configuration || !LODStrategy)
	{
		return false;
	}
	// Up to a chunk a frame the tick's decisions hold; more is a cut (a teleport, a respawn, a switch of view target), whose
	// frame would otherwise stream for the camera of the frame before (world modes review WM-MP4).
	if (FVector::DistSquared(CameraPosition, CurrentCameraPosition) <= FMath::Square(Configuration->GetChunkWorldSize()))
	{
		return false;
	}
	FLODQueryContext Context = BuildQueryContext();
	if (FVector::DistSquared(Context.ViewerPosition, CurrentCameraPosition) <= FMath::Square(Configuration->GetChunkWorldSize()))
	{
		return false; // the context reads another viewer than the camera manager (the editor viewport, the owner)
	}
	// The camera is the streaming viewer for this pass: the prefetch's speed starts over after a jump (ApplyDescentPrefetch).
	CurrentCameraPosition = Context.ViewerPosition;
	CurrentViewerPosition = Context.ViewerPosition;
	StreamingViewerLead = 0.0;
	CameraRadialSpeed = 0.0;
	LastPrefetchCamera = Context.ViewerPosition;
	if (MeshRenderer)
	{
		MeshRenderer->SetViewerPosition(CurrentCameraPosition);
	}

	// The tick's streaming decisions again, for this camera: the strategy's view, the load and unload decisions, the queues'
	// order, and the first generation launches, so the chunks under the new position are on their way in this frame.
	LODStrategy->Update(Context, 0.0f);
	const FIntVector ViewerChunk = WorldToChunkCoord(Context.ViewerPosition);
	UpdateLoadDecisions(Context);
	CachedViewerChunk = ViewerChunk;
	LastStreamingUpdatePosition = Context.ViewerPosition;
	UpdateUnloadDecisions(Context);
	ReprioritizeQueues(Context);
	const float TimeSlice = Configuration->StreamingTimeSliceMS;
	ProcessGenerationQueue(TimeSlice * 0.4f);
	++CameraCutUpdates;
	UE_LOG(LogVoxelStreaming, Verbose, TEXT("Streaming updated for the frame's camera after a cut (WM-MP4): viewer chunk (%d, %d, %d), %d generations pending, %d in flight."),
		ViewerChunk.X, ViewerChunk.Y, ViewerChunk.Z, GenerationQueue.Num(), AsyncGenerationInProgress.Num());
	return true;
}

void UVoxelChunkManager::UpdateFarField(const FVector& CameraPosition, const FVector& ViewerPosition, float FieldOfView)
{
	using namespace VoxelFarFieldManagerPrivate;
	if (!FarField || !Configuration)
	{
		return;
	}
	const double StartTime = FPlatformTime::Seconds();
	const FVoxelFarFieldSettings& Settings = FarField->GetSettings();
	UWorld* World = GetWorld();
	FarFieldLastCamera = CameraPosition;
	FarFieldLastViewer = ViewerPosition;
	bFarFieldHasLastCamera = true;

	// The camera measures the error and the horizon; the streaming viewer (under it while it descends, WM-BP10) is the
	// voxel window's centre. Both lie on one up-line, so the hole's centre is the same ground point.
	FVoxelFarFieldView View;
	View.Camera = CameraPosition;
	View.Viewer = ViewerPosition;
	// Pixels per radian from the game viewport's width and the horizontal field of view.
	double ViewportWidth = 1920.0;
	if (const UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
	{
		FVector2D Size;
		Viewport->GetViewportSize(Size);
		if (Size.X > 1.0)
		{
			ViewportWidth = Size.X;
		}
	}
	const double FieldOfViewRadians = FMath::DegreesToRadians(FMath::Clamp(FieldOfView > 0.0f ? static_cast<double>(FieldOfView) : 90.0, 5.0, 170.0));
	View.PixelsPerRadian = ViewportWidth / (2.0 * FMath::Tan(FieldOfViewRadians * 0.5));
	View.MaxScreenError = FMath::Max(0.25f, CVarFarFieldError.GetValueOnGameThread());
	if (Settings.bHeightmap && CVarFarFieldHeightmapError.GetValueOnGameThread() > 0.0f)
	{
		View.MaxScreenError = FMath::Max(0.25f, CVarFarFieldHeightmapError.GetValueOnGameThread());
	}
	// The viewer's ground point, as the LOD strategy measures the window's horizons on it (GetPlanetGroundRadius: the
	// analytic terrain along the viewer's direction, no zones). One sample when the viewer moves. A heightmap world
	// measures its distances on the XY plane (WM-MP1).
	if (!Settings.bHeightmap && FVector::DistSquared(ViewerPosition, FarFieldGroundViewer) > 1.0)
	{
		const FVector Direction = (ViewerPosition - Settings.Center).GetSafeNormal();
		const FSphericalPlanetWorldMode* Planet = WorldMode.IsValid() && WorldMode->GetWorldModeType() == EWorldMode::SphericalPlanet
			? static_cast<const FSphericalPlanetWorldMode*>(WorldMode.Get()) : nullptr;
		FarFieldGroundRadius = Settings.Radius + (Planet && !Direction.IsNearlyZero()
			? static_cast<double>(Planet->GetTerrainAltitude(Direction, Configuration->NoiseParams)) : 0.0);
		FarFieldGroundViewer = ViewerPosition;
	}
	View.GroundRadius = FarFieldGroundRadius;
	// Within the unload distance of the ground point (plus a chunk's reach past its centre) the window may hold chunks;
	// band patches reaching past three quarters of the load distance are built while covered, ready for when their
	// chunks unload.
	const double HalfDiagonal = 0.5 * UE_DOUBLE_SQRT_3 * Settings.ChunkWorldSize;
	const double LoadDistance = LODStrategy ? LODStrategy->GetLoadDistance() : Configuration->ViewDistance;
	const double UnloadDistance = LODStrategy ? LODStrategy->GetUnloadDistance() : Configuration->ViewDistance * 1.2;
	// Above the voxel cut-over (WM-BP10) the window empties: once it holds no chunk there is no hole to cut, and the far
	// field refines by its error alone (the band's forced splits would build band patches for a window that is empty).
	const bool bNoWindow = IsVoxelWindowSuspended() && LoadedChunkCoords.Num() == 0;
	View.BandRadius = bNoWindow ? 0.0 : UnloadDistance + HalfDiagonal;
	View.PrebuildRadius = bNoWindow ? 0.0 : 0.75 * LoadDistance;
	View.Time = FPlatformTime::Seconds();
	// A new chunk fades in over the patch under it (the appearance fade, R24 P3): hide the patch once it has.
	static IConsoleVariable* FadeDuration = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.MeshFade.Duration"));
	View.HideDelay = (FadeDuration ? FMath::Max(0.0f, FadeDuration->GetFloat()) : 0.35f) + 0.1;
	View.ChunkStateVersion = ChunkStateVersion;
	View.bAvoidStandIns = CVarFarFieldAvoidStandIns.GetValueOnGameThread() != 0;
	View.bBuildAllRoots = CVarFarFieldBuildAllRoots.GetValueOnGameThread() != 0;

	FarField->SetMaxBuildsInFlight(CVarFarFieldMaxBuilds.GetValueOnGameThread());
	FVoxelFarFieldChanges Changes;
	FarField->Update(View, [this](const FIntVector& Coord) { return IsChunkDrawnForFarField(Coord); }, Changes);

	// Apply: components go with their patches, are (re)filled when a patch is built, and show what the selection drew.
	AActor* Owner = GetOwner();
	UMaterialInterface* TerrainMaterial = MeshRenderer ? MeshRenderer->GetSurfaceMaterial() : nullptr;
	UMaterialInterface* WaterMaterial = Configuration->WaterMeshMaterial.Get();
	const bool bUnderSea = Settings.bSea && FarField->GetFrame().IsUnderSea(CameraPosition);
	for (const uint64 Key : Changes.Removed)
	{
		if (TObjectPtr<UProceduralMeshComponent>* Found = FarFieldComponents.Find(Key))
		{
			if (IsValid(*Found))
			{
				(*Found)->DestroyComponent();
			}
			FarFieldComponents.Remove(Key);
		}
		if (const FIntPoint* Size = FarFieldComponentSizes.Find(Key))
		{
			FarFieldComponentVertices -= Size->X;
			FarFieldComponentIndices -= Size->Y;
			FarFieldComponentSizes.Remove(Key);
		}
	}
	for (const uint64 Key : Changes.Built)
	{
		const FVoxelPlanetFarField::FPatchState* Patch = FarField->FindPatch(Key);
		if (!Patch || !Patch->Mesh.IsValid() || !Owner)
		{
			continue;
		}
		const FVoxelFarFieldPatchMesh& Mesh = *Patch->Mesh;
		TObjectPtr<UProceduralMeshComponent>& Component = FarFieldComponents.FindOrAdd(Key);
		if (!IsValid(Component))
		{
			Component = MakePatchComponent(Owner, Mesh.Origin);
		}
		else
		{
			Component->ClearAllMeshSections();
			Component->SetWorldLocation(Mesh.Origin);
		}
		SetPatchSection(Component, 0, Mesh.Terrain, false, TerrainMaterial);
		// The sea's underside only near the voxel window: seen from under the water, the surface is only visible close by.
		const bool bUnderside = Settings.bSea && !Mesh.Sea.IsEmpty() && Patch->Key.Level >= Settings.BandLevel - FarFieldUndersideLevels;
		if (Settings.bSea && !Mesh.Sea.IsEmpty())
		{
			SetPatchSection(Component, 1, Mesh.Sea, false, WaterMaterial);
			if (bUnderside)
			{
				SetPatchSection(Component, 2, Mesh.Sea, true, WaterMaterial);
			}
			Component->SetTranslucentSortPriority(0);
		}
		const int32 SeaCopies = Settings.bSea ? (bUnderside ? 2 : 1) : 0;
		const FIntPoint Size(Mesh.Terrain.Positions.Num() + SeaCopies * Mesh.Sea.Positions.Num(),
			Mesh.Terrain.Triangles.Num() + SeaCopies * Mesh.Sea.Triangles.Num());
		if (const FIntPoint* Old = FarFieldComponentSizes.Find(Key))
		{
			FarFieldComponentVertices -= Old->X;
			FarFieldComponentIndices -= Old->Y;
		}
		FarFieldComponentSizes.Add(Key, Size);
		FarFieldComponentVertices += Size.X;
		FarFieldComponentIndices += Size.Y;
		// The component keeps its own copy.
		FarField->ReleaseMesh(Key);
	}
	auto ApplyVisibility = [this, bUnderSea](uint64 Key)
	{
		const FVoxelPlanetFarField::FPatchState* Patch = FarField->FindPatch(Key);
		const TObjectPtr<UProceduralMeshComponent>* Found = FarFieldComponents.Find(Key);
		if (!Patch || !Found || !IsValid(*Found))
		{
			return;
		}
		UProceduralMeshComponent* Component = *Found;
		const int32 Sections = Component->GetNumSections();
		if (Sections > 0)
		{
			Component->SetMeshSectionVisible(0, Patch->bDrawTerrain);
		}
		if (Sections > 1)
		{
			Component->SetMeshSectionVisible(1, Patch->bDrawSea);
		}
		if (Sections > 2)
		{
			Component->SetMeshSectionVisible(2, Patch->bDrawSea && bUnderSea);
		}
	};
	for (const uint64 Key : Changes.Visibility)
	{
		ApplyVisibility(Key);
	}
	// The sea's undersides show only while the viewer is inside the sea radius (as the ocean shell's).
	UpdateUnderside(bUnderSea, bFarFieldUndersideShown, [&](bool)
	{
		for (const TPair<uint64, TObjectPtr<UProceduralMeshComponent>>& Pair : FarFieldComponents)
		{
			ApplyVisibility(Pair.Key);
		}
	});

	// Figures for the bench and vox.Planet.
	FVoxelFarFieldStats& Stats = FarFieldStats;
	Stats.bActive = true;
	Stats.bDrawsSea = Settings.bSea;
	Stats.Patches = FarField->GetPatchCount();
	Stats.DrawnPatches = FarField->GetDrawnPatchCount();
	Stats.DrawnTerrain = 0;
	Stats.DrawnSea = 0;
	Stats.HiddenBand = 0;
	FarField->ForEachPatch([&Stats, &Settings](const FVoxelPlanetFarField::FPatchState& Patch)
	{
		Stats.DrawnTerrain += Patch.bDrawTerrain ? 1 : 0;
		Stats.DrawnSea += Patch.bDrawSea ? 1 : 0;
		Stats.HiddenBand += (Patch.Key.Level >= Settings.BandLevel && !Patch.bDrawTerrain && Patch.bCovered) ? 1 : 0;
	});
	Stats.PatchesBuilt = FarField->ConsumeBuiltCount();
	Stats.BuildsPending = FarField->GetBuildsPending();
	Stats.TotalBuilt = FarField->GetTotalBuilt();
	Stats.BuildMsMean = Stats.TotalBuilt > 0 ? static_cast<float>(FarField->GetTotalBuildMs() / Stats.TotalBuilt) : 0.0f;
	Stats.Components = FarFieldComponents.Num();
	Stats.MemoryMB = static_cast<float>((FarField->GetMeshMemoryBytes() + EstimateBytes(FarFieldComponentVertices, FarFieldComponentIndices))
		/ (1024.0 * 1024.0));
	Stats.StandIns = FarField->GetStandInCount();
	Stats.StandInMinLevel = FarField->GetStandInMinLevel();
	Stats.TotalStandIns = FarField->GetTotalStandIns();
	Stats.StandInUpdates = FarField->GetStandInUpdates();
	Stats.CameraUpdates = FarFieldCameraUpdates;
	Stats.UpdateMs = static_cast<float>((FPlatformTime::Seconds() - StartTime) * 1000.0);

	// VP4: what stood in over the voxel window this update, and why.
	if (CVarFarFieldTrace.GetValueOnGameThread() != 0 && Stats.StandIns > 0)
	{
		FIntVector Undrawn;
		FString UndrawnText = TEXT("none");
		if (FarField->GetUndrawnChunk(Undrawn))
		{
			const FVoxelChunkState* State = ChunkStates.Find(Undrawn);
			UndrawnText = FString::Printf(TEXT("%s state %d, renderer %d, loaded before %d"), *Undrawn.ToString(),
				State ? static_cast<int32>(State->State) : -1, MeshRenderer && MeshRenderer->IsChunkLoaded(Undrawn) ? 1 : 0,
				LoadedChunkCoords.Contains(Undrawn) ? 1 : 0);
		}
		UE_LOG(LogVoxelStreaming, Log, TEXT("voxel.World.FarFieldTrace: frame %llu (dumping %d): %d stand-in(s) over the window, coarsest level %d (band %d); band patches not ready: %d covered, %d uncovered; a chunk not drawn under one: %s"),
			static_cast<uint64>(GFrameCounter), GIsDumpingMovie, Stats.StandIns, Stats.StandInMinLevel, Settings.BandLevel,
			FarField->GetUnreadyBandCount(true), FarField->GetUnreadyBandCount(false), *UndrawnText);
	}
}

FString UVoxelChunkManager::GetFarFieldDebugString() const
{
	if (!FarField)
	{
		return TEXT("no far field (a cubic planet or no world mode, voxel.World.FarField 0, or not a game world)");
	}
	const FVoxelFarFieldStats& Stats = FarFieldStats;
	const FVoxelFarFieldSettings& Settings = FarField->GetSettings();
	int32 RootsBuilt = 0;
	const int32 Roots = FarField->GetRoots().Num();
	for (const uint64 Key : FarField->GetRoots())
	{
		const FVoxelPlanetFarField::FPatchState* Root = FarField->FindPatch(Key);
		RootsBuilt += Root && Root->bResident ? 1 : 0;
	}
	return FString::Printf(TEXT("%spatches=%d drawn=%d (terrain %d, sea %d) hidden by the voxel window=%d components=%d builds pending=%d built=%lld (%.2f ms each) update %.2f ms, %.1f MB; band level %d (%.0f uu), sink %.0f, sea %s; stand-ins over the window %lld in %lld updates (voxel.World.FarFieldAvoidStandIns %d); roots built %d of %d (voxel.World.FarFieldBuildAllRoots %d); updates for the frame's camera after a cut %lld (voxel.World.FarFieldCameraUpdate %d)"),
		Settings.bHeightmap ? *FString::Printf(TEXT("heightmap (root tiles of %d chunks, out to %.0f uu) "), Settings.RootChunks, Settings.FarDistance) : TEXT(""),
		Stats.Patches, Stats.DrawnPatches, Stats.DrawnTerrain, Stats.DrawnSea, Stats.HiddenBand, Stats.Components, Stats.BuildsPending,
		Stats.TotalBuilt, Stats.BuildMsMean, Stats.UpdateMs, Stats.MemoryMB, Settings.BandLevel, Settings.GetPatchArc(Settings.BandLevel),
		Settings.Sink, Settings.bSea ? TEXT("far field") : (Settings.bHeightmap ? TEXT("tiles / far ocean / none") : TEXT("ocean shell / none")),
		Stats.TotalStandIns, Stats.StandInUpdates,
		CVarFarFieldAvoidStandIns.GetValueOnGameThread(), RootsBuilt, Roots, CVarFarFieldBuildAllRoots.GetValueOnGameThread(),
		FarFieldCameraUpdates, CVarFarFieldCameraUpdate.GetValueOnGameThread());
}

FString UVoxelChunkManager::GetFarOceanDebugString() const
{
	if (!FarOceanStatic)
	{
		return TEXT("no far ocean (not an island, bIslandFarOcean off, or not a game world)");
	}
	FIntPoint GridMin, GridMax;
	FVoxelFarOcean::GetGridColumns(FarOceanSettings, GridMin, GridMax);
	return FString::Printf(TEXT("visible=%d box %dx%d column(s) water=%s seabed material=%d reaches %.0f uu past the edge"),
		FarOceanStatic->IsVisible() ? 1 : 0, GridMax.X - GridMin.X + 1, GridMax.Y - GridMin.Y + 1,
		FarOceanStatic->GetNumSections() > 1 ? TEXT("yes") : TEXT("no"), FarOceanSettings.SeabedMaterialID, FarOceanSettings.OuterDistance);
}

void UVoxelChunkManager::AddConditioningZone(const FVoxelConditioningZone& Zone)
{
	ConditioningZones.Add(Zone);
	if (FarField)
	{
		// The far field's patches narrow enough to take zones rebuild with it (their old meshes draw until then).
		FarField->InvalidateZone(Zone);
	}
	if (Zone.IsRadial())
	{
		UE_LOG(LogVoxelStreaming, Log,
			TEXT("Added planet conditioning zone toward %s inner=%.0f falloff=%.0f target altitude=%.0f strength=%.2f; %d total."),
			*Zone.Direction.ToCompactString(), Zone.InnerRadius, Zone.FalloffWidth, Zone.TargetHeight, Zone.Strength, ConditioningZones.Num());
	}
	else
	{
		UE_LOG(LogVoxelStreaming, Log,
			TEXT("Added terrain conditioning zone at (%.0f,%.0f) inner=%.0f falloff=%.0f target=%.0f strength=%.2f; %d total."),
			Zone.Center.X, Zone.Center.Y, Zone.InnerRadius, Zone.FalloffWidth, Zone.TargetHeight, Zone.Strength, ConditioningZones.Num());
	}
	// A zone of the other kind is kept but never applies (each kind's gather and blend skip the other).
	const bool bPlanet = Configuration && Configuration->WorldMode == EWorldMode::SphericalPlanet;
	if (Configuration && Zone.IsRadial() != bPlanet)
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("AddConditioningZone: a %s zone in a %s world has no effect (planet zones need a Direction, FVoxelConditioningZone::MakeRadial)."),
			Zone.IsRadial() ? TEXT("planet") : TEXT("heightmap"), bPlanet ? TEXT("planet") : TEXT("heightmap"));
	}
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

	// A planet (spherical SP-B10): the cap the chunk's box subtends from the centre.
	if (Configuration->WorldMode == EWorldMode::SphericalPlanet)
	{
		FVector CapDirection;
		double CapAngle = 0.0;
		if (GetPlanetCapOfBox(FBox(ChunkOrigin, ChunkOrigin + FVector(ChunkWorldSize)), CapDirection, CapAngle))
		{
			GatherConditioningZonesOnSphere(CapDirection, CapAngle, OutZones);
		}
		return;
	}

	const FBox2D ChunkRegion(
		FVector2D(ChunkOrigin.X, ChunkOrigin.Y),
		FVector2D(ChunkOrigin.X + ChunkWorldSize, ChunkOrigin.Y + ChunkWorldSize));

	GatherConditioningZonesForRegion(ChunkRegion, OutZones);
}

bool UVoxelChunkManager::GetPlanetCapOfBox(const FBox& Box, FVector& OutDirection, double& OutAngle) const
{
	if (!Configuration || Configuration->WorldMode != EWorldMode::SphericalPlanet)
	{
		return false;
	}
	const FVector Center = FVoxelWorldModeFactory::MakePlanetParams(*Configuration).PlanetCenter;
	const FVector Offset = Box.GetCenter() - Center;
	const double Distance = Offset.Size();
	const double HalfDiagonal = Box.GetExtent().Size();
	OutDirection = Distance > UE_KINDA_SMALL_NUMBER ? Offset / Distance : FVector::UpVector;
	OutAngle = Distance <= HalfDiagonal ? UE_DOUBLE_PI : FMath::Asin(HalfDiagonal / Distance);
	return true;
}

void UVoxelChunkManager::GatherConditioningZones(const FBox& Box, TArray<FVoxelConditioningZone>& OutZones) const
{
	// The zones a world box's terrain takes, in any mode (code review CR2, CM-20): on a planet the cap the box subtends
	// (GetPlanetCapOfBox), on a heightmap world its XY region.
	FVector CapDirection;
	double CapAngle = 0.0;
	if (GetPlanetCapOfBox(Box, CapDirection, CapAngle))
	{
		GatherConditioningZonesOnSphere(CapDirection, CapAngle, OutZones);
		return;
	}
	GatherConditioningZonesForRegion(FBox2D(FVector2D(Box.Min.X, Box.Min.Y), FVector2D(Box.Max.X, Box.Max.Y)), OutZones);
}

void UVoxelChunkManager::GatherConditioningZonesOnSphere(const FVector& Direction, double AngularRadius, TArray<FVoxelConditioningZone>& OutZones) const
{
	if ((ConditioningZones.Num() == 0 && TerrainConditioner == nullptr) || !Configuration
		|| Configuration->WorldMode != EWorldMode::SphericalPlanet)
	{
		return;
	}
	const double Radius = FVoxelWorldModeFactory::MakePlanetParams(*Configuration).PlanetRadius;
	for (const FVoxelConditioningZone& Zone : ConditioningZones)
	{
		if (Zone.MeetsCap(Direction, AngularRadius, Radius))
		{
			OutZones.Add(Zone);
		}
	}
	if (TerrainConditioner)
	{
		TerrainConditioner->GatherConditioningOnSphere(Direction, AngularRadius, OutZones);
	}
}

void UVoxelChunkManager::GatherConditioningZonesForRegion(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const
{
	if (ConditioningZones.Num() == 0 && TerrainConditioner == nullptr)
	{
		return;
	}

	// Static zones whose influence overlaps the region (heightmap zones: planet zones apply only on a planet).
	for (const FVoxelConditioningZone& Zone : ConditioningZones)
	{
		if (!Zone.IsRadial() && Zone.GetBounds2D().Intersect(Region))
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

	// A block world (WM-C5, KC6): the top of the topmost block the generator makes in the block column containing (X, Y),
	// from the height sampled where the generator samples it, the column's lower corner.
	if (FVoxelBlockSurface::UsesBlockSurface(*Configuration))
	{
		const FVector& Origin = Configuration->WorldOrigin;
		const float VoxelSize = Configuration->VoxelSize;
		const float ColumnHeight = GetGeneratedColumnHeight(
			FVoxelBlockSurface::ColumnCorner(WorldX, Origin.X, VoxelSize), FVoxelBlockSurface::ColumnCorner(WorldY, Origin.Y, VoxelSize));
		return static_cast<float>(FVoxelBlockSurface::BlockTopForHeight(ColumnHeight, Origin.Z, VoxelSize));
	}
	return GetGeneratedColumnHeight(WorldX, WorldY);
}

float UVoxelChunkManager::GetGeneratedColumnHeight(double WorldX, double WorldY) const
{
	const IVoxelWorldMode* WM = GetWorldMode();
	if (!bIsInitialized || !WM || !Configuration)
	{
		return 0.0f;
	}

	// The generated rung (FVoxelSurfaceQuery::GetGeneratedSurface): base terrain + continentalness (the world mode carries
	// the biome context set at init) with the terrain conditioning zones there (POI / claim flatten) layered on exactly as
	// generation does.
	TArray<FVoxelConditioningZone> Zones;
	if (ConditioningZones.Num() > 0 || TerrainConditioner != nullptr)
	{
		GatherConditioningZonesForPoint(WorldX, WorldY, Zones);
	}
	return static_cast<float>(FVoxelSurfaceQuery::GetGeneratedSurface(*WM, FVector(WorldX, WorldY, 0.0), Configuration->NoiseParams,
		Zones.Num() > 0 ? &Zones : nullptr).Position.Z);
}

bool UVoxelChunkManager::QuerySurfacePoint(const FVector& WorldPos, EVoxelSurfaceLayer Layer, FVoxelSurfaceSample& OutSample) const
{
	const IVoxelWorldMode* WM = GetWorldMode();
	if (!bIsInitialized || !Configuration || !WM)
	{
		return false;
	}
	if (Layer == EVoxelSurfaceLayer::EditMerged)
	{
		return QueryEditMergedSurfacePoint(WorldPos, OutSample);
	}

	// The generated rung's zones, gathered for the place as GetGeneratedSurfacePoint gathers them.
	TArray<FVoxelConditioningZone> Zones;
	if (Layer == EVoxelSurfaceLayer::Generated && (ConditioningZones.Num() > 0 || TerrainConditioner != nullptr))
	{
		if (WM->GetWorldModeType() == EWorldMode::SphericalPlanet)
		{
			const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*WM);
			GatherConditioningZonesOnSphere(FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, Planet.GetPlanetParams().PlanetCenter), 0.0, Zones);
		}
		else
		{
			GatherConditioningZonesForPoint(WorldPos.X, WorldPos.Y, Zones);
		}
	}
	const FVoxelBiomeSnapshot Snapshot = FVoxelBiomeSnapshot::FromConfig(Configuration->BiomeConfiguration);
	OutSample = FVoxelSurfaceQuery::SampleSurfacePoint(*WM, WorldPos, Configuration->VoxelSize, Configuration->NoiseParams, Snapshot,
		GetSeaSurface(), Zones.Num() > 0 ? &Zones : nullptr);
	if (Layer == EVoxelSurfaceLayer::Generated && IsBlockSurface())
	{
		// A block world's generated surface is the block top GetGeneratedSurfaceHeight gives (sampled at the column's corner).
		OutSample.Position.Z = GetGeneratedSurfaceHeight(WorldPos.X, WorldPos.Y);
		OutSample.Altitude = WM->GetAltitude(OutSample.Position);
	}
	return true;
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
				// Uniform (G67): the scan stops at the first voxel that differs, so it costs a surface chunk nothing.
				Result.bUniform = FChunkDescriptor::ComputeUniformValue(VoxelData, Result.UniformValue);
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
				// A uniform chunk is collapsed now rather than after far compression's idle wait (G67): its readers take
				// the shared uniform snapshot (G69) and point queries its value, so nothing expands it again.
				if (Result.bUniform && CVarFarCompressionUniformAtGeneration.GetValueOnGameThread() != 0
					&& State->Descriptor.CollapseToUniform(Result.UniformValue))
				{
					++UniformAtGenerationCount;
				}
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
	// The readbacks that landed this tick, handed on after the loop (G67: their uniformity scans run together).
	struct FReadyReadback
	{
		FIntVector Coord;
		FVoxelNoiseGenerationRequest GenRequest;
		TArray<FVoxelData> VoxelData;
		uint32 DispatchSerial = 0;
		bool bUniform = false;
		FVoxelData UniformValue;
	};
	TArray<FReadyReadback> Ready;
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
			FReadyReadback& Landed = Ready.AddDefaulted_GetRef();
			Landed.Coord = ChunkCoord;
			Landed.GenRequest = MoveTemp(Pending.GenRequest);
			Landed.VoxelData = MoveTemp(VoxelData);
			Landed.DispatchSerial = Pending.DispatchSerial;
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

	// Uniform (G67): scanned here, as the readbacks land, so each result is stored this tick and frees its generation slot (a
	// worker hop held the slot for another tick and slowed generation). The chunks that landed together are scanned across
	// workers, so the game thread waits for about the slowest one, not their sum: a fully uniform chunk reads its whole
	// megabyte, a chunk with any surface stops at its first block that differs. The tree injector scans after injecting.
	if (Ready.Num() > 0 && CVarFarCompressionUniformAtGeneration.GetValueOnGameThread() != 0 && !WillInjectTreesAfterReadback())
	{
		ParallelFor(Ready.Num(), [&Ready](int32 Index)
		{
			FReadyReadback& Landed = Ready[Index];
			Landed.bUniform = FChunkDescriptor::ComputeUniformValue(Landed.VoxelData, Landed.UniformValue);
		});
	}
	for (FReadyReadback& Landed : Ready)
	{
		LaunchPostReadbackProcessing(Landed.Coord, MoveTemp(Landed.GenRequest), MoveTemp(Landed.VoxelData), Landed.DispatchSerial,
			Landed.bUniform, Landed.UniformValue);
	}
}

bool UVoxelChunkManager::WillInjectTreesAfterReadback() const
{
	// Voxel trees are a heightmap feature (cubic, which planets do not use: spherical SD3, SP-B9).
	return Configuration && WorldMode.IsValid() &&
		Configuration->WorldMode != EWorldMode::SphericalPlanet &&
		Configuration->MeshingMode == EMeshingMode::Cubic &&
		Configuration->TreeMode != EVoxelTreeMode::HISM &&
		Configuration->TreeTemplates.Num() > 0 &&
		Configuration->TreeDensity > 0.0f;
}

void UVoxelChunkManager::LaunchPostReadbackProcessing(const FIntVector& ChunkCoord, FVoxelNoiseGenerationRequest GenRequest, TArray<FVoxelData> VoxelData, uint32 DispatchSerial,
	bool bUniform, const FVoxelData& UniformValue)
{
	// The water/underground post-passes already ran on the GPU (AddVoxelPostPassDispatches), so the
	// readback data is finished except for cubic-mode voxel-tree injection. Without trees there is
	// nothing left to compute — enqueue directly and skip the worker hop. Never on a planet (SP-B9).
	if (!WillInjectTreesAfterReadback())
	{
		FAsyncGenerationResult Result;
		Result.ChunkCoord = ChunkCoord;
		Result.bSuccess = true;
		// Uniform (G67): scanned as the readbacks landed (ProcessPendingGPUReadbacks).
		Result.bUniform = bUniform;
		Result.UniformValue = UniformValue;
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
			Result.bUniform = FChunkDescriptor::ComputeUniformValue(VoxelData, Result.UniformValue);
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
		if (IsChunkBeyondUnloadDistance(Request.ChunkCoord))
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

		// Mark as meshing (the request reads the edit layer below, so it has every edit made so far: G57)
		SetChunkState(Request.ChunkCoord, EChunkState::Meshing);
		State->bRemeshAfterMeshing = false;

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
		// A far-compressed chunk without edits meshes from its snapshot (decoded on a worker, usually prefetched when the
		// chunk was queued), copied into the request on the mesh worker (G65); edits merge here, so a chunk with edits
		// (rarely compressed) reads its array as before.
		TSharedPtr<const TArray<FVoxelData>> DeferredVoxels;
		UE::Tasks::FTask DeferredDecode;
		if (CVarFarCompressionSnapshotReads.GetValueOnGameThread() != 0 && State->Descriptor.IsCompact()
			&& !(EditManager && EditManager->ChunkHasEdits(Request.ChunkCoord)))
		{
			const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("chunk mesh"));
			DeferredVoxels = GetSeamVoxelSnapshot(Request.ChunkCoord, *State, &DeferredDecode);
		}
		else
		{
			const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("chunk mesh"));
			MeshRequest.VoxelData = State->Descriptor.GetVoxelDataForRead();
		}

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
		LaunchAsyncMeshGeneration(Request, MoveTemp(MeshRequest), MoveTemp(DeferredVoxels), MoveTemp(DeferredDecode));
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

void UVoxelChunkManager::LaunchAsyncMeshGeneration(const FChunkLODRequest& Request, FVoxelMeshingRequest MeshRequest,
	TSharedPtr<const TArray<FVoxelData>> DeferredVoxels, UE::Tasks::FTask DeferredDecode)
{
	// Mark as in-progress (stamped: the serial travels with the result, see FAsyncJobStamp)
	const uint32 DispatchSerial = StampAsyncDispatch(AsyncMeshingInProgress, Request.ChunkCoord);
	++MeshWorkStats.ChunkMeshJobs;

	// Capture mesher pointer (it's a TUniquePtr, so we need raw pointer for lambda)
	IVoxelMesher* MesherPtr = Mesher.Get();
	const FIntVector ChunkCoord = Request.ChunkCoord;
	const int32 LODLevel = Request.LODLevel;

	// Use a weak pointer to safely check if ChunkManager is still valid
	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);

	// Dispatch to a thread-pool worker
	Async(EAsyncExecution::ThreadPool, [MesherGuard = FMesherWorkerGuard(MesherWorkerJobs), WeakThis, MesherPtr, MeshRequest = MoveTemp(MeshRequest), ChunkCoord, LODLevel, DispatchSerial,
		DeferredVoxels = MoveTemp(DeferredVoxels), DeferredDecode = MoveTemp(DeferredDecode)]() mutable
	{
		// Generate mesh on background thread
		FChunkMeshData MeshData;
		const double WorkerStart = FPlatformTime::Seconds();
		TakeDeferredMeshVoxels(DeferredVoxels, DeferredDecode, MeshRequest.VoxelData);
		const bool bSuccess = MesherPtr->GenerateMeshCPU(MeshRequest, MeshData);
		const double WorkerMs = (FPlatformTime::Seconds() - WorkerStart) * 1000.0;

		// Queue result for game thread (thread-safe MPSC queue)
		if (UVoxelChunkManager* This = WeakThis.Get())
		{
			FAsyncMeshResult Result;
			Result.ChunkCoord = ChunkCoord;
			Result.LODLevel = LODLevel;
			Result.bSuccess = bSuccess;
			Result.DispatchSerial = DispatchSerial;
			Result.WorkerMs = WorkerMs;
			if (bSuccess)
			{
				Result.MeshData = MoveTemp(MeshData);
			}
			This->CompletedMeshQueue.Enqueue(MoveTemp(Result));
		}
	});
}

void UVoxelChunkManager::ProcessCompletedAsyncMeshes()
{
	const int32 MaxProcessPerFrame = 8; // Limit how many we process to spread render uploads

	// Bounded drain: results beyond this tick's budget stay queued. The old
	// `while (Dequeue(Result) && Count < Max)` popped the 9th result and dropped it, stranding its
	// chunk in Meshing for the session (R23b; the demo allows 16 async meshes in flight).
	VoxelDrainBounded(CompletedMeshQueue, MaxProcessPerFrame, [this](FAsyncMeshResult& Result) -> bool
	{
		// The worker time counts a superseded result too: the work was done.
		MeshWorkStats.ChunkMeshWorkerMs += Result.WorkerMs;
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
	// Above a planet's voxel cut-over (WM-BP10) every chunk is: the window holds nothing.
	if (LODStrategy->IsWindowSuspended())
	{
		return true;
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
			// (BenchEverMeshed drops it in RemoveChunkState.)
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
		SnapshotCounters.RemovedOnUnload += SeamSnapshotCache.Remove(ChunkCoord);
		ShellCache.Remove(ChunkCoord);
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

FVoxelResidencySnapshot UVoxelChunkManager::GetResidencySnapshot() const
{
	FVoxelResidencySnapshot Out;
	Out.Viewer = CurrentViewerPosition;
	Out.Camera = CurrentCameraPosition;
	Out.StreamingLead = StreamingViewerLead;
	if (LODStrategy)
	{
		Out.bWindowSuspended = LODStrategy->IsWindowSuspended();
		Out.CutoverAltitude = LODStrategy->GetCutoverAltitude();
		Out.CutoverResumeAltitude = Out.CutoverAltitude * FDistanceBandLODStrategy::CutoverResumeFraction;
		Out.ViewerSurfaceAltitude = LODStrategy->GetViewerSurfaceAltitude();
	}
	if (!Configuration || !WorldMode)
	{
		return Out;
	}
	const bool bPlanet = Configuration->WorldMode == EWorldMode::SphericalPlanet;
	Out.bRadialLayers = bPlanet;
	const double ChunkWorld = Configuration->GetChunkWorldSize();
	const FVector Centre = Configuration->WorldOrigin;
	const double Radius = Configuration->WorldRadius;
	auto AltitudeOf = [&](const FVector& P) { return bPlanet ? FVector::Dist(P, Centre) - Radius : P.Z - Centre.Z; };
	Out.ViewerAltitude = AltitudeOf(Out.Viewer);
	// From the viewer's ground point as the strategy's horizons are measured (a planet: along the ground from the terrain
	// under the viewer, WM-BP8; heightmap worlds: 2D distances from its XY).
	auto GroundDistanceOf = [&](const FVector& ChunkCentre) -> double
	{
		if (LODStrategy)
		{
			return LODStrategy->GetHorizonDistance(ChunkCentre, Out.Viewer);
		}
		return bPlanet ? FVector::Dist(ChunkCentre, Centre + (Out.Viewer - Centre).GetSafeNormal() * Radius) : FVector::Dist2D(ChunkCentre, Out.Viewer);
	};

	bool bFirst = true;
	for (const FIntVector& Coord : LoadedChunkCoords)
	{
		++Out.Tracked;
		if (const FVoxelChunkState* State = ChunkStates.Find(Coord); State && State->State == EChunkState::Loaded)
		{
			++Out.Ready;
			const int32 LOD = FMath::Max(0, State->LODLevel);
			if (Out.ByLOD.Num() <= LOD)
			{
				Out.ByLOD.SetNumZeroed(LOD + 1);
			}
			++Out.ByLOD[LOD];
		}
		const FVector ChunkCentre = WorldMode->ChunkCoordToWorld(Coord, Configuration->ChunkSize, Configuration->VoxelSize, 0)
			+ FVector(0.5 * ChunkWorld);
		const double Altitude = AltitudeOf(ChunkCentre);
		++Out.ByLayer.FindOrAdd(bPlanet ? FMath::FloorToInt32(Altitude / ChunkWorld) : Coord.Z);
		Out.MaxDistance = FMath::Max(Out.MaxDistance, FVector::Dist(ChunkCentre, Out.Viewer));
		Out.MaxGroundDistance = FMath::Max(Out.MaxGroundDistance, GroundDistanceOf(ChunkCentre));
		Out.MinChunkAltitude = bFirst ? Altitude : FMath::Min(Out.MinChunkAltitude, Altitude);
		Out.MaxChunkAltitude = bFirst ? Altitude : FMath::Max(Out.MaxChunkAltitude, Altitude);
		bFirst = false;
	}
	return Out;
}

void UVoxelChunkManager::GetBenchLODBacklog(int32& OutStale, int32& OutStaleNear) const
{
	OutStale = 0;
	OutStaleNear = 0;
	if (!LODStrategy || (Configuration && !Configuration->IsLODEnabled()))
	{
		return;
	}

	const FLODQueryContext Context = BuildStreamingContext();
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
		++ChunkStateVersion;
	}
}

void UVoxelChunkManager::RemoveChunkState(const FIntVector& ChunkCoord)
{
	ChunkStates.Remove(ChunkCoord);
	++ChunkStateVersion;
	// Its next mesh, after a reload, is a first mesh again (G14).
	BenchEverMeshed.Remove(ChunkCoord);
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
		const uint64 MaxAge = static_cast<uint64>(FMath::Max(1, CVarSnapshotMaxAgeTicks.GetValueOnGameThread()));
		for (auto It = ShellCache.CreateIterator(); It; ++It)
		{
			if (SeamTickCounter - It->Value.LastUsedTick > MaxAge)
			{
				It.RemoveCurrent();
			}
		}
		for (auto It = RecentlyEvictedSnapshots.CreateIterator(); It; ++It)
		{
			if (SeamTickCounter - It->Value > 600)
			{
				It.RemoveCurrent();
			}
		}
		for (auto It = SeamSnapshotCache.CreateIterator(); It; ++It)
		{
			if (SeamTickCounter - It->Value.LastUsedTick > MaxAge)
			{
				RecentlyEvictedSnapshots.Add(It->Key, SeamTickCounter);
				++SnapshotCounters.EvictedByAge;
				SnapshotCounters.EvictedByAgeUnread += (It->Value.Hits == 0) ? 1 : 0;
				SnapshotCounters.EvictedAgeTicksSum += static_cast<int64>(SeamTickCounter - It->Value.CreatedTick);
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
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("seam job"));
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
	// Participant snapshots: a far-compressed participant's is decoded by a worker task, which the job waits for on its
	// own worker (G65).
	TArray<UE::Tasks::FTask> SnapshotDecodes;
	auto TakeSnapshot = [this, &Job, &States, &SnapshotDecodes](int32 Participant)
	{
		UE::Tasks::FTask Decode;
		TSharedPtr<const TArray<FVoxelData>> Snapshot = GetSeamVoxelSnapshot(Job.Participants[Participant].Coord, *States[Participant], &Decode);
		if (Decode.IsValid())
		{
			SnapshotDecodes.Add(MoveTemp(Decode));
		}
		return Snapshot;
	};
	const int32 SharedLOD = States[0]->MeshedLODLevel;

	// Worker dispatch mirrors the chunk-mesh CPU path: raw mesher pointer + weak manager for the
	// result enqueue. The seam meshers read only the request + Config (thread-safe). Voxel data
	// is attached as SHARED snapshots (built at most once per chunk content version) — dispatch
	// itself no longer copies voxel arrays. Seam jobs ALWAYS run on the CPU via the dedicated
	// SeamMesher (mode-matched: CPU DC, CPU MC or CPU cubic) — a GPU main mesher cannot serve them.
	IVoxelMesher* SeamMesherPtr = EnsureSeamMesher();
	TWeakObjectPtr<UVoxelChunkManager> WeakThis(this);
	const FVoxelSeamKey Key = Job.Key;
	++MeshWorkStats.SeamJobs;
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
		SeamRequest.VoxelDataA = TakeSnapshot(0);
		SeamRequest.VoxelDataB = TakeSnapshot(1);

		const int32 FaceLODLevel = FMath::Min(States[0]->MeshedLODLevel, States[1]->MeshedLODLevel);
		SeamJobsInFlight.Add(Job.Key);
		SeamInFlightPeak = FMath::Max(SeamInFlightPeak, SeamJobsInFlight.Num());
		Async(EAsyncExecution::ThreadPool, [MesherGuard = FMesherWorkerGuard(MesherWorkerJobs), WeakThis, SeamMesherPtr, SeamRequest = MoveTemp(SeamRequest), Key, LODLevel = FaceLODLevel, DirtiedAt = Job.DirtiedAtSeconds, ReadyAt = Job.ReadyAtSeconds, ScheduledAt = Job.ScheduledAtSeconds, DispatchedAt, OwnerEpoch = Job.OwnerResidencyEpoch, SnapshotDecodes]() mutable
		{
			WaitForSnapshotDecodes(SnapshotDecodes);
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
			SeamRequest.VoxelData[i] = TakeSnapshot(i);
		}

		SeamJobsInFlight.Add(Job.Key);
		SeamInFlightPeak = FMath::Max(SeamInFlightPeak, SeamJobsInFlight.Num());
		Async(EAsyncExecution::ThreadPool, [MesherGuard = FMesherWorkerGuard(MesherWorkerJobs), WeakThis, SeamMesherPtr, SeamRequest = MoveTemp(SeamRequest), Key, LODLevel = MinParticipantLOD, DirtiedAt = Job.DirtiedAtSeconds, ReadyAt = Job.ReadyAtSeconds, ScheduledAt = Job.ScheduledAtSeconds, DispatchedAt, OwnerEpoch = Job.OwnerResidencyEpoch, SnapshotDecodes]() mutable
		{
			WaitForSnapshotDecodes(SnapshotDecodes);
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
			SeamRequest.VoxelData[i] = TakeSnapshot(i);
		}

		SeamJobsInFlight.Add(Job.Key);
		SeamInFlightPeak = FMath::Max(SeamInFlightPeak, SeamJobsInFlight.Num());
		Async(EAsyncExecution::ThreadPool, [MesherGuard = FMesherWorkerGuard(MesherWorkerJobs), WeakThis, SeamMesherPtr, SeamRequest = MoveTemp(SeamRequest), Key, LODLevel = MinParticipantLOD, DirtiedAt = Job.DirtiedAtSeconds, ReadyAt = Job.ReadyAtSeconds, ScheduledAt = Job.ScheduledAtSeconds, DispatchedAt, OwnerEpoch = Job.OwnerResidencyEpoch, SnapshotDecodes]() mutable
		{
			WaitForSnapshotDecodes(SnapshotDecodes);
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
		MeshWorkStats.SeamWorkerMs += Result.WorkerMs;

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

/** Hand a snapshot's decode to a caller that waits for it on its worker, or wait for it here on the game thread (G65). */
static void HandOutSnapshotDecode(const UE::Tasks::FTask& Decode, UE::Tasks::FTask* OutReady, const FIntVector& Coord)
{
	if (OutReady)
	{
		*OutReady = Decode.IsValid() && !Decode.IsCompleted() ? Decode : UE::Tasks::FTask();
		return;
	}
	if (Decode.IsValid() && !Decode.IsCompleted())
	{
		const VoxelResidencyStats::FGameThreadWaitScope WaitScope;
		const uint64 StartCycles = FPlatformTime::Cycles64();
		Decode.Wait();
		VoxelResidencyStats::RecordWait(FPlatformTime::Cycles64() - StartCycles, Coord);
	}
}

/** The snapshot reader bit (UVoxelChunkManager::ESnapshotReader) for a read reason (G69 instrumentation); "collision" is set by the cook itself (own or neighbour). */
static uint8 SnapshotReaderBit(const TCHAR* Reason)
{
	if (!Reason)
	{
		return 1 << 7; // SnapReaderOther
	}
	if (FCString::Strcmp(Reason, TEXT("collision")) == 0)
	{
		return 0;
	}
	if (FCString::Strcmp(Reason, TEXT("collision prefetch")) == 0)
	{
		return 1 << 0; // SnapReaderCollisionOwn
	}
	if (FCString::Strcmp(Reason, TEXT("seam job")) == 0)
	{
		return 1 << 2; // SnapReaderSeamJob
	}
	if (FCString::Strcmp(Reason, TEXT("chunk mesh")) == 0)
	{
		return 1 << 3; // SnapReaderChunkMesh
	}
	if (FCString::Strcmp(Reason, TEXT("prefetch")) == 0)
	{
		return 1 << 4; // SnapReaderPrefetch
	}
	if (FCString::Strncmp(Reason, TEXT("water"), 5) == 0)
	{
		return 1 << 5; // SnapReaderWater
	}
	if (FCString::Strcmp(Reason, TEXT("scatter")) == 0)
	{
		return 1 << 6; // SnapReaderScatter
	}
	return 1 << 7; // SnapReaderOther
}

TSharedPtr<const TArray<FVoxelData>> UVoxelChunkManager::GetUniformSnapshotArray(const FVoxelData& Value, int32 TotalVoxels)
{
	const uint32 Key = static_cast<uint32>(Value.MaterialID) | (static_cast<uint32>(Value.Density) << 8)
		| (static_cast<uint32>(Value.BiomeID) << 16) | (static_cast<uint32>(Value.Metadata) << 24);
	TSharedPtr<const TArray<FVoxelData>>& Shared = UniformSnapshotArrays.FindOrAdd(Key);
	if (!Shared.IsValid() || Shared->Num() != TotalVoxels)
	{
		TSharedPtr<TArray<FVoxelData>> Filled = MakeShared<TArray<FVoxelData>>();
		Filled->Init(Value, TotalVoxels);
		Shared = Filled;
	}
	return Shared;
}

int64 UVoxelChunkManager::GetSnapshotOwnBytes(const FIntVector& Coord, const FSeamVoxelSnapshot& Snap) const
{
	if (!Snap.Data.IsValid() || Snap.Kind == static_cast<uint8>(ESnapshotKind::SharedUniform))
	{
		return 0;
	}
	if (Snap.Kind == static_cast<uint8>(ESnapshotKind::SharedResident))
	{
		// The chunk's own array while the chunk still holds it (G71); once the chunk compacts, unloads or clones it, the
		// entry holds it alone.
		const FVoxelChunkState* State = ChunkStates.Find(Coord);
		if (State && State->Descriptor.SharesVoxelArray(Snap.Data.Get()))
		{
			return 0;
		}
	}
	if (Snap.Ready.IsValid() && !Snap.Ready.IsCompleted())
	{
		const int64 CS = Configuration ? Configuration->ChunkSize : 0;
		return CS * CS * CS * static_cast<int64>(sizeof(FVoxelData));
	}
	return static_cast<int64>(Snap.Data->GetAllocatedSize());
}

void UVoxelChunkManager::NoteSnapshotMade(const FIntVector& Coord, FSeamVoxelSnapshot& Entry, ESnapshotKind Kind)
{
	if (RecentlyEvictedSnapshots.Remove(Coord) > 0)
	{
		++SnapshotCounters.RemadeAfterEviction;
	}
	Entry.Kind = static_cast<uint8>(Kind);
	Entry.CreatedTick = SeamTickCounter;
	Entry.FirstReason = VoxelResidencyStats::GetReadReason();
	Entry.LastReason = Entry.FirstReason;
	Entry.ReaderMask |= SnapshotReaderBit(Entry.FirstReason);
	++SnapshotCounters.Made[static_cast<int32>(Kind)];
}

TSharedPtr<const TArray<FVoxelData>> UVoxelChunkManager::GetSeamVoxelSnapshot(const FIntVector& Coord, FVoxelChunkState& State,
	UE::Tasks::FTask* OutReady)
{
	const uint32 Version = State.Descriptor.ContentVersion;
	if (FSeamVoxelSnapshot* Cached = SeamSnapshotCache.Find(Coord))
	{
		if (Cached->ContentVersion == Version && Cached->Data.IsValid())
		{
			Cached->LastUsedTick = SeamTickCounter;
			++Cached->Hits;
			Cached->LastReason = VoxelResidencyStats::GetReadReason();
			Cached->ReaderMask |= SnapshotReaderBit(Cached->LastReason);
			++SnapshotCounters.Hits;
			HandOutSnapshotDecode(Cached->Ready, OutReady, Coord);
			return Cached->Data;
		}
		++SnapshotCounters.Replaced;
	}

	// A far-compressed chunk without edits (G65): its snapshot is decoded by a worker task, and the chunk stays compressed.
	// Its readers wait for the task on their own workers (seam jobs, cooks), or here (game-thread readers).
	FVoxelCompactSource Source;
	if (CVarFarCompressionSnapshotReads.GetValueOnGameThread() != 0
		&& !(EditManager && EditManager->ChunkHasEdits(Coord))
		&& State.Descriptor.CopyCompactSource(Source))
	{
		// A uniform chunk (G69): every voxel the same, so its snapshot is the shared array for that value. Nothing to decode.
		if (Source.Residency == EVoxelDataResidency::Uniform && CVarSnapshotShareUniform.GetValueOnGameThread() != 0)
		{
			FSeamVoxelSnapshot& Entry = SeamSnapshotCache.FindOrAdd(Coord);
			Entry = FSeamVoxelSnapshot();
			Entry.ContentVersion = Version;
			Entry.LastUsedTick = SeamTickCounter;
			Entry.Data = GetUniformSnapshotArray(Source.UniformValue, Source.TotalVoxels);
			NoteSnapshotMade(Coord, Entry, ESnapshotKind::SharedUniform);
			if (OutReady)
			{
				*OutReady = UE::Tasks::FTask();
			}
			return Entry.Data;
		}

		TSharedPtr<TArray<FVoxelData>> Decoded = MakeShared<TArray<FVoxelData>>();
		const TCHAR* Reason = VoxelResidencyStats::GetReadReason();
		UE::Tasks::FTask Decode = UE::Tasks::Launch(UE_SOURCE_LOCATION,
			[Decoded, Source = MoveTemp(Source), Coord, Reason]()
			{
				const VoxelResidencyStats::FReadScope ResidencyReadScope(Reason ? Reason : TEXT("snapshot"));
				TRACE_CPUPROFILER_EVENT_SCOPE(VoxelChunk_SnapshotDecode);
				const uint64 StartCycles = FPlatformTime::Cycles64();
				Source.Expand(*Decoded);
				VoxelResidencyStats::RecordExpand(Source.Residency == EVoxelDataResidency::Compressed,
					FPlatformTime::Cycles64() - StartCycles, Coord);
			},
			UE::Tasks::ETaskPriority::BackgroundHigh);
		++SnapshotDecodeTasks;
		const ESnapshotKind Kind = (Source.Residency == EVoxelDataResidency::Uniform) ? ESnapshotKind::DecodedUniform
			: ESnapshotKind::DecodedCompressed;
		FSeamVoxelSnapshot& Entry = SeamSnapshotCache.FindOrAdd(Coord);
		Entry = FSeamVoxelSnapshot();
		Entry.ContentVersion = Version;
		Entry.LastUsedTick = SeamTickCounter;
		Entry.Data = Decoded;
		Entry.Ready = Decode;
		NoteSnapshotMade(Coord, Entry, Kind);
		HandOutSnapshotDecode(Decode, OutReady, Coord);
		return Decoded;
	}

	// Build once per (chunk, content version). ContentVersion covers edits (bumped on explicit voxel edits), so the
	// merged snapshot stays valid exactly as long as the seams that would read it. A resident chunk without edits shares
	// its own array, copy-on-write (G71): no copy, and its readers' voxels never change under them (the water pull's
	// flags, the one write that keeps the ContentVersion, drop this entry and then clone the array if a reader still
	// holds it). With edits the edit-merged copy is built on the game thread.
	const bool bWithEdits = EditManager && EditManager->ChunkHasEdits(Coord);
	TSharedPtr<const TArray<FVoxelData>> Data;
	ESnapshotKind Kind = ESnapshotKind::Copied;
	if (!bWithEdits && CVarSnapshotShareResident.GetValueOnGameThread() != 0)
	{
		Data = State.Descriptor.ShareVoxelData();
		Kind = ESnapshotKind::SharedResident;
	}
	if (!Data.IsValid())
	{
		TSharedPtr<TArray<FVoxelData>> Built = MakeShared<TArray<FVoxelData>>(State.Descriptor.GetVoxelDataForRead());
		if (bWithEdits)
		{
			EditManager->ApplyEditsToVoxelData(Coord, *Built);
		}
		Data = Built;
		Kind = bWithEdits ? ESnapshotKind::CopiedWithEdits : ESnapshotKind::Copied;
	}

	FSeamVoxelSnapshot& Entry = SeamSnapshotCache.FindOrAdd(Coord);
	Entry = FSeamVoxelSnapshot();
	Entry.ContentVersion = Version;
	Entry.LastUsedTick = SeamTickCounter;
	Entry.Data = Data;
	Entry.Ready = UE::Tasks::FTask();
	NoteSnapshotMade(Coord, Entry, Kind);
	if (OutReady)
	{
		*OutReady = UE::Tasks::FTask();
	}
	return Entry.Data;
}

bool UVoxelChunkManager::UsesCollisionNeighborShells() const
{
	return CVarCollisionNeighborShells.GetValueOnGameThread() != 0;
}

bool UVoxelChunkManager::ShouldUseCollisionShells(int32 Depth) const
{
	return UsesCollisionNeighborShells() && Configuration && 8 * Depth <= Configuration->ChunkSize;
}

TSharedPtr<const FVoxelChunkShell> UVoxelChunkManager::StoreChunkShell(const FIntVector& Coord, uint32 Version, int32 Depth,
	TSharedPtr<const FVoxelChunkShell> Shell, UE::Tasks::FTask Ready, EShellKind Kind)
{
	FChunkShellEntry& Entry = ShellCache.FindOrAdd(Coord);
	Entry = FChunkShellEntry();
	Entry.ContentVersion = Version;
	Entry.Depth = Depth;
	Entry.LastUsedTick = SeamTickCounter;
	Entry.Shell = MoveTemp(Shell);
	Entry.Ready = MoveTemp(Ready);
	Entry.Kind = static_cast<uint8>(Kind);
	++ShellsMade[static_cast<int32>(Kind)];
	return Entry.Shell;
}

TSharedPtr<const FVoxelChunkShell> UVoxelChunkManager::GetChunkShell(const FIntVector& Coord, FVoxelChunkState& State, int32 Depth,
	UE::Tasks::FTask* OutReady)
{
	if (OutReady)
	{
		*OutReady = UE::Tasks::FTask();
	}
	if (!Configuration || !State.Descriptor.HasVoxelDataAvailable())
	{
		return nullptr;
	}
	const int32 CS = Configuration->ChunkSize;
	const uint32 Version = State.Descriptor.ContentVersion;
	if (FChunkShellEntry* Cached = ShellCache.Find(Coord))
	{
		if (Cached->ContentVersion == Version && Cached->Depth >= Depth && Cached->Shell.IsValid())
		{
			Cached->LastUsedTick = SeamTickCounter;
			++Cached->Hits;
			++ShellHits;
			HandOutSnapshotDecode(Cached->Ready, OutReady, Coord);
			return Cached->Shell;
		}
	}

	const bool bEdits = EditManager && EditManager->ChunkHasEdits(Coord);

	// Cut the shell from a full snapshot on a worker, once that snapshot is filled (it may still be decoding).
	auto CutFromSnapshot = [this, &Coord, Version, Depth, CS, OutReady](const TSharedPtr<const TArray<FVoxelData>>& Data,
		const UE::Tasks::FTask& DataReady) -> TSharedPtr<const FVoxelChunkShell>
	{
		TSharedPtr<FVoxelChunkShell> Shell = MakeShared<FVoxelChunkShell>();
		auto CutBody = [Shell, Data, CS, Depth]()
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(VoxelChunk_ShellFromSnapshot);
			FVoxelChunkShell::Extract(*Data, CS, Depth, *Shell);
		};
		UE::Tasks::FTask Cut;
		if (DataReady.IsValid() && !DataReady.IsCompleted())
		{
			Cut = UE::Tasks::Launch(UE_SOURCE_LOCATION, MoveTemp(CutBody), UE::Tasks::Prerequisites(DataReady),
				UE::Tasks::ETaskPriority::BackgroundHigh);
		}
		else
		{
			Cut = UE::Tasks::Launch(UE_SOURCE_LOCATION, MoveTemp(CutBody), UE::Tasks::ETaskPriority::BackgroundHigh);
		}
		HandOutSnapshotDecode(Cut, OutReady, Coord);
		return StoreChunkShell(Coord, Version, Depth, Shell, Cut, EShellKind::FromSnapshot);
	};

	// The chunk's full snapshot at this version is already cached (by a seam job, a mesh job or its own cook): cut from it.
	if (const FSeamVoxelSnapshot* Snap = SeamSnapshotCache.Find(Coord))
	{
		if (Snap->ContentVersion == Version && Snap->Data.IsValid()
			&& Snap->Kind != static_cast<uint8>(ESnapshotKind::SharedUniform))
		{
			return CutFromSnapshot(Snap->Data, Snap->Ready);
		}
	}

	// A far-compressed chunk without edits: decode on a worker and keep only the shell (the chunk stays compressed).
	FVoxelCompactSource Source;
	if (!bEdits && CVarFarCompressionSnapshotReads.GetValueOnGameThread() != 0 && State.Descriptor.CopyCompactSource(Source))
	{
		if (Source.Residency == EVoxelDataResidency::Uniform)
		{
			const FVoxelData& V = Source.UniformValue;
			const uint64 Key = (static_cast<uint64>(V.MaterialID) | (static_cast<uint64>(V.Density) << 8)
				| (static_cast<uint64>(V.BiomeID) << 16) | (static_cast<uint64>(V.Metadata) << 24)) | (static_cast<uint64>(Depth) << 32);
			TSharedPtr<const FVoxelChunkShell>& Shared = UniformShells.FindOrAdd(Key);
			if (!Shared.IsValid())
			{
				TSharedPtr<FVoxelChunkShell> Filled = MakeShared<FVoxelChunkShell>();
				FVoxelChunkShell::FillUniform(V, CS, Depth, *Filled);
				Shared = Filled;
			}
			return StoreChunkShell(Coord, Version, Depth, Shared, UE::Tasks::FTask(), EShellKind::SharedUniform);
		}
		TSharedPtr<FVoxelChunkShell> Shell = MakeShared<FVoxelChunkShell>();
		const TCHAR* Reason = VoxelResidencyStats::GetReadReason();
		UE::Tasks::FTask Decode = UE::Tasks::Launch(UE_SOURCE_LOCATION,
			[Shell, Source = MoveTemp(Source), Coord, CS, Depth, Reason]()
			{
				const VoxelResidencyStats::FReadScope ResidencyReadScope(Reason ? Reason : TEXT("shell"));
				TRACE_CPUPROFILER_EVENT_SCOPE(VoxelChunk_ShellDecode);
				const uint64 StartCycles = FPlatformTime::Cycles64();
				TArray<FVoxelData> Full;
				Source.Expand(Full);
				VoxelResidencyStats::RecordExpand(true, FPlatformTime::Cycles64() - StartCycles, Coord);
				FVoxelChunkShell::Extract(Full, CS, Depth, *Shell);
			},
			UE::Tasks::ETaskPriority::BackgroundHigh);
		++SnapshotDecodeTasks;
		HandOutSnapshotDecode(Decode, OutReady, Coord);
		return StoreChunkShell(Coord, Version, Depth, Shell, Decode, EShellKind::DecodedCompressed);
	}

	// A chunk with edits: cut from its edit-merged snapshot.
	if (bEdits)
	{
		UE::Tasks::FTask SnapReady;
		const TSharedPtr<const TArray<FVoxelData>> Snap = GetSeamVoxelSnapshot(Coord, State, &SnapReady);
		if (!Snap.IsValid())
		{
			return nullptr;
		}
		return CutFromSnapshot(Snap, SnapReady);
	}

	// A resident chunk without edits: cut the faces from its array here (with voxel.FarCompression.SnapshotReads 0 a
	// compact chunk is expanded in place first, as before G65).
	TSharedPtr<FVoxelChunkShell> Shell = MakeShared<FVoxelChunkShell>();
	if (!FVoxelChunkShell::Extract(State.Descriptor.GetVoxelDataForRead(), CS, Depth, *Shell))
	{
		return nullptr;
	}
	return StoreChunkShell(Coord, Version, Depth, Shell, UE::Tasks::FTask(), EShellKind::FromResident);
}

void UVoxelChunkManager::PrefetchCollisionInputs(const FIntVector& ChunkCoord, int32 LODLevel)
{
	if (CVarFarCompressionSnapshotReads.GetValueOnGameThread() == 0)
	{
		return;
	}
	if (!Configuration)
	{
		return;
	}
	const int32 Depth = VoxelNeighborSlices::GetNeighborDepth(LODLevel, Configuration->ChunkSize);
	if (!ShouldUseCollisionShells(Depth))
	{
		PrefetchVoxelSnapshotsAround(ChunkCoord);
		return;
	}
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("collision prefetch"));
	for (int32 DZ = -1; DZ <= 1; ++DZ)
	{
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				// Not the chunk itself: its whole snapshot would wait in the cache for as long as the cook waits in the queue
				// (after a teleport, hundreds of them); its decode starts at the cook, and the cook's worker waits for it.
				if (DX == 0 && DY == 0 && DZ == 0)
				{
					continue;
				}
				const FIntVector Coord = ChunkCoord + FIntVector(DX, DY, DZ);
				FVoxelChunkState* State = ChunkStates.Find(Coord);
				// Only far-compressed chunks without edits: the rest are cut at the cook, on the game thread (the faces only).
				if (!State || !State->Descriptor.IsCompact() || (EditManager && EditManager->ChunkHasEdits(Coord)))
				{
					continue;
				}
				UE::Tasks::FTask Decode;
				GetChunkShell(Coord, *State, Depth, &Decode);
			}
		}
	}
}

void UVoxelChunkManager::PrefetchVoxelSnapshotsAround(const FIntVector& ChunkCoord)
{
	if (CVarFarCompressionSnapshotReads.GetValueOnGameThread() == 0)
	{
		return;
	}
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("prefetch"));
	for (int32 DZ = -1; DZ <= 1; ++DZ)
	{
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const FIntVector Coord = ChunkCoord + FIntVector(DX, DY, DZ);
				FVoxelChunkState* State = ChunkStates.Find(Coord);
				// Only far-compressed chunks without edits: others would be read (expanded) here.
				if (!State || !State->Descriptor.IsCompact() || (EditManager && EditManager->ChunkHasEdits(Coord)))
				{
					continue;
				}
				UE::Tasks::FTask Decode;
				GetSeamVoxelSnapshot(Coord, *State, &Decode);
			}
		}
	}
}

FString UVoxelChunkManager::DescribeSnapshotCache() const
{
	static const TCHAR* KindNames[] = { TEXT("copied"), TEXT("copied with edits"), TEXT("decoded compressed"),
		TEXT("decoded uniform"), TEXT("shared uniform"), TEXT("shared with the resident chunk") };
	constexpr int32 NumKinds = static_cast<int32>(ESnapshotKind::Num);
	const double ToMB = 1.0 / (1024.0 * 1024.0);

	int32 KindN[NumKinds] = {};
	double KindMB[NumKinds] = {};
	int32 Pending = 0;
	int32 Held = 0;
	double HeldMB = 0.0;
	double TotalMB = 0.0;
	// Since the last read, in seam ticks (one a frame): < 16, < 64, < 128, >= 128 (waiting for the sweep).
	int32 AgeN[4] = {};
	double AgeMB[4] = {};
	// Reads after the one that made it: 0, 1-2, 3-9, 10+.
	int32 HitN[4] = {};
	double HitMB[4] = {};
	// Chebyshev distance from the viewer's chunk: 0-1, 2-3, 4-7, 8+; and the chunk's state.
	int32 DistN[4] = {};
	int32 Compact = 0, Resident = 0, Gone = 0;
	TMap<FString, int32> ByFirst, ByLast;
	TMap<uint32, int32> UniformValues;
	// Who took each snapshot (ESnapshotReader bits, prefetch aside): a seam job or a chunk mesh; only collision cooks as a
	// neighbour; only its own cook; both; anything else.
	int32 ReadN[5] = {};
	double ReadMB[5] = {};
	// Entries sharing a resident chunk's array (G71), and the MB of those the chunk still holds (not in the cache's total).
	int32 SharedN = 0;
	double SharedResidentMB = 0.0;
	const FIntVector ViewerChunk = WorldToChunkCoord(CurrentViewerPosition);

	for (const auto& Pair : SeamSnapshotCache)
	{
		const FSeamVoxelSnapshot& Snap = Pair.Value;
		if (!Snap.Data.IsValid())
		{
			continue;
		}
		const bool bPending = Snap.Ready.IsValid() && !Snap.Ready.IsCompleted();
		// What the cache holds of its own (G71: none of a resident chunk's array it shares).
		const double MB = GetSnapshotOwnBytes(Pair.Key, Snap) * ToMB;
		TotalMB += MB;
		const int32 Kind = FMath::Clamp<int32>(Snap.Kind, 0, NumKinds - 1);
		++KindN[Kind];
		KindMB[Kind] += MB;
		Pending += bPending ? 1 : 0;
		const FVoxelChunkState* OwnerState = ChunkStates.Find(Pair.Key);
		const bool bChunkShares = OwnerState && OwnerState->Descriptor.SharesVoxelArray(Snap.Data.Get());
		if (Kind == static_cast<int32>(ESnapshotKind::SharedResident))
		{
			++SharedN;
			SharedResidentMB += bChunkShares ? static_cast<double>(Snap.Data->GetAllocatedSize()) * ToMB : 0.0;
		}
		if (Snap.Kind != static_cast<uint8>(ESnapshotKind::SharedUniform)
			&& Snap.Data.GetSharedReferenceCount() > (bChunkShares ? 2 : 1))
		{
			++Held;
			HeldMB += MB;
		}
		const uint64 Age = SeamTickCounter - Snap.LastUsedTick;
		const int32 AgeBin = Age < 16 ? 0 : (Age < 64 ? 1 : (Age < 128 ? 2 : 3));
		++AgeN[AgeBin];
		AgeMB[AgeBin] += MB;
		const int32 HitBin = Snap.Hits == 0 ? 0 : (Snap.Hits <= 2 ? 1 : (Snap.Hits <= 9 ? 2 : 3));
		++HitN[HitBin];
		HitMB[HitBin] += MB;
		const FIntVector D = Pair.Key - ViewerChunk;
		const int32 Cheb = FMath::Max3(FMath::Abs(D.X), FMath::Abs(D.Y), FMath::Abs(D.Z));
		++DistN[Cheb <= 1 ? 0 : (Cheb <= 3 ? 1 : (Cheb <= 7 ? 2 : 3))];
		if (const FVoxelChunkState* State = OwnerState)
		{
			if (State->Descriptor.IsCompact())
			{
				++Compact;
			}
			else
			{
				++Resident;
			}
		}
		else
		{
			++Gone;
		}
		{
			const uint8 M = Snap.ReaderMask & static_cast<uint8>(~SnapReaderPrefetch);
			const int32 R = (M & (SnapReaderSeamJob | SnapReaderChunkMesh)) ? 0
				: (M == SnapReaderCollisionNeighbor ? 1
				: (M == SnapReaderCollisionOwn ? 2
				: (M == (SnapReaderCollisionOwn | SnapReaderCollisionNeighbor) ? 3 : 4)));
			++ReadN[R];
			ReadMB[R] += MB;
		}
		ByFirst.FindOrAdd(Snap.FirstReason ? Snap.FirstReason : TEXT("(untagged)"))++;
		ByLast.FindOrAdd(Snap.LastReason ? Snap.LastReason : TEXT("(untagged)"))++;
		if (Kind == static_cast<int32>(ESnapshotKind::DecodedUniform) && !bPending && Snap.Data->Num() > 0)
		{
			const FVoxelData& V = (*Snap.Data)[0];
			UniformValues.FindOrAdd(static_cast<uint32>(V.MaterialID) | (static_cast<uint32>(V.Density) << 8)
				| (static_cast<uint32>(V.BiomeID) << 16) | (static_cast<uint32>(V.Metadata) << 24))++;
		}
	}

	auto Join = [](const TMap<FString, int32>& Map)
	{
		TArray<TPair<FString, int32>> Sorted;
		for (const auto& P : Map)
		{
			Sorted.Emplace(P.Key, P.Value);
		}
		Sorted.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
		FString Joined;
		for (const auto& P : Sorted)
		{
			Joined += FString::Printf(TEXT("%s%s %d"), Joined.IsEmpty() ? TEXT("") : TEXT(", "), *P.Key, P.Value);
		}
		return Joined;
	};

	FString Out = FString::Printf(TEXT("entries %d, %.1f MB (%d decoding); held by a reader too: %d (%.1f MB); chunk compact %d, ")
		TEXT("resident %d, unloaded %d\n"), SeamSnapshotCache.Num(), TotalMB, Pending, Held, HeldMB, Compact, Resident, Gone);
	for (int32 K = 0; K < NumKinds; ++K)
	{
		Out += FString::Printf(TEXT("kind %s: %d, %.1f MB\n"), KindNames[K], KindN[K], KindMB[K]);
	}
	double SharedMB = 0.0;
	for (const auto& P : UniformSnapshotArrays)
	{
		SharedMB += P.Value.IsValid() ? P.Value->GetAllocatedSize() * ToMB : 0.0;
	}
	Out += FString::Printf(TEXT("uniform snapshots: %d distinct values among the decoded; shared arrays %d (%.1f MB, in the total ")
		TEXT("memory stats, not above)\n"), UniformValues.Num(), UniformSnapshotArrays.Num(), SharedMB);
	{
		int64 Clones = 0;
		double CloneMs = 0.0;
		VoxelResidencyStats::GetSharedArrayClones(Clones, CloneMs);
		Out += FString::Printf(TEXT("resident arrays shared (voxel.Snapshot.ShareResident %d): %d entries, %.1f MB the chunks hold (in ")
			TEXT("the voxel data, not above); copy-on-write clones %lld (%.1f ms)\n"), CVarSnapshotShareResident.GetValueOnGameThread(),
			SharedN, SharedResidentMB, Clones, CloneMs);
	}
	Out += FString::Printf(TEXT("since last read (ticks): <16 %d (%.0f MB), 16-63 %d (%.0f MB), 64-127 %d (%.0f MB), >=128 %d (%.0f MB)\n"),
		AgeN[0], AgeMB[0], AgeN[1], AgeMB[1], AgeN[2], AgeMB[2], AgeN[3], AgeMB[3]);
	Out += FString::Printf(TEXT("reads after the first: 0 %d (%.0f MB), 1-2 %d (%.0f MB), 3-9 %d (%.0f MB), 10+ %d (%.0f MB)\n"),
		HitN[0], HitMB[0], HitN[1], HitMB[1], HitN[2], HitMB[2], HitN[3], HitMB[3]);
	Out += FString::Printf(TEXT("distance from the viewer's chunk: 0-1 %d, 2-3 %d, 4-7 %d, 8+ %d\n"), DistN[0], DistN[1], DistN[2], DistN[3]);
	Out += FString::Printf(TEXT("made by: %s\n"), *Join(ByFirst));
	Out += FString::Printf(TEXT("last read by: %s\n"), *Join(ByLast));
	Out += FString::Printf(TEXT("taken by: a seam job or chunk mesh %d (%.0f MB), only collision cooks as a neighbour %d (%.0f MB), ")
		TEXT("only its own cook %d (%.0f MB), both %d (%.0f MB), other %d (%.0f MB)\n"),
		ReadN[0], ReadMB[0], ReadN[1], ReadMB[1], ReadN[2], ReadMB[2], ReadN[3], ReadMB[3], ReadN[4], ReadMB[4]);
	{
		// G71: the entries a seam job or a chunk mesh took, by the chunk's rendered LOD (0, 1, 2, 3+, none) and by who took
		// them (seam jobs only, its chunk mesh only, both), and what a boundary shell deep enough for that LOD's seam rings
		// would hold: 2 * stride + 2 voxels on the -X/-Y/-Z faces (the neighbour side's first cell layer and its gradient
		// taps), 3 * stride + 2 on the +X/+Y/+Z faces (the owner side's ring a layer further in).
		const int64 CS = Configuration ? Configuration->ChunkSize : 0;
		int32 N[5][3] = {};
		double MB[5][3] = {};
		double ShellEstimateMB[5] = {};
		for (const auto& Pair : SeamSnapshotCache)
		{
			const FSeamVoxelSnapshot& Snap = Pair.Value;
			const uint8 M = Snap.ReaderMask;
			if (!Snap.Data.IsValid() || !(M & (SnapReaderSeamJob | SnapReaderChunkMesh))
				|| Snap.Kind == static_cast<uint8>(ESnapshotKind::SharedUniform))
			{
				continue;
			}
			const FVoxelChunkState* State = ChunkStates.Find(Pair.Key);
			const int32 LOD = (State && State->MeshedLODLevel >= 0) ? FMath::Min(State->MeshedLODLevel, 3) : 4;
			const int32 Who = (M & SnapReaderChunkMesh) ? ((M & SnapReaderSeamJob) ? 2 : 1) : 0;
			const double EntryMB = GetSnapshotOwnBytes(Pair.Key, Snap) * ToMB;
			++N[LOD][Who];
			MB[LOD][Who] += EntryMB;
			if (LOD < 4 && CS > 0)
			{
				const int64 Stride = int64(1) << (State ? State->MeshedLODLevel : 0);
				const int64 Inner = FMath::Max<int64>(0, CS - (2 * Stride + 2) - (3 * Stride + 2));
				const double ShellShare = 1.0 - static_cast<double>(Inner * Inner * Inner) / static_cast<double>(CS * CS * CS);
				ShellEstimateMB[LOD] += EntryMB * ShellShare;
			}
		}
		static const TCHAR* LODNames[5] = { TEXT("LOD 0"), TEXT("LOD 1"), TEXT("LOD 2"), TEXT("LOD 3+"), TEXT("no mesh") };
		Out += TEXT("G71, seam/mesh entries by LOD (seam jobs only / its mesh only / both; a seam shell would hold):");
		for (int32 L = 0; L < 5; ++L)
		{
			Out += FString::Printf(TEXT(" %s %d/%d/%d (%.0f/%.0f/%.0f MB; shell %.0f MB)"), LODNames[L], N[L][0], N[L][1], N[L][2],
				MB[L][0], MB[L][1], MB[L][2], ShellEstimateMB[L]);
		}
		Out += TEXT("\n");
	}
	{
		static const TCHAR* ShellKindNames[] = { TEXT("from resident"), TEXT("from snapshot"), TEXT("decoded"), TEXT("shared uniform") };
		int32 ShellN[static_cast<int32>(EShellKind::Num)] = {};
		double ShellMB = 0.0;
		int32 ShellPending = 0;
		const int64 CS = Configuration ? Configuration->ChunkSize : 0;
		TSet<const FVoxelChunkShell*> SharedShells;
		for (const auto& P : UniformShells)
		{
			if (P.Value.IsValid())
			{
				SharedShells.Add(P.Value.Get());
				ShellMB += P.Value->GetAllocatedSize() * ToMB;
			}
		}
		for (const auto& P : ShellCache)
		{
			const FChunkShellEntry& E = P.Value;
			++ShellN[FMath::Clamp<int32>(E.Kind, 0, static_cast<int32>(EShellKind::Num) - 1)];
			const bool bPending = E.Ready.IsValid() && !E.Ready.IsCompleted();
			ShellPending += bPending ? 1 : 0;
			if (E.Shell.IsValid() && !SharedShells.Contains(E.Shell.Get()))
			{
				ShellMB += (bPending ? 6.0 * E.Depth * CS * CS * sizeof(FVoxelData)
					: static_cast<double>(E.Shell->GetAllocatedSize())) * ToMB;
			}
		}
		Out += FString::Printf(TEXT("shells (voxel.Collision.NeighborShells %d): %d, %.1f MB (%d filling): %s %d, %s %d, %s %d, %s %d; ")
			TEXT("session made %lld / %lld / %lld / %lld, hits %lld\n"), UsesCollisionNeighborShells() ? 1 : 0, ShellCache.Num(), ShellMB,
			ShellPending, ShellKindNames[0], ShellN[0], ShellKindNames[1], ShellN[1], ShellKindNames[2], ShellN[2], ShellKindNames[3],
			ShellN[3], ShellsMade[0], ShellsMade[1], ShellsMade[2], ShellsMade[3], ShellHits);
	}
	const FSnapshotCacheCounters& C = SnapshotCounters;
	Out += FString::Printf(TEXT("session: made copied %lld, with edits %lld, decoded compressed %lld, decoded uniform %lld, shared ")
		TEXT("uniform %lld, shared resident %lld; reads after ")
		TEXT("the first %lld; replaced %lld; evicted by age %lld (%lld never read again, mean life %.0f ticks); removed on unload %lld, on ")
		TEXT("water %lld"),
		C.Made[0], C.Made[1], C.Made[2], C.Made[3], C.Made[4], C.Made[5], C.Hits, C.Replaced, C.EvictedByAge, C.EvictedByAgeUnread,
		C.EvictedByAge > 0 ? static_cast<double>(C.EvictedAgeTicksSum) / C.EvictedByAge : 0.0, C.RemovedOnUnload, C.RemovedOnWater);
	Out += FString::Printf(TEXT("\nsession: made again within 600 ticks of an eviction %lld (max age %d ticks); a cook's own dropped ")
		TEXT("after the cook %lld"), C.RemadeAfterEviction, CVarSnapshotMaxAgeTicks.GetValueOnGameThread(), C.ReleasedAfterCook);
	return Out;
}

void UVoxelChunkManager::ProcessFarCompressionSweep(const FLODQueryContext& Context, FVoxelTimingStats& Timing)
{
	const bool bEnabled = CVarFarCompression.GetValueOnGameThread() != 0;
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
		if (Result.Scratch.Max() > 0 && FarCompressScratchPool.Num() + FarCompressJobsInFlight < MaxInFlight)
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
		if (Budget >= MaxScans || FarCompressJobsInFlight >= MaxInFlight)
		{
			// Out of budget this tick; retry next tick (bCompressionEvaluated stays false).
			continue;
		}
		++Budget;
		++FarCompressEncodes;
		DispatchFarCompression(Pair.Key, D, Codec);
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

	// The resident array stays readable (neighbour reads, seam snapshots, point queries) and writable while the worker
	// runs, and a write clears the ticket, which voids the result. The worker encodes the chunk's own array, shared
	// copy-on-write (G71: a write meanwhile clones it), or with voxel.Snapshot.ShareResident 0 a copy, a memcpy into a
	// pooled array, as before; against ~3.5 ms for the encode.
	TSharedPtr<const TArray<FVoxelData>> Shared;
	TArray<FVoxelData> Copy;
	if (CVarSnapshotShareResident.GetValueOnGameThread() != 0)
	{
		Shared = Descriptor.ShareVoxelData();
	}
	else
	{
		Copy = FarCompressScratchPool.Num() > 0 ? FarCompressScratchPool.Pop(EAllowShrinking::No) : TArray<FVoxelData>();
		const TArray<FVoxelData>& Resident = Descriptor.GetResidentArray();
		Copy.SetNumUninitialized(Resident.Num(), EAllowShrinking::No);
		FMemory::Memcpy(Copy.GetData(), Resident.GetData(), static_cast<SIZE_T>(Resident.Num()) * sizeof(FVoxelData));
		FarCompressBytesInFlight += Copy.GetAllocatedSize();
	}
	++FarCompressJobsInFlight;

	// Background priority, outside the thread pool that runs generation, meshing and seam jobs:
	// compression only saves memory, so it never delays the streaming front.
	UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[Results = FarCompressResults, ChunkCoord, Ticket, Codec, ChunkSize = Descriptor.ChunkSize, Shared = MoveTemp(Shared),
			Copy = MoveTemp(Copy)]() mutable
		{
			FFarCompressResult Done;
			Done.ChunkCoord = ChunkCoord;
			Done.Ticket = Ticket;
			Done.Form = FChunkDescriptor::ComputeCompactForm(Shared.IsValid() ? *Shared : Copy, Codec, ChunkSize);
			Shared.Reset(); // let go before the result lands, so the chunk's array is unique again when it is installed
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

	// Collision is cooked whole-chunk with its 26 neighbours' voxels, a missing one read as air: re-cook the neighbours
	// that were cooked before this chunk had data (G32: stale collision strips along chunk faces).
	if (CollisionManager)
	{
		CollisionManager->OnChunkVoxelDataAvailable(ChunkCoord);
	}

	// New voxel data has no water pulled from the neighbours yet: its first mesh submit does that (G58).
	State->bWaterSettled = false;

	// Fire event
	OnChunkGenerated.Broadcast(ChunkCoord);

	UE_LOG(LogVoxelStreaming, Verbose, TEXT("Chunk (%d, %d, %d) generation complete"),
		ChunkCoord.X, ChunkCoord.Y, ChunkCoord.Z);
}

void UVoxelChunkManager::HandOffChunkScatter(const FIntVector& ChunkCoord, FVoxelChunkState& State)
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("scatter"));
	if (!ScatterManager || !Configuration || !Configuration->bEnableScatter)
	{
		return;
	}
	// Scatter must classify against the terrain the player actually sees. When the chunk has edits (player dig/build, POI,
	// editor), merge them into a scratch copy so scatter extracts from edit-merged voxels: otherwise grass can cover a dug
	// cave opening or mushrooms can appear on a carved-flat surface.
	if (EditManager && EditManager->ChunkHasEdits(ChunkCoord))
	{
		TArray<FVoxelData> EditMergedVoxels = State.Descriptor.GetVoxelDataForRead();
		EditManager->ApplyEditsToVoxelData(ChunkCoord, EditMergedVoxels);
		ScatterManager->OnChunkMeshDataReady(ChunkCoord, EditMergedVoxels, State.Descriptor.ChunkSize, Configuration->VoxelSize);
	}
	else if (CVarFarCompressionSnapshotReads.GetValueOnGameThread() != 0 && State.Descriptor.IsCompact())
	{
		// A far-compressed chunk hands scatter its snapshot (decoded on a worker, prefetched when the chunk was queued for
		// its mesh) and stays compressed (G65).
		const TSharedPtr<const TArray<FVoxelData>> Snapshot = GetSeamVoxelSnapshot(ChunkCoord, State);
		ScatterManager->OnChunkMeshDataReady(ChunkCoord, *Snapshot, State.Descriptor.ChunkSize, Configuration->VoxelSize);
	}
	else
	{
		ScatterManager->OnChunkMeshDataReady(ChunkCoord, State.Descriptor.GetVoxelDataForRead(), State.Descriptor.ChunkSize,
			Configuration->VoxelSize);
	}
}

void UVoxelChunkManager::ProcessEditScatterReaders()
{
	if (EditScatterReaderChunks.Num() == 0)
	{
		EditScatterEditedChunks.Reset();
		return;
	}
	for (const FIntVector& Reader : EditScatterReaderChunks)
	{
		// An edited chunk re-extracts with its re-mesh, and its extraction launches after this edit: it reads it.
		if (EditScatterEditedChunks.Contains(Reader))
		{
			++EditScatterStats.ReadersFolded;
			continue;
		}
		// A chunk without voxel data has not been handed to scatter yet (its extraction reads the edit when it is), or is
		// on its way out.
		FVoxelChunkState* State = ChunkStates.Find(Reader);
		if (!State || !State->Descriptor.HasVoxelDataAvailable() || State->State == EChunkState::PendingUnload)
		{
			++EditScatterStats.ReadersWithoutData;
			continue;
		}
		// Its own voxels did not change, so no re-mesh: drop its scatter caches (the instances stay until the re-extraction
		// replaces them) and hand its voxels to scatter now. A re-mesh hand-off that follows finds its types done.
		ScatterManager->RegenerateChunkScatter(Reader);
		HandOffChunkScatter(Reader, *State);
		++EditScatterStats.ReadersReextracted;
	}
	EditScatterReaderChunks.Reset();
	EditScatterEditedChunks.Reset();
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
					// Scatter hears of this submit too (world modes review G79): a chunk whose first mesh landed after it was
					// re-queued (a LOD change decided while the mesh was on a worker) went to the renderer here and never to
					// scatter, so its first extraction waited for its next mesh on the path below, the LOD refine near the
					// viewer, with every type landing at once. The hand-off reads the chunk's voxels, not this mesh, and a
					// chunk whose types are done takes nothing from it. A chunk on its way out is left alone.
					if (ScatterManager && Configuration && Configuration->bEnableScatter
						&& State->State != EChunkState::PendingUnload && State->State != EChunkState::Unloaded)
					{
						const double ScatT0 = FPlatformTime::Seconds();
						HandOffChunkScatter(ChunkCoord, *State);
						SubmitScatterSecondsThisTick += FPlatformTime::Seconds() - ScatT0;
					}

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
					BenchEverMeshed.Add(ChunkCoord);
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
			RemeshIfEditedWhileMeshing(ChunkCoord);
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

		// Notify scatter manager with voxel data for LOD-independent surface extraction. Scatter reads
		// the chunk's voxels, never the mesh: the interior mesh has no vertex in the cells along the
		// chunk's max faces, which belong to its seams (world modes review G27).
		if (ScatterManager && Configuration && Configuration->bEnableScatter)
		{
			const double ScatT0 = FPlatformTime::Seconds();
			HandOffChunkScatter(ChunkCoord, *State);
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
		BenchEverMeshed.Add(ChunkCoord);

		// Propagate water flags from loaded neighbors into this chunk's voxel data
		// so caves connected across chunk boundaries receive consistent water flags.
		if (Configuration && Configuration->bEnableWaterLevel)
		{
			const double WaterT0 = FPlatformTime::Seconds();
			uint8 FloodedFaces = 0;
			PropagateWaterFromNeighbors(ChunkCoord, &FloodedFaces);

			// The chunk's water is in place, the first time since its voxel data arrived: an edit that reached it before (a
			// POI's terraform or a dungeon stamp at stream-in, a save's edits) is scanned for voxels it opened to the sea now
			// (G58), and the faces it shares with chunks whose water is in place for edit water to cross (G59).
			const bool bWaterArrived = !State->bWaterSettled;
			State->bWaterSettled = true;
			if (WaterPropagation && bWaterArrived)
			{
				WaterPropagation->OnChunkWaterSettled(ChunkCoord);
			}

			// Its neighbours that pulled before its water was there pull again (G70): across every face the first time (its
			// generated water arrived with it), else across the faces this pull flooded.
			QueueWaterRepulls(ChunkCoord, bWaterArrived ? 0x3F : FloodedFaces);

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

	RemeshIfEditedWhileMeshing(ChunkCoord);
}

void UVoxelChunkManager::RemeshIfEditedWhileMeshing(const FIntVector& ChunkCoord)
{
	// Found again: an OnChunkLoaded listener may have changed ChunkStates.
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (State && State->bRemeshAfterMeshing && State->State == EChunkState::Loaded)
	{
		// The mesh that just landed was built before an edit reached the chunk (G57): mesh it again with the edit.
		State->bRemeshAfterMeshing = false;
		++MeshWorkStats.RemeshesAfterMeshing;
		MarkChunkDirty(ChunkCoord);
	}
}

bool UVoxelChunkManager::UsesWaterTiles() const
{
	// The tiles are XY columns meshed at the WaterLevel plane; a planet's sea is a sphere (its surface is the world
	// actor's water sphere until SP-E2), so the tiles would draw in the wrong place there.
	return Configuration && Configuration->bEnableWaterLevel && Configuration->WaterMeshMaterial
		&& Configuration->WorldMode != EWorldMode::SphericalPlanet;
}

bool UVoxelChunkManager::PropagateWaterFromNeighbors(const FIntVector& ChunkCoord, uint8* OutFloodedFaces, int32* OutFloodedVoxels)
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("water pull"));
	if (OutFloodedFaces)
	{
		*OutFloodedFaces = 0;
	}
	if (OutFloodedVoxels)
	{
		*OutFloodedVoxels = 0;
	}
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

	// "Below the sea" as generation's fill pass tests it: the WaterLevel plane, or a planet's WaterRadius sphere (SP-B8),
	// in world space (the chunk grid is relative to WorldOrigin; the plane is world Z, as the terrain's heights are, G19).
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*Configuration);
	const FVector VoxelOrigin = ChunkWorldPos + Configuration->WorldOrigin;
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
	const int32 MaxSeedZ = Sea.bRadial ? CS - 1 : FMath::Clamp(FMath::FloorToInt32((Sea.PlaneZ - VoxelOrigin.Z) / VS), 0, CS - 1);

	// The seed search reads only (G65): a far-compressed chunk without edits through its snapshot (decoded on a worker,
	// usually prefetched when the chunk was queued), so a chunk no water flows into is neither expanded nor marked
	// mutated. The chunk's own array is taken for writing once a seed is found.
	const bool bSnapshotReads = CVarFarCompressionSnapshotReads.GetValueOnGameThread() != 0;
	auto ReadVoxels = [this, bSnapshotReads](const FIntVector& Coord, FVoxelChunkState& ReadState,
		TSharedPtr<const TArray<FVoxelData>>& OutHold) -> const TArray<FVoxelData>&
	{
		if (bSnapshotReads && ReadState.Descriptor.IsCompact() && !(EditManager && EditManager->ChunkHasEdits(Coord)))
		{
			OutHold = GetSeamVoxelSnapshot(Coord, ReadState);
			return *OutHold;
		}
		return ReadState.Descriptor.GetVoxelDataForRead();
	};
	TSharedPtr<const TArray<FVoxelData>> OwnHold;
	const TArray<FVoxelData>& OwnVoxels = ReadVoxels(ChunkCoord, *State, OwnHold);

	// Collect seed voxels from 6 face neighbors
	TArray<int32> Seeds;
	TSet<int32> SeedSet;

	static const FIntVector FaceOffsets[6] = {
		{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
	};

	for (int32 F = 0; F < 6; ++F)
	{
		const FIntVector NeighborCoord = ChunkCoord + FaceOffsets[F];
		FVoxelChunkState* NeighborState = ChunkStates.Find(NeighborCoord);
		if (!NeighborState || !NeighborState->Descriptor.HasVoxelDataAvailable())
		{
			continue;
		}

		TSharedPtr<const TArray<FVoxelData>> NeighborHold;
		const TArray<FVoxelData>& NeighborData = ReadVoxels(NeighborCoord, *NeighborState, NeighborHold);

		// Determine which boundary face to check on each chunk
		// For face direction F, check our boundary face and the neighbor's opposite face
		// e.g. +X neighbor: our X=CS-1 face vs neighbor's X=0 face
		const int32 MaxIdx = CS - 1;

		// +Z face: the whole plane sits at Z=MaxIdx — skip it when that plane is above water
		if (!Sea.bRadial && F == 4 && (VoxelOrigin.Z + MaxIdx * VS) > Sea.PlaneZ)
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
				const FVoxelData& OurVoxel = OwnVoxels[OurIdx];

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

				// Seed this voxel (once: a voxel on a chunk edge faces two neighbours)
				bool bAlreadySeeded = false;
				SeedSet.Add(OurIdx, &bAlreadySeeded);
				if (!bAlreadySeeded)
				{
					Seeds.Add(OurIdx);
				}
			}
		}
	}

	if (Seeds.Num() == 0)
	{
		return false;
	}

	// The flags go into the voxel array without a ContentVersion bump (GetVoxelDataMutable): an edit-merged snapshot
	// built before them would lack them, and the water tile reads one for a chunk with edits (G59). Dropped before the
	// write, so a snapshot sharing the resident array (G71) does not make the write clone it unless a job still holds it.
	OwnHold.Reset();
	SnapshotCounters.RemovedOnWater += SeamSnapshotCache.Remove(ChunkCoord);
	ShellCache.Remove(ChunkCoord);

	// Water flows in: write the seeds' flags into the chunk's own array, and flood from them.
	TArray<FVoxelData>& VoxelData = State->Descriptor.GetVoxelDataMutable();
	for (const int32 SeedIdx : Seeds)
	{
		VoxelData[SeedIdx].SetWaterFlag(true);
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

	// The faces the flood reached (the neighbours there may pull it on: G70). The queue holds every voxel it flagged.
	if (OutFloodedFaces)
	{
		uint8 Faces = 0;
		for (const int32 Idx : BFSQueue)
		{
			const int32 Z = Idx / SliceSize;
			const int32 Y = (Idx - Z * SliceSize) / CS;
			const int32 X = Idx - Z * SliceSize - Y * CS;
			Faces |= (X == CS - 1 ? 1 : 0) | (X == 0 ? 2 : 0) | (Y == CS - 1 ? 4 : 0) | (Y == 0 ? 8 : 0)
				| (Z == CS - 1 ? 16 : 0) | (Z == 0 ? 32 : 0);
			if (Faces == 0x3F)
			{
				break;
			}
		}
		*OutFloodedFaces = Faces;
	}
	if (OutFloodedVoxels)
	{
		*OutFloodedVoxels = BFSQueue.Num();
	}

	return true;
}

// ==================== 2D Water Tile System ====================

void UVoxelChunkManager::QueueWaterRepulls(const FIntVector& ChunkCoord, uint8 FaceMask)
{
	if (FaceMask == 0 || !Configuration || !Configuration->bEnableWaterLevel || CVarWaterPullAgain.GetValueOnGameThread() == 0)
	{
		return;
	}
	static const FIntVector FaceOffsets[6] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
	const int32 CS = Configuration->ChunkSize;
	const float VS = Configuration->VoxelSize;
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*Configuration);
	for (int32 F = 0; F < 6; ++F)
	{
		if (!(FaceMask & (1 << F)))
		{
			continue;
		}
		const FIntVector Neighbour = ChunkCoord + FaceOffsets[F];
		const FVoxelChunkState* NeighbourState = ChunkStates.Find(Neighbour);
		// A neighbour that has not pulled yet does when its mesh lands, and reads this chunk's water then.
		if (!NeighbourState || !NeighbourState->bWaterSettled || !NeighbourState->Descriptor.HasVoxelDataAvailable())
		{
			continue;
		}
		// Wholly above the sea: nothing to flood (PropagateWaterFromNeighbors' own test).
		const FVector VoxelOrigin = FVoxelCoordinates::ChunkToWorld(Neighbour, CS, VS) + Configuration->WorldOrigin;
		if (Sea.IsAbove(FBox(VoxelOrigin, VoxelOrigin + FVector((CS - 1) * VS))))
		{
			continue;
		}
		bool bAlreadyQueued = false;
		WaterRepullSet.Add(Neighbour, &bAlreadyQueued);
		if (!bAlreadyQueued)
		{
			WaterRepullQueue.Add(Neighbour);
			++WaterRepullStats.Queued;
		}
	}
}

void UVoxelChunkManager::ProcessWaterRepulls()
{
	// About a millisecond a tick, at least one pull: a pull without seeds reads only its six faces, one that floods a chunk
	// runs a BFS over it. First in, first out, so a flood spreads ring by ring.
	const double Deadline = FPlatformTime::Seconds() + 0.001;
	int32 Index = 0;
	for (; Index < WaterRepullQueue.Num(); ++Index)
	{
		if (Index > 0 && FPlatformTime::Seconds() > Deadline)
		{
			break;
		}
		const FIntVector ChunkCoord = WaterRepullQueue[Index];
		WaterRepullSet.Remove(ChunkCoord);
		FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
		// Only a Loaded chunk: one queued for a mesh or meshing pulls when that mesh lands, and its voxels are not written
		// while a job may read them.
		if (!State || !State->Descriptor.HasVoxelDataAvailable() || !State->bWaterSettled || State->State != EChunkState::Loaded)
		{
			++WaterRepullStats.Skipped;
			continue;
		}
		++WaterRepullStats.Run;
		uint8 FloodedFaces = 0;
		int32 FloodedVoxels = 0;
		if (PropagateWaterFromNeighbors(ChunkCoord, &FloodedFaces, &FloodedVoxels))
		{
			++WaterRepullStats.Flooded;
			WaterRepullStats.FloodedVoxels += FloodedVoxels;
			if (UsesWaterTiles())
			{
				UpdateWaterTileContribution(ChunkCoord);
			}
			QueueWaterRepulls(ChunkCoord, FloodedFaces);
		}
	}
	WaterRepullQueue.RemoveAt(0, Index, EAllowShrinking::No);
}

void UVoxelChunkManager::UpdateWaterTileContribution(const FIntVector& ChunkCoord)
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("water tile"));
	FVoxelChunkState* State = ChunkStates.Find(ChunkCoord);
	if (!State || !State->Descriptor.HasVoxelDataAvailable())
	{
		return;
	}

	const int32 CS = State->Descriptor.ChunkSize;

	// Chunks entirely above the water level cannot hold water flags — skip the full-volume
	// column scan (this runs per mesh submit) and just clear any stale contribution. World Z, as generation floods (G19).
	if (Configuration)
	{
		const FVector ChunkWorldPos = FVoxelCoordinates::ChunkToWorld(ChunkCoord, CS, Configuration->VoxelSize) + Configuration->WorldOrigin;
		if (ChunkWorldPos.Z > Configuration->WaterLevel)
		{
			RemoveWaterTileContribution(ChunkCoord);
			return;
		}
	}

	TArray<bool> PartialMask;
	PartialMask.SetNumZeroed(CS * CS);

	// The water as it is: an edit-driven flood puts its flags in the edit layer (UVoxelWaterPropagation), so a chunk with
	// edits builds its columns from the edit-merged voxels, which a dug channel's water and a filled sea's rock are part of
	// (G59). The snapshot is the one the seam jobs read for this content version; most chunks have no edits.
	TSharedPtr<const TArray<FVoxelData>> EditMerged;
	if (EditManager && EditManager->ChunkHasEdits(ChunkCoord))
	{
		EditMerged = GetEditMergedSnapshot(ChunkCoord);
	}
	else if (CVarFarCompressionSnapshotReads.GetValueOnGameThread() != 0 && State->Descriptor.IsCompact())
	{
		// A far-compressed chunk reads its snapshot (the generated voxels, no edits), decoded on a worker (G65).
		EditMerged = GetSeamVoxelSnapshot(ChunkCoord, *State);
	}
	bool bAnyWater = FVoxelWaterMesher::BuildColumnMask(
		EditMerged.IsValid() ? *EditMerged : State->Descriptor.GetVoxelDataForRead(), CS, PartialMask);

	FIntVector2 TileCoord(ChunkCoord.X, ChunkCoord.Y);

	if (bAnyWater)
	{
		// A new tile: the far field's band patches over this column hide their sea under it (WM-MP1), so they re-select
		// (the far field re-reads the window on a chunk-state version change).
		const bool bNewTile = !WaterTiles.Contains(TileCoord);
		FWaterTileState& Tile = WaterTiles.FindOrAdd(TileCoord);
		if (bNewTile)
		{
			++ChunkStateVersion;
		}
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

bool UVoxelChunkManager::HasWaterTile(const FIntPoint& Column) const
{
	return WaterTiles.Contains(FIntVector2(Column.X, Column.Y));
}

bool UVoxelChunkManager::GetWaterTileColumn(const FVector& WorldPosition, TArray<int32>& OutWetChunkZ) const
{
	OutWetChunkZ.Reset();
	if (!Configuration)
	{
		return false;
	}
	const int32 CS = Configuration->ChunkSize;
	const FIntVector Voxel = FVoxelCoordinates::WorldToVoxel(WorldPosition - Configuration->WorldOrigin, Configuration->VoxelSize);
	const FIntVector2 TileCoord(FMath::FloorToInt32(double(Voxel.X) / CS), FMath::FloorToInt32(double(Voxel.Y) / CS));
	const FWaterTileState* Tile = WaterTiles.Find(TileCoord);
	if (!Tile)
	{
		return false;
	}
	const int32 Column = (Voxel.X - TileCoord.X * CS) + (Voxel.Y - TileCoord.Y * CS) * CS;
	for (const TPair<int32, TArray<bool>>& Layer : Tile->PartialMasks)
	{
		if (Layer.Value.IsValidIndex(Column) && Layer.Value[Column])
		{
			OutWetChunkZ.Add(Layer.Key);
		}
	}
	OutWetChunkZ.Sort();
	return true;
}

void UVoxelChunkManager::AuditWaterTiles(int32 MaxList, const FIntPoint& DetailColumn)
{
	if (!Configuration || !UsesWaterTiles())
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.TileAudit: no water tiles in this world."));
		return;
	}
	const int32 CS = Configuration->ChunkSize;
	const double VS = Configuration->VoxelSize;
	const double ChunkWorld = CS * VS;
	const double WaterZ = Configuration->WaterLevel;
	const int32 WaterLayer = FMath::FloorToInt32((WaterZ - Configuration->WorldOrigin.Z) / ChunkWorld);

	struct FColumnAudit
	{
		FIntVector2 Tile = FIntVector2(0, 0);
		bool bTile = false;
		int32 Wet = 0;
		int32 ShouldBeWet = 0;
		int32 Missing = 0;
		int32 Extra = 0;
	};
	// Every chunk column with a tile or a loaded chunk at the water plane's layer.
	TSet<FIntVector2> Columns;
	for (const TPair<FIntVector, FVoxelChunkState>& Pair : ChunkStates)
	{
		if (Pair.Key.Z == WaterLayer && Pair.Value.Descriptor.HasVoxelDataAvailable())
		{
			Columns.Add(FIntVector2(Pair.Key.X, Pair.Key.Y));
		}
	}
	for (const TPair<FIntVector2, FWaterTileState>& Pair : WaterTiles)
	{
		Columns.Add(Pair.Key);
	}
	TArray<FColumnAudit> Audits;
	int64 TotalWet = 0, TotalShould = 0, TotalMissing = 0, TotalExtra = 0;
	int32 NoTileColumns = 0, NoTileMissing = 0, ColumnsMissing = 0;
	for (const FIntVector2& TileCoord : Columns)
	{
		FColumnAudit A;
		A.Tile = TileCoord;
		const FWaterTileState* Tile = WaterTiles.Find(TileCoord);
		A.bTile = Tile != nullptr;
		TArray<bool> Wet;
		Wet.SetNumZeroed(CS * CS);
		if (Tile)
		{
			for (const TPair<int32, TArray<bool>>& Layer : Tile->PartialMasks)
			{
				for (int32 I = 0; I < CS * CS && I < Layer.Value.Num(); ++I)
				{
					Wet[I] = Wet[I] || Layer.Value[I];
				}
			}
		}
		for (int32 Y = 0; Y < CS; ++Y)
		{
			for (int32 X = 0; X < CS; ++X)
			{
				const double WX = Configuration->WorldOrigin.X + (static_cast<double>(TileCoord.X) * CS + X + 0.5) * VS;
				const double WY = Configuration->WorldOrigin.Y + (static_cast<double>(TileCoord.Y) * CS + Y + 0.5) * VS;
				const double Ground = GetGeneratedSurfaceHeight(WX, WY);
				const bool bShould = Ground < WaterZ - VS; // at least a voxel of water over the ground
				const bool bWet = Wet[X + Y * CS];
				A.Wet += bWet ? 1 : 0;
				A.ShouldBeWet += bShould ? 1 : 0;
				A.Missing += (bShould && !bWet) ? 1 : 0;
				A.Extra += (bWet && Ground > WaterZ + VS) ? 1 : 0;
			}
		}
		TotalWet += A.Wet;
		TotalShould += A.ShouldBeWet;
		TotalMissing += A.Missing;
		TotalExtra += A.Extra;
		if (!A.bTile)
		{
			++NoTileColumns;
			NoTileMissing += A.Missing;
		}
		ColumnsMissing += A.Missing > 0 ? 1 : 0;
		Audits.Add(A);
	}
	Audits.Sort([](const FColumnAudit& L, const FColumnAudit& R) { return L.Missing > R.Missing; });
	UE_LOG(LogVoxelStreaming, Warning,
		TEXT("voxel.Water.TileAudit: water plane Z %.0f (chunk layer %d); %d chunk columns (%d tiles, %d loaded at the plane's layer with no tile); voxel columns wet %lld, under the plane by a voxel or more %lld, missing (under the plane, dry) %lld in %d chunk columns (%d of them in columns with no tile), extra (wet over the plane) %lld"),
		WaterZ, WaterLayer, Columns.Num(), WaterTiles.Num(), NoTileColumns, TotalWet, TotalShould, TotalMissing, ColumnsMissing, NoTileMissing, TotalExtra);

	int32 Listed = 0;
	for (const FColumnAudit& A : Audits)
	{
		const bool bDetail = DetailColumn.X == A.Tile.X && DetailColumn.Y == A.Tile.Y;
		if (!bDetail && (A.Missing == 0 || Listed >= MaxList))
		{
			continue;
		}
		if (!bDetail)
		{
			++Listed;
		}
		const double CX = Configuration->WorldOrigin.X + (A.Tile.X + 0.5) * ChunkWorld;
		const double CY = Configuration->WorldOrigin.Y + (A.Tile.Y + 0.5) * ChunkWorld;
		UE_LOG(LogVoxelStreaming, Warning, TEXT("  chunk column (%d, %d) centre (%.0f, %.0f): tile %s; wet %d, under the plane %d, missing %d, extra %d"),
			A.Tile.X, A.Tile.Y, CX, CY, A.bTile ? TEXT("yes") : TEXT("NO"), A.Wet, A.ShouldBeWet, A.Missing, A.Extra);
		const FWaterTileState* Tile = WaterTiles.Find(A.Tile);
		if (Tile && MeshRenderer)
		{
			FString RendererLine;
			MeshRenderer->GetWaterTileDrawInfo(A.Tile, RendererLine);
			UE_LOG(LogVoxelStreaming, Warning, TEXT("    tile dirty %s, in the dirty queue %s; renderer: %s"),
				Tile->bDirty ? TEXT("yes") : TEXT("no"), DirtyWaterTileSet.Contains(A.Tile) ? TEXT("yes") : TEXT("no"), *RendererLine);
		}
		for (TPair<FIntVector, FVoxelChunkState>& Pair : ChunkStates)
		{
			if (Pair.Key.X != A.Tile.X || Pair.Key.Y != A.Tile.Y)
			{
				continue;
			}
			const FIntVector Coord = Pair.Key;
			FVoxelChunkState& State = Pair.Value;
			const TCHAR* Residency = TEXT("?");
			switch (State.Descriptor.Residency)
			{
			case EVoxelDataResidency::Resident: Residency = TEXT("resident"); break;
			case EVoxelDataResidency::Uniform: Residency = TEXT("uniform"); break;
			case EVoxelDataResidency::Compressed: Residency = TEXT("compressed"); break;
			default: Residency = TEXT("empty"); break;
			}
			int32 StoredWet = -1;
			const TArray<bool>* Stored = Tile ? Tile->PartialMasks.Find(Coord.Z) : nullptr;
			if (Stored)
			{
				StoredWet = 0;
				for (const bool B : *Stored)
				{
					StoredWet += B ? 1 : 0;
				}
			}
			// The mask rebuilt from the chunk's voxels, as UpdateWaterTileContribution reads them.
			int32 FreshWet = -1, Stale = 0, FreshMissing = 0;
			if (State.Descriptor.HasVoxelDataAvailable())
			{
				TSharedPtr<const TArray<FVoxelData>> EditMerged;
				if (EditManager && EditManager->ChunkHasEdits(Coord))
				{
					EditMerged = GetEditMergedSnapshot(Coord);
				}
				else if (CVarFarCompressionSnapshotReads.GetValueOnGameThread() != 0 && State.Descriptor.IsCompact())
				{
					EditMerged = GetSeamVoxelSnapshot(Coord, State);
				}
				TArray<bool> Fresh;
				Fresh.SetNumZeroed(CS * CS);
				FVoxelWaterMesher::BuildColumnMask(EditMerged.IsValid() ? *EditMerged : State.Descriptor.GetVoxelDataForRead(), CS, Fresh);
				FreshWet = 0;
				for (int32 Y = 0; Y < CS; ++Y)
				{
					for (int32 X = 0; X < CS; ++X)
					{
						const int32 I = X + Y * CS;
						FreshWet += Fresh[I] ? 1 : 0;
						if (Stored && (*Stored)[I] != Fresh[I])
						{
							++Stale;
						}
						if (!Stored && Fresh[I])
						{
							++Stale; // a contribution the tile never received
						}
						if (Coord.Z == WaterLayer && !Fresh[I])
						{
							const double WX = Configuration->WorldOrigin.X + (static_cast<double>(Coord.X) * CS + X + 0.5) * VS;
							const double WY = Configuration->WorldOrigin.Y + (static_cast<double>(Coord.Y) * CS + Y + 0.5) * VS;
							FreshMissing += GetGeneratedSurfaceHeight(WX, WY) < WaterZ - VS ? 1 : 0;
						}
					}
				}
			}
			UE_LOG(LogVoxelStreaming, Warning, TEXT("    layer Z %d: state %d, %s (version %u), edits %s, water settled %s, LOD %d (meshed %d); stored mask wet %d; rebuilt wet %d, stale (stored != rebuilt) %d%s"),
				Coord.Z, static_cast<int32>(State.State), Residency, State.Descriptor.ContentVersion,
				(EditManager && EditManager->ChunkHasEdits(Coord)) ? TEXT("yes") : TEXT("no"), State.bWaterSettled ? TEXT("yes") : TEXT("no"),
				State.LODLevel, State.MeshedLODLevel, StoredWet, FreshWet, Stale,
				Coord.Z == WaterLayer ? *FString::Printf(TEXT(", under the plane with no water flag in the voxels %d"), FreshMissing) : TEXT(""));
		}
	}
}

int32 UVoxelChunkManager::ResendWaterTiles()
{
	for (TPair<FIntVector2, FWaterTileState>& Tile : WaterTiles)
	{
		Tile.Value.bDirty = true;
		if (!DirtyWaterTileSet.Contains(Tile.Key))
		{
			DirtyWaterTileQueue.Add(Tile.Key);
			DirtyWaterTileSet.Add(Tile.Key);
		}
	}
	return WaterTiles.Num();
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
		// No more contributors — remove the tile entirely. The far field's band patches over this column drew no sea
		// under it (WM-MP1): a chunk-state version change has them re-select.
		if (MeshRenderer)
		{
			MeshRenderer->RemoveWaterTile(TileCoord);
		}
		WaterTiles.Remove(TileCoord);
		DirtyWaterTileSet.Remove(TileCoord);
		++ChunkStateVersion;
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

	// Its seam jobs and the water pull at its mesh's completion read it and its neighbours: start the far-compressed ones'
	// snapshots decoding while it waits (G65).
	PrefetchVoxelSnapshotsAround(Request.ChunkCoord);

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

void UVoxelChunkManager::ResetBenchCounters()
{
	BenchRemeshCount = 0;
	for (int64& C : BenchRemeshByReason) { C = 0; }
	BenchSeamJobsCompleted = 0;
	BenchUnloadLagSumMs = 0.0;
	BenchUnloadLagMaxMs = 0.0;
	BenchUnloadLagCount = 0;
	BenchUnloadDistSumUU = 0.0;
	BenchUnloadDistMaxUU = 0.0;
	UnloadEnqueueTimeSeconds.Reset();
	if (CVarBenchCountPreBenchMeshes.GetValueOnGameThread() == 0)
	{
		BenchEverMeshed.Reset();
	}
	BenchQueuedRemeshReason.Reset();
}

void UVoxelChunkManager::StartBenchmark(const FVoxelBenchConfig& InConfig)
{
	ActiveBenchmark = MakeUnique<FVoxelStreamingBenchmark>(InConfig, this);
}

// Console: voxel.Bench.Run [tag] [velocityUU] [distanceUU] [key=value ...]
// Starts a deterministic traverse from the current view position and writes a report under
// Saved/VoxelBench/. Optional key=value arguments (':' separates components and '/' separates route
// points, because -ExecCmds splits commands on commas):
//   dir=X:Y[:Z]            straight-path direction (default +X); dir=up: local up at the start (a climb)
//   climb=Lat:Lon[:Alt]    planets: climb along local up from Alt (default 500) over the ground at a latitude and
//                          longitude (spherical Phase 6; the distance is how high it climbs); on a heightmap world
//                          climb=X:Y[:Alt] (world modes review WM-MP11), straight up from the ground at (X, Y)
//   descend=Lat:Lon:Alt    planets: descend along local down from Alt over the ground (spherical Phase 6, WM-BP10:
//                          through the voxel cut-over; the distance is how far down, at most Alt - 300); on a heightmap
//                          world descend=X:Y:Alt
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
	TEXT("Run a streaming benchmark: voxel.Bench.Run [tag] [velocityUU] [distanceUU] [dir=X:Y[:Z]|up] [start=X:Y[:Z]] ")
	TEXT("[route=X:Y[:Z]/X:Y[:Z]/...] [orbit=CX:CY:R] [gc=Lat:Lon:Heading[:Alt]] [climb=Lat:Lon[:Alt] | climb=X:Y[:Alt]] [descend=Lat:Lon:Alt | descend=X:Y:Alt] [follow=AltUU] [warmup=Sec] [catchup=Sec] [build=Label]"),
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

		UVoxelChunkManager* CM = UVoxelChunkManager::Find(TargetWorld, /*bRequireInitialized*/ false);
		if (!CM)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run: no UVoxelChunkManager found in the game world"));
			return;
		}

		FVoxelBenchConfig Config;
		TArray<FString> Positional;
		bool bHasStart = false, bStartHasZ = false, bDistanceGiven = false, bHasGreatCircle = false;
		bool bHasClimb = false, bDirUp = false;
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
			if (Key == TEXT("dir") && Value.Equals(TEXT("up"), ESearchCase::IgnoreCase))
			{
				bDirUp = true; // local up at the start, resolved once the start is known
			}
			else if (Key == TEXT("dir") && ParseBenchVector(Value, Vec, bHasZ))
			{
				Config.Direction = Vec;
			}
			else if (Key == TEXT("climb"))
			{
				// A climb along local up (spherical Phase 6, WM-BP3): from Alt over the generated ground at Lat, Lon; on a
				// heightmap world (WM-MP11) straight up from the ground at (X, Y).
				TArray<FString> Parts;
				Value.ParseIntoArray(Parts, TEXT(":"), true);
				const UVoxelWorldConfiguration* Cfg = CM->GetConfiguration();
				if (Parts.Num() >= 2 && Cfg && Cfg->WorldMode == EWorldMode::SphericalPlanet)
				{
					const double Lat = FCString::Atod(*Parts[0]);
					const double Lon = FCString::Atod(*Parts[1]);
					const FVector Direction = FVoxelStreamingBenchmark::LatLonToDirection(Lat, Lon);
					const FVector Ground = CM->GetGeneratedSurfacePoint(Cfg->WorldOrigin + Direction * Cfg->WorldRadius);
					FVoxelStreamingBenchmark::MakeClimb(Config, Cfg->WorldOrigin, Ground, Parts.Num() > 2 ? FCString::Atof(*Parts[2]) : 500.0f,
						FVector2D(Lat, Lon));
					bHasClimb = true;
				}
				else if (Parts.Num() >= 2 && Cfg)
				{
					const double X = FCString::Atod(*Parts[0]);
					const double Y = FCString::Atod(*Parts[1]);
					const FVector Ground(X, Y, CM->GetGeneratedSurfaceHeight(X, Y));
					FVoxelStreamingBenchmark::MakeClimbAlong(Config, FVector::UpVector, Ground, Parts.Num() > 2 ? FCString::Atof(*Parts[2]) : 500.0f,
						FVector2D(X, Y));
					bHasClimb = true;
				}
				else
				{
					UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run: climb=Lat:Lon[:Alt] (a planet) or climb=X:Y[:Alt] (a heightmap world) needs two values"));
				}
			}
			else if (Key == TEXT("descend"))
			{
				// A descent along local down (spherical Phase 6, WM-BP10): from Alt over the generated ground at Lat, Lon; on a
				// heightmap world (WM-MP11) straight down onto the ground at (X, Y).
				TArray<FString> Parts;
				Value.ParseIntoArray(Parts, TEXT(":"), true);
				const UVoxelWorldConfiguration* Cfg = CM->GetConfiguration();
				if (Parts.Num() >= 3 && Cfg && Cfg->WorldMode == EWorldMode::SphericalPlanet)
				{
					const double Lat = FCString::Atod(*Parts[0]);
					const double Lon = FCString::Atod(*Parts[1]);
					const FVector Direction = FVoxelStreamingBenchmark::LatLonToDirection(Lat, Lon);
					const FVector Ground = CM->GetGeneratedSurfacePoint(Cfg->WorldOrigin + Direction * Cfg->WorldRadius);
					FVoxelStreamingBenchmark::MakeDescent(Config, Cfg->WorldOrigin, Ground, FCString::Atof(*Parts[2]), FVector2D(Lat, Lon));
					bHasClimb = true;
				}
				else if (Parts.Num() >= 3 && Cfg)
				{
					const double X = FCString::Atod(*Parts[0]);
					const double Y = FCString::Atod(*Parts[1]);
					const FVector Ground(X, Y, CM->GetGeneratedSurfaceHeight(X, Y));
					FVoxelStreamingBenchmark::MakeDescentAlong(Config, FVector::UpVector, Ground, FCString::Atof(*Parts[2]), FVector2D(X, Y));
					bHasClimb = true;
				}
				else
				{
					UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Bench.Run: descend=Lat:Lon:Alt (a planet) or descend=X:Y:Alt (a heightmap world) needs three values"));
				}
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
		// A descent stops 300 uu over the ground it started over.
		if (Config.bDescend)
		{
			const float MaxDescent = FMath::Max(Config.ClimbLatLonAltitude.Z - 300.0, 0.0);
			if (!bDistanceGiven || Config.TraverseDistance > MaxDescent)
			{
				Config.TraverseDistance = MaxDescent;
			}
		}

		// Start: an explicit start= (Z defaults to 500 above the analytic surface), else the player view
		// (on the ground, so the leading edge stays at LOD0), else the chunk-manager owner.
		FVector Start = FVector::ZeroVector;
		if (bHasGreatCircle || bHasClimb)
		{
			Start = Config.StartPosition; // the great circle's or the climb's own start
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
		if (bDirUp && !bHasClimb)
		{
			// dir=up: a climb along local up at the start (radial on a planet, +Z on a heightmap world).
			Config.Direction = CM->GetUpVectorAt(Start);
			Config.PathType = EVoxelBenchPath::Straight;
			Config.bClimb = true;
			Config.ClimbLatLonAltitude = FVector(-999.0, -999.0, 0.0); // no latitude: a start= point
		}
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
// mesher CVar change (e.g. voxel.MCBoundaryMorphWidth) takes effect immediately while the LOD rings /
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
		UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.RemeshAll: re-meshing %d chunk(s) across %d manager(s)"), TotalDirtied, Managers);
	}));

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

namespace VoxelEditAuditHelpers
{
	/** The PIE or Game world: the console's own when it is one, else the first PIE/Game world context. */
	UWorld* FindGameWorld(UWorld* World)
	{
		if (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game))
		{
			return World;
		}
		if (GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
				{
					return Ctx.World();
				}
			}
		}
		return nullptr;
	}
}

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
	TEXT("Compare every cached scatter chunk's surface points with a fresh extraction now (world modes review G33: stale ")
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
				[CM](const FIntVector& Coord) { return CM->GetEditMergedSnapshot(Coord); }, Stale);
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
			const IConsoleVariable* Rule = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Scatter.EditReaders"));
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Scatter.EditAudit [%s]: compared %d cached chunk(s) in %.2f s: stale %d (%d changed points; reading an edited chunk %d, edited themselves %d, neither %d); skipped %d pending, %d with a cleared volume, %d without voxels; edits (EditReaders %d): %lld system broadcasts, readers re-extracted %lld, folded %lld, without data %lld, legacy face neighbours %lld; scatter regenerations %lld, extractions launched %lld"),
				*TargetWorld->GetName(), Stats.Compared, Seconds, Stats.Stale, ChangedPoints, ReadsEdited, OwnEdits, Other, Stats.SkippedPending,
				Stats.SkippedCleared, Stats.SkippedNoVoxels, Rule ? Rule->GetInt() : -1, E.Broadcasts, E.ReadersReextracted, E.ReadersFolded,
				E.ReadersWithoutData, E.LegacyFaceNeighbours, ScatterMgr->GetTotalRegenerations(), ScatterMgr->GetTotalExtractionsLaunched());
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

// Console: voxel.Edit.System <x y z | pawn> <radius> [dig|fill=dig] [none|edge|corner]
//          voxel.Edit.SystemLayers <x y z | pawn> [dig|fill=dig] [axes=xyz] [depth=1]
// Debug system edits (world modes review G33), as a POI's terraform makes them (EEditSource::System, one operation); `pawn`
// stands for the player pawn's location:
// - voxel.Edit.System: a sphere brush at (x, y, z) (ApplyBrushEdit, density 255 a voxel at full strength); `edge` moves the
//   centre onto the nearest chunk edge running closest to local up (the edit spans the four chunks around it), `corner`
//   onto the nearest chunk corner (eight).
// - voxel.Edit.SystemLayers: the chunk holding (x, y, z), every voxel of its first `depth` layers on the given min faces
//   (x: local X < depth, and so on) set to air (dig) or to solid (fill, the voxel's material, stone where it was air). Its
//   min corner is a chunk corner, so on a planet the chunks at -X / -Y / -Z read it (G29, G31) though none of them is
//   edited: the edit G33's rule before missed for the edge and corner neighbours.
// Both log the edited chunks and, for each, the chunks whose extraction reads its edited voxels (GetEditReaders).
static void RunDebugSystemEdit(const TArray<FString>& InArgs, UWorld* World, bool bLayers)
{
	const TCHAR* Name = bLayers ? TEXT("voxel.Edit.SystemLayers") : TEXT("voxel.Edit.System");
	UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
	// "pawn" in place of x y z: the player pawn's location.
	TArray<FString> Args = InArgs;
	if (TargetWorld && Args.Num() > 0 && Args[0].Equals(TEXT("pawn"), ESearchCase::IgnoreCase))
	{
		const APlayerController* PC = TargetWorld->GetFirstPlayerController();
		const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("%s: no player pawn"), Name);
			return;
		}
		const FVector L = Pawn->GetActorLocation();
		Args.RemoveAt(0);
		Args.Insert(FString::SanitizeFloat(L.Z), 0);
		Args.Insert(FString::SanitizeFloat(L.Y), 0);
		Args.Insert(FString::SanitizeFloat(L.X), 0);
	}
	if (!TargetWorld || Args.Num() < (bLayers ? 3 : 4))
	{
		UE_LOG(LogVoxelStreaming, Warning, TEXT("%s: %s"), Name, !TargetWorld ? TEXT("no PIE/Game world found (start PIE first)")
			: (bLayers ? TEXT("usage: <x y z | pawn> [dig|fill] [axes=xyz] [depth=1]") : TEXT("usage: <x y z | pawn> radius [dig|fill] [none|edge|corner]")));
		return;
	}
	FVector P(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2]));
	const int32 ModeArg = bLayers ? 3 : 4;
	const bool bFill = Args.Num() > ModeArg && Args[ModeArg].Equals(TEXT("fill"), ESearchCase::IgnoreCase);
	// voxel.Edit.System's snap: "edge" moves the centre onto the chunk edge nearest it that runs closest to local up (the two
	// other axes onto chunk boundaries), "corner" onto the nearest chunk corner.
	const FString Snap = (!bLayers && Args.Num() > 5) ? Args[5].ToLower() : FString(TEXT("none"));
	for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
	{
		UVoxelEditManager* EditMgr = CM ? CM->GetEditManager() : nullptr;
		const UVoxelWorldConfiguration* Config = CM ? CM->GetConfiguration() : nullptr;
		if (!EditMgr || !Config)
		{
			continue;
		}
		TMap<FIntVector, FVoxelEditedVoxelBounds> Edited;
		FDelegateHandle Handle = EditMgr->OnChunkEdited.AddLambda([&Edited, EditMgr](const FIntVector& Chunk, EEditSource, const FVector&, float)
		{
			FIntVector Min, Max;
			FVoxelEditedVoxelBounds& B = Edited.FindOrAdd(Chunk);
			if (EditMgr->GetBroadcastEditBounds(Min, Max))
			{
				B.Add(Min);
				B.Add(Max);
			}
		});
		const EEditSource PrevSource = EditMgr->GetEditSource();
		EditMgr->SetEditSource(EEditSource::System);
		int32 Voxels = 0;
		if (bLayers)
		{
			const FString Axes = Args.Num() > 4 ? Args[4].ToLower() : FString(TEXT("xyz"));
			const int32 Depth = Args.Num() > 5 ? FMath::Max(1, FCString::Atoi(*Args[5])) : 1;
			const int32 CS = Config->ChunkSize;
			const float VS = Config->VoxelSize;
			const FIntVector Chunk = CM->WorldToChunkCoord(P);
			const FVector ChunkMin = Config->WorldOrigin + FVector(Chunk) * Config->GetChunkWorldSize();
			EditMgr->BeginEditOperation(TEXT("Debug system layers"));
			for (int32 Z = 0; Z < CS; ++Z)
			for (int32 Y = 0; Y < CS; ++Y)
			for (int32 X = 0; X < CS; ++X)
			{
				const bool bOnLayer = (Axes.Contains(TEXT("x")) && X < Depth) || (Axes.Contains(TEXT("y")) && Y < Depth) || (Axes.Contains(TEXT("z")) && Z < Depth);
				if (!bOnLayer)
				{
					continue;
				}
				const FVector VoxelPos = ChunkMin + (FVector(X, Y, Z) + FVector(0.5)) * VS;
				const FVoxelData Current = CM->GetEditMergedVoxelAtWorldPosition(VoxelPos);
				const FVoxelData NewData = bFill ? FVoxelData::Solid(Current.IsSolid() ? Current.MaterialID : EVoxelMaterial::Stone, Current.BiomeID) : FVoxelData::Air();
				Voxels += EditMgr->ApplyEdit(VoxelPos, NewData, EEditMode::Set) ? 1 : 0;
			}
			EditMgr->EndEditOperation();
		}
		else
		{
			if (Snap == TEXT("edge") || Snap == TEXT("corner"))
			{
				const double CW = Config->GetChunkWorldSize();
				const FVector Rel = P - Config->WorldOrigin;
				const FVector Up = Config->WorldMode == EWorldMode::SphericalPlanet ? Rel.GetSafeNormal() : FVector::UpVector;
				// The axis along the edge: the one closest to local up (kept for an edge, snapped too for a corner).
				const int32 Along = FMath::Abs(Up.X) >= FMath::Abs(Up.Y) ? (FMath::Abs(Up.X) >= FMath::Abs(Up.Z) ? 0 : 2) : (FMath::Abs(Up.Y) >= FMath::Abs(Up.Z) ? 1 : 2);
				FVector Snapped = Rel;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if (Axis != Along || Snap == TEXT("corner"))
					{
						Snapped[Axis] = FMath::RoundToDouble(Rel[Axis] / CW) * CW;
					}
				}
				P = Config->WorldOrigin + Snapped;
			}
			FVoxelBrushParams Brush;
			Brush.Shape = EVoxelBrushShape::Sphere;
			Brush.Radius = FMath::Max(1.0f, FCString::Atof(*Args[3]));
			Brush.Strength = 1.0f;
			Brush.FalloffType = EVoxelBrushFalloff::Sharp;
			Brush.DensityDelta = 255;
			Brush.MaterialID = EVoxelMaterial::Stone;
			Voxels = EditMgr->ApplyBrushEdit(P, Brush, bFill ? EEditMode::Add : EEditMode::Subtract);
		}
		EditMgr->SetEditSource(PrevSource);
		EditMgr->OnChunkEdited.Remove(Handle);

		UE_LOG(LogVoxelStreaming, Warning, TEXT("%s (%.0f, %.0f, %.0f) %s: %d voxel(s) in %d chunk(s)"), Name, P.X, P.Y, P.Z,
			bFill ? TEXT("fill") : TEXT("dig"), Voxels, Edited.Num());
		const UVoxelScatterManager* ScatterMgr = CM->GetScatterManager();
		for (const TPair<FIntVector, FVoxelEditedVoxelBounds>& Pair : Edited)
		{
			FString ReaderText;
			if (ScatterMgr)
			{
				TArray<FIntVector> Readers;
				ScatterMgr->GetEditReaders(Pair.Key, Pair.Value.IsValid() ? &Pair.Value.Min : nullptr, Readers);
				for (const FIntVector& R : Readers)
				{
					ReaderText += FString::Printf(TEXT(" (%d,%d,%d)%s"), R.X, R.Y, R.Z, Edited.Contains(R) ? TEXT("*") : TEXT(""));
				}
			}
			UE_LOG(LogVoxelStreaming, Warning, TEXT("  chunk (%d,%d,%d) local (%d,%d,%d)-(%d,%d,%d); read by:%s"), Pair.Key.X, Pair.Key.Y, Pair.Key.Z,
				Pair.Value.Min.X, Pair.Value.Min.Y, Pair.Value.Min.Z, Pair.Value.Max.X, Pair.Value.Max.Y, Pair.Value.Max.Z,
				ReaderText.IsEmpty() ? TEXT(" none") : *ReaderText);
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs GVoxelEditSystemCmd(
	TEXT("voxel.Edit.System"),
	TEXT("Debug: a system edit (as a POI terraform) with a sphere brush. Args: <x y z | pawn> radius [dig|fill] [none|edge|corner]. Logs the edited chunks ")
	TEXT("and the chunks whose scatter reads them (world modes review G33)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) { RunDebugSystemEdit(Args, World, false); }));

static FAutoConsoleCommandWithWorldAndArgs GVoxelEditSystemLayersCmd(
	TEXT("voxel.Edit.SystemLayers"),
	TEXT("Debug: a system edit of the min-face layers of the chunk holding a point. Args: <x y z | pawn> [dig|fill] [axes=xyz] [depth=1]. ")
	TEXT("Logs the chunks whose scatter reads it (world modes review G33)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) { RunDebugSystemEdit(Args, World, true); }));

// Console: voxel.Edit.MeshWork [reset]
// World modes review G56 / G57: the meshing work since the last reset (UVoxelChunkManager::GetMeshWorkStats): chunk mesh jobs
// and seam jobs with their worker time, streaming's as well as edits', and what edits asked of the meshing pipeline. To
// measure an edit, hold the viewer still (vox.Freeze 1), reset, edit (voxel.Edit.System), wait for the pipeline to settle and
// log; voxel.Edit.NeighbourRemesh 1 is the rule before for an A/B.
static TMap<TWeakObjectPtr<UVoxelChunkManager>, TPair<UVoxelChunkManager::FMeshWorkStats, double>> GVoxelMeshWorkBaselines;

static FAutoConsoleCommandWithWorldAndArgs GVoxelEditMeshWorkCmd(
	TEXT("voxel.Edit.MeshWork"),
	TEXT("Log the meshing work since the last 'voxel.Edit.MeshWork reset' (world modes review G56): chunk meshes and seam jobs ")
	TEXT("with their worker time, and what edits asked of the meshing pipeline. Arg: reset."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		UWorld* TargetWorld = VoxelEditAuditHelpers::FindGameWorld(World);
		if (!TargetWorld)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Edit.MeshWork: no PIE/Game world found (start PIE first)"));
			return;
		}
		const bool bReset = Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase);
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
			if (!CM)
			{
				continue;
			}
			const UVoxelChunkManager::FMeshWorkStats& Now = CM->GetMeshWorkStats();
			if (bReset)
			{
				GVoxelMeshWorkBaselines.Add(CM, TPair<UVoxelChunkManager::FMeshWorkStats, double>(Now, FPlatformTime::Seconds()));
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Edit.MeshWork [%s]: reset"), *TargetWorld->GetName());
				continue;
			}
			const TPair<UVoxelChunkManager::FMeshWorkStats, double>* Base = GVoxelMeshWorkBaselines.Find(CM);
			const UVoxelChunkManager::FMeshWorkStats B = Base ? Base->Key : UVoxelChunkManager::FMeshWorkStats();
			const double Since = Base ? FPlatformTime::Seconds() - Base->Value : 0.0;
			const IConsoleVariable* Rule = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Edit.NeighbourRemesh"));
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Edit.MeshWork [%s]: %s %.1f s: chunk meshes %lld (worker %.1f ms), seam jobs %lld (worker %.1f ms); edits: %lld broadcasts, %lld edited chunks re-meshed, %lld on their way to a mesh (%lld re-meshed after it landed), %lld face-neighbour re-meshes (voxel.Edit.NeighbourRemesh %d); now: %d to generate, %d to mesh, %d seam jobs in flight"),
				*TargetWorld->GetName(), Base ? TEXT("since reset") : TEXT("since Initialize (no reset),"), Since,
				Now.ChunkMeshJobs - B.ChunkMeshJobs, Now.ChunkMeshWorkerMs - B.ChunkMeshWorkerMs, Now.SeamJobs - B.SeamJobs,
				Now.SeamWorkerMs - B.SeamWorkerMs, Now.EditBroadcasts - B.EditBroadcasts, Now.EditedRemeshes - B.EditedRemeshes,
				Now.EditedOnTheWay - B.EditedOnTheWay, Now.RemeshesAfterMeshing - B.RemeshesAfterMeshing,
				Now.NeighbourRemeshes - B.NeighbourRemeshes, Rule ? Rule->GetInt() : -1, CM->GetPendingGenerationCount(),
				CM->GetPendingMeshingCount(), CM->GetSeamJobsInFlightCount());
		}
	}));

// Console: voxel.Water.Stats [x y | pawn]
// World modes review G58 / G59: what edit-driven water propagation has done (UVoxelWaterPropagation::GetStats: edit scans,
// the chunk-arrival face scans and their seeds, floods, flooded voxels), and with a position the water tile's column there:
// the chunk layers whose voxels hold water in it (edit-merged for a chunk with edits since G59), before the tile's dilation.
static FAutoConsoleCommandWithWorldAndArgs GVoxelWaterStatsCmd(
	TEXT("voxel.Water.Stats"),
	TEXT("Log edit-driven water propagation's counters (world modes review G58 / G59); with 'x y' or 'pawn', also the water ")
	TEXT("tile's column there (the chunk layers that mark it wet)."),
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
				UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: no water propagation (bEnableWaterLevel off)"), *TargetWorld->GetName());
			}
			const UVoxelChunkManager::FWaterRepullStats R = CM->GetWaterRepullStats();
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Water.Stats [%s]: the pull's second passes (G70, voxel.Water.PullAgain): %lld queued, %lld run, %lld flooded (%lld voxels), %lld skipped, %d pending"),
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
// G77: the water tiles against the generated surface (UVoxelChunkManager::AuditWaterTiles).
static FAutoConsoleCommandWithWorldAndArgs GVoxelWaterTileAuditCmd(
	TEXT("voxel.Water.TileAudit"),
	TEXT("G77: audit the 2D water tiles against the generated surface: voxel columns under the water plane that no tile marks ")
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
// G77 diagnostics: mark every water tile dirty so its mesh is generated and sent to the renderer again.
static FAutoConsoleCommandWithWorldAndArgs GVoxelWaterResendTilesCmd(
	TEXT("voxel.Water.ResendTiles"),
	TEXT("G77: send every 2D water tile's mesh to the renderer again (every tile marked dirty)."),
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
		for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(TargetWorld))
		{
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

// Console: voxel.FarCompression.Decodes [reset]
// Where far-compressed chunks were expanded since the last reset (G65): per reader (the FReadScope tags) and per thread
// kind, the count (uniform / compressed) and the time, game-thread waits for worker decodes, and each running chunk
// manager's snapshot decodes this session.
static FAutoConsoleCommand GVoxelFarCompressionDecodesCmd(
	TEXT("voxel.FarCompression.Decodes"),
	TEXT("Log where far-compressed chunk data was expanded (per reader, game thread or worker, and game-thread waits) since "
	     "the last reset. 'reset' clears the statistics."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase))
		{
			VoxelResidencyStats::Reset();
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Decodes: reset"));
			return;
		}
		if (GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				UWorld* World = Ctx.World();
				if (!World || (Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Game))
				{
					continue;
				}
				for (const UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(World))
				{
					if (CM)
					{
						UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Decodes: snapshot decodes on workers (session): %lld"),
							CM->GetSnapshotDecodeCount());
					}
				}
			}
		}
		TArray<FString> Lines;
		VoxelResidencyStats::Describe().ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.FarCompression.Decodes: %s"), *Line);
		}
	}));

// Console: voxel.Snapshot.Stats [reset]
// The seam snapshot cache (world modes review G69): what it holds (by how each entry was made, the reader that made it
// and the last that read it, its age and reads, the chunk's distance and state) and the session's counters.
static FAutoConsoleCommand GVoxelSnapshotStatsCmd(
	TEXT("voxel.Snapshot.Stats"),
	TEXT("Log the seam snapshot cache's contents and session counters (G69). 'reset' clears the counters."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const bool bReset = Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase);
		if (!GEngine)
		{
			return;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* World = Ctx.World();
			if (!World || (Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Game))
			{
				continue;
			}
			for (UVoxelChunkManager* CM : UVoxelChunkManager::FindAll(World))
			{
				if (CM)
				{
					if (bReset)
					{
						CM->ResetSnapshotCacheCounters();
						UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Snapshot.Stats: reset"));
						continue;
					}
					TArray<FString> Lines;
					CM->DescribeSnapshotCache().ParseIntoArrayLines(Lines);
					for (const FString& Line : Lines)
					{
						UE_LOG(LogVoxelStreaming, Warning, TEXT("voxel.Snapshot.Stats: %s"), *Line);
					}
				}
			}
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

// ==================== Queue Re-Prioritization ====================

int32 UVoxelChunkManager::ReprioritizeQueues(const FLODQueryContext& Context)
{
	if (!Configuration)
	{
		return 0;
	}

	const FVector& ViewerPosition = Context.ViewerPosition;
	const float ChunkWorldSize = Configuration->GetChunkWorldSize();
	const FVector WorldOrigin = Configuration->WorldOrigin;
	const float ViewDistance = Configuration->ViewDistance;
	// Add 20% buffer to prevent flip-flopping at ViewDistance boundary
	const float EvictDistanceSq = FMath::Square(ViewDistance * 1.2f);

	int32 EvictedCount = 0;
	int32 LODUpdatedCount = 0;
	// Above a planet's voxel cut-over (WM-BP10) nothing queued is wanted.
	const bool bWindowSuspended = LODStrategy && LODStrategy->IsWindowSuspended();

	// Re-prioritize, update LOD levels, and evict stale items from generation queue
	for (int32 i = GenerationQueue.Num() - 1; i >= 0; --i)
	{
		FChunkLODRequest& Request = GenerationQueue[i];
		const FVector ChunkCenter = WorldOrigin + FVector(Request.ChunkCoord) * ChunkWorldSize + FVector(ChunkWorldSize * 0.5f);
		const float DistSq = FVector::DistSquared(ChunkCenter, ViewerPosition);
		// In range by the strategy's horizon (horizontal in heightmap worlds), not 3D: a viewer high above the terrain
		// would otherwise evict the ground it just requested.
		const float HorizonDistance = LODStrategy ? LODStrategy->GetHorizonDistance(ChunkCenter, ViewerPosition) : FMath::Sqrt(DistSq);

		if (bWindowSuspended || FMath::Square(HorizonDistance) > EvictDistanceSq)
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
	return EvictedCount;
}

// ==================== Collision Mesh Generation ====================

bool UVoxelChunkManager::PrepareCollisionMeshRequest(
	const FIntVector& ChunkCoord,
	int32 LODLevel,
	FVoxelMeshingRequest& OutMeshRequest,
	TSharedPtr<const TArray<FVoxelData>>& OutSharedVoxels,
	TMap<FIntVector, TSharedPtr<const TArray<FVoxelData>>>& OutNeighborSnapshots,
	uint32* OutMissingNeighborMask,
	TArray<UE::Tasks::FTask>* OutSnapshotsReady,
	TMap<FIntVector, TSharedPtr<const FVoxelChunkShell>>* OutNeighborShells)
{
	const VoxelResidencyStats::FReadScope ResidencyReadScope(TEXT("collision"));
	if (OutMissingNeighborMask)
	{
		*OutMissingNeighborMask = 0;
	}
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
	// A far-compressed chunk's snapshot may still be decoding on a worker (G65): with OutSnapshotsReady the cook's worker
	// waits for it; its size is the chunk's once decoded.
	UE::Tasks::FTask OwnDecode;
	OutSharedVoxels = GetSeamVoxelSnapshot(ChunkCoord, *State, OutSnapshotsReady ? &OwnDecode : nullptr);
	if (!OutSharedVoxels.IsValid() || (!OwnDecode.IsValid() && OutSharedVoxels->Num() == 0))
	{
		return false;
	}
	if (OwnDecode.IsValid())
	{
		OutSnapshotsReady->Add(MoveTemp(OwnDecode));
	}
	if (FSeamVoxelSnapshot* OwnEntry = SeamSnapshotCache.Find(ChunkCoord))
	{
		OwnEntry->ReaderMask |= SnapReaderCollisionOwn;
	}

	// Neighbours as boundary shells (G69): the slice extraction reads only GetNeighborDepth planes of each.
	const int32 ShellDepth = VoxelNeighborSlices::GetNeighborDepth(LODLevel, ChunkSize);
	const bool bShells = OutNeighborShells && ShouldUseCollisionShells(ShellDepth);
	if (bShells)
	{
		OutNeighborShells->Reserve(26);
		// A cook reads its own chunk once: cut the chunk's shell from the snapshot for its neighbours' cooks, then drop the
		// snapshot unless another reader (a seam job, a mesh job, the water) took it too. The cook's worker holds the data.
		UE::Tasks::FTask OwnShellReady;
		GetChunkShell(ChunkCoord, *State, ShellDepth, &OwnShellReady);
		if (const FSeamVoxelSnapshot* OwnEntry = SeamSnapshotCache.Find(ChunkCoord))
		{
			if ((OwnEntry->ReaderMask & static_cast<uint8>(~SnapReaderCollisionOwn)) == 0)
			{
				SeamSnapshotCache.Remove(ChunkCoord);
				++SnapshotCounters.ReleasedAfterCook;
			}
		}
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
		bool bHasData = false;
		if (FVoxelChunkState* NState = ChunkStates.Find(NCoord))
		{
			if (NState->Descriptor.HasVoxelDataAvailable() && bShells)
			{
				UE::Tasks::FTask Decode;
				TSharedPtr<const FVoxelChunkShell> Shell = GetChunkShell(NCoord, *NState, ShellDepth, OutSnapshotsReady ? &Decode : nullptr);
				if (Shell.IsValid())
				{
					OutNeighborShells->Add(NCoord, MoveTemp(Shell));
					bHasData = true;
					if (Decode.IsValid())
					{
						OutSnapshotsReady->Add(MoveTemp(Decode));
					}
				}
			}
			else if (NState->Descriptor.HasVoxelDataAvailable())
			{
				UE::Tasks::FTask Decode;
				TSharedPtr<const TArray<FVoxelData>> Snap = GetSeamVoxelSnapshot(NCoord, *NState, OutSnapshotsReady ? &Decode : nullptr);
				if (FSeamVoxelSnapshot* NEntry = SeamSnapshotCache.Find(NCoord))
				{
					NEntry->ReaderMask |= SnapReaderCollisionNeighbor;
				}
				if (Snap.IsValid() && (Decode.IsValid() || Snap->Num() > 0))
				{
					OutNeighborSnapshots.Add(NCoord, MoveTemp(Snap));
					bHasData = true;
					if (Decode.IsValid())
					{
						OutSnapshotsReady->Add(MoveTemp(Decode));
					}
				}
			}
		}
		if (!bHasData && OutMissingNeighborMask)
		{
			*OutMissingNeighborMask |= UVoxelCollisionManager::GetNeighborBit(FIntVector(DX, DY, DZ));
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
