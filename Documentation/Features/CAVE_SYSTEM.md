# Cave System

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

## Overview

Caves are carved by subtracting density from the terrain each world mode generates, so the same carve serves all three
modes (InfinitePlane, IslandBowl, SphericalPlanet; cubic worlds on either heightmap mode):

```
FinalDensity = max(0, BaseDensity - CaveDensity * 255)
```

The carve composes with the SDF convention (above 127 solid), and the edit layer, collision, scatter and ores need
nothing of their own for it.

### Pipeline position

Per voxel, in the CPU generator (`FVoxelCPUNoiseGenerator`) and the density shader (`GpuCaveCarve`) alike:

1. Terrain height (or a planet's terrain altitude) and the density from the SDF.
2. Depth below the surface, in voxels (along local up on a planet).
3. The biome blend; the leading biome's ID (`BIOME_SYSTEM.md`).
4. **Cave carving**, for solid voxels below the surface, where `CalculateCaveDensity` > 0.
5. The material: biome, depth, underwater.
6. Height material rules.
7. The cave-wall material override (optional).
8. Ore veins (only voxels still solid: a carved voxel is air).

## Cave Types (`ECaveType`)

- **Cheese**: large caverns. One 3D fBm field; where it exceeds `Threshold` the voxel carves, with a smooth falloff
  (`CarveFalloff`). `Frequency` sets the size (lower is larger), `Threshold` how common they are (higher is rarer).
- **Spaghetti**: winding tunnels. Two independent fBm fields (the second at `SecondNoiseSeedOffset` and
  `SecondNoiseFrequencyScale`); a tunnel forms where both are within [-`Threshold`, `Threshold`] of zero, so
  `Threshold` sets the tunnel width.
- **Noodle**: the Spaghetti algorithm with a tighter threshold and higher frequency: thin passages.

## Configuration

### UVoxelCaveConfiguration (data asset)

| Property | Default | Description |
|----------|---------|-------------|
| `bEnableCaves` | true | the asset's own switch |
| `CaveLayers` | three layers (`InitializeDefaults`, below) | composable layers |
| `BiomeOverrides` | none | per-biome scale and minimum depth |
| `UnderwaterMinDepth` | 0 | minimum depth below a seabed where the column's terrain is under the sea (0 = off) |
| `bOverrideCaveWallMaterial` | false | a material on still-solid voxels partly carved near a cave |
| `CaveWallMaterialID` | 2 | that material |
| `CaveWallMaterialMinDepth` | 10 | minimum depth for the override, in voxels |

A world carves caves when `UVoxelWorldConfiguration::bEnableCaves` is set and `CaveConfiguration` names an asset whose
`bEnableCaves` is set; without an asset nothing carves.

`InitializeDefaults()` gives a new asset three layers: Cheese (frequency 0.00025, threshold 0.33, from 27 voxels down,
fade 6, vertical scale 0.6), Spaghetti (0.0002, threshold 0.5, 10 to 30 voxels) and Noodle (0.0003, threshold 0.16, 2 to
15 voxels, fade 3, vertical scale 0.45).

### FCaveLayerConfig

Each layer carves independently; layers compose by union (the largest carve).

| Property | Default | Description |
|----------|---------|-------------|
| `bEnabled` | true | enable this layer |
| `CaveType` | Cheese | the cave type |
| `SeedOffset` | 0 | added to the world seed |
| `Frequency` | 0.005 | base noise frequency (lower = larger) |
| `Octaves` | 3 | fBm octaves |
| `Persistence` | 0.5 | amplitude step per octave |
| `Lacunarity` | 2.0 | frequency step per octave |
| `Threshold` | 0.5 | carve threshold (suits Cheese; see below for tunnels) |
| `CarveStrength` | 1.0 | maximum carve [0, 1] |
| `CarveFalloff` | 0.1 | edge smoothness |
| `MinDepth` | 5.0 | minimum depth below the surface (voxels) |
| `MaxDepth` | 0.0 | maximum depth (0 = unlimited) |
| `DepthFadeWidth` | 4.0 | fade width at both depth limits |
| `VerticalScale` | 0.5 | vertical squash (below 1 = flatter caves): world Z on a heightmap, the altitude along local up on a planet (`FSphericalPlanetWorldMode::ScaleAltitude`) |
| `SecondNoiseSeedOffset` | 7777 | the second field's seed (Spaghetti, Noodle) |
| `SecondNoiseFrequencyScale` | 1.2 | the second field's frequency ratio |

### FBiomeCaveOverride

| Property | Default | Description |
|----------|---------|-------------|
| `BiomeID` | 0 | the biome |
| `CaveScale` | 1.0 | carve multiplier (0 = no caves) |
| `MinDepthOverride` | -1.0 | replaces the layers' `MinDepth` (-1 = keep) |

## Implementation

`UVoxelChunkManager::BuildChunkGenerationRequest` copies `bEnableCaves` and `CaveConfiguration` into the
`FVoxelNoiseGenerationRequest`.

**CPU.** `FVoxelCPUNoiseGenerator::SampleCaveLayer` samples one layer's carve; `CalculateCaveDensity` applies every
layer with its depth window and fades, the biome override (the first match) and `UnderwaterMinDepth`, takes the union
and the biome scale. Every world mode's generator calls it after the base density. `FVoxelCaveQuery` answers "how
carved is this point" with the same math without a chunk (editor field views, gameplay).
`UVoxelCaveConfiguration::AnyLayerReachesDepth` lets a reader skip the biome lookup outside every layer's window.

**GPU.** The layers go to the density shader as packed vectors (up to 4 layers, `VOXELGEN_MAX_GPU_CAVE_LAYERS`), the
biome overrides as a small table (`CaveBiomeOverrideData`). `GpuCaveCarve` in `GenerateVoxelDensity.usf` is the twin
of `CalculateCaveDensity` for every world mode; `CaveGeneration.ush` holds the carve-from-noise tails
(`CheeseCarveFromNoise`, `TunnelCarveFromNoise`). Only the sample position differs: `VerticalScale` along Z on the
heightmap branch, along local up on a planet (`SphericalPlanet.ush`). The GPU applies the biome overrides and
`UnderwaterMinDepth` as the CPU does since G18 (before it, GPU heightmap worlds carved under the sea and in biomes
scaled to 0; the fix bumped `GenerationVersion` to 2). Pinned by `Generation.GPUvsCPUCavesParity` (the core carve, the
overrides, a cave scale of 0.5, a chunk under the sea with and without `UnderwaterMinDepth`) and
`Generation.GPUvsCPUSphericalParity`.

**Readers past a chunk.** Scatter extraction reads the voxels just past a chunk's faces from the generator, and carves
them the same way: the voxel over a heightmap chunk's top layer at any depth (G28) and a planet chunk's voxels past its
+X, +Y and +Z faces (G72), so cave floors at chunk faces get their points (`SCATTER_SYSTEM.md`).

## Tuning Guide

- **Larger caves**: lower `Frequency` (e.g. 0.001); for Cheese raise `Threshold` slightly; lower `VerticalScale` for
  flatter caverns.
- **More caves**: lower `Threshold` for Cheese, raise it for Spaghetti and Noodle; add layers.
- **No surface openings**: raise `MinDepth`, and `DepthFadeWidth` for a softer start.
- **Per biome**: an `FBiomeCaveOverride` with `CaveScale` 0 removes caves from a biome; `MinDepthOverride` pushes them
  deeper.

### A layer that fills its depth window

A layer whose noise carves most of space fills its depth window as one cavern, with pillars where the noise leaves
rock: a Spaghetti or Noodle threshold near 0.5 (the struct's default, which suits Cheese) or a low Cheese threshold.
Its roof is then the window's fade, not the noise:
- The carve is multiplied by a smoothstep over depths `MinDepth - DepthFadeWidth` to `MinDepth`, and a voxel stays
  solid while the carve is under about 0.5. So where the noise carves fully, the roof lies
  `MinDepth - DepthFadeWidth / 2` voxels under the terrain everywhere, parallel to it (flat under flat ground). The
  floor lies `MaxDepth + DepthFadeWidth / 2` down.
- Under the sea, `UnderwaterMinDepth` replaces `MinDepth` per column, so the roof steps down along the coast.
- Under a conditioning line with a minimum cave depth (`FVoxelConditioningLineSurface::MinCaveDepth`; `WORLD_MODES.md`,
  "The surface under a line") every layer's minimum depth is at least the line's on its core, easing back to the
  layer's own across the line's falloff, so a road has no hole in it. It only deepens: applied after the biome
  override and the underwater depth, as the larger of the two (`CalculateCaveDensity`'s `MinDepthUnderLines`, the
  shader's `GpuCaveCarve`), on the CPU, the GPU and in the single-voxel reader (`FVoxelSurfaceQuery::GenerateVoxelAt`).
  Test `VoxelWorlds.Generation.ConditioningLine.CaveUnderLine`.

`VoxelDemo`'s cave configuration was such a layer until 2026-10-20 (Spaghetti at 0.5, depths 20 to 45): the flat
ceilings 1,350 uu under the ground were its roof, not an artefact (G34).

### The demo configuration

`DemoCaveConfig` (`VoxelDemo`, and every review, planet, cubic and seam test configuration that references it) holds
tunnels and caverns:

| Layer | Enabled | Threshold | Frequency | Depth (voxels) | Fade | Vertical scale |
|-------|---------|-----------|-----------|----------------|------|----------------|
| 0 Cheese | yes | 0.33 | 0.00025 | 27 to 60 | 6 | 0.6 |
| 1 Spaghetti | yes | 0.15 | 0.0002 | 20 to 45 | 4 | 0.5 |
| 2 Noodle | no | 0.16 | 0.0003 | 2 to 15 | 3 | 0.45 |

`UnderwaterMinDepth` 25 (both layers start 25 voxels under a seabed), biome 3 cave scale 0. Nothing carves within 16
voxels (1,200 uu) of the ground, so there are no openings at the surface; enabling the Noodle layer would add them.
Under the demo's land about 15% of the rock 16 to 49 voxels down is cave, in 44% of the columns. The Cheese layer's
`MaxDepth` 60 matters: without a floor its caverns went on as deep as the world streams and gave chunks of uniform
rock a surface (about 180 more chunks drawn, 17 to 28% more triangles); with it the caverns fade out by 66 voxels.
History: `WORLD_MODES_REVIEW_PLAN.md` §19.15.

## Underground Classification

Cave carving sets `VOXEL_FLAG_UNDERGROUND` on carved air. After generation, `ApplyUndergroundClassificationPass` (and
its GPU post-pass) extends the flag to further underground air (a column scan) and to solid boundary voxels
(neighbour propagation). Scatter definitions choose where they spawn by `SurfaceLocationMask`
(`EScatterSurfaceLocationFlags`: `Surface`, `Underwater`, `Underground`), read from these flags. See
[UNDERGROUND_CLASSIFICATION.md](UNDERGROUND_CLASSIFICATION.md).

## Performance

- Caves sample noise only for solid voxels (`Density >= 127`) below the surface; a disabled configuration costs
  nothing.
- With the demo configuration CPU generation costs about 8% more a chunk than with the single cavern layer it
  replaced.
