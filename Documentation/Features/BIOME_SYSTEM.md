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
Ocean; `VoxelBiomeRegistry.h`) with a fixed blend width of 0.15. The table is built once, by the first query on any
thread (a function-local static, which C++ initialises on one thread while the others wait); before G83 it was built
lazily with no lock, and the first queries of several map tile tasks at once crashed a run of `VoxelWorlds.Map` alone.

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
- Climate shaping (below): `bEqualizeClimate`, `TemperatureLatitudeWeight`, `TemperatureLapseRate`, and
  `bEqualizeContinentalness` beside the continentalness settings. All off by default.

`InitializeDefaults()` fills a new asset; `IsDataValid` checks it in the editor. `BuildGpuData()` bakes biomes, height
rules and ores into float4 buffers for the density shader (`BakedGpuBiomes` and the strides `VOXELGEN_*_FLOAT4_STRIDE`).

Worker threads and long-lived tasks (the map's tile drawing, the surface queries) read a plain-value copy,
`FVoxelBiomeSnapshot::FromConfig`, taken on the game thread; the configuration's own functions delegate to the
snapshot's static functions, so the two cannot drift.

## Climate shaping

The blend reads the climate after four optional steps, each off by default, so a world changes only when its
configuration turns one on (none is in the save fingerprint, which leaves biome tuning out by design). Every reader
shapes the climate the same way:
- **CPU:** `FVoxelBiomeSnapshot::ShapeTemperature`, `ShapeMoisture` and `ShapeContinentalness`. This covers generation,
  the surface queries, the far field, the map, PCG and the editor's field viewer.
- **GPU:** `Shaders/Private/ClimateShaping.ush`, the same arithmetic.

`VoxelWorlds.Generation.GPUvsCPUClimateShapingParity` holds the two equal on the plane and on a planet.

1. **Equalisation** (`bEqualizeClimate` for temperature and moisture, `bEqualizeContinentalness` for continentalness).
   - The raw 2-octave fields cluster near 0: about half of a field's samples lie within 0.2 of it.
   - Equalising passes the raw value through the fields' measured CDF, so it is uniform on [-1, 1]. A box then covers
     its share of the world: a box 0.5 wide on one axis holds a quarter of it.
   - The CDF is `VOXELGEN_CLIMATE_CDF`: 129 points over [-1, 1], linear between them, in
     `Shaders/Shared/VoxelGenerationShared.ush`, shared by C++ and HLSL.
   - `VoxelWorlds.Biome.ClimateEqualize.Table` measures it against the field and prints a new table when it drifts.
     `.Uniform` checks that each tenth of the equalised range holds a tenth of the samples.
   - Equalised continentalness feeds both the height curves and the biome gate, so a curve key at c sits at the
     (c + 1) / 2 quantile.
2. **Latitude** (`TemperatureLatitudeWeight`, planets only).
   - Temperature leans toward 1 - 2 |sin(latitude)|: 1 at the equator, -1 at the poles.
   - That ramp is linear in sin(latitude), so it is uniform by area, like an equalised field.
   - The weight runs from 0 (the field alone) to 1 (latitude alone). Latitude is measured from the +Z pole through the
     planet's centre, as `FVoxelCubeSphere::DirectionToLatLon` does.
3. **Lapse** (`TemperatureLapseRate`).
   - Temperature drops by the rate for every 10,000 uu (100 m) of the column's terrain above the datum, so high ground
     turns to the colder biomes.
   - The datum is world Z 0 on a heightmap and the datum radius on a planet, as the height rules measure it. Ground
     below the datum is not warmed.
   - The height used is the one the reader has: generation's, with conditioning zones applied.
4. The blend clamps all three axes to [-1, 1], as before.

### Measuring a world's biomes

- `voxel.World.BiomeCoverage [samples] [radius]` samples the natural surface (the analytic height and the shaped
  climate) over the world and logs the land's share and each biome's share of the land. For each biome it also logs a
  site: its flattest land sample at least 2 m above the sea.
- `voxel.World.HeightProfile x0 y0 x1 y1 [points]` logs the natural surface along a line. It is how a continentalness
  curve's cliffs and notches show up before any chunk is meshed.

### The Smooth Terrain showcase (2026-11-06)

The parent repository's showcase worlds turn all four steps on: `SmoothTerrain_Island` (IslandBowl, an 8 km island,
300 m relief), `SmoothTerrain_Planet` (a 50 km planet, ridged terrain, latitude weight 0.4) and `CubicTerrain_Plane`
(an InfinitePlane block world).
- They share eleven biomes: Coast, Meadow, Temperate Forest, Taiga, Tundra, Snowfield, Marsh, Savanna, Desert, Alpine
  and Ashlands. Their climate cells are 2-15 km across, against the demo's few hundred metres.
- One table holds every world: `Tools/SmoothTerrain/smooth_terrain.json`, built by `Scripts/setup_smooth_terrain.py`.
  The table holds the biomes, each world's overrides of their boxes, the continentalness curves and the height rules.

`voxel.World.BiomeCoverage` measured:

| World | Land | Biomes (share of the land) |
|---|---|---|
| Island | 45% | each biome 3-16% |
| Planet | 42% | Taiga 22, Meadow 18, Alpine 15, Forest 13, Coast 8.5, Tundra 8, Savanna 7, Marsh 4, Snowfield 2, Desert 1, Ashlands 1 |
| Block world | 38.9% | every biome present |

On the planet, latitude and the lapse rate cool most of the land, so the hot biomes (Desert, Ashlands) are small.

**Overlay biomes.** Coast, Marsh, Alpine and Ashlands are drawn over other biomes' boxes, gated by continentalness
(the shore, the peaks) or by a band of temperature.
- An overlay takes its material outright only where its share of the blend passes 0.9. Against a base biome of equal
  raw weight, that needs a priority lead of about 16: 2^(16 x 0.2) is about 9.2.
- With a smaller lead the whole overlap dithers between the two materials.
- The showcase gives the overlays 20-24 and the base biomes 0-3.

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
depth under the surface, which cycles with the grid on a slope, and drew bands of two materials along the grid (the
GPU shader kept that pick behind a parameter pinned to 1 until G97 removed it). A weight over 0.9 takes its biome's
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
