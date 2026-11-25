# Streaming and Seams

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

How `UVoxelChunkManager` (module `VoxelStreaming`) takes a chunk from the LOD strategy's request to a drawn mesh with
collision, and back out; how the boundaries between chunks are meshed; and how workers read voxels. The voxel window
itself (distance bands, the column layer range, the planet shell, the far field, the cut-over, the descent prefetch)
is in [LOD_SYSTEM.md](LOD_SYSTEM.md); the meshers' side of the seam rules (Dual Contouring's mixed-LOD fallback
vertex, Marching Cubes' transition ribbon and geomorph) is in [DUAL_CONTOURING.md](DUAL_CONTOURING.md) and
[MARCHING_CUBES_MESHING.md](MARCHING_CUBES_MESHING.md); the crossfade's renderer side is in
[RENDERING_SYSTEM.md](RENDERING_SYSTEM.md). The July design this model came from is archived as
[SEAM_OWNERSHIP_ARCHITECTURE.md](../Research/Archive/SEAM_OWNERSHIP_ARCHITECTURE.md); several cvars it names are gone.

## The invariant

A chunk mesh is a function of the chunk's own voxels and edits. It is meshed in the Interior domain
(`EVoxelMeshCellDomain::Interior`) and has no vertex in the cells along its +X, +Y and +Z faces. The geometry there
belongs to **seams**: each face, edge and corner between chunks is meshed exactly once, by one job that reads every
participant's voxels. Consequences the rest of the system relies on:

- A chunk never waits for a neighbour to mesh, and never re-meshes because a neighbour arrived or changed. A
  neighbour's arrival restales the seams it takes part in, nothing else.
- Neighbours at different LODs meet in a seam job that knows both LODs, so there is no stitching inside chunk meshes.
- Scatter reads a chunk's voxels, never its mesh (the mesh has no vertex along the max faces: G27).
- Collision is not split this way: a cook meshes the whole chunk (`EVoxelMeshCellDomain::WholeChunk`) with its 26
  neighbours' voxels (see Collision cooking).

## The chunk lifecycle

A chunk's record is `FVoxelChunkState` in `ChunkStates`: its `FChunkDescriptor` (voxels, residency,
`ContentVersion`), its `EChunkState`, the LOD it is wanted at (`LODLevel`) and the LOD its drawn mesh has
(`MeshedLODLevel`). The states:

| State | Means | Leaves by |
|---|---|---|
| `Unloaded` | no record, or generation failed | the strategy asks for it |
| `PendingGeneration` | in `GenerationQueue` | dispatch to a worker or the GPU |
| `Generating` | a CPU worker or a GPU readback is making its voxels | the result is accepted (`OnChunkGenerationComplete`) |
| `PendingMeshing` | in `MeshingQueue` | dispatch to a mesh worker |
| `Meshing` | a worker is meshing it, or its mesh waits in `PendingMeshQueue` for the render submit | the submit (`OnChunkMeshingComplete`) |
| `Loaded` | meshed and drawn | a re-mesh (LOD change, edit) or an unload |
| `PendingUnload` | in `UnloadQueue` | `ProcessUnloadQueue` removes it |

Each queue is a priority-sorted array with a set beside it for duplicate checks (`AddToGenerationQueue`,
`AddToMeshingQueue`, `AddToUnloadQueue`). Worker results come back through MPSC queues (`CompletedGenerationQueue`,
`CompletedMeshQueue`, `CompletedSeamMeshQueue`) drained within a per-tick budget (`VoxelDrainBounded`).

### One tick

`TickComponent` runs, in order:

1. `RecoverLostRenderData`: a recreated scene proxy drops the geometry it held; every `Loaded` chunk it lost is
   re-meshed and the seam buckets are re-sent from the slots the manager keeps (G15); paused, see below (F3.4).
2. The query context (`BuildStreamingContext`): the camera, the streaming viewer ahead of a descending camera
   (`ApplyDescentPrefetch`), the viewer's speed for the speed-adaptive load budget (`voxel.Stream.SpeedAdaptive`).
3. The LOD strategy's update, the cut-over (a suspended window lets every chunk go), the further viewers' ground
   windows (`UpdateGroundWindows`, every tick: a server's other players, below), then the streaming decisions
   when the viewer changed chunk: `UpdateLoadDecisions` (new requests, edit-pinned chunks), `UpdateUnloadDecisions`
   (every tick), `ReprioritizeQueues`.
4. Generation: `ProcessGenerationQueue`, `ProcessPendingGPUReadbacks`, `ProcessCompletedAsyncGenerations`.
5. Meshing: `ProcessMeshingQueue`, `ProcessCompletedAsyncMeshes`, then `CheckStuckAsyncJobs`.
6. Render submit, time-budgeted, from `PendingMeshQueue`; `ProcessUnloadQueue`; the water tiles; the sea undersides;
   the far field's update.
7. Water propagation's flood and the water pull's second passes (`ProcessWaterRepulls`, G70).
8. LOD level changes (`EvaluateLODLevelChanges`, scheduled by `FVoxelLODEvalSchedule`).
9. `TickSeamScheduler` (after everything above, so this tick's residency and rendered-LOD changes are in the registry).
10. `ProcessFarCompressionSweep`, then the collision manager's and the scatter manager's updates
    (`ProcessEditScatterReaders` first), then one `FlushPendingOperations` on the renderer.

While streaming is paused (`SetStreamingEnabled(false)`: `voxel.Stream.Enabled 0`, `vox.Freeze 1 streaming`) the tick
runs `TickWhilePaused` instead: nothing is discovered, generated, unloaded or changes LOD, and a plain pause submits
nothing. Chunks re-meshed in place still are, and they alone of the meshing queue launch (`PausedRemeshChunks`):

- A scene proxy recreated meanwhile (a material or render-state change, a Lumen toggle) gets its geometry back (visual
  polish F3.4): `RecoverLostRenderData` re-meshes the chunks it lost and re-sends the seam buckets. Before, the paused
  tick returned at once and the chunks stayed lost until the resume. Test `Streaming.Cubic.RendererProxyRecreatedWhilePaused`
  (the viewer moved far away while paused loads and unloads nothing).
- `voxel.Mesh.RemeshAll` (`RemeshAllLoadedChunks`, G115) re-meshes every loaded chunk and restales every seam, and
  with streaming paused it enlists the chunks it re-meshes in place (Loaded or on a worker; one already queued for a
  LOD change keeps that request for the resume) and has the paused tick run the seam scheduler, which builds the
  restaled seams. Pausing is how a scene is held for an A/B: before, the paused tick launched none of these, so a
  mesher cvar changed and `RemeshAll` run with streaming frozen showed nothing until the resume (F2.9's bisect read
  that as `RemeshAll` ignoring the cvar). Running, it always re-meshed every interior and seam; the seam registry
  restales a chunk's seams whatever content version it is passed. Test `Streaming.RemeshAll.Planet` / `.Plane` (a mesh
  job per loaded chunk, every chunk mesh and seam bucket submitted again, no residency or LOD change, running and paused).

The meshes that land are submitted (with them the meshes that were on workers when the pause began, which would
otherwise fill the pending queue whose cap stops the launches; a chunk queued again when its mesh lands stays enlisted),
the water tiles are sent again and the renderer's batch is flushed (it draws its updates only then).

A second tick function (`FVoxelFarFieldCameraTickFunction`, `TG_PostUpdateWork`) runs `TickAfterCameraUpdate`: after
a camera cut of more than a chunk it updates the far field and runs the streaming decisions again for the camera this
frame renders with (F4.3, MP1). Each step is
timed into `FVoxelTimingStats`, which the streaming bench reports.

**The tick budget** (G282, `FVoxelTickBudget`, `voxel.Stream.TickBudgetMs`, 4 ms; 0 = the tick before G282). Each
section has a cap of its own, and after a teleport onto fresh ground they added up: on VoxelDemo the tick averaged 8 ms
in the first second and its longest was 27 to 37 ms, made of single pieces of work. The work the player stands on and
sees is not held: the collision cooks' applies and launches and the cooks under and ahead of the pawn, and the meshes'
submits to the renderer, which keep their own caps (the submits 4 ms, the applies `voxel.Collision.MaxAppliesPerFrame`).
What can wait a tick draws on what is left of the tick's budget, at least one piece a section a tick and nearest first,
so the world settles as it does without it: the far field's build launches (each gathers its patch's conditioning and
water on the game thread, 1 to 2 ms a patch; six at once were updates of 5 to 13 ms about once a second, also at rest),
the far field's components of removed patches (hidden at once, destroyed later: a teleport removes the place left's 500
patches, 12 ms of one update), the lakes' and river reaches' meshes (built on the thread pool, their components made
within the budget: one valley river's mesh alone took 10 to 14 ms of a tick) and the collision components of chunks that
left the shell (their data dropped at once). The collision manager also takes a jump of more than a chunk in one tick
as a cut, not speed (Collision cooking, below). Measured with the old and the new path alternated in one editor, six
teleports a side between two sites 90 km apart (`Scripts/qa/stream_probe.py`): in the first two seconds the longest tick
11 to 18 ms against 27 to 34, the mean tick 5.4 to 6.2 ms against 6.4 to 7.0, the time in frames over 33.3 ms 0 to 7 %
against 5 to 12 %; collision under the pawn after 0.26 to 0.32 s against 0.31 to 0.39, 216 to 227 cooks a teleport
against 282 to 307, the last cook after 7.5 to 8.3 s against 7.6 to 8.7, no fall-through. The ten seconds after are
unchanged (a tick of 3.3 to 3.8 ms: the submits, the seams, the far field's walk of its tree), and so is when the window
is full: a cap on those is a trade against how soon the terrain shows, left to the user as D196
(`Documentation/FINDINGS.md`). `voxel.Stream.Probe` records per tick what waited; tests
`VoxelWorlds.Streaming.TickBudget.*` and `VoxelWorlds.FarField.Plane.LaunchBudget` (the held far field settles to the
free one's selection).

### Generation

`ProcessGenerationQueue` builds a request per chunk (`BuildChunkGenerationRequest`: the world mode, the conditioning
over the chunk, `FVoxelConditioning`) and launches it within `voxel.Stream.MaxAsyncGenTasks` and `voxel.Stream.MaxLoadPerFrame`:

- **CPU**: a thread-pool task runs `FVoxelCPUNoiseGenerator`, injects voxel trees on the same worker, and queues the
  result.
- **GPU** (`bUseGPUGeneration` and `voxel.Stream.GPUGeneration` 1; `-VoxelForceCPU` forces the CPU): the compute
  dispatch goes to the render thread with an async readback (`FVoxelGPUNoiseGenerator`), polled each tick in
  `ProcessPendingGPUReadbacks`; the CPU post-passes run after it (`LaunchPostReadbackProcessing`).

Either path scans the result for a uniform chunk (all air, all rock, open sea) as it is made (`bUniform`); the chunk
is then stored collapsed (`FChunkDescriptor::CollapseToUniform`), its readers take a shared array and a point query
reads its value, so after a teleport the window's uniform chunks hold no 1 MB arrays (G67; test
`Streaming.{Plane,Planet}.UniformAtGeneration`).

Only the dispatch that owns a chunk's in-flight slot may complete it: each dispatch is stamped with a serial
(`FAsyncJobStamp`, `AcceptAsyncResult`), so a result for a chunk that unloaded, or that the watchdog re-queued, is
discarded. `OnChunkGenerationComplete` queues the mesh, registers the chunk with the seam registry (its incident
seams restale), tells the collision manager its voxels exist (G32, below) and broadcasts `OnChunkGenerated`.

### Meshing and the submit

`ProcessMeshingQueue` dispatches within `voxel.Stream.MaxAsyncMeshTasks`, `voxel.Stream.MaxMeshDispatchPerFrame` and
`voxel.Stream.MaxPendingMeshes`. A chunk whose previous job is still on a worker keeps its place for a later tick; a
chunk the viewer has left behind the unload distance goes straight to unload (the stale cull, R23). The request
carries the chunk's voxels with its edits merged and nothing of its neighbours; a far-compressed chunk without
edits meshes from its snapshot, decoded on the worker (G65). The meshers are the CPU ones (`FVoxelCPUDualContourMesher`,
`FVoxelCPUMarchingCubesMesher`, `FVoxelCPUCubicMesher`) on the thread pool; Shutdown waits for every job holding a
raw pointer to what it frees, the meshers or the noise generator: chunk meshes, seam jobs, generations and their tree
injection, the collision manager's cooks (`FVoxelWorkerJobGuard`, `WorkerJobs`, `GetWorkerJobs`; G76, G104). A worker
that reads the world mode holds it shared (generation's tree injection, scatter's extraction), so the mode outlives
Shutdown's reset until the last such job ends (G104). The wait gives up after 10 s and logs it.

A finished mesh waits in `PendingMeshQueue` for the time-budgeted submit. `OnChunkMeshingComplete`:

1. submits the mesh to the renderer, held at alpha 0 when it is an LOD change or a faded-in first appearance (see
   The crossfade and LOD holds);
2. hands the chunk's voxels to scatter (`HandOffChunkScatter`; edit-merged when the chunk has edits);
3. records the drawn LOD and tells the registry (`UpdateChunkRenderedLOD`), which restales the incident seams;
4. on the first submit since the voxels arrived, pulls water flags across the faces from settled neighbours
   (`PropagateWaterFromNeighbors`) and queues neighbours to pull again (`QueueWaterRepulls`, G70), then updates the
   chunk's water tile (see [WATER_SYSTEM.md](WATER_SYSTEM.md));
5. sets `Loaded`, broadcasts `OnChunkLoaded`, and re-meshes at once if an edit reached the chunk while this mesh was on
   a worker (`RemeshIfEditedWhileMeshing`, G57).

A mesh that lands for a chunk already re-queued (an LOD change decided while it was on a worker) is still submitted
and still handed to scatter (G79), so the chunk is never invisible in between.

### Unloading and residency

`UpdateUnloadDecisions` queues every settled chunk past the unload distance (1.2 x the load distance, the strategy's
streaming horizon), farthest first; a refused candidate does not use the per-frame budget (R23d). An edited chunk is
pinned: it loads within the load distance even where the strategy's analytic culls would not want it (a build above
the surface band), and stays out to the unload distance (R23e). Above a planet's cut-over edited chunks unload too;
their edits stay in the edit layer.

`ProcessUnloadQueue` removes the chunk's mesh and its seam buckets from the renderer, drops its snapshot, face slabs
and seam slots, its water tile contribution and its scatter, and `RemoveChunkState` unregisters it from the seam
registry. A seam result for an owner that left while the job ran is rejected by the owner's residency epoch
(`FVoxelSeamRegistry::GetResidencyEpoch`, `IsResidencyCurrent`), so a late result never recreates a bucket.

`CheckStuckAsyncJobs` warns about, frees and re-queues a generation or mesh job in flight longer than
`voxel.Stream.MeshStuckSeconds` (10 s).

**The residency audit** (`voxel.Stream.AuditResidency`, `UVoxelChunkManager::AuditResidency`, R23) counts geometry or
pipeline state that outlived its chunk: tracked chunks beyond the unload distance by state, renderer chunks and seam
buckets with no tracked owner, async jobs past the stuck threshold; it also reports the worst dispatch-to-complete
time this session. After streaming settles every count is 0 (`findings=0`); non-zero counts right after a teleport
are expected. `GetResidencySnapshot` (`FVoxelResidencySnapshot`) gives the chunks per LOD and layer and the window's
reach for `vox.World` and the bench.

## Seams

### Ownership

`FVoxelSeamRegistry` (pure C++, no world access; the chunk manager pushes every change into it) tracks seams by
`FVoxelSeamKey`: the owner, the type (`EVoxelSeamType::Face`, `Edge`, `Corner`) and an axis. The owner is the
participant with the minimum coordinate, so both sides of a boundary map to the same key with no negotiation:

| Type | Participants | Axis |
|---|---|---|
| Face | 2: owner and owner + the axis | the face's normal |
| Edge | 4: the 2 x 2 block across the other two axes | the axis the edge runs along |
| Corner | 8: the 2 x 2 x 2 block | unused |

A chunk owns up to seven seams (faces +X, +Y, +Z; the three edges; one corner). Their meshes are kept per owner in
seven slots (`FSeamOwnerSlots`) and submitted as one merged bucket (`SubmitSeamOwnerBucket`), so the scene proxy draws
one seam batch per chunk. A cubic world tracks face seams only (`SetFaceSeamsOnly`): blocks need no edge or corner
geometry.

### Invalidation

A seam is dirty when any participant's `ContentVersion` changes (generation, an edit: `RegisterChunk`,
`UpdateChunkContent`), its rendered LOD changes (`UpdateChunkRenderedLOD`), or a participant loads or unloads.
`ContentVersion` is the chunk's logical content counter: the descriptor bumps it on generation and voxel writes, and
the chunk manager bumps it for an edit (`HandleChunkEdited`), whose voxels live in the edit layer. Water flags do not
bump it (the water pull drops the chunk's cached snapshot instead).

### Scheduling and the job

Only ready seams (every participant resident) enter the scan rotation. Each tick `TickSeamScheduler`:

1. every 64 ticks evicts snapshots and face slabs unread for `voxel.Snapshot.MaxAgeTicks` (128) ticks (workers keep
   what they hold alive through their own references);
2. drains finished jobs (`ProcessCompletedSeamMeshes`, at most `voxel.Seam.MaxSubmitsPerTick`), storing each in its
   owner's slot and submitting the merged bucket;
3. schedules dirty ready seams, nearest first (`ScheduleReadySeams`, `ComputeSeamPriority`: the near tier within
   `voxel.Seam.NearChunkRadius` chunks; at most `voxel.Seam.MaxSchedulePerTick` examined);
4. drains jobs into free in-flight slots (`DrainSeamJobs`, `voxel.Seam.MaxJobsPerTick`, `voxel.Seam.MaxInFlight`) and
   dispatches each (`DispatchSeamJob`);
5. submits deferred buckets whose fade ended and releases the LOD holds whose seams landed (below).

`DispatchSeamJob` requires every participant to have voxel data and a drawn mesh; the participants may differ in LOD,
and the mixed-LOD meshers stitch across them (Dual Contouring's boundary fallback vertex, G17; Marching Cubes' ribbon
and geomorph). The request carries each participant's shared snapshot (built at most once per `ContentVersion`); a
far-compressed participant's is decoded on a worker that the job waits for on its own worker (G65). Marching Cubes'
seam frames read the snapshots in place (`FVoxelMeshingRequest::NeighbourSnapshotViews`; G63).
Seam jobs always run on the CPU through the seam mesher matching the meshing mode (`EnsureSeamMesher`).

A seam re-dirtied while its job is on a worker stays scheduled and is rebuilt once the job finishes
(`NotifySeamJobFinished`), so the newest content is always built (R24).

Tests: `Streaming.SeamRegistry.*` (ownership, coverage, dirty propagation, scheduling, the residency epoch, in-flight
re-dirty), `Streaming.{Plane,Planet}.ClosedSurface` (a streamed world is closed at every boundary and LOD), the
mesher suites' `SeamClosure.*`, `Streaming.Cubic.SeamPath`.

## The crossfade and LOD holds

Every chunk mesh swap (an LOD change, a re-mesh) dither-crossfades from the old mesh to the new over
`voxel.MeshFade.Duration` (0.35 s) on the region component (`UVoxelRegionComponent`, `FVoxelFadeScheduler`); new
geometry at least `voxel.MeshFade.AppearMinDistance` (7,000 uu) from the viewer fades in from nothing, nearer
geometry appears at once (R24 P3). `voxel.MeshFade 0` swaps instantly; `voxel.MeshFade.MaxConcurrent` caps the
fades.

An LOD change rebuilds every seam the chunk touches a frame or more after the interior lands; until they land the
old seams no longer meet the new interior. So the manager holds the new interior at alpha 0
(`ShouldHoldForSeams`, `BeginSeamHold`) with the old interior and seams fully drawn, holds the rebuilt seam buckets the
same way, and releases the set together once those seams have landed: one fade takes old {interior + seams} to new
{interior + seams} (`FVoxelSeamTransitionHolds`, R24 P2). Transitions that share seams (an LOD ring moving) merge into
one group. A group waits at most `voxel.MeshFade.SeamHoldMs` (1,000 ms). A first appearance that fades in waits for
its ready seams the same way (`voxel.MeshFade.AppearHold`). A seam bucket still fading from an earlier slot is
deferred until that fade ends (`DeferBucket`, `FlushDueSeamBuckets`) rather than replaced mid-fade.

An edit's re-mesh reaches the scene proxy within a frame and crossfades like any swap (G74: what looked like an undone
dig was the held tool covering the cell; judge a dig from above). Tests: `Streaming.SeamRegistry.LODChangeHold*`,
`Rendering.Crossfade.*`, `Streaming.EditReachesProxy.{Smooth,Cubic}` (these need a render scene: see
[TESTING_STRATEGY.md](../Guides/TESTING_STRATEGY.md)). `voxel.MeshFade.Stats` logs the fades, holds and what a chunk's
proxy holds.

## How workers read voxels

Workers never read a `FChunkDescriptor` that the game thread may write. They read immutable, shared reads the manager
hands out on the game thread.

**Snapshots.** `GetVoxelSnapshot` returns a chunk's whole voxels with its edits merged, built at most once per
`ContentVersion` and cached in `VoxelSnapshotCache`. Its kind (`ESnapshotKind`) depends on the chunk:

| Kind | When | Cost |
|---|---|---|
| `SharedResident` | resident, no edits | none: the chunk's own array, shared copy-on-write (G71) |
| `CopiedWithEdits` | resident, with edits | a copy with the edits applied, on the game thread |
| `SharedUniform` | uniform | none: one array per value (`UniformSnapshotArrays`; G69) |
| `DecodedCompressed` | far-compressed, no edits | a worker task decodes a copy of the compact form (`FVoxelCompactSource`); the chunk stays compressed (G65) |
| `Copied` | resident, no edits, when the chunk has no array to share | a copy, on the game thread |

Copy-on-write: a chunk's array is `FChunkDescriptor::VoxelArray`; `ShareVoxelData` hands it out, and a write
(`GetVoxelDataMutable`, `SetVoxel`) clones it first while a reader still holds it (`DetachVoxelArray`). The water
pull, the one write that keeps the `ContentVersion`, drops the cache entry before it writes. A shared entry counts no
memory of its own (`GetSnapshotOwnBytes`). Tests: `Compression.SharedArray.{CopyOnWrite,WorkerReads}`,
`Streaming.{Plane,Planet}.SharedResidentSnapshots`, `Compression.Codec.CompactSource`,
`Streaming.{Plane,Planet}.FarReadsOffGameThread`.

A decode is a task the reader waits for on its own worker (seam jobs, cooks, chunk meshes) or, for game-thread
readers, on the game thread. A chunk queued for a mesh or a cook starts its neighbourhood's decodes ahead
(`PrefetchVoxelSnapshotsAround`, `PrefetchCollisionInputs`). Readers that still expand a chunk in place are counted by
`voxel.FarCompression.Decodes`.

**Face slabs.** A collision cook reads only the planes of each neighbour next to the faces, edges and corners it
shares (`VoxelNeighborSlices::GetNeighborDepth`: one plane at collision LOD 0, stride + 1 above). It takes each
neighbour's `FVoxelFaceSlabs` (96 KB at 64³ and depth 1, against 1 MB) from `FaceSlabsCache` (`GetChunkFaceSlabs`;
cut from the resident array, from the edit-merged snapshot, or decoded on a worker; uniform chunks share one per
value and depth), while `8 x depth <= ChunkSize` (`ShouldUseCollisionShells`). A cook drops its own chunk's snapshot
once taken unless another reader took it too (G69; tests
`Compression.ChunkShell.Extract`, `Streaming.{Plane,Planet}.CollisionNeighborShells`).

**Far compression.** A `Loaded` chunk that is not dirty, sits in a distance band of stride
`voxel.FarCompression.MinStride` (4) or more and has been settled `voxel.FarCompression.IdleFrames` (300) frames is
encoded on a worker (`ProcessFarCompressionSweep`, `DispatchFarCompression`; codec `voxel.FarCompression.Codec`) and
held compact (`EVoxelDataResidency::Compressed` or `Uniform`). Readers go through the snapshot or a compact source;
a point read of a uniform chunk reads its value. `voxel.FarCompression.Stats` logs the residency counts and memory;
`voxel.Snapshot.Stats` the cache by kind, reader, age and rendered LOD.

## Collision cooking

`UVoxelCollisionManager` decides which chunks get collision (a radius around the focus with a velocity lead, chunks
ahead and below a falling pawn first; the `voxel.Collision.*` tuning in `Documentation/EDITOR_CONSOLE_GUIDE.md` §3.7)
and cooks them on workers. The focus is the first player's pawn; on a server every other player's pawn is a further
focus with a shell of its own (Further viewers and their ground windows, below). `PrepareCollisionMeshRequest` hands a cook the chunk's snapshot and its 26 neighbours'
face slabs; the cook meshes the whole chunk with the main mesher at the collision LOD. On VoxelDemo (64-voxel
chunks, Dual Contouring, LOD 0, four cooks at once) a cook takes 20 to 21 ms on its worker, 13.6 to 14.3 of them the
mesh and 5 to 6 the Chaos triangle mesh (`voxel.Collision.CookStats`; 75 to 80 ms before G275's fix of the mesher's
working memory, `DUAL_CONTOURING.md`, "Cost"), and a teleport onto fresh ground launches about 470 of them, 175 of
chunks with no surface (G283).

The shell's radius grows with the focus's speed (`voxel.Collision.LeadSeconds`), and a focus that moves more than a chunk
in one tick has jumped (a teleport, a respawn) and gives it no speed (`voxel.Collision.JumpIsCut`, G282). Taken as
speed, a teleport of 90 km grew the radius to 877,518 uu for about a second: the sweep queued every chunk still loaded at
the place left (776 sorted inserts, 24 ms of one tick), cooks were launched there, and the next sweep dropped them and
destroyed the old place's components (13 ms). Now the old place loses its collision data in the jump's tick and its
components go within the tick budget (above); a teleport launches about a quarter fewer cooks and the ground under the
pawn arrives sooner. Tests `TickBudget.Collision.Jump`, `TickBudget.Collision.DeferredDestroys`.

A neighbour without voxel data at cook time is read as air. The cook records those neighbours
(`FChunkCollisionData::MissingNeighborMask`); when one arrives (`OnChunkVoxelDataAvailable`, from
`OnChunkGenerationComplete`) the chunk re-cooks unless both sides of that boundary are all air (exact) or all solid (a
buried wall). Arrivals around a chunk fold into one re-cook once no other missing neighbour is still generating, and
re-cooks queue below every first cook; the old body stays live until the new one replaces it (G32; tests
`Collision.{NeighborArrival,EditNeighbors}`). `voxel.Collision.AuditNeighbors`
lists cooks that read a neighbour as air and are stale.

**A chunk's collision is a static component.** Each cooked chunk is one `UVoxelCollisionComponent` at the chunk's
corner, on the `VoxelCollisionContainer` actor the collision manager spawns (transient, not replicated: every machine
cooks its own). The component and the container's root are static (`EComponentMobility::Static`): a component is
placed before it is registered and a re-cook replaces it, so nothing ever moves one. A character's networking depends
on that. To the engine a Movable component is a dynamic movement base (`MovementBaseUtility::IsDynamicBase`), and a
character standing on one sends and receives its position relative to the base; neither machine can resolve the
other's copy of a chunk's component, so while the components were Movable a client dropped every server correction on
terrain and a dedicated server corrected every move of a client that walked on it (G215: 8,050 corrections in a 261 s
walk of a road, and after a server-side teleport the client's pawn hung 28 uu over the server's). On a static base
both sides send world positions, and the client applies a correction whose base it cannot resolve and finds its own
floor there. Tests `Collision.StaticBase` (a plane, a block world and a planet: every component static, attached to
the static root, at its chunk's corner and no dynamic base, across a re-cook) and, in the editor, the game module's
`VoxelEngine.Replication.TerrainBase` (a dedicated server and a client on `VoxelDemo`: the client walks, the server
moves and teleports its pawn, the client follows).

## Further viewers and their ground windows

The streaming has one **streaming viewer**: the camera of the world's first player controller
(`BuildQueryContext`), with the collision shell centred on that controller's pawn. A game world with no player yet (a
dedicated server before its first player joins, a test world) streams around where its players will spawn: on a planet
the configuration's spawn point over the datum, in the heightmap modes the origin; only the editor's own world (the
preview) follows the level viewport's camera (G224: before, a server in Play In Editor streamed around wherever the
editor's camera was left until a player joined). A client and a standalone game have
no other player. A server has: every other player's pawn is a **further viewer** (`GatherFurtherViewers`: every player
controller of the world but the first; a player with no pawn is none), and each gets a **ground window**, so the server
has ground under each of its players (G217: before, a second player beyond the first's collision radius stood on
nothing on a dedicated or a listen server, the NPCs the server spawned around it had no floor, and its moves were
checked against no terrain).

- **The window** (`UpdateGroundWindows`, every tick after the strategy's update): the chunks the LOD strategy's window
  holds for a viewer standing at the pawn (`IVoxelLODStrategy::GetWindowChunksAround`: the column's slab and the near
  layers on a heightmap world, the chunk's slab and the near band on a planet, the island's footprint, the shell),
  out to the collision shell's radius for that pawn (`UVoxelCollisionManager::GetFocusRadius`: the configured radius
  plus its speed's lead) and `GroundWindowMarginChunks` (2) chunk widths more: a cook reads its 26 neighbours' voxels (a
  chunk's diagonal), and an eighth of a chunk each for the pawn's move and its radius's change before the windows are
  worked out again. The edited chunks within that reach are in it wherever the analytic window puts them (a dungeon's
  stamp under the slab, a build over it), as the streaming viewer's edit pins. A chunk more is kept (the strategy's keep
  window), so a pawn pacing at an edge or changing layer reloads nothing. The cut-over (the streaming viewer's
  altitude) does not empty a ground window.
- **Its chunks are ordinary chunks**: generated, meshed at the LOD their distance from the streaming viewer gives (far
  from it, the coarsest), with their seams, water pull and scatter hand-off, and unloaded as any chunk. Only who wants
  them differs: the load pass queues the window's untracked chunks (priority by the distance from the nearest viewer,
  as every queue's since), and the unload decisions, the generation queue's eviction and the mesh dispatch's stale
  cull leave a chunk of the keep set alone (`IsChunkInGroundWindow`). Far compression judges such a chunk by the
  distance band of its nearest viewer (`IVoxelLODStrategy::GetDistanceBandLODAround`): the chunks around a further
  player stay resident, as the first player's near bands do, and the rest of its window is compressed as the rest of
  the first's is (its cooks decode them on workers, as the first's). So the server reads the same
  things there as under its first player: voxels (water flags for swimming, digs, block breaks), cooked collision,
  scatter. A dedicated server meshes them as it meshes the first player's window; a listen server's host draws a far
  player's disc of coarse chunks over the far field.
- **Collision**: the chunk manager hands the collision manager the further viewers as foci (`SetFurtherFoci`). A chunk
  gets collision within the radius of any focus, cooks in the order of its distance from the nearest, and loses it
  outside every one's; the path coverage runs under each (its velocity is its pawn's).
- **Scatter**: every distance the scatter manager decides by is a chunk's distance from the nearest viewer
  (`UVoxelScatterManager::SetFurtherViewers`, `GetViewerDistance`), so the server has the scatter's collision and what
  a player chops around each player.
- **Nothing is kept per player.** The viewers are gathered every tick and the windows are two sets (the chunks to load,
  the chunks to keep) replaced whole whenever a viewer came, left, moved an eighth of a chunk, changed its radius by as
  much, an edit reached a chunk nobody tracks, or a second passed. A chunk two players need is loaded once; a player
  who dies, respawns or leaves releases what no other viewer needs at the next unload decision; when the first player
  leaves, the next is the streaming viewer. Nothing is replicated and no client's streaming is read.
- **Cost.** A further viewer adds its window's chunks to the server: with the shell radius R and chunks of width W, the
  window's chunks within R + 2 W (R + 3 W kept). On `VoxelDemo` (chunks of 4,800 uu, a view distance of 40,000, a
  shell radius of 20,000) a player far from every other adds 557 to 566 loaded chunks to the server (855 to 875 kept
  at most), about half of a full window (a client holds 980 to 1,110), and about 100 of them resident (its near
  bands): with two players 7.6 km apart a server held 1,570 to 1,670 chunks, 191 or 192 of them resident (220 MB of
  voxel data), about 215 uniform and the rest compressed, once the compression sweep had caught up with the new
  window (two to three minutes after the second player arrived; 40 s after, a run read 438 resident). Players
  standing together share theirs. A steady tick's streaming decisions grow by the unload
  decision's pass over those chunks (each is a candidate of the strategy's every tick, refused by one set lookup): on
  the test worlds 0.006, 0.017 and 0.040 ms a tick with one, two and four viewers on the plane (180 chunks a viewer)
  and 0.011, 0.027 and 0.113 ms on the planet; the windows themselves cost a rebuild a second per viewer (0.002 and
  0.028 ms a tick of those, with four).
- `voxel.Stream.GroundWindows 0` is the first player only, as before (a comparison); `voxel.World.Viewers` logs every
  world's streaming viewer, its further viewers with their shell radius and the chunk under each, and the windows'
  sizes. `SetBenchmarkView`'s fixed view has no further viewer (the bench runs the same with players in the world);
  tests set them with `SetFurtherViewerOverride`.

Tests: `Streaming.FurtherViewers.{Plane,Planet}` (a further viewer's shell is cooked as a streaming viewer's there, from
the same voxels; the streaming viewer's window and shell do not change; a chunk two viewers need is loaded once and
released with the last; edits made before and under a further viewer collide; a walking viewer has collision under its
feet at every tick; far compression takes a ground window's far chunks and none near its viewer; with no further
viewer the world holds what one viewer's did), `Streaming.FurtherViewers.Scatter` (a further viewer's chunks hold the
spawn points a streaming viewer's there do),
`Streaming.FurtherViewers.Players` (the viewers are the players' pawns: join, no pawn, death, respawn, leave, the
first player leaving, the cvar). In a play session: `Scripts/qa/server_ground.py` (a dedicated server and two clients,
or a listen server and a client: the second player walks out of the first's shell and is teleported 7.6 km away, and
stands on the server's own collision).

## What an edit re-meshes

`UVoxelEditManager` broadcasts each edited chunk; `HandleChunkEdited`:

- bumps the chunk's `ContentVersion` and marks it dirty (`MarkChunkDirty`): a `Loaded` chunk re-meshes, its seams
  restale. A chunk on its way to a mesh restales its seams too; a queued mesh reads the edit when it is dispatched, and
  a mesh already on a worker is redone once it lands (`bRemeshAfterMeshing`, G57). An edit in a chunk the manager does
  not track forces a streaming update, which pin-loads it.
- re-meshes no neighbour: their meshes read none of the edited voxels, and the boundary is in the edited chunk's
  seams (G56). Tests `Streaming.EditRemesh.{Plane,Planet}`: after each edit every chunk mesh and seam bucket equals a
  full rebuild. `voxel.Edit.MeshWork` counts the work.
- re-cooks collision: the chunk, its six face neighbours, and the edge and corner neighbours the edit's sphere reaches
  within a cook's neighbour depth (`MarkNeighborsDirtyForEdit`; all of them for an edit of unknown radius, an undo or a
  redo).
- scatter: a player edit clears scatter in the edit's radius at once and never re-extracts; a system or editor edit
  re-extracts the chunk and, once per tick, the chunks whose extraction reads the changed voxels
  (`UVoxelEditManager::GetBroadcastEditBounds`, `UVoxelScatterManager::GetEditReaders`, `ProcessEditScatterReaders`;
  G33). See [SCATTER_SYSTEM.md](SCATTER_SYSTEM.md).
- water: tells `UVoxelWaterPropagation`, which floods from the changed voxels once the chunk's water is settled (G58).

The edit layer, undo and the history budget are in [EDIT_LAYER.md](EDIT_LAYER.md).

## The chunk manager's owners

Code review CR4 moved the parts of the chunk manager that had a home of their own out of `VoxelChunkManager.cpp`
(`Documentation/CODE_REVIEW_PLAN.md` §11 has the responsibility map, §18.1 rows 1-7 the moves). What the manager
holds now:

| Owner | Holds | File |
|---|---|---|
| `FVoxelSeamRegistry` (`SeamRegistry`) | seam keys, dirty state, the ready rotation, the job queue, residency epochs | `VoxelSeamRegistry.h` |
| `FVoxelSeamTransitionHolds` (`SeamHolds`) | held LOD transitions and deferred seam buckets | `VoxelSeamTransitionHolds.h` |
| `FVoxelLODEvalSchedule` | when LOD levels are evaluated | `VoxelLODEvalSchedule.h` |
| `FVoxelStreamingViewerPolicy` (`ViewerPolicy`) | the descent prefetch's lead, the cut-over transitions log (`FVoxelCutoverTransition`) | `VoxelStreamingViewerPolicy.h` |
| `FVoxelWaterTiles` (`WaterTiles`) | the 2D water tiles' masks, dirty queue and renderer tiles | `VoxelWaterTiles.h` |
| `FVoxelWorldBoundaryOwner`, `FVoxelFarOceanOwner`, `FVoxelPlanetOceanOwner`, `FVoxelSeaUnderside` | the edge walls, the far ocean's and planet ocean's components, the sea undersides | `VoxelWorldComponentOwners.h` |
| `FVoxelFarField` + `FVoxelFarFieldPresenter` | the far field's patches (selection and builds) and their components | `VoxelFarField.h`, `VoxelWorldComponentOwners.h` |
| `FVoxelStreamingCounters` (`BenchCounters`), `FVoxelStreamingBenchmark` | the bench's counters and runs (`voxel.Bench.Run`) | `VoxelChunkManager.h`, `VoxelStreamingBenchmark.h` |
| `FVoxelConditioningSources` (`ConditioningSources`) | where the terrain conditioning comes from: the zones added to the manager and the game's conditioner (its zones and lines); its gathers fill an `FVoxelConditioning` | `VoxelConditioningSources.h` |
| `FVoxelTreeQuery` | a cubic world's standing voxel trees | `VoxelTreeQuery.h` |
| `UVoxelCollisionManager`, `UVoxelScatterManager`, `UVoxelWaterPropagation`, `UVoxelEditManager` | collision, scatter, the edit-driven flood, the edit layer | their own headers |

The console commands are in `Source/VoxelStreaming/Private/Diagnostics/` by system (`VoxelWorldConsole.cpp`,
`VoxelCollisionConsole.cpp`, `VoxelScatterConsole.cpp`, `VoxelEditConsole.cpp`, `VoxelWaterConsole.cpp`,
`VoxelSnapshotConsole.cpp`) and the debug stats and audits in `VoxelChunkManagerDiagnostics.cpp`.

Still in the manager: the streaming loop and decisions, generation dispatch and readback, mesh dispatch and submit,
LOD evaluation, chunk states and queues, initialization, the seam scheduler's dispatch, the snapshot and face-slab
cache, the collision request builder, the water pull and far compression's sweep. CR0 sized the snapshot cache, the
request builder and the water pull as a second round once their A/B cvars retire; generation, the seam scheduler and
the sweep need an abstraction over the chunk map first.

## Diagnostics

Every command and cvar is in `Documentation/EDITOR_CONSOLE_GUIDE.md` (§3.2 streaming, §3.5 seams and snapshots, §3.6
far compression, §3.7 collision, §3.8 editing). The ones for this system:

| Command | Shows |
|---|---|
| `voxel.Stream.AuditResidency` | residency findings (0 = healthy) |
| `voxel.World.Stats`, `vox.World` | the manager's stats; the window's reach and the cut-over |
| `voxel.World.Viewers` | every world's streaming viewer, a server's further viewers and their ground windows |
| `voxel.Stream.GroundWindows 0\|1` | 0: the first player only, as before G217 (a comparison) |
| `voxel.Seam.LogLatency N` | seam scheduler latency every N ticks |
| `voxel.Snapshot.Stats [reset]` | the snapshot and face-slab cache |
| `voxel.FarCompression.Stats`, `voxel.FarCompression.Decodes [reset]` | residency by kind; in-place expansions per reader |
| `voxel.Collision.AuditNeighbors` | stale cooks that read a neighbour as air |
| `voxel.Edit.MeshWork [reset]` | meshing work and what edits asked of it |
| `voxel.MeshFade.Stats` | fades, holds, a chunk's proxy state |
| `voxel.Mesh.RemeshAll` | re-mesh every loaded chunk and seam in place, streaming running or paused |
| `voxel.Stream.Enabled 0\|1` | pause or resume streaming |
