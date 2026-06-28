# How-To: Biome-Keyed PCG Decoration

**Audience**: Setting up runtime PCG decoration (foliage/props) that varies per voxel biome.
**Design/rationale**: [PCG_DECORATION_ARCHITECTURE.md](../Research/PCG_DECORATION_ARCHITECTURE.md).
**Status**: Sampler + hybrid edit-aware surface + biome dispatcher implemented and smoke-tested
(`feature/pcg-integration`).

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

### 1. Author a decoration subgraph per biome

A decoration subgraph takes points on its **Input** node and spawns/scatters. Minimal form:

```
Input  →  Static Mesh Spawner  →  Output
```

Wire the graph **Input** node's `In` pin to the spawner's `In` pin, and the spawner's `Out` to the
**Output** node's `Out` pin. (Connecting a Spatial input to a Point input auto-inserts a `To Point`
node — that's expected.) Configure the spawner's mesh (e.g. a weighted mesh selector). Use the points'
attributes (`Slope`, `MaterialID`) inside the subgraph for density/filtering as desired.

Author one subgraph per biome (e.g. `Forest` → trees/bushes, `Desert` → rocks/cacti). They can differ
arbitrarily — that is the point of per-biome subgraphs.

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
