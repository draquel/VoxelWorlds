// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelValleyConfiguration.h"
#include "VoxelValleyNetwork.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The valley network on made ground (Documentation/VALLEYS_PLAN.md, section 6.1; phase V1).
//
// - Generation.Valleys.Regions: the same network from any tiling. Tiles of 8 cells and of 4 give every node the same
//   place, ground, link, flags, drainage and floor, and every valley link the same curve; a cached tile is the one
//   built from nothing.
// - Generation.Valleys.Rules: on hills falling to a sea, every link steps to a neighbour; every link but a breach's
//   falls in height; no floor rises along a link or a segment; a coast's floor lies MinFloorHeight under the sea and it
//   has no link; a valley link into a coast ends there, at a drowned mouth (V3); every chain ends (at the sea or a sink kept), so the links make no loop.
// - Generation.Valleys.Sinks: the sink rule (D180). A hollow on a slope is breached through its saddle: its sink links
//   on, the breach's steps hold the sink's floor, and its chain reaches the sea. A hollow wider than the breach's
//   reach keeps its sink with no link.
// - Generation.Valleys.Fingerprint: a world with valleys has the save fingerprint's term, with the network's version
//   and its settings; one without has the fingerprint it had; the network's tiling changes no term; a block world and
//   a planet have no valleys yet (the plan's V5).
// ---------------------------------------------------------------------------

namespace VoxelValleyTest
{
	/** Hills on ground falling 1 % toward the sea on both sides of X = 0, the coasts about 6 km out. */
	double Hills(double X, double Y)
	{
		return 6000.0 - 0.01 * FMath::Abs(X) + 3000.0 * FMath::Sin(X / 80000.0) * FMath::Cos(Y / 65000.0) + 1500.0 * FMath::Sin((X - Y) / 31000.0);
	}

	FVoxelValleyGround MakeGround(TFunction<double(double, double)> Height, bool bSea = true)
	{
		FVoxelValleyGround Ground;
		Ground.Height = MoveTemp(Height);
		Ground.bSea = bSea;
		Ground.SeaLevel = 0.0;
		Ground.Seed = 1234;
		return Ground;
	}

	bool SameNode(const FVoxelValleyNode& A, const FVoxelValleyNode& B)
	{
		return A.Cell == B.Cell && A.Position == B.Position && A.Height == B.Height && A.Floor == B.Floor && A.bHasLink == B.bHasLink
			&& (!A.bHasLink || A.Down == B.Down) && A.bSea == B.bSea && A.bSink == B.bSink && A.bBreach == B.bBreach
			&& A.Drainage == B.Drainage && A.bValley == B.bValley && A.bHead == B.bHead;
	}

	/** Every segment of a network over cells [Low, High), by link. */
	TMap<FIntPoint, TArray<FVoxelValleySegment>> SegmentsByLink(const FVoxelValleyNetwork& Network, const FIntPoint& Low, const FIntPoint& High)
	{
		TMap<FIntPoint, TArray<FVoxelValleySegment>> Out;
		const FIntPoint TileLow = Network.TileOf(Low), TileHigh = Network.TileOf(High - FIntPoint(1, 1));
		for (int32 TY = TileLow.Y; TY <= TileHigh.Y; ++TY)
		{
			for (int32 TX = TileLow.X; TX <= TileHigh.X; ++TX)
			{
				for (const FVoxelValleySegment& Segment : Network.GetTile(FIntPoint(TX, TY))->Segments)
				{
					Out.FindOrAdd(Segment.Link).Add(Segment);
				}
			}
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyRegionsTest,
	"VoxelWorlds.Generation.Valleys.Regions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyRegionsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyTest;
	FVoxelValleySettings Eight;
	Eight.TileCells = 8;
	FVoxelValleySettings Four = Eight;
	Four.TileCells = 4;
	const FVoxelValleyNetwork A(Eight, MakeGround(&Hills));
	const FVoxelValleyNetwork B(Four, MakeGround(&Hills));

	// Cells from -16 to 15 each way: 4 x 4 tiles of 8, 8 x 8 of 4
	int32 Nodes = 0, Mismatches = 0, Links = 0, Valleys = 0;
	for (int32 Y = -16; Y < 16; ++Y)
	{
		for (int32 X = -16; X < 16; ++X)
		{
			const FVoxelValleyNode NA = A.GetNode(FIntPoint(X, Y));
			const FVoxelValleyNode NB = B.GetNode(FIntPoint(X, Y));
			++Nodes;
			Links += NA.bHasLink ? 1 : 0;
			Valleys += NA.bValley ? 1 : 0;
			if (!SameNode(NA, NB))
			{
				if (++Mismatches <= 3)
				{
					AddError(FString::Printf(TEXT("cell (%d, %d): the tiles of 8 and of 4 disagree (link %d/%d, drainage %d/%d, floor %.2f/%.2f)"),
						X, Y, NA.bHasLink, NB.bHasLink, NA.Drainage, NB.Drainage, NA.Floor, NB.Floor));
				}
			}
		}
	}
	TestEqual(TEXT("every node is the same from tiles of 8 and of 4"), Mismatches, 0);
	TestTrue(TEXT("the ground has links and valleys"), Links > Nodes / 2 && Valleys > 10);

	// Every valley link's curve, the same from both tilings
	const TMap<FIntPoint, TArray<FVoxelValleySegment>> SA = SegmentsByLink(A, FIntPoint(-16, -16), FIntPoint(16, 16));
	const TMap<FIntPoint, TArray<FVoxelValleySegment>> SB = SegmentsByLink(B, FIntPoint(-16, -16), FIntPoint(16, 16));
	int32 CurveMismatches = 0;
	for (const TPair<FIntPoint, TArray<FVoxelValleySegment>>& Pair : SA)
	{
		const TArray<FVoxelValleySegment>* Other = SB.Find(Pair.Key);
		bool bSame = Other && Other->Num() == Pair.Value.Num();
		for (int32 K = 0; bSame && K < Pair.Value.Num(); ++K)
		{
			const FVoxelValleySegment& P = Pair.Value[K];
			const FVoxelValleySegment& Q = (*Other)[K];
			bSame = P.A == Q.A && P.B == Q.B && P.FloorA == Q.FloorA && P.FloorB == Q.FloorB && P.FloorHalfWidth == Q.FloorHalfWidth
				&& P.ValleyHalfWidth == Q.ValleyHalfWidth && P.TaperA == Q.TaperA && P.TaperB == Q.TaperB;
		}
		CurveMismatches += bSame ? 0 : 1;
	}
	TestEqual(TEXT("every valley link has the same curve from both tilings"), CurveMismatches, 0);
	TestEqual(TEXT("... and both have the same links"), SA.Num(), SB.Num());

	// A cached tile is the tile built from nothing
	const TSharedRef<const FVoxelValleyTile> Cached = A.GetTile(FIntPoint(0, 0));
	const TSharedRef<FVoxelValleyTile> Fresh = FVoxelValleyNetwork::BuildTile(A.GetSettings(), A.GetGround(), FIntPoint(0, 0));
	bool bSame = Cached->Nodes.Num() == Fresh->Nodes.Num() && Cached->Segments.Num() == Fresh->Segments.Num();
	for (int32 K = 0; bSame && K < Cached->Nodes.Num(); ++K)
	{
		bSame = SameNode(Cached->Nodes[K], Fresh->Nodes[K]);
	}
	TestTrue(TEXT("a cached tile is the one built from nothing"), bSame);
	TestTrue(TEXT("a tile asked for again is the one kept"), &A.GetTile(FIntPoint(0, 0)).Get() == &Cached.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyRulesTest,
	"VoxelWorlds.Generation.Valleys.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyRulesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyTest;
	const FVoxelValleyNetwork Network(FVoxelValleySettings(), MakeGround(&Hills));
	// A coast's floor: MinFloorHeight under the sea, a drowned mouth (the valleys plan's V3)
	const double CoastFloor = 0.0 - FVoxelValleySettings().MinFloorHeight;

	int32 Far = 0, Rises = 0, FloorRises = 0, BadCoasts = 0, Loops = 0, Breaches = 0, Coasts = 0, ToSea = 0;
	for (int32 Y = -24; Y < 24; ++Y)
	{
		for (int32 X = -24; X < 24; ++X)
		{
			const FVoxelValleyNode Node = Network.GetNode(FIntPoint(X, Y));
			if (Node.bSea)
			{
				++Coasts;
				BadCoasts += (Node.bHasLink || FMath::Abs(Node.Floor - CoastFloor) > 1.0e-3) ? 1 : 0;
				continue;
			}
			if (!Node.bHasLink)
			{
				continue;
			}
			const FVoxelValleyNode Down = Network.GetNode(Node.Down);
			Far += FMath::Max(FMath::Abs(Node.Down.X - X), FMath::Abs(Node.Down.Y - Y)) > 1 ? 1 : 0;
			Breaches += Node.bBreach ? 1 : 0;
			Rises += (!Node.bBreach && Down.Height >= Node.Height) ? 1 : 0;
			FloorRises += Down.Floor > Node.Floor ? 1 : 0;
			// The chain ends: at the sea or at a sink kept
			FVoxelValleyNode C = Node;
			int32 Steps = 0;
			while (C.bHasLink && Steps < 10000)
			{
				C = Network.GetNode(C.Down);
				++Steps;
			}
			Loops += Steps >= 10000 ? 1 : 0;
			ToSea += C.bSea ? 1 : 0;
		}
	}
	TestEqual(TEXT("every link steps to a neighbour"), Far, 0);
	TestEqual(TEXT("every link but a breach's falls"), Rises, 0);
	TestEqual(TEXT("no floor rises along a link"), FloorRises, 0);
	TestTrue(TEXT("the ground has coasts"), Coasts > 0);
	TestEqual(TEXT("a coast's floor lies under the sea and it has no link"), BadCoasts, 0);
	TestEqual(TEXT("every chain ends: the links make no loop"), Loops, 0);
	TestTrue(TEXT("chains reach the sea"), ToSea > 0);

	// Along each valley link's curve the floor never rises, and a link into a coast ends at the coast's floor
	int32 SegmentRises = 0, MouthsAbove = 0, Mouths = 0;
	for (int32 TY = -3; TY < 3; ++TY)
	{
		for (int32 TX = -3; TX < 3; ++TX)
		{
			const TSharedRef<const FVoxelValleyTile> Tile = Network.GetTile(FIntPoint(TX, TY));
			for (int32 K = 0; K < Tile->Segments.Num(); ++K)
			{
				const FVoxelValleySegment& Segment = Tile->Segments[K];
				SegmentRises += Segment.FloorB > Segment.FloorA + 1.0e-6 ? 1 : 0;
				const bool bLast = K + 1 == Tile->Segments.Num() || Tile->Segments[K + 1].Link != Segment.Link;
				if (bLast && Network.GetNode(Network.GetNode(Segment.Link).Down).bSea)
				{
					++Mouths;
					MouthsAbove += FMath::Abs(Segment.FloorB - CoastFloor) > 1.0e-3 ? 1 : 0;
				}
			}
		}
	}
	TestEqual(TEXT("no floor rises along a curve"), SegmentRises, 0);
	TestTrue(TEXT("valleys reach the sea"), Mouths > 0);
	TestEqual(TEXT("a valley into a coast ends at the coast's floor"), MouthsAbove, 0);
	AddInfo(FString::Printf(TEXT("%d coasts, %d breach steps, %d chains to the sea, %d mouths"), Coasts, Breaches, ToSea, Mouths));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleySinksTest,
	"VoxelWorlds.Generation.Valleys.Sinks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleySinksTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyTest;
	const FVoxelValleySettings Settings;
	const double Cell = Settings.CellSize;

	// A hollow 30 m deep on ground falling 2 % to a sea 4 km out: its sink is breached through the saddle
	{
		auto Height = [](double X, double Y)
		{
			return 8000.0 - 0.02 * X - 3000.0 * FMath::Exp(-(FMath::Square(X - 200000.0) + FMath::Square(Y)) / FMath::Square(60000.0));
		};
		const FVoxelValleyNetwork Network(Settings, MakeGround(Height));
		const FIntPoint Middle = Network.CellOf(FVector2D(200000.0, 0.0));
		FVoxelValleyNode Sink;
		bool bFound = false;
		for (int32 Y = -3; Y <= 3; ++Y)
		{
			for (int32 X = -3; X <= 3; ++X)
			{
				const FVoxelValleyNode Node = Network.GetNode(Middle + FIntPoint(X, Y));
				if (Node.bSink && (!bFound || Node.Height < Sink.Height))
				{
					Sink = Node;
					bFound = true;
				}
			}
		}
		TestTrue(TEXT("the hollow has a sink"), bFound);
		if (bFound)
		{
			TestTrue(TEXT("the sink is breached"), Sink.bHasLink && Sink.bBreach);
			FVoxelValleyNode C = Sink;
			int32 Steps = 0, BadSteps = 0, Rose = 0;
			bool bInBreach = true;
			while (C.bHasLink && Steps < 1000)
			{
				const FVoxelValleyNode Next = Network.GetNode(C.Down);
				Rose += Next.Height > C.Height ? 1 : 0;
				if (bInBreach && C.bBreach)
				{
					BadSteps += FMath::Abs(C.Floor - Sink.Floor) > 1.0e-6 ? 1 : 0;
				}
				bInBreach &= C.bBreach;
				C = Next;
				++Steps;
			}
			TestTrue(TEXT("the breach climbs to the saddle"), Rose > 0);
			TestEqual(TEXT("the breach's steps hold the sink's floor"), BadSteps, 0);
			TestTrue(TEXT("the sink's chain reaches the sea"), C.bSea);
		}
	}

	// A hollow 8 km wide on a plateau with no sea: no lower node within the breach's reach, so the sink keeps no link
	{
		auto Height = [](double X, double Y)
		{
			return 5000.0 - 3000.0 * FMath::Exp(-(FMath::Square(X) + FMath::Square(Y)) / FMath::Square(400000.0));
		};
		const FVoxelValleyNetwork Network(Settings, MakeGround(Height, false));
		FVoxelValleyNode Lowest;
		bool bFound = false;
		for (int32 Y = -2; Y <= 1; ++Y)
		{
			for (int32 X = -2; X <= 1; ++X)
			{
				const FVoxelValleyNode Node = Network.GetNode(FIntPoint(X, Y));
				if (!bFound || Node.Height < Lowest.Height)
				{
					Lowest = Node;
					bFound = true;
				}
			}
		}
		TestTrue(TEXT("the wide hollow's lowest node is a sink"), Lowest.bSink);
		TestFalse(TEXT("... with no link: its hollow is wider than the breach's reach"), Lowest.bHasLink);
		// A node a few cells up the hollow's side drains to it
		FVoxelValleyNode C = Network.GetNode(Lowest.Cell + FIntPoint(3, 0));
		int32 Steps = 0;
		while (C.bHasLink && Steps < 100)
		{
			C = Network.GetNode(C.Down);
			++Steps;
		}
		TestEqual(TEXT("the hollow's side drains to the sink kept"), C.Cell, Lowest.Cell);
	}
	AddInfo(FString::Printf(TEXT("cell %.0f uu, breach reach %d cells"), Cell, Settings.BreachCells));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyFingerprintTest,
	"VoxelWorlds.Generation.Valleys.Fingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyFingerprintTest::RunTest(const FString& Parameters)
{
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->MeshingMode = EMeshingMode::DualContouring;
	const FVoxelWorldFingerprint Without = FVoxelWorldModeFactory::MakeFingerprint(*Config);
	TestFalse(TEXT("a world with no valleys has no term"), Without.Shape.Contains(TEXT("Valleys")));
	TestFalse(TEXT("... and no network"), FVoxelValleyNetwork::Create(*Config).IsValid());

	UVoxelValleyConfiguration* Valleys = NewObject<UVoxelValleyConfiguration>();
	Config->ValleyConfiguration = Valleys;
	const FVoxelWorldFingerprint With = FVoxelWorldModeFactory::MakeFingerprint(*Config);
	TestTrue(TEXT("a world with valleys has the term"), With.Shape.Contains(FString::Printf(TEXT("Valleys=%d:%08X"), FVoxelValleyNetwork::Version, Valleys->Settings.GetGenerationHash())));
	TestFalse(TEXT("... so a save made without them is refused"), With.Matches(Without));
	TestEqual(TEXT("... and the generation version does not move (D176)"), With.GenerationVersion, Without.GenerationVersion);
	TestTrue(TEXT("... and a network"), FVoxelValleyNetwork::Create(*Config).IsValid());

	Valleys->Settings.FloorFraction = 0.35f;
	TestFalse(TEXT("a setting that changes the valleys changes the term"), With.Matches(FVoxelWorldModeFactory::MakeFingerprint(*Config)));
	Valleys->Settings.FloorFraction = FVoxelValleySettings().FloorFraction;
	Valleys->Settings.TileCells = 16;
	TestTrue(TEXT("the network's tiling changes no valley, and not the term"), With.Matches(FVoxelWorldModeFactory::MakeFingerprint(*Config)));

	Config->WorldMode = EWorldMode::IslandBowl;
	TestTrue(TEXT("an island that names one has valleys"), FVoxelValleyNetwork::IsEnabled(*Config) && FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Valleys=")));
	Config->WorldMode = EWorldMode::SphericalPlanet;
	TestTrue(TEXT("a planet that names one has valleys (V5)"), FVoxelValleyNetwork::IsEnabled(*Config) && FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Valleys=")));
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->MeshingMode = EMeshingMode::Cubic;
	TestTrue(TEXT("a block world that names one has valleys (V5)"), FVoxelValleyNetwork::IsEnabled(*Config) && FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Valleys=")));
	Config->ValleyConfiguration = nullptr;
	TestTrue(TEXT("... and one that names none has no term"), !FVoxelValleyNetwork::IsEnabled(*Config) && !FVoxelWorldModeFactory::MakeFingerprint(*Config).Shape.Contains(TEXT("Valleys")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
