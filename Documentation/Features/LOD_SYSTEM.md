# LOD System Architecture

**Module**: VoxelLOD  
**Dependencies**: VoxelCore

## Table of Contents

1. [Overview](#overview)
2. [Interface Design](#interface-design)
3. [Distance Band Strategy](#distance-band-strategy)
4. [Future Strategies](#future-strategies)
5. [Integration with ChunkManager](#integration-with-chunkmanager)
6. [LOD Morphing](#lod-morphing)
7. [Performance Considerations](#performance-considerations)
8. [Implementation Guide](#implementation-guide)

---

## Overview

The LOD (Level of Detail) system determines which chunks should be loaded and at what detail level based on viewer position and other criteria. The system is **pluggable** via the `IVoxelLODStrategy` interface, allowing different LOD implementations to be swapped without modifying core systems.

### Design Goals

- **Pluggable**: Easy to swap strategies without refactoring
- **Flexible**: Support multiple world modes and use cases
- **Performant**: Fast queries (called frequently)
- **Predictable**: Clear memory and performance budgets
- **Extensible**: Can add new strategies later

### Current Implementations

1. **FDistanceBandLODStrategy** (Default) - Distance-based LOD rings
2. **FQuadtreeLODStrategy** (Future) - Screen-space adaptive for 2D terrain
3. **FOctreeLODStrategy** (Future) - 3D adaptive for spherical/cave systems

---

## Interface Design

### IVoxelLODStrategy

```cpp
/**
 * LOD Strategy Interface
 * 
 * All LOD implementations must conform to this interface. Strategies determine
 * which chunks should be loaded/rendered at what detail level.
 * 
 * Performance: GetLODForChunk() is called frequently and must be fast (< 1μs).
 * Thread Safety: All const methods must be thread-safe.
 */
class IVoxelLODStrategy {
public:
    virtual ~IVoxelLODStrategy() = default;
    
    // ==================== Core Queries ====================
    
    /**
     * Get LOD level for a chunk at given coordinate.
     * @param ChunkCoord Chunk position in chunk space
     * @param Context Query context with viewer state
     * @return LOD level (0 = finest detail, higher = coarser)
     */
    virtual int32 GetLODForChunk(
        FIntVector ChunkCoord, 
        const FLODQueryContext& Context
    ) const = 0;
    
    /**
     * Get the LOD level of the distance band a chunk falls in, even with the LOD
     * system off (then GetLODForChunk is 0 everywhere). Used by systems keyed on
     * distance rather than detail: far-chunk compression. See "LOD Off and Cubic Meshing".
     * Default: GetLODForChunk.
     */
    virtual int32 GetDistanceBandLOD(
        FIntVector ChunkCoord, 
        const FLODQueryContext& Context
    ) const;
    
    /**
     * Get morph factor for LOD transition blending.
     * @param ChunkCoord Chunk position in chunk space
     * @param Context Query context with viewer state
     * @return Morph factor 0-1 (0 = current LOD, 1 = next LOD)
     */
    virtual float GetLODMorphFactor(
        FIntVector ChunkCoord, 
        const FLODQueryContext& Context
    ) const = 0;
    
    // ==================== Visibility & Streaming ====================
    
    /**
     * Get all chunks that should be visible this frame.
     * @param Context Query context with viewer state
     * @return Array of chunk requests with LOD and priority
     */
    virtual TArray<FChunkLODRequest> GetVisibleChunks(
        const FLODQueryContext& Context
    ) const = 0;
    
    /**
     * Get chunks that need to be loaded (not currently loaded).
     * @param OutLoad Output array of chunks to load
     * @param Context Query context with viewer state
     */
    virtual void GetChunksToLoad(
        TArray<FChunkLODRequest>& OutLoad, 
        const FLODQueryContext& Context
    ) const = 0;
    
    /**
     * Get EVERY chunk that is out of range, farthest first. No per-frame cap here: the chunk
     * manager vetoes candidates it must keep (mid-pipeline, edit-pinned) and only then applies
     * MaxChunksToUnloadPerFrame via SelectUnloadCandidates, so refused chunks never use up the
     * budget (R23d, VOXEL_QA_FOLLOWUPS_PLAN.md).
     * @param OutUnload Output array of chunks to unload
     * @param Context Query context with viewer state
     */
    virtual void GetChunksToUnload(
        TArray<FIntVector>& OutUnload, 
        const FLODQueryContext& Context
    ) const = 0;

    /** Veto first, cap second: pick this frame's unloads from a farthest-first candidate list. */
    static int32 SelectUnloadCandidates(const TArray<FIntVector>& Candidates,
        TFunctionRef<bool(const FIntVector&)> CanUnload, int32 MaxPerFrame, TArray<FIntVector>& OutSelected);

    /** Unload horizon (ViewDistance * UnloadDistanceMultiplier for distance bands; -1 = none). */
    virtual float GetUnloadDistance() const;

    /** Load horizon (the view distance; -1 = none). Edit-pinned chunks pin-load within it and stay
     *  resident out to GetUnloadDistance(), so they never appear alone past the terrain edge. */
    virtual float GetLoadDistance() const;

    /** The distance both horizons are measured in, viewer to chunk centre. Default 3D; distance
     *  bands: horizontal in heightmap worlds (see "The Vertical Window"). The chunk manager's queue
     *  eviction, stale-cull, edit pins and residency audit all use it, so nothing disagrees with the
     *  strategy's own unload rule (KI11 in WORLD_MODES_REVIEW_PLAN.md). */
    virtual float GetHorizonDistance(const FVector& ChunkCenter, const FVector& ViewerPosition) const;
    
    // ==================== Lifecycle ====================
    
    /**
     * Initialize strategy from world configuration.
     * Called once at world creation.
     */
    virtual void Initialize(const FVoxelWorldConfiguration& WorldConfig) = 0;
    
    /**
     * Update strategy state.
     * Called every frame from game thread.
     * @param Context Query context with viewer state
     * @param DeltaTime Time since last update
     */
    virtual void Update(const FLODQueryContext& Context, float DeltaTime) = 0;
    
    // ==================== Optional Optimization ====================
    
    /**
     * Should this chunk be updated this frame? (Throttling)
     * Default: Always update
     */
    virtual bool ShouldUpdateChunk(
        FIntVector ChunkCoord, 
        const FLODQueryContext& Context
    ) const {
        return true;
    }
    
    /**
     * Get priority for chunk generation (higher = more important).
     * Used for sorting generation queue.
     * Default: Inverse distance
     */
    virtual float GetChunkPriority(
        FIntVector ChunkCoord, 
        const FLODQueryContext& Context
    ) const {
        return 1.0f;
    }
    
    // ==================== Debugging ====================
    
    /**
     * Get debug information string.
     * Used for on-screen display and logging.
     */
    virtual FString GetDebugInfo() const = 0;
    
    /**
     * Draw debug visualization in viewport.
     * Optional: Draw LOD zones, chunk bounds, etc.
     */
    virtual void DrawDebugVisualization(
        UWorld* World, 
        const FLODQueryContext& Context
    ) const {}
};
```

### FLODQueryContext

```cpp
/**
 * LOD Query Context
 * 
 * Encapsulates all viewer and world state needed for LOD queries.
 * Passed to all LOD strategy methods.
 */
struct FLODQueryContext {
    // ==================== Camera/Viewer State ====================
    
    /** Current viewer (camera) position in world space */
    FVector ViewerPosition;
    
    /** Viewer forward direction (normalized) */
    FVector ViewerForward;

    // ViewerRight, ViewerUp, ViewDistance, AspectRatio, WorldRadius, MaxChunksToLoadPerFrame, TimeSliceMS,
    // FrameNumber, GameTime, DeltaTime deleted in CR1 (STREAM-11)
    
    /** Field of view in degrees */
    float FieldOfView;
    
    /** Frustum planes for culling (optional) */
    TArray<FPlane> FrustumPlanes;
    
    // ==================== World State ====================
    
    /** World origin (for spherical: planet center; bowl: island center) */
    FVector WorldOrigin;
    
    /** World mode (Infinite, Spherical, Island) */
    EWorldMode WorldMode;
    
    // ==================== Performance Budgets ====================
    
    /** Maximum chunks to unload per frame */
    int32 MaxChunksToUnloadPerFrame = 8;
    
    // ==================== Optional Extended Data ====================
    
    /** Biome system reference (for biome-aware LOD) */
    const FBiomeSystem* BiomeSystem = nullptr;
    
    /** Edit layer reference (for edit-aware LOD) */
    const FEditLayer* EditLayer = nullptr;
};
```

### FChunkLODRequest

```cpp
/**
 * Chunk LOD Request
 * 
 * Request to load/render a specific chunk at a specific LOD level.
 * Used for streaming and rendering decisions.
 */
struct FChunkLODRequest {
    /** Chunk coordinate in chunk space */
    FIntVector ChunkCoord;
    
    /** LOD level to load (0 = finest) */
    int32 LODLevel;
    
    /** Priority for sorting (higher = more important) */
    float Priority;
    
    /** Morph factor for LOD transitions (0-1) */
    float MorphFactor;
    
    /** Comparison operator for priority queue (descending order) */
    bool operator<(const FChunkLODRequest& Other) const {
        return Priority > Other.Priority;
    }
};
```

---

## Distance Band Strategy

The **FDistanceBandLODStrategy** is the default LOD implementation. It uses concentric distance rings around the viewer to determine LOD levels.

### Configuration

```cpp
class FDistanceBandLODStrategy : public IVoxelLODStrategy {
public:
    /** LOD band configuration */
    struct FLODBand {
        float MinDistance;      // Meters from viewer
        float MaxDistance;      // Meters from viewer
        int32 LODLevel;         // 0 = finest detail
        int32 VoxelStride;      // Sampling stride (1, 2, 4, 8, ...)
        int32 ChunkSize;        // Voxels per chunk edge
        float MorphRange;       // Distance to blend to next LOD
    };
    
    /** LOD bands (sorted by distance) */
    TArray<FLODBand> LODBands;
    
    /** Enable LOD morphing? */
    bool bEnableMorphing = true;
    
    /** Enable frustum culling? */
    bool bViewFrustumCulling = true;
};
```

### Example Configurations

**Infinite Plane:**
```cpp
LODBands = {
    { 0.0f,    512.0f,  0, 1,  32,  64.0f  },  // Close: Full detail
    { 512.0f,  1024.0f, 1, 2,  32,  64.0f  },  // Mid: Half detail
    { 1024.0f, 2048.0f, 2, 4,  32,  128.0f },  // Far: Quarter detail
    { 2048.0f, 4096.0f, 3, 8,  64,  256.0f },  // Very far: Coarse, big chunks
    { 4096.0f, 8192.0f, 4, 16, 128, 512.0f },  // Horizon: Very coarse
};
```

**Spherical Planet:** the same bands as any world (the test planets use the demo's), measured in 3D from the viewer
(see "The Window and Culling (Spherical Planet)" below).

**Island/Bowl:**
```cpp
LODBands = {
    { 0.0f,    256.0f,  0, 1,  32,  32.0f  },  // High detail island
    { 256.0f,  512.0f,  1, 2,  32,  32.0f  },
    { 512.0f,  1024.0f, 2, 4,  32,  64.0f  },
    { 1024.0f, 2048.0f, 3, 8,  64,  128.0f },
};
```

### Implementation

```cpp
int32 FDistanceBandLODStrategy::GetLODForChunk(
    FIntVector ChunkCoord, 
    const FLODQueryContext& Context) const
{
    // Convert chunk coord to world position
    FVector ChunkCenter = ChunkCoordToWorldPosition(ChunkCoord, Context);
    
    // Calculate distance to viewer
    float Distance = GetDistanceToViewer(ChunkCenter, Context);
    
    // Find matching band
    for (const FLODBand& Band : LODBands) {
        if (Distance >= Band.MinDistance && Distance < Band.MaxDistance) {
            return Band.LODLevel;
        }
    }
    
    // Beyond all bands - use coarsest LOD
    return LODBands.Last().LODLevel;
}

float FDistanceBandLODStrategy::GetLODMorphFactor(
    FIntVector ChunkCoord, 
    const FLODQueryContext& Context) const
{
    if (!bEnableMorphing) return 0.0f;
    
    FVector ChunkCenter = ChunkCoordToWorldPosition(ChunkCoord, Context);
    float Distance = GetDistanceToViewer(ChunkCenter, Context);
    int32 CurrentLOD = GetLODForChunk(ChunkCoord, Context);
    
    if (CurrentLOD >= LODBands.Num() - 1) {
        return 0.0f; // No transition at max LOD
    }
    
    const FLODBand& Band = LODBands[CurrentLOD];
    float TransitionStart = Band.MaxDistance - Band.MorphRange;
    
    if (Distance > TransitionStart) {
        // In transition zone
        return FMath::Clamp(
            (Distance - TransitionStart) / Band.MorphRange, 
            0.0f, 
            1.0f
        );
    }
    
    return 0.0f;
}
```

### Visible Chunks Algorithm

```cpp
TArray<FChunkLODRequest> FDistanceBandLODStrategy::GetVisibleChunks(
    const FLODQueryContext& Context) const
{
    TArray<FChunkLODRequest> Requests;
    
    // Iterate through each LOD band
    for (const FLODBand& Band : LODBands) {
        int32 ChunkWorldSize = Band.ChunkSize * GetVoxelSize();
        int32 ChunkRadius = FMath::CeilToInt(Band.MaxDistance / ChunkWorldSize);
        
        FIntVector ViewerChunk = WorldPositionToChunkCoord(
            Context.ViewerPosition, 
            ChunkWorldSize
        );
        
        // Grid iteration within this band's range
        for (int32 x = -ChunkRadius; x <= ChunkRadius; ++x) {
            for (int32 y = -ChunkRadius; y <= ChunkRadius; ++y) {
                for (int32 z = GetMinZ(Context); z <= GetMaxZ(Context); ++z) {
                    FIntVector ChunkCoord = ViewerChunk + FIntVector(x, y, z);
                    
                    // Distance check (is this chunk in this band?)
                    FVector ChunkCenter = ChunkCoordToWorldPosition(ChunkCoord, Context);
                    float Distance = GetDistanceToViewer(ChunkCenter, Context);
                    
                    if (Distance < Band.MinDistance || Distance >= Band.MaxDistance) {
                        continue; // Not in this band
                    }
                    
                    // Frustum culling (optional)
                    if (bViewFrustumCulling && !IsChunkInFrustum(ChunkCoord, Context)) {
                        continue;
                    }
                    
                    // Create request
                    FChunkLODRequest Request;
                    Request.ChunkCoord = ChunkCoord;
                    Request.LODLevel = Band.LODLevel;
                    Request.Priority = CalculatePriority(ChunkCoord, Context);
                    Request.MorphFactor = GetLODMorphFactor(ChunkCoord, Context);
                    
                    Requests.Add(Request);
                }
            }
        }
    }
    
    // Sort by priority
    Requests.Sort();
    
    return Requests;
}
```

### Priority Calculation

```cpp
float FDistanceBandLODStrategy::CalculatePriority(
    FIntVector ChunkCoord, 
    const FLODQueryContext& Context) const
{
    FVector ChunkCenter = ChunkCoordToWorldPosition(ChunkCoord, Context);
    float Distance = GetDistanceToViewer(ChunkCenter, Context);
    
    // Base priority: inverse distance (closer = higher priority)
    float Priority = 1.0f / FMath::Max(Distance, 1.0f);
    
    // Boost for chunks in view direction
    FVector ToChunk = (ChunkCenter - Context.ViewerPosition).GetSafeNormal();
    float DotProduct = FVector::DotProduct(ToChunk, Context.ViewerForward);
    
    if (DotProduct > 0) {
        // Forward chunks get up to 2x priority boost
        Priority *= (1.0f + DotProduct);
    }
    
    return Priority;
}
```

### Distance Calculation

Two distances, for two questions:

```cpp
// How detailed: the LOD band. 3D in every world mode, so deep and high chunks fall into farther
// bands (and compress) even under the viewer's feet.
float FDistanceBandLODStrategy::GetDistanceToViewer(FVector Position, const FLODQueryContext& Context) const
{
    return FVector::Distance(Position, Context.ViewerPosition);
}

// Whether in range at all: the load and unload horizons. Horizontal in heightmap worlds, where each
// column's vertical extent is its window; along the ground on a planet, where each chunk's slab is.
float FDistanceBandLODStrategy::GetHorizonDistance(const FVector& ChunkCenter, const FVector& ViewerPosition) const
{
    if (IsHeightmapWorld())
    {
        return FVector::Dist2D(ChunkCenter, ViewerPosition);
    }
    // The arc between the two directions from the centre, at the radius of the terrain under the viewer.
    return GetPlanetGroundDistance(PlanetCenter, GetPlanetGroundRadius(ViewerPosition), ViewerPosition, ChunkCenter);
}
```

Islands measured both in 2D until the world modes review's M4 (WM-S2): whole columns sat in the near bands,
uncompressed (KI13), and the residency audit, which measured 3D, reported chunks the island's 2D rule kept
(KI11). InfinitePlane measured both in 3D, so a viewer high above the terrain lost the ground at the edge of the view
and, from 40,000 uu up, all of it. With the horizon horizontal the ground under any viewer stays in range (decision D8);
the LOD distance keeps the far-away and the far-below coarse.

A planet's LOD distance stays Euclidean (3D, SP-C3): near the viewer it matches the distance along the surface. Its
horizon has been the distance along the ground since spherical Phase 6 (WM-BP8, WORLD_MODES_REVIEW_PLAN.md §20.4): with a
3D horizon a viewer about the view distance above the ground loaded nothing, the planet's version of what D8 fixed for
heightmaps.

### LOD Off and Cubic Meshing

`bEnableLOD` off turns the LOD system off: every chunk is LOD 0 (full detail), and there are no LOD
re-meshes, crossfades, 2:1 balance or seam holds. Read the effective value through
`UVoxelWorldConfiguration::IsLODEnabled()`, not the property: **cubic meshing always runs with LOD
off**, whatever `bEnableLOD` says (decision D7 in
[WORLD_MODES_REVIEW_PLAN.md](../WORLD_MODES_REVIEW_PLAN.md) §8). The cubic mesher never reads the
LOD level, so LOD changes would only re-mesh and crossfade identical geometry; greedy meshing is
cubic's answer to distance instead. The chunk manager logs `LOD system disabled for cubic mode` once
at initialisation when a cubic config has `bEnableLOD` on. The asset is not modified, so the same
config meshed smooth keeps its LOD.

With LOD off the bands stay in use as a distance table, for the two systems keyed on *how far* a
chunk is rather than *how coarse* it is:

| System | Rule | With LOD on | With LOD off |
|--------|------|-------------|--------------|
| Near window (heightmap worlds, see "The Vertical Window") | Within band 0's `MaxDistance` plus two chunk widths, a column also loads the viewer's layer ± 2 | Same | Same: keyed on band 0's distance, not the LOD level |
| Far-chunk compression (`voxel.FarCompression.*`) | A settled, idle chunk is compressed once its distance band's stride (`1 << LODLevel`) reaches `voxel.FarCompression.MinStride` (default 4) | The band is the chunk's LOD level (distance plus hysteresis and 2:1 balance), as before | The band its distance falls in, from `GetDistanceBandLOD` (no hysteresis) |

So with the demo bands (stride 1 to 7,000 uu, stride 2 to 14,000 uu, stride 4 beyond) and the default
`MinStride` of 4, chunks more than 14,000 uu away compress in both modes. A one-band config (band 0 out to
the view distance) puts every column in the near window and, with `MinStride` above 1, compresses nothing.

---

## Mode-Specific Culling

The LOD strategy includes mode-specific culling optimizations that skip chunks guaranteed to be empty or invisible, significantly reducing the number of chunks loaded.

### The Vertical Window (Infinite Plane & Island/Bowl)

Heightmap worlds take their vertical streaming range from the terrain, not from the viewer's altitude (decision D8
in [WORLD_MODES_REVIEW_PLAN.md](../WORLD_MODES_REVIEW_PLAN.md) §8, work item WM-S1). Each chunk column has its own window
of layers (`FDistanceBandLODStrategy::GetColumnWindow`):

| Part | Layers | Where |
|------|--------|-------|
| Surface slab | The world mode's surface over the column's footprint (`IVoxelWorldMode::GetSurfaceRange`, five height samples), ± 1.5 chunks, extended up to the water plane | Every column within the horizon, at every distance |
| Near window | The viewer's own layer ± 2 (`NearWindowLayers`) | Columns within band 0's `MaxDistance` + two chunk widths, horizontally (`GetNearWindowRadius`: 16,600 uu on the demo) |
| Height bounds | Both parts are clipped to the world mode's `GetTerrainHeightBounds` ± one chunk | Everywhere |

So every column streams its own ground whatever the viewer's altitude: a viewer flying 7,200 uu up or a camera
48,000 uu up keeps the terrain below it, and the viewer's altitude only adds layers near it (caves, dungeons, digging
below the surface, a tower above it). The slab trims each column to the 4-5 layers around its surface; the demo's full height
range is 8. Cave interiors and edits deeper than the slab load only near the viewer; edited chunks
are also pin-loaded by the chunk manager within the view distance (dungeon stamps are edits).

**Unloading uses a wider keep window** (`EWindowUse::Keep`): the slab, or the whole height range in columns within the
near radius. Nothing the near window loaded unloads while the viewer changes layer (walking across a
layer boundary, flying up); the extra layers go once their column leaves the near radius. The
balanced-LOD cache covers the keep window, so every chunk the strategy keeps has a balanced LOD.

With `bFarBandSurfaceSlabCulling` off every column streams its whole height range (the name predates D8, when the slab
applied beyond band 0 only). Until M4 the window was the viewer's layer −2 to +8 (island −2 to +4), with the slab on
InfinitePlane beyond band 0 only: a viewer above layer 0 lost the ground below it (G13), and islands kept every layer
of every column (KI5). A planet streams by chunk with the same rules on a sphere (each chunk's surface slab, the
viewer's altitude ± 2 chunks near it, the horizon along the ground; spherical Phase 6, WM-BP8), below.

**Height Range Calculation**: `IVoxelWorldMode::GetTerrainHeightBounds` (the full noise amplitude and continentalness;
for an island, also the floor beyond its edge), plus one chunk of buffer on each side.

### Island Boundary Culling (Island/Bowl Mode)

For island mode, chunks beyond the island extent are skipped:

```cpp
bool FDistanceBandLODStrategy::ShouldCullIslandBoundary(
    const FIntVector& ChunkCoord,
    const FLODQueryContext& Context) const
{
    if (WorldMode != EWorldMode::IslandBowl) return false;

    // The column's box in X and Y, against the island's footprint (IVoxelWorldMode::GetWorldBounds): a circle or a
    // rectangle R + F around WorldOrigin + the configured offset. The map's IsTileInsideWorld uses the same test.
    const FVector ChunkCenter = ChunkCoordToWorldCenter(ChunkCoord);
    const float Half = BaseChunkSize * VoxelSize * 0.5f;
    const FBox2D Column(FVector2D(ChunkCenter.X - Half, ChunkCenter.Y - Half),
                        FVector2D(ChunkCenter.X + Half, ChunkCenter.Y + Half));
    return !IslandBounds.IntersectsBox2D(Column);
}
```

**Island Extent**: `IslandRadius + FalloffWidth`. Until M6 (WORLD_MODES_REVIEW_PLAN.md §14.10) a circle kept every chunk
whose centre was within the extent plus a chunk diagonal: a ring of columns wholly past the edge, holding the old floor and
water there. The far ocean draws the sea past what streams now (WORLD_MODES.md, "Beyond the edge").

### The Window and Culling (Spherical Planet)

A planet streams by chunk, with the heightmap rules of D8 on a sphere (spherical Phase 6, WM-BP8;
WORLD_MODES_REVIEW_PLAN.md §20.4). `FDistanceBandLODStrategy::IsInPlanetWindow` decides each chunk:

- **The horizon, along the ground** (`GetHorizonDistance` → `GetPlanetGroundDistance`): the arc between the viewer's
  direction from the planet centre and the chunk centre's, on the sphere through the viewer's ground point (the analytic
  terrain under the viewer, `GetPlanetGroundRadius`, one sample when the viewer moves). A cone around the viewer's
  up-line, as a heightmap world's horizon is a cylinder: a chunk loads within the view distance of it and unloads past it
  × `UnloadDistanceMultiplier`, whatever the viewer's altitude.
- **The shell**: the terrain's real bounds (`FSphericalPlanetWorldMode::GetDisplacementBounds`, noise at continentalness's
  extremes, within the configured `PlanetMaxTerrainHeight` / `PlanetMaxTerrainDepth`) widened by `ShellSlackChunks` (½):
  `ShellWindowInnerRadius` to `ShellWindowOuterRadius`. Nothing outside it holds terrain.
- **The surface slab** (`GetPlanetChunkSlab`): a chunk loads when its radial extent (the nearest point and the farthest
  corner of its box from the centre) meets the surface's altitude range over the cube-sphere cells its eight corner
  directions span (`FVoxelCubeSphereGrid::GatherSpanned`), ± `SlabSlackChunks` (1.5). The cells are about a chunk across
  on the datum sphere (`CellsPerEdgeFor(R, chunk size)`: 82 a face edge on a 2.5 km planet, 32,725 on a 1,000 km one), and
  a cell's range is five terrain samples, its corners and its centre, as a heightmap column's five heights. Cells and
  chunks are memoized (each cache starts over past a million entries).
- **Near the viewer** (within `GetNearWindowRadius` along the ground, band 0 + two chunks): off its slab a chunk also
  loads within the viewer's altitude ± `NearWindowLayers` chunk widths, inside the shell, up to the band's ceiling (none
  for a viewer above it), and stays while it is within the unload distance of the viewer (3D), so a viewer changing
  altitude unloads nothing near it. As a heightmap world streams the viewer's layer ± 2 within its height bounds: caves,
  dungeons and digging below the slab, and the chunks around a viewer near the ground or at the sea's surface (a flyer
  skimming the ground, a swimmer), whose terrain waits and settle hold need them cooked. Edits off the window are pinned
  by the chunk manager.
- **The near band's ceiling** (`GetPlanetNearBandCeiling`, G52): the top of the slab over the near disk, the highest
  surface the near chunks' cells hold (`GetPlanetSurfaceEnvelope` over the near radius's widest angle, at the shell's
  inner radius, plus half a chunk diagonal; memoized by the viewer's cell) + `SlabSlackChunks`, or the sea's surface +
  `SlabSlackChunks` where that is higher. A chunk off its slab wholly above it lies above its own slab too: air. So the
  band reaches no higher, and a viewer above the ceiling takes none. Until G52 the band followed the viewer at any
  altitude inside the shell: a viewer descending onto the 50 km or 1,000 km test planet from 100 km up streamed a new
  layer of air every second, taking the generation its ground disk needed, and loaded the last layers over the ground in
  its last second (WORLD_MODES_REVIEW_PLAN.md §20.7). The keep window is unchanged, so a viewer crossing the ceiling
  unloads nothing.
- **The cull** (`ShouldCullBeyondHorizon`, SP-C2), a superset of the window: a chunk wholly inside the shell's inner
  radius or outside its outer one, or whose nearest point lies beyond the horizon, the sum of the two tangent distances to
  the datum sphere, `sqrt(2Rh + h^2) + sqrt(2RH + H^2)` (`GetPlanetHorizonDistance`; R the radius, h the viewer's altitude,
  H the terrain's top: a peak shows over the horizon from that far). A viewer inside the planet's lowest terrain is not
  culled.

With `bFarBandSurfaceSlabCulling` off the window is the whole shell inside the cone (fine on a thin shell; with km-scale
relief that is tens of thousands of chunks). The LOD band is the chunk's 3D distance from the viewer, so a high viewer's
ground disk sits at the coarsest band. Planets draw their sea with the ocean shell, not voxel water tiles, so a sea
column's slab is its seabed's (heightmap slabs reach up to the water plane).

**Candidates** (`ForEachPlanetCandidate`): the chunk columns along the grid axis closest to the viewer's up, inside the
bounding box of the cone, crossing the spherical shell of the slab's envelope over the disk (`GetPlanetSurfaceEnvelope`:
the cells within the cone's angle, memoized by the viewer's cell; `GetShellCrossing`), plus the near window's cone and
altitude band. Load, unload (`GetChunksToUnload`, the keep window) and the balanced LOD cache use the same window.

So the window is the ground disk of the view distance, a few chunks thick around the surface wherever it is: 950 to
1,160 chunks on the 2.5 km planet and on a 1,000 km one with km-scale relief alike (`LOD.Planet.SurfaceSlab`; the whole
shell would be 1,000-1,200 and 17,700), and 1,080-1,260 live on the 50 km and 1,000 km test planets
(WORLD_MODES_REVIEW_PLAN.md §20.4), against 2,800-3,000 for them before WM-BP8, whose
shells were thicker than the view distance, so their windows were the whole sphere of it around the viewer. A climbing
viewer keeps its ground disk (`LOD.Planet.GroundHorizon`): until WM-BP8 the window was clipped to the viewer's Z ± the
view distance and measured in 3D from the viewer, so it held nothing from about the view distance up (the planet's G13).
A flyer above the surface slab adds nothing off it (G52); until then a flyer inside the shell added its near window (160
to 200 chunks of air on the 1,000 km planet), and only above the shell nothing.
Beyond the window the far field draws the planet (below).

### The Far Field (Spherical Planet; heightmap worlds since WM-MP1)

The voxel window has one chunk size and generates every chunk at full resolution, so it cannot reach the horizon (58,000
chunks on the ground of a 50 km planet) or orbit. Past it a planet draws its far field (spherical Phase 6, WM-BP9,
decision D19; WORLD_MODES_REVIEW_PLAN.md §20.5): `FVoxelPlanetFarField` (VoxelStreaming), a cube-sphere quadtree of
patches of the analytic surface. It is not an `IVoxelLODStrategy`: its patches are heightfields with their own
primitives, and the voxel window keeps its size. Since the world modes review's MP3 (WM-MP1, decision D36;
WORLD_MODES_REVIEW_PLAN.md §23) a plane and an island draw the same far field over the XY plane ("On a heightmap world"
below): before, nothing was drawn past the voxel window but an island's far ocean.

- **Patches.** Six face quadtrees on the planet's surface coordinate (`FVoxelCubeSphere`'s faces and tangent warp; level 0
  is a face, each level halves a patch along U and V, `FVoxelFarFieldPatchKey`). A patch is `PatchQuads` x `PatchQuads`
  quads (`voxel.World.FarFieldQuads`, 16) of the conditioned analytic surface (`GetConditionedTerrainAltitude`, zones
  gathered for patches up to 32 chunks in radius), drawn `Sink` (a voxel) under it, with normals from the grid (one ring
  of samples past its edges), the surface's materials (below), and skirts deep enough to close the gap to a neighbour up
  to three levels coarser or finer. A shared cube edge is the same direction from both faces (the gnomonic coordinate exactly +-1 there),
  so the terrain's step across the edge's plane (G43) never splits it.
- **Materials** (G45, decision D28; WORLD_MODES_REVIEW_PLAN.md §20.27). Each grid vertex samples the surface's material
  mix (`FVoxelSurfaceQuery::QueryPlanetSurfaceMaterialMixAt`: in a biome blend zone the two leading biomes' surface
  materials, height rules applied, and the second's share, which is what generation's dithered per-voxel picks cover
  there; one material outside the zones). A quad carries its corners' two largest materials and, at each corner, the
  second's weight of the two; both materials are flat per triangle (a grid vertex is duplicated where neighbouring quads
  hold different pairs) and only the weight interpolates. The terrain material picks one of the two per pixel against a
  temporal screen dither (`FarFieldMaterialPick`, [MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) "Far-Field Material Blend"),
  which TSR resolves into a gradient across the quad. `voxel.World.FarFieldMaterialBlend 0` gives each quad one
  material, the most common of its corners' picks (`QueryPlanetSurfaceConditionsAt`), so a border is a staircase of
  quads, as before G45; it is read when patches build (`voxel.World.FarField 0` then `1` rebuilds them).
- **Selection.** A patch splits while its geometric error (the drawn triangles against the surface at the quad centres,
  curvature and the sea's sag included) projects to more than `voxel.World.FarFieldError` pixels (2) from the camera,
  and stays whole under 0.7 of it; a patch wholly under the camera's horizon (the lowest terrain's sphere, plus the
  angle its top stays in view) is neither drawn nor split. A patch keeps drawing until its subtree covers its area, so
  the planet never shows a hole while it refines; builds run on the thread pool (`voxel.World.FarFieldMaxBuilds`, 6 in
  flight), nearest first. A patch whose subtree waits for a build stands in for it, hiding everything under it, so a
  wait where nothing would show must not reach one (visual polish VP4, `voxel.World.FarFieldAvoidStandIns`, 1): a band
  patch the voxel chunks cover covers its area without a mesh, a patch culled in its last visit (coming up over the
  horizon) waits for its build drawing nothing, and a chunk re-meshing counts as drawn. Before, the nearest built
  ancestor stood in, often a face's root (the others released), and its coarse surface flashed over the voxel terrain
  and the horizon for a frame or more ([VISUAL_POLISH_PLAN.md](../VISUAL_POLISH_PLAN.md) "VP4 terrain flicker").
  The chunk manager updates the far field in its tick (`TG_PrePhysics`) with the player camera manager's camera, the
  frame before's (the camera managers update after `TG_PostPhysics`); after a cut, a camera more than a chunk from that
  one, it updates it again in `TG_PostUpdateWork` for this frame's (`FVoxelFarFieldCameraTickFunction`,
  `UpdateFarFieldForCamera`; F4.3, `voxel.World.FarFieldCameraUpdate`), so the frame of a teleport draws the far field
  where the camera arrived rather than the old position's selection.
- **The hole.** Within the window's reach of the viewer's ground point (the unload distance plus a chunk's half
  diagonal, along the ground as the window measures its horizons) patches split down to the band level, one to two
  chunks across (`FVoxelFarFieldSettings::ComputeBandLevel`: 64 a face edge on the 2.5 km planet, 1,024 on a 50 km one,
  16,384 on a 1,000 km one). A band patch hides its terrain once every voxel chunk its surface passes through is drawn
  (its vertices', its quad centres' and a vertex spacing around each: `SurfaceChunks`), after the chunks' fade-in. A
  partly covered patch stays drawn a voxel under the voxel surface, which hides it: no gap and no overlap along the
  window's ragged edge, and no far-field floor in a hole or cave where the chunks are drawn. Patch geometry never depends
  on the window: the hole moves with the window by visibility alone, recomputed when a chunk changes state.
- **Residency.** One procedural mesh component per resident patch at the patch's origin (its centre's point on the
  datum, double), vertices as float offsets of the patch's size; a patch hidden for 10 s releases its component and
  builds again when needed (its subtree draws meanwhile), except the roots and band patches near the window's edge,
  which are built while covered so they show the moment their chunks unload (a covered one the viewer leaves there
  builds while it draws nothing, VP4). Every root is built from the first updates, also a face under the horizon, so
  after a teleport onto a face the camera never saw its root stands in at once (F4.2,
  `voxel.World.FarFieldBuildAllRoots`, 1; before, a root was built once some of its face came into view and the first
  update there drew no far field).
- **The sea**, at scale: where the ocean shell would sag past its budget (`ShouldDrawSea`: from about R 3.3 x 10^5) each
  patch draws a sea section over the cells near or under the sea, 8 x 8 cells a patch, over the voxel window too (a
  planet has no voxel water tiles), with an underside near the window shown from under the sea. The small planet keeps
  its ocean shell.
- **On a heightmap world** (world modes review WM-MP1, decisions D36 and D37; WORLD_MODES_REVIEW_PLAN.md §23.10) the same
  class runs with a heightmap frame: the key's face is `FVoxelFarFieldPatchKey::HeightmapFace` and its X and Y are signed
  cell indices of one quadtree over the XY plane whose level-0 cells are **root tiles** of `voxel.World.FarFieldRootChunks`
  chunks aligned to the chunk grid (0, the default: about a quarter of the far distance in chunks rounded to a power of
  two, `ComputeHeightmapRootChunks`: 64 with 64-voxel chunks of 100 uu, 409,600 uu tiles; 256 with 1,600 uu chunks),
  so the band level is log2 of that and a band patch is one chunk column. Roots exist for the tiles within
  `voxel.World.FarFieldDistance` of the camera (2,000,000 uu, the island far ocean's reach; about 10 x 10 tiles about
  the camera, made as it moves and dropped with their trees past it) and, on a bounded world, only over the island's bounds box, which
  they never leave; a patch is culled past the far distance or wholly outside the box (there is no horizon on a plane).
  The surface is the generated height (`IVoxelWorldMode::GetTerrainHeightAt` with the heightmap conditioning zones, as
  `GetGeneratedSurfaceHeight` reads it; in a cubic world the block top of the column, `FVoxelBlockSurface`, so the far
  field meets the blocks at the window's edge without a step), local up is +Z, the materials
  `FVoxelSurfaceQuery::QuerySurfaceConditions` and its mix (`QuerySurfaceMaterialMix`, G45's rule with the heightmap
  climate), and the error, the skirts, the hole rule (band patches hidden once every chunk their surface passes through
  is drawn, distances measured on the plane from the streaming viewer's XY), stand-in avoidance and the camera update
  after a cut are the planet's. The sea is a section on the `WaterLevel` plane over the cells whose terrain dips near or
  under it, with the water tiles' UVs (world X and Y over the voxel size, one pattern with the tiles and the far ocean),
  and, since the water tiles draw the sea over the streamed columns, a band patch's sea hides where its column's water
  tile is drawn (`SetWaterTileQuery`, `UVoxelChunkManager::HasWaterTile`) and draws where none is: the ring between the
  window's load and unload horizons holds chunks without tiles, and before the query its sea went with its terrain (a
  dry strip at the window's edge seen from above). A plane has no horizon, so its far field splits far more patches
  than a planet's at the same screen error (on `VoxelDemo` at 2 px about 1,500 patches and 300 MB from the ground,
  against a planet's 29-56 MB): heightmap worlds take their own budget, `voxel.World.FarFieldHeightmapError` (4 px; 0 =
  `voxel.World.FarFieldError`). An island's far ocean keeps its open sea past the bounds box and its static seabed and
  builds no grid water or floor while the far field is on (D37: two seas inside the bounds would z-fight;
  `voxel.World.FarField 0` brings the grid back). Heightmap conditioning zones rebuild the patches they meet
  (`InvalidateBox`), a planet's radial ones the caps. The build queue's priority offset for non-root patches is 1e12
  (it was 1e30, which a double adds a distance to without change, so with six builds a tick the patches under the
  camera could starve behind thousands of far ones: found on the plane, the planet's six roots and horizon hid it).

`voxel.World.FarField 0` drops the far field (the ocean shell draws a planet's sea and an island's far ocean its grid
water, as before); `voxel.World.FarFieldStats`, `vox.Planet` / `vox.World` and the bench's `FarFieldMs`, `FarFieldPatchN`,
`FarFieldMB`, `FarFieldBuiltN` and `FarFieldStandInN` (stand-ins over band patches the chunks cover, VP4) report it;
`voxel.World.FarFieldTrace 1` logs every update with such a stand-in and why.

### Orbit and Back: the Voxel Cut-Over (Spherical Planet; heightmap worlds since WM-MP2)

From high up the voxel window adds little the far field does not draw: its ground disk is a narrow cone under the
viewer, all of it in the coarsest band. So a planet's window empties above a cut-over altitude and streams back on the
way down (spherical Phase 6, WM-BP10, decision D18; WORLD_MODES_REVIEW_PLAN.md §20.6). A plane's or an island's window
does the same since the world modes review's MP3 (WM-MP2, decision D38): the altitude is measured over the generated
ground under the streaming viewer or the sea where higher, `PlanetVoxelCutoverAltitude` and the three-view-distance
default, `voxel.World.VoxelCutover` and G49's guard apply as on a planet, a suspended heightmap window returns no column
(`GetVisibleChunks`, the balanced LOD cache) and unloads every chunk, and the cut-over is in force only while the chunk
manager has a far field (`FLODQueryContext::bFarFieldActive`): without one `GetCutoverAltitude` is 0 (the configured
altitude stays readable as `GetPlanetCutoverAltitude`) and the window keeps its ground disk at any altitude (D8), since
a suspended window with nothing drawn under it is a hole in the world. The same gate holds on a planet since G78
(world modes review §23.13): `voxel.World.FarField 0` there once left the cut-over in force, so from orbit nothing drew
the planet; now it keeps the window, and `vox.Planet` reports "cut-over off: no far field".

- **The cut-over** (`FDistanceBandLODStrategy`, `IVoxelLODStrategy::IsWindowSuspended`): decided in `Update` from the
  streaming viewer's altitude over the surface under it, the analytic terrain along its direction or the sea where that
  is higher (`GetViewerSurfaceAltitude`: a flyer skimming a deep sea keeps its window). Above
  `UVoxelWorldConfiguration::PlanetVoxelCutoverAltitude` (0: three view distances, `DefaultCutoverViewDistances`:
  120,000 uu on the review planets) the window is suspended; it resumes below `CutoverResumeFraction` (0.85) of it, so a flyer hovering at the cut-over does
  not churn. `voxel.World.VoxelCutover` overrides it (-1: the configuration's; 0: never, the window keeping its ground
  disk at any altitude as WM-BP8 made it). A suspended window resumes only with the camera under the cut-over as well
  (`FLODQueryContext::ViewerLead` gives the strategy the camera's altitude; G49, `voxel.World.CutoverResumeGuard`, 0 =
  the streaming viewer alone decides, the rule before): the descent prefetch below can bring a resume forward to the
  cut-over but no higher.
- **Suspended**, `GetVisibleChunks` returns nothing, `GetChunksToUnload` every loaded chunk and the balanced LOD cache is
  empty; the chunk manager evicts its generation queue, stale-culls whatever finishes generating, and neither pins nor
  keeps edited chunks (their edits stay in the edit layers and load with the window when it resumes). The window
  unloads at the configured unload rate (1,100 chunks in a few seconds).
- **Why three view distances.** At one, a 5,000 uu/s descent onto the 50 km test planet resumed the window only 10 s
  before landing (the prefetch's lead is at most half the altitude) and touched down with half of its 1,300 chunks
  loaded, 5 s of catch-up after landing against 0.36 s with the window kept; and a viewer 400 m up already saw only the
  far field. Kept up to three view distances, the window costs little (on M about 1,500 chunks and 510 MB climbing to
  60,000, within D3 and the same frame time) and a descent has about 20 s to stream it back.
- **The far field** cuts its hole while chunks are held; once the window holds none, it refines by its screen error alone
  (`BandRadius` 0: no band patches forced for an empty window). When the window resumes the band returns, its patches
  build while their parents draw, and the chunks fade in over them (`voxel.MeshFade.Appear`, measured from the camera).
- **The descent prefetch** (`UVoxelChunkManager::ApplyDescentPrefetch`): under a camera descending faster than
  `voxel.Stream.DescentMinSpeed` (1,500 uu/s) the streaming viewer goes ahead of it along local down by what it covers
  in `voxel.Stream.DescentLead` seconds (3), at most half its altitude and never within two chunks of the surface
  (`ComputeDescentLead`; the camera's radial speed smoothed, a jump of ten chunks in a frame treated as a teleport). The
  window resumes and refines its LOD that much earlier; the cut-over, the LOD bands and the window are the streaming
  viewer's, the far field's error and horizon and the appearance fade the camera's. Before G49's guard (above) a fast
  descent that stopped above the cut-over resumed the window for a moment and suspended it again as the lead decayed
  (on L 0.7-1.8 s, 20-100 chunks generated and about 1,000 queued generations dropped a stop); now a window the lead
  resumes has the camera under the cut-over, and a window suspends only with the streaming viewer, which is never above
  the camera, over the cut-over, so only a climb back over the cut-over suspends it again.
- **The camera is the streaming viewer**, so a camera that leaves the pawn (the orbit camera, a debug camera) takes the
  window with it, as it always has: above the cut-over the ground under a pawn left behind unloads. `vox.Orbit` holds the
  pawn (a swimmer too since G48) and settles it on release; survey cameras freeze streaming.

`vox.Planet` (and `vox.World` in every mode) reports the window's state, the cut-over and the lead; the bench adds
`Suspended`, `LeadUU` and `SurfAlt` columns, a `voxelCutover` summary and the run's `cutoverTransitions` (each resume and
suspension with the camera, the streaming viewer and the lead, and for a suspension the seconds active, the chunks held
and generated and the queued generations dropped; `UVoxelChunkManager::GetCutoverTransitions`, also logged), and a
descent route (`descend=Lat:Lon:Alt` on a planet, `descend=X:Y:Alt` on a heightmap world; climbs `climb=` likewise).

### Integration with Streaming

Load and unload use the same pieces:

```cpp
// GetVisibleChunks / GetChunksToLoad: per column within the load horizon, the layers of its window
for each column with GetHorizonDistance(column centre) <= ViewDistance:   // horizontal in heightmap worlds
    GetColumnWindow(X, Y, Context, EWindowUse::Load).ForEachLayer(Z):
        skip if ShouldCullIslandBoundary(...) || ShouldCullBeyondHorizon(...)
        request (X, Y, Z) at its balanced LOD

// A planet streams by chunk instead (WM-BP8): the candidates of ForEachPlanetCandidate, each kept by
// GetHorizonDistance (along the ground) <= ViewDistance && IsInPlanetWindow(..., EWindowUse::Load)

// GetChunksToUnload: every loaded chunk that is out of range, farthest first
if (GetHorizonDistance(ChunkCenter, Viewer) > ViewDistance * UnloadDistanceMultiplier ||
    !GetColumnWindow(X, Y, Context, EWindowUse::Keep).Contains(Z) ||   // heightmap worlds
    !IsInPlanetWindow(ChunkCoord, Context, EWindowUse::Keep) ||          // a planet
    ShouldCullIslandBoundary(...) || ShouldCullBeyondHorizon(...))
{
    OutUnload.Add(ChunkCoord);
}
```

### What Bounds Residency

There is no chunk-count cap (`MaxLoadedChunks` was never read and was removed in M4, WM-S3). What is resident is
bounded by geometry:

- **The horizon:** chunks load within the view distance and unload past it × `UnloadDistanceMultiplier` (1.2), measured
  horizontally in heightmap worlds (a cylinder) and along the ground from the viewer's ground point on a planet (a cone
  around the viewer's up-line; 3D from the viewer until spherical Phase 6's WM-BP8).
- **The vertical window:** each column's slab, plus the near window's layers near the viewer (above); in all, the
  world mode's height bounds.
- **The world's footprint:** an island keeps only columns that touch its R + falloff footprint (since M6; before, out to
  a chunk diagonal past it); a planet only chunks in its terrain shell and above its horizon, each within its surface
  slab or near the viewer below it (WM-BP8).
- **Edits:** edited chunks pin-load within the view distance and stay to the unload distance, outside the window too.
- **The pipeline:** chunks mid-generation or mid-meshing finish before they unload; `MaxChunksToUnloadPerFrame` paces
  unloading, not residency.

On the demo geometry (4,800 uu chunks, 40,000 uu view) that is about 980 chunks settled and 1,080-1,220 at peak while
moving, for InfinitePlane and an island alike; a fully resident island S (0.8 km across) holds about 1,020. Measured in
[WORLD_MODES_REVIEW_PLAN.md](../WORLD_MODES_REVIEW_PLAN.md) §14.8.

---

## Future Strategies

### Quadtree LOD (Future)

**Use Case**: Infinite plane worlds with view-dependent LOD

**Approach**:
- 2D quadtree (X/Y plane)
- Screen-space subdivision criterion
- 4-way branching
- T-junction handling required

**Pros**:
- Adaptive to terrain complexity
- Screen-space optimization
- Lower memory than distance bands

**Cons**:
- Complex meshing (T-junctions)
- Tree traversal overhead
- Harder to debug

**When to implement**: If profiling shows distance bands are inefficient for flat, large-scale worlds.

### Octree LOD (Future)

**Use Case**: Spherical planets, cave systems, true 3D worlds

**Approach**:
- Full 3D octree
- Surface vs interior importance
- 8-way branching
- 3D T-junction handling

**Pros**:
- Optimal for 3D terrain
- Efficient for sparse volumes (caves)
- Can deprioritize underground

**Cons**:
- Very complex meshing
- High implementation cost
- Tree management overhead

**When to implement**: Only if spherical planet mode requires interior detail or extensive cave systems.

---

## Integration with ChunkManager

### ChunkManager Update Loop

The ChunkManager has been optimized with streaming decision caching to minimize per-frame overhead:

```cpp
void UVoxelChunkManager::TickComponent(float DeltaTime) {
    // 1. Build query context from camera
    FLODQueryContext Context = BuildQueryContext();
    FIntVector CurrentViewerChunk = WorldToChunkCoord(Context.ViewerPosition);

    // 2. Check if streaming update is needed (Phase 2 optimization)
    bool bViewerChunkChanged = (CurrentViewerChunk != CachedViewerChunk);
    bool bNeedStreamingUpdate = bForceStreamingUpdate || bViewerChunkChanged;

    // 3. Always update LOD strategy (for morph interpolation)
    LODStrategy->Update(Context, DeltaTime);

    // 4. Streaming decisions only when viewer crosses chunk boundary
    if (bNeedStreamingUpdate) {
        TArray<FChunkLODRequest> ToLoad;
        TArray<FIntVector> ToUnload;

        LODStrategy->GetChunksToLoad(ToLoad, LoadedChunkCoords, Context);
        LODStrategy->GetChunksToUnload(ToUnload, LoadedChunkCoords, Context);

        // Queue with O(1) duplicate detection (Phase 1 optimization)
        for (const FChunkLODRequest& Request : ToLoad) {
            AddToGenerationQueue(Request);  // Uses TSet for dedup
        }

        CachedViewerChunk = CurrentViewerChunk;
        bForceStreamingUpdate = false;
    }

    // 5. Process queues (time-sliced)
    ProcessGenerationQueue(Configuration->StreamingTimeSliceMS); // Context.TimeSliceMS deleted in CR1 (STREAM-11)
    ProcessUnloadQueue(Context.MaxChunksToUnloadPerFrame);

    // 6. LOD transitions only when viewer moved significantly
    float PositionDeltaSq = FVector::DistSquared(Context.ViewerPosition, LastLODUpdatePosition);
    if (bViewerChunkChanged || PositionDeltaSq > LODUpdateThresholdSq) {
        UpdateLODTransitions(Context);
        LastLODUpdatePosition = Context.ViewerPosition;
    }
}
```

### Building Query Context

```cpp
FLODQueryContext UVoxelChunkManager::BuildQueryContext() const {
    FLODQueryContext Context;
    
    // Get player camera
    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    if (PC) {
        FVector Location;
        FRotator Rotation;
        PC->GetPlayerViewPoint(Location, Rotation);
        
        Context.ViewerPosition = Location;
        Context.ViewerForward = Rotation.Vector();
        
        // FOV and frustum
        Context.FieldOfView = PC->PlayerCameraManager->GetFOVAngle();
        Context.FrustumPlanes = CalculateFrustumPlanes(Location, Rotation, Context.FieldOfView);
    }
    
    // World-specific data
    Context.WorldOrigin = WorldConfiguration->GetWorldOrigin();
    Context.WorldMode = WorldConfiguration->GetWorldMode();
    // (the view distance, radius, budgets and frame fields were deleted in CR1, STREAM-11)
    
    // Optional extensions
    Context.BiomeSystem = BiomeSystem;
    Context.EditLayer = EditLayer;
    
    return Context;
}
```

---

## LOD Morphing

LOD morphing smoothly transitions between LOD levels to avoid popping artifacts.

### Vertex Shader Implementation

```hlsl
// In VoxelVertexFactory.usf

struct FVoxelVertexFactoryInput {
    float3 Position : ATTRIBUTE0;
    // ... other attributes
};

float3 MorphVertex(float3 CurrentPos, float3 NextLODPos, float MorphFactor) {
    return lerp(CurrentPos, NextLODPos, MorphFactor);
}

FVertexFactoryInterpolantsVSToPS Main(FVoxelVertexFactoryInput Input) {
    // Get LOD morph factor from uniform
    float MorphFactor = VoxelUniforms.LODMorphFactor;
    
    // Calculate next LOD position (simplified)
    float3 NextLODPos = CalculateNextLODPosition(Input.Position, CurrentLOD);
    
    // Morph between current and next LOD
    float3 MorphedPos = MorphVertex(Input.Position, NextLODPos, MorphFactor);
    
    // Transform to world space
    Output.Position = mul(float4(MorphedPos, 1.0), LocalToWorld);
    
    return Output;
}
```

### C++ Uniform Update

```cpp
void UVoxelChunkManager::UpdateLODTransitions(const FLODQueryContext& Context) {
    for (auto& Pair : LoadedChunks) {
        FIntVector ChunkCoord = Pair.Key;
        FChunkState& State = Pair.Value;
        
        // Query current morph factor
        float NewMorphFactor = LODStrategy->GetLODMorphFactor(ChunkCoord, Context);
        
        if (FMath::Abs(State.MorphFactor - NewMorphFactor) > 0.01f) {
            State.MorphFactor = NewMorphFactor;
            
            // Update renderer (sets vertex shader uniform)
            MeshRenderer->UpdateLODTransition(ChunkCoord, NewMorphFactor);
        }
    }
}
```

---

## Performance Considerations

### Query Frequency

With **Phase 2 streaming optimizations**, query frequency is dramatically reduced:

**Before optimization**:
- `GetVisibleChunks()` called every frame
- 1000 visible chunks × 60 FPS = 60,000 LOD queries/sec

**After optimization**:
- `GetVisibleChunks()` called only when viewer crosses chunk boundary
- At walking speed (~600 cm/s) with 3200 unit chunks: ~0.19 crossings/sec
- Reduces to ~190 LOD queries/sec (99.7% reduction)

### Implemented Optimizations

**Phase 1: Queue Management**
1. **O(1) Duplicate Detection**: TSet tracking alongside queue arrays
2. **Sorted Insertion**: `Algo::LowerBound()` for O(log n) insert instead of O(n²) re-sort
3. **Queue Growth Limiting**: Cap at 2× processing rate per frame

**Phase 2: Decision Caching**
1. **Chunk Boundary Caching**: `CachedViewerChunk` tracks viewer's current chunk
2. **Position Delta Threshold**: Skip LOD updates when viewer moved < 100 units
3. **Cached Positions**: `LastStreamingUpdatePosition`, `LastLODUpdatePosition`

**Future Phase 3: LOD Hysteresis** (if needed)
1. **Buffer Zones**: Prevent rapid LOD switching at band boundaries
2. **Asymmetric Thresholds**: Different distances for upgrade vs downgrade
3. **Implementation**: Add ~50-100 unit hysteresis per LOD band boundary

### Additional Optimization Strategies

1. **Spatial Indexing**: Use grid or tree for visibility queries
2. **Frustum Culling**: Early-out for off-screen chunks
3. **SIMD**: Vectorize distance calculations

### Memory Overhead

**Distance Bands**: Minimal state (just band configuration)
**Streaming Cache**: ~48 bytes (FIntVector + 2× FVector + bool)
**Quadtree**: ~16 bytes per node × ~1000 nodes = 16 KB
**Octree**: ~32 bytes per node × ~10,000 nodes = 320 KB

---

## Implementation Guide

### Step 1: Define Interface

```cpp
// Source/VoxelLOD/Public/IVoxelLODStrategy.h
class IVoxelLODStrategy {
    // ... (full interface as shown above)
};
```

### Step 2: Implement Distance Bands

```cpp
// Source/VoxelLOD/Private/DistanceBandLODStrategy.cpp
class FDistanceBandLODStrategy : public IVoxelLODStrategy {
    // ... (implementation as shown above)
};
```

### Step 3: Create UObject Wrapper

```cpp
// Source/VoxelLOD/Public/VoxelLODStrategyBase.h
UCLASS(Abstract, Blueprintable, EditInlineNew)
class UVoxelLODStrategyBase : public UObject {
    virtual IVoxelLODStrategy* GetImplementation() PURE_VIRTUAL(...);
};

UCLASS()
class UDistanceBandLODStrategy : public UVoxelLODStrategyBase {
    UPROPERTY(EditAnywhere)
    TArray<FLODBandConfig> Bands;
    
    virtual IVoxelLODStrategy* GetImplementation() override {
        if (!Impl) {
            Impl = MakeUnique<FDistanceBandLODStrategy>();
            // Configure from Bands array
        }
        return Impl.Get();
    }
    
private:
    TUniquePtr<FDistanceBandLODStrategy> Impl;
};
```

### Step 4: Integrate with ChunkManager

```cpp
// Source/VoxelStreaming/Private/VoxelChunkManager.cpp
void UVoxelChunkManager::Initialize(UVoxelWorldConfiguration* Config) {
    // Create LOD strategy
    if (Config->LODConfig && Config->LODConfig->StrategyClass) {
        LODStrategy = NewObject<UVoxelLODStrategyBase>(this, Config->LODConfig->StrategyClass);
        LODStrategy->GetImplementation()->Initialize(*Config);
    }
}
```

### Step 5: Add Configuration Asset

```cpp
// Content/Config/DA_DefaultLODConfig.uasset
UCLASS()
class UVoxelLODConfiguration : public UDataAsset {
    UPROPERTY(EditAnywhere)
    TSubclassOf<UVoxelLODStrategyBase> StrategyClass;
    
    UPROPERTY(EditAnywhere, Instanced)
    UVoxelLODStrategyBase* StrategyInstance;
};
```

### Step 6: Test

```cpp
// Unit test
TEST(VoxelLOD, DistanceBandBasic) {
    FDistanceBandLODStrategy Strategy;
    // Configure bands
    // Test GetLODForChunk at various distances
    // Verify correct LOD levels
}
```

---

## Debugging Tools

### Debug Visualization

```cpp
void FDistanceBandLODStrategy::DrawDebugVisualization(
    UWorld* World, 
    const FLODQueryContext& Context) const
{
    // Draw LOD rings
    for (const FLODBand& Band : LODBands) {
        FColor Color = GetLODColor(Band.LODLevel);
        DrawDebugCircle(
            World, 
            Context.ViewerPosition, 
            Band.MaxDistance, 
            32, 
            Color
        );
    }
    
    // Draw visible chunk bounds
    TArray<FChunkLODRequest> Visible = GetVisibleChunks(Context);
    for (const FChunkLODRequest& Request : Visible) {
        FColor Color = GetLODColor(Request.LODLevel);
        FBox Bounds = GetChunkBounds(Request.ChunkCoord);
        DrawDebugBox(World, Bounds.GetCenter(), Bounds.GetExtent(), Color);
    }
}
```

### Debug Console Commands

```cpp
// Register console commands
static FAutoConsoleCommand ShowLODZones(
    TEXT("voxel.lod.showzones"),
    TEXT("Toggle LOD zone visualization"),
    FConsoleCommandDelegate::CreateLambda([]() {
        // Toggle debug draw
    })
);
```

---

## LOD Seam Handling

When adjacent chunks render at different LOD levels, their boundary vertices may not align, creating visible seams. The VoxelWorlds system handles this differently for each meshing mode:

### Cubic Meshing

Cubic runs without the LOD system (see [LOD Off and Cubic Meshing](#lod-off-and-cubic-meshing)), so every chunk is LOD 0 and there are no LOD boundaries to stitch. Chunk boundaries go through the seam-ownership pipeline like every other mode (world modes review WM-C1, [WORLD_MODES_REVIEW_PLAN.md](../WORLD_MODES_REVIEW_PLAN.md) §14.9): a chunk mesh is the Interior domain, every face except those on the chunk's six boundary planes, read from the chunk's own voxels only; the faces on a shared plane belong to that pair's face seam job, which culls them against both chunks' voxels. No cubic face lies only on a chunk edge or corner, so the seam registry tracks face seams alone for cubic (`FVoxelSeamRegistry::SetFaceSeamsOnly`). A chunk never waits for a neighbour's data and is meshed once; a neighbour arriving or changing rebuilds a face seam, not the chunk. At the edge of the loaded region a plane whose other side is not loaded has no seam yet, so it shows no faces (the legacy path drew walls there, KC13).

### Marching Cubes (Transvoxel)

Marching Cubes has mismatched vertices at LOD boundaries because vertex positions depend on density interpolation. The **Transvoxel algorithm** solves this:

1. **Transition Detection**: The chunk manager detects which faces border lower-LOD neighbors
2. **Transition Cells**: Boundary cells use special 9-sample transition cells instead of standard 8-corner cubes
3. **Seamless Geometry**: Transition cells produce geometry that correctly connects to both LOD levels

See [MARCHING_CUBES_MESHING.md](MARCHING_CUBES_MESHING.md) for detailed Transvoxel documentation.

### Dual Contouring (Boundary Cell Merging)

Dual Contouring uses **LOD boundary cell merging** instead of Transvoxel transition cells. When a chunk face borders a coarser neighbor:

1. **Merge Ratio**: Computed from `CoarserStride / CurrentStride`
2. **Cell Grouping**: Fine boundary cells are grouped into `MergeRatio x MergeRatio` blocks
3. **QEF Re-solve**: The grouped edge crossings are merged into a single QEF and re-solved
4. **Vertex Aliasing**: All fine cells in the group point to the merged vertex

This approach is simpler than Transvoxel (no lookup tables) and naturally extends to arbitrary LOD level differences.

See [DUAL_CONTOURING.md](DUAL_CONTOURING.md) for detailed boundary merging documentation.

### Configuration

```cpp
// Marching Cubes LOD transitions
bool bUseTransvoxel = true;  // Recommended for MC (the skirt fallback was deleted in CR1, MESH-05)

// Dual Contouring LOD transitions
// No explicit config — uses NeighborLODLevels from FVoxelMeshingRequest
// QEF solver settings affect merged vertex quality:
float QEFSVDThreshold = 0.1f;
float QEFBiasStrength = 0.5f;
```

### Chunk Manager Integration

The chunk manager populates LOD transition data in the meshing request:

```cpp
// For Marching Cubes: set TransitionFaces bitmask
if (NeighborState->LODLevel > CurrentLOD) {
    MeshRequest.TransitionFaces |= TransitionFlags[face];
}

// For Dual Contouring: set NeighborLODLevels array
MeshRequest.NeighborLODLevels[face] = NeighborState->LODLevel;
```

---

## Next Steps

1. ~~Implement `IVoxelLODStrategy` interface~~ ✓
2. ~~Implement `FDistanceBandLODStrategy`~~ ✓
3. ~~Create UObject wrappers~~ ✓
4. ~~Integrate with ChunkManager~~ ✓
5. ~~Test with simple scene~~ ✓
6. Add debug visualization
7. ~~Profile and optimize~~ ✓ (Phase 1 & 2 streaming optimizations)
8. ~~Implement Transvoxel for smooth meshing LOD seams~~ ✓
9. ~~Mode-specific culling~~ ✓ (Terrain bounds, island boundary, horizon/shell)
10. ~~LOD material selection fix~~ ✓ (Upward surface scanning)
11. **(Future)** Phase 3: LOD hysteresis if thrashing observed at large view distances
12. **(Future)** Octree LOD for orbital-scale spherical planet viewing

See [IMPLEMENTATION_PHASES.md](IMPLEMENTATION_PHASES.md) for detailed roadmap.

---

**Status**: Implemented - Distance band LOD with mode-specific culling, Transvoxel seam handling, and streaming optimizations
