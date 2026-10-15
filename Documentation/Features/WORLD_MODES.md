# World Modes

**Module**: VoxelGeneration
**Dependencies**: VoxelCore
**Last Updated**: 2026-10-14 (island sections from the world modes review, WORLD_MODES_REVIEW_PLAN.md §13)

## Overview

The world mode system supports three terrain types, each optimized for different use cases. All modes use the same voxel generation and meshing pipelines but with mode-specific density calculations and LOD culling.

## IVoxelWorldMode Interface

```cpp
class IVoxelWorldMode {
public:
    virtual ~IVoxelWorldMode() = default;

    // Core density evaluation
    virtual float GetDensityAt(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams) const = 0;

    // Material and biome selection
    virtual uint8 GetMaterialAt(const FVector& WorldPos, float Density) const = 0;
    virtual uint8 GetBiomeAt(const FVector& WorldPos) const = 0;

    // Static helper for CPU/GPU consistency
    static float ApplyWorldModeSDF(float BaseDensity, const FVector& WorldPos, const FWorldModeParams& Params);
};
```

---

## Mode 1: Infinite Plane (Default)

### Overview
Standard heightmap-style terrain extending infinitely in X and Y directions. The most common mode for traditional voxel games.

### Configuration (UVoxelWorldConfiguration)
```cpp
// World Settings
EWorldMode WorldMode = EWorldMode::InfinitePlane;

// Terrain Generation
float SeaLevel = 0.0f;           // Base elevation
float HeightScale = 5000.0f;     // Noise amplitude
float BaseHeight = 0.0f;         // Additional offset
```

### Implementation (FInfinitePlaneWorldMode)
```cpp
float FInfinitePlaneWorldMode::GetDensityAt(const FVector& WorldPos,
                                             const FVoxelNoiseParams& NoiseParams) const
{
    // Sample 2D heightmap noise
    float HeightNoise = SampleFBM2D(WorldPos.X, WorldPos.Y, NoiseParams);

    // Calculate terrain height at this XY position
    float TerrainHeight = SeaLevel + BaseHeight + (HeightNoise * HeightScale);

    // SDF: positive below surface, negative above
    return TerrainHeight - WorldPos.Z;
}
```

### LOD Culling
- **Terrain Bounds Culling**: Chunks above `TerrainMaxHeight` or below `TerrainMinHeight` are skipped
- Height range: `SeaLevel + BaseHeight ± HeightScale` with buffer for noise variation

### Use Cases
- Traditional Minecraft-style worlds
- Large-scale terrain exploration
- Any game requiring unlimited horizontal exploration

---

## Mode 2: Island/Bowl

### Overview
Bounded terrain with a falloff at the edges, for a contained world: an island in the sea, or a landmass ringed by a
low floor. Reviewed as a "large but finite world" with every game system on in 2026-10 (WORLD_MODES_REVIEW_PLAN.md §5,
§13). The terrain itself is correct; the gaps are around it (no world edge, POI placement, streaming cost), each with a
work item in that plan's §14 (milestones M2-M4 and M6). See "Measured" and "Known gaps" below.

### Configuration (UVoxelWorldConfiguration)
```cpp
// World Settings
EWorldMode WorldMode = EWorldMode::IslandBowl;

// Island Shape
int32 IslandShape = 0;               // 0=Circular, 1=Rectangle

// Island Parameters
float IslandRadius = 50000.0f;       // Radius (circular) or SizeX (rectangle)
float IslandSizeY = 50000.0f;        // SizeY (rectangle only, ignored for circular)
float IslandFalloffWidth = 10000.0f; // Width of transition zone
int32 IslandFalloffType = 1;         // 0=Linear, 1=Smooth, 2=Squared, 3=Exponential
float IslandCenterX = 0.0f;          // Island centre (see the note below)
float IslandCenterY = 0.0f;
float IslandEdgeHeight = -1000.0f;   // Terrain height the falloff reaches at R + FalloffWidth
```

**Island centre:** generation and the analytic height read `IslandCenterX/Y` as absolute world coordinates; the LOD
cull reads them as an offset from `WorldOrigin`. They agree only when `WorldOrigin` is 0 in X and Y, so keep it there
for island worlds until WM-A1 gives every consumer one params builder (finding KI3: with the demo's origin the cull sat
55,706 uu off centre and cut 1,098 land columns). `bIslandBowlShape` was removed on 2026-10-14: it was copied in four
places and read nowhere (KI4).

### Island Shape Types
```cpp
enum class EIslandShape : uint8 {
    Circular = 0,    // Radial distance from center (default)
    Rectangle = 1    // Axis-aligned box with separate X/Y sizes
};
```

### Falloff Types
```cpp
enum class EIslandFalloffType : uint8 {
    Linear = 0,      // Simple linear fade
    Smooth = 1,      // Hermite interpolation (default, natural-looking)
    Squared = 2,     // Faster drop near edge
    Exponential = 3  // Steep near the island, flattening toward the edge
};
```

Every curve runs from 1 at `IslandRadius` to exactly 0 at the edge (`IslandRadius + FalloffWidth`), so the terrain
meets `EdgeHeight` there. Exponential is `(e^-3t - e^-3) / (1 - e^-3)`; before 2026-10-14 it was `e^-3t`, which ended at
about 0.05 and left a step of 5% of the relief at the edge (KI4).

### Implementation (FIslandBowlWorldMode)

For **Circular** islands, falloff is based on Euclidean distance from center:
```cpp
float Distance = FMath::Sqrt(DX * DX + DY * DY);
float T = (Distance - IslandRadius) / FalloffWidth;  // 0 at edge, 1 at falloff end
```

For **Rectangle** islands, falloff is based on the maximum normalized axis distance:
```cpp
// Calculate how far into falloff zone on each axis
float TX = FMath::Max(0.0f, (FMath::Abs(DX) - IslandRadius) / FalloffWidth);
float TY = FMath::Max(0.0f, (FMath::Abs(DY) - SizeY) / FalloffWidth);
float T = FMath::Max(TX, TY);  // Use the axis furthest into falloff
```

```cpp
float FIslandBowlWorldMode::CalculateFalloffFactorForPoint(
    float X, float Y,
    const FIslandBowlParams& IslandParams)
{
    const float DX = FMath::Abs(X - IslandParams.CenterX);
    const float DY = FMath::Abs(Y - IslandParams.CenterY);

    float T = 0.0f;
    if (IslandParams.Shape == EIslandShape::Rectangle)
    {
        // Rectangle: use max of normalized axis distances
        float TX = FMath::Max(0.0f, (DX - IslandParams.IslandRadius) / IslandParams.FalloffWidth);
        float TY = FMath::Max(0.0f, (DY - IslandParams.SizeY) / IslandParams.FalloffWidth);
        T = FMath::Clamp(FMath::Max(TX, TY), 0.0f, 1.0f);
    }
    else
    {
        // Circular: use Euclidean distance
        float Distance = FMath::Sqrt(DX * DX + DY * DY);
        T = FMath::Clamp((Distance - IslandParams.IslandRadius) / IslandParams.FalloffWidth, 0.0f, 1.0f);
    }

    // Apply falloff curve (Linear, Smooth, Squared, or Exponential)
    return ApplyFalloffCurve(T, IslandParams.FalloffType);
}
```

### Beyond the edge
Past `R + FalloffWidth` generation writes a flat floor at `EdgeHeight - 1000` (`FIslandBowlWorldMode::OutsideFloorDepth`),
so the terrain drops 1,000 uu at the edge in a vertical wall. The cull keeps a ring of chunks out to one chunk diagonal past
the edge (6,788 uu with the demo's 4,800 uu chunks), so the floor and the water plane exist only in that ring; past it there
is nothing: no terrain, water or collision. A pawn that swims or walks off the ring falls until KillZ destroys it, with no
respawn (KI2, KI14; work items WM-I4 and WM-I5 add recovery and a boundary, WM-E1 a visible edge).

### LOD Culling
- **Terrain Bounds Culling**: Chunks above/below the terrain height range are skipped (same as Infinite Plane), with
  the range floored at `IslandEdgeHeight - 1 chunk`
- **Island Boundary Culling** (by chunk centre, 2D):
  - Circular: chunks whose centre is farther than `IslandRadius + FalloffWidth + chunk diagonal` are skipped
  - Rectangle: chunks beyond `IslandRadius + FalloffWidth + half a chunk` in X, or `IslandSizeY + FalloffWidth + half a
    chunk` in Y, are skipped
- **No far-band surface slab.** `bFarBandSurfaceSlabCulling` is InfinitePlane-only, so every island column keeps the
  whole height range (7-8 layers against the plane's 3-4 past band 0; KI5, work item WM-S1)
- **LOD distance is 2D** (height ignored), so whole columns sit in the near bands and stay uncompressed (KI13); the
  residency audit measures 3D and reports false findings in the top and bottom layers (KI11; work item WM-S2)
- Vertical window: the viewer's layer -2 to +4 (InfinitePlane: -2 to +8)

### Measured (review B, 2026-10-13)
Focused PIE against the InfinitePlane control (`DemoSmoothWorldConfig`) on the same routes; details in
WORLD_MODES_REVIEW_PLAN.md §13.3-13.5. Tiers: S, M and L are 0.8, 5 and 10 km across (R + F = 40,000, 250,000 and
500,000 uu).

| Measure | Island | Control |
|---------|--------|---------|
| Settled at the spawn: loaded chunks | S: 1,518 (the whole island) | 1,123 |
| Settled at the spawn: voxel data / renderer GPU | S: 247 / 23 MB | 135 / 23 MB |
| Peak loaded chunks on M's routes | 1,534-1,984 (+30-55%) | 1,177-1,282 |
| Frame P95 inside the island, v1500-v6000 | within +10% | |
| Manager P95, chord v1500-v3000 | +17-21% | |
| Coast (orbit at R), v1500-v6000 | frame +14-30%, manager +23-38%, catch-up 2.6-3.5× | |
| Peak systems memory while streaming | 1.0-1.55 GB | 1.05-1.27 GB |

- **Per-frame cost does not depend on the island's size:** L's interior chord matches M's (frame P95 32.2 vs 32.0 ms).
  The island-specific costs (no slab, 2D LOD distance, the coast) are per view.
- **Memory budget (decision D3):** 0.6 GB of voxel systems memory settled for a fully resident S-class island, and at
  most 1.5 GB at peak while streaming, at any speed and tier. The review met it in every run but M's v6000 chord (1.55 GB).
- **Largest island** the streaming-only model holds without new memory work: about 10-20 km across. Map tile images grow
  with the explored area and are never evicted (KI15: a fully explored L is about 0.55 GB of tiles; work item WM-E2).
- A 25-minute soak on M held frame time and memory flat.

### Known gaps
Each has a work item in WORLD_MODES_REVIEW_PLAN.md §14.2.

- **POI placement** samples InfinitePlane height in every mode, so POIs past R stand in the sea or over the floor on pads
  raised out of the water, and off-island POIs still stream and show on the map (KI1; WM-A3, WM-I1).
- **No world edge or recovery** (KI2, KI14; WM-I4, WM-I5, WM-E1).
- **Map exploration and markers run past the coast**, the PCG sampler has no island mask, and water-off navigation walks
  the floor (KI9; WM-I2). Scatter and surface NPCs already stop at the coast.
- **A changed island shape loads the old save** (the key is map + seed; KI7; WM-A4).
- **Spawn off the island with water off** takes the floor for land and freezes the pawn (KI8; WM-I3).

### Use Cases
- Floating islands
- Contained game worlds with defined boundaries
- Arena-style environments
- Tutorial areas with natural walls

---

## Mode 3: Spherical Planet

### Overview
Radial terrain on a spherical surface, allowing seamless exploration around an entire planet. Uses cubic chunks with radial density calculation.

### Configuration (UVoxelWorldConfiguration)
```cpp
// World Settings
EWorldMode WorldMode = EWorldMode::SphericalPlanet;
float WorldRadius = 100000.0f;           // Planet base radius

// Planet Parameters
float PlanetMaxTerrainHeight = 5000.0f;  // Mountains above radius
float PlanetMaxTerrainDepth = 2000.0f;   // Valleys below radius
float PlanetHeightScale = 5000.0f;       // Terrain amplitude

// Spawn Configuration
int32 PlanetSpawnLocation = 2;           // 0=+X, 1=+Y, 2=+Z (North Pole), 3=-Z
float PlanetSpawnAltitude = 500.0f;      // Height above surface
```

### Implementation (FSphericalPlanetWorldMode)
```cpp
float FSphericalPlanetWorldMode::GetDensityAt(const FVector& WorldPos,
                                               const FVoxelNoiseParams& NoiseParams) const
{
    // Vector from planet center to sample point
    FVector ToPoint = WorldPos - WorldOrigin;
    float DistFromCenter = ToPoint.Size();

    // Base sphere SDF (positive inside planet)
    float SphereSDF = WorldRadius - DistFromCenter;

    // Sample noise using direction vector (for seamless spherical wrapping)
    FVector Direction = ToPoint.GetSafeNormal();
    float NoiseValue = SampleSphericalFBM(Direction, NoiseParams);

    // Apply terrain displacement
    float TerrainOffset = NoiseValue * PlanetHeightScale;

    return SphereSDF + TerrainOffset;
}
```

### Spawn Position
```cpp
FVector UVoxelWorldConfiguration::GetPlanetSpawnPosition() const
{
    FVector SpawnDirection;
    switch (PlanetSpawnLocation) {
        case 0: SpawnDirection = FVector(1, 0, 0); break;  // +X (Equator)
        case 1: SpawnDirection = FVector(0, 1, 0); break;  // +Y (Equator)
        case 2: SpawnDirection = FVector(0, 0, 1); break;  // +Z (North Pole)
        case 3: SpawnDirection = FVector(0, 0, -1); break; // -Z (South Pole)
    }

    return WorldOrigin + SpawnDirection * (WorldRadius + PlanetSpawnAltitude);
}
```

### LOD Culling
Three types of culling are applied:

1. **Inner Shell Culling**: Chunks closer to center than `WorldRadius - MaxTerrainDepth` (inside planet core)
2. **Outer Shell Culling**: Chunks farther than `WorldRadius + MaxTerrainHeight` (empty space)
3. **Horizon Culling**: Chunks beyond geometric horizon `√(2Rh + h²)` where R = radius, h = viewer altitude

### Use Cases
- Planetary exploration games
- Space games with landable planets
- No Man's Sky style experiences
- Any world requiring true spherical geometry

### Current Limitations
- Designed for surface exploration (terrestrial gameplay)
- Orbital-scale viewing would benefit from octree LOD (future enhancement)
- Gravity/character controller integration not yet implemented

---

## GPU Implementation

All world modes share common GPU shader functions for consistency between CPU and GPU generation:

```hlsl
// WorldModeSDF.ush

float ApplyWorldModeSDF(float3 WorldPos, float BaseDensity, WorldModeParams Params)
{
    if (Params.Mode == WORLD_MODE_INFINITE)
    {
        return BaseDensity;  // No modification needed
    }
    else if (Params.Mode == WORLD_MODE_SPHERICAL)
    {
        float3 ToCenter = WorldPos - Params.WorldOrigin;
        float DistFromCenter = length(ToCenter);
        float SphereSDF = Params.WorldRadius - DistFromCenter;

        // Noise sampled via direction for seamless wrapping
        float3 Dir = normalize(ToCenter);
        float NoiseValue = SampleSphericalNoise(Dir, Params.NoiseParams);

        return SphereSDF + NoiseValue * Params.HeightScale;
    }
    else if (Params.Mode == WORLD_MODE_ISLAND)
    {
        float2 ToCenter = WorldPos.xy - Params.IslandCenter.xy;
        float Dist2D = length(ToCenter);

        float FalloffFactor = 1.0;
        if (Dist2D > Params.IslandRadius)
        {
            float T = saturate((Dist2D - Params.IslandRadius) / Params.FalloffWidth);
            FalloffFactor = ApplyFalloffCurve(T, Params.FalloffType);
        }

        float EdgeDensity = Params.EdgeHeight - WorldPos.z;
        return lerp(EdgeDensity, BaseDensity, FalloffFactor);
    }

    return BaseDensity;
}

float ApplyFalloffCurve(float T, int FalloffType)
{
    switch (FalloffType)
    {
        case 0: return 1.0 - T;                           // Linear
        case 1: return 1.0 - T * T * (3.0 - 2.0 * T);     // Smooth (Hermite)
        case 2: return 1.0 - T * T;                       // Squared
        case 3: return exp(-T * 3.0);                     // Exponential
    }
    return 1.0 - T;
}
```

---

## Switching World Modes

World mode can only be set at initialization time. Changing modes requires reinitializing the voxel world:

```cpp
void AVoxelWorldTestActor::InitializeVoxelWorld()
{
    // Get configuration (mode is set in Configuration asset or defaults)
    UVoxelWorldConfiguration* Config = Configuration;

    // LOD strategy reads mode from config and applies appropriate culling
    LODStrategy->Initialize(Config);

    // For spherical planet, log spawn position helper
    if (Config->WorldMode == EWorldMode::SphericalPlanet)
    {
        FVector SpawnPos = Config->GetPlanetSpawnPosition();
        UE_LOG(LogVoxel, Log, TEXT("Recommended spawn: (%.0f, %.0f, %.0f)"),
            SpawnPos.X, SpawnPos.Y, SpawnPos.Z);
    }
}
```

---

## Performance Comparison

Measured on the demo's geometry (voxel 75 uu, 64-voxel chunks, view distance 40,000 uu) in the world modes review,
2026-10-13 (WORLD_MODES_REVIEW_PLAN.md §12-§13). The old table here (200-500 chunks) was never measured.

| Mode | Loaded chunks, settled at the spawn | Peak loaded while streaming | Notes |
|------|-------------------------------------|-----------------------------|-------|
| Infinite Plane (smooth) | 1,123 | 1,177-1,282 | Far-band surface slab: 3-4 layers per column past band 0 |
| Infinite Plane, cubic | 1,123 | 1,194-1,246 | Same window; every chunk at full detail (LOD forced off, D7) |
| Island/Bowl | S: 1,518 (the whole island) | M: 1,534-1,984 | No slab yet: 7-8 layers per column (KI5) |
| Spherical Planet | not measured | not measured | CPU-only prototype |

All modes benefit from the streaming optimizations:
- O(1) duplicate detection in queues
- Separated load/unload decisions
- Position-based LOD update thresholds

---

## Completed Enhancements

- **Cave Systems**: Multi-layer cave generation (Cheese caverns, Spaghetti tunnels, Noodle passages) via `UVoxelCaveConfiguration` data asset. Density subtraction model works across all 3 world modes. See `Documentation/CAVE_SYSTEM.md`.

## Future Enhancements

- **Octree LOD**: For orbital-scale spherical planet viewing
- **Biome Transitions**: Smooth material blending between biomes

(Water, an earlier entry here, is implemented: per-chunk water tiles, underwater materials and water flags for
InfinitePlane and IslandBowl; see `WATER_SYSTEM.md`.)

---

## GPU Generation Parity & World-Mode Review (Phase A–D, 2026-07-01)

`bUseGPUGeneration` runs terrain generation on the GPU (render-thread compute dispatch + async readback).
The GPU `GenerateVoxelDensity.usf` is verified bit-close to the CPU `FVoxelCPUNoiseGenerator` by the
`VoxelWorlds.Generation.GPUvsCPU*` automation tests (full `FVoxelData` — density + material + biome +
metadata — at LOD 0/1/2, real RHI in-editor). Per-mode status:

| Mode | Analytic (`GetTerrainHeightAt`) | GPU generation | Parity result |
|------|--------------------------------|----------------|---------------|
| **InfinitePlane** | ✅ continentalness-modulated (matches gen) | ✅ full | density 99.9%, biome/metadata 100%, material 99.8% (0.2% = frac-dithered biome-blend-boundary voxels) |
| **IslandBowl** | ✅ continentalness + edge falloff (matches gen) | ✅ full | density 99.9%, biome/material/metadata 100% |
| **SphericalPlanet** | ⚠️ radial — analytic consumers assume Z-up (see gap analysis) | ❌ generic 3D-noise branch (RESERVED) | not GPU-parity; see gap analysis |

**Feature × mode parity (Phase D hardening):** each feature path below is a dedicated `GPUvsCPU*` test that
carries the full pipeline (biomes + materials + metadata) at LOD 0/1/2. All green for the two shipped modes
(InfinitePlane + IslandBowl); every value is 100% except the FP-boundary channels noted above.

| Feature | Test | Result |
|---------|------|--------|
| Cave carving (cheese + tunnel + cave-wall override) | `GPUvsCPUCavesParity` | density 99.9%, material/biome/meta 100% |
| Noise types (Perlin, Simplex, **Cellular**, **Voronoi**) | `GPUvsCPUNoiseTypeParity` | 100% all channels (all four types) |
| Water on (underwater materials + water-fill flag) | `GPUvsCPUWaterParity` | 100% all channels |
| Ore veins (**streak** shape + **per-biome** ranges) | `GPUvsCPUOreVariantParity` | 100% all channels |

Ports are deliberate mirrors: cave noise/threshold/depth-fade, the Perlin-permutation `Hash3D` behind
Cellular/Voronoi, the underwater-material gate (`WaterLevelEnabled && EnableUnderwaterMaterials &&
TerrainHeight < WaterLevel`), and the per-biome ore bake (`GetOreVeinsForBiome` → `OreStart/OreCount`) are
identical CPU↔shader, so Phase D added **no** shader changes — it locked the existing behaviour with tests.
Known limit (documented): ore veins with `Rarity < 1` use a precision-sensitive position hash (double on
CPU, float on GPU) and may differ by a few voxels; per-biome cave *scale* / underwater cave suppression are
CPU-only (the GPU cave loop carves the global layers). **Perf:** GPU single-chunk generation 3.6 ms vs CPU
23.5 ms (32³, with readback); streaming perf per [[voxel-streaming-benchmark]] unchanged since Phase B
(no gen-path code changed in Phase D).

**Cached analytic world mode (fixed Phase C):** the chunk manager now builds the analytic
`IVoxelWorldMode` matching `config->WorldMode` (was always `FInfinitePlaneWorldMode`). So spawn / nav /
`GetGeneratedSurfaceHeight` / `QueryEditMergedSurface` use the correct terrain math for every mode. POI placement
does **not**: it calls `FInfinitePlaneWorldMode::SampleTerrainHeight` directly in every mode, which is right inside an
island's radius and up to 7,000 uu wrong past it (finding KI1; work items WM-A3 and WM-I1 route it through the world
mode). `SetBiomeContext` is promoted to `IVoxelWorldMode` (InfinitePlane + IslandBowl apply
continentalness; other modes no-op).

**IslandBowl height = continentalness ∘ falloff (Phase C):** continentalness (internal oceans/mountains)
and the island edge falloff are orthogonal and compose — `ApplyFalloffToHeight(continentalness-modulated
height, falloff, EdgeHeight)`. Applied identically in CPU generation, the analytic query, and the GPU
shader (parity-locked). Small islands are ~unchanged (continentalness is near-constant at its ~50 km
wavelength); large islands gain internal terrain variation while still fading at the world edge.

## Spherical Planet — GPU / Analytic Gap Analysis (RESERVATION)

Spherical mode is **not** GPU-generation-parity or analytic-correct yet. The core mismatch: on a sphere,
"terrain height" is a **radial displacement from `PlanetCenter` along a direction**, not `Z(X, Y)`. Every
`IVoxelWorldMode` analytic consumer (spawn, nav, POI, `GetGeneratedSurfaceHeight`,
`QueryEditMergedSurface`) assumes a Z-up heightmap, so they are ill-defined on a sphere. Prioritized gap
list (implementation reserved):

1. **Ray/direction surface query** — replace `GetTerrainHeightAt(X, Y) → Z` with
   `GetSurfacePoint(Direction) → (point on shell, radial normal)`. Foundational: every other analytic
   consumer needs it.
2. **GPU radial SDF** — port the spherical SDF into `GenerateVoxelDensity.usf` (currently the generic
   3D-noise `else` branch): `density = (PlanetRadius + radialNoise·HeightScale) − |WorldPos − PlanetCenter|`,
   with noise sampled by **direction** (`normalize(WorldPos − PlanetCenter)`) for seamless wrapping.
3. **Radial culling & streaming** — cull to the shell band `[PlanetRadius − MaxDepth, PlanetRadius +
   MaxHeight]` instead of a Z min/max; `GetTerrainHeightBounds` (Z-extent) is the wrong shape here.
4. **Gravity, orientation & movement** — the pawn's "down" is toward `PlanetCenter`, not −Z; movement,
   collision orientation, and camera need a radial-up frame (game-side, outside VoxelWorlds).
5. **Biome/continentalness by direction** — sample the temp/moisture/continentalness fields on the sphere
   surface (by direction) rather than planar X/Y so bands wrap coherently around the planet.

Recommended sequencing if Spherical is picked up: (1) → (2) → (3) unlock a correct, GPU-generated,
streamable planet; (4) → (5) make it playable and natural. Until then, Spherical worlds should use CPU
generation (`voxel.GPUGeneration.Enable 0`) and treat the analytic queries as approximate.

---

**Status**: All three world modes implemented and tested with mode-specific LOD culling.
GPU generation parity: InfinitePlane + IslandBowl fully verified across the feature matrix — density,
materials, biomes, metadata, caves, all four noise types, water/underwater, and ore variants at LOD 0/1/2
(Phase A–D). SphericalPlanet reserved (see gap analysis).
