// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The conditioning at a place and the conditioner's line gathers (Documentation/TERRAIN_LINES_PLAN.md, L1).
//
// - LineGathers: the chunk manager's gathers fill an FVoxelConditioning from the game's conditioner: its zones, and its
//   lines through GatherConditioningLines (an XY region) and GatherConditioningLinesOnSphere (a cap), each asked with the
//   caller's need. A Complete gather gets the conditioner's line, a Ready one what the conditioner has at hand (here
//   none); a line is carried by reference, never copied; the chunk manager's own point queries ask Complete; and a set
//   that holds only a line changes no height, since nothing evaluates a line.
// ---------------------------------------------------------------------------

namespace VoxelConditioningGatherTestUtils
{
	/** A conditioner with one line it supplies only to a Complete gather, counting what it was asked. */
	struct FLineConditioner : IVoxelTerrainConditioner
	{
		FVoxelConditioningLineRef Line = MakeShared<FVoxelConditioningLine>();
		mutable int32 RegionComplete = 0;
		mutable int32 RegionReady = 0;
		mutable int32 SphereComplete = 0;
		mutable int32 SphereReady = 0;

		virtual void GatherConditioning(const FBox2D& Region, TArray<FVoxelConditioningZone>& OutZones) const override
		{
		}

		virtual void GatherConditioningLines(const FBox2D& Region, EVoxelConditioningGather Need, TArray<FVoxelConditioningLineRef>& OutLines) const override
		{
			if (Need == EVoxelConditioningGather::Complete)
			{
				++RegionComplete;
				OutLines.Add(Line);
			}
			else
			{
				++RegionReady;
			}
		}

		virtual void GatherConditioningLinesOnSphere(const FVector& Direction, double AngularRadius, EVoxelConditioningGather Need,
			TArray<FVoxelConditioningLineRef>& OutLines) const override
		{
			if (Need == EVoxelConditioningGather::Complete)
			{
				++SphereComplete;
				OutLines.Add(Line);
			}
			else
			{
				++SphereReady;
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineGathersTest,
	"VoxelWorlds.Streaming.Conditioning.LineGathers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineGathersTest::RunTest(const FString& Parameters)
{
	using namespace VoxelConditioningGatherTestUtils;

	// A plane: the region gathers.
	{
		FLineConditioner Conditioner;
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ConditioningLineGathersPlane"));
		W.ConfigureSmallPlane();
		W.StartManager();
		W.Manager->SetTerrainConditioner(&Conditioner);

		const FBox2D Region(FVector2D(-800.0, -800.0), FVector2D(800.0, 800.0));
		FVoxelConditioning Complete;
		W.Manager->GatherConditioning(Region, EVoxelConditioningGather::Complete, Complete);
		TestEqual(TEXT("Plane: a Complete region gather takes the conditioner's line"), Complete.Lines.Num(), 1);
		TestTrue(TEXT("Plane: the line is carried by reference"), Complete.Lines.Num() == 1 && &Complete.Lines[0].Get() == &Conditioner.Line.Get());
		TestEqual(TEXT("Plane: no zone came with it"), Complete.Zones.Num(), 0);
		TestFalse(TEXT("Plane: a set holding only a line is not empty"), Complete.IsEmpty());
		TestEqual(TEXT("Plane: a set holding only a line changes no height"), Complete.ApplyToHeight(100.0, -250.0, 123.5f), 123.5f);

		FVoxelConditioning Ready;
		W.Manager->GatherConditioning(Region, EVoxelConditioningGather::Ready, Ready);
		TestTrue(TEXT("Plane: a Ready region gather takes only what is at hand"), Ready.IsEmpty());

		FVoxelConditioning OfBox;
		W.Manager->GatherConditioning(FBox(FVector(-800.0, -800.0, -5000.0), FVector(800.0, 800.0, 5000.0)), EVoxelConditioningGather::Ready, OfBox);
		TestTrue(TEXT("Plane: the box gather passes the need on"), OfBox.IsEmpty());
		TestEqual(TEXT("Plane: the conditioner was asked Complete once"), Conditioner.RegionComplete, 1);
		TestEqual(TEXT("Plane: the conditioner was asked Ready twice"), Conditioner.RegionReady, 2);
		TestEqual(TEXT("Plane: the cap gather was not used"), Conditioner.SphereComplete + Conditioner.SphereReady, 0);

		// The chunk manager's own point query asks Complete, and the generated surface is the natural one under a line.
		const FVector Probe(300.0, -450.0, 0.0);
		const double Generated = W.Manager->GetGeneratedSurfacePoint(Probe).Z;
		TestEqual(TEXT("Plane: a point query asks Complete"), Conditioner.RegionComplete, 2);
		TestEqual(TEXT("Plane: the generated surface under a line is the natural one"), Generated, W.Manager->GetSurfacePointAt(Probe).Position.Z, 1e-3);

		W.Manager->SetTerrainConditioner(nullptr);
	}

	// A planet: the cap gathers.
	{
		FLineConditioner Conditioner;
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ConditioningLineGathersPlanet"));
		W.ConfigureSmallPlanet();
		W.StartManager();
		W.Manager->SetTerrainConditioner(&Conditioner);

		const FVector Direction = FVector(1.0, 0.5, 0.25).GetSafeNormal();
		FVoxelConditioning Complete;
		W.Manager->GatherConditioningOnSphere(Direction, 0.05, EVoxelConditioningGather::Complete, Complete);
		TestEqual(TEXT("Planet: a Complete cap gather takes the conditioner's line"), Complete.Lines.Num(), 1);
		TestEqual(TEXT("Planet: a set holding only a line changes no altitude"), Complete.ApplyToAltitude(Direction, W.Config->WorldRadius, -40.25f), -40.25f);

		FVoxelConditioning Ready;
		W.Manager->GatherConditioningOnSphere(Direction, 0.05, EVoxelConditioningGather::Ready, Ready);
		TestTrue(TEXT("Planet: a Ready cap gather takes only what is at hand"), Ready.IsEmpty());

		// A world box on a planet gathers by the cap it subtends.
		const FVector Site = W.Config->WorldOrigin + Direction * W.Config->WorldRadius;
		FVoxelConditioning OfBox;
		W.Manager->GatherConditioning(FBox(Site - FVector(800.0), Site + FVector(800.0)), EVoxelConditioningGather::Complete, OfBox);
		TestEqual(TEXT("Planet: the box gather takes the line through the cap gather"), OfBox.Lines.Num(), 1);
		TestEqual(TEXT("Planet: the conditioner was asked Complete twice"), Conditioner.SphereComplete, 2);
		TestEqual(TEXT("Planet: the conditioner was asked Ready once"), Conditioner.SphereReady, 1);
		TestEqual(TEXT("Planet: the region gather was not used"), Conditioner.RegionComplete + Conditioner.RegionReady, 0);

		W.Manager->SetTerrainConditioner(nullptr);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
