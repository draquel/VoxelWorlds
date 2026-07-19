// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"

/**
 * CPU-based smooth mesher using Dual Contouring algorithm.
 *
 * Places vertices inside cells via QEF (Quadratic Error Function) minimization
 * instead of on edges like Marching Cubes. This enables seamless LOD transitions
 * through cell merging at LOD boundaries — no transition cells or lookup tables needed.
 *
 * Algorithm (per chunk):
 * 1. Edge Crossing Detection: Check 3 edges (+X, +Y, +Z) per cell for density sign changes
 * 2. QEF Vertex Placement: Collect hermite data from cell edges, solve 3x3 QEF via SVD
 * 3. Quad Generation: For each edge crossing, emit quad from 4 cells sharing that edge
 * 4. LOD Boundary Merging: Merge fine boundary cells to match coarser neighbor resolution
 *
 * @see IVoxelMesher
 * @see FQEFSolver
 */
class VOXELMESHING_API FVoxelCPUDualContourMesher : public IVoxelMesher
{
public:
	FVoxelCPUDualContourMesher();
	virtual ~FVoxelCPUDualContourMesher() override;

	// ============================================================================
	// IVoxelMesher Interface
	// ============================================================================

	virtual void Initialize() override;
	virtual void Shutdown() override;
	virtual bool IsInitialized() const override;

	virtual bool GenerateMeshCPU(
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData) override;

	virtual bool GenerateMeshCPU(
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData,
		FVoxelMeshingStats& OutStats) override;

	/**
	 * Seam-ownership P1 (SEAM_OWNERSHIP_ARCHITECTURE.md §2.2): mesh the single-owner FACE seam
	 * between a same-LOD chunk pair, from BOTH sides' full voxel data.
	 *
	 * Emits ONLY the seam geometry: quads dual to the edges whose surrounding cells include a
	 * slab cell (owner-side cell GridSize-1 ≡ neighbour-side cell -1). Slab cells solve their QEF
	 * against the combined two-chunk data (full hermite reach, nothing clamped); the adjacent
	 * interior "ring" cells (owner GridSize-2 / neighbour 0) are recomputed with each side's OWN
	 * data + Air clamp, bit-identical to what that side's Interior-domain pass produced — so the
	 * seam terminates exactly on both interior meshes with no communication. Edges that also touch
	 * another face's slab (the transverse rim) are left open — they belong to the P2 edge/corner
	 * seam jobs. Pair with EVoxelMeshCellDomain::Interior chunk meshes.
	 *
	 * Output positions are in the OWNER's local frame. Thread-safety matches GenerateMeshCPU
	 * (reads only the request + Config).
	 *
	 * @param SeamRequest Face-seam job payload (both sides' voxels, axis, shared LOD)
	 * @param OutMeshData Output seam mesh (reset first; empty output with true = valid no-op)
	 * @return true unless the request is invalid or the mesher is uninitialized
	 */
	bool GenerateFaceSeamMeshCPU(
		const FVoxelFaceSeamRequest& SeamRequest,
		FChunkMeshData& OutMeshData);

	/**
	 * Seam-ownership P2a: mesh the single-owner EDGE seam of a same-LOD chunk 4-tuple.
	 *
	 * Emits the quads dual to edges whose surrounding cells include an edge-column cell
	 * (owner-frame column at both face-slab layers, excluding the corner cells at its ends).
	 * The edge column solves against all four participants (quadrant sampler, full hermite
	 * reach); the eight surrounding ring columns are recomputed with the SAME restricted
	 * samplers their original jobs used — participant-interior columns with that chunk's
	 * single sampler, face-slab columns with the face jobs' pair samplers, each in its
	 * original computation frame — so the edge seam terminates bit-exactly on the interior
	 * meshes and the face-seam meshes with no communication (§2.2, composed).
	 * The corner cells at the column ends belong to the P2b corner-seam jobs and stay open.
	 *
	 * Output positions are in the OWNER's local frame. Thread-safety matches GenerateMeshCPU.
	 *
	 * @param SeamRequest Edge-seam job payload (four participants' voxels, parallel axis, LOD)
	 * @param OutMeshData Output seam mesh (reset first; empty output with true = valid no-op)
	 * @return true unless the request is invalid or the mesher is uninitialized
	 */
	bool GenerateEdgeSeamMeshCPU(
		const FVoxelEdgeSeamRequest& SeamRequest,
		FChunkMeshData& OutMeshData);

	/**
	 * Seam-ownership P2b: mesh the single-owner CORNER seam of a same-LOD chunk 8-tuple.
	 *
	 * Emits the quads dual to the 12 edges touching the corner cell (the one cell lying in all
	 * three face slabs). The corner cell solves against all eight participants (octant sampler,
	 * full hermite reach); the surrounding ring cells — participant-interior corner cells, the
	 * twelve face-slab corner cells, and the six adjacent edge-column end cells — are recomputed
	 * with the SAME restricted octant masks their original jobs' samplers used, in those jobs'
	 * frames, so the corner seam terminates bit-exactly on every neighbouring mesh with no
	 * communication (§2.2, fully composed). With face (P1), edge (P2a), and corner seams, a
	 * uniform-LOD region is completely watertight under voxel.Seam.Meshing.
	 *
	 * Output positions are in the OWNER's local frame. Thread-safety matches GenerateMeshCPU.
	 *
	 * @param SeamRequest Corner-seam job payload (eight participants' voxels, shared LOD)
	 * @param OutMeshData Output seam mesh (reset first; empty output with true = valid no-op)
	 * @return true unless the request is invalid or the mesher is uninitialized
	 */
	bool GenerateCornerSeamMeshCPU(
		const FVoxelCornerSeamRequest& SeamRequest,
		FChunkMeshData& OutMeshData);

	virtual FVoxelMeshingHandle GenerateMeshAsync(
		const FVoxelMeshingRequest& Request,
		FOnVoxelMeshingComplete OnComplete) override;

	virtual bool IsComplete(const FVoxelMeshingHandle& Handle) const override;
	virtual bool WasSuccessful(const FVoxelMeshingHandle& Handle) const override;

	virtual FRHIBuffer* GetVertexBuffer(const FVoxelMeshingHandle& Handle) override;
	virtual FRHIBuffer* GetIndexBuffer(const FVoxelMeshingHandle& Handle) override;
	virtual bool GetBufferCounts(
		const FVoxelMeshingHandle& Handle,
		uint32& OutVertexCount,
		uint32& OutIndexCount) const override;
	virtual bool GetRenderData(
		const FVoxelMeshingHandle& Handle,
		FChunkRenderData& OutRenderData) override;
	virtual bool ReadbackToCPU(
		const FVoxelMeshingHandle& Handle,
		FChunkMeshData& OutMeshData) override;

	virtual void ReleaseHandle(const FVoxelMeshingHandle& Handle) override;
	virtual void ReleaseAllHandles() override;

	virtual void SetConfig(const FVoxelMeshingConfig& Config) override;
	virtual const FVoxelMeshingConfig& GetConfig() const override;

	virtual bool GetStats(
		const FVoxelMeshingHandle& Handle,
		FVoxelMeshingStats& OutStats) const override;

	virtual FString GetMesherTypeName() const override { return TEXT("CPU Dual Contouring"); }

private:
	// ============================================================================
	// DC Data Structures
	// ============================================================================

	/** Hermite data for a single edge crossing */
	struct FDCEdgeCrossing
	{
		FVector3f Position;  // World-space crossing point on edge
		FVector3f Normal;    // Surface gradient normal at crossing
		bool bValid = false; // Whether this edge has a crossing
	};

	/** Per-cell QEF vertex */
	struct FDCCellVertex
	{
		FVector3f Position;     // QEF-solved vertex position
		FVector3f Normal;       // Averaged normal from edge crossings
		uint8 EmittedMaterialID = 0; // Material of the primary emission (valid when MeshVertexIndex >= 0)
		uint8 EmittedBiomeID = 0;    // Biome of the primary emission (valid when MeshVertexIndex >= 0)
		int32 MeshVertexIndex = -1;  // Primary emission index in output FChunkMeshData (-1 = not emitted yet)
		bool bValid = false;         // Whether this cell has a QEF vertex
	};

	/**
	 * Flat array indexing for cells. Grid dimension = GridSize + 3 to cover
	 * cells from -1 to GridSize+1 (needed for edge lookups at cell+1 offsets).
	 */
	static FORCEINLINE int32 CellIndex(int32 CX, int32 CY, int32 CZ, int32 GridDim)
	{
		return (CX + 1) + (CY + 1) * GridDim + (CZ + 1) * GridDim * GridDim;
	}

	/** Flat array indexing for edges: 3 edges per cell position (axes 0,1,2) */
	static FORCEINLINE int32 EdgeIndex(int32 CX, int32 CY, int32 CZ, int32 Axis, int32 GridDim)
	{
		return ((CX + 1) + (CY + 1) * GridDim + (CZ + 1) * GridDim * GridDim) * 3 + Axis;
	}

	// ============================================================================
	// Density & Voxel Access (copied from FVoxelCPUMarchingCubesMesher)
	// ============================================================================

	float GetDensityAt(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z) const;

	FVoxelData GetVoxelAt(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z) const;

	FVector3f CalculateGradientNormal(
		const FVoxelMeshingRequest& Request,
		float X, float Y, float Z) const;

	FVector3f CalculateGradientNormalLOD(
		const FVoxelMeshingRequest& Request,
		float X, float Y, float Z,
		int32 Stride) const;

	// ============================================================================
	// DC Core Algorithm
	// ============================================================================

	/**
	 * Pass 1: Detect edge crossings across the grid.
	 * Each cell owns edges in +X, +Y, +Z from its minimum corner.
	 */
	void DetectEdgeCrossings(
		const FVoxelMeshingRequest& Request,
		int32 Stride,
		int32 GridDim,
		TArray<FDCEdgeCrossing>& OutEdgeCrossings,
		TArray<int32>& OutValidEdgeIndices);

	/**
	 * Pass 2: Solve QEF for each cell that has edge crossings.
	 * Collects hermite data from up to 12 edges touching the cell.
	 */
	void SolveCellVertices(
		const FVoxelMeshingRequest& Request,
		int32 Stride,
		int32 GridDim,
		const TArray<FDCEdgeCrossing>& EdgeCrossings,
		TArray<FDCCellVertex>& OutCellVertices);

	/**
	 * Pass 3: Generate quads for each edge crossing.
	 * For each edge, finds the 4 cells sharing it and emits a quad.
	 */
	void GenerateQuads(
		const FVoxelMeshingRequest& Request,
		int32 Stride,
		int32 GridDim,
		const TArray<FDCEdgeCrossing>& EdgeCrossings,
		const TArray<int32>& ValidEdgeIndices,
		TArray<FDCCellVertex>& CellVertices,
		FChunkMeshData& OutMeshData,
		uint32& OutTriangleCount);

	/**
	 * Append a mesh vertex for a cell, carrying explicit material data.
	 *
	 * Material/biome are per-QUAD, not per-cell: every triangle must be
	 * material-uniform because the pixel shader rounds the hardware-interpolated
	 * UV1.x back to an integer atlas index — a triangle with mixed IDs sweeps
	 * through every intermediate index, rendering stripes of unrelated materials
	 * at borders. GenerateQuads owns the caching, and duplicates a cell vertex
	 * (same position/normal) when quads of different materials share it.
	 *
	 * Always appends; returns the new vertex index in the output mesh.
	 */
	int32 EmitVertex(
		const FVoxelMeshingRequest& Request,
		const FDCCellVertex& Vertex,
		uint8 MaterialID,
		uint8 BiomeID,
		FChunkMeshData& OutMeshData) const;

	/** Generate vertical skirt geometry at LOD transition boundaries to hide seams. */
	void GenerateSkirts(
		const FVoxelMeshingRequest& Request,
		int32 Stride,
		FChunkMeshData& OutMeshData,
		uint32& OutTriangleCount);

	// ============================================================================
	// State
	// ============================================================================

	bool bIsInitialized = false;
	FVoxelMeshingConfig Config;
	std::atomic<uint64> NextRequestId{1};

	struct FCachedResult
	{
		FChunkMeshData MeshData;
		FVoxelMeshingStats Stats;
		bool bSuccess = false;
	};
	TMap<uint64, FCachedResult> CachedResults;
	mutable FCriticalSection CacheLock;
};
