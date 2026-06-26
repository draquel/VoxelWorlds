# Quick Start Guide

Build, run, and exercise the VoxelWorlds plugin. This guide assumes the plugin is already present in
a project (it ships as a git submodule under `Plugins/VoxelWorlds`); it is **not** a from-scratch
authoring guide. For internals see [ARCHITECTURE.md](../ARCHITECTURE.md) and the
[Features docs](../README.md).

## Prerequisites

- **Unreal Engine 5.8**
- Visual Studio 2022 or JetBrains Rider (C++ toolchain)
- The plugin enabled in your `.uproject` (`"Name": "VoxelWorlds", "Enabled": true`)

## Build

1. Right-click the `.uproject` → **Generate Visual Studio project files** (or use Rider's UE support).
2. Build the editor target (Rider/VS: build the `Editor` configuration), or from a shell:
   ```
   <UE_5.8>/Engine/Build/BatchFiles/Build.bat <Project>Editor Win64 Development -project="<path>/<Project>.uproject"
   ```
3. Open the project in the editor.

## Module layout

VoxelWorlds is split into focused runtime modules (see `Source/`):

| Module | Responsibility |
|--------|----------------|
| `VoxelCore` | Data structures, coordinates, configs, material/biome registries, edit manager |
| `VoxelGeneration` | CPU/GPU noise, world modes, tree injection |
| `VoxelMeshing` | Cubic, Marching Cubes, Dual Contouring meshers (CPU + GPU); water mesher |
| `VoxelLOD` | Pluggable LOD strategy (distance-band) |
| `VoxelRendering` | Scene proxy, custom renderer (FLocalVertexFactory) + PMC fallback, world component |
| `VoxelStreaming` | Chunk manager, collision manager, water propagation, benchmark, test actor |
| `VoxelScatter` | Foliage/scatter placement and HISM rendering |
| `VoxelMap` | Map/minimap subsystem |

## See terrain in the editor

The fastest path is the bundled test actor:

1. Place an **`AVoxelWorldTestActor`** in a level.
2. Assign a **`UVoxelWorldConfiguration`** asset (world mode, voxel/chunk size, meshing mode).
3. Press Play (PIE). The actor drives the full pipeline: noise → world mode → meshing → rendering,
   with distance-based chunk streaming and async mesh generation.

Meshing mode is chosen via `EMeshingMode` on the configuration (Cubic / MarchingCubes / DualContouring).

## Useful console commands & cvars

- `voxel.RemeshAll` — re-mesh all loaded chunks (use after toggling meshing/LOD options).
- `voxel.Bench.Run` — run the streaming performance benchmark (see
  [Research/STREAMING_PERFORMANCE.md](../Research/STREAMING_PERFORMANCE.md)).
- `voxel.VertexColorDebugMode 0|1|2` — visualize vertex AO / material / biome encoding
  (chunks must be re-meshed to see changes).

## Run the automation tests

Correctness tests live in each module's `Tests/` folder (noise, biomes, cubic/MC meshing, and the
DC/MC **LOD-boundary** suites incl. the GPU `GT0–GT7` mirror). Run them from the editor's
**Session Frontend → Automation** tab (filter on `VoxelWorlds`), or headless:

```
<UE_5.8>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe <Project>.uproject ^
  -ExecCmds="Automation RunTests VoxelWorlds; Quit" -unattended -nullrhi
```

Note: GPU/real-RHI boundary tests self-skip under `-nullrhi`; run them through the editor (or the
`unreal-mcp` AutomationTest toolset) to exercise the GPU meshers. See
[TESTING_STRATEGY.md](TESTING_STRATEGY.md).

## Where to go next

- [ARCHITECTURE.md](../ARCHITECTURE.md) — system overview and data flow
- [CODING_STANDARDS.md](CODING_STANDARDS.md) — project conventions
- [Documentation index](../README.md) — full Features / Guides / Research map
