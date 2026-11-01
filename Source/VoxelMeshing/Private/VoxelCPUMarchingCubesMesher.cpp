// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelCrossingGradient.h"
#include "VoxelMeshingVoxelSampler.h"
#include "VoxelDigitalLine.h"
#include "VoxelMeshing.h"
#include "MarchingCubesTables.h"
#include "TransvoxelTables.h"
#include "HAL/IConsoleManager.h"

// Note: MarchingCubes meshing uses triplanar blending, so FaceType is not needed.
// UV1.x stores MaterialID, UV1.y is reserved (set to 0).

// LOD-seam geomorph (Transvoxel "secondary positions", baked): near a face that borders a
// coarser neighbour, ramp the fine surface toward the coarse-LOD surface so the boundary
// contour meets the coarse neighbour with no vertical step. Baked at mesh time (the chunk is
// re-meshed when its own or a neighbour's LOD changes), so it needs no vertex-format or shader
// change on the CPU path. See Documentation/GEOMORPH_IMPLEMENTATION_PLAN.md.
TAutoConsoleVariable<float> CVarMCBoundaryMorphWidth(
	TEXT("voxel.MCBoundaryMorphWidth"),
	2.0f,
	TEXT("MC LOD-seam geomorph ramp width, in coarse cells. Larger = gentler ramp, more fine detail traded."),
	ECVF_Default);

/**
 * Vertex normals of strided cells (G61, WORLD_MODES_REVIEW_PLAN.md §14.1 and §20.9; Dual Contouring's G44 in §20.8).
 *
 * Generated density saturates a voxel either side of the surface (SignedDistanceToDensity), so the stride-scale gradient
 * (CalculateGradientNormalLOD: taps Stride voxels either side) reads 0 or 255 on every axis the surface crosses within
 * them: at stride 4 each component becomes a sign and the normal snaps toward the nearest of the 26 cube directions (LOD 2
 * leaning toward the cube diagonals on a planet, snapped slopes on a heightmap). Every LOD's data is at voxel resolution,
 * so an edge vertex's normal at stride > 1 is LOD 0's at the voxel crossing on its edge nearest it
 * (CalculateEdgeShadingNormal).
 *
 * Vertex positions at stride > 1 (G80, WORLD_MODES_REVIEW_PLAN.md §23.16, voxel.Mesh.VoxelLODCrossings): interpolated between the two samples a stride
 * apart, the same saturation put a strided edge's vertex toward the edge's middle by where the surface crosses the
 * strided grid, so LOD 1 and 2 were terraced a stride apart (ribs in the terrain's own shadows under a low sun). The
 * vertex is now that voxel crossing (FindEdgeVoxelCrossing), a function of the edge, so regular cells, seam frames and
 * the transition ribbon still agree on every shared edge; the boundary geomorph's target follows the coarse cells' new
 * vertices (ComputeCoarseCrossingsAlongAxis).
 *
 * Both switches (voxel.Mesh.VoxelShadingNormals, voxel.Mesh.VoxelLODCrossings) are read through VoxelCrossingGradient's
 * predicates, shared with Dual Contouring, and the two meshers' crossing rules live side by side in VoxelLODCrossing
 * (VoxelCrossingGradient.h; code review CR2, MESH-10 and MESH-11).
 */

FVoxelCPUMarchingCubesMesher::FVoxelCPUMarchingCubesMesher()
{
}

FVoxelCPUMarchingCubesMesher::~FVoxelCPUMarchingCubesMesher()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelCPUMarchingCubesMesher::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	UE_LOG(LogVoxelMeshing, Log, TEXT("CPU MarchingCubes Mesher initialized"));
	bIsInitialized = true;
}

void FVoxelCPUMarchingCubesMesher::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	bIsInitialized = false;
	UE_LOG(LogVoxelMeshing, Log, TEXT("CPU MarchingCubes Mesher shutdown"));
}

bool FVoxelCPUMarchingCubesMesher::IsInitialized() const
{
	return bIsInitialized;
}

bool FVoxelCPUMarchingCubesMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData)
{
	FVoxelMeshingStats Stats;
	return GenerateMeshCPU(Request, OutMeshData, Stats);
}

bool FVoxelCPUMarchingCubesMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("CPU MarchingCubes Mesher not initialized"));
		return false;
	}

	if (!Request.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid meshing request"));
		return false;
	}

	const double StartTime = FPlatformTime::Seconds();

	// Reset output
	OutMeshData.Reset();
	OutStats = FVoxelMeshingStats();

	const int32 ChunkSize = Request.ChunkSize;

	// Calculate LOD stride - each LOD level doubles the stride
	// LOD 0 = stride 1 (full detail), LOD 1 = stride 2, LOD 2 = stride 4, etc.
	const int32 LODLevel = FMath::Clamp(Request.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;  // 2^LODLevel

	// Number of cubes to process at this LOD level
	const int32 LODChunkSize = ChunkSize / Stride;

	UE_LOG(LogVoxelMeshing, Log, TEXT("MarchingCubes meshing chunk (%d,%d,%d) at LOD %d (stride %d, cubes %d^3)"),
		Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
		LODLevel, Stride, LODChunkSize);

	// Pre-allocate arrays (estimate based on typical terrain, scaled for LOD)
	const int32 EstimatedTriangles = LODChunkSize * LODChunkSize * 2;
	OutMeshData.Positions.Reserve(EstimatedTriangles * 3);
	OutMeshData.Normals.Reserve(EstimatedTriangles * 3);
	OutMeshData.UVs.Reserve(EstimatedTriangles * 3);
	OutMeshData.UV1s.Reserve(EstimatedTriangles * 3);
	OutMeshData.Colors.Reserve(EstimatedTriangles * 3);
	OutMeshData.Indices.Reserve(EstimatedTriangles * 3);

	uint32 TriangleCount = 0;
	uint32 SolidVoxels = 0;

	// Count solid voxels (sampled at LOD stride)
	for (int32 Z = 0; Z < ChunkSize; Z += Stride)
	{
		for (int32 Y = 0; Y < ChunkSize; Y += Stride)
		{
			for (int32 X = 0; X < ChunkSize; X += Stride)
			{
				if (!Request.GetVoxel(X, Y, Z).IsAir())
				{
					SolidVoxels++;
				}
			}
		}
	}

	// Seam-ownership (SEAM_OWNERSHIP_ARCHITECTURE.md §2.1, P3): the Interior domain meshes only
	// cells with zero neighbor dependence. MC cells sample their own 8 corners plus gradient
	// normals reaching ±stride, so excluding the FIRST and LAST cell layer per axis leaves every
	// remaining cell (corners AND normal taps) entirely inside own voxel data — no clamping, no
	// neighbor slices. The excluded two-layer band per face (plus the transvoxel ribbon and the
	// boundary geomorph at mixed LOD) is produced by the single-owner seam jobs.
	const bool bInteriorDomain = (Request.MeshCellDomain == EVoxelMeshCellDomain::Interior);

	// Get transition face mask for Transvoxel (baked neighbor LODs — a launch-time snapshot, the
	// legacy boundary mechanism; inapplicable to the interior-only pass)
	const uint8 TransitionMask = (Config.bUseTransvoxel && !bInteriorDomain) ? GetTransitionFaces(Request) : 0;
	const bool bHasTransitions = TransitionMask != 0;

	if (bHasTransitions)
	{
		UE_LOG(LogVoxelMeshing, Verbose, TEXT("Chunk has transition faces: 0x%02X"), TransitionMask);
	}

	// TWO-PASS HYBRID approach for Transvoxel:
	//
	// Pass 1: Generate transition cells at all aligned boundary positions. Track which
	//         positions produced non-empty geometry (surface crosses the transition face).
	// Pass 2: Generate MC for all cells EXCEPT those covered by a non-empty transition
	//         cell. This ensures:
	//   - No missing geometry: empty transition cells (case 0) get MC fallback
	//   - No overlap where transition cells are active: clean outer-edge matching
	//   - Small T-junctions at the inner edge of transition strips (per Lengyel)
	//
	// The transition cell's outer edge matches the coarser neighbor's MC grid exactly
	// (same densities from shared neighbor data, same InterpolateEdge formula).
	// Boundary MC cells produce stride-1 resolution vertices that DON'T match the
	// coarser MC's stride-2 vertices — this is why we must skip them where transition
	// cells are active.

	// Pass 1: Generate a zero-thickness transition ribbon at every coarse-aligned
	// boundary position, for each active face. Corner cells (on two active faces at
	// once) get a separate flat ribbon per face — the ribbons live in different boundary
	// planes (e.g. +X in the X plane, +Y in the Y plane) so they do not overlap, and
	// each reduces its own face's fine contour to its coarser neighbour. No corner-skip
	// is needed (the old thick cells skipped corners to avoid overlapping volumes).
	if (Config.bUseTransvoxel && bHasTransitions)
	{
		for (int32 Face = 0; Face < 6; Face++)
		{
			if (!(TransitionMask & (1 << Face)))
				continue;

			const int32 DepthAxis = Face / 2;
			const int32 BoundaryPos = (Face % 2 == 0) ? 0 : (ChunkSize - Stride);

			const int32 NeighborLOD = Request.NeighborLODLevels[Face];
			const int32 CoarserStride = (NeighborLOD > Request.LODLevel)
				? (1 << NeighborLOD) : Stride;

			for (int32 FP2 = 0; FP2 < ChunkSize; FP2 += CoarserStride)
			{
				for (int32 FP1 = 0; FP1 < ChunkSize; FP1 += CoarserStride)
				{
					int32 CellX, CellY, CellZ;
					switch (DepthAxis)
					{
					case 0: CellX = BoundaryPos; CellY = FP1; CellZ = FP2; break;
					case 1: CellX = FP1; CellY = BoundaryPos; CellZ = FP2; break;
					default: CellX = FP1; CellY = FP2; CellZ = BoundaryPos; break;
					}

					ProcessTransitionCell(
						Request, CellX, CellY, CellZ, CoarserStride, Face, OutMeshData, TriangleCount);
				}
			}
		}
	}

	// Pass 2: Generate regular MC for ALL cells, including the boundary rows. With the
	// zero-thickness transition design the finer chunk's MC runs right up to the
	// boundary and produces the fine boundary contour; the Pass-1 transition ribbons
	// then reduce that contour to the coarser neighbour's grid IN the boundary plane.
	// (Previously the boundary slab was skipped and owned by a thick transition cell,
	// which left holes on the fine-interior side and depth-offset slivers near coarse
	// corners — see LOD_SEAM_INVESTIGATION.md.)
	// Interior domain: cells [Stride, ChunkSize-2*Stride] per axis (exclude the first and last
	// cell layer — the seam-job band). Full domain: all cells [0, ChunkSize).
	const int32 CellLo = bInteriorDomain ? Stride : 0;
	const int32 CellHiEx = bInteriorDomain ? (ChunkSize - Stride) : ChunkSize;
	for (int32 Z = CellLo; Z < CellHiEx; Z += Stride)
	{
		for (int32 Y = CellLo; Y < CellHiEx; Y += Stride)
		{
			for (int32 X = CellLo; X < CellHiEx; X += Stride)
			{
				ProcessCubeLOD(Request, X, Y, Z, Stride, OutMeshData, TriangleCount, TransitionMask);
			}
		}
	}

	// Calculate stats
	const double EndTime = FPlatformTime::Seconds();
	OutStats.VertexCount = OutMeshData.Positions.Num();
	OutStats.IndexCount = OutMeshData.Indices.Num();
	OutStats.FaceCount = TriangleCount;
	OutStats.SolidVoxelCount = SolidVoxels;
	OutStats.CulledFaceCount = 0;
	OutStats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("MarchingCubes meshing complete: %d verts, %d tris, %.2fms"),
		OutStats.VertexCount, TriangleCount, OutStats.GenerationTimeMs);

	return true;
}

void FVoxelCPUMarchingCubesMesher::ProcessCubeLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount,
	uint8 TransitionMask)
{
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;

	// Lengyel's corner ordering — matches the Transvoxel regular MC tables
	// so that interior cell triangulations are compatible with transition cells.
	// 0=(0,0,0), 1=(1,0,0), 2=(0,1,0), 3=(1,1,0),
	// 4=(0,0,1), 5=(1,0,1), 6=(0,1,1), 7=(1,1,1)
	static const FIntVector LengyelCornerOffsets[8] = {
		FIntVector(0, 0, 0), FIntVector(1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(1, 1, 0),
		FIntVector(0, 0, 1), FIntVector(1, 0, 1),
		FIntVector(0, 1, 1), FIntVector(1, 1, 1),
	};

	// Sample density and compute world positions at 8 cube corners
	float CornerDensities[8];
	FVector3f CornerPositions[8];
	for (int32 i = 0; i < 8; i++)
	{
		const FIntVector& Offset = LengyelCornerOffsets[i];
		const int32 SampleX = X + Offset.X * Stride;
		const int32 SampleY = Y + Offset.Y * Stride;
		const int32 SampleZ = Z + Offset.Z * Stride;
		CornerDensities[i] = GetDensityAt(Request, SampleX, SampleY, SampleZ);
		CornerPositions[i] = FVector3f(
			static_cast<float>(SampleX) * VoxelSize,
			static_cast<float>(SampleY) * VoxelSize,
			static_cast<float>(SampleZ) * VoxelSize
		);
	}

	// Build case index using Lengyel's corner ordering.
	// Bits mark SOLID corners (density >= IsoLevel) in our convention.
	uint16 SolidMask = 0;
	for (int32 i = 0; i < 8; i++)
	{
		if (CornerDensities[i] >= IsoLevel)
		{
			SolidMask |= (1 << i);
		}
	}

	// Lengyel's tables use opposite polarity: bit set = OUTSIDE the surface.
	// Complement to convert our solid-mask to Lengyel's outside-mask.
	const uint16 CaseIndex = (~SolidMask) & 0xFF;

	// Look up equivalence class and cell data
	const uint8 CellClass = TransvoxelTables::RegularCellClass[CaseIndex];
	const TransvoxelTables::FRegularCellData& CellData = TransvoxelTables::RegularCellData[CellClass];
	const int32 TriangleCount = CellData.GetTriangleCount();

	// Early out if no geometry (fully inside or fully outside)
	if (TriangleCount == 0)
	{
		return;
	}

	// Convert SolidMask (Lengyel corner order, bits = solid) to classic MC ordering
	// for material/biome lookups. Classic corners 2↔3 and 6↔7 are swapped.
	const uint8 ClassicCubeIndex = static_cast<uint8>(
		(SolidMask & 0x33) |           // bits 0,1,4,5 unchanged
		((SolidMask & 0x04) << 1) |    // Lengyel bit 2 → classic bit 3
		((SolidMask & 0x08) >> 1) |    // Lengyel bit 3 → classic bit 2
		((SolidMask & 0x40) << 1) |    // Lengyel bit 6 → classic bit 7
		((SolidMask & 0x80) >> 1)      // Lengyel bit 7 → classic bit 6
	);

	const uint8 MaterialID = GetDominantMaterialLOD(Request, X, Y, Z, Stride, ClassicCubeIndex);
	const uint8 BiomeID = GetDominantBiomeLOD(Request, X, Y, Z, Stride, ClassicCubeIndex);

	// Decode edge vertices from RegularVertexData.
	// Each uint16: low nibble = corner A, next nibble = corner B, high byte = reuse info.
	const int32 VertexCount = CellData.GetVertexCount();
	const uint16* VertexDataRow = TransvoxelTables::RegularVertexData[CaseIndex];
	FVector3f CellVertices[12];
	int32 VertexCorners[12][2];
	// G80: at a stride, the vertex at the voxel crossing on its edge (the point G61's normal reads).
	const bool bVoxelCrossings = VoxelCrossingGradient::UseVoxelLODCrossings(Stride);

	for (int32 i = 0; i < VertexCount; i++)
	{
		const uint16 VertexCode = VertexDataRow[i];
		const int32 CornerA = VertexCode & 0x0F;
		const int32 CornerB = (VertexCode >> 4) & 0x0F;
		VertexCorners[i][0] = CornerA;
		VertexCorners[i][1] = CornerB;

		if (CornerA == CornerB)
		{
			CellVertices[i] = CornerPositions[CornerA];
		}
		else
		{
			CellVertices[i] = InterpolateEdge(
				CornerDensities[CornerA], CornerDensities[CornerB],
				CornerPositions[CornerA], CornerPositions[CornerB],
				IsoLevel
			);
			FVector3f Crossing;
			if (bVoxelCrossings && FindEdgeVoxelCrossing(Request,
				FIntVector(X, Y, Z) + LengyelCornerOffsets[CornerA] * Stride, FIntVector(X, Y, Z) + LengyelCornerOffsets[CornerB] * Stride,
				CornerDensities[CornerA], CornerDensities[CornerB], Crossing))
			{
				CellVertices[i] = Crossing * VoxelSize;
			}
		}
	}

	// LOD-seam geomorph: for cubes in the boundary slab of an active transition face, ramp the
	// fine vertices toward the coarse-LOD surface so the boundary contour meets the coarser
	// neighbour with no vertical step (the ribbon then has ~zero height to span). No-op unless
	// this cube borders a coarser neighbour.
	if (TransitionMask != 0)
	{
		ApplyBoundaryMorph(Request, Stride, TransitionMask, CellVertices, VertexCount);
	}

	// Vertex normals. G61: at stride > 1, LOD 0's normal at the voxel crossing on the vertex's edge (a function of the
	// edge alone, so the geomorph does not change it); otherwise the stride-scale gradient at the vertex, as before. At
	// stride 1 the fitted gradient at the vertex (G62).
	const bool bVoxelShading = VoxelCrossingGradient::UseVoxelShadingNormals(Stride);
	FVector3f CellNormals[12];
	for (int32 i = 0; i < VertexCount; i++)
	{
		// Per component (FVector3f's operator/ multiplies by the reciprocal): the normals before G61 bit for bit.
		const FVector3f VertexVoxel(CellVertices[i].X / VoxelSize, CellVertices[i].Y / VoxelSize, CellVertices[i].Z / VoxelSize);
		if (bVoxelShading)
		{
			const FIntVector& OffsetA = LengyelCornerOffsets[VertexCorners[i][0]];
			const FIntVector& OffsetB = LengyelCornerOffsets[VertexCorners[i][1]];
			CellNormals[i] = CalculateEdgeShadingNormal(Request,
				FIntVector(X, Y, Z) + OffsetA * Stride, FIntVector(X, Y, Z) + OffsetB * Stride,
				CornerDensities[VertexCorners[i][0]], CornerDensities[VertexCorners[i][1]], VertexVoxel, Stride);
		}
		else
		{
			CellNormals[i] = (Stride == 1)
				? CalculateShadingNormal(Request, VertexVoxel.X, VertexVoxel.Y, VertexVoxel.Z)
				: CalculateGradientNormalLOD(Request, VertexVoxel.X, VertexVoxel.Y, VertexVoxel.Z, Stride);
		}
	}

	// Emit triangles using CellData triangle indices
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
	const FColor VertexColor(MaterialID, BiomeID, 0, 255);
	const FVector2f MaterialUV(static_cast<float>(MaterialID), 0.0f);

	for (int32 t = 0; t < TriangleCount; t++)
	{
		const int32 Idx0 = CellData.VertexIndex[t * 3 + 0];
		const int32 Idx1 = CellData.VertexIndex[t * 3 + 1];
		const int32 Idx2 = CellData.VertexIndex[t * 3 + 2];

		const FVector3f& P0 = CellVertices[Idx0];
		const FVector3f& P1 = CellVertices[Idx1];
		const FVector3f& P2 = CellVertices[Idx2];

		const FVector3f& N0 = CellNormals[Idx0];
		const FVector3f& N1 = CellNormals[Idx1];
		const FVector3f& N2 = CellNormals[Idx2];

		// Dominant-axis UV projection based on face normal
		const FVector3f FaceNormal = FVector3f::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
		const float AbsX = FMath::Abs(FaceNormal.X);
		const float AbsY = FMath::Abs(FaceNormal.Y);
		const float AbsZ = FMath::Abs(FaceNormal.Z);

		FVector2f UV0, UV1, UV2;

		if (AbsZ >= AbsX && AbsZ >= AbsY)
		{
			UV0 = FVector2f(P0.X * UVScale / VoxelSize, P0.Y * UVScale / VoxelSize);
			UV1 = FVector2f(P1.X * UVScale / VoxelSize, P1.Y * UVScale / VoxelSize);
			UV2 = FVector2f(P2.X * UVScale / VoxelSize, P2.Y * UVScale / VoxelSize);
		}
		else if (AbsX >= AbsY)
		{
			UV0 = FVector2f(P0.Y * UVScale / VoxelSize, P0.Z * UVScale / VoxelSize);
			UV1 = FVector2f(P1.Y * UVScale / VoxelSize, P1.Z * UVScale / VoxelSize);
			UV2 = FVector2f(P2.Y * UVScale / VoxelSize, P2.Z * UVScale / VoxelSize);
		}
		else
		{
			UV0 = FVector2f(P0.X * UVScale / VoxelSize, P0.Z * UVScale / VoxelSize);
			UV1 = FVector2f(P1.X * UVScale / VoxelSize, P1.Z * UVScale / VoxelSize);
			UV2 = FVector2f(P2.X * UVScale / VoxelSize, P2.Z * UVScale / VoxelSize);
		}

		const uint32 BaseVertex = OutMeshData.Positions.Num();

		OutMeshData.Positions.Add(P0);
		OutMeshData.Positions.Add(P1);
		OutMeshData.Positions.Add(P2);

		OutMeshData.Normals.Add(N0);
		OutMeshData.Normals.Add(N1);
		OutMeshData.Normals.Add(N2);

		OutMeshData.UVs.Add(UV0);
		OutMeshData.UVs.Add(UV1);
		OutMeshData.UVs.Add(UV2);

		OutMeshData.UV1s.Add(MaterialUV);
		OutMeshData.UV1s.Add(MaterialUV);
		OutMeshData.UV1s.Add(MaterialUV);

		OutMeshData.Colors.Add(VertexColor);
		OutMeshData.Colors.Add(VertexColor);
		OutMeshData.Colors.Add(VertexColor);

		OutMeshData.Indices.Add(BaseVertex + 0);
		OutMeshData.Indices.Add(BaseVertex + 1);
		OutMeshData.Indices.Add(BaseVertex + 2);

		OutTriangleCount++;
	}
}

float FVoxelCPUMarchingCubesMesher::GetDensityAt(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	return VoxelMeshingSampler::DensityAt(Request, X, Y, Z, VoxelMeshingSampler::MarchingCubesRule);
}

float FVoxelCPUMarchingCubesMesher::GetDensityAtTrilinear(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z) const
{
	// Trilinear interpolation for fractional voxel positions.
	// This is essential for Transvoxel mid-point samples that fall between voxels.

	// Get integer coordinates and fractional parts
	const int32 X0 = FMath::FloorToInt(X);
	const int32 Y0 = FMath::FloorToInt(Y);
	const int32 Z0 = FMath::FloorToInt(Z);

	const float FracX = X - static_cast<float>(X0);
	const float FracY = Y - static_cast<float>(Y0);
	const float FracZ = Z - static_cast<float>(Z0);

	// If on integer coordinates, skip interpolation
	if (FracX < KINDA_SMALL_NUMBER && FracY < KINDA_SMALL_NUMBER && FracZ < KINDA_SMALL_NUMBER)
	{
		return GetDensityAt(Request, X0, Y0, Z0);
	}

	// Sample the 8 corners of the cell containing this point
	const float D000 = GetDensityAt(Request, X0,     Y0,     Z0);
	const float D100 = GetDensityAt(Request, X0 + 1, Y0,     Z0);
	const float D010 = GetDensityAt(Request, X0,     Y0 + 1, Z0);
	const float D110 = GetDensityAt(Request, X0 + 1, Y0 + 1, Z0);
	const float D001 = GetDensityAt(Request, X0,     Y0,     Z0 + 1);
	const float D101 = GetDensityAt(Request, X0 + 1, Y0,     Z0 + 1);
	const float D011 = GetDensityAt(Request, X0,     Y0 + 1, Z0 + 1);
	const float D111 = GetDensityAt(Request, X0 + 1, Y0 + 1, Z0 + 1);

	// Trilinear interpolation
	// First interpolate along X
	const float D00 = FMath::Lerp(D000, D100, FracX);
	const float D10 = FMath::Lerp(D010, D110, FracX);
	const float D01 = FMath::Lerp(D001, D101, FracX);
	const float D11 = FMath::Lerp(D011, D111, FracX);

	// Then along Y
	const float D0 = FMath::Lerp(D00, D10, FracY);
	const float D1 = FMath::Lerp(D01, D11, FracY);

	// Finally along Z
	return FMath::Lerp(D0, D1, FracZ);
}

FVoxelData FVoxelCPUMarchingCubesMesher::GetVoxelAt(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	// The shared sampler under Marching Cubes' rule (VoxelMeshingVoxelSampler.h, MESH-09): a missing neighbour reads as the
	// nearest own voxel, edges and corners one voxel deep.
	return VoxelMeshingSampler::VoxelAt(Request, X, Y, Z, VoxelMeshingSampler::MarchingCubesRule);
}

FVector3f FVoxelCPUMarchingCubesMesher::InterpolateEdge(
	float d0, float d1,
	const FVector3f& p0, const FVector3f& p1,
	float IsoLevel) const
{
	// Avoid division by zero
	if (FMath::Abs(d1 - d0) < KINDA_SMALL_NUMBER)
	{
		return (p0 + p1) * 0.5f;
	}

	// Linear interpolation factor
	float t = (IsoLevel - d0) / (d1 - d0);
	t = FMath::Clamp(t, 0.0f, 1.0f);

	return p0 + (p1 - p0) * t;
}

bool FVoxelCPUMarchingCubesMesher::ComputeCoarseSurfaceAlongAxis(
	const FVoxelMeshingRequest& Request,
	const FVector3f& V, int32 Axis,
	int32 CoarserStride,
	float& OutCoord) const
{
	// Density sampled on the coarse (stride-E) lattice, trilinear between coarse grid points: the surface the coarser
	// neighbour sees, evaluated on the line along Axis through V. The two face-parallel coordinates are clamped to
	// [0, ChunkSize]: neighbour data is one plane deep, so a read more than a voxel past a chunk face returns clamped or
	// air densities and corrupts the coarse surface (worst on steep terrain: a large spurious morph, seam holes); at a
	// boundary the clamp samples the shared plane, what the neighbour sees, so the morph target equals its coarse
	// contour. Along Axis the search walks the coarse cells around V and returns the iso-crossing nearest it (caves and
	// several crossings: the closest). One search for every up (code review CR2, MESH-30).
	const float IsoLevel = Config.IsoLevel;
	const int32 E = FMath::Max(1, CoarserStride);
	const float Ef = static_cast<float>(E);
	const int32 ChunkSize = Request.ChunkSize;
	const int32 AxisB = (Axis + 1) % 3;
	const int32 AxisC = (Axis + 2) % 3;

	auto SampleCoarse = [&](float Aq) -> float
	{
		const int32 B0 = FMath::FloorToInt(V[AxisB] / Ef) * E;
		const int32 C0 = FMath::FloorToInt(V[AxisC] / Ef) * E;
		const int32 A0 = FMath::FloorToInt(Aq / Ef) * E;
		const float Fb = (V[AxisB] - static_cast<float>(B0)) / Ef;
		const float Fc = (V[AxisC] - static_cast<float>(C0)) / Ef;
		const float Fa = (Aq - static_cast<float>(A0)) / Ef;
		const int32 Ba = FMath::Clamp(B0, 0, ChunkSize);
		const int32 Bb = FMath::Clamp(B0 + E, 0, ChunkSize);
		const int32 Ca = FMath::Clamp(C0, 0, ChunkSize);
		const int32 Cb = FMath::Clamp(C0 + E, 0, ChunkSize);
		auto At = [&](int32 A, int32 B, int32 C)
		{
			FIntVector I;
			I[Axis] = A;
			I[AxisB] = B;
			I[AxisC] = C;
			return GetDensityAt(Request, I.X, I.Y, I.Z);
		};
		const float D0 = FMath::Lerp(FMath::Lerp(At(A0, Ba, Ca), At(A0, Bb, Ca), Fb), FMath::Lerp(At(A0, Ba, Cb), At(A0, Bb, Cb), Fb), Fc);
		const float D1 = FMath::Lerp(FMath::Lerp(At(A0 + E, Ba, Ca), At(A0 + E, Bb, Ca), Fb), FMath::Lerp(At(A0 + E, Ba, Cb), At(A0 + E, Bb, Cb), Fb), Fc);
		return FMath::Lerp(D0, D1, Fa);
	};

	const int32 SearchCells = 4;
	const float Near = V[Axis];
	const int32 ABase = FMath::FloorToInt(Near / Ef) * E;
	float BestDist = TNumericLimits<float>::Max();
	bool bFound = false;
	for (int32 k = -SearchCells; k < SearchCells; k++)
	{
		const float Aa = static_cast<float>(ABase + k * E);
		const float Ab = Aa + Ef;
		const float Da = SampleCoarse(Aa);
		const float Db = SampleCoarse(Ab);
		const float Denom = Db - Da;
		if ((Da >= IsoLevel) != (Db >= IsoLevel))
		{
			const float T = (FMath::Abs(Denom) < KINDA_SMALL_NUMBER) ? 0.5f : FMath::Clamp((IsoLevel - Da) / Denom, 0.0f, 1.0f);
			const float Ac = Aa + T * Ef;
			const float Dist = FMath::Abs(Ac - Near);
			if (Dist < BestDist)
			{
				BestDist = Dist;
				OutCoord = Ac;
				bFound = true;
			}
		}
	}
	return bFound;
}

bool FVoxelCPUMarchingCubesMesher::ComputeCoarseCrossingsAlongAxis(
	const FVoxelMeshingRequest& Request,
	const FVector3f& V, int32 Axis,
	int32 CoarserStride,
	float& OutCoord) const
{
	// The coarse cells' vertices on the four coarse edge columns along Axis around the line (ComputeCoarseSurfaceAlongAxis'
	// lattice, its clamp and search band), blended bilinearly: on a column, that column's coarse vertex.
	const float IsoLevel = Config.IsoLevel;
	const int32 E = FMath::Max(1, CoarserStride);
	const float Ef = static_cast<float>(E);
	const int32 ChunkSize = Request.ChunkSize;
	const int32 AxisB = (Axis + 1) % 3;
	const int32 AxisC = (Axis + 2) % 3;
	const int32 B0 = FMath::FloorToInt(V[AxisB] / Ef) * E;
	const int32 C0 = FMath::FloorToInt(V[AxisC] / Ef) * E;
	const float Fb = (V[AxisB] - static_cast<float>(B0)) / Ef;
	const float Fc = (V[AxisC] - static_cast<float>(C0)) / Ef;
	const int32 Bs[2] = { FMath::Clamp(B0, 0, ChunkSize), FMath::Clamp(B0 + E, 0, ChunkSize) };
	const int32 Cs[2] = { FMath::Clamp(C0, 0, ChunkSize), FMath::Clamp(C0 + E, 0, ChunkSize) };
	const float Near = V[Axis];
	const int32 SearchCells = 4;
	const int32 ABase = FMath::FloorToInt(Near / Ef) * E;

	float Sum = 0.0f;
	for (int32 Column = 0; Column < 4; ++Column)
	{
		const float Weight = ((Column & 1) ? Fb : 1.0f - Fb) * ((Column & 2) ? Fc : 1.0f - Fc);
		if (Weight <= 0.0f)
		{
			continue;
		}
		FIntVector At;
		At[AxisB] = Bs[Column & 1];
		At[AxisC] = Cs[(Column >> 1) & 1];
		float BestDist = TNumericLimits<float>::Max();
		float Best = 0.0f;
		bool bFound = false;
		for (int32 k = -SearchCells; k < SearchCells; ++k)
		{
			FIntVector EndA = At;
			FIntVector EndB = At;
			EndA[Axis] = ABase + k * E;
			EndB[Axis] = EndA[Axis] + E;
			const float Da = GetDensityAt(Request, EndA.X, EndA.Y, EndA.Z);
			const float Db = GetDensityAt(Request, EndB.X, EndB.Y, EndB.Z);
			FVector3f Crossing;
			if ((Da >= IsoLevel) != (Db >= IsoLevel) && FindEdgeVoxelCrossing(Request, EndA, EndB, Da, Db, Crossing))
			{
				const float Dist = FMath::Abs(Crossing[Axis] - Near);
				if (Dist < BestDist)
				{
					BestDist = Dist;
					Best = Crossing[Axis];
					bFound = true;
				}
			}
		}
		if (!bFound)
		{
			return false; // a column the blend leans on has no coarse vertex near: no morph, never worse than the wall
		}
		Sum += Weight * Best;
	}
	OutCoord = Sum;
	return true;
}

void FVoxelCPUMarchingCubesMesher::MorphVertexToCoarse(
	const FVoxelMeshingRequest& Request,
	int32 SelfStride,
	uint8 TransitionMask,
	FVector3f& Vertex) const
{
	const float VoxelSize = Request.VoxelSize;
	const int32 ChunkSize = Request.ChunkSize;
	const int32 SelfLOD = Request.LODLevel;
	// Seam jobs pass a band-confined ramp width (zero exactly at the band's inner edge) so
	// morphed band vertices stay coincident with the unmorphed interior mesh at the junction.
	const float MorphWidth = (Request.MorphWidthOverride > 0.0f)
		? Request.MorphWidthOverride
		: FMath::Max(0.01f, CVarMCBoundaryMorphWidth.GetValueOnAnyThread());

	const float Vx = Vertex.X / VoxelSize;
	const float Vy = Vertex.Y / VoxelSize;
	const float Vz = Vertex.Z / VoxelSize;

	// Ramp weight = proximity to the nearest active transition face (1 at the seam, 0 at depth
	// MorphWidth coarse cells). Track the coarser stride of that nearest face. Pure function of
	// the vertex position + chunk params, so a vertex shared by two cells (or by the fine MC
	// and the ribbon) morphs identically on both -> stays coincident -> watertight.
	float BestW = 0.0f;
	int32 BestE = SelfStride;
	for (int32 Face = 0; Face < 6; Face++)
	{
		if (!(TransitionMask & (1 << Face)))
		{
			continue;
		}
		const int32 NbrLOD = Request.NeighborLODLevels[Face];
		const int32 E = (NbrLOD > SelfLOD) ? (1 << NbrLOD) : SelfStride;
		if (E <= SelfStride)
		{
			continue; // neighbour not actually coarser — nothing to reduce to
		}
		const int32 Axis = Face / 2;
		const bool bPositive = (Face % 2) == 1;
		const float BoundaryCoord = bPositive ? static_cast<float>(ChunkSize) : 0.0f;
		const float VCoord = (Axis == 0) ? Vx : (Axis == 1) ? Vy : Vz;
		const float DistCells = FMath::Abs(VCoord - BoundaryCoord) / static_cast<float>(E);
		const float W = FMath::Clamp((MorphWidth - DistCells) / MorphWidth, 0.0f, 1.0f);
		if (W > BestW)
		{
			BestW = W;
			BestE = E;
		}
	}

	if (BestW <= 0.0f)
	{
		return;
	}

	// The morph runs along the axis nearest the chunk's local up: Z on a heightmap world, the axis nearest the radial
	// direction on a planet (spherical SP-D2). One body for every up (code review CR2, MESH-30).
	const FVector3f V(Vx, Vy, Vz);
	const FVector AbsUp = Request.LocalUp.GetAbs();
	const int32 Axis = (AbsUp.X >= AbsUp.Y && AbsUp.X >= AbsUp.Z) ? 0 : (AbsUp.Y >= AbsUp.Z ? 1 : 2);

	// Steepness gate: only morph where the surface is a HEIGHT FUNCTION along Axis (normal mostly along it). On steep
	// surfaces the morph along one axis is ill-conditioned (it distorts the thin boundary ribbon and opens the seam), so
	// those keep their un-morphed, watertight boundary. The surface normal is -gradient(density); a small |normal[Axis]|
	// relative to |normal| means a steep surface. Sampled +-1 voxel (within the 1-plane neighbour data) and a pure
	// function of position, so coincident vertices share the decision and stay watertight. (v1 limitation; a
	// normal-direction morph would generalise: see GEOMORPH_IMPLEMENTATION_PLAN.md.)
	const float Gx = GetDensityAtTrilinear(Request, Vx + 1.0f, Vy, Vz) - GetDensityAtTrilinear(Request, Vx - 1.0f, Vy, Vz);
	const float Gy = GetDensityAtTrilinear(Request, Vx, Vy + 1.0f, Vz) - GetDensityAtTrilinear(Request, Vx, Vy - 1.0f, Vz);
	const float Gz = GetDensityAtTrilinear(Request, Vx, Vy, Vz + 1.0f) - GetDensityAtTrilinear(Request, Vx, Vy, Vz - 1.0f);
	const float GMag = FMath::Sqrt(Gx * Gx + Gy * Gy + Gz * Gz);
	const float GAxis = (Axis == 0) ? Gx : (Axis == 1) ? Gy : Gz;
	constexpr float SteepThreshold = 0.5f; // |normal[Axis]|/|normal| below this => too steep => skip
	if (GMag < KINDA_SMALL_NUMBER || FMath::Abs(GAxis) < SteepThreshold * GMag)
	{
		return;
	}

	// Morph toward the coarse surface along Axis (the coarse cells' crossings with voxel.Mesh.VoxelLODCrossings, G80, else
	// the coarse lattice's trilinear surface). With no coarse crossing found nearby the vertex stays (never worse than the
	// current wall).
	float SecCoord;
	const bool bFound = VoxelCrossingGradient::UseVoxelLODCrossings(BestE)
		? ComputeCoarseCrossingsAlongAxis(Request, V, Axis, BestE, SecCoord)
		: ComputeCoarseSurfaceAlongAxis(Request, V, Axis, BestE, SecCoord);
	if (bFound)
	{
		Vertex[Axis] = FMath::Lerp(V[Axis], SecCoord, BestW) * VoxelSize;
	}
}

void FVoxelCPUMarchingCubesMesher::ApplyBoundaryMorph(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	uint8 TransitionMask,
	FVector3f* CellVertices,
	int32 VertexCount) const
{
	if (TransitionMask == 0)
	{
		return;
	}

	for (int32 i = 0; i < VertexCount; i++)
	{
		MorphVertexToCoarse(Request, Stride, TransitionMask, CellVertices[i]);
	}
}

FVector3f FVoxelCPUMarchingCubesMesher::CalculateGradientNormal(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z) const
{
	// The central difference about the voxel the point floors to, taps one voxel either side: the stride-1 case of
	// CalculateGradientNormalLOD (code review CR2, MESH-10).
	return CalculateGradientNormalLOD(Request, X, Y, Z, 1);
}

// ============================================================================
// LOD Helper Functions
// ============================================================================

FVector3f FVoxelCPUMarchingCubesMesher::CalculateGradientNormalLOD(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z,
	int32 Stride) const
{
	// Use stride-scaled central difference for gradient approximation
	// This gives smoother normals at higher LOD levels
	const float Delta = static_cast<float>(Stride);

	const int32 IX = FMath::FloorToInt(X);
	const int32 IY = FMath::FloorToInt(Y);
	const int32 IZ = FMath::FloorToInt(Z);

	// Central difference gradient with LOD-scaled sampling
	float gx = GetDensityAt(Request, IX + Stride, IY, IZ) - GetDensityAt(Request, IX - Stride, IY, IZ);
	float gy = GetDensityAt(Request, IX, IY + Stride, IZ) - GetDensityAt(Request, IX, IY - Stride, IZ);
	float gz = GetDensityAt(Request, IX, IY, IZ + Stride) - GetDensityAt(Request, IX, IY, IZ - Stride);

	// Normal points away from solid (opposite to gradient direction)
	FVector3f Normal(-gx, -gy, -gz);

	if (!Normal.Normalize())
	{
		return FVector3f(0.0f, 0.0f, 1.0f);
	}

	return Normal;
}

FVector3f FVoxelCPUMarchingCubesMesher::CalculateEdgeShadingNormal(
	const FVoxelMeshingRequest& Request,
	const FIntVector& EndA, const FIntVector& EndB,
	float DensityA, float DensityB,
	const FVector3f& FallbackPosition,
	int32 FallbackStride) const
{
	FVector3f Crossing;
	if (!FindEdgeVoxelCrossing(Request, EndA, EndB, DensityA, DensityB, Crossing))
	{
		return CalculateGradientNormalLOD(Request, FallbackPosition.X, FallbackPosition.Y, FallbackPosition.Z, FallbackStride);
	}
	return CalculateShadingNormal(Request, Crossing.X, Crossing.Y, Crossing.Z);
}

bool FVoxelCPUMarchingCubesMesher::FindEdgeVoxelCrossing(
	const FVoxelMeshingRequest& Request,
	const FIntVector& EndA, const FIntVector& EndB,
	float DensityA, float DensityB,
	FVector3f& OutCrossing) const
{
	return VoxelLODCrossing::FindEdgeVoxelCrossing(
		[&](int32 VX, int32 VY, int32 VZ) { return GetDensityAt(Request, VX, VY, VZ); },
		EndA, EndB, DensityA, DensityB, Config.IsoLevel, OutCrossing);
}

FVector3f FVoxelCPUMarchingCubesMesher::CalculateShadingNormal(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z) const
{
	return VoxelCrossingGradient::ShadingNormal(
		[&](int32 VX, int32 VY, int32 VZ) { return GetDensityAt(Request, VX, VY, VZ); },
		[&](int32 VX, int32 VY, int32 VZ) { return IsVoxelHeld(Request, VX, VY, VZ); },
		X, Y, Z, [&]() { return CalculateGradientNormal(Request, X, Y, Z); });
}

bool FVoxelCPUMarchingCubesMesher::IsVoxelHeld(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	const int32 CS = Request.ChunkSize;
	if (X < -1 || X > CS || Y < -1 || Y > CS || Z < -1 || Z > CS)
	{
		return false;
	}
	const bool bXPos = X == CS, bXNeg = X < 0;
	const bool bYPos = Y == CS, bYNeg = Y < 0;
	const bool bZPos = Z == CS, bZNeg = Z < 0;
	const bool bOutX = bXPos || bXNeg;
	const bool bOutY = bYPos || bYNeg;
	const bool bOutZ = bZPos || bZNeg;
	const int32 OutCount = (bOutX ? 1 : 0) + (bOutY ? 1 : 0) + (bOutZ ? 1 : 0);
	if (OutCount == 0)
	{
		return true;
	}

	// GetVoxelAt's sources one voxel past the chunk (each plane, strip and corner holds that depth); it clamps elsewhere.
	if (Request.VolumeViews[13] != nullptr)
	{
		return Request.VolumeViews[FVoxelMeshingRequest::VolumeViewIndex(bXPos ? 1 : (bXNeg ? -1 : 0),
			bYPos ? 1 : (bYNeg ? -1 : 0), bZPos ? 1 : (bZNeg ? -1 : 0))] != nullptr;
	}
	using R = FVoxelMeshingRequest;
	if (OutCount == 1)
	{
		const int32 SliceSize = CS * CS;
		const TArray<FVoxelData>& Plane = bOutX ? (bXPos ? Request.NeighborXPos : Request.NeighborXNeg)
			: bOutY ? (bYPos ? Request.NeighborYPos : Request.NeighborYNeg)
			: (bZPos ? Request.NeighborZPos : Request.NeighborZNeg);
		return Plane.Num() == SliceSize;
	}
	if (OutCount == 2)
	{
		const uint32 Edge = !bOutZ ? (bXPos ? (bYPos ? R::EDGE_XPOS_YPOS : R::EDGE_XPOS_YNEG) : (bYPos ? R::EDGE_XNEG_YPOS : R::EDGE_XNEG_YNEG))
			: !bOutY ? (bXPos ? (bZPos ? R::EDGE_XPOS_ZPOS : R::EDGE_XPOS_ZNEG) : (bZPos ? R::EDGE_XNEG_ZPOS : R::EDGE_XNEG_ZNEG))
			: (bYPos ? (bZPos ? R::EDGE_YPOS_ZPOS : R::EDGE_YPOS_ZNEG) : (bZPos ? R::EDGE_YNEG_ZPOS : R::EDGE_YNEG_ZNEG));
		return Request.HasEdge(Edge);
	}
	const uint32 Corner = bXPos
		? (bYPos ? (bZPos ? R::CORNER_XPOS_YPOS_ZPOS : R::CORNER_XPOS_YPOS_ZNEG) : (bZPos ? R::CORNER_XPOS_YNEG_ZPOS : R::CORNER_XPOS_YNEG_ZNEG))
		: (bYPos ? (bZPos ? R::CORNER_XNEG_YPOS_ZPOS : R::CORNER_XNEG_YPOS_ZNEG) : (bZPos ? R::CORNER_XNEG_YNEG_ZPOS : R::CORNER_XNEG_YNEG_ZNEG));
	return Request.HasCorner(Corner);
}

uint8 FVoxelCPUMarchingCubesMesher::GetDominantMaterialLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	uint8 CubeIndex) const
{
	// The surface material of a strided cell (LOD > 0): from each solid strided corner, the last solid voxel before air
	// along the chunk's local up (ScanSurfaceAlongLocalUp: a voxel column on a heightmap world, the digital line along up
	// on a planet, spherical SP-D1); on a slope the corners' surfaces lie at different heights, and the highest along up
	// wins (grass over the dirt under it). One walk for every up (code review CR2, MESH-30).
	uint8 SurfaceMaterial = 0;
	double Highest = TNumericLimits<double>::Lowest();
	for (int32 i = 0; i < 8; i++)
	{
		if (CubeIndex & (1 << i))
		{
			const FIntVector Corner = FIntVector(X, Y, Z) + MarchingCubesTables::CornerOffsets[i] * Stride;
			uint8 Value = 0;
			const double Along = ScanSurfaceAlongLocalUp(Request, Corner, /*bBiome=*/false, Value);
			if (Along > Highest)
			{
				Highest = Along;
				SurfaceMaterial = Value;
			}
		}
	}
	return SurfaceMaterial;
}

uint8 FVoxelCPUMarchingCubesMesher::GetDominantBiomeLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	uint8 CubeIndex) const
{
	// The surface biome of a strided cell, as GetDominantMaterialLOD finds its material.
	uint8 SurfaceBiome = 0;
	double Highest = TNumericLimits<double>::Lowest();
	for (int32 i = 0; i < 8; i++)
	{
		if (CubeIndex & (1 << i))
		{
			const FIntVector Corner = FIntVector(X, Y, Z) + MarchingCubesTables::CornerOffsets[i] * Stride;
			uint8 Value = 0;
			const double Along = ScanSurfaceAlongLocalUp(Request, Corner, /*bBiome=*/true, Value);
			if (Along > Highest)
			{
				Highest = Along;
				SurfaceBiome = Value;
			}
		}
	}
	return SurfaceBiome;
}

double FVoxelCPUMarchingCubesMesher::ScanSurfaceAlongLocalUp(
	const FVoxelMeshingRequest& Request,
	const FIntVector& Corner,
	bool bBiome,
	uint8& OutValue) const
{
	// The LOD material scan's per-corner walk: along the digital line in local up's direction (a voxel column when up is
	// +Z: FVoxelDigitalLine::Make(+Z) steps (0, 0, 1)) to the last solid voxel before air, at most MaxScanDistance steps,
	// scored by how far along up it lies (the Z of the last solid voxel when up is +Z).
	constexpr int32 MaxScanDistance = 8;
	const FVoxelDigitalLine Line = FVoxelDigitalLine::Make(Request.LocalUp);
	OutValue = 0;
	FIntVector Last = Corner;
	for (int32 K = 0; K <= MaxScanDistance; ++K)
	{
		const FIntVector I = Line.Step(Corner, K);
		const FVoxelData Voxel = GetVoxelAt(Request, I.X, I.Y, I.Z);
		if (!Voxel.IsSolid())
		{
			break;
		}
		OutValue = bBiome ? Voxel.BiomeID : Voxel.MaterialID;
		Last = I;
	}
	return FVector::DotProduct(FVector(Last), Request.LocalUp);
}

// ============================================================================
// Configuration
// ============================================================================

void FVoxelCPUMarchingCubesMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelCPUMarchingCubesMesher::GetConfig() const
{
	return Config;
}

// ============================================================================
// Transvoxel Implementation
// ============================================================================

uint8 FVoxelCPUMarchingCubesMesher::GetTransitionFaces(const FVoxelMeshingRequest& Request) const
{
	// The TransitionFaces field is set by the chunk manager based on neighbor LOD levels.
	// A face needs transition cells if the neighbor chunk is COARSER (higher LOD level number).
	return Request.TransitionFaces;
}

int32 FVoxelCPUMarchingCubesMesher::GetTransitionCellStride(
	const FVoxelMeshingRequest& Request,
	int32 FaceIndex,
	int32 CurrentStride) const
{
	// Get the neighbor's LOD level for this face
	const int32 NeighborLOD = Request.NeighborLODLevels[FaceIndex];

	if (NeighborLOD < 0)
	{
		// No neighbor, use current stride
		return CurrentStride;
	}

	// Calculate neighbor's stride: 2^LODLevel
	const int32 NeighborStride = 1 << NeighborLOD;

	// The transition cell should use the LARGER stride (coarser neighbor's stride)
	// to ensure the transition cell spans the same area as one of the neighbor's cells.
	// This guarantees the corner vertices will align with the neighbor's grid.
	return FMath::Max(CurrentStride, NeighborStride);
}

bool FVoxelCPUMarchingCubesMesher::HasRequiredNeighborData(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	int32 FaceIndex) const
{
	// Check if all sample positions for this transition cell have valid neighbor data.
	// Transition cells at chunk edges/corners may require edge/corner neighbor data,
	// not just face neighbor data.

	const int32 ChunkSize = Request.ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Check all 13 sample positions
	for (int32 i = 0; i < 13; i++)
	{
		const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][i];

		// Calculate sample position (need to check integer bounds for trilinear corners too)
		const float SampleX = static_cast<float>(X) + Offset.X * static_cast<float>(Stride);
		const float SampleY = static_cast<float>(Y) + Offset.Y * static_cast<float>(Stride);
		const float SampleZ = static_cast<float>(Z) + Offset.Z * static_cast<float>(Stride);

		// Check all 8 corners that trilinear interpolation would sample
		// (or just the integer position if it's exact)
		const int32 X0 = FMath::FloorToInt(SampleX);
		const int32 Y0 = FMath::FloorToInt(SampleY);
		const int32 Z0 = FMath::FloorToInt(SampleZ);

		// Check both the floor and ceil positions for trilinear interpolation
		for (int32 dx = 0; dx <= 1; dx++)
		{
			for (int32 dy = 0; dy <= 1; dy++)
			{
				for (int32 dz = 0; dz <= 1; dz++)
				{
					const int32 CheckX = X0 + dx;
					const int32 CheckY = Y0 + dy;
					const int32 CheckZ = Z0 + dz;

					// Skip if within chunk bounds
					if (CheckX >= 0 && CheckX < ChunkSize &&
						CheckY >= 0 && CheckY < ChunkSize &&
						CheckZ >= 0 && CheckZ < ChunkSize)
					{
						continue;
					}

					// Determine what neighbor data is required
					const bool bXPos = (CheckX >= ChunkSize);
					const bool bXNeg = (CheckX < 0);
					const bool bYPos = (CheckY >= ChunkSize);
					const bool bYNeg = (CheckY < 0);
					const bool bZPos = (CheckZ >= ChunkSize);
					const bool bZNeg = (CheckZ < 0);

					const int32 OutCount = (bXPos || bXNeg ? 1 : 0) +
					                       (bYPos || bYNeg ? 1 : 0) +
					                       (bZPos || bZNeg ? 1 : 0);

					// A seam job reading its participants in place (G63): the neighbour in that direction is held or not.
					if (Request.VolumeViews[13] != nullptr)
					{
						if (Request.VolumeViews[FVoxelMeshingRequest::VolumeViewIndex(bXPos ? 1 : (bXNeg ? -1 : 0),
							bYPos ? 1 : (bYNeg ? -1 : 0), bZPos ? 1 : (bZNeg ? -1 : 0))] == nullptr)
						{
							return false;
						}
						continue;
					}

					// Check face neighbors
					if (OutCount == 1)
					{
						if (bXPos && Request.NeighborXPos.Num() != SliceSize) return false;
						if (bXNeg && Request.NeighborXNeg.Num() != SliceSize) return false;
						if (bYPos && Request.NeighborYPos.Num() != SliceSize) return false;
						if (bYNeg && Request.NeighborYNeg.Num() != SliceSize) return false;
						if (bZPos && Request.NeighborZPos.Num() != SliceSize) return false;
						if (bZNeg && Request.NeighborZNeg.Num() != SliceSize) return false;
					}
					// Check edge neighbors
					else if (OutCount == 2)
					{
						if (bXPos && bYPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS)) return false;
						if (bXPos && bYNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG)) return false;
						if (bXNeg && bYPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS)) return false;
						if (bXNeg && bYNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG)) return false;
						if (bXPos && bZPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS)) return false;
						if (bXPos && bZNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG)) return false;
						if (bXNeg && bZPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS)) return false;
						if (bXNeg && bZNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG)) return false;
						if (bYPos && bZPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS)) return false;
						if (bYPos && bZNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG)) return false;
						if (bYNeg && bZPos && !Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS)) return false;
						if (bYNeg && bZNeg && !Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG)) return false;
					}
					// Check corner neighbors
					else if (OutCount == 3)
					{
						if (bXPos && bYPos && bZPos && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS)) return false;
						if (bXPos && bYPos && bZNeg && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG)) return false;
						if (bXPos && bYNeg && bZPos && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS)) return false;
						if (bXPos && bYNeg && bZNeg && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG)) return false;
						if (bXNeg && bYPos && bZPos && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS)) return false;
						if (bXNeg && bYPos && bZNeg && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG)) return false;
						if (bXNeg && bYNeg && bZPos && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS)) return false;
						if (bXNeg && bYNeg && bZNeg && !Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG)) return false;
					}
				}
			}
		}
	}

	return true;
}

void FVoxelCPUMarchingCubesMesher::GetTransitionCellDensities(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	int32 FaceIndex,
	float OutDensities[13]) const
{
	// Sample 13 points for the ZERO-THICKNESS transition cell (see
	// LOD_SEAM_INVESTIGATION.md). The transition cell is a flat reduction ribbon that
	// lives entirely IN the boundary plane between the finer chunk and its coarser
	// neighbour:
	// - Points 0-8: the fine 3x3 grid, sampled ON the boundary plane. This is the SAME
	//   plane and densities the finer chunk's regular MC samples for its boundary face,
	//   so the fine contour is shared exactly (no inner T-junctions / holes).
	// - Points 9-12: the 4 coarse corners, also ON the boundary plane, coincident with
	//   fine corners 0,2,6,8. The coarse contour interpolates between them exactly as the
	//   coarser neighbour does, so the outer contour matches B exactly.
	// Because both the fine face and the coarse corners are on the same plane (zero
	// depth), the table invariant d[0x9]=d[0] etc. holds and the contour topology is
	// consistent with B's boundary plane — eliminating the depth-offset near-corner
	// flips that the earlier thick cell left as slivers.
	//
	// Depth axis (face-normal) is PINNED to the boundary voxel coordinate; face-parallel
	// axes spread across the coarse cell at CoarserStride (Stride parameter).

	const int32 CurrentStride = 1 << Request.LODLevel;
	const int32 DepthAxis = FaceIndex / 2;
	const bool bPositiveFace = (FaceIndex % 2) == 1;
	const int32 BaseDepthCoord = (DepthAxis == 0) ? X : (DepthAxis == 1) ? Y : Z;
	// Negative faces: boundary at the cell's depth coord (0). Positive faces: boundary
	// is one chunk-stride outward (the +face plane at ChunkSize).
	const int32 BoundaryDepthCoord = bPositiveFace ? (BaseDepthCoord + CurrentStride) : BaseDepthCoord;

	for (int32 i = 0; i < 13; i++)
	{
		const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][i];

		// Face-parallel axes use offset * CoarserStride; the depth axis is pinned to the
		// boundary plane (offset depth component ignored -> zero thickness).
		const float SampleX = (DepthAxis == 0) ? static_cast<float>(BoundaryDepthCoord)
			: (static_cast<float>(X) + Offset.X * static_cast<float>(Stride));
		const float SampleY = (DepthAxis == 1) ? static_cast<float>(BoundaryDepthCoord)
			: (static_cast<float>(Y) + Offset.Y * static_cast<float>(Stride));
		const float SampleZ = (DepthAxis == 2) ? static_cast<float>(BoundaryDepthCoord)
			: (static_cast<float>(Z) + Offset.Z * static_cast<float>(Stride));

		OutDensities[i] = GetDensityAtTrilinear(Request, SampleX, SampleY, SampleZ);
	}

	// Lengyel invariant: the 4 coarse corners share the fine corners' state (they are
	// coincident on the boundary plane). Set explicitly so the transition table's
	// vertex-reuse + triangulation run exactly as designed.
	OutDensities[9]  = OutDensities[0];
	OutDensities[10] = OutDensities[2];
	OutDensities[11] = OutDensities[6];
	OutDensities[12] = OutDensities[8];
}

bool FVoxelCPUMarchingCubesMesher::ProcessTransitionCell(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	int32 FaceIndex,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	// Get all 13 density samples for this transition cell
	// Samples 0-8: face samples (3x3 grid on transition face)
	// Samples 9-12: interior corners (at the opposite side of the cell)
	float Densities[13];
	GetTransitionCellDensities(Request, X, Y, Z, Stride, FaceIndex, Densities);

	const float IsoLevel = Config.IsoLevel;
	const float VoxelSize = Request.VoxelSize;

	// Build the 9-bit case index from face samples using Lengyel's bit ordering.
	//
	// CRITICAL: The TransitionVertexData table uses a DIFFERENT sample-to-bit mapping
	// than the natural row-by-row order. Lengyel's bits trace the 3x3 grid perimeter
	// clockwise, then the center last:
	//
	//   Natural sample layout:     Lengyel bit layout:
	//     6---7---8                  6---5---4
	//     |   |   |                  |   |   |
	//     3---4---5                  7---8---3
	//     |   |   |                  |   |   |
	//     0---1---2                  0---1---2
	//
	//   Bit 0→Sample 0, Bit 1→Sample 1, Bit 2→Sample 2, Bit 3→Sample 5,
	//   Bit 4→Sample 8, Bit 5→Sample 7, Bit 6→Sample 6, Bit 7→Sample 3, Bit 8→Sample 4
	//
	// The endpoint indices in TransitionVertexData (low byte nibbles) still use the
	// NATURAL sample order (0-8). Only the CASE BITS use the perimeter ordering.
	// Using direct bit N → sample N (as we did before) selects the WRONG case,
	// producing edges between samples on the same side of the isosurface.
	//
	// Reference: Godot Voxel's transvoxel.cpp, Lengyel's Transvoxel reference.
	static constexpr int32 BitToSample[9] = { 0, 1, 2, 5, 8, 7, 6, 3, 4 };
	uint16 SolidMask = 0;
	for (int32 Bit = 0; Bit < 9; Bit++)
	{
		if (Densities[BitToSample[Bit]] >= IsoLevel)
		{
			SolidMask |= (1 << Bit);
		}
	}
	const uint16 CaseIndex = (~SolidMask) & 0x1FF;

	// Look up the equivalence class
	// The high bit (0x80) indicates inverted winding order
	// The low 7 bits contain the equivalence class (0-55)
	const uint8 CellClassData = TransvoxelTables::TransitionCellClass[CaseIndex];
	const bool bInverted = (CellClassData & 0x80) != 0;
	const uint8 CellClass = CellClassData & 0x7F;

	// Early out for empty cases (class 0)
	// No surface crosses the transition face in this cell. Adjacent non-boundary
	// MC cells handle any interior crossings. No fallback MC needed.
	if (CellClass == 0)
	{
		return false;
	}

	// Bounds check
	if (CellClass >= 56)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid transition cell class %d for case %d"), CellClass, CaseIndex);
		return false;
	}

	const uint8 CellData = TransvoxelTables::TransitionCellData[CellClass];
	const int32 VertexCount = CellData >> 4;
	const int32 TriangleCount = CellData & 0x0F;

	if (VertexCount == 0 || TriangleCount == 0)
	{
		return false;
	}

	// Bounds check vertex count
	if (VertexCount > 12)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid vertex count %d for class %d"), VertexCount, CellClass);
		return false;
	}

	// ZERO-THICKNESS cell: positions collapse onto the boundary plane (matching the
	// density sampling in GetTransitionCellDensities). Face-parallel axes spread across
	// the coarse cell at CoarserStride; the depth axis is pinned to the boundary plane
	// (depth scale 0), so the cell is a flat fine->coarse reduction ribbon shared with
	// fine MC (fine contour) and the coarser neighbour (coarse contour).
	const int32 CurrentStride = 1 << Request.LODLevel;
	const int32 DepthAxis = FaceIndex / 2;
	const bool bPositiveFace = (FaceIndex % 2) == 1;
	const int32 BaseDepthCoord = (DepthAxis == 0) ? X : (DepthAxis == 1) ? Y : Z;
	const int32 BoundaryDepthCoord = bPositiveFace ? (BaseDepthCoord + CurrentStride) : BaseDepthCoord;
	const float FaceScale = static_cast<float>(Stride) * VoxelSize;

	// Base position: face-parallel from the cell coord, depth pinned to the boundary plane.
	FVector3f BasePos = FVector3f(
		static_cast<float>(X) * VoxelSize,
		static_cast<float>(Y) * VoxelSize,
		static_cast<float>(Z) * VoxelSize
	);
	BasePos[DepthAxis] = static_cast<float>(BoundaryDepthCoord) * VoxelSize;

	// Cell scale: face-parallel = CoarserStride; depth = 0 (flat).
	FVector3f CellScale(FaceScale, FaceScale, FaceScale);
	CellScale[DepthAxis] = 0.0f;

	// Generate vertices
	// The vertex data encoding (from Eric Lengyel's Transvoxel):
	// - High byte: vertex reuse info (ignored for basic implementation)
	// - Low byte: edge endpoints
	//   - High nibble (bits 4-7): first endpoint index
	//   - Low nibble (bits 0-3): second endpoint index
	//
	// Endpoint indices:
	//   0-8: The 9 samples on the transition face
	//   9 (0x9): Interior corner 0 (sample index 9)
	//   A (0xA): Interior corner 1 (sample index 10)
	//   B (0xB): Interior corner 2 (sample index 11)
	//   C (0xC): Interior corner 3 (sample index 12)
	TArray<FVector3f> CellVertices;
	TArray<bool> VertexOnOuterFace;  // true = both endpoints are coarse corners (9-12), i.e. on the outer/coarse face — use CoarserStride for normals
	int32 VertexSamples[12][2];      // each vertex's edge (sample indices), for its normal at stride > 1 (G61)
	CellVertices.Reserve(VertexCount);
	VertexOnOuterFace.Reserve(VertexCount);

	// A sample's voxel (GetTransitionCellDensities' sampling: the depth axis pinned to the boundary plane, offsets of
	// 0, 0.5 and 1 coarse cells on the face axes, integers since CoarserStride is even).
	auto SampleVoxel = [&](int32 Sample) -> FIntVector
	{
		const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][Sample];
		FIntVector V(X + FMath::RoundToInt32(Offset.X * static_cast<float>(Stride)),
			Y + FMath::RoundToInt32(Offset.Y * static_cast<float>(Stride)),
			Z + FMath::RoundToInt32(Offset.Z * static_cast<float>(Stride)));
		V[DepthAxis] = BoundaryDepthCoord;
		return V;
	};

	// IMPORTANT: Index by CASE, not by class! The vertex data is pre-transformed per case.
	const uint16* VertexData = TransvoxelTables::TransitionVertexData[CaseIndex];
	for (int32 i = 0; i < VertexCount; i++)
	{
		const uint16 VData = VertexData[i];

		// Extract endpoints from LOW byte only
		const uint8 LowByte = VData & 0xFF;
		const int32 EndpointA = (LowByte >> 4) & 0x0F;  // High nibble of low byte
		const int32 EndpointB = LowByte & 0x0F;         // Low nibble of low byte

		// Map endpoint indices to sample indices
		// 0-8 map directly to face samples (indices 0-8 in Densities array)
		// 9-C (0x9-0xC) map to interior corners (indices 9-12 in Densities array)
		auto MapEndpointToSample = [](int32 Endpoint) -> int32
		{
			if (Endpoint <= 8)
			{
				return Endpoint;  // Face samples 0-8
			}
			// Interior corners: 0x9->9, 0xA->10, 0xB->11, 0xC->12
			switch (Endpoint)
			{
				case 0x9: return 9;   // Interior corner 0
				case 0xA: return 10;  // Interior corner 1
				case 0xB: return 11;  // Interior corner 2
				case 0xC: return 12;  // Interior corner 3
				default:
					return 0;  // Fallback
			}
		};

		const int32 SampleA = MapEndpointToSample(EndpointA);
		const int32 SampleB = MapEndpointToSample(EndpointB);

		FVector3f VertexPos;

		if (SampleA == SampleB)
		{
			// Vertex is exactly at this sample point
			const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][SampleA];
			VertexPos = BasePos + Offset * CellScale;
		}
		else
		{
			// Edge between two different samples — plain MC-style interpolation.
			//
			// With the corrected orientation (fine 9-sample face on the INNER side,
			// coarse 4-corner face on the OUTER boundary side), every edge interpolates
			// to the same position the adjacent mesh produces:
			//   - face-face (both 0-8): the fine inner contour, shared with the finer
			//     chunk's regular MC on that plane.
			//   - corner-corner (both 9-12): the coarse outer contour, shared with the
			//     coarser neighbour along its corner-to-corner edges.
			//   - face-corner (one of each): a genuine depth-bridging crossing.
			// No fin-snapping or boundary projection is needed; those were band-aids for
			// the previous inverted orientation and themselves introduced holes (a
			// snapped/degenerate triangle drops one user of an interior edge).
			const float DensityA = Densities[SampleA];
			const float DensityB = Densities[SampleB];

			const FVector3f& OffsetA = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][SampleA];
			const FVector3f& OffsetB = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][SampleB];
			const FVector3f PosA = BasePos + OffsetA * CellScale;
			const FVector3f PosB = BasePos + OffsetB * CellScale;

			VertexPos = InterpolateEdge(DensityA, DensityB, PosA, PosB, IsoLevel);

			// G80: a strided edge's vertex at its voxel crossing, as the coincident fine or coarse MC cell on the edge
			// puts it (corner-corner edges are the coarser neighbour's, face-face ones the fine mesh's).
			const FIntVector EndA = SampleVoxel(SampleA);
			const FIntVector EndB = SampleVoxel(SampleB);
			const FIntVector EdgeDelta = EndB - EndA;
			const int32 EdgeLength = FMath::Abs(EdgeDelta.X) + FMath::Abs(EdgeDelta.Y) + FMath::Abs(EdgeDelta.Z);
			FVector3f Crossing;
			if (VoxelCrossingGradient::UseVoxelLODCrossings(EdgeLength)
				&& FindEdgeVoxelCrossing(Request, EndA, EndB, DensityA, DensityB, Crossing))
			{
				VertexPos = Crossing * VoxelSize;
			}
		}

		// Validate vertex position is finite
		if (!FMath::IsFinite(VertexPos.X) || !FMath::IsFinite(VertexPos.Y) || !FMath::IsFinite(VertexPos.Z))
		{
			UE_LOG(LogVoxelMeshing, Error, TEXT("Transition cell: NaN/Inf vertex %d - skipping cell"), i);
			return false;
		}

		VertexSamples[CellVertices.Num()][0] = SampleA;
		VertexSamples[CellVertices.Num()][1] = SampleB;
		CellVertices.Add(VertexPos);
		// "Outer face" = the coarse boundary face, whose vertices lie between coarse
		// corners (samples 9-12). Those use the coarser stride for normals so they match
		// the coarser neighbour; everything touching the fine inner face uses the fine stride.
		VertexOnOuterFace.Add(SampleA >= 9 && SampleB >= 9);

	}

	// Geomorph the ribbon's FINE-side vertices toward the coarse surface, matching the (also
	// morphed) fine-MC boundary contour so they stay coincident -> the vertical wall collapses
	// while the seam stays watertight. The OUTER (coarse-corner) vertices are left on the coarse
	// contour so the exact neighbour match (T6/T8) is preserved.
	{
		const int32 SelfStride = 1 << Request.LODLevel;
		const uint8 FaceMask = static_cast<uint8>(1 << FaceIndex);
		for (int32 i = 0; i < CellVertices.Num(); i++)
		{
			if (!VertexOnOuterFace[i])
			{
				MorphVertexToCoarse(Request, SelfStride, FaceMask, CellVertices[i]);
			}
		}
	}

	// Get material and biome info.
	// Use natural-order solid mask (not Lengyel bit ordering) for MC-based material lookup.
	uint8 NaturalSolidMask = 0;
	for (int32 i = 0; i < 8; i++)
	{
		if (Densities[i] >= IsoLevel)
		{
			NaturalSolidMask |= (1 << i);
		}
	}
	const uint8 MaterialID = GetDominantMaterialLOD(Request, X, Y, Z, CurrentStride, NaturalSolidMask);
	const uint8 BiomeID = GetDominantBiomeLOD(Request, X, Y, Z, CurrentStride, NaturalSolidMask);
	const FColor VertexColor(MaterialID, BiomeID, 0, 255);

	// Add vertices to mesh
	const uint32 BaseIndex = OutMeshData.Positions.Num();

	for (int32 i = 0; i < CellVertices.Num(); i++)
	{
		const FVector3f& Pos = CellVertices[i];
		OutMeshData.Positions.Add(Pos);

		// Calculate normal using gradient — match the stride of the adjacent mesh:
		// Outer face vertices use CoarserStride (matches coarser neighbor MC normals),
		// interior/depth vertices use CurrentStride (matches finer chunk MC normals).
		// G61: at a stride > 1, LOD 0's normal at the voxel crossing on the vertex's edge, as the coincident vertex of the
		// fine or the coarse MC cell on that edge gets it.
		const int32 NormalStride = VertexOnOuterFace[i] ? Stride : CurrentStride;
		const FVector3f PosVoxel(Pos.X / VoxelSize, Pos.Y / VoxelSize, Pos.Z / VoxelSize);
		const int32 SampleA = VertexSamples[i][0];
		const int32 SampleB = VertexSamples[i][1];
		// G62: at stride 1 the fitted gradient at the vertex, as ProcessCubeLOD's.
		const FVector3f Normal = VoxelCrossingGradient::UseVoxelShadingNormals(NormalStride)
			? CalculateEdgeShadingNormal(Request, SampleVoxel(SampleA), SampleVoxel(SampleB), Densities[SampleA], Densities[SampleB],
				PosVoxel, NormalStride)
			: (NormalStride == 1)
				? CalculateShadingNormal(Request, PosVoxel.X, PosVoxel.Y, PosVoxel.Z)
				: CalculateGradientNormalLOD(Request, PosVoxel.X, PosVoxel.Y, PosVoxel.Z, NormalStride);
		OutMeshData.Normals.Add(Normal);

		// UV mapping (triplanar-style based on normal)
		const FVector3f AbsNormal = FVector3f(FMath::Abs(Normal.X), FMath::Abs(Normal.Y), FMath::Abs(Normal.Z));
		FVector2f UV;
		if (AbsNormal.Z >= AbsNormal.X && AbsNormal.Z >= AbsNormal.Y)
		{
			UV = FVector2f(Pos.X, Pos.Y) * Config.UVScale / VoxelSize;
		}
		else if (AbsNormal.X >= AbsNormal.Y)
		{
			UV = FVector2f(Pos.Y, Pos.Z) * Config.UVScale / VoxelSize;
		}
		else
		{
			UV = FVector2f(Pos.X, Pos.Z) * Config.UVScale / VoxelSize;
		}
		OutMeshData.UVs.Add(UV);

		// UV1: MaterialID only (smooth meshing uses triplanar, no FaceType needed)
		OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));

		OutMeshData.Colors.Add(VertexColor);
	}

	// Add triangles with proper winding order.
	// Each face maps the table's (u, v) face axes to 3D world axes, and the cell's depth
	// axis to the third. The corrected orientation (fine face inner, coarse corners
	// outer) reflects the whole cell along its depth axis relative to the old tables
	// (every sample's depth coord went d -> 1-d). A reflection flips handedness, so the
	// per-face winding-reverse decision is the TOGGLE of the previous one for all faces:
	//   old { true, true, false, false, true, true }  ->  new { false, false, true, true, false, false }
	// Combined with bInverted (which flips winding for reflected equivalence classes):
	//   Use original winding when bInverted != FaceNeedsWindingReverse[FaceIndex].
	// Verified by T10 (orientation test): no back-facing transition triangles vs the
	// field gradient.
	static constexpr bool FaceNeedsWindingReverse[6] = { false, false, true, true, false, false };
	const bool bUseOriginalWinding = (bInverted != FaceNeedsWindingReverse[FaceIndex]);

	// Maximum allowed edge length squared for triangle validation.
	// Triangles with edges longer than this are degenerate (fins from underground vertices).
	const float MaxCellDim = FMath::Max3(CellScale.X, CellScale.Y, CellScale.Z);
	const float MaxEdgeLengthSq = MaxCellDim * MaxCellDim * 4.0f; // 2× cell diagonal

	const uint8* Triangles = TransvoxelTables::TransitionCellTriangles[CellClass];
	for (int32 t = 0; t < TriangleCount; t++)
	{
		const int32 BaseT = t * 3;
		if (Triangles[BaseT] == 0xFF)
		{
			break;
		}

		// Validate triangle indices
		const uint8 Idx0 = Triangles[BaseT];
		const uint8 Idx1 = Triangles[BaseT + 1];
		const uint8 Idx2 = Triangles[BaseT + 2];

		if (Idx0 >= VertexCount || Idx1 >= VertexCount || Idx2 >= VertexCount)
		{
			UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid triangle index in class %d: %d,%d,%d (vertex count: %d)"),
				CellClass, Idx0, Idx1, Idx2, VertexCount);
			continue;
		}

		// Skip degenerate triangles with overly long edges (fin artifacts from
		// interior vertices at incorrect positions on steep terrain)
		{
			const FVector3f& V0 = CellVertices[Idx0];
			const FVector3f& V1 = CellVertices[Idx1];
			const FVector3f& V2 = CellVertices[Idx2];
			const float EdgeSq01 = (V1 - V0).SizeSquared();
			const float EdgeSq12 = (V2 - V1).SizeSquared();
			const float EdgeSq20 = (V0 - V2).SizeSquared();
			if (EdgeSq01 > MaxEdgeLengthSq || EdgeSq12 > MaxEdgeLengthSq || EdgeSq20 > MaxEdgeLengthSq)
			{
				continue; // Skip this degenerate triangle
			}
		}

		if (bUseOriginalWinding)
		{
			OutMeshData.Indices.Add(BaseIndex + Idx0);
			OutMeshData.Indices.Add(BaseIndex + Idx1);
			OutMeshData.Indices.Add(BaseIndex + Idx2);
		}
		else
		{
			OutMeshData.Indices.Add(BaseIndex + Idx2);
			OutMeshData.Indices.Add(BaseIndex + Idx1);
			OutMeshData.Indices.Add(BaseIndex + Idx0);
		}
	}

	OutTriangleCount += TriangleCount;

	return true;
}

