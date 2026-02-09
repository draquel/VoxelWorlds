# VoxelWorlds Architecture Overview

**Version**: 2.0  
**Last Updated**: 2026-01-30  
**Target Engine**: Unreal Engine 5.7

## Table of Contents

1. [System Overview](#system-overview)
2. [Architecture Diagram](#architecture-diagram)
3. [Core Data Structures](#core-data-structures)
4. [Module Organization](#module-organization)
5. [Key Systems](#key-systems)
6. [Integration Points](#integration-points)
7. [Data Flow](#data-flow)
8. [Performance Considerations](#performance-considerations)
9. [Design Decisions](#design-decisions)

---

## System Overview

VoxelWorlds is a **GPU-driven voxel terrain system** designed for high-performance, large-scale worlds in Unreal Engine 5.7. The architecture emphasizes:

- **Modularity**: Clear separation of concerns with interface-based integration
- **Performance**: GPU-first approach minimizing CPU overhead
- **Flexibility**: Pluggable LOD strategies and rendering implementations
- **Scalability**: Streaming system handles thousands of chunks efficiently

### Key Capabilities

- Multiple world modes (infinite plane, spherical planet, island/bowl)
- Dual meshing styles (cubic/smooth)
- Pluggable LOD system (distance bands, quadtree, octree)
- Hybrid rendering (Custom VF + PMC fallback)
- GPU compute pipeline for generation and meshing
- Biome system with material assignment
- Edit layer for terrain modification
- Vegetation scatter system

---

## Architecture Diagram

```
VoxelWorld (Root Container)
│
├── WorldConfiguration
│   ├── WorldMode (Infinite/Spherical/Island)
│   ├── MeshingMode (Cubic/Smooth)
│   ├── VoxelSize, ChunkSize
│   └── Performance Budgets
│
├── ChunkManager (Streaming Coordinator)
│   ├── Spatial Indexing (Grid/Octree)
│   ├── ChunkPool (Reusable chunks)
│   ├── LODStrategy (Pluggable) ◄── NEW
│   │   ├── IVoxelLODStrategy Interface
│   │   ├── DistanceBandLODStrategy (default)
│   │   ├── QuadtreeLODStrategy (future)
│   │   └── OctreeLODStrategy (future)
│   └── StreamingSystem
│       ├── LoadQueue (prioritized)
│       └── UnloadQueue
│
├── VoxelDataProvider
│   ├── NoiseGenerator (GPU Compute)
│   │   ├── Noise Library (Perlin, Simplex, Voronoi)
│   │   ├── FBM/Fractal Support
│   │   └── Domain Warping
│   ├── BiomeSystem
│   │   ├── BiomeDefinitions
│   │   ├── BiomeBlending
│   │   └── Material Assignment
│   └── EditLayer (Overlay Modifications)
│       ├── Sparse Edit Storage
│       ├── Add/Subtract/Paint Operations
│       └── Serialization
│
├── MaterialRegistry
│   ├── MaterialDefinitions (256 types)
│   ├── Atlas Configuration (cubic)
│   └── Physical Properties
│
├── RenderingSystem (Hybrid) ◄── ENHANCED
│   ├── IVoxelMeshRenderer Interface (Pluggable)
│   │   ├── VoxelCustomVFRenderer (GPU-driven, runtime)
│   │   │   ├── FVoxelVertexFactory
│   │   │   ├── FVoxelSceneProxy
│   │   │   ├── Per-Chunk GPU Buffers
│   │   │   └── LOD Morphing (Vertex Shader)
│   │   └── VoxelPMCRenderer (Editor/tools)
│   │       └── ProceduralMeshComponents
│   ├── MeshingSystem
│   │   ├── CubicMesher (Face-culling, Greedy, AO)
│   │   └── SmoothMesher (Marching Cubes / Dual Contouring)
│   └── CollisionManager (Separate, Async, Lower LOD)
│
└── ScatterSystem
    ├── Vegetation Placement
    ├── HISM/ISM Management
    └── Biome-based Distribution
```

---

## Core Data Structures

### FVoxelData (4 bytes per voxel)
```cpp
struct FVoxelData {
    uint8 MaterialID;      // 256 material types
    uint8 Density;         // 0-255 for smooth terrain (127 = surface)
    uint8 BiomeID;         // 256 biome types
    uint8 Metadata;        // AO cache, lighting, custom flags
};
```

**Memory**: 32³ chunk = 32,768 voxels × 4 bytes = 128 KB

### FChunkDescriptor
```cpp
struct FChunkDescriptor {
    FIntVector ChunkCoord;     // Chunk position in chunk space
    int32 LODLevel;            // 0 = finest detail
    int32 ChunkSize;           // Voxels per edge (32, 64, 128, etc.)
    
    TArray<FVoxelData> VoxelData;  // Voxel contents
    
    FBox Bounds;               // World-space bounds
    bool bIsDirty;             // Needs remeshing
    bool bHasEdits;            // Has edit layer modifications
    float MorphFactor;         // LOD transition blend (0-1)
    
    int32 GenerationSeed;      // For deterministic regeneration (int32 for BP)
};
```

### FChunkRenderData
```cpp
struct FChunkRenderData {
    FIntVector ChunkCoord;
    int32 LODLevel;
    
    // GPU buffer handles (RDG)
    FRDGBufferRef ComputeVertexBuffer;
    FRDGBufferRef ComputeIndexBuffer;
    
    uint32 VertexCount;
    uint32 IndexCount;
    bool bNeedsCollisionUpdate;
    
    // For PMC fallback
    TArray<FVoxelData> VoxelData;
};
```

### FLODQueryContext
```cpp
struct FLODQueryContext {
    // Camera state
    FVector ViewerPosition;
    FVector ViewerForward;
    float ViewDistance;
    float FieldOfView;
    TArray<FPlane> FrustumPlanes;
    
    // World state
    FVector WorldOrigin;
    EWorldMode WorldMode;
    float WorldRadius;
    
    // Performance budgets
    int32 MaxChunksToLoadPerFrame = 4;
    int32 MaxChunksToUnloadPerFrame = 8;
    float TimeSliceMS = 2.0f;
    
    // Frame info
    int64 FrameNumber;          // int64 for Blueprint compatibility
    float GameTime;
};
```

### FVoxelVertex (28 bytes - optimized)
```cpp
struct FVoxelVertex {
    FVector3f Position;          // 12 bytes
    uint32 PackedNormalAndAO;    // 4 bytes (10/10/10 normal + 8-bit AO)
    FVector2f UV;                // 8 bytes
    uint32 PackedMaterialData;   // 4 bytes (MaterialID, BiomeID, Metadata)
};
```

**Comparison**: PMC uses ~48+ bytes per vertex

---

## Module Organization

### Dependency Graph
```
VoxelRuntime
    ├── depends on → VoxelStreaming
    ├── depends on → VoxelRendering
    ├── depends on → VoxelEditing
    └── depends on → VoxelScatter

VoxelStreaming
    ├── depends on → VoxelLOD
    ├── depends on → VoxelGeneration
    └── depends on → VoxelMeshing

VoxelRendering
    └── depends on → VoxelCore

VoxelLOD
    └── depends on → VoxelCore

VoxelGeneration
    └── depends on → VoxelCore

VoxelMeshing
    └── depends on → VoxelCore

VoxelCore
    └── (no dependencies - shared foundation)
```

### Module Responsibilities

**VoxelCore**
- Core data structures (FVoxelData, FChunkDescriptor)
- Math utilities (coordinate conversions)
- Configuration types (UVoxelWorldConfiguration)
- No dependencies on other modules

**VoxelLOD**
- IVoxelLODStrategy interface
- LOD implementations (DistanceBand, Quadtree, Octree)
- FLODQueryContext and related types
- Depends only on VoxelCore

**VoxelGeneration** *(LoadingPhase: PostConfigInit)*
- Noise generation (GPU compute shaders)
- Biome system
- World mode implementations (IVoxelWorldMode)
- Material assignment logic
- **Note**: Requires `PostConfigInit` loading phase for global shader registration

**VoxelMeshing** *(LoadingPhase: PostConfigInit)*
- Cubic mesher (face culling, greedy meshing, AO)
- Smooth mesher (Marching Cubes, Dual Contouring)
- Mesh generation compute shaders
- **Note**: Requires `PostConfigInit` loading phase for global shader registration

**VoxelRendering**
- IVoxelMeshRenderer interface
- Custom Vertex Factory implementation
- PMC renderer fallback
- Collision manager

**VoxelStreaming**
- ChunkManager (main streaming coordinator)
- Chunk state machine
- Load/unload queue management
- Integrates LOD, generation, and meshing

**VoxelEditing**
- Edit layer system
- Editing tools (add, subtract, paint)
- Serialization

**VoxelScatter**
- Vegetation placement
- HISM/ISM management
- GPU scatter generation

**VoxelRuntime**
- AVoxelWorld actor
- UVoxelWorldSubsystem
- Blueprint integration
- Editor tools

### Module Loading Phases

Modules that use global shaders (`IMPLEMENT_GLOBAL_SHADER` macro) must use `PostConfigInit` loading phase in the `.uplugin` file. This ensures the shader system is fully initialized before shader registration occurs.

| Module | LoadingPhase | Reason |
|--------|--------------|--------|
| VoxelCore | Default | No shaders |
| VoxelLOD | Default | No shaders |
| VoxelGeneration | **PostConfigInit** | GPU compute shaders |
| VoxelMeshing | **PostConfigInit** | GPU compute shaders |
| VoxelRendering | Default | Uses shaders but no IMPLEMENT_GLOBAL_SHADER |
| VoxelStreaming | Default | No shaders |

**Failure to set correct LoadingPhase** will cause editor crashes during startup with access violations in `CreatePackage` or `ProcessNewlyLoadedUObjects`.

---

## Key Systems

### 1. Pluggable LOD System

See: [LOD_SYSTEM.md](LOD_SYSTEM.md) for complete details.

**Interface**: `IVoxelLODStrategy`
```cpp
class IVoxelLODStrategy {
    virtual int32 GetLODForChunk(FIntVector ChunkCoord, const FLODQueryContext& Context) const = 0;
    virtual float GetLODMorphFactor(FIntVector ChunkCoord, const FLODQueryContext& Context) const = 0;
    virtual TArray<FChunkLODRequest> GetVisibleChunks(const FLODQueryContext& Context) const = 0;
    virtual void GetChunksToLoad(TArray<FChunkLODRequest>& OutLoad, const FLODQueryContext& Context) const = 0;
    virtual void GetChunksToUnload(TArray<FIntVector>& OutUnload, const FLODQueryContext& Context) const = 0;
    virtual void Initialize(const FVoxelWorldConfiguration& WorldConfig) = 0;
    virtual void Update(const FLODQueryContext& Context, float DeltaTime) = 0;
};
```

**Default Implementation**: `FDistanceBandLODStrategy`
- Distance-based LOD rings
- Simple and predictable
- Configurable bands per world mode
- LOD morphing support

**Future Implementations**:
- `FQuadtreeLODStrategy` - Screen-space adaptive for infinite plane
- `FOctreeLODStrategy` - 3D adaptive for spherical worlds

### 2. Hybrid Rendering System

See: [RENDERING_SYSTEM.md](RENDERING_SYSTEM.md) for complete details.

**Interface**: `IVoxelMeshRenderer`
```cpp
class IVoxelMeshRenderer {
    virtual void Initialize(UWorld* World, const FVoxelWorldConfiguration& Config) = 0;
    virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) = 0;
    virtual void RemoveChunk(FIntVector ChunkCoord) = 0;
    virtual void SetMaterial(UMaterialInterface* Material) = 0;
    virtual void UpdateLODTransition(FIntVector ChunkCoord, float MorphFactor) = 0;
};
```

**Runtime**: `FVoxelCustomVFRenderer`
- Custom Vertex Factory (`FVoxelVertexFactory`)
- GPU-driven rendering (no CPU readback)
- Direct compute shader → vertex shader pipeline
- LOD morphing in vertex shader
- Maximum performance

**Editor/Tools**: `FVoxelPMCRenderer`
- ProceduralMeshComponent-based
- Requires GPU→CPU readback
- Simpler for debugging and tools
- Full UE feature compatibility

### 3. GPU Compute Pipeline

See: [GPU_PIPELINE.md](GPU_PIPELINE.md) for complete details.

**Phase 1: Voxel Generation**
```
Input: ChunkCoord, LOD, WorldMode, NoiseParams
Compute Shader: GenerateVoxelData.usf
Output: RWStructuredBuffer<FVoxelData>
```

**Phase 2: Meshing**
```
Input: VoxelData buffer, MeshingMode
Compute Shader: GenerateCubicMesh.usf or GenerateSmoothMesh.usf
Output: RWStructuredBuffer<FVoxelVertex>, RWStructuredBuffer<uint32>
```

**Phase 3: Rendering**
```
Custom VF: Direct GPU buffer binding (no CPU copy)
PMC: Readback to CPU, then CreateMeshSection
```

### 4. World Modes

See: [WORLD_MODES.md](WORLD_MODES.md) for complete details.

**Infinite Plane**
- Standard heightmap-style terrain
- Extends infinitely in XY
- Height in Z axis
- Distance band LOD

**Spherical Planet**
- Planet with radius
- Surface detail, coarse interior
- Curvature affects meshing
- Hybrid distance band + octree LOD

**Island/Bowl**
- Bounded world with falloff
- Radial attenuation from center
- Ocean level support
- Distance band LOD

### 5. Material System

See: [MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) for complete details.

**Cubic Mode**: Tile atlas with per-face UVs
**Smooth Mode**: Triplanar projection with material blending

### 6. Biome System

See: [BIOME_SYSTEM.md](BIOME_SYSTEM.md) for complete details.

Selection based on:
- Temperature (noise-driven)
- Moisture (noise-driven)
- Elevation

Each biome defines:
- Terrain shaping (noise layers)
- Material assignment
- Vegetation types

---

## Integration Points

### ChunkManager ↔ LODStrategy

```cpp
// ChunkManager uses cached streaming decisions to avoid per-frame recalculation
void UVoxelChunkManager::TickComponent(float DeltaTime) {
    FLODQueryContext Context = BuildQueryContext();
    FIntVector CurrentViewerChunk = WorldToChunkCoord(Context.ViewerPosition);

    // Phase 2 Optimization: Only update streaming when viewer crosses chunk boundary
    bool bViewerChunkChanged = (CurrentViewerChunk != CachedViewerChunk);

    // Always update LOD strategy for morph factor interpolation
    LODStrategy->Update(Context, DeltaTime);

    // Streaming decisions only when necessary
    if (bForceStreamingUpdate || bViewerChunkChanged) {
        TArray<FChunkLODRequest> ToLoad;
        TArray<FIntVector> ToUnload;

        LODStrategy->GetChunksToLoad(ToLoad, LoadedChunkCoords, Context);
        LODStrategy->GetChunksToUnload(ToUnload, LoadedChunkCoords, Context);

        // Queue operations with O(1) duplicate detection (Phase 1)
        for (const FChunkLODRequest& Request : ToLoad) {
            AddToGenerationQueue(Request);  // Uses TSet for dedup
        }

        CachedViewerChunk = CurrentViewerChunk;
    }

    // LOD transitions only when viewer moved significantly
    if (bViewerChunkChanged || PositionDeltaSq > LODUpdateThresholdSq) {
        UpdateLODTransitions(Context);
    }
}
```

### ChunkManager ↔ Renderer

```cpp
// ChunkManager dispatches render updates
void UVoxelChunkManager::OnChunkMeshingComplete(
    FIntVector ChunkCoord, 
    const FChunkRenderData& RenderData)
{
    // Send to renderer (GPU buffers stay on GPU)
    MeshRenderer->UpdateChunkMesh(RenderData);
    
    // Collision handled separately by CollisionManager::Update()
    // (async pipeline: data prep → thread pool mesh gen + trimesh → game thread physics)
    
    // Update scatter/foliage
    ScatterSystem->GenerateScatterForChunk(ChunkCoord);
}
```

### GPU Compute ↔ Renderer

```cpp
// Direct GPU buffer handoff (Custom VF)
void FVoxelCustomVFRenderer::UpdateChunkMesh(const FChunkRenderData& RenderData) {
    FRDGBuilder GraphBuilder(RHICmdList);
    
    // Extract buffers (stay on GPU!)
    FBufferRHIRef VertexBuffer = 
        GraphBuilder.ConvertToExternalBuffer(RenderData.ComputeVertexBuffer);
    FBufferRHIRef IndexBuffer = 
        GraphBuilder.ConvertToExternalBuffer(RenderData.ComputeIndexBuffer);
    
    // Update vertex factory
    VertexFactory->UpdateBuffers(VertexBuffer, IndexBuffer);
    
    // Mark scene proxy dirty
    SceneProxy->UpdateChunkBuffers(RenderData.ChunkCoord);
}
```

---

## Data Flow

### Chunk Load Flow
```
[Frame N]
Player Movement
    ↓
LODStrategy::GetVisibleChunks()
    ↓
ChunkManager::RequestChunkGeneration()
    ↓
Queue generation request

[Frame N+1]
ChunkManager::ProcessGenerationQueue()
    ↓
Dispatch GPU Compute: GenerateVoxelData
    ↓
GPU executes (async)

[Frame N+2]
Compute complete (async readback or GPU-side)
    ↓
ChunkManager::OnChunkGenerationComplete()
    ↓
Queue meshing request

[Frame N+3]
ChunkManager::ProcessMeshingQueue()
    ↓
Dispatch GPU Compute: GenerateMesh
    ↓
GPU executes (async)

[Frame N+4]
Meshing complete
    ↓
ChunkManager::OnChunkMeshingComplete()
    ↓
MeshRenderer::UpdateChunkMesh()
    ↓
Chunk visible on screen
```

### LOD Transition Flow
```
[Continuous]
ChunkManager::Update()
    ↓
For each loaded chunk:
    ↓
    LODStrategy::GetLODMorphFactor()
    ↓
    If MorphFactor changed:
        ↓
        MeshRenderer::UpdateLODTransition()
        ↓
        Update vertex shader uniform
        ↓
        Vertex shader morphs positions
```

---

## Performance Considerations

### Memory Budget
```
Per Chunk (LOD 0, 32³):
- Voxel Data: 128 KB
- Vertex Data: ~50 KB (cubic) / ~100 KB (smooth)
- Index Data: ~30 KB
- GPU Buffers: Same (no duplicate)
Total: ~200 KB per chunk

Target: 1000 visible chunks = 200 MB
Maximum: 2000 visible chunks = 400 MB
```

### Compute Budget (60 FPS = 16.6ms frame)
```
Per Frame:
- Chunk generation: 4 chunks × 0.5ms = 2.0ms
- Meshing: 4 chunks × 0.5ms = 2.0ms
- LOD updates: 0.5ms
- Collision: 0.5ms
Total GPU Compute: ~5ms (30% of frame)

Remaining: ~11ms for rendering, game logic
```

### Streaming Throughput
```
Load: 4 chunks per frame = 240 chunks/sec
Unload: 8 chunks per frame = 480 chunks/sec

At player speed 1000 cm/s:
- Crosses ~10 chunks/sec
- Load rate 24x faster than needed
- Good safety margin
```

### Optimization Strategies

1. **GPU-First**: Keep data on GPU, avoid CPU readback
2. **Batch Operations**: Process multiple chunks per compute dispatch
3. **Async Compute**: Overlap generation with rendering
4. **LOD Morphing**: Smooth transitions without regeneration
5. **Collision Throttling**: Update less frequently than visuals
6. **Frustum Culling**: Don't generate off-screen chunks
7. **Chunk Pooling**: Reuse memory allocations

### Streaming Optimizations (Phase 1 & 2)

The streaming system has been optimized to eliminate per-frame overhead:

**Phase 1: Queue Management**
- O(1) duplicate detection using TSet tracking alongside queue arrays
- Sorted insertion with `Algo::LowerBound()` instead of repeated Sort() calls
- Per-frame queue growth limiting (2x processing rate cap)

**Phase 2: Decision Caching**
- `CachedViewerChunk`: Only recompute visible chunks when viewer crosses chunk boundary
- `LastLODUpdatePosition`: Skip LOD morph factor updates when viewer moved < 100 units
- `bForceStreamingUpdate`: Flag for manual refresh when needed

**Future Phase 3: LOD Hysteresis** (if needed)
- Buffer zone around LOD boundaries to prevent rapid back-and-forth remeshing
- Separate thresholds for LOD upgrades vs downgrades
- Would add ~50-100 units of hysteresis per LOD band boundary

---

## Design Decisions

### Why Pluggable LOD?

**Decision**: Make LOD strategies swappable via interface.

**Rationale**:
- Allows experimentation without core refactoring
- Can ship simple (distance bands) and add complexity later
- Different strategies optimal for different world modes
- Facilitates testing and comparison

**Trade-offs**:
- Additional abstraction layer (minor virtual call overhead)
- Interface design must be forward-compatible
- More initial architecture work

**Conclusion**: Worth it for flexibility and future-proofing.

### Why Hybrid Rendering?

**Decision**: Two renderer implementations via interface.

**Rationale**:
- Custom VF maximizes runtime performance (zero CPU overhead)
- PMC provides editor/tools simplicity and compatibility
- Clean separation allows independent optimization
- Can ship runtime on Custom VF, debug with PMC

**Trade-offs**:
- Maintain two code paths
- Custom VF has steep learning curve
- More testing surface area

**Conclusion**: Performance gains justify maintenance cost.

### Why GPU-First Pipeline?

**Decision**: All generation and meshing on GPU via compute shaders.

**Rationale**:
- Voxel terrain is embarrassingly parallel
- GPU has 1000x more parallel compute than CPU
- Keeping data GPU-side eliminates transfer bottleneck
- Enables massive chunk throughput

**Trade-offs**:
- HLSL compute shader expertise required
- GPU debugging is harder
- Limited by GPU memory

**Conclusion**: Absolutely necessary for performance targets.

### Why Distance Bands Default?

**Decision**: Start with simple distance-based LOD rings.

**Rationale**:
- Trivial to implement and debug
- Predictable performance and memory
- Sufficient for most use cases
- Can add quadtree/octree later if profiling shows need

**Trade-offs**:
- Not spatially adaptive
- May use more memory than necessary
- Fixed hierarchy

**Conclusion**: YAGNI principle - ship simple, optimize if needed.

### Why 4-Byte Voxel?

**Decision**: Pack voxel data into 4 bytes (MaterialID, Density, BiomeID, Metadata).

**Rationale**:
- 32³ chunk = 128 KB (cache-friendly)
- 256 materials and biomes sufficient
- 8-bit density enough for smooth terrain
- 8-bit metadata for AO, flags

**Trade-offs**:
- Limited to 256 material types
- Lower density precision than float
- Metadata byte is tight

**Conclusion**: Good balance of memory and capability.

---

## Next Steps

See [IMPLEMENTATION_PHASES.md](IMPLEMENTATION_PHASES.md) for development roadmap.

**Phase 1 (Current)**: Foundation
- Core data structures
- LOD interface and distance band implementation
- Hybrid rendering interface
- Chunk manager skeleton

**Phase 2**: Generation
- GPU noise library
- Infinite plane world mode
- Basic biome system
- Cubic meshing

**Phase 3**: Advanced Meshing
- Greedy meshing and AO
- Smooth meshing
- Custom Vertex Factory
- GPU-driven renderer

**Phase 4+**: World modes, editing, scatter, optimization

---

## References

- [LOD System Details](LOD_SYSTEM.md)
- [Smooth Meshing (Marching Cubes & Transvoxel)](SMOOTH_MESHING.md)
- [Rendering System Details](RENDERING_SYSTEM.md)
- [GPU Pipeline Details](GPU_PIPELINE.md)
- [Data Structures Reference](DATA_STRUCTURES.md)
- [World Modes Guide](WORLD_MODES.md)
- [Implementation Phases](IMPLEMENTATION_PHASES.md)
- [Coding Standards](CODING_STANDARDS.md)

---

**Architecture Version**: 2.0  
**Status**: Design Complete, Implementation In Progress
