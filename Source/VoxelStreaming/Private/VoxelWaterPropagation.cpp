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

namespace
{
	/** 6-connected neighbour offsets. */
	const FIntVector GWaterNeighbourOffsets[6] = {
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0),
		FIntVector(0, 0, 1), FIntVector(0, 0, -1)
	};
}

void UVoxelWaterPropagation::Initialize(UVoxelChunkManager* InChunkManager, UVoxelEditManager* InEditManager, float InWaterLevel)
{
	ChunkManager = InChunkManager;
	EditManager = InEditManager;
	Sea = FVoxelSeaSurface::MakePlane(InWaterLevel);

	if (InChunkManager)
	{
		if (UVoxelWorldConfiguration* Config = InChunkManager->GetConfiguration())
		{
			ChunkSize = Config->ChunkSize;
			VoxelSize = Config->VoxelSize;
			WorldOrigin = Config->WorldOrigin;
			Sea = FVoxelSeaSurface::FromConfig(*Config);
			Sea.bEnabled = true; // created only when water is on
		}
	}

	if (Sea.bRadial)
	{
		UE_LOG(LogVoxelWaterPropagation, Log, TEXT("Water propagation initialized (sea radius %.0f around (%.0f, %.0f, %.0f), ChunkSize=%d, VoxelSize=%.0f)"),
			Sea.Radius, Sea.Center.X, Sea.Center.Y, Sea.Center.Z, ChunkSize, VoxelSize);
	}
	else
	{
		UE_LOG(LogVoxelWaterPropagation, Log, TEXT("Water propagation initialized (WaterLevel=%.0f, ChunkSize=%d, VoxelSize=%.0f)"),
			Sea.PlaneZ, ChunkSize, VoxelSize);
	}
}

void UVoxelWaterPropagation::OnChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius)
{
	// The flood's own batches change only voxels it gave the flag; their neighbours are its frontier already. Scanning them
	// would also restart its budget (seeds start a new count), so a flood held by MaxPropagationVoxels would never stop.
	if (bApplyingWater || !ChunkManager.IsValid() || ChunkSize <= 0 || VoxelSize <= 0.0f)
	{
		return;
	}

	// Seeds can only be voxels the edit changed: an unchanged voxel's neighbours are as they were, and edits make no water
	// (G58). The edit manager reports them while it broadcasts, for every edit: a brush's voxels in this chunk, an operation
	// of single-voxel edits (a POI's terraform, a dungeon stamp: radius 0 since G53), an undo, a clear or a load. Outside a
	// broadcast, the brush's box, else the whole chunk.
	FIntVector Min(0), Max(ChunkSize - 1);
	if (!(EditManager.IsValid() && EditManager->GetBroadcastEditBounds(Min, Max)) && EditRadius > 0.0f)
	{
		const FVector Local = (EditCenter - WorldOrigin) / VoxelSize - FVector(ChunkCoord * ChunkSize);
		const double Reach = EditRadius / VoxelSize + 1.0;
		Min = FIntVector(FMath::FloorToInt32(Local.X - Reach), FMath::FloorToInt32(Local.Y - Reach), FMath::FloorToInt32(Local.Z - Reach));
		Max = FIntVector(FMath::FloorToInt32(Local.X + Reach), FMath::FloorToInt32(Local.Y + Reach), FMath::FloorToInt32(Local.Z + Reach));
	}
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Min[Axis] = FMath::Clamp(Min[Axis], 0, ChunkSize - 1);
		Max[Axis] = FMath::Clamp(Max[Axis], 0, ChunkSize - 1);
	}

	// Only voxels below the sea can take water. The plane clips the box's top; a sphere drops a box wholly above it.
	const FIntVector Base = ChunkCoord * ChunkSize;
	if (!Sea.bRadial)
	{
		const int32 TopBelow = FMath::FloorToInt32(Sea.PlaneZ / VoxelSize) - Base.Z;
		Max.Z = FMath::Min(Max.Z, TopBelow);
	}
	else
	{
		const FBox Box(WorldOrigin + FVector(Base + Min) * VoxelSize, WorldOrigin + FVector(Base + Max) * VoxelSize);
		if (Sea.IsAbove(Box))
		{
			return;
		}
	}
	if (Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
	{
		return;
	}

	++Stats.ScansQueued;
	if (!ChunkManager->IsChunkWaterSettled(ChunkCoord))
	{
		// Most system edits land before their chunks load (a POI's terraform, a dungeon stamp at stream-in), and a save's
		// edits before any; and a chunk wholly under a heightmap sea has no water flags until its first mesh pulls them from
		// its neighbours. Nothing can be read there yet: scanned once its water is in place.
		++Stats.ScansDeferred;
		DeferScan(ChunkCoord, Min, Max);
		return;
	}
	QueueScan(ChunkCoord, Min, Max);
}

void UVoxelWaterPropagation::OnChunkWaterSettled(const FIntVector& ChunkCoord)
{
	FPendingScan Deferred;
	if (DeferredScans.RemoveAndCopyValue(ChunkCoord, Deferred))
	{
		QueueScan(ChunkCoord, Deferred.Min, Deferred.Max);
	}
}

namespace
{
	/** Grow a pending scan's box to hold another; a scan under way starts over (its seeds are visited; the rest reads the same). */
	void JoinScanBox(FIntVector& Min, FIntVector& Max, int32& Next, const FIntVector& OtherMin, const FIntVector& OtherMax)
	{
		Min = FIntVector(FMath::Min(Min.X, OtherMin.X), FMath::Min(Min.Y, OtherMin.Y), FMath::Min(Min.Z, OtherMin.Z));
		Max = FIntVector(FMath::Max(Max.X, OtherMax.X), FMath::Max(Max.Y, OtherMax.Y), FMath::Max(Max.Z, OtherMax.Z));
		Next = 0;
	}
}

void UVoxelWaterPropagation::QueueScan(const FIntVector& ChunkCoord, const FIntVector& LocalMin, const FIntVector& LocalMax)
{
	// One box per chunk: a held brush or a batch broadcasting the chunk again joins it.
	if (FPendingScan* Existing = PendingScans.Find(ChunkCoord))
	{
		JoinScanBox(Existing->Min, Existing->Max, Existing->Next, LocalMin, LocalMax);
		return;
	}
	FPendingScan& Scan = PendingScans.Add(ChunkCoord);
	Scan.Min = LocalMin;
	Scan.Max = LocalMax;
	PendingScanOrder.Add(ChunkCoord);
}

void UVoxelWaterPropagation::DeferScan(const FIntVector& ChunkCoord, const FIntVector& LocalMin, const FIntVector& LocalMax)
{
	if (FPendingScan* Existing = DeferredScans.Find(ChunkCoord))
	{
		JoinScanBox(Existing->Min, Existing->Max, Existing->Next, LocalMin, LocalMax);
		return;
	}
	FPendingScan& Scan = DeferredScans.Add(ChunkCoord);
	Scan.Min = LocalMin;
	Scan.Max = LocalMax;
}

int32 UVoxelWaterPropagation::ScanPendingEdits(int32 MaxReads)
{
	int32 Reads = 0;
	int32 SeedsFound = 0;
	while (PendingScanOrder.Num() > 0 && Reads < MaxReads)
	{
		const FIntVector ChunkCoord = PendingScanOrder[0];
		FPendingScan* Scan = PendingScans.Find(ChunkCoord);
		if (!Scan)
		{
			PendingScanOrder.RemoveAt(0);
			continue;
		}
		if (!ChunkManager->IsChunkWaterSettled(ChunkCoord))
		{
			// Unloaded (or regenerated) since the edit: it waits again.
			DeferScan(ChunkCoord, Scan->Min, Scan->Max);
			PendingScans.Remove(ChunkCoord);
			PendingScanOrder.RemoveAt(0);
			continue;
		}

		const FIntVector Size = Scan->Max - Scan->Min + FIntVector(1);
		const int32 Count = Size.X * Size.Y * Size.Z;
		const FIntVector Base = ChunkCoord * ChunkSize + Scan->Min;
		while (Scan->Next < Count && Reads < MaxReads)
		{
			const int32 i = Scan->Next++;
			const FIntVector Key = Base + FIntVector(i % Size.X, (i / Size.X) % Size.Y, i / (Size.X * Size.Y));
			if (!IsBelowSea(Key))
			{
				continue;
			}

			// The voxel as it is now, edits included: a dug voxel reads as air (G58).
			++Reads;
			if (!CanReceiveWater(Key))
			{
				continue;
			}
			for (const FIntVector& Offset : GWaterNeighbourOffsets)
			{
				++Reads;
				if (IsWaterSource(Key + Offset))
				{
					if (!Visited.Contains(Key))
					{
						AddSeed(Key);
						++SeedsFound;
					}
					break;
				}
			}
		}

		if (Scan->Next >= Count)
		{
			PendingScans.Remove(ChunkCoord);
			PendingScanOrder.RemoveAt(0);
		}
	}
	Stats.ScanReads += Reads;

	if (SeedsFound > 0)
	{
		// A new seed restarts the flood's budget, as an edit did before.
		TotalPropagated = 0;
		UE_LOG(LogVoxelWaterPropagation, Log, TEXT("Water propagation seeded with %d voxels"), SeedsFound);
	}
	return SeedsFound;
}

void UVoxelWaterPropagation::AddSeed(const FIntVector& Key)
{
	if (!bEventRunning)
	{
		bEventRunning = true;
		EventSeeds = 0;
		EventFlooded = 0;
		++Stats.FloodsStarted;
	}
	Visited.Add(Key);
	BFSQueue.Add(Key);
	++EventSeeds;
	++Stats.Seeds;
}

void UVoxelWaterPropagation::FinishFlood(bool bCapped)
{
	if (bEventRunning)
	{
		++Stats.FloodsFinished;
		UE_LOG(LogVoxelWaterPropagation, Log, TEXT("Water propagation %s: %d voxels flooded from %d seeds"),
			bCapped ? TEXT("stopped at its limit") : TEXT("done"), EventFlooded, EventSeeds);
	}
	bEventRunning = false;
	BFSQueue.Reset();
	QueueHead = 0;
	Visited.Empty();
}

int32 UVoxelWaterPropagation::ProcessPropagation(int32 MaxVoxelsPerFrame)
{
	if (!HasPendingWork() || !ChunkManager.IsValid() || !EditManager.IsValid())
	{
		return 0;
	}

	// An operation someone holds open would take the flood's edits as its own (and BeginEditOperation would end it).
	if (EditManager->IsEditOperationInProgress())
	{
		return 0;
	}

	ScanPendingEdits(MaxScanReadsPerFrame);
	if (QueueHead >= BFSQueue.Num())
	{
		if (bEventRunning)
		{
			FinishFlood(false);
		}
		return 0;
	}

	int32 ProcessedThisFrame = 0;

	// Group this frame's water flags into one operation, as a system edit; the source the caller had is kept.
	const EEditSource PreviousSource = EditManager->GetEditSource();
	EditManager->SetEditSource(EEditSource::System);
	EditManager->BeginEditOperation(TEXT("Water Propagation"));
	bApplyingWater = true;

	while (QueueHead < BFSQueue.Num() && ProcessedThisFrame < MaxVoxelsPerFrame && TotalPropagated < MaxPropagationVoxels)
	{
		const FIntVector Key = BFSQueue[QueueHead++];

		// The voxel may have changed since it was queued (an edit, or its chunk unloaded).
		FVoxelData WaterVoxel;
		if (!IsBelowSea(Key) || !ReadVoxel(Key, WaterVoxel) || !WaterVoxel.IsAir() || WaterVoxel.HasWaterFlag())
		{
			continue;
		}

		// The voxel as it is, edits included, with the flag: a dig keeps its density and material, and no voxel goes back
		// to its generated value (G58). The edit manager writes it to the edit layer, which the chunk's re-mesh and its
		// save read; its broadcast at the operation's end marks the chunks dirty.
		WaterVoxel.SetWaterFlag(true);
		if (!EditManager->ApplyEdit(VoxelKeyToWorld(Key), WaterVoxel, EEditMode::Set))
		{
			// Edit protection refused it: water does not pass through it either.
			++Stats.RejectedEdits;
			continue;
		}

		++ProcessedThisFrame;
		++TotalPropagated;
		++EventFlooded;
		++Stats.FloodedVoxels;

		// Enqueue 6-connected neighbours
		for (const FIntVector& Offset : GWaterNeighbourOffsets)
		{
			const FIntVector NeighborKey = Key + Offset;
			if (!Visited.Contains(NeighborKey) && CanReceiveWater(NeighborKey))
			{
				Visited.Add(NeighborKey);
				BFSQueue.Add(NeighborKey);
			}
		}
	}

	EditManager->EndEditOperation();
	bApplyingWater = false;
	EditManager->SetEditSource(PreviousSource);

	if (ProcessedThisFrame > 0)
	{
		UE_LOG(LogVoxelWaterPropagation, Verbose, TEXT("Water propagation: %d voxels this frame, %d total, %d remaining in queue"),
			ProcessedThisFrame, TotalPropagated, BFSQueue.Num() - QueueHead);
	}

	if (QueueHead >= BFSQueue.Num())
	{
		FinishFlood(false);
	}
	else if (TotalPropagated >= MaxPropagationVoxels)
	{
		UE_LOG(LogVoxelWaterPropagation, Log, TEXT("Water propagation reached max limit (%d voxels), clearing %d remaining"),
			MaxPropagationVoxels, BFSQueue.Num() - QueueHead);
		++Stats.CapHits;
		FinishFlood(true);
	}
	else if (QueueHead > 4096)
	{
		// Drop the processed front so the queue does not grow with every voxel a long flood visits.
		BFSQueue.RemoveAt(0, QueueHead, EAllowShrinking::No);
		QueueHead = 0;
	}

	return ProcessedThisFrame;
}

FVector UVoxelWaterPropagation::VoxelKeyToWorld(const FIntVector& Key) const
{
	return WorldOrigin + (FVector(Key) + FVector(0.5)) * VoxelSize;
}

FIntVector UVoxelWaterPropagation::VoxelKeyToChunk(const FIntVector& Key) const
{
	return FIntVector(
		FMath::FloorToInt32(double(Key.X) / ChunkSize),
		FMath::FloorToInt32(double(Key.Y) / ChunkSize),
		FMath::FloorToInt32(double(Key.Z) / ChunkSize));
}

bool UVoxelWaterPropagation::IsBelowSea(const FIntVector& Key) const
{
	const FVector GridPos = FVector(Key) * VoxelSize;
	return Sea.IsBelow(Sea.bRadial ? WorldOrigin + GridPos : GridPos);
}

bool UVoxelWaterPropagation::ReadVoxel(const FIntVector& Key, FVoxelData& OutVoxel) const
{
	if (!ChunkManager.IsValid() || !ChunkManager->IsChunkWaterSettled(VoxelKeyToChunk(Key)))
	{
		return false;
	}
	OutVoxel = ChunkManager->GetEditMergedVoxelAtWorldPosition(VoxelKeyToWorld(Key));
	return true;
}

bool UVoxelWaterPropagation::CanReceiveWater(const FIntVector& Key) const
{
	// Must be below the sea, in a chunk whose water is in place (an unloaded chunk reads as air: water written there would
	// carve its ground away when it loads; one not meshed yet lacks the sea's flags, which the flood would copy into the
	// edit layer), and read as air without the flag.
	FVoxelData Voxel;
	return IsBelowSea(Key) && ReadVoxel(Key, Voxel) && Voxel.IsAir() && !Voxel.HasWaterFlag();
}

bool UVoxelWaterPropagation::IsWaterSource(const FIntVector& Key) const
{
	FVoxelData Voxel;
	return ReadVoxel(Key, Voxel) && Voxel.IsAir() && Voxel.HasWaterFlag();
}
