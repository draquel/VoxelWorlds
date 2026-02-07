# Scatter System

**Module**: VoxelScatter
**Dependencies**: VoxelCore, VoxelStreaming

## Overview

The scatter system places vegetation (trees, grass, rocks) on voxel terrain using CPU-based placement and Hierarchical Instanced Static Mesh (HISM) rendering. The system extracts surface points from mesh data, applies placement rules, and manages HISM instances with optimized rebuild strategies.

## Architecture

```
VoxelChunkManager
    │
    ├── OnChunkMeshDataReady() ──► VoxelScatterManager
    │                                    │
    │                                    ├── VoxelSurfaceExtractor (extract surface points)
    │                                    │
    │                                    ├── VoxelScatterPlacement (apply rules, generate spawn points)
    │                                    │
    │                                    └── VoxelScatterRenderer (HISM instance management)
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

    // Rendering
    float CullDistance = 10000.0f;
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

When mesh data is ready, surface points are extracted using spatial hashing:

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

        // Spatial hash cell
        FIntVector Cell(
            FMath::FloorToInt(WorldPos.X / TargetSpacing),
            FMath::FloorToInt(WorldPos.Y / TargetSpacing),
            FMath::FloorToInt(WorldPos.Z / TargetSpacing)
        );

        if (OccupiedCells.Contains(Cell)) continue;

        // Extract surface point data from mesh
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
   - Rebuilds are queued, not immediate
   - Processed in `Tick()` when viewer has been stationary for 0.5s
   - Prevents flickering during movement

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
                AllTransforms.Add(Point.GetTransform(Definition->bAlignToSurfaceNormal, Definition->SurfaceOffset));
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

## Future Enhancements

1. **GPU-based scatter generation**: Move placement to compute shader
2. **Foliage LOD**: Leverage HISM's built-in LOD system
3. **Procedural variation**: Per-instance custom data for shader variation
4. **Async surface extraction**: Move to background threads
5. **Instance pooling**: Reuse instances when chunks unload/reload

## See Also

- [BIOME_SYSTEM.md](BIOME_SYSTEM.md) - Biome-scatter integration
- [EDIT_LAYER.md](EDIT_LAYER.md) - Edit system integration
- [IMPLEMENTATION_PHASES.md](IMPLEMENTATION_PHASES.md) - Phase 7 details
