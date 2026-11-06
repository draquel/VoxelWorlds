# Dual Contouring Meshing System

> State as of 2026-11-02 (code review CR5). Terms: `Documentation/GLOSSARY.md`.

**Module**: VoxelMeshing

Dual Contouring runs on the CPU only: `FVoxelCPUDualContourMesher` meshes each chunk's interior and the face, edge and
corner seams between chunks. The GPU Dual Contouring mesher was retired in seam ownership (its QEF solve was not
bit-exact with the CPU seam rings) and deleted in code review CR1 with its shaders (D-CR12).

## Table of Contents

1. [Overview](#overview)
2. [Algorithm Overview](#algorithm-overview)
3. [Pipeline Detail](#pipeline-detail)
4. [QEF Solver](#qef-solver)
5. [Chunk Boundaries and LOD Seams](#chunk-boundaries-and-lod-seams)
6. [Configuration](#configuration)
7. [Comparison with Marching Cubes](#comparison-with-marching-cubes)
8. [Performance Considerations](#performance-considerations)
9. [Troubleshooting](#troubleshooting)

---

## Overview

Dual Contouring (DC) is the second smooth meshing algorithm in the VoxelWorlds system, alongside [Marching Cubes](MARCHING_CUBES_MESHING.md). Where Marching Cubes places vertices **on edges** where the density field crosses the isosurface, Dual Contouring places vertices **inside cells** at positions that minimize error against all intersecting surface planes. This enables:

- **Sharp feature preservation**: Creases, corners, and hard edges emerge naturally from the QEF solve
- **No transition tables at LOD boundaries**: the mixed-LOD seam meshers fan fine crossings onto the coarse side's cell vertices
- **Quad-based output**: Generates quads (2 triangles each) rather than variable triangle counts per cell

Both DC and Marching Cubes share the **smooth terrain rendering pipeline** — triplanar texturing, the same vertex format (`FVoxelVertex`), and the same material system. The choice between them is purely algorithmic (set via `EMeshingMode::DualContouring` in the world configuration).

### Key Files

| File | Purpose |
|------|---------|
| `VoxelCPUDualContourMesher.h/cpp` | The mesher: the chunk's interior (and the collision cook's whole-chunk request) |
| `VoxelCPUDualContourSeams.cpp` | The face, edge and corner seam meshers, same-LOD and mixed-LOD |
| `VoxelCrossingGradient.h` (Private) | Voxel-resolution crossings and fitted shading normals, shared with Marching Cubes |
| `QEFSolver.h` (Private) | QEF solver with Jacobi SVD |
| `VoxelMeshingTypes.h` | Shared config (`FVoxelMeshingConfig`) and the request (`FVoxelMeshingRequest`, `EVoxelMeshCellDomain`) |

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

Dual Contouring processes a chunk in **3 passes**:

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

### Cell Domains

The request's `MeshCellDomain` says which cells a pass meshes:

- **`Interior`** (every streamed chunk mesh): only the cells `[0, GridSize-1)` per axis, which depend on no neighbour. The
  request carries no neighbour data; the boundary layers belong to the seam jobs ([Chunk Boundaries and LOD
  Seams](#chunk-boundaries-and-lod-seams)).
- **`WholeChunk`**: the chunk's own cells plus the `-1` boundary layer against the neighbour slices in the request. The
  collision cook uses it.

---

## Pipeline Detail

### Pass 1: Edge Crossing Detection

Each cell owns three edges emanating from its minimum corner in the +X, +Y, and +Z directions. For each edge:

1. Sample density at both endpoints
2. If the density crosses the isosurface threshold (`IsoLevel`), find the crossing: at stride 1 by interpolating between
   the two samples, at a stride by LOD 0's crossing on the edge (G80, below)
   ```cpp
   float t = (IsoLevel - D0) / (D1 - D0);
   FVector3f CrossPos = P0 + (P1 - P0) * t;
   ```
3. Compute the surface normal at the crossing point (the QEF's plane normal): the fitted gradient (G62, below), or where
   the fit is ill-conditioned the central difference about the voxel the crossing floors to. In the Interior domain and in
   every seam sampler the fit reads only the voxels the job holds and the central difference's taps clamp into them ("the
   terrain continues": no neighbour dependence), a clamped axis's one-sided difference scaled back to the full span
   (`VoxelDCGradient::ClampedGradientNormal`; unscaled it lost up to half its weight and drew lines of shading along chunk
   faces, G25)
4. Store hermite data: `{Position, Normal, bValid}` in the edge crossings array
5. Append the edge index to a compact valid-edge list (used by Pass 3)

**Crossings at a stride (G80).** Generated density saturates a voxel either side of the surface
(`SignedDistanceToDensity`), so a crossing interpolated between two samples a stride apart lands toward the edge's
middle whatever the surface does between them: LOD 1 and 2 would be terraced a stride apart, the "ribs" visible in
their own shadows under a low sun. At a stride the crossing is LOD 0's on the edge: the first voxel step along it that
changes sign, interpolated within that step (`VoxelLODCrossing::FindVoxelCrossing`), and its QEF normal is the fitted
gradient there. Every LOD's crossings lie on LOD 0's surface; LOD 2 still drops the detail its cells cannot hold. Tests
`Meshing.LODCrossings.Planes`, `Streaming.{Planet,Plane}.LODPositions`.

**Shading normals (G62).** The central difference about the voxel a crossing floors to reads the saturated rock under a
surface a fraction of a voxel above it, so its normals tilt with the surface's phase in the voxel layer (bands a voxel
layer apart). The shading normals are instead the density gradient fitted to the voxels on the ramp around the crossing
(`VoxelCrossingGradient::FitNormal`: weighted least squares over the 3x3x3 block around the crossing's nearest voxel,
0 < density < 255, tent weights, a free offset; exact for any plane), over the voxels the view holds (skipped, not
clamped). Where the ramp's voxels span fewer than three directions the central difference is used, which is exact
there. The QEF takes the fitted gradient too, at a stride (G80) and at LOD 0 (G81). Tests `Meshing.CrossingGradient.Planes`,
`Streaming.{Planet,Plane}.GradientBands`.

**Vertex normals at a stride (G44).** Gradient taps a stride apart read 0 or 255 on every axis the surface crosses
between them, so at LOD 2 each component is a sign and the normal snaps toward a cube direction (a brighter LOD 2 ring
on planets). A strided cell's **vertex normal** is LOD 0's instead: the fitted normals of its voxel-resolution crossings,
summed (G80; G44 first took the normal of the voxel cell next to the crossing nearest the vertex). Tests
`Streaming.{Planet,Plane}.LODNormals`. The interior pass and the seam template (`ComputeCellVertex`) run the same op
sequence, so ring recomputes stay bit-identical.

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
3. Solve the QEF (see [QEF Solver](#qef-solver) section); the vertex normal is the crossings' shading normals summed
   (Pass 1's notes)
4. Assign material and biome via nearest-solid-voxel voting across the cell's 8 corners
5. Store the result in the cell vertices array

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
4. Check **edge ownership**: the edge itself must lie in `[0, GridSize)` per axis, so each physical edge belongs to one
   chunk; in the Interior domain all 4 cells must also be interior
5. Determine **winding order** from the density sign at the edge start:
   - `D0 < IsoLevel` → flipped winding
   - `D0 >= IsoLevel` → standard winding
6. Split the quad along the diagonal that maximises the smaller triangle's area (a fixed diagonal can make slivers the
   renderer drops) and emit 6 indices

### Materials and soft borders (VP2-O4)

The material ID reaches the pixel shader in UV1.x and is rounded there, so every triangle carries one material: the
quad's, the material of its edge's solid endpoint. A cell vertex shared by quads of different materials is duplicated
(same position and normal). Since VP2-O4 a border between two materials fades across a cell instead of following the
quads' edges:

- **The cell's material.** Each cell votes: the materials of the solid endpoints of its sign-change edges, each corner
  once, the most common winning (a tie to the first in the cell-edge table). The interior pass records each crossing's
  solid material in Pass 1; the seams' `ComputeCellVertex` reads it with the cell's own sampler, so a ring recompute
  agrees with the interior pass. A coarse cell solved by the boundary fallback (G17) takes its first fine crossing's.
- **The quad's blend.** The second material is the most common cell material among the quad's cells that differs from
  the quad's own. Each vertex gets a blend code (`VoxelMaterialBlend`, `ChunkRenderData.h`): the second material, and
  the weight flag where its cell has the second material. Both materials are flat per quad, so only the weight
  interpolates. A quad whose cells all have its material has no blend.
- **Why it is continuous.** A vertex then shows its cell's material in every quad it is emitted for: with the weight
  where the cell has the second material, without it where it has the first. So the material changes from cell to
  cell, across the quads, and a border is a ramp one cell wide (75 cm at LOD 0, the stride's width further out).
  A third material inside one quad shows the quad's own material at its corner.
- **Emission.** `VoxelDCMaterialBlend::EmitQuadCells` (`VoxelCPUDualContourShared.h`) emits a quad or fan's cells for
  the interior pass and every seam path: it reuses a vertex emitted with the same material, biome and blend code, and
  otherwise keeps a duplicate per (cell, code, material, biome).
- **Transport.** `FChunkMeshData::MaterialBlends` (one code per vertex; the other meshers leave it empty), then
  `FVoxelVertex`'s bits 16-23 and flag bits 24-25, then the vertex colour the far field uses: R the second material, A
  255 less its weight. `M_VoxelMaster`'s `FarFieldMaterialPick` picks one of the two per pixel
  ([MATERIAL_SYSTEM.md](MATERIAL_SYSTEM.md) "Soft Material Borders").

Tests `VoxelWorlds.Meshing.DualContour.MaterialBorder.MB5_LOD0_SoftBorder`, `.MB6_LOD1_SoftBorder` and
`.MB7_OneMaterialNoBlend`. They check that a triangle's blend is flat, that blends occur only near the border, and that
every vertex at one position shows one material.

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
- `Solve(SVDThreshold, CellBounds, BiasStrength)` — Compute optimal vertex position

### SVD via Jacobi Eigenvalue Decomposition

The 3x3 symmetric matrix `A^T*A` is decomposed using iterative Givens rotations (up to 20 iterations, convergence threshold `1e-8`):

1. Find the largest off-diagonal element
2. Apply a Givens rotation to zero it out on both sides of the diagonal
3. Accumulate the rotation into the eigenvector matrix
4. Repeat until convergence

The pseudoinverse solves for the offset from the mass point:
```
v = MassPoint + V * S^-1 * V^T * (ATb - ATA * MassPoint)
```
where eigenvalues below `SVDThreshold` are treated as zero (degenerate axes), so a degenerate axis stays at the mass point:
a flat cell's vertex is the mass point on its plane (G81). Before G81 the rotation zeroed its pivot above the diagonal
only (wrong eigenpairs) and the solve was for `v` itself, so a degenerate axis went to the chunk's origin and flat ground
had vertices a voxel or more off the surface. Test `Meshing.QEF.Solve`.

### Out-of-Bounds Clamping

If the QEF solution falls outside the cell bounds, it is blended toward the mass point:

```cpp
float Blend = saturate(DistOutside / CellSize * BiasStrength * 2.0);
Result = lerp(QEFResult, MassPoint, Blend);
```

A result still outside the cell after the blend is cut back along the segment from the mass point (inside the cell: its
crossings are on the cell's edges) at the cell's faces, so every vertex stays in its own cell.

---

## Chunk Boundaries and LOD Seams

Dual Contouring closes chunk boundaries, at one LOD or across two, by **seam ownership**: a chunk meshes its interior
from its own voxels, and each boundary between chunks (a face, an edge, a corner) is meshed once by one seam job from
the voxels of every chunk that touches it. The scheduling, ownership and invalidation rules are in
[STREAMING_AND_SEAMS.md](STREAMING_AND_SEAMS.md); the mesher-side rules are these:

- **Same-LOD seams** (`GenerateFaceSeamMeshCPU`, `GenerateEdgeSeamMeshCPU`, `GenerateCornerSeamMeshCPU`) recompute the
  participants' boundary-layer cells (the rings) with each side's own view of its voxels, so a ring vertex is
  bit-identical to the one the interior pass would make, and emit only the quads the interiors left out.
- **Mixed-LOD seams** (`GenerateMixedFaceSeam`, `GenerateMixedEdgeSeam`, `GenerateMixedCornerSeam`) recompute each
  side's rings at that side's own stride and stitch the boundary at the finest participant's stride: every fine crossing
  gets a fan onto the coarser side's ring vertices, a quad collapsing to a triangle where its two coarse-side slots fall
  in the same coarse cell.
- **Boundary fallback vertex (G17).** A fine crossing can face a coarse cell whose corners all have one sign (the surface
  pokes through the coarse cell's face between its samples), so that cell has no vertex at its own stride. It gets one
  from `ComputeBoundaryFallbackVertex`: the same QEF solve over every voxel-resolution crossing on the cell's six faces,
  inside the cell's bounds. It is a function of the voxels, the cell and its stride only, so every mixed job that fans
  onto the cell solves the same vertex. Tests `Meshing.DualContour.SeamClosure.MS4_MixedFaceClosureNoise`,
  `Streaming.{Planet,Plane}.ClosedSurface`.

Every position is QEF output clamped to its own cell, so no seam moves a vertex an interior pass emits. The seam suites
are `VoxelWorlds.Meshing.DualContour.SeamClosure.*` (closure, ring bit-match, empty seams; `MS*` for mixed LODs).

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
- `IsoLevel` — Isosurface threshold (default 0.5 = density 127)

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
| LOD transitions | Transvoxel ribbon + geomorph in the seam band | Mixed-LOD seam fans onto the coarse rings |
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
- Seam ownership: interior meshes plus face, edge and corner seam jobs
- Voxel-resolution crossings and fitted shading normals (`VoxelCrossingGradient.h`)

### When to Use Which

**Use Marching Cubes when:**
- Rounded, organic terrain is wanted and sharp features are not

**Use Dual Contouring when:**
- Sharp edges and corners must be preserved (cliffs, caves, man-made structures)
- Lower triangle counts are desired
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

### Cost

Meshing runs on worker threads. The QEF solve is the most expensive pass (the iterative Jacobi decomposition per cell).
The fitted shading normals (G62) and voxel-resolution crossings (G80) cost chunk-mesh worker time at LOD 1 and 2 in
exchange for LOD 0's surface; the edge-crossing and cell arrays are temporaries of one job, sized `GridDim^3`.

---

## Troubleshooting

### Vertex Instability / Flickering

**Symptom**: Vertices jitter or snap between positions across frames

**Solutions**:
1. Increase `QEFSVDThreshold` (try 0.2 - 0.5) to suppress degenerate axes
2. Increase `QEFBiasStrength` (try 0.7 - 1.0) to keep vertices closer to mass point
3. Check that density field is smooth (noisy inputs cause unstable QEF solutions)

### Holes at Chunk Boundaries or LOD Seams

**Symptom**: Holes, gaps or cracks at chunk edges, at one LOD or between two

**Solutions**:
1. The interior mesh leaves the boundary layers out by design: check that the seam jobs for the chunk ran and were not
   left stale (`STREAMING_AND_SEAMS.md` has the diagnostics)
2. Run the seam suites (`VoxelWorlds.Meshing.DualContour.SeamClosure.*`) and `Streaming.{Planet,Plane}.ClosedSurface`
3. For a hole between two LODs, check whether the coarse cell facing it got a boundary fallback vertex (G17)

### Inverted Faces

**Symptom**: Some quads appear inside-out (dark or culled)

**Solution**: Check winding order determination. The flip condition is `D0 < IsoLevel` where `D0` is the density at the edge's starting cell position.

---

## References

- [Dual Contouring of Hermite Data](https://www.cs.rice.edu/~jwarren/papers/dualcontour.pdf) — Ju, Losasso, Schaefer, Warren (2002)
- [Manifold Dual Contouring](https://people.engr.tamu.edu/schaefer/research/dualsimp_tvcg.pdf) — Schaefer, Ju, Warren (2007)
- [Marching Cubes Meshing (companion doc)](MARCHING_CUBES_MESHING.md) — MC algorithm, Transvoxel, LOD material selection
- [Streaming and seams](STREAMING_AND_SEAMS.md) — seam jobs, ownership and scheduling
- [Architecture Overview](../ARCHITECTURE.md) — System architecture and module organization
