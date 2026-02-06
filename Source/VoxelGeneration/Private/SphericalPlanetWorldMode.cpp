// Copyright Daniel Raquel. All Rights Reserved.

#include "SphericalPlanetWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelMaterialRegistry.h"

FSphericalPlanetWorldMode::FSphericalPlanetWorldMode()
	: TerrainParams()
	, PlanetParams()
{
}

FSphericalPlanetWorldMode::FSphericalPlanetWorldMode(
	const FWorldModeTerrainParams& InTerrainParams,
	const FSphericalPlanetParams& InPlanetParams)
	: TerrainParams(InTerrainParams)
	, PlanetParams(InPlanetParams)
{
}

float FSphericalPlanetWorldMode::GetDensityAt(
	const FVector& WorldPos,
	int32 LODLevel,
	float NoiseValue) const
{
	// Calculate distance from planet center
	const float DistFromCenter = CalculateRadialDistance(WorldPos, PlanetParams.PlanetCenter);

	// Quick reject: if way outside or inside the terrain shell, return early
	const float InnerRadius = PlanetParams.GetInnerRadius();
	const float OuterRadius = PlanetParams.GetOuterRadius();

	if (DistFromCenter > OuterRadius + 1000.0f)
	{
		// Far above surface - definitely air
		return -1000.0f;
	}
	if (DistFromCenter < InnerRadius - 1000.0f)
	{
		// Deep inside core - definitely solid
		return 1000.0f;
	}

	// Get radial displacement from noise
	const float RadialDisplacement = NoiseToRadialDisplacement(NoiseValue, TerrainParams);

	// Calculate terrain radius at this direction
	const float TerrainRadius = PlanetParams.PlanetRadius + RadialDisplacement;

	// Signed distance: positive inside solid, negative in air
	return CalculateSignedDistance(DistFromCenter, TerrainRadius);
}

float FSphericalPlanetWorldMode::GetTerrainHeightAt(
	float X,
	float Y,
	const FVoxelNoiseParams& NoiseParams) const
{
	// For spherical planets, this method doesn't make as much sense
	// since "height" is radial. We interpret X,Y as a direction and return
	// the terrain radius at that direction.

	// Create a direction from X,Y (treating them as angles or normalized coords)
	// This is a simplified approach - actual implementation might use proper spherical coords
	FVector Direction(X, Y, 0.0f);
	Direction.Normalize();
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::UpVector;
	}

	// Sample noise at this direction
	const float NoiseValue = SampleSphericalNoise(Direction, NoiseParams);

	// Return terrain radius
	return PlanetParams.PlanetRadius + NoiseToRadialDisplacement(NoiseValue, TerrainParams);
}

FIntVector FSphericalPlanetWorldMode::WorldToChunkCoord(
	const FVector& WorldPos,
	int32 ChunkSize,
	float VoxelSize) const
{
	// For spherical planets, we use standard Cartesian chunk coordinates
	// centered on the planet. The spherical nature is handled in density calculation.
	const FVector RelativePos = WorldPos - PlanetParams.PlanetCenter;
	const float ChunkWorldSize = ChunkSize * VoxelSize;

	return FIntVector(
		FMath::FloorToInt(RelativePos.X / ChunkWorldSize),
		FMath::FloorToInt(RelativePos.Y / ChunkWorldSize),
		FMath::FloorToInt(RelativePos.Z / ChunkWorldSize));
}

FVector FSphericalPlanetWorldMode::ChunkCoordToWorld(
	const FIntVector& ChunkCoord,
	int32 ChunkSize,
	float VoxelSize,
	int32 LODLevel) const
{
	const float ChunkWorldSize = ChunkSize * VoxelSize;
	return PlanetParams.PlanetCenter + FVector(ChunkCoord) * ChunkWorldSize;
}

int32 FSphericalPlanetWorldMode::GetMinZ() const
{
	// For spherical planets, Z range is symmetric and large
	// since we need chunks in all directions from center
	return -CHUNK_RANGE;
}

int32 FSphericalPlanetWorldMode::GetMaxZ() const
{
	return CHUNK_RANGE;
}

uint8 FSphericalPlanetWorldMode::GetMaterialAtDepth(
	const FVector& WorldPos,
	float SurfaceHeight,
	float DepthBelowSurface) const
{
	// Material assignment based on depth below surface
	// Similar to other modes but using radial depth

	if (DepthBelowSurface < 0.0f)
	{
		// Above surface - air, no material
		return 0;
	}
	else if (DepthBelowSurface < 100.0f)
	{
		return EVoxelMaterial::Grass;
	}
	else if (DepthBelowSurface < 400.0f)
	{
		return EVoxelMaterial::Dirt;
	}
	else
	{
		return EVoxelMaterial::Stone;
	}
}

// ==================== Static Helpers ====================

float FSphericalPlanetWorldMode::CalculateRadialDistance(
	const FVector& WorldPos,
	const FVector& PlanetCenter)
{
	return FVector::Dist(WorldPos, PlanetCenter);
}

FVector FSphericalPlanetWorldMode::GetDirectionFromCenter(
	const FVector& WorldPos,
	const FVector& PlanetCenter)
{
	const FVector ToPoint = WorldPos - PlanetCenter;
	if (ToPoint.IsNearlyZero())
	{
		return FVector::UpVector;
	}
	return ToPoint.GetSafeNormal();
}

float FSphericalPlanetWorldMode::SampleSphericalNoise(
	const FVector& Direction,
	const FVoxelNoiseParams& NoiseParams)
{
	// Convert direction to a position for noise sampling
	// We use the direction scaled by a large factor to get varied noise
	// This creates coherent noise across the sphere surface

	// Use spherical mapping: direction components directly as noise input
	// Scale by a factor to control feature size on the sphere
	const float NoiseScale = 10000.0f;  // Adjust for feature size
	const FVector NoisePos = Direction * NoiseScale;

	// Use the existing 2D terrain noise sampling, treating X,Y as the coords
	// This gives us coherent features across the sphere
	return FInfinitePlaneWorldMode::SampleTerrainNoise2D(NoisePos.X, NoisePos.Y, NoiseParams);
}

float FSphericalPlanetWorldMode::NoiseToRadialDisplacement(
	float NoiseValue,
	const FWorldModeTerrainParams& TerrainParams)
{
	// Convert noise [-1, 1] to radial displacement
	// HeightScale controls the magnitude of terrain features
	// BaseHeight shifts the overall terrain level
	return TerrainParams.BaseHeight + (NoiseValue * TerrainParams.HeightScale * 0.5f);
}

float FSphericalPlanetWorldMode::CalculateSignedDistance(
	float DistFromCenter,
	float TerrainRadius)
{
	// Signed distance: positive when inside solid (below terrain), negative when in air
	return TerrainRadius - DistFromCenter;
}

bool FSphericalPlanetWorldMode::IsWithinPlanetBounds(
	const FVector& WorldPos,
	const FSphericalPlanetParams& PlanetParams)
{
	const float Distance = CalculateRadialDistance(WorldPos, PlanetParams.PlanetCenter);
	return Distance >= PlanetParams.GetInnerRadius() && Distance <= PlanetParams.GetOuterRadius();
}
