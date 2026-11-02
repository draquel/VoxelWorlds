# How-To: Biome-Keyed PCG Decoration

> State as of 2026-11-02 (code review CR5).

**Audience**: Setting up runtime PCG decoration (foliage/props) that varies per voxel biome.
**Design/rationale**: [PCG_DECORATION_ARCHITECTURE.md](../Research/PCG_DECORATION_ARCHITECTURE.md).
**What exists**: the Voxel Surface Sampler (edit-merged near loaded chunks, the generator farther out, a planet's
cube-sphere grid) and the Voxel Biome Dispatcher, in the `VoxelPCG` module. Reacting to edits and claims after
generation (phase 7) is not built.

---

## The pipeline

```
Voxel Surface Sampler  →  Voxel Biome Dispatcher  →  (graph Output)
   emits surface points        partitions points by BiomeID, runs each
   tagged with BiomeID,         biome's mapped decoration subgraph, merges
   Material, Slope, Normal
```

- **Voxel Surface Sampler** (`VoxelPCG`) reads the runtime voxel terrain — edit-merged voxel data near
  loaded chunks (honors digging/building, hugs the exact surface), the procedural generator farther out —
  and emits PCG points carrying `BiomeID`, `MaterialID`, `Slope`, `Normal`.
- **Voxel Biome Dispatcher** (`VoxelPCG`) routes those points to per-biome decoration subgraphs using a
  data-driven `UVoxelPCGBiomeDecorationMapping`. Adding a biome is a data edit on the mapping, not a graph
  edit. Biome *selection* stays voxel-authoritative; this only maps the result to decoration.

PCG output here is **visual only** — interactive/harvestable/collidable props must go through the
gameplay systems (Interaction/Inventory/Item), not raw PCG instances.

## Steps

### 1. Author a decoration graph per biome

**One graph per biome, not per scatter item.** A biome's graph receives that biome's surface points on its
**Input** node (already carrying `BiomeID`, `MaterialID`, `Slope`, `Normal` from the Voxel Surface Sampler)
and contains **all** of that biome's scatter — trees + bushes + rocks + grass for a Forest, etc. Author one
per biome (`G_ForestDecoration`, `G_DesertDecoration`, …); they can differ arbitrarily — that is the point.

#### What's inside a biome graph: one branch per item

Each scatter item is one **branch off the Input node**: `filter(s) → density → Static Mesh Spawner`. The
incoming points already have the attributes; you just filter and thin them per item:

```
                 ┌─ Slope filter → Material filter → density → Spawner (Trees)
Input (points) ──┤
                 └─ Slope filter →                   density → Spawner (Rocks)
                                                       … more items …            → Output
```

#### The three node types

| Job | Node | Key settings |
|-----|------|--------------|
| Filter by a numeric attribute (slope, material) | **Filter Attribute Elements by Range** | `Target Attribute` = `Slope` / `MaterialID`; set **Min/Max Threshold** → check **Use Constant Threshold**, pick the type (`Double` for Slope, `Integer32` for MaterialID) and value. Take the **InsideFilter** output. Min==Max gives an exact match. |
| Keep a % of points (density) | **Random Choice** | uncheck **Fixed Mode**, set **Ratio** (e.g. `0.02` = 2%). Take the **Chosen** output. (Alternative: **Self Pruning** for minimum spacing.) |
| Place the mesh | **Static Mesh Spawner** | set the mesh (weighted mesh selector → add an entry → Static Mesh). |

#### Worked item — Forest trees (2% on flat grass)

```
Input → [Filter Slope 0–20] → [Filter MaterialID==grass] → [Random Choice 0.02] → [Spawner: tree] → Output
```

A ready-made reference graph is at **`/Game/PluginTesting/PCG/G_ForestDecoration`** with exactly this trees
branch wired and configured (Slope `0–20`, MaterialID `1`, Ratio `0.02`, a placeholder cylinder mesh) and a
comment box. Open it as a template; set `MaterialID` to your real grass id (`EVoxelMaterial`) and swap the
cylinder for a tree mesh.

#### Add more items

A second item (rocks) is the **same branch** off `Input` with different settings (e.g. `Slope 25–90`, skip
the material filter, `Ratio 0.05`, rock mesh). Run several spawners in one graph; route each spawner's `Out`
to the graph `Output` (the Output pin accepts multiple connections), or `Merge Points` first.

> **Wiring note:** connecting these nodes in the PCG editor connects directly. (Some external graph-editing
> tools auto-insert small `Filter Data` adapter nodes between union-typed pins — harmless and deletable; you
> won't get them when you drag-connect by hand.)

### 2. Create the biome → graph mapping

Create a **Voxel PCG Biome Decoration Mapping** data asset (`UVoxelPCGBiomeDecorationMapping`). Add one
entry per biome:

| Field | Meaning |
|-------|---------|
| `BiomeID` | The voxel biome id (matches `FVoxelData::BiomeID` and the biome config's biome ids) |
| `BiomeLabel` | Optional documentation label |
| `DecorationGraph` | The PCG subgraph from step 1 for this biome |

Biomes with no entry (or a null graph) get no decoration.

### 3. Build the main graph

```
Voxel Surface Sampler  →  Voxel Biome Dispatcher  →  Output
```

- Wire the graph **Input** node into the sampler's **Bounding Shape** pin (required — PCG only schedules
  the node when it is on the execution path; this also supplies the sampling footprint).
- On the **Voxel Biome Dispatcher**, set **Biome Mapping** to the asset from step 2 (and **Biome
  Attribute** = `BiomeID`, the default).
- Wire sampler `Out` → dispatcher `In`, dispatcher `Out` → graph `Output`.

### 4. Place it in the world

Add the main graph to a **PCG component / PCG Volume** over the terrain. Notes:

- The voxel terrain only exists in a **game world (PIE / packaged)** — the sampler outputs nothing in the
  editor world (it logs `no initialized VoxelChunkManager found`). Verify in PIE.
- PCG needs a **`PCGWorldActor`** in the world; if generation produces nothing, trigger one editor-world
  generate (which creates it) or ensure the level has one, then run PIE.
- The sample footprint = the component/volume bounds (or a Bounding Shape input). A larger footprint spans
  more biomes.

## Tooling gotchas (from building the example)

- The **Voxel Surface Sampler** and **Voxel Biome Dispatcher** are custom plugin nodes. Epic's PCG MCP /
  some tools that add nodes by display name **cannot add custom-plugin nodes** — add them in the PCG graph
  editor, or via Python `UPCGGraph::AddNodeOfType(unreal.PCGVoxelSurfaceSamplerSettings / unreal.PCGVoxelBiomeDispatcherSettings)`.
  Stock nodes (Static Mesh Spawner) add normally.
- Graph edges aren't exposed to Python (`AddLabeledEdge` isn't a UFUNCTION) — wire edges in the editor or
  via tooling that drives `UPCGGraph` edge APIs.
- The dispatcher dynamically schedules one subgraph **per biome present** and merges the results — verify
  with verbose `LogPCG`: `Voxel Biome Dispatcher: merged N biome subgraph result(s)`.

## Worked example (smoke test)

Under `/Game/PluginTesting/PCG/`:
- `PCG_BiomeDecoration_Cube` — a minimal decoration subgraph (Input → Static Mesh Spawner [Cube] → Output).
- `BiomeDecorationMapping` — maps biome ids 0–5 to that one subgraph (bare-bones; production maps each
  biome to a distinct graph).
- `PCG_VoxelBiomeTest` — Voxel Surface Sampler → Voxel Biome Dispatcher → Output.

Result in PIE over the `VoxelWorldsTest` map: the sampler generated 441 biome-tagged points; the
dispatcher partitioned them into **3 biomes** and merged 3 subgraph results → 441 cubes on the terrain —
confirming the partition + dynamic per-biome dispatch + merge pipeline.

## See also

- [PCG_DECORATION_ARCHITECTURE.md](../Research/PCG_DECORATION_ARCHITECTURE.md) — full design, priority
  stack (POI > construction > ambient), Claims/World-Annotation system, multiplayer/edit notes, phase plan.
- [BIOME_SYSTEM.md](../Features/BIOME_SYSTEM.md) — how biomes are selected (voxel-authoritative).
- [SCATTER_SYSTEM.md](../Features/SCATTER_SYSTEM.md) — the bespoke HISM scatter that PCG decoration
  coexists with / supersedes over time.
