// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelSeaSurface.h"
#include "VoxelWaterPropagation.generated.h"

class UVoxelChunkManager;
class UVoxelEditManager;
struct FVoxelData;
enum class EEditSource : uint8;

/**
 * Per-frame water BFS propagation state.
 *
 * Manages bounded flood fill of water flags when terrain edits expose
 * air voxels adjacent to water. Processes a fixed budget of voxels per
 * frame to avoid hitches, giving a visual "water filling" effect.
 *
 * It reads the edit-merged voxels (the generated voxels with the edit layer applied) and sets the flag through the edit
 * layer on the voxel as it is, so a dug voxel floods and keeps the dig's density and material, and a filled one stays solid
 * (world modes review G58). Voxels are keyed by their global index in the chunk grid (relative to WorldOrigin).
 *
 * A flood reads and writes only chunks whose water is in place (UVoxelChunkManager::IsChunkWaterSettled), so it stops at
 * one that is not. When a chunk's water settles, the faces it shares with chunks whose water is in place are scanned for
 * dry air below the sea beside water across the face, an edit on either side: a flood continues into a chunk that loads
 * after it, and a dug pocket floods when the chunk beside it loads with water (G59). The chunk manager's own pull across
 * faces (PropagateWaterFromNeighbors) reads the generated voxels only.
 *
 * The flood's edit operations stay out of the edit manager's undo history (G60): undoing the edit that let the water in
 * puts that edit's voxels back as they were before it, its flood there included, and a redo of it floods them again. Water
 * the flood put beyond the edit's own voxels (a cave or a pocket it opened) stays: nothing drains it.
 */
UCLASS()
class VOXELSTREAMING_API UVoxelWaterPropagation : public UObject
{
	GENERATED_BODY()

public:
	/** What the propagation has done since Initialize, for tests and the log. */
	struct FStats
	{
		/** OnChunkEdited calls whose changed voxels were queued for a seed scan (some below the sea). */
		int64 ScansQueued = 0;
		/** Of those, edits of a chunk whose water is not in place yet (no voxel data, or no mesh since), scanned once it is. */
		int64 ScansDeferred = 0;
		/** Voxels read by the seed scans (each scanned voxel below the sea, and its neighbours when it can take water). */
		int64 ScanReads = 0;
		/** Seeds found: changed voxels that can take water with a water-flagged air neighbour, and the face scans' seeds. */
		int64 Seeds = 0;
		/** Faces queued for a scan when a chunk's water settled: shared with a chunk whose water is in place, one of the two with edits. */
		int64 FaceScansQueued = 0;
		/** Of Seeds, those the face scans found: dry air below the sea beside water across the face, an edit on either voxel. */
		int64 FaceSeeds = 0;
		/** Floods started (seeds found while no flood was running) and finished (the queue emptied or the cap was hit). */
		int64 FloodsStarted = 0;
		int64 FloodsFinished = 0;
		/** Voxels given the water flag. */
		int64 FloodedVoxels = 0;
		/** Water-flag edits the edit manager refused (edit protection). */
		int64 RejectedEdits = 0;
		/** Floods stopped by MaxPropagationVoxels. */
		int64 CapHits = 0;
		/** Game-thread time the seed scans (edit boxes and faces) took, in all and in the slowest frame. */
		double ScanSeconds = 0.0;
		double MaxFrameScanSeconds = 0.0;
	};

	/**
	 * Initialize the propagation system.
	 *
	 * @param InChunkManager The chunk manager to query voxel data from
	 * @param InEditManager The edit manager to apply water flag edits through
	 * @param InWaterLevel The world-space water level (Z coordinate). With a chunk manager the sea comes from its
	 *        configuration instead (FVoxelSeaSurface::FromConfig: on a planet the WaterRadius sphere, SP-B8)
	 */
	void Initialize(UVoxelChunkManager* InChunkManager, UVoxelEditManager* InEditManager, float InWaterLevel);

	/**
	 * Called when a chunk is edited (UVoxelEditManager::OnChunkEdited, through the chunk manager). Queues the voxels the
	 * edit changed in the chunk (UVoxelEditManager::GetBroadcastEditBounds) for a seed scan in the next
	 * ProcessPropagation: every edit's, a brush's as well as an operation of single-voxel edits (a POI's terraform, a
	 * dungeon stamp), an undo, a clear or a load. Outside a broadcast it scans the brush's box, else the whole chunk. The
	 * propagation's own edits are not scanned.
	 *
	 * @param ChunkCoord The edited chunk coordinate
	 * @param Source Source of the edit (Player, System, etc.)
	 * @param EditCenter World-space center of the edit (a brush's; the zero vector otherwise)
	 * @param EditRadius World-space radius of the edit (a brush's; 0 otherwise)
	 */
	void OnChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius);

	/**
	 * Called when a chunk's water is in place (UVoxelChunkManager::IsChunkWaterSettled: its first mesh since its voxel data
	 * arrived was submitted, which pulls the water flags across its faces), once per arrival. An edit that reached the chunk
	 * before (a POI's terraform or a dungeon stamp at stream-in, a save's edits) waited for it; its seed scan is queued now.
	 * So are the faces the chunk shares with chunks whose water is in place, where either chunk has edits (G59): a flood
	 * that stopped at this chunk continues into it, and a dry pocket dug beside it floods from its water.
	 *
	 * @param ChunkCoord The chunk whose water settled
	 */
	void OnChunkWaterSettled(const FIntVector& ChunkCoord);

	/**
	 * Scan the queued edits for seeds (dry air below the sea, edit-merged, with a water-flagged air neighbour) within a
	 * read budget, then process a bounded number of BFS nodes. Call once per frame from Tick. Waits while an edit operation
	 * is in progress (the flood's edits would join it).
	 *
	 * @param MaxVoxelsPerFrame Maximum voxels to process this frame
	 * @return Number of voxels that received water flags this frame
	 */
	int32 ProcessPropagation(int32 MaxVoxelsPerFrame = 512);

	/** Check if there's pending propagation work: edits or chunk faces to scan for seeds, or a flood to continue. */
	bool HasPendingWork() const { return QueueHead < BFSQueue.Num() || PendingScanOrder.Num() > 0 || PendingFaceScans.Num() > 0; }

	/** @return What the propagation has done since Initialize (see FStats) */
	const FStats& GetStats() const { return Stats; }

	/** Maximum voxels to flood per edit trigger (total, not per frame). */
	UPROPERTY(EditDefaultsOnly, Category = "Water")
	int32 MaxPropagationVoxels = 8192;

	/** Maximum voxel reads the seed scans make per frame; a large edit under the sea is scanned over several frames. */
	UPROPERTY(EditDefaultsOnly, Category = "Water")
	int32 MaxScanReadsPerFrame = 32768;

private:
	/** The changed voxels of one chunk still to be scanned for seeds (chunk-local, inclusive), and the scan's position. */
	struct FPendingScan
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Max = FIntVector::ZeroValue;
		int32 Next = 0;
	};

	/** A face a chunk shares with its neighbour (GWaterNeighbourOffsets[Face]) still to be scanned, and the scan's position. */
	struct FPendingFaceScan
	{
		FIntVector ChunkCoord = FIntVector::ZeroValue;
		int32 Face = 0;
		int32 Next = 0;
	};

	/** Chunk manager for voxel queries. */
	UPROPERTY()
	TWeakObjectPtr<UVoxelChunkManager> ChunkManager;

	/** Edit manager for applying water flag changes. */
	UPROPERTY()
	TWeakObjectPtr<UVoxelEditManager> EditManager;

	/** The sea water spreads below: the WaterLevel plane, or a planet's WaterRadius sphere (SP-B8). */
	FVoxelSeaSurface Sea;

	/** BFS queue of global voxel keys; entries before QueueHead are done. */
	TArray<FIntVector> BFSQueue;

	/** The next BFS queue entry to process. */
	int32 QueueHead = 0;

	/** Set of visited voxels (global voxel keys). */
	TSet<FIntVector> Visited;

	/** Chunks whose edits wait for a seed scan, by chunk, and the order they came in. */
	TMap<FIntVector, FPendingScan> PendingScans;
	TArray<FIntVector> PendingScanOrder;

	/** Edits of chunks whose water is not in place yet, waiting for it (OnChunkWaterSettled). */
	TMap<FIntVector, FPendingScan> DeferredScans;

	/** Faces of chunks whose water settled, waiting for a scan (in the order they came in). */
	TArray<FPendingFaceScan> PendingFaceScans;

	/** Total voxels propagated in the current flood event. */
	int32 TotalPropagated = 0;

	/** Seeds of the current flood event, and whether one is running (for the log and FStats). */
	int32 EventSeeds = 0;
	int32 EventFlooded = 0;
	bool bEventRunning = false;

	/** True while the propagation applies its own edits: their broadcasts are not scanned. */
	bool bApplyingWater = false;

	FStats Stats;

	/** Configuration cache */
	int32 ChunkSize = 32;
	float VoxelSize = 100.f;
	FVector WorldOrigin = FVector::ZeroVector;

	/**
	 * The world-space centre of a voxel's cell, the position its edits are applied at and read from (the edit manager and
	 * the chunk manager map a position to the voxel by flooring it relative to WorldOrigin).
	 */
	FVector VoxelKeyToWorld(const FIntVector& Key) const;

	/** The chunk holding a voxel. */
	FIntVector VoxelKeyToChunk(const FIntVector& Key) const;

	/**
	 * Whether a voxel is at or below the sea as generation's fill pass tests it: at its sample point (the cell's min
	 * corner) in world space, against the WaterLevel plane or a planet's sphere (G19).
	 */
	bool IsBelowSea(const FIntVector& Key) const;

	/**
	 * Read a voxel edit-merged (UVoxelChunkManager::GetEditMergedVoxelAtWorldPosition).
	 *
	 * @param Key Global voxel key
	 * @param OutVoxel Receives the voxel
	 * @return False when its chunk's water is not in place (UVoxelChunkManager::IsChunkWaterSettled: not loaded, or no mesh
	 *         since its data arrived, so a chunk under the sea still lacks its flags): nothing is known there, and nothing
	 *         may be written
	 */
	bool ReadVoxel(const FIntVector& Key, FVoxelData& OutVoxel) const;

	/** Whether a voxel can receive water: its chunk's water is in place, it is below the sea, and it reads as air without the flag. */
	bool CanReceiveWater(const FIntVector& Key) const;

	/** Whether a voxel is water: it reads as water-flagged air (a filled voxel keeps the flag but is solid). */
	bool IsWaterSource(const FIntVector& Key) const;

	/** Queue a chunk's changed voxels for a seed scan, joining a box already queued for the chunk. */
	void QueueScan(const FIntVector& ChunkCoord, const FIntVector& LocalMin, const FIntVector& LocalMax);

	/** Keep a chunk's changed voxels until its water is in place, joining a box already kept for it. */
	void DeferScan(const FIntVector& ChunkCoord, const FIntVector& LocalMin, const FIntVector& LocalMax);

	/**
	 * Whether any voxel of the two layers a face joins (the chunk's along it and its neighbour's) is below the sea.
	 *
	 * @param ChunkCoord The chunk
	 * @param Face Index into the 6 face offsets
	 * @return True if the face can hold a seed
	 */
	bool FaceReachesBelowSea(const FIntVector& ChunkCoord, int32 Face) const;

	/**
	 * Scan queued edits, then queued faces, for seeds, at most MaxReads voxel reads.
	 *
	 * @return Seeds found
	 */
	int32 ScanPendingEdits(int32 MaxReads);

	/**
	 * Scan queued faces for seeds: each pair of voxels across a face where either has an edit, one is water-flagged air and
	 * the other dry air below the sea (the seed). Each pair below the sea counts a read, and each voxel read edit-merged.
	 *
	 * @param MaxReads Reads to stop at
	 * @param Reads The reads made so far this frame; incremented
	 * @return Seeds found
	 */
	int32 ScanPendingFaces(int32 MaxReads, int32& Reads);

	/** Add a seed to the flood (starting a flood event when none runs). */
	void AddSeed(const FIntVector& Key);

	/** End the current flood event: log it and clear the visited set. */
	void FinishFlood(bool bCapped);
};
