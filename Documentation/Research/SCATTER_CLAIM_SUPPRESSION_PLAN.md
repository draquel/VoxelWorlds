# HISM Scatter — Claim-Driven Suppression (Design Plan)

**Status:** Phase 1 (scatter exclusion API) BUILT on `feature/scatter-edit-aware` — reviewed leans
locked in: world-subsystem facade (`UVoxelScatterExclusionSubsystem`), oriented-box containment from
the start, reserved-but-unused `EdgeFalloff` field. Phase 2 (VoxelWorldPOI claim bridge) not started.
**Author:** design pass, 2026-07-13.
**Related:** `PCG_DECORATION_ARCHITECTURE.md` (the same claim→decoration pattern for the PCG path),
`Plugins/WorldClaims/Documentation/CLAIMS_ARCHITECTURE.md`, memory `scatter-edit-aware`.

---

## 1. Problem

POIs (and player construction) need to **clear the ambient foliage** inside their footprint —
either the whole claimed zone (a city plaza) or just a static-asset footprint (under a building).

Claim-driven suppression **already exists — but only for the PCG decoration path**:
`Claim.Decoration.Suppress` on a `UWorldClaimComponent` makes the PCG decoration graph `Difference`
the claim out, and `UClaimDecorationRefreshSubsystem` (in VoxelWorldPOI) regenerates the affected
PCG components live when claims change.

The demo's trees/grass/mushrooms are **not** PCG — they are the **VoxelScatter HISM** system
(`UVoxelScatterManager`), which has **no claim awareness**. So a POI claim clears PCG decoration but
leaves the HISM trees standing. Closing that gap is the goal.

## 2. Hard constraint: VoxelWorlds must not depend on WorldClaims

VoxelWorlds is an independent plugin (see root `CLAUDE.md`). `UWorldClaimRegistry`'s own header is
explicit: *"PCG decoration does NOT use this API — it discovers claims by the mirrored actor tag,
keeping VoxelWorlds/PCG free of any WorldClaims code dependency."*

So VoxelScatter **must not** call into WorldClaims. The claim→scatter mapping lives **outside**
VoxelWorlds, in the one plugin already allowed to see both sides — **VoxelWorldPOI** — exactly as
`UClaimDecorationRefreshSubsystem` does for PCG today.

## 3. Design — two decoupled halves

```
 WorldClaims                 VoxelWorldPOI  (sees both)            VoxelWorlds / VoxelScatter
 -----------                 --------------------------            --------------------------
 UWorldClaimRegistry  --OnClaimsChanged-->  UScatterClaim        --Register/Unregister-->  UVoxelScatterManager
 (Claim.Decoration.Suppress)               SuppressionSubsystem   ExclusionVolume(GUID, box)  (generic exclusion
                                            (debounced bridge)                                  volumes — no claim
                                                                                                knowledge)
```

### 3a. VoxelScatter side — a generic **exclusion-volume API** (no claim knowledge)

VoxelScatter already has the core idea: `FClearedScatterVolume { FVector Center; float Radius; }`
stored in `ClearedVolumesPerChunk` and consulted at every placement/filter site
(`IsPointInClearedVolume`, `ApplyClearedVolumesToResult`, the two extractors). But those are:
- **sphere-only**, **per-chunk-keyed**, and **cleared on chunk unload/regen** — correct for
  *edit-driven, transient* clears (fill a hole back in, reload, foliage returns), **wrong** for a
  claim that must persist across streaming.

Proposed additions to `UVoxelScatterManager` (public, Blueprint-callable):

```cpp
// A persistent, world-level exclusion region. Oriented box (matches a claim footprint) + optional
// falloff for a soft edge later. Keyed by a stable GUID so the owner can update/remove it.
struct FScatterExclusionVolume {
    FGuid    Id;
    FTransform Frame;      // world transform of the box centre
    FVector  HalfExtent;   // oriented-box half-extents
    // (future) EScatterExclusionPolicy Policy = SuppressAll;  // SuppressAll | SuppressTypes(tags)
    bool ContainsPoint(const FVector& P) const;   // oriented-box test
    FBox GetWorldBounds() const;                  // AABB for chunk pre-filter
};

void RegisterScatterExclusionVolume(const FScatterExclusionVolume& Volume);   // add or replace by Id
void UnregisterScatterExclusionVolume(const FGuid& Id);
```

Stored in a **separate, persistent** `TMap<FGuid, FScatterExclusionVolume>` (NOT the transient
per-chunk cleared-volume map — keeping the two semantics apart is deliberate).

Behaviour:
- **Placement consult.** Every spawn-point placement/filter path also tests the persistent exclusion
  volumes (pre-filtered by `GetWorldBounds()` overlap with the chunk, then per-point
  `ContainsPoint`). This covers fresh generation, re-extraction, and distance-stream supplements —
  so a chunk that streams in *while a claim is active* is born bare.
- **Register = clear live.** On `Register`, immediately drop existing spawn points inside the volume
  (reuse the smooth per-(chunk,type) `UpdateChunkTypeInstances` path already used by
  `ClearScatterInRadius`) for the chunks the box overlaps — no flash, no full rebuild.
- **Unregister = regrow.** On `Unregister`, `RegenerateChunkScatter` the overlapped chunks so foliage
  comes back (flash-free path from the edit-aware work).

VoxelScatter stays completely claim-agnostic: it just owns generic exclusion boxes.

### 3b. VoxelWorldPOI side — the claim bridge (mirrors the PCG one)

New `UScatterClaimSuppressionSubsystem : UTickableWorldSubsystem` in **VoxelWorldPOI**, a near-twin of
`UClaimDecorationRefreshSubsystem`:
- Binds `UWorldClaimRegistry::OnClaimsChanged`; accumulates changed bounds; **debounces** (~0.4 s) to
  coalesce bursts.
- On flush: for every claim carrying `Claim.Decoration.Suppress`, build an `FScatterExclusionVolume`
  from the claim's frame + `ClaimExtent` (GUID derived from the claim component) and
  `RegisterScatterExclusionVolume`. For suppress-claims that disappeared, `Unregister` their GUID.
- Reaches `UVoxelScatterManager` through a VoxelWorlds accessor (see Open Questions).

One `Claim.Decoration.Suppress` claim now suppresses **both** PCG decoration (existing path) **and**
HISM foliage (this bridge). "Whole zone vs under-the-building" is purely the claim box's extent — an
authoring choice per claim, no system change.

## 4. How it composes with what exists

- **Edit-driven cleared volumes** (transient, sphere, per-chunk): untouched. Player digs still clear
  live and reset on reload. Claim exclusion volumes are a *separate persistent* list; both are
  consulted at placement.
- **Player-edit clear-only policy** (just shipped): unrelated — player edits don't mint claims.
- **PCG suppression:** same tag, same `OnClaimsChanged` event, parallel bridge. Consistent semantics.
- **Priority:** claims already have `Priority`; MVP ignores it (any Suppress claim excludes). Only
  matters once non-suppress decoration policies map to scatter (future).
- **Existing POIs (free win):** `UPOIPlacementSubsystem` already tags its city claims
  `Claim.Decoration.Suppress` (`POIPlacementSubsystem.cpp`). So the moment this bridge lands, existing
  POI footprints go bare of HISM foliage with **no POI-side change** — the cheapest end-to-end
  validation. See `Plugins/VoxelWorldPOI/Documentation/POI_POPULATION_PLAN.md`.

## 5. Phasing

- **Phase 1 — VoxelScatter exclusion API** (VoxelWorlds, self-contained, unit-testable):
  `FScatterExclusionVolume` + register/unregister + placement consult + live clear/regrow.
  Automation tests: point-in-oriented-box; a registered volume drops spawn points inside it across
  fresh + re-extract; unregister restores. No WorldClaims involved.
- **Phase 2 — VoxelWorldPOI bridge:** `UScatterClaimSuppressionSubsystem` + accessor to the scatter
  manager. PIE: drop a `Claim.Decoration.Suppress` volume over grassy terrain → HISM foliage clears
  in the box (and the existing PCG suppression still fires); remove it → foliage regrows. Confirm
  debounce coalesces bursts. The canonical demo is the **"tower in a POI claim"** vertical slice —
  a static structure standing on a bare flattened POI pad ringed by wilderness foliage — jointly
  owned with `POI_POPULATION_PLAN.md` (that plan spawns the structure; this one clears the footprint).
- **Phase 3 — richer policy (future, optional):** `Claim.Decoration.OwnGraph` (POI supplies its own
  scatter set → typed exclusion), `Claim.Decoration.Blend` (falloff ring → probabilistic thinning via
  the volume's falloff), oriented-box → spline/volume footprints, spatial acceleration if claim
  counts grow.

## 6. Testing / verification

- **Unit (Phase 1):** oriented-box containment; exclusion filter over the CPU extractor path (reuse
  the `VoxelScatterCleanupTests` harness — it already forces CPU extraction); register-then-extract
  yields zero points in-box; unregister-then-extract restores.
- **PIE (Phase 2):** GPU scatter path. Place a camp claim (`Claim.Construction.Camp` +
  `Claim.Decoration.Suppress`) via the POI path; verify HISM trees/grass clear inside the footprint,
  PCG decoration also clears (unchanged), and removing the claim regrows both. `GetRefreshCount()`-style
  counter on the bridge as a headless hook.

## 7. Open questions (for review)

1. **Accessor to the scatter manager from the bridge.** `UVoxelScatterManager` is owned by
   `UVoxelChunkManager` (owned by the voxel world actor). Options: (a) expose
   `UVoxelChunkManager::GetScatterManager()` + a way to find the chunk manager from the world; (b) a
   thin VoxelWorlds `UWorldSubsystem` facade that forwards `Register/UnregisterScatterExclusionVolume`
   to the active scatter manager (stable, discovery-free entry point — my lean). Either keeps the
   claim knowledge out of VoxelWorlds.
2. **Oriented box vs AABB for MVP.** Claims are oriented boxes; simplest first cut is the AABB
   (`GetClaimBounds`) which slightly over-suppresses on rotated claims. Oriented-box test is cheap —
   probably just do it correctly from the start.
3. **Soft edges.** Worth a falloff band (probabilistic thinning at the rim) in Phase 1's struct even
   if unused, or defer entirely to Phase 3? Leaning: leave the field in, ignore it for now.
4. **Player-built structures.** Should player `Claim.Construction.*` placements also suppress HISM
   foliage under them via the same bridge? (Presumably yes — same tag path — but confirms scope.)
