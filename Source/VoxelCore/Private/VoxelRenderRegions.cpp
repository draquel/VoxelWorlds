// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelRenderRegions.h"
#include "VoxelWorldConfiguration.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarVoxelRenderRegionChunks(
	TEXT("voxel.Render.RegionChunks"),
	16,
	TEXT("Planets: chunks along a render region's edge (spherical Phase 6, WM-BP7). Each region is its own terrain primitive and\n")
	TEXT("scatter HISM per type, located at the region's origin in double, with vertex and instance positions in float relative to\n")
	TEXT("it, so they keep sub-0.01 uu precision at any distance from the world origin. 0 = one primitive at the world origin\n")
	TEXT("(absolute float positions, 8 uu coarse at 1e8). Heightmap worlds always use one primitive. Read when the renderers start."),
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
	Regions.RegionChunks = Config.WorldMode == EWorldMode::SphericalPlanet
		? FMath::Max(0, CVarVoxelRenderRegionChunks.GetValueOnAnyThread())
		: 0;
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
