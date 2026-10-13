// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * When the chunk manager re-evaluates LOD levels (VoxelStreaming).
 *
 * UVoxelChunkManager::EvaluateLODLevelChanges walks every loaded chunk, compares its committed LOD with
 * the LOD strategy's current answer and queues at most MaxLODRemeshPerFrame of the differences. LOD bands
 * are distances from the viewer POSITION, so the strategy's answers change continuously as the viewer
 * moves, not only when it crosses into another chunk; and the per-frame cap leaves candidates for later.
 *
 * The schedule this replaces evaluated only on a viewer-chunk change or while a "sweep" flag was set. The
 * flag was set whenever every queue was empty and cleared only by an evaluation that ran with every queue
 * empty and generated no work. Once streaming refilled the queues, a set flag could never clear and a
 * clear flag could never be set, so whichever state it held at that moment lasted until streaming next
 * went idle: LOD re-evaluated every frame, or only on chunk changes with the remainder of each capped
 * batch dropped until the next one. Which of the two a run got depended on whether an LOD re-mesh was
 * still in flight when the queues last emptied — a timing race. That is KI12 (WORLD_MODES_REVIEW_PLAN.md
 * §13.10): island benches bimodal at 12,500 / 1,341 LOD re-meshes on the same route.
 *
 * Here no trigger outlives the condition that set it:
 *  - the viewer entered another chunk, or moved MoveThreshold since the last evaluation;
 *  - the last evaluation stopped at the per-frame cap (work left over);
 *  - the pipeline went idle (busy -> idle) since the last evaluation that queued nothing: catches chunks
 *    that became Loaded with a stale LOD while the viewer stood still. This sweep ends at the first
 *    evaluation that queues nothing, whether or not the pipeline is still idle, and a viewer standing
 *    still in a settled world evaluates nothing (the old flag re-evaluated every idle frame).
 *
 * Usage, once per tick: `if (Schedule.ShouldEvaluate(...)) { ...evaluate...; Schedule.OnEvaluated(...); }`.
 * Not thread-safe: game thread only, like the chunk manager that owns it.
 */
struct FVoxelLODEvalSchedule
{
	/** Viewer travel (uu) since the last evaluation that triggers one by itself. */
	float MoveThreshold = 100.0f;

	/**
	 * Decide whether to evaluate LOD levels this tick. Records the pipeline going idle as a pending sweep,
	 * so call it every tick, even on ticks that would evaluate anyway.
	 * @param bViewerChunkChanged Viewer entered another chunk this tick.
	 * @param ViewerPosition      Viewer (streaming origin) position this tick.
	 * @param bPipelineIdle       No generation or meshing work queued, in flight or awaiting upload.
	 * @return True when EvaluateLODLevelChanges should run this tick.
	 */
	bool ShouldEvaluate(bool bViewerChunkChanged, const FVector& ViewerPosition, bool bPipelineIdle)
	{
		if (bPipelineIdle && !bWasPipelineIdle)
		{
			bIdleSweep = true;
		}
		bWasPipelineIdle = bPipelineIdle;
		const bool bMoved = !bHasEvaluated
			|| FVector::DistSquared(ViewerPosition, LastEvalPosition) >= FMath::Square(MoveThreshold);
		return bViewerChunkChanged || bMoved || bCapReached || bIdleSweep;
	}

	/**
	 * Record an evaluation's outcome.
	 * @param ViewerPosition   Viewer position the evaluation used.
	 * @param NumQueued        LOD re-meshes it queued.
	 * @param bInCapReached    It stopped at the per-frame cap with candidates left over.
	 */
	void OnEvaluated(const FVector& ViewerPosition, int32 NumQueued, bool bInCapReached)
	{
		bHasEvaluated = true;
		LastEvalPosition = ViewerPosition;
		bCapReached = bInCapReached;
		if (NumQueued == 0)
		{
			bIdleSweep = false;
		}
	}

	/** Forget all history (world (re)initialisation): the next ShouldEvaluate returns true. */
	void Reset()
	{
		bHasEvaluated = false;
		LastEvalPosition = FVector::ZeroVector;
		bCapReached = false;
		bIdleSweep = false;
		bWasPipelineIdle = false;
	}

	/** @return True while an idle-pipeline sweep is pending (diagnostics and tests). */
	bool IsSweepPending() const { return bIdleSweep; }

	/** @return True when the last evaluation stopped at the per-frame cap (diagnostics and tests). */
	bool IsCapReached() const { return bCapReached; }

private:
	FVector LastEvalPosition = FVector::ZeroVector;
	bool bHasEvaluated = false;
	bool bCapReached = false;
	bool bIdleSweep = false;
	bool bWasPipelineIdle = false;
};
