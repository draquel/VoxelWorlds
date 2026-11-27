// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "VoxelWaterMesher.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The water tile's column mask (G294): FVoxelWaterMesher::BuildColumnMask walks a chunk's voxels in the array's order, a
// slice at a time; before G294 it walked up each column, a slice apart in memory per voxel, stopping at the column's
// first water (BuildColumnMaskByColumn, kept for voxel.Water.StridedScans). The two must give the same mask and the same
// answer on any voxels: seabeds at random heights, water in some columns only, water under an underground flag (a cave's,
// which the mask leaves out), solid voxels flagged wet, a mask already holding wet columns (both only add to it), and
// chunks of 16, 32 and 64 voxels. The times of the two on 64-voxel chunks are logged as "[ColumnMask]" lines. Pure CPU.
// ---------------------------------------------------------------------------

namespace VoxelWaterColumnMaskTests
{
	/** Voxels for one case: a seabed at a height per column, water over it up to a level, noise in the flags. */
	static TArray<FVoxelData> MakeVoxels(int32 CS, int32 Seed, float WetColumnShare)
	{
		FRandomStream Random(Seed);
		TArray<FVoxelData> Voxels;
		Voxels.SetNum(CS * CS * CS);
		const int32 Level = Random.RandRange(0, CS);
		for (int32 Y = 0; Y < CS; ++Y)
		{
			for (int32 X = 0; X < CS; ++X)
			{
				const int32 Bed = Random.RandRange(0, CS);
				const bool bWetColumn = Random.FRand() < WetColumnShare;
				for (int32 Z = 0; Z < CS; ++Z)
				{
					FVoxelData& V = Voxels[X + Y * CS + Z * CS * CS];
					V = Z < Bed ? FVoxelData::Solid(1) : FVoxelData::Air();
					if (bWetColumn && Z >= Bed && Z < Level)
					{
						V.SetWaterFlag(true);
					}
					// A cave's air: the mask leaves its water out
					if (Random.FRand() < 0.02f)
					{
						V.SetUndergroundFlag(true);
						V.SetWaterFlag(true);
					}
					// A solid voxel with the water flag (a voxel filled over a flooded one): not water
					if (Random.FRand() < 0.01f)
					{
						V.Density = 255;
						V.SetWaterFlag(true);
					}
				}
			}
		}
		return Voxels;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterColumnMaskOrderTest,
	"VoxelWorlds.Meshing.Water.ColumnMaskOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterColumnMaskOrderTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterColumnMaskTests;
	int32 Cases = 0;
	int32 WetCases = 0;
	int32 DryCases = 0;
	for (const int32 CS : { 16, 32, 64 })
	{
		for (int32 Seed = 0; Seed < 12; ++Seed)
		{
			const float Share = (Seed % 4) * 0.33f; // 0, a third, two thirds, all columns
			const TArray<FVoxelData> Voxels = MakeVoxels(CS, 1000 * CS + Seed, Share);
			// Half the cases start from a mask that already holds some wet columns
			TArray<bool> Start;
			Start.SetNumZeroed(CS * CS);
			if (Seed % 2)
			{
				for (int32 i = 0; i < Start.Num(); i += 7)
				{
					Start[i] = true;
				}
			}
			TArray<bool> InOrder = Start;
			TArray<bool> ByColumn = Start;
			const bool bInOrder = FVoxelWaterMesher::BuildColumnMask(Voxels, CS, InOrder);
			const bool bByColumn = FVoxelWaterMesher::BuildColumnMaskByColumn(Voxels, CS, ByColumn);
			++Cases;
			WetCases += bInOrder ? 1 : 0;
			DryCases += bInOrder ? 0 : 1;
			if (!TestEqual(*FString::Printf(TEXT("chunk %d, seed %d: the same answer"), CS, Seed), bInOrder, bByColumn)
				|| !TestTrue(*FString::Printf(TEXT("chunk %d, seed %d: the same mask"), CS, Seed), InOrder == ByColumn))
			{
				return false;
			}
		}
	}
	TestTrue(TEXT("some cases have water and some none (non-vacuous)"), WetCases > 0 && DryCases > 0);

	// A short input is refused by both, the mask untouched
	{
		TArray<FVoxelData> Short;
		Short.SetNum(10);
		TArray<bool> Mask;
		Mask.SetNumZeroed(16 * 16);
		TestFalse(TEXT("a short voxel array: no water"), FVoxelWaterMesher::BuildColumnMask(Short, 16, Mask));
		TestFalse(TEXT("a short voxel array: no water (by column)"), FVoxelWaterMesher::BuildColumnMaskByColumn(Short, 16, Mask));
		TestFalse(TEXT("a short voxel array: the mask untouched"), Mask.Contains(true));
	}

	// The times, for reading: a 64-voxel chunk with no water (the whole volume read by both) and one with water everywhere
	for (const float Share : { 0.0f, 1.0f })
	{
		const TArray<FVoxelData> Voxels = MakeVoxels(64, 77, Share);
		TArray<bool> Mask;
		// Warm (the voxels in the caches from the run before) and cold (64 MB read between runs, as a chunk the game thread
		// reads first after a worker generated it)
		TArray<uint32> Evict;
		Evict.SetNumZeroed(16 * 1024 * 1024);
		uint32 Sink = 0;
		double Seconds[2][2] = { { 0.0, 0.0 }, { 0.0, 0.0 } }; // [cold][by column]
		constexpr int32 Runs = 20;
		for (int32 Run = 0; Run < Runs; ++Run)
		{
			for (int32 Cold = 0; Cold < 2; ++Cold)
			{
				for (int32 ByColumn = 0; ByColumn < 2; ++ByColumn)
				{
					if (Cold)
					{
						for (int32 i = 0; i < Evict.Num(); i += 16)
						{
							Sink += Evict[i]++;
						}
					}
					Mask.SetNumZeroed(64 * 64);
					const double T0 = FPlatformTime::Seconds();
					if (ByColumn)
					{
						FVoxelWaterMesher::BuildColumnMaskByColumn(Voxels, 64, Mask);
					}
					else
					{
						FVoxelWaterMesher::BuildColumnMask(Voxels, 64, Mask);
					}
					Seconds[Cold][ByColumn] += FPlatformTime::Seconds() - T0;
				}
			}
		}
		const FString Line = FString::Printf(TEXT("[ColumnMask] 64-voxel chunk, %s: warm in order %.1f us, by column %.1f us; cold in order %.1f us, by column %.1f us (%u)"),
			Share > 0.0f ? TEXT("water in every column") : TEXT("no water"), Seconds[0][0] * 1e6 / Runs, Seconds[0][1] * 1e6 / Runs,
			Seconds[1][0] * 1e6 / Runs, Seconds[1][1] * 1e6 / Runs, Sink & 1);
		AddInfo(Line);
		UE_LOG(LogTemp, Log, TEXT("%s"), *Line);
	}
	AddInfo(FString::Printf(TEXT("%d cases compared (%d with water)"), Cases, WetCases));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
