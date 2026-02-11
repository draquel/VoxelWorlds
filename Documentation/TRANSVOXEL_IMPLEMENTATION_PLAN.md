# Transvoxel Seam Stitching — Implementation Plan

**Project**: VoxelWorlds Plugin (UE 5.7.2)
**Target File**: `VoxelCPUSmoothMesher.cpp` (CPU path; GPU path later)
**Status**: Transvoxel was previously implemented but disabled due to bugs. This plan provides a corrected, step-by-step reimplementation.

---

## 1. Context & Known Issues

The previous implementation was disabled (`bUseTransvoxel = false`) due to:

1. **Requires face, edge, AND corner neighbor data** — boundary cells need voxel data from up to 7 neighboring chunks, but the current `ExtractNeighborEdgeSlices()` only provides face-neighbor data.
2. **Subtle bugs with fractional sample positions at LOD 0** — transition cell vertex positions were incorrectly computed, causing geometry to not align with the regular Marching Cubes mesh.
3. **Edge cases at chunk corners** — where two or three transition faces meet, the transition cell geometry was incorrect or missing.

### Root Cause Analysis

Based on research of working implementations (Godot Voxel Tools, stoyannk/voxels, Gnurfos/transvoxel_rs, Eric Lengyel's official tables), the most common bugs in Transvoxel implementations are:

- **Wrong sample point numbering**: The transition cell sample numbering in Lengyel's paper Figure 4.16 does NOT match the table indexing order. The correct bit ordering for the 9-bit case code is:
  ```
  0x040 --- 0x020 --- 0x010
    |         |         |
  0x080 --- 0x100 --- 0x008
    |         |         |
  0x001 --- 0x002 --- 0x004
  ```
- **Transition cell occupies half the regular cell space** — the transition geometry should only fill the half of the boundary cell closest to the higher-LOD neighbor, not the full cell.
- **Secondary vertex positions not computed** — regular mesh vertices near transition boundaries must be "pushed inward" to make room for transition cells. Without this, transition geometry overlaps regular geometry.
- **Winding order reversal** — when `transitionCellClass[caseCode]` has bit 7 set (`& 0x80`), triangle winding must be reversed.
- **Missing vertex position clamping** — edge interpolation t-values should be clamped to avoid degenerate triangles (typically `[0.001, 0.999]`).

---

## 2. Architecture Overview

### What Transvoxel Does

When a high-LOD chunk (e.g., LOD 0) borders a low-LOD chunk (e.g., LOD 1), the boundary vertices don't align because:
- High-LOD has N vertices along the boundary edge
- Low-LOD has N/2 vertices along the same edge

Transvoxel inserts **transition cells** along the boundary face of the **higher-LOD** chunk. These cells have:
- **9 sample points** on the high-resolution face (3×3 grid)
- **4 sample points** on the low-resolution face (2×2 corners)
- Total: **13 unique sample points** per transition cell (but only 9 contribute to the case code)

The transition cell produces geometry that:
- Matches the high-LOD mesh on its interior face
- Matches the low-LOD mesh on its exterior face
- Fills any gaps between the two resolutions

### Two-Part Approach

Following the Godot Voxel Tools approach (the most battle-tested open-source implementation):

1. **Regular cells with secondary positions**: Process standard Marching Cubes cells as normal, but for cells touching a transition boundary, compute a **secondary vertex position** that is pushed inward to make room for the transition cell.

2. **Transition cells**: Process the boundary face cells using the Transvoxel lookup tables (512 cases → 56 equivalence classes).

The vertex shader (or material in our case via MPC) interpolates between primary and secondary positions based on whether a neighboring chunk is at a lower LOD.

---

## 3. Implementation Phases

### Phase A: Neighbor Data Expansion (30 min)

**Problem**: Current `ExtractNeighborEdgeSlices()` only provides 1-voxel-deep slices from face neighbors. Transition cells need density values that may be at fractional positions between voxels, and the regular mesh needs neighbor data for gradient computation already. We need to ensure we have sufficient neighbor data.

**Tasks**:

1. **Verify neighbor data extent**: Transition cells sample a 3×3 grid on the boundary face. At LOD 0 with stride 1, this means we need density data at positions `(0, y, z)`, `(0.5, y, z)` for the midpoints — but since we're sampling from voxel data, we actually just need the boundary voxels themselves. The 9 sample positions map to actual voxel positions within the chunk and its face neighbor.

2. **Ensure face-neighbor density is available**: For each face where `TransitionFaces` bit is set, we need the neighbor chunk's voxel data at the positions that correspond to our boundary. The key insight: **the 4 low-res corner samples come from the same positions as 4 of our 9 high-res samples** — they use the same density values, just at half resolution.

3. **No edge/corner neighbor data needed**: This is the critical correction from the previous implementation. Transition cells only need data from the **face neighbor** and the chunk's own boundary voxels. The 13 sample points all lie within the chunk boundary and its immediate face neighbor. Edge/corner neighbor data is NOT required for transition cells.

**Files to modify**:
- `VoxelCPUSmoothMesher.h/cpp` — verify `GetDensityAt()` can access boundary+1 voxel positions

### Phase B: Transition Cell Tables (20 min)

**Problem**: Verify our `TransvoxelTables.h/cpp` tables match Eric Lengyel's official tables exactly.

**Tasks**:

1. **Compare with official source**: Cross-reference against https://github.com/EricLengyel/Transvoxel/blob/main/Transvoxel.cpp

2. **Required tables**:
   - `transitionCellClass[512]` — maps 9-bit case code to equivalence class (high bit = inverse/flip winding)
   - `transitionCellData[56]` — vertex count (high nibble) and triangle count (low nibble) + triangle vertex indices (groups of 3, up to 36 bytes)
   - `transitionVertexData[56][12]` — 16-bit codes encoding which edge each vertex lies on and the endpoint corner indices

3. **Verify table structure matches our code's expectations**: The `TransitionCellData` struct should have:
   ```cpp
   struct FTransitionCellData
   {
       uint8 GeometryCounts; // High nibble = vertex count, low nibble = triangle count
       uint8 VertexIndex[36]; // Triangle indices (groups of 3)
       
       int32 GetVertexCount() const { return GeometryCounts >> 4; }
       int32 GetTriangleCount() const { return GeometryCounts & 0x0F; }
   };
   ```

4. **Verify transition vertex data encoding**: Each 16-bit value encodes:
   - Low byte, low nibble: first endpoint corner index (0-12)
   - Low byte, high nibble: second endpoint corner index (0-12)
   - High byte: reuse/direction code (for vertex sharing optimization; can ignore for initial implementation)

**Files to modify**:
- `TransvoxelTables.h/cpp` — verify or replace with official tables

### Phase C: Transition Cell Processing (2 hours)

**This is the core implementation.** Replace the existing broken `ProcessTransitionCell()` with a corrected version.

**Tasks**:

#### C.1: Transition Cell Sample Position Mapping

For each face direction (±X, ±Y, ±Z), define how the 13 transition cell sample points map to voxel coordinates. The transition cell lies on a 2D face of the chunk boundary.

For the **+X face** (as an example), the transition cell at position `(cellY, cellZ)` in the face grid has sample points:

```
High-res face (3×3 grid, 9 points):
  Point 0: corner (MaxX, cellY*Stride,           cellZ*Stride)
  Point 1: edge   (MaxX, cellY*Stride + Stride/2, cellZ*Stride)
  Point 2: corner (MaxX, cellY*Stride + Stride,   cellZ*Stride)
  Point 3: edge   (MaxX, cellY*Stride,            cellZ*Stride + Stride/2)
  Point 4: center (MaxX, cellY*Stride + Stride/2, cellZ*Stride + Stride/2)
  Point 5: edge   (MaxX, cellY*Stride + Stride,   cellZ*Stride + Stride/2)
  Point 6: corner (MaxX, cellY*Stride,            cellZ*Stride + Stride)
  Point 7: edge   (MaxX, cellY*Stride + Stride/2, cellZ*Stride + Stride)
  Point 8: corner (MaxX, cellY*Stride + Stride,   cellZ*Stride + Stride)

Low-res face (4 points, same density as corners 0,2,6,8):
  Point 9:  = Point 0 position, pushed outward by TransitionCellWidth
  Point A:  = Point 2 position, pushed outward by TransitionCellWidth  
  Point B:  = Point 6 position, pushed outward by TransitionCellWidth
  Point C:  = Point 8 position, pushed outward by TransitionCellWidth
```

**Critical**: The low-res points (9-C) sample at the **same voxel positions** as corners 0,2,6,8 but their 3D positions are offset outward (into the neighboring chunk's space) by a configurable amount. Lengyel recommends 0.5 × cell width.

**IMPORTANT**: The numbering above must be transformed per-face. Create a mapping function:
```cpp
FVector3f GetTransitionSamplePosition(int32 SampleIndex, int32 FaceIndex, 
    int32 CellU, int32 CellV, int32 Stride, int32 ChunkSize);
```

#### C.2: Case Code Construction

Build the 9-bit case code from the 9 high-res samples:

```cpp
uint16 CaseCode = 0;
// Bit ordering per Lengyel's corrected table indexing:
// Sample 0 → bit 0 (0x001)
// Sample 1 → bit 1 (0x002)  
// Sample 2 → bit 2 (0x004)
// Sample 3 → bit 3 (0x008)
// Sample 4 → bit 4 (0x010) — but see CRITICAL NOTE
// ... etc.

// CRITICAL: The bit assignment follows this spatial layout:
// 0x040  0x020  0x010
// 0x080  0x100  0x008
// 0x001  0x002  0x004

for (int32 i = 0; i < 9; i++)
{
    float Density = GetDensityAt(SamplePositions[i]);
    if (Density >= IsoLevel) // Inside surface
    {
        CaseCode |= (1 << SampleBitIndex[i]);
    }
}
```

Where `SampleBitIndex` maps logical sample index to the correct bit position:
```cpp
// Maps sample index (0-8) to bit position for case code
static const int32 SampleBitIndex[9] = {
    0,  // Sample 0 → bit 0
    1,  // Sample 1 → bit 1
    2,  // Sample 2 → bit 2
    7,  // Sample 3 → bit 7
    8,  // Sample 4 → bit 8 (center, highest bit)
    3,  // Sample 5 → bit 3
    6,  // Sample 6 → bit 6
    5,  // Sample 7 → bit 5
    4,  // Sample 8 → bit 4
};
```

> **NOTE**: Verify this mapping against the official tables. This is the #1 source of bugs. The mapping depends on how you number the 9 sample points relative to the face orientation.

#### C.3: Triangle Generation

```cpp
if (CaseCode == 0 || CaseCode == 511) return; // No surface crossing

uint8 CellClass = transitionCellClass[CaseCode];
bool bFlipWinding = (CellClass & 0x80) != 0;
CellClass &= 0x7F;

const FTransitionCellData& CellData = transitionCellData[CellClass];
int32 VertexCount = CellData.GetVertexCount();
int32 TriangleCount = CellData.GetTriangleCount();

// Generate vertices
TArray<FVector3f> CellVertices;
TArray<FVector3f> CellNormals;
CellVertices.SetNum(VertexCount);
CellNormals.SetNum(VertexCount);

for (int32 i = 0; i < VertexCount; i++)
{
    uint16 VertexData = transitionVertexData[CellClass][i];
    
    // Low byte contains endpoint indices
    int32 EndpointA = VertexData & 0x0F;        // Low nibble
    int32 EndpointB = (VertexData >> 4) & 0x0F; // High nibble of low byte
    
    // Get sample positions and densities for the two endpoints
    FVector3f PosA = GetTransitionSamplePosition3D(EndpointA, ...);
    FVector3f PosB = GetTransitionSamplePosition3D(EndpointB, ...);
    float DensityA = GetDensityAtPosition(EndpointA, ...);
    float DensityB = GetDensityAtPosition(EndpointB, ...);
    
    // Interpolate
    float t = FMath::Clamp(
        (IsoLevel - DensityA) / (DensityB - DensityA), 
        0.001f, 0.999f
    );
    
    CellVertices[i] = PosA + (PosB - PosA) * t;
    CellNormals[i] = CalculateGradientNormal(CellVertices[i]);
}

// Generate triangles
for (int32 t = 0; t < TriangleCount; t++)
{
    int32 i0 = CellData.VertexIndex[t * 3 + 0];
    int32 i1 = CellData.VertexIndex[t * 3 + 1];
    int32 i2 = CellData.VertexIndex[t * 3 + 2];
    
    if (bFlipWinding)
    {
        Swap(i1, i2);
    }
    
    // Add triangle to mesh output
    AddTriangle(CellVertices[i0], CellVertices[i1], CellVertices[i2],
                CellNormals[i0], CellNormals[i1], CellNormals[i2]);
}
```

#### C.4: Face Iteration

For each transition face, iterate over the 2D grid of cells on that face:

```cpp
void ProcessTransitionFace(int32 FaceIndex, int32 LODLevel)
{
    const int32 Stride = 1 << LODLevel;
    const int32 CellCount = (ChunkSize - 1) / Stride; // e.g., 31 at LOD 0
    
    // Transition cells iterate in pairs (2 high-res cells = 1 low-res cell)
    // So we step by 2 in the face's UV space
    for (int32 CellU = 0; CellU < CellCount; CellU += 2)
    {
        for (int32 CellV = 0; CellV < CellCount; CellV += 2)
        {
            ProcessTransitionCell(FaceIndex, CellU, CellV, LODLevel);
        }
    }
}
```

> **Wait** — this is wrong for the common case. If our chunk is at LOD 0 (stride 1) and the neighbor is at LOD 1 (stride 2), each transition cell spans 2×2 high-res cells to match 1 low-res cell. So we iterate over pairs. But at LOD 0, the cell count is 31, so we'd have 15 transition cells along each axis (31/2 = 15.5, handle the last cell as a partial).

**Corrected iteration**:
```cpp
// Number of transition cells = floor(CellCount / 2)
const int32 TransCellCount = CellCount / 2;

for (int32 u = 0; u < TransCellCount; u++)
{
    for (int32 v = 0; v < TransCellCount; v++)
    {
        ProcessTransitionCell(FaceIndex, u, v, LODLevel);
    }
}
```

### Phase D: Secondary Vertex Positions (1 hour)

**Problem**: Regular Marching Cubes vertices near transition boundaries must be pushed inward to prevent overlap with transition cell geometry.

**Approach** (following Godot Voxel Tools):

1. **During regular cell processing**, compute both a **primary position** (standard MC vertex position) and a **secondary position** (pushed inward from chunk boundary).

2. **Store both positions per vertex**: Add secondary position data to the vertex format. In our case, since we use `FVoxelLocalVertex` (32 bytes) and can't easily expand it, we have two options:

   **Option A — Material-based approach (Recommended for our architecture)**:
   - Store a `cell_border_mask` (6 bits, one per face) and `vertex_border_mask` (6 bits) packed into an existing vertex channel.
   - Compute the secondary position offset in the material/vertex shader.
   - The transition mask is passed as a per-chunk parameter via MPC (already used for LOD morphing).

   **Option B — CPU-side approach (Simpler, recommended for initial implementation)**:
   - When `bUseTransvoxel = true` and a face has a transition, directly compute the final vertex position on CPU, moving boundary vertices inward.
   - This avoids shader changes but means the mesh must be regenerated when neighbor LOD changes (which already happens in our system).

**Recommended: Option B for initial implementation**, then Option A as optimization.

**Secondary position computation** (Lengyel's formula):
```cpp
// For each vertex in cells touching the boundary:
FVector3f GetSecondaryPosition(FVector3f PrimaryPos, int32 FaceIndex, 
    float CellSize, float ChunkWorldSize)
{
    // Push inward by half a cell width
    const float PushDistance = CellSize * 0.5f;
    FVector3f Offset = FVector3f::ZeroVector;
    
    // Determine which axis and direction to push based on face
    switch (FaceIndex)
    {
        case 0: Offset.X = PushDistance; break;  // -X face: push +X
        case 1: Offset.X = -PushDistance; break; // +X face: push -X
        case 2: Offset.Y = PushDistance; break;  // -Y face: push +Y
        case 3: Offset.Y = -PushDistance; break; // +Y face: push -Y
        case 4: Offset.Z = PushDistance; break;  // -Z face: push +Z
        case 5: Offset.Z = -PushDistance; break; // +Z face: push -Z
    }
    
    return PrimaryPos + Offset;
}
```

**But it's more nuanced than that.** The actual formula from Lengyel's dissertation involves a position-dependent scaling so only vertices near the boundary are affected, and the effect diminishes toward the interior:

```cpp
FVector3f ComputeSecondaryOffset(FVector3f NormalizedPos, int32 FaceIndex, 
    int32 BlockSize)
{
    // NormalizedPos is in [0, BlockSize] range within the chunk
    FVector3f Delta = FVector3f::ZeroVector;
    
    // Per Lengyel: delta = (1 - t/N) where t is distance from boundary
    // and N is transition width (typically 1 cell)
    float t, N = 1.0f; // transition width in cells
    
    // Calculate offset for each axis where this face has a transition
    // Only vertices in the outermost cell get the full offset
    // Example for +X face (face index 1):
    t = (float)(BlockSize - 1) - NormalizedPos.X; // distance from +X boundary
    if (t < N) 
    {
        Delta.X = -(N - t); // push inward (toward -X)
    }
    
    return Delta;
}
```

### Phase E: Regular Cell Boundary Exclusion (30 min)

**Problem**: Regular Marching Cubes cells on the transition boundary must NOT generate geometry on the boundary face itself — that geometry is replaced by the transition cells.

**Tasks**:

1. **Skip boundary cells on transition faces**: When processing regular MC cells, if a cell is on the outermost layer of the chunk along a transition face, either:
   - Skip the cell entirely (simplest, but loses interior geometry), OR
   - Process the cell but shrink it to half-width (the interior half only)

2. **Recommended approach**: Process the cell normally but restrict it to the interior half. The transition cell handles the exterior half.

```cpp
// In the main MC loop:
for (int32 X = 0; X < CellCount; X++)
{
    for (int32 Y = 0; Y < CellCount; Y++)
    {
        for (int32 Z = 0; Z < CellCount; Z++)
        {
            // Check if this is a boundary cell on a transition face
            bool bIsTransitionBoundary = false;
            if (X == 0 && (TransitionFaces & TRANSITION_XNEG)) bIsTransitionBoundary = true;
            if (X == CellCount-1 && (TransitionFaces & TRANSITION_XPOS)) bIsTransitionBoundary = true;
            // ... same for Y, Z
            
            if (bIsTransitionBoundary)
            {
                // Process with half-cell (interior portion only)
                ProcessRegularCellHalf(X, Y, Z, FaceIndex);
            }
            else
            {
                ProcessCubeLOD(X, Y, Z); // Standard MC
            }
        }
    }
}
```

**Simpler alternative** (used by Godot Voxel Tools): Don't exclude anything. Instead, use the secondary vertex position approach (Phase D) to push regular vertices inward, creating a gap that the transition cells fill. This is cleaner and avoids the half-cell complexity.

**Recommendation**: Use the secondary position approach. Regular cells are processed normally, their boundary vertices get secondary positions, and transition cells fill the gap.

### Phase F: Integration & Testing (1 hour)

**Tasks**:

1. **Wire up the new `ProcessTransitionFace()` into the mesher**:
   ```cpp
   void FVoxelCPUSmoothMesher::GenerateMesh(const FVoxelMeshingRequest& Request, ...)
   {
       // ... existing MC processing ...
       
       if (Request.Config.bUseTransvoxel)
       {
           for (int32 Face = 0; Face < 6; Face++)
           {
               if (Request.TransitionFaces & (1 << Face))
               {
                   ProcessTransitionFace(Face, Request.LODLevel);
               }
           }
       }
   }
   ```

2. **Test with a simple 2-chunk scenario**:
   - Place two adjacent chunks, one at LOD 0, one at LOD 1
   - Verify no visible seams
   - Toggle wireframe to inspect transition cell geometry

3. **Test edge cases**:
   - Two adjacent transition faces (chunk corner where two lower-LOD neighbors meet)
   - Three adjacent transition faces (chunk at a corner)
   - LOD 0 → LOD 1, LOD 0 → LOD 2 (skip-LOD)
   - Flat terrain (many cells with no surface crossing)
   - Near-tangent surface (surface nearly parallel to boundary face — worst case for seams)

4. **Verify material/biome assignment**: Transition cell vertices need MaterialID and BiomeID. Use the same `GetDominantMaterialLOD()` approach as regular cells.

### Phase G: LOD 0 Special Case (30 min)

**Problem**: At LOD 0 (stride 1), the previous implementation had "fractional sample position" bugs.

**Root cause**: At LOD 0, the transition cell's 9 high-res samples map directly to integer voxel positions. The 5 "mid-edge" samples (points 1, 3, 4, 5, 7) fall on half-integer positions. Since our voxel data is on integer positions, we need to **interpolate** (bilinear/trilinear) for these mid-edge samples.

**Fix**:
```cpp
float SampleDensityAtFractional(float X, float Y, float Z)
{
    // Trilinear interpolation for fractional positions
    int32 X0 = FMath::FloorToInt32(X);
    int32 Y0 = FMath::FloorToInt32(Y);
    int32 Z0 = FMath::FloorToInt32(Z);
    
    float fx = X - X0;
    float fy = Y - Y0;
    float fz = Z - Z0;
    
    // If position is integer, just return the direct sample
    if (FMath::IsNearlyZero(fx) && FMath::IsNearlyZero(fy) && FMath::IsNearlyZero(fz))
    {
        return GetDensityAt(X0, Y0, Z0);
    }
    
    // Trilinear interpolation
    float d000 = GetDensityAt(X0, Y0, Z0);
    float d100 = GetDensityAt(X0+1, Y0, Z0);
    float d010 = GetDensityAt(X0, Y0+1, Z0);
    float d110 = GetDensityAt(X0+1, Y0+1, Z0);
    float d001 = GetDensityAt(X0, Y0, Z0+1);
    float d101 = GetDensityAt(X0+1, Y0, Z0+1);
    float d011 = GetDensityAt(X0, Y0+1, Z0+1);
    float d111 = GetDensityAt(X0+1, Y0+1, Z0+1);
    
    return FMath::Lerp(
        FMath::Lerp(FMath::Lerp(d000, d100, fx), FMath::Lerp(d010, d110, fx), fy),
        FMath::Lerp(FMath::Lerp(d001, d101, fx), FMath::Lerp(d011, d111, fx), fy),
        fz
    );
}
```

**Wait — this is actually wrong for Transvoxel!** The 9 high-res samples must be at **actual voxel positions**, not interpolated positions. At LOD 0, each transition cell spans 2 regular cells, so the 9 sample points ARE at integer positions:

```
For a transition cell spanning cells (0,0) and (1,1) on the face:
  Point 0 = (0, 0)  — integer
  Point 1 = (1, 0)  — integer (midpoint of edge 0-2)
  Point 2 = (2, 0)  — integer
  Point 3 = (0, 1)  — integer
  Point 4 = (1, 1)  — integer (center)
  Point 5 = (2, 1)  — integer
  Point 6 = (0, 2)  — integer
  Point 7 = (1, 2)  — integer
  Point 8 = (2, 2)  — integer
```

So at LOD 0, **all 9 samples are at integer positions** and no interpolation is needed. The previous bug was likely due to incorrect coordinate mapping, not fractional positions. At LOD 1+ with stride 2+, the mid-edge samples may again be at stride/2 positions which are still integer at LOD 1 (stride 2, so midpoint = 1).

**The key insight**: Transition cells always span **2 regular cells** in each axis on the high-res side. At any LOD level, if the stride is `S`, the 9 sample points are spaced at `S` intervals (corners) and `S/2` intervals (midpoints). At LOD 0 (S=1), midpoints are at 0.5 — **this IS a fractional position!**

**Corrected understanding**: At LOD 0, midpoint samples DO fall at half-integer positions. Use trilinear interpolation for these. At LOD 1 (S=2), midpoints are at integer positions. At LOD 2 (S=4), midpoints are at S/2=2 intervals, also integer.

**So the `SampleDensityAtFractional()` function IS needed for LOD 0.**

---

## 4. Verification Checklist

- [ ] `TransvoxelTables.h/cpp` matches official Lengyel tables exactly
- [ ] Sample point numbering matches the table's expected bit order (see C.2)
- [ ] Case code 0 and 511 are skipped (no surface crossing)
- [ ] Winding order is reversed when `transitionCellClass[code] & 0x80`
- [ ] Edge interpolation t-value is clamped to `[0.001, 0.999]`
- [ ] Low-res points (9-12) sample at same voxel positions as corners (0,2,6,8)
- [ ] Transition cell geometry occupies only the boundary half of the cell
- [ ] Regular mesh vertices near boundaries are pushed inward (secondary positions)
- [ ] LOD 0 midpoint samples use trilinear interpolation
- [ ] Normals are computed via gradient for transition cell vertices
- [ ] MaterialID/BiomeID assigned to transition cell vertices
- [ ] Works for all 6 face directions
- [ ] Works when 2+ faces have transitions simultaneously
- [ ] No Z-fighting between transition and regular geometry

---

## 5. Key References

- **Official tables**: https://github.com/EricLengyel/Transvoxel/blob/main/Transvoxel.cpp
- **Official documentation**: https://transvoxel.org/
- **Lengyel's dissertation**: https://transvoxel.org/Lengyel-VoxelTerrain.pdf (Section 4.3)
- **Godot Voxel Tools** (most mature OSS implementation): https://github.com/Zylann/godot_voxel/blob/master/meshers/transvoxel/transvoxel.cpp
- **stoyannk/voxels** (clean C++ reference): https://github.com/stoyannk/voxels (TransVoxelImpl.cpp)
- **Gnurfos/transvoxel_rs** (Rust, very clean): https://github.com/Gnurfos/transvoxel_rs
- **Sample point numbering fix** (GameDev.net, with Lengyel's own correction): https://gamedev.net/forums/topic/607301-voxel-algorithm/
- **BinaryConstruct XNA implementation** (lessons learned): https://www.binaryconstruct.com/posts/transvoxel-xna/

---

## 6. Estimated Time

| Phase | Time | Description |
|-------|------|-------------|
| A | 30 min | Neighbor data verification |
| B | 20 min | Table verification |
| C | 2 hours | Core transition cell processing |
| D | 1 hour | Secondary vertex positions |
| E | 30 min | Boundary cell handling |
| F | 1 hour | Integration & testing |
| G | 30 min | LOD 0 special case |
| **Total** | **~6 hours** | |

---

## 7. Configuration

After implementation, the recommended default configuration:

```cpp
// In UVoxelWorldConfiguration or FVoxelMeshingConfig:
Config.bUseTransvoxel = true;      // Enable Transvoxel
Config.bGenerateSkirts = false;     // Disable skirts (redundant with Transvoxel)
Config.TransitionCellWidth = 0.5f;  // Half-cell width for transition geometry
Config.EdgeClampMargin = 0.001f;    // Prevent degenerate triangles
```

---

## 8. Debug Aids

Add these debug visualization options:

1. **`bShowTransitionCells`** — render transition cell geometry in a distinct color (wireframe overlay)
2. **`bShowSecondaryPositions`** — draw lines from primary to secondary vertex positions
3. **`bShowTransitionFaces`** — highlight which chunk faces have active transitions
4. **Log transition cell counts** — per-chunk count of transition cells generated, to verify reasonable numbers (e.g., for a 32³ chunk at LOD 0, expect ~30 transition cells per active face for typical terrain)
