// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelWorldModeTypes.h"

class UVoxelBiomeConfiguration;

/**
 * Abstract interface for world generation modes.
 *
 * World modes define how terrain is generated based on the world type:
 * - InfinitePlane: 2D heightmap-style terrain extending infinitely in X/Y
 * - SphericalPlanet: Radial terrain around a center point
 * - IslandBowl: Bounded terrain with edge falloff
 *
 * Each mode provides:
 * - Density calculation (SDF) for terrain generation
 * - Terrain height sampling for heightmap-based modes
 * - Coordinate transformations
 * - Vertical bounds for chunk streaming
 * - Where the world ends (IsInsideWorld, GetWorldBounds) and a local frame (GetUpVector, GetAltitude,
 *   WorldToSurface, SurfaceToWorld), so consumers work in any mode without assuming +Z is up or that the
 *   world has no edge (world modes review WM-A2; spherical requirements SP-A1, SP-A2)
 *
 * Build a mode from a world configuration with FVoxelWorldModeFactory::Create. Game code reaches the configured
 * mode's bounds and frame through UVoxelChunkManager (a component of the world actor), which exposes them to
 * Blueprint.
 *
 * @see FInfinitePlaneWorldMode
 */
class VOXELGENERATION_API IVoxelWorldMode
{
public:
	virtual ~IVoxelWorldMode() = default;

	// ==================== Core SDF Functions ====================

	/**
	 * Get the density value at a world position.
	 *
	 * Uses signed distance field (SDF) where:
	 * - Positive = below surface (solid)
	 * - Zero = at surface
	 * - Negative = above surface (air)
	 *
	 * The returned value is the raw SDF distance, which should be
	 * converted to voxel density [0-255] by the generator.
	 *
	 * @param WorldPos Position in world space
	 * @param LODLevel Current LOD level (for detail reduction)
	 * @param NoiseValue Pre-sampled noise value at this position
	 * @return Signed distance to surface (positive = inside/solid)
	 */
	virtual float GetDensityAt(
		const FVector& WorldPos,
		int32 LODLevel,
		float NoiseValue) const = 0;

	/**
	 * Get terrain height at an X,Y position.
	 *
	 * For heightmap-based modes (InfinitePlane), this samples 2D noise
	 * and returns the terrain surface Z coordinate.
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param NoiseParams Noise parameters for terrain generation
	 * @return Z coordinate of terrain surface at this X,Y
	 */
	virtual float GetTerrainHeightAt(
		float X,
		float Y,
		const FVoxelNoiseParams& NoiseParams) const = 0;

	// ==================== Coordinate Transforms ====================

	/**
	 * Convert a world position to chunk coordinates.
	 *
	 * @param WorldPos Position in world space
	 * @param ChunkSize Number of voxels per chunk edge
	 * @param VoxelSize Size of each voxel in world units
	 * @return Chunk coordinate containing this world position
	 */
	virtual FIntVector WorldToChunkCoord(
		const FVector& WorldPos,
		int32 ChunkSize,
		float VoxelSize) const = 0;

	/**
	 * Convert chunk coordinates to world position (chunk origin).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param ChunkSize Number of voxels per chunk edge
	 * @param VoxelSize Size of each voxel in world units
	 * @param LODLevel LOD level (affects effective chunk size)
	 * @return World position of chunk origin
	 */
	virtual FVector ChunkCoordToWorld(
		const FIntVector& ChunkCoord,
		int32 ChunkSize,
		float VoxelSize,
		int32 LODLevel) const = 0;

	// ==================== Vertical Bounds ====================

	/**
	 * Get the minimum Z level for chunk generation.
	 *
	 * For bounded modes, this limits how deep chunks are generated.
	 * For infinite modes, returns a practical limit.
	 *
	 * @return Minimum chunk Z coordinate
	 */
	virtual int32 GetMinZ() const = 0;

	/**
	 * Get the maximum Z level for chunk generation.
	 *
	 * For bounded modes, this limits chunk generation height.
	 * For infinite modes, returns a practical limit.
	 *
	 * @return Maximum chunk Z coordinate
	 */
	virtual int32 GetMaxZ() const = 0;

	// ==================== Configuration ====================

	/**
	 * Get the world mode type.
	 *
	 * @return The EWorldMode enum value for this mode
	 */
	virtual EWorldMode GetWorldModeType() const = 0;

	/**
	 * Check if this mode uses heightmap-based (2D) generation.
	 *
	 * Heightmap modes sample noise in 2D and use Z as height,
	 * while volumetric modes sample full 3D noise.
	 *
	 * @return True if this is a heightmap-based mode
	 */
	virtual bool IsHeightmapBased() const = 0;

	// ==================== Material Assignment ====================

	/**
	 * Get the material ID based on position and depth below surface.
	 *
	 * Used to assign materials like stone (deep), dirt (middle), grass (surface).
	 *
	 * @param WorldPos Position in world space
	 * @param SurfaceHeight Height of terrain surface at this X,Y
	 * @param DepthBelowSurface Distance below the terrain surface (positive = below)
	 * @return Material ID to assign to this voxel
	 */
	virtual uint8 GetMaterialAtDepth(
		const FVector& WorldPos,
		float SurfaceHeight,
		float DepthBelowSurface) const = 0;

	// ==================== Analytic Height Context (optional) ====================

	/**
	 * Provide the biome configuration used for continentalness height modulation in the analytic height
	 * query (GetTerrainHeightAt). Heightmap modes that apply continentalness (InfinitePlane, IslandBowl)
	 * use it so the analytic surface matches generation; other modes ignore it. Non-owning — the caller
	 * keeps the configuration alive. Default: no-op.
	 */
	virtual void SetBiomeContext(const UVoxelBiomeConfiguration* /*InBiomeConfig*/) {}

	/**
	 * Worst-case terrain-height bounds [OutMin, OutMax] over all (X,Y) for this mode — the authority for
	 * vertical chunk-culling (VoxelLOD). A conservative superset that CONTAINS every value the generator can
	 * produce, so culling to it never removes a chunk holding real terrain. Default: a very wide range (no
	 * vertical culling). Heightmap modes override with tight bounds.
	 */
	virtual void GetTerrainHeightBounds(float& OutMin, float& OutMax) const
	{
		OutMin = -1.0e9f;
		OutMax = 1.0e9f;
	}

	/**
	 * The terrain surface's vertical extent over one patch of the world (a chunk column's footprint): the lowest and
	 * highest surface height there, in world Z. The streaming window (VoxelLOD) keeps every column's chunks around this
	 * range at every distance, so a column streams its own ground whatever the viewer's altitude (world modes review
	 * D8, WM-S1). On a planet the same window is an altitude band around the surface (spherical SP-C1, Phase 1).
	 *
	 * Default: heightmap modes sample GetTerrainHeightAt at the footprint's four corners and its centre (variation
	 * between the samples is the caller's slack); other modes return GetTerrainHeightBounds, the whole range. Cost: five
	 * height samples, so callers memoize per column.
	 *
	 * @param MinXY Footprint minimum corner (world X, Y)
	 * @param MaxXY Footprint maximum corner (world X, Y)
	 * @param NoiseParams Terrain noise parameters, as for GetTerrainHeightAt
	 * @return [lowest, highest] surface height over the footprint, world Z
	 */
	virtual FFloatInterval GetSurfaceRange(const FVector2D& MinXY, const FVector2D& MaxXY, const FVoxelNoiseParams& NoiseParams) const
	{
		if (!IsHeightmapBased())
		{
			float Min = 0.0f, Max = 0.0f;
			GetTerrainHeightBounds(Min, Max);
			return FFloatInterval(Min, Max);
		}
		const FVector2D Samples[5] = {
			MinXY, FVector2D(MaxXY.X, MinXY.Y), FVector2D(MinXY.X, MaxXY.Y), MaxXY, (MinXY + MaxXY) * 0.5 };
		FFloatInterval Range(TNumericLimits<float>::Max(), TNumericLimits<float>::Lowest());
		for (const FVector2D& Sample : Samples)
		{
			const float Height = GetTerrainHeightAt(static_cast<float>(Sample.X), static_cast<float>(Sample.Y), NoiseParams);
			Range.Min = FMath::Min(Range.Min, Height);
			Range.Max = FMath::Max(Range.Max, Height);
		}
		return Range;
	}

	// ==================== World Bounds (WM-A2) ====================
	// The defaults describe an unbounded heightmap world with its datum at Z 0, so a mode that does not override them
	// (a game plugin's own mode, a test mock) keeps working. The built-in modes override them.

	/**
	 * Whether the vertical line at (X, Y) passes through the world: the test for consumers that place, explore,
	 * spawn or navigate. An island's footprint is R + falloff (beyond it generation writes only a flat floor); a plane
	 * has no edge. On a planet it says whether the line meets the terrain shell; 2D consumers on a planet key on
	 * WorldToSurface instead. Cheap: safe to call per sample.
	 *
	 * @param X World X
	 * @param Y World Y
	 * @return True when (X, Y) is inside the world's footprint
	 */
	virtual bool IsInsideWorld(double /*X*/, double /*Y*/) const { return true; }

	/**
	 * Where the world ends: unbounded for a plane, the R + falloff footprint for an island, the terrain shell for a
	 * planet. IsInsideWorld agrees with FVoxelWorldBounds::ContainsXY.
	 *
	 * @return The world's bounds, with the terrain's vertical extent
	 */
	virtual FVoxelWorldBounds GetWorldBounds() const
	{
		FVoxelWorldBounds Bounds;
		GetTerrainHeightBounds(Bounds.MinZ, Bounds.MaxZ);
		return Bounds;
	}

	// ==================== Local Frame (SP-A1, SP-A2) ====================

	/**
	 * Local up at a world position: +Z in heightmap modes, the radial direction on a planet.
	 *
	 * @param WorldPos World position
	 * @return Unit up vector
	 */
	virtual FVector GetUpVector(const FVector& /*WorldPos*/) const { return FVector::UpVector; }

	/**
	 * Signed height above the datum, the surface the terrain's noise displaces from: SeaLevel in heightmap modes (so
	 * Z - SeaLevel), the planet radius on a planet. The water plane (WaterLevel / WaterRadius) is separate.
	 *
	 * @param WorldPos World position
	 * @return Height above the datum along GetUpVector (negative below it)
	 */
	virtual float GetAltitude(const FVector& WorldPos) const { return static_cast<float>(WorldPos.Z); }

	/**
	 * A world position's coordinate on the surface parameterisation, the key 2D consumers use: (0, X, Y) in heightmap
	 * modes, a cube face and (U, V) in [-1, 1] on a planet. Ignores altitude: every point on one up-line maps to the
	 * same coordinate.
	 *
	 * @param WorldPos World position
	 * @return Surface coordinate
	 */
	virtual FVoxelSurfaceCoord WorldToSurface(const FVector& WorldPos) const
	{
		return FVoxelSurfaceCoord(0, FVector2D(WorldPos.X, WorldPos.Y));
	}

	/**
	 * Inverse of WorldToSurface: the world position at a surface coordinate and an altitude above the datum.
	 *
	 * @param Coord Surface coordinate
	 * @param Altitude Height above the datum (see GetAltitude)
	 * @return World position
	 */
	virtual FVector SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const
	{
		return FVector(Coord.UV.X, Coord.UV.Y, Altitude);
	}
};

/**
 * Terrain generation parameters for world modes.
 *
 * These settings control how noise is converted to terrain height
 * and how the SDF is calculated.
 */
struct VOXELGENERATION_API FWorldModeTerrainParams
{
	/** Base height of terrain (sea level) */
	float SeaLevel = 0.0f;

	/** Scale factor for noise-to-height conversion */
	float HeightScale = 5000.0f;

	/** Minimum terrain height offset from sea level */
	float BaseHeight = 0.0f;

	// NOTE: no Min/MaxHeight clamp here — terrain height is intentionally unclamped so the CPU stays
	// bit-identical to the GPU generator (which never clamped). A height cap would have to be applied
	// identically on BOTH the CPU and the GPU shader; see FInfinitePlaneWorldMode::NoiseToTerrainHeight.

	FWorldModeTerrainParams() = default;

	FWorldModeTerrainParams(float InSeaLevel, float InHeightScale, float InBaseHeight)
		: SeaLevel(InSeaLevel)
		, HeightScale(InHeightScale)
		, BaseHeight(InBaseHeight)
	{
	}
};
