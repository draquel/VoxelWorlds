# VoxelWorlds

> State as of 2026-11-02 (code review CR5).

A voxel terrain engine for Unreal Engine 5.8, in C++ with Blueprint access. It generates terrain on the GPU (with a
CPU twin held to it by parity tests), meshes it on CPU workers, streams it around the viewer with LOD, collision and
compression, and draws it through one renderer on UE's `FLocalVertexFactory`, with an analytic far field to the horizon.
Worlds are an infinite plane, a bounded island or a spherical planet up to about 1,000 km in radius, in smooth or
block (cubic) terrain. The plugin depends on no other project plugin.

## Features

- **Three meshers**: cubic (greedy), Marching Cubes and Dual Contouring (QEF), all on CPU workers.
- **Seam ownership**: each chunk boundary is meshed by one seam job that reads both sides, so seams are watertight
  across LOD levels and a chunk never re-meshes for a neighbour.
- **World modes** behind `IVoxelWorldMode`: infinite plane, island with a sloping seabed and far ocean, spherical
  planet with gravity-frame queries, orbit and back.
- **Streaming**: a distance-band voxel window per column (heightmap) or per chunk (planet), async generation, meshing
  and Chaos collision cooking, far-chunk compression, a voxel cut-over above three view distances.
- **Far field**: cube-face or root-tile quadtrees of the analytic surface past the voxel window, in every mode.
- **Rendering**: one scene proxy per render region (16³ chunks), double-precision region origins, mesh-swap crossfades,
  water tiles.
- **Generation**: hashed split-origin noise (`FVoxelNoise`), biomes with dithered blending, caves, ore veins, water,
  conditioning zones for POI pads, voxel trees.
- **Editing**: a sparse edit layer with brushes, undo and redo, water that floods digs, binary persistence.
- **Materials**: a material registry and atlas (saved texture arrays for smooth triplanar, a packed atlas for cubic)
  behind one master material.
- **Scatter**: foliage and props extracted from the voxels, placed per biome, drawn as HISMs per region.
- **Map and navigation**: map and minimap tiles with a globe for planets; A* over the surface without a navmesh.
- **Runtime PCG**: a voxel surface sampler and a biome dispatcher for PCG graphs.

## Documentation

- [Documentation/ARCHITECTURE.md](Documentation/ARCHITECTURE.md): modules, data flow, design decisions. Start here.
- [Documentation/README.md](Documentation/README.md): the index of every Features doc, guide and research note.
- [Documentation/Guides/QUICK_START.md](Documentation/Guides/QUICK_START.md): build, place a world, run the tests.
- [Documentation/Guides/TESTING_STRATEGY.md](Documentation/Guides/TESTING_STRATEGY.md): the test suites and the
  in-editor gate.
- `Documentation/GLOSSARY.md` and `Documentation/EDITOR_CONSOLE_GUIDE.md` at the project root: the vocabulary, and every
  `voxel.*` command and cvar.
- [.claude/instructions.md](.claude/instructions.md): the rules for changing the plugin.
