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

Manages HISM components with deferred rebuild strategy.

```cpp
UCLASS()
class UVoxelScatterRenderer : public UObject
{
public:
    void Initialize(UVoxelScatterManager* Manager, UWorld* World);
    void Tick(const FVector& ViewerPosition, float DeltaTime);

    // Instance management
    void UpdateChunkInstances(const FIntVector& ChunkCoord, const FChunkScatterData& ScatterData);
    void RemoveChunkInstances(const FIntVector& ChunkCoord);

    // Deferred rebuilds (prevents flicker)
    void QueueRebuild(int32 ScatterTypeID);

protected:
    // One HISM per scatter type
    TMap<int32, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HISMComponents;

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
    float SpawnDistance = 0.0f;              // 0 = use global ScatterRadius
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

### Rebuild Strategy

The scatter renderer uses a **full rebuild** approach per scatter type:

1. **Why not individual RemoveInstance?**
   - HISM's `RemoveInstance()` shifts all subsequent indices
   - Index tracking becomes invalid after any removal
   - Complex to maintain chunk→instance mappings

2. **Rebuild approach:**
   - When a chunk changes, queue affected scatter types for rebuild
   - On rebuild: Clear all instances, collect from all chunks, batch add
   - Uses `HISM->AddInstances()` for efficient batch addition

3. **Deferred rebuilds:**
   - Rebuilds are queued via `QueueRebuild()`, not immediate
   - Processed in `Tick()` when BOTH conditions met:
     - Viewer stationary for 0.5s (`RebuildStationaryDelay`)
     - World stable (`GetPendingGenerationCount() == 0`)
   - Prevents flickering during movement and initial world loading

```cpp
void UVoxelScatterRenderer::RebuildScatterType(int32 ScatterTypeID)
{
    UHierarchicalInstancedStaticMeshComponent* HISM = GetOrCreateHISM(ScatterTypeID);

    // Collect all transforms FIRST
    TArray<FTransform> AllTransforms;
    for (const auto& ChunkPair : ChunkScatterTypes)
    {
        if (!ChunkPair.Value.Contains(ScatterTypeID)) continue;

        const FChunkScatterData* Data = ScatterManager->GetChunkScatterData(ChunkPair.Key);
        for (const FScatterSpawnPoint& Point : Data->SpawnPoints)
        {
            if (Point.ScatterTypeID == ScatterTypeID)
            {
                AllTransforms.Add(Point.GetTransform(...));
            }
        }
    }

    // Clear and add in quick succession
    HISM->ClearInstances();
    if (AllTransforms.Num() > 0)
    {
        HISM->AddInstances(AllTransforms, false, true);
    }
}
```

### New Chunk Optimization

New chunks (no existing scatter) bypass the rebuild system entirely:

```cpp
void UVoxelScatterRenderer::UpdateChunkInstances(const FIntVector& ChunkCoord, const FChunkScatterData& ScatterData)
{
    const bool bIsNewChunk = !ChunkScatterTypes.Contains(ChunkCoord);

    if (bIsNewChunk && ScatterData.bIsValid)
    {
        // Direct instance addition - no rebuild, no flicker
        TMap<int32, TArray<FTransform>> TransformsByType;
        // ... group transforms by scatter type ...

        for (auto& Pair : TransformsByType)
        {
            UHierarchicalInstancedStaticMeshComponent* HISM = GetOrCreateHISM(Pair.Key);
            HISM->AddInstances(Pair.Value, false, true);
        }
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
- `MaxRebuildsPerFrame = 0` (unlimited): Rebuilds are fast once transforms are collected
- `RebuildStationaryDelay = 0.5s`: Prevents flicker during movement
- World stability check: Rebuilds wait for `GetPendingGenerationCount() == 0` (prevents initial load flicker)

### Memory

- Surface data cached per chunk (`SurfaceDataCache`)
- Scatter spawn points cached per chunk (`ScatterDataCache`)
- HISM components shared across chunks (one per scatter type)

### Distance-Based Loading

- Per-definition `SpawnDistance` allows different ranges per scatter type
- Trees can have larger spawn distance to prevent pop-in
- Grass can have smaller distance for performance

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

## Future Enhancements

1. ~~**GPU-based scatter generation**~~: COMPLETE (7D-2)
2. **Foliage LOD**: Leverage HISM's built-in LOD system
3. **Procedural variation**: Per-instance custom data for shader variation
4. ~~**Async surface extraction**~~: COMPLETE (7D-1)
5. ~~**Voxel-based surface extraction**~~: COMPLETE (7D-5)
6. **Instance pooling**: Reuse instances when chunks unload/reload
7. **GPU scatter placement** (7D-3): Move rule evaluation to compute shader
8. **Direct GPU->HISM** (7D-4): Eliminate readback by writing transforms directly

## See Also

- [BIOME_SYSTEM.md](BIOME_SYSTEM.md) - Biome-scatter integration
- [EDIT_LAYER.md](EDIT_LAYER.md) - Edit system integration
- [IMPLEMENTATION_PHASES.md](IMPLEMENTATION_PHASES.md) - Phase 7 details
