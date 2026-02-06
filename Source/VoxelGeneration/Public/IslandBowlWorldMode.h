// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelWorldMode.h"

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

	/** Exponential falloff (very gradual then sharp drop) */
	Exponential
};

/**
 * Island-specific configuration parameters.
 */
struct VOXELGENERATION_API FIslandBowlParams
{
	/** Radius of the island in world units (distance from center to edge start) */
	float IslandRadius = 50000.0f;

	/** Width of the falloff zone where terrain fades to nothing */
	float FalloffWidth = 10000.0f;

	/** Type of falloff curve to use */
	EIslandFalloffType FalloffType = EIslandFalloffType::Smooth;

	/** Center of the island in world X coordinate (relative to WorldOrigin) */
	float CenterX = 0.0f;

	/** Center of the island in world Y coordinate (relative to WorldOrigin) */
	float CenterY = 0.0f;

	/** Minimum terrain height at island edges (can be negative for bowl effect) */
	float EdgeHeight = -1000.0f;

	/** Whether to create a bowl (lowered edges) or plateau (raised center) */
	bool bBowlShape = false;

	FIslandBowlParams() = default;

	FIslandBowlParams(float InRadius, float InFalloff, EIslandFalloffType InType = EIslandFalloffType::Smooth)
		: IslandRadius(InRadius)
		, FalloffWidth(InFalloff)
		, FalloffType(InType)
	{
	}

	/** Get the total island extent (radius + falloff) */
	float GetTotalExtent() const
	{
		return IslandRadius + FalloffWidth;
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
 * - IslandRadius to IslandRadius+FalloffWidth: Gradual falloff
 * - Beyond FalloffWidth: Air only (or minimum edge height)
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

	// ==================== Static Helpers ====================

	/**
	 * Calculate the distance from a point to the island center (2D, ignoring Z).
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
	 * Calculate the falloff factor based on distance from center.
	 *
	 * @param Distance Distance from island center
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

private:
	/** Base terrain generation parameters */
	FWorldModeTerrainParams TerrainParams;

	/** Island-specific parameters */
	FIslandBowlParams IslandParams;

	/** Practical vertical limits for chunk generation */
	static constexpr int32 MIN_Z_CHUNKS = -4;
	static constexpr int32 MAX_Z_CHUNKS = 8;
};
