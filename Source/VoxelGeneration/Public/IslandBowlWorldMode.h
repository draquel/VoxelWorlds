// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelWorldMode.h"
#include "VoxelBiomeSnapshot.h"

class UVoxelBiomeConfiguration;

/**
 * Island shape types for boundary calculation.
 */
enum class EIslandShape : uint8
{
	/** Circular island (radial distance from center) */
	Circular,

	/** Rectangular island (axis-aligned box with separate X/Y sizes) */
	Rectangle
};

/**
 * Falloff curve types for island edge transitions.
 */
enum class EIslandFalloffType : uint8
{
	/** Linear falloff (simple but sharp transition) */
	Linear,

	/** Smooth hermite falloff (gradual, natural-looking) */
	Smooth,

	/** Squared falloff (faster transition near edge) */
	Squared,

	/** Exponential falloff (steep near the island, flattening toward the edge; rescaled to reach 0 at the edge) */
	Exponential
};

/**
 * Island-specific configuration parameters. Build them from a world configuration with
 * FVoxelWorldModeFactory::MakeIslandParams, the one builder generation, the LOD cull, the map and the editor share.
 */
struct VOXELGENERATION_API FIslandBowlParams
{
	/** Shape of the island (Circular or Rectangle) */
	EIslandShape Shape = EIslandShape::Circular;

	/** Radius/SizeX of the island in world units (distance from center to edge start) */
	float IslandRadius = 50000.0f;

	/** Size Y of the island (only used for Rectangle shape) */
	float SizeY = 50000.0f;

	/** Width of the falloff zone where terrain fades to nothing */
	float FalloffWidth = 10000.0f;

	/** Type of falloff curve to use */
	EIslandFalloffType FalloffType = EIslandFalloffType::Smooth;

	/**
	 * Centre of the island, world X. Absolute: FVoxelWorldModeFactory::MakeIslandParams resolves the config's
	 * offset (UVoxelWorldConfiguration::IslandCenterX, relative to WorldOrigin) to WorldOrigin.X + offset.
	 */
	float CenterX = 0.0f;

	/** Centre of the island, world Y. Absolute, like CenterX */
	float CenterY = 0.0f;

	/** Terrain height the falloff fades to at the edge (R + FalloffWidth); beyond it the seabed slopes down from here */
	float EdgeHeight = -1000.0f;

	/** How far below EdgeHeight the seabed beyond the edge ends up (GetSeabedHeight; WM-E1) */
	float SeabedDepth = 6000.0f;

	/** Horizontal distance past the edge over which the seabed slopes down by SeabedDepth; 0 is a step at the edge */
	float SeabedSlopeWidth = 20000.0f;

	FIslandBowlParams() = default;

	FIslandBowlParams(float InRadius, float InFalloff, EIslandFalloffType InType = EIslandFalloffType::Smooth)
		: IslandRadius(InRadius)
		, SizeY(InRadius)
		, FalloffWidth(InFalloff)
		, FalloffType(InType)
	{
	}

	/** Get the total island extent in X (radius/sizeX + falloff) */
	float GetTotalExtentX() const
	{
		return IslandRadius + FalloffWidth;
	}

	/** Get the total island extent in Y (sizeY + falloff, or same as X for circular) */
	float GetTotalExtentY() const
	{
		return (Shape == EIslandShape::Rectangle ? SizeY : IslandRadius) + FalloffWidth;
	}

	/** Get the maximum total extent (for circular compatibility) */
	float GetTotalExtent() const
	{
		return FMath::Max(GetTotalExtentX(), GetTotalExtentY());
	}
};

/**
 * Island Bowl World Mode implementation.
 *
 * Generates bounded terrain with falloff at the edges, creating an island
 * or bowl-shaped landmass. Terrain height is modulated by distance from
 * the island center, fading to nothing (or a minimum height) at the edges.
 *
 * Island Shape:
 * - Center to IslandRadius: Full terrain height (no falloff)
 * - IslandRadius to IslandRadius+FalloffWidth: Gradual falloff, reaching EdgeHeight at the edge
 * - Beyond the edge: the seabed slopes from EdgeHeight down by SeabedDepth over SeabedSlopeWidth, then stays flat
 *   (GetSeabedHeight; world modes review WM-E1: it was a flat floor 1,000 uu down, a cliff at the edge)
 *
 * Density Calculation:
 * - Sample base terrain height (same as InfinitePlane)
 * - Calculate falloff factor based on distance from center
 * - Final height = BaseHeight * FalloffFactor + EdgeHeight * (1 - FalloffFactor)
 * - Density = FinalHeight - WorldPos.Z
 *
 * @see IVoxelWorldMode
 * @see FInfinitePlaneWorldMode
 */
class VOXELGENERATION_API FIslandBowlWorldMode : public IVoxelWorldMode
{
public:
	/**
	 * Construct with default parameters.
	 */
	FIslandBowlWorldMode();

	/**
	 * Construct with specified parameters.
	 *
	 * @param InTerrainParams Base terrain generation parameters
	 * @param InIslandParams Island-specific parameters
	 */
	FIslandBowlWorldMode(
		const FWorldModeTerrainParams& InTerrainParams,
		const FIslandBowlParams& InIslandParams);

	virtual ~FIslandBowlWorldMode() = default;

	// ==================== IVoxelWorldMode Interface ====================

	virtual float GetDensityAt(
		const FVector& WorldPos,
		int32 LODLevel,
		float NoiseValue) const override;

	/** The natural surface at (X, Y): (X, Y, GetTerrainHeightAt), Normal = +Z, the altitude above SeaLevel. */
	virtual FVoxelTerrainSurfacePoint GetSurfaceOnUpLine(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams) const override;

	/**
	 * This mode's terrain height at (X, Y): the natural surface's Z, the seabed beyond the island's edge (the heightmap
	 * modes' own height since code review CR2, CORE-25; code that works in every mode uses GetSurfaceOnUpLine).
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param NoiseParams Noise parameters for terrain generation
	 * @return Z coordinate of the terrain surface at (X, Y)
	 */
	float GetTerrainHeightAt(
		float X,
		float Y,
		const FVoxelNoiseParams& NoiseParams) const;

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

	virtual EWorldMode GetWorldModeType() const override { return EWorldMode::IslandBowl; }
	virtual bool IsHeightmapBased() const override { return true; }

	virtual uint8 GetMaterialAtDepth(
		const FVector& WorldPos,
		float SurfaceHeight,
		float DepthBelowSurface) const override;

	// ==================== Configuration ====================

	const FWorldModeTerrainParams& GetTerrainParams() const { return TerrainParams; }
	void SetTerrainParams(const FWorldModeTerrainParams& InParams) { TerrainParams = InParams; }

	const FIslandBowlParams& GetIslandParams() const { return IslandParams; }
	void SetIslandParams(const FIslandBowlParams& InParams) { IslandParams = InParams; }

	/**
	 * Provide the biome configuration for continentalness height modulation (IVoxelWorldMode). IslandBowl
	 * applies continentalness (internal oceans/mountains) to the base height, THEN the island edge falloff,
	 * so a large island gets internal variation while still fading at the world edges.
	 * Snapshotted BY VALUE at this call (game thread) — no UObject reference is retained, so the mode is
	 * safe to share into background tasks that may outlive the world.
	 */
	virtual void SetBiomeContext(const UVoxelBiomeConfiguration* InBiomeConfig) override
	{
		BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(InBiomeConfig);
	}

	/**
	 * This mode's vertical-cull bounds (GetWorldBounds' MinZ / MaxZ): the base continentalness extent, with the minimum
	 * lowered to the seabed's floor beyond the edge (EdgeHeight - SeabedDepth) when that is deeper.
	 */
	void GetTerrainHeightBounds(float& OutMin, float& OutMax) const;

	// ==================== World Bounds and Local Frame (IVoxelWorldMode, WM-A2) ====================
	// Up is +Z and the surface coordinate is (0, X, Y) (the defaults); the datum is SeaLevel.

	/** Inside the island's R + falloff footprint (IsWithinIslandBounds): beyond it generation writes only the seabed. */
	virtual bool IsInsideWorld(double X, double Y) const override;

	/** A circle (R + F) or a rectangle (R + F by SizeY + F) around the island centre, with the terrain's height bounds. */
	virtual FVoxelWorldBounds GetWorldBounds() const override;

	/** Z above SeaLevel. */
	virtual float GetAltitude(const FVector& WorldPos) const override;

	/** (U, V, SeaLevel + Altitude). */
	virtual FVector SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const override;

	// ==================== Static Helpers ====================

	/**
	 * Calculate the distance from a point to the island center (2D, ignoring Z).
	 * For circular islands, returns Euclidean distance.
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param CenterX Island center X
	 * @param CenterY Island center Y
	 * @return Distance from center
	 */
	static float CalculateDistanceFromCenter(
		float X, float Y,
		float CenterX, float CenterY);

	/**
	 * Calculate the normalized distance from a point to the island boundary.
	 * Works for both circular and rectangular islands.
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param IslandParams Island configuration parameters
	 * @return Normalized distance: 0 = at center, 1 = at edge, >1 = in falloff zone
	 */
	static float CalculateNormalizedDistance(
		float X, float Y,
		const FIslandBowlParams& IslandParams);

	/**
	 * Calculate the falloff factor based on distance from center.
	 *
	 * @param Distance Distance from island center (or normalized distance for rectangle)
	 * @param IslandRadius Inner radius where falloff starts
	 * @param FalloffWidth Width of the falloff zone
	 * @param FalloffType Type of falloff curve
	 * @return Falloff factor in range [0, 1] (1 = full terrain, 0 = no terrain)
	 */
	static float CalculateFalloffFactor(
		float Distance,
		float IslandRadius,
		float FalloffWidth,
		EIslandFalloffType FalloffType);

	/**
	 * Calculate the falloff factor using island params (handles both shapes).
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param IslandParams Island configuration
	 * @return Falloff factor in range [0, 1] (1 = full terrain, 0 = no terrain)
	 */
	static float CalculateFalloffFactorForPoint(
		float X, float Y,
		const FIslandBowlParams& IslandParams);

	/**
	 * The falloff curve on the normalized position in the falloff zone (shared by both shapes, and mirrored by
	 * ApplyIslandFalloffCurve in Shaders/Private/IslandFalloff.ush).
	 *
	 * @param T Position in the falloff zone: 0 at IslandRadius, 1 at the edge (clamped)
	 * @param FalloffType Type of falloff curve
	 * @return Falloff factor: 1 at T = 0, exactly 0 at T = 1 for every curve
	 */
	static float ApplyFalloffCurve(float T, EIslandFalloffType FalloffType);

	/**
	 * The island terrain height at (X, Y) for a base (continentalness-modulated) terrain height: the falloff
	 * toward EdgeHeight inside the island's extent, the seabed (GetSeabedHeight) beyond it, which needs no noise.
	 * The one composition generation (CPU and GPU), GetTerrainHeightAt and GetDensityAt share.
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param BaseHeight Terrain height before the island falloff
	 * @param IslandParams Island configuration
	 * @return Island terrain height
	 */
	static float ComposeTerrainHeight(
		float X, float Y,
		float BaseHeight,
		const FIslandBowlParams& IslandParams);

	/**
	 * Apply falloff to a terrain height value.
	 *
	 * @param BaseHeight Original terrain height from noise
	 * @param FalloffFactor Falloff factor [0, 1]
	 * @param EdgeHeight Height at island edges (when falloff = 0)
	 * @return Modified terrain height
	 */
	static float ApplyFalloffToHeight(
		float BaseHeight,
		float FalloffFactor,
		float EdgeHeight);

	/**
	 * Check if a position is within the island's total extent.
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param IslandParams Island configuration
	 * @return True if within island bounds (including falloff zone)
	 */
	static bool IsWithinIslandBounds(
		float X, float Y,
		const FIslandBowlParams& IslandParams);

	/**
	 * How far (X, Y) lies past the island's edge, measured the way the falloff measures it: radially for a circle,
	 * the larger axis excess for a rectangle (so the seabed's contours are circles or rectangles around the island).
	 *
	 * @param X World X coordinate
	 * @param Y World Y coordinate
	 * @param IslandParams Island configuration
	 * @return Distance past the edge; zero or negative inside the island's extent
	 */
	static float GetDistancePastEdge(
		float X, float Y,
		const FIslandBowlParams& IslandParams);

	/**
	 * The seabed beyond the island's edge (world modes review WM-E1): EdgeHeight at the edge, sloping down by
	 * SeabedDepth over SeabedSlopeWidth with a smoothstep (flat where it meets the falloff, flat where it reaches the
	 * open sea's floor), flat beyond. Mirrored by IslandSeabedHeight in Shaders/Private/IslandFalloff.ush; the far
	 * ocean's seabed (FVoxelFarOcean) follows it.
	 *
	 * @param DistancePastEdge Distance past the edge (GetDistancePastEdge); clamped at 0
	 * @param IslandParams Island configuration
	 * @return Seabed height (world Z)
	 */
	static float GetSeabedHeight(
		float DistancePastEdge,
		const FIslandBowlParams& IslandParams);

private:
	/** Base terrain generation parameters */
	FWorldModeTerrainParams TerrainParams;

	/** Island-specific parameters */
	FIslandBowlParams IslandParams;

	/**
	 * Value-captured biome data for continentalness height modulation (GetTerrainHeightAt), snapshotted
	 * from the config in SetBiomeContext. Plain data, no UObject reference — safe on any thread and for
	 * tasks that outlive the world. Default (never set) => raw base terrain height (no continentalness).
	 */
	FVoxelBiomeSnapshot BiomeSnapshot;

	/** Practical vertical limits for chunk generation */
	static constexpr int32 MIN_Z_CHUNKS = -4;
	static constexpr int32 MAX_Z_CHUNKS = 8;
};
