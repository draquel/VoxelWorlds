// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Components/SplineComponent.h"
#include "VoxelChunkManager.h"
#include "VoxelConditioningLine.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelTerrainLineComponent.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The hand-placed line (Documentation/TERRAIN_LINES_PLAN.md, L4): UVoxelTerrainLineComponent turns a spline on its actor
// into a conditioning line of the world, registered before the chunk manager's first generation request.
//
// - Params: what the component samples from a spline on a plane and on a planet: a vertex every two voxels along it, the
//   two height modes (the natural terrain along the line with its offset, averaging and grade; the spline's own height),
//   the half width from each spline point's scale, the line's own values, an id that is stable and ordered by Order; and
//   no line without a spline.
// - RegistersBeforeFirstRequest: the component registers when the voxel world starts, whichever of the two begins first,
//   before any generation request is built, and the world's first chunks generate with the line (the streamed voxels on
//   the core are at the target). A component that begins play after chunks have generated is the case the guarantee
//   prevents: the chunk manager counts it and warns, and those chunks keep the natural terrain.
// - Warnings: a spline with a sharp corner on a grade is added with the bend bound's warning, which the component keeps.
// ---------------------------------------------------------------------------

namespace VoxelTerrainLineTestUtils
{
	/** A terrain line actor whose spline runs straight through the points given (world space). */
	AVoxelTerrainLineActor* SpawnLine(UWorld* World, const TArray<FVector>& Points)
	{
		AVoxelTerrainLineActor* Actor = World->SpawnActor<AVoxelTerrainLineActor>();
		USplineComponent* Spline = Actor->GetSpline();
		Spline->SetSplinePoints(Points, ESplineCoordinateSpace::World, false);
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			Spline->SetSplinePointType(Index, ESplinePointType::Linear, false);
		}
		Spline->UpdateSpline();
		return Actor;
	}

	/** The points of a straight run through a site on a plane, the spline's own Z rising along it. */
	TArray<FVector> PlaneRun(const FVector2D& Site, const FVector2D& Direction, double HalfLength, double Z0, double Z1)
	{
		const FVector2D A = Site - Direction * HalfLength;
		const FVector2D B = Site + Direction * HalfLength;
		return { FVector(A.X, A.Y, Z0), FVector(B.X, B.Y, Z1) };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTerrainLineParamsTest,
	"VoxelWorlds.Streaming.TerrainLine.Params",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTerrainLineParamsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTerrainLineTestUtils;

	// A plane.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("TerrainLineParamsPlane"));
		W.ConfigureSmallPlane();
		W.StartManager();
		const float VoxelSize = W.Config->VoxelSize;
		const FVector2D Site(900.0, -1300.0);
		const FVector2D Direction(0.8, 0.6);
		const double HalfLength = 1500.0;
		AVoxelTerrainLineActor* Actor = SpawnLine(W.World, PlaneRun(Site, Direction, HalfLength, 5000.0, 5300.0));
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		TestTrue(TEXT("The component finds the actor's spline"), Line->GetSpline() == Actor->GetSpline());
		Line->HalfWidth = 3.0f * VoxelSize;
		Line->FalloffWidth = 4.0f * VoxelSize;
		Line->Strength = 0.75f;
		Line->Blend = EVoxelConditioningLineBlend::LowerOnly;
		Line->Surface.BandMaterialID = 5;
		Line->Surface.MinCaveDepth = 8.0f;
		Line->Surface.ClearHalfWidth = 5.0f * VoxelSize;
		// The spline's scale: 1 at the first point, 2 at the second.
		Actor->GetSpline()->SetScaleAtSplinePoint(1, FVector(1.0, 2.0, 1.0));

		const auto Natural = [&](const FVector& Position)
		{
			return static_cast<float>(W.Manager->GetSurfacePointAt(FVector(Position.X, Position.Y, 0.0)).Position.Z);
		};

		// Following the terrain as it is: no averaging, no grade limit, an offset.
		Line->HeightMode = EVoxelTerrainLineHeight::FollowTerrain;
		Line->SmoothingDistance = 0.0f;
		Line->MaxGrade = 0.0f;
		Line->TerrainOffset = 120.0f;
		FVoxelConditioningLineParams Params;
		if (TestTrue(TEXT("Plane: a line is made"), Line->BuildLineParams(*W.Manager, Params)))
		{
			// Along the spline, which rises 300 uu over its 3,000: 3,015 uu of spline, so 31 steps of a little under two
			// voxels (never more).
			const double SplineLength = FMath::Sqrt(FMath::Square(2.0 * HalfLength) + FMath::Square(300.0));
			const int32 Expected = FMath::CeilToInt32(SplineLength / (2.0 * VoxelSize)) + 1;
			const double ExpectedSpacing = 2.0 * HalfLength / (Expected - 1);
			TestEqual(TEXT("Plane: a vertex every two voxels along the spline"), Params.Vertices.Num(), Expected);
			TestFalse(TEXT("Plane: a heightmap line"), Params.bRadial);
			double WorstOffLine = 0.0, WorstTarget = 0.0, WorstSpacing = 0.0;
			for (int32 Index = 0; Index < Params.Vertices.Num(); ++Index)
			{
				const FVoxelConditioningLineVertex& Vertex = Params.Vertices[Index];
				const FVector2D P(Vertex.Position.X, Vertex.Position.Y);
				const FVector2D FromSite = P - Site;
				WorstOffLine = FMath::Max(WorstOffLine, FMath::Abs(FVector2D::CrossProduct(FromSite, Direction)));
				WorstTarget = FMath::Max(WorstTarget, FMath::Abs(static_cast<double>(Vertex.TargetHeight) - (Natural(Vertex.Position) + 120.0)));
				if (Index > 0)
				{
					const FVector2D Previous(Params.Vertices[Index - 1].Position.X, Params.Vertices[Index - 1].Position.Y);
					WorstSpacing = FMath::Max(WorstSpacing, FMath::Abs(FVector2D::Distance(P, Previous) - ExpectedSpacing));
				}
			}
			TestTrue(TEXT("Plane: the vertices lie on the spline"), WorstOffLine < 0.1);
			TestTrue(TEXT("Plane: the vertices are evenly spaced, two voxels apart at most"), WorstSpacing < 0.1 && ExpectedSpacing <= 2.0 * VoxelSize);
			TestTrue(TEXT("Plane: following the terrain, the target is the natural height plus the offset"), WorstTarget < 0.01);
			TestEqual(TEXT("Plane: the first vertex is the spline's start"), FVector2D(Params.Vertices[0].Position.X, Params.Vertices[0].Position.Y), Site - Direction * HalfLength);
			TestEqual(TEXT("Plane: the last vertex is the spline's end"), FVector2D(Params.Vertices.Last().Position.X, Params.Vertices.Last().Position.Y), Site + Direction * HalfLength);

			// The half width follows the spline's scale, from 1 at the start to 2 at the end.
			TestEqual(TEXT("Plane: the half width at the start is the component's"), Params.Vertices[0].HalfWidth, 3.0f * VoxelSize, 0.01f);
			TestEqual(TEXT("Plane: at a point scaled by 2 it is twice that"), Params.Vertices.Last().HalfWidth, 6.0f * VoxelSize, 0.01f);
			bool bMonotone = true;
			for (int32 Index = 1; Index < Params.Vertices.Num(); ++Index)
			{
				bMonotone &= Params.Vertices[Index].HalfWidth >= Params.Vertices[Index - 1].HalfWidth - 1e-3f;
			}
			TestTrue(TEXT("Plane: and widens between them"), bMonotone);

			// The line's own values.
			TestEqual(TEXT("Plane: the falloff"), Params.FalloffWidth, 4.0f * VoxelSize);
			TestEqual(TEXT("Plane: the strength"), Params.Strength, 0.75f);
			TestTrue(TEXT("Plane: the blend"), Params.Blend == EVoxelConditioningLineBlend::LowerOnly);
			TestEqual(TEXT("Plane: the band"), Params.Surface.BandMaterialID, 5);
			TestEqual(TEXT("Plane: the cave depth"), Params.Surface.MinCaveDepth, 8.0f);
			TestEqual(TEXT("Plane: the clear width"), Params.Surface.ClearHalfWidth, 5.0f * VoxelSize);
			TestEqual(TEXT("Plane: the id is the component's"), Params.Id, Line->GetLineId());
		}

		// Averaged along the line and held to a grade: no step steeper than the grade, and the targets stay near the ground.
		Line->SmoothingDistance = 600.0f;
		Line->MaxGrade = 0.05f;
		Line->TerrainOffset = 0.0f;
		FVoxelConditioningLineParams Graded;
		if (TestTrue(TEXT("Plane: a graded line is made"), Line->BuildLineParams(*W.Manager, Graded)))
		{
			float SteepestTarget = 0.0f, SteepestNatural = 0.0f;
			for (int32 Index = 1; Index < Graded.Vertices.Num(); ++Index)
			{
				SteepestTarget = FMath::Max(SteepestTarget, FMath::Abs(Graded.Vertices[Index].TargetHeight - Graded.Vertices[Index - 1].TargetHeight));
				SteepestNatural = FMath::Max(SteepestNatural, FMath::Abs(Natural(Graded.Vertices[Index].Position) - Natural(Graded.Vertices[Index - 1].Position)));
			}
			const float Spacing = 2.0f * VoxelSize;
			AddInfo(FString::Printf(TEXT("Plane: the ground's steepest step along the line %.1f %%, the graded target's %.1f %%"),
				100.0f * SteepestNatural / Spacing, 100.0f * SteepestTarget / Spacing));
			TestTrue(TEXT("Plane: the ground along the line is steeper than the grade somewhere (non-vacuous)"), SteepestNatural > 0.05f * Spacing * 1.2f);
			TestTrue(TEXT("Plane: no step of the target is steeper than the grade"), SteepestTarget <= 0.05f * Spacing + 1e-3f);
		}

		// Averaged with no grade limit: the average narrows toward the ends, where the line meets the ground, and is the
		// mean of the ground over the whole distance each way in the middle.
		Line->MaxGrade = 0.0f;
		FVoxelConditioningLineParams Averaged;
		if (TestTrue(TEXT("Plane: an averaged line is made"), Line->BuildLineParams(*W.Manager, Averaged)))
		{
			const int32 Num = Averaged.Vertices.Num();
			TestEqual(TEXT("Plane: the averaged line starts on the ground"), Averaged.Vertices[0].TargetHeight, Natural(Averaged.Vertices[0].Position), 0.01f);
			TestEqual(TEXT("Plane: and ends on it"), Averaged.Vertices.Last().TargetHeight, Natural(Averaged.Vertices.Last().Position), 0.01f);
			// 600 uu each way at a vertex every 96.8 uu along the ground (97.3 along the spline): six vertices each way.
			const int32 Middle = Num / 2;
			double Sum = 0.0;
			for (int32 Other = Middle - 6; Other <= Middle + 6; ++Other)
			{
				Sum += Natural(Averaged.Vertices[Other].Position);
			}
			TestEqual(TEXT("Plane: in the middle the target is the ground's mean over the distance each way"), Averaged.Vertices[Middle].TargetHeight, static_cast<float>(Sum / 13.0), 0.01f);
			TestTrue(TEXT("Plane: which is not the ground there (non-vacuous)"), FMath::Abs(Averaged.Vertices[Middle].TargetHeight - Natural(Averaged.Vertices[Middle].Position)) > 1.0f);
		}

		// The spline's own height: from 5,000 at the start to 5,300 at the end.
		Line->HeightMode = EVoxelTerrainLineHeight::SplineHeight;
		FVoxelConditioningLineParams Own;
		if (TestTrue(TEXT("Plane: a line at the spline's height is made"), Line->BuildLineParams(*W.Manager, Own)))
		{
			TestEqual(TEXT("Plane: the spline's height at the start"), Own.Vertices[0].TargetHeight, 5000.0f, 0.01f);
			TestEqual(TEXT("Plane: at the end"), Own.Vertices.Last().TargetHeight, 5300.0f, 0.01f);
			double Worst = 0.0;
			for (const FVoxelConditioningLineVertex& Vertex : Own.Vertices)
			{
				const double Along = FVector2D::DotProduct(FVector2D(Vertex.Position.X, Vertex.Position.Y) - (Site - Direction * HalfLength), Direction) / (2.0 * HalfLength);
				Worst = FMath::Max(Worst, FMath::Abs(Vertex.TargetHeight - (5000.0 + 300.0 * Along)));
			}
			TestTrue(TEXT("Plane: and the spline's height at every vertex between"), Worst < 0.5);
		}

		// The id: stable, the order in its high half, different for another actor.
		AVoxelTerrainLineActor* Other = SpawnLine(W.World, PlaneRun(Site, Direction, 500.0, 0.0, 0.0));
		const int64 Id = Line->GetLineId();
		TestEqual(TEXT("The id is stable"), Line->GetLineId(), Id);
		TestTrue(TEXT("The id of an order-0 line fits in its low half"), Id >= 0 && Id <= static_cast<int64>(MAX_uint32));
		TestNotEqual(TEXT("Another actor's line has another id"), Other->GetTerrainLine()->GetLineId(), Id);
		Other->GetTerrainLine()->Order = 1;
		TestTrue(TEXT("A higher order is a higher id, whatever the names"), Other->GetTerrainLine()->GetLineId() > MAX_uint32);
		Line->Order = -1;
		TestTrue(TEXT("A negative order comes before every order-0 line"), Line->GetLineId() < 0);

		// No spline, no line.
		AActor* Bare = W.World->SpawnActor<AActor>();
		UVoxelTerrainLineComponent* Alone = NewObject<UVoxelTerrainLineComponent>(Bare);
		Alone->RegisterComponent();
		FVoxelConditioningLineParams None;
		TestFalse(TEXT("An actor with no spline makes no line"), Alone->BuildLineParams(*W.Manager, None));
		Line->SplineName = TEXT("NoSuchSpline");
		TestFalse(TEXT("A spline name the actor does not have makes no line"), Line->BuildLineParams(*W.Manager, None));
		W.Stop();
	}

	// A planet: the vertices are directions, the targets altitudes.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("TerrainLineParamsPlanet"));
		W.ConfigureSmallPlanet();
		W.StartManager();
		const FVector Center = W.Config->WorldOrigin;
		const double Radius = W.Config->WorldRadius;
		const float VoxelSize = W.Config->VoxelSize;
		const FVector Site = FVector(0.7, 0.6, 0.4).GetSafeNormal();
		const FVector Tangent = FVector::CrossProduct(FVector::UpVector, Site).GetSafeNormal();
		const auto Along = [&](double DistanceUU)
		{
			return (Site * FMath::Cos(DistanceUU / Radius) + Tangent * FMath::Sin(DistanceUU / Radius)).GetSafeNormal();
		};
		// The spline 400 uu over the datum at its start and 700 at its end.
		AVoxelTerrainLineActor* Actor = SpawnLine(W.World, { Center + Along(-800.0) * (Radius + 400.0), Center + Along(800.0) * (Radius + 700.0) });
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		Line->HalfWidth = 3.0f * VoxelSize;
		Line->FalloffWidth = 4.0f * VoxelSize;
		Line->HeightMode = EVoxelTerrainLineHeight::SplineHeight;
		FVoxelConditioningLineParams Params;
		if (TestTrue(TEXT("Planet: a line is made"), Line->BuildLineParams(*W.Manager, Params)))
		{
			TestTrue(TEXT("Planet: a planet line"), Params.bRadial);
			double WorstLength = 0.0, WorstOffArc = 0.0;
			const FVector ArcNormal = FVector::CrossProduct(Along(-800.0), Along(800.0)).GetSafeNormal();
			for (const FVoxelConditioningLineVertex& Vertex : Params.Vertices)
			{
				WorstLength = FMath::Max(WorstLength, FMath::Abs(Vertex.Position.Size() - 1.0));
				WorstOffArc = FMath::Max(WorstOffArc, FMath::Abs(FVector::DotProduct(Vertex.Position, ArcNormal)) * Radius);
			}
			TestTrue(TEXT("Planet: the vertices are unit directions"), WorstLength < 1e-9);
			TestTrue(TEXT("Planet: on the great circle under the spline"), WorstOffArc < 0.1);
			TestTrue(TEXT("Planet: the first direction is the spline's start"), Params.Vertices[0].Position.Equals(Along(-800.0), 1e-9));
			TestEqual(TEXT("Planet: the spline's altitude at the start"), Params.Vertices[0].TargetHeight, 400.0f, 0.05f);
			TestEqual(TEXT("Planet: at the end"), Params.Vertices.Last().TargetHeight, 700.0f, 0.05f);
		}

		Line->HeightMode = EVoxelTerrainLineHeight::FollowTerrain;
		Line->SmoothingDistance = 0.0f;
		Line->MaxGrade = 0.0f;
		Line->TerrainOffset = -90.0f;
		FVoxelConditioningLineParams Follow;
		if (TestTrue(TEXT("Planet: a line following the terrain is made"), Line->BuildLineParams(*W.Manager, Follow)))
		{
			double Worst = 0.0;
			for (const FVoxelConditioningLineVertex& Vertex : Follow.Vertices)
			{
				const float Natural = W.Manager->GetSurfacePointAt(Center + Vertex.Position * Radius).Altitude;
				Worst = FMath::Max(Worst, FMath::Abs(static_cast<double>(Vertex.TargetHeight) - (Natural - 90.0)));
			}
			TestTrue(TEXT("Planet: following the terrain, the target is the natural altitude plus the offset"), Worst < 0.01);
		}
		W.Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTerrainLineRegistersBeforeFirstRequestTest,
	"VoxelWorlds.Streaming.TerrainLine.RegistersBeforeFirstRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTerrainLineRegistersBeforeFirstRequestTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTerrainLineTestUtils;
	const FVector2D Site(900.0, -1300.0);
	const FVector2D Direction(0.8, 0.6);

	// A causeway 180 uu over the ground, three voxels to each side: the conditioned surface is over three voxels from
	// the natural one, so a core at the target is a core that generated with the line.
	const auto Configure = [](UVoxelTerrainLineComponent* Line, float VoxelSize)
	{
		Line->HalfWidth = 3.0f * VoxelSize;
		Line->FalloffWidth = 4.0f * VoxelSize;
		Line->HeightMode = EVoxelTerrainLineHeight::FollowTerrain;
		Line->SmoothingDistance = 1200.0f;
		Line->MaxGrade = 0.08f;
		Line->TerrainOffset = 180.0f;
	};

	// The loaded points of the core: how many there are, how many lie within a voxel of the line's target, how many
	// within a voxel of the natural ground, and how many are places where the two are over a voxel and a half apart (a
	// place that tells a chunk generated with the line from one generated without it).
	struct FCoreCount
	{
		int32 Found = 0;
		int32 OnTarget = 0;
		int32 AtNatural = 0;
		int32 Telling = 0;
	};
	const auto CountCore = [&](VoxelTesting::FTestChunkWorld& W, const FVoxelConditioningLine& Line)
	{
		FCoreCount Count;
		const float VoxelSize = W.Config->VoxelSize;
		for (int32 Index = -12; Index <= 12; ++Index)
		{
			for (const double Across : { -100.0, 0.0, 100.0 })
			{
				const FVector2D P = Site + Direction * (Index * 2.0 * VoxelSize) + FVector2D(-Direction.Y, Direction.X) * Across;
				float Target = 0.0f, Weight = 0.0f;
				if (!Line.Evaluate(P.X, P.Y, Target, Weight) || Weight < 1.0f)
				{
					continue;
				}
				FVoxelSurfaceSample Hit;
				if (W.Manager->QueryEditMergedSurfacePoint(FVector(P.X, P.Y, Target), Hit))
				{
					const double Natural = W.Manager->GetSurfacePointAt(FVector(P.X, P.Y, 0.0)).Position.Z;
					++Count.Found;
					Count.OnTarget += FMath::Abs(Hit.Position.Z - Target) < VoxelSize ? 1 : 0;
					Count.AtNatural += FMath::Abs(Hit.Position.Z - Natural) < VoxelSize ? 1 : 0;
					Count.Telling += FMath::Abs(Natural - Target) > 1.5 * VoxelSize ? 1 : 0;
				}
			}
		}
		return Count;
	};

	// 1. The line's actor begins play before the voxel world starts: it registers when the chunk manager announces the
	//    end of its Initialize, and the first chunks generate with it.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("TerrainLineBeforeWorld"));
		W.ConfigureSmallPlane();
		W.SpawnManager();
		AVoxelTerrainLineActor* Actor = SpawnLine(W.World, PlaneRun(Site, Direction, 1500.0, 0.0, 0.0));
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		Configure(Line, W.Config->VoxelSize);
		Actor->DispatchBeginPlay();
		TestFalse(TEXT("Before the world: nothing to register with yet"), Line->IsLineRegistered());

		W.InitializeManager();
		TestTrue(TEXT("Before the world: registered at the end of the chunk manager's Initialize"), Line->IsLineRegistered());
		TestEqual(TEXT("Before the world: the manager holds the line"), W.Manager->GetNumConditioningLines(), 1);
		TestEqual(TEXT("Before the world: no generation request was built before it"), W.Manager->GetNumGenerationRequestsBuilt(), static_cast<int64>(0));
		TestEqual(TEXT("Before the world: the line keeps the limits"), Line->GetLineWarnings().Num(), 0);

		if (Line->IsLineRegistered())
		{
			const FVoxelConditioningLine& Registered = *Line->GetRegisteredLine();
			TestEqual(TEXT("Before the world: the registered line carries the component's id"), Registered.GetId(), Line->GetLineId());
			W.SetViewer(W.ViewerOverGround(Site.X, Site.Y, 300.0f));
			if (TestTrue(TEXT("Before the world: streaming settles"), W.Settle()))
			{
				const FCoreCount Core = CountCore(W, Registered);
				AddInfo(FString::Printf(TEXT("Before the world: %lld generation requests built, %d core points loaded (%d where the target is over 1.5 voxels from the ground), %d within a voxel of the target"),
					W.Manager->GetNumGenerationRequestsBuilt(), Core.Found, Core.Telling, Core.OnTarget));
				TestTrue(TEXT("Before the world: chunks were requested (non-vacuous)"), W.Manager->GetNumGenerationRequestsBuilt() > 0);
				TestTrue(TEXT("Before the world: the core is loaded, most of it where the line moves the ground (non-vacuous)"), Core.Found >= 60 && Core.Telling >= 40);
				TestEqual(TEXT("Before the world: every loaded point of the core generated with the line"), Core.OnTarget, Core.Found);
				TestEqual(TEXT("Before the world: no source registered late"), W.Manager->GetNumLateConditioningSources(), 0);
				TArray<FString> Report;
				W.Manager->GetWorldReport(FVoxelWorldReportNotes(), Report);
				TestTrue(TEXT("Before the world: the world report says so"),
					Report.ContainsByPredicate([](const FString& Text) { return Text.Contains(TEXT("every source registered before the first generation request")); }));
			}
		}
		W.Stop();
	}

	// 2. The voxel world starts first (its actor began play before the line's): the component registers at its own
	//    BeginPlay, still before any tick.
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("TerrainLineAfterWorld"));
		W.ConfigureSmallPlane();
		W.StartManager();
		AVoxelTerrainLineActor* Actor = SpawnLine(W.World, PlaneRun(Site, Direction, 1500.0, 0.0, 0.0));
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		Configure(Line, W.Config->VoxelSize);
		TestFalse(TEXT("After the world: not registered before it begins play"), Line->IsLineRegistered());
		Actor->DispatchBeginPlay();
		TestTrue(TEXT("After the world: registered at BeginPlay"), Line->IsLineRegistered());
		TestEqual(TEXT("After the world: no generation request was built before it"), W.Manager->GetNumGenerationRequestsBuilt(), static_cast<int64>(0));
		if (Line->IsLineRegistered())
		{
			W.SetViewer(W.ViewerOverGround(Site.X, Site.Y, 300.0f));
			if (TestTrue(TEXT("After the world: streaming settles"), W.Settle()))
			{
				const FCoreCount Core = CountCore(W, *Line->GetRegisteredLine());
				TestTrue(TEXT("After the world: the core is loaded, most of it where the line moves the ground (non-vacuous)"), Core.Found >= 60 && Core.Telling >= 40);
				TestEqual(TEXT("After the world: every loaded point of the core generated with the line"), Core.OnTarget, Core.Found);
				TestEqual(TEXT("After the world: no source registered late"), W.Manager->GetNumLateConditioningSources(), 0);
			}
		}
		TestEqual(TEXT("After the world: the manager holds the one line"), W.Manager->GetNumConditioningLines(), 1);
		W.Stop();
	}

	// 3. What the guarantee prevents: a line that arrives after chunks have generated. The chunk manager counts it and
	//    warns, and the chunks already generated keep the natural terrain.
	{
		AddExpectedMessagePlain(TEXT("A conditioning line was registered after this world had built"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 1);
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("TerrainLineLate"));
		W.ConfigureSmallPlane();
		W.StartManager();
		W.SetViewer(W.ViewerOverGround(Site.X, Site.Y, 300.0f));
		if (TestTrue(TEXT("Late: streaming settles"), W.Settle()))
		{
			const int64 Built = W.Manager->GetNumGenerationRequestsBuilt();
			AVoxelTerrainLineActor* Actor = SpawnLine(W.World, PlaneRun(Site, Direction, 1500.0, 0.0, 0.0));
			UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
			Configure(Line, W.Config->VoxelSize);
			Actor->DispatchBeginPlay();
			TestTrue(TEXT("Late: the line is still added"), Line->IsLineRegistered());
			TestTrue(TEXT("Late: chunks had been requested (non-vacuous)"), Built > 0);
			TestEqual(TEXT("Late: the chunk manager counts the late source"), W.Manager->GetNumLateConditioningSources(), 1);
			if (Line->IsLineRegistered())
			{
				W.Settle();
				const FCoreCount Core = CountCore(W, *Line->GetRegisteredLine());
				AddInfo(FString::Printf(TEXT("Late: registered after %lld generation requests; %d core points loaded, %d at the natural ground, %d at the target"),
					Built, Core.Found, Core.AtNatural, Core.OnTarget));
				TestTrue(TEXT("Late: the core is loaded, most of it where the line would move the ground (non-vacuous)"), Core.Found >= 60 && Core.Telling >= 40);
				TestEqual(TEXT("Late: the chunks generated before the line keep the natural terrain"), Core.AtNatural, Core.Found);
				TestTrue(TEXT("Late: and are not at the line's target where the two differ"), Core.OnTarget <= Core.Found - Core.Telling);
				TArray<FString> Report;
				W.Manager->GetWorldReport(FVoxelWorldReportNotes(), Report);
				TestTrue(TEXT("Late: the world report names it"),
					Report.ContainsByPredicate([](const FString& Text) { return Text.Contains(TEXT("1 source(s) REGISTERED LATE")); }));
			}
		}
		W.Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTerrainLineWarningsTest,
	"VoxelWorlds.Streaming.TerrainLine.Warnings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTerrainLineWarningsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTerrainLineTestUtils;
	// A spline with a right-angle corner, at its own height on a 20 % grade: every vertex is on a straight leg but the
	// corner's, which turns far more than the bend bound allows.
	AddExpectedMessagePlain(TEXT("that their bisector meets a neighbour's inside the reach"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("break the bend bound"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("TerrainLineWarnings"));
	W.ConfigureSmallPlane();
	W.StartManager();
	const float Ground = static_cast<float>(W.Manager->GetSurfacePointAt(FVector(0.0, 0.0, 0.0)).Position.Z);
	AVoxelTerrainLineActor* Actor = SpawnLine(W.World, { FVector(-1000.0, 0.0, Ground - 100.0), FVector(0.0, 0.0, Ground + 100.0), FVector(0.0, 1000.0, Ground + 300.0) });
	UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
	Line->HalfWidth = 3.0f * W.Config->VoxelSize;
	Line->FalloffWidth = 4.0f * W.Config->VoxelSize;
	Line->HeightMode = EVoxelTerrainLineHeight::SplineHeight;
	Actor->DispatchBeginPlay();
	TestTrue(TEXT("A line that breaks the bend bound is still added"), Line->IsLineRegistered() && W.Manager->GetNumConditioningLines() == 1);
	const TArray<FString>& Warnings = Line->GetLineWarnings();
	for (const FString& Warning : Warnings)
	{
		AddInfo(Warning);
	}
	TestTrue(TEXT("The component keeps the bend bound's warning"),
		Warnings.ContainsByPredicate([](const FString& Text) { return Text.Contains(TEXT("break the bend bound")); }));
	TestTrue(TEXT("And the sharp turn's"),
		Warnings.ContainsByPredicate([](const FString& Text) { return Text.Contains(TEXT("bisector meets a neighbour's inside the reach")); }));
	TestEqual(TEXT("Nothing else is wrong with the line"), Warnings.Num(), 2);
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
