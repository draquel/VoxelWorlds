# LOD Seam Investigation — Stage 1 Findings (2026-06-10)

## Headline

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
