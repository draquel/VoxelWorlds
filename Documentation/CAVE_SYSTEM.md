# Cave System

## Overview

The cave system generates underground cavities by subtracting density from the base terrain SDF. It operates as a post-process applied **after** each world mode calculates base terrain density, making it compatible with all three world modes (InfinitePlane, IslandBowl, SphericalPlanet).

```
FinalDensity = max(0, BaseDensity - CaveDensity * 255)
```

## Architecture

### Density Subtraction Model

Caves work by reducing voxel density in solid terrain. This approach:
- Works identically across all 3 world modes
- Composes naturally with the existing SDF convention (positive = solid, 127 = surface)
- Requires no changes to world mode SDF logic
- Edit layer, collision, scatter, and ore veins all work automatically

### Pipeline Position

Cave carving is inserted per-voxel **after** base density + biome selection and **before** ore vein checks:

1. Sample terrain noise → TerrainHeight
2. Calculate Density from SDF
3. Calculate DepthBelowSurface
4. Sample biome → BiomeID
5. **Cave carving** (subtract density if CalculateCaveDensity > 0)
6. Material assignment (biome/depth/water)
7. Cave wall material override (optional)
8. Height material rules
9. Ore vein overrides (only solid voxels → caves already carved to air are skipped)

## Cave Types

### Cheese Caves (`ECaveType::Cheese`)

Large, open caverns created by single noise threshold carving.

**Algorithm:** Sample 3D fBm noise. Where noise exceeds `Threshold`, carve with smooth falloff.

**Characteristics:**
- Produces large, organic cavern shapes
- Size controlled primarily by `Frequency` (lower = larger)
- `Threshold` controls how common caves are (higher = rarer, smaller)
- Good for underground lakes, boss arenas, large rooms

**Default parameters:** Freq=0.003, Threshold=0.65, MinDepth=8, VertScale=0.6

### Spaghetti Caves (`ECaveType::Spaghetti`)

Winding tunnel networks created by dual-noise field intersection.

**Algorithm:** Sample two independent 3D fBm noise fields. A tunnel forms where **both** noise fields are simultaneously near zero (within [-Threshold, Threshold]).

**Characteristics:**
- Produces long, winding passages
- Tunnel width controlled by `Threshold` (lower = thinner)
- Second noise field frequency scale creates varied curvature
- Good for exploration tunnels, mining passages

**Default parameters:** Freq=0.006, Threshold=0.15, MinDepth=5, VertScale=0.4

### Noodle Caves (`ECaveType::Noodle`)

Thin, narrow passages created by tight dual-noise intersection.

**Algorithm:** Same as Spaghetti but with tighter threshold and higher frequency.

**Characteristics:**
- Very thin passages, barely walkable
- Creates intricate networks of hairline cracks
- Higher second noise frequency scale for more variation
- Good for water channels, lava tubes, connecting passages

**Default parameters:** Freq=0.012, Threshold=0.08, MinDepth=3, VertScale=0.35

## Configuration

### UVoxelCaveConfiguration

Data asset (create via Content Browser → Data Asset → VoxelCaveConfiguration).

| Property | Type | Description |
|----------|------|-------------|
| `bEnableCaves` | bool | Master enable for cave generation |
| `CaveLayers` | TArray\<FCaveLayerConfig\> | Composable cave layers |
| `BiomeOverrides` | TArray\<FBiomeCaveOverride\> | Per-biome scaling |
| `bOverrideCaveWallMaterial` | bool | Apply special material on cave walls |
| `CaveWallMaterialID` | uint8 | Material index for cave walls |
| `CaveWallMaterialMinDepth` | float | Minimum depth for wall material override |

### FCaveLayerConfig

Each layer generates one type of cave independently. Layers compose via union (max carve density).

| Property | Default | Description |
|----------|---------|-------------|
| `bEnabled` | true | Enable this layer |
| `CaveType` | Cheese | Cave geometry type |
| `SeedOffset` | 0 | Added to world seed for unique patterns |
| `Frequency` | 0.005 | Base noise frequency (lower = larger) |
| `Octaves` | 3 | fBm octave count |
| `Persistence` | 0.5 | Amplitude decay per octave |
| `Lacunarity` | 2.0 | Frequency growth per octave |
| `Threshold` | 0.5 | Noise threshold for carving |
| `CarveStrength` | 1.0 | Maximum carve intensity [0, 1] |
| `CarveFalloff` | 0.1 | Edge smoothness |
| `MinDepth` | 5.0 | Minimum depth below surface (voxels) |
| `MaxDepth` | 0.0 | Maximum depth (0 = unlimited) |
| `DepthFadeWidth` | 4.0 | Fade zone width at depth boundaries |
| `VerticalScale` | 0.5 | Vertical squash (<1 = flatter caves) |
| `SecondNoiseSeedOffset` | 7777 | Seed for dual-noise (Spaghetti/Noodle) |
| `SecondNoiseFrequencyScale` | 1.2 | Frequency ratio for second noise |

### FBiomeCaveOverride

| Property | Default | Description |
|----------|---------|-------------|
| `BiomeID` | 0 | Biome index to override |
| `CaveScale` | 1.0 | Multiplier (0 = no caves) |
| `MinDepthOverride` | -1.0 | Override MinDepth (-1 = use layer default) |

## Wiring

### World Configuration

In `UVoxelWorldConfiguration`:
- `bEnableCaves` — master enable
- `CaveConfiguration` — reference to `UVoxelCaveConfiguration` data asset

### Generation Pipeline

`VoxelChunkManager::ProcessGenerationQueue()` copies cave settings into `FVoxelNoiseGenerationRequest`:
- `GenRequest.bEnableCaves` from `Configuration->bEnableCaves`
- `GenRequest.CaveConfiguration` from `Configuration->CaveConfiguration`

### CPU Path

`VoxelCPUNoiseGenerator` has two static methods:
- `SampleCaveLayer()` — samples a single layer's carve density
- `CalculateCaveDensity()` — iterates all layers with depth/biome constraints

All three world mode generators call `CalculateCaveDensity()` after base density calculation.

### GPU Path

Cave parameters are passed as flat arrays (up to 4 layers) to the compute shader. `CaveGeneration.ush` provides `SampleCheeseCave()` and `SampleTunnelCave()`. Currently implemented for InfinitePlane mode only.

## Tuning Guide

### Making Larger Caves
- Decrease `Frequency` (e.g., 0.001)
- Increase `Threshold` slightly for Cheese
- Decrease `VerticalScale` for flatter caverns

### Making More Caves
- Lower `Threshold` for Cheese caves
- Increase `Threshold` for Spaghetti/Noodle
- Add more layers

### Preventing Surface Breakout
- Increase `MinDepth` (default 5-8 voxels)
- Increase `DepthFadeWidth` for softer transitions

### Biome-Specific Caves
- Use `FBiomeCaveOverride` with `CaveScale = 0` to disable in specific biomes
- Use `MinDepthOverride` to push caves deeper in flat biomes

## Performance

- CPU: ~15-30% overhead per chunk with 3 default layers (3D noise sampling is the cost)
- GPU: ~10-20% overhead (parallel noise evaluation)
- The `bEnableCaves` check short-circuits when disabled (zero cost)
- Caves only sample noise for voxels that are solid (`Density >= 127`) and below surface
