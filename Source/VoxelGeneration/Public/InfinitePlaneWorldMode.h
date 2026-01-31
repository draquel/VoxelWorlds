// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelWorldMode.h"

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

	/** Practical vertical limits for chunk generation */
	static constexpr int32 MIN_Z_CHUNKS = -64;
	static constexpr int32 MAX_Z_CHUNKS = 64;
};
