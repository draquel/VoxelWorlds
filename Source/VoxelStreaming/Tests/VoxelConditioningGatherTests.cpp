// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The conditioning at a place and the conditioner's line gathers (Documentation/TERRAIN_LINES_PLAN.md, L1).
//
// - LineGathers: the chunk manager's gathers fill an FVoxelConditioning from the game's conditioner: its zones, and its
//   lines through GatherConditioningLines (an XY region) and GatherConditioningLinesOnSphere (a cap), each asked with the
//   caller's need. A Complete gather gets the conditioner's line, a Ready one what the conditioner has at hand (here
//   none); a line is carried by reference, never copied; the chunk manager's own point queries ask Complete; and a set
//   that holds only a line with no segment changes no height.
// ---------------------------------------------------------------------------

namespace VoxelConditioningGatherTestUtils
{
	/** A conditioner with one line it supplies only to a Complete gather, counting what it was asked. */
	struct FLineConditioner : IVoxelTerrainConditioner
	{
		/** A line with no segment: it is gathered and carried, and conditions nothing. */
		FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(FVoxelConditioningLineParams());
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

// ---------------------------------------------------------------------------
// Lines added to the chunk manager (Documentation/TERRAIN_LINES_PLAN.md, L2).
//
// - LineGathersBruteForce: with a set of lines added, a chunk's gather (an XY box on a plane, the cap its box subtends
//   on a planet) misses no line that a brute force over every segment finds a weight for at a point of the chunk, the
//   gather is not everything, and the generated surface at a point is the natural one with the lines' blends in id
//   order, each from a nearest segment found by trying them all.
// - AddLine: AddConditioningLine from the values a Blueprint passes: the count, the conditioning-changed broadcast, the
//   generated surface on the core, the world report, and the streamed voxels: the edit-merged surface along the core
//   is at the target, on a plane and on a planet. The line is added before streaming starts, so nothing here depends
//   on when a conditioner registers (G187).
// ---------------------------------------------------------------------------

namespace VoxelConditioningGatherTestUtils
{
	/** A line's target and weight at a place, every segment tried (no bounds, no blocks). False when there is no weight. */
	bool BruteEvaluate(const FVoxelConditioningLine& Line, const FVector& Place, double PlanetRadius, float& OutTarget, float& OutWeight)
	{
		const TArray<FVoxelConditioningLineVertex>& V = Line.GetVertices();
		const double Scale = Line.IsRadial() ? PlanetRadius : 1.0;
		const FVector P = Place * Scale;
		double Best = TNumericLimits<double>::Max();
		int32 BestSegment = INDEX_NONE;
		double BestT = 0.0;
		for (int32 Index = 0; Index + 1 < V.Num(); ++Index)
		{
			const FVector A = V[Index].Position * Scale;
			const FVector AB = V[Index + 1].Position * Scale - A;
			const double T = FMath::Clamp(FVector::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0);
			const double Dist = (P - (A + AB * T)).Size();
			if (Dist < Best)
			{
				Best = Dist;
				BestSegment = Index;
				BestT = T;
			}
		}
		if (BestSegment == INDEX_NONE)
		{
			return false;
		}
		const double Dist = Line.IsRadial() ? 2.0 * PlanetRadius * FMath::Asin(FMath::Min(1.0, Best / (2.0 * PlanetRadius))) : Best;
		const float T = static_cast<float>(BestT);
		OutTarget = V[BestSegment].TargetHeight + T * (V[BestSegment + 1].TargetHeight - V[BestSegment].TargetHeight);
		OutWeight = FVoxelConditioningLine::WeightAtDistance(static_cast<float>(Dist),
			V[BestSegment].HalfWidth + T * (V[BestSegment + 1].HalfWidth - V[BestSegment].HalfWidth), Line.GetFalloffWidth(), Line.GetStrength());
		return OutWeight > 0.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineGathersBruteForceTest,
	"VoxelWorlds.Streaming.Conditioning.LineGathersBruteForce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineGathersBruteForceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelConditioningGatherTestUtils;
	using EBlend = EVoxelConditioningLineBlend;

	// A plane: straight lines of every heading and length, their targets a little off the natural ground at each vertex.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ConditioningLineBruteForcePlane"));
		W.ConfigureSmallPlane();
		W.Config->HeightScale = 300.0f;
		W.StartManager();
		const float VoxelSize = W.Config->VoxelSize;
		const double ChunkWorldSize = static_cast<double>(W.Config->ChunkSize) * VoxelSize;

		FRandomStream Random(40213);
		TArray<FVoxelConditioningLineRef> Lines;
		for (int32 Index = 0; Index < 14; ++Index)
		{
			const FVector2D Start(Random.FRandRange(-15000.0f, 15000.0f), Random.FRandRange(-15000.0f, 15000.0f));
			const double Heading = Random.FRandRange(0.0f, 2.0f * UE_PI);
			const double Length = Random.FRandRange(1500.0f, 22000.0f);
			const float Offset = Random.FRandRange(-250.0f, 250.0f);
			FVoxelConditioningLineParams Params;
			Params.FalloffWidth = Random.FRandRange(2.0f, 5.0f) * VoxelSize;
			Params.Blend = static_cast<EBlend>(Index % 3);
			// Ids out of order of adding: the gather sorts.
			Params.Id = (Index * 7) % 14 + 1;
			const float HalfWidth = Random.FRandRange(1.0f, 4.0f) * VoxelSize;
			const int32 Steps = FMath::CeilToInt32(Length / (2.0 * VoxelSize));
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const FVector2D P = Start + FVector2D(FMath::Cos(Heading), FMath::Sin(Heading)) * (Length * Step / Steps);
				Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(P.X, P.Y, 0.0),
					static_cast<float>(W.Manager->GetSurfacePointAt(FVector(P.X, P.Y, 0.0)).Position.Z) + Offset, HalfWidth));
			}
			const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(Params);
			Lines.Add(Line);
			W.Manager->AddConditioningLine(Line);
		}
		TestEqual(TEXT("Plane: the lines were added"), W.Manager->GetNumConditioningLines(), Lines.Num());

		int32 Chunks = 0, Affected = 0, Missed = 0, Gathered = 0, Unsorted = 0;
		for (int32 CY = -12; CY < 12; ++CY)
		{
			for (int32 CX = -12; CX < 12; ++CX)
			{
				const FVector Min(CX * ChunkWorldSize, CY * ChunkWorldSize, -1600.0);
				FVoxelConditioning Out;
				W.Manager->GatherConditioning(FBox(Min, Min + FVector(ChunkWorldSize)), EVoxelConditioningGather::Complete, Out);
				++Chunks;
				Gathered += Out.Lines.Num();
				for (int32 Index = 1; Index < Out.Lines.Num(); ++Index)
				{
					Unsorted += Out.Lines[Index - 1]->GetId() > Out.Lines[Index]->GetId() ? 1 : 0;
				}
				for (const FVoxelConditioningLineRef& Line : Lines)
				{
					bool bWeighs = false;
					for (int32 J = 0; J <= 8 && !bWeighs; ++J)
					{
						for (int32 I = 0; I <= 8 && !bWeighs; ++I)
						{
							float Target = 0.0f, Weight = 0.0f;
							bWeighs = BruteEvaluate(*Line, FVector(Min.X + ChunkWorldSize * I / 8.0, Min.Y + ChunkWorldSize * J / 8.0, 0.0), 0.0, Target, Weight);
						}
					}
					if (bWeighs)
					{
						++Affected;
						Missed += Out.Lines.Contains(Line) ? 0 : 1;
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("Plane: %d chunks, %d (chunk, line) pairs with a weight, %d missed, %d gathered in all (of %d possible)"),
			Chunks, Affected, Missed, Gathered, Chunks * Lines.Num()));
		TestTrue(TEXT("Plane: chunks and lines meet (non-vacuous)"), Affected > 100);
		TestEqual(TEXT("Plane: no line that weighs on a chunk is missed"), Missed, 0);
		TestTrue(TEXT("Plane: the gather is not wildly conservative"), Gathered <= 3 * Affected);
		TestEqual(TEXT("Plane: every gather's lines are in id order"), Unsorted, 0);

		// The generated surface at points: the natural height with each line's blend, in id order, by brute force.
		TArray<FVoxelConditioningLineRef> ById = Lines;
		ById.Sort([](const FVoxelConditioningLineRef& A, const FVoxelConditioningLineRef& B) { return A->GetId() < B->GetId(); });
		int32 Points = 0, Conditioned = 0;
		double Worst = 0.0;
		for (int32 Index = 0; Index < 3000; ++Index)
		{
			// Around a vertex of a line, out to a little past its reach.
			const FVoxelConditioningLine& Near = *Lines[Random.RandRange(0, Lines.Num() - 1)];
			const double Spread = 1.2 * Near.GetReach();
			const FVector P = Near.GetVertices()[Random.RandRange(0, Near.GetVertices().Num() - 1)].Position
				+ FVector(Random.FRandRange(-Spread, Spread), Random.FRandRange(-Spread, Spread), 0.0);
			const float Natural = static_cast<float>(W.Manager->GetSurfacePointAt(P).Position.Z);
			float Expected = Natural;
			for (const FVoxelConditioningLineRef& Line : ById)
			{
				float Target = 0.0f, Weight = 0.0f;
				if (BruteEvaluate(*Line, P, 0.0, Target, Weight))
				{
					Expected = FVoxelConditioningLine::BlendHeight(Line->GetBlend(), Expected, Target, Weight);
				}
			}
			++Points;
			Conditioned += Expected != Natural ? 1 : 0;
			Worst = FMath::Max(Worst, FMath::Abs(W.Manager->GetGeneratedSurfacePoint(P).Z - Expected));
		}
		AddInfo(FString::Printf(TEXT("Plane: %d points, %d under a line; the generated surface is off the brute force by at most %.4f uu"), Points, Conditioned, Worst));
		TestTrue(TEXT("Plane: points lie under lines (non-vacuous)"), Conditioned > 100);
		TestTrue(TEXT("Plane: the generated surface is the brute force's"), Worst < 0.01);
		W.Stop();
	}

	// A planet: arcs around a site, the chunks around it, the cap each chunk's box subtends.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ConditioningLineBruteForcePlanet"));
		W.ConfigureSmallPlanet();
		W.StartManager();
		const FVector Center = W.Config->WorldOrigin;
		const double Radius = W.Config->WorldRadius;
		const float VoxelSize = W.Config->VoxelSize;
		const double ChunkWorldSize = static_cast<double>(W.Config->ChunkSize) * VoxelSize;
		const FVector Site = FVector(1.0, 0.8, 0.5).GetSafeNormal();
		const FVector East = FVector::CrossProduct(FVector::UpVector, Site).GetSafeNormal();
		const FVector North = FVector::CrossProduct(Site, East);

		FRandomStream Random(77101);
		TArray<FVoxelConditioningLineRef> Lines;
		for (int32 Index = 0; Index < 8; ++Index)
		{
			// An arc from a point near the site along a heading, a vertex every 2 voxels.
			const FVector Start = (Site + East * Random.FRandRange(-0.25f, 0.25f) + North * Random.FRandRange(-0.25f, 0.25f)).GetSafeNormal();
			const double Heading = Random.FRandRange(0.0f, 2.0f * UE_PI);
			const FVector Tangent = ((East * FMath::Cos(Heading) + North * FMath::Sin(Heading)) - Start * FVector::DotProduct(East * FMath::Cos(Heading) + North * FMath::Sin(Heading), Start)).GetSafeNormal();
			const double Length = Random.FRandRange(1000.0f, 9000.0f);
			const float Offset = Random.FRandRange(-200.0f, 200.0f);
			FVoxelConditioningLineParams Params;
			Params.bRadial = true;
			Params.FalloffWidth = Random.FRandRange(2.0f, 5.0f) * VoxelSize;
			Params.Blend = static_cast<EBlend>(Index % 3);
			Params.Id = Index + 1;
			const float HalfWidth = Random.FRandRange(1.0f, 4.0f) * VoxelSize;
			const int32 Steps = FMath::CeilToInt32(Length / (2.0 * VoxelSize));
			for (int32 Step = 0; Step <= Steps; ++Step)
			{
				const double Angle = Length * Step / Steps / Radius;
				const FVector Direction = (Start * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal();
				Params.Vertices.Add(FVoxelConditioningLineVertex(Direction, W.Manager->GetSurfacePointAt(Center + Direction * Radius).Altitude + Offset, HalfWidth));
			}
			const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(Params);
			Lines.Add(Line);
			W.Manager->AddConditioningLine(Line);
		}

		const FVector SitePoint = Center + Site * Radius;
		const FIntVector SiteChunk(FMath::FloorToInt32(SitePoint.X / ChunkWorldSize), FMath::FloorToInt32(SitePoint.Y / ChunkWorldSize), FMath::FloorToInt32(SitePoint.Z / ChunkWorldSize));
		int32 Chunks = 0, Affected = 0, Missed = 0, Gathered = 0;
		for (int32 CZ = -5; CZ <= 5; ++CZ)
		{
			for (int32 CY = -5; CY <= 5; ++CY)
			{
				for (int32 CX = -5; CX <= 5; ++CX)
				{
					const FVector Min = FVector(SiteChunk + FIntVector(CX, CY, CZ)) * ChunkWorldSize;
					FVector CapDirection;
					double CapAngle = 0.0;
					if (!W.Manager->GetPlanetCapOfBox(FBox(Min, Min + FVector(ChunkWorldSize)), CapDirection, CapAngle))
					{
						continue;
					}
					FVoxelConditioning Out;
					W.Manager->GatherConditioningOnSphere(CapDirection, CapAngle, EVoxelConditioningGather::Complete, Out);
					++Chunks;
					Gathered += Out.Lines.Num();
					for (const FVoxelConditioningLineRef& Line : Lines)
					{
						bool bWeighs = false;
						for (int32 Z = 0; Z <= 4 && !bWeighs; ++Z)
						{
							for (int32 Y = 0; Y <= 4 && !bWeighs; ++Y)
							{
								for (int32 X = 0; X <= 4 && !bWeighs; ++X)
								{
									float Target = 0.0f, Weight = 0.0f;
									const FVector Direction = (Min + FVector(X, Y, Z) * (ChunkWorldSize / 4.0) - Center).GetSafeNormal();
									bWeighs = BruteEvaluate(*Line, Direction, Radius, Target, Weight);
								}
							}
						}
						if (bWeighs)
						{
							++Affected;
							Missed += Out.Lines.Contains(Line) ? 0 : 1;
						}
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("Planet: %d chunks, %d (chunk, line) pairs with a weight, %d missed, %d gathered in all (of %d possible)"),
			Chunks, Affected, Missed, Gathered, Chunks * Lines.Num()));
		TestTrue(TEXT("Planet: chunks and lines meet (non-vacuous)"), Affected > 100);
		TestEqual(TEXT("Planet: no line that weighs on a chunk is missed"), Missed, 0);
		TestTrue(TEXT("Planet: the gather is not everything"), Gathered < Chunks * Lines.Num());

		// The generated altitude along directions: the natural one with each line's blend in id order, by brute force.
		int32 Points = 0, Conditioned = 0;
		double Worst = 0.0;
		for (int32 Index = 0; Index < 3000; ++Index)
		{
			const FVector Direction = (Site + East * Random.FRandRange(-0.4f, 0.4f) + North * Random.FRandRange(-0.4f, 0.4f)).GetSafeNormal();
			const float Natural = W.Manager->GetSurfacePointAt(Center + Direction * Radius).Altitude;
			float Expected = Natural;
			for (const FVoxelConditioningLineRef& Line : Lines)
			{
				float Target = 0.0f, Weight = 0.0f;
				if (BruteEvaluate(*Line, Direction, Radius, Target, Weight))
				{
					Expected = FVoxelConditioningLine::BlendHeight(Line->GetBlend(), Expected, Target, Weight);
				}
			}
			++Points;
			Conditioned += Expected != Natural ? 1 : 0;
			Worst = FMath::Max(Worst, static_cast<double>(FMath::Abs(W.Manager->GetAltitudeAt(W.Manager->GetGeneratedSurfacePoint(Center + Direction * Radius)) - Expected)));
		}
		AddInfo(FString::Printf(TEXT("Planet: %d directions, %d under a line; the generated altitude is off the brute force by at most %.4f uu"), Points, Conditioned, Worst));
		TestTrue(TEXT("Planet: directions lie under lines (non-vacuous)"), Conditioned > 100);
		TestTrue(TEXT("Planet: the generated altitude is the brute force's"), Worst < 0.02);
		W.Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningAddLineTest,
	"VoxelWorlds.Streaming.Conditioning.AddLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningAddLineTest::RunTest(const FString& Parameters)
{
	// A plane: a level line through a site, added from the values, before streaming starts.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ConditioningAddLinePlane"));
		W.ConfigureSmallPlane();
		W.StartManager();
		const float VoxelSize = W.Config->VoxelSize;
		const FVector2D Site(900.0, -1300.0);
		const float Target = static_cast<float>(W.Manager->GetSurfacePointAt(FVector(Site.X, Site.Y, 0.0)).Position.Z) + 180.0f;
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = 4.0f * VoxelSize;
		Params.Id = 3;
		const FVector2D Step = FVector2D(0.8, 0.6) * (2.0 * VoxelSize);
		for (int32 Index = -15; Index <= 15; ++Index)
		{
			const FVector2D P = Site + Step * Index;
			Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(P.X, P.Y, 0.0), Target, 3.0f * VoxelSize));
		}

		int32 Broadcasts = 0;
		const FDelegateHandle Handle = W.Manager->OnTerrainConditioningChanged.AddLambda([&Broadcasts]() { ++Broadcasts; });
		TestEqual(TEXT("Plane: no line yet"), W.Manager->GetNumConditioningLines(), 0);
		W.Manager->AddConditioningLine(Params);
		TestEqual(TEXT("Plane: the line is counted"), W.Manager->GetNumConditioningLines(), 1);
		TestEqual(TEXT("Plane: adding a line says the conditioning changed"), Broadcasts, 1);
		W.Manager->NotifyTerrainConditioningChanged(FBox2D(Site - FVector2D(500.0), Site + FVector2D(500.0)));
		TestEqual(TEXT("Plane: a conditioner's notification says so too"), Broadcasts, 2);
		W.Manager->OnTerrainConditioningChanged.Remove(Handle);

		TestEqual(TEXT("Plane: the generated surface on the core is the target"), W.Manager->GetGeneratedSurfacePoint(FVector(Site.X, Site.Y, 0.0)).Z, static_cast<double>(Target), 1e-2);
		const FVector Far(Site.X + 4000.0, Site.Y - 4000.0, 0.0);
		TestEqual(TEXT("Plane: far from the line the generated surface is the natural one"),
			W.Manager->GetGeneratedSurfacePoint(Far).Z, W.Manager->GetSurfacePointAt(Far).Position.Z, 1e-3);

		TArray<FString> Report;
		W.Manager->GetWorldReport(FVoxelWorldReportNotes(), Report);
		TestTrue(TEXT("Plane: the world report names the line"),
			Report.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("0 zones and 1 lines (30 segments) added to the world")); }));

		// The streamed voxels: along the core the edit-merged surface is at the target, within a voxel.
		W.SetViewer(W.ViewerOverGround(Site.X, Site.Y, 300.0f));
		if (TestTrue(TEXT("Plane: streaming settles"), W.Settle()))
		{
			int32 Found = 0, OnTarget = 0;
			for (int32 Index = -12; Index <= 12; ++Index)
			{
				for (const double Across : { -100.0, 0.0, 100.0 })
				{
					const FVector2D P = Site + Step * Index + FVector2D(-0.6, 0.8) * Across;
					FVoxelSurfaceSample Hit;
					if (W.Manager->QueryEditMergedSurfacePoint(FVector(P.X, P.Y, Target), Hit))
					{
						++Found;
						OnTarget += FMath::Abs(Hit.Position.Z - Target) < VoxelSize ? 1 : 0;
					}
				}
			}
			AddInfo(FString::Printf(TEXT("Plane: %d of 75 core points loaded, %d within a voxel of the target"), Found, OnTarget));
			TestTrue(TEXT("Plane: the core is loaded (non-vacuous)"), Found >= 60);
			TestEqual(TEXT("Plane: the streamed voxels along the core are at the target"), OnTarget, Found);
			W.Manager->GetWorldReport(FVoxelWorldReportNotes(), Report);
			TestTrue(TEXT("Plane: generation requests carried the line"),
				Report.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("conditioning:")) && !Line.Contains(TEXT(" 0 generation requests carried a line")); }));
		}
		W.Stop();
	}

	// A planet: an arc through a site, its target an altitude.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("ConditioningAddLinePlanet"));
		W.ConfigureSmallPlanet();
		W.StartManager();
		const FVector Center = W.Config->WorldOrigin;
		const double Radius = W.Config->WorldRadius;
		const float VoxelSize = W.Config->VoxelSize;
		const FVector Site = FVector(0.7, 0.6, 0.4).GetSafeNormal();
		const FVector Tangent = FVector::CrossProduct(FVector::UpVector, Site).GetSafeNormal();
		const float Target = W.Manager->GetSurfacePointAt(Center + Site * Radius).Altitude + 180.0f;
		FVoxelConditioningLineParams Params;
		Params.bRadial = true;
		Params.FalloffWidth = 4.0f * VoxelSize;
		const auto AlongLine = [&](double DistanceUU)
		{
			return (Site * FMath::Cos(DistanceUU / Radius) + Tangent * FMath::Sin(DistanceUU / Radius)).GetSafeNormal();
		};
		// A short line: the planet's ground rises and falls by more than the cut and fill cap within a few chunks.
		for (int32 Index = -8; Index <= 8; ++Index)
		{
			Params.Vertices.Add(FVoxelConditioningLineVertex(AlongLine(Index * 2.0 * VoxelSize), Target, 3.0f * VoxelSize));
		}
		W.Manager->AddConditioningLine(Params);
		TestEqual(TEXT("Planet: the line is counted"), W.Manager->GetNumConditioningLines(), 1);
		TestEqual(TEXT("Planet: the generated altitude on the core is the target"),
			W.Manager->GetAltitudeAt(W.Manager->GetGeneratedSurfacePoint(Center + Site * Radius)), Target, 1e-2f);

		W.SetViewer(Center + Site * (Radius + Target + 300.0));
		if (TestTrue(TEXT("Planet: streaming settles"), W.Settle()))
		{
			int32 Found = 0, OnTarget = 0;
			for (int32 Index = -12; Index <= 12; ++Index)
			{
				FVoxelSurfaceSample Hit;
				if (W.Manager->QueryEditMergedSurfacePoint(Center + AlongLine(Index * VoxelSize) * (Radius + Target), Hit))
				{
					++Found;
					OnTarget += FMath::Abs(W.Manager->GetAltitudeAt(Hit.Position) - Target) < VoxelSize ? 1 : 0;
				}
			}
			AddInfo(FString::Printf(TEXT("Planet: %d of 25 core points loaded, %d within a voxel of the target"), Found, OnTarget));
			TestTrue(TEXT("Planet: the core is loaded (non-vacuous)"), Found >= 20);
			TestEqual(TEXT("Planet: the streamed voxels along the core are at the target"), OnTarget, Found);
		}
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
