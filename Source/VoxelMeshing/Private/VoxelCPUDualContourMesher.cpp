// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUDualContourMesher.h"
#include "VoxelCPUDualContourShared.h"
#include "VoxelMeshing.h"
#include "QEFSolver.h"
#include "VoxelCrossingGradient.h"
#include "VoxelMeshingVoxelSampler.h"
#include "HAL/IConsoleManager.h"

// Shared with the Marching Cubes mesher (VoxelCPUMarchingCubesMesher.cpp) through VoxelCrossingGradient.h (G62).
TAutoConsoleVariable<int32> CVarVoxelCrossingGradientNormals(
	TEXT("voxel.Mesh.CrossingGradientNormals"),
	1,
	TEXT("Smooth meshing's shading normals at voxel resolution (G62): 1 = the density gradient fitted to the voxels on the ")
	TEXT("density ramp around the point (default); 0 = the central difference about the voxel the point floors to, as before ")
	TEXT("(the voxel under a surface lies in the saturated rock, so a slope tilted by 1.0-1.8 times by where the surface ")
	TEXT("crosses the voxel layer: bands a layer apart, and faint rings around hills at LOD 1 and 2). Both meshers' vertex ")
	TEXT("normals at every LOD; Dual Contouring's QEF keeps the central difference, so no vertex moves. Set it before chunks ")
	TEXT("mesh: interior meshes and seam jobs read it per job."),
	ECVF_Default);

// Shared with the Marching Cubes mesher (VoxelCPUMarchingCubesMesher.cpp: G61).
TAutoConsoleVariable<int32> CVarVoxelShadingNormals(
	TEXT("voxel.Mesh.VoxelShadingNormals"),
	1,
	TEXT("Smooth meshing's vertex normals at LOD > 0: 1 = LOD 0's normals at voxel resolution (default): Dual Contouring ")
	TEXT("(G44) takes the normal of the voxel cell where the surface crosses the cell's edges nearest the vertex, Marching ")
	TEXT("Cubes (G61) the gradient at the voxel crossing on the vertex's edge nearest it; 0 = the gradients at the cell's ")
	TEXT("stride, as before (their taps read the saturated density, so LOD 2 leans toward the cube diagonals: a brighter ")
	TEXT("ring on planets). Vertex positions are the same either way. Set it before chunks mesh: interior meshes and seam ")
	TEXT("jobs read it per job."),
	ECVF_Default);

// Shared with the Marching Cubes mesher (VoxelCPUMarchingCubesMesher.cpp: G80).
TAutoConsoleVariable<int32> CVarVoxelLODCrossings(
	TEXT("voxel.Mesh.VoxelLODCrossings"),
	1,
	TEXT("Smooth meshing's edge crossings at LOD > 0 (G80): 1 = where the surface crosses the edge at voxel resolution ")
	TEXT("(default): Dual Contouring the first voxel step along the strided edge that changes sign, with the fitted gradient as ")
	TEXT("its QEF normal there (G62), Marching Cubes the voxel crossing on the edge nearest the strided interpolation (G61), its ")
	TEXT("LOD-seam geomorph toward the coarse cells' crossings; 0 = interpolated between the samples a stride apart, as before ")
	TEXT("(generated density saturates a voxel either side of the surface, so the crossing snaps toward the edge's middle by ")
	TEXT("where the surface crosses the strided grid: terraces a stride apart at LOD 1 and 2, ribs in their shadows under a low ")
	TEXT("sun). Interior meshes and seam jobs alike; set it before chunks mesh (voxel.Mesh.RemeshAll after a change)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarDCQEFSolve(
	TEXT("voxel.Mesh.QEFSolve"),
	1,
	TEXT("Dual Contouring's QEF vertex solve (G81): 1 = for the offset from the cell's mass point, with a Jacobi decomposition ")
	TEXT("that keeps its matrix symmetric (default); 0 = as before: for the position itself, so a direction the SVD threshold ")
	TEXT("drops went to the chunk's origin (a flat cell's vertex was the plane's point nearest the chunk's corner, pulled ")
	TEXT("back toward the mass point only part of the way when it lay within a cell of the cell), and each rotation zeroed ")
	TEXT("its pivot above the diagonal only (wrong eigenpairs: a vertex a voxel or more off the surface where a cell's normals ")
	TEXT("were nearly parallel, more at LOD 1 and 2). Interior meshes and seam jobs alike; set it before chunks mesh ")
	TEXT("(voxel.Mesh.RemeshAll after a change)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarDCScaleClampedGradients(
	TEXT("voxel.Mesh.ScaleClampedGradients"),
	1,
	TEXT("Dual Contouring gradient normals where a tap is clamped at the edge of a mesh job's voxels (chunk faces, G25): ")
	TEXT("1 = scale the clamped axis's one-sided difference to the full span (default); 0 = leave it at up to half weight, ")
	TEXT("as before (lines of shading along chunk faces). Set it before chunks mesh."),
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

	// Pass 3: Generate quads
	GenerateQuads(Request, Stride, GridDim, EdgeCrossings, ValidEdgeIndices, CellVertices, OutMeshData, TriangleCount);

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
	const bool bVoxelCrossings = VoxelCrossingGradient::UseVoxelLODCrossings(Stride);
	const bool bFittedQEFNormals = VoxelDCShading::UseFittedQEFNormals(Stride);
	auto DensityFn = [&](int32 X, int32 Y, int32 Z) { return GetDensityAt(Request, X, Y, Z); };
	static const int32 NoEdgeOffset[3] = { 0, 0, 0 };

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
						// At a stride, LOD 0's crossing on the edge and the fitted gradient's normal there (G62's shading
						// normal: exact for a plane, so the QEF's vertex lies on it; the central difference tilts with the
						// voxel phase, and a strided cell spreads that tilt over a stride; G80).
						Crossing.Position = bVoxelCrossings
							? VoxelLODCrossing::FindVoxelCrossing(DensityFn, VX, VY, VZ, Axis, Stride, IsoLevel, NoEdgeOffset).VoxelPosition * VoxelSize
							: P0 + (P1 - P0) * t;
						Crossing.bValid = true;

						const float CrossVoxelX = Crossing.Position.X / VoxelSize;
						const float CrossVoxelY = Crossing.Position.Y / VoxelSize;
						const float CrossVoxelZ = Crossing.Position.Z / VoxelSize;
						// LOD 0's vertex normal sums its crossings' shading normals (G62).
						if (Stride == 1 && VoxelCrossingGradient::UseCrossingGradientNormals())
						{
							Crossing.ShadingNormal = CalculateShadingNormal(Request, CrossVoxelX, CrossVoxelY, CrossVoxelZ);
						}
						// The QEF's plane normal: the fitted gradient with G80 (a stride) and G81 (stride 1), else the central
						// difference at the crossing's stride.
						Crossing.Normal = bFittedQEFNormals
							? ((Stride == 1) ? Crossing.ShadingNormal : CalculateShadingNormal(Request, CrossVoxelX, CrossVoxelY, CrossVoxelZ))
							: (Stride > 1)
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
	const float IsoLevel = Config.IsoLevel;
	// At a stride with G80 the crossings are LOD 0's with the fitted gradient's normals, and the vertex normal sums them as
	// LOD 0's does (AvgNormal: the cell's own crossings, nearer the analytic normals than G44's one voxel cell); without
	// G80 it is LOD 0's at the voxel crossing nearest the vertex (G44). LOD 0's shading normals are the fitted gradient at
	// each crossing (G62), which G81 gives the QEF too.
	const bool bVoxelShading = VoxelCrossingGradient::UseVoxelShadingNormals(Stride) && !VoxelCrossingGradient::UseVoxelLODCrossings(Stride);
	const bool bCrossingShading = Stride == 1 && VoxelCrossingGradient::UseCrossingGradientNormals();
	auto DensityFn = [&](int32 X, int32 Y, int32 Z) { return GetDensityAt(Request, X, Y, Z); };
	auto GradientFn = [&](float X, float Y, float Z) { return CalculateShadingNormal(Request, X, Y, Z); };

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
				FQEFSolver QEF;
				FVector3f AvgNormal = FVector3f::ZeroVector;
				FVector3f ShadingSum = FVector3f::ZeroVector;
				VoxelDCShading::FCellVoxelCrossings VoxelCrossings;

				for (const auto& Edge : CellEdges)
				{
					const int32 EIdx = EdgeIndex(CX + Edge.DX, CY + Edge.DY, CZ + Edge.DZ, Edge.Axis, GridDim);
					const FDCEdgeCrossing& Crossing = EdgeCrossings[EIdx];
					if (Crossing.bValid)
					{
						QEF.Add(Crossing.Position, Crossing.Normal);
						AvgNormal += Crossing.Normal;
						if (bCrossingShading)
						{
							ShadingSum += Crossing.ShadingNormal;
						}
						if (bVoxelShading)
						{
							const int32 EdgeOffset[3] = { Edge.DX, Edge.DY, Edge.DZ };
							VoxelCrossings.Add(VoxelLODCrossing::FindVoxelCrossing(DensityFn,
								(CX + Edge.DX) * Stride, (CY + Edge.DY) * Stride, (CZ + Edge.DZ) * Stride, Edge.Axis, Stride, IsoLevel, EdgeOffset));
						}
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
				Vertex.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength, CVarDCQEFSolve.GetValueOnAnyThread() != 0);

				if (!AvgNormal.Normalize())
				{
					AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
				}
				if (bCrossingShading && ShadingSum.Normalize())
				{
					Vertex.Normal = ShadingSum;
				}
				else
				{
					Vertex.Normal = bVoxelShading
						? VoxelCrossings.VertexNormal(Vertex.Position, VoxelSize, DensityFn, GradientFn, IsoLevel, AvgNormal) : AvgNormal;
				}
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
