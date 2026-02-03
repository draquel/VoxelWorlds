# Smooth Meshing System

**Module**: VoxelMeshing
**Last Updated**: 2026-02-02

## Table of Contents

1. [Overview](#overview)
2. [Marching Cubes Algorithm](#marching-cubes-algorithm)
3. [LOD Support](#lod-support)
4. [Transvoxel Algorithm](#transvoxel-algorithm)
5. [Configuration](#configuration)
6. [Implementation Details](#implementation-details)
7. [Performance Considerations](#performance-considerations)
8. [Troubleshooting](#troubleshooting)

---

## Overview

The smooth meshing system generates organic, curved terrain surfaces using the **Marching Cubes** algorithm, as an alternative to the blocky cubic meshing style. The system includes:

- **Marching Cubes**: Core algorithm for isosurface extraction from density fields
- **LOD Support**: Strided sampling for level-of-detail rendering
- **Transvoxel**: Seamless LOD transitions without visible seams

### When to Use Smooth Meshing

- Organic terrain (hills, caves, overhangs)
- Sculpted/deformed terrain
- Planetary surfaces
- Any terrain where smooth contours are desired

### Key Files

| File | Purpose |
|------|---------|
| `VoxelCPUSmoothMesher.h/cpp` | CPU implementation |
| `VoxelGPUSmoothMesher.h/cpp` | GPU compute implementation |
| `MarchingCubesTables.h/cpp` | Lookup tables for standard Marching Cubes |
| `TransvoxelTables.h/cpp` | Lookup tables for LOD transitions |
| `VoxelMeshingTypes.h` | Configuration structures |

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

For smooth meshing, this is converted to a 0.0-1.0 float range, with an `IsoLevel` threshold (default 0.5) determining the surface.

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

## Transvoxel Algorithm

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

1. **Detect Transition Faces**: The chunk manager identifies which chunk faces border lower-LOD neighbors
2. **Mark Cells**: Cells on transition boundaries are processed differently
3. **Sample 9 Points**: Instead of 8 corners, sample 9 points on the transition face
4. **Build Case Index**: 2^9 = 512 possible configurations
5. **Generate Transition Geometry**: Use special lookup tables that produce geometry matching both LOD levels

### Implementation

**In VoxelMeshingRequest:**
```cpp
// Set by chunk manager based on neighbor LOD levels
uint8 TransitionFaces = 0;

// Flags for each face
static constexpr uint8 TRANSITION_XNEG = 1 << 0;
static constexpr uint8 TRANSITION_XPOS = 1 << 1;
static constexpr uint8 TRANSITION_YNEG = 1 << 2;
static constexpr uint8 TRANSITION_YPOS = 1 << 3;
static constexpr uint8 TRANSITION_ZNEG = 1 << 4;
static constexpr uint8 TRANSITION_ZPOS = 1 << 5;
```

**In Chunk Manager (ProcessMeshingQueue):**
```cpp
// Check each neighbor's LOD level
for (int i = 0; i < 6; i++) {
    FIntVector NeighborCoord = ChunkCoord + NeighborOffsets[i];
    if (NeighborState->LODLevel > CurrentLOD) {
        // Neighbor is coarser - need transition cells
        MeshRequest.TransitionFaces |= TransitionFlags[i];
    }
}
```

**In Smooth Mesher:**
```cpp
for each cell:
    if (IsTransitionCell(X, Y, Z, TransitionMask, FaceIndex)) {
        ProcessTransitionCell(X, Y, Z, FaceIndex);
    } else {
        ProcessCubeLOD(X, Y, Z);  // Standard Marching Cubes
    }
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
// Enable smooth meshing (vs cubic)
bool bUseSmoothMeshing = false;

// Isosurface threshold (0.0-1.0, default 0.5 = density 127)
float IsoLevel = 0.5f;

// Use Transvoxel for seamless LOD transitions (recommended)
bool bUseTransvoxel = true;

// Fallback: skirts (vertical geometry to hide seams)
// Only used when Transvoxel is disabled
bool bGenerateSkirts = true;
float SkirtDepth = 2.0f;
```

### Recommended Settings

**For Production (Transvoxel enabled):**
```cpp
Config.bUseSmoothMeshing = true;
Config.IsoLevel = 0.5f;
Config.bUseTransvoxel = true;
Config.bGenerateSkirts = false;  // Not needed with Transvoxel
```

**For Debugging (simpler, but with seams):**
```cpp
Config.bUseSmoothMeshing = true;
Config.bUseTransvoxel = false;
Config.bGenerateSkirts = true;
Config.SkirtDepth = 2.0f;
```

---

## Implementation Details

### CPU vs GPU

**CPU Mesher (`FVoxelCPUSmoothMesher`):**
- Synchronous execution
- Good for debugging
- Used when GPU mesher unavailable
- Performance: ~30-80ms per 32^3 chunk at LOD 0

**GPU Mesher (`FVoxelGPUSmoothMesher`):**
- Asynchronous compute shaders
- Much faster for large worlds
- Requires RDG/RHI setup
- Performance: ~1-5ms per chunk

### Neighbor Data Handling

Smooth meshing requires sampling beyond chunk boundaries. The meshing request includes neighbor slice data:

```cpp
// Face neighbors (ChunkSize^2 voxels each)
TArray<FVoxelData> NeighborXPos, NeighborXNeg;
TArray<FVoxelData> NeighborYPos, NeighborYNeg;
TArray<FVoxelData> NeighborZPos, NeighborZNeg;

// Edge neighbors (ChunkSize voxels each) - for corner cubes
TArray<FVoxelData> EdgeXPosYPos, EdgeXPosYNeg, ...;

// Corner neighbors (single voxels) - for corner-of-corner cubes
FVoxelData CornerXPosYPosZPos, ...;
```

### Material Assignment

For smooth meshes, material comes from the majority of solid corners:

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

---

## Performance Considerations

### Triangle Counts

Smooth meshing typically produces more triangles than cubic:

| Chunk Type | Cubic (Greedy) | Smooth (MC) |
|------------|----------------|-------------|
| Flat surface | ~2 tris | ~200 tris |
| Hilly terrain | ~500 tris | ~2000 tris |
| Complex caves | ~2000 tris | ~8000 tris |

### Optimization Strategies

1. **LOD**: Use aggressive LOD to reduce distant triangle counts
2. **Transvoxel**: Avoids expensive skirt geometry
3. **GPU Meshing**: 10-20x faster than CPU
4. **Early-Out**: Skip empty cubes (EdgeTable[cubeIndex] == 0)
5. **Pre-allocated Buffers**: Reserve based on estimates

### Memory Usage

Per 32^3 chunk at LOD 0:
- Input voxel data: 128 KB
- Output vertices (typical): ~50-100 KB
- Output indices (typical): ~30-60 KB

---

## Troubleshooting

### Visible Seams at LOD Boundaries

**Symptom**: Gaps or cracks between chunks at different LOD levels

**Solutions**:
1. Enable Transvoxel: `Config.bUseTransvoxel = true`
2. Verify TransitionFaces is being set correctly in chunk manager
3. Check that neighbor LOD levels are being queried correctly

### Incorrect Surface Position

**Symptom**: Surface appears at wrong density level

**Solution**: Adjust `Config.IsoLevel` (0.5 = density 127)

### Inverted Normals

**Symptom**: Lighting looks inside-out

**Solution**: Check triangle winding order in lookup tables

### Performance Issues

**Symptoms**: Slow chunk generation, frame drops

**Solutions**:
1. Switch to GPU mesher
2. Increase LOD aggressiveness
3. Reduce MaxChunksPerFrame in config
4. Profile with Unreal Insights

### Missing Geometry at Chunk Edges

**Symptom**: Holes at chunk boundaries

**Solution**: Ensure neighbor data is being extracted correctly in `ExtractNeighborEdgeSlices()`

---

## References

- [Marching Cubes Original Paper](http://www.cs.carleton.edu/cs_comps/0405/shape/marching_cubes.html)
- [Transvoxel Algorithm](https://transvoxel.org/) - Eric Lengyel
- [Polygonising a Scalar Field](http://paulbourke.net/geometry/polygonise/) - Paul Bourke
- [GPU Gems 3: Chapter 1](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu)

---

**Status**: Implemented - CPU Transvoxel active, GPU implementation pending
