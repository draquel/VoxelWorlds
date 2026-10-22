// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelData.h"

/**
 * A chunk's voxels within Depth planes of each of its six faces (world modes review G69).
 *
 * A collision cook reads its 26 neighbours only next to the faces, edges and corners it shares with them (one plane at
 * collision LOD 0, stride + 1 planes above it: VoxelNeighborSlices::GetNeighborDepth), so it takes each neighbour's shell
 * instead of the neighbour's whole ChunkSize^3 array: at 64^3 and depth 1, 96 KB instead of 1 MB. Immutable once built;
 * shared between cooks like the seam snapshots.
 *
 * Layout: six slabs, Face = Axis * 2 + (High ? 1 : 0) for Axis 0 = X, 1 = Y, 2 = Z. Slab Face holds the voxels whose
 * coordinate along Axis lies in [0, Depth) (low face) or [ChunkSize - Depth, ChunkSize) (high face), at slab plane K =
 * that coordinate minus the slab's first, and in-plane index U + V * ChunkSize over the other two axes in order (Y, Z for
 * an X face; X, Z for a Y face; X, Y for a Z face). A voxel near an edge or a corner lies in several slabs; any of them
 * answers.
 */
struct FVoxelChunkShell
{
	/** Voxels per chunk axis. */
	int32 ChunkSize = 0;

	/** Planes held next to each face. */
	int32 Depth = 0;

	/** The six slabs, Depth * ChunkSize * ChunkSize voxels each. */
	TArray<FVoxelData> Slabs;

	/** Voxels in one slab. */
	FORCEINLINE int32 GetSlabVoxels() const { return Depth * ChunkSize * ChunkSize; }

	/**
	 * Whether the shell holds the voxel (it lies within Depth of a face).
	 *
	 * @param X Chunk-local X
	 * @param Y Chunk-local Y
	 * @param Z Chunk-local Z
	 * @return True if Get answers for it
	 */
	FORCEINLINE bool Contains(int32 X, int32 Y, int32 Z) const
	{
		if (X < 0 || Y < 0 || Z < 0 || X >= ChunkSize || Y >= ChunkSize || Z >= ChunkSize)
		{
			return false;
		}
		const int32 Hi = ChunkSize - Depth;
		return X < Depth || X >= Hi || Y < Depth || Y >= Hi || Z < Depth || Z >= Hi;
	}

	/**
	 * Read one voxel next to a face, edge or corner.
	 *
	 * @param X Chunk-local X
	 * @param Y Chunk-local Y
	 * @param Z Chunk-local Z
	 * @return The voxel, or air if the shell does not hold it (Contains is false)
	 */
	FORCEINLINE FVoxelData Get(int32 X, int32 Y, int32 Z) const
	{
		if (!Contains(X, Y, Z) || Slabs.Num() != 6 * GetSlabVoxels())
		{
			return FVoxelData::Air();
		}
		const int32 Hi = ChunkSize - Depth;
		const int32 Plane = ChunkSize * ChunkSize;
		const int32 SlabVoxels = GetSlabVoxels();
		if (X < Depth)  { return Slabs[0 * SlabVoxels + X * Plane + Y + Z * ChunkSize]; }
		if (X >= Hi)    { return Slabs[1 * SlabVoxels + (X - Hi) * Plane + Y + Z * ChunkSize]; }
		if (Y < Depth)  { return Slabs[2 * SlabVoxels + Y * Plane + X + Z * ChunkSize]; }
		if (Y >= Hi)    { return Slabs[3 * SlabVoxels + (Y - Hi) * Plane + X + Z * ChunkSize]; }
		if (Z < Depth)  { return Slabs[4 * SlabVoxels + Z * Plane + X + Y * ChunkSize]; }
		return Slabs[5 * SlabVoxels + (Z - Hi) * Plane + X + Y * ChunkSize];
	}

	/**
	 * Build a shell from a whole chunk (any thread).
	 *
	 * @param Full      The chunk's ChunkSize^3 voxels, index X + Y * ChunkSize + Z * ChunkSize^2
	 * @param InChunkSize Voxels per chunk axis
	 * @param InDepth   Planes to keep next to each face (clamped to 1..ChunkSize)
	 * @param Out       Receives the shell
	 * @return False if Full is not ChunkSize^3 voxels (Out is left empty)
	 */
	static bool Extract(const TArray<FVoxelData>& Full, int32 InChunkSize, int32 InDepth, FVoxelChunkShell& Out)
	{
		Out.ChunkSize = InChunkSize;
		Out.Depth = FMath::Clamp(InDepth, 1, FMath::Max(1, InChunkSize));
		Out.Slabs.Reset();
		const int32 CS = InChunkSize;
		if (CS <= 0 || Full.Num() != CS * CS * CS)
		{
			return false;
		}
		const int32 D = Out.Depth;
		const int32 Plane = CS * CS;
		const int32 SlabVoxels = Out.GetSlabVoxels();
		Out.Slabs.SetNumUninitialized(6 * SlabVoxels);
		FVoxelData* Dst = Out.Slabs.GetData();
		const FVoxelData* Src = Full.GetData();
		for (int32 K = 0; K < D; ++K)
		{
			const int32 Lo = K;
			const int32 Hi = CS - D + K;
			// X faces: (U, V) = (Y, Z).
			for (int32 Z = 0; Z < CS; ++Z)
			{
				for (int32 Y = 0; Y < CS; ++Y)
				{
					Dst[0 * SlabVoxels + K * Plane + Y + Z * CS] = Src[Lo + Y * CS + Z * Plane];
					Dst[1 * SlabVoxels + K * Plane + Y + Z * CS] = Src[Hi + Y * CS + Z * Plane];
				}
			}
			// Y faces: (U, V) = (X, Z); rows of X are contiguous in both.
			for (int32 Z = 0; Z < CS; ++Z)
			{
				FMemory::Memcpy(&Dst[2 * SlabVoxels + K * Plane + Z * CS], &Src[Lo * CS + Z * Plane], CS * sizeof(FVoxelData));
				FMemory::Memcpy(&Dst[3 * SlabVoxels + K * Plane + Z * CS], &Src[Hi * CS + Z * Plane], CS * sizeof(FVoxelData));
			}
			// Z faces: (U, V) = (X, Y); a whole plane is contiguous.
			FMemory::Memcpy(&Dst[4 * SlabVoxels + K * Plane], &Src[Lo * Plane], Plane * sizeof(FVoxelData));
			FMemory::Memcpy(&Dst[5 * SlabVoxels + K * Plane], &Src[Hi * Plane], Plane * sizeof(FVoxelData));
		}
		return true;
	}

	/**
	 * Build the shell of a uniform chunk (every voxel Value).
	 *
	 * @param Value     The chunk's voxel
	 * @param InChunkSize Voxels per chunk axis
	 * @param InDepth   Planes next to each face
	 * @param Out       Receives the shell
	 */
	static void FillUniform(const FVoxelData& Value, int32 InChunkSize, int32 InDepth, FVoxelChunkShell& Out)
	{
		Out.ChunkSize = InChunkSize;
		Out.Depth = FMath::Clamp(InDepth, 1, FMath::Max(1, InChunkSize));
		Out.Slabs.Init(Value, 6 * Out.GetSlabVoxels());
	}

	/** Bytes held. */
	SIZE_T GetAllocatedSize() const { return Slabs.GetAllocatedSize(); }
};
