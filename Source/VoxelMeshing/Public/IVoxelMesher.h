// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"

/**
 * Abstract interface for voxel mesh generation.
 *
 * Synchronous CPU meshing of a chunk's interior (GenerateMeshCPU) and of the seams between chunks (the three
 * seam entry points), called from the chunk manager's worker tasks. The three implementations are the cubic,
 * Marching Cubes and Dual Contouring meshers; the GPU meshers that once implemented an asynchronous half of
 * this interface were never dispatched by a world and were deleted (code review CR1, MESH-01/02/03).
 *
 * @see FVoxelCPUCubicMesher
 * @see FVoxelCPUMarchingCubesMesher
 * @see FVoxelCPUDualContourMesher
 * @see Documentation/ARCHITECTURE.md
 */
class VOXELMESHING_API IVoxelMesher
{
public:
	virtual ~IVoxelMesher() = default;

	// ============================================================================
	// Lifecycle
	// ============================================================================

	/**
	 * Initialize the mesher.
	 * Must be called before any mesh generation.
	 */
	virtual void Initialize() = 0;

	/**
	 * Shutdown the mesher and release resources.
	 */
	virtual void Shutdown() = 0;

	/**
	 * Check if mesher is initialized and ready.
	 */
	virtual bool IsInitialized() const = 0;

	// ============================================================================
	// Chunk Meshing
	// ============================================================================

	/**
	 * Generate mesh data synchronously on the calling thread (a worker).
	 * Results are written to OutMeshData.
	 *
	 * @param Request Meshing request with voxel data and parameters
	 * @param OutMeshData Output mesh data (positions, normals, UVs, indices)
	 * @return true if mesh was generated successfully
	 */
	virtual bool GenerateMeshCPU(
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData) = 0;

	/**
	 * Generate mesh data synchronously with statistics.
	 *
	 * @param Request Meshing request with voxel data and parameters
	 * @param OutMeshData Output mesh data
	 * @param OutStats Output generation statistics
	 * @return true if mesh was generated successfully
	 */
	virtual bool GenerateMeshCPU(
		const FVoxelMeshingRequest& Request,
		FChunkMeshData& OutMeshData,
		FVoxelMeshingStats& OutStats) = 0;

	// ============================================================================
	// Configuration
	// ============================================================================

	/**
	 * Set the meshing configuration.
	 *
	 * @param Config New configuration settings
	 */
	virtual void SetConfig(const FVoxelMeshingConfig& Config) = 0;

	/**
	 * Get the current meshing configuration.
	 *
	 * @return Current configuration settings
	 */
	virtual const FVoxelMeshingConfig& GetConfig() const = 0;

	// ============================================================================
	// Statistics
	// ============================================================================

	/**
	 * Get the mesher type name for debugging.
	 *
	 * @return Type name string (e.g., "CPU Cubic", "CPU MarchingCubes")
	 */
	virtual FString GetMesherTypeName() const = 0;

	// ============================================================================
	// Seam-Ownership Meshing (SEAM_OWNERSHIP_ARCHITECTURE.md §2.2)
	// ============================================================================

	/**
	 * Single-owner seam meshing entry points (CPU, worker-thread safe). A mesher that
	 * participates in the seam-ownership pipeline overrides these to produce the boundary
	 * geometry between interior-domain chunk meshes. Defaults return false (no seam
	 * support) — the chunk manager's activation gate only enables the pipeline for
	 * mesher types known to implement them.
	 */
	virtual bool GenerateFaceSeamMeshCPU(const FVoxelFaceSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) { return false; }
	virtual bool GenerateEdgeSeamMeshCPU(const FVoxelEdgeSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) { return false; }
	virtual bool GenerateCornerSeamMeshCPU(const FVoxelCornerSeamRequest& SeamRequest, FChunkMeshData& OutMeshData) { return false; }
};
