# Seam Ownership Architecture — making chunk boundaries structurally watertight

Status: **DRAFT for review** (2026-07-18). Companion work landed on `feature/lod-transition-polish`:
near-field correction latency tuning (shipped), swap crossfade (§5, **implemented** on this branch).
The DC boundary-weld series (#43/#44) is held in draft — its findings are inputs to this document.

## 1. Problem statement

The engine is *capable* but not *reliably* watertight: seams appear at chunk boundaries for seconds
during streaming/LOD transitions (occasionally until the player stands on them), and mixed-LOD
corners can hole or distort deterministically. One player-visible seam class at a time has been
fixed for two years of project history (MC transvoxel tears, DC LOD seams, material borders, GPU
parity, absent-neighbor clamps, asymmetric corners...) — each fix real, each revealing the next.
That pattern is the tell: the failures are not bugs in the passes, they are properties of the
architecture. Two properties, specifically.

### Fact A — boundary correctness is two independent computations hoping to agree

Every chunk meshes its own boundary from a partial snapshot of its neighbors (face/edge/corner
slices, deep planes, 6 neighbor LODs). Watertightness holds only if both sides independently
compute bit-compatible boundary geometry from different vantage points.

Evidence from the 2026-07-17/18 investigation (see memory `gpu-dc-cpu-parity` and PRs #43/#44):

- Absent-neighbor divergence: GPU read clamped-interior where CPU read Air → deterministic
  see-through boundary holes (#43).
- Corner LOD blindness: the weld pinned cells from FACE LODs only, so the chunk diagonal to a
  coarse neighbor could not know it existed → 4 chunks made 4 different weld decisions at one
  corner → deterministic holes surviving `voxel.RemeshAll` (#44).
- Every repair strategy for the residual traded artifact classes rather than eliminating them:
  fabricated snap positions → protruding fins (worse than holes: scatter covers holes, nothing
  covers protrusions); legitimate feature snaps → pinch-plates (fine cells dragged E voxels);
  distortion-capped snaps → cracks. The live streaming regime then exposed timing variants the
  static tests could not (stale windows healing only on incidental remesh).

The convergent lesson: **mixed-resolution boundary correspondence computed independently from
partial views has no clean fixed point.** Each fix asymptotically approaches "the chunk needs the
neighbor's full data and exact algorithm state" — at which point it is no longer independent
meshing.

### Fact B — consistency is eventual, repaired reactively

A chunk meshes against neighbor state X. The neighbor later loads / changes LOD / changes content.
Correction is reactive: generation-complete fan-out (26 neighbors), LOD-transition cascades
(6 faces), completion-time revalidation (#42), all funneling into a debounced coalescer
(~90 frames, #40) and a priority meshing queue, ending in a **full 64³ chunk remesh** per
correction. The inconsistency window is therefore:

    debounce (~1.5 s far-field) + queue wait (scales with streaming pressure) + remesh + submit

Under a moving load front this is seconds — the reported "seams for a few seconds, occasionally
until the player is standing on them." The model guarantees visible windows whose length grows
exactly when the most meshing is happening. Priority tiers found in the wild (higher pops first):
80 invisible-chunk safety net, 60 revalidation requeue, 50 first-time mesh, 20 LOD cascade,
~1e-4 (0.5 × 1/distance) coalesced neighbor corrections — i.e., the most common correction class
waited behind the entire streaming wave. (Mitigated on this branch; see §4.)

### What the presentation layer can and cannot fix

A swap crossfade (§5) softens *pops* and makes *short* windows imperceptible. It cannot make a
3-second hole acceptable, and it cannot fix Fact A at all. Fact A and Fact B require the
architecture change below.

## 2. The proposal — single-owner seam meshing

Decompose every chunk mesh into **interior** and **seams**, with each physical boundary meshed
exactly once by one job that reads both sides' data.

```
Chunk mesh  =  Interior mesh                    (cells strictly inside the chunk)
            +  6 face-seam meshes    (shared)   (one per face PAIR — owned, not duplicated)
            +  12 edge-seam meshes   (shared)   (one per edge 4-tuple)
            +  8 corner-seam meshes  (shared)   (one per corner 8-tuple)
```

### 2.1 Interior meshes

- Cells whose full sample neighborhood lies inside the chunk's own voxel volume. For DC at
  stride S with gradient reach 2S: cells `[1, GridSize-2]`-ish (exact margin per mesher).
- **Zero dependence on neighbor data.** Meshable the moment the chunk's own voxels exist; never
  invalidated by neighbor loads, neighbor LOD changes, or neighbor edits.
- Consequence for Fact B: the dominant remesh-churn driver (boundary-driven full-chunk remeshes)
  disappears. Interior remeshes happen only for own-data changes (edits) and own-LOD changes.

### 2.2 Seam meshes

- A seam job owns the boundary shell between a chunk pair (face), 4-tuple (edge), or 8-tuple
  (corner): the 1-coarse-cell-thick region straddling the shared plane/line/point.
- **Built from BOTH sides' actual voxel data** (direct read of both descriptors under the
  residency system — no slice snapshots), at the finer of the participating resolutions, with the
  coarse side's cell decomposition respected on its side.
- **One owner, one computation.** Canonical ownership rule: the seam is owned by the
  minimum-coordinate participating chunk (deterministic, no negotiation). The scheduler runs the
  seam job when (a) all participants' voxel data is resident and (b) any participant's
  content-version or rendered LOD changed.
- Stitching contract: the seam mesh must terminate exactly on the interior meshes' outermost
  vertex rings. This is achievable deterministically: the seam job recomputes the interior
  boundary-ring vertices from the same inputs the interior job used (same chunk data, same
  algorithm, same stride) — bit-identical results without communication. The seam job is the ONLY
  place cross-LOD correspondence is computed, and it sees everything, so the entire #43/#44
  problem class (agreement between partial views) is eliminated **by construction** rather than
  by tolerance.
- Fact B consequence: a neighbor change invalidates a *seam*, not a chunk. A face seam at 64²×1
  coarse cells is ~1/64th of a chunk remesh; edge/corner seams are near-free. Correction latency
  drops from "full chunk remesh behind a queue" to "rebuild a strip, trivially prioritizable."
- Absent participant: seam not built; the interior mesh's boundary ring is capped with the
  existing Air-seal behavior (a wall at the streaming frontier — far away by definition).

### 2.3 Mesher applicability

- **DC**: interior pass is the current pipeline restricted to interior cells (weld deleted
  entirely — it exists only to reconcile Fact A). Seam pass = DC over the union shell with known
  correspondence on both rings. The #44 design laws still bind the seam mesher: positions must be
  surface-derived; distortion bounded by own-resolution cells.
- **MC**: identical decomposition; transvoxel transition cells become an implementation detail of
  the face-seam job (which is where they always logically belonged — they exist to stitch two
  known resolutions).
- **Cubic**: trivially compatible (face culling across the pair inside the seam job).

### 2.4 Renderer

- Per-chunk section (interior) + per-seam sections. Seam sections are small; batch them per
  owning chunk to bound section count (interior + up to 7 owned-seam buckets per chunk; in
  practice one merged "owned seams" section per chunk per material is enough).
- Swap semantics unchanged (a seam rebuild swaps only that section) — which also shrinks what a
  crossfade needs to hide.

### 2.5 Migration plan (phased, each phase shippable)

1. **P0 — scheduler groundwork**: per-chunk monotonic ContentVersion (already exists from #42),
   seam registry (ownership map, dirty tracking), seam job scheduling & priorities. No visual
   change (seam jobs produce nothing yet).
2. **P1 — DC face seams**: interior-restricted DC pass + face-seam jobs for same-LOD pairs; weld
   removed for faces. GT/DT suites rewritten to assert seam-mesh closure instead of two-sided
   vertex matching (a strictly simpler property).
3. **P2 — DC mixed-LOD + edges/corners**: the cross-LOD seam mesher (the one hard algorithmic
   piece — but now written once, with full information, instead of N cooperating approximations).
4. **P3 — MC parity**: port transvoxel into the seam job; delete per-chunk transition faces.
5. **P4 — cleanup**: delete slice extraction for render meshing (collision may retain it),
   neighbor-LOD snapshots, cascades that exist only to re-agree boundaries.

Estimated effort: P0-P1 ~1 week; P2 the risk center ~1-2 weeks; P3-P4 ~1 week. The payoff is
categorical, not incremental: the seam bug class closes, and boundary correction cost drops by
~64× making Fact B windows sub-frame-budget near the player.

#### P0 as implemented (branch `feature/seam-ownership-p0`)

Landed as pure scaffolding — **no geometry produced, no visual/behavioural change** (DC suite stays
19/19 green). What shipped:

- **`FVoxelSeamRegistry`** (`Source/VoxelStreaming/{Public,Private}/VoxelSeamRegistry.{h,cpp}`) — a
  plain C++ (no UObject/RHI) registry. Canonical seam identity `FVoxelSeamKey{Owner, Type, Axis}`
  where the owner is the **minimum-coordinate participant** (§2.2), so both sides of any boundary map
  to the same key — no duplication, no gaps. A chunk is incident to 26 seams (6 face / 12 edge / 8
  corner) and owns 7 (the positive-direction seams: 3 face + 3 edge + 1 corner). Holds a per-chunk
  residency/content/LOD mirror, per-seam dirty flag + participant snapshot (mirroring
  `FMeshBoundaryDep`), and a priority-sorted seam job queue with a **stub processor** (logs/counts,
  emits nothing).
- **Wiring into `UVoxelChunkManager`** — dirty tracking hooks the *same* change points the #42
  revalidation fires from: `OnChunkGenerationComplete` (data available → `RegisterChunk`),
  `OnChunkMeshingComplete` (rendered-LOD change → `UpdateChunkRenderedLOD`), `MarkChunkDirty` (edits →
  `UpdateChunkContent`), `RemoveChunkState` (unload → `UnregisterChunk`). A per-tick scheduler
  (`TickSeamScheduler`) moves dirty+all-resident seams into the job queue (near-viewer first) and runs
  the stub processor. Gated by `voxel.Seam.Registry` (default on; scaffolding is a no-op either way),
  with `voxel.Seam.Debug` / `voxel.Seam.NearChunkRadius` / `voxel.Seam.Max{Schedule,Jobs}PerTick`.
- **Tests** — `VoxelWorlds.Streaming.SeamRegistry.{Ownership,Coverage,DirtyPropagation,Scheduling}`
  (headless, pure-logic): min-coordinate ownership determinism, exactly-once boundary coverage across
  a lattice incl. mixed LOD, dirty propagation to precisely the 26 incident seams, and the
  all-participants-resident scheduling gate.

**P1 entry point:** replace the stub body in `FVoxelSeamRegistry::ProcessSeamJobQueue` with the DC
face-seam mesher (same-LOD pairs first), restrict the interior DC pass to interior cells, remove the
face weld, and rewrite GT/DT to assert seam-mesh closure. The scheduler, ownership, dirty tracking,
and job queue are already in place.

#### P1 plan (branch `feature/seam-ownership-p1`, in progress)

**Delivery mode — cvar-gated parallel path.** P1 lands behind `voxel.Seam.Meshing` (default **0**).
At 0 the legacy per-chunk pipeline (boundary layers + weld + cascades) is bit-identical to today and
the existing DT/GT suites keep guarding it. At 1 (dev/test) chunks mesh interior-only and face-seam
jobs fill same-LOD boundaries. The default flips only when seam coverage is complete (P2: mixed-LOD
faces + edge/corner seams) — flipping earlier would leave visible holes at edge/corner rims. The weld
is therefore *bypassed* by the seam path in P1, not deleted (deletion is P4). "Each phase shippable"
= mergeable with zero regression, not default-on.

**Scope.** CPU DC first (headless `-VoxelForceCPU` exercises it end-to-end); the GPU mirror follows
before any default flip. MC = P3. Same-LOD face seams only; mixed-LOD + edges/corners = P2.

**Cell domains** (grounded in `VoxelCPUDualContourMesher.cpp`; `GridSize = ChunkSize/Stride`; cells
`-1..GridSize-1` get QEF vertices; an edge is owned iff its coord ∈ `[0,GridSize)`³; quads reference
the 4 cells at offsets 0/−1 around the edge):

- **Interior domain**: quad-emitting edges restricted to those whose 4 surrounding cells all lie in
  `[0, GridSize-1)`³ — i.e. no quad references a boundary-layer cell (−1 or GridSize−1). Cell solve
  restricted to the same range; weld and skirts skipped. The interior request carries **no neighbour
  data at all**: positions never sample past own voxels (cell GridSize−2's far corner reads voxel
  `ChunkSize−Stride`), and crossing NORMALS whose gradient taps would exit the chunk use the existing
  absent-neighbour Air fallback — one-sided rim normals, the same trade the default geometry-only
  deep depth (`stride+1`) already accepts at boundaries today. Consequence: the interior mesh is a
  pure function of own-chunk content — never invalidated by any neighbour event (Fact B kill).
- **Face-seam domain** (per same-LOD pair, one world-cell slab): the shared cell layer, owner-side
  `C[axis] == GridSize−1` ≡ neighbour-side `C[axis] == −1`. The seam job owns the edges whose
  surrounding cells include ≥1 slab cell of that face **and** no cell of another face's slab (edges
  whose cells span two/three slabs are edge/corner-seam territory — P2; unmeshed under the flag in
  P1, asserted as the only permitted opening). Quads at the slab's inner boundary reference ring
  vertices (owner cells `GridSize−2`, neighbour cells `0`) which the seam job **recomputes
  bit-identically** from each side's own data with the same Air-fallback clamp rule the interior pass
  used — same code, same inputs, same stride ⇒ positional identity without communication (the
  stitching contract of §2.2). Slab-cell QEF/normals read BOTH chunks' full arrays directly (no
  slices, no deep planes, no clamping across the face) — the #43/#44 problem class cannot arise.

**Runtime pipeline** (P0 scaffolding filled in): the stub in `ProcessSeamJobQueue` is replaced by a
callback into the chunk manager, which builds a seam meshing request (both descriptors resident via
`EnsureResident`, edit-merged), dispatches it on the async worker pool, and submits the completed
seam mesh to the renderer as a per-owner seam bucket, independently swappable from the interior mesh
(§2.4). Registry dirty→schedule→process flow is unchanged from P0. Renderer facts that bind the
bucket design (verified in `FVoxelSceneProxy`/`UVoxelWorldComponent`): every proxy/component/fade map
is keyed by bare `FIntVector` with a hard 1:1 chunk→mesh assumption, so seam buckets need their own
parallel keyspace (the water-tile maps are the working precedent) — sharing the chunk key would
collide with `ChunkPreviousMeshes`/`ChunkFadeStates`; seam batches must replicate the per-chunk RVT
clone loop (terrain writes RVT — seams that skip it would crack the RVT-baked ground) and count
against `MaxMeshBatchesPerFrame`, which motivates §2.4's one-merged-bucket-per-owner batching; the
crossfade is opt-in per submit (`bCrossfade`), so seam swaps can simply not fade in v1. The seam
mesher must preserve the adaptive-diagonal quad split (the DT7 T-junction sliver fix).

**Tests.** New `VoxelWorlds.Meshing.DualContour.InteriorDomain.*` (domain restriction: interior mesh
≡ full mesh when the surface avoids boundaries; no interior quad references a slab cell; determinism)
and `...DualContour.SeamClosure.*` (interior A + interior B + face seam is closed over the face
interior — zero open edges except the P1-permitted slab rim; seam ring vertices bit-match interior
ring vertices). Existing DT/GT suites unchanged (they guard the flag-off path until P4).

### 2.6 Risks / open questions

- Seam mesher at mixed LOD is genuinely novel code (P2). Mitigation: it replaces an approach
  empirically shown unable to converge; and it can be developed against the existing DT/GT
  harness geometry with exact-zero assertions (no tolerances).
- Data residency: seam jobs read multiple descriptors; far-compression (#37) must guarantee
  residency for all participants during the job (EnsureResident already exists).
- Draw-call growth: bounded by seam batching (§2.4); measure with the bench.
- Skirts/water/scatter consumers of the current per-chunk mesh: scatter reads chunk meshes —
  seams add small extra meshes; scatter can ignore them (surface extraction is voxel-driven).

## 3. Interim decision

Until seam ownership lands, **do not further patch the two-sided weld** (holes→fins→plates→cracks
was the empirical stop signal). The held #43 (absent-neighbor Air) remains a candidate to merge
independently if live testing clears it — it aligns both sides' *reads*, which is compatible with
either architecture.

## 4. Shipped on this branch — near-field correction latency

The Fact-B window, attacked directly where it hurts (all cvar-tunable):

- `voxel.Stream.NearCorrectionDistance` (default 12000): within it —
- coalesced neighbor corrections use `voxel.Stream.NearCorrectionDebounceFrames` (default 5)
  instead of 90, and enqueue at priority 65 (above first-time meshes at 50) instead of
  `0.5 × 1/distance` (which sat behind the entire load front);
- both LOD-transition cascades and the revalidation requeue likewise promote to 65 near the
  viewer. Far-field behavior is unchanged (coalescing throughput is a far-field win).

Expected effect: a stale boundary within ~2.5 chunks of the player re-meshes within a few frames
plus one mesh dispatch, instead of `1.5 s + queue`. This does not fix Fact A — it shortens Fact B.

## 5. Swap crossfade — design (IMPLEMENTED on this branch)

Goal: soften every per-chunk mesh swap (LOD flips AND correction remeshes), mesher-agnostic.

Engine findings (UE 5.8, verified in source): `FMeshBatch::bDitheredLODTransition` exists, but the
dither alpha is driven by static-mesh view-state fading maps (`StaticMeshFadeOutDitheredLODMap*`,
HLOD visibility states) — **not available to dynamic custom-proxy batches**. Our vertex path is a
layout-compatible engine `FLocalVertexFactory` — no custom shader surface to add a per-section
fade constant without forking the VF.

Therefore the design is material-driven:

1. **Fade material variant**: duplicate of the master terrain material with Blend Mode = Masked,
   `DitheredTemporalAA(FadeAlpha)` → OpacityMask (one scalar param added; the base graph
   untouched). Only *transitioning* chunks use it, bounding the masked-pass cost.
2. **Proxy**: `FVoxelChunkRenderData` gains an optional `Previous` buffer set + fade start time.
   All Update paths move current→previous instead of releasing. `GetDynamicMeshElements` emits
   two batches while fading (previous with 1−t, current with t), then releases previous.
3. **Game thread**: a small MID pool (transitioning chunks only, ~dozens) owned by
   `UVoxelWorldComponent`; per-tick sets each transition's FadeAlpha; the existing (currently
   write-only) per-chunk MorphFactor plumbing is repurposed as the fade-value carrier.
4. Duration ~0.3–0.4 s; cvar-tunable; instant-off cvar for A/B.

Interaction with §2: unchanged and still wanted — seam ownership removes the *artifacts*, the
crossfade removes the *pop* of legitimate LOD resolution changes.

### 5.1 As implemented (this branch)

- **Cvars**: `voxel.MeshFade` (1 = crossfade, 0 = instant swap for A/B) and
  `voxel.MeshFade.Duration` (seconds, default 0.35).
- **Fade material contract**: a Masked duplicate of the terrain master with two scalar params —
  `FadeAlpha` (shared fade progress t) and `FadeInvert` — wired
  `OpacityMask = lerp(DTAA, 1 − DTAA, FadeInvert)` where `DTAA = DitherTemporalAA(FadeAlpha)`.
  Both draws use the SAME t with complementary masks, so every pixel is covered by exactly one
  surface at all times. (The naive "previous gets 1−t" version samples the same dither pattern
  and leaves up to half the pixels uncovered at mid-fade — see-through terrain.)
  Asset: `M_VoxelMaster_Fade` next to the master; `UVoxelWorldComponent` auto-derives
  `<Master>_Fade` by name (or `SetFadeMaterial` explicitly). No fade asset = legacy instant swap.
- **Proxy** (`FVoxelSceneProxy`): all three update paths take a game-thread-decided `bCrossfade`
  flag; a swap moves the outgoing buffers/factory into a `ChunkPreviousMeshes` slot (capped at
  one generation) instead of releasing. `GetDynamicMeshElements` draws previous(out-MID) +
  current(in-MID) while a chunk has both a Previous slot and an attached fade state; RVT clones
  always keep the shared opaque material so dither never punches holes into cached RVT pages.
- **Game thread** (`UVoxelWorldComponent`): swap detection at the three submit entry points; two
  MID pools (in/out — pool split so FadeInvert never needs rewriting on reuse), atlas parameters
  copied per acquisition from the live atlas MID; a tick (enabled only while fades are active)
  advances FadeAlpha on the MIDs, batches bookkeeping alphas through the repurposed
  MorphFactor carrier, and releases each Previous set at t = 1.
- Mesh→empty transitions (chunk remeshed to nothing) still pop — removal is not a swap.

## 6. Recommended sequence

1. (done) Near-field latency tuning — branch `feature/lod-transition-polish`.
2. Crossfade per §5 (~1 day, render-thread care required).
3. LOD0-extension decision: bench `voxel.Bench.Run` with bands 12000/22000/40000 (the live trial
   felt better; cost ~2.9× LOD0 chunks — commit the asset if acceptable).
4. Review this document; if approved, schedule §2 phases P0–P2 as the next major engine effort.
