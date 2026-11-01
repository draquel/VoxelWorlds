// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMeshingTypes.h"
#include "VoxelData.h"

/**
 * The smooth meshers' voxel reads over a meshing request (code review CR2, MESH-09). Inside the chunk a read is the chunk's
 * own voxel; outside it, the neighbour data the request carries as the chunk manager extracted it (face planes
 * NeighborPlaneDepth deep, edge strips, corner voxels, and at LOD > 0 the edge grids and corner boxes behind them), or a seam
 * job's volumes read in place (G63). Both meshers walk the same ladder (own voxel, a seam job's views, face planes, edges,
 * corners) under two rules, kept side by side as each mesher had them:
 *   - a neighbour the request does not carry reads as Air for Dual Contouring (a solid-to-air crossing at the chunk face,
 *     refined when the neighbour loads and the chunk re-meshes) and as the chunk's nearest own voxel for Marching Cubes
 *     ("the terrain continues");
 *   - past an edge or a corner Dual Contouring reads the deep grid or box at the read's depth (a strided cell's outward reach,
 *     which at stride 1 is the base strip or corner voxel), Marching Cubes the base strip or corner voxel whatever the depth
 *     (its cubes are inward; only its gradient taps reach there).
 * Every read is the expression the mesher had, so each mesher's meshes are bit for bit what they were.
 */
namespace VoxelMeshingSampler
{
	/** How a mesher reads outside the data the request carries. */
	struct FOutsideRule
	{
		/** A neighbour the request does not carry: true = Air, false = the chunk's nearest own voxel. */
		bool bAirWhenMissing;
		/** Past an edge or a corner: true = the deep grid or box at the read's depth, false = the base strip or corner voxel. */
		bool bDeepEdgesAndCorners;
	};

	/** Dual Contouring's rule: Air where a neighbour is missing, edges and corners at the read's depth. */
	constexpr FOutsideRule DualContouringRule{ true, true };

	/** Marching Cubes' rule: the nearest own voxel where a neighbour is missing, edges and corners one voxel deep. */
	constexpr FOutsideRule MarchingCubesRule{ false, false };

	/** The chunk's own voxel at an in-chunk position: the borrowed volume (a seam job's view, G63), else VoxelData. */
	FORCEINLINE const FVoxelData& OwnVoxel(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z)
	{
		const int32 Index = X + Y * Request.ChunkSize + Z * Request.ChunkSize * Request.ChunkSize;
		const FVoxelData* Own = Request.NeighbourSnapshotViews[13];
		return Own ? Own[Index] : Request.VoxelData[Index];
	}

	/**
	 * A voxel outside the chunk read from the neighbour's volume as the copied arrays held it: across one face at
	 * min(depth, NeighborPlaneDepth - 1) into the neighbour (NeighborPlaneVoxel's clamp), across an edge the strip one voxel
	 * deep on both pinned axes, across a corner the corner voxel. A neighbour not held reads the clamped own voxel.
	 */
	inline FVoxelData NeighbourViewVoxel(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z,
		int32 ClampedX, int32 ClampedY, int32 ClampedZ)
	{
		const int32 CS = Request.ChunkSize;
		const int32 D[3] = { X < 0 ? -1 : (X >= CS ? 1 : 0), Y < 0 ? -1 : (Y >= CS ? 1 : 0), Z < 0 ? -1 : (Z >= CS ? 1 : 0) };
		const FVoxelData* View = Request.NeighbourSnapshotViews[FVoxelMeshingRequest::VolumeViewIndex(D[0], D[1], D[2])];
		if (View == nullptr)
		{
			return OwnVoxel(Request, ClampedX, ClampedY, ClampedZ);
		}
		const int32 OutCount = (D[0] != 0 ? 1 : 0) + (D[1] != 0 ? 1 : 0) + (D[2] != 0 ? 1 : 0);
		const int32 P[3] = { X, Y, Z };
		int32 N[3];
		for (int32 A = 0; A < 3; ++A)
		{
			if (D[A] == 0)
			{
				N[A] = P[A];
				continue;
			}
			// Depth into the neighbour past this face (0 = the plane touching it); only a face plane reaches deeper.
			const int32 Depth = (OutCount == 1) ? FMath::Min(D[A] > 0 ? P[A] - CS : -P[A] - 1, FMath::Max(Request.NeighborPlaneDepth, 1) - 1) : 0;
			N[A] = D[A] > 0 ? Depth : (CS - 1 - Depth);
		}
		return View[N[0] + N[1] * CS + N[2] * CS * CS];
	}

	/**
	 * The voxel at a chunk-local position, inside the chunk or in the neighbour data the request carries.
	 *
	 * @param Request The meshing request
	 * @param X, Y, Z Voxel position (chunk-local; outside [0, ChunkSize) reads a neighbour)
	 * @param Rule The mesher's rule outside the data carried
	 * @return The voxel
	 */
	inline FVoxelData VoxelAt(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z, const FOutsideRule& Rule)
	{
		const int32 ChunkSize = Request.ChunkSize;

		if (X >= 0 && X < ChunkSize && Y >= 0 && Y < ChunkSize && Z >= 0 && Z < ChunkSize)
		{
			return OwnVoxel(Request, X, Y, Z);
		}

		const int32 ClampedX = FMath::Clamp(X, 0, ChunkSize - 1);
		const int32 ClampedY = FMath::Clamp(Y, 0, ChunkSize - 1);
		const int32 ClampedZ = FMath::Clamp(Z, 0, ChunkSize - 1);

		// A seam job reading its participants in place (G63): the voxel its copied plane, strip or corner held.
		if (Request.NeighbourSnapshotViews[13] != nullptr)
		{
			return NeighbourViewVoxel(Request, X, Y, Z, ClampedX, ClampedY, ClampedZ);
		}

		// A neighbour the request does not carry.
		auto Missing = [&]() -> FVoxelData
		{
			return Rule.bAirWhenMissing ? FVoxelData::Air() : OwnVoxel(Request, ClampedX, ClampedY, ClampedZ);
		};

		const int32 SliceSize = ChunkSize * ChunkSize;

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

		// 0-based depth into a neighbour along each out-of-bounds axis (0 = the first plane just past the chunk face).
		const int32 DepthX = bXPos ? (X - ChunkSize) : (bXNeg ? (-X - 1) : 0);
		const int32 DepthY = bYPos ? (Y - ChunkSize) : (bYNeg ? (-Y - 1) : 0);
		const int32 DepthZ = bZPos ? (Z - ChunkSize) : (bZNeg ? (-Z - 1) : 0);

		// One axis out: the face neighbour's plane at the read's depth (plane 0 the face slice, deeper planes from the Deep
		// arrays, clamped past NeighborPlaneDepth). At stride 1 / LOD 0 the depth is 0.
		if (OutCount == 1)
		{
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
			return Missing();
		}

		// Two axes out: the edge neighbour, indexed by the free (in-range) axis. Under Dual Contouring's rule the deep grid at
		// (DepthA, DepthB), a strided cell's diagonal reach (at stride 1 both depths are 0 and it is the base strip); under
		// Marching Cubes' the base strip.
		if (OutCount == 2)
		{
			auto Edge = [&](const TArray<FVoxelData>& Base, const TArray<FVoxelData>& Deep, int32 Free, int32 DepthA, int32 DepthB) -> FVoxelData
			{
				return Rule.bDeepEdgesAndCorners ? Request.EdgeDeepVoxel(Base, Deep, Free, DepthA, DepthB) : Base[Free];
			};

			// XY edges (Z free)
			if (bXPos && bYPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YPOS))
			{
				return Edge(Request.EdgeXPosYPos, Request.EdgeXPosYPosDeep, ClampedZ, DepthX, DepthY);
			}
			if (bXPos && bYNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_YNEG))
			{
				return Edge(Request.EdgeXPosYNeg, Request.EdgeXPosYNegDeep, ClampedZ, DepthX, DepthY);
			}
			if (bXNeg && bYPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YPOS))
			{
				return Edge(Request.EdgeXNegYPos, Request.EdgeXNegYPosDeep, ClampedZ, DepthX, DepthY);
			}
			if (bXNeg && bYNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_YNEG))
			{
				return Edge(Request.EdgeXNegYNeg, Request.EdgeXNegYNegDeep, ClampedZ, DepthX, DepthY);
			}

			// XZ edges (Y free)
			if (bXPos && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZPOS))
			{
				return Edge(Request.EdgeXPosZPos, Request.EdgeXPosZPosDeep, ClampedY, DepthX, DepthZ);
			}
			if (bXPos && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XPOS_ZNEG))
			{
				return Edge(Request.EdgeXPosZNeg, Request.EdgeXPosZNegDeep, ClampedY, DepthX, DepthZ);
			}
			if (bXNeg && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZPOS))
			{
				return Edge(Request.EdgeXNegZPos, Request.EdgeXNegZPosDeep, ClampedY, DepthX, DepthZ);
			}
			if (bXNeg && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_XNEG_ZNEG))
			{
				return Edge(Request.EdgeXNegZNeg, Request.EdgeXNegZNegDeep, ClampedY, DepthX, DepthZ);
			}

			// YZ edges (X free)
			if (bYPos && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZPOS))
			{
				return Edge(Request.EdgeYPosZPos, Request.EdgeYPosZPosDeep, ClampedX, DepthY, DepthZ);
			}
			if (bYPos && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_YPOS_ZNEG))
			{
				return Edge(Request.EdgeYPosZNeg, Request.EdgeYPosZNegDeep, ClampedX, DepthY, DepthZ);
			}
			if (bYNeg && bZPos && Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZPOS))
			{
				return Edge(Request.EdgeYNegZPos, Request.EdgeYNegZPosDeep, ClampedX, DepthY, DepthZ);
			}
			if (bYNeg && bZNeg && Request.HasEdge(FVoxelMeshingRequest::EDGE_YNEG_ZNEG))
			{
				return Edge(Request.EdgeYNegZNeg, Request.EdgeYNegZNegDeep, ClampedX, DepthY, DepthZ);
			}
			return Missing();
		}

		// Three axes out: the corner neighbour. Under Dual Contouring's rule the deep box at (DepthX, DepthY, DepthZ), a
		// strided cell's reach along the body diagonal (at stride 1 all depths are 0 and it is the corner voxel); under
		// Marching Cubes' the corner voxel.
		if (OutCount == 3)
		{
			auto Corner = [&](const FVoxelData& Base, const TArray<FVoxelData>& Deep) -> FVoxelData
			{
				return Rule.bDeepEdgesAndCorners ? Request.CornerDeepVoxel(Base, Deep, DepthX, DepthY, DepthZ) : Base;
			};

			if (bXPos && bYPos && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZPOS))
			{
				return Corner(Request.CornerXPosYPosZPos, Request.CornerXPosYPosZPosDeep);
			}
			if (bXPos && bYPos && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YPOS_ZNEG))
			{
				return Corner(Request.CornerXPosYPosZNeg, Request.CornerXPosYPosZNegDeep);
			}
			if (bXPos && bYNeg && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZPOS))
			{
				return Corner(Request.CornerXPosYNegZPos, Request.CornerXPosYNegZPosDeep);
			}
			if (bXPos && bYNeg && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XPOS_YNEG_ZNEG))
			{
				return Corner(Request.CornerXPosYNegZNeg, Request.CornerXPosYNegZNegDeep);
			}
			if (bXNeg && bYPos && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZPOS))
			{
				return Corner(Request.CornerXNegYPosZPos, Request.CornerXNegYPosZPosDeep);
			}
			if (bXNeg && bYPos && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YPOS_ZNEG))
			{
				return Corner(Request.CornerXNegYPosZNeg, Request.CornerXNegYPosZNegDeep);
			}
			if (bXNeg && bYNeg && bZPos && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZPOS))
			{
				return Corner(Request.CornerXNegYNegZPos, Request.CornerXNegYNegZPosDeep);
			}
			if (bXNeg && bYNeg && bZNeg && Request.HasCorner(FVoxelMeshingRequest::CORNER_XNEG_YNEG_ZNEG))
			{
				return Corner(Request.CornerXNegYNegZNeg, Request.CornerXNegYNegZNegDeep);
			}
			return Missing();
		}

		return FVoxelData::Air();
	}

	/** VoxelAt's density in [0, 1]. */
	FORCEINLINE float DensityAt(const FVoxelMeshingRequest& Request, int32 X, int32 Y, int32 Z, const FOutsideRule& Rule)
	{
		return static_cast<float>(VoxelAt(Request, X, Y, Z, Rule).Density) / 255.0f;
	}
}
