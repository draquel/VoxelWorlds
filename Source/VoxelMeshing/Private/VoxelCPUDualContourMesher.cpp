// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUDualContourMesher.h"
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
	TEXT("sun). Interior meshes and seam jobs alike; set it before chunks mesh (voxel.RemeshAll after a change)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDCQEFSolve(
	TEXT("voxel.Mesh.QEFSolve"),
	1,
	TEXT("Dual Contouring's QEF vertex solve (G81): 1 = for the offset from the cell's mass point, with a Jacobi decomposition ")
	TEXT("that keeps its matrix symmetric (default); 0 = as before: for the position itself, so a direction the SVD threshold ")
	TEXT("drops went to the chunk's origin (a flat cell's vertex was the plane's point nearest the chunk's corner, pulled ")
	TEXT("back toward the mass point only part of the way when it lay within a cell of the cell), and each rotation zeroed ")
	TEXT("its pivot above the diagonal only (wrong eigenpairs: a vertex a voxel or more off the surface where a cell's normals ")
	TEXT("were nearly parallel, more at LOD 1 and 2). Interior meshes and seam jobs alike; set it before chunks mesh ")
	TEXT("(voxel.RemeshAll after a change)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDCScaleClampedGradients(
	TEXT("voxel.Mesh.ScaleClampedGradients"),
	1,
	TEXT("Dual Contouring gradient normals where a tap is clamped at the edge of a mesh job's voxels (chunk faces, G25): ")
	TEXT("1 = scale the clamped axis's one-sided difference to the full span (default); 0 = leave it at up to half weight, ")
	TEXT("as before (lines of shading along chunk faces). Set it before chunks mesh."),
	ECVF_Default);

namespace VoxelDCGradient
{
	/**
	 * The central-difference gradient normal (taps Stride voxels either side) with every tap clamped into the box [Lo, Hi]:
	 * "the terrain continues" past the data a view holds, so no mesh depends on a neighbour's voxels (seam ownership).
	 *
	 * A clamped tap shortens its axis's difference (one-sided at a chunk face), so that component is scaled back to the full
	 * span. Unscaled, the axis a chunk face cut lost up to half its weight and the normals of the cells along the face leaned
	 * away from it: lines of shading along chunk faces wherever the surface crosses them obliquely, darker or brighter by the
	 * face's axis against the sun (G25; on a planet at stride 4, after G44, the Z faces' strips). Where no tap clamps the
	 * result is the plain central difference, bit for bit.
	 *
	 * @param Density (X, Y, Z) -> density at a voxel inside the box
	 * @param Lo, Hi The box (inclusive voxel coordinates per axis)
	 * @param IX, IY, IZ The voxel the gradient is taken at
	 * @param Stride The taps' distance (voxels)
	 * @return The outward normal (minus the gradient), +Z where it vanishes
	 */
	template <typename TDensityFn>
	FVector3f ClampedGradientNormal(const TDensityFn& Density, const int32 Lo[3], const int32 Hi[3],
		int32 IX, int32 IY, int32 IZ, int32 Stride)
	{
		const int32 I[3] = { IX, IY, IZ };
		float G[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			int32 Plus[3];
			int32 Minus[3];
			for (int32 A = 0; A < 3; ++A)
			{
				const int32 Offset = (A == Axis) ? Stride : 0;
				Plus[A] = FMath::Clamp(I[A] + Offset, Lo[A], Hi[A]);
				Minus[A] = FMath::Clamp(I[A] - Offset, Lo[A], Hi[A]);
			}
			G[Axis] = Density(Plus[0], Plus[1], Plus[2]) - Density(Minus[0], Minus[1], Minus[2]);
			const int32 Span = Plus[Axis] - Minus[Axis];
			if (Span > 0 && Span < 2 * Stride && CVarDCScaleClampedGradients.GetValueOnAnyThread() != 0)
			{
				G[Axis] *= static_cast<float>(2 * Stride) / static_cast<float>(Span);
			}
		}
		FVector3f Normal(-G[0], -G[1], -G[2]);
		if (!Normal.Normalize())
		{
			return FVector3f(0.0f, 0.0f, 1.0f);
		}
		return Normal;
	}

	/**
	 * The shading normal at a point of the surface at voxel resolution (G62): the density gradient fitted to the voxels
	 * of the box [Lo, Hi] around it (VoxelCrossingGradient::FitNormal; voxels outside the box are skipped, not clamped),
	 * or, where the fit is ill-conditioned or voxel.Mesh.CrossingGradientNormals is 0, ClampedGradientNormal at stride 1
	 * about the voxel the point floors to, as before. Every view (the interior pass, each seam sampler) passes its own
	 * density and box, so a ring recompute runs the interior pass's op sequence.
	 *
	 * @param Density (X, Y, Z) -> density at a voxel inside the box
	 * @param Lo, Hi The box (inclusive voxel coordinates per axis)
	 * @param X, Y, Z The point (voxels)
	 * @return The outward unit normal
	 */
	template <typename TDensityFn>
	FVector3f CrossingShadingNormal(const TDensityFn& Density, const int32 Lo[3], const int32 Hi[3], float X, float Y, float Z)
	{
		const VoxelCrossingGradient::FBoxHeld IsHeld{ { Lo[0], Lo[1], Lo[2] }, { Hi[0], Hi[1], Hi[2] } };
		return VoxelCrossingGradient::ShadingNormal(Density, IsHeld, X, Y, Z,
			[&]() { return ClampedGradientNormal(Density, Lo, Hi, FMath::FloorToInt(X), FMath::FloorToInt(Y), FMath::FloorToInt(Z), 1); });
	}
}

/**
 * Vertex normals of strided cells (G44, WORLD_MODES_REVIEW_PLAN.md §14.1 and §20.8).
 *
 * Generated density saturates a voxel either side of the surface (SignedDistanceToDensity), so a gradient whose taps lie
 * Stride voxels apart reads 0 or 255 on every axis the surface crosses within them: at stride 4 each component becomes a
 * sign and the normal snaps toward the nearest of the 26 cube directions. On a planet, where local up is diagonal, LOD 2
 * leaned toward the cube diagonals (the ring brighter than the LOD 0/1 centre and the far field); on a heightmap its
 * slopes snapped. Every LOD's data is at voxel resolution, so a strided cell's vertex normal is the normal LOD 0 gives the
 * voxel cell where the surface crosses the strided cell's edges nearest the vertex: LOD 0's estimator with LOD 0's
 * footprint. Averaging voxel-scale normals over the whole strided cell instead smooths them (brighter on rough terrain),
 * and a single gradient sample is noisier than LOD 0's cell average (darker).
 *
 * Edge crossings at LOD > 0 (G80, WORLD_MODES_REVIEW_PLAN.md §23.16, VISUAL_POLISH_PLAN.md VP5): interpolated between the two samples a stride
 * apart, the same saturation put a strided edge's crossing toward the edge's middle by where the surface crosses the
 * strided grid, so LOD 1 and 2 were terraced a stride apart (ribs in the terrain's own shadows under a low sun). With
 * voxel.Mesh.VoxelLODCrossings the crossing is LOD 0's on the first voxel step along the edge that changes sign
 * (VoxelLODCrossing::FindVoxelCrossing; Marching Cubes' rule beside it) and its QEF normal the fitted gradient there
 * (G62's shading normal), so every LOD's vertices lie on LOD 0's surface; LOD 2 still drops the detail its cells cannot hold.
 *
 * Density and Gradient are the caller's views (the mesher's request, or a seam sampler, each with its clamp rule), so the
 * interior pass and the seam rings run one op sequence and stay bit-identical. Every voxel read lies inside the strided
 * cell, so no view reads past the data a cell's own vertex reads. Gradient is LOD 0's shading normal: the fitted gradient
 * of G62 (CalculateShadingNormal, the samplers' ShadingNormal), which reads the voxels within a voxel and a half of the
 * crossing that its view holds.
 */
namespace VoxelDCShading
{
	/**
	 * Whether the QEF's planes take the fitted gradient's normal (G62's shading normal, exact for a plane) at a crossing of
	 * this stride: at a stride with G80's voxel crossings, at stride 1 with G81's solve (which uses the normals: before it,
	 * a flat cell's vertex fell back to its mass point whatever they said) and the fitted gradient on.
	 */
	static bool UseFittedQEFNormals(int32 Stride)
	{
		return (Stride > 1)
			? VoxelCrossingGradient::UseVoxelLODCrossings(Stride)
			: (CVarDCQEFSolve.GetValueOnAnyThread() != 0 && VoxelCrossingGradient::UseCrossingGradientNormals());
	}

	using VoxelLODCrossing::FVoxelCrossing;

	/**
	 * LOD 0's vertex normal of the voxel cell with min corner Cell: LOD 0's gradient normals at its crossings, summed in
	 * the cell's edge order (SolveCellVertices' CellEdges) and normalized, with SolveCellVertices' +Z fallback.
	 */
	template <typename TDensityFn, typename TGradientFn>
	FVector3f VoxelCellNormal(const TDensityFn& Density, const TGradientFn& Gradient, const FIntVector& Cell, float IsoLevel)
	{
		static const int32 Edges[12][4] = {
			{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 2},
			{1, 0, 0, 1}, {1, 0, 0, 2},
			{0, 1, 0, 0}, {0, 1, 0, 2},
			{0, 0, 1, 0}, {0, 0, 1, 1},
			{1, 1, 0, 2}, {1, 0, 1, 1}, {0, 1, 1, 0},
		};
		float Corner[8];
		for (int32 i = 0; i < 8; ++i)
		{
			Corner[i] = Density(Cell.X + (i & 1), Cell.Y + ((i >> 1) & 1), Cell.Z + ((i >> 2) & 1));
		}
		FVector3f Sum = FVector3f::ZeroVector;
		for (const int32* E : Edges)
		{
			const int32 A[3] = { E[0], E[1], E[2] };
			int32 B[3] = { E[0], E[1], E[2] };
			++B[E[3]];
			const float D0 = Corner[A[0] + A[1] * 2 + A[2] * 4];
			const float D1 = Corner[B[0] + B[1] * 2 + B[2] * 4];
			if ((D0 >= IsoLevel) == (D1 >= IsoLevel))
			{
				continue;
			}
			const float T = FMath::Clamp((IsoLevel - D0) / (D1 - D0), 0.0f, 1.0f);
			float C[3] = { static_cast<float>(Cell.X + A[0]), static_cast<float>(Cell.Y + A[1]), static_cast<float>(Cell.Z + A[2]) };
			C[E[3]] += T;
			Sum += Gradient(C[0], C[1], C[2]);
		}
		if (!Sum.Normalize())
		{
			Sum = FVector3f(0.0f, 0.0f, 1.0f);
		}
		return Sum;
	}

	/** A strided cell's voxel crossings in the cell's edge order, and the vertex normal they give. */
	struct FCellVoxelCrossings
	{
		FVoxelCrossing Crossings[12];
		int32 Num = 0;

		void Add(const FVoxelCrossing& Crossing)
		{
			if (Num < 12)
			{
				Crossings[Num++] = Crossing;
			}
		}

		/**
		 * LOD 0's normal of the voxel cell at the crossing nearest the vertex (the first in edge order on a tie).
		 *
		 * @param VertexPosition The solved vertex (cell frame, uu)
		 * @param VoxelSize Voxel size (uu)
		 * @param Density, Gradient, IsoLevel As for VoxelCellNormal
		 * @param Fallback The normal when the cell has no crossing (none reaches here: the QEF needs one)
		 */
		template <typename TDensityFn, typename TGradientFn>
		FVector3f VertexNormal(const FVector3f& VertexPosition, float VoxelSize, const TDensityFn& Density,
			const TGradientFn& Gradient, float IsoLevel, const FVector3f& Fallback) const
		{
			if (Num == 0)
			{
				return Fallback;
			}
			int32 Best = 0;
			float BestDistSq = TNumericLimits<float>::Max();
			for (int32 i = 0; i < Num; ++i)
			{
				const float DistSq = FVector3f::DistSquared(Crossings[i].VoxelPosition * VoxelSize, VertexPosition);
				if (DistSq < BestDistSq)
				{
					BestDistSq = DistSq;
					Best = i;
				}
			}
			return VoxelCellNormal(Density, Gradient, Crossings[Best].VoxelCell, IsoLevel);
		}
	};
}

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
						// G80: at a stride, LOD 0's crossing on the edge and the fitted gradient's normal there (G62's shading
						// normal: exact for a plane, so the QEF's vertex lies on it; the central difference tilts with the
						// voxel phase, and a strided cell spreads that tilt over a stride).
						Crossing.Position = bVoxelCrossings
							? VoxelLODCrossing::FindVoxelCrossing(DensityFn, VX, VY, VZ, Axis, Stride, IsoLevel, NoEdgeOffset).VoxelPosition * VoxelSize
							: P0 + (P1 - P0) * t;
						Crossing.bValid = true;

						const float CrossVoxelX = Crossing.Position.X / VoxelSize;
						const float CrossVoxelY = Crossing.Position.Y / VoxelSize;
						const float CrossVoxelZ = Crossing.Position.Z / VoxelSize;
						// G62: LOD 0's vertex normal sums its crossings' shading normals.
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
	// Full-domain (legacy) behaviour is unchanged.
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
	// seam samplers' per-side box, so ring recomputes match); one voxel past each face in the Full domain (a missing
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
// Face-Seam Meshing (seam-ownership P1 — SEAM_OWNERSHIP_ARCHITECTURE.md §2.2)
// ============================================================================

namespace VoxelDCFaceSeam
{
	/**
	 * What the three seam samplers share (code review CR2, MESH-13): a voxel's density, and the gradient and shading normals
	 * over the sampler's held box. TDerived gives GetVoxel (its view of the participants' voxels, Air outside it) and GetBox
	 * (the box of voxels that view holds, frame-local). One body, so every sampler's ring recompute runs the interior pass's
	 * op sequence.
	 */
	template <typename TDerived>
	struct TSeamSamplerBase
	{
		float GetDensity(int32 X, int32 Y, int32 Z) const
		{
			return static_cast<float>(static_cast<const TDerived*>(this)->GetVoxel(X, Y, Z).Density) / 255.0f;
		}

		/**
		 * Gradient normal with taps CLAMPED into the sampler's valid data box (frame-local), matching the Interior pass's
		 * clamp rule: "the terrain continues" past the data edge instead of the Air cliff that checkerboarded chunk-rim
		 * shading. Every sampler's box is a box by construction (per-axis allowed halves).
		 */
		FVector3f GradientNormal(float X, float Y, float Z, int32 Stride) const
		{
			int32 Lo[3];
			int32 Hi[3];
			static_cast<const TDerived*>(this)->GetBox(Lo, Hi);

			const int32 IX = FMath::FloorToInt(X);
			const int32 IY = FMath::FloorToInt(Y);
			const int32 IZ = FMath::FloorToInt(Z);
			return VoxelDCGradient::ClampedGradientNormal(
				[this](int32 TX, int32 TY, int32 TZ) { return GetDensity(TX, TY, TZ); }, Lo, Hi, IX, IY, IZ, Stride);
		}

		/** The shading normal at a voxel-resolution crossing over the same box: the interior pass's CalculateShadingNormal view (G62). */
		FVector3f ShadingNormal(float X, float Y, float Z) const
		{
			int32 Lo[3];
			int32 Hi[3];
			static_cast<const TDerived*>(this)->GetBox(Lo, Hi);
			return VoxelDCGradient::CrossingShadingNormal(
				[this](int32 TX, int32 TY, int32 TZ) { return GetDensity(TX, TY, TZ); }, Lo, Hi, X, Y, Z);
		}
	};

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
	struct FSeamSampler : TSeamSamplerBase<FSeamSampler>
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

		/** The sampler's valid data box (frame-local): own chunk only, or both chunks along the face axis for Combined. */
		void GetBox(int32 Lo[3], int32 Hi[3]) const
		{
			const int32 CS = Req.ChunkSize;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Lo[Axis] = 0;
				Hi[Axis] = CS - 1;
			}
			if (Mode == EMode::Combined)
			{
				Hi[Req.Axis] = 2 * CS - 1;
			}
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
		/** The mixed meshers tried ComputeBoundaryFallbackVertex on this invalid cell (G17); never retried. */
		bool bFallbackTried = false;
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
	struct FQuadSampler : TSeamSamplerBase<FQuadSampler>
	{
		const FVoxelEdgeSeamRequest& Req;
		int32 UAxis;                  // edge-parallel axis
		int32 PerpA;                  // smaller perpendicular axis
		int32 PerpB;                  // larger perpendicular axis
		uint8 QuadrantMask;           // visible quadrants, bit = dv + dw*2
		FIntVector FrameOffsetVoxels; // frame-local -> owner-frame voxel offset

		FQuadSampler(const FVoxelEdgeSeamRequest& InReq, int32 InUAxis, int32 InPerpA, int32 InPerpB, uint8 InQuadrantMask,
			const FIntVector& InFrameOffsetVoxels)
			: Req(InReq), UAxis(InUAxis), PerpA(InPerpA), PerpB(InPerpB), QuadrantMask(InQuadrantMask), FrameOffsetVoxels(InFrameOffsetVoxels)
		{
		}

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

		/** The mask's valid box (frame-local): masks are boxes by construction (per-axis allowed halves). */
		void GetBox(int32 Lo[3], int32 Hi[3]) const
		{
			const int32 CS = Req.ChunkSize;
			Lo[UAxis] = -FrameOffsetVoxels[UAxis];
			Hi[UAxis] = CS - 1 - FrameOffsetVoxels[UAxis];
			const bool bDv0 = (QuadrantMask & 0b0101) != 0;
			const bool bDv1 = (QuadrantMask & 0b1010) != 0;
			const bool bDw0 = (QuadrantMask & 0b0011) != 0;
			const bool bDw1 = (QuadrantMask & 0b1100) != 0;
			Lo[PerpA] = (bDv0 ? 0 : CS) - FrameOffsetVoxels[PerpA];
			Hi[PerpA] = (bDv1 ? 2 * CS - 1 : CS - 1) - FrameOffsetVoxels[PerpA];
			Lo[PerpB] = (bDw0 ? 0 : CS) - FrameOffsetVoxels[PerpB];
			Hi[PerpB] = (bDw1 ? 2 * CS - 1 : CS - 1) - FrameOffsetVoxels[PerpB];
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
	struct FOctSampler : TSeamSamplerBase<FOctSampler>
	{
		const FVoxelCornerSeamRequest& Req;
		uint8 OctantMask;             // visible octants, bit = dx + dy*2 + dz*4
		FIntVector FrameOffsetVoxels; // frame-local -> owner-frame voxel offset

		FOctSampler(const FVoxelCornerSeamRequest& InReq, uint8 InOctantMask, const FIntVector& InFrameOffsetVoxels)
			: Req(InReq), OctantMask(InOctantMask), FrameOffsetVoxels(InFrameOffsetVoxels)
		{
		}

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

		/** The octant mask's valid box (frame-local): octant masks are boxes by construction (per-axis allowed halves). */
		void GetBox(int32 Lo[3], int32 Hi[3]) const
		{
			const int32 CS = Req.ChunkSize;
			// Per-axis half presence: low/high octant-mask bits per axis (X, Y, Z).
			static const uint8 LowHalf[3] = { 0x55, 0x33, 0x0F };
			static const uint8 HighHalf[3] = { 0xAA, 0xCC, 0xF0 };
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const bool bLow = (OctantMask & LowHalf[Axis]) != 0;
				const bool bHigh = (OctantMask & HighHalf[Axis]) != 0;
				Lo[Axis] = (bLow ? 0 : CS) - FrameOffsetVoxels[Axis];
				Hi[Axis] = (bHigh ? 2 * CS - 1 : CS - 1) - FrameOffsetVoxels[Axis];
			}
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
		// SolveCellVertices' vertex normal at a stride (G80, else G44) and at LOD 0 (G62), op for op.
		const bool bVoxelShading = VoxelCrossingGradient::UseVoxelShadingNormals(Stride) && !VoxelCrossingGradient::UseVoxelLODCrossings(Stride);
		const bool bCrossingShading = Stride == 1 && VoxelCrossingGradient::UseCrossingGradientNormals();
		FVector3f ShadingSum = FVector3f::ZeroVector;
		VoxelDCShading::FCellVoxelCrossings VoxelCrossings;
		auto DensityFn = [&S](int32 X, int32 Y, int32 Z) { return S.GetDensity(X, Y, Z); };
		auto GradientFn = [&S](float X, float Y, float Z) { return S.ShadingNormal(X, Y, Z); };
		// DetectEdgeCrossings' crossing and QEF normal, op for op (G80, G81).
		const bool bVoxelCrossings = VoxelCrossingGradient::UseVoxelLODCrossings(Stride);
		const bool bFittedQEFNormals = VoxelDCShading::UseFittedQEFNormals(Stride);
		static const int32 NoEdgeOffset[3] = { 0, 0, 0 };

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

			const FVector3f CrossPos = bVoxelCrossings
				? VoxelLODCrossing::FindVoxelCrossing(DensityFn, VX, VY, VZ, Edge.Axis, Stride, IsoLevel, NoEdgeOffset).VoxelPosition * VoxelSize
				: P0 + (P1 - P0) * t;
			const FVector3f CrossShading = (bCrossingShading || bFittedQEFNormals)
				? S.ShadingNormal(CrossPos.X / VoxelSize, CrossPos.Y / VoxelSize, CrossPos.Z / VoxelSize) : FVector3f::ZeroVector;
			const FVector3f CrossNormal = bFittedQEFNormals
				? CrossShading
				: S.GradientNormal(CrossPos.X / VoxelSize, CrossPos.Y / VoxelSize, CrossPos.Z / VoxelSize, Stride);

			QEF.Add(CrossPos, CrossNormal);
			AvgNormal += CrossNormal;
			if (bCrossingShading)
			{
				ShadingSum += CrossShading;
			}
			if (bVoxelShading)
			{
				const int32 EdgeOffset[3] = { Edge.DX, Edge.DY, Edge.DZ };
				VoxelCrossings.Add(VoxelLODCrossing::FindVoxelCrossing(DensityFn, VX, VY, VZ, Edge.Axis, Stride, IsoLevel, EdgeOffset));
			}
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
		Out.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength, CVarDCQEFSolve.GetValueOnAnyThread() != 0);

		if (!AvgNormal.Normalize())
		{
			AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
		}
		if (bCrossingShading && ShadingSum.Normalize())
		{
			Out.Normal = ShadingSum;
		}
		else
		{
			Out.Normal = bVoxelShading
				? VoxelCrossings.VertexNormal(Out.Position, VoxelSize, DensityFn, GradientFn, IsoLevel, AvgNormal) : AvgNormal;
		}
		return true;
	}

	/**
	 * The vertex of a coarse cell that has no crossing at its own stride but has a finer one on its boundary
	 * (G17, WORLD_MODES_REVIEW_PLAN.md §14.1). The mixed meshers stitch at the finest stride of their
	 * participants, so a fine crossing on the face plane (or a tuple's shared lines) can face a coarse cell
	 * whose corners all have one sign: the surface pokes through the coarse cell's face between its samples.
	 * That cell has no vertex at its own stride, and skipping the fine fan left a small hole bounded by the
	 * fine side's quads (two to six open edges). Here the cell gets one: the same QEF solve over every
	 * voxel-resolution (stride-1) crossing on the cell's six faces, normals at stride 1, positions in the
	 * coarse cell's bounds (surface-derived, inside its own cell: the #44 laws).
	 *
	 * Only for cells that ComputeCellVertex left invalid, so it never moves a vertex an interior pass or a
	 * same-LOD seam emits (they only emit valid cells). It is a pure function of the sampler view, the cell
	 * and its stride, not of the calling job's stitch stride, so every mixed job that fans onto the cell
	 * (face, edge, corner) solves the same vertex. Any finer crossing on the boundary implies a stride-1
	 * crossing there, so a cell a fan needs always gets a vertex.
	 */
	template <typename TSampler>
	static bool ComputeBoundaryFallbackVertex(
		const TSampler& S,
		int32 CX, int32 CY, int32 CZ,
		int32 Stride, float VoxelSize, float IsoLevel, float SVDThreshold, float BiasStrength,
		FSeamCellVertex& Out)
	{
		Out.bFallbackTried = true;
		if (Stride <= 1)
		{
			return false; // a stride-1 cell's boundary crossings are its own edges: ComputeCellVertex saw them all
		}

		const int32 Lo[3] = { CX * Stride, CY * Stride, CZ * Stride };
		const int32 Hi[3] = { Lo[0] + Stride, Lo[1] + Stride, Lo[2] + Stride };
		FQEFSolver QEF;
		FVector3f AvgNormal = FVector3f::ZeroVector;
		// The vertex normal from the crossings' shading normals (G62); the QEF keeps the central difference.
		const bool bCrossingShading = VoxelCrossingGradient::UseCrossingGradientNormals();
		const bool bFittedQEFNormals = VoxelDCShading::UseFittedQEFNormals(1);
		FVector3f ShadingSum = FVector3f::ZeroVector;

		for (int32 Z = Lo[2]; Z <= Hi[2]; ++Z)
		{
			for (int32 Y = Lo[1]; Y <= Hi[1]; ++Y)
			{
				for (int32 X = Lo[0]; X <= Hi[0]; ++X)
				{
					const int32 P[3] = { X, Y, Z };
					const bool bOnMin[3] = { X == Lo[0], Y == Lo[1], Z == Lo[2] };
					const bool bOnMax[3] = { X == Hi[0], Y == Hi[1], Z == Hi[2] };
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						if (P[Axis] == Hi[Axis])
						{
							continue; // the edge would leave the cell
						}
						// On the boundary: some other axis sits on the cell's min or max plane.
						const int32 A1 = (Axis + 1) % 3;
						const int32 A2 = (Axis + 2) % 3;
						if (!(bOnMin[A1] || bOnMax[A1] || bOnMin[A2] || bOnMax[A2]))
						{
							continue;
						}

						const float D0 = S.GetDensity(X, Y, Z);
						int32 N[3] = { X, Y, Z };
						++N[Axis];
						const float D1 = S.GetDensity(N[0], N[1], N[2]);
						if ((D0 >= IsoLevel) == (D1 >= IsoLevel))
						{
							continue;
						}

						float t = (IsoLevel - D0) / (D1 - D0);
						t = FMath::Clamp(t, 0.0f, 1.0f);
						const FVector3f P0(static_cast<float>(X) * VoxelSize, static_cast<float>(Y) * VoxelSize, static_cast<float>(Z) * VoxelSize);
						const FVector3f P1(static_cast<float>(N[0]) * VoxelSize, static_cast<float>(N[1]) * VoxelSize, static_cast<float>(N[2]) * VoxelSize);
						const FVector3f CrossPos = P0 + (P1 - P0) * t;
						const FVector3f CrossShading = (bCrossingShading || bFittedQEFNormals)
							? S.ShadingNormal(CrossPos.X / VoxelSize, CrossPos.Y / VoxelSize, CrossPos.Z / VoxelSize) : FVector3f::ZeroVector;
						const FVector3f CrossNormal = bFittedQEFNormals
							? CrossShading : S.GradientNormal(CrossPos.X / VoxelSize, CrossPos.Y / VoxelSize, CrossPos.Z / VoxelSize, 1);
						QEF.Add(CrossPos, CrossNormal);
						AvgNormal += CrossNormal;
						if (bCrossingShading)
						{
							ShadingSum += CrossShading;
						}
					}
				}
			}
		}

		if (QEF.Count == 0)
		{
			return false;
		}

		const float CellWorldSize = static_cast<float>(Stride) * VoxelSize;
		const FVector3f Min(static_cast<float>(Lo[0]) * VoxelSize, static_cast<float>(Lo[1]) * VoxelSize, static_cast<float>(Lo[2]) * VoxelSize);
		const FBox3f CellBounds(Min, Min + FVector3f(CellWorldSize));

		Out.bValid = true;
		Out.MeshVertexIndex = -1;
		Out.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength, CVarDCQEFSolve.GetValueOnAnyThread() != 0);
		if (!AvgNormal.Normalize())
		{
			AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
		}
		Out.Normal = (bCrossingShading && ShadingSum.Normalize()) ? ShadingSum : AvgNormal;
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
	 * construction). A fine crossing facing a coarse cell with no crossing at its own stride fans
	 * onto that cell's boundary fallback vertex (G17, ComputeBoundaryFallbackVertex) instead of
	 * being skipped, which left a hole.
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
					if (!Cell.bValid && !Cell.bFallbackTried)
					{
						// G17: a coarser ring cell with no crossing at its stride; B's interior never emits it.
						int32 C[3];
						C[U] = 0;
						C[V] = BV;
						C[W] = BW;
						if (ComputeBoundaryFallbackVertex(SamplerB, C[0], C[1], C[2], SB, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
						{
							Cell.Position[U] += FrameOffsetU;
						}
					}
					if (!Cell.bValid)
					{
						return; // no surface on the cell's boundary at all: nothing to fan onto
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
					if (!Cell.bValid && !Cell.bFallbackTried)
					{
						// G17: a coarser slab cell with no crossing at the owner's stride.
						int32 C[3];
						C[U] = SLA;
						C[V] = AV;
						C[W] = AW;
						ComputeBoundaryFallbackVertex(SamplerC, C[0], C[1], C[2], SA, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell);
					}
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
			FIntVector FrameOffset = FIntVector::ZeroValue; // the owning participant's frame (cell solve)
			int32 CellABase = 0, CellBBase = 0;       // frame-local cell coords of (iv, iw) = (0, 0)
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
				Col.FrameOffset = FrameOffset;
				Col.CellABase = CellABase;
				Col.CellBBase = CellBBase;

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
					if (!Cell.bValid && !Cell.bFallbackTried)
					{
						// G17: a coarser cell with no crossing at its stride, solved as every job solves it.
						int32 C[3];
						C[U] = UVox / Col.StrideC;
						C[PerpA] = Col.CellABase + IV;
						C[PerpB] = Col.CellBBase + IW;
						const FQuadSampler Sampler{ Req, U, PerpA, PerpB, Col.ParticipantMask, Col.FrameOffset };
						if (ComputeBoundaryFallbackVertex(Sampler, C[0], C[1], C[2], Col.StrideC, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
						{
							Cell.Position[PerpA] += (Col.FrameOffset[PerpA] != 0 ? 1 : 0) * ChunkWorldSpan;
							Cell.Position[PerpB] += (Col.FrameOffset[PerpB] != 0 ? 1 : 0) * ChunkWorldSpan;
						}
					}
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
			FIntVector FrameOffset = FIntVector::ZeroValue; // the owning participant's frame (cell solve)
			int32 CellBase[3] = { 0, 0, 0 }; // frame-local cell coords of index (0, 0, 0)
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
						Reg.CellBase[Axis] = CellBase[Axis];
					}
					Reg.FrameOffset = FrameOffset;

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
				if (!Cell.bValid && !Cell.bFallbackTried)
				{
					// G17: a coarser cell with no crossing at its stride, solved as every job solves it.
					const FOctSampler Sampler{ Req, Reg.ParticipantMask, Reg.FrameOffset };
					if (ComputeBoundaryFallbackVertex(Sampler, Reg.CellBase[0] + Idx[0], Reg.CellBase[1] + Idx[1], Reg.CellBase[2] + Idx[2],
						Reg.StrideC, VoxelSize, IsoLevel, SVDThreshold, BiasStrength, Cell))
					{
						Cell.Position.X += (Reg.FrameOffset.X != 0) ? ChunkWorldSpan : 0.0f;
						Cell.Position.Y += (Reg.FrameOffset.Y != 0) ? ChunkWorldSpan : 0.0f;
						Cell.Position.Z += (Reg.FrameOffset.Z != 0) ? ChunkWorldSpan : 0.0f;
					}
				}
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
// Configuration
// ============================================================================

void FVoxelCPUDualContourMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelCPUDualContourMesher::GetConfig() const
{
	return Config;
}
