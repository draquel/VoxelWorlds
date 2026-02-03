// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"

/**
 * CPU-based smooth mesher using Marching Cubes algorithm.
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
 * @see FVoxelGPUSmoothMesher
 * @see MarchingCubesTables
 */
class VOXELMESHING_API FVoxelCPUSmoothMesher : public IVoxelMesher
{
public:
	FVoxelCPUSmoothMesher();
	virtual ~FVoxelCPUSmoothMesher() override;

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

	virtual FString GetMesherTypeName() const override { return TEXT("CPU Smooth"); }

private:
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
	 */
	void ProcessCubeLOD(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Stride,
		FChunkMeshData& OutMeshData,
		uint32& OutTriangleCount);

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
	void ProcessTransitionCell(
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
	};

	/** Collection of debug data from last mesh generation */
	TArray<FTransitionCellDebugData> TransitionCellDebugData;

	/** Enable detailed transition cell logging */
	bool bDebugLogTransitionCells = false;

	/** Enable collection of debug visualization data */
	bool bCollectDebugVisualization = false;

	/**
	 * Enable transition cell debug logging.
	 * When enabled, detailed information about each transition cell is logged.
	 */
	void SetDebugLogging(bool bEnable) { bDebugLogTransitionCells = bEnable; }

	/**
	 * Enable debug visualization data collection.
	 * When enabled, TransitionCellDebugData is populated during meshing.
	 */
	void SetDebugVisualization(bool bEnable) { bCollectDebugVisualization = bEnable; }

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
