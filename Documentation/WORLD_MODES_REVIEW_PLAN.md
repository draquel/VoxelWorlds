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
with the decisions it needed in §8. M1 results: §14.5. M2 (the world-mode API) results: §14.6.

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
| A world edge | Critical, **WM-I4/WM-I5 (recovery, boundary), WM-E1 (the visible edge)** | KI2, KI14. An ocean that continues to the horizon beyond the loaded ring (far water plane or ring), a seabed that slopes down instead of the `EdgeHeight - 1000` cliff, and a boundary rule. The rule can be soft (current pushes back, swim limit with a warning) or hard (blocking volume). *Decided with D9: hard at R+F, with a soft push-back before it, and a respawn as the safety net* |
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
| Map tile eviction | Critical past ~10 km, **WM-E2** *(added 2026-10-14)* | KI15. The first cost that grows with the island (§13.8) |
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
5. **The work items** in the order of §14.3: M1 → M2 → M3 → M4 → M5 → M6, then spherical Phase 1.

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
| C2 | **Refine** | Correct in both setups. As shipped (LOD on) it does not keep up; on the intended path (LOD off) a fresh world settles in 17 s like smooth (16 s) and the bench mesh queue peaks at 75-135 instead of 457-683, and the remaining lag is the neighbour cascade (KC2). The rest of this row is the as-shipped pass. A fresh world settles in 37 s (1,118 chunks loaded, 658 with geometry). Surveys at chunk faces, a chunk corner and both LOD band edges (7,000 and 14,000 uu) show no holes, missing or stale faces (`rc_c2_corner_near`, `rc_c2_bands_north`). Fades render with the atlas material (`rc_c2_fade_2`). R23 A→B→C: site C settles in 37 s, audit 0, nothing left over site B (`rc_r23_siteB_west`). After every settle `AuditResidency` reports 0 and no chunk is left in `Meshing`. `FarCompression.Stats`: 901 compressed, 253 uniform, 122 resident, 1,133 MB reclaimed. Problems: W-04 cannot be walked (C5). Flown instead at 3,000 uu/s following the terrain (`voxel.Bench.Run c2_w04_follow 3000 180000 dir=0:1 follow=300`), the frontier fell behind: rendered chunks dropped from about 720 to 315, the mesh queue peaked at 1,739, the terrain ahead was missing (`rc_c2_w04_follow_3`), catch-up took 28 s and thrash reached 13,698. That run's end-of-run audit listed 604 `PendingUnload` chunks beyond the unload radius; they drained within seconds, so it is an unload backlog, not a leak. The edge of the loaded region shows vertical walls (a missing neighbour is treated as air), visible only from outside the view distance (`rc_r23_siteB_east`) |
| C3 | **Bug** as shipped, **Refine** on the intended path | As shipped (LOD on), cubic fails the §1 thresholds at every speed except v6000 frame time, where both sides are saturated. On the intended path (LOD off, greedy on) cubic beats the control on frame time (−5% to −25%) and manager time (−33% to −47%) and matches its warmup, but still misses catch-up (1.6-3.2×) and memory (systems +83% to +116%, renderer GPU 9×). See §12.2 |
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
| KC2 | **Confirmed** as a cost, and on the intended path it is the main remaining streaming cost; the correctness risk is **dismissed** for what this session exercised | The legacy whole-chunk path runs for cubic and is correct: no hole, missing or stale face in any survey, R23 clean, audits 0, chunk-corner edits closed. Cost: with LOD off, its neighbour re-mesh cascade is the real re-mesh cost left: 1,329-2,540 neighbour re-meshes per run against 0 for the control, 2.0 meshes per chunk on the initial load, and catch-up 1.6-3.2× the control's in PIE (as shipped it adds 2,039-2,302 on top of the LOD re-meshes). Its per-frame deferral, waiting for neighbour data, is what inflates the bench's "other" count (KC14). The seam registry still tracks every cubic chunk (1,237 chunks, 10,774 seams, 4,040 dirty) and its P0 stub processor drains the jobs with no geometry (`VoxelChunkManager.cpp:4110-4114`): 6,140 jobs on the initial load with LOD off, 23,246 with LOD on (smooth: 10,898 real ones). No streaming test covers the path (C1) |
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
| KC11 | **Far compression is keyed on the LOD level, so the intended cubic path has none.** Compression skips chunks below `voxel.FarCompression.MinStride`, and with `bEnableLOD` off every chunk is LOD 0. The far-band slab is keyed on band 0's distance instead, so it survives `bEnableLOD` off (but not a one-band config) | `VoxelChunkManager.cpp:4677`; `DistanceBandLODStrategy.cpp:956-962`. LOD off: 0 compressed, 1,123-1,245 MB resident voxel data against 215 MB (spawn) and 600-760 MB (bench peak) for the control, loaded chunks unchanged. One band: no slab either, +47% chunks, 1,803 MB | The intended cubic path's main memory cost. **Fixed (§12.7)**: compression keys on the distance band |
| KC12 | **Melee misses across a one-block height difference.** From a one-block pit, 10 swings at a dummy on the rim did no damage; on the same level it died in 4 | C5 | Common in cubic terrain |
| KC13 | **The edge of the loaded region shows vertical walls.** A missing neighbour counts as air, so boundary faces are emitted. Visible only from beyond the view distance | `VoxelCPUCubicMesher.cpp:930-1001`; `rc_r23_siteB_east` | Cosmetic |
| KC14 | **The legacy path re-queues waiting chunks every frame, and the bench counts each one as a re-mesh.** When a chunk's face neighbour has no voxel data yet, the legacy path (cubic only; the seam path never waits) defers it and re-queues it at the end of every frame with the default reason "other". The bench's thrash counter counts queue insertions of already-meshed chunks, so each frame's wait counts once. Traced after the bench runs | `ShouldDeferMeshForNeighbors` under `!bSeamMeshingActive`, `VoxelChunkManager.cpp:3051-3066`; re-queue at `:3383-3386`; counter at `:5798-5803`. "Other" is 0 for smooth in every run; for cubic 0-1,074 in PIE and up to 91,074 headless, where CPU generation keeps neighbours waiting longer. The verbose run that counted 53,799 cubic re-meshes executed 5,729 meshes | Mostly a measurement artefact: queue churn and delayed re-meshes rather than wasted meshing. The wait itself is part of KC2's catch-up cost |

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
| I2 | **Refine** | S reads as an island on the map and from the air (`rbI2_islandS_map`, `rbI2_S_lagoon_oblique`): lagoons, inner seas and coastline all come from the noise inside a circle. Problems: a round plateau stands in the sea at the north edge (a campsite pad, KI1), visible on the map; from outside the loaded ring the island is a slab floating in the void, with the water plane, the −4,500 floor and the cave-cut bottom layer showing (`rbI2_S_edge_outside`, KI2); under water a vertical 1,000 uu wall rings the island at R+F (`rbI2_S_edge_cliff_underwater`). Rectangle shape with Exponential falloff (`RB_S_RectExp`, temporary): a rectangular island with straight coasts and diagonal creases from the Chebyshev falloff (`rbI2_S_RectExp_map`, `rbI2_S_RectExp_oblique`). Map exploration runs past the coast (KI9). A survey from a camera 48,000 uu up shows chunk-sized blank rectangles in the lagoon; at 4,500 uu the same water renders whole, so they are a camera artefact |
| I3 | **Pass** | The whole of S is resident from the centre: 1,518 chunks, settled 247 MB voxel data, 23 MB renderer, audit 0, 9 s to drain. After visits to three coasts the centre holds 1,878 chunks and 266 MB: the outer ring stays loaded, bounded by the island cull (§13.4). Memory returns to the same level |
| I4 | **Refine** (inside the island) / **Bug** (along the coast) | Inside the island frame P95 is within +10% of the control and the island keeps up at v1500; manager P95 is +17-21% and the generation queue backs up at v3000, from the 35-55% more chunks per view (KI5). Along the coast (orbit at R) the island misses every threshold: frame +14-30%, manager +23-38%, catch-up 2.6-3.5× at v1500-v3000. The island's LOD re-meshing is **bimodal** between identical cold runs (KI12, new): 12,344-12,500 or 1,341-1,372 LOD re-meshes on the same route; in the low mode the island beats the control. Headless (CPU generation) the island lands in the high mode twice out of two: frame P95 +24%, manager P95 +28%. L's interior chord equals M's: per-frame cost does not depend on island size. §13.3. *(§13.10: the low mode skipped LOD work it owed; the high-mode rows are the island's numbers, and KI12 is fixed)* |
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
| KI2 | **Confirmed** | No edge: a 1,000 uu wall at R+F, a floor ring out to R+F + 6,788 uu (the cull's chunk diagonal), then nothing. Water ends with that ring. The pawn swims off it and falls about 4 minutes to KillZ, which destroys it without a respawn; with water off it walks off the floor's edge. From outside, the island floats in the void (I2, I5) |
| KI3 | **Confirmed** | §13.2: 1,098 land columns culled and 1,347 floor-only columns kept with the demo's origin; in PIE the south coast is a straight cut through a hill. With `WorldOrigin` 0 the two agree exactly |
| KI4 | **Partly confirmed** | `bIslandBowlShape` is copied (config, request, island params, chunk manager ×2, map, field viewer, CPU generator) and read nowhere, shaders included. Exponential falloff leaves a 1,016-1,307 uu step instead of 1,000 (a 5% residual on top of the floor cliff every falloff type has). Island params are built in four places (`VoxelChunkManager.cpp:994`, `:2561`; `VoxelMapSubsystem.cpp:176`; `VoxelFieldTypes.cpp:63`). **Dismissed for these configs:** `GetTerrainHeightBounds` already contains the floor ([−12,000, 10,000] vs −4,500); it would miss it only when `EdgeHeight − 1000` is below the continentalness minimum. The world mode's `GetDensityAt` calls the floor air, which generation does not use (latent) |
| KI5 | **Confirmed** | +35% chunks at the spawn on S (1,518 vs 1,123), +30-55% peak loaded on the M routes, generation queue 2.7× the control's at v3000, +32-54% meshes, and the main reason for the manager and coast misses in §13.3 |
| KI6 | **Confirmed** (code) | `MaxLoadedChunks` is read nowhere (only `VoxelWorldTestActor.cpp:490` sets it). The island cull bounds S at 2,400 chunks; nothing else bounds anything |
| KI7 | **Confirmed** | I8: with the radius changed, the same save loaded silently: 30 edited chunks, 2 dungeon records, 1,499 explored tiles, and the player restored past the new edge, frozen |
| KI8 | **Partly confirmed** | The 8-direction search finds land from the falloff band and from 30,000 uu past R+F (water on). Water off, off the island: the floor height counts as dry land and the pawn freezes over the void, waiting for collision. `WaterLevel` without `WorldOrigin.Z` and `GetPlanetSpawnPosition` were not exercised (Z origin 0, no planet) |
| KI9 | **Confirmed** for the map and markers, **dismissed** for scatter and NPCs, **code-only** for PCG and water-off navigation | Map exploration: 393 tiles on S for 216 columns; 786 band and 124 beyond-R+F tiles of 1,497 on M, all saved. Markers: §13.1 I6. Scatter: none past the coast. Surface NPCs: spawn only on loaded, dry ground and despawn in deep water. Navigation with water on refuses the sea. PCG: no PCG components in VoxelDemo; the sampler has no island mask (`PCGVoxelSurfaceSampler.cpp:244-291`). Navigation with water off treats the floor as walkable by code; not run |
| KI10 | **Confirmed** | `WP_VoxelCoexistTest`'s voxel world references `DemoWorldConfig` (InfinitePlane); `WP_IslandBowlConfig` is referenced by nothing. Island tests: the two existing ones plus the three probes added here. This review is the first island bench |
| KI11 | **Confirmed**: false findings, not chunks kept too long | Every island audit finding in the session (4-64 per run) is a Loaded chunk in the top or bottom layer (z = 3 or −4), 48,000-52,000 uu away in 3D and 46,082-47,942 uu in 2D, inside the radius by the island's own 2D rule. The control's audits were 0 |

### 13.7 New findings

| ID | Finding | Evidence | Impact |
|----|---------|----------|--------|
| KI12 | **Island LOD re-meshing is bimodal between identical cold runs.** M's v3000 chord gave 12,500, 12,344, 1,341 and 1,372 LOD re-meshes in four runs (same command, fresh world, same path to within 31 uu of height). In the high mode chunks mesh at LOD 1 five times as often (5,020 vs 987 meshes) and frame / manager P95 are 41 / 14.2-14.5 ms; in the low mode 32 / 9.7-10.0 ms. Headless (CPU generation) both island runs landed in the high mode (9,269 and 9,304 against the control's 1,142, §13.3), so the high mode may be the island's normal state and the PIE low mode the variant. Judging by the LOD counts, the PIE mode held for whole stretches of the session (the M batch high, the L batch low), and it may also switch inside a run (the soak's re-mesh rate doubled at minute 10, §13.5). It is not explained by a save load, the override, the island parameters or Python state, all checked. The control gave 7,168 twice | Bench JSON `rbI4_M_chord_v3000{,b,c,d}`; meshing log LOD counts | The island's streaming numbers have two values; its threshold verdicts depend on which mode a run lands in. Likely an ordering- or timing-dependent path in LOD evaluation, which only runs on a viewer chunk change or a drained-queue sweep (`VoxelChunkManager.cpp:768-791`) and drops candidates past the per-frame cap. Needs an instrumented run. **Explained and fixed (§13.10):** the sweep flag froze in whatever state it had when the queues last emptied, near the start of the route; set, LOD was evaluated every tick, clear, only at chunk changes, 8 at a time. The low mode was the broken one. It affected every world mode; the control's headless run was in it |
| KI13 | **Island LOD distance is 2D everywhere, so whole columns sit in the near bands.** InfinitePlane measures 3D distance, so deep and high chunks fall into the far band and compress; the island keeps them uncompressed | At the same spawn: 224 resident (uncompressed) chunks vs the control's 113; settled voxel data 247 vs 135 MB (`DistanceBandLODStrategy.cpp:644-648`) | Memory, alongside KI5; the reason KI11's audit disagrees |
| KI14 | **No recovery when the pawn leaves the generated world.** KillZ destroys the pawn and nothing respawns it: nothing in the project overrides `FellOutOfWorld`, so the engine default destroys the actor and the death and respawn path never runs. The character's chunk-based wait has no timeout, so a spawn or a restored save position where no terrain will ever generate freezes the pawn for good | I5 (water on: no pawn after KillZ), I7 (water off, off the island), I8 (radius change) | Soft locks; any world edge design needs this |
| KI15 | **Map tile images are never evicted.** Each explored tile keeps a 64×64 `FColor` image (16 KB) for the session | `VoxelMapSubsystem.cpp:1648` (`TileCache.Add`, no removal outside `Deinitialize`); `VoxelMapTypes.h:27` | Mode-independent, but in a finite world it is the first cost that grows with the world: a fully explored L is about 0.55 GB of map tiles (§13.8) |

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
    0.55 GB; a 20 km island ≈ 2.2 GB. Exploration past the coast (KI9) adds the sea ring on top.
  - The save: explored tiles are compressed coordinates (1,497 tiles in a few kB), edits and dungeon records grow with
    play, not with size.
  - Float precision: clean to about 100 km from the origin (`Research/TRAVERSAL_LIMITS.md`), so up to about 200 km
    across for a centred island.
- **Estimate.** With the current streaming-only model, an island up to **about 10-20 km across** works without new
  memory work (L measured at 10 km; at 20 km the map tile cache reaches about 2 GB when fully explored). Past that the map
  cache needs eviction or a coarser far map; streaming itself would hold to the ~200 km precision limit.
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
  performance items: together they explain the chunk excess, the generation backlog and most of the coast misses. §13.11
  measured KI5 per layer: on the chord the island's ~340 extra chunks are layers −3, 2 and 3, the layers the control's
  slab trims.
- Add from §13.11: **rebuild the balanced-LOD cache only when the viewer moves** (1.3-1.9 ms a tick on the control, every
  tick), and decide whether heightmap modes should take the **vertical range from the terrain** instead of the viewer's
  altitude (the −2 floor).
- ~~**Find the cause of KI12** before the next island bench; until then island benches need at least two runs per route.~~
  Done (§13.10): KI12 was a latch in LOD evaluation, in every world mode. It is fixed, and one run per route is enough
  again. Island numbers to use are the high-mode rows of §13.3.
- **Save fingerprint** (KI7), **one island-centre definition** (KI3) and **the bounds query** (KI9) as already listed.
- Add: **evict or downsample map tiles** (KI15) before islands larger than about 10 km.

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
- **The −2 floor is still a design question.** At z 7,200 the fixed window starts at layer −1, while the control's
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
| KC2 | Cubic is the last user of the legacy whole-chunk path | Open. On the intended path it is cubic's main remaining cost: PIE catch-up 1.6-3.2× the control, legacy neighbour slices 2.6 ms of the manager's slowest ticks (§12.8) | WM-C1 |
| KC3 | GPU cubic meshing is test-only | Open as dead code (D1: delete it). The test hazard was fixed by T5 | WM-C4 |
| KC4 | Cubic scatter is wrong with GPU extraction | Open | WM-C3 |
| KC5 | Player dig and place are tuned for smooth terrain | Open | WM-C2 |
| KC6 | Surface queries return the smooth surface, so props sink into blocks | Deferred (D1: first-class only) | — |
| KC7 | Two-block rises strand NPCs | Partly confirmed; deferred (D1) | — |
| KC8 | No content exercises cubic | Fixed in M1 (WM-H5; UE5_PluginDev#102, merged): `DemoCubicWorldConfig` is on the intended path and `Config/CubicScatterConfig` is deleted. The review configs exist | WM-H5 |
| KC9 | Voxel trees are not gameplay | Deferred (D1). The unlimited yield from one voxel is G3 | — |
| KC10 | The smooth config's tree templates use material IDs that do not exist | Fixed in M1 (WM-H4; draquel/VoxelWorlds#85, UE5_PluginDev#102, merged) | WM-H4 |
| KC11 | Far compression was keyed on the LOD level | Fixed (§12.7) | — |
| KC12 | Melee misses across a one-block height difference | Deferred (D1) | — |
| KC13 | Vertical walls at the edge of the loaded region | Dismissed: cosmetic, visible only from beyond the view distance. WM-C1 changes how cubic meshes chunk boundaries; recheck it there | (WM-C1) |
| KC14 | The legacy path re-queues a waiting chunk every frame | Dismissed as a defect: it inflates a bench counter, not the meshing. The loop goes away with WM-C1 | (WM-C1) |

**Island:**

| ID | Finding | Status | Work item |
|----|---------|--------|-----------|
| KI1 | POI placement samples InfinitePlane height in every mode | Partly fixed in M2 (WM-A3, §14.6): placement samples the configured world mode, so land sites are judged on the island's ground; with the project catalog no POI is placed past R+F or in the sea on S, M or L (`POIPlacementProbe` asserts the second). Open: a site of a type that may place anywhere, past R+F; the bounds test (WM-I1) | WM-I1 |
| KI2 | No world edge | Open | WM-I5 (boundary), WM-E1 (visible edge) |
| KI3 | The island centre means two different things | Fixed in M2 (WM-A1, §14.6): one builder, the centre is `WorldOrigin` + offset everywhere; `CullCentreProbe` asserts | — |
| KI4 | Shape-parameter defects | Fixed: `bIslandBowlShape` removed, the Exponential falloff reaches 0 at R+F and `GetDensityAt` agrees with the floor beyond it (M1, WM-H6); the param builders are one (M2, WM-A1), and `GetTerrainHeightBounds` now includes the floor beyond the edge (M2, WM-A2) | — |
| KI5 | No far-band slab culling on islands | Open: +30-55% loaded chunks; on the chord the excess sits in layers −3, 2 and 3 (§13.11). The slab now samples the world mode's height (M2, WM-A3), so WM-S1 only has to lift its InfinitePlane-only gate | WM-S1 |
| KI6 | No finite-world concept; `MaxLoadedChunks` is read nowhere | Open for the setting. Pre-generation and baking stay nice-to-haves (§6) | WM-S3 |
| KI7 | The save key ignores world shape | Fixed in M2 (WM-A4, §14.6): the save records the world's fingerprint; a save from a different radius is refused, kept on disk, and the world starts fresh | — |
| KI8 | Spawn is only water-aware | Partly: off the island with water off, the floor counts as land and the pawn freezes | WM-I3 |
| KI9 | Consumers have no island mask | Partly: open for map exploration, the world map and markers, and by code for PCG and water-off navigation; dismissed for scatter and surface NPCs. The mask exists since M2 (`IsInsideWorld`, `GetWorldBounds`, WM-A2); the consumers adopt it in WM-I2 | WM-I2 |
| KI10 | Island test coverage is minimal | Partly fixed: three probe tests (§13); `CullCentreProbe` asserts and `POIPlacementProbe` asserts land sites (M2); `VoxelWorlds.WorldModes.Api.*` cover the bounds, frame, params and fingerprint. `WP_IslandBowlConfig` is unused | WM-I6 |
| KI11 | The residency audit and island LOD measure distance differently | Open (false findings) | WM-S2 |
| KI12 | Bimodal LOD re-meshing | Fixed (§13.10), in every world mode | — |
| KI13 | Island LOD distance is 2D, so whole columns stay in the near bands | Open | WM-S2 |
| KI14 | No recovery when the pawn leaves the generated world | Open; not island-specific | WM-I4 |
| KI15 | Map tile images are never evicted | Open; not island-specific, but it is what limits islands past about 10 km (§13.8) | WM-E2 |

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
| G13 | The vertical range follows the viewer's altitude | Open (D8) | WM-S1; recheck in WM-S4 |
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
§14.6): draquel/VoxelWorlds `feature/wm-a1-params-builder` (WM-A1) and `feature/wm-a2-a4-world-mode-api` (WM-A2, WM-A3
and WM-A4's VoxelWorlds side, stacked on it); UE5_PluginDev `claude/cool-gates-ffddfc` (WM-A3 in VoxelWorldPOI, WM-A4 in
VoxelWorldSave, and the gitlink).

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-A1 | **One params builder per world mode.** Generation (CPU and GPU), the LOD cull, the map, the editor field viewer and the chunk manager build island params (and planet params, SP-A6) with one function, and the island centre is `WorldOrigin` plus the offset everywhere. `CullCentreProbe` becomes an assertion: no land column culled with the demo's origin | KI3, KI4 | VoxelWorlds | M | WM-H6 (same code; do it first or together) |
| WM-A2 | **Bounds and a local frame on `IVoxelWorldMode`.** `IsInsideWorld(X, Y)` and `GetWorldBounds()` (island: the R+F footprint; plane: unbounded; planet: the shell), and SP-A1/SP-A2 with their heightmap implementations: `GetUpVector` is +Z, `GetAltitude` is Z above the datum, `WorldToSurface` is (0, X, Y). The planet implementations come with spherical Phase 1. Blueprint-callable through the world actor | KI1, KI9 | VoxelWorlds | M | WM-A1 |
| WM-A3 | **Height through the world mode.** The LOD strategy's slab sampling and POI placement call the configured world mode instead of `FInfinitePlaneWorldMode`'s static height math | KI1, KI5 | VoxelWorlds, parent (VoxelWorldPOI) | S-M | WM-A1 |
| WM-A4 | **Save fingerprint.** The save header records the world mode, the shape params and a generation version. On a mismatch the save is not loaded: logged, kept on disk, and the world starts fresh. Shared with SP-G9 | KI7 | parent (VoxelWorldSave) | S-M | WM-A1 |

**M3. Island correctness.** Afterwards nothing is placed, explored or spawned past the coast, and leaving the world
cannot soft-lock the player.

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-I1 | **POI placement on the island.** Sites outside the bounds are not placed. Sites in the falloff band are judged on the island's height (RequireLand, the water check, the dungeon cover rule), so pads no longer raise plateaus out of the sea. `POIPlacementProbe` becomes an assertion on S, M and L: no POI past R+F, no land POI under water | KI1 | parent (VoxelWorldPOI) | M | WM-A2, WM-A3 |
| WM-I2 | **Consumers respect the bounds.** Map exploration stops at the bounds and the world map's pan is clamped to them; markers only inside; the PCG sampler skips points outside; navigation treats outside as blocked, water on or off | KI9 | VoxelWorlds (map, PCG, navigation), parent (markers) | M | WM-A2 |
| WM-I3 | **Spawn inside the world.** The spawn search rejects points outside the bounds, so the off-island floor is never taken for land | KI8 | VCP | S | WM-A2 |
| WM-I4 | **Recovery.** Falling out of the world (KillZ) runs the death and respawn path instead of destroying the pawn, and the chunk-based collision wait gives up after a timeout and moves the pawn to the respawn point. Every world mode | KI14 | VCP, parent (game module) | S-M | — |
| WM-I5 | **The boundary (D9).** A hard boundary at R+F, taken from the bounds query, and a soft push-back in the last stretch of sea before it, with a HUD hint | KI2 | VoxelWorlds (boundary), VCP (push-back), parent (HUD) | M | WM-A2, WM-I4 |
| WM-I6 | **Island tests and content.** The probe assertions above; delete or tune `WP_IslandBowlConfig` | KI10 | VoxelWorlds, parent | S | WM-A1, WM-I1 |

**M4. The streaming window.** Every heightmap world; the island's performance items.

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-S1 | **Vertical range from the terrain (D8).** In heightmap modes a column's candidates are its surface slab (the world mode's height ± 1.5 chunks, up to the water plane) at every distance, plus the viewer's layer ± 2 within band 0. The viewer-relative −2 to +8 offsets go. The slab samples the world mode (WM-A3), so islands get it. Check caves, dungeon rooms and edited chunks below the slab near the viewer, the island's floor ring, and a flight at z 7,200 | KI5, G13 | VoxelWorlds | M | WM-A3, WM-H1 (same function) |
| WM-S2 | **3D LOD distance for islands.** Islands measure LOD distance in 3D like InfinitePlane, and the island boundary cull stays 2D. The residency audit then agrees with the unload rule | KI13, KI11 | VoxelWorlds | S | — |
| WM-S3 | **Remove `MaxLoadedChunks`.** The view distance and the island cull already bound residency (§13.4); document what does | KI6 | VoxelWorlds | S | — |
| WM-S4 | **Re-bench, one batch, one build.** Island M chord, perimeter and coast at v1500-v6000, S at its spawn, flights at z 7,200 and the I2 survey from 48,000 uu, each with the control. Check §1's thresholds and the D3 budget, and update §13's verdicts | KI5, KI11, KI13, G13 | VoxelWorlds (docs) | M | WM-S1, WM-S2 |

**M5. Cubic, terrain-only (D1).**

| ID | Work item | Findings | Repos | Size | Needs |
|----|-----------|----------|-------|------|-------|
| WM-C1 | **Cubic on the seam path.** The faces on a chunk boundary belong to the pair's seam job, which culls them against both chunks' voxels (the seam doc's "trivially compatible" case), so a chunk no longer waits for its neighbours' data to mesh its interior. Then delete the legacy neighbour-slice extraction, the deferral, `CaptureMeshDeps`/revalidate, the 26-neighbour re-mesh on generation and the seam registry's stub jobs. Add a cubic streaming test, and re-run C2 and §12.8's benches against a same-build control. Recheck KC13 | KC2, KC13, KC14 | VoxelWorlds | L | — |
| WM-C2 | **Block-accurate editing.** In cubic the player digs at `ImpactPoint − ½·VoxelSize·normal`, places at `+ ½·VoxelSize·normal` and sets whole voxels, so one click is one block; the demo brush runs with `bUseDiscreteEditing` in cubic | KC5 | VCP, VoxelWorlds | M | — |
| WM-C3 | **Cubic scatter on the CPU extractor.** Cubic skips the GPU extraction branch, and `PlacementMode = BlockFaceSnap` is read | KC4 | VoxelWorlds | S | — |
| WM-C4 | **Delete the GPU cubic mesher** and its tests; the README stops claiming it | KC3 | VoxelWorlds | S | — |

**M6. The island's visible edge and scale.**

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
| M3 Island correctness | WM-I1 to WM-I6 | ~6.5 days | M2 | `POIPlacementProbe` asserts; I5-I8 re-run: swimming out stops at R+F, falling out respawns, an off-island spawn stands on land, a radius change starts fresh; the I6 tour finds no marker or explored tile past the bounds |
| M4 Streaming window | WM-S1 to WM-S4 | ~4 days | WM-A3, WM-H1 | WM-S4: island M within §1's thresholds on chord, perimeter and coast; peak loaded chunks within +25% of the control; the D3 budget met on every run, M's v6000 chord included; island audits 0; the control no worse |
| M5 Cubic terrain-only | WM-C1 to WM-C4 | ~6.5 days | — | Cubic against a same-build control: catch-up within +25% and manager P95 within +15% (§12.8's misses); C2 and C4 pass; the legacy path is deleted |
| M6 Edge and scale | WM-E1, WM-E2 | ~4.5 days | WM-A2, WM-I5 | From past R+F the sea reaches the horizon with no void; a fully explored L keeps its map tiles under a fixed budget |
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
| 1. A correct planet on the CPU | After M4 and M5 | L | SP-C1 (candidates cover the shell) takes WM-S1's shape: a per-column interval around the surface plus the viewer's neighbourhood, which for a planet is a shell. §13.11's fix already keeps a pole viewer's window at its own layer instead of near layer 40. WM-C1 removes the legacy path before planets test seams at mid latitudes (SP-D4) |
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

| ID | Status | Branch | Check |
|----|--------|--------|-------|
| WM-A1 | Done, in review | VoxelWorlds `feature/wm-a1-params-builder` | `FVoxelWorldModeFactory` (VoxelGeneration) builds the island and planet params, the analytic world mode and the generation request's world-mode fields; the request carries `FIslandBowlParams` / `FSphericalPlanetParams` (the copies `FIslandModeParams` / `FSphericalPlanetModeParams` are gone). Users: generation (CPU and GPU), the chunk manager (both sites), the LOD island and shell culls, the map, the field viewer, POI placement and the probe tests. The island centre is `WorldOrigin` + offset everywhere. `CullCentreProbe` asserts: with the demo's origin 0 land columns are culled (was 1,098) and generation centres the island at (0, 0); likewise for an island 120,000 uu off the origin. Live, `Review_Island_M_DemoOrigin`: the cull logs `Center=(0, 0)`, `GetWorldBounds` agrees, the south coast slopes into the sea where review B saw a straight cut through a hill (`m2_ki3_coast_north`), audit 0. Of the project's configs only `Review_Island_M_DemoOrigin` has an island off the origin, so it is the only content whose terrain moves |
| WM-A2 | Done, in review | VoxelWorlds `feature/wm-a2-a4-world-mode-api` | `IVoxelWorldMode::IsInsideWorld`, `GetWorldBounds` (VoxelCore `FVoxelWorldBounds`: Unbounded, Circle, Rectangle, Shell), `GetUpVector`, `GetAltitude` (above `SeaLevel`; above `PlanetRadius`), `WorldToSurface` / `SurfaceToWorld` (`FVoxelSurfaceCoord`). Defaults describe an unbounded heightmap world, so the dungeon plugin's world mode and test mocks build unchanged. The island's height bounds include the floor beyond the edge. The planet side is implemented (shell, radial frame, a gnomonic cube mapping that round-trips on all six faces) but unused until Phase 1. Blueprint: `UVoxelChunkManager` `IsInsideWorld`, `GetWorldBounds`, `GetUpVectorAt`, `GetAltitudeAt`, `WorldToSurface`, `SurfaceToWorld`, `GetWorldFingerprint` (`Voxel|World`); called live through Python on the island: inside at 0.99 (R+F), outside at 1.01 (R+F), round trip exact. Tests `WorldModes.Api.Bounds`, `.Frame`, `.FactoryParams` |
| WM-A3 | Done, in review | VoxelWorlds (LOD), UE5_PluginDev (VoxelWorldPOI) | The LOD strategy builds its world mode with the factory; the far-band slab and the terrain height bounds sample it (the slab used InfinitePlane's static math without continentalness, off the generated surface by up to 2,481 uu in `LOD.SlabUsesWorldModeHeight`). The slab stays InfinitePlane-only and the vertical window is unchanged (WM-S1). POI placement and the conditioner sample the world's own world mode (`FPOIPlacementConfig::TerrainWorldMode`). `POIPlacementProbe` with the project catalog: S 1 POI (inside R), M 4 (3 inside, 1 on land in the band), L 22 (21 inside, 1 on land in the band); none past R+F, no land POI in the sea (asserted). Before: M placed 12, 8 of the 9 past R in the sea or over the floor, on pads raised 1,900-5,900 uu |
| WM-A4 | Done, in review | VoxelWorlds (fingerprint), UE5_PluginDev (VoxelWorldSave) | `FVoxelWorldFingerprint` (world mode, shape params, voxel grid, `FVoxelWorldModeFactory::GenerationVersion` = 1), recorded in `FVoxelWorldSaveDocument::WorldFingerprint`. `LoadWorld` refuses a save from a different world, logs what differs, keeps the file, starts fresh, toasts, and keeps auto-save off until an explicit save; a save from before fingerprints loads. Live, review B's I8 again on `Review_Island_S`: saved (216 explored tiles, the player), radius 30,000 → 20,000 in memory, Play: `LoadWorld ... belongs to a different world (IslandRadius=30000.0 vs 20000.0): NOT loaded`, a fresh world, the file's hash unchanged at Play and after Stop (tear-down auto-save skipped); radius back, Play: the save loads (216 tiles restored). Tests `VoxelWorldSave.Document.WorldFingerprint`, `WorldModes.Api.Fingerprint` |

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
