# Water System Architecture

> State as of 2026-11-22 (the valleys plan's V3 to V5: "Valley rivers", under "On a Planet"); 2026-11-19 (G229: a lake's
> sheet cut to its shore, in "Lakes and river reaches drawn"); 2026-11-16
> (the hydrology plan's H1 to H8: "Lakes and river reaches", "A chunk's water columns",
> "Lakes and river reaches drawn", "The river and the lake placed by hand" and "Ponds and brooks from the seed", under
> "On a Planet"; the water plan's W1, 2026-11-13: the material paragraphs; its W2: "The water levels"; the rest as of
> code review CR5, 2026-11-02).
> Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelCore (flags, the sea, the water levels), VoxelGeneration (fill pass), VoxelMeshing (water mesher), VoxelStreaming (propagation, tiles, the far ocean, the meshes of lakes and river reaches), VoxelRendering (rendering)

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
- Separate water material (Single Layer Water; a translucent one draws too)
- One query for every reader of the water: the water levels (`FVoxelWaterLevels`: the sample at a position, the range
  over bounds, the sea)
- Lakes and river reaches as bodies of the water levels, gathered by bounds from the chunk manager: generation floods
  them to their own levels on the CPU and the GPU, every reader of the water sees them, and each is drawn by one mesh
  of its own (the tiles, the far seas and the shell stay the sea's)

---

## Architecture Diagram

```
UVoxelWorldConfiguration
    ├── bEnableWaterLevel
    ├── WaterLevel (world Z height)
    └── WaterMeshMaterial (a Single Layer Water material)

FVoxelData::Metadata (per-voxel)
    └── Bit 4: VOXEL_FLAG_WATER

VoxelGeneration
    └── ApplyWaterFillPass() ← Column-scan during chunk generation

VoxelStreaming
    ├── UVoxelWaterPropagation ← BFS flood fill on terrain edits
    ├── FVoxelWaterTiles       ← The 2D tiles: masks, dirty queue, meshing
    └── UVoxelChunkManager
        ├── PropagateWaterFromNeighbors() ← Cross-chunk boundary propagation
        ├── UpdateWaterTileContribution() ← Partial mask from chunk (FVoxelWaterTiles::Contribute)
        ├── RemoveWaterTileContribution() ← On chunk unload (FVoxelWaterTiles::Remove)
        └── ProcessDirtyWaterTiles()      ← Time-sliced mesh generation (FVoxelWaterTiles::ProcessDirty)

VoxelMeshing
    └── FVoxelWaterMesher (static utility)
        ├── BuildColumnMask()             ← bool[ChunkSize²] from voxel data
        └── GenerateWaterMeshFromMask()   ← Dilate + greedy merge + emit quads

VoxelRendering
    └── IVoxelMeshRenderer (interface)
        └── FVoxelMeshRenderer
            └── UVoxelRegionComponent
                └── FVoxelSceneProxy (second render pass)
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
- GPU generation runs the CPU fill pass's phases as compute shaders (`WaterColumnSeedCS`, `WaterLineSeedCS` on a planet, `WaterFloodSweepCS` / `WaterFloodSweepRadialCS`; `Shaders/Private/VoxelPostPasses.usf`), so both generators flag the same voxels

---

## Water Fill & Propagation

### Initial Fill Pass (Generation Time)

**File**: `VoxelGeneration/Private/VoxelGenerationPostPasses.cpp`

During chunk generation, `FVoxelCPUNoiseGenerator::ApplyWaterFillPass()` runs three phases:

1. **Column seed** (in a chunk holding the water level or wholly under it): a column open from the chunk's top down to
   `WaterLevel` takes the water flag from there down to the first solid or cave-flagged voxel. In a chunk wholly under the
   level the scan starts at the chunk's top layer, so a column whose top voxel is air is open sea: a heightmap column is
   solid below its surface and air above it, caves aside, and caves are barriers here (G129; before it such a chunk had no
   water of its own and `PropagateWaterFromNeighbors` flooded all of it on the game thread when its mesh landed, up to
   17 ms a chunk under `SmoothTerrain_Island`'s lakes)
2. **Flood**: a BFS spreads the flag through non-cave air below the sea
3. **Clear** the cave flags (a temporary barrier: caves below the sea stay dry)

Where a lake or a river reach may reach the chunk the pass reads a level per column in place of the one level ("A
chunk's water columns", below): each column seeds from its own level, and the flood passes a voxel at or under the
level over its own column.

### Cross-Chunk Boundary Propagation

**File**: `VoxelStreaming/Private/VoxelChunkManager.cpp`
**Method**: `PropagateWaterFromNeighbors()`

Called at each mesh submit of a chunk, before its water tile update. Ensures consistent water flags across chunk boundaries:

1. For each of 6 face neighbors (+X, -X, +Y, -Y, +Z, -Z):
2. Check boundary voxels: if our voxel is dry air below water level AND neighbor has water flag
3. Seed the voxel and BFS flood fill within the chunk
4. Returns true if any propagation occurred (the faces it flooded and the voxels, on request)

"Below water level" is the sea's test at the voxel, or, in a chunk a lake or a river reach may reach, the voxel
against the level over its own column (`UVoxelChunkManager::IsVoxelUnderWater`, "A chunk's water columns" below), so
the pull floods to the levels generation's fill did.

It reads and writes the chunk's **generated** voxels (`FChunkDescriptor`), not the edit layer. Water an edit-driven
flood put in the edit layer crosses chunk faces through `UVoxelWaterPropagation` instead (the chunk-arrival face scans
below, G59). Before it writes flags it drops the chunk's edit-merged snapshot (`GetVoxelSnapshot`), which it does not
version, so the water tile reads them. A resident chunk's snapshot shares its array (G71); dropped first, the write clones
the array only while a seam job or a cook still holds it, and that reader keeps the voxels it took.

**The second pulls (world modes review G70).** A chunk pulls once when its mesh lands, and a chunk wholly under the sea
got all of its water that way (since G129 on the plane and G132 on a planet it has its open sea from generation), so
water that reached it only through a neighbour that flooded after it pulled never came:
which voxels flooded depended on the order the chunks finished meshing in. Now a chunk's water rising queues its face
neighbours that have already pulled (`bWaterSettled`) and reach below the sea to pull again (`QueueWaterRepulls`): across
every face when its water first settles (its generated water arrived with it), else across the faces its pull flooded.
`ProcessWaterRepulls` runs the queue each tick within about a millisecond, first in first out, on Loaded chunks only (a
chunk queued for a mesh or meshing pulls when that mesh lands, and its voxels are not written while a job may read them);
a pull that floods refreshes its water tile and queues on. The flood so reaches every dry air voxel below the sea connected
to the sea through loaded chunks, whatever the order (before G70 each chunk pulled once). `voxel.Water.Stats` reports the
second pulls (`GetWaterRepullStats`). Tests: `Streaming.Water.PullOrder.Plane`, `.Planet`.

### Edit-Triggered Propagation

**File**: `VoxelStreaming/Public/VoxelWaterPropagation.h`
**Class**: `UVoxelWaterPropagation`

When terrain edits (dig, subtract) expose air adjacent to existing water, this system propagates water flags via bounded BFS.
Every read is of the **edit-merged** voxel (`UVoxelChunkManager::GetVoxelAt`: the generated voxel
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
  mesh since the data arrived, which runs `PropagateWaterFromNeighbors`. A chunk wholly under the sea has its open sea
  from generation (G129 on the plane, G132 on a planet), and the pull adds what reaches it across its faces. An edit of a chunk that is
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
- In a world with a lake or a river reach every "below the sea" above is "at or under the level over the voxel's own
  column" (`UVoxelChunkManager::IsVoxelUnderWater`): a dig inside a body's footprint and under its level floods when
  it reaches the water, outside the footprint it stays dry, and nothing drains (the hydrology plan's D149). The seed
  clip takes the range over the edit's box and the face test the range over the face (a lake may stand over one end of
  it). The flood is made when the world has a sea, a lake or a river reach: with the sea off and a lake too
- `GetStats()` counts scans, reads, seeds (`FaceSeeds` of them from the face scans, `FaceScansQueued` faces), floods and
  flooded voxels, and times the scans (`ScanSeconds`, `MaxFrameScanSeconds`); `voxel.Water.Stats [x y | pawn]` logs them
  and the water tile's column there (`UVoxelChunkManager::GetWaterTileColumn`: the chunk layers that mark it wet); the log
  line `Water propagation done: N voxels flooded from M seeds` ends each flood

Until 2026-10-20 both steps read the generated voxels without the edit layer (`GetGeneratedVoxelAt`): a voxel the
player dug still read solid, so a dig under the sea never flooded, and a flood would have set each voxel to its generated
value plus the flag, undoing an edit there; only brushes were scanned, a flagged fill counted as water, an unloaded chunk
read as air, and keys ignored `WorldOrigin` (world modes review G58). Tests: `VoxelWorlds.Streaming.WaterPropagation.Plane`, `.Planet`.

**The flood's water elsewhere (G59).** A flood's water lives in the edit layer. The 2D water tiles build a chunk with
edits from its edit-merged voxels (`UpdateWaterTileContribution`, below), so a channel dug inland from the shore below sea
level draws its water surface once it floods; and the chunk-arrival face scans above carry a flood into a chunk that
loads after it. Until 2026-10-20 both read the generated voxels only (world modes review G59). Tests: `VoxelWorlds.Streaming.WaterPropagation.ChunkArrival.Plane`, `.Planet`, `.Tiles.Plane`.

**The flood and undo (G60).** Until 2026-10-20 each frame's flood batch was an undo step of its own, and the edit
manager's undo record kept only each voxel's previous value: the flood replaces a dug voxel's relative edit with a Set
whose previous value was the dig's unset one, so the first undo after a dig under the sea (the demo test actor's Z) took
the flood back and removed the dig at every voxel it had reached, leaving the dig's partly dug voxels; the second undo
took the rest (world modes review G60). A save keeps the flood as edits in its
`.edits.bin`; loading it floods nothing again (the flooded voxels are water already). Tests:
`VoxelWorlds.Streaming.WaterPropagation.Undo.Plane`, `.Planet`, `VoxelWorlds.Edit.UndoRedo.Records`.

### On a Planet (spherical SP-B8)

The three steps above test "below the sea" with `FVoxelSeaSurface` (VoxelCore): the `WaterLevel` Z plane in
heightmap worlds, the sphere of `WaterRadius` around the planet centre on a planet (`FVoxelSeaSurface::FromConfig`).
Generation runs `ApplyRadialWaterFillPass` on planets: chunks the sea sphere crosses seed the open sea along digital
lines in the chunk's local down direction (the column scan's rule: the part above the sea must be clear from where
the line enters the chunk, then air takes the flag down to the first solid or cave voxel), a BFS spreads through
non-cave air inside the radius, and the cave flags clear. A chunk wholly under the sea seeds every line from where it
enters it, as a heightmap chunk under the level does (G132): a planet's terrain is solid under its surface and air above
it along local down, caves aside. Before G132 such chunks had no water of their own and `PropagateWaterFromNeighbors`
flooded them on the game thread when their meshes landed (2.9 ms a frame on average and 12.5 at P95 along
`SmoothTerrain_Planet`'s equator). The 2D water tiles below are XY grids at `WaterLevel`, so they are off on planets
(`UVoxelChunkManager::UsesWaterTiles`).

**The water levels: one query for every reader (the water plan's W2, 2026-11-13; SP-E1 before it).** Nothing reads
the configuration's `WaterLevel` or `WaterRadius` but the configuration, its validation and one builder
(`FVoxelWaterLevels::FromConfig`, the one caller of `FVoxelSeaSurface::FromConfig`). Every reader asks the water
levels, `FVoxelWaterLevels` (VoxelCore): an immutable snapshot of the bodies of water over some bounds, a value cheap
to copy to a worker, with three ways in, by what a reader does:

| A reader that | Asks | Who |
|---|---|---|
| tests a point | `Sample(Position)`: whether a body covers the column, which body, its surface level there, the position's depth (positive under the surface, `FVoxelSeaSurface::GetDepth`'s sign; `NoWaterDepth`, -1e9, with no water), the surface's normal and, for a river reach, its flow. The cheap forms for a reader that runs per voxel or per vertex: `GetDepth`, `IsUnder` (at or below the surface: the flood's own test, exact where a sphere's depth sign is not), `IsAbove` and `Crosses` for a box | swimming, breath, the underwater view and the crossing effects (`FVCTerrainQueryHelper`), the spawn search, the NPC deep-water test, scatter's underwater class, the navigation's cell test (`FVoxelSurfaceNavParams::IsUnderwater`), `GetSurfaceOrSeaLevel`, the descent prefetch, the cut-over's surface under the viewer, the water pull and the edit-driven flood (per voxel), the surface's material in the far field, the map and the PCG sampler (`FVoxelSurfaceQuery`), a map pixel where a lake or a river reach lies over its tile, a voxel tree where one lies over its chunks, the debug commands |
| copies one level for an area | `GetRange(Bounds)`: `bAny` (a body lies over or under a column of the box), `bUniform` (one level over it: the scalar is the truth there; false wherever a lake's or a river reach's footprint may reach the box), the lowest and highest level, and the bounds asked, so a copy of a level always says where it holds | a chunk's generation request (and through it both generators and the shaders), the LOD window's slab per column and a planet's near layers over the near disk, the pull's and the flood's seed clip over their box, a water tile over its column, a map tile and its edit overlay while their range is uniform, a navigation region's frame on a planet, the POI placement's water line and the planet's sky over the whole world |
| draws the sea itself | `GetSea()`: the sea body's surface, `FVoxelSeaSurface` | the far field's sea, the island's far ocean, a planet's ocean shell, the demo actor's fallback plane |

A **water level** is a surface's height in the world's own vertical measure: world Z on a heightmap world, the
distance from the planet centre on a planet (so the sea's is `WaterLevel` or `WaterRadius`). It is not an altitude,
which is measured from the world mode's datum; a planet reader that wants the sea as an altitude subtracts the datum
radius, in double, as it always did. The snapshot carries the frame its levels are measured in (`FVoxelWaterFrame`:
plain Z, or the planet's centre and datum radius), so a body is evaluated with the sea off too.

In heightmap worlds the sea is the plane at its level in world Z, wherever `WorldOrigin` puts the chunk grid: the
terrain's heights are world Z, generation floods air at or below the level, and the tiles draw there. Every plane test
works in world space, the water pull and the edit-driven flood included (world modes review G19: some consumers used
`WaterLevel + WorldOrigin.Z` until 2026-10-23; test `Streaming.Plane.OneSeaPlane`). On a planet it is the sphere.

W2 changed no output: with the sea alone every range is uniform and every sample is the sea's own arithmetic (tests
`Core.WaterLevels.SeaOnPlaneAndPlanet`, `Core.WaterLevels.NoWater`, `Streaming.Water.LevelsQuery`; the gates in
`Documentation/WATER_EPIC_PLAN.md`, W2's result). Two rules keep a later change of a body's level possible (the water
plan's D112; no level changes today): a reader holds a body as a record or asks again, never a level it has baked in
for good, and every copy of a level names the bounds it was taken over (a request is one chunk's, a tile's level is its
column's, the LOD slab's is cached per column and dropped when the bodies change).

**Lakes and river reaches (the hydrology plan's H1, 2026-11-15).** A body of water beyond the sea is a record with a
**footprint**, the columns it may hold water over (`FVoxelWaterBody`: an id that is not the sea's 0, a kind, a level,
the bounds its water lies in, and the footprint). Water is in a column of the footprint where the generated ground
lies under the level; the footprint does not draw the shore, it keeps the body's level off other low ground nearby.

- **A lake** (`FVoxelWaterLake`, `FVoxelWaterBody::MakeLake`) has one level and a footprint that is a grid of cells:
  in world XY on a heightmap world (`Make`; `MakeFromOutline` takes the cells whose centres lie inside a drawn polygon,
  grown by one cell), in the plane tangent to the datum sphere at the lake's centre on a planet (`MakeOnSphere`: a
  direction's cell is its datum point's offset from the centre's along the plane's two axes; `MakeFromOutlineOnSphere`
  takes an outline's corners as directions, puts the centre at their middle and the corners on that plane as a
  direction's cell is read, and keeps the cells inside, grown by one). Both outline forms take a filter, asked of each
  cell inside the outline at its centre: a hand-placed lake keeps the cells whose ground lies under its level.
- **A river reach** (`FVoxelWaterReach`, `FVoxelWaterBody::MakeReach`) is a polyline, upstream to downstream, each
  vertex a position (world XY, or a unit direction on a planet), a bed level, a surface level and the footprint's half
  width (the bed's half width and its falloff). Its level at a place is the surface level interpolated on the nearest
  segment, so the surface is level across the river and slopes along it. The nearest segment is found by the search a
  conditioning line uses (`FVoxelPolylineBounds`, VoxelCore: the blocks of segments with their bounds, the nearest
  place on a segment, the earlier segment on a tie), and a reach merges coincident vertices by the line's own rule, so
  a reach and a bed made from the same vertices are read at the same place along the same segment (test
  `Streaming.Water.Bodies.BedAndReach`: the reach's bed level is the line's target at every point of the footprint, on
  a plane and a planet).
- **The sample** takes the bodies whose footprints cover the position's column and answers with the highest level
  among them: a pond inside a larger lake where it stands higher, a river reach over the lake or the sea it runs into
  until it comes down to that body's level. The sea's level stands until a body is strictly higher, and of two bodies
  at one level the lower id answers. Over a river reach the normal is the local up tilted against the slope and the
  flow is the direction downstream, along the surface, times the reach's speed (`GetFlowSpeed`; nothing reads the
  flow until the plan's H6).
- **The range** over a box holds the levels of the sea and of every body whose footprint may reach the box's columns
  (a lake by its cells, a river reach by the blocks of its segments), and is uniform only when no such body does.
- **The cheap forms** are the sea's own arithmetic while the snapshot holds the sea alone (`IsSeaAlone`), and with a
  body the sample's depth and its sign (`IsAbove` and `Crosses` conservatively, by the bodies' levels over the box).

**Gathering by bounds.** The chunk manager holds the bodies added to it (`AddWaterBody`: a hand-placed river or
lake) and a list of sources (`AddWaterBodySource`, `IVoxelWaterBodySource`: `GatherWaterBodies(Bounds, Need,
OutBodies)` and `GatherWaterBodiesOnSphere`, the twin of `IVoxelTerrainConditioner`), in `FVoxelWaterBodySources`, the
twin of `FVoxelConditioningSources`. Both register on `OnInitialized`, before the first generation request, as
conditioning does; one registered later is warned about and counted with the late conditioning sources. The gathers
take the need the line gathers have (`EVoxelConditioningGather`, now in VoxelCore: `Complete` for a reader that must
agree with the generated voxels, `Ready` for one whose region is too wide to wait for a body).

| Entry | Gives | Who |
|---|---|---|
| `SampleWater(Position)`, `GetWaterDepth`, BlueprintPure `GetWaterDepthAt` | the point gather: the sample of the body over a position. With no body in the world it is the sea's own arithmetic; with one, the snapshot of the place (a cell a chunk across, gathered `Complete`) is kept for the 16 places asked last, so a sample stays a lookup | the swimmer, the breath, the underwater view and the crossing effects, a floating item and a body (through the character plugin's bridge), the spawn fallback, the NPC deep-water tests, the navigation's cell, the cut-over's surface under the camera, `GetSurfaceOrSeaLevel`, the island's seabed samples, the debug commands |
| `GatherWaterLevelsAt(Position)` | that place's snapshot, for a reader that takes a few samples close together | `QuerySurfacePoint`, the surface NPC spawn check |
| `GatherWaterLevels(Box, Need)`, `GatherWaterLevelsOnSphere`, `GatherWaterLevelsForChunk` | a snapshot of the sea and the bodies whose footprints may reach the box's columns (its XY on a heightmap world, the cap it subtends on a planet); with no body in the world, the sea's | a chunk's scatter extraction and the voxel trees over a chunk and the chunks around it (`Complete`); a far-field patch's build, a map tile and its edit overlay, the spawn search (`Ready`); the PCG sampler (the need its conditioning takes) |
| `GetWaterRange(Box, Need)` | the range of that gather, with no snapshot made where the world has no body | the LOD window (`IVoxelLODStrategy::SetWaterRangeGather`: a column's slab `Complete`, a planet's near disk `Ready`) |
| `GetWaterLevels()` | the sea's snapshot, over the whole world: for what means the sea itself | the water tiles, the far field's sea, the far ocean, the shell, the planet sky, the POI placement's water line, and a generation request's one level |
| `GatherWaterLevelsForChunk(ChunkCoord)`, `IsVoxelUnderWater(ChunkCoord, Local)` | the levels a chunk generates with, and whether a voxel is at or under the level over its own column, as the fill tests it | a chunk's generation request (the bodies it carries), the water pull, the edit-driven flood |

When a body is added, or a source announces one it worked out after a `Ready` gather missed it
(`NotifyWaterBodiesChanged(Region)`, `NotifyWaterBodiesChangedOnSphere`), the chunk manager drops the point gather's
snapshots and the LOD window's column slabs, rebuilds the far field's patches there and fires `OnWaterBodiesChanged`
with the region (a body's footprint as a box or a cap, or everywhere for a source). The map draws the tiles the region
meets again, and the character plugin's bridge passes it to the framework's `UCGFWaterSubsystem::NotifyWaterChanged`.
`voxel.Water.Stats` lists the sea and the bodies added to the world, whether a source is registered, and the point
gather's counters.

**What each reader does with a level that varies** (the plan's section 5.5): a map pixel is water when its ground
lies under the level over that pixel, and its depth shade is from that level (the tile's edit overlay takes a level a
column); the navigation's cell asks the world's sample at the cell, in a planet's region frame too, and water deeper
than the wade depth blocks a walker as the sea does; a voxel tree is tested against the level over its own column by
the injector and by the tree query alike (`FVoxelTreeInjector`'s `WaterOver`), so no tree stands in a pond; scatter's
underwater class, the far field's and the PCG sampler's surface material are the sample's over the point; the LOD
window keeps a column's chunks up to the highest level over it. The water tiles, the far field's sea sections, the far
ocean and the shell stay the sea's. Generation, the pull and the flood read a level per column ("A chunk's water
columns", next), so a lake's voxels carry the water flag and the swimmer, who asks the voxel's flag first, swims in
one. A body is drawn by its own mesh ("Lakes and river reaches drawn", below), and a tile's mask holds the sea's
columns only.

**A chunk's water columns (the hydrology plan's H2, 2026-11-15).** A generation request copies one level, the range
over the chunk, and that stays so wherever the range is uniform: every chunk of a world with no body, and every chunk
no body's footprint may reach. Where a lake or a river reach may reach the chunk, the request also carries the water
levels gathered over it (`FVoxelNoiseGenerationRequest::WaterBodies`, from `GatherWaterLevelsForChunk`, complete, as
its conditioning), and the generators build the chunk's water columns from them, `FVoxelChunkWaterColumns`
(VoxelGeneration): the level over each XY column on a heightmap world, or over each down-line on a planet
(`FVoxelDownLines`, the lines the post-passes walk: a planet chunk has no columns; a line takes the level over its
first voxel in the chunk), with a value for no water. One function builds the list (`Build`) and every reader of "is
this voxel under the water here" reads it, as with a chunk's conditioning line segments:

- **The CPU generator.** The density loop compares a column's terrain with its own level for the underwater
  materials and the caves' underwater minimum depth, so a lake's bed is an underwater bed; the fill seeds each column
  from its own level and floods a voxel at or under its own column's level.
- **The GPU.** The dispatch uploads the same float4s once: the density pass reads them as `WaterColumnData`, and the
  fill runs four kernels that read them in place of the one level ([GPU_PIPELINE.md](GPU_PIPELINE.md), "The water
  over a column", "Stage 2"). The kernels of a chunk with one level are as they were.
- **The pull and the flood.** `UVoxelChunkManager::IsVoxelUnderWater` asks `FVoxelChunkWaterReader`, which makes
  the same rows a column at a time, the first time a voxel of the column is asked about, from the same gather; the
  chunk manager keeps the readers of the 64 chunks asked last and drops them when the bodies change. A chunk no body
  may reach is answered by the sea's own arithmetic.

A row holds the level twice, in the two forms the generators compare with (world Z on a heightmap world; on a planet
the level as an altitude over the datum for the terrain, and the level less the chunk anchor's distance from the
centre for a voxel, both formed in double), and a lake's **minimum cave depth** (`FVoxelWaterBody::MinCaveDepth`, 0
by default): over the lake's footprint no cave opens above that depth, as under a conditioning line, in both
generators and in `FVoxelSurfaceQuery`'s cave test. A river reach has none: its bed's line carries the caves' depth.

What follows from a level per column: water stands in a column of a body's footprint where the ground is under the
level and nowhere else, so low ground beside a lake stays dry and a lake ends in a wall of water where its footprint
ends over ground under its level (a footprint reaches past the water by the bank's width, so a well-made body has
none). Where the sea stands in a column of the footprint the higher level answers. A chunk under a lake takes the
lake's water from its top layer, as a chunk under the sea does (G129), and a chunk over it takes none. On a small
planet a down-line wanders from the radial by a voxel or two across a chunk, so the footprint's edge in the voxels
follows the lines, not the sphere: the fill, the pull and the flood agree with each other exactly, and with the point
gather's sample but along that edge.

H2 changed no output: no world has a body, a request without one builds no list, and the gates read as the base (the
plan's H2 result). Tests: `Generation.WaterColumns.Fill` (made ground: a column inside, the shore, low ground
outside the footprint, a chunk the footprint crosses into, the chunks under and over the lake, a cave beside the
water, with the sea off, under the ground and a voxel under the lake; a river reach along a channel),
`Generation.WaterColumns.Planet` (a row a down-line against the sample at each voxel; the reader's rows are the
list's; the fill), `Generation.WaterColumns.OneLevel` (a chunk generated column by column from a lake that covers
none of its columns is the chunk generated from the one level, byte for byte); `Generation.GPUvsCPUWaterBodiesParity`
(a lake and a river reach on its bed on a plane and a planet, a lake with a minimum cave depth over caves, a lake with
the sea off, the chunk under a lake; in the editor); `Streaming.Water.PullOrder.LakePlane`, `.LakePlanet` (the pull's
order with a lake in the sea's place); `Streaming.WaterPropagation.Lake.Plane`, `.Planet` (a streamed world with the
sea off and a lake: generation against the columns, a dig in the lake's bed fills, a pit outside the footprint stays
dry, a channel fills as far as the footprint goes).

**Lakes and river reaches drawn (the hydrology plan's H3, 2026-11-15; D147).** Each lake and each river reach is
drawn by one mesh of its own, made from its record, with an underside; the sea's drawing does not know them.

- **The geometry** (`FVoxelWaterBodyMesh::Build`, VoxelStreaming; no engine object, so a test reads it). A lake on a
  heightmap world is a level sheet over its footprint's cells, merged into rectangles; on a planet a quad a cell on
  the sphere of the lake's level, the cells sharing their corners (`FVoxelWaterLake::GetGridPoint`). A river reach is
  a ribbon: two vertices a reach vertex, half the footprint's width to either side of it at the vertex's surface
  level, so the ribbon is level across the river and slopes along it as the sample's level does; at a bend the two
  edges are mitred (to twice the half width at most). Without more, the sheet covers the whole footprint and the
  ground hides it where the ground stands over the level, as the sea's sheet is hidden by a coast. A lake made by a
  maker that reads the ground (the hand-placed lake and the generated pond, below; not on a block world, whose
  cells are its columns) carries the natural ground at its cells' corners and centres
  (`FVoxelWaterLake::WithGround`, G229), and its sheet is cut to the shore: a cell whose corners and centre all lie
  under the level and a margin (`FVoxelWaterBodyMesh::ShoreMarginVoxels`, a tenth of a voxel; none at a corner on
  the footprint's outline) is drawn whole, and a cell of the shore as four triangles (two corners and the centre),
  each cut where the ground, linear between them, rises over the level and the margin, the cut points shared with the
  cells beside it. The outline takes no margin because on a gentle shore the ground at the level and a margin can lie
  past the footprint's cell of shore, and a sheet drawn out to the outline there shows its steps again: the sheet
  reaches the outline only where the ground there lies under the level (a rim narrower than a cell). A cut piece
  thinner than a ten-thousandth of a cell is left out: on a planet three points nearly in line on the grid's plane
  make a triangle on the sphere whose facing the sphere's curve decides. Up close the ground hides the sheet
  past the shore whether it is cut or not; beyond the voxel window the far field's coarser ground does not, and an
  uncut sheet showed its footprint's square cell of shore along the edge (G229): a cut one shows the shore's own line,
  no further up it than the margin. A river reach's ribbon is not cut (from far its edges are toothed where the far
  field's ground crosses it, and it reads as one line). A vertex is an offset from the mesh's
  origin (the lake's middle, the reach's middle vertex), so the floats stay small on a far body. The first texture
  coordinate counts voxels (across and along a reach; the grid's two axes on a lake), the second holds the water
  material id and, on a reach, its flow speed; a reach's tangent is the direction downstream and its normal the local
  up tilted against the slope. The underside is the top's vertices with the triangles turned and the top's normals,
  as on the sea sphere.
- **The components** (`FVoxelWaterBodyMeshOwner`, the chunk manager's `WaterBodyMeshes`): one procedural mesh
  component a body, the top a section that always shows and the underside one that shows while the camera is under
  that body's surface inside its footprint (the body's own sample at the camera, as the far seas use the camera and
  not the streaming viewer). A lake takes the configuration's `WaterMeshMaterial`, a river reach its
  `RiverMeshMaterial` (the lake's while that is unset). The bodies drawn are those a gather (`Ready`) finds within
  the far field's distance of the viewer (four chunks at least): they are gathered when the bodies change
  (`OnWaterBodiesChanged`'s cause) and when the viewer has moved an eighth of that distance, a body whose record is
  the same keeps its component, and one beyond the distance loses it. Nothing is made on a dedicated server, in an
  editor world, or with neither material set (`UVoxelChunkManager::DrawsWaterBodies`). `voxel.Water.BodyMeshes 0`
  hides them; `voxel.Water.Stats` counts them.
- **The tiles stay the sea's.** A tile's column mask takes a chunk's flagged columns; where the chunk has water
  columns of its own (a body may reach it), a column counts only when its level is the sea's
  (`UpdateWaterTileContribution`), so no tile draws under a lake or a river and the sea's tile beside a river mouth is
  as it was. The far field's sea sections, the far ocean and the shell read the sea alone, as before.
- **The river's look.** The master water material has a static switch, `RiverFlow` (off on the sea's instances, so
  their shader is the one without it): with it on the ripples drift along the mesh's tangent, downstream, at the
  reach's speed from the second texture coordinate (the material's `RiverSpeed` while a reach carries none), at full
  strength whatever the wind. The instances and the configurations' `RiverMeshMaterial` are made by the parent
  project's water material script.

Tests: `Streaming.Water.BodyMesh.Lake`, `.BodyMesh.Reach` (the geometry against the record on a plane and a planet:
the level at each vertex, the sheet over every footprint cell and no other, the head and the mouth, the planet's
vertices on the sphere of the level, the winding, the underside); `.BodyMesh.LakeShore` (a lake in a bowl whose
ground its footprint carries, on a plane and a planet: the ground read back, every point under the level under the
sheet and none over the level and its margin, the sheet's area the bowl's at the level and the margin where the
uncut sheet's is the cells', the cut points shared; a shallow bowl whose ground at the level and the margin lies past
the footprint: no corner of the outline over the level drawn); `Streaming.Water.BodyMeshes.Plane`, `.Planet` (a
streamed world with a lake and a river reach: a mesh a body with its material, the underside by the camera's place,
the switch, a body kept across a change elsewhere, no tile layer wet in the lake's column while a sea column's is,
nothing drawn without a material, the dedicated server's rule).

**The river and the lake placed by hand (the hydrology plan's H4, 2026-11-15; D144).** Two components turn a spline
on their actor into a body, as `UVoxelTerrainLineComponent` turns one into a line (`WORLD_MODES.md`, "The hand-placed
line"; the how-to is `Guides/TERRAIN_LINES.md`, "A river and a lake"): `UVoxelRiverComponent` on `AVoxelRiverActor`
("Voxel River" in the Place Actors panel) and `UVoxelLakeComponent` on `AVoxelLakeActor` ("Voxel Lake"), both
`UVoxelWaterBodyComponent` (VoxelStreaming). The actors are in the level on every machine and are not replicated; a
body registers at BeginPlay when the voxel world has started, else on `UVoxelChunkManager::OnInitialized`, before the
first generation request, and from then on it is a body like any other: generation floods it, the readers answer
it, a mesh draws it.

- **The order.** A river is made against the water it meets, so the first body of a world to register makes the ones
  that come before it register first (`RegisterBody`): every lake before any river, and of two rivers the one with
  the higher `Order`, then the one whose actor's name sorts first. The order is the level's alone, the same on every
  machine and whichever actor began play first. A body's id is a hash of its actor's and its component's names
  (`GetWaterBodyId`; never the sea's 0).
- **A river** (`BuildRiver`) samples its spline with the terrain line's own sampler
  (`UVoxelTerrainLineComponent::BuildLineParamsFromSpline`, with a lower-only line of the river's widths: a vertex
  every two voxels, the corners rounded where the spline breaks the bend bound), reads at each vertex the natural
  ground on the line, the lowest natural ground of its two banks (from the floor's edge to the footprint's, a voxel
  apart; a bank that lies in another body counts as that body's surface) and the level of the body the vertex lies
  in (every body of the water levels gathered over the river is asked: a river reach wherever its footprint covers
  the place, a lake or the sea where the ground is under its level; the highest), and gives the vertices a profile
  with `FVoxelConditioningLineShape::MakeRiverProfile`, the lower-only twin of `MakeProfile` (below). From the
  vertices the profile keeps it makes the **bed**, a lower-only conditioning line whose targets are the bed's
  heights and whose half width is the floor's (a unit where the river narrows to nothing), with the component's
  `Surface` (a band, the caves' depth, a clear width) and an id as a terrain line's (`GetLineId`), and the **river
  reach**, the same vertices with the bed's and the surface's levels and a footprint of the floor and one bank
  (`FVoxelWaterReach::Make`, with `FlowSpeed`). The bed is added first (`AddConditioningLine`), then the body.
- **The profile** (`MakeRiverProfile`: the plan's section 5.3; plain positions and heights, pure). The surface is what
  was asked for, lowered where it must be and never raised: the ground along the line averaged over
  `SmoothingDistance` and never over the ground itself, less `SurfaceBelowGround`, or at the spline's height the
  spline's own heights. Then, from the head down: (1) it never rises, a vertex over one upstream of it is lowered to
  it; (2) it stays `BankMargin` under the lowest ground of the banks, and from a low bank on; (7) inside another body
  it lies a tuck (10 uu) under that body's level, so that body answers there; and in a world with a sea no surface
  lies under the sea's level less the tuck (ground under the sea holds the sea's water whatever a river says, so a
  river down a beach goes no lower). The bed is the surface less `Depth`
  times a taper: (4) from nothing at a head to full over `HeadLength`, unless the first vertex lies in a body (a river
  that leaves a lake starts at the lake's level at full width, and its bed notches the rim: rule 6); (3) to nothing
  again over the last `HeadLength` of a river whose last vertex lies in no body, which is an end on land and is
  warned about. (5) Of the vertices inside the body a river ends in, and inside the one it starts in, the three next
  to the land are kept, so the mesh ends under that body's surface. (8) Where the bed's grade at a vertex that turns
  is over what the bend bound leaves there, the bed's higher end is lowered (a pass from the end, one from the
  start), by no more than the river's depth there again: the surface stays and the water is deeper, a head still
  starts from nothing, and a line made from the bed keeps the bound's second part through the bends of a river that
  is not steep and tightly curved at once (one that is keeps the line's bend warning). The result counts what was
  lowered (rises of the given heights, low banks, beds deepened), the deepest cut and the fall.
- **A lake** (`BuildLake`) reads the natural ground a voxel apart along its closed spline (an open one is closed
  straight from its last point to its first), takes its level (`LevelMode`: the lowest ground along the outline less
  `LevelMargin`, so the drawn rim holds; or the actor's own height) and makes the footprint from the outline with the
  filter above: the cells inside whose ground lies under the level, grown by one cell (`CellSizeVoxels`, 4). So the
  footprint is the water and a cell of shore, as the plan's section 5.1 asks of a maker of footprints, and the sheet
  no wider: it carries the natural ground at its cells' corners and centres, so the sheet is cut to the shore (G229,
  above; not on a block world). The record's depth is the deepest cell read and a chunk; `MinCaveDepth` (8) is the body's. No ground
  moves. A lake with nothing under its level adds no body.
- **What they say** (`GetBodyWarnings`, logged as "Water body <actor>: ..."): what was done about the spline's bends
  (the terrain line's sentence); a spline at its own height that rises downstream or stands over a bank, with how
  many vertices and by how much; a river that ends on land; the limits the bed's line breaks (the cut and fill cap
  above all: a river that follows the terrain cuts through a rise), but for the narrow core and band a head and a
  dry end have on purpose; a lake with no water; an outline whose own ground lies under the level (the water ends in a wall
  there); ground under the level in the footprint's grown cells outside the outline (a rim narrower than a cell);
  an outline that is not a closed loop. A body is added whatever they say.
- **What they read.** The natural ground, as a terrain line does, so the same on every machine and whatever else
  registers: a road's cut and fill or a pad beside a river is not seen. And the bodies the chunk manager holds when
  the body is made: the lakes and the rivers before it in the order, and a source's (the generated ponds and
  brooks of a world that has them, below: the network is registered before any actor's component).

Tests: `Generation.RiverProfile.Rules` (made ground: a slope, a low bank, a rise of the ground, given heights that
rise or stand over the banks, a mouth, a head in a lake, the sea's floor), `.Bends` (the bed's grade through a curve
on a steep slope, against the engine's own bend check; the limit on how far a bed is deepened); `Core.WaterLevels.Lake` (an outline on a planet, the kept cells);
`Streaming.HandPlacedWater.Plane`, `.Island`, `.Planet`, `.Cubic` (a river down a course of the natural ground to
the sea and a lake round a hollow, as actors: the order, the profile's rules read back from the record and the
ground, then a streamed world whose voxels hold both bodies' water and whose sample is the lake's and the river's
with its flow), `.Rules` (a spline that climbs, an end on land, a head in a lake, a tributary, a lake over its
outline and one with no water, a river that begins play before the world, two worlds making the same river);
`Map.WaterBodies.HandPlaced.Plane`, `.Planet` (the map's pixels over both); and in the game module
`VoxelEngine.Water.HandPlaced.Plane`, `.Island`, `.Planet`, `.Cubic` (the pawn swims in the lake and is carried
down the river, following its surface down; an item floats in each).

**Ponds and brooks from the seed (the hydrology plan's H7 and H8, 2026-11-16; D145, D146, D151).** A world whose
configuration names a hydrology configuration (`UVoxelWorldConfiguration::HydrologyConfiguration`, a
`UVoxelHydrologyConfiguration` asset holding `FVoxelHydrologySettings`) has lakes and river reaches generated from its
seed and its natural ground, with no terrain change but the beds: springs run downhill, a run long and gentle enough
is a river with its bed, and a hollow one meets fills as a lake. In every world mode (`FVoxelHydrology::IsEnabled`):
the plane and the island since H7, a planet and a block world since H8 ("On a planet", "On a block world", below). A
world that names none has no generator, no source and no term in its fingerprint: it is what it was.

- **The generator** (`FVoxelHydrology`, VoxelGeneration: pure functions of the seed and a ground function,
  `FVoxelHydrologyGround`). The natural ground is read on a grid of cells (`CellVoxels`, 8), aligned to the world
  (`FVoxelHydrologyGrid`: a cell's place, its neighbours, a step's length, the lattice's squares).
  - *Springs.* A square of `SpringSpacingCells` cells holds one candidate, placed by a hash; it is a spring when it
    stands `MinSpringHeight` over the sea (`GetSpring`). The count of brooks goes with the square of the spacing, and
    the game layer places no POI on a body: 48 cells is the default (a square of 288 m on the demo), since a spring
    every 24 cells left 4 of 9 cities and 4 of 15 villages around the demo's start without a site.
  - *Descents.* A spring's water steps to the neighbour with the steepest fall, cell by cell. At a pit it takes the
    pit's lake and goes on from the lake's way out; it ends in the sea, in a lake with no outlet, or as no river when
    it is longer than `MaxRiverLength` (`TraceSpring`, a `FVoxelHydrologyTrace`). The way on from a cell is the cell's
    alone, whichever descent comes to it: inside a hollow it is the hollow's own way to its saddle (a breadth-first
    walk from the cell before the saddle), and from a saddle it is the steepest fall where that leads out of the
    hollow, else the cell the flood found beyond it. A descent that comes into a hollow over the hollow's own saddle
    is cut back to the saddle and goes on by the outlet, and so is one whose step from another lake's saddle lands in
    the hollow: two hollows that meet in one saddle cell at one level, the first's water spilling into the second
    (G231, fixed 2026-11-17: 65 cells with two ways on in the streamed world test's network before it, none after).
    A descent is a river when it ends in the sea or in a kept lake,
    is `MinRiverLength` long at least, and falls no more than `MaxMeanGrade` over its length.
  - *A pit's lake* (`MakeLake`, a `FVoxelHydrologyLake`): one flood from the pit, lowest cell first. While it climbs,
    the cells it has taken are the hollow under the level it has reached. When it takes a cell under that level it has
    passed a saddle and goes on over the ground beyond: if that ground runs away (it reaches the sea, falls further
    than a lake may be deep, or is wider than a lake may be) the saddle is the rim; if the flood climbs out of it
    again, it was one more hollow of the same lake. So two pits in one hollow give one lake, from either pit, and no
    lake needs another made first (the spike's rule needed the lakes below, and left rivers without an end). A hollow
    deeper than `MaxLakeDepth` holds a lake that deep with no outlet; one wider than `MaxLakeCells` or longer than
    `MaxLakeSpanCells` holds the largest hollow under the caps, with no outlet. Where a flood closes a lake depends
    on the pit it started from, so a closed lake is the one the flood from its own lowest cell finds (the flood goes
    again from there, up to four times while it finds a lower one): every pit of a capped hollow whose flood comes to
    that cell has the same lake, and two pits of one hollow give one lake or two that share no cell (G233, fixed
    2026-11-17; before it two pits of one wide hollow gave two lakes with their own levels, which overlapped).
  - *A lake's water* comes from a second flood on a grid of two voxels a cell, inside the hollow and one cell round
    it: the generator's cell (6 m on the demo) is coarser than a rim is wide, and water stands in every column of a
    footprint whose ground lies under the level. The fine flood's rim is the lowest ground every way out of the
    hollow's surroundings crosses (never over the hollow's own rim); the footprint is the cells it took under the
    level and one fine cell round them; the level is `LakeLevelMarginVoxels` under the rim and under every ground on
    the footprint's edge, read a voxel apart. Off a block world the footprint carries the natural ground at its
    cells' corners and centres, for the sheet to be cut to the shore (G229, "Lakes and river reaches drawn"). A lake
    is a body when its water is `MinLakeDepthVoxels` deep and covers
    `MinLakeArea`. A lake is found by any descent that comes to its pit, a river's or not.
  - *River systems* (`BuildSystem`, a `FVoxelHydrologySystem`): the rivers that end at one mouth form a tree, and a
    river reach is a chain of it between a spring, a join, a lake and the mouth. A reach's floor is as wide as the
    springs that drain through it (`HalfWidthVoxels`, wider by `HalfWidthGrowth` a doubling, to `MaxHalfWidthVoxels`).
    Each chain is shaped as a line (a vertex every two voxels, its corners spread until no vertex turns more than the
    bend bound allows, through its two ends, which stay), reads its ground, its banks and the bodies it meets as a
    hand-placed river does (`FVoxelRiverCourse`, shared with `UVoxelRiverComponent`) and takes the river profile
    (`MakeRiverProfile`). A chain that other chains end at starts at the lowest surface they arrive with, so the
    surface never rises from one reach to the next, and a chain that ends at a join lies a tuck under the reach it
    joins over its last bank's width. A reach comes to the lake it ends in at that lake's level (the profile's
    `MinSurface`, as for the sea). A system is whole or not at all: when a bed breaks the bend bound it is made again
    with its corners spread wider and its pools deeper (three tries), and when a bed would lie deeper than
    `MaxCutVoxels` or still breaks the bound, the system has no reach. The lakes stay.
  - *Any region finds the same bodies* (`BuildRegion`): a descent depends on the ground within the longest river's
    length of its spring, a lake on its pit, a system on the springs within that length of its mouth; a region works
    out the springs within twice that length of itself. Ids come from cells: a reach's from its chain's first cell, a
    lake's from its lowest cell and its size, a bed's line id from the chain's first cell, over every road's and
    trail's between POIs and under a hand-placed line's of order 0.
- **On a planet** (H8). A region is a cap (`FVoxelConditioningRegion::OfCap`), a place a unit direction, a height an
  altitude over the datum (the ground is the planet's own altitude function, read along the radial), and a body's
  level the datum radius more.
  - *The grid* is a cube sphere's (`FVoxelCubeSphereGrid`): as many cells along a face's edge as bring a cell nearest
    to `CellVoxels` voxels at a face's middle, in whole lattice squares, so a square's cells lie on one face. A cell is
    an atlas coordinate and its place its middle's direction. Its neighbours are the cells it shares a side or a
    corner with on the sphere, found by index across a face's edge (`FVoxelCubeSphereGrid::GetAdjacentCell`: two
    faces' cells meet edge to edge along the edge they share): eight of them, seven for the three cells round each of
    the cube's corners, and the same relation from both sides. A step is as long as the arc between the two cells'
    middles, and the steepest fall is by that length (a cube sphere's cells are not all one size). So a descent, a
    flood and a river are the same cells whichever face's region asks, which is what the plan asked of H8 first.
    `Neighbour`, the step along a face's own plane, lands within rounding of a cell's border near a cube's corner
    and is not used.
  - *A lake's width* is measured east and north of its pit on the plane tangent there, in cells' widths. *A lake's
    water* is worked out on the plane tangent at the hollow's lowest cell, the plane its footprint lies in
    (`FVoxelWaterLake::MakeOnSphere`): a fine cell belongs to the generator's cell its direction lies in.
  - *A river system's chains* are shaped on the datum sphere (each sampled and spread point brought back to it), the
    banks are read across the line on the tangent plane (`FVoxelRiverCourse::ReadBanks` with the vertices' ups), and
    a reach and its bed are a planet's: directions, the bed's targets altitudes, the reach's levels distances from
    the centre. A bed's line id counts an atlas coordinate from 0.
- **On a block world** (H8; `FVoxelHydrologyGround::bBlocks`: cubic meshing on a heightmap). The descents and the
  hollows read the natural height, as on a smooth world: block tops are the same height over whole terraces, and a
  descent over them would find a pit at every step.
  - *A lake's water is whole blocks.* The fine flood runs on the block columns (a fine cell is a column, read at
    its lower corner as the generators read it) and on their block tops (`FVoxelBlockSurface::BlockTopForHeight`), so
    the rim holds at its own block's height with no margin: the water's top is the lowest block top on the way out,
    and no higher than the lowest column beside the footprint. The body's level lies a tenth of a voxel under that
    top, inside the layer whose blocks are the water's top ones (a level on a layer's boundary would flag the layer
    over it: the fill takes the layer a level lies in), and the sheet is drawn there.
  - *A bed steps a block a column at most* (D151), as a road's line does: the river profile holds the bed's grade to
    `FVoxelHydrology::GetBlockBedGrade` (`FRiverProfileParams::MaxBedGrade`: a segment steeper than that has its
    higher end lowered, with no limit of its own; the surface stays, the water there is deeper, and the cut's cap
    judges the result), and the engine's own check of a line on a block world (`FVoxelConditioningLineLimits::bBlockWorld`)
    refuses a system whose bed still breaks it.
- **The network** (`FVoxelHydrologyNetwork`, VoxelStreaming) keeps what is worked out: a descent by its spring's
  square, a lake by its pit, a system by its mouth, and by tile (`TileCells` cells) the bodies and beds that reach it.
  It is the world's source of generated bodies (`IVoxelWaterBodySource`) and of their beds (`IVoxelTerrainConditioner`,
  lines only), made by the chunk manager in `Initialize` and registered before `OnInitialized` fires, so a hand-placed
  river meets a generated lake and the game's POI layer reads the generated bodies. A complete gather works a missing
  tile out then and there (counted: `SyncTiles` on the game thread once the world streams); a gather for a reader that
  takes what is at hand queues it for the worker and the tile is announced when it arrives with a body
  (`UpdateHydrology`: the far field and the map draw again there); a gather over more tiles than a reader of the voxels
  asks for gets what is kept. The chunk manager warms the tiles within `WarmUpViewDistances` view distances of where
  the world will first stream before it returns from `Initialize`, in parallel, and has the worker keep that disc and
  the wider one of `FarViewDistances` worked out around the viewers. `HoldsWaterBodies` says whether a region's tiles
  are kept: the game layer asks it before it places for a reader that takes what is at hand. What such a reader is
  still waiting for past that disc the game layer asks for by region (`RequestRegion`, G232: its roads and pads far
  from the viewers): the worker walks the region's tiles itself, so the asking thread does nothing more, and queues
  those that are not kept, the nearest to a place first, up to a limit per ask. `GetLoad` gives the cache's size and
  what is queued without walking the cache (`GetStats` walks every tile), so the asker can wait for an idle worker.
  On a planet (H8) a tile is a cell of a cube sphere grid of its own, about `TileCells` cells wide, and its region
  the cap round it; the gathers, the warm-up, the announcements and `HoldsWaterBodiesOnSphere` take caps. What costs
  nothing on a plane (the tiles of a box are a rectangle) is a flood through the tiles' neighbours there, so three
  things keep a planet of any size cheap: a cap wider than a gather's limit is given up by its area, before any tile
  is looked at; a warm-up that does not wait hands its disc to the worker, which goes through its tiles; and a cap
  inside a disc a warm-up has gone through whole is held without a look at its tiles (else tile by tile, as far as
  the first that is not kept). A reader of the levels' range over a wide cap at every update (the LOD window's near
  disk: `GetPlanetNearLayersCeiling`) gets it from each kept tile's own range, with no body listed
  (`IVoxelWaterBodySource::GetLevelRangeAtHandOnSphere`, through `UVoxelChunkManager::GetWaterRange`), and keeps it
  for the viewer's cell of the slab grid: listing and sorting the thousands of bodies of a 50 km planet's near disk
  several times a frame held a frame for seconds.
- **Drawing.** A generated body is drawn as any body is, by a mesh of its own (H3), but only near the viewer: a lake
  within `LakeDrawDistance` (6 km) and a river reach within `ReachDrawDistance` (2.5 km), and neither further than the
  far field draws the ground. A world that generates hydrology has thousands of bodies inside the far field's 20 km,
  most of them brooks under a pixel wide there, and a mesh each for all of them cost the demo 14 to 20 ms every time
  the meshes were made again; the bodies within the two distances cost 1 to 2.4 ms. The meshes are made again when
  tiles arrive with bodies, no more often than every 30 frames. Past its distance a lake is the far field's ground
  with no water on it, and a brook its bed. A river or a lake placed by hand in such a world is drawn as far as in
  any world.
- **The fingerprint.** `FVoxelWorldModeFactory::MakeFingerprint` ends with `Hydrology=<version>:<hash of the settings
  that decide the bodies>` in a world that generates hydrology (`FVoxelHydrology::Version`, 2 since G231 and G233;
  `FVoxelHydrologySettings::GetGenerationHash`), so its saves made without it, or with other settings, are refused.
  The generation version does not move: a world that names no hydrology configuration generates what it did.
- **What it costs.** On the demo at the default lattice a tile (0.15 km2) takes about 0.3 ms of one core to work
  out (the descents, the floods, the systems' profiles), four times that at a spring every 24 cells; the bodies of a
  region depend on the ground 1.6 km round it at the default longest river. A world's start works out the tiles
  round it and, for the game layer's placement, those within 11 km in parallel: about half a second with the roads'
  warm-up, which reads the bodies of its searches' boxes. `voxel.Water.Stats` reports the network (tiles, springs, rivers, systems, lakes, who worked the tiles
  out); `voxel.Water.Hydrology.Survey` works a window out, reads every river reach back against the plan's rules and
  logs a digest two machines compare; `voxel.Water.Hydrology.TraceSync 1` logs who had a tile worked out on the game
  thread.

Tests: `Generation.Hydrology.Lakes` (made ground: a hollow on a slope, two pits in one hollow, a hollow that spills
into a lower one, the depth, width and span caps, a dip too shallow; no column of water against a footprint's edge),
`.Regions` (rolling ground with a sea: two regions that overlap, a region round both and a run with a cache another
region filled give each body with the same record; every river reach against the rules; a cap no bed keeps refuses
every system whole), `.Fingerprint`; `Streaming.Hydrology.Network` (the gathers by need, the worker, the
announcements, the warm-up, the counters), `.World` (a streamed plane: the network there at the registration, a
generated lake's and a river reach's voxels flagged and sampled, no tile worked out on the game thread along a walk);
in the POI layer `POI.WaterBodies.Source`. For H8: `Core.CubeSphere.AdjacentCells` (every cell's neighbour has the cell
among its own, eight of them but seven round a cube's corner; the cap gather against a count of all cells);
`Generation.Hydrology.Planet` (a made planet: a hollow on a face's edge and one at a cube's corner each one lake with
no column of water against its footprint's edge; two caps that overlap across the edge, a cap round both and a run
with a cache give each body with the same record; the rules, with river reaches that run across the edge),
`.Blocks` (a block lake's water whole blocks and watertight in blocks; over ground with a cliff no bed steeper than
a block a column, beds lowered for it); `Streaming.Hydrology.Planet`, `.Cubic` (a streamed planet and a streamed
block world: a generated lake's and a river reach's voxels flagged and sampled, a gather over a cap, no tile worked
out on the game thread along a walk). The gate in a play world is `Scripts/qa/hydrology_gate.py`.

**Valley rivers (the valleys plan's V3, 2026-11-22; D179).** A world with valleys (`WORLD_MODES.md`, "Valleys") and
hydrology has a river on the floor of every valley, which the brooks run into. A traced river is one system a mouth,
whole or not at all; a valley has no length bound, so its river is made a link at a time.

- **A piece a link** (`FVoxelHydrology::BuildValleyRiver`, `VoxelHydrologyValleys.cpp`; a system with
  `bValleyRiver`, its mouth the link's cell, one reach). The link's curve from the network (`FVoxelValleySegment`s,
  upper node to lower; a head's from half the link on, where its floor is whole) is cut back at both ends to a
  straight chord into its node, rounded to a radius the winding fits inside, wound across the floor (the room is
  `RiverWinding` of the floor's half width less the river's and the bank, `RiverHalfWaves` half waves, nothing at the
  two nodes), and rounded again to the bend bound's turn (`FVoxelConditioningLineShape::RoundCorners`). A node's
  corner is split between the piece in and the piece out and rounding keeps a line's ends, hence the chords; the
  footprint and the bed are round at the node and cover the corner's outside. Where the winding still folds on a
  tight bend and the course breaks the bound, the piece winds over half the room, a quarter, then none: no piece is
  refused, a valley is never dry. The width is the segment's river half width (by drainage, the network's).
- **Its surface** is the floor or the ground there, whichever is lower, less `SurfaceBelowGroundVoxels`, held from
  falling under a tuck under the sea, and never rising: from its upper node's level to its lower node's, so the
  pieces meet at each node at one place and one level, and a piece into a coast ends a tuck under the sea. A node's
  level is the same whichever piece asks: its own floor or ground (whichever is lower) less the margin, and no
  higher than the level of any valley link into it, so it never stands over a node upstream (since V4: taken from the
  node's own ground alone, a node a neighbouring valley's drowned mouth had cut low left a piece of the demo 1.7 m
  under the next). A head starts at its own first place and no lower than the node it runs to. A coast's
  floor lies `MinFloorHeight` under the sea, so a valley comes down to a drowned mouth (a flat at the sea's very level
  is neither sea nor land, and a river reaching it would end on land), and a piece into a coast keeps only the
  `BodyVertices` past the shore that a brook keeps inside the body it ends in; a piece with no more than two places
  on land is no river (a valley of one link at a coast, its head half way along where the ground is already under the
  sea: refused as "a valley river that starts in the sea"). A head
  starts from nothing over `HeadLength`. The bed is the surface less the depth, lowered through the bends as a
  brook's (rule 8, `FVoxelConditioningLineShape::HoldBedThroughBends`, shared with `MakeRiverProfile`), a lower-only
  line with the brooks' band and cave depth. The reach's id is `MakeValleyReachId` (the seed and the link), its bed
  line's `MakeValleyBedLineId`, apart from the brooks'.
- **The brooks.** A descent that comes into a valley river's water ends there (`EVoxelHydrologyEnd::ValleyRiver`):
  the river is made as one that ends in the sea, its reaches meeting the valley river's pieces near its mouth as
  water. `TraceSpring` gathers the pieces within the longest river of the spring.
- **The network** keeps the pieces by link (`IVoxelHydrologyCache::FindValleyRiver` / `StoreValleyRiver`) beside
  the systems; a region (`FVoxelHydrology::GatherValleyRivers`, `BuildRegion`) holds the pieces of the links whose
  segments reach it. A piece depends on its link alone, so any region, tiling or cache gives it the same record.
- **Drawn** out to `ValleyRiverDrawDistance` (6 km; `FVoxelWaterBody::DrawDistance`, which a body's source may set
  and the chunk manager's body meshes read before the kind's distance), a brook's 2.5 km being too short for a river
  seen from the hills.
- **On a planet** (the valleys plan's V5) a piece is worked on the datum sphere: the network's points lie on it (its
  centre's frame), every point of the course is brought back onto it after each sampling, rounding and winding (a mean
  of points on a sphere lies under it, as a brook's chain is shaped), the winding runs across the course on the plane
  tangent at each point (up x along), the vertices are directions and the levels the altitude plus the datum radius,
  the bed a planet line (`bRadial`) whose check reads a segment's sag. A node's neighbours are the lattice's on the
  sphere (`FVoxelValleyNetwork::GetNeighbour`), so a node's level is the same from both sides of a face's edge. A
  region is a cap (`GatherValleyRivers` takes the conditioning's region, a box or a cap; a cap's links by the chord
  round its middle), and a descent ends in a piece's water by `SampleOnSphere`.
- **On a block world** a piece's bed steps a block a column at most, as a brook's does: after the bends a segment
  steeper than `GetBlockBedGrade` has its higher end lowered, from the end up and from the start down; the course and
  the surface are the smooth world's, and the bed's check is the block world's (`bBlockWorld`).
- **The fingerprint** is the valleys' term (`FVoxelValleyNetwork::Version` 2 since V3: the same settings make other
  bodies); a world with no valleys makes the bodies it did.

Tests: `Generation.Hydrology.ValleyRivers.Pieces` (made hills falling to a sea, the network carved: every valley link
has its piece or would start in the sea, no surface rises, a piece meets the piece below at the node and never under it, a head starts from
nothing, a piece into a coast ends a tuck under the sea, the course keeps to its valley's floor, no bed breaks a
line's limits, every head leads to the sea), `.Regions` (two regions that overlap and a run with a cache give each
valley river the same id and vertices), `.Brooks` (descents end in a valley river, and every brook's end lies in
water at its level, less at most the tuck, as does every valley river's round three mouths); in the game module
`VoxelEngine.Valleys.Demo` reads the same rules on VoxelDemo's own valleys within 4 km of its spawn (the joints
there are what the made hills did not have). On a made planet (`.Planet`): every valley link but those that start in
the sea has its piece, its vertices directions, no surface rising, the pieces meeting at the nodes (across a face's
edge too) and never under one another, heads from nothing, mouths a tuck under the sea, no bed note; two caps that
overlap give the pieces they share the same. On a block world (`.Blocks`): no bed steps more than a block a column, the
engine's check finds nothing, and the pieces are the smooth world's but for a bed lowered. The survey (`voxel.Water.Hydrology.Survey`) counts the pieces and reads
them back against the same rules as any reach; `hydrology_gate.py --valleys [--swim=river]` plays a world with
valleys given in memory, and VoxelDemo has them since V4.

H1 changed no output: no world has a body, a gather with none is the sea's snapshot, and the gates read as the base
(the plan's H1 result). The cost of the point gather was measured by `Streaming.Water.Bodies.Plane` and `.Planet` in a
development editor: about 0.04 microseconds a sample on a plane and 0.07 on a planet with a lake over the place,
against 0.06 and 0.08 with the sea alone (which builds its snapshot from the configuration at each call). Tests:
`Core.WaterLevels.Lake`, `Core.WaterLevels.Reach` (the sample, the range and the cheap forms on a plane and a planet,
at a bend, at a join, at a mouth, a pond inside a lake); `Streaming.Water.Bodies.Plane`, `.Planet` (the gathers, the
point gather, a source, the announcement), `.BedAndReach`; `Map.WaterBodies.Plane`, `.Planet` (a pixel, and a pond
announced after the tiles were drawn); `Navigation.WaterBodies`; `Cubic.VoxelTrees.Lake`.

**The sea sphere (SP-E2).** A planet's sea surface is the chunk manager's `FVoxelPlanetOcean`: a cube sphere at
`WaterRadius` on the surface coordinate's tangent warp, its resolution set so no quad sags more than 8 uu under the
sphere (32 to 160 quads along a face edge; 160 on the 250,000 uu review planet, 153,600 quads), UVs in voxels along
each face (as the tiles count voxels), the water material id in UV1, drawn with `WaterMeshMaterial`. Two sections: the
top faces outward and always shows; the underside faces inward and shows only while the viewer is inside the sea radius
(the far ocean's rule). The underside keeps the top's outward normal, so a material tells "seen from below" by the view
direction against that normal, as it does on a tile's back face; no ray meets the shell twice from either side, so it
needs no sorting and no rebuild. `voxel.World.PlanetOcean 0` hides it. Where the analytic sea passes through dry ground (a cave below
sea level), the shell shows there too: it is a sphere, not a per-chunk surface.

**The far field's sea (spherical Phase 6, WM-BP9).** Where the shell's 160 quads a face edge would sag more than 8 uu
(`FVoxelFarField::ShouldDrawSea`: from about R 3.3 x 10^5, so the 50 km and 1,000 km test planets) the chunk
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

### Operating Mode

**2D Tile Mode**: a two-step process used by the tile system (the per-chunk `GenerateWaterMesh` had no caller and was
deleted in CR1, MESH-24):

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

**Files**: `VoxelStreaming/Public/VoxelWaterTiles.h` (`FVoxelWaterTiles`, the chunk manager's owner of the tiles since
code review CR4), `VoxelStreaming/Private/VoxelChunkManager.cpp` (the masks it builds)

### Concept

Water tiles are indexed by `FIntVector2` (XY chunk column), independent from 3D terrain chunks indexed by `FIntVector`. A single water tile aggregates water from all Z-level chunks in that column.

### Data Structures

```cpp
class FVoxelWaterTiles
{
    struct FTile
    {
        TMap<int32, TArray<bool>> PartialMasks;  // chunk Z -> ChunkSize² wet-column mask
        bool bDirty = true;
    };
    TMap<FIntVector2, FTile> Tiles;              // XY chunk column -> its water
    TArray<FIntVector2> DirtyQueue;
    TSet<FIntVector2> DirtySet;                  // O(1) duplicate check
};
```

`FVoxelWaterTileGrid` carries what meshing needs: the chunk size, voxel size, `WorldOrigin` and the sea plane
(`WaterLevel`, G19).

### Lifecycle

**Chunk loads or re-meshes** → `UpdateWaterTileContribution(ChunkCoord)`:
1. Build partial mask from chunk voxel data via `BuildColumnMask()`: the edit-merged snapshot when the chunk has edits
   (`GetVoxelSnapshot`; an edit-driven flood's water is in the edit layer, G59), else the generated voxels
2. `FVoxelWaterTiles::Contribute` stores it as the tile's `PartialMasks[Z]`, marks the tile dirty and queues it

**Chunk unloads** → `RemoveWaterTileContribution(ChunkCoord)` (`FVoxelWaterTiles::Remove`):
1. Remove Z's partial mask from tile
2. If no contributors remain, remove tile entirely from renderer
3. Otherwise mark dirty for re-evaluation

**Per tick** → `ProcessDirtyWaterTiles(MaxTilesPerFrame)` (`FVoxelWaterTiles::ProcessDirty`):
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

Water tiles are rendered through the `IVoxelMeshRenderer` interface alongside terrain chunks, by `FVoxelMeshRenderer` (the
only renderer since the PMC renderer was deleted in code review CR1).

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

### GPU Renderer (FVoxelMeshRenderer)

**Files**: `VoxelMeshRenderer.h/.cpp`, `VoxelRegionComponent.h/.cpp`, `VoxelSceneProxy.h/.cpp`

Three-layer architecture matching the terrain chunk pipeline:

```
FVoxelMeshRenderer   (game thread API)
    ↓ SetWaterMaterial / UpdateWaterTileMesh / RemoveWaterTile / ClearAllWaterTiles
UVoxelRegionComponent     (game → render thread bridge via ENQUEUE_RENDER_COMMAND)
    ↓
FVoxelSceneProxy         (render thread — GPU buffers, draw calls)
    ↓ GetDynamicMeshElements — second pass with water material
```

With render regions (`RENDERING_SYSTEM.md`, "Render regions") a tile goes to the component of the region holding its
column's Z-0 chunk, which over a lake is air with no mesh: the proxy draws its water tiles with no chunk mesh (its
`GetDynamicMeshElements` once returned at the top without one, so a region holding only water tiles drew nothing: the
lake bed without water in a block of chunk columns at a region's corner, world modes review G77, fixed 2026-10-26),
and the component's bounds include the tile's box (`UVoxelRegionComponent::WaterTileWorldBounds`, `CalcBounds`) and
reach the render scene on the next tick (`RequestRenderBoundsRefresh`).
`voxel.Water.TileAudit [maxList] [chunkX chunkY]` checks
the tiles against the generated surface (every voxel column more than a voxel under the water plane should be wet) and
lists the worst chunk columns with their chunks' residency, edits, settled state, LOD and the stored against the rebuilt
mask (a stale mask against flags missing in the voxels) and, per tile, the renderer's side (the region component, its
bounds, the proxy's mesh and the frames since a main view drew it); `voxel.Water.ResendTiles` sends every tile's mesh
again; `Rendering.WaterTile.ComponentBounds` is the bounds' test.

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
- `MeshBatch.bUseAsOccluder = false` (what is under the water shows through it)
- Counts toward the same mesh batch limit (`MaxMeshBatchesPerFrame`, 2,000)

**GetViewRelevance**: Water material relevance is OR'd with terrain relevance, so the scene proxy reports what the water
material needs: the Single Layer Water pass for such a material, the translucent pass for a translucent one. Nothing is
set by hand, and the water batch needs nothing of its own.

**Buffer creation**: Water vertices go through the same `FVoxelVertex → FVoxelLocalVertex` conversion pipeline as terrain, creating identical GPU buffer structures (the vertex stream, the positions alone under manual vertex fetch (G120), color SRV, tangent SRV, texcoord SRV, index buffer, FLocalVertexFactory).

## Configuration

**File**: `VoxelCore/Public/VoxelWorldConfiguration.h`

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `bEnableWaterLevel` | bool | false | Master toggle for the water system |
| `WaterLevel` | float | 0.0 | Water surface Z height (InfinitePlane/IslandBowl modes) |
| `WaterRadius` | float | 100000.0 | Water sphere radius (SphericalPlanet mode): the sea for generation and gameplay, and the sea sphere's radius |
| `WaterMeshMaterial` | UMaterialInterface* | nullptr | Material for every sea surface the engine draws: the tiles, the far-field sea, the island's far ocean, a planet's ocean shell (Single Layer Water, or translucent) |

### Material Requirements

The `WaterMeshMaterial` is one of:
- **Single Layer Water** (what this project draws, the water plan's D110): blend mode Opaque, the Single Layer Water
  shading model with its output node (absorption and scattering). It draws in the engine's water pass through the scene
  proxy's `FLocalVertexFactory` batches and through the procedural mesh and far-field patch components alike, with no
  code of its own (the proxy's combined relevance carries the flag).
- **Translucent**: draws in the translucent pass; lit by the translucency volume, it reflects less, and it sorts against
  rain and other translucent effects.

What such a material can and cannot rely on here:

- **The surface's normal is the up normal on every face it is seen from.** The tiles draw with back-face culling off
  (their back faces show from below), and the far-field sea's, the far ocean's and the shell's undersides are separate
  sections with reversed winding and the top's normal. "Seen from below" is the view direction against the vertex
  normal, not the two-sided sign.
- **There is no spare vertex channel.** The mesher's UV1 is dropped by `FVoxelMeshRenderer::ConvertToVoxelVertices` and
  rebuilt on the GPU from the material id; shore distance per vertex would need a vertex format change. Scene depth
  gives the shore per pixel: a Single Layer Water material reads the depth under the water with
  `SceneDepthWithoutWater` (an opaque material cannot read `SceneDepth`), and may sample custom depth and custom
  stencil, the only scene textures open to it.
- **The opacity pin of a Single Layer Water material is its top layer's coverage.** Left unconnected the sea is an opaque
  sheet of the base colour: foam goes in the base colour with its mask in opacity.
- **A planet's sea is 5 x 10^6 uu from the origin** (the 50 km planet): take a texture coordinate from the world position
  modulo a tile while it is still a large-world value (`frac(position / tile)` in graph nodes), not inside a Custom
  node, whose inputs are single precision; and project on the three world planes by the normal, since there is no one
  plane.
- **Geometry stays flat** (D109): swimming, the shoreline and the underwater test read the sea's level, so a material
  moves the normal, never the vertices.

When `WaterMeshMaterial` is null no water is drawn; the game module's demo actor (`AVoxelDemoActor::bShowWaterPlane`) draws a static plane at `WaterLevel` for such a world.

This project's water is the parent's content: the master `M_VoxelWater` (`/Game/PluginTesting/Materials/`) and its
instances `MI_VoxelWater_Demo`, `_Megascans` and `_Cubic` (`/Game/PluginTesting/Materials/Water/`), built by the parent's
`Scripts/setup_water_material.py` (the water plan's W1, 2026-11-13; before it every world drew the engine's Datasmith
`M_Water` and none the project's own, G184). Every world configuration under `/Game/PluginTesting` names an instance.
The master: two detail normals scrolled along `MPC_VoxelMaterial`'s `WindDirection` in two cross-faded phases, their
slope by `RippleStrength` (the weather fills both), fading with distance; shore foam from the water's depth, skipped
over a swimmer's body by custom depth; from below, no absorption inside Snell's window and an underside colour outside
it. The parent's `Documentation/WATER_EPIC_PLAN.md` (W1's result) has the look's parameters and the shots. A river
reach draws with `RiverMeshMaterial`: here an instance `MI_VoxelRiver_Demo`, `_Megascans` or `_Cubic`, a child of the
world's look with the master's static switch `RiverFlow` on ("Lakes and river reaches drawn").

### Lumen: Translucency Reflections

This applies to a translucent water material only (the engine's Datasmith `M_Water`, which every world drew before the
water plan's W1, is translucent, default lit); a Single Layer Water material takes its reflections in the water pass and
shows no such lines. With Lumen and a translucent water material, the water's reflections come
from Lumen's radiance cache unless front-layer translucency reflections are on. Lumen marks the probes translucent surfaces
need in a pass at 1/`r.Lumen.TranslucencyReflections.MarkDownsampleFactor` of the screen; at the engine default (4) a sea
seen at grazing angles skips probe cells along the view and shows thin dark lines under its horizon, at fixed screen rows,
most visible over deep water from a swimmer's or a boat's height (world modes review G16). Set
`r.Lumen.TranslucencyReflections.MarkDownsampleFactor=1` in the project's `DefaultEngine.ini` (`[SystemSettings]`), as this
project does: the mark pass costs about 0.02 ms more at 1280 x 720.

---

## Data Flow

### Full Pipeline: Chunk Load to Water Render

```
[1] Chunk Generation (CPU or GPU)
    ↓ The water fill pass — column seed, flood, cave flags cleared
    ↓
[2] Chunk Meshing (terrain mesh)
    ↓
[3] PropagateWaterFromNeighbors()
    ↓ Cross-chunk boundary BFS flood fill
    ↓
[4] UpdateWaterTileContribution()
    ├─ BuildColumnMask() → bool[ChunkSize²] (edit-merged for a chunk with edits)
    └─ FVoxelWaterTiles::Contribute → the tile's PartialMasks[Z]
    ↓ Mark dirty → enqueue
    ↓
[5] ProcessDirtyWaterTiles() (time-sliced, per tick)
    ├─ OR-combine partial masks
    ├─ GenerateWaterMeshFromMask()
    │   ├─ Dilate by 3 cells
    │   ├─ Greedy rectangle merge
    │   └─ Emit water quads (MaterialID=254)
    └─ MeshRenderer->UpdateWaterTileMesh()
        └─ ConvertToVoxelVertices → ENQUEUE_RENDER_COMMAND → GPU buffers
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

**Files**: `VoxelStreaming/Public/VoxelFarOcean.h` (geometry), `UVoxelChunkManager::RebuildFarOcean`

Water tiles exist only for loaded chunk columns, so an island's sea used to end at the edge of what was streamed and the
island floated in a void seen from outside (world modes review KI2). Since milestone M6 (WM-E1) a bounded heightmap world
with `bIslandFarOcean` draws the sea past that: a procedural mesh component on the world actor, at `WaterLevel` with
`WaterMeshMaterial` and world-space UVs as the tiles (one seamless surface), out to `IslandFarOceanDistance` past the edge.

- **From the bounds box outward.** Inside the island's bounds box the far field draws the land and the sea at
  `WaterLevel` (mode parity WM-MP1, decision D37, 2026-10-26: an edge patch's sea hidden with its terrain where the
  window's chunks and tiles are, so a tile and the far field never overlap); the far ocean's open sea starts at the box.
  The far field's sea uses the tiles' UVs too (world X and Y over the voxel size), one pattern across the three. (The
  grid water that covered unloaded columns inside the box while the far field was off went in code review CR1, D-CR13.)
- **Under it:** a seabed that continues the island's (`FIslandBowlWorldMode::GetSeabedHeight`), half a voxel under it
  so the loaded voxel seabed covers it.
- **From below:** the underside is a separate section shown only while the viewer is under the water plane (the tiles
  are drawn two-sided always).
- **Proxy recreation (G15):** the far ocean's mesh lives on its own component, so a recreated terrain scene proxy does
  not lose it; nothing needs re-sending.

Details, cvars (`voxel.World.FarOcean`, `voxel.World.FarOceanStats`) and the trade-offs: [WORLD_MODES.md](WORLD_MODES.md),
"Beyond the edge".

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
- Water requires Single Layer Water (or translucent) shading
- Terrain uses opaque materials with triplanar projection
- Separate materials allow independent optimization
- The water draws in its own pass (the engine's water pass, or the translucent pass for a translucent material)
- Material relevance (opaque, Single Layer Water, translucent) must be reported correctly to the renderer

### Why Bounded BFS Propagation?

**Decision**: Limit water propagation to `MaxVoxelsPerFrame` per tick.

**Rationale**:
- Large terrain edits could expose thousands of air voxels simultaneously
- Unbounded BFS would cause frame rate spikes
- Bounded processing creates a natural "water filling" visual effect
- Per-frame budget (512) keeps propagation cost under 0.5ms

---

## References

- [Rendering System](RENDERING_SYSTEM.md) — the renderer, render regions
- [Architecture Overview](../ARCHITECTURE.md) — System-wide architecture
- [GPU Pipeline](GPU_PIPELINE.md) — Compute shader generation pipeline

