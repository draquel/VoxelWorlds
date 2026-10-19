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
	/**
	 * Radius of the planet surface (the datum) in world units. Double (spherical Phase 6, WM-BP4): every quantity of size R
	 * is kept in double from the centre to the final altitude or signed distance, so a 1,000 km planet (R 1e8, where a
	 * float's spacing is 8 uu) has no cancellation at magnitude R. Altitudes, chunk and voxel sizes stay float.
	 */
	double PlanetRadius = 100000.0;

	/** Maximum terrain height above PlanetRadius */
	float MaxTerrainHeight = 5000.0f;

	/** Maximum terrain depth below PlanetRadius (caves, valleys) */
	float MaxTerrainDepth = 2000.0f;

	/** Center of the planet in world space */
	FVector PlanetCenter = FVector::ZeroVector;

	/**
	 * How far above PlanetRadius the world reaches (spherical Phase 6): the outer out-of-world bound and KillZ's place
	 * (FVoxelWorldBounds::OrbitRadius = PlanetRadius + OrbitCeiling). 0 = 3 x PlanetRadius. Not part of the terrain.
	 */
	double OrbitCeiling = 0.0;

	FSphericalPlanetParams() = default;

	FSphericalPlanetParams(double InRadius, const FVector& InCenter = FVector::ZeroVector)
		: PlanetRadius(InRadius)
		, PlanetCenter(InCenter)
	{
	}

	/** Get the inner shell radius (planet surface minus max depth) */
	double GetInnerRadius() const
	{
		return PlanetRadius - MaxTerrainDepth;
	}

	/** Get the outer shell radius (planet surface plus max height) */
	double GetOuterRadius() const
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

	/**
	 * Legacy heightmap query (spherical SP-A3): a planet has no height per (X, Y), so this returns the world Z where the
	 * vertical line through (X, Y) meets the terrain on the upper hemisphere (the planet centre's Z when the line misses
	 * the terrain) and logs a warning the first time it is called. Planet code uses GetSurfacePoint instead; the
	 * fallback only keeps a stray legacy caller on the surface rather than at a radius read as a Z.
	 */
	virtual float GetTerrainHeightAt(
		float X,
		float Y,
		const FVoxelNoiseParams& NoiseParams) const override;

	/** The terrain shell's world-Z extent: PlanetCenter.Z -/+ the outer radius (what GetWorldBounds reports). */
	virtual void GetTerrainHeightBounds(float& OutMin, float& OutMax) const override;

	/** The terrain shell as altitudes: -MaxTerrainDepth to +MaxTerrainHeight (spherical SP-A4). */
	virtual void GetAltitudeBounds(float& OutMin, float& OutMax) const override;

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

	/**
	 * The generated surface (spherical SP-B10): GetSurfacePoint with planet conditioning zones applied to the altitude
	 * along every direction it samples (FVoxelTerrainConditioning::ApplyToAltitude, as GenerateChunkSphericalPlanet does),
	 * so the point and its normal follow a flattened pad and its ring. No zones: GetSurfacePoint exactly.
	 *
	 * @param WorldPos Any position on the up-line
	 * @param NoiseParams Terrain noise
	 * @param Zones Conditioning zones (planet zones apply; gather them for the place, e.g. UVoxelChunkManager::GatherConditioningZonesOnSphere)
	 * @return The surface point, normal and altitude
	 */
	FVoxelTerrainSurfacePoint GetConditionedSurfacePoint(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams,
		const TArray<struct FVoxelConditioningZone>& Zones) const;

	/**
	 * GetTerrainAltitude with planet conditioning zones applied (spherical SP-B10).
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param NoiseParams Terrain noise
	 * @param Zones Conditioning zones (planet zones apply)
	 * @return Altitude (uu)
	 */
	float GetConditionedTerrainAltitude(const FVector& Direction, const FVoxelNoiseParams& NoiseParams,
		const TArray<struct FVoxelConditioningZone>& Zones) const;

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

	/** The biome snapshot SetBiomeContext took (continentalness for the surface); invalid without a biome config. */
	const FVoxelBiomeSnapshot& GetBiomeSnapshot() const { return BiomeSnapshot; }

	/**
	 * The terrain's altitude above the datum along a direction: SampleRadialDisplacement with this planet's radius,
	 * terrain params and biome snapshot, what GetSurfacePoint uses without its normal (spherical Phase 4, the map's
	 * pixels).
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param NoiseParams Terrain noise
	 * @return Altitude (uu)
	 */
	float GetTerrainAltitude(const FVector& Direction, const FVoxelNoiseParams& NoiseParams) const
	{
		return SampleRadialDisplacement(Direction, PlanetParams.PlanetRadius, TerrainParams, NoiseParams, &BiomeSnapshot);
	}

	// ==================== Static Helpers ====================

	/**
	 * Calculate the distance from a point to the planet center, in double (WM-BP4: a float of size R is 8 uu coarse at
	 * R 1e8). Subtract PlanetRadius in double before narrowing anything to float.
	 *
	 * @param WorldPos World position
	 * @param PlanetCenter Center of the planet
	 * @return Distance from planet center
	 */
	static double CalculateRadialDistance(
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
	static FVector GetNoiseSamplePoint(const FVector& Direction, double PlanetRadius)
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
		double PlanetRadius,
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
		double PlanetRadius,
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
		double PlanetRadius,
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
	 * outer shell radius (air) and at +1000 more than 1000 uu below the inner one (core). Both radii in double; the
	 * difference is formed in double (ShellSignedDistanceAtAltitude).
	 *
	 * @param DistFromCenter Distance from the planet centre
	 * @param TerrainRadius PlanetRadius + displacement along the point's direction
	 * @param PlanetParams Planet shell
	 * @return Signed distance (positive = solid)
	 */
	static float ShellSignedDistance(
		double DistFromCenter,
		double TerrainRadius,
		const FSphericalPlanetParams& PlanetParams);

	/**
	 * ShellSignedDistance written with altitudes (spherical Phase 6, WM-BP4): TerrainAltitude - VoxelAltitude, held at
	 * -1000 more than 1000 uu above MaxTerrainHeight and at +1000 more than 1000 uu below -MaxTerrainDepth. Both are of
	 * the size of the relief, not of R, so nothing cancels; the generator computes VoxelAltitude = |P - C| - R in double,
	 * the GPU from the chunk origin's altitude and a small difference (SphericalPlanet.ush).
	 *
	 * @param VoxelAltitude The point's distance from the centre minus PlanetRadius
	 * @param TerrainAltitude The terrain's altitude along the point's direction
	 * @param PlanetParams Planet shell
	 * @return Signed distance (positive = solid)
	 */
	static float ShellSignedDistanceAtAltitude(
		double VoxelAltitude,
		double TerrainAltitude,
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
	 * ScaleAltitude relative to the planet centre: PlanetRelativePos with its altitude scaled by VerticalScale, in double.
	 * Planet caves sample the planet noise here (WM-BP5), so they move with the planet and stay exact at any radius.
	 *
	 * @param PlanetRelativePos Position relative to the planet centre
	 * @param PlanetRadius Datum radius
	 * @param VerticalScale Scale applied to the altitude
	 * @return Cave noise sample position, relative to the centre
	 */
	static FVector ScaleAltitudeRelative(
		const FVector& PlanetRelativePos,
		double PlanetRadius,
		float VerticalScale);

	/**
	 * The planet's continentalness field: the 2-octave Simplex field at the biome configuration's frequency and seed
	 * offset (the heightmap modes' parameters), sampled by the planet noise (WM-BP5).
	 *
	 * @param WorldSeed The terrain noise seed
	 * @param BiomeSnapshot Biome data with the continentalness settings
	 * @return Noise parameters
	 */
	static FVoxelNoiseParams GetContinentalnessNoiseParams(int32 WorldSeed, const FVoxelBiomeSnapshot& BiomeSnapshot);

	/**
	 * Temperature and moisture along a direction: each field at the direction's point on the datum sphere (SP-B1) by the
	 * planet noise (WM-BP5), as generation, the surface queries and the map sample them.
	 *
	 * @param Direction Unit direction from the planet centre
	 * @param PlanetRadius Datum radius
	 * @param TemperatureParams Temperature field (FVoxelPlanetNoise::MakeFieldParams)
	 * @param MoistureParams Moisture field
	 * @param OutTemperature Temperature in about [-1, 1]
	 * @param OutMoisture Moisture in about [-1, 1]
	 */
	static void SampleClimate(
		const FVector& Direction,
		double PlanetRadius,
		const FVoxelNoiseParams& TemperatureParams,
		const FVoxelNoiseParams& MoistureParams,
		float& OutTemperature,
		float& OutMoisture);

	/**
	 * Calculate signed distance from a point to the terrain surface.
	 *
	 * @param DistFromCenter Distance from planet center
	 * @param TerrainRadius Radius of terrain at this direction (PlanetRadius + displacement)
	 * @return Signed distance (positive = inside solid, negative = air)
	 */
	static float CalculateSignedDistance(
		double DistFromCenter,
		double TerrainRadius);

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
