// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "VoxelBiomeBlendDither.h"
#include "VoxelBiomeSnapshot.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G39 (WORLD_MODES_REVIEW_PLAN.md §14.1). A biome blend zone's material pick used frac(W0 * 17.3 + W1 * 31.7 + depth
// * 0.1): the weights change slowly across a zone, so the pick followed the voxel's depth under the surface, which cycles
// with the grid on a slope, and drew bands of two materials along the voxel grid. Now the value is a lattice dither of
// the voxel's key (FVoxelBiomeBlendDither) and the biomes are walked in BiomeID order.
//
// - Biome.BlendDither.Uniform: the equalised dither is uniform on [0, 1) (16 bins within 4% of 1/16 over 2M voxels) and a
//   biome takes its weight's share of the voxels (within 0.01 at 0.1 ... 0.9).
// - Biome.BlendDither.Patches: neighbouring voxels mostly agree (patches, not speckle): at weight 0.5 two voxels a step
//   apart take the same biome 80% of the time or more, and on a lattice point exactly the key's hash decides.
// - Biome.BlendDither.Keys: a chunk's GridBase plus a voxel's local index is KeyAt at the voxel's sample point, for an
//   origin off the voxel grid, negative chunks included.
// - Biome.BlendDither.NoGridBands: a gentle slope through a two-biome zone at weight 0.7. By the surface voxel's depth
//   phase (where the surface crosses the voxel's layer, in 8 bins), the old pick's share of the dominant biome swings by
//   most of the range (the bands); the dither's stays within 0.05.
// - Biome.BlendDither.DominanceFlip: where the dominant biome changes inside a zone (the weights crossing 0.5), the old
//   weight-order walk changes almost every voxel's pick; the BiomeID-order walk changes a few in a thousand.
// ---------------------------------------------------------------------------

namespace BiomeBlendDitherTestUtils
{
	/** Sets voxel.Biome.BlendDither for a scope. */
	struct FScopedBlendDither
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedBlendDither(int32 Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Biome.BlendDither"));
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedBlendDither()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/** A two-biome blend: biome 3 (weight W3) and biome 7 (1 - W3), sorted by weight as GetBiomeBlend sorts them. */
	FBiomeBlend TwoBiomes(float W3)
	{
		FBiomeBlend Blend;
		Blend.BiomeCount = 2;
		const bool b3First = W3 >= 0.5f;
		Blend.BiomeIDs[0] = b3First ? 3 : 7;
		Blend.BiomeIDs[1] = b3First ? 7 : 3;
		Blend.Weights[0] = b3First ? W3 : 1.0f - W3;
		Blend.Weights[1] = b3First ? 1.0f - W3 : W3;
		return Blend;
	}

	/** Whether a voxel takes biome 3. */
	bool Takes3(const FBiomeBlend& Blend, float Depth, const FIntVector& Key)
	{
		return Blend.BiomeIDs[FVoxelBiomeSnapshot::PickBlendIndex(Blend, Depth, FVoxelBlendDitherKey(Key, 1337))] == 3;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeBlendDitherUniformTest, "VoxelWorlds.Biome.BlendDither.Uniform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeBlendDitherUniformTest::RunTest(const FString& Parameters)
{
	using namespace BiomeBlendDitherTestUtils;
	FScopedBlendDither Scoped(1);
	FRandomStream Random(39);
	constexpr int32 N = 2000000;
	int32 Bins[16] = {};
	TArray<float> Values;
	Values.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i)
	{
		const FIntVector Key(Random.RandRange(-3000000, 3000000), Random.RandRange(-3000000, 3000000), Random.RandRange(-30000, 30000));
		Values[i] = FVoxelBiomeBlendDither::Sample(FVoxelBlendDitherKey(Key, 7));
		++Bins[FMath::Clamp(FMath::FloorToInt32(Values[i] * 16.0f), 0, 15)];
	}
	float WorstBin = 0.0f;
	for (const int32 Count : Bins)
	{
		WorstBin = FMath::Max(WorstBin, FMath::Abs(Count / (N / 16.0f) - 1.0f));
	}
	AddInfo(FString::Printf(TEXT("16 bins over %d voxels: the worst %.2f%% off an even share"), N, WorstBin * 100.0f));
	TestTrue(TEXT("The dither is uniform (every bin within 4% of 1/16)"), WorstBin < 0.04f);
	float WorstCoverage = 0.0f;
	for (const float Weight : { 0.1f, 0.25f, 0.5f, 0.75f, 0.9f })
	{
		int32 Below = 0;
		for (const float V : Values)
		{
			Below += V < Weight ? 1 : 0;
		}
		const float Coverage = static_cast<float>(Below) / N;
		WorstCoverage = FMath::Max(WorstCoverage, FMath::Abs(Coverage - Weight));
		AddInfo(FString::Printf(TEXT("Weight %.2f: %.4f of the voxels under it"), Weight, Coverage));
	}
	TestTrue(TEXT("A biome takes its weight's share (within 0.01)"), WorstCoverage < 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeBlendDitherPatchesTest, "VoxelWorlds.Biome.BlendDither.Patches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeBlendDitherPatchesTest::RunTest(const FString& Parameters)
{
	using namespace BiomeBlendDitherTestUtils;
	FScopedBlendDither Scoped(1);
	const FBiomeBlend Blend = TwoBiomes(0.5f);
	FRandomStream Random(40);
	int32 Same[3] = {}, Pairs = 0;
	for (int32 i = 0; i < 200000; ++i)
	{
		const FIntVector Key(Random.RandRange(-100000, 100000), Random.RandRange(-100000, 100000), Random.RandRange(-1000, 1000));
		const bool bHere = Takes3(Blend, 0.3f, Key);
		Same[0] += (bHere == Takes3(Blend, 0.3f, Key + FIntVector(1, 0, 0))) ? 1 : 0;
		Same[1] += (bHere == Takes3(Blend, 0.3f, Key + FIntVector(0, 1, 0))) ? 1 : 0;
		Same[2] += (bHere == Takes3(Blend, 0.3f, Key + FIntVector(0, 0, 1))) ? 1 : 0;
		++Pairs;
	}
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float Share = static_cast<float>(Same[Axis]) / Pairs;
		AddInfo(FString::Printf(TEXT("Axis %d: neighbours take the same biome %.3f of the time (0.5 for independent picks)"), Axis, Share));
		TestTrue(*FString::Printf(TEXT("Axis %d: patches, not speckle"), Axis), Share >= 0.8f);
	}

	// Deterministic, and the depth does not enter it: the same voxel takes the same biome at any depth.
	int32 DepthChanges = 0;
	for (int32 i = 0; i < 10000; ++i)
	{
		const FIntVector Key(Random.RandRange(-100000, 100000), Random.RandRange(-100000, 100000), Random.RandRange(-1000, 1000));
		DepthChanges += (Takes3(Blend, 0.0f, Key) != Takes3(Blend, 0.73f, Key)) ? 1 : 0;
	}
	TestEqual(TEXT("The depth under the surface plays no part"), DepthChanges, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeBlendDitherKeysTest, "VoxelWorlds.Biome.BlendDither.Keys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeBlendDitherKeysTest::RunTest(const FString& Parameters)
{
	struct FCase { FVector Origin; float VoxelSize; int32 ChunkSize; };
	int32 Checked = 0, Wrong = 0;
	for (const FCase& Case : { FCase{ FVector(12345.0, 54321.0, 0.0), 75.0f, 64 }, FCase{ FVector(3112.5, -1937.5, -2187.5), 50.0f, 32 },
		FCase{ FVector::ZeroVector, 100.0f, 16 } })
	{
		FRandomStream Random(41);
		for (int32 i = 0; i < 20000; ++i)
		{
			const FIntVector Chunk(Random.RandRange(-5000, 5000), Random.RandRange(-5000, 5000), Random.RandRange(-200, 200));
			const FIntVector Local(Random.RandRange(0, Case.ChunkSize - 1), Random.RandRange(0, Case.ChunkSize - 1), Random.RandRange(0, Case.ChunkSize - 1));
			const FVector Sample = Case.Origin + FVector(Chunk * Case.ChunkSize + Local) * Case.VoxelSize;
			const FIntVector FromGrid = FVoxelBiomeBlendDither::GridBase(Chunk, Case.ChunkSize, Case.Origin, Case.VoxelSize) + Local;
			Wrong += (FromGrid != FVoxelBiomeBlendDither::KeyAt(Sample, Case.VoxelSize)) ? 1 : 0;
			++Checked;
		}
	}
	AddInfo(FString::Printf(TEXT("%d voxels: GridBase + local against KeyAt at the sample point"), Checked));
	TestEqual(TEXT("Generation and the analytic queries find the same key"), Wrong, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeBlendDitherNoGridBandsTest, "VoxelWorlds.Biome.BlendDither.NoGridBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeBlendDitherNoGridBandsTest::RunTest(const FString& Parameters)
{
	using namespace BiomeBlendDitherTestUtils;
	// A gentle slope (0.13 voxels of rise per voxel, diagonally across the grid) at a fixed blend weight. Each column's
	// surface voxel is the highest at or under the surface; its depth phase is where the surface crosses its layer.
	const FBiomeBlend Blend = TwoBiomes(0.7f);
	auto PhaseSpread = [&](int32 Mode, float& OutMin, float& OutMax)
	{
		FScopedBlendDither Scoped(Mode);
		int32 Count[8] = {}, Took[8] = {};
		for (int32 X = -600; X < 600; ++X)
		{
			for (int32 Y = -600; Y < 600; ++Y)
			{
				const double Height = 0.13 * (0.8 * X + 0.6 * Y) + 0.37; // in voxels
				const int32 TopZ = FMath::FloorToInt32(Height);
				const float Depth = static_cast<float>(Height - TopZ);
				const int32 Bin = FMath::Clamp(FMath::FloorToInt32(Depth * 8.0f), 0, 7);
				++Count[Bin];
				Took[Bin] += Takes3(Blend, Depth, FIntVector(X, Y, TopZ)) ? 1 : 0;
			}
		}
		OutMin = 1.0f;
		OutMax = 0.0f;
		for (int32 b = 0; b < 8; ++b)
		{
			const float Share = Count[b] > 0 ? static_cast<float>(Took[b]) / Count[b] : 0.0f;
			OutMin = FMath::Min(OutMin, Share);
			OutMax = FMath::Max(OutMax, Share);
		}
	};
	float OldMin, OldMax, NewMin, NewMax;
	PhaseSpread(0, OldMin, OldMax);
	PhaseSpread(1, NewMin, NewMax);
	AddInfo(FString::Printf(TEXT("Weight 0.7, the dominant biome's share by the surface's depth phase (8 bins): before %.3f-%.3f, the dither %.3f-%.3f"),
		OldMin, OldMax, NewMin, NewMax));
	TestTrue(TEXT("Before: the share follows the grid's phase (bands; non-vacuous)"), OldMax - OldMin > 0.5f);
	TestTrue(TEXT("The dither: the same share at every phase (within 0.05)"), NewMax - NewMin < 0.05f);
	TestTrue(TEXT("The dither: the dominant biome's share is its weight (within 0.03)"), FMath::Abs(0.5f * (NewMin + NewMax) - 0.7f) < 0.03f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeBlendDitherDominanceFlipTest, "VoxelWorlds.Biome.BlendDither.DominanceFlip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeBlendDitherDominanceFlipTest::RunTest(const FString& Parameters)
{
	using namespace BiomeBlendDitherTestUtils;
	// The same voxels with biome 3 just dominant and just not (sorted by weight, so the blend's order flips): a walk in
	// weight order would read the dither against the other biome; the BiomeID walk changes only voxels between the two
	// weights.
	FScopedBlendDither Scoped(1);
	const FBiomeBlend Before = TwoBiomes(0.501f);
	const FBiomeBlend After = TwoBiomes(0.499f);
	FRandomStream Random(42);
	int32 Changed = 0;
	constexpr int32 N = 200000;
	for (int32 i = 0; i < N; ++i)
	{
		const FIntVector Key(Random.RandRange(-100000, 100000), Random.RandRange(-100000, 100000), Random.RandRange(-1000, 1000));
		Changed += (Takes3(Before, 0.4f, Key) != Takes3(After, 0.4f, Key)) ? 1 : 0;
	}
	// The weight-order walk for comparison: biome 3 if the dither is under the first entry's weight when 3 is first.
	int32 ChangedByWeightOrder = 0;
	for (int32 i = 0; i < N; ++i)
	{
		const FIntVector Key(Random.RandRange(-100000, 100000), Random.RandRange(-100000, 100000), Random.RandRange(-1000, 1000));
		const float R = FVoxelBiomeBlendDither::Sample(FVoxelBlendDitherKey(Key, 1337));
		const bool b3Before = R < 0.501f;  // 3 first, weight 0.501
		const bool b3After = !(R < 0.501f); // 7 first, weight 0.501: 3 when the value is past it
		ChangedByWeightOrder += (b3Before != b3After) ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("Across the flip: the BiomeID walk changes %d of %d voxels, a walk in weight order %d"), Changed, N, ChangedByWeightOrder));
	TestTrue(TEXT("The BiomeID walk changes only the voxels between the weights (under 0.5%)"), Changed < N / 200);
	TestTrue(TEXT("A walk in weight order would change nearly all (non-vacuous)"), ChangedByWeightOrder > N * 9 / 10);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
