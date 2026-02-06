# Implementation Phases

Development roadmap for the VoxelWorlds plugin.

## Phase 1: Foundation (Weeks 1-2) ✓ COMPLETE

**Goal**: Core architecture and interfaces

### Tasks
- [x] Architecture design
- [x] Documentation structure
- [x] Core data structures (FVoxelData, FChunkDescriptor)
- [x] IVoxelLODStrategy interface
- [x] IVoxelMeshRenderer interface
- [x] FDistanceBandLODStrategy implementation
- [x] Chunk manager skeleton (UVoxelChunkManager)
- [x] Module setup (.Build.cs files) - VoxelCore, VoxelLOD, VoxelRendering, VoxelStreaming

### Deliverables
- VoxelCore module with data structures
- VoxelLOD module with distance band LOD
- VoxelRendering module with interfaces
- Basic ChunkManager

### Success Criteria
- All interfaces compile
- Distance band LOD can determine visible chunks
- ChunkManager can track chunk state

---

## Phase 2: Generation & Basic Rendering (Weeks 3-4) ✓ COMPLETE

**Goal**: First visible terrain

### Tasks
- [x] GPU noise library (Perlin, Simplex)
- [x] Infinite plane world mode
- [x] Cubic meshing (face culling)
- [x] PMC renderer implementation
- [x] Basic biome system (3 biomes: Plains, Desert, Tundra)
- [x] Material registry (7 materials with vertex color visualization)
- [x] Chunk generation pipeline
- [x] Basic streaming

### Deliverables
- Working infinite plane world
- Visible cubic voxel terrain
- 3 biomes with different materials
- Chunk loading/unloading

### Success Criteria
- Can generate and render 100+ chunks ✓
- Frame time < 16ms with 100 chunks ✓
- No visible gaps between chunks ✓
- Materials display correctly ✓

---


## Phase 3: Advanced Meshing (Weeks 5-6) ✓ COMPLETE

**Goal**: Optimized rendering

### Tasks
- [x] Greedy meshing algorithm
- [x] Ambient occlusion calculation
- [x] Custom Vertex Factory → FLocalVertexFactory (see notes)
- [x] GPU-driven renderer (FVoxelCustomVFRenderer + FVoxelSceneProxy)
- [x] LOD morphing (Material Parameter Collection approach)

### Deliverables
- Optimized cubic meshing (50% fewer triangles) ✓
- Custom VF renderer using FLocalVertexFactory ✓
- LOD transitions via material-based MorphFactor ✓

### Success Criteria
- Can render 1000+ chunks at 60 FPS ✓
- Greedy meshing reduces tri count by 40%+ ✓
- LOD transitions are smooth ✓
- FLocalVertexFactory renderer stable and working ✓

### Notes on Vertex Factory Decision
After extensive research and implementation attempts with a fully custom FVoxelVertexFactory, the project switched to using Epic's **FLocalVertexFactory**. This decision was made because:
1. Custom vertex factory had persistent rendering issues (terrain attached to camera, translucent appearance, materials going blank)
2. FLocalVertexFactory is battle-tested and integrates reliably with UE5's rendering pipeline
3. LOD morphing is achieved via Material Parameter Collection instead of vertex shader uniforms
4. The tradeoff (slightly less optimal vertex format) is acceptable for stability

See **RENDERING_SYSTEM.md** for detailed documentation on the vertex factory architecture.

---

## Phase 4: Smooth Meshing (Weeks 7-8) ✓ COMPLETE

**Goal**: Marching Cubes terrain with LOD support

### Tasks
- [x] Marching Cubes algorithm (CPU implementation)
- [x] Trilinear interpolation for vertex positioning
- [x] Gradient-based normal calculation
- [x] LOD stride support (2^LODLevel stepping)
- [x] Neighbor chunk data for seamless boundaries
- [x] Skirt-based LOD seam hiding
- [x] LOD configuration gates (bEnableLOD, bEnableLODSeams)
- [x] Transvoxel transition cells (implemented, disabled by default)

### Deliverables
- Working smooth terrain meshing ✓
- LOD support with configurable seam handling ✓
- Configuration options for debugging ✓

### Success Criteria
- Smooth terrain renders correctly ✓
- LOD transitions functional ✓
- Seams can be hidden or disabled for debugging ✓

### Notes on LOD Seam Handling
Two approaches were implemented for LOD seam handling:

1. **Skirts** (Default): Vertical geometry strips along chunk boundaries that hide gaps between LOD levels. Simple and reliable but adds geometry.

2. **Transvoxel** (Disabled): Eric Lengyel's algorithm using 13-sample transition cells. Implemented but disabled due to complexity:
   - Requires face, edge, AND corner neighbor data for boundary cells
   - Subtle bugs with fractional sample positions at LOD 0
   - Edge cases at chunk corners need data from 7 neighbors
   - Future consideration: Octree-based LOD may be simpler

Configuration options in `UVoxelWorldConfiguration`:
- `bEnableLOD`: Master toggle, when false all chunks use LOD 0
- `bEnableLODSeams`: When false, disables all seam handling (skirts/Transvoxel)
- `bEnableLODMorphing`: Smooth vertex transitions between LOD levels

---

## Phase 5: World Modes (Weeks 9-10) ✓ COMPLETE

**Goal**: Multiple world types

### Tasks
- [x] Material atlas system
- [x] Spherical planet mode
- [x] Island/bowl mode
- [x] World mode-specific LOD culling
- [x] LOD material selection fix (smooth terrain)
- [x] Streaming performance optimizations
- [ ] Advanced biome blending (deferred)
- [ ] Height-based material assignment (deferred)
- [ ] Ocean/water level support (deferred)

### Deliverables
- 3 working world modes ✓
- Mode-specific chunk culling ✓
- Optimized streaming pipeline ✓

### Success Criteria
- All 3 world modes functional ✓
- Spherical planet has correct curvature ✓
- Island has smooth falloff ✓
- LOD transitions without material pop-in ✓

### Notes on Material Atlas System
The material atlas system was implemented with the following components:

1. **UVoxelMaterialAtlas Data Asset**:
   - Packed atlas textures for cubic meshing (UV-based sampling)
   - Auto-generated Texture2DArrays for smooth terrain (triplanar sampling)
   - Per-material configuration via FVoxelMaterialTextureConfig
   - Material Lookup Table (LUT) for face-variant texture mapping
   - BuildTextureArrays() for runtime texture array generation from source textures

2. **MaterialID Encoding**:
   - UV1.x channel stores MaterialID as float (avoids sRGB conversion issues)
   - UV1.y channel stores FaceType for cubic terrain (0=Top, 1=Side, 2=Bottom)
   - Shader extracts via `int MaterialID = (int)floor(MaterialUV.x + 0.5)`

3. **Unified Master Material (M_VoxelMaster)**:
   - Single material supporting both cubic and smooth terrain modes
   - 4 Material Functions: MF_GetMaterialID, MF_TriplanarSampleAlbedoRoughness, MF_TriplanarSampleNormal, MF_CubicAtlasSample
   - Automatic mode switching via Lerp nodes controlled by bSmoothTerrain parameter
   - Mode automatically synced from UVoxelWorldConfiguration::MeshingMode

4. **Smooth Terrain Path (Triplanar + Texture2DArrays)**:
   - Custom HLSL nodes in material functions
   - SampleLevel() instead of Sample() for ray tracing compatibility
   - UDN (Unreal Developer Network) blend for normal maps (eliminates plaid artifacts)
   - Blend weights: `pow(abs(WorldNormal), Sharpness)` normalized

5. **Cubic Terrain Path (UV-based + PackedAtlas)**:
   - MaterialLUT texture lookup for per-material, per-face atlas positions
   - UV0 provides local face coordinates, transformed to atlas space
   - Tangent-space normals converted to world-space via Transform node
   - Fallback handling for missing normal/roughness textures

6. **Runtime Material Binding**:
   - Dynamic Material Instance created from M_VoxelMaster
   - UVoxelWorldComponent::UpdateMaterialAtlasParameters() binds all textures
   - FVoxelCustomVFRenderer::Initialize() syncs mode from configuration
   - Texture arrays auto-built on first use if dirty

### Notes on World Modes Implementation

All three world modes are now fully implemented:

1. **Infinite Plane Mode (FInfinitePlaneWorldMode)**:
   - 2D heightmap terrain extending infinitely in X/Y
   - Configurable SeaLevel, HeightScale, BaseHeight
   - Terrain bounds culling: Chunks above/below terrain height range are skipped

2. **Island Bowl Mode (FIslandBowlWorldMode)**:
   - Bounded terrain with configurable edge falloff
   - Falloff types: Linear, Smooth (hermite), Squared, Exponential
   - Parameters: IslandRadius, FalloffWidth, IslandCenterX/Y, EdgeHeight, bBowlShape
   - Island boundary culling: Chunks beyond IslandRadius + FalloffWidth are skipped

3. **Spherical Planet Mode (FSphericalPlanetWorldMode)**:
   - Radial terrain on spherical surface
   - Cubic chunks with radial density calculation
   - Noise sampled via direction vector from planet center
   - Parameters: WorldRadius, PlanetMaxTerrainHeight, PlanetMaxTerrainDepth, PlanetSpawnLocation
   - Horizon culling: Chunks beyond geometric horizon are skipped
   - Shell culling: Inner core and outer space chunks are skipped
   - Spawn position helper: `GetPlanetSpawnPosition()` returns surface spawn location

### Notes on Mode-Specific LOD Culling

The LOD strategy now includes mode-specific culling optimizations:

1. **Terrain Bounds Culling (Infinite Plane)**:
   - `ShouldCullOutsideTerrainBounds()` skips chunks above/below terrain height range
   - Height range: SeaLevel + BaseHeight ± HeightScale with buffer

2. **Island Boundary Culling (Island Mode)**:
   - `ShouldCullIslandBoundary()` skips chunks beyond island extent
   - Extent = IslandRadius + FalloffWidth + chunk diagonal buffer

3. **Horizon/Shell Culling (Spherical Planet)**:
   - `ShouldCullBeyondHorizon()` skips chunks beyond geometric horizon
   - Horizon distance: √(2Rh + h²) where R = planet radius, h = viewer altitude
   - Inner shell: Chunks closer to center than WorldRadius - MaxTerrainDepth
   - Outer shell: Chunks farther than WorldRadius + MaxTerrainHeight

### Notes on Streaming Optimizations

The streaming system was significantly optimized:

1. **Queue Management (O(1) Duplicate Detection)**:
   - TSet tracking alongside queue arrays
   - `Algo::LowerBound()` for O(log n) sorted insertion
   - Queue growth capped at 2× processing rate per frame

2. **Separated Load/Unload Decisions**:
   - Load decisions: Only when viewer crosses chunk boundary
   - Unload decisions: Every frame (cheap operation, prevents orphaned chunks)
   - `UpdateLoadDecisions()` and `UpdateUnloadDecisions()` split from single method

3. **LOD Update Threshold**:
   - Skip LOD morph factor updates when viewer moved < 100 units
   - Cached positions: `CachedViewerChunk`, `LastStreamingUpdatePosition`, `LastLODUpdatePosition`

### Notes on LOD Material Selection Fix (Smooth Terrain)

Fixed material pop-in at LOD > 0 in smooth terrain mode:

**Problem**: At higher LOD levels, strided sampling misses thin surface material layer, showing underground materials that get "covered" when LOD 0 loads.

**Solution**: `GetDominantMaterialLOD()` scans upward from each solid corner to find actual surface:
```cpp
// For each solid corner in the cube:
// 1. Scan upward from corner position (max 8 voxels)
// 2. Track the last solid material before hitting air
// 3. Record the Z position of the surface transition
// 4. Use material from corner with highest surface Z

// This ensures grass (on surface) is selected over dirt (underground)
// even when LOD stride causes corners to sample below surface
```

Same approach applied to `GetDominantBiomeLOD()` for biome selection.

---

## Phase 6: Editing & Collision (Weeks 11-12)

**Goal**: Interactive terrain

### Tasks
- [ ] Edit layer implementation
- [ ] Add/subtract/paint tools
- [ ] Collision manager
- [ ] Async collision generation
- [ ] Edit serialization
- [ ] Undo/redo system

### Deliverables
- Working terrain editing
- Physics collision
- Save/load edits

### Success Criteria
- Can add/remove voxels in real-time
- Collision updates don't block rendering
- Edits persist across sessions
- Physics works correctly

---

## Phase 7: Scatter & Polish (Weeks 13-14)

**Goal**: Complete feature set

### Tasks
- [ ] Scatter system
- [ ] GPU-based vegetation placement
- [ ] HISM integration
- [ ] Foliage LOD
- [ ] Performance profiling
- [ ] Memory optimization
- [ ] Debug visualization tools

### Deliverables
- Working vegetation system
- Profiling tools
- Debug overlays

### Success Criteria
- Vegetation places correctly
- Performance targets met
- Memory within budget
- Debug tools are useful

---

## Phase 8: Advanced Features (Future)

**Optional enhancements based on profiling**

### Potential Features
- [ ] Quadtree LOD (if distance bands insufficient)
- [ ] Octree LOD (if needed for spherical)
- [ ] Advanced noise (Voronoi, Cellular)
- [ ] Weather/season system
- [ ] Multiplayer support
- [ ] Destructible terrain
- [ ] Cave generation
- [ ] Water simulation

---

## Current Status

**Active Phase**: Phase 6 (Editing & Collision)
**Progress**: Phase 1 COMPLETE - Phase 2 COMPLETE - Phase 3 COMPLETE - Phase 4 COMPLETE - Phase 5 COMPLETE

**Phase 1 Completed**:
1. ~~VoxelCore module~~ - Core data structures (FVoxelData, FChunkDescriptor, etc.)
2. ~~VoxelLOD module~~ - IVoxelLODStrategy + FDistanceBandLODStrategy
3. ~~VoxelRendering module~~ - IVoxelMeshRenderer interface
4. ~~VoxelStreaming module~~ - UVoxelChunkManager skeleton

**Phase 2 Completed**:
1. ~~GPU noise library~~ - CPU and GPU noise generators with Perlin/Simplex FBM (VoxelGeneration module)
   - FVoxelCPUNoiseGenerator: CPU-based noise for fallback and single-point sampling
   - FVoxelGPUNoiseGenerator: RDG-based compute shader noise generation
   - Automation tests passing (CPUNoiseGenerator, GPUNoiseGeneratorAsync, GPUvsCPUConsistency, Performance)

2. ~~Infinite plane world mode~~ - 2D heightmap terrain with SDF-based generation
   - IVoxelWorldMode interface: Abstract base for all world modes
   - FInfinitePlaneWorldMode: 2D heightmap extending infinitely in X/Y, Z as height
   - WorldModeSDF.ush: GPU shader functions for terrain SDF calculation
   - Terrain params: SeaLevel, HeightScale, BaseHeight (configurable in UVoxelWorldConfiguration)
   - Automation tests passing (Basic, Density, CPUGeneration, Coordinates, GPUConsistency, Materials)

3. ~~Cubic meshing system~~ - Face culling mesh generation (VoxelMeshing module)
   - IVoxelMesher interface: Abstract meshing interface with sync/async support
   - FVoxelCPUCubicMesher: CPU-based face culling algorithm
   - FVoxelGPUCubicMesher: RDG-based compute shader with atomic counters
   - CubicMeshGeneration.usf: GPU compute shader for mesh generation
   - CubicMeshCommon.ush: Shared face tables, vertex helpers, packing functions
   - Neighbor chunk support: Seamless boundaries via edge slice data
   - 28-byte FVoxelVertex format: Packed normals, UVs, material data
   - Performance: CPU ~0.65ms, GPU ~2.89ms for 32³ terrain chunks
   - Automation tests passing (EmptyChunk, SingleVoxel, FaceCulling, FullChunk, GPUAsync, GPUReadback, CPUvsGPU, Performance, ChunkBoundary)

4. ~~PMC renderer~~ - ProceduralMeshComponent-based renderer (VoxelRendering module)
   - FVoxelPMCRenderer: Full IVoxelMeshRenderer implementation
   - AVoxelPMCContainerActor: Transient actor holding all PMC instances
   - Component pooling: Reusable PMC components to reduce allocations
   - Data conversion: FChunkMeshData (FVector3f, uint32) to PMC format (FVector, int32)
   - Automatic tangent generation from normals
   - Statistics tracking: Vertex/triangle counts, memory usage
   - Collision mesh support via bGenerateCollision config
   - Dynamic vertex color material for biome visualization

5. ~~Biome & Material System~~ - Climate-based biome selection with material registry (VoxelCore module)
   - FVoxelMaterialDefinition: Material struct with ID, Name, Color
   - FVoxelMaterialRegistry: Static registry with 7 materials
     - Grass (Forest Green), Dirt (Brown), Stone (Gray)
     - Sand (Tan), Snow (White), Sandstone (Dark Tan), Frozen Dirt (Gray-Blue)
   - FBiomeDefinition: Biome struct with temperature/moisture ranges, material assignments
   - FVoxelBiomeRegistry: Static registry with 3 biomes
     - Plains: Temperature -0.3 to 0.6, Grass/Dirt/Stone
     - Desert: Temperature 0.5 to 1.0, dry, Sand/Sandstone/Stone
     - Tundra: Temperature -1.0 to -0.3, Snow/Frozen Dirt/Stone
   - Temperature/moisture noise sampling in VoxelCPUNoiseGenerator
   - Vertex color output with AO support in VoxelCPUCubicMesher

6. ~~Chunk Streaming Pipeline~~ - Full generation and rendering pipeline
   - AVoxelWorldTestActor: Test actor for runtime voxel world instantiation
   - UVoxelChunkManager: Coordinates chunk states (Unloaded → Generating → Meshing → Ready)
   - Distance-based chunk loading via FDistanceBandLODStrategy
   - Unload distance multiplier for hysteresis
   - Debug visualization and statistics output

**Phase 3 Completed**:
1. ~~Greedy meshing algorithm~~ - COMPLETE
   - Slice-based face processing with 2D masks
   - Rectangle merging for same-material adjacent faces
   - Configurable via `bUseGreedyMeshing` in FVoxelMeshingConfig
   - 40-60% triangle reduction achieved
   - Chunk boundary fixes:
     - Fixed neighbor data extraction (now uses any chunk with valid voxel data, not just Loaded state)
     - Added automatic neighbor remeshing when chunks finish generating
     - Seamless boundaries regardless of chunk load order

2. ~~Per-vertex ambient occlusion~~ - COMPLETE
   - Standard voxel AO algorithm checking side1, side2, and corner neighbors per vertex
   - Static lookup table (AONeighborOffsets[6][4][3]) for all face/vertex combinations
   - CalculateVertexAO(): Returns 0-3 occlusion level per vertex
   - CalculateFaceAO(): Computes all 4 vertices of a face
   - Brightness mapping: AO 0 = 100%, AO 3 = 25% (formula: 1.0 - AO * 0.25)
   - Works with both simple and greedy meshing modes
   - Cross-chunk boundary support via GetVoxelAt() neighbor lookups
   - Diagonal neighbor lookups (multi-axis out-of-bounds) treated as air
   - Enabled by default via `bCalculateAO = true` in FVoxelMeshingConfig

3. ~~FLocalVertexFactory Renderer~~ - COMPLETE
   - FVoxelCustomVFRenderer: GPU-driven renderer using FLocalVertexFactory
   - FVoxelSceneProxy: Scene proxy with per-chunk frustum culling
   - UVoxelWorldComponent: Primitive component bridge between game/render threads
   - FVoxelLocalVertex: 32-byte vertex format compatible with FLocalVertexFactory
   - Vertex color encoding: R=MaterialID, G=BiomeID, B=AO<<6, A=255
   - Debug visualization modes for MaterialID and BiomeID colors
   - ViewProjectionMatrix-based frustum culling

4. ~~LOD MorphFactor~~ - COMPLETE
   - Material Parameter Collection (MPC) approach for per-pixel LOD morphing
   - MPC parameters: LODStartDistance, LODEndDistance, LODInvRange
   - Material calculates MorphFactor based on camera distance
   - Full pipeline: VoxelWorldTestActor → Renderer → WorldComponent → MPC → Material
   - Configurable via Details panel on VoxelWorldTestActor

**Phase 4 Completed**:
1. ~~Smooth meshing (Marching Cubes)~~ - COMPLETE
   - FVoxelCPUSmoothMesher: CPU-based Marching Cubes implementation
   - 256-case lookup table for cube configurations
   - Trilinear interpolation for vertex positioning on edges
   - Gradient-based normal calculation using central differences
   - LOD stride support: 2^LODLevel voxel stepping for lower detail levels
   - Neighbor chunk data support for seamless boundaries
   - Skirt generation for LOD seam hiding (configurable)
   - Transvoxel transition cells (implemented but disabled by default)

2. ~~LOD Configuration Gates~~ - COMPLETE
   - `bEnableLOD`: Master toggle in UVoxelWorldConfiguration
   - `bEnableLODSeams`: Disables all seam handling when false
   - `bUseTransvoxel`: Per-mesher toggle (default: false)
   - `bGenerateSkirts`: Fallback seam handling (default: true)

**Phase 5 Completed**:
1. ~~Material atlas system~~ - COMPLETE
   - UVoxelMaterialAtlas: Data asset with packed atlases and auto-built Texture2DArrays
   - FVoxelMaterialTextureConfig: Per-material configuration (source textures, face variants)
   - MaterialID stored in UV1.x channel, FaceType in UV1.y (float encoding, sRGB-safe)
   - M_VoxelMaster: Unified material with automatic cubic/smooth mode switching
   - 4 Material Functions: MF_GetMaterialID, MF_TriplanarSampleAlbedoRoughness, MF_TriplanarSampleNormal, MF_CubicAtlasSample
   - Smooth path: Triplanar sampling with Texture2DArrays, UDN normal blending
   - Cubic path: UV-based atlas sampling with MaterialLUT lookup
   - Automatic mode sync from UVoxelWorldConfiguration::MeshingMode
   - Runtime binding via DynamicMaterialInstance in UVoxelWorldComponent
   - VoxelTriplanarCommon.ush: Shared triplanar utility functions

2. ~~World Modes~~ - COMPLETE
   - FInfinitePlaneWorldMode: 2D heightmap, infinite X/Y, terrain bounds culling
   - FIslandBowlWorldMode: Bounded terrain with configurable falloff (4 types)
   - FSphericalPlanetWorldMode: Radial terrain, horizon/shell culling, spawn position helper

3. ~~Mode-Specific LOD Culling~~ - COMPLETE
   - Terrain bounds culling for Infinite Plane
   - Island boundary culling for Island/Bowl mode
   - Horizon + shell culling for Spherical Planet

4. ~~Streaming Optimizations~~ - COMPLETE
   - O(1) duplicate detection with TSet tracking
   - Separated load/unload decisions (unload every frame)
   - Position-based LOD update threshold

5. ~~LOD Material Selection Fix~~ - COMPLETE
   - Upward surface scanning for smooth terrain at LOD > 0
   - Prevents underground material pop-in

**Next Immediate Steps** (Phase 6):
1. Edit layer implementation
2. Add/subtract/paint tools

---

## Development Principles

Throughout all phases:
- **Test early and often**
- **Profile before optimizing**
- **Document as you go**
- **Keep modules decoupled**
- **GPU-first approach**
- **Measure performance targets**

---

## Performance Milestones

### Phase 2 Targets
- 100 visible chunks
- 60 FPS
- < 100 MB memory

### Phase 3 Targets
- 500 visible chunks  
- 60 FPS
- < 150 MB memory

### Phase 7 Targets (Final)
- 1000+ visible chunks
- 60 FPS
- < 250 MB memory
- < 5ms GPU compute per frame
- 4 chunks generated/meshed per frame

