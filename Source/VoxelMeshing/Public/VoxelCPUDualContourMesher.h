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
		uint8 MaterialID = 0;
		uint8 BiomeID = 0;
		int32 MeshVertexIndex = -1;  // Index in output FChunkMeshData (-1 = not emitted yet)
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
	// Density & Voxel Access (copied from FVoxelCPUSmoothMesher)
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
	 * Pass 4: Merge boundary cells for LOD transitions.
	 * Groups 2x2 fine boundary cells into 1 coarser cell at LOD transition faces.
	 */
	void MergeLODBoundaryCells(
		const FVoxelMeshingRequest& Request,
		int32 Stride,
		int32 GridDim,
		TArray<FDCEdgeCrossing>& EdgeCrossings,
		TArray<FDCCellVertex>& CellVertices);

	/**
	 * Get the dominant material for a cell by voting across solid voxels.
	 */
	uint8 GetCellMaterial(
		const FVoxelMeshingRequest& Request,
		int32 CellX, int32 CellY, int32 CellZ,
		int32 Stride) const;

	/**
	 * Get the dominant biome for a cell by voting across solid voxels.
	 */
	uint8 GetCellBiome(
		const FVoxelMeshingRequest& Request,
		int32 CellX, int32 CellY, int32 CellZ,
		int32 Stride) const;

	/**
	 * Emit a vertex into the mesh data if not already emitted.
	 * Returns the vertex index in the output mesh.
	 */
	int32 EmitVertex(
		const FVoxelMeshingRequest& Request,
		FDCCellVertex& Vertex,
		FChunkMeshData& OutMeshData);

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
