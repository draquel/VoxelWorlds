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
     * Get chunks that should be unloaded (no longer needed).
     * @param OutUnload Output array of chunks to unload
     * @param Context Query context with viewer state
     */
    virtual void GetChunksToUnload(
        TArray<FIntVector>& OutUnload, 
        const FLODQueryContext& Context
    ) const = 0;
    
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
    
    /** Viewer right direction (normalized) */
    FVector ViewerRight;
    
    /** Viewer up direction (normalized) */
    FVector ViewerUp;
    
    /** Maximum view distance (for culling) */
    float ViewDistance;
    
    /** Field of view in degrees */
    float FieldOfView;
    
    /** Frustum planes for culling (optional) */
    TArray<FPlane> FrustumPlanes;
    
    // ==================== World State ====================
    
    /** World origin (for spherical: planet center; bowl: island center) */
    FVector WorldOrigin;
    
    /** World mode (Infinite, Spherical, Island) */
    EWorldMode WorldMode;
    
    /** World radius (for spherical worlds) */
    float WorldRadius;
    
    // ==================== Performance Budgets ====================
    
    /** Maximum chunks to load per frame */
    int32 MaxChunksToLoadPerFrame = 4;
    
    /** Maximum chunks to unload per frame */
    int32 MaxChunksToUnloadPerFrame = 8;
    
    /** Time slice for streaming operations (milliseconds) */
    float TimeSliceMS = 2.0f;
    
    // ==================== Frame Information ====================

    /** Current frame number (for temporal logic) - int64 for Blueprint compatibility */
    int64 FrameNumber;

    /** Current game time (seconds) */
    float GameTime;

    /** Time since last frame (seconds) */
    float DeltaTime;
    
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

**Spherical Planet:**
```cpp
LODBands = {
    { 0.0f,    400.0f,  0, 1,  32,  50.0f  },  // Surface detail
    { 400.0f,  800.0f,  1, 2,  32,  50.0f  },  
    { 800.0f,  1600.0f, 2, 4,  32,  100.0f },
    { 1600.0f, 3200.0f, 3, 8,  64,  200.0f },
    { 3200.0f, 6400.0f, 4, 16, 128, 400.0f },
};
```

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

```cpp
float FDistanceBandLODStrategy::GetDistanceToViewer(
    FVector Position, 
    const FLODQueryContext& Context) const
{
    switch (Context.WorldMode) {
        case EWorldMode::SphericalPlanet:
            // Distance along planet surface (geodesic), not euclidean
            return CalculateSphericalDistance(
                Position, 
                Context.ViewerPosition, 
                Context.WorldOrigin,
                Context.WorldRadius
            );
            
        case EWorldMode::IslandBowl:
            // Could use 2D distance (ignore Z) for flatter LOD
            return FVector::Dist2D(Position, Context.ViewerPosition);
            
        default: // Infinite plane
            return FVector::Distance(Position, Context.ViewerPosition);
    }
}
```

---

## Mode-Specific Culling

The LOD strategy includes mode-specific culling optimizations that skip chunks guaranteed to be empty or invisible, significantly reducing the number of chunks loaded.

### Terrain Bounds Culling (Infinite Plane & Island/Bowl)

For infinite plane and island modes, chunks above or below the terrain height range are skipped. Both modes use the same 2D heightmap terrain generation:

```cpp
bool FDistanceBandLODStrategy::ShouldCullOutsideTerrainBounds(
    const FIntVector& ChunkCoord,
    const FLODQueryContext& Context) const
{
    // Applies to both Infinite Plane and Island modes
    if (WorldMode != EWorldMode::InfinitePlane &&
        WorldMode != EWorldMode::IslandBowl) return false;

    const float ChunkWorldSize = BaseChunkSize * VoxelSize;
    const float ChunkMinZ = Context.WorldOrigin.Z + (ChunkCoord.Z * ChunkWorldSize);
    const float ChunkMaxZ = ChunkMinZ + ChunkWorldSize;

    // Skip chunks entirely above terrain max or below terrain min
    if (ChunkMaxZ < TerrainMinHeight) return true;
    if (ChunkMinZ > TerrainMaxHeight) return true;

    return false;
}
```

**Height Range Calculation**:
- `TerrainMinHeight = SeaLevel + BaseHeight - Buffer`
- `TerrainMaxHeight = SeaLevel + BaseHeight + HeightScale + Buffer`
- For Island mode: `TerrainMinHeight = min(TerrainMinHeight, IslandEdgeHeight - Buffer)` to handle bowl shapes
- Buffer (one chunk) accounts for noise variation and prevents popping

### Island Boundary Culling (Island/Bowl Mode)

For island mode, chunks beyond the island extent are skipped:

```cpp
bool FDistanceBandLODStrategy::ShouldCullIslandBoundary(
    const FIntVector& ChunkCoord,
    const FLODQueryContext& Context) const
{
    if (WorldMode != EWorldMode::IslandBowl) return false;

    const float ChunkWorldSize = BaseChunkSize * VoxelSize;
    const FVector ChunkCenter = Context.WorldOrigin +
        FVector(ChunkCoord) * ChunkWorldSize +
        FVector(ChunkWorldSize * 0.5f);

    // 2D distance from island center
    const FVector2D ToCenter(
        ChunkCenter.X - (Context.WorldOrigin.X + IslandCenterOffset.X),
        ChunkCenter.Y - (Context.WorldOrigin.Y + IslandCenterOffset.Y)
    );
    const float Distance2D = ToCenter.Size();

    // Add buffer for chunk diagonal
    const float ChunkDiagonal = ChunkWorldSize * UE_SQRT_2;
    const float CullDistance = IslandTotalExtent + ChunkDiagonal;

    return Distance2D > CullDistance;
}
```

**Island Extent**: `IslandRadius + FalloffWidth`

### Horizon and Shell Culling (Spherical Planet)

For spherical planets, three types of culling are applied:

```cpp
bool FDistanceBandLODStrategy::ShouldCullBeyondHorizon(
    const FIntVector& ChunkCoord,
    const FLODQueryContext& Context) const
{
    if (WorldMode != EWorldMode::SphericalPlanet) return false;

    const float ChunkWorldSize = BaseChunkSize * VoxelSize;
    const FVector ChunkCenter = Context.WorldOrigin +
        FVector(ChunkCoord) * ChunkWorldSize +
        FVector(ChunkWorldSize * 0.5f);

    // Distance from planet center
    const float ChunkDistFromCenter = FVector::Distance(ChunkCenter, Context.WorldOrigin);

    // Inner shell culling (planet core - no terrain here)
    const float InnerShellRadius = PlanetRadius - PlanetMaxTerrainDepth;
    if (ChunkDistFromCenter < InnerShellRadius - ChunkWorldSize) {
        return true;  // Deep inside planet
    }

    // Outer shell culling (empty space above terrain)
    const float OuterShellRadius = PlanetRadius + PlanetMaxTerrainHeight;
    if (ChunkDistFromCenter > OuterShellRadius + ChunkWorldSize) {
        return true;  // Far above surface
    }

    // Horizon culling
    const float ViewerDistFromCenter = FVector::Distance(Context.ViewerPosition, Context.WorldOrigin);
    const float ViewerAltitude = ViewerDistFromCenter - PlanetRadius;

    if (ViewerAltitude > 0) {
        // Geometric horizon distance: sqrt(2*R*h + h^2)
        const float HorizonDistance = FMath::Sqrt(
            2.0f * PlanetRadius * ViewerAltitude +
            ViewerAltitude * ViewerAltitude
        );

        // Add buffer for terrain height and chunk size
        const float CullDistance = HorizonDistance +
            PlanetMaxTerrainHeight +
            ChunkWorldSize * UE_SQRT_3;

        const float ChunkDistance = FVector::Distance(ChunkCenter, Context.ViewerPosition);
        if (ChunkDistance > CullDistance) {
            return true;  // Beyond horizon
        }
    }

    return false;
}
```

**Horizon Formula**: `√(2Rh + h²)` where R = planet radius, h = viewer altitude

### Integration with Streaming

Culling checks are applied in both load and unload decisions:

```cpp
// In GetChunksToLoad(): Skip chunks that would be culled
if (ShouldCullOutsideTerrainBounds(ChunkCoord, Context) ||
    ShouldCullIslandBoundary(ChunkCoord, Context) ||
    ShouldCullBeyondHorizon(ChunkCoord, Context))
{
    continue;  // Don't add to load queue
}

// In GetChunksToUnload(): Unload chunks that should now be culled
if (ShouldCullOutsideTerrainBounds(ChunkCoord, Context) ||
    ShouldCullIslandBoundary(ChunkCoord, Context) ||
    ShouldCullBeyondHorizon(ChunkCoord, Context))
{
    OutUnload.Add(ChunkCoord);
}
```

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
    ProcessGenerationQueue(Context.TimeSliceMS);
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
        Context.ViewerRight = Rotation.RotateVector(FVector::RightVector);
        Context.ViewerUp = Rotation.RotateVector(FVector::UpVector);
        
        // FOV and frustum
        Context.FieldOfView = PC->PlayerCameraManager->GetFOVAngle();
        Context.FrustumPlanes = CalculateFrustumPlanes(Location, Rotation, Context.FieldOfView);
    }
    
    // World-specific data
    Context.WorldOrigin = WorldConfiguration->GetWorldOrigin();
    Context.WorldMode = WorldConfiguration->GetWorldMode();
    Context.WorldRadius = WorldConfiguration->GetWorldRadius();
    Context.ViewDistance = WorldConfiguration->GetViewDistance();
    
    // Performance budgets
    Context.MaxChunksToLoadPerFrame = WorldConfiguration->MaxChunksPerFrame;
    Context.TimeSliceMS = WorldConfiguration->StreamingTimeSlice;
    
    // Frame info
    Context.FrameNumber = GFrameNumber;
    Context.GameTime = GetWorld()->GetTimeSeconds();
    Context.DeltaTime = GetWorld()->GetDeltaSeconds();
    
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

Cubic meshes naturally align at boundaries because voxel faces are axis-aligned. No special seam handling is required.

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
bool bUseTransvoxel = true;  // Recommended for MC
bool bGenerateSkirts = true;  // Fallback if Transvoxel disabled
float SkirtDepth = 2.0f;

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
