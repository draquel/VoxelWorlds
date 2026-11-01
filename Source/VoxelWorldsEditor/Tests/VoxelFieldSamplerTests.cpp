// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "VoxelFieldTypes.h"
#include "VoxelFieldImageBaker.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelSurfaceQuery.h"
#include "IVoxelWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoise.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Parity + sanity tests for the editor field sampler. The field functions must never silently drift
 * from the runtime queries they wrap, so the Height field is asserted bit-equal to
 * FVoxelSurfaceQuery::GetSurfaceHeight, cave presence is range-checked, and the baker output size is
 * verified.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFieldSamplerParityTest, "VoxelWorlds.Editor.FieldSampler.Parity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelFieldSamplerParityTest::RunTest(const FString& Parameters)
{
	FVoxelFieldRegistry::EnsureBuiltinsRegistered();

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	if (!TestNotNull(TEXT("Config created"), Config))
	{
		return false;
	}
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->bEnableBiomes = true;
	Config->bEnableCaves = false;
	Config->NoiseParams.Seed = 24680;

	FVoxelFieldSampleContext Ctx = FVoxelFieldSampleContext::FromConfiguration(Config);
	TestTrue(TEXT("Context is valid"), Ctx.IsValid());
	if (!Ctx.IsValid())
	{
		return false;
	}

	// Height field must equal the runtime surface query exactly (no drift).
	const FVoxelEditorField* Height = FVoxelFieldRegistry::FindField(TEXT("Height"));
	if (TestNotNull(TEXT("Height field registered"), Height))
	{
		const double SampleXs[] = { -123450.0, -1000.0, 0.0, 4242.0, 98765.0 };
		const double SampleYs[] = { -67890.0, -50.0, 0.0, 777.0, 54321.0 };
		for (double X : SampleXs)
		{
			for (double Y : SampleYs)
			{
				const float FieldH = Height->Sample(Ctx, X, Y, 0.0);
				const float RefH = FVoxelSurfaceQuery::GetSurfaceHeight(
					*Ctx.WorldMode, static_cast<float>(X), static_cast<float>(Y), Ctx.NoiseParams);
				TestEqual(FString::Printf(TEXT("Height parity @ (%.0f, %.0f)"), X, Y), FieldH, RefH);
			}
		}
	}

	// Slope must always be a valid angle.
	const FVoxelEditorField* Slope = FVoxelFieldRegistry::FindField(TEXT("Slope"));
	if (TestNotNull(TEXT("Slope field registered"), Slope))
	{
		const float S = Slope->Sample(Ctx, 3210.0, -6540.0, 0.0);
		TestTrue(TEXT("Slope within [0,90]"), S >= 0.0f && S <= 90.0f);
	}

	// Cave presence must be a normalized carve density even with caves disabled (returns 0).
	const FVoxelEditorField* Cave = FVoxelFieldRegistry::FindField(TEXT("CavePresence"));
	if (TestNotNull(TEXT("Cave field registered"), Cave))
	{
		const float V = Cave->Sample(Ctx, 1000.0, 1000.0, -5000.0);
		TestTrue(TEXT("Cave presence within [0,1]"), V >= 0.0f && V <= 1.0f);
		TestEqual(TEXT("Cave presence is 0 when caves disabled"), V, 0.0f);
	}

	// The baker must produce a full Resolution x Resolution image.
	if (Height)
	{
		FVoxelFieldBakeParams BakeParams;
		BakeParams.Resolution = 32;
		BakeParams.RegionSize = 64000.0;
		BakeParams.Center = FVector2D(0.0, 0.0);

		TArray<FColor> Pixels;
		float RangeMin = 0.0f, RangeMax = 0.0f;
		const bool bBaked = FVoxelFieldImageBaker::BakeToPixels(*Height, Ctx, BakeParams, Pixels, RangeMin, RangeMax);
		TestTrue(TEXT("Bake succeeded"), bBaked);
		TestEqual(TEXT("Pixel count matches resolution"), Pixels.Num(), 32 * 32);
	}

	// Legend data path: a categorical field collects the present category ids and labels each one.
	const FVoxelEditorField* BiomeField = FVoxelFieldRegistry::FindField(TEXT("Biome"));
	if (TestNotNull(TEXT("Biome field registered"), BiomeField))
	{
		FVoxelFieldBakeParams BiomeBake;
		BiomeBake.Resolution = 24;
		BiomeBake.RegionSize = 200000.0;
		BiomeBake.Center = FVector2D(0.0, 0.0);

		TArray<FColor> Px;
		TArray<int32> PresentIds;
		float Mn = 0.0f, Mx = 0.0f;
		FVoxelFieldImageBaker::BakeToPixels(*BiomeField, Ctx, BiomeBake, Px, Mn, Mx, &PresentIds);
		TestTrue(TEXT("Biome bake collected at least one category"), PresentIds.Num() >= 1);
		for (int32 CatId : PresentIds)
		{
			TestFalse(TEXT("Biome category label is non-empty"), BiomeField->GetCategoryLabel(Ctx, CatId).IsEmpty());
		}
	}

	return true;
}

/**
 * Spherical Phase 4 (WORLD_MODES_REVIEW_PLAN.md §18, WM-SY3; SP-A3): on a planet the 2D fields show the surface on the
 * upper hemisphere above (X, Y): Height is its altitude, Slope is measured against local up, the climate fields are
 * sampled at the direction's point on the datum sphere, and the legacy height query is not called.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFieldSamplerPlanetTest, "VoxelWorlds.Editor.FieldSampler.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelFieldSamplerPlanetTest::RunTest(const FString& Parameters)
{
	FVoxelFieldRegistry::EnsureBuiltinsRegistered();

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->WorldMode = EWorldMode::SphericalPlanet;
	Config->WorldOrigin = FVector(1000.0, -2000.0, 500.0);
	Config->WorldRadius = 250000.0f;
	Config->PlanetHeightScale = 6000.0f;
	Config->PlanetMaxTerrainHeight = 6500.0f;
	Config->PlanetMaxTerrainDepth = 6500.0f;
	Config->bEnableBiomes = true;
	Config->bEnableCaves = false;
	Config->NoiseParams.Seed = 24680;
	Config->NoiseParams.Frequency = 0.000025f;

	const FVoxelFieldSampleContext Ctx = FVoxelFieldSampleContext::FromConfiguration(Config);
	if (!TestTrue(TEXT("Context is valid"), Ctx.IsValid()) || !TestTrue(TEXT("The context knows it is a planet"), Ctx.bPlanet))
	{
		return false;
	}

	const FVoxelEditorField* Height = FVoxelFieldRegistry::FindField(TEXT("Height"));
	const FVoxelEditorField* Slope = FVoxelFieldRegistry::FindField(TEXT("Slope"));
	const FVoxelEditorField* Temperature = FVoxelFieldRegistry::FindField(TEXT("Temperature"));
	if (!TestNotNull(TEXT("Fields registered"), Height) || !Slope || !Temperature)
	{
		return false;
	}
	const double Offsets[] = { -150000.0, -50000.0, 0.0, 30000.0, 120000.0 }; // inside the planet's disc
	for (double DX : Offsets)
	{
		for (double DY : Offsets)
		{
			const double X = Config->WorldOrigin.X + DX;
			const double Y = Config->WorldOrigin.Y + DY;
			const FVector Direction = Ctx.PlanetDirectionAbove(X, Y);
			const FVoxelTerrainSurfacePoint Surface = Ctx.WorldMode->GetSurfacePoint(Config->WorldOrigin + Direction, Ctx.NoiseParams);
			TestTrue(TEXT("The direction is on the upper hemisphere"), Direction.Z > 0.0);
			TestTrue(FString::Printf(TEXT("Height is the altitude above (%.0f, %.0f)"), DX, DY),
				FMath::IsNearlyEqual(Height->Sample(Ctx, X, Y, 0.0), Surface.Altitude, 0.01f));
			const float ExpectedSlope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Surface.Normal, Direction), -1.0, 1.0)));
			TestTrue(FString::Printf(TEXT("Slope against local up at (%.0f, %.0f)"), DX, DY),
				FMath::IsNearlyEqual(Slope->Sample(Ctx, X, Y, 0.0), ExpectedSlope, 0.01f));
			// A planet's climate is the planet noise (spherical Phase 6, WM-BP5)
			const float ExpectedTemperature = FVoxelNoise::FBM3D(Direction * Config->WorldRadius, Ctx.TempNoiseParams);
			TestTrue(TEXT("Temperature at the datum-sphere point"),
				FMath::IsNearlyEqual(Temperature->Sample(Ctx, X, Y, 0.0), ExpectedTemperature, 1.0e-5f));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
