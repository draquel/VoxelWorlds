// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelCoreTypes.h"
#include "LODTypes.h"
#include "VoxelWorldConfiguration.generated.h"

/**
 * Configuration data asset for a voxel world.
 *
 * Contains all settings needed to initialize a voxel world instance.
 * Create as a Data Asset in the editor for reusable configurations.
 *
 * @see Documentation/ARCHITECTURE.md
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelWorldConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	// ==================== World Settings ====================

	/** World generation mode (Infinite, Spherical, Island) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World")
	EWorldMode WorldMode = EWorldMode::InfinitePlane;

	/** Meshing style (Cubic blocks or Smooth terrain) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World")
	EMeshingMode MeshingMode = EMeshingMode::Cubic;

	/** World origin position */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World")
	FVector WorldOrigin = FVector::ZeroVector;

	/** World radius for spherical/island modes (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World", meta = (ClampMin = "1000", EditCondition = "WorldMode != EWorldMode::InfinitePlane"))
	float WorldRadius = 100000.0f;

	// ==================== Terrain Generation Settings (Infinite Plane) ====================

	/** Sea level height - base elevation of terrain (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain", meta = (EditCondition = "WorldMode == EWorldMode::InfinitePlane"))
	float SeaLevel = 0.0f;

	/** Height scale - multiplier for noise-to-height conversion (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain", meta = (ClampMin = "100", ClampMax = "50000", EditCondition = "WorldMode == EWorldMode::InfinitePlane"))
	float HeightScale = 5000.0f;

	/** Base height - additional offset added to terrain height (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain", meta = (EditCondition = "WorldMode == EWorldMode::InfinitePlane"))
	float BaseHeight = 0.0f;

	// ==================== Voxel Settings ====================

	/** Size of one voxel in world units (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel", meta = (ClampMin = "1", ClampMax = "1000"))
	float VoxelSize = 100.0f;

	/** Number of voxels per chunk edge (typically 32) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel", meta = (ClampMin = "8", ClampMax = "128"))
	int32 ChunkSize = VOXEL_DEFAULT_CHUNK_SIZE;

	/** Random seed for world generation (0 = random) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
	int32 WorldSeed = 0;

	// ==================== Noise Generation Settings ====================

	/** Parameters for noise-based terrain generation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise Generation")
	FVoxelNoiseParams NoiseParams;

	/** Use GPU compute shaders for terrain generation (true) or CPU fallback (false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise Generation")
	bool bUseGPUGeneration = true;

	// ==================== LOD Settings ====================

	/**
	 * Enable Level of Detail system.
	 * When disabled, all chunks render at LOD 0 (full detail).
	 * Useful for debugging LOD seams or when view distance is small.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bEnableLOD = true;

	/** LOD distance bands configuration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "bEnableLOD"))
	TArray<FLODBand> LODBands;

	/** Enable smooth LOD transitions (morphing) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "bEnableLOD"))
	bool bEnableLODMorphing = true;

	/**
	 * Enable LOD seam handling (skirts, Transvoxel, etc.).
	 * When disabled, no seam geometry is generated between LOD levels.
	 * Useful for debugging or when LOD is disabled entirely.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "bEnableLOD"))
	bool bEnableLODSeams = true;

	/** Enable view frustum culling for chunks */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bEnableFrustumCulling = true;

	/** Maximum view distance for chunk loading (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "1000"))
	float ViewDistance = 10000.0f;

	// ==================== Streaming Settings ====================

	/** Maximum chunks to load per frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ClampMax = "32"))
	int32 MaxChunksToLoadPerFrame = 4;

	/** Maximum chunks to unload per frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxChunksToUnloadPerFrame = 8;

	/** Time budget for streaming operations per frame (milliseconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "0.5", ClampMax = "10"))
	float StreamingTimeSliceMS = 2.0f;

	/** Maximum number of chunks to keep loaded */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "100", ClampMax = "10000"))
	int32 MaxLoadedChunks = 2000;

	// ==================== Meshing Settings ====================

	/**
	 * Use greedy meshing to merge adjacent faces with the same material.
	 * Reduces triangle count by 40-60% but requires special material handling for texture atlases.
	 * When disabled, each voxel face is a separate quad with 0-1 UVs (simpler material setup).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meshing")
	bool bUseGreedyMeshing = false;

	/**
	 * Calculate per-vertex ambient occlusion.
	 * Adds subtle shadowing in corners and crevices.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meshing")
	bool bCalculateAO = true;

	/**
	 * UV scale multiplier for texture coordinates.
	 * With greedy meshing: UVs scale with merged quad size (e.g., 5x3 quad = UV 0-5, 0-3).
	 * Without greedy meshing: Each face is 0-1 * UVScale.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meshing", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float UVScale = 1.0f;

	// ==================== Rendering Settings ====================

	/** Use GPU-driven custom vertex factory (true) or PMC fallback (false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bUseGPURenderer = true;

	/** Generate collision meshes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bGenerateCollision = true;

	/** LOD level to use for collision (higher = simpler) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (ClampMin = "0", ClampMax = "4", EditCondition = "bGenerateCollision"))
	int32 CollisionLODLevel = 1;

public:
	UVoxelWorldConfiguration();

	/** Get the world size of a single chunk */
	FORCEINLINE float GetChunkWorldSize() const
	{
		return ChunkSize * VoxelSize;
	}

	/** Get the world size of a single chunk at a specific LOD */
	float GetChunkWorldSizeAtLOD(int32 LODLevel) const;

	/** Get LOD band for a given distance, returns nullptr if beyond all bands */
	const FLODBand* GetLODBandForDistance(float Distance) const;

	/** Get the LOD level for a given distance */
	int32 GetLODLevelForDistance(float Distance) const;

	/** Validate configuration and log any issues */
	bool ValidateConfiguration() const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
