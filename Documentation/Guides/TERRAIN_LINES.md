# Terrain lines: a road, a cutting or a trail from a spline

How to draw a line in a level and have the voxel terrain carry it: a levelled road, a cutting or a river bed, an
embankment, a painted trail. The line is a **conditioning line** (`Features/WORLD_MODES.md`, "Conditioning lines", "The
surface under a line" and "The hand-placed line"): it changes what the world generates, on every machine, in every
world mode (plane, island, planet, a block world). Nothing is drawn in the editor: the terrain shows the line in PIE.

## Placing one

1. From the Place Actors panel drag a **Voxel Terrain Line** (`AVoxelTerrainLineActor`) into the level. It is a spline
   with a `UVoxelTerrainLineComponent`. (Or add the component to any actor that has a spline component; `SplineName`
   picks the spline when the actor has several.)
2. Move the spline's points to where the line should run, and add points (Alt-drag a point) for its curves. Only where
   the spline runs over the ground is read, not its height, unless you choose `Spline Height` below: on a plane or an
   island its X and Y, on a planet the direction of each point from the planet centre. So the spline may hang over the
   terrain.
3. Set the component's values (the `TerrainLine` component's Details, "Terrain Line").
4. Play. The line is in the terrain from the world's start, on a dedicated server and on each client alike.

A line changes only chunks generated after it is registered, and it registers before the world's first chunk. Moving
the spline or changing a value shows on the next start of the world. A saved world keeps its edits; the generated
terrain under them follows the level's lines as they are when the world starts.

## The values

| Value | Default | What it does |
|---|---|---|
| Height Mode | Follow Terrain | `Follow Terrain`: the line follows the natural ground along it. `Spline Height`: the line takes the spline's own height (its Z; on a planet its altitude over the datum) |
| Terrain Offset | 0 | Follow Terrain: added to the target, in world units. Under zero for a cutting or a bed, over it for a causeway |
| Smoothing Distance | 1200 | Follow Terrain: the ground's height is averaged this far each way along the line, so the line does not follow every bump. It narrows toward each end, where the line meets the ground |
| Max Grade | 0.15 | Follow Terrain: the line's steepest slope along itself (0.15 is 15 %; 0 is no limit) |
| Half Width | 225 | Half the width of the fully blended core, in world units (225 is 3 voxels of 75 uu: a road 6 voxels wide). Each spline point's **scale Y** multiplies it there: scale a point up and the line widens around it |
| Falloff Width | 300 | The width outside the core over which the terrain eases back to the natural ground |
| Strength | 1 | 1 reaches the target on the core; 0 leaves the height alone (a line that only paints and clears) |
| Blend | Level | `Level`: cut and fill toward the target (a road). `Lower Only`: only ground above the target moves (a cutting, a river bed). `Raise Only`: only ground below it (an embankment) |
| Surface: Band Material ID | -1 | The material painted along the line: an index into the world's material atlas; -1 paints nothing |
| Surface: Band Half Width | 0 | Half the band's width; 0 is the core's width. The band is never wider than the core |
| Surface: Band Depth | 4 | How many voxels deep the band's material goes. Keep 4 or more (the distant meshes read the material under the surface) |
| Surface: Min Cave Depth | 0 | No cave opens within this many voxels under the line (8 keeps a road whole over a cave system) |
| Surface: Clear Half Width | 0 | No scatter, PCG point or voxel tree within this distance of the line. It may be wider than the core (a verge) |
| Vertex Spacing Voxels | 2 | A vertex of the line every this many voxels along the spline |
| Order | 0 | Where lines meet, the higher order is applied later and wins on its core |

## Three lines to start from

The review maps `/Game/PluginTesting/Tests/Review/Review_TerrainLines_Plane` and `Review_TerrainLines_Planet` hold one
of each (`Scripts/setup_terrain_line_review.py` at the project root builds them; the material ids are the demo
atlas's):

| Line | Values |
|---|---|
| A road | Level, strength 1, half width 3 voxels, falloff 4 voxels, Follow Terrain with a max grade of 0.12; band material 5 (gravel), min cave depth 8, clear half width 5 voxels |
| A cutting or a dry bed | Lower Only, half width 4 voxels, falloff 6 voxels, Follow Terrain with a terrain offset of -300; band material 2 (stone), min cave depth 8 |
| A painted trail | Strength 0, half width 1.5 voxels, smoothing 0; band material 4, clear half width 2.5 voxels. The ground is not moved |

Widths are in world units on the component; think of them in voxels (the world configuration's `VoxelSize`), since the
terrain is sampled per voxel: a band 6 voxels wide stays unbroken at every level of detail, one 3 voxels wide breaks
into dashes past the second LOD band (`WORLD_MODES.md`, "The surface under a line").

## What the log tells you

When the world starts, each line logs one line (`LogVoxelStreaming`: "Terrain line <actor>: N vertices over ..."), and
a warning for each limit it breaks. The line is added whatever they say; the warnings say where it will look wrong:

| Warning | What to change |
|---|---|
| "break the bend bound" | The line turns too sharply for its slope: on the inside of the bend the levelled ground steps. Widen the curve (a radius of at least 5 m for a 6-voxel road on up to a 10 % grade, 13 m at 25 %), lower Max Grade, or flatten the spline through the bend |
| "turn so sharply ... that their bisector meets a neighbour's inside the reach" | A corner. Round it to a radius of at least the half width plus the falloff |
| "within twice the reach of each other" | The line comes back on itself or crosses itself (a hairpin, a closed loop's join). Keep its parts farther apart, or use two lines |
| "the cut and fill cap" | The target is more than half a chunk from the natural ground: chunks there may not stream. Lower the offset, or let the line follow the terrain more closely (a smaller smoothing distance, a higher max grade) |
| "under two voxels" | The core or the falloff is too narrow to mesh cleanly |
| "steeper than one block a column" | A block world: the line climbs more than a block per column; lower Max Grade |

`GetLineWarnings` on the component returns the same sentences to a Blueprint. `voxel.World.Stats` reports the lines
in the world, and whether every conditioning source registered before the first chunk was requested.

## What a line does not do

- No editor preview, and no change to chunks that have already generated: a line spawned while the world runs is
  added with a warning, and only chunks generated later carry it.
- No bridge, tunnel or overhang: a line moves the height of the ground, it does not carve volumes.
- Across water it levels the bed, not the surface.
- Where a line meets a POI's pad, the pad wins the height; the band is painted wherever the line runs.
