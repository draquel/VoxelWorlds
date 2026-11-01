// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 4: the edit-merged surface along local up (WORLD_MODES_REVIEW_PLAN.md §18, WM-SY2; SP-A3).
//
// - On a streamed planet at the pole, 45 degrees, the equator and a cube corner, QueryEditMergedSurfacePoint finds the
//   surface on the radial line through the queried direction, within a quarter voxel of the analytic surface, with its
//   normal near the analytic one and the slope measured against local up; a dig lowers it along local up.
// - On a plane it is the column query at (X, Y) under +Z (the one edit-merged query since code review CR2, CM-21), and
//   GetGeneratedSurfacePoint is (X, Y, the generated column's height).
// ---------------------------------------------------------------------------

namespace VoxelPlanetSurfaceQueryTestUtils
{
	/** Directions within MaxAngle (radians) of Axis: a ring pattern, Count of them. */
	TArray<FVector> DirectionsAround(const FVector& Axis, double MaxAngle, int32 Count)
	{
		const FVector T1 = FVector::CrossProduct(FMath::Abs(Axis.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector, Axis).GetSafeNormal();
		const FVector T2 = FVector::CrossProduct(Axis, T1);
		TArray<FVector> Out;
		for (int32 i = 0; i < Count; ++i)
		{
			const double Angle = MaxAngle * FMath::Sqrt((i + 0.5) / Count);
			const double Bearing = i * 2.39996323; // golden angle
			Out.Add((Axis * FMath::Cos(Angle) + (T1 * FMath::Cos(Bearing) + T2 * FMath::Sin(Bearing)) * FMath::Sin(Angle)).GetSafeNormal());
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetEditMergedSurfaceTest,
	"VoxelWorlds.Streaming.Planet.EditMergedSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetEditMergedSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetSurfaceQueryTestUtils;

	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const TArray<TPair<const TCHAR*, FVector>> Sites = {
		{ TEXT("north pole"), FVector(0, 0, 1) },
		{ TEXT("45 degrees"), FVector(C45, 0, C45) },
		{ TEXT("equator"), FVector(0, 1, 0) },
		{ TEXT("cube corner"), FVector(1, 1, 1).GetSafeNormal() },
	};

	for (const TPair<const TCHAR*, FVector>& Site : Sites)
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("PlanetEditMergedSurface"));
		W.ConfigureSmallPlanet();
		W.StartManager();
		const FVector Center = W.Config->WorldOrigin;
		const double Radius = W.Config->WorldRadius;
		const float VoxelSize = W.Config->VoxelSize;
		const FVoxelTerrainSurfacePoint SiteSurface = W.Manager->GetSurfacePointAt(Center + Site.Value);
		W.SetViewer(SiteSurface.Position + Site.Value * 300.0);
		if (!TestTrue(*FString::Printf(TEXT("%s: streaming settles"), Site.Key), W.Settle()))
		{
			W.Stop();
			continue;
		}

		int32 Found = 0, Queried = 0;
		double WorstAlong = 0.0, WorstAcross = 0.0, WorstNormalDeg = 0.0, WorstSlopeError = 0.0, WorstUp = 0.0;
		for (const FVector& Direction : DirectionsAround(Site.Value, 1200.0 / Radius, 40))
		{
			++Queried;
			FVoxelSurfaceSample Hit;
			if (!W.Manager->QueryEditMergedSurfacePoint(Center + Direction * Radius, Hit))
			{
				continue;
			}
			++Found;
			const FVoxelTerrainSurfacePoint Analytic = W.Manager->GetSurfacePointAt(Center + Direction);
			const FVector Offset = Hit.Position - Analytic.Position;
			WorstAlong = FMath::Max(WorstAlong, FMath::Abs(FVector::DotProduct(Offset, Direction)));
			WorstAcross = FMath::Max(WorstAcross, (Offset - Direction * FVector::DotProduct(Offset, Direction)).Size());
			WorstNormalDeg = FMath::Max(WorstNormalDeg, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Hit.Normal, Analytic.Normal), -1.0, 1.0))));
			const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Hit.Normal, Direction), -1.0, 1.0)));
			WorstSlopeError = FMath::Max(WorstSlopeError, FMath::Abs(Slope - Hit.SlopeDegrees));
			WorstUp = FMath::Max(WorstUp, (Hit.Up - Direction).Size());
			TestTrue(TEXT("Altitude matches the position"), FMath::IsNearlyEqual(Hit.Altitude, W.Manager->GetAltitudeAt(Hit.Position), 0.01f));
		}
		AddInfo(FString::Printf(TEXT("%s: %d of %d found; worst along up %.2f uu, across %.4f uu, normal %.1f deg off the analytic, slope error %.4f deg"),
			Site.Key, Found, Queried, WorstAlong, WorstAcross, WorstNormalDeg, WorstSlopeError));
		TestEqual(*FString::Printf(TEXT("%s: every direction near the viewer has a loaded surface"), Site.Key), Found, Queried);
		TestTrue(*FString::Printf(TEXT("%s: within a quarter voxel of the analytic surface along up"), Site.Key), WorstAlong <= VoxelSize * 0.25);
		TestTrue(*FString::Printf(TEXT("%s: on the radial line"), Site.Key), WorstAcross <= 0.01);
		TestTrue(*FString::Printf(TEXT("%s: the normal within 10 degrees of the analytic one"), Site.Key), WorstNormalDeg <= 10.0);
		TestTrue(*FString::Printf(TEXT("%s: the slope is against local up"), Site.Key), WorstSlopeError <= 0.01);
		TestTrue(*FString::Printf(TEXT("%s: local up is radial"), Site.Key), WorstUp <= 1.0e-6);

		// A dig at the site lowers the surface along local up.
		FVoxelSurfaceSample Before;
		const bool bBefore = W.Manager->QueryEditMergedSurfacePoint(SiteSurface.Position, Before);
		FVoxelBrushParams Brush;
		Brush.Shape = EVoxelBrushShape::Sphere;
		Brush.Radius = 300.0f;
		Brush.Strength = 1.0f;
		Brush.FalloffType = EVoxelBrushFalloff::Linear;
		Brush.DensityDelta = 255;
		W.Manager->GetEditManager()->ApplyBrushEdit(Before.Position, Brush, EEditMode::Subtract);
		FVoxelSurfaceSample After;
		const bool bAfter = W.Manager->QueryEditMergedSurfacePoint(SiteSurface.Position, After);
		AddInfo(FString::Printf(TEXT("%s: dig of radius 300: altitude %.1f -> %.1f"), Site.Key, Before.Altitude, After.Altitude));
		TestTrue(*FString::Printf(TEXT("%s: the surface is found before and after the dig"), Site.Key), bBefore && bAfter);
		TestTrue(*FString::Printf(TEXT("%s: the dig lowers it by more than 100 uu along local up"), Site.Key), After.Altitude <= Before.Altitude - 100.0f);
		TestTrue(*FString::Printf(TEXT("%s: still on the radial line"), Site.Key),
			FVector::CrossProduct((After.Position - Center).GetSafeNormal(), Site.Value).Size() < 1.0e-6);

		// The generated point is the analytic one on a planet.
		TestTrue(*FString::Printf(TEXT("%s: GetGeneratedSurfacePoint is the analytic surface"), Site.Key),
			FVector::Dist(W.Manager->GetGeneratedSurfacePoint(Center + Site.Value * 10.0), SiteSurface.Position) < 0.01);
		W.Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneEditMergedSurfaceUnchangedTest,
	"VoxelWorlds.Streaming.Plane.EditMergedSurfaceUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneEditMergedSurfaceUnchangedTest::RunTest(const FString& Parameters)
{
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("PlaneEditMergedSurfaceUnchanged"));
	W.ConfigureSmallPlane();
	W.Config->WorldOrigin = FVector(12345.0, -6789.0, 0.0);
	W.StartManager();
	const FVector Origin = W.Config->WorldOrigin;
	W.SetViewer(FVector(Origin.X, Origin.Y, W.Manager->GetGeneratedSurfacePoint(FVector(Origin.X, Origin.Y, 0.0)).Z + 300.0));
	if (!TestTrue(TEXT("Streaming settles"), W.Settle()))
	{
		W.Stop();
		return true;
	}

	FRandomStream Random(77);
	int32 Found = 0, Column = 0, Ladder = 0;
	constexpr int32 Points = 1000;
	for (int32 i = 0; i < Points; ++i)
	{
		const double X = Origin.X + Random.FRandRange(-1500.0, 1500.0);
		const double Y = Origin.Y + Random.FRandRange(-1500.0, 1500.0);
		// The heightmap sample is the column's at (X, Y) under +Z, whatever the query position's Z (code review CR2, CM-21:
		// the one edit-merged query, the column query's body).
		FVoxelSurfaceSample Hit;
		const bool bNew = W.Manager->QueryEditMergedSurfacePoint(FVector(X, Y, Random.FRandRange(-5000.0, 5000.0)), Hit);
		Found += bNew ? 1 : 0;
		Column += (!bNew || (Hit.Position.X == X && Hit.Position.Y == Y && Hit.Up == FVector::UpVector
			&& Hit.Normal.Z >= 0.0 && Hit.SlopeDegrees >= 0.0f && Hit.SlopeDegrees <= 90.0f)) ? 1 : 0;

		// The ladder's one entry (code review CR2, CM-21): each rung's height equals the entry it dispatches to (the natural
		// and generated rungs sample through FVoxelSurfaceQuery, whose heightmap positions carry the sample's float X and Y).
		const FVector At(X, Y, 123.0);
		FVoxelSurfaceSample Natural, GeneratedSample, Edited;
		Ladder += (W.Manager->QuerySurfacePoint(At, EVoxelSurfaceLayer::Natural, Natural)
			&& Natural.Position.Z == W.Manager->GetSurfacePointAt(At).Position.Z
			&& W.Manager->QuerySurfacePoint(At, EVoxelSurfaceLayer::Generated, GeneratedSample)
			&& GeneratedSample.Position.Z == W.Manager->GetGeneratedSurfacePoint(At).Z
			&& W.Manager->QuerySurfacePoint(At, EVoxelSurfaceLayer::EditMerged, Edited) == bNew
			&& (!bNew || Edited.Position == Hit.Position)) ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("%d points: %d found, %d on their column with an up-facing normal"), Points, Found, Column));
	TestTrue(TEXT("Most points have a loaded surface"), Found >= Points * 9 / 10);
	TestEqual(TEXT("Every edit-merged sample lies on its column at (X, Y) under +Z"), Column, Points);
	TestEqual(TEXT("QuerySurfacePoint's three rungs equal the entries they dispatch to"), Ladder, Points);
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
