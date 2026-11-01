// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelCubeSphere.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The planet's streaming window and horizon (WORLD_MODES_REVIEW_PLAN.md §15, WM-P6, and §20, WM-BP8;
// SPHERICAL_PLANET_REQUIREMENTS.md SP-C1, SP-C2, SP-C5, SP-I2). Pure logic, runs headless.
//
// - With slab culling off the window is the terrain shell (its real bounds + half a chunk): the candidates cover every
//   chunk that meets the shell within the view distance along the ground at the pole, at 45 degrees and on the equator,
//   against a brute-force enumeration, and nothing outside the shell; with a long view distance the equator reaches past
//   the old window's cap of 16 layers.
// - A column through the inner radius has two crossings; the viewer's altitude no longer clips them (WM-BP8).
// - The horizon adds the peaks' tangent distance: a chunk with a peak past the viewer's own horizon stays, one past the
//   sum goes, and a viewer at the datum culls too.
// - With the slab (WM-BP8) the window is each chunk's surface slab: every chunk holding the surface within the view
//   distance is a candidate, and no candidate lies off the surface by more than the slack, except near the viewer; on a
//   1,000 km planet with km relief as on the small one.
// - A climbing viewer keeps its ground disk: the horizon is measured along the ground from the ground point (WM-BP8).
// - A suspended window resumes only with the camera under the cut-over, whatever the descent prefetch's lead (G49).
// ---------------------------------------------------------------------------

namespace PlanetShellWindowTestUtils
{
	class FProbeStrategy : public FDistanceBandLODStrategy
	{
	public:
		using FDistanceBandLODStrategy::GetColumnLayerRange;
		using FDistanceBandLODStrategy::ShouldCullBeyondHorizon;
		using FDistanceBandLODStrategy::ShellWindowInnerRadius;
		using FDistanceBandLODStrategy::ShellWindowOuterRadius;
		using FDistanceBandLODStrategy::GetPlanetChunkSlab;
		using FDistanceBandLODStrategy::FPlanetChunkSlab;
		using FDistanceBandLODStrategy::PlanetSlabGrid;
		using FDistanceBandLODStrategy::PlanetNearBandAngle;
	};

	constexpr double ChunkWorld = 4800.0;

	/**
	 * The demo's chunk geometry and bands on a 250,000 uu planet with a +-6,500 uu shell (Review_Planet). The terrain
	 * reaches the shell exactly (PlanetHeightScale twice it, no continentalness), so its real bounds are the shell; the
	 * terrain noise is Review_Planet's unless given. Rooted.
	 */
	UVoxelWorldConfiguration* MakePlanetConfig(float ViewDistance = 40000.0f, float Radius = 250000.0f, float Shell = 6500.0f,
		bool bSlab = false, float Frequency = 2.5e-5f, int32 Octaves = 3)
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
		Config->PlanetHeightScale = 2.0f * Shell;
		Config->BaseHeight = 0.0f;
		Config->NoiseParams.Frequency = Frequency;
		Config->NoiseParams.Octaves = Octaves;
		Config->bSurfaceSlabCulling = bSlab;
		Config->bEnableFrustumCulling = false;
		return Config;
	}

	FLODQueryContext MakeContext(const FVector& Viewer)
	{
		FLODQueryContext Context;
		Context.ViewerPosition = Viewer;
		Context.WorldMode = EWorldMode::SphericalPlanet;
		// The chunk manager's planet context has a far field (voxel.World.FarField 1); without one there is no cut-over
		// in any mode (D38, G78; LOD.Planet.CutoverFarFieldGate).
		Context.bFarFieldActive = true;
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

	FIntVector ChunkOf(const FVector& P)
	{
		return FIntVector(FMath::FloorToInt32(P.X / ChunkWorld), FMath::FloorToInt32(P.Y / ChunkWorld), FMath::FloorToInt32(P.Z / ChunkWorld));
	}

	FVector CentreOf(const FIntVector& Coord)
	{
		return (FVector(Coord) + FVector(0.5)) * ChunkWorld;
	}

	TSet<FIntVector> Candidates(const FProbeStrategy& Strategy, const FLODQueryContext& Context)
	{
		TSet<FIntVector> Out;
		for (const FChunkLODRequest& Request : Strategy.GetVisibleChunks(Context))
		{
			Out.Add(Request.ChunkCoord);
		}
		return Out;
	}

	/** The planet's terrain as generation makes it (the factory's world mode; no conditioning zones). */
	struct FTerrain
	{
		TSharedPtr<IVoxelWorldMode> Mode;
		const FSphericalPlanetWorldMode* Planet = nullptr;
		FVoxelNoiseParams Noise;
		double Radius = 0.0;

		explicit FTerrain(const UVoxelWorldConfiguration& Config)
			: Mode(FVoxelWorldModeFactory::Create(Config))
			, Noise(Config.NoiseParams)
			, Radius(Config.WorldRadius)
		{
			Planet = static_cast<const FSphericalPlanetWorldMode*>(Mode.Get());
		}

		double Altitude(const FVector& Direction) const { return Planet->GetTerrainAltitude(Direction, Noise); }
		FVector SurfacePoint(const FVector& Direction) const { return Direction * (Radius + Altitude(Direction)); }
	};

	/**
	 * Brute force: the chunks holding a surface point within the view distance along the ground, from a grid of directions
	 * a quarter chunk apart over the disk (each sample's chunk counts when its centre is within the view distance and not
	 * past the horizon, the strategy's own per-chunk tests).
	 */
	TSet<FIntVector> SurfaceChunksInDisk(const FProbeStrategy& Strategy, const FTerrain& Terrain, const FLODQueryContext& Context, double ViewDistance)
	{
		const FVector Viewer = Context.ViewerPosition;
		const FVector Up = Viewer.GetSafeNormal();
		FVector East, North;
		FVoxelCubeSphere::GetTangentFrame(Up, East, North);
		const double GroundRadius = Strategy.GetPlanetGroundRadius(Viewer);
		const double Step = ChunkWorld / 4.0;
		const double Reach = ViewDistance + ChunkWorld;
		const int32 K = FMath::CeilToInt32(Reach / Step);
		TSet<FIntVector> Out;
		for (int32 I = -K; I <= K; ++I)
		{
			for (int32 J = -K; J <= K; ++J)
			{
				const FVector2D Tangent(I * Step, J * Step);
				if (Tangent.Size() > Reach)
				{
					continue;
				}
				const FVector Direction = (Up * GroundRadius + East * Tangent.X + North * Tangent.Y).GetSafeNormal();
				const FIntVector Coord = ChunkOf(Terrain.SurfacePoint(Direction));
				if (FDistanceBandLODStrategy::GetPlanetGroundDistance(FVector::ZeroVector, GroundRadius, Viewer, CentreOf(Coord)) <= ViewDistance
					&& !Strategy.ShouldCullBeyondHorizon(Coord, Context))
				{
					Out.Add(Coord);
				}
			}
		}
		return Out;
	}

	/**
	 * Brute force: how far a chunk lies off the surface over its neighbourhood. The surface's altitude range at a 9 x 9 x 9
	 * lattice of directions (half a chunk apart) over the chunk box widened by one and a half chunks on every side (the
	 * cells the strategy reads for a chunk are about a chunk across and reach up to a cell past its corners), against the
	 * chunk's altitude extent. 0 when the extent meets the range. The lattice is half a chunk apart because the km relief
	 * of the 1,000 km planet rises up to about 100 uu between points a chunk apart (G43: a 5 x 5 x 5 lattice put three
	 * cube-corner chunks 99 uu past the slack after the noise's kernel changed).
	 */
	double DistanceOffSurface(const FTerrain& Terrain, const FIntVector& Coord)
	{
		const FVector Min = FVector(Coord) * ChunkWorld - FVector(1.5 * ChunkWorld);
		const double Size = 4.0 * ChunkWorld;
		constexpr int32 Points = 9;
		double SurfaceMin = TNumericLimits<double>::Max(), SurfaceMax = TNumericLimits<double>::Lowest();
		for (int32 X = 0; X < Points; ++X)
		for (int32 Y = 0; Y < Points; ++Y)
		for (int32 Z = 0; Z < Points; ++Z)
		{
			const double Altitude = Terrain.Altitude((Min + FVector(X, Y, Z) * (Size / (Points - 1))).GetSafeNormal());
			SurfaceMin = FMath::Min(SurfaceMin, Altitude);
			SurfaceMax = FMath::Max(SurfaceMax, Altitude);
		}
		const FBox Box(FVector(Coord) * ChunkWorld, FVector(Coord + FIntVector(1)) * ChunkWorld);
		const double Near = FMath::Sqrt(Box.ComputeSquaredDistanceToPoint(FVector::ZeroVector)) - Terrain.Radius;
		double Far2 = 0.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Far2 += FMath::Square(FMath::Max(FMath::Abs(Box.Min[Axis]), FMath::Abs(Box.Max[Axis])));
		}
		const double Far = FMath::Sqrt(Far2) - Terrain.Radius;
		return FMath::Max3(0.0, SurfaceMin - Far, Near - SurfaceMax);
	}

	/** The sites every planet test visits: the north pole, 45 degrees, the equator and a cube corner. */
	struct FSite
	{
		const TCHAR* Name;
		FVector Up;
	};
	TArray<FSite> GetSites()
	{
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		return {
			{ TEXT("north pole"), FVector(0, 0, 1) },
			{ TEXT("45 degrees"), FVector(C45, 0, C45) },
			{ TEXT("equator"), FVector(1, 0, 0) },
			{ TEXT("cube corner"), FVector(-1, 1, 1).GetSafeNormal() },
		};
	}
}

/**
 * SP-C1, SP-I2: the candidates cover the shell at every latitude. With slab culling off the window is the terrain shell:
 * for viewers over the datum at the north pole, at 45 degrees, on the equator and at a cube corner, every chunk that meets
 * the shell within the view distance along the ground (from the viewer's ground point, WM-BP8) and the horizon is a
 * candidate (brute force over the box around the ground point), and every candidate meets the shell widened by the
 * window's slack.
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
		TestEqual(TEXT("The window's shell is the terrain's real bounds + half a chunk (inner)"), Strategy.ShellWindowInnerRadius, Inner - 0.5 * ChunkWorld, 0.01);
		TestEqual(TEXT("... outer"), Strategy.ShellWindowOuterRadius, Outer + 0.5 * ChunkWorld, 0.01);

		for (const FSite& Site : GetSites())
		{
			// From 1,000 uu up the horizon (80,000 uu) is inside a 100,000 uu view; from 20,000 up the view decides.
			const double Altitude = ViewDistance > 90000.0f ? 20000.0 : 1000.0;
			const FVector Viewer = Site.Up * (R + Altitude);
			const FLODQueryContext Context = MakeContext(Viewer);
			Strategy.Update(Context, 1.0f / 60.0f);
			const TSet<FIntVector> Visible = Candidates(Strategy, Context);

			// Brute force: every chunk in the box around the ground point that meets the shell, within the view distance
			// along the ground and the horizon.
			const double GroundRadius = Strategy.GetPlanetGroundRadius(Viewer);
			const double Horizon = FDistanceBandLODStrategy::GetVisibilityHorizon(R, Altitude, Config->PlanetMaxTerrainHeight);
			const int32 Reach = FMath::CeilToInt(ViewDistance / ChunkWorld) + 2;
			const FIntVector GroundChunk = ChunkOf(Site.Up * GroundRadius);
			int32 Expected = 0, Missing = 0, MaxLayerReach = 0;
			for (int32 Z = -Reach; Z <= Reach; ++Z)
			for (int32 Y = -Reach; Y <= Reach; ++Y)
			for (int32 X = -Reach; X <= Reach; ++X)
			{
				const FIntVector Coord = GroundChunk + FIntVector(X, Y, Z);
				const FVector Center = CentreOf(Coord);
				if (FDistanceBandLODStrategy::GetPlanetGroundDistance(FVector::ZeroVector, GroundRadius, Viewer, Center) > ViewDistance
					|| FVector::Dist(Center, Viewer) - HalfDiagonal > Horizon || !BoxMeetsShell(Coord, Inner, Outer))
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
			AddInfo(FString::Printf(TEXT("View %.0f, %s: %d chunks meet the shell in range, %d candidates, %d missing, %d outside the window's shell; reach %d layers from the ground point's"),
				ViewDistance, Site.Name, Expected, Visible.Num(), Missing, OutsideShell, MaxLayerReach));
			TestTrue(*FString::Printf(TEXT("%s: the shell has chunks in range"), Site.Name), Expected > 100);
			TestEqual(*FString::Printf(TEXT("View %.0f, %s: every chunk meeting the shell in range is a candidate"), ViewDistance, Site.Name), Missing, 0);
			TestEqual(*FString::Printf(TEXT("View %.0f, %s: no candidate outside the window's shell"), ViewDistance, Site.Name), OutsideShell, 0);
			if (ViewDistance > 90000.0f && Site.Up.X > 0.99)
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
 * shell twice, above and below the centre, and both crossings are its window; a column past the outer radius has none;
 * the viewer's own layer does not move the window, and from far above the viewer's altitude no longer clips it (WM-BP8).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetColumnLayerRangeTest,
	"VoxelWorlds.LOD.Planet.ColumnWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetColumnLayerRangeTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	UVoxelWorldConfiguration* Config = MakePlanetConfig(60000.0f, 20000.0f, 2000.0f);
	FProbeStrategy Strategy;
	Strategy.Initialize(Config);
	const double Inner = Strategy.ShellWindowInnerRadius; // 20,000 - 2,000 - 2,400
	const double Outer = Strategy.ShellWindowOuterRadius; // 20,000 + 2,000 + 2,400
	TestEqual(TEXT("Window radii: the terrain's bounds (the shell here) + half a chunk"), Inner, 15600.0, 0.01);
	TestEqual(TEXT("... outer"), Outer, 24400.0, 0.01);

	for (const double ViewerZ : { 21000.0, 25000.0, 30000.0 })
	{
		const FLODQueryContext Context = MakeContext(FVector(0.0, 0.0, ViewerZ));
		const FProbeStrategy::FColumnLayerRange Window = Strategy.GetColumnLayerRange(0, 0, Context, FProbeStrategy::EWindowUse::Load);
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
	const FProbeStrategy::FColumnLayerRange Outside = Strategy.GetColumnLayerRange(6, 0, MakeContext(FVector(0.0, 0.0, 21000.0)), FProbeStrategy::EWindowUse::Load);
	TestTrue(TEXT("A column past the outer radius has an empty window"), Outside.SlabMinZ > Outside.SlabMaxZ && Outside.NearMinZ > Outside.NearMaxZ);

	// A column at the equator's edge (Rho past the inner radius) has one crossing through the equatorial plane.
	const FProbeStrategy::FColumnLayerRange Edge = Strategy.GetColumnLayerRange(4, 0, MakeContext(FVector(21000.0, 0.0, 0.0)), FProbeStrategy::EWindowUse::Load);
	TestTrue(TEXT("A column past the inner radius has one range through the centre's plane"), Edge.Contains(0) && Edge.Contains(-1) && Edge.NearMinZ > Edge.NearMaxZ);

	// The viewer's altitude does not clip it (WM-BP8; Phase 1 to P6.2 clipped it to the viewer's Z +- the view distance,
	// which emptied the window under a climbing viewer): from far above both crossings stay.
	const FProbeStrategy::FColumnLayerRange Low = Strategy.GetColumnLayerRange(0, 0, MakeContext(FVector(0.0, 0.0, 21000.0)), FProbeStrategy::EWindowUse::Load);
	const FProbeStrategy::FColumnLayerRange High = Strategy.GetColumnLayerRange(0, 0, MakeContext(FVector(0.0, 0.0, 200000.0)), FProbeStrategy::EWindowUse::Load);
	TestTrue(TEXT("From 200,000 up the column keeps both crossings"), High.NearMinZ <= High.NearMaxZ && High.SlabMinZ <= High.SlabMaxZ);
	TestTrue(TEXT("... the same as from the surface"), High.SlabMinZ == Low.SlabMinZ && High.SlabMaxZ == Low.SlabMaxZ
		&& High.NearMinZ == Low.NearMinZ && High.NearMaxZ == Low.NearMaxZ);

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

	TestEqual(TEXT("Horizon at the datum = the peaks' tangent distance"), FDistanceBandLODStrategy::GetVisibilityHorizon(R, 0.0, H),
		FMath::Sqrt(2.0 * R * H + H * H), 0.01);
	TestEqual(TEXT("A viewer below the datum sees as far as one at it"), FDistanceBandLODStrategy::GetVisibilityHorizon(R, -3000.0, H),
		FDistanceBandLODStrategy::GetVisibilityHorizon(R, 0.0, H), 0.01);

	for (const double Altitude : { 2000.0, 0.0, -500.0 })
	{
		const FVector Viewer(0.0, 0.0, R + Altitude);
		const FLODQueryContext Context = MakeContext(Viewer);
		const double Horizon = FDistanceBandLODStrategy::GetVisibilityHorizon(R, Altitude, H);
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

namespace PlanetShellWindowTestUtils
{
	/** A small planet with Review_Planet's terrain, and a 1,000 km one with Review_Planet_L's terrain noise (no continentalness). */
	struct FPlanetCase
	{
		const TCHAR* Name;
		float Radius;
		float Shell;
		float Frequency;
		int32 Octaves;
	};
	TArray<FPlanetCase> GetPlanetCases()
	{
		return {
			{ TEXT("small planet (R 250,000, terrain +-6,500)"), 250000.0f, 6500.0f, 2.5e-5f, 3 },
			{ TEXT("1,000 km planet (R 1e8, Review_Planet_L's noise, terrain +-192,000)"), 1.0e8f, 192000.0f, 3.91e-7f, 9 },
		};
	}

	/** Whether a chunk's radial extent meets its slab (the strategy's own numbers). */
	bool InItsSlab(const FProbeStrategy& Strategy, const FIntVector& Coord)
	{
		const FProbeStrategy::FPlanetChunkSlab Slab = Strategy.GetPlanetChunkSlab(Coord);
		return Slab.AltitudeMax >= Slab.SlabMin && Slab.AltitudeMin <= Slab.SlabMax;
	}
}

/**
 * WM-BP8 (SP-C5): the per-chunk radial slab. On the small planet and on a 1,000 km planet with km relief, for a viewer
 * 300 uu over the ground at the north pole, at 45 degrees, on the equator and at a cube corner:
 * - none missing: every chunk holding a surface point within the view distance along the ground is a candidate (brute
 *   force over a grid of directions a quarter chunk apart);
 * - none outside the slab except near the viewer: every candidate farther than the near radius lies within the slab's
 *   slack (1.5 chunks) of the surface within one and a half chunks of it (brute force over 125 directions), and the
 *   viewer, inside the slab, takes the near layers;
 * - the slab is no larger than the whole shell's window (slab culling off).
 * A flyer above the surface slab around it gets nothing off the slab, near it or not (G52: the near layers are air there;
 * where the shell reaches that high). And a viewer underground streams the chunks around it (the near window: three
 * chunks under the surface on the 1,000 km planet; on the small planet, whose shell is thinner, the chunks around it
 * inside the shell), with nothing else below the slab.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetSurfaceSlabTest,
	"VoxelWorlds.LOD.Planet.SurfaceSlab",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetSurfaceSlabTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	constexpr float ViewDistance = 40000.0f;
	const double Slack = 1.5 * ChunkWorld;
	for (const FPlanetCase& Case : GetPlanetCases())
	{
		UVoxelWorldConfiguration* SlabConfig = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, true, Case.Frequency, Case.Octaves);
		UVoxelWorldConfiguration* ShellConfig = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, false, Case.Frequency, Case.Octaves);
		FProbeStrategy Strategy;
		FProbeStrategy Whole;
		Strategy.Initialize(SlabConfig);
		Whole.Initialize(ShellConfig);
		const FTerrain Terrain(*SlabConfig);
		const double NearRadius = Strategy.GetNearLayersRadius();

		for (const FSite& Site : GetSites())
		{
			const FVector Viewer = Terrain.SurfacePoint(Site.Up) + Site.Up * 300.0;
			const FLODQueryContext Context = MakeContext(Viewer);
			const double ColdStart = FPlatformTime::Seconds();
			Strategy.Update(Context, 1.0f / 60.0f);
			const TSet<FIntVector> Visible = Candidates(Strategy, Context);
			const double ColdMs = (FPlatformTime::Seconds() - ColdStart) * 1000.0;
			const double WarmStart = FPlatformTime::Seconds();
			const int32 WarmCount = Candidates(Strategy, Context).Num();
			const double WarmMs = (FPlatformTime::Seconds() - WarmStart) * 1000.0;
			Whole.Update(Context, 1.0f / 60.0f);
			const int32 WholeCount = Candidates(Whole, Context).Num();

			// None missing.
			const TSet<FIntVector> Surface = SurfaceChunksInDisk(Strategy, Terrain, Context, ViewDistance);
			int32 Missing = 0;
			for (const FIntVector& Coord : Surface)
			{
				Missing += Visible.Contains(Coord) ? 0 : 1;
			}

			// None outside the slab, except near the viewer.
			const double GroundRadius = Strategy.GetPlanetGroundRadius(Viewer);
			int32 Near = 0, NearBelowSlab = 0, OffSlab = 0;
			double WorstOff = 0.0;
			for (const FIntVector& Coord : Visible)
			{
				if (FDistanceBandLODStrategy::GetPlanetGroundDistance(FVector::ZeroVector, GroundRadius, Viewer, CentreOf(Coord)) <= NearRadius)
				{
					++Near;
					NearBelowSlab += InItsSlab(Strategy, Coord) ? 0 : 1;
					continue;
				}
				const double Off = DistanceOffSurface(Terrain, Coord);
				WorstOff = FMath::Max(WorstOff, Off);
				OffSlab += (Off > Slack + 1.0) ? 1 : 0;
			}
			AddInfo(FString::Printf(TEXT("%s, %s: %d candidates (the whole shell's window: %d), %d surface chunks in the disk, %d missing; %d near the viewer (%d below their slab), %d farther off the surface than the slack (worst %.0f uu); %.1f ms cold, %.1f ms warm"),
				Case.Name, Site.Name, Visible.Num(), WholeCount, Surface.Num(), Missing, Near, NearBelowSlab, OffSlab, WorstOff, ColdMs, WarmMs));
			TestTrue(*FString::Printf(TEXT("%s, %s: the disk has surface chunks"), Case.Name, Site.Name), Surface.Num() > 50);
			TestEqual(*FString::Printf(TEXT("%s, %s: every surface chunk in range is a candidate"), Case.Name, Site.Name), Missing, 0);
			TestEqual(*FString::Printf(TEXT("%s, %s: no candidate off the surface by more than the slack, except near the viewer"), Case.Name, Site.Name), OffSlab, 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: the slab is no larger than the whole shell's window"), Case.Name, Site.Name), Visible.Num() <= WholeCount);
			TestEqual(*FString::Printf(TEXT("%s, %s: the warm query returns the same window"), Case.Name, Site.Name), WarmCount, Visible.Num());
			TestTrue(*FString::Printf(TEXT("%s, %s: 300 over the ground the viewer takes the near layers"), Case.Name, Site.Name),
				Strategy.IsPlanetNearBandActive(Viewer));

			// A flyer two chunks above the near layers' ceiling, where that is inside the shell: nothing off the slab (G52).
			const double FlyerAltitude = Strategy.GetPlanetNearLayersCeiling(Viewer) + 2.0 * ChunkWorld;
			if (FlyerAltitude + FDistanceBandLODStrategy::NearLayers * ChunkWorld < Strategy.ShellWindowOuterRadius - Case.Radius)
			{
				const FVector Flyer = Site.Up * (Case.Radius + FlyerAltitude);
				const FLODQueryContext FlyerContext = MakeContext(Flyer);
				Strategy.Update(FlyerContext, 1.0f / 60.0f);
				int32 FlyerOffSlab = 0;
				for (const FIntVector& Coord : Candidates(Strategy, FlyerContext))
				{
					FlyerOffSlab += InItsSlab(Strategy, Coord) ? 0 : 1;
				}
				AddInfo(FString::Printf(TEXT("%s, %s: a flyer %.0f over the datum (%.0f over the ground, two chunks over the near layers' ceiling): %d candidates off their slab"),
					Case.Name, Site.Name, FlyerAltitude, FlyerAltitude - Terrain.Altitude(Site.Up), FlyerOffSlab));
				TestFalse(*FString::Printf(TEXT("%s, %s: a flyer above the slab takes no near band"), Case.Name, Site.Name), Strategy.IsPlanetNearBandActive(Flyer));
				TestEqual(*FString::Printf(TEXT("%s, %s: a flyer above the slab streams nothing off the slab"), Case.Name, Site.Name), FlyerOffSlab, 0);
			}
		}

		// A viewer in a cave three chunks under the surface at 45 degrees: its own chunk and every neighbour in the shell are
		// candidates (the near window), and nothing beyond the near radius lies off the surface by more than the slack. The
		// small planet's shell (+-6,500 + half a chunk) holds few chunks that deep, so only the counts are relaxed there.
		const bool bDeepShell = Case.Shell > 4.0 * ChunkWorld;
		{
			const FVector Up = GetSites()[1].Up;
			const FVector Viewer = Terrain.SurfacePoint(Up) - Up * 3.0 * ChunkWorld;
			const FLODQueryContext Context = MakeContext(Viewer);
			Strategy.Update(Context, 1.0f / 60.0f);
			const TSet<FIntVector> Visible = Candidates(Strategy, Context);
			const FIntVector Own = ChunkOf(Viewer);
			int32 Around = 0, AroundMissing = 0;
			for (int32 DZ = -1; DZ <= 1; ++DZ)
			for (int32 DY = -1; DY <= 1; ++DY)
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const FIntVector Coord = Own + FIntVector(DX, DY, DZ);
				if (BoxMeetsShell(Coord, Strategy.ShellWindowInnerRadius, Strategy.ShellWindowOuterRadius))
				{
					++Around;
					AroundMissing += Visible.Contains(Coord) ? 0 : 1;
				}
			}
			const double GroundRadius = Strategy.GetPlanetGroundRadius(Viewer);
			int32 FarOff = 0, BelowSlab = 0;
			for (const FIntVector& Coord : Visible)
			{
				BelowSlab += InItsSlab(Strategy, Coord) ? 0 : 1;
				if (FDistanceBandLODStrategy::GetPlanetGroundDistance(FVector::ZeroVector, GroundRadius, Viewer, CentreOf(Coord)) > NearRadius)
				{
					FarOff += DistanceOffSurface(Terrain, Coord) > Slack + 1.0 ? 1 : 0;
				}
			}
			AddInfo(FString::Printf(TEXT("%s, a viewer three chunks underground: %d candidates, %d below their slab; %d of the %d chunks around it missing; %d off the surface beyond the near radius"),
				Case.Name, Visible.Num(), BelowSlab, AroundMissing, Around, FarOff));
			TestTrue(*FString::Printf(TEXT("%s: some chunks around an underground viewer are in the shell"), Case.Name), Around >= (bDeepShell ? 20 : 1));
			TestEqual(*FString::Printf(TEXT("%s: an underground viewer streams the chunks around it"), Case.Name), AroundMissing, 0);
			if (bDeepShell)
			{
				TestTrue(*FString::Printf(TEXT("%s: the near window adds chunks below the slab"), Case.Name), BelowSlab > 0);
			}
			TestEqual(*FString::Printf(TEXT("%s: nothing below the slab beyond the near radius"), Case.Name), FarOff, 0);
		}

		SlabConfig->RemoveFromRoot();
		ShellConfig->RemoveFromRoot();
	}
	return true;
}

namespace PlanetShellWindowTestUtils
{
	/**
	 * Brute force: the chunks the near layers added before G52, whatever the viewer's altitude over the slab: off their
	 * slab, meeting the shell, within the near radius along the ground and meeting the viewer's altitude ± NearLayers
	 * chunk widths (over the box around the viewer's chunk that holds the near disk).
	 */
	TSet<FIntVector> NearBandBeforeG52(const FProbeStrategy& Strategy, const FVector& Viewer, double Radius)
	{
		const double ShellLo = Strategy.ShellWindowInnerRadius - Radius;
		const double ShellHi = Strategy.ShellWindowOuterRadius - Radius;
		const double ViewerAltitude = Viewer.Size() - Radius;
		const double Reach = FDistanceBandLODStrategy::NearLayers * ChunkWorld;
		const double GroundRadius = Strategy.GetPlanetGroundRadius(Viewer);
		const int32 K = FMath::CeilToInt32(Strategy.GetNearLayersRadius() / ChunkWorld) + 3;
		const FIntVector Own = ChunkOf(Viewer);
		TSet<FIntVector> Out;
		for (int32 DX = -K; DX <= K; ++DX)
		for (int32 DY = -K; DY <= K; ++DY)
		for (int32 DZ = -K; DZ <= K; ++DZ)
		{
			const FIntVector Coord = Own + FIntVector(DX, DY, DZ);
			if (FDistanceBandLODStrategy::GetPlanetGroundDistance(FVector::ZeroVector, GroundRadius, Viewer, CentreOf(Coord)) > Strategy.GetNearLayersRadius())
			{
				continue;
			}
			const FProbeStrategy::FPlanetChunkSlab Slab = Strategy.GetPlanetChunkSlab(Coord);
			if (Slab.AltitudeMax >= ShellLo && Slab.AltitudeMin <= ShellHi && !InItsSlab(Strategy, Coord)
				&& Slab.AltitudeMax >= ViewerAltitude - Reach && Slab.AltitudeMin <= ViewerAltitude + Reach)
			{
				Out.Add(Coord);
			}
		}
		return Out;
	}

	/** Brute force: the highest surface within a ground distance of a direction (a lattice a quarter chunk apart). */
	double HighestSurfaceWithin(const FTerrain& Terrain, const FVector& Up, double Distance)
	{
		FVector East, North;
		FVoxelCubeSphere::GetTangentFrame(Up, East, North);
		const double Step = ChunkWorld / 4.0;
		const int32 K = FMath::CeilToInt32(Distance / Step);
		double Highest = TNumericLimits<double>::Lowest();
		for (int32 I = -K; I <= K; ++I)
		{
			for (int32 J = -K; J <= K; ++J)
			{
				const FVector2D Tangent(I * Step, J * Step);
				if (Tangent.Size() <= Distance)
				{
					Highest = FMath::Max(Highest, Terrain.Altitude((Up * Terrain.Radius + East * Tangent.X + North * Tangent.Y).GetSafeNormal()));
				}
			}
		}
		return Highest;
	}
}

/**
 * G52 (WORLD_MODES_REVIEW_PLAN.md §20.6): the near layers only while the viewer is not above the surface slab around it.
 * On the small planet and the 1,000 km one at the four sites:
 * - the ceiling (GetPlanetNearLayersCeiling) is the slab's top over the near disk: at least the highest surface within the
 *   near radius (brute force) plus the slack, and the same along the viewer's up-line;
 * - the band reaches no higher than the ceiling: at every stop no candidate off its slab lies wholly above it;
 * - a viewer 300 over the ground and one just under the ceiling take the band: every chunk the band added before G52 under
 *   the ceiling is a candidate, and so are the chunks the spawn's terrain wait reads (the 3 x 3 grid in the tangent plane
 *   and the chunk under the feet);
 * - a viewer just over the ceiling, two chunks over it, 20,000 over it and 100,000 over the ground (the descent bench's
 *   resume height) takes none: no candidate off its slab near it, where the old band held air (counted on the 1,000 km
 *   planet, whose shell reaches that high);
 * - climbing from 300 over the ground to two chunks over the ceiling unloads none of the band's chunks (the keep window
 *   is unchanged: no churn at the ceiling);
 * - a viewer in a cave three chunks under the surface keeps the band (chunks below their slab, the chunks around it);
 * - over a sea deeper than the slab, a swimmer at the surface keeps the band (its own chunk streams: the settle onto the
 *   sea waits for it to cook), which the same viewer without the sea would not.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetNearBandTest,
	"VoxelWorlds.LOD.Planet.NearBand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetNearBandTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	constexpr float ViewDistance = 40000.0f;
	const double Slack = 1.5 * ChunkWorld;
	for (const FPlanetCase& Case : GetPlanetCases())
	{
		UVoxelWorldConfiguration* Config = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, true, Case.Frequency, Case.Octaves);
		Config->VoxelCutoverAltitude = 1.0e9f; // the window under the cut-over (LOD.Planet.Cutover checks above it)
		FProbeStrategy Strategy;
		Strategy.Initialize(Config);
		const FTerrain Terrain(*Config);
		const double NearRadius = Strategy.GetNearLayersRadius();
		const double ShellTop = Strategy.ShellWindowOuterRadius - Case.Radius;
		const bool bThickShell = Case.Shell > 10.0 * ChunkWorld;
		int32 AirBefore = 0;
		int32 AirAboveCeilingOnTheGround = 0;

		// The off-slab candidates near a viewer, and whether the old band's chunks and the chunks around it are all in.
		struct FLook
		{
			bool bActive = false;
			int32 OffSlabNear = 0;
			int32 OldBand = 0;
			int32 OldBandAboveCeiling = 0;
			int32 OldBandMissing = 0;
			int32 OffSlabAboveCeiling = 0;
			int32 Around = 0;
			int32 AroundMissing = 0;
			int32 WaitGrid = 0;
			int32 WaitGridMissing = 0;
			int32 BelowSlab = 0;
			TSet<FIntVector> Visible;
		};
		auto Look = [&](FProbeStrategy& At, const FVector& Viewer)
		{
			FLook Out;
			const FLODQueryContext Context = MakeContext(Viewer);
			At.Update(Context, 1.0f / 60.0f);
			Out.Visible = Candidates(At, Context);
			Out.bActive = At.IsPlanetNearBandActive(Viewer);
			const double Ceiling = At.GetPlanetNearLayersCeiling(Viewer);
			const double GroundRadius = At.GetPlanetGroundRadius(Viewer);
			for (const FIntVector& Coord : Out.Visible)
			{
				if (!InItsSlab(At, Coord) && FDistanceBandLODStrategy::GetPlanetGroundDistance(FVector::ZeroVector, GroundRadius, Viewer, CentreOf(Coord)) <= NearRadius)
				{
					++Out.OffSlabNear;
					const FProbeStrategy::FPlanetChunkSlab Slab = At.GetPlanetChunkSlab(Coord);
					Out.BelowSlab += Slab.AltitudeMax < Slab.SlabMin ? 1 : 0;
					Out.OffSlabAboveCeiling += Slab.AltitudeMin > Ceiling ? 1 : 0;
				}
			}
			// The band before G52, less what lies wholly above the ceiling (air: the band reaches no higher now).
			const TSet<FIntVector> Old = NearBandBeforeG52(At, Viewer, Case.Radius);
			Out.OldBand = Old.Num();
			for (const FIntVector& Coord : Old)
			{
				if (At.GetPlanetChunkSlab(Coord).AltitudeMin > Ceiling)
				{
					++Out.OldBandAboveCeiling;
					continue;
				}
				Out.OldBandMissing += Out.Visible.Contains(Coord) ? 0 : 1;
			}
			// The spawn's terrain wait (AVCCharacterBase::InitiateChunkBasedWait on a planet): a 3 x 3 grid in the tangent
			// plane at the pawn and the chunk half a chunk under it.
			{
				const FVector Up = Viewer.GetSafeNormal();
				FVector East, North;
				FVoxelCubeSphere::GetTangentFrame(Up, East, North);
				TSet<FIntVector> Grid;
				for (int32 DX = -1; DX <= 1; ++DX)
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					Grid.Add(ChunkOf(Viewer + (East * DX + North * DY) * ChunkWorld));
				}
				Grid.Add(ChunkOf(Viewer - Up * (0.5 * ChunkWorld)));
				for (const FIntVector& Coord : Grid)
				{
					if (BoxMeetsShell(Coord, At.ShellWindowInnerRadius, At.ShellWindowOuterRadius))
					{
						++Out.WaitGrid;
						Out.WaitGridMissing += Out.Visible.Contains(Coord) ? 0 : 1;
					}
				}
			}
			const FIntVector Own = ChunkOf(Viewer);
			for (int32 DZ = -1; DZ <= 1; ++DZ)
			for (int32 DY = -1; DY <= 1; ++DY)
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const FIntVector Coord = Own + FIntVector(DX, DY, DZ);
				if (BoxMeetsShell(Coord, At.ShellWindowInnerRadius, At.ShellWindowOuterRadius))
				{
					++Out.Around;
					Out.AroundMissing += Out.Visible.Contains(Coord) ? 0 : 1;
				}
			}
			return Out;
		};

		for (const FSite& Site : GetSites())
		{
			const double GroundAltitude = Terrain.Altitude(Site.Up);
			const FVector Low = Site.Up * (Case.Radius + GroundAltitude + 300.0);
			const double Ceiling = Strategy.GetPlanetNearLayersCeiling(Low);
			const double Highest = HighestSurfaceWithin(Terrain, Site.Up, NearRadius + ChunkWorld);
			AddInfo(FString::Printf(TEXT("%s, %s: ground %.0f, highest surface within the near radius + a chunk %.0f, the band's ceiling %.0f (%.0f over the ground), the shell's top %.0f"),
				Case.Name, Site.Name, GroundAltitude, Highest, Ceiling, Ceiling - GroundAltitude, ShellTop));
			// The cells' five samples miss some of the surface between them, which the slab's slack covers: at least a chunk over
			// every sampled point.
			TestTrue(*FString::Printf(TEXT("%s, %s: the ceiling is a chunk or more over the highest surface near the viewer"), Case.Name, Site.Name), Ceiling >= Highest + ChunkWorld);
			// The envelope reads the cells whose centres lie within the near disk's widest angle plus two cell angles (2 pi / N,
			// GetPlanetSurfaceEnvelope) of the viewer's cell's centre; its samples reach a cell's half diagonal past those
			// centres, and the viewer lies up to another from its cell's centre: no more than the slack over the highest
			// surface within that radius (half a chunk for the samples). G43: the bound used to be four chunks past the near
			// radius, two short of the cells read on the 1,000 km planet, whose cells are a chunk across.
			const double CellAngle = 0.5 * UE_DOUBLE_PI / Strategy.PlanetSlabGrid.GetCellsPerEdge();
			const double ReadAngle = Strategy.PlanetNearBandAngle + 4.0 * CellAngle + UE_DOUBLE_SQRT_2 * CellAngle;
			const double HighestReached = HighestSurfaceWithin(Terrain, Site.Up, ReadAngle * Case.Radius);
			TestTrue(*FString::Printf(TEXT("%s, %s: the ceiling is the slab's slack over the surface the near disk reads"), Case.Name, Site.Name),
				Ceiling <= HighestReached + Slack + 0.5 * ChunkWorld);
			TestEqual(*FString::Printf(TEXT("%s, %s: the same ceiling along the up-line"), Case.Name, Site.Name),
				Strategy.GetPlanetNearLayersCeiling(Site.Up * (Case.Radius + GroundAltitude + 50000.0)), Ceiling, 0.01);

			struct FStop
			{
				const TCHAR* Name;
				double Altitude;
				bool bActive;
			};
			const FStop Stops[] = {
				{ TEXT("300 over the ground"), GroundAltitude + 300.0, true },
				{ TEXT("just under the ceiling"), Ceiling - 100.0, true },
				{ TEXT("just over the ceiling"), Ceiling + 100.0, false },
				{ TEXT("two chunks over the ceiling"), Ceiling + 2.0 * ChunkWorld, false },
				{ TEXT("20,000 over the ceiling"), Ceiling + 20000.0, false },
				{ TEXT("100,000 over the ground"), GroundAltitude + 100000.0, false },
			};
			FString Line;
			TSet<FIntVector> LoadedLow;
			for (const FStop& Stop : Stops)
			{
				const FVector Viewer = Site.Up * (Case.Radius + Stop.Altitude);
				const FLook L = Look(Strategy, Viewer);
				if (LoadedLow.Num() == 0)
				{
					LoadedLow = L.Visible;
				}
				Line += FString::Printf(TEXT(" | %s (%.0f): band %s, %d off their slab near it, the old band %d (%d wholly above the ceiling, %d of the rest not in), the spawn wait's %d chunks %d not in"),
					Stop.Name, Stop.Altitude, L.bActive ? TEXT("on") : TEXT("off"), L.OffSlabNear, L.OldBand, L.OldBandAboveCeiling, L.OldBandMissing,
					L.WaitGrid, L.WaitGridMissing);
				TestEqual(*FString::Printf(TEXT("%s, %s, %s: the band's state"), Case.Name, Site.Name, Stop.Name), L.bActive, Stop.bActive);
				TestEqual(*FString::Printf(TEXT("%s, %s, %s: nothing off its slab wholly above the ceiling"), Case.Name, Site.Name, Stop.Name), L.OffSlabAboveCeiling, 0);
				if (Stop.bActive)
				{
					TestEqual(*FString::Printf(TEXT("%s, %s, %s: every chunk of the band as before under the ceiling is a candidate"), Case.Name, Site.Name, Stop.Name), L.OldBandMissing, 0);
					TestEqual(*FString::Printf(TEXT("%s, %s, %s: the chunks the spawn's terrain wait reads are candidates"), Case.Name, Site.Name, Stop.Name), L.WaitGridMissing, 0);
					AirAboveCeilingOnTheGround += (Stop.Altitude == GroundAltitude + 300.0) ? L.OldBandAboveCeiling : 0;
				}
				else
				{
					TestEqual(*FString::Printf(TEXT("%s, %s, %s: nothing off its slab near the viewer"), Case.Name, Site.Name, Stop.Name), L.OffSlabNear, 0);
					AirBefore += L.OldBand;
				}
			}
			AddInfo(FString::Printf(TEXT("%s, %s:%s"), Case.Name, Site.Name, *Line));

			// Climbing from 300 over the ground to two chunks over the ceiling unloads none of the band's chunks.
			{
				const FVector Flyer = Site.Up * (Case.Radius + Ceiling + 2.0 * ChunkWorld);
				const FLODQueryContext Context = MakeContext(Flyer);
				Strategy.Update(Context, 1.0f / 60.0f);
				TArray<FIntVector> Unload;
				Strategy.GetChunksToUnload(Unload, LoadedLow, Context);
				const double GroundRadius = Strategy.GetPlanetGroundRadius(Flyer);
				const double HalfDiagonal = 0.5 * ChunkWorld * UE_DOUBLE_SQRT_3;
				int32 BandUnloaded = 0;
				for (const FIntVector& Coord : Unload)
				{
					BandUnloaded += (!InItsSlab(Strategy, Coord)
						&& FDistanceBandLODStrategy::GetPlanetGroundDistance(FVector::ZeroVector, GroundRadius, Flyer, CentreOf(Coord)) <= NearRadius
						&& FVector::Dist(CentreOf(Coord), Flyer) <= Strategy.GetUnloadDistance() - HalfDiagonal) ? 1 : 0;
				}
				TestEqual(*FString::Printf(TEXT("%s, %s: climbing over the ceiling unloads none of the band's chunks"), Case.Name, Site.Name), BandUnloaded, 0);
			}

			// A cave three chunks under the surface keeps the band.
			{
				const FLook Cave = Look(Strategy, Site.Up * (Case.Radius + GroundAltitude - 3.0 * ChunkWorld));
				AddInfo(FString::Printf(TEXT("%s, %s, a viewer three chunks underground: band %s, %d off their slab near it (%d below it), the old band %d (%d wholly above the ceiling, %d of the rest not in), %d of the %d chunks around it not in"),
					Case.Name, Site.Name, Cave.bActive ? TEXT("on") : TEXT("off"), Cave.OffSlabNear, Cave.BelowSlab, Cave.OldBand, Cave.OldBandAboveCeiling, Cave.OldBandMissing, Cave.AroundMissing, Cave.Around));
				TestTrue(*FString::Printf(TEXT("%s, %s: a viewer in a cave takes the band"), Case.Name, Site.Name), Cave.bActive);
				TestEqual(*FString::Printf(TEXT("%s, %s: a viewer in a cave streams the band as before"), Case.Name, Site.Name), Cave.OldBandMissing, 0);
				TestEqual(*FString::Printf(TEXT("%s, %s: a viewer in a cave streams the chunks around it"), Case.Name, Site.Name), Cave.AroundMissing, 0);
				if (bThickShell)
				{
					TestTrue(*FString::Printf(TEXT("%s, %s: the cave's band holds chunks below the slab"), Case.Name, Site.Name), Cave.BelowSlab > 0);
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s: the band before G52 held %d chunks of air at the stops over the ceiling, and %d wholly above it at 300 over the ground"),
			Case.Name, AirBefore, AirAboveCeilingOnTheGround));
		if (bThickShell)
		{
			TestTrue(*FString::Printf(TEXT("%s: over the ceiling the band before G52 held air inside the shell"), Case.Name), AirBefore > 0);
		}

		// A swimmer at the surface of a sea six chunks deep over the lowest seabed of 2,000 directions (where the shell
		// reaches that high).
		{
			FVector SeaSite = FVector::UpVector;
			double SeabedAltitude = TNumericLimits<double>::Max();
			for (int32 I = 0; I < 2000; ++I)
			{
				const double Z = 1.0 - 2.0 * (I + 0.5) / 2000.0;
				const double Phi = I * 2.399963229728653;
				const FVector Direction(FMath::Sqrt(1.0 - Z * Z) * FMath::Cos(Phi), FMath::Sqrt(1.0 - Z * Z) * FMath::Sin(Phi), Z);
				const double Altitude = Terrain.Altitude(Direction);
				if (Altitude < SeabedAltitude)
				{
					SeabedAltitude = Altitude;
					SeaSite = Direction;
				}
			}
			const double SeaAltitude = SeabedAltitude + 6.0 * ChunkWorld;
			if (SeaAltitude + 2.0 * ChunkWorld < ShellTop)
			{
				UVoxelWorldConfiguration* SeaConfig = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, true, Case.Frequency, Case.Octaves);
				SeaConfig->VoxelCutoverAltitude = 1.0e9f;
				SeaConfig->bEnableWaterLevel = true;
				SeaConfig->WaterRadius = static_cast<float>(Case.Radius + SeaAltitude);
				FProbeStrategy SeaStrategy;
				SeaStrategy.Initialize(SeaConfig);
				const FVector Swimmer = SeaSite * (static_cast<double>(SeaConfig->WaterRadius) + 90.0);
				const FLook Sea = Look(SeaStrategy, Swimmer);
				const FLook Dry = Look(Strategy, Swimmer);
				AddInfo(FString::Printf(TEXT("%s: a swimmer over a sea %.0f deep: band %s (ceiling %.0f), its chunk %s, %d off their slab near it; without the sea: band %s (ceiling %.0f)"),
					Case.Name, 6.0 * ChunkWorld, Sea.bActive ? TEXT("on") : TEXT("off"), SeaStrategy.GetPlanetNearLayersCeiling(Swimmer),
					Sea.Visible.Contains(ChunkOf(Swimmer)) ? TEXT("in") : TEXT("out"), Sea.OffSlabNear,
					Dry.bActive ? TEXT("on") : TEXT("off"), Strategy.GetPlanetNearLayersCeiling(Swimmer)));
				TestTrue(*FString::Printf(TEXT("%s: a swimmer at the sea's surface takes the band"), Case.Name), Sea.bActive);
				TestTrue(*FString::Printf(TEXT("%s: the swimmer's own chunk streams"), Case.Name), Sea.Visible.Contains(ChunkOf(Swimmer)));
				TestEqual(*FString::Printf(TEXT("%s: the chunks the spawn's terrain wait reads stream around the swimmer"), Case.Name), Sea.WaitGridMissing, 0);
				TestEqual(*FString::Printf(TEXT("%s: the swimmer streams the band as before"), Case.Name), Sea.OldBandMissing, 0);
				TestTrue(*FString::Printf(TEXT("%s: the sea raises the ceiling"), Case.Name),
					SeaStrategy.GetPlanetNearLayersCeiling(Swimmer) > Strategy.GetPlanetNearLayersCeiling(Swimmer));
				SeaConfig->RemoveFromRoot();
			}
			else
			{
				AddInfo(FString::Printf(TEXT("%s: a sea six chunks deep would reach past the shell (seabed %.0f, shell top %.0f): no sea case"),
					Case.Name, SeabedAltitude, ShellTop));
			}
		}
		Config->RemoveFromRoot();
	}
	return true;
}

/**
 * WM-BP8 (the planet's D8): a climbing viewer keeps its ground disk. At the four sites on both planets, a viewer climbing
 * from 300 uu to 100,000 uu over the ground keeps every chunk holding the surface within the view distance of its ground
 * point (P6.1: 92 chunks left at 40,000 uu up on the small planet, none from about 50,000); nothing streams farther than
 * the view distance along the ground; off the slab only the near window's chunks, around the viewer's altitude, and none
 * once the viewer is above the surface slab around it (G52: the band would be air; above the shell none either); and the
 * window loaded on the ground unloads none of its surface chunks as the viewer climbs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetGroundHorizonTest,
	"VoxelWorlds.LOD.Planet.GroundHorizon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetGroundHorizonTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	constexpr float ViewDistance = 40000.0f;
	for (const FPlanetCase& Case : GetPlanetCases())
	{
		UVoxelWorldConfiguration* Config = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, true, Case.Frequency, Case.Octaves);
		// The window under the voxel cut-over (WM-BP10 empties it above; LOD.Planet.Cutover checks that): out of reach here.
		Config->VoxelCutoverAltitude = 1.0e9f;
		FProbeStrategy Strategy;
		Strategy.Initialize(Config);
		const FTerrain Terrain(*Config);

		for (const FSite& Site : GetSites())
		{
			const FVector Ground = Terrain.SurfacePoint(Site.Up);
			TSet<FIntVector> GroundDisk;
			TSet<FIntVector> LoadedOnTheGround;
			FString Climb;
			for (const double Altitude : { 300.0, 10000.0, 40000.0, 100000.0 })
			{
				const FVector Viewer = Ground + Site.Up * Altitude;
				const FLODQueryContext Context = MakeContext(Viewer);
				Strategy.Update(Context, 1.0f / 60.0f);
				const TSet<FIntVector> Visible = Candidates(Strategy, Context);
				if (GroundDisk.Num() == 0)
				{
					GroundDisk = SurfaceChunksInDisk(Strategy, Terrain, Context, ViewDistance);
					LoadedOnTheGround = Visible;
				}

				const double GroundRadius = Strategy.GetPlanetGroundRadius(Viewer);
				const double ViewerAltitude = Viewer.Size() - Case.Radius;
				const double NearReach = FDistanceBandLODStrategy::NearLayers * ChunkWorld;
				int32 Missing = 0, NotInSlab = 0, NotNearViewer = 0;
				double Farthest = 0.0;
				for (const FIntVector& Coord : GroundDisk)
				{
					Missing += Visible.Contains(Coord) ? 0 : 1;
				}
				for (const FIntVector& Coord : Visible)
				{
					const double Along = FDistanceBandLODStrategy::GetPlanetGroundDistance(FVector::ZeroVector, GroundRadius, Viewer, CentreOf(Coord));
					Farthest = FMath::Max(Farthest, Along);
					if (!InItsSlab(Strategy, Coord))
					{
						++NotInSlab;
						const FProbeStrategy::FPlanetChunkSlab Slab = Strategy.GetPlanetChunkSlab(Coord);
						const bool bNear = Along <= Strategy.GetNearLayersRadius()
							&& Slab.AltitudeMax >= ViewerAltitude - NearReach && Slab.AltitudeMin <= ViewerAltitude + NearReach;
						NotNearViewer += bNear ? 0 : 1;
					}
				}
				TArray<FIntVector> Unload;
				Strategy.GetChunksToUnload(Unload, LoadedOnTheGround, Context);
				int32 GroundUnloaded = 0;
				for (const FIntVector& Coord : Unload)
				{
					GroundUnloaded += GroundDisk.Contains(Coord) ? 1 : 0;
				}
				Climb += FString::Printf(TEXT(" | %.0f up: %d candidates, %d of the disk missing, %d not in their slab, farthest %.0f along the ground, %d of the disk unloaded"),
					Altitude, Visible.Num(), Missing, NotInSlab, Farthest, GroundUnloaded);

				TestEqual(*FString::Printf(TEXT("%s, %s, %.0f up: the ground disk is kept"), Case.Name, Site.Name, Altitude), Missing, 0);
				TestTrue(*FString::Printf(TEXT("%s, %s, %.0f up: nothing past the view distance along the ground"), Case.Name, Site.Name, Altitude), Farthest <= ViewDistance + 1.0);
				TestEqual(*FString::Printf(TEXT("%s, %s, %.0f up: none of the ground's surface chunks unloads"), Case.Name, Site.Name, Altitude), GroundUnloaded, 0);
				TestEqual(*FString::Printf(TEXT("%s, %s, %.0f up: off the slab only the near window, around the viewer's altitude"), Case.Name, Site.Name, Altitude), NotNearViewer, 0);
				if (ViewerAltitude - NearReach > Strategy.ShellWindowOuterRadius - Case.Radius)
				{
					TestEqual(*FString::Printf(TEXT("%s, %s, %.0f up: above the shell nothing streams around the flyer"), Case.Name, Site.Name, Altitude), NotInSlab, 0);
				}
				if (!Strategy.IsPlanetNearBandActive(Viewer))
				{
					TestEqual(*FString::Printf(TEXT("%s, %s, %.0f up: above the surface slab nothing streams around the flyer"), Case.Name, Site.Name, Altitude), NotInSlab, 0);
				}
			}
			AddInfo(FString::Printf(TEXT("%s, %s: ground disk %d chunks%s"), Case.Name, Site.Name, GroundDisk.Num(), *Climb));
			TestTrue(*FString::Printf(TEXT("%s, %s: the ground disk has chunks"), Case.Name, Site.Name), GroundDisk.Num() > 50);
		}
		Config->RemoveFromRoot();
	}
	return true;
}

/**
 * Spherical Phase 6, WM-BP10 (D18): the voxel cut-over. Above VoxelCutoverAltitude over the surface under the
 * viewer (the view distance by default) the window is empty: no candidate, every loaded chunk to unload. Between the
 * resume altitude (0.85 of it) and the cut-over a suspended window stays suspended and an active one active
 * (hysteresis); under it the ground disk is back whole. Over open sea the cut-over is measured over the sea, not the
 * seabed (a flyer skimming the waves keeps its window). voxel.World.VoxelCutover overrides it (0: never). On the small
 * planet and the 1,000 km one at the four sites.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetCutoverTest,
	"VoxelWorlds.LOD.Planet.Cutover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetCutoverTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	// The rule.
	TestFalse(TEXT("Rule: active under the cut-over"), FDistanceBandLODStrategy::ComputeCutoverState(false, 39000.0, 40000.0));
	TestTrue(TEXT("Rule: suspended over it"), FDistanceBandLODStrategy::ComputeCutoverState(false, 40001.0, 40000.0));
	TestTrue(TEXT("Rule: a suspended window stays suspended over the resume altitude"), FDistanceBandLODStrategy::ComputeCutoverState(true, 35000.0, 40000.0));
	TestFalse(TEXT("Rule: ... and resumes under it"), FDistanceBandLODStrategy::ComputeCutoverState(true, 33000.0, 40000.0));
	TestFalse(TEXT("Rule: an active window stays active between the two"), FDistanceBandLODStrategy::ComputeCutoverState(false, 35000.0, 40000.0));
	TestFalse(TEXT("Rule: a cut-over of 0 never suspends"), FDistanceBandLODStrategy::ComputeCutoverState(false, 1.0e9, 0.0));

	constexpr float ViewDistance = 40000.0f;
	const double Resume = FDistanceBandLODStrategy::CutoverResumeFraction;
	for (const FPlanetCase& Case : GetPlanetCases())
	{
		UVoxelWorldConfiguration* Config = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, true, Case.Frequency, Case.Octaves);
		{
			// The default (VoxelCutoverAltitude 0): three view distances.
			FProbeStrategy Default;
			Default.Initialize(Config);
			TestTrue(*FString::Printf(TEXT("%s: the cut-over defaults to three view distances"), Case.Name),
				FMath::IsNearlyEqual(Default.GetConfiguredCutoverAltitude(), FDistanceBandLODStrategy::DefaultCutoverViewDistances * ViewDistance, 1.0));
		}
		// The checks below at the altitudes of a cut-over at the view distance.
		Config->VoxelCutoverAltitude = ViewDistance;
		FProbeStrategy Strategy;
		Strategy.Initialize(Config);
		const FTerrain Terrain(*Config);
		const double Cutover = Strategy.GetConfiguredCutoverAltitude();
		TestTrue(*FString::Printf(TEXT("%s: the configured cut-over"), Case.Name), FMath::IsNearlyEqual(Cutover, static_cast<double>(ViewDistance), 1.0));

		for (const FSite& Site : GetSites())
		{
			const FVector Ground = Terrain.SurfacePoint(Site.Up);
			auto Visit = [&](double Altitude, TSet<FIntVector>& OutVisible)
			{
				const FLODQueryContext Context = MakeContext(Ground + Site.Up * Altitude);
				Strategy.Update(Context, 1.0f / 60.0f);
				OutVisible = Candidates(Strategy, Context);
				return Context;
			};
			const double BetweenAltitude = 0.5 * (1.0 + Resume) * Cutover;
			const double BackAltitude = 0.5 * Resume * Cutover;
			TSet<FIntVector> OnTheGround, High, Between, Back, Rising;
			Visit(300.0, OnTheGround);
			const bool bGroundSuspended = Strategy.IsWindowSuspended();
			const FLODQueryContext HighContext = Visit(Cutover + 5000.0, High);
			const bool bHighSuspended = Strategy.IsWindowSuspended();
			TArray<FIntVector> Unload;
			Strategy.GetChunksToUnload(Unload, OnTheGround, HighContext);
			Visit(BetweenAltitude, Between);
			const bool bBetweenSuspended = Strategy.IsWindowSuspended();
			const FLODQueryContext BackContext = Visit(BackAltitude, Back);
			const bool bBackSuspended = Strategy.IsWindowSuspended();
			int32 Missing = 0;
			const TSet<FIntVector> Disk = SurfaceChunksInDisk(Strategy, Terrain, BackContext, ViewDistance);
			for (const FIntVector& Coord : Disk)
			{
				Missing += Back.Contains(Coord) ? 0 : 1;
			}
			// Climbing back up from there: active up to the cut-over.
			Visit(BetweenAltitude, Rising);
			const bool bRisingSuspended = Strategy.IsWindowSuspended();

			AddInfo(FString::Printf(TEXT("%s, %s: %d candidates at 300 up; %.0f up: %d (suspended %d), %d of %d to unload; %.0f up: %d (suspended %d); %.0f up: %d (suspended %d, %d of the disk missing); back up to %.0f: %d (suspended %d)"),
				Case.Name, Site.Name, OnTheGround.Num(), Cutover + 5000.0, High.Num(), bHighSuspended ? 1 : 0, Unload.Num(), OnTheGround.Num(),
				BetweenAltitude, Between.Num(), bBetweenSuspended ? 1 : 0, BackAltitude, Back.Num(), bBackSuspended ? 1 : 0, Missing,
				BetweenAltitude, Rising.Num(), bRisingSuspended ? 1 : 0));
			TestFalse(*FString::Printf(TEXT("%s, %s: active on the ground"), Case.Name, Site.Name), bGroundSuspended);
			TestTrue(*FString::Printf(TEXT("%s, %s: chunks on the ground"), Case.Name, Site.Name), OnTheGround.Num() > 50);
			TestTrue(*FString::Printf(TEXT("%s, %s: suspended over the cut-over"), Case.Name, Site.Name), bHighSuspended);
			TestEqual(*FString::Printf(TEXT("%s, %s: no candidate over the cut-over"), Case.Name, Site.Name), High.Num(), 0);
			TestEqual(*FString::Printf(TEXT("%s, %s: every loaded chunk unloads over the cut-over"), Case.Name, Site.Name), Unload.Num(), OnTheGround.Num());
			TestTrue(*FString::Printf(TEXT("%s, %s: still suspended over the resume altitude"), Case.Name, Site.Name), bBetweenSuspended && Between.Num() == 0);
			TestFalse(*FString::Printf(TEXT("%s, %s: resumed under it"), Case.Name, Site.Name), bBackSuspended);
			TestEqual(*FString::Printf(TEXT("%s, %s: the ground disk whole again"), Case.Name, Site.Name), Missing, 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: an active window stays active under the cut-over"), Case.Name, Site.Name), !bRisingSuspended && Rising.Num() > 0);
		}

		// Over open sea the cut-over is measured over the sea: the lowest seabed of 2,000 directions, 1.5 cut-overs under a sea.
		{
			FVector SeaSite = FVector::UpVector;
			double SeabedAltitude = TNumericLimits<double>::Max();
			for (int32 I = 0; I < 2000; ++I)
			{
				const double Z = 1.0 - 2.0 * (I + 0.5) / 2000.0;
				const double Phi = I * 2.399963229728653;
				const FVector Direction(FMath::Sqrt(1.0 - Z * Z) * FMath::Cos(Phi), FMath::Sqrt(1.0 - Z * Z) * FMath::Sin(Phi), Z);
				const double Altitude = Terrain.Altitude(Direction);
				if (Altitude < SeabedAltitude)
				{
					SeabedAltitude = Altitude;
					SeaSite = Direction;
				}
			}
			UVoxelWorldConfiguration* SeaConfig = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, true, Case.Frequency, Case.Octaves);
			SeaConfig->bEnableWaterLevel = true;
			SeaConfig->VoxelCutoverAltitude = ViewDistance;
			SeaConfig->WaterRadius = static_cast<float>(Case.Radius + SeabedAltitude + 1.5 * Cutover);
			FProbeStrategy SeaStrategy;
			SeaStrategy.Initialize(SeaConfig);
			const FVector Skimming = SeaSite * (static_cast<double>(SeaConfig->WaterRadius) + 300.0);
			SeaStrategy.Update(MakeContext(Skimming), 1.0f / 60.0f);
			AddInfo(FString::Printf(TEXT("%s: 300 over a sea %.0f deep: %.0f over the surface, suspended %d"),
				Case.Name, 1.5 * Cutover, SeaStrategy.GetViewerSurfaceAltitude(), SeaStrategy.IsWindowSuspended() ? 1 : 0));
			TestFalse(*FString::Printf(TEXT("%s: skimming a deep sea the window is active"), Case.Name), SeaStrategy.IsWindowSuspended());
			TestTrue(*FString::Printf(TEXT("%s: measured over the sea"), Case.Name), FMath::IsNearlyEqual(SeaStrategy.GetViewerSurfaceAltitude(), 300.0, 16.0));
			SeaConfig->RemoveFromRoot();
		}

		// voxel.World.VoxelCutover: an override, and 0 for never.
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.VoxelCutover")))
		{
			const float Saved = Var->GetFloat();
			const FVector Up = GetSites()[1].Up;
			const FVector Ground = Terrain.SurfacePoint(Up);
			Var->Set(0.0f, ECVF_SetByCode);
			Strategy.Update(MakeContext(Ground + Up * (10.0 * Cutover)), 1.0f / 60.0f);
			TestFalse(*FString::Printf(TEXT("%s: voxel.World.VoxelCutover 0 never suspends"), Case.Name), Strategy.IsWindowSuspended());
			Var->Set(5000.0f, ECVF_SetByCode);
			Strategy.Update(MakeContext(Ground + Up * 6000.0), 1.0f / 60.0f);
			TestTrue(*FString::Printf(TEXT("%s: voxel.World.VoxelCutover 5000 suspends at 6,000 up"), Case.Name), Strategy.IsWindowSuspended());
			Var->Set(Saved, ECVF_SetByCode);
		}
		Config->RemoveFromRoot();
	}
	return true;
}

/**
 * G49 (WORLD_MODES_REVIEW_PLAN.md §20.28): a suspended window resumes only with the camera under the cut-over, so the
 * descent prefetch's lead (the streaming viewer ahead of the camera, FLODQueryContext::ViewerLead) can bring a resume
 * forward to the cut-over but no higher.
 * - The rule (ComputeCutoverState with the camera): an active window as before (it suspends with the viewer over the
 *   cut-over, wherever the camera is); a suspended one resumes with the viewer under the resume altitude and the camera
 *   under the cut-over, and stays suspended while the camera is over it; with the camera at the viewer it is the rule
 *   without one at every altitude.
 * - A strategy on the small planet and the 1,000 km one at the four sites, the cut-over at the view distance (C): suspended
 *   at 1.5 C; the camera at 1.4 C led to a viewer at 0.7 C keeps it suspended with no candidate (the resume held), and
 *   with voxel.World.CutoverResumeGuard 0 the same context resumes it (the rule before); the camera just under C with the
 *   viewer at 0.5 C resumes it under the guard; the viewer under the resume altitude with no lead resumes it under either rule.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetCutoverResumeGuardTest,
	"VoxelWorlds.LOD.Planet.CutoverResumeGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetCutoverResumeGuardTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	// The rule, C = 40,000 (resumes below 34,000).
	TestFalse(TEXT("Rule: active, the viewer under the cut-over, the camera over it: stays active"), FDistanceBandLODStrategy::ComputeCutoverState(false, 30000.0, 40000.0, 60000.0));
	TestTrue(TEXT("Rule: active, the viewer over the cut-over: suspends"), FDistanceBandLODStrategy::ComputeCutoverState(false, 40001.0, 40000.0, 40001.0));
	TestTrue(TEXT("Rule: suspended, the viewer over the resume altitude: stays suspended"), FDistanceBandLODStrategy::ComputeCutoverState(true, 35000.0, 40000.0, 35000.0));
	TestFalse(TEXT("Rule: suspended, the viewer under the resume altitude, the camera under the cut-over: resumes"), FDistanceBandLODStrategy::ComputeCutoverState(true, 20000.0, 40000.0, 40000.0));
	TestTrue(TEXT("Rule: suspended, the viewer under the resume altitude, the camera over the cut-over: stays suspended"), FDistanceBandLODStrategy::ComputeCutoverState(true, 20000.0, 40000.0, 40001.0));
	TestFalse(TEXT("Rule: a cut-over of 0 never suspends"), FDistanceBandLODStrategy::ComputeCutoverState(true, 1.0e9, 0.0, 1.0e9));
	int32 Differ = 0;
	for (int32 I = 0; I <= 200; ++I)
	{
		const double Altitude = I * 500.0;
		for (const bool bWas : { false, true })
		{
			Differ += FDistanceBandLODStrategy::ComputeCutoverState(bWas, Altitude, 40000.0, Altitude)
				!= FDistanceBandLODStrategy::ComputeCutoverState(bWas, Altitude, 40000.0) ? 1 : 0;
		}
	}
	TestEqual(TEXT("Rule: with the camera at the viewer it is the rule without a camera (0 to 100,000, both states)"), Differ, 0);

	IConsoleVariable* GuardVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.CutoverResumeGuard"));
	TestNotNull(TEXT("voxel.World.CutoverResumeGuard exists"), GuardVar);
	if (!GuardVar)
	{
		return false;
	}
	const int32 SavedGuard = GuardVar->GetInt();

	constexpr float ViewDistance = 40000.0f;
	for (const FPlanetCase& Case : GetPlanetCases())
	{
		UVoxelWorldConfiguration* Config = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, true, Case.Frequency, Case.Octaves);
		Config->VoxelCutoverAltitude = ViewDistance;
		FProbeStrategy Strategy;
		Strategy.Initialize(Config);
		const FTerrain Terrain(*Config);
		const double Cutover = Strategy.GetConfiguredCutoverAltitude();

		for (const FSite& Site : GetSites())
		{
			const FVector Ground = Terrain.SurfacePoint(Site.Up);
			// The camera at CameraOver over the ground, the streaming viewer Lead under it (the context the chunk manager builds).
			auto Visit = [&](double CameraOver, double Lead, int32& OutCandidates)
			{
				FLODQueryContext Context = MakeContext(Ground + Site.Up * (CameraOver - Lead));
				Context.ViewerLead = Lead;
				Strategy.Update(Context, 1.0f / 60.0f);
				OutCandidates = Candidates(Strategy, Context).Num();
				return Strategy.IsWindowSuspended();
			};
			int32 High = 0, Held = 0, Before = 0, AtCutover = 0, NoLead = 0, Dummy = 0;

			GuardVar->Set(1, ECVF_SetByConsole);
			const bool bHigh = Visit(1.5 * Cutover, 0.0, High);
			const bool bHeld = Visit(1.4 * Cutover, 0.7 * Cutover, Held);
			const bool bHeldFlag = Strategy.IsCutoverResumeHeld();
			const double CameraSeen = Strategy.GetViewerSurfaceAltitude() + 0.7 * Cutover;

			GuardVar->Set(0, ECVF_SetByConsole);
			const bool bBefore = Visit(1.4 * Cutover, 0.7 * Cutover, Before);

			GuardVar->Set(1, ECVF_SetByConsole);
			Visit(1.5 * Cutover, 0.0, Dummy);
			const bool bAtCutover = Visit(Cutover - 1.0, 0.5 * Cutover, AtCutover);
			Visit(1.5 * Cutover, 0.0, Dummy);
			const bool bNoLead = Visit(0.8 * Cutover, 0.0, NoLead);

			AddInfo(FString::Printf(TEXT("%s, %s: 1.5 C suspended %d (%d candidates); camera 1.4 C, viewer 0.7 C: guard 1 suspended %d (held %d, %d candidates, camera seen %.0f over the ground), guard 0 suspended %d (%d candidates); camera 1 under C, viewer 0.5 C: suspended %d (%d); 0.8 C with no lead: suspended %d (%d)"),
				Case.Name, Site.Name, bHigh ? 1 : 0, High, bHeld ? 1 : 0, bHeldFlag ? 1 : 0, Held, CameraSeen, bBefore ? 1 : 0, Before,
				bAtCutover ? 1 : 0, AtCutover, bNoLead ? 1 : 0, NoLead));
			TestTrue(*FString::Printf(TEXT("%s, %s: suspended at 1.5 cut-overs"), Case.Name, Site.Name), bHigh && High == 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: the camera over the cut-over keeps it suspended (the resume held)"), Case.Name, Site.Name), bHeld && bHeldFlag && Held == 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: the strategy reads the camera from the lead"), Case.Name, Site.Name), FMath::IsNearlyEqual(CameraSeen, 1.4 * Cutover, 1.0));
			TestTrue(*FString::Printf(TEXT("%s, %s: with the guard off the lead resumes it (the rule before)"), Case.Name, Site.Name), !bBefore && Before > 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: the camera just under the cut-over resumes it"), Case.Name, Site.Name), !bAtCutover && AtCutover > 0);
			TestTrue(*FString::Printf(TEXT("%s, %s: under the resume altitude with no lead it resumes"), Case.Name, Site.Name), !bNoLead && NoLead > 0);
		}
		Config->RemoveFromRoot();
	}
	GuardVar->Set(SavedGuard, ECVF_SetByConsole);
	return true;
}

/**
 * G78 (world modes review §23.13): D38's gate on a planet. Without a far field in the context there is no cut-over in
 * force (GetCutoverAltitude 0, the configured altitude kept): a viewer at 1.5 cut-overs keeps the window active; with a
 * far field the same viewer suspends it; a far field going away while suspended resumes it at once.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetCutoverFarFieldGateTest,
	"VoxelWorlds.LOD.Planet.CutoverFarFieldGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetCutoverFarFieldGateTest::RunTest(const FString& Parameters)
{
	using namespace PlanetShellWindowTestUtils;

	constexpr float ViewDistance = 40000.0f;
	for (const FPlanetCase& Case : GetPlanetCases())
	{
		UVoxelWorldConfiguration* Config = MakePlanetConfig(ViewDistance, Case.Radius, Case.Shell, true, Case.Frequency, Case.Octaves);
		Config->VoxelCutoverAltitude = ViewDistance;
		FProbeStrategy Strategy;
		Strategy.Initialize(Config);
		const FTerrain Terrain(*Config);
		const double Configured = Strategy.GetConfiguredCutoverAltitude();
		TestTrue(*FString::Printf(TEXT("%s: the configured cut-over"), Case.Name), FMath::IsNearlyEqual(Configured, static_cast<double>(ViewDistance), 1.0));

		for (const FSite& Site : GetSites())
		{
			const FVector Ground = Terrain.SurfacePoint(Site.Up);
			auto Visit = [&](bool bFarField, int32& OutCandidates)
			{
				FLODQueryContext Context = MakeContext(Ground + Site.Up * (1.5 * Configured));
				Context.bFarFieldActive = bFarField;
				Strategy.Update(Context, 1.0f / 60.0f);
				OutCandidates = Candidates(Strategy, Context).Num();
				return Strategy.IsWindowSuspended();
			};
			int32 NoFarField = 0, WithFarField = 0, GoneAgain = 0;
			const bool bNoFarField = Visit(false, NoFarField);
			const double InForceWithout = Strategy.GetCutoverAltitude();
			const bool bWithFarField = Visit(true, WithFarField);
			const double InForceWith = Strategy.GetCutoverAltitude();
			const bool bGoneAgain = Visit(false, GoneAgain);
			AddInfo(FString::Printf(TEXT("%s, %s at %.0f over the surface: no far field: window %s, cut-over in force %.0f, %d candidates; with a far field: %s, %.0f, %d; the far field gone again: %s, %d"),
				Case.Name, Site.Name, 1.5 * Configured, bNoFarField ? TEXT("suspended") : TEXT("active"), InForceWithout, NoFarField,
				bWithFarField ? TEXT("suspended") : TEXT("active"), InForceWith, WithFarField, bGoneAgain ? TEXT("suspended") : TEXT("active"), GoneAgain));
			TestFalse(*FString::Printf(TEXT("%s, %s: no far field, no cut-over: active at 1.5 cut-overs"), Case.Name, Site.Name), bNoFarField);
			TestEqual(*FString::Printf(TEXT("%s, %s: no cut-over in force without a far field"), Case.Name, Site.Name), InForceWithout, 0.0);
			TestTrue(*FString::Printf(TEXT("%s, %s: the configured altitude is kept"), Case.Name, Site.Name), FMath::IsNearlyEqual(Strategy.GetConfiguredCutoverAltitude(), Configured, 1.0));
			TestTrue(*FString::Printf(TEXT("%s, %s: with a far field the same viewer suspends the window"), Case.Name, Site.Name), bWithFarField);
			TestTrue(*FString::Printf(TEXT("%s, %s: the cut-over in force with a far field"), Case.Name, Site.Name), FMath::IsNearlyEqual(InForceWith, Configured, 1.0));
			TestEqual(*FString::Printf(TEXT("%s, %s: a suspended window holds no candidate"), Case.Name, Site.Name), WithFarField, 0);
			TestFalse(*FString::Printf(TEXT("%s, %s: the far field gone, the window resumes"), Case.Name, Site.Name), bGoneAgain);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
