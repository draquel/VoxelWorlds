# Map System

**Module**: VoxelMap
**Dependencies**: VoxelCore, VoxelGeneration, VoxelStreaming

## Overview

The Map System generates 2D top-down map tile data from voxel terrain. Each tile corresponds to one chunk's XY footprint and stores a color image of the terrain surface. Tile generation is fully async and uses deterministic terrain height queries — no loaded chunk data is required.

The system lives entirely within the VoxelWorlds plugin and has zero knowledge of players, characters, or UI. External consumers (such as minimap or world map widgets in VoxelCharacterPlugin) query the subsystem for tile data and drive exploration via `RequestTilesInRadius()`.

## Architecture

```
UVoxelMapSubsystem (UWorldSubsystem)
├── Tile Cache: TMap<uint64, FVoxelMapTile>
├── Exploration Mask: TSet<uint64>
├── Pending Queue: TSet<uint64>
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
    int32 Resolution;          // Pixels per edge (matches ChunkSize, e.g. 32)
    uint8 Version;             // Format version for future save/load
    bool bIsReady;             // Runtime flag: true when fully generated
};
```

Each tile maps 1:1 to a chunk's XY footprint. At `Resolution=32` (matching ChunkSize), each pixel represents one voxel column. At default settings (32 voxels * 100 units), one tile covers 3200 world units.

All data fields use `UPROPERTY()` for future `FArchive` serialization. `bIsReady` is intentionally not serialized as it is runtime-only state.

### UVoxelMapSubsystem

World subsystem that manages tile generation, caching, and exploration tracking.

**Public API:**

| Method | Description |
|--------|-------------|
| `GetTile(FIntPoint)` | Get a generated tile, or nullptr if not ready |
| `HasTile(FIntPoint)` | Check if a tile has been generated |
| `GetTileCache()` | Get the full tile cache for bulk iteration |
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

### Resolution and Memory

- One 32x32 tile = 1024 pixels = 4KB
- 1000 tiles = 4MB — negligible memory footprint
- One tile takes ~1ms to generate (1024 noise samples at ~1us each)
- All generation is async — zero game thread cost beyond result marshaling

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
- For SphericalPlanet, tiles act as a flat map projection; the data layer works unchanged
- For IslandBowl, areas beyond the island edge render based on their material (water/void)

## File Listing

| File | Description |
|------|-------------|
| `Source/VoxelMap/VoxelMap.Build.cs` | Module build configuration |
| `Source/VoxelMap/Public/VoxelMap.h` | Module API header |
| `Source/VoxelMap/Private/VoxelMap.cpp` | Module startup/shutdown |
| `Source/VoxelMap/Public/VoxelMapTypes.h` | FVoxelMapTile struct |
| `Source/VoxelMap/Public/VoxelMapSubsystem.h` | UVoxelMapSubsystem declaration |
| `Source/VoxelMap/Private/VoxelMapSubsystem.cpp` | Subsystem implementation |

## See Also

- [BIOME_SYSTEM.md](BIOME_SYSTEM.md) — Biome definitions and blend logic used by tile coloring
- [MATERIAL_REGISTRY.md](MATERIAL_REGISTRY.md) — Material ID to color mapping
- [CHUNK_STREAMING.md](CHUNK_STREAMING.md) — UVoxelChunkManager and OnChunkGenerated delegate
- [NOISE_GENERATION.md](NOISE_GENERATION.md) — FVoxelCPUNoiseGenerator and FBM3D noise sampling
