// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "VoxelHydrology.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelWaterLevels.h"

/** What the hydrology network has done since it was made (voxel.Water.Stats; the gate's "no river worked out inside a gather"). */
struct FVoxelHydrologyStats
{
	/** Gathers by need */
	int64 CompleteGathers = 0;
	int64 ReadyGathers = 0;

	/**
	 * Tiles worked out inside a Complete gather on the game thread once the world streams (a chunk asked for bodies the
	 * network did not hold), their cost and the dearest gather. The gate: none
	 */
	int64 SyncTiles = 0;
	double SyncMs = 0.0;
	double WorstSyncMs = 0.0;

	/** The same before the world's first tick: what registers at the world's start asked for bodies past the warm-up's disc (the POI layer's survey of the placement) */
	int64 StartSyncTiles = 0;
	double StartSyncMs = 0.0;

	/** Tiles worked out inside a Complete gather on another thread (the POI layer's placement and road searches) */
	int64 OffThreadTiles = 0;
	double OffThreadMs = 0.0;

	/** Tiles worked out by the worker (around the viewers, and what a Ready gather queued) */
	int64 WorkerTiles = 0;
	double WorkerMs = 0.0;

	/** Tiles worked out by a warm-up that waited (the registration's) */
	int64 WarmTiles = 0;
	double WarmMs = 0.0;

	/** Tiles a Ready gather did not get (queued), tiles announced when they arrived with a body, Ready gathers too wide to queue anything */
	int64 ReadyMisses = 0;
	int64 Announced = 0;
	int64 ReadyTooWide = 0;

	/** Complete gathers too wide to work out (answered with what was at hand, and warned about) */
	int64 CompleteTooWide = 0;

	/** Warm-ups that did not wait and went through a disc's tiles (a viewer had moved), their cost on the caller's thread and the dearest */
	int64 WarmUpAsks = 0;
	double WarmUpMs = 0.0;
	double WorstWarmUpMs = 0.0;

	/** Tiles the worker queued for regions RequestRegion asked for (G232) */
	int64 RequestedTiles = 0;

	/** Descents and lakes worked out, and ground samples the lattice squares' descents read */
	int64 TracesMade = 0;
	int64 LakesMade = 0;
	int64 SystemsMade = 0;

	/** The cache now */
	int32 Tiles = 0;
	int32 TilesWithBodies = 0;
	int32 Queued = 0;
	int32 Squares = 0;
	int32 Springs = 0;
	int32 Rivers = 0;
	int32 Lakes = 0;
	int32 KeptLakes = 0;
	int32 Systems = 0;
	int32 RefusedSystems = 0;
	int32 Reaches = 0;
	int32 Conflicts = 0;
};

/**
 * The hydrology of one world, kept as it is worked out (Documentation/HYDROLOGY_PLAN.md, section 5.8): the generator
 * (FVoxelHydrology) says what a region's ponds and brooks are; this holds the answers and decides who works them out
 * and when. It is the world's source of generated water bodies (IVoxelWaterBodySource) and of their beds
 * (IVoxelTerrainConditioner: lines only), the twin of the POI layer's road network.
 *
 * What is kept: a descent by its spring's lattice square, a lake by its pit, a river system by its mouth, and by
 * tile (a square of the generator's cells) the bodies and beds that reach it, so a chunk's gather is a lookup.
 *
 * A region is a world XY box on a heightmap world and a cap on a planet (FVoxelConditioningRegion), and the gathers,
 * the warm-up and the announcements come in both forms. A planet's tiles are the cells of a cube sphere
 * (FVoxelCubeSphereGrid), about as wide as a heightmap world's; a tile's region there is the cap around it.
 *
 * Three ways a tile is worked out:
 *  - by a Complete gather that finds it missing, then and there on the caller's thread: a chunk's voxels are
 *    generated once, so correctness never depends on the warm-up. Counted (FVoxelHydrologyStats::SyncTiles on the game
 *    thread, OffThreadTiles elsewhere).
 *  - by the worker, one tile at a time off the game thread: the tiles around each viewer (WarmUp), those a Ready
 *    gather did not get, and the regions a reader asked for (RequestRegion: the POI layer's roads and pads far from
 *    the viewers, G232). A Ready gather never works anything out: it takes what is kept and queues the rest, and a
 *    tile that arrives with a body after a Ready gather missed it is announced (TakeAnnouncements).
 *  - by a warm-up that waits (the world's start): in parallel on the task threads, before the first chunk asks.
 *
 * Thread-safe: the gathers run on the game thread and on the POI layer's workers while the worker stores what it
 * made. The generator is deterministic, so a tile worked out twice is the same tile; the first stored is kept.
 */
class VOXELSTREAMING_API FVoxelHydrologyNetwork
	: public IVoxelWaterBodySource
	, public IVoxelTerrainConditioner
	, public IVoxelHydrologyCache
	, public TSharedFromThis<FVoxelHydrologyNetwork, ESPMode::ThreadSafe>
{
public:
	/**
	 * @param InGround The world's natural ground (copied; its function is shared and thread-safe)
	 * @param InSettings The hydrology configuration's settings (sanitized here)
	 * @param InViewDistance The world's view distance: what the warm-up's radii are counted in
	 */
	FVoxelHydrologyNetwork(const FVoxelHydrologyGround& InGround, const FVoxelHydrologySettings& InSettings, double InViewDistance);
	virtual ~FVoxelHydrologyNetwork() = default;

	const FVoxelHydrologySettings& GetSettings() const { return Settings; }
	const FVoxelHydrologyGround& GetGround() const { return Ground; }

	// ---- IVoxelWaterBodySource ----

	virtual void GatherWaterBodies(const FBox2D& Bounds, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& OutBodies) const override;
	virtual void GatherWaterBodiesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need,
		TArray<FVoxelWaterBody>& OutBodies) const override;

	/** Whether every tile of a region is kept: a Complete gather over it then works nothing out. */
	virtual bool HoldsWaterBodies(const FBox2D& Bounds) const override;
	virtual bool HoldsWaterBodiesOnSphere(const FVector& Direction, double AngularRadius) const override;

	/** The levels of the kept tiles' bodies round a cap, from each tile's own range: no body is listed or copied. */
	virtual bool GetLevelRangeAtHandOnSphere(const FVector& Direction, double AngularRadius, bool& bOutAny, double& OutMinLevel, double& OutMaxLevel) const override;

	// ---- IVoxelTerrainConditioner: the beds ----

	virtual void GatherConditioning(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const override {}
	virtual void GatherConditioningLines(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelConditioningLineRef>& OutLines) const override;
	virtual void GatherConditioningLinesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need,
		TArray<FVoxelConditioningLineRef>& OutLines) const override;

	// ---- IVoxelHydrologyCache ----

	virtual TSharedPtr<const FVoxelHydrologyTrace> FindTrace(const FIntPoint& Square) override;
	virtual TSharedRef<const FVoxelHydrologyTrace> StoreTrace(const TSharedRef<const FVoxelHydrologyTrace>& Trace) override;
	virtual TSharedPtr<const FVoxelHydrologyLake> FindLake(const FIntPoint& Pit) override;
	virtual TSharedRef<const FVoxelHydrologyLake> StoreLake(const TSharedRef<const FVoxelHydrologyLake>& Lake) override;
	virtual TSharedPtr<const FVoxelHydrologySystem> FindSystem(const FIntPoint& Mouth) override;
	virtual TSharedRef<const FVoxelHydrologySystem> StoreSystem(const TSharedRef<const FVoxelHydrologySystem>& System) override;

	// ---- The warm-up ----

	/**
	 * Work out the tiles around viewers before a chunk asks: every tile within a distance of a centre that the network
	 * does not hold.
	 * @param Places The viewers' places: world (X, Y, 0) on a heightmap world, the unit direction from the planet's
	 *               centre on a planet (GetPlaceOf)
	 * @param Radius The distance, along the surface on a planet (GetWarmUpRadius, GetFarRadius)
	 * @param bWait True: in parallel on the task threads, returning when all are kept (the world's start). False: queued
	 *              for the worker, the nearest first
	 * @return How many tiles were worked out or queued
	 */
	int32 WarmUpAround(TConstArrayView<FVector> Places, double Radius, bool bWait);

	/** WarmUpAround on a heightmap world, by the viewers' world X and Y. */
	int32 WarmUp(TConstArrayView<FVector2D> Centres, double Radius, bool bWait);

	/**
	 * Queue for the worker the tiles of a region the network does not hold, the nearest to a place first, after what is
	 * queued: what a reader that took what was at hand is waiting for (the POI layer's roads and pads far from the
	 * viewers, G232). Nothing is worked out or walked here: the worker goes through the region's tiles (a few
	 * thousand: a millisecond or more on the caller's thread) before the tiles it has queued.
	 * @param Region A box on a heightmap world, a cap on a planet
	 * @param Place Where the nearest tiles are counted from (GetPlaceOf)
	 * @param MaxTiles Queue no more than this many
	 * @return Whether the region was taken (one of this world's kind, and the network not shut down)
	 */
	bool RequestRegion(const FVoxelConditioningRegion& Region, const FVector& Place, int32 MaxTiles);

	/**
	 * The cache's load, without walking it (GetStats walks every kept tile).
	 * @param OutKept The tiles kept
	 * @param OutQueued The tiles, the planet discs and the requested regions waiting for the worker
	 * @param OutLimit The tiles kept past which the ones used longest ago are dropped
	 */
	void GetLoad(int32& OutKept, int32& OutQueued, int32& OutLimit) const;

	/** @return The place of a world position: its column (X, Y, 0), or on a planet the unit direction from the centre */
	FVector GetPlaceOf(const FVector& WorldPosition) const;

	/** @return The region within a distance of a place: a box on a heightmap world, a cap on a planet */
	FVoxelConditioningRegion GetRegionAround(const FVector& Place, double Radius) const;

	/** @return The radius the world's start waits for, and the chunks' gathers stay inside: the settings' view distances and a tile */
	double GetWarmUpRadius() const;

	/** @return The radius the worker keeps worked out for what draws far away */
	double GetFarRadius() const;

	/**
	 * The tiles that arrived with a body after a Ready gather had missed them: their regions (a box, or on a planet the
	 * cap around the tile). The owner hands each to the chunk manager's water and conditioning announcements.
	 * @param OutRegions Appended with the regions; the list is emptied
	 */
	void TakeAnnouncements(TArray<FVoxelConditioningRegion>& OutRegions);

	/** @return Whether a tile with a body was stored since the last call: what draws the bodies gathers them again */
	bool TakeBodiesArrived();

	/** The world streams from now on (its first tick): a tile worked out inside a gather on the game thread counts as one a frame felt. */
	void MarkStarted();

	/**
	 * The lakes and the river systems that reach a region, worked out here when they are missing (a survey, a test).
	 * @param Region A box on a heightmap world, a cap on a planet
	 * @param Out The generator's own answer for the region
	 */
	void Survey(const FVoxelConditioningRegion& Region, FVoxelHydrologyRegion& Out);
	void Survey(const FBox2D& Region, FVoxelHydrologyRegion& Out) { Survey(FVoxelConditioningRegion::OfBox(Region), Out); }

	/** @return The counters and the cache's sizes now */
	FVoxelHydrologyStats GetStats() const;

	/** Zero the counters (the cache stays). */
	void ResetStats();

	/** Block until the worker has emptied its queue (tests). */
	void WaitForWorker();

	/** Stop the worker after the tile it is on and drop what is queued. The owner calls it before it lets go. */
	void Shutdown();

private:
	/** The bodies and the beds that reach a tile. Immutable once stored. */
	struct FTile
	{
		TArray<FVoxelWaterBody> Bodies;
		TArray<FVoxelConditioningLineRef> Beds;

		/** The lowest and highest level of the bodies (unset with none), and on a planet the tile's middle */
		double MinLevel = 0.0;
		double MaxLevel = 0.0;
		FVector Centre = FVector::ZeroVector;
	};

	struct FTileEntry
	{
		TSharedPtr<const FTile, ESPMode::ThreadSafe> Tile;
		uint64 LastUsed = 0;
	};

	/** Who works a tile out, for the counters */
	enum class EBuild : uint8 { Gather, Worker, Warm };

	/** A tile's region: its box, or on a planet the cap around it */
	FVoxelConditioningRegion RegionOfTile(const FIntPoint& Tile) const;

	/** Whether a region is one of this world's kind (a box on a heightmap world, a cap on a planet) with something in it */
	bool IsRegionOfWorld(const FVoxelConditioningRegion& Region) const;

	/**
	 * The tiles a region meets.
	 * @param Limit Stop past this many
	 * @param OutTiles The tiles (emptied first): row by row on a heightmap world
	 * @return False when there are more than the limit (OutTiles is then not the whole list)
	 */
	bool TilesOf(const FVoxelConditioningRegion& Region, int64 Limit, TArray<FIntPoint>& OutTiles) const;

	/**
	 * The tiles a region meets, one at a time: row by row on a heightmap world, in a flood's order on a planet.
	 * @param Limit Give up past this many (a planet's cap wider than that many tiles is given up before any is visited)
	 * @param Visit Called with each tile; false stops the walk
	 * @return False when Visit stopped the walk or the region meets more tiles than the limit
	 */
	bool VisitTiles(const FVoxelConditioningRegion& Region, int64 Limit, TFunctionRef<bool(const FIntPoint&)> Visit) const;

	/** @return How far a tile lies from a place (0 when the place is in the tile's region) */
	double DistanceToTile(const FIntPoint& Tile, const FVector& Place) const;

	/** The gather both interfaces share: the tiles of a region, worked out or queued by the need. */
	void Gather(const FVoxelConditioningRegion& Region, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>* OutBodies, TArray<FVoxelConditioningLineRef>* OutLines);

	/** Whether every tile of a region is kept. */
	bool Holds(const FVoxelConditioningRegion& Region) const;

	/** Work tiles out here and keep them: the springs' descents first, in parallel, then each tile's region. Lock not held. @return The wall time, in milliseconds */
	double BuildNow(const TArray<FIntPoint>& Tiles, EBuild Who, bool bParallel);

	/** Queue a tile for the worker unless it is kept or queued, and start the worker. Lock held. @return Whether it was queued */
	bool Enqueue(const FIntPoint& Tile);

	/** The worker's loop. */
	void RunWorker();

	/** Drop the entries used longest ago when a map is over its limit. Lock held. */
	void Trim();

	FVoxelHydrologyGround Ground;
	FVoxelHydrologySettings Settings;
	double ViewDistance = 10000.0;
	double CellSize = 800.0;
	double TileSize = 51200.0;

	/** The generator's grid of the world, and a planet's tiles: the cells of a cube sphere, a tile's width about TileSize */
	FVoxelHydrologyGrid Grid;
	FVoxelCubeSphereGrid TileGrid;

	/** A planet: a tile's width at a face's middle as an angle, which bounds how far any tile's corner lies from its middle */
	double TileCapAngle = 0.0;

	mutable FCriticalSection Lock;
	uint64 UseCounter = 0;
	TMap<FIntPoint, FTileEntry> Tiles;
	TMap<FIntPoint, TSharedRef<const FVoxelHydrologyTrace>> Traces;
	TMap<FIntPoint, TSharedRef<const FVoxelHydrologyLake>> Lakes;
	TMap<FIntPoint, TSharedRef<const FVoxelHydrologySystem>> Systems;

	TArray<FIntPoint> Queue;
	TSet<FIntPoint> QueuedTiles;
	bool bWorkerRunning = false;
	bool bShutdown = false;
	UE::Tasks::FTask WorkerTask;

	/** Tiles a Ready gather did not get: announced when they arrive with a body */
	TSet<FIntPoint> ReadyMissed;
	TArray<FVoxelConditioningRegion> Announcements;
	bool bBodiesArrived = false;
	bool bWarnedTooWide = false;
	bool bStarted = false;

	/** A disc a warm-up that does not wait has gone through: asked again for a viewer that has barely moved, it is not walked again */
	struct FWarmedDisc
	{
		FVector Centre = FVector::ZeroVector;
		double Radius = 0.0;
		uint64 Epoch = 0;

		/** Every tile of the disc is kept (until tiles are dropped: Epoch): a region inside it is held */
		bool bComplete = false;
	};
	TArray<FWarmedDisc, TInlineAllocator<8>> WarmedDiscs;

	/** The discs a warm-up that waited worked out whole (the world's start, the game layer's): what Holds answers from first */
	TArray<FWarmedDisc, TInlineAllocator<8>> HeldDiscs;

	/** A planet: the discs a warm-up that does not wait asked for, which the worker has yet to go through */
	TArray<FWarmedDisc> PendingDiscs;

	/** A region RequestRegion asked for: the worker walks it (its tiles, the nearest to Place first) before the tiles queued */
	struct FRequestedRegion
	{
		FVoxelConditioningRegion Region;
		FVector Place = FVector::ZeroVector;
		int32 MaxTiles = 0;
	};
	TArray<FRequestedRegion> PendingRegions;

	/** Counts the times tiles were dropped from the cache: a disc gone through before then is gone through again */
	uint64 TrimEpoch = 0;

	FVoxelHydrologyStats Stats;
};
