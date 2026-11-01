// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCubeSphere.h"
#include "VoxelSeaSurface.h"
#include "VoxelWorldModeTypes.h"
#include "VoxelCoreTypes.h"
#include "VoxelTerrainConditioning.h"

class IVoxelWorldMode;
class UVoxelChunkManager;

/**
 * A map tile pixel's sample on the surface domain (code review CR2, REND-15): on a heightmap world the world (X, Y) of the
 * pixel's corner (in float, as the drawer samples the height), on a planet the unit direction through the pixel's centre.
 * Direction is local up in both (+Z on a heightmap world).
 */
struct FVoxelMapTileSample
{
	float X = 0.0f;
	float Y = 0.0f;
	FVector Direction = FVector::UpVector;
};

/**
 * The map's tile frame (code review CR2, REND-15): everything about the map's tiles that depends on the kind of world,
 * a planet's cube-sphere atlas (spherical Phase 4, WM-SY8: a tile a cell about a chunk across, its pixels along its
 * face's U and V) or a heightmap world's chunk-column grid (a tile a chunk column, its pixels along world X and Y),
 * behind one set of questions: the tile holding a position, a tile's origin and size, its neighbours, the tiles a chunk
 * spans or a radius covers; and the drawer's samples, altitudes, positions, water, shading and the pixel of a
 * neighbouring tile an apron grid point falls in. Each method keeps its kind's expression, so the tiles drawn are the
 * ones the two drawers made. Built by the map subsystem when it resolves its world; a value type the tile tasks copy.
 */
struct VOXELMAP_API FVoxelMapTileFrame
{
	/** A planet (the atlas of Grid); else a heightmap world */
	bool bPlanet = false;

	/** The planet's tile grid (invalid on a heightmap world), centre and datum radius */
	FVoxelCubeSphereGrid Grid;
	FVector Center = FVector::ZeroVector;
	double Radius = 0.0;

	/** The chunk grid: a tile is ChunkSize pixels across */
	int32 ChunkSize = 32;
	float VoxelSize = 100.0f;
	FVector WorldOrigin = FVector::ZeroVector;

	/** Heightmap worlds: the world's bounds (exploration stops at an island's edge, WM-I2) */
	FVoxelWorldBounds Bounds;

	/** The world's sea (FVoxelSeaSurface::FromConfig): the heightmap's WaterLevel plane, the planet's water sphere */
	FVoxelSeaSurface Sea;

	/** The terrain's extent for the elevation and depth shading: a heightmap's height bounds, a planet's altitude bounds */
	float MinAltitude = 0.0f;
	float MaxAltitude = 0.0f;

	/** Whether the frame draws water, and the water's altitude (the WaterLevel; the sea radius less the datum radius) */
	bool HasWater() const;
	float GetWaterAltitude() const;

	// ---- The grid ----

	/** A tile's edge: a chunk's on a heightmap world; along a face's centre lines on the datum sphere on a planet. */
	double GetTileWorldSize() const;

	/** The tile holding a world position (its chunk column; the cell its direction from the centre falls in). */
	FIntPoint TileOf(const FVector& WorldPos) const;

	/** A tile's origin: its minimum corner on a heightmap world (Z 0); its (U, V) = (0, 0) corner on the datum sphere on a planet. */
	FVector TileOrigin(FIntPoint Tile) const;

	/** The tile a step away: Tile + (DX, DY) on a heightmap world; the grid's neighbour (across a face edge too) on a planet. */
	FIntPoint Neighbour(FIntPoint Tile, int32 DX, int32 DY) const;

	/** Whether a tile is one of the world's: inside the bounds on a heightmap world (always on a plane), a cell of the atlas on a planet. */
	bool IsValidTile(FIntPoint Tile) const;

	/** Whether the drawer can draw a tile: any on a heightmap world (a tile asked for is drawn), a cell of the atlas on a planet. */
	bool CanDrawTile(FIntPoint Tile) const { return !bPlanet || Grid.IsValidCell(Tile); }

	/** The tiles a chunk belongs to: its (X, Y) on a heightmap world; on a planet the cells its corners and centre span. */
	void TilesOfChunk(const FIntVector& ChunkCoord, TArray<FIntPoint>& OutTiles) const;

	/**
	 * The tiles within a radius of a world position: on a heightmap world the square of tiles the radius reaches, inside
	 * the world; on a planet the tiles whose centres lie within the radius (plus a tile) along the surface.
	 */
	void TilesWithinRadius(const FVector& WorldPos, double InRadius, TArray<FIntPoint>& OutTiles) const;

	/**
	 * Planets: the tile and pixel a direction from the centre falls in.
	 * @return False on a heightmap world
	 */
	bool DirectionToTilePixel(const FVector& Direction, FIntPoint& OutTile, int32& OutPX, int32& OutPY) const;

	/**
	 * Whether an edited chunk can reach a tile's surface, given the tile's lowest analytic altitude: not when it lies
	 * wholly more than a chunk below it (dungeon interiors under a loaded entrance need not be loaded).
	 */
	bool ChunkMayReachSurface(const FIntVector& ChunkCoord, float LowestAnalyticAltitude) const;

	// ---- The drawer ----

	/**
	 * The sample at a tile's grid point (GX, GY) of the (ChunkSize + 2)^2 grid with its one-pixel apron: pixel (GX - 1,
	 * GY - 1). On a heightmap world the pixel's corner (X, Y); on a planet the direction through the pixel's centre.
	 */
	FVoxelMapTileSample SampleAt(FIntPoint Tile, int32 GX, int32 GY) const;

	/**
	 * The generated surface's altitude at a sample, conditioning zones applied: the generated height (its world Z) on a
	 * heightmap world, the conditioned terrain altitude along the direction on a planet.
	 */
	float Altitude(const IVoxelWorldMode& WorldMode, const FVoxelMapTileSample& Sample, const FVoxelNoiseParams& NoiseParams,
		const TArray<FVoxelConditioningZone>& Zones) const;

	/** The world point of a sample at an altitude: (X, Y, altitude); centre + direction x (radius + altitude). */
	FVector PositionAt(const FVoxelMapTileSample& Sample, float InAltitude) const;

	/** Whether the surface at an altitude is under water (the WaterLevel; the sea radius). */
	bool IsWater(float InAltitude) const;

	/** The shading ranges: land from the water (or the lowest terrain) to the highest, water depth from the water down to the lowest. */
	void GetShadingSpans(float& OutLandBase, float& OutLandSpan, float& OutDepthSpan) const;

	/**
	 * The hillshade at a grid point: the surface normal from its four neighbours' altitudes (central differences on a
	 * heightmap world; the neighbours' ground points on a planet) lit by the cartographic NW key light (in the local
	 * frame on a planet), remapped so flat ground keeps its colour.
	 *
	 * @param G The grid index GY x GridSize + GX of an interior point
	 * @param GridSize The grid's edge (ChunkSize + 2)
	 * @param Altitudes The grid's altitudes
	 * @param Samples The grid's samples
	 * @return The brightness factor
	 */
	float ShadeAt(int32 G, int32 GridSize, TConstArrayView<float> Altitudes, TConstArrayView<FVoxelMapTileSample> Samples) const;

	/**
	 * The tile and pixel an apron grid point of a tile belongs to (a neighbouring tile's border pixel; the tile's own
	 * pixel for an interior point): on a heightmap world the tile a step over and the pixel wrapped, on a planet the cell
	 * the apron sample's direction falls in (across a face edge the neighbour is turned).
	 */
	void ApronPixel(FIntPoint Tile, int32 GX, int32 GY, const FVoxelMapTileSample& Sample, FIntPoint& OutTile, int32& OutPX, int32& OutPY) const;

	/**
	 * The conditioning zones over a tile and its apron (POI pads, campsites), gathered on the game thread: generation
	 * layers them onto the base height, so the map must too (QA W-05).
	 */
	void GatherZones(const UVoxelChunkManager& ChunkManager, FIntPoint Tile, TArray<FVoxelConditioningZone>& OutZones) const;
};
