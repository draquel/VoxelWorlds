# Noise Library Reference

> State as of 2026-11-02 (code review CR5).

## Overview

One noise serves every field of every world mode (terrain, continentalness, temperature, moisture, caves, ores):
`FVoxelNoise` on the CPU (`Source/VoxelGeneration/Public/VoxelNoise.h`) and `NoiseFBM3D` on the GPU
(`Shaders/Private/VoxelNoise.ush`), the same float algebra in the same order. It has four noise types, combined by
fBm, on a hashed integer lattice sampled from a split origin, so it has no period and keeps its precision at any
distance from the world origin. Planets have sampled it since spherical Phase 6; heightmap worlds (InfinitePlane,
IslandBowl, cubic on either) since the world modes review's MP4 (decision D39); CR3 renamed it from its planet
name.

The classic table noise heightmap worlds sampled before MP4 (Ken Perlin's 256-entry permutation, which repeats every
256 noise units, at absolute float positions) survives only as the tests' control,
`Source/VoxelGeneration/Tests/VoxelClassicNoiseControl.h` (`ClassicFBM3D`).

## Noise Types (`EVoxelNoiseType`)

| Type | What it is | Output | Use |
|------|-------|-----------------|-----|
| `Perlin` | gradient noise | [-1, 1] | general terrain |
| `Simplex` (default) | gradient noise on the simplex lattice, the continuous kernel (below) | [-1, 1] | terrain, climate, caves |
| `Cellular` | Worley F1: distance to the nearest feature point | F1 x 2 - 1 | bubbly, cell-like shapes |
| `Voronoi` | the cell edge distance F2 - F1 | (F2 - F1) x 2 - 1 | cracked, fractured shapes, flat cell interiors |

Cellular and Voronoi share one 3x3x3 feature-point search (`FVoxelNoise::Cellular`, `NoiseCellular`). The type
integers are `VOXELGEN_NOISE_TYPE_*` in `Shaders/Shared/VoxelGenerationShared.ush`, which C++ and the shaders share.

## fBm Composition

```
Total = 0; MaxValue = 0; Amplitude = 1; Frequency = Params.Frequency
for each octave i (Octaves, at most 16):
    Total    += Sample(Position * Frequency) * Amplitude
    MaxValue += Amplitude
    Frequency *= Lacunarity
    Amplitude *= Persistence
Result = MaxValue > 0 ? Total / MaxValue : 0
```

The result is normalised: about [-1, 1] whatever the octaves and the persistence, and callers scale it themselves (the
terrain by `HeightScale`, caves and ores against their thresholds). Until G88 the parameters had an `Amplitude`, the first
octave's weight, which divided out and did nothing; at 0 it gave 0 / 0. It went, and the sum of the weights is now at
least 1 unless `Persistence` is negative, the guard's only case (`VoxelWorlds.Noise.FBMNormalised`).

### Parameters (`FVoxelNoiseParams`)

| Parameter | Default | Effect |
|-----------|---------|--------|
| `NoiseType` | `Simplex` | the noise type |
| `Seed` | 0 | 0 = the world seed |
| `Octaves` | 6 | detail layers (1-16) |
| `Frequency` | 0.001 | scale of features in 1/uu (lower = larger) |
| `Lacunarity` | 2.0 | frequency step between octaves |
| `Persistence` | 0.5 | amplitude step between octaves |

`FVoxelNoise::MakeFieldParams` builds the 2-octave fields that callers make themselves (climate, continentalness).

## How it samples

- **Hashed lattice.** A lattice cell's gradient or feature point comes from an integer hash of the cell
  (`FVoxelNoise::LatticeHash`, `VoxelHash::Lattice`; GPU `VoxelLatticeHash` in `Shaders/Shared/VoxelHash.ush`, the mix
  the ore `Rarity` test shares), cells wrapping mod 2^32: no period (`VoxelWorlds.Noise.PlanetHashNoPeriod`).
- **Split-origin sampling.** A sample point is an origin plus a small float offset. Per octave the origin is scaled by
  the octave's frequency in double and split into an integer cell and a float fraction (`FVoxelNoiseSplit`; simplex in
  its skewed lattice, the others in the cube lattice). The CPU splits each sample point itself
  (`FVoxelNoise::FBM3D(Position, Params)`); the GPU gets one split origin per chunk and field (`NoiseOrigins`, built by
  `BuildNoiseOrigins` in `VoxelGPUNoiseGenerator.cpp`) and adds the voxel's offset from the chunk's anchor: a planet's
  anchor is the chunk's centre on the datum sphere, a heightmap chunk's its origin corner (the 2D fields at (X, Y, 0),
  each cave layer at (X, Y, Z x VerticalScale), each ore at the corner through its streak map). Within 10^-5 of a
  double reference (`FVoxelNoise::ReferenceFBM3D`) at origins up to 10^9 noise units (`VoxelWorlds.Noise.SplitOrigin`,
  `VoxelWorlds.Noise.HeightmapNoise`).
- **Continuous Simplex (G43).** The classic corner weight (0.6 - d^2)^4 reaches past the simplex, so a corner's
  contribution dropped out where a sample crossed into the next simplex: steps of up to 0.0064 of the noise's range,
  every octave's at once on the planes x = y, y = z and x = z (on a planet the cube sphere's face edges, hundreds of uu
  on a 1,000 km planet; about a quarter voxel along X = Y on a heightmap world). The kernel is that weight less its value
  and slope at d^2 = 0.5, v^2 (v^2 + 0.4 v + 0.06) with v = 0.5 - d^2, the sum scaled by 33.25 instead of 32 for the
  same spread: no step, a continuous slope, within 1.2% rms of the classic noise (correlation 0.9996)
  (`VoxelWorlds.Noise.SimplexContinuous`, `VoxelWorlds.WorldModes.Planet.FaceEdgeContinuity`,
  `VoxelWorlds.WorldModes.Plane.NoiseCreases`). The classic kernel is the tests' control
  (`VoxelClassicNoiseControl::HashedSimplexClassicKernel`); the GPU shader has the continuous kernel alone since G97
  removed its classic leg.

### Where each field samples

- **Planet**: terrain, continentalness, temperature and moisture at the direction's point on the datum sphere; caves at
  the planet-relative position with the altitude scaled (`ScaleAltitudeRelative`); ores at the planet-relative position.
- **Heightmap**: the 2D fields at (X, Y, 0); caves at the position with Z scaled; ores at the position.
- **Ores, every mode**: the position squashed along one direction per ore type for a streak (`GetPlanetOreSamplePoint`,
  a linear map), and the `Rarity` test an integer hash of the position rounded to whole uu (`OreRarityHash`), so the
  GPU's float and the CPU's double agree (`ORE_VEINS.md`).

## GPU/CPU Consistency

The CPU splits each sample point itself; the GPU splits a per-chunk origin and adds each voxel's offset, so the two
differ by float rounding only. The `VoxelWorlds.Generation.GPUvsCPU*Parity` tests hold every world mode's densities
within 2 steps on 99% of the voxels with materials, biomes and flags equal (`GPU_PIPELINE.md`).

## Versions

Changing this noise changes every world's terrain, so it bumps a generation version (`DATA_STRUCTURES.md`, "Version
fingerprints"): `PlanetGenerationVersion` 5 for the planet noise (D21, D22) and 6 for the continuous kernel (G43, D27);
`GenerationVersion` 3 for MP4's move of the heightmap worlds onto it. Every save from before is refused and kept on
disk.

## Usage

```cpp
// Terrain-style field
FVoxelNoiseParams Params;
Params.NoiseType = EVoxelNoiseType::Simplex;
Params.Frequency = 0.001f;
Params.Octaves = 6;
float Value = FVoxelNoise::FBM3D(Position, Params);   // FVoxelCPUNoiseGenerator::FBM3D forwards here

// Cave-style threshold
FVoxelNoiseParams CaveNoise;
CaveNoise.Frequency = 0.003f;
CaveNoise.Octaves = 3;
if (FVoxelNoise::FBM3D(ScaledPos, CaveNoise) > Threshold) { /* carve */ }

// Large origin plus a small offset (what the GPU does per chunk)
float Near = FVoxelNoise::FBM3D(ChunkAnchor, FVector3f(VoxelOffset), Params);
```
