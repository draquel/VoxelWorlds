# Noise Library Reference

## Overview

The VoxelWorlds noise library provides four noise algorithms with GPU and CPU implementations, combined via fBm (Fractal Brownian Motion) for terrain generation, cave carving, biome selection, and ore vein placement.

## Noise Types

### Perlin (`EVoxelNoiseType::Perlin`)

Classic gradient noise by Ken Perlin. Smooth, continuous, slightly grid-aligned artifacts.

- **Range:** [-1, 1]
- **Performance:** Baseline
- **Best for:** General terrain, subsurface features
- **Artifacts:** Slight axis-alignment at low frequencies

### Simplex (`EVoxelNoiseType::Simplex`)

Improved noise using simplex grid. Less directional artifacts than Perlin, slightly faster.

- **Range:** [-1, 1]
- **Performance:** ~0.9x Perlin (faster)
- **Best for:** Default choice for terrain, biome noise, cave noise
- **Artifacts:** Minimal

### Cellular (`EVoxelNoiseType::Cellular`)

Worley/Cell noise. Returns distance to nearest feature point (F1).

- **Range:** [-1, 1] (mapped from F1 distance [0, ~1.5])
- **Performance:** ~1.5-2x Perlin (3x3x3 cell search)
- **Best for:** Organic patterns, rocky textures, cave shapes
- **Characteristics:** Produces cell-like, bubbly patterns

### Voronoi (`EVoxelNoiseType::Voronoi`)

Cell noise returning edge distance (F2 - F1). Highlights boundaries between cells.

- **Range:** [-1, 1] (mapped from F2-F1 [0, ~1])
- **Performance:** ~1.8x Perlin (3x3x3 cell search + cell ID)
- **Best for:** Cracked/fractured patterns, canyon-like features
- **Characteristics:** Pronounced cell edges, flat cell interiors

## fBm Composition

All noise types are combined via Fractal Brownian Motion (fBm):

```
Total = 0
for each octave i:
    Total += NoiseAt(Position * Frequency) * Amplitude
    Frequency *= Lacunarity
    Amplitude *= Persistence
Result = Total / MaxAmplitude
```

### Parameters

| Parameter | Typical Range | Effect |
|-----------|---------------|--------|
| `Frequency` | 0.0001 - 0.1 | Scale of features (lower = larger) |
| `Octaves` | 1 - 8 | Detail layers (more = finer detail) |
| `Persistence` | 0.3 - 0.7 | How much detail each octave adds |
| `Lacunarity` | 1.5 - 3.0 | Frequency jump between octaves |
| `Amplitude` | 0.5 - 2.0 | Overall noise strength |
| `Seed` | any int32 | Deterministic randomization |

### Ridged fBm

Available on GPU as `RidgedFBM3D()`. Applies `1 - |noise|` per octave for sharp ridge features (mountain peaks, ridgelines).

## GPU Implementation

### Files

| File | Contents |
|------|----------|
| `VoxelNoiseCommon.ush` | Hash functions, permutation table, utilities |
| `PerlinNoise.ush` | `Perlin3D(float3, int)` |
| `SimplexNoise.ush` | `Simplex3D(float3, int)` |
| `CellularNoise.ush` | `Cellular3D(float3, int)`, `Voronoi3D(float3, int)` |
| `FBMNoise.ush` | `FBM3D()`, `FBMSimplex()`, `FBMPerlin()`, `FBMCellular()`, `FBMVoronoi()`, `RidgedFBM3D()` |

### Noise Type Defines

```hlsl
#define NOISE_TYPE_PERLIN 0
#define NOISE_TYPE_SIMPLEX 1
#define NOISE_TYPE_CELLULAR 2
#define NOISE_TYPE_VORONOI 3
```

Must match `EVoxelNoiseType` enum values.

### GPU Functions

```hlsl
// Core fBm with noise type selection
float FBM3D(float3 Position, int NoiseType, int Seed, int Octaves,
            float Frequency, float Amplitude, float Lacunarity, float Persistence)

// Convenience wrappers
float FBMSimplex(float3 Position, int Seed, int Octaves, float Frequency, float Persistence)
float FBMPerlin(float3 Position, int Seed, int Octaves, float Frequency, float Persistence)
float FBMCellular(float3 Position, int Seed, int Octaves, float Frequency, float Persistence)
float FBMVoronoi(float3 Position, int Seed, int Octaves, float Frequency, float Persistence)

// Raw Cellular/Voronoi
float2 Cellular3D(float3 Position, int Seed)  // Returns (F1, F2)
float3 Voronoi3D(float3 Position, int Seed)   // Returns (F1, F2, CellID)
```

## CPU Implementation

### Files

| File | Contents |
|------|----------|
| `VoxelCPUNoiseGenerator.h` | Public API |
| `VoxelCPUNoiseGenerator.cpp` | All implementations |

### CPU Functions

```cpp
// fBm with automatic noise type selection: the entry point every field samples. Since the world modes review's MP4
// (decision D39) it is FVoxelPlanetNoise::FBM3D in every world mode (the section below).
static float FBM3D(const FVector& Position, const FVoxelNoiseParams& Params);

// The classic table noise (Ken Perlin's 256-entry permutation, the 0.6 Simplex kernel): what heightmap worlds sampled
// before MP4. No generation path reads it; the tests keep it as the control that repeats every 256 lattice units.
static float Perlin3D(const FVector& Position, int32 Seed = 0);
static float Simplex3D(const FVector& Position, int32 Seed = 0);
static void Cellular3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2);
static void Voronoi3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2, float& OutCellID);
static float ClassicFBM3D(const FVector& Position, const FVoxelNoiseParams& Params);
```

## GPU/CPU Consistency

The GPU and the CPU sample the same noise with the same float operations in the same order (`FVoxelPlanetNoise` and
`PlanetNoise.ush`, below). The CPU splits each sample point itself; the GPU splits a per-chunk origin and adds each
voxel's offset, so the two differ by float rounding only: the `VoxelWorlds.Generation.GPUvsCPU*Parity` tests hold every
world mode's densities within 2 steps on 99% of the voxels and their materials, biomes and flags equal (the planet lines
were within a few voxels since spherical Phase 6; the heightmap lines, bit-identical before MP4, are now of the same kind).

The classic shader functions (`PerlinNoise.ush`, `SimplexNoise.ush`, `CellularNoise.ush`, `FBMNoise.ush`) are no longer
called by generation; `FBMNoise.ush` is still included for its `NOISE_TYPE_*` defines.

## The Terrain Noise (spherical Phase 6; every world mode since MP4)

The permutation noise above repeats its fine octaves every 256 noise units (the top terrain octave 61 times around
`Review_Planet_L`'s equator, the continental frequency every ~120 km on a plane), and an absolute float position of size
10^8 is 8 uu coarse. Every field of every world mode samples `FVoxelPlanetNoise` (`VoxelGeneration/VoxelPlanetNoise.h`;
GPU: `Shaders/Private/PlanetNoise.ush`, the same float algebra; the name is from spherical Phase 6, when planets alone
had it; heightmap worlds came through `FVoxelCPUNoiseGenerator::FBM3D` to it in the world modes review's MP4, decision
D39, `GenerationVersion` 3):

- **Hashed lattice:** the gradient or feature point of a lattice cell comes from an integer hash of the cell
  (`LatticeHash`, the ore `Rarity` hash's mixing), cells wrapping mod 2^32: no period
  (`VoxelWorlds.Noise.PlanetHashNoPeriod`).
- **Split-origin sampling:** a sample point is an origin plus a small float offset. Per octave the origin is scaled by
  the octave's frequency in double and split into an integer cell (`FVoxelNoiseSplit::Cell`) and a float fraction;
  simplex splits in its skewed lattice, the others in the cube lattice. The CPU splits each sample point itself
  (`FBM3D(Position, Params)`); the GPU gets one split origin per chunk and field (`NoiseOrigins`) and adds the
  voxel's offset from the chunk's anchor (`PlanetFBM3D`): a planet's anchor is the chunk's centre on the datum sphere,
  a heightmap chunk's its origin corner (the 2D fields at (X, Y, 0), each cave layer at (X, Y, Z × VerticalScale), each
  ore at the corner through its streak map; `BuildNoiseOrigins` in `VoxelGPUNoiseGenerator.cpp`). Within 10^-5 of a
  double reference at origins up to 10^9 noise units (`VoxelWorlds.Noise.SplitOrigin`, `VoxelWorlds.Noise.HeightmapNoise`).
- **Continuous Simplex (G43):** the classic corner weight (0.6 − d²)⁴ reaches past the simplex (a point's nearest corner
  outside its simplex is √0.5 away), so a corner's contribution dropped out where a sample crossed into the next simplex:
  steps of up to 0.0064 of the noise's range, every octave's at once on the planes x = y, y = z and x = z through the
  planet centre, the cube sphere's face edges (up to 465 uu across them on `Review_Planet_L`'s terrain; about a quarter
  voxel along X = Y on a heightmap world). The kernel is now that weight less its value and slope at d² = 0.5,
  v²(v² + 0.4v + 0.06) with v = 0.5 − d², and the sum is scaled by 33.25 instead of 32 for the same spread: no step, a
  continuous slope, within 1.2% rms of the classic noise (correlation 0.9996) (`VoxelWorlds.Noise.PlanetSimplexContinuous`,
  `VoxelWorlds.WorldModes.Planet.FaceEdgeContinuity`, `VoxelWorlds.WorldModes.Plane.NoiseCreases`).
  `voxel.Noise.ContinuousSimplex 0` (`voxel.Planet.ContinuousSimplex` before MP4) restores the classic weight, for A/B only.
- **Fields:** on a planet terrain and continentalness at the direction's point on the datum sphere, temperature and
  moisture likewise (the 2-octave fields: `MakeFieldParams`, `SampleClimate`), caves at the planet-relative position with
  the altitude scaled (`ScaleAltitudeRelative`), ores at the planet-relative position; on a heightmap world the 2D fields
  at (X, Y, 0), caves at the position with Z scaled, ores at the position. Ores in every mode squash the position along
  one direction per ore type for a streak (`GetPlanetOreSamplePoint`, a linear map) and pass their `Rarity` test by an
  integer hash of the position rounded to whole uu (`OreRarityHash`; the planet's since spherical Phase 4, the heightmap
  modes' since MP4, whose `frac(sin(p · k))` the GPU's float and the CPU's double rounded differently).

Changing this noise changes every world: planets bump `FVoxelWorldModeFactory::PlanetGenerationVersion` (5 since
D21/D22, 6 since G43/D27), and MP4's move of the heightmap worlds onto it bumped `GenerationVersion` 2 → 3 (every save from
before is refused and kept on disk).

## Usage Examples

### Terrain Height (existing)
```cpp
FVoxelNoiseParams Params;
Params.NoiseType = EVoxelNoiseType::Simplex;
Params.Frequency = 0.001f;
Params.Octaves = 6;
float Height = FVoxelCPUNoiseGenerator::FBM3D(Position, Params);
```

### Cave Carving (new)
```cpp
// Cheese cave: single threshold
FVoxelNoiseParams CaveNoise;
CaveNoise.NoiseType = EVoxelNoiseType::Simplex;
CaveNoise.Frequency = 0.003f;
CaveNoise.Octaves = 3;
float Noise = FVoxelCPUNoiseGenerator::FBM3D(ScaledPos, CaveNoise);
if (Noise > Threshold) { /* carve */ }
```

### Cellular Terrain (experimental)
```cpp
FVoxelNoiseParams CellParams;
CellParams.NoiseType = EVoxelNoiseType::Cellular;
CellParams.Frequency = 0.002f;
CellParams.Octaves = 4;
// Produces bubble/cell-like terrain patterns
float CellNoise = FVoxelCPUNoiseGenerator::FBM3D(Position, CellParams);
```

## Performance Comparison

Approximate relative cost per 32^3 chunk (CPU, 6 octaves):

| Noise Type | Relative Cost |
|------------|---------------|
| Simplex | 1.0x (baseline) |
| Perlin | 1.1x |
| Cellular | 1.8x |
| Voronoi | 1.9x |

GPU performance is dominated by memory bandwidth rather than ALU, so noise type differences are smaller (~1.3x for Cellular vs Simplex).
