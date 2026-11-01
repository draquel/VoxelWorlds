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
- SphericalPlanet: tiles on the cube sphere (below); heightmap tiles keep their (chunk X, Y) keys and code paths
- For IslandBowl, areas beyond the island edge render based on their material (water/void)

## Planets (spherical Phase 4, SP-G3)

A planet's tiles are cells of a cube-sphere grid (`FVoxelCubeSphereGrid`, VoxelCore `VoxelCubeSphere.h`): N cells
along a face edge, N = ⌈(π/2)·R / chunk size⌉ (82 on `Review_Planet`: 6 × 82² tiles about a chunk across, a pixel about
75 uu, as on the plane), each addressed by its **atlas coordinate** (Face × N + tile U, tile V). Every `FIntPoint` key,
the tile store and its budgets, the queue and the save blobs therefore work unchanged; `IsPlanetMap`,
`GetPlanetTileGrid`, `GetPlanetCenter`, `GetPlanetRadius` and `PlanetDirectionToTilePixel` expose the grid.

- **Pixels** along their directions: pixel (px, py) → UV → direction (`DirectionAt`) → the analytic surface point
  (`GetTerrainAltitude`); material and biome as generation writes them (`FVoxelSurfaceQuery::QueryPlanetSurfaceConditions`),
  water where the sea sphere is above the terrain (shaded by depth), elevation shading by altitude over
  `GetAltitudeBounds`, the hillshade in the local frame (east, north, up) with the plane's light. The one-pixel apron
  comes from the neighbouring tile, across face edges through the direction.
- **Exploration** by surface key: `RequestTilesInRadius` gathers tiles by angle from the position's tile through the
  grid's neighbours (`GatherWithinAngle`); `OnChunkGenerated` marks the tiles the chunk's surface patch spans
  (`GetChunkTiles`). The two hemispheres no longer share tiles.
- **Edit overlays** along local up: `FVoxelMapEditOverlay::BuildPlanet` walks each pixel's line along local up through
  the edited chunks (a ray-box test, a coarse nearest-voxel walk, a trilinear refine), storing altitudes
  (`PlanetFormatVersion` 2; plane overlays stay version 1, byte for byte). Overlays of the other world kind are dropped
  on import.
- **`voxel.Map.Capture`** on a planet writes a north-up image of the tangent plane around the pawn.
- **The widgets** (VoxelCharacterPlugin): the minimap draws the tangent plane under the player (`FVCPlanetMapFrame`, north
  toward the +Z pole, heading-up, latitude and longitude shown) and the world map is a **globe** (decision D11,
  `FVCGlobeProjection`): an orthographic view around a direction, north up, opening on the player, dragged to turn it and
  zoomed with the wheel (32 tiles from the centre at zoom 1, so zoomed in it is a local map). Both sample the tile images
  through `FVCPlanetMapSampler` (a 16-texel control-point grid; cells across a face edge or the rim per texel). Markers
  carry a 3D position (`FVCMapMarker::WorldLocation`, gathered through the one `UVCMapMarkerRegistry::OnGatherMarkers`
  by centre and distance since code review CR2); POIs reach planets in
  Phase 5, so no provider binds them yet.
- **Saves**: the same blobs with atlas keys and version-2 overlays; `PlanetGenerationVersion` (3 since D13, 5
  since spherical Phase 6's P6.2) refuses older planet saves.

**At scale** (spherical Phase 6, WM-BP11; WORLD_MODES_REVIEW_PLAN.md §20.6). A 1,000 km planet's atlas is 32,725 tiles a
face edge, 6.4 billion tiles, so the map works on what is near the player and draws the rest coarsely:

- **The zoom range from R** (`UVCWorldMapWidget::GetPlanetZoomRange`, `GetZoomRange`, `SetZoom`): zoomed out to the
  whole globe with a tenth of the view to spare at any radius (`MinZoom` 0.25 showed it only up to R ≈ 5 × 10⁵: 0.0276 on
  a 50 km planet, 0.00138 on a 1,000 km one), zoomed in to `MaxZoom` as before.
- **The globe albedo** (`FVoxelMapGlobeAlbedo`, `UVoxelMapSubsystem::GetGlobeAlbedo`): a cube-sphere grid of at most
  `GlobeAlbedoCellsPerEdge` (256) cells a face edge, never more than the tile grid, each the colour of the surface at
  its centre as a tile pixel draws it (the sea's blue by depth; land its surface material shaded by elevation and a
  hillshade from the neighbouring cells), sampled from the far field's analytic surface (`FVoxelFarFieldSurface`) on a
  worker thread when first asked for (`OnMapTileReady` fires with (-1, -1) when it lands). Zoomed out until a texel spans
  more than two tiles (`UVCWorldMapWidget::GlobeAlbedoTexelsPerTile`) the world map draws it (`FVCPlanetMapSampler::
  bUseGlobeAlbedo`) instead of one tile pixel per texel, asking for no tile image; its fog of war is the albedo cells
  holding an explored tile (`IsGlobeCellExplored`, refreshed when the exploration changes, at most once a second past
  100,000 explored tiles).
- **`voxel.Map.ExploreAll`** counts in `int64` (6 N² overflowed `int32` from about 50 million tiles) and explores at most
  `voxel.Map.ExploreAllMaxTiles` (1,000,000) tiles, nearest the pawn first (`GatherWithinAngle`'s flood fill); it queues
  the images of the nearest `PlanetExploreAllImageTiles` (65,536), the rest drawn when the map shows them. The small
  planet (40,000 tiles) is explored whole as before.
- **Markers on a zoomed-out globe** (VoxelWorldPOI, `UPOIMapMarkerSubsystem::GetPlanetMarkersNear`): the globe asks for
  the markers of everything it shows, and on a big planet that was every POI cell of the planet (L's 36 million: the map
  held the game thread for about 1 min 45 s, G50). The far markers now come from at most `PlanetMarkerMaxCells` (20,000)
  cells around the view's centre, and the last cap's POIs are reused by the half-second refresh; the map draws at most
  `MaxMarkers` (64) anyway. The small planet's few hundred cells are always whole.
- **Redraws at the albedo's zoom** (`UVCWorldMapWidget`): the tile images do not show there, so a tile's arrival redraws
  the globe at most every 2 s (for the exploration it carries), and only the albedo's own arrival at once; a redraw per
  arrival held the open map at 6.5 FPS after an `ExploreAll` queued 65,536 images (G51).
- `vox.Map [zoom|min|0]` (the game module, QA) opens the world map at a zoom.

Tests: `VoxelWorlds.Map.Planet.TileKeys`, `.TileMatchesGeneration` (1,000 pixels at five sites, each the generated
surface voxel's material or the sea), `.Exploration`, `.EditOverlay`, `.ZoomAtScale` (the albedo against the analytic
surface cell by cell, its fog of war, `ExploreAll` capped on a planet of 5.8 million tiles); `VoxelCharacter.Planet.MinimapFrame`,
`.WorldMapProjection`, `.MapSampler`, `.MapZoomAtScale` (the zoom range on the 2.5, 50 and 1,000 km planets; the
globe zoomed out drawn from the albedo).

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
