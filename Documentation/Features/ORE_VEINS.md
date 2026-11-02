# Ore Veins

> State as of 2026-11-02 (code review CR5).

**Modules**: VoxelCore (configuration), VoxelGeneration (placement, CPU and GPU)

## Overview

Ore veins replace the material of solid voxels underground where a 3D noise field passes a threshold. Ores are
configured globally (every biome) or per biome, in two shapes (blobs and streaks). They are placed during generation
on the CPU (`FVoxelCPUNoiseGenerator::CheckOreVeinPlacement`) and the GPU (`GpuCheckPlanetOreVein` in
`SphericalPlanet.ush`) alike, in every world mode, and are pinned by `Generation.GPUvsCPUOreVariantParity` (heightmap)
and `Generation.GPUvsCPUSphericalParity` (a planet, with a gold ore at `Rarity` 0.5).

## FOreVeinConfig (`VoxelBiomeDefinition.h`)

| Field | Default | Description |
|-------|---------|-------------|
| `Name` | "" | display name |
| `MaterialID` | 0 | the ore's material (an index into the material atlas) |
| `MinDepth` | 3.0 | minimum depth below the surface, in voxels |
| `MaxDepth` | 0.0 | maximum depth (0 = unlimited) |
| `Shape` | `Blob` | `EOreVeinShape::Blob` or `Streak` |
| `Frequency` | 0.05 | noise frequency (0.001-1.0; lower = larger deposits) |
| `Threshold` | 0.85 | the noise value (in [0, 1]) a voxel must reach (higher = rarer) |
| `SeedOffset` | 0 | added to the world seed |
| `Rarity` | 1.0 | below 1, keeps only that share of the voxels that pass |
| `StreakStretch` | 4.0 | a streak's elongation (1-10) |
| `Priority` | 0 | evaluation order, higher first; the first ore that places wins |

## Placement

For each solid voxel deeper than 10 voxels under the surface (the guard keeps ores out of the smooth surface's
sampling range), after the biome material, height rules and the cave-wall override (`BIOME_SYSTEM.md`):

1. The applicable ores for the voxel's leading biome (below), in priority order.
2. For each ore whose depth window (`MinDepth`, `MaxDepth`) holds the voxel: its noise, a 2-octave Simplex field at
   the ore's frequency and seed (`SampleOreVeinNoise`, mapped to [0, 1]). On a planet the field is sampled at the
   planet-relative position (`SamplePlanetOreVeinNoise`), so the ores move with the planet.
3. Where the noise reaches `Threshold` and the `Rarity` test passes, the voxel takes the ore's material.

### Shapes

- **Blob**: rounded, irregular clusters. `Frequency` sets their size, `Threshold` how many.
- **Streak**: elongated veins. The position is squashed along one direction per ore type by `StreakStretch`
  (`GetPlanetOreSamplePoint`; the direction from the ore's `SeedOffset`, `GetPlanetOreStreakDirection`), a linear map,
  so the GPU can sample a chunk's origin and add each voxel's offset. Every world mode uses it since MP4; the heightmap
  form before it turned the direction with the absolute position, which became white noise far from the origin.

### Rarity

Below 1, `Rarity` keeps an ore voxel when an integer hash of its position, rounded to whole uu, is at most `Rarity`
(`OreRarityKey`, `OreRarityHash`; the GPU's `GpuOreRarityHashKey` in `BiomeMaterial.ush` is the same bits). A planet
hashes the planet-relative position (spherical Phase 4, SP-H3; `PlanetGenerationVersion` 3), a heightmap world the
absolute position (MP4, `GenerationVersion` 3). The hash replaced `frac(sin(p . k))`, whose argument near 10^7 the GPU's float and the CPU's
double round differently (`VoxelWorlds.WorldModes.Planet.OreRarityHash`).

## Global and biome ores

`UVoxelBiomeConfiguration` holds `bEnableOreVeins` (true) and `GlobalOreVeins`; each `FBiomeDefinition` may hold
`BiomeOreVeins` and `bAddToGlobalOres`. `UVoxelBiomeConfiguration::GetOreVeinsForBiome` resolves them:

| The biome's `BiomeOreVeins` | `bAddToGlobalOres` | Ores used |
|-----------------------------|--------------------|-----------|
| empty | n/a | `GlobalOreVeins` |
| not empty | false | only the biome's |
| not empty | true | both, sorted by priority |

`bEnableOreVeins` false places none. For the GPU, `BuildGpuData` flattens each biome's resolved list into one buffer
(`BakedGpuOreVeins`) that the biome's `OreStart` and `OreCount` index.

## Default ores

`InitializeDefaults()` gives a new biome configuration three global ores. Their materials are placeholders until ore
textures exist in the atlas:

| Ore | Material | Depth (voxels) | Shape | Frequency | Threshold | Seed offset | Priority |
|-----|----------|----------------|-------|-----------|-----------|-------------|----------|
| Coal | Stone (placeholder) | 12-60 | Blob | 0.08 | 0.82 | 100 | 10 |
| Iron | Stone (placeholder) | 15-100 | Streak | 0.06 | 0.87 | 200 | 20 |
| Gold | Sand (placeholder) | 30 and deeper | Blob | 0.04 | 0.93 | 300 | 30 |

## Example: a biome-only ore

```cpp
FBiomeDefinition& Desert = Config->Biomes[1];

FOreVeinConfig DesertGold;
DesertGold.Name = TEXT("Desert Gold");
DesertGold.MaterialID = EVoxelMaterial::Gold;
DesertGold.MinDepth = 15.0f;
DesertGold.MaxDepth = 40.0f;
DesertGold.Frequency = 0.05f;
DesertGold.Threshold = 0.88f;
DesertGold.SeedOffset = 350;
DesertGold.Priority = 25;

Desert.BiomeOreVeins.Add(DesertGold);
Desert.bAddToGlobalOres = false;   // replace the global ores in this biome
Config->RebuildCaches();           // and rebuild the GPU data before generation reads it
```

In the editor the same is done in the `UVoxelBiomeConfiguration` asset: the "Ore Veins" category for global ores,
each biome's `BiomeOreVeins` for its own.

## Tuning

- **Rarer**: raise `Threshold` (0.85 to 0.95), lower `Rarity`, raise `MinDepth`. A `Threshold` between 0.7 and 0.98
  and a `Frequency` between 0.01 and 0.2 give visible deposits.
- **Larger deposits**: lower `Frequency` (0.05 to 0.02), lower `Threshold` slightly.
- **Longer streaks**: `Streak` with a larger `StreakStretch` (4 to 8).
- **Overlap**: the higher priority ore is tried first and wins; give each ore its own `SeedOffset`.

## Performance

Each applicable ore adds one 2-octave noise sample per solid voxel deeper than 10 voxels, after the depth check
(`IsValidDepth`); the priority-sorted global list is cached in the configuration.
