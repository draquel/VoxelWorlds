// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelConditioningLine.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Lakes and river reaches on the chunk manager (the hydrology plan's H1, section 5.2): the static bodies and the
// sources, the gathers by bounds, the point gather and the announcement.
//
// - Streaming.Water.Bodies.Plane, .Planet: a world with a sea under its land, a lake and a river reach added when the
//   world starts and a source registered beside them. The announcement names each body's footprint (a box, a cap) and
//   everywhere for a source; the point gather (SampleWater) answers with the lake in its footprint, the reach along
//   its polyline and the sea elsewhere, keeps the snapshot of a place (a second sample there gathers nothing) and drops
//   it when the bodies change; the range over a chunk the lake reaches is not uniform and holds its level, with a
//   Complete gather for a chunk and a Ready one where asked; a source is asked with the need the gather was made
//   with, and stops answering once removed. The world then streams with no source registered late, and the cost of a
//   sample with a body near is measured against the sea's alone.
// - Streaming.Water.Bodies.BedAndReach: a river reach and a conditioning line made from the same vertices (a bend, a
//   vertex given twice) read the same place along the same segment: at random points of the footprint the reach's
//   bed level is the line's target there, on a plane and on a planet.
// ---------------------------------------------------------------------------

namespace VoxelWaterBodiesStreamingTest
{
	/** A source that hands out one pond and counts how it was asked. */
	struct FPondSource : IVoxelWaterBodySource
	{
		FVoxelWaterBody Pond;
		mutable int32 RegionComplete = 0;
		mutable int32 RegionReady = 0;
		mutable int32 SphereComplete = 0;
		mutable int32 SphereReady = 0;

		virtual void GatherWaterBodies(const FBox2D& Bounds, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& OutBodies) const override
		{
			++(Need == EVoxelConditioningGather::Complete ? RegionComplete : RegionReady);
			if (Pond.MeetsRegion(Bounds))
			{
				OutBodies.Add(Pond);
			}
		}

		virtual void GatherWaterBodiesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need, TArray<FVoxelWaterBody>& OutBodies) const override
		{
			++(Need == EVoxelConditioningGather::Complete ? SphereComplete : SphereReady);
			if (Pond.MeetsCap(Direction, AngularRadius, PlanetRadius))
			{
				OutBodies.Add(Pond);
			}
		}

		double PlanetRadius = 0.0;
	};

	TBitArray<> AllCells(int32 Cells)
	{
		return TBitArray<>(true, Cells * Cells);
	}

	void Run(FAutomationTestBase& Test, bool bPlanet)
	{
		const FString Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		VoxelTesting::FTestChunkWorld W;
		W.Create(bPlanet ? TEXT("VoxelWaterBodiesPlanetWorld") : TEXT("VoxelWaterBodiesPlaneWorld"));
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
			W.Config->WaterRadius = W.Config->WorldRadius - 3000.0f; // under every piece of land
		}
		else
		{
			W.ConfigureSmallPlane();
			W.Config->WaterLevel = -3000.0f;
		}
		W.Config->bEnableWaterLevel = true;
		W.StartManager();
		UVoxelChunkManager* Manager = W.Manager;
		const FVoxelWaterLevels SeaLevels = Manager->GetWaterLevels();
		const FVoxelWaterFrame Frame = SeaLevels.GetFrame();
		const double SeaLevel = bPlanet ? SeaLevels.GetSea().Radius : SeaLevels.GetSea().PlaneZ;
		const double ChunkWorldSize = W.Config->GetChunkWorldSize();

		// The site: a point of the ground, its local up and two directions along the surface.
		const FVector SiteDirection = FVector(0.5, 0.3, 0.8).GetSafeNormal();
		const FVector Site = bPlanet ? Manager->GetGeneratedSurfacePoint(Frame.Center + SiteDirection * Frame.DatumRadius)
			: Manager->GetGeneratedSurfacePoint(FVector(2300.0, -1700.0, 0.0));
		const FVector Up = Manager->GetUpVectorAt(Site);
		const FVector East = bPlanet ? FVector::CrossProduct(FVector::UpVector, Up).GetSafeNormal() : FVector::ForwardVector;
		const FVector North = FVector::CrossProduct(Up, East);
		const double SiteHeight = Frame.HeightOf(Site);
		// A point Along and Across the surface from the site, at a height over (or under) the site's.
		const auto At = [&](double Along, double Across, double Over)
		{
			if (!bPlanet)
			{
				return Site + East * Along + North * Across + FVector(0.0, 0.0, Over);
			}
			const FVector Direction = (Up * Frame.DatumRadius + East * Along + North * Across).GetSafeNormal();
			return Frame.Center + Direction * (SiteHeight + Over);
		};
		const auto DirectionOf = [&](const FVector& P) { return (P - Frame.Center).GetSafeNormal(); };

		// ---- Before any body: the sea's own arithmetic, and its cost.
		Test.TestFalse(*FString::Printf(TEXT("%s: a world starts with no lake and no river reach"), *Name), Manager->HasWaterBodies());
		FVoxelWaterSample S;
		Test.TestTrue(*FString::Printf(TEXT("%s: with the sea alone the point gather is the sea's sample"), *Name),
			Manager->SampleWater(At(0.0, 0.0, 0.0), S) && S.Kind == EVoxelWaterBodyKind::Sea && S.Depth == SeaLevels.GetDepth(At(0.0, 0.0, 0.0))
			&& Manager->GetWaterGatherStats().PointSamples == 0);
		const int32 TimedSamples = 200000;
		double SeaSeconds = 0.0;
		{
			double Sum = 0.0;
			const double Start = FPlatformTime::Seconds();
			for (int32 i = 0; i < TimedSamples; ++i)
			{
				Manager->SampleWater(At((i % 97) * 9.0 - 400.0, (i % 89) * 9.0 - 400.0, 0.0), S);
				Sum += S.Depth;
			}
			SeaSeconds = FPlatformTime::Seconds() - Start;
		}

		// ---- The bodies, added when the world starts: a lake 8 x 8 cells of 200 uu at the site, 250 over its ground, and
		// a river reach 3,000 uu east of it running north for 2,000 uu, falling 200.
		TArray<FVoxelConditioningRegion> Regions;
		const FDelegateHandle RegionHandle = Manager->OnWaterBodiesChanged.AddLambda([&Regions](const FVoxelConditioningRegion& Region) { Regions.Add(Region); });
		const double LakeLevel = SiteHeight + 250.0;
		const TSharedRef<const FVoxelWaterLake> LakeCells = bPlanet
			? FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, 200.0, FIntPoint(8, 8), AllCells(8))
			: FVoxelWaterLake::Make(FVector2D(Site.X - 800.0, Site.Y - 800.0), 200.0, FIntPoint(8, 8), AllCells(8));
		Manager->AddWaterBody(FVoxelWaterBody::MakeLake(11, LakeLevel, LakeCells, Frame, 400.0));

		TArray<FVoxelWaterReachVertex> ReachVertices;
		const int32 ReachSteps = 20;
		for (int32 Step = 0; Step <= ReachSteps; ++Step)
		{
			const double T = static_cast<double>(Step) / ReachSteps;
			const FVector P = At(3000.0, -1000.0 + 2000.0 * T, 0.0);
			const double Surface = SiteHeight + 400.0 - 200.0 * T;
			ReachVertices.Add(FVoxelWaterReachVertex(bPlanet ? DirectionOf(P) : FVector(P.X, P.Y, 0.0), Surface - 100.0, Surface, 150.0));
		}
		const TSharedRef<const FVoxelWaterReach> Reach = FVoxelWaterReach::Make(ReachVertices, bPlanet, 100.0f);
		Manager->AddWaterBody(FVoxelWaterBody::MakeReach(12, Reach, Frame));

		Test.TestTrue(*FString::Printf(TEXT("%s: two bodies are in the world"), *Name), Manager->HasWaterBodies() && Manager->GetNumWaterBodies() == 2);
		Test.TestEqual(*FString::Printf(TEXT("%s: bodies added at the world's start are not late"), *Name), Manager->GetNumLateConditioningSources(), 0);
		if (Test.TestEqual(*FString::Printf(TEXT("%s: each body added is announced"), *Name), Regions.Num(), 2))
		{
			if (bPlanet)
			{
				Test.TestTrue(TEXT("Planet: a lake's announcement is its cap"),
					Regions[0].bCap && !Regions[0].bEverywhere && Regions[0].Direction.Equals(Up, 1.0e-6) && Regions[0].AngularRadius > 800.0 / Frame.DatumRadius
					&& Regions[0].AngularRadius < 1300.0 / Frame.DatumRadius);
				Test.TestTrue(TEXT("Planet: a river reach's announcement is a cap that holds it"),
					Regions[1].bCap && FVoxelPolylineBounds::AngleBetween(Regions[1].Direction, DirectionOf(At(3000.0, 0.0, 0.0))) < 200.0 / Frame.DatumRadius
					&& Regions[1].AngularRadius > 1000.0 / Frame.DatumRadius);
			}
			else
			{
				Test.TestTrue(TEXT("Plane: a lake's announcement is its footprint's box"),
					!Regions[0].bCap && !Regions[0].bEverywhere && Regions[0].Box.Min.Equals(FVector2D(Site.X - 800.0, Site.Y - 800.0))
					&& Regions[0].Box.Max.Equals(FVector2D(Site.X + 800.0, Site.Y + 800.0)));
				Test.TestTrue(TEXT("Plane: a river reach's announcement is its polyline's box grown by its footprint"),
					Regions[1].Box.Min.Equals(FVector2D(Site.X + 3000.0 - 150.0, Site.Y - 1000.0 - 150.0), 1.0e-3)
					&& Regions[1].Box.Max.Equals(FVector2D(Site.X + 3000.0 + 150.0, Site.Y + 1000.0 + 150.0), 1.0e-3));
			}
		}

		// ---- The point gather.
		Test.TestTrue(*FString::Printf(TEXT("%s: the lake answers over its footprint"), *Name),
			Manager->SampleWater(At(300.0, -200.0, 100.0), S) && S.bWater && S.Kind == EVoxelWaterBodyKind::Lake && S.BodyId == 11
			&& FMath::IsNearlyEqual(S.SurfaceLevel, LakeLevel) && FMath::IsNearlyEqual(S.Depth, 150.0, 0.5) && S.SurfaceNormal.Equals(Manager->GetUpVectorAt(At(300.0, -200.0, 100.0)), 1.0e-6));
		Test.TestTrue(*FString::Printf(TEXT("%s: GetWaterDepth and GetWaterDepthAt are the sample's depth"), *Name),
			FMath::IsNearlyEqual(Manager->GetWaterDepth(At(300.0, -200.0, 100.0)), S.Depth, 1.0e-6)
			&& FMath::IsNearlyEqual(Manager->GetWaterDepthAt(At(300.0, -200.0, 100.0)), static_cast<float>(S.Depth), 0.01f));
		Test.TestTrue(*FString::Printf(TEXT("%s: GetSeaDepthAt stays the sea's"), *Name),
			Manager->GetSeaDepthAt(At(300.0, -200.0, 100.0)) == static_cast<float>(SeaLevels.GetDepth(At(300.0, -200.0, 100.0))));
		Test.TestTrue(*FString::Printf(TEXT("%s: outside the footprint the sea answers"), *Name),
			Manager->SampleWater(At(1500.0, 0.0, 0.0), S) && S.Kind == EVoxelWaterBodyKind::Sea && FMath::IsNearlyEqual(S.SurfaceLevel, SeaLevel));
		// Halfway along the reach, 100 uu off its centre line.
		Test.TestTrue(*FString::Printf(TEXT("%s: the river reach answers along its polyline, at the level interpolated there"), *Name),
			Manager->SampleWater(At(3100.0, 0.0, 200.0), S) && S.Kind == EVoxelWaterBodyKind::Reach && S.BodyId == 12
			&& FMath::IsNearlyEqual(S.SurfaceLevel, SiteHeight + 300.0, 1.0) && FMath::IsNearlyEqual(S.Flow.Size(), 100.0, 0.01)
			&& FVector::DotProduct(S.Flow.GetSafeNormal(), North) > 0.95);
		Test.TestTrue(*FString::Printf(TEXT("%s: past the reach's half width the sea answers"), *Name),
			Manager->SampleWater(At(3200.0, 0.0, 200.0), S) && S.Kind == EVoxelWaterBodyKind::Sea);

		// A place is gathered once: more samples there read the kept snapshot.
		const UVoxelChunkManager::FWaterGatherStats Before = Manager->GetWaterGatherStats();
		for (int32 i = 0; i < 50; ++i)
		{
			Manager->SampleWater(At(300.0 + i, -200.0, 100.0 - i), S);
		}
		const UVoxelChunkManager::FWaterGatherStats After = Manager->GetWaterGatherStats();
		Test.TestTrue(*FString::Printf(TEXT("%s: samples at a place already asked gather nothing"), *Name),
			After.PointSamples == Before.PointSamples + 50 && After.PointGathers <= Before.PointGathers + 1 && After.PointSnapshots >= 1);
		const FVoxelWaterLevels AtSite = Manager->GatherWaterLevelsAt(At(300.0, -200.0, 100.0));
		FVoxelWaterSample FromSnapshot;
		Test.TestTrue(*FString::Printf(TEXT("%s: the place's snapshot gives the same sample"), *Name),
			!AtSite.IsSeaAlone() && AtSite.Sample(At(300.0, -200.0, 100.0), FromSnapshot) && FromSnapshot.BodyId == 11 && FMath::IsNearlyEqual(FromSnapshot.SurfaceLevel, LakeLevel));

		// ---- The range and the chunk's gather.
		const FIntVector SiteChunk = Manager->WorldToChunkCoord(At(0.0, 0.0, 100.0));
		const FVoxelWaterLevels ChunkLevels = Manager->GatherWaterLevelsForChunk(SiteChunk);
		// (A gather is by the footprints' bounds: the reach's may reach the chunk's cap on a small planet too.)
		Test.TestTrue(*FString::Printf(TEXT("%s: a chunk at the lake gathers the sea and the lake"), *Name),
			!ChunkLevels.IsSeaAlone() && ChunkLevels.GetBodies()[0].Kind == EVoxelWaterBodyKind::Sea
			&& ChunkLevels.GetBodies().ContainsByPredicate([](const FVoxelWaterBody& Body) { return Body.Id == 11 && Body.Kind == EVoxelWaterBodyKind::Lake; }));
		const FVoxelWaterLevelRange ChunkRange = Manager->GetWaterRange(ChunkLevels.GetBounds(), EVoxelConditioningGather::Complete);
		Test.TestTrue(*FString::Printf(TEXT("%s: the range over that chunk holds both levels and is not uniform"), *Name),
			ChunkRange.bAny && !ChunkRange.bUniform && FMath::IsNearlyEqual(ChunkRange.MaxLevel, LakeLevel) && FMath::IsNearlyEqual(ChunkRange.MinLevel, SeaLevel));
		const FVoxelWaterLevelRange FarRange = Manager->GetWaterRange(FBox::BuildAABB(At(-9000.0, 7000.0, 0.0), FVector(0.5 * ChunkWorldSize)), EVoxelConditioningGather::Complete);
		Test.TestTrue(*FString::Printf(TEXT("%s: the range away from every body is the sea's, uniform"), *Name),
			FarRange.bAny && FarRange.bUniform && FMath::IsNearlyEqual(FarRange.MaxLevel, SeaLevel));
		const FVoxelWaterLevels FarLevels = Manager->GatherWaterLevels(FBox::BuildAABB(At(-9000.0, 7000.0, 0.0), FVector(0.5 * ChunkWorldSize)), EVoxelConditioningGather::Ready);
		Test.TestTrue(*FString::Printf(TEXT("%s: a gather that meets no body is the sea alone"), *Name), FarLevels.IsSeaAlone() && FarLevels.HasWater());

		// ---- A source: a pond inside the lake, 100 higher, handed out by bounds.
		FPondSource Source;
		Source.PlanetRadius = Frame.DatumRadius;
		const FVector PondCentre = At(-400.0, 400.0, 0.0);
		const TSharedRef<const FVoxelWaterLake> PondCells = bPlanet
			? FVoxelWaterLake::MakeOnSphere(DirectionOf(PondCentre), Frame.DatumRadius, 100.0, FIntPoint(2, 2), AllCells(2))
			: FVoxelWaterLake::Make(FVector2D(PondCentre.X - 100.0, PondCentre.Y - 100.0), 100.0, FIntPoint(2, 2), AllCells(2));
		Source.Pond = FVoxelWaterBody::MakeLake(40, LakeLevel + 100.0, PondCells, Frame, 100.0);
		Manager->AddWaterBodySource(&Source);
		Test.TestTrue(*FString::Printf(TEXT("%s: a source registered may change the water anywhere"), *Name), Regions.Num() == 3 && Regions[2].bEverywhere);
		Test.TestEqual(*FString::Printf(TEXT("%s: the bodies' change emptied the point gather"), *Name), Manager->GetWaterGatherStats().PointSnapshots, 0);
		Test.TestTrue(*FString::Printf(TEXT("%s: the source's pond answers where it stands over the lake"), *Name),
			Manager->SampleWater(PondCentre, S) && S.BodyId == 40 && FMath::IsNearlyEqual(S.SurfaceLevel, LakeLevel + 100.0));
		Test.TestTrue(*FString::Printf(TEXT("%s: a point's gather asks the source for everything"), *Name),
			(bPlanet ? Source.SphereComplete : Source.RegionComplete) >= 1 && (bPlanet ? Source.SphereReady : Source.RegionReady) == 0);
		const int32 ReadyBefore = bPlanet ? Source.SphereReady : Source.RegionReady;
		Manager->GetWaterRange(FBox::BuildAABB(PondCentre, FVector(300.0)), EVoxelConditioningGather::Ready);
		Test.TestEqual(*FString::Printf(TEXT("%s: a Ready gather asks the source for what is at hand"), *Name), (bPlanet ? Source.SphereReady : Source.RegionReady), ReadyBefore + 1);
		Manager->RemoveWaterBodySource(&Source);
		Test.TestTrue(*FString::Printf(TEXT("%s: a source removed may change the water anywhere"), *Name), Regions.Num() == 4 && Regions[3].bEverywhere);
		Test.TestTrue(*FString::Printf(TEXT("%s: without the source the lake answers there again"), *Name),
			Manager->SampleWater(PondCentre, S) && S.BodyId == 11);
		Manager->RemoveWaterBodySource(&Source);
		Test.TestEqual(*FString::Printf(TEXT("%s: removing a source that is not registered announces nothing"), *Name), Regions.Num(), 4);

		// A source's own announcement: the place's snapshot is dropped and the region named.
		Manager->SampleWater(At(0.0, 0.0, 0.0), S);
		if (bPlanet)
		{
			Manager->NotifyWaterBodiesChangedOnSphere(Up, 0.01);
			Test.TestTrue(TEXT("Planet: a source's announcement names its cap"), Regions.Num() == 5 && Regions[4].bCap && Regions[4].AngularRadius == 0.01);
		}
		else
		{
			const FBox2D Named(FVector2D(Site.X - 100.0, Site.Y - 100.0), FVector2D(Site.X + 100.0, Site.Y + 100.0));
			Manager->NotifyWaterBodiesChanged(Named);
			Test.TestTrue(TEXT("Plane: a source's announcement names its region"), Regions.Num() == 5 && !Regions[4].bCap && Regions[4].Box == Named);
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: an announcement empties the point gather"), *Name), Manager->GetWaterGatherStats().PointSnapshots, 0);

		// ---- The cost of a sample with a body near (the plan's done-when), against the sea's alone, over the same points.
		{
			double Sum = 0.0;
			const double Start = FPlatformTime::Seconds();
			for (int32 i = 0; i < TimedSamples; ++i)
			{
				Manager->SampleWater(At((i % 97) * 9.0 - 400.0, (i % 89) * 9.0 - 400.0, 0.0), S);
				Sum += S.Depth;
			}
			const double BodySeconds = FPlatformTime::Seconds() - Start;
			const UVoxelChunkManager::FWaterGatherStats Timed = Manager->GetWaterGatherStats();
			Test.AddInfo(FString::Printf(TEXT("[WaterSample] %s: %.3f microseconds a sample with a lake over the place (%d samples, %lld gathers in all, %d places kept), %.3f with the sea alone"),
				*Name, 1.0e6 * BodySeconds / TimedSamples, TimedSamples, Timed.PointGathers, Timed.PointSnapshots, 1.0e6 * SeaSeconds / TimedSamples));
			UE_LOG(LogTemp, Display, TEXT("[WaterSample] %s: %.3f microseconds a sample with a lake over the place, %.3f with the sea alone"),
				*Name, 1.0e6 * BodySeconds / TimedSamples, 1.0e6 * SeaSeconds / TimedSamples);
		}

		// ---- The world streams with its bodies: nothing registered late.
		W.SetViewer(At(0.0, 0.0, 500.0));
		Test.TestTrue(*FString::Printf(TEXT("%s: streaming settles"), *Name), W.Settle());
		Test.TestTrue(*FString::Printf(TEXT("%s: generation requests were built"), *Name), Manager->GetNumGenerationRequestsBuilt() > 0);
		Test.TestEqual(*FString::Printf(TEXT("%s: no source was registered late"), *Name), Manager->GetNumLateConditioningSources(), 0);
		Test.TestTrue(*FString::Printf(TEXT("%s: the lake still answers once the world streams"), *Name),
			Manager->SampleWater(At(300.0, -200.0, 100.0), S) && S.BodyId == 11);

		Manager->OnWaterBodiesChanged.Remove(RegionHandle);
		W.Stop();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterBodiesPlaneTest,
	"VoxelWorlds.Streaming.Water.Bodies.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterBodiesPlaneTest::RunTest(const FString& Parameters)
{
	VoxelWaterBodiesStreamingTest::Run(*this, false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterBodiesPlanetTest,
	"VoxelWorlds.Streaming.Water.Bodies.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterBodiesPlanetTest::RunTest(const FString& Parameters)
{
	VoxelWaterBodiesStreamingTest::Run(*this, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterBedAndReachTest,
	"VoxelWorlds.Streaming.Water.Bodies.BedAndReach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterBedAndReachTest::RunTest(const FString& Parameters)
{
	for (const bool bPlanet : { false, true })
	{
		const FString Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		const double R = 100000.0;
		// A course with a bend, a hairpin and a vertex given twice: in the plane, or laid on the sphere around +X.
		const TArray<FVector2D> Course = {
			FVector2D(0.0, 0.0), FVector2D(600.0, 100.0), FVector2D(1200.0, 0.0), FVector2D(1200.0, 0.0), FVector2D(1500.0, 500.0),
			FVector2D(1400.0, 1100.0), FVector2D(900.0, 1300.0), FVector2D(950.0, 1900.0), FVector2D(1700.0, 2100.0) };
		const auto Place = [&](const FVector2D& P)
		{
			return bPlanet ? (FVector(R, P.X, P.Y)).GetSafeNormal() : FVector(P.X, P.Y, 0.0);
		};
		FVoxelConditioningLineParams LineParams;
		LineParams.bRadial = bPlanet;
		LineParams.FalloffWidth = 120.0f;
		LineParams.Blend = EVoxelConditioningLineBlend::LowerOnly;
		TArray<FVoxelWaterReachVertex> ReachVertices;
		for (int32 Index = 0; Index < Course.Num(); ++Index)
		{
			// The bed falls 37 uu a vertex; the line's target is a world Z, or an altitude on a planet (the level less R).
			const float Target = 900.0f - 37.0f * Index;
			const float HalfWidth = 90.0f + 5.0f * Index;
			LineParams.Vertices.Add(FVoxelConditioningLineVertex(Place(Course[Index]), Target, HalfWidth));
			const double Bed = (bPlanet ? R : 0.0) + Target;
			// The reach's footprint: the bed's half width and its falloff.
			ReachVertices.Add(FVoxelWaterReachVertex(Place(Course[Index]), Bed, Bed + 60.0, HalfWidth + 120.0));
		}
		const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(LineParams);
		const TSharedRef<const FVoxelWaterReach> Reach = FVoxelWaterReach::Make(ReachVertices, bPlanet);
		TestEqual(*FString::Printf(TEXT("%s: the reach and its bed have the same segments"), *Name), Reach->NumSegments(), Line->NumSegments());

		FRandomStream Random(bPlanet ? 31 : 17);
		int32 Covered = 0, Mismatches = 0;
		double Worst = 0.0;
		for (int32 i = 0; i < 20000; ++i)
		{
			const FVector2D P(Random.FRandRange(-300.0f, 2000.0f), Random.FRandRange(-300.0f, 2400.0f));
			FVoxelWaterReachSample At;
			FVoxelConditioningLineSample Bed;
			const FVector Where = Place(P);
			const bool bReach = bPlanet ? Reach->SampleOnSphere(Where, R, At) : Reach->SampleAt(P.X, P.Y, At);
			if (!bReach)
			{
				continue;
			}
			++Covered;
			const bool bLine = bPlanet ? Line->SampleOnSphere(Where, R, 0.0, Bed) : Line->Sample(P.X, P.Y, 0.0, Bed);
			const double Difference = bLine ? FMath::Abs((At.BedLevel - (bPlanet ? R : 0.0)) - static_cast<double>(Bed.Target)) : TNumericLimits<double>::Max();
			Worst = FMath::Max(Worst, Difference);
			// The line's target is a float; the reach's level a double.
			Mismatches += Difference < 0.01 && FMath::IsNearlyEqual(At.Distance, static_cast<double>(Bed.Distance), 0.05) ? 0 : 1;
		}
		AddInfo(FString::Printf(TEXT("%s: %d points in the reach's footprint, the bed's target and the reach's bed level apart by %.5f at most"), *Name, Covered, Worst));
		TestTrue(*FString::Printf(TEXT("%s: points fell in the footprint (non-vacuous)"), *Name), Covered > 2000);
		TestEqual(*FString::Printf(TEXT("%s: the reach's bed level is the line's target at every point of the footprint (mismatches)"), *Name), Mismatches, 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
