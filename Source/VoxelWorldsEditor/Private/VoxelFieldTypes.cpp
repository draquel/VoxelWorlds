// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelFieldTypes.h"

#include "VoxelWorldConfiguration.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelMaterialDefinition.h"

#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelWorldModeFactory.h"

#include "VoxelSurfaceQuery.h"
#include "VoxelCaveQuery.h"
#include "VoxelCPUNoiseGenerator.h"

#define LOCTEXT_NAMESPACE "VoxelEditorFields"

// ============================================================================
// FVoxelFieldSampleContext
// ============================================================================

FVoxelFieldSampleContext FVoxelFieldSampleContext::FromConfiguration(const UVoxelWorldConfiguration* Config)
{
	FVoxelFieldSampleContext Ctx;
	if (!Config)
	{
		return Ctx;
	}

	Ctx.NoiseParams = Config->NoiseParams;
	Ctx.VoxelSize = Config->VoxelSize;
	Ctx.WorldOrigin = Config->WorldOrigin;
	Ctx.bEnableBiomes = Config->bEnableBiomes;
	Ctx.BiomeConfig = Config->bEnableBiomes ? Config->BiomeConfiguration : nullptr;
	Ctx.bEnableCaves = Config->bEnableCaves;
	Ctx.CaveConfig = Config->bEnableCaves ? Config->CaveConfiguration : nullptr;
	Ctx.bEnableWaterLevel = Config->bEnableWaterLevel;
	Ctx.WaterLevel = Config->WaterLevel;
	Ctx.Sea = FVoxelSeaSurface::FromConfig(*Config);
	Ctx.bPlanet = Config->WorldMode == EWorldMode::SphericalPlanet;
	Ctx.PlanetRadius = Config->WorldRadius;

	// The analytic world mode, from the factory generation and the chunk manager use (WM-A1), so sampled
	// height/material/biome match generation. The factory sets the biome context the way generation applies
	// continentalness: whenever the config has a biome configuration, whatever bEnableBiomes says.
	Ctx.WorldMode = FVoxelWorldModeFactory::Create(*Config);

	// Value snapshot for the material/biome surface queries (same data the world mode captured).
	Ctx.BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(Ctx.BiomeConfig);

	// Derive the biome-climate noise params — mirrors FVoxelCPUNoiseGenerator / UVoxelMapSubsystem.
	auto InitClimate = [](FVoxelNoiseParams& P)
	{
		P.NoiseType = EVoxelNoiseType::Simplex;
		P.Octaves = 2;
		P.Persistence = 0.5f;
		P.Lacunarity = 2.0f;
		P.Amplitude = 1.0f;
	};
	InitClimate(Ctx.TempNoiseParams);
	InitClimate(Ctx.MoistureNoiseParams);
	InitClimate(Ctx.ContinentalnessNoiseParams);

	const int32 Seed = Ctx.NoiseParams.Seed;
	if (Ctx.bEnableBiomes && Ctx.BiomeConfig)
	{
		Ctx.TempNoiseParams.Seed = Seed + Ctx.BiomeConfig->TemperatureSeedOffset;
		Ctx.TempNoiseParams.Frequency = Ctx.BiomeConfig->TemperatureNoiseFrequency;
		Ctx.MoistureNoiseParams.Seed = Seed + Ctx.BiomeConfig->MoistureSeedOffset;
		Ctx.MoistureNoiseParams.Frequency = Ctx.BiomeConfig->MoistureNoiseFrequency;

		if (Ctx.BiomeConfig->bEnableContinentalness)
		{
			Ctx.bUseContinentalness = true;
			Ctx.ContinentalnessNoiseParams.Seed = Seed + Ctx.BiomeConfig->ContinentalnessSeedOffset;
			Ctx.ContinentalnessNoiseParams.Frequency = Ctx.BiomeConfig->ContinentalnessNoiseFrequency;
		}
	}
	else
	{
		// Fallback defaults matching VoxelCPUNoiseGenerator when biomes are on but no config is set.
		Ctx.TempNoiseParams.Seed = Seed + 1234;
		Ctx.TempNoiseParams.Frequency = 0.00005f;
		Ctx.MoistureNoiseParams.Seed = Seed + 5678;
		Ctx.MoistureNoiseParams.Frequency = 0.00007f;
	}

	Ctx.bValid = true;
	return Ctx;
}

FVector FVoxelFieldSampleContext::PlanetDirectionAbove(double X, double Y) const
{
	const double DX = X - WorldOrigin.X;
	const double DY = Y - WorldOrigin.Y;
	const double R = PlanetRadius;
	const FVector Direction = FVector(DX, DY, FMath::Sqrt(FMath::Max(0.0, R * R - DX * DX - DY * DY))).GetSafeNormal();
	return Direction.IsNearlyZero() ? FVector::UpVector : Direction;
}

FVoxelSurfaceSample FVoxelFieldSampleContext::PlanetSurfaceAbove(double X, double Y) const
{
	return FVoxelSurfaceQuery::SampleSurfacePoint(*WorldMode, WorldOrigin + PlanetDirectionAbove(X, Y) * PlanetRadius, VoxelSize,
		NoiseParams, BiomeSnapshot, Sea);
}

// ============================================================================
// FVoxelEditorField color mapping
// ============================================================================

FColor FVoxelEditorField::MapScalarToColor(float Value, float RangeMin, float RangeMax) const
{
	const float T = (RangeMax > RangeMin)
		? FMath::Clamp((Value - RangeMin) / (RangeMax - RangeMin), 0.0f, 1.0f)
		: 0.0f;
	// Explicit lerp using FLinearColor's member operators (avoids relying on float * FLinearColor).
	const FLinearColor Mixed = RampLow + (RampHigh - RampLow) * T;
	return Mixed.ToFColor(/*bSRGB=*/true);
}

FColor FVoxelEditorField::MapCategoricalToColor(float Value) const
{
	const int32 CategoryId = FMath::Clamp(FMath::RoundToInt(Value), 0, 255);

	if (ColorMode == EVoxelFieldColor::MaterialPalette)
	{
		return FVoxelMaterialRegistry::GetMaterialColor(static_cast<uint8>(CategoryId));
	}

	// BiomePalette (and default): distinct hashed hue per id via golden-ratio spacing.
	const float Hue01 = FMath::Fmod(static_cast<float>(CategoryId) * 0.61803398875f, 1.0f);
	const uint8 H = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Hue01 * 255.0f), 0, 255));
	return FLinearColor::MakeFromHSV8(H, /*S=*/190, /*V=*/220).ToFColor(/*bSRGB=*/true);
}

FText FVoxelEditorField::GetCategoryLabel(const FVoxelFieldSampleContext& Ctx, int32 CategoryId) const
{
	const int32 ClampedId = FMath::Clamp(CategoryId, 0, 255);

	if (ColorMode == EVoxelFieldColor::MaterialPalette)
	{
		if (const FVoxelMaterialDefinition* Mat = FVoxelMaterialRegistry::GetMaterial(static_cast<uint8>(ClampedId)))
		{
			if (!Mat->Name.IsEmpty())
			{
				return FText::Format(LOCTEXT("MatLabel", "{0}  (#{1})"), FText::FromString(Mat->Name), FText::AsNumber(ClampedId));
			}
		}
		return FText::Format(LOCTEXT("MatIdLabel", "Material #{0}"), FText::AsNumber(ClampedId));
	}

	if (ColorMode == EVoxelFieldColor::BiomePalette)
	{
		if (Ctx.BiomeConfig)
		{
			if (const FBiomeDefinition* Biome = Ctx.BiomeConfig->GetBiome(static_cast<uint8>(ClampedId)))
			{
				if (!Biome->Name.IsEmpty())
				{
					return FText::Format(LOCTEXT("BiomeLabel", "{0}  (#{1})"), FText::FromString(Biome->Name), FText::AsNumber(ClampedId));
				}
			}
		}
		return FText::Format(LOCTEXT("BiomeIdLabel", "Biome #{0}"), FText::AsNumber(ClampedId));
	}

	return FText::AsNumber(ClampedId);
}

// ============================================================================
// FVoxelFieldRegistry
// ============================================================================

namespace
{
	TArray<FVoxelEditorField>& GetMutableFields()
	{
		static TArray<FVoxelEditorField> Fields;
		return Fields;
	}

	bool GBuiltinsRegistered = false;
}

void FVoxelFieldRegistry::RegisterField(const FVoxelEditorField& Field)
{
	TArray<FVoxelEditorField>& Fields = GetMutableFields();
	for (FVoxelEditorField& Existing : Fields)
	{
		if (Existing.Id == Field.Id)
		{
			Existing = Field;
			return;
		}
	}
	Fields.Add(Field);
}

const FVoxelEditorField* FVoxelFieldRegistry::FindField(FName Id)
{
	for (const FVoxelEditorField& Field : GetMutableFields())
	{
		if (Field.Id == Id)
		{
			return &Field;
		}
	}
	return nullptr;
}

const TArray<FVoxelEditorField>& FVoxelFieldRegistry::GetFields()
{
	return GetMutableFields();
}

TArray<FName> FVoxelFieldRegistry::GetFieldIds()
{
	TArray<FName> Ids;
	for (const FVoxelEditorField& Field : GetMutableFields())
	{
		Ids.Add(Field.Id);
	}
	return Ids;
}

void FVoxelFieldRegistry::EnsureBuiltinsRegistered()
{
	if (GBuiltinsRegistered)
	{
		return;
	}
	GBuiltinsRegistered = true;

	// --- Terrain height (auto-ranged for contrast) ---
	{
		FVoxelEditorField F;
		F.Id = TEXT("Height");
		F.DisplayName = LOCTEXT("FieldHeight", "Terrain Height");
		F.Kind = EVoxelFieldKind::Scalar2D;
		F.bAutoRange = true;
		F.Units = LOCTEXT("UnitCm", "cm");
		F.RampLow = FLinearColor(0.02f, 0.05f, 0.25f);
		F.RampHigh = FLinearColor(1.0f, 0.98f, 0.85f);
		F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double /*Z*/)
		{
			if (C.bPlanet)
			{
				return C.PlanetSurfaceAbove(X, Y).Altitude;
			}
			return FVoxelSurfaceQuery::GetSurfaceHeight(*C.WorldMode, static_cast<float>(X), static_cast<float>(Y), C.NoiseParams);
		};
		RegisterField(F);
	}

	// --- Slope (degrees, 0..90) ---
	{
		FVoxelEditorField F;
		F.Id = TEXT("Slope");
		F.DisplayName = LOCTEXT("FieldSlope", "Slope");
		F.Kind = EVoxelFieldKind::Scalar2D;
		F.DisplayMin = 0.0f;
		F.DisplayMax = 90.0f;
		F.Units = LOCTEXT("UnitDeg", "deg");
		F.RampLow = FLinearColor(0.05f, 0.25f, 0.05f);
		F.RampHigh = FLinearColor(0.90f, 0.10f, 0.05f);
		F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double /*Z*/)
		{
			if (C.bPlanet)
			{
				return C.PlanetSurfaceAbove(X, Y).SlopeDegrees;
			}
			return FVoxelSurfaceQuery::ComputeSlopeDegrees(*C.WorldMode, static_cast<float>(X), static_cast<float>(Y), C.VoxelSize, C.NoiseParams);
		};
		RegisterField(F);
	}

	// --- Surface material (categorical) ---
	{
		FVoxelEditorField F;
		F.Id = TEXT("SurfaceMaterial");
		F.DisplayName = LOCTEXT("FieldMaterial", "Surface Material");
		F.Kind = EVoxelFieldKind::Categorical;
		F.ColorMode = EVoxelFieldColor::MaterialPalette;
		F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double /*Z*/)
		{
			if (C.bPlanet)
			{
				return static_cast<float>(C.PlanetSurfaceAbove(X, Y).MaterialID);
			}
			const float H = FVoxelSurfaceQuery::GetSurfaceHeight(*C.WorldMode, static_cast<float>(X), static_cast<float>(Y), C.NoiseParams);
			uint8 Mat = 0, Biome = 0;
			FVoxelSurfaceQuery::QuerySurfaceConditions(static_cast<float>(X), static_cast<float>(Y), H, C.VoxelSize,
				C.BiomeSnapshot, C.GetSeed(), C.bEnableWaterLevel, C.WaterLevel, Mat, Biome);
			return static_cast<float>(Mat);
		};
		RegisterField(F);
	}

	// --- Biome (categorical) ---
	{
		FVoxelEditorField F;
		F.Id = TEXT("Biome");
		F.DisplayName = LOCTEXT("FieldBiome", "Biome");
		F.Kind = EVoxelFieldKind::Categorical;
		F.ColorMode = EVoxelFieldColor::BiomePalette;
		F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double /*Z*/)
		{
			if (C.bPlanet)
			{
				return static_cast<float>(C.PlanetSurfaceAbove(X, Y).BiomeID);
			}
			const float H = FVoxelSurfaceQuery::GetSurfaceHeight(*C.WorldMode, static_cast<float>(X), static_cast<float>(Y), C.NoiseParams);
			uint8 Mat = 0, Biome = 0;
			FVoxelSurfaceQuery::QuerySurfaceConditions(static_cast<float>(X), static_cast<float>(Y), H, C.VoxelSize,
				C.BiomeSnapshot, C.GetSeed(), C.bEnableWaterLevel, C.WaterLevel, Mat, Biome);
			return static_cast<float>(Biome);
		};
		RegisterField(F);
	}

	// --- Temperature ([-1,1] climate noise) ---
	{
		FVoxelEditorField F;
		F.Id = TEXT("Temperature");
		F.DisplayName = LOCTEXT("FieldTemperature", "Temperature");
		F.Kind = EVoxelFieldKind::Scalar2D;
		F.DisplayMin = -1.0f;
		F.DisplayMax = 1.0f;
		F.RampLow = FLinearColor(0.10f, 0.35f, 0.95f);   // cold -> blue
		F.RampHigh = FLinearColor(0.95f, 0.25f, 0.10f);  // hot  -> red
		F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double /*Z*/)
		{
			const FVector SamplePos = C.bPlanet
				? FSphericalPlanetWorldMode::GetNoiseSamplePoint(C.PlanetDirectionAbove(X, Y), C.PlanetRadius) : FVector(X, Y, 0.0);
			return FVoxelCPUNoiseGenerator::FBM3D(SamplePos, C.TempNoiseParams);
		};
		RegisterField(F);
	}

	// --- Moisture ([-1,1] climate noise) ---
	{
		FVoxelEditorField F;
		F.Id = TEXT("Moisture");
		F.DisplayName = LOCTEXT("FieldMoisture", "Moisture");
		F.Kind = EVoxelFieldKind::Scalar2D;
		F.DisplayMin = -1.0f;
		F.DisplayMax = 1.0f;
		F.RampLow = FLinearColor(0.85f, 0.75f, 0.45f);   // dry   -> tan
		F.RampHigh = FLinearColor(0.05f, 0.45f, 0.35f);  // humid -> teal
		F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double /*Z*/)
		{
			const FVector SamplePos = C.bPlanet
				? FSphericalPlanetWorldMode::GetNoiseSamplePoint(C.PlanetDirectionAbove(X, Y), C.PlanetRadius) : FVector(X, Y, 0.0);
			return FVoxelCPUNoiseGenerator::FBM3D(SamplePos, C.MoistureNoiseParams);
		};
		RegisterField(F);
	}

	// --- Continentalness ([-1,1]; flat 0 when disabled on the biome config) ---
	{
		FVoxelEditorField F;
		F.Id = TEXT("Continentalness");
		F.DisplayName = LOCTEXT("FieldContinentalness", "Continentalness");
		F.Kind = EVoxelFieldKind::Scalar2D;
		F.DisplayMin = -1.0f;
		F.DisplayMax = 1.0f;
		F.RampLow = FLinearColor(0.05f, 0.10f, 0.45f);   // ocean
		F.RampHigh = FLinearColor(0.55f, 0.45f, 0.25f);  // inland
		F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double /*Z*/)
		{
			const FVector SamplePos = C.bPlanet
				? FSphericalPlanetWorldMode::GetNoiseSamplePoint(C.PlanetDirectionAbove(X, Y), C.PlanetRadius) : FVector(X, Y, 0.0);
			return C.bUseContinentalness
				? FVoxelCPUNoiseGenerator::FBM3D(SamplePos, C.ContinentalnessNoiseParams)
				: 0.0f;
		};
		RegisterField(F);
	}

	// --- Cave presence (3D carve density [0,1] at SampleZ) ---
	{
		FVoxelEditorField F;
		F.Id = TEXT("CavePresence");
		F.DisplayName = LOCTEXT("FieldCavePresence", "Cave Presence");
		F.Kind = EVoxelFieldKind::Scalar3D;
		F.DisplayMin = 0.0f;
		F.DisplayMax = 1.0f;
		F.RampLow = FLinearColor(0.02f, 0.02f, 0.02f);   // solid
		F.RampHigh = FLinearColor(0.20f, 0.90f, 1.00f);  // open cavity
		F.Sample = [](const FVoxelFieldSampleContext& C, double X, double Y, double Z)
		{
			// A heightmap preview (the cave query takes a Z height); planets show none.
			if (!C.bEnableCaves || !C.CaveConfig || C.bPlanet)
			{
				return 0.0f;
			}
			const float H = FVoxelSurfaceQuery::GetSurfaceHeight(*C.WorldMode, static_cast<float>(X), static_cast<float>(Y), C.NoiseParams);
			uint8 Mat = 0, Biome = 0;
			FVoxelSurfaceQuery::QuerySurfaceConditions(static_cast<float>(X), static_cast<float>(Y), H, C.VoxelSize,
				C.BiomeSnapshot, C.GetSeed(), C.bEnableWaterLevel, C.WaterLevel, Mat, Biome);
			const bool bUnderwater = C.bEnableWaterLevel && H < C.WaterLevel;
			return FVoxelCaveQuery::SampleCaveDensityAt(FVector(X, Y, Z), H, C.VoxelSize, Biome, C.CaveConfig, C.GetSeed(), bUnderwater);
		};
		RegisterField(F);
	}
}

#undef LOCTEXT_NAMESPACE
