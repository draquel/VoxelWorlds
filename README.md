# VoxelWorlds — GPU-Driven Voxel Terrain System

A high-performance voxel terrain engine for **Unreal Engine 5.8**, featuring three meshing
algorithms (cubic, Marching Cubes, Dual Contouring), watertight chunk and LOD seams through
single-owner seam jobs, distance-based streaming, and a full procedural generation stack (biomes, caves,
water, ore, scatter).

## Key Features

- **Three meshing algorithms** — Cubic (Minecraft-style, greedy), **Marching Cubes** (smooth), and
  **Dual Contouring** (smooth, sharp-feature-preserving via QEF), behind the `IVoxelMesher` interface,
  all on CPU workers (the GPU meshers were never dispatched by a world and are deleted: the GPU cubic
  mesher in world modes review WM-C4, the GPU Marching Cubes and Dual Contouring meshers in code review CR1).
- **Seam ownership** — every mode meshes chunk interiors only and gives each chunk boundary to one seam
  job that reads both sides' voxels, so a chunk never waits for, or re-meshes on, a neighbour
  (`Documentation/Research/SEAM_OWNERSHIP_ARCHITECTURE.md`; cubic since world modes review M5).
- **Watertight LOD seams** — MC seams handled by **geomorph** (bending the fine surface toward the
  coarse contour at the boundary); DC LOD seams and 4-chunk corner seams sealed.
- **Pluggable LOD** — swappable strategies via `IVoxelLODStrategy` (distance-band default) with
  asymmetric refine/coarsen hysteresis; LOD flips crossfade (a dithered mesh swap). Cubic meshing
  runs without LOD: greedy meshing is its answer to distance.
- **Streaming** — distance-based chunk load/unload with async mesh generation, stale-cull, thrash
  instrumentation, async Chaos collision cooking, and far-chunk voxel-data compression (keyed on
  distance, so it also covers LOD-off and cubic worlds).
- **Rendering** — one renderer built on UE's `FLocalVertexFactory` (`FVoxelCustomVFRenderer`: a world
  component per render region, its scene proxy drawing chunk, seam and water meshes).
- **Procedural generation** — CPU/GPU noise library, multiple world modes (infinite plane, spherical
  planet, island/bowl), biomes, caves, ore veins, water, and a foliage/scatter system with voxel
  tree injection.
- **Editing** — sparse edit-layer overlay with Add/Subtract/Paint brushes, discrete block editing,
  undo/redo, and binary persistence.
- **Materials** — material registry + atlas (Texture2DArrays for smooth triplanar, packed atlas for
  cubic) driven by a unified `M_VoxelMaster` material.
- **Map** — map/minimap subsystem (`VoxelMap`).

## Documentation

Start with **[Documentation/ARCHITECTURE.md](Documentation/ARCHITECTURE.md)**, then browse the
**[Documentation index](Documentation/README.md)**, which is organized into:

- **Features/** — how each shipping system works (meshing, rendering, LOD, generation, materials, edit, map)
- **Guides/** — [Quick Start](Documentation/Guides/QUICK_START.md),
  [Coding Standards](Documentation/Guides/CODING_STANDARDS.md),
  [Testing Strategy](Documentation/Guides/TESTING_STRATEGY.md),
  [Master Material Setup](Documentation/Guides/MASTER_MATERIAL_SETUP.md)
- **Research/** — investigations, implementation plans, and performance studies
  ([roadmap](Documentation/Research/IMPLEMENTATION_PHASES.md),
  [streaming perf](Documentation/Research/STREAMING_PERFORMANCE.md),
  [LOD seam investigation](Documentation/Research/LOD_SEAM_INVESTIGATION.md))

## Project Structure

```
VoxelWorlds/
├── Source/
│   ├── VoxelCore/         # Data structures, configs, material/biome registries, edit manager
│   ├── VoxelGeneration/   # CPU/GPU noise, world modes, tree injection
│   ├── VoxelMeshing/      # Cubic / Marching Cubes / Dual Contouring meshers (CPU); water mesher
│   ├── VoxelLOD/          # Pluggable LOD strategy (distance band)
│   ├── VoxelRendering/    # Scene proxy, FLocalVertexFactory renderer, world component
│   ├── VoxelStreaming/    # Chunk manager, collision manager, water propagation, benchmark, test actor
│   ├── VoxelScatter/      # Foliage/scatter placement + HISM rendering
│   └── VoxelMap/          # Map/minimap subsystem
├── Shaders/               # HLSL/USF compute shaders (.usf/.ush)
├── Content/               # Default assets, materials, configurations
└── Documentation/         # See Documentation/README.md
```

## Status

Core engine is implemented and in active development. Meshing (cubic CPU, MC/DC CPU+GPU), LOD with
watertight seams, streaming, generation (biomes/caves/water/ore/scatter), editing, materials, and the
map subsystem are functional. See
[Documentation/Research/IMPLEMENTATION_PHASES.md](Documentation/Research/IMPLEMENTATION_PHASES.md)
for the phase-by-phase roadmap.

## Testing

Automation tests live in each module's `Tests/` folder: noise, biome, world-mode, cubic and MC
meshing, the Dual-Contouring / Marching-Cubes **seam** suites (closure and ring bit-match), the cubic
seam-closure and streaming tests, and GPU/CPU parity tests that exercise the real-RHI path. Run them via the editor's
**Session Frontend → Automation** tab (filter `VoxelWorlds`) or headless with
`-ExecCmds="Automation RunTests VoxelWorlds; Quit"`. GPU/real-RHI boundary tests self-skip under
`-nullrhi`. See [Documentation/Guides/TESTING_STRATEGY.md](Documentation/Guides/TESTING_STRATEGY.md).

## Quick Reference

### Key Interfaces
- `IVoxelLODStrategy` — LOD strategy abstraction
- `IVoxelMeshRenderer` — renderer abstraction
- `IVoxelWorldMode` — world mode abstraction
- `IVoxelNoiseGenerator` — noise generation abstraction
- `IVoxelMesher` — mesh generation abstraction

### Key Classes
- `UVoxelChunkManager` — chunk streaming coordinator
- `UVoxelCollisionManager` — distance-based collision with async cooking
- `FDistanceBandLODStrategy` — default LOD implementation
- `FInfinitePlaneWorldMode` / `FIslandBowlWorldMode` / `FSphericalPlanetWorldMode` — world modes
- `FVoxelCPUNoiseGenerator` / `FVoxelGPUNoiseGenerator` — noise generation
- `FVoxelCPUCubicMesher` — cubic meshing (CPU, greedy or simple), interior domain plus face seams
- `FVoxelCPUMarchingCubesMesher` — Marching Cubes meshing
- `FVoxelCPUDualContourMesher` — Dual Contouring meshing
- `FVoxelWaterMesher` — water surface meshing
- `FVoxelCustomVFRenderer` — the renderer (UE `FLocalVertexFactory`)
- `FVoxelSceneProxy` / `UVoxelWorldComponent` — scene proxy + primitive component bridge
- `FVoxelMaterialRegistry` / `UVoxelMaterialAtlas` — material definitions and texture atlas
- `FVoxelBiomeRegistry` — biome definitions registry
- `UVoxelEditManager` — edit layer storage and undo/redo
- `UVoxelScatterManager` / `UVoxelScatterRenderer` — scatter placement + HISM rendering
- `FVoxelTreeInjector` — deterministic cross-chunk voxel tree injection
- `UVoxelMapSubsystem` — map/minimap subsystem
- `AVoxelWorldTestActor` — test actor for runtime world instantiation
- `UVoxelWorldConfiguration` — world configuration asset

### Key Data Structures
- `FVoxelData` — per-voxel data (MaterialID, Density, BiomeID, Metadata)
- `FVoxelVertex` — GPU vertex format
- `FChunkDescriptor` / `FChunkMeshData` — chunk metadata, the chunk's mesh
- `FVoxelMeshingRequest` — mesh generation request with face + deep-neighbor data for seamless LOD boundaries
- `FVoxelNoiseParams` — noise generation parameters
- `FWorldModeTerrainParams` — terrain configuration (SeaLevel, HeightScale, etc.)
- `FVoxelEdit` / `FChunkEditLayer` / `FVoxelBrushParams` — editing types
- `FScatterDefinition` / `FScatterSpawnPoint` / `FChunkScatterData` — scatter types
- `FVoxelTreeTemplate` — tree template (trunk/canopy params, placement rules)

## Console Commands (selection)

- `voxel.RemeshAll` — re-mesh all loaded chunks
- `voxel.Bench.Run` — streaming performance benchmark
- `voxel.VertexColorDebugMode 0|1|2` — debug vertex AO / material / biome encoding

---

**Engine**: Unreal Engine 5.8 · **Language**: C++ · **Rendering**: FLocalVertexFactory + compute shaders
