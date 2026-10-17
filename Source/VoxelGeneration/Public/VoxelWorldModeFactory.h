// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"

class UVoxelWorldConfiguration;
struct FVoxelNoiseGenerationRequest;

/**
 * The one place world-mode parameters are built from a world configuration (world modes review WM-A1, KI3/KI4;
 * spherical requirement SP-A6).
 *
 * Generation (CPU and GPU, through the request), the LOD strategy's island and shell culls, the chunk manager's
 * analytic world mode, the map and the editor field viewer all take their params from here, so they cannot drift
 * apart again. Before this, island params were copied field by field in six places and the island centre meant two
 * things: generation read IslandCenterX/Y as an absolute position, the LOD cull as an offset from WorldOrigin.
 *
 * Positions in the built params are absolute world space: the island centre is WorldOrigin + IslandCenterX/Y (the
 * config's documented meaning), the planet centre is WorldOrigin.
 *
 * Thread safety: the functions read the configuration, so call them on the game thread (or wherever the config is
 * not being edited). The world modes they return are immutable afterwards and safe to share with worker threads.
 */
struct VOXELGENERATION_API FVoxelWorldModeFactory
{
	/**
	 * Version of the generation code, recorded in every world save's fingerprint (WM-A4). Bump it when the same
	 * configuration starts generating different terrain (noise, height composition, island falloff, caves, ...): saves
	 * made before the bump are then refused, instead of loading their edits and records onto terrain that moved.
	 * 1: the first recorded version (WM-A1 moved islands whose WorldOrigin is not zero onto WorldOrigin + offset).
	 * 2: G18, the GPU heightmap caves apply the cave configuration's biome overrides and underwater minimum depth as the
	 *    CPU does (GPU InfinitePlane and IslandBowl worlds stop carving caves under the sea and in biomes scaled to 0).
	 */
	static constexpr int32 GenerationVersion = 2;

	/**
	 * Version of the planet's generation, in a planet's fingerprint shape ("PlanetGeneration=N") so a change to planet
	 * terrain refuses planet saves only; GenerationVersion would refuse every save. 2: spherical Phase 1 (noise by 3D
	 * direction on the datum sphere, continentalness on the radial height, materials by altitude; SP-B1, SP-B2, SP-B5).
	 * Fingerprints from before have no entry and are refused.
	 */
	static constexpr int32 PlanetGenerationVersion = 2;

	/**
	 * Terrain params for the heightmap modes (InfinitePlane, IslandBowl).
	 *
	 * @param Config World configuration
	 * @return SeaLevel, HeightScale and BaseHeight from the config
	 */
	static FWorldModeTerrainParams MakeTerrainParams(const UVoxelWorldConfiguration& Config);

	/**
	 * Terrain params for a spherical planet: radial, so SeaLevel is 0 and the height scale is PlanetHeightScale.
	 *
	 * @param Config World configuration
	 * @return The planet's terrain params
	 */
	static FWorldModeTerrainParams MakePlanetTerrainParams(const UVoxelWorldConfiguration& Config);

	/**
	 * Island params, with the centre resolved to WorldOrigin + IslandCenterX/Y.
	 *
	 * @param Config World configuration (read whatever its WorldMode)
	 * @return The island's shape, extent, falloff, absolute centre and edge height
	 */
	static FIslandBowlParams MakeIslandParams(const UVoxelWorldConfiguration& Config);

	/**
	 * Spherical planet params; the planet is centred on WorldOrigin.
	 *
	 * @param Config World configuration (read whatever its WorldMode)
	 * @return The planet's radius, shell and centre
	 */
	static FSphericalPlanetParams MakePlanetParams(const UVoxelWorldConfiguration& Config);

	/**
	 * The analytic world mode for a configuration, with its biome context set (continentalness), so its height
	 * queries match generation. Each caller gets its own instance: a caller whose background tasks may outlive the
	 * chunk manager (map tiles, POI placement) must not borrow the manager's.
	 *
	 * @param Config World configuration
	 * @return A new world mode of the config's WorldMode (InfinitePlane for an unknown mode)
	 */
	static TSharedRef<IVoxelWorldMode> Create(const UVoxelWorldConfiguration& Config);

	/**
	 * Fill a generation request's world-mode fields: WorldMode, WorldOrigin, the terrain params (the planet's for a
	 * planet) and the island or planet params. Leaves every other field alone.
	 *
	 * @param Config World configuration
	 * @param Request Generation request to fill
	 */
	static void FillGenerationRequest(const UVoxelWorldConfiguration& Config, FVoxelNoiseGenerationRequest& Request);

	/**
	 * The world's fingerprint for saves (WM-A4, KI7; SP-G9): the world mode, the mode's shape params (island: shape,
	 * radius, size Y for a rectangle, falloff width and curve, centre, edge height; planet: radius, shell, centre), the
	 * voxel grid (voxel size, chunk size, world origin: edits are stored per chunk) and GenerationVersion. Terrain
	 * tuning (noise, height scale, biomes) is left out on purpose: the save key already holds the seed, and refusing a
	 * save on every tuning change would get in the way more than it protects.
	 *
	 * @param Config World configuration
	 * @return The fingerprint; equal configs give equal fingerprints
	 */
	static FVoxelWorldFingerprint MakeFingerprint(const UVoxelWorldConfiguration& Config);
};
