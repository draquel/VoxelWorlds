// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "VoxelMapTileStore.h"
#include "VoxelMapSubsystem.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Map tile images under a fixed budget (WORLD_MODES_REVIEW_PLAN.md §14.2 WM-E2, finding KI15), every level of the tile
// pyramid in one least-recently-used store (MAP_PYRAMID_PLAN.md P1).
//
// - Budget: the store never exceeds its budget, drops the least recently used images first at every level alike, keeps a
//   tile read all along, keeps a stale image readable until its redraw is stored; unlimited keeps everything.
// - ViewRequests (an island, through the subsystem): exploring the whole island queues nothing; a view's request draws
//   its tiles at its level, under the budget; a new view replaces the old (the queue never past a view's tiles); the
//   coarser level stored under a view is what a sampler falls back on.
// ---------------------------------------------------------------------------

namespace VoxelMapTileStoreTest
{
	/** A Resolution^2 image whose pixel colour encodes the tile and the pixel. */
	TArray<FColor> MakeImage(int32 Resolution, int32 Seed)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Resolution * Resolution);
		for (int32 Y = 0; Y < Resolution; ++Y)
		{
			for (int32 X = 0; X < Resolution; ++X)
			{
				Pixels[Y * Resolution + X] = FColor(uint8(X * 4), uint8(Y * 4), uint8(Seed & 0xFF), 255);
			}
		}
		return Pixels;
	}

	/** Tile i at a level cycling 0..3 (the store keeps every level together). */
	FVoxelMapTileId Id(int32 I)
	{
		return FVoxelMapTileId(FIntPoint(I, -I), I % 4);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapTileStoreBudgetTest,
	"VoxelWorlds.Map.TileStore.Budget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapTileStoreBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapTileStoreTest;
	constexpr int32 Res = 64;
	constexpr int64 TileBytes = Res * Res * 4; // 16 KB

	FVoxelMapTileStore Store;
	Store.SetBudget(64 * TileBytes);
	const FVoxelMapTileId Kept = Id(5);
	for (int32 I = 0; I < 400; ++I)
	{
		Store.Store(Id(I), MakeImage(Res, I), Res);
		TestTrue(TEXT("The images never exceed the budget"), Store.GetBytes() <= 64 * TileBytes);
		// One tile is read all along (a view showing it at every fill).
		if (I > 5)
		{
			Store.Find(Kept);
		}
	}
	AddInfo(FString::Printf(TEXT("400 tiles over four levels into 64: %d kept, %lld evicted, by level %d/%d/%d/%d"), Store.Num(),
		Store.GetEvictions(), Store.NumAtLevel(0), Store.NumAtLevel(1), Store.NumAtLevel(2), Store.NumAtLevel(3)));
	TestTrue(TEXT("The newest tiles are kept"), Store.Contains(Id(399)) && Store.Contains(Id(380)));
	TestFalse(TEXT("The oldest are gone"), Store.Contains(Id(100)));
	TestTrue(TEXT("A tile read all along is kept"), Store.Contains(Kept));
	TestTrue(TEXT("Every level is kept alike"), Store.NumAtLevel(0) > 0 && Store.NumAtLevel(3) > 0);
	TestTrue(TEXT("Trimmed to 90% of the budget at most once in a while"), Store.Num() >= 57 && Store.Num() <= 64);
	if (const FVoxelMapTile* Tile = Store.Find(Id(399)))
	{
		TestTrue(TEXT("A stored tile keeps its id, level and pixels"), Tile->GetId() == Id(399) && Tile->Resolution == Res
			&& Tile->PixelData.Num() == Res * Res && Tile->PixelData[Res + 2] == FColor(8, 4, uint8(399 & 0xFF), 255));
	}

	// A stale image stays readable until its redraw is stored.
	TestTrue(TEXT("Marking a stored tile stale"), Store.MarkStale(Id(399)));
	TestTrue(TEXT("... it is stale"), Store.IsStale(Id(399)));
	TestNotNull(TEXT("... and still readable"), Store.Find(Id(399)));
	Store.Store(Id(399), MakeImage(Res, 7), Res);
	TestFalse(TEXT("A redraw clears the mark"), Store.IsStale(Id(399)));
	TestFalse(TEXT("A tile without an image cannot be stale"), Store.MarkStale(Id(100)));

	// The same coordinate at two levels is two tiles.
	FVoxelMapTileStore Levels;
	Levels.Store(FVoxelMapTileId(FIntPoint(3, 4), 0), MakeImage(Res, 1), Res);
	Levels.Store(FVoxelMapTileId(FIntPoint(3, 4), 2), MakeImage(Res, 2), Res);
	TestEqual(TEXT("A coordinate at two levels is two tiles"), Levels.Num(), 2);
	TestEqual(TEXT("... each with its own image"), static_cast<int32>(Levels.Find(FVoxelMapTileId(FIntPoint(3, 4), 2))->PixelData[0].B), 2);

	// A budget of 0 keeps everything.
	FVoxelMapTileStore Unlimited;
	Unlimited.SetBudget(0);
	for (int32 I = 0; I < 2000; ++I)
	{
		Unlimited.Store(Id(I), MakeImage(Res, I), Res);
	}
	TestEqual(TEXT("Unlimited: every image kept"), Unlimited.Num(), 2000);
	TestEqual(TEXT("Unlimited: 16 KB each"), Unlimited.GetBytes(), 2000 * TileBytes);
	return true;
}

namespace VoxelMapTileStoreTest
{
	/** Pump the game thread's tasks (tile completions) until the map's queue is empty, or 60 s. */
	bool DrainMap(UVoxelMapSubsystem* Map)
	{
		const double Deadline = FPlatformTime::Seconds() + 60.0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			const FString Stats = Map->GetDebugStats();
			if (Stats.Contains(TEXT("waiting=0 drawing=0")))
			{
				return true;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		return false;
	}

	/** The tiles at a level within Radius tiles (at that level) of a level-0 tile, nearest first, as a view asks for them. */
	TArray<FVoxelMapTileId> ViewAround(const FVoxelMapTileFrame& Frame, const FIntPoint& Centre0, int32 Level, int32 Radius)
	{
		const FVoxelMapTileId Centre = Frame.LevelTileOf(Centre0, Level);
		TArray<FVoxelMapTileId> View;
		for (int32 Ring = 0; Ring <= Radius; ++Ring)
		{
			for (int32 Y = -Ring; Y <= Ring; ++Y)
			{
				for (int32 X = -Ring; X <= Ring; ++X)
				{
					if (FMath::Max(FMath::Abs(X), FMath::Abs(Y)) == Ring)
					{
						View.Add(FVoxelMapTileId(Centre.Coord + FIntPoint(X, Y), Level));
					}
				}
			}
		}
		return View;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapViewRequestsTest,
	"VoxelWorlds.Map.Pyramid.ViewRequests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapViewRequestsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapTileStoreTest;
	IConsoleVariable* BudgetVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Map.TileBudgetMB"));
	if (!TestNotNull(TEXT("The budget cvar"), BudgetVar))
	{
		return true;
	}
	const float OldBudget = BudgetVar->GetFloat();

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelMapViewRequestsWorld"));
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);

	// A small island (R + F = 20,000 uu over 1,600 uu tiles of 16 x 16 pixels: about 500 tiles).
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::IslandBowl;
	Config->MeshingMode = EMeshingMode::Cubic;
	Config->ChunkSize = 16;
	Config->VoxelSize = 100.0f;
	Config->ViewDistance = 3200.0f;
	Config->IslandRadius = 15000.0f;
	Config->IslandFalloffWidth = 5000.0f;
	Config->bUseGPUGeneration = false;
	Config->bGenerateCollision = false;
	Config->bEnableScatter = false;
	Config->bWorldBoundary = false;
	Config->bIslandFarOcean = false;

	AActor* Owner = World->SpawnActor<AActor>();
	UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>(Owner);
	Manager->RegisterComponent();
	VoxelTesting::FRecordingMeshRenderer Renderer;
	Renderer.bKeepMeshes = false;
	Renderer.Initialize(World, Config);
	Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);

	UVoxelMapSubsystem* Map = World->GetSubsystem<UVoxelMapSubsystem>();
	if (TestNotNull(TEXT("The game world has a map"), Map))
	{
		// A tiny budget so the views overrun it: 16 x 16 tiles are 1 KB. Set at console priority: a value typed in the
		// editor's console would override a lower-priority set.
		BudgetVar->Set(TEXT("0.06"), ECVF_SetByConsole); // ~61 images
		Map->ApplyTileBudgets();

		// Exploring the whole island draws nothing: the views ask for what they show.
		const int32 Explored = static_cast<int32>(Map->ExploreAllTiles(FVector::ZeroVector, 0.0f));
		const FVoxelMapTileStore& Store = Map->GetTileStore();
		const FVoxelMapTileQueue& Queue = Map->GetTileQueue();
		AddInfo(FString::Printf(TEXT("%d tiles explored; %s"), Explored, *Map->GetDebugStats()));
		TestTrue(TEXT("The island has hundreds of tiles"), Explored > 300);
		TestEqual(TEXT("Exploration is kept whole"), Map->GetExploredTiles().Num(), Explored);
		TestEqual(TEXT("ExploreAll queues no image"), Queue.NumWaiting() + Queue.NumDrawing(), 0);
		TestEqual(TEXT("... and draws none"), Store.Num(), 0);

		// The level matching a texel: 16 x 16 tiles of 100 uu voxels; a 380 uu texel reads level 1 (200 uu pixels), a
		// 400 uu one level 2.
		const FVoxelMapTileFrame& Frame = Map->GetTileFrame();
		TestEqual(TEXT("Texels finer than voxels read level 0"), Map->LevelForTexel(50.0), 0);
		TestEqual(TEXT("A 380 uu texel reads level 1"), Map->LevelForTexel(380.0), 1);
		TestEqual(TEXT("A 400 uu texel reads level 2"), Map->LevelForTexel(400.0), 2);

		// A view at level 2 around the centre (5 x 5 tiles): drawn, at level 2, within the budget.
		const FIntPoint Centre0 = Map->WorldToTileCoord(FVector::ZeroVector);
		const TArray<FVoxelMapTileId> First = ViewAround(Frame, Centre0, 2, 2);
		const int32 Queued = Map->RequestView(EVoxelMapRequester::WorldMap, First);
		TestEqual(TEXT("The view's tiles are queued"), Queued, First.Num());
		TestTrue(TEXT("The queue holds the view's tiles, no more"), Queue.NumWaiting() + Queue.NumDrawing() <= First.Num());
		TestTrue(TEXT("The view's tiles drew"), DrainMap(Map));
		const FVoxelMapTile* CentreTile = Map->FindTile(First[0]);
		TestTrue(TEXT("The centre tile is stored at its level"), CentreTile && CentreTile->Level == 2 && CentreTile->Resolution == Config->ChunkSize);
		TestEqual(TEXT("Every image is the view's level"), Store.NumAtLevel(2), Store.Num());
		TestEqual(TEXT("A view of drawn tiles queues nothing"), Map->RequestView(EVoxelMapRequester::WorldMap, First), 0);

		// Zoomed in: a view at level 0 replaces it; panned at once, the first zoom's waiting tiles are dropped.
		const TArray<FVoxelMapTileId> Zoomed = ViewAround(Frame, Centre0, 0, 3);
		const TArray<FVoxelMapTileId> Panned = ViewAround(Frame, Centre0 + FIntPoint(4, 0), 0, 3);
		Map->RequestView(EVoxelMapRequester::WorldMap, Zoomed);
		Map->RequestView(EVoxelMapRequester::WorldMap, Panned);
		int32 ZoomedOnlyWaiting = 0;
		for (const FVoxelMapTileId& Tile : Zoomed)
		{
			ZoomedOnlyWaiting += (!Panned.Contains(Tile) && Queue.IsWaiting(Tile)) ? 1 : 0;
		}
		TestEqual(TEXT("A replaced view's tiles stop waiting"), ZoomedOnlyWaiting, 0);
		TestTrue(TEXT("The queue never past the views' tiles"), Queue.NumWaiting() + Queue.NumDrawing() <= Panned.Num() + 4);
		TestTrue(TEXT("The panned view drew"), DrainMap(Map));
		TestNotNull(TEXT("The panned view's centre is drawn at level 0"), Map->FindTile(Panned[0]));
		TestTrue(TEXT("The level-0 tile is the island's: the map's GetTile reads it"), Map->GetTile(Panned[0].Coord) != nullptr);

		// The minimap's view first, then the world map closing: nothing waits.
		Map->RequestView(EVoxelMapRequester::WorldMap, ViewAround(Frame, Centre0 + FIntPoint(-6, 3), 1, 3));
		Map->RequestView(EVoxelMapRequester::Minimap, ViewAround(Frame, Centre0, 1, 1));
		Map->RequestView(EVoxelMapRequester::WorldMap, {});
		TestTrue(TEXT("Closing the world map leaves at most the minimap's tiles"), Queue.NumWaiting() <= 9);
		TestTrue(TEXT("The minimap's view drew"), DrainMap(Map));
		AddInfo(FString::Printf(TEXT("After the views: %s"), *Map->GetDebugStats()));
		TestTrue(TEXT("The images stay within the budget"), Store.GetBytes() <= int64(0.06 * 1024 * 1024));
		TestTrue(TEXT("... which binds (the views drew more than it holds)"), Store.GetEvictions() > 0);
		TestNotNull(TEXT("The minimap's centre is kept (the most recent)"), Map->FindTile(Frame.LevelTileOf(Centre0, 1)));
	}

	BudgetVar->Set(*FString::SanitizeFloat(OldBudget), ECVF_SetByConsole);
	Manager->Shutdown();
	Owner->Destroy();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	Config->RemoveFromRoot();
	return true;
}

/**
 * World modes review WM-MP9 (§23): voxel.Map.ExploreAllMaxTiles caps a heightmap world's ExploreAll as it caps a
 * planet's (spherical Phase 6, WM-BP11): the tiles nearest the centre, in square rings, up to the cap. On a plane (whose
 * tiles are unbounded) ExploreAll within 8,000 uu of a point over 1,600 uu tiles covers an 11 x 11 square: with the cap
 * at 30 it explores 30 tiles, the centre tile, its first and second rings and five of the third, none farther; with the
 * cap past the count it explores all 121, the same set the raster walk explored before.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapExploreAllCapPlaneTest,
	"VoxelWorlds.Map.ExploreAllCap.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapExploreAllCapPlaneTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapTileStoreTest;
	IConsoleVariable* CapVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Map.ExploreAllMaxTiles"));
	if (!TestNotNull(TEXT("The cap cvar"), CapVar))
	{
		return true;
	}
	const FString OldCap = CapVar->GetString();

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelMapExploreAllCapWorld"));
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->MeshingMode = EMeshingMode::Cubic;
	Config->ChunkSize = 16;
	Config->VoxelSize = 100.0f;
	Config->ViewDistance = 3200.0f;
	Config->bUseGPUGeneration = false;
	Config->bGenerateCollision = false;
	Config->bEnableScatter = false;

	AActor* Owner = World->SpawnActor<AActor>();
	UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>(Owner);
	Manager->RegisterComponent();
	VoxelTesting::FRecordingMeshRenderer Renderer;
	Renderer.bKeepMeshes = false;
	Renderer.Initialize(World, Config);
	Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);

	UVoxelMapSubsystem* Map = World->GetSubsystem<UVoxelMapSubsystem>();
	if (TestNotNull(TEXT("The game world has a map"), Map))
	{
		const FVector Center(12345.0, -6789.0, 0.0);
		auto Unpack = [](uint64 PackedKey) { return FIntPoint(static_cast<int32>(static_cast<uint32>(PackedKey >> 32)), static_cast<int32>(static_cast<uint32>(PackedKey & 0xFFFFFFFF))); };

		CapVar->Set(TEXT("30"), ECVF_SetByCode);
		const int64 Capped = Map->ExploreAllTiles(Center, 8000.0f);
		TestTrue(TEXT("Nothing is queued"), DrainMap(Map));
		// The centre tile once the map has resolved its world (its tile grid is the chunk manager's; before the first call
		// the coordinate is not the grid's).
		const FIntPoint CenterTile = Map->WorldToTileCoord(Center);
		auto RingOf = [&CenterTile](const FIntPoint& Tile) { return FMath::Max(FMath::Abs(Tile.X - CenterTile.X), FMath::Abs(Tile.Y - CenterTile.Y)); };
		// 30 tiles are the centre, its first ring (8), its second (16) and five of the third.
		int32 Ring0 = 0, Ring1 = 0, Ring2 = 0, PastRing3 = 0;
		for (const uint64 PackedKey : Map->GetExploredTiles())
		{
			const int32 Ring = RingOf(Unpack(PackedKey));
			Ring0 += Ring == 0 ? 1 : 0;
			Ring1 += Ring == 1 ? 1 : 0;
			Ring2 += Ring == 2 ? 1 : 0;
			PastRing3 += Ring > 3 ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("Cap 30: %lld tiles explored (%d kept), the centre tile %d, its first ring %d, its second %d, past the third ring %d"), Capped, Map->GetExploredTiles().Num(), Ring0, Ring1, Ring2, PastRing3));
		TestEqual(TEXT("With the cap at 30, 30 tiles are explored"), Capped, int64(30));
		TestEqual(TEXT("... the centre tile among them"), Ring0, 1);
		TestEqual(TEXT("... its whole first ring"), Ring1, 8);
		TestEqual(TEXT("... its whole second ring"), Ring2, 16);
		TestEqual(TEXT("... none past the third ring (the nearest first)"), PastRing3, 0);

		CapVar->Set(TEXT("1000000"), ECVF_SetByCode);
		const int64 All = Map->ExploreAllTiles(Center, 8000.0f);
		TestTrue(TEXT("Nothing is queued"), DrainMap(Map));
		int32 MaxRing = 0;
		for (const uint64 PackedKey : Map->GetExploredTiles())
		{
			MaxRing = FMath::Max(MaxRing, RingOf(Unpack(PackedKey)));
		}
		AddInfo(FString::Printf(TEXT("Cap lifted: %lld tiles explored, %d kept, out to ring %d"), All, Map->GetExploredTiles().Num(), MaxRing));
		TestEqual(TEXT("With the cap lifted the 11 x 11 square is explored"), All, int64(121));
		TestEqual(TEXT("... and kept"), Map->GetExploredTiles().Num(), 121);
		TestEqual(TEXT("... out to the fifth ring"), MaxRing, 5);
	}

	CapVar->Set(*OldCap, ECVF_SetByCode);
	Manager->Shutdown();
	Owner->Destroy();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
