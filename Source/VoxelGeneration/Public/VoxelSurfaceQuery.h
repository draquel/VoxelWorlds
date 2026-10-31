// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"
#include "VoxelWorldModeTypes.h"

struct FVoxelBiomeSnapshot;
struct FVoxelNoiseParams;
struct FVoxelSeaSurface;
class IVoxelWorldMode;
class FSphericalPlanetWorldMode;
class UVoxelCaveConfiguration;

/**
 * The surface ladder's rungs (GLOSSARY.md), the layers a "where is the ground here" query answers from:
 * UVoxelChunkManager::QuerySurfacePoint takes one.
 */
enum class EVoxelSurfaceLayer : uint8
{
	/** The analytic surface without conditioning zones, caves or edits (IVoxelWorldMode::GetSurfaceOnUpLine). */
	Natural,
	/** The natural surface with the conditioning zones applied, and the block top on a cubic world: what generation makes. */
	Generated,
	/** The surface of the loaded voxels with edits (false where no loaded chunk holds it). */
	EditMerged
};

/**
 * A sampled terrain surface point with its conditions: from the procedural generator (FVoxelSurfaceQuery, no loaded chunk
 * data required) or from loaded, edit-merged voxels (UVoxelChunkManager::QueryEditMergedSurfacePoint). The geometry
 * (Position, Normal, Altitude) is FVoxelTerrainSurfacePoint's (code review CR2, CORE-24: one geometry struct); the
 * heightmap height is Position.Z, the planet's altitude is Altitude.
 */
struct VOXELGENERATION_API FVoxelSurfaceSample : public FVoxelTerrainSurfacePoint
{
	/** Slope angle in degrees (0 = flat, 90 = vertical), measured against Up. */
	float SlopeDegrees = 0.0f;

	/** Local up at the point: +Z in heightmap modes, the radial direction on a planet. */
	FVector Up = FVector::UpVector;

	/** Surface material ID (biome-blended; 0 when no biome config). */
	uint8 MaterialID = 0;

	/** Dominant biome ID (0 when no biome config). */
	uint8 BiomeID = 0;
};

/**
 * A surface point's material mix (G45, FVoxelSurfaceQuery::QueryPlanetSurfaceMaterialMixAt): its two leading surface
 * materials and the second's share of them, what the per-voxel material picks there cover on average.
 */
struct FVoxelSurfaceMaterialMix
{
	/** The leading material, then the second (the leading one again when there is no second) */
	uint8 Materials[2] = { 0, 0 };

	/** The second material's share of the two, in [0, 0.5] */
	float Weight = 0.0f;

	/** The dominant biome */
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
	// ==================== The ladder's rungs in every mode (code review CR2, CORE-27) ====================
	// One entry per question, taking the world mode and a world position: the generated surface (GetGeneratedSurface, one
	// sample), the conditions at a surface point (QuerySurfaceConditionsAt, QuerySurfaceMaterialMixAt) and everything at
	// once (SampleSurfacePoint). The Z-up and planet forms below them are their implementations, kept for the callers
	// whose arithmetic they are until those move (the far field, the map).

	/**
	 * The generated surface on the local up-line through a position, one sample (the surface ladder's generated rung,
	 * GLOSSARY.md): the natural surface (IVoxelWorldMode::GetSurfaceOnUpLine) with the conditioning zones there layered
	 * on as generation does (FVoxelTerrainConditioning::ApplyToHeight on a heightmap, ApplyToAltitude on a planet). The
	 * normal is local up; the block top of a cubic world is the chunk manager's (SnapToBlockTop), not applied here.
	 *
	 * @param WorldMode World mode
	 * @param WorldPos Any position on the up-line
	 * @param NoiseParams Terrain noise
	 * @param Zones The conditioning zones there (gathered for the place), or null / empty for the natural surface
	 * @param OutContinentalness Optional: on a planet the continentalness its height sampled (what its conditions reuse);
	 *        0 on a heightmap world (its conditions sample their own)
	 * @return The surface point, with Normal = local up
	 */
	static FVoxelTerrainSurfacePoint GetGeneratedSurface(
		const IVoxelWorldMode& WorldMode,
		const FVector& WorldPos,
		const FVoxelNoiseParams& NoiseParams,
		const TArray<struct FVoxelConditioningZone>* Zones,
		float* OutContinentalness = nullptr);

	/**
	 * The surface material and biome at a surface point, in any world mode: the same biome pipeline as generation
	 * (temperature / moisture / continentalness, the tiered blend, the blended material at depth 0 with the sea's variant
	 * under it, height rules). Heightmap modes: QuerySurfaceConditions at the point's (X, Y, Z); a planet:
	 * QueryPlanetSurfaceConditionsAt along the point's direction at its altitude (depth 0).
	 *
	 * @param WorldMode World mode
	 * @param Surface The surface point (GetGeneratedSurface or GetSurfaceOnUpLine)
	 * @param Continentalness On a planet, what GetGeneratedSurface returned for it (the blend reuses it); ignored elsewhere
	 * @param VoxelSize Voxel size (the dither's key)
	 * @param NoiseParams Terrain noise (its seed derives the climate seeds)
	 * @param BiomeSnapshot Value-captured biome data (FVoxelBiomeSnapshot::FromConfig)
	 * @param Sea The world's sea (FVoxelSeaSurface::FromConfig)
	 * @param OutSurfaceMaterial Surface material ID (0 when the snapshot is invalid)
	 * @param OutBiomeID Dominant biome ID (0 when the snapshot is invalid)
	 */
	static void QuerySurfaceConditionsAt(
		const IVoxelWorldMode& WorldMode,
		const FVoxelTerrainSurfacePoint& Surface,
		float Continentalness,
		float VoxelSize,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		const FVoxelSeaSurface& Sea,
		uint8& OutSurfaceMaterial, uint8& OutBiomeID);

	/**
	 * The surface material mix at a surface point, in any world mode (G45 / WM-MP1: what the per-voxel picks cover there
	 * on average, for the far field). Heightmap modes: QuerySurfaceMaterialMix at the point's (X, Y, Z); a planet:
	 * QueryPlanetSurfaceMaterialMixAt along its direction.
	 *
	 * @param WorldMode World mode
	 * @param Surface The surface point
	 * @param Continentalness On a planet, what GetGeneratedSurface returned for it; ignored elsewhere
	 * @param NoiseParams Terrain noise
	 * @param BiomeSnapshot Value-captured biome data
	 * @param Sea The world's sea
	 * @return The mix (material 0 and weight 0 when the snapshot is invalid)
	 */
	static FVoxelSurfaceMaterialMix QuerySurfaceMaterialMixAt(
		const IVoxelWorldMode& WorldMode,
		const FVoxelTerrainSurfacePoint& Surface,
		float Continentalness,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		const FVoxelSeaSurface& Sea);

	/**
	 * The voxel generation makes at a world position, by density alone (material 0, no flags): the signed distance to the
	 * generated surface on the position's up-line (GetGeneratedSurface: a heightmap's height, a planet's shell), less the
	 * cave carve where it is solid under the ground (FVoxelCPUNoiseGenerator::CalculateCaveDensity at its depth, for the
	 * biome generation gives it, under the sea or not). The one single-voxel entry for a reader that needs a voxel its own
	 * chunk data cannot show (scatter's voxels past a chunk's faces; code review CR2, SCAT-16).
	 *
	 * @param WorldMode World mode
	 * @param WorldPos The voxel's sample position, as generation samples it
	 * @param VoxelSize Voxel size (depths are in voxels)
	 * @param NoiseParams Terrain noise
	 * @param Zones The conditioning zones there, or null (used only when Surface is null)
	 * @param Caves The world's cave configuration when its caves are on, else null
	 * @param Sea The world's sea (FVoxelSeaSurface::FromConfig)
	 * @param bEnableBiomes Whether the world's biomes are on (the planet's biome for the cave overrides)
	 * @param BiomeHint The biome generation gives the position (a heightmap column's, which its chunk's voxels carry), or
	 *        -1: on a planet derived from its climate when a cave override needs it, behind
	 *        UVoxelCaveConfiguration::AnyLayerReachesDepth; 0 on a heightmap
	 * @param Surface The generated surface on the position's up-line when the caller has it (its height sampled where
	 *        the caller sampled it), else sampled here with Zones
	 * @param Continentalness What GetGeneratedSurface returned with Surface (a planet's biome reuses it)
	 * @return The voxel's density (the material 0)
	 */
	static FVoxelData GenerateVoxelAt(
		const IVoxelWorldMode& WorldMode,
		const FVector& WorldPos,
		float VoxelSize,
		const FVoxelNoiseParams& NoiseParams,
		const TArray<struct FVoxelConditioningZone>* Zones,
		const UVoxelCaveConfiguration* Caves,
		const FVoxelSeaSurface& Sea,
		bool bEnableBiomes,
		int32 BiomeHint = -1,
		const FVoxelTerrainSurfacePoint* Surface = nullptr,
		float Continentalness = 0.0f);

	// ==================== Heightmap (Z-up) forms ====================

	/** Terrain surface Z at a world X,Y: the natural surface (IVoxelWorldMode::GetSurfaceOnUpLine) in a heightmap mode. */
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
	 * QueryPlanetSurfaceConditions from a terrain altitude the caller has already sampled (spherical Phase 6, WM-BP9: the
	 * far field samples the altitude of every patch vertex once and reads the material from it). The same arithmetic:
	 * QueryPlanetSurfaceConditions calls this after its own SampleRadialDisplacement.
	 *
	 * @param Planet The planet world mode
	 * @param Direction Unit direction from the planet centre
	 * @param TerrainAltitude The terrain's altitude along Direction (conditioned where zones apply)
	 * @param Continentalness The continentalness SampleRadialDisplacement returned for Direction (the biome blend reuses it)
	 * @param VoxelAltitude The queried point's altitude (TerrainAltitude for the surface material: depth 0)
	 * @param VoxelSize Voxel size (generation measures the depth in voxels)
	 * @param NoiseParams Terrain noise (its seed derives the climate seeds)
	 * @param BiomeSnapshot Value-captured biome data
	 * @param Sea The world's sea
	 * @param OutSurfaceMaterial Surface material ID (0 when the snapshot is invalid)
	 * @param OutBiomeID Dominant biome ID (0 when the snapshot is invalid)
	 */
	static void QueryPlanetSurfaceConditionsAt(
		const FSphericalPlanetWorldMode& Planet,
		const FVector& Direction,
		float TerrainAltitude,
		float Continentalness,
		double VoxelAltitude,
		float VoxelSize,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		const FVoxelSeaSurface& Sea,
		uint8& OutSurfaceMaterial, uint8& OutBiomeID);

	/**
	 * The surface material mix at a planet's surface point (G45): what QueryPlanetSurfaceConditionsAt's per-voxel picks cover
	 * there on average, for the far field, whose vertices lie km apart while a blend zone's picks change every few voxels.
	 * The blend's materials and shares at depth 0 (FVoxelBiomeSnapshot::GetBlendedMaterialShares, the sea's variant below it),
	 * height rules applied to each, summed per material; the two largest, with the second's share of the two.
	 *
	 * @param Planet The planet world mode
	 * @param Direction Unit direction from the planet centre
	 * @param TerrainAltitude The terrain's altitude along Direction (conditioned where zones apply)
	 * @param Continentalness The continentalness SampleRadialDisplacement returned for Direction
	 * @param NoiseParams Terrain noise (its seed derives the climate seeds)
	 * @param BiomeSnapshot Value-captured biome data
	 * @param Sea The world's sea
	 * @return The mix (material 0 and weight 0 when the snapshot is invalid)
	 */
	static FVoxelSurfaceMaterialMix QueryPlanetSurfaceMaterialMixAt(
		const FSphericalPlanetWorldMode& Planet,
		const FVector& Direction,
		float TerrainAltitude,
		float Continentalness,
		const FVoxelNoiseParams& NoiseParams,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		const FVoxelSeaSurface& Sea);

	/**
	 * The surface material mix at a heightmap world's surface point (world modes review WM-MP1, the heightmap far field):
	 * what QuerySurfaceConditions's per-voxel picks cover there on average. The climate and blend as QuerySurfaceConditions
	 * samples them, the blend's materials and shares at depth 0 (the sea's variant under WaterLevel), height rules applied
	 * to each, summed per material; the two largest, with the second's share of the two.
	 *
	 * @param WorldX World X
	 * @param WorldY World Y
	 * @param TerrainHeight The generated surface height there (conditioning zones included)
	 * @param BiomeSnapshot Value-captured biome data
	 * @param WorldSeed The terrain noise's seed (the climate seeds derive from it)
	 * @param bEnableWaterLevel Whether the world has a sea
	 * @param WaterLevel The sea's level (world Z)
	 * @return The mix (material 0 and weight 0 when the snapshot is invalid)
	 */
	static FVoxelSurfaceMaterialMix QuerySurfaceMaterialMix(
		float WorldX, float WorldY, float TerrainHeight,
		const FVoxelBiomeSnapshot& BiomeSnapshot,
		int32 WorldSeed,
		bool bEnableWaterLevel, float WaterLevel);

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
