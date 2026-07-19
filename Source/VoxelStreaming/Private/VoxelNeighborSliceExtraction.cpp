// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelNeighborSliceExtraction.h"
#include "VoxelMeshingTypes.h"

namespace VoxelNeighborSlices
{

void Extract(
	int32 ChunkSize,
	const FIntVector& ChunkCoord,
	bool bDeepOff,
	bool bDeepFull,
	TFunctionRef<bool(const FIntVector&)> HasNeighborData,
	TFunctionRef<FVoxelData(const FIntVector&, int32, int32, int32)> GetNeighborVoxel,
	FVoxelMeshingRequest& OutRequest)
{
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Reset edge/corner flags
	OutRequest.EdgeCornerFlags = 0;

	// ---- Deep neighbor planes (smooth meshers at LOD > 0) ----
	// A strided boundary cell reaches `stride` voxels into the neighbor and its
	// gradient normals reach ~2*stride; one face plane only suffices at stride 1.
	// For LOD > 0 we additionally supply (2*stride - 1) deeper planes per face so
	// Dual Contouring's outward boundary cell computes identically to the inward
	// neighbor (watertight), and Marching Cubes gets correct boundary normals. LOD 0
	// keeps a single plane (no extra cost). Capped so the source index stays in range.
	const int32 MeshStride = 1 << FMath::Clamp(OutRequest.LODLevel, 0, 7);
	// Total deep planes incl. plane 0. Default stride+1 = geometry-only: the outward DC boundary
	// cell reaches `stride` voxels deep (watertight), plus one plane for one-sided boundary normals.
	// -VoxelDeepFull restores 2*stride (adds central-difference normal reach at higher per-job cost,
	// ~14% slower catch-up at v6000); -VoxelDeepOff drops to 1 plane (no deep data, loses the seam fix).
	int32 DeepDepth;
	if (MeshStride <= 1 || bDeepOff) { DeepDepth = 1; }
	else if (bDeepFull)             { DeepDepth = 2 * MeshStride; }
	else                                 { DeepDepth = MeshStride + 1; }
	const int32 ExtraPlanes = FMath::Clamp(DeepDepth - 1, 0, ChunkSize - 1);
	OutRequest.NeighborPlaneDepth = ExtraPlanes + 1;

	// Fill a Deep array with planes one voxel deeper than the face slice (plane 0).
	// Axis: 0=X face, 1=Y face, 2=Z face; bNeg selects the -axis neighbor. Only fills
	// when the neighbor has data (caller guards) and ExtraPlanes > 0. In-plane index
	// (a + b*ChunkSize) matches the plane-0 layout for that face.
	auto FillDeep = [ChunkSize, SliceSize, ExtraPlanes, &GetNeighborVoxel]
		(TArray<FVoxelData>& DeepArr, const FIntVector& NCoord, int32 Axis, bool bNeg)
	{
		if (ExtraPlanes <= 0) { return; }
		DeepArr.SetNumUninitialized(ExtraPlanes * SliceSize);
		for (int32 k = 0; k < ExtraPlanes; ++k)
		{
			const int32 D = bNeg ? (ChunkSize - 2 - k) : (1 + k);
			for (int32 b = 0; b < ChunkSize; ++b)
			{
				for (int32 a = 0; a < ChunkSize; ++a)
				{
					const int32 NX = (Axis == 0) ? D : a;
					const int32 NY = (Axis == 1) ? D : ((Axis == 0) ? a : b);
					const int32 NZ = (Axis == 2) ? D : b;
					DeepArr[k * SliceSize + a + b * ChunkSize] = GetNeighborVoxel(NCoord, NX, NY, NZ);
				}
			}
		}
	};

	// Deep edge data: the 2-axis outward DC boundary cell reaches diagonally into the edge
	// neighbor, so fill the full NeighborPlaneDepth^2 grid of free-axis strips. AxisA/AxisB
	// are the two pinned axes (0=X,1=Y,2=Z), bNegA/bNegB select the negative neighbor, and
	// FreeAxis is the remaining (in-range) axis. Layout matches FVoxelMeshingRequest::EdgeDeepVoxel:
	// ((dA * D) + dB) * ChunkSize + free, with (0,0) == the base Edge* strip.
	const int32 DeepN = OutRequest.NeighborPlaneDepth; // == ExtraPlanes + 1
	auto FillEdgeDeep = [ChunkSize, DeepN, &GetNeighborVoxel]
		(TArray<FVoxelData>& DeepArr, const FIntVector& NCoord,
		 int32 AxisA, bool bNegA, int32 AxisB, bool bNegB, int32 FreeAxis)
	{
		if (DeepN <= 1) { return; } // LOD0: the single base strip is sufficient
		const int32 D = DeepN;
		DeepArr.SetNumUninitialized(D * D * ChunkSize);
		for (int32 a = 0; a < D; ++a)
		{
			const int32 CA = bNegA ? (ChunkSize - 1 - a) : a;
			for (int32 b = 0; b < D; ++b)
			{
				const int32 CB = bNegB ? (ChunkSize - 1 - b) : b;
				for (int32 f = 0; f < ChunkSize; ++f)
				{
					int32 C[3] = { 0, 0, 0 };
					C[AxisA] = CA; C[AxisB] = CB; C[FreeAxis] = f;
					DeepArr[(a * D + b) * ChunkSize + f] = GetNeighborVoxel(NCoord, C[0], C[1], C[2]);
				}
			}
		}
	};

	// Deep corner data: the 3-axis outward DC boundary cell reaches along the body diagonal
	// into the corner neighbor, so fill the full NeighborPlaneDepth^3 box. Layout matches
	// FVoxelMeshingRequest::CornerDeepVoxel: ((dX * D) + dY) * D + dZ, with (0,0,0) == the
	// scalar Corner* voxel.
	auto FillCornerDeep = [ChunkSize, DeepN, &GetNeighborVoxel]
		(TArray<FVoxelData>& DeepArr, const FIntVector& NCoord, bool bNegX, bool bNegY, bool bNegZ)
	{
		if (DeepN <= 1) { return; }
		const int32 D = DeepN;
		DeepArr.SetNumUninitialized(D * D * D);
		for (int32 a = 0; a < D; ++a)
		{
			const int32 CX = bNegX ? (ChunkSize - 1 - a) : a;
			for (int32 b = 0; b < D; ++b)
			{
				const int32 CY = bNegY ? (ChunkSize - 1 - b) : b;
				for (int32 c = 0; c < D; ++c)
				{
					const int32 CZ = bNegZ ? (ChunkSize - 1 - c) : c;
					DeepArr[(a * D + b) * D + c] = GetNeighborVoxel(NCoord, CX, CY, CZ);
				}
			}
		}
	};

	// ==================== Extract Face Neighbors ====================

	// +X neighbor (extract X=0 slice from neighbor)
	FIntVector NeighborXPosCoord = ChunkCoord + FIntVector(1, 0, 0);
	if (HasNeighborData(NeighborXPosCoord))
	{
		OutRequest.NeighborXPos.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				OutRequest.NeighborXPos[Y + Z * ChunkSize] = GetNeighborVoxel(NeighborXPosCoord, 0, Y, Z);
			}
		}
		FillDeep(OutRequest.NeighborXPosDeep, NeighborXPosCoord, 0, false);
	}

	// -X neighbor (extract X=ChunkSize-1 slice from neighbor)
	FIntVector NeighborXNegCoord = ChunkCoord + FIntVector(-1, 0, 0);
	if (HasNeighborData(NeighborXNegCoord))
	{
		OutRequest.NeighborXNeg.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				OutRequest.NeighborXNeg[Y + Z * ChunkSize] = GetNeighborVoxel(NeighborXNegCoord, ChunkSize - 1, Y, Z);
			}
		}
		FillDeep(OutRequest.NeighborXNegDeep, NeighborXNegCoord, 0, true);
	}

	// +Y neighbor (extract Y=0 slice from neighbor)
	FIntVector NeighborYPosCoord = ChunkCoord + FIntVector(0, 1, 0);
	if (HasNeighborData(NeighborYPosCoord))
	{
		OutRequest.NeighborYPos.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborYPos[X + Z * ChunkSize] = GetNeighborVoxel(NeighborYPosCoord, X, 0, Z);
			}
		}
		FillDeep(OutRequest.NeighborYPosDeep, NeighborYPosCoord, 1, false);
	}

	// -Y neighbor (extract Y=ChunkSize-1 slice from neighbor)
	FIntVector NeighborYNegCoord = ChunkCoord + FIntVector(0, -1, 0);
	if (HasNeighborData(NeighborYNegCoord))
	{
		OutRequest.NeighborYNeg.SetNumUninitialized(SliceSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborYNeg[X + Z * ChunkSize] = GetNeighborVoxel(NeighborYNegCoord, X, ChunkSize - 1, Z);
			}
		}
		FillDeep(OutRequest.NeighborYNegDeep, NeighborYNegCoord, 1, true);
	}

	// +Z neighbor (extract Z=0 slice from neighbor)
	FIntVector NeighborZPosCoord = ChunkCoord + FIntVector(0, 0, 1);
	if (HasNeighborData(NeighborZPosCoord))
	{
		OutRequest.NeighborZPos.SetNumUninitialized(SliceSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborZPos[X + Y * ChunkSize] = GetNeighborVoxel(NeighborZPosCoord, X, Y, 0);
			}
		}
		FillDeep(OutRequest.NeighborZPosDeep, NeighborZPosCoord, 2, false);
	}

	// -Z neighbor (extract Z=ChunkSize-1 slice from neighbor)
	FIntVector NeighborZNegCoord = ChunkCoord + FIntVector(0, 0, -1);
	if (HasNeighborData(NeighborZNegCoord))
	{
		OutRequest.NeighborZNeg.SetNumUninitialized(SliceSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				OutRequest.NeighborZNeg[X + Y * ChunkSize] = GetNeighborVoxel(NeighborZNegCoord, X, Y, ChunkSize - 1);
			}
		}
		FillDeep(OutRequest.NeighborZNegDeep, NeighborZNegCoord, 2, true);
	}

	// ==================== Extract Edge Neighbors (for Marching Cubes) ====================

	// Edge X+Y+ (diagonal chunk at +X+Y, extract X=0, Y=0, Z varies)
	FIntVector EdgeXPosYPos = ChunkCoord + FIntVector(1, 1, 0);
	if (HasNeighborData(EdgeXPosYPos))
	{
		OutRequest.EdgeXPosYPos.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXPosYPos[Z] = GetNeighborVoxel(EdgeXPosYPos, 0, 0, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_YPOS;
		FillEdgeDeep(OutRequest.EdgeXPosYPosDeep, EdgeXPosYPos, 0, false, 1, false, 2);
	}

	// Edge X+Y- (diagonal chunk at +X-Y, extract X=0, Y=ChunkSize-1, Z varies)
	FIntVector EdgeXPosYNeg = ChunkCoord + FIntVector(1, -1, 0);
	if (HasNeighborData(EdgeXPosYNeg))
	{
		OutRequest.EdgeXPosYNeg.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXPosYNeg[Z] = GetNeighborVoxel(EdgeXPosYNeg, 0, ChunkSize - 1, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_YNEG;
		FillEdgeDeep(OutRequest.EdgeXPosYNegDeep, EdgeXPosYNeg, 0, false, 1, true, 2);
	}

	// Edge X-Y+ (diagonal chunk at -X+Y, extract X=ChunkSize-1, Y=0, Z varies)
	FIntVector EdgeXNegYPos = ChunkCoord + FIntVector(-1, 1, 0);
	if (HasNeighborData(EdgeXNegYPos))
	{
		OutRequest.EdgeXNegYPos.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXNegYPos[Z] = GetNeighborVoxel(EdgeXNegYPos, ChunkSize - 1, 0, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_YPOS;
		FillEdgeDeep(OutRequest.EdgeXNegYPosDeep, EdgeXNegYPos, 0, true, 1, false, 2);
	}

	// Edge X-Y- (diagonal chunk at -X-Y, extract X=ChunkSize-1, Y=ChunkSize-1, Z varies)
	FIntVector EdgeXNegYNeg = ChunkCoord + FIntVector(-1, -1, 0);
	if (HasNeighborData(EdgeXNegYNeg))
	{
		OutRequest.EdgeXNegYNeg.SetNumUninitialized(ChunkSize);
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			OutRequest.EdgeXNegYNeg[Z] = GetNeighborVoxel(EdgeXNegYNeg, ChunkSize - 1, ChunkSize - 1, Z);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_YNEG;
		FillEdgeDeep(OutRequest.EdgeXNegYNegDeep, EdgeXNegYNeg, 0, true, 1, true, 2);
	}

	// Edge X+Z+ (diagonal chunk at +X+Z, extract X=0, Z=0, Y varies)
	FIntVector EdgeXPosZPos = ChunkCoord + FIntVector(1, 0, 1);
	if (HasNeighborData(EdgeXPosZPos))
	{
		OutRequest.EdgeXPosZPos.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXPosZPos[Y] = GetNeighborVoxel(EdgeXPosZPos, 0, Y, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_ZPOS;
		FillEdgeDeep(OutRequest.EdgeXPosZPosDeep, EdgeXPosZPos, 0, false, 2, false, 1);
	}

	// Edge X+Z- (diagonal chunk at +X-Z, extract X=0, Z=ChunkSize-1, Y varies)
	FIntVector EdgeXPosZNeg = ChunkCoord + FIntVector(1, 0, -1);
	if (HasNeighborData(EdgeXPosZNeg))
	{
		OutRequest.EdgeXPosZNeg.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXPosZNeg[Y] = GetNeighborVoxel(EdgeXPosZNeg, 0, Y, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XPOS_ZNEG;
		FillEdgeDeep(OutRequest.EdgeXPosZNegDeep, EdgeXPosZNeg, 0, false, 2, true, 1);
	}

	// Edge X-Z+ (diagonal chunk at -X+Z, extract X=ChunkSize-1, Z=0, Y varies)
	FIntVector EdgeXNegZPos = ChunkCoord + FIntVector(-1, 0, 1);
	if (HasNeighborData(EdgeXNegZPos))
	{
		OutRequest.EdgeXNegZPos.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXNegZPos[Y] = GetNeighborVoxel(EdgeXNegZPos, ChunkSize - 1, Y, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_ZPOS;
		FillEdgeDeep(OutRequest.EdgeXNegZPosDeep, EdgeXNegZPos, 0, true, 2, false, 1);
	}

	// Edge X-Z- (diagonal chunk at -X-Z, extract X=ChunkSize-1, Z=ChunkSize-1, Y varies)
	FIntVector EdgeXNegZNeg = ChunkCoord + FIntVector(-1, 0, -1);
	if (HasNeighborData(EdgeXNegZNeg))
	{
		OutRequest.EdgeXNegZNeg.SetNumUninitialized(ChunkSize);
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			OutRequest.EdgeXNegZNeg[Y] = GetNeighborVoxel(EdgeXNegZNeg, ChunkSize - 1, Y, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_XNEG_ZNEG;
		FillEdgeDeep(OutRequest.EdgeXNegZNegDeep, EdgeXNegZNeg, 0, true, 2, true, 1);
	}

	// Edge Y+Z+ (diagonal chunk at +Y+Z, extract Y=0, Z=0, X varies)
	FIntVector EdgeYPosZPos = ChunkCoord + FIntVector(0, 1, 1);
	if (HasNeighborData(EdgeYPosZPos))
	{
		OutRequest.EdgeYPosZPos.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYPosZPos[X] = GetNeighborVoxel(EdgeYPosZPos, X, 0, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YPOS_ZPOS;
		FillEdgeDeep(OutRequest.EdgeYPosZPosDeep, EdgeYPosZPos, 1, false, 2, false, 0);
	}

	// Edge Y+Z- (diagonal chunk at +Y-Z, extract Y=0, Z=ChunkSize-1, X varies)
	FIntVector EdgeYPosZNeg = ChunkCoord + FIntVector(0, 1, -1);
	if (HasNeighborData(EdgeYPosZNeg))
	{
		OutRequest.EdgeYPosZNeg.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYPosZNeg[X] = GetNeighborVoxel(EdgeYPosZNeg, X, 0, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YPOS_ZNEG;
		FillEdgeDeep(OutRequest.EdgeYPosZNegDeep, EdgeYPosZNeg, 1, false, 2, true, 0);
	}

	// Edge Y-Z+ (diagonal chunk at -Y+Z, extract Y=ChunkSize-1, Z=0, X varies)
	FIntVector EdgeYNegZPos = ChunkCoord + FIntVector(0, -1, 1);
	if (HasNeighborData(EdgeYNegZPos))
	{
		OutRequest.EdgeYNegZPos.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYNegZPos[X] = GetNeighborVoxel(EdgeYNegZPos, X, ChunkSize - 1, 0);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YNEG_ZPOS;
		FillEdgeDeep(OutRequest.EdgeYNegZPosDeep, EdgeYNegZPos, 1, true, 2, false, 0);
	}

	// Edge Y-Z- (diagonal chunk at -Y-Z, extract Y=ChunkSize-1, Z=ChunkSize-1, X varies)
	FIntVector EdgeYNegZNeg = ChunkCoord + FIntVector(0, -1, -1);
	if (HasNeighborData(EdgeYNegZNeg))
	{
		OutRequest.EdgeYNegZNeg.SetNumUninitialized(ChunkSize);
		for (int32 X = 0; X < ChunkSize; ++X)
		{
			OutRequest.EdgeYNegZNeg[X] = GetNeighborVoxel(EdgeYNegZNeg, X, ChunkSize - 1, ChunkSize - 1);
		}
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::EDGE_YNEG_ZNEG;
		FillEdgeDeep(OutRequest.EdgeYNegZNegDeep, EdgeYNegZNeg, 1, true, 2, true, 0);
	}

	// ==================== Extract Corner Neighbors (for Marching Cubes) ====================

	// Corner X+Y+Z+ (diagonal chunk at +X+Y+Z, extract voxel at 0,0,0)
	FIntVector CornerPPP = ChunkCoord + FIntVector(1, 1, 1);
	if (HasNeighborData(CornerPPP))
	{
		OutRequest.CornerXPosYPosZPos = GetNeighborVoxel(CornerPPP, 0, 0, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS;
		FillCornerDeep(OutRequest.CornerXPosYPosZPosDeep, CornerPPP, false, false, false);
	}

	// Corner X+Y+Z- (diagonal chunk at +X+Y-Z, extract voxel at 0,0,ChunkSize-1)
	FIntVector CornerPPN = ChunkCoord + FIntVector(1, 1, -1);
	if (HasNeighborData(CornerPPN))
	{
		OutRequest.CornerXPosYPosZNeg = GetNeighborVoxel(CornerPPN, 0, 0, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG;
		FillCornerDeep(OutRequest.CornerXPosYPosZNegDeep, CornerPPN, false, false, true);
	}

	// Corner X+Y-Z+ (diagonal chunk at +X-Y+Z, extract voxel at 0,ChunkSize-1,0)
	FIntVector CornerPNP = ChunkCoord + FIntVector(1, -1, 1);
	if (HasNeighborData(CornerPNP))
	{
		OutRequest.CornerXPosYNegZPos = GetNeighborVoxel(CornerPNP, 0, ChunkSize - 1, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS;
		FillCornerDeep(OutRequest.CornerXPosYNegZPosDeep, CornerPNP, false, true, false);
	}

	// Corner X+Y-Z- (diagonal chunk at +X-Y-Z, extract voxel at 0,ChunkSize-1,ChunkSize-1)
	FIntVector CornerPNN = ChunkCoord + FIntVector(1, -1, -1);
	if (HasNeighborData(CornerPNN))
	{
		OutRequest.CornerXPosYNegZNeg = GetNeighborVoxel(CornerPNN, 0, ChunkSize - 1, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG;
		FillCornerDeep(OutRequest.CornerXPosYNegZNegDeep, CornerPNN, false, true, true);
	}

	// Corner X-Y+Z+ (diagonal chunk at -X+Y+Z, extract voxel at ChunkSize-1,0,0)
	FIntVector CornerNPP = ChunkCoord + FIntVector(-1, 1, 1);
	if (HasNeighborData(CornerNPP))
	{
		OutRequest.CornerXNegYPosZPos = GetNeighborVoxel(CornerNPP, ChunkSize - 1, 0, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS;
		FillCornerDeep(OutRequest.CornerXNegYPosZPosDeep, CornerNPP, true, false, false);
	}

	// Corner X-Y+Z- (diagonal chunk at -X+Y-Z, extract voxel at ChunkSize-1,0,ChunkSize-1)
	FIntVector CornerNPN = ChunkCoord + FIntVector(-1, 1, -1);
	if (HasNeighborData(CornerNPN))
	{
		OutRequest.CornerXNegYPosZNeg = GetNeighborVoxel(CornerNPN, ChunkSize - 1, 0, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG;
		FillCornerDeep(OutRequest.CornerXNegYPosZNegDeep, CornerNPN, true, false, true);
	}

	// Corner X-Y-Z+ (diagonal chunk at -X-Y+Z, extract voxel at ChunkSize-1,ChunkSize-1,0)
	FIntVector CornerNNP = ChunkCoord + FIntVector(-1, -1, 1);
	if (HasNeighborData(CornerNNP))
	{
		OutRequest.CornerXNegYNegZPos = GetNeighborVoxel(CornerNNP, ChunkSize - 1, ChunkSize - 1, 0);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS;
		FillCornerDeep(OutRequest.CornerXNegYNegZPosDeep, CornerNNP, true, true, false);
	}

	// Corner X-Y-Z- (diagonal chunk at -X-Y-Z, extract voxel at ChunkSize-1,ChunkSize-1,ChunkSize-1)
	FIntVector CornerNNN = ChunkCoord + FIntVector(-1, -1, -1);
	if (HasNeighborData(CornerNNN))
	{
		OutRequest.CornerXNegYNegZNeg = GetNeighborVoxel(CornerNNN, ChunkSize - 1, ChunkSize - 1, ChunkSize - 1);
		OutRequest.EdgeCornerFlags |= FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG;
		FillCornerDeep(OutRequest.CornerXNegYNegZNegDeep, CornerNNN, true, true, true);
	}
}

} // namespace VoxelNeighborSlices
