# LOD Seam Geomorph (Transvoxel Secondary Positions) — Implementation Plan

**Project**: VoxelWorlds Plugin (UE 5.8)
**Target**: `VoxelCPUMarchingCubesMesher` (CPU, increment 1) + `MarchingCubesMeshGeneration.usf` `MainCS` (GPU, increment 2)
**Status**: ✅ IMPLEMENTED (CPU + GPU). This plan is retained as the design record; the geomorph
seam handling described here shipped in commits `ec2976b` (MC LOD-seam geomorph, CPU) and `332c8a6`
(GPU MC LOD-seam geomorph port + `voxel.RemeshAll`). Original status: Planned 2026-06-23.

---

## 1. Problem & confirmed root cause

The remaining MC LOD-seam artifact is a **vertical step** at every LOD ring — terrain "dips/rises
carving cube-shaped features", the raised/dropped edge alternating sides along the border in a
sine-wave. It is **not a crack and not a bug**: it is the inherent C0 height discontinuity of
discrete LOD, made *opaque and vertical* by the zero-thickness transvoxel ribbon that sealed the
cracks in the prior pass.

**Confirmed live (2026-06-23, GPU MC, `CPUDCSeamTestConfig`):** at an identical camera, seams ON =
opaque vertical step walls; seams OFF = see-through cracks at the *same* LOD boundaries. Same height
gap — walled vs unwalled. Pushing the LOD bands out (#3) relocates and shrinks the step but does not
remove it (relocation, not cure; costs near-field LOD0 triangles). See
`LOD_SEAM_INVESTIGATION.md` for the full history and `[[mc-lod-step-rootcause]]` memory.

### Geometry of the step
The boundary plane (e.g. X = ChunkSize) is vertical. The fine surface (X &lt; boundary) crosses it
along the **fine contour** `C_fine(Y)`; the coarse neighbour (X &gt; boundary) crosses it along the
**coarse contour** `C_coarse(Y)`. Coarse-stride sampling lands the iso-crossing at a different height,
so `C_fine ≠ C_coarse`. The ribbon is a flat lens *in* the boundary plane bridging the two contours —
because it has zero depth, the height gap renders as a vertical curtain. **That curtain is the step.**

To remove it we must make `C_fine ≈ C_coarse` at the seam by **bending the fine surface toward the
coarse surface as it approaches the boundary** — a ramp, not a cliff.

---

## 2. Why the existing morph scaffolding is unusable

`VoxelVertexFactory.ush` has `ApplyLODMorph()` (line ~187) but:
- it snaps to a coarse **world grid** (`floor(pos/grid+0.5)*grid`) — morphs toward a blocky lattice,
  **not** toward the coarse neighbour's surface (wrong target for seams);
- it is **never called** — `VertexFactoryGetWorldPosition` (line ~338) returns the raw position;
- the C++ `LODMorphFactor`/`LODGridSize` shader params never reach the `.ush` (it uses `static` defaults);
- `FVoxelCustomVFRenderer::UpdateLODTransition` (VoxelCustomVFRenderer.cpp ~L523) early-returns (stubbed).

So re-enabling the existing path would not fix the seam. We design the correct mechanism instead and
leave that dead path alone (it is removed/replaced only in the optional v2).

---

## 3. The fix: baked Transvoxel secondary positions (a ramp)

**Key insight:** the engine **re-meshes a chunk whenever its own or a neighbour's LOD changes**, so the
secondary positions can be **baked into the mesh at generation time** — no per-vertex morph target, no
vertex-format change, no shader change, no morph-factor plumbing for v1. The morph is a spatial
relationship ("match the coarser neighbour"), independent of camera distance, so a static bake is correct.

### Algorithm (per fine MC vertex)
For each vertex emitted by regular MC in the **boundary slab** — cells within a morph width `W`
(coarse cells) of an **active transition face** (a face whose `TransitionMask` bit is set, i.e. it
borders a coarser neighbour):

```
d = distance from the vertex to the transition face, in coarse cells (E = CoarserStride voxels)
w = saturate((W - d) / W)            // 1 at the seam, 0 at depth W, linear ramp
primary   = the normal fine MC vertex position (today's output)
secondary = the fine vertex pulled onto the COARSE iso-surface (see below)
final     = lerp(primary, secondary, w)
```

- **At the seam (`w = 1`)** the vertex lands on the coarse surface the ribbon already matches to the
  neighbour → `C_fine → C_coarse` → ribbon collapses to ~zero height → **no wall**. Watertightness with
  the neighbour is preserved by construction (the boundary is still defined by the coarse contour).
- **At depth `W` (`w = 0`)** the vertex is unchanged fine detail. The ramp lives in between.
- **`W`** is the tuning knob (start at **2** coarse cells; larger = gentler ramp, more fine detail traded).

### Computing `secondary` (the coarse iso-surface point)
v1 (plane worlds): a **vertical** morph. `secondary = (Px, Py, Zc)` where `Zc` is the height at which the
**coarse-stride** density field (trilinear over the stride-`E` grid) crosses the iso-level in the column
at `(Px, Py)`, choosing the crossing nearest the vertex's own `Pz`. This is exactly "where this surface
point sits at the coarser LOD", and is correct for height-function terrain (`INFINITE_PLANE`).
- General worlds (spherical/island): morph along the world-mode "up" (radial for planets) instead of Z.
  Deferred past v1; v1 targets the plane terrain where the artifact lives.
- Overhangs/caves (column crosses iso multiple times): pick the nearest crossing; if none within a
  search band, fall back to `w = 0` (leave the vertex un-morphed — never worse than today).

### Corner / multi-face cells
A cell bordering coarser neighbours on 2–3 faces uses the **nearest** active transition face's distance
(`w = max` over the per-face weights). This is the historically buggy region of Transvoxel — gated by the
hole metric (below).

### Watertightness invariants (must hold; gated by tests)
1. The morph only moves vertices in the boundary slab; the deeper interior is untouched → intra-chunk
   continuity holds (ramp weight is continuous, 0 at depth `W`).
2. At the seam the morphed contour lies on the coarse contour the ribbon/neighbour use → no new cracks.
3. Morph is gated to **active transition faces only** → same-LOD boundaries and LOD0 chunks are byte-for-byte
   unchanged.

---

## 4. Increments

### Increment 1 — CPU bake + CVar + headless metric — DONE (2026-06-24) ✓
- `voxel.MCBoundaryMorph` CVar (default 1; mirrors `voxel.DCBoundaryWeld`) + `voxel.MCBoundaryMorphWidth`
  (default 2.0).
- `MorphVertexToCoarse` (per-vertex core) + `ApplyBoundaryMorph` (fine-MC path, `ProcessCubeLOD`) +
  `ComputeCoarseSurfaceZ` helper.
- Headless **T13** step-magnitude metric (`MarchingCubesLODBoundaryTests.cpp`, NonLinearZ LOD1|LOD2):
  morph reduces the seam step **19.3 → 1.9 units (~90%)** with **0 holes**; full suite **T1–T13 green**.

**Three watertightness refinements the test-driven loop forced (do not drop these):**
1. **Morph the ribbon too, not just fine MC.** The zero-thickness ribbon shares the fine-MC boundary
   contour; morphing only the fine MC pulled them apart → holes. Fix: `ProcessTransitionCell` morphs the
   ribbon's FINE-side vertices via the same `MorphVertexToCoarse` (leaving the coarse OUTER vertices on
   the coarse contour, so T6/T8 neighbour-match is preserved). The morph is a pure function of position,
   so coincident fine-MC/ribbon vertices move identically and stay welded.
2. **Clamp coarse samples to the chunk.** `ComputeCoarseSurfaceZ` sampled the coarse lattice up to E
   voxels past the chunk face, but neighbour data is only one plane deep → garbage densities → spurious
   morph. Clamp face-parallel sample coords to `[0, ChunkSize]`; at a boundary this samples the shared
   boundary plane = exactly the neighbour's coarse contour.
3. **Steepness gate (the v1 height-function restriction, made real).** Vertical-Z morph is ill-conditioned
   on steep surfaces — it distorts the thin ribbon and opens the seam (the synthetic Cliff field: a steep
   planar ramp). Gate: skip the morph where the surface normal (`-grad density`) is not mostly vertical
   (`|Nz| < 0.5·|N|`). Steep cliffs keep their un-morphed (watertight) boundary; gentle/moderate terrain
   morphs. A normal-direction (vs vertical-only) morph would lift this restriction — deferred.

### Increment 2 — GPU port (the live path)
- Mirror the bake in `MarchingCubesMeshGeneration.usf` `MainCS` (line ~334). Pass `MorphWidth` + the
  transition mask (already uploaded) as uniforms; forward the CVar from `VoxelGPUMarchingCubesMesher`.
- Shader recompiles in-editor (no C++ build). Headless T11/T12 pin the CPU spec the shader mirrors.

### Increment 3 — live A/B verify
- CLAUDIUS on `CPUDCSeamTestConfig` (scripted): same seams-ON oblique camera, `voxel.MCBoundaryMorph 0/1`
  + remesh — the vertical wall should become a ramp. Anti-fall + freeze + oblique survey (see
  `[[voxel-live-verify-workflow]]`). Tune `W`.

### Optional v2 — shader geomorph (only if LOD-switch *pop* is objectionable)
Promote the baked secondary to a runtime morph: pack a secondary-position/delta into the vertex (28→34–40 B),
lerp in the VF shader by a genuine distance morph factor, and correctly re-wire the morph path (replace the
dead `ApplyLODMorph`, feed the uniform, un-stub `UpdateLODTransition` with a cheap per-chunk update). Buys
temporal smoothness; **not** needed to kill the spatial step.

---

## 5. Files

| Path | Increment | Change |
|------|-----------|--------|
| `Source/VoxelMeshing/Private/VoxelCPUMarchingCubesMesher.cpp` | 1 | CVars, `ProcessCubeLOD` boundary-slab morph, `ComputeCoarseSurfaceZ` helper |
| `Source/VoxelMeshing/Public/VoxelCPUMarchingCubesMesher.h` | 1 | helper decl |
| `Source/VoxelMeshing/Tests/MarchingCubesLODBoundaryTests.cpp` | 1 | step-magnitude metric + assertions |
| `Shaders/Private/MarchingCubesMeshGeneration.usf` | 2 | mirror morph in `MainCS` |
| `Source/VoxelMeshing/Private/VoxelGPUMarchingCubesMesher.cpp` | 2 | forward `MorphWidth`/CVar uniform |

No changes to `FVoxelVertex`, the vertex factory, the renderer, or the LOD strategy in v1.

---

## 6. Verification

- **Headless** (`VoxelWorlds.Meshing.MarchingCubes.LODBoundary`): new step-magnitude metric drops with
  morph on; T6/T8 (coarse-side match) and T9 (holes == 0) stay green; LOD0 / same-LOD meshes byte-identical
  (morph gated to transition faces).
- **Live** A/B on `CPUDCSeamTestConfig` (GPU after increment 2): wall → ramp at the same camera.
- **Perf** (`voxel.Bench.Run`): the extra coarse-stride column sampling is confined to the boundary slab
  of active transition faces; measure the delta on LOD≥1 chunks.

## 7. Risks
- **Corner/edge cells** (2–3 transition faces) — classic Transvoxel failure mode; gated by the hole metric.
- **Perf** — extra coarse sampling per boundary-slab vertex; scope tightly + benchmark.
- **Detail trade-off** — fine detail is reduced within `W` of the seam (that is the ramp); tune via `W`.
- **Non-plane worlds** — v1 vertical morph is correct only for `INFINITE_PLANE`; radial morph is a follow-up.

## 8. Effort
v1 (increments 1–3) ≈ 3 days (CPU+test ~1.5, GPU port ~1, live tune ~0.5). Optional v2 ≈ +2–3 days.
