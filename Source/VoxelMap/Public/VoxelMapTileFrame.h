// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCubeSphere.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldModeTypes.h"
#include "VoxelCoreTypes.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelMapTypes.h"

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

	/** The world's water levels (UVoxelChunkManager::GetWaterLevels): each tile draws its water at the range over its own bounds */
	FVoxelWaterLevels Water;

	/** The terrain's extent for the elevation and depth shading: a heightmap's height bounds, a planet's altitude bounds */
	float MinAltitude = 0.0f;
	float MaxAltitude = 0.0f;

	/**
	 * A tile's world bounds, from its grid's corner samples at the terrain's lowest and highest altitude: what a tile
	 * asks the water levels' range over (GetTileWater).
	 */
	FBox GetTileBounds(const FVoxelMapTileId& Id) const;

	/** The water levels' range over a tile: the one level the tile's water is drawn and tested at. */
	FVoxelWaterLevelRange GetTileWater(const FVoxelMapTileId& Id) const { return Water.GetRange(GetTileBounds(Id)); }

	/** Whether a tile with that range draws water, and the water's altitude (the level; on a planet the level less the datum radius) */
	bool HasWater(const FVoxelWaterLevelRange& TileWater) const;
	float GetWaterAltitude(const FVoxelWaterLevelRange& TileWater) const;

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
	 * The generated surface's altitude at a sample, conditioning applied: the generated height (its world Z) on a
	 * heightmap world, the conditioned terrain altitude along the direction on a planet.
	 */
	float Altitude(const IVoxelWorldMode& WorldMode, const FVoxelMapTileSample& Sample, const FVoxelNoiseParams& NoiseParams,
		const FVoxelConditioning& Conditioning) const;

	/** The world point of a sample at an altitude: (X, Y, altitude); centre + direction x (radius + altitude). */
	FVector PositionAt(const FVoxelMapTileSample& Sample, float InAltitude) const;

	/** Whether the surface at an altitude is under a tile's water (its range's level). */
	bool IsWater(const FVoxelWaterLevelRange& TileWater, float InAltitude) const;

	/** A tile's shading ranges: land from its water (or the lowest terrain) to the highest, water depth from the water down to the lowest. */
	void GetShadingSpans(const FVoxelWaterLevelRange& TileWater, float& OutLandBase, float& OutLandSpan, float& OutDepthSpan) const;

	/**
	 * The hillshade at a grid point: the surface normal from its four neighbours' altitudes (central differences on a
	 * heightmap world; the neighbours' ground points on a planet) lit by the cartographic NW key light (in the local
	 * frame on a planet), remapped so flat ground keeps its colour.
	 *
	 * @param G The grid index GY x GridSize + GX of an interior point
	 * @param GridSize The grid's edge (ChunkSize + 2)
	 * @param Altitudes The grid's altitudes
	 * @param Samples The grid's samples
	 * @param Level The tile's pyramid level (a heightmap's grid points are 2^Level voxels apart)
	 * @return The brightness factor
	 */
	float ShadeAt(int32 G, int32 GridSize, TConstArrayView<float> Altitudes, TConstArrayView<FVoxelMapTileSample> Samples, int32 Level = 0) const;

	/**
	 * The tile and pixel an apron grid point of a tile belongs to (a neighbouring tile's border pixel; the tile's own
	 * pixel for an interior point): on a heightmap world the tile a step over and the pixel wrapped, on a planet the cell
	 * the apron sample's direction falls in (across a face edge the neighbour is turned).
	 */
	void ApronPixel(FIntPoint Tile, int32 GX, int32 GY, const FVoxelMapTileSample& Sample, FIntPoint& OutTile, int32& OutPX, int32& OutPY) const;

	/**
	 * The conditioning over a tile and its apron (POI pads, campsites), gathered on the game thread: generation
	 * layers it onto the base height, so the map must too (QA W-05). What is ready (EVoxelConditioningGather::Ready): a
	 * tile does not wait for a line; it is drawn again when the conditioning changes.
	 */
	void GatherConditioning(const UVoxelChunkManager& ChunkManager, FIntPoint Tile, FVoxelConditioning& Out) const;

	// ---- The tile pyramid (MAP_PYRAMID_PLAN.md P1, D84) ----

	/** A plane's coarsest level (its pyramid has no top; the world map's zoom-out limit asks for far less). */
	static constexpr int32 MaxPlaneLevel = 16;

	/** The coarsest level: on a planet the one whose tile holds a whole face (ceil(log2 N)); on a plane MaxPlaneLevel. */
	int32 GetMaxLevel() const;

	/** Planets: tiles along a face edge at a level, ceil(N / 2^Level) (the face's last tile partly empty). */
	int32 TilesPerFaceEdgeAt(int32 Level) const;

	/** The world size of a level-0 pixel (a voxel column on a heightmap world; about one on a planet). */
	double GetPixelWorldSize() const { return GetTileWorldSize() / FMath::Max(1, ChunkSize); }

	/**
	 * The level whose pixels match a texel: the coarsest whose pixel spacing (2^L level-0 pixels) is at most the texel, so
	 * a view composed from it is as sharp as its texels allow and reads about one pixel a texel.
	 * @param TexelWorldSize The texel's size (uu)
	 * @return The level, 0 to GetMaxLevel()
	 */
	int32 LevelForTexel(double TexelWorldSize) const;

	/** The tile at a level holding a level-0 tile (X >> L, Y >> L on a plane; per face on a planet). */
	FVoxelMapTileId LevelTileOf(const FIntPoint& Tile0, int32 Level) const;

	/** The tile a level up holding a tile. */
	FVoxelMapTileId ParentOf(const FVoxelMapTileId& Id) const;

	/** The level-0 tile at a tile's minimum corner (on a planet always a cell of the atlas, for a valid tile). */
	FIntPoint FirstLevel0Tile(const FVoxelMapTileId& Id) const;

	/** Whether a tile at any level is one of the world's: inside the bounds on a heightmap world, in the level's atlas on a planet. */
	bool IsValidTile(const FVoxelMapTileId& Id) const;

	/** Whether the drawer can draw a tile at any level: any on a heightmap world, one in the level's atlas on a planet. */
	bool CanDrawTile(const FVoxelMapTileId& Id) const;

	/**
	 * Whether a level-0 tile lies under a tile or within Ring level-0 tiles of it (on a planet, on the same face): the edit
	 * overlays a coarse tile's pixels and apron read.
	 */
	bool CoversLevel0Tile(const FVoxelMapTileId& Id, const FIntPoint& Tile0, int32 Ring) const;

	/** The level-0 tiles under a tile and Ring around it (on a planet, those on its face). */
	void Level0TilesOf(const FVoxelMapTileId& Id, int32 Ring, TArray<FIntPoint>& OutTiles) const;

	/**
	 * The sample at a tile's grid point at any level: SampleAt's, with the pixels 2^Level level-0 pixels apart (a heightmap
	 * tile's pixel corners, a planet tile's pixel centres along its face from its first level-0 cell). Level 0 is SampleAt.
	 */
	FVoxelMapTileSample SampleAt(const FVoxelMapTileId& Id, int32 GX, int32 GY) const;

	/**
	 * The level-0 tile and pixel a grid point of a tile at any level falls in (the edit overlay drawn there): ApronPixel at
	 * level 0; on a heightmap world the level-0 pixel at the point's corner; on a planet the level-0 pixel the point's
	 * direction falls in.
	 */
	void SourcePixel(const FVoxelMapTileId& Id, int32 GX, int32 GY, const FVoxelMapTileSample& Sample, FIntPoint& OutTile0, int32& OutPX, int32& OutPY) const;

	/** GatherConditioning for a tile at any level: the conditioning over its area and apron. */
	void GatherConditioning(const UVoxelChunkManager& ChunkManager, const FVoxelMapTileId& Id, FVoxelConditioning& Out) const;

private:
	/** Planets: a tile's face and its indices along the face at its level. @return False outside the level's atlas */
	bool FaceIndicesAt(const FVoxelMapTileId& Id, int32& OutFace, int32& OutU, int32& OutV) const;
};
