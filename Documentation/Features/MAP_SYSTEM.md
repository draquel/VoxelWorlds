# Map System

**Module**: VoxelMap
**Dependencies**: VoxelCore, VoxelGeneration, VoxelStreaming

## Overview

The Map System generates 2D top-down map tile data from voxel terrain. Each tile corresponds to one chunk's XY footprint and stores a color image of the terrain surface. Tile generation is fully async and uses deterministic terrain height queries — no loaded chunk data is required.

The system lives entirely within the VoxelWorlds plugin and has zero knowledge of players, characters, or UI. External consumers (such as minimap or world map widgets in VoxelCharacterPlugin) query the subsystem for tile data and drive exploration via `RequestTilesInRadius()`.

Tile images live under a fixed memory budget (world modes review WM-E2, finding KI15; see [Tile Images and Memory](#tile-images-and-memory)): every drawn tile keeps a small overview, and its full-resolution image only while recently used. Exploration is kept whole and saved; a tile whose images were dropped is drawn again when something looks at it.

## Architecture

```
UVoxelMapSubsystem (UWorldSubsystem)
├── Tile Store: FVoxelMapTileStore (detail + overview images, LRU under two budgets)
├── Exploration Mask: TSet<uint64> (never evicted; saved)
├── Draw Queue: FVoxelMapTileQueue (+ DetailWanted: tiles whose drawing keeps its detail)
│
├── Event-Driven Generation ──→ UVoxelChunkManager::OnChunkGenerated
│   (auto-generates tiles as chunks stream in)
│
├── Predictive Generation ──→ RequestTilesInRadius()
│   (generates ahead of chunk streaming, driven by external callers)
│
└── Background Thread ──→ GenerateTileAsync()
    ├── IVoxelWorldMode::GetTerrainHeightAt()  (height sampling)
    ├── FVoxelCPUNoiseGenerator::FBM3D()       (biome noise)
    ├── FBiomeDefinition::GetMaterialAtDepth()  (biome material)
    └── FVoxelMaterialRegistry::GetMaterialColor() (material → color)
```

## Key Types

### FVoxelMapTile

Defined in `VoxelMapTypes.h`. Represents one chunk's 2D map image.

```cpp
USTRUCT()
struct FVoxelMapTile
{
    FIntPoint TileCoord;       // Chunk XY coordinate
    TArray<FColor> PixelData;  // Resolution x Resolution BGRA pixels
    int32 Resolution;          // Pixels per edge: ChunkSize (64 on the demo), or ChunkSize / 4 for an overview
    uint8 Version;             // Format version for future save/load
    bool bIsReady;             // Runtime flag: true when fully generated
};
```

Each tile maps 1:1 to a chunk's XY footprint. At full resolution (`Resolution` = ChunkSize) each pixel represents one voxel column: on the demo, 64 voxels of 75 uu, so a tile covers 4,800 uu. A tile's overview has a quarter of the resolution a side (16 × 16 on the demo); `GetTile` returns whichever the tile has, and consumers scale by its `Resolution`.

All data fields use `UPROPERTY()` for future `FArchive` serialization. `bIsReady` is intentionally not serialized as it is runtime-only state.

### UVoxelMapSubsystem

World subsystem that manages tile generation, caching, and exploration tracking.

**Public API:**

| Method | Description |
|--------|-------------|
| `GetTile(FIntPoint)` | The tile's detail image, or its overview when the detail was dropped (its `Resolution` says which); nullptr if it has none. Counts as a use (LRU) |
| `HasTile(FIntPoint)` / `HasTileDetail(FIntPoint)` | Whether the tile has any image / its full-resolution one |
| `RequestTileImage(FIntPoint, bool bDetail)` | For an explored tile: draw it again if it has no image, or (bDetail) only its overview. The world map calls it for the tiles in view (BlueprintCallable) |
| `GetTileOverviewResolution()` | Pixels a side of an overview (ChunkSize / 4); a view showing more texels per tile wants detail |
| `GetTileStore()` | The images and their budgets (diagnostics, tests); replaces `GetTileCache()` (removed with WM-E2) |
| `ExploreAllTiles(FVector, float)` | Debug (`voxel.Map.ExploreAll`): explore every tile of a bounded world, as a fully explored save restores it |
| `RequestTilesInRadius(FVector, float)` | Mark tiles as explored and queue generation |
| `IsTileExplored(FIntPoint)` | Check if a tile is in the explored set |
| `GetExploredTiles()` | Get all explored tile coordinates |
| `WorldToTileCoord(FVector)` | Convert world position to tile coordinate |
| `TileCoordToWorld(FIntPoint)` | Convert tile coordinate to world origin |
| `GetTileWorldSize()` | World size of one tile edge (ChunkSize * VoxelSize) |
| `GetTileResolution()` | Pixels per tile edge (matches ChunkSize) |

**Events:**

| Delegate | Description |
|----------|-------------|
| `OnMapTileReady(FIntPoint)` | Fired on game thread when a tile finishes generating |

## Tile Generation Pipeline

### Dual-Strategy Generation

Tiles are generated through two complementary paths:

**1. Event-driven (chunk load)**

The subsystem binds to `UVoxelChunkManager::OnChunkGenerated` during initialization. When a chunk finishes generating, the corresponding tile is automatically queued for map tile generation. This provides baseline coverage as the player explores.

**2. Predictive (look-ahead)**

`RequestTilesInRadius()` generates tiles for areas visible on a minimap or world map, even if those chunks haven't loaded yet. Since `GetTerrainHeightAt()` is deterministic and doesn't need loaded chunks, predictive generation works without any chunk data. This is critical because the minimap view radius typically exceeds the chunk streaming radius.

### Async Generation

All tile generation runs on background threads via `AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask)`. Concurrent tasks are capped at 4 (`MaxConcurrentTileGenTasks`) to avoid thread pool saturation.

When a task completes, it marshals results back to the game thread via `AsyncTask(ENamedThreads::GameThread)` and checks for more pending tiles to process, maintaining a steady pipeline.

**Thread safety:** All terrain sampling APIs (`GetTerrainHeightAt`, `FBM3D`, `GetMaterialColor`) are stateless and safe to call from any thread. Biome definitions and noise parameters are captured by value before the async lambda — UObject pointers are never accessed on background threads. `TileCache` writes are protected by `FCriticalSection` and happen exclusively on the game thread.

### Color Generation

Each pixel's color is determined through a multi-step process:

**1. Height sampling**
```
Height = WorldMode->GetTerrainHeightAt(WorldX, WorldY, NoiseParams)
```

**2. Material selection (biome-aware)**

When biomes are enabled, the system replicates the same noise sampling used by `FVoxelCPUNoiseGenerator`:
- Sample temperature and moisture at the pixel's world position using `FBM3D`
- Select the dominant biome using `FBiomeDefinition::Contains()` with closest-center tiebreaking
- Get the biome's surface material via `GetMaterialAtDepth(0.0)`
- Apply height material rules (e.g., snow above a certain elevation)

When biomes are disabled, falls back to `WorldMode->GetMaterialAtDepth()`.

**3. Water rendering**

When water is enabled and the terrain height is below the water level, the pixel renders as water with a depth gradient:
- Shallow water: light blue `(20, 80, 180)`
- Deep water: dark blue, scaled by `DepthFactor = Clamp(1.0 - Depth/3000, 0.3, 1.0)`

**4. Land elevation shading**

Above-water terrain gets topographic shading based on elevation above the water level (or 0 if water is disabled):
- Low elevation: 45% brightness
- High elevation (4000+ units above water): 100% brightness
- Formula: `Brightness = Lerp(0.45, 1.0, Clamp(Elevation / 4000, 0, 1))`

This creates a natural relief effect matching the water depth gradient.

### Tile Images and Memory

A full-resolution tile on the demo is 64 × 64 pixels, 16 KB. Until WM-E2 every drawn tile kept its image for the session
(KI15): a fully explored island L (10 km across, 34,512 tiles) held 539 MB of images, a 20 km island would hold about
2.2 GB. `FVoxelMapTileStore` (VoxelMap, world-free and unit-tested) now keeps two images per drawn tile and two budgets:

| Image | Size (demo) | Kept | Budget (cvar, MB; 0 = unlimited) |
|-------|-------------|------|----------------------------------|
| Detail | 64 × 64, 16 KB | Where something asked for it: the minimap's area (`RequestTilesInRadius`), streamed chunks (`OnChunkGenerated`), a zoomed-in world map (`RequestTileImage(.., true)`), and any tile that already had it | `voxel.Map.DetailBudgetMB` 32 (2,048 tiles: the minimap's area and a world map zoomed in twice) |
| Overview | 16 × 16, 1 KB, the detail box-filtered | Every drawn tile | `voxel.Map.OverviewBudgetMB` 48 (49,152 tiles: an island about 12 km across, fully explored) |

Over a budget the least recently used images go first (a read through `GetTile` or a store counts as a use), down to 90%
of the budget so trimming runs rarely: a detail image is dropped and its tile keeps the overview; past the overview budget,
tiles without detail lose the overview too and stay explored. Tiles restored from a save are stored as overviews only
(the minimap asks for detail where the player is). A detail budget of 0 keeps every detail image, as before WM-E2.

Why least recently used rather than distance: the subsystem knows no player, and what the minimap and the world map show
is by definition what was read last; distance from the player would evict what the world map is looking at when it pans
away. Why a coarser image rather than dropping tiles outright: the world map at its default zoom shows 16 texels per tile,
exactly an overview, so a fully explored island stays fully drawn without regeneration; only zooming in or the minimap
asks for detail, a few hundred to two thousand tiles.

Measured on a fully explored island L in one build (`voxel.Map.ExploreAll` / an imported save, WORLD_MODES_REVIEW_PLAN.md
§14.10): 37.1 MB of images (3.4 MB detail for the 216 tiles around the spawn, 33.7 MB of overviews) against 573 MB with
both budgets unlimited (539 MB of detail images, the pre-WM-E2 cost, plus the overviews). The world map at zoom 1 draws
the whole view from overviews.

Drawing: a 64 × 64 tile takes about 8 ms of background work (four run at a time, about 480 tiles a second while the game
runs); all generation is async, with no game-thread cost beyond storing the result.

## Configuration

The subsystem reads configuration from `UVoxelWorldConfiguration` (via `UVoxelChunkManager`):

| Setting | Source | Usage |
|---------|--------|-------|
| `ChunkSize` | VoxelWorldConfiguration | Tile resolution and coordinate mapping |
| `VoxelSize` | VoxelWorldConfiguration | World-space scaling |
| `WorldOrigin` | VoxelWorldConfiguration | Coordinate origin offset |
| `NoiseParams` | VoxelWorldConfiguration | Terrain height noise parameters |
| `bEnableBiomes` | VoxelWorldConfiguration | Whether to use biome-aware coloring |
| `BiomeConfiguration` | VoxelWorldConfiguration | Biome definitions, blend width, height rules |
| `bEnableWaterLevel` | VoxelWorldConfiguration | Whether to render water |
| `WaterLevel` | VoxelWorldConfiguration | Height threshold for water rendering |

All configuration is cached on first resolve (`ResolveChunkManager()`) and treated as read-only thereafter.

## Exploration System

The `ExploredTiles` set tracks which tile coordinates the player has visited or observed. Tiles are marked explored when:

1. `RequestTilesInRadius()` is called (minimap/world map visibility)
2. `OnChunkGenerated` fires (chunk streaming proximity)
3. A save restores them (`ImportExploredTiles`), or `voxel.Map.ExploreAll` explores the whole world (debug)

Only tiles that overlap the world mode's bounds are explored (`IsTileInsideWorld`, world modes review WM-I2): on an
island, exploration stops at `R + FalloffWidth`, and the ring of floor chunks the island streams past its edge is not
mapped. Explored tiles restored from a save that lie outside (explored before 2026-10-15) are dropped when the map
resolves its world. `GetWorldBounds()` returns the bounds the map resolved; `UVCWorldMapWidget` clamps its pan to their
box. `voxel.Map.Stats` reports how many explored tiles lie outside the world (0 once this applies).

The world map widget uses exploration state for fog of war — unexplored tiles render as dark areas.

## Key Packing

`FIntPoint` coordinates are packed into `uint64` keys for `TMap`/`TSet` storage:

```cpp
Key = (uint64(uint32(X)) << 32) | uint64(uint32(Y))
```

This avoids `GetTypeHash` issues with `FIntPoint` in some container configurations.

## World Mode Compatibility

The subsystem is world-mode-agnostic by design:
- Uses only `IVoxelWorldMode*` interface — never casts to a specific mode
- `GetTerrainHeightAt()` is pure virtual, implemented by all modes (InfinitePlane, IslandBowl, SphericalPlanet)
- Biome sampling uses the same noise functions regardless of world mode
- **Not** SphericalPlanet: the tiles are a top-down XY projection, so a planet's two hemispheres land on the same
  tiles, and `GetTerrainHeightAt` returns a radius there, not a height. A planet map needs tiles keyed by a
  cube-sphere surface parameterisation (SPHERICAL_PLANET_REQUIREMENTS.md SP-A2, SP-G3)
- For IslandBowl, areas beyond the island edge render based on their material (water/void)

## File Listing

| File | Description |
|------|-------------|
| `Source/VoxelMap/VoxelMap.Build.cs` | Module build configuration |
| `Source/VoxelMap/Public/VoxelMap.h` | Module API header |
| `Source/VoxelMap/Private/VoxelMap.cpp` | Module startup/shutdown |
| `Source/VoxelMap/Public/VoxelMapTypes.h` | FVoxelMapTile struct |
| `Source/VoxelMap/Public/VoxelMapTileStore.h` | FVoxelMapTileStore: detail and overview images under two budgets (WM-E2) |
| `Source/VoxelMap/Public/VoxelMapTileQueue.h` | FVoxelMapTileQueue: which tiles wait for a drawing, per-tile generations (R22) |
| `Source/VoxelMap/Public/VoxelMapEditOverlay.h` | FVoxelMapEditOverlay: edited columns drawn over the analytic terrain (R22) |
| `Source/VoxelMap/Public/VoxelMapSubsystem.h` | UVoxelMapSubsystem declaration |
| `Source/VoxelMap/Private/VoxelMapSubsystem.cpp` | Subsystem implementation |

## See Also

- [BIOME_SYSTEM.md](BIOME_SYSTEM.md) — Biome definitions and blend logic used by tile coloring
- [MATERIAL_REGISTRY.md](MATERIAL_REGISTRY.md) — Material ID to color mapping
- [CHUNK_STREAMING.md](CHUNK_STREAMING.md) — UVoxelChunkManager and OnChunkGenerated delegate
- [NOISE_GENERATION.md](NOISE_GENERATION.md) — FVoxelCPUNoiseGenerator and FBM3D noise sampling
