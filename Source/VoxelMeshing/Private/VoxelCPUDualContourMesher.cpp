// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUDualContourMesher.h"
#include "VoxelCPUDualContourShared.h"
#include "VoxelMeshing.h"
#include "QEFSolver.h"
#include "VoxelCrossingGradient.h"
#include "VoxelMeshingVoxelSampler.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarMeshDCDenseArrays(
	TEXT("voxel.Mesh.DCDenseArrays"),
	0,
	TEXT("A/B (G275): 1 = Dual Contouring's chunk meshes keep an edge crossing and a cell vertex for every cell of the chunk, as ")
	TEXT("before (48 MB zeroed and walked for a 64-voxel chunk); 0 = a sign per corner and the crossings and vertices of the ")
	TEXT("surface's cells only (default). The meshes are the same, bit for bit."),
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
	OutMeshData.MaterialBlends.Reserve(EstimatedTriangles * 3);
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

	TArray<int32> ValidEdgeIndices;
	ValidEdgeIndices.Reserve(GridSize * GridSize * 4);
	double SetupEnd = 0.0;
	double CrossingsEnd = 0.0;
	double VerticesEnd = 0.0;
	if (CVarMeshDCDenseArrays.GetValueOnAnyThread() != 0)
	{
		// Pass 1: Detect edge crossings (flat array: 3 edges per cell position)
		TArray<FDCEdgeCrossing> EdgeCrossings;
		EdgeCrossings.SetNumZeroed(TotalCells * 3);
		TArray<FDCCellVertex> CellVertices;
		CellVertices.SetNumZeroed(TotalCells);
		SetupEnd = FPlatformTime::Seconds();
		DetectEdgeCrossings(Request, Stride, GridDim, EdgeCrossings, ValidEdgeIndices);
		CrossingsEnd = FPlatformTime::Seconds();

		// Pass 2: Solve QEF for cell vertices
		SolveCellVertices(Request, Stride, GridDim, EdgeCrossings, CellVertices);
		VerticesEnd = FPlatformTime::Seconds();

		// Pass 3: Generate quads
		GenerateQuads(Request, Stride, GridDim, EdgeCrossings, ValidEdgeIndices, CellVertices, OutMeshData, TriangleCount);
	}
	else
	{
		// The same three passes over the surface's cells only (G275). A chunk's surface crosses a few percent of its cells:
		// a sign per corner says which, and only those cells' crossings and vertices are made and kept, in the order the
		// dense passes make them, so the mesh is the same. The dense arrays cost 48 MB zeroed and walked for a chunk of 64
		// voxels: a mesh bound by memory, slower beside other meshes, and a load on the cache every other thread shares.
		FDCSurfaceGrid Grid;
		DetectCornerSigns(Request, Stride, GridDim, Grid);
		SetupEnd = FPlatformTime::Seconds();
		DetectSurfaceCrossings(Request, Stride, GridDim, Grid, ValidEdgeIndices);
		CrossingsEnd = FPlatformTime::Seconds();
		SolveSurfaceVertices(Request, Stride, GridDim, Grid);
		VerticesEnd = FPlatformTime::Seconds();
		GenerateSurfaceQuads(Request, Stride, GridDim, Grid, ValidEdgeIndices, OutMeshData, TriangleCount);
	}

	// Calculate stats
	const double EndTime = FPlatformTime::Seconds();
	OutStats.SetupTimeMs = static_cast<float>((SetupEnd - StartTime) * 1000.0);
	OutStats.CrossingsTimeMs = static_cast<float>((CrossingsEnd - SetupEnd) * 1000.0);
	OutStats.VerticesTimeMs = static_cast<float>((VerticesEnd - CrossingsEnd) * 1000.0);
	OutStats.QuadsTimeMs = static_cast<float>((EndTime - VerticesEnd) * 1000.0);
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
						const int32 EIdx = EdgeIndex(CX, CY, CZ, Axis, GridDim);
						ComputeEdgeCrossing(Request, Stride, VX, VY, VZ, Axis, D0, D1, OutEdgeCrossings[EIdx]);
						OutValidEdgeIndices.Add(EIdx);
					}
				}
			}
		}
	}
}

void FVoxelCPUDualContourMesher::ComputeEdgeCrossing(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 VX, int32 VY, int32 VZ,
	int32 Axis,
	float D0, float D1,
	FDCEdgeCrossing& Crossing) const
{
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	auto DensityFn = [&](int32 X, int32 Y, int32 Z) { return GetDensityAt(Request, X, Y, Z); };
	int32 NX = VX, NY = VY, NZ = VZ;
	if (Axis == 0) NX += Stride;
	else if (Axis == 1) NY += Stride;
	else NZ += Stride;
	const bool bSolid0 = (D0 >= IsoLevel);

	float t = (IsoLevel - D0) / (D1 - D0);
	t = FMath::Clamp(t, 0.0f, 1.0f);

	const FVector3f P0(static_cast<float>(VX) * VoxelSize,
		static_cast<float>(VY) * VoxelSize,
		static_cast<float>(VZ) * VoxelSize);
	const FVector3f P1(static_cast<float>(NX) * VoxelSize,
		static_cast<float>(NY) * VoxelSize,
		static_cast<float>(NZ) * VoxelSize);

	// At a stride, LOD 0's crossing on the edge and the fitted gradient's normal there (G62's shading
	// normal: exact for a plane, so the QEF's vertex lies on it; the central difference tilts with the
	// voxel phase, and a strided cell spreads that tilt over a stride; G80).
	Crossing.Position = (Stride > 1)
		? VoxelLODCrossing::FindVoxelCrossing(DensityFn, VX, VY, VZ, Axis, Stride, IsoLevel) * VoxelSize
		: P0 + (P1 - P0) * t;
	Crossing.bValid = true;
	// The solid endpoint's material: the cells around the edge vote with it (soft borders, VP2-O4)
	Crossing.bSolidAtStart = bSolid0;
	Crossing.SolidMaterial = bSolid0
		? GetVoxelAt(Request, VX, VY, VZ).MaterialID
		: GetVoxelAt(Request, NX, NY, NZ).MaterialID;

	const float CrossVoxelX = Crossing.Position.X / VoxelSize;
	const float CrossVoxelY = Crossing.Position.Y / VoxelSize;
	const float CrossVoxelZ = Crossing.Position.Z / VoxelSize;
	// LOD 0's vertex normal sums its crossings' shading normals (G62).
	if (Stride == 1)
	{
		Crossing.ShadingNormal = CalculateShadingNormal(Request, CrossVoxelX, CrossVoxelY, CrossVoxelZ);
	}
	// The QEF's plane normal: the fitted gradient (G80 at a stride, G81 at stride 1).
	Crossing.Normal = (Stride == 1)
		? Crossing.ShadingNormal : CalculateShadingNormal(Request, CrossVoxelX, CrossVoxelY, CrossVoxelZ);
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

	// 12 edges of a cube cell: 4 along each axis (the seams' ring recompute walks the same table)
	const auto& CellEdges = VoxelDCTables::CellEdges;

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
	// Interior domain: solve only the interior cells [0, GridSize-1) —
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
				const FDCEdgeCrossing* Crossings[12];
				for (int32 Index = 0; Index < 12; ++Index)
				{
					const auto& Edge = CellEdges[Index];
					const FDCEdgeCrossing& Crossing = EdgeCrossings[EdgeIndex(CX + Edge.DX, CY + Edge.DY, CZ + Edge.DZ, Edge.Axis, GridDim)];
					Crossings[Index] = Crossing.bValid ? &Crossing : nullptr;
				}
				SolveCellVertex(Request, Stride, CX, CY, CZ, Crossings, OutCellVertices[CellIndex(CX, CY, CZ, GridDim)]);
			}
		}
	}
}

bool FVoxelCPUDualContourMesher::SolveCellVertex(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 CX, int32 CY, int32 CZ,
	const FDCEdgeCrossing* const Crossings[12],
	FDCCellVertex& Vertex) const
{
	const float VoxelSize = Request.VoxelSize;
	const float CellWorldSize = static_cast<float>(Stride) * VoxelSize;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;
	// At a stride the crossings are LOD 0's with the fitted gradient's normals (G80), and the vertex normal sums them as
	// LOD 0's does (AvgNormal: the cell's own crossings). LOD 0's shading normals are the fitted gradient at each crossing
	// (G62), which G81 gives the QEF too.
	const bool bCrossingShading = Stride == 1;
	const auto& CellEdges = VoxelDCTables::CellEdges;

	FQEFSolver QEF;
	FVector3f AvgNormal = FVector3f::ZeroVector;
	FVector3f ShadingSum = FVector3f::ZeroVector;
	VoxelDCMaterialBlend::FCellMaterialVote MaterialVote;

	for (int32 Index = 0; Index < 12; ++Index)
	{
		if (const FDCEdgeCrossing* CrossingPtr = Crossings[Index])
		{
			const auto& Edge = CellEdges[Index];
			const FDCEdgeCrossing& Crossing = *CrossingPtr;
			const int32 EndDX = Edge.DX + ((Edge.Axis == 0 && !Crossing.bSolidAtStart) ? 1 : 0);
			const int32 EndDY = Edge.DY + ((Edge.Axis == 1 && !Crossing.bSolidAtStart) ? 1 : 0);
			const int32 EndDZ = Edge.DZ + ((Edge.Axis == 2 && !Crossing.bSolidAtStart) ? 1 : 0);
			MaterialVote.Add(VoxelDCMaterialBlend::CornerIndex(EndDX, EndDY, EndDZ), Crossing.SolidMaterial);
			QEF.Add(Crossing.Position, Crossing.Normal);
			AvgNormal += Crossing.Normal;
			if (bCrossingShading)
			{
				ShadingSum += Crossing.ShadingNormal;
			}
		}
	}

	if (QEF.Count == 0)
	{
		return false;
	}

	const float MinX = static_cast<float>(CX * Stride) * VoxelSize;
	const float MinY = static_cast<float>(CY * Stride) * VoxelSize;
	const float MinZ = static_cast<float>(CZ * Stride) * VoxelSize;
	const FBox3f CellBounds(
		FVector3f(MinX, MinY, MinZ),
		FVector3f(MinX + CellWorldSize, MinY + CellWorldSize, MinZ + CellWorldSize)
	);

	Vertex.bValid = true;
	Vertex.MeshVertexIndex = -1;  // A zeroed vertex has 0 here; EmitVertex needs -1 to know it is unemitted
	Vertex.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength);

	if (!AvgNormal.Normalize())
	{
		AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
	}
	Vertex.Normal = (bCrossingShading && ShadingSum.Normalize()) ? ShadingSum : AvgNormal;
	// Material/biome are assigned per-quad in GenerateQuads (from the owned
	// edge's solid endpoint) so triangles stay material-uniform; the cell's own
	// material decides the quad's blend toward a neighbouring material (VP2-O4).
	Vertex.CellMaterial = MaterialVote.Result();
	return true;
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

	// Duplicate-emission cache for cell vertices shared by quads of different
	// materials: key = (cell index << 16) | (material << 8) | biome.
	// Only material-border cells ever land here, so the map stays small.
	TMap<uint64, int32> DuplicateVertexCache;

	// Interior domain: quads may only reference interior cells
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

		if (!IsQuadEdge(CX, CY, CZ, Axis, GridSize, bInteriorDomain))
		{
			continue;
		}
		const FDCCellOffset* Offsets = QuadCellOffsets[Axis];

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

		EmitEdgeQuad(Request, Stride, CX, CY, CZ, Axis, Verts, CellIdxs, DuplicateVertexCache, OutMeshData, OutTriangleCount);
	}
}

// Precomputed 4-cell offsets per axis (winding-correct)
const FVoxelCPUDualContourMesher::FDCCellOffset FVoxelCPUDualContourMesher::QuadCellOffsets[3][4] = {
	// Axis 0 (X-edge): vary Y,Z
	{{0, 0, 0}, {0, -1, 0}, {0, -1, -1}, {0, 0, -1}},
	// Axis 1 (Y-edge): vary Z,X (Z×X = +Y)
	{{0, 0, 0}, {0, 0, -1}, {-1, 0, -1}, {-1, 0, 0}},
	// Axis 2 (Z-edge): vary X,Y
	{{0, 0, 0}, {-1, 0, 0}, {-1, -1, 0}, {0, -1, 0}},
};

bool FVoxelCPUDualContourMesher::IsQuadEdge(int32 CX, int32 CY, int32 CZ, int32 Axis, int32 GridSize, bool bInteriorDomain)
{
	// Edge ownership: the edge position itself must be in [0, GridSize).
	// This ensures each physical edge is owned by exactly one chunk.
	// Cell offsets (containing 0 and -1) access cells in [-1, GridSize-1],
	// which SolveCellVertices already computes.
	const bool bOwned = (CX >= 0 && CX < GridSize && CY >= 0 && CY < GridSize && CZ >= 0 && CZ < GridSize);
	if (!bOwned)
	{
		return false;
	}

	// Interior domain: skip any quad whose 4 cells are not all interior. (The surrounding
	// cells sit at offsets 0/-1 from the edge, so this bounds-checks each against
	// [0, GridSize-1) — boundary-layer cells were not solved in this domain.)
	if (bInteriorDomain)
	{
		const FDCCellOffset* Offsets = QuadCellOffsets[Axis];
		for (int32 i = 0; i < 4; i++)
		{
			const int32 ACX = CX + Offsets[i].DX;
			const int32 ACY = CY + Offsets[i].DY;
			const int32 ACZ = CZ + Offsets[i].DZ;
			if (ACX < 0 || ACX >= GridSize - 1 ||
				ACY < 0 || ACY >= GridSize - 1 ||
				ACZ < 0 || ACZ >= GridSize - 1)
			{
				return false;
			}
		}
	}
	return true;
}

void FVoxelCPUDualContourMesher::EmitEdgeQuad(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 CX, int32 CY, int32 CZ,
	int32 Axis,
	FDCCellVertex* Verts[4],
	const int32 CellIdxs[4],
	TMap<uint64, int32>& DuplicateVertexCache,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount) const
{
	const float IsoLevel = Config.IsoLevel;

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

	// Emit vertices with the quad's material and its blend toward a neighbouring cell's material (VP2-O4). A cell
	// vertex already emitted with a different material or blend is duplicated (same position/normal) via the cache.
	int32 Indices[4];
	VoxelDCMaterialBlend::EmitQuadCells(Verts, CellIdxs, 4, QuadMaterial, QuadBiome, DuplicateVertexCache,
		[&](const FDCCellVertex& EmitCell, uint8 EmitMaterial, uint8 EmitBiome, uint32 EmitBlend)
		{
			return EmitVertex(Request, EmitCell, EmitMaterial, EmitBiome, EmitBlend, OutMeshData);
		},
		Indices);

	// Split the quad along the diagonal that maximises the smaller of the two triangle
	// areas (a max-min-area / Delaunay-like choice). A fixed 0-2 diagonal can produce a
	// near-zero-area sliver on quads whose four vertices approach collinearity (the
	// renderer drops such slivers, punching pinholes); choosing the better diagonal is
	// cheap, leaves well-shaped quads untouched, and improves shading on skewed quads.
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

// ============================================================================
// The same passes over the surface's cells only (G275)
// ============================================================================

void FVoxelCPUDualContourMesher::DetectCornerSigns(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 GridDim,
	FDCSurfaceGrid& Grid) const
{
	const int32 GridSize = Request.ChunkSize / Stride;
	const float IsoLevel = Config.IsoLevel;
	const int32 TotalCells = GridDim * GridDim * GridDim;
	Grid.CornerSolid.SetNumZeroed(TotalCells);
	// Written before they are read: an edge's slot when its crossing is made, read only where the two corner signs differ.
	Grid.EdgeSlots.SetNumUninitialized(TotalCells * 3);
	Grid.CellSlots.Init(INDEX_NONE, TotalCells);
	Grid.Crossings.Reserve(GridSize * GridSize * 6);
	Grid.Vertices.Reserve(GridSize * GridSize * 3);

	// Every corner the dense pass reads: an edge starts at a cell in [-1, GridSize] and ends one step along one axis, so
	// a corner has at most one coordinate at GridSize + 1.
	uint8* Solid = Grid.CornerSolid.GetData();
	for (int32 CZ = -1; CZ <= GridSize + 1; CZ++)
	{
		for (int32 CY = -1; CY <= GridSize + 1; CY++)
		{
			const int32 OuterYZ = (CY > GridSize ? 1 : 0) + (CZ > GridSize ? 1 : 0);
			if (OuterYZ > 1)
			{
				continue;
			}
			const int32 MaxX = (OuterYZ == 0) ? GridSize + 1 : GridSize;
			int32 Index = CellIndex(-1, CY, CZ, GridDim);
			for (int32 CX = -1; CX <= MaxX; CX++, Index++)
			{
				Solid[Index] = (GetDensityAt(Request, CX * Stride, CY * Stride, CZ * Stride) >= IsoLevel) ? 1 : 0;
			}
		}
	}
}

void FVoxelCPUDualContourMesher::DetectSurfaceCrossings(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 GridDim,
	FDCSurfaceGrid& Grid,
	TArray<int32>& OutValidEdgeIndices) const
{
	const int32 GridSize = Request.ChunkSize / Stride;
	const uint8* Solid = Grid.CornerSolid.GetData();
	const int32 CornerStep[3] = { 1, GridDim, GridDim * GridDim };

	// DetectEdgeCrossings' order: the valid edges, and so the quads, come out as they did.
	for (int32 CZ = -1; CZ <= GridSize; CZ++)
	{
		for (int32 CY = -1; CY <= GridSize; CY++)
		{
			int32 Corner = CellIndex(-1, CY, CZ, GridDim);
			for (int32 CX = -1; CX <= GridSize; CX++, Corner++)
			{
				const uint8 Solid0 = Solid[Corner];
				if (Solid0 == Solid[Corner + CornerStep[0]] && Solid0 == Solid[Corner + CornerStep[1]] && Solid0 == Solid[Corner + CornerStep[2]])
				{
					continue;
				}
				const int32 VX = CX * Stride;
				const int32 VY = CY * Stride;
				const int32 VZ = CZ * Stride;
				const float D0 = GetDensityAt(Request, VX, VY, VZ);
				for (int32 Axis = 0; Axis < 3; Axis++)
				{
					if (Solid0 == Solid[Corner + CornerStep[Axis]])
					{
						continue;
					}
					const float D1 = GetDensityAt(Request, VX + (Axis == 0 ? Stride : 0), VY + (Axis == 1 ? Stride : 0), VZ + (Axis == 2 ? Stride : 0));
					const int32 EIdx = Corner * 3 + Axis;
					Grid.EdgeSlots[EIdx] = Grid.Crossings.AddDefaulted();
					ComputeEdgeCrossing(Request, Stride, VX, VY, VZ, Axis, D0, D1, Grid.Crossings.Last());
					OutValidEdgeIndices.Add(EIdx);
				}
			}
		}
	}
}

void FVoxelCPUDualContourMesher::SolveSurfaceVertices(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 GridDim,
	FDCSurfaceGrid& Grid) const
{
	const int32 GridSize = Request.ChunkSize / Stride;
	const auto& CellEdges = VoxelDCTables::CellEdges;
	const uint8* Solid = Grid.CornerSolid.GetData();
	const int32 StepY = GridDim;
	const int32 StepZ = GridDim * GridDim;

	// SolveCellVertices' domain (see there): the interior cells alone in the Interior domain.
	const bool bInteriorDomain = (Request.MeshCellDomain == EVoxelMeshCellDomain::Interior);
	const int32 CellMin = bInteriorDomain ? 0 : -1;
	const int32 CellMaxEx = bInteriorDomain ? (GridSize - 1) : GridSize;
	for (int32 CZ = CellMin; CZ < CellMaxEx; CZ++)
	{
		for (int32 CY = CellMin; CY < CellMaxEx; CY++)
		{
			int32 Corner = CellIndex(CellMin, CY, CZ, GridDim);
			for (int32 CX = CellMin; CX < CellMaxEx; CX++, Corner++)
			{
				// A cell whose eight corners agree has no crossing on any of its twelve edges: nothing to solve.
				const int32 Sum = Solid[Corner] + Solid[Corner + 1] + Solid[Corner + StepY] + Solid[Corner + StepY + 1]
					+ Solid[Corner + StepZ] + Solid[Corner + StepZ + 1] + Solid[Corner + StepZ + StepY] + Solid[Corner + StepZ + StepY + 1];
				if (Sum == 0 || Sum == 8)
				{
					continue;
				}

				const FDCEdgeCrossing* Crossings[12];
				for (int32 Index = 0; Index < 12; ++Index)
				{
					const auto& Edge = CellEdges[Index];
					const int32 Start = Corner + Edge.DX + Edge.DY * StepY + Edge.DZ * StepZ;
					const int32 End = Start + (Edge.Axis == 0 ? 1 : (Edge.Axis == 1 ? StepY : StepZ));
					Crossings[Index] = (Solid[Start] != Solid[End]) ? &Grid.Crossings[Grid.EdgeSlots[Start * 3 + Edge.Axis]] : nullptr;
				}
				FDCCellVertex Vertex;
				if (SolveCellVertex(Request, Stride, CX, CY, CZ, Crossings, Vertex))
				{
					Grid.CellSlots[Corner] = Grid.Vertices.Add(Vertex);
				}
			}
		}
	}
}

void FVoxelCPUDualContourMesher::GenerateSurfaceQuads(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	int32 GridDim,
	FDCSurfaceGrid& Grid,
	const TArray<int32>& ValidEdgeIndices,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount) const
{
	const int32 GridSize = Request.ChunkSize / Stride;
	const bool bInteriorDomain = (Request.MeshCellDomain == EVoxelMeshCellDomain::Interior);
	// GenerateQuads' cache, keyed by the cell's place in the grid as there.
	TMap<uint64, int32> DuplicateVertexCache;

	for (const int32 EIdx : ValidEdgeIndices)
	{
		const int32 Axis = EIdx % 3;
		const int32 CellLinear = EIdx / 3;
		const int32 CX = (CellLinear % GridDim) - 1;
		const int32 CY = ((CellLinear / GridDim) % GridDim) - 1;
		const int32 CZ = (CellLinear / (GridDim * GridDim)) - 1;
		if (!IsQuadEdge(CX, CY, CZ, Axis, GridSize, bInteriorDomain))
		{
			continue;
		}
		const FDCCellOffset* Offsets = QuadCellOffsets[Axis];

		FDCCellVertex* Verts[4];
		int32 CellIdxs[4];
		bool bAllValid = true;
		for (int32 i = 0; i < 4; i++)
		{
			const int32 CIdx = CellIndex(CX + Offsets[i].DX, CY + Offsets[i].DY, CZ + Offsets[i].DZ, GridDim);
			const int32 Slot = Grid.CellSlots[CIdx];
			if (Slot == INDEX_NONE)
			{
				bAllValid = false;
				break;
			}
			Verts[i] = &Grid.Vertices[Slot];
			CellIdxs[i] = CIdx;
		}
		if (!bAllValid)
		{
			continue;
		}

		EmitEdgeQuad(Request, Stride, CX, CY, CZ, Axis, Verts, CellIdxs, DuplicateVertexCache, OutMeshData, OutTriangleCount);
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
	uint32 BlendCode,
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
	OutMeshData.MaterialBlends.Add(BlendCode);

	return Index;
}

// ============================================================================
// Density & Voxel Access: the shared sampler under Dual Contouring's rule (VoxelMeshingVoxelSampler.h, MESH-09)
// ============================================================================

float FVoxelCPUDualContourMesher::GetDensityAt(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	return VoxelMeshingSampler::DensityAt(Request, X, Y, Z, VoxelMeshingSampler::DualContouringRule);
}

FVoxelData FVoxelCPUDualContourMesher::GetVoxelAt(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z) const
{
	return VoxelMeshingSampler::VoxelAt(Request, X, Y, Z, VoxelMeshingSampler::DualContouringRule);
}

FVector3f FVoxelCPUDualContourMesher::CalculateGradientNormal(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z) const
{
	return CalculateGradientNormalLOD(Request, X, Y, Z, 1);
}

FVector3f FVoxelCPUDualContourMesher::CalculateGradientNormalLOD(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z,
	int32 Stride) const
{
	const int32 IX = FMath::FloorToInt(X);
	const int32 IY = FMath::FloorToInt(Y);
	const int32 IZ = FMath::FloorToInt(Z);

	// Interior domain (seam-ownership): gradient taps that would exit the chunk CLAMP to the
	// nearest edge voxel ("the terrain continues") instead of reading the absent-neighbour Air
	// fallback ("the terrain cliffs"). The Air tilt biased every rim crossing's normal toward
	// the boundary, shading each chunk's rim ring differently — a per-chunk checkerboard at
	// world scale. Clamping keeps the zero-neighbour-dependence property (pure function of own
	// content); the seam samplers apply the same rule so ring recomputes still match bit-exactly.
	// WholeChunk-domain behaviour is unchanged.
	if (Request.MeshCellDomain == EVoxelMeshCellDomain::Interior)
	{
		const int32 Max = Request.ChunkSize - 1;
		const int32 Lo[3] = { 0, 0, 0 };
		const int32 Hi[3] = { Max, Max, Max };
		return VoxelDCGradient::ClampedGradientNormal(
			[&](int32 TX, int32 TY, int32 TZ) { return GetDensityAt(Request, TX, TY, TZ); }, Lo, Hi, IX, IY, IZ, Stride);
	}

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

FVector3f FVoxelCPUDualContourMesher::CalculateShadingNormal(
	const FVoxelMeshingRequest& Request,
	float X, float Y, float Z) const
{
	// The voxels this view holds: the chunk's own in the Interior domain (CalculateGradientNormalLOD's clamp box, and the
	// seam samplers' per-side box, so ring recomputes match); one voxel past each face in the WholeChunk domain (a missing
	// neighbour reads as Air, which the fit skips).
	const int32 CS = Request.ChunkSize;
	const bool bInterior = (Request.MeshCellDomain == EVoxelMeshCellDomain::Interior);
	const int32 Lo = bInterior ? 0 : -1;
	const int32 Hi = bInterior ? (CS - 1) : CS;
	const VoxelCrossingGradient::FBoxHeld IsHeld{ { Lo, Lo, Lo }, { Hi, Hi, Hi } };
	return VoxelCrossingGradient::ShadingNormal(
		[&](int32 VX, int32 VY, int32 VZ) { return GetDensityAt(Request, VX, VY, VZ); }, IsHeld, X, Y, Z,
		[&]() { return CalculateGradientNormal(Request, X, Y, Z); });
}

// ============================================================================

void FVoxelCPUDualContourMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelCPUDualContourMesher::GetConfig() const
{
	return Config;
}
