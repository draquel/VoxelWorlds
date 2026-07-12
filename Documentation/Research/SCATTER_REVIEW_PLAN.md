# Scatter Review — Implementation Plan

**Status:** In progress (started 2026-07-12)
**Module:** VoxelScatter (+ VoxelCore types)
**Scope:** Three items from the scatter review — (1) underwater placement, (2) the "refresh flash", (3) performance.

**Recommended sequence:** Phase A (flash fix) → Phase B (underwater bitmask) → Phase C (perf pass).
All three land in **one build cycle** because Phase B adds reflected types (enum/property/struct field),
which can't Live-Code — forcing an editor-close → `Build.bat VoxelEngineEditor Win64 Development` → relaunch.

Work happens in the **VoxelWorlds submodule** (branch there, PR, bump parent).

---

## Phase A — Eliminate the "refresh flash" (Issue 2)

**Root cause:** A single-chunk *supplemental* scatter pass triggers a whole-world per-type rebuild.
`ProcessCompletedAsyncScatter` → `UpdateChunkInstances(chunk, full-merged-data)` → existing-chunk branch
→ `QueueRebuild(type)` → `RebuildScatterType` → `ReleaseAllForScatterType` zero-scales **every instance of
that type across every loaded chunk in one frame**, then re-adds at 2000/frame. Gated to fire only when the
viewer is stationary ≥0.5 s AND the gen pipeline is idle — so rebuilds accumulate while exploring and all fire
the instant you stop, blinking out and streaming back every grass/rock/tree in the world.

**Goal:** No scatter operation may touch more than one chunk's worth of a type.

### A1. Reroute async supplemental passes to append-only (core fix)
`VoxelScatterManager::ProcessCompletedAsyncScatter` (~line 1827): the `ExistingScatter && bIsValid` branch is a
supplemental pass. `CompletedScatterTypes` guarantees a supplemental only generates *new* types for the chunk,
so the correct render call is append-only. Replace the shared `UpdateChunkInstances(...)` at ~1858 with a branch:
first-pass (new chunk) → `UpdateChunkInstances`; supplemental → `AddSupplementalInstances(chunk, Result.ScatterData)`
using only the new result's points (same as the distance-stream path at ~1489).

### A2. Localize `UpdateChunkInstances` existing-chunk branch (structural safety)
`VoxelScatterRenderer::UpdateChunkInstances` existing-chunk branch (~lines 200-232): instead of `QueueRebuild(type)`
(global), compute `oldTypes ∪ newTypes` and per type call `UpdateChunkTypeInstances(chunk, type, thisChunkTransforms)`
(empty = release). Makes the remaining sync/fallback callers flash-proof, runs every tick (no stationary gate).

### A3. Delete now-dead global-rebuild machinery
Remove from the renderer: `QueueRebuild`, `RebuildScatterType`, `FlushPendingRebuilds`, `ReleaseAllForScatterType`,
`PendingRebuildScatterTypes`, `MaxRebuildsPerFrame`, and viewer-stationary tracking (`RebuildStationaryDelay`,
`TimeSinceViewerMoved`, `ViewerMovementThreshold`, `LastViewerPosition`). `Tick()` collapses to `FlushPendingInstanceAdds()`.
Update `GetDebugStats` and `GetTotalMemoryUsage`.

**Safe because** distance-cleanup uses `ReleaseChunkScatterType`, edits use `UpdateChunkTypeInstances`, unload uses
`RemoveChunkInstances`; rebuilds were only reachable via the existing-chunk branch. ~150 lines deleted.

---

## Phase B — "Underwater" placement as independent bitmask toggles (Issue 1)

**Decision:** Replace the 3-value `EScatterSurfaceLocation` with independent **Surface (dry) / Underwater / Underground**
mask, so a def can allow any combination. Surface = dry land (above water). Water flag already exists on air voxels
(`VOXEL_FLAG_WATER`), set the same way the water mesher reads it.

### B1. New reflected types — `VoxelScatterTypes.h`
- Bitflags enum in VoxelCore:
  `UENUM(BlueprintType, meta=(Bitflags, UseEnumValuesAsMaskValuesInEditor="true")) enum class EScatterSurfaceLocationFlags : uint8 { Surface=0x01, Underwater=0x02, Underground=0x04 };`
- `FVoxelSurfacePoint`: add `bool bIsUnderwater = false;`
- `FScatterDefinition`: replace `SurfaceLocation` with
  `int32 SurfaceLocationMask` + `meta=(Bitmask, BitmaskEnum="/Script/VoxelCore.EScatterSurfaceLocationFlags")`,
  default `Surface`.

### B2. Filter logic — `CanSpawnAt`
Derive the point's single category (Underground > Underwater > Surface) and test the mask.

### B3. Classification plumbing — set `bIsUnderwater`
Mirror `bAirAboveIsUnderground`: `bAirAboveIsWater = AboveVoxel.HasWaterFlag()`; `bIsUnderwater = bAirAboveIsWater && !bIsUnderground`.
- `ExtractSurfacePointsFromVoxelData`, `ExtractSurfacePointsCubic`, and GPU `ClassifySurfacePointsUnderground` (→ rename `ClassifySurfacePoints`).
- Inert when `bEnableWaterLevel=false` (no water flags set).

### B4. Underground visibility toggle — `SetSurfaceScatterVisible`
Hide test → `bHide = (SurfaceLocationMask & Underground) == 0` (hide types that can't legitimately appear underground).

### B5. Migration — `VoxelScatterConfiguration::PostLoad`
Keep old enum as deprecated `UPROPERTY() SurfaceLocation_DEPRECATED`; one-shot guarded remap old→mask
(`SurfaceOnly→Surface`, `Any→all`, `UndergroundOnly→Underground`). Code defaults set the mask directly.

---

## Phase C — Performance & config audit (Issue 3)

### C1. Remove dead code
`PerformDistanceCleanup()` is never called (cleanup lives in `PerformDistanceSpawn`). Delete declaration + definition.

### C2. Live PIE profiling pass
Use existing `RenderSubScatterMs` (VoxelChunkManager) + `stat game` / `stat foliage` + manager/renderer `GetDebugStats`.
Baseline idle vs. exploration across the ScatterRadius boundary, before/after Phase A. Add a `vox.Scatter.Stats`
console command (gated `#if !UE_BUILD_SHIPPING`) to dump stats on demand.

### C3. Config-abuse audit
Flag: Density × SurfacePointSpacing, CullDistance ≫ SpawnDistance, MinScreenSize==0, bCastShadows on dense grass,
non-shrinking pool peak. Deliver as tuning recommendations.

---

## Testing
- **Unit** (extend `VoxelScatter/Tests/`, drives real pipeline):
  - Classification: synthetic voxel columns (water/underground/dry) → assert flags.
  - Mask filter: each mask combo × each category → spawn/skip.
  - Flash regression: supplemental new-type pass on chunk 1 leaves chunk 2's instances untouched.
- **PIE verify:** no flash on stop after traversal; underwater-only def only below WaterLevel; edits still clear
  locally; distance streaming intact.

## Build / PR
- Branch on VoxelWorlds submodule, squash-merge, bump parent submodule.
- Full rebuild required (Phase B reflected types).

## Risks / open items
- B5 migration guard is the one fiddly bit (guarded PostLoad chosen).
- Flooded caves treated as Underground (category precedence).
- `MaxInstanceAddsPerFrame = 2000` becomes the main streaming-smoothness knob post-fix; validate in C2.
