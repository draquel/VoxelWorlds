# Traversal & World-Size Limits

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

How far a player can travel and how big a world can be, limit by limit, from the one that bites first to the largest.
The probe began with a fall-through far from the origin, which turned out to be a collision-cook-lead bug, not a
precision limit. Sizes are for the demo's configuration (64³ chunks of 75 uu = 4,800 uu chunks).

## Summary

| # | Limit | Where it bites | State |
|---|-------|----------------|-------|
| 1 | **Collision cook lead** (walking) | Chunk boundary crossings, at any distance | **Fixed.** Collision cooks around the pawn, ahead of it by its speed, along its path and under a descent. No distance limit at walking speed. |
| 2 | **Generation throughput** (fast travel) | The pawn outruns the load front | **Fixed for realistic speed.** The per-frame load budget rises with the viewer's speed (`voxel.Stream.SpeedAdaptive`, on by default; horizontal, 3D on a planet), checked to about 15 m/s. Around 40 m/s and above the pawn still outruns generation; generation biased toward the direction of travel is not built. A descending camera is led by the descent prefetch (`voxel.Stream.DescentLead`). |
| 3 | **Analytic height against the real isosurface** | Spawn, navigation and AI ground placement | **Fixed for the generated surface.** The generated surface (`GetGeneratedSurfacePoint`) applies continentalness and the conditioning zones as generation does (`VoxelWorlds.Generation.HeightParity.*`: 0.00 voxels apart), and GPU generation is held to the CPU by the `GPUvsCPU*` parity tests. Caves, overhangs and edits are not in it: callers that need them use the edit-merged surface (`QueryEditMergedSurfacePoint`) or a trace. |
| 4 | **Float vertex positions** | Terrain vertices far from the origin | **Not a limit.** Every primitive sits at its render region's origin (16³ chunks, double) and stores float offsets from it, in every world mode, so a vertex's float spans at most a region (MP2, WM-MP5). |
| 5 | **Chaos and character movement precision** | Physics far from the origin | **Not a limit in reach.** UE's large world coordinates keep transforms in double; collision trimeshes are chunk-local floats placed by a double transform. Residual float paths in character movement would matter only at thousands of km. |
| 6 | **Chunk coordinate (`int32`)** | Astronomical | 2³¹ chunks × 4,800 uu ≈ 1.0e13 uu, about 100 million km. Not a practical limit. |
| 7 | **World bounds and KillZ** | Per map | `WorldSettings` (KillZ, `bEnableWorldBoundsChecks`) per level; a large world lowers or disables KillZ and widens the bounds. A planet sets its own KillZ under its orbit ceiling. |

Generation has no distance limit either: every world mode samples the hashed, split-origin noise (`FVoxelNoise`, MP4,
`GenerationVersion` 3), exact to float precision at any distance and without the old 256-unit period that repeated
features about every 120 km. No known ceiling remains on a heightmap world short of the double precision of the position.

## Why regions: float precision against distance

A single-precision float has a 23-bit mantissa, so a value of magnitude M is quantized to about M × 1.19e-7. With
every vertex stored as an absolute float position, as the renderer once did:

| Distance from origin | Vertex quantization | Effect |
|---|---|---|
| 150,000 uu (1.5 km) | 0.018 cm | invisible |
| 1,000,000 uu (10 km) | 0.12 cm | invisible |
| 10,000,000 uu (100 km) | 1.2 cm | sub-voxel |
| 100,000,000 uu (1,000 km) | 12 cm | visible facets and seam drift |

Absolute floats were clean to about 100 km and broke down past 500-1,000 km. Render regions (decision D23 for planets,
D40 for every mode) put the float's magnitude at a region's size wherever the region is: a plane's vertex 1,000 km out
is within 0.0005 uu of the double reference (`Rendering.Planet.RegionPrecision`).

## World-origin rebasing: not needed

Shifting the world origin toward the viewer was the usual answer to #4 and #5. The renderer took the region route
instead, which resets the float's magnitude without a cross-system rebase, and collision is chunk-local with a double
transform. A rebase would matter only for gameplay actors thousands of km out, which no world mode here reaches.

## Planets: the radius ceiling

Every surface vertex of a planet lies about R from its centre, so with one primitive at the origin the float ceiling
(#4) would be a radius of about 100 km. Planets render in regions too (spherical Phase 6, P6.2), and generation keeps
every size-R value in double on the CPU (`ShellSignedDistanceAtAltitude`) or in the chunk's frame on the GPU
(`PlanetFrame.ush`). Planets are precise to about 1,000 km in radius (`Review_Planet_L`): density steps 0 against a
double reference, normals within 0.01°, vertices within 0.006 uu. `Features/WORLD_MODES.md` "The radius ceiling" has the
detail.
