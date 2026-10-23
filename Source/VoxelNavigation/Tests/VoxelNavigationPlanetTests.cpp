// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSurfaceNavigationSubsystem.h"
#include "VoxelSurfacePathfinder.h"
#include "VoxelCubeSphere.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "Testing/VoxelTestChunkWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 4: surface navigation on a planet (WORLD_MODES_REVIEW_PLAN.md §18, WM-SY11; SP-G4).
//
// - RegionFrames: the tangent-plane frame round-trips, keeps cells within 2% of their size 33 km from a region's centre
//   on Review_Planet's radius, refuses points past 60 degrees; regions are a cube-sphere grid about 19.2 km across.
// - Path: on a streamed small planet at 45 degrees and across a cube-face edge, routes lie on the surface along local
//   up, end at the goal, and reuse their region's height cache; walkability follows the sea sphere.
// - Blocks: a 3D blocked box and a temporary block on the planet are routed around.
// - Plane.SubsystemUnchanged: on a plane the subsystem's routes equal FVoxelSurfacePathfinder over the old samplers,
//   bit for bit.
// ---------------------------------------------------------------------------

namespace VoxelNavPlanetTest
{
	/** The surface altitude along a direction from the analytic surface. */
	float AnalyticAltitude(UVoxelChunkManager* Manager, const FVector& Center, const FVector& Direction)
	{
		return Manager->GetSurfacePointAt(Center + Direction).Altitude;
	}

	/** A point on the surface a distance away from a site along a tangent direction. */
	FVector SurfaceAway(UVoxelChunkManager* Manager, const FVector& Center, double Radius, const FVector& Site, const FVector& Tangent, double Distance)
	{
		const FVector Direction = (Site * Radius + Tangent.GetSafeNormal() * Distance).GetSafeNormal();
		return Manager->GetSurfacePointAt(Center + Direction).Position;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavPlanetRegionFramesTest,
	"VoxelWorlds.Navigation.Planet.RegionFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNavPlanetRegionFramesTest::RunTest(const FString& Parameters)
{
	const FVector Center(1000.0, -2000.0, 500.0);
	constexpr double Radius = 250000.0;
	TestEqual(TEXT("Review_Planet: 21 regions a face edge (about 19 km)"), FVoxelCubeSphereGrid::CellsPerEdgeFor(Radius, 19200.0), 21);

	FRandomStream Random(41);
	int32 BadRoundTrip = 0, BadAxes = 0, BadScale = 0;
	double WorstScale = 0.0;
	for (int32 i = 0; i < 200; ++i)
	{
		const FVector Up = i == 0 ? FVector(0, 0, 1) : Random.GetUnitVector();
		const FVoxelPlanetNavFrame Frame = FVoxelPlanetNavFrame::Make(Center, Radius, Up);
		BadAxes += (FMath::Abs(FVector::DotProduct(Frame.East, Frame.North)) < 1.0e-9 && FMath::Abs(FVector::DotProduct(Frame.East, Frame.Up)) < 1.0e-9
			&& FMath::Abs(FVector::DotProduct(Frame.North, Frame.Up)) < 1.0e-9 && (FMath::Abs(Up.Z) > 0.9999 || Frame.North.Z > 0.0)) ? 0 : 1;

		// Round trip: a world point near the frame (within 20 degrees) at an altitude.
		const FVector Offset = (Frame.East * Random.FRandRange(-1.0, 1.0) + Frame.North * Random.FRandRange(-1.0, 1.0)) * 0.3;
		const FVector World = Center + (Frame.Up + Offset).GetSafeNormal() * (Radius + Random.FRandRange(-6000.0, 6000.0));
		FVector FramePoint;
		BadRoundTrip += (Frame.ToFrame(World, FramePoint) && FVector::Dist(Frame.ToWorld(FramePoint), World) < 1.0e-4) ? 0 : 1;

		// A 150 uu cell 33 km from the frame's centre (a 20 km search from a start at a region's corner).
		const double Angle = Random.FRandRange(0.0, 2.0 * UE_DOUBLE_PI);
		const double X = 33000.0 * FMath::Cos(Angle);
		const double Y = 33000.0 * FMath::Sin(Angle);
		const double Step = FVector::Dist(Frame.ToWorld(FVector(X, Y, 0.0)), Frame.ToWorld(FVector(X + 150.0 * FMath::Cos(Angle), Y + 150.0 * FMath::Sin(Angle), 0.0)));
		WorstScale = FMath::Max(WorstScale, FMath::Abs(Step / 150.0 - 1.0));
		BadScale += FMath::Abs(Step / 150.0 - 1.0) > 0.02 ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("A 150 uu cell 33 km from a region's centre: worst %.2f%% off its size"), 100.0 * WorstScale));
	TestEqual(TEXT("Frame axes orthonormal, north toward +Z"), BadAxes, 0);
	TestEqual(TEXT("ToFrame and ToWorld invert"), BadRoundTrip, 0);
	TestEqual(TEXT("Cells within 2% of their size 33 km out"), BadScale, 0);

	const FVoxelPlanetNavFrame Frame = FVoxelPlanetNavFrame::Make(Center, Radius, FVector(1, 0, 0));
	FVector FramePoint;
	TestFalse(TEXT("Past 60 degrees a point is outside the frame"), Frame.ToFrame(Center + FVector(0.3, 1.0, 0.0).GetSafeNormal() * Radius, FramePoint));
	TestTrue(TEXT("The frame's centre is (0, 0) at its altitude"), Frame.ToFrame(Center + FVector(Radius + 250.0, 0, 0), FramePoint)
		&& FramePoint.Equals(FVector(0.0, 0.0, 250.0), 1.0e-6));
	return true;
}

namespace VoxelNavPlanetTest
{
	/**
	 * Path's checks on a streamed planet of a radius: the small one, and a 1,000 km one for spherical Phase 6 (WM-BP11: the
	 * surface queries, the region frames and the walkability on the precise altitude at scale).
	 */
	void RunPlanetPath(FAutomationTestBase& Test, float PlanetRadius, const TCHAR* WorldName)
	{
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		struct FSite { const TCHAR* Name; FVector Direction; FVector Tangent; };
		const TArray<FSite> Sites = {
			{ TEXT("45 degrees"), FVector(C45, 0.0, C45), FVector(0, 1, 0) },
			// Across the +X / +Y face edge: from face +X to face +Y.
			{ TEXT("cube-face edge"), FVector(1, 1, 0.2).GetSafeNormal(), FVector(-1, 1, 0) },
		};
		for (const FSite& Site : Sites)
		{
			VoxelTesting::FTestChunkWorld W;
			W.Create(WorldName);
			W.ConfigureSmallPlanet(PlanetRadius);
			W.Config->PlanetHeightScale = 600.0f; // terrain within +-300: walkable slopes, some water
			W.Config->bEnableWaterLevel = true;
			W.Config->WaterRadius = W.Config->WorldRadius - 150.0f;
			W.StartManager();
			const FVector Center = W.Config->WorldOrigin;
			const double Radius = W.Config->WorldRadius;
			const float VoxelSize = W.Config->VoxelSize;
			W.SetViewer(Center + Site.Direction * (Radius + 300.0));
			UVoxelSurfaceNavigationSubsystem* Nav = W.World->GetSubsystem<UVoxelSurfaceNavigationSubsystem>();
			if (!Test.TestNotNull(TEXT("The game world has navigation"), Nav) || !Test.TestTrue(*FString::Printf(TEXT("%s: streaming settles"), Site.Name), W.Settle()))
			{
				W.Stop();
				continue;
			}
			Nav->SetChunkManager(W.Manager);

			// A chord through the site whose ground is dry along its length (turned up to 60 degrees from the given tangent, so
			// the edge site's chord still crosses the edge).
			FVector Tangent = Site.Tangent;
			for (const double Turn : { 0.0, 20.0, -20.0, 40.0, -40.0, 60.0, -60.0 })
			{
				const FVector Candidate = Site.Tangent.RotateAngleAxis(Turn, Site.Direction);
				bool bDry = true;
				for (int32 s = 0; s <= 20 && bDry; ++s)
				{
					const FVector Ground = SurfaceAway(W.Manager, Center, Radius, Site.Direction, Candidate, -700.0 + 70.0 * s);
					bDry = Radius + W.Manager->GetAltitudeAt(Ground) > W.Config->WaterRadius + 60.0;
				}
				if (bDry)
				{
					Tangent = Candidate;
					break;
				}
			}
			const FVector From = SurfaceAway(W.Manager, Center, Radius, Site.Direction, Tangent, -700.0);
			const FVector To = SurfaceAway(W.Manager, Center, Radius, Site.Direction, Tangent, 700.0);
			TArray<FVector> Points;
			FVoxelSurfacePathStats Stats;
			const bool bOk = Nav->FindPathWithStats(From, To, Points, Stats);
			Test.AddInfo(FString::Printf(TEXT("%s: %s, %d points, %d nodes, %d samples%s; from face %d to face %d"), Site.Name, bOk ? TEXT("ok") : TEXT("FAILED"),
				Points.Num(), Stats.NodesExpanded, Stats.HeightSamples, Stats.bPartial ? TEXT(" (partial)") : TEXT(""),
				FVoxelCubeSphere::DirectionToSurface((From - Center).GetSafeNormal()).Face, FVoxelCubeSphere::DirectionToSurface((To - Center).GetSafeNormal()).Face));
			if (!Test.TestTrue(*FString::Printf(TEXT("%s: a route"), Site.Name), bOk && Points.Num() > 0))
			{
				W.Stop();
				continue;
			}
			double WorstOff = 0.0, Length = 0.0;
			FVector Previous = From;
			for (const FVector& Point : Points)
			{
				const FVector Direction = (Point - Center).GetSafeNormal();
				WorstOff = FMath::Max(WorstOff, static_cast<double>(FMath::Abs(W.Manager->GetAltitudeAt(Point) - AnalyticAltitude(W.Manager, Center, Direction))));
				Length += FVector::Dist(Previous, Point);
				Previous = Point;
			}
			const double Straight = FVector::Dist(From, To);
			Test.AddInfo(FString::Printf(TEXT("%s: worst waypoint %.1f uu off the surface along up; length %.0f against %.0f straight"), Site.Name, WorstOff, Length, Straight));
			Test.TestTrue(*FString::Printf(TEXT("%s: waypoints on the surface along local up (within a voxel)"), Site.Name), WorstOff <= VoxelSize);
			Test.TestTrue(*FString::Printf(TEXT("%s: the route ends within a cell of the goal"), Site.Name), FVector::Dist(Points.Last(), To) <= 2.0 * Nav->GetParams().CellSize + VoxelSize);
			Test.TestTrue(*FString::Printf(TEXT("%s: the route is not a detour (under 1.6x the straight line)"), Site.Name), Length <= 1.6 * Straight);
			Test.TestFalse(*FString::Printf(TEXT("%s: complete"), Site.Name), Stats.bPartial);

			// The same start reuses its region's cache: a second identical search samples nothing new.
			const int32 Cells = Nav->GetCachedCellCount();
			Nav->FindPathWithStats(From, To, Points, Stats);
			Test.TestEqual(*FString::Printf(TEXT("%s: a second search reuses the region's heights"), Site.Name), Nav->GetCachedCellCount(), Cells);

			// Spherical Phase 6, P6.5: a box at the planet's centre is no surface and clears nothing (projected into every
			// region's frame it spanned the planet: minutes a stamp on the 1,000 km planet); a system edit, which gives no
			// centre (zero, radius 0), clears the edited chunk's cells; both at once.
			{
				const int32 Before = Nav->GetCachedCellCount();
				const double T0 = FPlatformTime::Seconds();
				Nav->InvalidateBox(FBox(Center - FVector(200.0), Center + FVector(200.0)));
				const int32 AfterCore = Nav->GetCachedCellCount();
				UVoxelEditManager* Edits = W.Manager->GetEditManager();
				if (Test.TestNotNull(TEXT("An edit manager"), Edits))
				{
					Edits->OnChunkEdited.Broadcast(W.Manager->WorldToChunkCoord(From), EEditSource::System, FVector::ZeroVector, 0.0f);
				}
				const double Seconds = FPlatformTime::Seconds() - T0;
				const int32 AfterEdit = Nav->GetCachedCellCount();
				Test.AddInfo(FString::Printf(TEXT("%s: %d cached cells; a box at the centre clears %d, a system edit of the start's chunk %d, in %.4f s"),
					Site.Name, Before, Before - AfterCore, AfterCore - AfterEdit, Seconds));
				Test.TestEqual(*FString::Printf(TEXT("%s: a box at the planet's centre clears nothing"), Site.Name), AfterCore, Before);
				Test.TestTrue(*FString::Printf(TEXT("%s: a system edit clears the edited chunk's cells"), Site.Name), AfterEdit < AfterCore);
				Test.TestTrue(*FString::Printf(TEXT("%s: invalidation in bounded time"), Site.Name), Seconds < 0.5);
			}

			// Walkability follows the sea sphere.
			FRandomStream Random(7);
			int32 Disagree = 0, Under = 0;
			for (int32 i = 0; i < 200; ++i)
			{
				const FVector Around = FVector::CrossProduct(Site.Direction, Random.GetUnitVector()).GetSafeNormal();
				const FVector Point = SurfaceAway(W.Manager, Center, Radius, Site.Direction, Around, Random.FRandRange(0.0, 2000.0));
				FVector OnSurface;
				Nav->ProjectToSurface(Point + (Point - Center).GetSafeNormal() * 500.0, OnSurface);
				const double Altitude = W.Manager->GetAltitudeAt(OnSurface);
				const bool bExpectedWet = Radius + Altitude < W.Config->WaterRadius - Nav->GetParams().MaxWadeDepth - 30.0;
				const bool bExpectedDry = Radius + Altitude > W.Config->WaterRadius - Nav->GetParams().MaxWadeDepth + 30.0;
				const bool bWalkable = Nav->IsWalkableAt(OnSurface);
				Under += bExpectedWet ? 1 : 0;
				Disagree += ((bExpectedWet && bWalkable) || (bExpectedDry && !bWalkable)) ? 1 : 0;
			}
			Test.AddInfo(FString::Printf(TEXT("%s: walkability at 200 points (%d under the sea): %d disagree with the sea sphere"), Site.Name, Under, Disagree));
			Test.TestEqual(*FString::Printf(TEXT("%s: walkable exactly where the sea sphere leaves ground"), Site.Name), Disagree, 0);
			W.Stop();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavPlanetPathTest,
	"VoxelWorlds.Navigation.Planet.Path",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNavPlanetPathTest::RunTest(const FString& Parameters)
{
	VoxelNavPlanetTest::RunPlanetPath(*this, 20000.0f, TEXT("VoxelNavPlanetPath"));
	return true;
}

/**
 * Spherical Phase 6, WM-BP11: Path on a 1,000 km planet (R 10^8): routes on the surface along local up within a voxel,
 * the region caches and the walkability against the sea sphere, with every altitude and frame point formed at that radius.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavPlanetPathAtScaleTest,
	"VoxelWorlds.Navigation.Planet.PathAtScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNavPlanetPathAtScaleTest::RunTest(const FString& Parameters)
{
	VoxelNavPlanetTest::RunPlanetPath(*this, 1.0e8f, TEXT("VoxelNavPlanetPathAtScale"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavPlanetBlocksTest,
	"VoxelWorlds.Navigation.Planet.Blocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNavPlanetBlocksTest::RunTest(const FString& Parameters)
{
	using namespace VoxelNavPlanetTest;
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("VoxelNavPlanetBlocks"));
	W.ConfigureSmallPlanet();
	W.Config->PlanetHeightScale = 400.0f; // gentle ground: the straight route is open
	W.StartManager();
	const FVector Center = W.Config->WorldOrigin;
	const double Radius = W.Config->WorldRadius;
	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const FVector Site(C45, 0.0, C45);
	W.SetViewer(Center + Site * (Radius + 300.0));
	UVoxelSurfaceNavigationSubsystem* Nav = W.World->GetSubsystem<UVoxelSurfaceNavigationSubsystem>();
	if (TestNotNull(TEXT("The game world has navigation"), Nav) && TestTrue(TEXT("Streaming settles"), W.Settle()))
	{
		Nav->SetChunkManager(W.Manager);
		const FVector From = SurfaceAway(W.Manager, Center, Radius, Site, FVector(0, 1, 0), -800.0);
		const FVector To = SurfaceAway(W.Manager, Center, Radius, Site, FVector(0, 1, 0), 800.0);
		const FVector Middle = SurfaceAway(W.Manager, Center, Radius, Site, FVector(0, 1, 0), 0.0);
		auto RouteNear = [&](const TArray<FVector>& Points, const FVector& Point, double Distance)
		{
			FVector Previous = From;
			for (const FVector& Next : Points)
			{
				for (int32 s = 0; s <= 40; ++s)
				{
					if (FVector::Dist(FMath::Lerp(Previous, Next, s / 40.0), Point) < Distance)
					{
						return true;
					}
				}
				Previous = Next;
			}
			return false;
		};

		TArray<FVector> Open;
		TestTrue(TEXT("The open route"), Nav->FindPath(From, To, Open) && RouteNear(Open, Middle, 200.0));

		// A box 600 uu wide around the middle, the full height of the terrain.
		const FVector Up = (Middle - Center).GetSafeNormal();
		const int32 Handle = Nav->AddBlockedBox(FBox(Middle - FVector(300.0, 300.0, 300.0), Middle + FVector(300.0, 300.0, 300.0)));
		TArray<FVector> Around;
		const bool bAround = Nav->FindPath(From, To, Around);
		TestTrue(TEXT("Around the box: a route that keeps out of it"), bAround && !RouteNear(Around, Middle, 250.0));
		TestFalse(TEXT("The box's middle is not walkable"), Nav->IsWalkableAt(Middle));
		Nav->RemoveBlockedBox(Handle);
		TestTrue(TEXT("Without the box the middle is walkable again"), Nav->IsWalkableAt(Middle));

		// A temporary block of 300 uu at the middle.
		Nav->BlockCellsTemporarily(Middle + Up * 50.0, 300.0f, 30.0f);
		TArray<FVector> AroundTemp;
		TestFalse(TEXT("The temporary block's middle is not walkable"), Nav->IsWalkableAt(Middle));
		TestTrue(TEXT("Around the temporary block"), Nav->FindPath(From, To, AroundTemp) && !RouteNear(AroundTemp, Middle, 250.0));
		AddInfo(FString::Printf(TEXT("Open route %d points, around the box %d, around the temporary block %d"), Open.Num(), Around.Num(), AroundTemp.Num()));
	}
	W.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavPlaneSubsystemUnchangedTest,
	"VoxelWorlds.Navigation.Plane.SubsystemUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNavPlaneSubsystemUnchangedTest::RunTest(const FString& Parameters)
{
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("VoxelNavPlaneUnchanged"));
	W.ConfigureSmallPlane();
	W.StartManager();
	W.SetViewer(FVector(0.0, 0.0, 600.0));
	UVoxelSurfaceNavigationSubsystem* Nav = W.World->GetSubsystem<UVoxelSurfaceNavigationSubsystem>();
	if (TestNotNull(TEXT("The game world has navigation"), Nav) && TestTrue(TEXT("Streaming settles"), W.Settle()))
	{
		Nav->SetChunkManager(W.Manager);
		UVoxelChunkManager* Manager = W.Manager;
		FRandomStream Random(13);
		int32 Identical = 0, Routes = 0;
		for (int32 i = 0; i < 8; ++i)
		{
			const FVector From(Random.FRandRange(-2500.0, 2500.0), Random.FRandRange(-2500.0, 2500.0), 0.0);
			const FVector To(Random.FRandRange(-2500.0, 2500.0), Random.FRandRange(-2500.0, 2500.0), 0.0);
			TArray<FVector> Got;
			const bool bGot = Nav->FindPath(From, To, Got);

			// The old construction: the cell's height from QueryEditMergedSurface at its centre, else the generated
			// height; outside the world blocked.
			const FVoxelSurfaceNavParams Params = Nav->GetParams();
			FVoxelSurfacePathfinder Old(Params,
				[Manager, &Params](double X, double Y)
				{
					const FIntPoint Cell(FMath::FloorToInt(X / FMath::Max(Params.CellSize, 1.f)), FMath::FloorToInt(Y / FMath::Max(Params.CellSize, 1.f)));
					const double CX = (Cell.X + 0.5) * Params.CellSize;
					const double CY = (Cell.Y + 0.5) * Params.CellSize;
					float Height = 0.f;
					FVector Normal;
					float Slope = 0.f;
					uint8 Material = 0, Biome = 0;
					return Manager->QueryEditMergedSurface(CX, CY, Height, Normal, Slope, Material, Biome) ? Height : Manager->GetGeneratedSurfaceHeight(CX, CY);
				},
				[Manager, &Params](const FIntPoint& Cell)
				{
					return !Manager->IsInsideWorld((Cell.X + 0.5) * Params.CellSize, (Cell.Y + 0.5) * Params.CellSize);
				});
			TArray<FVector> Expected;
			const bool bExpected = Old.FindPath(From, To, Expected);
			Routes += bExpected ? 1 : 0;
			Identical += (bGot == bExpected && Got == Expected) ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("8 searches (%d routes): %d identical to the old construction"), Routes, Identical));
		TestTrue(TEXT("Some routes"), Routes >= 4);
		TestEqual(TEXT("Every route is the old construction's, bit for bit"), Identical, 8);
	}
	W.Stop();
	return true;
}

/**
 * G19 (WORLD_MODES_REVIEW_PLAN.md §14.1): on a heightmap world navigation's sea is the plane generation floods,
 * WaterLevel itself, wherever WorldOrigin puts the chunk grid: a cell's ground deeper under it than a walker wades is
 * water, ground just short of that depth is not.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavPlaneSeaWithOriginTest,
	"VoxelWorlds.Navigation.Plane.SeaWithOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNavPlaneSeaWithOriginTest::RunTest(const FString& Parameters)
{
	for (const double OriginZ : { -3000.0, 2600.0 })
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("VoxelNavPlaneSeaWithOrigin"));
		W.ConfigureSmallPlane();
		W.Config->WorldOrigin = FVector(12345.0, 54321.0, OriginZ);
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = 0.0f;
		W.StartManager();
		W.SetViewer(FVector(W.Config->WorldOrigin.X, W.Config->WorldOrigin.Y, 200.0));
		UVoxelSurfaceNavigationSubsystem* Nav = W.World->GetSubsystem<UVoxelSurfaceNavigationSubsystem>();
		const FString Name = FString::Printf(TEXT("WorldOrigin.Z %.0f"), OriginZ);
		if (TestNotNull(TEXT("The game world has navigation"), Nav) && TestTrue(*FString::Printf(TEXT("%s: streaming settles"), *Name), W.Settle()))
		{
			Nav->SetChunkManager(W.Manager);
			Nav->IsWalkableAt(FVector(W.Config->WorldOrigin.X, W.Config->WorldOrigin.Y, 0.0)); // refreshes the parameters
			const FVoxelSurfaceNavParams Params = Nav->GetParams();
			const double X = W.Config->WorldOrigin.X, Y = W.Config->WorldOrigin.Y;
			const float Deep = W.Config->WaterLevel - Params.MaxWadeDepth - 10.0f;
			const float Shallow = W.Config->WaterLevel - Params.MaxWadeDepth + 10.0f;
			AddInfo(FString::Printf(TEXT("%s: navigation's sea %.0f (WaterLevel %.0f), wade depth %.0f"), *Name, Params.Sea.PlaneZ,
				W.Config->WaterLevel, Params.MaxWadeDepth));
			TestEqual(*FString::Printf(TEXT("%s: navigation's sea is the WaterLevel plane"), *Name), Params.Sea.PlaneZ, static_cast<double>(W.Config->WaterLevel));
			TestTrue(*FString::Printf(TEXT("%s: ground deeper than a wade is water"), *Name), Params.IsUnderwater(X, Y, Deep));
			TestFalse(*FString::Printf(TEXT("%s: ground short of it is not"), *Name), Params.IsUnderwater(X, Y, Shallow));
		}
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
