// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

/**
 * The map's tile draw queue (VoxelMap, QA refinement R22): which tiles wait for a (re)draw, which are drawing, and
 * whether a finished drawing is still current. Kept free of the world so the policy unit-tests headless; the
 * subsystem owns one and calls it on the game thread only.
 *
 * - A request is independent of the tile cache: a cached tile can be queued for a redraw, and its cached image
 *   stays on screen until the new one lands.
 * - Each tile has its own generation. An invalidating request (an edit or conditioning change landed) bumps it, so
 *   a drawing that started earlier is discarded on completion and the tile is drawn again; other tiles' drawings
 *   are unaffected.
 * - A non-invalidating request for a tile that is drawing is satisfied by that drawing.
 * - Priority requests (edit refreshes) start before the backlog (for example restored exploration).
 */
class VOXELMAP_API FVoxelMapTileQueue
{
public:
	/**
	 * Ask for a tile to be drawn.
	 * @param Key         Packed tile key
	 * @param bInvalidate The tile's current content is stale: a drawing already running is discarded and redone
	 * @param bPriority   Start before non-priority tiles
	 */
	void Request(uint64 Key, bool bInvalidate, bool bPriority = false);

	/**
	 * Take the next waiting tile that is not drawing (priority tiles first) and mark it drawing.
	 * @param OutKey        Receives the tile key
	 * @param OutGeneration Receives the generation the drawing must report to Complete
	 * @return False when nothing can start
	 */
	bool StartNext(uint64& OutKey, uint32& OutGeneration);

	/**
	 * As StartNext, skipping waiting tiles the caller cannot start now (they keep waiting, in place).
	 * @param OutKey        Receives the tile key
	 * @param OutGeneration Receives the generation the drawing must report to Complete
	 * @param CanStart      Whether a waiting tile may start now
	 * @return False when nothing can start
	 */
	bool StartNext(uint64& OutKey, uint32& OutGeneration, TFunctionRef<bool(uint64 Key)> CanStart);

	/**
	 * Start a specific waiting tile now.
	 * @param Key           Packed tile key
	 * @param OutGeneration Receives the generation the drawing must report to Complete
	 * @return False when the tile is not waiting or is already drawing
	 */
	bool Start(uint64 Key, uint32& OutGeneration);

	/**
	 * A drawing finished.
	 * @param Key        Packed tile key
	 * @param Generation The generation Start / StartNext returned for it
	 * @return True if the result is current (store it); false if the tile was invalidated meanwhile (discard it;
	 *         the tile is waiting again)
	 */
	bool Complete(uint64 Key, uint32 Generation);

	/**
	 * A drawing could not start after all (no world mode): forget it. Leaves no trace of the tile in the queue.
	 * @param Key Packed tile key
	 */
	void Cancel(uint64 Key);

	/** The tile waits for a drawing (it may also be drawing, when it was invalidated meanwhile). */
	bool IsWaiting(uint64 Key) const { return Waiting.Contains(Key); }

	/** The tile is drawing now. */
	bool IsDrawing(uint64 Key) const { return Drawing.Contains(Key); }

	/** Waiting or drawing: a request would be redundant unless it invalidates. */
	bool IsQueued(uint64 Key) const { return IsWaiting(Key) || IsDrawing(Key); }

	/** Number of drawings in flight. */
	int32 NumDrawing() const { return Drawing.Num(); }

	/** Number of tiles waiting. */
	int32 NumWaiting() const { return Waiting.Num(); }

	/** The tile's current generation (0 until first invalidated). */
	uint32 GetGeneration(uint64 Key) const { return Generations.FindRef(Key); }

	/** Forget everything (shutdown). */
	void Reset();

private:
	/** Tiles waiting for a drawing. */
	TSet<uint64> Waiting;

	/** The waiting tiles that start first. A subset of Waiting. */
	TSet<uint64> PriorityWaiting;

	/** Tiles drawing now -> the generation their drawing started under. */
	TMap<uint64, uint32> Drawing;

	/** Per-tile generation; bumped by invalidating requests. */
	TMap<uint64, uint32> Generations;
};
