// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBlockSurface.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCoreTypes.h"
#include "VoxelWorldConfiguration.h"

bool FVoxelBlockSurface::UsesBlockSurface(const UVoxelWorldConfiguration& Config)
{
	return Config.MeshingMode == EMeshingMode::Cubic
		&& Config.WorldMode != EWorldMode::SphericalPlanet
		&& Config.VoxelSize > 0.0f;
}

double FVoxelBlockSurface::ColumnCorner(double Coord, double Origin, float VoxelSize)
{
	return Origin + FMath::FloorToDouble((Coord - Origin) / VoxelSize) * VoxelSize;
}

bool FVoxelBlockSurface::IsGeneratedSolid(float Height, double VoxelZ, float VoxelSize)
{
	// The generators' own arithmetic: a float signed distance from the column height to the voxel's position, mapped to
	// a density and compared with the surface threshold (IsSolid).
	const float SignedDistance = FInfinitePlaneWorldMode::CalculateSignedDistance(static_cast<float>(VoxelZ), Height);
	return FInfinitePlaneWorldMode::SignedDistanceToDensity(SignedDistance, VoxelSize) >= VOXEL_SURFACE_THRESHOLD;
}

int64 FVoxelBlockSurface::TopSolidLayerForHeight(float Height, double OriginZ, float VoxelSize)
{
	// The layer whose cell holds the height is solid (its signed distance is at least 0); the one above can still be,
	// when the height lies within the density's rounding (0.004 voxel) under its bottom face.
	if (!FMath::IsFinite(Height) || VoxelSize <= 0.0f)
	{
		return 0;
	}
	int64 Layer = static_cast<int64>(FMath::FloorToDouble((static_cast<double>(Height) - OriginZ) / VoxelSize));
	// Each loop moves at most a step or two: the density saturates a voxel either side of the height.
	for (int32 Step = 0; Step < 4 && IsGeneratedSolid(Height, OriginZ + static_cast<double>(Layer + 1) * VoxelSize, VoxelSize); ++Step)
	{
		++Layer;
	}
	for (int32 Step = 0; Step < 4 && !IsGeneratedSolid(Height, OriginZ + static_cast<double>(Layer) * VoxelSize, VoxelSize); ++Step)
	{
		--Layer;
	}
	return Layer;
}

double FVoxelBlockSurface::BlockTopForHeight(float Height, double OriginZ, float VoxelSize)
{
	return LayerTop(TopSolidLayerForHeight(Height, OriginZ, VoxelSize), OriginZ, VoxelSize);
}
