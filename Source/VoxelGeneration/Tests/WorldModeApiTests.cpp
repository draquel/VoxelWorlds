// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelNoiseTypes.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The world-mode API (WORLD_MODES_REVIEW_PLAN.md §14.2 M2; SPHERICAL_PLANET_REQUIREMENTS.md SP-A1, SP-A2, SP-A6,
// SP-G9). Pure CPU, runs headless.
//
// - Params: one builder per mode; the island centre is WorldOrigin + IslandCenterX/Y (WM-A1, KI3).
// - Bounds: plane unbounded, island the R + falloff footprint, planet the shell (WM-A2).
// - Frame: up, altitude above the datum, surface coordinate and its inverse, in every mode (WM-A2).
// - Fingerprint: equal configs match, a shape change does not, and the log names what changed (WM-A4).
// ---------------------------------------------------------------------------

namespace WorldModeApiTest
{
	UVoxelWorldConfiguration* MakeConfig(EWorldMode Mode)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = Mode;
		Config->VoxelSize = 100.0f;
		Config->ChunkSize = 32;
		Config->SeaLevel = 250.0f;
		Config->HeightScale = 2000.0f;
		Config->BaseHeight = 100.0f;
		Config->WorldOrigin = FVector(12345.0f, 54321.0f, 0.0f);
		Config->IslandShape = 0;
		Config->IslandRadius = 30000.0f;
		Config->IslandSizeY = 20000.0f;
		Config->IslandFalloffWidth = 10000.0f;
		Config->IslandFalloffType = 1;
		Config->IslandCenterX = -2345.0f;
		Config->IslandCenterY = 4321.0f;
		Config->IslandEdgeHeight = -3500.0f;
		Config->WorldRadius = 250000.0f;
		Config->PlanetMaxTerrainHeight = 5000.0f;
		Config->PlanetMaxTerrainDepth = 3000.0f;
		Config->PlanetHeightScale = 4000.0f;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldModeFactoryParamsTest,
	"VoxelWorlds.WorldModes.Api.FactoryParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldModeFactoryParamsTest::RunTest(const FString& Parameters)
{
	using namespace WorldModeApiTest;

	// Island: the centre is absolute, and generation, the world mode and the request share it.
	UVoxelWorldConfiguration* Island = MakeConfig(EWorldMode::IslandBowl);
	const FIslandBowlParams Params = FVoxelWorldModeFactory::MakeIslandParams(*Island);
	TestEqual(TEXT("Island centre X = WorldOrigin.X + offset"), Params.CenterX, 12345.0f - 2345.0f);
	TestEqual(TEXT("Island centre Y = WorldOrigin.Y + offset"), Params.CenterY, 54321.0f + 4321.0f);
	TestEqual(TEXT("Island radius"), Params.IslandRadius, 30000.0f);
	TestEqual(TEXT("Island falloff curve"), static_cast<int32>(Params.FalloffType), static_cast<int32>(EIslandFalloffType::Smooth));

	FVoxelNoiseGenerationRequest Request;
	FVoxelWorldModeFactory::FillGenerationRequest(*Island, Request);
	TestEqual(TEXT("Request world mode"), static_cast<int32>(Request.WorldMode), static_cast<int32>(EWorldMode::IslandBowl));
	TestEqual(TEXT("Request carries the absolute centre"), Request.IslandParams.CenterX, Params.CenterX);
	TestEqual(TEXT("Request terrain params"), Request.HeightScale, 2000.0f);

	const TSharedRef<IVoxelWorldMode> IslandMode = FVoxelWorldModeFactory::Create(*Island);
	TestEqual(TEXT("Create builds an island"), static_cast<int32>(IslandMode->GetWorldModeType()), static_cast<int32>(EWorldMode::IslandBowl));
	TestTrue(TEXT("The island mode is inside at its absolute centre"), IslandMode->IsInsideWorld(Params.CenterX, Params.CenterY));
	TestFalse(TEXT("... and outside at the raw offset's mirror beyond R + F"), IslandMode->IsInsideWorld(Params.CenterX + 40001.0, Params.CenterY));

	// Planet: centred on WorldOrigin; the request carries the planet's terrain params.
	UVoxelWorldConfiguration* Planet = MakeConfig(EWorldMode::SphericalPlanet);
	const FSphericalPlanetParams PlanetParams = FVoxelWorldModeFactory::MakePlanetParams(*Planet);
	TestTrue(TEXT("Planet centre = WorldOrigin"), PlanetParams.PlanetCenter.Equals(Planet->WorldOrigin));
	TestEqual(TEXT("Planet radius"), PlanetParams.PlanetRadius, 250000.0f);
	FVoxelNoiseGenerationRequest PlanetRequest;
	FVoxelWorldModeFactory::FillGenerationRequest(*Planet, PlanetRequest);
	TestEqual(TEXT("Planet request height scale is PlanetHeightScale"), PlanetRequest.HeightScale, 4000.0f);
	TestEqual(TEXT("Planet request sea level is 0 (radial terrain)"), PlanetRequest.SeaLevel, 0.0f);
	TestEqual(TEXT("Planet request radius"), PlanetRequest.SphericalPlanetParams.PlanetRadius, 250000.0f);
	TestEqual(TEXT("Create builds a planet"), static_cast<int32>(FVoxelWorldModeFactory::Create(*Planet)->GetWorldModeType()),
		static_cast<int32>(EWorldMode::SphericalPlanet));

	TestEqual(TEXT("Create builds a plane"),
		static_cast<int32>(FVoxelWorldModeFactory::Create(*MakeConfig(EWorldMode::InfinitePlane))->GetWorldModeType()),
		static_cast<int32>(EWorldMode::InfinitePlane));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldModeBoundsTest,
	"VoxelWorlds.WorldModes.Api.Bounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldModeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace WorldModeApiTest;

	// Plane: no edge.
	{
		const TSharedRef<IVoxelWorldMode> Plane = FVoxelWorldModeFactory::Create(*MakeConfig(EWorldMode::InfinitePlane));
		const FVoxelWorldBounds Bounds = Plane->GetWorldBounds();
		TestFalse(TEXT("Plane: unbounded"), Bounds.IsBounded());
		TestTrue(TEXT("Plane: inside 10,000 km out"), Plane->IsInsideWorld(1.0e9, -1.0e9));
		TestFalse(TEXT("Plane: no footprint box"), Bounds.GetBox2D().bIsValid != 0);
		TestTrue(TEXT("Plane: height bounds set"), Bounds.MaxZ > Bounds.MinZ);
	}

	// Island, both shapes: IsInsideWorld agrees with the bounds and with generation's own test, around the edge.
	for (const int32 Shape : { 0, 1 })
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::IslandBowl);
		Config->IslandShape = Shape;
		const FIslandBowlParams Params = FVoxelWorldModeFactory::MakeIslandParams(*Config);
		const TSharedRef<IVoxelWorldMode> Island = FVoxelWorldModeFactory::Create(*Config);
		const FVoxelWorldBounds Bounds = Island->GetWorldBounds();
		const TCHAR* Name = Shape == 0 ? TEXT("Circle") : TEXT("Rectangle");

		TestEqual(FString::Printf(TEXT("%s: shape"), Name), static_cast<int32>(Bounds.Shape),
			static_cast<int32>(Shape == 0 ? EVoxelWorldBoundsShape::Circle : EVoxelWorldBoundsShape::Rectangle));
		TestEqual(FString::Printf(TEXT("%s: centre X"), Name), Bounds.Center.X, static_cast<double>(Params.CenterX));
		TestEqual(FString::Printf(TEXT("%s: half extent X = R + F"), Name), Bounds.HalfExtent.X, 40000.0);
		TestEqual(FString::Printf(TEXT("%s: half extent Y"), Name), Bounds.HalfExtent.Y, Shape == 0 ? 40000.0 : 30000.0);

		int32 Disagreements = 0;
		int32 Inside = 0;
		for (int32 i = 0; i < 400; ++i)
		{
			const double Angle = UE_TWO_PI * i / 400.0;
			for (const double Distance : { 25000.0, 29999.0, 34000.0, 39990.0, 40010.0, 45000.0 })
			{
				const double X = Params.CenterX + Distance * FMath::Cos(Angle);
				const double Y = Params.CenterY + Distance * FMath::Sin(Angle);
				const bool bInside = Island->IsInsideWorld(X, Y);
				Inside += bInside ? 1 : 0;
				Disagreements += (bInside != Bounds.ContainsXY(X, Y)) ? 1 : 0;
				Disagreements += (bInside != FIslandBowlWorldMode::IsWithinIslandBounds(X, Y, Params)) ? 1 : 0;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: IsInsideWorld, the bounds and generation agree"), Name), Disagreements, 0);
		TestTrue(FString::Printf(TEXT("%s: some samples inside, some outside"), Name), Inside > 0 && Inside < 2400);

		// The floor generation writes beyond the edge is inside the height bounds (KI4's bounds clause).
		TestTrue(FString::Printf(TEXT("%s: the outside floor is within the height bounds"), Name),
			Bounds.MinZ <= Params.EdgeHeight - FIslandBowlWorldMode::OutsideFloorDepth);
		const FBox2D Box = Bounds.GetBox2D();
		TestTrue(FString::Printf(TEXT("%s: footprint box"), Name), Box.bIsValid
			&& FMath::IsNearlyEqual(Box.Max.X - Box.Min.X, 2.0 * Bounds.HalfExtent.X));
	}

	// Planet: the shell.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::SphericalPlanet);
		const TSharedRef<IVoxelWorldMode> Planet = FVoxelWorldModeFactory::Create(*Config);
		const FVoxelWorldBounds Bounds = Planet->GetWorldBounds();
		TestEqual(TEXT("Planet: shell"), static_cast<int32>(Bounds.Shape), static_cast<int32>(EVoxelWorldBoundsShape::Shell));
		TestEqual(TEXT("Planet: inner radius"), Bounds.InnerRadius, 247000.0f);
		TestEqual(TEXT("Planet: outer radius"), Bounds.OuterRadius, 255000.0f);
		TestTrue(TEXT("Planet: centre"), Bounds.Center.Equals(Config->WorldOrigin));
		TestTrue(TEXT("Planet: a point on the surface is inside"), Bounds.Contains(Config->WorldOrigin + FVector(0, 0, 250000)));
		TestFalse(TEXT("Planet: a point in orbit is outside"), Bounds.Contains(Config->WorldOrigin + FVector(0, 300000, 0)));
		TestTrue(TEXT("Planet: the column over the centre meets the shell"), Planet->IsInsideWorld(Config->WorldOrigin.X, Config->WorldOrigin.Y));
		TestFalse(TEXT("Planet: a column past the outer radius does not"), Planet->IsInsideWorld(Config->WorldOrigin.X + 260000.0, Config->WorldOrigin.Y));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldModeFrameTest,
	"VoxelWorlds.WorldModes.Api.Frame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldModeFrameTest::RunTest(const FString& Parameters)
{
	using namespace WorldModeApiTest;

	// Heightmap modes: up is +Z, altitude is Z above SeaLevel, the surface coordinate is (0, X, Y).
	for (const EWorldMode Mode : { EWorldMode::InfinitePlane, EWorldMode::IslandBowl })
	{
		const TSharedRef<IVoxelWorldMode> World = FVoxelWorldModeFactory::Create(*MakeConfig(Mode));
		const TCHAR* Name = Mode == EWorldMode::InfinitePlane ? TEXT("Plane") : TEXT("Island");
		const FVector P(31234.5, -9876.25, 1750.0);
		TestTrue(FString::Printf(TEXT("%s: up is +Z"), Name), World->GetUpVector(P).Equals(FVector::UpVector));
		TestEqual(FString::Printf(TEXT("%s: altitude is Z above SeaLevel"), Name), World->GetAltitude(P), 1500.0f);
		const FVoxelSurfaceCoord Coord = World->WorldToSurface(P);
		TestEqual(FString::Printf(TEXT("%s: face 0"), Name), Coord.Face, 0);
		TestTrue(FString::Printf(TEXT("%s: UV is XY"), Name), Coord.UV.Equals(FVector2D(P.X, P.Y)));
		TestTrue(FString::Printf(TEXT("%s: SurfaceToWorld inverts it"), Name),
			World->SurfaceToWorld(Coord, World->GetAltitude(P)).Equals(P, 1.0e-3));
	}

	// Planet: up is radial, altitude is above PlanetRadius, and the cube-face coordinate round-trips on every face.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::SphericalPlanet);
		const TSharedRef<IVoxelWorldMode> Planet = FVoxelWorldModeFactory::Create(*Config);
		const FVector C = Config->WorldOrigin;
		const FVector OnPole = C + FVector(0.0, 0.0, 251000.0);
		TestTrue(TEXT("Planet: up at the north pole is +Z"), Planet->GetUpVector(OnPole).Equals(FVector::UpVector, 1.0e-6));
		TestTrue(TEXT("Planet: up on the equator is radial"),
			Planet->GetUpVector(C + FVector(-251000.0, 0.0, 0.0)).Equals(FVector(-1.0, 0.0, 0.0), 1.0e-6));
		TestEqual(TEXT("Planet: altitude above PlanetRadius"), Planet->GetAltitude(OnPole), 1000.0f, 0.05f);

		FRandomStream Random(1234);
		int32 FacesSeen = 0;
		int32 RoundTripFailures = 0;
		int32 OutOfRange = 0;
		for (int32 i = 0; i < 2000; ++i)
		{
			const FVector Dir = Random.GetUnitVector();
			const float Altitude = Random.FRandRange(-2000.0f, 4000.0f);
			const FVector P = C + Dir * (250000.0 + Altitude);
			const FVoxelSurfaceCoord Coord = Planet->WorldToSurface(P);
			FacesSeen |= 1 << Coord.Face;
			OutOfRange += (FMath::Abs(Coord.UV.X) > 1.0 + 1.0e-6 || FMath::Abs(Coord.UV.Y) > 1.0 + 1.0e-6) ? 1 : 0;
			RoundTripFailures += Planet->SurfaceToWorld(Coord, Planet->GetAltitude(P)).Equals(P, 0.5) ? 0 : 1;
		}
		TestEqual(TEXT("Planet: all six faces are used"), FacesSeen, 0x3F);
		TestEqual(TEXT("Planet: UV stays in [-1, 1]"), OutOfRange, 0);
		TestEqual(TEXT("Planet: WorldToSurface and SurfaceToWorld round-trip"), RoundTripFailures, 0);
		TestEqual(TEXT("Planet: the north pole is the centre of the +Z face"), Planet->WorldToSurface(OnPole).Face, 4);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldModeFingerprintTest,
	"VoxelWorlds.WorldModes.Api.Fingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldModeFingerprintTest::RunTest(const FString& Parameters)
{
	using namespace WorldModeApiTest;

	UVoxelWorldConfiguration* Island = MakeConfig(EWorldMode::IslandBowl);
	const FVoxelWorldFingerprint Base = FVoxelWorldModeFactory::MakeFingerprint(*Island);
	AddInfo(TEXT("Fingerprint: ") + Base.ToString());
	TestTrue(TEXT("Recorded"), Base.IsRecorded());
	TestEqual(TEXT("Generation version"), Base.GenerationVersion, FVoxelWorldModeFactory::GenerationVersion);
	TestTrue(TEXT("Equal configs match"), Base.Matches(FVoxelWorldModeFactory::MakeFingerprint(*MakeConfig(EWorldMode::IslandBowl))));
	TestTrue(TEXT("Records the island radius"), Base.Shape.Contains(TEXT("IslandRadius=30000")));
	TestTrue(TEXT("Records the absolute island centre"), Base.Shape.Contains(TEXT("IslandCenter=(10000.0,58642.0)")));

	// A different radius: no match, and the description names the radius and nothing else.
	Island->IslandRadius = 20000.0f;
	const FVoxelWorldFingerprint Smaller = FVoxelWorldModeFactory::MakeFingerprint(*Island);
	TestFalse(TEXT("A different radius does not match"), Base.Matches(Smaller));
	const FString Diff = Base.DescribeDifferences(Smaller);
	AddInfo(TEXT("Radius change: ") + Diff);
	TestEqual(TEXT("The difference names the radius only"), Diff, FString(TEXT("IslandRadius=30000.0 vs 20000.0")));
	Island->IslandRadius = 30000.0f;

	// SizeY is unread by a circular island, so it is not part of its shape.
	Island->IslandSizeY = 99000.0f;
	TestTrue(TEXT("A circular island ignores SizeY"), Base.Matches(FVoxelWorldModeFactory::MakeFingerprint(*Island)));
	Island->IslandShape = 1;
	TestFalse(TEXT("A rectangle records SizeY"), Base.Matches(FVoxelWorldModeFactory::MakeFingerprint(*Island)));
	Island->IslandShape = 0;

	// The grid and the mode.
	Island->ChunkSize = 64;
	TestFalse(TEXT("A different chunk size does not match"), Base.Matches(FVoxelWorldModeFactory::MakeFingerprint(*Island)));
	Island->ChunkSize = 32;
	const FVoxelWorldFingerprint Plane = FVoxelWorldModeFactory::MakeFingerprint(*MakeConfig(EWorldMode::InfinitePlane));
	TestFalse(TEXT("A different world mode does not match"), Base.Matches(Plane));
	TestTrue(TEXT("The difference names the world mode"), Base.DescribeDifferences(Plane).StartsWith(TEXT("world mode IslandBowl vs InfinitePlane")));

	// Terrain tuning is not part of the fingerprint.
	Island->HeightScale = 9000.0f;
	TestTrue(TEXT("Terrain tuning does not change the fingerprint"), Base.Matches(FVoxelWorldModeFactory::MakeFingerprint(*Island)));

	// A save from before fingerprints: not recorded, and it matches nothing.
	const FVoxelWorldFingerprint Legacy;
	TestFalse(TEXT("A default fingerprint is unrecorded"), Legacy.IsRecorded());
	TestFalse(TEXT("An unrecorded fingerprint does not match"), Legacy.Matches(Base));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
