// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelNoiseGenerator.h"

class FInfinitePlaneWorldMode;

/**
 * CPU-based noise generator for voxel terrain.
 *
 * Implements Perlin and Simplex 3D noise with fBm (Fractal Brownian Motion).
 * This is the fallback implementation for testing and editor scenarios.
 *
 * Performance: ~10-50ms per 32^3 chunk depending on octaves
 * Thread Safety: All methods are thread-safe
 *
 * @see IVoxelNoiseGenerator
 * @see FVoxelGPUNoiseGenerator
 */
class VOXELGENERATION_API FVoxelCPUNoiseGenerator : public IVoxelNoiseGenerator
{
public:
	FVoxelCPUNoiseGenerator();
	virtual ~FVoxelCPUNoiseGenerator();

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

	// ==================== Noise Algorithms ====================

	/**
	 * Sample 3D Perlin noise at a position.
	 *
	 * @param Position World position to sample
	 * @param Seed Random seed for permutation table
	 * @return Noise value in range [-1, 1]
	 */
	static float Perlin3D(const FVector& Position, int32 Seed = 0);

	/**
	 * Sample 3D Simplex noise at a position.
	 *
	 * @param Position World position to sample
	 * @param Seed Random seed for permutation table
	 * @return Noise value in range [-1, 1]
	 */
	static float Simplex3D(const FVector& Position, int32 Seed = 0);

	/**
	 * Sample fBm (Fractal Brownian Motion) noise at a position.
	 * Combines multiple octaves of noise for natural-looking terrain.
	 *
	 * @param Position World position to sample
	 * @param Params Noise parameters (octaves, frequency, etc.)
	 * @return Noise value in range approximately [-1, 1]
	 */
	static float FBM3D(const FVector& Position, const FVoxelNoiseParams& Params);

private:
	bool bIsInitialized = false;

	/** Counter for generating unique request IDs */
	std::atomic<uint64> NextRequestId{1};

	/** Stored results for async requests (CPU just completes immediately) */
	TMap<uint64, TArray<FVoxelData>> StoredResults;
	FCriticalSection ResultsLock;

	// ==================== World Mode Generation ====================

	/**
	 * Generate chunk using Infinite Plane world mode (2D heightmap).
	 */
	void GenerateChunkInfinitePlane(
		const FVoxelNoiseGenerationRequest& Request,
		const FInfinitePlaneWorldMode& WorldMode,
		TArray<FVoxelData>& OutVoxelData);

	/**
	 * Generate chunk using full 3D noise (for volumetric modes).
	 */
	void GenerateChunk3DNoise(
		const FVoxelNoiseGenerationRequest& Request,
		TArray<FVoxelData>& OutVoxelData);

	// ==================== Noise Helper Functions ====================

	/** Fade function for smooth interpolation */
	static float Fade(float T);

	/** Linear interpolation */
	static float Lerp(float A, float B, float T);

	/** Gradient function for Perlin noise */
	static float Grad(int32 Hash, float X, float Y, float Z);

	/** Get permutation value with seed offset */
	static int32 Perm(int32 Index, int32 Seed = 0);

	/** Hash function for simplex noise */
	static int32 Hash(int32 I, int32 Seed = 0);

	/** Floor function that returns int32 */
	static int32 FastFloor(float X);

	/** Dot product for simplex gradient */
	static float SimplexDot(const int32* G, float X, float Y, float Z);
};
