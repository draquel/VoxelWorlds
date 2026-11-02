# GPU Compute Pipeline

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelGeneration
**Dependencies**: VoxelCore

## Overview

The GPU pipeline generates each chunk's voxel data (density, biome and material, caves and ores, then the water and
underground post-passes) in one render graph and reads the finished voxels back. Only generation runs on the GPU. The
chunk meshers run on CPU workers (`MARCHING_CUBES_MESHING.md`, `DUAL_CONTOURING.md`); the GPU Dual Contouring and
Marching Cubes meshers, never dispatched by a world, were deleted in code review CR1, and scatter extraction reads the
voxels on the CPU since G27.

Every world mode generates on the GPU when it is allowed: `UVoxelWorldConfiguration::bUseGPUGeneration` (default true),
the `voxel.Stream.GPUGeneration` cvar (0 forces CPU without a restart) and the `-VoxelForceCPU` switch (headless and
`-nullrhi` runs, where compute cannot dispatch). The CPU generator (`FVoxelCPUNoiseGenerator`) computes the same voxels
and is the reference the parity tests compare against.

## Module Configuration

Modules that use global shaders (`IMPLEMENT_GLOBAL_SHADER`) must load at `PostConfigInit` in the `.uplugin`, so the
shader system is initialised before the module loads. VoxelGeneration, VoxelMeshing and VoxelRendering do.

```json
{
    "Name": "VoxelGeneration",
    "Type": "Runtime",
    "LoadingPhase": "PostConfigInit"
}
```

A later loading phase crashes the editor at startup with access violations in `CreatePackage` or
`ProcessNewlyLoadedUObjects`.

### Header practice

To avoid static initialisation problems in GPU modules:

1. Forward-declare RDG types such as `FRDGPooledBuffer` in headers rather than including the render graph headers.
2. Include `RenderGraphBuilder.h`, `RenderGraphResources.h` and the like in `.cpp` files (or private headers such as
   `VoxelGenerationShaders.h`).
3. Use `RHIFwd.h` for RHI forward declarations in headers.

## Shaders

| File | Contents |
|------|----------|
| `Shaders/Private/GenerateVoxelDensity.usf` | `MainCS` (`FGenerateVoxelDensityCS`), 4x4x4 threads, one thread per voxel |
| `Shaders/Private/VoxelPostPasses.usf` | the water and underground post-passes (below) |
| `WorldModeSDF.ush`, `IslandFalloff.ush` | the heightmap terrain and the island's edge falloff |
| `SphericalPlanet.ush`, `PlanetFrame.ush` | the planet branch, in the chunk's frame |
| `VoxelNoise.ush`, `VoxelNoiseCommon.ush` | the hashed split-origin noise (`NOISE_LIBRARY.md`) |
| `CaveGeneration.ush` | `GpuCaveCarve`, one carve for every world mode (`CAVE_SYSTEM.md`) |
| `BiomeMaterial.ush` | biome selection, the dithered blend, height rules, ores (`BIOME_SYSTEM.md`, `ORE_VEINS.md`) |
| `Shaders/Shared/VoxelGenerationShared.ush`, `VoxelHash.ush` | the enum integers, flag bits, strides and tables (C++ includes the same file), and the lattice hash (mirrors `VoxelHash.h`) |

The shader classes are declared in `Source/VoxelGeneration/Private/VoxelGenerationShaders.h` and implemented in
`VoxelGenerationShaders.cpp`.

## Pipeline Stages

### Stage 1: Density, biome and material

`MainCS` generates at the base voxel size whatever the chunk's LOD (the LOD stride is applied by the meshers, as on
the CPU). Per voxel, in the CPU generator's order:

1. **Terrain.** On a heightmap world (InfinitePlane, IslandBowl, cubic on either) the terrain noise at the chunk's
   noise origin plus the voxel's offset (`NoiseOrigins`, MP4), continentalness, the island falloff, the conditioning
   zones, then the signed distance to the surface. On a planet the radial height along the voxel's direction
   (`SphericalPlanet.ush`).
2. **Biome blend** from temperature, moisture and continentalness, before the caves (the caves read the leading
   biome's cave overrides).
3. **Caves**: `GpuCaveCarve` with the biome's overrides and the sea's minimum depth.
4. **Material**: the blended biome material (dithered in blend zones, G39), underwater materials, height rules, the
   cave-wall override and ore veins.
5. Pack density, material, biome and flags into one `uint` per voxel.

### Stage 2: Post-passes

The water fill and the underground classification run as compute passes in the same render graph
(`AddVoxelPostPassDispatches` in `VoxelGPUNoiseGenerator.cpp`), so the readback returns finished voxel data and no
thread does per-voxel work after it. Heightmap modes scan world-Z columns (`WaterColumnSeedCS`, `WaterFloodSweepCS`,
`UndergroundColumnCS`, `UndergroundMarkCS`, `UndergroundApplyCS`); a planet scans digital lines along each chunk's local
down (`WaterLineSeedCS`, `UndergroundLinesCS`, one thread per line, on the lines `FVoxelDownLines` computes for the CPU
passes) and floods inside the sea sphere (`WaterFloodSweepRadialCS`).

### Stage 3: Readback

The streaming path never stalls a thread. `FVoxelGPUNoiseGenerator::BeginGenerateChunkGPU` enqueues the graph and an
`FRHIGPUBufferReadback` copy in one render command and returns a handle; the chunk manager calls
`PollGenerateChunkGPU` once a frame until it reports `Ready` (or `Failed`). A landed result is scanned for a uniform
value (all air, all rock, open sea) so the chunk can be stored collapsed (G67). `GenerateChunkAsync` with the blocking
`ReadbackToCPU` remains for tests and editor tools.

## Parity tests

Generation on the GPU must equal the CPU's within float rounding. The `VoxelWorlds.Generation.GPUvsCPU*Parity` tests
(`Material`, `BlendDither`, `IslandBowl`, `Caves`, `NoiseType`, `Water`, `OreVariant`, `Spherical`) and
`Generation.GPUNoiseGeneratorAsync` compare whole chunks and log a CRC per case. They need a real RHI: under
`-nullrhi` they skip themselves, so run them in the editor (the project `CLAUDE.md`, "Running correctness tests").

The shader carries the current rules only. VoxelWorlds 8 kept two dead legs behind parameters pinned to 1, the classic
Simplex kernel (before G43) and the old blend pick (before G39), because removing them changes DXC's code generation;
G97 removed them (2026-11-02), and G88 the noise's amplitude parameter (`NoiseFBM3D`'s octave weights start at 1). The
GPU's output moved by rounding in 24 of the 111 parity lines, the record since then
(`Documentation/CodeReview/VW8/gpu_parity_lines_2026-11-02_generation.txt`): G97 alone moved all 24 (the InfinitePlane
case, the caves under the sea and six planet cases, each at three LODs), and G88 the InfinitePlane case's three again.
Every test still passes and no line has a voxel whose density differs from the CPU's by more than 2 steps; the moved
lines' density disagreements went from 2-50 voxels of 32,768 to 3-48, their material disagreements from 0-7 to 0-9.
The CPU did not change (the headless suite's mesh digests and the explored maps are byte-identical to before).

## Performance

A 32^3 chunk dispatches 8x8x8 groups of 64 threads (one thread per voxel); a 64^3 chunk 16x16x16. The shaders keep
their branches and loops un-inlined (`[branch]`, `[loop]`, one cellular search for Cellular and Voronoi) so FXC
compiles them for SM5 (DX11) in seconds rather than hours (F2.13, `VISUAL_POLISH_PLAN.md`).
