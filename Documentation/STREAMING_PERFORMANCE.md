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

**Scenario:** a fixed-velocity straight traverse at low altitude (so the leading
edge streams at LOD0), longer than the initially loaded radius, followed by a
**catch-up** phase that waits for the queues to drain to equilibrium after the
viewer stops. The benchmark teleports the player pawn along the path each tick
(with flying movement) so it never falls through un-meshed terrain.

**Code:** `Source/VoxelStreaming/{Public,Private}/VoxelStreamingBenchmark.{h,cpp}`
(`FVoxelStreamingBenchmark`: phases Warmup -> Traverse -> CatchUp, per-frame
sampler, CSV+JSON report). Chunk-manager hooks live in `VoxelChunkManager`
(`SetBenchmarkView` deterministic streaming origin, thrash counters, unload
lag/distance instrumentation), ticked from `TickComponent`.

### Running it

**PIE (in-editor):**
```
voxel.Bench.Run [tag] [velocityUU] [distanceUU]
```
A console command; targets the PIE/Game world's chunk manager and starts from the
player view. Example: `voxel.Bench.Run mytest 6000 60000` (6000 uu/s over 60000 uu).

**Headless (focus-immune, CPU mesher):**
```
UnrealEditor-Cmd <uproject> /Game/PluginTesting/VoxelWorldsTest -game -nullrhi \
  -VoxelForceCPU -unattended -ExecCmds="voxel.Bench.Run <tag> 6000 60000"
```
`-VoxelForceCPU` forces the CPU mesher (GPU compute can't dispatch under `-nullrhi`).
Let it run, then read the report. **Note:** `-ExecCmds` separates multiple commands
with **commas**, not semicolons.

**Sweep runner:** `Scripts/voxel_bench_sweep.ps1` runs a fresh headless process per
velocity (cold + focus-immune) with `-ExtraArgs` pass-through.

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
- Headless uses the map's **saved** config (runtime config assignment doesn't
  persist to `-game`), so PIE(GPU) vs headless(CPU) compare scheduler/queue
  dynamics, not identical per-job cost.

---

## Tuning Knobs (command-line)

All parsed at chunk-manager `Initialize` (or LOD-strategy `Initialize`); pass on the
editor/game command line. Defaults reflect shipped behaviour.

| Flag | Effect |
|------|--------|
| `-VoxelForceCPU` | Force the CPU mesher (needed under `-nullrhi`). |
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
