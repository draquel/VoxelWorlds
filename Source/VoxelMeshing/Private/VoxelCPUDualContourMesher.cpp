// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUDualContourMesher.h"
#include "VoxelMeshing.h"
#include "QEFSolver.h"

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

	// Pass 1: Detect edge crossings
	TMap<uint64, FDCEdgeCrossing> EdgeCrossings;
	DetectEdgeCrossings(Request, Stride, EdgeCrossings);

	// Pass 2: Solve QEF for cell vertices
	TMap<uint64, FDCCellVertex> CellVertices;
	SolveCellVertices(Request, Stride, EdgeCrossings, CellVertices);

	// Pass 3.5: Merge LOD boundary cells (before quad generation)
	MergeLODBoundaryCells(Request, Stride, EdgeCrossings, CellVertices);

	// Pass 3: Generate quads
	GenerateQuads(Request, Stride, EdgeCrossings, CellVertices, OutMeshData, TriangleCount);

	// Calculate stats
	const double EndTime = FPlatformTime::Seconds();
	OutStats.VertexCount = OutMeshData.Positions.Num();
	OutStats.IndexCount = OutMeshData.Indices.Num();
	OutStats.FaceCount = TriangleCount;
	OutStats.SolidVoxelCount = SolidVoxels;
	OutStats.CulledFaceCount = 0;
	OutStats.GenerationTimeMs = static_cast<float>((EndTime - StartTime) * 1000.0);

	UE_LOG(LogVoxelMeshing, Verbose,
		TEXT("DC meshing complete: %d verts, %d tris, %d edge crossings, %d cells, %.2fms"),
		OutStats.VertexCount, TriangleCount, EdgeCrossings.Num(), CellVertices.Num(),
		OutStats.GenerationTimeMs);

	return true;
}

// ============================================================================
// Pass 1: Edge Crossing Detection
// ============================================================================

void FVoxelCPUDualContourMesher::DetectEdgeCrossings(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	TMap<uint64, FDCEdgeCrossing>& OutEdgeCrossings)
{
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const int32 GridSize = ChunkSize / Stride;

	// Iterate all grid cells including +1 overlap for neighbor boundary edges.
	// Cell coordinates are in grid space: voxel position = CellCoord * Stride.
	// We process from -1 to GridSize to handle cross-chunk boundary edges.
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

				// Check 3 edges: +X, +Y, +Z
				for (int32 Axis = 0; Axis < 3; Axis++)
				{
					int32 NX = VX, NY = VY, NZ = VZ;
					if (Axis == 0) NX += Stride;
					else if (Axis == 1) NY += Stride;
					else NZ += Stride;

					// Boundary cells (-1 and GridSize) need edge crossings for proper
					// QEF data. Quad ownership filtering in GenerateQuads prevents
					// duplicate emission, so we detect all edges in the iteration range.
					const float D1 = GetDensityAt(Request, NX, NY, NZ);

					// Check for sign change (crossing)
					const bool bSolid0 = (D0 >= IsoLevel);
					const bool bSolid1 = (D1 >= IsoLevel);

					if (bSolid0 != bSolid1)
					{
						// Compute crossing point via linear interpolation
						float t = (IsoLevel - D0) / (D1 - D0);
						t = FMath::Clamp(t, 0.0f, 1.0f);

						const FVector3f P0(static_cast<float>(VX) * VoxelSize,
							static_cast<float>(VY) * VoxelSize,
							static_cast<float>(VZ) * VoxelSize);
						const FVector3f P1(static_cast<float>(NX) * VoxelSize,
							static_cast<float>(NY) * VoxelSize,
							static_cast<float>(NZ) * VoxelSize);

						FDCEdgeCrossing Crossing;
						Crossing.Position = P0 + (P1 - P0) * t;

						// Compute gradient normal at the crossing point
						const float CrossVoxelX = Crossing.Position.X / VoxelSize;
						const float CrossVoxelY = Crossing.Position.Y / VoxelSize;
						const float CrossVoxelZ = Crossing.Position.Z / VoxelSize;
						Crossing.Normal = (Stride > 1)
							? CalculateGradientNormalLOD(Request, CrossVoxelX, CrossVoxelY, CrossVoxelZ, Stride)
							: CalculateGradientNormal(Request, CrossVoxelX, CrossVoxelY, CrossVoxelZ);

						OutEdgeCrossings.Add(MakeEdgeKey(CX, CY, CZ, Axis), Crossing);
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
	const TMap<uint64, FDCEdgeCrossing>& EdgeCrossings,
	TMap<uint64, FDCCellVertex>& OutCellVertices)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 GridSize = ChunkSize / Stride;
	const float VoxelSize = Request.VoxelSize;
	const float CellWorldSize = static_cast<float>(Stride) * VoxelSize;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;

	// For each cell in the grid, check if any of its 12 edges have crossings
	for (int32 CZ = -1; CZ <= GridSize; CZ++)
	{
		for (int32 CY = -1; CY <= GridSize; CY++)
		{
			for (int32 CX = -1; CX <= GridSize; CX++)
			{
				FQEFSolver QEF;

				// A cell at (CX,CY,CZ) has 12 edges.
				// Edges owned by this cell (3 edges at minimum corner):
				//   (CX,CY,CZ) along X, Y, Z
				// Edges owned by neighboring cells but touching this cell (9 edges):
				//   +X face: (CX+1,CY,CZ) along Y, Z
				//   +Y face: (CX,CY+1,CZ) along X, Z
				//   +Z face: (CX,CY,CZ+1) along X, Y
				//   +XY edge: (CX+1,CY+1,CZ) along Z
				//   +XZ edge: (CX+1,CY,CZ+1) along Y
				//   +YZ edge: (CX,CY+1,CZ+1) along X

				struct FEdgeRef { int32 DX, DY, DZ, Axis; };
				static const FEdgeRef CellEdges[12] = {
					// 3 edges at minimum corner
					{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 2},
					// 3 edges along +X face
					{1, 0, 0, 1}, {1, 0, 0, 2},
					// 2 edges along +Y face
					{0, 1, 0, 0}, {0, 1, 0, 2},
					// 1 edge along +Z face
					{0, 0, 1, 0}, {0, 0, 1, 1},
					// 3 edges at far corners
					{1, 1, 0, 2}, {1, 0, 1, 1}, {0, 1, 1, 0},
				};

				for (const auto& Edge : CellEdges)
				{
					const uint64 Key = MakeEdgeKey(CX + Edge.DX, CY + Edge.DY, CZ + Edge.DZ, Edge.Axis);
					const FDCEdgeCrossing* Crossing = EdgeCrossings.Find(Key);
					if (Crossing)
					{
						QEF.Add(Crossing->Position, Crossing->Normal);
					}
				}

				if (QEF.Count == 0)
				{
					continue;
				}

				// Compute cell bounds in world space
				const float MinX = static_cast<float>(CX * Stride) * VoxelSize;
				const float MinY = static_cast<float>(CY * Stride) * VoxelSize;
				const float MinZ = static_cast<float>(CZ * Stride) * VoxelSize;
				const FBox3f CellBounds(
					FVector3f(MinX, MinY, MinZ),
					FVector3f(MinX + CellWorldSize, MinY + CellWorldSize, MinZ + CellWorldSize)
				);

				FDCCellVertex Vertex;
				Vertex.Position = QEF.Solve(SVDThreshold, CellBounds, BiasStrength);
				Vertex.Normal = (QEF.MassPoint / static_cast<float>(QEF.Count)); // temp, overwrite below

				// Average normal from edge crossings
				FVector3f AvgNormal = FVector3f::ZeroVector;
				for (const auto& Edge : CellEdges)
				{
					const uint64 Key = MakeEdgeKey(CX + Edge.DX, CY + Edge.DY, CZ + Edge.DZ, Edge.Axis);
					const FDCEdgeCrossing* Crossing = EdgeCrossings.Find(Key);
					if (Crossing)
					{
						AvgNormal += Crossing->Normal;
					}
				}
				if (!AvgNormal.Normalize())
				{
					AvgNormal = FVector3f(0.0f, 0.0f, 1.0f);
				}
				Vertex.Normal = AvgNormal;

				// Material and biome from solid voxels in cell
				Vertex.MaterialID = GetCellMaterial(Request, CX * Stride, CY * Stride, CZ * Stride, Stride);
				Vertex.BiomeID = GetCellBiome(Request, CX * Stride, CY * Stride, CZ * Stride, Stride);

				OutCellVertices.Add(MakeCellKey(CX, CY, CZ), Vertex);
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
	const TMap<uint64, FDCEdgeCrossing>& EdgeCrossings,
	TMap<uint64, FDCCellVertex>& CellVertices,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 GridSize = ChunkSize / Stride;
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;

	for (const auto& Pair : EdgeCrossings)
	{
		const uint64 EdgeKey = Pair.Key;
		const FDCEdgeCrossing& Crossing = Pair.Value;

		// Decode edge key
		const int32 Axis = static_cast<int32>((EdgeKey >> 30) & 0x3);
		const int32 CX = static_cast<int32>(EdgeKey & 0x3FF) - 1;
		const int32 CY = static_cast<int32>((EdgeKey >> 10) & 0x3FF) - 1;
		const int32 CZ = static_cast<int32>((EdgeKey >> 20) & 0x3FF) - 1;

		// The 4 cells sharing this edge depend on the axis:
		// Axis X (edge along X at CX,CY,CZ): cells (CX, CY-1..CY, CZ-1..CZ) — vary Y,Z
		// Axis Y (edge along Y at CX,CY,CZ): cells (CX-1..CX, CY, CZ-1..CZ) — vary X,Z
		// Axis Z (edge along Z at CX,CY,CZ): cells (CX-1..CX, CY-1..CY, CZ) — vary X,Y
		struct FCellOffset { int32 DX, DY, DZ; };
		FCellOffset Offsets[4];

		if (Axis == 0) // X-edge: cells vary in Y and Z
		{
			Offsets[0] = {0,  0,  0};
			Offsets[1] = {0, -1,  0};
			Offsets[2] = {0, -1, -1};
			Offsets[3] = {0,  0, -1};
		}
		else if (Axis == 1) // Y-edge: cells vary in X and Z (Z first, then X → Z×X = +Y)
		{
			Offsets[0] = { 0, 0,  0};
			Offsets[1] = { 0, 0, -1};
			Offsets[2] = {-1, 0, -1};
			Offsets[3] = {-1, 0,  0};
		}
		else // Z-edge: cells vary in X and Y
		{
			Offsets[0] = { 0,  0, 0};
			Offsets[1] = {-1,  0, 0};
			Offsets[2] = {-1, -1, 0};
			Offsets[3] = { 0, -1, 0};
		}

		// Edge ownership: only emit quads for edges where at least one cell
		// is in the valid range [0, GridSize-1]. This prevents duplicate quads.
		bool bOwned = false;
		for (int32 i = 0; i < 4; i++)
		{
			const int32 CCX = CX + Offsets[i].DX;
			const int32 CCY = CY + Offsets[i].DY;
			const int32 CCZ = CZ + Offsets[i].DZ;
			if (CCX >= 0 && CCX < GridSize && CCY >= 0 && CCY < GridSize && CCZ >= 0 && CCZ < GridSize)
			{
				bOwned = true;
				break;
			}
		}
		if (!bOwned)
		{
			continue;
		}

		// Look up the 4 cell vertices
		FDCCellVertex* Verts[4] = {nullptr, nullptr, nullptr, nullptr};
		bool bAllValid = true;

		for (int32 i = 0; i < 4; i++)
		{
			const uint64 CellKey = MakeCellKey(CX + Offsets[i].DX, CY + Offsets[i].DY, CZ + Offsets[i].DZ);
			Verts[i] = CellVertices.Find(CellKey);
			if (!Verts[i])
			{
				bAllValid = false;
				break;
			}
		}

		if (!bAllValid)
		{
			continue;
		}

		// Emit vertices if not already emitted
		int32 Indices[4];
		for (int32 i = 0; i < 4; i++)
		{
			Indices[i] = EmitVertex(Request, *Verts[i], OutMeshData);
		}

		// Determine winding order from density sign at the edge start
		const int32 VX = CX * Stride;
		const int32 VY = CY * Stride;
		const int32 VZ = CZ * Stride;
		const float D0 = GetDensityAt(Request, VX, VY, VZ);
		const bool bFlip = (D0 < IsoLevel); // Air at start → flip winding

		// Emit 2 triangles as a quad
		if (bFlip)
		{
			OutMeshData.Indices.Add(Indices[0]);
			OutMeshData.Indices.Add(Indices[1]);
			OutMeshData.Indices.Add(Indices[2]);

			OutMeshData.Indices.Add(Indices[0]);
			OutMeshData.Indices.Add(Indices[2]);
			OutMeshData.Indices.Add(Indices[3]);
		}
		else
		{
			OutMeshData.Indices.Add(Indices[0]);
			OutMeshData.Indices.Add(Indices[2]);
			OutMeshData.Indices.Add(Indices[1]);

			OutMeshData.Indices.Add(Indices[0]);
			OutMeshData.Indices.Add(Indices[3]);
			OutMeshData.Indices.Add(Indices[2]);
		}

		OutTriangleCount += 2;
	}
}

// ============================================================================
// Pass 4: LOD Boundary Cell Merging
// ============================================================================

void FVoxelCPUDualContourMesher::MergeLODBoundaryCells(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	TMap<uint64, FDCEdgeCrossing>& EdgeCrossings,
	TMap<uint64, FDCCellVertex>& CellVertices)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 GridSize = ChunkSize / Stride;
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;
	const float SVDThreshold = Config.QEFSVDThreshold;
	const float BiasStrength = Config.QEFBiasStrength;

	// Check each face for LOD transitions
	for (int32 Face = 0; Face < 6; Face++)
	{
		const int32 NeighborLOD = Request.NeighborLODLevels[Face];
		if (NeighborLOD <= Request.LODLevel)
		{
			continue; // No transition or finer neighbor — skip
		}

		const int32 CoarserStride = 1 << NeighborLOD;
		const int32 MergeRatio = CoarserStride / Stride; // How many fine cells per coarse cell (per axis)

		if (MergeRatio <= 1)
		{
			continue;
		}

		const int32 DepthAxis = Face / 2;
		const bool bPositiveFace = (Face % 2 == 1);
		const int32 BoundaryCell = bPositiveFace ? (GridSize - 1) : 0;

		// Iterate boundary cells in 2D (the two axes orthogonal to DepthAxis)
		// Group them into MergeRatio x MergeRatio blocks
		int32 Axis1Size = GridSize, Axis2Size = GridSize;

		for (int32 A2 = 0; A2 < Axis2Size; A2 += MergeRatio)
		{
			for (int32 A1 = 0; A1 < Axis1Size; A1 += MergeRatio)
			{
				// Merge MergeRatio x MergeRatio fine cells into 1 coarse cell
				FQEFSolver MergedQEF;
				int32 MergedCount = 0;

				for (int32 DA2 = 0; DA2 < MergeRatio && (A2 + DA2) < Axis2Size; DA2++)
				{
					for (int32 DA1 = 0; DA1 < MergeRatio && (A1 + DA1) < Axis1Size; DA1++)
					{
						int32 CX, CY, CZ;
						switch (DepthAxis)
						{
						case 0: CX = BoundaryCell; CY = A1 + DA1; CZ = A2 + DA2; break;
						case 1: CX = A1 + DA1; CY = BoundaryCell; CZ = A2 + DA2; break;
						default: CX = A1 + DA1; CY = A2 + DA2; CZ = BoundaryCell; break;
						}

						const uint64 CellKey = MakeCellKey(CX, CY, CZ);
						const FDCCellVertex* ExistingVertex = CellVertices.Find(CellKey);
						if (!ExistingVertex)
						{
							continue;
						}

						// Collect hermite data from this cell's edge crossings
						struct FEdgeRef { int32 DX, DY, DZ, Axis; };
						static const FEdgeRef CellEdges[12] = {
							{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 0, 2},
							{1, 0, 0, 1}, {1, 0, 0, 2},
							{0, 1, 0, 0}, {0, 1, 0, 2},
							{0, 0, 1, 0}, {0, 0, 1, 1},
							{1, 1, 0, 2}, {1, 0, 1, 1}, {0, 1, 1, 0},
						};

						for (const auto& Edge : CellEdges)
						{
							const uint64 EKey = MakeEdgeKey(CX + Edge.DX, CY + Edge.DY, CZ + Edge.DZ, Edge.Axis);
							const FDCEdgeCrossing* Crossing = EdgeCrossings.Find(EKey);
							if (Crossing)
							{
								MergedQEF.Add(Crossing->Position, Crossing->Normal);
							}
						}

						// Remove the fine cell vertex (will be replaced by merged vertex)
						CellVertices.Remove(CellKey);
						MergedCount++;
					}
				}

				if (MergedQEF.Count == 0 || MergedCount == 0)
				{
					continue;
				}

				// Re-detect edge crossings at coarser resolution along the boundary face.
				// This ensures the boundary edge topology matches the neighbor's grid exactly.
				// The neighbor's cells at this face boundary sample at CoarserStride spacing,
				// so we must produce equivalent edge crossings.
				int32 BaseCX, BaseCY, BaseCZ;
				switch (DepthAxis)
				{
				case 0: BaseCX = BoundaryCell; BaseCY = A1; BaseCZ = A2; break;
				case 1: BaseCX = A1; BaseCY = BoundaryCell; BaseCZ = A2; break;
				default: BaseCX = A1; BaseCY = A2; BaseCZ = BoundaryCell; break;
				}

				// Compute merged cell bounds
				const float MinX = static_cast<float>(BaseCX * Stride) * VoxelSize;
				const float MinY = static_cast<float>(BaseCY * Stride) * VoxelSize;
				const float MinZ = static_cast<float>(BaseCZ * Stride) * VoxelSize;
				float SizeX = static_cast<float>(Stride) * VoxelSize;
				float SizeY = SizeX, SizeZ = SizeX;

				// Merged cell spans MergeRatio fine cells in the non-depth axes
				switch (DepthAxis)
				{
				case 0:
					SizeY = static_cast<float>(FMath::Min(MergeRatio, Axis1Size - A1) * Stride) * VoxelSize;
					SizeZ = static_cast<float>(FMath::Min(MergeRatio, Axis2Size - A2) * Stride) * VoxelSize;
					break;
				case 1:
					SizeX = static_cast<float>(FMath::Min(MergeRatio, Axis1Size - A1) * Stride) * VoxelSize;
					SizeZ = static_cast<float>(FMath::Min(MergeRatio, Axis2Size - A2) * Stride) * VoxelSize;
					break;
				default:
					SizeX = static_cast<float>(FMath::Min(MergeRatio, Axis1Size - A1) * Stride) * VoxelSize;
					SizeY = static_cast<float>(FMath::Min(MergeRatio, Axis2Size - A2) * Stride) * VoxelSize;
					break;
				}

				const FBox3f MergedBounds(
					FVector3f(MinX, MinY, MinZ),
					FVector3f(MinX + SizeX, MinY + SizeY, MinZ + SizeZ)
				);

				// Solve merged QEF
				FDCCellVertex MergedVertex;
				MergedVertex.Position = MergedQEF.Solve(SVDThreshold, MergedBounds, BiasStrength);

				// Average normal
				FVector3f AvgNormal = MergedQEF.MassPoint / static_cast<float>(MergedQEF.Count);
				// Re-compute gradient normal at the solved position
				const float VoxX = MergedVertex.Position.X / VoxelSize;
				const float VoxY = MergedVertex.Position.Y / VoxelSize;
				const float VoxZ = MergedVertex.Position.Z / VoxelSize;
				MergedVertex.Normal = CalculateGradientNormalLOD(Request, VoxX, VoxY, VoxZ, CoarserStride);

				MergedVertex.MaterialID = GetCellMaterial(Request, BaseCX * Stride, BaseCY * Stride, BaseCZ * Stride, CoarserStride);
				MergedVertex.BiomeID = GetCellBiome(Request, BaseCX * Stride, BaseCY * Stride, BaseCZ * Stride, CoarserStride);

				// Store merged vertex at the base cell position
				// All fine cells in this group now point to this merged vertex
				const uint64 MergedKey = MakeCellKey(BaseCX, BaseCY, BaseCZ);
				CellVertices.Add(MergedKey, MergedVertex);

				// Add aliases so quad generation can find this vertex from any of the original fine cell coords
				for (int32 DA2 = 0; DA2 < MergeRatio && (A2 + DA2) < Axis2Size; DA2++)
				{
					for (int32 DA1 = 0; DA1 < MergeRatio && (A1 + DA1) < Axis1Size; DA1++)
					{
						if (DA1 == 0 && DA2 == 0)
						{
							continue; // Already stored at base position
						}

						int32 AliasCX, AliasCY, AliasCZ;
						switch (DepthAxis)
						{
						case 0: AliasCX = BoundaryCell; AliasCY = A1 + DA1; AliasCZ = A2 + DA2; break;
						case 1: AliasCX = A1 + DA1; AliasCY = BoundaryCell; AliasCZ = A2 + DA2; break;
						default: AliasCX = A1 + DA1; AliasCY = A2 + DA2; AliasCZ = BoundaryCell; break;
						}

						// Point alias cells to same merged vertex
						CellVertices.Add(MakeCellKey(AliasCX, AliasCY, AliasCZ), MergedVertex);
					}
				}
			}
		}
	}
}

// ============================================================================
// Material & Biome Voting
// ============================================================================

uint8 FVoxelCPUDualContourMesher::GetCellMaterial(
	const FVoxelMeshingRequest& Request,
	int32 CellX, int32 CellY, int32 CellZ,
	int32 Stride) const
{
	// Find the solid voxel closest to the isosurface (density nearest 0.5)
	constexpr uint8 IsosurfaceThreshold = 128;
	uint8 BestMaterial = 0;
	int32 ClosestDistance = INT32_MAX;

	// Check the 8 corners of the cell
	for (int32 dz = 0; dz <= 1; dz++)
	{
		for (int32 dy = 0; dy <= 1; dy++)
		{
			for (int32 dx = 0; dx <= 1; dx++)
			{
				const int32 VX = CellX + dx * Stride;
				const int32 VY = CellY + dy * Stride;
				const int32 VZ = CellZ + dz * Stride;
				const FVoxelData Voxel = GetVoxelAt(Request, VX, VY, VZ);

				if (Voxel.IsSolid())
				{
					const int32 Dist = FMath::Abs(static_cast<int32>(Voxel.Density) - IsosurfaceThreshold);
					if (Dist < ClosestDistance)
					{
						ClosestDistance = Dist;
						BestMaterial = Voxel.MaterialID;
					}
				}
			}
		}
	}

	return BestMaterial;
}

uint8 FVoxelCPUDualContourMesher::GetCellBiome(
	const FVoxelMeshingRequest& Request,
	int32 CellX, int32 CellY, int32 CellZ,
	int32 Stride) const
{
	constexpr uint8 IsosurfaceThreshold = 128;
	uint8 BestBiome = 0;
	int32 ClosestDistance = INT32_MAX;

	for (int32 dz = 0; dz <= 1; dz++)
	{
		for (int32 dy = 0; dy <= 1; dy++)
		{
			for (int32 dx = 0; dx <= 1; dx++)
			{
				const int32 VX = CellX + dx * Stride;
				const int32 VY = CellY + dy * Stride;
				const int32 VZ = CellZ + dz * Stride;
				const FVoxelData Voxel = GetVoxelAt(Request, VX, VY, VZ);

				if (Voxel.IsSolid())
				{
					const int32 Dist = FMath::Abs(static_cast<int32>(Voxel.Density) - IsosurfaceThreshold);
					if (Dist < ClosestDistance)
					{
						ClosestDistance = Dist;
						BestBiome = Voxel.BiomeID;
					}
				}
			}
		}
	}

	return BestBiome;
}

// ============================================================================
// Vertex Emission
// ============================================================================

int32 FVoxelCPUDualContourMesher::EmitVertex(
	const FVoxelMeshingRequest& Request,
	FDCCellVertex& Vertex,
	FChunkMeshData& OutMeshData)
{
	// Return existing index if already emitted
	if (Vertex.MeshVertexIndex >= 0)
	{
		return Vertex.MeshVertexIndex;
	}

	const float VoxelSize = Request.VoxelSize;
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;

	const int32 Index = OutMeshData.Positions.Num();
	Vertex.MeshVertexIndex = Index;

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
	OutMeshData.UV1s.Add(FVector2f(static_cast<float>(Vertex.MaterialID), 0.0f));

	// Vertex color: R=MaterialID, G=BiomeID (same format as smooth mesher)
	OutMeshData.Colors.Add(FColor(Vertex.MaterialID, Vertex.BiomeID, 0, 255));

	return Index;
}

// ============================================================================
// Density & Voxel Access (copied from FVoxelCPUSmoothMesher)
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

	// Single-axis out of bounds: use face neighbor data
	if (OutCount == 1)
	{
		if (bXPos && Request.NeighborXPos.Num() == SliceSize)
		{
			return Request.NeighborXPos[Y + Z * ChunkSize];
		}
		if (bXNeg && Request.NeighborXNeg.Num() == SliceSize)
		{
			return Request.NeighborXNeg[Y + Z * ChunkSize];
		}
		if (bYPos && Request.NeighborYPos.Num() == SliceSize)
		{
			return Request.NeighborYPos[X + Z * ChunkSize];
		}
		if (bYNeg && Request.NeighborYNeg.Num() == SliceSize)
		{
			return Request.NeighborYNeg[X + Z * ChunkSize];
		}
		if (bZPos && Request.NeighborZPos.Num() == SliceSize)
		{
			return Request.NeighborZPos[X + Y * ChunkSize];
		}
		if (bZNeg && Request.NeighborZNeg.Num() == SliceSize)
		{
			return Request.NeighborZNeg[X + Y * ChunkSize];
		}
		return Request.GetVoxel(ClampedX, ClampedY, ClampedZ);
	}

	// Edge case (2 axes out of bounds): use edge neighbor data
	if (OutCount == 2)
	{
		if (bXPos && bYPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS))
		{
			return Request.EdgeXPosYPos[Z];
		}
		if (bXPos && bYNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG))
		{
			return Request.EdgeXPosYNeg[Z];
		}
		if (bXNeg && bYPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS))
		{
			return Request.EdgeXNegYPos[Z];
		}
		if (bXNeg && bYNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG))
		{
			return Request.EdgeXNegYNeg[Z];
		}

		if (bXPos && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS))
		{
			return Request.EdgeXPosZPos[Y];
		}
		if (bXPos && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG))
		{
			return Request.EdgeXPosZNeg[Y];
		}
		if (bXNeg && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS))
		{
			return Request.EdgeXNegZPos[Y];
		}
		if (bXNeg && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG))
		{
			return Request.EdgeXNegZNeg[Y];
		}

		if (bYPos && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS))
		{
			return Request.EdgeYPosZPos[X];
		}
		if (bYPos && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG))
		{
			return Request.EdgeYPosZNeg[X];
		}
		if (bYNeg && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS))
		{
			return Request.EdgeYNegZPos[X];
		}
		if (bYNeg && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG))
		{
			return Request.EdgeYNegZNeg[X];
		}

		return Request.GetVoxel(ClampedX, ClampedY, ClampedZ);
	}

	// Corner case (3 axes out of bounds): use corner neighbor data
	if (OutCount == 3)
	{
		if (bXPos && bYPos && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS))
		{
			return Request.CornerXPosYPosZPos;
		}
		if (bXPos && bYPos && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG))
		{
			return Request.CornerXPosYPosZNeg;
		}
		if (bXPos && bYNeg && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS))
		{
			return Request.CornerXPosYNegZPos;
		}
		if (bXPos && bYNeg && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG))
		{
			return Request.CornerXPosYNegZNeg;
		}
		if (bXNeg && bYPos && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS))
		{
			return Request.CornerXNegYPosZPos;
		}
		if (bXNeg && bYPos && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG))
		{
			return Request.CornerXNegYPosZNeg;
		}
		if (bXNeg && bYNeg && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS))
		{
			return Request.CornerXNegYNegZPos;
		}
		if (bXNeg && bYNeg && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG))
		{
			return Request.CornerXNegYNegZNeg;
		}

		return Request.GetVoxel(ClampedX, ClampedY, ClampedZ);
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
