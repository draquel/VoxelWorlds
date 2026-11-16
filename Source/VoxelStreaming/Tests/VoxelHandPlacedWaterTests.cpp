// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Testing/VoxelTestHandPlacedWater.h"
#include "Components/SplineComponent.h"
#include "VoxelChunkManager.h"
#include "VoxelConditioningLine.h"
#include "VoxelData.h"
#include "VoxelLakeComponent.h"
#include "VoxelRiverComponent.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The river and the lake placed by hand (the hydrology plan's H4, section 5.7; D144): UVoxelRiverComponent and
// UVoxelLakeComponent turn a spline on their actor into a river reach with its bed and into a lake, registered before
// the chunk manager's first generation request.
//
// - Streaming.HandPlacedWater.Plane, .Island, .Planet, .Cubic: a small world of each kind with a sea. A river's
//   course is found on its natural ground (steepest descent from high ground to the sea) and a hollow over the sea,
//   and a river actor and a lake actor are laid over them as a designer lays them: only where the splines run is
//   read. The river begins play first and the lake still registers before it; no generation request is built before
//   either. The river: a vertex of the reach a vertex of the bed, a surface that never rises and lies under the
//   natural ground at the footprint's edge on both sides, a head that starts from nothing, a mouth a tuck under the
//   sea and a few vertices into it. The lake: a level under the lowest ground along its outline, cells under it. Then
//   the world streams round each: the lake's and the river's voxels hold flagged water, and the world's sample there
//   is the lake's, and the river reach's with its flow downstream at the component's speed.
// - Streaming.HandPlacedWater.Rules: what the components say and do where a drawing is not a river or a lake (a plane):
//   a spline at its own height that climbs is lowered, with a warning; a river that stops on land thins to nothing,
//   with a warning; a river whose head lies in a lake starts a tuck under the lake's level at full width; a tributary
//   that ends in a river of a higher order ends in it, whichever began play first; a lake at its actor's height over
//   its outline's ground is made with the wall's warning, one with nothing under its level adds no body and says so;
//   a river whose actor began play before the voxel world registers when the world starts; two worlds make the same
//   river, vertex for vertex.
// ---------------------------------------------------------------------------

namespace VoxelHandPlacedWaterTest
{
	using namespace VoxelTesting;

	/** The values of the tests' river, in the small worlds' voxels of 50 uu. */
	void ConfigureRiver(UVoxelRiverComponent* River)
	{
		River->HalfWidth = 150.0f;
		River->BankWidth = 200.0f;
		River->Depth = 100.0f;
		River->SurfaceBelowGround = 50.0f;
		River->BankMargin = 25.0f;
		River->SmoothingDistance = 300.0f;
		River->HeadLength = 400.0f;
		River->FlowSpeed = 120.0f;
	}

	bool HasWarning(const UVoxelWaterBodyComponent* Body, const TCHAR* Text)
	{
		return Body->GetBodyWarnings().ContainsByPredicate([Text](const FString& Warning) { return Warning.Contains(Text); });
	}

	FString JoinWarnings(const UVoxelWaterBodyComponent* Body)
	{
		return FString::Join(Body->GetBodyWarnings(), TEXT(" | "));
	}

	/** The world point of a reach vertex at a water level. */
	FVector VertexAt(const FVoxelWaterFrame& Frame, const FVoxelWaterReachVertex& Vertex, double Level)
	{
		return Frame.bRadial ? Frame.Center + Vertex.Position * Level : FVector(Vertex.Position.X, Vertex.Position.Y, Level);
	}

	void RunWorld(FAutomationTestBase& Test, EHandPlacedWorld Kind)
	{
		const FString Name = GetHandPlacedWorldName(Kind);
		auto Say = [&Name](const TCHAR* What) { return FString::Printf(TEXT("%s: %s"), *Name, What); };
		// What a component says of a course found on noise (a corner rounded, a deep cut) is reported below, not judged.
		Test.AddExpectedMessagePlain(TEXT("Water body "), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		FTestChunkWorld W;
		W.Create(*FString::Printf(TEXT("HandPlacedWater%s"), *Name));
		ConfigureHandPlacedWorld(W, Kind);
		W.StartManager();
		UVoxelChunkManager* Manager = W.Manager;
		const FHandPlacedGround Ground(*Manager);
		const FVoxelWaterFrame Frame = Ground.Frame;
		const double LevelBase = Frame.bRadial ? Frame.DatumRadius : 0.0;
		const double Sea = Ground.SeaHeight();
		const FVector Site = Frame.bRadial ? Frame.Center + FVector(0.3, 0.5, 0.8).GetSafeNormal() * Frame.DatumRadius : FVector::ZeroVector;

		TArray<FVector> Course;
		FHandPlacedHollow Hollow;
		const bool bCourse = FindRiverCourse(Ground, Site, 6000.0, 1200.0, 5000.0, Course);
		const bool bHollow = FindHollow(Ground, Site, 6000.0, 150.0, Hollow);
		if (!Test.TestTrue(*Say(TEXT("the ground has a course down to the sea (the fixture)")), bCourse)
			|| !Test.TestTrue(*Say(TEXT("the ground has a hollow over the sea (the fixture)")), bHollow))
		{
			W.Stop();
			return;
		}
		Test.AddInfo(FString::Printf(TEXT("%s: the sea at %.0f; a course of %d places from %.0f; a hollow %.0f deep in a ring of %.0f, its pit at %.0f"),
			*Name, Sea, Course.Num(), Ground.HeightAt(Course[0]), Hollow.Depth, Hollow.Radius, Hollow.PitHeight));

		AVoxelRiverActor* RiverActor = SpawnRiverAlong(W.World, Ground, Course);
		UVoxelRiverComponent* River = RiverActor->GetRiver();
		ConfigureRiver(River);
		AVoxelLakeActor* LakeActor = SpawnLakeRound(W.World, Ground, Hollow.Pit, Hollow.Radius);
		UVoxelLakeComponent* Lake = LakeActor->GetLake();
		Lake->LevelMargin = 25.0f;

		// The river begins play first: the lake registers before it all the same.
		FinishHandPlaced(RiverActor);
		Test.TestTrue(*Say(TEXT("the river registers at BeginPlay")), River->IsBodyRegistered());
		Test.TestTrue(*Say(TEXT("... and has made the lake register before it")), Lake->IsBodyRegistered());
		FinishHandPlaced(LakeActor);
		const TArray<FVoxelWaterBody>& Bodies = Manager->GetStaticWaterBodies();
		if (!Test.TestEqual(*Say(TEXT("the world holds the two bodies")), Bodies.Num(), 2))
		{
			Test.AddInfo(FString::Printf(TEXT("%s: the river: %s; the lake: %s"), *Name, *JoinWarnings(River), *JoinWarnings(Lake)));
			W.Stop();
			return;
		}
		Test.TestTrue(*Say(TEXT("the lake first, then the river")), Bodies[0].Kind == EVoxelWaterBodyKind::Lake && Bodies[1].Kind == EVoxelWaterBodyKind::Reach);
		Test.TestTrue(*Say(TEXT("the bodies carry the components' ids")), Bodies[0].Id == Lake->GetWaterBodyId() && Bodies[1].Id == River->GetWaterBodyId()
			&& Bodies[0].Id != Bodies[1].Id && Bodies[0].Id != FVoxelWaterBody::SeaId);
		Test.TestEqual(*Say(TEXT("the river's bed is the world's one line")), Manager->GetNumConditioningLines(), 1);
		Test.TestEqual(*Say(TEXT("no generation request was built before them")), Manager->GetNumGenerationRequestsBuilt(), static_cast<int64>(0));

		// ---- The river.
		const FVoxelRiverBuild& Build = River->GetRiverBuild();
		const TSharedPtr<const FVoxelConditioningLine> Bed = River->GetRegisteredBed();
		const TArray<FVoxelWaterReachVertex>& Vertices = Bodies[1].Reach->GetVertices();
		Test.AddInfo(FString::Printf(TEXT("%s: the river: %d vertices of %d sampled, surface %.0f down to %.0f, the bed up to %.0f under the ground, %d lowered for a bank, %d beds deepened; warnings: %s"),
			*Name, Vertices.Num(), Build.NumSampled, Bodies[1].Reach->GetMaxLevel() - LevelBase, Bodies[1].Reach->GetMinLevel() - LevelBase,
			Build.Profile.DeepestCut, Build.Profile.NumLowBanks, Build.Profile.NumBendHolds, *JoinWarnings(River)));
		if (Test.TestTrue(*Say(TEXT("the river has a bed and vertices (non-vacuous)")), Bed.IsValid() && Vertices.Num() >= 10))
		{
			Test.TestEqual(*Say(TEXT("a vertex of the reach a vertex of the bed")), Bed->GetVertices().Num(), Vertices.Num());
			Test.TestTrue(*Say(TEXT("the bed lowers only, with the component's id")), Bed->GetBlend() == EVoxelConditioningLineBlend::LowerOnly && Bed->GetId() == River->GetLineId());
			Test.TestTrue(*Say(TEXT("the river runs at the component's speed")), Bodies[1].Reach->GetFlowSpeed() == River->FlowSpeed);

			int32 Rises = 0, BedOver = 0, OverEdge = 0, EdgesRead = 0, BedNotTarget = 0;
			for (int32 Index = 0; Index < Vertices.Num(); ++Index)
			{
				const FVoxelWaterReachVertex& Vertex = Vertices[Index];
				Rises += Index > 0 && Vertex.SurfaceLevel > Vertices[Index - 1].SurfaceLevel + 1.0e-6 ? 1 : 0;
				BedOver += Vertex.BedLevel > Vertex.SurfaceLevel + 1.0e-6 ? 1 : 0;
				BedNotTarget += FMath::IsNearlyEqual(Vertex.BedLevel - LevelBase, static_cast<double>(Bed->GetVertices()[Index].TargetHeight), 0.05) ? 0 : 1;
				if (Vertex.SurfaceLevel - LevelBase <= Sea + 1.0)
				{
					continue; // in the sea
				}
				// The natural ground at the footprint's edge on both sides stands over the surface by the margin.
				const FVector Here = VertexAt(Frame, Vertex, LevelBase);
				const FVector Up = Frame.UpAt(Here);
				FVector Along = VertexAt(Frame, Vertices[FMath::Min(Index + 1, Vertices.Num() - 1)], LevelBase) - VertexAt(Frame, Vertices[FMath::Max(Index - 1, 0)], LevelBase);
				Along -= Up * FVector::DotProduct(Along, Up);
				const FVector Across = FVector::CrossProduct(Up, Along.GetSafeNormal());
				for (const double Side : { -1.0, 1.0 })
				{
					++EdgesRead;
					OverEdge += Vertex.SurfaceLevel - LevelBase > Ground.HeightAt(Here + Across * (Side * Vertex.HalfWidth)) - River->BankMargin + 0.5 ? 1 : 0;
				}
			}
			Test.TestEqual(*Say(TEXT("the surface never rises downstream")), Rises, 0);
			Test.TestEqual(*Say(TEXT("the bed lies under the surface")), BedOver, 0);
			Test.TestEqual(*Say(TEXT("the reach's bed level is the bed line's target")), BedNotTarget, 0);
			Test.TestTrue(*Say(TEXT("footprint edges were read on land (non-vacuous)")), EdgesRead >= 10);
			Test.TestEqual(*Say(TEXT("the surface lies the margin under the ground at the footprint's edge, on both sides")), OverEdge, 0);

			Test.TestTrue(*Say(TEXT("the river has a head: it starts in no body")), !Build.Profile.bStartsInBody);
			Test.TestTrue(*Say(TEXT("... its width and its depth start from nothing")),
				FMath::IsNearlyEqual(Vertices[0].HalfWidth, static_cast<double>(River->BankWidth), 0.01) && FMath::IsNearlyEqual(Vertices[0].BedLevel, Vertices[0].SurfaceLevel, 0.01));
			Test.TestTrue(*Say(TEXT("the river ends in the sea")), Build.Profile.bEndsInBody && !HasWarning(River, TEXT("ends on land")));
			Test.TestTrue(*Say(TEXT("... a tuck under the sea's level, and nowhere lower")),
				FMath::IsNearlyEqual(Vertices.Last().SurfaceLevel - LevelBase, Sea - 10.0, 0.05) && FMath::IsNearlyEqual(Bodies[1].Reach->GetMinLevel() - LevelBase, Sea - 10.0, 0.05));
			Test.TestTrue(*Say(TEXT("... keeping a few vertices inside it")), Build.Profile.First + Build.Profile.Num <= Build.NumSampled);
			Test.TestTrue(*Say(TEXT("the river falls along its length")), Vertices[0].SurfaceLevel - Vertices.Last().SurfaceLevel > 150.0);
		}

		// ---- The lake.
		const FVoxelLakeBuild& LakeBuild = Lake->GetLakeBuild();
		Test.AddInfo(FString::Printf(TEXT("%s: the lake: level %.0f (the outline's ground %.0f to %.0f), %d cells under it, the deepest %.0f; warnings: %s"),
			*Name, LakeBuild.Level - LevelBase, LakeBuild.LowestOutlineGround, LakeBuild.HighestOutlineGround, LakeBuild.NumWetCells, LakeBuild.Deepest, *JoinWarnings(Lake)));
		Test.TestTrue(*Say(TEXT("the lake's level is the margin under the lowest ground along its outline")),
			FMath::IsNearlyEqual(LakeBuild.Level - LevelBase, LakeBuild.LowestOutlineGround - Lake->LevelMargin, 0.01));
		Test.TestTrue(*Say(TEXT("the lake stands over the sea and over its pit")), LakeBuild.Level - LevelBase > Sea + 50.0 && LakeBuild.Level - LevelBase > Hollow.PitHeight + 50.0);
		Test.TestTrue(*Say(TEXT("cells inside the outline lie under it (non-vacuous)")), LakeBuild.NumWetCells >= 4 && LakeBuild.Deepest > 50.0);
		Test.TestEqual(*Say(TEXT("no place of the outline lies under the level")), LakeBuild.NumOutlinePlacesUnder, 0);
		Test.TestTrue(*Say(TEXT("the lake's record: its level, its footprint, the caves' depth")),
			Bodies[0].Level == LakeBuild.Level && Bodies[0].Lake == LakeBuild.Footprint && Bodies[0].MinCaveDepth == Lake->MinCaveDepth);
		Test.TestTrue(*Say(TEXT("the footprint is the water and a cell of shore, not the whole outline")),
			LakeBuild.Footprint->NumCells() > LakeBuild.NumWetCells && LakeBuild.Footprint->NumCells() < 9 * LakeBuild.NumWetCells + 16);

		// ---- The world streams round the lake, then round the river: the voxels hold their water.
		const FVector InLake = Ground.AtHeight(Hollow.Pit, 0.5 * (Hollow.PitHeight + (LakeBuild.Level - LevelBase)));
		W.SetViewer(Ground.AtHeight(Hollow.Pit, LakeBuild.Level - LevelBase + 300.0));
		if (Test.TestTrue(*Say(TEXT("the world settles round the lake")), W.Settle()))
		{
			FVoxelWaterSample Sample;
			Test.TestTrue(*Say(TEXT("a voxel of the lake holds flagged water")), Manager->GetVoxelAt(InLake).HasWaterFlag());
			Test.TestTrue(*Say(TEXT("the world's sample in the lake is the lake's")), Manager->SampleWater(InLake, Sample) && Sample.bWater
				&& Sample.Kind == EVoxelWaterBodyKind::Lake && Sample.BodyId == Lake->GetWaterBodyId() && Sample.Depth > 0.0 && Sample.Flow.IsZero());
			Test.TestFalse(*Say(TEXT("a voxel over the lake holds none")), Manager->GetVoxelAt(Ground.AtHeight(Hollow.Pit, LakeBuild.Level - LevelBase + 100.0)).HasWaterFlag());
		}

		// A vertex of the river at its full depth, on land, about two thirds of the way down.
		int32 At = INDEX_NONE;
		for (int32 Index = (2 * Vertices.Num()) / 3; Index > 4 && At == INDEX_NONE; --Index)
		{
			if (Vertices[Index].SurfaceLevel - LevelBase > Sea + 20.0 && Vertices[Index].SurfaceLevel - Vertices[Index].BedLevel > 0.9 * River->Depth)
			{
				At = Index;
			}
		}
		if (Test.TestTrue(*Say(TEXT("the river has a vertex at full depth on land (the fixture)")), At != INDEX_NONE))
		{
			const FVoxelWaterReachVertex& Vertex = Vertices[At];
			const FVector InRiver = VertexAt(Frame, Vertex, Vertex.SurfaceLevel - 0.5 * River->Depth);
			const FVector Up = Frame.UpAt(InRiver);
			W.SetViewer(InRiver + Up * 300.0);
			if (Test.TestTrue(*Say(TEXT("the world settles round the river")), W.Settle()))
			{
				FVoxelWaterSample Sample;
				Test.TestTrue(*Say(TEXT("a voxel of the river holds flagged water: the bed is cut and the reach fills it")), Manager->GetVoxelAt(InRiver).HasWaterFlag());
				const bool bSampled = Manager->SampleWater(InRiver, Sample);
				Test.TestTrue(*Say(TEXT("the world's sample in the river is the river reach's")), bSampled && Sample.bWater
					&& Sample.Kind == EVoxelWaterBodyKind::Reach && Sample.BodyId == River->GetWaterBodyId() && Sample.Depth > 0.0);
				// At a vertex the nearest segment is the one before it or the one after: the flow runs along one of the two.
				const FVector Before = (VertexAt(Frame, Vertex, LevelBase) - VertexAt(Frame, Vertices[At - 1], LevelBase)).GetSafeNormal();
				const FVector After = (VertexAt(Frame, Vertices[At + 1], LevelBase) - VertexAt(Frame, Vertex, LevelBase)).GetSafeNormal();
				// The flow runs along the surface, which falls: its part across the up is the river's way, its part along the
				// up points down.
				const FVector Level = FVector::VectorPlaneProject(Sample.Flow, Up).GetSafeNormal();
				const double Along = FMath::Max(FVector::DotProduct(Level, FVector::VectorPlaneProject(Before, Up).GetSafeNormal()),
					FVector::DotProduct(Level, FVector::VectorPlaneProject(After, Up).GetSafeNormal()));
				Test.AddInfo(FString::Printf(TEXT("%s: in the river at vertex %d: depth %.0f, flow %.1f uu/s, %.3f along the river, %.1f uu/s of it down"),
					*Name, At, Sample.Depth, Sample.Flow.Size(), Along, -FVector::DotProduct(Sample.Flow, Up)));
				Test.TestTrue(*Say(TEXT("... its flow downstream at the component's speed")),
					FMath::IsNearlyEqual(Sample.Flow.Size(), static_cast<double>(River->FlowSpeed), 0.01) && Along > 0.98);
				Test.TestTrue(*Say(TEXT("... down the surface's fall, not up it")), FVector::DotProduct(Sample.Flow, Up) <= 1.0e-6);
				Test.TestFalse(*Say(TEXT("a voxel over the river holds none")), Manager->GetVoxelAt(VertexAt(Frame, Vertex, Vertex.SurfaceLevel + 100.0)).HasWaterFlag());
			}
		}
		Test.TestEqual(*Say(TEXT("no source registered late")), Manager->GetNumLateConditioningSources(), 0);
		W.Stop();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHandPlacedWaterPlaneTest,
	"VoxelWorlds.Streaming.HandPlacedWater.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHandPlacedWaterPlaneTest::RunTest(const FString& Parameters)
{
	VoxelHandPlacedWaterTest::RunWorld(*this, VoxelTesting::EHandPlacedWorld::Plane);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHandPlacedWaterIslandTest,
	"VoxelWorlds.Streaming.HandPlacedWater.Island",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHandPlacedWaterIslandTest::RunTest(const FString& Parameters)
{
	VoxelHandPlacedWaterTest::RunWorld(*this, VoxelTesting::EHandPlacedWorld::Island);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHandPlacedWaterPlanetTest,
	"VoxelWorlds.Streaming.HandPlacedWater.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHandPlacedWaterPlanetTest::RunTest(const FString& Parameters)
{
	VoxelHandPlacedWaterTest::RunWorld(*this, VoxelTesting::EHandPlacedWorld::Planet);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHandPlacedWaterCubicTest,
	"VoxelWorlds.Streaming.HandPlacedWater.Cubic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHandPlacedWaterCubicTest::RunTest(const FString& Parameters)
{
	VoxelHandPlacedWaterTest::RunWorld(*this, VoxelTesting::EHandPlacedWorld::Cubic);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHandPlacedWaterRulesTest,
	"VoxelWorlds.Streaming.HandPlacedWater.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHandPlacedWaterRulesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelHandPlacedWaterTest;

	// The warnings the components log are the test's subject.
	AddExpectedMessagePlain(TEXT("Water body "), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	FTestChunkWorld W;
	W.Create(TEXT("HandPlacedWaterRules"));
	ConfigureHandPlacedWorld(W, EHandPlacedWorld::Plane);
	W.StartManager();
	UVoxelChunkManager* Manager = W.Manager;
	const FHandPlacedGround Ground(*Manager);
	const double Sea = Ground.SeaHeight();
	TArray<FVector> Course;
	FHandPlacedHollow Hollow;
	if (!TestTrue(TEXT("The ground has a course down to the sea (the fixture)"), FindRiverCourse(Ground, FVector::ZeroVector, 6000.0, 1200.0, 5000.0, Course))
		|| !TestTrue(TEXT("The ground has a hollow over the sea (the fixture)"), FindHollow(Ground, FVector::ZeroVector, 6000.0, 150.0, Hollow)))
	{
		W.Stop();
		return true;
	}

	// 1. A spline at its own height that climbs: 2,000 uu straight over the course's start, 100 under the ground at its
	//    first point and 400 higher at its last. The surface is lowered where it rises, and the river says so.
	{
		const FVector A = Course[0];
		const FVector B = Ground.OnGround(Ground.Offset(A, 2000.0, 0.0));
		AVoxelRiverActor* Actor = W.World->SpawnActor<AVoxelRiverActor>();
		Actor->GetSpline()->SetSplinePoints({ FVector(A.X, A.Y, A.Z - 100.0), FVector(B.X, B.Y, A.Z + 300.0) }, ESplineCoordinateSpace::World, true);
		UVoxelRiverComponent* River = Actor->GetRiver();
		ConfigureRiver(River);
		River->HeightMode = EVoxelTerrainLineHeight::SplineHeight;
		FVoxelRiverBuild Build;
		if (TestTrue(TEXT("Climbs: the river is made"), River->BuildRiver(*Manager, Build)))
		{
			int32 Rises = 0;
			for (int32 Index = 1; Index < Build.Vertices.Num(); ++Index)
			{
				Rises += Build.Vertices[Index].SurfaceLevel > Build.Vertices[Index - 1].SurfaceLevel + 1.0e-6 ? 1 : 0;
			}
			TestTrue(TEXT("Climbs: the spline's rises are counted"), Build.Profile.NumRises > Build.NumSampled / 2 && Build.Profile.MostRise > 300.0);
			TestEqual(TEXT("Climbs: the river as made never rises"), Rises, 0);
			TestTrue(TEXT("Climbs: its surface is no higher than the spline's first point"), Build.Vertices.Num() > 0 && Build.Vertices[0].SurfaceLevel <= A.Z - 100.0 + 0.05);
		}
		Actor->DispatchBeginPlay();
		TestTrue(TEXT("Climbs: the river is added whatever it says"), River->IsBodyRegistered() && Manager->GetNumWaterBodies() == 1);
		TestTrue(TEXT("Climbs: ... and says the spline rises downstream"), HasWarning(River, TEXT("rises downstream")));
	}

	// 2. A river that stops on land: the first half of the course.
	{
		TArray<FVector> Half(Course.GetData(), Course.Num() / 2);
		AVoxelRiverActor* Actor = SpawnRiverAlong(W.World, Ground, Half);
		UVoxelRiverComponent* River = Actor->GetRiver();
		ConfigureRiver(River);
		FinishHandPlaced(Actor);
		const FVoxelRiverBuild& Build = River->GetRiverBuild();
		TestTrue(TEXT("On land: the course's first half stops over the sea (the fixture)"), Ground.HeightAt(Half.Last()) > Sea + 50.0);
		TestTrue(TEXT("On land: the river is added"), River->IsBodyRegistered());
		TestTrue(TEXT("On land: it ends in no body, and says so"), !Build.Profile.bEndsInBody && HasWarning(River, TEXT("ends on land")));
		if (TestTrue(TEXT("On land: it has vertices"), Build.Vertices.Num() > 8))
		{
			TestTrue(TEXT("On land: it thins to nothing at its end"), FMath::IsNearlyEqual(Build.Vertices.Last().HalfWidth, static_cast<double>(River->BankWidth), 0.01)
				&& FMath::IsNearlyEqual(Build.Vertices.Last().BedLevel, Build.Vertices.Last().SurfaceLevel, 0.01));
			TestEqual(TEXT("On land: every vertex is kept"), Build.Vertices.Num(), Build.NumSampled);
		}
		TestFalse(TEXT("On land: the narrow bed of a head and an end is not a fault of the drawing"), HasWarning(River, TEXT("the levelled core is")));
	}

	// 3. A lake, and a river whose head lies in it: from the pit straight out over the lowest of the ring and 1,500 uu on.
	//    The river begins play first.
	AVoxelLakeActor* LakeActor = SpawnLakeRound(W.World, Ground, Hollow.Pit, Hollow.Radius);
	UVoxelLakeComponent* Lake = LakeActor->GetLake();
	Lake->LevelMargin = 25.0f;
	{
		double LowestAngle = 0.0, Lowest = TNumericLimits<double>::Max();
		for (int32 Turn = 0; Turn < 32; ++Turn)
		{
			const double Angle = UE_DOUBLE_TWO_PI * Turn / 32.0;
			const double Height = Ground.HeightAt(Ground.Offset(Hollow.Pit, Hollow.Radius * FMath::Cos(Angle), Hollow.Radius * FMath::Sin(Angle)));
			if (Height < Lowest)
			{
				Lowest = Height;
				LowestAngle = Angle;
			}
		}
		const double Far = Hollow.Radius + 1500.0;
		const FVector End = Ground.OnGround(Ground.Offset(Hollow.Pit, Far * FMath::Cos(LowestAngle), Far * FMath::Sin(LowestAngle)));
		AVoxelRiverActor* Actor = W.World->SpawnActor<AVoxelRiverActor>();
		Actor->GetSpline()->SetSplinePoints({ Hollow.Pit + FVector(0.0, 0.0, 300.0), End + FVector(0.0, 0.0, 300.0) }, ESplineCoordinateSpace::World, true);
		UVoxelRiverComponent* River = Actor->GetRiver();
		ConfigureRiver(River);
		const int32 BodiesBefore = Manager->GetNumWaterBodies();
		Actor->DispatchBeginPlay();
		TestTrue(TEXT("Lake head: the lake registered when the river did"), Lake->IsBodyRegistered() && River->IsBodyRegistered());
		const TArray<FVoxelWaterBody>& Bodies = Manager->GetStaticWaterBodies();
		if (TestEqual(TEXT("Lake head: two more bodies"), Bodies.Num(), BodiesBefore + 2))
		{
			TestTrue(TEXT("Lake head: the lake before the river"), Bodies[BodiesBefore].Kind == EVoxelWaterBodyKind::Lake && Bodies[BodiesBefore + 1].Kind == EVoxelWaterBodyKind::Reach);
			const FVoxelRiverBuild& Build = River->GetRiverBuild();
			const double LakeLevel = Lake->GetLakeBuild().Level;
			AddInfo(FString::Printf(TEXT("Lake head: the lake at %.0f; the river keeps %d of %d vertices from %d, its first surface %.0f; %s"),
				LakeLevel, Build.Profile.Num, Build.NumSampled, Build.Profile.First, Build.Vertices.Num() > 0 ? Build.Vertices[0].SurfaceLevel : 0.0, *JoinWarnings(River)));
			TestTrue(TEXT("Lake head: the river starts in the lake"), Build.Profile.bStartsInBody);
			if (TestTrue(TEXT("Lake head: it has vertices"), Build.Vertices.Num() > 4))
			{
				TestTrue(TEXT("Lake head: a tuck under the lake's level"), FMath::IsNearlyEqual(Build.Vertices[0].SurfaceLevel, LakeLevel - 10.0, 0.05));
				TestTrue(TEXT("Lake head: at its full width and depth: no head"),
					FMath::IsNearlyEqual(Build.Vertices[0].HalfWidth, static_cast<double>(River->HalfWidth + River->BankWidth), 0.01)
					&& Build.Vertices[0].SurfaceLevel - Build.Vertices[0].BedLevel > 0.99 * River->Depth);
				TestTrue(TEXT("Lake head: the vertices deep in the lake are dropped"), Build.Profile.First > 0);
			}
		}
		FinishHandPlaced(LakeActor);
		TestEqual(TEXT("Lake head: the lake is not added twice"), Manager->GetNumWaterBodies(), BodiesBefore + 2);
	}

	// 4. A tributary: a river of order 1 down the course, and one of order 0 that runs 900 uu across to a place two
	//    thirds of the way down it. The tributary begins play first; it ends in the river all the same.
	{
		AVoxelRiverActor* MainActor = SpawnRiverAlong(W.World, Ground, Course);
		UVoxelRiverComponent* Main = MainActor->GetRiver();
		ConfigureRiver(Main);
		Main->Order = 1;
		const int32 Join = (2 * Course.Num()) / 3;
		const FVector Along = (Course[Join + 1] - Course[Join - 1]).GetSafeNormal2D();
		const FVector From = Ground.OnGround(Course[Join] + FVector(-Along.Y, Along.X, 0.0) * 900.0);
		AVoxelRiverActor* SideActor = W.World->SpawnActor<AVoxelRiverActor>();
		SideActor->GetSpline()->SetSplinePoints({ From + FVector(0.0, 0.0, 300.0), Course[Join] + FVector(0.0, 0.0, 300.0) }, ESplineCoordinateSpace::World, true);
		UVoxelRiverComponent* Side = SideActor->GetRiver();
		ConfigureRiver(Side);
		const int32 BodiesBefore = Manager->GetNumWaterBodies();
		SideActor->DispatchBeginPlay();
		TestTrue(TEXT("Tributary: the river of the higher order registered when the tributary did"), Main->IsBodyRegistered() && Side->IsBodyRegistered());
		const TArray<FVoxelWaterBody>& Bodies = Manager->GetStaticWaterBodies();
		if (TestEqual(TEXT("Tributary: two more bodies"), Bodies.Num(), BodiesBefore + 2))
		{
			TestTrue(TEXT("Tributary: the river first"), Bodies[BodiesBefore].Id == Main->GetWaterBodyId() && Bodies[BodiesBefore + 1].Id == Side->GetWaterBodyId());
			TestTrue(TEXT("Tributary: the river's bed is applied after the tributary's"), Main->GetLineId() > Side->GetLineId());
			const FVoxelRiverBuild& Build = Side->GetRiverBuild();
			AddInfo(FString::Printf(TEXT("Tributary: it keeps %d of %d vertices; %s"), Build.Profile.Num, Build.NumSampled, *JoinWarnings(Side)));
			TestTrue(TEXT("Tributary: it ends in the river, not on land"), Build.Profile.bEndsInBody && !HasWarning(Side, TEXT("ends on land")));
			// Where it ends, the river's own surface is a tuck over the tributary's last.
			FVoxelWaterReachSample MainSample;
			if (TestTrue(TEXT("Tributary: its last vertex lies in the river's footprint"), Build.Vertices.Num() > 0
				&& Bodies[BodiesBefore].Reach->SampleAt(Build.Vertices.Last().Position.X, Build.Vertices.Last().Position.Y, MainSample)))
			{
				// (Or at it, where the river has come down to the sea: no surface lies under the sea less the tuck.)
				TestTrue(TEXT("Tributary: ... a tuck under the river there, or at it"), Build.Vertices.Last().SurfaceLevel <= MainSample.SurfaceLevel + 0.05
					&& Build.Vertices.Last().SurfaceLevel >= MainSample.SurfaceLevel - 10.0 - 1.0);
			}
		}
	}

	// 5. A lake at its actor's height, 80 over the lowest ground of its outline: made, with the wall's warning. And one
	//    far under the ground: no water, no body, and it says so.
	{
		AVoxelLakeActor* High = SpawnLakeRound(W.World, Ground, Hollow.Pit, Hollow.Radius);
		High->GetLake()->LevelMode = EVoxelLakeLevel::ActorHeight;
		High->SetActorLocation(FVector(Hollow.Pit.X, Hollow.Pit.Y, Hollow.RimHeight + 80.0));
		const int32 BodiesBefore = Manager->GetNumWaterBodies();
		FinishHandPlaced(High);
		const FVoxelLakeBuild& Build = High->GetLake()->GetLakeBuild();
		TestTrue(TEXT("High lake: its level is its actor's height"), FMath::IsNearlyEqual(Build.Level, Hollow.RimHeight + 80.0, 0.01));
		TestTrue(TEXT("High lake: places of the outline lie under it"), Build.NumOutlinePlacesUnder > 0 && Build.MostOutlineUnder > 40.0);
		TestTrue(TEXT("High lake: it is added, with the wall's warning"), Manager->GetNumWaterBodies() == BodiesBefore + 1 && HasWarning(High->GetLake(), TEXT("ends in a wall")));

		AVoxelLakeActor* Dry = SpawnLakeRound(W.World, Ground, Hollow.Pit, Hollow.Radius);
		Dry->GetLake()->LevelMode = EVoxelLakeLevel::ActorHeight;
		Dry->SetActorLocation(FVector(Hollow.Pit.X, Hollow.Pit.Y, Hollow.PitHeight - 500.0));
		FinishHandPlaced(Dry);
		TestTrue(TEXT("Dry lake: nothing lies under its level"), Dry->GetLake()->GetLakeBuild().NumWetCells == 0);
		TestTrue(TEXT("Dry lake: no body is added, and it says the lake holds no water"),
			Manager->GetNumWaterBodies() == BodiesBefore + 1 && HasWarning(Dry->GetLake(), TEXT("holds no water")));
	}
	TestEqual(TEXT("Nothing was generated while the bodies registered"), Manager->GetNumGenerationRequestsBuilt(), static_cast<int64>(0));
	W.Stop();

	// 6. A river whose actor begins play before the voxel world starts registers when it starts; and a second world
	//    makes the same river, vertex for vertex.
	TArray<FVoxelWaterReachVertex> First;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		FTestChunkWorld Other;
		Other.Create(Pass == 0 ? TEXT("HandPlacedWaterBeforeWorld") : TEXT("HandPlacedWaterAgain"));
		ConfigureHandPlacedWorld(Other, EHandPlacedWorld::Plane);
		Other.SpawnManager();
		AVoxelRiverActor* Actor = Other.World->SpawnActor<AVoxelRiverActor>();
		TArray<FVector> Points;
		for (int32 Index = 0; Index < Course.Num(); Index += 4)
		{
			Points.Add(Course[Index] + FVector(0.0, 0.0, 300.0));
		}
		Actor->GetSpline()->SetSplinePoints(Points, ESplineCoordinateSpace::World, true);
		UVoxelRiverComponent* River = Actor->GetRiver();
		ConfigureRiver(River);
		Actor->DispatchBeginPlay();
		TestFalse(TEXT("Before the world: nothing to register with yet"), River->IsBodyRegistered());
		Other.InitializeManager();
		TestTrue(TEXT("Before the world: registered at the end of the chunk manager's Initialize"), River->IsBodyRegistered());
		TestTrue(TEXT("Before the world: the manager holds the river and its bed"), Other.Manager->GetNumWaterBodies() == 1 && Other.Manager->GetNumConditioningLines() == 1);
		TestEqual(TEXT("Before the world: no generation request was built before it"), Other.Manager->GetNumGenerationRequestsBuilt(), static_cast<int64>(0));
		const TArray<FVoxelWaterReachVertex>& Vertices = River->GetRiverBuild().Vertices;
		if (Pass == 0)
		{
			First = Vertices;
		}
		else
		{
			int32 Differing = Vertices.Num() == First.Num() ? 0 : 1;
			for (int32 Index = 0; Index < FMath::Min(Vertices.Num(), First.Num()); ++Index)
			{
				Differing += Vertices[Index].Position == First[Index].Position && Vertices[Index].BedLevel == First[Index].BedLevel
					&& Vertices[Index].SurfaceLevel == First[Index].SurfaceLevel && Vertices[Index].HalfWidth == First[Index].HalfWidth ? 0 : 1;
			}
			TestTrue(TEXT("Again: the river has vertices (non-vacuous)"), Vertices.Num() > 8);
			TestEqual(TEXT("Again: a second world makes the same river, vertex for vertex"), Differing, 0);
		}
		Other.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
