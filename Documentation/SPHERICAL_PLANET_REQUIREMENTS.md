# Spherical planet requirements

Spherical mode (`EWorldMode::SphericalPlanet`) has been reserved since the GPU generation epic. This document
collects what the docs already say about it and what the code actually does, then lists what is needed to bring
it to the level of InfinitePlane and IslandBowl. It is a requirements list, not an implementation plan. The
plan is written after the scope decisions in §2 are taken and the cubic and island reviews
([WORLD_MODES_REVIEW_PLAN.md](WORLD_MODES_REVIEW_PLAN.md)) have reported, since several requirements share work
with island fixes (the world-bounds query, mode-aware POI sampling, the save-key fingerprint).

**Sources:**

- The docs: `Features/WORLD_MODES.md` "Spherical Planet — GPU / Analytic Gap Analysis" and its five reserved
  gaps, `LOD_SYSTEM.md`, `MAP_SYSTEM.md`, `WATER_SYSTEM.md`, `CAVE_SYSTEM.md`, `SCATTER_SYSTEM.md`,
  `Research/GEOMORPH_IMPLEMENTATION_PLAN.md`, `Research/ENGINE_INTEGRATION.md`,
  `WorldClaims/Documentation/CLAIMS_ARCHITECTURE.md`.
- A code survey on 2026-10-12.

File references are relative to `Plugins/` unless noted; line numbers are as of VoxelWorlds `a85b8c6`
(on `cc706f3` and later, `VoxelChunkManager.cpp` lines after 1650 are 16 higher). Distances
are in Unreal units (1 uu = 1 cm); the default `WorldRadius` of 100,000 uu is a 1 km planet.

**Priorities:**

- **P0:** needed for a correct, walkable planet.
- **P1:** needed for parity with the other modes (the game systems work on it).
- **P2:** later or optional.

## 1. Where spherical mode stands

What exists:

- `FSphericalPlanetWorldMode` (`VoxelWorlds/Source/VoxelGeneration/{Public,Private}/SphericalPlanetWorldMode.*`).
  It provides a radial SDF, inner and outer shell radii, a radial-depth material helper and Cartesian chunk
  transforms centred on the planet.
- A CPU generator, `GenerateChunkSphericalPlanet` (`VoxelCPUNoiseGenerator.cpp:1311-1532`). It handles radial
  depth, biomes and climate sampled by 3D direction, caves and ores by radial depth, and underwater materials
  against `WaterRadius`.
- Inner-shell, outer-shell and horizon culling in `DistanceBandLODStrategy.cpp:1074-1144`, also used for unloading.
- An analytic world mode built per config by the chunk manager (`VoxelChunkManager.cpp:975-985`).
- `GetPlanetSpawnPosition` on the config, and a visual water sphere on the test actor.

What is wrong or missing:

| # | Problem | Evidence |
|---|---------|----------|
| 1 | **GPU generation has no planet support and is on by default.** Spherical falls through to the generic 3D-noise branch, which produces blobs instead of a planet. Nothing forces CPU generation for this mode. | `Shaders/Private/GenerateVoxelDensity.usf:327-346`; `WorldModeSDF.ush:190-193` (placeholder only); `VoxelChunkManager.cpp:1003-1004`; `bUseGPUGeneration` defaults to true. **Guarded 2026-10-14 (SP-B3):** planets generate on the CPU and log why |
| 2 | **The terrain is mirrored across the equator.** The noise samples `FBM(Direction.xy × 10000, 0)`, so z and −z get the same height, and features stretch north to south near the equator. The 10,000 factor is hard-coded, so feature size scales with the radius. `NoiseScale` is never read. | `SphericalPlanetWorldMode.cpp:165-181` |
| 3 | **Every surface turns to snow.** Height-material rules are given the distance from the centre as "height", and the default rules put snow at ≥ 4,000. | `VoxelCPUNoiseGenerator.cpp:1458`; `VoxelBiomeConfiguration.cpp:111-132` |
| 4 | **The analytic surface query is wrong.** `GetTerrainHeightAt(X, Y)` returns a radius, not a Z, and only for the equatorial direction (X, Y, 0). Every consumer reads it as a Z height. | `SphericalPlanetWorldMode.cpp:54-77`; `IVoxelWorldMode.h:54-68` |
| 5 | **No water flags.** The water-fill pass is skipped for spherical, while the underground classification pass still scans world-Z columns. | `VoxelCPUNoiseGenerator.cpp:1553-1555`, `:1686-1747` |
| 6 | **The chunk candidate set is an axis-aligned box** (XY radius × a Z range capped at ±16 chunks). At the equator, world Z is horizontal, so reach along it is cut short. | `DistanceBandLODStrategy.cpp:192-211` |
| 7 | **The horizon cull under-buffers distant mountains.** It adds `PlanetMaxTerrainHeight` where `√(2R·Hmax + Hmax²)` is needed. | `DistanceBandLODStrategy.cpp:1074-1144` |
| 8 | **Config hygiene.** `BaseHeight` is hidden in the UI for spherical but still used. Nothing checks that `PlanetMaxTerrainHeight`/`Depth` cover the real amplitude (`0.5·PlanetHeightScale + BaseHeight`), so terrain past the shell is clipped by the density quick-reject. `GetPlanetSpawnPosition` ignores terrain displacement, and no spawn code calls it. **Partly fixed 2026-10-14 (SP-A5):** `BaseHeight` is shown, and the shell and water checks warn at load; the spawn clauses stay with SP-B11 and SP-F4 | `VoxelWorldConfiguration.h:99-191`; `VoxelWorldConfiguration.cpp:233-259` |
| 9 | **No tests, config or map.** No automation test or content asset uses SphericalPlanet. **Partly fixed 2026-10-14:** a planet config and test map exist (SP-I1) and `VoxelWorlds.Configuration.PlanetSettings` tests the config checks; the generation tests are SP-I2 | `*/Tests/*`, `Content/` |
| 10 | **Every system that reads the surface assumes +Z is up**: scatter, PCG, map, navigation, POIs, dungeons, campsites, NPCs, movement, camera, world items, water, sky, clock. See Appendix A. | Appendix A |

Several docs overstate this state; they are listed in §10 of the review plan.

## 2. Scope decisions

**Decided 2026-10-12: all five recommendations accepted.** Small planets first, Cartesian chunks with a
cube-sphere surface parameterisation for 2D systems, smooth meshing only, game systems in the order given, surface
play only.

| ID | Decision | Options | Recommendation (accepted) |
|----|----------|---------|---------------------------|
| SD1 | **Planet size** | Small, 1-10 km radius (100,000-1,000,000 uu): you can walk around it, the horizon is close, float precision is ample. Large, 50-100 km and up: at the float vertex ceiling, needs double-precision SDF math, origin rebasing and an orbital LOD | **Small first.** On a 10 km planet the eye-height horizon is ~20,000 uu, half the demo view distance, so the horizon cull rather than the view distance bounds what loads. Chunk counts should be no worse than a plane's (to be measured). Large planets become a P2 track |
| SD2 | **Chunk topology** | Keep the Cartesian chunk grid centred on the planet, or move to cube-sphere (warped) chunks | **Keep Cartesian.** Generation, meshing, seams, collision, fades and compression are orientation-agnostic and already work there. Add a cube-sphere *surface parameterisation* only for the 2D systems (map tiles, nav grids, POI cells, water tiles, explored-tile keys), which today key on XY and merge the two hemispheres |
| SD3 | **Meshing modes on planets** | Smooth only, or smooth and cubic | **Smooth only.** Cubic voxels on a sphere form terraces everywhere except near the six points where the world axes meet the surface. With 75-100 uu blocks and a 50 uu step height, every step there is a jump, which makes cubic planets effectively unwalkable. Cubic planets would need a cube-sphere voxel grid (SD2's rejected option) |
| SD4 | **Which game systems must work on a planet** | All (POIs, dungeons, campsites, NPCs, map, nav, save, clock), or a first subset | **Terrain, character, scatter, water, map, nav, NPCs, save, clock and sky first; POIs and campsites next; dungeons last** (they need rotated stamping on an axis-aligned voxel grid) |
| SD5 | **Space and orbit** | Surface-only, or seamless space-to-surface | **Surface-only.** An orbital LOD (quadtree on a cube-sphere, or octree) and distant-planet impostors are P2 |

The priorities below follow these decisions. Revisiting SD1 (large planets) would move SP-H1 to P0;
revisiting SD3 (cubic planets) would bring SP-D5 in.

## 3. Requirements

### A. World-mode API (the foundation)

The reserved gap analysis names this as gap 1. Every other consumer needs it.

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-A1 | `IVoxelWorldMode` exposes a local frame. `GetUpVector(WorldPos)` returns +Z for heightmap modes and the radial direction for planets. `GetAltitude(WorldPos)` returns the signed height above the datum (sea level). `GetSurfacePoint(Direction)` returns the surface point plus its normal | P0 | Only `GetTerrainHeightAt(X, Y) → Z` |
| SP-A2 | `IVoxelWorldMode` exposes a 2D surface parameterisation. `WorldToSurface(WorldPos) → (Face, U, V)` and its inverse are a cube-sphere mapping for planets and `(0, X, Y)` for heightmap modes. This is the key for every 2D consumer | P0 | 2D systems key on XY |
| SP-A3 | Callers migrate from `GetTerrainHeightAt` to SP-A1/SP-A2 (the list is Appendix A). On a planet, `GetTerrainHeightAt` logs once and returns a world Z on the upper hemisphere, so legacy callers degrade instead of reading a radius as a Z | P1 | Returns a radius |
| SP-A4 | `GetTerrainHeightBounds` generalises to altitude bounds; the planet version is the radial shell | P1 | Z extent; the planet uses the ±1e9 default |
| SP-A5 | Config validation and honest UI: `BaseHeight` is either shown or not used. The shell (`PlanetMaxTerrainHeight`/`Depth`) is checked against the real amplitude. `NoiseScale` is used or removed. Using `WaterLevel` on a planet logs a warning | P0 | **Done 2026-10-14** (WM-H10, VoxelWorlds#87): `UVoxelWorldConfiguration::GetSettingsWarnings` checks the shell against `BaseHeight ± PlanetHeightScale/2` and flags water on a planet (logged at load and at world start); `BaseHeight` is shown; the unread `NoiseScale` is removed |
| SP-A6 | One builder for planet params. They are rebuilt in several places today: the chunk manager, the map and the editor field viewer | P1 | `VoxelChunkManager.cpp:975-985`, `:2510-2519`; `VoxelMapSubsystem.cpp:70`; `VoxelFieldTypes.cpp:69` |

### B. Generation

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-B1 | Seamless noise sampled by 3D direction, `FBM3D(Direction × R_noise)`, with feature size set in world units and not tied to the radius | P0 | Mirrored 2D sampling (problem #2) |
| SP-B2 | Height-material rules use altitude above the datum | P0 | Uses the radius; everything turns to snow (problem #3) |
| SP-B3 | GPU guard: until SP-B4 lands, spherical worlds use CPU generation and log why | P0 | **Done 2026-10-14** (WM-H10, VoxelWorlds#87): `ShouldUseGPUGeneration()` is false for planets; the chunk manager logs a warning when `bUseGPUGeneration` is on |
| SP-B4 | GPU radial SDF in `GenerateVoxelDensity.usf`: planet params uploaded; density = `(R + displacement) − |P − C|`; noise by direction; biomes, materials, caves and ores matching the CPU. Locked by a `GPUvsCPUSphericalParity` test at LOD 0/1/2 | P1 | Generic noise branch |
| SP-B5 | Continentalness modulates radial height the way it does in heightmap modes | P1 | Used for biome selection only (`VoxelCPUNoiseGenerator.cpp:1386-1392`) |
| SP-B6 | Caves: `VerticalScale` acts along local up, and the `bIsUnderwater` suppression is passed | P1 | Squashes along world Z (`:1071`); not passed (`:1437`) |
| SP-B7 | Underground classification scans along local up, on both CPU and GPU | P1 | World-Z column scans (`:1700-1747`; `VoxelPostPasses.usf:182-197`) |
| SP-B8 | Water flags by sea radius (a radial fill pass), plus edit-time water propagation by radius | P1 | Fill skipped; propagation compares world Z (`VoxelWaterPropagation.cpp:76`, `:237`) |
| SP-B9 | Voxel-tree injection is gated off on planets (it only applies to cubic, which SD3 excludes) | P1 | Not gated by world mode (`VoxelChunkManager.cpp:2671-2675`) |
| SP-B10 | Conditioning zones get a radial model (footprint on the surface, target altitude), so claims, POI pads and campsites can flatten ground | P1 (P2 if SD4 defers POIs) | Skipped on planets (`VoxelCPUNoiseGenerator.cpp:1400-1402`; `CLAIMS_ARCHITECTURE.md:218-225`) |
| SP-B11 | Spawn helper: `GetPlanetSpawnPosition` (or its replacement) includes the terrain displacement at the chosen direction | P1 | Ignores displacement (up to +2,500 uu by default against a 500 uu spawn altitude) |

### C. Streaming and LOD

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-C1 | Candidate enumeration covers every chunk that intersects the shell within the view distance, at any latitude | P0 | Axis-aligned box with a ±16-chunk Z cap (problem #6). Until 2026-10-13 the vertical window also added the viewer's layer twice, which would have centred a pole viewer's window near layer 40, above the surface; fixed in the review plan's §13.11. The review plan's D8 makes heightmap windows a per-column interval supplied by the world mode (WM-S1); the planet version of that interval is the shell around the surface |
| SP-C2 | Horizon cull adds `√(2R·Hmax + Hmax²)`, so mountains beyond the horizon still load | P1 | Under-buffered (problem #7) |
| SP-C3 | LOD distance stays 3D Euclidean (fine for surface play); `LOD_SYSTEM.md` stops claiming geodesic distance | P1 | Doc mismatch |
| SP-C4 | The collision cook lead uses local down and the tangential heading | P1 | −Z and a horizontal heading (`VoxelCollisionManager.cpp:403-430`) |
| SP-C5 | A planet equivalent of the far-band surface slab: far columns load only the altitude band around the analytic surface | P2 | Heightmap modes only |
| SP-C6 | Orbital view: a quadtree-on-cube-sphere or octree LOD, plus a distant-planet impostor | P2 | Not started (SD5) |

### D. Meshing and rendering

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-D1 | The marching-cubes LOD material and biome scan runs along local up | P1 | Scans +Z (`VoxelCPUMarchingCubesMesher.cpp:1296-1376`) |
| SP-D2 | Geomorph runs along local up, or is disabled on planets | P1 | Morphs along Z, gated on \|Nz\|, so there is none at the equator (`:1116-1142`; `MarchingCubesMeshGeneration.usf:446-457`) |
| SP-D3 | Material face classification (top, side, bottom) uses local up | P1 | World +Z (`VoxelMaterialCommon.ush:91-104`, `:268`) |
| SP-D4 | Verified on a planet at mid latitudes (45°), where chunks straddle the surface diagonally: seam closure, crossfade, appear fade and collision winding | P0 | Expected to work (orientation-agnostic); untested |
| SP-D5 | Cubic planets | P2 | Only if SD3 changes; needs a cube-sphere voxel grid |

### E. Water

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-E1 | One sea-radius definition used by every water consumer: scatter underwater class, nav water check, NPC spawner, spawn search, swimming | P1 | Consumers read the Z-plane `WaterLevel` (default 0, so the whole southern hemisphere counts as underwater for scatter) |
| SP-E2 | Ocean surface: a sphere shell with the water material; per-chunk Z-plane water tiles are off on planets | P1 | Static engine sphere; tiles still run if `WaterMeshMaterial` is set |
| SP-E3 | Underwater camera and post-process by radial depth | P1 | Z-based |

### F. Character and gameplay (game side)

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-F1 | Gravity toward the planet centre: `UVCMovementComponent` sets the CMC gravity direction (UE 5.4+ custom gravity) each tick | P0 | World −Z; no gravity-direction code anywhere |
| SP-F2 | `UVCMovementComponent` Z uses become gravity-relative: swim and buoyancy, floor trace, ledge check, embed resolve | P0 | `VCMovementComponent.cpp:242-295`, `:440`, `:769-791`, `:930-971` |
| SP-F3 | Camera and control rotation are relative to gravity | P0 | World yaw and pitch (`VCCharacterBase.cpp:1632-1645`) |
| SP-F4 | Spawn and the settle hold work by direction. A `vox.TP` equivalent takes latitude and longitude | P0 | Heightmap search; works only at the north pole, by coincidence |
| SP-F5 | KillZ is off (or radial) on planets. The world-item pool parks items away from the surface | P0 | The default KillZ (~−1,048,575 uu) cuts through planets over ~10 km radius. The pool parks at (0, 0, −100,000), which is the south-pole surface of the default planet (`WorldItemPoolSubsystem.cpp:144`) |
| SP-F6 | NPC AI movement and auto-jump are gravity-relative | P1 | `VCNPCAIController.cpp:697-715`, `:802` |
| SP-F7 | World-item drop traces follow gravity; the placeable slope test uses local up | P1 | ±Z traces (`WorldItem.cpp:186-195`); `acos(Normal.Z)` (`ItemFragment_Placeable.h:39-42`) |
| SP-F8 | The cylinder edit brush uses local up as its axis | P1 | Z axis (`VoxelEditManager.cpp:299-320`) |

### G. Surface systems

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-G1 | Scatter: surface extraction from mesh normals instead of world-Z column scans; slope against local up, elevation filters by altitude, instances aligned to local up, underwater by sea radius | P1 | Z columns, `Normal.Z` face types, world-Z elevation, world-up yaw (`VoxelScatterManager.cpp:2487-2770`; `VoxelScatterTypes.h:110-126`, `:235-249`, `:318-324`) |
| SP-G2 | The PCG surface sampler emits points on a surface-parameter grid, oriented to local up | P1 | XY grid, `MakeFromZ` (`PCGVoxelSurfaceSampler.cpp:208-286`) |
| SP-G3 | Map: tiles keyed by surface parameters; a world-map view (unfolded cube or globe); a minimap in the local tangent frame; explored tiles saved by surface key | P1 | Top-down XY tiles that merge the hemispheres (`VoxelMapSubsystem.cpp:420-431`; `VoxelWorldSaveTypes.h:75-83`) |
| SP-G4 | Surface navigation on a local tangent-frame grid keyed by surface parameters (navigation range is local, so a per-request tangent grid is enough) | P1 | 2.5D XY grid, one height per column |
| SP-G5 | POI placement on surface cells with the site survey in the tangent frame; campsites and footprint levelling in the tangent frame (needs SP-B10) | P1 | XY cells and InfinitePlane heights, so POIs land inside the planet (`POIPlacement.cpp:55-58`) |
| SP-G6 | Dungeons stamped in a rotated tangent frame (rotation in `DungeonVoxelStamper`, a tilted entrance stitcher) | P2 | Translation-only stamping; the stitcher sweeps Z from +10,000 to −10,000 and misses a planet's surface |
| SP-G7 | World clock and sun: a single directional light fixed in space and a rotating planet give day and night sides naturally. The clock drives the planet rotation angle, and the HUD and NPC schedules use local solar time at the player | P1 | One global sun pitch and yaw in the world frame (`WorldClockMath.cpp:48-59`) |
| SP-G8 | Sky: SkyAtmosphere centred on the planet (bottom radius = sea radius) instead of Z-height exponential fog | P1 | `DayNightController.cpp:80-90`, `:155-160` |
| SP-G9 | Save: the key or header fingerprints world mode and planet params (shared with island KI7); explored tiles are keyed by surface parameters | P1 | Map + seed only |
| SP-G10 | WorldClaims use the radial zone model from SP-B10. World Partition stays deferred for planets | P2 | Claims are oriented boxes (fine); conditioning is not applied |

### H. Precision and scale

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-H1 | Planet-relative double math in the CPU SDF; planet-relative or chunk-relative coordinates in the GPU SDF | P2 (P0 if SD1 = large) | Float: cancellation error ≈ ULP(R), ~1 uu at R = 10⁷ and ~64 uu at Earth scale |
| SP-H2 | Document the radius ceiling (~100 km with `FVector3f` vertex baking and no origin rebasing). If rebasing is ever added, separate the planet centre from the chunk-grid origin; `WorldOrigin` plays both roles today | P1 | Undocumented |
| SP-H3 | The ore `Rarity` position hash uses planet-relative coordinates | P1 | `sin(pos·k)` loses precision at large \|pos\| |

### I. Tooling and tests

| ID | Requirement | Pri | Today |
|----|-------------|-----|-------|
| SP-I1 | A planet config and test map (`PluginTesting/Planet`), derived from the demo config so biomes, caves and POI tables match | P0 | **Done 2026-10-14** (WM-H10, UE5_PluginDev#102): `PlanetWorldConfig` (a 2.5 km planet) and `PlanetTest` (VoxelDemo on it, PlayerStart over the north pole), from `Scripts/setup_planet_test.py` |
| SP-I2 | Automation tests: analytic vs generated surface by direction; seamless noise (the hemispheres differ, no mirror, continuous across cube-face edges); shell and horizon cull; enumeration covers the shell at the equator; params validation | P0 | None |
| SP-I3 | `GPUvsCPUSphericalParity` (with SP-B4) | P1 | None |
| SP-I4 | A bench great-circle route at fixed altitude (extends review-plan T2) | P1 | Straight +X line only |
| SP-I5 | Debug commands: `vox.TPLatLon`, `vox.Planet` (radius, shell, horizon, viewer altitude and latitude) | P1 | None |

## 4. Suggested phasing (input to the implementation plan)

| Phase | Content | Exit check |
|-------|---------|------------|
| 0. Guardrails | SP-A5, SP-B3, SP-I1, doc corrections | A spherical config generates on the CPU with a warning and no clipping. **Met 2026-10-14** (review plan §14.5): `PlanetTest` generates on the CPU with the SP-B3 warning, no config warnings, terrain inside its shell |
| 1. A correct planet (CPU) | SP-A1, SP-A2, SP-B1, SP-B2, SP-B5-B8, SP-C1, SP-C2, SP-D4, SP-I2 | A survey camera shows a seamless, unmirrored planet with sensible materials and water at all latitudes |
| 2. GPU | SP-B4, SP-I3 | GPU parity green; GPU default re-enabled for spherical |
| 3. Walkable | SP-F1-F5, SP-C4, SP-B11, SP-E1-E3 | Walk and swim around the planet with no fall-through (`vox.Debug.Capture`) |
| 4. Systems | SP-A3, SP-A4, SP-A6, SP-D1-D3, SP-F6-F8, SP-G1-G4, SP-G7-G9, SP-H2, SP-H3, SP-I4, SP-I5 | The QA plan's world, NPC, map and save rows pass on the planet |
| 5. Places | SP-B10, SP-G5 (then SP-G6, SP-G10) | POIs and campsites sit on the surface; dungeons if SD4 includes them |
| 6. Big planets and orbit (optional) | SP-H1, SP-C5, SP-C6 | Per SD1 and SD5 |

Phases 3 and 4 depend on the review: the world-bounds query, mode-aware POI sampling and the save-key fingerprint
from the island findings should be designed so the planet versions slot into the same interfaces.

**Scheduled 2026-10-14** (review plan [§14.4](WORLD_MODES_REVIEW_PLAN.md), decision D9): Phase 0 is work item
WM-H10 in milestone M1. The heightmap side of SP-A1 and SP-A2, plus SP-A6 and SP-G9, are built with the island's
world-mode API in M2 (WM-A1, WM-A2, WM-A4), so Phase 1 adds the planet implementations. Phase 1 starts after M4 (the
per-column streaming window that SP-C1 builds on) and M5 (the legacy whole-chunk path is deleted before SP-D4 tests
seams at mid latitudes).

## Appendix A. Z-up audit

Every site found in the 2026-10-12 survey that assumes "up = +Z" or a heightmap `Z(X, Y)`.

Severity:

- **a:** breaks on a sphere.
- **b:** needs a local tangent or radial-up frame.
- **c:** fine.

### VoxelWorlds

| System | Where | Assumption | Sev | Req |
|--------|-------|------------|-----|-----|
| Analytic height | `SphericalPlanetWorldMode.cpp:54-77`; `IVoxelWorldMode.h:65` | `(X, Y) → Z` contract; the planet returns a radius | a | A1, A3 |
| Generated surface height | `VoxelChunkManager.cpp:2608-2632` | Z from `GetTerrainHeightAt`; XY conditioning | a | A1 |
| Edit-merged surface query | `VoxelChunkManager.cpp:1647-1719` | Vertical column at (X, Y); normal flipped to +Z; slope = `acos(N.Z)` | a | A1 |
| `FVoxelSurfaceQuery` | `VoxelSurfaceQuery.cpp:17-201` | Heightfield gradient; normal `(−dx, −dy, 1)`; biome at (X, Y, 0) | a | A1 |
| Conditioning zones | `VoxelTerrainConditioning.h:26-116`; skipped in gen `:1400-1402` | XY footprint + target Z | a | B10 |
| Underground classification | `VoxelCPUNoiseGenerator.cpp:1700-1747`; `VoxelPostPasses.usf:182-197` | Top-down −Z column | a | B7 |
| Water fill, propagation, tiles, mesher | gen `:1546-1682`; `VoxelChunkManager.cpp:5295-5511`; `VoxelWaterPropagation.cpp:76`, `:237`; `VoxelWaterMesher.cpp:121` | `WaterLevel` is a Z plane; XY tiles | a | B8, E1, E2 |
| Cave shape | gen `:1071`; `GenerateVoxelDensity.usf:233` | `VerticalScale` on world Z | b | B6 |
| Cave depth fade, ore depth, ore streaks | gen `:1187-1217`, `:1243-1259`, `:1473-1483` | Radial depth on the CPU | c | — |
| Height-material rules | gen `:1458` | Radius used as height (all snow) | a | B2 |
| Biome and climate sampling | gen `:1426-1430` (3D direction) vs `VoxelSurfaceQuery.cpp:93`, map `:722` (X, Y, 0) | Analytic paths use XY | c gen / a analytic | A1, B5 |
| Tree injection | `VoxelTreeInjector.cpp:48`, `:191-229`, `:292`, `:343` | Heightmap Z, vertical trunk | a | B9 |
| Scatter extraction (smooth, cubic) | `VoxelScatterManager.cpp:2630-2770`, `:2802-2918` | Z-column scan; face type by `Normal.Z`; cubic normal = UpVector | a | G1 |
| Scatter underground override, underwater | `VoxelScatterManager.cpp:2487-2531`, `:2565`, `:2948-2949` | `Normal.Z > 0.3`; `Z < WaterLevel` | a | G1, E1 |
| Scatter slope, elevation, rotation | `VoxelScatterTypes.h:110-126`, `:235-249`, `:318-324` | World up; world-Z elevation; yaw about world up | b | G1 |
| Scatter cover check | `VoxelScatterManager.cpp:2539-2547` | Guarded by `IsHeightmapBased()` | c | — |
| PCG sampler | `PCGVoxelSurfaceSampler.cpp:208-286`, `:110-111` | XY grid, `MakeFromZ`, `WaterLevel` | a | G2 |
| Map tiles and exploration | `VoxelMapSubsystem.cpp:420-431`, `:669-692`, `:737`; `VoxelMapSubsystem.h:59-83` | Top-down XY; height = radius; hemispheres merge | a | G3 |
| Surface navigation | `VoxelSurfaceNavigationSubsystem.cpp:192-367` | 2.5D XY cells, one height per column | a | G4 |
| MC LOD material scan | `VoxelCPUMarchingCubesMesher.cpp:1296-1376` | Scan +Z | b | D1 |
| MC geomorph | `VoxelCPUMarchingCubesMesher.cpp:1116-1142`; `MarchingCubesMeshGeneration.usf:446-457` | Morph along Z, gated on \|Nz\| | b | D2 |
| Face type (atlas, cubic) | `VoxelCPUCubicMesher.cpp:119`; `VoxelMaterialCommon.ush:91-104`, `:268` | Top = +Z | b | D3 |
| Edit brushes | `VoxelEditManager.cpp:299-320` | Cylinder axis = Z (sphere and cube fine) | b | F8 |
| Collision cook lead | `VoxelCollisionManager.cpp:403-430` | Below feet = −Z; horizontal heading | b | C4 |
| LOD candidates and culling | `DistanceBandLODStrategy.cpp:192-211`, `:1074-1144` | Shell cull fine; Z range capped at ±16 | b | C1, C2 |
| Collision trimesh, uniform compression, claims volumes | per-chunk mesh; `ChunkDescriptor.h:316-337`; `WorldClaimComponent.cpp:29` | Orientation-agnostic | c | — |
| Editor field viewer | `VoxelFieldTypes.cpp:67-76`, `:268-289` | XY height and slope fields | a | A6 |

### Game side

| System | Where | Assumption | Sev | Req |
|--------|-------|------------|-----|-----|
| Gravity | `VoxelCharacterPlugin` (no gravity-direction code) | World −Z | a | F1 |
| Movement | `VCMovementComponent.cpp:242-295`, `:440`, `:769-791`, `:930-971` | Buoyancy, floor trace, ledges, embed resolve on Z | b | F2 |
| Camera and input | `VCCharacterBase.cpp:1632-1645` | World yaw/pitch | b | F3 |
| Spawn and settle | `VCVoxelNavigationHelper.cpp:173-240`; `VCCharacterBase.cpp:838-1000` | Heightmap spiral; ±Z sweeps | a | F4 |
| Water depth | `VCVoxelNavigationHelper.cpp:104-163` | `WaterLevel + Origin.Z − Loc.Z` | a | E1 |
| NPC AI | `VCNPCAIController.cpp:697-715`, `:802` | Feet Z and rise; rotation about UpVector | b | F6 |
| POI placement | `POIPlacement.cpp:55-178`; `POIPlacementSubsystem.cpp:151-181`, `:282-293`, `:747-769` | InfinitePlane heights + XY cells | a | G5 |
| Dungeon stamping | `POIPlacementSubsystem.cpp:989-1008`; `DungeonVoxelStamper.h:59-68` | Translation-only, vertical = Z | b | G6 |
| Dungeon entrance stitcher | `DungeonEntranceStitcher.cpp:49-79` | Z sweep ±10,000 | a | G6 |
| Campsites and footprint levelling | `POIPlacementLibrary.cpp:74-128`; `POICampsite.cpp:128` | XY disk, heightmap cut-and-fill, yaw only | a | G5, B10 |
| Surface NPC spawner | `SurfaceNPCSubsystem.cpp:299-349`, `:443`, `:654-710` | XY surface query; −Z collision probe | a | F6, G4 |
| World clock and sun | `WorldClockMath.cpp:48-59`; `DayNightController.cpp:142-149` | One world-frame sun direction | a | G7 |
| Sky and fog | `DayNightController.cpp:80-90`, `:155-160` | Z-height fog | a | G8 |
| World items | `WorldItem.cpp:186-220`; `WorldItemPoolSubsystem.cpp:144` | ±Z drop trace; pool parked on the south pole | a/b | F5, F7 |
| Placeable slope | `ItemFragment_Placeable.h:39-42` | `acos(Normal.Z)` | b | F7 |
| Debug teleport | `Source/VoxelEngine/Private/Debug/VoxelDebugSubsystem.cpp:1765-1786` | Heightmap settle | a | F4, I5 |
