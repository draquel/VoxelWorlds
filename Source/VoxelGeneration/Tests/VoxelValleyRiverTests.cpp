// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelHydrology.h"
#include "VoxelRiverCourse.h"
#include "VoxelValleyNetwork.h"
#include "VoxelConditioningLineShape.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The valley rivers (Documentation/VALLEYS_PLAN.md, section 6.4; phase V3), on made hills falling to a sea with the
// valley network carved into them.
//
// - Generation.Hydrology.ValleyRivers.Pieces: a piece a valley link: its surface never rises; it meets the piece below
//   at the node, at the same place, never under it; a head starts from nothing; a piece into a coast ends a tuck under
//   the sea; its course keeps to its valley's floor; and from every head the pieces lead to the sea.
// - Generation.Hydrology.ValleyRivers.Regions: two regions that overlap, and a run with a cache, give every valley river
//   the same id and the same vertices.
// - Generation.Hydrology.ValleyRivers.Brooks: descents end in a valley river (EVoxelHydrologyEnd::ValleyRiver), and each
//   such river system's last reach ends in the valley river's water, at its level less at most the tuck.
// ---------------------------------------------------------------------------

namespace VoxelValleyRiverTest
{
	double Hills(double X, double Y)
	{
		return 6000.0 - 0.01 * FMath::Abs(X) + 3000.0 * FMath::Sin(X / 80000.0) * FMath::Cos(Y / 65000.0) + 1500.0 * FMath::Sin((X - Y) / 31000.0);
	}

	struct FWorld
	{
		TSharedPtr<FVoxelValleyNetwork> Valleys;
		FVoxelHydrologyGround Ground;
		FVoxelHydrologySettings Settings;
	};

	FWorld MakeWorld()
	{
		FWorld World;
		FVoxelValleyGround ValleyGround;
		ValleyGround.Height = &Hills;
		ValleyGround.bSea = true;
		ValleyGround.SeaLevel = 0.0;
		ValleyGround.Seed = 1234;
		World.Valleys = MakeShared<FVoxelValleyNetwork>(FVoxelValleySettings(), ValleyGround);
		const TSharedPtr<FVoxelValleyNetwork> Valleys = World.Valleys;
		// The ground the hydrology reads: the hills with the valleys carved, as a world mode's natural surface is
		World.Ground.Height = [Valleys](double X, double Y) { return Valleys->ApplyToHeight(X, Y, Hills(X, Y)); };
		World.Ground.bSea = true;
		World.Ground.SeaLevel = 0.0;
		World.Ground.VoxelSize = 75.0;
		World.Ground.ChunkWorldSize = 2400.0;
		World.Ground.Seed = 1234;
		World.Ground.Valleys = World.Valleys;
		return World;
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

	bool SameReach(const FVoxelHydrologyReach& A, const FVoxelHydrologyReach& B)
	{
		const TArray<FVoxelWaterReachVertex>& VA = A.Body.Reach->GetVertices();
		const TArray<FVoxelWaterReachVertex>& VB = B.Body.Reach->GetVertices();
		bool bSame = A.Body.Id == B.Body.Id && VA.Num() == VB.Num() && A.Bed.IsValid() == B.Bed.IsValid();
		for (int32 K = 0; bSame && K < VA.Num(); ++K)
		{
			bSame = VA[K].Position == VB[K].Position && VA[K].SurfaceLevel == VB[K].SurfaceLevel && VA[K].BedLevel == VB[K].BedLevel && VA[K].HalfWidth == VB[K].HalfWidth;
		}
		return bSame;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyRiverPiecesTest,
	"VoxelWorlds.Generation.Hydrology.ValleyRivers.Pieces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyRiverPiecesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyRiverTest;
	const FWorld World = MakeWorld();
	const FVoxelValleyNetwork& Valleys = *World.Valleys;
	const double Bank = World.Settings.BankWidthVoxels * World.Ground.VoxelSize;
	const double Tuck = FVoxelConditioningLineShape::FRiverProfileParams().BodyTuck;
	FCache Cache;

	int32 Pieces = 0, Rises = 0, Joints = 0, JointGaps = 0, JointRises = 0, Heads = 0, BadHeads = 0, Mouths = 0, BadMouths = 0, OffFloor = 0, Notes = 0;
	TMap<FIntPoint, TSharedRef<const FVoxelHydrologySystem>> ByLink;
	auto PieceOf = [&](const FIntPoint& Link) -> TSharedRef<const FVoxelHydrologySystem>
	{
		if (const TSharedRef<const FVoxelHydrologySystem>* Found = ByLink.Find(Link))
		{
			return *Found;
		}
		TArray<TSharedRef<const FVoxelHydrologySystem>> One;
		const FVoxelValleyNode Node = Valleys.GetNode(Link);
		FVoxelHydrology::GatherValleyRivers(World.Settings, World.Ground, FBox2D(Node.Position, Node.Position), &Cache, One);
		TSharedPtr<const FVoxelHydrologySystem> Piece = Cache.FindValleyRiver(Link);
		const TSharedRef<const FVoxelHydrologySystem> Made = Piece.IsValid() ? Piece.ToSharedRef() : FVoxelHydrology::BuildValleyRiver(World.Settings, World.Ground, Link);
		return ByLink.Add(Link, Made);
	};
	for (int32 TY = -2; TY < 2; ++TY)
	{
		for (int32 TX = -2; TX < 2; ++TX)
		{
			for (const FVoxelValleyNode& Node : Valleys.GetTile(FIntPoint(TX, TY))->Nodes)
			{
				if (!Node.bValley)
				{
					continue;
				}
				const TSharedRef<const FVoxelHydrologySystem> Piece = PieceOf(Node.Cell);
				if (!TestTrue(FString::Printf(TEXT("the valley link (%d, %d) has its river"), Node.Cell.X, Node.Cell.Y), Piece->bValleyRiver && Piece->Reaches.Num() == 1))
				{
					continue;
				}
				++Pieces;
				if (!Piece->ConflictNote.IsEmpty())
				{
					if (Notes < 4)
					{
						AddInfo(Piece->ConflictNote);
						const TArray<FVoxelWaterReachVertex>& V = Piece->Reaches[0].Body.Reach->GetVertices();
						TArray<FVector> P;
						for (const FVoxelWaterReachVertex& X : V) { P.Add(FVector(X.Position.X, X.Position.Y, 0.0)); }
						FString Where;
						for (int32 K = 1; K + 1 < P.Num(); ++K)
						{
							const double Turn = FVoxelConditioningLineShape::GetTurnAt(P, K);
							if (Turn > 0.05) { Where += FString::Printf(TEXT(" %d:%.0f(%.0f)"), K, FMath::RadiansToDegrees(Turn), FVector::Dist(P[K], P[K - 1])); }
						}
						AddInfo(FString::Printf(TEXT("  %d vertices, head %d, turns:%s"), P.Num(), Node.bHead ? 1 : 0, *Where));
					}
					++Notes;
				}
				const FVoxelHydrologyReach& Reach = Piece->Reaches[0];
				const TArray<FVoxelWaterReachVertex>& Vertices = Reach.Body.Reach->GetVertices();
				for (int32 K = 1; K < Vertices.Num(); ++K)
				{
					Rises += Vertices[K].SurfaceLevel > Vertices[K - 1].SurfaceLevel + 1.0e-6 ? 1 : 0;
				}
				// Its course keeps to its valley's floor
				TArray<FVoxelValleySegment> Near;
				for (int32 K = 0; K < Vertices.Num(); K += 7)
				{
					Near.Reset();
					Valleys.GatherSegments(FBox2D(FVector2D(Vertices[K].Position), FVector2D(Vertices[K].Position)), Near);
					double Best = MAX_dbl;
					double Floor = 0.0;
					for (const FVoxelValleySegment& Segment : Near)
					{
						if (Segment.Link != Node.Cell)
						{
							continue;
						}
						const FVector2D P(Vertices[K].Position);
						const FVector2D AB = Segment.B - Segment.A;
						const double T = FMath::Clamp(((P - Segment.A) | AB) / FMath::Max(AB.SizeSquared(), 1.0e-6), 0.0, 1.0);
						const double D = FVector2D::Distance(P, Segment.A + AB * T);
						if (D < Best)
						{
							Best = D;
							Floor = Segment.FloorHalfWidth;
						}
					}
					OffFloor += Best > Floor + 1.0 ? 1 : 0;
				}
				if (Node.bHead)
				{
					++Heads;
					BadHeads += (Reach.Profile.bStartsInBody || Vertices[0].HalfWidth > Bank + 1.0) ? 1 : 0;
				}
				const FVoxelValleyNode Down = Valleys.GetNode(Node.Down);
				if (Down.bSea)
				{
					++Mouths;
					BadMouths += (Reach.bEndsAtJoin || FMath::Abs(Vertices.Last().SurfaceLevel - (World.Ground.SeaLevel - Tuck)) > 1.0e-3) ? 1 : 0;
				}
				else if (Down.bValley)
				{
					// It meets the piece below at the node, never under it
					const TSharedRef<const FVoxelHydrologySystem> Next = PieceOf(Down.Cell);
					if (Next->Reaches.Num() == 1)
					{
						++Joints;
						const FVoxelWaterReachVertex& First = Next->Reaches[0].Body.Reach->GetVertices()[0];
						JointGaps += FVector::Dist2D(First.Position, Vertices.Last().Position) > 1.0 ? 1 : 0;
						JointRises += First.SurfaceLevel > Vertices.Last().SurfaceLevel + 1.0e-3 ? 1 : 0;
					}
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d pieces, %d heads, %d into a coast, %d joints, %d with a bed note"), Pieces, Heads, Mouths, Joints, Notes));
	TestTrue(TEXT("the hills have valley rivers, heads, mouths and joints"), Pieces > 20 && Heads > 0 && Mouths > 0 && Joints > 0);
	TestEqual(TEXT("no surface rises along a piece"), Rises, 0);
	TestEqual(TEXT("a piece's bed keeps a line's limits"), Notes, 0);
	TestEqual(TEXT("a piece meets the piece below at the node"), JointGaps, 0);
	TestEqual(TEXT("... never under it"), JointRises, 0);
	TestEqual(TEXT("a head starts from nothing"), BadHeads, 0);
	TestEqual(TEXT("a piece into a coast ends a tuck under the sea"), BadMouths, 0);
	TestEqual(TEXT("a river's course keeps to its valley's floor"), OffFloor, 0);

	// From every head, the pieces lead to the sea
	int32 Chains = 0, ToSea = 0;
	for (int32 TY = -2; TY < 2; ++TY)
	{
		for (int32 TX = -2; TX < 2; ++TX)
		{
			for (const FVoxelValleyNode& Head : Valleys.GetTile(FIntPoint(TX, TY))->Nodes)
			{
				if (!Head.bHead)
				{
					continue;
				}
				++Chains;
				FVoxelValleyNode Node = Head;
				for (int32 Guard = 0; Node.bValley && Guard < 1000; ++Guard)
				{
					Node = Valleys.GetNode(Node.Down);
				}
				ToSea += Node.bSea ? 1 : 0;
			}
		}
	}
	TestTrue(TEXT("valley rivers start at heads"), Chains > 0);
	TestEqual(TEXT("every valley river leads to the sea"), ToSea, Chains);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyRiverRegionsTest,
	"VoxelWorlds.Generation.Hydrology.ValleyRivers.Regions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyRiverRegionsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyRiverTest;
	const FWorld World = MakeWorld();
	FVoxelHydrologyRegion A, B, Cached;
	const FBox2D BoxA(FVector2D(-400000.0, -200000.0), FVector2D(100000.0, 200000.0));
	const FBox2D BoxB(FVector2D(-100000.0, -200000.0), FVector2D(400000.0, 200000.0));
	FVoxelHydrology::BuildRegion(World.Settings, World.Ground, BoxA, nullptr, A);
	FVoxelHydrology::BuildRegion(World.Settings, World.Ground, BoxB, nullptr, B);
	FCache Cache;
	FVoxelHydrology::BuildRegion(World.Settings, World.Ground, BoxA, &Cache, Cached);

	TMap<FIntPoint, const FVoxelHydrologyReach*> InA;
	for (const TSharedRef<const FVoxelHydrologySystem>& System : A.Systems)
	{
		if (System->bValleyRiver)
		{
			InA.Add(System->Mouth, &System->Reaches[0]);
		}
	}
	int32 Shared = 0, Differ = 0;
	for (const TSharedRef<const FVoxelHydrologySystem>& System : B.Systems)
	{
		if (const FVoxelHydrologyReach* const* Other = System->bValleyRiver ? InA.Find(System->Mouth) : nullptr)
		{
			++Shared;
			Differ += SameReach(**Other, System->Reaches[0]) ? 0 : 1;
		}
	}
	int32 CachedDiffer = 0, CachedValley = 0;
	for (const TSharedRef<const FVoxelHydrologySystem>& System : Cached.Systems)
	{
		if (const FVoxelHydrologyReach* const* Other = System->bValleyRiver ? InA.Find(System->Mouth) : nullptr)
		{
			++CachedValley;
			CachedDiffer += SameReach(**Other, System->Reaches[0]) ? 0 : 1;
		}
	}
	AddInfo(FString::Printf(TEXT("%d valley rivers in the first region, %d shared with the second"), InA.Num(), Shared));
	TestTrue(TEXT("the regions share valley rivers"), Shared > 5);
	TestEqual(TEXT("two regions give each valley river the same id and vertices"), Differ, 0);
	TestEqual(TEXT("a run with a cache gives the same valley rivers"), CachedValley, InA.Num());
	TestEqual(TEXT("... the same records"), CachedDiffer, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyRiverBrooksTest,
	"VoxelWorlds.Generation.Hydrology.ValleyRivers.Brooks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyRiverBrooksTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyRiverTest;
	const FWorld World = MakeWorld();
	const double Tuck = FVoxelConditioningLineShape::FRiverProfileParams().BodyTuck;
	FCache Cache;
	int32 ToValley = 0, Ends = 0, OnLand = 0, OverBody = 0, Brooks = 0, ValleyEnds = 0;
	FString FirstFault;
	// A region's reaches' ends against the survey's rule: each lies in the water (the sea, a lake, another reach) at its
	// level, less at most the tuck. A valley river's piece into a coast is read back as a brook is
	const auto CheckRegion = [&](const FBox2D& Box)
	{
		FVoxelHydrologyRegion Region;
		FVoxelHydrology::BuildRegion(World.Settings, World.Ground, Box, &Cache, Region);
		TArray<FVoxelWaterBody> Bodies;
		for (const TSharedRef<const FVoxelHydrologyLake>& Lake : Region.Lakes)
		{
			Bodies.Add(Lake->Body);
		}
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				Bodies.Add(Reach.Body);
			}
		}
		for (const TSharedRef<const FVoxelHydrologySystem>& System : Region.Systems)
		{
			Brooks += System->bValleyRiver ? 0 : 1;
			for (const FVoxelHydrologyReach& Reach : System->Reaches)
			{
				const FVoxelWaterReachVertex& Last = Reach.Body.Reach->GetVertices().Last();
				if (Reach.bEndsAtJoin || !Box.IsInside(FVector2D(Last.Position)))
				{
					continue;
				}
				TArray<FVoxelWaterBody> Others = Bodies.FilterByPredicate([&Reach](const FVoxelWaterBody& Body) { return Body.Id != Reach.Body.Id; });
				const FVoxelWaterLevels Water = FVoxelWaterLevels::Make(World.Ground.GetSea(), World.Ground.GetFrame(), FVoxelWaterLevels::WholeWorld(), MoveTemp(Others));
				const double GroundThere = World.Ground.HeightAt(Last.Position);
				const double Level = FVoxelRiverCourse::GetBodyLevelAt(Water, Last.Position, GroundThere);
				++Ends;
				ValleyEnds += System->bValleyRiver ? 1 : 0;
				const bool bOnLand = FMath::IsNaN(Level);
				const bool bOver = !bOnLand && (Level < Last.SurfaceLevel - 1.0e-3 || Level > Last.SurfaceLevel + Tuck + 1.0e-3);
				OnLand += bOnLand ? 1 : 0;
				OverBody += bOver ? 1 : 0;
				if ((bOnLand || bOver) && FirstFault.IsEmpty())
				{
					FirstFault = FString::Printf(TEXT("a %s ends at (%.0f, %.0f) with its surface at %.1f, the ground %.1f, the water %.1f"),
						System->bValleyRiver ? TEXT("valley river") : TEXT("brook"), Last.Position.X, Last.Position.Y, Last.SurfaceLevel, GroundThere, Level);
				}
			}
		}
	};
	CheckRegion(FBox2D(FVector2D(-200000.0, -150000.0), FVector2D(200000.0, 150000.0)));
	// And round valley mouths, where a valley comes down to the sea
	int32 Mouths = 0;
	for (int32 TY = -2; TY < 2 && Mouths < 3; ++TY)
	{
		for (int32 TX = -2; TX < 2 && Mouths < 3; ++TX)
		{
			for (const FVoxelValleyNode& Node : World.Valleys->GetTile(FIntPoint(TX, TY))->Nodes)
			{
				if (Mouths < 3 && Node.bValley && World.Valleys->GetNode(Node.Down).bSea)
				{
					const FVector2D At = World.Valleys->GetNode(Node.Down).Position;
					CheckRegion(FBox2D(At - FVector2D(60000.0), At + FVector2D(60000.0)));
					++Mouths;
				}
			}
		}
	}
	for (const TPair<FIntPoint, TSharedRef<const FVoxelHydrologyTrace>>& Entry : Cache.Traces)
	{
		ToValley += (Entry.Value->bKept && Entry.Value->End == EVoxelHydrologyEnd::ValleyRiver) ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("%d descents end in a valley river; %d brook systems, %d reach ends checked (%d of valley rivers, round %d mouths)"), ToValley, Brooks, Ends, ValleyEnds, Mouths));
	if (!FirstFault.IsEmpty())
	{
		AddInfo(FirstFault);
	}
	TestTrue(TEXT("descents end in a valley river"), ToValley > 0);
	TestTrue(TEXT("valley rivers end in the sea round the mouths"), ValleyEnds > 0);
	TestEqual(TEXT("no brook or valley river ends on land"), OnLand, 0);
	TestEqual(TEXT("every brook and valley river ends in the water at its level, less at most the tuck"), OverBody, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
