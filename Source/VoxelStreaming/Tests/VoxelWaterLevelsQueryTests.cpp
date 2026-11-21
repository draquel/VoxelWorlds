// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelSeaSurface.h"
#include "VoxelWaterLevels.h"
#include "VoxelWaterPropagation.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The water plan's W2: the chunk manager hands out the world's water levels (GetWaterLevels), and the entries that
// stayed for the game plugins and Blueprints (GetSeaSurface, GetSeaDepthAt) are wrappers over them.
//
// - Streaming.Water.LevelsQuery: a plane world (WorldOrigin.Z at -3,000, the sea at 0) and a small planet. The chunk
//   manager's levels are the configuration's sea: one body, the sample's depth and level those of
//   FVoxelSeaSurface::FromConfig at points above and below; GetSeaSurface is that sea and GetSeaDepthAt the sample's
//   depth as a float. The snapshot is built from the configuration as it is when asked, as GetSeaSurface was: a level
//   changed after the world started is the level the next query reads. With water off there is no body and
//   GetSeaDepthAt is the large negative value it always was.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterLevelsQueryTest,
	"VoxelWorlds.Streaming.Water.LevelsQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterLevelsQueryTest::RunTest(const FString& Parameters)
{
	for (const bool bPlanet : { false, true })
	{
		const FString Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		VoxelTesting::FTestChunkWorld W;
		W.Create(bPlanet ? TEXT("VoxelWaterLevelsPlanetWorld") : TEXT("VoxelWaterLevelsPlaneWorld"));
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
			W.Config->WaterRadius = W.Config->WorldRadius - 300.0f;
		}
		else
		{
			W.ConfigureSmallPlane();
			W.Config->WorldOrigin = FVector(12345.0, 54321.0, -3000.0);
			W.Config->WaterLevel = 0.0f;
		}
		W.Config->bEnableWaterLevel = true;
		W.StartManager();

		const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*W.Config);
		const FVoxelWaterLevels Levels = W.Manager->GetWaterLevels();
		const FVoxelSeaSurface ManagerSea = W.Manager->GetSeaSurface();
		TestTrue(*FString::Printf(TEXT("%s: the levels have one body, the sea"), *Name),
			Levels.HasWater() && Levels.GetBodies().Num() == 1 && Levels.GetBodies()[0].Kind == EVoxelWaterBodyKind::Sea
			&& Levels.GetBodies()[0].Level == (bPlanet ? Sea.Radius : Sea.PlaneZ));
		TestTrue(*FString::Printf(TEXT("%s: GetSeaSurface is the configuration's sea"), *Name),
			ManagerSea.bEnabled && ManagerSea.bRadial == bPlanet && ManagerSea.PlaneZ == Sea.PlaneZ && ManagerSea.Radius == Sea.Radius
			&& ManagerSea.Center == Sea.Center);
		TestTrue(*FString::Printf(TEXT("%s: the edit-driven flood is on with water"), *Name), W.Manager->GetWaterPropagation() != nullptr);

		FRandomStream Random(77);
		int32 Mismatches = 0;
		for (int32 i = 0; i < 500; ++i)
		{
			const FVector P = bPlanet
				? Sea.Center + Random.GetUnitVector() * (Sea.Radius + Random.FRandRange(-900.0, 900.0))
				: FVector(Random.FRandRange(-50000.0, 50000.0), Random.FRandRange(-50000.0, 50000.0), Random.FRandRange(-900.0, 900.0));
			FVoxelWaterSample Sample;
			const bool bSampled = Levels.Sample(P, Sample);
			Mismatches += (bSampled && Sample.Depth == Sea.GetDepth(P) && Sample.SurfaceLevel == (bPlanet ? Sea.Radius : Sea.PlaneZ)
				&& W.Manager->GetSeaDepthAt(P) == static_cast<float>(Sea.GetDepth(P))) ? 0 : 1;
		}
		TestEqual(*FString::Printf(TEXT("%s: the sample and GetSeaDepthAt are the sea's depth (mismatches)"), *Name), Mismatches, 0);

		// Built when asked: a level changed after the world started is what the next query reads.
		const FVector Probe = bPlanet ? Sea.Center + FVector(0.0, 0.6, 0.8) * static_cast<double>(W.Config->WorldRadius) : FVector(0.0, 0.0, 100.0);
		if (bPlanet)
		{
			W.Config->WaterRadius = W.Config->WorldRadius + 500.0f;
		}
		else
		{
			W.Config->WaterLevel = 600.0f;
		}
		TestTrue(*FString::Printf(TEXT("%s: the levels follow the configuration (500 under the new sea)"), *Name),
			FMath::IsNearlyEqual(W.Manager->GetWaterLevels().GetDepth(Probe), 500.0, 0.01)
			&& FMath::IsNearlyEqual(W.Manager->GetSeaDepthAt(Probe), 500.0f, 0.01f));

		// Water off: no body, and the depth the wrapper always gave.
		W.Config->bEnableWaterLevel = false;
		TestTrue(*FString::Printf(TEXT("%s: no body with water off"), *Name),
			!W.Manager->GetWaterLevels().HasWater() && W.Manager->GetWaterLevels().GetBodies().Num() == 0 && !W.Manager->GetSeaSurface().bEnabled);
		TestEqual(*FString::Printf(TEXT("%s: GetSeaDepthAt with water off"), *Name), W.Manager->GetSeaDepthAt(Probe), -1.0e9f);
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
