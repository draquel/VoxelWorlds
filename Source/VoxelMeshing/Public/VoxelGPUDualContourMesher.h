// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"
#include "RHIFwd.h"

// Forward declarations
class FRDGPooledBuffer;

/**
 * GPU-based Dual Contouring mesher using RDG compute shaders.
 *
 * Implements the same DC algorithm as FVoxelCPUDualContourMesher but executes
 * on the GPU via a 4-pass compute shader pipeline:
 *   Pass 0: ResetCountersCS     - Zero all atomic counters
 *   Pass 1: EdgeCrossingCS      - Detect density sign changes, store hermite data
 *   Pass 2: QEFSolveCS          - Solve 3x3 QEF per cell via Jacobi SVD
 *   Pass 3: QuadGenerationCS    - Emit quads from 4-cell edge sharing
 *
 * LOD boundary merging is handled as a CPU pre-pass that produces a merge map
 * buffer uploaded to the GPU before dispatch.
 *
 * Performance: Target < 3ms per 32^3 chunk on modern GPUs
 * Thread Safety: Must be called from game thread, work dispatched to render thread
 *
 * @see IVoxelMesher
 * @see FVoxelCPUDualContourMesher
 */
class VOXELMESHING_API FVoxelGPUDualContourMesher : public IVoxelMesher
{
public:
	FVoxelGPUDualContourMesher();
	virtual ~FVoxelGPUDualContourMesher() override;

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

	virtual FString GetMesherTypeName() const override { return TEXT("GPU Dual Contouring"); }

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

	/** Dispatch the 4-pass compute shader pipeline on the render thread */
	void DispatchComputeShader(
		const FVoxelMeshingRequest& Request,
		uint64 RequestId,
		TSharedPtr<FMeshingResult> Result,
		FOnVoxelMeshingComplete OnComplete);

	/** Read vertex/index counts from GPU */
	void ReadCounters(TSharedPtr<FMeshingResult> Result);

	/** Pack voxel data for GPU upload */
	TArray<uint32> PackVoxelDataForGPU(const TArray<FVoxelData>& VoxelData);

	/**
	 * Build LOD merge map for boundary cell merging.
	 * CPU pre-pass that identifies boundary cells to merge for LOD transitions.
	 * Returns pairs of (OriginalCellIndex, ReplacementCellIndex).
	 */
	TArray<uint32> BuildLODMergeMap(
		const FVoxelMeshingRequest& Request,
		int32 GridDim,
		int32 Stride);
};
