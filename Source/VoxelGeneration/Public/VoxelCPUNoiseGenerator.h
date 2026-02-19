// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelNoiseGenerator.h"
#include "VoxelBiomeDefinition.h"

class FInfinitePlaneWorldMode;
class FIslandBowlWorldMode;
class FSphericalPlanetWorldMode;
class UVoxelCaveConfiguration;
struct FCaveLayerConfig;

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

	/**
	 * Sample 3D Cellular (Worley) noise at a position.
	 * Uses 3x3x3 cell search to find distances to nearest two feature points.
	 *
	 * @param Position World position to sample (pre-scaled by frequency)
	 * @param Seed Random seed
	 * @param OutF1 Distance to nearest feature point
	 * @param OutF2 Distance to second nearest feature point
	 */
	static void Cellular3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2);

	/**
	 * Sample 3D Voronoi noise at a position.
	 * Same as Cellular but also returns a stable cell ID.
	 *
	 * @param Position World position to sample (pre-scaled by frequency)
	 * @param Seed Random seed
	 * @param OutF1 Distance to nearest feature point
	 * @param OutF2 Distance to second nearest feature point
	 * @param OutCellID Stable hash-based cell identifier [0, 1]
	 */
	static void Voronoi3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2, float& OutCellID);

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

	/**
	 * Generate chunk using Island Bowl world mode (bounded terrain with falloff).
	 */
	void GenerateChunkIslandBowl(
		const FVoxelNoiseGenerationRequest& Request,
		const FIslandBowlWorldMode& WorldMode,
		TArray<FVoxelData>& OutVoxelData);

	/**
	 * Generate chunk using Spherical Planet world mode (radial terrain on sphere).
	 */
	void GenerateChunkSphericalPlanet(
		const FVoxelNoiseGenerationRequest& Request,
		const FSphericalPlanetWorldMode& WorldMode,
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

	// ==================== Cave Generation Helpers ====================

	/**
	 * Sample a single cave layer's carve density at a position.
	 * Cheese: single noise field threshold carving.
	 * Spaghetti/Noodle: dual-noise field intersection carving.
	 *
	 * @param WorldPos World position to sample
	 * @param LayerConfig Configuration for this cave layer
	 * @param WorldSeed Base world seed
	 * @return Carve density in range [0, 1] where 0 = no carving, 1 = full carving
	 */
	static float SampleCaveLayer(const FVector& WorldPos, const FCaveLayerConfig& LayerConfig, int32 WorldSeed);

	/**
	 * Calculate total cave carve density at a position from all enabled layers.
	 * Applies depth constraints, biome scaling, and union composition (max).
	 *
	 * @param WorldPos World position to sample
	 * @param DepthBelowSurface Depth below terrain surface in voxels
	 * @param BiomeID Current biome ID for per-biome overrides
	 * @param CaveConfig Cave configuration data asset
	 * @param WorldSeed Base world seed
	 * @return Carve density in range [0, 1]
	 */
	static float CalculateCaveDensity(
		const FVector& WorldPos,
		float DepthBelowSurface,
		uint8 BiomeID,
		const UVoxelCaveConfiguration* CaveConfig,
		int32 WorldSeed,
		bool bIsUnderwater = false);

	// ==================== Ore Vein Helpers ====================

	/**
	 * Sample ore vein noise at a position.
	 * @param WorldPos World position to sample
	 * @param OreConfig Configuration for this ore type
	 * @param WorldSeed Base world seed
	 * @return Normalized noise value in range [0, 1]
	 */
	static float SampleOreVeinNoise(const FVector& WorldPos, const struct FOreVeinConfig& OreConfig, int32 WorldSeed);

	/**
	 * Check if an ore vein should be placed at a position and return the ore material.
	 * @param WorldPos World position to check
	 * @param DepthBelowSurface Depth below terrain surface in voxels
	 * @param OreConfigs Array of ore configurations to check
	 * @param WorldSeed Base world seed
	 * @param OutMaterialID Output material ID if ore found
	 * @return True if ore should be placed here
	 */
	static bool CheckOreVeinPlacement(
		const FVector& WorldPos,
		float DepthBelowSurface,
		const TArray<struct FOreVeinConfig>& OreConfigs,
		int32 WorldSeed,
		uint8& OutMaterialID);

	// ==================== Water Fill Pass ====================

	/** Post-generation: mark air voxels below water level as water via column scan. */
	static void ApplyWaterFillPass(
		const FVoxelNoiseGenerationRequest& Request,
		TArray<FVoxelData>& OutVoxelData);
};
