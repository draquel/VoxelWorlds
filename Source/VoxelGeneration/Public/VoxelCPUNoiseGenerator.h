// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelNoiseGenerator.h"
#include "VoxelBiomeDefinition.h"

class FInfinitePlaneWorldMode;
class FIslandBowlWorldMode;
class FSphericalPlanetWorldMode;
struct FSphericalPlanetParams;
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

	virtual bool ReadbackToCPU(
		const FVoxelGenerationHandle& Handle,
		TArray<FVoxelData>& OutVoxelData) override;

	virtual void ReleaseHandle(const FVoxelGenerationHandle& Handle) override;

	// ==================== Noise Algorithms ====================

	/**
	 * Sample fBm (Fractal Brownian Motion) noise at a position: the noise every world mode's fields sample (terrain,
	 * continentalness, climate, caves, ores, the surface queries and the map). Since the world modes review's MP4 (WM-MP6,
	 * WM-MP7, decision D39; GenerationVersion 3) it is FVoxelPlanetNoise::FBM3D for heightmap worlds too: a hashed integer
	 * lattice with no period, split-origin sampling exact to float precision at any distance, and the continuous Simplex
	 * kernel (G43). The GPU's heightmap branch samples the same noise at a per-chunk origin (GenerateVoxelDensity.usf,
	 * PlanetNoise.ush), the two within float rounding of each other.
	 *
	 * @param Position World position to sample
	 * @param Params Noise parameters (octaves, frequency, etc.)
	 * @return Noise value in range approximately [-1, 1]
	 */
	static float FBM3D(const FVector& Position, const FVoxelNoiseParams& Params);

	/**
	 * Run the post-generation passes (water fill + underground classification) on already-generated
	 * voxel data. GenerateChunkCPU runs these inline; the GPU path runs shader ports of the same
	 * passes inside its generation graph (VoxelPostPasses.usf / AddVoxelPostPassDispatches), so this
	 * is the CPU reference implementation the shaders must stay bit-identical to (enforced by the
	 * GPUvsCPU parity tests). Static + state-free.
	 */
	static void ApplyPostReadbackPasses(const FVoxelNoiseGenerationRequest& Request, TArray<FVoxelData>& OutVoxelData);

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

	// ==================== Cave Generation Helpers ====================

	/**
	 * Sample a single cave layer's carve density at a position.
	 * Cheese: single noise field threshold carving.
	 * Spaghetti/Noodle: dual-noise field intersection carving.
	 *
	 * @param WorldPos World position to sample
	 * @param LayerConfig Configuration for this cave layer
	 * @param WorldSeed Base world seed
	 * @param Planet On a planet, VerticalScale acts along local up (FSphericalPlanetWorldMode::ScaleAltitude, SP-B6);
	 *        null in heightmap modes, where it scales world Z
	 * @return Carve density in range [0, 1] where 0 = no carving, 1 = full carving
	 */
	static float SampleCaveLayer(const FVector& WorldPos, const FCaveLayerConfig& LayerConfig, int32 WorldSeed,
		const FSphericalPlanetParams* Planet = nullptr);

public:
	/**
	 * Calculate total cave carve density at a position from all enabled layers.
	 * Applies depth constraints, biome scaling, and union composition (max).
	 *
	 * Public + static + state-free so FVoxelCaveQuery (the chunk-independent cave sampling facade)
	 * and any gameplay/AI caller can query cave presence without a loaded chunk. Keep bit-identical
	 * to the generation-time carving so the cave-presence heatmap matches real terrain.
	 *
	 * @param WorldPos World position to sample
	 * @param DepthBelowSurface Depth below terrain surface in voxels
	 * @param BiomeID Current biome ID for per-biome overrides
	 * @param CaveConfig Cave configuration data asset
	 * @param WorldSeed Base world seed
	 * @param bIsUnderwater The terrain surface above is under the water (caves keep UnderwaterMinDepth below it)
	 * @param Planet On a planet, the layers' VerticalScale acts along local up (SP-B6); null in heightmap modes
	 * @return Carve density in range [0, 1]
	 */
	static float CalculateCaveDensity(
		const FVector& WorldPos,
		float DepthBelowSurface,
		uint8 BiomeID,
		const UVoxelCaveConfiguration* CaveConfig,
		int32 WorldSeed,
		bool bIsUnderwater = false,
		const FSphericalPlanetParams* Planet = nullptr);

private:
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
	 * @param PlanetRelativePos On a planet: the voxel's position relative to the planet centre; the Rarity test then
	 *        uses OreRarityHash (spherical SP-H3). Null: the heightmap modes' position hash, unchanged
	 * @return True if ore should be placed here
	 */
	static bool CheckOreVeinPlacement(
		const FVector& WorldPos,
		float DepthBelowSurface,
		const TArray<struct FOreVeinConfig>& OreConfigs,
		int32 WorldSeed,
		uint8& OutMaterialID,
		const FVector* PlanetRelativePos = nullptr);

public:
	/**
	 * The integer key a planet's ore Rarity test hashes (spherical SP-H3): the voxel's planet-relative position rounded
	 * to whole uu (floor(x + 0.5) per axis). Exact on the CPU (double) and the GPU (float holds whole uu below 2^24, a
	 * 167 km radius), and the same at every LOD for the same world position.
	 *
	 * @param PlanetRelativePos Position relative to the planet centre
	 * @return The key
	 */
	static FIntVector OreRarityKey(const FVector& PlanetRelativePos);

	/**
	 * A planet's ore vein noise (spherical Phase 6, WM-BP5): the same 2-octave field at the ore's frequency and seed,
	 * by the planet noise (FVoxelPlanetNoise) at the planet-relative position, a streak squashed along one direction per
	 * ore type (GetPlanetOreStreakDirection). Exact at any radius; the planet's ores move with it.
	 *
	 * @param PlanetRelativePos Position relative to the planet centre
	 * @param OreConfig Ore vein configuration
	 * @param WorldSeed World seed
	 * @return Noise in [0, 1]
	 */
	static float SamplePlanetOreVeinNoise(const FVector& PlanetRelativePos, const struct FOreVeinConfig& OreConfig, int32 WorldSeed);

	/** A planet streak ore's direction: the heightmap formula at the origin (one per ore type, WM-BP5). */
	static FVector GetPlanetOreStreakDirection(const struct FOreVeinConfig& OreConfig);

	/** Where a planet ore samples its noise: the planet-relative position, squashed along the streak direction for a streak. */
	static FVector GetPlanetOreSamplePoint(const FVector& PlanetRelativePos, const struct FOreVeinConfig& OreConfig);

	/**
	 * A planet's ore Rarity value in [0, 1) (spherical SP-H3): an integer hash of the key and the seed, the same bits on
	 * the CPU and the GPU (GpuOreRarityHash in BiomeMaterial.ush). Replaces frac(sin(p . k)), whose argument near 10^7
	 * the GPU's float and the CPU's double round differently. An ore is kept where the value is at most its Rarity.
	 *
	 * @param Key OreRarityKey of the voxel
	 * @param Seed World seed
	 * @return Value in [0, 1), 24 bits
	 */
	static float OreRarityHash(const FIntVector& Key, int32 Seed);

private:

	// ==================== Water Fill Pass ====================

	/** Post-generation: mark air voxels below water level as water via column scan. */
	static void ApplyWaterFillPass(
		const FVoxelNoiseGenerationRequest& Request,
		TArray<FVoxelData>& OutVoxelData);

	/**
	 * ApplyWaterFillPass on a planet (SP-B8): the sea is the sphere of WaterRadius around the planet centre, and the
	 * open-sea seeding runs along lines in local down instead of world-Z columns. Same phases and barriers.
	 */
	static void ApplyRadialWaterFillPass(
		const FVoxelNoiseGenerationRequest& Request,
		TArray<FVoxelData>& OutVoxelData);

	/** Post-generation: mark air voxels below terrain surface as underground (caves, enclosed voids). */
	static void ApplyUndergroundClassificationPass(
		const FVoxelNoiseGenerationRequest& Request,
		TArray<FVoxelData>& OutVoxelData);
};
