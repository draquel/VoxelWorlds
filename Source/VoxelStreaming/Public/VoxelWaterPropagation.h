// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
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
 */
UCLASS()
class VOXELSTREAMING_API UVoxelWaterPropagation : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Initialize the propagation system.
	 *
	 * @param InChunkManager The chunk manager to query voxel data from
	 * @param InEditManager The edit manager to apply water flag edits through
	 * @param InWaterLevel The world-space water level (Z coordinate)
	 */
	void Initialize(UVoxelChunkManager* InChunkManager, UVoxelEditManager* InEditManager, float InWaterLevel);

	/**
	 * Called when a chunk is edited. Checks for newly exposed air adjacent
	 * to water and seeds the BFS queue if found.
	 *
	 * @param ChunkCoord The edited chunk coordinate
	 * @param Source Source of the edit (Player, System, etc.)
	 * @param EditCenter World-space center of the edit
	 * @param EditRadius World-space radius of the edit
	 */
	void OnChunkEdited(const FIntVector& ChunkCoord, EEditSource Source, const FVector& EditCenter, float EditRadius);

	/**
	 * Process a bounded number of BFS nodes. Call once per frame from Tick.
	 *
	 * @param MaxVoxelsPerFrame Maximum voxels to process this frame
	 * @return Number of voxels that received water flags this frame
	 */
	int32 ProcessPropagation(int32 MaxVoxelsPerFrame = 512);

	/** Check if there's pending propagation work. */
	bool HasPendingWork() const { return BFSQueue.Num() > 0; }

	/** Maximum voxels to flood per edit trigger (total, not per frame). */
	UPROPERTY(EditDefaultsOnly, Category = "Water")
	int32 MaxPropagationVoxels = 8192;

private:
	/** Chunk manager for voxel queries. */
	UPROPERTY()
	TWeakObjectPtr<UVoxelChunkManager> ChunkManager;

	/** Edit manager for applying water flag changes. */
	UPROPERTY()
	TWeakObjectPtr<UVoxelEditManager> EditManager;

	/** World-space water level. */
	float WaterLevel = 0.f;

	/** BFS queue of world-space voxel positions to process. */
	TArray<FVector> BFSQueue;

	/** Set of visited voxel positions (global voxel coords as FIntVector). */
	TSet<FIntVector> Visited;

	/** Total voxels propagated in the current flood event. */
	int32 TotalPropagated = 0;

	/** Configuration cache */
	int32 ChunkSize = 32;
	float VoxelSize = 100.f;
	FVector WorldOrigin = FVector::ZeroVector;

	/** Convert world position to a unique global voxel key for the visited set. */
	FIntVector WorldToVoxelKey(const FVector& WorldPos) const;

	/** Get world-space center of a voxel from its global key. */
	FVector VoxelKeyToWorld(const FIntVector& Key) const;

	/** Check if a world position is a valid air voxel below water level that can receive water. */
	bool CanReceiveWater(const FVector& WorldPos) const;
};
