// Copyright Daniel Raquel. All Rights Reserved.

#include "SphericalPlanetWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelGeneration.h"
#include "VoxelCubeSphere.h"
#include "VoxelTerrainConditioning.h"

#include <atomic>

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
	// The space above the terrain belongs to the world up to the orbit ceiling (spherical Phase 6, D18): flight and the
	// orbit camera live there, and KillZ goes below it.
	const double Ceiling = PlanetParams.OrbitCeiling > 0.0 ? PlanetParams.OrbitCeiling : 3.0 * PlanetParams.PlanetRadius;
	Bounds.OrbitRadius = FMath::Max(static_cast<double>(PlanetParams.PlanetRadius) + Ceiling, static_cast<double>(Bounds.OuterRadius));
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
	// The cube faces and the tangent warp (FVoxelCubeSphere, WORLD_MODES_REVIEW_PLAN.md §15.1).
	return FVoxelCubeSphere::DirectionToSurface(GetDirectionFromCenter(WorldPos, PlanetParams.PlanetCenter));
}

FVector FSphericalPlanetWorldMode::SurfaceToWorld(const FVoxelSurfaceCoord& Coord, float Altitude) const
{
	return PlanetParams.PlanetCenter + FVoxelCubeSphere::SurfaceToDirection(Coord.Face, Coord.UV) * (PlanetParams.PlanetRadius + Altitude);
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

FVoxelTerrainSurfacePoint FSphericalPlanetWorldMode::GetConditionedSurfacePoint(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams,
	const TArray<FVoxelConditioningZone>& Zones) const
{
	if (Zones.Num() == 0)
	{
		return GetSurfacePoint(WorldPos, NoiseParams);
	}
	const FVector& Center = PlanetParams.PlanetCenter;
	const float Radius = PlanetParams.PlanetRadius;
	auto SurfaceAt = [&](const FVector& Direction)
	{
		return Center + Direction * (Radius + GetConditionedTerrainAltitude(Direction, NoiseParams, Zones));
	};

	// GetSurfacePoint's construction over the conditioned surface.
	const FVector Dir = GetDirectionFromCenter(WorldPos, Center);
	FVoxelTerrainSurfacePoint Point;
	Point.Position = SurfaceAt(Dir);
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

float FSphericalPlanetWorldMode::GetConditionedTerrainAltitude(const FVector& Direction, const FVoxelNoiseParams& NoiseParams,
	const TArray<FVoxelConditioningZone>& Zones) const
{
	const float Natural = GetTerrainAltitude(Direction, NoiseParams);
	return Zones.Num() > 0 ? FVoxelTerrainConditioning::ApplyToAltitude(Direction, PlanetParams.PlanetRadius, Natural, Zones) : Natural;
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
	// Legacy heightmap query (spherical SP-A3, Phase 4): every planet consumer uses GetSurfacePoint. A stray caller gets
	// the world Z where the vertical line through (X, Y) meets the terrain on the upper hemisphere, not a radius.
	static std::atomic<bool> bWarned{false};
	if (!bWarned.exchange(true))
	{
		UE_LOG(LogVoxelGeneration, Warning, TEXT("FSphericalPlanetWorldMode::GetTerrainHeightAt called on a planet: it returns the terrain's Z "
			"on the upper hemisphere above (X, Y). Use GetSurfacePoint / GetUpVector / GetAltitude (spherical SP-A3)."));
	}

	const FVector& Center = PlanetParams.PlanetCenter;
	const double DX = static_cast<double>(X) - Center.X;
	const double DY = static_cast<double>(Y) - Center.Y;
	const double Rho2 = DX * DX + DY * DY;
	const double Outer = PlanetParams.GetOuterRadius();
	if (Rho2 >= Outer * Outer)
	{
		return static_cast<float>(Center.Z); // the line passes outside the terrain shell
	}

	// Along the line z (height above the centre), f(z) = |P(z) - C| - TerrainRadius(direction of P(z)) is positive
	// above the terrain and negative below. March down from where the line enters the shell to the first point below the
	// terrain (the topmost crossing), then bisect.
	auto Above = [&](double Z) -> double
	{
		const FVector Direction = FVector(DX, DY, Z).GetSafeNormal();
		const double TerrainRadius = PlanetParams.PlanetRadius
			+ SampleRadialDisplacement(Direction.IsNearlyZero() ? FVector::UpVector : Direction, PlanetParams.PlanetRadius,
				TerrainParams, NoiseParams, &BiomeSnapshot);
		return FMath::Sqrt(Rho2 + Z * Z) - TerrainRadius;
	};
	constexpr double MarchStep = 200.0;
	double High = FMath::Sqrt(Outer * Outer - Rho2);
	double Low = High;
	bool bCrossed = false;
	while (Low > 0.0)
	{
		Low = FMath::Max(0.0, Low - MarchStep);
		if (Above(Low) <= 0.0)
		{
			bCrossed = true;
			break;
		}
		High = Low;
	}
	if (!bCrossed)
	{
		return static_cast<float>(Center.Z); // the line meets no terrain on the upper hemisphere
	}
	for (int32 Iteration = 0; Iteration < 40; ++Iteration)
	{
		const double Mid = 0.5 * (Low + High);
		(Above(Mid) <= 0.0 ? Low : High) = Mid;
	}
	return static_cast<float>(Center.Z + 0.5 * (Low + High));
}

void FSphericalPlanetWorldMode::GetTerrainHeightBounds(float& OutMin, float& OutMax) const
{
	OutMin = static_cast<float>(PlanetParams.PlanetCenter.Z) - PlanetParams.GetOuterRadius();
	OutMax = static_cast<float>(PlanetParams.PlanetCenter.Z) + PlanetParams.GetOuterRadius();
}

void FSphericalPlanetWorldMode::GetAltitudeBounds(float& OutMin, float& OutMax) const
{
	OutMin = -PlanetParams.MaxTerrainDepth;
	OutMax = PlanetParams.MaxTerrainHeight;
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
