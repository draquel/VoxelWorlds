# PCG Decoration Architecture — Biome-Keyed, Edit-Aware Runtime Scatter

**Status**: Design (agreed with project owner 2026-06-27)
**Scope**: How PCG ("Generate at Runtime") integrates with the voxel world to decorate terrain with
foliage/props — edit-aware, biome-keyed, POI/construction-aware, and multiplayer-safe.
**Parent**: [ENGINE_INTEGRATION.md](ENGINE_INTEGRATION.md) §2 (PCG with Runtime Generation).
**Related**: [BIOME_SYSTEM.md](../Features/BIOME_SYSTEM.md), [SCATTER_SYSTEM.md](../Features/SCATTER_SYSTEM.md),
[EDIT_LAYER.md](../Features/EDIT_LAYER.md), [MAP_SYSTEM.md](../Features/MAP_SYSTEM.md).

---

## TL;DR

PCG is the **visual surface-decoration layer**. It does **not** own biomes, terrain, or gameplay state.

```
Voxel terrain + edits + biome selection + surface material   ← authoritative, deterministic, server-owned
        │  (edit-merged surface; per-point BiomeID/Material/Slope)
        ▼
Edit-aware hybrid sampler  ── near: read edit-merged voxel data · far: re-sample generator
        │
        ▼
Per-biome decoration subgraphs  ── routed by BiomeID, under a priority stack:
        POI graph  >  construction footprint (exclude)  >  ambient biome decoration
        ▲
Standalone Claims / World-Annotation system  ── POIs + constructions register tagged footprints;
                                                also feeds Map / Compass / (later) AI-nav
```

Everything PCG emits is **cosmetic**. Interactive things (harvestables, mineables, the POI structure
itself, the construction) are **gameplay state** owned by the server via the Item/Interaction/Inventory
plugins; PCG at most supplies deterministic positions.

---

## 1. Core principle — biome is *selected* by voxel, *interpreted* by PCG

Biome selection is the single source of truth and stays in the voxel generator
(`temp/moisture/continentalness → BiomeID`). It already drives terrain shape, surface material, and
editable trees; if PCG re-derived biome, foliage and terrain material could disagree and determinism
(needed for edits, save/load, multiplayer) would break.

The contract between the two worlds is **`BiomeID` (+ MaterialID + Slope + Normal) per surface point**,
which the **Voxel Surface Sampler** node already emits. PCG consumes that and decides *what to place*.

This realizes the latent intent in `FBiomeDefinition.Vegetation` (biomes were always meant to own their
decoration) — but expressed as **PCG graphs** instead of a flat `FScatterDefinition` list, unlocking
clustering, exclusion, splines, multi-mesh, and density maps.

## 2. The edit-aware hybrid sampler (the spine)

Requirement: when the player digs/builds near them, decoration must react. The pure "re-sample the
generator" approach is **edit-blind** (the generator doesn't know about the runtime edit layer), so it is
correct only for the far band. The sampler is therefore **distance-banded**:

| Band | Source | Properties |
|------|--------|------------|
| **Near** (loaded chunks) | Edit-**merged** voxel data (base + edit layer) | Edit-aware; **exact surface** (also fixes the analytic-vs-voxelized Z error seen in the first round-trip) |
| **Far** (beyond loaded chunks) | Re-sample the procedural generator (`FVoxelSurfaceQuery`) | Stream-independent reach; approximate surface; no edits to honor at distance |

Free behaviour: because the near band reads the **real edited surface** (actual material + slope), many
edits "work around" themselves — dig through grass to stone and the `AllowedMaterials = grass` rule simply
stops matching; build a steep mound and the slope filter culls it. No special-casing.

This supersedes the earlier "decoupled from streaming" framing for the near band only.

## 3. Per-biome decoration — thin router, not Biome Core

Routing: the sampler emits biome-tagged points → **Attribute Partition by `BiomeID`** → a **per-biome
subgraph** (each biome's decoration authored independently) → spawners. The mapping
(`BiomeID → UPCGGraph`) lives on the authoritative biome asset (a `DecorationGraph` per
`FBiomeDefinition`), so it shares the biome source of truth.

**PCG Biome Core** (`Engine/Plugins/Experimental/PCGBiomeCore`) exists and is feature-rich (assemblies,
exclusion volumes/splines, runtime ground scatter, local caching) — but it is Experimental and built
around painted/volume/texture/spline biome influence, not our deterministic per-point `BiomeID`. Adopting
it means re-expressing biome in its representation (impedance mismatch). **Decision: build the thin router,
but mine Biome Core as reference** — especially `BP_PCGBiomeCore_RuntimeGroundScatter`, `BiomeLocalCache`,
and the `BiomeFilter_*` / exclusion-spline falloff patterns.

## 4. The priority stack — working with/around POIs and constructions

```
POI decoration graph     (highest — a ruin brings its own rubble/vines in its footprint)
   ▲ claims its footprint, excludes everything below
Construction footprints  (exclude ambient scatter from building floors/pads)
   ▲
Ambient biome decoration (fills everything not claimed — §3)
```

PCG runs the difference top-down: high-priority sources claim regions; ambient biome decoration fills the
remainder. A claim can be both an **exclusion** (no wild grass on the courtyard) and a **generation
source** (its themed graph) — the duality PCG is designed for.

## 5. The Claims / World-Annotation system (standalone)

A first-class spatial registry of **claims**, separate from the edit layer:

```
Claim = { Shape (volume/spline/region), Priority, Tags, DecorationPolicy, DisplayInfo }
```

- **Producers**: POIs (hand-authored, deterministically placed: cities, dungeons), player constructions,
  system events.
- **Consumers**: PCG (exclusion/priority), Map/Minimap, Compass, later AI-nav and quest/discovery.
- **Authority**: server-owned and replicated (gameplay state; map/compass depend on it; POI placement is
  deterministic).

### Boundary-safe integration (critical)

`VoxelWorlds` is independent of the game-framework plugins (`CLAUDE.md`), so PCG must **not** code-depend
on a game-level POI/claims system. Resolution leans on PCG's native design: the claims system materializes
footprints as **PCG-readable tagged data** (tagged actors / a PCG data layer), and decoration graphs read
them **by tag** (`Get Actor Data` / by-tag filters). VoxelPCG knows only "exclusion regions tagged
`Claim.*`" — never "city" or "dungeon". One producer feeds PCG, Map, and Compass alike. (If push-based is
ever preferred, a thin `IExclusionProvider`-style interface defined *inside* VoxelPCG and implemented by
game-level code is the boundary-safe alternative.)

The claims system therefore lives at the **game level / a shared module**, not inside VoxelWorlds.

### POI decoration policy

Each claim carries a policy so POIs can blend or stand out:

| Policy | Behaviour |
|--------|-----------|
| `OwnGraph` | POI supplies its own PCG graph (dungeon rubble, city props) in its footprint |
| `InheritBiome` | POI uses the ambient per-biome decoration (reads as part of local terrain) |
| `Suppress` | Bare footprint |
| `Blend` (falloff) | Transition ring where ambient decoration fades toward the POI's own (city edge dissolves into wild terrain) — Biome Core's exclusion-spline falloff is the reference |

### Claims also condition terrain (forward link, not blocking)

"Construct a flat area to support a structure" is the player version of what a hand-authored **city**
needs: flat ground beneath it, generated deterministically. A claim can therefore feed **voxel terrain
generation** (flatten/condition the surface under the footprint at gen time, like deterministic tree
injection) in addition to runtime decoration. This argues for the claims registry being queryable *during*
world generation, not only at runtime — a later phase.

## 6. Multiplayer

- PCG runtime generation is **local per machine, never per-instance replicated** — consistency comes from
  **determinism**: same seed + same inputs → same placement everywhere. Noise and biome are deterministic.
- The edit-aware near band depends on the edit layer and the claims registry, both **server-authoritative
  and replicated**, so all clients converge (transient mismatch until an edit/claim replicates, then a
  regen resolves it — acceptable for cosmetics).
- **Firm line: PCG output is visual.** Harvestables, mineables, the POI structure, the construction are
  gameplay state via the Interaction/Inventory/Item plugins; PCG supplies at most deterministic positions.
- **Server build policy (per category):** cosmetic foliage → client-only (skip generation on dedicated
  server); decoration whose collision affects gameplay → server+client, deterministic.

## 7. Edit / claim reactivity (regeneration)

`EDIT_LAYER` dirty events **and** claim changes dirty the overlapping PCG runtime cells → **throttled**
regeneration (debounced like the bespoke `RebuildStationaryDelay` + world-stable checks, so rapid digging
doesn't thrash). Cell-granular regen is coarser than the bespoke per-instance surgical removal but
acceptable for ambient decoration; near-band edit-awareness (§2) covers most cases without explicit
clearing.

## 8. Relationship to the existing systems

| Concern | Owner | Fate under this design |
|---------|-------|------------------------|
| Terrain + biome selection + surface material | Voxel generator | Unchanged (authoritative) |
| Editable/destructible voxel trees (`FVoxelTreeInjector`) | VoxelGeneration | **Stays bespoke** (PCG can't do destructible voxel geometry) |
| Surface foliage / grass / props (HISM scatter) | `SCATTER_SYSTEM` | **PCG supersedes** this layer over time |
| Cleared-volume / surgical edit removal | `SCATTER_SYSTEM` | Replaced by near-band edit-awareness + dirty-on-edit regen |
| Biome → vegetation mapping | latent `FBiomeDefinition.Vegetation` | Realized as `BiomeID → UPCGGraph` |

`FVoxelSurfaceQuery` (the shared generator-query utility, used by both the tree injector and the sampler)
is the reusable core for the far band.

---

## 9. Revised phase plan (supersedes the old §2 Phase 4)

The original Phase 4 was a blunt `bUsePCGScatter` on/off toggle + a parity pass. **That is the wrong next
step**: edit-awareness is now a hard requirement, and coexistence is a *per-category/per-biome* policy that
emerges from this model — not a global switch. Revised order:

| Phase | Work | Notes |
|-------|------|-------|
| **3 — done** | Basic round-trip: sampler → static-mesh spawner → output; meshes on runtime terrain in PIE | Committed |
| **4 (revised) — Edit-aware hybrid sampler — DONE** | Near band reads edit-merged voxel data (`UVoxelChunkManager::QueryEditMergedSurface` + `FVoxelSurfaceQuery::ExtractSurfaceFromColumn`); far band falls back to the generator; Bounding Shape pin now required | The load-bearing spine; **fixed the Z-hugging** — PIE cubes now follow the terraced voxel surface, confirming the near band reads voxel data. Committed `ca90fd7`/`fead0f5` |
| **5 — Per-biome routing — DONE (data-driven dispatcher)** | `UVoxelPCGBiomeDecorationMapping` asset (BiomeID → decoration graph; "biomes as configuration", standalone — not on `FBiomeDefinition`, to keep VoxelCore PCG-free) + `UPCGVoxelBiomeDispatcher` node that partitions points by BiomeID and **dynamically schedules each biome's mapped subgraph** (`Context->ScheduleGraph` + `FPCGInputForwardingElement`, the `FPCGSubgraphElement` pattern) and merges. Setup how-to: [PCG_BIOME_DECORATION_SETUP.md](../Guides/PCG_BIOME_DECORATION_SETUP.md). | Smoke-tested in PIE: 441 points → dispatcher merged **3 biome subgraph results**. Committed `02fa52f`/`c0c27ef` |
| **6 — Claims-driven exclusion — DESIGNED** | The game-level **World Claims** system (registry + claim component/volume) produces tagged footprints; PCG reads them by tag for exclusion (Suppress) / route (OwnGraph) / falloff (Blend). Also covers terrain conditioning (gen-time density-level hook for deterministic POIs; runtime Edit-Layer System edit for player placements) and seed-based POI placement. Full design: `Plugins/WorldClaims/Documentation/CLAIMS_ARCHITECTURE.md`. | Designed 2026-06-28; new `WorldClaims` plugin (game-level, boundary-safe via tags); build pending |
| **7 — Edit/claim reactivity** | Hook `EDIT_LAYER` dirty + claim changes → throttled PCG cell regen | Debounced |
| **8 — Coexistence / parity** | Decide which categories PCG owns vs bespoke; retire superseded HISM-foliage where parity is proven | Replaces the old global toggle |

**Cross-cutting / deferred:** claims conditioning terrain generation (§5 forward link); multiplayer
server-build policy per category (§6); routing interactive props to gameplay systems (§6); World Partition
reconciliation (`ENGINE_INTEGRATION.md` §3).

## 10. Open questions

- Exact tag schema / data-layer shape for claims → PCG (tagged actors vs PCG data layer vs partition data).
- Near/far band cutover distance and its relation to collision radius and PCG runtime grid size.
- Regen throttling policy under sustained digging.
- Whether `FVoxelSurfaceQuery` gains a voxel-data-reading sibling for the near band, or the near band lives
  in the PCG element directly.
