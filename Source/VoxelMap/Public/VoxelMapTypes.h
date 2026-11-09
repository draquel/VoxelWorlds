// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMapTypes.generated.h"

/**
 * A map tile's place in the tile pyramid (MAP_PYRAMID_PLAN.md P1, D84). Level 0 is a chunk column (a heightmap world) or a
 * cell of the cube-sphere grid (a planet), ChunkSize pixels a side. A level-L tile covers 2^L x 2^L level-0 tiles at the
 * same pixels, so its pixels are 2^L voxel columns apart: on a heightmap world its coordinate is the level-0 one shifted
 * right by L (X >> L, Y >> L, unbounded); on a planet a quadtree per cube face, the atlas coordinate (Face x N_L + U >> L,
 * V >> L) with N_L = ceil(N / 2^L) tiles a face edge (FVoxelMapTileFrame has the arithmetic).
 */
struct FVoxelMapTileId
{
	/** The tile's coordinate at its level (a level-0 tile's chunk column or atlas coordinate) */
	FIntPoint Coord = FIntPoint(0, 0);

	/** The pyramid level (0 = a chunk across) */
	int32 Level = 0;

	FVoxelMapTileId() = default;
	explicit FVoxelMapTileId(const FIntPoint& InCoord, int32 InLevel = 0) : Coord(InCoord), Level(InLevel) {}

	bool operator==(const FVoxelMapTileId& Other) const { return Coord == Other.Coord && Level == Other.Level; }
	bool operator!=(const FVoxelMapTileId& Other) const { return !(*this == Other); }

	friend uint32 GetTypeHash(const FVoxelMapTileId& Id)
	{
		return HashCombineFast(GetTypeHash(Id.Coord), ::GetTypeHash(Id.Level));
	}

	/** "L2 (3, -4)" */
	FString ToString() const { return FString::Printf(TEXT("L%d (%d, %d)"), Level, Coord.X, Coord.Y); }
};

/**
 * One map tile's image at a level of the tile pyramid: ChunkSize x ChunkSize pixels, a level-0 tile a voxel column a pixel,
 * a level-L tile 2^L voxel columns a pixel (FVoxelMapTileId).
 *
 * The color is determined by the surface material at each pixel's sample (e.g., grass=green, stone=gray), shaded.
 */
USTRUCT()
struct VOXELMAP_API FVoxelMapTile
{
	GENERATED_BODY()

	/** Tile coordinate at its level (chunk X, Y at level 0; a planet's atlas coordinate). */
	UPROPERTY()
	FIntPoint TileCoord = FIntPoint(0, 0);

	/** The tile's pyramid level (FVoxelMapTileId). */
	UPROPERTY()
	int32 Level = 0;

	/** Pixel data in BGRA format. Array size = Resolution * Resolution. */
	UPROPERTY()
	TArray<FColor> PixelData;

	/** Pixels per edge (matches ChunkSize, e.g. 32). */
	UPROPERTY()
	int32 Resolution = 0;

	/** Format version for future save/load compatibility. */
	UPROPERTY()
	uint8 Version = 1;

	/** Runtime-only flag: true when pixel data has been fully generated. */
	bool bIsReady = false;

	/** The tile's id. */
	FVoxelMapTileId GetId() const { return FVoxelMapTileId(TileCoord, Level); }
};
