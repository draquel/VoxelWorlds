// Copyright Daniel Raquel. All Rights Reserved.

#include "SphericalPlanetWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
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

	// The tangent warp (WORLD_MODES_REVIEW_PLAN.md §15.1): u = (4 / pi) * atan(g) maps the gnomonic coordinate g in
	// [-1, 1] to u in [-1, 1], uniform in angle along the face's centre lines; its inverse is g = tan(pi / 4 * u).
	double WarpFromGnomonic(double G)
	{
		return FMath::Atan(G) * (4.0 / UE_DOUBLE_PI);
	}

	double GnomonicFromWarp(double U)
	{
		return FMath::Tan(U * (UE_DOUBLE_PI / 4.0));
	}
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
	using namespace SphericalPlanetSurfacePrivate;
	const FCubeFace& F = GCubeFaces[Face];
	// Scale the direction onto the face plane (N . P = 1), read its gnomonic coordinates along T and B, then warp them.
	const double Scale = 1.0 / FVector::DotProduct(Dir, F.N);
	return FVoxelSurfaceCoord(Face, FVector2D(
		WarpFromGnomonic(FVector::DotProduct(Dir, F.T) * Scale),
		WarpFromGnomonic(FVector::DotProduct(Dir, F.B) * Scale)));
}

FVector FSphericalPlanetWorldMode::SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const
{
	using namespace SphericalPlanetSurfacePrivate;
	const FCubeFace& F = GCubeFaces[FMath::Clamp(Coord.Face, 0, 5)];
	const FVector Dir = (F.N + GnomonicFromWarp(Coord.UV.X) * F.T + GnomonicFromWarp(Coord.UV.Y) * F.B).GetSafeNormal();
	return PlanetParams.PlanetCenter + Dir * (PlanetParams.PlanetRadius + Altitude);
}

FVoxelTerrainSurfacePoint FSphericalPlanetWorldMode::GetSurfacePoint(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams) const
{
	const FVector& Center = PlanetParams.PlanetCenter;
	const float Radius = PlanetParams.PlanetRadius;
	auto SurfaceAt = [&](const FVector& Direction)
	{
		return Center + Direction * (Radius + SampleRadialDisplacement(Direction, Radius, TerrainParams, NoiseParams, &BiomeSnapshot));
	};

	const FVector Dir = GetDirectionFromCenter(WorldPos, Center);
	FVoxelTerrainSurfacePoint Point;
	Point.Position = SurfaceAt(Dir);

	// Central differences along two tangents, SurfaceNormalStep of arc either side on the datum sphere. (T1, T2, Dir) is
	// right-handed, so the cross product of the two differences points out of the terrain.
	const FVector T1 = FVector::CrossProduct(FMath::Abs(Dir.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector, Dir).GetSafeNormal();
	const FVector T2 = FVector::CrossProduct(Dir, T1);
	const double Angle = SurfaceNormalStep / FMath::Max(Radius, 1.0f);
	const FVector D1 = SurfaceAt((Dir + T1 * Angle).GetSafeNormal()) - SurfaceAt((Dir - T1 * Angle).GetSafeNormal());
	const FVector D2 = SurfaceAt((Dir + T2 * Angle).GetSafeNormal()) - SurfaceAt((Dir - T2 * Angle).GetSafeNormal());
	Point.Normal = FVector::CrossProduct(D1, D2).GetSafeNormal();
	if (FVector::DotProduct(Point.Normal, Dir) < 0.0)
	{
		Point.Normal = -Point.Normal;
	}
	Point.Altitude = GetAltitude(Point.Position);
	return Point;
}

float FSphericalPlanetWorldMode::GetDensityAt(
	const FVector& WorldPos,
	int32 LODLevel,
	float NoiseValue) const
{
	// The terrain noise is the caller's; continentalness is not applied (SampleRadialDisplacement is the full surface).
	const float DistFromCenter = CalculateRadialDistance(WorldPos, PlanetParams.PlanetCenter);
	const float TerrainRadius = PlanetParams.PlanetRadius + NoiseToRadialDisplacement(NoiseValue, TerrainParams);
	return ShellSignedDistance(DistFromCenter, TerrainRadius, PlanetParams);
}

float FSphericalPlanetWorldMode::GetTerrainHeightAt(
	float X,
	float Y,
	const FVoxelNoiseParams& NoiseParams) const
{
	// Legacy: interprets (X, Y) as the equatorial direction (X, Y, 0) and returns the terrain RADIUS there, not a Z.
	// Heightmap consumers that call this on a planet read it wrongly; SP-A3 (Phase 4) moves them to GetSurfacePoint.
	FVector Direction(X, Y, 0.0f);
	Direction.Normalize();
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::UpVector;
	}
	return PlanetParams.PlanetRadius
		+ SampleRadialDisplacement(Direction, PlanetParams.PlanetRadius, TerrainParams, NoiseParams, &BiomeSnapshot);
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
	float PlanetRadius,
	const FVoxelNoiseParams& NoiseParams)
{
	// The full 3D point on the datum sphere: no hemisphere mirror, no seam, feature size in world units (SP-B1).
	return FVoxelCPUNoiseGenerator::FBM3D(GetNoiseSamplePoint(Direction, PlanetRadius), NoiseParams);
}

FWorldModeTerrainParams FSphericalPlanetWorldMode::ComputeEffectiveTerrainParams(
	const FVector& Direction,
	float PlanetRadius,
	const FWorldModeTerrainParams& BaseParams,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot* BiomeSnapshot,
	float& OutContinentalness)
{
	return FInfinitePlaneWorldMode::ComputeEffectiveTerrainParamsAt(
		GetNoiseSamplePoint(Direction, PlanetRadius), BaseParams, NoiseParams, BiomeSnapshot, OutContinentalness);
}

float FSphericalPlanetWorldMode::SampleRadialDisplacement(
	const FVector& Direction,
	float PlanetRadius,
	const FWorldModeTerrainParams& TerrainParams,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot* BiomeSnapshot,
	float* OutContinentalness)
{
	float Continentalness = 0.0f;
	const FWorldModeTerrainParams Effective = ComputeEffectiveTerrainParams(
		Direction, PlanetRadius, TerrainParams, NoiseParams, BiomeSnapshot, Continentalness);
	if (OutContinentalness)
	{
		*OutContinentalness = Continentalness;
	}
	return NoiseToRadialDisplacement(SampleSphericalNoise(Direction, PlanetRadius, NoiseParams), Effective);
}

void FSphericalPlanetWorldMode::GetDisplacementBounds(
	const FWorldModeTerrainParams& TerrainParams,
	const FVoxelBiomeSnapshot* BiomeSnapshot,
	float& OutMin,
	float& OutMax)
{
	// Displacement = (BaseHeight + Offset(c)) + Noise * HeightScale * Mult(c) / 2, Noise in [-1, 1]. Decoupled extremes,
	// as FInfinitePlaneWorldMode::GetTerrainHeightBounds: a superset of what generation can produce.
	float MinOffset = 0.0f, MaxOffset = 0.0f, MaxScaleMult = 1.0f;
	if (BiomeSnapshot)
	{
		BiomeSnapshot->GetContinentalnessExtremes(MinOffset, MaxOffset, MaxScaleMult);
	}
	const float Amplitude = 0.5f * TerrainParams.HeightScale * MaxScaleMult;
	OutMin = TerrainParams.BaseHeight + MinOffset - Amplitude;
	OutMax = TerrainParams.BaseHeight + MaxOffset + Amplitude;
}

float FSphericalPlanetWorldMode::ShellSignedDistance(
	float DistFromCenter,
	float TerrainRadius,
	const FSphericalPlanetParams& PlanetParams)
{
	// Quick reject well outside the terrain shell: definitely air above it, definitely core below it.
	if (DistFromCenter > PlanetParams.GetOuterRadius() + 1000.0f)
	{
		return -1000.0f;
	}
	if (DistFromCenter < PlanetParams.GetInnerRadius() - 1000.0f)
	{
		return 1000.0f;
	}
	return CalculateSignedDistance(DistFromCenter, TerrainRadius);
}

FVector FSphericalPlanetWorldMode::ScaleAltitude(
	const FVector& WorldPos,
	const FSphericalPlanetParams& PlanetParams,
	float VerticalScale)
{
	const FVector ToPoint = WorldPos - PlanetParams.PlanetCenter;
	const double Dist = ToPoint.Size();
	if (Dist < UE_KINDA_SMALL_NUMBER)
	{
		return WorldPos;
	}
	const double Altitude = Dist - PlanetParams.PlanetRadius;
	return PlanetParams.PlanetCenter + ToPoint * ((PlanetParams.PlanetRadius + Altitude * VerticalScale) / Dist);
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
