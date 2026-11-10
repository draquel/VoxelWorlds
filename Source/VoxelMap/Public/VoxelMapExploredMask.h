// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

/**
 * The map's exploration as a sparse mask over level-0 tiles (VoxelMap; MAP_PYRAMID_PLAN.md P2, D84): it replaces the key
 * per explored tile (a TSet<uint64>, 22 MB a million keys, walked in full by the fog of war, G170), so a whole explored
 * planet costs kilobytes and ExploreAll fills it in milliseconds. Kept free of the world so it unit-tests headless;
 * UVoxelMapSubsystem owns one and calls it on the game thread.
 *
 * Tiles are addressed per face: on a planet (Face, U, V), a cell of the cube-sphere grid's face, 0 <= U, V < N; on a
 * heightmap world Face 0 and (U, V) the chunk column, unbounded and possibly negative. The mask is made of blocks of
 * 64 x 64 tiles, each a 512-byte bitmap with a count; a block whose tiles are all explored collapses to its count (a
 * planet face's last blocks hold fewer tiles: they are full when those are). Each block keeps "any explored" bits over
 * its aligned 2^k squares (k 1-5), and above the blocks a sparse count per aligned square of 2^m blocks, so whether
 * anything in an aligned square of 2^K tiles is explored is one lookup at any K: the fog of war of a level-L pixel, which
 * covers 2^(L - log2 resolution) tiles a side (FReader caches the lookups for a fill).
 */
class VOXELMAP_API FVoxelMapExploredMask
{
	/** A block's bits (defined below). */
	struct FBlock;

public:
	/** A block is 2^BlockShift tiles a side. */
	static constexpr int32 BlockShift = 6;
	static constexpr int32 BlockSize = 1 << BlockShift;
	static constexpr int32 BlockArea = BlockSize * BlockSize;

	/** Levels of block summaries above a block: an aligned square of up to 2^(BlockShift + MaxCoarseLevels) tiles answers in one lookup. */
	static constexpr int32 MaxCoarseLevels = 12;

	/** What a block holds. */
	enum class EBlockState : uint8
	{
		None,
		Some,
		All
	};

	/**
	 * The mask's faces: 6 faces of N tiles a side on a planet; 1 face, unbounded, on a heightmap world. Clears the mask.
	 * @param InFaces Faces (1 or 6)
	 * @param InFaceTiles Tiles along a face's edge (0: unbounded)
	 */
	void Configure(int32 InFaces, int32 InFaceTiles);

	/** Drop every explored tile (the configuration stays). */
	void Reset();

	/** @return Faces (1 on a heightmap world, 6 on a planet) */
	int32 GetFaces() const { return Faces; }

	/** @return Tiles along a face's edge (0: unbounded) */
	int32 GetFaceTiles() const { return FaceTiles; }

	/** @return Whether a tile is one the mask can hold (on a planet inside the face; on a plane any) */
	bool IsInRange(int32 Face, int32 U, int32 V) const;

	/**
	 * Mark a tile explored.
	 * @return True if it was not explored before (false too outside the mask's range)
	 */
	bool Set(int32 Face, int32 U, int32 V);

	/**
	 * Mark a rectangle of tiles explored, whole blocks at once (ExploreAll). Clipped to the face on a planet.
	 * @param Face The face
	 * @param Rect Tiles from Rect.Min (inclusive) to Rect.Max (exclusive)
	 * @return Number of tiles newly explored
	 */
	int64 SetRect(int32 Face, const FIntRect& Rect);

	/** @return Whether a tile is explored */
	bool IsExplored(int32 Face, int32 U, int32 V) const;

	/**
	 * Whether any tile of the aligned square of 2^Log2Size tiles a side holding (U, V) is explored (Log2Size 0: the tile).
	 * @return True when one is
	 */
	bool IsAnyExplored(int32 Face, int32 U, int32 V, int32 Log2Size) const;

	/** @return What the block holding tile (U, V) holds */
	EBlockState GetBlockState(int32 Face, int32 U, int32 V) const;

	/** @return Explored tiles */
	int64 Num() const { return Count; }

	/** @return Blocks holding an explored tile */
	int32 NumBlocks() const { return Blocks.Num(); }

	/** @return Of those, the full blocks (collapsed) */
	int32 NumFullBlocks() const { return FullBlocks; }

	/** @return Bytes the mask holds (its blocks, bitmaps and summaries) */
	SIZE_T GetAllocatedSize() const;

	/** Every explored tile (O(explored): tests, the old blob, diagnostics). */
	void ForEach(TFunctionRef<void(int32 Face, int32 U, int32 V)> Visit) const;

	/**
	 * Drop explored tiles (a save explored outside the world's bounds).
	 * @param BlockMayHold Whether a block's tiles (Face, the block's tile rectangle) may hold one to drop; false keeps the block whole
	 * @param ShouldRemove Whether to drop a tile
	 * @return Number of tiles dropped
	 */
	int64 RemoveIf(TFunctionRef<bool(int32 Face, const FIntRect& BlockTiles)> BlockMayHold, TFunctionRef<bool(int32 Face, int32 U, int32 V)> ShouldRemove);

	/**
	 * Add every tile another mask holds (of the same configuration).
	 * @return Number of tiles newly explored
	 */
	int64 Merge(const FVoxelMapExploredMask& Other);

	/**
	 * Write the mask: the configuration, then each block, full ones by coordinate, partial ones with their bits.
	 * @param Ar The archive
	 */
	void Save(FArchive& Ar) const;

	/**
	 * Read what Save wrote, replacing the mask (configuration included).
	 * @return False for malformed data (the mask left empty)
	 */
	bool Load(FArchive& Ar);

	/**
	 * Cached reads for a fill (game thread, the mask unchanged meanwhile): neighbouring texels fall in the same block, so
	 * most lookups skip the hash.
	 */
	class VOXELMAP_API FReader
	{
	public:
		explicit FReader(const FVoxelMapExploredMask& InMask) : Mask(InMask) {}

		/** FVoxelMapExploredMask::IsAnyExplored, cached. */
		bool IsAnyExplored(int32 Face, int32 U, int32 V, int32 Log2Size);

	private:
		const FVoxelMapExploredMask& Mask;
		uint64 BlockKey = MAX_uint64;
		const FBlock* Block = nullptr;
		uint64 CoarseKey[MaxCoarseLevels + 1];
		bool bCoarseHit[MaxCoarseLevels + 1];
		bool bCoarseValid[MaxCoarseLevels + 1] = {};
	};

private:
	/** A block's bits: the 64 x 64 tiles row by row (64 words), then the "any" bits of its 2^k squares, k 1-5. */
	struct FBlock
	{
		/** Explored tiles in the block */
		int32 Count = 0;

		/** The bits (BitWords words); empty when the block is full */
		TArray<uint64> Bits;
	};

	/** Bit offsets of each level of a block's bits (level 0 the tiles, level k the 2^k squares). */
	static constexpr int32 LevelBitOffset[BlockShift + 1] = { 0, 4096, 5120, 5376, 5440, 5456, 5460 };
	static constexpr int32 BitWords = (5460 + 63) / 64;

	/** The hash key of a block (or a coarse square of blocks): the face and the two indices, 29 bits each. */
	static uint64 KeyOf(int32 Face, int32 BX, int32 BY);

	/** Tiles a block can hold: 4096, or fewer at a bounded face's far edges. */
	int32 CapacityOf(int32 BX, int32 BY) const;

	/** Set a tile's bit in a partial block and its squares' bits. @return True if new */
	static bool SetBit(FBlock& Block, int32 LU, int32 LV);

	/** Rebuild a partial block's square bits and count from its tile bits. */
	static void RebuildSquares(FBlock& Block);

	/** A block went from no tile to some: count it in the coarse squares over it. */
	void AddCoarse(int32 Face, int32 BX, int32 BY);

	/** A block went from some tiles to none. */
	void RemoveCoarse(int32 Face, int32 BX, int32 BY);

	/** Collapse a partial block that has become full. */
	void CollapseIfFull(FBlock& Block, int32 BX, int32 BY);

	/** Make a block full (every tile it holds explored). @return Tiles newly explored */
	int64 MakeFull(int32 Face, int32 BX, int32 BY);

	/** OR tile rows into a block (row Y's bit X is tile (X, Y) of the block), adding it if it had none. @return Tiles newly explored */
	int64 OrRows(int32 Face, int32 BX, int32 BY, const uint64 (&Rows)[BlockSize]);

	int32 Faces = 1;
	int32 FaceTiles = 0;
	int64 Count = 0;
	int32 FullBlocks = 0;
	TMap<uint64, FBlock> Blocks;

	/** Per coarse level m (index m - 1): the blocks holding a tile under each aligned square of 2^m blocks. */
	TMap<uint64, int32> Coarse[MaxCoarseLevels];
};
