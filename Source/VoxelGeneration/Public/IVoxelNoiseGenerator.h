// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"

class FRHICommandListImmediate;
class FRHIBuffer;

/**
 * Abstract interface for voxel noise generation.
 *
 * Provides both GPU (async) and CPU (blocking) generation paths.
 * GPU generation uses compute shaders for maximum performance,
 * while CPU fallback is available for testing and editor scenarios.
 *
 * Usage:
 * 1. Call Initialize() once at startup
 * 2. Use GenerateChunkAsync() for runtime GPU generation
 * 3. Use GenerateChunkCPU() for testing or editor use
 * 4. Call Shutdown() before destruction
 *
 * @see FVoxelGPUNoiseGenerator
 * @see FVoxelCPUNoiseGenerator
 */
class VOXELGENERATION_API IVoxelNoiseGenerator
{
public:
	virtual ~IVoxelNoiseGenerator() = default;

	// ==================== Lifecycle ====================

	/**
	 * Initialize the generator.
	 * Must be called before any generation operations.
	 */
	virtual void Initialize() = 0;

	/**
	 * Shutdown the generator and release resources.
	 * Must be called before destruction.
	 */
	virtual void Shutdown() = 0;

	/**
	 * Check if the generator is initialized and ready for use.
	 */
	virtual bool IsInitialized() const = 0;

	// ==================== Generation ====================

	/**
	 * Generate voxel data for a chunk asynchronously on the GPU.
	 *
	 * @param Request Generation request parameters
	 * @param OnComplete Callback when generation finishes
	 * @return Handle for tracking the async operation
	 */
	virtual FVoxelGenerationHandle GenerateChunkAsync(
		const FVoxelNoiseGenerationRequest& Request,
		FOnVoxelGenerationComplete OnComplete) = 0;

	/**
	 * Generate voxel data for a chunk synchronously on the CPU.
	 * This is a blocking operation suitable for testing and editor use.
	 *
	 * @param Request Generation request parameters
	 * @param OutVoxelData Output array to fill with voxel data
	 * @return True if generation succeeded
	 */
	virtual bool GenerateChunkCPU(
		const FVoxelNoiseGenerationRequest& Request,
		TArray<FVoxelData>& OutVoxelData) = 0;

	/**
	 * Sample noise at a single world position.
	 * Useful for debugging and point queries.
	 *
	 * @param WorldPosition Position in world space
	 * @param Params Noise parameters
	 * @return Noise value in range [-1, 1]
	 */
	virtual float SampleNoiseAt(
		const FVector& WorldPosition,
		const FVoxelNoiseParams& Params) = 0;

	// ==================== Buffer Access ====================

	/**
	 * Get the GPU buffer for a completed generation request.
	 * Only valid after GenerateChunkAsync completes.
	 *
	 * @param Handle Handle from GenerateChunkAsync
	 * @return Pointer to the GPU buffer, or nullptr if not available
	 */
	virtual FRHIBuffer* GetGeneratedBuffer(const FVoxelGenerationHandle& Handle) = 0;

	/**
	 * Read back generated voxel data from GPU to CPU.
	 * This is a blocking operation that stalls the GPU.
	 *
	 * @param Handle Handle from GenerateChunkAsync
	 * @param OutVoxelData Output array to fill with voxel data
	 * @return True if readback succeeded
	 */
	virtual bool ReadbackToCPU(
		const FVoxelGenerationHandle& Handle,
		TArray<FVoxelData>& OutVoxelData) = 0;

	/**
	 * Release resources associated with a generation handle.
	 * Call this when you're done with the generated data.
	 *
	 * @param Handle Handle to release
	 */
	virtual void ReleaseHandle(const FVoxelGenerationHandle& Handle) = 0;

	// ==================== Utilities ====================

	/**
	 * Convert noise value [-1, 1] to voxel density [0, 255].
	 * Surface threshold (127) corresponds to noise value of 0.
	 */
	static uint8 NoiseToDensity(float NoiseValue)
	{
		// Clamp to [-1, 1], then map to [0, 255]
		float Clamped = FMath::Clamp(NoiseValue, -1.0f, 1.0f);
		return static_cast<uint8>(FMath::Clamp((Clamped + 1.0f) * 127.5f, 0.0f, 255.0f));
	}

	/**
	 * Convert voxel density [0, 255] to noise value [-1, 1].
	 */
	static float DensityToNoise(uint8 Density)
	{
		return (static_cast<float>(Density) / 127.5f) - 1.0f;
	}
};
