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

// ---------------------------------------------------------------------------
// WM-PL4: every reader of a planet's generated surface sees the zones. On a streamed planet with a raised pad at 45 degrees
// (a static planet zone): the edit-merged surface on the pad and its ring (the generated voxels) lies within a voxel of
// GetGeneratedSurfacePoint and of FVoxelSurfaceQuery::SampleSurfacePoint with the zones (the PCG sampler's far points),
// the pad's material there is generation's, and GetPlanetSpawnPosition stands on the pad.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetConditionedSurfaceTest,
	"VoxelWorlds.Streaming.Planet.ConditionedSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetConditionedSurfaceTest::RunTest(const FString& Parameters)
{
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("PlanetConditionedSurface"));
	W.ConfigureSmallPlanet();
	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const FVector Site(C45, 0.0, C45);
	W.Config->PlanetSpawnLocation = 2; // the north pole
	W.Config->PlanetSpawnAltitude = 0.0f;
	W.StartManager();
	const FVector Center = W.Config->WorldOrigin;
	const double Radius = W.Config->WorldRadius;
	const float VoxelSize = W.Config->VoxelSize;

	// A pad 300 uu over the natural ground at the site.
	const float Natural = W.Manager->GetSurfacePointAt(Center + Site).Altitude;
	const float Target = Natural + 300.0f;
	const float Inner = 900.0f;
	const float Falloff = 700.0f;
	W.Manager->AddConditioningZone(FVoxelConditioningZone::MakeRadial(Site, Inner, Falloff, Target));
	TestEqual(TEXT("The generated altitude at the site is the pad's"), W.Manager->GetGeneratedPlanetAltitude(Site), Target, 1e-3f);

	// A second pad at the spawn direction: the spawn position stands on it.
	const float PoleTarget = W.Manager->GetSurfacePointAt(Center + FVector::UpVector).Altitude + 200.0f;
	W.Manager->AddConditioningZone(FVoxelConditioningZone::MakeRadial(FVector::UpVector, 500.0f, 300.0f, PoleTarget));
	TestEqual(TEXT("GetPlanetSpawnPosition stands on the pad under it"),
		static_cast<double>(W.Manager->GetAltitudeAt(W.Manager->GetPlanetSpawnPosition())), static_cast<double>(PoleTarget), 1.0);

	W.SetViewer(Center + Site * (Radius + Target + 300.0));
	if (TestTrue(TEXT("Streaming settles"), W.Settle()))
	{
		const FVector East = FVector::CrossProduct(FVector::UpVector, Site).GetSafeNormal();
		const FVector North = FVector::CrossProduct(Site, East);
		auto At = [&](double Distance, double Bearing)
		{
			const FVector Tangent = East * FMath::Cos(Bearing) + North * FMath::Sin(Bearing);
			return (Site * FMath::Cos(Distance / Radius) + Tangent * FMath::Sin(Distance / Radius)).GetSafeNormal();
		};
		const TArray<FVoxelConditioningZone> Zones = { FVoxelConditioningZone::MakeRadial(Site, Inner, Falloff, Target) };
		const FVoxelSeaSurface Sea = W.Manager->GetSeaSurface();
		int32 Points = 0, Found = 0, OffGenerated = 0, OffSample = 0, OnPad = 0;
		double Worst = 0.0;
		for (const double Distance : { 0.0, 300.0, 600.0, 850.0, 1100.0, 1300.0, 1500.0 })
		{
			for (int32 b = 0; b < 6; ++b)
			{
				const FVector Direction = At(Distance, b * UE_DOUBLE_PI / 3.0 + 0.2);
				const FVector Probe = Center + Direction * Radius;
				++Points;
				FVoxelSurfaceSample Hit;
				if (!W.Manager->QueryEditMergedSurfacePoint(Probe, Hit))
				{
					continue;
				}
				++Found;
				const FVector Generated = W.Manager->GetGeneratedSurfacePoint(Probe);
				const FVoxelSurfaceSample Analytic = FVoxelSurfaceQuery::SampleSurfacePoint(*W.Manager->GetWorldMode(), Probe, VoxelSize,
					W.Config->NoiseParams, FVoxelBiomeSnapshot(), Sea, &Zones);
				const double ToGenerated = FVector::Dist(Hit.Position, Generated);
				Worst = FMath::Max(Worst, ToGenerated);
				OffGenerated += ToGenerated > VoxelSize ? 1 : 0;
				OffSample += FVector::Dist(Analytic.Position, Generated) > 0.01 ? 1 : 0;
				if (Distance <= Inner)
				{
					OnPad += FMath::Abs(Hit.Altitude - Target) <= VoxelSize ? 1 : 0;
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%d of %d points found loaded; the worst %.1f uu from GetGeneratedSurfacePoint"), Found, Points, Worst));
		TestTrue(TEXT("The pad and its ring are loaded (non-vacuous)"), Found >= Points * 3 / 4);
		TestEqual(TEXT("The generated voxels lie on GetGeneratedSurfacePoint (pad and ring)"), OffGenerated, 0);
		TestEqual(TEXT("SampleSurfacePoint with the zones is GetGeneratedSurfacePoint"), OffSample, 0);
		TestTrue(TEXT("Inside the inner radius the edit-merged surface is at the pad's altitude"), OnPad >= 6 * 4 * 3 / 4);
	}
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
