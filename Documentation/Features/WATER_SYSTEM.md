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

Called at each mesh submit of a chunk, before its water tile update. Ensures consistent water flags across chunk boundaries:

1. For each of 6 face neighbors (+X, -X, +Y, -Y, +Z, -Z):
2. Check boundary voxels: if our voxel is dry air below water level AND neighbor has water flag
3. Seed the voxel and BFS flood fill within the chunk
4. Returns true if any propagation occurred (the faces it flooded and the voxels, on request)

It reads and writes the chunk's **generated** voxels (`FChunkDescriptor`), not the edit layer. Water an edit-driven
flood put in the edit layer crosses chunk faces through `UVoxelWaterPropagation` instead (the chunk-arrival face scans
below, G59). Before it writes flags it drops the chunk's edit-merged snapshot (`GetEditMergedSnapshot`), which it does not
version, so the water tile reads them. A resident chunk's snapshot shares its array (G71); dropped first, the write clones
the array only while a seam job or a cook still holds it, and that reader keeps the voxels it took.

**The second pulls (world modes review G70).** A chunk pulls once when its mesh lands, and a chunk wholly under the sea
gets all of its water that way, so water that reached it only through a neighbour that flooded after it pulled never came:
which voxels flooded depended on the order the chunks finished meshing in. Now a chunk's water rising queues its face
neighbours that have already pulled (`bWaterSettled`) and reach below the sea to pull again (`QueueWaterRepulls`): across
every face when its water first settles (its generated water arrived with it), else across the faces its pull flooded.
`ProcessWaterRepulls` runs the queue each tick within about a millisecond, first in first out, on Loaded chunks only (a
chunk queued for a mesh or meshing pulls when that mesh lands, and its voxels are not written while a job may read them);
a pull that floods refreshes its water tile and queues on. The flood so reaches every dry air voxel below the sea connected
to the sea through loaded chunks, whatever the order. `voxel.Water.PullAgain` (1; 0 = one pull each, the rule before);
`voxel.Water.Stats` reports the second pulls (`GetWaterRepullStats`). Tests: `Streaming.Water.PullOrder.Plane`, `.Planet`;
`Streaming.{Planet,Plane}.FarReadsOffGameThread` compare every chunk's water between their two runs.

### Edit-Triggered Propagation

**File**: `VoxelStreaming/Public/VoxelWaterPropagation.h`
**Class**: `UVoxelWaterPropagation`

When terrain edits (dig, subtract) expose air adjacent to existing water, this system propagates water flags via bounded BFS.
Every read is of the **edit-merged** voxel (`UVoxelChunkManager::GetEditMergedVoxelAtWorldPosition`: the generated voxel
with the chunk's edit layer applied), so a dug voxel reads as air and a filled one as solid. Voxels are keyed by their
global index in the chunk grid (relative to `WorldOrigin`), and "below the sea" is tested at the voxel's sample point as
generation's fill pass tests it.

**Seeding** (`OnChunkEdited`, scanned in the next `ProcessPropagation`):
- Only voxels the edit changed can be seeds (edits make no water): the chunk-local bounds of the voxels it changed in the
  chunk (`UVoxelEditManager::GetBroadcastEditBounds`). Every edit reports them, so a brush, an operation of single-voxel
  edits (a POI's terraform, a dungeon stamp, `voxel.Edit.System`), an undo, a clear and a load all seed alike. Outside a
  broadcast the brush's box is used, else the whole chunk.
- The box is clipped to below the sea (the plane cuts its top; a box wholly above the sphere is dropped) and queued per
  chunk (a chunk broadcast again joins its box). The scans run at the start of `ProcessPropagation` within
  `MaxScanReadsPerFrame` voxel reads (32,768), so a large edit under the sea is spread over frames.
- A seed is a changed voxel that can receive water (below the sea, in a chunk whose water is in place, air without the
  flag) with a 6-connected **water-flagged air** neighbour. A filled voxel keeps the water flag of the sea it filled (`Add` keeps the
  metadata) but is solid, so it is no source: a cavity dug under a fill plug stays dry.
- A chunk's water must be in place (`UVoxelChunkManager::IsChunkWaterSettled`): it has voxel data and has submitted a
  mesh since the data arrived, which runs `PropagateWaterFromNeighbors`. A chunk wholly under a heightmap sea has no water
  flags before that (generation's column scan seeds only the chunk holding the water level). An edit of a chunk that is
  not settled (a POI's terraform or a dungeon stamp at stream-in, a save's edits, a chunk generated but not meshed yet)
  waits: its box is kept until the chunk manager calls `OnChunkWaterSettled` at that mesh submit (once per arrival of
  the chunk's voxel data), then queued.
- **Chunk arrival** (G59): a flood stops at a chunk whose water is not in place, and a dig's scan finds no water there;
  so when a chunk's water settles, `OnChunkWaterSettled` also queues the faces it shares with chunks whose water is in
  place, where either chunk has edits and the face reaches below the sea. The scan (`ScanPendingFaces`, after the edit
  boxes within the same read budget) takes each pair of voxels across the face with an edit on either side: one dry air
  below the sea, the other water-flagged air, makes the dry one a seed. A flood that stopped at the chunk continues into
  it, and a pocket dug beside it floods from the water it loaded with. A pair neither edit touched holds the generated
  water, as `PropagateWaterFromNeighbors` left it.
- The propagation's own batches are not scanned: their voxels took the flag, and their neighbours are already its
  frontier (a new seed also restarts the flood's budget, so scanning them would never let a capped flood stop).

**Processing** (`ProcessPropagation`, called per frame):
- Pop from the FIFO queue, set the water flag, enqueue valid neighbours
- The flag is set on the voxel **as it is**, edits included (`EEditMode::Set` of the edit-merged voxel plus the flag): a
  dig keeps its density and material, and no voxel goes back to its generated value
- Nothing is read or written in a chunk whose water is not in place (an unloaded chunk reads as air, and water written
  there would carve its ground away when it loads; one not meshed yet lacks the sea's flags, which the flood would copy
  into the edit layer): the flood stops at it
- Bounded by `MaxVoxelsPerFrame` (default 512) for smooth "water filling" effect
- Total limit of `MaxPropagationVoxels` (default 8192) per flood event
- Each frame's flags are one system edit operation (the source the caller had set is restored after it); its broadcast
  marks the chunks dirty as any edit's does. It waits while another edit operation is in progress
- That operation stays **out of the undo history** (`BeginEditOperation(..., bAddToUndoHistory=false)`: no undo step, the
  redo stack kept). The flood follows from the edit that let the water in: undoing that edit puts its voxels back as they
  were before it, the flood there with them (an undo restores each voxel's previous edit, `FVoxelEditChange`), and its
  redo floods them again. Water the flood put beyond the edit's own voxels (a cave or a sealed pocket it opened) stays:
  nothing drains it
- `GetStats()` counts scans, reads, seeds (`FaceSeeds` of them from the face scans, `FaceScansQueued` faces), floods and
  flooded voxels, and times the scans (`ScanSeconds`, `MaxFrameScanSeconds`); `voxel.Water.Stats [x y | pawn]` logs them
  and the water tile's column there (`UVoxelChunkManager::GetWaterTileColumn`: the chunk layers that mark it wet); the log
  line `Water propagation done: N voxels flooded from M seeds` ends each flood

Until 2026-10-20 both steps read the generated voxels without the edit layer (`GetVoxelAtWorldPosition`): a voxel the
player dug still read solid, so a dig under the sea never flooded, and a flood would have set each voxel to its generated
value plus the flag, undoing an edit there; only brushes were scanned, a flagged fill counted as water, an unloaded chunk
read as air, and keys ignored `WorldOrigin` (world modes review G58, WORLD_MODES_REVIEW_PLAN.md §19.14). Tests: `VoxelWorlds.Streaming.WaterPropagation.Plane`, `.Planet`.

**The flood's water elsewhere (G59).** A flood's water lives in the edit layer. The 2D water tiles build a chunk with
edits from its edit-merged voxels (`UpdateWaterTileContribution`, below), so a channel dug inland from the shore below sea
level draws its water surface once it floods; and the chunk-arrival face scans above carry a flood into a chunk that
loads after it. Until 2026-10-20 both read the generated voxels only (world modes review G59, WORLD_MODES_REVIEW_PLAN.md
§19.16). Tests: `VoxelWorlds.Streaming.WaterPropagation.ChunkArrival.Plane`, `.Planet`, `.Tiles.Plane`.

**The flood and undo (G60).** Until 2026-10-20 each frame's flood batch was an undo step of its own, and the edit
manager's undo record kept only each voxel's previous value: the flood replaces a dug voxel's relative edit with a Set
whose previous value was the dig's unset one, so the first undo after a dig under the sea (the demo test actor's Z) took
the flood back and removed the dig at every voxel it had reached, leaving the dig's partly dug voxels; the second undo
took the rest (world modes review G60, WORLD_MODES_REVIEW_PLAN.md §19.17). A save keeps the flood as edits in its
`.edits.bin`; loading it floods nothing again (the flooded voxels are water already). Tests:
`VoxelWorlds.Streaming.WaterPropagation.Undo.Plane`, `.Planet`, `VoxelWorlds.Edit.UndoRedo.Records`.

### On a Planet (spherical SP-B8)

The three steps above test "below the sea" with `FVoxelSeaSurface` (VoxelCore): the `WaterLevel` Z plane in
heightmap worlds, the sphere of `WaterRadius` around the planet centre on a planet (`FVoxelSeaSurface::FromConfig`).
Generation runs `ApplyRadialWaterFillPass` on planets: chunks the sea sphere crosses seed the open sea along digital
lines in the chunk's local down direction (the column scan's rule: the part above the sea must be clear from where
the line enters the chunk, then air takes the flag down to the first solid or cave voxel), a BFS spreads through
non-cave air inside the radius, and the cave flags clear. Chunks wholly under the sea fill from their neighbours as on
the plane. The 2D water tiles below are XY grids at `WaterLevel`, so they are off on planets
(`UVoxelChunkManager::UsesWaterTiles`).

**One sea for gameplay (SP-E1, spherical Phase 3).** Everything that asks "how deep under the water is this" goes
through the same type: `FVoxelSeaSurface::GetDepth(Position)`, the signed depth below the plane (`PlaneZ − Z`) or the
sphere (`Radius − |P − C|`), positive under the sea. The chunk manager hands it out (`GetSeaSurface`, BlueprintPure
`GetSeaDepthAt`): swimming and breath (`FVCVoxelNavigationHelper`), the underwater post-process's darkening, the spawn
search, the navigation water check (`FVoxelSurfaceNavParams::IsUnderwater`), the NPC spawner and scatter's underwater
class. In heightmap worlds the sea is the `WaterLevel` plane in world Z (`FromConfig`), wherever `WorldOrigin` puts the
chunk grid: the terrain's heights are world Z, generation floods air at or below `WaterLevel`, and the tiles draw there.
Every plane test works in world space, the water pull and the edit-driven flood included (review plan G19, §20.20: some
consumers used `WaterLevel + WorldOrigin.Z` until 2026-10-23). On a planet it is the sphere; since spherical Phase 4 the
map and the PCG sampler read it too.

**The ocean shell (SP-E2).** A planet's sea surface is the chunk manager's `FVoxelPlanetOcean`: a cube sphere at
`WaterRadius` on the surface coordinate's tangent warp, its resolution set so no quad sags more than 8 uu under the
sphere (32 to 160 quads along a face edge; 160 on the 250,000 uu review planet, 153,600 quads), UVs in voxels along
each face (as the tiles count voxels), the water material id in UV1, drawn with `WaterMeshMaterial`. Two sections: the
top faces outward and always shows; the underside faces inward and shows only while the viewer is inside the sea radius
(the far ocean's rule). With a one-sided material no ray meets the shell twice from either side, so it needs no sorting
and no rebuild. `voxel.World.PlanetOcean 0` hides it. Where the analytic sea passes through dry ground (a cave below
sea level), the shell shows there too: it is a sphere, not a per-chunk surface.

**The far field's sea (spherical Phase 6, WM-BP9).** Where the shell's 160 quads a face edge would sag more than 8 uu
(`FVoxelPlanetFarField::ShouldDrawSea`: from about R 3.3 x 10^5, so the 50 km and 1,000 km test planets) the chunk
manager builds no shell and the far field's patches draw the sea ([LOD_SYSTEM.md](LOD_SYSTEM.md) "The Far Field"): each
patch a section at `WaterRadius` over its cells whose terrain reaches under the sea or within two voxels of it, an 8 x 8
grid a patch, UVs in voxels of arc along the face as the shell's (less a whole number of 1,024 per patch, so the floats
stay small), the water material id in UV1, `WaterMeshMaterial`; an underside for the patches near the viewer (four
levels above the band and finer), shown from under the sea. The sections refine with the quadtree (a patch's chord sag
counts in its error), so near the viewer the sea sags under a uu, and their vertices are offsets from each patch's
origin where the shell's were floats of size R (G40). They stay drawn over the voxel window, which has no water tiles on
a planet, and like the shell they show in a dry cave below sea level. `voxel.World.FarField 0` brings the shell back.
Above the voxel cut-over (WM-BP10) they are the only sea drawn, as they are the only terrain; whether the camera is under
the sea (the underside's test) is the camera's position, not the streaming viewer's.

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

**Chunk loads or re-meshes** → `UpdateWaterTileContribution(ChunkCoord)`:
1. Build partial mask from chunk voxel data via `BuildColumnMask()`: the edit-merged snapshot when the chunk has edits
   (`GetEditMergedSnapshot`; an edit-driven flood's water is in the edit layer, G59), else the generated voxels
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
| `WaterRadius` | float | 100000.0 | Water sphere radius (SphericalPlanet mode): the sea for generation and gameplay, and the ocean shell's radius |
| `bShowWaterPlane` | bool | true | Show fallback static water plane when `WaterMeshMaterial` is null |
| `WaterMeshMaterial` | UMaterialInterface* | nullptr | Material for water tile rendering (should be translucent or Single Layer Water) |

### Material Requirements

The `WaterMeshMaterial` should be:
- **Single Layer Water** shader model (preferred) — for realistic water rendering with refraction
- **Translucent** blend mode — for simple transparent water
- Should NOT be opaque — water tiles render as a second pass and need correct translucency sorting

When `WaterMeshMaterial` is null, the system falls back to a static water plane at `WaterLevel` (controlled by `bShowWaterPlane`).

### Lumen: Translucency Reflections

With Lumen and a translucent water material (the default `M_Water` is translucent, default lit), the water's reflections come
from Lumen's radiance cache unless front-layer translucency reflections are on. Lumen marks the probes translucent surfaces
need in a pass at 1/`r.Lumen.TranslucencyReflections.MarkDownsampleFactor` of the screen; at the engine default (4) a sea
seen at grazing angles skips probe cells along the view and shows thin dark lines under its horizon, at fixed screen rows,
most visible over deep water from a swimmer's or a boat's height (world modes review G16, §20.24). Set
`r.Lumen.TranslucencyReflections.MarkDownsampleFactor=1` in the project's `DefaultEngine.ini` (`[SystemSettings]`), as this
project does: the mark pass costs about 0.02 ms more at 1280 x 720.

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
    ├─ BuildColumnMask() → bool[ChunkSize²] (edit-merged for a chunk with edits)
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
[1] An edit (player dig, system batch, undo, redo, clear, load)
    ↓
[2] UVoxelWaterPropagation::OnChunkEdited()
    ↓ Queue the voxels the edit changed in the chunk (GetBroadcastEditBounds), clipped to below the sea
    ↓ (a chunk whose water is not in place: kept until OnChunkWaterSettled)
    ↓
[3] ProcessPropagation() (per frame)
    ├─ Scan the queued boxes (32,768 reads): edit-merged dry air with a water neighbour → seeds
    ├─ BFS flood fill from seeds (512 voxels)
    ├─ Set the flag on each voxel as it is (edit layer, EEditMode::Set), in an operation outside the undo history (G60)
    └─ The operation's broadcast marks the chunks dirty
    ↓
[4] Chunks remesh; their water tiles read the edit-merged voxels (G59)
    ↓
[5] A chunk whose water settles later (loads, or meshes for the first time)
    └─ OnChunkWaterSettled(): its faces with settled chunks, an edit on either side → seeds → [3]
```

---

## Far Ocean (Islands)

**Files**: `VoxelStreaming/Public/VoxelFarOcean.h` (geometry), `UVoxelChunkManager::RebuildFarOcean` / `UpdateFarOceanGrid`

Water tiles exist only for loaded chunk columns, so an island's sea used to end at the edge of what was streamed and the
island floated in a void seen from outside (world modes review KI2). Since milestone M6 (WM-E1) a bounded heightmap world
with `bIslandFarOcean` draws the sea past that: procedural mesh components on the world actor, at `WaterLevel` with
`WaterMeshMaterial` and world-space UVs as the tiles (one seamless surface), out to `IslandFarOceanDistance` past the edge.

- **No overlap with the tiles.** The far ocean's grid covers every chunk column the streaming has not opened; a column
  opens when it holds a chunk with voxel data or has a water tile. The grid is rebuilt in the same tick the open set
  changes, after the water tiles are processed, so a tile and the far ocean never cover the same column (the material is
  translucent: two layers would draw darker) and no column is left without water. Only a window around the open columns
  is rebuilt each time; the rest of the grid only when the window moves.
- **Under it:** a seabed that continues the island's (`FIslandBowlWorldMode::GetSeabedHeight`), half a voxel under it
  so the loaded voxel seabed covers it, and a floor under unloaded columns inside the island's edge.
- **From below:** the undersides are separate sections shown only while the viewer is under the water plane (the tiles
  are drawn two-sided always).
- **Proxy recreation (G15):** the far ocean's meshes live on their own components, so a recreated terrain scene proxy
  does not lose them; nothing needs re-sending.

Details, cvars (`voxel.World.FarOcean`, `voxel.World.FarOceanInland`, `voxel.World.FarOceanStats`) and the trade-offs:
WORLD_MODES.md, "Beyond the edge".

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
