// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMapSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// QA refinement R25: map exploration persists with the world save. The explored tiles travel as a base64 blob of
// zlib-compressed int32 (X, Y) pairs (a string keeps them JSON-safe; packed 64-bit keys would lose precision as JSON
// numbers). Pure tests (no world, no RHI) -- run headless.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapExplorationRoundTripTest,
	"VoxelWorlds.Map.Exploration.EncodeDecodeRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapExplorationRoundTripTest::RunTest(const FString& Parameters)
{
	// Negative and far coordinates (the QA player was at tile -128 on X) plus a block of neighbours.
	TArray<FIntPoint> Tiles = { FIntPoint(-128, -13), FIntPoint(0, 0), FIntPoint(2147480000, -2147480000), FIntPoint(-1, 1) };
	for (int32 Y = 100; Y < 140; ++Y)
	{
		for (int32 X = -40; X < 0; ++X)
		{
			Tiles.Add(FIntPoint(X, Y));
		}
	}

	const FString Encoded = UVoxelMapSubsystem::EncodeTileCoords(Tiles);
	TestFalse(TEXT("encodes to a non-empty blob"), Encoded.IsEmpty());
	TestTrue(TEXT("a block of neighbours compresses below 8 bytes per tile"), Encoded.Len() < Tiles.Num() * 8);

	TArray<FIntPoint> Decoded;
	TestTrue(TEXT("decodes"), UVoxelMapSubsystem::DecodeTileCoords(Encoded, Tiles.Num(), Decoded));
	TestEqual(TEXT("same tile count"), Decoded.Num(), Tiles.Num());
	bool bAllMatch = Decoded.Num() == Tiles.Num();
	for (int32 i = 0; bAllMatch && i < Tiles.Num(); ++i)
	{
		bAllMatch = Decoded[i] == Tiles[i];
	}
	TestTrue(TEXT("every coordinate survives, negatives and extremes included"), bAllMatch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapExplorationEdgeCasesTest,
	"VoxelWorlds.Map.Exploration.EmptyAndMalformed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapExplorationEdgeCasesTest::RunTest(const FString& Parameters)
{
	TArray<FIntPoint> Out;
	TestTrue(TEXT("nothing explored encodes to an empty string"), UVoxelMapSubsystem::EncodeTileCoords({}).IsEmpty());
	TestTrue(TEXT("an empty blob with count 0 decodes to nothing"), UVoxelMapSubsystem::DecodeTileCoords(FString(), 0, Out) && Out.Num() == 0);
	TestFalse(TEXT("an empty blob claiming tiles is malformed"), UVoxelMapSubsystem::DecodeTileCoords(FString(), 5, Out));
	TestFalse(TEXT("text that is not base64 is malformed"), UVoxelMapSubsystem::DecodeTileCoords(TEXT("@@not-base64@@"), 3, Out));

	const FString Encoded = UVoxelMapSubsystem::EncodeTileCoords({ FIntPoint(1, 2), FIntPoint(3, 4) });
	// The engine reports a size mismatch as a possibly corrupt asset (an Error log): expected here.
	AddExpectedError(TEXT("Failed to uncompress memory"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("a count larger than the data is malformed"), UVoxelMapSubsystem::DecodeTileCoords(Encoded, 50, Out));
	TestEqual(TEXT("a failed decode leaves no tiles"), Out.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
