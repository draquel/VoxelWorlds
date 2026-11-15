// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelSurfaceNavigationSubsystem.h"
#include "VoxelSurfacePathfinder.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Navigation at a lake (the hydrology plan's H1, section 5.5): a cell's water test asks the level over the cell itself.
//
// - Navigation.WaterBodies: a plane world and a planet with the sea off. Points around a site are asked before any
//   body exists; then a lake is added over the site, deeper than a walker wades at every column of its footprint, and
//   a second, shallow one elsewhere (30 uu over its ground: a wade). With the lakes, no point inside the deep lake's
//   footprint is walkable, every point outside it answers as before, and the point in the shallow lake stays as it
//   was: water deeper than the wade depth blocks a walker, as the sea does, cell by cell.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNavWaterBodiesTest,
	"VoxelWorlds.Navigation.WaterBodies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNavWaterBodiesTest::RunTest(const FString& Parameters)
{
	for (const bool bPlanet : { false, true })
	{
		const FString Name = bPlanet ? TEXT("Planet") : TEXT("Plane");
		VoxelTesting::FTestChunkWorld W;
		W.Create(bPlanet ? TEXT("VoxelNavWaterBodiesPlanet") : TEXT("VoxelNavWaterBodiesPlane"));
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
		}
		else
		{
			W.ConfigureSmallPlane();
		}
		W.Config->bEnableWaterLevel = false;
		W.StartManager();
		UVoxelChunkManager* Manager = W.Manager;
		UVoxelSurfaceNavigationSubsystem* Nav = W.World->GetSubsystem<UVoxelSurfaceNavigationSubsystem>();
		if (!TestNotNull(TEXT("The game world has navigation"), Nav))
		{
			W.Stop();
			continue;
		}
		Nav->SetChunkManager(Manager);
		const FVoxelWaterFrame Frame = Manager->GetWaterLevels().GetFrame();

		// The site and the surface around it.
		const FVector SiteDirection = FVector(0.4, -0.5, 0.75).GetSafeNormal();
		const FVector Site = bPlanet ? Manager->GetGeneratedSurfacePoint(Frame.Center + SiteDirection * Frame.DatumRadius)
			: Manager->GetGeneratedSurfacePoint(FVector(-1900.0, 2100.0, 0.0));
		const FVector Up = Manager->GetUpVectorAt(Site);
		const FVector East = bPlanet ? FVector::CrossProduct(FVector::UpVector, Up).GetSafeNormal() : FVector::ForwardVector;
		const FVector North = FVector::CrossProduct(Up, East);
		// The generated ground Along and Across the surface from the site.
		const auto GroundAt = [&](double Along, double Across)
		{
			return bPlanet ? Manager->GetGeneratedSurfacePoint(Frame.Center + (Up * Frame.DatumRadius + East * Along + North * Across).GetSafeNormal() * Frame.DatumRadius)
				: Manager->GetGeneratedSurfacePoint(Site + East * Along + North * Across);
		};

		// The points: a grid inside the deep lake's footprint (700 uu each way of 800), a ring well outside it, and the
		// shallow lake's point.
		TArray<FVector> Inside, Outside;
		double HighestInside = -TNumericLimits<double>::Max();
		for (int32 I = -7; I <= 7; ++I)
		{
			for (int32 J = -7; J <= 7; ++J)
			{
				const FVector P = GroundAt(100.0 * I, 100.0 * J);
				HighestInside = FMath::Max(HighestInside, Frame.HeightOf(P));
				if (I % 2 == 0 && J % 2 == 0)
				{
					Inside.Add(P);
				}
			}
		}
		for (int32 K = 0; K < 48; ++K)
		{
			const double Angle = UE_DOUBLE_TWO_PI * K / 48.0;
			Outside.Add(GroundAt(2000.0 * FMath::Cos(Angle), 2000.0 * FMath::Sin(Angle)));
		}
		const FVector Shallow = GroundAt(-3000.0, 600.0);

		// Before any body.
		TArray<bool> InsideBefore, OutsideBefore;
		int32 WalkableInsideBefore = 0, WalkableOutsideBefore = 0;
		for (const FVector& P : Inside)
		{
			InsideBefore.Add(Nav->IsWalkableAt(P));
			WalkableInsideBefore += InsideBefore.Last() ? 1 : 0;
		}
		for (const FVector& P : Outside)
		{
			OutsideBefore.Add(Nav->IsWalkableAt(P));
			WalkableOutsideBefore += OutsideBefore.Last() ? 1 : 0;
		}
		const bool bShallowBefore = Nav->IsWalkableAt(Shallow);
		TestTrue(*FString::Printf(TEXT("%s: the ground at the site is walkable without water (non-vacuous)"), *Name), WalkableInsideBefore > Inside.Num() / 2);
		TestTrue(*FString::Printf(TEXT("%s: the ground around it too"), *Name), WalkableOutsideBefore > Outside.Num() / 2);
		TestTrue(*FString::Printf(TEXT("%s: with the sea off and no body the parameters have no water"), *Name),
			!Nav->GetParams().bHasWater && !Nav->GetParams().WaterSampler);

		// The lakes: a deep one over the site (16 x 16 cells of 100 uu, its level 200 over the highest ground under it),
		// a shallow one at the other point (30 over its ground: less than a wade).
		const TSharedRef<const FVoxelWaterLake> DeepCells = bPlanet
			? FVoxelWaterLake::MakeOnSphere(Up, Frame.DatumRadius, 100.0, FIntPoint(16, 16), TBitArray<>(true, 256))
			: FVoxelWaterLake::Make(FVector2D(Site.X - 800.0, Site.Y - 800.0), 100.0, FIntPoint(16, 16), TBitArray<>(true, 256));
		const TSharedRef<const FVoxelWaterLake> ShallowCells = bPlanet
			? FVoxelWaterLake::MakeOnSphere((Shallow - Frame.Center).GetSafeNormal(), Frame.DatumRadius, 100.0, FIntPoint(2, 2), TBitArray<>(true, 4))
			: FVoxelWaterLake::Make(FVector2D(Shallow.X - 100.0, Shallow.Y - 100.0), 100.0, FIntPoint(2, 2), TBitArray<>(true, 4));
		Manager->AddWaterBody(FVoxelWaterBody::MakeLake(1, HighestInside + 200.0, DeepCells, Frame, 1000.0));
		Manager->AddWaterBody(FVoxelWaterBody::MakeLake(2, Frame.HeightOf(Shallow) + 30.0, ShallowCells, Frame, 100.0));

		int32 WalkableInside = 0, OutsideChanged = 0;
		for (const FVector& P : Inside)
		{
			WalkableInside += Nav->IsWalkableAt(P) ? 1 : 0;
		}
		for (int32 Index = 0; Index < Outside.Num(); ++Index)
		{
			OutsideChanged += Nav->IsWalkableAt(Outside[Index]) == OutsideBefore[Index] ? 0 : 1;
		}
		const FVoxelSurfaceNavParams Params = Nav->GetParams();
		AddInfo(FString::Printf(TEXT("%s: %d of %d points in the lake were walkable before it, %d of %d around it; wade depth %.0f"),
			*Name, WalkableInsideBefore, Inside.Num(), WalkableOutsideBefore, Outside.Num(), Params.MaxWadeDepth));
		TestTrue(*FString::Printf(TEXT("%s: with a lake in the world a cell asks the world's sample"), *Name), Params.bHasWater && static_cast<bool>(Params.WaterSampler));
		TestEqual(*FString::Printf(TEXT("%s: no point under the deep lake is walkable"), *Name), WalkableInside, 0);
		TestEqual(*FString::Printf(TEXT("%s: the points outside its footprint answer as before (changed)"), *Name), OutsideChanged, 0);
		TestEqual(*FString::Printf(TEXT("%s: a lake a walker wades through leaves its ground as it was"), *Name), Nav->IsWalkableAt(Shallow), bShallowBefore);
		if (!bPlanet)
		{
			TestTrue(TEXT("Plane: the parameters' own test reads the lake's level over the cell"),
				Params.IsUnderwater(Site.X, Site.Y, static_cast<float>(Site.Z)) && !Params.IsUnderwater(Site.X + 2000.0, Site.Y, static_cast<float>(GroundAt(2000.0, 0.0).Z)));
		}
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
