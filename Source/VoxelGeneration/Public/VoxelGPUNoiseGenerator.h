// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelNoiseGenerator.h"
#include "RHIFwd.h"

// Forward declarations to avoid heavy RDG includes in header
class FRDGPooledBuffer;

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
