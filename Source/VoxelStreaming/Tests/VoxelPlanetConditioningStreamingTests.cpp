// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelSeaSurface.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 5: conditioning on a planet's chunk manager (WORLD_MODES_REVIEW_PLAN.md §19, WM-PL1; SP-B10).
//
// - ConditioningGather: a planet chunk's zones are gathered by the cap its box subtends from the centre
//   (GetPlanetCapOfBox + GatherConditioningZonesOnSphere, what the generation request does). Against a brute force over
//   the chunk's voxels, no zone that weighs on any of them is missed; heightmap zones never come back on a planet.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetConditioningGatherTest,
	"VoxelWorlds.Streaming.Planet.ConditioningGather",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetConditioningGatherTest::RunTest(const FString& Parameters)
{
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("PlanetConditioningGather"));
	W.ConfigureSmallPlanet();
	W.StartManager();
	const FVector Center = W.Config->WorldOrigin;
	const double Radius = W.Config->WorldRadius;
	const double ChunkWorldSize = static_cast<double>(W.Config->ChunkSize) * W.Config->VoxelSize;

	// Three planet zones (45 degrees, a cube corner, near the north pole) and one heightmap zone that must not apply.
	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const TArray<FVoxelConditioningZone> Zones = {
		FVoxelConditioningZone::MakeRadial(FVector(C45, 0, C45), 1500.0f, 1000.0f, 120.0f),
		FVoxelConditioningZone::MakeRadial(FVector(1, 1, 1), 900.0f, 600.0f, -80.0f),
		FVoxelConditioningZone::MakeRadial(FVector(0.05, 0.02, 1), 3000.0f, 1200.0f, 0.0f),
	};
	for (const FVoxelConditioningZone& Zone : Zones)
	{
		W.Manager->AddConditioningZone(Zone);
	}
	AddExpectedMessagePlain(TEXT("zone in a planet world has no effect"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	W.Manager->AddConditioningZone(FVoxelConditioningZone(FVector2D(0.0, 0.0), 1.0e6f, 0.0f, 0.0f));

	int32 Chunks = 0, Missed = 0, Extra = 0, Heightmap = 0, Affected = 0;
	for (const FVoxelConditioningZone& Zone : Zones)
	{
		// The chunks around the zone's surface point (the shell is within +-1,000 of the datum).
		const FVector Site = Center + Zone.Direction * Radius;
		const FIntVector SiteChunk(
			FMath::FloorToInt32((Site.X - Center.X) / ChunkWorldSize),
			FMath::FloorToInt32((Site.Y - Center.Y) / ChunkWorldSize),
			FMath::FloorToInt32((Site.Z - Center.Z) / ChunkWorldSize));
		for (int32 DZ = -3; DZ <= 3; ++DZ)
		{
			for (int32 DY = -3; DY <= 3; ++DY)
			{
				for (int32 DX = -3; DX <= 3; ++DX)
				{
					const FIntVector Coord = SiteChunk + FIntVector(DX, DY, DZ);
					const FVector Min = Center + FVector(Coord) * ChunkWorldSize;
					FVector CapDirection;
					double CapAngle = 0.0;
					if (!W.Manager->GetPlanetCapOfBox(FBox(Min, Min + FVector(ChunkWorldSize)), CapDirection, CapAngle))
					{
						continue;
					}
					TArray<FVoxelConditioningZone> Gathered;
					W.Manager->GatherConditioningZonesOnSphere(CapDirection, CapAngle, Gathered);
					++Chunks;

					// Brute force over the chunk's voxels (every other one, and the far corner) for each planet zone.
					for (const FVoxelConditioningZone& Candidate : Zones)
					{
						bool bWeighs = false;
						for (int32 Z = 0; Z <= 32 && !bWeighs; Z += 2)
						{
							for (int32 Y = 0; Y <= 32 && !bWeighs; Y += 2)
							{
								for (int32 X = 0; X <= 32 && !bWeighs; X += 2)
								{
									const FVector P = Min + FVector(X, Y, Z) * W.Config->VoxelSize;
									bWeighs = Candidate.GetWeightAtDirection((P - Center).GetSafeNormal(), Radius) > 0.0f;
								}
							}
						}
						const bool bGathered = Gathered.ContainsByPredicate([&Candidate](const FVoxelConditioningZone& G)
						{
							return G.IsRadial() && G.Direction.Equals(Candidate.Direction, 1e-12);
						});
						Affected += bWeighs ? 1 : 0;
						Missed += (bWeighs && !bGathered) ? 1 : 0;
						Extra += (!bWeighs && bGathered) ? 1 : 0;
					}
					Heightmap += Gathered.FilterByPredicate([](const FVoxelConditioningZone& G) { return !G.IsRadial(); }).Num();
				}
			}
		}
	}

	AddInfo(FString::Printf(TEXT("%d chunk queries: %d chunk-zone pairs weighed on, %d missed, %d gathered conservatively"), Chunks, Affected, Missed, Extra));
	TestTrue(TEXT("Chunks queried (non-vacuous)"), Chunks > 500 && Affected > 50);
	TestEqual(TEXT("No zone that weighs on a chunk is missed"), Missed, 0);
	TestTrue(TEXT("The cap test is not wildly conservative"), Extra <= 2 * Affected);
	TestEqual(TEXT("Heightmap zones never come back on a planet"), Heightmap, 0);

	// A point query: the zones at a direction, and only those.
	{
		TArray<FVoxelConditioningZone> AtSite;
		W.Manager->GatherConditioningZonesOnSphere(Zones[0].Direction, 0.0, AtSite);
		TestEqual(TEXT("A point query at the 45-degree zone's centre gathers it alone"), AtSite.Num(), 1);
	}

	// The generated altitude takes the zone: the target at its centre, the natural altitude far from every zone.
	{
		const float AtCentre = W.Manager->GetGeneratedPlanetAltitude(Zones[0].Direction);
		TestEqual(TEXT("The generated altitude at the zone's centre is its target"), AtCentre, Zones[0].TargetHeight, 1e-3f);
		const FVector Far = FVector(-1, 0.2, -0.3).GetSafeNormal();
		TestEqual(TEXT("Far from every zone the generated altitude is the natural one"),
			W.Manager->GetGeneratedPlanetAltitude(Far), W.Manager->GetSurfacePointAt(Center + Far).Altitude, 1e-2f);
	}
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
