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
 * How a scatter definition's mesh is rendered.
 */
UENUM(BlueprintType)
enum class EScatterMeshType : uint8
{
	/** Standard: assigned static mesh via HISM */
	StaticMesh,

	/** Runtime cross-billboard (2 intersecting quads) for grass/flowers */
	CrossBillboard,

	/** Trees stamped directly into VoxelData (editable terrain) */
	VoxelInjection
};

/**
 * How scatter positions are determined on the surface.
 */
UENUM(BlueprintType)
enum class EScatterPlacementMode : uint8
{
	/** Default: density-interpolated positions (smooth terrain) */
	SurfaceInterpolated,

	/** Snap to block face center (cubic terrain) */
	BlockFaceSnap
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

/**
 * Type of noise algorithm to use for terrain generation.
 */
UENUM(BlueprintType)
enum class EVoxelNoiseType : uint8
{
	/** Classic Perlin noise - smooth gradient noise */
	Perlin,

	/** Simplex noise - faster and less directional artifacts than Perlin */
	Simplex,

	/** Cellular (Worley) noise - organic cell patterns, F1 distance */
	Cellular,

	/** Voronoi noise - cell edge patterns, F2-F1 distance */
	Voronoi
};

/**
 * Parameters controlling noise-based terrain generation.
 *
 * fBm (Fractal Brownian Motion) combines multiple octaves of noise
 * to create natural-looking terrain with both large features and fine detail.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelNoiseParams
{
	GENERATED_BODY()

	/** Type of noise algorithm */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	EVoxelNoiseType NoiseType = EVoxelNoiseType::Simplex;

	/** Random seed for noise generation (0 = use world seed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	int32 Seed = 0;

	/** Number of noise layers to combine (more = more detail, slower) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "1", ClampMax = "16"))
	int32 Octaves = 6;

	/** Base frequency of noise (lower = larger features) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "0.00001", ClampMax = "1.0"))
	float Frequency = 0.001f;

	/** Base amplitude of noise */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float Amplitude = 1.0f;

	/** Frequency multiplier per octave (typically 2.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float Lacunarity = 2.0f;

	/** Amplitude multiplier per octave (typically 0.5) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Persistence = 0.5f;

	FVoxelNoiseParams() = default;
};
