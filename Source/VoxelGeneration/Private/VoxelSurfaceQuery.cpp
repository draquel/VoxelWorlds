// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelSurfaceQuery.h"
#include "IVoxelWorldMode.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelWaterLevels.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelNoise.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelCaveConfiguration.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeBlendDither.h"

namespace
{
	/** The natural height at a heightmap (X, Y), as the modes sample it (the X and Y floats). */
	FORCEINLINE float NaturalHeight(const IVoxelWorldMode& WorldMode, float WorldX, float WorldY, const FVoxelNoiseParams& NoiseParams)
	{
		return static_cast<float>(WorldMode.GetSurfaceOnUpLine(FVector(WorldX, WorldY, 0.0), NoiseParams).Position.Z);
	}

	/** The water over a column, as the material rules take it: whether a body covers it, and that body's level there. */
	struct FColumnWater
	{
		bool bWater = false;
		/** World Z, or the distance from the planet centre (FVoxelWaterBody::Level) */
		double Level = 0.0;
	};

	/** The water levels' sample at a position, reduced to what the surface's material needs. */
	FORCEINLINE FColumnWater WaterOver(const FVoxelWaterLevels& Water, const FVector& WorldPos)
	{
		FVoxelWaterSample Sample;
		FColumnWater Over;
		Over.bWater = Water.Sample(WorldPos, Sample);
		Over.Level = Sample.SurfaceLevel;
		return Over;
	}
}

void FVoxelSurfaceQuery::SampleHeightmapClimate(
	float WorldX, float WorldY, float TerrainHeight,
	int32 WorldSeed,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	float& OutTemperature, float& OutMoisture, float& OutContinentalness)
{
	OutTemperature = 0.0f;
	OutMoisture = 0.0f;
	OutContinentalness = 0.0f;
	if (!BiomeSnapshot.bIsValid)
	{
		return;
	}
	// The fields the CPU generator samples (FVoxelNoise::MakeFieldParams: simplex, two octaves), at (X, Y, 0): the climate is 2D.
	const FVector BiomeSamplePos(WorldX, WorldY, 0.0f);
	OutTemperature = BiomeSnapshot.ShapeTemperature(
		FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, FVoxelNoise::MakeFieldParams(WorldSeed + BiomeSnapshot.TemperatureSeedOffset, BiomeSnapshot.TemperatureNoiseFrequency)),
		TerrainHeight, 0.0f, false);
	OutMoisture = BiomeSnapshot.ShapeMoisture(
		FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, FVoxelNoise::MakeFieldParams(WorldSeed + BiomeSnapshot.MoistureSeedOffset, BiomeSnapshot.MoistureNoiseFrequency)));
	if (BiomeSnapshot.bEnableContinentalness)
	{
		OutContinentalness = BiomeSnapshot.ShapeContinentalness(
			FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, FVoxelNoise::MakeFieldParams(WorldSeed + BiomeSnapshot.ContinentalnessSeedOffset, BiomeSnapshot.ContinentalnessNoiseFrequency)));
	}
}

void FVoxelSurfaceQuery::SamplePlanetClimate(
	const FVector& Direction,
	double PlanetRadius,
	float TerrainAltitude,
	int32 WorldSeed,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	float& OutTemperature, float& OutMoisture)
{
	OutTemperature = 0.0f;
	OutMoisture = 0.0f;
	if (!BiomeSnapshot.bIsValid)
	{
		return;
	}
	FSphericalPlanetWorldMode::SampleClimate(Direction, PlanetRadius,
		FVoxelNoise::MakeFieldParams(WorldSeed + BiomeSnapshot.TemperatureSeedOffset, BiomeSnapshot.TemperatureNoiseFrequency),
		FVoxelNoise::MakeFieldParams(WorldSeed + BiomeSnapshot.MoistureSeedOffset, BiomeSnapshot.MoistureNoiseFrequency),
		OutTemperature, OutMoisture);
	OutTemperature = BiomeSnapshot.ShapeTemperature(OutTemperature, TerrainAltitude, static_cast<float>(Direction.Z), true);
	OutMoisture = BiomeSnapshot.ShapeMoisture(OutMoisture);
}

bool FVoxelSurfaceQuery::QueryClimateAt(
	const IVoxelWorldMode& WorldMode,
	const FVector& WorldPos,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	const FVoxelConditioning* Conditioning,
	FVoxelClimateSample& OutSample)
{
	OutSample = FVoxelClimateSample();
	float Continentalness = 0.0f;
	const FVoxelTerrainSurfacePoint Surface = GetGeneratedSurface(WorldMode, WorldPos, NoiseParams, Conditioning, &Continentalness);
	OutSample.SurfaceAltitude = Surface.Altitude;
	OutSample.Up = WorldMode.GetUpVector(WorldPos);
	OutSample.bValid = BiomeSnapshot.bIsValid;
	if (!OutSample.bValid)
	{
		return false;
	}
	if (WorldMode.GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		// A heightmap column: the fields at (X, Y) with the surface's height; the seed is the noise's (the configuration's world seed).
		SampleHeightmapClimate(static_cast<float>(WorldPos.X), static_cast<float>(WorldPos.Y), static_cast<float>(Surface.Position.Z), NoiseParams.Seed,
			BiomeSnapshot, OutSample.Temperature, OutSample.Moisture, OutSample.Continentalness);
	}
	else
	{
		// A planet: the fields along the direction at the terrain's altitude; the continentalness the radial height sampled.
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(WorldMode);
		const FVector Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, Planet.GetPlanetParams().PlanetCenter);
		OutSample.SinLatitude = static_cast<float>(Direction.Z);
		OutSample.Continentalness = Continentalness;
		SamplePlanetClimate(Direction, Planet.GetPlanetParams().PlanetRadius, Surface.Altitude, NoiseParams.Seed, BiomeSnapshot,
			OutSample.Temperature, OutSample.Moisture);
	}
	OutSample.BiomeID = BiomeSnapshot.GetBiomeBlend(OutSample.Temperature, OutSample.Moisture, OutSample.Continentalness).GetDominantBiome();
	return true;
}

FVoxelTerrainSurfacePoint FVoxelSurfaceQuery::GetGeneratedSurface(
	const IVoxelWorldMode& WorldMode,
	const FVector& WorldPos,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelConditioning* Conditioning,
	float* OutContinentalness)
{
	const bool bConditioned = Conditioning && !Conditioning->IsEmpty();
	if (WorldMode.GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		// The natural height with the heightmap zones layered on as the generator does (G23).
		FVoxelTerrainSurfacePoint Point = WorldMode.GetSurfaceOnUpLine(WorldPos, NoiseParams);
		if (bConditioned)
		{
			Point.Position.Z = Conditioning->ApplyToHeight(WorldPos.X, WorldPos.Y, static_cast<float>(Point.Position.Z));
			Point.Altitude = WorldMode.GetAltitude(Point.Position);
		}
		if (OutContinentalness)
		{
			*OutContinentalness = 0.0f;
		}
		return Point;
	}

	// A planet: the radial height (its continentalness kept for the blend) with the planet zones applied, as
	// GenerateChunkSphericalPlanet blends them (SP-B10); the point as centre + direction x (R + altitude), in double.
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(WorldMode);
	const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
	const FVector Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, Params.PlanetCenter);
	float Continentalness = 0.0f;
	float Altitude = FSphericalPlanetWorldMode::SampleRadialDisplacement(
		Direction, Params.PlanetRadius, Planet.GetTerrainParams(), NoiseParams, &Planet.GetBiomeSnapshot(), &Continentalness);
	if (bConditioned)
	{
		Altitude = Conditioning->ApplyToAltitude(Direction, Params.PlanetRadius, Altitude);
	}
	if (OutContinentalness)
	{
		*OutContinentalness = Continentalness;
	}
	FVoxelTerrainSurfacePoint Point;
	Point.Altitude = Altitude;
	Point.Position = Params.PlanetCenter + Direction * (static_cast<double>(Params.PlanetRadius) + static_cast<double>(Altitude));
	Point.Normal = Direction;
	return Point;
}

void FVoxelSurfaceQuery::QuerySurfaceConditionsAt(
	const IVoxelWorldMode& WorldMode,
	const FVoxelTerrainSurfacePoint& Surface,
	float Continentalness,
	float VoxelSize,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	const FVoxelWaterLevels& Water,
	uint8& OutSurfaceMaterial, uint8& OutBiomeID)
{
	if (WorldMode.GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		const FColumnWater Over = WaterOver(Water, Surface.Position);
		QuerySurfaceConditions(static_cast<float>(Surface.Position.X), static_cast<float>(Surface.Position.Y),
			static_cast<float>(Surface.Position.Z), VoxelSize, BiomeSnapshot, NoiseParams.Seed,
			Over.bWater && !Water.IsRadial(), static_cast<float>(Over.Level), OutSurfaceMaterial, OutBiomeID);
		return;
	}
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(WorldMode);
	const FVector Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(Surface.Position, Planet.GetPlanetParams().PlanetCenter);
	QueryPlanetSurfaceConditionsAt(Planet, Direction, Surface.Altitude, Continentalness, static_cast<double>(Surface.Altitude), VoxelSize,
		NoiseParams, BiomeSnapshot, Water, OutSurfaceMaterial, OutBiomeID);
}

FVoxelSurfaceMaterialMix FVoxelSurfaceQuery::QuerySurfaceMaterialMixAt(
	const IVoxelWorldMode& WorldMode,
	const FVoxelTerrainSurfacePoint& Surface,
	float Continentalness,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	const FVoxelWaterLevels& Water)
{
	if (WorldMode.GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		const FColumnWater Over = WaterOver(Water, Surface.Position);
		return QuerySurfaceMaterialMix(static_cast<float>(Surface.Position.X), static_cast<float>(Surface.Position.Y),
			static_cast<float>(Surface.Position.Z), BiomeSnapshot, NoiseParams.Seed, Over.bWater && !Water.IsRadial(), static_cast<float>(Over.Level));
	}
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(WorldMode);
	const FVector Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(Surface.Position, Planet.GetPlanetParams().PlanetCenter);
	return QueryPlanetSurfaceMaterialMixAt(Planet, Direction, Surface.Altitude, Continentalness, NoiseParams, BiomeSnapshot, Water);
}

uint8 FVoxelSurfaceQuery::QueryMapSurfaceMaterial(
	const IVoxelWorldMode& WorldMode,
	const FVector& Position,
	const FVector& Direction,
	float SurfaceAltitude,
	float VoxelSize,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	bool bUseBiomes,
	const FVoxelWaterLevels& Water,
	const FVoxelConditioning* Conditioning,
	const FVoxelNoiseParams& FallbackTempNoise,
	const FVoxelNoiseParams& FallbackMoistureNoise)
{
	const bool bPlanet = WorldMode.GetWorldModeType() == EWorldMode::SphericalPlanet;
	uint8 MaterialID = 0;
	if (bUseBiomes && BiomeSnapshot.bIsValid)
	{
		// The SAME shared pipeline as chunk generation, tree placement and PCG (temperature / moisture / continentalness
		// noise -> tiered biome blend -> blended material -> priority-sorted height rules).
		uint8 BiomeID = 0;
		if (bPlanet)
		{
			QueryPlanetSurfaceConditions(static_cast<const FSphericalPlanetWorldMode&>(WorldMode), Position, VoxelSize, NoiseParams, BiomeSnapshot, Water,
				MaterialID, BiomeID, Conditioning);
		}
		else
		{
			const FColumnWater Over = WaterOver(Water, Position);
			QuerySurfaceConditions(
				static_cast<float>(Position.X), static_cast<float>(Position.Y), SurfaceAltitude, VoxelSize,
				BiomeSnapshot, NoiseParams.Seed, Over.bWater, static_cast<float>(Over.Level),
				MaterialID, BiomeID);
		}
	}
	else if (bUseBiomes)
	{
		// No valid biome config: static registry fallback (matches the CPU generators' hardcoded climate fields).
		float Temperature = 0.0f;
		float Moisture = 0.0f;
		if (bPlanet)
		{
			// The planet generator's climate: the planet noise on the datum sphere (WM-BP5)
			FSphericalPlanetWorldMode::SampleClimate(Direction, static_cast<const FSphericalPlanetWorldMode&>(WorldMode).GetPlanetParams().PlanetRadius,
				FallbackTempNoise, FallbackMoistureNoise, Temperature, Moisture);
		}
		else
		{
			const FVector BiomeSamplePos(Position.X, Position.Y, 0.0f);
			Temperature = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, FallbackTempNoise);
			Moisture = FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, FallbackMoistureNoise);
		}
		const FBiomeBlend Blend = FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f);
		MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, 0.0f,
			FVoxelBlendDitherKey(FVoxelBiomeBlendDither::KeyAt(Position, VoxelSize), NoiseParams.Seed));
	}
	else
	{
		// Legacy: the world mode's hardcoded material by depth (the surface's at depth 0).
		MaterialID = WorldMode.GetMaterialAtDepth(Position,
			bPlanet ? static_cast<float>(static_cast<const FSphericalPlanetWorldMode&>(WorldMode).GetPlanetParams().PlanetRadius + SurfaceAltitude) : SurfaceAltitude,
			0.0f);
	}
	return MaterialID;
}

FVoxelData FVoxelSurfaceQuery::GenerateVoxelAt(
	const IVoxelWorldMode& WorldMode,
	const FVector& WorldPos,
	float VoxelSize,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelConditioning* Conditioning,
	const UVoxelCaveConfiguration* Caves,
	const FVoxelWaterLevels& Water,
	bool bEnableBiomes,
	int32 BiomeHint,
	const FVoxelTerrainSurfacePoint* Surface,
	float Continentalness)
{
	FVoxelTerrainSurfacePoint Sampled;
	if (!Surface)
	{
		Sampled = GetGeneratedSurface(WorldMode, WorldPos, NoiseParams, Conditioning, &Continentalness);
		Surface = &Sampled;
	}

	// The generators' density and depth: a heightmap's signed distance to the generated height (InfinitePlane and
	// IslandBowl alike), a planet's shell distance to the generated altitude along the voxel's direction.
	uint8 Density = 0;
	float DepthBelowSurface = 0.0f;
	bool bUnderwater = false;
	const FSphericalPlanetParams* PlanetParams = nullptr;
	FVector Direction = FVector::UpVector;
	const bool bPlanet = WorldMode.GetWorldModeType() == EWorldMode::SphericalPlanet;
	// The water over the voxel's column: a level is world Z on a heightmap world, the distance from the centre on a planet.
	const FColumnWater Over = WaterOver(Water, WorldPos);
	if (!bPlanet)
	{
		const float GeneratedHeight = static_cast<float>(Surface->Position.Z);
		Density = FInfinitePlaneWorldMode::SignedDistanceToDensity(
			FInfinitePlaneWorldMode::CalculateSignedDistance(static_cast<float>(WorldPos.Z), GeneratedHeight), VoxelSize);
		DepthBelowSurface = (GeneratedHeight - static_cast<float>(WorldPos.Z)) / VoxelSize;
		bUnderwater = Over.bWater && GeneratedHeight < Over.Level;
	}
	else
	{
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(WorldMode);
		const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
		const float Altitude = Surface->Altitude;
		Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, Params.PlanetCenter);
		const double Distance = FSphericalPlanetWorldMode::CalculateRadialDistance(WorldPos, Params.PlanetCenter);
		const float SignedDistance = FSphericalPlanetWorldMode::ShellSignedDistance(Distance, Params.PlanetRadius + Altitude, Params);
		Density = FInfinitePlaneWorldMode::SignedDistanceToDensity(SignedDistance, VoxelSize);
		DepthBelowSurface = static_cast<float>((static_cast<double>(Altitude) - (Distance - Params.PlanetRadius)) / VoxelSize);
		bUnderwater = Over.bWater && static_cast<double>(Altitude) < Over.Level - Params.PlanetRadius;
		PlanetParams = &Params;
	}

	// The cave carve where the voxel is solid and under the ground, as generation applies it. Without a biome hint the
	// biome costs two climate fields on a planet, so the depth is checked against the layers' windows first (G72).
	if (Caves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f
		&& (BiomeHint >= 0 || Caves->AnyLayerReachesDepth(DepthBelowSurface)))
	{
		uint8 BiomeID = BiomeHint >= 0 ? static_cast<uint8>(BiomeHint) : 0;
		if (BiomeHint < 0 && bPlanet && Caves->BiomeOverrides.Num() > 0 && bEnableBiomes)
		{
			const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(WorldMode);
			const FVoxelBiomeSnapshot& Snapshot = Planet.GetBiomeSnapshot();
			const FVoxelNoiseParams TemperatureParams = FVoxelNoise::MakeFieldParams(NoiseParams.Seed + Snapshot.TemperatureSeedOffset,
				Snapshot.TemperatureNoiseFrequency);
			const FVoxelNoiseParams MoistureParams = FVoxelNoise::MakeFieldParams(NoiseParams.Seed + Snapshot.MoistureSeedOffset,
				Snapshot.MoistureNoiseFrequency);
			float Temperature = 0.0f;
			float Moisture = 0.0f;
			FSphericalPlanetWorldMode::SampleClimate(Direction, PlanetParams->PlanetRadius, TemperatureParams, MoistureParams, Temperature, Moisture);
			if (Snapshot.bIsValid)
			{
				Temperature = Snapshot.ShapeTemperature(Temperature, Surface->Altitude, static_cast<float>(Direction.Z), true);
				Moisture = Snapshot.ShapeMoisture(Moisture);
			}
			BiomeID = Snapshot.bIsValid
				? Snapshot.GetBiomeBlend(Temperature, Moisture, Continentalness).GetDominantBiome()
				: FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f).GetDominantBiome();
		}
		const float Carve = FVoxelCPUNoiseGenerator::CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Caves, NoiseParams.Seed,
			bUnderwater, PlanetParams);
		if (Carve > 0.0f)
		{
			Density = static_cast<uint8>(FMath::Clamp(FMath::Max(0.0f, static_cast<float>(Density) - Carve * 255.0f), 0.0f, 255.0f));
		}
	}
	return FVoxelData(0, Density);
}

float FVoxelSurfaceQuery::GetSurfaceHeight(
	const IVoxelWorldMode& WorldMode,
	float WorldX, float WorldY,
	const FVoxelNoiseParams& NoiseParams)
{
	return NaturalHeight(WorldMode, WorldX, WorldY, NoiseParams);
}

float FVoxelSurfaceQuery::ComputeSlopeDegrees(
	const IVoxelWorldMode& WorldMode,
	float WorldX, float WorldY, float VoxelSize,
	const FVoxelNoiseParams& NoiseParams)
{
	// Sample terrain height at 4 neighboring positions to compute gradient
	const float Step = VoxelSize;

	const float HX0 = NaturalHeight(WorldMode, WorldX - Step, WorldY, NoiseParams);
	const float HX1 = NaturalHeight(WorldMode, WorldX + Step, WorldY, NoiseParams);
	const float HY0 = NaturalHeight(WorldMode, WorldX, WorldY - Step, NoiseParams);
	const float HY1 = NaturalHeight(WorldMode, WorldX, WorldY + Step, NoiseParams);

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

	const float HX0 = NaturalHeight(WorldMode, WorldX - Step, WorldY, NoiseParams);
	const float HX1 = NaturalHeight(WorldMode, WorldX + Step, WorldY, NoiseParams);
	const float HY0 = NaturalHeight(WorldMode, WorldX, WorldY - Step, NoiseParams);
	const float HY1 = NaturalHeight(WorldMode, WorldX, WorldY + Step, NoiseParams);

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

	// The climate fields as the CPU generator samples them (the shared sampler)
	float Temperature = 0.0f, Moisture = 0.0f, Continentalness = 0.0f;
	SampleHeightmapClimate(WorldX, WorldY, TerrainHeight, WorldSeed, BiomeSnapshot, Temperature, Moisture, Continentalness);

	// Select biome (now with continentalness for proper tiered gating)
	FBiomeBlend Blend = BiomeSnapshot.GetBiomeBlend(Temperature, Moisture, Continentalness);
	OutBiomeID = Blend.GetDominantBiome();

	// Get surface material (depth = 0 for surface), blend zones dithered at the surface's voxel (G39)
	const FVoxelBlendDitherKey Dither(FVoxelBiomeBlendDither::KeyAt(FVector(WorldX, WorldY, TerrainHeight), VoxelSize), WorldSeed);
	const bool bIsUnderwater = bEnableWaterLevel && TerrainHeight < WaterLevel;
	if (bIsUnderwater)
	{
		OutSurfaceMaterial = BiomeSnapshot.GetBlendedMaterialWithWater(Blend, 0.0f, TerrainHeight, WaterLevel, Dither);
	}
	else
	{
		OutSurfaceMaterial = BiomeSnapshot.GetBlendedMaterial(Blend, 0.0f, Dither);
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
	bool bEnableWaterLevel, float WaterLevel,
	const FVoxelConditioning* Conditioning)
{
	FVoxelSurfaceSample Sample;

	// The generated height: the natural height with the conditioning zones layered on as the generator does (G23).
	const bool bConditioned = Conditioning && !Conditioning->IsEmpty();
	auto Height = [&](float X, float Y)
	{
		const float Natural = NaturalHeight(WorldMode, X, Y, NoiseParams);
		return bConditioned ? Conditioning->ApplyToHeight(X, Y, Natural) : Natural;
	};

	// Center height + 4-neighbor heights (compute gradient once, derive slope and normal).
	const float Step = VoxelSize;
	const float CentreHeight = Height(WorldX, WorldY);

	const float HX0 = Height(WorldX - Step, WorldY);
	const float HX1 = Height(WorldX + Step, WorldY);
	const float HY0 = Height(WorldX, WorldY - Step);
	const float HY1 = Height(WorldX, WorldY + Step);

	const float DX = (HX1 - HX0) / (2.0f * Step);
	const float DY = (HY1 - HY0) / (2.0f * Step);

	const float GradientMag = FMath::Sqrt(DX * DX + DY * DY);
	Sample.SlopeDegrees = FMath::RadiansToDegrees(FMath::Atan(GradientMag));
	Sample.Normal = FVector(-DX, -DY, 1.0f).GetSafeNormal();

	QuerySurfaceConditions(
		WorldX, WorldY, CentreHeight, VoxelSize,
		BiomeSnapshot, WorldSeed, bEnableWaterLevel, WaterLevel,
		Sample.MaterialID, Sample.BiomeID);

	Sample.Position = FVector(WorldX, WorldY, CentreHeight);
	Sample.Altitude = WorldMode.GetAltitude(Sample.Position);
	return Sample;
}

void FVoxelSurfaceQuery::QueryPlanetSurfaceConditions(
	const FSphericalPlanetWorldMode& Planet,
	const FVector& Position,
	float VoxelSize,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	const FVoxelWaterLevels& Water,
	uint8& OutSurfaceMaterial, uint8& OutBiomeID,
	const FVoxelConditioning* Conditioning)
{
	OutSurfaceMaterial = 0;
	OutBiomeID = 0;
	if (!BiomeSnapshot.bIsValid)
	{
		return;
	}

	const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
	const FVector Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(Position, Params.PlanetCenter);

	// GenerateChunkSphericalPlanet's order and arithmetic: the radial height (its continentalness reused for the blend),
	// the depth in voxels and the altitude, climate at the datum-sphere point, the blend, the material (the sea's
	// variant when water is on), height rules by the altitude. At a surface point the depth is 0.
	float Continentalness = 0.0f;
	float TerrainAltitude = FSphericalPlanetWorldMode::SampleRadialDisplacement(
		Direction, Params.PlanetRadius, Planet.GetTerrainParams(), NoiseParams, &BiomeSnapshot, &Continentalness);
	if (Conditioning && !Conditioning->IsEmpty())
	{
		// The conditioned terrain (SP-B10), as the generator blends it.
		TerrainAltitude = Conditioning->ApplyToAltitude(Direction, Params.PlanetRadius, TerrainAltitude);
	}
	// Altitudes, formed in double (WM-BP4), as the generator compares them.
	const double VoxelAltitude = FSphericalPlanetWorldMode::CalculateRadialDistance(Position, Params.PlanetCenter) - Params.PlanetRadius;
	QueryPlanetSurfaceConditionsAt(Planet, Direction, TerrainAltitude, Continentalness, VoxelAltitude, VoxelSize, NoiseParams,
		BiomeSnapshot, Water, OutSurfaceMaterial, OutBiomeID);
}

void FVoxelSurfaceQuery::QueryPlanetSurfaceConditionsAt(
	const FSphericalPlanetWorldMode& Planet,
	const FVector& Direction,
	float TerrainAltitude,
	float Continentalness,
	double VoxelAltitude,
	float VoxelSize,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	const FVoxelWaterLevels& Water,
	uint8& OutSurfaceMaterial, uint8& OutBiomeID)
{
	OutSurfaceMaterial = 0;
	OutBiomeID = 0;
	if (!BiomeSnapshot.bIsValid)
	{
		return;
	}

	const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
	const float DepthBelowSurface = static_cast<float>((static_cast<double>(TerrainAltitude) - VoxelAltitude) / FMath::Max(VoxelSize, KINDA_SMALL_NUMBER));
	const float Altitude = static_cast<float>(VoxelAltitude);

	// Climate as generation samples it: the planet noise at the direction's point on the datum sphere (WM-BP5)
	float Temperature = 0.0f, Moisture = 0.0f;
	SamplePlanetClimate(Direction, Params.PlanetRadius, TerrainAltitude, NoiseParams.Seed, BiomeSnapshot, Temperature, Moisture);

	const FBiomeBlend Blend = BiomeSnapshot.GetBiomeBlend(Temperature, Moisture, Continentalness);
	OutBiomeID = Blend.GetDominantBiome();
	// Blend zones dithered at the voxel's key (G39): its position along the direction, in double
	const FVoxelBlendDitherKey Dither(FVoxelBiomeBlendDither::KeyAt(
		Params.PlanetCenter + Direction * (Params.PlanetRadius + VoxelAltitude), VoxelSize), NoiseParams.Seed);
	// The water over the terrain along the direction: its level less the datum radius is the altitude generation compares.
	const FColumnWater Over = WaterOver(Water, Params.PlanetCenter + Direction * (Params.PlanetRadius + static_cast<double>(TerrainAltitude)));
	OutSurfaceMaterial = (Over.bWater && Water.IsRadial())
		? BiomeSnapshot.GetBlendedMaterialWithWater(Blend, DepthBelowSurface, TerrainAltitude, static_cast<float>(Over.Level - Params.PlanetRadius), Dither)
		: BiomeSnapshot.GetBlendedMaterial(Blend, DepthBelowSurface, Dither);
	OutSurfaceMaterial = BiomeSnapshot.ApplyHeightMaterialRules(OutSurfaceMaterial, Altitude, DepthBelowSurface);
}

FVoxelSurfaceMaterialMix FVoxelSurfaceQuery::QueryPlanetSurfaceMaterialMixAt(
	const FSphericalPlanetWorldMode& Planet,
	const FVector& Direction,
	float TerrainAltitude,
	float Continentalness,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	const FVoxelWaterLevels& Water)
{
	FVoxelSurfaceMaterialMix Mix;
	if (!BiomeSnapshot.bIsValid)
	{
		return Mix;
	}
	const FSphericalPlanetParams& Params = Planet.GetPlanetParams();

	// Climate and the blend as QueryPlanetSurfaceConditionsAt has them
	float Temperature = 0.0f, Moisture = 0.0f;
	SamplePlanetClimate(Direction, Params.PlanetRadius, TerrainAltitude, NoiseParams.Seed, BiomeSnapshot, Temperature, Moisture);
	const FBiomeBlend Blend = BiomeSnapshot.GetBiomeBlend(Temperature, Moisture, Continentalness);
	Mix.BiomeID = Blend.GetDominantBiome();

	// The blend's surface materials and shares, height rules applied to each, merged per material.
	uint8 Materials[MAX_BIOME_BLEND];
	float Shares[MAX_BIOME_BLEND];
	const FColumnWater Over = WaterOver(Water, Params.PlanetCenter + Direction * (Params.PlanetRadius + static_cast<double>(TerrainAltitude)));
	const bool bWater = Over.bWater && Water.IsRadial();
	const int32 Num = BiomeSnapshot.GetBlendedMaterialShares(Blend, 0.0f, bWater, TerrainAltitude,
		static_cast<float>(Over.Level - Params.PlanetRadius), Materials, Shares);
	uint8 Merged[MAX_BIOME_BLEND];
	float MergedShares[MAX_BIOME_BLEND];
	int32 MergedNum = 0;
	for (int32 i = 0; i < Num; ++i)
	{
		const uint8 Material = BiomeSnapshot.ApplyHeightMaterialRules(Materials[i], TerrainAltitude, 0.0f);
		int32 Slot = 0;
		while (Slot < MergedNum && Merged[Slot] != Material)
		{
			++Slot;
		}
		if (Slot == MergedNum)
		{
			Merged[MergedNum] = Material;
			MergedShares[MergedNum] = 0.0f;
			++MergedNum;
		}
		MergedShares[Slot] += Shares[i];
	}
	// The two largest.
	int32 First = 0;
	for (int32 i = 1; i < MergedNum; ++i)
	{
		First = MergedShares[i] > MergedShares[First] ? i : First;
	}
	int32 Second = -1;
	for (int32 i = 0; i < MergedNum; ++i)
	{
		if (i != First && (Second < 0 || MergedShares[i] > MergedShares[Second]))
		{
			Second = i;
		}
	}
	Mix.Materials[0] = Merged[First];
	Mix.Materials[1] = Second >= 0 ? Merged[Second] : Merged[First];
	const float Pair = MergedShares[First] + (Second >= 0 ? MergedShares[Second] : 0.0f);
	Mix.Weight = (Second >= 0 && Pair > 0.0f) ? MergedShares[Second] / Pair : 0.0f;
	return Mix;
}

FVoxelSurfaceMaterialMix FVoxelSurfaceQuery::QuerySurfaceMaterialMix(
	float WorldX, float WorldY, float TerrainHeight,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	int32 WorldSeed,
	bool bEnableWaterLevel, float WaterLevel)
{
	FVoxelSurfaceMaterialMix Mix;
	if (!BiomeSnapshot.bIsValid)
	{
		return Mix;
	}

	// The climate and the blend as QuerySurfaceConditions samples them (the same noise parameters).
	auto ClimateParams = [WorldSeed](int32 SeedOffset, float Frequency)
	{
		FVoxelNoiseParams Params;
		Params.NoiseType = EVoxelNoiseType::Simplex;
		Params.Octaves = 2;
		Params.Persistence = 0.5f;
		Params.Lacunarity = 2.0f;
		Params.Seed = WorldSeed + SeedOffset;
		Params.Frequency = Frequency;
		return Params;
	};
	const FVector BiomeSamplePos(WorldX, WorldY, 0.0f);
	const float Temperature = BiomeSnapshot.ShapeTemperature(
		FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, ClimateParams(BiomeSnapshot.TemperatureSeedOffset, BiomeSnapshot.TemperatureNoiseFrequency)),
		TerrainHeight, 0.0f, false);
	const float Moisture = BiomeSnapshot.ShapeMoisture(
		FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, ClimateParams(BiomeSnapshot.MoistureSeedOffset, BiomeSnapshot.MoistureNoiseFrequency)));
	const float Continentalness = BiomeSnapshot.bEnableContinentalness
		? BiomeSnapshot.ShapeContinentalness(
			FVoxelCPUNoiseGenerator::FBM3D(BiomeSamplePos, ClimateParams(BiomeSnapshot.ContinentalnessSeedOffset, BiomeSnapshot.ContinentalnessNoiseFrequency)))
		: 0.0f;
	const FBiomeBlend Blend = BiomeSnapshot.GetBiomeBlend(Temperature, Moisture, Continentalness);
	Mix.BiomeID = Blend.GetDominantBiome();

	// The blend's surface materials and shares, height rules applied to each, merged per material (as the planet's mix).
	uint8 Materials[MAX_BIOME_BLEND];
	float Shares[MAX_BIOME_BLEND];
	const int32 Num = BiomeSnapshot.GetBlendedMaterialShares(Blend, 0.0f, bEnableWaterLevel, TerrainHeight, WaterLevel, Materials, Shares);
	uint8 Merged[MAX_BIOME_BLEND];
	float MergedShares[MAX_BIOME_BLEND];
	int32 MergedNum = 0;
	for (int32 i = 0; i < Num; ++i)
	{
		const uint8 Material = BiomeSnapshot.ApplyHeightMaterialRules(Materials[i], TerrainHeight, 0.0f);
		int32 Slot = 0;
		while (Slot < MergedNum && Merged[Slot] != Material)
		{
			++Slot;
		}
		if (Slot == MergedNum)
		{
			Merged[MergedNum] = Material;
			MergedShares[MergedNum] = 0.0f;
			++MergedNum;
		}
		MergedShares[Slot] += Shares[i];
	}
	int32 First = 0;
	for (int32 i = 1; i < MergedNum; ++i)
	{
		First = MergedShares[i] > MergedShares[First] ? i : First;
	}
	int32 Second = -1;
	for (int32 i = 0; i < MergedNum; ++i)
	{
		if (i != First && (Second < 0 || MergedShares[i] > MergedShares[Second]))
		{
			Second = i;
		}
	}
	if (MergedNum == 0)
	{
		return Mix;
	}
	Mix.Materials[0] = Merged[First];
	Mix.Materials[1] = Second >= 0 ? Merged[Second] : Merged[First];
	const float Pair = MergedShares[First] + (Second >= 0 ? MergedShares[Second] : 0.0f);
	Mix.Weight = (Second >= 0 && Pair > 0.0f) ? MergedShares[Second] / Pair : 0.0f;
	return Mix;
}

FVoxelSurfaceSample FVoxelSurfaceQuery::SampleSurfacePoint(
	const IVoxelWorldMode& WorldMode,
	const FVector& WorldPos, float VoxelSize,
	const FVoxelNoiseParams& NoiseParams,
	const FVoxelBiomeSnapshot& BiomeSnapshot,
	const FVoxelWaterLevels& Water,
	const FVoxelConditioning* Conditioning)
{
	if (WorldMode.GetWorldModeType() != EWorldMode::SphericalPlanet)
	{
		// Heightmap modes: the (X, Y) sample with the zones there, and the water over that column (a level is a world Z).
		const FColumnWater Over = WaterOver(Water, WorldPos);
		return SampleSurface(WorldMode, static_cast<float>(WorldPos.X), static_cast<float>(WorldPos.Y), VoxelSize, NoiseParams,
			BiomeSnapshot, NoiseParams.Seed, Over.bWater && !Water.IsRadial(), static_cast<float>(Over.Level), Conditioning);
	}

	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(WorldMode);
	const FVoxelTerrainSurfacePoint Surface = (Conditioning && !Conditioning->IsEmpty())
		? Planet.GetConditionedSurfacePoint(WorldPos, NoiseParams, *Conditioning)
		: Planet.GetSurfacePoint(WorldPos, NoiseParams);
	FVoxelSurfaceSample Sample;
	Sample.Position = Surface.Position;
	Sample.Normal = Surface.Normal;
	Sample.Up = Planet.GetUpVector(Surface.Position);
	Sample.Altitude = Surface.Altitude;
	Sample.SlopeDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Sample.Normal, Sample.Up), -1.0, 1.0)));
	QueryPlanetSurfaceConditions(Planet, Surface.Position, VoxelSize, NoiseParams, BiomeSnapshot, Water, Sample.MaterialID, Sample.BiomeID, Conditioning);
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
