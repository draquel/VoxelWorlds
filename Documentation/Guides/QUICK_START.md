# Quick Start Guide

> State as of 2026-11-02 (code review CR5).

Build, run, and exercise the VoxelWorlds plugin. This guide assumes the plugin is already present in
a project (it ships as a git submodule under `Plugins/VoxelWorlds`); it is **not** a from-scratch
authoring guide. For internals see [ARCHITECTURE.md](../ARCHITECTURE.md) and the
[documentation index](../README.md).

## Prerequisites

- **Unreal Engine 5.8**
- Visual Studio 2022 or JetBrains Rider (C++ toolchain)
- The plugin enabled in your `.uproject` (`"Name": "VoxelWorlds", "Enabled": true`), with the PCG and
  ProceduralMeshComponent plugins it lists

## Build

1. Right-click the `.uproject` → **Generate Visual Studio project files** (or use Rider's UE support).
2. Build the editor target (Rider/VS: build the `Editor` configuration), or from a shell:
   ```
   <UE_5.8>/Engine/Build/BatchFiles/Build.bat <Project>Editor Win64 Development -project="<path>/<Project>.uproject"
   ```
3. Open the project in the editor.

## Module layout

VoxelWorlds is split into ten runtime modules and one editor module (see `Source/`):

| Module | Responsibility |
|--------|----------------|
| `VoxelCore` | Data structures, coordinates, the world configuration, material/biome registries, the edit manager |
| `VoxelGeneration` | CPU/GPU generation, the noise, world modes, surface queries, tree injection |
| `VoxelLOD` | The LOD strategy (distance bands): the voxel window, horizons, the cut-over |
| `VoxelMeshing` | Cubic, Marching Cubes and Dual Contouring meshers (CPU); the water mesher |
| `VoxelRendering` | The `FLocalVertexFactory` renderer: region components, the scene proxy, crossfades |
| `VoxelStreaming` | The chunk manager, collision, water, the far field, the world actor, the benchmark, the console commands |
| `VoxelScatter` | Foliage/scatter extraction, placement and HISM rendering |
| `VoxelMap` | Map/minimap subsystem |
| `VoxelNavigation` | Surface pathfinding without a navmesh |
| `VoxelPCG` | Runtime PCG: the voxel surface sampler and biome dispatcher |
| `VoxelWorldsEditor` | Editor only: the field preview panel and actor |

## See terrain in the editor

The fastest path is the bundled world actor:

1. Place an **`AVoxelWorldActor`** in a level.
2. Assign a **`UVoxelWorldConfiguration`** asset (world mode, voxel/chunk size, meshing mode), the voxel
   material and the material atlas.
3. Press Play (PIE). The actor drives the full pipeline: generation → meshing → rendering, with
   chunk streaming, collision and the far field.

The meshing mode is `EMeshingMode` on the configuration (Cubic / MarchingCubes / DualContouring), the world
mode `EWorldMode` (InfinitePlane / IslandBowl / SphericalPlanet). The demo level is
`/Game/PluginTesting/Demo/VoxelDemo`; `PlanetTest` is a small planet.

## Useful console commands & cvars

- `voxel.Mesh.RemeshAll`: re-mesh all loaded chunks (after changing a meshing option).
- `voxel.World.ConfigOverride <ObjectPath>`: run a different configuration in the level's world actor (set before Play).
- `voxel.World.Stats`, `voxel.FarCompression.Stats`, `voxel.Snapshot.Stats`: what the world holds.
- `voxel.Bench.Run`: the streaming benchmark (see
  [Research/STREAMING_PERFORMANCE.md](../Research/STREAMING_PERFORMANCE.md)).

Every command and cvar is in the project's `Documentation/EDITOR_CONSOLE_GUIDE.md`.

## Run the automation tests

Correctness tests live in each module's `Tests/` folder (`VoxelWorlds.*`: noise, biomes, world modes, the
meshers and their seam suites, streaming, scatter, water, the far field, GPU/CPU parity). Run them from the
editor's **Session Frontend → Automation** tab (filter on `VoxelWorlds`), or headless:

```
<UE_5.8>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe <Project>.uproject ^
  -ExecCmds="Automation RunTests VoxelWorlds; Quit" -unattended -nullrhi
```

Headless runs skip, not fail, the tests that need a real RHI (the GPU/CPU parity tests and the renderer tests
that need a scene proxy); run those in the editor (or through the `unreal-mcp` automation toolset) after any
change to generation, meshing or rendering. See [TESTING_STRATEGY.md](TESTING_STRATEGY.md).

## Where to go next

- [ARCHITECTURE.md](../ARCHITECTURE.md): system overview and data flow
- [CODING_STANDARDS.md](CODING_STANDARDS.md): project conventions
- [Documentation index](../README.md): the full Features / Guides / Research map
