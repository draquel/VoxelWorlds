// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"

struct FVoxelNoiseParams;
class IVoxelWorldMode;
class UVoxelBiomeConfiguration;

/**
 * A sampled terrain surface point, derived purely from the procedural generator
 * (no loaded chunk data required).
 */
struct VOXELGENERATION_API FVoxelSurfaceSample
{
	/** World-space Z of the terrain surface at the queried X,Y. */
	float Height = 0.0f;

	/** Surface normal derived from the height-field gradient (points up-ish). */
	FVector Normal = FVector::UpVector;

	/** Slope angle in degrees (0 = flat, 90 = vertical). */
	float SlopeDegrees = 0.0f;

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
	 * Replicates the CPU noise generator's biome pipeline
	 * (temperature/moisture/continentalness noise -> biome selection -> surface material).
	 *
	 * @param OutSurfaceMaterial Surface material ID (0 when BiomeConfig is null/invalid)
	 * @param OutBiomeID Dominant biome ID (0 when BiomeConfig is null/invalid)
	 */
	static void QuerySurfaceConditions(
		float WorldX, float WorldY, float TerrainHeight, float VoxelSize,
		const UVoxelBiomeConfiguration* BiomeConfig,
		int32 WorldSeed,
		bool bEnableWaterLevel, float WaterLevel,
		uint8& OutSurfaceMaterial, uint8& OutBiomeID);

	/**
	 * Convenience: sample everything (height, normal, slope, material, biome) at a world X,Y.
	 * Computes the height gradient once and derives both slope and normal from it.
	 */
	static FVoxelSurfaceSample SampleSurface(
		const IVoxelWorldMode& WorldMode,
		float WorldX, float WorldY, float VoxelSize,
		const FVoxelNoiseParams& NoiseParams,
		const UVoxelBiomeConfiguration* BiomeConfig,
		int32 WorldSeed,
		bool bEnableWaterLevel, float WaterLevel);

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
