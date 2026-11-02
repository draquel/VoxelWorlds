# Marching Cubes Meshing System

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelMeshing

Marching Cubes runs on the CPU only (`FVoxelCPUMarchingCubesMesher`, on the chunk manager's worker tasks). The GPU
Marching Cubes mesher that mirrored it was never dispatched by a world and was deleted in code review CR1 with its
shaders (D-CR12).

## Table of Contents

1. [Overview](#overview)
2. [Smooth Terrain Pipeline](#smooth-terrain-pipeline)
3. [Marching Cubes Algorithm](#marching-cubes-algorithm)
4. [LOD Support](#lod-support)
5. [LOD Seams: Transvoxel Ribbon and Geomorph](#lod-seams-transvoxel-ribbon-and-geomorph)
6. [Configuration](#configuration)
7. [Implementation Details](#implementation-details)
8. [Performance Considerations](#performance-considerations)
9. [Troubleshooting](#troubleshooting)
10. [Marching Cubes on a Planet](#marching-cubes-on-a-planet)

---

## Overview

The Marching Cubes meshing system generates organic, curved terrain surfaces using the **Marching Cubes** algorithm, as an alternative to the blocky cubic meshing style. The system includes:

- **Marching Cubes**: Core algorithm for isosurface extraction from density fields
- **LOD Support**: Strided sampling for level-of-detail rendering
- **Seams and LOD seams**: a chunk meshes its interior; face, edge and corner seam jobs mesh the boundary bands. Where
  a face borders a coarser chunk the face seam job emits a **Transvoxel ribbon** (zero-thickness transition cells on the
  boundary plane) and **geomorphs** the fine band toward the coarse contour ([LOD Seams](#lod-seams-transvoxel-ribbon-and-geomorph));
  history: [GEOMORPH_IMPLEMENTATION_PLAN.md](../Research/Archive/GEOMORPH_IMPLEMENTATION_PLAN.md),
  [TRANSVOXEL_IMPLEMENTATION_PLAN.md](../Research/Archive/TRANSVOXEL_IMPLEMENTATION_PLAN.md).

### When to Use Marching Cubes Meshing

- Organic terrain (hills, caves, overhangs)
- Sculpted/deformed terrain
- Planetary surfaces
- Any terrain where smooth contours are desired

> **See also**: [Dual Contouring](DUAL_CONTOURING.md) for the alternative smooth meshing algorithm that preserves sharp features via QEF vertex placement.

### Key Files

| File | Purpose |
|------|---------|
| `VoxelCPUMarchingCubesMesher.h/cpp` | CPU implementation |
| `VoxelCPUMarchingCubesSeams.cpp` | CPU seam jobs (face, edge and corner seams between chunks) |
| `VoxelCrossingGradient.h` (Private) | Voxel-resolution crossings and fitted shading normals, shared with Dual Contouring |
| `MarchingCubesTables.h/cpp` | Lookup tables for standard Marching Cubes |
| `TransvoxelTables.h/cpp` | Lookup tables for LOD transitions |
| `VoxelMeshingTypes.h` | Configuration structures |

---

## Smooth Terrain Pipeline

Marching Cubes and [Dual Contouring](DUAL_CONTOURING.md) are both **smooth meshing algorithms** — they produce organic, curved surfaces from the voxel density field. They share a common rendering pipeline that differs from the cubic meshing path:

### Shared Infrastructure

| Component | Smooth (MC + DC) | Cubic |
|-----------|------------------|-------|
| Rendering mode | Triplanar texturing | UV-atlas per face |
| Vertex format | `FVoxelVertex` (28 bytes) | Same format, different UV usage |
| Material parameter | `bSmoothTerrain = 1` in shader | `bSmoothTerrain = 0` |
| Normal source | Density gradient fitted to the voxels around the vertex (G62) | Face normals |
| UV computation | Triplanar projection (dominant axis) | Per-face atlas lookup |

### Algorithm and rendering style

The algorithm (MarchingCubes, DualContouring or Cubic) is selected by `EMeshingMode` in the world configuration; the
renderer derives the rendering style from it (triplanar for the two smooth modes, atlas for cubic). `FVoxelMeshingConfig`
carries no mode flag of its own (its `bUseSmoothMeshing` was read by nothing and was deleted in CR1, MESH-06).

### Shared Mesh Types

Both smooth meshers implement the `IVoxelMesher` interface and produce the same output types:
- **Input**: `FVoxelMeshingRequest` (voxel data, LOD, cell domain; neighbour slices or snapshot views for seam jobs and the collision cook)
- **Output**: `FChunkMeshData` (positions, normals, UVs, colors, indices)
- **Config**: `FVoxelMeshingConfig` (shared fields + algorithm-specific fields)

Both close chunk boundaries by seam ownership: the interior mesh (`EVoxelMeshCellDomain::Interior`) carries no
neighbour data and meshes only the cells that need none; seam jobs mesh the rest. The scheduling is in
[STREAMING_AND_SEAMS.md](STREAMING_AND_SEAMS.md).

**MC seam jobs** (`VoxelCPUMarchingCubesSeams.cpp`) mesh each participant's boundary band through the same
`ProcessCubeLOD`, ribbon and geomorph code as a chunk, so junctions with the interior meshes are bit-identical. Per
chunk the interior mesh covers cells `[1, N-2]` per axis; a face seam the cells with one axis at `{0, N-1}`, an edge seam
two, a corner seam three. Each participant frame reads the participants' snapshots in place
(`FVoxelMeshingRequest::NeighbourSnapshotViews`): at every read, the voxel a copied face plane, edge strip or corner
would hold. Copying them was 95% of a seam job's time; reading in place made MC seam jobs about 30 times cheaper (G63).
`voxel.Seam.MCStats` times the stages.

### LOD Transition Differences

The two smooth meshers handle LOD boundaries differently:

| | Marching Cubes | Dual Contouring |
|-|---------------|-----------------|
| Mechanism | Transvoxel ribbon + band-confined geomorph, in the face seam job | Mixed-LOD seam meshers fan fine crossings onto the coarse rings |
| Lookup tables | 512-entry tables (56 classes) | None |
| Request fields | `TransitionFaces`, `NeighborLODLevels`, `MorphWidthOverride` | `NeighborLODLevels` |

---

## Marching Cubes Algorithm

### Concept

Marching Cubes processes voxel data in 2x2x2 cubes. For each cube:

1. Sample density at 8 corners
2. Determine which corners are "inside" the surface (density >= threshold)
3. Build an 8-bit index from the corner states (256 possible configurations)
4. Look up which edges are intersected by the isosurface
5. Interpolate vertex positions along those edges
6. Generate triangles from the lookup table

### Cube Layout

```
        7-------6
       /|      /|
      4-------5 |
      | |     | |
      | 3-----|-2
      |/      |/
      0-------1

Corner 0: (0,0,0)  Corner 4: (0,0,1)
Corner 1: (1,0,0)  Corner 5: (1,0,1)
Corner 2: (1,1,0)  Corner 6: (1,1,1)
Corner 3: (0,1,0)  Corner 7: (0,1,1)
```

### Edge Numbering

```
Edge 0:  0-1  (bottom front)
Edge 1:  1-2  (bottom right)
Edge 2:  2-3  (bottom back)
Edge 3:  3-0  (bottom left)
Edge 4:  4-5  (top front)
Edge 5:  5-6  (top right)
Edge 6:  6-7  (top back)
Edge 7:  7-4  (top left)
Edge 8:  0-4  (left front vertical)
Edge 9:  1-5  (right front vertical)
Edge 10: 2-6  (right back vertical)
Edge 11: 3-7  (left back vertical)
```

### Density Field

The `FVoxelData.Density` field stores values from 0-255:
- **0**: Fully outside (air)
- **127**: Surface threshold (`VOXEL_SURFACE_THRESHOLD`)
- **255**: Fully inside (solid)

For Marching Cubes meshing, this is converted to a 0.0-1.0 float range, with an `IsoLevel` threshold (default 0.5) determining the surface.

### Edge Interpolation

When an edge crosses the isosurface, the exact vertex position is calculated:

```cpp
float t = (IsoLevel - density0) / (density1 - density0);
t = FMath::Clamp(t, 0.0f, 1.0f);
FVector3f Position = pos0 + (pos1 - pos0) * t;
```

### Normal Calculation

Normals are computed using the gradient of the density field (central differences):

```cpp
float gx = GetDensity(x+1, y, z) - GetDensity(x-1, y, z);
float gy = GetDensity(x, y+1, z) - GetDensity(x, y-1, z);
float gz = GetDensity(x, y, z+1) - GetDensity(x, y, z-1);
FVector3f Normal = FVector3f(-gx, -gy, -gz).GetSafeNormal();
```

That is the central difference before G62 (`CalculateGradientNormal`, the vertex floored to a voxel). LOD 0's vertex normal
is `CalculateShadingNormal`: the density gradient fitted to the voxels on the ramp around the vertex
(`VoxelCrossingGradient::FitNormal`: weighted least squares over the 3x3x3 block around its nearest voxel, 0 < density <
255, tent weights, a free offset, so a geomorphed vertex off the surface is fitted too), over the voxels the request holds
(`IsVoxelHeld`: the chunk, or one voxel past it where that face's plane, edge's strip or corner is present). The central
difference read the saturated rock under the surface and tilted a slope's normal by 1.0-1.8 times with the surface's phase
in the voxel layer (bands; G62). Where the voxels on the ramp do not span three directions it is used, exact there.
Tests `Meshing.CrossingGradient.Planes`, `Streaming.{Planet,Plane}.GradientBands`.

**Normals at a stride (G61).** Generated density saturates a voxel either side of the surface
(`SignedDistanceToDensity`), so taps `Stride` voxels apart read 0 or 255 on every axis the surface crosses between them:
at LOD 2 each gradient component is a sign and the normal snaps toward one of the 26 cube directions (a bright ring on
planets, streaks on heightmap slopes). So at stride > 1 an edge vertex's normal is LOD 0's (`CalculateShadingNormal`) at
the voxel crossing on its edge nearest it (`CalculateEdgeShadingNormal`: the edge walked from its min end in voxel steps). It is a function of the
edge alone, so every cell, seam band and transition ribbon sharing the edge gives the vertex the same normal, and the
boundary geomorph (which moves vertices) does not change it. Where the edge has no voxel crossing the stride-scale
gradient (`CalculateGradientNormalLOD`) stands in. Tests `Meshing.MarchingCubes.LODNormals`,
`Streaming.{Planet,Plane}.LODNormalsMC`.

**Vertices at a stride (G80).** The same saturation put an interpolated strided vertex toward the edge's middle, whatever
the surface did between the two samples, so LOD 1 and 2 were terraced a stride apart (ribs in the terrain's own shadows
under a low sun). At stride > 1 an edge vertex is the voxel crossing on its edge (`FindEdgeVoxelCrossing`, the point
G61's normal reads), so regular cells, seam bands and the transition ribbon (its coarse corner-corner edges are the coarser
neighbour's, its face-face ones the fine mesh's; one-voxel edges keep `InterpolateEdge`) still agree on every shared edge.
The boundary geomorph's target is the coarse cells' vertices: the voxel crossings on the four coarse edge columns along
the morph axis around the vertex, blended bilinearly (`ComputeCoarseCrossingsAlongAxis`; on a column, that column's
coarse vertex; before G80 the trilinear coarse density's crossing). Tests `Meshing.LODCrossings.Planes`,
`Streaming.{Planet,Plane}.LODPositions`.

---

## LOD Support

### Stride-Based LOD

Higher LOD levels sample fewer voxels:

| LOD Level | Stride | Cubes per 32^3 Chunk |
|-----------|--------|---------------------|
| 0 | 1 | 31^3 = 29,791 |
| 1 | 2 | 15^3 = 3,375 |
| 2 | 4 | 7^3 = 343 |
| 3 | 8 | 3^3 = 27 |

### LOD-Aware Sampling

At higher LOD levels, each cube covers a larger region:

```cpp
const int32 Stride = 1 << LODLevel;  // 2^LODLevel

// Sample corner positions
for (int i = 0; i < 8; i++) {
    int32 CornerX = X + CornerOffsets[i].X * Stride;
    int32 CornerY = Y + CornerOffsets[i].Y * Stride;
    int32 CornerZ = Z + CornerOffsets[i].Z * Stride;
    Density[i] = GetDensityAt(CornerX, CornerY, CornerZ);
}
```

### The LOD Seam Problem

When adjacent chunks are at different LOD levels, their boundary vertices don't align:

```
High-LOD Chunk    |    Low-LOD Chunk
                  |
    *---*---*     |     *-------*
    |   |   |     |     |       |
    *---*---*     |     |       |
    |   |   |     |     |       |
    *---*---*     |     *-------*
```

The high-LOD chunk has 3 vertices on the boundary, but the low-LOD chunk only has 2. This creates visible gaps/seams.

---

## LOD Seams: Transvoxel Ribbon and Geomorph

### Overview

The Transvoxel algorithm (Eric Lengyel, 2010) solves LOD seams by using special **transition cells** at chunk boundaries. These cells have:

- **9 sample points** on the high-resolution side (3x3 grid)
- Geometry that connects to the **4 corner points** the low-resolution neighbor produces

### Transition Cell Layout

```
High-res side (3x3 grid):

    6---7---8
    |   |   |
    3---4---5
    |   |   |
    0---1---2

Low-res corners: 0, 2, 6, 8 (the 4 corners)
```

### How It Works

1. **Detect Transition Faces**: the face seam job sets `TransitionFaces` (and `NeighborLODLevels`) in the finer
   participant's frame when the other participant is coarser; chunk meshes never set it
2. **Sample 9 Points**: Instead of 8 corners, sample 9 points on the transition face
3. **Build Case Index**: 2^9 = 512 possible configurations
4. **Generate Transition Geometry**: `ProcessTransitionCell` uses the lookup tables to emit a zero-thickness ribbon on the
   boundary plane matching both LOD levels

The same job **geomorphs** the fine band toward the coarse contour (along the axis nearest local up, steep slopes
skipped) with a band-confined ramp (`MorphWidthOverride`) that reaches zero at the band's inner edge, so morphed band
vertices stay coincident with the unmorphed interior mesh; whole-chunk meshing uses `voxel.Mesh.MCBoundaryMorphWidth`
(2 coarse cells, also `UVoxelWorldsSettings::MCBoundaryMorphWidth`). Ribbon cells that would need voxels beyond the
two face participants (the positive-perpendicular perimeter rows) skip themselves (`HasRequiredNeighborData`), leaving
small boundary-plane gaps along those face edges at mixed LOD that the geomorph makes less visible. `bUseTransvoxel` is
on unless the world configuration's `bEnableLODSeams` is off. Tests `Meshing.MarchingCubes.LODBoundary.*`,
`Meshing.MarchingCubes.SeamClosure.MCS3_MixedFaceParity`.

### Implementation

**In VoxelMeshingRequest:**
```cpp
// Set by the MC face seam job when the other participant is coarser
uint8 TransitionFaces = 0;

// Flags for each face
static constexpr uint8 TRANSITION_XNEG = 1 << 0;
static constexpr uint8 TRANSITION_XPOS = 1 << 1;
static constexpr uint8 TRANSITION_YNEG = 1 << 2;
static constexpr uint8 TRANSITION_YPOS = 1 << 3;
static constexpr uint8 TRANSITION_ZNEG = 1 << 4;
static constexpr uint8 TRANSITION_ZPOS = 1 << 5;
```

**In MarchingCubes Mesher:**
```cpp
for each cell:
    ProcessCubeLOD(X, Y, Z);  // Standard Marching Cubes over the interior domain
// the transition ribbon along a coarser neighbour is emitted by the seam jobs
// (ProcessTransitionCell), not by the chunk mesh
```

### Transition Cell Tables

**TransitionCellClass[512]**: Maps 512 configurations to 56 equivalence classes

**TransitionCellData[56]**: Vertex and triangle counts per class

**TransitionVertexData[56][12]**: Vertex generation instructions

**TransitionCellTriangles[56][36]**: Triangle indices

---

## Configuration

### FVoxelMeshingConfig

```cpp
// Isosurface threshold (0.0-1.0, default 0.5 = density 127)
float IsoLevel = 0.5f;

// Transvoxel ribbon at LOD seams (the chunk manager clears it when the world's bEnableLODSeams is off)
bool bUseTransvoxel = true;
```

The skirt fallback (`bGenerateSkirts`, `SkirtDepth`) was unreachable from every production path since seam
ownership and was deleted in CR1 (MESH-05).

### Recommended Settings

```cpp
Config.IsoLevel = 0.5f;
Config.bUseTransvoxel = true;
```

---

## Implementation Details

### Neighbor Data Handling

An interior mesh reads only its own voxels. A seam job's participant frame reads its neighbours through
`NeighbourSnapshotViews` (G63); a whole-chunk request (the collision cook) carries copied face planes (`NeighborXPos` ..., `NeighborPlaneDepth` deep), edge strips
(`EdgeXPosYPos` ...) and corner voxels.

### Material Assignment

For Marching Cubes meshes at LOD 0, material comes from the majority of solid corners:

```cpp
uint8 GetDominantMaterial(int X, int Y, int Z, uint8 CubeIndex) {
    // Count materials from solid corners
    TMap<uint8, int32> MaterialCounts;
    for (int i = 0; i < 8; i++) {
        if (CubeIndex & (1 << i)) {  // Corner is solid
            MaterialCounts[GetVoxel(corner[i]).MaterialID]++;
        }
    }
    // Return most common material
    return MaterialCounts.FindMaxKey();
}
```

### LOD-Aware Material Selection (LOD > 0)

At higher LOD levels, the standard majority-vote approach causes material pop-in because strided sampling misses thin surface layers. The surface material (e.g., grass) may only be 1-2 voxels thick, while the strided cube samples deeper underground materials (e.g., dirt).

**Solution**: Upward surface scanning finds the actual surface transition:

```cpp
uint8 GetDominantMaterialLOD(const FVoxelMeshingRequest& Request,
                              int32 X, int32 Y, int32 Z,
                              int32 Stride, uint8 CubeIndex) const
{
    uint8 SurfaceMaterial = 0;
    double Highest = TNumericLimits<double>::Lowest();

    // For each solid corner in the cube
    for (int32 i = 0; i < 8; i++) {
        if (CubeIndex & (1 << i)) {
            const FIntVector Corner = FIntVector(X, Y, Z) + CornerOffsets[i] * Stride;

            // Walk up from the corner along the chunk's local up (a voxel column on a heightmap world, the digital
            // line along up on a planet) to the last solid voxel before air: the surface, at most 8 steps away.
            uint8 Value = 0;
            const double Along = ScanSurfaceAlongLocalUp(Request, Corner, /*bBiome=*/false, Value);

            // Use the material from the corner whose surface lies highest along up
            // (prefers grass on slopes over dirt underground)
            if (Along > Highest) {
                Highest = Along;
                SurfaceMaterial = Value;
            }
        }
    }
    return SurfaceMaterial;
}
```

**Why This Works**:
- Even when LOD stride causes corners to sample below surface, scanning upward finds the actual surface
- Using highest surface Z ensures grass (on top) is preferred over dirt (on slopes where one corner touches surface)
- The scan stops after 8 voxels, which limits its cost while covering typical terrain variations
- Same approach applied to `GetDominantBiomeLOD()` for consistent biome selection
- At LOD 0 majority voting is used; cubic meshing uses face-based material assignment

---

## Performance Considerations

### Triangle Counts

Marching Cubes meshing typically produces more triangles than cubic:

| Chunk Type | Cubic (Greedy) | MarchingCubes |
|------------|----------------|-------------|
| Flat surface | ~2 tris | ~200 tris |
| Hilly terrain | ~500 tris | ~2000 tris |
| Complex caves | ~2000 tris | ~8000 tris |

### Optimization Strategies

1. **LOD**: Use aggressive LOD to reduce distant triangle counts
2. **Early-Out**: Skip empty cubes (EdgeTable[cubeIndex] == 0)
3. **Pre-allocated Buffers**: Reserve based on estimates

The fitted shading normals (G62) cost chunk-mesh worker time, about half as much again; seam jobs read their
participants in place (G63).

### Memory Usage

A chunk's voxels are 4 bytes each (1 MB at 64^3); meshing reads them from the chunk's snapshot.

---

## Troubleshooting

### Visible Seams at LOD Boundaries

**Symptom**: Gaps or cracks between chunks at different LOD levels

**Solutions**:
1. Check that the world configuration's `bEnableLODSeams` is on (it keeps `bUseTransvoxel`)
2. Check that the face seam jobs ran and were not left stale ([STREAMING_AND_SEAMS.md](STREAMING_AND_SEAMS.md))
3. Small gaps along a mixed-LOD face's positive-perpendicular edges are the ribbon's known limit (above)

### Incorrect Surface Position

**Symptom**: Surface appears at wrong density level

**Solution**: Adjust `Config.IsoLevel` (0.5 = density 127)

### Inverted Normals

**Symptom**: Lighting looks inside-out

**Solution**: Check triangle winding order in lookup tables

### Performance Issues

**Symptoms**: Slow chunk generation, frame drops

**Solutions**:
1. Increase LOD aggressiveness
2. Time the seam stages with `voxel.Seam.MCStats`
3. Profile with Unreal Insights

### Missing Geometry at Chunk Edges

**Symptom**: Holes at chunk boundaries

**Solution**: The interior mesh leaves the boundary bands to the seam jobs by design; run the seam suite
(`Meshing.MarchingCubes.SeamClosure.*`) and check the chunk's seam jobs ran.

---

## References

- [Dual Contouring (companion doc)](DUAL_CONTOURING.md) — DC algorithm, QEF solver, mixed-LOD seams
- [Marching Cubes Original Paper](http://www.cs.carleton.edu/cs_comps/0405/shape/marching_cubes.html)
- [Transvoxel Algorithm](https://transvoxel.org/) - Eric Lengyel
- [Polygonising a Scalar Field](http://paulbourke.net/geometry/polygonise/) - Paul Bourke
- [GPU Gems 3: Chapter 1](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu)

---

## Marching Cubes on a Planet

Meshing and seam requests carry `LocalUp`, the chunk centre's local up (+Z on heightmap worlds, which runs the old
arithmetic). On a planet:

- The LOD material and biome scan walks the **digital line along `LocalUp`** (K = 0 to 8) instead of +Z, and prefers
  the corner farthest along it (`Meshing.MarchingCubes.PlanetMaterialScan`: LOD 1 at 45° takes the surface material
  as LOD 0 does).
- The CPU geomorph morphs along the **axis nearest `LocalUp`**, its steepness gate on the normal along that axis as on
  the plane (`Meshing.MarchingCubes.PlanetGeomorph`); the gate skips vertices more than 60° off the axis, as it skips
  steep slopes on the plane.

Planets mesh with Dual Contouring by default, so these run only on an MC planet (SP-D1, SP-D2).
