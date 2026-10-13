# Streaming Performance & Benchmarking

How streaming performance is measured and tuned in VoxelWorlds, the command-line
knobs available, and the levers explored so far (what shipped, what's a tradeoff,
what's a dead end). Companion to [PERFORMANCE_TARGETS.md](PERFORMANCE_TARGETS.md)
(budgets) and [LOD_SYSTEM.md](LOD_SYSTEM.md) (the distance-band LOD strategy).

The streaming concerns this targets, in order of impact found: the mesh queue
growing under motion, re-mesh **thrash** (chunks re-meshed repeatedly), and
**lazy deletion** (chunks lingering far behind the viewer).

---

## The Benchmark Harness

A deterministic, repeatable measurement of the chunk manager's queue/scheduler
dynamics under motion plus a catch-up phase after the viewer stops. Built to
iterate on streaming perf intelligently rather than by feel.

**Scenario:** a fixed-velocity traverse at low altitude (so the leading
edge streams at LOD0), longer than the initially loaded radius, followed by a
**catch-up** phase that waits for the queues to drain to equilibrium after the
viewer stops. The path is a straight line by default; it can also be a waypoint
route or an orbit, at a fixed altitude or following the terrain. The benchmark teleports the player pawn along the path each tick
(with flying movement) so it never falls through un-meshed terrain.

**Code:** `Source/VoxelStreaming/{Public,Private}/VoxelStreamingBenchmark.{h,cpp}`
(`FVoxelStreamingBenchmark`: phases Warmup -> Traverse -> CatchUp, per-frame
sampler, CSV+JSON report). Chunk-manager hooks live in `VoxelChunkManager`
(`SetBenchmarkView` deterministic streaming origin, thrash counters, unload
lag/distance instrumentation), ticked from `TickComponent`.

### Running it

**PIE (in-editor):**
```
voxel.Bench.Run [tag] [velocityUU] [distanceUU] [key=value ...]
```
A console command; targets the PIE/Game world's chunk manager and starts from the
player view. Example: `voxel.Bench.Run mytest 6000 60000` (6000 uu/s over 60000 uu).

Optional `key=value` arguments. Components are separated by `:` and route points by `/`,
because `-ExecCmds` splits on commas:

| Argument | Effect |
|----------|--------|
| `dir=X:Y[:Z]` | Straight-path direction (default +X) |
| `start=X:Y[:Z]` | Start here instead of the player view; Z defaults to 500 uu above the analytic surface |
| `route=X:Y[:Z]/X:Y[:Z]/...` | Fly a polyline from the start through these points. Distance 0 or omitted = the whole route |
| `orbit=CX:CY:R` | Fly a circle of radius R around (CX, CY), from the point nearest the start. Distance 0 or omitted = one lap |
| `follow=Alt` | Fly Alt uu above the analytic surface (`GetGeneratedSurfaceHeight`) instead of at a fixed Z. Heightmap world modes only |
| `warmup=Sec` | Max settle time before the traverse (default 10). The report's `warmupSec` is how long the initial load took, so a long warmup with distance 0 measures how long a whole finite world takes to build |
| `catchup=Sec` | Max drain time after the traverse (default 90) |
| `build=Label` | Build label recorded in the report (the sweep runner passes the VoxelWorlds commit) |

Examples: an island chord, `voxel.Bench.Run chord 3000 500000 start=-250000:0 dir=1:0 follow=300`;
a perimeter loop, `voxel.Bench.Run loop 3000 0 orbit=0:0:200000 follow=300`; a full-residency
settle, `voxel.Bench.Run settle 0 0 warmup=300`.

**Config override (A/B world modes and meshing modes on one map):** `-VoxelConfig=<ObjectPath>`
on the command line, or the `voxel.ConfigOverride <ObjectPath>` console variable set in the editor
before pressing Play, replaces the voxel world actor's configuration at initialize
(`UVoxelWorldConfiguration::GetOverridePath`). While an override is active the world save key gets
the config's name appended, so override runs never load or overwrite the map's normal save.

**Headless (focus-immune, CPU mesher):**
```
UnrealEditor-Cmd <uproject> /Game/PluginTesting/VoxelWorldsTest -game -nullrhi \
  -VoxelForceCPU -unattended -ExecCmds="voxel.Bench.Run <tag> 6000 60000"
```
`-VoxelForceCPU` forces the CPU mesher (GPU compute can't dispatch under `-nullrhi`).
Let it run, then read the report. **Note:** `-ExecCmds` separates multiple commands
with **commas**, not semicolons.

**Sweep runner:** `Scripts/voxel_bench_sweep.ps1` runs a fresh headless process per
velocity (cold + focus-immune). `-Config` passes `-VoxelConfig=`, `-BenchArgs` passes
`key=value` bench arguments, `-Distance` fixes the traverse length, and `-ExtraArgs` passes
anything else. It records the VoxelWorlds commit as the report's build label and defaults to
the UE 5.8 install on D:.

**Report:** `Saved/VoxelBench/<timestamp>_<tag>.{csv,json}`.

### Report keys

| Key | Meaning |
|-----|---------|
| `peak{Gen,Mesh,Unload}Queue` | Deepest each queue reached |
| `peakLoadedChunks` | Peak resident chunk count |
| `frameMsP50/95/99`, `totalMsP50/95/99` | Frame time / chunk-manager work per frame |
| `catchUpSec` | Time to drain to equilibrium after the viewer stops (the key UX metric) |
| `thrashRemeshCount` + `thrashLOD`/`thrashNeighbor`/`thrashDirty` | Re-mesh requests, attributed by source |
| `unloadLagMean/MaxMs` | Enqueue -> actual-unload dwell (drain latency) |
| `unloadDistMean/MaxUU` | Viewer distance when a chunk is finally removed (lingering) |
| `effMax*` | Resulting scheduler caps (after overrides/adaptive throttle) |
| `run` | What was measured: map, world type, build label, engine, RHI, viewport, `-Voxel*` switches, config path and override, world and meshing mode, mesher, GPU generation/meshing/scatter flags, seed, voxel and chunk size, view distance, LOD band count, `lodEnabled` (effective: false for cubic), `WorldOrigin`, island or planet params, path shape and parameters |
| `warmupSec` / `warmupDrained` | How long the initial load took to settle, and whether it drained (vs hit the warmup timeout) |
| `memory` | Peak and end-of-run MB for voxel data, renderer GPU/CPU, collision, scatter and the process; `peakVoxelSystemsMB` (all voxel systems together); end-of-run resident/uniform/compressed/empty chunk counts |
| `auditFindings` / `auditReport` | `AuditResidency` at the end of catch-up, taken from where the run ended (0 = healthy) |

The CSV adds per-sample memory columns (`VoxelMB`, `EditMB`, `RendCPUMB`, `RendGPUMB`, `CollMB`,
`ScatMB`, `ProcessMB`, `ResidentN`, `UniformN`, `CompressedN`, `EmptyN`), refreshed every 0.25 s.

### Methodology notes (important)

- **Unfocused-PIE frame times are unreliable.** When the PIE window is not the
  foreground window, UE's "Use Less CPU when in Background" throttle + Windows
  background deprioritization starve the game thread and the async pools. Measure
  frame time/throughput via the **headless** path (no editor window = no throttle)
  or with the PIE window deliberately focused. Queue metrics (peakMeshQ, thrash,
  unloadDist) are reliable either way.
- **Each velocity needs a cold start.** Back-to-back same-path runs are confounded:
  the first run's not-yet-deleted chunks still serve the re-traverse. Use a fresh
  process per velocity (the sweep runner does this).
- Headless uses the map's **saved** config unless `-VoxelConfig=` overrides it, so
  PIE(GPU) vs headless(CPU) compare scheduler/queue dynamics, not identical per-job cost.

---

## Tuning Knobs (command-line)

All parsed at chunk-manager `Initialize` (or LOD-strategy `Initialize`); pass on the
editor/game command line. Defaults reflect shipped behaviour.

| Flag | Effect |
|------|--------|
| `-VoxelForceCPU` | Force the CPU mesher (needed under `-nullrhi`). |
| `-VoxelConfig=<ObjectPath>` | Run a different `UVoxelWorldConfiguration` than the map's own (see "Config override" above). |
| `-VoxelMaxAsyncGen/Mesh/LODRemesh/Pending=N` | Override the scheduler caps. |
| `-VoxelPinScheduler` | Disable adaptive throttling (pin the caps). |
| `-VoxelLODScale=N` | Scale the LOD band boundaries within a fixed view distance (A/B the detail/thrash tradeoff). N<1 pulls fine-LOD reach in. |
| `-VoxelLODRefineHyst=N` / `-VoxelLODCoarsenHyst=N` | Override the asymmetric LOD hysteresis margins (chunk-widths). Lower refine = LOD0 lands sooner (more thrash); coarsen damps moving-away churn. Config: `LODRefineHysteresis` / `LODCoarsenHysteresis`. |
| `-VoxelDeepFull` | Restore 2*stride deep neighbour depth (central-difference boundary normals; the pre-stride+1 default). |
| `-VoxelDeepOff` | Drop deep neighbour data to 1 plane (loses the LOD-seam fix; lowest cost). |
| `-VoxelNoStaleCull` | Disable stale-culling (mesh chunks the viewer has already passed; the old behaviour). |

---

## Performance Levers & Findings

Measured headless at v6000 (a deliberately punishing fast low traverse).

| Lever | Status | Effect (v6000) |
|-------|--------|----------------|
| **Stale-cull passed-by chunks** | ✅ Shipped (default on) | **catch-up -47%**, thrash -13%, peak loaded -13% |
| **Geo deep-depth (stride+1) default** | ✅ Shipped (default) | ~14% faster catch-up, watertight, softer boundary normals |
| **Drop premature LOD cascade** | ✅ Shipped | -6% thrash, simpler, more correct |
| LOD band scale (`-VoxelLODScale`) | Tradeoff dial | thrash ∝ sum of band radii; not a free win |
| Asymmetric LOD hysteresis (eager refine) | ✅ Shipped (config, default 0.25/0.5) | LOD0 lands ~at entry vs halfway across; +15% thrash, catch-up flat |
| Scheduler caps | Not the bottleneck | work-bound, not cap-bound (Mesh 4 vs 32 = no change) |
| LOD 2:1 balance + hysteresis | Already a damper | turning it off *raises* thrash +33% |
| Proactive queue sweep | Deferred | would bound peak queue *during* motion |
| **Far-chunk compression** (memory, not speed) | ✅ Shipped (default on; keyed on distance band, so cubic too) | settled voxel data ~1.1 GB -> ~130 MB at 400 m; encode costs ~14 ms manager P95 while moving (PIE) |

### Stale-cull (the headline win)

**Root cause of "lazy deletion":** unloading is already prompt (unload lag ~1.7ms,
the unload queue stays empty, and the unload decision runs every frame). The symptom
was that chunks weren't *reaching* the loaded-then-unloadable state in time: at speed,
a chunk queued while near doesn't finish meshing until the viewer is ~34k uu past it,
then it loads and instantly unloads. The engine was spending ~half the catch-up
window **meshing chunks the viewer had already driven past**.

**Fix:** when dequeuing a chunk to mesh, if it is now beyond the LOD strategy's unload
horizon (`GetUnloadDistance()` = `ViewDistance * UnloadDistanceMultiplier`), route it
straight to unload instead of meshing it — it would be deleted the instant it loaded
anyway. Safe and watertight by construction: culled chunks are beyond the unload
horizon (off-screen), equivalent to a normal region-edge unload. `IVoxelLODStrategy::
GetUnloadDistance()` (default -1 = disabled) gates it per strategy.

### Geo deep-depth default

LOD-seam watertightness needs deep neighbour data `stride` voxels deep; the previous
default went to `2*stride` purely to give central-difference *normals* at boundaries.
`stride+1` is the geometry-watertight minimum, so it is now the default: cheaper mesh
jobs (and ~half the GPU-DC deep upload), with one-sided (softer) boundary normals.
`-VoxelDeepFull` restores the old behaviour. See [LOD_SEAM_INVESTIGATION.md](LOD_SEAM_INVESTIGATION.md).

### LOD band scale — a tradeoff, not a fix

For a planar traverse, total LOD-transition thrash scales ~linearly with the **sum of
band radii** (the fine-LOD reach). So "widening" bands outward *increases* thrash; the
only band-tuning thrash lever is pulling fine detail inward (coarser sooner), which is
a detail/render-distance tradeoff. Width-*equalizing* the bands was tested and
regressed (+39%). The current band layout is already near the tight end. The 2:1
adjacency balance + hysteresis (cvar `voxel.LODBalance`) is a churn **damper** — it
already removes ~25% of raw transitions and cannot be removed without reintroducing
LOD seams.

### Combined result

All three shipped levers together, vs the pre-session baseline (headless v6000):

| Metric | Before | After | Δ |
|--------|--------|-------|---|
| Catch-up after stop | 10.9s | 4.65s | **-57%** |
| Re-mesh thrash | 3590 | 2961 | -18% |
| Peak resident chunks | 883 | 685 | -22% |

### Far-chunk compression (memory)

Settled far chunks keep their ~1 MB voxel array only for rare reads (neighbour planes, re-meshes,
edits, point queries), so a budgeted sweep in `ProcessFarCompressionSweep` compacts them: all-air and
all-solid chunks collapse to one value, the rest are Oodle-encoded (`voxel.FarCompression.Codec`).
Any access decompresses transparently (`FChunkDescriptor::EnsureResident`) and keeps the encoded
buffer, so compacting the chunk again is free. Inspect with `voxel.FarCompression.Stats`; the bench
report's `memory{}` records end-of-run resident / uniform / compressed counts.

A chunk qualifies when it is `Loaded`, clean, settled for `voxel.FarCompression.IdleFrames` (300) and
**far**: its distance band's stride (`1 << LODLevel`) reaches `voxel.FarCompression.MinStride` (4).
With the LOD system on, the band is the chunk's LOD level; with it off (every cubic world) the LOD
strategy maps the chunk's distance onto the same bands (`IVoxelLODStrategy::GetDistanceBandLOD`). With
the demo bands that is everything beyond 14,000 uu, in both modes. See
[LOD_SYSTEM.md](../Features/LOD_SYSTEM.md) "LOD Off and Cubic Meshing".

The legacy boundary path (cubic) reads up to 26 neighbours straight from their descriptors on every
mesh job, so on that path the sweep holds a chunk expanded by such a read resident for `IdleFrames`
before compacting it again; otherwise a neighbour re-mesh cascade decodes the same chunks job after
job. The seam pipeline reads through its snapshot cache and compacts again at once.

Measured on VoxelDemo's 400 m config ([WORLD_MODES_REVIEW_PLAN.md](../WORLD_MODES_REVIEW_PLAN.md)
§12.7): settled resident voxel data 134 MB smooth, 127 MB cubic (1,123 MB uncompressed). The cost is
the encode: up to `voxel.FarCompression.MaxScansPerTick` (4) encodes per tick on the game thread,
about 14 ms of chunk-manager P95 while moving, in both modes (finding G6 there). It is not timed in a
section of its own, so the bench shows it as unattributed manager time.

### Deferred

A **proactive** per-frame sweep of stale generation+mesh queue entries (rather than
only culling at dequeue) would additionally bound the queue *depth* during motion and
save the generation work spent on passed-by chunks. The shipped dequeue-cull reclaims
the mesh *work* (the catch-up win) but stale entries still sit harmlessly at the
low-priority front of the queue until reached.

---

## References

- [PERFORMANCE_TARGETS.md](PERFORMANCE_TARGETS.md) — frame/memory/streaming budgets
- [LOD_SYSTEM.md](LOD_SYSTEM.md) — distance-band LOD strategy, hysteresis, 2:1 balance
- [LOD_SEAM_INVESTIGATION.md](LOD_SEAM_INVESTIGATION.md) — deep neighbour data / watertightness
- [DUAL_CONTOURING.md](DUAL_CONTOURING.md) — the DC mesher the per-job-cost work tunes
