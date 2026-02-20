// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelMesher.h"
#include "RHIFwd.h"
#include "RHIGPUReadback.h"

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

	/** Called each frame to poll async GPU readbacks. */
	virtual void Tick(float DeltaTime) override;

private:
	bool bIsInitialized = false;

	/** Counter for generating unique request IDs */
	std::atomic<uint64> NextRequestId{1};

	/** Current configuration */
	FVoxelMeshingConfig Config;

	/** Async readback state machine phases */
	enum class EReadbackPhase : uint8
	{
		WaitingForCounters,  // (legacy) Counter readback enqueued, polling IsReady()
		CopyingCounters,     // (legacy) Render cmd enqueued to Lock/copy/Unlock counters
		WaitingForAllReadbacks, // All 3 readbacks enqueued at once, polling IsReady()
		CopyingAllData,      // Render cmd enqueued to Lock/copy/Unlock all data
		WaitingForData,      // Vertex/index readback enqueued, polling IsReady()
		CopyingData,         // Render cmd enqueued to Lock/copy/Unlock mesh data
		Complete,            // Data ready on CPU, OnComplete fired
		Failed
	};

	/** Stored meshing results */
	struct FMeshingResult
	{
		// GPU buffers extracted from RDG graph
		TRefCountPtr<FRDGPooledBuffer> VertexBuffer;
		TRefCountPtr<FRDGPooledBuffer> IndexBuffer;
		TRefCountPtr<FRDGPooledBuffer> CounterBuffer;

		FIntVector ChunkCoord;
		int32 ChunkSize = 0;
		uint32 VertexCount = 0;
		uint32 IndexCount = 0;
		std::atomic<bool> bIsComplete{false};
		std::atomic<bool> bWasSuccessful{false};
		std::atomic<bool> bCountsRead{false};
		FVoxelMeshingStats Stats;

		// Async readback state
		EReadbackPhase ReadbackPhase = EReadbackPhase::WaitingForCounters;
		FRHIGPUBufferReadback* CounterReadback = nullptr;
		FRHIGPUBufferReadback* VertexReadback = nullptr;
		FRHIGPUBufferReadback* IndexReadback = nullptr;

		// Deferred completion callback (fired when CPU data is ready)
		FOnVoxelMeshingComplete PendingOnComplete;
		FVoxelMeshingHandle PendingHandle;

		// Pre-read mesh data (populated by TickReadbacks, consumed by ReadbackToCPU)
		FChunkMeshData ReadbackMeshData;

		// Config snapshot captured at dispatch time (for max buffer capacities)
		uint32 CapturedMaxVertices = 0;
		uint32 CapturedMaxIndices = 0;

		// Chunk world position captured at dispatch time (shader outputs world-space positions;
		// must subtract this to convert back to local chunk space for the rendering pipeline)
		FVector3f ChunkWorldPosition = FVector3f::ZeroVector;

		// Atomic flags to prevent game-thread polling of IsReady() before render-thread
		// EnqueueCopy has been called (a freshly-created FRHIGPUBufferReadback with no
		// pending fence reports IsReady()=true, which would cause premature Lock())
		std::atomic<bool> bCounterReadbackEnqueued{false};
		std::atomic<bool> bDataReadbackEnqueued{false};

		~FMeshingResult()
		{
			delete CounterReadback;
			delete VertexReadback;
			delete IndexReadback;
		}
	};

	TMap<uint64, TSharedPtr<FMeshingResult>> MeshingResults;
	mutable FCriticalSection ResultsLock;

	/** Poll all pending async readbacks and fire callbacks for completed ones. */
	void TickReadbacks();

	/** Copy vertex data from readback buffer into Result->ReadbackMeshData. Render thread only. */
	static void CopyVertexReadbackData_RT(TSharedPtr<FMeshingResult> Result);

	/** Copy index data from readback buffer into Result->ReadbackMeshData. Render thread only. */
	static void CopyIndexReadbackData_RT(TSharedPtr<FMeshingResult> Result);

	/** Dispatch the compute shader on the render thread */
	void DispatchComputeShader(
		const FVoxelMeshingRequest& Request,
		uint64 RequestId,
		TSharedPtr<FMeshingResult> Result,
		FOnVoxelMeshingComplete OnComplete);

	/** Pack voxel data for GPU upload */
	TArray<uint32> PackVoxelDataForGPU(const TArray<FVoxelData>& VoxelData);

	/** Create the triangle table buffer for GPU */
	TArray<int32> CreateTriangleTableData();
};
