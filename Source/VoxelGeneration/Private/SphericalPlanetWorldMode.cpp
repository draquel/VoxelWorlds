// Copyright Daniel Raquel. All Rights Reserved.

#include "SphericalPlanetWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelGeneration.h"
#include "VoxelCubeSphere.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelPlanetNoise.h"
#include "VoxelBiomeSnapshot.h"

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
	Bounds.MinZ = static_cast<float>(PlanetParams.PlanetCenter.Z - PlanetParams.GetOuterRadius());
	Bounds.MaxZ = static_cast<float>(PlanetParams.PlanetCenter.Z + PlanetParams.GetOuterRadius());
	// The space above the terrain belongs to the world up to the orbit ceiling (spherical Phase 6, D18): flight and the
	// orbit camera live there, and KillZ goes below it.
	const double Ceiling = PlanetParams.OrbitCeiling > 0.0 ? PlanetParams.OrbitCeiling : 3.0 * PlanetParams.PlanetRadius;
	Bounds.OrbitRadius = FMath::Max(PlanetParams.PlanetRadius + Ceiling, PlanetParams.GetOuterRadius());
	return Bounds;
}

FVector FSphericalPlanetWorldMode::GetUpVector(const FVector& WorldPos) const
{
	return GetDirectionFromCenter(WorldPos, PlanetParams.PlanetCenter);
}

float FSphericalPlanetWorldMode::GetAltitude(const FVector& WorldPos) const
{
	// The subtraction in double; only the altitude (the relief's size) is narrowed (WM-BP4).
	return static_cast<float>(CalculateRadialDistance(WorldPos, PlanetParams.PlanetCenter) - PlanetParams.PlanetRadius);
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
	// The surface point as centre + direction x (R + altitude), the sum in double (WM-BP4): a float R + altitude is 8 uu
	// coarse at R 1e8, which tilted the normal below by degrees.
	const FVector& Center = PlanetParams.PlanetCenter;
	const double Radius = PlanetParams.PlanetRadius;
	auto SurfaceAt = [&](const FVector& Direction)
	{
		return Center + Direction * (Radius + static_cast<double>(SampleRadialDisplacement(Direction, Radius, TerrainParams, NoiseParams, &BiomeSnapshot)));
	};

	const FVector Dir = GetDirectionFromCenter(WorldPos, Center);
	FVoxelTerrainSurfacePoint Point;
	Point.Position = SurfaceAt(Dir);

	// Central differences along two tangents, SurfaceNormalStep of arc either side on the datum sphere. (T1, T2, Dir) is
	// right-handed, so the cross product of the two differences points out of the terrain.
	const FVector T1 = FVector::CrossProduct(FMath::Abs(Dir.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector, Dir).GetSafeNormal();
	const FVector T2 = FVector::CrossProduct(Dir, T1);
	const double Angle = SurfaceNormalStep / FMath::Max(Radius, 1.0);
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
	const double Radius = PlanetParams.PlanetRadius;
	auto SurfaceAt = [&](const FVector& Direction)
	{
		return Center + Direction * (Radius + static_cast<double>(GetConditionedTerrainAltitude(Direction, NoiseParams, Zones)));
	};

	// GetSurfacePoint's construction over the conditioned surface.
	const FVector Dir = GetDirectionFromCenter(WorldPos, Center);
	FVoxelTerrainSurfacePoint Point;
	Point.Position = SurfaceAt(Dir);
	const FVector T1 = FVector::CrossProduct(FMath::Abs(Dir.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector, Dir).GetSafeNormal();
	const FVector T2 = FVector::CrossProduct(Dir, T1);
	const double Angle = SurfaceNormalStep / FMath::Max(Radius, 1.0);
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
	const double VoxelAltitude = CalculateRadialDistance(WorldPos, PlanetParams.PlanetCenter) - PlanetParams.PlanetRadius;
	return ShellSignedDistanceAtAltitude(VoxelAltitude, NoiseToRadialDisplacement(NoiseValue, TerrainParams), PlanetParams);
}

FVoxelTerrainSurfacePoint FSphericalPlanetWorldMode::GetSurfaceOnUpLine(const FVector& WorldPos, const FVoxelNoiseParams& NoiseParams) const
{
	// The point as centre + direction x (R + altitude), the sum in double (WM-BP4), as GetSurfacePoint forms it.
	const FVector Direction = GetDirectionFromCenter(WorldPos, PlanetParams.PlanetCenter);
	FVoxelTerrainSurfacePoint Point;
	Point.Altitude = GetTerrainAltitude(Direction, NoiseParams);
	Point.Position = PlanetParams.PlanetCenter + Direction * (static_cast<double>(PlanetParams.PlanetRadius) + static_cast<double>(Point.Altitude));
	Point.Normal = Direction;
	return Point;
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

double FSphericalPlanetWorldMode::CalculateRadialDistance(
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
	double PlanetRadius,
	const FVoxelNoiseParams& NoiseParams)
{
	// The full 3D point on the datum sphere: no hemisphere mirror, no seam, feature size in world units (SP-B1). Planet
	// noise (WM-BP5, D22): the point split in double per octave, a hashed lattice with no period.
	return FVoxelPlanetNoise::FBM3D(GetNoiseSamplePoint(Direction, PlanetRadius), NoiseParams);
}

FWorldModeTerrainParams FSphericalPlanetWorldMode::ComputeEffectiveTerrainParams(
	const FVector& Direction,
	double PlanetRadius,
	const FWorldModeTerrainParams& BaseParams,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot* BiomeSnapshot,
	float& OutContinentalness)
{
	// FInfinitePlaneWorldMode::ComputeEffectiveTerrainParamsAt's curves, with the continentalness field sampled by the
	// planet noise at the direction's point on the datum sphere (WM-BP5).
	OutContinentalness = 0.0f;
	FWorldModeTerrainParams Effective = BaseParams;
	if (BiomeSnapshot && BiomeSnapshot->bEnableContinentalness)
	{
		OutContinentalness = FVoxelPlanetNoise::FBM3D(GetNoiseSamplePoint(Direction, PlanetRadius),
			GetContinentalnessNoiseParams(NoiseParams.Seed, *BiomeSnapshot));
		float HeightOffset = 0.0f;
		float HeightScaleMult = 1.0f;
		BiomeSnapshot->GetContinentalnessTerrainParams(OutContinentalness, HeightOffset, HeightScaleMult);
		Effective.BaseHeight += HeightOffset;
		Effective.HeightScale *= HeightScaleMult;
	}
	return Effective;
}

FVoxelNoiseParams FSphericalPlanetWorldMode::GetContinentalnessNoiseParams(int32 WorldSeed, const FVoxelBiomeSnapshot& BiomeSnapshot)
{
	return FVoxelPlanetNoise::MakeFieldParams(WorldSeed + BiomeSnapshot.ContinentalnessSeedOffset, BiomeSnapshot.ContinentalnessNoiseFrequency);
}

void FSphericalPlanetWorldMode::SampleClimate(const FVector& Direction, double PlanetRadius, const FVoxelNoiseParams& TemperatureParams,
	const FVoxelNoiseParams& MoistureParams, float& OutTemperature, float& OutMoisture)
{
	const FVector SamplePoint = GetNoiseSamplePoint(Direction, PlanetRadius);
	OutTemperature = FVoxelPlanetNoise::FBM3D(SamplePoint, TemperatureParams);
	OutMoisture = FVoxelPlanetNoise::FBM3D(SamplePoint, MoistureParams);
}

FVector FSphericalPlanetWorldMode::ScaleAltitudeRelative(const FVector& PlanetRelativePos, double PlanetRadius, float VerticalScale)
{
	const double Dist = PlanetRelativePos.Size();
	if (Dist < UE_KINDA_SMALL_NUMBER)
	{
		return PlanetRelativePos;
	}
	const double Altitude = Dist - PlanetRadius;
	return PlanetRelativePos * ((PlanetRadius + Altitude * VerticalScale) / Dist);
}

float FSphericalPlanetWorldMode::SampleRadialDisplacement(
	const FVector& Direction,
	double PlanetRadius,
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
	double DistFromCenter,
	double TerrainRadius,
	const FSphericalPlanetParams& PlanetParams)
{
	return ShellSignedDistanceAtAltitude(DistFromCenter - PlanetParams.PlanetRadius, TerrainRadius - PlanetParams.PlanetRadius, PlanetParams);
}

float FSphericalPlanetWorldMode::ShellSignedDistanceAtAltitude(
	double VoxelAltitude,
	double TerrainAltitude,
	const FSphericalPlanetParams& PlanetParams)
{
	// Quick reject well outside the terrain shell: definitely air above it, definitely core below it.
	if (VoxelAltitude > static_cast<double>(PlanetParams.MaxTerrainHeight) + 1000.0)
	{
		return -1000.0f;
	}
	if (VoxelAltitude < -static_cast<double>(PlanetParams.MaxTerrainDepth) - 1000.0)
	{
		return 1000.0f;
	}
	return static_cast<float>(TerrainAltitude - VoxelAltitude);
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
	double DistFromCenter,
	double TerrainRadius)
{
	// Signed distance: positive when inside solid (below terrain), negative when in air. Formed in double: both radii are
	// of size R.
	return static_cast<float>(TerrainRadius - DistFromCenter);
}

bool FSphericalPlanetWorldMode::IsWithinPlanetBounds(
	const FVector& WorldPos,
	const FSphericalPlanetParams& PlanetParams)
{
	const double Distance = CalculateRadialDistance(WorldPos, PlanetParams.PlanetCenter);
	return Distance >= PlanetParams.GetInnerRadius() && Distance <= PlanetParams.GetOuterRadius();
}
