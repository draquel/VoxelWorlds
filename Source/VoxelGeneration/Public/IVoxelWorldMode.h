// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"

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

	/** Maximum terrain height (for clamping) */
	float MaxHeight = 10000.0f;

	/** Minimum terrain height (for clamping) */
	float MinHeight = -10000.0f;

	FWorldModeTerrainParams() = default;

	FWorldModeTerrainParams(float InSeaLevel, float InHeightScale, float InBaseHeight)
		: SeaLevel(InSeaLevel)
		, HeightScale(InHeightScale)
		, BaseHeight(InBaseHeight)
	{
	}
};
