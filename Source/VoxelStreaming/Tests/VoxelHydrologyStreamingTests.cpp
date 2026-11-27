// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelHydrologyNetwork.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The hydrology network and a world that generates hydrology (Documentation/HYDROLOGY_PLAN.md, section 5.8; phase H7).
//
// - Streaming.Hydrology.Network: the network on made ground. A complete gather gives the generator's own bodies for
//   the region, worked out then and there and kept (the same gather again works nothing out); a gather for a reader
//   that takes what is at hand gives nothing the first time, queues the tiles for the worker, and gets the bodies once
//   the worker is done, with the tiles that hold a body announced; a warm-up that waits leaves nothing to work out in
//   the gathers inside its disc; the beds come with the bodies, as lines whose ids lie over the roads'; the counters
//   tell a tile worked out on the game thread once the world streams from one worked out before; regions prepared
//   together (PrepareWaterBodies, G297) have their tiles worked out in one build, each once, and the gathers over them
//   then work nothing out.
// - Streaming.Hydrology.World: a streamed plane whose configuration names a hydrology configuration. The network is
//   there when the world's sources register (a complete gather inside the registration finds a generated lake), the
//   fingerprint has the term, no source registers late; the voxels of a generated lake hold flagged water under its
//   level and the world's sample there is the lake's; a river reach's voxels hold water over its bed, with the
//   reach's flow; and no tile is worked out inside a gather on the game thread while the viewer walks on, the worker
//   having been there first; no cell of the network has two ways on, though the plane's relief is as fine as the
//   generator's cells (G231). A world that names no hydrology configuration has no network and no source.
// ---------------------------------------------------------------------------

namespace VoxelHydrologyStreamingTest
{
	FVoxelHydrologyGround MakeRollingGround()
	{
		FVoxelHydrologyGround Ground;
		Ground.Height = [](double X, double Y)
		{
			const auto Dip = [X, Y](double CentreX, double CentreY) { return 900.0 * FMath::Exp(-(FMath::Square(X - CentreX) + FMath::Square(Y - CentreY)) / (1600.0 * 1600.0)); };
			return 900.0 + 2600.0 * FMath::Sin(X / 21000.0) * FMath::Cos(Y / 17000.0) + 1300.0 * FMath::Sin((X + Y) / 9000.0) * FMath::Cos((X - Y) / 12500.0)
				+ 260.0 * FMath::Sin(X / 2900.0 + 1.7) * FMath::Sin(Y / 3300.0 + 0.3) - Dip(12000.0, -6000.0) - Dip(20000.0, 2000.0);
		};
		Ground.bSea = true;
		Ground.SeaLevel = 0.0;
		Ground.VoxelSize = 100.0;
		Ground.ChunkWorldSize = 3200.0;
		Ground.Seed = 4242;
		return Ground;
	}

	FVoxelHydrologySettings MakeSettings()
	{
		FVoxelHydrologySettings Settings;
		Settings.SpringSpacingCells = 8;
		Settings.MinSpringHeight = 600.0f;
		Settings.MinRiverLength = 12000.0f;
		Settings.MaxRiverLength = 60000.0f;
		Settings.MaxMeanGrade = 0.3f;
		Settings.MinLakeArea = 2.0e5f;
		return Settings;
	}

	TSet<uint32> IdsOf(const TArray<FVoxelWaterBody>& Bodies)
	{
		TSet<uint32> Ids;
		for (const FVoxelWaterBody& Body : Bodies)
		{
			Ids.Add(Body.Id);
		}
		return Ids;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyNetworkTest,
	"VoxelWorlds.Streaming.Hydrology.Network",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyNetworkTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHydrologyStreamingTest;
	const FVoxelHydrologyGround Ground = MakeRollingGround();
	const FVoxelHydrologySettings Settings = MakeSettings();
	const FBox2D Box(FVector2D(-30000.0, -20000.0), FVector2D(30000.0, 25000.0));

	// What the generator says the region holds.
	FVoxelHydrologyRegion Expected;
	FVoxelHydrology::BuildRegion(Settings, Ground, Box, nullptr, Expected);
	TSet<uint32> ExpectedIds;
	TSet<int64> ExpectedBeds;
	for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Expected.Lakes)
	{
		ExpectedIds.Add(Lake->Body.Id);
	}
	for (const TSharedRef<const FVoxelHydrologySystem>& System : Expected.Systems)
	{
		for (const FVoxelHydrologyReach& Reach : System->Reaches)
		{
			if (Reach.Body.MeetsRegion(Box))
			{
				ExpectedIds.Add(Reach.Body.Id);
			}
			if (Reach.Bed->MeetsRegion(Box))
			{
				ExpectedBeds.Add(Reach.Bed->GetId());
			}
		}
	}
	if (!TestTrue(TEXT("the ground has lakes and river reaches in the region (the fixture)"), Expected.Lakes.Num() >= 1 && ExpectedBeds.Num() >= 4))
	{
		return true;
	}

	// ---- A complete gather: the generator's bodies, worked out then and there, and kept.
	{
		const TSharedRef<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Network = MakeShared<FVoxelHydrologyNetwork, ESPMode::ThreadSafe>(Ground, Settings, 10000.0);
		TestFalse(TEXT("a new network holds nothing"), Network->HoldsWaterBodies(Box));
		TArray<FVoxelWaterBody> Bodies;
		Network->GatherWaterBodies(Box, EVoxelConditioningGather::Complete, Bodies);
		TestTrue(TEXT("a complete gather gives the generator's lakes and river reaches, each once"), IdsOf(Bodies).Num() == Bodies.Num() && IdsOf(Bodies).Num() == ExpectedIds.Num()
			&& IdsOf(Bodies).Includes(ExpectedIds));
		TArray<FVoxelConditioningLineRef> Beds;
		Network->GatherConditioningLines(Box, EVoxelConditioningGather::Complete, Beds);
		TSet<int64> BedIds;
		bool bOverRoads = true;
		for (const FVoxelConditioningLineRef& Bed : Beds)
		{
			BedIds.Add(Bed->GetId());
			// A road's line id lies under -2^61: a bed is applied after it.
			bOverRoads &= Bed->GetId() > -(static_cast<int64>(1) << 61) && Bed->GetId() < 0 && Bed->GetBlend() == EVoxelConditioningLineBlend::LowerOnly;
		}
		TestTrue(TEXT("... and their beds, lower-only lines, each once"), BedIds.Num() == Beds.Num() && BedIds.Num() == ExpectedBeds.Num() && BedIds.Includes(ExpectedBeds));
		TestTrue(TEXT("... applied after every road and trail between POIs and before a hand-placed line"), bOverRoads);
		TArray<FVoxelConditioningZone> Zones;
		Network->GatherConditioning(Box, Zones);
		TestEqual(TEXT("... and no zone"), Zones.Num(), 0);
		const FVoxelHydrologyStats First = Network->GetStats();
		TestTrue(TEXT("the tiles were worked out inside the gather, before the world streams: not counted as a frame's"), First.StartSyncTiles > 0 && First.SyncTiles == 0
			&& First.Tiles == First.StartSyncTiles && First.Rivers > 0 && First.Conflicts == 0);
		TestTrue(TEXT("the network now holds the region"), Network->HoldsWaterBodies(Box));
		TArray<FVoxelWaterBody> Again;
		Network->GatherWaterBodies(Box, EVoxelConditioningGather::Complete, Again);
		TestTrue(TEXT("the same gather again works nothing out and gives the same bodies"), Network->GetStats().StartSyncTiles == First.StartSyncTiles && IdsOf(Again).Num() == ExpectedIds.Num());
		// A gather over a part gives the part's bodies.
		const FBox2D Part(FVector2D(-5000.0, -5000.0), FVector2D(5000.0, 5000.0));
		TArray<FVoxelWaterBody> InPart;
		Network->GatherWaterBodies(Part, EVoxelConditioningGather::Complete, InPart);
		bool bAllMeet = true;
		for (const FVoxelWaterBody& Body : InPart)
		{
			bAllMeet &= Body.MeetsRegion(Part) && ExpectedIds.Contains(Body.Id);
		}
		TestTrue(TEXT("a gather over a part of it gives the bodies that reach the part"), bAllMeet && InPart.Num() < Bodies.Num());
		Network->MarkStarted();
		TArray<FVoxelWaterBody> Far;
		Network->GatherWaterBodies(FBox2D(FVector2D(200000.0, 200000.0), FVector2D(201000.0, 201000.0)), EVoxelConditioningGather::Complete, Far);
		TestTrue(TEXT("once the world streams, a tile worked out inside a gather on the game thread is counted"), Network->GetStats().SyncTiles > 0);
		Network->Shutdown();
	}

	// ---- A gather that takes what is at hand: nothing, then the worker's, announced.
	{
		const TSharedRef<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Network = MakeShared<FVoxelHydrologyNetwork, ESPMode::ThreadSafe>(Ground, Settings, 10000.0);
		const FBox2D Small(FVector2D(-12000.0, -12000.0), FVector2D(12000.0, 12000.0));
		TArray<FVoxelWaterBody> Bodies;
		Network->GatherWaterBodies(Small, EVoxelConditioningGather::Ready, Bodies);
		TestTrue(TEXT("a ready gather on a new network gives nothing and works nothing out on its thread"), Bodies.Num() == 0 && Network->GetStats().SyncTiles == 0
			&& Network->GetStats().StartSyncTiles == 0 && Network->GetStats().ReadyMisses > 0);
		Network->WaitForWorker();
		const FVoxelHydrologyStats Done = Network->GetStats();
		TestTrue(TEXT("the worker works the tiles out"), Done.WorkerTiles == Done.ReadyMisses && Done.Queued == 0 && Network->HoldsWaterBodies(Small));
		TArray<FVoxelConditioningRegion> Announced;
		Network->TakeAnnouncements(Announced);
		TestTrue(TEXT("the tiles that hold a body are announced, once"), Announced.Num() > 0 && Announced.Num() == Done.Announced && Announced.Num() <= Done.TilesWithBodies);
		TestTrue(TEXT("... and what draws the bodies is told they arrived, once"), Network->TakeBodiesArrived() && !Network->TakeBodiesArrived());
		TArray<FVoxelConditioningRegion> None;
		Network->TakeAnnouncements(None);
		TestEqual(TEXT("... the list is emptied"), None.Num(), 0);
		Network->GatherWaterBodies(Small, EVoxelConditioningGather::Ready, Bodies);
		TArray<FVoxelWaterBody> Complete;
		Network->GatherWaterBodies(Small, EVoxelConditioningGather::Complete, Complete);
		TestTrue(TEXT("the next ready gather gives what a complete one does"), Bodies.Num() > 0 && IdsOf(Bodies).Num() == IdsOf(Complete).Num() && IdsOf(Bodies).Includes(IdsOf(Complete)));
		// A region too wide for a reader that takes what is at hand: what is kept, and nothing queued.
		TArray<FVoxelWaterBody> Wide;
		Network->GatherWaterBodies(FBox2D(FVector2D(-2000000.0, -2000000.0), FVector2D(2000000.0, 2000000.0)), EVoxelConditioningGather::Ready, Wide);
		TestTrue(TEXT("a ready gather over a very wide region gives the bodies kept and queues nothing"), IdsOf(Wide).Includes(IdsOf(Complete)) && Network->GetStats().Queued == 0
			&& Network->GetStats().ReadyTooWide == 1);
		Network->Shutdown();
	}

	// ---- A warm-up that waits: the gathers inside its disc work nothing out.
	{
		const TSharedRef<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Network = MakeShared<FVoxelHydrologyNetwork, ESPMode::ThreadSafe>(Ground, Settings, 10000.0);
		const FVector2D Centre(3000.0, -2000.0);
		const TArray<FVector2D> Centres = { Centre };
		const double Radius = Network->GetWarmUpRadius();
		const int32 Warmed = Network->WarmUp(Centres, Radius, /*bWait=*/true);
		Network->MarkStarted();
		TestTrue(TEXT("a warm-up that waits works the disc's tiles out"), Warmed > 0 && Network->GetStats().WarmTiles == Warmed && Radius > 10000.0);
		TestEqual(TEXT("the same warm-up again finds them kept"), Network->WarmUp(Centres, Radius, /*bWait=*/true), 0);
		TArray<FVoxelWaterBody> Bodies;
		FRandomStream Random(77);
		for (int32 Index = 0; Index < 200; ++Index)
		{
			const FVector2D At = Centre + FVector2D(Random.FRandRange(-1.0, 1.0), Random.FRandRange(-1.0, 1.0)) * (Radius * 0.65);
			Network->GatherWaterBodies(FBox2D(At - FVector2D(1600.0, 1600.0), At + FVector2D(1600.0, 1600.0)), EVoxelConditioningGather::Complete, Bodies);
		}
		TestTrue(TEXT("complete gathers over chunks inside the disc work nothing out"), Network->GetStats().SyncTiles == 0 && Network->GetStats().StartSyncTiles == 0 && Bodies.Num() > 0);
		// The worker's warm-up: queued, the nearest first, worked out off this thread.
		const int32 Queued = Network->WarmUp(Centres, 1.6 * Radius, /*bWait=*/false);
		Network->WaitForWorker();
		TestTrue(TEXT("a warm-up that does not wait queues the rest for the worker"), Queued > 0 && Network->GetStats().WorkerTiles == Queued && Network->GetStats().SyncTiles == 0);
		Network->Shutdown();
	}

	// ---- A region asked for (G232): the worker walks it and works out its tiles, the nearest to the place first, up to a
	// limit per ask; nothing is worked out on the asking thread.
	{
		const TSharedRef<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Network = MakeShared<FVoxelHydrologyNetwork, ESPMode::ThreadSafe>(Ground, Settings, 10000.0);
		Network->MarkStarted();
		const FBox2D Region(FVector2D(40000.0, -60000.0), FVector2D(160000.0, 60000.0));
		// The place off any tile's edge, so the tile it lies in is the nearest; the far corner's tile is not among two.
		const FVector Place(41234.5, 1234.5, 0.0);
		const FBox2D Near(FVector2D(Place.X - 10.0, Place.Y - 10.0), FVector2D(Place.X + 10.0, Place.Y + 10.0));
		const FBox2D Far(FVector2D(159980.0, 59980.0), FVector2D(160000.0, 60000.0));
		int32 Kept = 0, Waiting = 0, Limit = 0;
		TestTrue(TEXT("a region of this world is taken"), Network->RequestRegion(FVoxelConditioningRegion::OfBox(Region), Place, 2));
		TestFalse(TEXT("... a cap on a heightmap world is not"), Network->RequestRegion(FVoxelConditioningRegion::OfCap(FVector::UpVector, 0.1), Place, 2));
		Network->WaitForWorker();
		Network->GetLoad(Kept, Waiting, Limit);
		const FVoxelHydrologyStats Limited = Network->GetStats();
		TestTrue(TEXT("an ask queues no more tiles than its limit, worked out by the worker"), Limited.RequestedTiles == 2 && Limited.WorkerTiles == 2 && Kept == 2 && Waiting == 0
			&& Limit > Kept && Limited.SyncTiles == 0);
		TestTrue(TEXT("... the tiles nearest the place first"), Network->HoldsWaterBodies(Near) && !Network->HoldsWaterBodies(Far));
		Network->RequestRegion(FVoxelConditioningRegion::OfBox(Region), Place, TNumericLimits<int32>::Max());
		Network->WaitForWorker();
		const FVoxelHydrologyStats Whole = Network->GetStats();
		TestTrue(TEXT("asked for whole, the region is held, each tile worked out once"), Network->HoldsWaterBodies(Region) && Whole.WorkerTiles == Whole.RequestedTiles
			&& Whole.RequestedTiles > 2 && Whole.SyncTiles == 0);
		Network->Shutdown();
		TestFalse(TEXT("a network shut down takes no ask"), Network->RequestRegion(FVoxelConditioningRegion::OfBox(Region), Place, 2));
	}

	// ---- Regions prepared together (G297): the tiles of several regions worked out in one build, each tile once; the
	// gathers over them then work nothing out and give what a complete gather on a fresh network gives. On the game thread
	// before the world streams they count as a gather's there, on a task as a gather's off the game thread.
	{
		const TSharedRef<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Network = MakeShared<FVoxelHydrologyNetwork, ESPMode::ThreadSafe>(Ground, Settings, 10000.0);
		// The region of the gathers above (it holds lakes and river reaches) in two halves, a box inside the first (its
		// tiles once), a box far away, and one too wide for a complete gather (skipped).
		const TArray<FBox2D> Regions = {
			FBox2D(Box.Min, FVector2D(Box.GetCenter().X, Box.Max.Y)),
			FBox2D(FVector2D(Box.GetCenter().X, Box.Min.Y), Box.Max),
			FBox2D(Box.Min + FVector2D(1000.0, 1000.0), Box.Min + FVector2D(3000.0, 3000.0)),
			FBox2D(FVector2D(160000.0, 160000.0), FVector2D(161000.0, 161000.0)),
			FBox2D(FVector2D(-4000000.0, -4000000.0), FVector2D(4000000.0, 4000000.0)) };
		const int32 Prepared = Network->PrepareWaterBodies(Regions);
		const FVoxelHydrologyStats After = Network->GetStats();
		AddInfo(FString::Printf(TEXT("prepared: %d tile(s) of %d region(s) in %.1f ms"), Prepared, Regions.Num(), After.StartSyncMs));
		TestTrue(TEXT("the regions' tiles are worked out together, each once, counted as a gather's before the world streams"), Prepared > 2
			&& After.StartSyncTiles == Prepared && After.Tiles == Prepared && After.SyncTiles == 0 && After.OffThreadTiles == 0);
		bool bHeld = true;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			bHeld &= Network->HoldsWaterBodies(Regions[Index]);
		}
		TestTrue(TEXT("... and kept: each region is held"), bHeld);
		TestFalse(TEXT("... but not the region too wide to work out"), Network->HoldsWaterBodies(Regions[4]));
		TestEqual(TEXT("prepared again, nothing is left to work out"), Network->PrepareWaterBodies(Regions), 0);
		const TSharedRef<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Fresh = MakeShared<FVoxelHydrologyNetwork, ESPMode::ThreadSafe>(Ground, Settings, 10000.0);
		bool bSame = true;
		int32 Bodies = 0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			TArray<FVoxelWaterBody> Here, There;
			Network->GatherWaterBodies(Regions[Index], EVoxelConditioningGather::Complete, Here);
			Fresh->GatherWaterBodies(Regions[Index], EVoxelConditioningGather::Complete, There);
			bSame &= IdsOf(Here).Num() == IdsOf(There).Num() && IdsOf(Here).Includes(IdsOf(There));
			Bodies += Here.Num();
		}
		TestTrue(TEXT("the regions have bodies (non-vacuous)"), Bodies > 0);
		TestTrue(TEXT("gathers over the regions work nothing out and give a fresh network's bodies"), bSame && Network->GetStats().StartSyncTiles == Prepared);
		Network->Shutdown();
		Fresh->Shutdown();

		const TSharedRef<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> OnTask = MakeShared<FVoxelHydrologyNetwork, ESPMode::ThreadSafe>(Ground, Settings, 10000.0);
		int32 PreparedOnTask = 0;
		// A thread of its own: a task waited for may run on the waiting thread.
		Async(EAsyncExecution::Thread, [&OnTask, &Regions, &PreparedOnTask]() { PreparedOnTask = OnTask->PrepareWaterBodies(Regions); }).Wait();
		TestTrue(TEXT("on another thread: the same tiles, counted as a gather's off the game thread"), PreparedOnTask == Prepared && OnTask->GetStats().OffThreadTiles == Prepared
			&& OnTask->GetStats().StartSyncTiles == 0);
		OnTask->Shutdown();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHydrologyWorldTest,
	"VoxelWorlds.Streaming.Hydrology.World",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHydrologyWorldTest::RunTest(const FString& Parameters)
{
	// ---- A world that names no hydrology configuration: no network, no source, the sea alone.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("VoxelHydrologyOffWorld"));
		W.ConfigureSmallPlane();
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = -300.0f;
		W.StartManager();
		TestTrue(TEXT("a world that names no hydrology configuration has no network, no source of bodies and no conditioner"),
			!W.Manager->GetHydrologyNetwork().IsValid() && !W.Manager->GetThreadSafeWaterBodySource().IsValid() && !W.Manager->HasWaterBodies()
			&& !W.Manager->HasTerrainConditioner());
		W.Stop();
	}

	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("VoxelHydrologyWorld"));
	W.ConfigureSmallPlane();
	W.Config->bEnableWaterLevel = true;
	W.Config->WaterLevel = -300.0f;
	UVoxelHydrologyConfiguration* Hydrology = NewObject<UVoxelHydrologyConfiguration>();
	Hydrology->AddToRoot();
	// The test plane's hills are a few hundred units high and a kilometre or two across: springs close together, short
	// rivers, small lakes.
	Hydrology->Settings.SpringSpacingCells = 6;
	Hydrology->Settings.MinSpringHeight = 150.0f;
	Hydrology->Settings.MinRiverLength = 2500.0f;
	Hydrology->Settings.MaxRiverLength = 20000.0f;
	Hydrology->Settings.MaxMeanGrade = 0.6f;
	Hydrology->Settings.MinLakeDepthVoxels = 1.5f;
	Hydrology->Settings.MinLakeArea = 2.0e5f;
	Hydrology->Settings.HeadLength = 400.0f;
	W.Config->HydrologyConfiguration = Hydrology;

	// What registers on the world's start finds the network there: a complete gather inside the registration.
	int32 BodiesAtRegistration = INDEX_NONE;
	bool bNetworkAtRegistration = false;
	const FDelegateHandle Handle = UVoxelChunkManager::OnInitialized.AddLambda([&](UVoxelChunkManager& Manager)
	{
		bNetworkAtRegistration = Manager.GetHydrologyNetwork().IsValid() && Manager.GetThreadSafeWaterBodySource().IsValid();
		const FVoxelWaterLevels Levels = Manager.GatherWaterLevels(FBox(FVector(-60000.0, -60000.0, -1.0e6), FVector(60000.0, 60000.0, 1.0e6)), EVoxelConditioningGather::Complete);
		BodiesAtRegistration = Levels.GetBodies().Num();
	});
	W.StartManager();
	UVoxelChunkManager::OnInitialized.Remove(Handle);
	UVoxelChunkManager* Manager = W.Manager;
	const TSharedPtr<FVoxelHydrologyNetwork, ESPMode::ThreadSafe> Network = Manager->GetHydrologyNetwork();
	if (!TestTrue(TEXT("a plane that names a hydrology configuration has a network"), Network.IsValid()))
	{
		W.Stop();
		Hydrology->RemoveFromRoot();
		return true;
	}
	TestTrue(TEXT("... there when the world's sources register, and a complete gather then finds generated bodies"), bNetworkAtRegistration && BodiesAtRegistration > 1);
	TestTrue(TEXT("... registered as a source of bodies and of beds"), Manager->HasWaterBodies() && Manager->HasTerrainConditioner() && Manager->GetNumWaterBodies() == 0);
	TestTrue(TEXT("the world's fingerprint has the hydrology's term"), Manager->GetWorldFingerprint().Shape.Contains(FString::Printf(TEXT("Hydrology=%d:"), FVoxelHydrology::Version)));

	// The bodies round the start: a lake and a river reach to stream to.
	FVoxelHydrologyRegion Region;
	Network->Survey(FBox2D(FVector2D(-60000.0, -60000.0), FVector2D(60000.0, 60000.0)), Region);
	const FVoxelHydrologyLake* Lake = nullptr;
	for (const TSharedRef<const FVoxelHydrologyLake>& Candidate : Region.Lakes)
	{
		if (!Lake || Candidate->Depth > Lake->Depth)
		{
			Lake = &Candidate.Get();
		}
	}
	const FVoxelHydrologyReach* Reach = nullptr;
	int32 NumReaches = 0;
	for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
	{
		for (const FVoxelHydrologyReach& Candidate : System->Reaches)
		{
			++NumReaches;
			if (!Reach || Candidate.Length > Reach->Length)
			{
				Reach = &Candidate;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("the test plane within 600 m of its origin: %d lakes, %d river systems with %d reaches (%d refused); the longest reach %.0f uu, %d vertices"),
		Region.Lakes.Num(), Region.Systems.Num(), NumReaches, Region.NumRefused, Reach ? Reach->Length : 0.0, Reach ? Reach->Body.Reach->GetVertices().Num() : 0));
	for (const TArray<TSharedRef<const FVoxelHydrologySystem>>* List : { &Region.Systems, &Region.Refused })
	{
		for (const TSharedRef<const FVoxelHydrologySystem>& System : *List)
		{
			if (!System->ConflictNote.IsEmpty())
			{
				AddInfo(FString::Printf(TEXT("two ways on (%s): %s"), System->Refused.IsEmpty() ? TEXT("a system made") : *System->Refused, *System->ConflictNote));
				break;
			}
		}
	}
	if (!TestTrue(TEXT("the test plane has a generated lake and a river reach (the fixture)"), Lake != nullptr && Reach != nullptr))
	{
		W.Stop();
		Hydrology->RemoveFromRoot();
		return true;
	}

	// ---- The lake: its voxels hold flagged water, and the world's sample is the lake's.
	const double CellSize = Network->GetSettings().CellVoxels * W.Config->VoxelSize;
	const FVector2D Pit(Lake->LowestCell.X * CellSize, Lake->LowestCell.Y * CellSize);
	const FVector InLake(Pit.X, Pit.Y, Lake->WaterLevel - FMath::Min(0.5 * Lake->Depth, 60.0));
	W.SetViewer(FVector(Pit.X, Pit.Y, Lake->WaterLevel + 300.0));
	if (TestTrue(TEXT("the world settles round the lake"), W.Settle()))
	{
		FVoxelWaterSample Sample;
		TestTrue(TEXT("a voxel of the generated lake holds flagged water"), Manager->GetVoxelAt(InLake).HasWaterFlag());
		TestTrue(TEXT("the world's sample in it is the lake's"), Manager->SampleWater(InLake, Sample) && Sample.bWater && Sample.Kind == EVoxelWaterBodyKind::Lake
			&& Sample.BodyId == Lake->Body.Id && Sample.Depth > 0.0 && FMath::IsNearlyEqual(Sample.SurfaceLevel, Lake->WaterLevel, 0.01));
		TestFalse(TEXT("a voxel over it holds none"), Manager->GetVoxelAt(FVector(Pit.X, Pit.Y, Lake->WaterLevel + 150.0)).HasWaterFlag());
		TestEqual(TEXT("no source registered late"), Manager->GetNumLateConditioningSources(), 0);
	}

	// ---- The river reach: its bed is cut and holds the reach's water, which runs.
	const TArray<FVoxelWaterReachVertex>& Vertices = Reach->Body.Reach->GetVertices();
	int32 At = INDEX_NONE;
	for (int32 Index = 4; Index < Vertices.Num() - 4 && At == INDEX_NONE; ++Index)
	{
		if (Vertices[Index].SurfaceLevel - Vertices[Index].BedLevel > 0.9 * Network->GetSettings().DepthVoxels * W.Config->VoxelSize
			&& Vertices[Index].SurfaceLevel > W.Config->WaterLevel + 30.0)
		{
			At = Index;
		}
	}
	if (TestTrue(TEXT("the river reach has a vertex at its full depth on land (the fixture)"), At != INDEX_NONE))
	{
		const FVoxelWaterReachVertex& Vertex = Vertices[At];
		const FVector InRiver(Vertex.Position.X, Vertex.Position.Y, 0.5 * (Vertex.SurfaceLevel + Vertex.BedLevel));
		W.SetViewer(InRiver + FVector(0.0, 0.0, 300.0));
		if (TestTrue(TEXT("the world settles round the river reach"), W.Settle()))
		{
			FVoxelWaterSample Sample;
			TestTrue(TEXT("a voxel of the generated river reach holds flagged water: the bed is cut and the reach fills it"), Manager->GetVoxelAt(InRiver).HasWaterFlag());
			TestTrue(TEXT("the generated ground on the reach's line lies under the natural ground: the bed"),
				Manager->GetGeneratedSurfacePoint(FVector(Vertex.Position.X, Vertex.Position.Y, 0.0)).Z < Manager->GetSurfacePointAt(FVector(Vertex.Position.X, Vertex.Position.Y, 0.0)).Position.Z - 20.0);
			const bool bSampled = Manager->SampleWater(InRiver, Sample);
			TestTrue(TEXT("the world's sample there is the river reach's, with its flow"), bSampled && Sample.Kind == EVoxelWaterBodyKind::Reach && Sample.Depth > 0.0
				&& FMath::IsNearlyEqual(Sample.Flow.Size(), static_cast<double>(Network->GetSettings().FlowSpeed), 0.01));
		}
	}

	// ---- No tile worked out inside a gather while the viewer walks on: the worker is there first.
	Network->WaitForWorker();
	Network->ResetStats();
	FVector Viewer(Pit.X, Pit.Y, Lake->WaterLevel + 300.0);
	W.SetViewer(Viewer);
	W.Settle();
	Network->WaitForWorker();
	Network->ResetStats();
	bool bSettled = true;
	for (int32 Step = 0; Step < 6 && bSettled; ++Step)
	{
		Viewer += FVector(2.0 * W.Config->GetChunkWorldSize(), 1.0 * W.Config->GetChunkWorldSize(), 0.0);
		W.SetViewer(Viewer);
		bSettled = W.Settle();
		// A walker gives the worker the time a settle takes; the test waits for it outright.
		Network->WaitForWorker();
	}
	const FVoxelHydrologyStats Stats = Network->GetStats();
	AddInfo(FString::Printf(TEXT("the walk: %lld complete gathers, %lld tiles by the worker, %lld inside a gather on the game thread, %lld on another thread"),
		Stats.CompleteGathers, Stats.WorkerTiles, Stats.SyncTiles, Stats.OffThreadTiles));
	TestTrue(TEXT("the world settles at each step of a walk of twelve chunks"), bSettled);
	TestTrue(TEXT("the chunks along the walk asked for their bodies (non-vacuous)"), Stats.CompleteGathers > 100);
	TestEqual(TEXT("no tile is worked out inside a gather on the game thread along the walk"), Stats.SyncTiles, static_cast<int64>(0));
	// The test plane's relief is as fine as the generator's cells (hollows a cell or two across, some under the sea's
	// level), far finer than a world's: two hollows there meet in one saddle at one level, the first's outlet the
	// second's cell, and a descent from the first goes on from the saddle by the second's outlet (G231).
	AddInfo(FString::Printf(TEXT("cells with two ways on in the network: %d of %d systems' cells"), Stats.Conflicts, Stats.Systems));
	TestTrue(TEXT("the network's systems along the walk (non-vacuous)"), Stats.Systems > 1000);
	TestEqual(TEXT("no cell of the network has two ways on"), Stats.Conflicts, 0);

	W.Stop();
	Hydrology->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
