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
 * Three ways a tile is worked out:
 *  - by a Complete gather that finds it missing, then and there on the caller's thread: a chunk's voxels are
 *    generated once, so correctness never depends on the warm-up. Counted (FVoxelHydrologyStats::SyncTiles on the game
 *    thread, OffThreadTiles elsewhere).
 *  - by the worker, one tile at a time off the game thread: the tiles around each viewer (WarmUp), and those a Ready
 *    gather did not get. A Ready gather never works anything out: it takes what is kept and queues the rest, and a
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

	/** Whether every tile of a region is kept: a Complete gather over it then works nothing out. */
	virtual bool HoldsWaterBodies(const FBox2D& Bounds) const override;

	// ---- IVoxelTerrainConditioner: the beds ----

	virtual void GatherConditioning(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const override {}
	virtual void GatherConditioningLines(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelConditioningLineRef>& OutLines) const override;

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
	 * @param Centres The viewers' world X and Y
	 * @param Radius The distance (GetWarmUpRadius, GetFarRadius)
	 * @param bWait True: in parallel on the task threads, returning when all are kept (the world's start). False: queued
	 *              for the worker, the nearest first
	 * @return How many tiles were worked out or queued
	 */
	int32 WarmUp(TConstArrayView<FVector2D> Centres, double Radius, bool bWait);

	/** @return The radius the world's start waits for, and the chunks' gathers stay inside: the settings' view distances and a tile */
	double GetWarmUpRadius() const;

	/** @return The radius the worker keeps worked out for what draws far away */
	double GetFarRadius() const;

	/**
	 * The tiles that arrived with a body after a Ready gather had missed them: their boxes. The owner hands each to the
	 * chunk manager's water and conditioning announcements.
	 * @param OutRegions Appended with the boxes; the list is emptied
	 */
	void TakeAnnouncements(TArray<FBox2D>& OutRegions);

	/** @return Whether a tile with a body was stored since the last call: what draws the bodies gathers them again */
	bool TakeBodiesArrived();

	/** The world streams from now on (its first tick): a tile worked out inside a gather on the game thread counts as one a frame felt. */
	void MarkStarted();

	/**
	 * The lakes and the river systems that reach a region, worked out here when they are missing (a survey, a test).
	 * @param Region World XY
	 * @param Out The generator's own answer for the region
	 */
	void Survey(const FBox2D& Region, FVoxelHydrologyRegion& Out);

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
	};

	struct FTileEntry
	{
		TSharedPtr<const FTile, ESPMode::ThreadSafe> Tile;
		uint64 LastUsed = 0;
	};

	/** Who works a tile out, for the counters */
	enum class EBuild : uint8 { Gather, Worker, Warm };

	FBox2D BoxOfTile(const FIntPoint& Tile) const;

	/** The tiles a region meets: [Min, Max) */
	FIntRect TilesOf(const FBox2D& Region) const;

	/** The gather both interfaces share: the tiles of a region, worked out or queued by the need. */
	void Gather(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>* OutBodies, TArray<FVoxelConditioningLineRef>* OutLines);

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
	TArray<FBox2D> Announcements;
	bool bBodiesArrived = false;
	bool bWarnedTooWide = false;
	bool bStarted = false;

	/** A disc a warm-up that does not wait has gone through: asked again for a viewer that has barely moved, it is not walked again */
	struct FWarmedDisc
	{
		FVector2D Centre = FVector2D::ZeroVector;
		double Radius = 0.0;
		uint64 Epoch = 0;
	};
	TArray<FWarmedDisc, TInlineAllocator<8>> WarmedDiscs;

	/** Counts the times tiles were dropped from the cache: a disc gone through before then is gone through again */
	uint64 TrimEpoch = 0;

	FVoxelHydrologyStats Stats;
};
