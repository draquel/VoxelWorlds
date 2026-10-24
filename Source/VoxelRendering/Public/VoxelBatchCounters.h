// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Mesh batches the voxel scene proxies emitted and skipped (spherical Phase 6, WM-BP3: the streaming bench's batch
 * columns). Every FVoxelSceneProxy::GetDynamicMeshElements call adds to them on the render thread (the main views and any
 * shadow gather alike); the game thread reads and clears them, so a read covers the calls since the previous one.
 */
struct VOXELRENDERING_API FVoxelBatchCounters
{
	/** Chunk, seam and water batches drawn, and the ones skipped as hidden, outside the frustum or over the per-call cap. */
	struct FCounts
	{
		int32 Drawn = 0;
		int32 SkippedInvisible = 0;
		int32 SkippedFrustum = 0;
		int32 SkippedOverLimit = 0;
		/** Of Drawn, the batches drawn in shadow gathers (calls whose view carries a shadow cull volume; visual polish VP3) */
		int32 ShadowDrawn = 0;

		int32 Skipped() const { return SkippedInvisible + SkippedFrustum + SkippedOverLimit; }
	};

	/** Render thread: add one GetDynamicMeshElements call's counts. */
	static void Add(const FCounts& Counts);

	/** @return The counts since the last Consume, then cleared */
	static FCounts Consume();
};
