# Map System

> State as of 2026-11-09 (MAP_PYRAMID_PLAN.md P1, the tile pyramid). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelMap
**Dependencies**: VoxelCore, VoxelGeneration, VoxelStreaming

## Overview

The Map System generates 2D top-down map tile data from voxel terrain. A level-0 tile corresponds to one chunk's XY footprint (a cell of the cube-sphere grid on a planet) and stores a color image of the terrain surface; the tiles form a pyramid, a level-L tile covering 2^L x 2^L level-0 tiles at the same 64 pixels ([The Tile Pyramid](#the-tile-pyramid-map_pyramid_planmd-p1)). Tile generation is fully async and uses deterministic terrain height queries — no loaded chunk data is required.

The system lives entirely within the VoxelWorlds plugin and has zero knowledge of players, characters, or UI. External consumers (such as minimap or world map widgets in VoxelCharacterPlugin) drive exploration via `RequestTilesInRadius()` and ask for the images they show via `RequestView()`.

Images are drawn for the views, not for exploration: each view asks for the tiles it shows at the level matching its texels, about 300 for the world map at any zoom, and one least-recently-used store of every level keeps them under one budget. Exploration is kept whole and saved, and marking a tile explored draws nothing. Player and system edits show on the map through per-tile edit overlays ([Edit Overlays](#edit-overlays-r22)), read under the pixels of every level.

## Architecture

```
UVoxelMapSubsystem (UWorldSubsystem)
├── Tile Store: FVoxelMapTileStore (every pyramid level, one LRU under one budget)
├── Exploration: TSet<uint64> of level-0 keys (never evicted; saved)
├── Draw Queue: FVoxelMapTileQueue (by requester, then each view's order: minimap, world map, edits)
│
├── Exploration ──→ UVoxelChunkManager::OnChunkGenerated, RequestTilesInRadius(), ExploreAllTiles(), a save
│   (marks tiles explored; draws nothing)
│
├── View Requests ──→ RequestView(requester, tiles nearest first)
│   (the minimap's and the world map's tiles at their levels; each replaces that view's last)
│
├── Edit Overlays: FVoxelMapEditOverlay per edited tile (R22; saved)
│
└── Background Thread ──→ GenerateTileAsync()
    ├── FVoxelMapTileFrame::Altitude()                 (the generated surface, conditioning zones applied)
    ├── FVoxelSurfaceQuery::QueryMapSurfaceMaterial()  (the surface material as generation writes it)
    ├── the edit overlays                              (edited columns' height, material, water)
    └── the map palette                                (the atlas's baked albedo per material, else the registry colour)
```

## Key Types

### FVoxelMapTile

Defined in `VoxelMapTypes.h`. Represents one tile's 2D map image at a level of the pyramid (`FVoxelMapTileId`: the coordinate at its level and the level).

```cpp
USTRUCT()
struct FVoxelMapTile
{
    FIntPoint TileCoord;       // The coordinate at its level (chunk X, Y at level 0; a planet's atlas coordinate)
    int32 Level;               // The pyramid level (0 = a chunk across)
    TArray<FColor> PixelData;  // Resolution x Resolution BGRA pixels
    int32 Resolution;          // Pixels per edge: ChunkSize (64 on the demo) at every level
    uint8 Version;             // Format version for future save/load
    bool bIsReady;             // Runtime flag: true when fully generated
};
```

A level-0 tile maps 1:1 to a chunk's XY footprint, each pixel one voxel column: on the demo, 64 voxels of 75 uu, so a tile covers 4,800 uu. A level-L tile covers 2^L × 2^L of them at the same 64 pixels, each pixel a point sample 2^L voxel columns apart.

All data fields use `UPROPERTY()` for future `FArchive` serialization. `bIsReady` is intentionally not serialized as it is runtime-only state.

### UVoxelMapSubsystem

World subsystem that manages tile generation, caching, and exploration tracking.

**Public API:**

| Method | Description |
|--------|-------------|
| `GetTile(FIntPoint)` | A level-0 tile's image; nullptr if it has none. Counts as a use (LRU) |
| `FindTile(FVoxelMapTileId, bTouch)` | A tile's image at any level (possibly stale while its redraw is queued); nullptr if it has none |
| `HasTile(FIntPoint)` | Whether a level-0 tile has an image |
| `RequestView(EVoxelMapRequester, tiles)` | A view's request: the tiles it shows at its level, nearest its centre first. Queues those without an image (or with a stale one) and replaces the requester's last request (an empty one clears it) |
| `LevelForTexel(double)` | The pyramid level whose pixels match a texel of that size: the coarsest whose pixels are at most the texel apart (BlueprintCallable) |
| `GetTileStore()` / `GetTileQueue()` | The images and their budget; the draw queue (diagnostics, tests) |
| `ExploreAllTiles(FVector, float)` | Debug (`voxel.Map.ExploreAll`): explore every tile of a bounded world (an unbounded one: within Radius of Center), as a fully explored save restores it, at most `voxel.Map.ExploreAllMaxTiles`; queues no image |
| `RequestTilesInRadius(FVector, float)` | Mark tiles as explored (the minimap's area); draws nothing |
| `GetExplorationRevision()` | A count bumped when a tile is newly explored (a view redraws its fog of war when it moves on) |
| `IsTileExplored(FIntPoint)` | Check if a tile is in the explored set |
| `GetExploredTiles()` | Get all explored tile coordinates |
| `WorldToTileCoord(FVector)` | Convert world position to tile coordinate |
| `TileCoordToWorld(FIntPoint)` | Convert tile coordinate to world origin |
| `GetTileWorldSize()` | World size of one tile edge (ChunkSize * VoxelSize) |
| `GetTileResolution()` | Pixels per tile edge (matches ChunkSize), at every level |
| `GetTileFrame()` | The tile frame: the grid, the pyramid's arithmetic (`LevelTileOf`, `ParentOf`, `FirstLevel0Tile`, `GetMaxLevel`, `TilesPerFaceEdgeAt`) |

**Events:**

| Delegate | Description |
|----------|-------------|
| `OnMapTileReady(FVoxelMapTileId)` | Fired on game thread when a tile's image (at any level) is stored |
| `OnGlobeAlbedoReady` | Fired on game thread when a planet's globe albedo is built |

## Tile Generation Pipeline

### Exploration and View Requests

Exploration and drawing are apart (MAP_PYRAMID_PLAN.md P1). Tiles are **explored** as chunks generate (`OnChunkGenerated`: the tiles the chunk spans), as the minimap asks (`RequestTilesInRadius`), by `voxel.Map.ExploreAll` and from a save; that draws nothing. Images are **drawn** when a view asks for them (`RequestView`): the minimap and the world map each ask, at every fill, for the tiles they show at their level, nearest their centre first. The generated surface is a function of the world's configuration and needs no loaded chunk, so any tile at any level can be drawn ahead of streaming.

### Async Generation

All tile generation runs on background threads via `AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask)`. Concurrent tasks are capped at 4 (`MaxConcurrentTileGenTasks`) to avoid thread pool saturation.

When a task completes, it marshals results back to the game thread via `AsyncTask(ENamedThreads::GameThread)` and checks for more pending tiles to process, maintaining a steady pipeline.

**Thread safety:** The task samples a standalone world mode the subsystem owns (a `TSharedPtr` the task co-owns, so a PIE stop cannot free it under the task), the tile frame (`FVoxelMapTileFrame`, a value type), a `FVoxelBiomeSnapshot` and the palette, all captured by value; UObject pointers are never accessed on background threads. `TileStore` writes happen on the game thread when a drawing completes, under `TileMutex`.

### Color Generation

Each pixel's colour is determined in four steps (`VoxelMapTileDrawing.cpp`; one drawer for both kinds of world, the
frame answering what differs since code review CR2):

1. **Height.** The generated surface at the pixel (`FVoxelMapTileFrame::Altitude`): on a heightmap world the world
   mode's surface on the up line through (X, Y) (`GetSurfaceOnUpLine`, continentalness and the island falloff included)
   with the terrain conditioning zones over the tile layered on (`FVoxelTerrainConditioning::ApplyToHeight`: POI pads,
   campsites); on a planet the conditioned altitude along the pixel's direction. A one-pixel apron from the neighbouring
   tiles feeds the hillshade, so shading is continuous across tile seams.
2. **Material.** `FVoxelSurfaceQuery::QueryMapSurfaceMaterial`: the material generation writes at the surface voxel,
   by the same pipeline as generation, tree placement and PCG (biome selection and blending, height rules, the
   underwater rules). The colour is the material atlas's baked average albedo (`BuildMapPalette`), else
   `FVoxelMaterialRegistry::GetMaterialColor`.
3. **Water.** Where the sea is above the terrain the pixel is blue `(20, 80, 180)` scaled by
   `Lerp(1.0, 0.3, Depth / DepthSpan)`, the depth span taken from the world's own terrain floor so a shallow sea still
   shows a gradient.
4. **Land shading.** The palette colour times an elevation tint, `Lerp(0.75, 1.0)` across the world's height range above
   the sea, times a hillshade from the neighbouring pixels (`FVoxelMapTileFrame::ShadeAt`, a north-west key light).

An edited column (below) draws with its overlay's height, material and water instead.

### Edit Overlays (R22)

The analytic map knows nothing of voxel edits, so each tile with edits that changed its **top surface** keeps an
`FVoxelMapEditOverlay`: per edited column the edited surface height, its material and whether water covers it (a
water-flagged voxel anywhere in the air above it), stored sparsely. The subsystem binds `UVoxelEditManager::OnChunkEdited`,
coalesces the edited tiles on a timer (`voxel.Map.EditRefreshSeconds`, 0.5 s; water propagation edits every frame), and
rebuilds at most `voxel.Map.EditRefreshMaxTiles` (8) a refresh, those whose edited chunks are loaded, from the chunks'
edit-merged snapshots (shared with the seam pipeline; at most one gather a frame on the game thread, the build on the tile
task). A column is masked only where an edited voxel lies at or above the surface and the surface differs from the
analytic one (height by more than half a voxel, material, or water), so dungeon interiors and other buried edits never
show. A column dug into a chunk that is not loaded keeps its previous value until the chunk loads. Neighbouring tiles
redraw when an overlay's border changes, so the hillshade stays continuous. On a planet the overlay is built along each
pixel's local up (below).

Overlays are saved with the world (`ExportEditOverlays` / `ImportEditOverlays`: magic `VMEO`, zlib and base64 in the
world document), so edits show on the map before their area loads again; a loaded save's tiles whose edited chunks are
loaded are rebuilt from them, a tile with no edits left is cleared. Only the server saves; a client's map rebuilds its
overlays as chunks load. `voxel.Map.Redraw [1]` redraws every explored tile (1 rebuilds the overlays too),
`voxel.Map.Capture [radius] [name] [highlight]` writes the cached tiles around the pawn to a PNG (overlay columns tinted
with highlight 1). Tests: `VoxelWorlds.Map.EditOverlay.*`, `VoxelWorlds.Map.Tiles.*`.

### The Tile Pyramid (MAP_PYRAMID_PLAN.md P1)

The map's cost grew with what was explored, not with what was shown (the map scaling plan, G163, G166-G170): every
explored tile was a request when a view touched it and an image in the queue, so a fully explored 50 km planet (16 million
tiles) asked for half a million images at one zoom. The tile pyramid (D84) bounds the work by the view.

- **Tiles with a level** (`FVoxelMapTileId`, `FVoxelMapTileFrame`'s pyramid functions). Level 0 is a chunk column (a
  cell of the cube-sphere grid on a planet); a level-L tile covers 2^L × 2^L level-0 tiles at the same 64 pixels, so its
  pixels are 2^L voxel columns apart. On a plane the tile at level L of level-0 tile (X, Y) is (X >> L, Y >> L), unbounded
  (`MaxPlaneLevel` 16); on a planet the pyramid is a quadtree per cube face, N_L = ceil(N / 2^L) tiles a face edge, the
  atlas coordinate (Face × N_L + U >> L, V >> L), the face's last tile partly empty, up to the level of one tile a face
  (ceil(log2 N): 11 on the 50 km planet).
- **One drawer at every level.** `GenerateTileAsync` samples `FVoxelMapTileFrame::SampleAt(Id, ...)`: a plane's pixel
  corners and a planet's pixel centres 2^L level-0 pixels apart, with the one-pixel apron, the conditioning zones over the
  tile's area (`GatherZones(Id)`), the hillshade at the level's spacing (`ShadeAt(.., Level)`), and the edit overlays of
  the level-0 tiles under its pixels (`SourcePixel`: a plane's corner pixel, a planet's pixel under the sample's
  direction; gathered from the footprint or from the overlays, whichever is fewer). A drawing costs the same at every
  level. Overlay rebuilds stay level 0's; when one changes an overlay, the coarser tiles over it go stale.
- **One store** (`FVoxelMapTileStore`, world-free and unit-tested): every level's images in one least-recently-used store
  under `voxel.Map.TileBudgetMB` (32 MB, 2,048 tiles of 64 × 64), trimmed to 90% when over. It replaces world modes
  review WM-E2's detail and overview budgets (two images a tile) and their 16 × 16 overview. A tile invalidated by an edit
  or a conditioning change keeps its image, marked stale, until its redraw lands.
- **Requests from the view** (`RequestView`, `FVoxelMapTileQueue`). A view asks for the tiles it shows at the level
  matching its texels (`LevelForTexel`: the coarsest whose pixels are at most a texel apart), nearest its centre first,
  and that replaces its last request: what left the view stops waiting. The queue serves the minimap, then the world map,
  then the edit refreshes, each in its own order (the `TSet` order it replaces served none). It holds at most the views'
  tiles, whatever is explored: about 300 for the 1024² world map at any zoom, about 36 for the minimap (level 1 at its
  160 uu texels).
- **The coarser level meanwhile.** While a view's tiles draw, the samplers show the nearest coarser level stored over each
  texel (VoxelCharacterPlugin's `FVCPlaneMapSampler`, `FVCPlanetMapSampler`), so a zoom or a pan shows the old image
  scaled up, then sharpens.

Exploration is still the key set (`ExploredTiles`) and `ExploreAll` keeps its cap: the explored mask that replaces both is
P2. A load and `ExploreAll` queue nothing, since only the views ask. Tests: `VoxelWorlds.Map.Pyramid.Levels` (the
arithmetic on a plane and on a planet of 37 tiles a face edge), `.ViewRequests` (an island through the subsystem),
`VoxelWorlds.Map.Tiles.ViewRequestsReplace` (the queue), `VoxelWorlds.Map.TileStore.Budget`, and
`VoxelWorlds.Map.Planet.EditOverlay` (a dig under a level-1 tile in view redraws it). The results against the map
scaling plan's measurements are in `Documentation/MAP_PYRAMID_PLAN.md` §6.

Drawing: a 64 × 64 tile takes about 7-13 ms of background work (four run at a time, 400-480 tiles a second while the game
runs); all generation is async, with no game-thread cost beyond storing the result. `voxel.Map.Stats [reset]` reports
the queue (waiting, drawing, the most waiting since the reset, by requester), the drawing since its last reset: tiles
drawn, their rate and the worker ms a tile, the drawings that replaced an image the store held, the images the views
asked for (`RequestView`), and the globe's fog-of-war rebuilds with their game-thread ms; then the images by level. Insights shows `ExploreAll`, the view requests, the drawing and
its completion, the store's trimming, the fog-of-war rebuild and the explored tiles' save and load as `VoxelMap_*` scopes
(the map scaling plan's M0, G163).

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

The world map widget uses exploration state for fog of war — unexplored tiles render as dark areas. Exploring draws
no image.

`voxel.Map.ExploreAll` explores at most `voxel.Map.ExploreAllMaxTiles` (1,000,000) tiles, nearest the pawn first, in
every world mode (a plane's tiles are unbounded; world modes review MP1, test `VoxelWorlds.Map.ExploreAllCap.Plane`).

## The Widgets (VoxelCharacterPlugin)

The minimap and the world map live in VoxelCharacterPlugin (`Plugins/VoxelCharacterPlugin/Documentation/MAP_UI_WIDGETS.md`).
What they ask of the map:

- **The minimap** (`UVCMinimapWidget`) fills its texture from the tile pyramid at its level (1 at the defaults) only when
  it must: with no texture, after `RefillShiftTexels` (2) texels of movement, when an image of a tile it shows changed (a
  signature over them), or after `MaxRefillSeconds` (2 s); in between it shifts the image by the movement. Each fill asks
  for the tiles it showed (`RequestView`, the minimap first). Before world modes review G64 it filled every refresh, ten
  times a second, 2-3 ms each. `vc.Minimap.Stats` reports the fills.
- **The world map** (`UVCWorldMapWidget`) composes its view from the pyramid at the level matching its texels and asks for
  those tiles at each rebuild (`RequestView`). Only the arrival of a tile in its view counts, and arrivals coalesce into at
  most one rebuild every quarter second (`FVCMapArrivalCoalescer`, G166: a rebuild per arrival held the 50 km globe at 94 ms
  frames; test `VoxelCharacter.Map.ArrivalsInViewCoalesce`); new exploration redraws the fog at most once a second.
- **Markers** (VoxelWorldPOI, `UPOIMapMarkerSubsystem`): a marker query is clamped to `MarkerMaxCells` (20,000) placement
  cells around its centre in every mode, and the map draws at most `MaxMarkers` (64) (G50: on a 1,000 km planet the
  zoomed-out globe asked for all 36 million POI cells and held the game thread for 1 min 45 s; test
  `VoxelWorlds.POI.Map.MarkerCap`).

## Key Packing

`FIntPoint` coordinates are packed into `uint64` keys for `TMap`/`TSet` storage:

```cpp
Key = (uint64(uint32(X)) << 32) | uint64(uint32(Y))
```

This avoids `GetTypeHash` issues with `FIntPoint` in some container configurations.

## World Mode Compatibility

The subsystem is world-mode-agnostic by design:
- What depends on the kind of world (a heightmap's chunk-column grid, a planet's cube-sphere atlas) is in
  `FVoxelMapTileFrame`; the drawer, the queue, the store and the saves are shared
- Heights come from the world mode's generated surface (above), not the legacy `GetTerrainHeightAt`, which on a planet
  only covers the upper hemisphere
- Biome sampling uses the same pipeline as generation regardless of world mode
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
- **Saves**: the same blobs with atlas keys and version-2 overlays; a save made under another generation version
  (`FVoxelWorldModeFactory::PlanetGenerationVersion`, 6) is refused with the rest of the world.

**At scale** (spherical Phase 6, WM-BP11). A 1,000 km planet's atlas is 32,725 tiles a
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
  `voxel.Map.ExploreAllMaxTiles` tiles, nearest the pawn first (`GatherWithinAngle`'s flood fill); since the pyramid
  (P1) it queues no image. The small planet (40,000 tiles) is explored whole as before.
- **Markers and redraws** on a zoomed-out globe follow the widget rules above (G50, G51); the last marker query's POIs
  are reused by the half-second refresh, and the albedo's own arrival redraws at once.
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
| `Source/VoxelMap/Public/VoxelMapTypes.h` | FVoxelMapTileId (a tile at a pyramid level), FVoxelMapTile |
| `Source/VoxelMap/Public/VoxelMapTileStore.h` | FVoxelMapTileStore: every level's images in one LRU under one budget (P1) |
| `Source/VoxelMap/Public/VoxelMapTileQueue.h` | FVoxelMapTileQueue: requests by requester and view order, per-tile generations (R22, P1) |
| `Source/VoxelMap/Public/VoxelMapEditOverlay.h` | FVoxelMapEditOverlay: edited columns drawn over the analytic terrain (R22) |
| `Source/VoxelMap/Public/VoxelMapTileFrame.h` | FVoxelMapTileFrame: the tile grid and its pyramid, samples, altitudes and shading per kind of world |
| `Source/VoxelMap/Public/VoxelMapSubsystem.h` | UVoxelMapSubsystem declaration |
| `Source/VoxelMap/Private/VoxelMapSubsystem.cpp` | Subsystem: exploration, queue, budgets, edit refresh |
| `Source/VoxelMap/Private/VoxelMapTileDrawing.cpp` | The tile task (`GenerateTileAsync`) and its completion |
| `Source/VoxelMap/Private/VoxelMapGlobe.cpp` | The planet globe albedo |
| `Source/VoxelMap/Private/VoxelMapPersistence.cpp` | Explored tiles and edit overlays to and from saves |

## See Also

- [BIOME_SYSTEM.md](BIOME_SYSTEM.md) — Biome definitions and blend logic used by tile coloring
- [MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) — Material ID to color mapping
- [LOD_SYSTEM.md](LOD_SYSTEM.md) — UVoxelChunkManager and OnChunkGenerated delegate
- [NOISE_LIBRARY.md](NOISE_LIBRARY.md) — FVoxelCPUNoiseGenerator and FBM3D noise sampling
