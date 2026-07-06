# DC Material Blend Plan (Phase 4 — deferred)

Soft cross-fade transitions at Dual Contouring material borders. Follow-up to the
material-border striping fix (per-quad materials + border vertex duplication,
`fix/dc-material-border-stripes`, VoxelWorlds PR #13). **Status: DEFERRED — scoped,
not started.**

## Goal

Post-fix, a grass→gravel border is a crisp polygonal edge following triangle
boundaries (MC-style). This plan replaces it with a smooth cross-fade roughly **one
cell wide** (~1 m at LOD0, proportionally wider at distant LODs), like
heightmap-terrain layer blending.

## Background — why the border is hard today

MaterialID reaches the pixel shader as a float in UV1.x; the hardware interpolates
it across each triangle and the shader rounds it back to an integer atlas index
(`GetVoxelMaterialIDFromUV` / `GetSmoothMaterialID`). An ID is a *categorical*
value: interpolating it produces stripes of every intermediate index (the original
bug). The fix therefore made every triangle material-uniform — which necessarily
makes borders hard. Any blending scheme must keep IDs flat per triangle and
interpolate only *continuous* quantities.

## Approach

Alongside the (per-triangle-uniform) **primary** material, each vertex carries a
**secondary** material and a **blend weight**:

- **Both IDs stay flat per triangle** (both are decoded by rounding — a varying
  secondary would stripe exactly like the original bug).
- **Only the weight interpolates** across the triangle (continuous → safe).
- Pixel shader: sample primary and secondary, `lerp` by the interpolated weight.

Per border quad (both meshers):

- **Primary** = the quad's owned-edge solid-endpoint material (what the striping
  fix already computes).
- **Secondary** = the differing material among the quad's 4 corner cells' native
  materials (most-common differing one if several).
- **Weight per corner** = 1 where the corner cell's native material == secondary,
  else 0. Hardware interpolation of the 0/1 corner weights produces the linear
  ramp across the cell.
- **Interior quads**: weight 0 everywhere → shader branches on `weight > 0`, so
  interior pixel cost is unchanged.

### Transport (verified against current formats — no size changes)

| Channel | Current use | Phase-4 use |
|---------|-------------|-------------|
| `UV1.x` | MaterialID (float, flat) | unchanged (primary) |
| `UV1.y` | always 0 on the smooth path (FaceType on cubic) | **secondary ID** (flat) |
| `Color.A` | constant 255 | **blend weight** (linear — alpha has no sRGB distortion, unlike RGB) |
| `FVoxelVertex.PackedMaterialData` bits 16–23 (reserved) | unused | **secondary ID** (GPU) |
| `FVoxelVertex.PackedMaterialData` bits 24–31 (flags) | unused | **weight** (GPU) |

⚠️ This **exhausts the free bits** in the 28-byte `FVoxelVertex`; any later feature
needing vertex flags forces a 32-byte format bump.

### Cheaper pixel-cost variant (decide during implementation)

Instead of sampling both materials (2× LUT + 2× triplanar = ~2× texture cost on
border pixels), a **dithered** transition picks ONE material per pixel by comparing
the weight against noise — same vertex data, no extra samples, slightly noisy
border that TAA smooths. Mesher work is identical either way; only the `.ush`
helper differs, so it can even be a material static switch.

## Work breakdown (~1.5–2 sessions)

| # | Task | Where | Est. |
|---|------|-------|------|
| 0 | Confirm how `M_VoxelMaster` decodes UV1 (custom-node inventory) | editor (MCP material tools) | 0.5 h |
| 1 | CPU DC: per-quad (primary, secondary, corner weights); duplication cache key extends to (cell, primary, secondary); emit UV1.y + Color.A | `Source/VoxelMeshing/Private/VoxelCPUDualContourMesher.cpp` (`GenerateQuads`/`EmitVertex`) | 2–3 h |
| 2 | GPU DC: same pair/weight logic in Pass 3 (`DCQuadGenerationCS`), packed into the free `PackedMaterialData` bits | `Shaders/Private/DualContourMeshGeneration.usf` | 2–3 h |
| 3 | Vertex-factory plumbing: `FVoxelLocalVertex::FromVoxelVertex` currently writes *FaceType* into UV1.y for ALL GPU-readback vertices; the smooth path must write the secondary ID instead — conversion needs a smooth/cubic flag from the scene proxy | `VoxelLocalVertexFactory.h` (~line 177), `VoxelSceneProxy.cpp` call sites | 1 h |
| 4 | New `.ush` helper `SampleVoxelTerrainBlended(...)` (+ dither variant) and swap into M_VoxelMaster's smooth path | `Shaders/Private/VoxelTriplanarCommon.ush` + material graph | 1–2 h |
| 5 | Tests (MB5+: pair uniform per triangle, weights nonzero only at borders, interior weight 0, CPU/GPU consistency via the existing MB readback harness) + PIE visual verify | `Source/VoxelMeshing/Tests/DualContourMaterialBorderTests.cpp` | 2 h |

Build-cycle note: NO shader `FParameters` changes are needed (unlike the Phase-2
fix), so the whole thing should Live-Code + `recompileshaders changed` without a
full editor-restart build.

## Limitations & risks

- **3+ materials meeting at one quad**: only two blend; the third snaps hard at its
  edge. Rare (needs three materials within one cell) and subtle.
- **MC stays hard-bordered** in v1. The shader change is backward-compatible
  (weight defaults to 0 → pure primary), so MC can adopt the identical scheme later
  without rework.
- **M_VoxelMaster is a binary asset** — the graph edit happens in-editor and isn't
  diffable in a PR.
- Border pixels cost ~2× texture samples (or accept dither noise instead).
- `Color.A` becomes load-bearing; anything else that later wants vertex alpha
  conflicts.

## Deferral assessment

Safe to defer: fully additive on top of the merged striping fix, no save-format
impact (meshes are runtime-generated), nothing else depends on it, and the cost
does not grow by waiting. The MB test harness makes verification cheap whenever
this is picked up.
