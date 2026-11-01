// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelStreamingBenchmark.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Bench.ParseArgs: voxel.Bench.Run's arguments read into a config (FVoxelBenchConfig::ParseArgs, code review CR4 row 5,
// CM-35), every form the command's help lists on a heightmap world: the positional tag, velocity and distance; dir=;
// dir=up; start= with and without Z (500 over the generated surface); route= (a point without Z at the start's altitude);
// orbit=; climb=X:Y[:Alt] and descend=X:Y:Alt; follow=, warmup=, catchup=, build=; an unknown key ignored; gc= refused off
// a planet.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBenchParseArgsTest,
	"VoxelWorlds.Bench.ParseArgs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelBenchParseArgsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTesting;
	FTestChunkWorld W;
	W.Create(TEXT("BenchParseArgsWorld"));
	W.ConfigureSmallPlane();
	W.StartManager();
	UVoxelChunkManager& CM = *W.Manager;
	UWorld& World = *W.World;

	auto Parse = [&CM, &World](const TCHAR* Line)
	{
		TArray<FString> Args;
		FString(Line).ParseIntoArrayWS(Args);
		FVoxelBenchConfig Config;
		FVoxelBenchConfig::ParseArgs(Args, CM, World, Config);
		return Config;
	};
	auto SurfaceZ = [&CM](double X, double Y) { return CM.GetGeneratedSurfacePoint(FVector(X, Y, 0.0)).Z; };

	// Positional, a straight path with a start and its Z, the scalar keys; an unknown key is ignored with a warning.
	{
		AddExpectedMessage(TEXT("ignoring unrecognised argument 'nonsense=1'"), EAutomationExpectedMessageFlags::Contains, 1);
		const FVoxelBenchConfig C = Parse(TEXT("chord 1500 40000 dir=0:1 start=100:200:300 follow=250 warmup=5 catchup=20 build=abc nonsense=1"));
		TestEqual(TEXT("tag"), C.Tag, FString(TEXT("chord")));
		TestEqual(TEXT("velocity"), C.VelocityUU, 1500.0f);
		TestEqual(TEXT("distance"), C.TraverseDistance, 40000.0f);
		TestTrue(TEXT("dir=0:1"), C.Direction.Equals(FVector(0.0, 1.0, 0.0)));
		TestTrue(TEXT("start=100:200:300"), C.StartPosition.Equals(FVector(100.0, 200.0, 300.0)));
		TestTrue(TEXT("a straight path"), C.PathType == EVoxelBenchPath::Straight);
		TestEqual(TEXT("follow"), C.FollowTerrainAltitude, 250.0f);
		TestEqual(TEXT("warmup"), C.WarmupTimeoutSec, 5.0f);
		TestEqual(TEXT("catchup"), C.CatchUpTimeoutSec, 20.0f);
		TestEqual(TEXT("build"), C.BuildLabel, FString(TEXT("abc")));
	}
	// A start without Z: 500 over the generated surface.
	{
		const FVoxelBenchConfig C = Parse(TEXT("s 1000 1000 start=4000:-2500"));
		TestEqual(TEXT("start without Z: 500 over the surface"), C.StartPosition.Z, SurfaceZ(4000.0, -2500.0) + 500.0, 1.0e-3);
	}
	// dir=up: a climb along +Z at the start.
	{
		const FVoxelBenchConfig C = Parse(TEXT("up 1000 5000 dir=up start=0:0:800"));
		TestTrue(TEXT("dir=up climbs along +Z"), C.bClimb && C.Direction.Equals(FVector::UpVector) && C.PathType == EVoxelBenchPath::Straight);
		TestTrue(TEXT("dir=up: no latitude"), C.ClimbLatLonAltitude.Equals(FVector(-999.0, -999.0, 0.0)));
	}
	// A route: the whole route without a distance, a point without Z at the start's altitude.
	{
		const FVoxelBenchConfig C = Parse(TEXT("r 1000 start=0:0:900 route=1000:0/1000:1000:450"));
		TestTrue(TEXT("route"), C.PathType == EVoxelBenchPath::Route && C.RoutePoints.Num() == 2);
		TestEqual(TEXT("route: the whole route"), C.TraverseDistance, 0.0f);
		if (C.RoutePoints.Num() == 2)
		{
			TestTrue(TEXT("route: a point without Z at the start's altitude"), C.RoutePoints[0].Equals(FVector(1000.0, 0.0, 900.0)));
			TestTrue(TEXT("route: a point with Z"), C.RoutePoints[1].Equals(FVector(1000.0, 1000.0, 450.0)));
		}
	}
	// An orbit.
	{
		const FVoxelBenchConfig C = Parse(TEXT("o 1000 start=0:0:900 orbit=10:20:3000"));
		TestTrue(TEXT("orbit"), C.PathType == EVoxelBenchPath::Orbit && C.OrbitCenter.Equals(FVector2D(10.0, 20.0)) && C.OrbitRadius == 3000.0f);
		TestEqual(TEXT("orbit: one lap"), C.TraverseDistance, 0.0f);
	}
	// climb= and descend= on a heightmap world: straight up and down from the ground at (X, Y).
	{
		const FVoxelBenchConfig C = Parse(TEXT("c 2000 10000 climb=4000:-2500:700"));
		TestTrue(TEXT("climb=X:Y:Alt"), C.bClimb && !C.bDescend && C.bClimbSiteXY
			&& C.StartPosition.Equals(FVector(4000.0, -2500.0, SurfaceZ(4000.0, -2500.0) + 700.0), 1.0e-3));
		const FVoxelBenchConfig D = Parse(TEXT("d 2000 descend=4000:-2500:20000"));
		TestTrue(TEXT("descend=X:Y:Alt"), D.bClimb && D.bDescend && D.bClimbSiteXY && D.Direction.Equals(-FVector::UpVector));
		TestEqual(TEXT("descend: stops 300 over the ground"), D.TraverseDistance, 20000.0f - 300.0f);
	}
	// gc= needs a planet: refused here (a warning), the path left straight.
	{
		AddExpectedMessage(TEXT("needs a planet world and three values"), EAutomationExpectedMessageFlags::Contains, 1);
		const FVoxelBenchConfig C = Parse(TEXT("g 1000 1000 gc=30:20:90 start=0:0:900"));
		TestTrue(TEXT("gc= off a planet: a straight path from the start"), C.PathType == EVoxelBenchPath::Straight
			&& C.StartPosition.Equals(FVector(0.0, 0.0, 900.0)));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
