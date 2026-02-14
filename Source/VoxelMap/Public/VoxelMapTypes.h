// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMapTypes.generated.h"

/**
 * A single map tile representing one chunk's XY footprint as a 2D color image.
 *
 * Each pixel maps to one voxel column in the chunk. The color is determined
 * by the surface material at that column (e.g., grass=green, stone=gray).
 *
 * Tiles are serializable via UPROPERTY for future save/load support.
 */
USTRUCT()
struct VOXELMAP_API FVoxelMapTile
{
	GENERATED_BODY()

	/** Tile coordinate in chunk space (X, Y). */
	UPROPERTY()
	FIntPoint TileCoord = FIntPoint(0, 0);

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
};
