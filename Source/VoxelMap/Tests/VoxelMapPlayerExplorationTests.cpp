// Copyright Daniel Raquel. All Rights Reserved.

// Each player's map shows the ground that player has been to (G270, Documentation/FINDINGS.md). A listen server streams
// chunks around its remote players as well as its own (the further viewers, G217), and the map marked every chunk that
// generated explored, so the host's map showed the other players' ground too.
//
// - VoxelWorlds.Map.Exploration.OwnPlayerOnly: a streaming viewer at A and a further viewer (another player) ten view
//   distances away at B, both streamed. A's tiles are explored; B's chunks are loaded and its tiles are not. With the
//   player itself at B (its minimap's request), B's tiles are explored.
// - VoxelWorlds.Map.Exploration.ResolvesUnasked (G272): a map nothing asks for tiles (a dedicated server's: no minimap)
//   explores the chunks that generate and records an edit's overlay, so a save there holds the edits' overlays. Before
//   G272 such a map never resolved its world: nothing explored, no overlay.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "TimerManager.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapResolvesUnaskedTest,
	"VoxelWorlds.Map.Exploration.ResolvesUnasked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapResolvesUnaskedTest::RunTest(const FString& Parameters)
{
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("MapResolvesUnasked"));
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

	// No minimap, no request, no import: the map is never called.
	// The middle of a tile (a tile is a chunk's column), so the dig's overlay is on that tile alone.
	const double HalfTile = 0.5 * W.Config->ChunkSize * W.Config->VoxelSize;
	const FVector Ground = W.ViewerOverGround(HalfTile, HalfTile, 0.0f);
	W.SetViewer(Ground + FVector(0.0, 0.0, 200.0));
	TestTrue(TEXT("The world settles"), W.Settle());
	TestTrue(TEXT("The chunks that generated are explored"), Map->GetNumExploredTiles() > 0);
	TestTrue(TEXT("... the viewer's tile among them"), Map->IsTileExplored(Map->WorldToTileCoord(Ground)));

	// A dig at the viewer's feet: its overlay is recorded (the rebuild runs on the map's timer and the task graph).
	TestEqual(TEXT("No overlay before the dig"), Map->GetEditOverlayCount(), 0);
	FVoxelBrushParams Brush;
	Brush.Shape = EVoxelBrushShape::Sphere;
	Brush.Radius = 300.0f;
	Brush.Strength = 1.0f;
	Brush.FalloffType = EVoxelBrushFalloff::Linear;
	Brush.DensityDelta = 255;
	W.Manager->GetEditManager()->ApplyBrushEdit(Ground, Brush, EEditMode::Subtract);
	const double Deadline = FPlatformTime::Seconds() + 30.0;
	const FIntPoint DugTile = Map->WorldToTileCoord(Ground);
	while (!Map->GetEditOverlay(DugTile) && FPlatformTime::Seconds() < Deadline)
	{
		W.Tick();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		++GFrameCounter; // the timer manager ticks once a frame
		W.World->GetTimerManager().Tick(0.1f);
		FPlatformProcess::Sleep(0.005f);
	}
	TestTrue(TEXT("The dig's overlay is recorded"), Map->GetEditOverlay(DugTile) != nullptr);
	int32 Count = 0;
	TestTrue(TEXT("... and a save would hold it"), !Map->ExportEditOverlays(Count).IsEmpty() && Count >= 1);
	AddInfo(FString::Printf(TEXT("Unasked map: %lld tile(s) explored, %d overlay(s) after the dig; %s"), Map->GetNumExploredTiles(), Map->GetEditOverlayCount(), *Map->GetDebugStats()));

	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
