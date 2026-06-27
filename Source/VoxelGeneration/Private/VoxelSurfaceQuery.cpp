// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSurfaceQuery.h"
#include "IVoxelWorldMode.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"

float FVoxelSurfaceQuery::GetSurfaceHeight(
	const IVoxelWorldMode& WorldMode,
	float WorldX, float WorldY,
	const FVoxelNoiseParams& NoiseParams)
{
	return WorldMode.GetTerrainHeightAt(WorldX, WorldY, NoiseParams);
}

float FVoxelSurfaceQuery::ComputeSlopeDegrees(
	const IVoxelWorldMode& WorldMode,
	float WorldX, float WorldY, float VoxelSize,
	const FVoxelNoiseParams& NoiseParams)
{
	// Sample terrain height at 4 neighboring positions to compute gradient
	const float Step = VoxelSize;

	const float HX0 = WorldMode.GetTerrainHeightAt(WorldX - Step, WorldY, NoiseParams);
	const float HX1 = WorldMode.GetTerrainHeightAt(WorldX + Step, WorldY, NoiseParams);
	const float HY0 = WorldMode.GetTerrainHeightAt(WorldX, WorldY - Step, NoiseParams);
	const float HY1 = WorldMode.GetTerrainHeightAt(WorldX, WorldY + Step, NoiseParams);

	// Central difference gradient
	const float DX = (HX1 - HX0) / (2.0f * Step);
	const float DY = (HY1 - HY0) / (2.0f * Step);

	// Slope angle from gradient magnitude
	const float GradientMag = FMath::Sqrt(DX * DX + DY * DY);
	return FMath::RadiansToDegrees(FMath::Atan(GradientMag));
}

FVector FVoxelSurfaceQuery::ComputeSurfaceNormal(
	const IVoxelWorldMode& WorldMode,
	float WorldX, float WorldY, float VoxelSize,
	const FVoxelNoiseParams& NoiseParams)
{
	const float Step = VoxelSize;

	const float HX0 = WorldMode.GetTerrainHeightAt(WorldX - Step, WorldY, NoiseParams);
	const float HX1 = WorldMode.GetTerrainHeightAt(WorldX + Step, WorldY, NoiseParams);
	const float HY0 = WorldMode.GetTerrainHeightAt(WorldX, WorldY - Step, NoiseParams);
	const float HY1 = WorldMode.GetTerrainHeightAt(WorldX, WorldY + Step, NoiseParams);

	const float DX = (HX1 - HX0) / (2.0f * Step);
	const float DY = (HY1 - HY0) / (2.0f * Step);

	// For a height field z = h(x,y), the surface normal is (-dh/dx, -dh/dy, 1) normalized.
	return FVector(-DX, -DY, 1.0f).GetSafeNormal();
}

void FVoxelSurfaceQuery::QuerySurfaceConditions(
	float WorldX, float WorldY, float TerrainHeight, float VoxelSize,
	const UVoxelBiomeConfiguration* BiomeConfig,
	int32 WorldSeed,
	bool bEnableWaterLevel, float WaterLevel,
	uint8& OutSurfaceMaterial, uint8& OutBiomeID)
{
	OutSurfaceMaterial = 0;
	OutBiomeID = 0;

	if (!BiomeConfig || !BiomeConfig->IsValid())
	{
		return;
	}

	// Sample temperature and moisture noise (same as VoxelCPUNoiseGenerator)
	FVoxelNoiseParams TempNoiseParams;
	TempNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	TempNoiseParams.Octaves = 2;
	TempNoiseParams.Persistence = 0.5f;
	TempNoiseParams.Lacunarity = 2.0f;
	TempNoiseParams.Amplitude = 1.0f;
	TempNoiseParams.Seed = WorldSeed + BiomeConfig->TemperatureSeedOffset;
	TempNoiseParams.Frequency = BiomeConfig->TemperatureNoiseFrequency;

	FVoxelNoiseParams MoistureNoiseParams;
	MoistureNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	MoistureNoiseParams.Octaves = 2;
	MoistureNoiseParams.Persistence = 0.5f;
	MoistureNoiseParams.Lacunarity = 2.0f;
	MoistureNoiseParams.Amplitude = 1.0f;
	MoistureNoiseParams.Seed = WorldSeed + BiomeConfig->MoistureSeedOffset;
	MoistureNoiseParams.Frequency = BiomeConfig->MoistureNoiseFrequency;

	// Sample at this world position (Z=0 for 2D biome sampling)
	const FVector BiomeSamplePos(WorldX, WorldY, 0.0f);
	const float Temperature = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, TempNoiseParams);
	const float Moisture = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, MoistureNoiseParams);

	// Sample continentalness noise if enabled
	float Continentalness = 0.0f;
	if (BiomeConfig->bEnableContinentalness)
	{
		FVoxelNoiseParams ContNoiseParams;
		ContNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		ContNoiseParams.Octaves = 2;
		ContNoiseParams.Persistence = 0.5f;
		ContNoiseParams.Lacunarity = 2.0f;
		ContNoiseParams.Amplitude = 1.0f;
		ContNoiseParams.Seed = WorldSeed + BiomeConfig->ContinentalnessSeedOffset;
		ContNoiseParams.Frequency = BiomeConfig->ContinentalnessNoiseFrequency;
		Continentalness = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, ContNoiseParams);
	}

	// Select biome (now with continentalness for proper tiered gating)
	FBiomeBlend Blend = BiomeConfig->GetBiomeBlend(Temperature, Moisture, Continentalness);
	OutBiomeID = Blend.GetDominantBiome();

	// Get surface material (depth = 0 for surface)
	const bool bIsUnderwater = bEnableWaterLevel && TerrainHeight < WaterLevel;
	if (bIsUnderwater)
	{
		OutSurfaceMaterial = BiomeConfig->GetBlendedMaterialWithWater(Blend, 0.0f, TerrainHeight, WaterLevel);
	}
	else
	{
		OutSurfaceMaterial = BiomeConfig->GetBlendedMaterial(Blend, 0.0f);
	}

	// Apply height material rules (snow on peaks, etc.)
	if (BiomeConfig->bEnableHeightMaterials)
	{
		OutSurfaceMaterial = BiomeConfig->ApplyHeightMaterialRules(OutSurfaceMaterial, TerrainHeight, 0.0f);
	}
}

FVoxelSurfaceSample FVoxelSurfaceQuery::SampleSurface(
	const IVoxelWorldMode& WorldMode,
	float WorldX, float WorldY, float VoxelSize,
	const FVoxelNoiseParams& NoiseParams,
	const UVoxelBiomeConfiguration* BiomeConfig,
	int32 WorldSeed,
	bool bEnableWaterLevel, float WaterLevel)
{
	FVoxelSurfaceSample Sample;

	// Center height + 4-neighbor heights (compute gradient once, derive slope and normal).
	const float Step = VoxelSize;
	Sample.Height = WorldMode.GetTerrainHeightAt(WorldX, WorldY, NoiseParams);

	const float HX0 = WorldMode.GetTerrainHeightAt(WorldX - Step, WorldY, NoiseParams);
	const float HX1 = WorldMode.GetTerrainHeightAt(WorldX + Step, WorldY, NoiseParams);
	const float HY0 = WorldMode.GetTerrainHeightAt(WorldX, WorldY - Step, NoiseParams);
	const float HY1 = WorldMode.GetTerrainHeightAt(WorldX, WorldY + Step, NoiseParams);

	const float DX = (HX1 - HX0) / (2.0f * Step);
	const float DY = (HY1 - HY0) / (2.0f * Step);

	const float GradientMag = FMath::Sqrt(DX * DX + DY * DY);
	Sample.SlopeDegrees = FMath::RadiansToDegrees(FMath::Atan(GradientMag));
	Sample.Normal = FVector(-DX, -DY, 1.0f).GetSafeNormal();

	QuerySurfaceConditions(
		WorldX, WorldY, Sample.Height, VoxelSize,
		BiomeConfig, WorldSeed, bEnableWaterLevel, WaterLevel,
		Sample.MaterialID, Sample.BiomeID);

	return Sample;
}
