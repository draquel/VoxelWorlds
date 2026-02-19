// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWaterPropagation.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelData.h"
#include "VoxelCoordinates.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelCoreTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelWaterPropagation, Log, All);

void UVoxelWaterPropagation::Initialize(UVoxelChunkManager* InChunkManager, UVoxelEditManager* InEditManager, float InWaterLevel)
{
	ChunkManager = InChunkManager;
	EditManager = InEditManager;
	WaterLevel = InWaterLevel;

	if (InChunkManager)
	{
		if (UVoxelWorldConfiguration* Config = InChunkManager->GetConfiguration())
		{
			ChunkSize = Config->ChunkSize;
			VoxelSize = Config->VoxelSize;
			WorldOrigin = Config->WorldOrigin;
		}
	}

	UE_LOG(LogVoxelWaterPropagation, Log, TEXT("Water propagation initialized (WaterLevel=%.0f, ChunkSize=%d, VoxelSize=%.0f)"),
		WaterLevel, ChunkSize, VoxelSize);
}

void UVoxelWaterPropagation::OnChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius)
{
	if (!ChunkManager.IsValid() || EditRadius <= 0.f)
	{
		return;
	}

	// Scan voxels in the edit sphere to find newly exposed air adjacent to water.
	// We iterate over the bounding box of the edit sphere in voxel steps.
	const float ScanRadius = EditRadius + VoxelSize; // Pad slightly to catch edge cases
	const FVector MinCorner = EditCenter - FVector(ScanRadius);
	const FVector MaxCorner = EditCenter + FVector(ScanRadius);

	const FIntVector MinVoxel = WorldToVoxelKey(MinCorner);
	const FIntVector MaxVoxel = WorldToVoxelKey(MaxCorner);

	// 6-connected neighbor offsets
	static const FIntVector Offsets[6] = {
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0),
		FIntVector(0, 0, 1), FIntVector(0, 0, -1)
	};

	int32 SeedsFound = 0;

	for (int32 Z = MinVoxel.Z; Z <= MaxVoxel.Z; ++Z)
	{
		for (int32 Y = MinVoxel.Y; Y <= MaxVoxel.Y; ++Y)
		{
			for (int32 X = MinVoxel.X; X <= MaxVoxel.X; ++X)
			{
				const FIntVector VoxelKey(X, Y, Z);
				const FVector WorldPos = VoxelKeyToWorld(VoxelKey);

				// Only consider voxels within the actual edit sphere
				if (FVector::DistSquared(WorldPos, EditCenter) > ScanRadius * ScanRadius)
				{
					continue;
				}

				// This voxel must be air, below water level, and NOT already water-flagged
				const FVoxelData Voxel = ChunkManager->GetVoxelAtWorldPosition(WorldPos);
				if (!Voxel.IsAir() || Voxel.HasWaterFlag() || WorldPos.Z > WaterLevel)
				{
					continue;
				}

				// Check if any face-adjacent neighbor has a water flag
				bool bAdjacentToWater = false;
				for (const FIntVector& Offset : Offsets)
				{
					const FVector NeighborPos = VoxelKeyToWorld(VoxelKey + Offset);
					const FVoxelData Neighbor = ChunkManager->GetVoxelAtWorldPosition(NeighborPos);
					if (Neighbor.HasWaterFlag())
					{
						bAdjacentToWater = true;
						break;
					}
				}

				if (bAdjacentToWater)
				{
					// Seed the BFS from this voxel
					if (!Visited.Contains(VoxelKey))
					{
						BFSQueue.Add(WorldPos);
						Visited.Add(VoxelKey);
						++SeedsFound;
					}
				}
			}
		}
	}

	if (SeedsFound > 0)
	{
		// Reset total propagation counter for a new flood event
		TotalPropagated = 0;
		UE_LOG(LogVoxelWaterPropagation, Log, TEXT("Water propagation seeded with %d voxels near edit at (%.0f, %.0f, %.0f)"),
			SeedsFound, EditCenter.X, EditCenter.Y, EditCenter.Z);
	}
}

int32 UVoxelWaterPropagation::ProcessPropagation(int32 MaxVoxelsPerFrame)
{
	if (BFSQueue.Num() == 0 || !ChunkManager.IsValid() || !EditManager.IsValid())
	{
		return 0;
	}

	// 6-connected neighbor offsets
	static const FIntVector Offsets[6] = {
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0),
		FIntVector(0, 0, 1), FIntVector(0, 0, -1)
	};

	int32 ProcessedThisFrame = 0;
	TSet<FIntVector> DirtyChunks;

	// Begin an edit operation so all water flag changes are grouped
	EditManager->SetEditSource(EEditSource::System);
	EditManager->BeginEditOperation(TEXT("Water Propagation"));

	while (BFSQueue.Num() > 0 && ProcessedThisFrame < MaxVoxelsPerFrame && TotalPropagated < MaxPropagationVoxels)
	{
		// Pop from front (FIFO for BFS). Use index 0 for correctness.
		const FVector CurrentPos = BFSQueue[0];
		BFSQueue.RemoveAt(0, EAllowShrinking::No);

		// Verify this voxel can still receive water (may have changed since queued)
		if (!CanReceiveWater(CurrentPos))
		{
			continue;
		}

		// Set water flag on this voxel via the edit manager
		FVoxelData WaterVoxel = ChunkManager->GetVoxelAtWorldPosition(CurrentPos);
		WaterVoxel.SetWaterFlag(true);

		EditManager->ApplyEdit(CurrentPos, WaterVoxel, EEditMode::Set);

		++ProcessedThisFrame;
		++TotalPropagated;

		// Track which chunk was modified for remeshing
		const FIntVector ChunkCoord = FVoxelCoordinates::WorldToChunk(CurrentPos, ChunkSize, VoxelSize);
		DirtyChunks.Add(ChunkCoord);

		// Enqueue 6-connected neighbors
		const FIntVector CurrentKey = WorldToVoxelKey(CurrentPos);
		for (const FIntVector& Offset : Offsets)
		{
			const FIntVector NeighborKey = CurrentKey + Offset;
			if (!Visited.Contains(NeighborKey))
			{
				const FVector NeighborPos = VoxelKeyToWorld(NeighborKey);
				if (CanReceiveWater(NeighborPos))
				{
					Visited.Add(NeighborKey);
					BFSQueue.Add(NeighborPos);
				}
			}
		}
	}

	EditManager->EndEditOperation();

	// Mark all modified chunks dirty for remeshing (the OnChunkEdited handler
	// already does this for individual edits, but we mark explicitly in case
	// the edit manager batched them)
	for (const FIntVector& ChunkCoord : DirtyChunks)
	{
		ChunkManager->MarkChunkDirty(ChunkCoord);
	}

	if (ProcessedThisFrame > 0)
	{
		UE_LOG(LogVoxelWaterPropagation, Verbose, TEXT("Water propagation: %d voxels this frame, %d total, %d remaining in queue"),
			ProcessedThisFrame, TotalPropagated, BFSQueue.Num());
	}

	// If we hit the total limit, clear the queue
	if (TotalPropagated >= MaxPropagationVoxels && BFSQueue.Num() > 0)
	{
		UE_LOG(LogVoxelWaterPropagation, Log, TEXT("Water propagation reached max limit (%d voxels), clearing %d remaining"),
			MaxPropagationVoxels, BFSQueue.Num());
		BFSQueue.Empty();
		Visited.Empty();
	}

	// If queue is empty, clean up visited set
	if (BFSQueue.Num() == 0)
	{
		Visited.Empty();
	}

	return ProcessedThisFrame;
}

FIntVector UVoxelWaterPropagation::WorldToVoxelKey(const FVector& WorldPos) const
{
	return FVoxelCoordinates::WorldToVoxel(WorldPos, VoxelSize);
}

FVector UVoxelWaterPropagation::VoxelKeyToWorld(const FIntVector& Key) const
{
	const float HalfVoxel = VoxelSize * 0.5f;
	return FVector(
		Key.X * VoxelSize + HalfVoxel,
		Key.Y * VoxelSize + HalfVoxel,
		Key.Z * VoxelSize + HalfVoxel
	);
}

bool UVoxelWaterPropagation::CanReceiveWater(const FVector& WorldPos) const
{
	if (!ChunkManager.IsValid())
	{
		return false;
	}

	// Must be below water level
	if (WorldPos.Z > WaterLevel)
	{
		return false;
	}

	// Must be air and not already water-flagged
	const FVoxelData Voxel = ChunkManager->GetVoxelAtWorldPosition(WorldPos);
	return Voxel.IsAir() && !Voxel.HasWaterFlag();
}
