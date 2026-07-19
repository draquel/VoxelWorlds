// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelWorldMode.h"
#include "VoxelBiomeSnapshot.h"

class UVoxelBiomeConfiguration;

/**
 * Infinite Plane World Mode implementation.
 *
 * Generates heightmap-style terrain extending infinitely in X/Y with Z as height.
 * Terrain surface is determined by 2D noise (sampling only X,Y coordinates),
 * creating natural hills and valleys.
 *
 * Density Calculation (SDF):
 * - TerrainHeight = BaseHeight + SeaLevel + (NoiseValue * HeightScale)
 * - Density = TerrainHeight - WorldPos.Z
 * - Positive = solid (below surface), Negative = air (above surface)
 *
 * Material Assignment:
 * - Stone: depth > 4 voxels below surface
 * - Dirt: 1-4 voxels below surface
 * - Grass: at or near surface
 *
 * Configuration:
 * - SeaLevel: Base terrain elevation (world units)
 * - HeightScale: Noise amplitude multiplier for terrain height variation
 * - BaseHeight: Additional height offset
 *
 * @see IVoxelWorldMode
 */
class VOXELGENERATION_API FInfinitePlaneWorldMode : public IVoxelWorldMode
{
public:
	/**
	 * Construct with default terrain parameters.
	 */
	FInfinitePlaneWorldMode();

	/**
	 * Construct with specified terrain parameters.
	 *
	 * @param InTerrainParams Terrain generation parameters
	 */
	explicit FInfinitePlaneWorldMode(const FWorldModeTerrainParams& InTerrainParams);

	virtual ~FInfinitePlaneWorldMode() = default;

	// ==================== IVoxelWorldMode Interface ====================

	virtual float GetDensityAt(
		const FVector& WorldPos,
		int32 LODLevel,
		float NoiseValue) const override;

	virtual float GetTerrainHeightAt(
		float X,
		float Y,
		const FVoxelNoiseParams& NoiseParams) const override;

	virtual FIntVector WorldToChunkCoord(
		const FVector& WorldPos,
		int32 ChunkSize,
		float VoxelSize) const override;

	virtual FVector ChunkCoordToWorld(
		const FIntVector& ChunkCoord,
		int32 ChunkSize,
		float VoxelSize,
		int32 LODLevel) const override;

	virtual int32 GetMinZ() const override;
	virtual int32 GetMaxZ() const override;

	virtual EWorldMode GetWorldModeType() const override { return EWorldMode::InfinitePlane; }
	virtual bool IsHeightmapBased() const override { return true; }

	virtual uint8 GetMaterialAtDepth(
		const FVector& WorldPos,
		float SurfaceHeight,
		float DepthBelowSurface) const override;

	// ==================== Terrain Configuration ====================

	/**
	 * Get the terrain parameters.
	 */
	const FWorldModeTerrainParams& GetTerrainParams() const { return TerrainParams; }

	/**
	 * Set the terrain parameters.
	 */
	void SetTerrainParams(const FWorldModeTerrainParams& InParams) { TerrainParams = InParams; }

	/**
	 * Provide the biome configuration used for continentalness height modulation.
	 *
	 * When set (and continentalness is enabled on the config), GetTerrainHeightAt applies the SAME
	 * height modulation (BaseHeight offset + HeightScale multiplier) that generation applies, so the
	 * analytic height used for spawn / nav / POI placement matches the real generated surface.
	 *
	 * The config is snapshotted BY VALUE at this call (game thread) — the mode holds no UObject
	 * reference afterwards, so the instance is safe to share into background tasks that may outlive
	 * the world (e.g. VoxelMap tile generation). Config changes after this call are not picked up
	 * (configs are static after world init).
	 *
	 * @param InBiomeConfig Biome configuration, or null to use raw (un-modulated) base terrain height.
	 */
	virtual void SetBiomeContext(const UVoxelBiomeConfiguration* InBiomeConfig) override
	{
		BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(InBiomeConfig);
	}

	/** Per-mode vertical-cull bounds (IVoxelWorldMode): wraps the static using this mode's params + biome snapshot. */
	virtual void GetTerrainHeightBounds(float& OutMin, float& OutMax) const override
	{
		FInfinitePlaneWorldMode::GetTerrainHeightBounds(TerrainParams, &BiomeSnapshot, OutMin, OutMax);
	}

	/**
	 * Get sea level height.
	 */
	float GetSeaLevel() const { return TerrainParams.SeaLevel; }

	/**
	 * Get height scale (noise amplitude multiplier).
	 */
	float GetHeightScale() const { return TerrainParams.HeightScale; }

	/**
	 * Get base height offset.
	 */
	float GetBaseHeight() const { return TerrainParams.BaseHeight; }

	// ==================== Static Helpers ====================

	/**
	 * Sample 2D noise for terrain height.
	 * Uses only X,Y coordinates for heightmap generation.
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param NoiseParams Noise generation parameters
	 * @return Noise value in range [-1, 1]
	 */
	static float SampleTerrainNoise2D(
		float X,
		float Y,
		const FVoxelNoiseParams& NoiseParams);

	/**
	 * Convert terrain noise to height.
	 *
	 * @param NoiseValue Noise sample in range [-1, 1]
	 * @param TerrainParams Terrain configuration
	 * @return World Z height for terrain surface
	 */
	static float NoiseToTerrainHeight(
		float NoiseValue,
		const FWorldModeTerrainParams& TerrainParams);

	/**
	 * Apply continentalness height modulation to base terrain params — the SINGLE SOURCE OF TRUTH
	 * shared by chunk generation and the analytic GetTerrainHeightAt query.
	 *
	 * Samples the continentalness noise field at (X,Y) (same params as generation: 2-octave Simplex,
	 * seed = BaseNoiseParams.Seed + snapshot seed offset) and offsets BaseHeight / scales HeightScale via
	 * the snapshot's baked continentalness curves. Returns BaseParams unchanged when BiomeSnapshot is
	 * null or continentalness is disabled. Pure and thread-safe (the snapshot is plain data — build it
	 * once per batch with FVoxelBiomeSnapshot::FromConfig, on the game thread).
	 *
	 * @param X,Y             World XY sample position
	 * @param BaseParams      Un-modulated terrain params
	 * @param BaseNoiseParams Terrain noise params (only Seed is used, to derive the continentalness seed)
	 * @param BiomeSnapshot   Value-captured biome data providing continentalness curves, or null
	 * @param OutContinentalness Sampled continentalness in [-1,1] (0 when disabled) — reusable by callers
	 * @return Effective terrain params with continentalness applied
	 */
	static FWorldModeTerrainParams ComputeEffectiveTerrainParams(
		float X,
		float Y,
		const FWorldModeTerrainParams& BaseParams,
		const FVoxelNoiseParams& BaseNoiseParams,
		const FVoxelBiomeSnapshot* BiomeSnapshot,
		float& OutContinentalness);

	/**
	 * Worst-case terrain-height bounds [OutMin, OutMax] over ALL (X,Y) for this heightmap mode — the
	 * authority for vertical chunk-culling bounds (VoxelLOD). Co-located with the height math so it
	 * stays in sync as height-affecting features are added (add a feature -> update it here, next to
	 * NoiseToTerrainHeight / ComputeEffectiveTerrainParams).
	 *
	 * Accounts for the base noise amplitude (BaseHeight ± HeightScale) AND continentalness (height
	 * offset range + max scale multiplier). The returned range is a conservative superset that CONTAINS
	 * every value the generator can produce at any (X,Y), so culling to it can never remove a chunk that
	 * holds real terrain. Callers add their own per-chunk buffer for meshing / edit slack.
	 *
	 * @param BaseParams    Un-modulated terrain params (SeaLevel, BaseHeight, HeightScale)
	 * @param BiomeSnapshot Value-captured biome data for the continentalness extent, or null (base range only)
	 * @param OutMin,OutMax  Guaranteed-containing terrain height range (world Z)
	 */
	static void GetTerrainHeightBounds(
		const FWorldModeTerrainParams& BaseParams,
		const FVoxelBiomeSnapshot* BiomeSnapshot,
		float& OutMin,
		float& OutMax);

	/**
	 * Calculate signed distance to terrain surface.
	 *
	 * @param WorldZ World Z coordinate of sample point
	 * @param TerrainHeight Z height of terrain surface at this X,Y
	 * @return Signed distance (positive = below surface/solid)
	 */
	static float CalculateSignedDistance(
		float WorldZ,
		float TerrainHeight);

	/**
	 * Convert signed distance to voxel density [0-255].
	 * Uses smooth falloff for surface blending.
	 *
	 * @param SignedDistance Distance to surface (positive = inside)
	 * @param VoxelSize Size of voxel for normalization
	 * @return Density value where 127 = surface threshold
	 */
	static uint8 SignedDistanceToDensity(
		float SignedDistance,
		float VoxelSize);

private:
	/** Terrain generation parameters */
	FWorldModeTerrainParams TerrainParams;

	/**
	 * Value-captured biome data for continentalness height modulation in GetTerrainHeightAt
	 * (snapshotted from the config in SetBiomeContext). Plain data, no UObject reference — safe on
	 * any thread and for tasks that outlive the world. Default (never set) => raw base terrain height.
	 */
	FVoxelBiomeSnapshot BiomeSnapshot;

	/** Practical vertical limits for chunk generation */
	static constexpr int32 MIN_Z_CHUNKS = -64;
	static constexpr int32 MAX_Z_CHUNKS = 64;
};
