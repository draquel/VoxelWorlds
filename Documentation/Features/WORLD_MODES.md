# World Modes

**Module**: VoxelGeneration
**Dependencies**: VoxelCore
**Last Updated**: 2026-10-16 (milestone M6: the island's seabed slopes into a far ocean that reaches the horizon, and
the island streams only the columns that touch it; WORLD_MODES_REVIEW_PLAN.md §14.10)

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
    virtual FFloatInterval GetSurfaceRange(const FVector2D& MinXY, const FVector2D& MaxXY,
                                           const FVoxelNoiseParams& NoiseParams) const; // a column's slab (WM-S1)
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

### Streaming window and LOD
- **Vertical window from the terrain** (decision D8, M4): every chunk column streams its own surface slab (the world mode's
  `GetSurfaceRange` over the footprint ± 1.5 chunks, up to the water plane) at every distance, plus the viewer's layer ± 2
  in the columns within band 0 + two chunks; all clipped to `GetTerrainHeightBounds` + one chunk. The viewer's altitude
  never removes the ground. Details: `Features/LOD_SYSTEM.md`, "The Vertical Window"
- **Horizon** (load at the view distance, unload at 1.2×) is measured horizontally; **LOD distance** in 3D

### Use Cases
- Traditional Minecraft-style worlds
- Large-scale terrain exploration
- Any game requiring unlimited horizontal exploration

---

## Mode 2: Island/Bowl

### Overview
Bounded terrain with a falloff at the edges, for a contained world: an island in the sea, or a landmass ringed by a
low floor. Reviewed as a "large but finite world" with every game system on in 2026-10 (WORLD_MODES_REVIEW_PLAN.md §5,
§13). The terrain itself is correct; the gaps were around it. Milestone M3 (2026-10-15, that plan's §14.7) keeps placement,
exploration, markers, PCG, navigation and spawning inside the island and gives it a hard edge with recovery; what was
left was streaming cost, fixed by M4 (2026-10-15, §14.8: the island now streams like the plane), and a visible edge,
fixed by M6 (2026-10-16, §14.10: a seabed that slopes from the coast into a far ocean that reaches the horizon). See
"Beyond the edge", "Measured" and "Known gaps" below.

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
float IslandSeabedDepth = 6000.0f;   // Beyond the edge the seabed slopes this far below EdgeHeight...
float IslandSeabedSlopeWidth = 20000.0f; // ...over this distance past the edge (0 = a step), then stays flat
bool  bIslandFarOcean = true;        // Draw the sea and the seabed past what is streamed (UVoxelChunkManager)
float IslandFarOceanDistance = 2000000.0f; // How far past the edge the far ocean reaches
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
(`IsWithinIslandBounds`). `FVoxelWorldBounds` also answers the footprint geometry its consumers need:
`GetDistanceToEdgeXY`, `ContainsCircleXY` (a POI site with its falloff ring), `IntersectsBox2D` (a map tile),
`ClampXY` (a spawn search start, the world map's pan) and `GetOutwardNormalXY` (the push-back). Since M3 every consumer
uses them: POI placement and markers, map exploration and the world map, the PCG surface sampler, surface navigation,
the spawn search and the boundary (WM-I1, WM-I2, WM-I3, WM-I5).

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
**The seabed (M6, WM-E1).** Past `R + FalloffWidth` generation writes the seabed: from `EdgeHeight` at the edge it slopes
down by `IslandSeabedDepth` over `IslandSeabedSlopeWidth` (a smoothstep, `FIslandBowlWorldMode::GetSeabedHeight`), then
stays flat. Distance past the edge is measured as the falloff measures it: radially for a circle, the larger axis excess
for a rectangle (`GetDistancePastEdge`). Generation on the CPU and the GPU (`IslandSeabedHeight` in `IslandFalloff.ush`,
locked by `GPUvsCPUIslandBowlParity`'s seabed cases), the analytic height and `GetDensityAt` share it, and
`GetTerrainHeightBounds` reaches its floor. Until M6 the terrain dropped 1,000 uu at the edge in a vertical wall to a flat
floor (`OutsideFloorDepth`, removed); `IslandSeabedDepth` 1,000 with a slope width of 0 is that step.

**What streams.** Only chunk columns that touch the footprint (`FVoxelWorldBounds::IntersectsBox2D`, the test the map
uses for its tiles). The cull used to keep a ring out to a chunk diagonal past the edge (6,788 uu with the demo's 4,800 uu
chunks) holding the floor and the water; nothing past the walls needs it now.

**The far ocean (M6, WM-E1).** The chunk manager draws the sea and the seabed past what is streamed, out to
`IslandFarOceanDistance` past the edge (`FVoxelFarOcean`; `UVoxelChunkManager::RebuildFarOcean`), on three procedural
mesh components of the world actor (the static meshes, the grid's frame and the grid's window):

| Mesh | Covers | Material | Updated |
|------|--------|----------|---------|
| Grid water | Every chunk column of the footprint's box (plus two columns) that the streaming has not opened: a column opens when it holds a chunk with voxel data or a water tile | `WaterMeshMaterial`, at `WaterLevel` like the water tiles | Whenever the open columns change (same tick, so a column's water tile and the far ocean never overlap: the water is translucent). Only the window around the open columns (their bounds plus 8 columns) is rebuilt then; the frame outside it is rebuilt when the open columns leave the window and it moves, so the cost does not grow with the island |
| Grid floor | Under the grid water's columns that touch the footprint: flat at `EdgeHeight` inside the edge, the seabed outside | The terrain's material instance, with the seabed's material id | With the grid water |
| Open sea | From the grid's rectangle out to the footprint's box plus `IslandFarOceanDistance`, in widening rings | `WaterMeshMaterial` | Built once |
| Seabed | From the edge out to the open sea's corners, half a voxel under the seabed profile, so the loaded voxel seabed hides it | Terrain material, seabed material id (the most common one generation gives just past the edge) | Built once |

The water's undersides are separate sections, shown only while the viewer is under the water plane. The components are
their own primitives, so a recreated terrain scene proxy (G15) does not touch them. Inside the island, columns past the
view distance also get the far ocean's water: unloaded land there reads as sea at the horizon (as the plane shows the
void past its view distance); `voxel.World.FarOceanInland <uu>` limits the water to columns within that distance of the
edge (0 = only those that reach it). `voxel.World.FarOcean 0` hides it, `voxel.World.FarOceanStats` logs its state; the
water needs `bEnableWaterLevel` and `WaterMeshMaterial` (without them the world actor draws its fallback plane), the
seabed is drawn either way. `GetSettingsWarnings` flags an `IslandEdgeHeight` at or above the water.

**Saves.** The seabed shapes only terrain past `R + F`, where a save holds nothing (POIs, map tiles and edits stay inside
the walls), so it is not part of the save fingerprint and `FVoxelWorldModeFactory::GenerationVersion` stays 1: saves
from before M6 load.

**The boundary (decision D9, M3).** Nothing gets there any more:

- **Hard boundary at R + F (WM-I5).** The chunk manager builds invisible collision walls on the bounds
  (`FVoxelWorldBoundary`, `UVoxelChunkManager::RebuildWorldBoundary`): an inscribed polygon for a circle, with its chords
  within 50 uu of the circle (63 walls for island S, 158 for M, 223 for L), four walls for a rectangle; 5 m thick, from a
  chunk under the lowest terrain to 50,000 uu over the highest. They block pawns, NPCs and physics bodies, and let sight,
  interaction and camera traces through. `UVoxelWorldConfiguration::bWorldBoundary` (default on) builds them; the cvar
  `voxel.World.Boundary 0` turns their collision off at runtime. Unbounded worlds and planets have none.
- **Soft push-back before it (WM-I5, VoxelCharacterPlugin).** In the last 4,000 uu before the edge
  (`UVCMovementComponent::WorldEdgePushBackDistance`) a current caps the pawn's outward speed, from its full speed at the
  band's inner side to drifting back at 250 uu/s at the edge. A swimmer heading out stalls about 1,600 uu before the wall,
  and the HUD says why ("A strong current pushes you back: this is the edge of the world"). Flying (debug flight, the
  streaming benchmark) is exempt.
- **Recovery (WM-I4, every world mode).** A player that falls out of the world (KillZ, or 20,000 uu under the world's
  lowest terrain, `AVCCharacterBase::OutOfWorldDepth`) goes back to the last ground it stood on inside the world and dies
  there through the combat pipeline, so the usual respawn follows; before, KillZ destroyed the pawn and nothing respawned
  it. A terrain wait that times out with nothing under the pawn moves it to the respawn point (the rest point, else a
  player start, else the world's centre) and waits there.

The visible edge (an ocean to the horizon, a seabed that slopes instead of the cliff) came with M6 (WM-E1, above).

### LOD Culling
- **Terrain Bounds Culling**: Chunks above/below the terrain height range are skipped (same as Infinite Plane). The
  range is the world mode's `GetTerrainHeightBounds`, whose minimum includes the floor beyond the edge
  (`IslandEdgeHeight - 1000`), plus one chunk
- **Island Boundary Culling** (2D): a chunk streams only when its column touches the footprint
  (`FVoxelWorldBounds::IntersectsBox2D`: the circle or the rectangle `R + F`). Until M6 a circle kept every chunk whose
  centre was within `R + F + chunk diagonal`, a ring of columns wholly outside the edge
- **The same streaming window as Infinite Plane** (M4, 2026-10-15): each column's surface slab at every distance, the
  viewer's layer ± 2 near the viewer, the horizon measured horizontally and LOD distance in 3D. Until M4 islands had no
  slab (every column kept 7-8 layers; KI5), measured LOD distance in 2D (whole columns stayed in the near bands,
  uncompressed; KI13), and the residency audit, measuring 3D, reported false findings (KI11). Columns that cross R + F
  keep their slab too: the seabed, up to the water plane

### Measured after M4 (2026-10-15)
The same routes as review B, one batch on one build, island M against the control at v1500, v3000 and v6000
(WORLD_MODES_REVIEW_PLAN.md §14.8):

| Measure | Island | Control |
|---------|--------|---------|
| Settled at the spawn: loaded chunks | S: 1,020 (the whole island) | 979 |
| Peak loaded chunks on M's routes | 965-1,218 (−15% to +8% of the control) | 1,081-1,219 |
| Frame P95, chord / perimeter / coast | −19% to +3% | |
| Manager P95, chord / perimeter / coast | −27% to +12% | |
| Peak voxel systems memory while streaming | 0.86-1.21 GB | 1.07-1.36 GB |
| Residency audit after every run | 0 | 0 |

- **The island streams like the plane.** Every column keeps its surface slab, so the layers above and below the
  terrain no longer load (on the v3000 chord the island held 169-198 chunks in each of layers −3 to 3; now 24-230,
  the control's shape).
- **Catch-up after stopping** is the one threshold missed: at v1500 the island stops with 10-50 chunks still queued
  (0.27-0.46 s to drain against the control's 0.17-0.18 s), because its sea-heavy view needs about 3% more
  generation than the control's route and v1500 sits at generation capacity. At v3000 and v6000 the two overlap.

### Measured (review B, 2026-10-13)
Before M4. Focused PIE against the InfinitePlane control (`DemoSmoothWorldConfig`) on the same routes; details in
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
- **Largest island** the streaming-only model holds without new memory work: about 10-20 km across. Map tile images grew
  with the explored area and were never evicted (KI15: a fully explored L is about 0.55 GB of tiles). *Fixed by WM-E2
  (M6): the map keeps its images under a fixed 80 MB budget; a fully explored L holds 37 MB (MAP_SYSTEM.md).*
- A 25-minute soak on M held frame time and memory flat.

### Known gaps
Each has a work item in WORLD_MODES_REVIEW_PLAN.md §14.2.

- ~~**POI placement**~~ Fixed by WM-A3 and WM-I1 (2026-10-15): placement samples the island's own height, and only sites
  wholly inside R + F, falloff ring included, are placed, whatever the type's surface filter (KI1).
- ~~**No world edge or recovery**~~ Fixed by WM-I4 and WM-I5 (2026-10-15), and the visible edge by WM-E1 (2026-10-16):
  see "Beyond the edge" (KI2).
- ~~**Map exploration and markers run past the coast**~~ Fixed by WM-I2 (2026-10-15): exploration and the world map's
  pan stop at the bounds, markers show only inside, the PCG sampler skips points outside, and navigation blocks every
  cell outside, water on or off (KI9). Scatter and surface NPCs already stopped at the coast.
- ~~**A changed island shape loads the old save**~~ Fixed by WM-A4 (2026-10-15): the save records the world's
  fingerprint (mode, shape, voxel grid, generation version), and a save from a different radius is refused, kept on disk,
  and the world starts fresh (KI7).
- ~~**Spawn off the island with water off**~~ Fixed by WM-I3 (2026-10-15): the spawn search only accepts points inside
  the bounds, and a start outside searches from a chunk inside the edge (KI8).
- ~~**Streaming cost**~~ Fixed by WM-S1 and WM-S2 (2026-10-15): the surface slab at every distance and 3D LOD distance
  (KI5, KI13); the residency audit agrees with the unload rule (KI11). See "Measured after M4".
- ~~**Map tile eviction**~~ Fixed by WM-E2 (2026-10-16): tile images live under a fixed budget, least recently used
  first, and are drawn again when looked at; exploration is kept whole (KI15; MAP_SYSTEM.md).
- **Unloaded island land past the view distance** reads as sea at the horizon (the far ocean stands in for it); a
  distant silhouette of the island stays a nice-to-have (WORLD_MODES_REVIEW_PLAN.md §6).

### Use Cases
- Floating islands
- Contained game worlds with defined boundaries
- Arena-style environments
- Tutorial areas with natural walls

---

## Mode 3: Spherical Planet

### Overview
Radial terrain on a spherical surface, on the same Cartesian chunk grid as the other modes, centred on the
planet. **Status (2026-10-16): a correct planet, generated on the GPU** (spherical Phases 1 and 2,
[WORLD_MODES_REVIEW_PLAN.md](../WORLD_MODES_REVIEW_PLAN.md) §15 and §16): seamless terrain sampled by 3D direction,
continentalness, materials by altitude, caves, underground and water flags along local up, and a streaming window that
covers the shell at every latitude. The GPU generator has a planet branch (Phase 2, locked to the CPU by
`Generation.GPUvsCPUSphericalParity`), so planets follow `bUseGPUGeneration` like the other modes. **Walkable since
2026-10-17** (Phase 3, review plan §17): gravity points at the centre (the character movement component sets the
engine's custom gravity direction on every move), movement, camera and spawn work in the gravity frame, every water
consumer asks one sea (`FVoxelSeaSurface`), and the chunk manager draws the ocean shell. **Its systems since
2026-10-17** (Phase 4, review plan §18): scatter, PCG, the map, navigation, NPC AI and the NPC spawner, world items and
placing, the edit brush, the clock and the sky work in the local frame ("Systems on a planet", below). **Its places
since 2026-10-17** (Phase 5, review plan §19): conditioning zones are caps on the datum sphere, POIs place on the cube
sphere and compose in their pad space, campsites level along local up, and dungeons stamp, stitch and play in pad space
("Places on a planet", below). What remains is in [SPHERICAL_PLANET_REQUIREMENTS.md](../SPHERICAL_PLANET_REQUIREMENTS.md). Test content
lives in the parent project: `/Game/PluginTesting/Planet` (`PlanetTest`, `PlanetWorldConfig`) and
`/Game/PluginTesting/Review/Review_Planet` (water on).

### Configuration (UVoxelWorldConfiguration)
```cpp
// World Settings
EWorldMode WorldMode = EWorldMode::SphericalPlanet;
float WorldRadius = 100000.0f;           // Planet base radius

// Planet Parameters
float PlanetMaxTerrainHeight = 5000.0f;  // Shell above the radius: must cover the highest surface (below)
float PlanetMaxTerrainDepth = 2000.0f;   // Shell below the radius: must cover the deepest surface (below)
float PlanetHeightScale = 5000.0f;       // Peak-to-trough terrain amplitude (before continentalness)
float BaseHeight = 0.0f;                 // Radial offset of the whole surface (shown for planets since SP-A5)

// Spawn Configuration
int32 PlanetSpawnLocation = 2;           // 0=+X, 1=+Y, 2=+Z (North Pole), 3=-Z
float PlanetSpawnAltitude = 500.0f;      // Height above surface
```

### Implementation (FSphericalPlanetWorldMode)

One surface function, `FSphericalPlanetWorldMode::SampleRadialDisplacement`, serves generation
(`GenerateChunkSphericalPlanet`), the analytic surface (`GetSurfacePoint`) and the configuration's shell check:

```cpp
// The 2D fields are sampled at the point on the datum sphere, relative to the centre (GetNoiseSamplePoint):
// a feature is ~1 / Frequency uu across on any radius, and a 3D field has no seam and no mirror (SP-B1).
FVector P = Direction * PlanetRadius;
float Noise = FBM3D(P, NoiseParams);
// Continentalness, the same field and curves as the heightmap modes (SP-B5).
Effective = ComputeEffectiveTerrainParamsAt(P, ...);    // BaseHeight + offset, PlanetHeightScale * multiplier
float Displacement = Effective.BaseHeight + Noise * Effective.HeightScale * 0.5;

// The signed distance generation writes (ShellSignedDistance): radial, positive inside.
float Dist = |WorldPos - PlanetCenter|;                 // PlanetCenter = WorldOrigin
if (Dist > OuterRadius + 1000) return -1000;            // quick reject: air above the shell
if (Dist < InnerRadius - 1000) return 1000;             // quick reject: core below the shell
return (PlanetRadius + Displacement) - Dist;
```

Per voxel, generation also: samples temperature and moisture at the same datum-sphere point; applies height-material
rules to the voxel's altitude above the datum (SP-B2); flattens caves along local up (`ScaleAltitude`: the altitude
times the layer's `VerticalScale`) and suppresses them under the sea (`WaterRadius`, SP-B6). The post-passes run along
local down: underground classification on digital lines along the chunk centre's down (SP-B7), and the water fill by
the sea sphere (`ApplyRadialWaterFillPass`, `FVoxelSeaSurface`, SP-B8); the chunk manager's cross-chunk water and the
edit-time propagation test the same sphere. The GPU generator does all of this too (GPU Implementation, below).
`GetTerrainHeightAt(X, Y)` is a legacy heightmap query: on a planet it logs once (naming SP-A3) and returns the world Z
where the vertical line through (X, Y) meets the terrain on the upper hemisphere, so a stray caller lands on the northern
surface instead of reading a radius as a Z. The surface queries are along local up (below); `GetAltitudeBounds` is the
shell, −`PlanetMaxTerrainDepth` to +`PlanetMaxTerrainHeight`, and `GetTerrainHeightBounds` the shell's world-Z extent
(SP-A4). Terrain outside the shell `[WorldRadius - PlanetMaxTerrainDepth,
WorldRadius + PlanetMaxTerrainHeight]` is cut off; `UVoxelWorldConfiguration::GetSettingsWarnings` reports a shell that
does not cover `BaseHeight ± PlanetHeightScale / 2` with continentalness's extremes (`GetDisplacementBounds`).

The surface coordinate (`WorldToSurface` / `SurfaceToWorld`) is a cube sphere with the tangent warp,
`u = (4/π)·atan(g)` per axis on each face: equal UV cells cover areas within 1.38× of each other.

### Systems on a planet (Phase 4)

Every system that reads the surface asks it along local up (review plan §18; heightmap worlds keep their own code paths,
checked bit for bit where it matters):

| System | On a planet | Where |
|--------|-------------|-------|
| Surface queries | `UVoxelChunkManager::QueryEditMergedSurfacePoint`: a digital line of edit-merged voxels along local up, the crossing refined in the trilinear density, the normal and slope against local up; `GetGeneratedSurfacePoint` (analytic); `FVoxelSurfaceQuery::QueryPlanetSurfaceConditions` / `SampleSurfacePoint` (generation's material and biome at a point). Heightmap modes: `QueryEditMergedSurface(X, Y)` | `VoxelChunkManager`, `VoxelSurfaceQuery` |
| Surface coordinate | `FVoxelCubeSphere` (face frames, the tangent warp, the tangent frame with north toward +Z) and `FVoxelCubeSphereGrid` (cells on the six faces, atlas keys `(Face * N + U, V)`, neighbours across face edges, a flood fill within an angle): the key of the map, navigation and the NPC spawner | `VoxelCore/VoxelCubeSphere.h` |
| Meshing | Requests carry the chunk centre's `LocalUp`: the MC LOD material scan walks along it, the CPU MC geomorph runs along the axis nearest it; the vertex factory's face type (top, side, bottom) is measured against the radial up | [MARCHING_CUBES_MESHING.md](MARCHING_CUBES_MESHING.md), [MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) |
| Ores | The `Rarity` test hashes the planet-relative voxel position with an exact integer hash, the same on the CPU and the GPU (`PlanetGenerationVersion` 3) | [ORE_VEINS.md](ORE_VEINS.md) |
| Scatter | Surface points along digital lines in the chunk's local up; face type, slope and elevation by local up and altitude; instances stand on local up; density per area kept | [SCATTER_SYSTEM.md](SCATTER_SYSTEM.md) |
| PCG | The surface sampler's points on the cube faces' UV grid, rotated to local up | `VoxelPCG` |
| Map | Tiles are cube-sphere cells; pixels along their directions; the minimap is the tangent plane under the player, the world map a globe | [MAP_SYSTEM.md](MAP_SYSTEM.md) |
| Navigation | Each search in the tangent-plane frame of the region it starts in, altitude as height, the sea as an altitude plane | [SURFACE_NAVIGATION.md](SURFACE_NAVIGATION.md) |
| Edit brush | The cylinder's axis is local up at the brush centre (`FVoxelBrushParams::Axis`, zero = local up) | [EDIT_LAYER.md](EDIT_LAYER.md) |
| Bench | `voxel.Bench.Run gc=Lat:Lon:Heading[:Alt]`: a great circle at a fixed altitude | `VoxelStreamingBenchmark` |
| Game side | NPC AI and eye height in the gravity frame, the NPC spawner on cube-sphere cells, world items and placing along gravity, local solar time and a sky centred on the planet (`WorldClockPlugin`) | parent project, `VoxelCharacterPlugin`, `InteractionPlugin`, `WorldClockPlugin` |

Tree injection never runs on a planet (SP-B9: cubic only).

### Places on a planet (Phase 5)

Review plan §19 (decisions D14-D16):

| System | On a planet | Where |
|--------|-------------|-------|
| Conditioning zones | A planet zone is a cap on the datum sphere: `FVoxelConditioningZone::Direction` (zero = a heightmap zone), `InnerRadius` and `FalloffWidth` as great-circle distances, `TargetHeight` an altitude, weighted with the heightmap's smoothstep (`GetWeightAtDirection`) and blended into the terrain's altitude along each direction (`FVoxelTerrainConditioning::ApplyToAltitude`) on the CPU and the GPU (`PlanetApplyConditioning`). The chunk manager gathers a chunk's zones by the cap its box subtends (`GatherConditioningZonesOnSphere`); conditioners answer `IVoxelTerrainConditioner::GatherConditioningOnSphere`. A zone of the other kind does nothing (with a warning). `PlanetGenerationVersion` 4 | `VoxelGeneration/VoxelTerrainConditioning.h`, `SphericalPlanet.ush` |
| The generated surface | The natural surface plus the zones: `UVoxelChunkManager::GetGeneratedSurfacePoint` / `GetGeneratedPlanetAltitude`, the start of `QueryEditMergedSurfacePoint`'s walk, the spawn, the map's planet tiles, scatter's line top, PCG's far points; `GetSurfacePointAt` stays the natural surface | `VoxelChunkManager` |
| POIs | Placed on the cube-sphere grid's cells (`FPOIPlacement::GeneratePlanetPOIs`, the site survey on the tangent plane in altitudes, the sea's altitude as the water line); each POI has a pad space, the tangent frame at its centre (x east, y north, z local up), in which its claims, structures and dungeon are composed with the plane's arithmetic; structures are draped onto the pad's cap; streaming, `poi.Find` and the map markers by 3D distance | parent project, `VoxelWorldPOI` (its `POI_POPULATION_PLAN.md`, `POI_DUNGEON_PLAN.md`) |
| Campsites | Generated campsites stand on the draped pad and respawn the player along their own up; `PlaceCampsite` levels its pad and apron along local up | `VoxelWorldPOI` |
| Dungeons | Stamped and stitched in a frame (`UDungeonVoxelStamper::StampDungeonInFrame`, `UDungeonEntranceStitcher::StitchEntranceInFrame`), tiles laid out in the tile actor's own frame (`ADungeonActor::bMapInActorFrame`); the POI's dungeon record and gameplay stay in pad space and convert at the boundary | `ProceduralDungeonPlugin`, `VoxelWorldPOI` |

### The radius ceiling (SP-H2)

The scene proxy bakes absolute world positions into float vertices (`FVector3f`), whose spacing (ULP) is about 0.06 uu
1,000,000 uu (10 km) from the world origin and 1 uu at 10,000,000 uu (100 km), where seams and the crossfade start to
show it; collision trimeshes are chunk-local, placed by a double transform, and stay exact
([TRAVERSAL_LIMITS.md](../Research/TRAVERSAL_LIMITS.md) #4, #5). Every surface vertex lies about R from the
planet's centre, so with the centre at the world origin the ceiling is a radius of about 100 km (a centre placed away
from the origin adds its distance); the GPU's density is planet-relative already (review plan §16), its cave and ore
noise at absolute float positions (SP-H1). Past that, origin rebasing
is needed, and it has to separate two roles `WorldOrigin` plays today: the planet's centre (generation, gravity, the
surface coordinate) and the chunk grid's origin (chunk keys, saves, the edit layer).

### Spawn Position

`UVoxelChunkManager::GetPlanetSpawnPosition()` (SP-B11) puts `PlanetSpawnAltitude` over the terrain along the configured
direction (`GetSurfacePointAt`). The character spawns by direction (SP-F4): `FVCVoxelNavigationHelper::FindSpawnablePosition`
takes the terrain along the direction from the centre to the requested position, or, when that is under the sea, the
nearest land on rings of directions one chunk of arc apart (`PlanetTest`'s PlayerStart is over the north pole, which is
sea on `Review_Planet`: the pawn lands 3 rings away). The configuration's own helper has no world mode, so it gives the
datum's point:

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

### Streaming Window and Culling
- **The shell window** (`FDistanceBandLODStrategy::GetPlanetShellWindow`, SP-C1): each chunk column streams the layers
  whose chunks meet the shell widened by half a chunk, `[R - MaxTerrainDepth - ½W, R + MaxTerrainHeight + ½W]`,
  within the view distance: one interval, or two when the column passes inside the inner radius. It covers the
  shell at every latitude; the window it replaced was the viewer's layer ± up to 16.
- **Shell culling**: chunks wholly inside the window's inner radius (core) or outside its outer radius (space).
- **Horizon culling** (SP-C2): chunks farther than `√(2Rh + h²) + √(2RH + H²)` (h the viewer's altitude, H =
  `MaxTerrainHeight`), so peaks past the viewer's own horizon still load; none for a viewer inside the planet.

### Use Cases
- Planetary exploration games
- Space games with landable planets
- No Man's Sky style experiences
- Any world requiring true spherical geometry

### Current Limitations
- **Small planets** (SD1): past the planet-relative distance and direction the GPU works in single precision (its
  cave and ore noise at absolute positions), as the CPU's noise does; parity is measured on a 250,000 uu planet.
  Large planets need SP-H1
- Voxel-carved dungeons do not hold pawns on smooth worlds, the plane included (G24 in the review plan); tile-dressed
  dungeons (the demo's) play on a planet as on the plane. A voxel-carved dungeon's walls on a planet step by up to
  √3/2 voxel (the stamp writes air and solid, not graded densities)
- Water: one sea for every consumer (`FVoxelSeaSurface`, `WaterRadius`). The ocean shell is a sphere, so it also shows
  inside dry caves below sea level
- The sun has no axial tilt: the poles sit in a low sun all day (decision D12)
- Designed for surface exploration; orbital viewing would need an octree or cube-sphere quadtree LOD (SD5)
- Radius: see "The radius ceiling" above

---

## GPU Implementation

`Shaders/Private/GenerateVoxelDensity.usf` mirrors the CPU generator for the two heightmap modes and is locked to
it by the `VoxelWorlds.Generation.GPUvsCPU*` parity tests:

- **InfinitePlane:** 2D terrain noise, continentalness, `NoiseToTerrainHeight` (`WorldModeSDF.ush`), then the
  signed distance to that height. Then, in the CPU's order, the biome blend, the caves (`GpuCaveCarve`, shared with
  the planet: `VerticalScale` along Z, the cave configuration's biome overrides, and `UnderwaterMinDepth` where the
  column's terrain is under the water level; G18, fixed 2026-10-17), materials, height rules, the cave-wall override
  and ores.
- **IslandBowl:** the same height, then `ApplyIslandFalloffToHeight` (`IslandFalloff.ush`, a port of
  `FIslandBowlWorldMode::ComposeTerrainHeight`): the falloff toward `EdgeHeight` inside the island's extent, the
  seabed beyond it (`IslandSeabedHeight`: a smoothstep down by `IslandSeabedDepth` over `IslandSeabedSlopeWidth`). The curves on `t` (0 at `IslandRadius`, 1 at the edge) are
  Linear `1 - t`, Smooth `(1-t)^2 (3 - 2(1-t))`, Squared `(1-t)^2` and Exponential
  `(e^-3t - e^-3) / (1 - e^-3)`; every curve reaches 0 at the edge.
- **SphericalPlanet** (spherical Phase 2, SP-B4; `SphericalPlanet.ush`): `FVoxelCPUNoiseGenerator::GenerateChunkSphericalPlanet`
  per voxel, in the same order. The radial height (terrain noise and continentalness at `Direction × PlanetRadius`),
  the shell signed distance with its quick rejects, biome materials with the underwater variant where the column's
  terrain is under `WaterRadius`, height rules by altitude, caves with `VerticalScale` along local up (through the
  shared `GpuCaveCarve`, biome overrides and `UnderwaterMinDepth` included), the cave-wall override, ores. Distances
  and directions are planet-relative: the C++ passes the chunk origin minus the planet centre, computed in double. The
  post-passes run along local down and flood inside the sea sphere (`GPU_PIPELINE.md`, Stage 1b). Params come from the
  request, which `FVoxelWorldModeFactory::FillGenerationRequest` fills. Parity at LOD 0/1/2 on both poles, the
  equator, 45°, a cube-face edge and corner, a coast, a cave and caves under the seabed:
  `Generation.GPUvsCPUSphericalParity` (review plan §16.2 for the numbers).

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

Measured on the demo's geometry (voxel 75 uu, 64-voxel chunks, view distance 40,000 uu) in the world modes review
(WORLD_MODES_REVIEW_PLAN.md §12-§14). The old table here (200-500 chunks) was never measured.

| Mode | Loaded chunks, settled at the spawn | Peak loaded while streaming | Notes |
|------|-------------------------------------|-----------------------------|-------|
| Infinite Plane (smooth) | 979 (M4) | 1,081-1,219 (M4) | Each column's surface slab at every distance, the viewer's layer ± 2 nearby (§14.8) |
| Infinite Plane, cubic | 1,123 (before M4) | 1,194-1,246 (before M4) | Same window as smooth; every chunk at full detail (LOD forced off, D7). Not re-measured since M4 |
| Island/Bowl | S: 1,020, the whole island (M4; 1,518 before) | M: 965-1,218 (M4; 1,534-1,984 before) | The same window as Infinite Plane since M4 |
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
streamable planet; (4) → (5) make it playable and natural. *(2026-10-16: (1), (2), (3) and (5) are done, spherical
Phases 1 and 2; planets generate on the GPU when `bUseGPUGeneration` is on. Gravity and movement, (4), are Phase 3.)* The full requirements list, superseding this summary, is
[SPHERICAL_PLANET_REQUIREMENTS.md](../SPHERICAL_PLANET_REQUIREMENTS.md).

---

**Status**: InfinitePlane and IslandBowl are implemented and tested, with mode-specific LOD culling.
SphericalPlanet generates a correct planet on the GPU or the CPU (spherical Phases 1 and 2), with generation,
streaming and GPU parity tests; the game systems on a planet are Phases 3 to 5 of the requirements doc.
GPU generation parity: InfinitePlane + IslandBowl fully verified across the feature matrix — density,
materials, biomes, metadata, caves, all four noise types, water/underwater, and ore variants at LOD 0/1/2
(Phase A–D); SphericalPlanet at LOD 0/1/2 (`GPUvsCPUSphericalParity`, spherical Phase 2).
