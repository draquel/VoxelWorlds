// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelNoiseGenerator.h"
#include "RHIFwd.h"

// Forward declarations to avoid heavy RDG includes in header
class FRDGPooledBuffer;
class FRHIGPUBufferReadback;

/**
 * Result of polling a non-blocking GPU generation readback.
 * Pending = compute/readback still in flight; Ready = OutVoxelData filled; Failed = handle invalid/error.
 */
enum class EVoxelGPUReadbackStatus : uint8
{
	Pending,
	Ready,
	Failed
};

/**
 * GPU-based noise generator for voxel terrain.
 *
 * Uses compute shaders to generate voxel density data on the GPU.
 * This is the high-performance implementation for runtime terrain generation.
 *
 * Performance: ~0.1-1ms per 32^3 chunk on modern GPUs
 * Thread Safety: Must be called from render thread or with proper synchronization
 *
 * @see IVoxelNoiseGenerator
 * @see FVoxelCPUNoiseGenerator
 */
class VOXELGENERATION_API FVoxelGPUNoiseGenerator : public IVoxelNoiseGenerator
{
public:
	FVoxelGPUNoiseGenerator();
	virtual ~FVoxelGPUNoiseGenerator();

	// ==================== IVoxelNoiseGenerator Interface ====================

	virtual void Initialize() override;
	virtual void Shutdown() override;
	virtual bool IsInitialized() const override { return bIsInitialized; }

	virtual FVoxelGenerationHandle GenerateChunkAsync(
		const FVoxelNoiseGenerationRequest& Request,
		FOnVoxelGenerationComplete OnComplete) override;

	virtual bool GenerateChunkCPU(
		const FVoxelNoiseGenerationRequest& Request,
		TArray<FVoxelData>& OutVoxelData) override;

	virtual float SampleNoiseAt(
		const FVector& WorldPosition,
		const FVoxelNoiseParams& Params) override;

	virtual FRHIBuffer* GetGeneratedBuffer(const FVoxelGenerationHandle& Handle) override;

	virtual bool ReadbackToCPU(
		const FVoxelGenerationHandle& Handle,
		TArray<FVoxelData>& OutVoxelData) override;

	virtual void ReleaseHandle(const FVoxelGenerationHandle& Handle) override;

	// ==================== Non-blocking streaming API ====================
	// Poll-based GPU generation for the chunk-manager streaming path. Unlike the delegate-based
	// GenerateChunkAsync + blocking ReadbackToCPU (test/editor use), this dispatches the compute pass
	// and an FRHIGPUBufferReadback copy, then hands ownership of readiness to the caller who polls
	// each frame — no thread is ever stalled (option (a) integration model).

	/**
	 * Dispatch density generation for a chunk on the GPU and enqueue an async readback.
	 * Non-blocking: returns immediately with a handle to poll via PollGenerateChunkGPU.
	 *
	 * @param Request Generation request parameters
	 * @return Handle for polling the readback (RequestId != 0 on success)
	 */
	FVoxelGenerationHandle BeginGenerateChunkGPU(const FVoxelNoiseGenerationRequest& Request);

	/**
	 * Poll a pending GPU generation started with BeginGenerateChunkGPU.
	 * Call once per frame on the game thread until it returns Ready or Failed.
	 *
	 * @param Handle Handle from BeginGenerateChunkGPU
	 * @param OutVoxelData Filled with unpacked voxel data when the status is Ready
	 * @return Pending (still in flight), Ready (OutVoxelData filled), or Failed (invalid handle/error)
	 */
	EVoxelGPUReadbackStatus PollGenerateChunkGPU(const FVoxelGenerationHandle& Handle, TArray<FVoxelData>& OutVoxelData);

private:
	bool bIsInitialized = false;

	/** Counter for generating unique request IDs */
	std::atomic<uint64> NextRequestId{1};

	/** Stored generation results */
	struct FGenerationResult
	{
		TRefCountPtr<FRDGPooledBuffer> OutputBuffer;
		TRefCountPtr<FRDGPooledBuffer> StagingBuffer;
		int32 ChunkSize = 0;
		bool bReadbackComplete = false;
		TArray<FVoxelData> CachedData;

		// ---- Non-blocking streaming readback (BeginGenerateChunkGPU / PollGenerateChunkGPU) ----
		/** Async GPU→CPU readback for the density buffer; owned here, deleted after the Lock completes. */
		FRHIGPUBufferReadback* DensityReadback = nullptr;
		/** Total voxel count (ChunkSize^3) — cached so the poll knows the readback byte size. */
		int32 TotalVoxels = 0;
		/** Set true on the render thread AFTER EnqueueCopy is recorded — guards IsReady() polling. */
		std::atomic<bool> bReadbackEnqueued{false};
		/** Set true on the render thread once CachedData has been filled from the mapped readback. */
		std::atomic<bool> bDataReady{false};
		/** Game-thread-only: true once the Lock render command has been enqueued (avoids double-enqueue). */
		bool bLockInFlight = false;

		FGenerationResult() = default;

		/**
		 * Safety net for an abandoned readback. Shutdown / ReleaseHandle call FlushRenderingCommands()
		 * before dropping their reference, so any pending GPU copy is complete before this runs; the
		 * normal completion path in PollGenerateChunkGPU deletes + nulls DensityReadback itself.
		 */
		~FGenerationResult()
		{
			if (DensityReadback)
			{
				delete DensityReadback;
				DensityReadback = nullptr;
			}
		}
	};

	TMap<uint64, TSharedPtr<FGenerationResult>> GenerationResults;
	FCriticalSection ResultsLock;

	/** Dispatch the compute shader on the render thread */
	void DispatchComputeShader(
		const FVoxelNoiseGenerationRequest& Request,
		uint64 RequestId,
		TSharedPtr<FGenerationResult> Result,
		FOnVoxelGenerationComplete OnComplete);

	/** Create the structured buffer for output */
	FBufferRHIRef CreateOutputBuffer(int32 ChunkSize);

	/** Create a staging buffer for CPU readback */
	FBufferRHIRef CreateStagingBuffer(int32 ChunkSize);
};
