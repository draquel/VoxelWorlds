// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IVoxelLODStrategy.h"
#include "LODTypes.h"
#include "VoxelCoreTypes.h"
#include "VoxelCubeSphere.h"
#include "VoxelWorldModeTypes.h"

class IVoxelWorldMode;
class FSphericalPlanetWorldMode;

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
	 * vertical extent is its window (GetColumnWindow), so the viewer's altitude never removes the ground (D8). On a
	 * planet the distance along the ground from the viewer's ground point (GetPlanetGroundDistance, spherical Phase 6,
	 * WM-BP8): the horizons are a cone around the viewer's up-line, and each chunk's radial extent is its window
	 * (IsInPlanetWindow), so a climbing viewer keeps its ground disk. LOD levels use 3D distance in every mode
	 * (GetDistanceToViewer).
	 */
	virtual float GetHorizonDistance(const FVector& ChunkCenter, const FVector& ViewerPosition) const override;

	/**
	 * The distance along the ground between a viewer and a point on a planet (spherical Phase 6, WM-BP8): the arc, on the
	 * sphere through the viewer's ground point, between the viewer's direction from the centre and the point's. A point
	 * straight above or below the viewer is at 0 whatever its altitude, as a heightmap world's horizontal distance is.
	 * @param Centre Planet centre
	 * @param GroundRadius Distance from the centre to the viewer's ground point (the terrain under the viewer)
	 * @param Viewer Viewer position
	 * @param Point Point (a chunk centre)
	 * @return GroundRadius times the angle between the two directions (uu)
	 */
	static double GetPlanetGroundDistance(const FVector& Centre, double GroundRadius, const FVector& Viewer, const FVector& Point);

	/**
	 * A planet viewer's ground point (WM-BP8): the analytic terrain along the viewer's direction from the centre, without
	 * conditioning zones (GetTerrainAltitude), at the radius GetPlanetGroundDistance measures on. Cached for the position
	 * the last Update saw; any other position is computed (one terrain sample). Game thread.
	 * @param ViewerPosition Viewer position
	 * @return Distance from the planet centre to the ground under the viewer (PlanetRadius on a heightmap world)
	 */
	double GetPlanetGroundRadius(const FVector& ViewerPosition) const;

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
	 * the column's surface slab (Slab*), plus the viewer's layer ± NearWindowLayers in a column near the viewer (Near*);
	 * both clipped to the terrain's height bounds. Planet (SP-C1): the column's crossings of the terrain shell
	 * (GetPlanetShellWindow), the one above the planet centre in Slab* and the one below in Near* (or one range when
	 * they meet); a planet streams by chunk, not by column (IsInPlanetWindow), and only enumerates candidates by
	 * crossings like these.
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
	 * A planet column's crossing of the shell (SP-C1): the layers whose chunks meet the shell from ShellWindowInnerRadius
	 * to ShellWindowOuterRadius over the column's footprint. Exact for the footprint: its nearest and farthest points from
	 * the planet's axis bound the crossing, so it covers the shell at every latitude. Not clipped to the viewer's
	 * altitude any more (spherical Phase 6, WM-BP8: the viewer-Z clip lost the ground under a climbing viewer).
	 * @param ChunkX Column X, chunk coordinates
	 * @param ChunkY Column Y, chunk coordinates
	 * @param Context Query context (world origin)
	 * @param OutWindow Receives the crossings: above the centre in Slab*, below it in Near* (or one range in Slab*)
	 */
	void GetPlanetShellWindow(int32 ChunkX, int32 ChunkY, const FLODQueryContext& Context, FColumnWindow& OutWindow) const;

	/**
	 * A column of chunks along a grid axis, crossing a spherical shell around the planet centre: the layers along Axis
	 * whose chunks meet the shell [InnerRadius, OuterRadius] (touching counts). GetPlanetShellWindow is the Z axis with the
	 * window's radii; the planet's candidate enumeration runs along whichever axis is closest to the viewer's local up.
	 * @param Axis Column direction: 0 = X, 1 = Y, 2 = Z
	 * @param ColumnA The column's chunk index along axis (Axis + 1) % 3
	 * @param ColumnB The column's chunk index along axis (Axis + 2) % 3
	 * @param InnerRadius Shell inner radius
	 * @param OuterRadius Shell outer radius
	 * @param WorldOrigin The chunk grid's origin
	 * @param OutWindow Receives the crossings: on the centre's positive side in Slab*, the negative in Near* (or one range)
	 */
	void GetShellCrossing(int32 Axis, int32 ColumnA, int32 ColumnB, double InnerRadius, double OuterRadius,
		const FVector& WorldOrigin, FColumnWindow& OutWindow) const;

	/**
	 * Whether a planet chunk is in the window for this viewer, apart from the horizon distance (spherical Phase 6, WM-BP8;
	 * the heightmap M4 rules on a sphere):
	 * - its radial extent meets the terrain shell (ShellWindowInnerRadius to ShellWindowOuterRadius, the terrain's real
	 *   bounds + ShellSlackChunks);
	 * - and its radial extent meets its slab (GetPlanetChunkSlab: the analytic surface's altitude range over the cap it
	 *   subtends ± SlabSlackChunks; with slab culling off, the whole shell);
	 * - or, within GetNearWindowRadius of the viewer along the ground: Load, the viewer's altitude ± NearWindowLayers chunk
	 *   widths; Keep, anything in the shell within the unload distance of the viewer (3D). As a heightmap world's near
	 *   window streams the viewer's layer ± NearWindowLayers within its height bounds: caves and digging below the slab,
	 *   and the chunks around a viewer above it (a flyer in the shell, a pawn held over the sea), whose terrain waits need
	 *   them cooked.
	 * Edits off the window are pinned by the chunk manager.
	 * @param ChunkCoord Chunk coordinates
	 * @param Context Query context (viewer position, world origin)
	 * @param Use Load (stream in) or Keep (retain)
	 * @return True when the chunk is in the window
	 */
	bool IsInPlanetWindow(const FIntVector& ChunkCoord, const FLODQueryContext& Context, EWindowUse Use) const;

	/** A planet chunk's viewer-independent facts (WM-BP8): its radial extent and its slab, as altitudes above the datum. */
	struct FPlanetChunkSlab
	{
		/** Nearest and farthest point of the chunk box from the planet centre, minus PlanetRadius. */
		float AltitudeMin = 0.0f;
		float AltitudeMax = 0.0f;
		/** The surface's altitude range over the cells the chunk's corners span, ± SlabSlackChunks (the whole shell when off). */
		float SlabMin = 0.0f;
		float SlabMax = 0.0f;
	};

	/**
	 * A planet chunk's radial extent and slab, memoized for the strategy's lifetime (the terrain is immutable per world).
	 * The slab is the union of the per-cell surface ranges (GetPlanetCellSurfaceRange) of the cube-sphere cells the
	 * chunk's eight corner directions span (FVoxelCubeSphereGrid::GatherSpanned), widened by SlabSlackChunks. Game thread.
	 * @param ChunkCoord Chunk coordinates
	 * @return The chunk's altitude extent and slab
	 */
	FPlanetChunkSlab GetPlanetChunkSlab(const FIntVector& ChunkCoord) const;

	/**
	 * One cube-sphere cell's analytic surface altitude range (WM-BP8): GetTerrainAltitude at its four corners and centre,
	 * as a heightmap column's GetSurfaceRange samples five heights. Memoized. Game thread.
	 * @param Cell Atlas coordinate on PlanetSlabGrid
	 * @return Altitude range (uu above the datum)
	 */
	FFloatInterval GetPlanetCellSurfaceRange(const FIntPoint& Cell) const;

	/**
	 * Calls Visit once for every chunk that may be in the planet's window for this viewer (WM-BP8): the chunks crossing the
	 * spherical shell of the window's altitude envelope inside the cone of the load horizon around the viewer's up-line,
	 * plus the near window's cone and envelope. A superset: the caller applies IsInPlanetWindow and the horizon. Columns
	 * run along the grid axis closest to the viewer's up, so each crosses the envelope steeply.
	 * @param Context Query context (viewer position, world origin)
	 * @param Use Load (stream in) or Keep (retain: the near window holds the shell within the unload distance)
	 * @param Visit Called with each candidate's chunk coordinates (no repeats)
	 */
	void ForEachPlanetCandidate(const FLODQueryContext& Context, EWindowUse Use, TFunctionRef<void(const FIntVector&)> Visit) const;

	/**
	 * The surface's altitude envelope over the disk of cells within an angle of a direction (WM-BP8): the union of their
	 * GetPlanetCellSurfaceRange. Memoized for the last direction cell and angle. Game thread.
	 * @param Direction Unit direction from the planet centre (the viewer's up)
	 * @param Angle Angular radius of the disk (radians), margins included
	 * @return Altitude envelope (uu above the datum), without slack
	 */
	FFloatInterval GetPlanetSurfaceEnvelope(const FVector& Direction, double Angle) const;

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
	 * Check if chunk should be culled for Island mode (beyond island boundary): its column does not overlap the
	 * island's footprint (FVoxelWorldBounds::IntersectsBox2D), the same test the map's IsTileInsideWorld and the far
	 * ocean use, so the columns that stream are exactly the ones the far ocean leaves open (WM-E1).
	 * Returns true if chunk should be CULLED (not rendered).
	 */
	bool ShouldCullIslandBoundary(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const;

	/**
	 * Check if chunk should be culled for Spherical Planet mode: wholly inside the shell window's inner radius or outside
	 * its outer one, or past the horizon (GetPlanetHorizonDistance, SP-C2).
	 * Returns true if chunk should be CULLED (not rendered).
	 */
	bool ShouldCullBeyondHorizon(
		const FIntVector& ChunkCoord,
		const FLODQueryContext& Context
	) const;

public:
	/**
	 * How far a planet viewer can see terrain (SP-C2): sqrt(2Rh + h^2) + sqrt(2RH + H^2), the viewer's tangent distance to
	 * the datum sphere plus a peak's, so mountains past the viewer's own horizon still load.
	 * @param PlanetRadius Datum radius R
	 * @param ViewerAltitude Viewer height above the datum h (clamped at 0)
	 * @param MaxTerrainHeight The terrain's highest point above the datum H (the shell's top)
	 * @return Distance (uu) past which no terrain shows
	 */
	static double GetPlanetHorizonDistance(double PlanetRadius, double ViewerAltitude, double MaxTerrainHeight);

protected:

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

	/** The island's footprint (IVoxelWorldMode::GetWorldBounds): a column streams only when it overlaps it */
	FVoxelWorldBounds IslandBounds;

	// ==================== Spherical Planet Mode Culling ====================
	// Cached from FVoxelWorldModeFactory::MakePlanetParams (SP-A6).

	/** Cached planet radius for horizon calculations (double: it meets R-sized distances, WM-BP4) */
	double PlanetRadius = 0.0;

	/**
	 * The terrain's highest point above the datum (the horizon's peaks, SP-C2): its real bounds, the displacement bounds
	 * (FSphericalPlanetWorldMode::GetDisplacementBounds) within the configured shell (WM-BP8).
	 */
	float PlanetMaxTerrainHeight = 0.0f;

	/** The terrain's deepest point below the datum (inner shell culling), from the same real bounds. */
	float PlanetMaxTerrainDepth = 0.0f;

	/** Planet centre in world space (WorldOrigin) */
	FVector PlanetCenter = FVector::ZeroVector;

	/**
	 * The shell window's radii (SP-C1): the terrain's real bounds (WM-BP8: the displacement bounds within the configured
	 * PlanetMaxTerrainHeight / Depth) widened by ShellSlackChunks on both sides.
	 */
	double ShellWindowInnerRadius = 0.0;
	double ShellWindowOuterRadius = 0.0;

	/**
	 * Slack around the terrain shell in chunk widths: a chunk this close to the shell loads, so a seam job beside a peak
	 * or valley at the shell's edge has its neighbour's voxels.
	 */
	static constexpr double ShellSlackChunks = 0.5;

	/** The planet's world mode (TerrainWorldMode, when a planet): the analytic surface the slab and the ground point read. */
	const FSphericalPlanetWorldMode* PlanetWorldMode = nullptr;

	/**
	 * Per-chunk radial slab on a planet (WM-BP8): bFarBandSurfaceSlabCulling on a planet. Off, every chunk in the shell
	 * near the viewer streams (the whole shell inside the horizon's cone).
	 */
	bool bPlanetSlab = false;

	/** The cells the slab caches surface ranges for: about one chunk width across on the datum sphere. */
	FVoxelCubeSphereGrid PlanetSlabGrid;

	/** Cell -> surface altitude range (GetPlanetCellSurfaceRange). Game thread; reset past PlanetSlabCacheLimit entries. */
	mutable TMap<FIntPoint, FFloatInterval> PlanetCellSurfaceCache;

	/** Chunk -> radial extent and slab (GetPlanetChunkSlab). Game thread; reset past PlanetSlabCacheLimit entries. */
	mutable TMap<FIntVector, FPlanetChunkSlab> PlanetChunkSlabCache;

	/** A long flight keeps adding cells and chunks; past this many entries a cache starts over (about 20-30 MB each). */
	static constexpr int32 PlanetSlabCacheLimit = 1000000;

	/** GetPlanetSurfaceEnvelope's memo: the direction's cell and the angle it was built for. */
	mutable FIntPoint EnvelopeMemoCell = FIntPoint(MIN_int32, MIN_int32);
	mutable double EnvelopeMemoAngle = -1.0;
	mutable FFloatInterval EnvelopeMemoRange;

	/** The viewer's ground point (GetPlanetGroundRadius), for the position the last Update saw. */
	FVector GroundRadiusViewer = FVector(TNumericLimits<double>::Max());
	double GroundRadiusCached = 0.0;

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
