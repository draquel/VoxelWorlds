# Lumen Off Screen: the Terrain's Distance Fields and Ray Tracing Geometry (D-VP4 spike)

> Spike of 2026-11-03 (D-VP4 = (b) as a spike, (d) meanwhile). Terms: `Documentation/GLOSSARY.md`. The prototype is on
> VoxelWorlds branch `spike/dvp4-lumen-offscreen` (draft PR VoxelWorlds#248, not to merge); the captures are on the
> parent repository's media branch `media/dvp4-lumen-offscreen` (`Documentation/Images/dvp4/`).

**The question.** The terrain draws through a custom renderer (`FVoxelSceneProxy` on `FLocalVertexFactory`) with no mesh
distance fields and no ray tracing geometry, so Lumen sees it only in screen space: an overhang that leaves the screen
stops darkening what it covers (visual polish F3.3). Can the voxel terrain give Lumen (and distance field shadows and AO)
a representation off screen, and at what cost?

**The answer.** Yes, both ways, without engine changes, and both fit seam ownership. The project runs Lumen with
hardware ray tracing (`r.RayTracing=True`, `r.Lumen.HardwareRayTracing` 1 on the RTX 4080 here), and in that mode Lumen
traces the ray tracing scene and ignores mesh distance fields, so **ray tracing geometry per chunk and seam mesh is the
path that fixes F3.3 for this project**. It is also the smaller job: it reuses the mesh buffers the proxy already
uploads. Distance fields work as well, under software Lumen, distance field shadows and AO, but their build costs a
multiple of the mesh job and their registration a primitive per chunk. What neither gives on its own is bounce light
from the terrain: a ray that hits terrain finds no Lumen surface cache, returns black, and the ground under an
overhang reads darker than it should. The production plan below adds the surface cache (cards) as a second step.

## 1. What was built

Two prototypes behind cvars, off by default (`VoxelTerrainRepresentationSpike.h/.cpp` in VoxelRendering, hooks in
`FVoxelSceneProxy`, `FVoxelMeshRenderer`, the mesh worker in `UVoxelChunkManager`, one field on `FChunkMeshData`):

| Path | How | Cvars and commands |
|---|---|---|
| Ray tracing geometry | On every chunk and seam upload the proxy builds an `FRayTracingGeometry` (fast build, no update) over the buffers it just created: the index buffer and the interleaved `FVoxelLocalVertex` buffer read as `VET_Float3` at stride 40. `GetDynamicRayTracingInstances` emits one instance per current mesh with the opaque material; replaced or removed meshes release theirs | `voxel.Render.Spike.RayTracing` (read at proxy creation and at each upload; the emission follows it live) |
| Mesh distance field | The mesh worker builds an `FDistanceFieldVolumeData` from the chunk's voxels beside the mesh; the renderer gives it to a distance-field-only primitive per chunk (`UVoxelChunkDistanceFieldComponent`, a proxy that draws nothing, `GetDistanceFieldAtlasData`) | `voxel.Render.Spike.DistanceField`, `.DistanceFieldResolution` (1 = a distance field voxel per terrain voxel), `voxel.Render.Spike.DFVisible 0\|1` (A/B in one session) |
| Counters | BLAS live and bytes (`RHICalcRayTracingGeometrySize`), builds, instances a frame; distance field objects and bytes, build and registration times | `voxel.Render.Spike.Stats [reset]`, `voxel.Render.Spike.BenchDF [n] [res] [chunk]` (an uncontended build over a synthetic chunk) |

### The three paths of the brief

1. **Feeding the global distance field from the density directly.** The engine's only non-mesh input to the global
   distance field is the heightfield path Landscape uses (`GetHeightfieldRepresentation`: a height and normal texture
   per component, `GHeightFieldTextureAtlas`). A heightfield holds one height per column, so it cannot hold an
   overhang (the very thing F3.3 is about) or a cave, and it is planar: a planet has no heightfield. Dismissed. The
   global distance field is otherwise composed from mesh distance field objects, which is path 2.
2. **Per-chunk mesh distance fields.** A custom proxy can register one: `bSupportsDistanceFieldRepresentation`,
   `GetDistanceFieldAtlasData`, `HasDistanceFieldRepresentation` (`DistanceFieldObjectManagement.cpp` reads only these
   and the instance transforms). Three constraints shaped the prototype:
   - **One distance field per primitive.** A primitive's instances all share its one `FDistanceFieldVolumeData`, so a
     region proxy holding 4,096 chunk slots cannot give each chunk its own; the prototype registers a primitive per
     chunk. A region-wide field would have to be rebuilt on every chunk change.
   - **Streaming reads bulk data from disk.** The engine streams the finer mips from `StreamableMips` with file reads
     (`DistanceFieldStreaming.cpp`; only `WITH_EDITOR` reads in-memory bulk data), which a runtime field has not got. The
     builder puts its one mip in the always-loaded slot and gives the streamed slots `BulkSize` 0, which the streamer
     never requests, so it works in a cooked game.
   - **The density is not a distance.** Generation saturates the density one voxel either side of the surface
     (`SignedDistanceToDensity`), so the voxels next to the surface give the distance exactly and a separable Euclidean
     distance transform (Felzenszwalb) gives it out to the band; the bricks are then encoded exactly as
     `MeshDistanceFieldUtilities.cpp` does (8³ bricks, a 4-voxel band, 8-bit quantised, empty bricks dropped).
3. **Hardware ray tracing geometry.** As `ProceduralMeshComponent` does: an `FRayTracingGeometry` per mesh and
   `GetDynamicRayTracingInstances`. About 150 lines, no new data on the CPU, nothing on the workers.

## 2. Does it work

The repro (`capture2.py` beside the captures): on flat ground an overhang made of 25 fill spheres
(`voxel.Edit.System ... 1200 fill`, a slab about 4.5 m thick whose underside is 3.5 m up), and a sealed cave dug 35 m
under the ground. The sun at noon with the clock stopped, exposure and anti-aliasing off. The overhang is shot from
42 m at FOV 90 (its underside on screen) and FOV 20 (only the shaded ground under it: the FOV 20 frame is the FOV 90
frame's centre 17.6 %, so the two show the same ground). With screen traces alone the ground brightens when the
overhang leaves the screen; with an off-screen representation it should not. In the sealed cave nothing should be lit.

Mean luminance (the inner 60 % of the compared regions), VoxelDemo and PlanetTest (30N 20E):

| Configuration | Demo: under the overhang, on screen → off screen | Demo: off/on | Demo: sealed cave | Planet: sealed cave |
|---|---|---|---|---|
| Hardware Lumen, as today | 0.248 → 0.270 | **1.09** (brightens: F3.3) | 0.339 | 0.077 |
| Hardware Lumen + ray tracing geometry | 0.176 → 0.159 | 0.91 | **0.237** | **0.039** |
| Hardware Lumen + distance fields | 0.239 → 0.258 | 1.08 (no effect) | 0.333 | 0.077 |
| Software Lumen, as today | 0.239 → 0.261 | 1.09 | 0.335 | 0.078 |
| Software Lumen + distance fields | 0.173 → 0.155 | 0.90 | **0.237** | **0.039** |
| Software Lumen + distance fields, mesh SDF tracing off (global distance field only) | | | | 0.039 |

- **Both representations stop the leak**: the ground under the overhang no longer brightens when the overhang leaves
  the screen, and the cave darkens by 30 % (demo; the white openings in its frame are the generator's own caves,
  which reach the surface) and 50 % (planet).
- **Distance fields do nothing under hardware Lumen**, as the engine's code says (hardware Lumen traces the ray tracing
  scene; the distance field scene is software Lumen's, distance field shadows' and AO's).
- **Off screen now reads darker than on screen (0.90-0.91).** A ray that hits terrain finds no Lumen surface cache
  (the terrain has no mesh cards) and returns zero radiance (`LumenHardwareRayTracingCommon.ush`,
  `CreateRayTracedLightingResult`): occlusion is right, bounce is lost, so off screen the overhang's lit underside adds
  nothing. On the planet the shaded ground under the overhang goes black. Hit lighting
  (`r.Lumen.HardwareRayTracing.LightingMode 1`) recovers a little (0.159 → 0.170 off screen) at a large GPU cost; the
  surface cache is the real fix (§5, step 2).
- Planet: both work 250 km from the origin in a tilted frame (the region proxies' render origins carry the ray tracing
  instances' transforms; the distance field objects sit at their chunk's origin in double).
- The global distance field shows a groove along chunk planes: each chunk's field ends half a voxel past its outer
  voxels and clamps its samples there, so between two chunks the composed distance is slightly too large. A field box
  overlapping its neighbours by the band (the voxels' distance extrapolated, or read from the seam's snapshot) closes it.

Captures (`Documentation/Images/dvp4/` on the media branch): `demo_overhang_hw.jpg` (hardware Lumen as today against
ray tracing geometry, FOV 90 and 20), `demo_overhang_sw.jpg` (software Lumen against distance fields), `demo_cave.jpg`,
`planet_overhang.jpg`, `demo_rtscene.jpg` (the ray tracing debug view: the terrain is in the scene only with the spike),
`demo_distancefields.jpg` (the mesh and the global distance field views).

## 3. Cost

Benches 2026-11-03 on the gate editor (RTX 4080, i9-10900KF): the streaming bench's chord on `VoxelDemo` (1,500 uu/s
over 40,000 uu) and great circle on `PlanetTest` (30N 20E, east), the clock at noon and stopped, the PIE viewport
2,874 x 1,358, saves set aside before every PIE; three runs per configuration, the four configurations interleaved in
each round, the sibling sessions' editors and suites closed. Per run the traverse phase's P50 / P95; the table gives the
median of the three runs (the three in brackets):

| Route | Configuration | GPU P50 ms | GPU P95 ms | Game thread P50 ms | Catch-up s |
|---|---|---|---|---|---|
| chord | hardware Lumen, as today | 6.59 (6.59 / 6.54 / 6.68) | 8.42 | 17.49 | 0.17-0.20 |
| chord | + ray tracing geometry | 6.75 (6.74 / 6.75 / 6.82) | 8.62 | 18.10 | 0.17-0.18 |
| chord | software Lumen, as today | 6.73 (6.73 / 6.74 / 6.58) | 8.98 | 16.66 | 0.17-0.19 |
| chord | software Lumen + distance fields | 8.33 (8.33 / 8.40 / 8.29) | 10.58 | 17.88 | 0.17 |
| great circle | hardware Lumen, as today | 5.76 (6.07 / 5.70 / 5.76) | 8.12 | 18.98 | 0.8-1.2 |
| great circle | + ray tracing geometry | 5.94 (5.94 / 6.01 / 5.93) | 8.22 | 18.78 | 0.8-1.1 |
| great circle | software Lumen, as today | 6.01 (6.19 / 5.98 / 6.01) | 10.18 | 17.68 | 0.8-0.9 |
| great circle | software Lumen + distance fields | 8.41 (8.41 / 8.35 / 8.41) | 11.39 | 20.40 | 0.95-1.0 |

| | Ray tracing geometry | Distance fields (resolution 1: 0.75 m) |
|---|---|---|
| GPU frame, P50 | **+0.16 ms** (chord), **+0.18 ms** (great circle): the BLAS builds while streaming and the traces against them | **+1.6 ms** and **+2.4 ms** over software Lumen alone: the atlas uploads and the global distance field's updates as chunks stream |
| Game thread, P50 | +0.6 ms on the chord (all three runs), -0.2 ms on the great circle: plausibly the per-frame instance gather holding the render thread, which the game thread waits for | **+1.2 ms** and **+2.7 ms**: a component registered per chunk (0.14 ms each, about 1,400 a run) |
| Build per chunk | On the GPU (fast build), nothing on the workers: about 15,000 builds a run, chunks and seams, 11 M triangles | **13.6 ms** on one uncontended core per 64³ chunk (`BenchDF`; 9.4 ms at resolution 0.5, 2.5 ms for a 32³ chunk), **22 ms** on average on the benches' busy workers and 45-220 ms at worst; once per mesh job (1,400 a run, about 31 s of worker time) |
| Memory | **57 MB** of BLAS on the demo (1,098 meshes, about 52 KB each), 47 MB on the planet (867); for scale, the demo's terrain meshes are 24 MB | **103 MB** on the demo (535 chunks with a surface, 193 KB each, 370 bricks), 85 MB on the planet (419); 49 KB a chunk at resolution 0.5 |
| Every frame | 1,098 dynamic ray tracing instances gathered (the bench's render thread column reads 0 in this build, so that cost is not measured; the static path removes it) | nothing |

The frame times are the editor's (PIE inside the editor at a fixed viewport), so read the deltas, not the totals.
Switching Lumen to software tracing by itself costs about the same as today (+0.14 / +0.25 ms); the distance fields'
cost is on top of it.

## 4. Fit with streaming, LOD, edits, the far field and seam ownership

| | Ray tracing geometry | Distance fields |
|---|---|---|
| Seam ownership | Built from the uploaded meshes, chunk and seam alike: no voxel is read, nothing crosses a chunk | Built from the chunk's own voxels; the engine composes objects by their minimum, so no neighbour is read. The chunk planes need the overlap above |
| Streaming | One BLAS build per upload on the GPU (fast build); released with the mesh | A build per mesh job on the worker, a primitive registered per chunk on the game thread |
| LOD | A LOD swap uploads a new mesh and builds its BLAS; only the current mesh is traced (a crossfade's outgoing half is not) | The field does not depend on the LOD, but the prototype rebuilds it at every mesh job; production builds it per content version, coarser for far chunks |
| Edits | The edit's re-mesh uploads and rebuilds | The edit's re-mesh rebuilds (the edit-merged voxels) |
| Proxy recreation | A recreated proxy starts empty and `RecoverLostRenderData` re-sends every mesh, so the BLAS come back with it (G15); toggling `r.Lumen.HardwareRayTracing` recreates the proxies | Components survive a proxy recreation |
| The far field | Procedural mesh components: the engine's `r.RayTracing.Geometry.ProceduralMeshes` (1) puts them in the ray tracing scene already (not measured here) | None: procedural meshes have no distance fields; far distance field occlusion would need fields per patch |
| Scatter | HISMs are in the ray tracing scene already (the trees in the debug view) | Static meshes carry their own fields |

## 5. Recommendation and plan

**Path: ray tracing geometry per chunk and seam mesh (path 3), then a Lumen surface cache for the terrain.** It fixes F3.3
in the project's Lumen mode, costs (§3) about 0.2 ms of GPU frame and 50-60 MB of BLAS at the demo's view distance, needs no CPU work on the workers and no new primitive per chunk,
and reuses the uploaded buffers. Distance fields (path 2) stay the answer only if the project moves to software Lumen
or wants distance field shadows for the far range (D-VP3 chose far cascades); keep the builder on the spike branch.

| Step | Work | Size |
|---|---|---|
| 1. RT geometry in production | Move the spike's build into `CreateMeshBuffers_RenderThread`'s callers behind `voxel.Render.RayTracing` (default on where `IsRayTracingEnabled()`); the static path (`GetStaticRayTracingInstances` with cached instances per region) instead of the per-frame dynamic one, so thousands of instances are not rebuilt every frame; the seam buckets' fades; memory in `GetGPUMemoryUsage` and the bench columns; the in-editor gate (a test that a chunk's BLAS exists after upload and goes with the mesh); `RENDERING_SYSTEM.md` | M |
| 2. Surface cache (cards) | Lumen captures cards from static mesh draw commands only (`LumenSceneCardCapture.cpp` walks `StaticMeshRelevances`), so the terrain needs `DrawStaticElements` batches per chunk (a cached mesh draw command per chunk: a change to how the proxy draws) and a `FCardRepresentationData` per chunk (six axis cards over its box, or cards fitted to the surface's facing). Gives the terrain bounce light and lit reflections; without it hits are black (§2) | L |
| 3. Hit lighting as the interim | Where step 2 is not done, `r.Lumen.HardwareRayTracing.LightingMode 1` for reflections and GI evaluates the terrain material at hits; needs the vertex factory's position SRV (the proxy binds a null one today) and costs GPU | S, with a GPU cost to measure |
| (Distance fields, if ever) | Per-content-version builds, coarser by LOD; registration without a component per chunk (`FScene::AddPrimitive` with a primitive scene description, or a region-wide field assembled from per-chunk bricks); box overlap by the band | L |

F3.3 stays open until step 1 lands; D-VP4 is recorded as decided (the spike, (d) meanwhile).
