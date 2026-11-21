// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "VoxelMapTypes.h"

/**
 * Who asks for a map tile's drawing, in the order the queue serves them (MAP_PYRAMID_PLAN.md P1): the minimap first,
 * then the world map, then the edit refreshes (an edit overlay's rebuild at level 0 and the neighbours its border
 * changed), then the prefetch (a planet's top levels, drawn when the map resolves its world and kept; P2).
 */
enum class EVoxelMapRequester : uint8
{
	Minimap = 0,
	WorldMap,
	Edits,
	Prefetch,
	Num
};

/**
 * The map's tile draw queue (VoxelMap, QA refinement R22; the view requests of MAP_PYRAMID_PLAN.md P1): which tiles wait
 * for a (re)draw, in what order, which are drawing, and whether a finished drawing is still current. Kept free of the world
 * so the policy unit-tests headless; the subsystem owns one and calls it on the game thread only.
 *
 * - Each requester has its own ordered list. A view (the minimap, the world map) replaces its list with every request
 *   (SetRequests, nearest the view's centre first): what left the view stops waiting, unless another requester still
 *   wants it. The edit refreshes append (Add). StartNext serves the requesters in EVoxelMapRequester order, each list in
 *   its order, so the queue holds at most what the views show plus the edits in flight, whatever is explored.
 * - A request is independent of the tile store: a stored tile can be queued for a redraw, and its image stays on screen
 *   until the new one lands.
 * - Each tile has its own generation. Invalidate bumps it (an edit or conditioning change landed), so a drawing that
 *   started earlier is discarded on completion; other tiles' drawings are unaffected. A request for a tile drawing under
 *   its current generation is satisfied by that drawing.
 */
class VOXELMAP_API FVoxelMapTileQueue
{
public:
	/**
	 * Replace a requester's requests (a view: the tiles it shows that lack an image, nearest its centre first). Tiles of
	 * its previous request not in this one stop waiting unless another requester wants them.
	 * @param Requester The requester
	 * @param NearestFirst The tiles, in the order to draw them
	 */
	void SetRequests(EVoxelMapRequester Requester, TConstArrayView<FVoxelMapTileId> NearestFirst);

	/**
	 * Add one tile at the end of a requester's requests.
	 * @param Requester The requester
	 * @param Id The tile
	 */
	void Add(EVoxelMapRequester Requester, const FVoxelMapTileId& Id);

	/**
	 * The tile's content is stale: bump its generation, so a drawing already running is discarded when it completes. The
	 * tile waits only if a requester asks for it (again).
	 * @param Id The tile
	 */
	void Invalidate(const FVoxelMapTileId& Id);

	/**
	 * Take the next waiting tile that is not drawing (by requester, then in each requester's order) and mark it drawing.
	 * @param OutId         Receives the tile
	 * @param OutGeneration Receives the generation the drawing must report to Complete
	 * @return False when nothing can start
	 */
	bool StartNext(FVoxelMapTileId& OutId, uint32& OutGeneration);

	/**
	 * As StartNext, skipping waiting tiles the caller cannot start now (they keep waiting, in place).
	 * @param OutId         Receives the tile
	 * @param OutGeneration Receives the generation the drawing must report to Complete
	 * @param CanStart      Whether a waiting tile may start now
	 * @return False when nothing can start
	 */
	bool StartNext(FVoxelMapTileId& OutId, uint32& OutGeneration, TFunctionRef<bool(const FVoxelMapTileId& Id)> CanStart);

	/**
	 * A drawing finished.
	 * @param Id         The tile
	 * @param Generation The generation StartNext returned for it
	 * @return True if the result is current (store it); false if the tile was invalidated meanwhile (discard it; it is
	 *         drawn again if a requester still asks for it)
	 */
	bool Complete(const FVoxelMapTileId& Id, uint32 Generation);

	/**
	 * A drawing could not start after all (no world mode): forget it. Leaves no trace of the tile in the queue.
	 * @param Id The tile
	 */
	void Cancel(const FVoxelMapTileId& Id);

	/** The tile waits for a drawing (it may also be drawing, when it was invalidated meanwhile). */
	bool IsWaiting(const FVoxelMapTileId& Id) const { return Waiting.Contains(Id); }

	/** The tile is drawing now. */
	bool IsDrawing(const FVoxelMapTileId& Id) const { return Drawing.Contains(Id); }

	/** Waiting or drawing. */
	bool IsQueued(const FVoxelMapTileId& Id) const { return IsWaiting(Id) || IsDrawing(Id); }

	/** The tile is drawing under its current generation: a request for it would be redundant. */
	bool IsDrawingCurrent(const FVoxelMapTileId& Id) const;

	/** Number of drawings in flight. */
	int32 NumDrawing() const { return Drawing.Num(); }

	/** Number of tiles waiting. */
	int32 NumWaiting() const { return Waiting.Num(); }

	/** Number of tiles a requester waits for (a tile several want counts for each). */
	int32 NumWaitingFor(EVoxelMapRequester Requester) const;

	/** The tile's current generation (0 until first invalidated). */
	uint32 GetGeneration(const FVoxelMapTileId& Id) const { return Generations.FindRef(Id); }

	/** Forget everything (shutdown). */
	void Reset();

private:
	static constexpr int32 NumRequesters = static_cast<int32>(EVoxelMapRequester::Num);
	static uint8 Bit(int32 Requester) { return static_cast<uint8>(1u << Requester); }

	/** One requester's tiles in order, and how far StartNext has taken them. */
	struct FRequestList
	{
		TArray<FVoxelMapTileId> Ids;
		int32 Cursor = 0;
	};

	/** Clear a requester's bit on a waiting tile; the tile stops waiting with no bit left. */
	void Release(int32 Requester, const FVoxelMapTileId& Id);

	FRequestList Lists[NumRequesters];

	/** Tiles waiting for a drawing -> the requesters that want them (a bit each). */
	TMap<FVoxelMapTileId, uint8> Waiting;

	/** Tiles drawing now -> the generation their drawing started under. */
	TMap<FVoxelMapTileId, uint32> Drawing;

	/** Per-tile generation; bumped by Invalidate. */
	TMap<FVoxelMapTileId, uint32> Generations;
};
