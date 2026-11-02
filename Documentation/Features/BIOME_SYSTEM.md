# Biome System

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Modules**: VoxelCore (data, selection, blend), VoxelGeneration (sampling the fields, CPU and GPU)

## Overview

A biome decides a column's materials: the surface, subsurface and deep material by depth, their underwater variants,
and which ore veins it carries. Biomes are chosen by three noise fields: temperature, moisture and continentalness.
Continentalness also shapes the terrain height (below). Biomes do not shape terrain otherwise and carry no vegetation;
scatter reads the voxel's biome and material (`SCATTER_SYSTEM.md`).

A world uses biomes when its `UVoxelWorldConfiguration::BiomeConfiguration` names a `UVoxelBiomeConfiguration` asset.
Without one, generation falls back to the static four-biome table `FVoxelBiomeRegistry` (Plains, Forest, Mountain,
Ocean; `VoxelBiomeRegistry.h`) with a fixed blend width of 0.15.

## Biome definition

`FBiomeDefinition` (`VoxelBiomeDefinition.h`):

| Field | Meaning |
|-------|---------|
| `BiomeID`, `Name` | the ID written into each voxel's `BiomeID` (the blend's leading biome) |
| `TemperatureRange`, `MoistureRange` | the biome's box in climate space, each in [-1, 1] |
| `ContinentalnessRange` | a gate: -1 deep ocean, 0 coastline, 1 continental interior (default the full range) |
| `SurfaceMaterial`, `SubsurfaceMaterial`, `DeepMaterial` | by depth under the terrain surface |
| `SurfaceDepth`, `SubsurfaceDepth` | the depths, in voxels, where the surface and subsurface layers end (1 and 4) |
| `UnderwaterSurfaceMaterial`, `UnderwaterSubsurfaceMaterial` | used where the column's terrain is under the sea |
| `BiomeOreVeins`, `bAddToGlobalOres` | the biome's ores, replacing or adding to the global list (`ORE_VEINS.md`) |
| `SelectionPriority` | weight boost in contested zones: 2^(priority x 0.2), so priority 10 counts 4x |

## The biome configuration

`UVoxelBiomeConfiguration` (`VoxelBiomeConfiguration.h`) holds:

- `Biomes`, and `BiomeBlendWidth` (0.15, in climate units) for the soft edges.
- The fields: `TemperatureNoiseFrequency` (0.00005), `MoistureNoiseFrequency` (0.00007),
  `ContinentalnessNoiseFrequency` (0.00002), each with a seed offset added to the world seed. They are 2-octave
  Simplex fields of the shared noise (`NOISE_LIBRARY.md`): on a heightmap world at (X, Y, 0), on a planet at the
  direction's point on the datum sphere (`FSphericalPlanetWorldMode::SampleClimate`).
- Continentalness (`bEnableContinentalness`): two curves, `ContinentalnessHeightCurve` (a height offset) and
  `ContinentalnessHeightScaleCurve` (a multiplier on the height scale), baked to 32 samples, which turn the field into
  oceans, coasts and inland heights. `ScaleContinentalnessHeights` rescales the offsets (used for planets).
- Height rules (`bEnableHeightMaterials`, `HeightMaterialRules`): `FHeightMaterialRule` puts a material between
  `MinHeight` and `MaxHeight` (world Z on a heightmap, altitude on a planet), at the surface only or down to
  `MaxDepthBelowSurface`, by `Priority` (snow on peaks, rock at altitude).
- Underwater materials (`bEnableUnderwaterMaterials`, `DefaultUnderwaterMaterial` sand).
- Ores (`bEnableOreVeins`, `GlobalOreVeins`).

`InitializeDefaults()` fills a new asset; `IsDataValid` checks it in the editor. `BuildGpuData()` bakes biomes, height
rules and ores into float4 buffers for the density shader (`BakedGpuBiomes` and the strides `VOXELGEN_*_FLOAT4_STRIDE`).

Worker threads and long-lived tasks (the map's tile drawing, the surface queries) read a plain-value copy,
`FVoxelBiomeSnapshot::FromConfig`, taken on the game thread; the configuration's own functions delegate to the
snapshot's static functions, so the two cannot drift.

## Selection and blend

`FVoxelBiomeSnapshot::ComputeBiomeBlend` (`UVoxelBiomeConfiguration::GetBiomeBlend`) weighs every biome at a
(temperature, moisture, continentalness) point:

1. **Continentalness gate.** A biome whose continentalness range lies further than the blend width away is excluded;
   inside the width a smoothstep fades it in.
2. **Climate weight.** The signed distance from the point to the biome's temperature-moisture box, smoothstepped over
   plus or minus the blend width.
3. **Priority.** The product of the two is multiplied by the priority boost.

Biomes with a weight over 0.001 are sorted by weight; the leading four (`FBiomeBlend`, `MAX_BIOME_BLEND`) are kept and
normalised to sum to 1. No candidate means the first biome. The voxel's `BiomeID` is the leading biome.

### The material in a blend zone

Where a voxel's blend has more than one biome and no weight over 0.9, the voxel takes one biome's material, chosen per
voxel with a probability equal to that biome's weight (`FVoxelBiomeSnapshot::PickBlendIndex`, the configuration's
`GetBlendedMaterial` and `GetBlendedMaterialWithWater`, the GPU's `GpuBlendedMaterial` in `BiomeMaterial.ush`). The
value that decides is the blend dither, `FVoxelBiomeBlendDither` (`VoxelBiomeBlendDither.h`; G39):

- **Key.** The voxel's integer position in voxels from the world's origin of space, round(WorldPos / VoxelSize), and
  the world seed. Generation counts it from the chunk's first voxel (`GridBase` plus the local index); the analytic
  readers (the surface query, the map) take `KeyAt` at the point they read, so they find the same voxel.
- **Value.** 3D value noise on a lattice 4 voxels apart: each lattice point's 16-bit integer hash (the ore `Rarity`
  hash's mix, on the lattice coordinates and the seed + 0x5D1A3), trilinear with smoothstep weights 0, 5, 16 and 27 of
  32, all in integers, then equalised through a fixed 257-entry table of the noise's distribution (`Cdf`), so it is
  uniform on [0, 1) and a biome covers its weight's share of the voxels. The CPU and the GPU compute the same multiple
  of 2^-24 (`Generation.GPUvsCPUBlendDitherParity`).
- **Order.** The walk over the blend's biomes goes in BiomeID order, not by weight, so a patch keeps its biome where
  the leading biome changes inside a zone.

Two biomes interleave in patches a few voxels across, their shares following the weights across the zone, with no
relation to the voxel grid (tests `VoxelWorlds.Biome.BlendDither.*`). The value used before G39 followed the voxel's
depth under the surface, which cycles with the grid on a slope, and drew bands of two materials along the grid;
`voxel.Biome.BlendDither 0` selects it, for A/B only (read when a chunk generates). A weight over 0.9 takes its biome's
material outright, so a zone's visible edge is where the leading weight crosses 0.9. The dither changed no generation
version: saves from before it load, their edits on top of the new materials.

## Material order in generation

Per solid voxel, on the CPU (`FVoxelCPUNoiseGenerator`) and the GPU (`BiomeMaterial.ush`) alike:

1. The blend from the column's fields (before the caves, which read the leading biome's cave overrides,
   `CAVE_SYSTEM.md`).
2. The blended material by depth: `GetMaterialAtDepth` of the picked biome, or its underwater variant where the
   column's terrain is under the sea.
3. Height rules.
4. The cave-wall override, then ore veins in the leading biome's list.

See [MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) for how material IDs are drawn and
[SCATTER_SYSTEM.md](SCATTER_SYSTEM.md) for vegetation placement.
