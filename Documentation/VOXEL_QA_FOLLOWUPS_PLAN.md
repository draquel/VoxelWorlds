# Voxel QA follow-ups plan (R22-R24)

The first VoxelDemo QA pass (2026-10-11) left three voxel engine items out of the refinement milestones:
R22 (the map ignores voxel edits), R23 (a distant chunk stayed visible) and R24 (seam pop-in). This plan traces
each to its cause, proposes a design, and records the decisions taken. The parent plan is
`Plugins/VoxelWorldPOI/Documentation/QA_REFINEMENT_PLAN.md` in UE5_PluginDev. File references are relative to
`Plugins/VoxelWorlds/Source/` unless noted; line numbers are as of VoxelWorlds `ebeaefb`.

## Decisions (2026-10-12)

- **Order:** R23, then R22, then R24.
- **R24:** crossfade seam strips and coordinate LOD transitions: a chunk whose LOD changes keeps its old mesh
  until its rebuilt seams land (capped wait), then old and new fade together. New frontier chunks fade in.
- **R22:** the map keeps a per-tile edit overlay that is saved with the world, so edits show right after loading.
  Areas from saves made before this change update the next time they load.
- **R22 scope (default, not asked):** every edit source (player digs and builds, POI stamps, shafts, campsite pads).
  Edits that do not change the top-down surface, such as dungeon interiors, produce no overlay.

## Summary

| ID | Item | Cause | Status of the cause | Repos | Size |
|----|------|-------|---------------------|-------|------|
| R23 | Lingering distant chunk | Seam strips submitted after their owner unloaded; mesh results dropped by the completion drain | Both reproduced live 2026-10-12; fixed in VoxelWorlds#70 | VoxelWorlds | S-M |
| R22 | Map shows edits | Map tiles are built from analytic noise only; nothing reads chunk or edit data | Traced in code; phases 1-2 (live overlays) in VoxelWorlds#73, phase 3 (saved with the world) in VoxelWorlds#74 | VoxelWorlds, UE5_PluginDev (VoxelWorldSave) | M-L |
| R24 | Seam pop-in | Seams have no fade path; LOD changes show the new interior beside the old seam until the rebuild lands | Reproduced live 2026-10-12; P1+P2 fixed in VoxelWorlds#71, P3 in VoxelWorlds#72 | VoxelWorlds | L |

## R23 Lingering distant chunk

**Report:** "Noticed a distant chunk which should be unloaded was still visible" (finding, 2026-10-11 15:32 UTC).

**Status (2026-10-12): implemented, R23a-R23f as specified below** (VoxelWorlds#70; verification results in the PR). As built:
- R23a: `FVoxelSeamRegistry::GetResidencyEpoch` / `IsResidencyCurrent`; `FVoxelSeamJob::OwnerResidencyEpoch` travels with
  the job, `DispatchSeamJob` and `ProcessCompletedSeamMeshes` drop stale ones (`[SeamLat] rejectDeparted`).
- R23b: `VoxelDrainBounded` (`VoxelStreaming/Public/VoxelBoundedDrain.h`) for the generation, mesh, seam and collision drains.
- R23c: `AsyncGenerationInProgress` / `AsyncMeshingInProgress` hold dispatch stamps (time, frame, serial); results carry
  the serial, so a superseded result is discarded. Unloads release the slot. `voxel.Stream.MeshStuckSeconds` (10 s, and
  at least 60 manager ticks so a PIE pause is not "stuck") re-queues stranded chunks. The in-flight skip in the
  generation and meshing queues defers a waiting request instead of dropping it.
- R23d: `GetChunksToUnload` returns every candidate farthest first; `IVoxelLODStrategy::SelectUnloadCandidates` applies
  the manager's veto, then the cap.
- R23e: `IVoxelLODStrategy::GetLoadDistance` (= view distance for distance bands); the "Edit-pinned" line logs the distance.
- R23f: `voxel.Stream.AuditResidency` (and `UVoxelChunkManager::AuditResidency` for scripts). Seam buckets are checked
  against a game-thread mirror in `UVoxelWorldComponent` (`IVoxelMeshRenderer::GetSeamMeshOwners`).

**The original hypothesis does not hold up.** Edit-pinned streaming (VoxelWorlds#55) loads any edited chunk inside
the unload radius (`VoxelStreaming/Private/VoxelChunkManager.cpp:2048-2097`, radius = `GetUnloadDistance()` =
1.2 × ViewDistance = 48,000 in the demo) and vetoes analytic unloads for edited chunks (`:2127-2139`). That can load
an edited chunk past the 40,000 view edge, but it logs "Edit-pinned chunk ... queued for load" every time, and no
surviving editor log from 2026-10-11 or 2026-10-12 contains that line. The log for 15:32 UTC was rotated out (the
oldest surviving log opens at 15:50 UTC), so this cannot be ruled out for that moment, but it is not needed to
explain the report. A live repro found two real defects instead.

**Live repro (2026-10-12, VoxelDemo, real RHI).** Start PIE, let streaming settle, then `vox.TP` 70 km east (site
B), wait 2.5 s while B streams in and its seam jobs are in flight, and `vox.TP` another 70 km (site C). Then run
`Saved/ClaudeScripts/r23_residency_probe.py`, freeze streaming (`vox.Freeze 1 streaming`) and fly `pie_camera` over B.

1. **Orphaned seam strips (confirmed).** Over site B, with no chunk loaded within about 17 km, an L-shaped pair of
   seam strips and several fragments float in empty space (`Saved/Screenshots/Claudius/r23_siteB_oblique.png`).
   Site A, which had settled before the first teleport, shows only a stray fragment.
   - Cause: `ProcessCompletedSeamMeshes` (`VoxelChunkManager.cpp:3866-3940`) never checks that the owner is still
     resident. A job dispatched just before its owner unloads completes afterwards; `SeamOwnerSlots.FindOrAdd`
     (`:3909`) recreates the owner's slot entry and `UpdateSeamMeshFromCPU` (`:3939`) submits a bucket for a chunk
     that no longer exists. The unload already ran `RemoveSeamMesh` (`:3340-3353`), so nothing ever removes it.
     The bucket is only replaced if that owner loads again.
   - Most likely after teleports, respawns and fast travel, when many owners unload while jobs are in flight. It
     can also happen at the trailing edge of normal travel.
2. **Mesh results dropped by the completion drain (confirmed).** The probe found chunk (-124,-71,0) still loaded
   110 km from the pawn, in state `Meshing`, not rendered and not edited, and still there 75 s later with an empty
   meshing queue (`Meshing: 1` in `GetDebugStats`).
   - Cause: `while (CompletedMeshQueue.Dequeue(Result) && ProcessedCount < MaxProcessPerFrame)` (`:3201`, cap 8)
     evaluates `Dequeue` first. When nine or more results are waiting, the ninth is popped and then thrown away.
     The demo config allows 16 async meshes (`MaxAsyncMeshTasks = 16`, gate at `:2636`/`:2675`), so bursts reach it.
   - The chunk then stays in `Meshing` and in `AsyncMeshingInProgress` for the rest of the session:
     - `UpdateUnloadDecisions` only unloads `Loaded` chunks (`:2125`), so it never unloads.
     - The in-progress skip in `ProcessMeshingQueue` (`:2681-2686`) silently drops every later remesh request, so it
       never remeshes.
     - It permanently holds one of the 16 async-mesh slots.
   - If the dropped result was an LOD remesh of a rendered chunk, the old mesh stays on screen wherever the player
     goes. That is exactly "a distant chunk that should have unloaded".
   - The slot leak also fits the open "aged-session throughput loss / worse over long runs" note in the pop-in epic:
     each leak permanently narrows the mesh pipeline. Not yet measured; see verification.
   - The same pattern exists in `ProcessCompletedAsyncGenerations` (`:2459`, cap 8 against `MaxAsyncGenerationTasks`
     8 in the demo, so not reachable today) and `VoxelCollisionManager::ProcessCompletedCollisionCooks`
     (`VoxelCollisionManager.cpp:904`, cap `voxel.Collision.MaxAppliesPerFrame`, default 4).

**Also found in code (not observed).**

3. **The unload cap counts chunks the manager then refuses.** `FDistanceBandLODStrategy::GetChunksToUnload`
   (`VoxelLOD/Private/DistanceBandLODStrategy.cpp:441-493`) walks `LoadedChunkCoords` in set order and stops at
   `MaxChunksToUnloadPerFrame` (16). The manager then skips edit-pinned chunks and chunks not in `Loaded` (such as
   the stranded chunk above), but they have already used the budget. Set order is stable, so 16 such chunks early
   in the set would stop every later chunk from unloading. Loaded counts in the 2026-10-11 logs stay at 1,050-1,300,
   so it did not happen there, but every stranded chunk now costs one slot per frame.
4. **Edit-pinned loads reach past the view edge.** The original hypothesis: an edited chunk at 1.0-1.2× view
   distance is loaded on its own, beyond the rest of the terrain.

**Fixes (one PR, VoxelWorlds).**

- **R23a Seam results for departed owners.** Drop a completed seam result unless its owner is still tracked and in
  the same residency as when the job was dispatched. Add a per-chunk residency epoch to `FVoxelSeamRegistry`
  (bumped in `RegisterChunk`); the job captures the owner's epoch, and `ProcessCompletedSeamMeshes` compares it before
  touching `SeamOwnerSlots`. An epoch, not just "is loaded", also rejects a result from an earlier residency of an
  owner that has since reloaded.
- **R23b Bounded drains.** Swap the operands in all three loops (`ProcessedCount < Max && Queue.Dequeue(...)`), via
  one small helper so the pattern cannot come back.
- **R23c Stranded-state safety net.** Stamp `AsyncMeshingInProgress` with a dispatch time. A chunk in flight for more
  than `voxel.Stream.MeshStuckSeconds` (default 10) logs a warning, leaves the set and goes back to `PendingMeshing`,
  so a mesher that loses a request (for example the GPU readback path) cannot strand a chunk either. Give generation
  the same treatment.
- **R23d Unload cap after the veto.** The strategy returns every unload candidate; the manager applies the
  `Loaded`-state and edit-pin filters and only then the per-frame cap. Alternatively it passes a filter callback. The
  candidates stay sorted farthest first.
- **R23e Edit-pin distance.** Pin-load edited chunks only within ViewDistance (new `IVoxelLODStrategy::GetLoadDistance()`)
  and keep them to the 1.2× unload radius. Add the distance to the "Edit-pinned" log line.
- **R23f Audit command.** `voxel.Stream.AuditResidency` logs:
  - manager chunks beyond the unload radius, by state;
  - renderer chunks the manager no longer tracks;
  - seam buckets whose owner is not tracked;
  - async jobs older than the stuck threshold.

  This replaces the Python probe and gives QA a one-line check.

**Tests (headless, pure logic).**
- `VoxelWorlds.Streaming.Residency.BoundedDrainKeepsOverflow`: 20 queued, cap 8: 8 processed, 12 left, none lost.
- `VoxelWorlds.Streaming.SeamRegistry.ResultEpochRejectsDepartedOwner`: register, capture epoch, unregister: rejected;
  re-register: the old epoch is still rejected and a new one accepted.
- `VoxelWorlds.LOD.UnloadCandidates.VetoDoesNotStarve`: 20 refused candidates ahead of one far chunk; the far chunk
  still unloads within the frame cap.

**Verification.**
- Rerun the A→B→C teleport repro. `voxel.Stream.AuditResidency` must report zero in every category, and the survey
  camera over sites A and B must show empty space.
- Soak: W-04 (`vox.SetSpeed 3000`, `vox.AutoRun 1 0 0`) for 20 minutes with periodic teleports. Log the async-mesh
  in-flight count at idle before and after the fix: a leak shows as a non-zero idle count that grows. Compare
  `voxel.Bench.Run` catch-up at minute 1 and minute 20 to settle the aged-session question.
- In-editor suites: `VoxelWorlds.Streaming`, `VoxelWorlds.LOD`, the DualContour seam suites.

**Risks.** The safety-net timeout must sit well above real mesh times, including hitches and GPU readback queues;
measure the worst dispatch-to-completion time during a teleport burst before settling on 10 s. Rejecting late seam results can briefly leave a reloaded owner without its seams until
they are re-dirtied; `RegisterChunk` already dirties all incident seams, so the rebuild is scheduled anyway.

## R22 Map shows player edits

**Report (W-05):** "terrain modifications for POIs are not recognized in the map." R17 made POI pads show through
conditioning zones; digs, builds, shafts and campsite pads are voxel edits and still do not show.

**What the code does.**
- Tiles are built analytically on background tasks (`VoxelMap/Private/VoxelMapSubsystem.cpp:579-853`).
  - Pass 1 samples `WorldMode->GetTerrainHeightAt` plus conditioning zones into a height grid with a 1-pixel apron.
  - Pass 2 colours each pixel through `FVoxelSurfaceQuery::QuerySurfaceConditions`, with hillshade and analytic
    water (height < `WaterLevel`).
  - No chunk voxel data and no edit-layer data are read.
- One tile is one chunk column: 64 × 64 pixels for 4,800 uu, so one pixel is one voxel column (75 uu).
- Edits live in `UVoxelEditManager::EditLayers`, a `TMap<FIntVector, FChunkEditLayer>` of sparse per-voxel edits
  (`VoxelCore/Public/VoxelEditManager.h:408`). The manager is game-thread only.
  - Add and Subtract edits store a density delta relative to the procedural voxel (`VoxelEditTypes.h:202-247`), so
    the edited surface cannot be computed from the edit layer alone. It needs the procedural data, which in practice
    means the chunk is loaded.
- `OnChunkEdited(ChunkCoord, Source, Center, Radius)` fires on the game thread once per chunk per edit operation.
  - Undo, redo, clear and load send a zero centre and radius, so key on the chunk coordinate.
  - Water propagation fires it every propagation frame (System `Set` edits that add the water flag), so listeners
    must coalesce.
- The edit-merged chunk data already exists as a shared immutable snapshot: `UVoxelChunkManager::GetSeamVoxelSnapshot`
  (`VoxelChunkManager.cpp:3953-3979`), private, cached per (chunk, ContentVersion).
- `FVoxelSurfaceQuery::ExtractSurfaceFromColumn` (`VoxelGeneration/Private/VoxelSurfaceQuery.cpp:165-201`) is the pure,
  thread-safe surface kernel.
- The map saves only explored tile coordinates (R25); every tile is regenerated after load. `LoadWorld` applies the
  document, which queues map tiles, before `LoadEditsFromFile` (`Plugins/VoxelWorldSave/.../VoxelWorldSaveSubsystem.cpp:486, 495`).

**Existing map bugs this work must fix.**
- A tile re-queued while it is still cached and four tasks are running stays in `PendingTiles` for good. The drain
  skips cached keys (`VoxelMapSubsystem.cpp:842`), so it never redraws and also blocks later re-queues.
- A tile re-queued while its task is in flight loses the request: the completion removes it from `PendingTiles`.
- `HandleTerrainConditioningChanged` (`:522-545`) empties the whole cache, so the map flashes blank until redraws
  land. Edits must keep the old tile visible until the new one arrives.

**Design: an edit overlay per tile.**
- A new plain struct `FVoxelMapEditOverlay` (VoxelMap) holds, for one tile:
  - a column mask of where edits change the top surface;
  - per masked column: surface height, material ID and a water bit (water-flagged voxel directly above the surface).
- **Building an overlay (worker).** `FVoxelMapEditOverlay::Build` takes the tile's edited chunk stack and the analytic
  heights, and returns the overlay:
  - Edited chunks arrive as (Z, shared snapshot), plus the edit-layer keys, from which it derives the edited columns.
  - For each edited column it scans top-down from the top of the highest edited chunk with `ExtractSurfaceFromColumn`.
  - Unedited chunks in that range come from their resident data if loaded, otherwise from the analytic height
    (solid below it).
  - A column whose surface and material match the analytic result is left out. This is how buried edits, such as
    dungeon interiors, produce nothing.
  - If the scan falls through the bottom of the stack (dug through into an unloaded chunk), the column stays
    pending.
- **Gathering (game thread).**
  - `OnChunkEdited` marks the tile (chunk X, Y) dirty.
  - A coalescing pass every `voxel.Map.EditRefreshSeconds` (default 0.5) builds the inputs for up to N dirty tiles.
  - A new public `UVoxelChunkManager::GetEditMergedSnapshot(Coord)` wraps the snapshot cache, so the copy is shared
    with the seam pipeline and built at most once per content version.
  - Only edited chunks at or above one chunk below the analytic surface are required. If one of them is not loaded,
    the tile keeps its current overlay and waits for that chunk's `OnChunkGenerated`, which the map already binds.
- **Composing tiles.**
  - The existing tile task captures a copy of the tile's overlay, and of its neighbours' border columns for the
    hillshade apron.
  - It uses overlay heights and materials where the mask is set, and the voxel water bit instead of the analytic
    water test there. A pit dug below the water line therefore stays land-coloured until water actually floods it.
  - Conditioning changes regenerate tiles from the stored overlays with no chunk data.
- **Staleness.**
  - A per-tile generation counter replaces the global one for edit refreshes; stale results re-pend only their tile.
  - The old tile stays cached until the new one lands.
  - If an overlay's border columns change, the neighbour tiles are re-queued so the hillshade stays continuous.
- **Saving (UE5_PluginDev, VoxelWorldSave).**
  - Overlays go into the world document as `MapEditOverlays`: tile key, mask, then height, material and water for
    the masked columns, zlib + base64 as in R25, with a format version.
  - On load they are imported before `LoadEditsFromFile`. That call's `OnChunkEdited` broadcasts then only mark tiles
    pending, and the saved overlay stays on screen until each chunk loads and the overlay is rebuilt.
  - Saves made before this change have no overlays and update area by area as chunks load (decision above).

**Alternatives considered.**
- *Reconstruct from the edit layer alone:* wrong for brush Add/Subtract, which are deltas.
- *Regenerate procedural data for every edited chunk after load:* exact, but costs a full chunk generation per edited
  chunk at load, and saves can hold thousands of edited chunks. The user chose the saved overlay instead.
- *Top-down scene capture of loaded terrain:* lighting-dependent, heavy, and needs the area loaded anyway.
- *Per-column `QueryEditMergedSurface`:* about 135 point lookups per column on the game thread, roughly 550k per tile.

**Phases.**
1. Overlay kernel and tests; map queue fixes (drain, in-flight re-queue, keep-cached refresh, per-tile generation).
2. Live refresh: `GetEditMergedSnapshot`, `OnChunkEdited` coalescing, gather and compose, neighbour apron, water bit.
3. Persistence: overlay encode/decode, save document field, load ordering, pending-on-load reconciliation
   (UE5_PluginDev for VoxelWorldSave).

**Tests (headless).**
- `VoxelWorlds.Map.EditOverlay.DigLowersSurface`: a carved column reports the lower surface and the exposed material.
- `VoxelWorlds.Map.EditOverlay.BuildRaisesSurface`: an added column reports the new top and the brush material, also
  when the build crosses into the chunk above.
- `VoxelWorlds.Map.EditOverlay.BuriedEditIsIgnored`: a cavity under an intact surface produces no mask bits.
- `VoxelWorlds.Map.EditOverlay.DugThroughStaysPending`: a scan that falls through the stack reports the column pending.
- `VoxelWorlds.Map.EditOverlay.WaterBitFromVoxels`: a dry pit below the water line stays land; a flooded one is water.
- `VoxelWorlds.Map.EditOverlay.EncodeDecodeRoundTrip`
- `VoxelWorlds.Map.Tiles.RequeueWhileCachedRedraws` and `VoxelWorlds.Map.Tiles.PerTileGenerationDiscardsStale`, with
  the queue policy factored into a static helper so it runs without a world.

**Verification.**
1. Back up the world save, start PIE in VoxelDemo, open the map and capture it (`EditorAppToolset.CaptureEditorImage`;
   `pie_screenshot` does not capture UMG).
2. Dig a crater (`vox.EditMode 1`, or `vox.Dig 20` at the crosshair) and build a three-layer pillar. Within about a
   second the map shows the crater darker and lower-shaded, and the pillar in its brush material.
3. Visit a campsite and a dungeon entrance (`vox.Campsite`, `vox.TPPOI Tiled`): the pad and shaft edits show.
4. Stop and restart PIE: the overlays are on the map before the area loads.
5. Undo an edit: the tile returns to its previous look.
6. Restore the save, comparing `savedAtUtc` first.

**Risks.**
- The voxel surface and the analytic surface differ slightly (height parity is exact, but conditioning and
  interpolation are not). Overlay columns could show a faint outline against analytic neighbours. If they do, compute
  every column of an edited tile from voxels while its chunks are loaded.
- Water-propagation churn is handled by coalescing; the overlay changes only when a water bit flips.
- Larger saves: only edited tiles with surface changes are stored, typically a few KB each.

### R22 as built, phases 1-2 (VoxelWorlds#73, 2026-10-12)

**Scope.** The overlay kernel, the map queue fixes and the live refresh. Persistence (phase 3) follows in a second
PR pair; until then overlays are rebuilt each session as the edited chunks load. Line numbers below are as of
VoxelWorlds `a85b8c6`.

**What the code does.**
- `FVoxelMapEditOverlay` (VoxelMap, plain struct): sorted masked column indices with a height, material and flags
  byte per column, plus `PendingColumns` as build output only. `Serialize` writes a version byte, the resolution, a
  Resolution² bit mask, then the values of the masked columns.
- `FVoxelMapEditOverlay::Build` takes the tile's chunk stack: per chunk Z, a shared edit-merged snapshot (null if
  not loaded) and the highest edited local Z per column, derived from the edit layer's keys. For each edited column
  it assembles the voxel column and calls `FVoxelSurfaceQuery::ExtractSurfaceFromColumn`.
  - The scan runs from one chunk above the highest edited (or analytic surface) chunk down to one chunk below the
    lowest. It stops above the first chunk the column edits but cannot read. Unlisted chunks read as the analytic
    terrain (a linear density across the analytic height, so the crossing lands on it).
  - A column is masked when an edited voxel lies at or above the surface voxel and the surface differs from the
    analytic one: height by more than half a voxel, material, or water. This replaced the plan's "compare against
    analytic" alone: edits under the surface (dungeon interiors) never enter the comparison, so a natural voxel
    surface that differs slightly from the analytic one cannot leak into the overlay.
  - A surface found in an analytic chunk under real air, below the analytic height, is pending (dug into a chunk
    that is not loaded), as is a scan that falls through. `MergePending` keeps the previous value there.
  - Water: a water-flagged voxel anywhere in the air run above the surface, not only the voxel directly on it. A
    pit dug into the seabed opens unflagged voxels under the sea, and drew as a dry patch in the sea with the
    one-voxel rule. A pit dug on land below the water line still has only dry air above it and stays land.
- `FVoxelMapTileQueue` (VoxelMap) holds the queue policy: waiting tiles, drawing tiles with the generation they
  started under, per-tile generations, priority waiting. A request is independent of the tile cache, so a cached
  tile can be redrawn (the old drain skipped cached keys, stranding the tile). An invalidating request during a
  drawing bumps that tile's generation, the stale result is discarded on completion and the tile is drawn again
  (the old completion dropped the re-queue). A non-invalidating request during a drawing is satisfied by it.
- `UVoxelChunkManager::GetEditMergedSnapshot` wraps the seam snapshot cache (built at most once per
  `ContentVersion`); `HasChunkVoxelData` is the cheap availability check.
- `UVoxelMapSubsystem`:
  - binds `UVoxelEditManager::OnChunkEdited`, keeps the edited chunk Zs per tile, and seeds them from the edit
    layers when it resolves (edits made before the map existed);
  - coalesces dirty tiles on a one-shot timer (`voxel.Map.EditRefreshSeconds`, 0.5). A tile whose edited chunks
    at or above one chunk below its lowest analytic surface are all loaded is queued for a rebuild, priority and
    invalidating, at most `voxel.Map.EditRefreshMaxTiles` (8) per refresh; the others keep their overlay and
    retry when a chunk of theirs generates;
  - gathers the stack when the drawing starts, at most one gather per frame (a skipped rebuild retries next
    tick), and the tile task builds the overlay after its analytic height pass, then draws edited columns
    (including the apron cells from the eight neighbours' overlays) with their own height, material and water;
  - redraws cached neighbours when an overlay's border columns change, so hillshade stays continuous;
  - no longer empties the cache on a conditioning change: every explored tile is redrawn and the old images
    stay until the new ones land.
- Debug: `voxel.Map.EditOverlay` (0 draws the analytic map only), `voxel.Map.Redraw [1 = rebuild overlays]`,
  `voxel.Map.Stats`, `voxel.Map.Capture [radius] [name] [highlight]` (cached tiles around the pawn to
  `Saved/Screenshots/VoxelMap/<name>.png`, north up, overlay columns tinted magenta with highlight 1).

**Measured** (VoxelDemo, the user's world save copied into the worktree, real RHI).
- Crater and pillar on land beside the pawn (x −470,285, y −295,534; brushes through
  `UVoxelEditManager::ApplyBrushEdit`): both tiles rebuilt at the 0.5 s refresh and drawn by 0.9 s. The crater
  draws as a shaded stone bowl, the three-layer snow pillar as a white block with a lit and a shaded face. A
  scripted pit in the seabed reads as water (after the water rule above). `vox.Dig 20` (the gameplay dig path)
  showed at the next refresh. `Images/R22/crater_pillar_before_after.png`.
- Undo of the pillar: the region is pixel-identical to the before image at the next refresh; the crater stays.
- `vox.TPPOI Tiled`: the dungeon re-stamped on approach (209k voxels); its rooms produce no overlay and the
  entrance shaft shows as a 30-column hole on the conditioning pad. `Images/R22/dungeon_shaft.png`.
- `vox.Campsite`: the saved edits around it (a dungeon entrance, earlier QA digs) rebuild as their chunks load,
  with no visible outline against analytic neighbours. `Images/R22/campsite_overlays_off_on.png`.
- Gather cost on the game thread: 0.03-0.6 ms per tile when the snapshots are shared with the seam pipeline,
  up to 2.3 ms on a dungeon tile with large edit layers; hence the one-gather-per-frame budget. Nine overlays with
  553 masked columns used 4.4 KB.
- Edits far from the player (38 tiles in this save) stay pending without any chunk work until visited.

**Tests.** New: `VoxelWorlds.Map.EditOverlay.{DigLowersSurface, BuildRaisesSurface, BuriedEditIsIgnored,
DugThroughStaysPending, WaterBitFromVoxels, EncodeDecodeRoundTrip, BorderChangesDetected}`,
`VoxelWorlds.Map.Tiles.{RequeueWhileCachedRedraws, PerTileGenerationDiscardsStale}`. Headless (`-nullrhi`): Map,
Streaming, LOD, Rendering, DualContour, MC SeamClosure, 65/65. In-editor (real RHI): the same plus MC CPUvsGPU,
Cubic and Generation (GPU noise and GPU-vs-CPU parity), 95/95.

**Known limits.**
- Until phase 3 (below), overlays are not saved: after a restart an edited area draws analytically until its
  chunks load.
- A pit scripted into the seabed was not flooded by water propagation within a minute; not investigated (the map
  reads it as water through the sea above it either way).
- Tiles with very large edit layers (dungeon stamps) cost up to ~2.3 ms to gather; an incremental per-chunk
  top-edit table would remove the walk if it ever shows in a profile.

### R22 as built, phase 3 (VoxelWorlds#74, 2026-10-12)

**What the code does.**
- `UVoxelMapSubsystem::ExportEditOverlays` / `ImportEditOverlays` (VoxelMap), with the pure
  `EncodeEditOverlays` / `DecodeEditOverlays`. The blob: magic `VMEO`, a format version, the tile count, then per
  tile its X, Y and `FVoxelMapEditOverlay::Serialize` data (which has its own version byte); zlib-compressed behind
  its uncompressed size, base64-encoded. Malformed data restores nothing and logs a warning.
- The world document (UE5_PluginDev, VoxelWorldSave) gains `MapEditOverlays` and `MapEditOverlayCount`.
  `ApplyDocument` imports the overlays first, then the explored tiles (so their first drawings include the
  overlays), and `LoadWorld` loads the edits after that, as planned.
- Pending-on-load reconciliation: an import replaces the overlays and marks every touched tile for the next
  coalesced refresh, which runs after the edits are in. A tile whose edited chunks are loaded is rebuilt from them,
  a tile with no edits left is cleared (a save whose edits sidecar is missing cannot leave stale overlays), and any
  other tile keeps the saved overlay until one of its chunks generates.
- Saves from before R22 carry no overlays: their edited areas update as their chunks load (user decision).
- `vox.SaveInfo` and the save log report the overlay count; `voxel.Map.Capture` takes an optional centre X Y.

**Measured** (VoxelDemo, worktree copy of the user's save).
- First load of the pre-R22 save: "Restored 0 map edit overlay(s)"; the overlays rebuilt as their chunks loaded.
- Nine overlays (553 columns) after visiting the crater site and the campsite: 2,796 base64 characters in the
  document (the explored-tile blob next to it, 3,719 tiles, is 10,920).
- PIE restart with the pawn at the campsite, about 690 km from the crater, whose chunks were confirmed unloaded
  (`HasChunkVoxelData` false for Z −2..0): "Restored 9 map edit overlay(s)" at load, and the map around the crater
  is pixel-identical to the capture taken while the area was loaded (157 overlay columns).
  `Images/R22/restored_before_area_loads.png`.
- Mid-session `vox.Load`: the same 9 overlays restore, the loaded tiles rebuild one per frame (0.03-1.1 ms), and
  the crater area is again identical.

**Tests.** New: `VoxelWorlds.Map.EditOverlay.SaveBlobRoundTrip`; `VoxelWorldSave.Document.RoundTrip` covers the new
fields. Headless: Map, VoxelWorldSave, Streaming, LOD, Rendering, DualContour, MC SeamClosure, 67/67. In-editor: the
same plus MC CPUvsGPU, Cubic and Generation (GPU noise, GPU-vs-CPU parity), 97/97.

**Known limits.**
- Only the server saves; a client's map rebuilds its overlays as chunks load (as before phase 3).
- Overlays of areas never revisited keep their saved look; edits made by other means while the world is not loaded
  (none exist today) would not show until the area loads.

## R24 Seam pop-in

**Report (W-04, streaming at speed):** "the only note is related to seam pop-in. If we can get this to blend or be
better hidden. The streaming itself is very robust."

**What the code does.**
- Interiors crossfade only on a mesh-to-mesh swap.
  - `bCrossfade = bChunkExisted && CanCrossfade()` (`VoxelRendering/Private/VoxelWorldComponent.cpp:343, 363`).
  - The proxy keeps one previous generation (`RetireOrReleaseChunk_AssumesLocked`, `VoxelSceneProxy.cpp:826-876`).
  - It draws the previous mesh with a fade-out MID and the current with a fade-in MID, using one alpha and the
    `FadeInvert` complementary dither (`VoxelSceneProxy.cpp:429-483`).
  - Alpha advances on the game thread (`AdvanceChunkCrossfades`, `VoxelWorldComponent.cpp:798-886`; 0.35 s).
  - A new chunk (none → mesh) pops, and so does a removal.
- Seams have no fade path at all.
  - `UpdateSeamMeshFromCPU` carries no crossfade flag (`IVoxelMeshRenderer.h:219-263`).
  - The seam draw always uses the shared material (`VoxelSceneProxy.cpp:536-644`).
  - A rebuild replaces the bucket inside one render command, so it pops.
- **Ordering.**
  - A seam is built only after all of its participants' interiors have been submitted (`DispatchSeamJob` drops jobs
    while any `MeshedLODLevel < 0`, `VoxelChunkManager.cpp:3714-3721`), and lands at least one tick later.
  - Measured near ready → visible is p50 35 ms, p95 68 ms; frontier seams also wait for the neighbours to load.
  - So a new chunk appears with open boundaries, then its seams pop in.
- **LOD changes.**
  - The new interior is submitted and starts crossfading. `UpdateChunkRenderedLOD` dirties all 26 incident seams
    (`VoxelSeamRegistry.cpp:488-501`).
  - Until each rebuilt seam lands, the proxy shows the new interior (fading in), the old interior (fading out) and
    the old seam strips built for the old LOD.
  - If a seam lands after the 0.35 s fade, the result is a plain crack or overlap until it does. At 3,000 uu/s the
    LOD rings move about 0.6 chunks a second, so this happens continuously around the player.
- **A stale-seam hazard.** A seam re-dirtied while its previous job is still in flight is dropped and left clean
  (`VoxelChunkManager.cpp:3691-3698`). The stale strip then stays until some later change re-dirties it. This
  contradicts the "newer result wins" comment in `VoxelSeamRegistry.h:339-341`.
- **Batch cap.** `MaxMeshBatchesPerFrame = 2000` (`VoxelSceneProxy.cpp:337-342`) covers chunks first, then seams, then
  water, and a fading chunk counts twice. Seams are therefore the first casualties when the cap is hit.

**Design (decision: crossfade plus coordinated transitions).**
- **Seam crossfade.**
  - The proxy gets `SeamPreviousMeshes` and `SeamFadeStates` keyed by `FVoxelSeamMeshKey`, mirroring the chunk maps
    in their own keyspace (sharing the chunk key collides, as the P1 renderer notes found).
  - `UpdateSeamMeshFromCPU` gains `bCrossfade`, set when the owner's bucket already exists.
  - The component reuses the fade MID pools and advances seam fades in the same tick.
  - RVT clones keep the shared opaque material, as for chunks.
- **Coordinated LOD transitions.**
  - When a chunk's interior swap comes from an LOD change, its crossfade starts with alpha held at 0: the old mesh
    stays fully visible and the new one is uploaded but dithered out.
  - The manager records the seam keys that change dirtied. As their results land (or are dropped because a
    participant left), the set empties and the hold releases. `voxel.MeshFade.SeamHoldMs` (default 150) caps the wait.
  - Rebuilt seams start their own crossfade when they land, so old and new seams blend over the same window as the
    interior.
  - Edits do not hold: a dig must show immediately and fades as today.
- **First appearance.**
  - A fade state may exist without a previous mesh: the current mesh draws with the fade-in MID until alpha reaches 1.
  - Used for new chunks and new seam buckets beyond `voxel.MeshFade.AppearMinDistance` (default 7,000, the LOD 0
    band edge), so nothing near the player turns briefly see-through.
  - Optionally the same hold applies: a new interior waits (capped) for its ready seams before fading in.
- **Prerequisite fix.** Instead of dropping a job whose seam is still in flight, mark the seam to re-dirty when the
  in-flight result completes, so the newest content always gets built.
- **Batch budget.** Draw seams before far fading chunks, or reserve part of the cap for seams. Show the existing
  `SkippedOverLimit` count on the Performance HUD so it is visible when the cap binds. Cap concurrent fades (for
  example 512) and fall back to an instant swap beyond it, because the MID pools are unbounded today.

**Alternatives considered.**
- *Seam swap crossfade only:* cheaper (M), but leaves the new-interior-beside-old-seam window on every LOD change,
  which is the main pop at W-04 speed.
- *Skirts under interior rims:* hide late seams without fading, but poke through in caves and overhangs, and hang
  below seams that have landed.
- *Hold the new interior until every incident seam lands, with no fade:* removes cracks but still pops, and frontier
  seams can wait seconds for neighbours.

**Phases.**
- **P0 Measure and fix (S).** Seam rebuild reason counters (first build, LOD change, edit, neighbour content) and a
  "crack window" metric (time an interior is visible while an incident seam is stale or missing) in the `[SeamLat]`
  log. Fix the in-flight re-dirty, add the proxy's `SkippedOverLimit` count to the HUD, and record a W-04 run with
  `voxel.MeshFade.Duration 2` as the before reference.
- **P1 Seam crossfade (M).** Proxy, component and renderer interface changes above.
- **P2 Coordinated LOD transitions (M).** Hold set in the manager, `SetChunkFadeHold` on the renderer, and the
  timeout cvar.
- **P3 First-appearance fade (S-M).** Fade state without a previous mesh, distance gate, optional hold for new chunks.
- **P4 Qualification.** Bench A/B with `voxel.Bench.Run` (MeshFade off vs on), a W-04 soak, and in-editor suites.

**Tests.**
- Factor the game-thread fade scheduling into a plain `FVoxelCrossfadeScheduler` in VoxelRendering (which has no
  tests today):
  - `VoxelWorlds.Rendering.Crossfade.SwapStartsFade`
  - `.HoldFreezesAlphaUntilRelease`
  - `.HoldTimesOut`
  - `.ResubmitDuringFadeRestarts`
  - `.AppearFadeRespectsDistanceGate`
  - `.ConcurrentFadeCapFallsBackToSwap`
- `VoxelWorlds.Streaming.SeamRegistry.InFlightRedirtyRebuilds`: a seam re-dirtied while in flight is scheduled
  again after the in-flight result completes.
- `VoxelWorlds.Streaming.SeamRegistry.LODChangeHoldSet`: an LOD change reports exactly the incident seam keys, and the
  set empties as they complete or a participant leaves.

**Verification (in-editor, real RHI).**
- W-04 at 3,000 uu/s with `voxel.MeshFade.Duration 2` (slow motion): no hard-edged strip pops at the LOD rings; the
  crack-window p95 for LOD changes drops below one frame.
- Normal duration: the user re-runs W-04.
- `voxel.Bench.Run` frame P95 within about 1 ms of fade-off; the HUD batch-skip count stays at 0.
- DualContour seam suites and `VoxelWorlds.Streaming.SeamRegistry.*` green in-editor.

**Risks.**
- Dithering mismatched old and new geometry is noisy, though less so than a pop. It is the trade already accepted for
  interiors.
- More batches during transitions (fading seams count twice): watch the cap.
- Holding interiors delays detail by up to the hold cap. Near seams land in about 35 ms, so the cap mostly matters
  at the frontier.
- A material-relevance regression like the water bug of #45: the proxy must keep OR-ing the fade material's
  relevance once, never calling `SetPrimitiveViewRelevance` per material.

### R24 as built (VoxelWorlds#71, 2026-10-12)

**Scope.** P1 (seam crossfade) and P2 (coordinated LOD transitions) in one PR, with the in-flight re-dirty fix and
the concurrent-fade cap. One addition the measurements forced: deferred seam bucket submits (below). Not done: P3
(first-appearance fade), the batch budget and HUD skip count, and P0's rebuild-reason counters and crack-window
metric. The `[SeamLat]` `lodHold` and `bucketDefer` fields and the renderer's crossfade counters stand in for P0.

**What the code does.**
- `FVoxelCrossfadeScheduler` (VoxelRendering) owns fade timing for interiors and seam buckets in one keyspace, so a
  release can start any mix of them on the same clock. The proxy keeps `SeamPreviousMeshes` and `SeamFadeStates`
  beside the chunk maps. A bucket emptied by a rebuild fades out instead of vanishing.
- On an LOD change of a visible chunk (`ShouldHoldForSeams`), the new interior is submitted held at alpha 0.
  `FVoxelSeamTransitionHolds` waits for the chunk's buildable incident seams. Each result holds its owner's
  bucket, and transitions sharing a chunk, seam or bucket merge into one group. A group releases when its seams
  have landed and its buckets have been submitted, or at `voxel.MeshFade.SeamHoldMs`.
- A seam result for a bucket that is still fading is stored, and the merged bucket is submitted when that fade
  ends (`DeferBucket` / `FlushDueSeamBuckets`). Replacing it mid-fade dropped the generation it was fading out
  of, so the other slots' transitions snapped to their end state.
- A seam re-dirtied while its job is in flight stays scheduled until `NotifySeamJobFinished`, then rebuilds.
- Defaults: `voxel.MeshFade.Seams 1`, `voxel.MeshFade.SeamHoldMs 1000` (was planned at 150), and
  `voxel.MeshFade.MaxConcurrent 512` (beyond it swaps are instant). The renderer's failsafe for a hold nobody
  releases is 3 s.

**Measured** (VoxelDemo; survey camera at x −470,286, z 2,500, looking north, moved y −300,332 → −287,332 →
−284,332; the last step sweeps 35-37 chunks through LOD 2 → 1 → 0; `voxel.MeshFade.Duration 2` for slow motion).
- Before (seam fade and hold off): a dark opening along a chunk boundary for about a second (burst frames 07-10).
  With `voxel.MeshFade 0` no opening was caught at all. The interior crossfade itself exposes the old seam ring.
- Hold cap 150 ms: about half the groups timed out. During a sweep, the transitions merge into large groups that
  need 0.2-0.6 s for all their seams, so the opening stayed.
- Cap 2 s, no deferral: no timeouts, but a fainter dithered remnant. A third of seam bucket fades (904 of 2,703)
  were restarts of a fade still running.
- After (defaults): no opening in any frame, at Duration 2 or 0.35. At 0.35 s: 0 timeouts, holds 0.2-0.63 s, and
  0 mid-fade restarts out of 2,156 seam fades (interiors: 0). At Duration 2 a few groups reach the 1 s cap, because
  a deferred bucket waits up to the fade duration. Raise `SeamHoldMs` along with the fade duration.
- Images: `Images/R24/seam_gap_before_after.jpg` (same frames before and after), `Images/R24/before_context.jpg`.

**Tests.** Headless (`-nullrhi`): Streaming, LOD, Rendering, DualContour, MC SeamClosure, 53/53. In-editor (real
RHI), the same plus MarchingCubes, Cubic and the Generation GPU-vs-CPU parity tests, 92/92. New tests:
`Rendering.Crossfade.*` (5), `Streaming.SeamRegistry.LODChangeHold{Set,TimesOut,GroupsMerge,DeferredBucket}`,
`Streaming.SeamRegistry.InFlightRedirtyRebuilds`.

**Known limits.**
- An interior resubmitted while its own fade runs still restarts (one retained generation). Measured 0 times.
- New chunks and new buckets at the frontier still pop (P3, done below).
- `voxel.Bench.Run` A/B and a W-04 run at normal duration were not done. The user's W-04 re-run is the check.

### R24 P3 as built (first-appearance fade, VoxelWorlds#72, 2026-10-12)

**Scope.** P3 as designed (a fade without a previous mesh, the distance gate, the optional hold), plus the
`voxel.Bench.Run` A/B of P4. Not done: the batch budget and HUD skip count (the proxy's over-limit count stayed
0 in every run below, so nothing forced it), P0's counters, and water tiles, which still pop at the frontier.

**What the code does.**
- A fade can have no previous mesh. `FVoxelCrossfadeScheduler::StartAppear` starts one only when the
  geometry's chunk cell is at least `voxel.MeshFade.AppearMinDistance` (default 7,000, the LOD 0 band edge)
  from the streaming viewer, measured to the nearest point of the cell. A seam bucket is measured by its
  owner's cell, which it lies on. Nearer geometry appears at once, so nothing near the player turns
  see-through.
- `UVoxelWorldComponent` tries an appearance for a first interior submit and for a new seam bucket. The
  proxy draws the current mesh with the fade-in MID alone (`FVoxelChunkFadeState::bAppear`, no fade-out MID).
  A resubmit during an appearance replaces the mesh in place on the same alpha; retaining the half-shown
  mesh as a Previous would snap it back to solid. A bucket removed while it appears goes at once.
- The chunk manager sends its viewer to the renderer every tick (`IVoxelMeshRenderer::SetViewerPosition`).
  `ShouldHoldForSeams` also holds a first appearance that the renderer will fade (`WillFadeInChunk`,
  `voxel.MeshFade.AppearHold`): the new interior stays invisible until its ready seams land, then fades in
  with them, in the same `FVoxelSeamTransitionHolds` groups and under the same `SeamHoldMs` cap as an LOD
  change. Seams whose other participants have no mesh yet are abandoned at dispatch, so a frontier chunk
  waits only for neighbours that are already on screen.
- New buckets joining a held transition already faded in from nothing (P2). They are now appearances too,
  which fixes a P2 case: a second slot landing for a held new bucket retained the never-shown first version
  as the Previous, which then popped in at the release.
- The deferral rule is unchanged: `GetSeamFadeRemainingSeconds` counts a running appearance, so a new bucket
  is not replaced mid-fade either.
- Cvars: `voxel.MeshFade.Appear` 1, `voxel.MeshFade.AppearMinDistance` 7000, `voxel.MeshFade.AppearHold` 1.
  The renderer stats (chunk manager debug dump) count appearance fades.

**Measured** (VoxelDemo, real RHI).
- Arrival: the survey camera parked 300 km away, then moved to x −470,286, y −300,332, z 2,500 looking +Y,
  with `voxel.MeshFade.Duration 2`; same frames with `voxel.MeshFade.Appear` 0 and 1. Off: far chunks pop in
  solid, one floating piece at a time. On: the same chunks dither in on the same frames, and a held chunk
  stays hidden until its seams land. The settled frames are pixel-identical, so no chunk is left hidden.
  `Images/R24/appear_before_after.jpg`.
- PIE start (initial load): 357 interior and 372 bucket appearances, 341 hold groups, 0 timeouts, longest
  hold 521 ms. Whole session: 0 mid-fade restarts, 0 over-limit frames, 0 failsafe releases.
- Bench: `voxel.Bench.Run p3 3000 40000` heading +X from the land site (pawn at the lake surface), one fresh
  PIE per run, alternating `voxel.MeshFade 0` and the defaults. Unfocused editor with the background throttle
  off; the PIE viewport is 3440×1408, hence the ~25 ms median.

  | Run | `voxel.MeshFade` | Frame P50 ms | Frame P95 ms | Frame P99 ms | Manager P95 ms | Catch-up s | Over-limit frames |
  |-----|------------------|--------------|--------------|--------------|----------------|------------|-------------------|
  | off A | 0 | 25.7 | 46.0 | 58.3 | 20.6 | 3.0 | 0 |
  | off B | 0 | 25.4 | 43.2 | 51.8 | 19.4 | 2.8 | 0 |
  | on A | 1 | 29.0 | 46.1 | 54.3 | 18.8 | 3.4 | 0 |
  | on B | 1 | 25.1 | 43.2 | 55.9 | 19.9 | 2.5 | 0 |
  | on C | 1 | 26.1 | 45.6 | 58.6 | 20.9 | 2.9 | 0 |

  Mean frame P95: 44.6 ms off, 45.0 ms on (+0.4 ms, inside the ±1.5 ms spread between runs). A first
  fades-off run with the background throttle still on (P95 48.8 ms) is left out. Holds during the traverse
  with fades on average 90-200 ms per log interval, the longest about 1 s, with 4 cap timeouts in about 750
  groups. With fades off (LOD holds only) they average 40-75 ms with none. The longer holds come from the
  deferral rule: a held group waits for buckets that are still appearing.
- Why no 40 km frontier shot: around the demo spawn the frontier is mostly open sea, and water tiles do not
  take this fade. The arrival exercises the same path in view. A survey camera also has to stay below
  z 4,800: InfinitePlane loads chunk layers from the viewer's layer −2 to +8, so a camera in layer 1 or
  higher drops the lowest terrain layer and shows only a sliver of terrain.

**Tests.** New: `VoxelWorlds.Rendering.Crossfade.AppearFadeRespectsDistanceGate`. Headless (`-nullrhi`):
Streaming, LOD, Rendering, DualContour, MC SeamClosure, 54/54. In-editor (real RHI), the same plus
MarchingCubes, Cubic and the Generation GPU tests (GPU noise and GPU-vs-CPU parity), 93/93.

**Known limits.**
- Inside the gate a new chunk still pops by design, for example around the player after a teleport, and its
  boundary shows open until its seams land.
- Water tiles still pop at the frontier.
- A hold that reaches the `SeamHoldMs` cap (about 0.5% of groups in the bench) fades its interiors in
  before their seams, as with an LOD change.
- A fade MID copies every terrain material parameter when it is acquired, and appearances acquire more of
  them. The bench shows no cost; if one appears, track a parameter version per pooled MID.

## Order and pull requests

1. **R23:** one VoxelWorlds PR plus the parent bump.
2. **R22:** VoxelWorlds (map, snapshot accessor) and UE5_PluginDev (VoxelWorldSave), phases 1-2 in one PR pair,
   phase 3 in a second.
3. **R24:** P0 first (it may change the P2 hold cap), then P1-P3, one VoxelWorlds PR per phase.

## Follow-ups found on the way (not in scope)

- `UVoxelSurfaceNavigationSubsystem::HandleChunkEdited` invalidates by edit centre and radius, so undo, redo, clear
  and load (zero centre) never invalidate the nav grid. Single-voxel `ApplyEdit` also carries a stale centre and
  radius from the last brush.
- `FChunkEditLayer::GetMergedVoxel` ignores Add/Subtract deltas; any caller gets the wrong voxel for brush edits.
- When a non-owner participant unloads, the owner keeps that seam's strip until the neighbour returns. It is at the
  loaded edge and was not seen, but it is the same "geometry with no live participant" class as R23a.
- The seam bucket comments in `VoxelSceneProxy.h` still describe per-axis buckets; the manager merges all seven slots
  into one bucket per owner.
