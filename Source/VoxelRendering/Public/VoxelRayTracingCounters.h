// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The terrain's ray tracing geometry (visual polish F3.3), summed over every voxel scene proxy: the acceleration structures
 * the proxies hold for their current chunk and seam meshes (the streaming bench's RTGeomN and RTMB columns,
 * voxel.Render.RayTracingStats). The proxies add to them on the render thread; any thread reads them.
 */
struct VOXELRENDERING_API FVoxelRayTracingCounters
{
	struct FCounts
	{
		/** Geometries alive now and their acceleration structures' bytes (RHICalcRayTracingGeometrySize) */
		int64 GeometriesLive = 0;
		int64 BytesLive = 0;

		/** Geometries built and released since the start (or the last Reset) */
		int64 Builds = 0;
		int64 Releases = 0;

		/** Instances the last render frame's gathers emitted (summed over the views), and since the start the gathers that left
		 *  a geometry out because its acceleration structure was still building */
		int64 InstancesLastFrame = 0;
		int64 SkippedPending = 0;

		/** Proxies alive that give their meshes ray tracing geometry */
		int64 ProxiesLive = 0;
	};

	/** @return The counts now */
	static FCounts Get();

	/** Clear the cumulative counts (Builds, Releases, SkippedPending); the live ones stay. */
	static void Reset();

	/** Render thread: a geometry was built (its bytes) or released. */
	static void AddBuild(int64 Bytes);
	static void AddRelease(int64 Bytes);

	/** Render thread: one proxy's gather emitted these instances and left these geometries out (still building). */
	static void AddGathered(int64 Instances, int64 SkippedPending);

	/** A proxy with ray tracing geometry was created (+1) or destroyed (-1). */
	static void AddProxy(int32 Delta);
};
