# Editor Tools — Procedural Field Visualization

Editor-only authoring tools for **iterating on world configuration by seeing the anticipated
procedural result** — terrain shape, biome placement, cave presence, and climate — over regions
**larger than the runtime view distance**, without streaming or entering PIE.

This is Milestone 1 of the editor toolset. It is intentionally a *foundation*: the field/registry
core is designed so new visualizations and future tools (non-streaming 3D preview, edit authoring,
claim/POI visualization) are additive.

---

## Where it lives (and why it never ships)

Everything is in the **`VoxelWorldsEditor`** module — `Source/VoxelWorldsEditor/`, registered in
`VoxelWorlds.uplugin` with `"Type": "Editor"`. Editor-type modules are compiled **only** for editor
targets and are stripped from cooked/shipping builds by construction, so no per-symbol `WITH_EDITOR`
gating is needed. The module depends only on the runtime voxel modules (downward) — never the reverse.

The one runtime-side addition is `FVoxelCaveQuery` (in `VoxelGeneration`, see below), a pure sampling
facade that is useful at runtime too and carries no editor dependency.

---

## Architecture: the shared field core

All user-facing surfaces are thin shells over one core, so adding a visualization is cheap.

| Type | File | Role |
|---|---|---|
| `FVoxelFieldSampleContext` | `Public/VoxelFieldTypes.h` | Immutable snapshot built from a `UVoxelWorldConfiguration` — instantiates the matching `IVoxelWorldMode` (with biome context, exactly like `UVoxelChunkManager::Initialize`) and resolves the biome-climate noise params. Chunk-independent + thread-safe for read. |
| `FVoxelEditorField` | `Public/VoxelFieldTypes.h` | A registered field: id, display name, `EVoxelFieldKind` (Scalar2D / Categorical / Scalar3D), color mapping, and a `Sample` delegate. |
| `FVoxelFieldRegistry` | `Public/VoxelFieldTypes.h` | Static registry of fields. Built-ins registered on module startup. |
| `FVoxelFieldImageBaker` | `Public/VoxelFieldImageBaker.h` | Samples a field over a region into an `FColor` array / transient `UTexture2D`. Row = world Y ascending, col = world X ascending — matches `UVoxelMapSubsystem` tiles, so a Height/Biome bake matches the minimap. |
| `FVoxelCaveQuery` | `VoxelGeneration/Public/VoxelCaveQuery.h` | Runtime cave-presence facade (sibling of `FVoxelSurfaceQuery`); delegates to `FVoxelCPUNoiseGenerator::CalculateCaveDensity`. |

### Built-in fields

`Height`, `Slope`, `SurfaceMaterial`, `Biome`, `Temperature`, `Moisture`, `Continentalness`,
`CavePresence`. Scalars use a heatmap ramp (Height auto-ranges for contrast); `SurfaceMaterial` uses
the voxel material palette; `Biome` uses a distinct hashed hue; `CavePresence` is a 3D field sampled
at `SampleZ`.

### Adding a new field (the extension point)

Register one `FVoxelEditorField` (e.g. from `EnsureBuiltinsRegistered` in `VoxelFieldTypes.cpp`, or
from any editor module at startup):

```cpp
FVoxelEditorField F;
F.Id = TEXT("MyField");
F.DisplayName = NSLOCTEXT("VoxelEditorFields", "MyField", "My Field");
F.Kind = EVoxelFieldKind::Scalar2D;      // or Categorical / Scalar3D
F.bAutoRange = true;                     // scalar auto-contrast (optional)
F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double Z) -> float
{
    return /* sample something from C at (X,Y[,Z]) */;
};
FVoxelFieldRegistry::RegisterField(F);
```

Both surfaces below pick it up automatically.

---

## Surface 1 — Dockable panel

**Tools ▸ Voxel Worlds ▸ Voxel Field Preview** (also under the Window/Tools tab list).

Location-agnostic iteration: pick a `UVoxelWorldConfiguration` asset, a field, a center / region size /
resolution / sample-Z, and **Bake**. The heatmap displays in an `SImage` that scales to fill the panel
(larger when the tab is maximized). Baking is synchronous on the game thread — cheap at typical
resolutions; it can move to async background baking later if needed.

Implementation: `Private/SVoxelFieldPreviewPanel.{h,cpp}`; tab + menu registration in
`Private/VoxelWorldsEditor.cpp`.

## Surface 2 — In-world preview actor

`AVoxelFieldPreviewActor` (`Public/VoxelFieldPreviewActor.h`) — drop it into a level to frame a region
spatially. Its Details panel drives Configuration / Field / RegionSize / Resolution / SampleZ, with a
**Regenerate** button; it re-bakes automatically on property change and when moved. The field is shown
on a flat plane sized to the region and centered on the actor. Its unlit texture material is built in
C++ (no content-asset dependency). Because it lives in the editor module, it never cooks — **don't leave
one saved in a level you intend to ship.**

---

## Verification

- **Correctness (headless):** automation test `VoxelWorlds.Editor.FieldSampler.Parity`
  (`Tests/VoxelFieldSamplerTests.cpp`) asserts the `Height` field is bit-identical to
  `FVoxelSurfaceQuery::GetSurfaceHeight`, cave presence stays in [0,1], and the baker output is sized
  correctly — so the tools cannot silently drift from real generation. Run headless:
  `UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests VoxelWorlds.Editor.FieldSampler.Parity" -unattended -nullrhi -nosplash -TestExit="Automation Test Queue Empty"`.
- **Visual:** open the panel, bake `Height`/`Biome` for a config, and cross-check against the
  `UVoxelMapSubsystem` minimap for the same config (shared math ⇒ they should agree).

## Build note

Adding the module or the actor `UCLASS` cannot Live-Code hot-reload — close the editor, run
`Build.bat VoxelEngineEditor Win64 Development`, relaunch. Editing only Slate/impl bodies can Live-Code.

---

## Future expansion (foundation is ready for these)

- **Non-streaming 3D "config-test" preview** — CPU generate+mesh a bounded region into a PMC preview.
- **Edit authoring + authored claims** — editor brushes → `UVoxelEditManager`, persisted with a config.
- **Claim / edit / scatter / POI visualization** — new registry fields + gizmos (claims read from placed
  `AWorldClaimVolume` actors, present in the editor world).
- **Editor Mode (EdMode) + toolkit** — viewport brush interaction, layered on the same core.
