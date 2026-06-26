# VoxelWorlds — GPU-Driven Voxel Terrain System

A high-performance voxel terrain engine for **Unreal Engine 5.8**, featuring three meshing
algorithms (cubic, Marching Cubes, Dual Contouring) with CPU and GPU implementations, watertight
LOD-seam handling, distance-based streaming, and a full procedural generation stack (biomes, caves,
water, ore, scatter).

## Key Features

- **Three meshing algorithms** — Cubic (Minecraft-style), **Marching Cubes** (smooth), and
  **Dual Contouring** (smooth, sharp-feature-preserving via QEF). Each has a **CPU and a GPU compute**
  implementation behind the `IVoxelMesher` interface.
- **Watertight LOD seams** — MC seams handled by **geomorph** (bending the fine surface toward the
  coarse contour at the boundary); DC LOD seams and 4-chunk corner seams sealed. The GPU DC path has
  reached **CPU watertightness parity** (verified by the `GT1–GT7` LOD-boundary test suite).
- **Pluggable LOD** — swappable strategies via `IVoxelLODStrategy` (distance-band default) with
  asymmetric refine/coarsen hysteresis and material-parameter-collection LOD morphing.
- **Streaming** — distance-based chunk load/unload with async mesh generation, stale-cull, thrash
  instrumentation, and async Chaos collision cooking.
- **Hybrid rendering** — a GPU-driven renderer built on UE's `FLocalVertexFactory`
  (`FVoxelCustomVFRenderer`) plus a `ProceduralMeshComponent` fallback (`FVoxelPMCRenderer`).
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
│   ├── VoxelMeshing/      # Cubic / Marching Cubes / Dual Contouring meshers (CPU+GPU); water mesher
│   ├── VoxelLOD/          # Pluggable LOD strategy (distance band)
│   ├── VoxelRendering/    # Scene proxy, FLocalVertexFactory renderer + PMC fallback, world component
│   ├── VoxelStreaming/    # Chunk manager, collision manager, water propagation, benchmark, test actor
│   ├── VoxelScatter/      # Foliage/scatter placement + HISM rendering
│   └── VoxelMap/          # Map/minimap subsystem
├── Shaders/               # HLSL/USF compute shaders (.usf/.ush)
├── Content/               # Default assets, materials, configurations
└── Documentation/         # See Documentation/README.md
```

## Status

Core engine is implemented and in active development. Meshing (cubic/MC/DC, CPU+GPU), LOD with
watertight seams, streaming, generation (biomes/caves/water/ore/scatter), editing, materials, and the
map subsystem are functional. See
[Documentation/Research/IMPLEMENTATION_PHASES.md](Documentation/Research/IMPLEMENTATION_PHASES.md)
for the phase-by-phase roadmap.

## Testing

Automation tests live in each module's `Tests/` folder: noise, biome, world-mode, cubic and MC
meshing, and the Dual-Contouring / Marching-Cubes **LOD-boundary** suites — including the GPU
`GT0–GT7` mirror that exercises the real-RHI meshing path. Run them via the editor's
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
- `FVoxelCPUCubicMesher` / `FVoxelGPUCubicMesher` — cubic meshing (CPU greedy + GPU compute)
- `FVoxelCPUMarchingCubesMesher` / `FVoxelGPUMarchingCubesMesher` — Marching Cubes meshing
- `FVoxelCPUDualContourMesher` / `FVoxelGPUDualContourMesher` — Dual Contouring meshing
- `FVoxelWaterMesher` — water surface meshing
- `FVoxelCustomVFRenderer` — GPU-driven renderer (UE `FLocalVertexFactory`)
- `FVoxelPMCRenderer` — `ProceduralMeshComponent` fallback renderer
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
- `FChunkDescriptor` / `FChunkMeshData` / `FChunkRenderData` — chunk metadata, CPU mesh, GPU render data
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
