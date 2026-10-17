// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The planet's streaming window and horizon (WORLD_MODES_REVIEW_PLAN.md §15, WM-P6; SPHERICAL_PLANET_REQUIREMENTS.md
// SP-C1, SP-C2, SP-I2). Pure logic, runs headless.
//
// - Every column's window is its crossing of the terrain shell (+ half a chunk), so the candidates cover every chunk
//   that meets the shell within the view distance at the pole, at 45 degrees and on the equator, against a brute-force
//   enumeration, and nothing far outside the shell; with a long view distance the equator reaches past the old window's
//   cap of 16 layers.
// - A column through the inner radius has two crossings.
// - The horizon adds the peaks' tangent distance: a chunk with a peak past the viewer's own horizon stays, one past the
//   sum goes, and a viewer at the datum culls too.
// ---------------------------------------------------------------------------

namespace PlanetShellWindowTestUtils
{
	class FProbeStrategy : public FDistanceBandLODStrategy
	{
	public:
		using FDistanceBandLODStrategy::GetColumnWindow;
		using FDistanceBandLODStrategy::ShouldCullBeyondHorizon;
		using FDistanceBandLODStrategy::ShellWindowInnerRadius;
		using FDistanceBandLODStrategy::ShellWindowOuterRadius;
	};

	constexpr double ChunkWorld = 4800.0;

	/** The demo's chunk geometry and bands on a 250,000 uu planet with a +-6,500 uu shell (Review_Planet). Rooted. */
	UVoxelWorldConfiguration* MakePlanetConfig(float ViewDistance = 40000.0f, float Radius = 250000.0f, float Shell = 6500.0f)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->bEnableLOD = true;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->ViewDistance = ViewDistance;
		Config->LODBands = {
			FLODBand(0.0f, 7000.0f, 0, 1),
			FLODBand(7000.0f, 14000.0f, 1, 2),
			FLODBand(14000.0f, ViewDistance, 2, 4) };
		Config->WorldRadius = Radius;
		Config->PlanetMaxTerrainHeight = Shell;
		Config->PlanetMaxTerrainDepth = Shell;
		Config->bEnableFrustumCulling = false;
		return Config;
	}

	FLODQueryContext MakeContext(const FVector& Viewer)
	{
		FLODQueryContext Context;
		Context.ViewerPosition = Viewer;
		Context.WorldMode = EWorldMode::SphericalPlanet;
		return Context;
	}

	/** Chunk boxes meeting the spherical shell [Inner, Outer] around the origin: nearest point inside Outer, farthest outside Inner. */
	bool BoxMeetsShell(const FIntVector& Coord, double Inner, double Outer)
	{
		const FBox Box(FVector(Coord) * ChunkWorld, FVector(Coord + FIntVector(1)) * ChunkWorld);
		const double Near2 = Box.ComputeSquaredDistanceToPoint(FVector::ZeroVector);
		double Far2 = 0.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Far2 += FMath::Square(FMath::Max(FMath::Abs(Box.Min[Axis]), FMath::Abs(Box.Max[Axis])));
		}
		return Near2 <= Outer * Outer && Far2 >= Inner * Inner;
	}
}

/**
 * SP-C1, SP-I2: the candidates cover the shell at every latitude. For viewers over the datum at the north pole,
 * at 45 degrees, on the equator and at a cube corner, every chunk that meets the terrain shell within the view distance
 * and the horizon is a candidate (brute force over the whole box around the viewer), and every candidate meets the shell
 * widened by the window's slack.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetShellCoverageTest,
	"VoxelWorlds.LOD.Planet.ShellCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetShellCoverageTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	for (const float ViewDistance : { 40000.0f, 100000.0f })
	{
		UVoxelWorldConfiguration* Config = MakePlanetConfig(ViewDistance);
		FProbeStrategy Strategy;
		Strategy.Initialize(Config);
		const double R = Config->WorldRadius;
		const double Inner = R - Config->PlanetMaxTerrainDepth;
		const double Outer = R + Config->PlanetMaxTerrainHeight;
		const double HalfDiagonal = 0.5 * ChunkWorld * UE_SQRT_3;

		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		const TPair<const TCHAR*, FVector> Places[] = {
			{ TEXT("north pole"), FVector(0, 0, 1) },
			{ TEXT("45 degrees"), FVector(C45, 0, C45) },
			{ TEXT("equator"), FVector(1, 0, 0) },
			{ TEXT("cube corner"), FVector(-1, 1, 1).GetSafeNormal() },
		};
		for (const TPair<const TCHAR*, FVector>& Place : Places)
		{
			// From 1,000 uu up the horizon (80,000 uu) is inside a 100,000 uu view; from 20,000 up the view decides.
			const double Altitude = ViewDistance > 90000.0f ? 20000.0 : 1000.0;
			const FVector Viewer = Place.Value * (R + Altitude);
			const FLODQueryContext Context = MakeContext(Viewer);
			Strategy.Update(Context, 1.0f / 60.0f);
			TSet<FIntVector> Visible;
			for (const FChunkLODRequest& Request : Strategy.GetVisibleChunks(Context))
			{
				Visible.Add(Request.ChunkCoord);
			}

			// Brute force: every chunk in the box around the viewer that meets the shell, within the view distance and
			// the horizon.
			const double Horizon = FDistanceBandLODStrategy::GetPlanetHorizonDistance(R, Altitude, Config->PlanetMaxTerrainHeight);
			const int32 Reach = FMath::CeilToInt(ViewDistance / ChunkWorld) + 1;
			const FIntVector ViewerChunk(FMath::FloorToInt(Viewer.X / ChunkWorld), FMath::FloorToInt(Viewer.Y / ChunkWorld), FMath::FloorToInt(Viewer.Z / ChunkWorld));
			int32 Expected = 0, Missing = 0, MaxLayerReach = 0;
			for (int32 Z = -Reach; Z <= Reach; ++Z)
			for (int32 Y = -Reach; Y <= Reach; ++Y)
			for (int32 X = -Reach; X <= Reach; ++X)
			{
				const FIntVector Coord = ViewerChunk + FIntVector(X, Y, Z);
				const FVector Center = (FVector(Coord) + FVector(0.5)) * ChunkWorld;
				const double Dist = FVector::Dist(Center, Viewer);
				if (Dist > ViewDistance || Dist - HalfDiagonal > Horizon || !BoxMeetsShell(Coord, Inner, Outer))
				{
					continue;
				}
				++Expected;
				if (!Visible.Contains(Coord))
				{
					++Missing;
				}
				MaxLayerReach = FMath::Max(MaxLayerReach, FMath::Abs(Z));
			}
			int32 OutsideShell = 0;
			for (const FIntVector& Coord : Visible)
			{
				OutsideShell += BoxMeetsShell(Coord, Strategy.ShellWindowInnerRadius - 1.0, Strategy.ShellWindowOuterRadius + 1.0) ? 0 : 1;
			}
			AddInfo(FString::Printf(TEXT("View %.0f, %s: %d chunks meet the shell in range, %d candidates, %d missing, %d outside the window's shell; reach %d layers from the viewer's"),
				ViewDistance, Place.Key, Expected, Visible.Num(), Missing, OutsideShell, MaxLayerReach));
			TestTrue(*FString::Printf(TEXT("%s: the shell has chunks in range"), Place.Key), Expected > 100);
			TestEqual(*FString::Printf(TEXT("View %.0f, %s: every chunk meeting the shell in range is a candidate"), ViewDistance, Place.Key), Missing, 0);
			TestEqual(*FString::Printf(TEXT("View %.0f, %s: no candidate outside the window's shell"), ViewDistance, Place.Key), OutsideShell, 0);
			if (ViewDistance > 90000.0f && Place.Value.X > 0.99)
			{
				TestTrue(TEXT("Equator, view 100,000: the shell reaches past the old window's 16 layers"), MaxLayerReach > 16);
			}
		}
		Config->RemoveFromRoot();
	}
	return true;
}

/**
 * SP-C1: a column's window, directly. On a small planet (radius 20,000) a column through the inner radius crosses the
 * shell twice, above and below the centre, and both crossings are its window when in reach; a column past the outer
 * radius has none; the viewer's own layer does not move the window.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetColumnWindowTest,
	"VoxelWorlds.LOD.Planet.ColumnWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetColumnWindowTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	UVoxelWorldConfiguration* Config = MakePlanetConfig(60000.0f, 20000.0f, 2000.0f);
	FProbeStrategy Strategy;
	Strategy.Initialize(Config);
	const double Inner = Strategy.ShellWindowInnerRadius; // 20,000 - 2,000 - 2,400
	const double Outer = Strategy.ShellWindowOuterRadius; // 20,000 + 2,000 + 2,400
	TestEqual(TEXT("Window radii: the shell + half a chunk"), Inner, 15600.0, 0.01);
	TestEqual(TEXT("... outer"), Outer, 24400.0, 0.01);

	for (const double ViewerZ : { 21000.0, 25000.0, 30000.0 })
	{
		const FLODQueryContext Context = MakeContext(FVector(0.0, 0.0, ViewerZ));
		const FProbeStrategy::FColumnWindow Window = Strategy.GetColumnWindow(0, 0, Context, FProbeStrategy::EWindowUse::Load);
		// Column (0, 0) spans X, Y in [0, 4800]: Rho from 0 to 6,788; above the centre Z from sqrt(Inner^2 - 6788^2) to Outer.
		const double ZLow = FMath::Sqrt(Inner * Inner - 6788.2 * 6788.2);
		const int32 UpMin = FMath::CeilToInt32(ZLow / ChunkWorld) - 1, UpMax = FMath::FloorToInt32(Outer / ChunkWorld);
		const int32 DownMin = FMath::CeilToInt32(-Outer / ChunkWorld) - 1, DownMax = FMath::FloorToInt32(-ZLow / ChunkWorld);
		TestEqual(*FString::Printf(TEXT("Viewer at Z %.0f: upper crossing, lowest layer"), ViewerZ), Window.SlabMinZ, UpMin);
		TestEqual(*FString::Printf(TEXT("Viewer at Z %.0f: upper crossing, highest layer"), ViewerZ), Window.SlabMaxZ, UpMax);
		TestEqual(*FString::Printf(TEXT("Viewer at Z %.0f: lower crossing, lowest layer"), ViewerZ), Window.NearMinZ, DownMin);
		TestEqual(*FString::Printf(TEXT("Viewer at Z %.0f: lower crossing, highest layer"), ViewerZ), Window.NearMaxZ, DownMax);
		TestFalse(TEXT("The core between the crossings is not in the window"), Window.Contains(0));
	}

	// A column outside the outer radius has no window.
	const FProbeStrategy::FColumnWindow Outside = Strategy.GetColumnWindow(6, 0, MakeContext(FVector(0.0, 0.0, 21000.0)), FProbeStrategy::EWindowUse::Load);
	TestTrue(TEXT("A column past the outer radius has an empty window"), Outside.SlabMinZ > Outside.SlabMaxZ && Outside.NearMinZ > Outside.NearMaxZ);

	// A column at the equator's edge (Rho past the inner radius) has one crossing through the equatorial plane.
	const FProbeStrategy::FColumnWindow Edge = Strategy.GetColumnWindow(4, 0, MakeContext(FVector(21000.0, 0.0, 0.0)), FProbeStrategy::EWindowUse::Load);
	TestTrue(TEXT("A column past the inner radius has one range through the centre's plane"), Edge.Contains(0) && Edge.Contains(-1) && Edge.NearMinZ > Edge.NearMaxZ);

	// The viewer's reach clips it: from far above, the lower crossing is out of reach.
	const FProbeStrategy::FColumnWindow High = Strategy.GetColumnWindow(0, 0, MakeContext(FVector(0.0, 0.0, 70000.0)), FProbeStrategy::EWindowUse::Load);
	TestTrue(TEXT("From 70,000 up the lower crossing is out of reach"), High.NearMinZ > High.NearMaxZ && High.SlabMinZ <= High.SlabMaxZ);

	Config->RemoveFromRoot();
	return true;
}

/**
 * SP-C2: the horizon adds the peaks' tangent distance. A chunk whose nearest point is just past the viewer's own horizon
 * plus the old buffer (MaxTerrainHeight) is kept; one past sqrt(2Rh + h^2) + sqrt(2RH + H^2) goes; a viewer at the datum
 * culls past the peaks' distance (the old cull had none there); a viewer inside the planet culls nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetHorizonCullTest,
	"VoxelWorlds.LOD.Planet.HorizonCull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetHorizonCullTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	// A 100 km view distance so the horizon, not the view, decides.
	UVoxelWorldConfiguration* Config = MakePlanetConfig(200000.0f);
	FProbeStrategy Strategy;
	Strategy.Initialize(Config);
	const double R = Config->WorldRadius;
	const double H = Config->PlanetMaxTerrainHeight;
	const double HalfDiagonal = 0.5 * ChunkWorld * UE_SQRT_3;

	TestEqual(TEXT("Horizon at the datum = the peaks' tangent distance"), FDistanceBandLODStrategy::GetPlanetHorizonDistance(R, 0.0, H),
		FMath::Sqrt(2.0 * R * H + H * H), 0.01);
	TestEqual(TEXT("A viewer below the datum sees as far as one at it"), FDistanceBandLODStrategy::GetPlanetHorizonDistance(R, -3000.0, H),
		FDistanceBandLODStrategy::GetPlanetHorizonDistance(R, 0.0, H), 0.01);

	for (const double Altitude : { 2000.0, 0.0, -500.0 })
	{
		const FVector Viewer(0.0, 0.0, R + Altitude);
		const FLODQueryContext Context = MakeContext(Viewer);
		const double Horizon = FDistanceBandLODStrategy::GetPlanetHorizonDistance(R, Altitude, H);
		const double Old = FMath::Sqrt(2.0 * R * FMath::Max(0.0, Altitude) + FMath::Square(FMath::Max(0.0, Altitude))) + H;

		// Chunks on the shell along the +X meridian, by the distance of their nearest point from the viewer.
		int32 KeptPastOldBuffer = 0, CulledPastHorizon = 0, WronglyCulled = 0, WronglyKept = 0;
		for (int32 Step = 0; Step < 400; ++Step)
		{
			const double Angle = Step * 0.002;
			const FVector OnShell(FMath::Sin(Angle) * R, 0.0, FMath::Cos(Angle) * R);
			const FIntVector Coord(FMath::FloorToInt(OnShell.X / ChunkWorld), 0, FMath::FloorToInt(OnShell.Z / ChunkWorld));
			const FVector Center = (FVector(Coord) + FVector(0.5)) * ChunkWorld;
			const double Nearest = FVector::Dist(Center, Viewer) - HalfDiagonal;
			const bool bCulled = Strategy.ShouldCullBeyondHorizon(Coord, Context);
			if (Nearest <= Horizon)
			{
				WronglyCulled += bCulled ? 1 : 0;
				KeptPastOldBuffer += (!bCulled && Nearest > Old) ? 1 : 0;
			}
			else
			{
				WronglyKept += bCulled ? 0 : 1;
				CulledPastHorizon += bCulled ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("Viewer at altitude %.0f: horizon %.0f uu (old buffer %.0f); %d chunks past the old buffer kept, %d past the horizon culled"),
			Altitude, Horizon, Old, KeptPastOldBuffer, CulledPastHorizon));
		TestEqual(TEXT("Nothing within the horizon is culled"), WronglyCulled, 0);
		TestEqual(TEXT("Everything past the horizon is culled"), WronglyKept, 0);
		TestTrue(TEXT("Chunks past the old buffer now load (mountains past the viewer's horizon)"), KeptPastOldBuffer > 0);
		TestTrue(TEXT("Chunks past the horizon are culled, at the datum too"), CulledPastHorizon > 0);
	}

	// Inside the planet (below the shell) nothing is culled by the horizon.
	const FLODQueryContext Inside = MakeContext(FVector(0.0, 0.0, R - 20000.0));
	TestFalse(TEXT("A viewer inside the planet culls no shell chunk by the horizon"),
		Strategy.ShouldCullBeyondHorizon(FIntVector(36, 0, 36), Inside));

	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
