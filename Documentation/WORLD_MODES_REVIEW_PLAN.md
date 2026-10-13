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

Four read-only code surveys (2026-10-12) found the items below. None has been reproduced live yet; the review
confirms or dismisses each one. IDs are stable, so the follow-up plan can refer to them.

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

Island features that are expected to work and only need confirming: campsites (world-mode aware, clamp the pad
above water), surface NPC spawning (needs a loaded surface, collision and dry land), scatter (works from loaded
meshes), WorldClaims conditioning (tested on IslandBowl), the world clock (no mode dependency), and GPU
generation parity (locked by tests).

## 3. Tooling to build before the review

The review needs measurements the current tools cannot produce. These items are small. The recommendation
(decision D4) is to ship them as one PR before any review run, so every run produces comparable,
self-describing reports.

| ID | Tool change | Why |
|----|-------------|-----|
| T1 | **Self-describing bench reports.** Add map, config asset path, `WorldMode`, `MeshingMode`, GPU generation/meshing/scatter flags, `-VoxelForceCPU`, RHI, voxel size, chunk size, view distance, `WorldOrigin`, island params and viewport size to the JSON. | Today no report says what it measured (`VoxelStreaming/Private/VoxelStreamingBenchmark.cpp:286-319`). |
| T2 | **Bench path control.** `voxel.Bench.Run` gains a direction and an optional start point (the config struct already has `Direction`, `VoxelStreamingBenchmark.h:22-23`). Add a route mode with a waypoint list and an orbit mode (circle of radius r around a centre), plus an optional terrain-follow altitude. | Island chords, coast crossings and perimeter loops; later, great-circle routes on planets. |
| T3 | **Memory and residency in the report.** Sample `GetVoxelMemoryStats` every N frames: voxel, compressed and uniform counts, renderer CPU and GPU MB, collision, scatter and process MB. Run `AuditResidency` at the end of each run and record its findings. | "Large finite world" is bounded by memory, and the bench records only chunk counts. |
| T4 | **Mode A/B without hand-editing maps.** Add a `-VoxelConfig=/Game/...` switch that the world actor reads before `Initialize`. It works headless and in PIE. | Headless runs use the map's saved config, and no switch exists for config, mode or meshing. |
| T5 | **Headless hygiene.** Point `Scripts/voxel_bench_sweep.ps1` at `D:\Program Files\Epic Games\UE_5.8` (it still defaults to UE_5.7 on C:). Give the cubic GPU tests a `GUsingNullRHI` skip and a bounded wait. | KC3; avoids a hung headless suite. |
| T6 | *(optional)* `STATGROUP_Voxel` cycle counters around generation, meshing, seams, collision and scatter. | Unreal Insights currently shows the voxel work only as coarse task names. |

Content for the review (local scaffolds; `Content/` is gitignored):

- **Configs, all duplicated from `DemoSmoothWorldConfig`** so they share seed, biomes, caves, water and POI
  tables, and differ only in the field under test:
  - `Review_Cubic`: `MeshingMode = Cubic`, `DemoCubicScatterConfig`, greedy on.
  - `Review_Cubic_GPUScatter`: the same with GPU scatter extraction on, for KC4.
  - `Review_Island_S/M/L`: IslandBowl at the sizes in §5, with `WorldOrigin` 0 and water on.
  - `Review_Island_M_DemoOrigin`: island M with the demo's `WorldOrigin`, to reproduce KI3.
- **Maps:** `VoxelDemo_Cubic` and `VoxelDemo_Island`, duplicates of `VoxelDemo` with the config swapped, so every
  game system is present.
- **Saves:** back up `Saved/SaveGames/Worlds/` before each map's first PIE and restore it after every stop. PIE
  stop auto-saves, and the user may play between sessions, so check `savedAtUtc` first.

## 4. Review A: cubic meshing

**Setup:** `VoxelDemo_Cubic` with `Review_Cubic`; `VoxelDemo` as the control. Build from
`D:\Program Files\Epic Games\UE_5.8`. Disable the editor's background throttle for PIE timing runs.

| ID | Scenario | How | Pass when |
|----|----------|-----|-----------|
| C1 | Automated suites | In-editor `AutomationTestToolset`: `Meshing.Cubic.*`, `Scatter.*`, plus the full headless suite (no GPU tests) | All green; record the cubic GPU tests separately (KC3) |
| C2 | Streaming correctness | PIE: spawn, walk, then W-04 (`vox.SetSpeed 3000`, `vox.AutoRun 1 0 0`, 1 min). Survey with `pie_camera` and `pie_screenshot` at chunk boundaries and at the config's LOD band edges. Repeat the R23 teleport repro (A→B→C), then run `voxel.Stream.AuditResidency` and `voxel.FarCompression.Stats` | No holes, missing faces or stale faces at chunk boundaries after late neighbours load. Fades render with the atlas material. Audit reports 0 findings. No chunk left in `Meshing` |
| C3 | Streaming performance | Focused-PIE bench at v1500, v3000 and v6000 over 40,000 uu, a cold start per velocity, cubic vs smooth control. Headless CPU sweep with `-VoxelConfig`. One extra cubic run with a single LOD band, to price KC1 | §1 thresholds vs control. Also record per-chunk mesh ms, vertex and triangle counts, renderer GPU MB, and the thrash breakdown |
| C4 | Editing | W-06 by hand: edit mode B, LMB/RMB on +X/+Y/+Z/−Z faces, at a chunk corner, and with a pickaxe (`vox.Dig`). Walk into the hole. Save, stop, play: the edits come back | One click removes or places exactly the targeted block (expected to fail today, KC5). No seams. No fall-through |
| C5 | Movement and collision | W-02 over one-block steps and two-block walls: jump, sprint, crouch, swim in cubic water. Drop a physics cube (`Saved/ClaudeScripts/pie_drop_cube_*.py`); a corpse rests on blocks. `vox.Debug.Capture 1` during an AutoRun at distance | Steps behave as designed (decision D5). Rigid bodies rest on top. No fall-through |
| C6 | Game systems | W-07 POI visit (pads, structures on block tops); `vox.TPPOI Tiled`, `vox.TPRoom` (dungeon entrance stitching); `vox.Campsite` and placing a campfire kit; `vox.NPCs`, `vox.SpawnNPC Wolf 3`, `vox.NavPath`, `vox.NavDebug 1`; drop world items (Q); chop trees and gather (KC9); W-05 map and minimap; save/load; one day/night cycle | Nothing floats or sinks more than one voxel (KC6). NPCs path over steps without getting stuck (KC7). Trees can be chopped |
| C7 | Scatter | `Review_Cubic` vs `Review_Cubic_GPUScatter`: grass and props on block tops at LOD 0 and beyond | Scatter on block-face centres in both (expected to fail with GPU extraction, KC4) |

**Output:** a pass/refine/bug table per scenario, the bench comparison, and a recommendation on decision D1
(how far cubic is supported).

## 5. Review B: island mode as a large finite world

**Sizes** (decision D2; island centred at the origin, demo view distance 40,000 uu):

| Tier | IslandRadius / FalloffWidth | Across | Chunk columns (approx.) | Purpose |
|------|-----------------------------|--------|--------------------------|---------|
| S | 30,000 / 10,000 uu | 0.8 km | ~220 | The whole island fits inside the view distance from the centre: **full-residency baseline** (memory, time to build the whole world) |
| M | 200,000 / 50,000 uu | 5 km | ~8,500 | **Main target**, "large but finite": a 15-minute walk across |
| L | 450,000 / 50,000 uu | 10 km | ~34,000 | Scale check; still far inside the ~100 km float-precision limit (`Research/TRAVERSAL_LIMITS.md`) |

**Setup:** `VoxelDemo_Island` with `Review_Island_*`. The control is `VoxelDemo` running the same bench routes.

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
| D1 | How far is cubic supported: first-class (every game system), terrain-only (meshing, streaming, editing), or dropped? | Decide **after** Review A, using its numbers. The review is scoped to gather the evidence |
| D2 | Island sizes for Review B | S / M / L as in §5 |
| D3 | Performance acceptance and memory budget | Relative to the InfinitePlane control (§1). Set the absolute memory budget after I3 measures full residency on S |
| D4 | Build T1-T5 before the review? | Yes, as one small PR. T6 optional |
| D5 | Cubic step policy | Keep "every block is a jump" (current design) and make nav and NPC auto-jump agree with block heights |
| D6 | Spherical scope (planet size, cubic on planets, game systems, orbit) | See the requirements doc, §2 |

## 9. Order of work

1. **T1-T5** (one VoxelWorlds PR plus a parent bump) and the review content (local).
2. **Review A: cubic** (C1-C7): one session.
3. **Review B: island** (I1-I10): one to two sessions. A and B share tooling and can run in either order;
   cubic first is quicker because it has fewer unknowns.
4. **Second revision of this plan:** confirmed findings become work items with sizes and an order, alongside the
   spherical phasing.

## 10. Documentation that disagrees with the code

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
- Project `CLAUDE.md` says headless runs "silently skip" GPU tests. Nothing in the code skips them; the
  headless suite filter leaves them out.
