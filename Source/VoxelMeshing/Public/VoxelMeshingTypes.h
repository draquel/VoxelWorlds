// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"

/**
 * Cell-domain restriction for seam-ownership meshing (SEAM_OWNERSHIP_ARCHITECTURE.md §2, P1+).
 *
 * WholeChunk = the chunk meshes its own cells plus the -1 boundary layer against neighbor slices
 * (the collision cook's request; Full until CR3, MESH-28). Interior = the seam-ownership interior pass: only
 * cells with zero neighbor dependence are meshed ([0, GridSize-1) per axis — no quad references a
 * -1 or GridSize-1 boundary-layer cell) and the request deliberately carries NO neighbor data
 * (gradient taps that would exit the chunk use the absent-neighbor Air fallback -> one-sided rim
 * normals, the same trade the default geometry-only deep depth already accepts). Boundary geometry
 * is produced separately by single-owner seam jobs. Honored by all three meshers.
 */
enum class EVoxelMeshCellDomain : uint8
{
	WholeChunk = 0,
	Interior = 1,
};

/**
 * Request structure for mesh generation.
 *
 * Contains all data needed to generate a mesh from voxel data.
 * Supports neighbor data for seamless chunk boundaries.
 *
 * @see IVoxelMesher
 */
struct VOXELMESHING_API FVoxelMeshingRequest
{

	/** Chunk position in chunk coordinate space */
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** LOD level for this mesh */
	int32 LODLevel = 0;

	/** Size of the chunk in voxels per axis */
	int32 ChunkSize = 32;

	/** World-space size of each voxel */
	float VoxelSize = 100.0f;

	/** World origin offset - all chunk positions are relative to this */
	FVector WorldOrigin = FVector::ZeroVector;

	/** Input voxel data (ChunkSize^3 elements) */
	TArray<FVoxelData> VoxelData;

	/**
	 * Face neighbor chunk data for seamless boundaries.
	 * Each array contains ChunkSize^2 voxels representing the face slice.
	 * Empty arrays mean boundary faces will be generated (chunk edge).
	 */
	TArray<FVoxelData> NeighborXPos;  // +X neighbor (East)

	TArray<FVoxelData> NeighborXNeg;  // -X neighbor (West)

	TArray<FVoxelData> NeighborYPos;  // +Y neighbor (North)

	TArray<FVoxelData> NeighborYNeg;  // -Y neighbor (South)

	TArray<FVoxelData> NeighborZPos;  // +Z neighbor (Top)

	TArray<FVoxelData> NeighborZNeg;  // -Z neighbor (Bottom)

	/**
	 * Additional neighbor planes BEYOND the first (plane 0) face slice above, for
	 * smooth meshers (Dual Contouring / Marching Cubes) at LOD > 0. A strided
	 * boundary cell physically reaches `stride` voxels into the neighbor and its
	 * gradient normals reach ~2*stride; one plane (the fields above) is only enough
	 * at stride 1. Each Deep array holds (NeighborPlaneDepth - 1) extra planes,
	 * plane k (k=0..) at one voxel deeper than the previous: for -X that is
	 * neighbor X = ChunkSize-2-k (world depth -(k+2)); for +X, neighbor X = 1+k
	 * (world depth +(k+1)). Empty for LOD 0 / non-smooth meshers — readers fall back
	 * to plane 0, so all existing single-plane consumers are unaffected.
	 */
	TArray<FVoxelData> NeighborXPosDeep;
	TArray<FVoxelData> NeighborXNegDeep;
	TArray<FVoxelData> NeighborYPosDeep;
	TArray<FVoxelData> NeighborYNegDeep;
	TArray<FVoxelData> NeighborZPosDeep;
	TArray<FVoxelData> NeighborZNegDeep;

	/** Total provided depth of the face-neighbor data (1 = a single plane, what LOD 0 needs). */
	int32 NeighborPlaneDepth = 1;

	/**
	 * Edge neighbor data for diagonal chunk boundaries (Marching Cubes).
	 * Each array contains ChunkSize voxels representing an edge strip.
	 * Named by the two positive/negative axes involved.
	 */
	TArray<FVoxelData> EdgeXPosYPos;  // +X+Y edge (Z varies)

	TArray<FVoxelData> EdgeXPosYNeg;  // +X-Y edge (Z varies)

	TArray<FVoxelData> EdgeXNegYPos;  // -X+Y edge (Z varies)

	TArray<FVoxelData> EdgeXNegYNeg;  // -X-Y edge (Z varies)

	TArray<FVoxelData> EdgeXPosZPos;  // +X+Z edge (Y varies)

	TArray<FVoxelData> EdgeXPosZNeg;  // +X-Z edge (Y varies)

	TArray<FVoxelData> EdgeXNegZPos;  // -X+Z edge (Y varies)

	TArray<FVoxelData> EdgeXNegZNeg;  // -X-Z edge (Y varies)

	TArray<FVoxelData> EdgeYPosZPos;  // +Y+Z edge (X varies)

	TArray<FVoxelData> EdgeYPosZNeg;  // +Y-Z edge (X varies)

	TArray<FVoxelData> EdgeYNegZPos;  // -Y+Z edge (X varies)

	TArray<FVoxelData> EdgeYNegZNeg;  // -Y-Z edge (X varies)

	/**
	 * Deep edge neighbor data for strided (LOD>0) Dual Contouring boundary cells.
	 * The 2-axis outward boundary cell at stride S reaches S voxels diagonally into the
	 * edge neighbour, so the single Edge* strip (depth 1,1) is insufficient. Each Deep
	 * array holds the full NeighborPlaneDepth x NeighborPlaneDepth grid of free-axis
	 * strips: index ((dA * NeighborPlaneDepth) + dB) * ChunkSize + Free, where dA/dB are
	 * 0-based depths into the two pinned axes (0,0 == the base Edge* strip). Empty when
	 * NeighborPlaneDepth == 1 (LOD0) — readers fall back to the 1-strip Edge* field.
	 */
	TArray<FVoxelData> EdgeXPosYPosDeep;

	TArray<FVoxelData> EdgeXPosYNegDeep;

	TArray<FVoxelData> EdgeXNegYPosDeep;

	TArray<FVoxelData> EdgeXNegYNegDeep;

	TArray<FVoxelData> EdgeXPosZPosDeep;

	TArray<FVoxelData> EdgeXPosZNegDeep;

	TArray<FVoxelData> EdgeXNegZPosDeep;

	TArray<FVoxelData> EdgeXNegZNegDeep;

	TArray<FVoxelData> EdgeYPosZPosDeep;

	TArray<FVoxelData> EdgeYPosZNegDeep;

	TArray<FVoxelData> EdgeYNegZPosDeep;

	TArray<FVoxelData> EdgeYNegZNegDeep;

	/**
	 * Corner neighbor data for diagonal chunk boundaries (Marching Cubes).
	 * Single voxel at each of the 8 chunk corners.
	 */
	FVoxelData CornerXPosYPosZPos;  // +X+Y+Z corner

	FVoxelData CornerXPosYPosZNeg;  // +X+Y-Z corner

	FVoxelData CornerXPosYNegZPos;  // +X-Y+Z corner

	FVoxelData CornerXPosYNegZNeg;  // +X-Y-Z corner

	FVoxelData CornerXNegYPosZPos;  // -X+Y+Z corner

	FVoxelData CornerXNegYPosZNeg;  // -X+Y-Z corner

	FVoxelData CornerXNegYNegZPos;  // -X-Y+Z corner

	FVoxelData CornerXNegYNegZNeg;  // -X-Y-Z corner

	/**
	 * Deep corner neighbor data for strided (LOD>0) Dual Contouring boundary cells.
	 * The 3-axis outward boundary cell reaches S voxels along the body diagonal into the
	 * corner neighbour. Each Deep array holds the full NeighborPlaneDepth^3 box:
	 * index ((dA * NeighborPlaneDepth) + dB) * NeighborPlaneDepth + dC, where dA/dB/dC are
	 * 0-based depths into the three pinned axes (0,0,0 == the single Corner* voxel). Empty
	 * when NeighborPlaneDepth == 1 (LOD0) — readers fall back to the scalar Corner* field.
	 */
	TArray<FVoxelData> CornerXPosYPosZPosDeep;

	TArray<FVoxelData> CornerXPosYPosZNegDeep;

	TArray<FVoxelData> CornerXPosYNegZPosDeep;

	TArray<FVoxelData> CornerXPosYNegZNegDeep;

	TArray<FVoxelData> CornerXNegYPosZPosDeep;

	TArray<FVoxelData> CornerXNegYPosZNegDeep;

	TArray<FVoxelData> CornerXNegYNegZPosDeep;

	TArray<FVoxelData> CornerXNegYNegZNegDeep;

	/** Flags indicating which edge/corner data is valid */
	uint32 EdgeCornerFlags = 0;

	/**
	 * Flags indicating which faces border coarser (higher LOD level) neighbors.
	 * Per Lengyel's Transvoxel: the FINER chunk generates transition cells
	 * so the face corners match the coarser neighbor's MC grid exactly.
	 * Bit 0: -X, Bit 1: +X, Bit 2: -Y, Bit 3: +Y, Bit 4: -Z, Bit 5: +Z
	 */
	uint8 TransitionFaces = 0;

	/**
	 * LOD levels of neighbor chunks for each face.
	 * Used by Transvoxel to determine transition cell stride.
	 * Order: -X, +X, -Y, +Y, -Z, +Z
	 * Value of -1 means no neighbor (chunk at world boundary).
	 */
	int32 NeighborLODLevels[6] = {-1, -1, -1, -1, -1, -1};

	/**
	 * Cell-domain restriction (seam-ownership P1). WholeChunk = whole-chunk meshing (the default;
	 * the collision cook's request). Interior = interior-only pass with zero neighbor deps;
	 * see EVoxelMeshCellDomain. Honored by all three meshers.
	 */
	EVoxelMeshCellDomain MeshCellDomain = EVoxelMeshCellDomain::WholeChunk;

	/**
	 * MC seam jobs only (P3): overrides voxel.Mesh.MCBoundaryMorphWidth (units: coarse cells) for
	 * the boundary geomorph. Seam-band cells morph with a ramp that reaches ZERO exactly at
	 * the band's inner edge — a wider ramp would move vertices shared with the
	 * unmorphed interior mesh and crack the junction. <= 0 uses the cvar (whole-chunk meshing).
	 */
	float MorphWidthOverride = -1.0f;

	/**
	 * Local up at the chunk (spherical Phase 4, SP-D1, SP-D2): +Z in heightmap modes, the chunk centre's radial direction
	 * on a planet (UVoxelChunkManager fills it). The marching-cubes LOD material scan and the boundary geomorph run
	 * along it (one body for every up; +Z gives the column arithmetic they had).
	 */
	FVector LocalUp = FVector::UpVector;

	/**
	 * Marching Cubes seam jobs only (world modes review G63, voxel.Seam.MCReadInPlace): the volumes of this chunk and of
	 * its neighbours, read in place from the seam job's snapshots (each ChunkSize^3 elements) instead of copied into
	 * VoxelData and the neighbour planes, strips and corners. Index (dx + 1) + (dy + 1) * 3 + (dz + 1) * 9 for the
	 * neighbour at (dx, dy, dz); [13] is this chunk's own volume, and a request that sets it leaves VoxelData empty (so
	 * IsValid fails and no whole-chunk mesh takes it). A neighbour reads as its face plane, edge strip or corner voxel
	 * would: face planes NeighborPlaneDepth deep (clamped past it), edges and corners one voxel. Null = not held. Read only
	 * by the CPU Marching Cubes mesher; not a UPROPERTY: a view valid for the call that sets it.
	 */
	const FVoxelData* NeighbourSnapshotViews[27] = {};

	/** NeighbourSnapshotViews index of the neighbour at (DX, DY, DZ), each in {-1, 0, 1}; 13 = the chunk itself. */
	static FORCEINLINE int32 VolumeViewIndex(int32 DX, int32 DY, int32 DZ)
	{
		return (DX + 1) + (DY + 1) * 3 + (DZ + 1) * 9;
	}

	// Transition face flag bits
	static constexpr uint8 TRANSITION_XNEG = 1 << 0;
	static constexpr uint8 TRANSITION_XPOS = 1 << 1;
	static constexpr uint8 TRANSITION_YNEG = 1 << 2;
	static constexpr uint8 TRANSITION_YPOS = 1 << 3;
	static constexpr uint8 TRANSITION_ZNEG = 1 << 4;
	static constexpr uint8 TRANSITION_ZPOS = 1 << 5;

	// Edge flag bits (0-11)
	static constexpr uint32 EDGE_XPOS_YPOS = 1 << 0;
	static constexpr uint32 EDGE_XPOS_YNEG = 1 << 1;
	static constexpr uint32 EDGE_XNEG_YPOS = 1 << 2;
	static constexpr uint32 EDGE_XNEG_YNEG = 1 << 3;
	static constexpr uint32 EDGE_XPOS_ZPOS = 1 << 4;
	static constexpr uint32 EDGE_XPOS_ZNEG = 1 << 5;
	static constexpr uint32 EDGE_XNEG_ZPOS = 1 << 6;
	static constexpr uint32 EDGE_XNEG_ZNEG = 1 << 7;
	static constexpr uint32 EDGE_YPOS_ZPOS = 1 << 8;
	static constexpr uint32 EDGE_YPOS_ZNEG = 1 << 9;
	static constexpr uint32 EDGE_YNEG_ZPOS = 1 << 10;
	static constexpr uint32 EDGE_YNEG_ZNEG = 1 << 11;

	// Corner flag bits (12-19)
	static constexpr uint32 CORNER_XPOS_YPOS_ZPOS = 1 << 12;
	static constexpr uint32 CORNER_XPOS_YPOS_ZNEG = 1 << 13;
	static constexpr uint32 CORNER_XPOS_YNEG_ZPOS = 1 << 14;
	static constexpr uint32 CORNER_XPOS_YNEG_ZNEG = 1 << 15;
	static constexpr uint32 CORNER_XNEG_YPOS_ZPOS = 1 << 16;
	static constexpr uint32 CORNER_XNEG_YPOS_ZNEG = 1 << 17;
	static constexpr uint32 CORNER_XNEG_YNEG_ZPOS = 1 << 18;
	static constexpr uint32 CORNER_XNEG_YNEG_ZNEG = 1 << 19;

	/** Get voxel at local position */
	FORCEINLINE const FVoxelData& GetVoxel(int32 X, int32 Y, int32 Z) const
	{
		const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
		return VoxelData[Index];
	}

	/** Check if request has valid voxel data */
	FORCEINLINE bool IsValid() const
	{
		return VoxelData.Num() == ChunkSize * ChunkSize * ChunkSize;
	}

	/** Get the world-space position of this chunk's origin (includes WorldOrigin offset) */
	FORCEINLINE FVector GetChunkWorldPosition() const
	{
		// All chunks cover the same world area regardless of LOD level
		// LOD only affects voxel resolution within the chunk, not chunk position
		return WorldOrigin + FVector(ChunkCoord) * static_cast<float>(ChunkSize) * VoxelSize;
	}

	/** Get expected neighbor slice size */
	FORCEINLINE int32 GetNeighborSliceSize() const
	{
		return ChunkSize * ChunkSize;
	}

	/** Check if a neighbor slice is present */
	FORCEINLINE bool HasNeighbor(int32 Face) const
	{
		switch (Face)
		{
		case 0: return NeighborXPos.Num() == GetNeighborSliceSize();
		case 1: return NeighborXNeg.Num() == GetNeighborSliceSize();
		case 2: return NeighborYPos.Num() == GetNeighborSliceSize();
		case 3: return NeighborYNeg.Num() == GetNeighborSliceSize();
		case 4: return NeighborZPos.Num() == GetNeighborSliceSize();
		case 5: return NeighborZNeg.Num() == GetNeighborSliceSize();
		default: return false;
		}
	}

	/** Check if an edge strip is present */
	FORCEINLINE bool HasEdge(uint32 EdgeFlag) const
	{
		return (EdgeCornerFlags & EdgeFlag) != 0;
	}

	/** Check if a corner is present */
	FORCEINLINE bool HasCorner(uint32 CornerFlag) const
	{
		return (EdgeCornerFlags & CornerFlag) != 0;
	}

	/**
	 * Resolve a face-neighbor voxel at plane `PlaneIdx` into the neighbor, where
	 * PlaneIdx 0 is the first (single) face slice and PlaneIdx >= 1 reads the Deep
	 * array. Clamps past the provided depth, and falls back to plane 0 when no Deep
	 * data is present (the single-plane read). InPlaneIdx is the index within
	 * a face slice (e.g. Y + Z*ChunkSize for an X face).
	 */
	FORCEINLINE FVoxelData NeighborPlaneVoxel(const TArray<FVoxelData>& Plane0, const TArray<FVoxelData>& Deep,
		int32 InPlaneIdx, int32 PlaneIdx) const
	{
		if (PlaneIdx <= 0)
		{
			return Plane0[InPlaneIdx];
		}
		const int32 SliceSize = GetNeighborSliceSize();
		const int32 DeepPlanes = SliceSize > 0 ? Deep.Num() / SliceSize : 0;
		if (DeepPlanes <= 0)
		{
			return Plane0[InPlaneIdx]; // only the single plane is available
		}
		const int32 K = FMath::Min(PlaneIdx - 1, DeepPlanes - 1);
		return Deep[K * SliceSize + InPlaneIdx];
	}

	/**
	 * Resolve an edge-neighbor voxel at depth (DepthA, DepthB) into the two pinned axes,
	 * where (0,0) is the base Edge* strip and >=1 reads the Deep grid. FreeIdx is the
	 * index along the free axis (0..ChunkSize-1). Clamps past the provided depth, and
	 * falls back to the base strip when no Deep data is present (the LOD 0 read).
	 */
	FORCEINLINE FVoxelData EdgeDeepVoxel(const TArray<FVoxelData>& Base1D, const TArray<FVoxelData>& Deep2D,
		int32 FreeIdx, int32 DepthA, int32 DepthB) const
	{
		if (Deep2D.Num() == 0)
		{
			return Base1D.IsValidIndex(FreeIdx) ? Base1D[FreeIdx] : FVoxelData();
		}
		const int32 D = FMath::Max(NeighborPlaneDepth, 1);
		const int32 a = FMath::Clamp(DepthA, 0, D - 1);
		const int32 b = FMath::Clamp(DepthB, 0, D - 1);
		const int32 Idx = (a * D + b) * ChunkSize + FreeIdx;
		if (Deep2D.IsValidIndex(Idx))
		{
			return Deep2D[Idx];
		}
		return Base1D.IsValidIndex(FreeIdx) ? Base1D[FreeIdx] : FVoxelData();
	}

	/**
	 * Resolve a corner-neighbor voxel at depth (DepthA, DepthB, DepthC) into the three
	 * pinned axes, where (0,0,0) is the single Corner* voxel and >=1 reads the Deep box.
	 * Clamps past the provided depth, and falls back to the scalar corner when no Deep
	 * data is present (the LOD 0 read).
	 */
	FORCEINLINE FVoxelData CornerDeepVoxel(const FVoxelData& Base, const TArray<FVoxelData>& Deep3D,
		int32 DepthA, int32 DepthB, int32 DepthC) const
	{
		if (Deep3D.Num() == 0)
		{
			return Base;
		}
		const int32 D = FMath::Max(NeighborPlaneDepth, 1);
		const int32 a = FMath::Clamp(DepthA, 0, D - 1);
		const int32 b = FMath::Clamp(DepthB, 0, D - 1);
		const int32 c = FMath::Clamp(DepthC, 0, D - 1);
		const int32 Idx = (a * D + b) * D + c;
		return Deep3D.IsValidIndex(Idx) ? Deep3D[Idx] : Base;
	}

	/** Get expected edge strip size */
	FORCEINLINE int32 GetEdgeStripSize() const
	{
		return ChunkSize;
	}
};

/**
 * Request for a single-owner FACE-SEAM meshing job (seam-ownership P1,
 * SEAM_OWNERSHIP_ARCHITECTURE.md §2.2).
 *
 * Same-LOD chunk pair: owner A (the min-coordinate participant) + neighbour B = A + unit(Axis).
 * The seam job meshes the one-world-cell slab straddling the shared face from BOTH sides' full
 * voxel arrays — no slices, no deep planes, no clamping across the face — and terminates
 * bit-exactly on the two interior meshes' boundary vertex rings (each ring recomputed with that
 * side's own data + the same absent-neighbour Air clamp its interior pass used). Output positions
 * are in the OWNER's local frame (B-side content sits at +ChunkSize*VoxelSize along Axis).
 *
 * Plain struct (not USTRUCT): an internal job payload, never reflected/serialized.
 */
struct VOXELMESHING_API FVoxelFaceSeamRequest
{
	/** Owner chunk (min-coordinate participant; the seam registry's canonical owner). */
	FIntVector OwnerChunkCoord = FIntVector::ZeroValue;

	/** Face normal axis: 0 = X, 1 = Y, 2 = Z. Neighbour B = Owner + unit(Axis). */
	uint8 Axis = 0;

	/** Owner-side LOD level. */
	int32 LODLevel = 0;

	/**
	 * Neighbour-side LOD level, or -1 for a same-LOD pair (== LODLevel). When the LODs differ
	 * (seam-ownership P2c) the seam meshes the owner's slab at the owner's stride, recomputes
	 * each ring at ITS side's stride, and stitches the face plane at the finer granularity
	 * (T-junction fans onto the coarser side's ring vertices).
	 */
	int32 LODLevelB = -1;

	/** Resolved neighbour-side LOD. */
	int32 GetLODLevelB() const { return (LODLevelB < 0) ? LODLevel : LODLevelB; }

	/** Voxels per chunk edge. */
	int32 ChunkSize = 32;

	/** World-space size of each voxel. */
	float VoxelSize = 100.0f;

	/** World origin offset (matches FVoxelMeshingRequest::WorldOrigin). */
	FVector WorldOrigin = FVector::ZeroVector;

	/** Local up at the owner chunk (matches FVoxelMeshingRequest::LocalUp; +Z in heightmap modes). */
	FVector LocalUp = FVector::UpVector;

	/**
	 * Owner-side voxels (ChunkSize^3, edit-merged), as a SHARED IMMUTABLE snapshot: the chunk
	 * manager builds one snapshot per (chunk, content version) and every seam job touching that
	 * chunk shares it — dispatch stops copying 128KB per participant per job (the traverse-phase
	 * game-thread cost found by the flip-qualification bench). Never mutate through this pointer.
	 */
	TSharedPtr<const TArray<FVoxelData>> VoxelDataA;

	/** Neighbour-side voxels (shared immutable snapshot — see VoxelDataA). */
	TSharedPtr<const TArray<FVoxelData>> VoxelDataB;

	/** Both voxel arrays present and parameters sane. */
	bool IsValid() const
	{
		const int32 Total = ChunkSize * ChunkSize * ChunkSize;
		return Axis <= 2 && LODLevel >= 0 && ChunkSize > 0
			&& VoxelDataA.IsValid() && VoxelDataA->Num() == Total
			&& VoxelDataB.IsValid() && VoxelDataB->Num() == Total;
	}
};

/**
 * Request for a single-owner EDGE-SEAM meshing job (seam-ownership P2a,
 * SEAM_OWNERSHIP_ARCHITECTURE.md §2.2 / "P2 plan").
 *
 * Same-LOD 4-tuple sharing the chunk edge parallel to EdgeAxis. Participants sit in the plane of
 * the two perpendicular axes taken in ASCENDING order (PerpA < PerpB) — matching
 * FVoxelSeamRegistry::GetParticipants order:
 *   [0] Owner (quadrant 0,0)   [1] Owner+unit(PerpA) (1,0)
 *   [2] Owner+unit(PerpB) (0,1)   [3] Owner+unit(PerpA)+unit(PerpB) (1,1)
 * The job meshes the edge-exclusive cell column (in both face slabs, excluding the corner cells at
 * the column's ends) from all four sides' data, and terminates bit-exactly on the surrounding
 * interior meshes AND face-seam meshes (their ring cells are recomputed with the same restricted
 * samplers those jobs used). Output positions are in the OWNER's local frame.
 *
 * Plain struct (not USTRUCT): an internal job payload, never reflected/serialized.
 */
struct VOXELMESHING_API FVoxelEdgeSeamRequest
{
	/** Owner chunk (min-coordinate participant; quadrant (0,0)). */
	FIntVector OwnerChunkCoord = FIntVector::ZeroValue;

	/** The axis the edge runs PARALLEL to: 0 = X, 1 = Y, 2 = Z. */
	uint8 EdgeAxis = 0;

	/** Owner-side LOD level (participant 0), and the shared LOD for same-LOD tuples. */
	int32 LODLevel = 0;

	/**
	 * Per-participant LOD levels (quadrant order), or all -1 for a same-LOD tuple (== LODLevel).
	 * When any differ (seam-ownership P2d) the mixed-LOD edge mesher runs: each ring/slab column
	 * computes at its OWNING job's stride, and stitching enumerates at the finest granularity
	 * with T-junction fans.
	 */
	int32 LODLevels[4] = { -1, -1, -1, -1 };

	/** Resolved LOD of participant q. */
	int32 GetLODLevelOf(int32 Quadrant) const
	{
		return (LODLevels[Quadrant] < 0) ? LODLevel : LODLevels[Quadrant];
	}

	/** True when every participant renders at the same LOD. */
	bool IsUniformLOD() const
	{
		for (int32 q = 0; q < 4; ++q)
		{
			if (GetLODLevelOf(q) != GetLODLevelOf(0))
			{
				return false;
			}
		}
		return true;
	}

	/** Voxels per chunk edge. */
	int32 ChunkSize = 32;

	/** World-space size of each voxel. */
	float VoxelSize = 100.0f;

	/** World origin offset (matches FVoxelMeshingRequest::WorldOrigin). */
	FVector WorldOrigin = FVector::ZeroVector;

	/** Local up at the owner chunk (matches FVoxelMeshingRequest::LocalUp; +Z in heightmap modes). */
	FVector LocalUp = FVector::UpVector;

	/**
	 * Participant voxel arrays (ChunkSize^3 each, edit-merged), in quadrant order
	 * [(0,0), (1,0), (0,1), (1,1)] over (PerpA, PerpB) — see the struct comment. Shared
	 * immutable snapshots (see FVoxelFaceSeamRequest::VoxelDataA); never mutate through them.
	 */
	TSharedPtr<const TArray<FVoxelData>> VoxelData[4];

	/** All four voxel arrays present and parameters sane. */
	bool IsValid() const
	{
		const int32 Total = ChunkSize * ChunkSize * ChunkSize;
		if (EdgeAxis > 2 || LODLevel < 0 || ChunkSize <= 0)
		{
			return false;
		}
		for (int32 i = 0; i < 4; ++i)
		{
			if (!VoxelData[i].IsValid() || VoxelData[i]->Num() != Total)
			{
				return false;
			}
		}
		return true;
	}
};

/**
 * Request for a single-owner CORNER-SEAM meshing job (seam-ownership P2b,
 * SEAM_OWNERSHIP_ARCHITECTURE.md "P2 plan").
 *
 * Same-LOD 8-tuple sharing the chunk corner at Owner + (1,1,1) chunks. Participants are in octant
 * order index = dx + dy*2 + dz*4 (X innermost) — matching FVoxelSeamRegistry::GetParticipants.
 * The job meshes the single corner cell (the one cell lying in all three face slabs) from all
 * eight sides' data and terminates bit-exactly on the surrounding interior meshes, face-seam
 * meshes, and edge-seam meshes (their ring cells are recomputed with the same restricted octant
 * masks those jobs' samplers used). Output positions are in the OWNER's local frame.
 *
 * Plain struct (not USTRUCT): an internal job payload, never reflected/serialized.
 */
struct VOXELMESHING_API FVoxelCornerSeamRequest
{
	/** Owner chunk (min-coordinate participant; octant (0,0,0)). */
	FIntVector OwnerChunkCoord = FIntVector::ZeroValue;

	/** Owner-side LOD level (octant 0), and the shared LOD for same-LOD tuples. */
	int32 LODLevel = 0;

	/**
	 * Per-participant LOD levels (octant order), or all -1 for a same-LOD tuple (== LODLevel).
	 * When any differ (seam-ownership P2d) the mixed-LOD corner mesher runs.
	 */
	int32 LODLevels[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

	/** Resolved LOD of octant o. */
	int32 GetLODLevelOf(int32 Octant) const
	{
		return (LODLevels[Octant] < 0) ? LODLevel : LODLevels[Octant];
	}

	/** True when every participant renders at the same LOD. */
	bool IsUniformLOD() const
	{
		for (int32 o = 0; o < 8; ++o)
		{
			if (GetLODLevelOf(o) != GetLODLevelOf(0))
			{
				return false;
			}
		}
		return true;
	}

	/** Voxels per chunk edge. */
	int32 ChunkSize = 32;

	/** World-space size of each voxel. */
	float VoxelSize = 100.0f;

	/** World origin offset (matches FVoxelMeshingRequest::WorldOrigin). */
	FVector WorldOrigin = FVector::ZeroVector;

	/** Local up at the owner chunk (matches FVoxelMeshingRequest::LocalUp; +Z in heightmap modes). */
	FVector LocalUp = FVector::UpVector;

	/**
	 * Participant voxel arrays (ChunkSize^3 each, edit-merged), octant order dx + dy*2 + dz*4.
	 * Shared immutable snapshots (see FVoxelFaceSeamRequest::VoxelDataA).
	 */
	TSharedPtr<const TArray<FVoxelData>> VoxelData[8];

	/** All eight voxel arrays present and parameters sane. */
	bool IsValid() const
	{
		const int32 Total = ChunkSize * ChunkSize * ChunkSize;
		if (LODLevel < 0 || ChunkSize <= 0)
		{
			return false;
		}
		for (int32 i = 0; i < 8; ++i)
		{
			if (!VoxelData[i].IsValid() || VoxelData[i]->Num() != Total)
			{
				return false;
			}
		}
		return true;
	}
};

/**
 * Statistics for a meshing operation.
 */
struct VOXELMESHING_API FVoxelMeshingStats
{

	/** Number of vertices generated */
	uint32 VertexCount = 0;

	/** Number of indices generated */
	uint32 IndexCount = 0;

	/** Number of faces generated */
	uint32 FaceCount = 0;

	/** Time taken to generate mesh in milliseconds */
	float GenerationTimeMs = 0.0f;

	/** Number of solid voxels processed */
	uint32 SolidVoxelCount = 0;

	/** Number of faces culled due to neighbors */
	uint32 CulledFaceCount = 0;

	/** Get triangle count */
	FORCEINLINE uint32 GetTriangleCount() const
	{
		return IndexCount / 3;
	}
};

/**
 * Configuration for mesh generation.
 */
struct VOXELMESHING_API FVoxelMeshingConfig
{

	/** Whether to generate UVs */
	bool bGenerateUVs = true;

	/** Whether to calculate ambient occlusion */
	bool bCalculateAO = true;

	/** UV scale for texture mapping */
	float UVScale = 1.0f;

	/**
	 * Whether to use greedy meshing algorithm.
	 * Greedy meshing merges adjacent coplanar faces with the same material
	 * into larger quads, significantly reducing triangle count (typically 40-60%).
	 * Disable for debugging or when per-voxel face data is needed.
	 * Only applies to cubic meshing.
	 */
	bool bUseGreedyMeshing = true;

	/**
	 * ISO surface threshold for smooth meshing (0.0-1.0).
	 * The isosurface is generated where density equals this value.
	 * Default 0.5 corresponds to density threshold 127 (VOXEL_SURFACE_THRESHOLD).
	 * Lower values produce larger/more solid meshes, higher values produce smaller.
	 */
	float IsoLevel = 0.5f;

	/**
	 * Use Transvoxel algorithm for seamless LOD transitions.
	 * Transvoxel generates special transition cells at LOD boundaries that
	 * properly connect high-resolution and low-resolution meshes without seams.
	 * Uses Eric Lengyel's official lookup tables for correct triangulation.
	 */
	bool bUseTransvoxel = true;

	// ============================================================================
	// Dual Contouring Options
	// ============================================================================

	/**
	 * SVD singular value threshold for QEF solver.
	 * Values below this are treated as zero during SVD decomposition.
	 * Higher values produce smoother but less feature-preserving results.
	 * Lower values preserve sharp features but may cause vertex instability.
	 */
	float QEFSVDThreshold = 0.1f;

	/**
	 * Blend factor for mass-point bias when QEF vertex is outside cell bounds.
	 * 0.0 = pure QEF solution (may be outside cell), 1.0 = always use mass point.
	 * Default 0.5 provides good balance between feature preservation and stability.
	 */
	float QEFBiasStrength = 0.5f;
};
