// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Components/SplineComponent.h"
#include "IVoxelWorldMode.h"
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
// - Bends (L5, D134): a spline that keeps the bend bound makes the line L4 made, vertex for vertex (the arithmetic is kept
//   here as the record); a corner that breaks it is rounded, the line then steps by no more than the bound across any
//   bisector and stays within the bound's radius of its spline, and two worlds make the same line; at the spline's
//   height the rounded line keeps the spline's heights along it; on a planet the vertices stay directions.
// - Warnings: what the component says of a spline that breaks the bend bound: how far the rounded line runs from it,
//   and, for a corner at the spline's height on a grade too steep to round within the bound, the bend bound's warning.
// ---------------------------------------------------------------------------

namespace VoxelTerrainLineTestUtils
{
	/** Run a spline straight through the points given (world space). */
	void SetSplineThrough(USplineComponent* Spline, const TArray<FVector>& Points)
	{
		Spline->SetSplinePoints(Points, ESplineCoordinateSpace::World, false);
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			Spline->SetSplinePointType(Index, ESplinePointType::Linear, false);
		}
		Spline->UpdateSpline();
	}

	/** A terrain line actor whose spline runs straight through the points given (world space). */
	AVoxelTerrainLineActor* SpawnLine(UWorld* World, const TArray<FVector>& Points)
	{
		AVoxelTerrainLineActor* Actor = World->SpawnActor<AVoxelTerrainLineActor>();
		SetSplineThrough(Actor->GetSpline(), Points);
		return Actor;
	}

	/** The points of a straight run through a site on a plane, the spline's own Z rising along it. */
	TArray<FVector> PlaneRun(const FVector2D& Site, const FVector2D& Direction, double HalfLength, double Z0, double Z1)
	{
		const FVector2D A = Site - Direction * HalfLength;
		const FVector2D B = Site + Direction * HalfLength;
		return { FVector(A.X, A.Y, Z0), FVector(B.X, B.Y, Z1) };
	}

	/** A unit vector in the XY plane, Degrees from +X toward +Y. */
	FVector2D Heading(double Degrees)
	{
		return FVector2D(FMath::Cos(FMath::DegreesToRadians(Degrees)), FMath::Sin(FMath::DegreesToRadians(Degrees)));
	}

	/**
	 * The points of two straight legs that meet at a corner on a plane: the first arrives along ArriveDegrees, the second
	 * leaves TurnDegrees to its left, each Leg long. The spline's own Z is Z0 at the start, Z1 at the corner, Z2 at the end.
	 */
	TArray<FVector> PlaneCorner(const FVector2D& Corner, double ArriveDegrees, double TurnDegrees, double Leg, double Z0 = 0.0, double Z1 = 0.0, double Z2 = 0.0)
	{
		const FVector2D A = Corner - Heading(ArriveDegrees) * Leg;
		const FVector2D B = Corner + Heading(ArriveDegrees + TurnDegrees) * Leg;
		return { FVector(A.X, A.Y, Z0), FVector(Corner.X, Corner.Y, Z1), FVector(B.X, B.Y, Z2) };
	}

	/**
	 * The line the component made of a spline on a heightmap world before it looked at bends (L4): the spline sampled
	 * every VertexSpacingVoxels voxels along its length, and following the terrain the natural height averaged over the
	 * smoothing distance each way (less toward the ends), held to the grade from both ends, plus the offset. Kept here,
	 * as it was, as the record of "the line L4 made": a spline that keeps the bend bound must still make exactly this.
	 */
	FVoxelConditioningLineParams SampledLine(const UVoxelTerrainLineComponent& Line, const UVoxelChunkManager& Manager)
	{
		const USplineComponent* Spline = Line.GetSpline();
		const UVoxelWorldConfiguration* Configuration = Manager.GetConfiguration();
		const IVoxelWorldMode* WorldMode = Manager.GetWorldMode();
		const FVoxelNoiseParams NoiseParams = Configuration->GetNoiseParams();
		const double Length = Spline->GetSplineLength();
		const double Spacing = FMath::Max(Line.VertexSpacingVoxels, 0.25f) * Configuration->VoxelSize;
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / Spacing - 0.01));
		const double Step = Length / Steps;
		TArray<FVector> Positions;
		TArray<float> SplineHeights;
		TArray<float> Natural;
		TArray<float> HalfWidths;
		for (int32 Index = 0; Index <= Steps; ++Index)
		{
			const float Distance = static_cast<float>(FMath::Min(Index * Step, Length));
			const FVector World = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
			Positions.Add(FVector(World.X, World.Y, 0.0));
			SplineHeights.Add(static_cast<float>(World.Z));
			Natural.Add(static_cast<float>(WorldMode->GetSurfaceOnUpLine(FVector(World.X, World.Y, 0.0), NoiseParams).Position.Z));
			HalfWidths.Add(Line.HalfWidth * FMath::Abs(static_cast<float>(Spline->GetScaleAtDistanceAlongSpline(Distance).Y)));
		}
		TArray<float> Targets;
		if (Line.HeightMode == EVoxelTerrainLineHeight::SplineHeight)
		{
			Targets = MoveTemp(SplineHeights);
		}
		else
		{
			const int32 Window = FMath::Max(0, FMath::RoundToInt32(Line.SmoothingDistance / Step));
			Targets.SetNumUninitialized(Natural.Num());
			for (int32 Index = 0; Index < Natural.Num(); ++Index)
			{
				const int32 Reach = FMath::Min3(Window, Index, Natural.Num() - 1 - Index);
				double Sum = 0.0;
				for (int32 Other = Index - Reach; Other <= Index + Reach; ++Other)
				{
					Sum += Natural[Other];
				}
				Targets[Index] = static_cast<float>(Sum / (2 * Reach + 1));
			}
			if (Line.MaxGrade > 0.0f)
			{
				const float MaxStep = static_cast<float>(Line.MaxGrade * Step);
				for (int32 Index = 1; Index < Targets.Num(); ++Index)
				{
					Targets[Index] = FMath::Clamp(Targets[Index], Targets[Index - 1] - MaxStep, Targets[Index - 1] + MaxStep);
				}
				for (int32 Index = Targets.Num() - 2; Index >= 0; --Index)
				{
					Targets[Index] = FMath::Clamp(Targets[Index], Targets[Index + 1] - MaxStep, Targets[Index + 1] + MaxStep);
				}
			}
			for (float& Target : Targets)
			{
				Target += Line.TerrainOffset;
			}
		}
		FVoxelConditioningLineParams Params;
		for (int32 Index = 0; Index < Positions.Num(); ++Index)
		{
			Params.Vertices.Add(FVoxelConditioningLineVertex(Positions[Index], Targets[Index], HalfWidths[Index]));
		}
		Params.FalloffWidth = Line.FalloffWidth;
		Params.Strength = Line.Strength;
		Params.Blend = Line.Blend;
		Params.Id = Line.GetLineId();
		Params.Surface = Line.Surface;
		return Params;
	}

	/** Whether two lines' vertices are the same, value for value. */
	bool SameVertices(const FVoxelConditioningLineParams& A, const FVoxelConditioningLineParams& B)
	{
		if (A.Vertices.Num() != B.Vertices.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Vertices.Num(); ++Index)
		{
			if (A.Vertices[Index].Position != B.Vertices[Index].Position || A.Vertices[Index].TargetHeight != B.Vertices[Index].TargetHeight
				|| A.Vertices[Index].HalfWidth != B.Vertices[Index].HalfWidth)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * The bend bound's measure on a heightmap line, sampled from the line itself: across the bisector of each vertex that
	 * turns, on the inside of the bend, the target on one side less the target on the other (two points a unit apart),
	 * times the weight there; the largest over every such vertex and every distance out to the reach. A line whose
	 * nearest segment changes there with a step shows it; along a line with no step the two points differ by the grade.
	 */
	double WorstStepAcrossBisectors(const FVoxelConditioningLine& Line)
	{
		const TArray<FVoxelConditioningLineVertex>& Vertices = Line.GetVertices();
		double Worst = 0.0;
		for (int32 Index = 1; Index + 1 < Vertices.Num(); ++Index)
		{
			const FVector2D P(Vertices[Index].Position.X, Vertices[Index].Position.Y);
			const FVector2D In = (P - FVector2D(Vertices[Index - 1].Position.X, Vertices[Index - 1].Position.Y)).GetSafeNormal();
			const FVector2D Out = (FVector2D(Vertices[Index + 1].Position.X, Vertices[Index + 1].Position.Y) - P).GetSafeNormal();
			if (FMath::Abs(FVector2D::CrossProduct(In, Out)) < 1e-4)
			{
				continue;
			}
			const FVector2D Inward = (Out - In).GetSafeNormal();
			const FVector2D Tangent = (In + Out).GetSafeNormal();
			for (int32 Step = 1; Step <= 32; ++Step)
			{
				const FVector2D Q = P + Inward * (Line.GetHeightReach() * Step / 32.0);
				float TargetA = 0.0f, WeightA = 0.0f, TargetB = 0.0f, WeightB = 0.0f;
				if (Line.Evaluate(Q.X - 0.5 * Tangent.X, Q.Y - 0.5 * Tangent.Y, TargetA, WeightA)
					&& Line.Evaluate(Q.X + 0.5 * Tangent.X, Q.Y + 0.5 * Tangent.Y, TargetB, WeightB))
				{
					Worst = FMath::Max(Worst, static_cast<double>(FMath::Abs(TargetB - TargetA)) * FMath::Min(WeightA, WeightB));
				}
			}
		}
		return Worst;
	}

	/** The farthest any vertex of a heightmap line lies from a spline, over the ground (the spline read every 5 uu). */
	double FarthestFromSpline(const FVoxelConditioningLineParams& Params, const USplineComponent& Spline)
	{
		TArray<FVector2D> Samples;
		const double Length = Spline.GetSplineLength();
		for (double Distance = 0.0; Distance < Length; Distance += 5.0)
		{
			Samples.Add(FVector2D(Spline.GetLocationAtDistanceAlongSpline(static_cast<float>(Distance), ESplineCoordinateSpace::World)));
		}
		Samples.Add(FVector2D(Spline.GetLocationAtDistanceAlongSpline(static_cast<float>(Length), ESplineCoordinateSpace::World)));
		double Farthest = 0.0;
		for (const FVoxelConditioningLineVertex& Vertex : Params.Vertices)
		{
			double Nearest = TNumericLimits<double>::Max();
			for (const FVector2D& Sample : Samples)
			{
				Nearest = FMath::Min(Nearest, FVector2D::DistSquared(Sample, FVector2D(Vertex.Position.X, Vertex.Position.Y)));
			}
			Farthest = FMath::Max(Farthest, FMath::Sqrt(Nearest));
		}
		return Farthest;
	}

	/** Whether any of a line's warnings is one of the bend bound's two. */
	bool HasBendWarning(const TArray<FString>& Warnings)
	{
		return Warnings.ContainsByPredicate([](const FString& Text)
		{
			return Text.Contains(TEXT("vertices break the bend bound")) || Text.Contains(TEXT("bisector meets a neighbour's inside the reach"));
		});
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTerrainLineBendsTest,
	"VoxelWorlds.Streaming.TerrainLine.Bends",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTerrainLineBendsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTerrainLineTestUtils;

	// A road 6 voxels wide with a falloff of 4: a reach of 7 voxels, the radius the bend bound asks a corner for.
	const auto ConfigureRoad = [](UVoxelTerrainLineComponent* Line, float VoxelSize, EVoxelTerrainLineHeight HeightMode, float MaxGrade)
	{
		Line->HalfWidth = 3.0f * VoxelSize;
		Line->FalloffWidth = 4.0f * VoxelSize;
		Line->HeightMode = HeightMode;
		Line->SmoothingDistance = 1200.0f;
		Line->MaxGrade = MaxGrade;
	};

	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("TerrainLineBends"));
	W.ConfigureSmallPlane();
	W.StartManager();
	const float VoxelSize = W.Config->VoxelSize;
	const double Reach = 7.0 * VoxelSize;
	const double Bound = 0.1 * VoxelSize;

	// 1. A spline that keeps the bound: a gentle S curve. The line is the one L4 made, vertex for vertex, in both height
	//    modes, and nothing is said about bends.
	{
		AVoxelTerrainLineActor* Actor = W.World->SpawnActor<AVoxelTerrainLineActor>();
		Actor->GetSpline()->SetSplinePoints({ FVector(-1500.0, -900.0, 4000.0), FVector(-500.0, -750.0, 4040.0), FVector(500.0, -1050.0, 4080.0), FVector(1500.0, -900.0, 4120.0) },
			ESplineCoordinateSpace::World, true);
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		for (const EVoxelTerrainLineHeight HeightMode : { EVoxelTerrainLineHeight::FollowTerrain, EVoxelTerrainLineHeight::SplineHeight })
		{
			const TCHAR* Name = HeightMode == EVoxelTerrainLineHeight::FollowTerrain ? TEXT("following the terrain") : TEXT("at the spline's height");
			ConfigureRoad(Line, VoxelSize, HeightMode, 0.08f);
			FVoxelConditioningLineParams Params;
			FVoxelTerrainLineBends Bends;
			if (!TestTrue(FString::Printf(TEXT("Within the bound, %s: a line is made"), Name), Line->BuildLineParams(*W.Manager, Params, &Bends)))
			{
				continue;
			}
			AddInfo(FString::Printf(TEXT("Within the bound, %s: %d vertices, the worst vertex turns %.2f of what the bound allows"), Name, Params.Vertices.Num(), Bends.DrawnBendRatio));
			TestTrue(FString::Printf(TEXT("Within the bound, %s: the curve turns, within the bound (non-vacuous)"), Name), Bends.DrawnBendRatio > 0.2 && Bends.DrawnBendRatio <= 1.0);
			TestFalse(FString::Printf(TEXT("Within the bound, %s: the spline does not break the bound"), Name), Bends.bBrokeBound);
			TestTrue(FString::Printf(TEXT("Within the bound, %s: no corner is rounded and the profile is left"), Name),
				!Bends.WasRounded() && Bends.FarthestFromSpline == 0.0 && !Bends.bProfileFlattened && Bends.BendRatio == Bends.DrawnBendRatio);
			TestTrue(FString::Printf(TEXT("Within the bound, %s: the line is the one L4 made, vertex for vertex"), Name), SameVertices(Params, SampledLine(*Line, *W.Manager)));
			TestFalse(FString::Printf(TEXT("Within the bound, %s: the engine's check has no bend warning"), Name),
				HasBendWarning(W.Manager->GetConditioningLineWarnings(*FVoxelConditioningLine::Make(Params))));
		}
	}

	// 2. The corner of L4's shots: two straight legs that meet at 60 degrees on a slope, following the terrain held to a
	//    grade of 20 %. The corner is put where the drawn line steps most, of 25 places and 6 directions.
	FVoxelConditioningLineParams CornerParams;
	FVoxelTerrainLineBends CornerBends;
	TArray<FVector> CornerPoints;
	{
		AVoxelTerrainLineActor* Actor = SpawnLine(W.World, PlaneCorner(FVector2D::ZeroVector, 0.0, 60.0, 1500.0));
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		ConfigureRoad(Line, VoxelSize, EVoxelTerrainLineHeight::FollowTerrain, 0.2f);
		double DrawnStep = 0.0;
		for (int32 X = -2; X <= 2; ++X)
		{
			for (int32 Y = -2; Y <= 2; ++Y)
			{
				for (int32 Direction = 0; Direction < 6; ++Direction)
				{
					const TArray<FVector> Points = PlaneCorner(FVector2D(1500.0 * X, 1500.0 * Y), 60.0 * Direction, 60.0, 1500.0);
					SetSplineThrough(Actor->GetSpline(), Points);
					const double Step = WorstStepAcrossBisectors(*FVoxelConditioningLine::Make(SampledLine(*Line, *W.Manager)));
					if (Step > DrawnStep)
					{
						DrawnStep = Step;
						CornerPoints = Points;
					}
				}
			}
		}
		SetSplineThrough(Actor->GetSpline(), CornerPoints);
		const FVoxelConditioningLineRef Drawn = FVoxelConditioningLine::Make(SampledLine(*Line, *W.Manager));
		TestTrue(TEXT("Corner: as drawn the target steps by over twice the bound across the bisector (non-vacuous)"), DrawnStep > 2.0 * Bound);
		TestTrue(TEXT("Corner: as drawn the engine's check warns of the bend"), HasBendWarning(W.Manager->GetConditioningLineWarnings(*Drawn)));

		if (TestTrue(TEXT("Corner: a line is made"), Line->BuildLineParams(*W.Manager, CornerParams, &CornerBends)))
		{
			const FVoxelConditioningLineRef Made = FVoxelConditioningLine::Make(CornerParams);
			const double MadeStep = WorstStepAcrossBisectors(*Made);
			const double Independent = FarthestFromSpline(CornerParams, *Actor->GetSpline());
			AddInfo(FString::Printf(TEXT("Corner at (%.0f, %.0f): as drawn the target steps by %.1f uu across the bisector (the bound %.1f; %.1f times what the bound allows); "
				"as made by %.2f uu (%.2f times), the corners spread over %.0f uu each way, %d vertices, up to %.1f uu from the spline (measured: %.1f)"),
				CornerPoints[1].X, CornerPoints[1].Y, DrawnStep, Bound, CornerBends.DrawnBendRatio, MadeStep, CornerBends.BendRatio, CornerBends.CornerDistance,
				CornerParams.Vertices.Num(), CornerBends.FarthestFromSpline, Independent));
			TestTrue(TEXT("Corner: the spline breaks the bound"), CornerBends.bBrokeBound && CornerBends.DrawnBendRatio > 1.0);
			TestTrue(TEXT("Corner: the corner is rounded and the profile made again"), CornerBends.WasRounded() && CornerBends.bProfileFlattened);
			// Two points a unit apart differ by the grade where nothing steps: 0.2 uu of slack.
			TestTrue(TEXT("Corner: the line as made steps by no more than the bound across any bisector"), MadeStep <= Bound + 0.25);
			TestTrue(TEXT("Corner: every vertex is within the bound"), CornerBends.BendRatio <= 1.0);
			TestFalse(TEXT("Corner: the engine's check has no bend warning left"), HasBendWarning(W.Manager->GetConditioningLineWarnings(*Made)));

			// Within the radius the bound asks for (the reach) of the spline, and the component says how far.
			TestTrue(TEXT("Corner: the line leaves the spline (non-vacuous), by no more than the bound's radius"),
				CornerBends.FarthestFromSpline > 1.0 && CornerBends.FarthestFromSpline <= Reach);
			TestTrue(TEXT("Corner: measured against the spline itself, the line is no farther than the component says"),
				Independent > 1.0 && Independent <= CornerBends.FarthestFromSpline + 5.0);
			TestTrue(TEXT("Corner: the corners are spread over no more than twice the reach"), CornerBends.CornerDistance <= 2.0 * Reach);
			TestTrue(TEXT("Corner: the line starts where the spline does"), FVector2D::Distance(FVector2D(CornerParams.Vertices[0].Position), FVector2D(CornerPoints[0])) < 0.1);
			TestTrue(TEXT("Corner: and ends where it does"), FVector2D::Distance(FVector2D(CornerParams.Vertices.Last().Position), FVector2D(CornerPoints[2])) < 0.1);
			double LongestSegment = 0.0, SteepestGrade = 0.0;
			float NarrowestHalfWidth = TNumericLimits<float>::Max(), WidestHalfWidth = 0.0f;
			for (int32 Index = 0; Index < CornerParams.Vertices.Num(); ++Index)
			{
				NarrowestHalfWidth = FMath::Min(NarrowestHalfWidth, CornerParams.Vertices[Index].HalfWidth);
				WidestHalfWidth = FMath::Max(WidestHalfWidth, CornerParams.Vertices[Index].HalfWidth);
				if (Index > 0)
				{
					const double Length = FVector::Dist(CornerParams.Vertices[Index].Position, CornerParams.Vertices[Index - 1].Position);
					LongestSegment = FMath::Max(LongestSegment, Length);
					SteepestGrade = FMath::Max(SteepestGrade, FMath::Abs(CornerParams.Vertices[Index].TargetHeight - CornerParams.Vertices[Index - 1].TargetHeight) / Length);
				}
			}
			TestTrue(TEXT("Corner: a vertex every two voxels at most"), LongestSegment <= 2.0 * VoxelSize + 0.01);
			TestTrue(TEXT("Corner: no segment steeper than the grade"), SteepestGrade <= 0.2 + 1e-3);
			TestTrue(TEXT("Corner: the half width is the component's along the line"), NarrowestHalfWidth == 3.0f * VoxelSize && WidestHalfWidth == 3.0f * VoxelSize);
		}
	}

	// 3. A server and a client: the same actor in another world of the same configuration makes the same line, value for
	//    value (nothing of the line is replicated: each machine makes it from the level's actor).
	if (CornerPoints.Num() == 3)
	{
		VoxelTesting::FTestChunkWorld Other;
		Other.Create(TEXT("TerrainLineBendsOther"));
		Other.ConfigureSmallPlane();
		Other.StartManager();
		AVoxelTerrainLineActor* Actor = SpawnLine(Other.World, CornerPoints);
		ConfigureRoad(Actor->GetTerrainLine(), VoxelSize, EVoxelTerrainLineHeight::FollowTerrain, 0.2f);
		FVoxelConditioningLineParams Params;
		FVoxelTerrainLineBends Bends;
		if (TestTrue(TEXT("Another world: a line is made"), Actor->GetTerrainLine()->BuildLineParams(*Other.Manager, Params, &Bends)))
		{
			TestTrue(TEXT("Another world: the rounded line is the same, vertex for vertex"), SameVertices(Params, CornerParams));
			TestTrue(TEXT("Another world: and so is what was done about its bends"),
				Bends.bBrokeBound == CornerBends.bBrokeBound && Bends.DrawnBendRatio == CornerBends.DrawnBendRatio && Bends.BendRatio == CornerBends.BendRatio
				&& Bends.CornerDistance == CornerBends.CornerDistance && Bends.FarthestFromSpline == CornerBends.FarthestFromSpline
				&& Bends.bProfileFlattened == CornerBends.bProfileFlattened);
		}
		Other.Stop();
	}

	// 4. At the spline's own height: a 60 degree corner on a 15 % grade. The corner is rounded further than one that
	//    follows the terrain (the profile is the designer's, so the turn itself must fit the grade), and the line keeps
	//    the spline's heights along it.
	{
		const float Ground = static_cast<float>(W.Manager->GetSurfacePointAt(FVector(0.0, 0.0, 0.0)).Position.Z);
		AVoxelTerrainLineActor* Actor = SpawnLine(W.World, PlaneCorner(FVector2D::ZeroVector, 0.0, 60.0, 1500.0, Ground - 225.0, Ground, Ground + 225.0));
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		ConfigureRoad(Line, VoxelSize, EVoxelTerrainLineHeight::SplineHeight, 0.0f);
		FVoxelConditioningLineParams Params;
		FVoxelTerrainLineBends Bends;
		if (TestTrue(TEXT("Spline height: a line is made"), Line->BuildLineParams(*W.Manager, Params, &Bends)))
		{
			const FVoxelConditioningLineRef Made = FVoxelConditioningLine::Make(Params);
			AddInfo(FString::Printf(TEXT("Spline height: as drawn %.1f times what the bound allows; as made %.2f times, the corners spread over %.0f uu each way, up to %.1f uu from the spline, the target steps by %.2f uu"),
				Bends.DrawnBendRatio, Bends.BendRatio, Bends.CornerDistance, Bends.FarthestFromSpline, WorstStepAcrossBisectors(*Made)));
			TestTrue(TEXT("Spline height: the spline breaks the bound and its corner is rounded"), Bends.bBrokeBound && Bends.WasRounded());
			TestFalse(TEXT("Spline height: the profile is not made again"), Bends.bProfileFlattened);
			TestTrue(TEXT("Spline height: every vertex is within the bound"), Bends.BendRatio <= 1.0);
			TestFalse(TEXT("Spline height: the engine's check has no bend warning left"), HasBendWarning(W.Manager->GetConditioningLineWarnings(*Made)));
			TestTrue(TEXT("Spline height: the line as made steps by no more than the bound across any bisector"), WorstStepAcrossBisectors(*Made) <= Bound + 0.25);
			TestTrue(TEXT("Spline height: the line is no farther from the spline than the component says, and no farther than the bound's radius"),
				FarthestFromSpline(Params, *Actor->GetSpline()) <= Bends.FarthestFromSpline + 5.0 && Bends.FarthestFromSpline <= Reach);
			TestTrue(TEXT("Spline height: the corner is spread further than the same corner following the terrain"), Bends.CornerDistance > CornerBends.CornerDistance);
			TestEqual(TEXT("Spline height: the spline's height at the start"), Params.Vertices[0].TargetHeight, Ground - 225.0f, 0.01f);
			TestEqual(TEXT("Spline height: at the end"), Params.Vertices.Last().TargetHeight, Ground + 225.0f, 0.01f);
			// The spline climbs 15 % along both legs; the rounded line, shorter through the corner, climbs the same rise there.
			double GentlestGrade = TNumericLimits<double>::Max(), SteepestGrade = 0.0;
			for (int32 Index = 1; Index < Params.Vertices.Num(); ++Index)
			{
				const double Grade = (Params.Vertices[Index].TargetHeight - Params.Vertices[Index - 1].TargetHeight)
					/ FVector::Dist(Params.Vertices[Index].Position, Params.Vertices[Index - 1].Position);
				GentlestGrade = FMath::Min(GentlestGrade, Grade);
				SteepestGrade = FMath::Max(SteepestGrade, Grade);
			}
			AddInfo(FString::Printf(TEXT("Spline height: the line climbs between %.1f %% and %.1f %% a segment"), 100.0 * GentlestGrade, 100.0 * SteepestGrade));
			TestTrue(TEXT("Spline height: the line climbs all the way, at the spline's 15 % on the legs and a little more through the shorter corner"),
				GentlestGrade > 0.14 && SteepestGrade < 0.19);
		}
	}
	W.Stop();

	// 5. A planet: the corner is rounded on the sampled points, which then become directions again.
	{
		VoxelTesting::FTestChunkWorld P;
		P.Create(TEXT("TerrainLineBendsPlanet"));
		P.ConfigureSmallPlanet();
		P.StartManager();
		const FVector Center = P.Config->WorldOrigin;
		const double Radius = P.Config->WorldRadius;
		const float PlanetVoxel = P.Config->VoxelSize;
		const FVector Site = FVector(0.7, 0.6, 0.4).GetSafeNormal();
		const FVector East = FVector::CrossProduct(FVector::UpVector, Site).GetSafeNormal();
		const FVector North = FVector::CrossProduct(Site, East);
		const FVector Start = (Site * FMath::Cos(800.0 / Radius) - East * FMath::Sin(800.0 / Radius)).GetSafeNormal();
		const FVector End = (Site * FMath::Cos(800.0 / Radius) + North * FMath::Sin(800.0 / Radius)).GetSafeNormal();
		AVoxelTerrainLineActor* Actor = SpawnLine(P.World, { Center + Start * (Radius + 300.0), Center + Site * (Radius + 300.0), Center + End * (Radius + 300.0) });
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		ConfigureRoad(Line, PlanetVoxel, EVoxelTerrainLineHeight::FollowTerrain, 0.15f);
		FVoxelConditioningLineParams Params;
		FVoxelTerrainLineBends Bends;
		if (TestTrue(TEXT("Planet: a line is made"), Line->BuildLineParams(*P.Manager, Params, &Bends)))
		{
			AddInfo(FString::Printf(TEXT("Planet: as drawn %.1f times what the bound allows; as made %.2f times, the corners spread over %.0f uu each way, %d vertices, up to %.1f uu from the spline"),
				Bends.DrawnBendRatio, Bends.BendRatio, Bends.CornerDistance, Params.Vertices.Num(), Bends.FarthestFromSpline));
			TestTrue(TEXT("Planet: a planet line"), Params.bRadial);
			TestTrue(TEXT("Planet: the spline breaks the bound, its corner is rounded and its profile made again"), Bends.bBrokeBound && Bends.WasRounded() && Bends.bProfileFlattened);
			TestTrue(TEXT("Planet: every vertex is within the bound"), Bends.BendRatio <= 1.0);
			TestFalse(TEXT("Planet: the engine's check has no bend warning left"), HasBendWarning(P.Manager->GetConditioningLineWarnings(*FVoxelConditioningLine::Make(Params))));
			TestTrue(TEXT("Planet: the line leaves the spline by no more than the bound's radius"),
				Bends.FarthestFromSpline > 1.0 && Bends.FarthestFromSpline <= 7.0 * PlanetVoxel);
			double WorstLength = 0.0, LongestSegment = 0.0;
			for (int32 Index = 0; Index < Params.Vertices.Num(); ++Index)
			{
				WorstLength = FMath::Max(WorstLength, FMath::Abs(Params.Vertices[Index].Position.Size() - 1.0));
				if (Index > 0)
				{
					LongestSegment = FMath::Max(LongestSegment, FVector::Dist(Params.Vertices[Index].Position, Params.Vertices[Index - 1].Position) * Radius);
				}
			}
			TestTrue(TEXT("Planet: the vertices are unit directions"), WorstLength < 1e-9);
			TestTrue(TEXT("Planet: a vertex every two voxels at most"), LongestSegment <= 2.0 * PlanetVoxel + 0.01);
			TestTrue(TEXT("Planet: the line starts where the spline does"), Params.Vertices[0].Position.Equals(Start, 1e-9));
			TestTrue(TEXT("Planet: and ends where it does"), Params.Vertices.Last().Position.Equals(End, 1e-9));
		}
		P.Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTerrainLineWarningsTest,
	"VoxelWorlds.Streaming.TerrainLine.Warnings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTerrainLineWarningsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTerrainLineTestUtils;
	// What the component says of a spline that breaks the bend bound, in the log and in GetLineWarnings: both lines below
	// log what was done; the second also keeps the engine's warning for the bound it still breaks.
	AddExpectedMessagePlain(TEXT("the spline breaks the bend bound"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("vertices break the bend bound"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("TerrainLineWarnings"));
	W.ConfigureSmallPlane();
	W.StartManager();
	const float VoxelSize = W.Config->VoxelSize;

	// 1. A 60 degree corner at the spline's own height on a 15 % grade: the corner is rounded within the bound, and the
	//    one thing said is how far the line runs from the spline.
	{
		const float Ground = static_cast<float>(W.Manager->GetSurfacePointAt(FVector(0.0, 0.0, 0.0)).Position.Z);
		AVoxelTerrainLineActor* Actor = SpawnLine(W.World, PlaneCorner(FVector2D::ZeroVector, 0.0, 60.0, 1500.0, Ground - 225.0, Ground, Ground + 225.0));
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		Line->HalfWidth = 3.0f * VoxelSize;
		Line->FalloffWidth = 4.0f * VoxelSize;
		Line->HeightMode = EVoxelTerrainLineHeight::SplineHeight;
		Actor->DispatchBeginPlay();
		TestTrue(TEXT("Rounded: the line is added"), Line->IsLineRegistered() && W.Manager->GetNumConditioningLines() == 1);
		const TArray<FString>& Warnings = Line->GetLineWarnings();
		for (const FString& Warning : Warnings)
		{
			AddInfo(TEXT("Rounded: ") + Warning);
		}
		const FVoxelTerrainLineBends& Bends = Line->GetLineBends();
		TestTrue(TEXT("Rounded: the component keeps what was done about the bends"), Bends.bBrokeBound && Bends.WasRounded() && Bends.BendRatio <= 1.0);
		if (TestEqual(TEXT("Rounded: one sentence, and no limit broken"), Warnings.Num(), 1))
		{
			TestTrue(TEXT("Rounded: it says the spline broke the bend bound"), Warnings[0].Contains(TEXT("the spline breaks the bend bound")));
			TestTrue(TEXT("Rounded: and how far the line runs from its spline at most"),
				Warnings[0].Contains(FString::Printf(TEXT("runs up to %.0f uu from its spline"), Bends.FarthestFromSpline)));
			TestTrue(TEXT("Rounded: and that the heights are the spline's"), Warnings[0].Contains(TEXT("at the spline's heights along it")));
		}
		if (Line->IsLineRegistered())
		{
			// As drawn the line passes within 25 uu of the corner; rounded, it cuts well inside it.
			double NearestToCorner = TNumericLimits<double>::Max();
			for (const FVoxelConditioningLineVertex& Vertex : Line->GetRegisteredLine()->GetVertices())
			{
				NearestToCorner = FMath::Min(NearestToCorner, FVector2D(Vertex.Position).Size());
			}
			TestTrue(TEXT("Rounded: the registered line is the rounded one"), NearestToCorner > 60.0);
		}
	}

	// 2. A right-angle corner at the spline's own height on a 40 % grade: no spreading within twice the reach brings the
	//    turn under what that grade allows. The line is rounded as far as helps, and keeps the bend bound's warning.
	{
		const FVector2D Corner(0.0, -4000.0);
		const float Ground = static_cast<float>(W.Manager->GetSurfacePointAt(FVector(Corner.X, Corner.Y, 0.0)).Position.Z);
		AVoxelTerrainLineActor* Actor = SpawnLine(W.World, PlaneCorner(Corner, 0.0, 90.0, 1000.0, Ground - 400.0, Ground, Ground + 400.0));
		UVoxelTerrainLineComponent* Line = Actor->GetTerrainLine();
		Line->HalfWidth = 3.0f * VoxelSize;
		Line->FalloffWidth = 4.0f * VoxelSize;
		Line->HeightMode = EVoxelTerrainLineHeight::SplineHeight;
		Actor->DispatchBeginPlay();
		TestTrue(TEXT("Steep: a line that still breaks the bend bound is added"), Line->IsLineRegistered() && W.Manager->GetNumConditioningLines() == 2);
		const TArray<FString>& Warnings = Line->GetLineWarnings();
		for (const FString& Warning : Warnings)
		{
			AddInfo(TEXT("Steep: ") + Warning);
		}
		const FVoxelTerrainLineBends& Bends = Line->GetLineBends();
		AddInfo(FString::Printf(TEXT("Steep: as drawn %.1f times what the bound allows; as made %.1f times, the corners spread over %.0f uu each way, up to %.1f uu from the spline"),
			Bends.DrawnBendRatio, Bends.BendRatio, Bends.CornerDistance, Bends.FarthestFromSpline));
		TestTrue(TEXT("Steep: the corner is rounded, over no more than twice the reach"), Bends.WasRounded() && Bends.CornerDistance <= 14.0 * VoxelSize);
		TestTrue(TEXT("Steep: which brings it nearer the bound, not within it"), Bends.BendRatio > 1.0 && Bends.BendRatio < Bends.DrawnBendRatio);
		if (TestTrue(TEXT("Steep: two sentences or more"), Warnings.Num() >= 2))
		{
			TestTrue(TEXT("Steep: the first says what was done, and that the line still breaks the bound"),
				Warnings[0].Contains(TEXT("the spline breaks the bend bound")) && Warnings[0].Contains(TEXT("the line still breaks the bound")));
			TestTrue(TEXT("Steep: the engine's bend bound warning follows"),
				Warnings.ContainsByPredicate([](const FString& Text) { return Text.Contains(TEXT("vertices break the bend bound")); }));
		}
	}
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
