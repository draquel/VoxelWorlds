# World modes review plan (cubic, island, spherical)

VoxelWorlds has been built and tuned almost entirely on one configuration: an InfinitePlane world with smooth
meshing (Dual Contouring in the demo). This plan sets up a review of the two other configurations that are
meant to be supported, and records what spherical mode still needs:

- **Cubic meshing:** does it still work end to end after the seam-ownership refactor and the R22-R24 work? How
  does it stream, and does it work with the game plugins?
- **Island mode (`EWorldMode::IslandBowl`):** can it hold a large but finite world with every other system
  running (scatter, PCG, map, navigation, POIs, dungeons, NPCs, campsites, save, clock, water) at a performance
  comparable to InfinitePlane?
- **Spherical mode (`EWorldMode::SphericalPlanet`):** the requirements are in
  [SPHERICAL_PLANET_REQUIREMENTS.md](SPHERICAL_PLANET_REQUIREMENTS.md). This plan only summarises them (§7).

The review runs the scenarios in §4 and §5 and records results against the acceptance criteria in §1. A second
revision of this document will then turn the confirmed findings into work items.

File references are relative to `Plugins/` unless noted. Line numbers are as of VoxelWorlds `a85b8c6` and
UE5_PluginDev `2f3e6e0`. The R22 map-overlay merge that followed (VoxelWorlds `cc706f3`) added
`voxel.Map.Capture` and moved `VoxelChunkManager.cpp` lines after 1650 down by 16; other files are unchanged.
Distances are in Unreal units (1 uu = 1 cm). The demo uses voxel size 75, chunk size 64 (4,800 uu per chunk), view distance
40,000 uu and `WorldOrigin` (12345, 54321, 0).

## 1. What "parity" means here

**Function.** Every scenario in the VoxelDemo QA plan (`VoxelWorldPOI/Documentation/QA_TEST_PLAN.md`) that
touches terrain passes in the mode under review, or has a written reason why it is out of scope for that mode.
That covers spawning, movement, streaming, the map, editing, POIs, dungeons, campsites, digging and gathering,
NPCs, world items, and save/load.

**Performance.** Each run is compared with an InfinitePlane smooth **control run**: same machine, same viewport
size, same bench scenario, and a config that differs only in the mode under review. Proposed thresholds
(decision D3):

| Metric | Threshold vs control |
|--------|----------------------|
| Frame P95 (traverse phase) | within +10% |
| Chunk-manager work P95 | within +15% |
| Catch-up after stopping | within +25% |
| Peak loaded chunks, resident voxel and mesh MB | within +25%, or explained (e.g. island slab culling, §3 KI5) |
| `voxel.Stream.AuditResidency` after settling | 0 findings |
| Fall-throughs (`vox.Debug.Capture` READY→NOT-READY under the feet) | none |

For reference, the latest InfinitePlane smooth numbers are from the R24 P3 bench (PIE, 3440×1408 viewport,
v3000 over 40,000 uu): frame P50 ~25 ms, P95 43-46 ms, P99 52-59 ms, manager P95 19-21 ms, catch-up 2.5-3.4 s,
about 1,230 chunks loaded at peak. The absolute targets in `Research/PERFORMANCE_TARGETS.md` (32³ chunks, 1,000
chunks, 210 MB) are older than the current demo and are not used as pass/fail here. Refreshing them is one of
the review's outputs.

## 2. Findings already traced in code (to confirm in the review)

Four read-only code surveys (2026-10-12) found the items below. The review confirms or dismisses each one. IDs are
stable, so the follow-up plan can refer to them. Review A has ruled on KC1-KC9 and added KC10-KC14 (§12.3, §12.4);
KI1-KI11 wait for Review B.

### Cubic

| ID | Finding | Evidence | Impact |
|----|---------|----------|--------|
| KC1 | **Cubic ignores LOD.** The CPU cubic mesher never reads `LODLevel`, so every cubic chunk is meshed at full resolution at every distance. LOD changes still trigger a remesh, a 6-face neighbour cascade and a 0.35 s crossfade, all of it for an identical mesh. | `VoxelWorlds/Source/VoxelMeshing/Private/VoxelCPUCubicMesher.cpp` (no `LODLevel` use); `VoxelChunkManager.cpp:3080`, `:3840-3933`, `:4792`, `:4945-4972` | Performance: full-detail meshing out to 40,000 uu, plus wasted remeshes |
| KC2 | **Cubic is the last user of the legacy whole-chunk path**: neighbour slices, deferral, `CaptureMeshDeps`/revalidate, the 26-neighbour remesh on generation (cubic only needs 6), and deep-plane extraction it never reads. Seam-registry bookkeeping still runs for cubic. No streaming run has exercised this path since seam P4b retired the rollback flag. | `VoxelChunkManager.cpp:447-456` (seam gate admits DC and MC only), `:3043-3125`, `:4713-4723`, `:5175-5190`; `VoxelNeighborSliceExtraction.cpp:30-40`; `Research/SEAM_OWNERSHIP_ARCHITECTURE.md:165-171` | Unverified correctness; overhead; blocks deleting the legacy machinery |
| KC3 | **GPU cubic meshing is test-only.** `bUseGPUMeshing` is ignored for cubic. The GPU shader has no LOD, greedy or non-occluding support. `Meshing.Cubic.GPUAsync`/`GPUReadback`/`CPUvsGPU` have no `-nullrhi` guard, and `Meshing.Cubic.Performance` waits without a bound (`CubicMeshingTests.cpp:566`), so it hangs headless. | `VoxelChunkManager.cpp:1110-1127`, `:3469-3482`; `VoxelGPUCubicMesher.cpp:207-242` | Dead code path; a test-suite hazard |
| KC4 | **Cubic scatter is wrong at LOD 0 when GPU extraction is on.** The GPU branch runs before the cubic check, so points land on greedy-quad vertices instead of block-face centres. `DemoCubicWorldConfig` turns GPU extraction on. `PlacementMode = BlockFaceSnap` is assigned but never read. | `VoxelScatter/Private/VoxelScatterManager.cpp:2000-2038`, `:106-117` | Grass and props misplaced |
| KC5 | **Player dig and place are tuned for smooth terrain.** Dig targets `floor(ImpactPoint)` with no normal offset, which is the air voxel on +X/+Y/+Z faces. Both brushes are smooth density deltas (80 against a 127 threshold), so one block takes about 2 clicks. Block-accurate editing exists only on the test actor (`bUseDiscreteEditing`, default off). | `VoxelCharacterPlugin/.../Core/VCCharacterBase.cpp:1758-1760`; `.../Core/VCPlayerController.cpp:1331-1386`; `VoxelWorlds/.../VoxelWorldTestActor.cpp:1056-1127` | Editing feels broken in cubic |
| KC6 | **Surface queries return the smooth or analytic surface.** Cubic block tops sit 0 to 1 voxel above it, so spawn, NPC spawns, POI pads, dungeon entrances and campsite props can sit up to one voxel low. The campsite apron is graded "so Dual Contour reconstructs a smooth slope", which gives terraces in cubic. | `VoxelChunkManager.cpp:1647-1719`, `:2608-2632`; `VoxelWorldPOI/.../POIPlacementLibrary.cpp:156-159`, `:178` | Props sunk into blocks |
| KC7 | **Step heights.** Player `MaxStepHeight` is 50 against a 75 uu block, so every one-block step needs a jump (by design: "full voxel steps require jumping"). NPC auto-jump fires for rises above 0.8·MaxStepHeight and up to 110. Navigation's `MaxStepRise` of 100 is checked against *interpolated* heights, so a two-block rise may pass nav and then fail auto-jump. | `VCMovementComponent.cpp:107-113`; `VCNPCAIController.cpp:700-722`; `VoxelNavigation/.../VoxelSurfaceNavigationSubsystem.cpp:19-23` | NPCs stuck on cubic terrain |
| KC8 | **No content exercises cubic.** No map uses `DemoCubicWorldConfig` (dated 2026-09-23, so older than the QA changes to the smooth config). `Config/CubicScatterConfig` is orphaned. No benchmark has ever recorded a cubic run. | Asset references; `Saved/VoxelBench` | Nothing to test with yet |
| KC9 | **Voxel-tree injection is cubic-only**, while smooth worlds use HISM trees, which are what tree felling (surface gameplay F8) chops. Whether cubic voxel trees can be chopped, and what they yield, is unknown. | `VoxelChunkManager.cpp:2671-2675`, `:2870-2876` | F8 parity |

### Island

| ID | Finding | Evidence | Impact |
|----|---------|----------|--------|
| KI1 | **POI placement samples InfinitePlane height math in every mode.** Inside `IslandRadius` the falloff factor is 1, so the result is right by coincidence. In the falloff band and beyond, `RequireLand` sites pass the water check against land that is not there. Pads then raise plateaus out of the sea, because conditioning runs after the falloff. Off-island POIs still stream claims and structures over the void, appear as map markers, and are returned by `FindNearestPOIs`. Dungeon cover checks in the falloff band can leave dungeons exposed or flooded. | `VoxelWorldPOI/Source/VoxelWorldPOI/Private/POIPlacement.cpp:55-59`; `POITerrainConditioner.h:46-47`; `POIPlacementSubsystem.cpp:151-166`, `:371` | Broken coastline content |
| KI2 | **There is no world edge.** Nothing generates beyond R+F plus one chunk diagonal: no terrain, water or collision. Inside that ring is a flat floor at `EdgeHeight - 1000`, with a 1,000 uu cliff at R+F. Water tiles end with the loaded chunks. There is no blocking or kill volume. | `VoxelGeneration/Private/IslandBowlWorldMode.cpp:58-61`; `VoxelLOD/Private/DistanceBandLODStrategy.cpp:1035-1072`; `VoxelChunkManager.cpp:1596-1600` | Players fall out of the world |
| KI3 | **The island centre means two different things.** Generation tests absolute `WorldPos` against the raw `CenterX/Y`. The LOD cull uses `WorldOrigin + offset`. They agree only when `WorldOrigin` is 0, and the demo's origin is (12345, 54321). A config derived from the demo would be culled about 55,700 uu (557 m) off centre. | `IslandBowlWorldMode.cpp`; `VoxelCPUNoiseGenerator.cpp:903-927`; `DistanceBandLODStrategy.cpp:1048-1049` | Missing or cut-off terrain |
| KI4 | **Shape-parameter defects:** `bIslandBowlShape` is copied in four places and read nowhere. Exponential falloff never reaches 0 (`exp(-3)` ≈ 0.05), which leaves a step at the edge. `GetTerrainHeightBounds` leaves out the `EdgeHeight - 1000` floor the generator produces. Island params are rebuilt from config in four places. | `VoxelChunkManager.cpp:960-972`, `:2497-2507`; `VoxelMap/Private/VoxelMapSubsystem.cpp:53-65`; `VoxelWorldsEditor/Private/VoxelFieldTypes.cpp:52-64`; `IslandBowlWorldMode.cpp:82-91`, `:232-234` | Cosmetic to moderate |
| KI5 | **Far-band surface-slab culling is InfinitePlane-only.** Islands load the full height range per far column, about 7 chunk layers against 3-4, so they will load more chunks than InfinitePlane at the same view distance. | `DistanceBandLODStrategy.cpp:142-146` | Memory and streaming cost |
| KI6 | **There is no finite-world concept.** `MaxLoadedChunks` (5000) is never enforced. There is no world-bounds API, no pre-generation and no bake. | `VoxelCore/Public/VoxelWorldConfiguration.h:338-340` | Limits what "finite" can offer |
| KI7 | **The save key ignores world shape.** The key is `<Prefix>_<Map>_<Seed>`. Changing island radius, centre or mode on the same map and seed silently reloads stale edits, dungeon records, NPC records and explored tiles. | `VoxelWorldSave/.../VoxelWorldSaveSubsystem.cpp:203-206`, `:233-235` | Stale world after config changes |
| KI8 | **Spawn is only water-aware.** It searches an 8-direction spiral up to 50,000 uu. With water off, an off-island start lands on the void floor. `WaterLevel` is compared without `WorldOrigin.Z` in some places. `GetPlanetSpawnPosition` ignores `IslandCenter`. | `VoxelCharacterPlugin/.../Movement/VCVoxelNavigationHelper.cpp:173-255`; `VoxelWorldTestActor.cpp:510-520` | Bad starts on some configs |
| KI9 | **Consumers have no island mask.** The PCG sampler emits points off-island. The minimap marks off-island tiles as explored and saves them. The world-map pan is unclamped. With water off, navigation treats the off-island floor as walkable. | `VoxelPCG/Private/PCGVoxelSurfaceSampler.cpp:244-291`; `VoxelMapSubsystem.cpp:453-484`; `VoxelSurfaceNavigationSubsystem.cpp:191-192` | Content and UI leak past the coast |
| KI10 | **Island test coverage is minimal.** The only island map (`WorldPartition/WP_VoxelCoexistTest`) is currently bound to the smooth InfinitePlane `DemoWorldConfig`, and `WP_IslandBowlConfig` is untuned. No island bench has ever been run. Only two tests use IslandBowl (`GPUvsCPUIslandBowlParity`, `Conditioning.FlattensIslandBowl`). | Asset references; `VoxelGeneration/Tests/*` | Nothing to test with yet |
| KI11 | **The residency audit and island LOD measure distance differently.** Found while verifying the tooling: after a PIE bench on island S ended 4,700 uu past the coast, `AuditResidency` reported 49 loaded, unrendered top-layer chunks 50,000-52,000 uu away, beyond the 48,000 uu unload radius. Island LOD measures distance in 2D; the audit measures it in 3D. To confirm in I4, then align the two | `VoxelStreaming/Private/VoxelChunkManager.cpp` (`AuditResidency`, `FVector::Dist`); `DistanceBandLODStrategy.cpp:631-635` | False audit findings, or chunks kept too long |

Island features that are expected to work and only need confirming: campsites (world-mode aware, clamp the pad
above water), surface NPC spawning (needs a loaded surface, collision and dry land), scatter (works from loaded
meshes), WorldClaims conditioning (tested on IslandBowl), the world clock (no mode dependency), and GPU
generation parity (locked by tests).

## 3. Tooling to build before the review

The review needs measurements the current tools cannot produce. These items are small. The recommendation
(decision D4) is to ship them as one PR before any review run, so every run produces comparable,
self-describing reports.

**Status (2026-10-12): T1-T5 built and verified** in VoxelWorlds#76 and UE5_PluginDev#94; T6 not started.
Usage and report keys: `Research/STREAMING_PERFORMANCE.md`. T5 went further than planned: every GPU test now skips
under `-nullrhi`, not just the cubic ones. Before, `WorldModes.InfinitePlane.GPUConsistency` crashed a full
headless run, and a full headless `VoxelWorlds` run now passes (207 tests).

| ID | Tool change | Why |
|----|-------------|-----|
| T1 | **Self-describing bench reports.** Add map, config asset path, `WorldMode`, `MeshingMode`, GPU generation/meshing/scatter flags, `-VoxelForceCPU`, RHI, voxel size, chunk size, view distance, `WorldOrigin`, island params and viewport size to the JSON. | Today no report says what it measured (`VoxelStreaming/Private/VoxelStreamingBenchmark.cpp:286-319`). |
| T2 | **Bench path control.** `voxel.Bench.Run` gains a direction and an optional start point (the config struct already has `Direction`, `VoxelStreamingBenchmark.h:22-23`). Add a route mode with a waypoint list and an orbit mode (circle of radius r around a centre), plus an optional terrain-follow altitude. | Island chords, coast crossings and perimeter loops; later, great-circle routes on planets. |
| T3 | **Memory and residency in the report.** Sample `GetVoxelMemoryStats` every N frames: voxel, compressed and uniform counts, renderer CPU and GPU MB, collision, scatter and process MB. Run `AuditResidency` at the end of each run and record its findings. | "Large finite world" is bounded by memory, and the bench records only chunk counts. |
| T4 | **Mode A/B without hand-editing maps.** Add a `-VoxelConfig=/Game/...` switch that the world actor reads before `Initialize`. It works headless and in PIE. | Headless runs use the map's saved config, and no switch exists for config, mode or meshing. |
| T5 | **Headless hygiene.** Point `Scripts/voxel_bench_sweep.ps1` at `D:\Program Files\Epic Games\UE_5.8` (it still defaults to UE_5.7 on C:). Give the cubic GPU tests a `GUsingNullRHI` skip and a bounded wait. | KC3; avoids a hung headless suite. |
| T6 | *(optional)* `STATGROUP_Voxel` cycle counters around generation, meshing, seams, collision and scatter. | Unreal Insights currently shows the voxel work only as coarse task names. |

Content for the review. `Scripts/setup_world_modes_review.py` creates it; the configs are committed under
`Content/PluginTesting/Review/` (UE5_PluginDev#94):

- **Configs, all duplicated from `DemoSmoothWorldConfig`** so they share seed, biomes, caves, water and POI
  tables, and differ only in the field under test:
  - `Review_Cubic`: `MeshingMode = Cubic`, `DemoCubicScatterConfig`, greedy on.
  - `Review_Cubic_GPUScatter`: the same with GPU scatter extraction on, for KC4.
  - `Review_Island_S/M/L`: IslandBowl at the sizes in §5, with `WorldOrigin` 0 and water on.
  - `Review_Island_M_DemoOrigin`: island M with the demo's `WorldOrigin`, to reproduce KI3.
- **No duplicate maps.** T4 runs any review config on `VoxelDemo` itself, so every game system is present.
  - In PIE: `voxel.ConfigOverride /Game/PluginTesting/Review/<Name>` in the editor console, then Play. Clear it
    with `voxel.ConfigOverride ""` afterwards.
  - Headless: `-VoxelConfig=`, or `-Config` on the sweep script.
- **Saves:** while an override is active the world save key gets the config's name appended
  (`World_VoxelDemo_<Seed>_<Config>`), so the demo's normal save is never loaded or overwritten. Start each tier
  from a fresh world with `vox.DeleteSave`, Stop, Play. Runs without an override still follow the usual rule: back
  up `Saved/SaveGames/Worlds/` first, and check `savedAtUtc` before restoring, since the user may play between
  sessions.

## 4. Review A: cubic meshing

**Setup:** `VoxelDemo` with `voxel.ConfigOverride /Game/PluginTesting/Review/Review_Cubic`; `VoxelDemo` without an
override as the control. Build from
`D:\Program Files\Epic Games\UE_5.8`. Disable the editor's background throttle for PIE timing runs.

| ID | Scenario | How | Pass when |
|----|----------|-----|-----------|
| C1 | Automated suites | In-editor `AutomationTestToolset`: `Meshing.Cubic.*` and `Scatter.*` (real RHI, so the GPU cubic tests run), plus the full headless suite | All green; record the cubic GPU tests separately (KC3) |
| C2 | Streaming correctness | PIE: spawn, walk, then W-04 (`vox.SetSpeed 3000`, `vox.AutoRun 1 0 0`, 1 min). Survey with `pie_camera` and `pie_screenshot` at chunk boundaries and at the config's LOD band edges. Repeat the R23 teleport repro (A→B→C), then run `voxel.Stream.AuditResidency` and `voxel.FarCompression.Stats` | No holes, missing faces or stale faces at chunk boundaries after late neighbours load. Fades render with the atlas material. Audit reports 0 findings. No chunk left in `Meshing` |
| C3 | Streaming performance | Focused-PIE bench at v1500, v3000 and v6000 over 40,000 uu, a cold start per velocity, cubic vs smooth control. Headless CPU sweep with `-VoxelConfig`. One extra cubic run with a single LOD band, to price KC1 | §1 thresholds vs control. Also record per-chunk mesh ms, vertex and triangle counts, renderer GPU MB, and the thrash breakdown |
| C4 | Editing | W-06 by hand: edit mode B, LMB/RMB on +X/+Y/+Z/−Z faces, at a chunk corner, and with a pickaxe (`vox.Dig`). Walk into the hole. Save, stop, play: the edits come back | One click removes or places exactly the targeted block (expected to fail today, KC5). No seams. No fall-through |
| C5 | Movement and collision | W-02 over one-block steps and two-block walls: jump, sprint, crouch, swim in cubic water. Drop a physics cube (`Saved/ClaudeScripts/pie_drop_cube_*.py`); a corpse rests on blocks. `vox.Debug.Capture 1` during an AutoRun at distance | Steps behave as designed (decision D5). Rigid bodies rest on top. No fall-through |
| C6 | Game systems | W-07 POI visit (pads, structures on block tops); `vox.TPPOI Tiled`, `vox.TPRoom` (dungeon entrance stitching); `vox.Campsite` and placing a campfire kit; `vox.NPCs`, `vox.SpawnNPC Wolf 3`, `vox.NavPath`, `vox.NavDebug 1`; drop world items (Q); chop trees and gather (KC9); W-05 map and minimap; save/load; one day/night cycle | Nothing floats or sinks more than one voxel (KC6). NPCs path over steps without getting stuck (KC7). Trees can be chopped |
| C7 | Scatter | `Review_Cubic` vs `Review_Cubic_GPUScatter`: grass and props on block tops at LOD 0 and beyond | Scatter on block-face centres in both (expected to fail with GPU extraction, KC4) |

**Output:** a pass/refine/bug table per scenario, the bench comparison, and a recommendation on decision D1
(how far cubic is supported). **Done 2026-10-13: see §12.**

## 5. Review B: island mode as a large finite world

**Sizes** (decision D2; island centred at the origin, demo view distance 40,000 uu):

| Tier | IslandRadius / FalloffWidth | Across | Chunk columns (approx.) | Purpose |
|------|-----------------------------|--------|--------------------------|---------|
| S | 30,000 / 10,000 uu | 0.8 km | ~220 | The whole island fits inside the view distance from the centre: **full-residency baseline** (memory, time to build the whole world) |
| M | 200,000 / 50,000 uu | 5 km | ~8,500 | **Main target**, "large but finite": a 15-minute walk across |
| L | 450,000 / 50,000 uu | 10 km | ~34,000 | Scale check; still far inside the ~100 km float-precision limit (`Research/TRAVERSAL_LIMITS.md`) |

**Setup:** `VoxelDemo` with `voxel.ConfigOverride /Game/PluginTesting/Review/Review_Island_<Tier>`. The control is
`VoxelDemo` without an override, running the same bench routes. Bench routes, for example:
- Chord: `voxel.Bench.Run islandM_chord 3000 500000 start=-250000:0 dir=1:0 follow=300`
- Perimeter: `voxel.Bench.Run islandM_loop 3000 0 orbit=0:0:200000 follow=300`
- Full residency (S): `voxel.Bench.Run islandS_settle 0 0 warmup=300`

| ID | Scenario | How | Pass when |
|----|----------|-----|-----------|
| I1 | Automated and probe tests | The existing island tests, plus probe tests that should fail today: analytic vs generated height across the inner area, the falloff band and outside (extend `HeightParity`); `ShouldCullIslandBoundary` with a non-zero `WorldOrigin` (KI3); POI generation over the island bounds, counting POIs beyond R+F and land POIs whose footprint is under water (KI1) | Results confirm or dismiss KI1, KI3 and KI4 with numbers |
| I2 | World shape | `voxel.Map.Capture` of the whole island (origin/main); a high `pie_camera` survey of the coast, the falloff band, the edge cliff and beyond; rectangle shape and Exponential falloff on S | The coastline reads as an island. Record the edge cliff and void (KI2, KI4) |
| I3 | Full residency (S) | Spawn at the centre, wait for generation and meshing queues to empty, then record time to settle, peak loaded chunks and resident MB (T3), and run `FarCompression.Stats` and `AuditResidency`. Fly to the coast and back; memory must return to the same level | Whole island resident inside the memory budget (decision D3); 0 audit findings |
| I4 | Streaming bench (M, L) | (a) centre to coast and out, at v1500/v3000/v6000; (b) a chord through the centre from coast to coast; (c) a perimeter loop at radius R (T2 orbit). Run the same routes on the InfinitePlane control. Repeat headless with `-VoxelConfig` | §1 thresholds. KI5 explains any chunk-count excess |
| I5 | The edge | Walk, then swim out past the coast with water on and with water off. Watch the ocean surface, the floor cliff, falling, KillZ, the minimap, NPCs following, collision ahead of the pawn | Today: expected to fall out of the world (KI2). Record exactly what happens, as input for the world-edge design |
| I6 | All-systems tour (M) | `vox.TPPOI` to every POI (route from `FindNearestPOIs`): each on land, no plateau in the sea, dungeons neither exposed nor flooded (`vox.TPRoom`), campsites on the coast, `vox.NPCs` counts and off-island spawn-reject log spam, scatter and PCG not past the coast, `vox.NavPath` toward the sea, markers on the island only, world map and exploration bounded, a day/night cycle | KI1 and KI9 confirmed or dismissed. Everything else passes |
| I7 | Spawn | PlayerStart at the centre, near the coast, and off-island (water on and off); island centre offset from the origin | The pawn always starts standing on land (KI8) |
| I8 | Save | Dig, place a campsite, clear a dungeon, explore, then save and reload. Then change `IslandRadius` on the same map and seed and load again | Round trip intact. Record what reloads after the radius change (KI7) |
| I9 | Long-session soak (M) | 20-30 min of mixed play and AutoRun loops around the island, with `AuditResidency` every 5 min | 0 findings, memory flat, no stuck mesh slots, frame times stable (the open "aged session" concern) |
| I10 | *(optional)* World Partition | Re-bind `WP_VoxelCoexistTest` to `Review_Island_M` and repeat the coexistence check | WP streams the placeholder actors while terrain streams |

**Output:** the scenario table, the bench comparison against the control, full-residency numbers for S, and an
estimate of the largest island the current streaming-only model can hold.

## 6. Mode-specific features

"Critical" means needed to call the mode on par with InfinitePlane for this project's game. "Nice to have" means
it would make the mode better without blocking parity. The review confirms the critical list; the follow-up
plan sizes and orders it.

### Cubic

| Feature | Class | Notes |
|---------|-------|-------|
| Cubic LOD, or an explicit single-LOD cubic | Critical | KC1. Either downsample blocks at stride > 1 (pick the majority or surface voxel per 2×2×2), or skip LOD for cubic and cap its view distance. Today it pays for LOD changes and gets nothing for them |
| Block-accurate player editing | Critical | KC5. Dig at `ImpactPoint − ½·VoxelSize·normal`, place at `+ ½·VoxelSize·normal`, set whole voxels (0/255) so one click equals one block. Port the test actor's discrete editing into the player path when the mode is cubic |
| Block-top surface query | Critical | KC6. A cubic surface height that snaps to the top of the solid voxel, used by spawn, NPCs, POIs, campsites and world items |
| Step policy | Critical | KC7. Keep "every block is a jump" and make nav and NPC auto-jump agree with block heights, or auto-step one block (decision D5) |
| Cubic-aware pads and aprons | Critical | KC6. Flat whole-voxel pads and stepped aprons for POIs and campsites |
| Correct scatter on block faces | Critical | KC4. Route cubic to the CPU extractor (or teach GPU extraction about block faces) and make `BlockFaceSnap` real |
| Seam support for cubic, or keep the legacy path | Critical (decision) | KC2. Cubic seam jobs are "trivially compatible" per the seam doc (face culling across the pair). Doing it lets the legacy neighbour-slice and revalidation machinery be deleted. The alternative is keeping that machinery for cubic alone. See D1 |
| GPU cubic meshing in streaming, or delete it | Nice | KC3. Today it is dead weight that only tests use |
| Choppable voxel trees | Nice | KC9. Felling removes connected log and leaf voxels and yields wood like HISM trees do |
| Block highlight and placement preview | Nice | Outline of the targeted block and a ghost of the block to place |
| Per-material hardness and break time | Nice | Mining feel; drops per material |

### Island

| Feature | Class | Notes |
|---------|-------|-------|
| A world edge | Critical | KI2. An ocean that continues to the horizon beyond the loaded ring (far water plane or ring), a seabed that slopes down instead of the `EdgeHeight - 1000` cliff, and a boundary rule. The rule can be soft (current pushes back, swim limit with a warning) or hard (blocking volume) |
| A world-bounds query on `IVoxelWorldMode` | Critical | KI1, KI9. `IsInsideWorld(X, Y)` / `GetWorldBounds()`, used by POI placement, PCG, NPC spawning, spawn, map exploration and navigation |
| World-mode-aware POI height sampling | Critical | KI1. Sample through `IVoxelWorldMode` instead of `FInfinitePlaneWorldMode` |
| One definition of the island centre | Critical | KI3, KI4. `WorldOrigin` plus offset everywhere, built by a single params builder |
| Save-key fingerprint | Critical | KI7. World mode, shape params and a generation version in the key or the save header |
| Slab culling for islands | Critical (perf) | KI5. Use the analytic height for the far-band slab |
| Whole-island map | Nice | A finite world can pre-render its whole map at load, clamp the world map to the island, and stop exploration at the coast |
| Distant island silhouette | Nice | A low-resolution whole-island mesh so the island stays visible from the sea beyond the view distance |
| Pre-generation for small islands | Nice | Generate everything at load, keep it compressed and resident, optionally cache it on disk. Builds on I3's numbers |
| Coastline shaping | Nice | A noise-perturbed coastline instead of a perfect circle, a beach band material rule, and optional cliffs |
| Bowl shape | Nice | KI4. Implement `bIslandBowlShape` or remove it |
| Archipelago | Nice | Several islands per world |
| WP placed-actor POI clusters | Nice | Deferred until POIs become placed actors (`Research/ENGINE_INTEGRATION.md`) |

### Spherical

See §7 and [SPHERICAL_PLANET_REQUIREMENTS.md](SPHERICAL_PLANET_REQUIREMENTS.md).

## 7. Spherical planet (summary)

Spherical mode today is a CPU density generator plus an LOD shell and horizon cull. It is not usable as a world:

- GPU generation is on by default and has no planet support; it falls through to generic 3D noise.
- The noise is mirrored across the equator.
- The default height-material rules turn every surface to snow.
- There are no water flags, no tests and no content.
- Every system that reads the surface assumes +Z is up.

The requirements document lists 57 requirements in nine areas, sorted into P0 (a correct, walkable planet), P1
(parity with the other modes) and P2 (later). It also records five scope decisions, all taken 2026-10-12 (planet
size, chunk topology, meshing modes, required game systems, orbit view), and includes the full Z-up audit (43
code sites).

## 8. Decisions

**Decided 2026-10-12: every recommendation below is accepted**, including the spherical scope recommendations
(SD1-SD5 in the requirements doc). T1-T5 are built first, as their own PR.

| ID | Question | Recommendation (accepted) |
|----|----------|---------------------------|
| D1 | How far is cubic supported: first-class (every game system), terrain-only (meshing, streaming, editing), or dropped? | Decide **after** Review A, using its numbers. The review is scoped to gather the evidence. **Review A recommends terrain-only (§12.5); awaiting the decision** |
| D2 | Island sizes for Review B | S / M / L as in §5 |
| D3 | Performance acceptance and memory budget | Relative to the InfinitePlane control (§1). Set the absolute memory budget after I3 measures full residency on S |
| D4 | Build T1-T5 before the review? | Yes, as one small PR. T6 optional |
| D5 | Cubic step policy | Keep "every block is a jump" (current design) and make nav and NPC auto-jump agree with block heights |
| D6 | Spherical scope (planet size, cubic on planets, game systems, orbit) | See the requirements doc, §2 |

## 9. Order of work

1. **T1-T5** and the review content: done 2026-10-12 (VoxelWorlds#76, UE5_PluginDev#94).
2. **Review A: cubic** (C1-C7): done 2026-10-13, results in §12 (D1 recommendation: terrain-only).
3. **Review B: island** (I1-I10): one to two sessions. A and B share tooling and can run in either order;
   cubic first is quicker because it has fewer unknowns.
4. **Second revision of this plan:** confirmed findings become work items with sizes and an order, alongside the
   spherical phasing.

## 10. Early signals from the tooling verification

These are not review results; the runs were short and only checked the tooling. They point where to look first.

- **Cubic** (headless CPU, VoxelWorldsTest, `-VoxelConfig=DemoCubicWorldConfig`, v3000 over 18,000 uu): 5,497 remeshes
  vs 201 for the same run in smooth, 1,427 MB peak voxel-system memory vs 580, frame P95 24.7 vs 12.3 ms. That fits
  KC1: cubic meshes at full detail everywhere and still pays for LOD changes.
- **Island S** (PIE, `Review_Island_S`, v3000 over 45,000 uu with `follow=400`): the whole world built in 19 s
  (warmup drained); peak 1,579 loaded chunks vs ~1,230 for InfinitePlane runs (fits KI5); audit 49 findings (KI11).
  A fresh island world saved 4 MB of edits on its first stop, probably POI stamps landing off-island (KI1).

## 11. Documentation that disagrees with the code

Collected by the surveys. These get fixed with the follow-up work, or sooner if cheap.

- `Features/WORLD_MODES.md:402-406` says POI placement uses the correct math in every mode (false, KI1). The
  `:163-167` cull description omits the chunk-diagonal ring and the floor in it. The `:343-347` chunk-count table
  was never measured. `:360-364` still lists water as future work.
- `Features/WORLD_MODES.md:198-312` and `Features/GPU_PIPELINE.md:50-51` show a direction-sampled spherical FBM
  and a GPU spherical branch; neither exists. `:441` says all three modes are "implemented and tested", but
  spherical has no tests.
- `Research/IMPLEMENTATION_PHASES.md:160` ("Spherical planet has correct curvature ✓") and `:326` ("water for
  all three modes") overstate the spherical state.
- `Features/LOD_SYSTEM.md:502-509` describes geodesic distance; the code uses Euclidean distance.
  `:1062-1064` ("cubic needs no special seam handling") holds only because cubic ignores LOD.
- `Features/MAP_SYSTEM.md:183-185` says the map data layer works unchanged on a planet. It does not: heights are
  radii, and the XY projection merges the two hemispheres.
- `Features/CAVE_SYSTEM.md:149` says GPU caves are InfinitePlane-only (stale: IslandBowl shares the branch).
- `Features/SCATTER_SYSTEM.md:752` says cubic routing covers both scatter paths (false for GPU extraction, KC4).
- `VoxelWorlds/README.md:96` claims GPU cubic meshing; it is test-only (KC3).
- `Research/TRAVERSAL_LIMITS.md:14` lists the analytic height mismatch as open; the height-parity and GPU work
  fixed it.
- Project `CLAUDE.md` said headless runs "silently skip" GPU tests. Nothing in the code skipped them; the
  headless suite filter left them out, and `InfinitePlane.GPUConsistency` crashed a full headless run. Since
  VoxelWorlds#76 every GPU test does skip under `-nullrhi`, which makes the statement true; the GPU tests still need
  an in-editor run to actually execute.

## 12. Review A results: cubic (2026-10-13)

Run on VoxelWorlds `4fe01cf` and UE5_PluginDev `1463315` (the review PRs above, merged), editor 5.8.3 from
`D:\Program Files\Epic Games\UE_5.8`, background throttle off, PIE in the level viewport (2874×1360, D3D12 SM6) with the
editor as the foreground window (60 fps when idle). Cubic means `VoxelDemo` with
`voxel.ConfigOverride /Game/PluginTesting/Review/Review_Cubic`, starting from a fresh world.

**The control** is VoxelDemo's own config, `DemoSmoothWorldConfig`, applied through the same override rather than run
with no override. The config is identical, but through the override the control gets its own save key, so both sides start
from a fresh world and the user's demo save is never loaded or written. The demo save's hashes and `savedAtUtc`
(2026-10-12T23:58:13Z) were unchanged at the end of the session.

**Bench method.** One cold PIE per run: Play, `vox.DeleteSave` (so the next Play is fresh again), clock pinned at noon,
`voxel.Bench.Run <tag> <v> 40000 start=0:0 dir=1:0 warmup=120`, Stop. Every warmup drained before the traverse. The
JSON reports are copied to [Research/Bench/ReviewA/](Research/Bench/ReviewA/). The session's helper scripts (status
probe, face aim and measure harness, cold-bench driver) are in the project's `Saved/ClaudeScripts/rc/`; screenshots named
below are in `Saved/Screenshots/Claudius/`.

### 12.1 Scenario results

Pass = meets the §4 "pass when"; Refine = works, but needs design or tuning to feel right; Bug = wrong behaviour.

| ID | Result | What happened |
|----|--------|---------------|
| C1 | **Pass** | Full headless `VoxelWorlds+VoxelWorldSave`: 207/207, with 18 GPU tests skipping under `-nullrhi` (`Saved/Logs/ReviewA_C1_headless_suite.log`). In-editor `Meshing.Cubic.*` + `Scatter.*`: 26/26, and the four GPU cubic tests really ran: `CPUvsGPU` matched 1,024/1,024 vertices, `Performance` measured 2.06 ms CPU vs 3.07 ms GPU per 32³ chunk and returned (no hang). Gap: nothing tests cubic *streaming*; the cubic tests are mesher unit tests only (KC2) |
| C2 | **Refine** | Correct, but it does not keep up. A fresh world settles in 37 s (1,118 chunks loaded, 658 with geometry). Surveys at chunk faces, a chunk corner and both LOD band edges (7,000 and 14,000 uu) show no holes, missing or stale faces (`rc_c2_corner_near`, `rc_c2_bands_north`). Fades render with the atlas material (`rc_c2_fade_2`). R23 A→B→C: site C settles in 37 s, audit 0, nothing left over site B (`rc_r23_siteB_west`). After every settle `AuditResidency` reports 0 and no chunk is left in `Meshing`. `FarCompression.Stats`: 901 compressed, 253 uniform, 122 resident, 1,133 MB reclaimed. Problems: W-04 cannot be walked (C5). Flown instead at 3,000 uu/s following the terrain (`voxel.Bench.Run c2_w04_follow 3000 180000 dir=0:1 follow=300`), the frontier fell behind: rendered chunks dropped from about 720 to 315, the mesh queue peaked at 1,739, the terrain ahead was missing (`rc_c2_w04_follow_3`), catch-up took 28 s and thrash reached 13,698. That run's end-of-run audit listed 604 `PendingUnload` chunks beyond the unload radius; they drained within seconds, so it is an unload backlog, not a leak. The edge of the loaded region shows vertical walls (a missing neighbour is treated as air), visible only from outside the view distance (`rc_r23_siteB_east`) |
| C3 | **Bug** | Fails the §1 thresholds at every speed except v6000 frame time, where both sides are saturated. See §12.2 |
| C4 | **Bug** | KC5 confirmed, see §12.3. The aimed block often survives while neighbours disappear. Block-centre targeting would need 2 clicks per full-density block, and open-air placement takes 2 clicks. A 2×2×2 pit dug across a chunk corner closes cleanly (no seams, `rc_c4_corner_pit`). The pawn dropped into it and landed (z −810, no READY→NOT-READY). Save, Stop, Play: all 9 edited chunks and the pawn position came back, and three probed columns matched |
| C5 | **Refine** | Steps behave as designed (D5) but the design hurts. `MaxStepHeight` 50 vs a 75 uu block, so `vox.AutoRun` stops at the first one-block rise (W-04 on foot is impossible). Holding Space jumps once (jump fires on press), so every block needs a tap. Jump apex ≈113 uu clears one block, not two. Tapping jump every 0.45 s, a hop-run covered 17,000 uu in 40 s at x ≈ 400,000 with `vox.Debug.Capture` on: 165 samples, collision under the feet every time, no READY→NOT-READY. A physics cube rests on a block top (gap 50 = its half height, speed 0). Swimming to the shore works when the shore block is at water level. Melee from a one-block pit at a dummy on the rim: 10 swings, 0 hits; on the same level it died in 4 (KC12). The player's corpse (`vox.Damage 1000`) rests on the blocks: pelvis 16 uu above the block top, the same as the smooth baseline after R1. (The training dummy despawns on death, and the wolf spawned for this wandered out of melee range, so the player's corpse is the only one tested.) |
| C6 | **Bug** | Props sink and NPCs strand (KC6, KC7), trees are not gameplay (KC9). Working: deer and wolves stand on block tops (gap ≈2 uu) and cross one-block steps (auto-jump "73 uu obstacle"); wolves chased and killed the player across terraces; `vox.NavPath` finds paths (0.14-0.93 ms); a dropped world item rests on a block top (gap 1); a player-placed campfire kit lands exactly on the block top (−1,200); dungeon rooms (`vox.TPRoom Entrance/Boss/Treasure`) are walkable and match the control's same dungeon (both show the carved voxel ceiling above the tile walls; `rc_c6_boss_left` vs `rc_ctrl_boss_left`); the map (`voxel.Map.Capture`, built from analytic height and so mode-independent), save/load and the day/night lighting work |
| C7 | **Bug** with GPU extraction, **Pass** with CPU | KC4 confirmed, see §12.3 (`rc_c7_cpu_scatter` vs `rc_c7_gpu_scatter`) |

### 12.2 Bench comparison

**Focused PIE**, v = traverse speed, 40,000 uu each, cold fresh world per run. "Systems MB" is peak voxel + renderer +
collision + scatter memory (`peakVoxelSystemsMB`).

| Run | Warmup s | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | Peak mesh queue | Peak loaded | Thrash (LOD / neighbour / other) | Voxel MB | Renderer GPU MB | Collision MB | Scatter MB | Systems MB | JSON |
|-----|----------|---------------------------|----------------|------------|-----------------|-------------|----------------------------------|----------|-----------------|--------------|------------|------------|------|
| Control v1500 | 8.8 | 18.2 / 31.8 / 40.0 | 15.6 | 0.17 | 4 | 1,224 | 293 (293 / 0 / 0) | 600 | 29 | 117 | 114 | 814 | `20261013_004246_rcA_ctrl_v1500` |
| Cubic v1500 | 21.6 | 30.1 / 50.1 / 75.8 | 29.7 | 12.2 | 683 | 1,208 | 9,443 (6,792 / 2,039 / 612) | 1,019 | 259 | 53 | 350 | 1,590 | `20261013_004851_rcA_cubic_v1500b` |
| Control v3000 | 6.8 | 25.2 / 40.5 / 47.9 | 20.4 | 2.1 | 6 | 1,189 | 280 (280 / 0 / 0) | 651 | 28 | 127 | 113 | 896 | `20261013_004433_rcA_ctrl_v3000` |
| Cubic v3000 | 22.8 | 35.5 / 62.0 / 78.9 | 32.4 | 20.3 | 618 | 1,223 | 7,650 (5,574 / 2,061 / 15) | 1,045 | 260 | 57 | 362 | 1,674 | `20261013_004357_rcA_cubic_v3000` |
| Control v6000 | 7.3 | 38.6 / 56.8 / 60.5 | 31.6 | 6.1 | 49 | 1,190 | 279 (279 / 0 / 0) | 707 | 28 | 119 | 147 | 993 | `20261013_004620_rcA_ctrl_v6000` |
| Cubic v6000 | 22.2 | 34.6 / 57.3 / 63.1 | 30.7 | 25.4 | 457 | 1,232 | 7,421 (4,942 / 2,302 / 177) | 1,016 | 260 | 66 | 345 | 1,653 | `20261013_004544_rcA_cubic_v6000` |
| Cubic, one LOD band, v3000 | 13.1 | 16.7 / 38.1 / 55.0 | 13.9 | 5.2 | 294 | 1,803 | 2,727 (0 / 2,104 / 623) | 1,803 | 252 | 59 | 273 | 2,375 | `20261013_004706_rcA_cubic1lod_v3000` |

All audits reported 0 findings. The first cubic v1500 run (`20261013_004154_rcA_cubic_v1500`) started from a save that
put the pawn 427,000 uu (4.3 km) away, which inflated its unload distances; it was repeated as `v1500b` from a fresh world.

**Against the §1 thresholds (cubic vs control):**

| Metric (threshold) | v1500 | v3000 | v6000 | One band, v3000 |
|--------------------|-------|-------|-------|-----------------|
| Frame P95 (+10%) | +58% ✗ | +53% ✗ | +1% ✓ (both saturated) | −6% ✓ |
| Manager P95 (+15%) | +90% ✗ | +59% ✗ | −3% ✓ | −32% ✓ |
| Catch-up (+25%) | 12.2 s vs 0.17 s ✗ | 9.7× ✗ | 4.2× ✗ | 2.5× ✗ |
| Systems memory (+25%) | +95% ✗ | +87% ✗ | +66% ✗ | +165% ✗ |
| Renderer GPU memory | 9× | 9× | 9× | 9× |
| Peak loaded chunks (+25%) | −1% ✓ | +3% ✓ | +4% ✓ | +52% ✗ |
| Audit / fall-throughs | 0 / none ✓ | 0 / none ✓ | 0 / none ✓ | 0 / none ✓ |

A fresh cubic world also takes about 3× longer to settle (warmup 22 s vs 7-9 s).

**Settled frame at the same site** (x −470,286, y −300,332, after streaming settles, from the renderer's own counters):
cubic 724 rendered chunks, 3.84 M triangles, 249 MB GPU; control 473 chunks, 0.98 M triangles, 26 MB GPU (3.9× the
triangles, 9.6× the GPU memory). Greedy quads share no vertices between faces, so cubic also carries 2 vertices per
triangle against about 0.6 for smooth.

**Initial load** (fresh world, pawn at the spawn, until the queues drain): cubic settles in 33 s, smooth in 16 s. For the
same 1,123 generated chunks cubic meshed 5,211 times (4.6 per chunk, with 2,109 interior crossfades while standing still);
smooth meshed 1,125 times (2 crossfades). The seam registry scheduled and processed 23,246 seam jobs for cubic, which
produce no geometry, against 10,898 for smooth.

**Headless CPU sweep** (`Scripts/voxel_bench_sweep.ps1 -Distance 40000 -BenchArgs "start=0:0 dir=1:0 warmup=120"`,
VoxelWorldsTest map, `-VoxelForceCPU`, `-Config` as below). This compares CPU work with no GPU or focus effects; CPU
generation makes every warmup about 40 s.

| Run | Frame P50 / P95 ms | Manager P95 ms | Catch-up s | Peak mesh queue | Thrash (LOD / neighbour / other) | Systems MB | JSON |
|-----|--------------------|----------------|------------|-----------------|----------------------------------|------------|------|
| Control v1500 | 4.0 / 10.8 | 7.8 | 3.8 | 12 | 369 (369 / 0 / 0) | 664 | `20261013_005515_rcA_hl_ctrl_v1500` |
| Cubic v1500 | 23.0 / 45.3 | 38.7 | 17.8 | 242 | 10,940 (5,635 / 3,310 / 1,995) | 1,441 | `20261013_005054_rcA_hl_cubic_v1500` |
| Control v3000 | 6.3 / 14.3 | 11.2 | 14.1 | 4 | 537 (537 / 0 / 0) | 572 | `20261013_005631_rcA_hl_ctrl_v3000` |
| Cubic v3000 | 24.6 / 43.2 | 38.0 | 25.5 | 370 | 10,942 (4,483 / 3,119 / 3,340) | 1,585 | `20261013_005226_rcA_hl_cubic_v3000` |
| Control v6000 | 10.2 / 23.3 | 18.1 | 21.3 | 7 | 523 (523 / 0 / 0) | 605 | `20261013_005750_rcA_hl_ctrl_v6000` |
| Cubic v6000 | 28.7 / 45.7 | 38.5 | 29.4 | 362 | 16,260 (3,636 / 3,114 / 9,510) | 1,651 | `20261013_005354_rcA_hl_cubic_v6000` |
| Cubic, one band, v3000 | 2.8 / 9.9 | 6.7 | 19.1 | 240 | 23,278 (0 / 3,055 / 20,223) | 2,438 | `20261013_005926_rcA_hl_cubic1lod_v3000` |

Headless shows the same pattern as PIE. Cubic misses frame time, manager time, catch-up and memory at every speed.
With one band, frame and manager time drop below the control and catch-up comes within 1.35× (19.1 vs 14.1 s), but
memory is 4× the control and unattributed "other" re-meshes explode to 20,223 (KC14).

**Per-chunk meshing** (headless, `-LogCmds="LogVoxelMeshing Verbose"`, one v3000 run each, CPU meshers; tags
`rcA_hlmesh_cubic_v3000` and `rcA_hlmesh_ctrl_v3000`; the smooth figures cover interiors only, not seam strips):

| | Meshes in the run | Mesh ms mean / p50 / p95 / max | Triangles per non-empty chunk, mean / p50 / p95 | Triangles meshed in total |
|---|-------------------|--------------------------------|------------------------------------------------|---------------------------|
| Cubic (greedy) | 10,149 (5,914 non-empty) | 26.3 / 24.6 / 46.4 / 133 | 6,990 / 5,960 / 18,452 | 41.3 M |
| Control (Dual Contouring) | 3,572 (1,511 non-empty) | 13.8 / 1.3 / 51.1 / 113 | 9,336 / 2,160 / 41,598 | 14.1 M |

A near cubic chunk costs about the same as a near smooth chunk, but every cubic chunk is a near chunk: the median job is
19× the smooth median, and the run spent about 5.4× the meshing CPU time (266 s vs 49 s) on the same route.

### 12.3 KC1-KC9: confirmed or dismissed

| ID | Verdict | Evidence |
|----|---------|----------|
| KC1 | **Confirmed** | Far terrain is meshed block by block at every distance (`rc_w04x_1`, `rc_c2_bands_north`). Renderer GPU memory is 252-260 MB in every cubic run against 28-29 MB for the control (9×). LOD changes still re-mesh: 4,942-6,792 LOD re-meshes per 40,000 uu run against 279-293. Each one crossfades an identical mesh: 4,788 interior crossfades in the first 155 s of play and about 16,700 by t = 670 s, with up to 4,110 mid-fade restarts. Even the initial load meshes each chunk 4.6 times (smooth: 1.0). A settled frame holds 3.9× the triangles, and the median cubic mesh job costs 19× the smooth median, because every chunk is a full-detail chunk (§12.2). **Price**, one band vs three at v3000: LOD thrash 5,574 → 0, catch-up 20.3 → 5.2 s, manager P95 32.4 → 13.9 ms, frame P95 62.0 → 38.1 ms. Dropping the bands is not free, though (KC11): the one-band run loaded 1,803 chunks (+47%), all resident and none compressed, and voxel memory went from 1,045 to 1,803 MB |
| KC2 | **Confirmed** as a cost; the correctness risk is **dismissed** for what this session exercised | The legacy whole-chunk path runs for cubic and is correct: no hole, missing or stale face in any survey, R23 clean, audits 0, chunk-corner edits closed. Cost: 2,039-2,302 neighbour re-meshes per run against 0 for the control, and still 2,104 with one band. The seam registry tracks every cubic chunk (1,237 chunks, 10,774 seams, 4,040 dirty) and runs its jobs for nothing: 23,246 seam jobs scheduled and processed during the initial load (smooth: 10,898) with no seam geometry at all (0 seam-bucket crossfades). No streaming test covers the path (C1) |
| KC3 | **Confirmed** (dead path); the test hazard is **fixed** by T5 | The cubic branch always builds `FVoxelCPUCubicMesher` and never reads `bUseGPUMeshing` (`VoxelChunkManager.cpp:1110-1126`); every PIE logged "Using Cubic mesher". The GPU cubic tests pass in-editor, skip headless, and `Performance` returns |
| KC4 | **Confirmed** | Same site, LOD0, instances within 6,000 uu. `Review_Cubic` (CPU extraction): every instance on a block top, rocks 100% on face centres, grass and mushrooms jittered inside the face (median 7-31 uu from the centre, maximum 36 < 37.5). `Review_Cubic_GPUScatter`: every instance still on a block top, but 24-25% of rocks sit on block corners (greedy-quad vertices); grass and mushrooms are pushed to face edges (median 25-33 uu); and density falls to about a third for grass (217-247 instances vs 655-725) and half for rocks (151-177 vs 288-317). All 8 definitions are forced to `BlockFaceSnap` (logged), but the GPU branch runs before the cubic check |
| KC5 | **Confirmed**, worse than described | *Targeting:* `floor(ImpactPoint)` picked the air voxel for +X and +Z faces every time, and also for a −Y face: the impact lies exactly on the voxel boundary, so floating-point noise picks the side ("Voxel destroyed at [-6435,-4664,-16]" for a block at y −4663). *Brush:* digging is a density delta, not a block. A near-threshold surface block went in 1 click. On the next block down, the aimed block survived until click 4 while its +X neighbour went at click 1, and 4 blocks (a plus shape) were gone after 5 clicks. On a +X face the aimed block went at click 2 and a neighbour at click 3. Aiming at the block centre took 2 clicks with no collateral. Placement targets the right voxel (offset along the normal) but adds +80 density: 1 click beside terrain, 2 in open air (the first click is invisible). *Demo:* in edit mode LMB also fires `AVoxelWorldTestActor`'s mouse brush (radius 300, smooth, `bUseDiscreteEditing` off). One press removed 6 scattered blocks around the aim point and left the aimed block (`rc_c4_lmbkey1`) |
| KC6 | **Confirmed** | Anything placed from the smooth or analytic surface sits up to one block low. Tiled dungeon: pad height −223, block top −150; the brazier, stone pillar and elevator call post stand at −223, 73-74 uu inside the block (`rc_c6_brazier_sunk`, `rc_c6_pillar_sunk`). Generated campsite: actor at −1,244 on a −1,200 block top; the campfire mesh is entirely inside the block (its top is −1,213) and the bench shows 2 uu (`rc_c6_campsite_close`). The control places the same dungeon and campsite at the same heights, and there they sit on the ground: props 0.4-0.6 uu from a −223.5 pad, campsite 0.3 uu above −1,244.0. Not affected: anything placed by trace or physics (spawn, NPCs, world items, a player-placed campfire kit, the physics cube) |
| KC7 | **Partly confirmed** | Player: every block is a jump (D5), so auto-run and a held jump stall at the first rise (C5). NPCs: one-block steps work (deer and wolves log "jumping a 73 uu obstacle"; wolves reached and killed the player across terraces). Two-block rises strand them: with the player on a 3×3 pillar two blocks high, two wolves stood at its base looping "stuck … blocking ahead and repathing" and never reached it. That is consistent with the nav/auto-jump mismatch (`voxel.Nav.MaxStepRise` 100 checked against interpolated heights), but the session did not isolate a path that nav accepted and movement refused. (At the one site checked, nav's surface sample under the player was identical in both modes, −1,215.85, so nav reads the same interpolated surface in cubic and smooth, not the block tops) |
| KC8 | **Confirmed** | No map references `DemoCubicWorldConfig` or `Config/CubicScatterConfig` (each is referenced only by itself; `DemoCubicScatterConfig` is used by the review configs). Before this review no PIE bench had recorded a cubic run. `Review_Cubic`, `_GPUScatter` and `_SingleLOD` are now the cubic test content |
| KC9 | **Confirmed**, worse than described | Cubic worlds get voxel trees (`TreeMode` VoxelData, 3 templates, density 5.1) and the cubic scatter config has no tree definition, so there is no HISM tree to chop. Swinging at a voxel tree does nothing: no harvestable scatter, and no dig without edit mode or a digging tool. Digging a trunk voxel yields Wood (material 20), 1 per dig, forever on the same voxel (G3). Nothing fells; the canopy stays. In `Review_Cubic` the canopies also render wrong (KC10, a config issue) |

### 12.4 New findings

Cubic:

| ID | Finding | Evidence | Impact |
|----|---------|----------|--------|
| KC10 | **`DemoSmoothWorldConfig`'s voxel tree templates use material IDs that do not exist.** Leaves are 22 and 23, but the registry has 22 materials (0-21: Wood 20, Leaves 21), and template 2's trunk is 21 (Leaves). The templates are inert in smooth (voxel trees are cubic-only), so this only shows when the config is meshed cubic, as `Review_Cubic` is: canopies render as opaque clay and brick blocks. `DemoCubicWorldConfig` uses 20 and 21 correctly. Nothing validates template material IDs against the registry | `Review_Cubic` / `DemoSmoothWorldConfig` / `DemoCubicWorldConfig` tree templates; `VoxelCore/Public/VoxelMaterialRegistry.h:30-34`; `rc_c6_tree_found` | Latent content bug; the review's tree screenshots show it. The canopies are not treated as leaves (not masked, not non-occluding) |
| KC11 | **Far compression and the far-band slab are keyed on the LOD band.** Compression skips chunks below `voxel.FarCompression.MinStride`; slab culling applies only beyond the first band. A cubic world without LOD bands loses both | `VoxelChunkManager.cpp:4677`; `DistanceBandLODStrategy.cpp:956-962`; one-band run: 0 compressed, 1,803 resident, +47% chunks, +73% voxel MB | Constrains the KC1 fix |
| KC12 | **Melee misses across a one-block height difference.** From a one-block pit, 10 swings at a dummy on the rim did no damage; on the same level it died in 4 | C5 | Common in cubic terrain |
| KC13 | **The edge of the loaded region shows vertical walls.** A missing neighbour counts as air, so boundary faces are emitted. Visible only from beyond the view distance | `VoxelCPUCubicMesher.cpp:930-1001`; `rc_r23_siteB_east` | Cosmetic |
| KC14 | **Unattributed re-meshes in cubic.** The bench's "other" re-mesh reason (an already-meshed chunk re-meshed outside the neighbour, LOD and dirty paths) is 0 for smooth in every run, 15-623 for cubic in PIE, 1,995-9,510 for cubic headless (rising with speed) and 20,223 for one-band cubic headless. Not traced in this session | §12.2 bench tables; `EVoxelRemeshReason::Other` in `VoxelChunkManager.h:120-127` | Wasted meshing; it hides how much of KC1's cost survives a one-band fix |

Mode-independent, found on the way:

| ID | Finding | Evidence |
|----|---------|----------|
| G1 | `UVoxelWorldComponent::GetTotalVertexCount`, `GetTotalTriangleCount` and `GetGPUMemoryUsage` (BlueprintPure) only ever add, so they report cumulative uploads: 54.7 M "vertices" at a site where, 30 s later, the renderer held 7.7 M. The renderer's own `GetDebugStats` counters are right | `VoxelRendering/Private/VoxelWorldComponent.cpp:315-317`, `:381-383` |
| G2 | A Subtract on a voxel that holds a Set edit **replaces** the Set instead of composing with it, so the voxel reverts to procedural minus the delta. POI pads are written with Set. One dig under a Set slab removed 5 blocks (the target and its 4 face neighbours, which only received the −21 falloff) | `VoxelCore/Private/VoxelEditManager.cpp:1072`; `VoxelWorldPOI/.../POIPlacementLibrary.cpp:169` |
| G3 | Gathering pays out on every dig whose brush modifies anything, and a dug voxel keeps its material ID, so one voxel yields without limit (6 digs at one trunk voxel, 6 Wood) | `VoxelCharacterPlugin/.../VCPlayerController.cpp:1344-1350`, `:1419-1431` |
| G4 | `voxel.Bench.Run … follow=` follows the seabed under lakes, so a long follow run drowns the pawn mid-run (tooling) | `c2_w04_follow` run |

### 12.5 Recommendation for D1

**Support cubic as terrain-only** (meshing, streaming, editing, scatter), and promote it to first-class only when a game
needs it. Do not drop it.

- **Why not drop:** the terrain layer is correct. Streaming, collision, edits at chunk corners, save/load and CPU scatter
  all passed, and the one-band run shows the performance gap has one main cause with a measured lever. Dropping cubic
  would let the legacy neighbour-slice path and GPU cubic mesher be deleted (KC2, KC3), but that is cleanup, not a reason.
- **Why not first-class now:** the game systems were built for a smooth surface. They fail in cubic for three root
  causes: the surface height query (KC6, which sinks POI props and campsites), the block step policy (KC7, KC12) and
  trees as gameplay (KC9). Each is bounded, but together they double the QA surface of every game feature for a mode no
  game uses yet.
- **Terrain-only work list** (for the second revision to size): cubic LOD, or one band with compression and the slab
  keyed on distance and a smaller cubic view distance (KC1, KC11); block-accurate dig and place in the player path and
  `bUseDiscreteEditing` on the demo brush in cubic (KC5); route cubic to CPU scatter extraction (KC4); move cubic onto
  the seam path, or keep the legacy path for cubic alone and stop the seam registry scheduling jobs for it, plus a cubic
  streaming test (KC2); trace the unattributed re-meshes (KC14); delete GPU cubic or keep it test-only (KC3); refresh or
  delete `DemoCubicWorldConfig` and the orphaned scatter config (KC8); fix the smooth config's tree material IDs and
  validate template IDs against the registry (KC10).
- **First-class adds:** one block-top surface query used by POI pads and props, campsites and navigation (KC6), with
  cubic pads and stepped aprons; NPC handling of two-block rises and a melee height tolerance (KC7, KC12); choppable
  voxel trees (KC9).
- **Performance after the terrain-only list:** the one-band run already meets the frame and manager thresholds. It still
  misses catch-up (2.5× in PIE, 1.35× headless) and memory, and renderer memory stays 9× until cubic has a coarse far
  mesh. So the parity target for cubic needs either real cubic LOD or an explicit, smaller cubic view distance (a
  decision for the second revision).
- **Mode-independent findings** G1-G3 apply to smooth as well and can be fixed independently of D1.
