# Water System Architecture

**Module**: VoxelCore (flags), VoxelGeneration (fill pass), VoxelMeshing (water mesher), VoxelStreaming (tile management), VoxelRendering (rendering)

## Table of Contents

1. [Overview](#overview)
2. [Architecture Diagram](#architecture-diagram)
3. [Water Flag Storage](#water-flag-storage)
4. [Water Fill & Propagation](#water-fill--propagation)
5. [Water Meshing](#water-meshing)
6. [2D Water Tile System](#2d-water-tile-system)
7. [Rendering Pipeline](#rendering-pipeline)
8. [Configuration](#configuration)
9. [Data Flow](#data-flow)
10. [Design Decisions](#design-decisions)

---

## Overview

The water system renders water surfaces where water-flagged air voxels exist in the world. Rather than using a single infinite water plane, the system generates per-tile water meshes that conform to the actual water boundaries, enabling correct water rendering in caves, overhangs, and island shorelines.

### Key Capabilities

- Per-voxel water flag storage in `FVoxelData::Metadata`
- CPU column-scan water fill pass during terrain generation
- BFS flood-fill propagation when terrain edits expose air adjacent to water
- 2D tile-based water meshing (decoupled from 3D terrain chunks)
- 3-cell dilation at shorelines to tuck water under terrain
- Greedy rectangle merging for minimal quad count
- Dual renderer support (Custom VF GPU renderer + PMC fallback)
- Separate water material (translucent / Single Layer Water)

---

## Architecture Diagram

```
UVoxelWorldConfiguration
    ├── bEnableWaterLevel
    ├── WaterLevel (world Z height)
    └── WaterMeshMaterial (translucent/SLW material)

FVoxelData::Metadata (per-voxel)
    └── Bit 4: VOXEL_FLAG_WATER

VoxelGeneration
    └── ApplyWaterFillPass() ← Column-scan during chunk generation

VoxelStreaming
    ├── UVoxelWaterPropagation ← BFS flood fill on terrain edits
    └── UVoxelChunkManager
        ├── PropagateWaterFromNeighbors() ← Cross-chunk boundary propagation
        ├── UpdateWaterTileContribution() ← Partial mask from chunk
        ├── RemoveWaterTileContribution() ← On chunk unload
        └── ProcessDirtyWaterTiles()      ← Time-sliced mesh generation

VoxelMeshing
    └── FVoxelWaterMesher (static utility)
        ├── BuildColumnMask()             ← bool[ChunkSize²] from voxel data
        └── GenerateWaterMeshFromMask()   ← Dilate + greedy merge + emit quads

VoxelRendering
    └── IVoxelMeshRenderer (interface)
        ├── FVoxelCustomVFRenderer (GPU) ← NEW
        │   └── UVoxelWorldComponent
        │       └── FVoxelSceneProxy (second render pass)
        └── FVoxelPMCRenderer (CPU fallback)
            └── Dedicated PMC per water tile
```

---

## Water Flag Storage

**File**: `VoxelCore/Public/VoxelData.h`

Water state is stored per-voxel in `FVoxelData::Metadata` (byte 3 of the 4-byte voxel):

```
Metadata byte layout:
  Bits 0-3: Ambient Occlusion (0-15)
  Bits 4-7: Flags nibble
    Bit 4 (flag 0): VOXEL_FLAG_WATER  = 0x01
    Bit 5 (flag 1): VOXEL_FLAG_CAVE   = 0x02
    Bit 6 (flag 2): VOXEL_FLAG_UNDERGROUND = 0x04
```

### Helper Methods

```cpp
bool HasWaterFlag() const;        // Check water flag
void SetWaterFlag(bool bHasWater); // Set/clear water flag
static FVoxelData Water();        // Create water voxel (air + water flag)
```

### Key Semantics

- Water voxels are **air** (Density < 127) with the water flag set
- Solid voxels never have water flags — they represent terrain under water
- The `VOXEL_FLAG_UNDERGROUND` flag distinguishes cave interiors where water may not propagate
- GPU generation sets water flags via per-voxel heuristic (`WorldPos.Z < WaterLevel && WorldPos.Z > TerrainHeight`)

---

## Water Fill & Propagation

### Initial Fill Pass (Generation Time)

**File**: `VoxelGeneration/Private/` (within terrain generation pipeline)

During chunk generation, `ApplyWaterFillPass()` performs a CPU column-scan:

1. For each XY column in the chunk, scan top-down from the highest Z
2. Flag air voxels below `WaterLevel` as water
3. Stop scanning at the first solid voxel (terrain surface)
4. Skip voxels with `VOXEL_FLAG_UNDERGROUND` (caves)

### Cross-Chunk Boundary Propagation

**File**: `VoxelStreaming/Private/VoxelChunkManager.cpp`
**Method**: `PropagateWaterFromNeighbors()`

Called after chunk generation completes, before water tile update. Ensures consistent water flags across chunk boundaries:

1. For each of 6 face neighbors (+X, -X, +Y, -Y, +Z, -Z):
2. Check boundary voxels: if our voxel is dry air below water level AND neighbor has water flag
3. Seed the voxel and BFS flood fill within the chunk
4. Returns true if any propagation occurred (triggers remeshing)

### Edit-Triggered Propagation

**File**: `VoxelStreaming/Public/VoxelWaterPropagation.h`
**Class**: `UVoxelWaterPropagation`

When terrain edits (dig, subtract) expose air adjacent to existing water, this system propagates water flags via bounded BFS:

**Seeding** (`OnChunkEdited`):
- Scan voxels in edit sphere's bounding box
- Find dry air below water level with a 6-connected water neighbor
- Add to BFS queue

**Processing** (`ProcessPropagation`, called per frame):
- Pop from FIFO queue, set water flag, enqueue valid neighbors
- Bounded by `MaxVoxelsPerFrame` (default 512) for smooth "water filling" effect
- Total limit of `MaxPropagationVoxels` (default 8192) per flood event

---

## Water Meshing

**File**: `VoxelMeshing/Public/VoxelWaterMesher.h`, `VoxelMeshing/Private/VoxelWaterMesher.cpp`
**Class**: `FVoxelWaterMesher` (static utility)

### Two Operating Modes

**Per-Chunk Mode** (`GenerateWaterMesh`): Scans a single chunk's voxel data, emits top-face quads at water surface boundaries. Used for simple cases.

**2D Tile Mode** (preferred): Two-step process used by the tile system:

#### Step 1: Build Column Mask

```cpp
static bool BuildColumnMask(
    const TArray<FVoxelData>& VoxelData,
    int32 ChunkSize,
    TArray<bool>& OutMask);
```

- Input: chunk's voxel data (3D)
- Output: `bool[ChunkSize * ChunkSize]` — true if ANY voxel in that XY column is water-flagged air (not underground)
- Returns: whether any water was found

#### Step 2: Generate Mesh from Combined Mask

```cpp
static void GenerateWaterMeshFromMask(
    const TArray<bool>& ColumnMask,
    int32 ChunkSize,
    float VoxelSize,
    const FVector& TileWorldPosition,
    float WaterLevel,
    FChunkMeshData& OutMeshData);
```

### Dilation & Greedy Merge Algorithm

```
1. Dilate column mask by 3 cells in each direction
   - Extends water quads past actual water boundary
   - Tucks water under terrain at shorelines
   - DilationRadius = 3 (accounts for marching cubes surface offsets)

2. Greedy rectangle merging on dilated mask
   - Scan each position in the 2D mask
   - Extend width along X axis while cells are marked
   - Extend height along Y axis for the full width
   - Emit merged quad, mark cells as processed

3. Emit water quad per rectangle
   - 4 vertices at WaterLevel Z height
   - Normal: (0, 0, 1) straight up
   - World-space UVs for seamless tiling
   - MaterialID = 254 (water identifier)
   - 2 triangles (6 indices, CW winding)
```

### Output Format

Water meshes are output as `FChunkMeshData` with:
- Positions at water level Z
- Up-facing normals
- World-space UVs for tiling
- UV1 channel: `(254.0, 0.0)` for material identification
- Vertex color R channel: 254 (water MaterialID)

---

## 2D Water Tile System

**File**: `VoxelStreaming/Private/VoxelChunkManager.cpp`

### Concept

Water tiles are indexed by `FIntVector2` (XY chunk column), independent from 3D terrain chunks indexed by `FIntVector`. A single water tile aggregates water from all Z-level chunks in that column.

### Data Structures

```cpp
struct FWaterTileState
{
    // Per-Z-level partial masks from loaded 3D chunks
    TMap<int32, TArray<bool>> PartialMasks;
    // Key = chunk Z coordinate
    // Value = ChunkSize² bool mask

    bool bDirty = true;
};

TMap<FIntVector2, FWaterTileState> WaterTiles;
TArray<FIntVector2> DirtyWaterTileQueue;
TSet<FIntVector2> DirtyWaterTileSet;  // O(1) duplicate check
```

### Lifecycle

**Chunk loads** → `UpdateWaterTileContribution(ChunkCoord)`:
1. Build partial mask from chunk voxel data via `BuildColumnMask()`
2. Store mask in `WaterTiles[XY].PartialMasks[Z]`
3. Mark tile dirty, enqueue for processing

**Chunk unloads** → `RemoveWaterTileContribution(ChunkCoord)`:
1. Remove Z's partial mask from tile
2. If no contributors remain, remove tile entirely from renderer
3. Otherwise mark dirty for re-evaluation

**Per tick** → `ProcessDirtyWaterTiles(MaxTilesPerFrame)`:
1. Pop tiles from dirty queue
2. OR-combine all partial masks into a single `ChunkSize * ChunkSize` mask
3. Call `FVoxelWaterMesher::GenerateWaterMeshFromMask()` (dilate + greedy merge)
4. Submit to renderer via `UpdateWaterTileMesh()`
5. If resulting mesh is empty, call `RemoveWaterTile()`

### Advantages

- Decouples water surface from 3D terrain chunk lifecycle
- Single water surface per XY column regardless of Z-level count
- Efficient 2D dilation and greedy merge
- Independent update rate from chunk streaming
- Seamless water at shorelines via dilation

---

## Rendering Pipeline

Water tiles are rendered through the `IVoxelMeshRenderer` interface alongside terrain chunks. Both renderer implementations support water tiles.

### Interface Methods

**File**: `VoxelRendering/Public/IVoxelMeshRenderer.h`

```cpp
virtual void SetWaterMaterial(UMaterialInterface* Material) { }
virtual void UpdateWaterTileMesh(
    const FIntVector2& TileCoord,
    const FChunkMeshData& WaterMeshData) { }
virtual void RemoveWaterTile(const FIntVector2& TileCoord) { }
virtual void ClearAllWaterTiles() { }
```

Default implementations are no-ops, allowing renderers to opt-in to water support.

### GPU Renderer (FVoxelCustomVFRenderer)

**Files**: `VoxelCustomVFRenderer.h/.cpp`, `VoxelWorldComponent.h/.cpp`, `VoxelSceneProxy.h/.cpp`

Three-layer architecture matching the terrain chunk pipeline:

```
FVoxelCustomVFRenderer   (game thread API)
    ↓ SetWaterMaterial / UpdateWaterTileMesh / RemoveWaterTile / ClearAllWaterTiles
UVoxelWorldComponent     (game → render thread bridge via ENQUEUE_RENDER_COMMAND)
    ↓
FVoxelSceneProxy         (render thread — GPU buffers, draw calls)
    ↓ GetDynamicMeshElements — second pass with water material
```

**Scene Proxy Water Data** (render thread):
```cpp
TMap<FIntVector2, FVoxelChunkRenderData> WaterTileRenderData;
TMap<FIntVector2, TSharedPtr<FVoxelLocalVertexBuffer>> WaterTileVertexBuffers;
TMap<FIntVector2, TSharedPtr<FVoxelLocalIndexBuffer>> WaterTileIndexBuffers;
TMap<FIntVector2, TSharedPtr<FLocalVertexFactory>> WaterTileVertexFactories;
UMaterialInterface* WaterMaterial = nullptr;
FMaterialRelevance WaterMaterialRelevance;
```

**GetDynamicMeshElements**: After the terrain chunk loop, a second pass iterates `WaterTileRenderData` using the water material proxy:
- Same frustum culling as terrain
- `MeshBatch.CastShadow = false` (water doesn't cast shadows)
- `MeshBatch.bUseAsOccluder = false` (water is translucent)
- Counts toward the same 500 mesh batch limit

**GetViewRelevance**: Water material relevance is OR'd with terrain relevance so the scene proxy correctly reports translucency support.

**Buffer creation**: Water vertices go through the same `FVoxelVertex → FVoxelLocalVertex` conversion pipeline as terrain, creating identical GPU buffer structures (vertex, color SRV, tangent SRV, texcoord SRV, index buffer, FLocalVertexFactory).

### PMC Renderer (FVoxelPMCRenderer)

**Files**: `VoxelPMCRenderer.h/.cpp`

Creates one `UProceduralMeshComponent` per water tile:
- Attached to the container actor
- Water material applied separately from terrain material
- Single mesh section containing all water quads for the tile
- Component pooling for reuse on tile removal/recreation

---

## Configuration

**File**: `VoxelCore/Public/VoxelWorldConfiguration.h`

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `bEnableWaterLevel` | bool | false | Master toggle for the water system |
| `WaterLevel` | float | 0.0 | Water surface Z height (InfinitePlane/IslandBowl modes) |
| `WaterRadius` | float | 100000.0 | Water sphere radius (SphericalPlanet mode) |
| `bShowWaterPlane` | bool | true | Show fallback static water plane when `WaterMeshMaterial` is null |
| `WaterMeshMaterial` | UMaterialInterface* | nullptr | Material for water tile rendering (should be translucent or Single Layer Water) |

### Material Requirements

The `WaterMeshMaterial` should be:
- **Single Layer Water** shader model (preferred) — for realistic water rendering with refraction
- **Translucent** blend mode — for simple transparent water
- Should NOT be opaque — water tiles render as a second pass and need correct translucency sorting

When `WaterMeshMaterial` is null, the system falls back to a static water plane at `WaterLevel` (controlled by `bShowWaterPlane`).

---

## Data Flow

### Full Pipeline: Chunk Load to Water Render

```
[1] Chunk Generation (GPU compute)
    ↓ ApplyWaterFillPass() — column-scan sets water flags
    ↓
[2] Chunk Meshing (terrain mesh)
    ↓
[3] PropagateWaterFromNeighbors()
    ↓ Cross-chunk boundary BFS flood fill
    ↓
[4] UpdateWaterTileContribution()
    ├─ BuildColumnMask() → bool[ChunkSize²]
    └─ Store in WaterTiles[XY].PartialMasks[Z]
    ↓ Mark dirty → enqueue
    ↓
[5] ProcessDirtyWaterTiles() (time-sliced, per tick)
    ├─ OR-combine partial masks
    ├─ GenerateWaterMeshFromMask()
    │   ├─ Dilate by 3 cells
    │   ├─ Greedy rectangle merge
    │   └─ Emit water quads (MaterialID=254)
    └─ MeshRenderer->UpdateWaterTileMesh()
        ├─ GPU: ConvertToVoxelVertices → ENQUEUE_RENDER_COMMAND → GPU buffers
        └─ PMC: CreateMeshSection on dedicated component
```

### Terrain Edit to Water Fill

```
[1] Player digs terrain
    ↓
[2] UVoxelWaterPropagation::OnChunkEdited()
    ↓ Scan edit sphere for dry air adjacent to water
    ↓ Seed BFS queue
    ↓
[3] ProcessPropagation() (bounded per frame, 512 voxels)
    ├─ BFS flood fill from seeds
    ├─ Set water flags on eligible air voxels
    └─ Mark affected chunks dirty
    ↓
[4] Chunks remesh → water tiles update (steps 3-5 above)
```

---

## Design Decisions

### Why 2D Tiles Instead of Per-Chunk Water?

**Decision**: Decouple water meshes from 3D terrain chunks using FIntVector2-keyed tiles.

**Rationale**:
- Water surface is inherently 2D (a single Z plane per XY column)
- Multiple Z-level chunks can contribute water to the same column
- Independent lifecycle from terrain chunk loading/unloading
- Simpler dilation and greedy merge on 2D data
- Avoids duplicate water quads at Z chunk boundaries

**Trade-offs**:
- Additional bookkeeping (partial masks, dirty queue)
- Water level must be flat per tile (no per-voxel water height variation)

### Why 3-Cell Dilation?

**Decision**: Dilate the water column mask by 3 cells before greedy merge.

**Rationale**:
- Marching Cubes and Dual Contouring shift the terrain surface by up to 1-2 voxels from the actual voxel boundary
- Dilation extends water quads under the terrain surface at shorelines
- Prevents visible gaps between water edge and terrain
- 3 cells provides sufficient overlap without excessive over-extension

### Why Separate Water Material?

**Decision**: Water uses a dedicated material, separate from terrain.

**Rationale**:
- Water requires translucent or Single Layer Water shading
- Terrain uses opaque materials with triplanar projection
- Separate materials allow independent optimization
- GPU renderer needs separate render passes for correct translucency sorting
- Material relevance (opaque vs translucent) must be reported correctly to the renderer

### Why Bounded BFS Propagation?

**Decision**: Limit water propagation to `MaxVoxelsPerFrame` per tick.

**Rationale**:
- Large terrain edits could expose thousands of air voxels simultaneously
- Unbounded BFS would cause frame rate spikes
- Bounded processing creates a natural "water filling" visual effect
- Per-frame budget (512) keeps propagation cost under 0.5ms

---

## References

- [Rendering System](RENDERING_SYSTEM.md) — GPU and PMC renderer details
- [Architecture Overview](ARCHITECTURE.md) — System-wide architecture
- [GPU Pipeline](GPU_PIPELINE.md) — Compute shader generation pipeline

---

**Status**: Implementation Complete (PMC + GPU Renderer)
