// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRenderRegions.h"
#include "VoxelWorldConfiguration.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarVoxelRenderRegionChunks(
	TEXT("voxel.Render.RegionChunks"),
	16,
	TEXT("Chunks along a render region's edge, in every world mode.\n")
	TEXT("Each region is its own terrain primitive and scatter HISM per type, located at the region's origin in double, with vertex\n")
	TEXT("and instance positions in float relative to it, so they keep sub-0.01 uu precision at any distance from the world origin\n")
	TEXT("(a plane 100 km out as much as a 1,000 km planet). 0 = one primitive at the world origin (absolute float positions, 8 uu\n")
	TEXT("coarse at 1e8). Read when the renderers start."),
	ECVF_Default);

namespace VoxelRenderRegionsDetail
{
	FORCEINLINE int32 FloorDiv(int32 A, int32 B)
	{
		return (A >= 0) ? (A / B) : -((-A + B - 1) / B);
	}
}

FVoxelRenderRegions FVoxelRenderRegions::FromConfig(const UVoxelWorldConfiguration& Config)
{
	FVoxelRenderRegions Regions;
	Regions.WorldOrigin = Config.WorldOrigin;
	Regions.ChunkWorldSize = Config.GetChunkWorldSize();
	// Every world mode (WM-MP5): a plane far from its origin loses float precision as a big planet does.
	Regions.RegionChunks = FMath::Max(0, CVarVoxelRenderRegionChunks.GetValueOnAnyThread());
	return Regions;
}

FIntVector FVoxelRenderRegions::GetRegion(const FIntVector& ChunkCoord) const
{
	if (!IsSplit())
	{
		return FIntVector::ZeroValue;
	}
	using VoxelRenderRegionsDetail::FloorDiv;
	return FIntVector(FloorDiv(ChunkCoord.X, RegionChunks), FloorDiv(ChunkCoord.Y, RegionChunks), FloorDiv(ChunkCoord.Z, RegionChunks));
}

FIntVector FVoxelRenderRegions::GetRegionAt(const FVector& WorldPos) const
{
	if (!IsSplit() || ChunkWorldSize <= 0.0)
	{
		return FIntVector::ZeroValue;
	}
	const FVector Local = (WorldPos - WorldOrigin) / ChunkWorldSize;
	return GetRegion(FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z)));
}

FVector FVoxelRenderRegions::GetRegionOrigin(const FIntVector& Region) const
{
	if (!IsSplit())
	{
		return FVector::ZeroVector;
	}
	return WorldOrigin + FVector(Region) * (static_cast<double>(RegionChunks) * ChunkWorldSize);
}
