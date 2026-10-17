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
revision of this document then turns the confirmed findings into work items. **Second revision, 2026-10-14: §14**
(the status of every finding, work items with sizes and dependencies, milestones M1-M6, and where spherical starts),
with the decisions it needed in §8. M1 results: §14.5. M2 (the world-mode API) results: §14.6. M3 (island correctness)
results: §14.7. M4 (the streaming window) results: §14.8. M5 (cubic, terrain-only) results: §14.9. M6 (the island's
visible edge and scale) results: §14.10. Spherical Phase 1 (a correct
planet on the CPU): plan and results in §15. Spherical Phase 2 (the planet on the GPU): §16.

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
stable, so the follow-up plan can refer to them. Review A has ruled on KC1-KC9 and added KC10-KC14 and G1-G6 (§12.3, §12.4).
Review B has ruled on KI1-KI11 and added KI12-KI15 and G7-G9 (§13.6, §13.7); KI12 was explained and fixed, and G10
found, in §13.10; the vertical-window bug in §13.11. §14.1 gives every finding's current status and its work item; it
names the §13.11 findings G11-G13.

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
estimate of the largest island the current streaming-only model can hold. **Done 2026-10-13: see §13.**

## 6. Mode-specific features

"Critical" means needed to call the mode on par with InfinitePlane for this project's game. "Nice to have" means
it would make the mode better without blocking parity. The review confirms the critical list; the follow-up
plan sizes and orders it. *(Second revision, 2026-10-14: §14 sizes and orders it. With D1 decided as terrain-only,
the cubic rows that serve the game systems are first-class only and deferred; the work item for each row is in its
class column.)*

### Cubic

| Feature | Class | Notes |
|---------|-------|-------|
| Cubic without the LOD system (D7) | Critical, **done 2026-10-13 (§12.7)** | KC1, KC11. Decided 2026-10-13: cubic forces LOD off, and greedy meshing is its LOD mechanism until there is a good reason to change it. Key far compression on distance so the LOD-off path keeps it. Measured in §12.2: LOD off already beats smooth per frame (before compression; §12.7 has the numbers with it). A far representation (coarse mesh or impostor) or a shorter cubic view distance is the only route to renderer-memory parity, and is not planned |
| Block-accurate player editing | Critical, **WM-C2** | KC5. Dig at `ImpactPoint − ½·VoxelSize·normal`, place at `+ ½·VoxelSize·normal`, set whole voxels (0/255) so one click equals one block. Port the test actor's discrete editing into the player path when the mode is cubic |
| Block-top surface query | First-class only, **deferred (D1)** | KC6. A cubic surface height that snaps to the top of the solid voxel, used by spawn, NPCs, POIs, campsites and world items |
| Step policy | First-class only, **deferred (D1)** | KC7, KC12. Keep "every block is a jump" and make nav and NPC auto-jump agree with block heights, or auto-step one block (decision D5) |
| Cubic-aware pads and aprons | First-class only, **deferred (D1)** | KC6. Flat whole-voxel pads and stepped aprons for POIs and campsites |
| Correct scatter on block faces | Critical, **WM-C3** | KC4. Route cubic to the CPU extractor (or teach GPU extraction about block faces) and make `BlockFaceSnap` real |
| Seam support for cubic, or keep the legacy path | Critical, **decided with D1: seam support (WM-C1)** | KC2. Cubic seam jobs are "trivially compatible" per the seam doc (face culling across the pair). Doing it lets the legacy neighbour-slice and revalidation machinery be deleted. The alternative is keeping that machinery for cubic alone. See D1. Measured in §12: with LOD off, the legacy cascade is the main remaining catch-up cost (2.0 meshes per chunk, catch-up 1.6-3.2× smooth) |
| GPU cubic meshing in streaming, or delete it | Nice, **decided with D1: delete (WM-C4)** | KC3. Today it is dead weight that only tests use |
| Choppable voxel trees | Nice, first-class only, **deferred (D1)** | KC9. Felling removes connected log and leaf voxels and yields wood like HISM trees do |
| Block highlight and placement preview | Nice | Outline of the targeted block and a ghost of the block to place |
| Per-material hardness and break time | Nice | Mining feel; drops per material |

### Island

| Feature | Class | Notes |
|---------|-------|-------|
| A world edge | Critical, **WM-I4/WM-I5 (recovery, boundary), WM-E1 (the visible edge)** | KI2, KI14. An ocean that continues to the horizon beyond the loaded ring (far water plane or ring), a seabed that slopes down instead of the `EdgeHeight - 1000` cliff, and a boundary rule. The rule can be soft (current pushes back, swim limit with a warning) or hard (blocking volume). *Decided with D9: hard at R+F, with a soft push-back before it, and a respawn as the safety net.* *Done: the boundary and recovery in M3 (§14.7), the far ocean and the sloping seabed in M6 (§14.10)* |
| A world-bounds query on `IVoxelWorldMode` | Critical, **WM-A2** | KI1, KI9. `IsInsideWorld(X, Y)` / `GetWorldBounds()`, used by POI placement, PCG, NPC spawning, spawn, map exploration and navigation |
| World-mode-aware POI height sampling | Critical, **WM-A3 + WM-I1** | KI1. Sample through `IVoxelWorldMode` instead of `FInfinitePlaneWorldMode` |
| One definition of the island centre | Critical, **WM-A1** | KI3, KI4. `WorldOrigin` plus offset everywhere, built by a single params builder |
| Save-key fingerprint | Critical, **WM-A4** | KI7. World mode, shape params and a generation version in the key or the save header |
| Slab culling for islands | Critical (perf), **WM-S1** | KI5. Use the analytic height for the far-band slab. *Widened by D8: every heightmap world takes its vertical range from the terrain* |
| 3D LOD distance for islands | Critical (perf), **WM-S2** *(added 2026-10-14)* | KI13, KI11. Measure LOD distance in 3D like InfinitePlane; the island boundary cull stays 2D |
| Whole-island map | Nice | A finite world can pre-render its whole map at load, clamp the world map to the island, and stop exploration at the coast |
| Distant island silhouette | Nice | A low-resolution whole-island mesh so the island stays visible from the sea beyond the view distance |
| Pre-generation for small islands | Nice | Generate everything at load, keep it compressed and resident, optionally cache it on disk. Builds on I3's numbers |
| Coastline shaping | Nice | A noise-perturbed coastline instead of a perfect circle, a beach band material rule, and optional cliffs |
| Bowl shape | Nice | KI4. Implement `bIslandBowlShape` or remove it. *WM-H6 removes the unread flag; a real bowl shape stays a nice-to-have* |
| Map tile eviction | Critical past ~10 km, **WM-E2** *(added 2026-10-14)* | KI15. The first cost that grows with the island (§13.8). *Done in M6 (§14.10): tile images under an 80 MB budget* |
| Archipelago | Nice | Several islands per world |
| WP placed-actor POI clusters | Nice | Deferred until POIs become placed actors (`Research/ENGINE_INTEGRATION.md`) |

### Spherical

See §7 and [SPHERICAL_PLANET_REQUIREMENTS.md](SPHERICAL_PLANET_REQUIREMENTS.md). §14.4 places its phases in this
plan's milestones.

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
(SD1-SD5 in the requirements doc). T1-T5 are built first, as their own PR. **Decided 2026-10-14, for the second
revision (§14):** D1, the D3 budget, D8 and D9, each as recommended.

| ID | Question | Recommendation (accepted) |
|----|----------|---------------------------|
| D1 | How far is cubic supported: first-class (every game system), terrain-only (meshing, streaming, editing), or dropped? | Decide **after** Review A, using its numbers. The review is scoped to gather the evidence. Review A recommended terrain-only (§12.5). **Decided 2026-10-14: terrain-only.** Cubic gets meshing, streaming, editing and CPU scatter (milestone M5): it moves onto the seam path, so the legacy neighbour path can be deleted (WM-C1), block-accurate editing (WM-C2), cubic scatter on the CPU extractor (WM-C3), and the GPU cubic mesher is deleted (WM-C4). The game-system items (block-top surface query and cubic pads, KC6; two-block rises, KC7; melee height, KC12; choppable voxel trees, KC9) wait until a game needs cubic |
| D2 | Island sizes for Review B | S / M / L as in §5 |
| D3 | Performance acceptance and memory budget | Relative to the InfinitePlane control (§1). Set the absolute memory budget after I3 measures full residency on S. Proposed in §13.4. **Accepted 2026-10-14:** 0.6 GB of voxel systems memory settled for a fully resident S-class island; at most 1.5 GB peak while streaming at any speed and tier. Benches report against it from now on |
| D4 | Build T1-T5 before the review? | Yes, as one small PR. T6 optional |
| D5 | Cubic step policy | Keep "every block is a jump" (current design) and make nav and NPC auto-jump agree with block heights |
| D6 | Spherical scope (planet size, cubic on planets, game systems, orbit) | See the requirements doc, §2 |
| D7 | Cubic LOD *(decided 2026-10-13, after Review A; implemented the same day, §12.7)* | Cubic forces the LOD system off (`MeshingMode == Cubic` overrides `bEnableLOD`). Greedy meshing is cubic's LOD mechanism until there is a good reason to change it, so block downsampling and other cubic LOD schemes are out of scope. Pair it with far compression keyed on distance (KC11): forcing LOD off alone trades the LOD cost for uncompressed voxel memory (§12.2) |
| D8 | Where heightmap worlds (InfinitePlane, IslandBowl) take their vertical streaming range from *(decided 2026-10-14, from §13.11)*. Today it is the viewer's layer −2 to +8 (island +4), clipped to the world's height range, so a viewer above layer 0 loses the ground below | **From the terrain.** Every column loads its own surface slab (the world mode's analytic surface ± 1.5 chunks, and up to the water plane) at every distance, islands included. Near the viewer (band 0) the viewer's own layer ± 2 is also loaded, for caves, dungeons and digging. The viewer's altitude only adds layers; it never removes the ground. This is WM-S1 and subsumes slab culling for islands (KI5). Keep the window a per-column interval supplied by the world mode, so the planet version (a shell around the surface, SP-C1/SP-C5) has the same shape |
| D9 | Order of work, island priorities and where spherical starts *(decided 2026-10-14)* | **Island first, then cubic, then spherical** (§14.3): M1 hygiene, M2 the world-mode API, M3 island correctness, M4 the streaming window, M5 cubic terrain-only, M6 the visible island edge and map tile eviction, then spherical Phase 1. Correctness before performance on the island. The island's boundary is **hard at R+F**, with a soft push-back in the sea before it, and falling out of the world respawns the pawn (KI14). Spherical Phase 0 (guardrails) rides in M1, and the heightmap side of its API (SP-A1, SP-A2, SP-A6, SP-G9) in M2; Phase 1 starts after M4 (the window shape SP-C1 needs) and M5 (the legacy path is gone before spherical touches streaming). The order is a priority, not a strict sequence: §14.3 lists what can run in parallel |

## 9. Order of work

1. **T1-T5** and the review content: done 2026-10-12 (VoxelWorlds#76, UE5_PluginDev#94).
2. **Review A: cubic** (C1-C7): done 2026-10-13, results in §12 (D1 recommendation: terrain-only). D7 and KC11
   implemented the same day (§12.7), and the compression encode moved to a worker thread (G6, §12.8).
3. **Review B: island** (I1-I10): done 2026-10-13, results in §13 (I10 not run). KI1-KI3, KI5-KI7 and KI10-KI11
   confirmed, KI4, KI8 and KI9 partly; new KI12-KI15. KI12 (bimodal LOD re-meshing) explained and fixed the same day:
   a latch in LOD evaluation, in every world mode (§13.10); the vertical candidate window was fixed with it (§13.11).
4. **Second revision of this plan:** confirmed findings become work items with sizes and an order, alongside the
   spherical phasing. Done 2026-10-14: §14, with D1, D3, D8 and D9 decided (§8).
5. **The work items** in the order of §14.3: M1 → M2 → M3 → M4 → M5 → M6, then spherical Phase 1. M1-M5 merged by
   2026-10-16; M6 done 2026-10-16 (§14.10).

## 10. Early signals from the tooling verification

These are not review results; the runs were short and only checked the tooling. They point where to look first.

- **Cubic** (headless CPU, VoxelWorldsTest, `-VoxelConfig=DemoCubicWorldConfig`, v3000 over 18,000 uu): 5,497 remeshes
  vs 201 for the same run in smooth, 1,427 MB peak voxel-system memory vs 580, frame P95 24.7 vs 12.3 ms. That fits
  KC1: cubic meshes at full detail everywhere and still pays for LOD changes.
- **Island S** (PIE, `Review_Island_S`, v3000 over 45,000 uu with `follow=400`): the whole world built in 19 s
  (warmup drained); peak 1,579 loaded chunks vs ~1,230 for InfinitePlane runs (fits KI5); audit 49 findings (KI11).
  A fresh island world saved 4 MB of edits on its first stop, probably POI stamps landing off-island (KI1).

## 11. Documentation that disagrees with the code

Collected by the surveys. These get fixed with the follow-up work, or sooner if cheap. *(§14: the ones still open are
work item WM-H9 in M1; the spherical ones go with spherical Phase 0.)*

- `Features/WORLD_MODES.md:402-406` says POI placement uses the correct math in every mode (false, KI1). The
  `:163-167` cull description omits the chunk-diagonal ring and the floor in it. The `:343-347` chunk-count table
  was never measured. `:360-364` still lists water as future work.
- `Features/WORLD_MODES.md:198-312` and `Features/GPU_PIPELINE.md:50-51` show a direction-sampled spherical FBM
  and a GPU spherical branch; neither exists. `:441` says all three modes are "implemented and tested", but
  spherical has no tests.
- `Research/IMPLEMENTATION_PHASES.md:160` ("Spherical planet has correct curvature ✓") and `:326` ("water for
  all three modes") overstate the spherical state.
- `Features/LOD_SYSTEM.md:502-509` describes geodesic distance; the code uses Euclidean distance.
  `:1062-1064` ("cubic needs no special seam handling") held only because cubic ignored LOD. *Fixed with D7
  (§12.7): the section now says cubic runs without LOD and names the legacy boundary path.*
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
`voxel.ConfigOverride /Game/PluginTesting/Review/Review_Cubic`, starting from a fresh world. Line numbers in §12 are as of
VoxelWorlds `4fe01cf`.

**Two cubic setups.** Cubic was designed to run *without* the LOD system: greedy meshing is its answer to distance,
because uniform blocks do not downsample the way a density surface does. The switch is `bEnableLOD` in the world config.
The cubic content does not ship that way: `Review_Cubic` (copied from the smooth config) and the original
`DemoCubicWorldConfig` both have `bEnableLOD` on with three LOD bands. The first pass therefore measured cubic **as
shipped** (LOD on, greedy on). A second pass the same day measured the **intended path**, `Review_Cubic_NoLOD`
(`bEnableLOD` off, greedy on), plus `Review_Cubic_NoLOD_Simple` (greedy off) to show what greedy saves. Where the two
differ, both are reported; §12.6 maps where cubic and smooth separate in the code. Decided after the review (D7): cubic
forces LOD off, and greedy meshing is its LOD mechanism.

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
| C2 | **Refine** | Correct in both setups. As shipped (LOD on) it does not keep up; on the intended path (LOD off) a fresh world settles in 17 s like smooth (16 s) and the bench mesh queue peaks at 75-135 instead of 457-683, and the remaining lag is the neighbour cascade (KC2). The rest of this row is the as-shipped pass. A fresh world settles in 37 s (1,118 chunks loaded, 658 with geometry). Surveys at chunk faces, a chunk corner and both LOD band edges (7,000 and 14,000 uu) show no holes, missing or stale faces (`rc_c2_corner_near`, `rc_c2_bands_north`). Fades render with the atlas material (`rc_c2_fade_2`). R23 A→B→C: site C settles in 37 s, audit 0, nothing left over site B (`rc_r23_siteB_west`). After every settle `AuditResidency` reports 0 and no chunk is left in `Meshing`. `FarCompression.Stats`: 901 compressed, 253 uniform, 122 resident, 1,133 MB reclaimed. Problems: W-04 cannot be walked (C5). Flown instead at 3,000 uu/s following the terrain (`voxel.Bench.Run c2_w04_follow 3000 180000 dir=0:1 follow=300`), the frontier fell behind: rendered chunks dropped from about 720 to 315, the mesh queue peaked at 1,739, the terrain ahead was missing (`rc_c2_w04_follow_3`), catch-up took 28 s and thrash reached 13,698. That run's end-of-run audit listed 604 `PendingUnload` chunks beyond the unload radius; they drained within seconds, so it is an unload backlog, not a leak. The edge of the loaded region shows vertical walls (a missing neighbour is treated as air), visible only from outside the view distance (`rc_r23_siteB_east`) *(M5, §14.9: **Pass.** On the seam path W-04 flown at 3,000 uu/s keeps up: catch-up 0.5 s, peak mesh queue 2, thrash 0; no walls at the loaded region's edge)* |
| C3 | **Bug** as shipped, **Refine** on the intended path | As shipped (LOD on), cubic fails the §1 thresholds at every speed except v6000 frame time, where both sides are saturated. On the intended path (LOD off, greedy on) cubic beats the control on frame time (−5% to −25%) and manager time (−33% to −47%) and matches its warmup, but still misses catch-up (1.6-3.2×) and memory (systems +83% to +116%, renderer GPU 9×). See §12.2 |
| C4 | **Bug** | KC5 confirmed, see §12.3. The aimed block often survives while neighbours disappear. Block-centre targeting would need 2 clicks per full-density block, and open-air placement takes 2 clicks. A 2×2×2 pit dug across a chunk corner closes cleanly (no seams, `rc_c4_corner_pit`). The pawn dropped into it and landed (z −810, no READY→NOT-READY). Save, Stop, Play: all 9 edited chunks and the pawn position came back, and three probed columns matched *(M5, §14.9: **Pass.** One click is one block on all six face directions, at a chunk corner and with a pickaxe; edits survive a save and reload)* |
| C5 | **Refine** | Steps behave as designed (D5) but the design hurts. `MaxStepHeight` 50 vs a 75 uu block, so `vox.AutoRun` stops at the first one-block rise (W-04 on foot is impossible). Holding Space jumps once (jump fires on press), so every block needs a tap. Jump apex ≈113 uu clears one block, not two. Tapping jump every 0.45 s, a hop-run covered 17,000 uu in 40 s at x ≈ 400,000 with `vox.Debug.Capture` on: 165 samples, collision under the feet every time, no READY→NOT-READY. A physics cube rests on a block top (gap 50 = its half height, speed 0). Swimming to the shore works when the shore block is at water level. Melee from a one-block pit at a dummy on the rim: 10 swings, 0 hits; on the same level it died in 4 (KC12). The player's corpse (`vox.Damage 1000`) rests on the blocks: pelvis 16 uu above the block top, the same as the smooth baseline after R1. (The training dummy despawns on death, and the wolf spawned for this wandered out of melee range, so the player's corpse is the only one tested.) |
| C6 | **Bug** | Props sink and NPCs strand (KC6, KC7), trees are not gameplay (KC9). Working: deer and wolves stand on block tops (gap ≈2 uu) and cross one-block steps (auto-jump "73 uu obstacle"); wolves chased and killed the player across terraces; `vox.NavPath` finds paths (0.14-0.93 ms); a dropped world item rests on a block top (gap 1); a player-placed campfire kit lands exactly on the block top (−1,200); dungeon rooms (`vox.TPRoom Entrance/Boss/Treasure`) are walkable and match the control's same dungeon (both show the carved voxel ceiling above the tile walls; `rc_c6_boss_left` vs `rc_ctrl_boss_left`); the map (`voxel.Map.Capture`, built from analytic height and so mode-independent), save/load and the day/night lighting work |
| C7 | **Bug** with GPU extraction, **Pass** with CPU | KC4 confirmed, see §12.3 (`rc_c7_cpu_scatter` vs `rc_c7_gpu_scatter`) |

### 12.2 Bench comparison

**Focused PIE**, v = traverse speed, 40,000 uu each, cold fresh world per run. "Systems MB" is peak voxel + renderer +
collision + scatter memory (`peakVoxelSystemsMB`).

| Run | Warmup s | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | Peak mesh queue | Peak loaded | Thrash (LOD / neighbour / other) | Voxel MB | Renderer GPU MB | Collision MB | Scatter MB | Systems MB | JSON |
|-----|----------|---------------------------|----------------|------------|-----------------|-------------|----------------------------------|----------|-----------------|--------------|------------|------------|------|
| Control v1500 | 8.8 | 18.2 / 31.8 / 40.0 | 15.6 | 0.17 | 4 | 1,224 | 293 (293 / 0 / 0) | 600 | 29 | 117 | 114 | 814 | `20261013_004246_rcA_ctrl_v1500` |
| Cubic, LOD on (as shipped), v1500 | 21.6 | 30.1 / 50.1 / 75.8 | 29.7 | 12.2 | 683 | 1,208 | 9,443 (6,792 / 2,039 / 612) | 1,019 | 259 | 53 | 350 | 1,590 | `20261013_004851_rcA_cubic_v1500b` |
| **Cubic, LOD off (intended), v1500** | 6.6 | 16.7 / 23.8 / 32.4 | 10.4 | 0.54 | 75 | 1,246 | 2,540 (0 / 2,540 / 0) | 1,245 | 262 | 56 | 221 | 1,757 | `20261013_012338_rcB_nolod_v1500` |
| Control v3000 | 6.8 | 25.2 / 40.5 / 47.9 | 20.4 | 2.1 | 6 | 1,189 | 280 (280 / 0 / 0) | 651 | 28 | 127 | 113 | 896 | `20261013_004433_rcA_ctrl_v3000` |
| Cubic, LOD on (as shipped), v3000 | 22.8 | 35.5 / 62.0 / 78.9 | 32.4 | 20.3 | 618 | 1,223 | 7,650 (5,574 / 2,061 / 15) | 1,045 | 260 | 57 | 362 | 1,674 | `20261013_004357_rcA_cubic_v3000` |
| **Cubic, LOD off (intended), v3000** | 6.4 | 16.7 / 36.5 / 66.0 | 12.8 | 4.0 | 135 | 1,194 | 2,659 (0 / 1,585 / 1,074) | 1,199 | 252 | 60 | 313 | 1,801 | `20261013_012418_rcB_nolod_v3000` |
| Cubic, LOD off, greedy off, v3000 | 6.6 | 16.7 / 37.1 / 63.7 | 13.7 | 4.65 | 161 | 1,198 | 3,201 (0 / 1,560 / 1,641) | 1,198 | 1,003 | 237 | 307 | 2,617 | `20261013_012537_rcB_nolodsimple_v3000` |
| Control v3000, re-run (drift check) | 4.6 | 24.2 / 38.4 / 44.8 | 21.4 | 1.9 | 7 | 1,189 | 280 (280 / 0 / 0) | 758 | 28 | 127 | 113 | 967 | `20261013_012612_rcB_ctrl_v3000` |
| Control v6000 | 7.3 | 38.6 / 56.8 / 60.5 | 31.6 | 6.1 | 49 | 1,190 | 279 (279 / 0 / 0) | 707 | 28 | 119 | 147 | 993 | `20261013_004620_rcA_ctrl_v6000` |
| Cubic, LOD on (as shipped), v6000 | 22.2 | 34.6 / 57.3 / 63.1 | 30.7 | 25.4 | 457 | 1,232 | 7,421 (4,942 / 2,302 / 177) | 1,016 | 260 | 66 | 345 | 1,653 | `20261013_004544_rcA_cubic_v6000` |
| **Cubic, LOD off (intended), v6000** | 6.8 | 16.8 / 50.9 / 63.2 | 16.9 | 9.7 | 121 | 1,195 | 1,895 (0 / 1,329 / 566) | 1,197 | 254 | 68 | 314 | 1,813 | `20261013_012456_rcB_nolod_v6000` |
| Cubic, LOD on, one band, v3000 | 13.1 | 16.7 / 38.1 / 55.0 | 13.9 | 5.2 | 294 | 1,803 | 2,727 (0 / 2,104 / 623) | 1,803 | 252 | 59 | 273 | 2,375 | `20261013_004706_rcA_cubic1lod_v3000` |

All audits reported 0 findings. The first cubic v1500 run (`20261013_004154_rcA_cubic_v1500`) started from a save that
put the pawn 427,000 uu (4.3 km) away, which inflated its unload distances; it was repeated as `v1500b` from a fresh world.

**Against the §1 thresholds (cubic vs the first-pass control):**

| Metric (threshold) | LOD on v1500 | LOD on v3000 | LOD on v6000 | **LOD off v1500** | **LOD off v3000** | **LOD off v6000** | LOD on, one band, v3000 |
|--------------------|--------------|--------------|--------------|-------------------|-------------------|-------------------|-------------------------|
| Frame P95 (+10%) | +58% ✗ | +53% ✗ | +1% ✓ (both saturated) | −25% ✓ | −10% ✓ | −10% ✓ | −6% ✓ |
| Manager P95 (+15%) | +90% ✗ | +59% ✗ | −3% ✓ | −33% ✓ | −37% ✓ | −47% ✓ | −32% ✓ |
| Catch-up (+25%) | 12.2 s vs 0.17 s ✗ | 9.7× ✗ | 4.2× ✗ | 0.54 s vs 0.17 s ✗ | 1.9× ✗ | 1.6× ✗ | 2.5× ✗ |
| Systems memory (+25%) | +95% ✗ | +87% ✗ | +66% ✗ | +116% ✗ | +101% ✗ | +83% ✗ | +165% ✗ |
| Renderer GPU memory | 9× | 9× | 9× | 9× | 9× | 9× | 9× |
| Peak loaded chunks (+25%) | −1% ✓ | +3% ✓ | +4% ✓ | +2% ✓ | 0% ✓ | 0% ✓ | +52% ✗ |
| Warmup (settle a fresh world) | 22 s vs 9 s | 23 s vs 7 s | 22 s vs 7 s | 6.6 s vs 8.8 s | 6.4 s vs 6.8 s | 6.8 s vs 7.3 s | 13 s vs 7 s |
| Audit / fall-throughs | 0 / none ✓ | 0 / none ✓ | 0 / none ✓ | 0 / none ✓ | 0 / none ✓ | 0 / none ✓ | 0 / none ✓ |

On the intended path the cubic frame P50 sits at the 60 fps cap (16.7 ms) at every speed, where the control's is 18-39 ms.
The control re-run at v3000 came within 8% of the first one on every metric, so drift between the passes is small.

**Settled state.** *Initial load:* fresh world, pawn at the spawn, until the queues drain (1,123 chunks generated in
every case). *Land site:* x −470,286, y −300,332 after streaming settles. Renderer numbers come from the renderer's own
counters.

| | Control (smooth) | Cubic, LOD on (as shipped) | **Cubic, LOD off (intended)** | Cubic, LOD off, greedy off |
|---|---|---|---|---|
| Initial load: time to settle | 16 s | 33 s | **17 s** | 17 s |
| Initial load: meshes per chunk | 1.0 (1,125) | 4.6 (5,211) | **2.0 (2,281)** | 2.0 (2,239) |
| Initial load: interior crossfades | 2 | 2,109 | 642 | 620 |
| Initial load: seam jobs scheduled and processed | 10,898 | 23,246, no geometry | 6,140, no geometry | 6,140, no geometry |
| Initial load: resident voxel data | 215 MB | 333 MB (compressed) | **1,123 MB (0 compressed)** | 1,123 MB |
| Land site: loaded / rendered chunks | 1,218 / 473 | 1,237 / 724 | 1,212 / 712 | 1,211 / 711 |
| Land site: triangles | 0.98 M | 3.84 M | 3.78 M | 15.2 M |
| Land site: renderer GPU | 26 MB | 249 MB | 245 MB | 987 MB |

Greedy meshing cuts cubic triangles and renderer GPU memory by 4.0×, and the bench shows collision memory falls 4.0× with
it (60 vs 237 MB), because collision is cooked from the main mesh. Turning LOD off changes nothing in the cubic geometry
(the mesher never read the LOD level), but it removes the LOD re-meshes and crossfades and halves the load time. It also
turns far-chunk compression off, so resident voxel data grows 3.4× (KC11). The far-band slab still applies with LOD off,
so the loaded chunk count stays at the smooth level. Greedy quads share no vertices between faces, so cubic carries
2 vertices per triangle against about 0.6 for smooth.

**Headless CPU sweep** (`Scripts/voxel_bench_sweep.ps1 -Distance 40000 -BenchArgs "start=0:0 dir=1:0 warmup=120"`,
VoxelWorldsTest map, `-VoxelForceCPU`, `-Config` as below). This compares CPU work with no GPU or focus effects; CPU
generation makes every warmup about 40 s.

| Run | Frame P50 / P95 ms | Manager P95 ms | Catch-up s | Peak mesh queue | Thrash (LOD / neighbour / other) | Systems MB | JSON |
|-----|--------------------|----------------|------------|-----------------|----------------------------------|------------|------|
| Control v1500 | 4.0 / 10.8 | 7.8 | 3.8 | 12 | 369 (369 / 0 / 0) | 664 | `20261013_005515_rcA_hl_ctrl_v1500` |
| Cubic, LOD on, v1500 | 23.0 / 45.3 | 38.7 | 17.8 | 242 | 10,940 (5,635 / 3,310 / 1,995) | 1,441 | `20261013_005054_rcA_hl_cubic_v1500` |
| Control v3000 | 6.3 / 14.3 | 11.2 | 14.1 | 4 | 537 (537 / 0 / 0) | 572 | `20261013_005631_rcA_hl_ctrl_v3000` |
| Cubic, LOD on, v3000 | 24.6 / 43.2 | 38.0 | 25.5 | 370 | 10,942 (4,483 / 3,119 / 3,340) | 1,585 | `20261013_005226_rcA_hl_cubic_v3000` |
| Control v6000 | 10.2 / 23.3 | 18.1 | 21.3 | 7 | 523 (523 / 0 / 0) | 605 | `20261013_005750_rcA_hl_ctrl_v6000` |
| Cubic, LOD on, v6000 | 28.7 / 45.7 | 38.5 | 29.4 | 362 | 16,260 (3,636 / 3,114 / 9,510) | 1,651 | `20261013_005354_rcA_hl_cubic_v6000` |
| Cubic, LOD on, one band, v3000 | 2.8 / 9.9 | 6.7 | 19.1 | 240 | 23,278 (0 / 3,055 / 20,223) | 2,438 | `20261013_005926_rcA_hl_cubic1lod_v3000` |
| **Cubic, LOD off, v1500** | 1.8 / 5.9 | 3.4 | 4.2 | 251 | 94,719 (0 / 3,645 / 91,074) | 1,745 | `20261013_012835_rcB_hl_nolod_v1500` |
| **Cubic, LOD off, v3000** | 2.0 / 7.6 | 4.7 | 15.2 | 172 | 55,326 (0 / 3,020 / 52,306) | 1,831 | `20261013_012954_rcB_hl_nolod_v3000` |
| **Cubic, LOD off, v6000** | 2.7 / 9.1 | 5.7 | 21.5 | 154 | 50,074 (0 / 2,665 / 47,409) | 1,835 | `20261013_013111_rcB_hl_nolod_v6000` |
| Cubic, LOD off, greedy off, v3000 | 1.9 / 8.7 | 5.5 | 13.9 | 187 | 58,781 (0 / 3,031 / 55,750) | 2,630 | `20261013_013228_rcB_hl_nolodsimple_v3000` |

Headless confirms both pictures. As shipped, cubic misses frame time, manager time, catch-up and memory at every speed. On the intended path cubic beats the control on frame time (−45% to −61%) and manager time (−57% to −69%), and headless its catch-up is within 1-9% of the control's (4.2 / 15.2 / 21.5 s vs 3.8 / 14.1 / 21.3 s): with CPU generation on both sides, generation rather than cubic meshing sets the pace. In PIE, where generation runs on the GPU, the cubic neighbour cascade is what remains (1.6-3.2×). Memory is 2.6-3.2× the control. **The thrash column overstates churn on cubic:** it counts queue insertions, and the legacy path re-queues a chunk every frame while it waits for a neighbour's voxel data (KC14), which headless, with slower CPU generation, does tens of thousands of times. The executed mesh counts below are the real measure.

**Per-chunk meshing** (headless, `-LogCmds="LogVoxelMeshing Verbose"`, one v3000 run each, CPU meshers; tags
`rcA_hlmesh_*_v3000` and `rcB_hlmesh_*_v3000`; the smooth figures cover interiors only, not seam strips):

| | Meshes in the run | Mesh ms mean / p50 / p95 / max | Triangles per non-empty chunk, mean / p50 / p95 | Triangles meshed in total |
|---|-------------------|--------------------------------|------------------------------------------------|---------------------------|
| Cubic, LOD on (greedy) | 10,149 (5,914 non-empty) | 26.3 / 24.6 / 46.4 / 133 | 6,990 / 5,960 / 18,452 | 41.3 M |
| Control (Dual Contouring) | 3,572 (1,511 non-empty) | 13.8 / 1.3 / 51.1 / 113 | 9,336 / 2,160 / 41,598 | 14.1 M |
| **Cubic, LOD off (greedy)** | 5,729 (3,131 non-empty) | 29.5 / 26.7 / 53.3 / 105 | 5,688 / 3,508 / 16,642 | 17.8 M |
| Cubic, LOD off, greedy off (simple) | 6,504 (3,560 non-empty) | 14.3 / 13.3 / 34.1 / 62 | 20,962 / 16,926 / 46,356 | 74.6 M |

A near cubic chunk costs about the same as a near smooth chunk, but every cubic chunk is a near chunk, so the median cubic job is 19-20× the smooth median. Over the same route the meshing CPU time was 49 s for smooth interiors, 266 s for cubic with LOD on and 169 s with LOD off (1.8× fewer meshes). Greedy meshing costs about 2× the CPU per chunk of simple meshing and cuts its triangles 3.7× (4.0× in the settled frame).

### 12.3 KC1-KC9: confirmed or dismissed

| ID | Verdict | Evidence |
|----|---------|----------|
| KC1 | **Confirmed as traced, but reframed**: the cubic mesher was never meant to use LOD, and the cost comes from the content shipping with the LOD system on. **Fixed by D7 (§12.7)**: cubic ignores `bEnableLOD` | Cubic's design answer to distance is greedy meshing with `bEnableLOD` off, and both cubic configs ship with it on (§12 intro). As shipped: far terrain is meshed block by block at every distance (`rc_w04x_1`, `rc_c2_bands_north`), yet LOD changes still re-mesh it, 4,942-6,792 LOD re-meshes per 40,000 uu run against 279-293 for the control, each crossfading an identical mesh (4,788 interior crossfades in the first 155 s of play, about 16,700 by t = 670 s). The initial load meshes each chunk 4.6 times and takes 33 s. **With `bEnableLOD` off** that cost disappears: 0 LOD re-meshes, 2.0 meshes per chunk, a 17 s load, and frame and manager time below the control at every speed (§12.2). What remains is inherent to full-detail cubic and not a LOD bug: a settled frame holds 3.9× the triangles and 9× the renderer GPU memory, and the median cubic mesh job costs 19× the smooth median, because every chunk is a full-detail chunk. Greedy meshing is what keeps that at 9× and not 38× (greedy off: 987 MB at the land site). One trap: turning LOD off also turns far compression off (KC11) |
| KC2 | **Confirmed** as a cost, and on the intended path it is the main remaining streaming cost; the correctness risk is **dismissed** for what this session exercised | The legacy whole-chunk path runs for cubic and is correct: no hole, missing or stale face in any survey, R23 clean, audits 0, chunk-corner edits closed. Cost: with LOD off, its neighbour re-mesh cascade is the real re-mesh cost left: 1,329-2,540 neighbour re-meshes per run against 0 for the control, 2.0 meshes per chunk on the initial load, and catch-up 1.6-3.2× the control's in PIE (as shipped it adds 2,039-2,302 on top of the LOD re-meshes). Its per-frame deferral, waiting for neighbour data, is what inflates the bench's "other" count (KC14). The seam registry still tracks every cubic chunk (1,237 chunks, 10,774 seams, 4,040 dirty) and its P0 stub processor drains the jobs with no geometry (`VoxelChunkManager.cpp:4110-4114`): 6,140 jobs on the initial load with LOD off, 23,246 with LOD on (smooth: 10,898 real ones). No streaming test covers the path (C1) *(Fixed in M5, §14.9: cubic meshes on the seam path and the legacy path is deleted; catch-up and manager P95 now within the control's)* |
| KC3 | **Confirmed** (dead path); the test hazard is **fixed** by T5 | The cubic branch always builds `FVoxelCPUCubicMesher` and never reads `bUseGPUMeshing` (`VoxelChunkManager.cpp:1110-1126`); every PIE logged "Using Cubic mesher". The GPU cubic tests pass in-editor, skip headless, and `Performance` returns *(Fixed in M5, §14.9: the GPU cubic mesher and its tests are deleted)* |
| KC4 | **Confirmed** | Same site, LOD0, instances within 6,000 uu. `Review_Cubic` (CPU extraction): every instance on a block top, rocks 100% on face centres, grass and mushrooms jittered inside the face (median 7-31 uu from the centre, maximum 36 < 37.5). `Review_Cubic_GPUScatter`: every instance still on a block top, but 24-25% of rocks sit on block corners (greedy-quad vertices); grass and mushrooms are pushed to face edges (median 25-33 uu); and density falls to about a third for grass (217-247 instances vs 655-725) and half for rocks (151-177 vs 288-317). All 8 definitions are forced to `BlockFaceSnap` (logged), but the GPU branch runs before the cubic check *(Fixed in M5, §14.9: cubic always extracts scatter on the CPU, and `BlockFaceSnap` is read)* |
| KC5 | **Confirmed**, worse than described | *Targeting:* `floor(ImpactPoint)` picked the air voxel for +X and +Z faces every time, and also for a −Y face: the impact lies exactly on the voxel boundary, so floating-point noise picks the side ("Voxel destroyed at [-6435,-4664,-16]" for a block at y −4663). *Brush:* digging is a density delta, not a block. A near-threshold surface block went in 1 click. On the next block down, the aimed block survived until click 4 while its +X neighbour went at click 1, and 4 blocks (a plus shape) were gone after 5 clicks. On a +X face the aimed block went at click 2 and a neighbour at click 3. Aiming at the block centre took 2 clicks with no collateral. Placement targets the right voxel (offset along the normal) but adds +80 density: 1 click beside terrain, 2 in open air (the first click is invisible). *Demo:* in edit mode LMB also fires `AVoxelWorldTestActor`'s mouse brush (radius 300, smooth, `bUseDiscreteEditing` off). One press removed 6 scattered blocks around the aim point and left the aimed block (`rc_c4_lmbkey1`) *(Fixed in M5, §14.9: one target rule, `GetEditVoxelCoord`; whole-block edits)* |
| KC6 | **Confirmed** | Anything placed from the smooth or analytic surface sits up to one block low. Tiled dungeon: pad height −223, block top −150; the brazier, stone pillar and elevator call post stand at −223, 73-74 uu inside the block (`rc_c6_brazier_sunk`, `rc_c6_pillar_sunk`). Generated campsite: actor at −1,244 on a −1,200 block top; the campfire mesh is entirely inside the block (its top is −1,213) and the bench shows 2 uu (`rc_c6_campsite_close`). The control places the same dungeon and campsite at the same heights, and there they sit on the ground: props 0.4-0.6 uu from a −223.5 pad, campsite 0.3 uu above −1,244.0. Not affected: anything placed by trace or physics (spawn, NPCs, world items, a player-placed campfire kit, the physics cube) |
| KC7 | **Partly confirmed** | Player: every block is a jump (D5), so auto-run and a held jump stall at the first rise (C5). NPCs: one-block steps work (deer and wolves log "jumping a 73 uu obstacle"; wolves reached and killed the player across terraces). Two-block rises strand them: with the player on a 3×3 pillar two blocks high, two wolves stood at its base looping "stuck … blocking ahead and repathing" and never reached it. That is consistent with the nav/auto-jump mismatch (`voxel.Nav.MaxStepRise` 100 checked against interpolated heights), but the session did not isolate a path that nav accepted and movement refused. (At the one site checked, nav's surface sample under the player was identical in both modes, −1,215.85, so nav reads the same interpolated surface in cubic and smooth, not the block tops) |
| KC8 | **Confirmed** | No map references `DemoCubicWorldConfig` or `Config/CubicScatterConfig` (each is referenced only by itself; `DemoCubicScatterConfig` is used by the review configs). Before this review no PIE bench had recorded a cubic run. `Review_Cubic`, `_GPUScatter` and `_SingleLOD` are now the cubic test content |
| KC9 | **Confirmed**, worse than described | Cubic worlds get voxel trees (`TreeMode` VoxelData, 3 templates, density 5.1) and the cubic scatter config has no tree definition, so there is no HISM tree to chop. Swinging at a voxel tree does nothing: no harvestable scatter, and no dig without edit mode or a digging tool. Digging a trunk voxel yields Wood (material 20), 1 per dig, forever on the same voxel (G3). Nothing fells; the canopy stays. In `Review_Cubic` the canopies also render wrong (KC10, a config issue) |

### 12.4 New findings

Cubic:

| ID | Finding | Evidence | Impact |
|----|---------|----------|--------|
| KC10 | **`DemoSmoothWorldConfig`'s voxel tree templates use material IDs that do not exist.** Leaves are 22 and 23, but the registry has 22 materials (0-21: Wood 20, Leaves 21), and template 2's trunk is 21 (Leaves). The templates are inert in smooth (voxel trees are cubic-only), so this only shows when the config is meshed cubic, as `Review_Cubic` is: canopies render as opaque clay and brick blocks. `DemoCubicWorldConfig` uses 20 and 21 correctly. Nothing validates template material IDs against the registry | `Review_Cubic` / `DemoSmoothWorldConfig` / `DemoCubicWorldConfig` tree templates; `VoxelCore/Public/VoxelMaterialRegistry.h:30-34`; `rc_c6_tree_found` | Latent content bug; the review's tree screenshots show it. The canopies are not treated as leaves (not masked, not non-occluding) |
| KC11 | **Far compression is keyed on the LOD level, so the intended cubic path has none.** Compression skips chunks below `voxel.FarCompression.MinStride`, and with `bEnableLOD` off every chunk is LOD 0. The far-band slab is keyed on band 0's distance instead, so it survives `bEnableLOD` off (but not a one-band config) | `VoxelChunkManager.cpp:4677`; `DistanceBandLODStrategy.cpp:956-962`. LOD off: 0 compressed, 1,123-1,245 MB resident voxel data against 215 MB (spawn) and 600-760 MB (bench peak) for the control, loaded chunks unchanged. One band: no slab either, +47% chunks, 1,803 MB | The intended cubic path's main memory cost. **Fixed (§12.7)**: compression keys on the distance band |
| KC12 | **Melee misses across a one-block height difference.** From a one-block pit, 10 swings at a dummy on the rim did no damage; on the same level it died in 4 | C5 | Common in cubic terrain |
| KC13 | **The edge of the loaded region shows vertical walls.** A missing neighbour counts as air, so boundary faces are emitted. Visible only from beyond the view distance | `VoxelCPUCubicMesher.cpp:930-1001`; `rc_r23_siteB_east` | Cosmetic *(Fixed by WM-C1, §14.9: the edge is open, as on the control)* |
| KC14 | **The legacy path re-queues waiting chunks every frame, and the bench counts each one as a re-mesh.** When a chunk's face neighbour has no voxel data yet, the legacy path (cubic only; the seam path never waits) defers it and re-queues it at the end of every frame with the default reason "other". The bench's thrash counter counts queue insertions of already-meshed chunks, so each frame's wait counts once. Traced after the bench runs | `ShouldDeferMeshForNeighbors` under `!bSeamMeshingActive`, `VoxelChunkManager.cpp:3051-3066`; re-queue at `:3383-3386`; counter at `:5798-5803`. "Other" is 0 for smooth in every run; for cubic 0-1,074 in PIE and up to 91,074 headless, where CPU generation keeps neighbours waiting longer. The verbose run that counted 53,799 cubic re-meshes executed 5,729 meshes | Mostly a measurement artefact: queue churn and delayed re-meshes rather than wasted meshing. The wait itself is part of KC2's catch-up cost *(Gone with the legacy path in M5, §14.9)* |

Mode-independent, found on the way:

| ID | Finding | Evidence |
|----|---------|----------|
| G1 | `UVoxelWorldComponent::GetTotalVertexCount`, `GetTotalTriangleCount` and `GetGPUMemoryUsage` (BlueprintPure) only ever add, so they report cumulative uploads: 54.7 M "vertices" at a site where, 30 s later, the renderer held 7.7 M. The renderer's own `GetDebugStats` counters are right | `VoxelRendering/Private/VoxelWorldComponent.cpp:315-317`, `:381-383` |
| G2 | A Subtract on a voxel that holds a Set edit **replaces** the Set instead of composing with it, so the voxel reverts to procedural minus the delta. POI pads are written with Set. One dig under a Set slab removed 5 blocks (the target and its 4 face neighbours, which only received the −21 falloff) | `VoxelCore/Private/VoxelEditManager.cpp:1072`; `VoxelWorldPOI/.../POIPlacementLibrary.cpp:169` |
| G3 | Gathering pays out on every dig whose brush modifies anything, and a dug voxel keeps its material ID, so one voxel yields without limit (6 digs at one trunk voxel, 6 Wood) | `VoxelCharacterPlugin/.../VCPlayerController.cpp:1344-1350`, `:1419-1431` |
| G4 | `voxel.Bench.Run … follow=` follows the seabed under lakes, so a long follow run drowns the pawn mid-run (tooling) | `c2_w04_follow` run |
| G5 | The bench's thrash counter counts meshing-queue insertions of already-meshed chunks, including deferral re-queues, so on a path that defers it reports many times the meshes actually run (KC14). Smooth does not defer, so its numbers are unaffected (tooling) | `VoxelChunkManager.cpp:5798-5803` |
| G6 | *(found with §12.7)* **Far-compression encoding is the largest single item in the chunk manager's P95 while moving, in both modes.** The sweep encodes up to `voxel.FarCompression.MaxScansPerTick` (4) chunks per tick on the game thread, and an Oodle encode of a 1 MB chunk costs about 3.5 ms in PIE. It is not timed separately, so it shows as the unattributed manager time: during the v3000 traverse, P95 14.9 ms for the smooth control, 13.9 ms for cubic with compression and 0.1 ms for cubic without it. A time budget, or encoding on a worker thread, would take it off the frame for both modes. **Fixed (§12.8)**: the encode runs on a worker; the sweep's P95 is 0.6-0.9 ms in PIE | `ProcessFarCompressionSweep`; the per-frame CSVs of the §12.7 runs (unattributed = `TotalMs` minus the timed sections) |

### 12.5 Recommendation for D1

**Support cubic as terrain-only** (meshing, streaming, editing, scatter), and promote it to first-class only when a game
needs it. Do not drop it.

- **Why not drop:** the terrain layer is correct, and on its intended path (greedy, LOD off) it is already faster than
  smooth per frame. Streaming, collision, edits at chunk corners, save/load and CPU scatter all passed. Dropping cubic
  would let the legacy neighbour-slice path and GPU cubic mesher be deleted (KC2, KC3), but that is cleanup, not a reason.
- **Why not first-class now:** the game systems were built for a smooth surface. They fail in cubic for three root
  causes: the surface height query (KC6, which sinks POI props and campsites), the block step policy (KC7, KC12) and
  trees as gameplay (KC9). Each is bounded, but together they double the QA surface of every game feature for a mode no
  game uses yet.
- **Terrain-only work list** (for the second revision to size):
  - ~~*Force LOD off for cubic* (D7): `MeshingMode == Cubic` overrides `bEnableLOD`, so a cubic config cannot pay for
    LOD it does not use (KC1). Do it together with the next item, or voxel memory rises.~~ **Done 2026-10-13, §12.7.**
    Still open: refresh or delete `DemoCubicWorldConfig` and the orphaned scatter config (KC8).
  - ~~*Far compression keyed on distance, not LOD level*, so the LOD-off path keeps it (KC11). This is the main memory
    lever.~~ **Done 2026-10-13, §12.7:** settled cubic voxel data 1,123 → 127 MB (smooth control 134 MB).
  - *Take cubic off the legacy neighbour cascade:* move it onto the seam path (face culling across a chunk pair is
    "trivially compatible") or give it a cheaper boundary rule, and stop the seam registry scheduling stub jobs for it;
    add a cubic streaming test (KC2). This is the main catch-up lever, and it removes the per-frame deferral loop (KC14).
  - *Block-accurate editing:* dig and place whole blocks in the player path, and `bUseDiscreteEditing` on the demo brush
    in cubic (KC5).
  - Route cubic to CPU scatter extraction (KC4); delete GPU cubic or keep it test-only (KC3); fix the smooth config's
    tree material IDs and validate template IDs against the registry (KC10).
- **First-class adds:** one block-top surface query used by POI pads and props, campsites and navigation (KC6), with
  cubic pads and stepped aprons; NPC handling of two-block rises and a melee height tolerance (KC7, KC12); choppable
  voxel trees (KC9).
- **Performance on the intended path:** frame and manager time and load time already meet the thresholds. Catch-up is
  within 9% of the control headless, where CPU generation sets the pace, but 1.6-3.2× in PIE, where the cubic neighbour
  cascade is left as the bottleneck: that waits on the KC2 work. Voxel memory waits on KC11 *(done, §12.7; compression
  brings smooth's encode cost with it, so cubic's frame-time lead over smooth is gone at v3000, see there; the encode moved
  to a worker in §12.8, which helped smooth more than cubic)*. Renderer GPU memory stays
  about 9× smooth however the pipeline is tuned, because every cubic chunk is full detail; greedy already takes it down
  from about 38×. Parity on that metric would need a far representation for cubic (a coarse far mesh or impostor) or a
  shorter cubic view distance. Both are design choices for the second revision, not bugs.
- **Mode-independent findings** G1-G3 and G6 apply to smooth as well and can be fixed independently of D1 (G6 is fixed,
  §12.8); G4-G5 are bench tooling.

### 12.6 Where cubic and smooth separate

Cubic and smooth share one density field and one streaming pipeline. The code branches on the meshing mode in only a
handful of places: the mesher, the renderer's material mode, voxel-tree injection and scatter extraction. Nothing in
the game plugins (POI, character, debug) looks at the meshing mode at all. Every Review A failure sits in a layer that is
*shared* but quietly assumes one mode: the LOD-keyed systems assume smooth's distance bands, and the surface consumers
assume the density isosurface, while the cubic surface is the block tops above it.

| Layer | Smooth (Dual Contouring) | Cubic | Where it splits, or doesn't | Findings |
|-------|--------------------------|-------|-----------------------------|----------|
| Density field, generation, caves, water, POI stamps | Shared | Shared | Not split | — |
| Voxel trees | Injected only in cubic; smooth uses HISM trees from scatter | Injected into the voxel data | `VoxelChunkManager.cpp:2687-2691`, `:2886-2891` | KC9, KC10 |
| Mesher | CPU Dual Contouring interiors (GPU DC retired) | CPU cubic, greedy or simple; never reads the LOD level | `VoxelChunkManager.cpp:1031-1126` | KC1, KC3 |
| Chunk boundaries | Seam-ownership pipeline: single-owner seam jobs | Legacy whole-chunk path: neighbour slices and a re-mesh cascade when neighbours load; a missing neighbour counts as air | Gate by mesher name, `VoxelChunkManager.cpp:447-456`; `VoxelCPUCubicMesher.cpp:930-1001` | KC2, KC13 |
| Seam registry | Drives the seam jobs | Still tracks every chunk; a P0 stub drains its jobs with no geometry | `VoxelChunkManager.cpp:4110-4114` | KC2 |
| LOD system | Distance bands, stride per band, 2:1 balance, seam holds, crossfades | **Off** (D7, done §12.7): `UVoxelWorldConfiguration::IsLODEnabled()` is false for cubic whatever `bEnableLOD` says; greedy meshing is cubic's LOD mechanism. Before D7 it ran in full when `bEnableLOD` was on, with morphing forced off | `IsLODEnabled()` read by every `bEnableLOD` gate (`DistanceBandLODStrategy::Initialize`, `VoxelChunkManager.cpp`) | KC1 |
| Far-chunk compression | Keyed on the chunk's distance band (its LOD level) | Keyed on the distance band its distance falls in (`GetDistanceBandLOD`, done §12.7). Before: off on the intended path (all chunks LOD 0) | `ProcessFarCompressionSweep` | KC11 |
| Far-band slab | Beyond band 0's distance | Same; works with LOD off | `DistanceBandLODStrategy.cpp:956-962` | KC11 |
| Collision | Cooked from the main mesher | Same code; cooks the greedy mesh (4× smaller than simple) | `VoxelCollisionManager.cpp:775-843` | — |
| Rendering | Triplanar material | Atlas material (per-face UVs) | `VoxelCustomVFRenderer.cpp:85` | — |
| Scatter | GPU extraction at LOD 0, CPU beyond | Definitions forced to `BlockFaceSnap`; the CPU path has a cubic extractor, the GPU path does not | `VoxelScatterManager.cpp:107-116`, `:1885`, `:2049`; GPU branch first at `:2000-2038` | KC4 |
| Surface queries: spawn, POI pads and props, campsites, nav, map | Density or analytic surface, which is the rendered surface | Same query, but the rendered surface is the block top up to one voxel above it | Not split | KC6, KC7 |
| Editing | Density brush (dent) | Same brush, aimed at `floor(ImpactPoint)`; no whole-block mode in the player path | Not split (the demo brush's `bUseDiscreteEditing` is the only block-aware path) | KC5, G2 |
| Movement | Slopes walkable | 75 uu steps over a 50 uu `MaxStepHeight` (jump per block, by design) | Not split | KC7, KC12 |

So the separation points to design deliberately are three. **Distance**: cubic opts out of LOD, so the systems that ride
on LOD (compression, and the slab in one-band setups) need a distance key of their own. Since §12.7 compression has
one; the slab is keyed on band 0's distance already and only needs a band 0 that ends before the view distance.
**Boundaries**: cubic needs a boundary rule of its own (seam jobs or a cheaper face-cull exchange) instead of the
legacy cascade. **The surface**: a
cubic surface query (block top) and block-accurate editing for the consumers that today read the density surface.

### 12.7 D7 and KC11 implemented (2026-10-13)

**What changed.**

- **Cubic forces LOD off (D7).** `UVoxelWorldConfiguration::IsLODEnabled()` returns `bEnableLOD && MeshingMode != Cubic`,
  and every reader of `bEnableLOD` now calls it: `FDistanceBandLODStrategy::Initialize` and the seven gates in
  `VoxelChunkManager.cpp`. The asset is not modified, so the same config meshed smooth keeps its LOD; in the editor
  `bEnableLOD` is greyed out for cubic. The chunk manager logs `LOD system disabled for cubic mode (bEnableLOD is
  ignored; greedy meshing replaces LOD)` once at initialisation. The bands stay in use as distances.
- **Far compression keyed on distance (KC11).** New `IVoxelLODStrategy::GetDistanceBandLOD`: with LOD on it is the
  chunk's LOD, with LOD off the band the chunk's distance falls in. The sweep uses the chunk's LOD level exactly as before
  when LOD is on, so smooth is unchanged, and `GetDistanceBandLOD` when it is off. A chunk compresses once its band's
  stride reaches `voxel.FarCompression.MinStride` (4): beyond 14,000 uu with the demo bands, in both modes.
- **Hold for the legacy path.** The first pass showed the legacy boundary path (cubic only) decoding the same compressed
  neighbours on every job of a re-mesh cascade: it reads up to 26 neighbours straight from their descriptors, and the
  sweep compacted them again on the next tick. Neighbour-slice time rose from P95 1.4 to 4.8 ms and manager P95 from 4.7
  to 12.7 ms (headless v3000). The sweep now holds a chunk expanded from its cached compact form resident for
  `voxel.FarCompression.IdleFrames` before compacting it again, on the legacy path only (the seam pipeline reads through
  its snapshot cache). With the hold: 1.8 ms and 8.4 ms (smooth control 8.1 ms). It costs about 230 MB of peak voxel
  data headless and nothing once settled.
- The bench report's `run{}` gains `lodEnabled` (the effective value). Tests: `VoxelWorlds.LOD.LODOff.CubicForcesOff` and
  `.DistanceBands`. Full `VoxelWorlds+VoxelWorldSave` suites 209/209, headless and in-editor (where the GPU tests ran).

**Method.** As in §12.2: a single editor in the foreground, background throttle off, PIE in the level viewport, one
cold fresh world per run, `voxel.Bench.Run <tag> <v> 40000 start=0:0 dir=1:0 warmup=120`. Cubic is `Review_Cubic`,
whose asset still has `bEnableLOD` on: before this change it ran the LOD system (§12.2 "LOD on (as shipped)"), and now
it must behave like `Review_Cubic_NoLOD`. The JSON reports are in [Research/Bench/D7/](Research/Bench/D7/).

**PIE** (§12.2 rows for comparison, in italics):

| Run | Warmup s | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | Peak mesh queue | Thrash (LOD / neighbour / other) | Peak / end voxel MB | End resident / uniform / compressed | Systems MB | JSON |
|-----|----------|---------------------------|----------------|------------|-----------------|----------------------------------|---------------------|-------------------------------------|------------|------|
| *Control v3000 (§12.2)* | *6.8* | *25.2 / 40.5 / 47.9* | *20.4* | *2.1* | *6* | *280 (280 / 0 / 0)* | *651 / 607* | *592 / 62 / 535* | *896* | *`20261013_004433_rcA_ctrl_v3000`* |
| *Control v3000 re-run (§12.2)* | *4.6* | *24.2 / 38.4 / 44.8* | *21.4* | *1.9* | *7* | *280 (280 / 0 / 0)* | *758 / 599* | *584 / 62 / 543* | *967* | *`20261013_012612_rcB_ctrl_v3000`* |
| Control v3000 | 5.5 | 25.7 / 41.0 / 49.2 | 22.8 | 2.4 | 7 | 280 (280 / 0 / 0) | 709 / 595 | 580 / 63 / 546 | 916 | `20261013_022543_d7_ctrl_v3000` |
| Control v3000 re-run | 5.7 | 25.2 / 42.0 / 48.4 | 21.1 | 2.1 | 7 | 280 (280 / 0 / 0) | 679 / 611 | 596 / 62 / 531 | 890 | `20261013_022843_d7_ctrl_v3000b` |
| *Cubic LOD off v1500 (§12.2)* | *6.6* | *16.7 / 23.8 / 32.4* | *10.4* | *0.54* | *75* | *2,540 (0 / 2,540 / 0)* | *1,245 / 1,189* | *1,189 / 0 / 0* | *1,757* | *`20261013_012338_rcB_nolod_v1500`* |
| **`Review_Cubic` v1500** | 8.3 | 16.7 / 28.1 / 36.1 | 16.2 | 0.77 | 74 | 3,286 (0 / 2,686 / 600) | 1,082 / 724 | 706 / 53 / 430 | 1,518 | `20261013_022723_d7_cubic_v1500` |
| *Cubic LOD on v3000 (§12.2, as shipped)* | *22.8* | *35.5 / 62.0 / 78.9* | *32.4* | *20.3* | *618* | *7,650 (5,574 / 2,061 / 15)* | *1,045 / 828* | *818 / 29 / 342* | *1,674* | *`20261013_004357_rcA_cubic_v3000`* |
| *Cubic LOD off v3000 (§12.2)* | *6.4* | *16.7 / 36.5 / 66.0* | *12.8* | *4.0* | *135* | *2,659 (0 / 1,585 / 1,074)* | *1,199 / 1,189* | *1,189 / 0 / 0* | *1,801* | *`20261013_012418_rcB_nolod_v3000`* |
| **`Review_Cubic` v3000** | 8.3 | 19.6 / 43.8 / 64.8 | 20.5 | 5.4 | 193 | 2,540 (0 / 1,508 / 1,032) | 1,070 / 852 | 840 / 42 / 307 | 1,530 | `20261013_022629_d7_cubic_v3000` |
| *Cubic LOD off v6000 (§12.2)* | *6.8* | *16.8 / 50.9 / 63.2* | *16.9* | *9.7* | *121* | *1,895 (0 / 1,329 / 566)* | *1,197 / 1,189* | *1,189 / 0 / 0* | *1,813* | *`20261013_012456_rcB_nolod_v6000`* |
| **`Review_Cubic` v6000** | 7.7 | 22.9 / 46.3 / 58.7 | 23.1 | 11.4 | 145 | 1,959 (0 / 1,360 / 599) | 1,088 / 1,034 | 1,023 / 24 / 143 | 1,645 | `20261013_022803_d7_cubic_v6000` |

All audits reported 0. Every cubic run logged the force-off line, and its report says `lodEnabled: false`.

**Settled state** (fresh world at the spawn, `voxel.FarCompression.Stats` after the initial load drains):

| | Smooth control | Cubic before (LOD off, §12.2) | **Cubic after** |
|---|---|---|---|
| Time to settle | 14 s | 17 s | 17 s |
| Resident voxel data 2-3 s after the drain | 453 MB (439 resident, 619 compressed) | 1,123 MB (0 compressed) | 718 MB (709 resident, 366 compressed) |
| Resident voxel data 10 s later / 45 s later | 134 / **134 MB** (112 / 79 / 932) | 1,123 MB | 129 / **127 MB** (105 / 81 / 937) |

After a 22,000 uu move from a settled cubic world, the audit reports 0 and the survey shows no holes or missing faces
where new chunks meshed against compressed neighbours.

**Headless CPU sweep** (VoxelWorldsTest, `-VoxelForceCPU`, the §12.2 arguments). No editor was open during these runs,
so frame and catch-up times are lower than §12.2's across the board, the control's included; compare memory and counts.

| Run | Frame P50 / P95 ms | Manager P95 ms | Catch-up s | Thrash (LOD / neighbour / other) | Peak / end voxel MB | End resident / uniform / compressed | Systems MB | JSON |
|-----|--------------------|----------------|------------|----------------------------------|---------------------|-------------------------------------|------------|------|
| *Control v3000 (§12.2)* | *6.3 / 14.3* | *11.2* | *14.1* | *537 (537 / 0 / 0)* | *251 / 196* | *172 / 103 / 914* | *572* | *`20261013_005631_rcA_hl_ctrl_v3000`* |
| Control v3000 | 3.6 / 10.8 | 8.1 | 8.5 | 535 (535 / 0 / 0) | 236 / 192 | 168 / 103 / 918 | 572 | `20261013_020144_d7_hl_ctrl_v3000` |
| `Review_Cubic` v1500 | 1.3 / 8.8 | 6.9 | 0.6 | 151,665 (0 / 4,669 / 146,996) | 924 / 437 | 413 / 83 / 693 | 1,379 | `20261013_021932_d7h_hl_cubic_v1500` |
| `Review_Cubic` v3000 | 1.5 / 11.2 | 8.4 | 10.1 | 52,149 (0 / 3,126 / 49,023) | 954 / 640 | 621 / 61 / 507 | 1,423 | `20261013_022045_d7h_hl_cubic_v3000` |
| `Review_Cubic` v6000 | 2.0 / 15.2 | 11.9 | 16.0 | 29,626 (0 / 2,708 / 26,918) | 937 / 760 | 745 / 56 / 388 | 1,519 | `20261013_022156_d7h_hl_cubic_v6000` |
| `Review_Cubic` v3000, no hold (first pass) | 1.9 / 15.4 | 12.7 | 11.4 | 40,612 (0 / 2,914 / 37,698) | 700 / 596 | 581 / 76 / 532 | 1,289 | `20261013_021355_d7_hl_cubic_v3000` |

The §12.2 headless cubic LOD-off rows held 1,189 MB of voxel data from start to end with nothing compressed. The headless
"other" thrash is KC14's per-frame deferral re-queue, not executed meshes.

**Verdict.**

- **D7 works as intended.** A cubic config with `bEnableLOD` on now runs the intended path: 0 LOD re-meshes, a
  neighbour/other thrash split that matches `Review_Cubic_NoLOD`, and the same loaded chunk count and renderer memory.
- **KC11 is fixed.** Settled cubic voxel data is 127 MB against the control's 134 MB (it was 1,123 MB). While moving, cubic
  still holds more than smooth (end of run 724-1,034 vs 595-611 MB): the legacy path keeps chunks in its mesh queue and
  re-meshes them through the neighbour cascade (KC2), and a chunk compresses only after `IdleFrames` settled.
- **Smooth is unchanged.** The control's compressed, uniform and resident counts, thrash and peak memory sit inside the
  spread of the two §12.2 control runs, in PIE and headless.
- **The cost: compression brings its encode with it (G6).** Cubic now pays the encode that smooth already pays, about
  14 ms of manager P95 while moving. Against the first-pass control, cubic's frame P95 is −12% (v1500), +8% (v3000) and
  −18% (v6000), inside the +10% threshold, and its manager P95 is +4%, 0% and −27%. Most of §12.2's −10% to −25%
  frame-time lead is gone; taking the encode off the game thread (G6) would restore it, for smooth as well. Catch-up rises
  by 0.2-1.7 s and still misses its threshold, which is KC2's to fix. *(G6 done in §12.8: it helped smooth more than
  cubic, so the lead did not come back.)*

### 12.8 G6 fixed: the compression encode runs on a worker (2026-10-13)

**What changed.**

- **The encode runs on a worker thread.** For a chunk that needs a fresh encode, the sweep copies its array into a
  pooled scratch array and starts a background `UE::Tasks` task (`BackgroundNormal`, outside the thread pool that runs
  generation, meshing and seam jobs). The task runs `FChunkDescriptor::ComputeCompactForm` on the copy: the uniform scan,
  then Oodle. At the start of the next sweep the game thread installs each finished result with `InstallCompactForm`.
  `TryCompress` is now those two calls in sequence, so the in-place path behaves as before.
- **Install only an unchanged array.** At dispatch the descriptor gets a ticket (`AsyncCompactTicket`). Every write path
  (`SetVoxel`, `SetVoxelByIndex`, `GetVoxelDataMutable`, `SetResidentVoxelData`, `AllocateVoxelData`, `ClearVoxelData`)
  and every residency change clears it. A result whose ticket no longer matches is discarded and counted as stale.
  `ContentVersion` alone would not do: water-flag propagation writes through `GetVoxelDataMutable()` without bumping it,
  and a reloaded chunk starts its count again.
- **A chunk that stopped qualifying keeps the result.** If the chunk started a re-mesh or moved inside the stride band
  while its encode ran, the result still matches the array. It is stored as the chunk's cached compact form (the uniform
  value or the retained buffer), and the sweep re-applies it for free once the chunk qualifies again. The legacy-path
  hold (`FarExpandedSeenFrame`) and the free re-apply path are unchanged.
- **Limits.** `voxel.FarCompression.MaxScansPerTick` (4) now limits encodes started per tick. The new
  `voxel.FarCompression.MaxInFlight` (8) limits encodes in flight, and with them the copies (1 MB each). Copies in flight
  or in the pool count as voxel data in the memory stats, and the pool is freed after `IdleFrames` with nothing
  dispatched. `voxel.FarCompression.Async 0` restores the game-thread encode for A/B.
- **Bench.** The sweep is a timed section of its own: CSV `FarCompMs`, `FarCompDispN`, `FarCompApplyN`; JSON
  `farCompressMsP50/P95/P99` over the traverse, `farCompressEncodes/Installed/Stale` for the session, and
  `run.farCompression` / `run.farCompressionAsync`. The HUD and `voxel.FarCompression.Stats` show it too.
  `voxel_bench_sweep.ps1` takes `-PreCmds` for cvar A/B runs. Tests: `VoxelWorlds.Compression.Async.MatchesSyncCompress`,
  `.TicketVoidedByWrites`, `.CacheOnlyInstall`. The `VoxelWorlds+VoxelWorldSave` suites pass 212/212, headless and
  in the editor, where the GPU tests run.

**Why a worker and no time budget.** One encode costs about 3.5 ms and cannot be split. A time budget alone could
therefore get the sweep down to one encode per tick at best, at a quarter of the throughput, and while moving that would
give back part of the memory win. On a worker the encode leaves the frame entirely. What stays on the game thread is a
1 MB copy per dispatch: 0.2 ms in PIE (CSV, mean by dispatch count), about 0.8-1.1 ms on a tick with 4 dispatches. A
time budget on top would save about 0.5 ms at P95 and slow compression down, so there is none. In PIE an encode is
installed one or two ticks after its dispatch. Headless, where CPU generation and meshing keep every core busy, the
background encodes queue and cubic sometimes reaches the 8 in flight; that delays compression, not the frame, and the
headless memory columns below still match or beat §12.7.

**Method.** As in §12.7: one editor (the worktree build, no other editor open) in the foreground, background
throttle off, PIE in the level viewport, a cold fresh world per run, `voxel.Bench.Run <tag> <v> 40000 start=0:0 dir=1:0
warmup=120`. The sync rows are the same build with `voxel.FarCompression.Async 0`. "Sweep" is the new `FarCompressMs`
section, and "unattributed" is what remains of `TotalMs` after every timed section. JSON reports are in
[Research/Bench/G6/](Research/Bench/G6/).

**PIE** (§12.7 rows for comparison, in italics; their sweep was untimed and measured about 14 ms of unattributed P95
at v3000, §12.4):

| Run | Warmup s | Frame P50 / P95 / P99 ms | Manager P95 ms | Sweep P50 / P95 / P99 ms | Catch-up s | Peak mesh queue | Thrash (LOD / neighbour / other) | Peak / end voxel MB | End resident / uniform / compressed | Systems MB | JSON |
|-----|----------|---------------------------|----------------|--------------------------|------------|-----------------|----------------------------------|---------------------|-------------------------------------|------------|------|
| *Control v3000 (§12.7)* | *5.5* | *25.7 / 41.0 / 49.2* | *22.8* | *untimed* | *2.4* | *7* | *280 (280 / 0 / 0)* | *709 / 595* | *580 / 63 / 546* | *916* | *`20261013_022543_d7_ctrl_v3000`* |
| *Control v3000 re-run (§12.7)* | *5.7* | *25.2 / 42.0 / 48.4* | *21.1* | *untimed* | *2.1* | *7* | *280 (280 / 0 / 0)* | *679 / 611* | *596 / 62 / 531* | *890* | *`20261013_022843_d7_ctrl_v3000b`* |
| Control v1500 | 4.1 | 16.7 / 27.4 / 34.5 | 8.6 | 0.06 / 0.73 / 1.27 | 0.17 | 4 | 280 (280 / 0 / 0) | 828 / 398 | 374 / 78 / 737 | 1,039 | `20261013_032658_g6_ctrl_v1500` |
| **Control v3000** | 11.2 | 22.7 / 35.7 / 42.5 | **13.5** | 0.26 / **0.91** / 1.34 | 0.93 | 5 | 537 (537 / 0 / 0) | 777 / 630 | 611 / 60 / 519 | 987 | `20261013_030923_g6_ctrl_v3000` |
| **Control v3000 re-run** | 3.8 | 22.7 / 35.9 / 42.2 | **13.5** | 0.26 / **0.90** / 1.34 | 1.07 | 7 | 280 (280 / 0 / 0) | 782 / 637 | 618 / 59 / 513 | 994 | `20261013_031208_g6_ctrl_v3000b` |
| Control v3000, sync encode | 5.5 | 24.8 / 40.9 / 47.5 | 21.9 | 2.44 / 13.28 / 16.99 | 2.20 | 7 | 280 (280 / 0 / 0) | 684 / 596 | 581 / 63 / 546 | 891 | `20261013_031246_g6_ctrl_sync_v3000` |
| Control v6000 | 4.5 | 33.3 / 47.5 / 54.7 | 20.0 | 0.21 / 0.60 / 0.83 | 5.0 | 9 | 280 (280 / 0 / 0) | 764 / 751 | 736 / 48 / 406 | 1,053 | `20261013_032732_g6_ctrl_v6000` |
| *`Review_Cubic` v1500 (§12.7)* | *8.3* | *16.7 / 28.1 / 36.1* | *16.2* | *untimed* | *0.77* | *74* | *3,286 (0 / 2,686 / 600)* | *1,082 / 724* | *706 / 53 / 430* | *1,518* | *`20261013_022723_d7_cubic_v1500`* |
| **`Review_Cubic` v1500** | 7.9 | 16.7 / 24.1 / 32.2 | **10.9** | 0.03 / **0.60** / 0.98 | 0.71 | 75 | 2,680 (0 / 2,680 / 0) | 1,102 / 729 | 707 / 53 / 429 | 1,534 | `20261013_031056_g6_cubic_v1500` |
| *`Review_Cubic` v3000 (§12.7)* | *8.3* | *19.6 / 43.8 / 64.8* | *20.5* | *untimed* | *5.4* | *193* | *2,540 (0 / 1,508 / 1,032)* | *1,070 / 852* | *840 / 42 / 307* | *1,530* | *`20261013_022629_d7_cubic_v3000`* |
| **`Review_Cubic` v3000** | 7.9 | 16.7 / 40.0 / 67.5 | **15.9** | 0.15 / **0.88** / 1.37 | 5.2 | 176 | 3,173 (0 / 1,577 / 1,596) | 1,076 / 838 | 821 / 42 / 326 | 1,529 | `20261013_031003_g6_cubic_v3000` |
| **`Review_Cubic` v3000 re-run** | 8.8 | 17.5 / 44.3 / 59.1 | **16.3** | 0.13 / **0.88** / 1.23 | 5.1 | 194 | 3,677 (0 / 1,671 / 2,006) | 1,082 / 849 | 833 / 42 / 314 | 1,532 | `20261013_032818_g6_cubic_v3000b` |
| `Review_Cubic` v3000, sync encode | 8.3 | 19.7 / 44.9 / 61.7 | 21.0 | 1.90 / 14.02 / 16.15 | 5.4 | 182 | 2,809 (0 / 1,497 / 1,312) | 1,072 / 858 | 846 / 41 / 302 | 1,529 | `20261013_031331_g6_cubic_sync_v3000` |
| *`Review_Cubic` v6000 (§12.7)* | *7.7* | *22.9 / 46.3 / 58.7* | *23.1* | *untimed* | *11.4* | *145* | *1,959 (0 / 1,360 / 599)* | *1,088 / 1,034* | *1,023 / 24 / 143* | *1,645* | *`20261013_022803_d7_cubic_v6000`* |
| **`Review_Cubic` v6000** | 7.4 | 19.7 / 51.8 / 65.2 | **16.4** | 0.26 / **0.82** / 1.07 | 9.7 | 153 | 1,871 (0 / 1,373 / 498) | 1,098 / 989 | 974 / 29 / 187 | 1,614 | `20261013_031134_g6_cubic_v6000` |

All audits reported 0. Stale results: 0-8 per run out of 800-1,650 encodes. The first control run logged 537 LOD
re-meshes against 278-280 in every other control run. Its warmup started almost at PIE start (24 chunks loaded), so the
initial load's LOD settling fell inside the run.

**Settled-start A/B** (control v3000). In the runs above the memory columns depend on how far the initial load had got
when the bench started: a 3.8 s warmup begins the traverse with the initial load's chunks not yet compressed, an 11.2 s
one with most of them done. For these four runs each bench started only after the initial load had drained and 15 s
more had passed, from 133.7 MB and 932 compressed chunks every time, with the encode alternating async, sync, async,
sync:

| Encode | Frame P95 ms | Manager P95 ms | Sweep P95 ms | Catch-up s | Voxel MB: peak / traverse mean / end | Compressed: traverse mean / end | JSON |
|--------|--------------|----------------|--------------|------------|--------------------------------------|---------------------------------|------|
| Async | 35.1 | 13.8 | 0.61 | 1.07 | 633 / 444 / 623 | 668 / 526 | `20261013_032031_g6s_ctrl_a1_577` |
| Sync | 37.2 | 19.1 | 10.78 | 1.71 | 631 / 434 / 610 | 671 / 534 | `20261013_032127_g6s_ctrl_a0_4891` |
| Async | 35.0 | 14.0 | 0.57 | 0.99 | 646 / 444 / 638 | 661 / 511 | `20261013_032216_g6s_ctrl_a1_20899` |
| Sync | 37.9 | 18.6 | 10.33 | 1.89 | 632 / 432 / 607 | 673 / 537 | `20261013_032313_g6s_ctrl_a0_19768` |

Peak memory is the same. While traversing, the async runs average 10 more resident chunks (+2.5%, about 10 MB) and
3-12 fewer compressed ones: a result is installed one or two ticks after its dispatch. They end 13-31 MB higher
because their catch-up ends about 0.8 s sooner, before the last chunks of the traverse have been idle for
`IdleFrames`. In catch-up the async runs pull ahead (1.0 s in: 515 and 501 compressed against 479 and 464); the sync
runs just get 0.7-0.9 s longer to compress before the report is written.

**Settled state** (fresh world at the spawn, `voxel.FarCompression.Stats` after the initial load drains):

| | Smooth control, async | Smooth control, sync | `Review_Cubic`, async | §12.7 (sync): control / cubic |
|---|---|---|---|---|
| Time to settle (polled every 4 s) | 13 s | 19 s | 18 s | 14 s / 17 s |
| 2-3 s after the drain | 230 MB (207 resident, 842 compressed) | 134 MB | 131 MB (105 resident, 937 compressed) | 453 / 718 MB |
| 10 s later / 45 s later | **134 / 134 MB** (112 / 79 / 932) | 134 / 134 MB (112 / 79 / 932) | **128 / 127 MB** (105 / 81 / 937) | 134 / 127 MB |

The settled resident, uniform and compressed counts match §12.7 exactly in both modes. Audits 0.

**Headless CPU sweep** (VoxelWorldsTest, `-VoxelForceCPU`, the §12.2 arguments; no editor was open, as in §12.7's
headless rows):

| Run | Frame P50 / P95 ms | Manager P95 ms | Sweep P95 ms | Catch-up s | Thrash (LOD / neighbour / other) | Peak / end voxel MB | End resident / uniform / compressed | Systems MB | JSON |
|-----|--------------------|----------------|--------------|------------|----------------------------------|---------------------|-------------------------------------|------------|------|
| *Control v3000 (§12.7)* | *3.6 / 10.8* | *8.1* | *untimed* | *8.5* | *535 (535 / 0 / 0)* | *236 / 192* | *168 / 103 / 918* | *572* | *`20261013_020144_d7_hl_ctrl_v3000`* |
| **Control v3000** | 3.3 / 8.5 | **5.8** | **0.15** | 8.5 | 535 (535 / 0 / 0) | 230 / 193 | 163 / 103 / 923 | 566 | `20261013_030125_g6_hl_ctrl_v3000` |
| Control v3000, sync encode | 3.8 / 10.7 | 8.2 | 4.27 | 8.9 | 535 (535 / 0 / 0) | 239 / 193 | 169 / 103 / 917 | 570 | `20261013_030602_g6_hl_ctrl_sync_v3000` |
| *`Review_Cubic` v1500 (§12.7)* | *1.3 / 8.8* | *6.9* | *untimed* | *0.6* | *151,665 (0 / 4,669 / 146,996)* | *924 / 437* | *413 / 83 / 693* | *1,379* | *`20261013_021932_d7h_hl_cubic_v1500`* |
| **`Review_Cubic` v1500** | 1.3 / 5.9 | **3.9** | **0.15** | 1.3 | 200,422 (0 / 4,696 / 195,726) | 884 / 420 | 389 / 84 / 716 | 1,332 | `20261013_030239_g6_hl_cubic_v1500` |
| *`Review_Cubic` v3000 (§12.7)* | *1.5 / 11.2* | *8.4* | *untimed* | *10.1* | *52,149 (0 / 3,126 / 49,023)* | *954 / 640* | *621 / 61 / 507* | *1,423* | *`20261013_022045_d7h_hl_cubic_v3000`* |
| **`Review_Cubic` v3000** | 1.4 / 6.9 | **4.5** | **0.16** | 10.0 | 70,723 (0 / 3,154 / 67,569) | 893 / 626 | 599 / 62 / 528 | 1,394 | `20261013_030347_g6_hl_cubic_v3000` |
| `Review_Cubic` v3000, sync encode | 1.6 / 11.8 | 9.2 | 5.50 | 10.6 | 50,107 (0 / 3,045 / 47,062) | 924 / 642 | 623 / 62 / 504 | 1,397 | `20261013_030712_g6_hl_cubic_sync_v3000` |
| *`Review_Cubic` v6000 (§12.7)* | *2.0 / 15.2* | *11.9* | *untimed* | *16.0* | *29,626 (0 / 2,708 / 26,918)* | *937 / 760* | *745 / 56 / 388* | *1,519* | *`20261013_022156_d7h_hl_cubic_v6000`* |
| **`Review_Cubic` v6000** | 1.7 / 7.8 | **5.0** | **0.23** | 15.9 | 51,077 (0 / 2,843 / 48,234) | 870 / 659 | 633 / 59 / 497 | 1,475 | `20261013_030457_g6_hl_cubic_v6000` |

The "other" thrash is KC14's per-frame deferral re-queue. It counts frames, so it rises when frames get shorter.

**What is left on the manager's P95.** Averaged over the slowest 5% of traverse ticks at v3000, the sweep is now 0.5 ms
(control) and 0.2 ms (cubic), against 13.2 and 10.4 ms with the sync encode. What remains:

- Control: collision 4.6 ms, seam pipeline 3.8, render submit 3.0, scatter 2.2, meshing 1.7.
- Cubic: render submit 9.7 ms, scatter 5.6, meshing 3.4 (of which the legacy neighbour slices are 2.6).

The smooth control also keeps about 2 ms of unattributed P95 with the encode on either thread. That is not the sweep:
it is one of the sections that are still untimed (water propagation, the debounced neighbour-remesh dispatch, the
deferral check), not identified here.

**Verdict.**

- **G6 is fixed for both modes.** The sweep's P95 falls from 10-14 ms to 0.6-0.9 ms in PIE and from 4-6 ms to
  0.15-0.23 ms headless. Same-build A/B at v3000: manager P95 21.9 → 13.5 ms (control) and 21.0 → 15.9/16.3 ms (cubic);
  frame P95 40.9 → 35.7/35.9 ms and 44.9 → 40.0/44.3 ms. Against §12.7, manager P95 is 36-41% lower for the control and
  20-33% lower for cubic at every speed. Catch-up is shorter or the same.
- **The memory win is kept.** Settled voxel data is unchanged: 134 MB smooth, 127 MB cubic, the same resident, uniform
  and compressed counts as §12.7. Peak memory is the same. While moving, the install lag of a tick or two holds
  about 10 MB (2.5%) more in the control. End-of-run numbers are within §12.7's spread for cubic. The control's end of
  run is +13 to +31 MB (+2-5%) in the settled-start A/B, because its shorter catch-up writes the report sooner. Headless, every
  memory and count column matches §12.7 or improves on it.
- **Cubic's lead over smooth did not come back.** The control gained more than cubic: its slowest ticks were dominated
  by the encode, while cubic's are dominated by render submit, scatter and the legacy neighbour slices.

  | Cubic vs a same-build control (2 runs at v3000) | v1500 | v3000 | v6000 |
  |-------------------------------------------------|-------|-------|-------|
  | Frame P95 (threshold +10%) | −12% ✓ | +18% ✗ | +9% ✓ |
  | Manager P95 (threshold +15%) | +27% ✗ | +19% ✗ | −18% ✓ |

  Against the first-pass control (§12.2), which the §12.7 verdict used, cubic is −24%, +4% and −9% on frame P95 and
  −30%, −21% and −48% on manager P95 (v3000: mean of the two runs). Closing what is left on cubic's P95 is KC2 work
  (the legacy neighbour cascade, slices and re-meshes) and KC4 work (scatter); its render submit cost comes from
  carrying 3-4× smooth's triangles.

## 13. Review B results: island (2026-10-13)

Run on VoxelWorlds `e1066ef` and UE5_PluginDev `18cbbfc` (main, with D7, KC11 and G6 merged), editor 5.8.3 from
`D:\Program Files\Epic Games\UE_5.8`, background throttle off, PIE in the level viewport (2874×1360, D3D12 SM6) with the
editor in the foreground. Island means `VoxelDemo` with `voxel.ConfigOverride /Game/PluginTesting/Review/Review_Island_<S|M|L>`
(§3) and a fresh world. All three tiers centre the island on the world origin, with `WorldOrigin` 0, water on at
−1,500, `EdgeHeight` −3,500 (so the floor beyond R+F is at −4,500) and Smooth falloff. Line numbers in §13 are as of
VoxelWorlds `e1066ef`.

**The control** is `DemoSmoothWorldConfig` through the same override, as in Review A (§12): the same config as plain
`VoxelDemo`, but with its own save key, so every run starts fresh and the user's demo save is never loaded or written
(its hashes and `savedAtUtc`, 2026-10-12T23:58:13Z, were unchanged at the end of the session). The control keeps the
demo's `WorldOrigin` (12345, 54321); bench start points are absolute world coordinates, so both sides fly over the same
terrain noise.

**Bench method.** One cold PIE per run: Play, `vox.DeleteSave`, clock pinned at noon, `voxel.Bench.Run <tag> <v> <dist>
warmup=120 <route>`, Stop. Every warmup drained. Routes, all with `follow=300`:

- **Chord** `start=-250000:0 dir=1:0`: on M, coast to coast through the centre (500,000 uu at v3000 and v6000; 250,000 uu,
  coast to centre, at v1500). On L the same coordinates lie entirely inside the island.
- **Perimeter** `orbit=0:0:200000`: 150,000 uu of arc along M's coastline at radius R. L's orbit uses its own R (450,000).
- **Coast crossing** `start=150000:0 dir=1:0`: 150,000 uu, from 50,000 uu inside M's coast to 50,000 uu past R+F. L's
  starts at 400,000.

`follow=` holds the pawn 300 uu above the seabed under water, so the pawn drowned in the first chord run (G4). For every
run reported here the player Blueprint's `DrowningDamagePerSecond` was set to 0 in memory, and restored afterwards. The
JSON reports are in [Research/Bench/ReviewB/](Research/Bench/ReviewB/), the session scripts are
`Saved/ClaudeScripts/rc/rb_*`, and screenshots are `Saved/Screenshots/Claudius/rb*` and `Saved/Screenshots/VoxelMap/rbI2_*`.

**Probe tests** (I1) are new automation tests that record what the code does and pass:
`VoxelWorlds.Review.Island.HeightProbe` (VoxelGeneration), `.CullCentreProbe` (VoxelLOD) and `.POIPlacementProbe`
(VoxelWorldPOI). They load the review configs, so they report the demo's seed, biomes and POI catalog. Their numbers are
`PROBE` lines in the test log; each file names the checks that become assertions with the fix.

### 13.1 Scenario results

Pass = meets the §5 "pass when"; Refine = works, but needs design or tuning; Bug = wrong behaviour.

| ID | Result | What happened |
|----|--------|---------------|
| I1 | **Bug** (KI1, KI3), KI4 partly | The three probes pass headless (`Saved/Logs/ReviewB_I1_probes.log`); numbers in §13.2. The island's own height query matches generation within one voxel in every zone. The InfinitePlane height that POI placement samples is right inside R and up to 7,138 uu wrong past it. Every placed POI past R is wrong: on M, 9 of the 12 POIs in the island's box sit in the falloff band or beyond R+F, and 8 of those 9 stand in the sea or over the void floor, on pads raised 1,900-5,900 uu above the island's surface. The cull probe reproduces KI3: with the demo's origin, 1,098 land columns are culled and 1,347 floor-only columns kept. The two existing island tests were not re-run separately; the full suites last passed in §12.8 |
| I2 | **Refine** | S reads as an island on the map and from the air (`rbI2_islandS_map`, `rbI2_S_lagoon_oblique`): lagoons, inner seas and coastline all come from the noise inside a circle. Problems: a round plateau stands in the sea at the north edge (a campsite pad, KI1), visible on the map; from outside the loaded ring the island is a slab floating in the void, with the water plane, the −4,500 floor and the cave-cut bottom layer showing (`rbI2_S_edge_outside`, KI2); under water a vertical 1,000 uu wall rings the island at R+F (`rbI2_S_edge_cliff_underwater`). Rectangle shape with Exponential falloff (`RB_S_RectExp`, temporary): a rectangular island with straight coasts and diagonal creases from the Chebyshev falloff (`rbI2_S_RectExp_map`, `rbI2_S_RectExp_oblique`). Map exploration runs past the coast (KI9). A survey from a camera 48,000 uu up shows chunk-sized blank rectangles in the lagoon; at 4,500 uu the same water renders whole, so they are a camera artefact *(M6, §14.10: from outside the loaded ring and from 48,000 uu up the island now sits in a sea that reaches the horizon, and the seabed slopes from the coast; the cliff and the void are gone)* |
| I3 | **Pass** | The whole of S is resident from the centre: 1,518 chunks, settled 247 MB voxel data, 23 MB renderer, audit 0, 9 s to drain. After visits to three coasts the centre holds 1,878 chunks and 266 MB: the outer ring stays loaded, bounded by the island cull (§13.4). Memory returns to the same level *(M4, §14.8: with the surface slab the whole of S is 1,020 chunks and 336 MB settled, the control's 352 MB at its spawn)* |
| I4 | **Refine** (inside the island) / **Bug** (along the coast) | Inside the island frame P95 is within +10% of the control and the island keeps up at v1500; manager P95 is +17-21% and the generation queue backs up at v3000, from the 35-55% more chunks per view (KI5). Along the coast (orbit at R) the island misses every threshold: frame +14-30%, manager +23-38%, catch-up 2.6-3.5× at v1500-v3000. The island's LOD re-meshing is **bimodal** between identical cold runs (KI12, new): 12,344-12,500 or 1,341-1,372 LOD re-meshes on the same route; in the low mode the island beats the control. Headless (CPU generation) the island lands in the high mode twice out of two: frame P95 +24%, manager P95 +28%. L's interior chord equals M's: per-frame cost does not depend on island size. §13.3. *(§13.10: the low mode skipped LOD work it owed; the high-mode rows are the island's numbers, and KI12 is fixed)* *(M4, §14.8: **Pass except catch-up at v1500.** The island meets the frame, manager, peak-loaded and memory thresholds on the chord, the perimeter and the coast at every speed; catch-up at v1500 is 0.27-0.46 s against the control's 0.17-0.18 s)* |
| I5 | **Bug** (KI2) | Water on: the pawn walks into the sea at 36,000 uu (S), swims past R+F at 171 uu/s to the end of the loaded water at 48,000 uu (the island cull edge), drops out of the water and falls at 4,000 uu/s. About 260 s later KillZ (−1,048,575) destroys it and nothing respawns it: the player is left with no pawn. Water off: the pawn walks down the falloff slope, drops 1,000 uu off the cliff at R+F onto the floor, crosses the floor ring and falls off its edge at 48,000 uu. `vox.Debug.Capture` never logs READY→NOT-READY, because the chunks past the edge were never loaded. The minimap keeps exploring the sea |
| I6 | **Bug** (KI1, KI9) | M, every band and off-island POI from the probe visited. The City in the falloff band stands on a cylinder 3,800 uu tall rising out of the sea (`rbI6_M_city_band`); a tiled dungeon's pad makes a round islet with the elevator on top (`rbI6_M_dungeon_band`); a campsite pad lifts 1,900 uu above the sea floor. The dungeon itself is dry and works (entrance room walkable, boss alive and lethal, respawn on the pad). The Tower 297,696 uu out streams its claim volume over the void, with no ground below it. Map markers: 20 within 400,000 uu, of which 3 inside R, 5 in the band and 12 past R+F. Map exploration: 1,497 tiles after the tour, 786 in the band and 124 past R+F (out to 268,000 uu). Working: scatter stops at the coast (all 13,689 instances inside R), surface NPCs never spawn off the island and despawn deer that wade into deep water (5 in the session, no log spam), `vox.NavPath` returns a partial path to the shore and NO PATH to a point past the coast, and a full day/night cycle ran (day 1 12:00 to day 2 12:55, every phase). VoxelDemo has no PCG components, so the PCG part of KI9 is code-only |
| I7 | **Refine** (KI8) | Spawn at the centre (no PlayerStart, origin): stands on land. Origin in the falloff band under water (`RB_S_SpawnCoast`): the spiral finds land one ring (4,800 uu) away and the pawn stands. Origin 30,000 uu past R+F with water on (`RB_S_SpawnOff`): land found at ring 8 (38,400 uu) and the pawn stands; past the 50,000 uu search it would fail. Origin off the island with water off (`RB_S_SpawnOffNoWater`): the spawn takes the floor height −4,500 as dry land at (0, 0), 70,000 uu from the centre, where nothing is generated; the character waits for collision that never comes and stays frozen (MOVE_NONE) |
| I8 | **Pass** (round trip), **Bug** (KI7) | M after a tour: two dungeons visited, 6 digs, 1,497 explored tiles, an item. Save, Stop, Play: 30 edited chunks (9 MB, mostly the two dungeon stamps), 2 dungeon records, 2 map overlays, the explored tiles, the item and the pawn position (149,997, −100,008) all came back. Then `IslandRadius` 200,000 → 120,000 in memory, Play: the same key loaded the same save silently (edits for 30 chunks, 2 dungeon records "pending until stamped", 1,499 explored tiles over what is now sea) and restored the player at a spot that is now 180,279 uu out, past the new R+F + ring (176,788). The pawn stayed frozen in MOVE_NONE, waiting for collision that cannot come |
| I9 | **Pass** | 25-minute bench orbit around M at r=150,000 with the clock running: frame P95 and memory flat in every 5-minute window, audits KI11 only, no stuck jobs. The re-mesh rate doubled at minute 10 with no frame cost (§13.5) |
| I10 | **Not run** | Optional. Loading `WP_VoxelCoexistTest` would have meant switching maps in an editor that held in-memory edits to review assets (a save prompt can block automation). KI10 is settled from the asset reference instead |

### 13.2 Probe results (I1)

**Heights** (`HeightProbe`, 16 rays from the island centre; generated = the surface CPU generation produces, caves off):

| Config, zone | Samples | Island analytic vs generated, max | POI placement height vs generated, max / mean | POI height says land where the island is under water |
|--------------|---------|-----------------------------------|-----------------------------------------------|------------------------------------------------------|
| S, inside R | 64 | 28 uu | 28 / 5 uu | 0 |
| S, falloff band | 80 | 27 uu | 6,073 / 1,672 uu | 25 |
| S, beyond R+F (loaded ring) | 32 | 0 uu | 6,975 / 4,203 uu | 26 |
| M, inside R | 64 | 20 uu | 20 / 5 uu | 0 |
| M, falloff band | 80 | 8 uu | 6,708 / 1,549 uu | 33 |
| M, beyond R+F | 32 | 0 uu | 7,138 / 4,020 uu | 22 |

The island's own height query (`FIslandBowlWorldMode::GetTerrainHeightAt`, which the chunk manager's surface queries,
spawn, the map, PCG and navigation use) lands within one voxel of generation everywhere. The InfinitePlane height that POI
placement samples is right inside R and off by up to 7,138 uu from R outwards.

At R+F the generated surface drops 1,000 uu across 80 uu with Smooth falloff (from `EdgeHeight` to the floor at
`EdgeHeight − 1000`), and 1,016-1,307 uu with Exponential falloff, which stops at 5% of the relief instead of 0.
`GetTerrainHeightBounds` returns [−12,000, 10,000] for these configs, which contains the −4,500 floor. The world mode's
`GetDensityAt` returns air (−1,000) beyond R+F, where generation writes that floor (`IslandBowlWorldMode.cpp:27-31` vs
`VoxelCPUNoiseGenerator.cpp:917-920`); nothing found here calls it for island generation.

**Cull centre** (`CullCentreProbe`, island M geometry, every chunk column within 300,000 uu): with `WorldOrigin` 0 the
cull keeps all 8,732 columns that hold island terrain, plus 236 floor-only ones. With the demo's origin and the offset
`Review_Island_M_DemoOrigin` uses, generation centres the island at (−12,345, −54,321) and the cull at (0, 0), 55,706 uu
apart: 1,098 land columns are culled (up to 48,767 uu inside the coastline's outer edge) and 1,347 floor-only columns are
kept. In PIE the island's south coast ends in a straight cut through a hill (`rbKI3_south_cut_high`).

**POI placement** (`POIPlacementProbe`, the project catalog's 5 types, placement over the island plus one 64,000 uu cell
all round; each POI re-judged on the island's height):

| Config | Placed | Inside R | Falloff band | Beyond R+F |
|--------|--------|----------|--------------|------------|
| S | 2 | 1, on land | 1 campsite, footprint under water, pad 5,976 uu above the island's surface | 0 |
| M | 12 | 3, on land | 5: 4 under water, pads 1,900-3,800 uu above the island's surface; 2 dungeons, 1 short of the cover rule | 4, all over the floor, pads 3,400-5,900 uu above it |
| L | 35 | 21, on land (3 dungeons, all covered) | 4: 3 under water, pads up to 4,520 uu above the surface; 1 dungeon short of the cover rule | 2, over the floor |

Every POI inside R is correct. Every RequireLand POI past R stands on the sea or the floor, and its pad raises a plateau
out of the water, because terrain conditioning runs after the falloff (`VoxelCPUNoiseGenerator.cpp:930-934`). At runtime
the subsystem's own survey logs "POI placement within 5 km: 32 placed" for island S, an island 0.8 km across.

### 13.3 Bench comparison (I4)

**Focused PIE**, island M vs the control on the same routes. "Generated / meshed" are the traverse totals from the CSV;
GenQ mean is over the traverse. Systems MB is peak voxel + renderer + collision + scatter memory.

| Route | Run | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | GenQ mean / peak | Peak mesh queue | Loaded mean / peak | Generated / meshed | LOD re-meshes | Peak voxel MB | Systems MB | Audit | JSON |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Chord v1500 | Control | 16.8 / 28.3 / 34.9 | 9.5 | 1.85 | 16 / 122 | 16 | 1,209 / 1,282 | 5,112 / 8,613 | 3,528 | 826 | 1,078 | 0 | `20261013_042454_rbI4_C_chord_v1500` |
| Chord v1500 | **Island M** | 16.7 / 30.0 / 39.7 | 11.5 | 1.88 | 32 / 272 | 104 | 1,663 / 1,984 | 6,286 / 12,336 | 6,102 | 783 | 1,023 | 44 | `20261013_041030_rbI4_M_chord_v1500` |
| Chord v3000 | Control | 23.4 / 37.3 / 44.3 | 12.4 | 2.84 | 236 / 404 | 10 | 979 / 1,177 | 8,907 / 16,072 | 7,168 | 792 | 1,147 | 0 | `20261013_040654_rbI4_C_chord_v3000` |
| Chord v3000 | Control, re-run | 23.8 / 37.5 / 45.7 | 12.5 | 2.84 | 254 / 412 | 12 | 961 / 1,177 | 8,797 / 15,962 | 7,168 | 849 | 1,151 | 0 | `20261013_045205_rbI4_C_chord_v3000b` |
| Chord v3000 | **Island M** (high mode) | 23.6 / 40.8 / 55.9 | 14.5 | 0.25 | 647 / 1,015 | 270 | 1,102 / 1,534 | 8,773 / 21,273 | 12,500 | 942 | 1,271 | 26 | `20261013_040328_rbI4_M_chord_v3000` |
| Chord v3000 | Island M, run c (high mode) | 23.8 / 41.0 / 55.8 | 14.2 | 0.36 | 649 / 1,028 | 95 | 1,107 / 1,554 | 8,508 / 21,086 | 12,344 | 862 | 1,289 | 26 | `20261013_045916_rbI4_M_chord_v3000c` |
| Chord v3000 | Island M, run b (low mode) | 19.0 / 32.0 / 39.9 | 10.0 | 0.17 | 389 / 669 | 125 | 1,372 / 1,642 | 10,509 / 11,836 | 1,341 | 803 | 1,030 | 64 | `20261013_044846_rbI4_M_chord_v3000b` |
| Chord v3000 | Island M, run d (low mode) | 19.1 / 32.0 / 39.8 | 9.7 | 0.17 | 400 / 706 | 126 | 1,361 / 1,626 | 10,453 / 11,817 | 1,372 | 797 | 1,085 | 64 | `20261013_050556_rbI4_M_chord_v3000d` |
| Chord v6000 | Control | 29.3 / 43.9 / 51.8 | 16.8 | 8.99 | 929 / 1,055 | 25 | 399 / 1,177 | 3,617 / 9,451 | 5,848 | 796 | 1,265 | 0 | `20261013_042653_rbI4_C_chord_v6000` |
| Chord v6000 | **Island M** | 25.1 / 41.3 / 52.1 | 13.3 | 6.13 | 1,234 / 1,564 | 16 | 530 / 1,158 | 4,122 / 5,128 | 1,314 | 1,130 | 1,552 | 0 | `20261013_041225_rbI4_M_chord_v6000` |
| Orbit v1500 | Control | 16.7 / 27.3 / 34.0 | 9.5 | 0.17 | 15 / 115 | 16 | 1,196 / 1,262 | 2,990 / 5,121 | 2,131 | 811 | 1,051 | 0 | `20261013_042859_rbI4_C_orbit_v1500` |
| Orbit v1500 | **Island M** | 18.0 / 33.8 / 44.4 | 13.2 | 0.44 | 36 / 269 | 193 | 1,777 / 1,959 | 3,941 / 7,863 | 3,959 | 962 | 1,202 | 45 | `20261013_041435_rbI4_M_orbit_v1500` |
| Orbit v3000 | Control | 24.1 / 37.8 / 43.8 | 12.5 | 1.83 | 191 / 326 | 8 | 1,020 / 1,188 | 2,600 / 4,727 | 2,131 | 827 | 1,066 | 0 | `20261013_043019_rbI4_C_orbit_v3000` |
| Orbit v3000 | **Island M** | 26.6 / 43.0 / 53.2 | 15.3 | 6.37 | 592 / 973 | 24 | 1,217 / 1,753 | 2,333 / 6,299 | 3,973 | 976 | 1,334 | 20 | `20261013_041604_rbI4_M_orbit_v3000` |
| Orbit v6000 | Control | 28.7 / 41.2 / 49.0 | 16.2 | 7.93 | 732 / 1,003 | 11 | 545 / 1,192 | 1,100 / 3,026 | 1,930 | 822 | 1,117 | 0 | `20261013_043122_rbI4_C_orbit_v6000` |
| Orbit v6000 | **Island M** | 33.5 / 53.5 / 61.2 | 21.1 | 12.41 | 1,120 / 1,606 | 473 | 694 / 1,736 | 928 / 2,856 | 2,334 | 1,010 | 1,309 | 3 | `20261013_041711_rbI4_M_orbit_v6000` |
| Coast v1500 | Control | 17.4 / 30.8 / 40.2 | 10.1 | 0.19 | 19 / 171 | 36 | 1,159 / 1,241 | 2,969 / 5,097 | 2,130 | 806 | 1,058 | 0 | `20261013_043330_rbI4_C_coast_v1500` |
| Coast v1500 | **Island M** | 16.7 / 28.8 / 40.0 | 10.5 | 0.17 | 20 / 264 | 144 | 1,275 / 1,922 | 2,060 / 4,736 | 2,682 | 952 | 1,212 | 0 | `20261013_041920_rbI4_M_coast_v1500` |
| Coast v3000 | Control | 26.0 / 41.7 / 52.1 | 14.4 | 3.26 | 236 / 469 | 7 | 944 / 1,171 | 2,354 / 4,482 | 2,130 | 815 | 1,120 | 0 | `20261013_043452_rbI4_C_coast_v3000` |
| Coast v3000 | **Island M** | 16.7 / 32.4 / 42.1 | 10.5 | 0.17 | 186 / 578 | 130 | 1,084 / 1,608 | 1,820 / 2,620 | 808 | 962 | 1,222 | 0 | `20261013_042042_rbI4_M_coast_v3000` |
| Coast v6000 | Control | 33.1 / 53.5 / 62.1 | 20.5 | 9.59 | 767 / 1,061 | 77 | 490 / 1,172 | 952 / 2,684 | 1,768 | 825 | 1,172 | 0 | `20261013_043555_rbI4_C_coast_v6000` |
| Coast v6000 | **Island M** | 19.4 / 51.3 / 66.5 | 20.4 | 0.17 | 540 / 1,390 | 402 | 580 / 1,513 | 984 / 2,393 | 1,689 | 984 | 1,244 | 0 | `20261013_042137_rbI4_M_coast_v6000` |

`thrash` other / neighbour counts were 0-3 in every run. The island's audit findings are all KI11 (§13.6); the control's
are 0. The M rows marked **Island M** ran in one stretch of the session in which the v3000 chord landed in the high mode
of KI12; runs b and d, and the L rows below, ran in stretches where it landed in the low mode.

**Against the §1 thresholds (island M vs control):**

| Metric (threshold) | Chord v1500 | Chord v3000 | Chord v6000 | Orbit v1500 | Orbit v3000 | Orbit v6000 | Coast v1500 | Coast v3000 | Coast v6000 |
|---|---|---|---|---|---|---|---|---|---|
| Frame P95 (+10%) | +6% ✓ | +9% ✓ (low mode −14%) | −6% ✓ | +24% ✗ | +14% ✗ | +30% ✗ | −6% ✓ | −22% ✓ | −4% ✓ |
| Manager P95 (+15%) | +21% ✗ | +17% ✗ (low mode −20%) | −21% ✓ | +38% ✗ | +23% ✗ | +30% ✗ | +4% ✓ | −27% ✓ | −1% ✓ |
| Catch-up (+25%) | +2% ✓ | ends off the island | ends off the island | 0.44 vs 0.17 s ✗ | 3.5× ✗ | +57% ✗ | ends off the island | ends off the island | ends off the island |
| Peak loaded chunks (+25%, or explained) | +55% (KI5) | +30% (KI5) | −2% | +55% (KI5) | +48% (KI5) | +46% (KI5) | +55% (KI5) | +37% (KI5) | +29% (KI5) |
| Peak voxel MB (+25%) | −5% ✓ | +19% ✓ | +42% ✗ | +19% ✓ | +18% ✓ | +23% ✓ | +18% ✓ | +18% ✓ | +19% ✓ |
| Systems MB (+25%) | −5% ✓ | +11% ✓ | +23% ✓ | +14% ✓ | +25% ✓ | +17% ✓ | +15% ✓ | +9% ✓ | +6% ✓ |
| Audit after settling | KI11 only | KI11 only | 0 | KI11 only | KI11 only | KI11 only | 0 | 0 | 0 |

"Ends off the island": the route stops past R+F, where nothing is left to load, so the island's catch-up (0.17-0.36 s)
says nothing about streaming; the control has to finish loading terrain there.

What the numbers say:

- **The island loads 30-55% more chunks for the same view** (KI5): no far-band slab, so every column keeps the full height
  range (7-8 layers; the control keeps 3-4 past band 0). It also measures LOD distance in 2D, so whole columns sit in
  the near bands: at the same spawn the island holds 224 uncompressed chunks against the control's 113 (§13.4).
- **Generation throughput is the same per frame, so the queue backs up.** On the v3000 chord the island generated 8,773
  chunks and the control 8,907, but the island needs more per metre: its generation queue averaged 647 against 236, and
  the terrain ahead lags. At v1500 both keep up.
- **Interior cost does not depend on island size.** L's chord (entirely inside L) and M's chord in the same LOD mode give
  frame P95 32.2 and 32.0 ms, manager P95 9.7 and 9.7 ms, 1,383 and 1,361 chunks loaded on average.
- **The coast is the worst place.** On the perimeter at R the view holds sea, falloff band and void floor, all at full column
  depth; the island misses every threshold there.
- **LOD re-meshing is bimodal (KI12).** Four island chord runs at v3000 from identical cold starts gave 12,500, 12,344,
  1,341 and 1,372 LOD re-meshes; the two control runs gave 7,168 each. See §13.6. *(Explained and fixed in §13.10: a
  latch decided at the start of the route whether LOD was evaluated every tick or only at chunk changes. The low-mode
  runs left about 285 chunks at the wrong LOD. Use the high-mode rows.)*

**L** (scale check; its chord lies inside the island, so its catch-up is a real interior one):

| Run | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | GenQ peak | Peak loaded | LOD re-meshes | Peak voxel MB | Systems MB | Audit | JSON |
|---|---|---|---|---|---|---|---|---|---|---|
| Chord v3000 (interior) | 19.3 / 32.2 / 40.3 | 9.7 | 6.48 | 692 | 1,908 | 1,354 | 971 | 1,405 | 36 | `20261013_043918_rbI4_L_chord_v3000` |
| Orbit at R=450,000 v3000 | 20.2 / 35.8 / 46.0 | 11.1 | 7.48 | 769 | 1,870 | 768 | 982 | 1,281 | 17 | `20261013_044046_rbI4_L_orbit_v3000` |
| Coast v1500 | 16.7 / 26.0 / 35.3 | 9.9 | 0.17 | 488 | 1,842 | 2,574 | 955 | 1,218 | 8 | `20261013_044253_rbI4_L_coast_v1500` |
| Coast v3000 | 16.7 / 30.1 / 39.1 | 10.2 | 0.17 | 550 | 1,456 | 988 | 920 | 1,186 | 8 | `20261013_044413_rbI4_L_coast_v3000` |
| Coast v6000 | 22.1 / 49.6 / 59.9 | 20.0 | 0.17 | 1,262 | 1,336 | 1,774 | 1,008 | 1,273 | 8 | `20261013_044509_rbI4_L_coast_v6000` |

L's interior chord beats the control's v3000 chord on frame and manager time (−14%, −22%) but takes 2.3× as long to catch
up (6.48 vs 2.84 s), with 62% more chunks at peak.

**Headless CPU** (`voxel_bench_sweep.ps1`, VoxelWorldsTest map, `-VoxelForceCPU`, `-Config` as below, editor open and idle):

| Run | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | GenQ mean / peak | Loaded mean / peak | Generated / meshed | LOD re-meshes | Peak voxel MB | Systems MB | Audit | JSON |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Control, chord v3000 | 3.6 / 7.2 / 12.0 | 4.6 | 24.2 | 760 / 894 | 510 / 1,177 | 5,177 / 6,159 | 1,142 | 223 | 807 | 0 | `20261013_061251_rbHL_C_chord_v3000` |
| Island M, chord v3000 | 3.0 / 8.9 / 15.7 | 6.0 | 18.0 (ends off the island) | 1,202 / 1,435 | 580 / 1,164 | 4,812 / 14,079 | 9,269 | 379 | 768 | 16 | `20261013_060851_rbHL_M_chord_v3000` |
| Island M, chord v3000, repeat | 2.9 / 8.8 / 15.5 | 5.8 | 18.3 (ends off the island) | 1,191 / 1,435 | 587 / 1,164 | 4,849 / 14,149 | 9,304 | 386 | 765 | 16 | `20261013_061637_rbHL_M2_chord_v3000` |

CPU generation keeps up with v3000 on neither side (about half the view is loaded on average), so these runs measure a
generation-starved frontier. The two island runs agree within 1%: headless the island lands in KI12's high mode every
time, with 8× the control's LOD re-meshes, 2.3× its meshes, frame P95 +24% and manager P95 +28%, the direction PIE's high
mode shows. *(§13.10: the control's headless run was in the low mode, so this compares the two regimes; §13.10 has both
sides in the same regime.)* Peak voxel data is +70%; systems memory is −5% because the island carries less scatter.

### 13.4 Full residency on S (I3)

Fresh `Review_Island_S`, pawn at the spawn (the island centre); `voxel.Bench.Run rbI3_islandS_settle 0 0 warmup=300`
started about 5 s after Play, then `voxel.FarCompression.Stats` and `AuditResidency`. Each later row is a zero-length
bench (drain, then record memory and the audit) after a `vox.TP`. The control row is the same measurement on the control
at its spawn, in the same build.

| Step | Loaded chunks | Rendered (triangles) | Voxel MB settled (resident / uniform / compressed chunks) | Renderer GPU MB | Collision / scatter MB, peak | Audit | JSON |
|------|---------------|----------------------|-----------------------------------------------------------|-----------------|------------------------------|-------|------|
| Control at its spawn | 1,123 | 424 (0.86 M) | 134.7 (113 / 79 / 931) | 23.1 | 101 / 90 | 0 | `20261013_050715_rbI3_C_settle` |
| **S, initial load** (drained 9.1 s after the bench started; the control 6.3 s) | **1,518** | 440 (0.87 M) | **246.7** (224 / 116 / 1,178) | **23.3** | 100 / 93 | 0 | `20261013_034841_rbI3_islandS_settle` |
| S, east coast (36,000, 0) | 1,082 | 319 | 225 | 21 | 91 / 123 | 10 (KI11) | `20261013_034959_rbI3_S_coastE` |
| S, back at the centre | 1,702 | 498 | 250.0 (224 / 147 / 1,331) | 26.6 | 98 / 123 | 4 (KI11) | `20261013_035038_rbI3_S_back` |
| S, after the west and south coasts, at the centre | 1,878 | 511 (1.02 M) | 265.7 (238 / 179 / 1,461) | 27.2 | 96 / 148 | 12 (KI11) | `20261013_035225_rbI3_S_back2` |

- **The whole island is resident from the centre**, because R+F (40,000 uu) equals the view distance: 216 island columns,
  1,518 chunks, about 7 layers each.
- **Settled cost of a fully resident S:** about 250 MB voxel data, 23-27 MB renderer GPU, 100 MB collision and
  90-150 MB scatter, about 0.5 GB in all. Against the control at the same spot: +35% chunks, +83% voxel data, the same
  renderer and collision. The voxel excess is the uncompressed near band: 224 resident chunks against 113, because the
  island's 2D LOD distance puts whole columns inside 14,000 uu, where InfinitePlane's 3D distance compresses the deep and
  high ones (KI13).
- **Memory returns to baseline, plus the outer ring.** After the coasts the centre holds 1,702, then 1,878 chunks
  and 250-266 MB. The extra chunks are the ring between the view distance (40,000) and the island cull radius (R+F + one
  chunk diagonal = 46,788 uu): they load while the pawn stands at a coast and stay because they sit inside the 48,000 uu
  unload radius from the centre. It is bounded: the cull keeps at most 300 columns × 8 layers (the height range −16,800 to
  14,800) = 2,400 chunks. Nothing leaked; every audit finding is KI11.
- **Exploration ran past the coast:** the save written at Stop held 393 explored map tiles for an island of 216 chunk
  columns (KI9), and no voxel edits.

**Memory budget (decision D3).** Proposed, from these numbers: a fully resident island S costs about 0.5 GB of voxel
systems memory settled (voxel 250, renderer 27, collision 100, scatter 150 MB), and the island's peak while moving is
1.0-1.55 GB against the control's 1.05-1.27 GB. Budget: **0.6 GB settled for a fully resident S-class island, and at most
1.5 GB peak while streaming at any speed and tier** (the control's highest peak, 1.27 GB at v6000, + 20%). The island met
it in every run but M's v6000 chord (1.55 GB).

### 13.5 Long-session soak (I9)

Fresh M, `voxel.Bench.Run rbI9_M_soak 1500 2250000 warmup=120 orbit=0:0:150000 follow=300`: 25 minutes and 2.4 laps of
a circle 50,000 uu inside the coast that crosses land and inland seas, with the clock running at one game day per 24 minutes (dusk at
minute 6, night from minute 7). Status, `FarCompression.Stats` and `AuditResidency` every 5 minutes; the windows below come
from the bench CSV (JSON `20261013_055916_rbI9_M_soak`).

| Minutes | Frame P50 / P95 ms | Manager P95 ms | Voxel MB mean / max | Renderer GPU MB | Collision / scatter MB | Editor process MB | Loaded mean | GenQ mean | Re-mesh requests |
|---------|--------------------|----------------|---------------------|-----------------|------------------------|-------------------|-------------|-----------|------------------|
| 0-5 | 18.3 / 33.6 | 12.2 | 601 / 897 | 26.6 | 91 / 192 | 6,221 | 1,770 | 52 | 12,447 |
| 5-10 | 18.0 / 32.9 | 12.5 | 589 / 823 | 27.0 | 91 / 347 | 6,824 | 1,761 | 46 | 12,678 |
| 10-15 | 17.0 / 32.2 | 12.0 | 594 / 792 | 26.8 | 91 / 314 | 7,032 | 1,808 | 39 | 24,687 |
| 15-20 | 17.2 / 31.9 | 12.1 | 580 / 813 | 26.8 | 91 / 154 | 6,963 | 1,769 | 36 | 24,256 |
| 20-25 | 18.1 / 33.9 | 12.5 | 602 / 815 | 26.9 | 90 / 151 | 7,077 | 1,793 | 47 | 24,834 |

Whole run: frame 17.7 / 33.0 / 43.7 ms, manager P95 12.3 ms, catch-up 0.9 s, peak 1,991 chunks, peak systems 1,368 MB.
The audits at the 5-minute marks found 53, 15, 63 and 54, every one KI11 (Loaded, top or bottom layer); no stuck jobs,
render orphans or seam orphans.

- **Pass:** frame times and memory stay flat for 25 minutes. The editor process grows by 0.8 GB in the first 10 minutes
  and then holds at 7.0-7.1 GB.
- **The re-mesh rate doubled at minute 10** (12,500 to 24,500 requests per window) and stayed there, with no frame cost.
  The run's totals are 62,373 LOD re-meshes and 36,600 "other" re-queues; the chord runs had 0-3 "other". Not isolated;
  it may be KI12's switch happening inside one session. *(§13.10: not KI12. Minute 10 is the start of the second lap, and
  the bench counted every reloaded chunk's first mesh as a re-mesh (G10, fixed). Executed meshes stayed flat.)*
- **After the bench** the pawn went back to its start, 150,000 uu from any loaded terrain, and fell through the world to
  KillZ before collision arrived (G9; not island-specific).
- The bench flies the pawn, so this run cannot see walking fall-throughs; I5's capture walked the island to its edge with
  no READY→NOT-READY.

### 13.6 KI1-KI11: confirmed or dismissed

| ID | Verdict | Evidence |
|----|---------|----------|
| KI1 | **Confirmed** | §13.2: POI placement height is off by up to 7,138 uu past R, and on M 8 of the 9 POIs past R stand in the sea or over the floor, on pads raised 1,900-5,900 uu. In PIE: the City on a 3,800 uu cylinder out of the sea, a dungeon islet, a campsite plateau on the S map; the off-island Tower streams its claim over the void; 17 of 20 map markers lie past R (12 past R+F). Off-island dungeons are also stamped: a fresh S world, after a walk to its coast, saved 4.9 MB of voxel edits for a dungeon in POI cell (−1, 3), about 200,000 uu out over the void, in chunks that never load (the 4 MB seen in the tooling verification, §10). The band dungeon stays dry and playable, so "flooded" was not seen, but 1 of the 2 band dungeons on M and the one on L fail the cover rule against the island's real ground |
| KI2 | **Confirmed** | No edge: a 1,000 uu wall at R+F, a floor ring out to R+F + 6,788 uu (the cull's chunk diagonal), then nothing. Water ends with that ring. The pawn swims off it and falls about 4 minutes to KillZ, which destroys it without a respawn; with water off it walks off the floor's edge. From outside, the island floats in the void (I2, I5) *(Fixed: the boundary and recovery in M3, §14.7; the visible edge in M6, §14.10: a seabed sloping from the coast and a far ocean to the horizon)* |
| KI3 | **Confirmed** | §13.2: 1,098 land columns culled and 1,347 floor-only columns kept with the demo's origin; in PIE the south coast is a straight cut through a hill. With `WorldOrigin` 0 the two agree exactly |
| KI4 | **Partly confirmed** | `bIslandBowlShape` is copied (config, request, island params, chunk manager ×2, map, field viewer, CPU generator) and read nowhere, shaders included. Exponential falloff leaves a 1,016-1,307 uu step instead of 1,000 (a 5% residual on top of the floor cliff every falloff type has). Island params are built in four places (`VoxelChunkManager.cpp:994`, `:2561`; `VoxelMapSubsystem.cpp:176`; `VoxelFieldTypes.cpp:63`). **Dismissed for these configs:** `GetTerrainHeightBounds` already contains the floor ([−12,000, 10,000] vs −4,500); it would miss it only when `EdgeHeight − 1000` is below the continentalness minimum. The world mode's `GetDensityAt` calls the floor air, which generation does not use (latent) |
| KI5 | **Confirmed** | +35% chunks at the spawn on S (1,518 vs 1,123), +30-55% peak loaded on the M routes, generation queue 2.7× the control's at v3000, +32-54% meshes, and the main reason for the manager and coast misses in §13.3 *(Fixed in M4, §14.8: islands keep the surface slab at every distance; M's peak loaded chunks are within −15% to +8% of the control on every route)* |
| KI6 | **Confirmed** (code) | `MaxLoadedChunks` is read nowhere (only `VoxelWorldTestActor.cpp:490` sets it). The island cull bounds S at 2,400 chunks; nothing else bounds anything *(M4, §14.8: `MaxLoadedChunks` removed; `LOD_SYSTEM.md` says what bounds residency)* |
| KI7 | **Confirmed** | I8: with the radius changed, the same save loaded silently: 30 edited chunks, 2 dungeon records, 1,499 explored tiles, and the player restored past the new edge, frozen |
| KI8 | **Partly confirmed** | The 8-direction search finds land from the falloff band and from 30,000 uu past R+F (water on). Water off, off the island: the floor height counts as dry land and the pawn freezes over the void, waiting for collision. `WaterLevel` without `WorldOrigin.Z` and `GetPlanetSpawnPosition` were not exercised (Z origin 0, no planet) |
| KI9 | **Confirmed** for the map and markers, **dismissed** for scatter and NPCs, **code-only** for PCG and water-off navigation | Map exploration: 393 tiles on S for 216 columns; 786 band and 124 beyond-R+F tiles of 1,497 on M, all saved. Markers: §13.1 I6. Scatter: none past the coast. Surface NPCs: spawn only on loaded, dry ground and despawn in deep water. Navigation with water on refuses the sea. PCG: no PCG components in VoxelDemo; the sampler has no island mask (`PCGVoxelSurfaceSampler.cpp:244-291`). Navigation with water off treats the floor as walkable by code; not run |
| KI10 | **Confirmed** | `WP_VoxelCoexistTest`'s voxel world references `DemoWorldConfig` (InfinitePlane); `WP_IslandBowlConfig` is referenced by nothing. Island tests: the two existing ones plus the three probes added here. This review is the first island bench |
| KI11 | **Confirmed**: false findings, not chunks kept too long | Every island audit finding in the session (4-64 per run) is a Loaded chunk in the top or bottom layer (z = 3 or −4), 48,000-52,000 uu away in 3D and 46,082-47,942 uu in 2D, inside the radius by the island's own 2D rule. The control's audits were 0 *(Fixed in M4, §14.8: the audit measures range with the strategy's horizon distance; island audits 0 in every M4 run)* |

### 13.7 New findings

| ID | Finding | Evidence | Impact |
|----|---------|----------|--------|
| KI12 | **Island LOD re-meshing is bimodal between identical cold runs.** M's v3000 chord gave 12,500, 12,344, 1,341 and 1,372 LOD re-meshes in four runs (same command, fresh world, same path to within 31 uu of height). In the high mode chunks mesh at LOD 1 five times as often (5,020 vs 987 meshes) and frame / manager P95 are 41 / 14.2-14.5 ms; in the low mode 32 / 9.7-10.0 ms. Headless (CPU generation) both island runs landed in the high mode (9,269 and 9,304 against the control's 1,142, §13.3), so the high mode may be the island's normal state and the PIE low mode the variant. Judging by the LOD counts, the PIE mode held for whole stretches of the session (the M batch high, the L batch low), and it may also switch inside a run (the soak's re-mesh rate doubled at minute 10, §13.5). It is not explained by a save load, the override, the island parameters or Python state, all checked. The control gave 7,168 twice | Bench JSON `rbI4_M_chord_v3000{,b,c,d}`; meshing log LOD counts | The island's streaming numbers have two values; its threshold verdicts depend on which mode a run lands in. Likely an ordering- or timing-dependent path in LOD evaluation, which only runs on a viewer chunk change or a drained-queue sweep (`VoxelChunkManager.cpp:768-791`) and drops candidates past the per-frame cap. Needs an instrumented run. **Explained and fixed (§13.10):** the sweep flag froze in whatever state it had when the queues last emptied, near the start of the route; set, LOD was evaluated every tick, clear, only at chunk changes, 8 at a time. The low mode was the broken one. It affected every world mode; the control's headless run was in it |
| KI13 | **Island LOD distance is 2D everywhere, so whole columns sit in the near bands.** InfinitePlane measures 3D distance, so deep and high chunks fall into the far band and compress; the island keeps them uncompressed | At the same spawn: 224 resident (uncompressed) chunks vs the control's 113; settled voxel data 247 vs 135 MB (`DistanceBandLODStrategy.cpp:644-648`) | Memory, alongside KI5; the reason KI11's audit disagrees. *(Fixed in M4, §14.8: LOD distance is 3D in every world mode)* |
| KI14 | **No recovery when the pawn leaves the generated world.** KillZ destroys the pawn and nothing respawns it: nothing in the project overrides `FellOutOfWorld`, so the engine default destroys the actor and the death and respawn path never runs. The character's chunk-based wait has no timeout, so a spawn or a restored save position where no terrain will ever generate freezes the pawn for good | I5 (water on: no pawn after KillZ), I7 (water off, off the island), I8 (radius change) | Soft locks; any world edge design needs this |
| KI15 | **Map tile images are never evicted.** Each explored tile keeps a 64×64 `FColor` image (16 KB) for the session | `VoxelMapSubsystem.cpp:1648` (`TileCache.Add`, no removal outside `Deinitialize`); `VoxelMapTypes.h:27` | Mode-independent, but in a finite world it is the first cost that grows with the world: a fully explored L is about 0.55 GB of map tiles (§13.8). *(Fixed in M6, WM-E2, §14.10: 80 MB budget; a fully explored L holds 37 MB)* |

Tooling found on the way:

| ID | Finding |
|----|---------|
| G7 | Streaming follows the view target, not the pawn: a `pie_camera` survey camera left in place starves the pawn's own chunks (a `vox.TP` settle then times out after 45 s). Release the camera (`pie_camera release:true`) before moving the pawn |
| G8 | `follow=` routes over the sea drown the pawn (G4); a drowned pawn respawns at the spawn and invalidates the run. Workaround used here: zero `DrowningDamagePerSecond` on the player Blueprint's CDO in memory (`rb_nodrown_cdo.py`). The bench could make its fly pawn immune instead |
| G9 | When a bench ends it puts the pawn back at its start position without a settle hold. Far from loaded terrain (here 150,000 uu) the pawn falls through the world before collision arrives |
| G10 | *(found with §13.10)* The bench's re-mesh ("thrash") counter remembered every chunk it had meshed and never forgot one on unload, so on a route that revisits terrain each reloaded chunk's first mesh counted as a re-mesh ("other"). It made §13.5's soak look as if its re-mesh rate doubled at the start of lap 2. **Fixed (§13.10):** the chunk is forgotten when it unloads |

### 13.8 How large an island the streaming-only model holds

- **Per frame, island size does not matter.** L (10 km across) and M (5 km) give the same interior numbers (§13.3), because
  the cost is set by the view distance. The island-specific costs (KI5, KI13, the coast) are per view, not per island.
- **What grows with the island:**
  - Map tile images, 16 KB per explored tile (KI15): fully explored M ≈ 8,500 island tiles ≈ 140 MB; L ≈ 34,000 tiles ≈
    0.55 GB; a 20 km island ≈ 2.2 GB. Exploration past the coast (KI9) adds the sea ring on top. *(M6, §14.10: the map
    keeps its images under an 80 MB budget whatever the size; L measured 539 → 37 MB)*
  - The save: explored tiles are compressed coordinates (1,497 tiles in a few kB), edits and dungeon records grow with
    play, not with size.
  - Float precision: clean to about 100 km from the origin (`Research/TRAVERSAL_LIMITS.md`), so up to about 200 km
    across for a centred island.
- **Estimate.** With the current streaming-only model, an island up to **about 10-20 km across** works without new
  memory work (L measured at 10 km; at 20 km the map tile cache reaches about 2 GB when fully explored). Past that the map
  cache needs eviction or a coarser far map; streaming itself would hold to the ~200 km precision limit. *(M6, §14.10:
  the map cache is now bounded, so the float-precision limit (~200 km across) is what remains; past about 12 km a fully
  explored island's overviews no longer all fit and the world map draws far tiles again on demand)*
- **Fully resident (pre-generated) islands** cost about 0.9 MB of voxel data per chunk column settled (S: 266 MB for 300
  columns) plus collision and scatter. 1 GB of voxel data holds about 1,100 columns, an island about 1.8 km across;
  M would need about 8 GB. Pre-generation (§6, nice to have) is realistic for S-class islands only, unless far columns
  are kept at a coarser stride or on disk.

### 13.9 What this means for the island plan (§6)

The critical list in §6 stands, with this evidence and these additions:

- **World-mode-aware POI placement** (KI1) is the most visible defect: plateaus in the sea, markers over the void. It
  needs the island height *and* a world-bounds test, so POIs past R+F are not placed at all.
- **A world edge** (KI2) must include recovery (KI14): a respawn when the pawn falls out of the world, and a timeout or
  fallback for the chunk-based wait.
- **Slab culling for islands** (KI5) and **3D-aware LOD distance for islands, or a per-column vertical band** (KI13) are the
  performance items *(both done in M4, §14.8: a per-column window from the terrain and 3D LOD distance)*: together they explain the chunk excess, the generation backlog and most of the coast misses. §13.11
  measured KI5 per layer: on the chord the island's ~340 extra chunks are layers −3, 2 and 3, the layers the control's
  slab trims.
- Add from §13.11: **rebuild the balanced-LOD cache only when the viewer moves** (1.3-1.9 ms a tick on the control, every
  tick), and decide whether heightmap modes should take the **vertical range from the terrain** instead of the viewer's
  altitude (the −2 floor).
- ~~**Find the cause of KI12** before the next island bench; until then island benches need at least two runs per route.~~
  Done (§13.10): KI12 was a latch in LOD evaluation, in every world mode. It is fixed, and one run per route is enough
  again. Island numbers to use are the high-mode rows of §13.3.
- **Save fingerprint** (KI7), **one island-centre definition** (KI3) and **the bounds query** (KI9) as already listed.
- Add: **evict or downsample map tiles** (KI15) before islands larger than about 10 km. *(Done in M6, §14.10)*

§14 turns this list into work items (M2-M4 and M6).

### 13.10 KI12 explained and fixed: when LOD levels are evaluated (2026-10-13)

**Cause.** `UVoxelChunkManager::EvaluateLODLevelChanges` walks every loaded chunk, compares its committed LOD with the
strategy's current answer, and queues at most `MaxLODRemeshPerFrame` of the differences (8 in the demo); the rest wait for
the next evaluation. The evaluation ran only on a tick where the viewer entered another chunk, or while a sweep flag
was set (`VoxelChunkManager.cpp:765-791` as of `e1066ef`). The flag was set on any tick where all five queues (generation
queued and in flight, meshing queued and in flight, upload) were empty, and cleared only by an evaluation on such a tick
that left the meshing queue empty. While streaming the queues are never empty, so the flag kept whatever state it had at
the last tick they were:

- **Flag set: LOD evaluated every tick** ("high mode"). Every LOD change is queued within a frame or two.
- **Flag clear: LOD evaluated only at viewer-chunk changes**, about 121 on the 500,000 uu chord, 8 re-meshes each
  ("low mode"). Everything past those 8 waits for the next chunk change, so the backlog never drains.

The island chord starts at R+F, where little loads, so the queues go empty dozens of times in its first 25,000 uu (47-136
ticks in the four §13.3 runs) and then not again until the route leaves the island. The flag's state at the last of
those ticks, set if an LOD re-mesh was still queued and clear if not, decided the whole run. That is a race between
meshing latency and the viewer's advance, hence two modes from identical starts.

**Evidence in the §13.3 CSVs**, before any new code:

- **Low runs (b, d):** 944 and 948 of their 1,343 and 1,372 re-meshes were queued on the tick after a viewer-chunk change,
  and 160 of run b's 176 increments are exactly 8. The rest came at the end of the route, past the island, where the
  queues drain and the flag sets.
- **High runs (a, c):** re-meshes on 1,873 and 1,853 ticks of the traverse, spread along the whole route at 570-830 per
  25,000 uu.
- **The control is exposed to the same race.** Its PIE runs happened to latch (7,168 twice). Its headless run did not:
  921 of its 1,142 re-meshes came the tick after a chunk change, and 120 of its 136 increments were batches of 8. So
  the headless "8× the control" in §13.3 compared the two regimes, not the two world modes.
- **The soak's doubling at minute 10 (§13.5) is not KI12.** Its log shows executed DC meshes flat at about 6,000 a minute
  and "LOD level changes" lines flat at about 400 a minute for all 25 minutes. The extra ~2,400 re-queues a minute from
  minute 10 on add up to its 36,600 "other" re-queues, and minute 10 is when the orbit starts its second lap
  (2π × 150,000 uu at 1,500 uu/s = 628 s, the view reaching lap-1 terrain 27 s earlier). The bench remembered every
  chunk it had meshed and never forgot one on unload, so each chunk reloaded on lap 2 counted as a re-mesh (G10, fixed
  below).

**The high mode is the intended one.** New bench columns count, every sample, the Loaded chunks whose committed LOD
differs from the strategy's answer with nothing queued for them: LOD changes the manager has not even queued, and the
subset the strategy puts at LOD 0 (near the viewer, shown coarser than they should be). In the low mode about 285 chunks
were at the wrong LOD on average, 32 of them near the viewer, on 99% of ticks. With the fix below the backlog averages
1.7-2.1 chunks, and the near ones are a one-tick blip at each chunk change (the sample is taken before that tick's
evaluation). The low mode was cheaper only because it skipped LOD work it owed.

**Fix** (`VoxelStreaming/Public/VoxelLODEvalSchedule.h`, used by `UVoxelChunkManager::TickComponent`). The flag is
replaced by `FVoxelLODEvalSchedule`. No trigger depends on what the queues held at some earlier moment. LOD levels are
evaluated when:

- the viewer entered another chunk, or moved 100 uu since the last evaluation (LOD bands are distances from the viewer's
  position, not its chunk);
- the last evaluation stopped at the per-frame cap with candidates left;
- the pipeline went from busy to idle since the last evaluation that queued nothing. This sweep keeps the old flag's
  purpose (a still viewer gets chunks that became Loaded with a stale LOD), ends at the first evaluation that queues
  nothing, idle or not, and costs nothing while the viewer stands still in a settled world (the flag re-evaluated every
  idle tick).

`EvaluateLODLevelChanges` now returns its candidate count, the number queued and whether it hit the cap.

**Bench.** CSV `LODCand` (candidates found by this tick's evaluation, −1 when none ran), `LODQueued`, `LODStale`, `LODStaleNear`;
JSON `lodEvalFrameFraction`, `lodStaleMean/P95/Max`, `lodStaleNearMean/Max` over the traverse. `LODStale` walks the loaded
chunks each sample (`GetBenchLODBacklog`), outside the manager's timed sections. G10: the "ever meshed" set forgets a chunk
when it unloads, so a reloaded chunk's first mesh is not counted as a re-mesh. Tests:
`VoxelWorlds.Streaming.LODEvalSchedule.SameRegimeWhateverTheIdleHistory` (models the old flag too: the same busy traverse
evaluates every tick or only at chunk changes depending on one idle tick, and the schedule evaluates the same ticks
either way), `.CapLeftoversEvaluateNextTick`, `.Triggers`. The `VoxelWorlds+VoxelWorldSave` suites pass 218/218 headless
on the final commit. No meshing or generation code changed, so the GPU tests that only run in the editor were not re-run.

**Method.** VoxelWorlds `285cf3c` (branch `fix/ki12-island-lod-bimodal`), built in a worktree. That commit carries a
temporary cvar, `voxel.LOD.LegacySweep 1`, that ran the old flag (removed in the next commit), so legacy and fixed rows
come from one binary; the legacy path logged the flag's state each time the pipeline turned busy. PIE in the level viewport, background throttle off, one cold fresh
world per run, `DrowningDamagePerSecond` 0 in memory, the §13.3 chord: `voxel.Bench.Run <tag> 3000 500000 warmup=120
start=-250000:0 dir=1:0 follow=300`, legacy and fixed runs interleaved. Unlike §13.3, this editor was not the foreground
window and the main checkout's editor stayed open beside it, so frame and manager times compare only within these tables.
JSON reports are in [Research/Bench/KI12/](Research/Bench/KI12/).

**PIE, island M chord v3000:**

| Run | LOD re-meshes | Ticks evaluating LOD | Ticks at the cap | Candidates per evaluation, mean | Stale LOD, mean / max | Stale near the viewer, mean / max | Frame P95 ms | Manager P95 ms | JSON |
|-----|---------------|----------------------|------------------|---------------------------------|-----------------------|-----------------------------------|--------------|----------------|------|
| Legacy a | 1,288 | 3% | 126 | 214 | 284 / 394 | 32.6 / 44 | 47.0 | 13.5 | `20261013_085644_ki12_L_M_v3000a` |
| Legacy b | 1,308 | 3% | 126 | 229 | 291 / 394 | 31.8 / 44 | 53.0 | 15.1 | `20261013_090330_ki12_L_M_v3000b` |
| Legacy c | 1,300 | 3% | 126 | 222 | 286 / 394 | 32.4 / 44 | 48.6 | 14.6 | `20261013_091022_ki12_L_M_v3000c` |
| Legacy d | 1,306 | 3% | 126 | 221 | 287 / 394 | 32.4 / 44 | 49.5 | 14.5 | `20261013_091716_ki12_L_M_v3000d` |
| **Fixed a** | **11,231** | 80% | 1,027 | 4.8 | 2.1 / 30 | 0.25 / 16 | 54.9 | 18.1 | `20261013_090006_ki12_N_M_v3000a` |
| **Fixed b** | **10,683** | 86% | 920 | 4.4 | 1.7 / 26 | 0.25 / 16 | 58.4 | 19.3 | `20261013_090655_ki12_N_M_v3000b` |
| **Fixed c** | **10,295** | 86% | 888 | 4.4 | 1.7 / 24 | 0.23 / 16 | 60.4 | 19.4 | `20261013_091350_ki12_N_M_v3000c` |
| **Fixed d** | **10,621** | 85% | 918 | 4.5 | 1.7 / 28 | 0.23 / 16 | 58.0 | 19.0 | `20261013_092040_ki12_N_M_v3000d` |
| Control, fixed a | 7,145 | 85% | 29 | 2.0 | 0.2 / 8 | 0.02 / 2 | 58.7 | 19.2 | `20261013_092416_ki12_N_C_v3000a` |
| Control, fixed b | 7,166 | 83% | 34 | 2.0 | 0.2 / 8 | 0.03 / 2 | 56.3 | 18.9 | `20261013_092752_ki12_N_C_v3000b` |

- **The legacy flag decided every run the same way here:** in all four the log shows it set at one or two busy turns
  in the first 1,100 ticks and clear at the last (ticks 710, 1,043, 1,045, 1,055), so all four ran low. The §13.3 session
  drew high in its M batch. The race is the same, only the odds differ with the machine's state.
- **Fixed: one regime, run after run.** 10,295-11,231 island LOD re-meshes, within 5% of their mean. The spread follows the
  number of ticks (4,204-4,632): the island hits the cap of 8 on about 21% of ticks, and the slower a run, the more LOD
  changes fold into requests that are still queued. The control's two runs land within 0.3% of each other and of
  §13.3's 7,168, which was the latched regime: the fix leaves the control where it already was.
- **The island needs more LOD throughput than the control** (cap reached on 888-1,027 ticks against 29-34), because its
  2D LOD distance changes whole columns at once (KI13). Its backlog still stays near zero.
- **The owed LOD work has a cost:** fixed vs legacy (low) island, frame P95 57.9 vs 49.5 ms (+17%) and manager P95 19.0 vs
  14.4 ms (+32%), means of the four runs each. §13.3's high and low runs differed the same way (+28%, +46%).
- **Same regime, same cost (this setup):** fixed island vs fixed control, frame P95 57.9 vs 57.5 ms and manager P95 19.0 vs
  19.1 ms, for 1.5× the LOD re-meshes. §13.3's foreground high-mode rows (+9% frame, +17% manager) remain the island's
  threshold numbers; its low-mode rows (−14%, −20%) are not valid, because they owed the LOD work they skipped.

**Headless CPU** (`voxel_bench_sweep.ps1 -PreCmds "voxel.LOD.LegacySweep 1"` or `0`, the same chord, the worktree
editor idle and the main editor open; the first row's build label reads `73613df-dirty` because the sweep took it before
`285cf3c` was committed, from the same binary):

| Run | LOD re-meshes | Ticks evaluating LOD | Stale LOD, mean / max | Stale near the viewer, mean / max | Frame P50 / P95 / P99 ms | Manager P95 ms | JSON |
|-----|---------------|----------------------|-----------------------|-----------------------------------|---------------------------|----------------|------|
| Island, legacy | 6,895 | 99.4% | 0.3 / 48 | 0.10 / 14 | 4.0 / 11.0 / 17.4 | 7.2 | `20261013_093217_ki12HL_L_M_v3000` |
| **Island, fixed** | **7,149** | 14.9% | 0.8 / 28 | 0.10 / 14 | 3.8 / 10.7 / 16.9 | 6.9 | `20261013_093619_ki12HL_N_M_v3000` |
| **Island, fixed, repeat** | **6,908** | 15.1% | 0.7 / 28 | 0.10 / 14 | 3.9 / 10.8 / 17.1 | 7.0 | `20261013_094930_ki12HL_N_M2_v3000` |
| Control, legacy | 1,129 | 1.2% | 159 / 245 | 4.0 / 9 | 4.6 / 9.5 / 13.7 | 5.5 | `20261013_094050_ki12HL_L_C_v3000` |
| **Control, fixed** | **5,996** | 16.5% | 0.5 / 8 | 0.10 / 3 | 5.1 / 10.4 / 14.1 | 6.6 | `20261013_094522_ki12HL_N_C_v3000` |
| **Control, fixed, repeat** | **5,888** | 16.6% | 0.5 / 8 | 0.10 / 3 | 5.2 / 10.5 / 14.2 | 6.6 | `20261013_095401_ki12HL_N_C2_v3000` |
| Island, final commit, one editor open | 9,162 | 12.1% | 0.9 / 28 | 0.10 / 14 | 3.0 / 8.9 / 15.3 | 5.6 | `20261013_100839_ki12HL_F_M_v3000` |

- **The old flag gave both regimes from one binary:** high for the island and low for the control, as in §13.3's headless
  runs (9,269 / 9,304 and 1,142 there). With the old flag, the PIE island here was low four times and the headless island
  high.
- **Fixed: one regime, both world modes.** Island 7,149 and 6,908 (3.4% apart), equal to the latched island run; control
  5,996 and 5,888 (1.8% apart). Headless ticks take about 3 ms, so the 100 uu trigger evaluates every 6-7 ticks, and the
  backlog stays below one chunk on average.
- **Island vs control in the same regime:** 1.2× the LOD re-meshes (not §13.3's 8×), frame P95 +3% (10.7-10.8 vs
  10.4-10.5 ms) and manager P95 +5% (6.9-7.0 vs 6.6 ms). §13.3's headless "+24% frame / +28% manager" set the island's
  high regime against the control's low one; here the low-regime control's manager P95 is 5.5 ms against 6.6 ms fixed.
- **Headless counts follow machine load, not the regime.** The last row is the final commit (the cvar removed; label
  `285cf3c-dirty`), run after this session's editor was closed, so one editor was open as in §13.3. It ran 44,202 ticks
  with 577 chunks loaded on average, against about 34,000 ticks and 440 chunks for the rows above (two editors open), and
  §13.3's headless island runs, 44,255-45,613 ticks and 580-587 chunks. Its 9,162 LOD re-meshes match §13.3's 9,269 and
  9,304. Headless CPU generation cannot keep up at v3000; the slower it is, the closer to the viewer chunks first load,
  already at their final LOD, so fewer of them pass through the transitions. Compare headless counts only within one
  batch; the regime shows in the stale-LOD columns.
- **G10:** a two-lap PIE orbit on M with the fix (`orbit=0:0:60000`, v6000, 753,982 uu): 4,845 chunks unloaded and
  reloaded, 6,638 LOD re-meshes and 0 "other" (`20261013_095738_ki12_G10_orbit2`).

**What changes in §13.** I4's chord verdict stands on the high-mode rows. KI12 no longer splits island benches, so one run
per route is enough again. The §13.3 headless comparison should be re-read against a control in the same regime (above).
§13.5's re-mesh doubling is G10, not an island effect.

### 13.11 The vertical candidate window counted the viewer's layer twice (2026-10-13)

Found while tracing KI12 (§13.10). Not an island finding: every world mode had it.

**Bug.** Each world mode streams a window of chunk layers around the viewer's layer: InfinitePlane −2 to +8,
IslandBowl −2 to +4, SphericalPlanet ±10 in the demo geometry. The terrain-bounds cull clips the window to the world's
height range (−16,800 to 14,800 uu here, layers −4 to 3). `FDistanceBandLODStrategy::GetVerticalChunkRange` already
returns absolute layers (viewer layer + offsets), and both candidate loops (`GetVisibleChunks` and
`RebuildBalancedLODCache`) added the viewer's layer again. A viewer on layer L streamed 2L−2 to 2L+8. On layer 0, where
most play and every review bench start sits, that is the same window, which is why it went unnoticed since the VoxelLOD
module was created (`13c3672`). Off layer 0 the window slid twice as far as the viewer:

| Viewer | Island window, old / fixed | InfinitePlane window, old / fixed |
|--------|----------------------------|-----------------------------------|
| Layer −1 (island chord over the sea, z −3,200) | −4 to 2 / −3 to 3 | −4 to 3 / −3 to 3 |
| Layer 0 | −2 to 3 / same | −2 to 3 / same |
| Layer 1 (camera or flight at z 4,800-9,600) | 0 to 3 / −1 to 3 | 0 to 3 / −1 to 3 |

(All clipped to layers −4 to 3; unclipped, InfinitePlane's old window on layer −1 was −4 to 6. Its 3D view distance also
stops the top at +8.) The R24 survey note that "a
camera at z ≥ 4,800 drops the lowest terrain layer" was this. Spherical would be worse: a viewer at the pole of a
100 km planet sits near layer 20, so its window centred near layer 40, entirely above the surface. That belongs on the
spherical requirements list when spherical streaming is picked up.

**Fix.** Both loops iterate the absolute layers `GetVerticalChunkRange` returns; the offsets themselves are unchanged.
Test: `VoxelWorlds.LOD.VerticalWindow.FollowsViewerLayer` (viewer on layers −3, −1, 0, 1 and 5, both heightmap modes). It
fails on the old window, e.g. "viewer on layer −3: lowest candidate layer expected −5, was −8".

**Also timed: the LOD cache rebuild.** `LODStrategy->Update`, which rebuilds the balanced-LOD cache over the whole
candidate volume every tick, ran outside every timed section. It is now `LODUpdateMs` (CSV `LODUpdMs`, JSON
`lodUpdateMsP50/P95`). It matches the ~2 ms of unattributed manager P95 noted for the control in §12.8: 1.3-1.4 ms P50 and 1.7-1.9 ms P95
on the control's chord, 0.7 / 1.0 ms on the island's, and 0.6-0.7 ms every tick on a still viewer in a settled world. It
depends only on the viewer's position and last tick's result, so it could run only when the viewer moves, like the LOD
evaluation in §13.10. That is a candidate work item, not done here.

**Bench.** New JSON `loadedByLayerMeanMax`: loaded chunks per layer over the traverse, mean and max, sampled with the
memory stats.

**Method.** VoxelWorlds `bdfd37a` on top of §13.10, with a temporary `voxel.LOD.LegacyVerticalWindow 1` that restored the
old window (removed in the next commit), so both windows come from one binary. This time the worktree editor was the only
editor open (not in the foreground), background throttle off, cold fresh world per run, drowning off in memory, legacy and
fixed interleaved. Routes: the §13.3 chord (`follow=300`); a straight flight at z 7,200 (layer 1, `start=-250000:0:7200
dir=1:0`, 150,000 uu at v3000); and S at its spawn (`warmup=300`, zero distance, as in §13.4). JSON reports are in
[Research/Bench/VerticalWindow/](Research/Bench/VerticalWindow/).

| Run | Window | Loaded chunks per layer, mean over the traverse | Loaded mean / peak | LOD re-meshes | Audit | Frame P95 / manager P95 ms | LOD cache rebuild P50 / P95 ms | JSON |
|-----|--------|-------------------------------------------------|--------------------|---------------|-------|----------------------------|--------------------------------|------|
| Island M chord a | old | −4: 168, −3: 180, −2: 190, −1: 194, 0: 194, 1: 187, 2: 176, 3: 100 | 1,390 / 1,646 | 12,628 | 64 | 32.5 / 11.9 | 0.71 / 1.01 | `20261013_195518_vw_L_M_chorda` |
| Island M chord b | old | −4: 168, −3: 180, −2: 189, −1: 193, 0: 193, 1: 186, 2: 176, 3: 102 | 1,386 / 1,598 | 12,630 | 64 | 31.8 / 11.7 | 0.71 / 1.01 | `20261013_200140_vw_L_M_chordb` |
| **Island M chord a** | **fixed** | −3: 203, −2: 213, −1: 217, 0: 217, 1: 211, 2: 202, 3: 191 | 1,454 / 1,666 | 11,522 | 52 | 31.7 / 11.9 | 0.71 / 1.03 | `20261013_195828_vw_N_M_chorda` |
| **Island M chord b** | **fixed** | −3: 211, −2: 220, −1: 224, 0: 223, 1: 220, 2: 212, 3: 200 | 1,510 / 1,662 | 11,522 | 52 | 31.1 / 11.8 | 0.71 / 1.01 | `20261013_200452_vw_N_M_chordb` |
| Control chord | old | −4: 29, −3: 117, −2: 227, −1: 234, 0: 233, 1: 223, 2: 56, 3: 33 | 1,152 / 1,247 | 7,168 | 0 | 29.2 / 10.0 | 1.38 / 1.85 | `20261013_200803_vw_L_C_chord` |
| **Control chord** | **fixed** | −3: 119, −2: 231, −1: 239, 0: 238, 1: 228, 2: 57, 3: 33 | 1,144 / 1,219 | 7,123 | 0 | 28.3 / 9.6 | 1.27 / 1.73 | `20261013_201115_vw_N_C_chord` |
| Control at z 7,200 | old | 0: 241, 1: 244, 2: 42, 3: 33 | 561 / 806 | 1,611 | 0 | 17.7 / 4.2 | 1.03 / 1.19 | `20261013_201227_vw_L_C_high` |
| **Control at z 7,200** | **fixed** | −1: 228, 0: 240, 1: 244, 2: 42, 3: 33 | 787 / 847 | 1,861 | 0 | 21.3 / 6.0 | 1.22 / 1.44 | `20261013_201338_vw_N_C_high` |
| Island M at z 7,200 | old | 0: 222, 1: 223, 2: 222, 3: 222 | 889 / 984 | 1,960 | 6 | 19.4 / 5.0 | 0.64 / 0.76 | `20261013_201446_vw_L_M_high` |
| **Island M at z 7,200** | **fixed** | −1: 220, 0: 221, 1: 222, 2: 221, 3: 220 | 1,106 / 1,230 | 2,450 | 8 | 24.1 / 7.5 | 0.70 / 0.91 | `20261013_201557_vw_N_M_high` |
| Island S at spawn | old | — | — / 1,512 | 0 | 0 | 16.7 / 0.9 | 0.64 / 0.64 | `20261013_201617_vw_L_S_settle` |
| **Island S at spawn** | **fixed** | — | — / 1,518 | 0 | 0 | 16.7 / 0.9 | 0.73 / 0.73 | `20261013_201641_vw_N_S_settle` |

What the numbers say:

- **Off layer 0 the old window lost terrain.** Flying at z 7,200 it streamed nothing below layer 0: no sea floor on the
  island and no terrain below z 0 on the control. Fixed, layer −1 loads (220-228 chunks); the cost is the work the old
  window skipped (manager P95 +1.8 / +2.5 ms on these flights).
- **It does not explain KI5 or KI11.** S at its spawn is unchanged (1,512 / 1,518, layer 0). On the chord the island
  keeps seven full layers with either window; the fix only trades layer −4 for a full layer 3 (loaded mean +7%, peak
  about the same, LOD re-meshes −9%). The audit still lists only KI11's top and bottom layer chunks (52 instead of 64).
- **KI5, measured per layer.** On the same chord the island keeps 191-224 chunks in every layer from −3 to 3. The control
  keeps 228-239 in layers −2 to 1 and only 33-119 in layers −3, 2 and 3, where its far-band slab culling trims columns to
  their surface. The island's ~340 extra chunks (+30% on this route) are those three layers: +84, +145 and +158 in run a. Slab
  culling for islands (§13.9) would remove most of them.
- **The −2 floor is still a design question.** *(Decided by D8 and built in M4, §14.8: the window comes from the terrain;
  at z 7,200 layers −3 and −2 now load under the flight, and from 48,000 uu up island S streams whole.)* At z 7,200 the fixed window starts at layer −1, while the control's
  ground-level run keeps 231 chunks in layer −2 and 119 in layer −3. For heightmap worlds the vertical range could come
  from the terrain (the global bounds and per-column slab already bound it) instead of the viewer's altitude. The §13.1
  I2 survey "from a camera 48,000 uu up" had a window entirely above the terrain (layer 10 ± offsets) with either window,
  so its chunk-sized blank rectangles may be chunks that never streamed rather than a camera artefact; not re-checked.
- **One regime, one editor:** the island chord gave 11,522 LOD re-meshes twice with the fixed window and 12,628 / 12,630
  with the old one, so §13.10's schedule holds run to run. With one editor open the counts sit nearer §13.3's high mode
  than §13.10's two-editor runs, as §13.10 predicted.

## 14. Second revision: work items (2026-10-14)

This section turns the findings of §12 and §13 into work items. It was written after the review and fix PRs merged
(VoxelWorlds `630e4a5`, UE5_PluginDev `0bc5ee2`; the `VoxelWorlds+VoxelWorldSave` suites pass 219/219 headless on
that build) and follows the decisions of 2026-10-14 in §8: D1 (cubic is terrain-only), the D3 memory budget, D8 (the
vertical range comes from the terrain) and D9 (the order of work).

Sizes are as in the QA refinement plan: S = a few hours, M = a day or two, L = several days. Work item IDs (`WM-*`)
are stable. Repos: VoxelWorlds; parent = UE5_PluginDev, which holds the game module, VoxelWorldPOI and
VoxelWorldSave; VCP = VoxelCharacterPlugin.

### 14.1 Status of every finding

Fixed; Open (confirmed, with a work item); Partly (some clauses confirmed, the rest dismissed); Deferred (confirmed,
no work until a decision changes); Dismissed (not a defect, or not worth the work).

Three findings from §13.11 get IDs here:

- **G11:** the vertical candidate window counted the viewer's layer twice. Fixed in §13.11.
- **G12:** the balanced-LOD cache is rebuilt over the whole candidate volume every tick, even for a still viewer:
  1.3-1.4 ms P50 and 1.7-1.9 ms P95 on the control's chord, 0.6-0.7 ms standing still.
- **G13:** the vertical range follows the viewer's altitude. Above layer 0 the lowest terrain layers are not streamed:
  at z 7,200 nothing below layer −1 loads, where the control on the ground keeps 231 chunks in layer −2. The I2 blank
  rectangles seen from 48,000 uu up (§13.1) are probably this; not re-checked.

**Cubic:**

| ID | Finding | Status | Work item |
|----|---------|--------|-----------|
| KC1 | Cubic paid for a LOD system it does not use | Fixed (D7, §12.7) | — |
| KC2 | Cubic is the last user of the legacy whole-chunk path | Fixed in M5 (WM-C1, §14.9): cubic meshes chunk boundaries in face-seam jobs; the legacy path is deleted. Against a same-build control, catch-up and manager P95 are within the thresholds at every speed | WM-C1 |
| KC3 | GPU cubic meshing is test-only | Fixed in M5 (WM-C4, §14.9): deleted with its tests | WM-C4 |
| KC4 | Cubic scatter is wrong with GPU extraction | Fixed in M5 (WM-C3, §14.9) | WM-C3 |
| KC5 | Player dig and place are tuned for smooth terrain | Fixed in M5 (WM-C2, §14.9): one click is one block | WM-C2 |
| KC6 | Surface queries return the smooth surface, so props sink into blocks | Deferred (D1: first-class only) | — |
| KC7 | Two-block rises strand NPCs | Partly confirmed; deferred (D1) | — |
| KC8 | No content exercises cubic | Fixed in M1 (WM-H5; UE5_PluginDev#102, merged): `DemoCubicWorldConfig` is on the intended path and `Config/CubicScatterConfig` is deleted. The review configs exist | WM-H5 |
| KC9 | Voxel trees are not gameplay | Deferred (D1). The unlimited yield from one voxel is G3 | — |
| KC10 | The smooth config's tree templates use material IDs that do not exist | Fixed in M1 (WM-H4; draquel/VoxelWorlds#85, UE5_PluginDev#102, merged) | WM-H4 |
| KC11 | Far compression was keyed on the LOD level | Fixed (§12.7) | — |
| KC12 | Melee misses across a one-block height difference | Deferred (D1) | — |
| KC13 | Vertical walls at the edge of the loaded region | Fixed by WM-C1 (§14.9): no boundary face is built without both chunks, so the edge is open as on the control | (WM-C1) |
| KC14 | The legacy path re-queues a waiting chunk every frame | Gone with the legacy path (WM-C1, §14.9) | (WM-C1) |

**Island:**

| ID | Finding | Status | Work item |
|----|---------|--------|-----------|
| KI1 | POI placement samples InfinitePlane height in every mode | Fixed in M2 and M3 (WM-A3, WM-I1; §14.6, §14.7): placement samples the configured world mode, and only sites wholly inside the bounds, falloff ring included, are placed, whatever the type's surface filter. `POIPlacementProbe` asserts on S, M and L | WM-I1 |
| KI2 | No world edge | Fixed: a hard boundary at R+F with a push-back before it in M3 (WM-I5, §14.7); the visible edge in M6 (WM-E1, §14.10): the seabed slopes from the coast and a far ocean reaches the horizon | WM-I5 (boundary), WM-E1 (visible edge) |
| KI3 | The island centre means two different things | Fixed in M2 (WM-A1, §14.6): one builder, the centre is `WorldOrigin` + offset everywhere; `CullCentreProbe` asserts | — |
| KI4 | Shape-parameter defects | Fixed: `bIslandBowlShape` removed, the Exponential falloff reaches 0 at R+F and `GetDensityAt` agrees with the floor beyond it (M1, WM-H6); the param builders are one (M2, WM-A1), and `GetTerrainHeightBounds` now includes the floor beyond the edge (M2, WM-A2) | — |
| KI5 | No far-band slab culling on islands | Fixed in M4 (WM-S1, §14.8): every column of every heightmap world keeps its surface slab at every distance; island M's peak loaded chunks are within −15% to +8% of the control on every route (were +29-55%) | WM-S1 |
| KI6 | No finite-world concept; `MaxLoadedChunks` is read nowhere | Fixed for the setting in M4 (WM-S3, §14.8): removed, and `LOD_SYSTEM.md` says what bounds residency. Pre-generation and baking stay nice-to-haves (§6) | WM-S3 |
| KI7 | The save key ignores world shape | Fixed in M2 (WM-A4, §14.6): the save records the world's fingerprint; a save from a different radius is refused, kept on disk, and the world starts fresh | — |
| KI8 | Spawn is only water-aware | Fixed in M3 (WM-I3, §14.7): the spawn search takes only points inside the bounds, and the water level includes `WorldOrigin.Z` | WM-I3 |
| KI9 | Consumers have no island mask | Fixed in M3 (WM-I2, §14.7): map exploration and the world map's pan, markers, the PCG sampler and navigation use the bounds; dismissed for scatter and surface NPCs | WM-I2 |
| KI10 | Island test coverage is minimal | Fixed in M3 (WM-I6, §14.7): the probes assert (`CullCentreProbe` since M2, `POIPlacementProbe` in full); `VoxelWorlds.WorldModes.Api.*`, `.Boundary.*`, `POI.Placement.WorldBounds`, `Navigation.WorldBounds` and `VoxelCharacter.WorldEdge.*` cover the bounds and their consumers. `WP_IslandBowlConfig` deleted | WM-I6 |
| KI11 | The residency audit and island LOD measure distance differently | Fixed in M4 (WM-S2, §14.8): the audit, the queue eviction and the edit pins measure range with the strategy's own horizon distance; island audits 0 in every M4 run | WM-S2 |
| KI12 | Bimodal LOD re-meshing | Fixed (§13.10), in every world mode | — |
| KI13 | Island LOD distance is 2D, so whole columns stay in the near bands | Fixed in M4 (WM-S2, §14.8): LOD distance is 3D in every world mode | WM-S2 |
| KI14 | No recovery when the pawn leaves the generated world | Fixed in M3 (WM-I4, §14.7), in every world mode | WM-I4 |
| KI15 | Map tile images are never evicted | Fixed in M6 (WM-E2, §14.10): detail and overview images under a fixed 80 MB budget, least recently used first, drawn again on demand; a fully explored L holds 37 MB (539 MB before) | WM-E2 |

**Mode-independent and tooling:**

| ID | Finding | Status | Work item |
|----|---------|--------|-----------|
| G1 | The world component's vertex, triangle and GPU memory stats only ever add | Fixed in M1 (WM-H2; draquel/VoxelWorlds#84, merged) | WM-H2 |
| G2 | A Subtract on a Set voxel replaces the Set | Fixed in M1 (WM-H3; draquel/VoxelWorlds#86, merged) | WM-H3 |
| G3 | Gathering pays out on every dig, without limit on one voxel | Fixed in M1 (WM-H7; VoxelCharacterPlugin#21, merged) | WM-H7 |
| G4, G8 | `follow=` routes over the sea drown the pawn | Fixed in M1 (WM-H8; draquel/VoxelWorlds#84, VoxelCharacterPlugin#21, merged) | WM-H8 |
| G5 | The bench's thrash counter counts deferral re-queues | Fixed in M1 (WM-H8; draquel/VoxelWorlds#84, merged) | WM-H8 |
| G6 | The compression encode ran on the game thread | Fixed (§12.8) | — |
| G7 | Streaming follows the view target, not the pawn | Dismissed: by design (a survey camera is a viewer). Release `pie_camera` before moving the pawn | — |
| G9 | The bench puts the pawn back at its start with no settle | Fixed in M1 (WM-H8; draquel/VoxelWorlds#84, merged) | WM-H8 |
| G10 | The bench's re-mesh counter counted reloads | Fixed (§13.10) | — |
| G11 | The vertical window counted the viewer's layer twice | Fixed (§13.11) | — |
| G12 | The balanced-LOD cache is rebuilt every tick | Fixed in M1 (WM-H1; draquel/VoxelWorlds#84, merged) | WM-H1 |
| G13 | The vertical range follows the viewer's altitude | Fixed in M4 (WM-S1, §14.8): at z 7,200 layers −3 and −2 load under the flight on both sides, and from 48,000 uu up island S streams whole | WM-S1, WM-S4 |
| G14 | The first PIE of an editor session re-meshes far more for LOD than later runs on the same route (533 against 287-304 on the control's 40,000 uu chord, in two sessions; §14.8) | Open (tooling), not investigated: start a bench batch with a throwaway PIE | — |
| G15 | A recreated scene proxy dropped every chunk mesh, seam bucket and water tile it held (found in M5's C4, §14.9) | Fixed in M5: the chunk manager re-sends them (`RecoverLostRenderData`), every meshing mode | (WM-C1) |
| G16 | The water material (`M_Water`) draws thin dark bands just under its horizon at grazing angles, at fixed screen rows (found in M6, §14.10). The water tiles alone show them on island M with the far ocean hidden; until M6 the water ended before the distance where they appear | Open, cosmetic; the material's, not the far ocean's | — |
| G17 | Small holes in DC face seams between chunks of different LOD: two open-edge loops of 5 or 6 edges where a face seam joins chunks at LOD 1 and 2 (also 0 and 1), found by spherical Phase 1's closed-surface test at the pole, 45° and the equator, and reproduced on an InfinitePlane chunk manager (§15.2). The seam is ready and clean when measured; MS1's unit tests do not cover 1\|2 or noise terrain | Fixed (§15.3): a fine crossing faced a coarse cell with no crossing at its own stride and the fan onto it was skipped; such a cell now gets a boundary fallback vertex in the mixed face, edge and corner meshers. `MS4_MixedFaceClosureNoise` covers 0\|1 to 2\|0 on noise terrain, and `Streaming.{Planet,Plane}.ClosedSurface` fail on any G17 gap | — |
| T6 | `STATGROUP_Voxel` cycle counters | Not planned: `LODUpdateMs` (§13.11) accounted for the unattributed manager time | — |
| §11 | Documentation that disagrees with the code | Fixed in M1 (WM-H9, draquel/VoxelWorlds#88; WM-H10's spherical items, draquel/VoxelWorlds#87; merged). Earlier: the `LOD_SYSTEM.md` cubic note, the project `CLAUDE.md` GPU-skip note | WM-H9, WM-H10 |
| I10 | World Partition on an island | Not run; deferred with World Partition (`Research/ENGINE_INTEGRATION.md`) | — |

### 14.2 Work items

**M1. Hygiene.** Small and independent: any order, in parallel, one PR each or a few bundled. **Status 2026-10-14: all ten
implemented and verified; merged 2026-10-15** (results in §14.5): draquel/VoxelWorlds#84 (WM-H1, H2, H8), draquel/VoxelWorlds#85 (WM-H6, H4 code), draquel/VoxelWorlds#86 (WM-H3), draquel/VoxelWorlds#87 (WM-H10,
stacked on draquel/VoxelWorlds#85), draquel/VoxelWorlds#88 (WM-H9 and this status, stacked on #83); draquel/VoxelCharacterPlugin#21 (WM-H7); parent
draquel/UE5_PluginDev#102 (content for WM-H4, H5, H10 and the gitlinks).

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-H1 | **Rebuild the balanced-LOD cache only when the viewer moves.** Trigger it the way `FVoxelLODEvalSchedule` triggers LOD evaluation (viewer chunk change or 100 uu of movement), plus config and bounds changes. `LODUpdateMs` shows the result: about 0 for a still viewer, with the control chord's LOD re-meshes and stale-LOD columns unchanged within one batch | G12 | VoxelWorlds | S | — |
| WM-H2 | **Renderer stats report current values.** `GetTotalVertexCount`, `GetTotalTriangleCount` and `GetGPUMemoryUsage` subtract on unload and replace, or read the renderer's `GetDebugStats` | G1 | VoxelWorlds | S | — |
| WM-H3 | **A Subtract composes with a Set.** A dig under a POI pad removes only what the brush reaches. Edit-layer test plus a save round trip | G2 | VoxelWorlds | M | — |
| WM-H4 | **Valid tree templates.** Fix `DemoSmoothWorldConfig`'s leaf and trunk IDs (Wood 20, Leaves 21), and warn at config load when a template uses a material ID the registry does not have | KC10 | VoxelWorlds, parent (content) | S | — |
| WM-H5 | **Cubic content.** Bring `DemoCubicWorldConfig` to the intended path (greedy; LOD is forced off since D7) or replace it with `Review_Cubic`; delete the orphaned `Config/CubicScatterConfig` | KC8 | parent (content) | S | — |
| WM-H6 | **Island shape parameters.** Remove the unread `bIslandBowlShape` (a real bowl shape stays a nice-to-have in §6), make Exponential falloff reach 0 at R+F, and make `FIslandBowlWorldMode::GetDensityAt` agree with generation beyond R+F | KI4 | VoxelWorlds | S | — |
| WM-H7 | **Gathering yields once per voxel.** Pay out only when a dig turns a solid voxel to air | G3 | VCP | S | — |
| WM-H8 | **Bench tooling.** A pawn the bench is flying cannot drown (G4, G8); the end-of-run restore holds the pawn until collision under it is ready (G9); the thrash counter counts executed meshes, not queue insertions (G5) | G4, G5, G8, G9 | VoxelWorlds | S | — |
| WM-H9 | **Docs.** The §11 items still open (the spherical ones go with WM-H10); refresh `Research/PERFORMANCE_TARGETS.md` from the review's control numbers, as §1 asked; `Features/WORLD_MODES.md`'s island sections from §13's numbers | §11 | VoxelWorlds | S | — |
| WM-H10 | **Spherical Phase 0, guardrails.** SP-A5 (config validation and honest UI), SP-B3 (spherical worlds generate on the CPU and log why) and SP-I1 (a planet config and test map), plus the spherical doc corrections in §11 | — | VoxelWorlds, parent (content) | M | — |

**M2. The world-mode API.** Shared by the island work and spherical. VoxelWorlds stays independent of the game plugins:
game-side callers (VoxelWorldPOI, VCP, the game module) reach the API through the world actor, as they already reach
the chunk manager's surface queries. **Status 2026-10-15: all four implemented and verified, PRs open** (results in
§14.6): draquel/VoxelWorlds#89 (WM-A1) and draquel/VoxelWorlds#90 (WM-A2, WM-A3 and WM-A4's VoxelWorlds side, and these
docs; stacked on #89); draquel/UE5_PluginDev#103 (WM-A3 in VoxelWorldPOI, WM-A4 in VoxelWorldSave, and the gitlink).

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-A1 | **One params builder per world mode.** Generation (CPU and GPU), the LOD cull, the map, the editor field viewer and the chunk manager build island params (and planet params, SP-A6) with one function, and the island centre is `WorldOrigin` plus the offset everywhere. `CullCentreProbe` becomes an assertion: no land column culled with the demo's origin | KI3, KI4 | VoxelWorlds | M | WM-H6 (same code; do it first or together) |
| WM-A2 | **Bounds and a local frame on `IVoxelWorldMode`.** `IsInsideWorld(X, Y)` and `GetWorldBounds()` (island: the R+F footprint; plane: unbounded; planet: the shell), and SP-A1/SP-A2 with their heightmap implementations: `GetUpVector` is +Z, `GetAltitude` is Z above the datum, `WorldToSurface` is (0, X, Y). The planet implementations come with spherical Phase 1. Blueprint-callable through the world actor | KI1, KI9 | VoxelWorlds | M | WM-A1 |
| WM-A3 | **Height through the world mode.** The LOD strategy's slab sampling and POI placement call the configured world mode instead of `FInfinitePlaneWorldMode`'s static height math | KI1, KI5 | VoxelWorlds, parent (VoxelWorldPOI) | S-M | WM-A1 |
| WM-A4 | **Save fingerprint.** The save header records the world mode, the shape params and a generation version. On a mismatch the save is not loaded: logged, kept on disk, and the world starts fresh. Shared with SP-G9 | KI7 | parent (VoxelWorldSave) | S-M | WM-A1 |

**M3. Island correctness.** Afterwards nothing is placed, explored or spawned past the coast, and leaving the world
cannot soft-lock the player. **Status 2026-10-15: all six implemented and verified, PRs open** (results in §14.7):
draquel/VoxelWorlds#91 (WM-I2: map, PCG, navigation, and the bounds geometry), draquel/VoxelWorlds#92 (WM-I5's
boundary and these docs; stacked on #91); draquel/VoxelCharacterPlugin#22 (WM-I3, WM-I4, WM-I5's push-back and HUD hint,
WM-I2's world map); draquel/UE5_PluginDev#104 (WM-I1, WM-I2's markers, WM-I6, `vox.Edge`, the gitlinks).

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-I1 | **POI placement on the island.** Sites outside the bounds are not placed. Sites in the falloff band are judged on the island's height (RequireLand, the water check, the dungeon cover rule), so pads no longer raise plateaus out of the sea. `POIPlacementProbe` becomes an assertion on S, M and L: no POI past R+F, no land POI under water | KI1 | parent (VoxelWorldPOI) | M | WM-A2, WM-A3 |
| WM-I2 | **Consumers respect the bounds.** Map exploration stops at the bounds and the world map's pan is clamped to them; markers only inside; the PCG sampler skips points outside; navigation treats outside as blocked, water on or off | KI9 | VoxelWorlds (map, PCG, navigation), parent (markers) | M | WM-A2 |
| WM-I3 | **Spawn inside the world.** The spawn search rejects points outside the bounds, so the off-island floor is never taken for land | KI8 | VCP | S | WM-A2 |
| WM-I4 | **Recovery.** Falling out of the world (KillZ) runs the death and respawn path instead of destroying the pawn, and the chunk-based collision wait gives up after a timeout and moves the pawn to the respawn point. Every world mode | KI14 | VCP, parent (game module) | S-M | — |
| WM-I5 | **The boundary (D9).** A hard boundary at R+F, taken from the bounds query, and a soft push-back in the last stretch of sea before it, with a HUD hint | KI2 | VoxelWorlds (boundary), VCP (push-back), parent (HUD) | M | WM-A2, WM-I4 |
| WM-I6 | **Island tests and content.** The probe assertions above; delete or tune `WP_IslandBowlConfig` | KI10 | VoxelWorlds, parent | S | WM-A1, WM-I1 |

**M4. The streaming window.** Every heightmap world; the island's performance items. **Status 2026-10-15: all four
implemented and verified, PRs open** (results in §14.8): draquel/VoxelWorlds#93 (WM-S1, S2, S3 and these docs);
draquel/UE5_PluginDev#105 (the gitlink and the doc map).

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-S1 | **Vertical range from the terrain (D8).** In heightmap modes a column's candidates are its surface slab (the world mode's height ± 1.5 chunks, up to the water plane) at every distance, plus the viewer's layer ± 2 within band 0. The viewer-relative −2 to +8 offsets go. The slab samples the world mode (WM-A3), so islands get it. Check caves, dungeon rooms and edited chunks below the slab near the viewer, the island's floor ring, and a flight at z 7,200 | KI5, G13 | VoxelWorlds | M | WM-A3, WM-H1 (same function) |
| WM-S2 | **3D LOD distance for islands.** Islands measure LOD distance in 3D like InfinitePlane, and the island boundary cull stays 2D. The residency audit then agrees with the unload rule | KI13, KI11 | VoxelWorlds | S | — |
| WM-S3 | **Remove `MaxLoadedChunks`.** The view distance and the island cull already bound residency (§13.4); document what does | KI6 | VoxelWorlds | S | — |
| WM-S4 | **Re-bench, one batch, one build.** Island M chord, perimeter and coast at v1500-v6000, S at its spawn, flights at z 7,200 and the I2 survey from 48,000 uu, each with the control. Check §1's thresholds and the D3 budget, and update §13's verdicts | KI5, KI11, KI13, G13 | VoxelWorlds (docs) | M | WM-S1, WM-S2 |

**M5. Cubic, terrain-only (D1).** **Status 2026-10-16: all four implemented and verified, PRs open** (results in §14.9):
draquel/VoxelWorlds#94 (WM-C1, WM-C3, WM-C4, WM-C2's demo brush, G15 and these docs); draquel/VoxelCharacterPlugin#23
(WM-C2); draquel/UE5_PluginDev#106 (`vox.Dig`'s targeting, the gitlinks and the doc map).

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-C1 | **Cubic on the seam path.** The faces on a chunk boundary belong to the pair's seam job, which culls them against both chunks' voxels (the seam doc's "trivially compatible" case), so a chunk no longer waits for its neighbours' data to mesh its interior. Then delete the legacy neighbour-slice extraction, the deferral, `CaptureMeshDeps`/revalidate, the 26-neighbour re-mesh on generation and the seam registry's stub jobs. Add a cubic streaming test, and re-run C2 and §12.8's benches against a same-build control. Recheck KC13 | KC2, KC13, KC14 | VoxelWorlds | L | — |
| WM-C2 | **Block-accurate editing.** In cubic the player digs at `ImpactPoint − ½·VoxelSize·normal`, places at `+ ½·VoxelSize·normal` and sets whole voxels, so one click is one block; the demo brush runs with `bUseDiscreteEditing` in cubic | KC5 | VCP, VoxelWorlds | M | — |
| WM-C3 | **Cubic scatter on the CPU extractor.** Cubic skips the GPU extraction branch, and `PlacementMode = BlockFaceSnap` is read | KC4 | VoxelWorlds | S | — |
| WM-C4 | **Delete the GPU cubic mesher** and its tests; the README stops claiming it | KC3 | VoxelWorlds | S | — |

**M6. The island's visible edge and scale.** **Status 2026-10-16: both implemented and verified, PRs open** (results in
§14.10): draquel/VoxelWorlds#95 (WM-E1, WM-E2 and these docs); draquel/VoxelCharacterPlugin#24 (the world map's half of
WM-E2); draquel/UE5_PluginDev#107 (the gitlinks and the doc map).

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-E1 | **A visible world edge.** An ocean plane or ring that continues to the horizon beyond the loaded ring, and a seabed that slopes down from the coast instead of the 1,000 uu cliff at R+F, so the island no longer floats in a void seen from outside | KI2 | VoxelWorlds | M-L | WM-A2, WM-I5 |
| WM-E2 | **Map tile eviction.** Evict tile images by distance or least recent use and regenerate them on demand (the explored keys stay in the save), or keep far tiles at a coarser resolution | KI15 | VoxelWorlds (map) | M | — |

**Not scheduled.** The island nice-to-haves of §6 (whole-island map, distant silhouette, pre-generation, coastline
shaping, a real bowl shape, archipelago, World Partition POI clusters) and the cubic first-class items (KC6, KC7,
KC9, KC12, block highlight, hardness) until D1 changes.

### 14.3 Milestones and order

D9's order is a priority, not a strict sequence. Rough totals assume S ≈ half a day, M ≈ 1.5 days, L ≈ 4 days.

| Milestone | Work items | Rough total | Needs | Done when |
|-----------|------------|-------------|-------|-----------|
| M1 Hygiene | WM-H1 to WM-H10 | ~7 days, mostly in parallel | — | Each item's check; a still viewer's `LODUpdateMs` ≈ 0; headless suites and the in-editor GPU tests green. **Met 2026-10-14, merged 2026-10-15 (§14.5)** |
| M2 World-mode API | WM-A1 to WM-A4 | ~5 days | WM-H6 | Island params come from one builder; `CullCentreProbe` asserts; a save from a different radius is refused and the world starts fresh; the control's bench unchanged within batch noise. **Met 2026-10-15, in review (§14.6)** |
| M3 Island correctness | WM-I1 to WM-I6 | ~6.5 days | M2 | `POIPlacementProbe` asserts; I5-I8 re-run: swimming out stops at R+F, falling out respawns, an off-island spawn stands on land, a radius change starts fresh; the I6 tour finds no marker or explored tile past the bounds. **Met 2026-10-15, in review (§14.7)** |
| M4 Streaming window | WM-S1 to WM-S4 | ~4 days | WM-A3, WM-H1 | WM-S4: island M within §1's thresholds on chord, perimeter and coast; peak loaded chunks within +25% of the control; the D3 budget met on every run, M's v6000 chord included; island audits 0; the control no worse. **Met 2026-10-15 but for one threshold, in review (§14.8):** frame, manager, peak loaded, memory and audits hold on every route and speed, D3 on every run (M's v6000 chord 1,066 MB), the control no worse; catch-up misses at v1500 on the chord and the perimeter (0.27-0.46 s against 0.17-0.18 s) and on the perimeter at v3000 (means 0.47 vs 0.25 s): the island's sea-heavy view leaves a 20-50 chunk generation queue at the stop |
| M5 Cubic terrain-only | WM-C1 to WM-C4 | ~6.5 days | — | Cubic against a same-build control: catch-up within +25% and manager P95 within +15% (§12.8's misses); C2 and C4 pass; the legacy path is deleted. **Met 2026-10-16, in review (§14.9):** catch-up −38% to 0% and manager P95 −21% to −50% of the control at v1500-v6000; C2 and C4 pass (C4 after G15, a renderer fix); the legacy path is deleted |
| M6 Edge and scale | WM-E1, WM-E2 | ~4.5 days | WM-A2, WM-I5 | From past R+F the sea reaches the horizon with no void; a fully explored L keeps its map tiles under a fixed budget. **Met 2026-10-16, in review (§14.10):** from 20,000-50,000 uu past R+F and from 48,000 uu up the island sits in a sea that reaches the horizon, on a seabed that slopes from the coast; a fully explored L holds 37 MB of map images (539 MB before) under an 80 MB budget; with the far ocean island M stays within the control's frame and manager P95 and D3 holds; perimeter catch-up is still missed by 0.3-0.5 s (M4's miss) |
| Spherical | Phases 1-5 (§14.4) | several weeks | M4, M5 | The requirements doc's exit checks |

**What can run in parallel.** M1's items with each other and with M2, except WM-H6 before or with WM-A1 (both touch
the island params). WM-I4 and WM-E2 need nothing and can start any time. M5 works in the chunk manager's legacy path and
the mesher, not in the LOD strategy or the world-mode API, so a separate session can take it alongside M3 and M4.
Sequence these, because they edit the same code: WM-H1 before WM-S1 (`RebuildBalancedLODCache`), WM-A3 before WM-S1 (the
slab sampling), WM-H6 with or before WM-A1.

**Benches.** Every before/after compares against a same-build control in one batch: LOD re-mesh counts vary with frame
rate and machine load (§13.10), and the control itself moved with G6 (§12.8). Report memory against the D3 budget.

### 14.4 Where spherical fits

The requirements doc's phases ([SPHERICAL_PLANET_REQUIREMENTS.md](SPHERICAL_PLANET_REQUIREMENTS.md) §4), placed in
this plan:

| Spherical phase | Placed | Size | Notes |
|-----------------|--------|------|-------|
| 0. Guardrails (SP-A5, SP-B3, SP-I1, doc corrections) | M1, WM-H10 | M | Stops a spherical config from producing the GPU's generic noise by default |
| The API it shares with the island (SP-A1 and SP-A2 heightmap side, SP-A6, SP-G9) | M2, WM-A1/A2/A4 | — | Designed so the planet implementations slot in, as the requirements doc asks (phases 3 and 4 depend on the review). **Done 2026-10-15 (§14.6)**, with a first planet side: shell bounds, radial up and altitude, a gnomonic cube-face coordinate; `GetSurfacePoint` comes with Phase 1 |
| 1. A correct planet on the CPU | After M4 and M5 (D9: after M6) | L | SP-C1 (candidates cover the shell) takes WM-S1's shape: a per-column interval around the surface plus the viewer's neighbourhood, which for a planet is a shell. §13.11's fix already keeps a pole viewer's window at its own layer instead of near layer 40. WM-C1 removes the legacy path before planets test seams at mid latitudes (SP-D4). **Done 2026-10-16 (§15)**: work items WM-P1 to WM-P8 |
| 2. GPU | After Phase 1 | L | As in the requirements doc |
| 3. Walkable | After Phase 1; can run beside Phase 2 (game side) | L | SP-F5 (KillZ) builds on WM-I4's recovery |
| 4. Systems | After Phase 3 | L, several PRs | Uses WM-A2's frame and WM-I2's bounds-aware consumers |
| 5. Places | After Phase 4 | M-L | Builds on WM-I1's world-mode-aware placement |
| 6. Big planets and orbit | Not planned | — | SD1, SD5 |

### 14.5 M1 results (2026-10-14)

All ten M1 items are implemented and verified; merged 2026-10-15. Built from VoxelWorlds `m1/integration` (a merge of the four
code PRs) and UE5_PluginDev `claude/blissful-noether-7c99ae`, editor 5.8.3 from `D:\Program Files\Epic Games\UE_5.8`,
in a worktree editor with background throttling off.

**Suites.** Headless `VoxelWorlds+VoxelWorldSave` 228/228 (219 before M1 plus 9 new tests), `VoxelCharacter` 26/26. In the
editor, where the GPU tests run, `VoxelWorlds+VoxelWorldSave` 228/228, including every `Generation.GPUvsCPU*Parity`,
`GPUNoiseGeneratorAsync`, `Meshing.Cubic.CPUvsGPU` and `Meshing.MarchingCubes.CPUvsGPU`; the island parity test now also
runs the Exponential curve (100% density, material, biome and metadata at LOD 0/1/2).

| ID | Status | PR | Check |
|----|--------|----|-------|
|  | Done, merged 2026-10-15 | VoxelWorlds#84 | Same-build A/B on the control (v3000, 40,000 uu chord, two runs each way, the old behaviour behind a temporary cvar): a still viewer's `LODUpdateMs` 1.03-1.19 ms mean → **0.001 ms**; while moving, P50 1.28-1.29 → 0.73-0.78 ms, P95 unchanged (1.75-1.97 ms, the ticks that do rebuild); LOD re-meshes 280 in all four runs; stale-LOD backlog 0.0-0.5 chunks mean, 0 near; manager P95 13.6-14.6 → 12.6-14.2 ms (noise) |
|  | Done, merged 2026-10-15 | VoxelWorlds#84 | Live, control world: the component's vertices, triangles and GPU MB equal the renderer's own counters at the spawn (494,590 / 862,170 / 23.1 MB), after a teleport about 95,000 uu away (561,124 / 979,708 / 26.2) and after the teleport back (537,168 / 934,740 / 25.0); before, they only grew. Test `Rendering.WorldComponent.StatsReportCurrentMeshes` |
|  | Done, merged 2026-10-15 | VoxelWorlds#86 | `Edit.Composition.SubtractComposesWithSet`: under a Set slab over air, the player's dig takes the aimed voxel 255 → 175 → air and its face neighbours 255 → 234 → 213; two digs remove one slab voxel (it was five); undo restores it. `.SaveRoundTrip`: the edits file reloads the same voxels |
|  | Done, merged 2026-10-15 | VoxelWorlds#85, UE5_PluginDev#102 | The new load warning flagged 21 configs (`DemoSmoothWorldConfig` and every config duplicated from it: leaves 22/23, Birch's trunk 21); all now use Wood 20 / Leaves 21 and load silently. `DemoCubicWorldConfig` was already correct. Test `Configuration.TreeTemplateMaterials` |
|  | Done, merged 2026-10-15 | UE5_PluginDev#102 | `DemoCubicWorldConfig`: `bEnableLOD` off and GPU scatter extraction off; greedy, `DemoCubicScatterConfig`, tree ids and the demo's biomes/caves/noise were already right (water stays at −1,510, under a block top). `Config/CubicScatterConfig`, referenced by nothing, deleted |
|  | Done, merged 2026-10-15 | VoxelWorlds#85 | `HeightProbe` asserts: the generated surface drops 1,000-1,001 uu across R+F with Smooth and 1,000-1,006 with Exponential (was 1,016-1,307); `GetDensityAt` 500 uu under the floor beyond R+F = +500 (was −1,000, air). GPU island parity with Exponential 100%. Tests `Generation.IslandBowl.FalloffReachesEdge`, `.DensityMatchesGenerationBeyondEdge` |
|  | Done, merged 2026-10-15 | VoxelCharacterPlugin#21 | Live, cubic demo world: six digs at one Wood voxel pay 1 Wood (was 6). Test `VoxelCharacter.Gather.OncePerVoxel`. The payout no longer depends on the aimed voxel, which `floor(ImpactPoint)` sometimes puts in the air (KC5) |
|  | Done, merged 2026-10-15 | VoxelWorlds#84, VoxelCharacterPlugin#21 | Island S, `follow=300` from the centre out over the sea at v1500 with the player's drowning on (no CDO hack): the pawn spent 71 s under water, down to 2,700 uu deep, and did not die; back at its start, 110,000 uu from the end of the run, it was held until the ground was ready (1.1 s) and released walking with damage restored. Every run of the batch logged its restore (0.1-1.1 s). Cubic (`DemoCubicWorldConfig`) v3000: thrash 1,465, all neighbour re-meshes, `other` 0, below the 1,747 mesh jobs launched; the same route counted 1,577-1,671 neighbour plus 1,596-2,006 deferral re-queues as "other" before (§12.8) |
|  | Done, merged 2026-10-15 | VoxelWorlds#88 | §11's non-spherical items fixed (WORLD_MODES.md island sections and POI claim, CAVE_SYSTEM, SCATTER_SYSTEM, README, TRAVERSAL_LIMITS); `Research/PERFORMANCE_TARGETS.md` has a current baseline from the control; `Features/WORLD_MODES.md`'s island sections carry §13's numbers and the known gaps |
|  | Done, merged 2026-10-15 | VoxelWorlds#87, UE5_PluginDev#102 | Phase 0's exit check: `PlanetTest` (a 2.5 km planet from the demo config) generates on the CPU with the SP-B3 warning, the config raises no warnings, and the terrain stays inside its shell; the pawn spawns on the north pole (z 250,090). Test `Configuration.PlanetSettings` (the default spherical settings get the depth warning; water on a planet is flagged; `ShouldUseGPUGeneration` is false for planets). Doc corrections per §11 |

JSON reports: [Research/Bench/M1/](Research/Bench/M1/) (`m1h1_*` the WM-H1 A/B, `m1h8_*` the WM-H8 runs).

**Left for later milestones**, as planned: KI4's four island param builders (WM-A1), the edge, recovery and boundary
(M3), slab culling and 3D LOD distance for islands (M4), cubic's legacy path (M5). M1 changed one shared behaviour on
purpose: the bench's `thrashRemeshCount` now counts executed meshes, so it is lower than before on any path that defers
(cubic); compare it only with runs from this build on.

### 14.6 M2 results (2026-10-15)

All four M2 items are implemented and verified; PRs open. Built from VoxelWorlds `feature/wm-a2-a4-world-mode-api`
(stacked on `feature/wm-a1-params-builder`, base `d2b3e6e`) and UE5_PluginDev `claude/cool-gates-ffddfc` (base
`cdd9059`), editor 5.8.3 from `D:\Program Files\Epic Games\UE_5.8`, in a worktree editor with background throttling off.

**Suites.** Headless `VoxelWorlds+VoxelWorldSave` 234/234 (228 before M2 plus 6 new; WM-A1 alone 228/228). In the
editor, where the GPU tests run, 234/234, including every `Generation.GPUvsCPU*Parity` (IslandBowl included),
`GPUNoiseGeneratorAsync`, `Meshing.Cubic.CPUvsGPU`, `Meshing.MarchingCubes.CPUvsGPU` and
`WorldModes.InfinitePlane.GPUConsistency`: WM-A1 changed what the generation request carries, and the island parity is
unchanged.

| ID | Status | PR | Check |
|----|--------|----|-------|
| WM-A1 | Done, in review | VoxelWorlds#89 | `FVoxelWorldModeFactory` (VoxelGeneration) builds the island and planet params, the analytic world mode and the generation request's world-mode fields; the request carries `FIslandBowlParams` / `FSphericalPlanetParams` (the copies `FIslandModeParams` / `FSphericalPlanetModeParams` are gone). Users: generation (CPU and GPU), the chunk manager (both sites), the LOD island and shell culls, the map, the field viewer, POI placement and the probe tests. The island centre is `WorldOrigin` + offset everywhere. `CullCentreProbe` asserts: with the demo's origin 0 land columns are culled (was 1,098) and generation centres the island at (0, 0); likewise for an island 120,000 uu off the origin. Live, `Review_Island_M_DemoOrigin`: the cull logs `Center=(0, 0)`, `GetWorldBounds` agrees, the south coast slopes into the sea where review B saw a straight cut through a hill (`m2_ki3_coast_north`), audit 0. Of the project's configs only `Review_Island_M_DemoOrigin` has an island off the origin, so it is the only content whose terrain moves |
| WM-A2 | Done, in review | VoxelWorlds#90 | `IVoxelWorldMode::IsInsideWorld`, `GetWorldBounds` (VoxelCore `FVoxelWorldBounds`: Unbounded, Circle, Rectangle, Shell), `GetUpVector`, `GetAltitude` (above `SeaLevel`; above `PlanetRadius`), `WorldToSurface` / `SurfaceToWorld` (`FVoxelSurfaceCoord`). Defaults describe an unbounded heightmap world, so the dungeon plugin's world mode and test mocks build unchanged. The island's height bounds include the floor beyond the edge. The planet side is implemented (shell, radial frame, a gnomonic cube mapping that round-trips on all six faces) but unused until Phase 1. Blueprint: `UVoxelChunkManager` `IsInsideWorld`, `GetWorldBounds`, `GetUpVectorAt`, `GetAltitudeAt`, `WorldToSurface`, `SurfaceToWorld`, `GetWorldFingerprint` (`Voxel|World`); called live through Python on the island: inside at 0.99 (R+F), outside at 1.01 (R+F), round trip exact. Tests `WorldModes.Api.Bounds`, `.Frame`, `.FactoryParams` |
| WM-A3 | Done, in review | VoxelWorlds#90 (LOD), UE5_PluginDev#103 (VoxelWorldPOI) | The LOD strategy builds its world mode with the factory; the far-band slab and the terrain height bounds sample it (the slab used InfinitePlane's static math without continentalness, off the generated surface by up to 2,481 uu in `LOD.SlabUsesWorldModeHeight`). The slab stays InfinitePlane-only and the vertical window is unchanged (WM-S1). POI placement and the conditioner sample the world's own world mode (`FPOIPlacementConfig::TerrainWorldMode`). `POIPlacementProbe` with the project catalog: S 1 POI (inside R), M 4 (3 inside, 1 on land in the band), L 22 (21 inside, 1 on land in the band); none past R+F, no land POI in the sea (asserted). Before: M placed 12, 8 of the 9 past R in the sea or over the floor, on pads raised 1,900-5,900 uu |
| WM-A4 | Done, in review | VoxelWorlds#90 (fingerprint), UE5_PluginDev#103 (VoxelWorldSave) | `FVoxelWorldFingerprint` (world mode, shape params, voxel grid, `FVoxelWorldModeFactory::GenerationVersion` = 1), recorded in `FVoxelWorldSaveDocument::WorldFingerprint`. `LoadWorld` refuses a save from a different world, logs what differs, keeps the file, starts fresh, toasts, and keeps auto-save off until an explicit save; a save from before fingerprints loads. Live, review B's I8 again on `Review_Island_S`: saved (216 explored tiles, the player), radius 30,000 → 20,000 in memory, Play: `LoadWorld ... belongs to a different world (IslandRadius=30000.0 vs 20000.0): NOT loaded`, a fresh world, the file's hash unchanged at Play and after Stop (tear-down auto-save skipped); radius back, Play: the save loads (216 tiles restored). Tests `VoxelWorldSave.Document.WorldFingerprint`, `WorldModes.Api.Fingerprint` |

**The control's bench (WM-A3 changes its slab).** Same build, one batch, the old static slab height behind a temporary
cvar (removed before the PR), `DemoSmoothWorldConfig` at v3000 over 40,000 uu from (0, 0), two cold runs each way:

| Run | Frame P50 / P95 ms | Manager P95 ms | Catch-up s | LOD re-meshes | Loaded mean / peak | Layer −3 mean | Peak voxel systems MB | Audit |
|-----|--------------------|----------------|------------|---------------|--------------------|---------------|-----------------------|-------|
| Old slab, a | 20.5 / 31.7 | 10.2 | 0.55 | 280 | 1,124 / 1,157 | 109 | 1,007 | 0 |
| Old slab, b | 22.0 / 33.4 | 11.2 | 0.63 | 279 | 1,119 / 1,157 | 109 | 944 | 0 |
| World-mode slab, a | 22.1 / 32.8 | 10.9 | 0.43 | 281 | 1,072 / 1,106 | 65 | 1,014 | 0 |
| World-mode slab, b | 19.5 / 30.3 | 9.5 | 0.40 | 281 | 1,077 / 1,110 | 65 | 979 | 0 |

Frame and manager time and the LOD re-meshes are unchanged within the batch's noise. The slab, now on the generated
surface, keeps about 4% fewer chunks (44 fewer in layer −3, the deep layer the old height kept where continentalness
lifts the ground), and catch-up is a little shorter. A survey of the far band from (−470,286, −300,332, 2,500) shows no
holes. JSON reports: `Saved/VoxelBench/*_m2a3_*` in the M2 worktree (not committed).

**Left for later milestones**, as planned: consumers of the bounds (WM-I1 placement, WM-I2 map, markers, PCG and
navigation, WM-I3 spawn, WM-I5 boundary), the island slab and 3D LOD distance (M4), `GetSurfacePoint` and every planet
consumer (spherical Phase 1). Two things to know: a save written from now on carries a fingerprint, so bumping
`FVoxelWorldModeFactory::GenerationVersion` invalidates every save made before it (on purpose); and terrain tuning
(noise, height scale, biomes) is not in the fingerprint, so a save still loads after a tuning change.

### 14.7 M3 results (2026-10-15)

All six M3 items are implemented and verified; PRs open. Built from VoxelWorlds `feature/wm-i5-world-boundary` (stacked on
`feature/wm-i2-bounds-consumers`, base `e0e6e39`), VoxelCharacterPlugin `feature/wm-i3-i5-spawn-recovery-boundary` (base
`39d7071`) and UE5_PluginDev `claude/m3-island-correctness-49311a` (base `127959f`), editor 5.8.3 from
`D:\Program Files\Epic Games\UE_5.8`, in a worktree editor with background throttling off. Live checks ran on `VoxelDemo`
with the review configs through `voxel.ConfigOverride`, from a fresh world unless a save was the point (I8, the terrain
wait); shape changes (water off, an island moved off the world origin, a smaller radius) were made in memory and
reverted, never saved. Screenshots: `Saved/Screenshots/Claudius/m3_*` in the M3 worktree (not committed).

**Suites.** Headless `VoxelWorlds+VoxelWorldSave+VoxelCharacter` 268/268 (260 before M3 plus 8 new). In the editor, where the GPU tests run, 268/268, every `Generation.GPUvsCPU*Parity` (IslandBowl included), `GPUNoiseGeneratorAsync`, `Meshing.Cubic.CPUvsGPU`, `Meshing.MarchingCubes.CPUvsGPU` and `WorldModes.InfinitePlane.GPUConsistency` among them; M3 does not change generation or meshing.

| ID | Status | PR | Check |
|----|--------|----|-------|
| WM-I1 | Done, in review | UE5_PluginDev#104 | `FPOIPlacement::GeneratePOIs` places only sites wholly inside the world mode's bounds, falloff ring included (`FVoxelWorldBounds::ContainsCircleXY`, counted as `RejectedOutsideWorld`); a cell whose centre is outside places nothing. `POIPlacementProbe` asserts on S, M and L: no POI past R+F, no site whose falloff ring crosses it, no land POI under water. The catalog's types all require land, so the probe places a second time with every type allowed anywhere: S 1 POI, M 18 (9 centred in the sea), L 60 (27 in the sea), none reaching past R+F. `POI.Placement.WorldBounds` (no content): with one place-anywhere type over an island of R+F 250,000, unbounded placement puts 401 of 568 sites outside; bounded, 167 sites, 59 in the sea, none outside (circle and rectangle). Live: island S's registration survey places 1 POI within 5 km (review B: 32); M's four (three inside R, the Tower in the falloff band) all stand on land, and the band Tower sits on the natural coast (`m3_I6_M_tower_band`) |
| WM-I2 | Done, in review | VoxelWorlds#91 (map, PCG, navigation), VoxelCharacterPlugin#22 (world map), UE5_PluginDev#104 (markers) | `FVoxelWorldBounds` answers the footprint geometry (`GetDistanceToEdgeXY`, `ContainsCircleXY`, `IntersectsBox2D`, `ClampXY`, `GetOutwardNormalXY`). Map: only tiles that overlap the bounds are explored, from the minimap and from generated chunks; explored tiles from an older save that lie outside are dropped; `voxel.Map.Stats` counts tiles outside. The world map clamps its pan to the bounds' box. Markers: the POI marker source drops anything outside. PCG: the surface sampler skips points outside. Navigation: every cell outside is blocked, water on or off. Live, island M from a fresh world: a bench from 150,000 to 300,000 uu (50,000 past R+F) at v6000, then the four POIs: 635, then 1,408 explored tiles, 0 outside (review B's tour: 124 of 1,497 past R+F); 4 markers within 400 km, all inside (review B: 20, 12 past R+F); `vox.NavPath` to a point past R+F: NO PATH. Island S with water off: a goal on the floor past the edge gives NO PATH, a goal 150 uu past it snaps inside, an inland goal is found. VoxelDemo has no PCG components, so the sampler is checked by code only (as in review B); the pan clamp is checked by code, not driven live. Tests `WorldModes.Api.BoundsGeometry`, `Navigation.WorldBounds` |
| WM-I3 | Done, in review | VoxelCharacterPlugin#22 | The spawn search takes only points inside the bounds; a start outside searches from a chunk inside the edge; the water level now includes `WorldOrigin.Z`. Live, review B's I7 again on S with the island moved 70,000 uu from the world origin, so the spawn at the origin is 30,000 uu past R+F: water off, the search starts at (34,800, 0) and the pawn stands walking at (34,829, −3) (review B: it took the floor at (0, 0) for land and froze); water on, in four directions, the start a chunk inside the edge is in the sea three times, the spiral finds land one ring away, and the pawn stands every time |
| WM-I4 | Done, in review | VoxelCharacterPlugin#22 | `AVCCharacterBase::FellOutOfWorld` puts the body back on the last ground it stood on inside the world (`GetLastSafeLocation`, sampled four times a second on the server), kills it through the combat pipeline and lets the game mode respawn it; a corpse or an unpossessed body keeps the engine default (destroy). A player 20,000 uu under the world's lowest terrain (`OutOfWorldDepth`) counts as fallen out without the minutes-long fall to KillZ. A terrain wait that times out with nothing under the pawn moves it to the respawn point (rest point, player start, the world's centre) and waits there, at most twice. Live on S: walls off, flown past the floor ring and dropped at 48,800 uu: "out of the world" at −32,000, the body back at its last safe ground (36,696, 0), death, respawn 5 s later standing on land at (31,883, 4,799) (review B: KillZ destroyed the pawn about 260 s later and nothing respawned it). KillZ itself, with the early check off on the pawn and a teleport to z −1,100,000: the same path, respawn standing. The terrain wait: a save whose player stands 30,000 uu under S's terrain, loaded mid-session (`vox.Load`, an exact-location resume): after the 60 s timeout "no terrain under (5000, 5000, -30000); moving to the respawn point (0, 0, 0)" (no rest point was set), a new wait there, and the pawn stands walking. Test `VoxelCharacter.WorldEdge.UnpossessedFallsOut` |
| WM-I5 | Done, in review | VoxelWorlds#92 (boundary), VoxelCharacterPlugin#22 (push-back, HUD hint) | Hard boundary: invisible walls on the bounds (`FVoxelWorldBoundary`, `UVoxelChunkManager::RebuildWorldBoundary`): an inscribed polygon with its chords within 50 uu of R+F (63 walls for S, 158 for M, 223 for L; 55 after S's radius went to 20,000), four walls for a rectangle; they block pawns, NPCs and physics bodies and let sight, interaction and camera traces through. `bWorldBoundary` (default on) and `voxel.World.Boundary 0|1`. Soft push-back: `UVCMovementComponent` caps the outward speed in the last 4,000 uu before the edge, from the full speed to drifting back at 250 uu/s at the edge, inside `CalcVelocity` (client prediction and server agree); flying is exempt. HUD: "A strong current pushes you back: this is the edge of the world" while in the band. Live on S, water on: walking east from (34,000, 0) into the sea and swimming out at full speed, the pawn stalls at 38,358 uu, 1,642 uu inside R+F (the formula gives 1,639), with the hint on (`m3_I5_S_pushback_hint_ui`); in debug flight, which the current ignores, it stops on the wall at 39,966 uu (R+F less the capsule radius). Water off, walking out over the falloff slope, it stalls 1,185 uu inside (formula 1,176). Tests `WorldModes.Boundary.CircleWalls`, `.OtherShapes`, `VoxelCharacter.WorldEdge.PushBack`, `.VitalsHint` |
| WM-I6 | Done, in review | UE5_PluginDev#104 | The probe assertions above. `Content/PluginTesting/WorldPartition/WP_IslandBowlConfig` deleted: no map, script, config or external actor references it (`WP_VoxelCoexistTest` binds `DemoWorldConfig`) |

**Scenarios I5-I8 again** (§13.1 has review B's results):

| ID | Result | What happened |
|----|--------|---------------|
| I5 | **Pass** | Water on: the swimmer stalls 1,642 uu before R+F with the hint on, and nothing passes the wall; with the walls' collision off, falling past the floor ring respawns the player on land within 10 s. Water off: the walker stalls 1,185 uu before the edge |
| I6 | **Pass** (markers, map, POIs, navigation) | M: no marker or explored tile past the bounds after a run 50,000 uu past R+F; every POI on land; NO PATH to the sea past R+F; `vox.NPCs` normal. Not repeated: the dungeon visit, the day/night cycle and scatter, which review B passed and M3 does not touch |
| I7 | **Pass** | Off-island spawns stand on land with water off and on, in four directions |
| I8 | **Pass** | S saved, `IslandRadius` 30,000 → 20,000 in memory, Play: `NOT loaded` (`IslandRadius=30000.0 vs 20000.0`), a fresh world, and the walls rebuilt on the new edge. Moving the island centre was refused the same way |

**Left for later milestones**, as planned: the streaming window (M4), the visible edge (WM-E1: from outside the loaded ring
the island still floats in the void, and the walls are invisible) and map tile eviction (WM-E2). Things to know:

- The walls exist in every bounded world unless `bWorldBoundary` is off; the streaming benchmark flies through them (it
  teleports the pawn each frame), and debug flight stops at them.
- Placement on an island changed: a site whose falloff ring crossed R+F is no longer placed. Only the review islands are
  affected; an island save from before M3 with such a POI keeps that POI's edits in its edit layer. The generation
  version (§14.6) was not bumped: InfinitePlane worlds are unchanged.
- The HUD hint lives in VoxelCharacterPlugin's vitals widget, where every other HUD line is; §14.2 had put the HUD in the
  parent repo.

### 14.8 M4 results (2026-10-15)

All four M4 items are implemented and verified; PRs open. Built from VoxelWorlds `feature/wm-s1-s3-streaming-window`
(base `a029006`) and UE5_PluginDev `claude/m4-streaming-window-e34c9f` (base `eb178c0`), editor 5.8.3 from
`D:\Program Files\Epic Games\UE_5.8`, in a worktree editor with background throttling off.

**The window, as built (WM-S1, D8).** Heightmap worlds (InfinitePlane and IslandBowl) take their vertical range from
the terrain. Each chunk column has its own window (`FDistanceBandLODStrategy::GetColumnWindow`), clipped to the world
mode's height bounds plus a chunk:

| Part | Layers | Where |
|------|--------|-------|
| Surface slab | The world mode's surface over the column's footprint (`IVoxelWorldMode::GetSurfaceRange`, new: five height samples in heightmap modes, the height bounds otherwise) ± 1.5 chunks, extended up to the water plane | Every column in range, at every distance, islands included |
| Near window | The viewer's own layer ± 2 | Columns within band 0 + two chunk widths, measured horizontally (16,600 uu on the demo) |
| Keep window (unloading) | The slab; in a near column, the whole height range | So a viewer changing layer unloads nothing near it; the extra layers go when their column leaves the near radius |

The viewer-relative −2 to +8 (island +4) window is gone. The planet keeps its viewer-relative window, now one branch of
the same per-column function, until spherical Phase 1 gives it a shell around the surface (SP-C1). The balanced-LOD cache
covers the keep window, so every chunk the strategy keeps has a balanced LOD; with the smaller volume its rebuild got
cheaper (below).

**Two distances (WM-S2 and D8).** LOD levels use 3D distance in every world mode; islands used 2D (KI13). The load and
unload horizons are measured horizontally in heightmap worlds (`IVoxelLODStrategy::GetHorizonDistance`, new; 3D on a
planet). §14.2 put WM-S2 as "3D like InfinitePlane", but with a 3D horizon a viewer 40,000 uu above the terrain loads
nothing at all (the old control at 48,000 uu: everything unloads), which D8 rules out: the viewer's altitude never
removes the ground. So both heightmap modes now share a cylinder for the horizon and a sphere for the LOD bands.
Everything in the chunk manager that asks whether a chunk is in range uses the strategy's horizon distance: the
generation queue's eviction, the stale-chunk cull, edit pins and their unload veto, and `AuditResidency`. The audit
cannot disagree with the unload rule any more (KI11).

**G14, found on the way: the first PIE of an editor session.** It re-meshes far more for LOD than every later run on
the same route: on the control's 40,000 uu chord from (0, 0) at v3000, 533 LOD re-meshes in the first PIE of two editor
sessions against 287-304 in every later run, old window or new (frame and manager time look normal). An early smoke run
blamed it on a keep margin the first build had (the near window's extra layers kept one chunk past the near radius); the
final build, without that margin, gave 533 again in its first PIE and 287 in the next two. Not investigated further. Every
batch in this section ran after several PIE sessions in the same editor; start any bench batch with a throwaway PIE.

**WM-S3.** `MaxLoadedChunks` is removed from `UVoxelWorldConfiguration` (it was read nowhere; the test actor set it).
What bounds residency is now written down in `Features/LOD_SYSTEM.md` ("What Bounds Residency") and on the config: the
horizon, each column's window, the world's footprint, edit pins, and the pipeline (chunks mid-job finish first).

**Suites.** Headless `VoxelWorlds+VoxelWorldSave+VoxelCharacter` 271/271 (268 before M4, plus four new
`LOD.VerticalWindow.*` tests, less `.FollowsViewerLayer`, which tested the old window). In the editor, where the GPU tests
run, 271/271, every `Generation.GPUvsCPU*Parity` (IslandBowl included), `GPUNoiseGeneratorAsync`, `Meshing.Cubic.CPUvsGPU`,
`Meshing.MarchingCubes.CPUvsGPU` and `WorldModes.InfinitePlane.GPUConsistency` among them. Both on the final build, with
the temporary cvar removed; M4 does not change generation or meshing.

| ID | Status | PR | Check |
|----|--------|----|-------|
| WM-S1 | Done, in review | VoxelWorlds#93 | The window above; tests `LOD.VerticalWindow.FromTerrain` (the candidates are exactly each column's slab, plus the viewer's layer ± 2 near it, for a viewer on layers −3 to 10, both heightmap modes; far columns do not depend on the viewer's altitude) and `.KeepWindow` (a viewer one layer up, one down or 48,000 uu up unloads nothing; 20,000 uu away the extra layers go and nothing else does). Live, the WM-S1 checks: **a dungeon room below the slab** (the control's tiled dungeon, boss room floor at z −5,343, layer −2): the pawn lands walking and the room renders (`m4_S1_dungeon_boss3`); **caves below the slab near the viewer**: a survey camera 12,500 uu underground (layer −3) holds layers −4 to 1 in the columns within 16,600 uu and the slab, −2 to 1, beyond; **edited chunks** are pinned by the chunk manager within the view distance, as before; **the island's floor ring**: S's floor and water stream out to the island cull's edge (`m4_S1_island_floor_ring`; the visible edge stays WM-E1); **a flight at z 7,200** and **the I2 survey from 48,000 uu**: the tables below |
| WM-S2 | Done, in review | VoxelWorlds#93 | LOD distance is 3D in every mode; the island boundary cull stays 2D; the horizon is horizontal in heightmap worlds and the chunk manager asks the strategy for it. Island audits are 0 in all 32 island runs, old window and new (review B: KI11 findings in most island runs, 3-64 each). Settled at S's spawn the island keeps 105 chunks uncompressed, the control 106 (review B: 224 against 113, KI13). Tests `LOD.VerticalWindow.IslandLODDistance3D` (chunks one, two and three layers under the viewer get LOD 0, 1 and 2; a viewer 48,000 uu over an island S streams every island column, all at the coarsest band) and `.HorizonDistance` |
| WM-S3 | Done, in review | VoxelWorlds#93 | `UVoxelWorldConfiguration::MaxLoadedChunks` removed (the test actor's assignment with it); nothing else referenced it. `Features/LOD_SYSTEM.md` "What Bounds Residency" |
| WM-S4 | Done, in review | VoxelWorlds#93 (docs) | Below. Every threshold holds on every route and speed except catch-up at v1500 (and on the perimeter at v3000), by 0.2-0.3 s |

**Bench method.** One build, one batch (VoxelWorlds at this PR's code plus a temporary `voxel.Temp.LegacyWindow` cvar that
restored the old window and distances, latched when the world initializes, removed before the PR; the near radius's
final form went into the running editor with Live Coding before the batch). For every route and speed, four cold fresh worlds back to
back: control (old window), control, island M, island M (old window). PIE in the worktree editor's level viewport
(1280×722), background throttle off, `vox.DeleteSave`, clock pinned at noon, `voxel.Bench.Run <tag> <v> <dist> warmup=120
<route>`, Stop. Routes as in §13.3, all with `follow=300`: chord `start=-250000:0 dir=1:0` (250,000 uu to the centre at
v1500, 500,000 coast to coast at v3000 and v6000), perimeter `orbit=0:0:200000` (150,000 uu along the coast at R), coast
crossing `start=150000:0 dir=1:0` (150,000 uu, ending 50,000 uu past R + F). The control is `DemoSmoothWorldConfig`
through the same override, as in §12-§13; the island is `Review_Island_M`. The legacy shim reproduces the old window,
slab gate and distances exactly in the strategy; the chunk manager's range checks follow the strategy's horizon in both
arms, so legacy island runs report audit 0 where review B saw KI11. JSON reports in
[Research/Bench/M4/](Research/Bench/M4/).

**Island M against the control** (this build; review B's numbers are in §13.3):

| Route | Run | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | GenQ mean / peak | Peak mesh queue | Loaded mean / peak | Generated / meshed | LOD re-meshes | Peak voxel MB | Systems MB | Audit | JSON |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Chord v1500 | Control | 16.7 / 20.3 / 26.0 | 7.0 | 0.18 | 6 / 80 | 10 | 1,106 / 1,195 | 4,202 / 7,603 | 3,404 | 894 | 1,145 | 0 | `20261015_142602_m4_C_new_chord_v1500` |
| Chord v1500 | **Island M** | 16.7 / 19.9 / 25.6 | 7.0 | 0.38 | 5 / 81 | 10 | 1,051 / 1,170 | 3,914 / 7,229 | 3,317 | 713 | 918 | 0 | `20261015_142911_m4_M_new_chord_v1500` |
| Chord v1500 | Control, old window | 16.7 / 20.7 / 26.5 | 7.2 | 0.17 | 8 / 90 | 9 | 1,133 / 1,205 | 4,712 / 8,214 | 3,502 | 852 | 1,104 | 0 | `20261015_142248_m4_C_old_chord_v1500` |
| Chord v1500 | Island M, old window | 16.7 / 23.6 / 34.5 | 9.8 | 1.11 | 14 / 140 | 22 | 1,598 / 1,750 | 5,718 / 11,458 | 5,768 | 926 | 1,135 | 0 | `20261015_143224_m4_M_old_chord_v1500` |
| Chord v3000 | Control | 17.5 / 28.4 / 34.6 | 8.8 | 0.59 | 27 / 107 | 10 | 1,081 / 1,153 | 8,467 / 15,419 | 6,952 | 863 | 1,117 | 0 | `20261015_140243_m4_C_new_chord_v3000` |
| Chord v3000 | **Island M** | 16.7 / 27.4 / 33.2 | 8.7 | 0.17 | 19 / 97 | 12 | 1,037 / 1,147 | 7,573 / 14,329 | 6,756 | 742 | 946 | 0 | `20261015_140554_m4_M_new_chord_v3000` |
| Chord v3000 | Control, old window | 19.1 / 30.2 / 36.2 | 9.7 | 0.61 | 45 / 190 | 9 | 1,086 / 1,151 | 9,294 / 16,400 | 7,117 | 854 | 1,106 | 0 | `20261015_135930_m4_C_old_chord_v3000` |
| Chord v3000 | Island M, old window | 19.5 / 36.3 / 49.7 | 12.0 | 0.17 | 302 / 605 | 38 | 1,326 / 1,644 | 9,848 / 21,344 | 11,522 | 934 | 1,150 | 0 | `20261015_140905_m4_M_old_chord_v3000` |
| Chord v6000 | Control | 24.5 / 36.3 / 42.3 | 13.9 | 5.39 | 632 / 794 | 12 | 500 / 1,129 | 4,296 / 11,182 | 6,889 | 858 | 1,364 | 0 | `20261015_145319_m4_C_new_chord_v6000` |
| Chord v6000 | **Island M** | 22.9 / 34.0 / 40.4 | 12.7 | 1.68 | 542 / 758 | 12 | 530 / 965 | 4,592 / 11,326 | 6,734 | 710 | 1,066 | 0 | `20261015_145507_m4_M_new_chord_v6000` |
| Chord v6000 | Control, old window | 24.0 / 34.9 / 40.0 | 13.8 | 5.93 | 723 / 861 | 13 | 468 / 1,115 | 4,374 / 11,347 | 6,977 | 873 | 1,323 | 0 | `20261015_145123_m4_C_old_chord_v6000` |
| Chord v6000 | Island M, old window | 24.2 / 39.7 / 51.8 | 14.7 | 4.22 | 1,112 / 1,336 | 22 | 543 / 1,165 | 4,267 / 12,443 | 8,176 | 970 | 1,250 | 0 | `20261015_145702_m4_M_old_chord_v6000` |
| Orbit v1500 | Control | 16.7 / 20.2 / 25.5 | 6.7 | 0.17 | 5 / 93 | 10 | 1,089 / 1,167 | 2,393 / 4,472 | 2,079 | 838 | 1,075 | 0 | `20261015_143639_m4_C_new_orbit_v1500` |
| Orbit v1500 | **Island M** | 16.7 / 20.2 / 24.9 | 6.9 | 0.46 | 6 / 97 | 10 | 1,127 / 1,213 | 2,475 / 4,540 | 2,067 | 891 | 1,127 | 0 | `20261015_143849_m4_M_new_orbit_v1500` |
| Orbit v1500 | Control, old window | 16.7 / 21.0 / 26.9 | 7.1 | 0.17 | 8 / 102 | 10 | 1,121 / 1,175 | 2,697 / 4,805 | 2,108 | 859 | 1,095 | 0 | `20261015_143429_m4_C_old_orbit_v1500` |
| Orbit v1500 | Island M, old window | 16.7 / 23.9 / 30.8 | 9.0 | 0.17 | 14 / 139 | 16 | 1,696 / 1,784 | 3,611 / 7,348 | 3,737 | 1,017 | 1,250 | 0 | `20261015_144101_m4_M_old_orbit_v1500` |
| Orbit v3000 | Control | 17.3 / 27.2 / 32.7 | 8.6 | 0.17 | 22 / 103 | 10 | 1,069 / 1,138 | 2,393 / 4,472 | 2,082 | 827 | 1,065 | 0 | `20261015_141138_m4_C_new_orbit_v3000` |
| Orbit v3000 | **Island M** | 16.7 / 27.9 / 34.6 | 9.6 | 0.48 | 26 / 101 | 10 | 1,106 / 1,165 | 2,504 / 4,603 | 2,100 | 893 | 1,118 | 0 | `20261015_141255_m4_M_new_orbit_v3000` |
| Orbit v3000 | Control, old window | 19.5 / 31.7 / 38.5 | 10.1 | 0.55 | 41 / 133 | 10 | 1,086 / 1,131 | 2,660 / 4,793 | 2,134 | 823 | 1,048 | 0 | `20261015_141022_m4_C_old_orbit_v3000` |
| Orbit v3000 | Island M, old window | 19.0 / 32.9 / 40.8 | 11.9 | 2.26 | 298 / 417 | 16 | 1,394 / 1,722 | 3,112 / 6,804 | 3,699 | 980 | 1,208 | 0 | `20261015_141417_m4_M_old_orbit_v3000` |
| Orbit v6000 | Control | 23.9 / 33.7 / 39.3 | 12.9 | 4.01 | 438 / 675 | 10 | 663 / 1,081 | 1,312 / 3,390 | 2,081 | 860 | 1,187 | 0 | `20261015_145855_m4_C_new_orbit_v6000` |
| Orbit v6000 | **Island M** | 23.7 / 33.5 / 39.9 | 13.0 | 4.48 | 456 / 699 | 10 | 683 / 1,166 | 1,308 / 3,374 | 2,066 | 878 | 1,214 | 0 | `20261015_145953_m4_M_new_orbit_v6000` |
| Orbit v6000 | Control, old window | 24.3 / 33.4 / 39.7 | 13.3 | 5.38 | 551 / 805 | 10 | 604 / 1,128 | 1,303 / 3,405 | 2,106 | 844 | 1,157 | 0 | `20261015_145759_m4_C_old_orbit_v6000` |
| Orbit v6000 | Island M, old window | 26.8 / 43.0 / 51.5 | 16.6 | 9.03 | 950 / 1,338 | 67 | 764 / 1,722 | 1,135 / 4,161 | 3,050 | 1,034 | 1,364 | 0 | `20261015_150059_m4_M_old_orbit_v6000` |
| Coast v1500 | Control | 16.7 / 23.6 / 30.2 | 7.5 | 0.17 | 7 / 82 | 10 | 1,115 / 1,219 | 2,607 / 4,696 | 2,092 | 892 | 1,141 | 0 | `20261015_144513_m4_C_new_coast_v1500` |
| Coast v1500 | **Island M** | 16.7 / 19.2 / 26.2 | 5.5 | 0.17 | 3 / 78 | 10 | 809 / 1,151 | 1,236 / 2,664 | 1,428 | 896 | 1,125 | 0 | `20261015_144718_m4_M_new_coast_v1500` |
| Coast v1500 | Control, old window | 16.7 / 24.1 / 30.5 | 7.6 | 0.19 | 10 / 125 | 10 | 1,118 / 1,214 | 2,785 / 4,903 | 2,120 | 917 | 1,166 | 0 | `20261015_144308_m4_C_old_coast_v1500` |
| Coast v1500 | Island M, old window | 16.7 / 22.3 / 31.9 | 8.2 | 0.17 | 8 / 172 | 22 | 1,197 / 1,736 | 1,680 / 4,096 | 2,416 | 1,012 | 1,243 | 0 | `20261015_144926_m4_M_old_coast_v1500` |
| Coast v3000 | Control | 19.2 / 32.2 / 37.5 | 10.7 | 0.19 | 37 / 123 | 10 | 1,083 / 1,134 | 2,606 / 4,698 | 2,092 | 897 | 1,146 | 0 | `20261015_141652_m4_C_new_coast_v3000` |
| Coast v3000 | **Island M** | 16.7 / 26.1 / 32.3 | 8.3 | 0.17 | 12 / 98 | 12 | 780 / 1,120 | 1,236 / 2,663 | 1,427 | 834 | 1,063 | 0 | `20261015_141810_m4_M_new_coast_v3000` |
| Coast v3000 | Control, old window | 19.2 / 32.1 / 37.4 | 10.6 | 0.33 | 49 / 185 | 10 | 1,076 / 1,149 | 2,769 / 4,886 | 2,118 | 909 | 1,159 | 0 | `20261015_141535_m4_C_old_coast_v3000` |
| Coast v3000 | Island M, old window | 16.7 / 35.0 / 48.5 | 12.3 | 0.17 | 119 / 461 | 18 | 1,013 / 1,612 | 1,606 / 4,008 | 2,416 | 1,014 | 1,246 | 0 | `20261015_141931_m4_M_old_coast_v3000` |
| Coast v6000 | Control | 26.4 / 41.8 / 49.8 | 16.9 | 5.59 | 585 / 838 | 18 | 553 / 1,120 | 1,156 / 3,235 | 2,086 | 853 | 1,252 | 0 | `20261015_150254_m4_C_new_coast_v6000` |
| Coast v6000 | **Island M** | 16.7 / 36.9 / 44.8 | 14.4 | 0.17 | 223 / 642 | 20 | 523 / 1,010 | 1,009 / 2,427 | 1,425 | 872 | 1,101 | 0 | `20261015_150346_m4_M_new_coast_v6000` |
| Coast v6000 | Control, old window | 26.8 / 39.9 / 46.2 | 16.7 | 6.41 | 622 / 911 | 12 | 541 / 1,151 | 1,166 / 3,234 | 2,076 | 923 | 1,227 | 0 | `20261015_150157_m4_C_old_coast_v6000` |
| Coast v6000 | Island M, old window | 16.7 / 43.9 / 55.8 | 18.7 | 0.17 | 416 / 1,102 | 217 | 646 / 1,518 | 1,100 / 3,036 | 1,958 | 1,016 | 1,248 | 0 | `20261015_150442_m4_M_old_coast_v6000` |

**Against the §1 thresholds (island M vs the control, both with the new window):**

| Metric (threshold) | Chord v1500 | Chord v3000 | Chord v6000 | Orbit v1500 | Orbit v3000 | Orbit v6000 | Coast v1500 | Coast v3000 | Coast v6000 |
|---|---|---|---|---|---|---|---|---|---|
| Frame P95 (+10%) | −2% ✓ | −4% ✓ | −6% ✓ | 0% ✓ | +3% ✓ | 0% ✓ | −19% ✓ | −19% ✓ | −12% ✓ |
| Manager P95 (+15%) | 0% ✓ | −1% ✓ | −8% ✓ | +3% ✓ | +12% ✓ | +1% ✓ | −27% ✓ | −22% ✓ | −15% ✓ |
| Catch-up (+25%) | 0.38 vs 0.18 s (+111% ✗) | ends off the island (0.17 vs 0.59 s) | ends off the island (1.68 vs 5.39 s) | 0.46 vs 0.17 s (+171% ✗) | 0.48 vs 0.17 s (+182% ✗) | 4.48 vs 4.01 s (+12% ✓) | ends off the island (0.17 vs 0.17 s) | ends off the island (0.17 vs 0.19 s) | ends off the island (0.17 vs 5.59 s) |
| Peak loaded chunks (+25%) | −2% ✓ | −1% ✓ | −15% ✓ | +4% ✓ | +2% ✓ | +8% ✓ | −6% ✓ | −1% ✓ | −10% ✓ |
| Peak voxel MB (+25%) | −20% ✓ | −14% ✓ | −17% ✓ | +6% ✓ | +8% ✓ | +2% ✓ | 0% ✓ | −7% ✓ | +2% ✓ |
| Systems MB (D3: ≤ 1,500 MB) | 918 MB ✓ | 946 MB ✓ | 1,066 MB ✓ | 1,127 MB ✓ | 1,118 MB ✓ | 1,214 MB ✓ | 1,125 MB ✓ | 1,063 MB ✓ | 1,101 MB ✓ |
| Audit after settling | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

The island's catch-up on the chord at v3000 and v6000 and on the coast crossing is not comparable: those routes end off
the island, where nothing is left to load (as in §13.3). The comparable ones are the chord at v1500 (it stops at the
island's centre) and the perimeter. Repeats, same build, three runs each (the batch's run first):

| Route | Control catch-up s | Island M catch-up s | Island M queued at the stop |
|-------|--------------------|---------------------|-----------------------------|
| Chord v1500 (to the centre) | 0.18 / 0.17 / 0.17 | 0.38 / 0.27 / 0.43 | 9-25 chunks |
| Perimeter v1500 | 0.17 / 0.17 / 0.17 | 0.46 / 0.46 / 0.46 | 46 chunks |
| Perimeter v3000 | 0.17 / 0.17 / 0.42 | 0.48 / 0.76 / 0.17 | 20-30 chunks |
| Perimeter v6000 (batch only) | 4.01 | 4.48 (+12%) | |

**Catch-up is missed at low speed, by 0.2-0.3 s:** at v1500 on the chord and the perimeter in every run, and on the
perimeter at v3000 on average (0.47 against 0.25 s over three runs each, with overlapping ranges). The control drains to
nothing before it stops (catch-up 0.17 s is the
metric's floor, ten ticks); the island stops with a standing generation queue and drains it at generation's rate. The
island's view along the coast and over the lagoons is mostly sea, and a sea column's slab runs from 1.5 chunks under the
seabed to 1.5 chunks over the water plane, one layer more than a land column's: on the perimeter at v1500 layer −3 holds
141 chunks against the control's 107, and the island generates 3% more chunks on the route (2,475 against 2,393). v1500
runs generation at capacity, so that 3% is a standing queue of 20-50 chunks. It is a property of a sea-heavy view, not of
the window or the island's shape (the old window's island runs carried far larger queues: 1.11 s on the v1500 chord, 2.26 s
on the v3000 perimeter). At v6000 both sides are generation-bound for seconds and the island is within +12%. Not tuned
here; the levers would be a thinner slab under the water plane or more generation throughput.

**The control is no worse.** Old window → new, same build:

| Route | Frame P95 ms, old → new | Manager P95 ms | Catch-up s | Peak loaded | LOD re-meshes | Systems MB |
|---|---|---|---|---|---|---|
| Chord v1500 | 20.7 → 20.3 | 7.2 → 7.0 | 0.17 → 0.18 | 1,205 → 1,195 | 3,502 → 3,404 | 1,104 → 1,145 |
| Chord v3000 | 30.2 → 28.4 | 9.7 → 8.8 | 0.61 → 0.59 | 1,151 → 1,153 | 7,117 → 6,952 | 1,106 → 1,117 |
| Chord v6000 | 34.9 → 36.3 | 13.8 → 13.9 | 5.93 → 5.39 | 1,115 → 1,129 | 6,977 → 6,889 | 1,323 → 1,364 |
| Orbit v1500 | 21.0 → 20.2 | 7.1 → 6.7 | 0.17 → 0.17 | 1,175 → 1,167 | 2,108 → 2,079 | 1,095 → 1,075 |
| Orbit v3000 | 31.7 → 27.2 | 10.1 → 8.6 | 0.55 → 0.17 | 1,131 → 1,138 | 2,134 → 2,082 | 1,048 → 1,065 |
| Orbit v6000 | 33.4 → 33.7 | 13.3 → 12.9 | 5.38 → 4.01 | 1,128 → 1,081 | 2,106 → 2,081 | 1,157 → 1,187 |
| Coast v1500 | 24.1 → 23.6 | 7.6 → 7.5 | 0.19 → 0.17 | 1,214 → 1,219 | 2,120 → 2,092 | 1,166 → 1,141 |
| Coast v3000 | 32.1 → 32.2 | 10.6 → 10.7 | 0.33 → 0.19 | 1,149 → 1,134 | 2,118 → 2,092 | 1,159 → 1,146 |
| Coast v6000 | 39.9 → 41.8 | 16.7 → 16.9 | 6.41 → 5.59 | 1,151 → 1,120 | 2,076 → 2,086 | 1,227 → 1,252 |

At v6000 the batch's single runs showed frame P95 +4-5% on the chord and the coast with manager time unchanged, so both
were repeated twice more, old and new interleaved: chord frame P95 36.5 → 36.3 ms (means of three), manager P95 14.3 →
13.9 ms, catch-up 6.30 → 5.68 s; coast 42.4 → 42.4 ms, 17.5 → 17.2 ms, 6.51 → 5.78 s. The control loses layer 3 and part of
layer 2 near the viewer (air the old −2..+8 window kept within 16,600 uu) and gains a little ground at the edge of the view
from the horizontal horizon; peak loaded moves −4% to +1%. Its balanced-LOD cache rebuild is cheaper (`LODUpdateMs` P50
0.46 against 0.80 ms at v3000, 0.90 against 1.56 at v6000).

**What M4 did for the island** (island M, old window → new, same build):

| Route | Frame P95 ms, old → new | Manager P95 ms | Peak loaded | Loaded mean | Generated / meshed | LOD re-meshes | Peak voxel MB | Systems MB |
|---|---|---|---|---|---|---|---|---|
| Chord v1500 | 23.6 → 19.9 | 9.8 → 7.0 | 1,750 → 1,170 | 1,598 → 1,051 | 5,718 / 11,458 → 3,914 / 7,229 | 5,768 → 3,317 | 926 → 713 | 1,135 → 918 |
| Chord v3000 | 36.3 → 27.4 | 12.0 → 8.7 | 1,644 → 1,147 | 1,326 → 1,037 | 9,848 / 21,344 → 7,573 / 14,329 | 11,522 → 6,756 | 934 → 742 | 1,150 → 946 |
| Chord v6000 | 39.7 → 34.0 | 14.7 → 12.7 | 1,165 → 965 | 543 → 530 | 4,267 / 12,443 → 4,592 / 11,326 | 8,176 → 6,734 | 970 → 710 | 1,250 → 1,066 |
| Orbit v1500 | 23.9 → 20.2 | 9.0 → 6.9 | 1,784 → 1,213 | 1,696 → 1,127 | 3,611 / 7,348 → 2,475 / 4,540 | 3,737 → 2,067 | 1,017 → 891 | 1,250 → 1,127 |
| Orbit v3000 | 32.9 → 27.9 | 11.9 → 9.6 | 1,722 → 1,165 | 1,394 → 1,106 | 3,112 / 6,804 → 2,504 / 4,603 | 3,699 → 2,100 | 980 → 893 | 1,208 → 1,118 |
| Orbit v6000 | 43.0 → 33.5 | 16.6 → 13.0 | 1,722 → 1,166 | 764 → 683 | 1,135 / 4,161 → 1,308 / 3,374 | 3,050 → 2,066 | 1,034 → 878 | 1,364 → 1,214 |
| Coast v1500 | 22.3 → 19.2 | 8.2 → 5.5 | 1,736 → 1,151 | 1,197 → 809 | 1,680 / 4,096 → 1,236 / 2,664 | 2,416 → 1,428 | 1,012 → 896 | 1,243 → 1,125 |
| Coast v3000 | 35.0 → 26.1 | 12.3 → 8.3 | 1,612 → 1,120 | 1,013 → 780 | 1,606 / 4,008 → 1,236 / 2,663 | 2,416 → 1,427 | 1,014 → 834 | 1,246 → 1,063 |
| Coast v6000 | 43.9 → 36.9 | 18.7 → 14.4 | 1,518 → 1,010 | 646 → 523 | 1,100 / 3,036 → 1,009 / 2,427 | 1,958 → 1,425 | 1,016 → 872 | 1,248 → 1,101 |

Loaded chunks per layer, mean over the v3000 chord: the old window kept every layer full (−3: 188, −2: 195, −1: 198, 0: 198,
1: 192, 2: 185, 3: 169); the new one keeps the control's shape (−3: 100, −2: 227, −1: 230, 0: 230, 1: 228, 2: 24; the
control: −3: 92, −2: 239, −1: 242, 0: 242, 1: 240, 2: 26). KI5's excess layers are gone.

**Flights at z 7,200** (`start=-250000:0:7200 dir=1:0`, 150,000 uu at v3000, no `follow`):

| Run | Window | Loaded per layer, mean | Peak loaded | Frame P95 / manager P95 ms | Catch-up s | Systems MB | JSON |
|-----|--------|------------------------|-------------|----------------------------|------------|------------|------|
| Control | old | −1: 228, 0: 240, 1: 243, 2: 34, 3: 33 | 1,026 | 21.3 / 5.9 | 0.17 | 1,006 | `20261015_150556_m4_C_old_fly` |
| Control | **new** | −3: 96, −2: 241, −1: 242, 0: 243, 1: 244, 2: 34, 3: 33 | 1,212 | 23.1 / 7.2 | 0.18 | 1,066 | `20261015_150827_m4_C_new_fly` |
| Island M | old | −1: 220, 0: 221, 1: 222, 2: 221, 3: 220 | 1,230 | 24.3 / 7.5 | 0.63 | 1,181 | `20261015_150709_m4_M_old_fly` |
| **Island M** | **new** | −3: 114, −2: 219, −1: 220, 0: 221, 1: 222, 2: 30, 3: 30 | 1,218 | 24.4 / 7.5 | 0.63 | 904 | `20261015_150943_m4_M_new_fly` |

G13 is fixed: with the old window neither side streamed anything below layer −1 at this altitude, the island kept full
air layers 2 and 3, and the control lost its valleys. Now both keep the ground (layers −3 and −2) and only the air the
slab or the mountains need. The flights cost the work the old window skipped (control manager P95 5.9 → 7.2 ms); island
against control, frame +6% and manager +5%, peak loaded +0.5%. The flight ends 100,000 uu inside the island, so its
catch-up is comparable: 0.63 s against 0.18, the same sea-view effect as above.

**Fully resident S, and the views from 48,000 uu** (fresh world at the spawn; settled = 75 s idle so far compression has
run, then `voxel.FarCompression.Stats` and a zero-length bench; "48,000 uu up" = a zero-length bench whose viewer is
(0, 0, 48,000), started after the spawn's load):

| Run | Window | Loaded chunks | Voxel MB settled (resident / uniform / compressed chunks) | Renderer GPU MB | Collision / scatter MB | Voxel systems MB settled | Audit | JSON |
|-----|--------|---------------|-----------------------------------------------------------|-----------------|------------------------|--------------------------|-------|------|
| Control at its spawn | old | 1,026 | 133.4 (112 / 63 / 851) | 23 | 102 / 92 | 359 | 0 | `20261015_155301_m4_C_old_settled` |
| Control at its spawn | **new** | 979 | 127.3 (106 / 47 / 826) | 23 | 101 / 90 | 352 | 0 | `20261015_155612_m4_C_new_settled` |
| S at its spawn | old | 1,512 | 248.1 (224 / 96 / 1,192) | 23 | 99 / 93 | 483 | 0 | `20261015_155436_m4_S_old_settled` |
| **S at its spawn** | **new** | **1,020** | **125.7 (105 / 63 / 852)** | 21 | 96 / 93 | **336** | 0 | `20261015_155747_m4_S_new_settled` |
| Control, 48,000 uu up | old | 626 mean, falling (3D horizon: everything unloads) | | | | | 248 (still unloading) | `20261015_151126_m4_C_old_high` |
| Control, 48,000 uu up | **new** | 979 (the ground keeps the spawn's disk) | | | | | 0 | `20261015_151209_m4_C_new_high` |
| S, 48,000 uu up | old | 1,512 (what the spawn loaded; the window, layers 8-14, held no terrain, so nothing new would load) | | | | | 0 | `20261015_151150_m4_S_old_high` |
| **S, 48,000 uu up** | **new** | **1,020, the whole island** | | | | | 0 | `20261015_151230_m4_S_new_high` |

- **I3 again:** the whole of S is resident from the centre in 1,020 chunks (review B: 1,518), 336 MB settled, the same as
  the control at its spawn (352 MB). The near-band excess of KI13 is gone: 105 uncompressed chunks against the control's
  106.
- **I2 again:** a survey camera 48,000 uu over S's centre sees the whole island, lagoons included, with no blank chunk
  rectangles (`m4_I2_S_from_48000_new`; review B's §13.1 I2 saw them). §13.11 suspected chunks that never streamed; with
  the window above the terrain they could not.

**The D3 budget is met on every run.**

| Budget (D3) | Measured |
|-------------|----------|
| 0.6 GB of voxel systems memory settled for a fully resident S-class island | 336 MB (the old window: 483 MB; review B: about 0.5 GB) |
| At most 1.5 GB at peak while streaming, any speed and tier | Island M 858-1,214 MB over its 16 runs with the new window (routes, repeats, the flight) and 1,135-1,364 MB with the old one; M's v6000 chord 1,066 MB (review B: 1,552, the one miss); the control 1,065-1,364 MB |

**What changes in §13.** I3 passes with the numbers above. I4's verdict ("Refine inside the island, Bug along the coast")
becomes **Pass except catch-up at v1500**: the island meets frame, manager, peak-loaded and memory thresholds on the chord,
the perimeter and the coast at every speed, the coast included (frame −12% to −19% of the control there, manager −15% to
−27%). KI5, KI11 and KI13 are fixed (§13.6), G13 too (§13.11); §13.4's budget is met with room; §13.8's "island-specific
costs (KI5, KI13, the coast) are per view" now leaves only the sea-view generation cost above.

**Left for later milestones**, as planned: cubic (M5), the visible edge and map tile eviction (M6), and spherical Phase 1,
which takes the window's shape: `GetColumnWindow`'s planet branch becomes a shell around the surface (SP-C1). Things to
know:

- A viewer high over a heightmap world keeps and streams the ground disk under it (the horizon is horizontal), at the
  coarsest LOD; a 3D horizon used to empty the world from 40,000 uu up.
- `bFarBandSurfaceSlabCulling` keeps its name; it now means the slab at every distance. Off, every column streams the
  whole height range.
- `IVoxelWorldMode::GetSurfaceRange` has a default, so a world mode outside VoxelWorlds (the dungeon plugin's, a test
  mock) works unchanged: heightmap modes sample their own height, others give the whole height range.
- The near window's extra layers unload as soon as their column leaves the near radius, as the old slab gate did.

### 14.9 M5 results (2026-10-16)

All four M5 items are implemented and verified; PRs open. Built from VoxelWorlds `feature/wm-c1-cubic-seam-path` (base
`af0037b`), VoxelCharacterPlugin `feature/wm-c2-block-editing` (base `8bc5249`) and UE5_PluginDev
`claude/m5-cubic-terrain-only-2f874f` (base `b781218`), editor 5.8.3 from `D:\Program Files\Epic Games\UE_5.8`, in a
worktree editor with background throttling off.

**Cubic on the seam path, as built (WM-C1).** Cubic meshes through the seam-ownership pipeline like Dual Contouring and
Marching Cubes (`SEAM_OWNERSHIP_ARCHITECTURE.md` §2.3, "trivially compatible"):

| Part | What it meshes | Reads |
|------|----------------|-------|
| Chunk mesh (Interior domain) | Every face except those on the chunk's six boundary planes, greedy-merged as before | The chunk's own voxels only: a chunk never waits for a neighbour and is meshed once |
| Face seam job (`FVoxelCPUCubicMesher::GenerateFaceSeamMeshCPU`) | The shared plane: A's faces toward +axis in its last layer and B's toward -axis in its first, each culled against the other side's voxel, greedy-merged in the plane; positions in the owner's frame | Both chunks' shared snapshots |
| Edge and corner seams | Nothing: no cubic face lies only on a chunk edge or corner. The registry tracks face seams alone for cubic (`FVoxelSeamRegistry::SetFaceSeamsOnly`) | — |

Vertex AO samples outside the chunk (interior) or the pair (seam) as air; no voxel material reads vertex AO. Collision
is unchanged: it cooks whole chunks with neighbour slices, extracted on a worker from shared snapshots.

**Deleted with the legacy whole-chunk path:** the game-thread neighbour-slice extraction for render meshes
(`ExtractNeighborEdgeSlices`, and with it `GetChunkCollisionMesh` and the synchronous branch of
`PrepareCollisionMeshRequest`), the deferral while a face neighbour generates (`ShouldDeferMeshForNeighbors`),
`CaptureMeshDeps` / `RevalidateMeshDeps` and `voxel.Stream.MeshDepRevalidate`, the 26-neighbour re-mesh on generation
and its coalescing (`QueueNeighborsForRemesh`, `ProcessPendingNeighborRemeshes`, `voxel.Stream.NeighborRemeshCoalesce`,
`.NeighborRemeshDebounceFrames`, `.NearCorrectionDistance`, `.NearCorrectionDebounceFrames`), the legacy LOD face
cascades, the transition-face and neighbour-LOD setup of chunk requests, `voxel.LogBoundaryResidency`, the
far-compression hold for the legacy path (`FarExpandedSeenFrame`), the registry's P0 stub processor
(`ProcessSeamJobQueue`) and `voxel.Seam.Registry` / `voxel.Seam.Debug` (the registry is always on). About 1,300 lines
of the chunk manager. The bench CSV loses `GenNeighborMs` and `MeshSliceMs`, the JSON `thrashNeighbor` (no path
produces neighbour re-meshes any more), and gains `meshMsP95`, `seamMsP50/P95`, `meshJobsLaunched` and
`seamJobsCompleted`.

**WM-C4.** `FVoxelGPUCubicMesher`, `CubicMeshGeneration.usf` and the `Meshing.Cubic.GPUAsync`, `.GPUReadback` and
`.CPUvsGPU` tests are deleted; `Meshing.Cubic.Performance` is CPU-only. `CubicMeshCommon.ush` stays (the GPU Marching
Cubes shader includes it). The README no longer claims a GPU cubic mesher.

**Bench method.** One build, one batch (VoxelWorlds at this PR's code plus a temporary `voxel.Temp.CubicLegacyBoundary` cvar
that kept cubic on the legacy whole-chunk path, latched when the world initializes, removed before the PR together with
the path itself). §12's route: `voxel.Bench.Run <tag> <v> 40000 start=0:0 dir=1:0 warmup=120`, one cold fresh world per
run, interleaved per speed: control, cubic, cubic on the legacy path; v3000 twice. PIE in the worktree editor's level
viewport (1280×722), background throttle off, `vox.DeleteSave`, clock pinned at noon, after two throwaway PIEs (G14). The
control is `DemoSmoothWorldConfig` and cubic is `Review_Cubic`, both through `voxel.ConfigOverride`, as in §12. JSON reports
in [Research/Bench/M5/](Research/Bench/M5/).

**PIE** ("Mesh" is the meshing section's P95, "Seam" the seam scheduler's, "Slices" the legacy neighbour-slice extraction's;
"Chunk / seam jobs" over the whole run; thrash = re-meshes of chunks that already had a mesh, by reason LOD / neighbour /
other):

| Run | Warmup s | Frame P50 / P95 / P99 ms | Manager P95 ms | Mesh / Seam / Slices P95 ms | Catch-up s | Peak mesh queue | Thrash (LOD / neighbour / other) | Chunk / seam jobs | Peak / end voxel MB | End resident / uniform / compressed | Renderer GPU MB | Systems MB | Audit | JSON |
|-----|----------|---------------------------|----------------|-----------------------------|------------|-----------------|----------------------------------|-------------------|---------------------|-------------------------------------|-----------------|------------|-------|------|
| Control v1500 | 1.2 | 16.7 / 24.9 / 31.5 | 7.3 | 1.20 / 2.72 / — | 0.17 | 7 | 251 (251 / 0 / 0) | 1,191 / 17,992 | 745 / 336 | 312 / 77 / 663 | 29 | 956 | 0 | `20261016_154526_m5_C_v1500a` |
| **Cubic** v1500 | 1.4 | 16.7 / 20.4 / 34.6 | 5.8 | 0.59 / 1.30 / — | 0.17 | 2 | 0 (0 / 0 / 0) | 717 / 1,953 | 664 / 187 | 160 / 90 / 802 | 295 | 1,105 | 0 | `20261016_154613_m5_K_v1500a` |
| Cubic, legacy path v1500 | 6.1 | 16.7 / 25.8 / 37.0 | 10.2 | 5.11 / 0.04 / 4.14 | 0.51 | 43 | 1,641 (0 / 1,641 / 0) | 3,041 / 0 | 932 / 518 | 495 / 59 / 498 | 301 | 1,386 | 0 | `20261016_154703_m5_L_v1500a` |
| Control v3000 | 1.0 | 19.8 / 30.7 / 36.9 | 9.9 | 2.19 / 2.77 / — | 0.28 | 7 | 252 (252 / 0 / 0) | 1,177 / 17,708 | 779 / 528 | 509 / 57 / 486 | 29 | 992 | 0 | `20261016_154326_m5_C_v3000a` |
| Control v3000, re-run | 2.3 | 22.6 / 37.4 / 44.1 | 12.4 | 2.42 / 3.67 / — | 0.45 | 7 | 252 (252 / 0 / 0) | 1,304 / 18,996 | 704 / 572 | 554 / 49 / 449 | 28 | 915 | 0 | `20261016_154920_m5_C_v3000b` |
| **Cubic** v3000 | 2.7 | 16.7 / 26.7 / 44.7 | 7.5 | 0.81 / 1.67 / — | 0.17 | 2 | 0 (0 / 0 / 0) | 868 / 2,366 | 592 / 306 | 282 / 76 / 694 | 286 | 1,041 | 0 | `20261016_154358_m5_K_v3000a` |
| **Cubic** v3000, re-run | 3.1 | 16.7 / 27.7 / 46.2 | 7.6 | 0.89 / 1.58 / — | 0.28 | 2 | 0 (0 / 0 / 0) | 896 / 2,435 | 600 / 311 | 288 / 76 / 688 | 289 | 1,045 | 0 | `20261016_154957_m5_K_v3000b` |
| Cubic, legacy path v3000 | 5.6 | 17.4 / 46.9 / 75.3 | 15.3 | 5.19 / 0.05 / 4.16 | 4.11 | 132 | 1,092 (0 / 1,092 / 0) | 2,496 / 0 | 943 / 649 | 632 / 44 / 376 | 285 | 1,412 | 0 | `20261016_154441_m5_L_v3000a` |
| Cubic, legacy path v3000, re-run | 7.2 | 17.7 / 54.2 / 77.4 | 18.6 | 5.73 / 0.06 / 4.44 | 4.90 | 140 | 1,101 (0 / 1,101 / 0) | 2,525 / 0 | 948 / 669 | 652 / 46 / 354 | 285 | 1,404 | 0 | `20261016_155041_m5_L_v3000b` |
| Control v6000 | 0.9 | 38.0 / 50.2 / 54.9 | 21.2 | 4.94 / 5.66 / — | 3.73 | 51 | 252 (252 / 0 / 0) | 1,144 / 15,563 | 782 / 755 | 742 / 24 / 286 | 28 | 1,030 | 0 | `20261016_154732_m5_C_v6000a` |
| **Cubic** v6000 | 3.9 | 17.5 / 35.2 / 47.6 | 10.6 | 0.85 / 1.56 / — | 2.87 | 0 | 0 (0 / 0 / 0) | 949 / 2,577 | 618 / 500 | 482 / 60 / 510 | 279 | 1,056 | 0 | `20261016_154805_m5_K_v6000a` |
| Cubic, legacy path v6000 | 7.7 | 19.5 / 51.0 / 64.1 | 17.4 | 5.71 / 0.06 / 4.07 | 8.46 | 138 | 979 (0 / 979 / 0) | 2,406 / 0 | 953 / 760 | 745 / 43 / 264 | 287 | 1,423 | 0 | `20261016_154845_m5_L_v6000a` |

All audits reported 0. The legacy rows reproduce §12.8's cubic (catch-up 0.71 / 5.2 / 9.7 s, manager P95 10.9 / 15.9 / 16.4
ms at v1500 / v3000 / v6000), so the batch compares against the same baseline §12.8 measured.

**Against §14.3's done-when (cubic vs the same-build control; v3000 = mean of the two runs each):**

| Metric (threshold) | v1500 | v3000 | v6000 |
|--------------------|-------|-------|-------|
| Catch-up (+25%) | 0.17 vs 0.17 s (0%, both at the metric's floor) ✓ | 0.23 vs 0.37 s (−38%) ✓ | 2.87 vs 3.73 s (−23%) ✓ |
| Manager P95 (+15%) | 5.8 vs 7.3 ms (−21%) ✓ | 7.6 vs 11.2 ms (−32%) ✓ | 10.6 vs 21.2 ms (−50%) ✓ |
| Frame P95 (+10%) | 20.4 vs 24.9 ms (−18%) ✓ | 27.2 vs 34.1 ms (−20%) ✓ | 35.2 vs 50.2 ms (−30%) ✓ |
| Peak loaded chunks (+25%) | 1,129 vs 1,123 (+1%) ✓ | 1,102 vs 1,076 (+2%) ✓ | 1,052 vs 1,052 (0%) ✓ |
| Peak voxel MB (+25%) | 664 vs 745 (−11%) ✓ | 596 vs 742 (−20%) ✓ | 618 vs 782 (−21%) ✓ |
| Systems MB (+25%; D3 ≤ 1,500) | 1,105 vs 956 (+16%) ✓ | 1,043 vs 954 (+9%) ✓ | 1,056 vs 1,030 (+3%) ✓ |
| Audit after settling | 0 | 0 | 0 |

§12.8's misses are closed: catch-up was 0.71 / 5.2 / 9.7 s against 0.17 / 0.93-1.07 / 5.0 s, and manager P95 +27% (v1500)
and +19% (v3000). Renderer GPU memory stays about 10× the control's (279-295 against 28-29 MB): every cubic chunk is full
detail, as §12.5 explained; it is in the systems total above and does not depend on how boundaries are meshed.

**What WM-C1 did for cubic** (legacy path → seam path, same build):

| Speed | Frame P95 ms | Manager P95 ms | Catch-up s | Peak mesh queue | Chunk mesh jobs | Re-meshes | Peak voxel MB | Systems MB |
|-------|--------------|----------------|------------|-----------------|-----------------|-----------|---------------|------------|
| v1500 | 25.8 → 20.4 | 10.2 → 5.8 | 0.51 → 0.17 | 43 → 2 | 3,041 → 717 | 1,641 → 0 | 932 → 664 | 1,386 → 1,105 |
| v3000 (means) | 50.6 → 27.2 | 17.0 → 7.6 | 4.51 → 0.23 | 136 → 2 | 2,511 → 882 | 1,097 → 0 | 946 → 596 | 1,408 → 1,043 |
| v6000 | 51.0 → 35.2 | 17.4 → 10.6 | 8.46 → 2.87 | 138 → 0 | 2,406 → 949 | 979 → 0 | 953 → 618 | 1,423 → 1,056 |

Each chunk is meshed once. The legacy path meshed 2.5-4.2 times as many chunk jobs: the deferral held chunks back until
their face neighbours had data, and every arrival re-meshed the loaded neighbours (the coalesced 26-neighbour cascade,
KC2). Its neighbour-slice extraction alone was 4.1-4.4 ms of the meshing section's P95. The seam path runs 1,953-2,577 face
seam jobs per run instead (the control: 15,563-18,996 face, edge and corner jobs at mixed LOD); the seam scheduler's P95
is 1.3-1.7 ms against the control's 2.7-5.7 ms. Peak voxel data falls by about a third: the cascade kept chunks decompressed
and queued. KC14's per-frame deferral loop is gone with the path; the M1 bench fix (WM-H8) had already stopped it from
inflating the thrash counter.

**What is left on cubic's manager P95.** Averaged over the slowest 5% of traverse ticks at v3000: render submit 5.8-6.5 ms
(on its worst ticks the chunk unloads, 9-14 ms in single ticks), collision 4.0 ms (single cook applies of 15-34 ms), the
seam scheduler 0.9-1.1, meshing 0.4-0.5 (control: render 3.2, seam 2.3, scatter 2.1, collision 1.8, meshing 1.5). Both are
pre-existing costs of cubic's larger meshes (3-4× the control's triangles), not boundary work; neither is near a threshold.

**Initial load and settled state** (fresh world at the spawn; "load" = a zero-length bench started as PIE came up, whose
warmup is the initial load; settled = after 75 s idle, `voxel.FarCompression.Stats` and the renderer's own counters):

| | Control (smooth) | **Cubic, seam path** | Cubic, legacy path |
|---|---|---|---|
| Load: warmup to drained (from bench start) | 7.6 s | 9.1 s | 12.0 s |
| Load: chunk mesh jobs / re-meshes / seam jobs | 805 / 2 (LOD) / 7,775 | 783 / **0** / 2,125 | 1,834 / 882 (neighbour) / 0 |
| Load: interior crossfades (renderer) | 3 | **0** | 592 (54 mid-fade restarts) |
| Settled: loaded / chunks with geometry | 979 / 428 | 979 / 459 | 979 / 610 |
| Settled: triangles / renderer GPU | 0.86 M / 23 MB | 3.80 M / 247 MB | 3.89 M / 252 MB |
| Settled: voxel data (resident / uniform / compressed chunks) | 128.3 MB (107 / 47 / 825) | 120.4 MB (99 / 50 / 830) | 121.4 MB (100 / 50 / 829) |
| Settled: seam registry (seams tracked / dirty, never-ready) | 8,578 / 3,288 | 3,348 / 822 | 8,578 / 3,288 (stub) |
| Settled: voxel systems MB / audit | 355 / 0 | 578 / 0 | — / 0 |

The 151 chunks that only the legacy path gives geometry are the walls at the loaded region's edge (KC13, below) and
boundary-only faces; the seam path's settled triangle count is 2% lower for the same terrain.

**Headless CPU sweep** (VoxelWorldsTest, `-VoxelForceCPU`, the same route; no editor open; whole-run job counts):

| Run | Frame P50 / P95 ms | Manager P95 ms | Mesh / Seam P95 ms | Catch-up s | Thrash (LOD / neighbour / other) | Chunk / seam jobs | Peak / end voxel MB | End resident / uniform / compressed | Systems MB | JSON |
|-----|--------------------|----------------|--------------------|------------|----------------------------------|-------------------|---------------------|-------------------------------------|------------|------|
| Control v1500 | 1.1 / 3.9 | 1.9 | 0.03 / 0.66 | 0.0 | 497 (497 / 0 / 0) | 2,055 / 28,242 | 218 / 166 | 138 / 93 / 821 | 492 | `20261016_155836_m5h_C_v1500` |
| Control v3000 | 1.3 / 5.6 | 3.3 | 0.14 / 1.08 | 4.7 | 498 (498 / 0 / 0) | 2,056 / 28,012 | 202 / 170 | 143 / 91 / 818 | 501 | `20261016_155934_m5h_C_v3000` |
| Control v6000 | 2.5 / 8.4 | 5.0 | 0.62 / 1.15 | 10.7 | 499 (499 / 0 / 0) | 2,053 / 27,783 | 224 / 164 | 136 / 93 / 823 | 531 | `20261016_160033_m5h_C_v6000` |
| **Cubic** v1500 | 0.9 / 1.6 | 0.2 | 0.00 / 0.01 | 0.0 | 0 (0 / 0 / 0) | 1,558 / 4,092 | 177 / 96 | 69 / 97 / 886 | 622 | `20261016_160142_m5h_K_v1500` |
| **Cubic** v3000 | 1.0 / 2.5 | 1.0 | 0.00 / 0.03 | 4.2 | 0 (0 / 0 / 0) | 1,558 / 4,088 | 153 / 84 | 55 / 94 / 903 | 587 | `20261016_160239_m5h_K_v3000` |
| **Cubic** v6000 | 1.2 / 3.5 | 1.8 | 0.00 / 0.04 | 10.2 | 0 (0 / 0 / 0) | 1,554 / 4,075 | 137 / 86 | 58 / 97 / 897 | 576 | `20261016_160339_m5h_K_v6000` |
| Cubic, legacy path v3000 | 1.1 / 4.9 | 3.0 | 0.88 / 0.01 | 6.0 | 2,460 (0 / 2,460 / 0) | 4,018 / 0 | 812 / 454 | 425 / 60 / 567 | 1,210 | `20261016_160443_m5h_L_v3000` |

Headless, cubic is within the control at every speed (catch-up 0.0 / 4.2 / 10.2 s against 0.0 / 4.7 / 10.7, manager P95
0.2-1.8 ms against 1.9-5.0) and needs a third of the chunk mesh jobs it needed on the legacy path (1,558 against 4,018 at
v3000; legacy thrash 2,460, systems 1,210 MB against 587).

**Scenarios (§4), on the final build** (`Review_Cubic`, fresh world, after a throwaway PIE):

| Scenario | Verdict | What was run |
|----------|---------|--------------|
| C2 Streaming | **Pass** | A fresh world settles at t = 15 s with the same geometry every start. A chunk corner (−2,055, 1,521) surveyed from both sides: no hole, missing or stale face (`m5_c2_corner_a`, `_b`). **W-04 flown** at 3,000 uu/s following the terrain (`voxel.Bench.Run m5_c2_w04_follow 3000 180000 dir=0:1 follow=300 warmup=60`): catch-up 0.5 s, peak mesh queue 2, thrash 0, frame P95 21.1 ms, manager P95 5.1 ms, peak systems 886 MB, audit 0, and the terrain ahead is there (`m5_c2_w04_mid`, `_mid2`; Review A as shipped: catch-up 28 s, mesh queue 1,739, thrash 13,698, the terrain ahead missing). **R23 A→B→C** (`vox.TP` to (80,000, 0), then (80,000, 80,000), then back to the spawn): every settle released on cooked collision; `AuditResidency` 0 findings, no render or seam orphans, no stuck jobs, no late seam results rejected; no chunk left in `Meshing`. `FarCompression.Stats`: 98 resident, 53 uniform, 953 compressed, 123.3 MB resident, 981 MB reclaimed, 0 stale. **KC13 rechecked:** with streaming paused and a camera 6,000-14,000 uu outside the loaded region, its edge is open, a cut-away into the caves like the control's; no walls (`m5_c2_kc13_frozen_high`, `_low`). **Fades** render with the atlas material: frontier chunks mid first-appearance fade, 35 active (`m5_c2_fade_4`). Scatter shows up to a second before the terrain it stands on fades in, on the control as well (`m5_ctrl_fade_2`): R24's first-appearance hold holds the terrain, not the scatter; unchanged here |
| C4 Editing | **Pass** (after G15, below) | The player's own input (`IA_PrimaryAction` / `IA_SecondaryAction`, edit mode on, first-person view), measured by depth traces at block-face centres before and after each click: on **+Z, +X and −Y** faces of the terrain and on **+Y, −X** (a scaffold pillar) and **−Z** (under a scaffold slab; both written through the edit manager to have those faces in reach), one dig removes exactly the block under the crosshair and one place adds exactly one block against the aimed face. The old rule (the floor of the impact point) picks the air block in front on every +X, +Y and +Z face (KC5). A stone pickaxe digs with edit mode off: one block, the one hit; `vox.Dig` at the crosshair removes the block the camera ray hits. **A pit across a chunk corner** (2×2 columns, two blocks deep, four chunks, through the server path): walls and floor closed on both chunk planes (`m5_c4_corner_pit_a`, `_b`); the pawn dropped into it lands on its floor at the corner, no fall-through. **`vox.Save`, Stop, Play:** the pit and every dig come back (column tops 825 / 825 / 750 / 825, two blocks down), but the first reload rendered the terrain around the player wrongly: G15 |

**G15, found in C4: a recreated scene proxy drops the geometry it held.** After the reload, a strip of terrain around the
saved spot rendered as the caves under it, with the plants at the right height. Collision, scatter and the edits were
right, and `voxel.RemeshAll` or re-meshing the one chunk brought it back. Temporary logs showed the chunk meshed once from
the right data (1,865 solid voxels, 1,464 vertices, the same as its collision) and submitted, and then the world
component's scene proxy recreated a moment later. The engine does this on material and render-state changes; here the
scatter system's runtime billboard material compiles in the first second of play, after the first chunks went out. A new
`FVoxelSceneProxy` starts empty, and nothing sent the old one's chunk meshes, seam buckets or water tiles again. A fresh
world loses the same way (7 chunks at the spawn in the check below). Smooth worlds lose the same chunks but usually
re-mesh them for LOD soon after; the legacy cubic path re-meshed them in the neighbour cascade. On the seam path a cubic
chunk is meshed once, so the loss stayed.

The fix: `UVoxelWorldComponent::CreateSceneProxy` flags a proxy that replaces one with chunks in it;
`IVoxelMeshRenderer::ConsumeLostRenderData` (new, default no-op) reports it once with the chunks the renderer had; at the
start of its tick the chunk manager re-meshes those that are `Loaded` (`RecoverLostRenderData`; chunks still in the
pipeline submit to the new proxy anyway), sends every seam bucket again from the slots it keeps (no seam job re-runs),
and marks every water tile dirty. Live, the reload logs `re-meshing 5 chunk(s), re-sending 4 seam bucket(s) and 2 water
tile(s)` and renders whole (`m5_fix_reload8`, `m5_fix_corner_pit_reload`); a fresh world logs 7 / 1 / 1 and its spawn
renders whole (`m5_fix_fresh_spawn`). Test `Streaming.Cubic.RendererProxyRecreated`: after the renderer drops everything,
the 105 chunks are re-meshed once each, the 77 seam buckets come back and the rendered faces equal the ones before
(88,321 of 88,321). Every meshing mode gets the fix. The benches above ran before it; it acts only when the engine
recreates the proxy, and otherwise costs a lock and a flag check per tick.

| ID | Status | PR | Check |
|----|--------|----|-------|
| WM-C1 | Done, in review | VoxelWorlds#94 | The tables above; tests `Meshing.Cubic.InteriorDomain` (no face on the six boundary planes; every other face as the whole-chunk mesher gives it), `.SeamClosure` (a 2×2×2 block of 8³ chunks, interiors plus the 12 face seams, equals the 16³ region meshed whole, in unit faces, greedy and simple), `Streaming.Cubic.SeamPath` (a real chunk manager into a recording renderer: 105 chunks, each meshed once and never re-meshed; the interiors and seam buckets equal the whole-chunk meshes against loaded neighbours, 88,321 unit faces, none missing, doubled or at the loaded region's edge; the legacy path fails it with 96 re-meshes and 18,669 extra faces, KC13's walls) and `.RendererProxyRecreated` (G15); C2 |
| WM-C2 | Done, in review | VoxelCharacterPlugin#23, VoxelWorlds#94, UE5_PluginDev#106 | `AVCCharacterBase::GetEditVoxelCoord` (Blueprint-callable) is the one target rule: place at `ImpactPoint + ½·VoxelSize·normal`; in cubic, dig at `− ½·VoxelSize·normal`; smooth digs unchanged. Cubic edits are whole blocks (a cube brush at the block's centre, density 255); the placement overlap check boxes the block itself. The four dig and place sites, `vox.Dig` and the demo brush (`AVoxelWorldTestActor::IsDiscreteEditing`: always discrete in cubic) use it. Test `VoxelCharacter.Edit.BlockTargeting` (all six face directions, impact points a hair either side of the face, the demo's offset origin; smooth targets unchanged); C4 |
| WM-C3 | Done, in review | VoxelWorlds#94 | `UVoxelScatterManager::WantsGPUExtraction`: cubic always extracts block-face centres on the CPU and logs that `bUseGPUScatterExtraction` is ignored. `BlockFaceSnap` keeps the jitter inside the face. Tests `Scatter.Cubic.CPUExtractionOnly`, `.BlockFaceSnapStaysOnFace` |
| WM-C4 | Done, in review | VoxelWorlds#94 | `FVoxelGPUCubicMesher` and `CubicMeshGeneration.usf` deleted with three tests; `CubicMeshCommon.ush` stays (GPU Marching Cubes includes it); the README, `ARCHITECTURE.md` and `LOD_SYSTEM.md` describe the CPU mesher only |

**Suites.** Headless `VoxelWorlds+VoxelWorldSave+VoxelCharacter` 275/275. In the editor, where the GPU tests run, 275/275, every
`Generation.GPUvsCPU*Parity`, `GPUNoiseGeneratorAsync`, `Meshing.MarchingCubes.CPUvsGPU` and
`WorldModes.InfinitePlane.GPUConsistency` among them. 271 before M5, less the three GPU cubic tests, plus seven new
(`Meshing.Cubic.InteriorDomain`, `.SeamClosure`, `Streaming.Cubic.SeamPath`, `.RendererProxyRecreated`,
`Scatter.Cubic.CPUExtractionOnly`, `.BlockFaceSnapStaysOnFace`, `VoxelCharacter.Edit.BlockTargeting`). Both on the final
build, with the temporary cvar and the diagnostics removed.

**What changes in §12.** C2 becomes **Pass**: the as-shipped problems (W-04 could not keep up, KC13's walls) are gone.
C4 becomes **Pass**. KC2, KC3, KC4, KC5, KC13 and KC14 are fixed (§14.1). §12.6's account of where cubic and smooth
separate loses its boundary row: both mesh chunk boundaries through single-owner seam jobs now. What still separates them
is detail, not boundaries: every cubic chunk is full detail (D7), so its meshes carry 3-4× the control's triangles and
about 10× its renderer memory, and its slowest ticks are render submits and collision applies of those meshes.

**Left for later**, as planned: KC6, KC7, KC9 and KC12 stay deferred with D1 (cubic is terrain-only), and spherical
Phase 1 can now test seams at mid latitudes without the legacy path (SP-D4). Things to know:

- The bench CSV and JSON changed (see the deletions above); compare `thrashRemeshCount` and the meshing columns only with
  runs from this build on.
- Cubic now builds face seams only; `voxel.Seam.*` cvars apply to it as to the smooth meshers.
- A renderer outside VoxelWorlds that keeps its geometry in a scene proxy should override `ConsumeLostRenderData`.

### 14.10 M6 results (2026-10-16)

Both M6 items are implemented and verified; PRs open. Built from VoxelWorlds `feature/wm-e1-e2-island-edge-scale` (base
`21779bc`), VoxelCharacterPlugin `feature/wm-e2-world-map-tiles` (base `288af96`) and UE5_PluginDev
`claude/m6-island-edge-scale-e3d76e` (base `7b7e473`), editor 5.8.3 from `D:\Program Files\Epic Games\UE_5.8`, in a
worktree editor with background throttling off. Live checks ran on `VoxelDemo` with the review configs through
`voxel.ConfigOverride`, from a fresh world. Screenshots: `Saved/Screenshots/Claudius/m6b_*` (before, on the unchanged
build) and `m6a_*` (after) in the M6 worktree (not committed).

**Design (written before building, as §14.2 asked).**

| Question | Choice | Why |
|----------|--------|-----|
| Where the far ocean lives | **Its own components**: procedural mesh components on the world actor (the static meshes; the grid's frame and window), built and updated by the chunk manager (`FVoxelFarOcean` geometry, `UVoxelChunkManager::RebuildFarOcean` / `UpdateFarOceanGrid`), beside the boundary walls | Not the world component's scene proxy: a proxy recreation empties it (G15), so everything in it must be re-sent; a component of its own keeps its sections and rebuilds its own proxy. Not the water tiles: they are per chunk column and per tile a draw with its own buffers, and the sea to the horizon would be thousands of them. Works with both renderers (custom VF and PMC) |
| How it meets the water tiles | The far ocean's **grid** covers every chunk column of the footprint's box (plus two) that the streaming has not **opened**; a column opens when it holds a chunk with voxel data or has a water tile. The grid is rebuilt in the tick the open set changes, after the water tiles are processed (a window around the open columns; the rest of the grid only when the window moves, below) | The water material is translucent: a far ocean under or over a water tile draws two layers. Exact per column, so no overlap and no gap, whatever loads |
| What streams past the edge | **Only columns that touch the footprint** (`FVoxelWorldBounds::IntersectsBox2D`, the map's tile test); the ring out to a chunk diagonal past R+F is gone | Nothing past the walls (WM-I5) needs terrain or collision, and the ring's floor and water were what the far ocean replaces; it also makes the grid's open set and the cull the same rule |
| The seabed | **Generation slopes it**: past R+F the height goes from `EdgeHeight` down by `IslandSeabedDepth` (6,000) over `IslandSeabedSlopeWidth` (20,000), a smoothstep, then flat. CPU (`FIslandBowlWorldMode::GetSeabedHeight`, shared by generation, the analytic height and `GetDensityAt`) and GPU (`IslandSeabedHeight` in `IslandFalloff.ush`) | The cliff was generated; only generation can remove it. A smoothstep starts flat, so it meets the falloff (Smooth ends flat) with no kink, and no step at R+F for any curve. Distance past the edge is the falloff's own metric (radial, or the larger axis excess for a rectangle), so the slope's contours follow the coast |
| The seabed past what streams | A **static seabed mesh** on the same profile, half a voxel under it, from R+F out to the open sea's corners; and a **floor** under the grid's columns inside the footprint (flat at `EdgeHeight`), rebuilt with the grid | The voxel seabed past R+F is the analytic profile (no noise there), which MC and DC follow within a few uu, so the drawn seabed hides under it wherever it loads and continues it where it does not. Without the floor the far ocean's water over unloaded columns inside R+F showed the void through it (a light ring in the first views) |
| With the boundary (WM-I5) | **Unchanged**: walls at R+F, push-back in the last 4,000 uu | The seabed starts at R+F at `EdgeHeight`, under the walls; the push-back keeps a swimmer about 1,600 uu inside them |
| GPU parity | `GPUvsCPUIslandBowlParity` gains a seabed case for a circle and a rectangle, at LOD 0/1/2 | The shader port is the risk; the existing cases never reached the floor |
| Save fingerprint | **No `GenerationVersion` bump; the seabed is not in the fingerprint** | Generation inside R+F is unchanged byte for byte; the seabed shapes only terrain past R+F, where a save holds nothing: POIs and map tiles stay inside the bounds (M3), and no edit reaches past R+F (the push-back stops a pawn 1,200-1,600 uu inside the walls and a dig reaches 500 uu; only debug flight gets to the wall). A bump would have refused every save, InfinitePlane's included, for a change none of them can see |

**WM-E2: map tile images under a fixed budget.** §14.2 offered eviction with regeneration on demand, or coarser far
tiles. Chosen: **both, as two tiers of one store** (`FVoxelMapTileStore`): every drawn tile keeps an overview (the image
box-filtered to a quarter of the resolution a side: 1 KB on the demo), the detail image (16 KB) is kept only within its
budget, least recently used first, and past the overview budget whole tiles go, least recently used first, to be drawn
again when looked at. Budgets: `voxel.Map.DetailBudgetMB` 32 (2,048 detail tiles) and `voxel.Map.OverviewBudgetMB` 48
(49,152 overviews), **80 MB in all**. Why:

- The world map at its default zoom draws 16 texels per tile, exactly an overview, so a fully explored island stays fully
  drawn from memory without regeneration; only the minimap's area (about 120 tiles) and a world map zoomed in past 1.5
  (at most about 1,850 tiles in view) want detail, which fits the detail budget.
- Least recently used rather than distance from the player: the map subsystem knows no player (it is used by the minimap
  and the world map alike), what is on screen is by definition what was read last, and a distance rule would evict what
  the world map shows when it pans away.
- The overview budget holds a fully explored island about 12 km across; past that, overviews are dropped and drawn again
  on demand (eight milliseconds of background work a tile).
- Exploration is a separate set and never evicted; saves keep the explored keys as before. Tiles restored from a save are
  drawn as overviews only.

**The far ocean, as built.** On island M (a 110 × 110 column grid at 4,800 uu a column) the static seabed is 17,840
triangles and the open sea 768 quads, built once in about 3 ms when the world starts; L's grid is 214 × 214 and its seabed
25,200 triangles (about 5.5 ms). The grid water is a few dozen to a few hundred quads. The first version rebuilt the whole
grid whenever a column opened or closed: about 1.3 ms a rebuild on M, on 14% of ticks at 1,500 uu/s and 34% at 3,000
(the benches below, first batch), and the cost grew with the island's area. The final version splits the grid: a
**window** around the open columns (their bounds plus 8 columns: about 31 × 31 columns, a twelfth of M's grid) is
rebuilt on every change, and the **frame** outside it only when the open columns leave the window and it moves (3 times
while S settles at its spawn, against 141 window rebuilds). Its cost no longer depends on the island's size.

**Bench method.** §14.8's routes on island M against the smooth control, both through `voxel.ConfigOverride`, with
`follow=300`: the perimeter (`orbit=0:0:200000`, 150,000 uu along the coast at R) at 1,500, 3,000 and 6,000 uu/s, the coast
crossing (`start=150000:0 dir=1:0`, ending 50,000 uu past R + F) and the chord (`start=-250000:0 dir=1:0`, 500,000 uu coast
to coast) at 3,000. PIE in the worktree editor's level viewport (1280×722), background throttle off, `vox.DeleteSave`,
clock pinned at noon, `warmup=120`, a throwaway PIE first (G14). Three batches on two builds:

- **First batch**, on a build with the first far ocean and a temporary `voxel.Temp.IslandCliff` cvar that restored the old
  edge (the flat floor 1,000 uu down, the old ring cull, no far ocean; removed before the PR): control, island M, and
  island M with the old edge. Two runs of each, the chord one for the island arms.
- **Final build**, two batches of control and island M, one run each per route. The second batch ran while a game
  controller in use elsewhere on the machine reached the unfocused PIE: the pawn wandered in its settled runs (gamepad
  input reaches a PIE without focus). The bench sets the streaming viewer and teleports the pawn every tick, so only the
  camera's rotation was exposed on the routes; the third batch blocked the player controller's move and look input after
  every PIE start and is the primary below. The two agree within run-to-run noise; the second batch's settled runs are
  discarded.

JSON reports in [Research/Bench/M6/](Research/Bench/M6/) (`m6_*` first batch, `m6f_*` second, `m6g_*` third, `*s_*_settled`
the settled runs).

**Island M against the control, final build** (far ocean = its share of the manager's tick):

| Route | Run | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | Loaded mean / peak | Peak voxel MB | Systems MB | LOD re-meshes | Far ocean P95 / max ms, rebuilds | Audit | JSON |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Perimeter v1500 | Control | 16.7 / 26.1 / 32.5 | 6.7 | 0.17 | 1,085 / 1,159 | 808 | 1,047 | 2,082 | 0.00 / 0.00, 0 | 0 | `20261016_171227_m6g_C_orbit_v1500` |
| Perimeter v1500 | Control, repeat | 16.7 / 25.5 / 31.3 | 6.7 | 0.76 | 1,103 / 1,228 | 781 | 1,021 | 2,084 | 0.00 / 0.00, 0 | 0 | `20261016_164653_m6f_C_orbit_v1500` |
| Perimeter v1500 | **Island M** | 16.7 / 26.2 / 32.2 | 7.0 | 0.92 | 1,122 / 1,213 | 825 | 1,065 | 2,067 | 0.39 / 1.74, 735 | 0 | `20261016_171437_m6g_M_orbit_v1500` |
| Perimeter v1500 | **Island M**, repeat | 16.7 / 25.6 / 31.0 | 6.9 | 0.55 | 1,123 / 1,213 | 826 | 1,067 | 2,068 | 0.37 / 2.12, 734 | 0 | `20261016_164905_m6f_M_orbit_v1500` |
| Perimeter v3000 | Control | 23.7 / 36.4 / 43.0 | 10.6 | 0.41 | 1,045 / 1,081 | 706 | 926 | 2,083 | 0.00 / 0.00, 0 | 0 | `20261016_171557_m6g_C_orbit_v3000` |
| Perimeter v3000 | Control, repeat | 21.6 / 34.4 / 42.0 | 10.0 | 0.55 | 1,049 / 1,089 | 804 | 1,042 | 2,083 | 0.00 / 0.00, 0 | 0 | `20261016_165026_m6f_C_orbit_v3000` |
| Perimeter v3000 | **Island M** | 23.1 / 36.0 / 43.3 | 10.7 | 1.07 | 1,069 / 1,166 | 793 | 1,033 | 2,066 | 0.65 / 2.02, 697 | 0 | `20261016_171720_m6g_M_orbit_v3000` |
| Perimeter v3000 | **Island M**, repeat | 22.6 / 34.7 / 39.9 | 10.6 | 0.86 | 1,059 / 1,120 | 787 | 1,016 | 2,069 | 0.64 / 4.08, 720 | 0 | `20261016_165148_m6f_M_orbit_v3000` |
| Perimeter v6000 | Control | 31.2 / 42.5 / 49.6 | 15.1 | 6.16 | 569 / 1,081 | 806 | 1,144 | 1,977 | 0.00 / 0.00, 0 | 0 | `20261016_171823_m6g_C_orbit_v6000` |
| Perimeter v6000 | Control, repeat | 31.6 / 45.5 / 53.9 | 16.4 | 7.48 | 527 / 1,065 | 803 | 1,111 | 1,996 | 0.00 / 0.00, 0 | 0 | `20261016_165251_m6f_C_orbit_v6000` |
| Perimeter v6000 | **Island M** | 31.9 / 45.5 / 53.0 | 15.9 | 6.38 | 555 / 1,123 | 841 | 1,167 | 1,974 | 0.73 / 1.52, 359 | 0 | `20261016_171925_m6g_M_orbit_v6000` |
| Perimeter v6000 | **Island M**, repeat | 31.6 / 43.3 / 49.5 | 15.5 | 7.17 | 566 / 1,160 | 824 | 1,174 | 2,015 | 0.65 / 2.77, 365 | 0 | `20261016_165352_m6f_M_orbit_v6000` |
| Coast v3000 | Control | 25.8 / 41.9 / 50.7 | 12.0 | 1.79 | 992 / 1,120 | 801 | 1,053 | 2,092 | 0.00 / 0.00, 0 | 0 | `20261016_172047_m6g_C_coast_v3000` |
| Coast v3000 | Control, repeat | 24.7 / 39.5 / 47.4 | 12.1 | 1.27 | 1,034 / 1,120 | 805 | 1,060 | 2,092 | 0.00 / 0.00, 0 | 0 | `20261016_165512_m6f_C_coast_v3000` |
| Coast v3000 | **Island M** | 16.7 / 31.9 / 41.0 | 9.0 | 0.17 | 721 / 1,107 | 823 | 1,055 | 1,432 | 0.33 / 2.40, 320 | 0 | `20261016_172208_m6g_M_coast_v3000` |
| Coast v3000 | **Island M**, repeat | 16.7 / 33.0 / 40.1 | 9.2 | 0.17 | 720 / 1,107 | 830 | 1,064 | 1,432 | 0.35 / 1.60, 320 | 0 | `20261016_165632_m6f_M_coast_v3000` |
| Chord v3000 | Control | 22.7 / 35.9 / 43.3 | 10.3 | 1.19 | 1,055 / 1,129 | 784 | 1,040 | 6,952 | 0.00 / 0.00, 0 | 0 | `20261016_172524_m6g_C_chord_v3000` |
| Chord v3000 | Control, repeat | 22.7 / 36.2 / 44.5 | 10.2 | 1.26 | 1,054 / 1,129 | 794 | 1,046 | 6,955 | 0.00 / 0.00, 0 | 0 | `20261016_165946_m6f_C_chord_v3000` |
| Chord v3000 | **Island M** | 21.2 / 33.9 / 40.8 | 9.8 | 0.17 | 1,014 / 1,129 | 776 | 988 | 6,755 | 0.45 / 1.99, 1,428 | 0 | `20261016_172837_m6g_M_chord_v3000` |
| Chord v3000 | **Island M**, repeat | 20.1 / 32.6 / 39.3 | 9.7 | 0.17 | 1,018 / 1,129 | 732 | 963 | 6,754 | 0.43 / 2.15, 1,425 | 0 | `20261016_170256_m6f_M_chord_v3000` |

**Against the §1 thresholds** (island M vs the control, mean of the two final-build runs each):

| Metric (threshold) | Perimeter v1500 | Perimeter v3000 | Perimeter v6000 | Coast v3000 | Chord v3000 |
|---|---|---|---|---|---|
| Frame P95 (+10%) | 25.9 vs 25.8 ms (+1%) ✓ | 35.3 vs 35.4 ms (0%) ✓ | 44.4 vs 44.0 ms (+1%) ✓ | 32.4 vs 40.7 ms (−20%) ✓ | 33.2 vs 36.1 ms (−8%) ✓ |
| Manager P95 (+15%) | 7.0 vs 6.7 ms (+4%) ✓ | 10.7 vs 10.3 ms (+3%) ✓ | 15.7 vs 15.7 ms (0%) ✓ | 9.1 vs 12.0 ms (−24%) ✓ | 9.7 vs 10.3 ms (−6%) ✓ |
| Catch-up (+25%) | 0.74 vs 0.47 s (+58%) ✗ | 0.97 vs 0.48 s (+101%) ✗ | 6.78 vs 6.82 s (−1%) ✓ | ends off the island (0.17 vs 1.53 s) | ends off the island (0.17 vs 1.23 s) |
| Peak loaded chunks (+25%) | 1,213 vs 1,194 (+2%) ✓ | 1,143 vs 1,085 (+5%) ✓ | 1,142 vs 1,073 (+6%) ✓ | 1,107 vs 1,120 (−1%) ✓ | 1,129 vs 1,129 (0%) ✓ |
| Peak voxel MB (+25%) | 826 vs 795 (+4%) ✓ | 790 vs 755 (+5%) ✓ | 833 vs 805 (+3%) ✓ | 826 vs 803 (+3%) ✓ | 754 vs 789 (−4%) ✓ |
| Systems MB, island M's highest (D3: ≤ 1,500 MB) | 1,067 MB ✓ | 1,033 MB ✓ | 1,174 MB ✓ | 1,064 MB ✓ | 988 MB ✓ |
| Far ocean P95 / worst tick (third batch) | 0.39 / 1.74 ms | 0.65 / 2.02 ms | 0.73 / 1.52 ms | 0.33 / 2.40 ms | 0.45 / 1.99 ms |
| Audit after settling | 0 | 0 | 0 | 0 | 0 |

**Catch-up is missed on the perimeter at 1,500 and 3,000 uu/s, by 0.3-0.5 s** (0.74 vs 0.47 s and 0.97 vs 0.48 s; the
control's own two runs are 0.17 and 0.76 s at 1,500). It is M4's miss (§14.8: the island stops with a standing generation
queue along the coast) and not M6's: in the first batch the old edge on the same build caught up in 0.78 s and 1.22 s
against the new edge's 0.77 s and 1.04 s. The routes that end off the island (coast, chord) are not comparable, as in
§14.8.

**What the edge costs** (first batch, same build; island M, old edge → M6's first far ocean, mean of two runs; the chord one):

| Route | Frame P95 ms | Manager P95 ms | Catch-up s | Peak loaded | Peak voxel MB | Systems MB | Far ocean P95 / max ms, rebuilds |
|-------|--------------|----------------|------------|-------------|---------------|------------|----------------------------------|
| Perimeter v1500 | 26.5 → 27.5 | 7.3 → 8.1 | 0.78 → 0.77 | 1,213 → 1,210 | 801 → 795 | 1,042 → 1,036 | 1.49 / 4.90, 734 |
| Perimeter v3000 | 35.6 → 36.8 | 10.7 → 11.7 | 1.22 → 1.04 | 1,166 → 1,166 | 788 → 803 | 1,013 → 1,043 | 2.33 / 5.44, 714 |
| Perimeter v6000 | 45.5 → 46.5 | 15.8 → 16.9 | 6.63 → 6.86 | 1,166 → 1,166 | 840 → 834 | 1,168 → 1,160 | 2.74 / 4.61, 378 |
| Coast v3000 | 32.7 → 34.4 | 9.2 → 10.4 | 0.17 → 0.17 | 1,100 → 1,090 | 792 → 798 | 1,025 → 1,032 | 1.39 / 4.35, 322 |
| Chord v3000 | 36.5 → 34.1 | 10.7 → 10.8 | 0.17 → 0.17 | 1,139 → 1,129 | 765 → 720 | 982 → 979 | 1.86 / 8.04, 1,408 |

The first far ocean added 0.6-1.2 ms to the manager's P95 (+14% against the control on the perimeter at 1,500 uu/s, at the
threshold), all of it the grid's rebuilds; nothing else moved beyond noise. Streaming only the columns that touch the
footprint changed peak loaded chunks by under 1%: on these routes the horizon, not the cull, bounds what loads past the
coast. With the window (final build) the far ocean's P95 is 0.33-0.73 ms and its worst tick 1.5-2.4 ms, and island M's
manager P95 is within 0-4% of the control on the perimeter.

**Settled at the spawn, and D3** (fresh world; settled = 75 s idle, then `voxel.FarCompression.Stats` and a zero-length
bench, as §14.8):

| Run | Loaded chunks | Voxel data settled (resident / uniform / compressed chunks) | Voxel systems MB | Far ocean | Audit | JSON |
|-----|---------------|-------------------------------------------------------------|------------------|-----------|-------|------|
| Control | 979 | 127.3 MB (106 / 47 / 826) | 369 | — | 0 | `20261016_173019_m6gs_C_settled` |
| **S, M6** | 1,020 | 127.7 MB (107 / 63 / 850) | **370** | 216 open columns, 20 grid quads, window 31 × 32 | 0 | `20261016_173200_m6gs_S_settled` |
| S, old edge (first batch) | 1,020 | 127.7 MB (107 / 63 / 850) | 377 | — | 0 | `20261016_163653_m6s_So_settled` |

| Budget (D3) | Measured |
|-------------|----------|
| 0.6 GB of voxel systems memory settled for a fully resident S | 370 MB (the control at its spawn: 369 MB) |
| At most 1.5 GB at peak while streaming | Island M 963-1,174 MB over its final-build runs (the control 926-1,144) |

The far ocean's meshes are not in the voxel systems total (they are the world actor's procedural mesh components, not the
renderer's): on M about 18,000 seabed triangles, 768 open-sea quads and the grid, about 2 MB with the CPU copy the
components keep.

**The views, before and after** (the same cameras on the build before M6, `m6b_*`, and after, `m6a_*`; a fresh world
with the pawn 6,000 uu inside the east coast; for the views from outside, streaming paused with what the pawn loaded):

| View | Before (M5) | After (M6) |
|------|-------------|------------|
| S, from the coast at sea level, looking out (`*_S_in_sea`) | The loaded water ends about 10,000 uu out; at sea level that is a few pixels under the horizon | Sea to the horizon |
| S, M, L from 20,000 uu past R+F at sea level (`*_out_sea`) | The island on a slab in the void: the water sheet, the 1,000 uu cliff, the floor ring and the open bottom of the loaded chunks under it | The island in a sea that reaches the horizon all round |
| S, M, L from 50,000 uu past R+F and 30,000 up (`*_out_high`) | The loaded region as a floating plate, grey void around and under it | The loaded land in a sea to the horizon |
| From 48,000 uu over the coast, looking out (`*_obl48k`) | All grey: nothing below the horizon | Sea to the horizon |
| §13's I2 survey: 48,000 uu over S's centre, straight down, oblique and toward the horizon (`*_S_i2_*`) | The island in its staircase of loaded columns, void around it; from the side a plate in the void | The whole island in the sea, no staircase; from the side two islets in a sea |
| A swimmer stalled by the push-back on S (`m6a_S_swimmer_pushback`) | (M3: the loaded water ended about 8,000 uu out) | Sea to the horizon, the edge hint on |

On M and L the views from outside show the land the pawn had loaded, surrounded by sea: the far ocean stands in for the
rest of the island, which is not streamed. From the ground inside a large island the same thing shows as sea at the
horizon past the view distance (`m6inl_all_ground`, island M from its centre); on the plane that is the void.
`voxel.World.FarOceanInland 0` leaves the unloaded interior empty instead (`m6inl_coast_*`); the default keeps it covered,
so no view shows the void.

Three artefacts found on the way and what came of them:

- **A light ring** around the island seen from above: the far ocean's water over unloaded columns inside R+F had nothing
  under it, so the fog showed through. Fixed with the grid floor.
- **A thin dark line along the edge of the loaded seabed**, seen at grazing angles: the loaded voxel seabed sat 1.5 voxels
  over the drawn one and shadowed it in a strip about that wide. The offset is now half a voxel; the line is faint.
- **Dark bands just under the horizon** at sea level, at fixed screen rows from any position: not the far ocean. With the
  far ocean hidden, the loaded water tiles over island M's coastal sea show the same bands (`mtiles_off`); fog, volumetric
  fog, anti-aliasing and the sky sphere are not involved. The water material draws them at grazing angles; until M6 no
  water reached that distance. Recorded as G16 (§14.1), cosmetic, not changed here.

**The edge, `vox.Edge` checks on S** (fresh world, water on):

| Check | Result |
|-------|--------|
| `vox.Edge` | Circle (0, 0), half extent 40,000, Z −12,000 to 10,000 (unchanged: the open-sea floor at −9,500 is inside the base bounds), 63 walls, boundary on |
| Swim east from 6,000 inside the edge at full speed | Stalls at 38,358 uu, 1,642 inside R+F, hint on (M3: 38,358) |
| Debug flight east | Stops on the wall at 39,966 (M3: 39,966) |
| Walls off (`voxel.World.Boundary 0`), fly on to 55,037, walk | Falls (nothing streams past the footprint now), "out of the world" 20,000 under the lowest terrain, body back at its last safe ground, death, respawn on land at (31,893, 4,799) |

**Map tiles, a fully explored L** (`ImportExploredTiles` with the 34,512 tiles of L's footprint, as a save restores a fully
explored world; then the drawings drain, about 70-80 s at 480 tiles a second):

| Build, budgets | Tiles with an image | Detail images | Overview images | Image memory |
|----------------|---------------------|---------------|-----------------|--------------|
| Before M6 | 34,512 | 34,512 × 16 KB | — | **539 MB** (34,512 × 16,384 B: the cache kept every image) |
| M6, budgets unlimited (`voxel.Map.DetailBudgetMB 0`, `OverviewBudgetMB 0`: every image kept, as before) | 34,512 | 34,512 (539.2 MB) | 34,512 (33.7 MB) | 573 MB |
| **M6, default budgets (32 / 48 MB)** | 34,512 | 216 (3.4 MB: the spawn's streamed area) | 34,512 (33.7 MB) | **37.1 MB** |

The world map opened at zoom 1 on that L draws the whole view from overviews (`m6a_L_worldmap`). The editor's private bytes
did not track the images (other memory is released while the tiles draw: they fell by 0.8 GB in two runs, the 539 MB run
on the old build included, and stayed flat in the default-budget run), so the table uses the map's own count of image
bytes, which `voxel.Map.Stats` now prints.
The unit test `Map.TileStore.Budget` repeats the L case without a world: 34,512 tiles restored, 121 minimap tiles in
detail, 35.6 MB.

| ID | Status | PR | Check |
|----|--------|----|-------|
| WM-E1 | Done, in review | VoxelWorlds#95 | The benches, the views and the edge checks above. Tests `Generation.IslandBowl.SeabedSlopesFromEdge` (the slope's profile, continuity at R + F for every falloff curve and both shapes, the flat floor past the slope, the height bounds reaching it; density beyond the edge matches generation), `WorldModes.FarOcean.Geometry` (the grid's water covers every column that is not open exactly once and none that is, sampled off the quads' diagonals; the open sea's rings cover the rest out to the far distance without overlap; a window build plus its frame build cover exactly what the whole grid does; undersides mirror the tops; the floor lies under the grid inside the footprint only; the seabed lies under the generated profile by the offset; inland coverage leaves deep columns open), `Streaming.Island.FarOcean` (a real chunk manager on a small island into a recording renderer, two viewer positions: no water tile under the far ocean, every other column of the box covered once across the frame and the window, the window holds every open column, and the components keep their sections through a scene proxy recreation, G15); `GPUvsCPUIslandBowlParity` gains the seabed for a circle and a rectangle at LOD 0-2; `IslandCullProbe` checks that no kept column lies outside the footprint; the review probes check no step at R + F (0-6 uu between the falloff's last sample and the seabed's first) |
| WM-E2 | Done, in review | VoxelWorlds#95, VoxelCharacterPlugin#24 | The table above. Tests `Map.TileStore.Budget` (the store alone: downsampling, both budgets held, least recently used first, a tile read all along keeps its detail, a redraw that wants detail gets it back, unlimited keeps everything; island L's 34,512 explored tiles with the minimap's 121 in detail in 35.6 MB) and `Map.TileStore.ExploredIslandUnderBudget` (the subsystem on a real island with tiny budgets: exploration kept whole, both budgets held while some tiles lose their images, a restored exploration keeps no detail, the minimap's centre tile in detail at full resolution, an evicted tile drawn again as an overview for a zoomed-out world map and in detail for a zoomed-in one). The world map (VoxelCharacterPlugin) asks for detail only when a tile spans more than 1.5 times its overview's resolution on screen, and draws evicted tiles again as they come into view |

**Suites.** Headless `VoxelWorlds+VoxelWorldSave+VoxelCharacter` 280/280. In the editor, where the GPU tests run,
280/280, every `Generation.GPUvsCPU*Parity` (the island's seabed cases included), `GPUNoiseGeneratorAsync`,
`Meshing.MarchingCubes.CPUvsGPU` and `WorldModes.InfinitePlane.GPUConsistency` among them. 275 before M6, plus five new
(`Generation.IslandBowl.SeabedSlopesFromEdge`, `WorldModes.FarOcean.Geometry`, `Streaming.Island.FarOcean`,
`Map.TileStore.Budget`, `Map.TileStore.ExploredIslandUnderBudget`). Both on the final build, with the temporary cvar removed.

**What changes in §13.** I2's "floats in a void" is gone from every view from outside and above (§13.1); I5's "walks or
swims off the floor's edge" was closed by M3's walls and the far ocean now shows what lies past them. KI2 and KI15 are fixed
(§14.1). §13.8's estimate loses the map: a fully explored island holds at most 80 MB of map images whatever its size, so the
float-precision limit (about 200 km across) is what remains for streaming; the far ocean's cost does not grow with the
island (only a window around the loaded columns is rebuilt when they change).

**Left for later.** Nothing of M6 is deferred. Next in §14.3's order is spherical Phase 1. Open and recorded:

- **G16** (§14.1): the water material's dark bands under the horizon at grazing angles. Cosmetic, the material's; the far
  ocean made them visible by taking the water to the horizon.
- **A distant island silhouette** (§6, not scheduled): with `voxel.World.FarOceanInland` at its default (−1) the far
  ocean covers every unloaded column, the interior of a large island included, so from the ground the land past the view
  distance reads as sea at the horizon instead of void; a coarse silhouette of the unloaded land would replace that.

Things to know:

- The far ocean is visual only: no collision (the walls stop everything at R + F) and nothing on a dedicated server. It
  needs `bEnableWaterLevel` and a `WaterMeshMaterial` for its water; the seabed and the floor are drawn with the terrain's
  material either way. `IslandEdgeHeight` must sit under the water level for the coast to read as sea; the configuration
  warns otherwise.
- The seabed past R + F changed from a 1,000 uu floor to a slope 6,000 uu deep: terrain there is different, but no save
  holds anything there, so saves load unchanged (no `GenerationVersion` bump, the seabed not in the fingerprint).
- The bench CSV gains `FarOceanMs` and `FarOceanRebuildN`, the JSON `farOceanMsP50/P95/Max` and `farOceanRebuilds`, and
  the run block the island's seabed and far-ocean settings.
- `voxel.Map.Stats` prints the map's image bytes (detail and overview); `voxel.Map.ExploreAll` explores every tile of a
  bounded world as a fully explored save would restore it. Map images are no longer a session-long cost to plan around.
- A renderer outside VoxelWorlds can override `IVoxelMeshRenderer::GetSurfaceMaterial` if its terrain material is not
  `GetMaterial()`; the far ocean's seabed draws with it.
- **A game controller in use elsewhere on the machine drives an unfocused PIE pawn** (XInput reaches it without focus).
  Bench and live checks should block the player controller's move and look input (`SetIgnoreMoveInput`,
  `SetIgnoreLookInput`) or confirm the pawn holds still.
- One editor crash on the RHI thread (`(OldPacked & DeletingBit) == 0`, "Resource is being deleted"), right after the far
  ocean and map tests ran in an editor launched seconds before. Not reproduced in four runs of the same tests afterwards,
  in the full in-editor suite or in the 60-odd PIE sessions of the benches; recorded in case it comes back.

## 15. Spherical Phase 1: a correct planet on the CPU (2026-10-16)

The requirements doc's Phase 1 ([SPHERICAL_PLANET_REQUIREMENTS.md](SPHERICAL_PLANET_REQUIREMENTS.md) §4): SP-A1, SP-A2,
SP-B1, SP-B2, SP-B5 to SP-B8, SP-C1, SP-C2, SP-D4 and SP-I2, after M6 by D9 (§14.4). CPU generation only: GPU parity
and the GPU default for planets (SP-B4, SP-I3) are Phase 2, walking (SP-F*) is Phase 3. Exit check: a survey camera
shows a seamless, unmirrored planet with sensible materials and water at all latitudes.

### 15.1 Plan

Work items (IDs `WM-P*`, stable):

| ID | Work item | Requirements | Repos | Size |
|----|-----------|--------------|-------|------|
| WM-P1 | **One radial height.** `FSphericalPlanetWorldMode` owns the planet's surface function: noise and continentalness sampled by 3D direction at the point on the datum sphere (`Direction × PlanetRadius`, planet-relative), and continentalness modulating the displacement as in the heightmap modes (height offset and height-scale multiplier). Generation, a new `IVoxelWorldMode::GetSurfacePoint` (the surface point and normal on the up-line through a position; on a planet, along the direction from the centre) and the config's shell check, which now counts continentalness's extremes, all use it | SP-A1, SP-B1, SP-B5 (SP-A5's check) | VoxelWorlds | M |
| WM-P2 | **The surface coordinate's warp:** the tangent warp on each cube face (below) | SP-A2 | VoxelWorlds | S |
| WM-P3 | **Materials and caves.** Height-material rules read the voxel's altitude above the datum; temperature and moisture are sampled like the terrain (by direction, in world units); caves apply `VerticalScale` along local up, and get `bIsUnderwater` from the sea radius | SP-B2, SP-B6 | VoxelWorlds | S |
| WM-P4 | **Underground classification along local up** (below) | SP-B7 | VoxelWorlds | S-M |
| WM-P5 | **Water by the sea radius:** a radial fill pass in generation, and the chunk manager's cross-chunk propagation and the edit-time propagation by radius; the Z-plane water tiles are off on planets | SP-B8 | VoxelWorlds | M |
| WM-P6 | **The shell window and the horizon.** `FDistanceBandLODStrategy::GetColumnWindow`'s planet branch becomes the column's crossings of the shell; the horizon cull adds the mountain term | SP-C1, SP-C2 | VoxelWorlds | M |
| WM-P7 | **Tests:** SP-I2's list, plus the automated half of SP-D4 (collision winding at 45°, a closed surface at 45° from a real chunk manager) | SP-I2, SP-D4 | VoxelWorlds | M |
| WM-P8 | **Live verification and benches:** the exit check before and after, SP-D4's checks at 45°, benches against the control | SP-D4 | VoxelWorlds (docs), parent (review content) | M |

Order: WM-P1 and WM-P2 (independent), then WM-P3, WM-P4 and WM-P5 (the generator, one commit each), WM-P6, WM-P7's
streaming tests, WM-P8. Unit tests land with each item. PRs: VoxelWorlds, generation (WM-P1 to WM-P5) and streaming
(WM-P6, the streaming tests and these docs) stacked on it; then UE5_PluginDev (a review planet config with water, its
script, the gitlink and the doc map). No game-side change is needed.

**Design (written before building).**

| Question | Choice | Why |
|----------|--------|-----|
| The shell window's shape | Per column, the layers whose chunks meet the shell from `R − MaxTerrainDepth − ½W` to `R + MaxTerrainHeight + ½W` (W a chunk's width): one interval, or two when the column passes inside the inner radius (one crossing on each side of the centre), clipped to the viewer's 3D view distance. Load and keep windows are the same; there is no near window | M4's per-column design (D8) with the shell as the column's range. The interval is exact for the column's footprint (its nearest and farthest points from the planet's axis bound the crossing), so it covers every latitude; the old box was capped at ±16 layers around the viewer, which at the equator, where world Z is horizontal, cut the reach short. The shell holds everything that generates, caves included, so no near window is needed. The ½-chunk slack loads the neighbours a seam job reads next to a peak or valley at the shell's edge. A band around the analytic surface (SP-C5) would hold fewer chunks; it is P2, and the benches measure what the shell costs |
| Noise sampling | Terrain, continentalness, temperature and moisture: `FBM3D(Direction × PlanetRadius)`, relative to the planet centre. Caves: the voxel's position with its altitude scaled, `Centre + Direction × (R + Altitude × VerticalScale)` | The point on the datum sphere carries world units: a feature is about 1/Frequency uu across on any radius, as on the plane, and a 3D field has no seam and no mirror. The plane's `(X, Y, 0)` and the sphere are both 2D sections of the same 3D field, so the terrain keeps its character. Caves flatten along local up and keep their horizontal scale at the datum (within 1% for a cave 3,000 uu down on a 250,000 uu planet) |
| Equal-area warp | **Switch now** to the tangent warp: on each face `u = (4/π)·atan(g)` per axis, g the gnomonic coordinate; UV stays in [−1, 1] | Equal UV cells cover areas within 1.41× of each other (gnomonic: 5.2×); the inverse is closed-form and exact; it is continuous at face edges. Nothing consumes the coordinate yet, but Phase 4's map tiles, nav tiles and explored-tile save keys will, and a change then would invalidate saves. An exactly equal-area map (a Lambert cube) costs more per query and no consumer needs exact areas |
| Water fill | Generation: the plane's three phases with the sea sphere in place of the water plane. Chunks the sea surface crosses seed along digital lines in the chunk's local down direction, open from the line's top (as a column is open from the chunk top); a BFS spreads through non-cave air inside the sea radius; the cave flags clear. The chunk manager's cross-chunk propagation and the edit-time propagation take the same test (`FVoxelSeaSurface`, VoxelCore). The Z-plane water tiles skip planets; the ocean surface stays the test actor's water sphere at `WaterRadius` until SP-E2 (Phase 3) | The plane's rules carry over (the open sea fills, caves stay dry, chunks below the surface fill from their neighbours), with the comparison against a Z plane replaced by the radius. The tiles are XY grids at the Z-plane level and would draw in the wrong place on a planet |
| Underground classification | The column scan's state machine on digital lines along the chunk centre's local down: lines sheared along the axis nearest to down, so every voxel is on exactly one line. Heightmap modes keep the column scan unchanged | At mid latitudes local down is up to 55° off every axis, and a world-Z scan reads hillsides as cave ceilings. Heightmap output stays bit-identical, so the GPU post-pass parity holds; the GPU port for planets is Phase 2 (`VoxelPostPasses.usf`) |
| Horizon | Cull a chunk whose centre is farther than `√(2R·hv + hv²) + √(2R·Hmax + Hmax²) + ½√3·W` from the viewer (hv the viewer's altitude, at least 0; Hmax = `MaxTerrainHeight`); no cull for a viewer below the shell | SP-C2: a peak of height Hmax shows over the horizon until the two tangent distances add up. The old buffer (+Hmax) cut mountains just past the viewer's own horizon, and a viewer at or below the datum got no cull at all |

**How each is verified.**

| ID | Check |
|----|-------|
| WM-P1 | Tests: analytic against generated surface by direction (poles, equator, 45°, cube-face edges and corners): the generated isosurface along the radius within a voxel of `GetSurfacePoint`; the two hemispheres differ; no step across face edges; a feature's size in uu does not depend on the radius; the shell check counts continentalness |
| WM-P2 | Round trip on every face; equal UV cells within 1.5× in area |
| WM-P3 | A height rule applies by altitude (snow above its altitude, not everywhere); the live survey |
| WM-P4 | A planet chunk at 45° with a cave under the ground: underground flags in the cave and none in the open air; the heightmap suites unchanged |
| WM-P5 | A coastal chunk at 45°: water flags inside the sea radius only, caves dry; propagation by radius; live, water at every latitude |
| WM-P6 | The candidates cover every chunk that meets the shell within the view distance at the pole, at 45° and on the equator (against brute force); the horizon cull keeps a peak past the viewer's own horizon; the benches |
| WM-P7 | Collision faces point away from the planet at 45°; a real chunk manager streaming a small planet at 45°, LOD on, renders a closed surface (no open edge but at the loaded region's edge) |
| WM-P8 | Survey screenshots before (this base) and after, at the poles, the equator, 45° and across cube-face edges; at 45°, crossfades and first-appearance fades running and a physics body landing on the slope; benches: the planet on its equator and its 45° and 80° circles of latitude at a fixed altitude against the control at a fixed altitude, one build, one batch, a throwaway PIE first (G14), settled at the spawn, against D3; headless and in-editor suites |

### 15.2 Results (2026-10-16)

All eight work items are implemented and verified; PRs open (draquel/VoxelWorlds#96 generation, WM-P1 to WM-P5;
draquel/VoxelWorlds#97 streaming, WM-P6, WM-P7 and these docs, stacked on it; draquel/UE5_PluginDev#108 content,
script, gitlink and doc map). Built from VoxelWorlds `feature/sp1-planet-streaming` (on `feature/sp1-planet-generation`,
base `ad68236`) and UE5_PluginDev `claude/spherical-p1-cpu-planet-952556` (base `4867ac5`), editor 5.8.3 from
`D:\Program Files\Epic Games\UE_5.8`, in a worktree editor with background throttling off. Live checks ran on
`PlanetTest` with `/Game/PluginTesting/Review/Review_Planet` through `voxel.ConfigOverride`: `PlanetWorldConfig` (a
250,000 uu planet from the demo config: its noise, biomes, caves and scatter) with water on at `WaterRadius` 249,500
(500 under the datum, as the demo's water sits 500 under its sea level) and a ±6,500 shell (the demo's continentalness
curves take the surface to ±6,000 now that they modulate the planet; `PlanetWorldConfig` is resaved with the same shell).
Screenshots: `Saved/Screenshots/Claudius/p1b_*` (before, on the unchanged build) and `p1a_*` (after) in the worktree (not
committed).

**As built.** 15 new automation tests (`VoxelWorlds.WorldModes.Planet.*` 10, `VoxelWorlds.LOD.Planet.*` 3,
`VoxelWorlds.Streaming.Planet.*` 2); the numbers below are theirs.

| ID | As built | Check |
|----|----------|-------|
| WM-P1 | `FSphericalPlanetWorldMode::SampleRadialDisplacement`: `FBM3D` at `GetNoiseSamplePoint(Direction) = Direction × PlanetRadius`, continentalness through the heightmap modes' `ComputeEffectiveTerrainParamsAt` (now taking a 3D sample point; the XY overload is unchanged). Generation, `GetDensityAt` (`ShellSignedDistance`), the new `IVoxelWorldMode::GetSurfacePoint` and the configuration's shell check (`GetDisplacementBounds`, through `FVoxelBiomeSnapshot::GetContinentalnessExtremes`) all call it. `GetSurfacePoint` returns `FVoxelTerrainSurfacePoint` (position, normal, altitude): heightmaps `(X, Y, GetTerrainHeightAt)` with a central-difference normal; the planet the surface on the line from the centre, its normal from central differences along two tangents. Blueprint: `UVoxelChunkManager::GetSurfacePointAt`. The save fingerprint gains `PlanetGeneration=2` (planets only), so a save from the mirrored planet is refused and kept | `SurfaceMatchesGeneration`: generated isosurface against `GetSurfacePoint` along the radius, 16 directions (poles, equator, 45°, face edges and corners, 23 chunks): max gap 0.81 uu. `NoiseSeamless`: 190 of 190 mirrored pairs differ (mean 947 uu); across cube-face edges the worst step between points 200 uu apart is 65.6 uu (59.1 inside faces); sign changes over 400,000 uu: 18 at R 250,000, 17 at R 1,000,000, 13 on the plane (feature size in world units, not tied to the radius). `SurfacePoint`: 200 of 200 normals perpendicular to the sampled surface. `Continentalness`: 592 of 600 directions modulated, displacement −2,835 to +2,000 inside the bounds −6,000 to +4,000. `Configuration.PlanetSettings` extended (the shell check counts continentalness) |
| WM-P2 | The tangent warp in `WorldToSurface` / `SurfaceToWorld`: `u = (4/π)·atan(g)` per axis, UV in [−1, 1] | `SurfaceCoordWarp`: UV cells (32 × 32 a face) within 1.380× in area (gnomonic: 5.2×), total solid angle 4π; `Api.Frame`'s round trip on all six faces |
| WM-P3 | Height-material rules get the voxel's altitude above the datum; temperature and moisture are sampled at the datum-sphere point; caves flatten along local up (`FSphericalPlanetWorldMode::ScaleAltitude`, through `SampleCaveLayer`) and get `bIsUnderwater` when the column's terrain radius is under `WaterRadius` | `HeightMaterialsByAltitude` (133 chunks): surface voxels at altitude ≥ 4,000: 72 of 72 snow; below: 0 of 865 (before: snow everywhere). `Caves`: 139,968 carved voxels on dry land; under the sea with `UnderwaterMinDepth` 60: 0 |
| WM-P4 | The underground pass's state machine on digital lines along the chunk centre's local down (`ForEachVoxelAlongDown`: lines sheared on the axis nearest to down, a bijection of the chunk's voxels); heightmap modes keep the column scan (bit-identical, so the GPU post-pass parity holds) | `UndergroundAlongLocalUp`: at 45°, a cave under the ground: 93 of 93 voxels flagged (world-Z columns: 0), open air 0 of 8,832; at the pole the lines are world Z: 1,702 underground voxels, 0 differ from the column scan |
| WM-P5 | `FVoxelSeaSurface` (VoxelCore): the `WaterLevel` plane, or the sphere of `WaterRadius` on a planet. Generation's `ApplyRadialWaterFillPass` (seed along local down in chunks the sphere crosses, BFS inside the radius, cave flags cleared); the chunk manager's cross-chunk propagation and `FVoxelWaterPropagation` (edit time) take the same test. The Z-plane water tiles are off on planets (`UVoxelChunkManager::UsesWaterTiles`); the test actor draws the sea as a tangent-warped cube sphere at `WaterRadius` (6 × 96 × 96 quads, a procedural mesh; before, a configured `WaterMeshMaterial` skipped the sphere and nothing was drawn) | `SeaSurface`; `WaterFill` at 45°: sea 9,432 of 9,432 air voxels wet, sky 0 of 16,616, cave 0 of 93; `ClosedSurface`: 77,913 to 96,836 water voxels in 52 to 63 chunks, 0 above the sea radius |
| WM-P6 | `FDistanceBandLODStrategy::GetPlanetShellWindow` (the column's crossings of the shell ± ½ chunk, one or two intervals, clipped to the view distance) replaces the planet's viewer-relative window; `ShouldCullBeyondHorizon` culls against the window's radii and adds the mountain term (`GetPlanetHorizonDistance`) | `ShellCoverage`, against brute force at the pole, 45°, the equator and a cube corner, view 40,000 and 100,000: 812 to 5,282 chunks meet the shell in range, 0 missing, 0 candidates outside the shell window; the window reaches 2 to 20 layers from the viewer's (the old window was capped at 16). `ColumnWindow`. `HorizonCull`: a viewer 2,000 up sees peaks to 89,064 uu (old buffer 38,186): 100 chunks past the old buffer kept, 213 past the horizon culled; at the datum and 500 under it, 57,378 (the old code: 6,500, and no cull below the datum) |
| WM-P7 | Streaming tests on a real chunk manager (CPU generation, seam path, LOD on) | `ClosedSurface`: a 20,000 uu planet streamed at the north pole, 45° and the equator, chunk 32, voxel 50, three LOD bands (136 to 158 chunks: 7 to 9 at LOD 0, 51 to 61 at LOD 1, 76 to 90 at LOD 2): no open edge but at the loaded region's edge (298 to 331), in T-junctions (0 to 10) and **G17**'s gaps at mixed-LOD face seams (5 to 6 a run, below); 0 other holes. `CollisionWindingMidLatitude`: chunk (73, 0, 73) at 45°: 1,734 of 1,734 faces wound outward, in the physics order and in the body |
| WM-P8 | Live checks and benches (below) | — |

Headless `VoxelWorlds+VoxelWorldSave+VoxelCharacter` on the PR code: **295 of 295** (280 before plus the 15), 12 GPU tests
skipping under `-nullrhi`. In-editor (real RHI) the same three suites: **295 of 295**, the GPU tests running (the GPU vs
CPU generation parity tests, `Meshing.MarchingCubes.CPUvsGPU`, `Generation.GPUNoiseGeneratorAsync` and the rest); only
the two retired GPU DC interior tests skip, as before.

**The exit check: the survey.** Nine survey cameras over `Review_Planet` (12,000 uu up and 20,000 back, looking 30° down,
unless noted), before on the base build and after (the after shots in a larger PIE viewport):

| View | Before | After |
|------|--------|-------|
| North pole | Sand and gravel (the underwater materials: the cap lies under `WaterRadius`) in concentric arcs around the pole, a stepped silhouette (the old window cut the surface at chunk layers), no sea drawn | An island and its coast in the sea sphere, grass, dirt and sand by biome |
| South pole, camera mirrored | Too dark to read (below); the silhouette repeats the north pole's | Different terrain (a coast and a peninsula); dark, as the sun is fixed overhead and the south pole is on the planet's night side (no planet-aware sky until SP-G8) |
| 45° | Snow everywhere, a stepped silhouette, no sea | A peninsula with forest, coasts at sea level, a second coast across the bay |
| 45°, low (3,000 up) | Snow | Coast, beach and forest close up |
| Equator | Snow, a stepped silhouette | Hills, two bays, beaches, grass and dirt |
| −45°, longitude 120 | Snow | Grass and rock on a ridge, the sea on one side |
| Cube-face edge (equator, longitude 45) | Snow | Grass and dirt, a pond; no step or seam along the face edge |
| Cube corner (35.26°, 45) | Snow | Two coasts across a strait; no seam at the corner |
| Equator, 30,000 up looking down | Snow to the loaded region's edge | The loaded region's land and sea; past the view distance the translucent sea shows the sky behind it (no far terrain; see below) |

The terrain is continuous across every face edge and corner surveyed, the hemispheres differ, materials follow altitude
and biome, and the sea's coasts sit where the terrain crosses `WaterRadius` at every latitude. **Exit check met.**

**SP-D4 at 45°, live** (pawn 600 uu over the surface at latitude 45, longitude 3, after the region streamed: 1,283
chunks loaded, 527 meshed): crossfades and first-appearance fades both run and none restarts (crossfades: interior 184,
seam bucket 33,525, 0 mid-fade restarts each; appearance fades: interior 7,886, seam bucket 8,062, 0 active when
settled). Radial line traces through the surface at the pawn and 1,000 to 8,000 uu around it hit the terrain's collision
from above with normals 0.83 to 1.00 along the outward radius. Three physics spheres (radius 100) dropped 1,500 uu over
the surface at longitudes 0, 3 and 6 land and roll down the 53° slope (gravity is world −Z until SP-F1) with their
centres 100 uu over the surface, the sphere's radius, at every sample. Seam closure at 45° is `ClosedSurface`'s.

**Benches.** The control (`DemoSmoothWorldConfig`) flies 150,000 uu along +X at z 7,200
(`start=-250000:0:7200 dir=1:0`). The planet flies 7,200 over its datum (radius 257,200) on circles of latitude, using
the bench's `orbit` route: the equator (a great circle), 45° and 80° (a circle of radius 44,663 uu, a tight turn), 150,000 uu
each. Both run at 1,500 and 3,000 uu/s. PIE ran in the worktree editor's level viewport (3440×1408) with background
throttling off, `vox.DeleteSave`, the clock pinned at noon, the player controller's move and look input blocked, and
`warmup=120`. Everything ran as one batch on one build, with a throwaway PIE first (G14). That build carried a temporary
`voxel.Temp.PlanetLegacyWindow` cvar, which restored the planet's old viewer-relative window and horizon (removed before
the PR). The control generates on the GPU and the planet on the CPU (SP-B3). So two more pairs ran after the batch in the
same editor session, on a CPU-generation copy of the control (`bUseGPUGeneration` off, not committed): the straight
route, and the 80° circle's radius at z 7,200. JSON reports in [Research/Bench/SP1/](Research/Bench/SP1/) (`p1b_*` the batch,
`p1bs_*` settled, `p1c_*` and `p1d_*` the CPU control).

| Route | Run | Frame P50 / P95 / P99 ms | Manager P95 ms | Catch-up s | Gen queue mean / peak | Loaded mean / peak | Peak voxel MB | Systems MB | LOD re-meshes | Generated / s, queue full | Audit | JSON |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Straight, v1500 | Control (GPU) | 16.7 / 24.1 / 31.3 | 6.7 | 0.17 | 10 / 89 | 1,140 / 1,224 | 823 | 1,014 | 1,860 | 113 | 0 | `20261016_202841_p1b_C_fly_v1500` |
| Straight, v1500 | Control (CPU) | 16.7 / 21.5 / 26.1 | 4.7 | 1.37 | 32 / 89 | 1,113 / 1,168 | 326 | 525 | 1,861 | 32 | 0 | `20261016_205729_p1c_K_fly_v1500` |
| Equator, v1500 | **Planet** | 16.7 / 19.2 / 23.3 | 4.3 | 1.77 | 30 / 90 | 1,077 / 1,154 | 308 | 550 | 1,462 | 29 | 0 | `20261016_203111_p1b_P_eq_v1500` |
| 45°, v1500 | **Planet** | 16.7 / 20.1 / 24.1 | 4.4 | 1.24 | 38 / 99 | 1,153 / 1,234 | 323 | 628 | 1,653 | 30 | 0 | `20261016_203347_p1b_P_45_v1500` |
| 80°, v1500 | **Planet** | 16.7 / 22.4 / 26.9 | 4.9 | 2.66 | 32 / 95 | 1,095 / 1,164 | 305 | 621 | 1,633 | 29 | 0 | `20261016_203621_p1b_P_80_v1500` |
| Circle r 44,663, v1500 | Control (CPU) | 16.7 / 24.0 / 30.4 | 5.6 | 4.68 | 75 / 160 | 1,043 / 1,186 | 332 | 601 | 2,040 | 27 | 0 | `20261016_210255_p1d_K_orb44_v1500` |
| Straight, v3000 | Control (GPU) | 17.6 / 27.7 / 32.6 | 7.7 | 0.42 | 30 / 92 | 1,118 / 1,180 | 799 | 991 | 1,861 | 84 | 0 | `20261016_203741_p1b_C_fly_v3000` |
| Straight, v3000 | Control (CPU) | 16.7 / 23.9 / 28.7 | 5.5 | 24.57 | 487 / 752 | 668 / 1,140 | 328 | 571 | 1,860 | 30 | 0 | `20261016_205938_p1c_K_fly_v3000` |
| Equator, v3000 | **Planet** | 16.7 / 22.5 / 26.7 | 4.9 | 20.09 | 388 / 610 | 724 / 1,154 | 319 | 628 | 1,462 | 29 | 0 | `20261016_203939_p1b_P_eq_v3000` |
| Equator, v3000 | Planet, old window | 16.7 / 23.3 / 28.0 | 6.0 | 28.29 | 578 / 865 | 789 / 1,368 | 360 | 610 | 1,764 | 30 | 0 | `20261016_204612_p1b_L_eq_v3000` |
| 45°, v3000 | **Planet** | 16.7 / 23.6 / 27.7 | 5.2 | 24.69 | 474 / 736 | 725 / 1,234 | 330 | 672 | 1,653 | 29 | 0 | `20261016_204145_p1b_P_45_v3000` |
| 45°, v3000 | Planet, old window | 16.9 / 24.6 / 29.2 | 6.1 | 30.57 | 646 / 920 | 721 / 1,374 | 333 | 671 | 1,794 | 28 | 0 | `20261016_204829_p1b_L_45_v3000` |
| 80°, v3000 | **Planet** | 16.8 / 27.5 / 33.8 | 6.5 | 24.35 | 464 / 723 | 673 / 1,165 | 292 | 654 | 1,633 | 26 | 0 | `20261016_204354_p1b_P_80_v3000` |
| Circle r 44,663, v3000 | Control (CPU) | 16.8 / 26.7 / 32.4 | 6.3 | 29.05 | 519 / 866 | 613 / 1,181 | 311 | 616 | 1,974 | 26 | 0 | `20261016_210506_p1d_K_orb44_v3000` |

"Generated / s, queue full" is the chunks generated per second while the generation queue held 50 or more (8 jobs in flight
in every run).

**Settled at the spawn** (the north pole for the planet; `warmup=180`, then 75 s, then a 30 s settled run):

| Run | Loaded | Resident / uniform / compressed | Resident voxel MB | Systems MB (voxel, render GPU, collision, scatter) | Audit |
|---|---|---|---|---|---|
| Control (GPU) | 979 | 107 / 47 / 825 | 128 | 356 (139, 23, 103, 90) | 0 |
| **Planet** | 1,007 | 96 / 200 / 711 | 113 | 383 (133, 19, 91, 93) | 0 |
| Planet, old window | 1,292 | 112 / 311 / 869 | 131 | 418 (160, 19, 104, 93) | 0 |

Against the GPU control and §1's thresholds:

- **Catch-up: missed on every route.** At v1500 the planet takes 1.24 to 2.66 s against 0.17 s, and at v3000 20.1 to 24.7 s
  against 0.42 s. It passes the rest: frame P95 (19.2 to 27.5 ms against 24.1 / 27.7; 80° at v3000 is level with the
  control), manager P95 (4.3 to 6.5 against 6.7 / 7.7), peak loaded chunks (at most +5%) and memory (peak voxel MB 292
  to 330 against 799 to 823).
- **The miss is CPU generation, not the planet.** With its queue full the planet generates 26 to 30 chunks a second, the
  same as the control on the CPU (26 to 32), while the control's GPU generator manages 84 to 113. The control on the CPU
  misses the same way: 1.37 s and 24.6 s. Phase 2's GPU planet (SP-B4) is the fix; nothing in Phase 1 tunes it.
- **Against the CPU control, the planet is within §1's thresholds on the equator and at 45°**, with one exception:
  catch-up on the equator at v1500 is 1.77 s against 1.37 s (+29%, 0.4 s, a miss by 4 points). 45° catches up faster at
  v1500 (1.24 s) and level at v3000 (24.7 s against 24.6), and the equator faster at v3000 (20.1 s). Frame P95 and
  manager P95 are below the CPU control's at both speeds. Peak loaded chunks are within +8% and systems MB +5 to +20%,
  both within +25%.
- **The 80° circle is the turn, not the latitude.** Against the straight CPU control, 80° at v3000 misses frame P95 by
  +15% and manager P95 by +18%, and at v1500 catch-up (2.66 s against 1.37). The CPU control on a circle of the same radius does as badly or worse: frame P95 26.7 ms,
  manager P95 6.3 ms, catch-up 29.1 s, and at v1500 a catch-up of 4.68 s against the planet's 2.66. A tight orbit turns
  the loaded region round quickly. Against that circle the planet's 80° runs are within +3% on frame and manager P95,
  and catch up faster.
- **The shell window against the old one** (the A/B, v3000): the old viewer-relative window queues more (peak 865 to 920
  against 610 to 736) and catches up 6 to 8 s later. It loads up to 19% more chunks at peak (1,368 to 1,374 against 1,154
  to 1,234), re-meshes 9 to 21% more for LOD, and holds 1,292 chunks settled at the pole against 1,007 (418 MB against
  383). The old window took the viewer's layer ± up to 16 in every column, so it loaded air and core the shell does not
  meet. The shell window loads more layers where the surface needs them: 16 layers across the equator (−8 to 7), against
  the control's 7.
- **D3: met.** Systems memory peaks at 550 to 672 MB while streaming (at most 1.5 GB) and is 383 MB settled (at most
  0.6 GB). The residency audit finds 0 on every run.

**G17 (new, §14.1): small holes in DC face seams between chunks of different LOD.** `ClosedSurface` found pentagonal
gaps (two open-edge loops of five or six edges, a few voxels across) where a face seam joins chunks at LOD 1 and 2:
north pole (1,1,12)|(2,1,12), 45° (7,6,7)|(8,6,7), equator (12,−1,−2)|(12,−1,−3), 5 or 6 edges each, and with two
LOD bands at 0|1 (four edges at latitude 45, longitude 45). Not planet-specific: an InfinitePlane chunk manager with
chunk 16, voxel 100 and three bands has the same, 5 edges at a 1|2 face seam. The seam is ready and clean and its LOD pair
current when measured, so it is the face seam's geometry, not scheduling; MS1's mixed-LOD face seam unit tests cover 0|1,
1|0 and 0|2 on flat and sloped binary solids only, not 1|2 or noise terrain. Not fixed here: the test reports G17 holes
and fails on any other hole, so a fix makes it strict. *Fixed since (§15.3), and the test is strict.*

**Things to know.**

- A planet save from before Phase 1 is refused and kept (`PlanetGeneration=2` in the fingerprint); heightmap saves are
  unaffected.
- The surface point type is `FVoxelTerrainSurfacePoint` (VoxelCore); `FVoxelSurfacePoint` is the scatter's.
- Physics bodies on the planet fall along world −Z until SP-F1: away from the poles they roll toward the equator, and
  fall through once they leave the collision radius around the viewer (20,000 uu here).
- In a survey the pawn falls out of the world when the camera streams elsewhere (the pole's chunks unload), and the
  respawn resets the view target: put the pawn in flying mode before each shot.
- Faint lines along chunk-layer boundaries in the survey are older than Phase 1: the plane shows them on its slopes
  (M6's island shots).

**Left for later.**

- **GPU** (Phase 2): the GPU generator's planet branch, parity at LOD 0/1/2, and the underground post-pass along local
  down in `VoxelPostPasses.usf`; planets keep generating on the CPU (SP-B3's warning).
- **The shell's cost.** The shell window streams every chunk the shell meets within the view distance, ±6,500 here: about
  1,007 settled at the spawn against the control's 979 (benches). A band around the analytic surface
  (SP-C5, P2) would hold fewer.
- **What the planet does not draw yet:** past the view distance the loaded region ends against the translucent sea
  sphere (the sky shows through it from high up); no far terrain or atmosphere (SP-C5, SP-G8).
- Walking (Phase 3): gravity toward the centre, movement, spawn (`GetPlanetSpawnPosition` still ignores displacement),
  KillZ; the pawn lands in the sea at the north pole now that the terrain is right.
- POIs still sit inside the planet (SP-G5, Phase 5); scatter, map, navigation and NPCs still assume +Z (Phase 4).
- `GetTerrainHeightAt(X, Y)` on a planet is still the legacy equatorial query (SP-A3, Phase 4).
- G17 (above): fixed in its own session (§15.3).
- **The sea sphere's light and dark halves.** Where a view crosses the sphere's cap twice (in and out, past the loaded
  terrain: the survey's 45° and south-pole views), part of the sea reads lighter and part darker, split along one grid
  row (near y = 0 there). The water material (`MI_Ocean`) is two-sided translucent and the sphere is one unsorted mesh, so
  in part of the view its far side blends over its near side. The sun (straight down) and shadows (the sphere casts none)
  are not involved, and world-space UVs and tangents on the sphere did not change it. Cosmetic, on the test actor's
  stand-in sea; SP-E2 (Phase 3) replaces it.

### 15.3 G17: holes in mixed-LOD seams (2026-10-16)

Fixed in draquel/VoxelWorlds#98 (`fix/g17-mixed-lod-face-seam`, base `b73bd25`, main after spherical Phase 1; the
parent's gitlink in draquel/UE5_PluginDev#109). Built with editor 5.8.3 from `D:\Program Files\Epic Games\UE_5.8` in a
worktree.

**Cause.** The mixed-LOD seam meshers (the face seam of P2c, the edge and corner seams of P2d;
`Research/SEAM_OWNERSHIP_ARCHITECTURE.md` §2.5) stitch at the finest stride of their participants: every fine
crossing gets a fan onto the cells of the coarser side. A coarse cell gets a vertex only from crossings on its own
twelve edges, sampled at its own stride. Where the surface pokes through a coarse cell's face between those samples (a
small bump or dimple crossing the face plane, a hilltop or a ridge grazing it), all the coarse cell's corners have one
sign: it has no vertex, the fan onto it was skipped ("a crack, never a protrusion"), and the fine side's quads around
the crossing had nothing to meet. That is the hole: two to six open edges, the 5 to 6 a run §15.2 measured. Scheduling
was never involved (§15.2 found the seams ready, clean and current), and nothing is planet-specific: noise terrain with
features a few voxels across, at any face orientation.

**Fix.** `ComputeBoundaryFallbackVertex` (`VoxelCPUDualContourMesher.cpp`): a coarse cell a fan needs that has no
crossing at its own stride gets a vertex from the same QEF solve over every voxel-resolution (stride-1) crossing on its
six faces, normals at stride 1, the position inside the coarse cell's bounds (surface-derived and in its own cell, so
the #44 laws hold). The three mixed meshers call it, lazily, for exactly the cells they used to skip.
- It only ever gives a vertex to a cell that was invalid. Interior passes and same-LOD seams only emit valid cells, so no
  vertex they emit moves and the ring bit-identity contract (SC2, MS2, ES2, CS2, MT2) is untouched; same-LOD seams'
  triangle counts are unchanged.
- It depends only on the sampler view, the cell and its stride, not on the calling job's stitch stride, so the face, edge
  and corner jobs that fan onto the same cell solve the same vertex.
- Any finer crossing on a cell's boundary implies a stride-1 crossing there, so a fan never finds its cell missing; by
  the parity argument around each primal face the fans then close.
- CPU only. The seam meshers have no GPU twin (§7.2 option C of the seam doc: interiors and seams are CPU DC), so GPU
  parity is unaffected; no shader or shared GPU code changed.

**Tests.**
- `Meshing.DualContour.SeamClosure.MS4_MixedFaceClosureNoise` (new): interior A + interior B + their face seam on noise
  terrain (a rolling heightfield with small 3D bumps, density as the generators write it), 24 patches on each axis for
  LOD 0\|1, 1\|0, 1\|2, 2\|1, 0\|2 and 2\|0, and a 1\|1 control.
- `Meshing.DualContour.SeamClosure.MT3_MixedTupleClosureNoise` (new): 2 × 2 × 2 lattices on the same terrain (split
  1\|2 on X, 2\|1 on Z, 0\|1 on Y, and LOD 0, 1 and 2 mixed), every interior, face, edge and corner mesh, closed
  everywhere inside the lattice.
- `Streaming.Planet.ClosedSurface` is strict: a G17 gap fails like any other hole. Two fixes to how it counts. It welds
  vertices within 0.01 uu instead of rounding them to a 0.01 grid: a seam bucket's ring vertex is solved in the
  neighbour's frame and shifted by a chunk, so it reached the check up to 0.00009 uu from the neighbour's own copy, and a
  vertex on a rounding boundary split in two (the last two "gaps" left after the fix, and every T-junction the test had
  counted). And an open edge is at the loaded region's edge when either end, not only its midpoint, lies within 8 voxels
  of a chunk that is not loaded: a seam missing a participant leaves open the quads that needed its cells, and a long
  LOD 2 edge's midpoint can lie farther out (850 uu against the 800 reach on the plane).
- `Streaming.Plane.ClosedSurface` (new): the same check on an InfinitePlane with chunk 16, voxel 100 and the planet's
  three LOD bands, from three viewers (216, 207 and 216 chunks: 8 to 9 at LOD 0, 60 to 78 at LOD 1, 120 to 148 at LOD 2).

**Results.** "Before" is the same build with the fallback switched off (a temporary edit, reverted).

| Run | Before | After |
|-----|--------|-------|
| `MS4`, 432 mixed face seams | 242 open edges in 46 seams: 0\|1 51, 1\|0 40, 1\|2 46, 2\|1 14, 0\|2 67, 2\|0 24 (1\|1 control: 0) | 0 |
| `MT3`, 24 lattices, fallback off in the edge and corner meshers only | 24 open edges in 3 lattices (1\|2 5, 2\|1 6, mixed 13) | 0 |
| Planet, north pole | 5 G17 edges at (1,1,12)\|(2,1,12), LOD 1\|2 | 0 |
| Planet, latitude 45 | 5 at (7,6,7)\|(8,6,7), 1\|2 | 0 |
| Planet, equator | 5 at (12,−1,−2)\|(12,−1,−3), 1\|2 | 0 |
| Plane, origin | 5 at (1,−1,−1)\|(2,−1,−1), 1\|2 | 0 |
| Plane, east | 8 at (14,1,0)\|(14,2,0), 1\|2, and 1 more by a same-LOD face (an edge seam's) | 0 |
| Plane, south-west | 20 at 1\|2 and 0\|1 faces | 0 |

These are the locations the planet run reported in §15.2. With the fix every run has 0 G17 gaps, 0 T-junctions and 0
other holes; the only open edges are at the loaded region's edge (planet 298 to 329, plane 166 to 196). With only the
face mesher's fallback, the plane still has 13 G17-classified edges and 2 others, from the edge and corner meshers.

**Suites.** Headless `VoxelWorlds+VoxelWorldSave+VoxelCharacter`: **298 of 298** (295, plus `MS4`, `MT3` and
`Plane.ClosedSurface`). In-editor (D3D12), the same three suites: **298 of 298**, the GPU tests running (the GPU vs
CPU generation parity tests, `Meshing.MarchingCubes.CPUvsGPU`, `GPUAsync`, `Generation.GPUNoiseGeneratorAsync`,
`InfinitePlane.GPUConsistency`); only the two retired GPU DC interior tests skip, as before. No live survey: the
closed-surface tests check every rendered edge of a streamed world, which a survey cannot.

## 16. Spherical Phase 2: the planet on the GPU (2026-10-16)

The requirements doc's Phase 2 ([SPHERICAL_PLANET_REQUIREMENTS.md](SPHERICAL_PLANET_REQUIREMENTS.md) §4): SP-B4 and
SP-I3, plus the GPU half of SP-B7. Phase 1 kept planets on CPU generation (SP-B3's guard), and its benches missed
catch-up only because the CPU generates about 30 chunks a second against the GPU's 85 to 115 (§15.2). Exit check: GPU
parity green at LOD 0/1/2, and the GPU default re-enabled for planets. Walking (Phase 3) is game-side and can run beside
this phase; nothing here touches it.

### 16.1 Plan

Work items (IDs `WM-G*`, stable):

| ID | Work item | Requirements | Repos | Size |
|----|-----------|--------------|-------|------|
| WM-G1 | **The planet branch of the density shader.** `GenerateVoxelDensity.usf` gets a `SphericalPlanet` branch that computes what `FVoxelCPUNoiseGenerator::GenerateChunkSphericalPlanet` computes, in the same order: the radial height (`SampleRadialDisplacement`: terrain noise at `Direction × PlanetRadius`, continentalness at the same point through the baked curves), the shell signed distance and its quick rejects, density, materials by biome (temperature and moisture at the datum-sphere point) with the underwater variant when the column's terrain radius is under `WaterRadius`, height rules by altitude above the datum, caves with `VerticalScale` along local up (`ScaleAltitude`), the biome cave overrides and the underwater minimum depth, the cave-wall override, ores, and the cave flag. The generic 3D-noise branch no longer catches planets. The C++ uploads the planet's params from the request, which `FVoxelWorldModeFactory::FillGenerationRequest` fills | SP-B4 | VoxelWorlds | M |
| WM-G2 | **The planet's post-passes on the GPU:** the radial water fill (`ApplyRadialWaterFillPass`) and the underground classification along local down (`ForEachVoxelAlongDown`), as compute passes in the generation graph | SP-B7 (GPU), SP-B8 (GPU) | VoxelWorlds | M |
| WM-G3 | **`VoxelWorlds.Generation.GPUvsCPUSphericalParity`** at LOD 0/1/2: the north and south poles, the equator, 45°, a cube-face edge, a cube corner, a coast and a cave, in the style of the other `GPUvsCPU*Parity` tests | SP-I3 | VoxelWorlds | M |
| WM-G4 | **The GPU default for planets:** `ShouldUseGPUGeneration()` follows `bUseGPUGeneration` in every mode once WM-G3 is green; the chunk manager's SP-B3 warning goes | SP-B3 (retired), SP-B4 | VoxelWorlds | S |
| WM-G5 | **Live survey and benches:** the planet surveyed with GPU generation against the CPU from the same cameras, and the §15.2 bench routes against the GPU control | — | VoxelWorlds (docs) | M |
| WM-G6 | **Docs:** these results, the requirement rows (SP-B3, SP-B4, SP-B7, SP-I3), §4's phase-2 exit check, `GPU_PIPELINE.md`, `WORLD_MODES.md`, the parent's doc map | — | VoxelWorlds, parent | S |

Order: WM-G1 and WM-G2 (the generator, one commit each), WM-G3 with them, WM-G4 once WM-G3 passes in the editor, then
WM-G5 and WM-G6. PRs: VoxelWorlds (all of it), then UE5_PluginDev (the gitlink and the doc map). No content change is
expected: `Review_Planet` and `PlanetWorldConfig` already set `bUseGPUGeneration` like the demo (to confirm in WM-G5).

**Design (written before building).**

| Question | Choice | Why |
|----------|--------|-----|
| Where the planet's water fill and underground classification run on the GPU path | In the generation graph, as compute passes after the density pass, like the plane's (`AddVoxelPostPassDispatches`). Water: a seed kernel with one thread per digital line along local down (the CPU's phase 1), the existing flood sweep with the sea sphere as its eligibility test (phase 2), and the cave-flag clear folded into the underground kernel (phase 3). Underground: one thread per line, the column scan's state machine. The seed and flood run only in chunks the sea surface crosses, the CPU's gate (`FVoxelSeaSurface::Crosses` on the voxel box), evaluated in C++ | The readback then returns finished voxel data, as for the plane: running the passes on the CPU after readback was the traversal frame-spike source the GPU post-passes removed (`GPU_PIPELINE.md`). Each voxel is on exactly one line and a line's state reads and writes only its own voxels, so one thread per line, walking the line in the CPU's order, reproduces the CPU's result exactly; the flood is the plane's monotone fixpoint with a different test |
| The lines' geometry | Computed once per chunk in C++ (double) by the function the CPU passes use, and uploaded: the line axis, its direction and the per-step shifts `round(K × slope)`. The heightmap kernels are unchanged | The GPU then walks exactly the CPU's lines: float rounding of `K × slope` could otherwise round the other way at a half and move a voxel to another line. Heightmap chunks keep the column kernel, so their output stays bit-identical (the GPU post-pass parity holds by construction) |
| Precision | The C++ passes the chunk origin relative to the planet centre, computed in double; distances, directions, the noise sample point (`Direction × PlanetRadius`) and the sea test use it. The cave and ore noise sample the absolute position, as the CPU does | Absolute positions on a 250,000 uu planet off the origin carry float errors of about 0.03 uu; relative to the centre the chunk origin is exact and the error stays in the direction's last bits. The noise fields themselves are float on both sides. This is SP-H1's GPU half for small planets; large planets stay P2 |
| Caves: biome overrides and the underwater minimum depth | The planet branch applies both, as the CPU planet does (`CalculateCaveDensity`), through a small override table (biome id, cave scale, minimum-depth override). The heightmap branch keeps its documented gap (no override, no underwater minimum depth, the Caves parity test's comment) | Planet parity needs them whenever the config uses them. Changing the heightmap branch would move the GPU terrain of existing heightmap worlds, which needs a `GenerationVersion` bump and refuses every save: a separate decision, recorded as a follow-up |
| Terrain conditioning | Not applied on planets, as on the CPU | The zones are heightmap zones (SP-B10, Phase 5) |
| The ore `Rarity` hash | As on the plane: the absolute position, float on the GPU and double on the CPU, so ores with `Rarity` < 1 may differ by a few voxels | SP-H3 (Phase 4) moves the hash to planet-relative coordinates for both; the parity test keeps `Rarity` at 1 like the plane's ore tests |
| `PlanetGenerationVersion` | Unchanged (2) | The GPU generates the CPU's terrain (WM-G3 measures how closely); saves stay valid |
| Shared shader code | Two helpers come out of the heightmap branch, continentalness at a sample point and one cave layer's carve, and both branches call them; nothing else in the heightmap branch moves | Less duplication, and the heightmap parity tests' numbers before and after show the refactor changed nothing |

**How each is verified.**

| ID | Check |
|----|-------|
| WM-G1, WM-G2 | WM-G3's test; the heightmap `GPUvsCPU*Parity` tests report the same per-channel numbers as before the change |
| WM-G3 | At every site and LOD: density within ±2 on ≥ 99% of voxels, and the plane's material, biome and metadata thresholds (justified with the measured numbers in §16.2); non-vacuous: the site chunks cross the surface, the coast chunk has water flags and underwater materials, the cave chunk has carved underground air |
| WM-G4 | `Configuration.PlanetSettings` asserts a planet may generate on the GPU; the log at world start names the GPU generator |
| WM-G5 | Survey cameras over `Review_Planet` at both poles, 45°, the equator and a cube corner, each shot once on the CPU (`voxel.GPUGeneration.Enable 0`) and once on the GPU from the same camera: the same terrain. Benches: the planet on its equator and its 45° and 80° circles at 7,200 over the datum, at 1,500 and 3,000 uu/s, against the GPU control on its straight route; settled at the spawn; one build, one batch, a throwaway PIE first (G14); against §1's thresholds and D3 |
| All | Headless `VoxelWorlds+VoxelWorldSave+VoxelCharacter` (298 today); the same suites in the editor with the GPU tests running |

**Exit check:** `GPUvsCPUSphericalParity` green in the editor at LOD 0/1/2 at every site, planets generating on the GPU
by default, the GPU survey showing the CPU's terrain.
