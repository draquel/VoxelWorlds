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
};
