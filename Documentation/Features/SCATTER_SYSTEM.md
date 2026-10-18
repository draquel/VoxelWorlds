# Scatter System

**Module**: VoxelScatter
**Dependencies**: VoxelCore, VoxelStreaming

## Overview

The scatter system places vegetation (trees, grass, rocks) on voxel terrain using CPU-based placement and Hierarchical Instanced Static Mesh (HISM) rendering. The system extracts surface points from voxel data, applies placement rules, and manages HISM instances with optimized rebuild strategies. All scatter generation runs asynchronously on the thread pool.

## Architecture

```
VoxelChunkManager
    │
    ├── OnChunkMeshDataReady(ChunkCoord, VoxelData, ChunkSize, VoxelSize)
    │       │
    │       ▼
    │   VoxelScatterManager
    │       │
    │       ├── ExtractSurfacePointsFromVoxelData() (cubic: ExtractSurfacePointsCubic, planet: ExtractSurfacePointsAlongUp)
    │       │       Reads the chunk's (edit-merged) voxels directly: LOD-independent, never the mesh
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

    // Chunk hand-off (called by ChunkManager when a chunk's mesh is ready; reads the voxels, not the mesh)
    void OnChunkMeshDataReady(const FIntVector& ChunkCoord, const TArray<FVoxelData>& VoxelData, int32 ChunkSize, float VoxelSize);
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

    // Replace one chunk+type's instances in place (empty = release). No global rebuild.
    void UpdateChunkTypeInstances(const FIntVector& ChunkCoord, int32 ScatterTypeID, TArray<FTransform> Transforms);

protected:
    // One HISM per scatter type
    TMap<int32, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HISMComponents;

    // Per-scatter-type instance pool for recycling
    TMap<int32, FHISMInstancePool> InstancePools;

    // Deferred per-frame instance adds (budget-limited, flushed every Tick)
    TArray<FPendingInstanceAdd> PendingInstanceAdds;
    int32 MaxInstanceAddsPerFrame = 2000;
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
    int32 SurfaceLocationMask = Surface;     // bitmask of EScatterSurfaceLocationFlags
    // Surface (dry land) | Underwater | Underground — enable any combination.
    // See UNDERGROUND_CLASSIFICATION.md. Underwater = open-sky surface below WaterLevel
    // (air voxel above carries VOXEL_FLAG_WATER); Underground takes precedence.

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

#### On a Planet: Radial Rays on the Cube-Sphere Grid (spherical Phase 4, SP-G1; world modes review G31)

World-Z columns cross a planet's surface at a slant away from the poles and run along it at the equator, so on a
planet `UVoxelScatterManager::ExtractSurfacePointsAlongUp` takes the column scan's columns to be the planet's **radial
lines**: one ray from the centre through the centre of each cell of the cube-sphere grid (`FVoxelCubeSphereGrid`,
VoxelCore) whose cells at the face centres are the point spacing across (`CellsPerEdgeFor(PlanetRadius, Stride *
VoxelSize)`). The grid is the same for every chunk, so neighbouring chunks sample one family of rays, and each chunk
keeps the crossings that lie in its own box, [0, ChunkSize) on each axis: every crossing has one owner, along a face as
anywhere else.

- **Down a ray** through the chunk (a 3D DDA over its voxel cells) the density is the trilinear interpolation of the
  voxels, sampled five times in each cell whose corners change sign (a cell whose corners agree has one sign
  throughout). Every rise through the surface threshold going down is a floor: the first on the ray is open, the rest
  are underground ("not the topmost", as in a column), and a ray that enters the chunk in the ground has its surface
  above it, so its floors are all underground. The air and solid corners nearest the crossing give the flags
  (underground, water) and the material, biome and AO; the normal is the trilinear gradient.
- **Past the faces.** The chunk's last cell on each axis reaches the voxels past its +X, +Y and +Z faces, which its
  data cannot show: they are taken as the planet generator makes them there, with the neighbour's edits applied
  (`FScatterExtractionContext::GetPlanetVoxelOutsideChunk`; `TouchingEdits` from the seven neighbours on those sides,
  `UVoxelScatterManager::GetPlanetShellNeighbours` / `GatherPlanetTouchingEdits`), each computed the first time a ray
  needs it (0.12 µs each, at most 12,481 for a 64³ chunk). Caves are not in them: a cave within a voxel past a chunk's
  max face reads as rock. They are the CPU formula's, which the GPU generator matches to a density step or two at a few
  voxels (`Generation.GPUvsCPUSphericalParity`), so on the GPU path a crossing at a face is very rarely doubled (one
  point in about two million on `Review_Planet`). A point on a face is interpolated from the same four voxels on both
  sides (fractions within 1e-7 of a cell face snap to it), so a voxel exactly at the threshold there cannot leave the
  crossing to neither chunk.
- **Classification** by the point's own radial up (the ray) and altitude: face type and slope against local up, the
  elevation filter compares the altitude above the datum (heightmap worlds: world Z, as before), the underwater class by
  the sea sphere (`FVoxelSeaSurface`).
- **Density per area**: the grid's cells shrink away from the face centres (to 0.71 of the area at an edge's midpoint),
  so the rays meet the surface up to 1.42 times as often as the column scan's columns. Each point's spawn weight is its
  cell's area at the point's radius over the column scan's area per point (Stride² voxels): at most 1 on the datum
  (a face centre's cell) and (r / R)² over it, so the weighted density per area is the column scan's on any part of the grid: 100.2% to 101.0%
  at the poles, 45°, the equator and a cube corner (`Scatter.Planet.DensityPerArea`), and no strip with too many or too
  few points at any of ten sites (`Scatter.Planet.LineTop.Duplicates`).
- **Instances** stand on local up (`FScatterSpawnPoint::LocalUp`, `GetRotationQuat`: `FindBetweenNormals(+Z, Up)`
  then the yaw about it; exact near the south pole), or on the surface normal with `bAlignToSurfaceNormal`. The jitter
  is in the tangent plane, from the same two random draws.
- **Harvest exclusion**: a felled tree's box turns to local up, its centre along up from the base; the id is the
  base position's, as before.
- **On the CPU**, as every world since G27 (the retired GPU extractor's gates were along +Z anyway).

Until G31 each planet chunk walked digital lines along local up at its own centre, stepping along the axis nearest it and
drifting sideways by the rounded slope. Neighbouring chunks' lines did not continue into each other (across the cube's
diagonals they stepped along different axes, and elsewhere their drift rounded differently), so strips along chunk faces
got too few points; the staircase rounding also thinned the points inside a chunk on some slopes, and a line top that
left the chunk needed G29's rules to decide whether the neighbour made its point. The rays need none of that.
`voxel.Scatter.LineTopAudit` still measures it on a live planet: pairs of open points from two chunks on one up-line,
and the local density around the pawn.

Heightmap worlds keep `ExtractSurfacePointsFromVoxelData` and the old expressions (`LocalUp` +Z, weight 1, the jitter
call unchanged). `voxel.Scatter.Digest` logs the cached spawn points' counts and a CRC per type, to compare builds
(extraction is deterministic; the retired GPU extractor's per-cell claim was not). Tests: `Scatter.Planet.AlongLocalUp`
(points within 8 uu of the surface, instances' up within 0.01° of the radial), `Scatter.Planet.DensityPerArea`,
`Scatter.Planet.LineTop.{Duplicates,FlatPad,Edits}` (one open point per ray at ten sites, over a flat pad at every height
across a chunk face, and over edits past the faces).

#### Mesh-vertex extraction (retired, world modes review G27)

Scatter used to have two paths that took surface points from mesh vertices: the GPU extractor
(`FVoxelGPUSurfaceExtractor`, `ScatterSurfaceExtraction.usf`, `bUseGPUScatterExtraction`, LOD 0 chunks only) and an
uncalled CPU one (`FVoxelSurfaceExtractor`). Both were deleted in G27 (`WORLD_MODES_REVIEW_PLAN.md` §19.5). Since seam
ownership a chunk's mesh is its interior mesh, which has no vertex in the cells along the chunk's +X, +Y and +Z faces
(they belong to the chunk's seams), so every chunk the GPU path extracted had no point there: bare strips in the grass
along chunk borders and along the contour where the terrain crosses a chunk layer's top. Its output buffer also held
4,096 points, sized for 32³ chunks, and dropped whichever points came last on the GPU (not deterministic either way).
`bUseGPUScatterExtraction` is kept so assets load, and ignored (logged once at initialisation).
`voxel.Scatter.BoundaryAudit` measures how a chunk's points cover the cell layers along its six faces.

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

#### Per-Chunk-Type Updates (no global rebuilds)

All instance updates are scoped to a single (chunk, scatter type) pair — the renderer
**never** releases and re-streams a whole scatter type across the world. That whole-type
rebuild used to zero-scale every instance of a type in one frame and re-add them budget-limited
over many frames, and because it was deferred until the viewer was stationary AND the generation
pipeline was idle, it produced a world-wide "refresh flash" whenever the player stopped moving.
It (and `QueueRebuild`/`RebuildScatterType`/`FlushPendingRebuilds`/`ReleaseAllForScatterType`)
has been removed.

Updates flow through two localized primitives, both flushed every `Tick()` via the per-frame
add budget (no viewer-stationary / world-stable gate):

- `AddSupplementalInstances(chunk, newData)` — append only the newly generated points for a
  chunk (used by distance streaming and by supplemental chunk-generation passes, which only
  ever add types the chunk doesn't already have).
- `UpdateChunkTypeInstances(chunk, type, transforms)` — replace just this chunk+type's
  instances (empty transforms = release). Used by edits and by re-mesh/regen of a chunk that
  already has scatter.

### New Chunk Optimization

New chunks (no existing scatter) group their spawn points by type and queue them as
`PendingInstanceAdd` entries; `FlushPendingInstanceAdds` recycles from the free list first,
then grows the pool. A chunk that already has scatter takes the per-chunk-type path above —
each affected type is replaced in place, so only that one chunk's instances ever move:

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
        // Existing chunk: replace each affected type in place (local, no global rebuild)
        for (int32 ScatterTypeID : AffectedTypes)
        {
            UpdateChunkTypeInstances(ChunkCoord, ScatterTypeID, ThisChunkTransformsForType);
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

On the manager's side (`UVoxelScatterManager::OnChunkUnloaded`) an unloaded chunk keeps no scatter state: its queued and
in-flight work (pending queue, in-flight extraction and distance stream, deferred supplemental pass, queued exclusion
regrow), its cleared volumes and completed types, and both caches (`RemoveChunkScatter`: the surface data and the spawn
points). When it streams back in, its extraction is a first pass, as on its first load: the surface data is cached
again (the distance spawn and the audits read it) and its spawn points replace nothing. Until world modes review G30 the
two cache removals were joined by a short-circuit `||`, so a chunk with surface data kept its spawn points cached after
unloading and its reload was taken for a supplemental pass (`WORLD_MODES_REVIEW_PLAN.md` §19.7).
`voxel.Scatter.Digest` logs a second line, `caches:` (`ComputeCacheResidency`), that checks this: chunks with spawn
points, with surface data, with spawn points but no surface data (0), spawn points against the renderer's instances
(`UVoxelScatterRenderer::GetChunkInstanceCount`; the chunks that differ are logged per type, `file` writes them to
`ScatterCaches_<world>_<time>.csv`), and spawn points that repeat another of their chunk.

### Distance Streaming Cleanup

When scatter types go out of range, the manager calls `ReleaseChunkScatterType()` directly instead of the previous stale-type tracking approach. Instances are zero-scaled immediately (CullDistance already hides them at that range) and recycled when the player returns and new transforms are generated. The release also drops the pair's adds still queued for the per-frame budget: a chunk submitted while the viewer was still near it (in the seconds after a teleport) can take a distance-limited type in its first pass and release it at the next check, behind the arrival's backlog. Until world modes review G30 those adds were flushed after the release into the pool for a type the chunk no longer tracked, which `RemoveChunkInstances` never released, so they stayed in the world and the chunk's reload drew the type a second time over them.

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
        // Track cleared volume (prevents regeneration) and scrub cached surface points in
        // the radius (so distance streaming can't resurrect scatter from pre-edit data)
        ClearedVolumesPerChunk.FindOrAdd(ChunkCoord).Add(FClearedScatterVolume(WorldPosition, Radius));

        // Remove spawn points within radius, collecting the affected types
        FChunkScatterData* ScatterData = ScatterDataCache.Find(ChunkCoord);
        for (int32 i = ScatterData->SpawnPoints.Num() - 1; i >= 0; --i)
        {
            if (FVector::DistSquared(ScatterData->SpawnPoints[i].Position, WorldPosition) <= Radius * Radius)
            {
                AffectedTypes.Add(ScatterData->SpawnPoints[i].ScatterTypeID);
                ScatterData->SpawnPoints.RemoveAt(i);
            }
        }

        // Replace just this chunk's affected types with the surviving transforms —
        // immediate, local, flushed every Tick. No whole-type rebuild, so no refresh flash.
        for (int32 TypeID : AffectedTypes)
        {
            ScatterRenderer->UpdateChunkTypeInstances(ChunkCoord, TypeID, SurvivingTransformsForType);
        }
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
- `MaxInstanceAddsPerFrame = 2000`: Budget for instance additions/recycling per frame — the single knob for streaming smoothness now that whole-type rebuilds are gone

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
- **Chunk unloaded and reloaded**: both caches were dropped on unload, so the reload's result is a first pass (cached,
  rendered through `UpdateChunkInstances`), never appended to the chunk's earlier spawn points (G30)
- **Edit during async**: Cleared volumes updated on game thread; stale result discarded
- **GetPendingGenerationCount()**: Returns queued + in-flight count for "world stable" check
- **Shutdown safety**: `TWeakObjectPtr<UVoxelScatterManager>` in lambda prevents use-after-free

### Key Files
- `VoxelScatterManager.h`: `FAsyncScatterResult`, `CompletedScatterQueue`, `AsyncScatterInProgress`
- `VoxelScatterManager.cpp`: `LaunchAsyncScatterGeneration()`, `ProcessCompletedAsyncScatter()`

## GPU Surface Extraction (Phase 7D-2, retired)

Retired in world modes review G27: see [Mesh-vertex extraction](#mesh-vertex-extraction-retired-world-modes-review-g27).
Every world extracts surface points from voxels on the thread pool (3.7 ms mean, 5.9 ms P95 per 64³ chunk on
`VoxelDemo`).

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
- `BlockFaceSnap` placement keeps an instance on the block face it was placed on: a definition's `PositionJitter` wider
  than half a face is scaled down to half a face (scaled, not clamped, so instances do not pile up on the face edges);
  a smaller jitter is unchanged. `bAutoCubicScatterDefaults` sets every definition to `BlockFaceSnap` in cubic worlds.

Routed automatically when `Configuration->MeshingMode == EMeshingMode::Cubic` in both sync and async scatter paths.
Cubic worlds always extracted on the CPU (finding KC4, work item WM-C3: the GPU extractor sampled mesh vertices, which
on greedy quads are block corners); since G27 every world does. The extractor reads voxels, not the mesh, so it is
unaffected by cubic chunk meshes being interior-only (the faces on chunk planes are seam meshes).
Tests: `VoxelWorlds.Scatter.Cubic.CPUExtractionOnly`, `.BlockFaceSnapStaysOnFace`.

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

1. ~~**GPU-based scatter generation**~~: COMPLETE (7D-2), retired in G27 (mesh vertices miss the chunk's boundary cells)
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

- [UNDERGROUND_CLASSIFICATION.md](UNDERGROUND_CLASSIFICATION.md) - Underground flag propagation and scatter surface/underground filtering
- [BIOME_SYSTEM.md](BIOME_SYSTEM.md) - Biome-scatter integration
- [CAVE_SYSTEM.md](CAVE_SYSTEM.md) - Cave generation (density subtraction, cave types)
- [EDIT_LAYER.md](EDIT_LAYER.md) - Edit system integration
- [IMPLEMENTATION_PHASES.md](IMPLEMENTATION_PHASES.md) - Phase 7 details
