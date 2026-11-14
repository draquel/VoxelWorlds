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

A spline that curves gently makes the line it draws. One that turns sharply, a corner above all, does not: the
component rounds it ("Bends", below), and the log says how far the line then runs from the spline.

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

## Bends

A levelled line cannot turn sharply. On the inside of a bend the ground is levelled to the nearest stretch of the line,
which changes across the bend's bisector, and on a slope the two stretches ask for different heights there: the
terrain steps, a crease up the inside of the bend. The limit is the **bend bound** (`WORLD_MODES.md`, "Conditioning
lines"). For a road 6 voxels wide with a falloff of 4 voxels (a reach, the half width plus the falloff, of 7 voxels:
525 uu) and a vertex every 2 voxels it allows 16 degrees a vertex, a curve of the reach's radius, on a grade of up to
10 %, and less on a steeper one: 11 degrees at 15 %, 6.7 at 25 %.

A spline within the bound makes the line it draws, vertex for vertex. Where a spline breaks it, the component makes the
line again when the world starts (D134):

- **It rounds the corners.** They are spread along the line, one vertex further each way at a time, until every vertex
  is within the bound, and no further. The line's two ends stay where the spline's are. A 60 degree corner of that road
  is spread over 3 m each way and the line then runs within 0.75 m of the spline; a right angle over 7.4 m, within
  2.2 m of it. The spreading stops at twice the reach each way (10.5 m for that road), which is enough for a corner of
  up to about 100 degrees and leaves the line within 3.1 m of the spline, six tenths of the reach.
- **Follow Terrain: it makes the profile again** on the rounded line, with the same smoothing and Max Grade, and holds
  the grade through each bend to what the turn left there allows. A tight bend on a slope is levelled out, with more
  cut and fill, where it would have creased. A line whose turns are within the bound, and which only climbs too
  steeply through them, keeps its vertices and gets the flatter profile alone.
- **Spline Height: the heights stay the spline's,** read along the rounded line. The profile is yours, so the turn
  itself has to fit the grade and the corner is spread further: a 60 degree corner of that road over 6 m each way on
  a 15 % grade and 8.7 m on 20 %, where the same corner following the terrain needs 3. A right angle fits on up to
  about 10 %; on 15 % or more, as any corner on a steep grade, it does not fit within twice the reach.
- **What is out of reach keeps its warning.** A corner of much over 100 degrees, a hairpin, a corner at the spline's
  height on a steep grade: the line takes the rounding that came nearest the bound (none, when none helped) and the
  log's warning names what is left. Draw such a bend as a curve.

When the component has made a line again its first warning says so: "the spline breaks the bend bound (its sharpest bend
turns 4.1 times what the bound allows there): the line's corners are rounded over 296 uu each way along it, so the line
runs up to 75 uu from its spline, and its profile is flatter through the bends" (a 60 degree corner on a hillside, the
road held to 20 %: `Documentation/TerrainLines/L5/` at the project root has it before and after). For the line to run
exactly where the spline does, draw the bend within the bound: curve points with tangents, or enough points, to a
radius of the reach on a gentle grade (5 m for that road), 13 m at 25 %.

## What the log tells you

When the world starts, each line logs one line (`LogVoxelStreaming`: "Terrain line <actor>: N vertices over ..."), and
a warning for what the component did about its bends and for each limit the line as made breaks. The line is added
whatever they say; the warnings say where it is not what you drew, and where it will look wrong:

| Warning | What to change |
|---|---|
| "the spline breaks the bend bound ...: the line's corners are rounded over N uu each way along it, so the line runs up to M uu from its spline" | Nothing, if the rounded line is the one you want: the terrain has no crease. For the line to follow the spline, draw the bend within the bound ("Bends") |
| "... the line runs where the spline does, and its profile is flatter through the bends" | Nothing: the line climbs less through its bends than Max Grade would let it |
| "... the line still breaks the bound" | The rounding stopped short (it spreads a corner over no more than twice the reach). The next two warnings say what is left |
| "vertices break the bend bound" | The line still turns too sharply for its slope: on the inside of the bend the levelled ground steps. At the spline's height, lower the grade through the bend or widen the curve (a radius of at least 5 m for a 6-voxel road on up to a 10 % grade, 13 m at 25 %) |
| "turn so sharply ... that their bisector meets a neighbour's inside the reach" | A corner too sharp to round (much over 100 degrees). Draw it as a curve with a radius of at least the half width plus the falloff |
| "within twice the reach of each other" | The line comes back on itself or crosses itself (a hairpin, a closed loop's join). Keep its parts farther apart, or use two lines |
| "the cut and fill cap" | The target is more than half a chunk from the natural ground: chunks there may not stream. Lower the offset, or let the line follow the terrain more closely (a smaller smoothing distance, a higher max grade) |
| "under two voxels" | The core or the falloff is too narrow to mesh cleanly |
| "steeper than one block a column" | A block world: the line climbs more than a block per column; lower Max Grade |

`GetLineWarnings` on the component returns the same sentences to a Blueprint, and `GetLineBends` (C++) the numbers:
how far over the bound the spline was, how far the corners were spread and how far the line runs from the spline.
`voxel.World.Stats` reports the lines in the world, and whether every conditioning source registered before the first
chunk was requested.

## What a line does not do

- No editor preview (so a rounded corner shows in PIE and in the log, not in the editor), and no change to chunks
  that have already generated: a line spawned while the world runs is added with a warning, and only chunks generated
  later carry it.
- No bridge, tunnel or overhang: a line moves the height of the ground, it does not carve volumes.
- Across water it levels the bed, not the surface.
- Where a line meets a POI's pad, the pad wins the height; the band is painted wherever the line runs.
