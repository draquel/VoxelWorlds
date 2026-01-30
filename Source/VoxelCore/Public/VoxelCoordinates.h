// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"

/**
 * Coordinate conversion utilities for voxel systems.
 *
 * Provides conversions between:
 * - World space (Unreal units, FVector)
 * - Chunk space (chunk coordinates, FIntVector)
 * - Local voxel space (within-chunk coordinates, FIntVector)
 * - Voxel space (global voxel coordinates, FIntVector)
 *
 * All functions are static and stateless.
 * Thread Safety: All methods are thread-safe (pure functions)
 *
 * @see Documentation/ARCHITECTURE.md
 */
namespace FVoxelCoordinates
{
	/**
	 * Convert world position to chunk coordinates.
	 *
	 * @param WorldPos World-space position
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @return Chunk coordinate containing this position
	 */
	FORCEINLINE FIntVector WorldToChunk(const FVector& WorldPos, int32 ChunkSize, float VoxelSize)
	{
		const float ChunkWorldSize = ChunkSize * VoxelSize;
		return FIntVector(
			FMath::FloorToInt(WorldPos.X / ChunkWorldSize),
			FMath::FloorToInt(WorldPos.Y / ChunkWorldSize),
			FMath::FloorToInt(WorldPos.Z / ChunkWorldSize)
		);
	}

	/**
	 * Convert world position to local voxel position within a chunk.
	 *
	 * @param WorldPos World-space position
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @return Local voxel position (0 to ChunkSize-1 per axis)
	 */
	FORCEINLINE FIntVector WorldToLocalVoxel(const FVector& WorldPos, int32 ChunkSize, float VoxelSize)
	{
		const float ChunkWorldSize = ChunkSize * VoxelSize;

		// Get position within chunk (0 to ChunkWorldSize)
		FVector LocalWorld(
			FMath::Fmod(WorldPos.X, ChunkWorldSize),
			FMath::Fmod(WorldPos.Y, ChunkWorldSize),
			FMath::Fmod(WorldPos.Z, ChunkWorldSize)
		);

		// Handle negative coordinates
		if (LocalWorld.X < 0) LocalWorld.X += ChunkWorldSize;
		if (LocalWorld.Y < 0) LocalWorld.Y += ChunkWorldSize;
		if (LocalWorld.Z < 0) LocalWorld.Z += ChunkWorldSize;

		return FIntVector(
			FMath::Clamp(FMath::FloorToInt(LocalWorld.X / VoxelSize), 0, ChunkSize - 1),
			FMath::Clamp(FMath::FloorToInt(LocalWorld.Y / VoxelSize), 0, ChunkSize - 1),
			FMath::Clamp(FMath::FloorToInt(LocalWorld.Z / VoxelSize), 0, ChunkSize - 1)
		);
	}

	/**
	 * Convert world position to global voxel coordinates.
	 *
	 * @param WorldPos World-space position
	 * @param VoxelSize World units per voxel
	 * @return Global voxel coordinate
	 */
	FORCEINLINE FIntVector WorldToVoxel(const FVector& WorldPos, float VoxelSize)
	{
		return FIntVector(
			FMath::FloorToInt(WorldPos.X / VoxelSize),
			FMath::FloorToInt(WorldPos.Y / VoxelSize),
			FMath::FloorToInt(WorldPos.Z / VoxelSize)
		);
	}

	/**
	 * Convert chunk coordinate to world-space origin (minimum corner).
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @return World position of chunk's minimum corner
	 */
	FORCEINLINE FVector ChunkToWorld(const FIntVector& ChunkCoord, int32 ChunkSize, float VoxelSize)
	{
		const float ChunkWorldSize = ChunkSize * VoxelSize;
		return FVector(
			ChunkCoord.X * ChunkWorldSize,
			ChunkCoord.Y * ChunkWorldSize,
			ChunkCoord.Z * ChunkWorldSize
		);
	}

	/**
	 * Get world-space center of a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @return World position of chunk center
	 */
	FORCEINLINE FVector ChunkToWorldCenter(const FIntVector& ChunkCoord, int32 ChunkSize, float VoxelSize)
	{
		const float ChunkWorldSize = ChunkSize * VoxelSize;
		const float HalfChunkSize = ChunkWorldSize * 0.5f;
		return FVector(
			ChunkCoord.X * ChunkWorldSize + HalfChunkSize,
			ChunkCoord.Y * ChunkWorldSize + HalfChunkSize,
			ChunkCoord.Z * ChunkWorldSize + HalfChunkSize
		);
	}

	/**
	 * Get world-space bounding box for a chunk.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @return Axis-aligned bounding box in world space
	 */
	FORCEINLINE FBox ChunkToWorldBounds(const FIntVector& ChunkCoord, int32 ChunkSize, float VoxelSize)
	{
		const FVector Min = ChunkToWorld(ChunkCoord, ChunkSize, VoxelSize);
		const FVector Max = Min + FVector(ChunkSize * VoxelSize);
		return FBox(Min, Max);
	}

	/**
	 * Convert local voxel position to world position (voxel center).
	 *
	 * @param ChunkCoord Chunk containing the voxel
	 * @param LocalPos Local voxel position within chunk
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @return World position of voxel center
	 */
	FORCEINLINE FVector LocalVoxelToWorld(const FIntVector& ChunkCoord, const FIntVector& LocalPos, int32 ChunkSize, float VoxelSize)
	{
		const FVector ChunkOrigin = ChunkToWorld(ChunkCoord, ChunkSize, VoxelSize);
		const float HalfVoxel = VoxelSize * 0.5f;
		return ChunkOrigin + FVector(
			LocalPos.X * VoxelSize + HalfVoxel,
			LocalPos.Y * VoxelSize + HalfVoxel,
			LocalPos.Z * VoxelSize + HalfVoxel
		);
	}

	/**
	 * Convert global voxel coordinate to chunk coordinate.
	 *
	 * @param VoxelCoord Global voxel coordinate
	 * @param ChunkSize Voxels per chunk edge
	 * @return Chunk coordinate containing this voxel
	 */
	FORCEINLINE FIntVector VoxelToChunk(const FIntVector& VoxelCoord, int32 ChunkSize)
	{
		return FIntVector(
			FMath::FloorToInt(static_cast<float>(VoxelCoord.X) / ChunkSize),
			FMath::FloorToInt(static_cast<float>(VoxelCoord.Y) / ChunkSize),
			FMath::FloorToInt(static_cast<float>(VoxelCoord.Z) / ChunkSize)
		);
	}

	/**
	 * Convert global voxel coordinate to local voxel position.
	 *
	 * @param VoxelCoord Global voxel coordinate
	 * @param ChunkSize Voxels per chunk edge
	 * @return Local position within chunk (0 to ChunkSize-1)
	 */
	FORCEINLINE FIntVector VoxelToLocal(const FIntVector& VoxelCoord, int32 ChunkSize)
	{
		FIntVector Local(
			VoxelCoord.X % ChunkSize,
			VoxelCoord.Y % ChunkSize,
			VoxelCoord.Z % ChunkSize
		);

		// Handle negative coordinates
		if (Local.X < 0) Local.X += ChunkSize;
		if (Local.Y < 0) Local.Y += ChunkSize;
		if (Local.Z < 0) Local.Z += ChunkSize;

		return Local;
	}

	/**
	 * Convert local voxel position to global voxel coordinate.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @param LocalPos Local voxel position
	 * @param ChunkSize Voxels per chunk edge
	 * @return Global voxel coordinate
	 */
	FORCEINLINE FIntVector LocalToVoxel(const FIntVector& ChunkCoord, const FIntVector& LocalPos, int32 ChunkSize)
	{
		return FIntVector(
			ChunkCoord.X * ChunkSize + LocalPos.X,
			ChunkCoord.Y * ChunkSize + LocalPos.Y,
			ChunkCoord.Z * ChunkSize + LocalPos.Z
		);
	}

	/**
	 * Get the 6 face-adjacent neighbor chunk coordinates.
	 *
	 * @param ChunkCoord Center chunk coordinate
	 * @param OutNeighbors Array to fill with 6 neighbor coordinates
	 */
	FORCEINLINE void GetAdjacentChunks(const FIntVector& ChunkCoord, TArray<FIntVector>& OutNeighbors)
	{
		OutNeighbors.Reset(6);
		OutNeighbors.Add(ChunkCoord + FIntVector(1, 0, 0));   // +X
		OutNeighbors.Add(ChunkCoord + FIntVector(-1, 0, 0));  // -X
		OutNeighbors.Add(ChunkCoord + FIntVector(0, 1, 0));   // +Y
		OutNeighbors.Add(ChunkCoord + FIntVector(0, -1, 0));  // -Y
		OutNeighbors.Add(ChunkCoord + FIntVector(0, 0, 1));   // +Z
		OutNeighbors.Add(ChunkCoord + FIntVector(0, 0, -1));  // -Z
	}

	/**
	 * Get all 26 surrounding neighbor chunk coordinates.
	 *
	 * @param ChunkCoord Center chunk coordinate
	 * @param OutNeighbors Array to fill with 26 neighbor coordinates
	 */
	FORCEINLINE void GetAllNeighborChunks(const FIntVector& ChunkCoord, TArray<FIntVector>& OutNeighbors)
	{
		OutNeighbors.Reset(26);
		for (int32 X = -1; X <= 1; ++X)
		{
			for (int32 Y = -1; Y <= 1; ++Y)
			{
				for (int32 Z = -1; Z <= 1; ++Z)
				{
					if (X != 0 || Y != 0 || Z != 0)
					{
						OutNeighbors.Add(ChunkCoord + FIntVector(X, Y, Z));
					}
				}
			}
		}
	}

	/**
	 * Calculate squared distance between viewer and chunk center.
	 *
	 * @param ViewerPos Viewer world position
	 * @param ChunkCoord Chunk coordinate
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @return Squared distance in world units
	 */
	FORCEINLINE float GetChunkDistanceSquared(const FVector& ViewerPos, const FIntVector& ChunkCoord, int32 ChunkSize, float VoxelSize)
	{
		const FVector ChunkCenter = ChunkToWorldCenter(ChunkCoord, ChunkSize, VoxelSize);
		return FVector::DistSquared(ViewerPos, ChunkCenter);
	}

	/**
	 * Calculate distance between viewer and chunk center.
	 *
	 * @param ViewerPos Viewer world position
	 * @param ChunkCoord Chunk coordinate
	 * @param ChunkSize Voxels per chunk edge
	 * @param VoxelSize World units per voxel
	 * @return Distance in world units
	 */
	FORCEINLINE float GetChunkDistance(const FVector& ViewerPos, const FIntVector& ChunkCoord, int32 ChunkSize, float VoxelSize)
	{
		return FMath::Sqrt(GetChunkDistanceSquared(ViewerPos, ChunkCoord, ChunkSize, VoxelSize));
	}

	/**
	 * Get face normal for a voxel face direction.
	 *
	 * @param Face Face direction
	 * @return Unit normal vector
	 */
	FORCEINLINE FVector GetFaceNormal(EVoxelFace Face)
	{
		switch (Face)
		{
			case EVoxelFace::Top:    return FVector(0, 0, 1);
			case EVoxelFace::Bottom: return FVector(0, 0, -1);
			case EVoxelFace::North:  return FVector(0, 1, 0);
			case EVoxelFace::South:  return FVector(0, -1, 0);
			case EVoxelFace::East:   return FVector(1, 0, 0);
			case EVoxelFace::West:   return FVector(-1, 0, 0);
			default:                 return FVector::ZeroVector;
		}
	}

	/**
	 * Get neighbor voxel offset for a face direction.
	 *
	 * @param Face Face direction
	 * @return Integer offset to neighbor voxel
	 */
	FORCEINLINE FIntVector GetFaceOffset(EVoxelFace Face)
	{
		switch (Face)
		{
			case EVoxelFace::Top:    return FIntVector(0, 0, 1);
			case EVoxelFace::Bottom: return FIntVector(0, 0, -1);
			case EVoxelFace::North:  return FIntVector(0, 1, 0);
			case EVoxelFace::South:  return FIntVector(0, -1, 0);
			case EVoxelFace::East:   return FIntVector(1, 0, 0);
			case EVoxelFace::West:   return FIntVector(-1, 0, 0);
			default:                 return FIntVector::ZeroValue;
		}
	}

	/**
	 * Get the opposite face direction.
	 *
	 * @param Face Input face
	 * @return Opposite face
	 */
	FORCEINLINE EVoxelFace GetOppositeFace(EVoxelFace Face)
	{
		switch (Face)
		{
			case EVoxelFace::Top:    return EVoxelFace::Bottom;
			case EVoxelFace::Bottom: return EVoxelFace::Top;
			case EVoxelFace::North:  return EVoxelFace::South;
			case EVoxelFace::South:  return EVoxelFace::North;
			case EVoxelFace::East:   return EVoxelFace::West;
			case EVoxelFace::West:   return EVoxelFace::East;
			default:                 return Face;
		}
	}
}
