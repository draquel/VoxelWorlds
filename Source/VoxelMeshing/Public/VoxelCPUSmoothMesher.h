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
	 * @param X, Y, Z Position to sample
	 * @return Density value in range [0.0, 1.0]
	 */
	float GetDensityAt(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z) const;

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
