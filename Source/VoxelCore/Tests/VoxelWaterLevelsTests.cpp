// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSeaSurface.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The water levels (the water plan's W2): a snapshot of the world's bodies of water, with the sea as its one body, read
// through the sample at a position, the range over bounds and the sea. W2 changes no output, so with the sea alone the
// three must be the sea surface's own answers, bit for bit:
//
// - Core.WaterLevels.SeaOnPlaneAndPlanet: a plane world and a planet, each from its configuration. The sample's depth
//   and the cheap GetDepth are FVoxelSeaSurface::GetDepth, the surface level is the plane's Z or the sphere's radius,
//   the normal is the local up; IsUnder is IsBelow (at points within 2 uu of the sphere too, where the depth's sign is
//   not the squared-distance test); IsAbove and Crosses are the sea's over random boxes; the range over any box is
//   uniform at the sea's level and names the box. The snapshot has one body, the sea, over the whole world.
// - Core.WaterLevels.NoWater: water off, and a default snapshot: no body, no sample (its depth far above any surface),
//   an empty range, nothing under and every box above.
// ---------------------------------------------------------------------------

namespace VoxelWaterLevelsTestUtils
{
	UVoxelWorldConfiguration* MakeConfig(EWorldMode Mode, bool bWater)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = Mode;
		Config->bEnableWaterLevel = bWater;
		Config->WaterLevel = -1500.0f;
		Config->WorldRadius = 250000.0f;
		Config->WaterRadius = 249500.0f;
		Config->WorldOrigin = Mode == EWorldMode::SphericalPlanet ? FVector(100000.0, -200000.0, 50000.0) : FVector(100.0, 200.0, 300.0);
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterLevelsSeaTest,
	"VoxelWorlds.Core.WaterLevels.SeaOnPlaneAndPlanet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterLevelsSeaTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterLevelsTestUtils;
	for (const EWorldMode Mode : { EWorldMode::InfinitePlane, EWorldMode::IslandBowl, EWorldMode::SphericalPlanet })
	{
		const bool bPlanet = Mode == EWorldMode::SphericalPlanet;
		const FString Name = bPlanet ? TEXT("Planet") : (Mode == EWorldMode::IslandBowl ? TEXT("Island") : TEXT("Plane"));
		UVoxelWorldConfiguration* Config = MakeConfig(Mode, true);
		const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*Config);
		const FVoxelWaterLevels Levels = FVoxelWaterLevels::FromConfig(*Config);
		const double SeaLevel = bPlanet ? 249500.0 : -1500.0;

		TestTrue(*FString::Printf(TEXT("%s: has water"), *Name), Levels.HasWater());
		TestEqual(*FString::Printf(TEXT("%s: levels are radii on a planet alone"), *Name), Levels.IsRadial(), bPlanet);
		TestTrue(*FString::Printf(TEXT("%s: the sea is the configuration's"), *Name),
			Levels.GetSea().bEnabled && Levels.GetSea().bRadial == Sea.bRadial && Levels.GetSea().PlaneZ == Sea.PlaneZ
			&& Levels.GetSea().Radius == Sea.Radius && Levels.GetSea().Center == Sea.Center);

		// One body: the sea, at its level, over the whole world.
		if (TestEqual(*FString::Printf(TEXT("%s: one body"), *Name), Levels.GetBodies().Num(), 1))
		{
			const FVoxelWaterBody& Body = Levels.GetBodies()[0];
			TestTrue(*FString::Printf(TEXT("%s: the body is the sea"), *Name), Body.Id == FVoxelWaterBody::SeaId && Body.Kind == EVoxelWaterBodyKind::Sea);
			TestEqual(*FString::Printf(TEXT("%s: the body's level"), *Name), Body.Level, SeaLevel);
			TestTrue(*FString::Printf(TEXT("%s: the body holds over the whole world"), *Name), Body.Bounds == FVoxelWaterLevels::WholeWorld());
		}
		TestTrue(*FString::Printf(TEXT("%s: the snapshot holds over the whole world"), *Name), Levels.GetBounds() == FVoxelWaterLevels::WholeWorld());

		// The sample and its cheap forms against the sea, at points around the surface and within 2 uu of it.
		FRandomStream Random(bPlanet ? 4242 : 2424);
		int32 DepthMismatches = 0, LevelMismatches = 0, UnderMismatches = 0, NormalMismatches = 0, NoSample = 0;
		for (int32 i = 0; i < 4000; ++i)
		{
			const double Spread = (i % 2 == 0) ? 2.0 : 5000.0;
			FVector P;
			if (bPlanet)
			{
				P = Sea.Center + Random.GetUnitVector() * (Sea.Radius + Random.FRandRange(-Spread, Spread));
			}
			else
			{
				P = FVector(Random.FRandRange(-1.0e6, 1.0e6), Random.FRandRange(-1.0e6, 1.0e6), Sea.PlaneZ + Random.FRandRange(-Spread, Spread));
			}
			if (i % 50 == 0 && !bPlanet)
			{
				P.Z = Sea.PlaneZ; // on the plane
			}
			FVoxelWaterSample Sample;
			if (!Levels.Sample(P, Sample) || !Sample.bWater || Sample.BodyId != FVoxelWaterBody::SeaId || Sample.Kind != EVoxelWaterBodyKind::Sea)
			{
				++NoSample;
				continue;
			}
			DepthMismatches += (Sample.Depth != Sea.GetDepth(P) || Levels.GetDepth(P) != Sea.GetDepth(P)) ? 1 : 0;
			LevelMismatches += (Sample.SurfaceLevel != SeaLevel) ? 1 : 0;
			UnderMismatches += (Levels.IsUnder(P) != Sea.IsBelow(P)) ? 1 : 0;
			const FVector Up = bPlanet ? (P - Sea.Center).GetSafeNormal() : FVector::UpVector;
			NormalMismatches += Sample.SurfaceNormal.Equals(Up, 1.0e-9) ? 0 : 1;
			NormalMismatches += Sample.Flow.IsZero() ? 0 : 1;
		}
		TestEqual(*FString::Printf(TEXT("%s: every point has the sea over it"), *Name), NoSample, 0);
		TestEqual(*FString::Printf(TEXT("%s: the sample's depth is the sea's (mismatches)"), *Name), DepthMismatches, 0);
		TestEqual(*FString::Printf(TEXT("%s: the surface level is the sea's (mismatches)"), *Name), LevelMismatches, 0);
		TestEqual(*FString::Printf(TEXT("%s: IsUnder is the sea's IsBelow (mismatches)"), *Name), UnderMismatches, 0);
		TestEqual(*FString::Printf(TEXT("%s: the normal is the local up, with no flow (mismatches)"), *Name), NormalMismatches, 0);

		// The box tests and the range over random boxes around the surface.
		int32 BoxMismatches = 0, RangeMismatches = 0;
		for (int32 i = 0; i < 2000; ++i)
		{
			const FVector Middle = bPlanet
				? Sea.Center + Random.GetUnitVector() * (Sea.Radius + Random.FRandRange(-3000.0, 3000.0))
				: FVector(Random.FRandRange(-1.0e5, 1.0e5), Random.FRandRange(-1.0e5, 1.0e5), Sea.PlaneZ + Random.FRandRange(-3000.0, 3000.0));
			const FBox Box = FBox::BuildAABB(Middle, FVector(Random.FRandRange(1.0, 1600.0), Random.FRandRange(1.0, 1600.0), Random.FRandRange(1.0, 1600.0)));
			BoxMismatches += (Levels.IsAbove(Box) != Sea.IsAbove(Box) || Levels.Crosses(Box) != Sea.Crosses(Box)) ? 1 : 0;
			const FVoxelWaterLevelRange Range = Levels.GetRange(Box);
			RangeMismatches += (Range.bAny && Range.bUniform && Range.MinLevel == SeaLevel && Range.MaxLevel == SeaLevel
				&& Range.GetLevel() == SeaLevel && Range.Bounds == Box) ? 0 : 1;
		}
		TestEqual(*FString::Printf(TEXT("%s: IsAbove and Crosses are the sea's (mismatches)"), *Name), BoxMismatches, 0);
		TestEqual(*FString::Printf(TEXT("%s: the range is uniform at the sea's level and names its box (mismatches)"), *Name), RangeMismatches, 0);

		// The scalar a reader copies is the configuration's, exactly (a float through a double and back).
		const FVoxelWaterLevelRange World = Levels.GetRange(FVoxelWaterLevels::WholeWorld());
		TestEqual(*FString::Printf(TEXT("%s: the level as a float is the configuration's"), *Name),
			static_cast<float>(World.GetLevel()), bPlanet ? Config->WaterRadius : Config->WaterLevel);

		// A snapshot converts from a sea: the same answers.
		const FVoxelWaterLevels FromSea = Sea;
		const FVector Probe = bPlanet ? Sea.Center + FVector(0.6, 0.0, 0.8) * (Sea.Radius - 600.0) : FVector(10.0, 20.0, Sea.PlaneZ - 600.0);
		TestTrue(*FString::Printf(TEXT("%s: a snapshot made from the sea answers as the configuration's"), *Name),
			FromSea.GetDepth(Probe) == Levels.GetDepth(Probe) && FMath::IsNearlyEqual(FromSea.GetDepth(Probe), 600.0, 1.0e-6)
			&& FromSea.GetBodies().Num() == 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterLevelsNoWaterTest,
	"VoxelWorlds.Core.WaterLevels.NoWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterLevelsNoWaterTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterLevelsTestUtils;
	const FVoxelWaterLevels Cases[] = {
		FVoxelWaterLevels(),
		FVoxelWaterLevels::FromConfig(*MakeConfig(EWorldMode::InfinitePlane, false)),
		FVoxelWaterLevels::FromConfig(*MakeConfig(EWorldMode::SphericalPlanet, false)),
		FVoxelWaterLevels(FVoxelSeaSurface()),
	};
	const TCHAR* Names[] = { TEXT("Default"), TEXT("Plane, water off"), TEXT("Planet, water off"), TEXT("A disabled sea") };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Cases); ++Index)
	{
		const FVoxelWaterLevels& Levels = Cases[Index];
		const FString Name = Names[Index];
		TestFalse(*FString::Printf(TEXT("%s: no water"), *Name), Levels.HasWater());
		TestEqual(*FString::Printf(TEXT("%s: no body"), *Name), Levels.GetBodies().Num(), 0);
		// Far under where the sea would be if it were on.
		const FVector Deep(100000.0, -200000.0, -1.0e6);
		FVoxelWaterSample Sample;
		TestFalse(*FString::Printf(TEXT("%s: no sample"), *Name), Levels.Sample(Deep, Sample));
		TestTrue(*FString::Printf(TEXT("%s: the sample says no water, far above any surface"), *Name),
			!Sample.bWater && Sample.Depth == FVoxelWaterSample::NoWaterDepth && Sample.Depth < 0.0);
		TestEqual(*FString::Printf(TEXT("%s: the depth is NoWaterDepth"), *Name), Levels.GetDepth(Deep), FVoxelWaterSample::NoWaterDepth);
		TestFalse(*FString::Printf(TEXT("%s: nothing is under water"), *Name), Levels.IsUnder(Deep));
		const FBox Box = FBox::BuildAABB(Deep, FVector(5000.0));
		TestTrue(*FString::Printf(TEXT("%s: every box is above the water"), *Name), Levels.IsAbove(Box));
		TestFalse(*FString::Printf(TEXT("%s: no surface crosses a box"), *Name), Levels.Crosses(Box));
		const FVoxelWaterLevelRange Range = Levels.GetRange(Box);
		TestTrue(*FString::Printf(TEXT("%s: the range is empty and names its box"), *Name),
			!Range.bAny && Range.MinLevel == 0.0 && Range.MaxLevel == 0.0 && Range.Bounds == Box);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
