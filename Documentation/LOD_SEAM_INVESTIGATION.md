# LOD Seam Investigation — Stage 1 Findings (2026-06-10)

> ## ⚠️ CURRENT UNDERSTANDING (2026-06-13, latest) — read this first
>
> There are **two distinct things** that were conflated, plus a real open bug:
>
> 1. **Cave/POV confound (real).** On the gentle test world (`LODSeamTestConfig`,
>    height ~3000–5000) the survey camera sat *below the surface* in the cave layer,
>    so early "void corridor / roofed void" shots were cave interiors, not seams.
>    From proper above-surface vantages that gentle world looks continuous.
>
> 2. **GPU MC has NO LOD-seam handling at all.** `VoxelGPUMarchingCubesMesher` has
>    zero transvoxel/skirt/transition code — raw LOD boundaries. If the shipping
>    config uses GPU MC, LOD borders mismatch in height (worse at higher LOD).
>
> 3. **Real open bug — steep-terrain LOD tearing in CPU MC.** On the actual demo
>    terrain (non-linear noise, steep) meshed with **CPU MC**, the LOD boundary
>    shows **gross floating/torn blocky fragments**
>    (`Saved/Screenshots/Claudius/cpumc_seam_close.png`). It is **deterministic**
>    (survives freeze + full remesh with all neighbors resident) and **identical
>    with seams ON vs OFF** (`cpumc_seam_seamsOFF.png`) — i.e. the transvoxel
>    transition is **not engaging/helping** on the live steep terrain. **This is the
>    user's actual seam and it is NOT yet fixed.**
>
> **Test blind spot:** the watertightness suite (T1–T7) uses a *gentle, linear-in-Z*
> field, so coarse/fine sampling produce identical heights → it cannot reproduce the
> steep-terrain tearing. T6/T7 show the transvoxel strip matching the coarse neighbor
> perfectly (`unmatchedB=0`) in that gentle field; the only residual there is minor
> zero-gap T-junctions. **A steep / non-linear reproducing test is the missing piece.**
>
> What the work *did* land (committed, valid): the watertightness test suite, the
> `voxel.LogBoundaryResidency` diagnostic, P2-A (mesh-against-phantom-data
> elimination), P2-B (LOD adjacency balance + hysteresis), and P3-interim (Pass-2
> boundary-slab ownership cutting CPU-MC transvoxel T-junctions 28 → 0.58 units).
> The Stage-1 headline below is **superseded** (T2/T3 proved same-LOD strided
> meshing watertight).
>
> **Open / next:** root-cause why CPU-MC transvoxel doesn't engage on steep terrain
> (is `TransitionFaces` set at these boundaries live? are transition cells generated
> and do they hold on steep slopes?), build a steep/non-linear reproducing test, and
> fix. GPU MC needs its own seam handling. See "Live CPU-MC findings" section below.

## Headline (Stage 1 — superseded, see correction above)

**The "transvoxel seam" problem is misattributed. LOD > 0 marching cubes meshing
produces torn, displaced geometry even between same-LOD chunks** — with no LOD
transition involved at all. Transvoxel sits on top of that broken foundation and
partially papers over it. Fixing transition cells first would be building on sand.

## Evidence (screenshots in `Saved/Screenshots/Claudius/survey_*.png`)

| # | Capture | Finding |
|---|---------|---------|
| 03 | Cardinal sweeps from LOD0 zone | Perforated band of torn geometry along the entire LOD0/LOD1 ring (~7000 from viewer); clean LOD0 inside, coherent LOD1 beyond; floating scatter trees above the band |
| 04/07 | Close-up at +X boundary (x≈6400, y=0) | LOD0 sheet ends; void corridor with floating MC fragments; LOD1 surface offset/exposed as cross-section wall. **Identical before/after forced remesh** → deterministic |
| 08 | Precision top-down over the band | The void is *roofed* — thin terrain lid hides the hollow corridor (explains why top-down views look mostly fine) |
| 11 | Same angle, `bEnableLODSeams=false` | **Far worse** — terrain in tatters. Transition cells/skirts are currently load-bearing patches over the deeper defect |
| 12/13 | Deep LOD2-only zone (x≈15000), seams off | **Same-LOD (LOD2↔LOD2) borders are shredded**: floating surface fragments, straight chunk-plane tears, vertical displacement between adjacent chunks' surfaces. Identical after remesh with all neighbors present → deterministic mesher output |

## Mesher internals (from `bDebugLogTransitionCells` capture)

- LOD0 boundary chunk (1,0,0), +X transition face: 95 transition cells processed,
  23 generated geometry, 71 empty (case 0 → fine-MC fallback). Plausible counts;
  the transition strip itself is only ~1 fine cell (100 units) deep — **the visible
  voids are 10–20× wider than the strip**, so they are not the skipped boundary layer.
- Transition masks on ring-edge chunks flip between remeshes (0x0A → 0x02) with
  small viewer moves: LOD bands (~3000–6000 wide vs 3200-unit chunks) make
  ring-edge chunks bistable. Band widths or hysteresis need attention later.

## Ruled out

- **Chunk streaming/state**: all chunks `Loaded`; remesh pipeline works; voids are
  inside submitted meshes.
- **Stale meshing order**: forced remesh with all neighbors present reproduces the
  damage exactly.
- **Generation data mismatch**: all three world-mode CPU generation paths sample at
  base `VoxelSize` with full-res 32³ data regardless of LOD (`GetEffectiveVoxelSize()`
  exists in `VoxelNoiseTypes.h` but is dead code — never called). LOD0 and LOD1
  chunks see the same density field.

## Stage 2 corrections & code findings (2026-06-11)

**Correction to the same-LOD claim:** recomputing band distances for the frozen
viewer (~0,0,900) shows the survey_12/13 tear plane at x=12800 separates chunk
(3,0,1) at ~11,970 (LOD1) from chunk (4,0,1) at ~15,000 (LOD2) — an LOD1|LOD2
boundary, not LOD2|LOD2. The unified symptom across all captures: **at every
coarser|finer LOD boundary, the boundary-adjacent geometry on one side is missing
or displaced**, with nothing patching it in seams-off mode. A true same-LOD tear
has not yet been isolated; verify with a controlled capture before assuming basic
stride meshing is sound.

Code facts established (`VoxelCPUMarchingCubesMesher.cpp`, `VoxelChunkManager.cpp`):

1. `GetVoxelAt()` (mesher ~line 821) handles out-of-bounds lookups only ONE
   plane/edge deep and ignores how far out the coordinate is. Gradient normals at
   LOD stride sample `±Stride` beyond corners (e.g. X=36 at stride 4), which
   silently returns the *plane-32 slice value* — wrong sample, corrupts boundary
   normals (cosmetic, not the tear cause).
2. When a face/edge slice is absent, `GetVoxelAt` silently falls back to the
   clamped own-chunk voxel (duplicate plane) — and `ExtractNeighborEdgeSlices`'
   `GetNeighborVoxel` returns **Air** when the neighbor's CPU `VoxelData` array
   isn't resident (`VoxelChunkManager.cpp:2760`). Both fallbacks displace the
   iso-surface in boundary cubes with no warning. `VoxelData` appears to be
   retained except on generation failure, but residency at meshing time has not
   been verified empirically.
3. `MeshRequest.NeighborLODLevels[i] = NeighborState->MeshedLODLevel` (rendered,
   not target LOD) — fine for seams, but nothing forces a chunk whose *own*
   MeshedLODLevel ≠ LODLevel to re-mesh once neighbors settle; stale-mesh windows
   at LOD flip zones may leave a chunk rendering coarser content than its slot.

Next concrete step: a **VoxelMeshing automation test** that builds two synthetic
adjacent chunks (flat density surface), meshes them at equal and at differing
LODs with properly extracted neighbor slices, and asserts boundary vertices are
watertight. That isolates mesher math from streaming/state effects and turns the
remaining hypotheses (slice content vs. request assembly vs. stale meshes) into
pass/fail facts.

## Two-chunk watertightness test plan (Stage 2, next step)

New file: `Source/VoxelMeshing/Tests/MarchingCubesLODBoundaryTests.cpp`
(follow the harness pattern in `MarchingCubesMeshingTests.cpp`, which already
has an LOD0 `ChunkBoundary` test).

**Fixture (shared helpers):**
- Analytic density field in world space (flat plane `z = h`, plus a sloped-plane
  variant so the surface crosses the shared chunk face). Fill two adjacent 32³
  chunks A (origin 0) and B (origin x=3200) from the same function — ground
  truth, no streaming involved.
- Slice builder that fills `FVoxelMeshingRequest` neighbor face/edge arrays in
  the production layout (`NeighborXPos[Y + Z*ChunkSize]` = B's plane 0, etc.)
  and sets `EdgeCornerFlags`/`NeighborLODLevels` the way the chunk manager does.
- Boundary-vertex collector + matcher: gather A's vertices with |x−3200| < ε and
  B's with |x−0| < ε (transformed to world), assert one-to-one positional match
  within tolerance; report max mismatch distance and unmatched count on failure.

**Test cases — each converts a hypothesis to pass/fail:**

| # | Setup | Expected | Decides |
|---|-------|----------|---------|
| T1 | A,B both LOD0, full slices | pass (baseline) | harness sanity |
| T2 | both LOD1 (stride 2), full slices, seams off | pass | is same-LOD strided boundary meshing sound? |
| T3 | both LOD2 (stride 4), full slices, seams off | pass | same, at stride 4 |
| T4 | A LOD0, B LOD1, seams off | cracks ≤ 1 fine cell | true magnitude of the raw LOD mismatch; gross displacement (≥ 1 coarse cell) ⇒ mesher math bug |
| T5 | T2 with neighbor arrays left empty | expected-fail; quantify displacement | documents the silent Air/clamp fallback hazard |
| T6 | A LOD0 + TransitionFaces(+X), B LOD1 | initially expected-fail | becomes the transvoxel acceptance test later |

**Run loop:** headless via
`UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests VoxelWorlds.Meshing.MarchingCubes.LODBoundary; Quit" -unattended -nullrhi`
— seconds per iteration, no PIE needed.

**Results (2026-06-11, tests implemented; T5b/T5c variants added 2026-06-13):**

| # | Result | Measured |
|---|--------|----------|
| T1 | ✅ pass | 47/47 boundary verts, exact match (0.000) |
| T2 | ✅ pass | 21/21, exact match (0.000) — stride-2 same-LOD boundary meshing is watertight |
| T3 | ✅ pass | 10/10, exact match (0.000) — stride-4 watertight |
| T4 | ✅ pass | max crack 28.1 units (0.28 fine cells) — raw LOD mismatch is small T-junction cracks, NOT gross displacement |
| T5 | ✅ pass (clamp hazard, gentle) | empty slices → clamp duplicate-plane displaces boundary ~19 units (0.19 voxel) on a gentle field — small, but the magnitude is terrain-dependent |
| T5c | ✅ pass (clamp hazard, gross) | **same confirmed empty-slice clamp path on a steep field**: tear ≥ 1 coarse cell (200+ units) — the clamp fallback *alone* reproduces the void-corridor / exposed-cross-section field artifact, no Air-fill required |
| T5b | ✅ pass (Air-race hazard, gross) | distinct path: a slice that is allocated-but-ungenerated (all Air) — 1022-vert wall, tear spanning the full face (3124 units). Plausible only if generation races meshing; unconfirmed pending P1 instrumentation |
| T6 | ❌ fail (expected) | B's 21 coarse verts ALL matched by A's transition cells (outer-edge alignment works), but A emits 28 extra fine verts on the plane up to 28 units off B's seam — fine boundary MC cells leak alongside transition cells (Pass-2 skip / empty-transition-cell MC fallback) |

**Conclusion per the decision tree:** T2/T3 pass and T4 is sane ⇒ same-LOD strided
boundary mesher math is SOUND. The gross field tears are a **request-assembly**
problem, not mesher math. There are two distinct assembly hazards, and only the
first is confirmed in code:

1. **Confirmed (clamp duplicate-plane).** `ExtractNeighborEdgeSlices` guards every
   face/edge/corner extraction with `HasNeighborData`, so a *non-resident*
   neighbor leaves the slice **empty** (it is NOT Air-filled). `GetVoxelAt` then
   clamps the out-of-bounds plane to the chunk's own edge voxel. T5/T5c show this
   alone produces tears whose size scales with terrain steepness — small on flat
   ground, ≥ 1 coarse cell on cliffs (matching the captures).
2. **Plausible, unconfirmed (Air-fill race).** `GetNeighborVoxel`'s `Air()` branch
   (`VoxelChunkManager.cpp:2779`) and a copied all-Air slice only occur if a
   neighbor's `VoxelData` is *allocated but not yet generated* at meshing time.
   T5b shows this would be gross, but whether the race actually happens needs the
   live instrumentation in P1.

Both point at the same fix area: a boundary chunk must not mesh against phantom
data. Next: instrument `VoxelChunkManager` request assembly (P1), then replace the
silent fallback with defer-or-warn (P2). T6's fine-cell leak is the transvoxel-side
follow-up (P3).

Run: `& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" VoxelEngine.uproject -ExecCmds="Automation RunTests VoxelWorlds.Meshing.MarchingCubes.LODBoundary; Quit" -unattended -nullrhi` (exit code is nonzero while T6 is an intentional red acceptance gate).

## P1 — live-repro instrumentation (2026-06-13)

Added a CVar-gated diagnostic at request-assembly time
(`VoxelChunkManager.cpp`, just before `LaunchAsyncMeshGeneration`; enable with
`voxel.LogBoundaryResidency 1|2`). For each meshed chunk it logs, per boundary
face: whether the neighbor slice is resident (`res=Y/N`), the neighbor LOD, the
neighbor slice's solid count, the chunk's own boundary-plane solid count, and a
`CLAMP` / `AIRRACE` flag. Repro: `LODSeamTestConfig` on the `VoxelWorldsTest`
map, PIE, stream-settle, then a frozen forced-remesh of the +X LOD-ring chunks.
Screenshots `Saved/Screenshots/Claudius/p1_plusx_boundary.png` (torn) and
`p1_plusx_after_remesh.png` (still torn).

**Findings (counts over one streamed session, ~7,900 chunk-mesh events):**

| Signal | Count | Verdict |
|--------|-------|---------|
| **Genuine clamp** (neighbor in `ChunkStates` with a `MeshedLODLevel`, but its `VoxelData` NOT resident → duplicate-plane clamp) | **584 faces** | **Confirmed dominant hazard** |
| Benign world-edge clamp (`nbrLOD=-1`, no neighbor at all) | 1197 faces | Expected, not a defect |
| `AIRRACE` (resident neighbor, all-Air slice, own plane has terrain) | 30 faces, **max `ownSolid` = 6/1024** | **Not substantiated** — all are terrain grazing a face corner (false positives); no large-mismatch case |
| Adjacent chunks ≥ 2 LOD levels apart (by neighbor *rendered* `MeshedLODLevel`) | 1387 events | ⚠️ Misleading metric — see P2-B correction below: this is rendered-LOD lag/churn, NOT a target-assignment defect |

**Decision:** Of the two candidate request-assembly hazards, the **clamp path is
the live mechanism** (neighbor known but data not resident at mesh time). The
**Air-fill race (T5b) is not occurring** in practice — the only all-Air resident
slices are where the surface barely grazes the face (≤ 6/1024 voxels), which is
legitimate, not a race.

**Two new facts the test plan did not predict:**
1. **The tear persists after a forced remesh with all neighbors resident**
   (`res=Y`, `sliceSolid ≈ ownSolid` on every face, yet the boundary is still
   torn in `p1_plusx_after_remesh.png`). So the clamp is only half the story:
   chunks meshed once against a missing neighbor are never refreshed (stale mesh)
   — re-meshing them with good data does not happen automatically.
2. **LOD churn.** The ring chunks re-meshed at flipping LODs (0/1/2) across a
   single pass, and the `nbrLOD`-gap count was high (1387). At the time this read
   like 2-level *target* gaps; **P2-B disproved that** (see below). The flipping is
   real (bistability) but the gaps are rendered-LOD lag, not target misassignment.

**Revised P2 (originally two prongs; P2-B re-scoped after measurement):**
- (A) **Residency:** a boundary chunk must not mesh against non-resident neighbor
  data — defer the mesh until neighbors are resident, and re-mesh on neighbor
  arrival / LOD change (kills the 584 clamp faces and the stale-mesh persistence).
  Replace the silent clamp with a `Warning` so regressions are visible.
  *(Implemented as P2-A — see below. NOTE: this was expected to be "the tear fix"
  but the seam audit found there was no catastrophic tear; P2-A still eliminates a
  real transient defect.)*
- (B) **LOD stability:** a 2:1 adjacency invariant + hysteresis. Implemented and
  measured (below) — it is a correctness safeguard + churn reduction, but **not**
  the tear fix; the current distance bands already satisfy 2:1 by geometry.

## P2-B — LOD adjacency balance + hysteresis (2026-06-13)

Implemented in `FDistanceBandLODStrategy`: `Update()` builds a `BalancedLODCache`
(raw band LOD → hysteresis vs last frame → 2:1 refine-to-fixpoint), and
`GetLODForChunk`/`GetVisibleChunks` read it so all manager call sites agree.
Toggle with `voxel.LODBalance` (default 1).

**A/B over a settled session (`voxel.LogBoundaryResidency 2`, wide forced remesh):**

| Metric | Balance OFF | Balance ON |
|--------|-------------|------------|
| `nbrLOD` gap (own LOD vs neighbor *rendered* `MeshedLODLevel`) | 645 / 1336 | 336 / 1316 |
| **target-vs-target** adjacency (own meshed LOD vs neighbors' meshed LOD) | **0** | **0** |
| Screenshot (`p2b_balance_off.png` vs `p2b_balance_on.png`) | identical | identical *(both were sub-surface/cave POV — see the CORRECTION at the top; not an LOD seam)* |

**Correction to P1:** target-LOD adjacency is **already ≤ 1 without balancing**.
With band widths (6000–7000) wider than a chunk (3200), two face-adjacent chunk
centers cannot span a full band, so distance-band targets differ by at most one
level **by geometry**. The P1 "1387 ≥2-level gaps" was `MeshedLODLevel` lag/churn,
not a target defect — an over-interpretation of that metric.

**What P2-B is worth (kept, not the fix):**
- *Correctness invariant:* nothing else guarantees the LOD strategy respects the
  mesher's single-level-transition limit. No-op for these wide bands, but protects
  narrower-band configs and the planned quadtree/octree strategies (which *can*
  produce 2-level gaps). CVar-gated, off-switchable.
- *Churn reduction:* hysteresis cut rendered-LOD-lag gaps ~48% (645→336) with a
  stationary viewer — less remesh thrash and transient popping.

At the time, the identical off/on screenshots were read as "the tear is
request-assembly (P2-A)." The seam audit later showed those screenshots were both
sub-surface/cave POV, so they show no seam either way — see P2-A below.

## P2-A — defer meshing against non-resident neighbors (2026-06-13)

Implemented in `UVoxelChunkManager`:
- `ProcessMeshingQueue` now calls `ShouldDeferMeshForNeighbors` before meshing a
  chunk. If a face neighbor exists in `ChunkStates` but its `VoxelData` is still in
  the generation pipeline (`PendingGeneration`/`Generating`), the chunk is deferred
  (kept `PendingMeshing`, re-queued after the loop) instead of meshing its boundary
  against a clamped duplicate plane. A per-frame `Examined` cap bounds the work.
- If a neighbor lacks data but it is *not* coming (Loaded-but-freed / unloading),
  the chunk meshes anyway (no permanent stall) with a one-line `Warning`.
- Convergence: a chunk with resident own-data never blocks a neighbor (defer only
  waits on *generating* neighbors), so there is no deadlock; the existing
  `QueueNeighborsForRemesh` (Loaded neighbors) plus the per-frame deferred retry
  cover re-mesh-on-arrival.

**Live result (`voxel.LogBoundaryResidency 2`):** genuine clamp faces (neighbor
known, data not resident) **584 → 0** over a streamed session; 0 "data-not-coming"
warnings; the ~18 remaining clamps are benign world-edge (no neighbor exists yet).

**Honest scope:** P2-A eliminates a real defect — meshing against phantom neighbor
data, which produced *transient* boundary glitches during streaming — but it does
**not** change the steady-state look, because (per the CORRECTION at the top) there
was no catastrophic tear to begin with; the dramatic shots were cave/POV. P2-A is
kept as correctness + it removed the residency confound that made the seam audit
trustworthy.

## Seam audit (2026-06-13) — the decisive visual check

Proper above-surface vantages across both rings, seams ON and OFF
(`Saved/Screenshots/Claudius/audit_*.png`):

| Ring | Seams ON | Seams OFF |
|------|----------|-----------|
| LOD0/1 (~7000) | continuous | continuous — no visible crack |
| LOD1/2 (~13000) | continuous | continuous (close-up clean; faint wide-shot lines were chunk-bound debug draw + scatter) |

**Conclusion (for the gentle test world only):** no catastrophic seam on the gentle
`LODSeamTestConfig` terrain. **⚠️ SUPERSEDED for the real terrain** — see "Live
CPU-MC findings" below: on the actual steep/non-linear demo terrain the LOD boundary
*does* tear grossly. The audit above used the gentle world, which doesn't exhibit it.

## Live CPU-MC findings on real terrain (2026-06-13)

Set the test actor to a CPU-MC config derived from the user's `DemoWorldConfig`
(`MeshingMode=MarchingCubes`, `use_gpu_meshing=False`, seams ON, morph OFF,
water/scatter/caves OFF for a clean surface, `BaseHeight=500 HeightScale=2000` for
viewability — still non-linear noise terrain). Screenshots:
`Saved/Screenshots/Claudius/cpumc_seam_close.png` (seams ON),
`cpumc_seam_seamsOFF.png`, `cpumc_after_remesh.png`.

**Observations:**
- The LOD boundary shows **gross floating/torn blocky fragments** — real geometry
  tearing, not hairline T-junctions. (This is what Stage 1 originally saw; it was
  *partly real*, not only the cave/POV confound.)
- **Deterministic:** persists unchanged after freeze + full forced remesh with all
  neighbors resident → not stale-mesh/churn (so P2-A/P2-B don't address it).
- **Seams ON ≈ seams OFF:** the transvoxel transition is **not engaging or not
  helping** on the live steep terrain. This is the key lead.
- Reproduces only with **steep/non-linear** terrain; the gentle linear-Z test field
  cannot exhibit it (coarse and fine interpolate to the same height).

**Why the tests passed anyway:** the watertightness suite's analytic field is linear
in Z, so coarse/fine sampling agree exactly. T6/T7 therefore show the transvoxel
strip matching the coarse neighbor perfectly (`unmatchedB=0`, max-nearest 0.00) with
only minor zero-gap T-junctions (`unmatchedA>0`). The suite has a **field blind
spot** for steep terrain.

**P3-interim (landed):** Pass 2 now skips the entire boundary slab on active
transition faces (the coarse transition cells own it), cutting CPU-MC T-junction
crack from ~28 → 0.58 units. T6 re-asserted on `unmatchedB==0` + max-crack ≤ 1.0;
the residual T-junctions (`unmatchedA`) are logged as a known limitation. **This is
not the steep-terrain fragment fix.**

## Transvoxel-engagement diagnostic + spurious-TF fix (2026-06-13)

Extended `voxel.LogBoundaryResidency` to log `TransitionFaces` (TF) per chunk and
flag faces that *should* transition (active boundary + coarser neighbor) but don't
(`NOTF!`), and "spurious" TF (set on a same/finer-LOD neighbor).

**Findings from the live CPU-MC repro:**
- Transvoxel **does engage** — 104/135 chunks had TF set, `missingTF=0` (no genuine
  coarser boundary was left without a transition). So the seam is **not** a
  trigger/engagement failure (earlier hypothesis disproved).
- **Spurious TF was rampant: 2097** surface-bearing face-occurrences had TF set on a
  **same-or-finer** LOD neighbor. Cause: the trigger fired on `bNeighborTransitioning`
  (neighbor rendered LOD ≠ target), not just on coarser neighbors. With P3's Pass-2
  skip, those degenerate transition cells (CoarserStride == Stride) replaced regular
  MC on same-LOD boundaries.
- **Fix (landed):** set `TransitionFaces` only for genuinely coarser neighbors
  (`MeshedLODLevel > CurrentLOD`); dropped spurious TF 2097 → **0**, genuine
  transitions preserved (32).

**BUT the visible artifact is unchanged** (`cpumc_TFfix.png` vs `cpumc_seam_close.png`).

### Correcting the visual target (user clarification)
The "blocky fragments" are just the **sand/grass/dirt biome material patches** —
normal. The actual artifact is the **thin blue slivers/cracks** showing the
background *through* the terrain, **mostly horizontal**, at chunk borders, present
**with and without seams**. These are genuine watertightness gaps.

### Decisive localization: LOD-transition-specific
Disabling LOD entirely (`enable_lod=False`, all chunks LOD0) makes the blue cracks
**completely vanish** — the surface is continuous
(`Saved/Screenshots/Claudius/cpumc_LODoff_grazing.png`). Therefore:

- **The cracks occur ONLY at boundaries between chunks of different LOD level**
  (none at same-LOD boundaries — consistent with T2/T3 watertight).
- They are **height mismatches**: a "horizontal sliver" is the vertical gap where
  the fine and coarse surfaces meet at different heights at the LOD boundary.
- **Transvoxel engages there** (32 genuine-TF chunks, missingTF=0) **but does not
  close the gap** on steep/non-linear terrain — identical with seams ON and OFF.
- Scales with terrain steepness; the gentle linear-Z test field makes coarse/fine
  heights identical, so T6/T7 cannot reproduce it (the suite's blind spot).

Screenshots: `cpumc_seam_close.png` / `cpumc_sliver_closeup.png` (cracks, LOD on),
`cpumc_topdown_pattern.png` (cracks along chunk-grid lines, worst at the LOD band),
`cpumc_LODoff_grazing.png` (no cracks, LOD off).

### T8 — non-linear-Z reproducing test: NEGATIVE result (2026-06-13)

Added `ETestField::NonLinearZ` (tanh density profile through the surface, so the
mesher's linear interpolation lands the iso-crossing at a **stride-dependent
height** — the real-terrain condition) and T8, which runs the transvoxel
acceptance across ±X/±Y at LOD0|LOD1 and +X at LOD1|LOD2 with that field.

**Result: T8 PASSES — `unmatchedB=0`, `maxNearestVert=0.00` on every case.** Even
on non-linear/steep terrain, the **single-face transvoxel transition matches the
coarse neighbor's boundary heights exactly.** So the core transition meshing is
**sound** — the live LOD-boundary cracks are **not** caused by the transition math
diverging on steep terrain. (Only the minor zero-gap T-junctions remain,
`unmatchedA>0`, as in T6.)

This redirects the hunt: the deterministic single-face mesher is correct, so the
live bug must be in the **multi-chunk context** the unit tests don't model.

### T9 — mesh HOLE detection: DETERMINISTIC REPRO (2026-06-13) ★

The earlier tests checked boundary **vertex matching**; an "opening larger than the
character" is **missing geometry (a hole)**, which vertex-matching can't see. T9
detects holes via manifold-ness: count mesh edges used by exactly one triangle that
are NOT on a chunk face (degenerate triangles dropped first). Pure LOD0 = 0 such
edges (watertight); the transvoxel transition strip = **58–64**.

| Field | Transition strip open edges | Pure LOD0 |
|-------|-----------------------------|-----------|
| Smooth | **64** | 0 |
| NonLinearZ | **64** | 0 |
| Cliff (steep) | **58** | 0 |

**This is the first deterministic reproduction of the actual bug.** Key points:
- The transvoxel transition strip is **not manifold-watertight** — it leaves ~60
  open edges (hole rims) the pure LOD0 mesh doesn't have.
- It reproduces **even on the smooth field**, so the holes are ALWAYS present at LOD
  transitions. They are near-zero-gap on gentle terrain (why earlier above-surface
  audits looked continuous and T6's crack was 0.58), but on **steep terrain the fine
  surface deviates from the straight coarse transition edges between coarse corners,
  opening the holes wide** — the visible "openings larger than the character."
- This reconciles "visually obvious" with "all vertex tests passed": the boundary
  vertices are placed correctly (T6/T8 = 0.00) but the strip is stitched with
  T-junctions + a coarse inner connection, not a shared-vertex manifold.

**Root cause:** the transition cell's high-res (9-sample) face is placed on the
BOUNDARY (toward the coarse neighbor) and overridden to coarse, while the 4-corner
(coarse) face is toward the FINE interior — inverted from Lengyel. So the outer face
T-junctions against the coarse grid and the inner face under-tessellates against the
fine interior, both leaving open edges.

**Fix plan (now test-driven via T9 + T6/T8):** put the coarse 4-corner face toward
the coarse neighbor (matches B exactly, no T-junctions) and the high-res 9-sample
face toward the fine interior (shares the fine MC's boundary vertices), i.e. the
correct Lengyel orientation; remove the midpoint-override now that the coarse side is
genuinely 4-corner. Validate: T9 → 0 open edges, T6/T8 stay watertight, then confirm
live on steep terrain.

T9 is committed RED as the failing repro / known-issue gate until the fix lands.

### Orientation fix landed (2026-06-14) — holes + winding FIXED, B-match residual remains ★

Applied the planned re-orientation:
- `TransitionCellSampleOffsets` (all 6 faces): flipped the depth axis so the fine
  9-sample face is on the INNER (fine-interior) side and the 4 coarse corners (9-12)
  are on the OUTER boundary (toward the coarser neighbour). This is a pure reflection
  along each face's depth axis vs the old tables.
- Removed the masking hacks: the coarse midpoint-override in `GetTransitionCellDensities`,
  and the fin-snapping + outer-boundary-projection in `ProcessTransitionCell` (plain
  `InterpolateEdge` now).
- Toggled `FaceNeedsWindingReverse` (reflection flips handedness) → `{F,F,T,T,F,F}`.
- `VertexOnOuterFace` now means "both endpoints are coarse corners (9-12)".
- Added **T10** (winding/orientation): counts back-facing strip triangles vs the field
  gradient; no other test catches a flipped (inside-out, back-face-culled) strip.

Results (Smooth/NonLinearZ/Cliff):

| Test | Before | After |
|------|--------|-------|
| T9 holes (+X strip) | 58–64 | **0** ✓ |
| T10 winding (+X strip back-faces) | n/a | **0** ✓ |
| T1–T5c | pass | pass |
| T6/T7/T8 A↔B vertex match | pass (via hacks) | **regressed** |

**The primary bug is fixed:** the transition strip is now manifold-watertight (0 open
edges) and correctly wound — the big "openings larger than the character" are gone.

**Remaining residual (T6/T7/T8):** A boundary-dump (T11, since removed) showed A's coarse
contour is the SAME curve as the coarse neighbour B and matches it EXACTLY on most
vertices — but A computes the contour's TOPOLOGY from the fine face sampled one fine
cell INWARD of the boundary (the strip is one fine cell thick). On sloped/curved terrain
that ~1-cell depth offset shifts the sampled height ~15 units, and near a coarse-cell
corner it can FLIP whether an edge crosses, so A occasionally drops/adds a vertex B has
(e.g. -X: B has `(2317,1800)`, A jumps `(2200,1822)→(2400,1800)` — an ~83-unit apparent
gap that is really a thin near-corner sliver). +X happened to have no such flips (only
the ~15-unit offset), which is why it nearly passed.

**This is the inherent limit of the thick-cell transition:** a single 9-bit case driving
BOTH the inner fine contour and the outer coarse contour cannot match B's boundary-plane
topology when the field varies across the cell depth. Confirmed two ways:
- Forcing corner densities to the fine-corner values (Lengyel invariant `d[0x9]=d[0]`…)
  keeps topology self-consistent but moves the coarse contour OFF B → ALL verts unmatched.
- Using boundary corner densities (current) matches B's POSITIONS exactly where topology
  agrees, but topology still comes from the inner-plane case → near-corner flips.

**True-watertight fix (next):** move to the proper zero-thickness Lengyel transition —
the transition cell sits IN the boundary plane (corners 9-12 coincident with fine corners
0,2,6,8, same densities), the finer chunk's regular MC runs to the boundary (no boundary-
slab skip) producing the fine boundary contour, and the transition cell is the flat
fine→coarse reduction ribbon. No depth offset → coarse topology == B (exact match) AND
the fine face == fine MC (no holes). This is an architecture change to the two-pass
skip; T9/T10 (hard gates) + T6/T7/T8 (B-match) make it test-driven.

**Open issues / next steps:**
1. **Transvoxel transition strip leaves holes (PRIMARY — deterministic repro in T9).**
   Fix per the plan above (correct Lengyel orientation). Earlier multi-chunk
   hypotheses below are now secondary — the core transition strip itself is the bug:
   (a) **LOD-state / transition-decision timing.** A boundary chunk meshes a
       transition for the neighbor's `MeshedLODLevel` *at mesh time*; rendered-LOD
       lag/churn can mean the fine chunk got no transition (or a wrong-stride one)
       relative to the neighbor's eventual rendered LOD. (But: cracks persisted
       after freeze + full remesh with settled LODs — so re-check whether the
       forced remesh actually rebuilt TF from the *settled* MeshedLODLevels.)
   (b) **Multi-face / corner interactions.** The live LOD ring curves, so chunks
       border coarser neighbors on 2+ faces; corner cells are left to regular MC by
       both passes and may gap. T8 only tests single-face transitions.
   (c) **2-level rendered gaps.** P2-B balances *target* LODs; rendered
       `MeshedLODLevel` can still be 2 levels apart transiently → transition built
       for 1-level is wrong.
   (d) **Rendering** (least likely given it survives remesh): overlapping/stale
       per-chunk meshes.
   Next probe: instrument the *specific* visible-crack chunks (their rendered LOD,
   neighbor rendered LODs, and TF) and/or compare the two bordering chunks' actual
   submitted boundary vertices live.
2. **GPU MC has no seam handling** — needs transvoxel/skirt added to the GPU shader
   if GPU MC is a shipping path.
3. **CPU-MC T-junctions** (minor) — coarse-2×2 boundary-face restructure for strict 1:1.
4. **DC LOD seams** — separate investigation (`BuildLODMergeMap` + DC shader).
5. **Spurious-TF fix (DONE)** — landed; eliminates degenerate same-LOD transition cells.

**Decision tree after first run:**
- T2/T3 fail → fix `ProcessCubeLOD`/`GetVoxelAt` strided boundary math first.
- T2/T3 pass and T4 is sane → mesher math is fine; move instrumentation into
  `VoxelChunkManager` request assembly (log slice residency + `MeshedLODLevel`
  staleness at meshing time during a live repro).
- Either way, replace the silent fallbacks surfaced by T5/T5c with an explicit
  defer-or-warn path (a chunk should not mesh its boundary against duplicated
  edge planes or phantom air).

## Where to look next (Stage 2 reframed)

Priority 1 — **same-LOD strided boundary meshing** in
`VoxelCPUMarchingCubesMesher` (`ProcessCubeLOD` + neighbor data consumption):
- Boundary cubes at stride S span voxels `ChunkSize-S .. ChunkSize`, requiring
  neighbor data at plane 32 (and beyond, for gradient normals at 33+). Verify
  `ExtractNeighborEdgeSlices` provides what strided cubes and gradient normals
  actually index, and what `GetVoxel` returns when it doesn't (air fallback would
  mesh boundary cubes against a false air wall → exactly the observed tears).
- The vertical displacement between adjacent chunks' surfaces suggests possible
  coordinate/stride errors for the boundary row, not just missing data.
- Note `49b015b` ("eliminate instance of missing chunks") was already fighting
  this class of bug.

Priority 2 — only after same-LOD borders are watertight: revisit LOD0↔LOD1
transitions (transvoxel or skirt fallback per face), then LOD band hysteresis.

## Repro protocol (uses CLAUDIUS pie_camera/pie_screenshot, see CLAUDE.md)

1. Test config: `/Game/PluginTesting/LODSeamTestConfig` (MarchingCubes, CPU,
   LOD on, morphing off, seed 555111, `BaseHeight=3000`, `HeightScale=2000`
   → land guaranteed at origin; LOD bands 0–7000 / 7000–13000 / 13000–20000).
   Note: `enable_lod_seams` was left **false** after the A/B test — re-enable for
   transvoxel testing.
2. Helper scripts in `Saved/`: `claudius_setup_lodtest.py` (assign config to the
   `VoxelWorldsTest` map's test actor, in memory), `claudius_freeze.py` /
   `claudius_unfreeze.py` (pin chunk streaming so the LOD ring stays put while the
   survey camera flies), `claudius_debug_remesh.py` (enable transition logging +
   mark chunks dirty; needs a brief unfreeze to process).
3. Start PIE → settle → freeze → `pie_camera`/`pie_screenshot`.
   Reference vantage for the +X boundary artifact: camera (5200, 0, 3200),
   pitch −18, yaw 0, FOV 75. LOD2 same-LOD artifact: (15000, 0, 5500), pitch −20.
