// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshing.h"
#include "QEFSolver.h"
#include "HAL/IConsoleManager.h"

// Toggles the strided-boundary weld that seals DC LOD seams (Pass 3.5 on CPU /
// Pass 2.6 on GPU). Default on; set 0 + remesh for an A/B comparison showing the
// pre-fix cracks. Read on the worker/game thread; the GPU mesher reads the same
// CVar at dispatch time.
TAutoConsoleVariable<int32> CVarDCBoundaryWeld(
	TEXT("voxel.DCBoundaryWeld"),
	1,
	TEXT("Dual Contouring: weld strided LOD-boundary cells onto the shared chunk-face feature to seal seams (1=on, 0=off)."),
	ECVF_Default);

FVoxelCPUDualContourMesher::FVoxelCPUDualContourMesher()
{
}

FVoxelCPUDualContourMesher::~FVoxelCPUDualContourMesher()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelCPUDualContourMesher::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	UE_LOG(LogVoxelMeshing, Log, TEXT("CPU Dual Contouring Mesher initialized"));
	bIsInitialized = true;
}

void FVoxelCPUDualContourMesher::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	ReleaseAllHandles();
	bIsInitialized = false;
	UE_LOG(LogVoxelMeshing, Log, TEXT("CPU Dual Contouring Mesher shutdown"));
}

bool FVoxelCPUDualContourMesher::IsInitialized() const
{
	return bIsInitialized;
}

// ============================================================================
// Mesh Generation
// ============================================================================

bool FVoxelCPUDualContourMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData)
{
	FVoxelMeshingStats Stats;
	return GenerateMeshCPU(Request, OutMeshData, Stats);
}

bool FVoxelCPUDualContourMesher::GenerateMeshCPU(
	const FVoxelMeshingRequest& Request,
	FChunkMeshData& OutMeshData,
	FVoxelMeshingStats& OutStats)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("CPU Dual Contouring Mesher not initialized"));
		return false;
	}

	if (!Request.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("Invalid meshing request"));
		return false;
	}

	const double StartTime = FPlatformTime::Seconds();

	OutMeshData.Reset();
	OutStats = FVoxelMeshingStats();

	const int32 ChunkSize = Request.ChunkSize;
	const int32 LODLevel = FMath::Clamp(Request.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;
	const int32 LODChunkSize = ChunkSize / Stride;

	UE_LOG(LogVoxelMeshing, Log, TEXT("DC meshing chunk (%d,%d,%d) at LOD %d (stride %d, cells %d^3)"),
		Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
		LODLevel, Stride, LODChunkSize);

	// Pre-allocate
	const int32 EstimatedTriangles = LODChunkSize * LODChunkSize * 2;
	OutMeshData.Positions.Reserve(EstimatedTriangles * 3);
	OutMeshData.Normals.Reserve(EstimatedTriangles * 3);
	OutMeshData.UVs.Reserve(EstimatedTriangles * 3);
	OutMeshData.UV1s.Reserve(EstimatedTriangles * 3);
	OutMeshData.Colors.Reserve(EstimatedTriangles * 3);
	OutMeshData.Indices.Reserve(EstimatedTriangles * 3);

	uint32 TriangleCount = 0;
	uint32 SolidVoxels = 0;

	// Count solid voxels
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

	// Grid dimension: cells range from -1 to GridSize+1 → GridSize+3 entries per axis
	const int32 GridSize = LODChunkSize;
	const int32 GridDim = GridSize + 3;
	const int32 TotalCells = GridDim * GridDim * GridDim;

	// Pass 1: Detect edge crossings (flat array: 3 edges per cell position)
	TArray<FDCEdgeCrossing> EdgeCrossings;
	EdgeCrossings.SetNumZeroed(TotalCells * 3);
	TArray<int32> ValidEdgeIndices;
	ValidEdgeIndices.Reserve(GridSize * GridSize * 4);
	DetectEdgeCrossings(Request, Stride, GridDim, EdgeCrossings, ValidEdgeIndices);

	// Pass 2: Solve QEF for cell vertices
	TArray<FDCCellVertex> CellVertices;
	CellVertices.SetNumZeroed(TotalCells);
	SolveCellVertices(Request, Stride, GridDim, EdgeCrossings, CellVertices);

	// Pass 3.5: Weld strided boundary cells onto the shared chunk-face plane so both
	// sides of every stride>1 boundary (same-LOD strided AND fine|coarse LOD
	// transitions) derive the boundary vertex from the same shared face-plane data
	// and coincide. Stride-1 boundaries are left untouched (already watertight).
	if (CVarDCBoundaryWeld.GetValueOnAnyThread() != 0)
	{
		WeldStridedBoundaryCells(Request, Stride, GridDim, CellVertices);
	}

	// Pass 3: Generate quads
	GenerateQuads(Request, Stride, GridDim, EdgeCrossings, ValidEdgeIndices, CellVertices, OutMeshData, TriangleCount);

	// Generate skirts at LOD transition boundaries (when Transvoxel is disabled)
	if (Config.bGenerateSkirts && Request.TransitionFaces != 0)
	{
		GenerateSkirts(Request, Stride, OutMeshData, TriangleCount);
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
		TEXT("DC meshing complete: %d verts, %d tris, %d valid edges, %.2fms"),
		OutStats.VertexCount, TriangleCount, ValidEdgeIndices.Num(),
		OutStats.GenerationTimeMs);

	return true;
}

// ============================================================================
// Pass 1: Edge Crossing Detection
// ============================================================================

void FVoxelCPUDualContourMesher::DetectEdgeCrossings(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 GridDim,
	TArray<FDCEdgeCrossing>& OutEdgeCrossings,
	TArray<int32>& OutValidEdgeIndices)
{
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const int32 GridSize = ChunkSize / Stride;

	for (int32 CZ = -1; CZ <= GridSize; CZ++)
	{
		for (int32 CY = -1; CY <= GridSize; CY++)
		{
			for (int32 CX = -1; CX <= GridSize; CX++)
			{
				const int32 VX = CX * Stride;
				const int32 VY = CY * Stride;
				const int32 VZ = CZ * Stride;

				const float D0 = GetDensityAt(Request, VX, VY, VZ);

				for (int32 Axis = 0; Axis < 3; Axis++)
				{
					int32 NX = VX, NY = VY, NZ = VZ;
					if (Axis == 0) NX += Stride;
					else if (Axis == 1) NY += Stride;
					else NZ += Stride;

					const float D1 = GetDensityAt(Request, NX, NY, NZ);

					const bool bSolid0 = (D0 >= IsoLevel);
					const bool bSolid1 = (D1 >= IsoLevel);

					if (bSolid0 != bSolid1)
					{
						float t = (IsoLevel - D0) / (D1 - D0);
						t = FMath::Clamp(t, 0.0f, 1.0f);

						const FVector3f P0(static_cast<float>(VX) * VoxelSize,
							static_cast<float>(VY) * VoxelSize,
							static_cast<float>(VZ) * VoxelSize);
						const FVector3f P1(static_cast<float>(NX) * VoxelSize,
							static_cast<float>(NY) * VoxelSize,
							static_cast<float>(NZ) * VoxelSize);

						const int32 EIdx = EdgeIndex(CX, CY, CZ, Axis, GridDim);
						FDCEdgeCrossing& Crossing = OutEdgeCrossings[EIdx];
						Crossing.Position = P0 + (P1 - P0) * t;
						Crossing.bValid = true;

						const float CrossVoxelX = Crossing.Position.X / VoxelSize;
						const float CrossVoxelY = Crossing.Position.Y / VoxelSize;
						const float CrossVoxelZ = Crossing.Position.Z / VoxelSize;
						Crossing.Normal = (Stride > 1)
							? CalculateGradientNormalLOD(Request, CrossVoxelX, CrossVoxelY, CrossVoxelZ, Stride)
							: CalculateGradientNormal(Request, CrossVoxelX, CrossVoxelY, CrossVoxelZ);

						OutValidEdgeIndices.Add(EIdx);
					}
				}
			}
		}
	}
}

// ============================================================================
// Pass 2: QEF Vertex Solve
// ============================================================================

void FVoxelCPUDualContourMesher::SolveCellVertices(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 GridDim,
	const TArray<FDCEdgeCrossing>& EdgeCrossings,
	TArray<FDCCellVertex>& OutCellVertices)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 GridSize = ChunkSize / Stride;
	const float VoxelSize = Request.VoxelSize;
	const float CellWorldSize = static_cast<float>(Stride) * VoxelSize;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;

	// 12 edges of a cube cell: 4 along each axis
	struct FEdgeRef { int32 DX, DY, DZ, Axis; };
	static const FEdgeRef CellEdges[12] = {
		{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 2},
		{1, 0, 0, 1}, {1, 0, 0, 2},
		{0, 1, 0, 0}, {0, 1, 0, 2},
		{0, 0, 1, 0}, {0, 0, 1, 1},
		{1, 1, 0, 2}, {1, 0, 1, 1}, {0, 1, 1, 0},
	};

	// Solve from -1 to GridSize-1 (NOT GridSize).
	// Cells at -1 use face-neighbor data correctly (one layer is sufficient since
	// edges from CX=-1 access neighbor at X=ChunkSize-1 and chunk at X=0).
	// Cells at GridSize would have incomplete QEF data: face-neighbor slices provide
	// only one layer (X=0 of neighbor), so X-axis edges at GridSize map both endpoints
	// to the same density value, producing no crossings. The resulting QEF vertices are
	// unreliable, causing quads to be skipped or degenerate. The +X neighbor chunk
	// generates these boundary quads using ITS CX=-1 cells (which work correctly).
	// Edge detection still iterates to GridSize so cells at GridSize-1 get full
	// boundary edge data from their +1 neighbor edges.
	for (int32 CZ = -1; CZ < GridSize; CZ++)
	{
		for (int32 CY = -1; CY < GridSize; CY++)
		{
			for (int32 CX = -1; CX < GridSize; CX++)
			{
				FQEFSolver QEF;
				FVector3f AvgNormal = FVector3f::ZeroVector;

				for (const auto& Edge : CellEdges)
				{
					const int32 EIdx = EdgeIndex(CX + Edge.DX, CY + Edge.DY, CZ + Edge.DZ, Edge.Axis, GridDim);
					const FDCEdgeCrossing& Crossing = EdgeCrossings[EIdx];
					if (Crossing.bValid)
					{
						QEF.Add(Crossing.Position, Crossing.Normal);
						AvgNormal += Crossing.Normal;
					}
				}

				if (QEF.Count == 0)
				{
					continue;
				}

				const float MinX = static_cast<float>(CX * Stride) * VoxelSize;
				const float MinY = static_cast<float>(CY * Stride) * VoxelSize;
				const float MinZ = static_cast<float>(CZ * Stride) * VoxelSize;
				const FBox3f CellBounds(
					FVector3f(MinX, MinY, MinZ),
					FVector3f(MinX + CellWorldSize, MinY + CellWorldSize, MinZ + CellWorldSize)
				);

				const int32 CIdx = CellIndex(CX, CY, CZ, GridDim);
				FDCCellVertex& Vertex = OutCellVertices[CIdx];
				Vertex.bValid = true;
				Vertex.MeshVertexIndex = -1;  // SetNumZeroed zeroes this to 0; EmitVertex needs -1 to know it's unemitted
				Vertex.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength);

				if (!AvgNormal.Normalize())
				{
					AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
				}
				Vertex.Normal = AvgNormal;
				// Material/biome are assigned per-quad in GenerateQuads (from the owned
				// edge's solid endpoint) so triangles stay material-uniform.
			}
		}
	}
}

// ============================================================================
// Pass 3: Quad Generation
// ============================================================================

void FVoxelCPUDualContourMesher::GenerateQuads(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 GridDim,
	const TArray<FDCEdgeCrossing>& EdgeCrossings,
	const TArray<int32>& ValidEdgeIndices,
	TArray<FDCCellVertex>& CellVertices,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 GridSize = ChunkSize / Stride;
	const float IsoLevel = Config.IsoLevel;

	// Precomputed 4-cell offsets per axis (winding-correct)
	struct FCellOffset { int32 DX, DY, DZ; };
	static const FCellOffset AxisOffsets[3][4] = {
		// Axis 0 (X-edge): vary Y,Z
		{{0, 0, 0}, {0, -1, 0}, {0, -1, -1}, {0, 0, -1}},
		// Axis 1 (Y-edge): vary Z,X (Z×X = +Y)
		{{0, 0, 0}, {0, 0, -1}, {-1, 0, -1}, {-1, 0, 0}},
		// Axis 2 (Z-edge): vary X,Y
		{{0, 0, 0}, {-1, 0, 0}, {-1, -1, 0}, {0, -1, 0}},
	};

	// Duplicate-emission cache for cell vertices shared by quads of different
	// materials: key = (cell index << 16) | (material << 8) | biome.
	// Only material-border cells ever land here, so the map stays small.
	TMap<uint64, int32> DuplicateVertexCache;

	// Iterate only edges with actual crossings
	for (const int32 EIdx : ValidEdgeIndices)
	{
		// Decode flat index back to (CX, CY, CZ, Axis)
		const int32 Axis = EIdx % 3;
		const int32 CellLinear = EIdx / 3;
		const int32 CX = (CellLinear % GridDim) - 1;
		const int32 CY = ((CellLinear / GridDim) % GridDim) - 1;
		const int32 CZ = (CellLinear / (GridDim * GridDim)) - 1;

		const FCellOffset* Offsets = AxisOffsets[Axis];

		// Edge ownership: the edge position itself must be in [0, GridSize).
		// This ensures each physical edge is owned by exactly one chunk.
		// Cell offsets (containing 0 and -1) access cells in [-1, GridSize-1],
		// which SolveCellVertices already computes.
		const bool bOwned = (CX >= 0 && CX < GridSize && CY >= 0 && CY < GridSize && CZ >= 0 && CZ < GridSize);
		if (!bOwned)
		{
			continue;
		}

		// Look up the 4 cell vertices via flat array
		FDCCellVertex* Verts[4];
		int32 CellIdxs[4];
		bool bAllValid = true;

		for (int32 i = 0; i < 4; i++)
		{
			const int32 CIdx = CellIndex(CX + Offsets[i].DX, CY + Offsets[i].DY, CZ + Offsets[i].DZ, GridDim);
			FDCCellVertex& V = CellVertices[CIdx];
			if (!V.bValid)
			{
				bAllValid = false;
				break;
			}
			Verts[i] = &V;
			CellIdxs[i] = CIdx;
		}

		if (!bAllValid)
		{
			continue;
		}

		// Determine winding order from density sign at the edge start
		const float D0 = GetDensityAt(Request, CX * Stride, CY * Stride, CZ * Stride);
		const bool bFlip = (D0 < IsoLevel);

		// Per-quad material: the quad is dual to this sign-change edge, so its
		// surface material is the material of the edge's SOLID endpoint (the voxel
		// the surface bounds). All 4 vertices carry this ID — triangles must be
		// material-uniform because the pixel shader rounds the hardware-interpolated
		// UV1.x back to an integer atlas index; a triangle with mixed IDs sweeps
		// through every intermediate index and stripes material borders.
		int32 SolidX = CX * Stride, SolidY = CY * Stride, SolidZ = CZ * Stride;
		if (bFlip) // edge start is empty — the solid endpoint is the +Axis end
		{
			if (Axis == 0) { SolidX += Stride; }
			else if (Axis == 1) { SolidY += Stride; }
			else { SolidZ += Stride; }
		}
		const FVoxelData SolidVoxel = GetVoxelAt(Request, SolidX, SolidY, SolidZ);
		const uint8 QuadMaterial = SolidVoxel.MaterialID;
		const uint8 QuadBiome = SolidVoxel.BiomeID;

		// Emit vertices. A cell vertex already emitted with a different material is
		// duplicated (same position/normal, different material data) via the cache.
		int32 Indices[4];
		for (int32 i = 0; i < 4; i++)
		{
			FDCCellVertex& V = *Verts[i];
			if (V.MeshVertexIndex >= 0 && V.EmittedMaterialID == QuadMaterial && V.EmittedBiomeID == QuadBiome)
			{
				Indices[i] = V.MeshVertexIndex;
			}
			else if (V.MeshVertexIndex < 0)
			{
				Indices[i] = EmitVertex(Request, V, QuadMaterial, QuadBiome, OutMeshData);
				V.MeshVertexIndex = Indices[i];
				V.EmittedMaterialID = QuadMaterial;
				V.EmittedBiomeID = QuadBiome;
			}
			else
			{
				const uint64 DupKey = (static_cast<uint64>(CellIdxs[i]) << 16)
					| (static_cast<uint64>(QuadMaterial) << 8)
					| static_cast<uint64>(QuadBiome);
				if (const int32* Found = DuplicateVertexCache.Find(DupKey))
				{
					Indices[i] = *Found;
				}
				else
				{
					Indices[i] = EmitVertex(Request, V, QuadMaterial, QuadBiome, OutMeshData);
					DuplicateVertexCache.Add(DupKey, Indices[i]);
				}
			}
		}

		// Split the quad along the diagonal that maximises the smaller of the two triangle
		// areas (a max-min-area / Delaunay-like choice). The boundary weld can pull the four
		// cell vertices of a boundary quad onto the near-1D seam contour; the fixed 0-2
		// diagonal then makes one triangle a near-zero-area sliver that spans a neighbouring
		// welded vertex. The renderer (and the watertightness metric) drop that sliver,
		// un-sealing the seam — the T-junction that cracked shallow 4-chunk corners (DT7
		// Smooth LOD1). Choosing the diagonal whose worst triangle is largest avoids the
		// sliver and steps through the intermediate vertex, matching the neighbour's tiling.
		// For a well-shaped quad whose 0-2 split is already fine this leaves it untouched, so
		// it does not disturb corners that were already sealed (e.g. Smooth LOD2).
		auto TriArea = [](const FVector3f& A, const FVector3f& B, const FVector3f& C) -> float
		{
			return 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
		};
		const FVector3f& P0 = Verts[0]->Position;
		const FVector3f& P1 = Verts[1]->Position;
		const FVector3f& P2 = Verts[2]->Position;
		const FVector3f& P3 = Verts[3]->Position;
		const float MinArea02 = FMath::Min(TriArea(P0, P1, P2), TriArea(P0, P2, P3));
		const float MinArea13 = FMath::Min(TriArea(P1, P2, P3), TriArea(P1, P3, P0));
		const bool bUse13 = MinArea13 > MinArea02;

		auto AddTri = [&](int32 a, int32 b, int32 c)
		{
			OutMeshData.Indices.Add(Indices[a]);
			OutMeshData.Indices.Add(Indices[b]);
			OutMeshData.Indices.Add(Indices[c]);
		};

		if (bFlip)
		{
			if (bUse13) { AddTri(1, 2, 3); AddTri(1, 3, 0); }
			else        { AddTri(0, 1, 2); AddTri(0, 2, 3); }
		}
		else
		{
			if (bUse13) { AddTri(1, 3, 2); AddTri(1, 0, 3); }
			else        { AddTri(0, 2, 1); AddTri(0, 3, 2); }
		}

		OutTriangleCount += 2;
	}
}

// ============================================================================
// Pass 4: Strided Boundary Cell Welding
// ============================================================================

void FVoxelCPUDualContourMesher::WeldStridedBoundaryCells(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 GridDim,
	TArray<FDCCellVertex>& CellVertices)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 GridSize = ChunkSize / Stride;
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const int32 SelfLOD = FMath::Clamp(Request.LODLevel, 0, 7);

	// Clamped rendered LODs of every neighbor that can share a boundary feature with this
	// chunk: 6 faces + 12 edge diagonals + 8 corner diagonals. -1 (absent) clamps to 0 — an
	// absent neighbor is not coarser (and the GetVoxelAt Air fallback seals it regardless).
	//
	// A boundary FEATURE's effective weld stride must be agreed by EVERY chunk sharing it:
	// a face by 2 chunks, a chunk-edge line by 4 (self + 2 face neighbors + the edge
	// diagonal), a chunk-corner point by 8. The previous per-axis rule consulted face LODs
	// only, so at an asymmetric-LOD corner (e.g. [coarse,fine/fine,fine]) the chunk DIAGONAL
	// to the coarse one had no way to know it and kept raw fine vertices at a corner the
	// other three chunks welded coarse — the deterministic see-through corner hole confirmed
	// live in the demo (survives voxel.RemeshAll; DT9/GT9 reproduce it).
	int32 FaceL[6], EdgeL[12], CornerL[8];
	int32 MaxAnyLOD = SelfLOD;
	for (int32 i = 0; i < 6; i++)
	{
		FaceL[i] = FMath::Max(Request.NeighborLODLevels[i], 0);
		MaxAnyLOD = FMath::Max(MaxAnyLOD, FaceL[i]);
	}
	for (int32 i = 0; i < 12; i++)
	{
		EdgeL[i] = FMath::Max(Request.EdgeLODLevels[i], 0);
		MaxAnyLOD = FMath::Max(MaxAnyLOD, EdgeL[i]);
	}
	for (int32 i = 0; i < 8; i++)
	{
		CornerL[i] = FMath::Max(Request.CornerLODLevels[i], 0);
		MaxAnyLOD = FMath::Max(MaxAnyLOD, CornerL[i]);
	}
	if (MaxAnyLOD <= 0)
	{
		return; // every sharer of every boundary feature is LOD 0 — stride-1 boundaries are already watertight (DT1)
	}

	// EDGE_*-order index of the edge diagonal across boundary axes (A1,Side1) x (A2,Side2):
	// groups XY=0..3, XZ=4..7, YZ=8..11; within a group [PosPos, PosNeg, NegPos, NegNeg]
	// on the (lower axis, higher axis) sides.
	auto EdgeIndexOf = [](int32 A1, int32 S1, int32 A2, int32 S2) -> int32
	{
		if (A1 > A2) { Swap(A1, A2); Swap(S1, S2); }
		const int32 Group = (A1 == 0) ? ((A2 == 1) ? 0 : 4) : 8;
		return Group + ((S1 > 0) ? 0 : 2) + ((S2 > 0) ? 0 : 1);
	};
	// CORNER_*-order index from the three boundary sides.
	auto CornerIndexOf = [](int32 SX, int32 SY, int32 SZ) -> int32
	{
		return ((SX > 0) ? 0 : 4) + ((SY > 0) ? 0 : 2) + ((SZ > 0) ? 0 : 1);
	};

	auto Dens = [&](int32 X, int32 Y, int32 Z) -> float { return GetDensityAt(Request, X, Y, Z); };
	// Iso-crossing along one axis between two samples; returns the interpolated
	// coordinate. The crossing depends only on the two sampled densities, so any
	// neighbor sampling the same world positions gets the identical result.
	auto Cross1D = [&](float Da, float Db, float Pa, float Pb, float& Out) -> bool
	{
		if ((Da >= IsoLevel) == (Db >= IsoLevel)) { return false; }
		const float t = FMath::Clamp((IsoLevel - Da) / (Db - Da), 0.0f, 1.0f);
		Out = FMath::Lerp(Pa, Pb, t);
		return true;
	};

	// Single per-cell pass. Each boundary cell is snapped onto the shared
	// sub-feature of the boundary faces it touches: one face -> the face plane
	// (free in the two perpendicular axes), two faces -> the shared edge line
	// (free along the third axis), three faces -> the shared corner point. Every
	// chunk that touches a given feature derives it from the same shared data, so
	// the welded vertices coincide and the seam (including its edges) is sealed.
	for (int32 CZ = -1; CZ < GridSize; CZ++)
	for (int32 CY = -1; CY < GridSize; CY++)
	for (int32 CX = -1; CX < GridSize; CX++)
	{
		const int32 CIdx = CellIndex(CX, CY, CZ, GridDim);
		if (!CellVertices[CIdx].bValid) { continue; }

		const int32 C[3] = { CX, CY, CZ };

		// Boundary side per axis: -1 at the negative-face layer, +1 at the positive, 0 interior.
		int32 Side[3];
		int32 NumBoundary = 0;
		for (int32 A = 0; A < 3; A++)
		{
			Side[A] = (C[A] == -1) ? -1 : ((C[A] == GridSize - 1) ? 1 : 0);
			if (Side[A] != 0) { NumBoundary++; }
		}
		if (NumBoundary == 0) { continue; } // interior cell — keep its floating QEF vertex

		// Effective stride of this cell's boundary feature = 1 << max LOD over EVERY chunk
		// sharing it (self, the face neighbor across each boundary axis, the edge diagonal
		// across each boundary axis pair, and — for a 3-axis cell — the corner diagonal).
		// Every sharer evaluates the same chunk set, so all of them pin (or don't) at the
		// same stride and their welded vertices coincide.
		int32 FeatureLOD = SelfLOD;
		int32 MinSharerLOD = SelfLOD;
		for (int32 A = 0; A < 3; A++)
		{
			if (Side[A] != 0)
			{
				const int32 L = FaceL[2 * A + ((Side[A] > 0) ? 1 : 0)];
				FeatureLOD = FMath::Max(FeatureLOD, L);
				MinSharerLOD = FMath::Min(MinSharerLOD, L);
			}
		}
		for (int32 A1 = 0; A1 < 3; A1++)
		{
			for (int32 A2 = A1 + 1; A2 < 3; A2++)
			{
				if (Side[A1] != 0 && Side[A2] != 0)
				{
					const int32 L = EdgeL[EdgeIndexOf(A1, Side[A1], A2, Side[A2])];
					FeatureLOD = FMath::Max(FeatureLOD, L);
					MinSharerLOD = FMath::Min(MinSharerLOD, L);
				}
			}
		}
		if (NumBoundary == 3)
		{
			const int32 L = CornerL[CornerIndexOf(Side[0], Side[1], Side[2])];
			FeatureLOD = FMath::Max(FeatureLOD, L);
			MinSharerLOD = FMath::Min(MinSharerLOD, L);
		}

		// Uniform-stride sharer sets keep their RAW vertices when no feature crossing exists:
		// every sharer holds an identical duplicate of the boundary cell (same world cube, same
		// boundary data), so the raws already coincide — while any side-local fallback would
		// DE-synchronize them (descent patches quantize on each chunk's own side). Mixed-stride
		// sharers are the opposite: a coarse cell's raw can never coincide with the fine cells'
		// raws, so ONLY the shared-feature fallbacks below can seal them (DT9/GT9).
		const bool bMixedSharers = (MinSharerLOD != FeatureLOD);

		const int32 CellE = 1 << FeatureLOD;
		if (CellE <= 1) { continue; } // all sharers at LOD 0 — full-res boundary, already watertight

		// Pin every boundary axis of the cell at the agreed feature stride.
		bool Pinned[3];
		int32 PlaneV[3];
		int32 Eff[3];
		for (int32 A = 0; A < 3; A++)
		{
			Pinned[A] = (Side[A] != 0);
			PlaneV[A] = (Side[A] > 0) ? ChunkSize : 0;
			Eff[A] = CellE;
		}
		const int32 NP = NumBoundary;

		FVector3f Pos = FVector3f::ZeroVector;
		bool bHas = false;

		if (NP == 3)
		{
			// Shared chunk corner point.
			Pos = FVector3f(PlaneV[0] * VoxelSize, PlaneV[1] * VoxelSize, PlaneV[2] * VoxelSize);
			bHas = true;
		}
		else if (NP == 2)
		{
			// Shared edge line: two axes pinned to their planes; the vertex slides
			// along the free axis to the iso-crossing of the shared edge column.
			int32 A1 = -1, A2 = -1, F = -1;
			for (int32 A = 0; A < 3; A++)
			{
				if (Pinned[A]) { if (A1 < 0) { A1 = A; } else { A2 = A; } }
				else { F = A; }
			}
			const int32 E = FMath::Max(Eff[A1], Eff[A2]);
			const int32 FV = C[F] * Stride;
			const int32 F0 = FMath::Clamp((FV >= 0 ? (FV / E) * E : 0), 0, ChunkSize - E);
			auto EdgeDens = [&](int32 Fc) -> float
			{
				int32 V[3]; V[A1] = PlaneV[A1]; V[A2] = PlaneV[A2]; V[F] = Fc;
				return Dens(V[0], V[1], V[2]);
			};
			float Crossed;
			if (Cross1D(EdgeDens(F0), EdgeDens(F0 + E), (float)F0, (float)(F0 + E), Crossed))
			{
				Pos[A1] = PlaneV[A1] * VoxelSize;
				Pos[A2] = PlaneV[A2] * VoxelSize;
				Pos[F] = Crossed * VoxelSize;
				bHas = true;
			}
			else if (bMixedSharers)
			{
				// No iso-crossing on the shared edge line near this cell: the surface passes
				// through the cell OFF the line — typically it meets one of the two FACE
				// planes instead (a coarse corner cell spans several fine face patches, and
				// its surface may cross a face patch whose fine cells welded onto that plane).
				// Descend the feature hierarchy: try an NP==1-style on-plane crossing weld on
				// each pinned plane (at this cell's quantized patch — shared with the sharer
				// across that plane), lowest axis first so every sharer attempts the same
				// order. Only if no feature near this cell carries the surface, snap to the
				// line window's midpoint — deterministic and identical for every sharer.
				// Uniform-stride cells skip this (see bMixedSharers above): their raws
				// already coincide and the side-local patches here would break that.
				auto TryPlaneCrossings = [&](int32 DAxis) -> bool
				{
					const int32 PAa = (DAxis == 0) ? 1 : 0;
					const int32 PBb = (DAxis == 2) ? 1 : 2;
					const int32 PE = Eff[DAxis];
					const int32 PPlane = PlaneV[DAxis];
					const int32 PUv = C[PAa] * Stride;
					const int32 PVv = C[PBb] * Stride;
					const int32 PU = FMath::Clamp((PUv >= 0 ? (PUv / PE) * PE : 0), 0, ChunkSize - PE);
					const int32 PV = FMath::Clamp((PVv >= 0 ? (PVv / PE) * PE : 0), 0, ChunkSize - PE);
					auto PSD = [&](int32 P1, int32 P2) -> float
					{
						int32 Vc[3]; Vc[DAxis] = PPlane; Vc[PAa] = P1; Vc[PBb] = P2;
						return Dens(Vc[0], Vc[1], Vc[2]);
					};
					auto PWD = [&](float P1, float P2) -> FVector3f
					{
						FVector3f W;
						W[DAxis] = PPlane * VoxelSize; W[PAa] = P1 * VoxelSize; W[PBb] = P2 * VoxelSize;
						return W;
					};
					FVector3f PSum = FVector3f::ZeroVector;
					int32 PCount = 0;
					auto PAdd = [&](float Da, float Db, const FVector3f& Pa, const FVector3f& Pb)
					{
						if ((Da >= IsoLevel) == (Db >= IsoLevel)) { return; }
						const float t = FMath::Clamp((IsoLevel - Da) / (Db - Da), 0.0f, 1.0f);
						PSum += FMath::Lerp(Pa, Pb, t);
						++PCount;
					};
					const float PD00 = PSD(PU, PV);
					const float PD10 = PSD(PU + PE, PV);
					const float PD01 = PSD(PU, PV + PE);
					const float PD11 = PSD(PU + PE, PV + PE);
					PAdd(PD00, PD10, PWD(PU, PV), PWD(PU + PE, PV));
					PAdd(PD01, PD11, PWD(PU, PV + PE), PWD(PU + PE, PV + PE));
					PAdd(PD00, PD01, PWD(PU, PV), PWD(PU, PV + PE));
					PAdd(PD10, PD11, PWD(PU + PE, PV), PWD(PU + PE, PV + PE));
					if (PCount == 0) { return false; }
					Pos = PSum / static_cast<float>(PCount);
					return true;
				};
				if (TryPlaneCrossings(A1) || TryPlaneCrossings(A2))
				{
					bHas = true;
				}
				else
				{
					Pos[A1] = PlaneV[A1] * VoxelSize;
					Pos[A2] = PlaneV[A2] * VoxelSize;
					Pos[F] = ((float)F0 + (float)E * 0.5f) * VoxelSize;
					bHas = true;
				}
			}
		}
		else // NP == 1: face plane
		{
			int32 D = 0;
			for (int32 A = 0; A < 3; A++) { if (Pinned[A]) { D = A; } }
			const int32 PAa = (D == 0) ? 1 : 0;
			const int32 PBb = (D == 2) ? 1 : 2;
			const int32 E = Eff[D];
			const int32 Plane = PlaneV[D];
			const int32 In = Plane - 1; // one fine voxel into the shared slab (world plane-1)
			const int32 Uv = C[PAa] * Stride;
			const int32 Vv = C[PBb] * Stride;
			const int32 U = FMath::Clamp((Uv >= 0 ? (Uv / E) * E : 0), 0, ChunkSize - E);
			const int32 V = FMath::Clamp((Vv >= 0 ? (Vv / E) * E : 0), 0, ChunkSize - E);
			auto SD = [&](int32 Depth, int32 P1, int32 P2) -> float
			{
				int32 Vc[3]; Vc[D] = Depth; Vc[PAa] = P1; Vc[PBb] = P2;
				return Dens(Vc[0], Vc[1], Vc[2]);
			};
			auto WD = [&](float Depth, float P1, float P2) -> FVector3f
			{
				FVector3f W; W[D] = Depth * VoxelSize; W[PAa] = P1 * VoxelSize; W[PBb] = P2 * VoxelSize;
				return W;
			};
			FVector3f Sum = FVector3f::ZeroVector;
			int32 Count = 0;
			auto AddV = [&](float Da, float Db, const FVector3f& Pa, const FVector3f& Pb)
			{
				if ((Da >= IsoLevel) == (Db >= IsoLevel)) { return; }
				const float t = FMath::Clamp((IsoLevel - Da) / (Db - Da), 0.0f, 1.0f);
				Sum += FMath::Lerp(Pa, Pb, t);
				++Count;
			};
			const float D00 = SD(Plane, U, V);
			const float D10 = SD(Plane, U + E, V);
			const float D01 = SD(Plane, U, V + E);
			const float D11 = SD(Plane, U + E, V + E);
			// Prefer the iso-crossings ON the face plane (the clean, exact-coincidence
			// case — used whenever the surface meets the face). Both neighbors get the
			// identical crossings from the shared plane densities.
			AddV(D00, D10, WD(Plane, U, V), WD(Plane, U + E, V));
			AddV(D01, D11, WD(Plane, U, V + E), WD(Plane, U + E, V + E));
			AddV(D00, D01, WD(Plane, U, V), WD(Plane, U, V + E));
			AddV(D10, D11, WD(Plane, U + E, V), WD(Plane, U + E, V + E));
			if (Count == 0)
			{
				// No on-plane crossing: a steep cell whose surface crosses perpendicular
				// to the plane. Fall back to the 4 corner edges crossing one fine voxel
				// into the shared slab (plane-1 -> plane) so the cell is not dropped.
				AddV(SD(In, U, V), D00, WD(In, U, V), WD(Plane, U, V));
				AddV(SD(In, U + E, V), D10, WD(In, U + E, V), WD(Plane, U + E, V));
				AddV(SD(In, U, V + E), D01, WD(In, U, V + E), WD(Plane, U, V + E));
				AddV(SD(In, U + E, V + E), D11, WD(In, U + E, V + E), WD(Plane, U + E, V + E));
			}
			if (Count > 0)
			{
				Pos = Sum / static_cast<float>(Count);
				bHas = true;
			}
			else if (bMixedSharers)
			{
				// No crossing on the plane patch or its one-voxel slab: snap to the patch
				// center on the plane — deterministic and identical for both sharers of the
				// face. A raw QEF keep at a coarse|fine face guarantees a mismatch (only a
				// SAME-stride pair's raws coincide — those keep raw via the !bHas tail).
				Pos = WD((float)Plane, (float)U + (float)E * 0.5f, (float)V + (float)E * 0.5f);
				bHas = true;
			}
		}

		if (!bHas)
		{
			// Uniform-stride sharers with no feature crossing near this cell: every sharer
			// holds an identical duplicate of the cell, so the raw QEF vertices already
			// coincide — keep them (a side-local fallback would de-synchronize the duplicates:
			// the GT7 uniform-corner regression). Mixed-stride cells never reach here — every
			// mixed branch above resolves to a shared feature position.
			continue;
		}

		FDCCellVertex& Welded = CellVertices[CIdx];
		Welded.Position = Pos;
		Welded.MeshVertexIndex = -1;
		int32 NEff = Stride;
		for (int32 A = 0; A < 3; A++) { if (Pinned[A]) { NEff = FMath::Max(NEff, Eff[A]); } }
		Welded.Normal = CalculateGradientNormalLOD(Request,
			Pos.X / VoxelSize, Pos.Y / VoxelSize, Pos.Z / VoxelSize, NEff);
		// Material/biome are per-quad (assigned in GenerateQuads) — geometry
		// coincidence is what seals the seam.
	}
}

// ============================================================================
// Vertex Emission
// ============================================================================

int32 FVoxelCPUDualContourMesher::EmitVertex(
	const FVoxelMeshingRequest& Request,
	const FDCCellVertex& Vertex,
	uint8 MaterialID,
	uint8 BiomeID,
	FChunkMeshData& OutMeshData) const
{
	const float VoxelSize = Request.VoxelSize;
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;

	const int32 Index = OutMeshData.Positions.Num();

	OutMeshData.Positions.Add(Vertex.Position);
	OutMeshData.Normals.Add(Vertex.Normal);

	// Triplanar UV projection based on normal direction
	const float AbsX = FMath::Abs(Vertex.Normal.X);
	const float AbsY = FMath::Abs(Vertex.Normal.Y);
	const float AbsZ = FMath::Abs(Vertex.Normal.Z);

	FVector2f UV;
	if (AbsZ >= AbsX && AbsZ >= AbsY)
	{
		UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Y * UVScale / VoxelSize);
	}
	else if (AbsX >= AbsY)
	{
		UV = FVector2f(Vertex.Position.Y * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
	}
	else
	{
		UV = FVector2f(Vertex.Position.X * UVScale / VoxelSize, Vertex.Position.Z * UVScale / VoxelSize);
	}

	OutMeshData.UVs.Add(UV);

	// UV1: MaterialID + reserved (same format as smooth mesher)
	OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));

	// Vertex color: R=MaterialID, G=BiomeID (same format as smooth mesher)
	OutMeshData.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));

	return Index;
}

// ============================================================================
// Density & Voxel Access (copied from FVoxelCPUMarchingCubesMesher)
// ============================================================================

float FVoxelCPUDualContourMesher::GetDensityAt(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	const FVoxelData Voxel = GetVoxelAt(Request, X, Y, Z);
	return static_cast<float>(Voxel.Density) / 255.0f;
}

FVoxelData FVoxelCPUDualContourMesher::GetVoxelAt(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	const int32 ChunkSize = Request.ChunkSize;

	// Check if within chunk bounds
	if (X >= 0 && X < ChunkSize && Y >= 0 && Y < ChunkSize && Z >= 0 && Z < ChunkSize)
	{
		return Request.GetVoxel(X, Y, Z);
	}

	// Handle neighbor chunk lookups
	const int32 SliceSize = ChunkSize * ChunkSize;

	const int32 ClampedX = FMath::Clamp(X, 0, ChunkSize - 1);
	const int32 ClampedY = FMath::Clamp(Y, 0, ChunkSize - 1);
	const int32 ClampedZ = FMath::Clamp(Z, 0, ChunkSize - 1);

	const bool bXPos = (X >= ChunkSize);
	const bool bXNeg = (X < 0);
	const bool bYPos = (Y >= ChunkSize);
	const bool bYNeg = (Y < 0);
	const bool bZPos = (Z >= ChunkSize);
	const bool bZNeg = (Z < 0);

	const bool bOutX = bXPos || bXNeg;
	const bool bOutY = bYPos || bYNeg;
	const bool bOutZ = bZPos || bZNeg;
	const int32 OutCount = (bOutX ? 1 : 0) + (bOutY ? 1 : 0) + (bOutZ ? 1 : 0);

	// 0-based depth into a neighbor along each out-of-bounds axis (0 = the first plane
	// just past the chunk face). Used to read deep edge/corner data for strided cells.
	const int32 DepthX = bXPos ? (X - ChunkSize) : (bXNeg ? (-X - 1) : 0);
	const int32 DepthY = bYPos ? (Y - ChunkSize) : (bYNeg ? (-Y - 1) : 0);
	const int32 DepthZ = bZPos ? (Z - ChunkSize) : (bZNeg ? (-Z - 1) : 0);

	// Single-axis out of bounds: use face neighbor data
	if (OutCount == 1)
	{
		// Read the correct DEPTH plane into the neighbor (PlaneIdx 0 = the face slice,
		// deeper planes from the Deep arrays). At stride 1 / LOD 0 PlaneIdx is 0 and
		// this is identical to the single-plane behavior.
		if (bXPos && Request.NeighborXPos.Num() == SliceSize)
		{
			return Request.NeighborPlaneVoxel(Request.NeighborXPos, Request.NeighborXPosDeep, Y + Z * ChunkSize, X - ChunkSize);
		}
		if (bXNeg && Request.NeighborXNeg.Num() == SliceSize)
		{
			return Request.NeighborPlaneVoxel(Request.NeighborXNeg, Request.NeighborXNegDeep, Y + Z * ChunkSize, -X - 1);
		}
		if (bYPos && Request.NeighborYPos.Num() == SliceSize)
		{
			return Request.NeighborPlaneVoxel(Request.NeighborYPos, Request.NeighborYPosDeep, X + Z * ChunkSize, Y - ChunkSize);
		}
		if (bYNeg && Request.NeighborYNeg.Num() == SliceSize)
		{
			return Request.NeighborPlaneVoxel(Request.NeighborYNeg, Request.NeighborYNegDeep, X + Z * ChunkSize, -Y - 1);
		}
		if (bZPos && Request.NeighborZPos.Num() == SliceSize)
		{
			return Request.NeighborPlaneVoxel(Request.NeighborZPos, Request.NeighborZPosDeep, X + Y * ChunkSize, Z - ChunkSize);
		}
		if (bZNeg && Request.NeighborZNeg.Num() == SliceSize)
		{
			return Request.NeighborPlaneVoxel(Request.NeighborZNeg, Request.NeighborZNegDeep, X + Y * ChunkSize, -Z - 1);
		}
		// Neighbor data unavailable — return Air so that edge crossings at chunk
		// boundaries are detected (solid→air transition). The geometry will be
		// refined when the neighbor loads and triggers a remesh.
		return FVoxelData::Air();
	}

	// Edge case (2 axes out of bounds): use edge neighbor data. For strided cells the
	// outward cell reaches diagonally into the edge neighbor, so read the deep grid at
	// (DepthA, DepthB); at stride 1 / LOD0 both depths are 0 and this returns the base
	// strip exactly as before. The free axis (in-range) indexes the strip.
	if (OutCount == 2)
	{
		// XY edges (Z free)
		if (bXPos && bYPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS))
		{
			return Request.EdgeDeepVoxel(Request.EdgeXPosYPos, Request.EdgeXPosYPosDeep, ClampedZ, DepthX, DepthY);
		}
		if (bXPos && bYNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG))
		{
			return Request.EdgeDeepVoxel(Request.EdgeXPosYNeg, Request.EdgeXPosYNegDeep, ClampedZ, DepthX, DepthY);
		}
		if (bXNeg && bYPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS))
		{
			return Request.EdgeDeepVoxel(Request.EdgeXNegYPos, Request.EdgeXNegYPosDeep, ClampedZ, DepthX, DepthY);
		}
		if (bXNeg && bYNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG))
		{
			return Request.EdgeDeepVoxel(Request.EdgeXNegYNeg, Request.EdgeXNegYNegDeep, ClampedZ, DepthX, DepthY);
		}

		// XZ edges (Y free)
		if (bXPos && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS))
		{
			return Request.EdgeDeepVoxel(Request.EdgeXPosZPos, Request.EdgeXPosZPosDeep, ClampedY, DepthX, DepthZ);
		}
		if (bXPos && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG))
		{
			return Request.EdgeDeepVoxel(Request.EdgeXPosZNeg, Request.EdgeXPosZNegDeep, ClampedY, DepthX, DepthZ);
		}
		if (bXNeg && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS))
		{
			return Request.EdgeDeepVoxel(Request.EdgeXNegZPos, Request.EdgeXNegZPosDeep, ClampedY, DepthX, DepthZ);
		}
		if (bXNeg && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG))
		{
			return Request.EdgeDeepVoxel(Request.EdgeXNegZNeg, Request.EdgeXNegZNegDeep, ClampedY, DepthX, DepthZ);
		}

		// YZ edges (X free)
		if (bYPos && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS))
		{
			return Request.EdgeDeepVoxel(Request.EdgeYPosZPos, Request.EdgeYPosZPosDeep, ClampedX, DepthY, DepthZ);
		}
		if (bYPos && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG))
		{
			return Request.EdgeDeepVoxel(Request.EdgeYPosZNeg, Request.EdgeYPosZNegDeep, ClampedX, DepthY, DepthZ);
		}
		if (bYNeg && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS))
		{
			return Request.EdgeDeepVoxel(Request.EdgeYNegZPos, Request.EdgeYNegZPosDeep, ClampedX, DepthY, DepthZ);
		}
		if (bYNeg && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG))
		{
			return Request.EdgeDeepVoxel(Request.EdgeYNegZNeg, Request.EdgeYNegZNegDeep, ClampedX, DepthY, DepthZ);
		}

		return FVoxelData::Air();
	}

	// Corner case (3 axes out of bounds): use corner neighbor data. For strided cells the
	// outward cell reaches along the body diagonal into the corner neighbor, so read the
	// deep box at (DepthX, DepthY, DepthZ); at stride 1 / LOD0 all depths are 0 and this
	// returns the single corner voxel exactly as before.
	if (OutCount == 3)
	{
		if (bXPos && bYPos && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS))
		{
			return Request.CornerDeepVoxel(Request.CornerXPosYPosZPos, Request.CornerXPosYPosZPosDeep, DepthX, DepthY, DepthZ);
		}
		if (bXPos && bYPos && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG))
		{
			return Request.CornerDeepVoxel(Request.CornerXPosYPosZNeg, Request.CornerXPosYPosZNegDeep, DepthX, DepthY, DepthZ);
		}
		if (bXPos && bYNeg && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS))
		{
			return Request.CornerDeepVoxel(Request.CornerXPosYNegZPos, Request.CornerXPosYNegZPosDeep, DepthX, DepthY, DepthZ);
		}
		if (bXPos && bYNeg && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG))
		{
			return Request.CornerDeepVoxel(Request.CornerXPosYNegZNeg, Request.CornerXPosYNegZNegDeep, DepthX, DepthY, DepthZ);
		}
		if (bXNeg && bYPos && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS))
		{
			return Request.CornerDeepVoxel(Request.CornerXNegYPosZPos, Request.CornerXNegYPosZPosDeep, DepthX, DepthY, DepthZ);
		}
		if (bXNeg && bYPos && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG))
		{
			return Request.CornerDeepVoxel(Request.CornerXNegYPosZNeg, Request.CornerXNegYPosZNegDeep, DepthX, DepthY, DepthZ);
		}
		if (bXNeg && bYNeg && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS))
		{
			return Request.CornerDeepVoxel(Request.CornerXNegYNegZPos, Request.CornerXNegYNegZPosDeep, DepthX, DepthY, DepthZ);
		}
		if (bXNeg && bYNeg && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG))
		{
			return Request.CornerDeepVoxel(Request.CornerXNegYNegZNeg, Request.CornerXNegYNegZNegDeep, DepthX, DepthY, DepthZ);
		}

		return FVoxelData::Air();
	}

	return FVoxelData::Air();
}

FVector3f FVoxelCPUDualContourMesher::CalculateGradientNormal(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z) const
{
	const int32 IX = FMath::FloorToInt(X);
	const int32 IY = FMath::FloorToInt(Y);
	const int32 IZ = FMath::FloorToInt(Z);

	float gx = GetDensityAt(Request, IX + 1, IY, IZ) - GetDensityAt(Request, IX - 1, IY, IZ);
	float gy = GetDensityAt(Request, IX, IY + 1, IZ) - GetDensityAt(Request, IX, IY - 1, IZ);
	float gz = GetDensityAt(Request, IX, IY, IZ + 1) - GetDensityAt(Request, IX, IY, IZ - 1);

	FVector3f Normal(-gx, -gy, -gz);

	if (!Normal.Normalize())
	{
		return FVector3f(0.0f, 0.0f, 1.0f);
	}

	return Normal;
}

FVector3f FVoxelCPUDualContourMesher::CalculateGradientNormalLOD(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z,
	int32 Stride) const
{
	const int32 IX = FMath::FloorToInt(X);
	const int32 IY = FMath::FloorToInt(Y);
	const int32 IZ = FMath::FloorToInt(Z);

	float gx = GetDensityAt(Request, IX + Stride, IY, IZ) - GetDensityAt(Request, IX - Stride, IY, IZ);
	float gy = GetDensityAt(Request, IX, IY + Stride, IZ) - GetDensityAt(Request, IX, IY - Stride, IZ);
	float gz = GetDensityAt(Request, IX, IY, IZ + Stride) - GetDensityAt(Request, IX, IY, IZ - Stride);

	FVector3f Normal(-gx, -gy, -gz);

	if (!Normal.Normalize())
	{
		return FVector3f(0.0f, 0.0f, 1.0f);
	}

	return Normal;
}

// ============================================================================
// Skirt Generation (LOD Seam Hiding)
// ============================================================================

void FVoxelCPUDualContourMesher::GenerateSkirts(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	const uint32 TrisBefore = OutTriangleCount;
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const float SkirtDepth = Config.SkirtDepth * VoxelSize * Stride;
	const float ChunkWorldSize = static_cast<float>(ChunkSize) * VoxelSize;

	// Tolerance for boundary detection — larger to catch QEF-solved vertices near boundary
	const float BoundaryTolerance = VoxelSize * Stride * 0.6f;

	const int32 OriginalVertexCount = OutMeshData.Positions.Num();
	const int32 OriginalIndexCount = OutMeshData.Indices.Num();

	const uint8 TransitionMask = Request.TransitionFaces;

	// Only horizontal faces (±X, ±Y) — vertical seams are rare and less visible
	for (int32 Face = 0; Face < 4; Face++)
	{
		static const uint8 FaceTransitionFlags[4] = {
			FVoxelMeshingRequest::TRANSITION_XNEG,
			FVoxelMeshingRequest::TRANSITION_XPOS,
			FVoxelMeshingRequest::TRANSITION_YNEG,
			FVoxelMeshingRequest::TRANSITION_YPOS,
		};

		if ((TransitionMask & FaceTransitionFlags[Face]) == 0)
		{
			continue;
		}

		struct FBoundaryEdge
		{
			int32 V0, V1;
		};
		TArray<FBoundaryEdge> BoundaryEdges;

		const bool bIsXFace = (Face == 0 || Face == 1);
		const bool bIsPositiveFace = (Face == 1 || Face == 3);
		const float BoundaryValue = bIsPositiveFace ? ChunkWorldSize : 0.0f;

		const FVector3f SkirtDir = FVector3f(0.0f, 0.0f, -1.0f);

		// Find edges on this boundary from original mesh triangles
		const int32 NumTriangles = OriginalIndexCount / 3;
		for (int32 TriIdx = 0; TriIdx < NumTriangles; TriIdx++)
		{
			const int32 BaseIdx = TriIdx * 3;
			const int32 Idx0 = OutMeshData.Indices[BaseIdx];
			const int32 Idx1 = OutMeshData.Indices[BaseIdx + 1];
			const int32 Idx2 = OutMeshData.Indices[BaseIdx + 2];

			if (Idx0 >= OriginalVertexCount || Idx1 >= OriginalVertexCount || Idx2 >= OriginalVertexCount)
			{
				continue;
			}

			const FVector3f& P0 = OutMeshData.Positions[Idx0];
			const FVector3f& P1 = OutMeshData.Positions[Idx1];
			const FVector3f& P2 = OutMeshData.Positions[Idx2];

			auto IsOnBoundary = [&](const FVector3f& P) -> bool
			{
				const float Coord = bIsXFace ? P.X : P.Y;
				return FMath::Abs(Coord - BoundaryValue) < BoundaryTolerance;
			};

			const bool b0 = IsOnBoundary(P0);
			const bool b1 = IsOnBoundary(P1);
			const bool b2 = IsOnBoundary(P2);

			auto AddEdgeIfOnBoundary = [&](int32 IdxA, int32 IdxB, bool bA, bool bB)
			{
				if (bA && bB)
				{
					BoundaryEdges.Add({IdxA, IdxB});
				}
			};

			AddEdgeIfOnBoundary(Idx0, Idx1, b0, b1);
			AddEdgeIfOnBoundary(Idx1, Idx2, b1, b2);
			AddEdgeIfOnBoundary(Idx2, Idx0, b2, b0);
		}

		const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;

		for (const FBoundaryEdge& Edge : BoundaryEdges)
		{
			// Copy vertex data (adding to arrays may reallocate)
			const FVector3f P0 = OutMeshData.Positions[Edge.V0];
			const FVector3f P1 = OutMeshData.Positions[Edge.V1];
			const FColor C0 = OutMeshData.Colors[Edge.V0];
			const FColor C1 = OutMeshData.Colors[Edge.V1];
			const FVector2f MatUV0 = OutMeshData.UV1s.IsValidIndex(Edge.V0) ? OutMeshData.UV1s[Edge.V0] : FVector2f::ZeroVector;
			const FVector2f MatUV1 = OutMeshData.UV1s.IsValidIndex(Edge.V1) ? OutMeshData.UV1s[Edge.V1] : FVector2f::ZeroVector;

			const FVector3f Bottom0 = P0 + SkirtDir * SkirtDepth;
			const FVector3f Bottom1 = P1 + SkirtDir * SkirtDepth;

			FVector3f SkirtNormal;
			if (bIsXFace)
			{
				SkirtNormal = bIsPositiveFace ? FVector3f(1, 0, 0) : FVector3f(-1, 0, 0);
			}
			else
			{
				SkirtNormal = bIsPositiveFace ? FVector3f(0, 1, 0) : FVector3f(0, -1, 0);
			}

			auto CalcUV = [&](const FVector3f& Pos) -> FVector2f
			{
				if (bIsXFace)
				{
					return FVector2f(Pos.Y * UVScale / VoxelSize, Pos.Z * UVScale / VoxelSize);
				}
				else
				{
					return FVector2f(Pos.X * UVScale / VoxelSize, Pos.Z * UVScale / VoxelSize);
				}
			};

			const FVector2f UV0 = CalcUV(P0);
			const FVector2f UV1 = CalcUV(P1);
			const FVector2f UVBottom0 = CalcUV(Bottom0);
			const FVector2f UVBottom1 = CalcUV(Bottom1);

			const uint32 BaseVertex = OutMeshData.Positions.Num();

			OutMeshData.Positions.Add(P0);
			OutMeshData.Positions.Add(Bottom0);
			OutMeshData.Positions.Add(P1);
			OutMeshData.Positions.Add(Bottom1);

			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);

			OutMeshData.UVs.Add(UV0);
			OutMeshData.UVs.Add(UVBottom0);
			OutMeshData.UVs.Add(UV1);
			OutMeshData.UVs.Add(UVBottom1);

			OutMeshData.UV1s.Add(FVector2f(MatUV0.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV0.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV1.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV1.X, 0.0f));

			OutMeshData.Colors.Add(C0);
			OutMeshData.Colors.Add(C0);
			OutMeshData.Colors.Add(C1);
			OutMeshData.Colors.Add(C1);

			if (bIsPositiveFace)
			{
				OutMeshData.Indices.Add(BaseVertex + 0);
				OutMeshData.Indices.Add(BaseVertex + 1);
				OutMeshData.Indices.Add(BaseVertex + 2);

				OutMeshData.Indices.Add(BaseVertex + 2);
				OutMeshData.Indices.Add(BaseVertex + 1);
				OutMeshData.Indices.Add(BaseVertex + 3);
			}
			else
			{
				OutMeshData.Indices.Add(BaseVertex + 0);
				OutMeshData.Indices.Add(BaseVertex + 2);
				OutMeshData.Indices.Add(BaseVertex + 1);

				OutMeshData.Indices.Add(BaseVertex + 2);
				OutMeshData.Indices.Add(BaseVertex + 3);
				OutMeshData.Indices.Add(BaseVertex + 1);
			}

			OutTriangleCount += 2;
		}
	}

	const uint32 SkirtTris = OutTriangleCount - TrisBefore;
	if (SkirtTris > 0)
	{
		UE_LOG(LogVoxelMeshing, Verbose, TEXT("DC: Generated %d skirt triangles for chunk (%d,%d,%d) at LOD %d"),
			SkirtTris, Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, Request.LODLevel);
	}
}

// ============================================================================
// Async Pattern (wraps sync for CPU mesher)
// ============================================================================

FVoxelMeshingHandle FVoxelCPUDualContourMesher::GenerateMeshAsync(
	const FVoxelMeshingRequest& Request,
	FOnVoxelMeshingComplete OnComplete)
{
	const uint64 RequestId = NextRequestId.fetch_add(1);
	FVoxelMeshingHandle Handle(RequestId, Request.ChunkCoord);

	FCachedResult Result;
	Result.bSuccess = GenerateMeshCPU(Request, Result.MeshData, Result.Stats);

	{
		FScopeLock Lock(&CacheLock);
		CachedResults.Add(RequestId, MoveTemp(Result));
	}

	Handle.bIsComplete = true;
	Handle.bWasSuccessful = Result.bSuccess;

	if (OnComplete.IsBound())
	{
		OnComplete.Execute(Handle, Handle.bWasSuccessful);
	}

	return Handle;
}

bool FVoxelCPUDualContourMesher::IsComplete(const FVoxelMeshingHandle& Handle) const
{
	return Handle.bIsComplete;
}

bool FVoxelCPUDualContourMesher::WasSuccessful(const FVoxelMeshingHandle& Handle) const
{
	return Handle.bWasSuccessful;
}

FRHIBuffer* FVoxelCPUDualContourMesher::GetVertexBuffer(const FVoxelMeshingHandle& Handle)
{
	return nullptr;
}

FRHIBuffer* FVoxelCPUDualContourMesher::GetIndexBuffer(const FVoxelMeshingHandle& Handle)
{
	return nullptr;
}

bool FVoxelCPUDualContourMesher::GetBufferCounts(
	const FVoxelMeshingHandle& Handle,
	uint32& OutVertexCount,
	uint32& OutIndexCount) const
{
	FScopeLock Lock(&CacheLock);
	const FCachedResult* Result = CachedResults.Find(Handle.RequestId);
	if (Result && Result->bSuccess)
	{
		OutVertexCount = Result->MeshData.GetVertexCount();
		OutIndexCount = Result->MeshData.Indices.Num();
		return true;
	}
	return false;
}

bool FVoxelCPUDualContourMesher::GetRenderData(
	const FVoxelMeshingHandle& Handle,
	FChunkRenderData& OutRenderData)
{
	FScopeLock Lock(&CacheLock);
	const FCachedResult* Result = CachedResults.Find(Handle.RequestId);
	if (Result && Result->bSuccess)
	{
		OutRenderData.ChunkCoord = Handle.ChunkCoord;
		OutRenderData.VertexCount = Result->MeshData.GetVertexCount();
		OutRenderData.IndexCount = Result->MeshData.Indices.Num();
		return true;
	}
	return false;
}

bool FVoxelCPUDualContourMesher::ReadbackToCPU(
	const FVoxelMeshingHandle& Handle,
	FChunkMeshData& OutMeshData)
{
	FScopeLock Lock(&CacheLock);
	const FCachedResult* Result = CachedResults.Find(Handle.RequestId);
	if (Result && Result->bSuccess)
	{
		OutMeshData = Result->MeshData;
		return true;
	}
	return false;
}

void FVoxelCPUDualContourMesher::ReleaseHandle(const FVoxelMeshingHandle& Handle)
{
	FScopeLock Lock(&CacheLock);
	CachedResults.Remove(Handle.RequestId);
}

void FVoxelCPUDualContourMesher::ReleaseAllHandles()
{
	FScopeLock Lock(&CacheLock);
	CachedResults.Empty();
}

void FVoxelCPUDualContourMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelCPUDualContourMesher::GetConfig() const
{
	return Config;
}

bool FVoxelCPUDualContourMesher::GetStats(
	const FVoxelMeshingHandle& Handle,
	FVoxelMeshingStats& OutStats) const
{
	FScopeLock Lock(&CacheLock);
	const FCachedResult* Result = CachedResults.Find(Handle.RequestId);
	if (Result)
	{
		OutStats = Result->Stats;
		return true;
	}
	return false;
}
