// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelStreamingBenchmark.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 4, WM-SY18 (SP-I4): the bench's great-circle route (voxel.Bench.Run gc=lat:lon:heading[:alt]). Every
// point lies at the flight altitude over the datum, on the plane through the centre that holds the start and the heading;
// the angle travelled is the arc length over the flight radius; heading 0 from the equator reaches the north pole a
// quarter of the way round, heading 90 runs along the equator.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBenchGreatCircleRouteTest,
	"VoxelWorlds.Bench.Planet.GreatCircleRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelBenchGreatCircleRouteTest::RunTest(const FString& Parameters)
{
	const FVector Center(1000.0, -2000.0, 500.0);
	constexpr double Radius = 250000.0;
	constexpr float Altitude = 1200.0f;
	const double Flight = Radius + Altitude;

	FRandomStream Random(23);
	double WorstAltitude = 0.0, WorstPlane = 0.0, WorstAngle = 0.0;
	for (int32 r = 0; r < 20; ++r)
	{
		FVoxelBenchConfig Config;
		FVoxelStreamingBenchmark::MakeGreatCircle(Config, Center, Radius, Random.FRandRange(-80.0, 80.0), Random.FRandRange(-180.0, 180.0),
			Random.FRandRange(0.0, 360.0), Altitude);
		TestTrue(TEXT("A great-circle path"), Config.PathType == EVoxelBenchPath::GreatCircle);
		TestTrue(TEXT("The start is the route's first point"), Config.StartPosition.Equals(FVoxelStreamingBenchmark::EvaluateGreatCircle(Config, 0.0), 1.0e-3));
		const FVector Normal = FVector::CrossProduct(Config.GreatCircleStart, Config.GreatCircleHeading).GetSafeNormal();
		for (int32 i = 0; i < 50; ++i)
		{
			const double S = Random.FRandRange(0.0, 2.0 * UE_DOUBLE_PI * Flight);
			const FVector Point = FVoxelStreamingBenchmark::EvaluateGreatCircle(Config, S);
			WorstAltitude = FMath::Max(WorstAltitude, FMath::Abs(FVector::Dist(Point, Center) - Flight));
			WorstPlane = FMath::Max(WorstPlane, FMath::Abs(FVector::DotProduct(Point - Center, Normal)));
			const double Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct((Point - Center).GetSafeNormal(), Config.GreatCircleStart), -1.0, 1.0));
			const double Expected = FMath::Fmod(S / Flight, 2.0 * UE_DOUBLE_PI);
			WorstAngle = FMath::Max(WorstAngle, FMath::Abs(Angle - (Expected <= UE_DOUBLE_PI ? Expected : 2.0 * UE_DOUBLE_PI - Expected)));
		}
	}
	AddInfo(FString::Printf(TEXT("1,000 points: altitude %.2e uu, off the plane %.2e uu, angle %.2e rad"), WorstAltitude, WorstPlane, WorstAngle));
	TestTrue(TEXT("At the flight altitude"), WorstAltitude < 1.0e-3);
	TestTrue(TEXT("On the great circle"), WorstPlane < 1.0e-3);
	TestTrue(TEXT("The arc length over the flight radius"), WorstAngle < 1.0e-6);

	// Heading 0 from the equator: the north pole a quarter round; heading 90: along the equator toward +Y.
	FVoxelBenchConfig North;
	FVoxelStreamingBenchmark::MakeGreatCircle(North, Center, Radius, 0.0, 0.0, 0.0, Altitude);
	TestTrue(TEXT("Heading 0 from the equator reaches the north pole"),
		FVoxelStreamingBenchmark::EvaluateGreatCircle(North, 0.5 * UE_DOUBLE_PI * Flight).Equals(Center + FVector(0, 0, Flight), 1.0e-2));
	FVoxelBenchConfig East;
	FVoxelStreamingBenchmark::MakeGreatCircle(East, Center, Radius, 0.0, 0.0, 90.0, Altitude);
	TestTrue(TEXT("Heading 90 from the equator runs along it to longitude 90"),
		FVoxelStreamingBenchmark::EvaluateGreatCircle(East, 0.5 * UE_DOUBLE_PI * Flight).Equals(Center + FVector(0, Flight, 0), 1.0e-2));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
