// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"

struct FVoxelBiomeSnapshot;
struct FVoxelNoiseParams;
struct FVoxelSeaSurface;
class IVoxelWorldMode;
class FSphericalPlanetWorldMode;

/**
 * A sampled terrain surface point: from the procedural generator (FVoxelSurfaceQuery, no loaded chunk data required) or
 * from loaded, edit-merged voxels (UVoxelChunkManager::QueryEditMergedSurfacePoint).
 */
struct VOXELGENERATION_API FVoxelSurfaceSample
{
	/**
	 * Heightmap modes: world-space Z of the terrain surface at the queried X,Y. On a planet, which has no Z per (X, Y):
	 * the surface's altitude above the datum (as Altitude).
	 */
	float Height = 0.0f;

	/** Surface normal derived from the height-field gradient (points up-ish). */
	FVector Normal = FVector::UpVector;

	/** Slope angle in degrees (0 = flat, 90 = vertical), measured against Up. */
	float SlopeDegrees = 0.0f;

	/** World position of the surface point (heightmap modes: (X, Y, Height)); spherical SP-A3. */
	FVector Position = FVector::ZeroVector;

	/** Local up at the point: +Z in heightmap modes, the radial direction on a planet. */
	FVector Up = FVector::UpVector;

	/** The point's altitude above the datum (IVoxelWorldMode::GetAltitude). */
	float Altitude = 0.0f;

	/** Surface material ID (biome-blended; 0 when no biome config). */
	uint8 MaterialID = 0;

	/** Dominant biome ID (0 when no biome config). */
	uint8 BiomeID = 0;
};

/**
 * Stateless, thread-safe surface queries against the procedural generator.
 *
 * Answers "what is the terrain surface at world X,Y" (height, normal, slope, biome,
 * material) by re-sampling IVoxelWorldMode + the biome noise pipeline — decoupled
 * from chunk streaming, so it works for any region whether or not a chunk is loaded.
 *
 * This is the shared core reused by:
 * - FVoxelTreeInjector (deterministic tree placement)
 * - VoxelPCG's Voxel Surface Sampler node (PCG point generation)
 *
 * All math here is pure and deterministic for a given (world mode, noise params,
 * seed, biome config). Safe to call off the game thread.
 */
class VOXELGENERATION_API FVoxelSurfaceQuery
{
public:
	/** Terrain surface Z at a world X,Y. Thin wrapper over IVoxelWorldMode::GetTerrainHeightAt. */
	static float GetSurfaceHeight(
		const IVoxelWorldMode& WorldMode,
		float WorldX, float WorldY,
		const FVoxelNoiseParams& NoiseParams);

	/**
	 * Slope angle (degrees) at a world X,Y, from a central-difference height gradient.
	 * VoxelSize is used as the sampling step.
	 */
	static float ComputeSlopeDegrees(
		const IVoxelWorldMode& WorldMode,
		float WorldX, float WorldY, float VoxelSize,
		const FVoxelNoiseParams& NoiseParams);

	/**
	 * Surface normal at a world X,Y, from a central-difference height gradient.
	 * Returns a normalized up-facing vector (FVector(-dHdX, -dHdY, 1) normalized).
	 */
	static FVector ComputeSurfaceNormal(
		const IVoxelWorldMode& WorldMode,
		float WorldX, float WorldY, float VoxelSize,
		const FVoxelNoiseParams& NoiseParams);

	/**
	 * Query surface material and biome at a world position.
	 * Uses the SAME biome pipeline as the CPU noise generator (temperature/moisture/continentalness
	 * noise -> tiered biome blend -> blended surface material -> height rules), via the shared
	 * FVoxelBiomeSnapshot algorithm cores.
	 *
	 * Takes a value snapshot instead of the UVoxelBiomeConfiguration UObject: build it once per
	 * batch with FVoxelBiomeSnapshot::FromConfig (game thread), then call this freely from any
	 * thread — including tasks that may outlive the world.
	 *
	 * @param BiomeSnapshot Value-captured biome data (FVoxelBiomeSnapshot::FromConfig)
	 * @param OutSurfaceMaterial Surface material ID (0 when the snapshot is invalid)
	 * @param OutBiomeID Dominant biome ID (0 when the snapshot is invalid)
	 */
	static void QuerySurfaceConditions(
		float WorldX, float WorldY, float TerrainHeight, float VoxelSize,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		int32 WorldSeed,
		bool bEnableWaterLevel, float WaterLevel,
		uint8& OutSurfaceMaterial, uint8& OutBiomeID);

	/**
	 * Convenience: sample everything (height, normal, slope, material, biome) at a world X,Y.
	 * Computes the height gradient once and derives both slope and normal from it.
	 * BiomeSnapshot as in QuerySurfaceConditions — hoist one per batch via FromConfig.
	 *
	 * @param Zones Optional heightmap conditioning zones there (POI pads and their falloff rings; gather them for the place,
	 *              e.g. UVoxelChunkManager::GatherConditioningZones): the generated surface, as the generator layers them
	 *              onto the natural height (FVoxelTerrainConditioning::ApplyToHeight), for the height, the gradient and the
	 *              material's water and height rules (world modes review G23). Planet zones are ignored. Null or empty:
	 *              the natural surface.
	 */
	static FVoxelSurfaceSample SampleSurface(
		const IVoxelWorldMode& WorldMode,
		float WorldX, float WorldY, float VoxelSize,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		int32 WorldSeed,
		bool bEnableWaterLevel, float WaterLevel,
		const TArray<struct FVoxelConditioningZone>* Zones = nullptr);

	/**
	 * Material and biome on a planet as GenerateChunkSphericalPlanet writes them for a voxel at a position, before caves
	 * and ores (spherical SP-A3): temperature, moisture and continentalness at the direction's point on the datum sphere,
	 * the biome blend, the blended material at the position's depth below the terrain (with the sea's variant when water
	 * is on), height rules by its altitude. Pass a surface point for the surface material (depth 0).
	 *
	 * @param Planet The planet world mode
	 * @param Position A surface point, or any position (its depth below the terrain along its direction is used)
	 * @param VoxelSize Voxel size (generation measures the depth in voxels)
	 * @param NoiseParams Terrain noise (the radial height; its seed derives the climate seeds)
	 * @param BiomeSnapshot Value-captured biome data (FVoxelBiomeSnapshot::FromConfig)
	 * @param Sea The world's sea (FVoxelSeaSurface::FromConfig): a sphere on a planet
	 * @param OutSurfaceMaterial Surface material ID (0 when the snapshot is invalid)
	 * @param OutBiomeID Dominant biome ID (0 when the snapshot is invalid)
	 * @param Zones Optional planet conditioning zones there (spherical SP-B10): the terrain's radius is the conditioned one,
	 *              as generation's, so a pad's surface point reads the surface material at depth 0
	 */
	static void QueryPlanetSurfaceConditions(
		const FSphericalPlanetWorldMode& Planet,
		const FVector& Position,
		float VoxelSize,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		const FVoxelSeaSurface& Sea,
		uint8& OutSurfaceMaterial, uint8& OutBiomeID,
		const TArray<struct FVoxelConditioningZone>* Zones = nullptr);

	/**
	 * Everything at the analytic surface on the local up-line through a position, in any world mode (spherical SP-A3):
	 * heightmap modes return SampleSurface at (X, Y) with Position (X, Y, Height); a planet the surface along the
	 * direction from its centre (GetSurfacePoint), the slope against local up, and QueryPlanetSurfaceConditions.
	 *
	 * @param WorldMode World mode
	 * @param WorldPos Any position on the up-line
	 * @param VoxelSize Voxel size (the heightmap gradient's step)
	 * @param NoiseParams Terrain noise
	 * @param BiomeSnapshot Value-captured biome data
	 * @param Sea The world's sea (FVoxelSeaSurface::FromConfig): the WaterLevel plane, or a planet's sphere
	 * @param Zones Optional conditioning zones there: the generated surface and its conditions. On a planet its zones
	 *              (spherical SP-B10, GetConditionedSurfacePoint); in heightmap modes its zones (SampleSurface's, G23)
	 * @return The surface sample
	 */
	static FVoxelSurfaceSample SampleSurfacePoint(
		const IVoxelWorldMode& WorldMode,
		const FVector& WorldPos, float VoxelSize,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		const FVoxelSeaSurface& Sea,
		const TArray<struct FVoxelConditioningZone>* Zones = nullptr);

	/**
	 * Find the topmost terrain surface in a vertical voxel column (index 0 = lowest Z, ascending).
	 *
	 * This is the near-band, EDIT-AWARE path: the caller assembles a column of edit-merged voxels
	 * (so digging/building is honored) and this finds the exact surface. Scans from the top for the
	 * highest solid voxel with air directly above, interpolates the density crossing at
	 * VOXEL_SURFACE_THRESHOLD, and reads the surface voxel's material/biome. Pure and deterministic.
	 *
	 * @param ColumnLowToHigh Voxels ordered from lowest to highest world Z
	 * @param BaseZ           World Z of ColumnLowToHigh[0]
	 * @param VoxelSize       World units per voxel (column step)
	 * @return false if the column has no air-over-solid transition (caller falls back to the generator)
	 */
	static bool ExtractSurfaceFromColumn(
		const TArray<FVoxelData>& ColumnLowToHigh,
		float BaseZ, float VoxelSize,
		float& OutHeight, uint8& OutMaterialID, uint8& OutBiomeID);
};
