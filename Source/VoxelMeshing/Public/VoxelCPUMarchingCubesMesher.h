// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"

/**
 * CPU-based Marching Cubes mesher.
 *
 * Generates smooth mesh geometry by interpolating vertex positions
 * along cube edges where the density field crosses the isosurface.
 * This produces organic, curved surfaces instead of blocky voxels.
 *
 * Algorithm:
 * - Process voxels in 2x2x2 cubes
 * - For each cube, determine which of 256 configurations applies
 * - Interpolate vertices along intersected edges
 * - Generate triangles based on lookup table
 *
 * Performance: ~30-80ms for 32^3 chunk on typical CPU
 *
 * @see IVoxelMesher
 * @see FVoxelGPUMarchingCubesMesher
 * @see MarchingCubesTables
 */
class VOXELMESHING_API FVoxelCPUMarchingCubesMesher : public IVoxelMesher
{
public:
	FVoxelCPUMarchingCubesMesher();
	virtual ~FVoxelCPUMarchingCubesMesher() override;

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

	// Async GPU methods - CPU mesher runs synchronously, wraps sync call
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

	virtual FString GetMesherTypeName() const override { return TEXT("CPU MarchingCubes"); }

	// ============================================================================
	// Seam-Ownership Meshing (P3 — implemented in VoxelCPUMarchingCubesSeams.cpp)
	// ============================================================================

	/**
	 * Single-owner MC seam meshing. Each participant's boundary band (its first/last cell
	 * layer toward the seam) is meshed with a per-participant SYNTHETIC request — the
	 * participant's own volume plus real neighbor slices extracted from the other
	 * participants' snapshots — driven through the same ProcessCubeLOD / transvoxel /
	 * geomorph code as legacy whole-chunk meshing. MC vertices and normals are pure
	 * functions of local samples, so junctions with the interior meshes are bit-identical
	 * by construction (same functions, same values). Mixed-LOD faces additionally emit the
	 * transvoxel ribbon (full face extent, in the finer participant's frame) and apply the
	 * boundary geomorph with full-depth coarse data.
	 */
	virtual bool GenerateFaceSeamMeshCPU(const FVoxelFaceSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) override;
	virtual bool GenerateEdgeSeamMeshCPU(const FVoxelEdgeSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) override;
	virtual bool GenerateCornerSeamMeshCPU(const FVoxelCornerSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) override;

private:
	/**
	 * Seam helper: mesh one participant's boundary-band cells from a synthetic request.
	 * BandMin/BandMaxEx are inclusive/exclusive voxel-coordinate cell bounds per axis (cells
	 * step by the request's stride). TransitionMask enables the boundary geomorph toward
	 * coarser facing neighbors (mixed LOD).
	 */
	void MeshSeamBandCells(
		const FVoxelMeshingRequest& SyntheticRequest,
		const FIntVector& BandMin,
		const FIntVector& BandMaxEx,
		uint8 TransitionMask,
		FChunkMeshData& OutMeshData,
		uint32& TriangleCount);
	// ============================================================================
	// Marching Cubes Core
	// ============================================================================

	/**
	 * Process a single cube at position (X, Y, Z).
	 * Generates triangles for the isosurface crossing this cube.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Cube position (cube occupies from (X,Y,Z) to (X+1,Y+1,Z+1))
	 * @param OutMeshData Output mesh data
	 * @param OutTriangleCount Counter for generated triangles
	 */
	void ProcessCube(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		FChunkMeshData& OutMeshData,
		uint32& OutTriangleCount);

	/**
	 * Get density value at position (normalized 0-1 scale).
	 * Handles chunk boundaries using neighbor data.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Position to sample (integer coordinates)
	 * @return Density value in range [0.0, 1.0]
	 */
	float GetDensityAt(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z) const;

	/**
	 * Get density value at fractional position using trilinear interpolation.
	 * Used for Transvoxel mid-point samples that fall between voxel positions.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Position to sample (fractional coordinates)
	 * @return Interpolated density value in range [0.0, 1.0]
	 */
	float GetDensityAtTrilinear(
		const FVoxelMeshingRequest& Request,
		float X, float Y, float Z) const;

	/**
	 * Get voxel data at position, handling chunk boundaries.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Position to sample
	 * @return Voxel data at position
	 */
	FVoxelData GetVoxelAt(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z) const;

	/**
	 * Interpolate vertex position along an edge.
	 * Finds the exact position where the isosurface crosses the edge.
	 *
	 * @param d0 Density at first corner
	 * @param d1 Density at second corner
	 * @param p0 Position of first corner
	 * @param p1 Position of second corner
	 * @param IsoLevel Isosurface threshold (typically 0.5)
	 * @return Interpolated position on the edge
	 */
	FVector3f InterpolateEdge(
		float d0, float d1,
		const FVector3f& p0, const FVector3f& p1,
		float IsoLevel) const;

	/**
	 * Calculate gradient-based normal at a position using central differences.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Position to calculate normal at
	 * @return Unit normal vector pointing away from solid
	 */
	FVector3f CalculateGradientNormal(
		const FVoxelMeshingRequest& Request,
		float X, float Y, float Z) const;

	/**
	 * Get the dominant material ID for a cube.
	 * Uses the material from the majority of solid corners.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Cube position
	 * @param CubeIndex The cube configuration index
	 * @return Material ID from dominant solid corner
	 */
	uint8 GetDominantMaterial(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		uint8 CubeIndex) const;

	/**
	 * Get the dominant biome ID for a cube.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Cube position
	 * @param CubeIndex The cube configuration index
	 * @return Biome ID from dominant solid corner
	 */
	uint8 GetDominantBiome(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		uint8 CubeIndex) const;

	// ============================================================================
	// LOD Support
	// ============================================================================

	/**
	 * Process a single cube at position with LOD stride.
	 * Generates triangles for the isosurface using strided sampling.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Cube position (in voxel coordinates, not LOD-scaled)
	 * @param Stride LOD stride (2^LODLevel)
	 * @param OutMeshData Output mesh data
	 * @param OutTriangleCount Counter for generated triangles
	 * @param DebugColorOverride When alpha != 0, overrides vertex color (for debug coloring)
	 * @param TransitionMask Active transition faces (borders coarser neighbours). When non-zero,
	 *                       boundary-slab vertices are geomorphed toward the coarse surface.
	 */
	void ProcessCubeLOD(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Stride,
		FChunkMeshData& OutMeshData,
		uint32& OutTriangleCount,
		FColor DebugColorOverride = FColor(0, 0, 0, 0),
		uint8 TransitionMask = 0);

	/**
	 * Geomorph (bake) the edge vertices of a boundary-slab cube toward the coarse-LOD surface.
	 *
	 * For each emitted vertex within the morph width of an active transition face, ramps its
	 * height toward the coarse iso-surface so the fine boundary contour meets the coarser
	 * neighbour with no vertical step (Transvoxel "secondary positions", baked). Gated by
	 * voxel.MCBoundaryMorph; no-op when TransitionMask == 0.
	 *
	 * @param Request The meshing request (provides density + neighbour data)
	 * @param Stride This chunk's LOD stride (2^LODLevel)
	 * @param TransitionMask Active transition faces (1 bit per face, ETransitionFace order)
	 * @param CellVertices In/out array of edge-crossing vertex positions (world units)
	 * @param VertexCount Number of valid entries in CellVertices
	 */
	void ApplyBoundaryMorph(
		const FVoxelMeshingRequest& Request,
		int32 Stride,
		uint8 TransitionMask,
		FVector3f* CellVertices,
		int32 VertexCount) const;

	/**
	 * Geomorph a single vertex toward the coarse-LOD surface (the per-vertex core of the
	 * boundary morph). Shared by the fine-MC path (ProcessCubeLOD) and the ribbon path
	 * (ProcessTransitionCell) so coincident vertices on both sides morph identically.
	 *
	 * @param Request The meshing request
	 * @param SelfStride This chunk's LOD stride (2^LODLevel)
	 * @param TransitionMask Active transition faces to ramp against
	 * @param Vertex In/out world-space vertex position; its height is morphed in place
	 */
	void MorphVertexToCoarse(
		const FVoxelMeshingRequest& Request,
		int32 SelfStride,
		uint8 TransitionMask,
		FVector3f& Vertex) const;

	/**
	 * Find the height at which the coarse-LOD iso-surface crosses the vertical column at (Vx,Vy).
	 *
	 * Samples density on the stride-CoarserStride lattice (trilinear) and returns the iso-crossing
	 * nearest NearVz. The morph target for height-function (plane) terrain.
	 *
	 * @param Request The meshing request
	 * @param Vx, Vy Column position in voxel units (fine resolution)
	 * @param NearVz Reference height (voxel units) — the crossing nearest this is returned
	 * @param CoarserStride The coarse neighbour's stride (E)
	 * @param OutVz Out: the coarse iso-surface height in voxel units
	 * @return true if a crossing was found within the search band
	 */
	bool ComputeCoarseSurfaceZ(
		const FVoxelMeshingRequest& Request,
		float Vx, float Vy, float NearVz,
		int32 CoarserStride,
		float& OutVz) const;

	/**
	 * Calculate gradient-based normal at a position using LOD-aware sampling.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Position to calculate normal at (in world units)
	 * @param Stride LOD stride for sampling offset
	 * @return Unit normal vector pointing away from solid
	 */
	FVector3f CalculateGradientNormalLOD(
		const FVoxelMeshingRequest& Request,
		float X, float Y, float Z,
		int32 Stride) const;

	/**
	 * Get dominant material for an LOD cube from strided corners.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Cube position
	 * @param Stride LOD stride
	 * @param CubeIndex The cube configuration index
	 * @return Material ID from dominant solid corner
	 */
	uint8 GetDominantMaterialLOD(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Stride,
		uint8 CubeIndex) const;

	/**
	 * Get dominant biome for an LOD cube from strided corners.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Cube position
	 * @param Stride LOD stride
	 * @param CubeIndex The cube configuration index
	 * @return Biome ID from dominant solid corner
	 */
	uint8 GetDominantBiomeLOD(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Stride,
		uint8 CubeIndex) const;

	// ============================================================================
	// Transvoxel LOD Transitions
	// ============================================================================

	/**
	 * Flags indicating which faces of a chunk border lower-LOD neighbors.
	 * Used to determine where transition cells are needed.
	 */
	enum ETransitionFace : uint8
	{
		TransitionNone = 0,
		TransitionXNeg = 1 << 0,  // -X face borders lower LOD
		TransitionXPos = 1 << 1,  // +X face borders lower LOD
		TransitionYNeg = 1 << 2,  // -Y face borders lower LOD
		TransitionYPos = 1 << 3,  // +Y face borders lower LOD
		TransitionZNeg = 1 << 4,  // -Z face borders lower LOD
		TransitionZPos = 1 << 5,  // +Z face borders lower LOD
	};

	/**
	 * Process a transition cell at the boundary between LOD levels.
	 * Uses Transvoxel algorithm to generate geometry that seamlessly
	 * connects high-resolution interior to low-resolution boundary.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Cell position in voxel coordinates
	 * @param Stride LOD stride (2^LODLevel)
	 * @param FaceIndex Which face this transition is on (0-5 for -X,+X,-Y,+Y,-Z,+Z)
	 * @param OutMeshData Output mesh data
	 * @param OutTriangleCount Counter for generated triangles
	 */
	bool ProcessTransitionCell(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Stride,
		int32 FaceIndex,
		FChunkMeshData& OutMeshData,
		uint32& OutTriangleCount);

	/**
	 * Check if a transition cell has all required neighbor data available.
	 * Transition cells at chunk edges/corners may require edge/corner neighbor data.
	 *
	 * @param Request The meshing request with neighbor data
	 * @param X, Y, Z Base cell position
	 * @param Stride LOD stride
	 * @param FaceIndex Which face (0-5)
	 * @return true if all required neighbor data is available
	 */
	bool HasRequiredNeighborData(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Stride,
		int32 FaceIndex) const;

	/**
	 * Get the 9-point density sample for a transition cell face.
	 * Samples the 3x3 grid of points on the high-resolution side.
	 *
	 * @param Request The meshing request with voxel data
	 * @param X, Y, Z Base cell position
	 * @param Stride LOD stride
	 * @param FaceIndex Which face (0-5)
	 * @param OutDensities Output array of 13 density values (9 face + 4 interior)
	 */
	void GetTransitionCellDensities(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Stride,
		int32 FaceIndex,
		float OutDensities[13]) const;

	/**
	 * Determine which faces need transition cells based on neighbor LOD levels.
	 * A face needs transitions if the neighbor chunk is at a lower LOD level.
	 *
	 * @param Request The meshing request (contains neighbor info)
	 * @return Bitmask of ETransitionFace flags
	 */
	uint8 GetTransitionFaces(const FVoxelMeshingRequest& Request) const;

	/**
	 * Check if a cell position is on a transition boundary.
	 *
	 * @param X, Y, Z Cell position
	 * @param ChunkSize Size of chunk
	 * @param Stride Current chunk's LOD stride
	 * @param TransitionMask Bitmask from GetTransitionFaces()
	 * @param OutFaceIndex Set to the face index if on boundary
	 * @return True if this cell needs transition geometry
	 */
	bool IsTransitionCell(
		int32 X, int32 Y, int32 Z,
		int32 ChunkSize, int32 Stride,
		uint8 TransitionMask,
		int32& OutFaceIndex) const;

	/**
	 * Get the stride to use for a transition cell on a given face.
	 * The transition cell stride should match the coarser neighbor's stride.
	 *
	 * @param Request The meshing request containing neighbor LOD levels
	 * @param FaceIndex Which face (0-5)
	 * @param CurrentStride Current chunk's LOD stride
	 * @return Stride to use for transition cell (neighbor's stride, or current if no valid neighbor)
	 */
	int32 GetTransitionCellStride(
		const FVoxelMeshingRequest& Request,
		int32 FaceIndex,
		int32 CurrentStride) const;

	/**
	 * Check if a cell falls within the footprint of any transition cell.
	 * Used to skip regular cell processing for positions covered by transition cells.
	 *
	 * @param Request The meshing request containing neighbor LOD levels
	 * @param X, Y, Z Cell position
	 * @param ChunkSize Size of chunk
	 * @param CurrentStride Current chunk's LOD stride
	 * @param TransitionMask Bitmask of faces needing transitions
	 * @return True if this cell is covered by a transition cell
	 */
	bool IsInTransitionRegion(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 ChunkSize, int32 CurrentStride,
		uint8 TransitionMask) const;

public:
	// ============================================================================
	// Transvoxel Debug Support
	// ============================================================================

	/**
	 * Debug data for a single transition cell, used for visualization.
	 */
	struct FTransitionCellDebugData
	{
		FIntVector ChunkCoord;
		FVector3f CellBasePos;      // World position of cell origin
		int32 FaceIndex;            // Which face (0-5)
		int32 Stride;               // Transition cell stride used
		int32 CurrentLOD;           // Current chunk's LOD
		int32 NeighborLOD;          // Neighbor's LOD
		float SampleDensities[13];  // Density values at each sample point
		TArray<FVector3f> SamplePositions;   // World positions of 13 samples
		TArray<FVector3f> GeneratedVertices; // Vertices generated by this cell
		uint16 CaseIndex;           // The 9-bit case index
		uint8 CellClass;            // Equivalence class (0-55)
		bool bInverted;             // Winding order inverted?

		// Anomaly detection results
		bool bHasFaceInteriorDisagreement = false;  // Interior corner disagrees with face corner on solid/air
		bool bHasClampedVertices = false;            // Some edge vertices have t clamped to 0 or 1
		bool bHasFoldedTriangles = false;            // Face normal opposes gradient normal
		int32 NumFilteredTriangles = 0;              // Triangles filtered by edge-length check
		uint8 DisagreementMask = 0;                  // Bits 0-3: which interior corners disagree

		// MC comparison: what regular MC would produce for this cell's footprint
		TArray<FVector3f> MCComparisonVertices;
		TArray<uint32> MCComparisonIndices;
	};

	/**
	 * Summary of transition cell anomalies for a mesh generation pass.
	 */
	struct FTransitionDebugSummary
	{
		int32 TotalTransitionCells = 0;
		int32 EmptyCells = 0;          // Cells that fell back to MC (class 0)
		int32 CellsWithDisagreement = 0;
		int32 CellsWithClampedVertices = 0;
		int32 CellsWithFoldedTriangles = 0;
		int32 TotalFilteredTriangles = 0;
		int32 PerFaceCounts[6] = {};   // How many transition cells per face
	};

	/** Compute summary from collected debug data */
	FTransitionDebugSummary GetTransitionDebugSummary() const;

	/** Collection of debug data from last mesh generation */
	TArray<FTransitionCellDebugData> TransitionCellDebugData;

	/** Enable detailed transition cell logging */
	bool bDebugLogTransitionCells = false;

	/** Enable collection of debug visualization data */
	bool bCollectDebugVisualization = false;

	/** Debug: Color transition cell triangles distinctly (orange=transition, green=MC, blue=fallback) */
	bool bDebugColorTransitionCells = false;

	/** Debug: Log anomalous transition cells (disagreements, clamped vertices, folded triangles) */
	bool bDebugLogAnomalies = false;

	/** Debug: Generate comparison MC geometry alongside transition cells */
	bool bDebugComparisonMesh = false;

	void SetDebugLogging(bool bEnable) { bDebugLogTransitionCells = bEnable; }
	void SetDebugVisualization(bool bEnable) { bCollectDebugVisualization = bEnable; }
	void SetDebugColorTransitionCells(bool bEnable) { bDebugColorTransitionCells = bEnable; }
	void SetDebugLogAnomalies(bool bEnable) { bDebugLogAnomalies = bEnable; }
	void SetDebugComparisonMesh(bool bEnable) { bDebugComparisonMesh = bEnable; }

	/**
	 * Get collected debug data from last mesh generation.
	 */
	const TArray<FTransitionCellDebugData>& GetTransitionCellDebugData() const { return TransitionCellDebugData; }

	/**
	 * Clear collected debug data.
	 */
	void ClearDebugData() { TransitionCellDebugData.Empty(); }

private:
	// ============================================================================
	// Skirt Generation (Fallback for LOD Seam Hiding)
	// ============================================================================

	/**
	 * Generate skirts along chunk boundaries to hide LOD seams.
	 * This is a fallback method when Transvoxel is disabled.
	 * Post-processes the generated mesh to find boundary edges and extend them outward.
	 *
	 * @param Request The meshing request with voxel data
	 * @param Stride LOD stride
	 * @param OutMeshData Output mesh data (modified in place)
	 * @param OutTriangleCount Counter for generated triangles
	 */
	void GenerateSkirts(
		const FVoxelMeshingRequest& Request,
		int32 Stride,
		FChunkMeshData& OutMeshData,
		uint32& OutTriangleCount);

	// ============================================================================
	// State
	// ============================================================================

	/** Whether the mesher is initialized */
	bool bIsInitialized = false;

	/** Current configuration */
	FVoxelMeshingConfig Config;

	/** Counter for generating request IDs */
	std::atomic<uint64> NextRequestId{1};

	/** Cached mesh results for async pattern */
	struct FCachedResult
	{
		FChunkMeshData MeshData;
		FVoxelMeshingStats Stats;
		bool bSuccess = false;
	};
	TMap<uint64, FCachedResult> CachedResults;
	mutable FCriticalSection CacheLock;
};
