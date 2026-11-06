// Copyright Daniel Raquel. All Rights Reserved.

// The climate options (UVoxelBiomeConfiguration's Climate category and bEqualizeContinentalness) and the ridged terrain
// fBm (FVoxelNoiseParams::bRidged): the shared CDF table against the field it equalises, the shaping rules, and the
// ridged fBm's range and its double reference. The GPU's copies are held to these by the GPUvsCPU parity tests.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelGenerationShared.ush"
#include "VoxelNoise.h"

namespace ClimateShapingTestUtils
{
	/** The field the table describes: a 2-octave climate field, sampled over four plane tiles of ~200 x 200 features. */
	static void SampleFieldCdf(TArray<float>& OutCdf, int32& OutSamples)
	{
		const FVoxelNoiseParams Field = FVoxelNoise::MakeFieldParams(0x2C11, 0.001f);
		constexpr int32 Grid = 2048;
		constexpr double Step = 97.0;   // uu: ~0.1 of a feature, so a tile spans ~200 features a side
		TArray<int64> Counts;
		Counts.Init(0, VOXELGEN_CLIMATE_CDF_COUNT);
		OutSamples = 0;
		for (int32 Tile = 0; Tile < 4; ++Tile)
		{
			// Four plane tiles far apart: ~160,000 features in all, the CDF within ~0.002 of the field's
			const FVector Base(Tile * 4.0e7, -Tile * 3.0e7, 0.0);
			for (int32 Y = 0; Y < Grid; ++Y)
			{
				for (int32 X = 0; X < Grid; X += 2)
				{
					const float V = FVoxelNoise::FBM3D(Base + FVector(X * Step, Y * Step, 0.0), Field);
					// Count the values below each table point: bucket the value at the first point above it
					const float Position = (FMath::Clamp(V, -1.0f, 1.0f) + 1.0f) * 0.5f * (VOXELGEN_CLIMATE_CDF_COUNT - 1);
					const int32 Above = FMath::Clamp(FMath::FloorToInt(Position) + 1, 0, VOXELGEN_CLIMATE_CDF_COUNT - 1);
					++Counts[Above];
					++OutSamples;
				}
			}
		}
		OutCdf.SetNum(VOXELGEN_CLIMATE_CDF_COUNT);
		int64 Running = 0;
		for (int32 i = 0; i < VOXELGEN_CLIMATE_CDF_COUNT; ++i)
		{
			Running += Counts[i];
			OutCdf[i] = static_cast<float>(static_cast<double>(Running) / OutSamples);
		}
		OutCdf[0] = 0.0f;
		OutCdf.Last() = 1.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelClimateEqualizeTableTest, "VoxelWorlds.Biome.ClimateEqualize.Table",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelClimateEqualizeTableTest::RunTest(const FString& Parameters)
{
	// The shared table is the climate field's measured CDF: within 0.004 of a fresh measurement at every point. On a
	// drift the test prints the measured table in the shared header's form, to paste into VoxelGenerationShared.ush.
	TArray<float> Measured;
	int32 Samples = 0;
	ClimateShapingTestUtils::SampleFieldCdf(Measured, Samples);
	static constexpr float Table[VOXELGEN_CLIMATE_CDF_COUNT] = VOXELGEN_CLIMATE_CDF;
	float MaxDiff = 0.0f;
	for (int32 i = 0; i < VOXELGEN_CLIMATE_CDF_COUNT; ++i)
	{
		MaxDiff = FMath::Max(MaxDiff, FMath::Abs(Measured[i] - Table[i]));
	}
	if (MaxDiff > 0.004f)
	{
		FString Rows;
		for (int32 i = 0; i < VOXELGEN_CLIMATE_CDF_COUNT; ++i)
		{
			Rows += FString::Printf(TEXT("%s%.6ff%s"), (i % 8 == 0) ? TEXT("\t") : TEXT(""), Measured[i],
				i + 1 < VOXELGEN_CLIMATE_CDF_COUNT ? ((i % 8 == 7) ? TEXT(", \\\n") : TEXT(", ")) : TEXT(" \\\n"));
		}
		AddInfo(FString::Printf(TEXT("Measured climate CDF (%d samples):\n#define VOXELGEN_CLIMATE_CDF { \\\n%s}"), Samples, *Rows));
	}
	TestTrue(FString::Printf(TEXT("The shared CDF table matches the field within 0.004 (max %.5f over %d samples)"), MaxDiff, Samples),
		MaxDiff <= 0.004f);

	// The table is a CDF: from 0 to 1, never decreasing.
	bool bMonotonic = Table[0] == 0.0f && Table[VOXELGEN_CLIMATE_CDF_COUNT - 1] == 1.0f;
	for (int32 i = 1; i < VOXELGEN_CLIMATE_CDF_COUNT; ++i)
	{
		bMonotonic &= Table[i] >= Table[i - 1];
	}
	TestTrue(TEXT("The table runs from 0 to 1 and never decreases"), bMonotonic);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelClimateEqualizeUniformTest, "VoxelWorlds.Biome.ClimateEqualize.Uniform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelClimateEqualizeUniformTest::RunTest(const FString& Parameters)
{
	// Equalised, a climate field covers [-1, 1] evenly: each tenth of the range holds a tenth of the samples, within 0.01,
	// where the raw field puts most of them in the middle fifth.
	const FVoxelNoiseParams Field = FVoxelNoise::MakeFieldParams(0x51F7, 0.0007f);
	int32 Buckets[10] = {};
	int32 RawMiddle = 0;
	int32 N = 0;
	for (int32 Y = 0; Y < 600; ++Y)
	{
		for (int32 X = 0; X < 600; ++X)
		{
			const float Raw = FVoxelNoise::FBM3D(FVector(X * 401.0 + 5.0e6, Y * 397.0 - 2.0e6, 0.0), Field);
			const float Equalised = FVoxelBiomeSnapshot::EqualizeField(Raw);
			++Buckets[FMath::Clamp(FMath::FloorToInt((Equalised + 1.0f) * 5.0f), 0, 9)];
			RawMiddle += FMath::Abs(Raw) < 0.2f;
			++N;
		}
	}
	float WorstShare = 0.0f;
	for (const int32 Count : Buckets)
	{
		WorstShare = FMath::Max(WorstShare, FMath::Abs(static_cast<float>(Count) / N - 0.1f));
	}
	TestTrue(FString::Printf(TEXT("Each tenth of the equalised range holds 0.1 of the samples within 0.01 (worst off by %.4f)"), WorstShare),
		WorstShare <= 0.01f);
	AddInfo(FString::Printf(TEXT("Raw field: %.2f of the samples within 0.2 of 0"), static_cast<float>(RawMiddle) / N));

	// Equalising keeps the order of values (a monotonic remap) and the ends of the range.
	bool bOrdered = true;
	float Previous = -2.0f;
	for (int32 i = 0; i <= 400; ++i)
	{
		const float V = FVoxelBiomeSnapshot::EqualizeField(-1.0f + i * 0.005f);
		bOrdered &= V >= Previous;
		Previous = V;
	}
	TestTrue(TEXT("Equalising never reverses two values"), bOrdered);
	TestEqual(TEXT("-1 stays -1"), FVoxelBiomeSnapshot::EqualizeField(-1.0f), -1.0f);
	TestEqual(TEXT("1 stays 1"), FVoxelBiomeSnapshot::EqualizeField(1.0f), 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelClimateShapingTest, "VoxelWorlds.Biome.ClimateShaping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelClimateShapingTest::RunTest(const FString& Parameters)
{
	// Off (the default snapshot) every Shape* returns its input: no world changes until it opts in.
	const FVoxelBiomeSnapshot Off;
	bool bIdentity = true;
	for (int32 i = 0; i <= 20; ++i)
	{
		const float V = -1.0f + i * 0.1f;
		bIdentity &= Off.ShapeTemperature(V, 12345.0f, 0.7f, true) == V;
		bIdentity &= Off.ShapeMoisture(V) == V && Off.ShapeContinentalness(V) == V;
	}
	TestTrue(TEXT("With every option off the climate is the raw fields"), bIdentity);

	// Latitude (E1): at weight 1 the temperature is 1 at the equator and -1 at the poles, linear in sin(latitude); heightmap
	// worlds (no latitude) are unchanged.
	FVoxelBiomeSnapshot Latitude;
	Latitude.TemperatureLatitudeWeight = 1.0f;
	TestTrue(TEXT("Equator: 1"), FMath::IsNearlyEqual(Latitude.ShapeTemperature(-0.4f, 0.0f, 0.0f, true), 1.0f));
	TestTrue(TEXT("North pole: -1"), FMath::IsNearlyEqual(Latitude.ShapeTemperature(0.4f, 0.0f, 1.0f, true), -1.0f));
	TestTrue(TEXT("South pole: -1"), FMath::IsNearlyEqual(Latitude.ShapeTemperature(0.4f, 0.0f, -1.0f, true), -1.0f));
	TestTrue(TEXT("Latitude 30 (sin 0.5): 0"), FMath::IsNearlyEqual(Latitude.ShapeTemperature(0.9f, 0.0f, 0.5f, true), 0.0f, 1e-6f));
	TestEqual(TEXT("No latitude on a heightmap"), Latitude.ShapeTemperature(0.3f, 0.0f, 1.0f, false), 0.3f);
	Latitude.TemperatureLatitudeWeight = 0.5f;
	TestTrue(TEXT("Weight 0.5: halfway between the field and latitude"),
		FMath::IsNearlyEqual(Latitude.ShapeTemperature(0.2f, 0.0f, 1.0f, true), -0.4f, 1e-6f));

	// Lapse (E2): the rate per 10,000 uu above the datum, nothing below it.
	FVoxelBiomeSnapshot Lapse;
	Lapse.TemperatureLapseRate = 0.5f;
	TestTrue(TEXT("20,000 uu up at 0.5 per 10,000: 1 colder"), FMath::IsNearlyEqual(Lapse.ShapeTemperature(0.3f, 20000.0f, 0.0f, false), -0.7f, 1e-6f));
	TestEqual(TEXT("Under the datum: no lapse"), Lapse.ShapeTemperature(0.3f, -5000.0f, 0.0f, false), 0.3f);

	// Equalisation (E3) on temperature and moisture together; continentalness on its own flag.
	FVoxelBiomeSnapshot Equalise;
	Equalise.bEqualizeClimate = true;
	TestEqual(TEXT("Temperature equalised"), Equalise.ShapeTemperature(0.25f, 0.0f, 0.0f, false), FVoxelBiomeSnapshot::EqualizeField(0.25f));
	TestEqual(TEXT("Moisture equalised"), Equalise.ShapeMoisture(-0.25f), FVoxelBiomeSnapshot::EqualizeField(-0.25f));
	TestEqual(TEXT("Continentalness has its own flag"), Equalise.ShapeContinentalness(0.25f), 0.25f);
	Equalise.bEqualizeContinentalness = true;
	TestEqual(TEXT("Continentalness equalised"), Equalise.ShapeContinentalness(0.25f), FVoxelBiomeSnapshot::EqualizeField(0.25f));

	// The order: equalise, then latitude, then the lapse.
	FVoxelBiomeSnapshot All;
	All.bEqualizeClimate = true;
	All.TemperatureLatitudeWeight = 0.5f;
	All.TemperatureLapseRate = 0.1f;
	const float Expected = (FVoxelBiomeSnapshot::EqualizeField(0.2f) + 1.0f) * 0.5f - 0.1f;
	TestTrue(TEXT("Equalised, leaned to the equator, then 10,000 uu of lapse"),
		FMath::IsNearlyEqual(All.ShapeTemperature(0.2f, 10000.0f, 0.0f, true), Expected, 1e-6f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNoiseRidgedTest, "VoxelWorlds.Noise.Ridged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNoiseRidgedTest::RunTest(const FString& Parameters)
{
	// The ridged fBm (E4) stays in [-1, 1], differs from the standard one, and matches its double reference and the
	// split-origin form the GPU uses, as the standard fBm does (VoxelWorlds.Noise.SplitOrigin).
	FRandomStream Random(0x41D6);
	FVoxelNoiseParams Ridged;
	Ridged.Frequency = 0.0004f;
	Ridged.Octaves = 6;
	Ridged.Seed = 77;
	Ridged.bRidged = true;
	FVoxelNoiseParams Standard = Ridged;
	Standard.bRidged = false;
	float MaxAbs = 0.0f, MaxRefDiff = 0.0f, MaxSplitDiff = 0.0f;
	int32 Differ = 0;
	for (int32 i = 0; i < 3000; ++i)
	{
		const FVector P(Random.FRandRange(-2.0e6, 2.0e6), Random.FRandRange(-2.0e6, 2.0e6), 0.0);
		const float V = FVoxelNoise::FBM3D(P, Ridged);
		MaxAbs = FMath::Max(MaxAbs, FMath::Abs(V));
		MaxRefDiff = FMath::Max(MaxRefDiff, static_cast<float>(FMath::Abs(V - FVoxelNoise::ReferenceFBM3D(P, Ridged))));
		const FVector Origin(FMath::RoundToDouble(P.X / 4096.0) * 4096.0, FMath::RoundToDouble(P.Y / 4096.0) * 4096.0, 0.0);
		MaxSplitDiff = FMath::Max(MaxSplitDiff, FMath::Abs(V - FVoxelNoise::FBM3D(Origin, FVector3f(P - Origin), Ridged)));
		Differ += FMath::Abs(V - FVoxelNoise::FBM3D(P, Standard)) > 1e-3f;
	}
	TestTrue(FString::Printf(TEXT("Ridged fBm within [-1, 1] (max |v| %.4f)"), MaxAbs), MaxAbs <= 1.0f);
	TestTrue(FString::Printf(TEXT("Ridged fBm matches its double reference within 1e-4 (max %.2e)"), MaxRefDiff), MaxRefDiff <= 1e-4f);
	TestTrue(FString::Printf(TEXT("Ridged fBm at a split origin matches the direct form within 1e-4 (max %.2e)"), MaxSplitDiff),
		MaxSplitDiff <= 1e-4f);
	TestTrue(TEXT("Ridged differs from the standard fBm almost everywhere"), Differ > 2900);

	// Off by default: every field's parameters are unchanged.
	TestFalse(TEXT("bRidged defaults off"), FVoxelNoiseParams().bRidged);
	TestFalse(TEXT("Climate fields are not ridged"), FVoxelNoise::MakeFieldParams(1, 0.001f).bRidged);
	return true;
}
