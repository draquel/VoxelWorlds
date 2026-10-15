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

// ==================== World Bounds and Local Frame ====================

namespace SphericalPlanetSurfacePrivate
{
	/** Cube face frames for the surface parameterisation: face normal N, U axis T, V axis B (a point on the face is N + U*T + V*B). */
	struct FCubeFace
	{
		FVector N;
		FVector T;
		FVector B;
	};

	const FCubeFace GCubeFaces[6] = {
		{ FVector( 1, 0, 0), FVector( 0, 1, 0), FVector(0, 0, 1) },  // 0: +X
		{ FVector(-1, 0, 0), FVector( 0,-1, 0), FVector(0, 0, 1) },  // 1: -X
		{ FVector( 0, 1, 0), FVector(-1, 0, 0), FVector(0, 0, 1) },  // 2: +Y
		{ FVector( 0,-1, 0), FVector( 1, 0, 0), FVector(0, 0, 1) },  // 3: -Y
		{ FVector( 0, 0, 1), FVector( 1, 0, 0), FVector(0, 1, 0) },  // 4: +Z
		{ FVector( 0, 0,-1), FVector( 1, 0, 0), FVector(0,-1, 0) },  // 5: -Z
	};
}

bool FSphericalPlanetWorldMode::IsInsideWorld(double X, double Y) const
{
	const double DX = X - PlanetParams.PlanetCenter.X;
	const double DY = Y - PlanetParams.PlanetCenter.Y;
	const double Outer = PlanetParams.GetOuterRadius();
	return DX * DX + DY * DY <= Outer * Outer;
}

FVoxelWorldBounds FSphericalPlanetWorldMode::GetWorldBounds() const
{
	FVoxelWorldBounds Bounds;
	Bounds.Shape = EVoxelWorldBoundsShape::Shell;
	Bounds.Center = PlanetParams.PlanetCenter;
	Bounds.InnerRadius = PlanetParams.GetInnerRadius();
	Bounds.OuterRadius = PlanetParams.GetOuterRadius();
	Bounds.HalfExtent = FVector2D(Bounds.OuterRadius, Bounds.OuterRadius);
	Bounds.MinZ = static_cast<float>(PlanetParams.PlanetCenter.Z) - Bounds.OuterRadius;
	Bounds.MaxZ = static_cast<float>(PlanetParams.PlanetCenter.Z) + Bounds.OuterRadius;
	return Bounds;
}

FVector FSphericalPlanetWorldMode::GetUpVector(const FVector& WorldPos) const
{
	return GetDirectionFromCenter(WorldPos, PlanetParams.PlanetCenter);
}

float FSphericalPlanetWorldMode::GetAltitude(const FVector& WorldPos) const
{
	return CalculateRadialDistance(WorldPos, PlanetParams.PlanetCenter) - PlanetParams.PlanetRadius;
}

FVoxelSurfaceCoord FSphericalPlanetWorldMode::WorldToSurface(const FVector& WorldPos) const
{
	const FVector Dir = GetDirectionFromCenter(WorldPos, PlanetParams.PlanetCenter);
	const FVector Abs = Dir.GetAbs();
	int32 Face;
	if (Abs.X >= Abs.Y && Abs.X >= Abs.Z)
	{
		Face = Dir.X >= 0.0 ? 0 : 1;
	}
	else if (Abs.Y >= Abs.Z)
	{
		Face = Dir.Y >= 0.0 ? 2 : 3;
	}
	else
	{
		Face = Dir.Z >= 0.0 ? 4 : 5;
	}
	const SphericalPlanetSurfacePrivate::FCubeFace& F = SphericalPlanetSurfacePrivate::GCubeFaces[Face];
	// Scale the direction onto the face plane (N . P = 1), then read its coordinates along T and B.
	const double Scale = 1.0 / FVector::DotProduct(Dir, F.N);
	return FVoxelSurfaceCoord(Face, FVector2D(FVector::DotProduct(Dir, F.T) * Scale, FVector::DotProduct(Dir, F.B) * Scale));
}

FVector FSphericalPlanetWorldMode::SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const
{
	const SphericalPlanetSurfacePrivate::FCubeFace& F = SphericalPlanetSurfacePrivate::GCubeFaces[FMath::Clamp(Coord.Face, 0, 5)];
	const FVector Dir = (F.N + Coord.UV.X * F.T + Coord.UV.Y * F.B).GetSafeNormal();
	return PlanetParams.PlanetCenter + Dir * (PlanetParams.PlanetRadius + Altitude);
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

	// Use spherical mapping: direction components directly as noise input, scaled by a fixed factor.
	// Known defects, fixed by SP-B1 in spherical Phase 1 (SPHERICAL_PLANET_REQUIREMENTS.md): only X and Y are
	// sampled, so the terrain is mirrored across the equator, and the fixed factor ties feature size to the
	// radius. (FSphericalPlanetParams::NoiseScale, which was never read, is gone.)
	const float NoiseScale = 10000.0f;
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
