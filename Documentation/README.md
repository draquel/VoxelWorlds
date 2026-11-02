# VoxelWorlds Documentation Index

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

Every document under `Documentation/`. Start with [ARCHITECTURE.md](ARCHITECTURE.md), then the Features doc of the
system you work on. The project root holds the vocabulary (`Documentation/GLOSSARY.md`), every console command and cvar
(`Documentation/EDITOR_CONSOLE_GUIDE.md`) and the open findings (`Documentation/FINDINGS.md`).

- **Features** describe each system as the code is: the current truth.
- **Guides** are how-tos and conventions.
- **Research** holds open design work and the measurement references.
- **Frozen** plans are history, each section true on its date; their results are in the Features docs.
- **Research/Archive** holds finished or superseded plans, each with a status line naming where its truth went.

## Overview

| Document | Contents |
|----------|----------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | The modules, the runtime pieces, a chunk's life, edits, voxel reads, surfaces, the far field; the design decisions (GPU generation and CPU meshing, seam ownership, regions, no Nanite, RVT, runtime PCG, World Partition for actors only) |

## Features

| Document | Contents |
|----------|----------|
| [STREAMING_AND_SEAMS.md](Features/STREAMING_AND_SEAMS.md) | The chunk manager's pipeline: seam ownership, voxel snapshots and shells, collision cooking, far compression, residency and its audit |
| [LOD_SYSTEM.md](Features/LOD_SYSTEM.md) | `IVoxelLODStrategy` and the distance bands: the voxel window per column or chunk, hysteresis and 2:1 balance, horizons, the cut-over, LOD off for cubic |
| [WORLD_MODES.md](Features/WORLD_MODES.md) | Infinite plane, island and spherical planet: the interface, each mode's generation, edge, streaming and scale, block worlds, mode parity |
| [DATA_STRUCTURES.md](Features/DATA_STRUCTURES.md) | The core value types and the version fingerprints that protect saved data |
| [GPU_PIPELINE.md](Features/GPU_PIPELINE.md) | Generation on the GPU: the density pass, the post-passes, the readback, CPU parity |
| [NOISE_LIBRARY.md](Features/NOISE_LIBRARY.md) | `FVoxelNoise` and `NoiseFBM3D`: the hashed, split-origin noise every field of every mode uses |
| [BIOME_SYSTEM.md](Features/BIOME_SYSTEM.md) | Biome selection by temperature, moisture and continentalness, materials by depth, dithered blend zones |
| [CAVE_SYSTEM.md](Features/CAVE_SYSTEM.md) | Cave layers (cheese, spaghetti, noodle) carved after the terrain, per biome |
| [ORE_VEINS.md](Features/ORE_VEINS.md) | Ore vein placement, global and per biome |
| [UNDERGROUND_CLASSIFICATION.md](Features/UNDERGROUND_CLASSIFICATION.md) | Which voxels and scatter points are underground, and the scatter surface mask |
| [WATER_SYSTEM.md](Features/WATER_SYSTEM.md) | Water flags, the fill pass, the cross-chunk pull, edit-driven floods, water tiles and their mesher, the seas |
| [DUAL_CONTOURING.md](Features/DUAL_CONTOURING.md) | The Dual Contouring mesher: QEF, fitted normals, LOD crossings, seams |
| [MARCHING_CUBES_MESHING.md](Features/MARCHING_CUBES_MESHING.md) | The Marching Cubes mesher: LOD stride, geomorph, the transition ribbon, seams |
| [RENDERING_SYSTEM.md](Features/RENDERING_SYSTEM.md) | `FVoxelMeshRenderer`, render regions, the scene proxy, crossfades, water tiles |
| [MATERIAL_SYSTEM.md](Features/MATERIAL_SYSTEM.md) | The material registry and atlas, saved texture arrays, the master material, hardness, the far field's material blend |
| [EDIT_LAYER.md](Features/EDIT_LAYER.md) | Edits, brushes, undo and redo, system edits outside the history, persistence |
| [SCATTER_SYSTEM.md](Features/SCATTER_SYSTEM.md) | Scatter extraction from the voxels, placement, the scatter horizon, HISM regions, voxel trees, edit readers |
| [MAP_SYSTEM.md](Features/MAP_SYSTEM.md) | Map and minimap tiles, the tile store's budgets, the globe, the edit overlay |
| [SURFACE_NAVIGATION.md](Features/SURFACE_NAVIGATION.md) | The `VoxelNavigation` module: A* over the surface without a navmesh, the tile cache, frames, block steps |
| [EDITOR_TOOLS.md](Features/EDITOR_TOOLS.md) | The `VoxelWorldsEditor` module: the field preview panel and actor |

## Guides

| Document | Contents |
|----------|----------|
| [QUICK_START.md](Guides/QUICK_START.md) | Build the plugin, place a world, run the tests |
| [TESTING_STRATEGY.md](Guides/TESTING_STRATEGY.md) | The test suites, headless and in-editor runs, the gate for meshing, generation and rendering |
| [CODING_STANDARDS.md](Guides/CODING_STANDARDS.md) | Conventions: Blueprint types, deprecated engine APIs, naming |
| [MASTER_MATERIAL_SETUP.md](Guides/MASTER_MATERIAL_SETUP.md) | Building `M_VoxelMaster` and its material functions |
| [PCG_BIOME_DECORATION_SETUP.md](Guides/PCG_BIOME_DECORATION_SETUP.md) | Setting up biome-keyed runtime PCG decoration (sampler, dispatcher, per-biome graphs) |

## Research

| Document | Contents |
|----------|----------|
| [STREAMING_PERFORMANCE.md](Research/STREAMING_PERFORMANCE.md) | The streaming bench (`voxel.Bench.Run`), its report, the bench gate scripts, the levers measured |
| [PERFORMANCE_TARGETS.md](Research/PERFORMANCE_TARGETS.md) | The demo's measured baseline and thresholds; the February design targets as history |
| [TRAVERSAL_LIMITS.md](Research/TRAVERSAL_LIMITS.md) | How far a player can travel and how big a world can be: each limit and its state |
| [PCG_DECORATION_ARCHITECTURE.md](Research/PCG_DECORATION_ARCHITECTURE.md) | Open: the runtime PCG design; phases 3-6 built, 7 (reactivity) and 8 (coexistence) open |
| [DC_MATERIAL_BLEND_PLAN.md](Research/DC_MATERIAL_BLEND_PLAN.md) | Open: soft material borders on Dual Contouring meshes, scoped and deferred |
| `Research/Bench/` | JSON bench reports cited by the world modes review (D7, G6, KI12, M1, M4-M6, reviews A and B, SP1-SP2, the vertical window) |
| [Research/Archive/](Research/Archive/) | Finished or superseded: the build phases, the start-here package, the edit layer's February spec, engine integration, geomorph, the LOD seam investigation, the map accuracy plan, the scatter review and claim suppression, seam ownership, Transvoxel, the voxel QA follow-ups (R22-R24), the custom vertex factory research and plan |

## Frozen plans

| Document | Contents |
|----------|----------|
| [WORLD_MODES_REVIEW_PLAN.md](WORLD_MODES_REVIEW_PLAN.md) | Frozen 2026-11-02. The cubic, island and planet reviews, spherical Phases 1-6, mode parity, G1-G86, D1-D41 |
| [VISUAL_POLISH_PLAN.md](VISUAL_POLISH_PLAN.md) | Frozen 2026-11-02. VP1-VP5: the sky, terrain materials, shadows, flicker, LOD ribs |
| [SPHERICAL_PLANET_REQUIREMENTS.md](SPHERICAL_PLANET_REQUIREMENTS.md) | Frozen 2026-11-02. The planet's requirements, phases and Z-up audit |

## Other

- `Images/R22`, `Images/R23`, `Images/R24`: before and after shots for the voxel QA follow-ups.
- `Presentation/`: two HTML pages (an architecture explainer and slides) from 2026-07, before the world modes review.
