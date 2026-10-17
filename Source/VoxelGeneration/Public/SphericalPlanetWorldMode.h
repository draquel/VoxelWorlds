// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelWorldMode.h"
#include "VoxelBiomeSnapshot.h"

/**
 * Spherical planet configuration parameters. Build them from a world configuration with
 * FVoxelWorldModeFactory::MakePlanetParams (SP-A6).
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
 * - The 2D fields (terrain noise, continentalness, temperature, moisture) are sampled at the point on the datum
 *   sphere along the direction from the centre (GetNoiseSamplePoint), so features keep their size in world units
 *   on any radius and the two hemispheres differ (spherical SP-B1)
 *
 * Density Calculation (SampleRadialDisplacement is the one surface function: generation, GetSurfacePoint and the
 * configuration's shell check share it):
 * - Sample terrain noise and continentalness at the direction's noise sample point
 * - Displacement = BaseHeight + offset + noise * HeightScale * multiplier / 2, continentalness giving the offset
 *   and the multiplier as in the heightmap modes (SP-B5)
 * - Calculate terrain radius = PlanetRadius + displacement
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

	/**
	 * Snapshot the biome configuration for continentalness (SP-B5), by value: GetSurfacePoint then applies the same
	 * height modulation generation does. No UObject reference is kept.
	 */
	virtual void SetBiomeContext(const UVoxelBiomeConfiguration* InBiomeConfig) override
	{
		BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(InBiomeConfig);
	}

	/** The surface along the direction from the planet centre to WorldPos, with its outward normal (SP-A1). */
	virtual FVoxelTerrainSurfacePoint GetSurfacePoint(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams) const override;

	// ==================== World Bounds and Local Frame (IVoxelWorldMode, WM-A2; SP-A1, SP-A2) ====================
	// The interface fits a planet: up is radial, altitude is above PlanetRadius, the surface coordinate is a cube face
	// with the tangent warp (spherical Phase 1, WORLD_MODES_REVIEW_PLAN.md §15). No 2D system consumes the coordinate
	// yet (Phase 4).

	/** Whether the vertical line at (X, Y) meets the terrain shell (within its outer radius in XY). */
	virtual bool IsInsideWorld(double X, double Y) const override;

	/** The terrain shell: PlanetRadius - MaxTerrainDepth to PlanetRadius + MaxTerrainHeight around the planet centre. */
	virtual FVoxelWorldBounds GetWorldBounds() const override;

	/** The radial direction from the planet centre (+Z at the centre itself). */
	virtual FVector GetUpVector(const FVector& WorldPos) const override;

	/** Distance from the planet centre minus PlanetRadius. */
	virtual float GetAltitude(const FVector& WorldPos) const override;

	/**
	 * The cube face the radial direction points through (0..5 = +X, -X, +Y, -Y, +Z, -Z) and (U, V) in [-1, 1] on it:
	 * the gnomonic projection onto the face, warped per axis by u = (4 / pi) * atan(g) (the tangent warp). Equal UV
	 * cells cover areas within about 1.41x of each other (the plain gnomonic map: 5.2x); along a face's centre lines
	 * UV is proportional to the angle, so a UV step du spans about PlanetRadius * pi / 4 * du on the datum sphere.
	 */
	virtual FVoxelSurfaceCoord WorldToSurface(const FVector& WorldPos) const override;

	/** The point at Altitude above PlanetRadius along the direction through (Face, U, V). */
	virtual FVector SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const override;

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
	 * Where the planet's 2D fields (terrain noise, continentalness, temperature, moisture) are sampled for a direction:
	 * the point on the datum sphere, relative to the planet centre (SP-B1). A feature is about 1 / Frequency uu across on
	 * the surface whatever the radius, as on the plane, and a 3D field has no seam and no mirror between hemispheres.
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius Datum radius
	 * @return Noise sample position
	 */
	static FVector GetNoiseSamplePoint(const FVector& Direction, float PlanetRadius)
	{
		return Direction * PlanetRadius;
	}

	/**
	 * Terrain noise by direction: FBM3D at GetNoiseSamplePoint (SP-B1).
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius Datum radius
	 * @param NoiseParams Terrain noise parameters
	 * @return Noise value in range [-1, 1]
	 */
	static float SampleSphericalNoise(
		const FVector& Direction,
		float PlanetRadius,
		const FVoxelNoiseParams& NoiseParams);

	/**
	 * The terrain params at a direction with continentalness applied: BaseHeight plus the height offset, HeightScale times
	 * the multiplier, the continentalness field sampled at GetNoiseSamplePoint (SP-B5). The same noise field and curves
	 * as FInfinitePlaneWorldMode::ComputeEffectiveTerrainParams. BaseParams unchanged without a snapshot or with
	 * continentalness off.
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius Datum radius
	 * @param BaseParams Un-modulated terrain params (HeightScale = PlanetHeightScale)
	 * @param NoiseParams Terrain noise params (the seed derives the continentalness seed)
	 * @param BiomeSnapshot Biome data with the continentalness curves, or null
	 * @param OutContinentalness Sampled continentalness in [-1, 1] (0 when off): biome selection reuses it
	 * @return Effective terrain params
	 */
	static FWorldModeTerrainParams ComputeEffectiveTerrainParams(
		const FVector& Direction,
		float PlanetRadius,
		const FWorldModeTerrainParams& BaseParams,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot* BiomeSnapshot,
		float& OutContinentalness);

	/**
	 * The planet's terrain height: the surface's radial displacement from PlanetRadius along a direction. Generation,
	 * GetSurfacePoint and the shell check all use it (SP-A1, SP-B1, SP-B5).
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius Datum radius
	 * @param TerrainParams Un-modulated terrain params
	 * @param NoiseParams Terrain noise params
	 * @param BiomeSnapshot Biome data for continentalness, or null
	 * @param OutContinentalness Optional: the sampled continentalness
	 * @return Displacement (uu, positive above the datum)
	 */
	static float SampleRadialDisplacement(
		const FVector& Direction,
		float PlanetRadius,
		const FWorldModeTerrainParams& TerrainParams,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot* BiomeSnapshot,
		float* OutContinentalness = nullptr);

	/**
	 * Worst-case displacement bounds over every direction: noise in [-1, 1] at continentalness's extremes, decoupled
	 * (FVoxelBiomeSnapshot::GetContinentalnessExtremes), as FInfinitePlaneWorldMode::GetTerrainHeightBounds does for the
	 * plane. The terrain shell (MaxTerrainHeight, MaxTerrainDepth) must contain it; UVoxelWorldConfiguration's settings
	 * check computes the same numbers.
	 *
	 * @param TerrainParams Un-modulated terrain params
	 * @param BiomeSnapshot Biome data for continentalness, or null
	 * @param OutMin Lowest displacement
	 * @param OutMax Highest displacement
	 */
	static void GetDisplacementBounds(
		const FWorldModeTerrainParams& TerrainParams,
		const FVoxelBiomeSnapshot* BiomeSnapshot,
		float& OutMin,
		float& OutMax);

	/**
	 * Convert noise value to radial terrain displacement, without continentalness (GetDensityAt's contract: the caller
	 * passes the terrain noise, as in the heightmap modes). SampleRadialDisplacement is the full surface.
	 *
	 * @param NoiseValue Raw noise value [-1, 1]
	 * @param TerrainParams Terrain scaling parameters
	 * @return Radial displacement from PlanetRadius
	 */
	static float NoiseToRadialDisplacement(
		float NoiseValue,
		const FWorldModeTerrainParams& TerrainParams);

	/**
	 * The signed distance generation writes: TerrainRadius - DistFromCenter, held at -1000 more than 1000 uu above the
	 * outer shell radius (air) and at +1000 more than 1000 uu below the inner one (core).
	 *
	 * @param DistFromCenter Distance from the planet centre
	 * @param TerrainRadius PlanetRadius + displacement along the point's direction
	 * @param PlanetParams Planet shell
	 * @return Signed distance (positive = solid)
	 */
	static float ShellSignedDistance(
		float DistFromCenter,
		float TerrainRadius,
		const FSphericalPlanetParams& PlanetParams);

	/**
	 * A position with its altitude above the datum scaled by VerticalScale and its direction kept: Centre + Direction *
	 * (PlanetRadius + Altitude * VerticalScale). Cave noise samples here, so VerticalScale flattens caves along local up
	 * as it flattens them along Z in heightmap modes (SP-B6), and keeps horizontal distances near the datum.
	 *
	 * @param WorldPos World position
	 * @param PlanetParams Planet centre and radius
	 * @param VerticalScale Scale applied to the altitude
	 * @return Cave noise sample position
	 */
	static FVector ScaleAltitude(
		const FVector& WorldPos,
		const FSphericalPlanetParams& PlanetParams,
		float VerticalScale);

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

	/** Continentalness for GetSurfacePoint (SetBiomeContext); plain data, safe on any thread. */
	FVoxelBiomeSnapshot BiomeSnapshot;

	/** Practical chunk range for spherical planet (relative to viewer) */
	static constexpr int32 CHUNK_RANGE = 32;
};
