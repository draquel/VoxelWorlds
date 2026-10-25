# Visual polish plan (VP1-VP3)

Three reports from a run around `PlanetTest` (2026-10-24), to fix before making media of the project:

1. "The sky flickers and moves strangely as the camera moves. Unsure if it is the skybox or the clouds."
2. "The terrain materials are extremely noisy. It is very distracting and makes the terrain unconvincing." Every smooth
   mode uses the same materials.
3. "The shadows made by hillsides disappear when the terrain is occluded, so shadows appear and disappear as you
   rotate." Every mode.

This plan records what the level, the materials and the renderer do, separates what the code confirms from what has to
be shown live, and lists the options for each. Milestone 1 (the shadow cull fix, gradient sampling and the sky A/B
session) is done; its results are in "Milestone 1 results" below. File references are relative to the UE5_PluginDev
root; line numbers in the findings are as of VoxelWorlds `952d4ae`, WorldClockPlugin `99295a9` and the parent `c91ff9d`.

## Summary

| ID | Report | Cause | Status | Modes | Repos | Size |
|----|--------|-------|--------|-------|-------|------|
| VP1 | Sky flickers and moves | The terrain's (and trees') shadows cast onto the atmosphere through the sun's cascades (F1.7), redrawn as the cascades re-fit and, before VP3-O1, as casters were culled with the view; the sky light captured at the planet's centre leaves every shadow black (F1.4). The volumetric clouds, the first suspect, are not visible on `PlanetTest` | Both shown live 2026-10-24; VP3-O1 removes about 80% of the sky's change from the shadows; F1.4 **fixed** (VP1-O5, `wc.Planet.SkyLightAtViewer`); VP1-O6 open | Planets (small ones most) | WorldClockPlugin, parent (maps) | S |
| VP2 | Terrain materials noisy | Every terrain texture sampled at mip 0 at every distance (F2.1); roughness 0, a mirror (F2.7) | F2.1 **fixed** (VP2-O1, `voxel.Material.GradientSampling`); F2.7 **fixed** (VP2-O6, `voxel.Material.ConstantRoughness`); F2.8 **fixed** (VP2-O2, `voxel.Material.TileVariation`, `MacroVariation`) | Every smooth mode (DC, MC); the cubic atlas too | VoxelWorlds (materials) | S, then M for the look |
| VP3 | Hill shadows come and go | The voxel scene proxy culled shadow casters with the camera's frustum (F3.1) | **Fixed** (VP3-O1, `voxel.Render.ShadowCullFrustum`); F3.2 and F3.3 open | Every mode | VoxelWorlds (VoxelRendering) | S; the shadow range and Lumen items M-L |
| VP4 | Fast terrain flicker as you move | A far-field patch waiting for a build where nothing of it would show was stood in for by its nearest built ancestor, often a cube face's root, drawn over the voxel terrain and the horizon for a frame (F4.1) | **Fixed** (VP4-O1, `voxel.World.FarFieldAvoidStandIns`); see "VP4 terrain flicker" | Planets | VoxelWorlds (VoxelStreaming) | S |

## Milestone 1 results (2026-10-24)

Branch `feat/vp-milestone1` in VoxelWorlds and the parent. All on `PlanetTest` in PIE (DX12, 2874 x 1360, TSR), pawn
at 45°N 20°E unless noted, the clock stopped (`vox.TimeScale 0`). Images in `Saved/Screenshots/Claudius/vp*`; the
scripts in `Saved/ClaudeScripts/` (`vp_sun_camera.py` poses a survey camera in the pawn's local frame relative to the
sun, `vp_image_metrics.py` is tooling item T1, `vp2_ab.sh`, `vp1_sweep.sh`, `vp1_walk.sh`, `vp_bench_ab.sh`).

### VP3-O1: shadow casters culled by the shadow's volume

`FVoxelSceneProxy::GetDynamicMeshElements` culls each chunk and seam bucket per view through `FVoxelViewCull`: the
view's frustum, or in a shadow gather the caster volume from `GetDynamicMeshElementsShadowCullFrustum` (boxes moved by
`GetPreShadowTranslation`). A shadow gather also skips the RVT clones and water. `voxel.Render.ShadowCullFrustum`
(1; 0 = before). `FVoxelBatchCounters::FCounts::ShadowDrawn` and the bench's `ShadowBatchN` column and
`shadowBatchesDrawnP50/P95/Max` count the batches drawn into shadow maps.

- **Repro:** at 16:50 (sun 12° up) a camera 4,000 uu over the pawn looking straight down, the hill casting the shadow
  outside its view. With the fix half the frame lies in the hill's shadow; with `voxel.Render.ShadowCullFrustum 0`
  the shadow is gone and only the trees' shadows remain (`vp3_nadir_1650_cull1b.png`, `_cull0.png`).
- **Cost** (bench `300 3000 gc=45:20:90:2000`, a warm-up then three runs per setting, alternating): voxel batches
  drawn into shadow maps per frame 1,070 → 335 and all voxel batches 1,379 → 645 (the sun's cascades reach 20,000 uu,
  the camera's frustum past 40,000, and the RVT clones are no longer emitted for shadows); GPU frame P50 5.27-5.34 →
  5.14-5.19 ms, P95 5.95-6.08 → 5.78-6.02; frame P95 24.5-25.1 → 25.2-25.3 ms (vsync-bound P50 16.67 either way).

### VP2-O1: footprint sampling

Both triplanar functions sample with `SampleGrad`, the UV derivatives from the translated world position (continuous;
the tiled position of P6.2 jumps by a period), with the arrays' own anisotropic samplers. The switch is
`MPC_VoxelMaterial`'s `GradientSampling` (new asset, `/VoxelWorlds/Materials/`), read by a collection parameter inside
each function; `voxel.Material.GradientSampling` (1; 0 = mip 0 as before) sets it in every game and PIE world through a
console variable sink, and each world component applies it when its scene proxy is created. `MATERIAL_SYSTEM.md`
"Footprint Sampling" and `MASTER_MATERIAL_SETUP.md` carry the new node code.

| View (scatter hidden) | Shimmer, mip 0 → gradients | Ground high-frequency energy with TSR, mip 0 → gradients |
|-----------------------|----------------------------|----------------------------------------------------------|
| Eye level, ground 3,000 ahead | 43.8 → 1.6 | 57.2 → 7.1 |
| Grazing, 30,000 ahead | 43.9 → 1.6 | 55.3 → 7.3 |
| 3,500 over the pawn | 45.0 → 0.6 | 58.8 → 3.5 |

Shimmer: mean absolute difference (luma, of 255) over the ground band between two anti-aliasing-off frames a 0.01°
turn apart (about a quarter pixel): how much the ground changes under a tiny camera move. The eye-level view through
the cvar after the rebuild gives the same (43.8 → 1.56). Base colour means match (139.9/138.7/74.1 against
140.2/145.1/79.1 RGB), so the mips are unbiased; the lit frames come out brighter because the normals are no longer
noise. GPU frame P50 5.11-5.22 → 4.94-5.07 ms over the bench route (three runs each): no extra cost.

### VP1: the sky A/B session

- **The volumetric clouds are not the flicker.** In every view tried (45°N 20°E eye level, looking up, from 4,000 and
  9,000 uu over the pawn, inside the 6,500-10,800 uu layer; 88°N) clouds on against `showflag.Cloud 0` differ by under
  1 luma, and a sweep looking up gives the same frame-to-frame change with clouds on, off, or traced at full
  resolution once the first frame settles. F1.1-F1.3 stand as written but do not show on `PlanetTest`.
- **F1.7 (new): shadows on the atmosphere.** The blocky dark shape in the sky over the hills (`vp1_mode0.png`, upper
  right) stays with clouds off and goes with `showflag.DynamicShadows 0`: the sun has `bCastShadowsOnAtmosphere` on,
  so the terrain's and the trees' shadows darken the air, sampled from the cascades (hence the staircase), and in the
  scaled atmosphere (about 230 times Earth's density near the ground) they show strongly. Measured as the mean
  frame-to-frame change over the sky band:

  | Sweep | Old culling | VP3-O1 | Atmosphere shadows off |
  |-------|-------------|--------|------------------------|
  | Turning, 4° steps at eye level | 3.28 | 2.15 | 1.83 |
  | Walking, 150 uu steps | 1.14 | 0.77 | 0.72 |

  The last column is the change the motion itself makes. With the old culling the shadowed air changed with every
  turn as casters were culled with the view; with VP3-O1 what is left over the baseline is 0.32 turning and 0.05
  walking: the cascades re-fitting.
- **F1.4 confirmed.** With the sky light at the planet's centre, ground in a terrain shadow is pure black (luma 0.0,
  `vp1_sky_centre.png`); with it moved 300 uu over the pawn the same ground is lit by the sky (104.5) and the sunlit
  ground rises from 103.3 to 163.8 (`vp1_sky_pawn.png`). The real-time capture from inside the planet gives no sky
  light at all.
- Not tested: exposure pumping (F1.6).

### New findings

- **F2.7 Terrain roughness 0.** `M_VoxelMaster`'s roughness is the sampled roughness times `RoughnessMultiplier`, 0 in
  the master and in `MI_VoxelDefault`, so the roughness buffer reads 0 on the ground: a mirror (the `Specular` 0.05 of
  VoxelWorlds#26 dims it head-on, but Fresnel takes it back at grazing angles). With the noise gone, the sky's
  reflection shows as pale, hard-edged patches on far, flat ground (`vp2_graze_g1_tsr.png`); `showflag.Specular 0`
  removes them. With the multiplier at 1 the ground reads 0.91 (std 0.01) and the patches go, but that value comes from
  the placeholder slice (no terrain material has a roughness map; empty slots take the first valid texture, the coal
  ore's ORD map, whose R is occlusion). Fix as **VP2-O6**: a constant terrain roughness (about 0.9) in the master until
  real roughness maps exist. Not applied.
- **F2.8 Repetition visible.** With the noise gone the one-metre tiles show as a regular grid on flat ground at mid
  distance (VP2-O2).
- **F3.4 Chunk interiors lost while streaming is paused.** After toggling `r.Lumen.*.Allow` with `vox.Freeze 1
  streaming` on, the frame showed only seam strips: the scene proxy was recreated and the chunk manager, paused, did
  not re-send the chunks (`RecoverLostRenderData`, G15) until streaming resumed. Debug-only; worth a line in the
  freeze command's help, or recovery that runs while paused.

### VP1-O5 and VP2-O6 (2026-10-24, same branch)

**VP1-O5, the sky light at the viewer** (WorldClockPlugin): with a planet frame `ADayNightController` moves the sky
light to the camera every tick (`bPlanetSkyLightAtViewer`, `ApplyPlanetSkyLight`) and turns its lower hemisphere solid
colour off: "lower" is world −Z, which on a planet can be half the sky, and the atmosphere draws the planet's ground under
the local horizon. `wc.Planet.SkyLightAtViewer` (1; 0 = the sky light where the map put it, as before). Test:
`WorldClock.Planet.Sky` extended.

| Where (16:50 local) | Darkest fifth of the ground, before → after | Brightest fifth |
|---------------------|-------------------------------------------|-----------------|
| 45°N 20°E, from 4,000 uu straight down (a hill's shadow) | 0.0 → 69.1 | 124.1 → 197.5 |
| 45°N 20°E, eye level (foreground in the hill's shadow) | 0.0 → 12.2 | 19.1 → 163.6 |
| 0°N 20°E, eye level in a forest (light moved by hand in PIE) | 27.7 → 35.1 at the viewer, 39.5 with the whole sphere | 173.1 → 174.6 |

At eye level the shadowed foreground goes from black to sky-lit grass (`vp1o5_45n_eye_ab.png`). Walking 150 uu steps
the frame changes as much either way (ground 30.2 a step with the sky light following, 30.5 without; sky band 3.5 and
3.1), so the capture moving with the camera adds no visible instability.

**VP2-O6, constant terrain roughness** (both masters): roughness = `lerp(sampled x RoughnessMultiplier,
TerrainRoughness, ConstantRoughness)`, `TerrainRoughness` a scalar parameter (0.9), `ConstantRoughness` in
`MPC_VoxelMaterial`, set by `voxel.Material.ConstantRoughness` (1; 0 = as before). At the grazing view on `PlanetTest`
(scatter hidden) the roughness buffer reads 0 → 0.9; the far ground, which with the sky light now reflected the sky as a
pale blue sheen with yellow streaks, is matte (blue channel 112 → 86, saturation 41.6 → 66.0; `vp2o6_graze_ab.png`).

**`VoxelDemo`** (a heightmap world; the sky light change does not apply there) with the mip-0 sampling and mirror
roughness against both fixes: the far hill's speckle and the wet-looking sheen on the dirt slope by the lake are gone
(`vp_demo_ab.png`).

Tests: `WorldClock.*` and `VoxelWorlds.Rendering.*` in the editor, 16/16.

### The first-PIE crash and the material atlas arrays (2026-10-24, after the merge)

**The crash.** A PIE run on `PlanetTest` on main crashed (`Saved/Crashes/UECC-*`, read in Rider with the 5.8 editor
symbols). On an RHI translate worker, a Lumen card-capture draw set a pipeline whose vertex shader had already been
freed and its memory reused: `UE::RHICore::ApplyStaticUniformBuffers` (`RHICoreShader.h:74`, called from
`FD3D12CommandContext::RHISetGraphicsPipelineState`) caught the mismatch, and formatting the check's message read the
freed shader's name and faulted. No project code was on any thread's stack, the minidump holds too little heap to name
the material, and nothing in the log shows shaders compiling or garbage collection in that run. It has not reproduced in
about 22 PIE starts (with and without RHI validation). It was the first PIE of the editor session, 18 s after the atlas
finished building its texture arrays; a forum report describes the same kind of crash on 5.8.3 and not on 5.8.1.

**F2.10 (new): the atlas arrays were 4096 RGBA32F.** `UVoxelMaterialAtlas::BuildSingleTextureArray` handed its slices to
`UTexture2DArray::UpdateSourceFromSourceTextures()`, which sizes the array to its largest slice and, when the slices'
gamma spaces or formats differ, converts all of them to RGBA32F. `TextureArraySize` (512 on `DemoMaterialAtlas`) was
never applied, so "512 px slices" in "The setup as found" was wrong: the albedo and normal arrays were 4096 x 4096 x 22
float (5.9 GB of source each, held for the editor session) and the roughness array 2048. Every editor session's first
PIE spent 20-24 s building them on the game thread; on the GPU they took 800 MB (BC, 246 + 492 + 62 MB).

**Fix.** The atlas builds the array's source itself: every slice at `TextureArraySize` (rounded up to a power of two) in
BGRA8, converted from its texture's own gamma into the array's (sRGB albedo, linear normals and roughness, so the
shader samples the same values) with the engine's gamma-correct resize, wrapping at the edges; each distinct texture is
decoded once (the demo atlas repeats one texture in 14 of 22 slots); slots without a texture take the first built
slice, as before. The settings are pinned per array instead of copied from whichever texture lands in slice 0: albedo
`TC_Default` (BC1/BC3), normals `TC_Normalmap` (BC5) in the normal-map texture group, roughness `TC_Masks` (BC1/BC3,
linear). One `UpdateResource` instead of two, and each build gets a new object name (`NewObject` with the old name
would reconstruct the array the materials still draw with in place). Tests `VoxelWorlds.MaterialAtlas.TextureArrays.Build`
and `.Rebuild`.

Measured in a fresh editor on `PlanetTest` per run (`Scripts/launch_editor.ps1`, settled, then the session's first PIE;
`Saved/ClaudeScripts/atlas_pie_measure.sh`, results in `atlas_measure_*.txt`); memory is the editor's working set /
committed bytes:

| | Before, run 1 | Before, run 2 | After, run 1 (not cached) | After, run 2 (cached) |
|---|---|---|---|---|
| PIE start, as the engine reports it | 25.8 s | 21.2 s | 3.6 s | 3.4 s |
| Building the three arrays | 24.3 s | 20.0 s | 2.3 s | 2.2 s |
| Editor before PIE | 4.5 / 5.6 GB | 4.6 / 5.6 GB | 4.5 / 5.6 GB | 4.5 / 5.6 GB |
| Right after PIE start | 18.5 / 21.9 GB | 18.2 / 21.7 GB | 4.7 / 7.5 GB | 4.6 / 7.4 GB |
| 45 s later (streaming) | 17.7 / 21.6 GB | 17.5 / 21.4 GB | 6.0 / 9.3 GB | 5.9 / 9.2 GB |
| Peak committed | 23.1 GB | 23.1 GB | 9.4 GB | 9.4 GB |
| Arrays on the GPU | 800 MB | 800 MB | 16 MB | 16 MB |

The arrays' platform data was ready when PIE finished starting in every run (the 4096 arrays came from the DDC; the 512
ones build in under a second). `BakeMapColors` still takes about 1 s of the 3.4 s: it decodes every full-size albedo.

**The look.** At the player's view heights the 512 slices look the same as 4096: the ground's fine detail (mean
absolute luma step between neighbouring pixels) at the 160 uu view is 18.85 against 18.96 for 4096 slices built by the
new code in the same session, 18.40 against 18.79 from 350 uu straight down (`atlas_before_*`, `atlas_after_*`,
`atlas_ref4096_*`). The new code at 4096 reproduces the old images (18.96 against 18.95). From 60 uu over the ground the
512 slices are softer (19.2 against 23.2, `atlas_after512_near.png` against `atlas_ref4096_near.png`). If close-ups
matter for media, `TextureArraySize` 1024 costs 64 MB on the GPU and about four times the build.

**Full crash dumps.** The editor is launched with `-fullcrashdump` (`Scripts/launch_editor.ps1`, and the Rider run
configuration's program arguments, which are local settings), so the next crash writes a full-memory dump that can
name the freed shader's material. A full dump is about the editor's working set.

### F2.11: the atlas arrays saved with the atlas (2026-10-24)

**Confirmed in a package.** A Development Win64 package of `PlanetTest` (`BuildCookRun -map=/Game/PluginTesting/Planet/PlanetTest`,
cooked for SM6 only, see "Found on the way") logged "Texture arrays built successfully" 7 ms after "Building texture
arrays": outside the editor `BuildSingleTextureArray` made array objects with no source and no platform data and bound
them. The terrain drew black: under the grass at the spawn and on every far hill (mean luma of the slope right of the
spawn 57; the terrain's strips black with instanced meshes hidden).

**Fix (the user's choices, 2026-10-24: the editor rebuilds stale arrays; the arrays in Git LFS).** The arrays are assets
next to the atlas, `<Atlas>_AlbedoArray`, `_NormalArray` and `_RoughnessArray`, referenced by it and cooked with it:

- `BuildTextureArrays()` creates them or updates them in place (`PreEditChange` / `PostEditChange`, as a reimport does)
  with F2.10's source build unchanged (formats, gamma, dedup, placeholders), and leaves the atlas and its arrays unsaved.
  A transient atlas (the tests) still gets transient arrays. Outside the editor it does nothing, so a packaged game keeps
  the saved arrays.
- The atlas saves a hash of the build's inputs (`TextureArrayBuildKey`, editor-only): `TextureArraySize`, a build
  version, each config's MaterialID and texture paths, and each texture's saved package hash from the asset registry
  (a loaded texture with unsaved changes, or a transient one, by its source id). Checking it loads no texture.
- A world binding the atlas calls `EnsureTextureArrays()` (`UVoxelWorldComponent` and `FVoxelPMCRenderer`'s
  `UpdateMaterialAtlasParameters`): in the editor missing or stale arrays are rebuilt there, with a warning naming the
  atlas and that they need saving; up-to-date ones are only bound. Outside the editor an atlas with source textures
  and no arrays logs one error.
- Cooking an atlas (`PreSave` with a target platform) logs an error when an array is missing, which fails the cook (a
  cook fails on any logged error), and a warning when they are stale. `IsDataValid` reports both as errors.
- `Scripts/rebuild_atlas_arrays.py` rebuilds and saves every atlas under `/Game`, headless or in a running editor
  (`--only-stale`). It built `DemoMaterialAtlas` (22 slices) and `Config/VoxelMaterialAtlas` (24; the third path,
  `Content/PluginTesting/VoxelMaterialAtlas`, is a redirector to it).

Tests: `VoxelWorlds.MaterialAtlas.TextureArrays.{Build,Rebuild}` unchanged (a transient atlas) and the new `.SavedAssets`
(an atlas asset in `/Temp`: the arrays are assets next to it, rebuilt in place, stale after a new `TextureArraySize` or
a source texture's new content, `EnsureTextureArrays()` rebuilding only then, an array with no texture left dropped, an
atlas with no source texture never stale). The six suites: 634 tests headless and in the editor.

**Results.**

- The package after the fix draws the terrain textured: the same view's slope 57 → 107 mean luma, the far hills lit
  (`f211_package_before_after.png`); the cook found the saved arrays up to date (no warning).
- The editor's first PIE on `PlanetTest` (`atlas_pie_measure.sh` in a fresh editor, settled): **0.28 s** against F2.10's
  3.4-3.6 s and 21-26 s before it; nothing built, nothing left dirty; the editor at 4.4 GB working set / 5.8 GB
  committed right after the PIE start (F2.10: 4.6-4.7 / 7.4-7.5). The atlas read as up to date in a new session from the
  asset registry, with no source texture loaded.
- Stale arrays, live (`TextureArraySize` changed in memory, then back): the PIE start rebuilt them in place with the
  warning, 4.2 s at 256 and 3.4 s at 512, about F2.10's every first PIE; the terrain drew normally after each rebuild.
- On disk each array is 11-14 MB (`DemoMaterialAtlas`'s three 38.8 MB, `VoxelMaterialAtlas`'s 39.2 MB): the source is
  saved delta-coded and Kraken-compressed, about half of 512 x 512 x 4 bytes a slice, and the repeated slices (14 of 22
  albedo and normal slices, 19 of 22 roughness slices copy another) do not compress away. Every save of a rebuild writes
  new bytes even for the same content. In LFS (`.gitattributes`); the repo held 18 MB of LFS objects before.

**Found on the way.**

- WorldClockPlugin did not compile for a game target: `ADayNightController` called `ADirectionalLight::GetComponent()`,
  which is editor-only. It now takes `ALight::GetLightComponent()`. No package of this project could be built before.
- A cook for the project's `D3D11TargetedShaderFormats=PCD3D_SM5` compiles the voxel compute shaders with FXC for a very
  long time on a cold cache: `FDCEdgeCrossingCS` 911 s, `FGenerateVoxelDensityCS` 980 s, and the Marching Cubes mesh
  shaders (`FGenerateMarchingCubesMeshCS`, `FGenerateTransitionCellsCS`) still compiling after 31 minutes, when the
  cook was stopped. The verification cooks here dropped SM5 in the worktree's config only (SM6: a 5 minute cook).
  Measured and fixed the next day (the user's choice: keep DX11, change the shaders): the SM5 global shader step took
  1 h 51 min and then failed on an FXC internal error, against 42 s after; see "F2.13: the voxel compute shaders on FXC".
- A cook fails on any logged error: with another process on 127.0.0.1:8080, Claudius's listener in the cook commandlet
  logs "unable to bind" and the cook exits `Error_UnknownCookFailure`. Pass
  `-AdditionalCookerOptions=-ini:Editor:[/Script/Claudius.ClaudiusSettings]:bEnableHttpServer=False` then.
- The atlas's source textures (`MaterialConfigs`' soft references, 4096 and 2K) are still cooked into the package,
  though the game reads only the arrays and the baked map colours.
- The repeated slices could be dropped altogether: a slice per distinct texture and the material's slice in the LUT
  would make the arrays on disk and on the GPU about a third of today's on the demo atlas (needs the masters' triplanar
  functions to read the slice from the LUT). Done the same day: "F2.12: one slice per distinct texture".

### F2.12: one slice per distinct texture (2026-10-24)

**What the arrays held.** `BuildSingleTextureArray` wrote a slice per MaterialID up to the highest configured, and a
material without a texture (or with no readable source) got a full copy of the first built slice. On `DemoMaterialAtlas`
that is 14 of 22 albedo and normal slices and 19 of 22 roughness slices (14 of its 22 materials have no albedo, only
three have a roughness map); on `VoxelMaterialAtlas` 14 of 24 of each. The copies do not compress away on disk and are
uploaded like any slice.

**Fix.** Each array holds one slice per distinct texture with readable source data, in MaterialID order, and the atlas
saves each MaterialID's slice per array (`AlbedoArraySlices`, `NormalArraySlices`, `RoughnessArraySlices`: 256 entries,
written by `BuildTextureArrays()` with the arrays, not editor-only, so a package builds its LUT from them). The kinds
are independent (a material may have an albedo and no normal map). A material without a texture, and a MaterialID with
no config below the highest configured, reads the first slice; one above the highest configured the highest's slice:
exactly what the copies and the sampler's clamp of the array index gave them, so the terrain draws the same.

- `BuildMaterialLUT` grows the LUT from 256 x 3 to 256 x 4: row 3 is the slice row, R the albedo slice, G the normal
  slice, B the roughness slice. Empty tables (arrays built before this) give every MaterialID the slice of its own index.
  `BuildTextureArrays()` rewrites an existing LUT in place, so the materials bound to it read the new slices with the new
  arrays (the bind order builds the LUT before `EnsureTextureArrays()` rebuilds stale arrays). The transient LUT keeps
  its CPU copy after upload (its bulk data is not single-use), so the rewrite can lock it.
- `MF_TriplanarSampleAlbedoRoughness` and `MF_TriplanarSampleNormal` take a `MaterialLUT` texture input, wired in
  `M_VoxelMaster` and `M_VoxelMaster_Fade` from the `MaterialLUT` parameter the cubic path already reads, and their Custom
  nodes `Load` the material's slices from row 3 and sample each array at its own slice. `MF_CubicAtlasSample` read the
  LUT at V = (FaceType + 0.5) / 3, which a fourth row would have shifted; it reads the same texels by `Load` now. The
  far field's `FarFieldMaterialPick` hands the triplanar functions a MaterialID like any voxel vertex: no change.
  `Scripts/setup_texture_array_slices.py` (headless, idempotent) made the edits; it rewrites only the array reads of
  the Custom nodes' code, so it applies over VP2-O2's texture variation as well (checked on that code offline).
- `TextureArrayBuildVersion` 2: every saved array is stale, and `Scripts/rebuild_atlas_arrays.py` rebuilt and saved both
  atlases. `GetArrayIndex` returns the albedo slice.

Tests: `VoxelWorlds.MaterialAtlas.TextureArrays.Build` (one slice per distinct texture: two albedo slices for four
materials, a texture used twice read by both, a material without one on the first slice) and `.SavedAssets` (the slice
tables built with the arrays and saved) changed; new `.SliceTables` (gaps, a texture with no source data, the highest
configured MaterialID and above, independent kinds, one placeholder slice when nothing is readable, the LUT's slice row
equal to the tables for every MaterialID with the face rows' column, row, UV scale and flags unchanged, the identity
before a build, and a rebuild after a new texture rewriting the same LUT object).

**Results.**

| | Before (a slice per MaterialID) | After (a slice per texture) |
|---|---|---|
| `DemoMaterialAtlas` slices (albedo, normal, roughness) | 22, 22, 22 | 8, 8, 3 |
| `DemoMaterialAtlas` arrays on disk | 13.3 + 14.1 + 11.4 = 38.8 MB | 3.3 + 3.8 + 0.7 = 7.9 MB |
| `DemoMaterialAtlas` arrays on the GPU | 4.0 + 7.8 + 4.0 = 15.8 MB | 1.4 + 2.8 + 0.6 = 4.8 MB |
| `VoxelMaterialAtlas` slices | 24, 24, 24 | 10, 10, 10 |
| `VoxelMaterialAtlas` arrays on disk | 12.2 + 14.4 + 12.6 = 39.2 MB | 4.6 + 5.3 + 4.4 = 14.3 MB |
| `VoxelMaterialAtlas` arrays on the GPU | 4.3 + 8.5 + 4.3 = 17.1 MB | 1.8 + 3.5 + 1.8 = 7.2 MB |

GPU sizes are `UTexture::GetMemorySize` of the arrays in PIE (BC1 albedo and roughness, BC5 normals, full mips): 31% and
42% of before. On disk the demo atlas's arrays are a fifth of before (its roughness array keeps only the three ore maps),
`VoxelMaterialAtlas`'s 36%; in Git LFS the next rebuild of an atlas adds about that much again, not 39 MB.

The terrain, before and after, in PIE (the same editor build for each side, fixed survey camera poses, the clock stopped,
AA off, the PIE paused for each shot; five poses each on `VoxelDemo` (`DemoMaterialAtlas`), `PlanetTest` (the same
atlas, the far field's blend off so it does not dither by frame) and `VoxelWorldsTest` (`VoxelMaterialAtlas`, the same
terrain), each shot lit, unlit (albedo), and as the world-normal and roughness buffers with the sampled roughness drawn
(`voxel.Material.ConstantRoughness 0`, `RoughnessMultiplier` 1)): the normal and roughness buffers are identical on every
pixel but the player pawn's (its idle pose and spawn differ by run) and two 2-4 pixel specks of distant geometry; the
unlit images are identical on every terrain pixel outside the lake (translucent water animates on its own clock) and
the pawn, 0 to 8 pixels a view differing by 1 of 255 (the lake's edge); `PlanetTest`'s near view, with no water, is
identical on every pixel. The cubic path (`Review_Cubic` on `VoxelDemo`) draws its tiles by face as before (grass tops,
the grass-over-dirt side tile on the step faces); it was not compared pixel for pixel. Shots in the worktree's
`Saved/Screenshots/Claudius/f212_*`.

Six suites (`VoxelWorlds+VoxelWorldSave+VoxelCharacter+WorldClock+InteractionPlugin+Dungeon`): 639/639 headless. In the
editor 638/639, no device loss: `VoxelWorlds.Configuration.TreeTemplateMaterials` failed because a PIE on
`VoxelWorldsTest` earlier in the same editor session had bound `VoxelMaterialAtlas`, and binding an atlas registers its
MaterialIDs (`FVoxelMaterialRegistry::SetAtlasPositions`: 22 and 23, which the test expects unregistered); in a fresh
editor session it passes, as do the four `MaterialAtlas.TextureArrays` tests. The test's dependence on the registry's
global state predates this change.

**Found on the way.** A fresh worktree has none of the gitignored content packs (`Content/Fab`, `Content/Megascans`,
`Content/ContentPacks`): both atlases' source textures live there, so an editor opened there finds every atlas stale,
rebuilds with nothing loaded and drops the arrays in memory (nothing saved, but the terrain draws untextured). The
directory junctions to the main checkout's packs (made from a `.cmd` file, before anything loads) are needed for any
editor run there. Two worktree editors picking the same Claudius port: the second one's listener fails to bind and the
first one answers, so check the port's owning process before sending anything.

### F2.13: the voxel compute shaders on FXC (2026-10-24)

**What a Windows cook spent.** The Windows targets build `PCD3D_SM6` for DX12 and `PCD3D_SM5` for DX11
(`Config/DefaultEngine.ini`, as UE's project template has it); SM5 compiles with FXC (the engine's `d3dcompiler_47.dll`,
10.0.19041, `/O3 /Gec /Zpr`). The cook's global shader step alone (`-run=CookGlobalShaders -platform=Windows`, the voxel
shaders out of the DDC by a comment appended to each `.usf`, `r.DumpShaderDebugInfo=1`, 18 shader workers) ran 6,657 s
and then **failed**: "1 errors encountered compiling global shaders for platform PCD3D_SM5", so no SM5 cook could have
finished. Per job (UE's "compile time exceeded threshold" lines):

| Shader | SM5 before | What FXC did |
|---|---|---|
| `FGenerateTransitionCellsCS` | 5,423 s | its first attempt |
| `FGenerateMarchingCubesMeshCS` | 5,372 s | its first attempt |
| `FDCEdgeCrossingCS` | 1,416 s | X3511 after 3 s (it cannot unroll the axis loop); then UE's fallback, below |
| `FGenerateVoxelDensityCS` | 963 s | "compilation aborted unexpectedly" (`ERROR_ARITHMETIC_OVERFLOW`) after about 12 min (1,225 s alone); then the fallback |
| `FVoxelWaterFloodSweepRadialCS` | error | "internal error: flattened side effect", the fallback too: the step's fatal error |
| `FVoxelWaterFloodSweepCS` | < 1 s | the same internal error; the fallback compiled |
| `FDCQEFSolveCS` | 73 s | its first attempt |

When FXC fails or aborts, `D3DShaderCompiler.cpp` compiles the shader with DXC to SPIR-V, cross-compiles that back to
HLSL and gives it to FXC again: 1.28 MB of HLSL with every call inlined and every loop unrolled for the DC edge pass,
600 KB for the density pass.

**Cause.** Timed with FXC outside UE on each shader's dumped input and on variants of it (the engine's DLL, UE's flags;
`fxc_time.py` in the session's notes):

- Both meshers' `GetVoxelAt` resolves a voxel outside the chunk with an if-ladder over the 26 neighbour buffers (DC:
  and the deep planes), and FXC inlines it at every call, its optimiser's time growing much faster than the copies.
  `MainCS` had about a hundred reads: the 8 corners and 16 more for the material and biome (unrolled), the geomorph's 6
  trilinear samples of 8 taps and its coarse-surface search, the gradient's 6 taps on each of the normal paths. With
  the same loop structure and a one-line `GetVoxelAt`, `MainCS` compiled in 1.8 s against 115 s.
- `DCEdgeCrossingCS`: `[unroll]` over 3 axes of 14 inlined reads each: X3511. `[loop]` alone compiled in 615 s (5,491
  instructions); flags do not rescue it (`/O1` 382 s, `/Gfp` 1,226 s, `/Od` 20 s but 20,758 instructions).
- `FGenerateVoxelDensityCS`: the 27-cell cellular search `[unroll]`ed at every fBm call site with a runtime noise type
  (the heightmap terrain, the planet's terrain, the default branch), Voronoi a second copy of it, and `GpuCaveCarve`'s
  `WorldMode == planet ? planet : heightmap` inlining both cave samplers at its three calls (and running both on FXC,
  whose `?:` does not short-circuit). Any one world-mode branch removed, it compiled (468-704 s); all three, it aborted.
- The water sweeps: FXC cannot flatten the else-if that holds the UAV write.

**Fix (the user's choice, 2026-10-24: keep DX11, change the shaders).** Nothing the shaders compute changes:

- `GetVoxelAt` (MC and DC) finds the neighbour by index (face 0-5, edge 0-11, corner 0-7, the `FACE_` / `EDGE_` /
  `CORNER_` order) and reads it with one `[branch] switch`; DC then reads the deep data once. The same fallback to the
  nearest voxel of the chunk.
- The voxel reads sit in `[loop]`s: the gradient's 6 taps (both meshers), MC's 8 corners, the trilinear sample's 8 taps,
  the coarse-surface search's 8 and the geomorph's 6 steepness taps, the transition cell's 13 samples, DC's axis loop
  and a cell's 8 corners (`GetCellSurfaceVoxel`: one read for the material and the biome, the voxel both chose before),
  and the vertex loops. MC reads the first solid corner once (`GetDominantVoxelLengyel`, `firstbitlow`), as does the
  transition cell its first solid sample. MC's vertex normals go through one `CalculateVertexNormal` (the edge walk
  `FindEdgeShadingPoint`, the fit `TryFitShadingNormal`, then the gradient) instead of three paths with a copy each.
- The cellular searches in `[loop]`s (`Cellular3D`, `Voronoi3D`, `PlanetCellular`), one search for Cellular and Voronoi
  (they compute the same F1 and F2), and `GpuCaveCarve(..., bPlanetFrame)` with a literal at each call.
- `[branch]` on the water sweeps' if-chain.

**Results.**

| Shader | SM5 before (the cook) | FXC after (alone) | Instructions after |
|---|---|---|---|
| `FGenerateTransitionCellsCS` | 5,423 s | 2.7 s | 2,666 |
| `FGenerateMarchingCubesMeshCS` | 5,372 s | 2.3 s | 2,236 |
| `FDCEdgeCrossingCS` | 1,416 s | 0.24 s | 867 |
| `FGenerateVoxelDensityCS` | 963 s | 18.0 s | 6,308 |
| `FDCQEFSolveCS` | 73 s | 0.67 s | 1,088 |
| `FVoxelWaterFloodSweepRadialCS` | error | 0.02 s | 94 |
| the 11 others | < 1 s | 0.3 s together | |

- `CookGlobalShaders` as above (SM5 and SM6, the voxel shaders not in the DDC): 6,657 s and a fatal error before,
  **42 s** after (66 jobs, the longest 23 s; no fallback, no error).
- DX12 is unchanged: the 46 GPU tests (`Generation.GPU*`, `WorldModes.InfinitePlane.GPUConsistency`,
  `Meshing.MarchingCubes`, `Meshing.CrossingGradient`, `Meshing.DualContour.MaterialBorder`) in the editor on D3D12 SM6
  give every one of their 304 messages identical before and after (parity percentages, mismatch counts, GPU CRCs,
  normal angles). DC's two GPU tests skip, as before (GPU DC interior meshing is retired).
- DX11, for the first time (`-d3d11`; before, the editor would have compiled the voxel shaders for 1.5 h at start, then
  failed): the 46 pass with the planet fix below; seven of the generation tests read a little differently from D3D12
  (FXC's floats against DXC's; the lowest density parity 99.4% in `GPUvsCPUCavesParity`, 99.8% on D3D12), inside their
  thresholds.
- The six suites (`VoxelWorlds+VoxelWorldSave+VoxelCharacter+WorldClock+InteractionPlugin+Dungeon`): 639/639 in the
  editor (D3D12) and 639/639 headless.
- The package on a cold cache: `BuildCookRun` Win64 Development of `PlanetTest`, SM6 and SM5, with a new, empty DDC
  (`-AdditionalCookerOptions=-LocalDataCachePath=<path>`; the engine's shipped `Compressed.ddp` is the only cache left):
  built, cooked, staged and packaged in 17 min 17 s, the cook **997 s** (about 413 s of start-up, 584 s cooking 995
  packages), 9,092 shader jobs compiled, no shader warning. The voxel shaders' SM5 jobs there, at full load: density
  46.7 s (the cook's slowest job, the engine's volumetric cloud shaders 40-47 s), MC 9.1 and 12.0 s. With the shared,
  warm DDC the same cook took 154 s. Before, no SM5 cook could finish (above). The packaged game on D3D11 (`-d3d11`)
  draws `PlanetTest` with GPU generation (968 chunks loaded, 53 fps; `f213_package_d3d11.png` in the session's notes).
- A cold-DDC cook's path must stay under 119 characters (a fatal error otherwise) and take forward slashes: UAT strips
  backslashes, and the cook then disables the file-system cache and falls back to the shared, warm Zen store unannounced.

**Found on the way.**

- On D3D11 `GPUvsCPUSphericalParity` failed at its conditioning pads: density parity 71.6% at `Planet L`'s pad, 95.0-99.1%
  at the small planet's pads and rings, every other site 100%. A pad's distance is 2R asin(chord / 2R); D3D11 bytecode
  has no asin, and FXC expands it to a polynomial with up to 5e-5 rad of error, which 2R turns into uu at the pad's edge.
  Older than this change: the old SM5 density pass (the DXC fallback's output) holds the same polynomial
  (`-0.018729`, `0.074261` in its `Output.d3dasm`). `PlanetApplyConditioning` now takes asin's series below 0.1: every
  pad 100% on D3D11, the D3D12 messages identical.
- The GPU mesher shaders do no work in a game: chunk meshes go to the CPU seam mesher (`voxel.Seam.CPUInteriorRouting`;
  GPU DC interior meshing is retired) and only the MC GPU tests run them, but every target compiles them
  (`ShouldCompilePermutation`: the SM5 feature level). GPU generation is the one used at run time (planets by default).

### VP2-O2 texture variation (2026-10-24)

The user's report after milestone 1: the texture tiling shows, "we need to do something to break up the pattern" (F2.8).
Branch `feat/vp2-texture-variation` (VoxelWorlds and the parent; rebased onto F2.13's `main` on 2026-10-25, below).

**What it does.** Both triplanar functions include a new `Shaders/Private/VoxelTextureVariation.ush`
(`MATERIAL_SYSTEM.md`, "Texture Variation"). Tile variation (`voxel.Material.TileVariation`, 1): the ground is a mosaic of
patches under a tile, each reading the texture at one of eight offsets picked by a smooth noise about a tile across,
blended across the patches' edges (after Inigo Quilez's "texture repetition", technique 3; offsets keep the normal map
valid). Triplanar projections under 2% are dropped, so most pixels read one projection, and a second read happens only in
the blend bands. Macro variation (`voxel.Material.MacroVariation`, 0.3): the albedo's brightness varies by up to ±30%
over 13 and 41 tiles. Every noise is periodic on the tiled position's period (655 tiles at `TriplanarScale` 0.01), so
nothing jumps where it wraps. `MPC_VoxelMaterial` gains `TileVariation` and `MacroVariation`;
`Scripts/setup_texture_variation.py` (parent) writes the nodes; test `VoxelWorlds.Rendering.TextureVariation.Assets`
(the collection scalars, the includes, the inputs, and every collection parameter node carrying its scalar's id).

**First try, too coarse.** With the noise 8 tiles across, the patches were several tiles wide and the tiles still repeated
inside each one: a cross-correlation of the off and on images found each region shifted as a block (by -46 to +39 px),
the lattice intact. The lattice measure below went 0.243 → 0.220. Patches under a tile fixed it.

**Results** on `PlanetTest` at 30°N 20°E, 11:30 local, the clock stopped, scatter hidden (`ShowFlag.InstancedStaticMeshes
0`), five survey poses, each with the variation off, tile only, tile and macro (`Saved/ClaudeScripts/vp2o2_ab.sh`, images
`vp2o2_pt30nB_*`):

- **Repetition** from 1,500 uu straight down (`vp_lattice.py`: the off image's three strongest autocorrelation peaks of
  the high-passed ground at 0.6-3 tiles, the tile lattice, and every image's autocorrelation at those lags; 1 = an exact
  repeat): off 0.281, tile 0.049, tile and macro 0.048.
- **Close** (120 uu up): the rows of brown marks become random ground, with no visible patch seams; contrast drops a
  little where two copies blend.
- **Grazing** (170 uu up, 9,000 ahead) and **from 3,500 uu**: the plaid grid on the far grass and mud is gone; the macro
  term breaks the far ground into broad lighter and darker areas.
- **Cost** (bench `300 3000 gc=45:20:90:2000`, three runs each, alternating, scatter on): GPU frame P50 5.60-6.48 ms off
  against 5.93-6.20 on, P95 8.10-9.44 against 7.67-8.12; frame P50 18.7-21.6 against 18.8-22.5 ms. No cost measurable:
  dropping the minor projections pays for the extra reads.

Gotchas: editing the include in a running editor does not recompile the masters by itself (its hash is part of the
shader key, but the materials must be recompiled after `recompileshaders changed`); the first bench batch ran with the
editor throttled to 3 FPS in the background and was discarded.

**Rebased onto F2.12 (2026-10-25).** VP2-O2 was finished before F2.11-F2.13 landed and stayed uncommitted until then.
F2.12 rewrote the same two binary material functions (a `MaterialLUT` input, each array read at its own slice), so the
rebase took `main`'s functions and ran `Scripts/setup_texture_variation.py` and then
`Scripts/setup_texture_array_slices.py` on them (headless): VP2-O2's code with each array read, `VoxelVariationSample`
included, at the material's slice from the LUT. `MF_CubicAtlasSample` and both masters were saved again by the second
script, their links unchanged. Checked on `PlanetTest` with VP2-O2's survey (`vp2o2_ab.sh`, the same five poses, images
`vp2o2_rebase_*`): the terrain textured, no material compile error, and the tile lattice's repetition from above off
0.314, tile 0.048, tile and macro 0.047 (VP2-O2's own shots measured the same way: 0.268, 0.046, 0.046). The six suites:
643/643 headless and in the editor.

### Next

- **VP1-O6 (new):** decide the atmosphere's shadows on small planets: keep them (stable now apart from the cascades'
  re-fit), soften them, or turn `bCastShadowsOnAtmosphere` off when the scaled atmosphere is thin.
- **F2.9 (new, seen with the sky light on):** faint striped shading on far gentle slopes at a low sun
  (`vp1o5_45n_eye_ab.png`, the far valley): to trace (shadow-map self-shadowing at grazing light, or G62's shading).
- **F2.11 done (2026-10-24):** a packaged game had no terrain textures on smooth worlds (shown in a package); the
  arrays are saved assets next to the atlas now, see "F2.11: the atlas arrays saved with the atlas".
- **F2.12 done (2026-10-24):** the arrays hold one slice per distinct texture and the triplanar functions read each
  material's slice from the LUT's slice row: the demo atlas's arrays 38.8 → 7.9 MB on disk and 15.8 → 4.8 MB on the GPU,
  the terrain unchanged; see "F2.12: one slice per distinct texture". VP2-O2's texture variation (uncommitted when this
  landed) edits the same two functions: rerun `Scripts/setup_texture_array_slices.py` after its own setup script (done
  when VP2-O2 was rebased).
- **F2.13 done (2026-10-24):** the SM5 (DX11) cook spent hours in FXC on the voxel compute shaders and then failed; with
  the shaders restructured (no change in what they compute) the global shader step takes 42 s against 1 h 51 min, and
  DX11 planets' pads match the CPU; see "F2.13: the voxel compute shaders on FXC".
- Then the rest of milestone 2 (VP2-O3, O4; VP2-O2 done above) and milestone 3 (VP3-O2, O3) as planned.

## VP4 terrain flicker (2026-10-25)

The user's report on `PlanetTest`: "flickering of the terrain. it is very fast and i am unsure what the cause could be. it
seems to happen both near and far at different times but consistently will occur in different areas as you move." (Their
`PlanetWorldConfig` had scatter turned off, uncommitted; the runs below have it off unless noted.)

### Repro and tooling

- **Capture:** `r.DumpingMovie N` writes N consecutive rendered frames (`Saved/Screenshots/WindowsEditor/MovieFrame*.png`),
  at about 3 a second in real time; `UEngine::bUseFixedFrameRate` 30 (set from Python by its raw property name,
  `Saved/ClaudeScripts/fixed_fps.py`; the console's `set Engine ...` does not reach the editor engine) makes every dumped
  frame 1/30 s of game time, so a burst plays as 30 fps play would. The pawn is hidden.
- **Metric** (`Saved/ClaudeScripts/vp4_flicker.py`): a flicker is a pixel that jumps and jumps back. For each frame t,
  spike = min(|L(t) - L(t-1)|, |L(t+1) - L(t)|) where the two differences have opposite signs, else 0; motion moves edges
  across pixels (a step that stays) and TSR's jitter stays under the threshold (24 luma). Reported: the share of pixels
  spiking per frame, and **events**: frames over 0.1% with a connected region of at least 1,500 px, with the region's
  mean change against its neighbours.
- **Scenario** (`scenario.sh`, `ab.sh`): `vox.TPLatLon 30 20` at 11:30 (the clock stopped), `vox.AutoRun 1 0 0` at
  1,500 uu/s, 300 frames (10 s, 15,000 uu); the bursts in `Saved/Bursts/`.

Standing still (at the pole and at 30°N) and in a 3 s walk nothing spiked. Running, a few frames a burst had an event: a
region of terrain 1,500-16,000 px, brighter by 10-31 luma, for exactly one frame. In those frames the region is drawn by
the far field's analytic surface: flatter shading, other material patches (its vertex picks), the 1 m tiling visible, a
deer on a hill hidden behind it, the horizon's silhouette changed (`Saved/Screenshots/Claudius/vp4_before_238.png`,
`vp4_before_48.png`, `vp4_before_287.png`). `voxel.World.FarField 0`: no event and no hole in 300 frames, so the voxel
terrain was drawn every frame and the far field was drawn over it. The other switches the investigation listed (crossfade,
sky light, shadow culling, texture variation, shadows, Lumen, anti-aliasing) were not needed: the cause was traced in the
far field's selection and confirmed frame by frame by instrumenting it (below).

### F4.1 The far field's stand-ins

`FVoxelPlanetFarField::Visit` draws a patch whose subtree is not ready (a patch under it waits for its build) in its
subtree's place, hiding everything under it (`ClearDrawnSubtree`) until the build lands, the next update at the earliest.
For refinement that is right: the patch was drawn last frame, and keeps being drawn until its four children are ready.
Patches hidden under their children are released after `ReleaseDelay` (10 s), so inside and around the voxel window the
nearest built ancestor of a band patch is usually a **cube face's root**: 16 x 16 quads over a 90° face (24,500 uu a quad
on `PlanetTest`), whose chords lie hundreds to thousands of uu over the voxel terrain in valleys (patches sink 75 uu) and
which replaces every finer patch out to the horizon. Three things made a patch wait for a build where nothing of it
would have shown:

1. **The prebuild ring.** A band patch whose chunks are all drawn stood in for its area without a mesh (lazy) only within
   `PrebuildRadius` (0.75 of the load distance, 30,000 uu); past it, band patches are kept built for when their chunks
   unload. As the viewer moves, covered, unbuilt band patches cross that ring outward and wait for a build: the most
   frequent trigger (straddling 22,000-30,000 uu in every case the test logged).
2. **The horizon.** A patch culled under the horizon (drawing nothing) for 10 s is released; when the viewer's move brings
   it back over the horizon it waits for a build. From eye height on `PlanetTest` the culling boundary is about 0.53 rad
   (132,000 uu) away, so far patches cross it continually.
3. **An empty chunk re-meshing.** `UVoxelChunkManager::IsChunkDrawnForFarField` counted a chunk re-meshing (an LOD change)
   as drawn only if the renderer held its mesh, which it does not for an empty mesh, so the band patch over it was
   uncovered for the re-mesh and waited for a build.

The trace (`voxel.World.FarFieldTrace 1`, below) over the session's bursts with the rule before (the A/B's three, the
pole's and two with scatter): of the 117 stand-in updates while they dumped, 74 came from (1), 39 from (2) and 4 from
chunks loading; over the whole session (teleports and settling included), (3) 41 times. Each flicker event of the A/B
fell on a frame with a stand-in (run 1: level 1 stand-ins at frames 257 and 287, the two events; run 2: frame 230;
run 3: 234 and 262); root stand-ins without an event drew little over the terrain from where the camera was.

### VP4-O1 No stand-in where nothing would show

`voxel.World.FarFieldAvoidStandIns` (1; 0 = before), `FVoxelFarFieldView::bAvoidStandIns`:

- A band patch whose chunks are all drawn covers its area drawing nothing, built or not, wherever it lies; past
  `PrebuildRadius` once its chunks have been drawn `HideDelay` (a chunk that has just appeared fades in over where the
  patch's mesh would be). One that must stay built for the window's edge (`IsKeptBuilt`) builds meanwhile and draws
  nothing until its chunks unload (`IsLazy`, `Visit`).
- A patch culled in its last visit (`FPatchState::bCulled`) waits for its build drawing nothing: it drew nothing a
  moment ago and lies at the horizon's edge. Not after a cut (the camera more than a chunk from the last update's, a
  teleport), where it may lie in full view and its ancestor stands in as before: without that, a teleport left 2,282
  directions of the test's grid with no surface for an update.
- A chunk loaded before counts as drawn while it re-meshes, also with an empty mesh (`LoadedChunkCoords`).

Stand-ins for chunks that are not loaded yet (after a teleport, the window catching up) are unchanged: there the far
field must fill the hole.

Instrumentation: `FVoxelPlanetFarField::GetStandInCount` / `GetStandInMinLevel` / `GetTotalStandIns` /
`GetStandInUpdates` (a stand-in counts when its subtree has a band patch the chunks cover), `GetUnreadyBandCount`,
`GetUndrawnChunk`; `FVoxelFarFieldStats::StandIns` (and the level and totals), in `voxel.World.FarFieldStats`; the
bench's `FarFieldStandInN` column and `farFieldStandInsTraverse` / `farFieldStandInSamples`; `voxel.World.FarFieldTrace 1`
logs every update with a stand-in (the frame, `GIsDumpingMovie`'s count, the coarsest level, the band patches waiting
covered and uncovered, and a chunk not drawn under one with its state).

### Results

Test `VoxelWorlds.FarField.Planet.NoStandInOverWindow` (pure logic, headless): the window loaded at 45° on the small
planet, settled past the release delay (162 patches released), the viewer walking 7,500 uu at 1,500 uu/s with an update
a frame at 30 fps. The rule before: 9 of 150 frames drew a stand-in over the window (16 in all, the coarsest a face's
root), from 14 covered band patches waiting and two patches back over the horizon. With VP4-O1: none, no covered band
patch waits, no gap out to three view distances, and the window shrunk to 0.85 of the view distance leaves no gap in the
very next update (the edge's band patches were built meanwhile). A teleport 40° toward the pole, into area culled until
then, leaves no gap in its first update under either rule.

Live on `PlanetTest`, the scenario above, alternating (one build, by the cvar):

| Run | Stand-in updates while dumping (a face's root) | Flicker events | Most pixels spiking in a frame |
|-----|-----------------------------------------------|----------------|--------------------------------|
| Before, runs 1-3 | 13 (6), 27 (8), 27 (8) | 2, 1, 2 | 1.06%, 0.27%, 0.29% |
| VP4-O1, runs 1-3 | 0, 0, 0 | 0, 0, 0 | 0.045%, 0.059%, 0.044% |

Again on the final build (with the cut rule), two runs each: stand-in updates 20 (8 roots) and 27 (7) → 0 and 0; the
one event left with VP4-O1 is deer walking across a few frames.

From the north pole at walking speed (600 uu/s, along +Y): 18 stand-in updates (8 roots) → 0. With scatter on: 23 → 0;
the grass and flowers streaming past spike in both (0.7% of the frame each frame), as do the foreground at the pole
under the grazing sun (detail passing a texel a frame) and the hill line as the camera bobs over bumps: motion, the same
either way. With VP4-O1 the stand-ins left are the ones after each teleport, for chunks not loaded yet.

Cost (bench `300 3000 gc=45:20:90:2000`, three runs each, alternating, scatter off): stand-ins over the traverse 2 → 0;
far-field patches built 20 → 8-9 (covered patches past the ring are no longer built to stand in), update P50 0.33-0.37
ms either way, memory peak 84.9 → 84.3-85.1 MB; GPU frame P50 3.73-4.65 → 4.05-4.29 ms, P95 6.99-7.95 → 6.75-7.28 ms;
frame P95 23.7-24.3 → 22.6-23.6 ms (vsync-bound P50 16.67); manager P95 4.10-4.14 → 3.92-4.23 ms. No cost.

Tests: the six suites (`VoxelWorlds+VoxelWorldSave+VoxelCharacter+WorldClock+InteractionPlugin+Dungeon`) 644/644 headless;
in the editor `VoxelWorlds.FarField` and `VoxelWorlds.Rendering` 18/18.

**F4.2 (found on the way, not fixed): a teleport onto a face never seen.** A cube face's root is built only once some of
it comes into view (a culled patch requests no build). After a teleport onto a face the camera has not looked at, nothing
of the far field is resident there, so the first update after it draws nothing there under either rule (the test's
teleport 90° away from 45°: no patch drawn, 4,042 empty directions); builds fill it over the next updates while the voxel
window loads. Building the six roots up front (a patch each, about 0.1-0.2 MB on `PlanetTest`) would close it.

Not traced here: the sky and lighting were not touched (the sky's own flicker is VP1, the pixelated clouds a separate
session).

## The setup as found

- **Level** `/Game/PluginTesting/Planet/PlanetTest`: `PlanetWorldConfig` (SphericalPlanet, Dual Contouring, R 250,000 uu,
  so 2.5 km; terrain ±6,500 uu over the datum, no water; voxel 75 uu, chunk 64; view distance 40,000), the material
  `MI_VoxelDefault` with `DemoMaterialAtlas`, `DemoBiomeConfig`. Lighting: `DirectionalLight_0`, `SkyAtmosphere_0`,
  `VolumetricCloud_0` (`m_SimpleVolumetricCloud_Inst`), `SkyLight_0`, `DayNightController_0`, and the OpenWorld
  template's `SM_SkySphere`, tagged `WorldClock.ZUpSky` and hidden on a planet. No height fog.
- **Planet sky** (`ADayNightController::ApplyPlanetSky`, `Plugins/WorldClockPlugin/Source/WorldClock/Private/DayNightController.cpp:173`):
  the atmosphere is centred on the planet and scaled to it. The last PIE logged "atmosphere at V(0) (ground 2.500 km,
  0.260 km high, scale 0.0043 of the authored), 1 cloud layer(s) scaled (0.065 to 0.108 km)".
- **Clouds** (`VolumetricCloud_0`): authored layer 5 km + 10 km, scaled by `ComputePlanetCloudLayer` (`:136`) to a bottom
  of max(5 km x K, terrain top) = 65 m and a height of 43 m. Left as authored: `TracingStartMaxDistance` 350 km,
  `TracingMaxDistance` 50 km, `ShadowTracingDistance` 15 km, the sample count scales, the cloud material. The engine
  traces clouds at quarter resolution and reconstructs them over frames (`r.VolumetricRenderTarget.Mode` 0).
- **Sky light** (`SkyLight_0`): movable, real-time capture, at (0, 0, 0), which is the planet's centre. The real-time
  capture renders the sky from the component's location (`SkyLightComponent.cpp:550-556` sets `CapturePosition`;
  `ReflectionEnvironmentRealTimeCapture.cpp:557` uses it as the cube view's origin).
- **Sun** (`DirectionalLight_0`): movable, cascaded shadow maps (`r.Shadow.Virtual.Enable=0` in `Config/DefaultEngine.ini`),
  dynamic shadow distance 20,000 uu with 4 cascades, no far cascades, distance field shadows off. The controller sets its
  rotation, intensity and colour every tick (`DayNightController.cpp:327`); on a planet the intensity and colour follow
  the local daylight where the pawn stands (D12).
- **GI and reflections**: Lumen for both, hardware ray tracing on (`r.RayTracing=True`, `r.Lumen.HardwareRayTracing`
  default 1). The voxel proxy sets `bAffectDistanceFieldLighting` and `bAffectDynamicIndirectLighting` false
  (`Plugins/VoxelWorlds/Source/VoxelRendering/Private/VoxelSceneProxy.cpp:161-162`) and provides no ray tracing geometry.
- **Terrain material**: `M_VoxelMaster` (and `M_VoxelMaster_Fade`) sample through `MF_TriplanarSampleAlbedoRoughness` and
  `MF_TriplanarSampleNormal`. `MI_VoxelDefault`: `TriplanarScale` 0.01 (one tile per metre), `TriplanarSharpness` 8,
  `Specular` 0.05. `DemoMaterialAtlas`: `TextureArraySize` 512, but the arrays were built at 4096 (F2.10, fixed);
  Grass, Dirt (mud), Stone (slope), Snow and Riverbed from the
  Stylized Spruce Forest pack, the ores from Megascans; Sand and Frozen Dirt have no textures. No terrain material has a
  roughness map.
- **Biomes** (`DemoBiomeConfig`): Plains (grass over dirt over stone), Forest (dirt), Mountain (stone); temperature
  frequency 5e-5 and moisture 7e-5 (features about 150-200 m across), blend width 0.1, height rules off.

## VP1 Sky flickers and moves strangely

### Findings

- **F1.1 The cloud layer sits on the terrain.** On `PlanetTest` the layer spans 65-108 m over the datum and the highest
  terrain is 65 m, so the clouds float tens of metres over the player and the peaks stand in their base.
  `ComputePlanetCloudLayer` keeps the bottom at or above the terrain top with no clearance. Volumetric clouds are built
  for layers kilometres away: traced at quarter resolution and reconstructed from earlier frames, they hold up while the
  clouds barely move on screen. A cloud 30 m overhead sweeps across the screen as the camera moves, so its reconstruction
  smears and shimmers. This is the leading suspect for "flickers as the camera moves". Not shown yet.
- **F1.2 Cloud tracing is still Earth-sized.** A 350 km start distance, 50 km of tracing and 15 km of shadow tracing on a
  planet 5 km across: the per-ray sample budget is spread over the whole shell, and the 43 m layer gets few samples
  per ray. Expect noise that the temporal reconstruction then smears. Not shown yet.
- **F1.3 The cloud material is Earth-sized too.** `m_SimpleVolumetricCloud_Inst` shapes clouds with noise kilometres
  across. Over a 43 m layer around a 2.5 km planet that gives a few blobs wrapped around the planet rather than a cloud
  field. This is a look problem rather than flicker.
- **F1.4 The sky light captures from inside the planet.** `SkyLight_0` is at the planet's centre, so every frame's
  real-time capture (the ambient light and the reflection fallback for the whole planet) is the sky as seen from
  2.5 km under the ground. It is the same everywhere on the planet. It does not follow the local time of day (D12: the
  sun is fixed in the world, so one side of the planet is in night). At the exact centre the atmosphere has no up
  direction; what the engine does there has to be checked. Expect wrong ambient light rather than flicker, unless the
  degenerate case gives an unstable result. Any planet setup with its sky light at the centre has the same problem; the
  review planets run on `VoxelDemo` with overrides, so check that map's sky light too.
- **F1.5 A small planet's sky moves correctly.** At R 2.5 km local up turns 1 degree every 44 m: walking 400 m moves the
  sun 9 degrees and the horizon drops away quickly. That is correct, but it can read as "the sky moves strangely". Keep it
  apart from the flicker. For media a bigger planet (`Review_Planet_M`, 50 km) reads as a world.
- **F1.6 Other things that can look like a flickering sky.** Auto-exposure pumping as the view turns between sky and
  ground (the controller's exposure-bias post-process plus the map's metering), and Lumen's terrain lighting changing
  with the view (VP3's F3.3), which shifts the whole frame's brightness.

### How to investigate

One variable at a time, in PIE on `PlanetTest`, at a fixed spot with a scripted camera move (a 90-degree yaw sweep, then
a 50 m walk with `vox.AutoRun`). Temporal noise does not show in one screenshot, so take bursts of `pie_screenshot`
along the move and measure the frame-to-frame difference over the sky pixels (tooling item T1 below).

1. `showflag.Cloud 0`: if the flicker goes, it is the clouds (F1.1-F1.3). Then `r.VolumetricRenderTarget.Mode 3`
   (full-resolution tracing, no reconstruction): if that also removes it, it is reconstruction against close clouds.
2. With clouds on: the clouds' tracing distances scaled by K (by hand on `VolumetricCloud_0`), then the layer's bottom
   raised clear of the terrain (bottom = 2-3 x the terrain top), each alone.
3. `showflag.Atmosphere 0`, to rule the atmosphere in or out.
4. The sky light moved to the camera in PIE (or `bRealTimeCapture` off), to see what F1.4 does to the ambient light,
   and whether it flickers at the centre.
5. Exposure locked (`r.EyeAdaptationQuality 0`, or a post-process volume with manual exposure), and Lumen GI off
   (`r.Lumen.DiffuseIndirect.Allow 0`), to rule out F1.6.
6. Repeat the step that changed things on `Review_Planet_M` (50 km), where K is larger and the clouds sit higher.

### Options

- **VP1-O1 Scale the whole cloud setup with the planet** (WorldClockPlugin, in `ApplyPlanetSky`): the tracing distances
  (start, max, shadow) by K like the layer, and the layer's bottom at a clearance over the terrain top (a multiple of
  it, or the terrain top plus a margin), so the camera on the ground is never in or just under the clouds. Small; a
  `ComputePlanetCloudLayer` unit test covers it.
- **VP1-O2 Cloud quality on planets**: a higher view sample count, or `r.VolumetricRenderTarget.Mode` 1 or 3 when the
  clouds are close (cost to measure). Cinematic mode is fine for media captures in any case.
- **VP1-O3 Scale the cloud material**: a planet-aware cloud material instance whose noise scale follows K, set by the
  controller. Medium; changes the look, so it needs the user's eye.
- **VP1-O4 No volumetric clouds on small planets**: under some radius, hide the volumetric clouds and draw a thin cloud
  shell (a sphere around the planet with a 2D cloud material), or none. A decision for the user.
- **VP1-O5 Capture the sky where the viewer is**: on a planet the controller moves the sky light to the camera (or to a
  point over the ground under it) every tick. The real-time capture re-renders anyway; check its time slicing while
  moving fast (`r.SkyLight.RealTimeReflectionCapture.TimeSlice`). Small.

## VP2 Terrain materials are noisy

### Findings

- **F2.1 Every terrain texture is read at mip 0 (confirmed).** Both triplanar functions sample with
  `SampleLevel(Samp, float3(UV, MatID), 0)`: albedo, roughness and normal, all three projections (the HLSL in
  `Plugins/VoxelWorlds/Content/Materials/Functions/MF_TriplanarSampleAlbedoRoughness.uasset` and
  `MF_TriplanarSampleNormal.uasset`; `MATERIAL_SYSTEM.md` documents it "for ray tracing compatibility", and P6.2's
  large-world tiling relies on it, "so no seam shows at a period boundary"). The mip chain VoxelWorlds#25 restored is
  never read. At one 512 px tile per metre, a pixel of ground 10 m away covers about 5 texels; 50 m away, about 26; at
  the grazing angles of a walking camera, hundreds. Every pixel beyond a few metres picks one texel out of that footprint,
  a different one with every sub-pixel camera move and every TSR jitter. That is aliasing TSR cannot resolve, and the
  normal map adds the same noise to the lighting. This alone likely accounts for most of the report, and probably for
  the "residual noise as you move" put down to TSR after the shimmer fixes in VoxelWorlds#25 and #26.
- **F2.2 The sampler has no anisotropy and no mip blending.** The functions use `View.MaterialTextureBilinearWrapedSampler`,
  not the arrays' own sampler (`TF_Default`, anisotropic since #25). With mips this would blur at grazing angles and
  show mip bands.
- **F2.3 Material mosaic in biome blend zones.** With biome features about 150-200 m across and a blend width of 0.1,
  much of the ground in view is a blend zone, and G39's dither picks one biome per cell of a 4-voxel lattice (3 m here)
  with hard per-quad borders (DC per-quad materials, `Research/DC_MATERIAL_BLEND_PLAN.md`). Grass and mud alternate in
  3 m patches: a coarser noise of its own.
- **F2.4 Repetition and no variation.** One-metre tiles (the pipeline's 512 px/m density), no macro variation, no change
  of scale with distance, and grass projected onto steep slopes (Plains has no slope rule, height rules are off). These
  make the terrain look unconvincing rather than noisy.
- **F2.5 The far field's dithered pick** (G45, planets) picks one of two materials per pixel against noise that moves
  every frame, which TSR averages. A small share of the screen; check it in motion.
- **F2.6 Content.** The stylized terrain textures sit beside photoreal Megascans ores, nothing has a roughness map, and
  slots without textures (Sand, Frozen Dirt) are filled with the first valid texture, the grass
  (`Plugins/VoxelWorlds/Source/VoxelCore/Private/VoxelMaterialAtlas.cpp:408-426`). The demo biomes use Sand only under
  water. The cubic atlas function (`MF_CubicAtlasSample`) also samples at mip 0. A packed atlas bleeds between tiles at
  low mips, so the fix there differs (padding, or arrays for cubic too); it is out of scope unless cubic media is wanted.

**Do the textures need replacing?** Not first. The mip-0 sampling turns any texture into noise beyond a few metres, so
the textures cannot be judged until F2.1 is fixed. Fix the sampling, then look again; replace them only if the result
still disappoints. The style (stylized like the current terrain, or photoreal like the ores) is the user's call, and a
replacement set should come with roughness maps.

### How to investigate

Fixed cameras on `PlanetTest`, `VoxelDemo` (DC) and an MC configuration: near ground, a mid view, a grazing view to the
horizon, and a view from altitude. For each, one `pie_screenshot` with `r.AntiAliasingMethod 0` (which freezes the
temporal noise into a still pattern) and a burst along a slow pan with TSR for T1's temporal metric. Then:

1. Gradient sampling in the two functions (VP2-O1), set through the custom nodes' `Code` by MCP, against the assets as
   they are. This is the main A/B.
2. With that in: `voxel.Biome.BlendDither 0` against 1 (F2.3) and `voxel.World.FarFieldMaterialBlend 0` against 1
   (F2.5), to see how much of what is left comes from the material picks.
3. The user looks at the result before any look work (O2-O5).

### Options

- **VP2-O1 Sample with gradients** (both functions, both masters inherit it): `SampleGrad` with each projection's UV
  derivatives taken from the position before tiling (the camera-relative position, small at any distance), so P6.2's
  tiling still shows no seam, and the arrays' own anisotropic sampler. Outside the pixel shader (ray tracing hit shaders,
  RVT), fall back to `SampleLevel` at a mip chosen from distance. The same number of samples as now. Small.
- **VP2-O2 Variation**: a second sample at a coarser scale (about 1/8) mixed in by distance, plus a low-frequency
  brightness and tint variation, to break the repetition. Medium; costs samples.
- **VP2-O3 Slope-aware rock in the shader**: blend toward the stone slice by the normal against local up (world Z on
  heightmap worlds, radial on a planet, which needs the planet's centre as a material parameter). Continuous, with no
  per-quad edges. Medium.
- **VP2-O4 Soft material borders on voxel meshes**: the meshers emit the second material and its weight in vertex
  colour R and A, which `M_VoxelMaster`'s `FarFieldMaterialPick` already reads since G45, so the shader side exists and
  `DC_MATERIAL_BLEND_PLAN.md` shrinks to the mesher side. Up close the temporal dither may itself read as noise, so a
  two-sample blend in border quads may be needed there (a decision during the work). Separately, G39's lattice spacing
  or the biome blend width can be tuned to coarsen the mosaic. Medium.
- **VP2-O5 Content**: fill Sand and Frozen Dirt, add roughness maps, keep one style, and raise `TextureArraySize` to 1024
  once mips are in use. After the user's call.

## VP3 Hill shadows appear and disappear

### Findings

- **F3.1 Shadow casters are culled by the camera's frustum (confirmed).** `FVoxelSceneProxy::GetDynamicMeshElements`
  culls every chunk (`VoxelSceneProxy.cpp:400-426`) and every seam bucket (`:558-567`) against `View->ViewFrustum`. For a
  shadow depth pass the engine calls the proxy with a snapshot of the camera's view (`FProjectedShadowInfo::SetupShadowDepthView`,
  `Engine/Source/Runtime/Renderer/Private/ShadowDepthRendering.cpp:1390`; `ModifyViewForShadow` leaves `ViewFrustum`
  alone) and passes the shadow's caster volume through `SetDynamicMeshElementsShadowCullFrustum`
  (`ShadowSetup.cpp:2863-2881`). The proxy never reads it; the engine's HISM does
  (`HierarchicalInstancedStaticMesh.cpp:1562`). So a hill behind, beside or above the view is dropped from the shadow
  map and casts no shadow onto the ground in view; turning the camera brings it into the frustum and its shadow appears.
  Every world mode is affected; scatter (trees, rocks) is not.
- **F3.2 No sun shadows past 200 m.** A dynamic shadow distance of 20,000 uu against a view distance of 40,000, no far
  cascades and no distance field shadows (the voxel meshes have no distance fields): distant hills never cast, and the
  shadow edge moves with the camera. Other maps' lights need the same check.
- **F3.3 Lumen sees the terrain only on screen.** With no ray tracing geometry and no distance fields, Lumen's sky
  occlusion and bounce light on the terrain come from screen traces alone. An overhang or ridge that leaves the screen
  stops darkening what it covers, so valleys brighten and darken as the view turns: soft shadows that come and go,
  besides F3.1's sun shadows.

### How to investigate

1. A repro: a ridge between the sun and the camera, a scripted yaw sweep that takes the ridge out of view and back, a
   `pie_screenshot` at each step. Find one on `PlanetTest` and one on `VoxelDemo`.
2. VP3-O1 behind a cvar (`voxel.Render.ShadowCullFrustum`, 0 = before) and the same sweep: the shadow should stay.
3. Cost: a shadow-pass count in `FVoxelBatchCounters` (batches drawn into shadow maps), and the bench's frame, render
   thread and GPU times on `VoxelDemo` and a planet, three runs per setting. More casters will now draw; check the
   2,000-batch cap per call (`VoxelSceneProxy.cpp:348`) is not reached by a cascade.
4. `r.Lumen.DiffuseIndirect.Allow 0` and `r.Lumen.ScreenProbeGather.ScreenTraces 0` on the same sweep, to see how much
   of what is left is F3.3.

### Options

- **VP3-O1 Cull with the shadow's volume** (VoxelRendering): when `View->GetDynamicMeshElementsShadowCullFrustum()` is
  set, test chunks and seams against it, the box moved by `View->GetPreShadowTranslation()` (zero for the sun's
  whole-scene cascades, set for other shadows); otherwise the camera's frustum as now. Small, and the fix for F3.1.
- **VP3-O2 Sun shadow range**: a longer dynamic shadow distance with more cascades (cost); far shadow cascades (`bCastFarShadow`
  on the region components and far cascades on the light; the proxy ignores `EDrawDynamicFlags::FarShadowCascade` and
  draws full detail); or virtual shadow maps again. VSM was turned off after a "Non-Nanite job queue overflow"
  (comment at `VoxelSceneProxy.cpp:343-344`); render regions and the batch cap have changed since, but geometry drawn
  every frame keeps VSM's page cache from helping. A decision on the budget.
- **VP3-O3 Make the terrain visible to Lumen**, an investigation spike to choose between: (a) ray tracing geometry per
  chunk built from its buffers (`GetDynamicRayTracingInstances`), which also gives ray-traced shadows and reflections of
  the terrain; what Lumen does when it hits geometry without a surface cache needs checking; (b) distance fields per
  chunk from the voxel densities (already a signed distance), for software Lumen, distance field shadows (F3.2's far
  shadows) and distance field AO; whether a custom proxy can register distance field objects needs checking; (c) a
  cheap stopgap: per-vertex sky visibility computed from the voxels in the smooth meshers, into the material's ambient
  occlusion input (the cubic mesher has vertex AO; the smooth meshers and `M_VoxelMaster` have none). Stable, does not
  depend on the view, approximate. (a) and (b) are large.

## Order

1. **Milestone 1, causes and the two small fixes** (done 2026-10-24: "Milestone 1 results"). VP3-O1 (shadow cull
   volume) and VP2-O1 (gradient sampling), each behind a cvar with its A/B. The VP1 A/B session (steps 1-6) to find
   the flicker; it pointed at F1.7 and F1.4, not the clouds. VP1-O5 and VP2-O6 followed the same day; VP1-O6 is open.
   Then the user looks at the result in PIE.
2. **Milestone 2, the terrain's look.** VP2-O2 to O4 in the order the user's review sets, and VP2-O5 if the textures
   still disappoint.
3. **Milestone 3, shadow range and Lumen.** VP3-O2's budget decision, and VP3-O3's spike.

Media capture can start after milestone 1, with cinematic cloud settings (VP1-O2) and a raised shadow distance for the
shots.

## Decisions for the user

- **D-VP1** Clouds on small planets: volumetric clouds scaled with the planet (VP1-O1 and O3), a 2D cloud shell, or
  none (VP1-O4). And which planet the media uses: `PlanetTest` (2.5 km), or `Review_Planet_M` (50 km).
- **D-VP2** After VP2-O1: keep the current textures with VP2-O2 to O4, or replace them, and in which style.
- **D-VP3** The sun shadow budget (distance and cascades, far cascades, or VSM), and whether to make the terrain
  visible to Lumen (VP3-O3).

## Tooling

- **T1 Temporal noise metric**: a script that takes a burst of PIE screenshots along a scripted camera move and reports
  the mean frame-to-frame difference over a mask (sky, or terrain), so VP1 and VP2's A/Bs give numbers, not only
  impressions.
- **T2 Shadow pass counts**: VP3's investigation step 3 (`FVoxelBatchCounters`, the bench's columns).
