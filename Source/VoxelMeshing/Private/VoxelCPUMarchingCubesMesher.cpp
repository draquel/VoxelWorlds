// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUMarchingCubesMesher.h"
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
TAutoConsoleVariable<int32> CVarMCBoundaryMorph(
	TEXT("voxel.MCBoundaryMorph"),
	1,
	TEXT("MC LOD-seam geomorph: ramp the fine surface to the coarse height near transition faces ")
	TEXT("(1=on, 0=off). Off reproduces the vertical-wall step."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarMCBoundaryMorphWidth(
	TEXT("voxel.MCBoundaryMorphWidth"),
	2.0f,
	TEXT("MC LOD-seam geomorph ramp width, in coarse cells. Larger = gentler ramp, more fine detail traded."),
	ECVF_Default);

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

	ReleaseAllHandles();
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

	// Don't clear debug data automatically - let it accumulate across chunks
	// so visualization can show data from multiple chunks. The caller should
	// call ClearDebugData() when they want to reset the collection.
	// Limit the size to prevent unbounded growth.
	if (bCollectDebugVisualization && TransitionCellDebugData.Num() > 10000)
	{
		// Keep only the most recent entries
		TransitionCellDebugData.RemoveAt(0, TransitionCellDebugData.Num() - 5000);
	}

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

	// Get transition face mask for Transvoxel
	const uint8 TransitionMask = Config.bUseTransvoxel ? GetTransitionFaces(Request) : 0;
	const bool bHasTransitions = TransitionMask != 0;

	// Debug logging for transition cell processing
	if (bDebugLogTransitionCells)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("=== MESH GENERATION START ==="));
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Chunk: (%d,%d,%d) LOD: %d, Stride: %d"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
			Request.LODLevel, Stride);
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Transvoxel: %s, TransitionMask: 0x%02X, HasTransitions: %s"),
			Config.bUseTransvoxel ? TEXT("ON") : TEXT("OFF"),
			TransitionMask,
			bHasTransitions ? TEXT("Yes") : TEXT("No"));
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Neighbor LODs: [-X:%d +X:%d -Y:%d +Y:%d -Z:%d +Z:%d]"),
			Request.NeighborLODLevels[0], Request.NeighborLODLevels[1],
			Request.NeighborLODLevels[2], Request.NeighborLODLevels[3],
			Request.NeighborLODLevels[4], Request.NeighborLODLevels[5]);
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Debug flags: Logging=%s, Visualization=%s"),
			bDebugLogTransitionCells ? TEXT("ON") : TEXT("OFF"),
			bCollectDebugVisualization ? TEXT("ON") : TEXT("OFF"));
	}

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
	for (int32 Z = 0; Z < ChunkSize; Z += Stride)
	{
		for (int32 Y = 0; Y < ChunkSize; Y += Stride)
		{
			for (int32 X = 0; X < ChunkSize; X += Stride)
			{
				ProcessCubeLOD(Request, X, Y, Z, Stride, OutMeshData, TriangleCount,
					FColor(0, 0, 0, 0), TransitionMask);
			}
		}
	}

	// Generate skirts as fallback when Transvoxel is disabled
	if (!Config.bUseTransvoxel && Config.bGenerateSkirts)
	{
		GenerateSkirts(Request, Stride, OutMeshData, TriangleCount);
	}

	// Log debug summary for transition cells
	if (bDebugLogTransitionCells)
	{
		UE_LOG(LogVoxelMeshing, Warning, TEXT("=== MESH GENERATION COMPLETE ==="));
		UE_LOG(LogVoxelMeshing, Warning, TEXT("  Chunk (%d,%d,%d) LOD %d: %d transition cells in debug data"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z,
			Request.LODLevel, TransitionCellDebugData.Num());

		if (TransitionCellDebugData.Num() > 0)
		{
			// Count by face
			int32 FaceCounts[6] = {0, 0, 0, 0, 0, 0};
			for (const auto& Cell : TransitionCellDebugData)
			{
				if (Cell.FaceIndex >= 0 && Cell.FaceIndex < 6)
				{
					FaceCounts[Cell.FaceIndex]++;
				}
			}
			UE_LOG(LogVoxelMeshing, Warning, TEXT("  By face: -X:%d +X:%d -Y:%d +Y:%d -Z:%d +Z:%d"),
				FaceCounts[0], FaceCounts[1], FaceCounts[2], FaceCounts[3], FaceCounts[4], FaceCounts[5]);
		}
		else
		{
			UE_LOG(LogVoxelMeshing, Warning, TEXT("  No transition cells collected (TransitionMask was 0x%02X, bCollectViz=%s)"),
				TransitionMask, bCollectDebugVisualization ? TEXT("ON") : TEXT("OFF"));
		}
	}

	// Anomaly detection summary
	if (bDebugLogAnomalies && bCollectDebugVisualization && TransitionCellDebugData.Num() > 0)
	{
		const FTransitionDebugSummary Summary = GetTransitionDebugSummary();
		UE_LOG(LogVoxelMeshing, Warning,
			TEXT("=== ANOMALY SUMMARY Chunk (%d,%d,%d) LOD %d ==="),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, Request.LODLevel);
		UE_LOG(LogVoxelMeshing, Warning,
			TEXT("  Transition cells: %d total, %d empty (fell back to MC)"),
			Summary.TotalTransitionCells, Summary.EmptyCells);
		UE_LOG(LogVoxelMeshing, Warning,
			TEXT("  Per face: -X:%d +X:%d -Y:%d +Y:%d -Z:%d +Z:%d"),
			Summary.PerFaceCounts[0], Summary.PerFaceCounts[1], Summary.PerFaceCounts[2],
			Summary.PerFaceCounts[3], Summary.PerFaceCounts[4], Summary.PerFaceCounts[5]);
		if (Summary.CellsWithDisagreement > 0 || Summary.CellsWithClampedVertices > 0
			|| Summary.CellsWithFoldedTriangles > 0 || Summary.TotalFilteredTriangles > 0)
		{
			UE_LOG(LogVoxelMeshing, Warning,
				TEXT("  Anomalies: %d disagreement, %d clamped, %d folded, %d filtered tris"),
				Summary.CellsWithDisagreement, Summary.CellsWithClampedVertices,
				Summary.CellsWithFoldedTriangles, Summary.TotalFilteredTriangles);
		}
		else
		{
			UE_LOG(LogVoxelMeshing, Warning, TEXT("  No anomalies detected"));
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

void FVoxelCPUMarchingCubesMesher::ProcessCube(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount)
{
	const float VoxelSize = Request.VoxelSize;
	const float IsoLevel = Config.IsoLevel;

	// Lengyel's corner ordering — matches the Transvoxel regular MC tables
	// so that LOD 0 boundary triangulations are compatible with transition cells.
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
		const int32 SampleX = X + Offset.X;
		const int32 SampleY = Y + Offset.Y;
		const int32 SampleZ = Z + Offset.Z;
		CornerDensities[i] = GetDensityAt(Request, SampleX, SampleY, SampleZ);
		CornerPositions[i] = FVector3f(
			static_cast<float>(SampleX) * VoxelSize,
			static_cast<float>(SampleY) * VoxelSize,
			static_cast<float>(SampleZ) * VoxelSize
		);
	}

	// Build case index using Lengyel's corner ordering
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

	// Early out if no geometry
	if (TriangleCount == 0)
	{
		return;
	}

	// Convert SolidMask (Lengyel corner order, bits = solid) to classic MC ordering
	// for material/biome lookups. Classic corners 2↔3 and 6↔7 are swapped.
	const uint8 ClassicCubeIndex = static_cast<uint8>(
		(SolidMask & 0x33) |
		((SolidMask & 0x04) << 1) |
		((SolidMask & 0x08) >> 1) |
		((SolidMask & 0x40) << 1) |
		((SolidMask & 0x80) >> 1)
	);

	const uint8 MaterialID = GetDominantMaterial(Request, X, Y, Z, ClassicCubeIndex);
	const uint8 BiomeID = GetDominantBiome(Request, X, Y, Z, ClassicCubeIndex);

	// Decode edge vertices from RegularVertexData
	const int32 VertexCount = CellData.GetVertexCount();
	const uint16* VertexDataRow = TransvoxelTables::RegularVertexData[CaseIndex];
	FVector3f CellVertices[12];

	for (int32 i = 0; i < VertexCount; i++)
	{
		const uint16 VertexCode = VertexDataRow[i];
		const int32 CornerA = VertexCode & 0x0F;
		const int32 CornerB = (VertexCode >> 4) & 0x0F;

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
		}
	}

	// Emit triangles
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

		// Calculate normals using gradient of density field
		const FVector3f N0 = CalculateGradientNormal(Request,
			P0.X / VoxelSize, P0.Y / VoxelSize, P0.Z / VoxelSize);
		const FVector3f N1 = CalculateGradientNormal(Request,
			P1.X / VoxelSize, P1.Y / VoxelSize, P1.Z / VoxelSize);
		const FVector3f N2 = CalculateGradientNormal(Request,
			P2.X / VoxelSize, P2.Y / VoxelSize, P2.Z / VoxelSize);

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

void FVoxelCPUMarchingCubesMesher::ProcessCubeLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	FChunkMeshData& OutMeshData,
	uint32& OutTriangleCount,
	FColor DebugColorOverride,
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

	for (int32 i = 0; i < VertexCount; i++)
	{
		const uint16 VertexCode = VertexDataRow[i];
		const int32 CornerA = VertexCode & 0x0F;
		const int32 CornerB = (VertexCode >> 4) & 0x0F;

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
		}
	}

	// LOD-seam geomorph: for cubes in the boundary slab of an active transition face, ramp the
	// fine vertices toward the coarse-LOD surface so the boundary contour meets the coarser
	// neighbour with no vertical step (the ribbon then has ~zero height to span). No-op unless
	// this cube borders a coarser neighbour; gated by voxel.MCBoundaryMorph.
	if (TransitionMask != 0)
	{
		ApplyBoundaryMorph(Request, Stride, TransitionMask, CellVertices, VertexCount);
	}

	// Emit triangles using CellData triangle indices
	const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;
	FColor VertexColor(MaterialID, BiomeID, 0, 255);
	if (bDebugColorTransitionCells)
	{
		if (DebugColorOverride.A != 0)
		{
			VertexColor = DebugColorOverride; // Caller-specified (blue for fallback MC)
		}
		else
		{
			VertexColor = FColor(0, 200, 0, 255); // Green for regular MC
		}
	}
	const FVector2f MaterialUV(static_cast<float>(MaterialID), 0.0f);

	for (int32 t = 0; t < TriangleCount; t++)
	{
		const int32 Idx0 = CellData.VertexIndex[t * 3 + 0];
		const int32 Idx1 = CellData.VertexIndex[t * 3 + 1];
		const int32 Idx2 = CellData.VertexIndex[t * 3 + 2];

		const FVector3f& P0 = CellVertices[Idx0];
		const FVector3f& P1 = CellVertices[Idx1];
		const FVector3f& P2 = CellVertices[Idx2];

		// Calculate normals using gradient of density field
		const FVector3f N0 = CalculateGradientNormalLOD(Request,
			P0.X / VoxelSize, P0.Y / VoxelSize, P0.Z / VoxelSize, Stride);
		const FVector3f N1 = CalculateGradientNormalLOD(Request,
			P1.X / VoxelSize, P1.Y / VoxelSize, P1.Z / VoxelSize, Stride);
		const FVector3f N2 = CalculateGradientNormalLOD(Request,
			P2.X / VoxelSize, P2.Y / VoxelSize, P2.Z / VoxelSize, Stride);

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
	const FVoxelData Voxel = GetVoxelAt(Request, X, Y, Z);
	return static_cast<float>(Voxel.Density) / 255.0f;
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
	const int32 ChunkSize = Request.ChunkSize;

	// Check if within chunk bounds
	if (X >= 0 && X < ChunkSize && Y >= 0 && Y < ChunkSize && Z >= 0 && Z < ChunkSize)
	{
		return Request.GetVoxel(X, Y, Z);
	}

	// Handle neighbor chunk lookups
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Clamp coordinates for fallback
	const int32 ClampedX = FMath::Clamp(X, 0, ChunkSize - 1);
	const int32 ClampedY = FMath::Clamp(Y, 0, ChunkSize - 1);
	const int32 ClampedZ = FMath::Clamp(Z, 0, ChunkSize - 1);

	// Determine which axes are out of bounds and in which direction
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
		// Read the correct DEPTH plane into the neighbor (PlaneIdx 0 = the face slice,
		// deeper planes from the Deep arrays). MC geometry only samples PlaneIdx 0
		// (cubes are inward); the deeper planes are used by the boundary gradient
		// normals (central difference reaching past the face). At LOD 0 PlaneIdx is 0.
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
		// Fallback to edge voxel
		return Request.GetVoxel(ClampedX, ClampedY, ClampedZ);
	}

	// Edge case (2 axes out of bounds): use edge neighbor data
	if (OutCount == 2)
	{
		// X+Y edge (Z varies)
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

		// X+Z edge (Y varies)
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

		// Y+Z edge (X varies)
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

		// Fallback to edge voxel
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

		// Fallback to corner voxel
		return Request.GetVoxel(ClampedX, ClampedY, ClampedZ);
	}

	// Out of bounds - treat as air
	return FVoxelData::Air();
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

bool FVoxelCPUMarchingCubesMesher::ComputeCoarseSurfaceZ(
	const FVoxelMeshingRequest& Request,
	float Vx, float Vy, float NearVz,
	int32 CoarserStride,
	float& OutVz) const
{
	const float IsoLevel = Config.IsoLevel;
	const int32 E = FMath::Max(1, CoarserStride);
	const float Ef = static_cast<float>(E);

	const int32 ChunkSize = Request.ChunkSize;

	// Density sampled on the coarse (stride-E) lattice, trilinear between coarse grid points.
	// This is the surface the coarser neighbour sees, evaluated at the fine (Vx,Vy) column.
	// Face-parallel sample coords are CLAMPED to [0, ChunkSize]: neighbour data is only one
	// plane deep, so reading more than one voxel past a chunk face returns clamped/air densities
	// and corrupts the coarse surface (worst on steep terrain -> large spurious morph -> seam
	// holes). At a boundary the clamp samples the shared boundary plane = exactly what the
	// neighbour sees, so the boundary morph target equals the neighbour's coarse contour.
	auto SampleCoarse = [&](float Zq) -> float
	{
		const int32 X0 = FMath::FloorToInt(Vx / Ef) * E;
		const int32 Y0 = FMath::FloorToInt(Vy / Ef) * E;
		const int32 Z0 = FMath::FloorToInt(Zq / Ef) * E;
		const float Fx = (Vx - static_cast<float>(X0)) / Ef;
		const float Fy = (Vy - static_cast<float>(Y0)) / Ef;
		const float Fz = (Zq - static_cast<float>(Z0)) / Ef;

		const int32 Xa = FMath::Clamp(X0,     0, ChunkSize);
		const int32 Xb = FMath::Clamp(X0 + E, 0, ChunkSize);
		const int32 Ya = FMath::Clamp(Y0,     0, ChunkSize);
		const int32 Yb = FMath::Clamp(Y0 + E, 0, ChunkSize);

		const float D000 = GetDensityAt(Request, Xa, Ya, Z0);
		const float D100 = GetDensityAt(Request, Xb, Ya, Z0);
		const float D010 = GetDensityAt(Request, Xa, Yb, Z0);
		const float D110 = GetDensityAt(Request, Xb, Yb, Z0);
		const float D001 = GetDensityAt(Request, Xa, Ya, Z0 + E);
		const float D101 = GetDensityAt(Request, Xb, Ya, Z0 + E);
		const float D011 = GetDensityAt(Request, Xa, Yb, Z0 + E);
		const float D111 = GetDensityAt(Request, Xb, Yb, Z0 + E);

		const float D00 = FMath::Lerp(D000, D100, Fx);
		const float D10 = FMath::Lerp(D010, D110, Fx);
		const float D01 = FMath::Lerp(D001, D101, Fx);
		const float D11 = FMath::Lerp(D011, D111, Fx);
		const float D0 = FMath::Lerp(D00, D10, Fy);
		const float D1 = FMath::Lerp(D01, D11, Fy);
		return FMath::Lerp(D0, D1, Fz);
	};

	// Walk coarse Z cells around NearVz; return the iso-crossing nearest NearVz (handles caves /
	// multiple crossings by picking the closest). Search band ±SearchCells coarse cells.
	const int32 SearchCells = 4;
	const int32 ZBase = FMath::FloorToInt(NearVz / Ef) * E;
	float BestDist = TNumericLimits<float>::Max();
	bool bFound = false;
	for (int32 k = -SearchCells; k < SearchCells; k++)
	{
		const float Za = static_cast<float>(ZBase + k * E);
		const float Zb = Za + Ef;
		const float Da = SampleCoarse(Za);
		const float Db = SampleCoarse(Zb);
		const float Denom = Db - Da;
		if ((Da >= IsoLevel) != (Db >= IsoLevel))
		{
			const float T = (FMath::Abs(Denom) < KINDA_SMALL_NUMBER)
				? 0.5f : FMath::Clamp((IsoLevel - Da) / Denom, 0.0f, 1.0f);
			const float Zc = Za + T * Ef;
			const float Dist = FMath::Abs(Zc - NearVz);
			if (Dist < BestDist)
			{
				BestDist = Dist;
				OutVz = Zc;
				bFound = true;
			}
		}
	}
	return bFound;
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
	const float MorphWidth = FMath::Max(0.01f, CVarMCBoundaryMorphWidth.GetValueOnAnyThread());

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

	// Steepness gate: only morph where the surface is a HEIGHT FUNCTION (normal mostly vertical).
	// On steep / near-vertical surfaces the vertical-Z morph is ill-conditioned — it distorts the
	// thin boundary ribbon and opens the seam — so skip them; they keep their un-morphed,
	// watertight boundary. The surface normal is -gradient(density); a small |normal.Z| relative
	// to |normal| means a steep surface. Sampled ±1 voxel (within the 1-plane neighbour data) and
	// a pure function of position, so coincident vertices share the decision and stay watertight.
	// (v1 limitation; a normal-direction morph would generalise — see GEOMORPH_IMPLEMENTATION_PLAN.md.)
	{
		const float Gx = GetDensityAtTrilinear(Request, Vx + 1.0f, Vy, Vz) - GetDensityAtTrilinear(Request, Vx - 1.0f, Vy, Vz);
		const float Gy = GetDensityAtTrilinear(Request, Vx, Vy + 1.0f, Vz) - GetDensityAtTrilinear(Request, Vx, Vy - 1.0f, Vz);
		const float Gz = GetDensityAtTrilinear(Request, Vx, Vy, Vz + 1.0f) - GetDensityAtTrilinear(Request, Vx, Vy, Vz - 1.0f);
		const float GMag = FMath::Sqrt(Gx * Gx + Gy * Gy + Gz * Gz);
		constexpr float SteepThreshold = 0.5f; // |normal.Z|/|normal| below this => too steep => skip
		if (GMag < KINDA_SMALL_NUMBER || FMath::Abs(Gz) < SteepThreshold * GMag)
		{
			return;
		}
	}

	// Morph the height toward the coarse surface. v1 morphs along Z (correct for
	// height-function / INFINITE_PLANE terrain). If no coarse crossing is found nearby, leave
	// the vertex untouched (never worse than the current wall).
	float SecVz;
	if (ComputeCoarseSurfaceZ(Request, Vx, Vy, Vz, BestE, SecVz))
	{
		Vertex.Z = FMath::Lerp(Vz, SecVz, BestW) * VoxelSize;
	}
}

void FVoxelCPUMarchingCubesMesher::ApplyBoundaryMorph(
	const FVoxelMeshingRequest& Request,
	int32 Stride,
	uint8 TransitionMask,
	FVector3f* CellVertices,
	int32 VertexCount) const
{
	if (TransitionMask == 0 || CVarMCBoundaryMorph.GetValueOnAnyThread() == 0)
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
	// Use central difference for gradient approximation
	// Sample at half-voxel offsets for better accuracy
	const float Delta = 0.5f;

	// Round to integers for sampling (we can't truly interpolate here without trilinear,
	// so we use nearest neighbor with offsets)
	const int32 IX = FMath::FloorToInt(X);
	const int32 IY = FMath::FloorToInt(Y);
	const int32 IZ = FMath::FloorToInt(Z);

	// Central difference gradient
	float gx = GetDensityAt(Request, IX + 1, IY, IZ) - GetDensityAt(Request, IX - 1, IY, IZ);
	float gy = GetDensityAt(Request, IX, IY + 1, IZ) - GetDensityAt(Request, IX, IY - 1, IZ);
	float gz = GetDensityAt(Request, IX, IY, IZ + 1) - GetDensityAt(Request, IX, IY, IZ - 1);

	// Normal points away from solid (opposite to gradient direction)
	FVector3f Normal(-gx, -gy, -gz);

	// Normalize, with fallback for degenerate cases
	if (!Normal.Normalize())
	{
		return FVector3f(0.0f, 0.0f, 1.0f);
	}

	return Normal;
}

uint8 FVoxelCPUMarchingCubesMesher::GetDominantMaterial(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	uint8 CubeIndex) const
{
	// Select material from the solid corner closest to the isosurface (density nearest 0.5).
	// This ensures consistent surface material selection across all LOD levels.

	constexpr uint8 IsosurfaceThreshold = 128; // 0.5 in uint8 density
	uint8 SurfaceMaterial = 0;
	int32 ClosestDistance = INT32_MAX;

	for (int32 i = 0; i < 8; i++)
	{
		// Check if this corner is inside (solid)
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const FVoxelData Voxel = GetVoxelAt(Request, X + Offset.X, Y + Offset.Y, Z + Offset.Z);

			// Calculate distance from isosurface (how close density is to 0.5)
			const int32 DistanceFromSurface = FMath::Abs(static_cast<int32>(Voxel.Density) - IsosurfaceThreshold);

			if (DistanceFromSurface < ClosestDistance)
			{
				ClosestDistance = DistanceFromSurface;
				SurfaceMaterial = Voxel.MaterialID;
			}
		}
	}

	return SurfaceMaterial;
}

uint8 FVoxelCPUMarchingCubesMesher::GetDominantBiome(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	uint8 CubeIndex) const
{
	// Select biome from the solid corner closest to the isosurface (density nearest 0.5).
	// Consistent with GetDominantMaterial for uniform surface selection.

	constexpr uint8 IsosurfaceThreshold = 128; // 0.5 in uint8 density
	uint8 SurfaceBiome = 0;
	int32 ClosestDistance = INT32_MAX;

	for (int32 i = 0; i < 8; i++)
	{
		// Check if this corner is inside (solid)
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const FVoxelData Voxel = GetVoxelAt(Request, X + Offset.X, Y + Offset.Y, Z + Offset.Z);

			// Calculate distance from isosurface (how close density is to 0.5)
			const int32 DistanceFromSurface = FMath::Abs(static_cast<int32>(Voxel.Density) - IsosurfaceThreshold);

			if (DistanceFromSurface < ClosestDistance)
			{
				ClosestDistance = DistanceFromSurface;
				SurfaceBiome = Voxel.BiomeID;
			}
		}
	}

	return SurfaceBiome;
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

uint8 FVoxelCPUMarchingCubesMesher::GetDominantMaterialLOD(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 Stride,
	uint8 CubeIndex) const
{
	// For LOD > 0, find the surface material by scanning upward from solid corners.
	// On slopes, the surface is at different Z levels across the cube, so we need
	// to find the actual surface transition (solid→air) for each corner and use
	// the material from just below that transition.
	//
	// Strategy: For each solid strided corner, scan upward to find where density
	// drops below threshold (the surface), then use the last solid voxel's material.

	constexpr int32 MaxScanDistance = 8; // Don't scan too far up
	uint8 SurfaceMaterial = 0;
	int32 HighestSurfaceZ = INT32_MIN;

	for (int32 i = 0; i < 8; i++)
	{
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const int32 CornerX = X + Offset.X * Stride;
			const int32 CornerY = Y + Offset.Y * Stride;
			const int32 CornerZ = Z + Offset.Z * Stride;

			// Scan upward from this corner to find the surface
			uint8 LastSolidMaterial = 0;
			int32 SurfaceZ = CornerZ;

			for (int32 dz = 0; dz <= MaxScanDistance; dz++)
			{
				const FVoxelData Voxel = GetVoxelAt(Request, CornerX, CornerY, CornerZ + dz);

				if (Voxel.IsSolid())
				{
					LastSolidMaterial = Voxel.MaterialID;
					SurfaceZ = CornerZ + dz;
				}
				else
				{
					// Found air - the surface is at the previous solid voxel
					break;
				}
			}

			// Use the material from the highest surface found (prefer grass over dirt)
			if (SurfaceZ > HighestSurfaceZ)
			{
				HighestSurfaceZ = SurfaceZ;
				SurfaceMaterial = LastSolidMaterial;
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
	// For LOD > 0, find the surface biome by scanning upward from solid corners.
	// Consistent with GetDominantMaterialLOD approach.

	constexpr int32 MaxScanDistance = 8;
	uint8 SurfaceBiome = 0;
	int32 HighestSurfaceZ = INT32_MIN;

	for (int32 i = 0; i < 8; i++)
	{
		if (CubeIndex & (1 << i))
		{
			const FIntVector& Offset = MarchingCubesTables::CornerOffsets[i];
			const int32 CornerX = X + Offset.X * Stride;
			const int32 CornerY = Y + Offset.Y * Stride;
			const int32 CornerZ = Z + Offset.Z * Stride;

			// Scan upward from this corner to find the surface
			uint8 LastSolidBiome = 0;
			int32 SurfaceZ = CornerZ;

			for (int32 dz = 0; dz <= MaxScanDistance; dz++)
			{
				const FVoxelData Voxel = GetVoxelAt(Request, CornerX, CornerY, CornerZ + dz);

				if (Voxel.IsSolid())
				{
					LastSolidBiome = Voxel.BiomeID;
					SurfaceZ = CornerZ + dz;
				}
				else
				{
					break;
				}
			}

			// Use the biome from the highest surface found
			if (SurfaceZ > HighestSurfaceZ)
			{
				HighestSurfaceZ = SurfaceZ;
				SurfaceBiome = LastSolidBiome;
			}
		}
	}

	return SurfaceBiome;
}

// ============================================================================
// Async Pattern (wraps sync for CPU mesher)
// ============================================================================

FVoxelMeshingHandle FVoxelCPUMarchingCubesMesher::GenerateMeshAsync(
	const FVoxelMeshingRequest& Request,
	FOnVoxelMeshingComplete OnComplete)
{
	const uint64 RequestId = NextRequestId.fetch_add(1);
	FVoxelMeshingHandle Handle(RequestId, Request.ChunkCoord);

	// CPU mesher runs synchronously
	FCachedResult Result;
	Result.bSuccess = GenerateMeshCPU(Request, Result.MeshData, Result.Stats);

	// Cache the result
	{
		FScopeLock Lock(&CacheLock);
		CachedResults.Add(RequestId, MoveTemp(Result));
	}

	Handle.bIsComplete = true;
	Handle.bWasSuccessful = Result.bSuccess;

	// Call completion delegate
	if (OnComplete.IsBound())
	{
		OnComplete.Execute(Handle, Handle.bWasSuccessful);
	}

	return Handle;
}

bool FVoxelCPUMarchingCubesMesher::IsComplete(const FVoxelMeshingHandle& Handle) const
{
	return Handle.bIsComplete;
}

bool FVoxelCPUMarchingCubesMesher::WasSuccessful(const FVoxelMeshingHandle& Handle) const
{
	return Handle.bWasSuccessful;
}

FRHIBuffer* FVoxelCPUMarchingCubesMesher::GetVertexBuffer(const FVoxelMeshingHandle& Handle)
{
	// CPU mesher doesn't create GPU buffers
	return nullptr;
}

FRHIBuffer* FVoxelCPUMarchingCubesMesher::GetIndexBuffer(const FVoxelMeshingHandle& Handle)
{
	// CPU mesher doesn't create GPU buffers
	return nullptr;
}

bool FVoxelCPUMarchingCubesMesher::GetBufferCounts(
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

bool FVoxelCPUMarchingCubesMesher::GetRenderData(
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

bool FVoxelCPUMarchingCubesMesher::ReadbackToCPU(
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

void FVoxelCPUMarchingCubesMesher::ReleaseHandle(const FVoxelMeshingHandle& Handle)
{
	FScopeLock Lock(&CacheLock);
	CachedResults.Remove(Handle.RequestId);
}

void FVoxelCPUMarchingCubesMesher::ReleaseAllHandles()
{
	FScopeLock Lock(&CacheLock);
	CachedResults.Empty();
}

void FVoxelCPUMarchingCubesMesher::SetConfig(const FVoxelMeshingConfig& InConfig)
{
	Config = InConfig;
}

const FVoxelMeshingConfig& FVoxelCPUMarchingCubesMesher::GetConfig() const
{
	return Config;
}

bool FVoxelCPUMarchingCubesMesher::GetStats(
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

// ============================================================================
// Skirt Generation (LOD Seam Hiding)
// ============================================================================

void FVoxelCPUMarchingCubesMesher::GenerateSkirts(
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

	// Tolerance for boundary detection - use larger tolerance to catch interpolated vertices
	const float BoundaryTolerance = VoxelSize * Stride * 0.6f;

	// LOD seams occur because adjacent chunks at different LOD levels have mismatched
	// vertex positions along their shared boundary. Skirts extend boundary vertices
	// downward to create vertical strips that hide gaps.

	const int32 OriginalVertexCount = OutMeshData.Positions.Num();
	const int32 OriginalIndexCount = OutMeshData.Indices.Num();

	// Only generate skirts on faces that have LOD transitions
	const uint8 TransitionMask = Request.TransitionFaces;

	// For each face, collect boundary edges
	// Face 0: -X (X near 0), Face 1: +X (X near ChunkWorldSize)
	// Face 2: -Y (Y near 0), Face 3: +Y (Y near ChunkWorldSize)

	for (int32 Face = 0; Face < 4; Face++)
	{
		// Check if this face has a LOD transition
		static const uint8 FaceTransitionFlags[4] = {
			FVoxelMeshingRequest::TRANSITION_XNEG,  // Face 0: -X
			FVoxelMeshingRequest::TRANSITION_XPOS,  // Face 1: +X
			FVoxelMeshingRequest::TRANSITION_YNEG,  // Face 2: -Y
			FVoxelMeshingRequest::TRANSITION_YPOS,  // Face 3: +Y
		};

		// Only generate skirts where there's an actual LOD transition
		if ((TransitionMask & FaceTransitionFlags[Face]) == 0)
		{
			continue;
		}

		// Collect edges on this boundary
		struct FBoundaryEdge
		{
			int32 V0, V1;  // Original vertex indices
		};
		TArray<FBoundaryEdge> BoundaryEdges;

		// Determine boundary parameters
		const bool bIsXFace = (Face == 0 || Face == 1);
		const bool bIsPositiveFace = (Face == 1 || Face == 3);
		const float BoundaryValue = bIsPositiveFace ? ChunkWorldSize : 0.0f;

		// Direction to extend skirt (DOWNWARD for vertical seam coverage)
		// Skirts drop straight down to cover vertical gaps between LOD levels
		const FVector3f SkirtDir = FVector3f(0.0f, 0.0f, -1.0f);

		// Find edges that lie on this boundary (only from original mesh)
		const int32 NumTriangles = OriginalIndexCount / 3;
		for (int32 TriIdx = 0; TriIdx < NumTriangles; TriIdx++)
		{
			const int32 BaseIdx = TriIdx * 3;
			const int32 Idx0 = OutMeshData.Indices[BaseIdx];
			const int32 Idx1 = OutMeshData.Indices[BaseIdx + 1];
			const int32 Idx2 = OutMeshData.Indices[BaseIdx + 2];

			// Skip if any index is out of range
			if (Idx0 >= OriginalVertexCount || Idx1 >= OriginalVertexCount || Idx2 >= OriginalVertexCount)
			{
				continue;
			}

			const FVector3f& P0 = OutMeshData.Positions[Idx0];
			const FVector3f& P1 = OutMeshData.Positions[Idx1];
			const FVector3f& P2 = OutMeshData.Positions[Idx2];

			// Check which vertices are on the boundary
			auto IsOnBoundary = [&](const FVector3f& P) -> bool
			{
				const float Coord = bIsXFace ? P.X : P.Y;
				return FMath::Abs(Coord - BoundaryValue) < BoundaryTolerance;
			};

			const bool b0 = IsOnBoundary(P0);
			const bool b1 = IsOnBoundary(P1);
			const bool b2 = IsOnBoundary(P2);

			// Add edges where both vertices are on the boundary
			auto AddEdgeIfOnBoundary = [&](int32 IdxA, int32 IdxB, bool bA, bool bB)
			{
				if (bA && bB)
				{
					FBoundaryEdge Edge;
					Edge.V0 = IdxA;
					Edge.V1 = IdxB;
					BoundaryEdges.Add(Edge);
				}
			};

			AddEdgeIfOnBoundary(Idx0, Idx1, b0, b1);
			AddEdgeIfOnBoundary(Idx1, Idx2, b1, b2);
			AddEdgeIfOnBoundary(Idx2, Idx0, b2, b0);
		}

		const float UVScale = Config.bGenerateUVs ? Config.UVScale : 0.0f;

		// Generate skirt geometry for each boundary edge
		for (const FBoundaryEdge& Edge : BoundaryEdges)
		{
			// IMPORTANT: Make copies, not references. Adding to arrays can cause reallocation.
			const FVector3f P0 = OutMeshData.Positions[Edge.V0];
			const FVector3f P1 = OutMeshData.Positions[Edge.V1];
			const FVector3f N0 = OutMeshData.Normals[Edge.V0];
			const FVector3f N1 = OutMeshData.Normals[Edge.V1];
			const FColor C0 = OutMeshData.Colors[Edge.V0];
			const FColor C1 = OutMeshData.Colors[Edge.V1];
			const FVector2f MatUV0 = OutMeshData.UV1s.IsValidIndex(Edge.V0) ? OutMeshData.UV1s[Edge.V0] : FVector2f::ZeroVector;
			const FVector2f MatUV1 = OutMeshData.UV1s.IsValidIndex(Edge.V1) ? OutMeshData.UV1s[Edge.V1] : FVector2f::ZeroVector;

			// Create skirt vertices that extend straight down
			// This creates a vertical curtain that hides gaps between LOD levels
			const FVector3f Bottom0 = P0 + SkirtDir * SkirtDepth;
			const FVector3f Bottom1 = P1 + SkirtDir * SkirtDepth;

			// Skirt normal faces outward from the boundary (perpendicular to the face)
			FVector3f SkirtNormal;
			if (bIsXFace)
			{
				SkirtNormal = bIsPositiveFace ? FVector3f(1, 0, 0) : FVector3f(-1, 0, 0);
			}
			else
			{
				SkirtNormal = bIsPositiveFace ? FVector3f(0, 1, 0) : FVector3f(0, -1, 0);
			}

			// UV coordinates based on world position
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

			// Add 4 vertices for the skirt quad
			const uint32 BaseVertex = OutMeshData.Positions.Num();

			// Vertices: 0=P0 (top), 1=Bottom0, 2=P1 (top), 3=Bottom1
			OutMeshData.Positions.Add(P0);
			OutMeshData.Positions.Add(Bottom0);
			OutMeshData.Positions.Add(P1);
			OutMeshData.Positions.Add(Bottom1);

			// All skirt vertices use the outward-facing normal
			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);
			OutMeshData.Normals.Add(SkirtNormal);

			OutMeshData.UVs.Add(UV0);
			OutMeshData.UVs.Add(UVBottom0);
			OutMeshData.UVs.Add(UV1);
			OutMeshData.UVs.Add(UVBottom1);

			// UV1: MaterialID only (smooth meshing uses triplanar, no FaceType needed)
			OutMeshData.UV1s.Add(FVector2f(MatUV0.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV0.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV1.X, 0.0f));
			OutMeshData.UV1s.Add(FVector2f(MatUV1.X, 0.0f));

			OutMeshData.Colors.Add(C0);
			OutMeshData.Colors.Add(C0);
			OutMeshData.Colors.Add(C1);
			OutMeshData.Colors.Add(C1);

			// Generate triangles with correct winding based on face direction
			// Skirt is a vertical quad: P0-P1 at top, Bottom0-Bottom1 at bottom
			// For positive faces (+X, +Y), normal faces outward
			if (bIsPositiveFace)
			{
				// Triangle 1: P0 -> Bottom0 -> P1
				OutMeshData.Indices.Add(BaseVertex + 0);
				OutMeshData.Indices.Add(BaseVertex + 1);
				OutMeshData.Indices.Add(BaseVertex + 2);

				// Triangle 2: P1 -> Bottom0 -> Bottom1
				OutMeshData.Indices.Add(BaseVertex + 2);
				OutMeshData.Indices.Add(BaseVertex + 1);
				OutMeshData.Indices.Add(BaseVertex + 3);
			}
			else
			{
				// Reverse winding for negative faces
				// Triangle 1: P0 -> P1 -> Bottom0
				OutMeshData.Indices.Add(BaseVertex + 0);
				OutMeshData.Indices.Add(BaseVertex + 2);
				OutMeshData.Indices.Add(BaseVertex + 1);

				// Triangle 2: P1 -> Bottom1 -> Bottom0
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
		UE_LOG(LogVoxelMeshing, Log, TEXT("Generated %d skirt triangles for chunk (%d,%d,%d) at LOD %d"),
			SkirtTris, Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, Request.LODLevel);
	}
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

bool FVoxelCPUMarchingCubesMesher::IsTransitionCell(
	int32 X, int32 Y, int32 Z,
	int32 ChunkSize, int32 Stride,
	uint8 TransitionMask,
	int32& OutFaceIndex) const
{
	// A cell is a transition cell if it's on the edge of the chunk
	// and that edge borders a coarser (higher LOD level) neighbor.
	// We only need transition cells on the last row of cells on each face.

	// Check each face
	// -X face (X == 0)
	if ((TransitionMask & TransitionXNeg) && X == 0)
	{
		OutFaceIndex = 0;
		return true;
	}
	// +X face (X == ChunkSize - Stride)
	if ((TransitionMask & TransitionXPos) && X == ChunkSize - Stride)
	{
		OutFaceIndex = 1;
		return true;
	}
	// -Y face (Y == 0)
	if ((TransitionMask & TransitionYNeg) && Y == 0)
	{
		OutFaceIndex = 2;
		return true;
	}
	// +Y face (Y == ChunkSize - Stride)
	if ((TransitionMask & TransitionYPos) && Y == ChunkSize - Stride)
	{
		OutFaceIndex = 3;
		return true;
	}
	// -Z face (Z == 0)
	if ((TransitionMask & TransitionZNeg) && Z == 0)
	{
		OutFaceIndex = 4;
		return true;
	}
	// +Z face (Z == ChunkSize - Stride)
	if ((TransitionMask & TransitionZPos) && Z == ChunkSize - Stride)
	{
		OutFaceIndex = 5;
		return true;
	}

	OutFaceIndex = -1;
	return false;
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

bool FVoxelCPUMarchingCubesMesher::IsInTransitionRegion(
	const FVoxelMeshingRequest& Request,
	int32 X, int32 Y, int32 Z,
	int32 ChunkSize, int32 CurrentStride,
	uint8 TransitionMask) const
{
	// Check each face for transition regions.
	// A cell is in a transition region if:
	// 1. That face has a transition (bit set in TransitionMask)
	// 2. The cell is within CurrentStride (thin cell depth) of the boundary
	// 3. The cell is NOT the boundary row itself (that's handled by IsTransitionCell)
	//
	// THIN TRANSITION CELLS: With thin cells (depth = CurrentStride), the transition region
	// has zero depth for positive faces (always false) and impossible range for negative faces
	// when CurrentStride=1 (X < 1 && X != 0 is empty). This effectively disables suppression.

	// For negative faces, transition region is [0, CurrentStride)
	// For positive faces, transition region is [ChunkSize - CurrentStride, ChunkSize - CurrentStride) (always false)

	// -X face
	if (TransitionMask & TransitionXNeg)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 0, CurrentStride);
		if (TransitionStride > CurrentStride && X >= 0 && X < CurrentStride && X != 0)
		{
			return true; // In -X transition region but not the boundary row
		}
	}

	// +X face
	if (TransitionMask & TransitionXPos)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 1, CurrentStride);
		if (TransitionStride > CurrentStride && X >= ChunkSize - CurrentStride && X < ChunkSize - CurrentStride)
		{
			return true; // In +X transition region but not the boundary row
		}
	}

	// -Y face
	if (TransitionMask & TransitionYNeg)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 2, CurrentStride);
		if (TransitionStride > CurrentStride && Y >= 0 && Y < CurrentStride && Y != 0)
		{
			return true;
		}
	}

	// +Y face
	if (TransitionMask & TransitionYPos)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 3, CurrentStride);
		if (TransitionStride > CurrentStride && Y >= ChunkSize - CurrentStride && Y < ChunkSize - CurrentStride)
		{
			return true;
		}
	}

	// -Z face
	if (TransitionMask & TransitionZNeg)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 4, CurrentStride);
		if (TransitionStride > CurrentStride && Z >= 0 && Z < CurrentStride && Z != 0)
		{
			return true;
		}
	}

	// +Z face
	if (TransitionMask & TransitionZPos)
	{
		const int32 TransitionStride = GetTransitionCellStride(Request, 5, CurrentStride);
		if (TransitionStride > CurrentStride && Z >= ChunkSize - CurrentStride && Z < ChunkSize - CurrentStride)
		{
			return true;
		}
	}

	return false;
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
	static const TCHAR* FaceNames[6] = { TEXT("-X"), TEXT("+X"), TEXT("-Y"), TEXT("+Y"), TEXT("-Z"), TEXT("+Z") };

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

	// Debug logging for transition cells
	if (bDebugLogTransitionCells)
	{
		UE_LOG(LogVoxelMeshing, Log, TEXT("=== TRANSITION CELL ==="));
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Chunk: (%d,%d,%d) LOD: %d"),
			Request.ChunkCoord.X, Request.ChunkCoord.Y, Request.ChunkCoord.Z, Request.LODLevel);
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Cell: (%d,%d,%d) Face: %s Stride: %d"),
			X, Y, Z, FaceNames[FaceIndex], Stride);
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Neighbor LOD: %d"), Request.NeighborLODLevels[FaceIndex]);
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Case: %d (0x%03X) Class: %d Inverted: %s"),
			CaseIndex, CaseIndex, CellClass, bInverted ? TEXT("Yes") : TEXT("No"));
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Face Densities: [%.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f]"),
			Densities[0], Densities[1], Densities[2],
			Densities[3], Densities[4], Densities[5],
			Densities[6], Densities[7], Densities[8]);
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Interior Densities: [%.3f, %.3f, %.3f, %.3f]"),
			Densities[9], Densities[10], Densities[11], Densities[12]);
	}

	// Early out for empty cases (class 0)
	// No surface crosses the transition face in this cell. Adjacent non-boundary
	// MC cells handle any interior crossings. No fallback MC needed.
	if (CellClass == 0)
	{
		if (bDebugLogTransitionCells)
		{
			UE_LOG(LogVoxelMeshing, Log, TEXT("  Result: Empty (class 0) - falling back to regular MC"));
		}
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

	if (bDebugLogTransitionCells)
	{
		UE_LOG(LogVoxelMeshing, Log, TEXT("  Generating: %d vertices, %d triangles"), VertexCount, TriangleCount);
	}

	// ---- Anomaly Detection: Interior-Face Corner Disagreement ----
	// Interior corners 9-12 correspond to face corners 0,2,6,8.
	// If they disagree on inside/outside, the surface crosses between face and interior.
	// This is expected but logged for diagnosis. Multiple disagreements can indicate
	// the transition cell is producing a surface very different from what regular MC would.
	uint8 DisagreementMask = 0;
	if (bDebugLogAnomalies || bCollectDebugVisualization)
	{
		static constexpr int32 InteriorToFace[4] = { 0, 2, 6, 8 };
		for (int32 i = 0; i < 4; i++)
		{
			const bool bFaceInside = (Densities[InteriorToFace[i]] >= IsoLevel);
			const bool bInteriorInside = (Densities[9 + i] >= IsoLevel);
			if (bFaceInside != bInteriorInside)
			{
				DisagreementMask |= (1 << i);
			}
		}
		if (DisagreementMask != 0 && bDebugLogAnomalies)
		{
			UE_LOG(LogVoxelMeshing, Warning,
				TEXT("ANOMALY [Disagreement] Cell (%d,%d,%d) Face %s: interior corners disagree mask=0x%X "
					 "(densities: face[%.3f,%.3f,%.3f,%.3f] interior[%.3f,%.3f,%.3f,%.3f])"),
				X, Y, Z, FaceNames[FaceIndex], DisagreementMask,
				Densities[0], Densities[2], Densities[6], Densities[8],
				Densities[9], Densities[10], Densities[11], Densities[12]);
		}
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

	// Collect debug visualization data if enabled
	FTransitionCellDebugData* DebugData = nullptr;
	if (bCollectDebugVisualization)
	{
		DebugData = &TransitionCellDebugData.AddDefaulted_GetRef();
		DebugData->ChunkCoord = Request.ChunkCoord;
		DebugData->CellBasePos = BasePos;
		DebugData->FaceIndex = FaceIndex;
		DebugData->Stride = Stride;
		DebugData->CurrentLOD = Request.LODLevel;
		DebugData->NeighborLOD = Request.NeighborLODLevels[FaceIndex];
		DebugData->CaseIndex = CaseIndex;
		DebugData->CellClass = CellClass;
		DebugData->bInverted = bInverted;
		FMemory::Memcpy(DebugData->SampleDensities, Densities, sizeof(Densities));

		// Store sample positions
		DebugData->SamplePositions.Reserve(13);
		for (int32 i = 0; i < 13; i++)
		{
			const FVector3f& Offset = TransvoxelTables::TransitionCellSampleOffsets[FaceIndex][i];
			DebugData->SamplePositions.Add(BasePos + Offset * CellScale);
		}
	}

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
	CellVertices.Reserve(VertexCount);
	VertexOnOuterFace.Reserve(VertexCount);
	bool bHasClampedVertices = false;

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

			if (bDebugLogAnomalies || bCollectDebugVisualization)
			{
				const float Denom = DensityB - DensityA;
				if (FMath::Abs(Denom) > KINDA_SMALL_NUMBER)
				{
					const float RawT = (IsoLevel - DensityA) / Denom;
					if (RawT < 0.0f || RawT > 1.0f)
					{
						bHasClampedVertices = true;
						if (bDebugLogAnomalies)
						{
							const TCHAR* EdgeType =
								(SampleA >= 9 && SampleB >= 9) ? TEXT("corner-corner (outer)") :
								(SampleA >= 9 || SampleB >= 9) ? TEXT("face-corner (depth)") :
								TEXT("face-face (inner)");
							UE_LOG(LogVoxelMeshing, Warning,
								TEXT("ANOMALY [Clamped] Cell (%d,%d,%d) Face %s: vertex %d %s edge %d-%d "
									 "t=%.3f (d=%.3f,%.3f) — both endpoints %s isosurface"),
								X, Y, Z, FaceNames[FaceIndex], i, EdgeType, SampleA, SampleB,
								RawT, DensityA, DensityB,
								RawT < 0.0f ? TEXT("above") : TEXT("below"));
						}
					}
				}
			}

			VertexPos = InterpolateEdge(DensityA, DensityB, PosA, PosB, IsoLevel);
		}

		// Validate vertex position is finite
		if (!FMath::IsFinite(VertexPos.X) || !FMath::IsFinite(VertexPos.Y) || !FMath::IsFinite(VertexPos.Z))
		{
			UE_LOG(LogVoxelMeshing, Error, TEXT("Transition cell: NaN/Inf vertex %d - skipping cell"), i);
			return false;
		}

		CellVertices.Add(VertexPos);
		// "Outer face" = the coarse boundary face, whose vertices lie between coarse
		// corners (samples 9-12). Those use the coarser stride for normals so they match
		// the coarser neighbour; everything touching the fine inner face uses the fine stride.
		VertexOnOuterFace.Add(SampleA >= 9 && SampleB >= 9);

		if (bDebugLogTransitionCells)
		{
			UE_LOG(LogVoxelMeshing, Log, TEXT("    Vertex %d: (%.1f, %.1f, %.1f) from samples %d-%d (d=%.3f,%.3f)"),
				i, VertexPos.X, VertexPos.Y, VertexPos.Z, SampleA, SampleB,
				Densities[SampleA], Densities[SampleB]);
		}
	}

	// Geomorph the ribbon's FINE-side vertices toward the coarse surface, matching the (also
	// morphed) fine-MC boundary contour so they stay coincident -> the vertical wall collapses
	// while the seam stays watertight. The OUTER (coarse-corner) vertices are left on the coarse
	// contour so the exact neighbour match (T6/T8) is preserved. Gated by voxel.MCBoundaryMorph.
	if (CVarMCBoundaryMorph.GetValueOnAnyThread() != 0)
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

	// Store generated vertices for debug visualization
	if (DebugData)
	{
		DebugData->GeneratedVertices = CellVertices;
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
	const FColor VertexColor = bDebugColorTransitionCells
		? FColor(255, 128, 0, 255)  // Orange for transition cells
		: FColor(MaterialID, BiomeID, 0, 255);

	// Add vertices to mesh
	const uint32 BaseIndex = OutMeshData.Positions.Num();

	for (int32 i = 0; i < CellVertices.Num(); i++)
	{
		const FVector3f& Pos = CellVertices[i];
		OutMeshData.Positions.Add(Pos);

		// Calculate normal using gradient — match the stride of the adjacent mesh:
		// Outer face vertices use CoarserStride (matches coarser neighbor MC normals),
		// interior/depth vertices use CurrentStride (matches finer chunk MC normals).
		const int32 NormalStride = VertexOnOuterFace[i] ? Stride : CurrentStride;
		const FVector3f Normal = CalculateGradientNormalLOD(Request,
			Pos.X / VoxelSize, Pos.Y / VoxelSize, Pos.Z / VoxelSize, NormalStride);
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
	int32 NumFilteredTriangles = 0;
	bool bHasFoldedTriangles = false;

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
				NumFilteredTriangles++;
				if (bDebugLogAnomalies)
				{
					UE_LOG(LogVoxelMeshing, Warning,
						TEXT("ANOMALY [Filtered] Cell (%d,%d,%d) Face %s: tri %d filtered (edges: %.1f, %.1f, %.1f, max: %.1f)"),
						X, Y, Z, FaceNames[FaceIndex], t,
						FMath::Sqrt(EdgeSq01), FMath::Sqrt(EdgeSq12), FMath::Sqrt(EdgeSq20),
						FMath::Sqrt(MaxEdgeLengthSq));
				}
				continue; // Skip this degenerate triangle
			}

			// Folded triangle detection: RENDERED face normal vs gradient at centroid
			// Account for winding reversal to avoid false positives
			if (bDebugLogAnomalies || bCollectDebugVisualization)
			{
				const FVector3f TableNormal = FVector3f::CrossProduct(V1 - V0, V2 - V0);
				const FVector3f FaceNormal = bUseOriginalWinding ? TableNormal : -TableNormal;
				if (FaceNormal.SizeSquared() > KINDA_SMALL_NUMBER)
				{
					const FVector3f Centroid = (V0 + V1 + V2) / 3.0f;
					const FVector3f GradNormal = CalculateGradientNormalLOD(Request,
						Centroid.X / VoxelSize, Centroid.Y / VoxelSize, Centroid.Z / VoxelSize, CurrentStride);
					const float Dot = FVector3f::DotProduct(FaceNormal.GetSafeNormal(), GradNormal);
					if (Dot < -0.1f) // Face normal opposes gradient by more than ~95 degrees
					{
						bHasFoldedTriangles = true;
						if (bDebugLogAnomalies)
						{
							UE_LOG(LogVoxelMeshing, Warning,
								TEXT("ANOMALY [Folded] Cell (%d,%d,%d) Face %s: tri %d has face normal opposing gradient "
									 "(dot=%.3f, verts: [%.1f,%.1f,%.1f] [%.1f,%.1f,%.1f] [%.1f,%.1f,%.1f])"),
								X, Y, Z, FaceNames[FaceIndex], t, Dot,
								V0.X, V0.Y, V0.Z, V1.X, V1.Y, V1.Z, V2.X, V2.Y, V2.Z);
						}
					}
				}
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

	// Store anomaly flags in debug data
	if (DebugData)
	{
		DebugData->bHasFaceInteriorDisagreement = (DisagreementMask != 0);
		DebugData->bHasClampedVertices = bHasClampedVertices;
		DebugData->bHasFoldedTriangles = bHasFoldedTriangles;
		DebugData->NumFilteredTriangles = NumFilteredTriangles;
		DebugData->DisagreementMask = DisagreementMask;
	}

	// ---- MC Comparison Mesh: generate what regular MC would produce ----
	if (bDebugComparisonMesh && DebugData)
	{
		FChunkMeshData TempMeshData;
		uint32 TempTriCount = 0;
		for (int32 d1 = 0; d1 < Stride; d1 += CurrentStride)
		{
			for (int32 d0 = 0; d0 < Stride; d0 += CurrentStride)
			{
				int32 CX, CY, CZ;
				switch (DepthAxis)
				{
				case 0: CX = X; CY = Y + d0; CZ = Z + d1; break;
				case 1: CX = X + d0; CY = Y; CZ = Z + d1; break;
				default: CX = X + d0; CY = Y + d1; CZ = Z; break;
				}
				if (CX < Request.ChunkSize && CY < Request.ChunkSize && CZ < Request.ChunkSize)
				{
					ProcessCubeLOD(Request, CX, CY, CZ, CurrentStride, TempMeshData, TempTriCount);
				}
			}
		}
		DebugData->MCComparisonVertices = MoveTemp(TempMeshData.Positions);
		DebugData->MCComparisonIndices = MoveTemp(TempMeshData.Indices);
	}

	return true;
}

FVoxelCPUMarchingCubesMesher::FTransitionDebugSummary FVoxelCPUMarchingCubesMesher::GetTransitionDebugSummary() const
{
	FTransitionDebugSummary Summary;
	Summary.TotalTransitionCells = TransitionCellDebugData.Num();

	for (const FTransitionCellDebugData& Cell : TransitionCellDebugData)
	{
		if (Cell.FaceIndex >= 0 && Cell.FaceIndex < 6)
		{
			Summary.PerFaceCounts[Cell.FaceIndex]++;
		}
		if (Cell.CellClass == 0)
		{
			Summary.EmptyCells++;
		}
		if (Cell.bHasFaceInteriorDisagreement)
		{
			Summary.CellsWithDisagreement++;
		}
		if (Cell.bHasClampedVertices)
		{
			Summary.CellsWithClampedVertices++;
		}
		if (Cell.bHasFoldedTriangles)
		{
			Summary.CellsWithFoldedTriangles++;
		}
		Summary.TotalFilteredTriangles += Cell.NumFilteredTriangles;
	}

	return Summary;
}
