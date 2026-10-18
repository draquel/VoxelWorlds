# VoxelWorlds Development Context

## Project Overview

This is a **GPU-driven voxel terrain system** for Unreal Engine 5.7 with pluggable LOD strategies and hybrid rendering architecture. The system supports multiple world modes (infinite plane, spherical planet, island/bowl) and dual meshing styles (cubic/smooth).

## Core Architecture Principles

### 1. Pluggable LOD System
All LOD strategies implement the `IVoxelLODStrategy` interface. This allows swapping LOD implementations without modifying core systems.

**Default**: `FDistanceBandLODStrategy` (distance-based LOD rings)  
**Future**: `FQuadtreeLODStrategy`, `FOctreeLODStrategy`

See: `Documentation/LOD_SYSTEM.md`

### 2. Hybrid Rendering Architecture
Two renderer implementations via `IVoxelMeshRenderer` interface:
- **Runtime**: `FVoxelCustomVFRenderer` - Custom Vertex Factory, GPU-driven, zero CPU overhead
- **Editor**: `FVoxelPMCRenderer` - ProceduralMeshComponent fallback for tools

See: `Documentation/RENDERING_SYSTEM.md`

### 3. GPU-First Pipeline
All voxel generation and meshing happens on GPU via compute shaders:
1. Generate voxel data (noise sampling, biome selection)
2. Mesh generation (cubic or smooth)
3. Direct GPU buffer binding (no CPU readback in runtime)

See: `Documentation/GPU_PIPELINE.md`

### 4. Modular Design
Clear module boundaries with interface-based integration:
- **VoxelCore**: Shared data structures, no dependencies
- **VoxelLOD**: LOD system (pluggable via interface)
- **VoxelRendering**: Rendering system (pluggable via interface)
- **VoxelGeneration**: Noise, biomes, world modes
- **VoxelMeshing**: Cubic and smooth meshing algorithms
- **VoxelStreaming**: Chunk management and streaming

## Key Interfaces to Maintain

### IVoxelLODStrategy
```cpp
// LOD strategy interface - all implementations must conform
// Location: Source/VoxelLOD/Public/IVoxelLODStrategy.h

class IVoxelLODStrategy {
    virtual int32 GetLODForChunk(FIntVector ChunkCoord, const FLODQueryContext& Context) const = 0;
    virtual float GetLODMorphFactor(FIntVector ChunkCoord, const FLODQueryContext& Context) const = 0;
    virtual TArray<FChunkLODRequest> GetVisibleChunks(const FLODQueryContext& Context) const = 0;
    // ... see LOD_SYSTEM.md for complete interface
};
```

### IVoxelMeshRenderer
```cpp
// Renderer interface - GPU-driven or PMC implementations
// Location: Source/VoxelRendering/Public/IVoxelMeshRenderer.h

class IVoxelMeshRenderer {
    virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) = 0;
    virtual void RemoveChunk(FIntVector ChunkCoord) = 0;
    virtual void UpdateLODTransition(FIntVector ChunkCoord, float MorphFactor) = 0;
    // ... see RENDERING_SYSTEM.md for complete interface
};
```

### IVoxelWorldMode
```cpp
// World mode interface - infinite, spherical, island implementations
// Location: Source/VoxelGeneration/Public/IVoxelWorldMode.h (abridged; see Documentation/Features/WORLD_MODES.md)

class IVoxelWorldMode {
    virtual float GetDensityAt(const FVector& WorldPos, int32 LODLevel, float NoiseValue) const = 0;
    virtual float GetTerrainHeightAt(float X, float Y, const FVoxelNoiseParams& NoiseParams) const = 0;
    virtual void GetTerrainHeightBounds(float& OutMin, float& OutMax) const;   // authority for vertical culling
    virtual FFloatInterval GetSurfaceRange(const FVector2D& MinXY, const FVector2D& MaxXY, const FVoxelNoiseParams&) const; // per-column streaming window (D8)
    // Bounds and local frame (world modes review WM-A2; spherical SP-A1/SP-A2)
    virtual bool IsInsideWorld(double X, double Y) const;
    virtual FVoxelWorldBounds GetWorldBounds() const;
    virtual FVector GetUpVector(const FVector& WorldPos) const;
    virtual float GetAltitude(const FVector& WorldPos) const;
    virtual void GetAltitudeBounds(float& OutMin, float& OutMax) const;      // spherical SP-A4 (a planet: its shell)
    virtual FVoxelSurfaceCoord WorldToSurface(const FVector& WorldPos) const;
    virtual FVector SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const;
};
```
Build world-mode params and instances ONLY through `FVoxelWorldModeFactory` (`VoxelWorldModeFactory.h`:
`MakeIslandParams`, `MakePlanetParams`, `MakeTerrainParams`, `Create`, `FillGenerationRequest`, `MakeFingerprint`); never
copy config fields into `FIslandBowlParams` / `FSphericalPlanetParams` by hand (that is how the island centre came to
mean two things, KI3). Positions in the built params are absolute (island centre = `WorldOrigin` + offset). Game code
reaches the configured mode's bounds and frame through `UVoxelChunkManager` (`Voxel|World` BlueprintPure functions).

"Is this under the water" is always `FVoxelSeaSurface` (VoxelCore): `IsBelow` / `GetDepth` against the `WaterLevel` plane
or, on a planet, the sphere of `WaterRadius` (spherical SP-B8, SP-E1). Get it from `UVoxelChunkManager::GetSeaSurface`
(`true` for the `WaterLevel + WorldOrigin.Z` plane game code uses) or `FVoxelSeaSurface::FromConfig`; never compare a Z
with `WaterLevel` in new code. A planet's sea surface is the chunk manager's ocean shell (`FVoxelPlanetOcean`); its
spawn point over the terrain is `UVoxelChunkManager::GetPlanetSpawnPosition`.

Surface queries go along local up (spherical Phase 4, SP-A3): `UVoxelChunkManager::QueryEditMergedSurfacePoint` (the
edit-merged surface on the up-line through a position; heightmaps: `QueryEditMergedSurface`), `GetGeneratedSurfacePoint`
(analytic), and `FVoxelSurfaceQuery::QueryPlanetSurfaceConditions` / `SampleSurfacePoint` for generation's material and
biome. Never call `GetTerrainHeightAt` in code that can run on a planet: there it logs once and degrades to the upper
hemisphere's Z. 2D systems key a planet on the cube sphere (`FVoxelCubeSphere`, `FVoxelCubeSphereGrid` in VoxelCore:
atlas coordinates Face × N + U, V), as the map, navigation regions and the NPC spawner do; heightmap code paths keep
their XY keys untouched.

Terrain conditioning has two kinds of zone (spherical Phase 5, SP-B10): a heightmap zone (an XY disc, a world-Z target,
`Direction` zero) and a planet zone (`FVoxelConditioningZone::MakeRadial`: a cap on the datum sphere, great-circle radii,
an altitude target). Each world mode applies only its own kind (`ApplyToHeight` / `ApplyToAltitude`); a conditioner
answers `GatherConditioning` for heightmaps and `GatherConditioningOnSphere` for planets. "The ground the generator
makes" is the natural surface with the zones layered on: on a planet `UVoxelChunkManager::GetGeneratedSurfacePoint` /
`GetGeneratedPlanetAltitude`, on a heightmap `GetGeneratedSurfaceHeight` (or `GetTerrainHeightAt` plus
`ApplyToHeight` with the zones from `GatherConditioningZones`). Any reader that means that ground uses it: scatter's
buried-chunk test (`FScatterExtractionContext::ConditioningZones`, from the chunk manager's conditioning gatherer) and
the PCG sampler's far points (`FVoxelSurfaceQuery::SampleSurface` / `SampleSurfacePoint` with the bounds' zones) do
since world modes review G23. A heightmap chunk's data cannot show the voxel over its top layer (the chunk above's):
near the generated ground scatter takes it as the generator made it, with the chunk above's edits applied
(`FScatterExtractionContext::GetVoxelAboveChunk`, G26); never assume air there. A planet chunk's scatter rays (one family on the cube-sphere grid for
every chunk, G31) read the voxels just past its +X, +Y and +Z faces the same way, as the planet generator made them with
the neighbours' edits (`FScatterExtractionContext::GetPlanetVoxelOutsideChunk`, `TouchingEdits`, G29). `GetSurfacePointAt` and `GetTerrainHeightAt` are the natural surface (POI placement
surveys it on purpose). Changing planet generation bumps
`FVoxelWorldModeFactory::PlanetGenerationVersion` (4 since Phase 5); heightmap changes bump `GenerationVersion`.

Any per-chunk state `UVoxelScatterManager` keeps is dropped in `OnChunkUnloaded`, both caches included, so a chunk that
streams back in is extracted as a first pass (world modes review G30: a short-circuit `||` once kept the spawn points
cached and turned the reload into an append). `voxel.Scatter.Digest`'s `caches:` line shows a leftover as
`scatterOnly` > 0 or an `instanceMismatch`.

## Core Data Structures

### FVoxelData (4 bytes per voxel)
```cpp
struct FVoxelData {
    uint8 MaterialID;   // 256 material types
    uint8 Density;      // 0-255 (127 = surface)
    uint8 BiomeID;      // 256 biome types
    uint8 Metadata;     // AO, lighting, flags
};
```

### FChunkDescriptor
```cpp
struct FChunkDescriptor {
    FIntVector ChunkCoord;
    int32 LODLevel;
    int32 ChunkSize;
    TArray<FVoxelData> VoxelData;
    FBox Bounds;
    float MorphFactor;
    // ... see DATA_STRUCTURES.md
};
```

### FLODQueryContext
```cpp
struct FLODQueryContext {
    FVector ViewerPosition;
    FVector ViewerForward;
    float ViewDistance;
    EWorldMode WorldMode;
    // ... see LOD_SYSTEM.md
};
```

## File Organization Rules

### Module Structure
```
VoxelWorlds/
├── Source/
│   ├── VoxelCore/          # Shared types, NO dependencies on other modules
│   ├── VoxelLOD/           # ONLY LOD system, depends on VoxelCore
│   ├── VoxelRendering/     # ONLY rendering, depends on VoxelCore
│   ├── VoxelGeneration/    # Noise, biomes, depends on VoxelCore
│   ├── VoxelMeshing/       # Meshing algorithms, depends on VoxelCore
│   ├── VoxelStreaming/     # Chunk management, depends on VoxelCore + VoxelLOD
│   ├── VoxelWorlds/        # UE integration module ("VoxelRuntime" in older docs), depends on all
│   └── VoxelWorldsEditor/  # EDITOR-ONLY authoring tools (Type "Editor"), stripped from shipping; see Documentation/Features/EDITOR_TOOLS.md
```
(Additional runtime modules also exist: VoxelScatter, VoxelMap, VoxelPCG, VoxelNavigation — surface pathfinding over the
terrain without a navmesh: `FVoxelSurfacePathfinder` (pure A* over a 2.5-D cell grid, height-sampler driven) +
`UVoxelSurfaceNavigationSubsystem` (tile cache over the chunk manager's edit-merged / analytic surface, blocked boxes,
`voxel.Nav.*` cvars, debug draw); see Documentation/Features/SURFACE_NAVIGATION.md. Depends on VoxelCore + VoxelStreaming only;
nothing in VoxelWorlds depends on it.)

### Naming Conventions
- **Interfaces**: `IVoxelXxx` (e.g., `IVoxelLODStrategy`)
- **Structs**: `FVoxelXxx` or `FXxx` (e.g., `FVoxelData`, `FChunkDescriptor`)
- **Classes**: `FVoxelXxx` for non-UObject, `UVoxelXxx` for UObject, `AVoxelXxx` for Actor
- **Enums**: `EVoxelXxx` or `EXxx` (e.g., `EWorldMode`, `EMeshingMode`)

### Header Organization
```cpp
// 1. Copyright/license
// 2. Header guard or #pragma once
// 3. Includes (Engine first, then project)
// 4. Forward declarations
// 5. Interface/class definition
// 6. Inline function implementations (if any)
```

## Coding Standards

### Performance Priorities
1. **GPU-first**: Keep data on GPU whenever possible
2. **Minimize CPU-GPU transfers**: No readback in runtime path
3. **Cache-friendly**: Contiguous memory layouts
4. **Batch operations**: Multiple chunks per frame
5. **Profile before optimizing**: Measure, don't guess

### Documentation Requirements
- All public interfaces must have Doxygen-style comments
- Include usage examples for complex interfaces
- Document performance characteristics (O(n), memory usage)
- Explain architectural decisions in header comments

### Example:
```cpp
/**
 * LOD strategy interface for voxel terrain system.
 * 
 * All LOD implementations must conform to this interface to integrate
 * with the chunk streaming system. Strategies determine which chunks
 * should be loaded/rendered at what detail level.
 * 
 * Performance: GetLODForChunk() is called frequently (per chunk per frame)
 * and must be fast (< 1μs per call).
 * 
 * Thread Safety: All methods must be thread-safe for read-only operations.
 * Update() may be called from game thread only.
 * 
 * @see FDistanceBandLODStrategy for reference implementation
 * @see Documentation/LOD_SYSTEM.md for architecture details
 */
class IVoxelLODStrategy {
    // ...
};
```

## When Implementing New Code

### Checklist
1. ✅ Check relevant documentation file first (ARCHITECTURE.md, LOD_SYSTEM.md, etc.)
2. ✅ Verify interface contract compliance
3. ✅ Follow Unreal Engine coding standards
4. ✅ Document public APIs with Doxygen comments
5. ✅ Consider performance implications (GPU vs CPU, memory, cache)
6. ✅ Add appropriate module dependencies in .Build.cs
7. ✅ Write unit tests for new functionality

### Common Patterns

**Creating a new LOD strategy:**
```cpp
// 1. Inherit from IVoxelLODStrategy
// 2. Implement all virtual methods
// 3. Add configuration properties
// 4. Register in LOD system
// See: Documentation/LOD_SYSTEM.md
```

**Adding a new world mode:**
```cpp
// 1. Inherit from IVoxelWorldMode
// 2. Implement SDF and coordinate conversion
// 3. Add to EWorldMode enum
// 4. Update world configuration
// See: Documentation/WORLD_MODES.md
```

**Creating a compute shader:**
```cpp
// 1. Create .usf file in Shaders/
// 2. Define shader class in C++
// 3. Register with shader system
// 4. Dispatch from chunk manager
// See: Documentation/GPU_PIPELINE.md
```

## Testing Strategy

### Unit Tests
- Test each LOD strategy independently
- Test voxel data packing/unpacking
- Test coordinate conversions
- Test biome selection logic

### Integration Tests
- Test LOD strategy with ChunkManager
- Test renderer with various mesh sizes
- Test world mode coordinate systems
- Test streaming under movement

### Performance Tests
- Profile GPU compute timings (< 2ms per 4 chunks)
- Measure memory usage per chunk (target: ~200KB)
- Check frame time budget (< 5ms total compute)
- Validate streaming throughput (4 loads + 8 unloads per frame)

See: `Documentation/TESTING_STRATEGY.md`

## Current Development Phase

**Phases 1–7: COMPLETE** — Foundation, Generation, Advanced Meshing, Marching Cubes Meshing, World Modes, Editing & Collision, Scatter & Polish.

**Phase 8: Advanced Features** (in progress)
- [x] `GetWorldMode()` public getter on `UVoxelChunkManager` — exposes terrain height queries for external systems
- [x] Terrain Map / Mini-Map
- [x] Cave generation — multi-layer (Cheese, Spaghetti, Noodle) via `UVoxelCaveConfiguration`. See `Documentation/CAVE_SYSTEM.md`
- [x] Advanced noise (Cellular, Voronoi) — GPU + CPU implementations. See `Documentation/NOISE_LIBRARY.md`
- [x] Dual Contouring mesher — CPU + GPU implementations with QEF solver and LOD boundary merging. See `Documentation/DUAL_CONTOURING.md`
- [~] Editor tools — **Milestone 1 DONE**: `VoxelWorldsEditor` module (Type "Editor", stripped from shipping) + field-sampler core + 8 built-in fields + dockable "Voxel Field Preview" panel + `AVoxelFieldPreviewActor`; runtime `FVoxelCaveQuery` added. See `Documentation/Features/EDITOR_TOOLS.md`
- [ ] Quadtree/Octree LOD, improved water, etc.

See: `Documentation/IMPLEMENTATION_PHASES.md` for full roadmap

## Common Development Commands

### For Claude Code
- "Implement the IVoxelLODStrategy interface from Documentation/LOD_SYSTEM.md"
- "Create FDistanceBandLODStrategy according to the architecture"
- "Review this implementation against CODING_STANDARDS.md"
- "Check if this follows the VoxelCore module structure"
- "Generate unit tests for this LOD strategy"

### Architecture Queries
- "How does the LOD system integrate with ChunkManager?" → Check ARCHITECTURE.md
- "What's the vertex format for Custom VF?" → Check RENDERING_SYSTEM.md
- "How do I add a new world mode?" → Check WORLD_MODES.md
- "What's the compute shader workflow?" → Check GPU_PIPELINE.md

## Performance Budgets

### Memory (per chunk at LOD 0)
- Voxel Data: 128 KB (32³ × 4 bytes)
- Vertex Data: ~50 KB (cubic) / ~100 KB (smooth)
- Index Data: ~30 KB
- **Total: ~200 KB per chunk**
- **Target: 1000 visible chunks = 200 MB**

### Compute (per frame at 60 FPS)
- Chunk generation: 4 chunks × 0.5ms = 2ms
- Meshing: 4 chunks × 0.5ms = 2ms
- LOD updates: 0.5ms
- Collision: 0.5ms
- **Total GPU compute: ~5ms (30% of 16.6ms frame)**

See: `Documentation/PERFORMANCE_TARGETS.md`

## Key Files Reference

### Interfaces
- `Source/VoxelLOD/Public/IVoxelLODStrategy.h` - LOD strategy interface
- `Source/VoxelRendering/Public/IVoxelMeshRenderer.h` - Renderer interface
- `Source/VoxelGeneration/Public/IVoxelWorldMode.h` - World mode interface

### Core Types
- `Source/VoxelCore/Public/VoxelTypes.h` - Data structures
- `Source/VoxelCore/Public/VoxelWorldConfiguration.h` - Configuration

### Implementations
- `Source/VoxelLOD/Private/DistanceBandLODStrategy.cpp` - Default LOD
- `Source/VoxelRendering/Private/VoxelCustomVFRenderer.cpp` - GPU renderer
- `Source/VoxelStreaming/Private/VoxelChunkManager.cpp` - Chunk streaming

### Meshing
- `Source/VoxelMeshing/Public/VoxelCPUMarchingCubesMesher.h` - CPU Marching Cubes
- `Source/VoxelMeshing/Public/VoxelCPUDualContourMesher.h` - CPU Dual Contouring
- `Source/VoxelMeshing/Public/VoxelGPUMarchingCubesMesher.h` - GPU Marching Cubes
- `Source/VoxelMeshing/Public/VoxelGPUDualContourMesher.h` - GPU Dual Contouring
- `Source/VoxelMeshing/Private/QEFSolver.h` - QEF solver (Jacobi SVD)

### Shaders
- `Shaders/VoxelGeneration.usf` - Voxel generation compute shader
- `Shaders/MarchingCubesMeshGeneration.usf` - Marching Cubes meshing compute shader
- `Shaders/DualContourMeshGeneration.usf` - Dual Contouring 4-pass compute shader

## Architecture Decision Records

### Why Pluggable LOD?
Allows experimentation and optimization without refactoring core systems. Can ship with simple distance bands and add complex strategies later based on profiling.

### Why Hybrid Rendering?
Custom Vertex Factory maximizes runtime performance (GPU-driven, zero CPU overhead), while PMC provides simplicity for editor tools and debugging.

### Why GPU-First?
Voxel terrain benefits enormously from GPU parallelism. Keeping data GPU-side eliminates the biggest bottleneck (CPU-GPU transfers).

### Why Distance Bands Default?
Simplest to implement, predictable performance, sufficient for most use cases. Can add quadtree/octree later if profiling shows need.

## Resources

- **Unreal Engine Documentation**: https://docs.unrealengine.com/
- **Compute Shader Guide**: https://docs.unrealengine.com/en-US/ProgrammingAndScripting/Rendering/
- **Vertex Factory Guide**: Internal UE documentation (see LocalVertexFactory.h)

## Notes for Claude Code

- Always check Documentation/ before implementing
- Maintain interface contracts strictly
- Performance is critical - profile before optimizing
- Keep modules decoupled via interfaces
- Document architectural decisions in code comments
- Test each component independently before integration

## Unreal Engine Implementation Notes
- Blueprint types do not support uint* fields

---

Last Updated: 2026-02-19
Architecture Version: 2.1
