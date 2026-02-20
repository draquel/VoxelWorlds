# Dual Contouring Meshing System

**Module**: VoxelMeshing
**Last Updated**: 2026-02-19

## Table of Contents

1. [Overview](#overview)
2. [Algorithm Overview](#algorithm-overview)
3. [Pipeline Detail](#pipeline-detail)
4. [QEF Solver](#qef-solver)
5. [LOD Boundary Merging](#lod-boundary-merging)
6. [GPU Implementation](#gpu-implementation)
7. [Configuration](#configuration)
8. [Comparison with Marching Cubes](#comparison-with-marching-cubes)
9. [Performance Considerations](#performance-considerations)
10. [Troubleshooting](#troubleshooting)

---

## Overview

Dual Contouring (DC) is the second smooth meshing algorithm in the VoxelWorlds system, alongside [Marching Cubes](MARCHING_CUBES_MESHING.md). Where Marching Cubes places vertices **on edges** where the density field crosses the isosurface, Dual Contouring places vertices **inside cells** at positions that minimize error against all intersecting surface planes. This enables:

- **Sharp feature preservation**: Creases, corners, and hard edges emerge naturally from the QEF solve
- **Simpler LOD transitions**: Boundary cells merge via QEF re-solve instead of requiring Transvoxel transition cells
- **Quad-based output**: Generates quads (2 triangles each) rather than variable triangle counts per cell

Both DC and Marching Cubes share the **smooth terrain rendering pipeline** — triplanar texturing, the same vertex format (`FVoxelVertex`), and the same material system. The choice between them is purely algorithmic (set via `EMeshingMode::DualContouring` in the world configuration).

### Key Files

| File | Purpose |
|------|---------|
| `VoxelCPUDualContourMesher.h/cpp` | CPU implementation |
| `VoxelGPUDualContourMesher.h/cpp` | GPU compute implementation |
| `QEFSolver.h` (Private) | QEF solver with Jacobi SVD |
| `DualContourMeshGeneration.usf` | 4-pass GPU compute shader |
| `VoxelMeshingTypes.h` | Shared config (`FVoxelMeshingConfig`) |
| `MarchingCubesCommon.ush` | Shared shader utilities (voxel packing, vertex format) |

### Enum

```cpp
// VoxelCoreTypes.h
enum class EMeshingMode : uint8
{
    Cubic,
    MarchingCubes,
    DualContouring
};
```

---

## Algorithm Overview

Dual Contouring processes a chunk in **4 passes**:

```
Pass 1: Edge Crossing Detection
    For each cell in [-1, GridSize], check 3 edges (+X, +Y, +Z)
    Store hermite data (position, normal) for edges with density sign changes
                    |
                    v
Pass 2: QEF Vertex Solve
    For each cell, collect up to 12 edge crossings touching it
    Solve 3x3 QEF via Jacobi SVD for optimal vertex position
                    |
                    v
Pass 3.5: LOD Boundary Cell Merging (before quad generation)
    Merge groups of fine boundary cells into single coarser cells
    Re-solve merged QEF to produce vertices matching neighbor resolution
                    |
                    v
Pass 3: Quad Generation
    For each valid edge crossing, find 4 cells sharing that edge
    Emit a quad (2 triangles) using the 4 cell vertices
```

### Grid Layout

The DC grid extends from `-1` to `GridSize+1` in each axis to ensure proper edge lookups at boundaries:

```
GridDim = GridSize + 3   (total entries per axis)
GridSize = ChunkSize / Stride   (active cell count)
```

Cell and edge indices are packed into flat arrays:
- **Cell index**: `(CX+1) + (CY+1)*GridDim + (CZ+1)*GridDim*GridDim`
- **Edge index**: `CellIndex * 3 + Axis` (3 edges per cell: +X, +Y, +Z)

---

## Pipeline Detail

### Pass 1: Edge Crossing Detection

Each cell owns three edges emanating from its minimum corner in the +X, +Y, and +Z directions. For each edge:

1. Sample density at both endpoints
2. If the density crosses the isosurface threshold (`IsoLevel`), interpolate the crossing position:
   ```cpp
   float t = (IsoLevel - D0) / (D1 - D0);
   FVector3f CrossPos = P0 + (P1 - P0) * t;
   ```
3. Compute the surface gradient normal at the crossing point via central differences
4. Store hermite data: `{Position, Normal, bValid}` in the edge crossings array
5. Append the edge index to a compact valid-edge list (used by Pass 3)

**Boundary handling**: The mesher samples beyond chunk bounds using neighbor face, edge, and corner data from `FVoxelMeshingRequest` — the same data structures used by the Marching Cubes mesher.

### Pass 2: QEF Vertex Solve

For each cell in the grid:

1. Collect hermite data from **12 edges** touching the cell (edges from the cell itself and from adjacent cells):
   ```cpp
   // 12 edge references: 3 from (0,0,0), 2 from (1,0,0), 2 from (0,1,0),
   // 2 from (0,0,1), and 1 each from (1,1,0), (1,0,1), (0,1,1)
   static const FEdgeRef CellEdges[12] = {
       {0,0,0,0}, {0,0,0,1}, {0,0,0,2},
       {1,0,0,1}, {1,0,0,2},
       {0,1,0,0}, {0,1,0,2},
       {0,0,1,0}, {0,0,1,1},
       {1,1,0,2}, {1,0,1,1}, {0,1,1,0},
   };
   ```
2. Add each valid crossing's plane (`Normal . (v - Point) = 0`) to the QEF accumulator
3. Solve the QEF (see [QEF Solver](#qef-solver) section)
4. Assign material and biome via nearest-solid-voxel voting across the cell's 8 corners
5. Store the result in the cell vertices array

### Pass 3.5: LOD Boundary Cell Merging

See [LOD Boundary Merging](#lod-boundary-merging) section.

### Pass 3: Quad Generation

For each valid edge crossing:

1. Decode the edge index back to `(CX, CY, CZ, Axis)`
2. Find the **4 cells** sharing that edge using axis-specific offset tables:
   ```
   Axis 0 (X-edge): cells at (0,0,0), (0,-1,0), (0,-1,-1), (0,0,-1)
   Axis 1 (Y-edge): cells at (0,0,0), (0,0,-1), (-1,0,-1), (-1,0,0)
   Axis 2 (Z-edge): cells at (0,0,0), (-1,0,0), (-1,-1,0), (0,-1,0)
   ```
3. Verify all 4 cells have valid QEF vertices
4. Check **edge ownership**: at least one of the 4 cells must be in the interior `[0, GridSize-1]` range (prevents duplicate quads from overlapping neighbor regions)
5. Determine **winding order** from the density sign at the edge start:
   - `D0 < IsoLevel` → flipped winding
   - `D0 >= IsoLevel` → standard winding
6. Emit 6 indices (2 triangles) referencing the 4 cell vertex positions

---

## QEF Solver

The QEF (Quadratic Error Function) solver minimizes the sum of squared distances from a vertex `v` to the intersection planes:

```
E(v) = sum_i (n_i . (v - p_i))^2
```

where `p_i` are edge crossing points and `n_i` are surface normals.

### Implementation (`QEFSolver.h`)

Located in `VoxelMeshing/Private/QEFSolver.h`, the solver is a header-only struct:

**Accumulated state** (no per-plane storage):
- `ATA[3][3]` — Symmetric 3x3 matrix `A^T * A` (outer products of normals)
- `ATb[3]` — Vector `A^T * b` where `b_i = n_i . p_i`
- `BTB` — Scalar `b^T * b` (for error computation)
- `MassPoint` — Average of all intersection points (fallback position)
- `Count` — Number of planes added

**Key methods**:
- `Add(Point, Normal)` — Accumulate a plane into the matrix
- `Merge(Other)` — Combine two QEFs (used for LOD cell merging)
- `Solve(SVDThreshold, CellBounds, BiasStrength)` — Compute optimal vertex position

### SVD via Jacobi Eigenvalue Decomposition

The 3x3 symmetric matrix `A^T*A` is decomposed using iterative Givens rotations (up to 20 iterations, convergence threshold `1e-8`):

1. Find the largest off-diagonal element
2. Apply a Givens rotation to zero it out
3. Accumulate the rotation into the eigenvector matrix
4. Repeat until convergence

The pseudoinverse solution is then:
```
v = V * S^-1 * V^T * ATb
```
where eigenvalues below `SVDThreshold` are treated as zero (degenerate axes).

### Out-of-Bounds Clamping

If the QEF solution falls outside the cell bounds, it is blended toward the mass point:

```cpp
float Blend = saturate(DistOutside / CellSize * BiasStrength * 2.0);
Result = lerp(QEFResult, MassPoint, Blend);
```

This prevents vertices from drifting too far from their cells while still allowing feature-preserving placement when the solution is within bounds.

---

## LOD Boundary Merging

Unlike Marching Cubes which uses the **Transvoxel algorithm** (special transition cells with 512-entry lookup tables), Dual Contouring handles LOD transitions by **merging boundary cells**.

### How It Works

When a chunk face borders a coarser (higher LOD level) neighbor:

1. Compute the **merge ratio**: `CoarserStride / CurrentStride` (e.g., LOD 0 next to LOD 1 → ratio 2)
2. Group boundary cells into `MergeRatio x MergeRatio` blocks
3. For each block:
   a. Collect all edge crossings from the fine cells
   b. Feed them into a single merged QEF (`QEF.Merge()`)
   c. Solve the merged QEF within the combined cell bounds
   d. Invalidate the fine cell vertices
   e. Store the merged vertex at the base position and alias all fine positions to it

### Example

LOD 0 chunk next to LOD 1 neighbor on the +X face (MergeRatio = 2):

```
Before merging:          After merging:
+------+------+         +------+------+
| V_00 | V_01 |         |             |
+------+------+   →     |   V_merged  |
| V_10 | V_11 |         |             |
+------+------+         +------+------+
```

The 4 fine-cell vertices are replaced by 1 merged vertex whose position is the QEF solution using all edge crossings from all 4 cells. This naturally matches the coarser neighbor's vertex spacing.

### GPU Implementation

On the GPU, the merge map is built as a **CPU pre-pass** and uploaded as a structured buffer:
- Array of `uint` pairs: `[OriginalCellIndex, ReplacementCellIndex]`
- During quad generation, each cell index is checked against the merge map and redirected if matched

---

## GPU Implementation

The GPU mesher (`FVoxelGPUDualContourMesher`) executes the same algorithm via **4 RDG compute shader passes** in a single render graph:

### Compute Shader Passes

| Pass | Kernel | Thread Group | Purpose |
|------|--------|--------------|---------|
| 0 | `DCResetCountersCS` | [1,1,1] | Zero atomic counters |
| 1 | `DCEdgeCrossingCS` | [8,8,4] | Detect density sign changes |
| 2 | `DCQEFSolveCS` | [8,8,4] | Solve 3x3 QEF per cell (Jacobi SVD in registers) |
| 2.5 | `DCPrepareIndirectArgsCS` | [1,1,1] | Write indirect dispatch args from valid edge count |
| 3 | `DCQuadGenerationCS` | [64,1,1] | Emit quads (indirect dispatch) |

### GPU Intermediate Buffers

```hlsl
// Edge crossing hermite data (32 bytes per entry)
struct FDCEdgeCrossingGPU {
    float3 Position;      // Crossing point
    float3 Normal;        // Gradient normal
    uint PackedMaterial;  // MaterialID(8) | BiomeID(8)
    uint Flags;           // bit 0 = bValid
};

// Cell vertex from QEF solve (32 bytes per entry)
struct FDCCellVertexGPU {
    float3 Position;      // QEF-solved position
    uint PackedNormal;    // 10+10+10+2 packed
    uint PackedMaterial;  // MaterialID(8) | BiomeID(8)
    uint Flags;           // bit 0 = bValid
    uint VertexIndex;     // Pre-assigned output mesh index
    uint _Pad;            // Alignment
};
```

### Async Readback State Machine

The GPU mesher uses a multi-phase async readback for PMC renderer compatibility:

```
WaitingForCounters → CopyingCounters → WaitingForData → CopyingData → Complete
```

1. **WaitingForCounters**: Counter readback enqueued, polling `IsReady()`
2. **CopyingCounters**: Lock/copy/unlock vertex and index counts
3. **WaitingForData**: Vertex/index readback enqueued with correct sizes
4. **CopyingData**: Lock/copy/unlock mesh data into `FChunkMeshData`
5. **Complete**: Data ready on CPU, `OnComplete` callback fired

### Shader Path

```
/Plugin/VoxelWorlds/Private/DualContourMeshGeneration.usf
```

Includes `MarchingCubesCommon.ush` for shared utilities: voxel packing/unpacking, vertex format (`FVoxelVertexGPU`), normal packing, material data packing.

---

## Configuration

### FVoxelMeshingConfig Fields

DC-specific configuration lives in `VoxelMeshingTypes.h` under the "Dual Contouring" category:

```cpp
// SVD singular value threshold for QEF solver (0.001 - 1.0)
// Higher = smoother but less feature-preserving
// Lower = sharper features but potential vertex instability
float QEFSVDThreshold = 0.1f;

// Blend factor for mass-point bias when QEF vertex is outside cell (0.0 - 1.0)
// 0.0 = pure QEF solution (may escape cell)
// 1.0 = always use mass point
float QEFBiasStrength = 0.5f;
```

DC also uses these shared smooth meshing fields:
- `bUseSmoothMeshing` — Must be `true` (DC is a smooth meshing algorithm)
- `IsoLevel` — Isosurface threshold (default 0.5 = density 127)
- `MaxVerticesPerChunk` / `MaxIndicesPerChunk` — Buffer capacity limits

### Recommended Settings

**Default (balanced):**
```cpp
Config.QEFSVDThreshold = 0.1f;   // Good balance of sharpness and stability
Config.QEFBiasStrength = 0.5f;   // Moderate mass-point clamping
Config.IsoLevel = 0.5f;
```

**Sharp features (cliffs, buildings):**
```cpp
Config.QEFSVDThreshold = 0.01f;  // Preserve sharp creases
Config.QEFBiasStrength = 0.3f;   // Less aggressive clamping
```

**Smooth terrain (rolling hills):**
```cpp
Config.QEFSVDThreshold = 0.5f;   // Smooth out noise
Config.QEFBiasStrength = 0.7f;   // Keep vertices well-centered
```

---

## Comparison with Marching Cubes

### Feature Matrix

| Feature | Marching Cubes | Dual Contouring |
|---------|---------------|-----------------|
| Vertex placement | On edges (interpolated) | Inside cells (QEF-solved) |
| Output topology | Triangles (variable count) | Quads (2 tris per edge crossing) |
| Sharp features | Rounded (edge-constrained) | Preserved (QEF minimization) |
| LOD transitions | Transvoxel (512-entry tables) | Cell merging (QEF re-solve) |
| Lookup tables | 256-entry edge + tri tables | None (algorithmic) |
| Triangle count | Higher (complex cells) | Lower (1 quad per edge crossing) |
| Vertex count | Higher (shared edges limited) | Lower (1 vertex per cell) |
| Implementation complexity | Moderate | Higher (QEF solver, SVD) |

### Shared Infrastructure

Both meshers share:
- `IVoxelMesher` interface and `FVoxelMeshingRequest` / `FChunkMeshData` types
- `FVoxelVertex` output format (28 bytes: position, packed normal+AO, UV, packed material)
- Triplanar UV computation (dominant-axis projection)
- Material and biome assignment from solid voxels
- Neighbor data handling (face, edge, corner slices)
- `MarchingCubesCommon.ush` shader utilities (GPU path)
- Renderer compatibility (both Custom VF and PMC)

### When to Use Which

**Use Marching Cubes when:**
- You need proven, battle-tested isosurface extraction
- Transvoxel LOD transitions are important (well-documented algorithm)
- Sharp features are not critical (organic terrain)

**Use Dual Contouring when:**
- Sharp edges and corners must be preserved (cliffs, caves, man-made structures)
- Lower triangle counts are desired
- LOD boundary merging is preferred over Transvoxel complexity
- You want per-cell vertex control via QEF tuning

---

## Performance Considerations

### Triangle and Vertex Counts

DC typically produces fewer triangles and vertices than MC for the same terrain:

| Chunk Type | MC Vertices | DC Vertices | MC Triangles | DC Triangles |
|------------|-------------|-------------|--------------|--------------|
| Flat surface | ~600 | ~200 | ~200 | ~100 |
| Hilly terrain | ~6000 | ~2000 | ~2000 | ~1000 |
| Complex caves | ~24000 | ~8000 | ~8000 | ~4000 |

*(Approximate — varies with terrain complexity)*

### GPU Performance

Per 32^3 chunk on modern GPUs:
- Edge crossing detection: ~0.3ms
- QEF solve (Jacobi SVD in registers): ~0.8ms
- Quad generation: ~0.2ms
- **Total: ~1.3ms per chunk** (target < 3ms)

The QEF solve pass is the most expensive due to the iterative Jacobi decomposition. The `[loop]` attribute (not `[unroll]`) is required for the Jacobi iteration to avoid shader compiler miscompilation with dynamic break and indexing.

### Memory Usage

Per 32^3 chunk at LOD 0:
- Edge crossings buffer: `GridDim^3 * 3 * 32 bytes` = ~3.3 MB (temporary)
- Cell vertices buffer: `GridDim^3 * 32 bytes` = ~1.1 MB (temporary)
- Output vertex data: ~50-80 KB (persistent)
- Output index data: ~20-40 KB (persistent)

Intermediate buffers are allocated per-dispatch and released after readback.

---

## Troubleshooting

### Vertex Instability / Flickering

**Symptom**: Vertices jitter or snap between positions across frames

**Solutions**:
1. Increase `QEFSVDThreshold` (try 0.2 - 0.5) to suppress degenerate axes
2. Increase `QEFBiasStrength` (try 0.7 - 1.0) to keep vertices closer to mass point
3. Check that density field is smooth (noisy inputs cause unstable QEF solutions)

### Missing Quads at Chunk Boundaries

**Symptom**: Holes or gaps at chunk edges

**Solutions**:
1. Verify neighbor data is being extracted correctly (face, edge, corner slices)
2. Check that the grid extends to `[-1, GridSize+1]` for proper boundary edge detection
3. Ensure edge ownership check is correct (at least one of 4 cells in interior range)

### LOD Seams

**Symptom**: Visible cracks between chunks at different LOD levels

**Solutions**:
1. Verify `NeighborLODLevels` is being populated correctly in `FVoxelMeshingRequest`
2. Check that `MergeLODBoundaryCells()` is running before quad generation
3. Ensure merge ratio calculation is correct (`CoarserStride / CurrentStride`)

### Inverted Faces

**Symptom**: Some quads appear inside-out (dark or culled)

**Solution**: Check winding order determination. The flip condition is `D0 < IsoLevel` where `D0` is the density at the edge's starting cell position.

### GPU Shader Compilation Issues

**Symptom**: Shader compile errors or visual artifacts on GPU path

**Solutions**:
1. Ensure `JacobiEigen3x3` uses `[loop]` not `[unroll]` for the iteration loop
2. Check that `GridDim` uniform matches the actual buffer allocation
3. Verify `MaxVertexCount` / `MaxIndexCount` bounds checks prevent buffer overflows

---

## References

- [Dual Contouring of Hermite Data](https://www.cs.rice.edu/~jwarren/papers/dualcontour.pdf) — Ju, Losasso, Schaefer, Warren (2002)
- [Manifold Dual Contouring](https://people.engr.tamu.edu/schaefer/research/dualsimp_tvcg.pdf) — Schaefer, Ju, Warren (2007)
- [Marching Cubes Meshing (companion doc)](MARCHING_CUBES_MESHING.md) — MC algorithm, Transvoxel, LOD material selection
- [Architecture Overview](ARCHITECTURE.md) — System architecture and module organization

---

**Status**: Implemented — CPU and GPU Dual Contouring with LOD boundary merging, QEF solver with Jacobi SVD
