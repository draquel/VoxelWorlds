// Copyright Daniel Raquel. All Rights Reserved.

// Each player's map shows the ground that player has been to (G270, Documentation/FINDINGS.md). A listen server streams
// chunks around its remote players as well as its own (the further viewers, G217), and the map marked every chunk that
// generated explored, so the host's map showed the other players' ground too.
//
// - VoxelWorlds.Map.Exploration.OwnPlayerOnly: a streaming viewer at A and a further viewer (another player) ten view
//   distances away at B, both streamed. A's tiles are explored; B's chunks are loaded and its tiles are not. With the
//   player itself at B (its minimap's request), B's tiles are explored.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelChunkManager.h"
#include "VoxelMapSubsystem.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelTestChunkWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapOwnPlayerExplorationTest,
	"VoxelWorlds.Map.Exploration.OwnPlayerOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapOwnPlayerExplorationTest::RunTest(const FString& Parameters)
{
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("MapOwnPlayerExploration"));
	W.ConfigureSmallPlane();
	W.ConfigureThreeLODBands();
	W.Config->bGenerateCollision = true;
	W.StartManager();
	UVoxelMapSubsystem* Map = W.World->GetSubsystem<UVoxelMapSubsystem>();
	if (!TestNotNull(TEXT("The game world has a map"), Map))
	{
		W.Stop();
		return false;
	}

	const FVector A = W.ViewerOverGround(0.0, 0.0, 200.0f);
	const FVector B = W.ViewerOverGround(10.0 * W.Config->ViewDistance, 0.0, 200.0f);
	Map->RequestTilesInRadius(A, 1.0f); // resolves the world: the tiles are the configuration's chunks
	const FIntPoint TileA = Map->WorldToTileCoord(A);
	const FIntPoint TileB = Map->WorldToTileCoord(B);
	TestFalse(TEXT("Nothing at B is explored yet"), Map->IsTileExplored(TileB));

	// This world's player at A, another player at B.
	W.SetViewer(A);
	W.SetFurtherViewers({ B });
	TestTrue(TEXT("The world settles with two viewers"), W.Settle());
	TestTrue(TEXT("The other player's ground is loaded"), W.Manager->IsChunkLoaded(W.Manager->WorldToChunkCoord(B - FVector(0.0, 0.0, 250.0))));
	TestTrue(TEXT("This player's ground is explored"), Map->IsTileExplored(TileA));
	TestFalse(TEXT("The other player's ground is not"), Map->IsTileExplored(TileB));
	const int64 ExploredWithTwo = Map->GetNumExploredTiles();

	// This player comes to B: its minimap's request around it explores it (UVCMinimapWidget, every refresh).
	Map->RequestTilesInRadius(B, 1.0f);
	TestTrue(TEXT("B is explored once this player is there"), Map->IsTileExplored(TileB));
	AddInfo(FString::Printf(TEXT("Explored tiles: %lld with the other player at B, %lld once this player is there"),
		ExploredWithTwo, Map->GetNumExploredTiles()));

	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
