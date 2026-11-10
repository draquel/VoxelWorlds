// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMapExploredMask.h"
#include "VoxelMapSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// MAP_PYRAMID_PLAN.md P2: the explored mask (FVoxelMapExploredMask), world-free, run headless.
//
// - Levels: random tiles on a plane (negative and far coordinates) and on a planet's faces; for every aligned square of
//   2^K tiles a side, K 0 to 14 (the bits, the squares in a block, the coarse summaries over blocks), IsAnyExplored and the
//   cached reader against a brute force over the tiles set; the block states.
// - Collapse: a block filled tile by tile collapses to its count; a planet face's last blocks are full at the tiles they
//   hold; a face filled as a rectangle is whole blocks; RemoveIf expands a full block, and a block emptied goes.
// - Blobs: the mask's blob round-trips (partial and full blocks, negative blocks), is refused when malformed, and a fully
//   explored 50 km planet (6 faces of 1,637 tiles) fills in milliseconds and saves in kilobytes.
// ---------------------------------------------------------------------------

namespace VoxelMapExploredMaskTest
{
	struct FTile
	{
		int32 Face = 0;
		int32 U = 0;
		int32 V = 0;
	};

	/** Brute force: any of the tiles in the aligned square of 2^K holding (U, V)? */
	bool AnyInSquare(const TArray<FTile>& Tiles, int32 Face, int32 U, int32 V, int32 K)
	{
		const int32 SU = U >> K;
		const int32 SV = V >> K;
		for (const FTile& T : Tiles)
		{
			if (T.Face == Face && (T.U >> K) == SU && (T.V >> K) == SV)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapExploredMaskLevelsTest,
	"VoxelWorlds.Map.ExploredMask.Levels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapExploredMaskLevelsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapExploredMaskTest;
	for (const bool bPlanet : { false, true })
	{
		FVoxelMapExploredMask Mask;
		const int32 N = 1637; // the 50 km planet's tiles a face edge
		Mask.Configure(bPlanet ? 6 : 1, bPlanet ? N : 0);
		FRandomStream Random(bPlanet ? 11 : 7);
		TArray<FTile> Tiles;
		// Clusters (a trail's) and scattered tiles; on the plane around negative and far coordinates.
		for (int32 Cluster = 0; Cluster < 12; ++Cluster)
		{
			const int32 Face = bPlanet ? Random.RandRange(0, 5) : 0;
			const int32 CU = bPlanet ? Random.RandRange(0, N - 1) : Random.RandRange(-200000, 200000);
			const int32 CV = bPlanet ? Random.RandRange(0, N - 1) : Random.RandRange(-200000, 200000);
			for (int32 I = 0; I < 60; ++I)
			{
				FTile T{ Face, CU + Random.RandRange(-40, 40), CV + Random.RandRange(-40, 40) };
				if (bPlanet)
				{
					T.U = FMath::Clamp(T.U, 0, N - 1);
					T.V = FMath::Clamp(T.V, 0, N - 1);
				}
				Mask.Set(T.Face, T.U, T.V);
				Tiles.Add(T);
			}
		}
		// Count: the distinct tiles.
		TSet<FIntVector> Distinct;
		for (const FTile& T : Tiles)
		{
			Distinct.Add(FIntVector(T.Face, T.U, T.V));
		}
		TestEqual(*FString::Printf(TEXT("%s: the count is the distinct tiles"), bPlanet ? TEXT("Planet") : TEXT("Plane")), Mask.Num(), static_cast<int64>(Distinct.Num()));
		TestFalse(TEXT("A tile set twice is not new"), Mask.Set(Tiles[0].Face, Tiles[0].U, Tiles[0].V));
		if (bPlanet)
		{
			TestFalse(TEXT("A tile off the face is refused"), Mask.Set(0, N, 0) || Mask.Set(6, 0, 0) || Mask.Set(0, -1, 3));
		}

		// Queries at every square size, at the tiles set and around them (hits and misses), the mask's and a reader's.
		int32 Checked = 0, Wrong = 0, ReaderWrong = 0, Hits = 0;
		FVoxelMapExploredMask::FReader Reader(Mask);
		for (int32 K = 0; K <= 14; ++K)
		{
			for (int32 I = 0; I < 300; ++I)
			{
				const FTile& Base = Tiles[Random.RandRange(0, Tiles.Num() - 1)];
				const int32 Spread = 1 << FMath::Min(K + 2, 20);
				FTile Q{ Base.Face, Base.U + Random.RandRange(-Spread, Spread), Base.V + Random.RandRange(-Spread, Spread) };
				if (bPlanet)
				{
					Q.Face = Random.RandRange(0, 9) == 0 ? Random.RandRange(0, 5) : Q.Face;
					Q.U = FMath::Clamp(Q.U, 0, N - 1);
					Q.V = FMath::Clamp(Q.V, 0, N - 1);
				}
				const bool bExpected = AnyInSquare(Tiles, Q.Face, Q.U, Q.V, K);
				const bool bGot = Mask.IsAnyExplored(Q.Face, Q.U, Q.V, K);
				Wrong += bGot != bExpected ? 1 : 0;
				ReaderWrong += Reader.IsAnyExplored(Q.Face, Q.U, Q.V, K) != bExpected ? 1 : 0;
				Hits += bExpected ? 1 : 0;
				++Checked;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %lld tiles in %d blocks; %d queries over squares of 1 to 2^14 tiles (%d explored), %d wrong, the reader %d"),
			bPlanet ? TEXT("Planet") : TEXT("Plane"), Mask.Num(), Mask.NumBlocks(), Checked, Hits, Wrong, ReaderWrong));
		TestTrue(TEXT("Both explored and unexplored squares asked"), Hits > 0 && Hits < Checked);
		TestEqual(TEXT("IsAnyExplored at every level is the brute force's"), Wrong, 0);
		TestEqual(TEXT("... and the cached reader's"), ReaderWrong, 0);

		// Block states.
		const FTile& T = Tiles[0];
		TestTrue(TEXT("A block with tiles is Some"), Mask.GetBlockState(T.Face, T.U, T.V) == FVoxelMapExploredMask::EBlockState::Some);
		if (!bPlanet)
		{
			TestTrue(TEXT("A block far away is None"), Mask.GetBlockState(0, T.U + 5000000, T.V) == FVoxelMapExploredMask::EBlockState::None);
		}
		int32 Visited = 0, VisitedWrong = 0;
		Mask.ForEach([&](int32 Face, int32 U, int32 V)
		{
			++Visited;
			VisitedWrong += Distinct.Contains(FIntVector(Face, U, V)) ? 0 : 1;
		});
		TestEqual(TEXT("ForEach visits every tile"), Visited, Distinct.Num());
		TestEqual(TEXT("... and only those"), VisitedWrong, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapExploredMaskCollapseTest,
	"VoxelWorlds.Map.ExploredMask.Collapse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapExploredMaskCollapseTest::RunTest(const FString& Parameters)
{
	using EState = FVoxelMapExploredMask::EBlockState;
	// A plane's block, tile by tile.
	{
		FVoxelMapExploredMask Mask;
		Mask.Configure(1, 0);
		for (int32 V = -64; V < 0; ++V)
		{
			for (int32 U = 128; U < 192; ++U)
			{
				if (!(U == 130 && V == -7))
				{
					Mask.Set(0, U, V);
				}
			}
		}
		TestTrue(TEXT("One tile short: partial"), Mask.GetBlockState(0, 128, -64) == EState::Some && Mask.NumFullBlocks() == 0);
		TestFalse(TEXT("... its missing tile unexplored"), Mask.IsExplored(0, 130, -7));
		TestTrue(TEXT("... its square of 2 explored"), Mask.IsAnyExplored(0, 130, -7, 1));
		const SIZE_T PartialBytes = Mask.GetAllocatedSize();
		Mask.Set(0, 130, -7);
		TestTrue(TEXT("The last tile: the block collapses"), Mask.GetBlockState(0, 128, -64) == EState::All && Mask.NumFullBlocks() == 1 && Mask.NumBlocks() == 1);
		TestEqual(TEXT("... 4,096 tiles"), Mask.Num(), static_cast<int64>(4096));
		TestTrue(TEXT("... in fewer bytes"), Mask.GetAllocatedSize() < PartialBytes);
		TestTrue(TEXT("... every tile explored"), Mask.IsExplored(0, 128, -64) && Mask.IsExplored(0, 191, -1) && Mask.IsAnyExplored(0, 150, -30, 3));
		TestFalse(TEXT("... and none beside it"), Mask.IsExplored(0, 192, -1) || Mask.IsExplored(0, 128, 0));

		// RemoveIf expands the full block, drops what it is asked to; a block emptied goes, its summaries with it.
		const int64 Removed = Mask.RemoveIf([](int32, const FIntRect&) { return true; }, [](int32, int32 U, int32 V) { return U < 160; });
		TestEqual(TEXT("RemoveIf drops half the block"), Removed, static_cast<int64>(2048));
		TestTrue(TEXT("... partial again"), Mask.GetBlockState(0, 128, -64) == EState::Some && Mask.NumFullBlocks() == 0 && Mask.Num() == 2048);
		TestTrue(TEXT("... its squares rebuilt"), !Mask.IsAnyExplored(0, 128, -64, 5) && Mask.IsAnyExplored(0, 160, -64, 5));
		Mask.RemoveIf([](int32, const FIntRect&) { return true; }, [](int32, int32, int32) { return true; });
		TestTrue(TEXT("Emptied: the block goes"), Mask.NumBlocks() == 0 && Mask.Num() == 0 && !Mask.IsAnyExplored(0, 150, -30, 12));
	}
	// A planet face: 100 tiles a side, so its last blocks hold 36 x 64, 64 x 36 and 36 x 36 tiles.
	{
		FVoxelMapExploredMask Mask;
		Mask.Configure(6, 100);
		for (int32 V = 64; V < 100; ++V)
		{
			for (int32 U = 64; U < 100; ++U)
			{
				Mask.Set(2, U, V);
			}
		}
		TestTrue(TEXT("A face's corner block is full at its 36 x 36 tiles"), Mask.GetBlockState(2, 70, 70) == EState::All && Mask.Num() == 36 * 36);
		const int64 Added = Mask.SetRect(4, FIntRect(-10, -10, 500, 500));
		TestEqual(TEXT("A face's rectangle, clipped to the face"), Added, static_cast<int64>(100 * 100));
		TestEqual(TEXT("... whole blocks (4 on the face, plus the corner block)"), Mask.NumFullBlocks(), 5);
		TestEqual(TEXT("... and again nothing new"), Mask.SetRect(4, FIntRect(0, 0, 100, 100)), static_cast<int64>(0));
		const int64 Strip = Mask.SetRect(1, FIntRect(10, 3, 90, 5));
		TestEqual(TEXT("A strip: its tiles"), Strip, static_cast<int64>(80 * 2));
		TestTrue(TEXT("... in partial blocks"), Mask.GetBlockState(1, 10, 3) == EState::Some && Mask.GetBlockState(1, 89, 4) == EState::Some);
		TestTrue(TEXT("... its ends exact"), Mask.IsExplored(1, 10, 3) && Mask.IsExplored(1, 89, 4) && !Mask.IsExplored(1, 9, 3) && !Mask.IsExplored(1, 90, 4) && !Mask.IsExplored(1, 50, 5));
		TestTrue(TEXT("The face's summary: the whole face holds some"), Mask.IsAnyExplored(1, 0, 99, 7) && !Mask.IsAnyExplored(3, 0, 0, 7));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapExploredMaskBlobsTest,
	"VoxelWorlds.Map.ExploredMask.Blobs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapExploredMaskBlobsTest::RunTest(const FString& Parameters)
{
	// A plane's mask: full and partial blocks, negative ones among them.
	{
		FVoxelMapExploredMask Mask;
		Mask.Configure(1, 0);
		Mask.SetRect(0, FIntRect(-300, -70, -100, 20));
		Mask.Set(0, 2147480000 >> 2, -2147480000 >> 2);
		for (int32 I = 0; I < 500; ++I)
		{
			Mask.Set(0, 1000 + I, 1000 + (I * 7) % 13);
		}
		const FString Blob = UVoxelMapSubsystem::EncodeExploredMask(Mask);
		FVoxelMapExploredMask Back;
		TestTrue(TEXT("The mask's blob decodes"), UVoxelMapSubsystem::DecodeExploredMask(Blob, Back));
		TestTrue(TEXT("... the same tiles"), Back.Num() == Mask.Num() && Back.NumBlocks() == Mask.NumBlocks() && Back.NumFullBlocks() == Mask.NumFullBlocks());
		int32 Wrong = 0;
		Mask.ForEach([&](int32 Face, int32 U, int32 V) { Wrong += Back.IsExplored(Face, U, V) ? 0 : 1; });
		TestEqual(TEXT("... every one"), Wrong, 0);
		TestTrue(TEXT("... the configuration too"), Back.GetFaces() == 1 && Back.GetFaceTiles() == 0);
		TestTrue(TEXT("The same mask writes the same blob"), UVoxelMapSubsystem::EncodeExploredMask(Back) == Blob);
		TestTrue(TEXT("An empty mask writes nothing"), UVoxelMapSubsystem::EncodeExploredMask(FVoxelMapExploredMask()).IsEmpty());

		// Not a mask blob: the coordinate pairs, text, a truncated blob.
		TestFalse(TEXT("A coordinate-pair blob is not a mask's"), UVoxelMapSubsystem::DecodeExploredMask(UVoxelMapSubsystem::EncodeTileCoords({ FIntPoint(1, 2) }), Back));
		TestFalse(TEXT("Text is not a mask's"), UVoxelMapSubsystem::DecodeExploredMask(TEXT("@@not-base64@@"), Back));
		AddExpectedError(TEXT("Failed to uncompress memory"), EAutomationExpectedErrorFlags::Contains, 0);
		TestFalse(TEXT("A truncated blob is refused"), UVoxelMapSubsystem::DecodeExploredMask(Blob.Left(Blob.Len() / 2 / 4 * 4), Back));
		TestEqual(TEXT("... leaving the mask empty"), Back.Num(), static_cast<int64>(0));
	}

	// A fully explored 50 km planet (MAP_PYRAMID_PLAN.md P2's target: ExploreAll under 10 ms, the save under 100 KB).
	{
		constexpr int32 N = 1637;
		FVoxelMapExploredMask Mask;
		Mask.Configure(6, N);
		const double Start = FPlatformTime::Seconds();
		int64 Added = 0;
		for (int32 Face = 0; Face < 6; ++Face)
		{
			Added += Mask.SetRect(Face, FIntRect(0, 0, N, N));
		}
		const double FillMs = 1000.0 * (FPlatformTime::Seconds() - Start);
		const double EncodeStart = FPlatformTime::Seconds();
		const FString Blob = UVoxelMapSubsystem::EncodeExploredMask(Mask);
		const double EncodeMs = 1000.0 * (FPlatformTime::Seconds() - EncodeStart);
		const double DecodeStart = FPlatformTime::Seconds();
		FVoxelMapExploredMask Back;
		const bool bDecoded = UVoxelMapSubsystem::DecodeExploredMask(Blob, Back);
		const double DecodeMs = 1000.0 * (FPlatformTime::Seconds() - DecodeStart);
		AddInfo(FString::Printf(TEXT("A 50 km planet explored: %lld tiles in %.2f ms, %d blocks (%d full), %.1f KB; the blob %d chars (encoded in %.2f ms, decoded in %.2f ms)"),
			Added, FillMs, Mask.NumBlocks(), Mask.NumFullBlocks(), Mask.GetAllocatedSize() / 1024.0, Blob.Len(), EncodeMs, DecodeMs));
		TestEqual(TEXT("Every tile: 6 N^2"), Added, static_cast<int64>(6) * N * N);
		TestEqual(TEXT("... 4,056 full blocks"), Mask.NumFullBlocks(), 6 * 26 * 26);
		TestEqual(TEXT("... no partial one"), Mask.NumBlocks(), Mask.NumFullBlocks());
		TestTrue(TEXT("... in milliseconds"), FillMs < 50.0);
		TestTrue(TEXT("The blob under 100 KB"), Blob.Len() < 100 * 1024);
		TestTrue(TEXT("... restores it"), bDecoded && Back.Num() == Mask.Num() && Back.NumFullBlocks() == Mask.NumFullBlocks());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
