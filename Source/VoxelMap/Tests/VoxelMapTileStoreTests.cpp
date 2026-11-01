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
// Map tile images under a fixed budget (WORLD_MODES_REVIEW_PLAN.md §14.2 WM-E2, finding KI15).
//
// The store keeps an overview (a quarter of the resolution a side) for every drawn tile and the detail image only within
// its budget, least recently used dropped first; overviews beyond their budget go too, least recently used first, from
// tiles without detail. The subsystem keeps exploration apart (never evicted) and draws a tile again when asked: the
// minimap's area in detail, a zoomed-in world map in detail, a zoomed-out one at least as an overview.
// ---------------------------------------------------------------------------

namespace VoxelMapTileStoreTest
{
	/** A Resolution^2 image whose pixel colour encodes the tile and the pixel (so a downsample is checkable). */
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

	uint64 Key(int32 X, int32 Y)
	{
		return (static_cast<uint64>(static_cast<uint32>(X)) << 32) | static_cast<uint64>(static_cast<uint32>(Y));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapTileStoreBudgetTest,
	"VoxelWorlds.Map.TileStore.Budget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapTileStoreBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapTileStoreTest;
	constexpr int32 Res = 64;
	constexpr int64 DetailBytes = Res * Res * 4;                                   // 16 KB
	constexpr int64 OverviewBytes = (Res / FVoxelMapTileStore::OverviewDownsample) * (Res / FVoxelMapTileStore::OverviewDownsample) * 4; // 1 KB

	// The overview is a box filter of the detail image.
	{
		const TArray<FColor> Image = MakeImage(Res, 9);
		const TArray<FColor> Small = FVoxelMapTileStore::Downsample(Image, Res, 4);
		TestEqual(TEXT("Downsample: 16 x 16"), Small.Num(), 16 * 16);
		// Pixel (2, 3) averages X 8-11 (R = 32..44, mean 38) and Y 12-15 (G = 48..60, mean 54).
		TestEqual(TEXT("Downsample: red is the block's mean"), (int32)Small[3 * 16 + 2].R, 38);
		TestEqual(TEXT("Downsample: green is the block's mean"), (int32)Small[3 * 16 + 2].G, 54);
		TestEqual(TEXT("Downsample: a size that does not divide gives nothing"), FVoxelMapTileStore::Downsample(Image, Res, 5).Num(), 0);
	}

	// Budgets: 64 detail images, 256 overviews.
	FVoxelMapTileStore Store;
	Store.SetBudgets(64 * DetailBytes, 256 * OverviewBytes);
	for (int32 i = 0; i < 400; ++i)
	{
		Store.Store(Key(i, 0), FIntPoint(i, 0), MakeImage(Res, i), Res, /*bKeepDetail*/ true);
		TestTrue(TEXT("Detail never exceeds its budget"), Store.GetDetailBytes() <= 64 * DetailBytes);
		TestTrue(TEXT("Overviews never exceed their budget"), Store.GetOverviewBytes() <= 256 * OverviewBytes);
	}
	AddInfo(FString::Printf(TEXT("400 tiles into 64 detail / 256 overview: %d with an image, %d with detail, evicted %lld detail, %lld overview"),
		Store.Num(), Store.NumDetail(), Store.GetDetailEvictions(), Store.GetOverviewEvictions()));
	TestTrue(TEXT("The newest tiles keep their detail"), Store.HasDetail(Key(399, 0)) && Store.HasDetail(Key(380, 0)));
	TestFalse(TEXT("The oldest lost theirs"), Store.HasDetail(Key(100, 0)));
	TestTrue(TEXT("Some old tiles kept an overview"), Store.Contains(Key(200, 0)) && !Store.HasDetail(Key(200, 0)));
	TestFalse(TEXT("The oldest lost their overview too"), Store.Contains(Key(0, 0)));
	if (const FVoxelMapTile* Tile = Store.Find(Key(200, 0)))
	{
		TestEqual(TEXT("A tile without detail returns its overview"), Tile->Resolution, Res / FVoxelMapTileStore::OverviewDownsample);
		TestEqual(TEXT("... with its own pixels"), Tile->PixelData.Num(), 16 * 16);
	}

	// Least recently USED: reading a tile protects it.
	const uint64 Kept = Key(390, 0);
	for (int32 i = 400; i < 460; ++i)
	{
		Store.Find(Kept); // the minimap reads it every update
		Store.Store(Key(i, 0), FIntPoint(i, 0), MakeImage(Res, i), Res, true);
	}
	TestTrue(TEXT("A tile read all along keeps its detail while newer ones arrive"), Store.HasDetail(Kept));
	TestFalse(TEXT("... while one stored at the same time but not read loses it"), Store.HasDetail(Key(391, 0)));

	// Drawn again on demand: an overview-only tile stored with detail has it back.
	Store.Store(Key(200, 0), FIntPoint(200, 0), MakeImage(Res, 200), Res, true);
	TestTrue(TEXT("A redraw that wants detail restores it"), Store.HasDetail(Key(200, 0)));
	// A redraw that does not ask for detail keeps the detail a tile already has (an edit refresh).
	Store.Store(Key(200, 0), FIntPoint(200, 0), MakeImage(Res, 201), Res, false);
	TestTrue(TEXT("A redraw keeps the detail it had"), Store.HasDetail(Key(200, 0)));
	// A tile restored from a save is stored overview only.
	Store.Store(Key(9000, 0), FIntPoint(9000, 0), MakeImage(Res, 1), Res, false);
	TestTrue(TEXT("Overview only"), Store.Contains(Key(9000, 0)) && !Store.HasDetail(Key(9000, 0)));

	// A fully explored island L: 34,512 tiles restored overview-only, then the minimap's 121 in detail, under the
	// default budgets (32 MB detail, 48 MB overview).
	FVoxelMapTileStore Island;
	Island.SetBudgets(32ll * 1024 * 1024, 48ll * 1024 * 1024);
	for (int32 i = 0; i < 34512; ++i)
	{
		Island.Store(Key(i % 186, i / 186), FIntPoint(i % 186, i / 186), MakeImage(Res, i), Res, false);
	}
	for (int32 i = 0; i < 121; ++i)
	{
		Island.Store(Key(i % 11, i / 11), FIntPoint(i % 11, i / 11), MakeImage(Res, i), Res, true);
	}
	const double TotalMB = (Island.GetDetailBytes() + Island.GetOverviewBytes()) / (1024.0 * 1024.0);
	AddInfo(FString::Printf(TEXT("Island L fully explored: %d tiles with an image, %d in detail, %.1f MB of images (every image kept: %.1f MB)"),
		Island.Num(), Island.NumDetail(), TotalMB, 34512.0 * DetailBytes / (1024.0 * 1024.0)));
	TestEqual(TEXT("Island L: every explored tile keeps its overview"), Island.Num(), 34512);
	TestEqual(TEXT("Island L: the minimap's tiles are in detail"), Island.NumDetail(), 121);
	TestTrue(TEXT("Island L: images within the 80 MB budget"), TotalMB <= 80.0);

	// Unlimited (0) is the behaviour before WM-E2: everything kept in detail.
	FVoxelMapTileStore Unlimited;
	Unlimited.SetBudgets(0, 0);
	for (int32 i = 0; i < 2000; ++i)
	{
		Unlimited.Store(Key(i, 1), FIntPoint(i, 1), MakeImage(Res, i), Res, true);
	}
	TestEqual(TEXT("Unlimited: every detail image kept"), Unlimited.NumDetail(), 2000);
	TestEqual(TEXT("Unlimited: 16 KB each"), Unlimited.GetDetailBytes(), 2000 * DetailBytes);
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMapTileBudgetSubsystemTest,
	"VoxelWorlds.Map.TileStore.ExploredIslandUnderBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMapTileBudgetSubsystemTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMapTileStoreTest;
	IConsoleVariable* DetailVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Map.DetailBudgetMB"));
	IConsoleVariable* OverviewVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Map.OverviewBudgetMB"));
	if (!TestNotNull(TEXT("Budget cvars"), DetailVar) || !TestNotNull(TEXT("Budget cvars"), OverviewVar))
	{
		return true;
	}
	const float OldDetail = DetailVar->GetFloat();
	const float OldOverview = OverviewVar->GetFloat();

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelMapBudgetTestWorld"));
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
		// Tiny budgets so the island overruns them: 16 x 16 tiles are 1 KB in detail and 64 B as an overview. Set at
		// console priority: a value typed in the editor's console would override a lower-priority set.
		DetailVar->Set(TEXT("0.05"), ECVF_SetByConsole);   // ~51 detail images
		OverviewVar->Set(TEXT("0.01"), ECVF_SetByConsole); // ~163 overviews
		Map->ApplyTileBudgets();

		const int32 Explored = static_cast<int32>(Map->ExploreAllTiles(FVector::ZeroVector, 0.0f));
		TestTrue(TEXT("Every tile drew"), DrainMap(Map));
		const FVoxelMapTileStore& Store = Map->GetTileStore();
		AddInfo(FString::Printf(TEXT("%d tiles explored; %s"), Explored, *Map->GetDebugStats()));
		TestTrue(TEXT("The island has hundreds of tiles"), Explored > 300);
		TestEqual(TEXT("Exploration is kept whole"), Map->GetExploredTiles().Num(), Explored);
		TestTrue(TEXT("Images within the overview budget"), Store.GetOverviewBytes() <= int64(0.01 * 1024 * 1024));
		TestTrue(TEXT("Images within the detail budget"), Store.GetDetailBytes() <= int64(0.05 * 1024 * 1024));
		TestTrue(TEXT("Some tiles lost their images (the budget binds)"), Store.Num() < Explored);
		TestEqual(TEXT("A restored exploration keeps no detail"), Store.NumDetail(), 0);

		// The minimap near the centre: its tiles are drawn again in detail, even those that had been evicted.
		Map->RequestTilesInRadius(FVector::ZeroVector, 3000.0f);
		TestTrue(TEXT("The minimap's tiles drew"), DrainMap(Map));
		const FIntPoint Centre = Map->WorldToTileCoord(FVector::ZeroVector);
		TestTrue(TEXT("The minimap's centre tile is in detail"), Map->HasTileDetail(Centre));
		const FVoxelMapTile* Tile = Map->GetTile(Centre);
		TestTrue(TEXT("... at full resolution"), Tile && Tile->Resolution == Config->ChunkSize);

		// The world map asks for a far tile that had lost everything: drawn again as an overview, then in detail.
		FIntPoint Evicted(INT32_MAX, INT32_MAX);
		for (const uint64 PackedKey : Map->GetExploredTiles())
		{
			const FIntPoint Coord(static_cast<int32>(static_cast<uint32>(PackedKey >> 32)), static_cast<int32>(static_cast<uint32>(PackedKey & 0xFFFFFFFF)));
			if (!Map->HasTile(Coord))
			{
				Evicted = Coord;
				break;
			}
		}
		if (TestTrue(TEXT("Some explored tile has no image"), Evicted.X != INT32_MAX))
		{
			TestTrue(TEXT("The world map's request queues it"), Map->RequestTileImage(Evicted, false));
			TestTrue(TEXT("It drew"), DrainMap(Map));
			TestTrue(TEXT("It has its overview back"), Map->HasTile(Evicted) && !Map->HasTileDetail(Evicted));
			TestTrue(TEXT("A zoomed-in request asks for detail"), Map->RequestTileImage(Evicted, true));
			TestTrue(TEXT("It drew"), DrainMap(Map));
			TestTrue(TEXT("It has its detail"), Map->HasTileDetail(Evicted));
		}
		TestTrue(TEXT("Still within the detail budget"), Store.GetDetailBytes() <= int64(0.05 * 1024 * 1024));
	}

	DetailVar->Set(*FString::SanitizeFloat(OldDetail), ECVF_SetByConsole);
	OverviewVar->Set(*FString::SanitizeFloat(OldOverview), ECVF_SetByConsole);
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
		TestTrue(TEXT("Every tile drew"), DrainMap(Map));
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
		TestTrue(TEXT("Every tile drew"), DrainMap(Map));
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
