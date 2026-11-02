# Rendering System Architecture

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelRendering
**Dependencies**: VoxelCore

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Key Components](#key-components)
4. [Render Regions](#render-regions)
5. [Shadow Culling](#shadow-culling)
6. [Crossfades](#crossfades)
7. [Vertex Format and Colour Encoding](#vertex-format-and-colour-encoding)
8. [Renderer Interface](#renderer-interface)
9. [Water Tile Rendering](#water-tile-rendering)
10. [What the Renderer Does Not Draw](#what-the-renderer-does-not-draw)
11. [Collision](#collision)
12. [Integration Flow](#integration-flow)
13. [Debugging](#debugging)

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
                └── FVoxelSceneProxy
                    └── FLocalVertexFactory per chunk, seam and water-tile mesh
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
  (`UVoxelChunkManager::RecoverLostRenderData`; G15, test `Streaming.Cubic.RendererProxyRecreated`)

**FVoxelLocalVertex** (`VoxelRendering/Public/VoxelLocalVertexFactory.h`)
- 40-byte vertex format compatible with `FLocalVertexFactory`
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
