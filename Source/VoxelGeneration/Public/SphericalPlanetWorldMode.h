// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelWorldMode.h"

/**
 * Spherical planet configuration parameters.
 */
struct VOXELGENERATION_API FSphericalPlanetParams
{
	/** Radius of the planet surface in world units */
	float PlanetRadius = 100000.0f;

	/** Maximum terrain height above PlanetRadius */
	float MaxTerrainHeight = 5000.0f;

	/** Maximum terrain depth below PlanetRadius (caves, valleys) */
	float MaxTerrainDepth = 2000.0f;

	/** Center of the planet in world space */
	FVector PlanetCenter = FVector::ZeroVector;

	/** Scale for noise sampling on sphere surface (lower = larger features) */
	float NoiseScale = 0.00005f;

	FSphericalPlanetParams() = default;

	FSphericalPlanetParams(float InRadius, const FVector& InCenter = FVector::ZeroVector)
		: PlanetRadius(InRadius)
		, PlanetCenter(InCenter)
	{
	}

	/** Get the inner shell radius (planet surface minus max depth) */
	float GetInnerRadius() const
	{
		return PlanetRadius - MaxTerrainDepth;
	}

	/** Get the outer shell radius (planet surface plus max height) */
	float GetOuterRadius() const
	{
		return PlanetRadius + MaxTerrainHeight;
	}
};

/**
 * Spherical Planet World Mode implementation.
 *
 * Generates terrain on a spherical surface. Terrain height is radial
 * displacement from the planet's base radius, with noise sampled using
 * the direction vector from the planet center.
 *
 * Coordinate System:
 * - Planet center is at PlanetCenter (typically WorldOrigin)
 * - Radial distance determines "height" above/below surface
 * - Noise is sampled using normalized direction (latitude/longitude-like)
 *
 * Density Calculation:
 * - Sample terrain noise using direction vector to point
 * - Calculate terrain radius = PlanetRadius + NoiseHeight
 * - Density = TerrainRadius - DistanceFromCenter (positive = inside solid)
 *
 * @see IVoxelWorldMode
 * @see FInfinitePlaneWorldMode
 */
class VOXELGENERATION_API FSphericalPlanetWorldMode : public IVoxelWorldMode
{
public:
	/**
	 * Construct with default parameters.
	 */
	FSphericalPlanetWorldMode();

	/**
	 * Construct with specified parameters.
	 *
	 * @param InTerrainParams Base terrain generation parameters (HeightScale used for radial displacement)
	 * @param InPlanetParams Planet-specific parameters
	 */
	FSphericalPlanetWorldMode(
		const FWorldModeTerrainParams& InTerrainParams,
		const FSphericalPlanetParams& InPlanetParams);

	virtual ~FSphericalPlanetWorldMode() = default;

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

	virtual EWorldMode GetWorldModeType() const override { return EWorldMode::SphericalPlanet; }
	virtual bool IsHeightmapBased() const override { return false; }  // 3D terrain, not heightmap

	virtual uint8 GetMaterialAtDepth(
		const FVector& WorldPos,
		float SurfaceHeight,
		float DepthBelowSurface) const override;

	// ==================== Configuration ====================

	const FWorldModeTerrainParams& GetTerrainParams() const { return TerrainParams; }
	void SetTerrainParams(const FWorldModeTerrainParams& InParams) { TerrainParams = InParams; }

	const FSphericalPlanetParams& GetPlanetParams() const { return PlanetParams; }
	void SetPlanetParams(const FSphericalPlanetParams& InParams) { PlanetParams = InParams; }

	// ==================== Static Helpers ====================

	/**
	 * Calculate the distance from a point to the planet center.
	 *
	 * @param WorldPos World position
	 * @param PlanetCenter Center of the planet
	 * @return Distance from planet center
	 */
	static float CalculateRadialDistance(
		const FVector& WorldPos,
		const FVector& PlanetCenter);

	/**
	 * Get the normalized direction from planet center to a point.
	 *
	 * @param WorldPos World position
	 * @param PlanetCenter Center of the planet
	 * @return Normalized direction vector (or UpVector if at center)
	 */
	static FVector GetDirectionFromCenter(
		const FVector& WorldPos,
		const FVector& PlanetCenter);

	/**
	 * Sample spherical noise using direction vector.
	 * Converts direction to pseudo-UV coordinates for 2D noise sampling.
	 *
	 * @param Direction Normalized direction from planet center
	 * @param NoiseParams Noise sampling parameters
	 * @return Noise value in range [-1, 1]
	 */
	static float SampleSphericalNoise(
		const FVector& Direction,
		const FVoxelNoiseParams& NoiseParams);

	/**
	 * Convert noise value to radial terrain displacement.
	 *
	 * @param NoiseValue Raw noise value [-1, 1]
	 * @param TerrainParams Terrain scaling parameters
	 * @return Radial displacement from PlanetRadius
	 */
	static float NoiseToRadialDisplacement(
		float NoiseValue,
		const FWorldModeTerrainParams& TerrainParams);

	/**
	 * Calculate signed distance from a point to the terrain surface.
	 *
	 * @param DistFromCenter Distance from planet center
	 * @param TerrainRadius Radius of terrain at this direction (PlanetRadius + displacement)
	 * @return Signed distance (positive = inside solid, negative = air)
	 */
	static float CalculateSignedDistance(
		float DistFromCenter,
		float TerrainRadius);

	/**
	 * Check if a position is within the planet's terrain shell.
	 *
	 * @param WorldPos World position
	 * @param PlanetParams Planet configuration
	 * @return True if within inner/outer radius bounds
	 */
	static bool IsWithinPlanetBounds(
		const FVector& WorldPos,
		const FSphericalPlanetParams& PlanetParams);

private:
	/** Base terrain generation parameters */
	FWorldModeTerrainParams TerrainParams;

	/** Planet-specific parameters */
	FSphericalPlanetParams PlanetParams;

	/** Practical chunk range for spherical planet (relative to viewer) */
	static constexpr int32 CHUNK_RANGE = 32;
};
