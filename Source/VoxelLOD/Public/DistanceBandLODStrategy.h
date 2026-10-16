// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelLODStrategy.h"
#include "LODTypes.h"
#include "VoxelCoreTypes.h"

class IVoxelWorldMode;

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

	/** With LOD on, GetLODForChunk; with it off, the raw band for the chunk's distance. */
	virtual int32 GetDistanceBandLOD(
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

	/** Set LOD bands configuration. The next Update rebuilds the balanced LOD cache. */
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

	/** Set the maximum view distance for chunk loading. The next Update rebuilds the balanced LOD cache. */
	void SetViewDistance(float Distance) { MaxViewDistance = Distance; bBalancedLODCacheDirty = true; }
	float GetViewDistance() const { return MaxViewDistance; }

	/**
	 * Viewer travel (uu) since the last balanced-LOD cache rebuild that triggers a rebuild by itself. Matches
	 * FVoxelLODEvalSchedule::MoveThreshold, the chunk manager's trigger for re-evaluating LOD levels, so the
	 * cache is never staler than the evaluation that reads it.
	 */
	static constexpr float BalancedLODCacheMoveThreshold = 100.0f;

	/** @return How many times the balanced LOD cache has been rebuilt (diagnostics and tests). */
	int32 GetBalancedLODCacheRebuildCount() const { return BalancedLODCacheRebuildCount; }

	/** Unload horizon: chunks beyond MaxViewDistance * UnloadDistanceMultiplier are out of range. */
	virtual float GetUnloadDistance() const override { return MaxViewDistance * UnloadDistanceMultiplier; }

	/** Load horizon: the configured view distance (chunks are requested within it). */
	virtual float GetLoadDistance() const override { return MaxViewDistance; }

	/**
	 * Horizontal distance in heightmap worlds (InfinitePlane, IslandBowl): the horizons are a cylinder, and each column's
	 * vertical extent is its window (GetColumnWindow), so the viewer's altitude never removes the ground (D8). 3D on a
	 * planet. LOD levels use 3D distance in every mode (GetDistanceToViewer).
	 */
	virtual float GetHorizonDistance(const FVector& ChunkCenter, const FVector& ViewerPosition) const override;

	/** The viewer's layer ± this many layers is streamed in columns near the viewer (heightmap worlds, D8). */
	static constexpr int32 NearWindowLayers = 2;

	/**
	 * Columns within this horizontal distance of the viewer add the viewer's layer ± NearWindowLayers to their surface
	 * slab: the first LOD band's outer edge plus two chunk widths (the margin keeps the extra layers from churning while
	 * the viewer wanders around the band edge). Within it they keep everything loaded in the height range, so a viewer
	 * moving up or down a layer unloads nothing nearby.
	 */
	float GetNearWindowRadius() const { return NearWindowRadius; }

	/**
	 * One column's vertical window, as absolute chunk layers (a range is empty when Min > Max). Heightmap worlds (D8):
	 * the column's surface slab, plus the viewer's layer ± NearWindowLayers in a column near the viewer; both clipped to
	 * the terrain's height bounds. Planet: the viewer's layer + MinVerticalChunks..MaxVerticalChunks (until Phase 1
	 * gives it a shell around the surface, SP-C1).
	 */
	struct FColumnWindow
	{
		int32 SlabMinZ = 0;
		int32 SlabMaxZ = -1;
		int32 NearMinZ = 0;
		int32 NearMaxZ = -1;

		bool Contains(int32 Z) const
		{
			return (Z >= SlabMinZ && Z <= SlabMaxZ) || (Z >= NearMinZ && Z <= NearMaxZ);
		}

		/** Calls Visit(Z) once for every layer in the window, slab first. */
		template <typename FVisit>
		void ForEachLayer(FVisit&& Visit) const
		{
			for (int32 Z = SlabMinZ; Z <= SlabMaxZ; ++Z)
			{
				Visit(Z);
			}
			for (int32 Z = NearMinZ; Z <= NearMaxZ; ++Z)
			{
				if (Z < SlabMinZ || Z > SlabMaxZ)
				{
					Visit(Z);
				}
			}
		}
	};

	/** Which window GetColumnWindow builds. */
	enum class EWindowUse : uint8
	{
		/** Chunks to stream in: the slab, plus the viewer's layer ± NearWindowLayers within GetNearWindowRadius. */
		Load,
		/**
		 * Chunks to keep (the unload rule, and the balanced-LOD cache's volume): the slab, plus the whole height range
		 * within GetNearWindowRadius. Wider than Load vertically, so nothing the near window loaded unloads when the
		 * viewer changes layer; the same radius, so the extra layers unload as soon as their column leaves it.
		 */
		Keep,
	};

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
	 * LOD distance from the viewer to a position: 3D in every world mode, so deep and high chunks fall into farther
	 * bands and compress (world modes review WM-S2; islands measured 2D until then, KI13). Whether a chunk is in range
	 * at all is GetHorizonDistance.
	 */
	float GetDistanceToViewer(
		const FVector& Position,
		const FLODQueryContext& Context
	) const;

	/** Whether the vertical window comes from the terrain (InfinitePlane, IslandBowl) rather than the viewer (planet). */
	bool IsHeightmapWorld() const { return WorldMode != EWorldMode::SphericalPlanet; }

	/**
	 * The vertical window of chunk column (ChunkX, ChunkY) for this viewer.
	 * @param ChunkX Column X, chunk coordinates
	 * @param ChunkY Column Y, chunk coordinates
	 * @param Context Query context (viewer position, world origin)
	 * @param Use Load (stream in) or Keep (retain)
	 * @return The column's window, absolute layers
	 */
	FColumnWindow GetColumnWindow(int32 ChunkX, int32 ChunkY, const FLODQueryContext& Context, EWindowUse Use) const;

	/**
	 * The chunk layers whose chunks overlap the world-Z interval [Lo, Hi] (touching counts).
	 * @param Lo Interval bottom, world Z
	 * @param Hi Interval top, world Z
	 * @param OriginZ World origin Z (layer 0's bottom)
	 * @param OutMin Lowest such layer
	 * @param OutMax Highest such layer (below OutMin when none)
	 */
	void GetLayersOverlapping(double Lo, double Hi, double OriginZ, int32& OutMin, int32& OutMax) const;

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
	 * committed band's edge by the applicable hysteresis margin (chunk-widths):
	 * RefineHysteresisFraction (eager) when moving closer, CoarsenHysteresisFraction
	 * (damped) when moving away. Multi-level changes (e.g. a teleport) are accepted
	 * immediately. First-seen chunks (no committed value) take the raw LOD.
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
	 * than MaxNeighborLODDelta). Called from Update() when ShouldRebuildBalancedLODCache says so.
	 */
	void RebuildBalancedLODCache(const FLODQueryContext& Context);

	/**
	 * Whether Update must rebuild the balanced LOD cache (WORLD_MODES_REVIEW_PLAN.md G12). The cache is a
	 * function of the viewer position, the world origin, the configuration and the balance cvars, so a
	 * still viewer would rebuild the same map every tick (1.3-1.9 ms on the demo). It rebuilds when the
	 * viewer enters another chunk or moves BalancedLODCacheMoveThreshold, when the origin or a balance
	 * cvar changes, and after Initialize, SetLODBands or SetViewDistance.
	 * @param Context This tick's query context.
	 * @return True when the cached map no longer matches Context.
	 */
	bool ShouldRebuildBalancedLODCache(const FLODQueryContext& Context) const;

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
	 * Get color for LOD level (for debug visualization).
	 */
	FColor GetLODDebugColor(int32 LODLevel) const;

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
	 * Hysteresis deadbands in chunk-width units, applied ASYMMETRICALLY: a small refine
	 * margin (eager — detail appears as you approach) and a larger coarsen margin (damped —
	 * terrain you just left doesn't flicker). The asymmetry gives responsive upgrades
	 * without reintroducing band-edge flip/churn. Read from UVoxelWorldConfiguration in
	 * Initialize; overridable via -VoxelLODRefineHyst / -VoxelLODCoarsenHyst.
	 */
	float RefineHysteresisFraction = 0.25f;
	float CoarsenHysteresisFraction = 0.5f;

	/**
	 * Balanced LOD per chunk, recomputed when the viewer moves (ShouldRebuildBalancedLODCache).
	 * GetLODForChunk and GetVisibleChunks read this so all consumers see the same balanced result.
	 * Empty / missing entries fall back to the raw distance-band LOD.
	 */
	TMap<FIntVector, int32> BalancedLODCache;

	/** LOD committed by the last rebuild per chunk; input to the next rebuild's hysteresis. */
	TMap<FIntVector, int32> CommittedLODCache;

	/** What the last rebuild was computed for; ShouldRebuildBalancedLODCache compares against it. */
	FVector LastRebuildViewerPosition = FVector::ZeroVector;
	FIntVector LastRebuildViewerChunk = FIntVector::ZeroValue;
	FVector LastRebuildWorldOrigin = FVector::ZeroVector;
	int32 LastRebuildBalanceCVar = INDEX_NONE;
	int32 LastRebuildDiagonalCVar = INDEX_NONE;

	/** Set by Initialize, SetLODBands and SetViewDistance: the next Update rebuilds whatever the viewer did. */
	bool bBalancedLODCacheDirty = true;

	/** Rebuilds so far (GetBalancedLODCacheRebuildCount). */
	int32 BalancedLODCacheRebuildCount = 0;

	/** Cached voxel size from configuration */
	float VoxelSize = 100.0f;

	/** Cached base chunk size from configuration */
	int32 BaseChunkSize = 32;

	/** Cached world mode from configuration */
	EWorldMode WorldMode = EWorldMode::InfinitePlane;

	/** Maximum view distance for chunk loading (from Configuration->ViewDistance) */
	float MaxViewDistance = 0.0f;

	/**
	 * SphericalPlanet only: the vertical candidate window, in chunk layers relative to the viewer's layer (set in
	 * Initialize). Heightmap worlds take theirs from the terrain (GetColumnWindow, D8).
	 */
	int32 MinVerticalChunks = -2;
	int32 MaxVerticalChunks = 4;

	/** See GetNearWindowRadius. Set in Initialize from the first LOD band; 0 without bands. */
	float NearWindowRadius = 0.0f;

	// ==================== Heightmap Terrain Bounds ====================

	/**
	 * The configured world mode, built by FVoxelWorldModeFactory (the builder generation uses). The terrain height
	 * bounds and the far-band slab's per-column height come from it, so the culls sample the height the world actually
	 * generates (WM-A3), continentalness included. Immutable after Initialize.
	 */
	TSharedPtr<IVoxelWorldMode> TerrainWorldMode;

	/** Minimum terrain height (SeaLevel + BaseHeight - buffer) */
	float TerrainMinHeight = 0.0f;

	/** Maximum terrain height (SeaLevel + BaseHeight + HeightScale + buffer) */
	float TerrainMaxHeight = 0.0f;

	// ==================== Surface Slab (heightmap worlds) ====================

	/**
	 * When enabled (config bFarBandSurfaceSlabCulling, heightmap worlds), every column streams the chunks around its OWN
	 * surface (± SlabSlackChunks, up to the water plane) at every distance, plus the viewer's layer ± NearWindowLayers
	 * near the viewer (D8, WM-S1). The global height range loads 8 layers per column everywhere (demo geometry), which
	 * is what makes long view distances explode; cave interiors and edits deeper than the slab load only near the viewer
	 * (edits are also pinned by the chunk manager). Off: every column streams the whole height range.
	 */
	bool bSurfaceSlabCulling = false;

	/**
	 * Slab slack in chunk widths: covers surface variation between the 5 footprint samples, POI flatten deltas from the
	 * natural height and the meshing skirt, while keeping the per-column cost at 4-5 layers instead of the height
	 * range's 8 (demo geometry).
	 */
	static constexpr float SlabSlackChunks = 1.5f;

	/** Noise params for the per-column analytic surface sample (TerrainWorldMode->GetSurfaceRange). */
	FVoxelNoiseParams SlabNoiseParams;

	/** Water plane: underwater columns extend their slab up to the water surface (the far ocean
	 *  must keep the chunks that mesh its surface, not just its floor). */
	bool bSlabHasWater = false;
	float SlabWaterLevel = 0.0f;

	/**
	 * Column (chunk XY) -> [min, max] analytic surface height over the chunk footprint
	 * (5-point sample: corners + centre). Memoized for the strategy's lifetime — generation
	 * params are immutable per world. Game-thread only.
	 */
	mutable TMap<FIntPoint, FFloatInterval> ColumnSurfaceCache;

	/** The memoized per-column surface range (world Z) over the chunk footprint. */
	void GetColumnSurfaceRange(int32 ChunkX, int32 ChunkY, const FVector& WorldOrigin,
		float& OutMin, float& OutMax) const;

	// ==================== Island Mode Culling ====================
	// Cached from FVoxelWorldModeFactory::MakeIslandParams, the params generation uses (WM-A1).

	/** Island shape type: 0 = Circular, 1 = Rectangle */
	int32 IslandShape = 0;

	/** Total island extent X (IslandRadius + FalloffWidth) for boundary culling */
	float IslandTotalExtentX = 0.0f;

	/** Total island extent Y (IslandSizeY + FalloffWidth) for rectangle culling */
	float IslandTotalExtentY = 0.0f;

	/** Island centre in world space: WorldOrigin + the config's IslandCenterX/Y, where generation puts it */
	FVector2D IslandCenter = FVector2D::ZeroVector;

	// ==================== Spherical Planet Mode Culling ====================
	// Cached from FVoxelWorldModeFactory::MakePlanetParams (SP-A6).

	/** Cached planet radius for horizon calculations */
	float PlanetRadius = 0.0f;

	/** Max terrain height above planet radius (for horizon buffer) */
	float PlanetMaxTerrainHeight = 0.0f;

	/** Max terrain depth below planet radius (for inner shell culling) */
	float PlanetMaxTerrainDepth = 0.0f;

	/** Planet centre in world space (WorldOrigin) */
	FVector PlanetCenter = FVector::ZeroVector;

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
