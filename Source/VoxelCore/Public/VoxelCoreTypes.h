// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.generated.h"

/**
 * World generation mode determining terrain shape and coordinate system.
 */
UENUM(BlueprintType)
enum class EWorldMode : uint8
{
	/** Infinite flat plane extending in X/Y, height in Z */
	InfinitePlane,

	/** Spherical planet with radial coordinates */
	SphericalPlanet,

	/** Island with bowl-shaped falloff at edges */
	IslandBowl
};

/**
 * Meshing algorithm for converting voxels to renderable geometry.
 */
UENUM(BlueprintType)
enum class EMeshingMode : uint8
{
	/** Block-style cubic voxels with face culling */
	Cubic,

	/** Smooth terrain using Marching Cubes or similar */
	Smooth
};

/**
 * Edit operation mode for terrain modifications.
 */
UENUM(BlueprintType)
enum class EEditMode : uint8
{
	/** Set voxel to specific value */
	Set,

	/** Add density (build terrain) */
	Add,

	/** Subtract density (dig terrain) */
	Subtract,

	/** Change material without affecting density */
	Paint,

	/** Smooth terrain by averaging nearby densities */
	Smooth
};

/**
 * Face direction for cubic voxels.
 */
UENUM(BlueprintType)
enum class EVoxelFace : uint8
{
	Top,      // +Z
	Bottom,   // -Z
	North,    // +Y
	South,    // -Y
	East,     // +X
	West      // -X
};

/**
 * Chunk state in the streaming lifecycle.
 */
UENUM(BlueprintType)
enum class EChunkState : uint8
{
	/** Chunk is not loaded */
	Unloaded,

	/** Chunk is queued for generation */
	PendingGeneration,

	/** Voxel data is being generated */
	Generating,

	/** Chunk is queued for meshing */
	PendingMeshing,

	/** Mesh is being generated */
	Meshing,

	/** Chunk is fully loaded and visible */
	Loaded,

	/** Chunk is queued for unloading */
	PendingUnload
};

/** Voxel density threshold - values below are air, at or above are solid */
constexpr uint8 VOXEL_SURFACE_THRESHOLD = 127;

/** Maximum supported LOD levels */
constexpr int32 VOXEL_MAX_LOD_LEVELS = 8;

/** Default chunk size (voxels per edge) */
constexpr int32 VOXEL_DEFAULT_CHUNK_SIZE = 32;

/** Maximum material types supported */
constexpr int32 VOXEL_MAX_MATERIALS = 256;

/** Maximum biome types supported */
constexpr int32 VOXEL_MAX_BIOMES = 256;
