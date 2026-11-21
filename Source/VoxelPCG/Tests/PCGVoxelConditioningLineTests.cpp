// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "PCGVoxelSurfaceSampler.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelNoiseTypes.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelConditioningLine.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelWaterLevels.h"
#include "VoxelChunkManager.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * PCG points along a conditioning line (Documentation/TERRAIN_LINES_PLAN.md, L3): the surface sampler emits no point
 * inside a line's clear width, and names the band's material on the points it keeps on the band.
 *
 * The sampler's own steps on a plane and on a planet, with the lines on a chunk manager where it gathers them: the
 * gather over its bounds (complete, as its points must stand where the voxels generate), a point at each site of its
 * grid (FVoxelSurfaceQuery::SampleSurfacePoint with the gathered conditioning), and its clear-width test
 * (FPCGVoxelSurfaceSamplerElement::IsInClearWidth). The road here clears a width narrower than its band, so the points
 * between the two carry the band's material. Bounds wider than the lines' gather limit ask only for the lines at hand.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGVoxelConditioningLineTest,
	"VoxelWorlds.PCG.ConditioningLineClearWidth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCGVoxelConditioningLineTest::RunTest(const FString& Parameters)
{
	constexpr int32 ChunkSize = 32;
	constexpr float VoxelSize = 75.0f;
	constexpr uint8 BandMaterial = 30;
	const double ChunkWorldSize = ChunkSize * VoxelSize;
	const float HalfWidth = 4.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;
	const float Clear = 2.0f * VoxelSize;
	const FVoxelBiomeSnapshot NoBiomes;

	// The need the sampler's gather states.
	TestTrue(TEXT("Bounds a few chunks wide ask for every line"),
		FPCGVoxelSurfaceSamplerElement::GetConditioningNeed(FBox(FVector(-5000.0), FVector(5000.0)), ChunkWorldSize) == EVoxelConditioningGather::Complete);
	TestTrue(TEXT("Bounds at the gather limit still ask for every line"),
		FPCGVoxelSurfaceSamplerElement::GetConditioningNeed(FBox(FVector(0.0), FVector(64.0 * ChunkWorldSize, 1000.0, 1000.0)), ChunkWorldSize) == EVoxelConditioningGather::Complete);
	TestTrue(TEXT("Bounds wider than the gather limit ask for the lines at hand"),
		FPCGVoxelSurfaceSamplerElement::GetConditioningNeed(FBox(FVector(0.0), FVector(1000.0, 65.0 * ChunkWorldSize, 1000.0)), ChunkWorldSize) == EVoxelConditioningGather::Ready);

	struct FCount
	{
		int32 Sites = 0;
		int32 Dropped = 0;
		int32 KeptInClear = 0;
		int32 KeptOnBand = 0;
		int32 KeptOnBandOtherMaterial = 0;
	};
	const auto Report = [this](const TCHAR* Name, const FCount& Count)
	{
		AddInfo(FString::Printf(TEXT("%s: %d sites, %d dropped inside the clear width, %d kept inside it; %d kept on the band, %d of them not the band's material"),
			Name, Count.Sites, Count.Dropped, Count.KeptInClear, Count.KeptOnBand, Count.KeptOnBandOtherMaterial));
		TestTrue(FString::Printf(TEXT("%s: sites lie inside the clear width (non-vacuous)"), Name), Count.Dropped > 20);
		TestEqual(FString::Printf(TEXT("%s: no point is kept inside the clear width"), Name), Count.KeptInClear, 0);
		TestTrue(FString::Printf(TEXT("%s: points are kept on the band beside the clear width"), Name), Count.KeptOnBand > 20);
		TestEqual(FString::Printf(TEXT("%s: a point on the band names the band's material"), Name), Count.KeptOnBandOtherMaterial, 0);
	};

	// ---- A plane.
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::InfinitePlane;
		Config->ChunkSize = ChunkSize;
		Config->VoxelSize = VoxelSize;
		Config->HeightScale = 900.0f;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 7411;
		Config->NoiseParams.Frequency = 0.0002f;
		Config->NoiseParams.Octaves = 3;
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>();
		Manager->AddToRoot();
		const FVector2D Site(2600.0, 3100.0);
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		Params.Id = 1;
		Params.Surface.BandMaterialID = BandMaterial;
		Params.Surface.ClearHalfWidth = Clear;
		const float Target = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector(Site.X, Site.Y, 0.0), Config->NoiseParams).Position.Z) + 100.0f;
		for (int32 Step = -30; Step <= 30; ++Step)
		{
			const FVector2D P = Site + FVector2D(0.82, 0.57) * (Step * 2.0 * VoxelSize);
			Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(P.X, P.Y, 0.0), Target, HalfWidth));
		}
		Manager->AddConditioningLine(Params);

		// The sampler's bounds and its grid of sites, a point every voxel.
		const FBox Bounds(FVector(Site.X - 3000.0, Site.Y - 3000.0, -5000.0), FVector(Site.X + 3000.0, Site.Y + 3000.0, 5000.0));
		FVoxelConditioning Conditioning;
		Manager->GatherConditioning(Bounds, FPCGVoxelSurfaceSamplerElement::GetConditioningNeed(Bounds, ChunkWorldSize), Conditioning);
		TestEqual(TEXT("Plane: the sampler's gather finds the line"), Conditioning.Lines.Num(), 1);
		FCount Count;
		FVoxelConditioningLineSample At;
		for (int32 IX = FMath::CeilToInt32(Bounds.Min.X / VoxelSize); IX <= FMath::FloorToInt32(Bounds.Max.X / VoxelSize); ++IX)
		{
			for (int32 IY = FMath::CeilToInt32(Bounds.Min.Y / VoxelSize); IY <= FMath::FloorToInt32(Bounds.Max.Y / VoxelSize); ++IY)
			{
				const FVector Query(IX * static_cast<double>(VoxelSize), IY * static_cast<double>(VoxelSize), 0.0);
				++Count.Sites;
				const FVoxelSurfaceSample Sample = FVoxelSurfaceQuery::SampleSurfacePoint(*Mode, Query, VoxelSize, Config->NoiseParams, NoBiomes, FVoxelWaterLevels(), &Conditioning);
				if (FPCGVoxelSurfaceSamplerElement::IsInClearWidth(Conditioning, *Mode, Sample.Position))
				{
					++Count.Dropped;
					continue;
				}
				Count.KeptInClear += Conditioning.IsClearedAt(Query.X, Query.Y) ? 1 : 0;
				if (Conditioning.Lines[0]->Sample(Query.X, Query.Y, 0.0, At) && At.bOnBand && At.Distance < At.HalfWidth - 1.0f)
				{
					++Count.KeptOnBand;
					Count.KeptOnBandOtherMaterial += Sample.MaterialID != BandMaterial ? 1 : 0;
				}
			}
		}
		Report(TEXT("Plane"), Count);
		Manager->RemoveFromRoot();
		Config->RemoveFromRoot();
	}

	// ---- A planet: the clear width along the surface.
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->ChunkSize = ChunkSize;
		Config->VoxelSize = VoxelSize;
		Config->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
		Config->WorldRadius = 100000.0f;
		Config->PlanetHeightScale = 1200.0f;
		Config->PlanetMaxTerrainHeight = 1500.0f;
		Config->PlanetMaxTerrainDepth = 1500.0f;
		Config->BaseHeight = 0.0f;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 7411;
		Config->NoiseParams.Frequency = 0.0002f;
		Config->NoiseParams.Octaves = 3;
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FSphericalPlanetWorldMode& PlanetMode = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(*Config);
		const double R = Planet.PlanetRadius;
		const FVector Site = FVector(1.0, 0.95, 0.33).GetSafeNormal();
		const FVector Tangent = (FVector(0.2, -0.4, 1.0) - Site * FVector::DotProduct(FVector(0.2, -0.4, 1.0), Site)).GetSafeNormal();
		const FVector Side = FVector::CrossProduct(Site, Tangent).GetSafeNormal();
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		Params.Id = 1;
		Params.bRadial = true;
		Params.Surface.BandMaterialID = BandMaterial;
		Params.Surface.ClearHalfWidth = Clear;
		const float Target = PlanetMode.GetTerrainAltitude(Site, Config->NoiseParams) + 100.0f;
		for (int32 Step = -30; Step <= 30; ++Step)
		{
			const double Angle = Step * 2.0 * VoxelSize / R;
			Params.Vertices.Add(FVoxelConditioningLineVertex((Site * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal(), Target, HalfWidth));
		}
		FVoxelConditioning Conditioning;
		Conditioning.Lines.Add(FVoxelConditioningLine::Make(Params));
		// A grid of directions around the site, a voxel apart along and across the line.
		FCount Count;
		FVoxelConditioningLineSample At;
		for (int32 Along = -24; Along <= 24; ++Along)
		{
			for (int32 Across = -12; Across <= 12; ++Across)
			{
				const FVector Direction = (Site + (Tangent * Along + Side * Across) * (VoxelSize / R)).GetSafeNormal();
				++Count.Sites;
				const FVoxelSurfaceSample Sample = FVoxelSurfaceQuery::SampleSurfacePoint(*Mode, Planet.PlanetCenter + Direction * R, VoxelSize, Config->NoiseParams, NoBiomes,
					FVoxelWaterLevels(), &Conditioning);
				if (FPCGVoxelSurfaceSamplerElement::IsInClearWidth(Conditioning, *Mode, Sample.Position))
				{
					++Count.Dropped;
					continue;
				}
				Count.KeptInClear += Conditioning.IsClearedOnSphere(Direction, R) ? 1 : 0;
				if (Conditioning.Lines[0]->SampleOnSphere(Direction, R, 0.0, At) && At.bOnBand && At.Distance < At.HalfWidth - 1.0f)
				{
					++Count.KeptOnBand;
					Count.KeptOnBandOtherMaterial += Sample.MaterialID != BandMaterial ? 1 : 0;
				}
			}
		}
		Report(TEXT("Planet"), Count);
		Config->RemoveFromRoot();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
