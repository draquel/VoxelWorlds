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
│   ├── VoxelCore/          # Data structures, math utilities
│   ├── VoxelLOD/           # Pluggable LOD system
│   ├── VoxelGeneration/    # Noise, biomes, world modes
│   ├── VoxelMeshing/       # Cubic and smooth meshing
│   ├── VoxelRendering/     # Hybrid rendering system
│   ├── VoxelStreaming/     # Chunk management and streaming
│   ├── VoxelEditing/       # Edit layer and tools
│   ├── VoxelScatter/       # Foliage and scatter system
│   └── VoxelRuntime/       # UE integration and components
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

### In Progress
- 🔄 Phase 2: Basic biome system (2-3 biomes)

### Planned
- ⏳ Phase 2: Chunk generation pipeline
- ⏳ Phase 3+: Advanced meshing (Greedy, Marching Cubes)
- ⏳ Phase 3+: Custom Vertex Factory renderer

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
- `FVoxelCPUNoiseGenerator` - CPU-based noise generation
- `FVoxelGPUNoiseGenerator` - GPU compute shader noise generation
- `FVoxelCPUCubicMesher` - CPU-based cubic mesh generation
- `FVoxelGPUCubicMesher` - GPU compute shader cubic meshing
- `FVoxelPMCRenderer` - ProceduralMeshComponent-based renderer
- `FVoxelVertexFactory` - Custom vertex factory for GPU rendering
- `UVoxelWorldConfiguration` - World configuration asset

### Key Data Structures
- `FVoxelData` - Per-voxel data (4 bytes)
- `FVoxelVertex` - GPU vertex format (28 bytes)
- `FChunkDescriptor` - Chunk metadata
- `FChunkMeshData` - CPU mesh data
- `FChunkRenderData` - GPU render data
- `FLODQueryContext` - LOD query parameters
- `FVoxelNoiseParams` - Noise generation parameters
- `FVoxelMeshingRequest` - Mesh generation request with neighbor data
- `FWorldModeTerrainParams` - Terrain configuration (SeaLevel, HeightScale, etc.)

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
**Rendering**: Custom Vertex Factory + Compute Shaders
