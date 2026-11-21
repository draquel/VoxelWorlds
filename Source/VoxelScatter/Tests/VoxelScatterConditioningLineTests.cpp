// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterExtraction.h"
#include "VoxelScatterTypes.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "IVoxelWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelConditioningLine.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Scatter along a conditioning line (Documentation/TERRAIN_LINES_PLAN.md, L3): a line's clear width holds no surface
// point on the ground, so no scatter is placed in it.
//
// The chunks around a site are generated with a levelled road through it (the CPU generator) and extracted twice: with
// the road in the extraction context, and with its twin that has no clear width. The twin's points inside the clear
// width show what would grow on the road; with the clear width none is left there, and every point outside it is the
// twin's (the same points, nothing else moved). On the plane by the column scan and by the block-face scan, and on a
// planet by the radial rays, where the clear width is measured along the surface.
// ---------------------------------------------------------------------------

namespace VoxelScatterLineTestUtils
{
	constexpr float VoxelSize = 75.0f;
	constexpr int32 ChunkSize = 32;

	UVoxelWorldConfiguration* MakeConfig(bool bPlanet)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = bPlanet ? EWorldMode::SphericalPlanet : EWorldMode::InfinitePlane;
		Config->VoxelSize = VoxelSize;
		Config->ChunkSize = ChunkSize;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 7411;
		Config->NoiseParams.Octaves = 3;
		Config->NoiseParams.Frequency = 0.0002f;
		Config->NoiseParams.Lacunarity = 2.0f;
		Config->NoiseParams.Persistence = 0.5f;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->BaseHeight = 0.0f;
		if (bPlanet)
		{
			Config->WorldOrigin = FVector(12345.0, 54321.0, 0.0);
			Config->WorldRadius = 100000.0f;
			Config->PlanetHeightScale = 1200.0f;
			Config->PlanetMaxTerrainHeight = 1500.0f;
			Config->PlanetMaxTerrainDepth = 1500.0f;
		}
		else
		{
			Config->WorldOrigin = FVector(-37.5, 112.5, 0.0);
			Config->HeightScale = 900.0f;
		}
		return Config;
	}

	/** The surface points of the 3 x 3 x 3 chunks around a site: generated with Generation, extracted with Context. */
	TArray<FVoxelSurfacePoint> ExtractAround(const UVoxelWorldConfiguration* Config, const IVoxelWorldMode& Mode, const FVector& Site, bool bCubic,
		const FVoxelConditioning& Generation, const FVoxelConditioning& ContextConditioning)
	{
		FScatterExtractionContext Context;
		Context.WorldMode = &Mode;
		Context.NoiseParams = Config->NoiseParams;
		Context.Conditioning = MakeShared<const FVoxelConditioning>(ContextConditioning);
		const double ChunkWorld = ChunkSize * VoxelSize;
		const FVector Local = (Site - Config->WorldOrigin) / ChunkWorld;
		const FIntVector Center(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), FMath::FloorToInt(Local.Z));
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		TArray<FVoxelSurfacePoint> Out;
		for (int32 DZ = -1; DZ <= 1; ++DZ)
		for (int32 DY = -1; DY <= 1; ++DY)
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			const FIntVector Coord = Center + FIntVector(DX, DY, DZ);
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = Coord;
			Request.ChunkSize = ChunkSize;
			Request.VoxelSize = VoxelSize;
			Request.NoiseParams = Config->NoiseParams;
			FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
			Request.bEnableBiomes = false;
			Request.bEnableCaves = false;
			Request.bEnableWaterLevel = false;
			Request.Conditioning = Generation;
			TArray<FVoxelData> Voxels;
			Generator.GenerateChunkCPU(Request, Voxels);
			FChunkSurfaceData Surface;
			UVoxelScatterManager::ExtractChunkSurfacePoints(bCubic, Voxels, Coord, Config->WorldOrigin + FVector(Coord) * ChunkWorld, ChunkSize, VoxelSize, VoxelSize, {},
				Context, Surface);
			Out.Append(Surface.SurfacePoints);
		}
		Generator.Shutdown();
		return Out;
	}

	struct FClearCheck
	{
		/** The twin's points (no clear width): all, and those on the ground inside the clear width */
		int32 TwinPoints = 0;
		int32 TwinInClear = 0;
		/** With the clear width: all, those on the ground inside it, and the twin's points outside it that are missing */
		int32 Points = 0;
		int32 InClear = 0;
		int32 OutsideMissing = 0;
	};

	/** Compare the points extracted with a clear width against its twin's. */
	FClearCheck Compare(const TArray<FVoxelSurfacePoint>& Twin, const TArray<FVoxelSurfacePoint>& Cleared, TFunctionRef<bool(const FVector&)> IsCleared)
	{
		FClearCheck Check;
		Check.TwinPoints = Twin.Num();
		Check.Points = Cleared.Num();
		TSet<FIntVector> Kept;
		for (const FVoxelSurfacePoint& Point : Cleared)
		{
			Kept.Add(FIntVector(FMath::RoundToInt32(Point.Position.X * 4.0), FMath::RoundToInt32(Point.Position.Y * 4.0), FMath::RoundToInt32(Point.Position.Z * 4.0)));
			Check.InClear += (!Point.bIsUnderground && IsCleared(Point.Position)) ? 1 : 0;
		}
		for (const FVoxelSurfacePoint& Point : Twin)
		{
			if (!Point.bIsUnderground && IsCleared(Point.Position))
			{
				++Check.TwinInClear;
				continue;
			}
			Check.OutsideMissing += Kept.Contains(FIntVector(FMath::RoundToInt32(Point.Position.X * 4.0), FMath::RoundToInt32(Point.Position.Y * 4.0),
				FMath::RoundToInt32(Point.Position.Z * 4.0))) ? 0 : 1;
		}
		return Check;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterConditioningLineClearTest,
	"VoxelWorlds.Scatter.ConditioningLine.ClearWidth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterConditioningLineClearTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterLineTestUtils;
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;
	const float Clear = 5.0f * VoxelSize;
	const auto Report = [this](const TCHAR* Name, const FClearCheck& Check)
	{
		AddInfo(FString::Printf(TEXT("%s: %d surface points without the clear width, %d of them on the ground inside it; %d with it, %d inside it; %d of the points outside it missing"),
			Name, Check.TwinPoints, Check.TwinInClear, Check.Points, Check.InClear, Check.OutsideMissing));
		TestTrue(FString::Printf(TEXT("%s: without the clear width points lie on the road (non-vacuous)"), Name), Check.TwinInClear > 300);
		TestEqual(FString::Printf(TEXT("%s: no surface point on the ground inside the clear width"), Name), Check.InClear, 0);
		TestEqual(FString::Printf(TEXT("%s: the points removed are the ones inside the clear width"), Name), Check.Points, Check.TwinPoints - Check.TwinInClear);
		TestEqual(FString::Printf(TEXT("%s: every point outside the clear width is kept"), Name), Check.OutsideMissing, 0);
	};

	// The plane: a levelled road through a site, 150 uu over the ground there.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(false);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FVector2D Site(2600.0, 3100.0);
		const float Target = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector(Site.X, Site.Y, 0.0), Config->NoiseParams).Position.Z) + 150.0f;
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		Params.Id = 1;
		for (int32 Step = -40; Step <= 40; ++Step)
		{
			const FVector2D P = Site + FVector2D(0.82, 0.57) * (Step * 2.0 * VoxelSize);
			Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(P.X, P.Y, 0.0), Target, HalfWidth));
		}
		FVoxelConditioning Twin;
		Twin.Lines.Add(FVoxelConditioningLine::Make(Params));
		Params.Surface.ClearHalfWidth = Clear;
		FVoxelConditioning Road;
		Road.Lines.Add(FVoxelConditioningLine::Make(Params));
		const FVector SitePoint(Site.X, Site.Y, Target);
		const auto IsCleared = [&Road](const FVector& P) { return Road.IsClearedAt(P.X, P.Y); };
		for (const bool bCubic : { false, true })
		{
			Report(bCubic ? TEXT("Plane, the block-face scan") : TEXT("Plane, the column scan"),
				Compare(ExtractAround(Config, *Mode, SitePoint, bCubic, Road, Twin), ExtractAround(Config, *Mode, SitePoint, bCubic, Road, Road), IsCleared));
		}
		Config->RemoveFromRoot();
	}

	// A planet: an arc through a site, the clear width along the surface.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(true);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FSphericalPlanetWorldMode& PlanetMode = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(*Config);
		const double R = Planet.PlanetRadius;
		const FVector Site = FVector(1.0, 0.95, 0.33).GetSafeNormal();
		const FVector Tangent = (FVector(0.2, -0.4, 1.0) - Site * FVector::DotProduct(FVector(0.2, -0.4, 1.0), Site)).GetSafeNormal();
		const float Target = PlanetMode.GetTerrainAltitude(Site, Config->NoiseParams) + 150.0f;
		FVoxelConditioningLineParams Params;
		Params.FalloffWidth = Falloff;
		Params.Id = 1;
		Params.bRadial = true;
		for (int32 Step = -40; Step <= 40; ++Step)
		{
			const double Angle = Step * 2.0 * VoxelSize / R;
			Params.Vertices.Add(FVoxelConditioningLineVertex((Site * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal(), Target, HalfWidth));
		}
		FVoxelConditioning Twin;
		Twin.Lines.Add(FVoxelConditioningLine::Make(Params));
		Params.Surface.ClearHalfWidth = Clear;
		FVoxelConditioning Road;
		Road.Lines.Add(FVoxelConditioningLine::Make(Params));
		const FVector SitePoint = Planet.PlanetCenter + Site * (R + Target);
		const auto IsCleared = [&Road, &Planet, R](const FVector& P) { return Road.IsClearedOnSphere((P - Planet.PlanetCenter).GetSafeNormal(), R); };
		Report(TEXT("Planet, the radial rays"),
			Compare(ExtractAround(Config, *Mode, SitePoint, false, Road, Twin), ExtractAround(Config, *Mode, SitePoint, false, Road, Road), IsCleared));
		Config->RemoveFromRoot();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
