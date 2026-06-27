# VoxelWorlds Documentation Index

Documentation for the VoxelWorlds plugin (Unreal Engine 5.8), organized by purpose:

- **[ARCHITECTURE.md](ARCHITECTURE.md)** — start here. System overview, module organization, data
  flow, and design decisions for the whole engine.
- **Features/** — durable reference for how each shipping system works (architecture, API, data
  structures, usage).
- **Guides/** — conventions and how-to guides (durable, process-light).
- **Research/** — investigations, root-cause analyses, implementation plans, and performance studies.
  Historical/process records kept for context; not day-to-day reference.
- **Research/Archive/** — superseded documents describing approaches that were replaced. Kept for
  history; do not treat as current.

---

## Features (system reference)

### Meshing
- [DUAL_CONTOURING.md](Features/DUAL_CONTOURING.md) — DC mesher (CPU + GPU), QEF solver, LOD boundary merging
- [MARCHING_CUBES_MESHING.md](Features/MARCHING_CUBES_MESHING.md) — MC mesher, LOD stride, geomorph seam handling

### Rendering & GPU
- [RENDERING_SYSTEM.md](Features/RENDERING_SYSTEM.md) — hybrid renderer (FLocalVertexFactory + PMC fallback), scene proxy, water, collision
- [GPU_PIPELINE.md](Features/GPU_PIPELINE.md) — RDG compute pipeline for generation and meshing
- [DATA_STRUCTURES.md](Features/DATA_STRUCTURES.md) — core types and memory layouts
- [LOD_SYSTEM.md](Features/LOD_SYSTEM.md) — pluggable LOD strategy, distance bands, morphing

### Generation & world
- [NOISE_LIBRARY.md](Features/NOISE_LIBRARY.md) — CPU/GPU noise generators
- [WORLD_MODES.md](Features/WORLD_MODES.md) — infinite plane, spherical planet, island/bowl
- [BIOME_SYSTEM.md](Features/BIOME_SYSTEM.md) — biome definitions and selection
- [CAVE_SYSTEM.md](Features/CAVE_SYSTEM.md) — cave carving
- [WATER_SYSTEM.md](Features/WATER_SYSTEM.md) — water meshing and propagation
- [SCATTER_SYSTEM.md](Features/SCATTER_SYSTEM.md) — foliage/scatter + voxel tree injection
- [ORE_VEINS.md](Features/ORE_VEINS.md) — ore vein placement
- [UNDERGROUND_CLASSIFICATION.md](Features/UNDERGROUND_CLASSIFICATION.md) — underground/surface classification

### Materials, editing, map
- [MATERIAL_SYSTEM.md](Features/MATERIAL_SYSTEM.md) — material registry and atlas
- [EDIT_LAYER.md](Features/EDIT_LAYER.md) — terrain editing overlay, brushes, undo/redo
- [MAP_SYSTEM.md](Features/MAP_SYSTEM.md) — map/minimap subsystem

## Guides
- [QUICK_START.md](Guides/QUICK_START.md) — build and run the plugin
- [CODING_STANDARDS.md](Guides/CODING_STANDARDS.md) — project conventions
- [TESTING_STRATEGY.md](Guides/TESTING_STRATEGY.md) — automation test approach
- [MASTER_MATERIAL_SETUP.md](Guides/MASTER_MATERIAL_SETUP.md) — M_VoxelMaster setup walkthrough

## Research (historical / process)
- [ENGINE_INTEGRATION.md](Research/ENGINE_INTEGRATION.md) — which UE 5.8 systems to adopt (RVT, runtime PCG, World Partition) vs. keep hand-built; why Nanite is build-time only; RVT spike plan; Mesh Terrain as secondary bake bridge
- [PCG_DECORATION_ARCHITECTURE.md](Research/PCG_DECORATION_ARCHITECTURE.md) — PCG runtime decoration design: biome-keyed per-biome subgraphs, edit-aware hybrid sampler, POI/construction priority stack, standalone Claims/World-Annotation system; revised phase plan
- [IMPLEMENTATION_PHASES.md](Research/IMPLEMENTATION_PHASES.md) — development roadmap / phase log
- [LOD_SEAM_INVESTIGATION.md](Research/LOD_SEAM_INVESTIGATION.md) — LOD seam root-cause investigation
- [GEOMORPH_IMPLEMENTATION_PLAN.md](Research/GEOMORPH_IMPLEMENTATION_PLAN.md) — geomorph seam plan (✅ implemented)
- [TRANSVOXEL_IMPLEMENTATION_PLAN.md](Research/TRANSVOXEL_IMPLEMENTATION_PLAN.md) — Transvoxel plan (⏸️ not pursued)
- [PERFORMANCE_TARGETS.md](Research/PERFORMANCE_TARGETS.md) — memory/compute budgets
- [STREAMING_PERFORMANCE.md](Research/STREAMING_PERFORMANCE.md) — streaming benchmark harness & tuning

### Archive (superseded)
- [00_START_HERE.md](Research/Archive/00_START_HERE.md) — original pre-implementation package doc
- [VoxelVertexFactoryResearch.md](Research/Archive/VoxelVertexFactoryResearch.md) — custom VF research (abandoned for FLocalVertexFactory)
- [VoxelVertexFactoryImplementationPlan.md](Research/Archive/VoxelVertexFactoryImplementationPlan.md) — custom VF plan (abandoned)

---

## Known documentation gaps (future work)
- **Cubic meshing** (`VoxelCPUCubicMesher` / `VoxelGPUCubicMesher`) has no dedicated feature doc;
  it is covered only in ARCHITECTURE and the top-level README.
- **Streaming as a feature** (`VoxelChunkManager`, `VoxelCollisionManager`) is documented only from a
  performance angle in `Research/STREAMING_PERFORMANCE.md`.
