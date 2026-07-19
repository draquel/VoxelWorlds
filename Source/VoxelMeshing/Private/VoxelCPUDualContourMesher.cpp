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

	// Seam-ownership P1 (SEAM_OWNERSHIP_ARCHITECTURE.md §2.1): the Interior domain meshes only
	// cells with zero neighbor dependence — boundary geometry is produced by single-owner seam
	// jobs instead, so the boundary weld and skirts (both boundary-reconciliation mechanisms)
	// do not apply to an interior-only pass.
	const bool bInteriorDomain = (Request.MeshCellDomain == EVoxelMeshCellDomain::Interior);

	// Pass 3.5: Weld strided boundary cells onto the shared chunk-face plane so both
	// sides of every stride>1 boundary (same-LOD strided AND fine|coarse LOD
	// transitions) derive the boundary vertex from the same shared face-plane data
	// and coincide. Stride-1 boundaries are left untouched (already watertight).
	if (!bInteriorDomain && CVarDCBoundaryWeld.GetValueOnAnyThread() != 0)
	{
		WeldStridedBoundaryCells(Request, Stride, GridDim, CellVertices);
	}

	// Pass 3: Generate quads
	GenerateQuads(Request, Stride, GridDim, EdgeCrossings, ValidEdgeIndices, CellVertices, OutMeshData, TriangleCount);

	// Generate skirts at LOD transition boundaries (when Transvoxel is disabled)
	if (!bInteriorDomain && Config.bGenerateSkirts && Request.TransitionFaces != 0)
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
	//
	// Interior domain (seam-ownership P1): solve only the interior cells [0, GridSize-1) —
	// the -1 and GridSize-1 boundary layers belong to the single-owner seam jobs.
	const bool bInteriorDomain = (Request.MeshCellDomain == EVoxelMeshCellDomain::Interior);
	const int32 CellMin = bInteriorDomain ? 0 : -1;
	const int32 CellMaxEx = bInteriorDomain ? (GridSize - 1) : GridSize;
	for (int32 CZ = CellMin; CZ < CellMaxEx; CZ++)
	{
		for (int32 CY = CellMin; CY < CellMaxEx; CY++)
		{
			for (int32 CX = CellMin; CX < CellMaxEx; CX++)
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

	// Interior domain (seam-ownership P1): quads may only reference interior cells
	// ([0, GridSize-1) per axis). Any quad touching a -1/GridSize-1 boundary-layer cell
	// belongs to a face/edge/corner seam job.
	const bool bInteriorDomain = (Request.MeshCellDomain == EVoxelMeshCellDomain::Interior);

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

		// Interior domain: skip any quad whose 4 cells are not all interior. (The surrounding
		// cells sit at offsets 0/-1 from the edge, so this bounds-checks each against
		// [0, GridSize-1) — boundary-layer cells were not solved in this domain.)
		if (bInteriorDomain)
		{
			bool bAllCellsInterior = true;
			for (int32 i = 0; i < 4; i++)
			{
				const int32 ACX = CX + Offsets[i].DX;
				const int32 ACY = CY + Offsets[i].DY;
				const int32 ACZ = CZ + Offsets[i].DZ;
				if (ACX < 0 || ACX >= GridSize - 1 ||
					ACY < 0 || ACY >= GridSize - 1 ||
					ACZ < 0 || ACZ >= GridSize - 1)
				{
					bAllCellsInterior = false;
					break;
				}
			}
			if (!bAllCellsInterior)
			{
				continue;
			}
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

	// Per-axis, per-side effective stride and whether that boundary needs welding.
	// EffStride = 1<<max(self, neighbor) = the coarser of the two sides. Both
	// neighbors compute the same value (each knows the other's LOD), so they weld
	// at the same stride. Stride-1 boundaries (the one-plane neighbor data is
	// exactly what the cell needs) are left floating — already watertight (DT1).
	int32 NegEff[3], PosEff[3];
	bool NegWeld[3], PosWeld[3];
	bool bAny = false;
	for (int32 A = 0; A < 3; A++)
	{
		const int32 NLneg = Request.NeighborLODLevels[2 * A];
		const int32 NLpos = Request.NeighborLODLevels[2 * A + 1];
		NegEff[A] = 1 << FMath::Max(SelfLOD, FMath::Max(NLneg, 0));
		PosEff[A] = 1 << FMath::Max(SelfLOD, FMath::Max(NLpos, 0));
		NegWeld[A] = NegEff[A] > 1;
		PosWeld[A] = PosEff[A] > 1;
		bAny = bAny || NegWeld[A] || PosWeld[A];
	}
	if (!bAny)
	{
		return; // pure stride-1 chunk on all faces — boundaries already watertight
	}

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
		bool Pinned[3] = { false, false, false };
		int32 PlaneV[3] = { 0, 0, 0 };
		int32 Eff[3] = { Stride, Stride, Stride };
		for (int32 A = 0; A < 3; A++)
		{
			if (C[A] == -1 && NegWeld[A]) { Pinned[A] = true; PlaneV[A] = 0; Eff[A] = NegEff[A]; }
			else if (C[A] == GridSize - 1 && PosWeld[A]) { Pinned[A] = true; PlaneV[A] = ChunkSize; Eff[A] = PosEff[A]; }
		}
		const int32 NP = (Pinned[0] ? 1 : 0) + (Pinned[1] ? 1 : 0) + (Pinned[2] ? 1 : 0);
		if (NP == 0) { continue; } // interior cell — keep its floating QEF vertex

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
		}

		if (!bHas)
		{
			// No shared-feature crossing — keep the cell's original QEF vertex rather
			// than dropping it: a dropped boundary cell punches a hole (missing
			// geometry, see-through), which is far more visible than the thin vertex
			// mismatch that keeping it may leave. Geometry stays present.
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
// Face-Seam Meshing (seam-ownership P1 — SEAM_OWNERSHIP_ARCHITECTURE.md §2.2)
// ============================================================================

namespace VoxelDCFaceSeam
{
	/**
	 * Voxel sampler over a face-seam request. Three views:
	 *   AOnly    — owner-local coords; Air outside A. Exactly A's Interior-pass view.
	 *   BOnly    — B-local coords; Air outside B. Exactly B's Interior-pass view.
	 *   Combined — owner-local coords; A for axis-coord [0, ChunkSize), B for
	 *              [ChunkSize, 2*ChunkSize) (shifted), Air otherwise (transverse
	 *              out-of-bounds is edge/corner-neighbour territory — P2).
	 * Ring recomputes MUST use the per-side views so their results are bit-identical to the
	 * interior passes (same data, same Air clamp); only slab cells use the combined view.
	 */
	struct FSeamSampler
	{
		const FVoxelFaceSeamRequest& Req;
		enum class EMode : uint8 { AOnly, BOnly, Combined };
		EMode Mode;

		FSeamSampler(const FVoxelFaceSeamRequest& InReq, EMode InMode) : Req(InReq), Mode(InMode) {}

		FVoxelData GetVoxel(int32 X, int32 Y, int32 Z) const
		{
			const int32 CS = Req.ChunkSize;
			if (X >= 0 && X < CS && Y >= 0 && Y < CS && Z >= 0 && Z < CS)
			{
				const TArray<FVoxelData>& Data = (Mode == EMode::BOnly) ? *Req.VoxelDataB : *Req.VoxelDataA;
				return Data[X + Y * CS + Z * CS * CS];
			}
			if (Mode == EMode::Combined)
			{
				int32 C[3] = { X, Y, Z };
				const int32 U = Req.Axis;
				if (C[U] >= CS && C[U] < 2 * CS)
				{
					C[U] -= CS;
					if (C[0] >= 0 && C[0] < CS && C[1] >= 0 && C[1] < CS && C[2] >= 0 && C[2] < CS)
					{
						return (*Req.VoxelDataB)[C[0] + C[1] * CS + C[2] * CS * CS];
					}
				}
			}
			return FVoxelData::Air();
		}

		float GetDensity(int32 X, int32 Y, int32 Z) const
		{
			return static_cast<float>(GetVoxel(X, Y, Z).Density) / 255.0f;
		}

		/**
		 * Replicates CalculateGradientNormal / CalculateGradientNormalLOD exactly (the two are
		 * numerically identical at Stride 1, which is when the interior pass picks the former).
		 */
		FVector3f GradientNormal(float X, float Y, float Z, int32 Stride) const
		{
			const int32 IX = FMath::FloorToInt(X);
			const int32 IY = FMath::FloorToInt(Y);
			const int32 IZ = FMath::FloorToInt(Z);

			float gx = GetDensity(IX + Stride, IY, IZ) - GetDensity(IX - Stride, IY, IZ);
			float gy = GetDensity(IX, IY + Stride, IZ) - GetDensity(IX, IY - Stride, IZ);
			float gz = GetDensity(IX, IY, IZ + Stride) - GetDensity(IX, IY, IZ - Stride);

			FVector3f Normal(-gx, -gy, -gz);
			if (!Normal.Normalize())
			{
				return FVector3f(0.0f, 0.0f, 1.0f);
			}
			return Normal;
		}
	};

	/** Cell vertex for the seam pass (mirrors the mesher's private FDCCellVertex). */
	struct FSeamCellVertex
	{
		FVector3f Position;
		FVector3f Normal;
		uint8 EmittedMaterialID = 0;
		uint8 EmittedBiomeID = 0;
		int32 MeshVertexIndex = -1;
		bool bValid = false;
	};

	/**
	 * MUST mirror SolveCellVertices' CellEdges table — same entries, same ORDER. QEF accumulation
	 * is order-sensitive in float, and the ring recompute must be bit-identical to the interior pass.
	 */
	struct FEdgeRef { int32 DX, DY, DZ, Axis; };
	static const FEdgeRef GCellEdges[12] = {
		{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 2},
		{1, 0, 0, 1}, {1, 0, 0, 2},
		{0, 1, 0, 0}, {0, 1, 0, 2},
		{0, 0, 1, 0}, {0, 0, 1, 1},
		{1, 1, 0, 2}, {1, 0, 1, 1}, {0, 1, 1, 0},
	};

	/** MUST mirror GenerateQuads' AxisOffsets (winding-correct 4-cell order per edge axis). */
	struct FCellOffset { int32 DX, DY, DZ; };
	static const FCellOffset GAxisOffsets[3][4] = {
		{{0, 0, 0}, {0, -1, 0}, {0, -1, -1}, {0, 0, -1}},
		{{0, 0, 0}, {0, 0, -1}, {-1, 0, -1}, {-1, 0, 0}},
		{{0, 0, 0}, {-1, 0, 0}, {-1, -1, 0}, {0, -1, 0}},
	};

	/**
	 * Quadrant-visibility sampler over an edge-seam request (seam-ownership P2a).
	 *
	 * Works in a chosen participant's FRAME (FrameOffsetVoxels maps frame-local coords into the
	 * owner-frame quadrant space) with a 4-bit visibility mask over the (PerpA, PerpB) quadrants
	 * (bit = dv + dw*2, matching FVoxelEdgeSeamRequest::VoxelData order). Along the edge-parallel
	 * axis only [0, ChunkSize) is owned — outside is Air (corner-seam territory, P2b). With the
	 * right mask+frame this reproduces EXACTLY every earlier job's sampler: a participant's
	 * interior view (single quadrant) and a face job's pair view (two quadrants along one axis) —
	 * which is what makes the edge seam's ring recomputes bit-identical to those jobs' outputs.
	 */
	struct FQuadSampler
	{
		const FVoxelEdgeSeamRequest& Req;
		int32 UAxis;                  // edge-parallel axis
		int32 PerpA;                  // smaller perpendicular axis
		int32 PerpB;                  // larger perpendicular axis
		uint8 QuadrantMask;           // visible quadrants, bit = dv + dw*2
		FIntVector FrameOffsetVoxels; // frame-local -> owner-frame voxel offset

		FVoxelData GetVoxel(int32 X, int32 Y, int32 Z) const
		{
			const int32 CS = Req.ChunkSize;
			int32 C[3] = { X + FrameOffsetVoxels.X, Y + FrameOffsetVoxels.Y, Z + FrameOffsetVoxels.Z };
			if (C[UAxis] < 0 || C[UAxis] >= CS)
			{
				return FVoxelData::Air();
			}
			const int32 CA = C[PerpA];
			const int32 CB = C[PerpB];
			if (CA < 0 || CA >= 2 * CS || CB < 0 || CB >= 2 * CS)
			{
				return FVoxelData::Air();
			}
			const int32 Dv = (CA >= CS) ? 1 : 0;
			const int32 Dw = (CB >= CS) ? 1 : 0;
			if (!(QuadrantMask & (1 << (Dv + Dw * 2))))
			{
				return FVoxelData::Air();
			}
			int32 L[3];
			L[UAxis] = C[UAxis];
			L[PerpA] = CA - Dv * CS;
			L[PerpB] = CB - Dw * CS;
			const TArray<FVoxelData>& Data = *Req.VoxelData[Dv + Dw * 2];
			return Data[L[0] + L[1] * CS + L[2] * CS * CS];
		}

		float GetDensity(int32 X, int32 Y, int32 Z) const
		{
			return static_cast<float>(GetVoxel(X, Y, Z).Density) / 255.0f;
		}

		/** Replicates CalculateGradientNormal(/LOD) exactly — see FSeamSampler::GradientNormal. */
		FVector3f GradientNormal(float X, float Y, float Z, int32 Stride) const
		{
			const int32 IX = FMath::FloorToInt(X);
			const int32 IY = FMath::FloorToInt(Y);
			const int32 IZ = FMath::FloorToInt(Z);

			float gx = GetDensity(IX + Stride, IY, IZ) - GetDensity(IX - Stride, IY, IZ);
			float gy = GetDensity(IX, IY + Stride, IZ) - GetDensity(IX, IY - Stride, IZ);
			float gz = GetDensity(IX, IY, IZ + Stride) - GetDensity(IX, IY, IZ - Stride);

			FVector3f Normal(-gx, -gy, -gz);
			if (!Normal.Normalize())
			{
				return FVector3f(0.0f, 0.0f, 1.0f);
			}
			return Normal;
		}
	};

	/**
	 * Octant-visibility sampler over a corner-seam request (seam-ownership P2b).
	 *
	 * The 3D generalization of FQuadSampler: eight participants around the shared corner, an
	 * 8-bit visibility mask (bit = dx + dy*2 + dz*4, matching FVoxelCornerSeamRequest::VoxelData
	 * order), and a frame offset mapping frame-local coords into owner-frame octant space. With
	 * the right mask+frame this reproduces EXACTLY every earlier job's sampler — an interior view
	 * (1 octant), a face job's pair view (2 octants), and an edge job's quad view (4 octants,
	 * whose parallel-axis clamp falls out of the octant masking for free).
	 */
	struct FOctSampler
	{
		const FVoxelCornerSeamRequest& Req;
		uint8 OctantMask;             // visible octants, bit = dx + dy*2 + dz*4
		FIntVector FrameOffsetVoxels; // frame-local -> owner-frame voxel offset

		FVoxelData GetVoxel(int32 X, int32 Y, int32 Z) const
		{
			const int32 CS = Req.ChunkSize;
			const int32 C[3] = { X + FrameOffsetVoxels.X, Y + FrameOffsetVoxels.Y, Z + FrameOffsetVoxels.Z };
			if (C[0] < 0 || C[0] >= 2 * CS || C[1] < 0 || C[1] >= 2 * CS || C[2] < 0 || C[2] >= 2 * CS)
			{
				return FVoxelData::Air();
			}
			const int32 Dx = (C[0] >= CS) ? 1 : 0;
			const int32 Dy = (C[1] >= CS) ? 1 : 0;
			const int32 Dz = (C[2] >= CS) ? 1 : 0;
			const int32 Octant = Dx + Dy * 2 + Dz * 4;
			if (!(OctantMask & (1 << Octant)))
			{
				return FVoxelData::Air();
			}
			const int32 LX = C[0] - Dx * CS;
			const int32 LY = C[1] - Dy * CS;
			const int32 LZ = C[2] - Dz * CS;
			return (*Req.VoxelData[Octant])[LX + LY * CS + LZ * CS * CS];
		}

		float GetDensity(int32 X, int32 Y, int32 Z) const
		{
			return static_cast<float>(GetVoxel(X, Y, Z).Density) / 255.0f;
		}

		/** Replicates CalculateGradientNormal(/LOD) exactly — see FSeamSampler::GradientNormal. */
		FVector3f GradientNormal(float X, float Y, float Z, int32 Stride) const
		{
			const int32 IX = FMath::FloorToInt(X);
			const int32 IY = FMath::FloorToInt(Y);
			const int32 IZ = FMath::FloorToInt(Z);

			float gx = GetDensity(IX + Stride, IY, IZ) - GetDensity(IX - Stride, IY, IZ);
			float gy = GetDensity(IX, IY + Stride, IZ) - GetDensity(IX, IY - Stride, IZ);
			float gz = GetDensity(IX, IY, IZ + Stride) - GetDensity(IX, IY, IZ - Stride);

			FVector3f Normal(-gx, -gy, -gz);
			if (!Normal.Normalize())
			{
				return FVector3f(0.0f, 0.0f, 1.0f);
			}
			return Normal;
		}
	};

	/**
	 * Compute one DC cell vertex against a sampler, replicating DetectEdgeCrossings +
	 * SolveCellVertices op-for-op: per-edge crossing formula (t clamp, P0/P1 construction),
	 * gradient normal at the crossing, QEF.Add in GCellEdges order, the same Solve call, and
	 * the same averaged-normal fallback. With the per-side samplers this makes ring-cell
	 * results bit-identical to the corresponding Interior-domain pass (or, via FQuadSampler's
	 * restricted masks, to the corresponding face-seam job). Templated so every sampler type
	 * shares the single op sequence — the bit-exactness contract lives in ONE place.
	 */
	template <typename TSampler>
	static bool ComputeCellVertex(
		const TSampler& S,
		int32 CX, int32 CY, int32 CZ,
		int32 Stride, float VoxelSize, float IsoLevel, float SVDThreshold, float BiasStrength,
		FSeamCellVertex& Out)
	{
		FQEFSolver QEF;
		FVector3f AvgNormal = FVector3f::ZeroVector;

		for (const FEdgeRef& Edge : GCellEdges)
		{
			const int32 VX = (CX + Edge.DX) * Stride;
			const int32 VY = (CY + Edge.DY) * Stride;
			const int32 VZ = (CZ + Edge.DZ) * Stride;

			const float D0 = S.GetDensity(VX, VY, VZ);

			int32 NX = VX, NY = VY, NZ = VZ;
			if (Edge.Axis == 0) NX += Stride;
			else if (Edge.Axis == 1) NY += Stride;
			else NZ += Stride;

			const float D1 = S.GetDensity(NX, NY, NZ);

			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				continue;
			}

			float t = (IsoLevel - D0) / (D1 - D0);
			t = FMath::Clamp(t, 0.0f, 1.0f);

			const FVector3f P0(static_cast<float>(VX) * VoxelSize,
				static_cast<float>(VY) * VoxelSize,
				static_cast<float>(VZ) * VoxelSize);
			const FVector3f P1(static_cast<float>(NX) * VoxelSize,
				static_cast<float>(NY) * VoxelSize,
				static_cast<float>(NZ) * VoxelSize);

			const FVector3f CrossPos = P0 + (P1 - P0) * t;
			const FVector3f CrossNormal = S.GradientNormal(
				CrossPos.X / VoxelSize, CrossPos.Y / VoxelSize, CrossPos.Z / VoxelSize, Stride);

			QEF.Add(CrossPos, CrossNormal);
			AvgNormal += CrossNormal;
		}

		if (QEF.Count == 0)
		{
			Out.bValid = false;
			return false;
		}

		const float CellWorldSize = static_cast<float>(Stride) * VoxelSize;
		const float MinX = static_cast<float>(CX * Stride) * VoxelSize;
		const float MinY = static_cast<float>(CY * Stride) * VoxelSize;
		const float MinZ = static_cast<float>(CZ * Stride) * VoxelSize;
		const FBox3f CellBounds(
			FVector3f(MinX, MinY, MinZ),
			FVector3f(MinX + CellWorldSize, MinY + CellWorldSize, MinZ + CellWorldSize)
		);

		Out.bValid = true;
		Out.MeshVertexIndex = -1;
		Out.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength);

		if (!AvgNormal.Normalize())
		{
			AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
		}
		Out.Normal = AvgNormal;
		return true;
	}

	/**
	 * Mixed-LOD face seam (seam-ownership P2c — the §2.3 cross-resolution mesher).
	 *
	 * The slab is ALWAYS the owner's last cell layer at the OWNER's stride (the interior domain
	 * [0, Grid-1) covers the neighbour's first layer but not the owner's last — regardless of
	 * which side is finer). Rings recompute at each side's OWN stride with that side's sampler,
	 * bit-identical to its Interior pass. The face plane stitches at the FINER granularity:
	 * every fine crossing gets a fan onto the coarser side's ring vertices, quads collapsing to
	 * triangles when their two coarse-side slots fall in the same coarse cell (the T-junction).
	 * All positions are surface-derived QEF output clamped to their own cells (#44 laws hold by
	 * construction); a fine crossing facing an invalid coarse cell is skipped — degrading to a
	 * crack, never a protrusion.
	 */
	static bool GenerateMixedFaceSeam(
		const FVoxelFaceSeamRequest& Req,
		const FVoxelMeshingConfig& Config,
		FChunkMeshData& Out)
	{
		const int32 ChunkSize = Req.ChunkSize;
		const int32 LODA = FMath::Clamp(Req.LODLevel, 0, 7);
		const int32 LODB = FMath::Clamp(Req.GetLODLevelB(), 0, 7);
		const int32 SA = 1 << LODA;
		const int32 SB = 1 << LODB;
		if ((ChunkSize % SA) != 0 || (ChunkSize % SB) != 0)
		{
			UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateMixedFaceSeam: ChunkSize %d not divisible by strides %d/%d"), ChunkSize, SA, SB);
			return false;
		}
		const int32 GridA = ChunkSize / SA;
		const int32 GridB = ChunkSize / SB;
		if (GridA < 3 || GridB < 3)
		{
			return true; // degenerate resolution — valid empty seam
		}
		const int32 SLA = GridA - 1;
		const int32 SLB = GridB - 1;
		const int32 MinS = FMath::Min(SA, SB);
		const int32 RA = SA / MinS; // fine steps per owner-side cell (1 when the owner is finer)
		const int32 RB = SB / MinS; // fine steps per neighbour-side cell
		const int32 FaceGrid = ChunkSize / MinS;

		const int32 U = Req.Axis;
		const int32 V = (U + 1) % 3;
		const int32 W = (U + 2) % 3;
		const float VoxelSize = Req.VoxelSize;
		const float IsoLevel = Config.IsoLevel;
		const float SVDThreshold = Config.QEFSVDThreshold;
		const float BiasStrength = Config.QEFBiasStrength;
		const float FrameOffsetU = static_cast<float>(ChunkSize) * VoxelSize;

		const FSeamSampler SamplerA(Req, FSeamSampler::EMode::AOnly);
		const FSeamSampler SamplerB(Req, FSeamSampler::EMode::BOnly);
		const FSeamSampler SamplerC(Req, FSeamSampler::EMode::Combined);

		// ---- Cell layers: rings at their side's stride, slab at the owner's ----------------
		TArray<FSeamCellVertex> RingA; RingA.SetNum(SLA * SLA);
		TArray<FSeamCellVertex> Slab;  Slab.SetNum(SLA * SLA);
		TArray<FSeamCellVertex> RingB; RingB.SetNum(SLB * SLB);

		for (int32 w = 0; w < SLA; ++w)
		{
			for (int32 v = 0; v < SLA; ++v)
			{
				const int32 Idx = v + w * SLA;
				int32 C[3];
				C[V] = v;
				C[W] = w;
				C[U] = SLA - 1;
				ComputeCellVertex(SamplerA, C[0], C[1], C[2], SA, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, RingA[Idx]);
				C[U] = SLA;
				ComputeCellVertex(SamplerC, C[0], C[1], C[2], SA, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Slab[Idx]);
			}
		}
		for (int32 w = 0; w < SLB; ++w)
		{
			for (int32 v = 0; v < SLB; ++v)
			{
				const int32 Idx = v + w * SLB;
				int32 C[3];
				C[V] = v;
				C[W] = w;
				C[U] = 0;
				if (ComputeCellVertex(SamplerB, C[0], C[1], C[2], SB, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, RingB[Idx]))
				{
					RingB[Idx].Position[U] += FrameOffsetU;
				}
			}
		}

		// ---- Vertex emission (identical projection/material carry to the other seam paths) --
		const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
		auto EmitSeamVertex = [&Out, VoxelSize, UVScale](const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID) -> int32
		{
			const int32 Index = Out.Positions.Num();
			Out.Positions.Add(Vertex.Position);
			Out.Normals.Add(Vertex.Normal);

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
			Out.UVs.Add(UV);
			Out.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
			Out.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
			return Index;
		};

		constexpr int32 KeyStride = 1 << 20; // layer id * KeyStride + cell index (unique across layers)
		TMap<uint64, int32> DuplicateVertexCache;
		uint32 TriangleCount = 0;

		// Fan emitter over up to 4 cyclic cell refs: adjacent duplicates (the T-junction's
		// coincident coarse slots) collapse the quad to a triangle. Quad case keeps the
		// adaptive max-min-area diagonal.
		struct FRef { FSeamCellVertex* Cell = nullptr; int32 Key = 0; };
		auto EmitFan = [&](FRef (&Refs)[4], bool bFlip, uint8 QuadMaterial, uint8 QuadBiome)
		{
			FRef Uniq[4];
			int32 N = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				if (N > 0 && Refs[i].Cell == Uniq[N - 1].Cell)
				{
					continue;
				}
				Uniq[N++] = Refs[i];
			}
			if (N > 1 && Uniq[N - 1].Cell == Uniq[0].Cell)
			{
				--N;
			}
			if (N < 3)
			{
				return; // fully degenerate (both sides coarse-coincident) — nothing to emit
			}

			int32 Indices[4];
			for (int32 i = 0; i < N; ++i)
			{
				FSeamCellVertex& Cell = *Uniq[i].Cell;
				if (Cell.MeshVertexIndex >= 0 && Cell.EmittedMaterialID == QuadMaterial && Cell.EmittedBiomeID == QuadBiome)
				{
					Indices[i] = Cell.MeshVertexIndex;
				}
				else if (Cell.MeshVertexIndex < 0)
				{
					Indices[i] = EmitSeamVertex(Cell, QuadMaterial, QuadBiome);
					Cell.MeshVertexIndex = Indices[i];
					Cell.EmittedMaterialID = QuadMaterial;
					Cell.EmittedBiomeID = QuadBiome;
				}
				else
				{
					const uint64 DupKey = (static_cast<uint64>(Uniq[i].Key) << 16)
						| (static_cast<uint64>(QuadMaterial) << 8)
						| static_cast<uint64>(QuadBiome);
					if (const int32* Found = DuplicateVertexCache.Find(DupKey))
					{
						Indices[i] = *Found;
					}
					else
					{
						Indices[i] = EmitSeamVertex(Cell, QuadMaterial, QuadBiome);
						DuplicateVertexCache.Add(DupKey, Indices[i]);
					}
				}
			}

			auto AddTri = [&](int32 a, int32 b, int32 c)
			{
				Out.Indices.Add(Indices[a]);
				Out.Indices.Add(Indices[b]);
				Out.Indices.Add(Indices[c]);
			};

			if (N == 3)
			{
				if (bFlip) { AddTri(0, 1, 2); }
				else       { AddTri(0, 2, 1); }
				TriangleCount += 1;
				return;
			}

			auto TriArea = [](const FVector3f& A, const FVector3f& B, const FVector3f& C) -> float
			{
				return 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
			};
			const FVector3f& P0 = Uniq[0].Cell->Position;
			const FVector3f& P1 = Uniq[1].Cell->Position;
			const FVector3f& P2 = Uniq[2].Cell->Position;
			const FVector3f& P3 = Uniq[3].Cell->Position;
			const float MinArea02 = FMath::Min(TriArea(P0, P1, P2), TriArea(P0, P2, P3));
			const float MinArea13 = FMath::Min(TriArea(P1, P2, P3), TriArea(P1, P3, P0));
			const bool bUse13 = MinArea13 > MinArea02;

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
			TriangleCount += 2;
		};

		// ---- Owner-side groups at stride SA (identical structure to the same-LOD path) ------
		auto EmitAQuad = [&](const int32 EdgeCellA[3], int32 EdgeAbsAxis)
		{
			const int32 VX = EdgeCellA[0] * SA;
			const int32 VY = EdgeCellA[1] * SA;
			const int32 VZ = EdgeCellA[2] * SA;

			const float D0 = SamplerC.GetDensity(VX, VY, VZ);
			int32 NX = VX, NY = VY, NZ = VZ;
			if (EdgeAbsAxis == 0) NX += SA;
			else if (EdgeAbsAxis == 1) NY += SA;
			else NZ += SA;
			const float D1 = SamplerC.GetDensity(NX, NY, NZ);

			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				return;
			}

			FRef Refs[4];
			for (int32 i = 0; i < 4; ++i)
			{
				const FCellOffset& Off = GAxisOffsets[EdgeAbsAxis][i];
				const int32 CellC[3] = { EdgeCellA[0] + Off.DX, EdgeCellA[1] + Off.DY, EdgeCellA[2] + Off.DZ };
				const int32 CU = CellC[U];
				const int32 TV = CellC[V];
				const int32 TW = CellC[W];
				if (TV < 0 || TV >= SLA || TW < 0 || TW >= SLA)
				{
					return;
				}
				TArray<FSeamCellVertex>* Layer = (CU == SLA) ? &Slab : (CU == SLA - 1) ? &RingA : nullptr;
				if (!Layer)
				{
					return;
				}
				FSeamCellVertex& Cell = (*Layer)[TV + TW * SLA];
				if (!Cell.bValid)
				{
					return;
				}
				Refs[i] = { &Cell, ((CU == SLA) ? KeyStride : 0) + TV + TW * SLA };
			}

			const bool bFlip = (D0 < IsoLevel);
			int32 SolidX = VX, SolidY = VY, SolidZ = VZ;
			if (bFlip)
			{
				if (EdgeAbsAxis == 0) { SolidX += SA; }
				else if (EdgeAbsAxis == 1) { SolidY += SA; }
				else { SolidZ += SA; }
			}
			const FVoxelData SolidVoxel = SamplerC.GetVoxel(SolidX, SolidY, SolidZ);
			EmitFan(Refs, bFlip, SolidVoxel.MaterialID, SolidVoxel.BiomeID);
		};

		int32 EdgeCell[3];
		// G1: face-crossing U-edges (4 slab cells).
		for (int32 w = 1; w <= SLA - 1; ++w)
		{
			for (int32 v = 1; v <= SLA - 1; ++v)
			{
				EdgeCell[U] = SLA;
				EdgeCell[V] = v;
				EdgeCell[W] = w;
				EmitAQuad(EdgeCell, U);
			}
		}
		// G2/G3: slab <-> ring-A stitching at the owner's stride.
		for (int32 Sel = 1; Sel <= SLA - 1; ++Sel)
		{
			for (int32 Run = 0; Run <= SLA - 1; ++Run)
			{
				EdgeCell[U] = SLA;
				EdgeCell[V] = Run;
				EdgeCell[W] = Sel;
				EmitAQuad(EdgeCell, V);

				EdgeCell[V] = Sel;
				EdgeCell[W] = Run;
				EmitAQuad(EdgeCell, W);
			}
		}

		// ---- Face-plane stitching at the FINE granularity (the T-junction fans) -------------
		auto EmitFaceEdge = [&](int32 FV, int32 FW, int32 EdgeAbsAxis)
		{
			int32 BaseVox[3];
			BaseVox[U] = ChunkSize;
			BaseVox[V] = FV * MinS;
			BaseVox[W] = FW * MinS;
			int32 EndVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			EndVox[EdgeAbsAxis] += MinS;

			const float D0 = SamplerC.GetDensity(BaseVox[0], BaseVox[1], BaseVox[2]);
			const float D1 = SamplerC.GetDensity(EndVox[0], EndVox[1], EndVox[2]);
			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				return;
			}

			FRef Refs[4];
			for (int32 i = 0; i < 4; ++i)
			{
				const FCellOffset& Off = GAxisOffsets[EdgeAbsAxis][i];
				const int32 OffXYZ[3] = { Off.DX, Off.DY, Off.DZ };
				const int32 OffU = OffXYZ[U];
				const int32 FineV = FV + OffXYZ[V];
				const int32 FineW = FW + OffXYZ[W];
				if (FineV < 0 || FineW < 0)
				{
					return;
				}
				if (OffU == 0)
				{
					// Neighbour-side slot: that side's ring at ITS stride.
					const int32 BV = FineV / RB;
					const int32 BW = FineW / RB;
					if (BV >= SLB || BW >= SLB)
					{
						return;
					}
					FSeamCellVertex& Cell = RingB[BV + BW * SLB];
					if (!Cell.bValid)
					{
						return; // fine crossing with no coarse vertex to fan onto -> crack, never a protrusion
					}
					Refs[i] = { &Cell, 2 * KeyStride + BV + BW * SLB };
				}
				else
				{
					// Owner-side slot: the slab at the owner's stride.
					const int32 AV = FineV / RA;
					const int32 AW = FineW / RA;
					if (AV >= SLA || AW >= SLA)
					{
						return;
					}
					FSeamCellVertex& Cell = Slab[AV + AW * SLA];
					if (!Cell.bValid)
					{
						return;
					}
					Refs[i] = { &Cell, KeyStride + AV + AW * SLA };
				}
			}

			const bool bFlip = (D0 < IsoLevel);
			int32 SolidVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			if (bFlip)
			{
				SolidVox[EdgeAbsAxis] += MinS;
			}
			const FVoxelData SolidVoxel = SamplerC.GetVoxel(SolidVox[0], SolidVox[1], SolidVox[2]);
			EmitFan(Refs, bFlip, SolidVoxel.MaterialID, SolidVoxel.BiomeID);
		};

		for (int32 FW = 0; FW < FaceGrid; ++FW)
		{
			for (int32 FV = 0; FV < FaceGrid; ++FV)
			{
				EmitFaceEdge(FV, FW, V);
				EmitFaceEdge(FV, FW, W);
			}
		}

		UE_LOG(LogVoxelMeshing, Verbose,
			TEXT("DC mixed face seam %s axis=%d LOD %d|%d: %d verts, %d tris"),
			*Req.OwnerChunkCoord.ToString(), U, LODA, LODB,
			Out.Positions.Num(), TriangleCount);

		return true;
	}

	// =========================================================================================
	// Shared emission helpers for the mixed-LOD edge/corner meshers (seam-ownership P2d).
	// =========================================================================================

	/** A cell reference for fan emission: the solved cell + a job-unique dedup key. */
	struct FSeamRef
	{
		FSeamCellVertex* Cell = nullptr;
		int32 Key = 0;
	};

	/**
	 * Vertex/fan emitter shared by the P2d meshers: per-cell/per-material vertex dedup, cyclic
	 * duplicate collapse (quad -> triangle at T-junctions), and the adaptive max-min-area
	 * diagonal for full quads — the exact emission semantics of the earlier seam paths.
	 */
	struct FSeamEmitter
	{
		FChunkMeshData& Out;
		float VoxelSize = 100.0f;
		float UVScale = 0.0f;
		TMap<uint64, int32> DuplicateVertexCache;
		uint32 TriangleCount = 0;

		FSeamEmitter(FChunkMeshData& InOut, float InVoxelSize, float InUVScale)
			: Out(InOut), VoxelSize(InVoxelSize), UVScale(InUVScale)
		{
		}

		int32 EmitVertex(const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID)
		{
			const int32 Index = Out.Positions.Num();
			Out.Positions.Add(Vertex.Position);
			Out.Normals.Add(Vertex.Normal);

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
			Out.UVs.Add(UV);
			Out.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
			Out.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
			return Index;
		}

		void EmitFan(FSeamRef (&Refs)[4], bool bFlip, uint8 QuadMaterial, uint8 QuadBiome)
		{
			FSeamRef Uniq[4];
			int32 N = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				if (N > 0 && Refs[i].Cell == Uniq[N - 1].Cell)
				{
					continue;
				}
				Uniq[N++] = Refs[i];
			}
			if (N > 1 && Uniq[N - 1].Cell == Uniq[0].Cell)
			{
				--N;
			}
			if (N < 3)
			{
				return;
			}

			int32 Indices[4];
			for (int32 i = 0; i < N; ++i)
			{
				FSeamCellVertex& Cell = *Uniq[i].Cell;
				if (Cell.MeshVertexIndex >= 0 && Cell.EmittedMaterialID == QuadMaterial && Cell.EmittedBiomeID == QuadBiome)
				{
					Indices[i] = Cell.MeshVertexIndex;
				}
				else if (Cell.MeshVertexIndex < 0)
				{
					Indices[i] = EmitVertex(Cell, QuadMaterial, QuadBiome);
					Cell.MeshVertexIndex = Indices[i];
					Cell.EmittedMaterialID = QuadMaterial;
					Cell.EmittedBiomeID = QuadBiome;
				}
				else
				{
					const uint64 DupKey = (static_cast<uint64>(Uniq[i].Key) << 16)
						| (static_cast<uint64>(QuadMaterial) << 8)
						| static_cast<uint64>(QuadBiome);
					if (const int32* Found = DuplicateVertexCache.Find(DupKey))
					{
						Indices[i] = *Found;
					}
					else
					{
						Indices[i] = EmitVertex(Cell, QuadMaterial, QuadBiome);
						DuplicateVertexCache.Add(DupKey, Indices[i]);
					}
				}
			}

			auto AddTri = [&](int32 a, int32 b, int32 c)
			{
				Out.Indices.Add(Indices[a]);
				Out.Indices.Add(Indices[b]);
				Out.Indices.Add(Indices[c]);
			};

			if (N == 3)
			{
				if (bFlip) { AddTri(0, 1, 2); }
				else       { AddTri(0, 2, 1); }
				TriangleCount += 1;
				return;
			}

			auto TriArea = [](const FVector3f& A, const FVector3f& B, const FVector3f& C) -> float
			{
				return 0.5f * FVector3f::CrossProduct(B - A, C - A).Size();
			};
			const FVector3f& P0 = Uniq[0].Cell->Position;
			const FVector3f& P1 = Uniq[1].Cell->Position;
			const FVector3f& P2 = Uniq[2].Cell->Position;
			const FVector3f& P3 = Uniq[3].Cell->Position;
			const float MinArea02 = FMath::Min(TriArea(P0, P1, P2), TriArea(P0, P2, P3));
			const float MinArea13 = FMath::Min(TriArea(P1, P2, P3), TriArea(P1, P3, P0));
			const bool bUse13 = MinArea13 > MinArea02;

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
			TriangleCount += 2;
		}
	};

	/**
	 * Mixed-LOD edge seam (seam-ownership P2d). Participants may render at up to four different
	 * LODs. Each of the 9 columns computes at its OWNING job's stride (column owner = the low
	 * quadrant per axis — the P2c ownership rule per column), reproducing that job's cells
	 * bit-exactly. Slot classification is by rectangle containment (mixed strides don't tile as
	 * a product grid), and ownership follows the participant-union rule: a fine edge belongs to
	 * this job iff its slots span >= 3 participants (pairs are the face jobs' territory,
	 * singles the interiors'). Fine edges whose slots exit the 9 columns along U are the
	 * corner jobs' territory.
	 */
	static bool GenerateMixedEdgeSeam(
		const FVoxelEdgeSeamRequest& Req,
		const FVoxelMeshingConfig& Config,
		FChunkMeshData& Out)
	{
		const int32 ChunkSize = Req.ChunkSize;
		int32 S[4];
		int32 MinS = TNumericLimits<int32>::Max();
		int32 MaxS = 1;
		for (int32 q = 0; q < 4; ++q)
		{
			const int32 L = FMath::Clamp(Req.GetLODLevelOf(q), 0, 7);
			S[q] = 1 << L;
			if ((ChunkSize % S[q]) != 0)
			{
				UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateMixedEdgeSeam: ChunkSize %d not divisible by stride %d"), ChunkSize, S[q]);
				return false;
			}
			if (ChunkSize / S[q] < 3)
			{
				return true; // degenerate resolution — valid empty seam
			}
			MinS = FMath::Min(MinS, S[q]);
			MaxS = FMath::Max(MaxS, S[q]);
		}

		const int32 U = Req.EdgeAxis;
		const int32 PerpA = (U == 0) ? 1 : 0;
		const int32 PerpB = (U == 2) ? 1 : 2;
		const float VoxelSize = Req.VoxelSize;
		const float IsoLevel = Config.IsoLevel;
		const float SVDThreshold = Config.QEFSVDThreshold;
		const float BiasStrength = Config.QEFBiasStrength;
		const float ChunkWorldSpan = static_cast<float>(ChunkSize) * VoxelSize;

		// ---- The 9 regions, each at its owning job's stride --------------------------------
		// Inward (-1) states span a BAND of cells (width ~2*MaxS), not a single cell: at mixed
		// strides the smaller jobs skip T-junction fans across a band as wide as the coarser
		// stride, and those fans reference cells several fine-cells deep. The extra cells are
		// ordinary cells of the owning job (same mask/frame/stride) — recomputed bit-exactly;
		// unreferenced ones are harmless.
		const int32 Band = 2 * MaxS;
		struct FColumn
		{
			int32 StrideC = 1;
			int32 VLo = 0, WLo = 0;                   // owner-frame voxel rect min (transverse)
			int32 VCnt = 1, WCnt = 1;                 // cells per transverse axis
			int32 UValidVox = 0;                      // owned U extent: [0, UValidVox) voxels
			int32 UCells = 0;
			uint8 ParticipantMask = 0;                // quadrant bits (dv + dw*2)
			TArray<FSeamCellVertex> Cells;            // indexed (iv + iw*VCnt)*UCells + u
		};
		FColumn Columns[9];
		auto ColumnIndex = [](int32 Dcv, int32 Dcw) { return (Dcv + 1) + (Dcw + 1) * 3; };

		for (int32 Dcw = -1; Dcw <= 1; ++Dcw)
		{
			for (int32 Dcv = -1; Dcv <= 1; ++Dcv)
			{
				FColumn& Col = Columns[ColumnIndex(Dcv, Dcw)];

				const int32 OwnerDv = (Dcv == 1) ? 1 : 0;
				const int32 OwnerDw = (Dcw == 1) ? 1 : 0;
				Col.StrideC = S[OwnerDv + OwnerDw * 2];
				const int32 GridC = ChunkSize / Col.StrideC;
				const int32 SLc = GridC - 1;

				uint8 Mask = 0;
				for (int32 dw = 0; dw <= 1; ++dw)
				{
					for (int32 dv = 0; dv <= 1; ++dv)
					{
						const bool bDvOK = (Dcv == 0) || ((Dcv == -1) == (dv == 0));
						const bool bDwOK = (Dcw == 0) || ((Dcw == -1) == (dw == 0));
						if (bDvOK && bDwOK)
						{
							Mask |= (1 << (dv + dw * 2));
						}
					}
				}
				Col.ParticipantMask = Mask;

				auto RectRange = [ChunkSize, Band](int32 Dc, int32 Sc, int32 SLcIn, int32& Lo, int32& Cnt)
				{
					if (Dc == -1)
					{
						Cnt = FMath::Clamp(Band / Sc, 1, SLcIn - 1);
						Lo = ChunkSize - Sc - Cnt * Sc;
					}
					else if (Dc == 0)
					{
						Cnt = 1;
						Lo = ChunkSize - Sc;
					}
					else
					{
						Cnt = 1;
						Lo = ChunkSize;
					}
				};
				RectRange(Dcv, Col.StrideC, SLc, Col.VLo, Col.VCnt);
				RectRange(Dcw, Col.StrideC, SLc, Col.WLo, Col.WCnt);
				Col.UCells = SLc;
				Col.UValidVox = SLc * Col.StrideC; // cells u in [0, SLc)

				FIntVector FrameOffset = FIntVector::ZeroValue;
				FrameOffset[PerpA] = OwnerDv * ChunkSize;
				FrameOffset[PerpB] = OwnerDw * ChunkSize;
				const int32 CellABase = (Col.VLo - FrameOffset[PerpA]) / Col.StrideC;
				const int32 CellBBase = (Col.WLo - FrameOffset[PerpB]) / Col.StrideC;

				Col.Cells.SetNum(Col.VCnt * Col.WCnt * Col.UCells);
				const FQuadSampler Sampler{ Req, U, PerpA, PerpB, Mask, FrameOffset };
				for (int32 iw = 0; iw < Col.WCnt; ++iw)
				{
					for (int32 iv = 0; iv < Col.VCnt; ++iv)
					{
						for (int32 u = 0; u < Col.UCells; ++u)
						{
							int32 C[3];
							C[U] = u;
							C[PerpA] = CellABase + iv;
							C[PerpB] = CellBBase + iw;
							FSeamCellVertex& Cell = Col.Cells[(iv + iw * Col.VCnt) * Col.UCells + u];
							if (ComputeCellVertex(Sampler, C[0], C[1], C[2], Col.StrideC, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
							{
								Cell.Position[PerpA] += OwnerDv * ChunkWorldSpan;
								Cell.Position[PerpB] += OwnerDw * ChunkWorldSpan;
							}
						}
					}
				}
			}
		}

		FSeamEmitter Emitter(Out, VoxelSize, Config.bGenerateUVs ? Config.UVScale : 0.0f);
		const FQuadSampler FullSampler{ Req, U, PerpA, PerpB, 0b1111, FIntVector::ZeroValue };
		constexpr int32 ColKeyStride = 1 << 16;

		// Classify one slot cell (given by its min-corner voxel) into the 9 regions.
		auto ClassifySlot = [&](int32 UVox, int32 VVox, int32 WVox, FSeamRef& OutRef, uint8& UnionMask) -> bool
		{
			for (int32 ci = 0; ci < 9; ++ci)
			{
				FColumn& Col = Columns[ci];
				const int32 VHi = Col.VLo + Col.VCnt * Col.StrideC;
				const int32 WHi = Col.WLo + Col.WCnt * Col.StrideC;
				if (VVox >= Col.VLo && VVox < VHi && WVox >= Col.WLo && WVox < WHi)
				{
					if (UVox < 0 || UVox >= Col.UValidVox)
					{
						return false; // the region's u-rim — corner-seam territory
					}
					const int32 IV = (VVox - Col.VLo) / Col.StrideC;
					const int32 IW = (WVox - Col.WLo) / Col.StrideC;
					const int32 CellIdx = (IV + IW * Col.VCnt) * Col.UCells + UVox / Col.StrideC;
					FSeamCellVertex& Cell = Col.Cells[CellIdx];
					if (!Cell.bValid)
					{
						return false;
					}
					OutRef = FSeamRef{ &Cell, ci * ColKeyStride + CellIdx };
					UnionMask |= Col.ParticipantMask;
					return true;
				}
			}
			return false; // outside the edge region — another job's territory
		};

		auto EmitMixedEdge = [&](int32 FU, int32 FV, int32 FW, int32 EdgeAbsAxis)
		{
			int32 BaseVox[3];
			BaseVox[U] = FU * MinS;
			BaseVox[PerpA] = FV * MinS;
			BaseVox[PerpB] = FW * MinS;
			int32 EndVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			EndVox[EdgeAbsAxis] += MinS;

			const float D0 = FullSampler.GetDensity(BaseVox[0], BaseVox[1], BaseVox[2]);
			const float D1 = FullSampler.GetDensity(EndVox[0], EndVox[1], EndVox[2]);
			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				return;
			}

			FSeamRef Refs[4];
			uint8 UnionMask = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				const FCellOffset& Off = GAxisOffsets[EdgeAbsAxis][i];
				const int32 OffXYZ[3] = { Off.DX, Off.DY, Off.DZ };
				const int32 SlotU = BaseVox[U] + OffXYZ[U] * MinS;
				const int32 SlotV = BaseVox[PerpA] + OffXYZ[PerpA] * MinS;
				const int32 SlotW = BaseVox[PerpB] + OffXYZ[PerpB] * MinS;
				if (!ClassifySlot(SlotU, SlotV, SlotW, Refs[i], UnionMask))
				{
					return;
				}
			}
			if (FMath::CountBits(UnionMask) < 3)
			{
				return; // computable by a single face pair (or interior) — that job owns it
			}

			const bool bFlip = (D0 < IsoLevel);
			int32 SolidVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			if (bFlip)
			{
				SolidVox[EdgeAbsAxis] += MinS;
			}
			const FVoxelData SolidVoxel = FullSampler.GetVoxel(SolidVox[0], SolidVox[1], SolidVox[2]);
			Emitter.EmitFan(Refs, bFlip, SolidVoxel.MaterialID, SolidVoxel.BiomeID);
		};

		// Fine enumeration over the edge neighbourhood: U along the whole column, transverse a
		// band of +-2 coarse cells around the edge line (classification rejects the excess).
		const int32 FineU = ChunkSize / MinS;
		const int32 TLo = (ChunkSize - 2 * MaxS) / MinS;
		const int32 THi = (ChunkSize + 2 * MaxS) / MinS;
		for (int32 FW = TLo; FW < THi; ++FW)
		{
			for (int32 FV = TLo; FV < THi; ++FV)
			{
				for (int32 FU = 0; FU < FineU; ++FU)
				{
					EmitMixedEdge(FU, FV, FW, U);
					EmitMixedEdge(FU, FV, FW, PerpA);
					EmitMixedEdge(FU, FV, FW, PerpB);
				}
			}
		}

		UE_LOG(LogVoxelMeshing, Verbose,
			TEXT("DC mixed edge seam %s axis=%d: %d verts, %d tris"),
			*Req.OwnerChunkCoord.ToString(), U, Out.Positions.Num(), Emitter.TriangleCount);

		return true;
	}

	/**
	 * Mixed-LOD corner seam (seam-ownership P2d). The 27 corner-block cells each compute at
	 * their OWNING job's stride (owner = the low octant per axis), reproducing every
	 * neighbouring job — interiors, same/mixed face jobs, same/mixed edge jobs — bit-exactly.
	 * Ownership: a fine edge belongs to the corner job iff its slot participant-union is not
	 * contained in any single axis half (halves are exactly the edge tuples' octant sets, and
	 * every face pair / interior is inside some half).
	 */
	static bool GenerateMixedCornerSeam(
		const FVoxelCornerSeamRequest& Req,
		const FVoxelMeshingConfig& Config,
		FChunkMeshData& Out)
	{
		const int32 ChunkSize = Req.ChunkSize;
		int32 S[8];
		int32 MinS = TNumericLimits<int32>::Max();
		int32 MaxS = 1;
		for (int32 o = 0; o < 8; ++o)
		{
			const int32 L = FMath::Clamp(Req.GetLODLevelOf(o), 0, 7);
			S[o] = 1 << L;
			if ((ChunkSize % S[o]) != 0)
			{
				UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateMixedCornerSeam: ChunkSize %d not divisible by stride %d"), ChunkSize, S[o]);
				return false;
			}
			if (ChunkSize / S[o] < 3)
			{
				return true;
			}
			MinS = FMath::Min(MinS, S[o]);
			MaxS = FMath::Max(MaxS, S[o]);
		}

		const float VoxelSize = Req.VoxelSize;
		const float IsoLevel = Config.IsoLevel;
		const float SVDThreshold = Config.QEFSVDThreshold;
		const float BiasStrength = Config.QEFBiasStrength;
		const float ChunkWorldSpan = static_cast<float>(ChunkSize) * VoxelSize;

		// ---- The 27 corner-block regions, each at its owning job's stride -------------------
		// Inward (-1) states span a BAND of cells (~2*MaxS wide) per axis, mirroring the mixed
		// edge mesher: T-junction fans skipped by the smaller jobs reference cells several
		// fine-cells deep at mixed strides.
		const int32 Band = 2 * MaxS;
		struct FCornerRegion
		{
			int32 StrideC = 1;
			int32 Lo[3] = { 0, 0, 0 };
			int32 Cnt[3] = { 1, 1, 1 };
			uint8 ParticipantMask = 0; // octant bits
			TArray<FSeamCellVertex> Cells; // indexed (ix + iy*Cnt0)*... x-innermost
		};
		FCornerRegion Regions[27];
		auto RegionIndexOf = [](int32 SX, int32 SY, int32 SZ) { return (SX + 1) + (SY + 1) * 3 + (SZ + 1) * 9; };

		for (int32 SZ = -1; SZ <= 1; ++SZ)
		{
			for (int32 SY = -1; SY <= 1; ++SY)
			{
				for (int32 SX = -1; SX <= 1; ++SX)
				{
					const int32 St[3] = { SX, SY, SZ };
					FCornerRegion& Reg = Regions[RegionIndexOf(SX, SY, SZ)];

					const int32 OwnerOct = ((SX == 1) ? 1 : 0) + ((SY == 1) ? 2 : 0) + ((SZ == 1) ? 4 : 0);
					const int32 Sc = S[OwnerOct];
					const int32 GridC = ChunkSize / Sc;
					const int32 SLc = GridC - 1;
					Reg.StrideC = Sc;

					uint8 Mask = 0;
					for (int32 Oct = 0; Oct < 8; ++Oct)
					{
						const int32 D[3] = { Oct & 1, (Oct >> 1) & 1, (Oct >> 2) & 1 };
						bool bAllowed = true;
						for (int32 Axis = 0; Axis < 3; ++Axis)
						{
							if ((St[Axis] == -1 && D[Axis] != 0) || (St[Axis] == 1 && D[Axis] != 1))
							{
								bAllowed = false;
								break;
							}
						}
						if (bAllowed)
						{
							Mask |= (1 << Oct);
						}
					}
					Reg.ParticipantMask = Mask;

					FIntVector FrameOffset(
						(SX == 1) ? ChunkSize : 0,
						(SY == 1) ? ChunkSize : 0,
						(SZ == 1) ? ChunkSize : 0);
					int32 CellBase[3];
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						if (St[Axis] == -1)
						{
							Reg.Cnt[Axis] = FMath::Clamp(Band / Sc, 1, SLc - 1);
							Reg.Lo[Axis] = ChunkSize - Sc - Reg.Cnt[Axis] * Sc;
						}
						else if (St[Axis] == 0)
						{
							Reg.Cnt[Axis] = 1;
							Reg.Lo[Axis] = ChunkSize - Sc;
						}
						else
						{
							Reg.Cnt[Axis] = 1;
							Reg.Lo[Axis] = ChunkSize;
						}
						CellBase[Axis] = (Reg.Lo[Axis] - FrameOffset[Axis]) / Sc;
					}

					Reg.Cells.SetNum(Reg.Cnt[0] * Reg.Cnt[1] * Reg.Cnt[2]);
					const FOctSampler Sampler{ Req, Mask, FrameOffset };
					for (int32 iz = 0; iz < Reg.Cnt[2]; ++iz)
					{
						for (int32 iy = 0; iy < Reg.Cnt[1]; ++iy)
						{
							for (int32 ix = 0; ix < Reg.Cnt[0]; ++ix)
							{
								FSeamCellVertex& Cell = Reg.Cells[(iz * Reg.Cnt[1] + iy) * Reg.Cnt[0] + ix];
								if (ComputeCellVertex(Sampler, CellBase[0] + ix, CellBase[1] + iy, CellBase[2] + iz,
									Sc, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
								{
									Cell.Position.X += (SX == 1) ? ChunkWorldSpan : 0.0f;
									Cell.Position.Y += (SY == 1) ? ChunkWorldSpan : 0.0f;
									Cell.Position.Z += (SZ == 1) ? ChunkWorldSpan : 0.0f;
								}
							}
						}
					}
				}
			}
		}

		FSeamEmitter Emitter(Out, VoxelSize, Config.bGenerateUVs ? Config.UVScale : 0.0f);
		const FOctSampler FullSampler{ Req, 0xFF, FIntVector::ZeroValue };

		// The six axis halves — exactly the edge tuples' octant sets. A union contained in any
		// half is computable by a smaller job (edge tuple, face pair, or interior).
		const uint8 HalfMasks[6] = { 0x55, 0xAA, 0x33, 0xCC, 0x0F, 0xF0 };

		constexpr int32 RegKeyStride = 1 << 10;
		auto ClassifySlot = [&](const int32 SlotVox[3], FSeamRef& OutRef, uint8& UnionMask) -> bool
		{
			for (int32 ci = 0; ci < 27; ++ci)
			{
				FCornerRegion& Reg = Regions[ci];
				bool bInside = true;
				int32 Idx[3];
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const int32 Hi = Reg.Lo[Axis] + Reg.Cnt[Axis] * Reg.StrideC;
					if (SlotVox[Axis] < Reg.Lo[Axis] || SlotVox[Axis] >= Hi)
					{
						bInside = false;
						break;
					}
					Idx[Axis] = (SlotVox[Axis] - Reg.Lo[Axis]) / Reg.StrideC;
				}
				if (!bInside)
				{
					continue;
				}
				const int32 CellIdx = (Idx[2] * Reg.Cnt[1] + Idx[1]) * Reg.Cnt[0] + Idx[0];
				FSeamCellVertex& Cell = Reg.Cells[CellIdx];
				if (!Cell.bValid)
				{
					return false;
				}
				OutRef = FSeamRef{ &Cell, ci * RegKeyStride + CellIdx };
				UnionMask |= Reg.ParticipantMask;
				return true;
			}
			return false;
		};

		auto EmitCornerEdge = [&](int32 FX, int32 FY, int32 FZ, int32 EdgeAbsAxis)
		{
			const int32 BaseVox[3] = { FX * MinS, FY * MinS, FZ * MinS };
			int32 EndVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			EndVox[EdgeAbsAxis] += MinS;

			const float D0 = FullSampler.GetDensity(BaseVox[0], BaseVox[1], BaseVox[2]);
			const float D1 = FullSampler.GetDensity(EndVox[0], EndVox[1], EndVox[2]);
			const bool bSolid0 = (D0 >= IsoLevel);
			const bool bSolid1 = (D1 >= IsoLevel);
			if (bSolid0 == bSolid1)
			{
				return;
			}

			FSeamRef Refs[4];
			uint8 UnionMask = 0;
			for (int32 i = 0; i < 4; ++i)
			{
				const FCellOffset& Off = GAxisOffsets[EdgeAbsAxis][i];
				const int32 SlotVox[3] = {
					BaseVox[0] + Off.DX * MinS,
					BaseVox[1] + Off.DY * MinS,
					BaseVox[2] + Off.DZ * MinS };
				if (!ClassifySlot(SlotVox, Refs[i], UnionMask))
				{
					return;
				}
			}
			for (int32 h = 0; h < 6; ++h)
			{
				if ((UnionMask & ~HalfMasks[h]) == 0)
				{
					return; // a smaller job (edge/face/interior) owns this
				}
			}

			const bool bFlip = (D0 < IsoLevel);
			int32 SolidVox[3] = { BaseVox[0], BaseVox[1], BaseVox[2] };
			if (bFlip)
			{
				SolidVox[EdgeAbsAxis] += MinS;
			}
			const FVoxelData SolidVoxel = FullSampler.GetVoxel(SolidVox[0], SolidVox[1], SolidVox[2]);
			Emitter.EmitFan(Refs, bFlip, SolidVoxel.MaterialID, SolidVoxel.BiomeID);
		};

		const int32 FLo = (ChunkSize - 2 * MaxS) / MinS;
		const int32 FHi = (ChunkSize + 2 * MaxS) / MinS;
		for (int32 FZ = FLo; FZ < FHi; ++FZ)
		{
			for (int32 FY = FLo; FY < FHi; ++FY)
			{
				for (int32 FX = FLo; FX < FHi; ++FX)
				{
					EmitCornerEdge(FX, FY, FZ, 0);
					EmitCornerEdge(FX, FY, FZ, 1);
					EmitCornerEdge(FX, FY, FZ, 2);
				}
			}
		}

		UE_LOG(LogVoxelMeshing, Verbose,
			TEXT("DC mixed corner seam %s: %d verts, %d tris"),
			*Req.OwnerChunkCoord.ToString(), Out.Positions.Num(), Emitter.TriangleCount);

		return true;
	}
}

bool FVoxelCPUDualContourMesher::GenerateFaceSeamMeshCPU(
	const FVoxelFaceSeamRequest& SeamRequest,
	FChunkMeshData& OutMeshData)
{
	using namespace VoxelDCFaceSeam;

	OutMeshData.Reset();

	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GenerateFaceSeamMeshCPU called before Initialize"));
		return false;
	}
	if (!SeamRequest.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateFaceSeamMeshCPU: invalid seam request (axis=%d, chunkSize=%d, A=%d, B=%d voxels)"),
			SeamRequest.Axis, SeamRequest.ChunkSize,
			SeamRequest.VoxelDataA.IsValid() ? SeamRequest.VoxelDataA->Num() : 0,
			SeamRequest.VoxelDataB.IsValid() ? SeamRequest.VoxelDataB->Num() : 0);
		return false;
	}

	// Mixed-LOD pair (seam-ownership P2c): a dedicated path — the same-LOD path below stays
	// bit-frozen (its ring identities are asserted by the SC tests and relied on by P2a/P2b).
	if (SeamRequest.GetLODLevelB() != SeamRequest.LODLevel)
	{
		return GenerateMixedFaceSeam(SeamRequest, Config, OutMeshData);
	}

	const int32 ChunkSize = SeamRequest.ChunkSize;
	const int32 LODLevel = FMath::Clamp(SeamRequest.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;
	if ((ChunkSize % Stride) != 0)
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateFaceSeamMeshCPU: ChunkSize %d not divisible by stride %d"), ChunkSize, Stride);
		return false;
	}
	const int32 GridSize = ChunkSize / Stride;
	if (GridSize < 3)
	{
		// No exclusively-owned face cells at this resolution (everything is edge/corner-seam
		// territory) — an empty seam is the correct output, not an error.
		return true;
	}

	const int32 U = SeamRequest.Axis;          // face normal axis
	const int32 V = (U + 1) % 3;               // transverse axes
	const int32 W = (U + 2) % 3;
	const int32 SL = GridSize - 1;             // slab cell layer (owner frame): == neighbour's -1 layer
	const int32 TCount = SL;                   // exclusive transverse cell range per axis: [0, SL)
	const float VoxelSize = SeamRequest.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;
	const float FrameOffsetU = static_cast<float>(ChunkSize) * VoxelSize;

	const FSeamSampler SamplerA(SeamRequest, FSeamSampler::EMode::AOnly);
	const FSeamSampler SamplerB(SeamRequest, FSeamSampler::EMode::BOnly);
	const FSeamSampler SamplerC(SeamRequest, FSeamSampler::EMode::Combined);

	// ---- Cell layers -------------------------------------------------------------------------
	// Layer 0 = ring A  (owner cell U == SL-1; A-only view; owner frame)      — interior ring dup
	// Layer 1 = slab    (owner cell U == SL;   combined view; owner frame)    — the seam proper
	// Layer 2 = ring B  (B-frame cell U == 0;  B-only view; shifted to owner) — interior ring dup
	// Transverse extent: cells (v, w) in [0, SL)^2 — the exclusive face region (cells at
	// transverse -1/SL belong to edge/corner seams, P2).
	TArray<FSeamCellVertex> Layers[3];
	for (int32 L = 0; L < 3; ++L)
	{
		Layers[L].SetNum(TCount * TCount);
	}

	for (int32 w = 0; w < TCount; ++w)
	{
		for (int32 v = 0; v < TCount; ++v)
		{
			const int32 Idx = v + w * TCount;

			int32 C[3];
			C[V] = v;
			C[W] = w;

			// Ring A: recompute exactly as A's Interior pass did (A data, Air clamp, A frame).
			C[U] = SL - 1;
			ComputeCellVertex(SamplerA, C[0], C[1], C[2], Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Layers[0][Idx]);

			// Slab: both sides' data, full hermite reach across the face (the seam's whole point).
			C[U] = SL;
			ComputeCellVertex(SamplerC, C[0], C[1], C[2], Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Layers[1][Idx]);

			// Ring B: recompute exactly as B's Interior pass did — in B's OWN frame (bit-identical
			// solve), then translate the result into the owner frame by one chunk along the axis.
			C[U] = 0;
			if (ComputeCellVertex(SamplerB, C[0], C[1], C[2], Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Layers[2][Idx]))
			{
				Layers[2][Idx].Position[U] += FrameOffsetU;
			}
		}
	}

	// ---- Vertex emission (replicates EmitVertex: triplanar UV + UV1/color material carry) -----
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
	auto EmitSeamVertex = [&OutMeshData, VoxelSize, UVScale](const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID) -> int32
	{
		const int32 Index = OutMeshData.Positions.Num();
		OutMeshData.Positions.Add(Vertex.Position);
		OutMeshData.Normals.Add(Vertex.Normal);

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
		OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
		OutMeshData.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
		return Index;
	};

	// Owner-frame cell U-layer -> layer array index (or -1 = not a seam-tracked layer).
	auto LayerOf = [SL](int32 CU) -> int32
	{
		return (CU == SL - 1) ? 0 : (CU == SL) ? 1 : (CU == SL + 1) ? 2 : -1;
	};

	TMap<uint64, int32> DuplicateVertexCache;
	uint32 TriangleCount = 0;

	// Emit the quad dual to one owned crossing edge. Mirrors GenerateQuads' flow: winding from
	// the edge-base density sign, per-quad material from the solid endpoint, per-cell/-material
	// vertex dedup, and the adaptive max-min-area diagonal split (the DT7 T-junction fix).
	auto EmitQuadForEdge = [&](const int32 EdgeCell[3], int32 EdgeAxis) -> void
	{
		const int32 VX = EdgeCell[0] * Stride;
		const int32 VY = EdgeCell[1] * Stride;
		const int32 VZ = EdgeCell[2] * Stride;

		const float D0 = SamplerC.GetDensity(VX, VY, VZ);
		int32 NX = VX, NY = VY, NZ = VZ;
		if (EdgeAxis == 0) NX += Stride;
		else if (EdgeAxis == 1) NY += Stride;
		else NZ += Stride;
		const float D1 = SamplerC.GetDensity(NX, NY, NZ);

		const bool bSolid0 = (D0 >= IsoLevel);
		const bool bSolid1 = (D1 >= IsoLevel);
		if (bSolid0 == bSolid1)
		{
			return; // no crossing on this edge
		}

		// Look up the 4 surrounding cell vertices across the three layers.
		FSeamCellVertex* Verts[4];
		int32 CellKeys[4];
		for (int32 i = 0; i < 4; ++i)
		{
			const FCellOffset& Off = GAxisOffsets[EdgeAxis][i];
			const int32 CellC[3] = { EdgeCell[0] + Off.DX, EdgeCell[1] + Off.DY, EdgeCell[2] + Off.DZ };
			const int32 Layer = LayerOf(CellC[U]);
			const int32 TV = CellC[V];
			const int32 TW = CellC[W];
			if (Layer < 0 || TV < 0 || TV >= TCount || TW < 0 || TW >= TCount)
			{
				return; // touches a cell outside the exclusive face region (P2 territory)
			}
			FSeamCellVertex& Cell = Layers[Layer][TV + TW * TCount];
			if (!Cell.bValid)
			{
				return; // matches the interior pass's all-cells-valid quad rule
			}
			Verts[i] = &Cell;
			CellKeys[i] = Layer * TCount * TCount + (TV + TW * TCount);
		}

		const bool bFlip = (D0 < IsoLevel);

		// Per-quad material from the solid endpoint (triangles must stay material-uniform).
		int32 SolidX = VX, SolidY = VY, SolidZ = VZ;
		if (bFlip)
		{
			if (EdgeAxis == 0) { SolidX += Stride; }
			else if (EdgeAxis == 1) { SolidY += Stride; }
			else { SolidZ += Stride; }
		}
		const FVoxelData SolidVoxel = SamplerC.GetVoxel(SolidX, SolidY, SolidZ);
		const uint8 QuadMaterial = SolidVoxel.MaterialID;
		const uint8 QuadBiome = SolidVoxel.BiomeID;

		int32 Indices[4];
		for (int32 i = 0; i < 4; ++i)
		{
			FSeamCellVertex& Cell = *Verts[i];
			if (Cell.MeshVertexIndex >= 0 && Cell.EmittedMaterialID == QuadMaterial && Cell.EmittedBiomeID == QuadBiome)
			{
				Indices[i] = Cell.MeshVertexIndex;
			}
			else if (Cell.MeshVertexIndex < 0)
			{
				Indices[i] = EmitSeamVertex(Cell, QuadMaterial, QuadBiome);
				Cell.MeshVertexIndex = Indices[i];
				Cell.EmittedMaterialID = QuadMaterial;
				Cell.EmittedBiomeID = QuadBiome;
			}
			else
			{
				const uint64 DupKey = (static_cast<uint64>(CellKeys[i]) << 16)
					| (static_cast<uint64>(QuadMaterial) << 8)
					| static_cast<uint64>(QuadBiome);
				if (const int32* Found = DuplicateVertexCache.Find(DupKey))
				{
					Indices[i] = *Found;
				}
				else
				{
					Indices[i] = EmitSeamVertex(Cell, QuadMaterial, QuadBiome);
					DuplicateVertexCache.Add(DupKey, Indices[i]);
				}
			}
		}

		// Adaptive max-min-area diagonal (mirrors GenerateQuads; preserves the DT7 sliver fix).
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

		TriangleCount += 2;
	};

	// ---- Owned edges -------------------------------------------------------------------------
	// The seam owns exactly the edges whose surrounding cells include >= 1 slab cell and lie
	// entirely within the exclusive face region. In the owner frame (SL = GridSize-1):
	//   group 1: U-axis edges at cell U == SL       (4 slab cells)          v,w in [1, SL-1]
	//   group 2: V-axis edges at cell U == SL       (slab + ring-A cells)   v in [0, SL-1], w in [1, SL-1]
	//   group 3: W-axis edges at cell U == SL       (slab + ring-A cells)   v in [1, SL-1], w in [0, SL-1]
	//   group 4: V-axis edges at cell U == SL + 1   (ring-B + slab cells)   v in [0, SL-1], w in [1, SL-1]
	//   group 5: W-axis edges at cell U == SL + 1   (ring-B + slab cells)   v in [1, SL-1], w in [0, SL-1]
	// Everything else is owned by an interior pass (all four cells interior to one chunk) or by
	// a P2 edge/corner seam (cells spanning two+ slabs). This partition is exactly-once.
	int32 EdgeCell[3];

	// Group 1 — face-crossing edges.
	EdgeCell[U] = SL;
	for (int32 w = 1; w <= SL - 1; ++w)
	{
		for (int32 v = 1; v <= SL - 1; ++v)
		{
			EdgeCell[V] = v;
			EdgeCell[W] = w;
			EmitQuadForEdge(EdgeCell, U);
		}
	}

	// Groups 2-5 — stitching edges on both sides of the slab.
	for (int32 Side = 0; Side < 2; ++Side)
	{
		EdgeCell[U] = SL + Side; // SL, then SL+1

		// V-axis edges (groups 2 / 4)
		for (int32 w = 1; w <= SL - 1; ++w)
		{
			for (int32 v = 0; v <= SL - 1; ++v)
			{
				EdgeCell[V] = v;
				EdgeCell[W] = w;
				EmitQuadForEdge(EdgeCell, V);
			}
		}

		// W-axis edges (groups 3 / 5)
		for (int32 w = 0; w <= SL - 1; ++w)
		{
			for (int32 v = 1; v <= SL - 1; ++v)
			{
				EdgeCell[V] = v;
				EdgeCell[W] = w;
				EmitQuadForEdge(EdgeCell, W);
			}
		}
	}

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("DC face seam %s axis=%d LOD=%d: %d verts, %d tris"),
		*SeamRequest.OwnerChunkCoord.ToString(), U, LODLevel,
		OutMeshData.Positions.Num(), TriangleCount);

	return true;
}

bool FVoxelCPUDualContourMesher::GenerateEdgeSeamMeshCPU(
	const FVoxelEdgeSeamRequest& SeamRequest,
	FChunkMeshData& OutMeshData)
{
	using namespace VoxelDCFaceSeam;

	OutMeshData.Reset();

	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GenerateEdgeSeamMeshCPU called before Initialize"));
		return false;
	}
	if (!SeamRequest.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateEdgeSeamMeshCPU: invalid seam request (axis=%d, chunkSize=%d)"),
			SeamRequest.EdgeAxis, SeamRequest.ChunkSize);
		return false;
	}

	// Mixed-LOD tuple (seam-ownership P2d): dedicated path — the same-LOD path below stays
	// bit-frozen (its ring identities are asserted by the ES tests).
	if (!SeamRequest.IsUniformLOD())
	{
		return GenerateMixedEdgeSeam(SeamRequest, Config, OutMeshData);
	}

	const int32 ChunkSize = SeamRequest.ChunkSize;
	const int32 LODLevel = FMath::Clamp(SeamRequest.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;
	if ((ChunkSize % Stride) != 0)
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateEdgeSeamMeshCPU: ChunkSize %d not divisible by stride %d"), ChunkSize, Stride);
		return false;
	}
	const int32 GridSize = ChunkSize / Stride;
	if (GridSize < 3)
	{
		return true; // no exclusively-owned edge cells at this resolution — valid empty seam
	}

	const int32 U = SeamRequest.EdgeAxis;                    // edge-parallel axis
	const int32 PerpA = (U == 0) ? 1 : 0;                    // perpendicular axes, ASCENDING
	const int32 PerpB = (U == 2) ? 1 : 2;                    // (matches registry participant order)
	const int32 SL = GridSize - 1;
	const int32 UCount = SL;                                 // edge-exclusive cells: u in [0, SL)
	const float VoxelSize = SeamRequest.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;
	const float ChunkWorldSpan = static_cast<float>(ChunkSize) * VoxelSize;

	// ---- The 9 cell columns around the edge (owner-frame transverse offsets dcv/dcw in [-1,1]).
	// Column (0,0) is the edge-exclusive column (all four quadrants visible, owner frame). The 8
	// ring columns recompute EXACTLY what their original jobs computed: participant interiors
	// (single quadrant, that participant's frame) and face slabs (the face job's pair mask, the
	// face OWNER's frame). Frame rule: a column computes in the quadrant frame of its minimum
	// participating side per axis — dc == +1 selects the far frame, else the owner-side frame.
	struct FColumnConfig
	{
		uint8 Mask = 0;
		FIntVector FrameOffsetVoxels = FIntVector::ZeroValue;
		int32 FrameCellA = 0;   // frame-local cell coord along PerpA
		int32 FrameCellB = 0;   // frame-local cell coord along PerpB
		float TranslateA = 0.f; // owner-frame position translation along PerpA
		float TranslateB = 0.f;
	};
	FColumnConfig Columns[9];
	TArray<FSeamCellVertex> Layers[9];

	auto ColumnIndex = [](int32 Dcv, int32 Dcw) { return (Dcv + 1) + (Dcw + 1) * 3; };

	for (int32 Dcw = -1; Dcw <= 1; ++Dcw)
	{
		for (int32 Dcv = -1; Dcv <= 1; ++Dcv)
		{
			FColumnConfig& Col = Columns[ColumnIndex(Dcv, Dcw)];

			// Per-axis visibility: 0 = pair (slab straddles), -1 = near quadrant only, +1 = far only.
			const int32 DvBitsAllowed[2] = { (Dcv <= 0) ? 1 : 0, (Dcv >= 0) ? 1 : 0 }; // [dv=0 allowed, dv=1 allowed]
			const int32 DwBitsAllowed[2] = { (Dcw <= 0) ? 1 : 0, (Dcw >= 0) ? 1 : 0 };
			for (int32 dw = 0; dw <= 1; ++dw)
			{
				for (int32 dv = 0; dv <= 1; ++dv)
				{
					if (DvBitsAllowed[dv] && DwBitsAllowed[dw])
					{
						Col.Mask |= (1 << (dv + dw * 2));
					}
				}
			}

			const int32 FrameDv = (Dcv == 1) ? 1 : 0;
			const int32 FrameDw = (Dcw == 1) ? 1 : 0;
			Col.FrameOffsetVoxels = FIntVector::ZeroValue;
			Col.FrameOffsetVoxels[PerpA] = FrameDv * ChunkSize;
			Col.FrameOffsetVoxels[PerpB] = FrameDw * ChunkSize;
			Col.FrameCellA = (Dcv == 1) ? 0 : (SL + Dcv); // -1 -> SL-1, 0 -> SL, +1 -> 0 (far frame)
			Col.FrameCellB = (Dcw == 1) ? 0 : (SL + Dcw);
			Col.TranslateA = FrameDv * ChunkWorldSpan;
			Col.TranslateB = FrameDw * ChunkWorldSpan;

			Layers[ColumnIndex(Dcv, Dcw)].SetNum(UCount);

			const FQuadSampler Sampler{ SeamRequest, U, PerpA, PerpB, Col.Mask, Col.FrameOffsetVoxels };
			for (int32 u = 0; u < UCount; ++u)
			{
				int32 C[3];
				C[U] = u;
				C[PerpA] = Col.FrameCellA;
				C[PerpB] = Col.FrameCellB;
				FSeamCellVertex& Cell = Layers[ColumnIndex(Dcv, Dcw)][u];
				if (ComputeCellVertex(Sampler, C[0], C[1], C[2], Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
				{
					Cell.Position[PerpA] += Col.TranslateA;
					Cell.Position[PerpB] += Col.TranslateB;
				}
			}
		}
	}

	// ---- Vertex emission (identical to the face-seam path) ---------------------------------
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
	auto EmitSeamVertex = [&OutMeshData, VoxelSize, UVScale](const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID) -> int32
	{
		const int32 Index = OutMeshData.Positions.Num();
		OutMeshData.Positions.Add(Vertex.Position);
		OutMeshData.Normals.Add(Vertex.Normal);

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
		OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
		OutMeshData.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
		return Index;
	};

	// Full-quad owner-frame sampler: crossing existence, winding, and material for owned edges.
	const FQuadSampler FullSampler{ SeamRequest, U, PerpA, PerpB, 0b1111, FIntVector::ZeroValue };

	TMap<uint64, int32> DuplicateVertexCache;
	uint32 TriangleCount = 0;

	auto EmitQuadForEdge = [&](const int32 EdgeCell[3], int32 EdgeAxis) -> void
	{
		const int32 VX = EdgeCell[0] * Stride;
		const int32 VY = EdgeCell[1] * Stride;
		const int32 VZ = EdgeCell[2] * Stride;

		const float D0 = FullSampler.GetDensity(VX, VY, VZ);
		int32 NX = VX, NY = VY, NZ = VZ;
		if (EdgeAxis == 0) NX += Stride;
		else if (EdgeAxis == 1) NY += Stride;
		else NZ += Stride;
		const float D1 = FullSampler.GetDensity(NX, NY, NZ);

		const bool bSolid0 = (D0 >= IsoLevel);
		const bool bSolid1 = (D1 >= IsoLevel);
		if (bSolid0 == bSolid1)
		{
			return;
		}

		FSeamCellVertex* Verts[4];
		int32 CellKeys[4];
		for (int32 i = 0; i < 4; ++i)
		{
			const FCellOffset& Off = GAxisOffsets[EdgeAxis][i];
			const int32 CellC[3] = { EdgeCell[0] + Off.DX, EdgeCell[1] + Off.DY, EdgeCell[2] + Off.DZ };
			const int32 Dcv = CellC[PerpA] - SL;
			const int32 Dcw = CellC[PerpB] - SL;
			const int32 CU = CellC[U];
			if (Dcv < -1 || Dcv > 1 || Dcw < -1 || Dcw > 1 || CU < 0 || CU >= UCount)
			{
				return; // outside the edge-seam region (interior / face / corner territory)
			}
			const int32 Layer = ColumnIndex(Dcv, Dcw);
			FSeamCellVertex& Cell = Layers[Layer][CU];
			if (!Cell.bValid)
			{
				return;
			}
			Verts[i] = &Cell;
			CellKeys[i] = Layer * UCount + CU;
		}

		const bool bFlip = (D0 < IsoLevel);

		int32 SolidX = VX, SolidY = VY, SolidZ = VZ;
		if (bFlip)
		{
			if (EdgeAxis == 0) { SolidX += Stride; }
			else if (EdgeAxis == 1) { SolidY += Stride; }
			else { SolidZ += Stride; }
		}
		const FVoxelData SolidVoxel = FullSampler.GetVoxel(SolidX, SolidY, SolidZ);
		const uint8 QuadMaterial = SolidVoxel.MaterialID;
		const uint8 QuadBiome = SolidVoxel.BiomeID;

		int32 Indices[4];
		for (int32 i = 0; i < 4; ++i)
		{
			FSeamCellVertex& Cell = *Verts[i];
			if (Cell.MeshVertexIndex >= 0 && Cell.EmittedMaterialID == QuadMaterial && Cell.EmittedBiomeID == QuadBiome)
			{
				Indices[i] = Cell.MeshVertexIndex;
			}
			else if (Cell.MeshVertexIndex < 0)
			{
				Indices[i] = EmitSeamVertex(Cell, QuadMaterial, QuadBiome);
				Cell.MeshVertexIndex = Indices[i];
				Cell.EmittedMaterialID = QuadMaterial;
				Cell.EmittedBiomeID = QuadBiome;
			}
			else
			{
				const uint64 DupKey = (static_cast<uint64>(CellKeys[i]) << 16)
					| (static_cast<uint64>(QuadMaterial) << 8)
					| static_cast<uint64>(QuadBiome);
				if (const int32* Found = DuplicateVertexCache.Find(DupKey))
				{
					Indices[i] = *Found;
				}
				else
				{
					Indices[i] = EmitSeamVertex(Cell, QuadMaterial, QuadBiome);
					DuplicateVertexCache.Add(DupKey, Indices[i]);
				}
			}
		}

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

		TriangleCount += 2;
	};

	// ---- Owned edges (exactly-once partition, owner frame) ---------------------------------
	// A DC edge belongs to the edge seam iff its 4 cells include >= 1 edge-column cell and no
	// corner cell (u outside [0, SL)). Enumerated directly:
	//   U-axis edges     at (u, cv, cw), cv,cw in {SL, SL+1}, u in [0, SL)
	//   PerpA-axis edges at (u, SL, cw), cw in {SL, SL+1},    u in [1, SL-1]
	//   PerpB-axis edges at (u, cv, SL), cv in {SL, SL+1},    u in [1, SL-1]
	int32 EdgeCell[3];

	for (int32 cw = SL; cw <= SL + 1; ++cw)
	{
		for (int32 cv = SL; cv <= SL + 1; ++cv)
		{
			for (int32 u = 0; u < SL; ++u)
			{
				EdgeCell[U] = u;
				EdgeCell[PerpA] = cv;
				EdgeCell[PerpB] = cw;
				EmitQuadForEdge(EdgeCell, U);
			}
		}
	}
	for (int32 Sel = SL; Sel <= SL + 1; ++Sel)
	{
		for (int32 u = 1; u <= SL - 1; ++u)
		{
			EdgeCell[U] = u;
			EdgeCell[PerpA] = SL;
			EdgeCell[PerpB] = Sel;
			EmitQuadForEdge(EdgeCell, PerpA);

			EdgeCell[PerpA] = Sel;
			EdgeCell[PerpB] = SL;
			EmitQuadForEdge(EdgeCell, PerpB);
		}
	}

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("DC edge seam %s axis=%d LOD=%d: %d verts, %d tris"),
		*SeamRequest.OwnerChunkCoord.ToString(), U, LODLevel,
		OutMeshData.Positions.Num(), TriangleCount);

	return true;
}

bool FVoxelCPUDualContourMesher::GenerateCornerSeamMeshCPU(
	const FVoxelCornerSeamRequest& SeamRequest,
	FChunkMeshData& OutMeshData)
{
	using namespace VoxelDCFaceSeam;

	OutMeshData.Reset();

	if (!bIsInitialized)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("GenerateCornerSeamMeshCPU called before Initialize"));
		return false;
	}
	if (!SeamRequest.IsValid())
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateCornerSeamMeshCPU: invalid seam request (chunkSize=%d)"),
			SeamRequest.ChunkSize);
		return false;
	}

	// Mixed-LOD tuple (seam-ownership P2d): dedicated path — the same-LOD path below stays
	// bit-frozen (its ring identities are asserted by the CS tests).
	if (!SeamRequest.IsUniformLOD())
	{
		return GenerateMixedCornerSeam(SeamRequest, Config, OutMeshData);
	}

	const int32 ChunkSize = SeamRequest.ChunkSize;
	const int32 LODLevel = FMath::Clamp(SeamRequest.LODLevel, 0, 7);
	const int32 Stride = 1 << LODLevel;
	if ((ChunkSize % Stride) != 0)
	{
		UE_LOG(LogVoxelMeshing, Error, TEXT("GenerateCornerSeamMeshCPU: ChunkSize %d not divisible by stride %d"), ChunkSize, Stride);
		return false;
	}
	const int32 GridSize = ChunkSize / Stride;
	if (GridSize < 3)
	{
		return true; // degenerate resolution — valid empty seam
	}

	const int32 SL = GridSize - 1;
	const float VoxelSize = SeamRequest.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;
	const float ChunkWorldSpan = static_cast<float>(ChunkSize) * VoxelSize;

	// ---- The 3x3x3 cell block around the corner cell (per-axis state s in {-1,0,+1} relative
	// to SL). One unified rule generates every original job's exact configuration:
	//   mask:  s == 0 -> both halves visible on that axis; s == -1 -> lower only; +1 -> upper only
	//   frame: s == +1 computes in the far chunk's frame on that axis (offset +ChunkSize)
	//   cell:  frame-local coord = (s == +1) ? 0 : SL + s
	// This reproduces: the corner cell itself (all 8 octants, owner frame), the six edge-column
	// end cells (4-octant masks == the edge jobs' quad samplers), the twelve face-slab corner
	// cells (2-octant masks == the face jobs' pair samplers), and the participant-interior
	// corner cells (single octants == the Interior passes). Cells whose state has no zero
	// component are never referenced by an owned quad; they are computed harmlessly.
	FSeamCellVertex Cells[27];
	auto CellIndexOf = [](int32 SX, int32 SY, int32 SZ) { return (SX + 1) + (SY + 1) * 3 + (SZ + 1) * 9; };

	for (int32 SZ = -1; SZ <= 1; ++SZ)
	{
		for (int32 SY = -1; SY <= 1; ++SY)
		{
			for (int32 SX = -1; SX <= 1; ++SX)
			{
				const int32 S[3] = { SX, SY, SZ };
				uint8 Mask = 0;
				for (int32 Oct = 0; Oct < 8; ++Oct)
				{
					const int32 D[3] = { Oct & 1, (Oct >> 1) & 1, (Oct >> 2) & 1 };
					bool bAllowed = true;
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						if ((S[Axis] == -1 && D[Axis] != 0) || (S[Axis] == 1 && D[Axis] != 1))
						{
							bAllowed = false;
							break;
						}
					}
					if (bAllowed)
					{
						Mask |= (1 << Oct);
					}
				}

				FIntVector FrameOffset(
					(SX == 1) ? ChunkSize : 0,
					(SY == 1) ? ChunkSize : 0,
					(SZ == 1) ? ChunkSize : 0);
				const int32 CellX = (SX == 1) ? 0 : (SL + SX);
				const int32 CellY = (SY == 1) ? 0 : (SL + SY);
				const int32 CellZ = (SZ == 1) ? 0 : (SL + SZ);

				const FOctSampler Sampler{ SeamRequest, Mask, FrameOffset };
				FSeamCellVertex& Cell = Cells[CellIndexOf(SX, SY, SZ)];
				if (ComputeCellVertex(Sampler, CellX, CellY, CellZ, Stride, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
				{
					Cell.Position.X += (SX == 1) ? ChunkWorldSpan : 0.0f;
					Cell.Position.Y += (SY == 1) ? ChunkWorldSpan : 0.0f;
					Cell.Position.Z += (SZ == 1) ? ChunkWorldSpan : 0.0f;
				}
			}
		}
	}

	// ---- Vertex emission (identical to the face/edge seam paths) ---------------------------
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
	auto EmitSeamVertex = [&OutMeshData, VoxelSize, UVScale](const FSeamCellVertex& Vertex, uint8 MaterialID, uint8 BiomeID) -> int32
	{
		const int32 Index = OutMeshData.Positions.Num();
		OutMeshData.Positions.Add(Vertex.Position);
		OutMeshData.Normals.Add(Vertex.Normal);

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
		OutMeshData.UV1s.Add(FVector2f(static_cast<float>(MaterialID), 0.0f));
		OutMeshData.Colors.Add(FColor(MaterialID, BiomeID, 0, 255));
		return Index;
	};

	const FOctSampler FullSampler{ SeamRequest, 0xFF, FIntVector::ZeroValue };

	TMap<uint64, int32> DuplicateVertexCache;
	uint32 TriangleCount = 0;

	auto EmitQuadForEdge = [&](const int32 EdgeCell[3], int32 EdgeAxis) -> void
	{
		const int32 VX = EdgeCell[0] * Stride;
		const int32 VY = EdgeCell[1] * Stride;
		const int32 VZ = EdgeCell[2] * Stride;

		const float D0 = FullSampler.GetDensity(VX, VY, VZ);
		int32 NX = VX, NY = VY, NZ = VZ;
		if (EdgeAxis == 0) NX += Stride;
		else if (EdgeAxis == 1) NY += Stride;
		else NZ += Stride;
		const float D1 = FullSampler.GetDensity(NX, NY, NZ);

		const bool bSolid0 = (D0 >= IsoLevel);
		const bool bSolid1 = (D1 >= IsoLevel);
		if (bSolid0 == bSolid1)
		{
			return;
		}

		FSeamCellVertex* Verts[4];
		int32 CellKeys[4];
		for (int32 i = 0; i < 4; ++i)
		{
			const FCellOffset& Off = GAxisOffsets[EdgeAxis][i];
			const int32 SX = (EdgeCell[0] + Off.DX) - SL;
			const int32 SY = (EdgeCell[1] + Off.DY) - SL;
			const int32 SZ = (EdgeCell[2] + Off.DZ) - SL;
			if (SX < -1 || SX > 1 || SY < -1 || SY > 1 || SZ < -1 || SZ > 1)
			{
				return; // outside the corner block (owned elsewhere)
			}
			const int32 Idx = CellIndexOf(SX, SY, SZ);
			FSeamCellVertex& Cell = Cells[Idx];
			if (!Cell.bValid)
			{
				return;
			}
			Verts[i] = &Cell;
			CellKeys[i] = Idx;
		}

		const bool bFlip = (D0 < IsoLevel);

		int32 SolidX = VX, SolidY = VY, SolidZ = VZ;
		if (bFlip)
		{
			if (EdgeAxis == 0) { SolidX += Stride; }
			else if (EdgeAxis == 1) { SolidY += Stride; }
			else { SolidZ += Stride; }
		}
		const FVoxelData SolidVoxel = FullSampler.GetVoxel(SolidX, SolidY, SolidZ);
		const uint8 QuadMaterial = SolidVoxel.MaterialID;
		const uint8 QuadBiome = SolidVoxel.BiomeID;

		int32 Indices[4];
		for (int32 i = 0; i < 4; ++i)
		{
			FSeamCellVertex& Cell = *Verts[i];
			if (Cell.MeshVertexIndex >= 0 && Cell.EmittedMaterialID == QuadMaterial && Cell.EmittedBiomeID == QuadBiome)
			{
				Indices[i] = Cell.MeshVertexIndex;
			}
			else if (Cell.MeshVertexIndex < 0)
			{
				Indices[i] = EmitSeamVertex(Cell, QuadMaterial, QuadBiome);
				Cell.MeshVertexIndex = Indices[i];
				Cell.EmittedMaterialID = QuadMaterial;
				Cell.EmittedBiomeID = QuadBiome;
			}
			else
			{
				const uint64 DupKey = (static_cast<uint64>(CellKeys[i]) << 16)
					| (static_cast<uint64>(QuadMaterial) << 8)
					| static_cast<uint64>(QuadBiome);
				if (const int32* Found = DuplicateVertexCache.Find(DupKey))
				{
					Indices[i] = *Found;
				}
				else
				{
					Indices[i] = EmitSeamVertex(Cell, QuadMaterial, QuadBiome);
					DuplicateVertexCache.Add(DupKey, Indices[i]);
				}
			}
		}

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

		TriangleCount += 2;
	};

	// ---- Owned edges: the 12 DC edges whose surrounding cells include the corner cell — for
	// each axis, the 2x2 fan at the corner (exactly-once: face jobs exclude multi-slab cells,
	// edge jobs exclude cells beyond their column's [0, SL) extent).
	for (int32 EdgeAxis = 0; EdgeAxis < 3; ++EdgeAxis)
	{
		const int32 A1 = (EdgeAxis + 1) % 3;
		const int32 A2 = (EdgeAxis + 2) % 3;
		for (int32 VB = SL; VB <= SL + 1; ++VB)
		{
			for (int32 VA = SL; VA <= SL + 1; ++VA)
			{
				int32 EdgeCell[3];
				EdgeCell[EdgeAxis] = SL;
				EdgeCell[A1] = VA;
				EdgeCell[A2] = VB;
				EmitQuadForEdge(EdgeCell, EdgeAxis);
			}
		}
	}

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("DC corner seam %s LOD=%d: %d verts, %d tris"),
		*SeamRequest.OwnerChunkCoord.ToString(), LODLevel,
		OutMeshData.Positions.Num(), TriangleCount);

	return true;
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
