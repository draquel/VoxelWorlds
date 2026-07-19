// Copyright Daniel Raquel. All Rights Reserved.

// Seam-ownership P3: single-owner Marching Cubes seam meshing (SEAM_OWNERSHIP_ARCHITECTURE.md §2.2).
//
// Design: MC vertices and normals are pure functions of local density samples (edge-lerp crossings,
// central-difference gradients) — no QEF, no accumulation-order sensitivity. So each participant's
// boundary band (its first/last cell layer toward the seam) is meshed with a per-participant
// SYNTHETIC FVoxelMeshingRequest — the participant's own volume plus REAL neighbor slices extracted
// from the other participants' snapshots — driven through the exact same ProcessCubeLOD /
// ProcessTransitionCell / geomorph code as legacy whole-chunk meshing. Junctions with the
// interior-domain chunk meshes are bit-identical by construction (same functions, same values).
//
// Coverage partition per chunk (cells, per axis, N = ChunkSize/Stride):
//   interior mesh: [1, N-2]   face seam: exactly one axis at {0, N-1}, others [1, N-2]
//   edge seam: exactly two axes at {0, N-1}   corner seam: all three
// Every cell is meshed exactly once across the world (MC cells never extend outside their chunk).
//
// Mixed LOD: the transvoxel ribbon (zero-thickness, on the boundary plane) is emitted by the FACE
// job in the finer participant's frame over the full face extent; ribbon cells that would need
// data beyond the two face participants (the positive-perpendicular perimeter rows) self-skip via
// HasRequiredNeighborData — a known v1 limitation (small boundary-plane gaps along those face
// edges at mixed LOD; the boundary geomorph reduces their visibility). The geomorph runs with a
// BAND-CONFINED ramp (MorphWidthOverride: zero exactly at the band's inner edge) so morphed band
// vertices stay coincident with the unmorphed interior mesh.

#include "VoxelCPUMarchingCubesMesher.h"
#include "VoxelMeshing.h"

namespace VoxelMCSeamDetail
{

// In-plane index layout must match ExtractNeighborEdgeSlices / GetVoxelAt:
// X faces -> [Y + Z*CS], Y faces -> [X + Z*CS], Z faces -> [X + Y*CS].
static FORCEINLINE int32 PlaneIndex(int32 Axis, int32 X, int32 Y, int32 Z, int32 CS)
{
	switch (Axis)
	{
	case 0:  return Y + Z * CS;
	case 1:  return X + Z * CS;
	default: return X + Y * CS;
	}
}

/** Fill a facing face's plane-0 + deep arrays from a sibling volume. bSiblingOnPos: the sibling
 *  sits on the consumer's POSITIVE side of Axis (so its LOW planes face the consumer). */
static void FillFacePlanes(
	const TArray<FVoxelData>& Src, int32 CS, int32 Axis, bool bSiblingOnPos, int32 Depth,
	TArray<FVoxelData>& OutPlane0, TArray<FVoxelData>& OutDeep)
{
	const int32 SliceSize = CS * CS;
	OutPlane0.SetNumUninitialized(SliceSize);
	const int32 ExtraPlanes = Depth - 1;
	if (ExtraPlanes > 0)
	{
		OutDeep.SetNumUninitialized(ExtraPlanes * SliceSize);
	}
	for (int32 d = 0; d < Depth; ++d)
	{
		const int32 AxisCoord = bSiblingOnPos ? d : (CS - 1 - d);
		for (int32 b = 0; b < CS; ++b)
		{
			for (int32 a = 0; a < CS; ++a)
			{
				// (a, b) map to the two non-Axis axes in ascending order — matching PlaneIndex.
				int32 V[3];
				V[Axis] = AxisCoord;
				V[(Axis == 0) ? 1 : 0] = a;
				V[(Axis == 2) ? 1 : 2] = b;
				const FVoxelData& Vox = Src[V[0] + V[1] * CS + V[2] * CS * CS];
				const int32 InPlane = a + b * CS;
				if (d == 0)
				{
					OutPlane0[InPlane] = Vox;
				}
				else
				{
					OutDeep[(d - 1) * SliceSize + InPlane] = Vox;
				}
			}
		}
	}
}

/** Fill a diagonal edge strip + deep grid from a sibling volume. AxisA < AxisB are the pinned
 *  axes; bPosA/bPosB: sibling on the consumer's positive side of that axis. FreeAxis varies. */
static void FillEdgeStrip(
	const TArray<FVoxelData>& Src, int32 CS, int32 AxisA, bool bPosA, int32 AxisB, bool bPosB,
	int32 FreeAxis, int32 Depth,
	TArray<FVoxelData>& OutStrip, TArray<FVoxelData>& OutDeep)
{
	OutStrip.SetNumUninitialized(CS);
	if (Depth > 1)
	{
		OutDeep.SetNumUninitialized(Depth * Depth * CS);
	}
	for (int32 dA = 0; dA < Depth; ++dA)
	{
		const int32 CA = bPosA ? dA : (CS - 1 - dA);
		for (int32 dB = 0; dB < Depth; ++dB)
		{
			const int32 CB = bPosB ? dB : (CS - 1 - dB);
			for (int32 f = 0; f < CS; ++f)
			{
				int32 V[3];
				V[AxisA] = CA;
				V[AxisB] = CB;
				V[FreeAxis] = f;
				const FVoxelData& Vox = Src[V[0] + V[1] * CS + V[2] * CS * CS];
				if (dA == 0 && dB == 0)
				{
					OutStrip[f] = Vox;
				}
				if (Depth > 1)
				{
					OutDeep[(dA * Depth + dB) * CS + f] = Vox;
				}
			}
		}
	}
}

/** Fill a diagonal corner voxel + deep box from a sibling volume. */
static void FillCorner(
	const TArray<FVoxelData>& Src, int32 CS, bool bPosX, bool bPosY, bool bPosZ, int32 Depth,
	FVoxelData& OutScalar, TArray<FVoxelData>& OutDeep)
{
	if (Depth > 1)
	{
		OutDeep.SetNumUninitialized(Depth * Depth * Depth);
	}
	for (int32 a = 0; a < Depth; ++a)
	{
		const int32 CX = bPosX ? a : (CS - 1 - a);
		for (int32 b = 0; b < Depth; ++b)
		{
			const int32 CY = bPosY ? b : (CS - 1 - b);
			for (int32 c = 0; c < Depth; ++c)
			{
				const int32 CZ = bPosZ ? c : (CS - 1 - c);
				const FVoxelData& Vox = Src[CX + CY * CS + CZ * CS * CS];
				if (a == 0 && b == 0 && c == 0)
				{
					OutScalar = Vox;
				}
				if (Depth > 1)
				{
					OutDeep[(a * Depth + b) * Depth + c] = Vox;
				}
			}
		}
	}
}

/** Select the request's face slice/deep arrays for (Axis, bPos). */
static void SelectFaceArrays(FVoxelMeshingRequest& R, int32 Axis, bool bPos,
	TArray<FVoxelData>*& OutPlane, TArray<FVoxelData>*& OutDeep)
{
	switch (Axis * 2 + (bPos ? 1 : 0))
	{
	case 0: OutPlane = &R.NeighborXNeg; OutDeep = &R.NeighborXNegDeep; break;
	case 1: OutPlane = &R.NeighborXPos; OutDeep = &R.NeighborXPosDeep; break;
	case 2: OutPlane = &R.NeighborYNeg; OutDeep = &R.NeighborYNegDeep; break;
	case 3: OutPlane = &R.NeighborYPos; OutDeep = &R.NeighborYPosDeep; break;
	case 4: OutPlane = &R.NeighborZNeg; OutDeep = &R.NeighborZNegDeep; break;
	default: OutPlane = &R.NeighborZPos; OutDeep = &R.NeighborZPosDeep; break;
	}
}

/** Select the request's edge strip/deep arrays + presence flag for canonical (AxisA<AxisB, signs). */
static void SelectEdgeArrays(FVoxelMeshingRequest& R, int32 AxisA, bool bPosA, int32 AxisB, bool bPosB,
	TArray<FVoxelData>*& OutStrip, TArray<FVoxelData>*& OutDeep, uint32& OutFlag)
{
	check(AxisA < AxisB);
	const int32 Pair = (AxisA == 0) ? ((AxisB == 1) ? 0 : 1) : 2; // XY, XZ, YZ
	const int32 Sel = Pair * 4 + (bPosA ? 0 : 2) + (bPosB ? 0 : 1);
	switch (Sel)
	{
	case 0:  OutStrip = &R.EdgeXPosYPos; OutDeep = &R.EdgeXPosYPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_XPOS_YPOS; break;
	case 1:  OutStrip = &R.EdgeXPosYNeg; OutDeep = &R.EdgeXPosYNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_XPOS_YNEG; break;
	case 2:  OutStrip = &R.EdgeXNegYPos; OutDeep = &R.EdgeXNegYPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_XNEG_YPOS; break;
	case 3:  OutStrip = &R.EdgeXNegYNeg; OutDeep = &R.EdgeXNegYNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_XNEG_YNEG; break;
	case 4:  OutStrip = &R.EdgeXPosZPos; OutDeep = &R.EdgeXPosZPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_XPOS_ZPOS; break;
	case 5:  OutStrip = &R.EdgeXPosZNeg; OutDeep = &R.EdgeXPosZNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_XPOS_ZNEG; break;
	case 6:  OutStrip = &R.EdgeXNegZPos; OutDeep = &R.EdgeXNegZPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_XNEG_ZPOS; break;
	case 7:  OutStrip = &R.EdgeXNegZNeg; OutDeep = &R.EdgeXNegZNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_XNEG_ZNEG; break;
	case 8:  OutStrip = &R.EdgeYPosZPos; OutDeep = &R.EdgeYPosZPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_YPOS_ZPOS; break;
	case 9:  OutStrip = &R.EdgeYPosZNeg; OutDeep = &R.EdgeYPosZNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_YPOS_ZNEG; break;
	case 10: OutStrip = &R.EdgeYNegZPos; OutDeep = &R.EdgeYNegZPosDeep; OutFlag = FVoxelMeshingRequest::EDGE_YNEG_ZPOS; break;
	default: OutStrip = &R.EdgeYNegZNeg; OutDeep = &R.EdgeYNegZNegDeep; OutFlag = FVoxelMeshingRequest::EDGE_YNEG_ZNEG; break;
	}
}

/** Select the request's corner scalar/deep + presence flag for the sign triple. */
static void SelectCornerArrays(FVoxelMeshingRequest& R, bool bPosX, bool bPosY, bool bPosZ,
	FVoxelData*& OutScalar, TArray<FVoxelData>*& OutDeep, uint32& OutFlag)
{
	const int32 Sel = (bPosX ? 0 : 4) + (bPosY ? 0 : 2) + (bPosZ ? 0 : 1);
	switch (Sel)
	{
	case 0:  OutScalar = &R.CornerXPosYPosZPos; OutDeep = &R.CornerXPosYPosZPosDeep; OutFlag = FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS; break;
	case 1:  OutScalar = &R.CornerXPosYPosZNeg; OutDeep = &R.CornerXPosYPosZNegDeep; OutFlag = FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG; break;
	case 2:  OutScalar = &R.CornerXPosYNegZPos; OutDeep = &R.CornerXPosYNegZPosDeep; OutFlag = FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS; break;
	case 3:  OutScalar = &R.CornerXPosYNegZNeg; OutDeep = &R.CornerXPosYNegZNegDeep; OutFlag = FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG; break;
	case 4:  OutScalar = &R.CornerXNegYPosZPos; OutDeep = &R.CornerXNegYPosZPosDeep; OutFlag = FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS; break;
	case 5:  OutScalar = &R.CornerXNegYPosZNeg; OutDeep = &R.CornerXNegYPosZNegDeep; OutFlag = FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG; break;
	case 6:  OutScalar = &R.CornerXNegYNegZPos; OutDeep = &R.CornerXNegYNegZPosDeep; OutFlag = FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS; break;
	default: OutScalar = &R.CornerXNegYNegZNeg; OutDeep = &R.CornerXNegYNegZNegDeep; OutFlag = FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG; break;
	}
}

/** Base synthetic request for one participant frame. */
static void InitFrameRequest(FVoxelMeshingRequest& R, const FIntVector& Coord, int32 LOD,
	int32 CS, float VoxelSize, const FVector& WorldOrigin, const TArray<FVoxelData>& Volume)
{
	R.ChunkCoord = Coord;
	R.LODLevel = LOD;
	R.ChunkSize = CS;
	R.VoxelSize = VoxelSize;
	R.WorldOrigin = WorldOrigin;
	R.VoxelData = Volume; // per-frame copy (worker-side; seam jobs are few and throttled)
	for (int32 i = 0; i < 6; ++i)
	{
		R.NeighborLODLevels[i] = LOD;
	}
	R.TransitionFaces = 0;
	R.EdgeCornerFlags = 0;
	R.MeshCellDomain = EVoxelMeshCellDomain::Full;
}

/** Translate a vertex range into the owner's local frame. */
static void TranslateRange(FChunkMeshData& Mesh, int32 FirstVertex, const FVector3f& Offset)
{
	if (Offset.IsNearlyZero())
	{
		return;
	}
	for (int32 i = FirstVertex; i < Mesh.Positions.Num(); ++i)
	{
		Mesh.Positions[i] += Offset;
	}
}

} // namespace VoxelMCSeamDetail

using namespace VoxelMCSeamDetail;

void FVoxelCPUMarchingCubesMesher::MeshSeamBandCells(
	const FVoxelMeshingRequest& SyntheticRequest,
	const FIntVector& BandMin,
	const FIntVector& BandMaxEx,
	uint8 TransitionMask,
	FChunkMeshData& OutMeshData,
	uint32& TriangleCount)
{
	const int32 Stride = 1 << FMath::Clamp(SyntheticRequest.LODLevel, 0, 7);
	for (int32 Z = BandMin.Z; Z < BandMaxEx.Z; Z += Stride)
	{
		for (int32 Y = BandMin.Y; Y < BandMaxEx.Y; Y += Stride)
		{
			for (int32 X = BandMin.X; X < BandMaxEx.X; X += Stride)
			{
				ProcessCubeLOD(SyntheticRequest, X, Y, Z, Stride, OutMeshData, TriangleCount,
					FColor(0, 0, 0, 0), TransitionMask);
			}
		}
	}
}

bool FVoxelCPUMarchingCubesMesher::GenerateFaceSeamMeshCPU(
	const FVoxelFaceSeamRequest& SeamRequest, FChunkMeshData& OutMeshData)
{
	if (!bIsInitialized || !SeamRequest.IsValid())
	{
		return false;
	}
	OutMeshData.Reset();
	uint32 TriangleCount = 0;

	const int32 CS = SeamRequest.ChunkSize;
	const float VoxelSize = SeamRequest.VoxelSize;
	const int32 Axis = SeamRequest.Axis;
	const int32 LODA = SeamRequest.LODLevel;
	const int32 LODB = SeamRequest.GetLODLevelB();

	struct FSide { FIntVector Coord; int32 LOD; const TArray<FVoxelData>* Vol; bool bFacingPos; };
	FIntVector BCoord = SeamRequest.OwnerChunkCoord;
	BCoord[Axis] += 1;
	const FSide Sides[2] = {
		{ SeamRequest.OwnerChunkCoord, LODA, SeamRequest.VoxelDataA.Get(), true  },
		{ BCoord,                      LODB, SeamRequest.VoxelDataB.Get(), false },
	};

	for (int32 SideIdx = 0; SideIdx < 2; ++SideIdx)
	{
		const FSide& P = Sides[SideIdx];
		const FSide& Other = Sides[1 - SideIdx];
		const int32 S = 1 << P.LOD;
		const int32 EOther = 1 << Other.LOD;
		const int32 Depth = FMath::Clamp(FMath::Max(S, 2 * EOther) + 1, 1, CS);

		FVoxelMeshingRequest R;
		InitFrameRequest(R, P.Coord, P.LOD, CS, VoxelSize, SeamRequest.WorldOrigin, *P.Vol);
		R.NeighborPlaneDepth = Depth;
		TArray<FVoxelData>* Plane; TArray<FVoxelData>* Deep;
		SelectFaceArrays(R, Axis, P.bFacingPos, Plane, Deep);
		FillFacePlanes(*Other.Vol, CS, Axis, P.bFacingPos, Depth, *Plane, *Deep);
		const int32 FacingFaceIdx = Axis * 2 + (P.bFacingPos ? 1 : 0);
		R.NeighborLODLevels[FacingFaceIdx] = Other.LOD;
		uint8 Mask = 0;
		if (Other.LOD > P.LOD)
		{
			Mask = 1 << FacingFaceIdx;
			R.TransitionFaces = Mask;
			// Band-confined geomorph ramp: zero at the band's inner edge (depth S voxels),
			// expressed in the coarse neighbor's cells.
			R.MorphWidthOverride = static_cast<float>(S) / static_cast<float>(EOther);
		}

		FIntVector BandMin(S, S, S), BandMaxEx(CS - S, CS - S, CS - S);
		BandMin[Axis] = P.bFacingPos ? (CS - S) : 0;
		BandMaxEx[Axis] = BandMin[Axis] + S;

		const int32 FirstVertex = OutMeshData.Positions.Num();
		MeshSeamBandCells(R, BandMin, BandMaxEx, Mask, OutMeshData, TriangleCount);

		// Mixed LOD: the finer side emits the transvoxel ribbon (full face; cells needing data
		// beyond the two participants self-skip via HasRequiredNeighborData).
		if (Other.LOD > P.LOD)
		{
			const int32 CoarserStride = EOther;
			const int32 BoundaryPos = P.bFacingPos ? (CS - S) : 0;
			for (int32 FP2 = 0; FP2 < CS; FP2 += CoarserStride)
			{
				for (int32 FP1 = 0; FP1 < CS; FP1 += CoarserStride)
				{
					int32 CellX, CellY, CellZ;
					switch (Axis)
					{
					case 0:  CellX = BoundaryPos; CellY = FP1; CellZ = FP2; break;
					case 1:  CellX = FP1; CellY = BoundaryPos; CellZ = FP2; break;
					default: CellX = FP1; CellY = FP2; CellZ = BoundaryPos; break;
					}
					ProcessTransitionCell(R, CellX, CellY, CellZ, CoarserStride, FacingFaceIdx,
						OutMeshData, TriangleCount);
				}
			}
		}

		const FIntVector DeltaP = P.Coord - SeamRequest.OwnerChunkCoord;
		TranslateRange(OutMeshData, FirstVertex,
			FVector3f(DeltaP.X, DeltaP.Y, DeltaP.Z) * static_cast<float>(CS) * VoxelSize);
	}
	return true;
}

bool FVoxelCPUMarchingCubesMesher::GenerateEdgeSeamMeshCPU(
	const FVoxelEdgeSeamRequest& SeamRequest, FChunkMeshData& OutMeshData)
{
	if (!bIsInitialized || !SeamRequest.IsValid())
	{
		return false;
	}
	OutMeshData.Reset();
	uint32 TriangleCount = 0;

	const int32 CS = SeamRequest.ChunkSize;
	const float VoxelSize = SeamRequest.VoxelSize;
	const int32 EdgeAxis = SeamRequest.EdgeAxis;
	const int32 P1 = (EdgeAxis == 0) ? 1 : 0;
	const int32 P2 = (EdgeAxis == 2) ? 1 : 2;

	for (int32 q = 0; q < 4; ++q)
	{
		const int32 qa = q & 1;
		const int32 qb = q >> 1;
		FIntVector Coord = SeamRequest.OwnerChunkCoord;
		Coord[P1] += qa;
		Coord[P2] += qb;
		const int32 LOD = SeamRequest.GetLODLevelOf(q);
		const int32 S = 1 << LOD;
		const int32 LodP1 = SeamRequest.GetLODLevelOf(q ^ 1);
		const int32 LodP2 = SeamRequest.GetLODLevelOf(q ^ 2);
		const int32 MaxE = FMath::Max3(S, 2 * (1 << LodP1), 2 * (1 << LodP2));
		const int32 Depth = FMath::Clamp(MaxE + 1, 1, CS);
		const bool bPosP1 = (qa == 0);
		const bool bPosP2 = (qb == 0);

		FVoxelMeshingRequest R;
		InitFrameRequest(R, Coord, LOD, CS, VoxelSize, SeamRequest.WorldOrigin, *SeamRequest.VoxelData[q]);
		R.NeighborPlaneDepth = Depth;

		TArray<FVoxelData>* Plane; TArray<FVoxelData>* Deep;
		SelectFaceArrays(R, P1, bPosP1, Plane, Deep);
		FillFacePlanes(*SeamRequest.VoxelData[q ^ 1], CS, P1, bPosP1, Depth, *Plane, *Deep);
		SelectFaceArrays(R, P2, bPosP2, Plane, Deep);
		FillFacePlanes(*SeamRequest.VoxelData[q ^ 2], CS, P2, bPosP2, Depth, *Plane, *Deep);

		TArray<FVoxelData>* Strip; uint32 Flag;
		const int32 AxisA = FMath::Min(P1, P2);
		const int32 AxisB = FMath::Max(P1, P2);
		const bool bPosA = (AxisA == P1) ? bPosP1 : bPosP2;
		const bool bPosB = (AxisB == P2) ? bPosP2 : bPosP1;
		SelectEdgeArrays(R, AxisA, bPosA, AxisB, bPosB, Strip, Deep, Flag);
		FillEdgeStrip(*SeamRequest.VoxelData[q ^ 3], CS, AxisA, bPosA, AxisB, bPosB, EdgeAxis, Depth, *Strip, *Deep);
		R.EdgeCornerFlags |= Flag;

		const int32 FaceP1 = P1 * 2 + (bPosP1 ? 1 : 0);
		const int32 FaceP2 = P2 * 2 + (bPosP2 ? 1 : 0);
		R.NeighborLODLevels[FaceP1] = LodP1;
		R.NeighborLODLevels[FaceP2] = LodP2;
		uint8 Mask = 0;
		int32 MaxCoarser = 0;
		if (LodP1 > LOD) { Mask |= (1 << FaceP1); MaxCoarser = FMath::Max(MaxCoarser, 1 << LodP1); }
		if (LodP2 > LOD) { Mask |= (1 << FaceP2); MaxCoarser = FMath::Max(MaxCoarser, 1 << LodP2); }
		if (Mask)
		{
			R.TransitionFaces = Mask;
			R.MorphWidthOverride = static_cast<float>(S) / static_cast<float>(MaxCoarser);
		}

		FIntVector BandMin(S, S, S), BandMaxEx(CS - S, CS - S, CS - S);
		BandMin[P1] = bPosP1 ? (CS - S) : 0;
		BandMaxEx[P1] = BandMin[P1] + S;
		BandMin[P2] = bPosP2 ? (CS - S) : 0;
		BandMaxEx[P2] = BandMin[P2] + S;

		const int32 FirstVertex = OutMeshData.Positions.Num();
		MeshSeamBandCells(R, BandMin, BandMaxEx, Mask, OutMeshData, TriangleCount);
		const FIntVector Delta = Coord - SeamRequest.OwnerChunkCoord;
		TranslateRange(OutMeshData, FirstVertex,
			FVector3f(Delta.X, Delta.Y, Delta.Z) * static_cast<float>(CS) * VoxelSize);
	}
	return true;
}

bool FVoxelCPUMarchingCubesMesher::GenerateCornerSeamMeshCPU(
	const FVoxelCornerSeamRequest& SeamRequest, FChunkMeshData& OutMeshData)
{
	if (!bIsInitialized || !SeamRequest.IsValid())
	{
		return false;
	}
	OutMeshData.Reset();
	uint32 TriangleCount = 0;

	const int32 CS = SeamRequest.ChunkSize;
	const float VoxelSize = SeamRequest.VoxelSize;

	for (int32 o = 0; o < 8; ++o)
	{
		const int32 dx = o & 1;
		const int32 dy = (o >> 1) & 1;
		const int32 dz = (o >> 2) & 1;
		const FIntVector Coord = SeamRequest.OwnerChunkCoord + FIntVector(dx, dy, dz);
		const int32 LOD = SeamRequest.GetLODLevelOf(o);
		const int32 S = 1 << LOD;
		const bool bPos[3] = { dx == 0, dy == 0, dz == 0 };

		int32 MaxCoarser = S;
		int32 FaceLods[3];
		for (int32 A = 0; A < 3; ++A)
		{
			FaceLods[A] = SeamRequest.GetLODLevelOf(o ^ (1 << A));
			MaxCoarser = FMath::Max(MaxCoarser, 2 * (1 << FaceLods[A]));
		}
		const int32 Depth = FMath::Clamp(MaxCoarser + 1, 1, CS);

		FVoxelMeshingRequest R;
		InitFrameRequest(R, Coord, LOD, CS, VoxelSize, SeamRequest.WorldOrigin, *SeamRequest.VoxelData[o]);
		R.NeighborPlaneDepth = Depth;

		TArray<FVoxelData>* Plane; TArray<FVoxelData>* Deep; uint32 Flag;
		uint8 Mask = 0;
		int32 MaxCoarserStride = 0;
		for (int32 A = 0; A < 3; ++A)
		{
			SelectFaceArrays(R, A, bPos[A], Plane, Deep);
			FillFacePlanes(*SeamRequest.VoxelData[o ^ (1 << A)], CS, A, bPos[A], Depth, *Plane, *Deep);
			const int32 FaceIdx = A * 2 + (bPos[A] ? 1 : 0);
			R.NeighborLODLevels[FaceIdx] = FaceLods[A];
			if (FaceLods[A] > LOD)
			{
				Mask |= (1 << FaceIdx);
				MaxCoarserStride = FMath::Max(MaxCoarserStride, 1 << FaceLods[A]);
			}
		}
		// The three diagonal edge strips (sibling across both axes of each pair).
		const int32 Pairs[3][2] = { {0, 1}, {0, 2}, {1, 2} };
		for (int32 pi = 0; pi < 3; ++pi)
		{
			const int32 AxisA = Pairs[pi][0];
			const int32 AxisB = Pairs[pi][1];
			const int32 FreeAxis = 3 - AxisA - AxisB;
			const int32 Sibling = o ^ (1 << AxisA) ^ (1 << AxisB);
			TArray<FVoxelData>* Strip;
			SelectEdgeArrays(R, AxisA, bPos[AxisA], AxisB, bPos[AxisB], Strip, Deep, Flag);
			FillEdgeStrip(*SeamRequest.VoxelData[Sibling], CS, AxisA, bPos[AxisA], AxisB, bPos[AxisB],
				FreeAxis, Depth, *Strip, *Deep);
			R.EdgeCornerFlags |= Flag;
		}
		// The diagonal corner voxel.
		{
			FVoxelData* Scalar;
			SelectCornerArrays(R, bPos[0], bPos[1], bPos[2], Scalar, Deep, Flag);
			FillCorner(*SeamRequest.VoxelData[o ^ 7], CS, bPos[0], bPos[1], bPos[2], Depth, *Scalar, *Deep);
			R.EdgeCornerFlags |= Flag;
		}
		if (Mask)
		{
			R.TransitionFaces = Mask;
			R.MorphWidthOverride = static_cast<float>(S) / static_cast<float>(MaxCoarserStride);
		}

		FIntVector BandMin, BandMaxEx;
		for (int32 A = 0; A < 3; ++A)
		{
			BandMin[A] = bPos[A] ? (CS - S) : 0;
			BandMaxEx[A] = BandMin[A] + S;
		}

		const int32 FirstVertex = OutMeshData.Positions.Num();
		MeshSeamBandCells(R, BandMin, BandMaxEx, Mask, OutMeshData, TriangleCount);
		const FIntVector Delta = Coord - SeamRequest.OwnerChunkCoord;
		TranslateRange(OutMeshData, FirstVertex,
			FVector3f(Delta.X, Delta.Y, Delta.Z) * static_cast<float>(CS) * VoxelSize);
	}
	return true;
}
