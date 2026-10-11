# Surface Navigation (VoxelNavigation module)

Pathfinding over the voxel terrain without an engine navmesh. Built for surface NPCs (game feature 10), usable by
anything that needs "how do I walk from A to B over the terrain" — on loaded chunks and beyond them.

## Why not Recast

A runtime navmesh would have to re-rasterise the tri-mesh chunk collision inside the collision radius (200 m in the
demo) every time a chunk cooks, knows nothing about terrain that has no collision yet, and voxel chunks / scatter
already opt out of navigation. The terrain, however, is a function: the generated surface height exists
everywhere (`UVoxelChunkManager::GetGeneratedSurfaceHeight`), and the edit-merged surface exists wherever a chunk is
loaded (`QueryEditMergedSurface`). A 2.5-D grid over those two queries gives a cheap, deterministic pathfinder.

## Pieces

| Piece | File | Role |
|-------|------|------|
| `FVoxelSurfaceNavParams` | `Public/VoxelSurfacePathfinder.h` | Cell size (150 = two demo voxels), `MaxStepRise` (100: ~34° per cell, a wide margin under a capsule's walkable angle because voxel terrain is locally steeper than the cell average), water level + `MaxWadeDepth`, slope cost, `MaxNodes`, `MaxSearchRadius`, goal snapping, partial routes. |
| `FVoxelSurfacePathfinder` | `Public/VoxelSurfacePathfinder.h` | Pure A* over cells. Height comes from a `TFunction` sampler, blocked cells from an optional query: unit-testable without a world. 8-connected, no corner cutting, octile heuristic, slope-weighted cost, string-pulled output, nearest-walkable goal snap, partial route to the closest cell when the budget runs out. |
| `UVoxelSurfaceNavigationSubsystem` | `Public/VoxelSurfaceNavigationSubsystem.h` | World subsystem (Game / PIE). Finds the chunk manager, caches heights per cell in 32 × 32 tiles (edit-merged when loaded, analytic otherwise; analytic cells re-sample when their chunk loads, edited cells drop on `OnChunkEdited`), blocked boxes + temporary blocked cells, cvars, stats, debug drawing. |

### Walkability rule

A step between neighbouring cells is allowed when both cells are above `WaterLevel − MaxWadeDepth`, neither is
blocked, and `|Δheight| ≤ MaxStepRise` (× √2 on diagonals). Diagonals additionally need both orthogonal neighbours
steppable. Cost = run × (1 + SlopeCostScale × rise / run). Surface only: caves, overhangs and bridges do not exist
for the pathfinder (one height per column).

### Subsystem API

- `FindPath(From, To, OutPoints)` / `FindPathWithStats(...)`: synchronous, budgeted (`voxel.Nav.MaxNodes`, 4096).
  Spread requests over frames (an NPC repaths a few times a second at most).
- `ProjectToSurface`, `IsWalkableAt`, `AddBlockedBox` / `RemoveBlockedBox`, `BlockCellsTemporarily` (a stuck walker
  marks where it failed for a few seconds), `InvalidateAll` / `InvalidateBox`.
- `SetDebugFocus` + `voxel.Nav.Debug 1` draws the grid (green = loaded surface, yellow = analytic, red = water,
  magenta = blocked); `DrawPath` draws a route for N seconds.

### CVars

`voxel.Nav.CellSize` (150), `voxel.Nav.MaxNodes` (4096), `voxel.Nav.MaxStepRise` (100), `voxel.Nav.MaxWadeDepth`
(60), `voxel.Nav.MaxRadius` (20000), `voxel.Nav.Debug` (0).

### Costs

Cached cells make a 150 m search a few thousand `TMap` lookups (well under a millisecond). First-touch sampling of a
32 × 32 tile is ~1024 height queries; the analytic query is noise evaluation (microseconds each), the edit-merged
query is a column scan of loaded voxel data. Tiles stay cached until invalidated; the cache is dropped when the cell
size cvar changes.

### Consumers

VoxelCharacterPlugin's `UVCVoxelPathProvider` (an `IVCPathProvider`) wraps the subsystem; `AVCNPCAIController` creates
one on possess when no provider was set, so surface creatures path over the voxels while dungeon enemies keep their
grid provider. The surface spawner uses `IsWalkableAt` to reject spawn points.

### Tests

`VoxelWorlds.Navigation.Flat`, `CliffDetour`, `Water`, `BudgetAndBlocks` (`Tests/VoxelNavigationTests.cpp`): pure
pathfinder over lambda heightfields.

### Related streaming fix

`voxel.Collision.ResweepSeconds` (VoxelStreaming, default 1 s): the collision decision sweep only considers loaded
chunks and used to run only when the pawn moved, so a pawn that stood still right after a teleport (save restore,
`vox.TP`) kept collision only under its own feet — creatures could not spawn and anything walking there would fall
through. The sweep now also re-runs periodically while stationary.

### Follow-ups

Async searches on a worker (tile sampling must stay on the game thread for the edit-merged query), hierarchical
search for routes beyond `MaxSearchRadius`, jump / drop links, swimming.
