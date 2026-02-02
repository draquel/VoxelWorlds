// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"
#include "RHIFwd.h"

// Forward declarations
class FRDGPooledBuffer;

/**
 * GPU-based smooth mesher using Marching Cubes compute shaders.
 *
 * Generates smooth mesh geometry on the GPU by interpolating vertex
 * positions along cube edges where the density field crosses the isosurface.
 * Uses atomic counters for dynamic vertex/index allocation.
 *
 * Performance: ~0.5-2ms per 32^3 chunk on modern GPUs
 * Thread Safety: Must be called from game thread, work dispatched to render thread
 *
 * @see IVoxelMesher
 * @see FVoxelCPUSmoothMesher
 * @see MarchingCubesTables
 */
class VOXELMESHING_API FVoxelGPUSmoothMesher : public IVoxelMesher
{
public:
	FVoxelGPUSmoothMesher();
	virtual ~FVoxelGPUSmoothMesher() override;

	// ============================================================================
	// IVoxelMesher Interface
	// ============================================================================

	virtual void Initialize() override;
	virtual void Shutdown() override;
	virtual bool IsInitialized() const override { return bIsInitialized; }

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

	virtual FString GetMesherTypeName() const override { return TEXT("GPU Smooth"); }

private:
	bool bIsInitialized = false;

	/** Counter for generating unique request IDs */
	std::atomic<uint64> NextRequestId{1};

	/** Current configuration */
	FVoxelMeshingConfig Config;

	/** Stored meshing results */
	struct FMeshingResult
	{
		TRefCountPtr<FRDGPooledBuffer> VertexBuffer;
		TRefCountPtr<FRDGPooledBuffer> IndexBuffer;
		TRefCountPtr<FRDGPooledBuffer> CounterBuffer;
		TRefCountPtr<FRDGPooledBuffer> StagingVertexBuffer;
		TRefCountPtr<FRDGPooledBuffer> StagingIndexBuffer;
		TRefCountPtr<FRDGPooledBuffer> StagingCounterBuffer;
		FIntVector ChunkCoord;
		int32 ChunkSize = 0;
		uint32 VertexCount = 0;
		uint32 IndexCount = 0;
		bool bIsComplete = false;
		bool bWasSuccessful = false;
		bool bCountsRead = false;
		FVoxelMeshingStats Stats;
	};

	TMap<uint64, TSharedPtr<FMeshingResult>> MeshingResults;
	mutable FCriticalSection ResultsLock;

	/** Dispatch the compute shader on the render thread */
	void DispatchComputeShader(
		const FVoxelMeshingRequest& Request,
		uint64 RequestId,
		TSharedPtr<FMeshingResult> Result,
		FOnVoxelMeshingComplete OnComplete);

	/** Read vertex/index counts from GPU */
	void ReadCounters(TSharedPtr<FMeshingResult> Result);

	/** Pack voxel data for GPU upload */
	TArray<uint32> PackVoxelDataForGPU(const TArray<FVoxelData>& VoxelData);

	/** Create the triangle table buffer for GPU */
	TArray<int32> CreateTriangleTableData();
};
