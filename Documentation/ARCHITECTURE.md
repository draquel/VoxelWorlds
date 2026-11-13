# VoxelWorlds Architecture Overview

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

VoxelWorlds is a voxel terrain engine for Unreal Engine 5.8. Terrain is generated on the GPU (a CPU generator is its
twin), meshed on CPU workers, streamed around a viewer with LOD, collision and compression, and drawn by one renderer on
UE's `FLocalVertexFactory`; past the voxel window an analytic far field draws the terrain to the horizon. Worlds are an
infinite plane, a bounded island or a spherical planet (`EWorldMode`), each smooth (Marching Cubes or Dual Contouring)
or cubic (`EMeshingMode`). The plugin depends on no other project plugin.

This document is the map: the modules, the runtime pieces, how a chunk moves through them, and the decisions behind
them. Each system's detail is in its Features doc ([README.md](README.md) lists them).

## Modules

Ten runtime modules and one editor module, in `Source/` (the empty `VoxelWorlds` module was deleted in code review CR1).

| Module | Loading phase | Depends on (voxel modules) | Holds |
|--------|---------------|----------------------------|-------|
| `VoxelCore` | Default | none | `FVoxelData`, `FChunkDescriptor` and its residency, `UVoxelWorldConfiguration`, `UVoxelWorldsSettings`, the material and biome registries and `UVoxelMaterialAtlas`, `UVoxelEditManager`, `FVoxelSeaSurface`, `FVoxelCubeSphere`, `FVoxelRenderRegions`, the chunk codec |
| `VoxelGeneration` | PostConfigInit (global shaders) | Core | `FVoxelGPUNoiseGenerator` / `FVoxelCPUNoiseGenerator`, `FVoxelNoise`, `IVoxelWorldMode` and the three modes, `FVoxelWorldModeFactory`, `FVoxelSurfaceQuery`, `FVoxelBlockSurface`, conditioning zones and lines, `FVoxelTreeInjector` |
| `VoxelLOD` | Default | Core, Generation | `IVoxelLODStrategy`, `FDistanceBandLODStrategy` |
| `VoxelMeshing` | PostConfigInit | Core | `IVoxelMesher`: `FVoxelCPUCubicMesher`, `FVoxelCPUMarchingCubesMesher`, `FVoxelCPUDualContourMesher`; `FVoxelWaterMesher` |
| `VoxelRendering` | PostConfigInit (vertex factory, shader directory) | Core | `IVoxelMeshRenderer`, `FVoxelMeshRenderer`, `UVoxelRegionComponent`, `FVoxelSceneProxy`, `FVoxelFadeScheduler` |
| `VoxelScatter` | PostEngineInit | Core, Generation | `UVoxelScatterManager`, `UVoxelScatterRenderer`, `UVoxelScatterExclusionSubsystem` |
| `VoxelStreaming` | PostEngineInit | Core, LOD, Rendering, Generation, Meshing, Scatter | `UVoxelChunkManager` and its owners, `UVoxelCollisionManager`, `UVoxelWaterPropagation`, `FVoxelFarField`, `AVoxelWorldActor`, `FVoxelStreamingBenchmark`, the console commands (`Private/Diagnostics/`) |
| `VoxelMap` | PostEngineInit | Core, Generation, Streaming | `UVoxelMapSubsystem`, `FVoxelMapTileStore`, the edit overlay |
| `VoxelNavigation` | PostEngineInit | Core, Streaming, Generation | `UVoxelSurfaceNavigationSubsystem`, `FVoxelSurfacePathfinder` |
| `VoxelPCG` | PostEngineInit | Core, Generation, Streaming (+ the PCG plugin) | the voxel surface sampler and biome dispatcher nodes, `UVoxelPCGBiomeDecorationMapping` |
| `VoxelWorldsEditor` | Default, Editor only | Core, Generation | the field preview panel, `AVoxelFieldPreviewActor`, `FVoxelFieldImageBaker` |

`VoxelMap` and `VoxelNavigation` also link `VoxelLOD` and `VoxelRendering` for their tests' chunk world. Modules that
register global shaders or rendering types load at `PostConfigInit` so the shader system is ready; those that create
world subsystems or need the full runtime load at `PostEngineInit`. A wrong phase crashes the editor at startup
(`CreatePackage`, `ProcessNewlyLoadedUObjects`). Shaders live in `Shaders/Private` (generation, materials) and
`Shaders/Shared` (code shared with C++).

## The pieces at runtime

`AVoxelWorldActor` puts a world in a level: it holds the configuration, the material and atlas, and creates the
`UVoxelChunkManager`, the `FDistanceBandLODStrategy` and the `FVoxelMeshRenderer`. The chunk manager is the hub. It owns
the world mode and the noise generator, the edit manager, the collision manager, the scatter manager, water
propagation, the seam registry, the snapshot cache, the far field and the components the actor needs beyond the
chunks (`FVoxelWorldBoundaryOwner`, `FVoxelFarOceanOwner`, `FVoxelPlanetOceanOwner`, `FVoxelFarFieldPresenter`), the
water tiles (`FVoxelWaterTiles`) and the viewer policy (`FVoxelStreamingViewerPolicy`: the streaming viewer, the descent
lead, the camera-cut pass). The map, navigation and PCG modules are clients: they find the chunk manager in the world
and read through its queries and events.

Game code (VoxelCharacterPlugin, VoxelWorldPOI, the project's game module) reaches the world only through the chunk
manager's API (surface queries, the world mode's bounds and frame, the sea, edits) and `IVoxelEditValidator`.

## Data flow

### A chunk's life

1. **Window.** Each tick the LOD strategy turns the streaming viewer into the voxel window: the chunks to hold and
   their LOD (`IVoxelLODStrategy`). A heightmap streams each column's surface slab plus the viewer's layers; a planet
   streams by chunk (each chunk's surface slab, the near layers, a horizon along the ground). Above the cut-over
   altitude (with a far field on) the window is suspended and every chunk unloads.
2. **Generation.** A queued chunk is generated by the GPU compute shader (`GenerateVoxelDensity.usf`, then the
   post-passes for water and underground flags) and read back, or by the CPU generator on a worker. A chunk whose
   voxels are all the same is stored collapsed to one value.
3. **Meshing.** A CPU worker meshes the chunk's interior from its own voxels (`EVoxelMeshCellDomain::Interior`). Each
   boundary the chunk shares is a seam: once all its participants have voxels, one seam job reads their snapshots and
   meshes the boundary at both sides' LODs (`FVoxelSeamRegistry`). Neither waits on the other.
4. **Rendering.** The mesh lands on the game thread as `FChunkMeshData`; the renderer uploads it to the render region's
   scene proxy, which crossfades it with the mesh it replaces (a dithered swap). Water tiles per XY column are a second
   pass with the water material.
5. **Collision.** The collision manager cooks a Chaos trimesh near the player on the thread pool, reading neighbours
   through boundary shells; a neighbour arriving later re-cooks a chunk that read it as missing.
6. **Scatter.** Once inside the scatter horizon a chunk's surface points are extracted from its voxels on a worker,
   placed per biome and drawn as HISM instances per scatter region.
7. **Far compression.** A settled chunk far enough out (its band's stride at least `voxel.FarCompression.MinStride`)
   is encoded on a worker and its array dropped; readers get snapshots decoded off the game thread.
8. **Unload.** A chunk past the unload horizon leaves every system, scatter caches included.

### Edits

`UVoxelEditManager` keeps a sparse overlay of edits per chunk (Add, Subtract, Paint, Set), with undo and redo for
player edits and per-voxel records. An edit operation broadcasts each changed chunk with its bounds; the chunk manager
re-meshes those chunks, restales their seams, re-cooks the collision of the chunks whose cooks read them, re-extracts
the scatter readers, and water propagation floods what the edit opened under the sea. System edits (POI terraforming,
dungeon stamps, floods) stay out of the undo history.

### Reading voxels

Readers off the game thread never touch a chunk's array directly: they take an immutable edit-merged snapshot
(`UVoxelChunkManager::GetVoxelSnapshot`), shared copy-on-write with the resident array, collapsed for a uniform chunk, or
decoded by a worker from the compact form. Collision cooks read neighbours as boundary shells. Point reads on the game
thread go through the chunk manager: `GetVoxelAt` (the voxel with its edits) and `GetGeneratedVoxelAt` (as generation
made it).

### Surfaces

One ladder of surfaces serves every consumer (`UVoxelChunkManager::QuerySurfacePoint`, `EVoxelSurfaceLayer`): the
natural surface of the world mode, the generated surface (conditioning lines and zones applied; block tops in a cubic world), and
the edit-merged surface of the loaded voxels. Every query runs along local up, so the same call works on a planet.

### Beyond the voxel window

`FVoxelFarField` draws the conditioned analytic surface past the window: six cube-face quadtrees on a planet, root-tile
quadtrees on the XY grid on a heightmap, patches split by screen-space error and built on the thread pool, one
patch component each (`UVoxelFarFieldPatchComponent`). Band patches hide once every chunk they cover is drawn, so the window and the far field
neither gap nor overlap. An island keeps its far ocean (open sea and seabed past its edge); a small planet without a far
field sea draws a sea sphere.

## Core data

**`FVoxelData`** is 4 bytes: `MaterialID`, `Density` (0-255, 127 the surface), `BiomeID`, `Metadata` (low nibble AO,
high nibble flags: water, cave, underground). A 64³ chunk, the demo's size, is 1 MB resident.

**`FChunkDescriptor`** holds a chunk's coordinate, LOD level, size and voxels in one of four residencies
(`EVoxelDataResidency`: empty, resident, uniform, compressed), the resident array shared copy-on-write
(`GetVoxelDataForRead`, `GetVoxelDataMutable`), and a content version that counts generation and edits.

**`UVoxelWorldConfiguration`** is the world's asset: world mode and its parameters, meshing mode, voxel and chunk size,
view distance and LOD bands, generation, materials, water, scatter, caves and the far field. `UVoxelWorldsSettings`
holds project-wide tuning; a `voxel.*` cvar overrides either. `voxel.World.ConfigOverride` (or `-VoxelConfig=`) swaps
a level's configuration for a test run.

[DATA_STRUCTURES.md](Features/DATA_STRUCTURES.md) has the full types.

## Design decisions

**GPU generation, CPU meshing.** Generation is one thread per voxel and costs the GPU a fraction of a millisecond per
chunk; the readback is one buffer. Meshing stays on CPU workers because a chunk mesh and the seams it meets must agree
exactly, which the GPU meshers never did; no world dispatched them and they were deleted (the cubic one in the world
modes review, Marching Cubes and Dual Contouring in code review CR1). The CPU generator stays as the GPU's twin, held to
it by the `GPUvsCPU*Parity` tests, and runs headless runs (`-VoxelForceCPU`).

**Seam ownership.** Each chunk boundary belongs to one seam job that reads both sides, instead of each chunk reading
its neighbours' faces. Seams are watertight across LOD levels by construction, a chunk never waits for a neighbour, and an
edit re-meshes only the chunks it changed. It replaced the June LOD-boundary welds and the Transvoxel plan
(`Research/Archive/SEAM_OWNERSHIP_ARCHITECTURE.md`).

**Distance bands behind an interface.** `IVoxelLODStrategy` keeps the strategy swappable; the distance band strategy
is the only one, since predictable rings with refine and coarsen hysteresis and a 2:1 balance have met every world mode,
the planet included. Cubic worlds run without LOD (`IsLODEnabled()` false): greedy meshing is their answer to distance.

**A 4-byte voxel.** 256 materials and biomes, 8-bit density and a metadata byte keep a chunk compact and cache-friendly;
far compression and uniform collapse do the rest (a demo window of about 1,100 chunks settles near 130 MB of voxel data).

**One renderer on `FLocalVertexFactory`.** A custom scene proxy per render region uploads each chunk, seam and water mesh
into UE's local vertex factory; the custom vertex factory research (`Research/Archive/`) and the
`ProceduralMeshComponent` fallback were dropped (the fallback in code review CR1). `DynamicMeshComponent` and
GeometryScript were not taken: a dynamic draw path without instancing is worse for runtime geometry.

**Precision by regions, not origin rebasing.** Every primitive sits at its render region's origin (16³ chunks, double)
and stores float offsets from it, in every world mode; generation keeps size-R values in double or in the chunk's frame
and the noise is split-origin. Heightmap worlds are exact to the range of a double position and planets to about
1,000 km, with no world-origin shift ([TRAVERSAL_LIMITS.md](Research/TRAVERSAL_LIMITS.md)).

**No Nanite.** Nanite geometry is built at cook time; terrain generated at runtime from a seed cannot be Nanite. The
hand-built renderer and LOD are the engine's best option for runtime geometry, not a stand-in for Nanite.

**Runtime Virtual Textures.** The scene proxy emits RVT mesh batches when the configuration lists
`RuntimeVirtualTextures` (copied onto each region component before it registers), and `M_VoxelMaster` has a Runtime
Virtual Texture Output node; with none assigned it costs nothing.

**Runtime PCG** decorates the surface through `VoxelPCG`: a sampler that emits biome-tagged points from edit-merged
voxels near loaded chunks and the generator farther out, and a dispatcher that runs one graph per biome. It coexists with
the bespoke scatter ([PCG_DECORATION_ARCHITECTURE.md](Research/PCG_DECORATION_ARCHITECTURE.md)).

**World Partition for actors only.** World Partition streams placed actors (POIs, props, gameplay) in a finite world; the
terrain is one always-loaded actor whose chunk manager streams on its own. The two were verified side by side in PIE.

**Mode-generic code.** A feature built for one world mode moves to all of them once it applies (the far field, the
cut-over, regions, the noise), and its name carries no mode (`FVoxelFarField`, `FVoxelNoise`, `VoxelCutoverAltitude`).

## References

- [Features](README.md#features): the system docs, one per system.
- [STREAMING_AND_SEAMS.md](Features/STREAMING_AND_SEAMS.md): the chunk manager, seam ownership, snapshots, collision and residency.
- [WORLD_MODES.md](Features/WORLD_MODES.md), [LOD_SYSTEM.md](Features/LOD_SYSTEM.md),
  [RENDERING_SYSTEM.md](Features/RENDERING_SYSTEM.md), [GPU_PIPELINE.md](Features/GPU_PIPELINE.md).
- [Guides/TESTING_STRATEGY.md](Guides/TESTING_STRATEGY.md), [Guides/CODING_STANDARDS.md](Guides/CODING_STANDARDS.md).
- `Documentation/GLOSSARY.md` and `Documentation/EDITOR_CONSOLE_GUIDE.md` at the project root.
