// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSurfaceQuery.h"
#include "IVoxelWorldMode.h"
#include "VoxelBiomeSnapshot.h"
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
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	int32 WorldSeed,
	bool bEnableWaterLevel, float WaterLevel,
	uint8& OutSurfaceMaterial, uint8& OutBiomeID)
{
	OutSurfaceMaterial = 0;
	OutBiomeID = 0;

	if (!BiomeSnapshot.bIsValid)
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
	TempNoiseParams.Seed = WorldSeed + BiomeSnapshot.TemperatureSeedOffset;
	TempNoiseParams.Frequency = BiomeSnapshot.TemperatureNoiseFrequency;

	FVoxelNoiseParams MoistureNoiseParams;
	MoistureNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	MoistureNoiseParams.Octaves = 2;
	MoistureNoiseParams.Persistence = 0.5f;
	MoistureNoiseParams.Lacunarity = 2.0f;
	MoistureNoiseParams.Amplitude = 1.0f;
	MoistureNoiseParams.Seed = WorldSeed + BiomeSnapshot.MoistureSeedOffset;
	MoistureNoiseParams.Frequency = BiomeSnapshot.MoistureNoiseFrequency;

	// Sample at this world position (Z=0 for 2D biome sampling)
	const FVector BiomeSamplePos(WorldX, WorldY, 0.0f);
	const float Temperature = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, TempNoiseParams);
	const float Moisture = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, MoistureNoiseParams);

	// Sample continentalness noise if enabled
	float Continentalness = 0.0f;
	if (BiomeSnapshot.bEnableContinentalness)
	{
		FVoxelNoiseParams ContNoiseParams;
		ContNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		ContNoiseParams.Octaves = 2;
		ContNoiseParams.Persistence = 0.5f;
		ContNoiseParams.Lacunarity = 2.0f;
		ContNoiseParams.Amplitude = 1.0f;
		ContNoiseParams.Seed = WorldSeed + BiomeSnapshot.ContinentalnessSeedOffset;
		ContNoiseParams.Frequency = BiomeSnapshot.ContinentalnessNoiseFrequency;
		Continentalness = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, ContNoiseParams);
	}

	// Select biome (now with continentalness for proper tiered gating)
	FBiomeBlend Blend = BiomeSnapshot.GetBiomeBlend(Temperature, Moisture, Continentalness);
	OutBiomeID = Blend.GetDominantBiome();

	// Get surface material (depth = 0 for surface)
	const bool bIsUnderwater = bEnableWaterLevel && TerrainHeight < WaterLevel;
	if (bIsUnderwater)
	{
		OutSurfaceMaterial = BiomeSnapshot.GetBlendedMaterialWithWater(Blend, 0.0f, TerrainHeight, WaterLevel);
	}
	else
	{
		OutSurfaceMaterial = BiomeSnapshot.GetBlendedMaterial(Blend, 0.0f);
	}

	// Apply height material rules (snow on peaks, etc.)
	OutSurfaceMaterial = BiomeSnapshot.ApplyHeightMaterialRules(OutSurfaceMaterial, TerrainHeight, 0.0f);
}

FVoxelSurfaceSample FVoxelSurfaceQuery::SampleSurface(
	const IVoxelWorldMode& WorldMode,
	float WorldX, float WorldY, float VoxelSize,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
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
		BiomeSnapshot, WorldSeed, bEnableWaterLevel, WaterLevel,
		Sample.MaterialID, Sample.BiomeID);

	return Sample;
}

bool FVoxelSurfaceQuery::ExtractSurfaceFromColumn(
	const TArray<FVoxelData>& ColumnLowToHigh,
	float BaseZ, float VoxelSize,
	float& OutHeight, uint8& OutMaterialID, uint8& OutBiomeID)
{
	const int32 Num = ColumnLowToHigh.Num();
	if (Num < 2)
	{
		return false;
	}

	// Scan top-down for the highest solid voxel that has air directly above it (= the top surface).
	for (int32 i = Num - 2; i >= 0; --i)
	{
		const FVoxelData& Cur = ColumnLowToHigh[i];
		const FVoxelData& Above = ColumnLowToHigh[i + 1];
		if (Cur.IsSolid() && Above.IsAir())
		{
			// Density crosses the surface threshold between voxel i (>=threshold) and i+1 (<threshold).
			const float D0 = static_cast<float>(Cur.Density);
			const float D1 = static_cast<float>(Above.Density);
			const float Denom = D1 - D0;
			float T = 0.5f;
			if (FMath::Abs(Denom) > KINDA_SMALL_NUMBER)
			{
				T = FMath::Clamp((static_cast<float>(VOXEL_SURFACE_THRESHOLD) - D0) / Denom, 0.0f, 1.0f);
			}

			OutHeight = BaseZ + (static_cast<float>(i) + T) * VoxelSize;
			OutMaterialID = Cur.MaterialID;
			OutBiomeID = Cur.BiomeID;
			return true;
		}
	}

	return false;
}
