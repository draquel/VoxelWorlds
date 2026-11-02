# Underground Classification System

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Modules**: VoxelGeneration (flag propagation), VoxelScatter (surface point classification)
**Dependencies**: VoxelCore (`VOXEL_FLAG_UNDERGROUND`, `EScatterSurfaceLocationFlags`, `FVoxelSurfacePoint::bIsUnderground`)

## Overview

The underground classification system determines which voxels and scatter surface points are underground (inside caves) versus on the open terrain surface. A scatter definition's `SurfaceLocationMask` (`EScatterSurfaceLocationFlags`: `Surface`, `Underwater`, `Underground`) then targets dry surface (grass, surface rocks), seabeds, cave floors (mushrooms, cave crystals) or any combination.

The system operates in two stages:

1. **Voxel flag propagation** (`FVoxelCPUNoiseGenerator::ApplyUndergroundClassificationPass`, and the same passes on the GPU): sets `VOXEL_FLAG_UNDERGROUND` on air and solid voxels during chunk generation
2. **Surface point classification** (the scatter extractors in `VoxelScatterManager.cpp`): maps the voxel flags onto scatter surface points, setting `FVoxelSurfacePoint::bIsUnderground`

## Voxel Flag: VOXEL_FLAG_UNDERGROUND

Stored in the flags nibble of `FVoxelData::Metadata` (flag value `0x04`, Metadata bit 6). Accessed via `HasUndergroundFlag()` / `SetUndergroundFlag()`.

```
FVoxelData::Metadata layout:
  Bits 0-3: AO (ambient occlusion, GetAO)
  Bits 4-7: flags nibble (GetFlags)
    flag 0x01 (bit 4): VOXEL_FLAG_WATER
    flag 0x02 (bit 5): VOXEL_FLAG_CAVE (temporary, cleared after the water fill)
    flag 0x04 (bit 6): VOXEL_FLAG_UNDERGROUND
    flag 0x08 (bit 7): (reserved)
```

The underground flag is set on:
- **Air voxels** inside caves (by cave carving and the column scan pass)
- **Solid boundary voxels** adjacent to underground air (by the boundary propagation pass)

## Flag Propagation Pipeline

`ApplyUndergroundClassificationPass()` (`VoxelGeneration/Private/VoxelGenerationPostPasses.cpp`) runs at the end of CPU generation, after cave carving, water fill, and all other generation passes; the GPU generator runs the same two passes (`UndergroundColumnCS` / `UndergroundLinesCS` and the boundary propagation, `VoxelPostPasses.usf`). It consists of two passes.

### Pass 1: Column Scan (Air Voxels)

Scans each (X, Y) column top-down to identify air voxels that are below thick terrain — these are underground.

```
Column scan state machine (per X,Y column, scanning Z from top to bottom):

  [Optional solid at top] -> Air (bSeenAir=true) -> Solid (count thickness)
       -> If ConsecutiveSolid >= MinSolidThickness -> bUnderground=true
       -> All subsequent air voxels get VOXEL_FLAG_UNDERGROUND

  MinSolidThickness = 3 (300 units at VoxelSize=100)
```

Key design decision: **solid voxels at the column top are NOT counted toward thickness.** The `bSeenAir` guard ensures only solid encountered AFTER the first air gap (sky/surface) counts. This prevents terrain peaks, ridges, and chunk-boundary geometry from falsely triggering underground detection.

Example column (Z=31 at top, Z=0 at bottom):
```
VZ=31: Air  (sky)          -> bSeenAir=true
VZ=30: Air  (sky)          -> (still surface)
VZ=29: Solid (terrain)     -> ConsecutiveSolid=1
VZ=28: Solid (terrain)     -> ConsecutiveSolid=2
VZ=27: Solid (terrain)     -> ConsecutiveSolid=3
VZ=26: Air  (cave)         -> ConsecutiveSolid=3 >= 3 -> bUnderground=true -> FLAG SET
VZ=25: Air  (cave)         -> FLAG SET
VZ=24: Solid (cave floor)  -> (solid, no flag)
VZ=23: Air  (deeper cave)  -> FLAG SET (bUnderground stays true)
...
```

Note: Cave carving in the generation pipeline already sets `VOXEL_FLAG_UNDERGROUND` on carved air voxels (all three world modes). Pass 1 catches additional underground air that wasn't directly carved but is below thick terrain.

**On a planet** (spherical SP-B7) the same state machine runs on digital lines along the chunk centre's local
down instead of world-Z columns: lines sheared along the axis nearest to down, voxel (a, b, c) on line
(b - round(K·SlopeB), c - round(K·SlopeC)) with K the step from the chunk's upper side, so every voxel is on exactly
one line (`FVoxelDownLines`, `VoxelGeneration/Private/VoxelDownLines.h`). At mid latitudes local down is up to 55° off
every axis, and a world-Z scan reads hillsides as ceilings or misses caves whose column runs into the ground to the
chunk's top. Heightmap worlds keep the column scan. The GPU walks the same lines since spherical Phase 2
(`UndergroundLinesCS` in `VoxelPostPasses.usf`, one thread per line): the C++ uploads the lines' axis, direction and
per-step shifts as `FVoxelDownLines` computes them, so both generators flag the same voxels
(`Generation.GPUvsCPUSphericalParity`). Heightmap chunks keep `UndergroundColumnCS`.

### Pass 2: Boundary Propagation (Solid Voxels)

Scatter surface points sit at solid-air mesh boundaries. The surface point position can map to either the solid or air side of the boundary depending on mesh interpolation. Pass 2 ensures the solid side also carries the flag.

```
For each solid voxel without VOXEL_FLAG_UNDERGROUND:
  Check 6-connected neighbors:
    - If ANY neighbor has VOXEL_FLAG_UNDERGROUND -> bAdjacentToUnderground
    - If ANY neighbor is non-solid WITHOUT underground flag -> bAdjacentToSurfaceAir

  Flag the solid voxel ONLY IF:
    bAdjacentToUnderground AND NOT bAdjacentToSurfaceAir
```

The **surface exposure guard** (`!bAdjacentToSurfaceAir`) prevents flagging solid voxels that face open sky. Without this guard, cave entrance walls adjacent to both underground air and surface air would be flagged, causing surface scatter near cave mouths to be incorrectly classified as underground.

**Propagation depth**: 2 iterations. The second iteration extends the flag one voxel deeper into solid geometry, covering mesh interpolation offsets at corners and edges where surface points may land 1-2 voxels from the cave air.

### Consumers of VOXEL_FLAG_UNDERGROUND

| Consumer | What It Checks | Effect |
|----------|---------------|--------|
| `ExtractSurfacePointsByColumn` | Air voxels in column scan | Sets `bIsUnderground` on CPU-extracted surface points |
| `ExtractSurfacePointsByBlockTop` | Air voxels in cubic block scan | Sets `bIsUnderground` on cubic surface points |
| `VoxelTreeInjector` | Air voxel above tree base | Prevents tree placement in caves |

## Surface Point Classification

The scatter extractors classify each surface point as they make it: the column scan (`ExtractSurfacePointsByColumn`),
the cubic block-face scan (`ExtractSurfacePointsByBlockTop`) and, on planets, the scan along local up
(`ExtractSurfacePointsByRay`). They run on the thread pool as part of the async scatter generation pipeline. A point
is underground when:

**Step 1 — Flags**: the air voxel above the crossing carries `VOXEL_FLAG_UNDERGROUND`, or the solid voxel under it does.

**Step 2 — Not the column's top**: a lower crossing in the same column is always covered (a cave floor, or the ground under
an overhang), even where Pass 1 left its air unflagged (a ceiling thinner than `MinSolidThickness`, or a cave spanning a
chunk boundary). On a slope each column's terrain surface is its topmost crossing. On a planet the columns are the scatter's radial rays (world
modes review G31, [SCATTER_SYSTEM.md](SCATTER_SYSTEM.md)): a later floor on a ray is covered, and so is every floor of a ray
that enters the chunk in the ground.

**Step 3 — Buried chunk**, below.

**Buried chunk** (heightmap worlds): a point not yet underground is underground when its whole chunk lies under the analytic terrain, `ChunkTop < GeneratedHeight - 1.5 * VoxelSize`: the cover over it lives in a neighbouring chunk, which no per-chunk check can see. The extractors (`ExtractSurfacePointsByColumn`, `ExtractSurfacePointsByBlockTop`) apply it per column. `GeneratedHeight` is the ground the generator makes: the world mode's natural height with the chunk's terrain conditioning zones (POI pads and their falloff rings) layered on, `FScatterExtractionContext::GetGeneratedTerrainHeight`, the zones gathered by the chunk manager when the extraction is set up (`FScatterExtractionContext::ConditioningZones`). Before world modes review G23 it was the natural height, so a pad cut below a chunk's top buried the chunk holding the pad's ground (no surface scatter on it), and under a raised pad the chunk below read as open surface.

The test compares the chunk's top, not the point, so carving into a surface chunk reclassifies the exposed floor as surface.

The voxel over the chunk's top layer belongs to the chunk above, which the chunk's own data cannot show; the column and cubic scans take it from `FScatterExtractionContext::GetVoxelOutsideChunk` (the one outside-voxel source of every extractor since CR2): the generated voxel there, over `FVoxelSurfaceQuery::GenerateVoxelAt`, with the cave configuration's carve subtracted as generation does (`FVoxelCPUNoiseGenerator::CalculateCaveDensity` at its depth under the generated ground, for the column's biome, with the sea's minimum depth; `FScatterExtractionContext::CaveConfiguration`, null when caves are off), and the chunk above's edits on that layer applied (`FScatterExtractionContext::OutsideEdits`, keyed in the chunk's frame at Z = ChunkSize, copied from the chunk manager's edit layers through `UVoxelScatterManager::SetEditLayerSource`). Solid, the top voxel is no surface: the chunk above extracts the real one, and the rest of the column is covered. Air, the crossing interpolates against that voxel's density, as the mesher does; in a buried chunk it is a cave floor at the boundary, underground by the test above. Before world modes review G26 the extractors took that voxel for air, which copied the surface onto the chunk's top face up to 1.5 voxels under the ground; before G28 they still did deeper than 1.5 voxels, so a buried chunk's whole top face carried underground points inside the rock, on which underground-only scatter spawned.

## Scatter Filtering

`FScatterDefinition::CanSpawnAt(const FVoxelSurfacePoint& Point)` puts each point in one category, by precedence
Underground > Underwater > Surface, and rejects it when the definition's `SurfaceLocationMask` lacks that category:

```cpp
const EScatterSurfaceLocationFlags PointCategory =
    Point.bIsUnderground ? EScatterSurfaceLocationFlags::Underground
    : Point.bIsUnderwater ? EScatterSurfaceLocationFlags::Underwater
    : EScatterSurfaceLocationFlags::Surface;
if ((SurfaceLocationMask & static_cast<int32>(PointCategory)) == 0)
    return false;
```

### Data Flow

```
CPU or GPU generation
    ├── Cave carving → VOXEL_FLAG_UNDERGROUND on carved air
    ├── ApplyUndergroundClassificationPass()
    │       ├── Pass 1: Column scan → flag remaining underground air
    │       └── Pass 2: Boundary propagation → flag adjacent solid voxels
    │
    ▼
HandOffChunkVoxels() → PendingGenerationQueue
    │
    ▼
LaunchAsyncScatterGeneration() [thread pool]
    ├── Surface extraction from the chunk's voxels → FVoxelSurfacePoint array, bIsUnderground set per point
    └── GenerateSpawnPoints()
            └── CanSpawnAt() → filters by SurfaceLocationMask + bIsUnderground
```

## Key Files

| File | Contains |
|------|----------|
| `VoxelCore/Public/VoxelData.h` | `VOXEL_FLAG_UNDERGROUND`, `HasUndergroundFlag()`, `SetUndergroundFlag()` |
| `VoxelCore/Public/VoxelScatterTypes.h` | `EScatterSurfaceLocationFlags`, `FVoxelSurfacePoint::bIsUnderground`, `CanSpawnAt()` |
| `VoxelGeneration/Private/VoxelGenerationPostPasses.cpp` | `ApplyUndergroundClassificationPass()` — Pass 1 + Pass 2 |
| `Shaders/Private/VoxelPostPasses.usf` | The GPU passes (`UndergroundColumnCS`, `UndergroundLinesCS`) |
| `VoxelScatter/Private/VoxelScatterManager.cpp` | The extractors' classification (flags, column top); the column and cubic scans' buried-chunk test and the voxel above the top layer (`FScatterExtractionContext::GetVoxelOutsideChunk`) |
| `VoxelScatter/Private/VoxelScatterPlacement.cpp` | `GenerateSpawnPointsForType()` — calls `CanSpawnAt()` filter |

## Tuning

### MinSolidThickness (Pass 1, default: 3)

Controls how many solid voxels constitute a "cave ceiling." Lower values classify more air as underground (catches shallow caves) but may false-positive at thin terrain overhangs. Higher values require thicker terrain above before classifying as underground.

### PropagationDepth (Pass 2, default: 2)

How many iterations of boundary propagation to run. Higher values flag solid voxels deeper from cave air, improving classification of surface points that land far from the cave boundary. Values above 2 provide diminishing returns and risk flagging too much solid geometry.

## See Also

- [SCATTER_SYSTEM.md](SCATTER_SYSTEM.md) — Full scatter pipeline documentation
- [CAVE_SYSTEM.md](CAVE_SYSTEM.md) — Cave generation (density subtraction, cave types)
- [DATA_STRUCTURES.md](DATA_STRUCTURES.md) — FVoxelData layout and metadata flags
