# LOD System Architecture

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelLOD
**Dependencies**: VoxelCore

## Table of Contents

1. [Overview](#overview)
2. [Interface Design](#interface-design)
3. [Distance Band Strategy](#distance-band-strategy)
4. [Mode-Specific Culling](#mode-specific-culling) (the vertical window, islands, planets, the far field, the cut-over)
5. [Far Compression and Uniform Chunks](#far-compression-and-uniform-chunks)
6. [Integration with ChunkManager](#integration-with-chunkmanager)
7. [Debugging](#debugging)
8. [LOD Seam Handling](#lod-seam-handling)

---

## Overview

The LOD (Level of Detail) system determines which chunks should be loaded and at what detail level based on viewer
position and other criteria: the **voxel window**. The system is **pluggable** via the `IVoxelLODStrategy` interface;
`FDistanceBandLODStrategy` is the one implementation, used by every world mode. Past the voxel window the **far field**
draws the terrain ([The Far Field](#the-far-field-spherical-planet-heightmap-worlds-since-wm-mp1)); it is not a
strategy, and the window keeps its size.

---

## Interface Design

### IVoxelLODStrategy

`Source/VoxelLOD/Public/IVoxelLODStrategy.h` (abridged; the header documents each method):

```cpp
class IVoxelLODStrategy
{
public:
    // Core queries. GetLODForChunk is called often and must be fast; const methods are thread-safe.
    virtual int32 GetLODForChunk(const FIntVector& ChunkCoord, const FLODQueryContext& Context) const = 0;
    // The LOD of the distance band a chunk falls in, even with the LOD system off (far compression reads it).
    virtual int32 GetDistanceBandLOD(const FIntVector& ChunkCoord, const FLODQueryContext& Context) const;

    // Visibility and streaming
    virtual TArray<FChunkLODRequest> GetVisibleChunks(const FLODQueryContext& Context) const = 0;
    virtual void GetChunksToLoad(TArray<FChunkLODRequest>& OutLoad, const TSet<FIntVector>& LoadedChunks,
        const FLODQueryContext& Context) const = 0;
    // EVERY out-of-range chunk, farthest first; the chunk manager vetoes the ones it must keep (mid-pipeline,
    // edit-pinned) and only then applies MaxChunksToUnloadPerFrame (R23d).
    virtual void GetChunksToUnload(TArray<FIntVector>& OutUnload, const TSet<FIntVector>& LoadedChunks,
        const FLODQueryContext& Context) const = 0;
    static int32 SelectUnloadCandidates(/* veto first, cap second */);

    // Lifecycle
    virtual void Initialize(const UVoxelWorldConfiguration* WorldConfig) = 0;
    virtual void Update(const FLODQueryContext& Context, float DeltaTime) = 0;

    // Horizons: load (the view distance), unload (x UnloadDistanceMultiplier), and the distance both are measured in
    // (horizontal in heightmap worlds, along the ground on a planet; the chunk manager's eviction, edit pins and
    // residency audit use it too)
    virtual float GetLoadDistance() const;
    virtual float GetUnloadDistance() const;
    virtual float GetStreamingHorizon(const FVector& ChunkCenter, const FVector& ViewerPosition) const;

    // The voxel cut-over (see "Orbit and Back")
    virtual bool IsWindowSuspended() const;
    virtual double GetCutoverAltitude() const;        // in force: 0 without a far field
    virtual void SetFarFieldActive(bool bActive);     // the chunk manager says when the far field is built or released
    virtual double GetViewerSurfaceAltitude() const;

    // Debugging
    virtual FString GetDebugInfo() const = 0;
    virtual void DrawDebugVisualization(UWorld* World, const FLODQueryContext& Context) const;
};
```

### FLODQueryContext

`Source/VoxelCore/Public/LODTypes.h`: what a query knows of the viewer and the world.

| Field | Meaning |
|-------|---------|
| `ViewerPosition`, `ViewerForward` | The streaming viewer (the camera, or ahead of it under the descent prefetch) |
| `ViewerLead` | How far the streaming viewer is ahead of the camera, so the strategy knows the camera's altitude (G49) |
| `bFarFieldActive` | The chunk manager has a far field; the cut-over is in force only then (D38, G78) |
| `FieldOfView`, `FrustumPlanes` | The view |
| `WorldOrigin`, `WorldMode` | The world (a planet's centre, an island's centre) |
| `MaxChunksToUnloadPerFrame` | The unload pace (8) |

### FChunkLODRequest

A chunk to load or render: `ChunkCoord`, `LODLevel` (0 = finest), `Priority` (higher first).

---

## Distance Band Strategy

The **FDistanceBandLODStrategy** uses concentric distance rings around the viewer to determine LOD levels.

### Configuration

The bands are the world configuration's `LODBands` (`FLODBand`: `MinDistance` and `MaxDistance` in uu, `LODLevel`,
`VoxelStride`, `ChunkSize`), with `ViewDistance`, `UnloadDistanceMultiplier` (1.2), `bEnableLOD` and
`bSurfaceSlabCulling`. The demo's bands are stride 1 to 7,000 uu, stride 2 to 14,000 uu and stride 4 beyond, out to a
40,000 uu view; the test planets use the same bands.

### LOD per Chunk

A chunk's raw LOD is the band its 3D distance from the viewer falls in (the coarsest band past the last). What
`GetLODForChunk` returns is the **balanced** LOD (`RebuildBalancedLODCache`):

- **Hysteresis.** A chunk changes band only past a deadband, asymmetric: it refines `RefineHysteresisFraction` (0.25)
  of a chunk width before the band edge and coarsens `CoarsenHysteresisFraction` (0.5) after it, so a viewer near a band
  edge does not flip chunks back and forth (tests `LOD.Hysteresis.*`).
- **2:1 balance.** Neighbouring chunks differ by at most one LOD level, which the seam meshers rely on.
- **When it is rebuilt.** The cache covers every candidate of the keep window and is rebuilt when the viewer enters
  another chunk or moves `BalancedLODCacheMoveThreshold` (100 uu), or when the origin or a balance input changes (tests
  `LOD.BalancedCache.*`). A chunk outside it falls back to the raw band.

Requests carry a priority: inverse distance, up to twice that for chunks ahead of the viewer (`CalculatePriority`).

### Distance Calculation

Two distances, for two questions:

```cpp
// How detailed: the LOD band. 3D in every world mode, so deep and high chunks fall into farther
// bands (and compress) even under the viewer's feet.
float FDistanceBandLODStrategy::GetDistanceToViewer(FVector Position, const FLODQueryContext& Context) const
{
    return FVector::Distance(Position, Context.ViewerPosition);
}

// Whether in range at all: the load and unload horizons. Horizontal in heightmap worlds, where each
// column's vertical extent is its window; along the ground on a planet, where each chunk's slab is.
float FDistanceBandLODStrategy::GetStreamingHorizon(const FVector& ChunkCenter, const FVector& ViewerPosition) const
{
    if (IsHeightmapWorld())
    {
        return FVector::Dist2D(ChunkCenter, ViewerPosition);
    }
    // The arc between the two directions from the centre, at the radius of the terrain under the viewer.
    return GetPlanetGroundDistance(PlanetCenter, GetPlanetGroundRadius(ViewerPosition), ViewerPosition, ChunkCenter);
}
```

Islands measured both in 2D until the world modes review's M4 (WM-S2): whole columns sat in the near bands,
uncompressed (KI13), and the residency audit, which measured 3D, reported chunks the island's 2D rule kept
(KI11). InfinitePlane measured both in 3D, so a viewer high above the terrain lost the ground at the edge of the view
and, from 40,000 uu up, all of it. With the horizon horizontal the ground under any viewer stays in range (decision D8);
the LOD distance keeps the far-away and the far-below coarse.

A planet's LOD distance stays Euclidean (3D, SP-C3): near the viewer it matches the distance along the surface. Its
horizon has been the distance along the ground since spherical Phase 6 (WM-BP8): with a
3D horizon a viewer about the view distance above the ground loaded nothing, the planet's version of what D8 fixed for
heightmaps.

### LOD Off and Cubic Meshing

`bEnableLOD` off turns the LOD system off: every chunk is LOD 0 (full detail), and there are no LOD
re-meshes, crossfades, 2:1 balance or seam holds. Read the effective value through
`UVoxelWorldConfiguration::IsLODEnabled()`, not the property: **cubic meshing always runs with LOD
off**, whatever `bEnableLOD` says (decision D7; test `LOD.LODOff.CubicForcesOff`). The cubic mesher never reads the
LOD level, so LOD changes would only re-mesh and crossfade identical geometry; greedy meshing is
cubic's answer to distance instead. The chunk manager logs `LOD system disabled for cubic mode` once
at initialisation when a cubic config has `bEnableLOD` on. The asset is not modified, so the same
config meshed smooth keeps its LOD.

With LOD off the bands stay in use as a distance table, for the two systems keyed on *how far* a
chunk is rather than *how coarse* it is:

| System | Rule | With LOD on | With LOD off |
|--------|------|-------------|--------------|
| Near window (heightmap worlds, see "The Vertical Window") | Within band 0's `MaxDistance` plus two chunk widths, a column also loads the viewer's layer ± 2 | Same | Same: keyed on band 0's distance, not the LOD level |
| Far-chunk compression (`voxel.FarCompression.*`, [below](#far-compression-and-uniform-chunks)) | A settled, idle chunk is compressed once its distance band's stride (`1 << LODLevel`) reaches `voxel.FarCompression.MinStride` (default 4) | The band is the chunk's LOD level (distance plus hysteresis and 2:1 balance) | The band its distance falls in, from `GetDistanceBandLOD` (no hysteresis) |

So with the demo bands (stride 1 to 7,000 uu, stride 2 to 14,000 uu, stride 4 beyond) and the default
`MinStride` of 4, chunks more than 14,000 uu away compress in both modes. A one-band config (band 0 out to
the view distance) puts every column in the near window and, with `MinStride` above 1, compresses nothing.

---

## Mode-Specific Culling

The LOD strategy includes mode-specific culling optimizations that skip chunks guaranteed to be empty or invisible, significantly reducing the number of chunks loaded.

### The Vertical Window (Infinite Plane & Island/Bowl)

Heightmap worlds take their vertical streaming range from the terrain, not from the viewer's altitude (decision D8,
work item WM-S1; tests `LOD.VerticalWindow.*`). Each chunk column has its own window
of layers (`FDistanceBandLODStrategy::GetColumnLayerRange`):

| Part | Layers | Where |
|------|--------|-------|
| Surface slab | The world mode's surface over the column's footprint (`IVoxelWorldMode::GetSurfaceRange`, five height samples), ± 1.5 chunks, extended up to the water plane | Every column within the horizon, at every distance |
| Near window | The viewer's own layer ± 2 (`NearLayers`) | Columns within band 0's `MaxDistance` + two chunk widths, horizontally (`GetNearLayersRadius`: 16,600 uu on the demo) |
| Height bounds | Both parts are clipped to the world mode's `GetTerrainHeightBounds` ± one chunk | Everywhere |

So every column streams its own ground whatever the viewer's altitude: a viewer flying 7,200 uu up or a camera
48,000 uu up keeps the terrain below it, and the viewer's altitude only adds layers near it (caves, dungeons, digging
below the surface, a tower above it). The slab trims each column to the 4-5 layers around its surface; the demo's full height
range is 8. Cave interiors and edits deeper than the slab load only near the viewer; edited chunks
are also pin-loaded by the chunk manager within the view distance (dungeon stamps are edits).

**Unloading uses a wider keep window** (`EWindowUse::Keep`): the slab, or the whole height range in columns within the
near radius. Nothing the near window loaded unloads while the viewer changes layer (walking across a
layer boundary, flying up); the extra layers go once their column leaves the near radius. The
balanced-LOD cache covers the keep window, so every chunk the strategy keeps has a balanced LOD.

With `bSurfaceSlabCulling` off every column streams its whole height range (the name predates D8, when the slab
applied beyond band 0 only). Until M4 the window was the viewer's layer −2 to +8 (island −2 to +4), with the slab on
InfinitePlane beyond band 0 only: a viewer above layer 0 lost the ground below it (G13), and islands kept every layer
of every column (KI5). A planet streams by chunk with the same rules on a sphere (each chunk's surface slab, the
viewer's altitude ± 2 chunks near it, the horizon along the ground; spherical Phase 6, WM-BP8), below.

**Height Range Calculation**: `IVoxelWorldMode::GetTerrainHeightBounds` (the full noise amplitude and continentalness;
for an island, also the floor beyond its edge), plus one chunk of buffer on each side.

### Island Boundary Culling (Island/Bowl Mode)

For island mode, chunks beyond the island extent are skipped:

```cpp
bool FDistanceBandLODStrategy::ShouldCullIslandBoundary(
    const FIntVector& ChunkCoord,
    const FLODQueryContext& Context) const
{
    if (WorldMode != EWorldMode::IslandBowl) return false;

    // The column's box in X and Y, against the island's footprint (IVoxelWorldMode::GetWorldBounds): a circle or a
    // rectangle R + F around WorldOrigin + the configured offset. The map's IsTileInsideWorld uses the same test.
    const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
    const float Half = BaseChunkSize * VoxelSize * 0.5f;
    const FBox2D Column(FVector2D(ChunkCenter.X - Half, ChunkCenter.Y - Half),
                        FVector2D(ChunkCenter.X + Half, ChunkCenter.Y + Half));
    return !IslandBounds.IntersectsBox2D(Column);
}
```

**Island Extent**: `IslandRadius + FalloffWidth`. Until M6 a circle kept every chunk
whose centre was within the extent plus a chunk diagonal: a ring of columns wholly past the edge, holding the old floor and
water there. The far ocean draws the sea past what streams now (WORLD_MODES.md, "Beyond the edge").

### The Window and Culling (Spherical Planet)

A planet streams by chunk, with the heightmap rules of D8 on a sphere (spherical Phase 6, WM-BP8). `FDistanceBandLODStrategy::IsInPlanetWindow` decides each chunk:

- **The horizon, along the ground** (`GetStreamingHorizon` → `GetPlanetGroundDistance`): the arc between the viewer's
  direction from the planet centre and the chunk centre's, on the sphere through the viewer's ground point (the analytic
  terrain under the viewer, `GetPlanetGroundRadius`, one sample when the viewer moves). A cone around the viewer's
  up-line, as a heightmap world's horizon is a cylinder: a chunk loads within the view distance of it and unloads past it
  × `UnloadDistanceMultiplier`, whatever the viewer's altitude.
- **The shell**: the terrain's real bounds (`FSphericalPlanetWorldMode::GetDisplacementBounds`, noise at continentalness's
  extremes, within the configured `PlanetMaxTerrainHeight` / `PlanetMaxTerrainDepth`) widened by `ShellSlackChunks` (½):
  `ShellWindowInnerRadius` to `ShellWindowOuterRadius`. Nothing outside it holds terrain.
- **The surface slab** (`GetPlanetChunkSlab`): a chunk loads when its radial extent (the nearest point and the farthest
  corner of its box from the centre) meets the surface's altitude range over the cube-sphere cells its eight corner
  directions span (`FVoxelCubeSphereGrid::GatherSpanned`), ± `SlabSlackChunks` (1.5). The cells are about a chunk across
  on the datum sphere (`CellsPerEdgeFor(R, chunk size)`: 82 a face edge on a 2.5 km planet, 32,725 on a 1,000 km one), and
  a cell's range is five terrain samples, its corners and its centre, as a heightmap column's five heights. Cells and
  chunks are memoized (each cache starts over past a million entries).
- **Near the viewer** (within `GetNearLayersRadius` along the ground, band 0 + two chunks): off its slab a chunk also
  loads within the viewer's altitude ± `NearLayers` chunk widths, inside the shell, up to the band's ceiling (none
  for a viewer above it), and stays while it is within the unload distance of the viewer (3D), so a viewer changing
  altitude unloads nothing near it. As a heightmap world streams the viewer's layer ± 2 within its height bounds: caves,
  dungeons and digging below the slab, and the chunks around a viewer near the ground or at the sea's surface (a flyer
  skimming the ground, a swimmer), whose terrain waits and settle hold need them cooked. Edits off the window are pinned
  by the chunk manager.
- **The near layers' ceiling** (`GetPlanetNearLayersCeiling`, G52): the top of the slab over the near disk, the highest
  surface the near chunks' cells hold (`GetPlanetSurfaceEnvelope` over the near radius's widest angle, at the shell's
  inner radius, plus half a chunk diagonal; memoized by the viewer's cell) + `SlabSlackChunks`, or the sea's surface +
  `SlabSlackChunks` where that is higher. A chunk off its slab wholly above it lies above its own slab too: air. So the
  band reaches no higher, and a viewer above the ceiling takes none. Until G52 the band followed the viewer at any
  altitude inside the shell: a viewer descending onto the 50 km or 1,000 km test planet from 100 km up streamed a new
  layer of air every second, taking the generation its ground disk needed, and loaded the last layers over the ground in
  its last second. The keep window is unchanged, so a viewer crossing the ceiling unloads nothing (test
  `LOD.Planet.NearBand`). A heightmap world needs no such ceiling: its near layers are already clipped to the terrain's
  height bounds (MP1).
- **The cull** (`ShouldCullBeyondHorizon`, SP-C2), a superset of the window: a chunk wholly inside the shell's inner
  radius or outside its outer one, or whose nearest point lies beyond the horizon, the sum of the two tangent distances to
  the datum sphere, `sqrt(2Rh + h^2) + sqrt(2RH + H^2)` (`GetVisibilityHorizon`; R the radius, h the viewer's altitude,
  H the terrain's top: a peak shows over the horizon from that far). A viewer inside the planet's lowest terrain is not
  culled.

With `bSurfaceSlabCulling` off the window is the whole shell inside the cone (fine on a thin shell; with km-scale
relief that is tens of thousands of chunks). The LOD band is the chunk's 3D distance from the viewer, so a high viewer's
ground disk sits at the coarsest band. Planets draw their sea with the sea sphere, not voxel water tiles, so a sea
column's slab is its seabed's (heightmap slabs reach up to the water plane).

**Candidates** (`ForEachPlanetCandidate`): the chunk columns along the grid axis closest to the viewer's up, inside the
bounding box of the cone, crossing the spherical shell of the slab's envelope over the disk (`GetPlanetSurfaceEnvelope`:
the cells within the cone's angle, memoized by the viewer's cell; `GetShellCrossing`), plus the near window's cone and
altitude band. Load, unload (`GetChunksToUnload`, the keep window) and the balanced LOD cache use the same window.

So the window is the ground disk of the view distance, a few chunks thick around the surface wherever it is: 950 to
1,160 chunks on the 2.5 km planet and on a 1,000 km one with km-scale relief alike (`LOD.Planet.SurfaceSlab`; the whole
shell would be 1,000-1,200 and 17,700), and 1,080-1,260 live on the 50 km and 1,000 km test planets, against 2,800-3,000 for them before WM-BP8, whose
shells were thicker than the view distance, so their windows were the whole sphere of it around the viewer. A climbing
viewer keeps its ground disk (`LOD.Planet.GroundHorizon`): until WM-BP8 the window was clipped to the viewer's Z ± the
view distance and measured in 3D from the viewer, so it held nothing from about the view distance up (the planet's G13).
A flyer above the surface slab adds nothing off it (G52); until then a flyer inside the shell added its near window (160
to 200 chunks of air on the 1,000 km planet), and only above the shell nothing.
Beyond the window the far field draws the planet (below).

### The Far Field (Spherical Planet; heightmap worlds since WM-MP1)

The voxel window has one chunk size and generates every chunk at full resolution, so it cannot reach the horizon (58,000
chunks on the ground of a 50 km planet) or orbit. Past it a planet draws its far field (spherical Phase 6, WM-BP9,
decision D19): `FVoxelFarField` (VoxelStreaming), a cube-sphere quadtree of
patches of the analytic surface. It is not an `IVoxelLODStrategy`: its patches are heightfields with their own
primitives, and the voxel window keeps its size. Since the world modes review's MP3 (WM-MP1, decision D36) a plane and an island draw the same far field over the XY plane ("On a heightmap world"
below): before, nothing was drawn past the voxel window but an island's far ocean.

- **Patches.** Six face quadtrees on the planet's surface coordinate (`FVoxelCubeSphere`'s faces and tangent warp; level 0
  is a face, each level halves a patch along U and V, `FVoxelFarFieldPatchKey`). A patch is `PatchQuads` x `PatchQuads`
  quads (`voxel.World.FarFieldQuads`, 16) of the conditioned analytic surface (`GetConditionedTerrainAltitude`, zones
  gathered for patches up to 32 chunks in radius), drawn `Sink` (a voxel) under it, with normals from the grid (one ring
  of samples past its edges), the surface's materials (below), and skirts deep enough to close the gap to a neighbour up
  to three levels coarser or finer. A shared cube edge is the same direction from both faces (the gnomonic coordinate exactly +-1 there),
  so the terrain's step across the edge's plane (G43) never splits it.
- **Materials** (G45, decision D28; tests `FarField.Planet.MaterialMix`, `.MaterialBlend`). Each grid vertex samples the surface's material
  mix (`FVoxelSurfaceQuery::QueryPlanetSurfaceMaterialMixAt`: in a biome blend zone the two leading biomes' surface
  materials, height rules applied, and the second's share, which is what generation's dithered per-voxel picks cover
  there; one material outside the zones). A quad carries its corners' two largest materials and, at each corner, the
  second's weight of the two; both materials are flat per triangle (a grid vertex is duplicated where neighbouring quads
  hold different pairs) and only the weight interpolates. The terrain material picks one of the two per pixel against a
  temporal screen dither (`FarFieldMaterialPick`, [MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) "Far-Field Material Blend"),
  which TSR resolves into a gradient across the quad. `voxel.World.FarFieldMaterialBlend 0` gives each quad one
  material, the most common of its corners' picks (`QueryPlanetSurfaceConditionsAt`), so a border is a staircase of
  quads, as before G45; it is read when patches build (`voxel.World.FarField 0` then `1` rebuilds them).
- **Selection.** A patch splits while its geometric error (the drawn triangles against the surface at the quad centres,
  curvature and the sea's sag included) projects to more than `voxel.World.FarFieldError` pixels (2) from the camera,
  and stays whole under 0.7 of it; a patch wholly under the camera's horizon (the lowest terrain's sphere, plus the
  angle its top stays in view) is neither drawn nor split. A patch keeps drawing until its subtree covers its area, so
  the planet never shows a hole while it refines; builds run on the thread pool (`voxel.World.FarFieldMaxBuilds`, 6 in
  flight), nearest first. A patch whose subtree waits for a build stands in for it, hiding everything under it, so a
  wait where nothing would show must not reach one (visual polish VP4, `voxel.World.FarFieldAvoidStandIns`, 1): a band
  patch the voxel chunks cover covers its area without a mesh, a patch culled in its last visit (coming up over the
  horizon) waits for its build drawing nothing, and a chunk re-meshing counts as drawn. Before, the nearest built
  ancestor stood in, often a face's root (the others released), and its coarse surface flashed over the voxel terrain
  and the horizon for a frame or more (test `FarField.{Planet,Plane}.NoStandInOverWindow`; history:
  [VISUAL_POLISH_PLAN.md](../VISUAL_POLISH_PLAN.md) "VP4 terrain flicker").
  The chunk manager updates the far field in its tick (`TG_PrePhysics`) with the player camera manager's camera, the
  frame before's (the camera managers update after `TG_PostPhysics`); after a cut, a camera more than a chunk from that
  one, it updates it again in `TG_PostUpdateWork` for this frame's (`FVoxelFarFieldCameraTickFunction`,
  `UpdateFarFieldForCamera`; F4.3, `voxel.World.FarFieldCameraUpdate`), so the frame of a teleport draws the far field
  where the camera arrived rather than the old position's selection (test `FarField.{Planet,Plane}.CameraAfterCut`).
- **The hole.** Within the window's reach of the viewer's ground point (the unload distance plus a chunk's half
  diagonal, along the ground as the window measures its horizons) patches split down to the edge level, one to two
  chunks across (`FVoxelFarFieldSettings::ComputeEdgeLevel`: 64 a face edge on the 2.5 km planet, 1,024 on a 50 km one,
  16,384 on a 1,000 km one). An edge patch hides its terrain once every voxel chunk its surface passes through is drawn
  (its vertices', its quad centres' and a vertex spacing around each: `SurfaceChunks`), after the chunks' fade-in. A
  partly covered patch stays drawn a voxel under the voxel surface, which hides it: no gap and no overlap along the
  window's ragged edge, and no far-field floor in a hole or cave where the chunks are drawn. Patch geometry never depends
  on the window: the hole moves with the window by visibility alone, recomputed when a chunk changes state.
- **Residency.** One procedural mesh component per resident patch at the patch's origin (its centre's point on the
  datum, double), vertices as float offsets of the patch's size; a patch hidden for 10 s releases its component and
  builds again when needed (its subtree draws meanwhile), except the roots and edge patches near the window's edge,
  which are built while covered so they show the moment their chunks unload (a covered one the viewer leaves there
  builds while it draws nothing, VP4). Every root is built from the first updates, also a face under the horizon, so
  after a teleport onto a face the camera never saw its root stands in at once (F4.2,
  `voxel.World.FarFieldBuildAllRoots`, 1; before, a root was built once some of its face came into view and the first
  update there drew no far field; test `FarField.Planet.TeleportOntoUnseenFace`).
- **The sea**, at scale: where the sea sphere would sag past its budget (`ShouldDrawSea`: from about R 3.3 x 10^5) each
  patch draws a sea section over the cells near or under the sea, 8 x 8 cells a patch, over the voxel window too (a
  planet has no voxel water tiles), with an underside near the window shown from under the sea. The small planet keeps
  its sea sphere.
- **On a heightmap world** (world modes review WM-MP1, decisions D36 and D37; tests `FarField.Plane.*`,
  `FarField.Island.Bounds`, `FarField.Cubic.BlockTop`) the same
  class runs with a heightmap frame: the key's face is `FVoxelFarFieldPatchKey::HeightmapFace` and its X and Y are signed
  cell indices of one quadtree over the XY plane whose level-0 cells are **root tiles** of `voxel.World.FarFieldRootChunks`
  chunks aligned to the chunk grid (0, the default: about a quarter of the far distance in chunks rounded to a power of
  two, `ComputeHeightmapRootChunks`: 64 with 64-voxel chunks of 100 uu, 409,600 uu tiles; 256 with 1,600 uu chunks),
  so the edge level is log2 of that and an edge patch is one chunk column. Roots exist for the tiles within
  `voxel.World.FarFieldDistance` of the camera (2,000,000 uu, the island far ocean's reach; about 10 x 10 tiles about
  the camera, made as it moves and dropped with their trees past it) and, on a bounded world, only over the island's bounds box, which
  they never leave; a patch is culled past the far distance or wholly outside the box (there is no horizon on a plane).
  The surface is the generated height (`IVoxelWorldMode::GetTerrainHeightAt` with the heightmap conditioning zones, as
  `GetGeneratedSurfaceHeight` reads it; in a cubic world the block top of the column, `FVoxelBlockSurface`, so the far
  field meets the blocks at the window's edge without a step), local up is +Z, the materials
  `FVoxelSurfaceQuery::QuerySurfaceConditions` and its mix (`QuerySurfaceMaterialMix`, G45's rule with the heightmap
  climate), and the error, the skirts, the hole rule (edge patches hidden once every chunk their surface passes through
  is drawn, distances measured on the plane from the streaming viewer's XY), stand-in avoidance and the camera update
  after a cut are the planet's. The sea is a section on the `WaterLevel` plane over the cells whose terrain dips near or
  under it, with the water tiles' UVs (world X and Y over the voxel size, one pattern with the tiles and the far ocean),
  and, since the water tiles draw the sea over the streamed columns, an edge patch's sea hides where its column's water
  tile is drawn (`SetWaterTileQuery`, `UVoxelChunkManager::HasWaterTile`) and draws where none is: the ring between the
  window's load and unload horizons holds chunks without tiles, and before the query its sea went with its terrain (a
  dry strip at the window's edge seen from above). A plane has no horizon, so its far field splits far more patches
  than a planet's at the same screen error (on `VoxelDemo` at 2 px about 1,500 patches and 300 MB from the ground,
  against a planet's 29-56 MB): heightmap worlds take their own budget, `voxel.World.FarFieldHeightmapError` (4 px; 0 =
  `voxel.World.FarFieldError`). An island's far ocean is its open sea past the bounds box and its static seabed (D37: two
  seas inside the bounds would z-fight; the grid water that stood in for unloaded columns while the far field was off
  went in code review CR1, D-CR13). Heightmap conditioning zones rebuild the patches they meet
  (`InvalidateBox`), a planet's radial ones the caps. The build queue's priority offset for non-root patches is 1e12
  (it was 1e30, which a double adds a distance to without change, so with six builds a tick the patches under the
  camera could starve behind thousands of far ones: found on the plane, the planet's six roots and horizon hid it).

`voxel.World.FarField 0` drops the far field, a development switch (code review CR1, D-CR13: nothing is drawn past the
voxel window but a small planet's sea sphere or an island's open sea and seabed); `voxel.World.FarFieldStats`, `vox.Planet` / `vox.World` and the bench's `FarFieldMs`, `FarFieldPatchN`,
`FarFieldMB`, `FarFieldBuiltN` and `FarFieldStandInN` (stand-ins over edge patches the chunks cover, VP4) report it;
`voxel.World.FarFieldTrace 1` logs every update with such a stand-in and why.

### Orbit and Back: the Voxel Cut-Over (Spherical Planet; heightmap worlds since WM-MP2)

From high up the voxel window adds little the far field does not draw: its ground disk is a narrow cone under the
viewer, all of it in the coarsest band. So a planet's window empties above a cut-over altitude and streams back on the
way down (spherical Phase 6, WM-BP10, decision D18). A plane's or an island's window
does the same since the world modes review's MP3 (WM-MP2, decision D38): the altitude is measured over the generated
ground under the streaming viewer or the sea where higher, `VoxelCutoverAltitude` and the three-view-distance
default, `voxel.Stream.VoxelCutover` and G49's guard apply as on a planet, a suspended heightmap window returns no column
(`GetVisibleChunks`, the balanced LOD cache) and unloads every chunk, and the cut-over is in force only while the chunk
manager has a far field (`FLODQueryContext::bFarFieldActive`): without one `GetCutoverAltitude` is 0 (the configured
altitude stays readable as `GetConfiguredCutoverAltitude`) and the window keeps its ground disk at any altitude (D8), since
a suspended window with nothing drawn under it is a hole in the world. The same gate holds on a planet since G78:
`voxel.World.FarField 0` there once left the cut-over in force, so from orbit nothing drew the planet; now it keeps the
window, and `vox.Planet` reports "cut-over off: no far field" (tests `LOD.Planet.CutoverFarFieldGate`,
`Streaming.Planet.CutoverFarFieldGate`, `LOD.Plane.Cutover`).

- **The cut-over** (`FDistanceBandLODStrategy`, `IVoxelLODStrategy::IsWindowSuspended`): decided in `Update` from the
  streaming viewer's altitude over the surface under it, the analytic terrain along its direction or the sea where that
  is higher (`GetViewerSurfaceAltitude`: a flyer skimming a deep sea keeps its window). Above
  `UVoxelWorldConfiguration::VoxelCutoverAltitude` (0: three view distances, `DefaultCutoverViewDistances`:
  120,000 uu on the review planets) the window is suspended; it resumes below `CutoverResumeFraction` (0.85) of it, so a flyer hovering at the cut-over does
  not churn. `voxel.Stream.VoxelCutover` overrides it (-1: the configuration's; 0: never, the window keeping its ground
  disk at any altitude as WM-BP8 made it). A suspended window resumes only with the camera under the cut-over as well
  (`FLODQueryContext::ViewerLead` gives the strategy the camera's altitude; G49, `voxel.World.CutoverResumeGuard`, 0 =
  the streaming viewer alone decides, the rule before): the descent prefetch below can bring a resume forward to the
  cut-over but no higher (tests `LOD.Planet.CutoverResumeGuard`, `Streaming.Planet.CutoverDescentStop`).
- **Suspended**, `GetVisibleChunks` returns nothing, `GetChunksToUnload` every loaded chunk and the balanced LOD cache is
  empty; the chunk manager evicts its generation queue, stale-culls whatever finishes generating, and neither pins nor
  keeps edited chunks (their edits stay in the edit layers and load with the window when it resumes). The window
  unloads at the configured unload rate (1,100 chunks in a few seconds).
- **Why three view distances.** At one, a 5,000 uu/s descent onto the 50 km test planet resumed the window only 10 s
  before landing (the prefetch's lead is at most half the altitude) and touched down with half of its 1,300 chunks
  loaded, 5 s of catch-up after landing against 0.36 s with the window kept; and a viewer 400 m up already saw only the
  far field. Kept up to three view distances, the window costs little (on M about 1,500 chunks and 510 MB climbing to
  60,000, within D3 and the same frame time) and a descent has about 20 s to stream it back.
- **The far field** cuts its hole while chunks are held; once the window holds none, it refines by its screen error alone
  (`EdgeRadius` 0: no edge patches forced for an empty window). When the window resumes the band returns, its patches
  build while their parents draw, and the chunks fade in over them (the appearance fade, measured from the camera).
- **The descent prefetch** (`UVoxelChunkManager::ApplyDescentPrefetch`, through `FVoxelStreamingViewerPolicy`; every
  world mode since MP1): under a camera descending faster than
  `voxel.Stream.DescentMinSpeed` (1,500 uu/s) the streaming viewer goes ahead of it along local down by what it covers
  in `voxel.Stream.DescentLead` seconds (3), at most half its altitude and never within two chunks of the surface
  (`ComputeDescentLead`; the camera's radial speed smoothed, a jump of ten chunks in a frame treated as a teleport). The
  window resumes and refines its LOD that much earlier; the cut-over, the LOD bands and the window are the streaming
  viewer's, the far field's error and horizon and the appearance fade the camera's. Before G49's guard (above) a fast
  descent that stopped above the cut-over resumed the window for a moment and suspended it again as the lead decayed
  (on L 0.7-1.8 s, 20-100 chunks generated and about 1,000 queued generations dropped a stop); now a window the lead
  resumes has the camera under the cut-over, and a window suspends only with the streaming viewer, which is never above
  the camera, over the cut-over, so only a climb back over the cut-over suspends it again.
- **The camera is the streaming viewer**, so a camera that leaves the pawn (the orbit camera, a debug camera) takes the
  window with it, as it always has: above the cut-over the ground under a pawn left behind unloads. `vox.Orbit` holds the
  pawn (a swimmer too since G48) and settles it on release; survey cameras freeze streaming.

`vox.Planet` (and `vox.World` in every mode) reports the window's state, the cut-over and the lead; the bench adds
`Suspended`, `LeadUU` and `SurfAlt` columns, a `voxelCutover` summary and the run's `cutoverTransitions` (each resume and
suspension with the camera, the streaming viewer and the lead, and for a suspension the seconds active, the chunks held
and generated and the queued generations dropped; `UVoxelChunkManager::GetCutoverTransitions`, also logged), and a
descent route (`descend=Lat:Lon:Alt` on a planet, `descend=X:Y:Alt` on a heightmap world; climbs `climb=` likewise).

### Integration with Streaming

Load and unload use the same pieces:

```cpp
// GetVisibleChunks / GetChunksToLoad: per column within the load horizon, the layers of its window
for each column with GetStreamingHorizon(column centre) <= ViewDistance:   // horizontal in heightmap worlds
    GetColumnLayerRange(X, Y, Context, EWindowUse::Load).ForEachLayer(Z):
        skip if ShouldCullIslandBoundary(...) || ShouldCullBeyondHorizon(...)
        request (X, Y, Z) at its balanced LOD

// A planet streams by chunk instead (WM-BP8): the candidates of ForEachPlanetCandidate, each kept by
// GetStreamingHorizon (along the ground) <= ViewDistance && IsInPlanetWindow(..., EWindowUse::Load)

// GetChunksToUnload: every loaded chunk that is out of range, farthest first
if (GetStreamingHorizon(ChunkCenter, Viewer) > ViewDistance * UnloadDistanceMultiplier ||
    !GetColumnLayerRange(X, Y, Context, EWindowUse::Keep).Contains(Z) ||   // heightmap worlds
    !IsInPlanetWindow(ChunkCoord, Context, EWindowUse::Keep) ||          // a planet
    ShouldCullIslandBoundary(...) || ShouldCullBeyondHorizon(...))
{
    OutUnload.Add(ChunkCoord);
}
```

### What Bounds Residency

There is no chunk-count cap (`MaxLoadedChunks` was never read and was removed in M4, WM-S3). What is resident is
bounded by geometry:

- **The horizon:** chunks load within the view distance and unload past it × `UnloadDistanceMultiplier` (1.2), measured
  horizontally in heightmap worlds (a cylinder) and along the ground from the viewer's ground point on a planet (a cone
  around the viewer's up-line; 3D from the viewer until spherical Phase 6's WM-BP8).
- **The vertical window:** each column's slab, plus the near window's layers near the viewer (above); in all, the
  world mode's height bounds.
- **The world's footprint:** an island keeps only columns that touch its R + falloff footprint (since M6; before, out to
  a chunk diagonal past it); a planet only chunks in its terrain shell and above its horizon, each within its surface
  slab or near the viewer below it (WM-BP8).
- **Edits:** edited chunks pin-load within the view distance and stay to the unload distance, outside the window too.
- **The pipeline:** chunks mid-generation or mid-meshing finish before they unload; `MaxChunksToUnloadPerFrame` paces
  unloading, not residency.

On the demo geometry (4,800 uu chunks, 40,000 uu view) that is about 980 chunks settled and 1,080-1,220 at peak while
moving, for InfinitePlane and an island alike; a fully resident island S (0.8 km across) holds about 1,020 (M4; history:
`WORLD_MODES_REVIEW_PLAN.md` §14.8).

---

## Far Compression and Uniform Chunks

A chunk's voxels are 1 MB at 64^3. Two rules keep the window's memory down without changing what any reader sees:

- **Far compression.** A settled, idle chunk (`voxel.FarCompression.IdleFrames`, 300 frames unchanged) whose distance
  band's stride reaches `voxel.FarCompression.MinStride` (4: LOD 2 and beyond) is compacted: to one value when uniform,
  else with `voxel.FarCompression.Codec` (3, Oodle). The sweep is time-sliced (`voxel.FarCompression.MaxScansPerTick`,
  `MaxInFlight`). A reader of a compressed chunk without edits does not expand it: its snapshot is decoded by a worker
  task from a copy of the compact form (`FVoxelCompactSource`), started when a chunk in its neighbourhood is queued for a
  mesh or a collision cook; mesh jobs, seam jobs and cooks wait for it on their own workers, and the chunk stays
  compressed. Before, every reader expanded the chunk on the game thread as the viewer came near, about 0.3 ms each and
  half of them repeats (G65; `voxel.FarCompression.SnapshotReads`, tests `Compression.Codec.CompactSource`,
  `Streaming.{Planet,Plane}.FarReadsOffGameThread`).
- **Uniform at generation.** A chunk whose every voxel is the same (all air, all rock, open sea) is collapsed to that
  value where it is generated (the CPU worker, the tree injector, a GPU readback as it lands;
  `FChunkDescriptor::CollapseToUniform`), not when the sweep reaches it; its snapshot is a shared array per value and a
  point read returns the value without expanding it. After a teleport this roughly halved the window's peak voxel data
  on the big planets (G67; `voxel.FarCompression.UniformAtGeneration`, tests `Compression.CollapseToUniform`,
  `Streaming.{Planet,Plane}.UniformAtGeneration`).

The snapshot cache those readers share (shells for collision cooks, copy-on-write resident arrays) is described in
[STREAMING_AND_SEAMS.md](STREAMING_AND_SEAMS.md). `voxel.FarCompression.Stats` logs residency and compression memory,
`voxel.FarCompression.Decodes` where compressed data was expanded.

---

## Integration with ChunkManager

Each tick `UVoxelChunkManager`:

1. Builds the query context from the camera (`BuildQueryContext`), with the streaming viewer placed by
   `FVoxelStreamingViewerPolicy` (the descent prefetch's lead) and `bFarFieldActive` set.
2. Calls `Update` on the strategy (the cut-over state, the balanced LOD cache).
3. Makes streaming decisions when the viewer enters another chunk (`CachedViewerChunk`) or an update is forced
   (`bForceStreamingUpdate`): `GetChunksToLoad` into the generation queue, `GetChunksToUnload` through the chunk
   manager's vetoes into `SelectUnloadCandidates`. After a camera cut (more than a chunk) it runs a second streaming
   pass in the same frame for the new camera (`voxel.Stream.CameraCutUpdate`, every world mode since MP1).
4. Re-meshes chunks whose balanced LOD changed (with a crossfade, `RENDERING_SYSTEM.md`) and updates the far field.

---

## Debugging

| Command | What it shows |
|---------|---------------|
| `voxel.World.Stats` | The world's bounds and sea, the window's reach, the far field, the cut-over and the descent prefetch |
| `vox.World` / `vox.Planet` (game module) | The window's reach per LOD and layer, the far field, the cut-over state, the lead |
| `voxel.Stream.AuditResidency` | Chunks tracked past the unload radius, renderer chunks and seam buckets with no owner, stuck jobs (0 findings once settled) |
| `voxel.World.FarFieldStats`, `voxel.World.FarFieldTrace 1` | The far field's patches and memory; every update with a stand-in |
| `voxel.FarCompression.Stats`, `voxel.FarCompression.Decodes` | Compression memory; where compressed data was expanded |

`FDistanceBandLODStrategy::GetDebugInfo` and `DrawDebugVisualization` give the bands and visible chunk bounds. The
streaming bench (`voxel.Bench.Run`) reports the window, the far field and the cut-over per frame.

---

## LOD Seam Handling

When adjacent chunks render at different LOD levels, their boundary vertices do not align. Every meshing mode closes
chunk boundaries by **seam ownership**: a chunk mesh covers only the cells that read no neighbour (the Interior
domain), and each face, edge and corner between chunks is meshed once by a seam job from the voxels of every chunk
that touches it, at each participant's own LOD. The scheduling is in [STREAMING_AND_SEAMS.md](STREAMING_AND_SEAMS.md);
the mesher rules per mode:

### Cubic Meshing

Cubic runs without the LOD system (see [LOD Off and Cubic Meshing](#lod-off-and-cubic-meshing)), so every chunk is LOD 0 and there are no LOD boundaries to stitch. A chunk mesh is the Interior domain, every face except those on the chunk's six boundary planes, read from the chunk's own voxels only; the faces on a shared plane belong to that pair's face seam job, which culls them against both chunks' voxels (WM-C1). No cubic face lies only on a chunk edge or corner, so the seam registry tracks face seams alone for cubic (`FVoxelSeamRegistry::SetFaceSeamsOnly`). At the edge of the loaded region a plane whose other side is not loaded has no seam yet, so it shows no faces.

### Marching Cubes

Where a face borders a coarser chunk, the face seam job, in the finer participant's frame, emits a **Transvoxel
ribbon** (zero-thickness transition cells on the boundary plane, 9 samples on the fine side) and geomorphs the fine
band toward the coarse contour with a ramp that ends at the band's inner edge. At a stride, vertices are LOD 0's
crossings on their edges (G80). See [MARCHING_CUBES_MESHING.md](MARCHING_CUBES_MESHING.md).

### Dual Contouring

The mixed-LOD face, edge and corner seam meshers recompute each side's ring cells at that side's own stride and stitch
at the finest participant's stride: every fine crossing fans onto the coarser side's cell vertices. A coarse cell with
no crossing at its own stride facing a fine crossing gets a boundary fallback vertex (G17). No lookup tables, and any
LOD difference the 2:1 balance allows. See [DUAL_CONTOURING.md](DUAL_CONTOURING.md).

### Configuration

The world configuration's `bEnableLODSeams` (on) keeps Marching Cubes' ribbon (`FVoxelMeshingConfig::bUseTransvoxel`).
Dual Contouring's seams take the QEF settings (`QEFSVDThreshold`, `QEFBiasStrength`). Seam requests carry the
participants' LOD levels (`NeighborLODLevels`, and for Marching Cubes `TransitionFaces`); chunk meshes carry none.
