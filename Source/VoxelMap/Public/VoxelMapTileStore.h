// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMapTypes.h"

/**
 * The map's tile images, every level of the tile pyramid in one least-recently-used store under one memory budget
 * (VoxelMap; MAP_PYRAMID_PLAN.md P1, D84: it replaces the detail and overview budgets of world modes review WM-E2). Kept
 * free of the world so the policy unit-tests headless; UVoxelMapSubsystem owns one and calls it on the game thread.
 *
 * Images are drawn on demand for the views (the minimap, the world map) at the level matching their texels, so the store
 * holds what the views showed last: about 300 tiles for the world map at any zoom, a few dozen for the minimap. A tile
 * at any level is ChunkSize^2 pixels (64 x 64 x 4 bytes = 16 KB on the demo); the default budget, 32 MB, holds 2,048.
 *
 * A tile is "used" when it is stored or read (Find with bTouch). Over the budget the least recently used images go, down
 * to 90% of it, so trimming runs rarely. A budget of 0 means unlimited. An image can be marked stale (an edit or the
 * terrain conditioning changed under it): it stays readable, so the views keep showing it, until its redraw lands.
 */
class VOXELMAP_API FVoxelMapTileStore
{
public:
	/**
	 * Set the budget (bytes of image data; 0 = unlimited). Trims at once if over.
	 * @param InBudgetBytes Bytes of images kept
	 */
	void SetBudget(int64 InBudgetBytes);

	/**
	 * Store a drawn tile (replacing its image and clearing its stale mark), then trim to the budget.
	 * @param Id Tile id
	 * @param Pixels Image, Resolution x Resolution (moved from)
	 * @param Resolution Pixels a side
	 */
	void Store(const FVoxelMapTileId& Id, TArray<FColor>&& Pixels, int32 Resolution);

	/**
	 * The tile's image.
	 * @param Id Tile id
	 * @param bTouch Count this read as a use (keeps the tile from eviction)
	 * @return The image, or null when the tile has none
	 */
	const FVoxelMapTile* Find(const FVoxelMapTileId& Id, bool bTouch = true) const;

	/** @return True when the tile has an image (stale or not) */
	bool Contains(const FVoxelMapTileId& Id) const { return Entries.Contains(Id); }

	/** @return True when the tile has an image marked stale */
	bool IsStale(const FVoxelMapTileId& Id) const;

	/**
	 * Mark a tile's image stale: still readable until its redraw is stored.
	 * @return True if the tile had an image
	 */
	bool MarkStale(const FVoxelMapTileId& Id);

	/** Drop a tile's image. */
	void Remove(const FVoxelMapTileId& Id);

	/** Drop everything. */
	void Reset();

	/** Trim to the budget now. @return Number of images dropped */
	int32 Enforce();

	/** @return Tiles with an image */
	int32 Num() const { return Entries.Num(); }

	/** @return Tiles with an image at a level */
	int32 NumAtLevel(int32 Level) const;

	/** @return Bytes of images */
	int64 GetBytes() const { return Bytes; }

	/** @return Images dropped since Reset */
	int64 GetEvictions() const { return Evictions; }

	/** @return The budget (0 = unlimited) */
	int64 GetBudget() const { return BudgetBytes; }

	/** Every tile id with an image (for redraws). */
	void GetIds(TArray<FVoxelMapTileId>& OutIds) const { Entries.GetKeys(OutIds); }

private:
	struct FEntry
	{
		FVoxelMapTile Tile;
		mutable uint64 LastUse = 0;
		bool bStale = false;
	};

	static int64 ImageBytes(const FVoxelMapTile& Tile) { return static_cast<int64>(Tile.PixelData.Num()) * sizeof(FColor); }

	TMap<FVoxelMapTileId, FEntry> Entries;
	mutable uint64 UseClock = 0;
	int64 Bytes = 0;
	int64 BudgetBytes = 0;
	int64 Evictions = 0;
};
