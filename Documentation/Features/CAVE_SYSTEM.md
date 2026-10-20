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
| `VerticalScale` | 0.5 | Vertical squash (<1 = flatter caves): world Z in heightmap modes, the altitude along local up on a planet (`FSphericalPlanetWorldMode::ScaleAltitude`) |
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

`UVoxelChunkManager::BuildChunkGenerationRequest()` (called by `ProcessGenerationQueue()`) copies cave settings into
`FVoxelNoiseGenerationRequest`:
- `GenRequest.bEnableCaves` from `Configuration->bEnableCaves`
- `GenRequest.CaveConfiguration` from `Configuration->CaveConfiguration`

### CPU Path

`VoxelCPUNoiseGenerator` has two static methods:
- `SampleCaveLayer()` — samples a single layer's carve density
- `CalculateCaveDensity()` — iterates all layers with depth/biome constraints

All three world mode generators call `CalculateCaveDensity()` after base density calculation.

### GPU Path

Cave parameters are passed as flat arrays (up to 4 layers) to the compute shader. `CaveGeneration.ush` provides
`SampleCheeseCave()` and `SampleTunnelCave()`. `GpuCaveCarve()` in `GenerateVoxelDensity.usf` is the GPU twin of
`CalculateCaveDensity()`, shared by every world mode: the biome overrides (`BiomeOverrides`, uploaded as a small table
of biome id, cave scale and minimum-depth override; the first match wins), `UnderwaterMinDepth` where the column's
terrain is under the sea, each layer's depth window and fades, the union, the biome scale. Only the noise sample
position differs: `VerticalScale` along Z for InfinitePlane and IslandBowl, which share the heightmap branch, along
local up on spherical planets (`SphericalPlanet.ush`). As on the CPU, the biome blend comes before the caves. Locked by
`GPUvsCPUCavesParity` (the core carving, the biome overrides, a cave scale of 0.5, and a chunk under the sea that carves
without `UnderwaterMinDepth` and not with it) and `GPUvsCPUSphericalParity`. Until G18 was fixed (2026-10-17, world
modes review plan §14.1) the heightmap branch ignored the overrides and `UnderwaterMinDepth`, so GPU worlds carved caves
under the sea and in biomes scaled to 0; the fix bumped `FVoxelWorldModeFactory::GenerationVersion` to 2.

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

### A Layer That Fills Its Depth Window
A layer whose noise carves most of space fills its depth window as one cavern, with pillars where the noise leaves
rock. That happens with a Spaghetti or Noodle threshold near 0.5 (the struct's default, which suits Cheese) or a low
Cheese threshold. Its roof is then the window's fade-in, not the noise:
- The carve is multiplied by a smoothstep over depths `MinDepth - DepthFadeWidth` to `MinDepth`, and a voxel stays
  solid while the carve is under about 0.5. So where the noise carves fully, the roof lies `MinDepth - DepthFadeWidth / 2`
  voxels under the terrain everywhere, parallel to it (flat under flat ground). The floor lies
  `MaxDepth + DepthFadeWidth / 2` down.
- Under the sea, `UnderwaterMinDepth` replaces `MinDepth` per column, so the roof steps down along the coast.

`VoxelDemo`'s `DemoCaveConfig` was such a layer until 2026-10-20: Spaghetti at threshold 0.5, depths 20 to 45, fade 4,
with roofs 1,350 uu under the ground and 1,725 under the seabed (world modes review G34, §19.12). `voxel.Gen.LayerAudit`
counts cave ceilings and floors per Z plane over the loaded chunks and compares the chunks with the most on a chunk-layer
boundary with a CPU regeneration, to tell a configured roof from a generation artefact.

### The Demo Configuration
`DemoCaveConfig` (VoxelDemo, and every review, planet, cubic and seam test configuration that references it) has been
tunnels and caverns since 2026-10-20 (world modes review §19.15):

| Layer | Enabled | Threshold | Frequency | Depth (voxels) | Fade | Vertical scale |
|-------|---------|-----------|-----------|----------------|------|----------------|
| 0 Cheese | yes | 0.33 | 0.00025 | 27 down, no floor | 6 | 0.6 |
| 1 Spaghetti | yes | 0.15 | 0.0002 | 20 to 45 | 4 | 0.5 |
| 2 Noodle | no | 0.16 | 0.0003 | 2 to 15 | 3 | 0.45 |

`UnderwaterMinDepth` 25 (both layers start 25 voxels under a seabed), biome 3 cave scale 0. Nothing carves within 16
voxels (1,200 uu) of the ground, so there are no openings at the surface; enabling the Noodle layer would add them.
Under VoxelDemo's land (CPU generation of 3 × 3 chunk columns at the spawn, the Village, the Tower and the City), the
share of air by depth under the ground:

| Depth (voxels) | 15-19 | 20-24 | 25-34 | 35-44 | 45-49 | 50-63 | Columns with a cave in 16-49 | Mean cave height |
|----------------|-------|-------|-------|-------|-------|-------|------------------------------|------------------|
| Before (Spaghetti 0.5) | 31.7% | 80.8% | 81.2% | 80.9% | 31.2% | 0 | 95.3% | 23.3 voxels |
| Spaghetti 0.15 + Cheese | 4.6% | 13.0% | 18.6% | 17.3% | 10.1% | 6.2% | 43.9% | 10.2 voxels |

The Cheese layer has no `MaxDepth`, so its caverns (about 6% of the rock) continue below the tunnels as deep as the
world streams. That has a cost: chunks under the tunnels that were uniform rock now have a surface, so on `VoxelDemo`
about 180 more chunks draw (+42 to 50%) with 17 to 28% more triangles and renderer memory. A `MaxDepth` on the Cheese
layer would end them. Spaghetti thresholds of 0.10 and 0.20 (with the Cheese layer) give 9.2% and 21.8% air over depths 16 to
49 against 14.6% at 0.15.

## Underground Classification

Cave carving sets `VOXEL_FLAG_UNDERGROUND` on all carved air voxels. After generation completes, `ApplyUndergroundClassificationPass()` extends this flag to additional underground air (via column scan) and to solid boundary voxels (via neighbor propagation). The scatter system uses these flags to filter `EScatterSurfaceLocation::SurfaceOnly` and `UndergroundOnly` scatter definitions.

See [UNDERGROUND_CLASSIFICATION.md](UNDERGROUND_CLASSIFICATION.md) for full details on the flag propagation pipeline and scatter surface point classification.

## Performance

- CPU: ~15-30% overhead per chunk with 3 default layers (3D noise sampling is the cost)
- GPU: ~10-20% overhead (parallel noise evaluation)
- The `bEnableCaves` check short-circuits when disabled (zero cost)
- Caves only sample noise for voxels that are solid (`Density >= 127`) and below surface
