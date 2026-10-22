// Copyright Daniel Raquel. All Rights Reserved.

// G69 (WORLD_MODES_REVIEW_PLAN.md §14.1, §20.15): a chunk's boundary shell holds exactly the voxels within its depth of each
// face, and answers for every one of them as the whole chunk does.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelChunkShell.h"
#include "Math/RandomStream.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelChunkShellExtractTest,
	"VoxelWorlds.Compression.ChunkShell.Extract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelChunkShellExtractTest::RunTest(const FString& Parameters)
{
	// Random voxels (every field), so a voxel read from the wrong slab or plane shows.
	for (const int32 CS : { 8, 16, 64 })
	{
		FRandomStream Rng(1234 + CS);
		TArray<FVoxelData> Full;
		Full.SetNumUninitialized(CS * CS * CS);
		for (FVoxelData& V : Full)
		{
			V = FVoxelData(static_cast<uint8>(Rng.RandRange(0, 255)), static_cast<uint8>(Rng.RandRange(0, 255)),
				static_cast<uint8>(Rng.RandRange(0, 255)), static_cast<uint8>(Rng.RandRange(0, 255)));
		}
		for (const int32 Depth : { 1, 2, 3, 5, CS / 2, CS })
		{
			FVoxelChunkShell Shell;
			TestTrue(FString::Printf(TEXT("CS %d depth %d: extracted"), CS, Depth), FVoxelChunkShell::Extract(Full, CS, Depth, Shell));
			TestEqual(FString::Printf(TEXT("CS %d depth %d: size"), CS, Depth), Shell.Slabs.Num(), 6 * Depth * CS * CS);
			int32 Held = 0, Wrong = 0, HeldWrongly = 0;
			for (int32 Z = 0; Z < CS; ++Z)
			{
				for (int32 Y = 0; Y < CS; ++Y)
				{
					for (int32 X = 0; X < CS; ++X)
					{
						const bool bNearFace = X < Depth || Y < Depth || Z < Depth || X >= CS - Depth || Y >= CS - Depth || Z >= CS - Depth;
						if (Shell.Contains(X, Y, Z) != bNearFace)
						{
							++HeldWrongly;
						}
						if (!bNearFace)
						{
							continue;
						}
						++Held;
						const FVoxelData A = Shell.Get(X, Y, Z);
						const FVoxelData& B = Full[X + Y * CS + Z * CS * CS];
						if (A.MaterialID != B.MaterialID || A.Density != B.Density || A.BiomeID != B.BiomeID || A.Metadata != B.Metadata)
						{
							++Wrong;
						}
					}
				}
			}
			TestEqual(FString::Printf(TEXT("CS %d depth %d: Contains is the within-depth rule"), CS, Depth), HeldWrongly, 0);
			TestEqual(FString::Printf(TEXT("CS %d depth %d: every voxel held reads as the chunk's (%d held)"), CS, Depth, Held), Wrong, 0);
			TestFalse(FString::Printf(TEXT("CS %d depth %d: out of the chunk"), CS, Depth), Shell.Contains(-1, 0, 0) || Shell.Contains(0, CS, 0));
		}
	}

	// A chunk of the wrong size makes no shell.
	{
		TArray<FVoxelData> Short;
		Short.Init(FVoxelData::Air(), 100);
		FVoxelChunkShell Shell;
		TestFalse(TEXT("wrong size refused"), FVoxelChunkShell::Extract(Short, 8, 1, Shell));
		TestEqual(TEXT("wrong size leaves it empty"), Shell.Slabs.Num(), 0);
		TestTrue(TEXT("an empty shell reads air"), Shell.Get(0, 0, 0).Density == FVoxelData::Air().Density);
	}

	// A uniform chunk's shell is its value everywhere it holds; the memory is the faces only.
	{
		const FVoxelData Rock = FVoxelData::Solid(7);
		FVoxelChunkShell Shell;
		FVoxelChunkShell::FillUniform(Rock, 64, 1, Shell);
		TestEqual(TEXT("uniform: size"), Shell.Slabs.Num(), 6 * 64 * 64);
		TestTrue(TEXT("uniform: a corner"), Shell.Get(63, 63, 63).MaterialID == 7 && Shell.Get(63, 63, 63).Density == Rock.Density);
		TestTrue(TEXT("uniform: a face"), Shell.Get(0, 30, 20).MaterialID == 7);
		TestEqual(TEXT("uniform: depth-1 shell of 64^3 is 96 KB"), static_cast<int64>(Shell.Slabs.Num() * sizeof(FVoxelData)), 96LL * 1024);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
