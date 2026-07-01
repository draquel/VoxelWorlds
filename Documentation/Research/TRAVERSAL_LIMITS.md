# Traversal & World-Size Limits

Constraint probe for "how far can a player travel / how big can the world be." Motivated by the
fall-through-at-distance investigation (which turned out to be a collision-cook-lead bug, **not** a
precision limit). Ordered from tightest-biting to largest. Values are for the reference config
(ChunkSize 32 × VoxelSize 100 = **3200 uu / 32 m chunks**).

## Summary table

| # | Constraint | Where it bites | Status / fix |
|---|-----------|----------------|--------------|
| 1 | **Collision cook lead** (walk) | Chunk boundary crossings, any distance | **FIXED** — pawn-centered, speed-aware shell + path/descent coverage (`voxel.Collision.LeadFix`). No practical distance limit at walk speed. |
| 2 | **Generation throughput** (fast traversal) | Pawn outruns the load front (`feetLd=0`) | **FIXED for realistic speed** — speed-adaptive load budget (`voxel.Stream.SpeedAdaptive`, auto). Validated to ~15 m/s. Extreme (~40 m/s+) still falls → needs **forward-biased generation** (TODO). |
| 3 | **Deterministic height ↔ real isosurface mismatch** | Spawn / nav / AI ground placement, grows with distance | **OPEN** — `GetTerrainHeightAt` is raw CPU 2D noise; actual surface is GPU-generated + biome/claim-conditioned 3D isosurface. Measured **~1476 uu off at (150k,150k)**. Does NOT affect the collision fall. Fix: GPU/CPU noise parity, apply conditioning to the height query, or use a collision/mesh trace (as the debug settle now does). |
| 4 | **Visible mesh float-bake** | Terrain vertex precision far from origin | Long-range. `VoxelSceneProxy` bakes absolute world pos into vertices as **FVector3f** (`ChunkOffset`). ULP = magnitude × 1.19e-7. Fix: **origin rebasing** or double-relative vertex encoding. |
| 5 | **Chaos / CMC precision** | Physics far from origin | Mitigated by UE5 LWC (double transforms). Collision trimeshes are chunk-**local** float placed by a **double** transform → precision-safe. Residual float paths in CMC degrade only at extreme coords. |
| 6 | **Chunk coord (int32)** | Astronomical | `FIntVector` × 3200 uu → ~6.9e12 uu ≈ **69 million km**. Not a practical limit. |
| 7 | **World bounds / KillZ** | Map-configured | Per-map `WorldSettings` (KillZ, `bEnableWorldBoundsChecks`). Large worlds must lower/disable KillZ and widen bounds. Verify per level. |

## Float vertex-bake precision (#4) vs distance

`ULP(M) = M × 2^-23 ≈ M × 1.19e-7` (single-precision, 23-bit mantissa):

| Distance from origin | Vertex quantization | Verdict |
|---|---|---|
| 150,000 uu (1.5 km) | 0.018 cm | invisible — **not** the reported symptom |
| 1,000,000 uu (10 km) | 0.12 cm | invisible |
| 10,000,000 uu (100 km) | 1.2 cm | sub-voxel, generally fine |
| 100,000,000 uu (1000 km) | 12 cm | visible facets / seam drift |

So the GPU renderer's float bake is clean to roughly **~100 km**, degrading past ~500–1000 km. The
1.5 km symptom in the original report was never precision — it was constraint #1.

## Practical traversable range (with current fixes)

- **Walk / realistic-fast (≤ ~20 m/s):** effectively unbounded for collision & streaming; visual
  precision sub-cm well past 10 km.
- **Extreme speed (~40 m/s / 144 km/h):** still outruns generation — needs forward-biased generation
  (constraint #2 TODO).
- **Visual precision ceiling:** ~100 km clean, artifacts beyond ~500–1000 km (constraint #4).

## Recommended long-term fix: world-origin rebasing

Periodically shift the world origin toward the viewer so absolute coordinates near the player stay
small. This resets the float-bake magnitude (#4), keeps Chaos/CMC in their precise range (#5), and
makes the world effectively unbounded. It is a cross-system change (renderer proxy offsets, collision
component placement, chunk-coord ↔ world mapping, gameplay actors) and is **not** implemented here —
tracked as the major follow-up. Until then the practical clean range is ~100 km from origin.
