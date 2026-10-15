# World Modes

**Module**: VoxelGeneration
**Dependencies**: VoxelCore
**Last Updated**: 2026-10-15 (the world-mode API of milestone M2: one params builder, bounds, a local frame, save
fingerprints; WORLD_MODES_REVIEW_PLAN.md §14.6)

## Overview

The world mode system supports three terrain types, each optimized for different use cases. All modes use the same voxel generation and meshing pipelines but with mode-specific density calculations and LOD culling.

## IVoxelWorldMode Interface

`Source/VoxelGeneration/Public/IVoxelWorldMode.h` (abridged):

```cpp
class IVoxelWorldMode {
public:
    // Generation and the analytic surface
    virtual float GetDensityAt(const FVector& WorldPos, int32 LODLevel, float NoiseValue) const = 0;
    virtual float GetTerrainHeightAt(float X, float Y, const FVoxelNoiseParams& NoiseParams) const = 0;
    virtual void  GetTerrainHeightBounds(float& OutMin, float& OutMax) const;   // authority for vertical culling
    virtual void  SetBiomeContext(const UVoxelBiomeConfiguration* BiomeConfig); // continentalness, by value

    // Where the world ends (WM-A2)
    virtual bool              IsInsideWorld(double X, double Y) const;  // island: R + F footprint; plane: always
    virtual FVoxelWorldBounds GetWorldBounds() const;                   // Unbounded / Circle / Rectangle / Shell

    // Local frame (SP-A1, SP-A2)
    virtual FVector            GetUpVector(const FVector& WorldPos) const;   // +Z; radial on a planet
    virtual float              GetAltitude(const FVector& WorldPos) const;   // above SeaLevel; above PlanetRadius
    virtual FVoxelSurfaceCoord WorldToSurface(const FVector& WorldPos) const; // (0, X, Y); cube face + UV
    virtual FVector            SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const;

    // Coordinates, vertical limits, type, materials: WorldToChunkCoord, ChunkCoordToWorld, GetMinZ/GetMaxZ,
    // GetWorldModeType, IsHeightmapBased, GetMaterialAtDepth
};
```

The bounds and frame functions have defaults for an unbounded heightmap world with its datum at Z 0, so a world mode
outside VoxelWorlds (the dungeon plugin's, a test mock) keeps working; the three built-in modes override them.

**One builder per mode (WM-A1).** `FVoxelWorldModeFactory` (`VoxelGeneration/Public/VoxelWorldModeFactory.h`) is the
only place world-mode params are built from a `UVoxelWorldConfiguration`: `MakeIslandParams`, `MakePlanetParams`,
`MakeTerrainParams`, `Create` (a world mode with its biome context) and `FillGenerationRequest`. Generation (CPU and GPU,
through the request), the chunk manager, the LOD strategy's culls and slab, the map, the editor field viewer and POI
placement all use it. `MakeFingerprint` and `GenerationVersion` describe the world for saves (WM-A4).

**Reaching it from game code and Blueprint.** The chunk manager (a component of the world actor) exposes the configured
mode's bounds and frame as `BlueprintPure` functions in the `Voxel|World` category: `IsInsideWorld`, `GetWorldBounds`,
`GetUpVectorAt`, `GetAltitudeAt`, `WorldToSurface`, `SurfaceToWorld` and `GetWorldFingerprint`. VoxelWorlds does not
depend on the game plugins; they call these through `AVoxelWorldTestActor::GetChunkManager()`, as they already do for
the surface queries.

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

**Island centre:** `IslandCenterX/Y` is an offset from `WorldOrigin`. `FVoxelWorldModeFactory::MakeIslandParams`
resolves it once to the absolute centre `WorldOrigin + offset`, and every consumer (generation on the CPU and the GPU,
the analytic height, the LOD cull, the map, the field viewer, the bounds query) uses that (WM-A1, 2026-10-15). Before,
generation read the offset as an absolute position and the cull added `WorldOrigin`: with the demo's origin the two sat
55,706 uu apart and the cull cut 1,098 land columns (KI3). An island config with a non-zero `WorldOrigin` now generates
centred on `WorldOrigin + offset`, which moves its terrain; saves record the generation version (WM-A4).
`bIslandBowlShape` was removed on 2026-10-14: it was copied in four places and read nowhere (KI4).

**Bounds:** `GetWorldBounds()` is a circle of radius `R + FalloffWidth` (or a rectangle of half sizes `R + F` by
`SizeY + F`) around that centre, and `IsInsideWorld(X, Y)` is the same footprint test generation uses
(`IsWithinIslandBounds`). Consumers start using it in milestone M3 (WM-I1, WM-I2, WM-I3, WM-I5).

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
- **Terrain Bounds Culling**: Chunks above/below the terrain height range are skipped (same as Infinite Plane). The
  range is the world mode's `GetTerrainHeightBounds`, whose minimum includes the floor beyond the edge
  (`IslandEdgeHeight - 1000`), plus one chunk
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

- **POI placement** samples the island's own height since WM-A3 (2026-10-15), so land POIs are no longer placed in the
  falloff band's sea or on the floor. Sites past R+F of types that may place anywhere are still placed, and still stream
  and show on the map, until placement uses the bounds query (KI1; WM-I1).
- **No world edge or recovery** (KI2, KI14; WM-I4, WM-I5, WM-E1).
- **Map exploration and markers run past the coast**, the PCG sampler has no island mask, and water-off navigation walks
  the floor (KI9; WM-I2). Scatter and surface NPCs already stop at the coast.
- ~~**A changed island shape loads the old save**~~ Fixed by WM-A4 (2026-10-15): the save records the world's
  fingerprint (mode, shape, voxel grid, generation version), and a save from a different radius is refused, kept on disk,
  and the world starts fresh (KI7).
- **Spawn off the island with water off** takes the floor for land and freezes the pawn (KI8; WM-I3).

### Use Cases
- Floating islands
- Contained game worlds with defined boundaries
- Arena-style environments
- Tutorial areas with natural walls

---

## Mode 3: Spherical Planet

### Overview
Radial terrain on a spherical surface, on the same Cartesian chunk grid as the other modes, centred on the
planet. **Status (2026-10-14): a prototype.** It generates on the CPU only, the terrain is mirrored across the
equator, and the game systems assume +Z is up. What exists, what is wrong and what a playable planet needs are
in [SPHERICAL_PLANET_REQUIREMENTS.md](../SPHERICAL_PLANET_REQUIREMENTS.md); the work is scheduled in
[WORLD_MODES_REVIEW_PLAN.md](../WORLD_MODES_REVIEW_PLAN.md) §14.4. A test config and map live in the parent
project at `/Game/PluginTesting/Planet`.

### Configuration (UVoxelWorldConfiguration)
```cpp
// World Settings
EWorldMode WorldMode = EWorldMode::SphericalPlanet;
float WorldRadius = 100000.0f;           // Planet base radius

// Planet Parameters
float PlanetMaxTerrainHeight = 5000.0f;  // Shell above the radius: must cover BaseHeight + PlanetHeightScale / 2
float PlanetMaxTerrainDepth = 2000.0f;   // Shell below the radius: must cover PlanetHeightScale / 2 - BaseHeight
float PlanetHeightScale = 5000.0f;       // Peak-to-trough terrain amplitude
float BaseHeight = 0.0f;                 // Radial offset of the whole surface (shown for planets since SP-A5)

// Spawn Configuration
int32 PlanetSpawnLocation = 2;           // 0=+X, 1=+Y, 2=+Z (North Pole), 3=-Z
float PlanetSpawnAltitude = 500.0f;      // Height above surface
```

### Implementation (FSphericalPlanetWorldMode)

What the code does today (CPU generation, `GenerateChunkSphericalPlanet`):

```cpp
// FSphericalPlanetWorldMode::SampleSphericalNoise: the direction's X and Y only, scaled by a fixed 10,000.
float Noise = FInfinitePlaneWorldMode::SampleTerrainNoise2D(Direction.X * 10000, Direction.Y * 10000, NoiseParams);

// FSphericalPlanetWorldMode::GetDensityAt: radial SDF, positive inside.
float Dist = |WorldPos - PlanetCenter|;                 // PlanetCenter = WorldOrigin
if (Dist > OuterRadius + 1000) return -1000;            // quick reject: air above the shell
if (Dist < InnerRadius - 1000) return 1000;             // quick reject: core below the shell
float TerrainRadius = WorldRadius + BaseHeight + Noise * PlanetHeightScale * 0.5;
return TerrainRadius - Dist;
```

Known defects, all listed with their fixes in the requirements doc (§1): `Direction.Z` is never sampled, so the
northern and southern hemispheres are mirror images and features stretch north-south near the equator, and the
fixed 10,000 ties feature size to the radius (SP-B1); height-material rules read the radius as a height, so the
default rules turn every surface to snow (SP-B2); `GetTerrainHeightAt(X, Y)` returns a radius for the equatorial
direction (X, Y, 0), not a Z (SP-A1, SP-A3). Terrain outside the shell `[WorldRadius - PlanetMaxTerrainDepth,
WorldRadius + PlanetMaxTerrainHeight]` is not streamed and is cut off by the quick reject;
`UVoxelWorldConfiguration::GetSettingsWarnings` reports a shell that does not cover the amplitude (SP-A5).

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
- **CPU generation only.** The GPU generator has no planet branch, so `UVoxelWorldConfiguration::ShouldUseGPUGeneration()`
  is false for planets and the chunk manager logs a warning when `bUseGPUGeneration` is on (SP-B3; SP-B4 adds the
  GPU planet)
- Mirrored, radius-scaled noise and all-snow materials (above); no tests beyond the config checks
- Every surface consumer (scatter, PCG, map, navigation, POIs, NPCs, water, movement) assumes +Z is up
  (requirements doc, Appendix A); gravity toward the centre is not implemented
- Water: generation compares against `WaterRadius`, every other consumer still reads the Z-plane `WaterLevel`
  (SP-E1); `GetSettingsWarnings` flags water on a planet
- Designed for surface exploration; orbital viewing would need an octree or cube-sphere quadtree LOD (SD5)
- The world-mode API has a planet side since M2 (2026-10-15): the shell as bounds, radial up, altitude above
  `PlanetRadius`, and a gnomonic cube-sphere surface coordinate that round-trips. Nothing consumes it on a planet yet;
  spherical Phase 1 moves the 2D systems onto it and may change the cube warp

---

## GPU Implementation

`Shaders/Private/GenerateVoxelDensity.usf` mirrors the CPU generator for the two heightmap modes and is locked to
it by the `VoxelWorlds.Generation.GPUvsCPU*` parity tests:

- **InfinitePlane:** 2D terrain noise, continentalness, `NoiseToTerrainHeight` (`WorldModeSDF.ush`), then the
  signed distance to that height.
- **IslandBowl:** the same height, then `ApplyIslandFalloffToHeight` (`IslandFalloff.ush`, a port of
  `FIslandBowlWorldMode::ComposeTerrainHeight`): the falloff toward `EdgeHeight` inside the island's extent, the
  flat floor at `EdgeHeight - 1000` beyond it. The curves on `t` (0 at `IslandRadius`, 1 at the edge) are
  Linear `1 - t`, Smooth `(1-t)^2 (3 - 2(1-t))`, Squared `(1-t)^2` and Exponential
  `(e^-3t - e^-3) / (1 - e^-3)`; every curve reaches 0 at the edge.
- **SphericalPlanet:** no branch. A planet request would fall through to the generic 3D-noise path and produce
  blobs, which is why planets generate on the CPU (`ShouldUseGPUGeneration`, SP-B3). The GPU radial SDF is SP-B4,
  spherical Phase 2.

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
did not until WM-A3 (2026-10-15): it called `FInfinitePlaneWorldMode::SampleTerrainHeight` directly in every mode, which
is right inside an island's radius and up to 7,000 uu wrong past it (finding KI1). It now samples the configured world
mode, built by `FVoxelWorldModeFactory` and carried in the placement config. `SetBiomeContext` is promoted to `IVoxelWorldMode` (InfinitePlane + IslandBowl apply
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
   consumer needs it. *(M2, 2026-10-15: the frame it builds on exists: `GetUpVector`, `GetAltitude`,
   `WorldToSurface`/`SurfaceToWorld` with a planet implementation. `GetSurfacePoint` itself comes with Phase 1.)*
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
streamable planet; (4) → (5) make it playable and natural. Until then spherical worlds generate on the CPU
whatever `bUseGPUGeneration` says (`ShouldUseGPUGeneration`, since 2026-10-14), and the analytic queries are
approximate. The full requirements list, superseding this summary, is
[SPHERICAL_PLANET_REQUIREMENTS.md](../SPHERICAL_PLANET_REQUIREMENTS.md).

---

**Status**: InfinitePlane and IslandBowl are implemented and tested, with mode-specific LOD culling.
SphericalPlanet is a CPU-only prototype with no tests beyond the configuration checks
(`VoxelWorlds.Configuration.PlanetSettings`); see the gap analysis and the requirements doc.
GPU generation parity: InfinitePlane + IslandBowl fully verified across the feature matrix — density,
materials, biomes, metadata, caves, all four noise types, water/underwater, and ore variants at LOD 0/1/2
(Phase A–D).
