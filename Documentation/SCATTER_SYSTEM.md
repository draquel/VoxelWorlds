# Scatter System

**Module**: VoxelScatter
**Dependencies**: VoxelCore, VoxelStreaming

## Overview

The scatter system places vegetation (trees, grass, rocks) on voxel terrain using CPU-based placement and Hierarchical Instanced Static Mesh (HISM) rendering. The system extracts surface points from voxel data (or optionally mesh vertices via GPU compute), applies placement rules, and manages HISM instances with optimized rebuild strategies. All scatter generation runs asynchronously on the thread pool.

## Architecture

```
VoxelChunkManager
    │
    ├── OnChunkMeshDataReady(ChunkCoord, LOD, MeshData, VoxelData, ChunkSize, VoxelSize)
    │       │
    │       ▼
    │   VoxelScatterManager
    │       │
    │       ├── [CPU Path - Default] ExtractSurfacePointsFromVoxelData()
    │       │       Uses 32³ voxel data directly (LOD-independent)
    │       │
    │       ├── [GPU Path - Optional] VoxelGPUSurfaceExtractor
    │       │       Compute shader extraction from mesh vertices
    │       │
    │       ├── VoxelScatterPlacement (apply rules, generate spawn points)
    │       │       Runs on thread pool (async)
    │       │
    │       └── VoxelScatterRenderer (HISM instance management)
    │               Game thread only
    │
    └── OnChunkEdited() ──► ClearScatterInRadius() or RegenerateChunkScatter()
```

## Key Components

### VoxelScatterManager

Coordinates scatter generation and manages per-chunk data caches.

```cpp
UCLASS(BlueprintType)
class UVoxelScatterManager : public UObject
{
public:
    // Lifecycle
    void Initialize(UVoxelWorldConfiguration* Config, UWorld* World);
    void Shutdown();
    void Update(const FVector& ViewerPosition, float DeltaTime);

    // Scatter definitions
    void AddScatterDefinition(const FScatterDefinition& Definition);
    bool RemoveScatterDefinition(int32 ScatterID);
    const FScatterDefinition* GetScatterDefinition(int32 ScatterID) const;

    // Mesh data callback (called by ChunkManager)
    void OnChunkMeshDataReady(const FIntVector& ChunkCoord, const FChunkMeshData& MeshData);
    void OnChunkUnloaded(const FIntVector& ChunkCoord);

    // Edit integration
    void ClearScatterInRadius(const FVector& WorldPosition, float Radius);
    void RegenerateChunkScatter(const FIntVector& ChunkCoord);
    bool IsPointInClearedVolume(const FIntVector& ChunkCoord, const FVector& WorldPosition) const;

protected:
    // Deferred generation queue (sorted by distance, throttled)
    TArray<FPendingScatterGeneration> PendingGenerationQueue;
    int32 MaxScatterGenerationsPerFrame = 2;

    // Cleared volumes for player edits (prevents regeneration)
    TMap<FIntVector, TArray<FClearedScatterVolume>> ClearedVolumesPerChunk;
};
```

### VoxelScatterRenderer

Manages HISM components with instance pooling and deferred rebuild strategy. Instances are never removed from the HISM — they are hidden by setting scale to zero and placed on a free list. When new instances are needed, they are recycled from the free list first via `UpdateInstanceTransform`, eliminating the visual flicker caused by `ClearInstances()`/`AddInstances()` cycling.

```cpp
UCLASS()
class UVoxelScatterRenderer : public UObject
{
public:
    void Initialize(UVoxelScatterManager* Manager, UWorld* World);
    void Tick(const FVector& ViewerPosition, float DeltaTime);

    // Instance management
    void UpdateChunkInstances(const FIntVector& ChunkCoord, const FChunkScatterData& ScatterData);
    void AddSupplementalInstances(const FIntVector& ChunkCoord, const FChunkScatterData& NewScatterData);
    void RemoveChunkInstances(const FIntVector& ChunkCoord);

    // Instance pool operations
    void ReleaseChunkScatterType(const FIntVector& ChunkCoord, int32 ScatterTypeID);
    void ReleaseAllForScatterType(int32 ScatterTypeID);

    // Deferred rebuilds (prevents flicker)
    void QueueRebuild(int32 ScatterTypeID);

protected:
    // One HISM per scatter type
    TMap<int32, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HISMComponents;

    // Per-scatter-type instance pool for recycling
    TMap<int32, FHISMInstancePool> InstancePools;

    // Pending rebuilds (processed when viewer is stationary)
    TSet<int32> PendingRebuildScatterTypes;
    float RebuildStationaryDelay = 0.5f;
    float TimeSinceViewerMoved = 0.0f;
};
```

## Data Structures

### FScatterDefinition

Defines placement rules for a scatter type.

```cpp
USTRUCT(BlueprintType)
struct FScatterDefinition
{
    // Identification
    int32 ScatterID;
    FString Name;
    bool bEnabled = true;

    // Mesh
    TSoftObjectPtr<UStaticMesh> Mesh;
    TArray<TSoftObjectPtr<UMaterialInterface>> OverrideMaterials;

    // Cubic Scatter Settings
    EScatterMeshType MeshType = EScatterMeshType::StaticMesh;
    EScatterPlacementMode PlacementMode = EScatterPlacementMode::SurfaceInterpolated;

    // Billboard Settings (when MeshType == CrossBillboard)
    TSoftObjectPtr<UTexture2D> BillboardTexture;
    float BillboardWidth = 100.0f;     // cm
    float BillboardHeight = 100.0f;    // cm

    // Billboard Atlas Settings
    bool bUseBillboardAtlas = false;
    TSoftObjectPtr<UTexture2D> BillboardAtlasTexture;
    int32 BillboardAtlasColumn = 0;
    int32 BillboardAtlasRow = 0;
    int32 BillboardAtlasColumns = 4;
    int32 BillboardAtlasRows = 4;

    // Voxel Injection Settings (when MeshType == VoxelInjection)
    int32 TreeTemplateID = 0;          // Index into Configuration->TreeTemplates

    // Placement Rules
    float Density = 0.1f;                    // 0.0-1.0, probability per surface point
    float MinSlopeDegrees = 0.0f;
    float MaxSlopeDegrees = 45.0f;
    TArray<EVoxelMaterial> AllowedMaterials;
    TArray<uint8> AllowedBiomes;
    float MinElevation = -FLT_MAX;
    float MaxElevation = FLT_MAX;
    bool bTopFacesOnly = true;
    float SpawnDistance = 0.0f;              // 0 = use global ScatterRadius

    // Instance Variation
    FVector2D ScaleRange = FVector2D(0.8f, 1.2f);
    bool bRandomYawRotation = true;
    bool bAlignToSurfaceNormal = false;
    float SurfaceOffset = 0.0f;
    float PositionJitter = 0.0f;

    // LOD & Culling
    float CullDistance = 50000.0f;           // Max render distance
    float LODStartDistance = 5000.0f;        // LOD transitions + shadow cutoff
    float MinScreenSize = 0.0f;              // Screen-size culling (0-1)

    // Rendering
    bool bCastShadows = true;
    bool bEnableCollision = false;
    bool bReceivesDecals = true;

    // Debug
    FColor DebugColor = FColor::Green;
};
```

### FScatterSpawnPoint

Individual scatter instance data.

```cpp
USTRUCT()
struct FScatterSpawnPoint
{
    FVector Position;           // World position
    FVector Normal;             // Surface normal
    float Scale;                // Random scale
    float YawRotation;          // Random Y rotation (radians)
    int32 ScatterTypeID;        // Reference to definition

    FTransform GetTransform(bool bAlignToNormal, float SurfaceOffset) const;
};
```

## Placement Pipeline

### 1. Surface Extraction

Two extraction methods are available:

#### CPU Path: Voxel-Based Extraction (Default)

The default path extracts surface points directly from the 32³ voxel data, which is **LOD-independent** — voxel data is always generated at full resolution regardless of the chunk's LOD level, producing identical scatter at any LOD.

```cpp
void UVoxelScatterManager::ExtractSurfacePointsFromVoxelData(
    const TArray<FVoxelData>& VoxelData,
    const FIntVector& ChunkCoord,
    const FVector& ChunkWorldOrigin,
    int32 ChunkSize,      // Always 32
    float VoxelSize,
    float SurfacePointSpacing,
    const TArray<FClearedScatterVolume>& ClearedVolumes,
    FChunkSurfaceData& OutSurfaceData)
{
    // Stride to match SurfacePointSpacing
    const int32 Stride = FMath::Max(1, FMath::RoundToInt(SurfacePointSpacing / VoxelSize));

    // Scan columns top-down for surface transitions
    for (int32 X = 0; X < ChunkSize; X += Stride)
    {
        for (int32 Y = 0; Y < ChunkSize; Y += Stride)
        {
            for (int32 Z = ChunkSize - 1; Z >= 0; --Z)
            {
                const FVoxelData& Voxel = VoxelData[X + Y*ChunkSize + Z*ChunkSize*ChunkSize];
                if (!Voxel.IsSolid()) continue;

                // Check voxel above is air (surface transition)
                if (Z + 1 < ChunkSize && VoxelData[...above...].IsSolid()) continue;

                // Interpolate Z position (same formula as Marching Cubes edge interpolation)
                // t = (VOXEL_SURFACE_THRESHOLD - D_solid) / (D_air - D_solid)
                float Fraction = ...;
                FVector WorldPos(ChunkWorldOrigin.X + X * VoxelSize,
                                 ChunkWorldOrigin.Y + Y * VoxelSize,
                                 ChunkWorldOrigin.Z + (Z + Fraction) * VoxelSize);

                // Normal from density gradient (central differences)
                // MaterialID/BiomeID read directly from FVoxelData
                // Skip if inside cleared volume

                break; // Found topmost surface for this column
            }
        }
    }
}
```

**Advantages**:
- LOD-independent: Identical scatter output regardless of chunk LOD level
- No mesh dependency: Works before mesh is fully ready
- Direct data access: MaterialID and BiomeID read from FVoxelData without UV decoding

**Known Limitation**: Uses base procedural VoxelData, not edit-merged data. Player edits are handled via `ClearedVolumesPerChunk` instead.

#### GPU Path: Mesh Vertex Extraction (Optional)

When `bUseGPUScatterExtraction` is enabled, surface points are extracted from mesh vertices via compute shader. See [GPU Surface Extraction](#gpu-surface-extraction-phase-7d-2) for details.

#### Legacy CPU Path: Mesh Vertex Extraction

The original `FVoxelSurfaceExtractor` samples mesh vertices with spatial hashing. Still used internally by the GPU fallback path:

```cpp
void FVoxelSurfaceExtractor::ExtractSurfacePoints(
    const FChunkMeshData& MeshData,
    const FIntVector& ChunkCoord,
    const FVector& ChunkWorldOrigin,
    float TargetSpacing,
    int32 LODLevel,
    FChunkSurfaceData& OutSurfaceData)
{
    // Spatial hash grid for even distribution
    TMap<FIntVector, int32> OccupiedCells;

    for (int32 VertIndex = 0; VertIndex < MeshData.Positions.Num(); ++VertIndex)
    {
        FVector WorldPos = ChunkWorldOrigin + FVector(MeshData.Positions[VertIndex]);
        FIntVector Cell(FMath::FloorToInt(WorldPos.X / TargetSpacing), ...);
        if (OccupiedCells.Contains(Cell)) continue;

        // Extract surface point data from mesh vertex
        FVoxelSurfacePoint Point;
        Point.Position = WorldPos;
        Point.Normal = FVector(MeshData.Normals[VertIndex]).GetSafeNormal();
        Point.MaterialID = DecodeFromUV1(MeshData.UV1s[VertIndex]);
        Point.BiomeID = MeshData.Colors[VertIndex].G;
        Point.FaceType = DecodeFaceType(MeshData.UV1s[VertIndex]);

        OutSurfaceData.SurfacePoints.Add(Point);
        OccupiedCells.Add(Cell, OutSurfaceData.SurfacePoints.Num() - 1);
    }
}
```

**Note**: This path is LOD-dependent — mesh vertices change with LOD level, causing different scatter density and positions at different LODs.

### 2. Scatter Placement

Spawn points are generated using deterministic randomness:

```cpp
void FVoxelScatterPlacement::GenerateSpawnPoints(
    const FChunkSurfaceData& SurfaceData,
    const TArray<FScatterDefinition>& Definitions,
    uint32 ChunkSeed,
    FChunkScatterData& OutScatterData)
{
    FRandomStream Random(ChunkSeed);

    for (const FVoxelSurfacePoint& Point : SurfaceData.SurfacePoints)
    {
        float SlopeAngle = FMath::RadiansToDegrees(FMath::Acos(Point.Normal.Z));

        for (const FScatterDefinition& Def : Definitions)
        {
            // Check placement rules
            if (!Def.bEnabled) continue;
            if (SlopeAngle < Def.MinSlopeDegrees || SlopeAngle > Def.MaxSlopeDegrees) continue;
            if (Def.AllowedMaterials.Num() > 0 && !Def.AllowedMaterials.Contains(Point.MaterialID)) continue;
            if (Def.AllowedBiomes.Num() > 0 && !Def.AllowedBiomes.Contains(Point.BiomeID)) continue;
            if (Point.Position.Z < Def.MinElevation || Point.Position.Z > Def.MaxElevation) continue;
            if (Def.bTopFacesOnly && Point.FaceType != EVoxelFaceType::Top) continue;

            // Density check
            if (Random.FRand() > Def.Density) continue;

            // Create spawn point
            FScatterSpawnPoint SpawnPoint;
            SpawnPoint.Position = Point.Position + Point.Normal * Def.SurfaceOffset;
            SpawnPoint.Normal = Point.Normal;
            SpawnPoint.Scale = Random.FRandRange(Def.ScaleRange.X, Def.ScaleRange.Y);
            SpawnPoint.YawRotation = Def.bRandomYawRotation ? Random.FRand() * 2.0f * PI : 0.0f;
            SpawnPoint.ScatterTypeID = Def.ScatterID;

            // Apply position jitter
            if (Def.PositionJitter > 0.0f)
            {
                SpawnPoint.Position.X += Random.FRandRange(-Def.PositionJitter, Def.PositionJitter);
                SpawnPoint.Position.Y += Random.FRandRange(-Def.PositionJitter, Def.PositionJitter);
            }

            OutScatterData.SpawnPoints.Add(SpawnPoint);
        }
    }
}
```

## HISM Management

### Instance Pool / Recycling System

The scatter renderer uses an **instance pool** per HISM to eliminate visual flicker during distance-based streaming. Instances are never removed from the HISM — they are hidden by zero-scaling and placed on a free list for later recycling.

#### FHISMInstancePool

Each scatter type has a pool tracking its HISM instance lifecycle:

```cpp
struct FHISMInstancePool
{
    TArray<int32> FreeIndices;                          // Zero-scaled, ready for recycling
    TMap<FIntVector, TArray<int32>> ChunkInstanceIndices; // Active instances per chunk
    int32 TotalAllocated = 0;                           // Active + free (high water mark)
};
```

#### Why Pooling Instead of ClearInstances/AddInstances?

The previous approach (`ClearInstances()` + `AddInstances()`) produced visible flicker because UE5 propagates these as separate render state updates — even when done in the same frame, there is a brief gap where the HISM has zero instances. The pool eliminates this:

- **Release**: `UpdateInstanceTransform(Index, ZeroScaleTransform)` — instance becomes invisible but stays allocated in HISM internal arrays. Zero-scale means zero bounding box, so HISM's spatial tree culls it at zero cost.
- **Recycle**: `UpdateInstanceTransform(Index, RealTransform)` — instance reappears at new position. No structural HISM change, no render state rebuild.
- **Grow**: `AddInstances()` only when free list is exhausted. Pool grows to peak usage then stabilizes.

#### Release Operations

```cpp
// Release a specific scatter type's instances from one chunk back to pool
void ReleaseChunkScatterType(const FIntVector& ChunkCoord, int32 ScatterTypeID)
{
    // 1. Look up pool and chunk indices
    // 2. Zero-scale each instance via UpdateInstanceTransform
    // 3. Move indices to FreeIndices
    // 4. Remove chunk from pool's ChunkInstanceIndices
    // 5. Update ChunkScatterTypes tracking
    // 6. Mark HISM render state dirty (single batch call)
}

// Release ALL instances for a scatter type (used by RebuildScatterType)
void ReleaseAllForScatterType(int32 ScatterTypeID)
{
    // Same as above but across all chunks
}
```

#### Recycling in FlushPendingInstanceAdds

The per-frame budget-limited flush recycles from the free list before growing:

```
For each PendingInstanceAdd entry:
  1. Get/create HISM and pool for ScatterTypeID
  2. Skip entries for chunks unloaded while pending
  3. Recycle min(needed, FreeIndices.Num()) via UpdateInstanceTransform
  4. Grow via AddInstances() for remainder (updates pool.TotalAllocated)
  5. Record all indices in pool.ChunkInstanceIndices[ChunkCoord]
  6. Batch mark HISM render state dirty after all updates
```

Both recycled and newly added instances count toward the per-frame budget (`MaxInstanceAddsPerFrame`).

#### Rebuild Strategy

Rebuilds (triggered by chunk edits/regeneration) now work through the pool:

1. `ReleaseAllForScatterType()` — all instances go to free list (zero-scaled)
2. Collect transforms from `ScatterDataCache` for all chunks with this type
3. Queue as `PendingInstanceAdd` entries — `FlushPendingInstanceAdds` recycles from free list

This means rebuilds reuse existing pool capacity — no net HISM growth.

#### Deferred Rebuilds

- Rebuilds are queued via `QueueRebuild()`, not immediate
- Processed in `Tick()` when BOTH conditions met:
  - Viewer stationary for 0.5s (`RebuildStationaryDelay`)
  - World stable (`GetPendingGenerationCount() == 0`)
- Prevents flickering during movement and initial world loading

### New Chunk Optimization

New chunks (no existing scatter) go directly through the deferred add pipeline:

```cpp
void UVoxelScatterRenderer::UpdateChunkInstances(const FIntVector& ChunkCoord, const FChunkScatterData& ScatterData)
{
    const bool bIsNewChunk = !ChunkScatterTypes.Contains(ChunkCoord);

    if (bIsNewChunk && ScatterData.bIsValid)
    {
        // Group transforms by type, queue as PendingInstanceAdds
        // FlushPendingInstanceAdds handles pool recycling + growth
    }
    else
    {
        // Existing chunk update - queue rebuild
        for (int32 ScatterTypeID : AffectedTypes)
        {
            QueueRebuild(ScatterTypeID);
        }
    }
}
```

### Chunk Unload

When a chunk unloads, all its scatter types are released back to the pool:

```cpp
void UVoxelScatterRenderer::RemoveChunkInstances(const FIntVector& ChunkCoord)
{
    // For each scatter type in this chunk:
    //   ReleaseChunkScatterType(ChunkCoord, TypeID)
    // Also discard any pending adds for this chunk
}
```

No rebuild is needed — instances silently return to the free list.

### Distance Streaming Cleanup

When scatter types go out of range, the manager calls `ReleaseChunkScatterType()` directly instead of the previous stale-type tracking approach. Instances are zero-scaled immediately (CullDistance already hides them at that range) and recycled when the player returns and new transforms are generated.

### Debug Stats

`GetDebugStats()` reports pool health:
```
ScatterRenderer: 5 HISM, 12400 instances (Active: 8200, Pooled: 4200, Util: 66%), 42 chunks, Pending: 0 rebuilds/0 adds
```

- **Active**: Instances with real transforms (visible within CullDistance)
- **Pooled**: Zero-scaled instances on free list (invisible, zero render cost)
- **Util**: Active / TotalAllocated — approaches 100% after initial exploration stabilizes

## Edit Integration

### Edit Source System

Scatter responds differently based on edit source:

```cpp
enum class EEditSource : uint8
{
    Player,  // Surgical removal, no regeneration
    System,  // Full regeneration allowed (POIs, game events)
    Editor   // Full regeneration allowed
};
```

### Targeted Removal

Player edits remove only instances within the brush radius:

```cpp
void UVoxelScatterManager::ClearScatterInRadius(const FVector& WorldPosition, float Radius)
{
    // Find affected chunks
    for (each affected chunk)
    {
        // Track cleared volume (prevents regeneration)
        ClearedVolumesPerChunk.FindOrAdd(ChunkCoord).Add(FClearedScatterVolume(WorldPosition, Radius));

        // Remove spawn points within radius
        FChunkScatterData* ScatterData = ScatterDataCache.Find(ChunkCoord);
        for (int32 i = ScatterData->SpawnPoints.Num() - 1; i >= 0; --i)
        {
            if (FVector::DistSquared(ScatterData->SpawnPoints[i].Position, WorldPosition) <= Radius * Radius)
            {
                ScatterTypesToRebuild.Add(ScatterData->SpawnPoints[i].ScatterTypeID);
                ScatterData->SpawnPoints.RemoveAt(i);
            }
        }
    }

    // Queue rebuilds for affected scatter types only
    for (int32 ScatterTypeID : ScatterTypesToRebuild)
    {
        ScatterRenderer->QueueRebuild(ScatterTypeID);
    }
}
```

### Cleared Volume Tracking

Prevents scatter from regenerating in player-edited areas:

```cpp
struct FClearedScatterVolume
{
    FVector Center;
    float Radius;

    bool ContainsPoint(const FVector& Point) const
    {
        return FVector::DistSquared(Center, Point) <= (Radius * Radius);
    }
};

// During scatter generation, skip cleared volumes
if (IsPointInClearedVolume(ChunkCoord, WorldPos))
{
    continue;  // Don't place scatter here
}

// Cleared volumes are removed when chunk fully unloads
// This allows scatter to regenerate when player returns later
void UVoxelScatterManager::OnChunkUnloaded(const FIntVector& ChunkCoord)
{
    ClearedVolumesPerChunk.Remove(ChunkCoord);
    // ...
}
```

## Configuration

### VoxelScatterConfiguration Asset

Editor-configurable scatter definitions:

```cpp
UCLASS(BlueprintType)
class UVoxelScatterConfiguration : public UDataAsset
{
public:
    UPROPERTY(EditAnywhere, Category = "Scatter")
    TArray<FScatterDefinition> ScatterDefinitions;

    UPROPERTY(EditAnywhere, Category = "Scatter")
    float SurfacePointSpacing = 100.0f;

    UPROPERTY(EditAnywhere, Category = "Scatter")
    bool bUseDefaultsIfEmpty = true;
};
```

### Default Scatter Types

Created automatically if no configuration is provided:

| Type | Density | Slope | Materials | Notes |
|------|---------|-------|-----------|-------|
| Grass | 50% | 0-30° | Grass | Top faces only, dense |
| Rocks | 5% | 0-60° | Stone, Dirt | Can appear on slopes |
| Trees | 2% | 0-20° | Grass | Very sparse, flat terrain |

## Performance Considerations

### Throttling

- `MaxScatterGenerationsPerFrame = 2`: Limits surface extraction per frame
- `MaxInstanceAddsPerFrame = 2000`: Budget for instance additions/recycling per frame
- `MaxRebuildsPerFrame = 0` (unlimited): Rebuilds are fast once transforms are collected
- `RebuildStationaryDelay = 0.5s`: Prevents flicker during movement
- World stability check: Rebuilds wait for `GetPendingGenerationCount() == 0` (prevents initial load flicker)

### Memory

- Surface data cached per chunk (`SurfaceDataCache`)
- Scatter spawn points cached per chunk (`ScatterDataCache`)
- HISM components shared across chunks (one per scatter type)
- Instance pool per scatter type (`InstancePools`) — tracks free list and per-chunk indices
- Pool never shrinks: peak allocation is the high water mark. Zero-scaled instances cost only a `FTransform` in HISM internal arrays and have zero rendering cost (zero bounding box = culled by spatial tree)

### Distance-Based Loading

- Per-definition `SpawnDistance` allows different ranges per scatter type
- Trees can have larger spawn distance to prevent pop-in
- Grass can have smaller distance for performance
- Out-of-range cleanup releases instances to pool via `ReleaseChunkScatterType()` (no rebuild)
- Returning to an area recycles from pool — faster than initial load since instances already exist in HISM

## Debug Visualization

Enable via `bScatterDebugVisualization` in VoxelWorldConfiguration:

- Spawn points: Colored spheres per scatter type
- Surface normals: Blue lines
- Scatter radius: Yellow sphere around viewer

## Async Generation (Phase 7D-1)

Surface extraction and scatter placement now run on the thread pool, keeping the game thread free.

### Data Flow
```
Game Thread:  Pop queue -> Launch Async
Thread Pool:               Extract -> Place -> Enqueue Result
Game Thread:               Drain Result -> Cache -> HISM Update
```

### Configuration
- `MaxAsyncScatterTasks` (1-4, default 2): Max concurrent async tasks
- `MaxScatterGenerationsPerFrame` (default 2): Max tasks launched per frame

### Edge Cases
- **Chunk unloaded during async**: Result arrives, chunk not in tracking -> discarded
- **Edit during async**: Cleared volumes updated on game thread; stale result discarded
- **GetPendingGenerationCount()**: Returns queued + in-flight count for "world stable" check
- **Shutdown safety**: `TWeakObjectPtr<UVoxelScatterManager>` in lambda prevents use-after-free

### Key Files
- `VoxelScatterManager.h`: `FAsyncScatterResult`, `CompletedScatterQueue`, `AsyncScatterInProgress`
- `VoxelScatterManager.cpp`: `LaunchAsyncScatterGeneration()`, `ProcessCompletedAsyncScatter()`

## GPU Surface Extraction (Phase 7D-2)

Optional GPU compute shader path for surface point extraction. Enabled via `bUseGPUScatterExtraction` in VoxelWorldConfiguration.

### Architecture
```
Game Thread:  Prepare request -> ENQUEUE_RENDER_COMMAND
Render Thread:                   Upload -> Dispatch CS -> Readback -> Enqueue result
Game Thread:  Drain GPU result -> Filter cleared volumes -> Launch CPU placement on thread pool
Thread Pool:                     Scatter placement -> Enqueue final result
Game Thread:  Drain final result -> Cache -> HISM Update
```

### Compute Shader
- File: `Shaders/Private/ScatterSurfaceExtraction.usf`
- Thread groups: `[numthreads(64, 1, 1)]`, one thread per mesh vertex
- Deduplication: Occupancy grid with `InterlockedCompareExchange`
- Output: `FSurfacePointGPU` (48 bytes) with atomic append counter

### Memory Per Dispatch
- Upload: ~400KB (10K vertices * 4 buffers)
- Occupancy grid: ~32KB (32x32x32 cells)
- Output: ~192KB (4096 points * 48 bytes max)
- Total: ~624KB per chunk, ~1.2MB with 2 concurrent

### Fallback
If GPU unavailable or `bUseGPUScatterExtraction = false`, seamlessly falls back to CPU async path.

### Key Files
- `ScatterSurfaceExtraction.usf`: GPU compute shader
- `VoxelGPUSurfaceExtractor.h/cpp`: RDG dispatch and readback
- `VoxelScatterManager.cpp`: `ProcessCompletedGPUExtractions()`

## Voxel-Based Surface Extraction (Phase 7D-5)

The default CPU extraction path uses voxel data instead of mesh vertices, making scatter generation **LOD-independent**.

### Why Voxel-Based?

Mesh-vertex-based extraction produces different results at different LOD levels because:
- LOD stride reduces vertex count (LOD 1 = half vertices, LOD 2 = quarter, etc.)
- Fewer surface points means lower scatter density at higher LODs
- Vertex positions shift slightly with LOD interpolation

Voxel data is **always 32³ at base VoxelSize** regardless of LOD level, so the same chunk produces identical surface points whether it's at LOD 0 or LOD 3.

### Algorithm

1. For each column (X, Y) at `SurfacePointSpacing` stride:
2. Scan Z from top (ChunkSize-1) to bottom (0)
3. Find first solid voxel (`Density >= 127`) with air above
4. Interpolate exact Z: `t = (127 - D_solid) / (D_air - D_solid)`
5. Compute normal from density gradient (central differences)
6. Read MaterialID and BiomeID directly from `FVoxelData`
7. Skip points inside cleared volumes (player edits)

### Key Properties
- **Deterministic**: Same voxel data always produces same surface points
- **LOD-independent**: No LOD-tiered filtering needed
- **CompletedScatterTypes**: Tracks which types have been generated per chunk to prevent duplicates
- **Supplemental passes**: Distance-based definitions still added when player approaches

### Key Files
- `VoxelScatterManager.h`: `ExtractSurfacePointsFromVoxelData()` declaration, `FPendingScatterGeneration::ChunkVoxelData`
- `VoxelScatterManager.cpp`: `ExtractSurfacePointsFromVoxelData()` implementation, integration in `LaunchAsyncScatterGeneration()`
- `VoxelChunkManager.cpp`: Passes `VoxelData`, `ChunkSize`, `VoxelSize` via `OnChunkMeshDataReady()`

## Cubic Terrain Scatter (Phase 7E)

Cubic meshing mode uses specialized scatter behavior: block-face snapping, cross-billboard meshes for grass/flowers, and voxel tree injection for editable terrain trees.

### Block-Face Snap Placement

When `MeshingMode == Cubic`, scatter uses `ExtractSurfacePointsCubic()` instead of the smooth-terrain voxel column scan:

- One surface point per exposed top block face
- Position at face center: `(X + 0.5) * VoxelSize, (Y + 0.5) * VoxelSize, (Z + 1) * VoxelSize`
- Normal always `FVector::UpVector`, SlopeAngle always 0
- No density interpolation — exact block-face positions
- Position jitter is skipped for `BlockFaceSnap` placement mode

Routed automatically when `Configuration->MeshingMode == EMeshingMode::Cubic` in both sync and async scatter paths.

### Scatter Mesh Types

`EScatterMeshType` (VoxelCoreTypes.h):

| Type | Description | Use Case |
|------|-------------|----------|
| `StaticMesh` | Standard assigned static mesh via HISM | Rocks, decorations |
| `CrossBillboard` | Runtime 2-quad cross mesh with alpha-tested material | Grass, flowers |
| `VoxelInjection` | Trees stamped directly into VoxelData | Editable/destructible trees |

### Billboard Scatter

Cross-billboard meshes are generated at runtime by `FVoxelBillboardMeshGenerator`:

- **Geometry**: 2 quads intersecting at 90 degrees (XZ + YZ planes), pivot at bottom center
- **Vertices**: 8 vertices, 4 triangles per instance
- **UVs**: Baked into mesh vertices — atlas mode uses tile region UVs, standalone uses [0,1]
- **Mesh cache**: Keyed by hash of (width, height, UVMin, UVMax) to avoid re-creation
- **Material**: Two-sided masked with `TextureSampleParameter2D("BaseTexture")` → BaseColor (RGB) + OpacityMask (Alpha)

#### Billboard Atlas Support

Multiple billboard types can share a single atlas texture with per-tile UV offsetting:

```cpp
// FScatterDefinition billboard atlas fields:
bool bUseBillboardAtlas = false;
TSoftObjectPtr<UTexture2D> BillboardAtlasTexture;
int32 BillboardAtlasColumn = 0;  // Tile column in atlas grid
int32 BillboardAtlasRow = 0;     // Tile row in atlas grid
int32 BillboardAtlasColumns = 4; // Atlas grid columns
int32 BillboardAtlasRows = 4;    // Atlas grid rows
```

- Atlas tile UVs computed in `CreateHISMComponent()`: `UVMin = (col/cols, row/rows)`, `UVMax = ((col+1)/cols, (row+1)/rows)`
- UVs baked into billboard mesh vertices (different mesh per tile, shared material)
- **Half-texel inset** applied to prevent bilinear filtering bleed at atlas tile boundaries
- Definitions sharing same atlas texture share one material instance but have different meshes

#### Billboard Material

`CreateBillboardMaterial()` creates a `UMaterialInstanceDynamic`:
1. Tries to load user-provided `/VoxelWorlds/Materials/M_Billboard_Master`
2. Falls back to runtime-generated `UMaterial` with proper settings:
   - `TwoSided = true`, `BlendMode = BLEND_Masked`, `bUsedWithInstancedStaticMeshes = true`
   - `TextureSampleParameter2D("BaseTexture")` connected to BaseColor + OpacityMask
   - Material expressions created via `#if WITH_EDITORONLY_DATA`
   - Cached in static `CachedRuntimeBillboardBaseMaterial`

### Default Cubic Scatter Definitions

Created when `MeshingMode == Cubic`:

| ID | Name | Type | Size | Density | Materials | Cull Dist |
|----|------|------|------|---------|-----------|-----------|
| 100 | CubicGrass | CrossBillboard | 80x80cm | 0.4 | Grass | 5000 |
| 101 | CubicFlowers | CrossBillboard | 60x60cm | 0.08 | Grass | 4000 |
| 102 | CubicRocks | StaticMesh | — | 0.05 | Stone, Dirt | 20000 |

### Scatter Edit Integration for Block Edits

When a block is broken (discrete edit), scatter on the block's top face must also be removed. The scatter clear radius is padded by `VoxelSize * 0.6` to account for the half-block offset between the edit center (block center) and the scatter position (top face center):

```cpp
// VoxelChunkManager OnChunkEdited handler:
const float ScatterClearRadius = EditRadius + Configuration->VoxelSize * 0.6f;
ScatterManager->ClearScatterInRadius(EditCenter, ScatterClearRadius);
```

## Voxel Tree System (Phase 7E)

### Tree Configuration

Tree settings in `UVoxelWorldConfiguration`:

```cpp
EVoxelTreeMode TreeMode = EVoxelTreeMode::VoxelData;
float VoxelTreeMaxDistance = 10000.0f;  // "Both" mode threshold
TArray<FVoxelTreeTemplate> TreeTemplates;
float TreeDensity = 1.5f;  // Trees per chunk (fractional = probability)
```

### Tree Templates (FVoxelTreeTemplate)

Each template defines trunk, canopy, and placement rules:

```cpp
struct FVoxelTreeTemplate
{
    // Identity
    int32 TemplateID;
    FString Name;

    // Trunk
    int32 TrunkHeight = 6;           // voxels
    int32 TrunkHeightVariance = 2;   // +/- random
    int32 TrunkRadius = 0;           // 0=1x1, 1=3x3 cross
    uint8 TrunkMaterialID = 20;      // Wood

    // Canopy
    ETreeCanopyShape CanopyShape;     // Sphere, Cone, FlatDisc, RoundedCube
    int32 CanopyRadius = 3;
    int32 CanopyRadiusVariance = 1;
    uint8 LeafMaterialID = 21;       // Leaves
    int32 CanopyVerticalOffset = 0;

    // Placement Rules
    TArray<uint8> AllowedMaterials;   // Surface material filter (empty = all)
    TArray<uint8> AllowedBiomes;      // Biome ID filter (empty = all)
    float MinElevation = -1000000.0f;
    float MaxElevation = 1000000.0f;
    float MaxSlopeDegrees = 30.0f;
};
```

Default templates:

| Template | Height | Canopy | Materials | Max Slope |
|----------|--------|--------|-----------|-----------|
| Oak | 5-8 | Sphere R=3 | Grass | 30 deg |
| Birch | 7-11 | Sphere R=2 | Grass | 25 deg |
| Bush | 2-3 | Sphere R=2 | Grass, Dirt | 40 deg |

### Voxel Tree Injection (FVoxelTreeInjector)

Stateless, thread-safe static class that stamps tree blocks into VoxelData during async generation.

**Cross-chunk algorithm**:
1. Compute max tree extent from templates → neighbor search radius (usually 1 chunk each direction)
2. For current chunk AND up to 26 neighbors, call `ComputeTreePositionsForChunk()`:
   - Deterministic seed from `HashCombine(WorldSeed, SourceChunkCoord)`
   - `numTrees = floor(TreeDensity) + probability(fractional_part)`
   - Random X,Y within source chunk → sample `WorldMode.GetTerrainHeightAt()` for Z
   - Filter by placement rules: material, biome, slope, elevation, water level
3. For each tree position, if bounding box overlaps current chunk: call `StampTree()`

**StampTree() algorithm**:
```
actualHeight = TrunkHeight + RandomVariance(seed)
actualCanopyR = CanopyRadius + RandomVariance(seed)

// Trunk: column of solid Wood voxels
for z in [0, actualHeight):
    if within chunk bounds: set MaterialID=Wood, Density=255

// Canopy: shape around top of trunk
canopyCenter = base + (0, 0, actualHeight + CanopyVerticalOffset)
for each voxel in canopy bounding box:
    if within shape AND within chunk AND current voxel is air:
        set MaterialID=Leaves, Density=255
```

**Integration**: Called in `VoxelChunkManager::LaunchAsyncGeneration()` after `GenerateChunkCPU()` completes, conditioned on `MeshingMode == Cubic && TreeMode != HISM && TreeTemplates.Num() > 0`.

**Placement filtering**: `QuerySurfaceConditions()` replicates the CPU noise generator's biome pipeline (temperature/moisture noise → biome selection → surface material) using static `FBM3D()`. This ensures trees respect biome boundaries without requiring the full noise generator.

**Edit integration**: Trees are part of VoxelData, so player edits work naturally — dig through trunks, remove leaves, etc. The edit layer preserves changes across chunk reload.

### Tree Mode Filtering

`EVoxelTreeMode` controls how trees are rendered:

| Mode | VoxelData Injection | HISM Scatter | Use Case |
|------|--------------------:|-------------:|----------|
| VoxelData | Yes (all distances) | No | Editable trees, performance cost |
| HISM | No | Yes (all distances) | Visual-only, best performance |
| Both | Yes (near) | Yes (far) | Hybrid: edit nearby, visual far |

In `OnChunkMeshDataReady()`, VoxelInjection scatter definitions are filtered based on TreeMode:
- **VoxelData**: Skip all VoxelInjection defs (trees already in terrain mesh)
- **HISM**: Include VoxelInjection defs (rendered as HISM instances)
- **Both**: Skip within `VoxelTreeMaxDistance`, include beyond

### Key Files (Phase 7E)

| File | Purpose |
|------|---------|
| `VoxelCore/Public/VoxelTreeTypes.h` | FVoxelTreeTemplate, EVoxelTreeMode, ETreeCanopyShape |
| `VoxelGeneration/Public/VoxelTreeInjector.h` | Tree injection API |
| `VoxelGeneration/Private/VoxelTreeInjector.cpp` | Cross-chunk deterministic injection + placement rules |
| `VoxelScatter/Public/VoxelBillboardMeshGenerator.h` | Billboard mesh generation API |
| `VoxelScatter/Private/VoxelBillboardMeshGenerator.cpp` | Runtime cross-billboard mesh + material |

## Future Enhancements

1. ~~**GPU-based scatter generation**~~: COMPLETE (7D-2)
2. **Foliage LOD**: Leverage HISM's built-in LOD system
3. **Procedural variation**: Per-instance custom data for shader variation
4. ~~**Async surface extraction**~~: COMPLETE (7D-1)
5. ~~**Voxel-based surface extraction**~~: COMPLETE (7D-5)
6. ~~**Cubic terrain scatter**~~: COMPLETE (7E)
7. ~~**Instance pooling**~~: COMPLETE — Per-HISM free list recycling via `UpdateInstanceTransform`, eliminates flicker from `ClearInstances`/`AddInstances` cycling
8. **GPU scatter placement** (7D-3): Move rule evaluation to compute shader
9. **Direct GPU->HISM** (7D-4): Eliminate readback by writing transforms directly
10. **Tree physics**: Unsupported leaf blocks fall when trunk is destroyed

## See Also

- [BIOME_SYSTEM.md](BIOME_SYSTEM.md) - Biome-scatter integration
- [EDIT_LAYER.md](EDIT_LAYER.md) - Edit system integration
- [IMPLEMENTATION_PHASES.md](IMPLEMENTATION_PHASES.md) - Phase 7 details
