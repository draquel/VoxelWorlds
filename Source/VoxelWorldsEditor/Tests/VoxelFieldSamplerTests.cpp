// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "VoxelFieldTypes.h"
#include "VoxelFieldImageBaker.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelSurfaceQuery.h"
#include "IVoxelWorldMode.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
