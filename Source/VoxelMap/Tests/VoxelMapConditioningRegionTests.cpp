// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "VoxelMapSubsystem.h"
#include "VoxelMapTileFrame.h"
#include "VoxelMapTypes.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelTerrainConditioning.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A change of terrain conditioning makes stale only the map tiles it reaches (Documentation/TERRAIN_LINES_PLAN.md, R1;
// UVoxelChunkManager::OnTerrainConditioningChangedIn). A conditioner's lines arrive one by one (a road worked out after
// the map asked for it), and each used to make every tile image stale.
//
// - The frame: a tile meets a changed region when the region meets what the tile gathers its conditioning over, its
//   area and apron, grown above level 0 by the least half width its level takes lines with; everywhere reaches every
//   tile; a cap reaches no tile of a plane, and an invalid box none.
// - The subsystem: with the tiles around a site drawn, a notification far from them makes none stale, one inside a
//   tile makes that tile stale, and a conditioner set or cleared makes all of them stale (the map's own counter).
// ---------------------------------------------------------------------------

namespace VoxelMapConditioningRegionTest
{
	/** Pump the map's tile tasks (and its timers) until nothing waits or draws. */
	bool DrainMap(UVoxelMapSubsystem* Map, UWorld* World, double TimeoutSeconds = 60.0)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		int32 IdleRounds = 0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			++GFrameCounter;
			World->GetTimerManager().Tick(0.1f);
			IdleRounds = Map->GetDebugStats().Contains(TEXT("waiting=0 drawing=0")) ? IdleRounds + 1 : 0;
			if (IdleRounds >= 12)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return false;
	}

	/** The map's count of tile images a conditioning change made stale (voxel.Map.Stats' "conditioning-stale="). */
	int64 ConditioningStale(const UVoxelMapSubsystem* Map)
	{
		const FString Stats = Map->GetDebugStats();
		const FString Key(TEXT("conditioning-stale="));
		const int32 At = Stats.Find(Key);
		return At == INDEX_NONE ? -1 : FCString::Atoi64(*Stats.Mid(At + Key.Len()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapConditioningRegionTest,
	"VoxelWorlds.Map.ConditioningRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapConditioningRegionTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapConditioningRegionTest;
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("VoxelMapConditioningRegion"));
	W.ConfigureSmallPlane();
	W.StartManager();
	UVoxelMapSubsystem* Map = W.World->GetSubsystem<UVoxelMapSubsystem>();
	W.SetViewer(W.ViewerOverGround(0.0, 0.0, 300.0f));
	if (!TestNotNull(TEXT("The game world has a map"), Map) || !TestTrue(TEXT("Streaming settles"), W.Settle()))
	{
		W.Stop();
		return true;
	}
	const FVector Site(800.0, 800.0, W.Manager->GetGeneratedSurfacePoint(FVector(800.0, 800.0, 0.0)).Z);
	// Exploring draws nothing: a view asks for the images (the world map, zoomed in on the tiles around the site).
	Map->RequestTilesInRadius(Site, 2500.0f);
	const FVoxelMapTileFrame Frame = Map->GetTileFrame();
	{
		TArray<FIntPoint> Around;
		Frame.TilesWithinRadius(Site, 2500.0, Around);
		TArray<FVoxelMapTileId> View;
		for (const FIntPoint& Coord : Around)
		{
			View.Add(FVoxelMapTileId(Coord, 0));
		}
		Map->RequestView(EVoxelMapRequester::WorldMap, View);
	}
	TestTrue(TEXT("The tiles around the site draw"), DrainMap(Map, W.World));
	const double Tile = Frame.GetTileWorldSize();
	const double Voxel = Frame.VoxelSize;
	TestEqual(TEXT("A tile is a chunk across"), Tile, static_cast<double>(W.Config->ChunkSize * W.Config->VoxelSize));

	// ---- The frame.
	const FVoxelMapTileId Here(FIntPoint(0, 0), 0);
	const FVoxelMapTileId East(FIntPoint(1, 0), 0);
	const FVoxelMapTileId West(FIntPoint(-1, 0), 0);
	const FVoxelMapTileId North(FIntPoint(0, 1), 0);
	const FVoxelConditioningRegion Inside = FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(0.4 * Tile), FVector2D(0.6 * Tile)));
	TestTrue(TEXT("A region inside a tile reaches that tile"), Frame.MeetsConditioningRegion(Here, Inside));
	TestFalse(TEXT("... and none of its neighbours"), Frame.MeetsConditioningRegion(East, Inside) || Frame.MeetsConditioningRegion(West, Inside) || Frame.MeetsConditioningRegion(North, Inside));
	// A tile gathers over its one-voxel apron: a region half a voxel past its edge still reaches it, one two voxels past does not.
	TestTrue(TEXT("A region on a tile's apron reaches it"),
		Frame.MeetsConditioningRegion(Here, FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(Tile + 0.5 * Voxel, 0.5 * Tile), FVector2D(Tile + 3.0 * Voxel, 0.6 * Tile)))));
	TestFalse(TEXT("A region past a tile's apron does not"),
		Frame.MeetsConditioningRegion(Here, FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(Tile + 2.0 * Voxel, 0.5 * Tile), FVector2D(Tile + 5.0 * Voxel, 0.6 * Tile)))));
	TestTrue(TEXT("Everywhere reaches every tile"), Frame.MeetsConditioningRegion(Here, FVoxelConditioningRegion::Everywhere())
		&& Frame.MeetsConditioningRegion(FVoxelMapTileId(FIntPoint(-40, 17), 5), FVoxelConditioningRegion::Everywhere()));
	TestFalse(TEXT("A cap reaches no tile of a plane"), Frame.MeetsConditioningRegion(Here, FVoxelConditioningRegion::OfCap(FVector::UpVector, 3.0)));
	TestFalse(TEXT("An invalid box reaches none"), Frame.MeetsConditioningRegion(Here, FVoxelConditioningRegion::OfBox(FBox2D(ForceInit))));
	// A level-3 tile is eight chunks across and takes lines as at least 0.71 of its pixel wide (eight voxels): a region
	// that far past its apron still reaches it, as its gather does.
	{
		const FVoxelMapTileId Coarse(FIntPoint(0, 0), 3);
		double Least = 0.0;
		const FVoxelConditioningRegion Gathered = Frame.GetConditioningRegion(Coarse, Least);
		TestEqual(TEXT("A level-3 tile's least half width is 0.71 of eight voxels"), Least, FVoxelConditioningLine::CoarseHalfWidthPerSpacing * 8.0 * Voxel, 1.0e-6);
		TestTrue(TEXT("... and its region is its eight chunks, its pixel's apron and that"), !Gathered.bCap
			&& FMath::IsNearlyEqual(Gathered.Box.Min.X, -8.0 * Voxel - Least, 1.0e-6) && FMath::IsNearlyEqual(Gathered.Box.Max.X, 8.0 * Tile + 8.0 * Voxel + Least, 1.0e-6));
		const double Edge = 8.0 * Tile + 8.0 * Voxel;
		TestTrue(TEXT("A region within the least half width of a coarse tile's apron reaches it"),
			Frame.MeetsConditioningRegion(Coarse, FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(Edge + 0.5 * Least, Tile), FVector2D(Edge + 4.0 * Least, 2.0 * Tile)))));
		TestFalse(TEXT("One beyond it does not"),
			Frame.MeetsConditioningRegion(Coarse, FVoxelConditioningRegion::OfBox(FBox2D(FVector2D(Edge + 1.5 * Least, Tile), FVector2D(Edge + 4.0 * Least, 2.0 * Tile)))));
		double Level0Least = 1.0;
		Frame.GetConditioningRegion(Here, Level0Least);
		TestEqual(TEXT("A level-0 tile takes lines as they are"), Level0Least, 0.0);
	}

	// ---- The subsystem: the tiles drawn around the site.
	TArray<FIntPoint> Tiles;
	Frame.TilesWithinRadius(Site, 2500.0, Tiles);
	int32 Drawn = 0;
	for (const FIntPoint& Coord : Tiles)
	{
		Drawn += Map->GetTile(Coord) ? 1 : 0;
	}
	TestTrue(TEXT("Several tiles are drawn, the site's among them (non-vacuous)"), Drawn >= 4 && Map->GetTile(FIntPoint(0, 0)) != nullptr);
	const int64 Before = ConditioningStale(Map);
	TestEqual(TEXT("No tile is stale from a conditioning change yet"), Before, static_cast<int64>(0));

	// Far from every tile: a road worked out late somewhere else.
	W.Manager->NotifyTerrainConditioningChanged(FBox2D(FVector2D(500000.0, -300000.0), FVector2D(540000.0, -200000.0)));
	TestEqual(TEXT("A change far from the tiles makes none stale"), ConditioningStale(Map), Before);

	// Inside one tile.
	W.Manager->NotifyTerrainConditioningChanged(Inside.Box);
	TestEqual(TEXT("A change inside a tile makes that tile stale, and no other"), ConditioningStale(Map), Before + 1);
	TestTrue(TEXT("The stale tile draws again"), DrainMap(Map, W.World));

	// A conditioner set or cleared: anywhere.
	const FString StatsBefore = Map->GetDebugStats();
	W.Manager->SetTerrainConditioner(nullptr);
	const int64 AfterAll = ConditioningStale(Map);
	AddInfo(FString::Printf(TEXT("%d tiles within the radius, %d drawn; stale after a change inside one tile: %lld; after a change everywhere: %lld more (%s)"),
		Tiles.Num(), Drawn, Before + 1, AfterAll - Before - 1, *StatsBefore.Left(60)));
	TestTrue(TEXT("A change everywhere makes every drawn tile stale"), AfterAll - Before - 1 >= Drawn);
	TestTrue(TEXT("The tiles draw again"), DrainMap(Map, W.World));
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
