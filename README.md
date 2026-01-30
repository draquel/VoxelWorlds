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
- ✅ Architecture design and documentation
- ✅ Interface definitions (LOD, Rendering, World Modes)
- ✅ Data structure specifications

### In Progress
- 🔄 Phase 1: Core data structures implementation
- 🔄 Phase 1: Distance Band LOD strategy
- 🔄 Phase 1: Hybrid rendering interface

### Planned
- ⏳ Phase 2: GPU noise generation
- ⏳ Phase 2: Infinite plane world mode
- ⏳ Phase 2: Cubic meshing
- ⏳ Phase 3+: Advanced features

See [Implementation Phases](Documentation/IMPLEMENTATION_PHASES.md) for detailed roadmap.

## Quick Reference

### Key Interfaces
- `IVoxelLODStrategy` - LOD strategy abstraction
- `IVoxelMeshRenderer` - Renderer abstraction
- `IVoxelWorldMode` - World mode abstraction

### Key Classes
- `UVoxelChunkManager` - Chunk streaming coordinator
- `FDistanceBandLODStrategy` - Default LOD implementation
- `FVoxelVertexFactory` - Custom vertex factory for GPU rendering
- `UVoxelWorldConfiguration` - World configuration asset

### Key Data Structures
- `FVoxelData` - Per-voxel data (4 bytes)
- `FChunkDescriptor` - Chunk metadata
- `FLODQueryContext` - LOD query parameters
- `FChunkRenderData` - Mesh render data

## Getting Started

1. **Read the Architecture**: Start with [ARCHITECTURE.md](Documentation/ARCHITECTURE.md)
2. **Understand LOD System**: Review [LOD_SYSTEM.md](Documentation/LOD_SYSTEM.md)
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
