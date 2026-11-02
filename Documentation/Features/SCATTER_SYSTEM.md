# Scatter System

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelScatter
**Dependencies**: VoxelCore, VoxelGeneration (the surface queries); the chunk manager (VoxelStreaming) drives it

## Overview

The scatter system places vegetation (trees, grass, rocks) on voxel terrain using CPU-based placement and Hierarchical Instanced Static Mesh (HISM) rendering. It extracts surface points from each chunk's voxels (never its mesh), applies placement rules, and manages HISM instances in pools whose writes are batched. Extraction and placement run on the thread pool.

## Architecture

```
VoxelChunkManager
    │
    ├── HandOffChunkVoxels(ChunkCoord, VoxelData, ChunkSize, VoxelSize)
    │       │
    │       ▼
    │   VoxelScatterManager
    │       │
    │       ├── ExtractSurfacePointsByColumn() (cubic: ExtractSurfacePointsByBlockTop, planet: ExtractSurfacePointsByRay)
    │       │       Reads the chunk's (edit-merged) voxels directly: LOD-independent, never the mesh
    │       │
    │       ├── VoxelScatterPlacement (apply rules, generate spawn points)
    │       │       Runs on thread pool (async)
    │       │
    │       └── VoxelScatterRenderer (HISM instance management)
    │               Game thread only
    │
    └── OnChunkEdited() ──► ClearScatterInRadius() (player) or RegenerateChunkScatter() (system / editor: the edited
                            chunk and the chunks whose extraction reads its edited voxels, see Edit Integration)
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
    const FScatterDefinition* GetScatterDefinition(int32 ScatterID) const;

    // Chunk hand-off (called by ChunkManager when a chunk's mesh is ready; reads the voxels, not the mesh)
    void HandOffChunkVoxels(const FIntVector& ChunkCoord, const TArray<FVoxelData>& VoxelData, int32 ChunkSize, float VoxelSize);
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

Manages HISM components with instance pooling and batched writes. Instances are hidden by setting scale to zero and placed on a free list. When new instances are needed, they are recycled from the free list first via `UpdateInstanceTransform`, eliminating the visual flicker caused by `ClearInstances()`/`AddInstances()` cycling. Since world modes review G42 every write to a HISM is held and applied at most once per `voxel.Scatter.HISMBatchSeconds`, releases zero-scale each instance where it stands, a HISM whose pool outweighs its live instances is rewritten without it, and the scatter HISMs are kept in regions of 4 chunks in every world mode (`voxel.Scatter.RegionChunks`; planets since G42, heightmap worlds since world modes review WM-MP5); see [Batched HISM writes](#batched-hism-writes-world-modes-review-g42).

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
    // One HISM per scatter type and region (regions of voxel.Scatter.RegionChunks^3 chunks, 4 by default in every world
    // mode, each HISM at its region's origin so instance transforms stay small floats)
    TMap<FVoxelScatterHISMKey, TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HISMComponents;

    // Per-HISM instance pool for recycling
    TMap<FVoxelScatterHISMKey, FHISMInstancePool> InstancePools;

    // Deferred per-frame instance adds (budget-limited, flushed every Tick)
    TArray<FPendingInstanceAdd> PendingInstanceAdds;
    int32 MaxInstanceAddsPerFrame = 2000;

    // Writes held per HISM for its next batch (world modes review G42): released and recycled indices, new instances
    TMap<FVoxelScatterHISMKey, FPendingHISMWrites> PendingHISMWrites;
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
    FName HarvestCategory;                   // "Tree", "Rock"...; None = not harvestable (HarvestScatterNear)
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
    TArray<uint8> AllowedMaterials;          // material IDs (empty = all)
    TArray<uint8> AllowedBiomes;
    float MinElevation = -1000000.0f;        // world Z; a planet's altitude over the datum
    float MaxElevation = 1000000.0f;
    bool bTopFacesOnly = true;
    bool bAvoidShadowedAreas = false;        // with MaxAmbientOcclusion (2)
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
USTRUCT(BlueprintType)
struct FScatterSpawnPoint
{
    FVector Position;           // World position
    FVector Normal;             // Surface normal
    int32 ScatterTypeID;        // Reference to definition
    uint32 InstanceSeed;        // The point's deterministic seed
    float Scale;                // Random scale
    float RotationYaw;          // Random yaw about local up (degrees)
    FVector LocalUp;            // +Z on a heightmap world, the radial direction on a planet

    FRotator GetRotation(bool bAlignToNormal) const;
};
```

## Placement Pipeline

### 1. Surface Extraction

Every world extracts surface points on the CPU from the chunk's **edit-merged voxels** (the chunk manager hands them over
at each mesh submit, `HandOffChunkVoxels`), so scatter is LOD-independent: voxel data is generated at full resolution at
any LOD, and the same chunk gives the same points at LOD 0 or LOD 3. One entry, `ExtractChunkSurfacePoints` (code review
CR2), picks the extractor:

| World | Extractor | Points |
|-------|-----------|--------|
| Heightmap, smooth | `ExtractSurfacePointsByColumn` | Every surface crossing down each (X, Y) column at the point spacing's stride, Z interpolated as the mesher does, normal from the density gradient |
| Heightmap, cubic | `ExtractSurfacePointsByBlockTop` | One point at the centre of each exposed top block face (below) |
| Planet | `ExtractSurfacePointsByRay` | Crossings down the planet's radial rays (below) |

Material, biome, AO and the underground and water flags come from the voxels at the crossing; the first crossing down a
column is open surface, the rest underground ([UNDERGROUND_CLASSIFICATION.md](UNDERGROUND_CLASSIFICATION.md)). Points
inside a cleared volume (a player edit) or an exclusion volume are skipped. The extraction is deterministic:
`voxel.Scatter.Digest` logs the cached spawn points' counts and a CRC per type, to compare builds.

#### The voxels past the chunk

An extractor reads a few voxels its chunk's data cannot show: on a heightmap world the voxel over each column's top
layer (the chunk above's bottom layer), on a planet the voxels past the chunk's +X, +Y and +Z faces. All three
extractors take them from one source, `FScatterExtractionContext::GetVoxelOutsideChunk` (code review CR2): the voxel as
generation makes it there (`FVoxelSurfaceQuery::GenerateVoxelAt`: the generated surface with the conditioning zones,
the cave carve from the world's `CaveConfiguration`), with the neighbour's edits there applied (`OutsideEdits`, from
`GatherAboveLayerEdits` on a heightmap world, `GatherPlanetTouchingEdits` on a planet, through the chunk manager's edit
layers, `SetEditLayerSource`). Tests can supply the neighbours' real voxels instead (`OutsideVoxelSource`).

What that rule fixed:
- **The generated surface, not the natural one** (G23). A buried chunk's test and the planet's outside voxels use the
  ground generation makes, POI pads and their falloff rings included (`ConditioningZones`, gathered by the chunk manager,
  `SetConditioningGatherer`). Before, a pad cut below a chunk's top left its ground points underground, and the chunk
  under a raised pad read as open surface.
- **The voxel over a heightmap chunk's top layer** (G26, G28). Solid, the chunk's top voxel is no surface (the chunk above
  extracts it) and the column below is covered; air, the crossing interpolates against it. Before G26 it was taken for
  air, which copied the surface onto the chunk's top face up to 1.5 voxels under the ground; before G28 it still was
  deeper than that, so a buried chunk's top face carried underground points inside the rock, where underground-only
  scatter spawned. Tests: `Scatter.TopFace.*`.
- **A planet chunk's faces** (G29, G72). The voxels past a planet chunk's faces are the planet generator's with the
  neighbours' edits, carved by the caves (`CalculateCaveDensity` along local up, for the biome generation gives that
  direction when the cave configuration has biome overrides). Before G72 a cave within a voxel past a chunk's max face
  read as rock: the cave floor there got no point, and where the cave met the face the rock made a floor in the air
  (1.3-1.6% of the points missed on `Review_Planet`, up to 7% added). Test: `Scatter.Planet.FaceCaves`.

#### On a Planet: Radial Rays on the Cube-Sphere Grid (spherical Phase 4, SP-G1; world modes review G31)

World-Z columns cross a planet's surface at a slant away from the poles and run along it at the equator, so on a
planet `UVoxelScatterManager::ExtractSurfacePointsByRay` takes the column scan's columns to be the planet's **radial
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
- **Past the faces.** The chunk's last cell on each axis reaches the voxels past its +X, +Y and +Z faces, which it takes
  from `GetVoxelOutsideChunk` (above), with the edits of the seven neighbours on those sides
  (`UVoxelScatterManager::GetPlanetShellNeighbours`), each computed the first time a ray needs it (at most 12,481 for a
  64³ chunk). The voxels past the faces are the CPU formula's, which the GPU generator matches to a density step or two
  at a few voxels (`Generation.GPUvsCPUSphericalParity`), so on the GPU path a crossing at a face is very rarely doubled
  (one point in about two million on `Review_Planet`). A point on a face is interpolated from the same four voxels on
  both sides (fractions within 1e-7 of a cell face snap to it), so a voxel exactly at the threshold there cannot leave
  the crossing to neither chunk.
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

Heightmap worlds keep `ExtractSurfacePointsByColumn` and the old expressions (`LocalUp` +Z, weight 1, the jitter
call unchanged). Tests: `Scatter.Planet.AlongLocalUp`
(points within 8 uu of the surface, instances' up within 0.01° of the radial), `Scatter.Planet.DensityPerArea`,
`Scatter.Planet.LineTop.{Duplicates,FlatPad,Edits}` (one open point per ray at ten sites, over a flat pad at every height
across a chunk face, and over edits past the faces).

#### Why not the mesh (G27)

Scatter once also took surface points from mesh vertices on the GPU. Since seam ownership a chunk's mesh is its interior
mesh, which has no vertex in the cells along the chunk's +X, +Y and +Z faces (they belong to its seams), so those chunks
had no point there: bare strips in the grass along chunk borders. That path was retired in world modes review G27
(decision D25); every chunk extracts from its voxels. `Scatter.Boundary.EveryCellLayerCovered` checks that a chunk's
points cover the cell layers along its faces.

### 2. Scatter Placement

`FVoxelScatterPlacement::GenerateSpawnPoints` runs each enabled definition over the chunk's surface points
(`GenerateSpawnPointsForType`), with deterministic randomness per point:

1. `FScatterDefinition::CanSpawnAt` filters the point: face type, surface location (`SurfaceLocationMask`), slope,
   material, biome, elevation, ambient occlusion.
2. The point's seed is a hash of its position and the chunk seed (`HashPosition`); it spawns when its first draw is
   under `Density` × the point's `SpawnWeight` (1 on heightmap worlds; a planet ray cell's area, above).
3. Scale, yaw and the position jitter come from the next draws (on a planet the jitter is in the tangent plane;
   `BlockFaceSnap` scales a jitter wider than half a block face down to it), then `SurfaceOffset` along the normal.

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

- **Release**: `UpdateInstanceTransform` with a zero-scale transform — instance becomes invisible but stays allocated in HISM internal arrays. Batched (the default) it keeps its location: an in-place change, which the HISM takes without rebuilding its cluster tree.
- **Recycle**: `UpdateInstanceTransform` with the new transform — instance reappears at its new position. No flicker, but not free: a move is not in place, so the HISM rebuilds its cluster tree (below).
- **Grow**: `AddInstances()` only when free list is exhausted.

The pool does not make changes cheap. In UE 5.8 a HISM keeps its instances in legacy mode: a change that is not in
place marks its cluster tree out of date, and the rebuild copies every instance on the game thread
(`BuildTreeAsync`; a change during the build throws it away and starts another), then recreates the HISM's scene proxy,
which registers every instance again with the distance-field and ray-tracing scenes on the render thread. Zero-scaled
pooled instances are among them. So a change costs all the HISM's instances, pooled ones included; see
[Batched HISM writes](#batched-hism-writes-world-modes-review-g42).

#### Release Operations

```cpp
// Release a specific scatter type's instances from one chunk back to pool
void ReleaseChunkScatterType(const FIntVector& ChunkCoord, int32 ScatterTypeID)
{
    // 1. Look up pool and chunk indices
    // 2. Zero-scale each instance where it stands, held for the HISM's next batch (written at once and parked at
    //    the HISM's origin, with the render state marked dirty, when voxel.Scatter.HISMBatchSeconds is 0)
    // 3. Move indices to FreeIndices
    // 4. Remove chunk from pool's ChunkInstanceIndices
    // 5. Update ChunkScatterTypes tracking; drop the pair's queued adds (G30)
}
```

#### Recycling in FlushPendingInstanceAdds

The per-frame budget-limited flush recycles from the free list before growing:

```
For each FPendingInstanceAdd entry:
  1. Get/create HISM and pool for ScatterTypeID
  2. Skip entries for chunks unloaded while pending
  3. Recycle min(needed, FreeIndices.Num()): each placement held for the HISM's next batch
  4. Grow for the remainder: new instances held for the batch, their indices the HISM's count plus those already held
     (updates pool.TotalAllocated)
  5. Record all indices in pool.ChunkInstanceIndices[ChunkCoord]
  (written at once, with the render state marked dirty, when voxel.Scatter.HISMBatchSeconds is 0)
```

Both recycled and newly added instances count toward the per-frame budget (`MaxInstanceAddsPerFrame`).

#### Per-Chunk-Type Updates (no global rebuilds)

All instance updates are scoped to a single (chunk, scatter type) pair — the renderer
**never** releases and re-streams a whole scatter type across the world (a whole-type rebuild
once did, and flashed the world's scatter whenever the player stopped moving).

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
`FPendingInstanceAdd` entries; `FlushPendingInstanceAdds` recycles from the free list first,
then grows the pool. A chunk that already has scatter takes the per-chunk-type path above —
each affected type is replaced in place, so only that one chunk's instances ever move:

```cpp
void UVoxelScatterRenderer::UpdateChunkInstances(const FIntVector& ChunkCoord, const FChunkScatterData& ScatterData)
{
    const bool bIsNewChunk = !ChunkScatterTypes.Contains(ChunkCoord);

    if (bIsNewChunk && ScatterData.bIsValid)
    {
        // Group transforms by type, queue as FPendingInstanceAdd entries
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
unloading and its reload was taken for a supplemental pass. Tests: `Scatter.Reload.*`.
`voxel.Scatter.Digest` logs a second line, `caches:` (`ComputeCacheResidency`), that checks this: chunks with spawn
points, with surface data, with spawn points but no surface data (0), spawn points against the renderer's instances
(`UVoxelScatterRenderer::GetChunkInstanceCount`; the chunks that differ are logged per type, `file` writes them to
`ScatterCaches_<world>_<time>.csv`), and spawn points that repeat another of their chunk.

### Distance Streaming Cleanup

When scatter types go out of range, the manager calls `ReleaseChunkScatterType()` directly instead of the previous stale-type tracking approach. Instances are zero-scaled immediately (CullDistance already hides them at that range) and recycled when the player returns and new transforms are generated. The release also drops the pair's adds still queued for the per-frame budget: a chunk submitted while the viewer was still near it (in the seconds after a teleport) can take a distance-limited type in its first pass and release it at the next check, behind the arrival's backlog. Until world modes review G30 those adds were flushed after the release into the pool for a type the chunk no longer tracked, which `RemoveChunkInstances` never released, so they stayed in the world and the chunk's reload drew the type a second time over them.

### Batched HISM writes (world modes review G42)

Each change to a HISM costs all its instances (the pool section above), and streaming changes the HISMs around the
viewer every frame: chunks loading at the window's front, unloading behind it, and distance-limited types (grass) spawned
and released at their `SpawnDistance`. On `Review_Planet_M` and `_L` that was most of the frame time over the small
planet's (G42): a HISM tree build and a proxy recreation nearly every frame, the
grass HISMs of 37,000 instances (half of them pooled), and the render thread re-registering them with the distance-field
scene (frames up to 51 ms). Writes are now batched per HISM:

- **Held writes.** `ReleaseChunkScatterType` and `FlushPendingInstanceAdds` update the pools and chunk indices at once
  (so `GetChunkInstanceCount`, the audits and the tests see no delay) and hold the HISM writes in `PendingHISMWrites`:
  an index's new transform (a release or a recycled placement; the last write wins) and the new instances, whose indices
  are the HISM's instance count plus those already held. `Tick` applies a HISM's writes at most once per
  `voxel.Scatter.HISMBatchSeconds` (0.1) and not while its cluster tree builds, unless they have waited
  `voxel.Scatter.HISMMaxHoldSeconds` (0.5): a change during a build would throw the build away.
- **In-place releases.** A release zero-scales the instance where it stands. The HISM takes an in-place change without
  rebuilding its tree, so releases (unloads, distance cleanup, edits) no longer rebuild anything.
- **No explicit render-state dirtying.** The HISM sends changed instances to its proxy itself: in-place changes at once,
  placements and new instances drawn as unbuilt until the tree build lands and recreates the proxy. The old
  `MarkRenderStateDirty` after every release and flush recreated the proxy a second time.
- **Compaction.** A HISM taking a batch whose pooled instances are at least `voxel.Scatter.HISMCompactMinPooled` (1,024)
  and outnumber its live ones is rewritten with its live instances only (`CompactHISM`: each chunk's instances, the held
  writes applied, renumbered; one `ClearInstances` + `AddInstances`).
- **Smaller regions than the renderer's.** `voxel.Scatter.RegionChunks` sizes the regions the HISMs are kept per: -1
  (default) = 4 chunks in every world mode (planets since G42, heightmap worlds since world modes review WM-MP5), 0 = the
  render regions (`voxel.Render.RegionChunks`, 16), N > 0 = N chunks.
- Queries see held writes: `FindNearestInstance` (harvest) reads each instance through them and also scans held
  placements and new instances; `GetTotalInstanceCount` counts held new instances. `FlushHISMWrites` applies every held
  write now.

The instances drawn are the same as with every write made at once (`voxel.Scatter.HISMBatchSeconds 0`, the behaviour
before G42, for A/B runs): `VoxelWorlds.Scatter.HISMBatch.{SameInstances,InPlaceRelease,Regions}`. The bench logs
`ScatBatchN` and `ScatCompactN` per sample, and the scatter section's steps (`ScatApplyMs`, `ScatLaunchMs`,
`ScatDistMs`, `ScatFlushMs`).

### Debug Stats

`GetDebugStats()` reports pool health:
```
ScatterRenderer: 5 HISM, 12400 instances (Active: 8200, Pooled: 4200, Util: 66%), 42 chunks, Pending: 0 adds
```

- **Active**: Instances with real transforms (visible within CullDistance)
- **Pooled**: Zero-scaled instances on free list (not drawn, but part of every tree build, proxy recreation and
  distance-field update of their HISM until compacted)
- **Util**: Active / TotalAllocated

`voxel.Scatter.Stats hism` adds the HISMs by size: per type its HISMs, instances, pooled instances and largest HISM, and
the twelve largest HISMs by region (`UVoxelScatterRenderer::GetHISMBreakdown`).

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
        FChunkScatterData* ScatterData = SpawnCache.Find(ChunkCoord);
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

### System and Editor Edits: the Chunks That Read Them

A system or editor edit (a POI's terraform, a dungeon stamp, an undo, a clear, a load) re-extracts scatter. Two kinds of
chunk need it (world modes review G33):

- **The edited chunk.** The chunk manager's edit handler calls `RegenerateChunkScatter` (the caches are dropped, the
  instances stay until the re-extraction replaces them per type) and marks the chunk dirty; its re-mesh hands its
  edit-merged voxels to scatter.
- **The chunks whose extraction reads its edited voxels**, from past their own faces (`FScatterExtractionContext`):
  - on a heightmap world, the chunk below: the voxel over its top layer is the edited chunk's bottom layer
    (`OutsideEdits`, keyed in the reader's frame at Z = ChunkSize, G26);
  - on a planet, the seven chunks at -X, -Y and -Z: a planet chunk's last cell on each axis reads the voxels past its +X,
    +Y and +Z faces (`OutsideEdits`, G29, G31). A face neighbour reads the layer facing it, an edge neighbour the column
    along the shared edge, the corner neighbour the corner voxel.

  `UVoxelScatterManager::GetEditReaders` gives them for the layers the edit reached: `UVoxelEditManager::GetBroadcastEditBounds`
  reports, during each `OnChunkEdited` broadcast, the bounds of the voxels the edit changed in that chunk. The handler
  collects them, and `UVoxelChunkManager::ProcessEditScatterReaders` re-extracts each once per tick from its voxels
  (`RegenerateChunkScatter`, then the same hand-off as a mesh completion), with no re-mesh: their own voxels did not
  change. A reader edited in the same tick is skipped (its own re-mesh re-extracts it, after the edits), and so is one
  without voxel data (its extraction reads the edit when it loads). `voxel.Scatter.EditReaders 0` restores the rule
  before G33 (the edited chunk's six face neighbours), for A/B.

Player edits are unchanged: they only clear scatter in a radius (above) and never re-extract.

`voxel.Scatter.EditAudit [maxLines]` checks the result: it re-runs every cached chunk's extraction now
(`UVoxelScatterManager::CompareWithFreshExtraction`: the chunk's edit-merged voxels and a context made now) and lists
the chunks whose cache differs, whether each reads an edited chunk, and what system edits did to scatter
(`UVoxelChunkManager::GetEditScatterStats`). `voxel.Edit.System` and `voxel.Edit.SystemLayers` make system edits for it
(a sphere, optionally on the nearest chunk edge or corner; a chunk's min-face layers). Tests: `Scatter.EditReaders.*`.

### Exclusion Volumes and Harvesting

A cleared volume lasts until its chunk unloads; an **exclusion volume** (`FScatterExclusionVolume`: an oriented box with
an `Id`) lasts until it is unregistered. Registering one clears the spawn points inside it at once through the
per-(chunk, type) replace path, filters in-flight results, and keeps chunks that stream in later bare inside it;
unregistering it re-runs placement for the chunks it overlapped, so the foliage regrows. Game code registers volumes
through the world subsystem `UVoxelScatterExclusionSubsystem`, which keeps the set and replays it to every scatter
manager that initializes later (a claim footprint kept bare, for instance); VoxelWorlds attaches no meaning to them.

`HarvestScatterNear` finds the nearest instance of a harvestable definition (`HarvestCategory`) within a radius and,
with `bRemove`, removes it for good by registering a small exclusion volume around its base
(`MakeHarvestExclusionVolume`; on a planet the box stands on local up). Scatter is local to each machine, so the server
decides and every machine that renders scatter removes it.

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
};
```

A world without a configuration, or one whose array is empty, places no scatter (the manager's built-in grass, rock
and tree definitions, and the cubic trio, went in code review CR1, D-CR18; `VoxelScatterTesting::SmoothDefinitions`
keeps the smooth three for the tests that ran on them).

## Performance Considerations

### Throttling

- `MaxScatterGenerationsPerFrame = 2`: Limits surface extraction per frame
- `MaxInstanceAddsPerFrame = 2000`: Budget for instance additions/recycling per frame — the single knob for streaming smoothness now that whole-type rebuilds are gone

### Memory

- Surface data cached per chunk (`SurfaceCache`)
- Scatter spawn points cached per chunk (`SpawnCache`)
- HISM components shared across chunks (one per scatter type and region)
- Instance pool per HISM (`InstancePools`) — tracks free list and per-chunk indices
- A pool grows to its peak; a HISM taking a batch whose pool outweighs its live instances is compacted (above). Pooled instances are not drawn, but each change to their HISM still processes them

### Distance-Based Loading

- Per-definition `SpawnDistance` allows different ranges per scatter type; a definition with `SpawnDistance` 0 spawns at
  the world configuration's `ScatterRadius`
- Trees can have larger spawn distance to prevent pop-in
- Grass can have smaller distance for performance
- Out-of-range cleanup releases instances to pool via `ReleaseChunkScatterType()` (in place: no tree rebuild)
- Returning to an area recycles from pool — faster than initial load since instances already exist in HISM

### The First Extraction and the Scatter Horizon (world modes review G79)

A chunk is handed to the scatter manager when it submits a mesh (`UVoxelChunkManager::HandOffChunkScatter`: its first
mesh and every LOD re-mesh), and the hand-off generates only the types whose spawn distance covers the chunk. The
**scatter horizon** (`GetScatterHorizon`) is the largest of those distances. While walking, every chunk arrives at the
window's front, which on most worlds lies beyond the horizon (`VoxelDemo`: a 40,000 uu view distance against trees at
25,000), so its first hand-off finds nothing in range. Until G79 that hand-off extracted nothing and cached nothing,
the distance pass (which reads only chunks with a surface cache) never saw the chunk, and its scatter waited for its
next mesh: the LOD refine near the viewer (8,200 uu on `VoxelDemo`), where every type landed at once, the trees inside
their fade band. Scatter looked as if it existed only on the finest LOD and popped in front of the player.

Now such a chunk is remembered (`ChunksAwaitingFirstExtraction`; an unload or a regeneration drops it) and the
distance pass hands it off again once it comes within the horizon, nearest first, `MaxDistanceSpawnChunksPerPass` a
pass, through the chunk manager's requester (`SetChunkHandOffRequester`: `HandOffChunkScatter` from the chunk's
edit-merged voxels, the far-compressed snapshot path included). Its first extraction then runs at the horizon with the
types in range there, and the distance pass streams the rest in at their own distances; types with `SpawnDistance` 0
stream at `ScatterRadius` like the others (and are released past it like the others, where before they stayed once
spawned). A hand-off that finds a type in range takes the chunk out of the set, so a chunk whose next mesh comes first
is handled as before. `voxel.Scatter.FirstExtractionAtHorizon` (1; 0 = the behaviour before), `voxel.Scatter.Stats`
reports the chunks awaiting, the deferred hand-offs and the horizon. Tests: `Scatter.FirstExtraction.Horizon`,
`.RuleBefore`, `Streaming.Plane.ScatterFirstExtractionAtHorizon`.

With the horizon in place the distances a definition needs are only the ones `ValidateScatterDistances` already
checks: `LODStartDistance < CullDistance` (a fade band) and `CullDistance` below the effective spawn distance (a
generated margin behind the visible edge, so streaming latency never shows at it).

## Debug Visualization

Enable via `bScatterDebugVisualization` in VoxelWorldConfiguration:

- Spawn points: Colored spheres per scatter type
- Surface normals: Blue lines
- Scatter radius: Yellow sphere around viewer

## Async Generation

Surface extraction and scatter placement run on the thread pool, keeping the game thread free.

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
- **Supplemental passes**: `CompletedScatterTypes` records which types a chunk has, so a later pass (the distance
  stream, the horizon hand-off) adds only the types it lacks

### Key Files
- `VoxelScatterManager.h`: `FAsyncScatterResult`, `CompletedScatterQueue`, `AsyncScatterInProgress`
- `VoxelScatterManager.cpp`: `LaunchAsyncScatterGeneration()`, `ProcessCompletedAsyncScatter()`

## Cubic Terrain Scatter

Cubic meshing mode uses specialized scatter behavior: block-face snapping, cross-billboard meshes for grass/flowers, and voxel tree injection for editable terrain trees.

### Block-Face Snap Placement

When `MeshingMode == Cubic`, scatter uses `ExtractSurfacePointsByBlockTop()` instead of the smooth-terrain voxel column scan:

- One surface point per exposed top block face
- Position at face center: `(X + 0.5) * VoxelSize, (Y + 0.5) * VoxelSize, (Z + 1) * VoxelSize`
- Normal always `FVector::UpVector`, SlopeAngle always 0
- No density interpolation — exact block-face positions
- `BlockFaceSnap` placement keeps an instance on the block face it was placed on: a definition's `PositionJitter` wider
  than half a face is scaled down to half a face (scaled, not clamped, so instances do not pile up on the face edges);
  a smaller jitter is unchanged. `bAutoCubicScatterDefaults` sets every definition to `BlockFaceSnap` in cubic worlds.

Routed automatically when `Configuration->MeshingMode == EMeshingMode::Cubic`. The extractor reads voxels, not the
mesh, so it is unaffected by cubic chunk meshes being interior-only (the faces on chunk planes are seam meshes).
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

### Scatter Edit Integration for Block Edits

When a block is broken (discrete edit), scatter on the block's top face must also be removed. The scatter clear radius is padded by `VoxelSize * 0.6` to account for the half-block offset between the edit center (block center) and the scatter position (top face center):

```cpp
// VoxelChunkManager OnChunkEdited handler:
const float ScatterClearRadius = EditRadius + Configuration->VoxelSize * 0.6f;
ScatterManager->ClearScatterInRadius(EditCenter, ScatterClearRadius);
```

## Voxel Tree System

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
   - `floor(TreeDensity)` trees, plus one with the fractional part's probability
   - Random X,Y within source chunk → the world mode's generated surface there (`GetSurfaceOnUpLine`) for Z
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

**Integration**: `FVoxelTreeInjector::InjectTrees` runs on the generation worker after CPU generation
(`UVoxelChunkManager::LaunchAsyncGeneration`) or after a GPU readback (`LaunchPostReadbackProcessing`), in a cubic
heightmap world (`MeshingMode == Cubic`, not a planet) with `TreeMode != HISM` and tree templates.

**Placement filtering**: `FVoxelSurfaceQuery::QuerySurfaceConditions()` and `ComputeSlopeDegrees()` give the surface
material, biome and slope by the generators' own pipeline, so trees respect biome boundaries without the full noise
generator.

**Edit integration**: Trees are part of VoxelData, so player edits work naturally — dig through trunks, remove leaves, etc. The edit layer preserves changes across chunk reload. A standing tree can be found and felled as a whole
(`UVoxelChunkManager::FindVoxelTreeAt` / `FellVoxelTree`: the stamp recomputed from its seed; the game module's
`vox.Chop` and `vox.Trees`).

### Tree Mode Filtering

`EVoxelTreeMode` controls how trees are rendered:

| Mode | VoxelData Injection | HISM Scatter | Use Case |
|------|--------------------:|-------------:|----------|
| VoxelData | Yes (all distances) | No | Editable trees, performance cost |
| HISM | No | Yes (all distances) | Visual-only, best performance |
| Both | Yes (near) | Yes (far) | Hybrid: edit nearby, visual far |

In `HandOffChunkVoxels()`, VoxelInjection scatter definitions are filtered based on TreeMode:
- **VoxelData**: Skip all VoxelInjection defs (trees already in terrain mesh)
- **HISM**: Include VoxelInjection defs (rendered as HISM instances)
- **Both**: Skip within `VoxelTreeMaxDistance`, include beyond

### Key Files

| File | Purpose |
|------|---------|
| `VoxelCore/Public/VoxelTreeTypes.h` | FVoxelTreeTemplate, EVoxelTreeMode, ETreeCanopyShape |
| `VoxelGeneration/Public/VoxelTreeInjector.h` | Tree injection API |
| `VoxelGeneration/Private/VoxelTreeInjector.cpp` | Cross-chunk deterministic injection + placement rules |
| `VoxelScatter/Public/VoxelBillboardMeshGenerator.h` | Billboard mesh generation API |
| `VoxelScatter/Private/VoxelBillboardMeshGenerator.cpp` | Runtime cross-billboard mesh + material |

## Not Built

- **Foliage LOD**: leverage HISM's built-in LOD system
- **Procedural variation**: per-instance custom data for shader variation
- **GPU scatter placement**: rule evaluation in a compute shader, transforms written straight to the HISMs
- **Tree physics**: unsupported leaf blocks fall when the trunk is destroyed

## See Also

- [UNDERGROUND_CLASSIFICATION.md](UNDERGROUND_CLASSIFICATION.md) - Underground flag propagation and scatter surface/underground filtering
- [BIOME_SYSTEM.md](BIOME_SYSTEM.md) - Biome-scatter integration
- [CAVE_SYSTEM.md](CAVE_SYSTEM.md) - Cave generation (density subtraction, cave types)
- [EDIT_LAYER.md](EDIT_LAYER.md) - Edit system integration
- [IMPLEMENTATION_PHASES.md](../Research/Archive/IMPLEMENTATION_PHASES.md) - Phase 7 history (archived)
