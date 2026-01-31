// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"

/**
 * CPU-based cubic mesher using face culling.
 *
 * Generates mesh geometry by checking each solid voxel and
 * emitting quad faces only where adjacent voxels are air.
 * This is the fallback path when GPU compute is unavailable.
 *
 * Algorithm:
 * - For each solid voxel, check all 6 neighbors
 * - If neighbor is air, emit a quad (4 vertices, 6 indices)
 * - Supports neighbor chunk data for seamless boundaries
 *
 * Performance: ~20-50ms for 32^3 chunk on typical CPU
 *
 * @see IVoxelMesher
 * @see FVoxelGPUCubicMesher
 */
class VOXELMESHING_API FVoxelCPUCubicMesher : public IVoxelMesher
{
public:
	FVoxelCPUCubicMesher();
	virtual ~FVoxelCPUCubicMesher() override;

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

	virtual FString GetMesherTypeName() const override { return TEXT("CPU Cubic"); }

private:
	/**
	 * Check if a face should be rendered.
	 * Returns true if the neighbor in the given direction is air.
	 */
	bool ShouldRenderFace(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Face) const;

	/**
	 * Get voxel data at position, handling chunk boundaries.
	 */
	FVoxelData GetVoxelAt(
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z) const;

	/**
	 * Emit a quad face to the mesh data.
	 */
	void EmitQuad(
		FChunkMeshData& MeshData,
		const FVoxelMeshingRequest& Request,
		int32 X, int32 Y, int32 Z,
		int32 Face,
		const FVoxelData& Voxel) const;

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

	// ============================================================================
	// Face Data Tables
	// ============================================================================

	/** Face direction offsets (6 faces) */
	static const FIntVector FaceOffsets[6];

	/** Face normals */
	static const FVector3f FaceNormals[6];

	/** Quad vertex offsets for each face (4 vertices per face) */
	static const FVector3f QuadVertices[6][4];

	/** UV coordinates for quad vertices */
	static const FVector2f QuadUVs[4];
};
