// Copyright Daniel Raquel. All Rights Reserved.

#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelBiomeConfiguration.h"
#include "HAL/IConsoleManager.h"

// When 1 (default), the analytic terrain-height query (GetTerrainHeightAt) applies the same
// continentalness height modulation as generation, so spawn / nav / POI placement matches the real
// generated surface. Set to 0 to restore the legacy raw base-noise height (A/B or emergency revert).
static int32 GVoxelAnalyticContinentalness = 1;
static FAutoConsoleVariableRef CVarVoxelAnalyticContinentalness(
	TEXT("voxel.Height.AnalyticContinentalness"),
	GVoxelAnalyticContinentalness,
	TEXT("1 (default): analytic GetTerrainHeightAt applies continentalness modulation to match the ")
	TEXT("generated surface (spawn/nav/POI). 0: legacy raw base-noise height."),
	ECVF_Default);

FInfinitePlaneWorldMode::FInfinitePlaneWorldMode()
	: TerrainParams()
{
}

FInfinitePlaneWorldMode::FInfinitePlaneWorldMode(const FWorldModeTerrainParams& InTerrainParams)
	: TerrainParams(InTerrainParams)
{
}

float FInfinitePlaneWorldMode::GetDensityAt(
	const FVector& WorldPos,
	int32 LODLevel,
	float NoiseValue) const
{
	// For InfinitePlane, we interpret NoiseValue as terrain height noise
	// Calculate terrain height at this X,Y from the noise value
	float TerrainHeight = NoiseToTerrainHeight(NoiseValue, TerrainParams);

	// Calculate signed distance to surface
	// Positive = below surface (solid), Negative = above surface (air)
	return CalculateSignedDistance(WorldPos.Z, TerrainHeight);
}

float FInfinitePlaneWorldMode::GetTerrainHeightAt(
	float X,
	float Y,
	const FVoxelNoiseParams& NoiseParams) const
{
	// Sample 2D noise at this X,Y position
	float NoiseValue = SampleTerrainNoise2D(X, Y, NoiseParams);

	// Match generation: modulate the terrain params by continentalness so the analytic height equals
	// the real generated surface (spawn / nav / POI placement). Gated so it can be reverted at runtime.
	// Generation applies continentalness whenever a biome config has it enabled; mirror that here.
	const UVoxelBiomeConfiguration* Ctx = (GVoxelAnalyticContinentalness != 0) ? BiomeContext : nullptr;
	float Continentalness = 0.0f;
	const FWorldModeTerrainParams EffectiveParams =
		ComputeEffectiveTerrainParams(X, Y, TerrainParams, NoiseParams, Ctx, Continentalness);

	// Convert to terrain height
	return NoiseToTerrainHeight(NoiseValue, EffectiveParams);
}

FIntVector FInfinitePlaneWorldMode::WorldToChunkCoord(
	const FVector& WorldPos,
	int32 ChunkSize,
	float VoxelSize) const
{
	float ChunkWorldSize = ChunkSize * VoxelSize;

	return FIntVector(
		FMath::FloorToInt(WorldPos.X / ChunkWorldSize),
		FMath::FloorToInt(WorldPos.Y / ChunkWorldSize),
		FMath::FloorToInt(WorldPos.Z / ChunkWorldSize)
	);
}

FVector FInfinitePlaneWorldMode::ChunkCoordToWorld(
	const FIntVector& ChunkCoord,
	int32 ChunkSize,
	float VoxelSize,
	int32 LODLevel) const
{
	float ChunkWorldSize = ChunkSize * VoxelSize * FMath::Pow(2.0f, static_cast<float>(LODLevel));

	return FVector(ChunkCoord) * ChunkWorldSize;
}

int32 FInfinitePlaneWorldMode::GetMinZ() const
{
	return MIN_Z_CHUNKS;
}

int32 FInfinitePlaneWorldMode::GetMaxZ() const
{
	return MAX_Z_CHUNKS;
}

uint8 FInfinitePlaneWorldMode::GetMaterialAtDepth(
	const FVector& WorldPos,
	float SurfaceHeight,
	float DepthBelowSurface) const
{
	// Material IDs:
	// 0 = Grass (at surface)
	// 1 = Dirt (1-3 voxel depths below surface)
	// 2 = Stone (deep underground)

	if (DepthBelowSurface <= 0.0f)
	{
		// At or above surface - air (no material)
		return 0;
	}
	else if (DepthBelowSurface < 100.0f) // ~1 voxel at default size
	{
		// Near surface - grass
		return 0;
	}
	else if (DepthBelowSurface < 400.0f) // ~1-4 voxels
	{
		// Subsurface - dirt
		return 1;
	}
	else
	{
		// Deep underground - stone
		return 2;
	}
}

// ==================== Static Helpers ====================

float FInfinitePlaneWorldMode::SampleTerrainNoise2D(
	float X,
	float Y,
	const FVoxelNoiseParams& NoiseParams)
{
	// Use FBM3D but with Z=0 for 2D heightmap sampling
	// This ensures consistency with the existing noise implementation
	FVector Position2D(X, Y, 0.0f);
	return FVoxelCPUNoiseGenerator::FBM3D(Position2D, NoiseParams);
}

float FInfinitePlaneWorldMode::NoiseToTerrainHeight(
	float NoiseValue,
	const FWorldModeTerrainParams& TerrainParams)
{
	// Noise value is in range [-1, 1]:
	//   TerrainHeight = SeaLevel + BaseHeight + (NoiseValue * HeightScale)
	//
	// Intentionally UNCLAMPED, to stay bit-identical with the GPU generator's NoiseToTerrainHeight
	// (Shaders/Private/WorldModeSDF.ush), which applies no height clamp. A former CPU-only clamp to
	// [MinHeight,MaxHeight] silently clipped peaks/valleys the GPU still generated, so the analytic
	// height diverged from the real (GPU) surface above ~10000 uu. The height range is bounded by the
	// caller's HeightScale (+ continentalness); chunk generation is bounded by GetMinZ()/GetMaxZ().
	return TerrainParams.SeaLevel + TerrainParams.BaseHeight + (NoiseValue * TerrainParams.HeightScale);
}

FWorldModeTerrainParams FInfinitePlaneWorldMode::ComputeEffectiveTerrainParams(
	float X,
	float Y,
	const FWorldModeTerrainParams& BaseParams,
	const FVoxelNoiseParams& BaseNoiseParams,
	const UVoxelBiomeConfiguration* BiomeConfig,
	float& OutContinentalness)
{
	OutContinentalness = 0.0f;
	FWorldModeTerrainParams Effective = BaseParams;

	// Continentalness modulates height independently of biome material selection (matches
	// FVoxelCPUNoiseGenerator::GenerateChunkInfinitePlane, which gates only on bEnableContinentalness).
	if (BiomeConfig && BiomeConfig->bEnableContinentalness)
	{
		// Same continentalness noise field as generation: 2-octave Simplex, seed offset from the config.
		FVoxelNoiseParams ContinentalnessNoiseParams;
		ContinentalnessNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		ContinentalnessNoiseParams.Octaves = 2;
		ContinentalnessNoiseParams.Persistence = 0.5f;
		ContinentalnessNoiseParams.Lacunarity = 2.0f;
		ContinentalnessNoiseParams.Amplitude = 1.0f;
		ContinentalnessNoiseParams.Seed = BaseNoiseParams.Seed + BiomeConfig->ContinentalnessSeedOffset;
		ContinentalnessNoiseParams.Frequency = BiomeConfig->ContinentalnessNoiseFrequency;

		const FVector SamplePos(X, Y, 0.0f);
		OutContinentalness = FVoxelCPUNoiseGenerator::FBM3D(SamplePos, ContinentalnessNoiseParams);

		float HeightOffset = 0.0f;
		float HeightScaleMult = 1.0f;
		BiomeConfig->GetContinentalnessTerrainParams(OutContinentalness, HeightOffset, HeightScaleMult);
		Effective.BaseHeight += HeightOffset;
		Effective.HeightScale *= HeightScaleMult;
	}

	return Effective;
}

float FInfinitePlaneWorldMode::CalculateSignedDistance(
	float WorldZ,
	float TerrainHeight)
{
	// Signed distance: positive = inside terrain (solid), negative = outside (air)
	// Distance = TerrainHeight - WorldZ
	// If WorldZ < TerrainHeight, we're below surface (positive distance = solid)
	// If WorldZ > TerrainHeight, we're above surface (negative distance = air)
	return TerrainHeight - WorldZ;
}

uint8 FInfinitePlaneWorldMode::SignedDistanceToDensity(
	float SignedDistance,
	float VoxelSize)
{
	// Convert signed distance to density [0-255]
	// 127 = surface threshold
	// <127 = air, >127 = solid

	// Normalize by voxel size for smooth density falloff
	// A distance of +/- VoxelSize maps to density extremes
	float NormalizedDistance = SignedDistance / VoxelSize;

	// Clamp to [-1, 1] range
	float Clamped = FMath::Clamp(NormalizedDistance, -1.0f, 1.0f);

	// Map to [0, 255] where 127 is surface
	// Positive (solid/inside) maps to 127-255
	// Negative (air/outside) maps to 0-127
	float Density = (Clamped + 1.0f) * 127.5f;

	return static_cast<uint8>(FMath::Clamp(Density, 0.0f, 255.0f));
}
