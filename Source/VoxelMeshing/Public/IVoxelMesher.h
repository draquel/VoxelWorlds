// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMeshingTypes.h"
#include "ChunkRenderData.h"
#include "RHI.h"

/**
 * Abstract interface for voxel mesh generation.
 *
 * Provides both synchronous CPU meshing and asynchronous GPU meshing
 * capabilities. Implementations handle the conversion of voxel data
 * to renderable mesh geometry.
 *
 * @see FVoxelCPUCubicMesher
 * @see FVoxelGPUCubicMesher
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
	// Synchronous CPU Meshing
	// ============================================================================

	/**
	 * Generate mesh data synchronously on the CPU.
	 *
	 * This is the fallback path for editor scenarios or when GPU
	 * is unavailable. Results are written to OutMeshData.
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
	// Asynchronous GPU Meshing
	// ============================================================================

	/**
	 * Generate mesh data asynchronously on the GPU.
	 *
	 * Submits a compute shader dispatch and returns immediately.
	 * The delegate is called when generation completes.
	 *
	 * @param Request Meshing request with voxel data and parameters
	 * @param OnComplete Delegate called when generation completes
	 * @return Handle for tracking the operation
	 */
	virtual FVoxelMeshingHandle GenerateMeshAsync(
		const FVoxelMeshingRequest& Request,
		FOnVoxelMeshingComplete OnComplete) = 0;

	/**
	 * Check if an async operation has completed.
	 *
	 * @param Handle Handle from GenerateMeshAsync
	 * @return true if the operation has completed
	 */
	virtual bool IsComplete(const FVoxelMeshingHandle& Handle) const = 0;

	/**
	 * Check if an async operation completed successfully.
	 *
	 * @param Handle Handle from GenerateMeshAsync
	 * @return true if completed successfully
	 */
	virtual bool WasSuccessful(const FVoxelMeshingHandle& Handle) const = 0;

	// ============================================================================
	// GPU Buffer Access
	// ============================================================================

	/**
	 * Get the vertex buffer for a completed GPU meshing operation.
	 *
	 * @param Handle Handle from a completed GenerateMeshAsync
	 * @return Pointer to vertex buffer, nullptr if not available
	 */
	virtual FRHIBuffer* GetVertexBuffer(const FVoxelMeshingHandle& Handle) = 0;

	/**
	 * Get the index buffer for a completed GPU meshing operation.
	 *
	 * @param Handle Handle from a completed GenerateMeshAsync
	 * @return Pointer to index buffer, nullptr if not available
	 */
	virtual FRHIBuffer* GetIndexBuffer(const FVoxelMeshingHandle& Handle) = 0;

	/**
	 * Get vertex and index counts for a completed operation.
	 *
	 * @param Handle Handle from a completed GenerateMeshAsync
	 * @param OutVertexCount Output vertex count
	 * @param OutIndexCount Output index count
	 * @return true if counts are available
	 */
	virtual bool GetBufferCounts(
		const FVoxelMeshingHandle& Handle,
		uint32& OutVertexCount,
		uint32& OutIndexCount) const = 0;

	/**
	 * Get the render data for a completed GPU meshing operation.
	 *
	 * This populates an FChunkRenderData with GPU buffer references.
	 *
	 * @param Handle Handle from a completed GenerateMeshAsync
	 * @param OutRenderData Output render data with GPU buffer references
	 * @return true if render data is available
	 */
	virtual bool GetRenderData(
		const FVoxelMeshingHandle& Handle,
		FChunkRenderData& OutRenderData) = 0;

	/**
	 * Read GPU mesh data back to CPU.
	 *
	 * This performs a GPU->CPU readback, which may stall.
	 * Use sparingly, primarily for collision or debugging.
	 *
	 * @param Handle Handle from a completed GenerateMeshAsync
	 * @param OutMeshData Output CPU mesh data
	 * @return true if readback succeeded
	 */
	virtual bool ReadbackToCPU(
		const FVoxelMeshingHandle& Handle,
		FChunkMeshData& OutMeshData) = 0;

	// ============================================================================
	// Resource Management
	// ============================================================================

	/**
	 * Release resources associated with a meshing handle.
	 *
	 * Call this when done with the mesh data to free GPU memory.
	 *
	 * @param Handle Handle to release
	 */
	virtual void ReleaseHandle(const FVoxelMeshingHandle& Handle) = 0;

	/**
	 * Release all pending handles and free resources.
	 */
	virtual void ReleaseAllHandles() = 0;

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
	 * Get statistics for a completed operation.
	 *
	 * @param Handle Handle from a completed operation
	 * @param OutStats Output statistics
	 * @return true if stats are available
	 */
	virtual bool GetStats(
		const FVoxelMeshingHandle& Handle,
		FVoxelMeshingStats& OutStats) const = 0;

	/**
	 * Get the mesher type name for debugging.
	 *
	 * @return Type name string (e.g., "CPU Cubic", "GPU Cubic")
	 */
	virtual FString GetMesherTypeName() const = 0;

	// ============================================================================
	// Per-Frame Update
	// ============================================================================

	/** Called each frame. GPU meshers use this to poll async readbacks. */
	virtual void Tick(float DeltaTime) {}
};
