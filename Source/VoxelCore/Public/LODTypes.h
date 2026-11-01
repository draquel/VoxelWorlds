// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "LODTypes.generated.h"

/**
 * Query context for LOD calculations.
 *
 * Passed to LOD strategy methods to provide world/camera state.
 * Should be rebuilt each frame before LOD queries.
 *
 * Thread Safety: POD type, safe to copy
 *
 * @see Documentation/LOD_SYSTEM.md
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FLODQueryContext
{
	GENERATED_BODY()

	// ==================== Camera/Viewer State ====================

	/** Current viewer/camera world position */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FVector ViewerPosition = FVector::ZeroVector;

	/** Viewer forward direction (normalized) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FVector ViewerForward = FVector::ForwardVector;

	/**
	 * Planets: how far ViewerPosition (the streaming viewer) lies under the camera along local down, by the descent prefetch
	 * (spherical Phase 6, WM-BP10); 0 = the viewer is the camera. The voxel cut-over reads the camera's altitude from it
	 * (G49: a suspended window resumes only with the camera under the cut-over).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	double ViewerLead = 0.0;

	/**
	 * A far field draws the world beyond the voxel window (UVoxelChunkManager::HasFarField). A heightmap world's voxel
	 * cut-over (world modes review WM-MP2, D38) applies only with one: above it the window would otherwise leave the sky.
	 * Planets suspend their window either way (the sea sphere at least draws the sea).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bFarFieldActive = false;

	/** Field of view in degrees (for frustum calculations) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	float FieldOfView = 90.0f;

	/** Frustum planes for culling (optional, 6 planes) */
	UPROPERTY()
	TArray<FPlane> FrustumPlanes;

	// ==================== World State ====================

	/** World origin for coordinate calculations */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FVector WorldOrigin = FVector::ZeroVector;

	/** Current world generation mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	EWorldMode WorldMode = EWorldMode::InfinitePlane;

	// ==================== Performance Budgets ====================

	/** Maximum chunks to unload per frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	int32 MaxChunksToUnloadPerFrame = 8;

	/** Default constructor */
	FLODQueryContext() = default;

	/** Get distance from viewer to world position */
	FORCEINLINE float GetDistanceToViewer(const FVector& WorldPos) const
	{
		return FVector::Dist(ViewerPosition, WorldPos);
	}

	/** Check if position is in front of viewer */
	FORCEINLINE bool IsInFrontOfViewer(const FVector& WorldPos) const
	{
		const FVector ToPos = (WorldPos - ViewerPosition).GetSafeNormal();
		return FVector::DotProduct(ViewerForward, ToPos) > 0.0f;
	}
};

/**
 * LOD request for a single chunk.
 *
 * Contains chunk identification and computed LOD values.
 * Used in streaming queues and LOD strategy results.
 *
 * Thread Safety: POD type, safe to copy
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FChunkLODRequest
{
	GENERATED_BODY()

	/** Chunk position in chunk coordinate space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** Computed LOD level (0 = finest) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	int32 LODLevel = 0;

	/** Load/update priority (higher = more important) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	float Priority = 0.0f;

	/** Default constructor */
	FChunkLODRequest() = default;

	/** Construct with values */
	FChunkLODRequest(const FIntVector& InChunkCoord, int32 InLODLevel, float InPriority = 0.0f)
		: ChunkCoord(InChunkCoord)
		, LODLevel(InLODLevel)
		, Priority(InPriority)
	{
	}

	/** Comparison for priority sorting (ascending — lowest priority first, highest at back for O(1) pop) */
	bool operator<(const FChunkLODRequest& Other) const
	{
		return Priority < Other.Priority;
	}
};

/**
 * Configuration for a single LOD distance band.
 *
 * Defines the distance range and settings for one LOD level.
 * Used by FDistanceBandLODStrategy.
 *
 * @see Documentation/LOD_SYSTEM.md
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FLODBand
{
	GENERATED_BODY()

	/** Minimum distance from viewer for this band (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0"))
	float MinDistance = 0.0f;

	/** Maximum distance from viewer for this band (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0"))
	float MaxDistance = 1000.0f;

	/** LOD level for this band (0 = finest) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0", ClampMax = "7"))
	int32 LODLevel = 0;

	/** Voxel sampling stride (1 = full detail, 2 = half, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "1"))
	int32 VoxelStride = 1;

	/** Chunk size for this LOD (voxels per edge) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "8"))
	int32 ChunkSize = VOXEL_DEFAULT_CHUNK_SIZE;

	/** Default constructor */
	FLODBand() = default;

	/** Construct with values */
	FLODBand(float InMinDist, float InMaxDist, int32 InLOD, int32 InStride = 1, int32 InChunkSize = VOXEL_DEFAULT_CHUNK_SIZE)
		: MinDistance(InMinDist)
		, MaxDistance(InMaxDist)
		, LODLevel(InLOD)
		, VoxelStride(InStride)
		, ChunkSize(InChunkSize)
	{
	}

	/** Check if distance falls within this band */
	FORCEINLINE bool ContainsDistance(float Distance) const
	{
		return Distance >= MinDistance && Distance < MaxDistance;
	}

	/** Get the world size covered by one chunk at this LOD */
	FORCEINLINE float GetChunkWorldSize(float VoxelSize) const
	{
		return ChunkSize * VoxelStride * VoxelSize;
	}
};

/**
 * Result of an LOD transition check.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FLODTransitionInfo
{
	GENERATED_BODY()

	/** Current LOD level */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	int32 CurrentLOD = 0;

	/** Target LOD level (may differ during transition) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	int32 TargetLOD = 0;

	/** Transition progress (0 = current, 1 = target) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	float TransitionProgress = 0.0f;

	/** Whether a transition is in progress */
	FORCEINLINE bool IsTransitioning() const
	{
		return CurrentLOD != TargetLOD && TransitionProgress > 0.0f && TransitionProgress < 1.0f;
	}
};
