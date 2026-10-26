// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVoxelWorldConfiguration;

/**
 * Render regions (spherical Phase 6, WM-BP7, decision D23): rendering precision at scale. Vertex positions, seam meshes,
 * water tiles and scatter instances are stored in float relative to their primitive's location, and a float of size 1e8
 * (a 1,000 km planet's surface, a plane 1,000 km from its origin) is 8 uu coarse. So the terrain is split into regions of
 * RegionChunks^3 chunks, each region its own primitive (a UVoxelWorldComponent, a scatter HISM per type) located at the
 * region's origin in double (UE's large-world coordinates). A vertex's float offset from its region origin is at most a
 * region across (76,800 uu at 16 chunks of 4,800), so its spacing is under 0.01 uu wherever the region is.
 *
 * Every world mode takes RegionChunks from voxel.Render.RegionChunks (16 by default; planets since WM-BP7, heightmap worlds
 * since world modes review WM-MP5, D40). 0 means one region at the world origin (0, 0, 0): one primitive with absolute
 * positions, the rule heightmap worlds had before, clean to about 100 km from the origin.
 *
 * Pure value type; any thread.
 */
struct VOXELCORE_API FVoxelRenderRegions
{
	/** Chunks along a region's edge; 0 = one region at the world origin. */
	int32 RegionChunks = 0;

	/** The chunk grid's origin (UVoxelWorldConfiguration::WorldOrigin). */
	FVector WorldOrigin = FVector::ZeroVector;

	/** A chunk's edge in world units. */
	double ChunkWorldSize = 0.0;

	/**
	 * The regions a world configuration renders with: voxel.Render.RegionChunks in every world mode (0 = one region).
	 *
	 * @param Config World configuration
	 * @return The regions
	 */
	static FVoxelRenderRegions FromConfig(const UVoxelWorldConfiguration& Config);

	/** Whether the world is split into regions (RegionChunks > 0). */
	bool IsSplit() const { return RegionChunks > 0; }

	/**
	 * The region holding a chunk (floor division of its coordinate by RegionChunks); (0, 0, 0) when not split.
	 *
	 * @param ChunkCoord Chunk coordinate
	 * @return Region coordinate
	 */
	FIntVector GetRegion(const FIntVector& ChunkCoord) const;

	/**
	 * The region holding a world position (the region of the chunk holding it).
	 *
	 * @param WorldPos World position
	 * @return Region coordinate
	 */
	FIntVector GetRegionAt(const FVector& WorldPos) const;

	/**
	 * A region's origin, where its primitives sit: WorldOrigin + Region x RegionChunks x ChunkWorldSize (its min corner);
	 * the world origin (0, 0, 0) when not split.
	 *
	 * @param Region Region coordinate
	 * @return Origin in world space (double)
	 */
	FVector GetRegionOrigin(const FIntVector& Region) const;

	/**
	 * A world position relative to a region origin, as a float: what a primitive at the region origin stores.
	 *
	 * @param WorldPos World position (double)
	 * @param Region Region coordinate
	 * @return The offset from the region's origin
	 */
	FVector3f ToRegionLocal(const FVector& WorldPos, const FIntVector& Region) const
	{
		return FVector3f(WorldPos - GetRegionOrigin(Region));
	}
};
