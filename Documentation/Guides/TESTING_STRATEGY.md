# Testing Strategy

> State as of 2026-11-02 (code review CR5).

How the project's automation tests are organised and run: the headless suite, the GPU and render-proxy tests it
cannot run, the in-editor gate that runs them, the mesh digest, the shared fixtures, and the traps the code review
met. Which gate a change needs is decided in `Scripts/qa/README.md`; this guide does not repeat it.

## The suite

Every test is an Unreal automation test (`IMPLEMENT_SIMPLE_AUTOMATION_TEST` and friends), compiled only in
development builds (`WITH_DEV_AUTOMATION_TESTS`; ItemInventoryPlugin, EquipmentPlugin and some of VoxelCharacterPlugin's
use `WITH_AUTOMATION_TESTS`). The project's test roots:

| Root | Plugin |
|---|---|
| `VoxelWorlds.*` | VoxelWorlds (every module), and VoxelWorldPOI under `VoxelWorlds.POI.*` |
| `VoxelCharacter.*` | VoxelCharacterPlugin |
| `Dungeon.*` | ProceduralDungeonPlugin |
| `ItemInventory.*` | ItemInventoryPlugin (crafting and storage included) |
| `Equipment.*` | EquipmentPlugin |
| `Interaction.Channeled.*`, `Interaction.Flow.*`, `Interaction.WorldItem.*` | InteractionPlugin |
| `VoxelWorldSave.*` | VoxelWorldSave |
| `WorldClock.*` | WorldClockPlugin |
| `VoxelEngine.*` | the game module (`Source/VoxelEngine/Private/Testing/`): the framework replication test, in the editor only (below) |

The headless run is `Scripts/qa/run_suite.ps1 [-Tag t] [-Filter roots]`: with the editor closed it runs

```
UnrealEditor-Cmd.exe VoxelEngine.uproject -ExecCmds="Automation RunTests <filter>; Quit" -nullrhi -unattended -nopause -NoSound -abslog=Saved/Logs/qa_<tag>.log
```

with the filter `Dungeon+Equipment+Interaction.Channeled+Interaction.Flow+Interaction.WorldItem+ItemInventory+VoxelCharacter+VoxelWorldSave+VoxelWorlds+WorldClock`,
and prints the wall time, the result counts and every failure. The suite is about 751 tests (CR4 added
`VoxelWorlds.Bench.ParseArgs` and `VoxelWorlds.Config.SettingsOverride` to 745, the framework's authority fixes eight,
VoxelWorlds 8 took four away) and takes about 12 minutes. The filter names `Interaction.Channeled`, `Interaction.Flow`
and `Interaction.WorldItem` rather than `Interaction` because the bare root also catches an
engine Metasound test. Use the D: engine (`D:\Program Files\Epic Games\UE_5.8`, or `UE_ENGINE_ROOT`) and never run the
suite during a build. `logwarnings.py <before> <after>` compares two runs' warning kinds.

A single root or test runs the same way with `-Filter` (for example `-Filter VoxelWorlds.Streaming.SeamRegistry`), or
in a running editor from the console (`Automation RunTests <filter>`), the Session Frontend, or the `unreal-mcp`
server's automation test toolset.

## Where tests live

Each module keeps its tests in a `Tests/` folder, in one of two places:

- at the module root (`Source/<Module>/Tests/`): every VoxelWorlds module (`VoxelCore`, `VoxelGeneration`,
  `VoxelLOD`, `VoxelMap`, `VoxelMeshing`, `VoxelNavigation`, `VoxelPCG`, `VoxelRendering`, `VoxelScatter`,
  `VoxelStreaming`, `VoxelWorldsEditor`), InteractionPlugin, VoxelWorldPOI, VoxelWorldSave and WorldClockPlugin;
- under `Private/Tests/`: EquipmentPlugin, ItemInventoryPlugin, ProceduralDungeonPlugin and VoxelCharacterPlugin.

The code review recorded the split (FW-08 in `Documentation/CODE_REVIEW_PLAN.md` §9.3.10, one placement at the module
root proposed); new tests follow the module's existing folder. A test file is named for what it covers
(`VoxelSeamRegistryTests.cpp`, `VoxelEditRemeshTests.cpp`), and a test's name says the rule it pins, with the review ID
in the file's comment (`VoxelWorlds.Streaming.EditRemesh.Plane`, G56/G57).

The game module's `Source/VoxelEngine/Private/Testing/` holds the world collector (below) and the one test that spans
the framework plugins, `VoxelEngine.Replication.Framework` (below): the plugins may not depend on each other that way,
and CommonGameFramework, under all three, cannot depend on them.

## Tests that need a GPU or a render scene

Under `-nullrhi` there is no GPU to dispatch to and components create no scene proxy. These tests check
`GUsingNullRHI`, log `Skipped: ...` with `AddInfo` and **return success**, so a headless run reports them green
without running them:

| Test | Needs | In the editor gate |
|---|---|---|
| `VoxelWorlds.Generation.GPUNoiseGeneratorAsync` | GPU generation | yes |
| `VoxelWorlds.Generation.GPUvsCPUMaterialParity` | GPU generation | yes |
| `VoxelWorlds.Generation.GPUvsCPUBlendDitherParity` | GPU generation | yes |
| `VoxelWorlds.Generation.GPUvsCPUIslandBowlParity` | GPU generation | yes |
| `VoxelWorlds.Generation.GPUvsCPUCavesParity` | GPU generation | yes |
| `VoxelWorlds.Generation.GPUvsCPUNoiseTypeParity` | GPU generation | yes |
| `VoxelWorlds.Generation.GPUvsCPUWaterParity` | GPU generation | yes |
| `VoxelWorlds.Generation.GPUvsCPUOreVariantParity` | GPU generation | yes |
| `VoxelWorlds.Generation.GPUvsCPUSphericalParity` | GPU generation | yes |
| `VoxelWorlds.Rendering.Crossfade.SwapReachesProxy` | a scene proxy | yes |
| `VoxelWorlds.Streaming.EditReachesProxy.Smooth` | a scene proxy | yes |
| `VoxelWorlds.Streaming.EditReachesProxy.Cubic` | a scene proxy | yes |
| `VoxelWorlds.WorldModes.InfinitePlane.GPUConsistency` | GPU generation | no |
| `VoxelWorlds.Generation.Performance` | GPU generation | no (a timing report, not a check) |

The skip sites are in `Source/VoxelGeneration/Tests/VoxelNoiseGeneratorTests.cpp`,
`Source/VoxelGeneration/Tests/InfinitePlaneWorldModeTests.cpp`, `Source/VoxelRendering/Tests/VoxelCrossfadeProxyTests.cpp`
and `Source/VoxelStreaming/Tests/VoxelEditReachesProxyTests.cpp`. A new test that needs either belongs in the same
pattern and in the editor gate's filter. **A green headless suite says nothing about GPU generation or the renderer.**
The chunk-manager tests stream on the CPU into a recording renderer (below), so they run headless.

## The replication test (PIE, in the editor only)

`VoxelEngine.Replication.Framework` (`Source/VoxelEngine/Private/Testing/FrameworkReplicationTest.cpp`, D-CR23) runs
the framework plugins' flows over the real net driver: it starts Play In Editor with a dedicated server and one client
in the editor process (net mode Play As Client, one process, port 17793, a copy of the user's play settings), on
`/Engine/Maps/Entry` with its own game mode, and drives both worlds from one latent command, a step per editor frame:

1. both worlds up and the client's pawn possessed (120 s), the test's item definitions registered in both machines'
   item databases, the server spawning a chest with three ore in it and a held interactable;
2. the chest's ore on the client at its first replication;
3. an add on the server reaching the client's bag through the fast array, its callback firing there;
4. an equip the client asks for: the RPC, the server's checks, the sword out of both bags and in the main hand on both,
   `OnRep_EquipmentSlots` on the client;
5. a move the client asks for (Pending on the client, the slot changed on both), then one into the chest nobody opened:
   `NotAuthorized` back through `ClientRPC_OperationFailed`, and after a 0.5 s hold nothing changed on either machine;
6. a held interaction: the server runs the handler once, after the option's 0.5 s hold, with its copy of the player, and
   `ClientRPC_InteractionResult` ends the client's channel;
7. PIE stopped and its end awaited.

Every step but the first has a 20 s deadline (a step takes a few frames on an idle machine) and a failure is an error
naming its step (`Step '5 client equip request': timed out after 20 s waiting for ...`); a failure stops PIE. The actors
are test-only classes in the game module (`FrameworkReplicationTestActors.h`), configured in their constructors so both
machines agree on what does not replicate. The test needs the editor and a renderer: `CanRunInEnvironment` skips it
under `-nullrhi`, in a commandlet and outside the editor, and its root is outside the headless suite's filter. Stop any
PIE session before running it. The in-editor gate runs it.

## The in-editor gate

`Scripts/qa/editor_gate.py <tag> [--port=8081] [--record=<file>]` runs those tests in a running editor and checks the
GPU against the CPU:

1. It waits for an editor on a Claudius port (`Scripts/qa/launch_gate_editor.ps1 -Port 8081` starts one beside the
   main checkout's editor on 8080) and turns off its background throttle (`disable_bg_throttle.py`: an unfocused
   editor pins itself to 3 FPS and a run stalls).
2. It runs `Automation RunTests VoxelWorlds.Generation.GPUvsCPU+VoxelWorlds.Generation.GPUNoiseGeneratorAsync+VoxelWorlds.Rendering.Crossfade+VoxelWorlds.Streaming.EditReachesProxy+VoxelEngine.Replication.Framework`:
   the 12 GPU and proxy tests, the six crossfade tests that also run headless and the replication test, **19 results**,
   each expected to be a single `Success`.
3. It extracts the **parity lines** (the parity tests' `[N voxels]: density~` lines, one per case and LOD: density,
   material, biome and metadata agreement and the GPU's CRC) and compares all **111** with the record,
   `Documentation/CodeReview/VW8/gpu_parity_lines_2026-11-02_generation.txt` (`parity_lines.py`; CR2's 114 less the
   three `BlendDither off` cases, gone with `voxel.Biome.BlendDither` in VoxelWorlds 8, and 24 lines moved by the shader
   cleanup of G97 and G88, `Features/GPU_PIPELINE.md`). A changed line is a finding to
   explain, not a tolerance to widen: a change that should not touch generation leaves every line identical.
4. It copies the editor's log to `Saved/QA/editor/`.

This is the gate for anything touching meshing, generation or rendering, and for the framework plugins' RPCs and
replicated state. The same tests can be run through
`unreal-mcp`'s automation test toolset; the parity comparison then needs `parity_lines.py` on the editor's log.

## The mesh digest

`VoxelWorlds.Meshing.Digest` (`Source/VoxelMeshing/Tests/MeshingDigestTests.cpp`, pure CPU, runs headless) meshes
fixed scenes through every CPU meshing path: both smooth meshers' chunk meshes at LOD 0, 1 and 2 in the Interior and
WholeChunk domains (with every neighbour plane, and with most missing), Marching Cubes' transition ribbon and geomorph
along +Z and a tilted up, both meshers' face, edge and corner seams at uniform and mixed LODs, each under the default
cvars and with each meshing cvar off. It logs one line per case,

```
[MeshDigest] <cvar state>.<mesher>.<terrain>.<path>.<lod> crc=<hex> v=<vertices> i=<indices>
```

and a total. `Scripts/qa/mesh_digest.py <a.log> <b.log>` compares two suite logs' lines (1,457 since CR3): a change
that should leave the meshers' output alone leaves every line identical. The first record is
`Documentation/CodeReview/CR2/mesh_digest_2026-10-31.txt`.

## Test worlds and the garbage collector

Tests that build worlds (`UWorld::CreateWorld`, then `DestroyWorld`) leave them to the garbage collector, and nothing
collects garbage during an automation run. In the editor every test world keeps its render scene alive, and at about
128 of them the D3D12 device is removed (G38). The game module collects garbage when a test ends with at least
`vox.Tests.GCEveryWorlds` (32) worlds cleaned up since the last collection
(`Source/VoxelEngine/Private/Testing/AutomationWorldCollector.cpp`; 0 = never). Keep it on for in-editor runs. A test
that builds a world destroys it (or lets its fixture's destructor do so) and holds no pointer into it afterwards.

## Shared fixtures

Code review CR2 (row 17) folded each plugin's private copies into one fixture header per plugin:

| Fixture | Header | Gives |
|---|---|---|
| `VoxelTesting::FTestChunkWorld` | `Source/VoxelStreaming/Public/Testing/VoxelTestChunkWorld.h` | a transient game world with a real chunk manager on an actor: `Create`, `ConfigureSmallPlane` / `ConfigureSmallPlanet` / `ConfigureThreeLODBands`, `StartManager`, `SetViewer`, `Settle`, `Stop` (the destructor stops too); CPU generation, no collision, scatter or caves by default |
| `VoxelTesting::FRecordingMeshRenderer` | same | the tests' only mesh renderer: the latest mesh per chunk and its LOD, the seam bucket per owner, the water tiles, submit counts; `SimulateProxyRecreated` drops everything as a recreated scene proxy does |
| `VoxelTesting::FScopedCVar` | `Source/VoxelCore/Public/Testing/VoxelTestScopedCVar.h` | a console variable set for a scope (see the traps below) |
| `VoxelMeshingTestFixtures` | `Source/VoxelMeshing/Tests/VoxelMeshingTestFixtures.h` | the test chunk, both meshers' configurations, voxels from a solid predicate, the closed-surface checks' keys |
| `VoxelScatterTesting` | `Source/VoxelScatter/Tests/VoxelScatterTestFixtures.h`, `Source/VoxelScatter/Public/Testing/VoxelScatterTestDefinitions.h` | a world with a scatter manager, heightmap chunk voxels per column, the scatter definitions the tests use |
| `VoxelFarFieldTestRig` | `Source/VoxelStreaming/Tests/VoxelFarFieldTestRig.h` | a far field with synchronous builds for the sphere and the plane |
| `VCTesting::FPlayWorld` | `Plugins/VoxelCharacterPlugin/Source/VoxelCharacterPlugin/Public/Testing/VCTestWorld.h` | a world that has begun play, for characters, controllers and interactables (VoxelWorldPOI uses it too) |
| `DungeonCoreTesting`, `DungeonVoxelTesting` | `DungeonCoreTestFixtures.h`, `DungeonVoxelTestFixtures.h` under ProceduralDungeonPlugin's `Private/Tests/` | dungeon configurations, results and tile sets; a voxel world for stamps |

A suite with its own configuration derives from the shared fixture and keeps only what differs. A few suites keep a
hand-built world for a reason recorded in `Documentation/CODE_REVIEW_PLAN.md` §16.2.

## Traps the code review met

- **`AddExpectedMessage` patterns are regular expressions** (`IsRegex` defaults to true). A message with `[`, `(`,
  `.` or `+` in it does not match itself: CR4's `Bench.ParseArgs` first expected `[:Alt]`, a character class. Escape
  the pattern or use `AddExpectedMessagePlain`.
- **Set cvars with `VoxelTesting::FScopedCVar`, never `Set(..., ECVF_SetByCode)` by hand.** A set below the priority a
  variable already holds is ignored with only a console-manager log line ("ignored as it is lower priority"): once any
  earlier test in the run (or a person at the console) set it from the console, a later set by code silently does
  nothing and the test runs on the wrong rule. In CR2 that made the mesh digest's rule-before cases run on the current
  rule inside the full suite (546 lines differed from the digest-only record). `FScopedCVar` sets and restores at the
  priority the variable holds.
- **A world a test builds is left to the garbage collector** (above): in the editor, keep `vox.Tests.GCEveryWorlds`
  on, and do not keep pointers into a destroyed world.
- **GPU and proxy tests pass headless by skipping** (above): run the editor gate.
- **An unfocused editor runs at 3 FPS**: in-editor runs and PIE numbers are meaningless until `disable_bg_throttle.py`
  has run.
- **Benchmarks are not tests.** Streaming performance is compared with `bench_gate.py` (three runs per route at the
  same PIE viewport) and `bench_table.py`, never inferred from the suite's wall time.

## Which gate

`Scripts/qa/README.md` maps each kind of change to its gate: documentation only, any code (build warnings, the suite,
a non-unity build for code moves), meshing, generation and rendering (add the editor gate and the mesh digest),
scatter, the map, rendering captures, streaming benches, headers. Both sides of a comparison are built and run in the
same checkout, and never in the user's main checkout while they work in it.
