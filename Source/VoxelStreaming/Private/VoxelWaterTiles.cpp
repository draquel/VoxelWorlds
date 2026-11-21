// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterTiles.h"
#include "VoxelStreaming.h"
#include "VoxelCoordinates.h"
#include "VoxelWaterMesher.h"
#include "IVoxelMeshRenderer.h"
#include "ChunkRenderData.h"

bool FVoxelWaterTiles::Contribute(const FIntVector& ChunkCoord, TArray<bool>&& Mask)
{
	const FIntVector2 TileCoord(ChunkCoord.X, ChunkCoord.Y);
	const bool bNewTile = !Tiles.Contains(TileCoord);
	FTile& Tile = Tiles.FindOrAdd(TileCoord);
	Tile.PartialMasks.Add(ChunkCoord.Z, MoveTemp(Mask));

	// Mark dirty and queue
	MarkDirty(TileCoord, Tile);
	return bNewTile;
}

bool FVoxelWaterTiles::Remove(const FIntVector& ChunkCoord, IVoxelMeshRenderer* Renderer)
{
	FIntVector2 TileCoord(ChunkCoord.X, ChunkCoord.Y);
	FTile* Tile = Tiles.Find(TileCoord);
	if (!Tile)
	{
		return false;
	}

	if (Tile->PartialMasks.Remove(ChunkCoord.Z) == 0)
	{
		// This chunk contributed nothing — the tile's combined mask is unchanged
		return false;
	}

	if (Tile->PartialMasks.IsEmpty())
	{
		// No more contributors — remove the tile entirely.
		if (Renderer)
		{
			Renderer->RemoveWaterTile(TileCoord);
		}
		Tiles.Remove(TileCoord);
		DirtySet.Remove(TileCoord);
		return true;
	}

	// Still has contributors — mark dirty for re-evaluation
	MarkDirty(TileCoord, *Tile);
	return false;
}

void FVoxelWaterTiles::ProcessDirty(int32 MaxTiles, const FVoxelWaterTileGrid& Grid, IVoxelMeshRenderer& Renderer)
{
	int32 Processed = 0;
	while (DirtyQueue.Num() > 0 && Processed < MaxTiles)
	{
		FIntVector2 TileCoord = DirtyQueue.Last();
		DirtyQueue.Pop();
		DirtySet.Remove(TileCoord);

		FTile* Tile = Tiles.Find(TileCoord);
		if (!Tile || !Tile->bDirty)
		{
			continue;
		}
		Tile->bDirty = false;

		// Combine all partial masks via OR
		const int32 CS = Grid.ChunkSize;
		TArray<bool> CombinedMask;
		CombinedMask.SetNumZeroed(CS * CS);
		for (auto& Pair : Tile->PartialMasks)
		{
			for (int32 i = 0; i < CS * CS; i++)
			{
				if (Pair.Value[i])
				{
					CombinedMask[i] = true;
				}
			}
		}

		// Generate water mesh from combined mask
		const float ChunkExtent = static_cast<float>(CS) * Grid.VoxelSize;
		const FVector TileWorldPos(
			Grid.WorldOrigin.X + TileCoord.X * ChunkExtent,
			Grid.WorldOrigin.Y + TileCoord.Y * ChunkExtent,
			Grid.WorldOrigin.Z);

		FChunkMeshData WaterMeshData;
		FVoxelWaterMesher::GenerateWaterMeshFromMask(
			CombinedMask, CS, Grid.VoxelSize,
			TileWorldPos, Grid.GetTileLevel(TileCoord), WaterMeshData);

		if (WaterMeshData.IsValid())
		{
			Renderer.UpdateWaterTileMesh(TileCoord, WaterMeshData);
			UE_LOG(LogVoxelStreaming, Log, TEXT("Water tile (%d,%d): mesh submitted — %d verts, %d tris"),
				TileCoord.X, TileCoord.Y,
				WaterMeshData.GetVertexCount(), WaterMeshData.GetTriangleCount());
		}
		else
		{
			Renderer.RemoveWaterTile(TileCoord);
		}
		++Processed;
	}
}

void FVoxelWaterTiles::MarkAllDirty()
{
	for (TPair<FIntVector2, FTile>& Tile : Tiles)
	{
		MarkDirty(Tile.Key, Tile.Value);
	}
}

void FVoxelWaterTiles::Reset()
{
	Tiles.Empty();
	DirtyQueue.Empty();
	DirtySet.Empty();
}

bool FVoxelWaterTiles::GetColumn(const FVector& WorldPosition, const FVoxelWaterTileGrid& Grid, TArray<int32>& OutWetChunkZ) const
{
	OutWetChunkZ.Reset();
	const int32 CS = Grid.ChunkSize;
	const FIntVector Voxel = FVoxelCoordinates::WorldToVoxel(WorldPosition - Grid.WorldOrigin, Grid.VoxelSize);
	const FIntVector2 TileCoord(FMath::FloorToInt32(double(Voxel.X) / CS), FMath::FloorToInt32(double(Voxel.Y) / CS));
	const FTile* Tile = Tiles.Find(TileCoord);
	if (!Tile)
	{
		return false;
	}
	const int32 Column = (Voxel.X - TileCoord.X * CS) + (Voxel.Y - TileCoord.Y * CS) * CS;
	for (const TPair<int32, TArray<bool>>& Layer : Tile->PartialMasks)
	{
		if (Layer.Value.IsValidIndex(Column) && Layer.Value[Column])
		{
			OutWetChunkZ.Add(Layer.Key);
		}
	}
	OutWetChunkZ.Sort();
	return true;
}

void FVoxelWaterTiles::MarkDirty(const FIntVector2& Column, FTile& Tile)
{
	Tile.bDirty = true;
	if (!DirtySet.Contains(Column))
	{
		DirtyQueue.Add(Column);
		DirtySet.Add(Column);
	}
}
