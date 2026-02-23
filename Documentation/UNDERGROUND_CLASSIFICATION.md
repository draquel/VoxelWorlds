# Underground Classification System

**Modules**: VoxelGeneration (flag propagation), VoxelScatter (surface point classification)
**Dependencies**: VoxelCore (`VOXEL_FLAG_UNDERGROUND`, `EScatterSurfaceLocation`, `FVoxelSurfacePoint::bIsUnderground`)

## Overview

The underground classification system determines which voxels and scatter surface points are underground (inside caves) versus on the open terrain surface. This enables `EScatterSurfaceLocation`-based filtering so that scatter definitions can target surface-only placement (grass, surface rocks) or underground-only placement (mushrooms, cave crystals).

The system operates in two stages:

1. **Voxel flag propagation** (`VoxelCPUNoiseGenerator`): Sets `VOXEL_FLAG_UNDERGROUND` on air and solid voxels during chunk generation
2. **Surface point classification** (`VoxelScatterManager`): Maps the voxel flags onto scatter surface points, setting `FVoxelSurfacePoint::bIsUnderground`

## Voxel Flag: VOXEL_FLAG_UNDERGROUND

Stored in `FVoxelData::Metadata` flags nibble (bit 2, value `0x04`). Accessed via `HasUndergroundFlag()` / `SetUndergroundFlag()`.

```
FVoxelData::Metadata layout:
  Bits 7-4: AO (ambient occlusion)
  Bit 3:    (reserved)
  Bit 2:    VOXEL_FLAG_UNDERGROUND (0x04)
  Bit 1:    VOXEL_FLAG_CAVE (0x02)
  Bit 0:    VOXEL_FLAG_WATER (0x01)
```

The underground flag is set on:
- **Air voxels** inside caves (by cave carving and the column scan pass)
- **Solid boundary voxels** adjacent to underground air (by the boundary propagation pass)

## Flag Propagation Pipeline

`ApplyUndergroundClassificationPass()` runs at the end of `GenerateChunkCPU()`, after cave carving, water fill, and all other generation passes. It consists of two passes.

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
| `ClassifySurfacePointsUnderground` | Voxel at surface point position | Sets `bIsUnderground` on scatter surface points |
| `ExtractSurfacePointsFromVoxelData` | Air voxels in column scan | Sets `bIsUnderground` on CPU-extracted surface points |
| `ExtractSurfacePointsCubic` | Air voxels in cubic block scan | Sets `bIsUnderground` on cubic surface points |
| `VoxelTreeInjector` | Air voxel above tree base | Prevents tree placement in caves |
| `VoxelWorldTestActor` | Voxel at viewer position | Viewer underground detection |

## Surface Point Classification

`ClassifySurfacePointsUnderground()` is a static function on `UVoxelScatterManager` that classifies an array of `FVoxelSurfacePoint` using the chunk's voxel data. It runs on the thread pool as part of the async scatter generation pipeline.

### Classification Algorithm

For each surface point:

**Step 1 — Direct flag check**: Map the surface point world position to a voxel coordinate. If the voxel has `VOXEL_FLAG_UNDERGROUND`, mark underground.

**Step 2 — Normal-offset air neighbor check**: If the voxel is solid and unflagged, offset the position by `Normal * VoxelSize * 0.5` (half a voxel toward the air side) and check that voxel. This handles mesh-interpolated positions that land on the solid side of a boundary.

**Step 3 — Column scan override** (upward-facing surfaces only, `Normal.Z > 0.3`):
If the point was marked underground by Steps 1-2, verify by scanning upward from the voxel position:

1. Skip solid voxels upward until the first air voxel is found
2. If the first air is **non-underground** (surface/sky) -> **de-classify** (this is terrain surface, not a cave floor)
3. If the first air is **underground** -> measure the **headroom** (contiguous air voxels before the next solid ceiling)
4. If headroom < 3 voxels -> **de-classify** (cave is too thin; scatter meshes would poke through the ceiling)
5. If all solid to chunk top -> **de-classify** (terrain interior at chunk boundary)

The `Normal.Z > 0.3` guard ensures cave walls and ceilings retain their underground classification. These surfaces face sideways or downward, and the vertical column scan isn't meaningful for them.

### Why the Column Scan Override Is Necessary

The voxel flag alone isn't sufficient for upward-facing surfaces because:
- Pass 2 boundary propagation flags solid voxels adjacent to underground air
- A solid voxel between a cave below and terrain surface above gets flagged (adjacent to underground air below)
- The surface point on TOP of that voxel (upward-facing, terrain surface) would be incorrectly classified

The column scan distinguishes "terrain surface above a cave" from "cave floor below terrain" by checking what kind of air exists above the surface point.

### Minimum Headroom Check

Even when correctly identified as a cave floor, very thin caves (1-2 voxels tall) cause visual artifacts: scatter meshes (mushrooms, crystals) extend through the thin cave ceiling and become visible from the terrain surface above.

The headroom threshold of 3 voxels (300 units at VoxelSize=100) ensures scatter only spawns in caves tall enough to contain the mesh without clipping. This is a pragmatic visual fix, not a classification accuracy concern.

## Scatter Filtering

`FScatterDefinition::CanSpawnAt(const FVoxelSurfacePoint& Point)` checks `EScatterSurfaceLocation`:

```cpp
if (SurfaceLocation == EScatterSurfaceLocation::SurfaceOnly && Point.bIsUnderground)
    return false;  // No surface scatter in caves
if (SurfaceLocation == EScatterSurfaceLocation::UndergroundOnly && !Point.bIsUnderground)
    return false;  // No cave scatter on terrain surface
// EScatterSurfaceLocation::Any -> no filtering
```

### Data Flow

```
GenerateChunkCPU()
    ├── Cave carving → VOXEL_FLAG_UNDERGROUND on carved air
    ├── ApplyUndergroundClassificationPass()
    │       ├── Pass 1: Column scan → flag remaining underground air
    │       └── Pass 2: Boundary propagation → flag adjacent solid voxels
    │
    ▼
OnChunkMeshDataReady() → PendingGenerationQueue
    │
    ▼
LaunchAsyncScatterGeneration() [thread pool]
    ├── GPU or CPU surface extraction → FVoxelSurfacePoint array
    ├── ClassifySurfacePointsUnderground() → sets bIsUnderground
    └── GenerateSpawnPoints()
            └── CanSpawnAt() → filters by SurfaceLocation + bIsUnderground
```

## Key Files

| File | Contains |
|------|----------|
| `VoxelCore/Public/VoxelData.h` | `VOXEL_FLAG_UNDERGROUND`, `HasUndergroundFlag()`, `SetUndergroundFlag()` |
| `VoxelCore/Public/VoxelScatterTypes.h` | `EScatterSurfaceLocation`, `FVoxelSurfacePoint::bIsUnderground`, `CanSpawnAt()` |
| `VoxelGeneration/Private/VoxelCPUNoiseGenerator.cpp` | `ApplyUndergroundClassificationPass()` — Pass 1 + Pass 2 |
| `VoxelScatter/Private/VoxelScatterManager.cpp` | `ClassifySurfacePointsUnderground()` — surface point classification |
| `VoxelScatter/Private/VoxelScatterPlacement.cpp` | `GenerateSpawnPointsForType()` — calls `CanSpawnAt()` filter |

## Tuning

### MinSolidThickness (Pass 1, default: 3)

Controls how many solid voxels constitute a "cave ceiling." Lower values classify more air as underground (catches shallow caves) but may false-positive at thin terrain overhangs. Higher values require thicker terrain above before classifying as underground.

### PropagationDepth (Pass 2, default: 2)

How many iterations of boundary propagation to run. Higher values flag solid voxels deeper from cave air, improving classification of surface points that land far from the cave boundary. Values above 2 provide diminishing returns and risk flagging too much solid geometry.

### Normal.Z Threshold (Classification, default: 0.3)

The minimum upward component of the surface normal for the column scan override to apply. Lower values apply the override to more surfaces (including steep slopes), higher values restrict it to near-horizontal floors. `0.3` corresponds to roughly 72 degrees from vertical — most natural cave floors and terrain surfaces.

### Minimum Headroom (Classification, default: 3 voxels)

The minimum cave height (in air voxels above the floor) required for underground scatter to spawn. Prevents scatter in thin caves where meshes would clip through the ceiling. Increase for larger scatter meshes; decrease (minimum 1) if caves are intentionally thin.

## See Also

- [SCATTER_SYSTEM.md](SCATTER_SYSTEM.md) — Full scatter pipeline documentation
- [CAVE_SYSTEM.md](CAVE_SYSTEM.md) — Cave generation (density subtraction, cave types)
- [DATA_STRUCTURES.md](DATA_STRUCTURES.md) — FVoxelData layout and metadata flags
