// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMapTypes.h"

/**
 * The map's tile images under a fixed memory budget (VoxelMap, world modes review WM-E2, finding KI15). Kept free of
 * the world so the policy unit-tests headless; UVoxelMapSubsystem owns one and calls it on the game thread.
 *
 * Every drawn tile keeps two images:
 *  - the detail image, the tile's full resolution (ChunkSize pixels a side: 64 x 64 x 4 bytes = 16 KB on the demo),
 *    which the minimap and a zoomed-in world map draw;
 *  - the overview, the detail image box-filtered down OverviewDownsample times a side (16 x 16 = 1 KB), which a
 *    zoomed-out world map draws (at the world map's default zoom a tile spans 16 of its texels).
 *
 * Two budgets, each enforced least recently used first:
 *  - DetailBudgetBytes: over it, the least recently used detail images are dropped; their tiles keep the overview
 *    (GetTile returns it, with its own Resolution) and are drawn again in detail when something asks for detail.
 *  - OverviewBudgetBytes: over it, the least recently used tiles without a detail image lose their overview too; they
 *    stay explored (the exploration set is separate, and saved) and are drawn again when looked at.
 * A tile is "used" when it is stored or read (Find with bTouch). Eviction trims to 90% of a budget, so it runs rarely.
 * A budget of 0 means unlimited (the store then behaves as the map did before WM-E2: every image kept).
 */
class VOXELMAP_API FVoxelMapTileStore
{
public:
	/** The overview is the detail image shrunk this many times a side */
	static constexpr int32 OverviewDownsample = 4;

	/**
	 * Set the budgets (bytes of image data; 0 = unlimited). Trims at once if over.
	 * @param InDetailBudgetBytes Detail images
	 * @param InOverviewBudgetBytes Overview images
	 */
	void SetBudgets(int64 InDetailBudgetBytes, int64 InOverviewBudgetBytes);

	/**
	 * Store a drawn tile: its detail image and the overview made from it. Then trim to the budgets.
	 * @param Key Packed tile key
	 * @param Coord Tile coordinate
	 * @param Pixels Detail image, Resolution x Resolution (moved from)
	 * @param Resolution Pixels a side
	 * @param bKeepDetail Keep the detail image (else only the overview is kept, unless the tile already had detail)
	 */
	void Store(uint64 Key, FIntPoint Coord, TArray<FColor>&& Pixels, int32 Resolution, bool bKeepDetail);

	/**
	 * The tile's best image: the detail image if kept, else the overview (its Resolution says which).
	 * @param Key Packed tile key
	 * @param bTouch Count this read as a use (keeps the tile from eviction)
	 * @return The image, or null when the tile has none
	 */
	const FVoxelMapTile* Find(uint64 Key, bool bTouch = true) const;

	/** @return True when the tile has any image */
	bool Contains(uint64 Key) const { return Entries.Contains(Key); }

	/** @return True when the tile has its detail image */
	bool HasDetail(uint64 Key) const;

	/** Drop a tile's images. */
	void Remove(uint64 Key);

	/** Drop everything. */
	void Reset();

	/** Trim to the budgets now. @return Number of images dropped (detail and overview) */
	int32 Enforce();

	/** @return Tiles with an image */
	int32 Num() const { return Entries.Num(); }

	/** @return Tiles with their detail image */
	int32 NumDetail() const { return DetailCount; }

	/** @return Bytes of detail images */
	int64 GetDetailBytes() const { return DetailBytes; }

	/** @return Bytes of overview images */
	int64 GetOverviewBytes() const { return OverviewBytes; }

	/** @return Detail images dropped and overviews dropped since Reset */
	int64 GetDetailEvictions() const { return DetailEvictions; }
	int64 GetOverviewEvictions() const { return OverviewEvictions; }

	/** @return The budgets (0 = unlimited) */
	int64 GetDetailBudget() const { return DetailBudgetBytes; }
	int64 GetOverviewBudget() const { return OverviewBudgetBytes; }

	/** Every tile key with an image (for redraws). */
	void GetKeys(TArray<uint64>& OutKeys) const { Entries.GetKeys(OutKeys); }

	/**
	 * Pure: box-filter an image down Factor times a side (Resolution must divide by it).
	 * @return The smaller image ((Resolution / Factor)^2 pixels); empty if the sizes do not fit
	 */
	static TArray<FColor> Downsample(const TArray<FColor>& Pixels, int32 Resolution, int32 Factor);

private:
	struct FEntry
	{
		FVoxelMapTile Detail;    // PixelData empty when dropped
		FVoxelMapTile Overview;
		mutable uint64 LastUse = 0;
	};

	int64 ImageBytes(const FVoxelMapTile& Tile) const { return static_cast<int64>(Tile.PixelData.Num()) * sizeof(FColor); }

	TMap<uint64, FEntry> Entries;
	mutable uint64 UseClock = 0;
	int64 DetailBytes = 0;
	int64 OverviewBytes = 0;
	int32 DetailCount = 0;
	int64 DetailBudgetBytes = 0;
	int64 OverviewBudgetBytes = 0;
	int64 DetailEvictions = 0;
	int64 OverviewEvictions = 0;
};
