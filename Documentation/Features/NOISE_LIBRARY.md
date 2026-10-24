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
// Core noise algorithms
static float Perlin3D(const FVector& Position, int32 Seed = 0);
static float Simplex3D(const FVector& Position, int32 Seed = 0);
static void Cellular3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2);
static void Voronoi3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2, float& OutCellID);

// fBm with automatic noise type selection
static float FBM3D(const FVector& Position, const FVoxelNoiseParams& Params);
```

## GPU/CPU Consistency

The GPU and CPU implementations use the same algorithms and permutation table. Expected consistency:
- Perlin/Simplex: tolerance ~0.001 (floating point differences)
- Cellular/Voronoi: tolerance ~0.001 (same 3x3x3 search, same hash)

To verify consistency, generate the same chunk with `bUseGPUGeneration` toggled and compare voxel data.

## Planet Noise (spherical Phase 6)

Planets do not use the permutation noise above. A 1,000 km planet would repeat its fine octaves every 256 noise units
(the top terrain octave 61 times around `Review_Planet_L`'s equator), and an absolute float position of size 10^8 is
8 uu coarse. Every planet field samples `FVoxelPlanetNoise` (`VoxelGeneration/VoxelPlanetNoise.h`; GPU:
`Shaders/Private/PlanetNoise.ush`, the same float algebra):

- **Hashed lattice:** the gradient or feature point of a lattice cell comes from an integer hash of the cell
  (`LatticeHash`, the ore `Rarity` hash's mixing), cells wrapping mod 2^32: no period
  (`VoxelWorlds.Noise.PlanetHashNoPeriod`).
- **Split-origin sampling:** a sample point is an origin plus a small float offset. Per octave the origin is scaled by
  the octave's frequency in double and split into an integer cell (`FVoxelNoiseSplit::Cell`) and a float fraction;
  simplex splits in its skewed lattice, the others in the cube lattice. The CPU splits each sample point itself
  (`FBM3D(Position, Params)`); the GPU gets one split origin per chunk and field (`PlanetNoiseOrigins`) and adds the
  voxel's offset from the chunk's anchor (`PlanetFBM3D`). Within 10^-5 of a double reference at origins up to 10^9
  noise units (`VoxelWorlds.Noise.SplitOrigin`).
- **Continuous Simplex (G43):** the classic corner weight (0.6 − d²)⁴ reaches past the simplex (a point's nearest corner
  outside its simplex is √0.5 away), so a corner's contribution dropped out where a sample crossed into the next simplex:
  steps of up to 0.0064 of the noise's range, every octave's at once on the planes x = y, y = z and x = z through the
  planet centre, the cube sphere's face edges (up to 465 uu across them on `Review_Planet_L`'s terrain). Planets use that
  weight less its value and slope at d² = 0.5, v²(v² + 0.4v + 0.06) with v = 0.5 − d², and scale the sum by 33.25 instead
  of 32 for the same spread: no step, a continuous slope, within 1.2% rms of the classic noise (correlation 0.9996)
  (`VoxelWorlds.Noise.PlanetSimplexContinuous`, `VoxelWorlds.WorldModes.Planet.FaceEdgeContinuity`).
  `voxel.Planet.ContinuousSimplex 0` restores the classic weight, for A/B only.
- **Fields:** terrain and continentalness at the direction's point on the datum sphere, temperature and moisture likewise
  (the heightmap's 2-octave fields: `MakeFieldParams`, `SampleClimate`), caves at the planet-relative position with the
  altitude scaled (`ScaleAltitudeRelative`), ores at the planet-relative position with one streak direction per ore.

Heightmap worlds keep the permutation noise bit for bit, the classic Simplex weight included (their steps are about a
quarter voxel; decision D27). Changing the planet noise changes every planet, so it bumps
`FVoxelWorldModeFactory::PlanetGenerationVersion` (5 since D21/D22, 6 since G43/D27).

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
