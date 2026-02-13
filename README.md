# VoxelWorlds - GPU-Driven Voxel Terrain System

A high-performance, GPU-driven voxel terrain system for Unreal Engine 5.7 featuring pluggable LOD strategies, hybrid rendering architecture, and support for multiple world modes.

## Key Features

- **GPU-First Architecture**: Voxel generation and meshing executed entirely on GPU via compute shaders
- **Pluggable LOD System**: Swappable LOD strategies via clean interface abstraction
- **Hybrid Rendering**: Custom Vertex Factory for runtime performance, PMC fallback for editor/tools
- **Multiple World Modes**: Infinite plane, spherical planet, and island/bowl configurations
- **Dual Meshing Modes**: Cubic (Minecraft-style) and smooth (Marching Cubes) terrain
- **Advanced Features**: Biome system, material atlas, edit layers, foliage scatter

## Architecture Documentation

### Core Documentation
- **[Architecture Overview](Documentation/ARCHITECTURE.md)** - Complete system design and component interaction
- **[Quick Start Guide](Documentation/QUICK_START.md)** - Getting started with development
- **[Coding Standards](Documentation/CODING_STANDARDS.md)** - Project conventions and best practices

### System Deep Dives
- **[LOD System](Documentation/LOD_SYSTEM.md)** - Pluggable LOD architecture and implementations
- **[Rendering System](Documentation/RENDERING_SYSTEM.md)** - Hybrid rendering approach and vertex factory
- **[Data Structures](Documentation/DATA_STRUCTURES.md)** - Core types and memory layouts
- **[GPU Pipeline](Documentation/GPU_PIPELINE.md)** - Compute shader workflow and integration
- **[World Modes](Documentation/WORLD_MODES.md)** - Infinite, spherical, and island world types
- **[Material System](Documentation/MATERIAL_SYSTEM.md)** - Material registry and atlas configuration
- **[Master Material Setup](Documentation/MASTER_MATERIAL_SETUP.md)** - M_VoxelMaster material creation guide
- **[Biome System](Documentation/BIOME_SYSTEM.md)** - Biome definitions and blending
- **[Edit Layer](Documentation/EDIT_LAYER.md)** - Terrain editing and modification system
- **[Scatter System](Documentation/SCATTER_SYSTEM.md)** - Vegetation and foliage placement

### Development
- **[Implementation Phases](Documentation/IMPLEMENTATION_PHASES.md)** - Development roadmap and milestones
- **[Performance Targets](Documentation/PERFORMANCE_TARGETS.md)** - Memory and compute budgets
- **[Testing Strategy](Documentation/TESTING_STRATEGY.md)** - Unit and integration testing approach

## Project Structure

```
VoxelWorlds/
├── Source/
│   ├── VoxelCore/          # Data structures, edit types, math utilities
│   ├── VoxelLOD/           # Pluggable LOD system
│   ├── VoxelGeneration/    # Noise, biomes, world modes
│   ├── VoxelMeshing/       # Cubic and smooth meshing
│   ├── VoxelRendering/     # Hybrid rendering system
│   ├── VoxelStreaming/     # Chunk management, collision, test actors
│   ├── VoxelScatter/       # Foliage and scatter system
│   └── VoxelRuntime/       # UE integration and components (planned)
├── Shaders/                # HLSL compute shaders
├── Content/                # Assets, materials, configurations
└── Documentation/          # This documentation
```

## Current Status

### Completed
- ✅ **Phase 1: Foundation** - Core architecture and interfaces
  - Architecture design and documentation
  - Interface definitions (LOD, Rendering, World Modes)
  - Data structure specifications (FVoxelData, FChunkDescriptor)
  - VoxelCore, VoxelLOD, VoxelRendering, VoxelStreaming modules
  - Distance Band LOD strategy implementation
- ✅ **GPU Noise Library** (Phase 2)
  - CPU and GPU noise generators (Perlin, Simplex with FBM)
  - RDG-based compute shader integration for UE 5.7
  - Full test coverage (4 automation tests passing)
- ✅ **Infinite Plane World Mode** (Phase 2)
  - IVoxelWorldMode interface with SDF-based terrain generation
  - FInfinitePlaneWorldMode: 2D heightmap terrain extending infinitely in X/Y
  - GPU shader integration (WorldModeSDF.ush)
  - Depth-based material assignment (Grass/Dirt/Stone)
  - Full test coverage (6 automation tests passing)
- ✅ **Cubic Meshing System** (Phase 2)
  - IVoxelMesher interface with sync/async meshing support
  - FVoxelCPUCubicMesher: CPU-based face culling with neighbor support
  - FVoxelGPUCubicMesher: GPU compute shader meshing with atomic counters
  - CubicMeshGeneration.usf compute shader with RDG integration
  - Seamless chunk boundaries via neighbor data
  - Full test coverage (9 automation tests passing)
- ✅ **PMC Renderer** (Phase 2)
  - FVoxelPMCRenderer: ProceduralMeshComponent-based IVoxelMeshRenderer implementation
  - AVoxelPMCContainerActor: Transient actor for PMC component management
  - Component pooling for reduced allocations
  - CPU mesh data conversion (FVector3f/uint32 to FVector/int32)
  - Automatic tangent generation and collision mesh support
  - Dynamic vertex color material creation for biome visualization
- ✅ **Biome & Material System** (Phase 2)
  - FVoxelMaterialRegistry: Static registry with 7 material definitions (Grass, Dirt, Stone, Sand, Snow, Sandstone, Frozen Dirt)
  - FVoxelBiomeRegistry: Static registry with 3 biome definitions (Plains, Desert, Tundra)
  - Temperature/moisture noise sampling for biome selection
  - Depth-based material assignment per biome (surface, subsurface, deep)
  - Vertex color visualization with AO support
- ✅ **Chunk Streaming Pipeline** (Phase 2)
  - Full generation pipeline: Noise → World Mode → Meshing → Rendering
  - AVoxelWorldTestActor: Test actor for runtime voxel world instantiation
  - Distance-based chunk loading/unloading
  - Debug visualization and statistics

- ✅ **Phase 3: Advanced Meshing** - COMPLETE
  - ✅ Greedy meshing algorithm (40-60% triangle reduction, seamless chunk boundaries)
  - ✅ Per-vertex ambient occlusion (standard voxel AO with neighbor sampling)
  - ✅ FLocalVertexFactory renderer (GPU-driven, stable rendering)
  - ✅ LOD morphing via Material Parameter Collection

- ✅ **Phase 4: Smooth Meshing** - COMPLETE
  - ✅ Marching Cubes algorithm (CPU implementation with trilinear interpolation)
  - ✅ Gradient-based normal calculation for smooth shading
  - ✅ LOD stride support (2^LODLevel voxel stepping)
  - ✅ Skirt-based LOD seam hiding (configurable)
  - ✅ LOD configuration gates (bEnableLOD, bEnableLODSeams)
  - ⏸️ Transvoxel algorithm (implemented but disabled - complex edge cases)

- ✅ **Phase 5: World Modes** - COMPLETE
  - ✅ Material atlas system (Texture2DArrays for smooth, packed atlas for cubic)
  - ✅ Unified M_VoxelMaster material with automatic mode switching
  - ✅ Spherical planet mode with horizon/shell culling
  - ✅ Island/bowl mode with configurable falloff
  - ✅ Advanced biome blending and height-based materials
  - ✅ Water level support with per-biome underwater materials
  - ✅ Ore vein system (Coal, Iron, Gold with depth constraints)

- ✅ **Phase 6: Editing & Collision** - COMPLETE
  - ✅ Edit layer with overlay architecture (sparse storage, merged at mesh time)
  - ✅ Brush tools: Add, Subtract, Paint with Sphere/Cube/Cylinder shapes
  - ✅ Discrete editing mode for single-block operations (cubic terrain)
  - ✅ Collision manager with async Chaos physics cooking
  - ✅ Undo/redo system with operation batching
  - ✅ Binary serialization for edit persistence (v2 format)
  - ✅ Input-based testing (mouse + keyboard controls in VoxelWorldTestActor)
  - ✅ **Async mesh generation** (visual meshing on background threads)
  - ✅ Performance optimizations (neighbor cache, collision throttling)

- ✅ **Phase 7: Scatter & Polish** - COMPLETE
  - ✅ **Phase 7A**: Scatter placement foundation (VoxelScatter module, surface extraction, placement rules)
  - ✅ **Phase 7B**: HISM mesh rendering (per-type HISM components, instance management)
  - ✅ **Phase 7C**: Performance optimization (deferred rebuilds, batch instance addition, throttled generation)
  - ✅ **Edit integration**: Targeted scatter removal for player edits, full regeneration for system edits
  - ✅ **Phase 7D-1**: Async scatter generation (thread pool extraction + placement)
  - ✅ **Phase 7D-2**: GPU surface extraction (compute shader with occupancy grid dedup)
  - ✅ **Phase 7D-5**: Voxel-based surface extraction (LOD-independent scatter from voxel data)
  - ✅ **Phase 7E**: Cubic terrain scatter support
    - Block-face snapping for cubic terrain placement
    - Cross-billboard HISM scatter (grass, flowers) with atlas UV support
    - Voxel tree injection (deterministic, cross-chunk, editable terrain trees)
    - Tree placement rules (material, biome, slope, elevation, water level)
    - Tree mode system (VoxelData / HISM / Both with distance threshold)
    - Runtime billboard material (TwoSided, Masked, with fallback generation)

### In Progress
- 🔄 **Phase 8: Advanced Features** — See [Implementation Phases](Documentation/IMPLEMENTATION_PHASES.md)

See [Implementation Phases](Documentation/IMPLEMENTATION_PHASES.md) for detailed roadmap.

## Quick Reference

### Key Interfaces
- `IVoxelLODStrategy` - LOD strategy abstraction
- `IVoxelMeshRenderer` - Renderer abstraction
- `IVoxelWorldMode` - World mode abstraction
- `IVoxelNoiseGenerator` - Noise generation abstraction
- `IVoxelMesher` - Mesh generation abstraction

### Key Classes
- `UVoxelChunkManager` - Chunk streaming coordinator
- `FDistanceBandLODStrategy` - Default LOD implementation
- `FInfinitePlaneWorldMode` - 2D heightmap world mode
- `FIslandBowlWorldMode` - Bounded island with edge falloff
- `FSphericalPlanetWorldMode` - Radial terrain on spherical surface
- `FVoxelCPUNoiseGenerator` - CPU-based noise generation
- `FVoxelGPUNoiseGenerator` - GPU compute shader noise generation
- `FVoxelCPUCubicMesher` - CPU-based cubic mesh generation with greedy meshing
- `FVoxelGPUCubicMesher` - GPU compute shader cubic meshing
- `FVoxelCPUSmoothMesher` - CPU-based smooth meshing (Marching Cubes)
- `FVoxelPMCRenderer` - ProceduralMeshComponent-based renderer (CPU fallback)
- `FVoxelCustomVFRenderer` - GPU-driven renderer using FLocalVertexFactory
- `FVoxelSceneProxy` - Scene proxy for chunk rendering with frustum culling
- `UVoxelWorldComponent` - Primitive component bridge for rendering
- `FVoxelMaterialRegistry` - Static material definitions registry
- `FVoxelBiomeRegistry` - Static biome definitions registry
- `UVoxelEditManager` - Edit layer storage and undo/redo management
- `UVoxelCollisionManager` - Distance-based collision with async cooking
- `UVoxelScatterManager` - Scatter placement coordinator with surface extraction
- `UVoxelScatterRenderer` - HISM instance management with deferred rebuilds
- `FVoxelBillboardMeshGenerator` - Runtime cross-billboard mesh generation for cubic scatter
- `FVoxelTreeInjector` - Deterministic cross-chunk voxel tree injection
- `AVoxelWorldTestActor` - Test actor for runtime world instantiation
- `UVoxelWorldConfiguration` - World configuration asset

### Key Data Structures
- `FVoxelData` - Per-voxel data (4 bytes: MaterialID, Density, BiomeID, Metadata)
- `FVoxelVertex` - GPU vertex format (28 bytes)
- `FChunkDescriptor` - Chunk metadata
- `FChunkMeshData` - CPU mesh data
- `FChunkRenderData` - GPU render data
- `FLODQueryContext` - LOD query parameters
- `FVoxelNoiseParams` - Noise generation parameters
- `FVoxelMeshingRequest` - Mesh generation request with neighbor data
- `FWorldModeTerrainParams` - Terrain configuration (SeaLevel, HeightScale, etc.)
- `FVoxelMaterialDefinition` - Material definition (ID, Name, Color)
- `FBiomeDefinition` - Biome definition (climate ranges, material assignments)
- `FVoxelEdit` - Single voxel edit with DensityDelta and BrushMaterialID
- `FChunkEditLayer` - Sparse per-chunk edit storage
- `FVoxelBrushParams` - Brush configuration (shape, radius, falloff, strength)
- `FChunkCollisionData` - Per-chunk collision state and UBodySetup reference
- `FScatterDefinition` - Scatter type configuration (mesh, density, placement rules, billboard/tree settings)
- `FScatterSpawnPoint` - Individual scatter instance data (position, rotation, scale)
- `FChunkScatterData` - Per-chunk scatter spawn points
- `FVoxelTreeTemplate` - Tree template (trunk/canopy params, placement rules, variance)

## Getting Started

1. **Read the Architecture**: Start with [ARCHITECTURE.md](Documentation/ARCHITECTURE.md)
2. **Understand LOD System**: Review [LOD_SYSTEM.md](Documentation/LOD_SYSTEM.md)
3. **Understand Rendering System**: Review [RENDERING_SYSTEM.md](Documentation/RENDERING_SYSTEM.md)
3. **Review Coding Standards**: Check [CODING_STANDARDS.md](Documentation/CODING_STANDARDS.md)
4. **Begin Implementation**: Follow [IMPLEMENTATION_PHASES.md](Documentation/IMPLEMENTATION_PHASES.md)

## Development Workflow

1. Check relevant documentation before implementing
2. Maintain interface contracts
3. Follow Unreal Engine coding conventions
4. Document public APIs
5. Consider performance implications
6. Write tests for new functionality

## Contact & Support

For questions about architecture decisions, refer to the documentation in `/Documentation/`.

For Claude Code assistance, the project context is available in `.claude/instructions.md`.

---

**Current Engine Version**: Unreal Engine 5.7
**Language**: C++ (17)
**Rendering**: FLocalVertexFactory + Compute Shaders (GPU-driven)
