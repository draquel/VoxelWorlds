// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelChunkValleySegments.h"
#include "VoxelConditioningLineShape.h"
#include "VoxelCubeSphere.h"
#include "VoxelHydrology.h"
#include "VoxelPolyline.h"
#include "VoxelValleyNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Valleys on planets and block worlds (Documentation/VALLEYS_PLAN.md, phase V5), on a made planet of hills over a sea
// (a datum radius of 10 km, 45 lattice cells along a face's edge) and on the made hills of V3's tests with block tops.
//
// - Generation.Valleys.Planet.Regions: the plan's first step, valleys that meet across a cube face's edge from both
//   sides. Tiles of 8 cells and of 4 (their edges fall elsewhere, across the faces' edges and round the cube's corners)
//   give every node of the planet the same place, ground, link, flags, drainage and floor, and every valley link the
//   same curve; valley links run across faces' edges; a cached tile is the one built from nothing.
// - Generation.Valleys.Planet.Rules: every link steps to one of its cell's neighbours on the sphere; every link but a
//   breach's falls in altitude; no floor rises along a link or a segment; a coast's floor lies MinFloorHeight under the
//   sea and it has no link; every chain ends; every point of a curve lies on the datum sphere.
// - Generation.Valleys.Planet.Carve: the point query (FVoxelValleyNetwork::ApplyToAltitude) and a chunk's list
//   (FVoxelChunkValleySegments::BuildOnSphere, the generators' floats) agree along and across the valleys; a floor's
//   middle holds its floor or lies deeper; away from every valley and under the sea the altitude is the natural one;
//   the carve's change has no step.
// - Generation.Hydrology.ValleyRivers.Planet: a piece a valley link on the sphere: its vertices are directions, its
//   surface never rises, it meets the piece below at the node and never under it, a head starts from nothing, a piece
//   into a coast ends a tuck under the sea, its bed keeps a line's limits on a planet; two caps that overlap give the
//   pieces they share the same.
// - Generation.Hydrology.ValleyRivers.Blocks: on a block world no piece's bed steps more than a block a column (the
//   engine's own check of a line on a block world finds nothing), and the pieces are the smooth world's otherwise.
// ---------------------------------------------------------------------------

namespace VoxelValleyPlanetTest
{
	constexpr double PlanetRadius = 1000000.0;

	/** Hills a few kilometres across over a sea at the datum, with smaller relief on them. */
	double Hills(const FVector& Direction)
	{
		const FVector P = Direction * PlanetRadius;
		return 2000.0 + 6000.0 * FMath::Sin(P.X / 180000.0) * FMath::Cos(P.Y / 150000.0) + 3000.0 * FMath::Sin((P.X + P.Z) / 110000.0) * FMath::Cos((P.Y - P.Z) / 130000.0)
			+ 600.0 * FMath::Sin(P.Z / 37000.0 + 1.7) * FMath::Sin(P.X / 41000.0 + 0.3);
	}

	FVoxelValleyGround MakeGround()
	{
		FVoxelValleyGround Ground;
		Ground.bRadial = true;
		Ground.PlanetRadius = PlanetRadius;
		Ground.Altitude = &Hills;
		Ground.bSea = true;
		Ground.SeaLevel = 0.0;
		Ground.Seed = 4242;
		return Ground;
	}

	/** The test's settings: D181's, with a shorter drainage reach and breach (a smaller margin, so tiles of 4 have margins that matter). */
	FVoxelValleySettings MakeSettings(int32 TileCells)
	{
		FVoxelValleySettings Settings;
		Settings.TileCells = TileCells;
		Settings.DrainageReach = 3;
		Settings.BreachCells = 3;
		return Settings;
	}

	bool SameNode(const FVoxelValleyNode& A, const FVoxelValleyNode& B)
	{
		return A.Cell == B.Cell && A.Position == B.Position && A.Height == B.Height && A.Floor == B.Floor && A.bHasLink == B.bHasLink
			&& (!A.bHasLink || A.Down == B.Down) && A.bSea == B.bSea && A.bSink == B.bSink && A.bBreach == B.bBreach
			&& A.Drainage == B.Drainage && A.bValley == B.bValley && A.bHead == B.bHead;
	}

	/** Every cell of a planet's lattice. */
	TArray<FIntPoint> AllCells(const FVoxelValleyNetwork& Network)
	{
		TArray<FIntPoint> Cells;
		const int32 N = Network.GetLattice().GetCells().GetCellsPerEdge();
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < 6 * N; ++X)
			{
				Cells.Add(FIntPoint(X, Y));
			}
		}
		return Cells;
	}

	int32 FaceOf(const FVoxelValleyNetwork& Network, const FIntPoint& Cell)
	{
		return Cell.X / FMath::Max(Network.GetLattice().GetCells().GetCellsPerEdge(), 1);
	}

	/** Every segment of a planet's network, by link. */
	TMap<FIntPoint, TArray<FVoxelValleySegment>> SegmentsByLink(const FVoxelValleyNetwork& Network)
	{
		TMap<FIntPoint, TArray<FVoxelValleySegment>> Out;
		const int32 PerEdge = Network.GetLattice().GetTilesPerEdge();
		for (int32 TY = 0; TY < PerEdge; ++TY)
		{
			for (int32 TX = 0; TX < 6 * PerEdge; ++TX)
			{
				for (const FVoxelValleySegment& Segment : Network.GetTile(FIntPoint(TX, TY))->Segments)
				{
					Out.FindOrAdd(Segment.Link).Add(Segment);
				}
			}
		}
		return Out;
	}

	/** What the generator keeps, on one thread. */
	struct FCache : IVoxelHydrologyCache
	{
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologyTrace>> Traces;
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologyLake>> Lakes;
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologySystem>> Systems;
		TMap<FIntPoint, TSharedRef<const FVoxelHydrologySystem>> ValleyRivers;
		virtual TSharedPtr<const FVoxelHydrologyTrace> FindTrace(const FIntPoint& Square) override { const auto* F = Traces.Find(Square); return F ? F->ToSharedPtr() : nullptr; }
		virtual TSharedRef<const FVoxelHydrologyTrace> StoreTrace(const TSharedRef<const FVoxelHydrologyTrace>& T) override { return Traces.FindOrAdd(T->Square, T); }
		virtual TSharedPtr<const FVoxelHydrologyLake> FindLake(const FIntPoint& Pit) override { const auto* F = Lakes.Find(Pit); return F ? F->ToSharedPtr() : nullptr; }
		virtual TSharedRef<const FVoxelHydrologyLake> StoreLake(const TSharedRef<const FVoxelHydrologyLake>& L) override { return Lakes.FindOrAdd(L->Pit, L); }
		virtual TSharedPtr<const FVoxelHydrologySystem> FindSystem(const FIntPoint& Mouth) override { const auto* F = Systems.Find(Mouth); return F ? F->ToSharedPtr() : nullptr; }
		virtual TSharedRef<const FVoxelHydrologySystem> StoreSystem(const TSharedRef<const FVoxelHydrologySystem>& S) override { return Systems.FindOrAdd(S->Mouth, S); }
		virtual TSharedPtr<const FVoxelHydrologySystem> FindValleyRiver(const FIntPoint& Link) override { const auto* F = ValleyRivers.Find(Link); return F ? F->ToSharedPtr() : nullptr; }
		virtual TSharedRef<const FVoxelHydrologySystem> StoreValleyRiver(const TSharedRef<const FVoxelHydrologySystem>& R) override { return ValleyRivers.FindOrAdd(R->Mouth, R); }
	};

	/** The hydrology's ground on the made planet: the hills with the valleys carved, as the planet's world mode gives it. */
	FVoxelHydrologyGround MakeHydrologyGround(const TSharedPtr<FVoxelValleyNetwork>& Valleys)
	{
		FVoxelHydrologyGround Ground;
		Ground.bRadial = true;
		Ground.PlanetCenter = FVector::ZeroVector;
		Ground.PlanetRadius = PlanetRadius;
		Ground.Altitude = [Valleys](const FVector& Direction) { return Valleys->ApplyToAltitude(Direction, Hills(Direction)); };
		Ground.bSea = true;
		Ground.SeaLevel = 0.0;
		Ground.VoxelSize = 75.0;
		Ground.ChunkWorldSize = 2400.0;
		Ground.Seed = 4242;
		Ground.Valleys = Valleys;
		return Ground;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyPlanetRegionsTest,
	"VoxelWorlds.Generation.Valleys.Planet.Regions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyPlanetRegionsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyPlanetTest;
	const FVoxelValleyNetwork A(MakeSettings(8), MakeGround());
	const FVoxelValleyNetwork B(MakeSettings(4), MakeGround());
	const int32 N = A.GetLattice().GetCells().GetCellsPerEdge();
	TestEqual(TEXT("both tilings have the same cells"), B.GetLattice().GetCells().GetCellsPerEdge(), N);

	int32 Nodes = 0, Mismatches = 0, Links = 0, Valleys = 0, Across = 0, ValleysAcross = 0, Corners = 0;
	for (const FIntPoint& Cell : AllCells(A))
	{
		const FVoxelValleyNode NA = A.GetNode(Cell);
		const FVoxelValleyNode NB = B.GetNode(Cell);
		++Nodes;
		Links += NA.bHasLink ? 1 : 0;
		Valleys += NA.bValley ? 1 : 0;
		if (NA.bHasLink && FaceOf(A, NA.Down) != FaceOf(A, Cell))
		{
			++Across;
			ValleysAcross += NA.bValley ? 1 : 0;
		}
		int32 Neighbours = 0;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			FIntPoint Next;
			Neighbours += A.GetNeighbour(Cell, Index, Next) ? 1 : 0;
		}
		Corners += Neighbours == 7 ? 1 : 0;
		if (!SameNode(NA, NB))
		{
			if (++Mismatches <= 3)
			{
				AddError(FString::Printf(TEXT("cell (%d, %d): the tiles of 8 and of 4 disagree (link %d/%d, drainage %d/%d, floor %.2f/%.2f)"),
					Cell.X, Cell.Y, NA.bHasLink, NB.bHasLink, NA.Drainage, NB.Drainage, NA.Floor, NB.Floor));
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d cells (%d along an edge), %d links, %d valley links, %d links across a face's edge (%d valleys), %d cells at the cube's corners"),
		Nodes, N, Links, Valleys, Across, ValleysAcross, Corners));
	TestEqual(TEXT("every node is the same from tiles of 8 and of 4"), Mismatches, 0);
	TestTrue(TEXT("the planet has links and valleys"), Links > Nodes / 4 && Valleys > 20);
	TestTrue(TEXT("valley links run across faces' edges"), ValleysAcross > 0);
	TestEqual(TEXT("the cube's eight corners have three cells each with seven neighbours"), Corners, 24);

	// Every valley link's curve, the same from both tilings
	const TMap<FIntPoint, TArray<FVoxelValleySegment>> SA = SegmentsByLink(A);
	const TMap<FIntPoint, TArray<FVoxelValleySegment>> SB = SegmentsByLink(B);
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

	// A cached tile is the tile built from nothing (one at a cube's corner: face 0's last tile along both edges)
	const FIntPoint CornerTile(A.GetLattice().GetTilesPerEdge() - 1, A.GetLattice().GetTilesPerEdge() - 1);
	const TSharedRef<const FVoxelValleyTile> Cached = A.GetTile(CornerTile);
	const TSharedRef<FVoxelValleyTile> Fresh = FVoxelValleyNetwork::BuildTile(A.GetSettings(), A.GetGround(), CornerTile);
	bool bSame = Cached->Nodes.Num() == Fresh->Nodes.Num() && Cached->Segments.Num() == Fresh->Segments.Num() && Cached->Nodes.Num() > 0;
	for (int32 K = 0; bSame && K < Cached->Nodes.Num(); ++K)
	{
		bSame = SameNode(Cached->Nodes[K], Fresh->Nodes[K]);
	}
	TestTrue(TEXT("a cached tile is the one built from nothing"), bSame);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyPlanetRulesTest,
	"VoxelWorlds.Generation.Valleys.Planet.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyPlanetRulesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyPlanetTest;
	const FVoxelValleyNetwork Network(MakeSettings(8), MakeGround());
	const FVoxelValleySettings& S = Network.GetSettings();
	int32 NotNeighbour = 0, Rises = 0, FloorRises = 0, Coasts = 0, BadCoasts = 0, Endless = 0, Mouths = 0, Breaches = 0;
	for (const FIntPoint& Cell : AllCells(Network))
	{
		const FVoxelValleyNode Node = Network.GetNode(Cell);
		if (Node.bSea)
		{
			++Coasts;
			BadCoasts += (Node.bHasLink || FMath::Abs(Node.Floor - (-static_cast<double>(S.MinFloorHeight))) > 1.0e-6) ? 1 : 0;
			continue;
		}
		if (!Node.bHasLink)
		{
			continue;
		}
		bool bNeighbour = false;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			FIntPoint Next;
			bNeighbour |= Network.GetNeighbour(Cell, Index, Next) && Next == Node.Down;
		}
		NotNeighbour += bNeighbour ? 0 : 1;
		const FVoxelValleyNode Down = Network.GetNode(Node.Down);
		Breaches += Node.bBreach ? 1 : 0;
		Rises += (!Node.bBreach && Down.Height >= Node.Height) ? 1 : 0;
		FloorRises += Down.Floor > Node.Floor + 1.0e-6 ? 1 : 0;
		Mouths += (Node.bValley && Down.bSea) ? 1 : 0;
		FVoxelValleyNode At = Node;
		int32 Guard = 0;
		for (; At.bHasLink && Guard < 10000; ++Guard)
		{
			At = Network.GetNode(At.Down);
		}
		Endless += Guard >= 10000 ? 1 : 0;
	}
	int32 Segments = 0, SegmentRises = 0, OffSphere = 0;
	for (const TPair<FIntPoint, TArray<FVoxelValleySegment>>& Pair : SegmentsByLink(Network))
	{
		for (const FVoxelValleySegment& Segment : Pair.Value)
		{
			++Segments;
			SegmentRises += Segment.FloorB > Segment.FloorA + 1.0e-6 ? 1 : 0;
			OffSphere += (FMath::Abs(Segment.A.Size() - PlanetRadius) > 1.0e-6 * PlanetRadius || FMath::Abs(Segment.B.Size() - PlanetRadius) > 1.0e-6 * PlanetRadius) ? 1 : 0;
		}
	}
	AddInfo(FString::Printf(TEXT("%d coasts, %d breach steps, %d valley mouths, %d segments"), Coasts, Breaches, Mouths, Segments));
	TestTrue(TEXT("the planet has coasts, mouths and segments"), Coasts > 0 && Mouths > 0 && Segments > 100);
	TestEqual(TEXT("every link steps to a neighbour on the sphere"), NotNeighbour, 0);
	TestEqual(TEXT("every link but a breach's falls"), Rises, 0);
	TestEqual(TEXT("no floor rises along a link"), FloorRises, 0);
	TestEqual(TEXT("no floor rises along a segment"), SegmentRises, 0);
	TestEqual(TEXT("a coast's floor lies MinFloorHeight under the sea and it has no link"), BadCoasts, 0);
	TestEqual(TEXT("every chain ends"), Endless, 0);
	TestEqual(TEXT("every point of a curve lies on the datum sphere"), OffSphere, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyPlanetCarveTest,
	"VoxelWorlds.Generation.Valleys.Planet.Carve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyPlanetCarveTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyPlanetTest;
	const TSharedRef<FVoxelValleyNetwork> Network = MakeShared<FVoxelValleyNetwork>(MakeSettings(8), MakeGround());
	const TMap<FIntPoint, TArray<FVoxelValleySegment>> ByLink = SegmentsByLink(*Network);

	int32 Samples = 0, Floors = 0, FloorsHeld = 0;
	double WorstDifference = 0.0;
	int32 Links = 0;
	for (const TPair<FIntPoint, TArray<FVoxelValleySegment>>& Pair : ByLink)
	{
		if (++Links % 3 != 0)
		{
			continue;
		}
		const FVoxelValleySegment& Segment = Pair.Value[Pair.Value.Num() / 2];
		const FVector Middle = (0.5 * (Segment.A + Segment.B)).GetSafeNormal();
		const FVector Along = (Segment.B - Segment.A).GetSafeNormal();
		const FVector Across = FVector::CrossProduct(Middle, Along).GetSafeNormal();
		// A chunk's list around the middle, as the generators get it (a cap of 30 m around the anchor)
		FVoxelChunkValleySegments List;
		FVoxelChunkValleySegments::BuildOnSphere(*Network, Middle, PlanetRadius, 3000.0, List);
		for (int32 K = -4; K <= 4; ++K)
		{
			// Across the floor and the side, within the chunk's cap
			const FVector Direction = (Middle * PlanetRadius + Across * (K * 600.0)).GetSafeNormal();
			const double Natural = Hills(Direction);
			const double FromQuery = Network->ApplyToAltitude(Direction, Natural);
			const FVector3f Offset((Direction - Middle) * PlanetRadius);
			const double FromList = List.Apply(Offset, static_cast<float>(Natural));
			WorstDifference = FMath::Max(WorstDifference, FMath::Abs(FromQuery - FromList));
			++Samples;
			if (K == 0 && Natural > 0.0)
			{
				const double Floor = 0.5 * (Segment.FloorA + Segment.FloorB);
				++Floors;
				FloorsHeld += FromQuery <= FMath::Max(Floor, Natural) + 1.0 ? 1 : 0;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d samples, the point query and the chunk's list %.4f uu apart at most; %d of %d floor middles at their floor or lower"), Samples, WorstDifference, FloorsHeld, Floors));
	TestTrue(TEXT("the network has valleys to sample"), Samples > 100);
	TestTrue(TEXT("the point query and the chunk's list agree"), WorstDifference < 0.05);
	TestTrue(TEXT("a floor's middle holds its floor or lies deeper"), Floors > 0 && FloorsHeld == Floors);

	// Away from every valley and under the sea: the natural altitude; and no step anywhere along a line of samples
	int32 Away = 0, AwayChanged = 0, Under = 0, UnderChanged = 0;
	double WorstStep = 0.0;
	const FVector Start = FVector(0.3, 0.8, 0.5).GetSafeNormal();
	const FVector Tangent = FVector::CrossProduct(Start, FVector::UpVector).GetSafeNormal();
	double Previous = 0.0, PreviousNatural = 0.0;
	for (int32 K = 0; K < 20000; ++K)
	{
		// A line of samples 30 uu apart along a great circle, 6 km long
		const double Angle = K * 30.0 / PlanetRadius;
		const FVector Direction = Start * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle);
		const double Natural = Hills(Direction);
		const double Carved = Network->ApplyToAltitude(Direction, Natural);
		TArray<FVoxelValleySegment> Near;
		Network->GatherSegmentsNear(Direction * PlanetRadius, 0.0, Near);
		if (Natural < 0.0)
		{
			++Under;
			UnderChanged += Carved != Natural ? 1 : 0;
		}
		else if (Near.Num() == 0)
		{
			++Away;
			AwayChanged += Carved != Natural ? 1 : 0;
		}
		if (K > 0 && Natural >= 0.0 && PreviousNatural >= 0.0)
		{
			WorstStep = FMath::Max(WorstStep, FMath::Abs((Carved - Natural) - (Previous - PreviousNatural)));
		}
		Previous = Carved;
		PreviousNatural = Natural;
	}
	AddInfo(FString::Printf(TEXT("along 6 km: %d samples away from every valley, %d under the sea; the carve's largest change over 30 uu %.1f uu"), Away, Under, WorstStep));
	TestTrue(TEXT("the line has samples away from the valleys"), Away > 100);
	TestEqual(TEXT("away from every valley the altitude is the natural one"), AwayChanged, 0);
	TestEqual(TEXT("under the sea the altitude is the natural one"), UnderChanged, 0);
	TestTrue(TEXT("the carve has no step"), WorstStep < 60.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyRiverPlanetTest,
	"VoxelWorlds.Generation.Hydrology.ValleyRivers.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyRiverPlanetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyPlanetTest;
	const TSharedPtr<FVoxelValleyNetwork> Valleys = MakeShared<FVoxelValleyNetwork>(MakeSettings(8), MakeGround());
	const FVoxelHydrologyGround Ground = MakeHydrologyGround(Valleys);
	const FVoxelHydrologySettings Settings;
	const double Tuck = FVoxelConditioningLineShape::FRiverProfileParams().BodyTuck;
	const double Bank = Settings.BankWidthVoxels * Ground.VoxelSize;

	TMap<FIntPoint, TSharedRef<const FVoxelHydrologySystem>> Made;
	const auto PieceOf = [&](const FIntPoint& Link) -> TSharedRef<const FVoxelHydrologySystem>
	{
		if (const TSharedRef<const FVoxelHydrologySystem>* Found = Made.Find(Link))
		{
			return *Found;
		}
		return Made.Add(Link, FVoxelHydrology::BuildValleyRiver(Settings, Ground, Link));
	};

	int32 Pieces = 0, AtSea = 0, Rises = 0, Notes = 0, Heads = 0, BadHeads = 0, Mouths = 0, BadMouths = 0, Joints = 0, JointGaps = 0, JointRises = 0;
	int32 NotDirections = 0, Across = 0, OtherRefusals = 0;
	double WorstGap = 0.0;
	for (const FIntPoint& Cell : AllCells(*Valleys))
	{
		const FVoxelValleyNode Node = Valleys->GetNode(Cell);
		if (!Node.bValley)
		{
			continue;
		}
		const TSharedRef<const FVoxelHydrologySystem> Piece = PieceOf(Cell);
		if (Piece->Refused.Contains(TEXT("starts in the sea")))
		{
			++AtSea;
			continue;
		}
		if (Piece->Reaches.Num() != 1)
		{
			if (++OtherRefusals <= 3)
			{
				AddError(FString::Printf(TEXT("the valley link (%d, %d) has no river: %s"), Cell.X, Cell.Y, *Piece->Refused));
			}
			continue;
		}
		++Pieces;
		if (!Piece->ConflictNote.IsEmpty() && ++Notes <= 3)
		{
			AddInfo(Piece->ConflictNote);
		}
		const FVoxelHydrologyReach& Reach = Piece->Reaches[0];
		const TArray<FVoxelWaterReachVertex>& Vertices = Reach.Body.Reach->GetVertices();
		for (int32 K = 0; K < Vertices.Num(); ++K)
		{
			NotDirections += FMath::Abs(Vertices[K].Position.Size() - 1.0) > 1.0e-6 ? 1 : 0;
			Rises += (K > 0 && Vertices[K].SurfaceLevel > Vertices[K - 1].SurfaceLevel + 1.0e-6) ? 1 : 0;
		}
		Across += FaceOf(*Valleys, Cell) != FaceOf(*Valleys, Node.Down) ? 1 : 0;
		if (Node.bHead)
		{
			++Heads;
			BadHeads += (Reach.Profile.bStartsInBody || Vertices[0].HalfWidth > Bank + 1.0) ? 1 : 0;
		}
		const FVoxelValleyNode Down = Valleys->GetNode(Node.Down);
		if (Down.bSea)
		{
			++Mouths;
			// A level on a planet is the altitude plus the datum radius
			BadMouths += (Reach.bEndsAtJoin || FMath::Abs(Vertices.Last().SurfaceLevel - (PlanetRadius + Ground.SeaLevel - Tuck)) > 1.0e-3) ? 1 : 0;
		}
		else if (Down.bValley)
		{
			const TSharedRef<const FVoxelHydrologySystem> Next = PieceOf(Down.Cell);
			if (Next->Reaches.Num() == 1)
			{
				++Joints;
				const FVoxelWaterReachVertex& First = Next->Reaches[0].Body.Reach->GetVertices()[0];
				const double Gap = FVector::Dist(First.Position * PlanetRadius, Vertices.Last().Position * PlanetRadius);
				WorstGap = FMath::Max(WorstGap, Gap);
				JointGaps += Gap > 1.0 ? 1 : 0;
				JointRises += First.SurfaceLevel > Vertices.Last().SurfaceLevel + 1.0e-3 ? 1 : 0;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d pieces (%d more would start in the sea), %d heads, %d into a coast, %d joints (the widest gap %.4f uu), %d across a face's edge, %d with a bed note"),
		Pieces, AtSea, Heads, Mouths, Joints, WorstGap, Across, Notes));
	TestTrue(TEXT("the planet has valley rivers, heads, mouths and joints"), Pieces > 20 && Heads > 0 && Mouths > 0 && Joints > 0);
	TestEqual(TEXT("every valley link but those that start in the sea has its river"), OtherRefusals, 0);
	TestEqual(TEXT("a planet piece's vertices are directions"), NotDirections, 0);
	TestEqual(TEXT("no surface rises along a piece"), Rises, 0);
	TestEqual(TEXT("a piece's bed keeps a line's limits on a planet"), Notes, 0);
	TestEqual(TEXT("a piece meets the piece below at the node"), JointGaps, 0);
	TestEqual(TEXT("... never under it"), JointRises, 0);
	TestEqual(TEXT("a head starts from nothing"), BadHeads, 0);
	TestEqual(TEXT("a piece into a coast ends a tuck under the sea"), BadMouths, 0);

	// Two caps that overlap: the pieces they share are the same, from a cache or not
	const FVector CapA = FVector(1.0, 0.95, 0.3).GetSafeNormal(), CapB = FVector(0.95, 1.0, 0.3).GetSafeNormal();
	const double Angle = 3.0 * FVoxelPolylineBounds::AngleBetween(CapA, CapB);
	TArray<TSharedRef<const FVoxelHydrologySystem>> InA, InB;
	FCache Cache;
	FVoxelHydrology::GatherValleyRivers(Settings, Ground, FVoxelConditioningRegion::OfCap(CapA, Angle), &Cache, InA);
	FVoxelHydrology::GatherValleyRivers(Settings, Ground, FVoxelConditioningRegion::OfCap(CapB, Angle), nullptr, InB);
	int32 Shared = 0, Different = 0;
	for (const TSharedRef<const FVoxelHydrologySystem>& RA : InA)
	{
		for (const TSharedRef<const FVoxelHydrologySystem>& RB : InB)
		{
			if (RA->Mouth != RB->Mouth)
			{
				continue;
			}
			++Shared;
			const TArray<FVoxelWaterReachVertex>& VA = RA->Reaches[0].Body.Reach->GetVertices();
			const TArray<FVoxelWaterReachVertex>& VB = RB->Reaches[0].Body.Reach->GetVertices();
			bool bSame = RA->Reaches[0].Body.Id == RB->Reaches[0].Body.Id && VA.Num() == VB.Num();
			for (int32 K = 0; bSame && K < VA.Num(); ++K)
			{
				bSame = VA[K].Position == VB[K].Position && VA[K].SurfaceLevel == VB[K].SurfaceLevel && VA[K].BedLevel == VB[K].BedLevel;
			}
			Different += bSame ? 0 : 1;
		}
	}
	AddInfo(FString::Printf(TEXT("two caps: %d and %d pieces, %d shared"), InA.Num(), InB.Num(), Shared));
	TestTrue(TEXT("the caps share pieces"), Shared > 0);
	TestEqual(TEXT("the pieces two caps share are the same"), Different, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyRiverBlocksTest,
	"VoxelWorlds.Generation.Hydrology.ValleyRivers.Blocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyRiverBlocksTest::RunTest(const FString& Parameters)
{
	// V3's made hills falling to a sea, as a block world
	const auto HillsXY = [](double X, double Y)
	{
		return 6000.0 - 0.01 * FMath::Abs(X) + 3000.0 * FMath::Sin(X / 80000.0) * FMath::Cos(Y / 65000.0) + 1500.0 * FMath::Sin((X - Y) / 31000.0);
	};
	FVoxelValleyGround ValleyGround;
	ValleyGround.Height = HillsXY;
	ValleyGround.bSea = true;
	ValleyGround.SeaLevel = 0.0;
	ValleyGround.Seed = 1234;
	const TSharedPtr<FVoxelValleyNetwork> Valleys = MakeShared<FVoxelValleyNetwork>(FVoxelValleySettings(), ValleyGround);
	FVoxelHydrologyGround Smooth;
	Smooth.Height = [Valleys, HillsXY](double X, double Y) { return Valleys->ApplyToHeight(X, Y, HillsXY(X, Y)); };
	Smooth.bSea = true;
	Smooth.SeaLevel = 0.0;
	Smooth.VoxelSize = 75.0;
	Smooth.ChunkWorldSize = 2400.0;
	Smooth.Seed = 1234;
	Smooth.Valleys = Valleys;
	FVoxelHydrologyGround Blocks = Smooth;
	Blocks.bBlocks = true;
	const FVoxelHydrologySettings Settings;
	const double Grade = FVoxelHydrology::GetBlockBedGrade();

	int32 Pieces = 0, Steep = 0, Notes = 0, Lowered = 0, Different = 0;
	for (int32 TY = -2; TY < 2; ++TY)
	{
		for (int32 TX = -2; TX < 2; ++TX)
		{
			for (const FVoxelValleyNode& Node : Valleys->GetTile(FIntPoint(TX, TY))->Nodes)
			{
				if (!Node.bValley)
				{
					continue;
				}
				const TSharedRef<const FVoxelHydrologySystem> OnBlocks = FVoxelHydrology::BuildValleyRiver(Settings, Blocks, Node.Cell);
				const TSharedRef<const FVoxelHydrologySystem> OnSmooth = FVoxelHydrology::BuildValleyRiver(Settings, Smooth, Node.Cell);
				if (OnBlocks->Reaches.Num() != 1 || OnSmooth->Reaches.Num() != 1)
				{
					Different += OnBlocks->Reaches.Num() != OnSmooth->Reaches.Num() ? 1 : 0;
					continue;
				}
				++Pieces;
				if (!OnBlocks->ConflictNote.IsEmpty() && ++Notes <= 3)
				{
					AddInfo(OnBlocks->ConflictNote);
				}
				const TArray<FVoxelWaterReachVertex>& V = OnBlocks->Reaches[0].Body.Reach->GetVertices();
				const TArray<FVoxelWaterReachVertex>& W = OnSmooth->Reaches[0].Body.Reach->GetVertices();
				if (V.Num() != W.Num())
				{
					++Different;
					continue;
				}
				for (int32 K = 0; K < V.Num(); ++K)
				{
					// The course and the surface are the smooth world's; the bed is no higher, lowered where it was steep
					Different += (V[K].Position != W[K].Position || V[K].SurfaceLevel != W[K].SurfaceLevel || V[K].BedLevel > W[K].BedLevel + 1.0e-9) ? 1 : 0;
					Lowered += V[K].BedLevel < W[K].BedLevel - 1.0e-6 ? 1 : 0;
					if (K > 0)
					{
						const double Run = FVector::Dist(V[K].Position, V[K - 1].Position);
						Steep += FMath::Abs(V[K].BedLevel - V[K - 1].BedLevel) > Grade * Run + 1.0e-6 ? 1 : 0;
					}
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d pieces on blocks, %d bed vertices lowered for the block bound"), Pieces, Lowered));
	TestTrue(TEXT("the block world has valley rivers"), Pieces > 20);
	TestEqual(TEXT("no piece's bed steps more than a block a column"), Steep, 0);
	TestEqual(TEXT("the engine's check of a line on a block world finds nothing"), Notes, 0);
	TestEqual(TEXT("a block world's pieces are the smooth world's but for a bed lowered"), Different, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
