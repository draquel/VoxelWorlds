# World Modes

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelGeneration
**Dependencies**: VoxelCore

## Overview

Three world modes (`EWorldMode`) shape the terrain: **InfinitePlane** (a heightmap without bounds), **IslandBowl** (a
bounded heightmap with a seabed and a far ocean past its edge) and **SphericalPlanet** (radial terrain around a
centre, up to about 1,000 km in radius). Any heightmap mode can be meshed with cubic blocks ("Block worlds"); a planet
is smooth only. Every mode shares the generation pipeline (CPU and GPU), the meshers, streaming, the far field, the
cut-over to altitude, precision far from the origin and one noise ("Mode parity"); each mode supplies its own density,
bounds and local frame through `IVoxelWorldMode`.

The review behind most of this page ran from 2026-10-12 to 10-26; its log is `WORLD_MODES_REVIEW_PLAN.md` (frozen
history). IDs in parentheses (WM-*, G*, D*, SP-*) name the work item, finding or decision there.

## IVoxelWorldMode Interface

`Source/VoxelGeneration/Public/IVoxelWorldMode.h` (abridged):

```cpp
class IVoxelWorldMode {
public:
    // Generation and the natural surface
    virtual float GetDensityAt(const FVector& WorldPos, int32 LODLevel, float NoiseValue) const = 0;
    virtual FVoxelTerrainSurfacePoint GetSurfaceOnUpLine(const FVector& WorldPos,
                                                         const FVoxelNoiseParams& NoiseParams) const = 0;
    virtual FVoxelTerrainSurfacePoint GetSurfacePoint(const FVector& WorldPos,
                                                      const FVoxelNoiseParams& NoiseParams) const; // with the normal
    virtual void  GetAltitudeBounds(float& OutMin, float& OutMax) const;   // every altitude generation can make
    virtual FFloatInterval GetSurfaceRange(const FVector2D& MinXY, const FVector2D& MaxXY,
                                           const FVoxelNoiseParams& NoiseParams) const; // a column's slab
    virtual void  SetBiomeContext(const UVoxelBiomeConfiguration* BiomeConfig); // continentalness, by value

    // Where the world ends
    virtual bool              IsInsideWorld(double X, double Y) const;  // island: R + F footprint; plane: always
    virtual FVoxelWorldBounds GetWorldBounds() const;                   // Unbounded / Circle / Rectangle / Shell, Z range

    // Local frame
    virtual FVector            GetUpVector(const FVector& WorldPos) const;    // +Z; radial on a planet
    virtual float              GetAltitude(const FVector& WorldPos) const;    // above SeaLevel; above the radius
    virtual FVoxelSurfaceCoord WorldToSurface(const FVector& WorldPos) const; // (0, X, Y); cube face + UV
    virtual FVector            SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const;

    // Coordinates, vertical limits, type, materials: WorldToChunkCoord, ChunkCoordToWorld, GetMinZ/GetMaxZ,
    // GetWorldModeType, IsHeightmapBased, GetMaterialAtDepth
};
```

`GetSurfaceOnUpLine` is the natural surface (no conditioning zones, caves or edits) on the local up-line through a
point, one sample; it is the one surface primitive every mode implements (code review CR2). The bounds and frame
functions have defaults for an unbounded heightmap world with its datum at Z 0, so a world mode outside VoxelWorlds (a
test mock) keeps working; the three built-in modes override them.

**One builder per mode.** `FVoxelWorldModeFactory` (`VoxelGeneration/Public/VoxelWorldModeFactory.h`) is the only place
world-mode params are built from a `UVoxelWorldConfiguration`: `MakeIslandParams`, `MakePlanetParams`,
`MakeTerrainParams`, `Create` (a world mode with its biome context) and `FillGenerationRequest`. Generation (CPU and
GPU), the chunk manager, the LOD strategy, the map, the editor field viewer and POI placement all use it (WM-A1).
`MakeFingerprint` and `GenerationVersion` describe the world for saves (`DATA_STRUCTURES.md`, "Version fingerprints").

**The surface ladder.** Game code asks the chunk manager: `GetGeneratedSurfacePoint` (the generated surface, conditioning
zones applied, block tops on a block world), `QueryEditMergedSurfacePoint` (the loaded voxels with edits) and
`QuerySurfacePoint` with an `EVoxelSurfaceLayer` (`Natural`, `Generated`, `EditMerged`) for any rung; all work along
local up in every mode. `FVoxelSurfaceQuery` answers the same without a chunk manager (`SampleSurfacePoint`, the
conditions at the surface).

**The conditioning at a place.** What turns the natural surface into the generated one travels as one value,
`FVoxelConditioning` (`VoxelGeneration/Public/VoxelTerrainConditioning.h`): the conditioning zones there (`Zones`, in
the order they are applied) and the conditioning lines there (`Lines`, shared references). A generation request carries
it (`FVoxelNoiseGenerationRequest::Conditioning`), and every reader of the generated surface takes it and asks it for
the height on a heightmap world (`ApplyToHeight`) or the altitude on a planet (`ApplyToAltitude`); none takes a list of
zones, so a new shape adds no parameter anywhere. The chunk manager fills it: `GatherConditioning` for an XY region or
a world box, `GatherConditioningOnSphere` for a cap, from its sources (`FVoxelConditioningSources`: the zones added
with `AddConditioningZone` and the game's `IVoxelTerrainConditioner`). Each gather states what it is for
(`EVoxelConditioningGather`): `Complete` for a chunk's generation request, a point query, a chunk's scatter and the PCG
sampler, which must agree with the generated voxels and so take every line (a conditioner may compute one then and
there); `Ready` for the far field and the map, whose regions are too wide to wait for a line, and which draw again
when the conditioning changes (`OnTerrainConditioningChanged`). Zones are always complete. A conditioner supplies zones
through `GatherConditioning` and `GatherConditioningOnSphere` and lines through `GatherConditioningLines` and
`GatherConditioningLinesOnSphere`, which default to none. The line itself (`FVoxelConditioningLine`) is declared and
nothing more: it has no vertices, nothing makes one and nothing evaluates one, so every world generates what its zones
alone make (`Documentation/TERRAIN_LINES_PLAN.md` at the project root, phase L1; the line arrives with L2).

In the heightmap modes `QueryEditMergedSurfacePoint` reads the edit-merged voxels on the voxel grid: voxel g holds the
density at `WorldOrigin + g * VoxelSize`, where the generator samples it and the meshers place it. The query takes the
four grid columns around (X, Y), one chunk under the generated estimate to one above, finds each column's top crossing
(`FVoxelSurfaceQuery::ExtractSurfaceFromColumn`) and blends them bilinearly; the nearest column names the material and
the biome, and the normal is the blend's slope (the density gradient where a column has no surface in its window). The
height is then on the mesh and the collision: on a hilly plane of 75 uu voxels 3.4 uu off on average and 26 at worst,
the slope 3 to 5 degrees off the collision triangle's (G165, 2026-11-09: one column read from an off-grid Z and (X, Y) had
put it 42 uu above on average and up to 266 on a steep slope; `VoxelWorlds.Streaming.SurfaceQuery.MatchesCollision`). The
nearest column is read over the whole window and the other three over a few voxels about its crossing (the whole window
when their top crossing is not there): about 3.2 us a query, a cold path search on `VoxelDemo` 3.2 ms against 2.7 before. A block world's query is the block tops instead
(`QueryEditMergedBlockSurface`), a planet's a digital line along local up.

**Reaching it from game code and Blueprint.** The chunk manager (a component of the world actor) exposes the configured
mode's bounds and frame as `BlueprintPure` functions in the `Voxel|World` category: `IsInsideWorld`, `GetWorldBounds`,
`GetUpVectorAt`, `GetAltitudeAt`, `WorldToSurface`, `SurfaceToWorld`, `GetSurfacePointAt`, `IsBlockSurface`,
`SnapToBlockTop`, `GetSpawnPosition` and `GetWorldFingerprint`. VoxelWorlds does not depend on the game plugins; they
call these through `AVoxelWorldActor::GetChunkManager()`.

---

## Mode 1: Infinite Plane (Default)

A heightmap extending without bounds in X and Y.

### Configuration (UVoxelWorldConfiguration)
```cpp
EWorldMode WorldMode = EWorldMode::InfinitePlane;
float SeaLevel = 0.0f;           // datum of the height
float HeightScale = 5000.0f;     // noise amplitude
float BaseHeight = 0.0f;         // additional offset
```

### Implementation (FInfinitePlaneWorldMode)

The terrain noise at (X, Y, 0) gives the height, `SeaLevel + BaseHeight + Noise * HeightScale`
(`NoiseToTerrainHeight`, unclamped, as the GPU's `WorldModeSDF.ush`), with continentalness's offset and scale
multiplier when the biome configuration enables it (`ComputeEffectiveTerrainParams`, `BIOME_SYSTEM.md`), then the
conditioning zones. The density is the signed distance to that height:

```cpp
float FInfinitePlaneWorldMode::GetDensityAt(const FVector& WorldPos, int32 LODLevel, float NoiseValue) const
{
    const float TerrainHeight = NoiseToTerrainHeight(NoiseValue, TerrainParams);
    return CalculateSignedDistance(WorldPos.Z, TerrainHeight);   // positive below the surface
}
```

### Streaming window and LOD

- **Vertical window from the terrain** (D8): every chunk column streams its own surface slab (the world mode's
  `GetSurfaceRange` over the footprint ± 1.5 chunks, up to the water plane) at every distance, plus the viewer's layer
  ± 2 in the columns near the viewer; all clipped to the terrain's height bounds plus one chunk. The viewer's altitude
  never removes the ground, and a climbing viewer loads no air above the bounds.
- **Horizon** (load at the view distance, unload at 1.2x) is measured horizontally; **LOD distance** in 3D.
- **The far field**: past the window the generated surface is drawn to `voxel.World.FarFieldDistance` (2,000,000 uu),
  a quadtree of patches per root tile of `voxel.World.FarFieldRootChunks` chunks on the chunk grid (0 = automatic), the
  sea at `WaterLevel` in it; band patches (one chunk column) are hidden where the window's chunks are drawn, so there is
  no gap and no overlap at the window's edge (WM-MP1, D36).
- **The voxel cut-over**: above `VoxelCutoverAltitude` (0 = three view distances) over the ground or the sea under the
  streaming viewer the window unloads and the far field alone draws; it streams back under 0.85 of it, ahead of a
  descending camera (the descent prefetch). Only while a far field is on: with `voxel.World.FarField 0` the window
  keeps its ground disk at any altitude (D38, kept by D41).

Details: `LOD_SYSTEM.md`, "The Vertical Window", "The Far Field" and "Orbit and Back".

---

## Mode 2: Island/Bowl

A bounded heightmap: an island in the sea, with a falloff at the edge, a seabed past it and a far ocean to the horizon.
Every game system stays inside it, a hard boundary stops the player at its edge, and it streams like the plane.

### Configuration (UVoxelWorldConfiguration)
```cpp
EWorldMode WorldMode = EWorldMode::IslandBowl;

int32 IslandShape = 0;               // 0 = Circular, 1 = Rectangle
float IslandRadius = 50000.0f;       // radius (circular) or SizeX (rectangle)
float IslandSizeY = 50000.0f;        // SizeY (rectangle only)
float IslandFalloffWidth = 10000.0f; // width of the falloff (F)
int32 IslandFalloffType = 1;         // 0 = Linear, 1 = Smooth, 2 = Squared, 3 = Exponential
float IslandCenterX = 0.0f;          // offset from WorldOrigin (below)
float IslandCenterY = 0.0f;
float IslandEdgeHeight = -1000.0f;   // terrain height the falloff reaches at R + F
float IslandSeabedDepth = 6000.0f;   // past the edge the seabed slopes this far below EdgeHeight...
float IslandSeabedSlopeWidth = 20000.0f; // ...over this distance (0 = a step), then stays flat
bool  bIslandFarOcean = true;        // draw the sea and the seabed past what is streamed
float IslandFarOceanDistance = 2000000.0f; // how far past the edge the far ocean reaches
bool  bWorldBoundary = true;         // invisible walls at R + F
```

**Island centre.** `IslandCenterX/Y` is an offset from `WorldOrigin`. `FVoxelWorldModeFactory::MakeIslandParams`
resolves it once to the absolute centre `WorldOrigin + offset`, and every consumer (generation on the CPU and the GPU,
the analytic height, the LOD cull, the map, the field viewer, the bounds query) uses that (WM-A1).

**Bounds.** `GetWorldBounds()` is a circle of radius `R + F` (or a rectangle of half sizes `R + F` by `SizeY + F`)
around the centre, and `IsInsideWorld(X, Y)` is the same footprint test generation uses (`IsWithinIslandBounds`).
`FVoxelWorldBounds` answers the footprint geometry its consumers need: `GetDistanceToEdgeXY`, `ContainsCircleXY` (a POI
site with its falloff ring), `IntersectsBox2D` (a map tile, a chunk column), `ClampXY` (a spawn search start, the world
map's pan) and `GetOutwardNormalXY` (the push-back). POI placement and markers, map exploration and the world map, the
PCG surface sampler, surface navigation, the spawn search and the boundary all use them, so nothing is placed, explored,
marked or navigated past the coast (WM-I1 to WM-I5).

### Shapes and falloff

```cpp
enum class EIslandShape : uint8 { Circular = 0, Rectangle = 1 };
enum class EIslandFalloffType : uint8 { Linear = 0, Smooth = 1, Squared = 2, Exponential = 3 };
```

The falloff parameter t runs from 0 at `IslandRadius` to 1 at the edge (`R + F`): for a circle the radial distance,
for a rectangle the larger of the two axis excesses (`CalculateFalloffFactorForPoint`). Every curve runs from 1 to
exactly 0, so the terrain meets `EdgeHeight` at the edge: Linear `1 - t`, Smooth `(1-t)^2 (3 - 2(1-t))`, Squared
`(1-t)^2`, Exponential `(e^-3t - e^-3) / (1 - e^-3)` (`ApplyFalloffCurve`). The island's height is the
continentalness-modulated heightmap height faded toward `EdgeHeight` by that factor (`ComposeTerrainHeight`,
`ApplyFalloffToHeight`), on the CPU, in the analytic query and on the GPU (`ApplyIslandFalloffToHeight`,
`IslandFalloff.ush`), pinned by `Generation.GPUvsCPUIslandBowlParity`.

### Beyond the edge

**The seabed.** Past `R + F` generation writes the seabed: from `EdgeHeight` at the edge it slopes down by
`IslandSeabedDepth` over `IslandSeabedSlopeWidth` (a smoothstep, `FIslandBowlWorldMode::GetSeabedHeight`), then stays
flat; distance past the edge is measured as the falloff measures it (`GetDistancePastEdge`). The CPU, the GPU
(`IslandSeabedHeight`) and the analytic height share it, and the height bounds reach its floor (WM-E1). The seabed is
left out of the save fingerprint: it shapes only terrain past `R + F`, where a save holds nothing.

**What streams.** Only chunk columns that touch the footprint (`FVoxelWorldBounds::IntersectsBox2D`), with the same
window as the plane: each column's surface slab at every distance, the viewer's layer ± 2 near it, the horizon
measured horizontally and LOD distance in 3D (WM-S1, WM-S2).

**What draws past the window.** The far field draws the island's land and inland water out to its bounds box, whose
corners past the circle hold the far field's sea (D37). Past the box the chunk manager draws the far ocean
(`FVoxelFarOcean`, `UVoxelChunkManager::RebuildFarOcean`) on its own procedural mesh component: the open sea in widening
rings out to `IslandFarOceanDistance` past the edge (`WaterMeshMaterial`; its underside shown only while the viewer is
under the water plane) and a static seabed half a voxel under the seabed profile, with the material generation gives
just past the edge. `voxel.World.FarOcean 0` hides it; `voxel.World.FarOceanStats` logs it. The water needs
`bEnableWaterLevel` and `WaterMeshMaterial`; `GetSettingsWarnings` flags an `IslandEdgeHeight` at or above the water.
`voxel.World.FarField 0` is a development switch: with it an island draws nothing inside its box past the window.

**The boundary.**

- **Hard boundary at R + F.** The chunk manager builds invisible collision walls on the bounds (`FVoxelWorldBoundary`,
  `UVoxelChunkManager::RebuildWorldBoundary`): an inscribed polygon for a circle, its chords within 50 uu of the circle,
  four walls for a rectangle; 5 m thick, from a chunk under the lowest terrain to 50,000 uu over the highest. They block
  pawns, NPCs and physics bodies, and let sight, interaction and camera traces through. `bWorldBoundary` builds them;
  `voxel.World.Boundary 0` turns their collision off at runtime. Unbounded worlds and planets have none.
- **Soft push-back before it** (VoxelCharacterPlugin). Within `UVCMovementComponent::WorldEdgePushBackDistance`
  (4,000 uu) of the edge a current caps the pawn's outward speed, down to drifting back at `WorldEdgePushBackSpeed`
  (250 uu/s) at the edge; the HUD says why. Debug flight is exempt.
- **Recovery (every world mode).** A player that falls out of the world (KillZ, or `AVCCharacterBase::OutOfWorldDepth`,
  20,000 uu, under the world's lowest terrain) goes back to the last ground it stood on and dies there through the
  combat pipeline, so the usual respawn follows. A terrain wait that times out with nothing under the pawn moves it to
  the respawn point.

`vox.Edge` reports and tests the edge. Streaming on island M stays within a plane control's frame, manager and memory
figures on chord, perimeter and coast routes; a fully explored 10 km island keeps its map images under the map's 80 MB
budget (`MAP_SYSTEM.md`). History: `WORLD_MODES_REVIEW_PLAN.md` §13-§14.

---

## Mode 3: Spherical Planet

Radial terrain on a sphere, on the same Cartesian chunk grid as the other modes, centred on `WorldOrigin`. A planet is
generated on the GPU or the CPU (the same voxels), walkable with gravity toward its centre, carries every game system
(scatter, PCG, map, navigation, NPCs, POIs, dungeons, campsites, saves, the clock) in its local frame, is precise up to
a radius of about 1,000 km, and can be left for orbit and landed on again. Test content in the parent project:
`/Game/PluginTesting/Demo` (`PlanetTest`) and `/Game/PluginTesting/Tests/Review/` (`Review_Planet`, `Review_Planet_M` 50 km,
`Review_Planet_L` 1,000 km).

### Configuration (UVoxelWorldConfiguration)
```cpp
EWorldMode WorldMode = EWorldMode::SphericalPlanet;
float WorldRadius = 100000.0f;           // the datum radius R
float PlanetMaxTerrainHeight = 5000.0f;  // shell above R: must cover the highest surface
float PlanetMaxTerrainDepth = 2000.0f;   // shell below R: must cover the deepest surface
float PlanetHeightScale = 5000.0f;       // peak-to-trough amplitude (before continentalness)
float BaseHeight = 0.0f;                 // radial offset of the whole surface
int32 PlanetSpawnLocation = 2;           // 0 = +X, 1 = +Y, 2 = +Z (north pole), 3 = -Z
float PlanetSpawnAltitude = 500.0f;      // spawn height above the surface
float PlanetOrbitCeiling = 0.0f;         // the out-of-world bound above R (0 = 3 R)
float VoxelCutoverAltitude = 0.0f;       // the voxel cut-over (0 = three view distances)
```

### Implementation (FSphericalPlanetWorldMode)

One surface function, `FSphericalPlanetWorldMode::SampleRadialDisplacement`, serves generation
(`FVoxelCPUNoiseGenerator::GenerateChunkSphericalPlanet`), the natural surface (`GetSurfaceOnUpLine`) and the
configuration's shell check:

```cpp
// The fields sample the point on the datum sphere, relative to the centre (GetNoiseSamplePoint): a feature is
// ~1 / Frequency uu across at any radius, and a 3D field has no seam and no mirror.
FVector P = Direction * PlanetRadius;
float Noise = FVoxelNoise::FBM3D(P, NoiseParams);
// Continentalness: the same field and curves as the heightmap modes.
Effective = ComputeEffectiveTerrainParams(Direction, PlanetRadius, ...);  // BaseHeight + offset, scale x multiplier
float Displacement = Effective.BaseHeight + Noise * Effective.HeightScale * 0.5;   // NoiseToRadialDisplacement

// The signed distance generation writes, in double: the terrain's altitude minus the voxel's
// (ShellSignedDistanceAtAltitude), positive inside, with quick rejects above and below the shell.
```

Per voxel, generation also samples temperature and moisture at the same datum-sphere point (`SampleClimate`); applies
height-material rules to the voxel's altitude; flattens caves along local up (`ScaleAltitude`: the altitude times the
layer's `VerticalScale`) and applies `UnderwaterMinDepth` under the sea; and runs the post-passes along local down:
underground classification on digital lines along the chunk centre's down (`FVoxelDownLines`), the water fill by the
sea sphere (`ApplyRadialWaterFillPass`, `FVoxelSeaSurface`). The GPU does all of it (`SphericalPlanet.ush`,
`GPU_PIPELINE.md`), pinned by `Generation.GPUvsCPUSphericalParity` at LOD 0, 1 and 2 on both poles, the equator, 45°,
a cube-face edge and corner, a coast, a cave and caves under the seabed.

`GetAltitudeBounds` is the shell, -`PlanetMaxTerrainDepth` to +`PlanetMaxTerrainHeight`, and `GetWorldBounds` its
world-Z extent. Terrain outside the shell is cut off; `UVoxelWorldConfiguration::GetSettingsWarnings` reports a shell
that does not cover `BaseHeight ± PlanetHeightScale / 2` with continentalness's extremes (`GetDisplacementBounds`).

The surface coordinate (`WorldToSurface` / `SurfaceToWorld`) is a cube sphere with the tangent warp,
`u = (4/π)·atan(g)` per axis on each face: equal UV cells cover areas within 1.38x of each other.

### Systems on a planet

Every system that reads the surface asks it along local up:

| System | On a planet | Where |
|--------|-------------|-------|
| Surface queries | `UVoxelChunkManager::QueryEditMergedSurfacePoint`: a digital line of edit-merged voxels along local up, the crossing refined in the trilinear density, the normal and slope against local up; `GetGeneratedSurfacePoint` (the generated surface, zones applied); `FVoxelSurfaceQuery::QueryPlanetSurfaceConditions` / `SampleSurfacePoint` (generation's material and biome at a point) | `VoxelChunkManager`, `VoxelSurfaceQuery` |
| Surface coordinate | `FVoxelCubeSphere` (face frames, the tangent warp, the tangent frame with north toward +Z) and `FVoxelCubeSphereGrid` (cells on the six faces, atlas keys `(Face * N + U, V)`, neighbours across face edges, a flood fill within an angle): the key of the map, navigation and the NPC spawner | `VoxelCore/VoxelCubeSphere.h` |
| Meshing | Requests carry the chunk centre's `LocalUp`: the Marching Cubes LOD material scan walks along it, the geomorph runs along the axis nearest it; the vertex factory's face type (top, side, bottom) is measured against the radial up | [MARCHING_CUBES_MESHING.md](MARCHING_CUBES_MESHING.md), [MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) |
| Ores | The `Rarity` test hashes the planet-relative voxel position, the same on the CPU and the GPU; the vein noise samples the shared noise with one streak direction per ore | [ORE_VEINS.md](ORE_VEINS.md) |
| Noise | Every field samples `FVoxelNoise` / `VoxelNoise.ush` (a hashed lattice, split-origin sampling, the continuous Simplex kernel); the GPU splits one origin per chunk and field (`NoiseOrigins`) | [NOISE_LIBRARY.md](NOISE_LIBRARY.md) |
| Scatter | Surface points along radial rays on the cube-sphere grid, one family for every chunk (G31); face type, slope and elevation by local up and altitude; instances stand on local up | [SCATTER_SYSTEM.md](SCATTER_SYSTEM.md) |
| PCG | The surface sampler's points on the cube faces' UV grid, rotated to local up | `VoxelPCG` |
| Map | Tiles are cube-sphere cells; the minimap is the tangent plane under the player, the world map a globe (a coarse albedo when zoomed out on a big planet) | [MAP_SYSTEM.md](MAP_SYSTEM.md) |
| Navigation | Each search in the tangent-plane frame of the region it starts in, altitude as height, the sea as an altitude plane | [SURFACE_NAVIGATION.md](SURFACE_NAVIGATION.md) |
| Edit brush | The cylinder's axis is local up at the brush centre (`FVoxelBrushParams::Axis`, zero = local up) | [EDIT_LAYER.md](EDIT_LAYER.md) |
| Bench | `voxel.Bench.Run gc=Lat:Lon:Heading[:Alt]` (a great circle), `climb=Lat:Lon[:Alt]`, `descend=Lat:Lon:Alt` | `VoxelStreamingBenchmark` |
| Game side | Gravity toward the centre (the movement component sets the engine's custom gravity direction on every move), movement, swimming, traces, camera and control rotation in the gravity frame (`FVCGravityFrame`), NPC AI and the NPC spawner, world items and placing along gravity, local solar time with a world-fixed sun and a sky centred on the planet | `VoxelCharacterPlugin`, `InteractionPlugin`, `WorldClockPlugin`, parent project |

Voxel tree injection never runs on a planet (cubic only).

### Places on a planet

| System | On a planet | Where |
|--------|-------------|-------|
| Conditioning zones | A planet zone is a cap on the datum sphere: `FVoxelConditioningZone::Direction` (zero = a heightmap zone), `InnerRadius` and `FalloffWidth` as great-circle distances, `TargetHeight` an altitude, weighted with the heightmap's smoothstep (`GetWeightAtDirection`) and blended into the terrain's altitude along each direction (`FVoxelConditioning::ApplyToAltitude`) on the CPU and the GPU (`PlanetApplyConditioning`). The chunk manager gathers a chunk's conditioning by the cap its box subtends (`GatherConditioning` with a box, `GetPlanetCapOfBox`); conditioners answer `IVoxelTerrainConditioner::GatherConditioningOnSphere`. A zone of the other kind does nothing (with a warning) | `VoxelGeneration/VoxelTerrainConditioning.h`, `SphericalPlanet.ush` |
| The generated surface | The natural surface plus the zones: `UVoxelChunkManager::GetGeneratedSurfacePoint`, the start of `QueryEditMergedSurfacePoint`'s walk, the spawn, the map's planet tiles, scatter's ray tops, PCG's far points; `GetSurfacePointAt` stays the natural surface | `VoxelChunkManager` |
| POIs | Placed on the cube-sphere grid's cells (`FPOIPlacement::GeneratePlanetPOIs`: the site survey on the tangent plane in altitudes, the sea's altitude as the water line); each POI has a pad space, the tangent frame at its centre (x east, y north, z local up), in which its claims, structures and dungeon are composed with the plane's arithmetic; structures are draped onto the pad's cap; streaming, `poi.Find` and the map markers by 3D distance | `VoxelWorldPOI` |
| Campsites | Generated campsites stand on the draped pad and respawn the player along their own up | `VoxelWorldPOI` |
| Dungeons | Stamped and stitched in a frame (`UDungeonVoxelStamper::StampDungeonInFrame`, `UDungeonEntranceStitcher::StitchEntranceInFrame`), tiles laid out in the tile actor's own frame (`ADungeonActor::bMapInActorFrame`); the POI's dungeon record and gameplay stay in pad space and convert at the boundary. Voxel-carved dungeons hold pawns as on the plane (G24); their walls step by up to √3/2 voxel (the stamp writes air and solid, not graded densities) | `ProceduralDungeonPlugin`, `VoxelWorldPOI` |

### The radius ceiling

Planets are precise up to a radius of about 1,000 km (10^8 uu, `Review_Planet_L`; D17). A float of size R is 8 uu
coarse there, so nothing of size R is kept in float:

| Where | How |
|-------|-----|
| CPU generation and queries | Double from the centre to the altitude: the SDF is the terrain's altitude minus the voxel's (`ShellSignedDistanceAtAltitude`), surface points are centre + direction x (R + altitude) in double, the sea is an altitude |
| Noise | `FVoxelNoise`: a hashed integer lattice (no period) sampled at an origin split in double into a lattice cell and a float fraction, plus a small float offset (D22) |
| GPU generation | In the chunk's frame (`PlanetFrame.ush`): the CPU uploads the chunk centre's direction, distance and altitude and the split noise origins; the shader forms only small values (a voxel's altitude over the anchor's, its change of direction, conditioning chords, the ore key from an integer chunk base) |
| Rendering | Render regions (`FVoxelRenderRegions`, `voxel.Render.RegionChunks`, 16^3 chunks by default): one voxel world component per region and one scatter HISM per type and region, each at its region's origin in double (UE's large-world coordinates); vertices and instances are floats relative to it (D23). The voxel materials' triplanar position is tiled by a whole number of texture periods in LWC math. Every world mode (D40) |
| Collision | Chunk-local trimeshes placed by a double transform |
| The far field | Patches sit at their own origins with float offsets of the patch's size; at scale its sea patches replace the sea sphere, whose vertices were floats of size R (G40) |

Past about 1,000 km the relief clamps (10 km, D24) and the streaming window set the limits, not precision. `WorldOrigin`
plays two roles, the planet's centre (generation, gravity, the surface coordinate) and the chunk grid's origin (chunk
keys, saves, the edit layer); a centre far from the world origin is as precise as one at it.

### Spawn

`UVoxelChunkManager::GetSpawnPosition()` puts `PlanetSpawnAltitude` over the terrain along the configured direction
(`GetSurfacePointAt`). The character spawns by direction (`FVCTerrainQueryHelper::FindSpawnablePosition`): the terrain
along the direction from the centre to the requested position, or, when that is under the sea, the nearest land on
rings of directions one chunk of arc apart; with no land near, a search over the whole planet
(`FindPlanetSpawnPointAtScale`: a 1,000 km planet's pole is open sea for 4.7 km of arc), and with no land at all the
sea's surface, swimming. The configuration's own `UVoxelWorldConfiguration::GetPlanetSpawnPosition` has no world mode,
so it gives the datum's point. `vox.TPLatLon`, `vox.Planet` and `vox.Orbit` are the QA commands.

### Streaming window and culling

A planet streams by chunk (`FDistanceBandLODStrategy::IsInPlanetWindow`; details in `LOD_SYSTEM.md`, "The Window and
Culling (Spherical Planet)"):
- **The horizon along the ground**: chunks load within the view distance of the viewer's ground point, measured along
  the ground (`GetPlanetGroundDistance`), so a climbing viewer keeps its ground disk; LOD bands stay 3D from the viewer.
- **The surface slab**: a chunk loads when its radial extent meets the analytic surface's altitude range over the
  cube-sphere cells its corners span, ± 1.5 chunks; near the viewer also the viewer's altitude ± 2 chunks, up to the
  top of the slab there or the sea's surface, and none for a viewer above them (G52: above them that band is air).
- **The shell**: the terrain's real bounds (`GetDisplacementBounds` within `[R - MaxTerrainDepth, R + MaxTerrainHeight]`)
  widened by half a chunk; nothing outside it loads. With `bSurfaceSlabCulling` off the window is the whole shell
  (`GetPlanetShellWindow`).
- **Horizon culling**: chunks farther than `√(2Rh + h²) + √(2RH + H²)` (h the viewer's altitude, H the terrain's top),
  so peaks past the viewer's own horizon still load; none for a viewer inside the planet.

On `Review_Planet_L` the window holds about 1,100-1,260 chunks at the surface.

### Orbit and back

The far field (`FVoxelFarField` with a planet surface: six cube-face quadtrees, `voxel.World.FarFieldError` 2 px, built
on the thread pool) draws the planet beyond the voxel window, to the horizon and from altitude. Above the voxel cut-over
the window unloads and the far field alone draws the planet; on the way down the window streams back ahead of the
descending camera and fades in, and it resumes only with the camera under the cut-over too (G49). A teleport onto a face
never seen is drawn at once (all six roots are built up front), and the frame of a camera cut draws the far field for
the new position. The far field shows neither caves, overhangs nor edits beyond the window, so they disappear above the
cut-over and return with the window. Its materials blend across its quads as the voxels' dithered biome picks do (G45).
`LOD_SYSTEM.md`, "The Far Field" and "Orbit and Back", has the rules and cvars.

### Limits

- **Radius**: about 1,000 km ("The radius ceiling").
- **Water**: one sea for every consumer (`FVoxelSeaSurface`, `WaterRadius`). On a small planet the sea is a sphere mesh
  (`FVoxelPlanetOcean`), so it also shows inside dry caves below sea level; at scale (from about R 3.3 x 10^5, where the
  sphere would sag past 8 uu) the far field draws the sea over the cells whose analytic terrain lies near or under it.
- **The sun** has no axial tilt: the poles sit in a low sun all day (D12).
- **Cubic planets** are out of scope (D35).

---

## Block worlds: cubic meshing on a heightmap

A cubic world draws and collides with whole blocks: the block at voxel (i, j, k) is the cell [k V, (k + 1) V) above
`WorldOrigin`, solid when the generator gives it a density of at least 127 from the column height H sampled at the
cell's lower corner. The top face of a column's topmost solid block therefore lies between H and H + V, up to a block
above the density isosurface that smooth meshing draws. The game systems use the block tops in a cubic heightmap world
(`FVoxelBlockSurface::UsesBlockSurface`, `UVoxelChunkManager::IsBlockSurface`; D29, D30):

| System | In a block world | Where |
|--------|------------------|-------|
| Surface queries | The generated surface samples the column at its corner and returns the top of the topmost block the generator makes (`FVoxelBlockSurface::BlockTopForHeight`, exact by the generator's own density rule); the edit-merged surface is the top face of the topmost ground voxel with no ground above it (`QueryEditMergedBlockSurface`; the configured voxel trees' leaves are not ground: a canopy is walked under); normal and slope stay the density gradient's. Spawn, surface NPC spawns, navigation, the dungeon entrance stitcher, PCG near points, `vox.TPPOI` and the bench's follow take block tops with no change of their own | `VoxelChunkManager`, `VoxelBlockSurface` |
| Pads | Generation unchanged (a pad zone's target is still the pad height); the pad's walking surface is `SnapToBlockTop(PadAltitude)` (`FPOIPlacement::GetPadSurfaceAltitude`): the claim, the structures and a dungeon's mouth stand there | `VoxelWorldPOI` |
| Steps | Players and NPCs step one block (`UVCMovementComponent::MaxStepHeight` = V + 5, `bStepUpOneBlock`); two blocks are a wall; the camera eases a block step | `VoxelCharacterPlugin` |
| Navigation | One cell per block column on the voxel grid, a step of at most one block (no √2 on diagonals, no corner cut past a pillar), the node budget scaled by the cell area (`FVoxelSurfaceNavParams::bBlockSteps`) | `SURFACE_NAVIGATION.md` |
| Voxel trees | Recomputed from the injector's seeded positions (`FVoxelTreeInjector::FindTreesWithTrunkAt`, `CollectTreeVoxels`; `UVoxelChunkManager::FindVoxelTreeAt`, `GetVoxelTreeBlocks`, `FellVoxelTree`); chopped like a scatter tree, felled to a stump at the third hit | VoxelWorlds, `VoxelCharacterPlugin` |
| Breaking blocks | Timed by the material's `Hardness` and the tool's mining speed, with an outline, a placement ghost and cracks (`UVCBlockInteractionComponent`) | `VoxelCharacterPlugin` |
| The far field | Its patches sample the block tops (`FVoxelFarFieldSurface::SampleAltitude` at a heightmap sample: the column's corner height through `BlockTopForHeight`), so past the window the terrain continues at the blocks' level, a voxel under them, with no step at the window's edge | `VoxelFarField` |

Cubic forces LOD off (`IsLODEnabled()`, D7) and meshes interiors plus face seams only.

---

## Mode parity

The heightmap modes have every planet feature that is not tied to the sphere (world modes review §23, milestones
MP1-MP4, closed 2026-10-26). One code path serves every mode in each row; "heightmap" is InfinitePlane and IslandBowl,
smooth or cubic.

| Feature | Heightmap worlds | Planets | Since |
|---------|------------------|---------|-------|
| Terrain past the view distance | The far field to the horizon: a quadtree per root tile on the XY grid (`FVoxelFarFieldPatchKey`), the island's over its bounds with the far ocean beyond (D37), the sea at `WaterLevel`, a cubic world's patches at the block tops; `voxel.World.FarField`, `voxel.World.FarFieldDistance` (2,000,000 uu), `voxel.World.FarFieldHeightmapError` (4 px) | Six cube-face quadtrees (`voxel.World.FarFieldError` 2 px) | MP3 (WM-MP1, D36) |
| The voxel window at altitude | Suspended above three view distances over the ground or the sea under the streaming viewer, only while a far field is on (D38, D41); resumed under 0.85 of it with the camera under it (G49) | The same (`VoxelCutoverAltitude`, `voxel.Stream.VoxelCutover`), the far-field gate included (G78) | MP3 (WM-MP2) |
| Streaming ahead of a fall | The descent prefetch leads the streaming viewer under a descending camera (`voxel.Stream.DescentLead`) | The same | MP1 (WM-MP3) |
| The frame of a teleport | A second streaming pass for the frame's camera after a cut of more than a chunk (`UpdateStreamingForCamera`) | The same, and the far field's second update (F4.3) | MP1 (WM-MP4) |
| Precision far from the origin | Render regions and scatter regions (`voxel.Render.RegionChunks` 16, `voxel.Scatter.RegionChunks`): no float of the world's size | The same | MP2 (WM-MP5, D40) |
| Noise | `FVoxelNoise` / `VoxelNoise.ush` for every field: a hashed split-origin lattice (no period) and the continuous Simplex kernel (G43); `GenerationVersion` 3 | The same (`PlanetGenerationVersion` 6) | MP4 (WM-MP6, WM-MP7, D39) |
| The near layers | Clipped to the terrain's height bounds (D8): a climbing viewer loads no air above them | Capped at the surface slab's top (G52) | MP1 (WM-MP8, no change needed) |
| The world map at scale | The marker cap, the tile-redraw throttle and `voxel.Map.ExploreAllMaxTiles` | The same | MP1 (WM-MP9) |
| The first spawn over the sea | The ring search, then a coarse spiral over the generated surface (`FindPlaneSpawnPointAtScale`) | `FindPlanetSpawnPointAtScale` | MP1 (WM-MP10) |
| Altitude QA tooling | `vox.FlyTo`, `vox.Orbit` (a look-down camera over a point), `vox.World`, the bench's `climb=X:Y[:Alt]` and `descend=X:Y:Alt` | `vox.Planet`, `climb=Lat:Lon`, `descend=Lat:Lon:Alt` | MP3 (WM-MP11) |

The exit check passed on `VoxelDemo`, island M and `Review_Cubic`: each suspends the window once on a
`vox.FlyTo 150000` and resumes it once on the way down, lands walking with no fall-through, and draws the terrain to the
horizon with no stand-in while still; on island M's and the cubic world's chord the far field costs 1.0-1.6 ms of
frame P95.

What stays planet-only is the sphere itself: gravity toward the centre and the gravity frame, the cube-sphere address of
chunks, map tiles and PCG, pad space for POIs and dungeons, orbit cameras around the centre, the sea as a radius, and the
local solar clock.

---

## GPU Implementation

`Shaders/Private/GenerateVoxelDensity.usf` mirrors the CPU generator for every mode (`GPU_PIPELINE.md`):

- **InfinitePlane:** the terrain noise at the chunk's noise origin plus the voxel's offset, continentalness,
  `NoiseToTerrainHeight` (`WorldModeSDF.ush`), the conditioning zones, then the signed distance to that height. Then, in
  the CPU's order, the biome blend, the caves (`GpuCaveCarve`), materials, height rules, the cave-wall override and
  ores.
- **IslandBowl:** the same height, then `ApplyIslandFalloffToHeight` (`IslandFalloff.ush`, the port of
  `FIslandBowlWorldMode::ComposeTerrainHeight`): the falloff toward `EdgeHeight` inside the island's extent, the seabed
  beyond it.
- **SphericalPlanet** (`SphericalPlanet.ush`, in the chunk's frame): `GenerateChunkSphericalPlanet` per voxel, in the
  same order; the post-passes run along local down and flood inside the sea sphere.

The `VoxelWorlds.Generation.GPUvsCPU*Parity` tests pin each mode and feature (materials, the blend dither, the island
with its seabed, caves with biome overrides and the sea's minimum depth, the four noise types, water, ore variants, the
planet). They need a real RHI: run them in the editor.

---

## Switching World Modes

The world mode is read once, when the world actor initialises its chunk manager (`AVoxelWorldActor::InitializeVoxelWorld`);
changing it means reinitialising the world. A runtime configuration override, `-VoxelConfig=<path>` on the command line
or `voxel.World.ConfigOverride`, replaces the placed configuration at initialisation, so a review or benchmark can
switch world mode or meshing mode without editing the map; its saves are kept apart (the save key carries the
override configuration's name).

---

## Performance

On the demo's geometry (voxel 75 uu, 64-voxel chunks, view distance 40,000 uu) a plane holds about 1,000 chunks
settled at the spawn and 1,100-1,200 at peak while streaming; an island streams the same window over its footprint
(a small island, 0.8 km across, is wholly resident with about 1,000); a cubic world holds about 1,100-1,250 at full
detail (LOD off); a planet's window holds 1,100-1,260 chunks at any radius. Peak voxel-systems memory stays under the
1.5 GB budget (D3) in every mode. Benchmarks: `voxel.Bench.Run` (`STREAMING_AND_SEAMS.md`).
