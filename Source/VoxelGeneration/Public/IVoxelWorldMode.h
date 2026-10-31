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
 * - The natural surface on the local up-line through a position (GetSurfaceOnUpLine, one sample; GetSurfacePoint with
 *   its normal): the surface ladder's first rung (GLOSSARY.md), in every mode
 * - Coordinate transformations
 * - Vertical bounds for chunk streaming
 * - Where the world ends (IsInsideWorld, GetWorldBounds), a local frame (GetUpVector, GetAltitude,
 *   WorldToSurface, SurfaceToWorld) and the surface along local up (GetSurfacePoint), so consumers work in any mode
 *   without assuming +Z is up or that the world has no edge (world modes review WM-A2; spherical requirements SP-A1,
 *   SP-A2)
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

	// ==================== The natural surface (the ladder's first rung) ====================

	/**
	 * The natural surface on the local up-line through a position, one sample: the analytic surface generation produces,
	 * without caves, conditioning zones or edits (code review CR2, CORE-25: the one primitive every mode implements; the
	 * heightmap modes' GetTerrainHeightAt is their own height and no longer the interface's). Heightmap modes return
	 * (X, Y, the terrain height at (X, Y)) with its altitude (GetAltitude); a planet the point along the direction from
	 * its centre at the terrain's altitude there, exactly as it samples it. The normal is not computed: it is local up
	 * (GetSurfacePoint samples the normal, at five times the cost). Thread-safe.
	 *
	 * @param WorldPos Any position on the up-line (only X, Y in heightmap modes; only the direction on a planet)
	 * @param NoiseParams Terrain noise parameters
	 * @return The surface point, its altitude, and Normal = local up
	 */
	virtual FVoxelTerrainSurfacePoint GetSurfaceOnUpLine(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams) const = 0;

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
	 * Provide the biome configuration used for continentalness height modulation in the analytic surface
	 * query (GetSurfaceOnUpLine). Heightmap modes that apply continentalness (InfinitePlane, IslandBowl)
	 * use it so the analytic surface matches generation; other modes ignore it. Non-owning — the caller
	 * keeps the configuration alive. Default: no-op.
	 */
	virtual void SetBiomeContext(const UVoxelBiomeConfiguration* /*InBiomeConfig*/) {}

	/**
	 * Worst-case terrain bounds as altitudes above the datum (GetAltitude), over the whole world (spherical SP-A4; code
	 * review CR2, CORE-26: the interface's one bounds method). Heightmap modes: their world-Z height bounds
	 * (GetWorldBounds().MinZ / MaxZ, the authority for vertical chunk-culling) through GetAltitude (Z - SeaLevel); a
	 * planet: its terrain shell, -MaxTerrainDepth to +MaxTerrainHeight. A conservative superset of every surface altitude
	 * generation can produce, so culling to it never removes a chunk holding real terrain. Consumers that shade or bound
	 * by height (the map) use it so they need no planet special case; the world-Z form is GetWorldBounds().
	 *
	 * @param OutMin Lowest altitude
	 * @param OutMax Highest altitude
	 */
	virtual void GetAltitudeBounds(float& OutMin, float& OutMax) const
	{
		const FVoxelWorldBounds Bounds = GetWorldBounds();
		OutMin = GetAltitude(FVector(0.0, 0.0, Bounds.MinZ));
		OutMax = GetAltitude(FVector(0.0, 0.0, Bounds.MaxZ));
	}

	/**
	 * The terrain surface's vertical extent over one patch of the world (a chunk column's footprint): the lowest and
	 * highest surface height there, in world Z. The streaming window (VoxelLOD) keeps every column's chunks around this
	 * range at every distance, so a column streams its own ground whatever the viewer's altitude (world modes review
	 * D8, WM-S1). A planet's window is the column's crossing of the terrain shell instead (spherical SP-C1; a band
	 * around the surface is SP-C5), so planets keep the default.
	 *
	 * Default: heightmap modes sample GetSurfaceOnUpLine at the footprint's four corners and its centre (variation
	 * between the samples is the caller's slack); other modes return the world's Z bounds, the whole range. Cost: five
	 * surface samples, so callers memoize per column.
	 *
	 * @param MinXY Footprint minimum corner (world X, Y)
	 * @param MaxXY Footprint maximum corner (world X, Y)
	 * @param NoiseParams Terrain noise parameters, as for GetSurfaceOnUpLine
	 * @return [lowest, highest] surface height over the footprint, world Z
	 */
	virtual FFloatInterval GetSurfaceRange(const FVector2D& MinXY, const FVector2D& MaxXY, const FVoxelNoiseParams& NoiseParams) const
	{
		if (!IsHeightmapBased())
		{
			const FVoxelWorldBounds Bounds = GetWorldBounds();
			return FFloatInterval(Bounds.MinZ, Bounds.MaxZ);
		}
		const FVector2D Samples[5] = {
			MinXY, FVector2D(MaxXY.X, MinXY.Y), FVector2D(MinXY.X, MaxXY.Y), MaxXY, (MinXY + MaxXY) * 0.5 };
		FFloatInterval Range(TNumericLimits<float>::Max(), TNumericLimits<float>::Lowest());
		for (const FVector2D& Sample : Samples)
		{
			const float Height = static_cast<float>(GetSurfaceOnUpLine(FVector(Sample.X, Sample.Y, 0.0), NoiseParams).Position.Z);
			Range.Min = FMath::Min(Range.Min, Height);
			Range.Max = FMath::Max(Range.Max, Height);
		}
		return Range;
	}

	/** Step (uu) GetSurfacePoint takes either side of a point for the surface normal's finite differences. */
	static constexpr float SurfaceNormalStep = 50.0f;

	/**
	 * The terrain surface on the local up-line through a position, with its outward normal (spherical SP-A1): in
	 * heightmap modes the point at (X, Y, the terrain height there); on a planet the point along the direction from the
	 * centre, so a direction D maps to GetSurfacePoint(Centre + D). The analytic surface generation produces, without
	 * caves, conditioning zones or edits: GetSurfaceOnUpLine with the normal from central differences SurfaceNormalStep
	 * either side.
	 *
	 * Cost: five surface samples. Thread-safe like GetSurfaceOnUpLine.
	 *
	 * @param WorldPos Any position on the up-line (only X, Y in heightmap modes; only the direction on a planet)
	 * @param NoiseParams Terrain noise parameters, as for GetSurfaceOnUpLine
	 * @return The surface point, its normal and its altitude
	 */
	virtual FVoxelTerrainSurfacePoint GetSurfacePoint(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams) const
	{
		// The offsets are formed in float, as the heightmap height is sampled.
		const float X = static_cast<float>(WorldPos.X);
		const float Y = static_cast<float>(WorldPos.Y);
		auto HeightAt = [this, &NoiseParams](float AtX, float AtY)
		{
			return static_cast<float>(GetSurfaceOnUpLine(FVector(AtX, AtY, 0.0), NoiseParams).Position.Z);
		};
		const float DX = HeightAt(X + SurfaceNormalStep, Y) - HeightAt(X - SurfaceNormalStep, Y);
		const float DY = HeightAt(X, Y + SurfaceNormalStep) - HeightAt(X, Y - SurfaceNormalStep);
		FVoxelTerrainSurfacePoint Point = GetSurfaceOnUpLine(WorldPos, NoiseParams);
		Point.Normal = FVector(-DX, -DY, 2.0f * SurfaceNormalStep).GetSafeNormal();
		return Point;
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
	 * planet. IsInsideWorld agrees with FVoxelWorldBounds::ContainsXY. MinZ / MaxZ are the terrain's world-Z extent, a
	 * conservative superset of every surface height generation can produce (the heightmap modes' height bounds: the
	 * authority for vertical chunk-culling; a planet's shell).
	 *
	 * @return The world's bounds, with the terrain's vertical extent
	 */
	virtual FVoxelWorldBounds GetWorldBounds() const
	{
		FVoxelWorldBounds Bounds;
		Bounds.MinZ = -1.0e9f; // no vertical culling for a mode that does not say
		Bounds.MaxZ = 1.0e9f;
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
