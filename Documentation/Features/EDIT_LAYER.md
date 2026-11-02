# Edit Layer System

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelCore (`VoxelEditTypes.h`, `VoxelEditManager.h/.cpp`); the chunk manager (VoxelStreaming) reacts to edits.

## Overview

The edit layer modifies voxel terrain at runtime with add, subtract, set and paint edits, keeps an undo and redo
history, and saves the edits to a file. The February design specification is archived as
[EDIT_LAYER_SPEC_2026-02.md](../Research/Archive/EDIT_LAYER_SPEC_2026-02.md); the code is the reference.

## How it is built

The edit layer is an **overlay** on the generated voxels:
- `UVoxelEditManager` keeps one `FChunkEditLayer` per edited chunk, a sparse `TMap<int32, FVoxelEdit>` by voxel index.
- Edits are never written into the generated data; a reader merges them with `FVoxelEdit::ApplyToProceduralData()`.
- An edit's mode is `EEditMode` (`Set`, `Add`, `Subtract`, `Paint`). `Add` and `Subtract` are relative (`DensityDelta`),
  `Add` and `Paint` carry `BrushMaterialID`, `Set` carries the whole voxel (`NewData`).
- `SaveEditsToFile` / `LoadEditsFromFile` write a binary file with the magic `VETI`, version 3 (version 2 still loads).
- The game module's `AVoxelDemoActor` drives edits from the mouse and keyboard; `bUseDiscreteEditing` (or a cubic world)
  edits single blocks.

### How a new edit combines with the edit already on a voxel

`UVoxelEditManager::ApplyEditInternal` keeps one edit per voxel. When a voxel already has one:

| Existing edit | New edit | Result |
|---------------|----------|--------|
| Add / Subtract | Add / Subtract | One relative edit with the summed delta; a sum of 0 removes the edit (or becomes a Paint when the new Add carries a material) |
| Set (absolute) | Add / Subtract / Paint | A Set whose voxel is the new edit applied to the existing voxel: a dig under a POI pad takes density from the pad, not from the procedural terrain under it. Undo restores the pad's Set (below) |
| anything | Set | The new Set replaces it |
| Add / Subtract | Paint | The Paint replaces it (the delta is lost) |

Until 2026-10-14 a relative edit on a Set voxel replaced the Set, so the voxel fell back to procedural data minus
the delta; one dig under a pad standing over air removed the aimed voxel and its four face neighbours, which had
only received the brush falloff (world modes review G2). Tests: `VoxelWorlds.Edit.Composition.*`.

### What `OnChunkEdited` tells its listeners

`OnChunkEdited(ChunkCoord, Source, EditCenter, EditRadius)` fires once per edited chunk when an operation ends (and for
an undo, redo, clear or load). The centre and radius are the operation's brush (`ApplyBrushEdit`), and a redo reports
the operation's again; an operation of single-voxel edits (a POI's terraform, a dungeon stamp, water propagation), an
undo, a clear and a load report the zero vector and 0. While it is broadcast, `GetBroadcastEditBounds` gives the
chunk-local bounds of the voxels the edit changed in that chunk, so a listener can tell which layers it reached (scatter's readers, world modes review G33),
or which voxels to look at (water propagation scans them for voxels the edit opened to the sea, G58; the movement
component's embed check, G55).
Until 2026-10-20 the centre and radius stayed set after a brush, so every later operation of single-voxel edits reported
the last brush's (G53): navigation invalidated around the old dig instead of the terraform, and collision's edge and
corner re-cook (G32) judged the wrong sphere. Tests: `VoxelWorlds.Scatter.EditReaders.Rule`.

### What an edit re-meshes

The chunk manager's handler for `OnChunkEdited` re-meshes the chunk the edit changed and restales its seams
(`MarkChunkDirty`), and re-cooks the collision of the chunk and its face neighbours (a cook reads its 26 neighbours'
voxels). A chunk mesh is built from the chunk's own voxels and edit layer, and a seam from its participants', so nothing
else needs a re-mesh. Until 2026-10-20 the handler also re-meshed the six face neighbours, which rebuilt the same meshes
and all their seams (world modes review G56). An edit that reaches a
chunk already on its way to a mesh restales its seams too, and if that mesh is on a worker (built before the edit) the
chunk re-meshes when it lands (G57; before, both stayed stale). `voxel.Edit.MeshWork [reset]` logs the meshing work since
a reset. Tests: `VoxelWorlds.Streaming.EditRemesh.Plane`, `.Planet`.

### Undo and redo

Every operation between `BeginEditOperation` and `EndEditOperation` (or the one `ApplyEdit` and `ApplyBrushEdit` start
themselves) is one undo step of at most `MaxUndoHistory` (100). It records, for each voxel it changed, the voxel's chunk
and the whole edit the voxel held before and after (`FVoxelEditChange`, 112 bytes, none for the generated voxel), and when
it ends the source, brush centre and radius it broadcast. The history (undo and redo) also keeps within
`voxel.Edit.UndoHistoryMB` (32 MB; 0 = no budget): past it the oldest operations are dropped, the newest always kept
(`GetHistoryMemoryUsage`; world modes review G68).

- **Undo** puts each voxel's previous edit back, or removes the edit when there was none, last change first, in the
  voxel's own chunk; it broadcasts each chunk it changed as a System edit with no radius (the restored voxels re-mesh,
  re-cook and re-extract their scatter, the chunks that read them too, and water propagation scans them).
- **Redo** puts each voxel's edit back as the operation left it, first change first, and broadcasts each chunk with the
  operation's own source, centre and radius, so a player brush clears scatter in its sphere again and a system edit
  re-extracts the chunks that read it.
- **Cancel** (`CancelEditOperation`) restores the open operation's voxels as an undo does.
- An operation begun with `bAddToUndoHistory` false takes no undo step and leaves the redo stack as it is; it is
  broadcast like any other. Water propagation's flood is one (it follows from the edit that let the water in): undoing
  that edit puts its voxels back as they were before it, the flood there with them, and its redo floods again. Water the
  flood put beyond the edit's voxels stays.
  The world's own generation is outside it too (G68): dungeon stamps (`UDungeonVoxelStamper`) and their entrances
  (`UDungeonEntranceStitcher`; the runtime campsite levelling that was the third went in code review CR1, D-CR17), so
  an undo takes back the player's own last edit, never a POI. Until 2026-10-22 they were undo steps: on `Review_Planet_L` four POI operations
  around a site held 94 MB of history, and an undo took back the last of them. As with the flood, undoing a player's
  edit made before a stamp puts back that edit's voxels as they were before it, the stamp's there with them.
- A clear (`ClearChunkEdits`, `ClearAllEdits`) is outside the history; a load (`LoadEditsFromFile`) clears it.
- `FVoxelEdit::OriginalData` deleted in CR1 (CORE-12): the edits file is version 3 without it (version 2 still loads).

Until 2026-10-20 the record was the operation's list of edits with their `OriginalData`: undo took the first of the
operation's chunks holding an edit at the voxel's local index and set `OriginalData` on it (removing it when that was
air), redo wrote every edit into the operation's first chunk and broadcast only that chunk with the current source and no
radius. So a brush across a chunk face came back in one chunk, a voxel's previous relative edit (a dig under a dig), a
previous Set to air and an edit removed by cancelling deltas did not come back, and a system edit redone as a player's
re-extracted no scatter. Water propagation's flood was an undo step of its own, and its undo removed the dig at the voxels
it had reached (world modes review G60; history: `WORLD_MODES_REVIEW_PLAN.md` §19.17). Tests: `VoxelWorlds.Edit.UndoRedo.Records`,
`VoxelWorlds.Streaming.WaterPropagation.Undo.Plane`, `.Planet`; `Streaming.EditRemesh.*` edits 6 and 7 and
`Scatter.EditReaders.{Plane,Planet}` undo and redo too.

### The cylinder brush's axis (spherical Phase 4, SP-F8)

`FVoxelBrushParams::Axis` is the Cylinder shape's axis. Zero (the default) means the world's local up at the brush
centre: +Z on heightmap worlds (the old arithmetic, the same voxels: `Edit.Brush.CylinderAxis`) and the radial direction
on a planet, so a dig there is a shaft along local up. A tilted axis widens the voxel box the brush visits by √2.
