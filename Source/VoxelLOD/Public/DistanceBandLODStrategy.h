// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelLODStrategy.h"
#include "LODTypes.h"
#include "VoxelCoreTypes.h"

/**
 * Distance-based LOD strategy using concentric rings.
 *
 * The default LOD implementation. Uses configurable distance bands
 * around the viewer to determine LOD levels. Simple, predictable,
 * and efficient for most use cases.
 *
 * Features:
 * - Configurable distance bands with LOD levels
 * - Optional LOD morphing for smooth transitions
 * - Optional view frustum culling
 * - Priority boost for chunks in view direction
 *
 * Performance: O(n) for visible chunk enumeration where n = chunks in range
 * Memory: Minimal state (just configuration)
 *
 * @see IVoxelLODStrategy
 * @see Documentation/LOD_SYSTEM.md
 */
class VOXELLOD_API FDistanceBandLODStrategy : public IVoxelLODStrategy
{
public:
	FDistanceBandLODStrategy();
	virtual ~FDistanceBandLODStrategy() = default;

	// ==================== IVoxelLODStrategy Interface ====================

	virtual int32 GetLODForChunk(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const override;

	virtual float GetLODMorphFactor(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const override;

	virtual TArray<FChunkLODRequest> GetVisibleChunks(
		const FLODQueryContext& Context
	) const override;

	virtual void GetChunksToLoad(
		TArray<FChunkLODRequest>& OutLoad,
		const TSet<FIntVector>& LoadedChunks,
		const FLODQueryContext& Context
	) const override;

	virtual void GetChunksToUnload(
		TArray<FIntVector>& OutUnload,
		const TSet<FIntVector>& LoadedChunks,
		const FLODQueryContext& Context
	) const override;

	virtual void Initialize(const UVoxelWorldConfiguration* WorldConfig) override;

	virtual void Update(const FLODQueryContext& Context, float DeltaTime) override;

	virtual float GetChunkPriority(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const override;

	virtual FString GetDebugInfo() const override;

	virtual void DrawDebugVisualization(
		UWorld* World,
		const FLODQueryContext& Context
	) const override;

	// ==================== Configuration ====================

	/** Get the configured LOD bands */
	const TArray<FLODBand>& GetLODBands() const { return LODBands; }

	/** Set LOD bands configuration */
	void SetLODBands(const TArray<FLODBand>& InBands);

	/** Enable/disable LOD morphing */
	void SetMorphingEnabled(bool bEnabled) { bEnableMorphing = bEnabled; }
	bool IsMorphingEnabled() const { return bEnableMorphing; }

	/** Enable/disable view frustum culling */
	void SetFrustumCullingEnabled(bool bEnabled) { bEnableFrustumCulling = bEnabled; }
	bool IsFrustumCullingEnabled() const { return bEnableFrustumCulling; }

	/** Set the unload distance multiplier (chunks beyond MaxDistance * multiplier are unloaded) */
	void SetUnloadDistanceMultiplier(float Multiplier) { UnloadDistanceMultiplier = Multiplier; }
	float GetUnloadDistanceMultiplier() const { return UnloadDistanceMultiplier; }

	/** Set the maximum view distance for chunk loading */
	void SetViewDistance(float Distance) { MaxViewDistance = Distance; }
	float GetViewDistance() const { return MaxViewDistance; }

	/** Unload horizon: chunks beyond MaxViewDistance * UnloadDistanceMultiplier are out of range. */
	virtual float GetUnloadDistance() const override { return MaxViewDistance * UnloadDistanceMultiplier; }

protected:
	// ==================== Internal Helpers ====================

	/**
	 * Convert chunk coordinate to world-space center position.
	 */
	FVector ChunkCoordToWorldCenter(const FIntVector& ChunkCoord) const;

	/**
	 * Convert world position to chunk coordinate.
	 */
	FIntVector WorldPosToChunkCoord(const FVector& WorldPos) const;

	/**
	 * Calculate distance from viewer to position based on world mode.
	 */
	float GetDistanceToViewer(
		const FVector& Position,
		const FLODQueryContext& Context
	) const;

	/**
	 * Find the LOD band containing the given distance.
	 * Returns nullptr if beyond all bands.
	 */
	const FLODBand* FindBandForDistance(float Distance) const;

	/**
	 * Find the band whose LODLevel equals the given level (first match).
	 * Used by hysteresis to look up a committed LOD's distance edges.
	 */
	const FLODBand* FindBandByLOD(int32 LODLevel) const;

	/**
	 * Raw distance-band LOD for a chunk, ignoring balancing/hysteresis.
	 * This is the unmodified band lookup; the public GetLODForChunk layers
	 * the balanced cache on top of it.
	 */
	int32 GetRawLODForDistance(float Distance) const;

	/**
	 * Apply LOD hysteresis to damp churn near band edges.
	 * A single-level change is only accepted once the distance has crossed the
	 * committed band's edge by LODHysteresisChunkFraction chunk-widths; multi-level
	 * changes (e.g. a teleport) are accepted immediately. First-seen chunks (no
	 * committed value) take the raw LOD.
	 *
	 * @param CommittedLOD The LOD committed for this chunk last frame
	 * @param RawLOD The raw distance-band LOD this frame
	 * @param Distance Chunk-center distance to viewer
	 * @return The LOD to commit this frame
	 */
	int32 ApplyLODHysteresis(int32 CommittedLOD, int32 RawLOD, float Distance) const;

	/**
	 * Recompute BalancedLODCache for all candidate chunks: raw band LOD, then
	 * hysteresis, then the 2:1 adjacency balance (no face-neighbor differs by more
	 * than MaxNeighborLODDelta). Called from Update() each frame.
	 */
	void RebuildBalancedLODCache(const FLODQueryContext& Context);

	/**
	 * Check if a chunk is within the view frustum.
	 */
	bool IsChunkInFrustum(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const;

	/**
	 * Calculate load priority for a chunk.
	 * Higher values = higher priority.
	 */
	float CalculatePriority(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const;

	/**
	 * Get the vertical range of chunks to consider.
	 */
	void GetVerticalChunkRange(
		const FLODQueryContext& Context,
		int32& OutMinZ,
		int32& OutMaxZ
	) const;

	/**
	 * Get color for LOD level (for debug visualization).
	 */
	FColor GetLODDebugColor(int32 LODLevel) const;

	/**
	 * Check if chunk should be culled for Infinite Plane mode (outside terrain bounds).
	 * Returns true if chunk should be CULLED (not rendered).
	 */
	bool ShouldCullOutsideTerrainBounds(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const;

	/**
	 * Check if chunk should be culled for Island mode (beyond island boundary).
	 * Returns true if chunk should be CULLED (not rendered).
	 */
	bool ShouldCullIslandBoundary(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const;

	/**
	 * Check if chunk should be culled for Spherical Planet mode (beyond horizon).
	 * Returns true if chunk should be CULLED (not rendered).
	 */
	bool ShouldCullBeyondHorizon(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const;

protected:
	// ==================== Configuration State ====================

	/** LOD bands sorted by distance */
	TArray<FLODBand> LODBands;

	/** Master LOD enable flag - when false, all chunks use LOD 0 */
	bool bEnableLOD = true;

	/** Enable smooth LOD transitions via vertex morphing */
	bool bEnableMorphing = true;

	/** Enable view frustum culling for chunk visibility */
	bool bEnableFrustumCulling = true;

	/** Multiplier for unload distance (relative to max LOD band distance) */
	float UnloadDistanceMultiplier = 1.2f;

	// ==================== LOD Adjacency Balance + Hysteresis (P2-B) ====================

	/**
	 * Maximum LOD-level difference permitted between face-adjacent chunks (the 2:1
	 * balance rule). The mesher's single-level Transvoxel transition cells can only
	 * bridge a one-level gap, so larger gaps are refined (coarser chunks pulled
	 * finer) until every adjacency is within this delta. See
	 * Documentation/LOD_SEAM_INVESTIGATION.md (P2-B).
	 */
	int32 MaxNeighborLODDelta = 1;

	/**
	 * Hysteresis deadband, expressed in chunk-world-size units. A chunk only
	 * changes its committed LOD once its distance crosses the band edge by this
	 * many chunk widths, damping the bistable flip/churn observed at ring edges.
	 */
	float LODHysteresisChunkFraction = 0.5f;

	/**
	 * Balanced LOD per chunk, recomputed every Update(). GetLODForChunk and
	 * GetVisibleChunks read this so all consumers see the same balanced result.
	 * Empty / missing entries fall back to the raw distance-band LOD.
	 */
	TMap<FIntVector, int32> BalancedLODCache;

	/** LOD committed last frame per chunk; input to this frame's hysteresis. */
	TMap<FIntVector, int32> CommittedLODCache;

	/** Cached voxel size from configuration */
	float VoxelSize = 100.0f;

	/** Cached base chunk size from configuration */
	int32 BaseChunkSize = 32;

	/** Cached world mode from configuration */
	EWorldMode WorldMode = EWorldMode::InfinitePlane;

	/** Maximum view distance for chunk loading (from Configuration->ViewDistance) */
	float MaxViewDistance = 0.0f;

	/** Vertical chunk range for infinite plane mode */
	int32 MinVerticalChunks = -2;
	int32 MaxVerticalChunks = 4;

	// ==================== Infinite Plane Mode Culling ====================

	/** Minimum terrain height (SeaLevel + BaseHeight - buffer) */
	float TerrainMinHeight = 0.0f;

	/** Maximum terrain height (SeaLevel + BaseHeight + HeightScale + buffer) */
	float TerrainMaxHeight = 0.0f;

	// ==================== Island Mode Culling ====================

	/** Island shape type: 0 = Circular, 1 = Rectangle */
	int32 IslandShape = 0;

	/** Total island extent X (IslandRadius + FalloffWidth) for boundary culling */
	float IslandTotalExtentX = 0.0f;

	/** Total island extent Y (IslandSizeY + FalloffWidth) for rectangle culling */
	float IslandTotalExtentY = 0.0f;

	/** Island center offset from WorldOrigin */
	FVector2D IslandCenterOffset = FVector2D::ZeroVector;

	// ==================== Spherical Planet Mode Culling ====================

	/** Cached planet radius for horizon calculations */
	float PlanetRadius = 0.0f;

	/** Max terrain height above planet radius (for horizon buffer) */
	float PlanetMaxTerrainHeight = 0.0f;

	/** Max terrain depth below planet radius (for inner shell culling) */
	float PlanetMaxTerrainDepth = 0.0f;

	// ==================== Runtime State ====================

	/** Cached viewer position from last update */
	FVector CachedViewerPosition = FVector::ZeroVector;

	/** Cached viewer chunk coordinate */
	FIntVector CachedViewerChunk = FIntVector::ZeroValue;

	/** Cached world origin from last update */
	FVector CachedWorldOrigin = FVector::ZeroVector;

	/** Whether the strategy has been initialized */
	bool bIsInitialized = false;
};
