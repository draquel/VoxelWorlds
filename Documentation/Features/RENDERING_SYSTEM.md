# Rendering System Architecture

> State as of 2026-11-04 (code review CR5; F3.3 ray tracing geometry; G116 steps 1-3, the chunk primitives). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelRendering
**Dependencies**: VoxelCore

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Key Components](#key-components)
4. [Render Regions](#render-regions)
5. [Chunk Primitives](#chunk-primitives)
6. [Shadow Culling](#shadow-culling)
7. [Crossfades](#crossfades)
8. [Vertex Format and Colour Encoding](#vertex-format-and-colour-encoding)
9. [Renderer Interface](#renderer-interface)
10. [Water Tile Rendering](#water-tile-rendering)
11. [What the Renderer Does Not Draw](#what-the-renderer-does-not-draw)
12. [Collision](#collision)
13. [Integration Flow](#integration-flow)
14. [Debugging](#debugging)

---

## Overview

The rendering system has one implementation of the `IVoxelMeshRenderer` interface, `FVoxelMeshRenderer`: a
`UVoxelRegionComponent` per render region, whose `FVoxelSceneProxy` uploads each chunk, seam and water mesh into
UE's `FLocalVertexFactory` and crossfades mesh swaps. The `ProceduralMeshComponent` fallback renderer the design
started with was deleted in code review CR1 (D-CR7): no asset or script ever selected it, and it had not implemented
the seam, fade, lost-data or water-draw members since seam ownership. The streaming tests record into a stub.

### Design Goals

- **Performance**: no CPU-side copy of a mesh after its upload
- **Maintainability**: one renderer behind one interface, the chunk manager's view of it

### Why FLocalVertexFactory

The first design was a custom `FVoxelVertexFactory` with the mesher's packed 28-byte vertex, a per-chunk LOD morph
uniform and GPU mesher buffers bound directly. It drew terrain attached to the camera, translucent-looking or blank
under UE5's large world coordinates, and the project switched to Epic's `FLocalVertexFactory`: a larger vertex
(40 bytes), no vertex-shader morph (LOD changes crossfade between meshes instead), full material compatibility. The
GPU meshers it was built for were deleted in CR1 (D-CR12); meshes are uploaded from the CPU.

---

## Architecture

```
AVoxelWorldActor
    │
    ├── UVoxelChunkManager (component)
    │   └── Coordinates chunk streaming, meshing and the far field
    │
    └── IVoxelMeshRenderer (interface)
        │
        └── FVoxelMeshRenderer
            └── UVoxelRegionComponent (one per render region)
                ├── FVoxelChunkSceneProxy per chunk mesh and seam bucket (a primitive without a component, G116)
                │   └── FVoxelChunkMesh: FLocalVertexFactory, buffers, ray tracing geometry
                └── FVoxelSceneProxy
                    └── FLocalVertexFactory per water tile (and per chunk and seam mesh with voxel.Render.ChunkPrimitives 0)
```

---

## Key Components

**FVoxelMeshRenderer** (`VoxelRendering/Private/VoxelMeshRenderer.cpp`)
- Implements `IVoxelMeshRenderer`
- Keeps one `UVoxelRegionComponent` per render region and routes every chunk, seam, water tile and fade to its region
- Converts the mesher's `FChunkMeshData` to vertices (`ConvertToVoxelVertices`) and assigns the configuration's
  runtime virtual textures before registration

**UVoxelRegionComponent** (`VoxelRendering/Public/VoxelRegionComponent.h`)
- `UPrimitiveComponent` that bridges game thread and render thread
- Creates `FVoxelSceneProxy` for actual rendering
- Drives the crossfades (`FVoxelFadeScheduler`, the fade material instances)
- Pushes `voxel.Material.MacroVariation` into `MPC_VoxelMaterial` ([MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md))
- Exposes statistics and chunk queries (`GetChunkDrawInfo`)

**FVoxelSceneProxy** (`VoxelRendering/Private/VoxelSceneProxy.cpp`)
- Manages per-chunk `FLocalVertexFactory` instances, created in `ProcessBatchUpdate_RenderThread`
- Culls chunk and seam batches per view and issues draw calls in `GetDynamicMeshElements()` (at most 2,000 batches a
  frame: chunks, their RVT clones, seam buckets, water tiles)
- Opts into the runtime virtual texture pass when the component has RVTs and emits the RVT batches
- Holds the only copy of the GPU geometry: the engine recreates the proxy on material and render-state changes
  (also in the first second of play, when the scatter system compiles its runtime billboard material), and a new
  proxy starts empty. `CreateSceneProxy` flags it, `IVoxelMeshRenderer::ConsumeLostRenderData` reports it once, and the
  chunk manager re-meshes the chunks that had a mesh and re-sends every seam bucket and water tile
  (`UVoxelChunkManager::RecoverLostRenderData`; G15, test `Streaming.Cubic.RendererProxyRecreated`), also while
  streaming is paused (`TickWhilePaused`; F3.4, test `Streaming.Cubic.RendererProxyRecreatedWhilePaused`)

**FVoxelChunkSceneProxy** (`VoxelRendering/Public/VoxelChunkSceneProxy.h`)
- One primitive per chunk interior mesh and seam bucket, registered by its region component without a component of its
  own; draws on the static path, and on the dynamic one while it fades (below, [Chunk Primitives](#chunk-primitives))
- Holds `FVoxelChunkMesh`es: the GPU geometry of one mesh (vertex factory, buffers, ray tracing geometry), shared by the
  primitive that draws it and the one fading out of it

**FVoxelLocalVertex** (`VoxelRendering/Public/VoxelLocalVertexFactory.h`)
- 40-byte vertex format compatible with `FLocalVertexFactory`, the CPU-side conversion; uploaded whole only where the
  vertex factory has no manual vertex fetch (below, G120)
- Converts from `FVoxelVertex` (the mesher's 28-byte output)
- Encodes MaterialID, face type, BiomeID and AO in UV1 and the vertex colour (below)

---

## Render Regions

The terrain renders in regions of `voxel.Render.RegionChunks`^3 chunks (`FVoxelRenderRegions`,
`VoxelCore/Public/VoxelRenderRegions.h`; 16 by default, 0 turns them off) in every world mode: planets since
spherical Phase 6 (WM-BP7, D23), heightmap worlds since the world modes review's MP2 (WM-MP5, D40), since a plane
100 km from its origin loses float precision as a big planet does.

- `FVoxelMeshRenderer` keeps one `UVoxelRegionComponent` per region at the region's origin (its min corner, in double),
  creating it on demand with the material, RVTs, atlas, distances and viewer replayed, and retiring it when it holds
  nothing (`UVoxelRegionComponent::HasRenderContent`). The primary component is region (0, 0, 0) and stays.
- The scene proxy stores vertices relative to its component's location (`FVoxelSceneProxy::RenderOrigin`,
  `ToRenderOffset`), so a float offset is at most a region across: a vertex 1,000 km from the origin is within
  0.0005 uu of the double reference, on a plane and on a planet (test `Rendering.Planet.RegionPrecision`).
- Scatter keeps one HISM per type and region (`FVoxelScatterHISMKey`) at the region's origin.
- **Water-only regions (G77).** A water tile goes to the region of its column's Z-0 chunk, which over a lake is often
  air with no mesh. Such a region draws its water tiles: the proxy draws with water tiles alone, the component's render
  bounds (`CalcBounds`) are the union of its chunk meshes and its water tiles' boxes (`WaterTileWorldBounds`) padded by
  a chunk, and every bounds change schedules the tick that sends them to the scene (`RequestRenderBoundsRefresh`).
  Before, a lake under such a region drew its bed without water in a block of chunk columns seen from altitude
  (test `Rendering.WaterTile.ComponentBounds`; `voxel.Water.ResendTiles`).

---

## Chunk Primitives

Every chunk interior mesh and seam bucket is a primitive of its own (G116 steps 1-3, 2026-11-04; the plan is
[LUMEN_TERRAIN_SURFACE_CACHE_PLAN.md](../Research/LUMEN_TERRAIN_SURFACE_CACHE_PLAN.md)). The engine culls static meshes,
caches ray tracing instances and captures Lumen's cards per primitive, so a region proxy drawing a thousand meshes got
none of it; a primitive per mesh gets all three. `voxel.Render.ChunkPrimitives` (1) chooses it; 0 is the region proxy
drawing every chunk and seam mesh itself, as before (a change re-meshes the terrain).

- **No component per chunk.** `UVoxelRegionComponent` registers each `FVoxelChunkSceneProxy` through
  `FPrimitiveSceneDesc` (`FSceneInterface::BatchAddPrimitives`, and `BatchRemovePrimitives` by proxy), as the engine's
  FastGeo streaming does: the spike measured 0.14 ms of game thread per registered component. Submits, re-meshes and
  removals queue on the game thread and go to the scene in one batch from the renderer's flush. A primitive sits at its
  chunk's origin (its vertices are float offsets a chunk across) with its mesh's box, padded by two voxels, as bounds.
- **The region component keeps** the water tiles (its own proxy draws them, re-sent from the component's copies when the
  engine recreates that proxy), the crossfade driver and G15's recovery: a recreated region proxy no longer loses the
  chunks, since they are not its own. A primitive's description is the component's (shadow, decals, virtual textures,
  ray tracing), with `bCastFarShadow` per primitive from its mesh's LOD (D-VP3's far cascades take LOD
  `voxel.Render.FarShadowMinLOD` and coarser, the region proxy's per-mesh filter).
- **Static draw.** `DrawStaticElements` gives the terrain batch and its runtime virtual texture clones once; the scene
  caches their draw commands per pass in GPUScene and culls the primitive per view and shadow cascade. The region proxy's
  per-mesh loop and its 2,000-batch cap are gone for the terrain.
- **Fades through dynamic relevance** (step 2). A fading primitive answers dynamic relevance for the fade's length and
  draws its two dithered halves in `GetDynamicMeshElements` (the crossfade, an appearance, a held swap, a seam bucket
  fading out), as the region proxy did; its static batches are not drawn meanwhile, and no cached command changes when a
  fade starts or ends. A new mesh is a new primitive that holds the outgoing one as its Previous, so the swap and the
  fade's first frame are the same frame; the fade's end clears the Previous on the render thread
  (`voxel.Render.ChunkPrimitives.Dynamic 1` draws every primitive dynamically, a diagnostic).
- **Cached ray tracing** (step 3). Each mesh's `FRayTracingGeometry` has a geometry group of its own, and the primitive
  caches its one instance (`CacheInstances`): a build completing or a mesh released dirties that primitive only. There is
  no per-frame gather any more, so every chunk is in the ray tracing scene again, not only those within the view's Lumen
  trace distance; `voxel.Render.RayTracing.TraceDistanceScale` acts on the region proxy's path alone.
- **Release.** The scene deletes a removed primitive in a task of its update; the primitive's meshes go to a queue the
  render thread drains each frame, where their vertex factories, buffers and ray tracing geometry are released.

What the engine needs from primitives without components, found the hard way:

- **Material and shader updates find primitives through `IPrimitiveComponent` only** (the renderer's `UpdateStaticDrawListsForMaterials`,
  the shader compiler's `PropagateMaterialChangesToPrimitives`, `FGlobalComponentRecreateRenderStateContext`). Without
  it a recompiled material left cached draw commands on freed shaders: a crash in `SetShaderBindings`. The module
  registers a resolver on `UVoxelRegionComponent` (`IPrimitiveComponent::AddImplementer`, FastGeo's pattern) that gives
  one interface per primitive; in the editor each add and removal tells the object cache
  (`FObjectCacheEventSink::NotifyUsedMaterialsChanged_Concurrent`), and the component's `GetUsedMaterials` names its
  materials (the default returns none).
- **The scene deletes proxies beside the tasks that cache new ones' draw commands**: releasing render resources in the
  destructor there gave commands bound to stale uniform buffers, hence the render thread's release queue.
- **`FSceneInterface::UpdateAllPrimitiveSceneInfos` asserts inside a render command pipe's replay**: nothing calls it
  from a render command.

Measured 2026-11-04 (the streaming bench, `voxel.Render.ChunkPrimitives` 0 and 1 interleaved, three runs each, noon
with the clock stopped, the 2,874 x 1,360 PIE viewport, ray tracing on; `RTBusyMs` is the render thread's own CPU time
in its frame, since the engine's `RTMs` reads 0 in the editor's PIE):

| Route | Path | GPU frame P50 / P95 | Render thread P50 / P95 | Game thread P50 / P95 | Primitives, added a second |
|---|---|---|---|---|---|
| `VoxelDemo`'s chord | region proxy | 6.32 / 8.02 | 8.85 / 14.52 | 18.57 / 27.58 | 0 |
| | chunk primitives | 6.37 / 8.05 | 8.63 / 14.59 | 18.35 / 27.72 | 1,079, 159 |
| `PlanetTest`'s great circle | region proxy | 6.50 / 8.73 | 12.72 / 23.57 | 23.74 / 35.91 | 0 |
| | chunk primitives | 6.36 / 8.65 | 12.53 / 23.56 | 24.42 / 37.55 | 1,070, 190 |

The great circle's six runs a side ran while other work loaded the machine: its game thread moved by -1.3 to +4.1 ms
between adjacent runs of the two paths, and an earlier quieter set (before the review fixes, three runs a side) gave
20.13 → 19.88 ms game thread and 6.22 → 6.04 ms GPU.

The cost the plan feared, caching every pass's commands at each of 160-190 adds a second, does not show on the render
thread, and the GPU is unchanged. Ray tracing at rest is the same geometry (`VoxelDemo` 1,098 meshes, 56.7 MB of BLAS;
`PlanetTest` 865, 45.9 MB), now all of it traced where the region proxy gathered 291-303 a frame. While streaming the
peak is about 85 geometries and 6 MB higher (1,163 against 1,080, 68 against 62 MB on the chord): a fading primitive's
outgoing mesh keeps its geometry until the fade ends. `r.RayTracing.Visualize` from 6,000 uu above `VoxelDemo`'s ground shows
the ridges past the Lumen trace distance in the ray tracing scene, which the gather left out. Lumen's frame is unchanged
until the cards: F3.3's repro gives the same figures on both paths (under the overhang 0.177 → 0.159, 0.90, the sealed
cave 0.111 on `VoxelDemo`; 0.433 → 0.378, 0.87, and 0.029 on `PlanetTest`), since a ray past the trace distance still
ends in screen traces and a terrain hit is still black. Tests `Rendering.ChunkPrimitives.Lifecycle`, `.Crossfade`
(`Crossfade.SwapReachesProxy` on the chunk primitives) and `.RayTracing` (`Rendering.RayTracing.ChunkGeometry`
likewise), in the in-editor gate.

---

## Shadow Culling

The proxy culls each batch per view (`FVoxelViewCull`): against the view's frustum, and in a shadow depth pass
against the shadow's caster volume (`FSceneView::GetDynamicMeshElementsShadowCullFrustum`, boxes moved by
`GetPreShadowTranslation`). A shadow pass calls with a snapshot of the camera's view whose `ViewFrustum` is still the
camera's; culling with it left every chunk out of view out of the shadow map, so hills behind or beside the camera
cast no shadow until turned into view. With the caster volume a hill out of view casts again, and shadow-pass voxel
batches fell from about 1,070 to 335 a frame at no GPU cost (VP3-O1). A shadow pass also skips the RVT clones and
water, which cast no shadow.
`FVoxelBatchCounters::FCounts::ShadowDrawn` and the bench's `ShadowBatchN` column count the batches drawn into shadow
maps.

### Far Shadow Cascades

The sun's dynamic cascades end at the light's `DynamicShadowDistanceMovableLight` (20,000 uu on the maps) against a
40,000 uu view, so past 200 m nothing cast a shadow and the shadow's edge moved with the camera (visual polish F3.2).
Since D-VP3 every map's sun has two far cascades from there to `FarShadowDistance` 40,000
(`Scripts/setup_far_shadow_cascades.py`; the day/night controller drives the map's light, `ADayNightController::SunLight`,
and leaves its shadow settings alone). The engine draws into a far cascade only the primitives with `bCastFarShadow`,
which the region components set, and flags the gather with `EDrawDynamicFlags::FarShadowCascade`.

In a far cascade the proxy draws the coarse terrain only (`FVoxelViewCull::bFarShadowCascade`): chunks and seam buckets
of LOD `voxel.Render.FarShadowMinLOD` (1) and coarser, each the current mesh once with the opaque material (no
crossfade halves: a dither mask is noise at a far cascade's texel), culled by the caster volume as every shadow gather
is. The far cascades' receivers lie past 20,000 uu, in the coarsest band (LOD 2 from 14,000 on the demo bands), so they
cast with the mesh they draw and do not self-shadow. A LOD 0 chunk (within 7,000 uu) shadows a far receiver only at a
very low sun; it is left out. Neither scatter's HISMs nor the far field cast into the far cascades (the HISMs keep
their own `bCastFarShadow`, off; the far field's components cast no shadow at all).
`FCounts::FarShadowDrawn`, `SkippedTooFine` and `MaxPerCall` and the bench's `FarShadowBatchN`, `MaxCallBatchN`
columns (the report's `farShadowBatchesDrawnP50`, `farShadowBatchesDrawnP95`, `farShadowBatchesDrawnMax`, `maxBatchesPerCall`) count them; one call draws at most 2,000 batches.

Measured 2026-11-02 (the gate editor at 2,874 x 1,360, the bench's chord on `VoxelDemo` and great circle on
`PlanetTest` with the clock stopped; the far cascades off is the engine's r.Shadow.MaxNumFarShadowCascades 0):

| Sun | Route | Voxel shadow batches a frame (P50), off → on | Of them in the far cascades | Most in one call |
|---|---|---|---|---|
| 17:30 (12° up) | chord | 672 → 2,460 | 1,700 | 550 |
| 17:30 | great circle | 500 → 1,100 | 550 | 400 |
| 12:00 | chord | 239 → 594 | 356 | 284 |
| 12:00 | great circle | 248 → 706 | 435 | 393 |

A low sun stretches every cascade's caster volume toward the light, so the far cascades take most of the window's
coarse chunks. `voxel.Render.FarShadowMinLOD 2` only trims the low-sun chord's far batches to 1,480. GPU frame P50 moved
by +0.3 ms at the low sun on the chord (6.48 → 6.77 in the quietest interleaved pair) and +0.1 ms on the great circle;
the other pairs were inside the shared machine's noise (other sessions' editors were running). The picture changes
from raised views: a hill's shadow that faded at 200 m now runs on across the far slope; at eye level the ground past
200 m is a thin band and almost no pixel changes. With the near cascades shortened (r.Shadow.DistanceScale 0.1) every
terrain shadow past 2,000 uu comes from the far cascades, which shows them working end to end.

### What Lumen Sees

Lumen (hardware ray tracing on this project: `r.Lumen.HardwareRayTracing` 1) traces the ray tracing scene, so the
terrain is in it: every current chunk mesh and seam bucket of `FVoxelSceneProxy` has an `FRayTracingGeometry` over the
vertex and index buffers the proxy uploaded (visual polish F3.3, 2026-11-03; D-VP4's spike chose the path,
[LUMEN_OFFSCREEN_TERRAIN_SPIKE.md](../Research/LUMEN_OFFSCREEN_TERRAIN_SPIKE.md)). An overhang that leaves the screen
keeps darkening the ground under it, and a sealed cave stays dark.

- **Built once on the GPU.** An upload builds its mesh's acceleration structure (fast build, no updates); nothing runs
  on the workers and no voxel is read. The geometry belongs to the mesh: an LOD swap, an edit's re-mesh and a seam
  rebuild upload new buffers and build anew, an unload or a cleared bucket releases it, and a recreated proxy gets its
  meshes, and with them its geometry, back through `RecoverLostRenderData` (G15, F3.4).
- **The current mesh only.** A crossfade's outgoing mesh, a held swap's visible Previous and a seam bucket fading out
  have none: rays see the mesh the fade ends on.
- **Cached per chunk** on the chunk primitives (the default since G116, [Chunk Primitives](#chunk-primitives)): every
  chunk and seam mesh whose build has completed is in the ray tracing scene, with no per-frame work. The rest of this
  bullet is the region proxy's path (`voxel.Render.ChunkPrimitives 0`).
- **Gathered every frame** (`GetDynamicRayTracingInstances`), one instance per mesh whose build has completed, with the
  terrain material; a mesh still building joins the next frame. Each mesh keeps its mesh batch and the instances view it
  and the proxy's transform, so the gather allocates nothing. Only meshes whose box is within the view's Lumen max
  trace distance (the post process setting, 20,000 uu by default) times `voxel.Render.RayTracing.TraceDistanceScale`
  (1) are gathered: about 290 of the window's 1,100 meshes. The engine's per-instance work each frame is the gather's
  cost: all 1,100 cost the game thread +0.9 ms (chord) and +1.5-3.0 ms (great circle). A ray from a pixel near the camera
  ends before the meshes left out; a pixel farther than the trace distance traces as before (screen traces). The
  engine's cached path (`CacheInstances`) would remove the per-frame work, but it takes instance transforms from
  GPUScene, which this proxy does not use (its batches carry the primitive uniform buffer): a first version on that path
  cached its instances and none reached the ray tracing scene.
- **Cost** (2026-11-03, the streaming bench, `voxel.Render.RayTracing` 0 and 1 interleaved, three runs each, noon with
  the clock stopped, a 2,874 x 1,358 PIE viewport): GPU frame P50 +0.14 ms on `VoxelDemo`'s chord (6.45 → 6.59) and
  +0.10 ms on `PlanetTest`'s great circle (5.82 → 5.92), game thread P50 +0.29 and +0.42 ms; 62 MB of BLAS at the
  chord's peak (55-57 MB at rest, about 1,100 meshes) and 68 MB on the great circle (46 MB at rest), the bench's
  `RTGeomN` and `BLASMB` columns (`peakRayTracingMB` in its JSON). Built on the GPU: about 19,000 builds a run, nothing
  on the workers.
- **Off** with `voxel.Render.RayTracing 0` (a change recreates the region proxies, whose chunks re-mesh), or where the
  project or the RHI does not allow ray tracing. `voxel.Render.RayTracingStats` gives the geometries alive, their BLAS
  megabytes and the instances the last frame gathered; test `Rendering.RayTracing.ChunkGeometry` (the in-editor gate).

What it does not give yet:

- **Bounce light from the terrain.** A ray that hits the terrain finds no Lumen surface cache (the terrain has no mesh
  cards) and returns black: occlusion is right, the bounce off the terrain is lost, so the ground under an overhang
  reads a little darker off screen than on screen (0.90 of it on `VoxelDemo`) and on a planet's shaded ground can go
  black. Lumen captures cards from a primitive's cached static mesh draw commands; since G116's steps 1-3 every chunk
  mesh is such a primitive ([Chunk Primitives](#chunk-primitives)), and step 4 gives each its cards (G116 in the
  project's findings; the plan is [LUMEN_TERRAIN_SURFACE_CACHE_PLAN.md](../Research/LUMEN_TERRAIN_SURFACE_CACHE_PLAN.md)).
- **Hit lighting is not the answer.** `r.Lumen.HardwareRayTracing.LightingMode 1` evaluates the terrain's material and
  direct light at a hit, and works with this proxy as it is (hit shaders take positions from the ray tracing geometry,
  not the vertex factory's position view, which the proxy leaves null). Sunlit terrain seen in a ray turns from black to
  lit, but terrain in shadow still reads its own indirect light from the missing surface cache: off screen under the
  overhang 0.160 → 0.171 on `VoxelDemo` and 0.376 → 0.383 on `PlanetTest`, the sealed caves unchanged (0.111, 0.029),
  for +1.3 ms of GPU frame P50 on the chord (4.91 → 6.20) and +0.85 ms on the great circle (4.83 → 5.68) (the streaming bench, three interleaved runs each) (2026-11-03, G116's work; captures on the parent's `media/g116-surface-cache-plan` branch).
- **Water.** Water tiles have no ray tracing geometry: the sea is translucent (or Single Layer Water) and is not an
  occluder Lumen needs.
- **The far field, unless Lumen's far field is on.** Its procedural mesh components build their own ray tracing
  geometry, a BLAS per section, but the engine culls the near-field layer past `r.RayTracing.Culling.Radius` (30,000
  uu, culling mode 3, in front of the camera too), and every patch drawn lies past the voxel window: none was ever
  traced (G117; `r.RayTracing.Culling 0` shows them all). The patches now sit in the engine's far-field layer
  (`bRayTracingFarField`, `voxel.World.FarFieldRayTracing` 1), which Lumen traces past the near field's reach when
  `r.LumenScene.FarField` is 1, out to `r.LumenScene.FarField.MaxTraceDistance` (10 km); the project leaves it 0, and
  then nothing gathers them. Turned on, it changes nothing visible on `VoxelDemo` from 9,000 uu up or on `PlanetTest`
  at 1.6 R (a hit is black for the same reason as the terrain's, G116) and costs +0.09-0.15 ms GPU P50 (the streaming
  bench, three interleaved runs, noon with the clock stopped: chord 6.34 → 6.46, great circle 5.85 → 5.94, a great
  circle at 1.6 R 4.82 → 4.97), so it stays off (decision D42 in the project's findings). The layer itself costs nothing
  against the near-field layer: both cull every patch with Lumen's far field off. The BLAS is built whatever the mode:
  103-110 MB for `VoxelDemo`'s far field (about 5,000 sections, two thirds of the scene's ray tracing geometry), 52 MB
  for `PlanetTest`'s at 1.6 R and 27 MB on the ground (G121).

| `VoxelDemo`, overhang and sealed cave (D-VP4's repro) | Under the overhang, on screen → off screen | Off / on | Sealed cave |
|---|---|---|---|
| `voxel.Render.RayTracing 0` (Lumen in screen space only) | 0.240 → 0.258 | 1.08 (brightens: F3.3) | 0.247 |
| `voxel.Render.RayTracing 1` | 0.177 → 0.159 | 0.90 | 0.111 |
| `PlanetTest` (30N 20E), 0 | 0.443 → 0.398 | 0.90 | 0.090 |
| `PlanetTest`, 1 | 0.433 → 0.378 | 0.87 | 0.029 |

Mean luminance of the inner 60 % of the compared regions, noon with the clock stopped, exposure and anti-aliasing off;
the spike's numbers for its prototype were 0.176 → 0.159 (0.91) on the demo and a planet cave of 0.039.

---

## Crossfades

A chunk's mesh is never swapped in one frame. When a chunk re-meshes (an LOD change, a correction) the proxy keeps the
previous mesh and draws both with complementary dither masks for `voxel.MeshFade.Duration` (0.35 s) through the
auto-derived Masked `<Master>_Fade` material; `voxel.MeshFade 0` swaps at once. `FVoxelFadeScheduler` keeps the clock:
interiors and seam buckets share one scheduler so everything released together fades together.

- **Holds** (R24). An LOD-change interior swap holds at the old mesh until its rebuilt seams land, then fades in with
  them (`voxel.MeshFade.SeamHoldMs`, 1,000 ms, a failsafe; a ring sweep releases in 0.2-0.6 s). The chunk manager
  drives them through `SetChunkFadeHold` / `SetSeamFadeHold`.
- **Appearances** (R24). New geometry at the streaming frontier fades in from nothing, held until its ready seams land
  (`voxel.MeshFade.AppearHold`); only geometry beyond `voxel.MeshFade.AppearMinDistance` (7,000 uu, the LOD 0 band's
  edge) fades, so nothing near the player turns see-through. The distance is measured from the camera
  (`SetViewerPosition`), not from the streaming viewer, which a descending camera's prefetch puts below it.
- `voxel.MeshFade.MaxConcurrent` (512) caps fades in flight; `voxel.MeshFade.Stats [fades] [x y z]` lists them, the
  holds, the proxy's fade states and what the last frame drew of a chunk (tests `Rendering.Crossfade.SwapReachesProxy`,
  `Streaming.EditReachesProxy.*`, which need a real RHI).

On the chunk primitives a fade is a primitive drawing dynamically for its length ([Chunk Primitives](#chunk-primitives));
the scheduler, the holds and the materials are the same, and so are the tests (`Rendering.ChunkPrimitives.Crossfade`).

The scheduling side (when a swap is held, seam buckets) is in [STREAMING_AND_SEAMS.md](STREAMING_AND_SEAMS.md).

---

## Vertex Format and Colour Encoding

`FVoxelLocalVertex` (40 bytes): position, tangent X and Z (`FPackedNormal`), UV0 (the face UVs for tiling), UV1, colour.

| Attribute | Data | Material access |
|-----------|------|-----------------|
| UV1.x | MaterialID (exact float) | `MF_GetMaterialID` |
| UV1.y | Face type against local up: 0 top, 1 side, 2 bottom (on a planet the radial direction) | |
| Colour R | MaterialID (0-255); in the far field the quad's second material (G45) | `round(VertexColor.R * 255)` |
| Colour G | BiomeID (0-255) | `round(VertexColor.G * 255)` |
| Colour B | AO in the top 2 bits (0, 64, 128, 192) | `floor(VertexColor.B * 4)` → 0-3 |
| Colour A | 255; in the far field 255 less the second material's weight at the vertex (G45) | |

```cpp
// In material:
AO = floor(VertexColor.B * 4);  // 0-3
AOFactor = 1.0 - (AO * 0.25);   // 1.0, 0.75, 0.5, 0.25
FinalColor = BaseColor * AOFactor;

```



### The Mesh Buffers

`FVoxelSceneProxy::CreateMeshBuffers_RenderThread` uploads every chunk, seam and water mesh as five buffers: the vertex
stream, the colour (4 bytes a vertex), tangent (8) and texcoord (UV0 and UV1, 16) buffers behind the vertex factory's
shader resource views, and the 32-bit indices. Where `FLocalVertexFactory` has manual vertex fetch at the proxy's feature
level (`bPositionOnlyVertexStream`: every desktop RHI the project targets) its shaders read only the position stream
and fetch the other attributes from the views, so the vertex stream holds the positions alone (12 bytes,
`FVoxelChunkRenderData::VertexStride`); elsewhere it holds the interleaved `FVoxelLocalVertex` (40). The tangent,
texcoord and colour stream components name the position buffer flagged `ManualFetch`: no stream binds them, and since
the position buffer is also the tangent one the vertex factory makes no position-only declarations, so the depth and
shadow passes draw as before. The ray tracing geometry (F3.3) reads the positions at the stream's stride. A vertex
costs 40 bytes on the GPU with manual vertex fetch, 68 without, plus the indices.

Each proxy counts the bytes of the mesh buffers it holds (`FVoxelSceneProxy::GetMeshBufferBytes`: added at upload,
taken off by `FVoxelChunkRenderData::ReleaseResources`), crossfade-retained meshes included; the renderer's
`GetGPUMemoryUsage`, the streaming bench's `RendGPUMB`, sums its region proxies
(`UVoxelRegionComponent::GetProxyGPUMemoryUsage`), and `VoxelWorlds.Rendering.RayTracing.ChunkGeometry` checks the
count and the stride through an upload, a crossfade, a seam and an unload.

Results (2026-11-04, VoxelWorlds `fix/g119-g120-vertex-memory`):
- **G119.** `FVoxelChunkRenderData::GetGPUMemoryUsage` counted the texcoord buffer at 8 bytes a vertex; it is 16
  (`CalcGPUMemoryUsage`). And `RendGPUMB` was not the proxies' figure: it was the mesher's vertices (`FVoxelVertex`,
  28 bytes) and the indices of the chunk meshes alone. It reads what the proxies hold now, about 2.5 times the old
  figure (chord peak / end 24.3 / 23.2 MB on main, 64.1 / 56.9 counted; great circle 26.6 / 25.8, 66.4 / 61.5).
  The proxy's memory footprint (`GetGPUMemoryUsage`, memreport) now covers the seam and water meshes it left out.
- **G120.** Every vertex went up twice, interleaved (40 bytes) and in the views (28), and the shaders read 12 of the
  interleaved 40. With the positions alone: `RendGPUMB` chord 64.1 / 56.9 → 43.0 / 38.7 MB (-33% / -32%), great
  circle 66.4 / 61.5 → 45.8 / 41.6 (-31% / -32%), three runs each, the clock fixed; the RHI's own sizes on `VoxelDemo`
  (settled, streaming frozen): the chunk vertex buffers 16.37 → 4.91 MB (12/40), the other four unchanged. GPU frame
  and game thread P50 unchanged (two adjacent pairs of fresh-editor sets: chord GPU 6.94 → 6.80 ms, game thread
  18.36 → 18.60; great circle 6.26 → 6.22 and 21.18 → 21.19; the machine drifted +2 ms of game thread over the session,
  so sets an hour apart do not compare). Mesh digests
  (209), the editor gate (20/20, the 111 parity lines) and `voxel.Render.RayTracingStats` on `VoxelDemo` (940
  geometries, 51.4 MB) unchanged; the captures within the run-to-run floor.

---

## Renderer Interface

`Source/VoxelRendering/Public/IVoxelMeshRenderer.h`, called from the game thread only. The groups:

| Group | Members |
|-------|---------|
| Lifecycle | `Initialize`, `Shutdown`, `IsInitialized`, `FlushPendingOperations` |
| Chunk meshes | `UpdateChunkMeshFromCPU`, `RemoveChunk`, `ClearAllChunks` |
| Seam meshes | `UpdateSeamMeshFromCPU`, `RemoveSeamMesh`, `ClearAllSeamMeshes`, `GetSeamMeshOwners` |
| Fades | `SetChunkFadeHold`, `SetSeamFadeHold`, `GetSeamFadeRemainingSeconds`, `WillFadeInChunk`, `SetViewerPosition` |
| Materials | `SetMaterial`, `GetMaterial`, `GetSurfaceMaterial`, `UpdateMaterialParameters`, `SetMaterialAtlas`, `GetMaterialAtlas` |
| Water | `SetWaterMaterial`, `UpdateWaterTileMesh`, `RemoveWaterTile`, `ClearAllWaterTiles`, `GetWaterTileDrawInfo` |
| Recovery | `ConsumeLostRenderData` |
| Queries | `IsChunkLoaded`, `GetLoadedChunkCount`, `GetLoadedChunks`, `GetGPUMemoryUsage`, `GetTotalVertexCount`, `GetTotalTriangleCount`, `GetChunkBounds`, `GetTotalBounds` |
| Debugging | `GetDebugStats`, `DrawDebugVisualization`, `GetRendererTypeName` |

---

## Water Tile Rendering

Water tiles are keyed by `FIntVector2` (XY chunk column), separate from terrain chunks keyed by `FIntVector`. See
[WATER_SYSTEM.md](WATER_SYSTEM.md) for the water system (flag storage, propagation, meshing, tile management).

```
FVoxelMeshRenderer::UpdateWaterTileMesh()
    ↓ ConvertToVoxelVertices (FChunkMeshData → FVoxelVertex[])
    ↓ the tile's render region
UVoxelRegionComponent::UpdateWaterTileFromCPUData()
    ↓ ENQUEUE_RENDER_COMMAND
FVoxelSceneProxy::UpdateWaterTileFromCPUData_RenderThread()
    ↓ GPU buffers, an FLocalVertexFactory, stored by FIntVector2

FVoxelSceneProxy::GetDynamicMeshElements()
    ├── [Pass 1] Terrain chunks and seams
    └── [Pass 2] Water tiles: WaterMaterial, the same culling, no shadow, not an occluder
```

**GetViewRelevance**: Water material relevance is OR'd with terrain material relevance so the scene proxy correctly
reports translucency support when water uses a translucent or Single Layer Water material. **CreateSceneProxy**: the
water material is re-synced to a recreated proxy.

### Water Material Requirements

- **Translucent** or **Single Layer Water** shading model, not opaque (water draws in a separate pass and needs sorting)
- Configured via `UVoxelWorldConfiguration::WaterMeshMaterial`; when null no water is drawn (the demo actor's
  fallback plane aside, `AVoxelDemoActor::bShowWaterPlane`)

---

## What the Renderer Does Not Draw

**An island's far ocean** (WM-E1): the chunk manager builds it on its own procedural mesh components, using
`IVoxelMeshRenderer::GetSurfaceMaterial()` (the material terrain is drawn with: the world component's atlas instance)
for its seabed.

**The far field** (a planet's since spherical Phase 6, WM-BP9; a plane's and an island's since MP3, WM-MP1;
[LOD_SYSTEM.md](LOD_SYSTEM.md) "The Far Field"): the chunk manager's `FVoxelFarFieldPresenter` keeps one procedural
mesh component per built patch, at the patch's origin in double with float offsets of the patch's size (the render
regions' rule, a patch being its own region): section 0 the terrain with `GetSurfaceMaterial()`, 1 the sea and 2 its
underside with the water material.

- A far-field terrain vertex is a voxel terrain vertex as the voxel vertex factory presents it
  (`FVoxelLocalVertex::FromVoxelVertex` with the planet's radial face up, +Z on a heightmap world): UV1 = (material id,
  face type), colour (the quad's second material, biome, AO 0, alpha 255 less the second material's weight; G45), the
  tangent cross(Z, N), UV0 the position on the normal's dominant plane in voxels (relative to the patch).
- Both materials are flat per triangle (vertices duplicated where quads' pairs differ) and the terrain material picks
  between them per pixel by the interpolated weight ([MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) "Far-Field Material
  Blend"); every voxel chunk vertex has alpha 255, weight 0.
- Sections are shown and hidden with `SetMeshSectionVisible` (a render command), so the window's hole moving costs no
  proxy rebuild; a component is created when its patch is built and goes when the patch is released or pruned. They
  are about 200 bytes a vertex (the component's CPU copy and the GPU one): 29-56 MB at the surveyed sites of the three
  test planets, up to 83 MB looking down on the small one from orbit (P6.4).
- Above the voxel cut-over ([LOD_SYSTEM.md](LOD_SYSTEM.md) "Orbit and Back") the window holds no chunk and the far
  field is all that draws the terrain: on the 50 km test planet 145 patches (18.5 MB) at 600 m, 59 (9 MB) from 1 R and
  41 (6 MB) from 2 R.

---

## Collision

Collision is cooked apart from the visual meshes, on workers, by `UVoxelCollisionManager` (VoxelStreaming): chunks
within a radius of the focus with a velocity lead, meshed with the main mesher at the collision LOD (whole-chunk
domain, the neighbours read from their boundary shells), built into a Chaos triangle mesh off the game thread and
registered on `UBoxComponent` holders under one container actor; edits mark chunks dirty, and a neighbour that arrives
after a cook re-cooks it (G32). The pipeline, its cvars and its audits are in
[STREAMING_AND_SEAMS.md](STREAMING_AND_SEAMS.md) "Collision cooking".

---

## Integration Flow

### Chunk Generation → Rendering

```
[Frame N] ChunkManager dispatches the chunk mesh to a worker
    ↓
[Frame N+1] the worker meshes it (FChunkMeshData)
    ↓
[Frame N+2] ChunkManager receives the mesh (ProcessCompletedAsyncMeshes)
    ↓
ChunkManager::OnChunkMeshingComplete()
    ↓
    ├→ MeshRenderer->UpdateChunkMeshFromCPU(ChunkCoord, LOD, MeshData)
    │   └→ batched upload on the render thread (ProcessBatchUpdate_RenderThread), with a crossfade
    │
    └→ the chunk's seams restaled and queued; collision queued if in range
```

---

## Debugging

| Command | What it shows |
|---------|---------------|
| `voxel.MeshFade.Stats [fades] [x y z]` | Fades, holds, the proxy's fade states; what the last frame drew of a chunk |
| `voxel.Water.ResendTiles` | Sends every water tile to its region again |
| `voxel.Stream.AuditResidency` | Renderer chunks and seam buckets with no tracked owner |
| `voxel.World.FarFieldStats` | The far field's patches and memory |

- **Missing chunks**: check the chunk's render region exists and holds it (`GetChunkDrawInfo`), and whether the proxy
  was recreated (`ConsumeLostRenderData`)
- **A swap that does not show**: judge an edit from above, not at the crosshair, where the held tool covers the cell
  (G74)
- Use RenderDoc to check the vertex factory's bindings and buffers
