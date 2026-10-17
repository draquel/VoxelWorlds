// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "PCGVoxelPlanetSurfaceGrid.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Spherical Phase 4 (WORLD_MODES_REVIEW_PLAN.md §18, WM-SY7; SP-G2): the Voxel Surface Sampler's grid on a planet. For a
 * box at 45 degrees and one straddling a cube-face edge (and so two faces), every surface point inside the box lies
 * within 0.9 of a spacing of a gathered cell's surface point, no two cells' points are closer than 0.4 of a spacing (no
 * point twice), the cells are the same on a second gather (stable seeds), and a box above the shell gathers nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGVoxelPlanetSurfaceGridTest,
	"VoxelWorlds.PCG.Planet.SurfaceGrid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCGVoxelPlanetSurfaceGridTest::RunTest(const FString& Parameters)
{
	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::SphericalPlanet;
	Config->WorldOrigin = FVector(1000.0, -2000.0, 500.0);
	Config->WorldRadius = 250000.0f;
	Config->PlanetHeightScale = 3000.0f;
	Config->PlanetMaxTerrainHeight = 4000.0f;
	Config->PlanetMaxTerrainDepth = 4000.0f;
	Config->NoiseParams.Frequency = 0.000025f;
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
	const FVector Center = Config->WorldOrigin;
	constexpr double Spacing = 100.0;

	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const TArray<TPair<const TCHAR*, FVector>> Sites = {
		{ TEXT("45 degrees"), FVector(C45, 0, C45) },
		{ TEXT("cube-face edge"), FVector(1, 1, 0).GetSafeNormal() },
	};
	for (const TPair<const TCHAR*, FVector>& Site : Sites)
	{
		const FVector Surface = Mode->GetSurfacePoint(Center + Site.Value, Config->NoiseParams).Position;
		const FBox Box(Surface - FVector(1500.0, 1500.0, 1500.0), Surface + FVector(1500.0, 1500.0, 1500.0));
		TArray<FPCGVoxelPlanetSurfaceCell> Cells, Again;
		TestTrue(*FString::Printf(TEXT("%s: the gather completes"), Site.Key), FPCGVoxelPlanetSurfaceGrid::GatherCells(Planet, Box, Spacing, 4000000, Cells));
		FPCGVoxelPlanetSurfaceGrid::GatherCells(Planet, Box, Spacing, 4000000, Again);

		TArray<FVector> Points;
		TSet<int32> Faces;
		for (const FPCGVoxelPlanetSurfaceCell& Cell : Cells)
		{
			const FVector Point = Mode->GetSurfacePoint(Center + Cell.Direction, Config->NoiseParams).Position;
			if (Box.IsInsideOrOn(Point))
			{
				Points.Add(Point);
				Faces.Add(Cell.Face);
			}
		}

		// Coverage: random surface points inside the box, each near a cell's point.
		FRandomStream Random(11);
		double WorstGap = 0.0;
		int32 Probes = 0;
		for (int32 i = 0; i < 4000 && Probes < 1500; ++i)
		{
			const FVector Target = Box.GetCenter() + FVector(Random.FRandRange(-1500.0, 1500.0), Random.FRandRange(-1500.0, 1500.0), Random.FRandRange(-1500.0, 1500.0));
			const FVector Probe = Mode->GetSurfacePoint(Target, Config->NoiseParams).Position;
			if (!Box.IsInsideOrOn(Probe) || Box.ExpandBy(-Spacing).IsInside(Probe) == false)
			{
				continue; // keep a spacing away from the box's faces, where the nearest cell may be outside
			}
			++Probes;
			double Nearest = TNumericLimits<double>::Max();
			for (const FVector& Point : Points)
			{
				Nearest = FMath::Min(Nearest, FVector::DistSquared(Point, Probe));
			}
			WorstGap = FMath::Max(WorstGap, FMath::Sqrt(Nearest));
		}

		// Duplicates: the closest pair of points.
		double Closest = TNumericLimits<double>::Max();
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			for (int32 j = i + 1; j < Points.Num(); ++j)
			{
				Closest = FMath::Min(Closest, FVector::DistSquared(Points[i], Points[j]));
			}
		}
		Closest = FMath::Sqrt(Closest);

		bool bStable = Cells.Num() == Again.Num();
		for (int32 i = 0; bStable && i < Cells.Num(); ++i)
		{
			bStable = Cells[i].Face == Again[i].Face && Cells[i].IU == Again[i].IU && Cells[i].IV == Again[i].IV;
		}

		AddInfo(FString::Printf(TEXT("%s: %d cells, %d points in the box on %d face(s); %d probes, worst gap %.1f uu; closest pair %.1f uu"),
			Site.Key, Cells.Num(), Points.Num(), Faces.Num(), Probes, WorstGap, Closest));
		TestTrue(*FString::Printf(TEXT("%s: points (non-vacuous)"), Site.Key), Points.Num() > 500 && Probes > 500);
		TestTrue(*FString::Printf(TEXT("%s: no gap wider than 0.9 of a spacing"), Site.Key), WorstGap <= 0.9 * Spacing);
		TestTrue(*FString::Printf(TEXT("%s: no point twice (closest pair over 0.4 of a spacing)"), Site.Key), Closest >= 0.4 * Spacing);
		TestTrue(*FString::Printf(TEXT("%s: the cells are stable"), Site.Key), bStable);
		if (Site.Value.Y > 0.5)
		{
			TestEqual(TEXT("cube-face edge: the box spans two faces"), Faces.Num(), 2);
		}
	}

	// A box in the sky, past the shell: no cells.
	const FVector Sky = Center + FVector(0, 0, 1) * (Config->WorldRadius + 20000.0);
	TArray<FPCGVoxelPlanetSurfaceCell> None;
	FPCGVoxelPlanetSurfaceGrid::GatherCells(Planet, FBox(Sky - FVector(1000.0), Sky + FVector(1000.0)), Spacing, 4000000, None);
	TestEqual(TEXT("A box above the shell gathers no cell"), None.Num(), 0);

	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
