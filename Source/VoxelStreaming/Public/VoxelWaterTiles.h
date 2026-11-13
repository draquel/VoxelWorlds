// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelWaterLevels.h"

class IVoxelMeshRenderer;

/** Where the water tiles lie: the chunk grid (UVoxelWorldConfiguration's values) and the world's water levels. */
struct FVoxelWaterTileGrid
{
	/** Voxels along a chunk's edge: a tile's mask is ChunkSize x ChunkSize columns */
	int32 ChunkSize = 32;

	/** A voxel's size (uu) */
	double VoxelSize = 100.0;

	/** World position of chunk (0, 0, 0)'s minimum corner */
	FVector WorldOrigin = FVector::ZeroVector;

	/** The world's water levels (UVoxelChunkManager::GetWaterLevels): each tile is meshed at the level over its column */
	FVoxelWaterLevels Water;

	/** A tile's bounds: its chunk column, at every height. */
	FBox GetTileBounds(const FIntVector2& Tile) const
	{
		const double Extent = ChunkSize * VoxelSize;
		const FVector2D Min(WorldOrigin.X + Tile.X * Extent, WorldOrigin.Y + Tile.Y * Extent);
		return FBox(FVector(Min.X, Min.Y, -UE_LARGE_WORLD_MAX), FVector(Min.X + Extent, Min.Y + Extent, UE_LARGE_WORLD_MAX));
	}

	/**
	 * World Z of the water surface a tile is meshed at: the levels' range over the tile's column (G19: world Z, as
	 * generation floods). One level a tile; a tile is flat.
	 */
	double GetTileLevel(const FIntVector2& Tile) const
	{
		return Water.GetRange(GetTileBounds(Tile)).GetLevel();
	}
};

/**
 * The 2D water tiles of a heightmap world: one tile per XY chunk column, its wet columns the OR of the masks the column's
 * loaded chunks contribute (FVoxelWaterMesher::BuildColumnMask), meshed at the sea plane and handed to the renderer
 * (IVoxelMeshRenderer::UpdateWaterTileMesh). The chunk manager builds each chunk's mask at its mesh submit and removes
 * it at its unload; this class keeps the masks, the dirty queue and the renderer's tiles. Game thread only.
 */
class VOXELSTREAMING_API FVoxelWaterTiles
{
public:
	/** One XY chunk column's water */
	struct FTile
	{
		/** Per chunk Z, the ChunkSize x ChunkSize wet-column mask that chunk contributes */
		TMap<int32, TArray<bool>> PartialMasks;

		/** Whether the combined mask needs meshing again */
		bool bDirty = true;
	};

	/**
	 * Set a chunk's contribution to its column's tile and queue the tile for meshing.
	 * @param ChunkCoord  The chunk
	 * @param Mask        Its wet columns (ChunkSize x ChunkSize), at least one set
	 * @return True when the column had no tile before (the far field's edge patches over it re-select)
	 */
	bool Contribute(const FIntVector& ChunkCoord, TArray<bool>&& Mask);

	/**
	 * Remove a chunk's contribution; the tile goes with its last contributor (its mesh removed from the renderer).
	 * @param ChunkCoord  The chunk
	 * @param Renderer    The renderer holding the tiles' meshes (may be null)
	 * @return True when the column's tile went
	 */
	bool Remove(const FIntVector& ChunkCoord, IVoxelMeshRenderer* Renderer);

	/**
	 * Mesh up to MaxTiles dirty tiles and hand them to the renderer (an empty mesh removes the tile's).
	 * @param MaxTiles  How many tiles to mesh this call
	 * @param Grid      The chunk grid and the sea plane
	 * @param Renderer  The renderer
	 */
	void ProcessDirty(int32 MaxTiles, const FVoxelWaterTileGrid& Grid, IVoxelMeshRenderer& Renderer);

	/** Queue every tile for meshing again (a renderer that lost its geometry, voxel.Water.ResendTiles). */
	void MarkAllDirty();

	/** Drop every tile and the queue (the renderer's meshes are not touched). */
	void Reset();

	/** @return Whether the XY chunk column has a tile */
	bool Has(const FIntPoint& Column) const { return Tiles.Contains(FIntVector2(Column.X, Column.Y)); }

	/** @return The column's tile, or null */
	const FTile* Find(const FIntVector2& Column) const { return Tiles.Find(Column); }

	/** @return Every tile by XY chunk column */
	const TMap<FIntVector2, FTile>& GetTiles() const { return Tiles; }

	/** @return Whether the column's tile is queued for meshing */
	bool IsQueued(const FIntVector2& Column) const { return DirtySet.Contains(Column); }

	/** @return How many tiles exist */
	int32 Num() const { return Tiles.Num(); }

	/**
	 * The chunk layers whose voxels hold water in the voxel column at a world position, before the tile's dilation.
	 * @param WorldPosition  Any point in the column
	 * @param Grid           The chunk grid
	 * @param OutWetChunkZ   The chunk Z of each layer whose mask has the column, sorted
	 * @return False when the column's chunk column has no tile
	 */
	bool GetColumn(const FVector& WorldPosition, const FVoxelWaterTileGrid& Grid, TArray<int32>& OutWetChunkZ) const;

private:
	/** Mark a tile dirty and queue it once */
	void MarkDirty(const FIntVector2& Column, FTile& Tile);

	/** XY chunk column -> its water */
	TMap<FIntVector2, FTile> Tiles;

	/** Tiles to mesh, the last first */
	TArray<FIntVector2> DirtyQueue;

	/** Set for O(1) duplicate detection in DirtyQueue */
	TSet<FIntVector2> DirtySet;
};
