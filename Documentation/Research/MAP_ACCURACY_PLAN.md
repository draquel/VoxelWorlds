# Map Accuracy Plan — Making VoxelMap Tiles Match the Generated Terrain

**Status:** Phase 1 in progress
**Problem:** The 2D map (UVoxelMapSubsystem tiles) does not match the generated world.
Water placement is wildly wrong; land materials are close but diverge at biome
boundaries and altitude bands.

## Root Causes (investigated 2026-07-19)

The map tile task (`VoxelMapSubsystem.cpp / GenerateTileAsync`) hand-replicates the
terrain pipeline instead of sharing it, and the replica has drifted:

1. **Missing continentalness height-scale curve (primary water bug).**
   Real surface height is
   `SeaLevel + (BaseHeight + Offset(cont)) + Noise * (HeightScale * ScaleMult(cont))`
   (`FInfinitePlaneWorldMode::ComputeEffectiveTerrainParams` + `NoiseToTerrainHeight`).
   The map captures `BakedHeightScaleCurve` but never applies it — it only adds the
   height *offset*. With the default scale curve (0.2 at cont=-1 → 1.0 at cont=+1),
   ocean regions really have 0.2× noise amplitude but the map renders 1.0× — so
   exactly where the oceans are, map heights are ~5× exaggerated and the
   `Height < WaterLevel` classification is effectively random.

2. **IslandBowl composition order.** The island mode applies continentalness *inside*
   the edge falloff (`Falloff(modulated height)`); the map adds the offset *after*
   the un-modulated falloffed height. Wrong on every IslandBowl config.

3. **Divergent biome selection.** Map: naive `Contains() + highest SelectionPriority`,
   fallback = first biome. Generator: `GetBiomeBlend` (continentalness soft gate,
   smoothstep T/M weights, exponential priority boost, Ocean-placeholder skip) then
   dithered `GetBlendedMaterial[WithWater]`. Diverges in every blend zone.

4. **Height-material rules unsorted.** Map iterates `HeightMaterialRules` in raw
   array order; the generator uses the priority-sorted cache
   (`ApplyHeightMaterialRules` / `SortedHeightRules`). Different winner when rules
   overlap (snow/rock altitude bands land differently).

5. **Colors from the static registry palette.** `FVoxelMaterialRegistry::GetMaterialColor`
   is a hardcoded table; the world renders from the material atlas textures. Correct
   material ID can still be a visibly wrong map color.

6. **Cosmetic ramps are arbitrary.** Elevation brightness `/4000`, water depth
   `/3000` — not derived from the config's actual height range; no slope shading.

**Why the drift exists:** the map's standalone world mode deliberately carries no
biome context so background tile tasks stay UObject-free across PIE teardown (the
old PIE-stop crash). `GetTerrainHeightAt` therefore returns RAW height and the map
re-applies modulation by hand — incompletely.

## Strategy

Stop reimplementing; share the pipeline. `FVoxelSurfaceQuery` (used by tree
placement + PCG) is already the analytic "height + biome + material at XY" ground
truth — exactly what a map pixel is. The only blocker is that the biome context is a
`UVoxelBiomeConfiguration*` (UObject, unsafe on tasks that outlive the world).

Fix: introduce a **plain-value biome snapshot** the UObject can produce; run the
shared math on the snapshot; let world modes carry the snapshot by value.

---

## Phase 1 — Height correctness (fixes water)

New plain struct **`FVoxelBiomeSnapshot`** (VoxelCore, `VoxelBiomeSnapshot.h`):
Phase 1 fields = `bEnableContinentalness`, `ContinentalnessSeedOffset`,
`ContinentalnessNoiseFrequency`, `BakedHeightCurve`, `BakedHeightScaleCurve`,
plus `GetContinentalnessTerrainParams(cont, &offset, &mult)` (the curve lerp moved
from the UObject). `UVoxelBiomeConfiguration::MakeSnapshot()` builds it.

Refactor:
- `IVoxelWorldMode`: modes store `FVoxelBiomeSnapshot` **by value**.
  `SetBiomeContext(const UVoxelBiomeConfiguration*)` becomes "capture snapshot now"
  (configs are static after init, so early capture is safe). Callers unchanged.
- `FInfinitePlaneWorldMode::ComputeEffectiveTerrainParams` + `GetTerrainHeightBounds`
  take the snapshot (thin UObject overloads kept where convenient).
- `UVoxelMapSubsystem::CreateStandaloneWorldMode` calls `SetBiomeContext(Config->BiomeConfiguration)`
  at resolve time (game thread — UObject touched only there). The mode instance
  remains UObject-free afterwards, so task-outlives-world safety is preserved.
- **Delete** the map's hand-rolled continentalness block (raw height + manual offset,
  the `BakedHeightCurve/BakedScaleCurve` captures). `GetTerrainHeightAt` now returns
  the true modulated height for InfinitePlane AND IslandBowl; the `Height < WaterLevel`
  check is unchanged and now correct.
- The map still computes its own continentalness sample for biome `Contains()`
  selection (that part is replaced in Phase 2).

Tests (model on `HeightIsosurfaceParityTests.cpp`):
- Snapshot-context mode height == UObject-context mode height across random XY
  (InfinitePlane + IslandBowl, continentalness enabled).
- Map-pixel-style height (standalone snapshot mode) == generator surface height.

## Phase 2 — Material parity

Extend `FVoxelBiomeSnapshot`: `BiomeDefs`, `BiomeBlendWidth`, underwater fields
(`bEnableUnderwaterMaterials`, `DefaultUnderwaterMaterial`), `bEnableHeightMaterials`
+ **priority-sorted** height rules, temperature/moisture seed offsets + frequencies,
validity flag.

- Move the bodies of `GetBiomeBlend`, `GetBlendedMaterial`,
  `GetBlendedMaterialWithWater`, `ApplyHeightMaterialRules` onto the snapshot
  (UObject methods delegate to a cached snapshot) — single source of truth,
  Ocean-skip and dither semantics carry over verbatim.
- `FVoxelSurfaceQuery::QuerySurfaceConditions` gains a snapshot overload.
- Map pixel loop: replace the local Contains/priority + unsorted-rules block with the
  shared snapshot call. Keep the map's blue water paint for submerged pixels.
- Restructure tile gen: height grid pass first, then material/color pass (enables
  Phase 3 hillshade to reuse the grid instead of 4 extra height samples per pixel).

Test: map-pixel material == `FVoxelSurfaceQuery::SampleSurface` material at random
points on demo-like configs.

## Phase 3 — Visual quality (optional)

- **Palette from the atlas:** per-material map color derived from the material atlas
  (editor-time average-albedo bake into the atlas/registry config, falling back to
  the registry table) instead of the hardcoded palette. (Runtime texture-source reads
  don't work in packaged builds — bake at author time.)
- **Hillshading:** lambert N·L from the tile height grid (fixed NW light, subtle),
  replacing/augmenting the flat elevation ramp.
- **Config-derived ramps:** water-depth and elevation gradients scaled from
  `GetTerrainHeightBounds` instead of hardcoded 3000/4000.

## Verification & logistics

- Unit tests as above; run via unreal-mcp `AutomationTestToolset` (CPU-only tests,
  targeted filter).
- PIE check on `/Game/PluginTesting/Demo/VoxelDemo`: open the in-game map, compare
  water/coastlines/biome patches against a `pie_camera` top-down survey screenshot.
- Header/UCLASS changes ⇒ no Live Coding: close editor → `Build.bat` → relaunch.
- Work lands as a VoxelWorlds submodule PR per repo convention (+ parent bump).
