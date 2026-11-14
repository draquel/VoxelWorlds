// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelConditioningLineTestUtils.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelCaveTypes.h"
#include "VoxelTreeInjector.h"
#include "VoxelTreeTypes.h"
#include "VoxelWaterLevels.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The surface under a conditioning line (Documentation/TERRAIN_LINES_PLAN.md, L3): a band of material, a minimum depth
// for caves and a width kept clear.
//
// - Surface: the band's reach across the line (the lesser of its half width and the core's), the least half width a
//   coarse reader takes a line to have, the later line's band where two overlap, the cave depth's profile, the clear
//   width and the reach that holds it, the planet's twins, the warnings, and a chunk's segment list giving what the set
//   gives at every column.
// - Band: on the plane (the biome pipeline), the island and a planet, a generated chunk carries the band's material on
//   exactly the voxels the analytic band names, to the band's depth and in the air over it; a line that only paints
//   changes nothing else; a levelled road is painted on the ground it levels; the later of two crossing lines wins.
// - CaveUnderLine: with a cave layer that opens at the surface, no voxel is carved above a line's minimum cave depth
//   (less the layer's fade) on its core, beyond the reach the chunk's densities, materials and biomes are the ones
//   without the line, and the single-voxel reader (FVoxelSurfaceQuery::GenerateVoxelAt) is solid where the chunk is.
// - Trees: no voxel tree is planted inside a line's clear width, the trees beyond the line's reach are the ones planted
//   without it, and a tree beside a line that moves the ground stands on the ground the line leaves.
// ---------------------------------------------------------------------------

namespace VoxelLineSurfaceTestUtils
{
	using namespace VoxelConditioningLineTestUtils;

	/** A material id no biome, depth rule or height rule gives: a band's, so a voxel with it was painted. */
	constexpr uint8 BandMaterial = 30;
	constexpr uint8 OtherBandMaterial = 31;

	struct FBandCheck
	{
		int32 Voxels = 0;
		/** Voxels the analytic band names (on the band, above its depth) */
		int32 Expected = 0;
		/** Of those, the ones that do not carry the band's material */
		int32 Missing = 0;
		/** Voxels with a band material where the analytic band names none */
		int32 Stray = 0;
		/** Voxels off the band that differ from the chunk without the line (counted for a line that only paints) */
		int32 OffBandDiffering = 0;
		/** Voxels whose density or biome differs from the chunk without the line (a line that only paints: none) */
		int32 ShapeDiffering = 0;
		/** Voxels left out: within the float's reach of the band's edge or of its depth */
		int32 Skipped = 0;
	};

	/**
	 * Generate a chunk with and without a conditioning and compare each voxel's material with the analytic band
	 * (FVoxelConditioning::GetBandAt or GetBandOnSphere at the voxel's column, the depth under the conditioned height).
	 */
	FBandCheck CheckBand(const IVoxelWorldMode& Mode, const FVoxelNoiseGenerationRequest& NaturalRequest, const FVoxelConditioning& Conditioning,
		const FVector& PlanetCentre, double PlanetRadius)
	{
		FBandCheck Check;
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		TArray<FVoxelData> Natural;
		Generator.GenerateChunkCPU(NaturalRequest, Natural);
		FVoxelNoiseGenerationRequest Request = NaturalRequest;
		Request.Conditioning = Conditioning;
		TArray<FVoxelData> Painted;
		Generator.GenerateChunkCPU(Request, Painted);
		Generator.Shutdown();
		if (Natural.Num() != Painted.Num() || Natural.Num() == 0)
		{
			Check.Missing = -1;
			return Check;
		}
		const bool bPlanet = PlanetRadius > 0.0;
		const int32 N = Request.ChunkSize;
		const float Voxel = Request.VoxelSize;
		const FVector ChunkOrigin = Request.GetChunkWorldPosition();
		for (int32 Z = 0; Z < N; ++Z)
		{
			for (int32 Y = 0; Y < N; ++Y)
			{
				for (int32 X = 0; X < N; ++X)
				{
					const int32 Index = X + Y * N + Z * N * N;
					const FVector WorldPos = ChunkOrigin + FVector(X, Y, Z) * Voxel;
					++Check.Voxels;
					uint8 Band = 0;
					float BandDepth = 0.0f;
					bool bBand = false;
					bool bNearEdge = false;
					float Depth = 0.0f;
					FVoxelConditioningLineSample At;
					if (bPlanet)
					{
						const FVector Relative = WorldPos - PlanetCentre;
						const FVector Direction = Relative.GetSafeNormal();
						const float NaturalAltitude = static_cast<const FSphericalPlanetWorldMode&>(Mode).GetTerrainAltitude(Direction, Request.NoiseParams);
						const float Altitude = Conditioning.ApplyToAltitude(Direction, PlanetRadius, NaturalAltitude);
						Depth = static_cast<float>((static_cast<double>(Altitude) - (Relative.Size() - PlanetRadius)) / Voxel);
						bBand = Conditioning.GetBandOnSphere(Direction, PlanetRadius, Band, &BandDepth);
						for (const FVoxelConditioningLineRef& Line : Conditioning.Lines)
						{
							bNearEdge |= Line->SampleOnSphere(Direction, PlanetRadius, 0.0, At)
								&& FMath::Abs(At.Distance - FMath::Min(Line->GetBandHalfWidthLimit(), At.HalfWidth)) < 0.5f;
						}
					}
					else
					{
						const float NaturalHeight = static_cast<float>(Mode.GetSurfaceOnUpLine(FVector(WorldPos.X, WorldPos.Y, 0.0), Request.NoiseParams).Position.Z);
						const float Height = Conditioning.ApplyToHeight(WorldPos.X, WorldPos.Y, NaturalHeight);
						Depth = (Height - static_cast<float>(WorldPos.Z)) / Voxel;
						bBand = Conditioning.GetBandAt(WorldPos.X, WorldPos.Y, Band, &BandDepth);
						for (const FVoxelConditioningLineRef& Line : Conditioning.Lines)
						{
							bNearEdge |= Line->Sample(WorldPos.X, WorldPos.Y, 0.0, At)
								&& FMath::Abs(At.Distance - FMath::Min(Line->GetBandHalfWidthLimit(), At.HalfWidth)) < 0.05f;
						}
					}
					const bool bShape = Painted[Index].Density != Natural[Index].Density || Painted[Index].BiomeID != Natural[Index].BiomeID;
					Check.ShapeDiffering += bShape ? 1 : 0;
					if (bNearEdge || (bBand && FMath::Abs(Depth - BandDepth) < 0.02f))
					{
						++Check.Skipped;
						continue;
					}
					const uint8 Material = Painted[Index].MaterialID;
					if (bBand && Depth < BandDepth)
					{
						++Check.Expected;
						Check.Missing += Material != Band ? 1 : 0;
					}
					else
					{
						Check.Stray += (Material == BandMaterial || Material == OtherBandMaterial) ? 1 : 0;
						Check.OffBandDiffering += FMemory::Memcmp(&Painted[Index], &Natural[Index], sizeof(FVoxelData)) != 0 ? 1 : 0;
					}
				}
			}
		}
		return Check;
	}

	/** A line's values with a band of BandMaterial as wide as its core, four voxels deep. */
	FVoxelConditioningLineParams WithBand(FVoxelConditioningLineParams Params, float Strength, uint8 Material = BandMaterial, float BandHalfWidth = 0.0f)
	{
		Params.Strength = Strength;
		Params.Surface.BandMaterialID = Material;
		Params.Surface.BandHalfWidth = BandHalfWidth;
		Params.Surface.BandDepth = 4.0f;
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineSurfaceTest,
	"VoxelWorlds.Generation.ConditioningLine.Surface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace VoxelLineSurfaceTestUtils;

	// A line along X from 0 to 1,000 at Y 0, its target 0: the half width rises from 100 to 200 (150 at X 500), the
	// falloff is 200 wide. A band of material 7 as wide as the core and 4 voxels deep, caves under 12 voxels, clear to 300.
	FVoxelConditioningLineParams Params;
	Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(0.0, 0.0, 0.0), 0.0f, 100.0f));
	Params.Vertices.Add(FVoxelConditioningLineVertex(FVector(1000.0, 0.0, 0.0), 0.0f, 200.0f));
	Params.FalloffWidth = 200.0f;
	Params.Id = 1;
	Params.Surface.BandMaterialID = 7;
	Params.Surface.BandDepth = 4.0f;
	Params.Surface.MinCaveDepth = 12.0f;
	Params.Surface.ClearHalfWidth = 300.0f;
	const FVoxelConditioningLineRef Line = FVoxelConditioningLine::Make(Params);
	FVoxelConditioningLineSample At;

	// ---- One line at a place.
	TestTrue(TEXT("On the core the place is on the band, at full profile and weight"),
		Line->Sample(500.0, 140.0, 0.0, At) && At.bOnBand && At.Profile == 1.0f && At.Weight == 1.0f);
	TestTrue(TEXT("In the falloff the place is off the band, with a profile under 1"),
		Line->Sample(500.0, 160.0, 0.0, At) && !At.bOnBand && At.Profile > 0.0f && At.Profile < 1.0f);
	TestTrue(TEXT("The sample gives the distance and the half width there"),
		FMath::IsNearlyEqual(At.Distance, 160.0f, 1e-3f) && FMath::IsNearlyEqual(At.HalfWidth, 150.0f, 1e-3f));
	TestEqual(TEXT("The reach is the half width plus the falloff where the clearing is narrower"), Line->GetReach(), 400.0);
	TestFalse(TEXT("Beyond the reach the line has no sample"), Line->Sample(500.0, 420.0, 0.0, At));

	// A band narrower than the core, and never wider than it.
	{
		FVoxelConditioningLineParams NarrowParams = Params;
		NarrowParams.Surface.BandHalfWidth = 120.0f;
		const FVoxelConditioningLineRef Narrow = FVoxelConditioningLine::Make(NarrowParams);
		TestTrue(TEXT("A band of half width 120 holds a place 110 out"), Narrow->Sample(500.0, 110.0, 0.0, At) && At.bOnBand);
		TestTrue(TEXT("A band of half width 120 does not hold a place 130 out, on the core"), Narrow->Sample(500.0, 130.0, 0.0, At) && !At.bOnBand && At.Weight == 1.0f);
		TestTrue(TEXT("Where the core is 100 wide the band ends with it"), Narrow->Sample(0.0, 110.0, 0.0, At) && !At.bOnBand);
		TestTrue(TEXT("Where the core is 100 wide the band holds a place 90 out"), Narrow->Sample(0.0, 90.0, 0.0, At) && At.bOnBand);
	}

	// A clearing wider than the core and the falloff: the reach holds it, the height's reach does not.
	{
		FVoxelConditioningLineParams WideParams = Params;
		WideParams.Surface.ClearHalfWidth = 700.0f;
		const FVoxelConditioningLineRef Wide = FVoxelConditioningLine::Make(WideParams);
		TestEqual(TEXT("A wide clearing is the reach"), Wide->GetReach(), 700.0);
		TestEqual(TEXT("The height's reach stays the half width plus the falloff"), Wide->GetHeightReach(), 400.0);
		TestTrue(TEXT("Within the clearing past the falloff the line is found, with no weight"),
			Wide->Sample(500.0, 650.0, 0.0, At) && At.Profile == 0.0f && At.Weight == 0.0f && !At.bOnBand);
		TestTrue(TEXT("A gather finds the line by its clearing"), Wide->MeetsRegion(FBox2D(FVector2D(500.0, 640.0), FVector2D(500.0, 660.0))));
		FVoxelConditioning WideSet;
		WideSet.Lines.Add(Wide);
		TestTrue(TEXT("The clearing clears 650 out"), WideSet.IsClearedAt(500.0, 650.0));
		TestFalse(TEXT("The clearing ends at its half width"), WideSet.IsClearedAt(500.0, 710.0));
	}

	// The least half width a coarse reader takes: the core and the band at least that wide.
	TestTrue(TEXT("With a least half width of 350 a place 300 out is on the core and the band"),
		Line->Sample(500.0, 300.0, 350.0, At) && At.bOnBand && At.Weight == 1.0f && FMath::IsNearlyEqual(At.HalfWidth, 350.0f));
	TestTrue(TEXT("With a least half width of 350 a place 500 out is in the falloff"),
		Line->Sample(500.0, 500.0, 350.0, At) && !At.bOnBand && At.Profile > 0.0f && At.Profile < 1.0f);
	{
		FVoxelConditioningLineParams NoBand = Params;
		NoBand.Surface.BandMaterialID = -1;
		TestTrue(TEXT("A least half width gives a line with no band none"), FVoxelConditioningLine::Make(NoBand)->Sample(500.0, 0.0, 350.0, At) && !At.bOnBand);
	}

	// What Make makes of a surface.
	{
		FVoxelConditioningLineParams Odd = Params;
		Odd.Surface.BandDepth = 0.0f;
		TestFalse(TEXT("A band with no depth is no band"), FVoxelConditioningLine::Make(Odd)->GetSurface().HasBand());
		Odd = Params;
		Odd.Surface.BandMaterialID = 300;
		TestFalse(TEXT("A material outside the table's range is no band"), FVoxelConditioningLine::Make(Odd)->GetSurface().HasBand());
		Odd = Params;
		Odd.Surface.BandDepth = 0.4f;
		TestEqual(TEXT("A band reaches at least the surface's own voxel"), FVoxelConditioningLine::Make(Odd)->GetSurface().BandDepth, 1.0f);
		Odd = Params;
		Odd.Surface.ClearHalfWidth = -5.0f;
		Odd.Surface.MinCaveDepth = -5.0f;
		const FVoxelConditioningLineRef Clamped = FVoxelConditioningLine::Make(Odd);
		TestTrue(TEXT("Negative widths and depths are none"), Clamped->GetSurface().ClearHalfWidth == 0.0f && Clamped->GetSurface().MinCaveDepth == 0.0f);
		TestFalse(TEXT("A default surface is not set"), FVoxelConditioningLineSurface().IsSet());
	}

	// ---- The set.
	FVoxelConditioning Set;
	Set.Lines.Add(Line);
	uint8 Material = 0;
	float Depth = 0.0f;
	TestTrue(TEXT("The set names the band on the core"), Set.GetBandAt(500.0, 100.0, Material, &Depth) && Material == 7 && Depth == 4.0f);
	Material = 99;
	TestTrue(TEXT("The set names no band in the falloff and leaves the material alone"), !Set.GetBandAt(500.0, 200.0, Material) && Material == 99);
	TestTrue(TEXT("Inside the clear width the place is cleared"), Set.IsClearedAt(500.0, 290.0));
	TestFalse(TEXT("Outside the clear width it is not"), Set.IsClearedAt(500.0, 310.0));
	TestEqual(TEXT("On the core caves keep the whole minimum depth"), Set.GetMinCaveDepthAt(500.0, 0.0), 12.0f);
	TestEqual(TEXT("Half way across the falloff they keep half of it"), Set.GetMinCaveDepthAt(500.0, 250.0), 6.0f, 1e-3f);
	TestEqual(TEXT("Beyond the falloff they keep none"), Set.GetMinCaveDepthAt(500.0, 360.0), 0.0f);
	TestTrue(TEXT("The set has line surfaces"), Set.HasLineSurfaces());
	{
		FVoxelConditioning Plain;
		Plain.Lines.Add(FVoxelConditioningLine::Make(MakeStraight(FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0), 0.0f, 0.0f, 100.0f, 200.0f)));
		Material = 99;
		TestTrue(TEXT("A line with a default surface paints, clears and deepens nothing"),
			!Plain.HasLineSurfaces() && !Plain.GetBandAt(500.0, 0.0, Material) && !Plain.IsClearedAt(500.0, 0.0) && Plain.GetMinCaveDepthAt(500.0, 0.0) == 0.0f);
	}

	// A line that only paints: no height, still a band.
	{
		FVoxelConditioningLineParams PaintParams = Params;
		PaintParams.Strength = 0.0f;
		FVoxelConditioning Paint;
		Paint.Lines.Add(FVoxelConditioningLine::Make(PaintParams));
		TestEqual(TEXT("A line of strength 0 moves no height"), Paint.ApplyToHeight(500.0, 0.0, 123.0f), 123.0f);
		TestTrue(TEXT("A line of strength 0 still paints its band"), Paint.GetBandAt(500.0, 0.0, Material) && Material == 7);
	}

	// Two lines that cross: the later id's band where both lie, the deeper cave depth.
	{
		FVoxelConditioningLineParams CrossParams = MakeStraight(FVector2D(500.0, -1000.0), FVector2D(500.0, 1000.0), 0.0f, 0.0f, 100.0f, 200.0f, EVoxelConditioningLineBlend::Level, 2);
		CrossParams.Surface.BandMaterialID = 9;
		CrossParams.Surface.BandDepth = 6.0f;
		CrossParams.Surface.MinCaveDepth = 20.0f;
		FVoxelConditioning Crossing;
		Crossing.Lines.Add(FVoxelConditioningLine::Make(CrossParams));
		Crossing.Lines.Add(Line);
		Crossing.SortLines();
		TestTrue(TEXT("Where two bands overlap the later line's wins, with its depth"), Crossing.GetBandAt(500.0, 50.0, Material, &Depth) && Material == 9 && Depth == 6.0f);
		TestTrue(TEXT("Beside the later line the earlier one's band shows"), Crossing.GetBandAt(300.0, 50.0, Material) && Material == 7);
		TestEqual(TEXT("The deeper minimum cave depth holds"), Crossing.GetMinCaveDepthAt(500.0, 0.0), 20.0f);
	}

	// The set's least half width: the height and the band widen, the clearing and the cave depth do not.
	{
		FVoxelConditioning Coarse = Set;
		Coarse.LeastHalfWidth = 350.0f;
		TestTrue(TEXT("A coarse reader's band holds a place 300 out"), Coarse.GetBandAt(500.0, 300.0, Material) && Material == 7);
		TestEqual(TEXT("A coarse reader's height is the target 300 out"), Coarse.ApplyToHeight(500.0, 300.0, 50.0f), 0.0f);
		TestEqual(TEXT("The set as it is leaves the height half blended 250 out"), Set.ApplyToHeight(500.0, 250.0, 50.0f), 25.0f, 1e-2f);
		TestFalse(TEXT("The clear width does not widen"), Coarse.IsClearedAt(500.0, 310.0));
		TestEqual(TEXT("The cave depth does not widen"), Coarse.GetMinCaveDepthAt(500.0, 250.0), 6.0f, 1e-3f);
		Coarse.Reset();
		TestEqual(TEXT("Reset clears the least half width"), Coarse.LeastHalfWidth, 0.0f);
	}

	// ---- A planet line: the same over the distance along the surface.
	{
		const double R = 100000.0;
		const FVector A = FVector(1.0, 0.0, 0.0);
		const FVector B = FVector(FMath::Cos(3000.0 / R), FMath::Sin(3000.0 / R), 0.0);
		FVoxelConditioningLineParams ArcParams = MakeArc(A, B, R, 50.0f, 250.0f, 150.0f, 200.0f);
		ArcParams.Surface.BandMaterialID = 5;
		ArcParams.Surface.MinCaveDepth = 10.0f;
		ArcParams.Surface.ClearHalfWidth = 250.0f;
		FVoxelConditioning Sphere;
		Sphere.Lines.Add(FVoxelConditioningLine::Make(ArcParams));
		const auto On = [&](double AlongUU, double AcrossUU)
		{
			return FVector(FMath::Cos(AlongUU / R) * FMath::Cos(AcrossUU / R), FMath::Sin(AlongUU / R) * FMath::Cos(AcrossUU / R), FMath::Sin(AcrossUU / R));
		};
		TestTrue(TEXT("Planet: the band on the core"), Sphere.GetBandOnSphere(On(1500.0, 140.0), R, Material, &Depth) && Material == 5 && Depth == 4.0f);
		TestFalse(TEXT("Planet: no band in the falloff"), Sphere.GetBandOnSphere(On(1500.0, 170.0), R, Material));
		TestTrue(TEXT("Planet: cleared inside the clear width"), Sphere.IsClearedOnSphere(On(1500.0, -240.0), R));
		TestFalse(TEXT("Planet: not cleared outside it"), Sphere.IsClearedOnSphere(On(1500.0, 260.0), R));
		TestEqual(TEXT("Planet: the minimum cave depth on the core"), Sphere.GetMinCaveDepthOnSphere(On(1500.0, 0.0), R), 10.0f);
		TestEqual(TEXT("Planet: half of it half way across the falloff"), Sphere.GetMinCaveDepthOnSphere(On(1500.0, 250.0), R), 5.0f, 2e-2f);
		Sphere.LeastHalfWidth = 400.0f;
		TestTrue(TEXT("Planet: a coarse reader's band holds a place 380 out"), Sphere.GetBandOnSphere(On(1500.0, 380.0), R, Material));
		// Each kind is ignored by the other's functions.
		TestTrue(TEXT("A planet line paints and clears nothing on a heightmap"), !Sphere.GetBandAt(0.0, 0.0, Material) && !Sphere.IsClearedAt(0.0, 0.0));
		TestTrue(TEXT("A heightmap line paints and clears nothing on a planet"), !Set.GetBandOnSphere(A, R, Material) && !Set.IsClearedOnSphere(A, R));
	}

	// ---- The warnings.
	{
		FVoxelConditioningLineLimits Limits;
		Limits.VoxelSize = VoxelSize;
		const auto Has = [](const TArray<FString>& Warnings, const TCHAR* Text)
		{
			return Warnings.ContainsByPredicate([Text](const FString& W) { return W.Contains(Text); });
		};
		FVoxelConditioningLineParams Road = WithBand(MakeStraight(FVector2D(0.0, 0.0), FVector2D(6000.0, 2000.0), 100.0f, 500.0f, 3.0f * VoxelSize, 4.0f * VoxelSize), 1.0f);
		Road.Surface.ClearHalfWidth = 6.0f * VoxelSize;
		Road.Surface.MinCaveDepth = 8.0f;
		const TArray<FString> RoadWarnings = FVoxelConditioningLine::Make(Road)->GetWarnings(Limits);
		TestEqual(FString::Printf(TEXT("A road with a band four voxels deep has no warning (%s)"), RoadWarnings.Num() > 0 ? *RoadWarnings[0] : TEXT("none")), RoadWarnings.Num(), 0);
		FVoxelConditioningLineParams Shallow = Road;
		Shallow.Surface.BandDepth = 2.0f;
		TestTrue(TEXT("A band under four voxels deep is warned about"), Has(FVoxelConditioningLine::Make(Shallow)->GetWarnings(Limits), TEXT("voxels deep")));
		FVoxelConditioningLineParams Thin = Road;
		Thin.Surface.BandHalfWidth = 0.75f * VoxelSize;
		TestTrue(TEXT("A band under two voxels wide is warned about"), Has(FVoxelConditioningLine::Make(Thin)->GetWarnings(Limits), TEXT("the band is")));
		// A wide verge does not tighten the bend's rule: a curve of the height's reach keeps it.
		FVoxelConditioningLineParams Curve;
		Curve.FalloffWidth = 4.0f * VoxelSize;
		Curve.Surface.ClearHalfWidth = 12.0f * VoxelSize;
		const double Radius = 600.0;
		for (int32 Step = 0; Step <= 12; ++Step)
		{
			const double Angle = Step * (2.0 * VoxelSize / Radius);
			Curve.Vertices.Add(FVoxelConditioningLineVertex(FVector(Radius * FMath::Sin(Angle), Radius * (1.0 - FMath::Cos(Angle)), 0.0), 0.0f, 3.0f * VoxelSize));
		}
		const TArray<FString> CurveWarnings = FVoxelConditioningLine::Make(Curve)->GetWarnings(Limits);
		TestEqual(FString::Printf(TEXT("A level curve with a wide verge has no warning (%s)"), CurveWarnings.Num() > 0 ? *CurveWarnings[0] : TEXT("none")), CurveWarnings.Num(), 0);
	}

	// ---- A chunk's segment list gives each column what the set gives.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::InfinitePlane);
		FVoxelConditioningLineParams PaintParams = WithBand(MakeStraight(FVector2D(-2400.0, -900.0), FVector2D(7600.0, 6100.0), 0.0f, 0.0f, 2.0f * VoxelSize, 3.0f * VoxelSize), 0.0f, 7);
		PaintParams.Surface.MinCaveDepth = 14.0f;
		FVoxelConditioning Paint;
		Paint.Lines.Add(FVoxelConditioningLine::Make(PaintParams));
		int32 OnBand = 0;
		int32 Wrong = 0;
		float LargestCave = 0.0f;
		for (const FIntVector& Chunk : { FIntVector(1, 1, 0), FIntVector(0, 0, 0), FIntVector(2, 1, 0) })
		{
			FVoxelNoiseGenerationRequest Request = MakeRequest(*Config, Chunk);
			Request.Conditioning = Paint;
			FVoxelChunkLineSegments List;
			FVoxelChunkLineSegments::Build(Request, List);
			FVoxelChunkLineColumns Columns;
			List.EvaluateColumns(ChunkSize, VoxelSize, Columns);
			const FVector Origin = Request.GetChunkWorldPosition();
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const double WX = Origin.X + X * VoxelSize;
					const double WY = Origin.Y + Y * VoxelSize;
					const FVoxelChunkLineSurface Surface = Columns.HasSurfaces() ? Columns.Surfaces[X + Y * ChunkSize] : FVoxelChunkLineSurface();
					LargestCave = FMath::Max(LargestCave, FMath::Abs(Surface.MinCaveDepth - Paint.GetMinCaveDepthAt(WX, WY)));
					if (Paint.Lines[0]->Sample(WX, WY, 0.0, At) && FMath::Abs(At.Distance - At.HalfWidth) < 0.05f)
					{
						continue;
					}
					const bool bBand = Paint.GetBandAt(WX, WY, Material, &Depth);
					OnBand += bBand ? 1 : 0;
					Wrong += (bBand != (Surface.BandMaterial >= 0) || (bBand && (Surface.BandMaterial != 7 || Surface.BandDepth != 4.0f))) ? 1 : 0;
				}
			}
		}
		AddInfo(FString::Printf(TEXT("Three chunks' columns: %d on the band, %d where the list and the set disagree; the cave depth differs by at most %.4f voxels"),
			OnBand, Wrong, LargestCave));
		TestTrue(TEXT("The band crosses the chunks"), OnBand > 150);
		TestEqual(TEXT("The chunk's list names the band on the columns the set names it on"), Wrong, 0);
		TestTrue(TEXT("The chunk's list gives the set's minimum cave depth"), LargestCave < 0.01f);
		Config->RemoveFromRoot();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineBandTest,
	"VoxelWorlds.Generation.ConditioningLine.Band",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineBandTest::RunTest(const FString& Parameters)
{
	using namespace VoxelLineSurfaceTestUtils;
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;
	const auto Report = [this](const FString& Name, const FBandCheck& Check, bool bPaintOnly)
	{
		AddInfo(FString::Printf(TEXT("%s: %d voxels, the band names %d; %d of them not painted, %d painted off the band; off the band %d differ from the chunk without the line, %d differ in density or biome; %d left out at the band's edge or depth"),
			*Name, Check.Voxels, Check.Expected, Check.Missing, Check.Stray, Check.OffBandDiffering, Check.ShapeDiffering, Check.Skipped));
		TestTrue(FString::Printf(TEXT("%s: the band crosses the chunk"), *Name), Check.Expected > 400);
		TestEqual(FString::Printf(TEXT("%s: every voxel the band names carries its material"), *Name), Check.Missing, 0);
		TestEqual(FString::Printf(TEXT("%s: no voxel off the band carries it"), *Name), Check.Stray, 0);
		if (bPaintOnly)
		{
			TestEqual(FString::Printf(TEXT("%s: off the band the chunk is byte for byte the one without the line"), *Name), Check.OffBandDiffering, 0);
			TestEqual(FString::Printf(TEXT("%s: a line that only paints moves no density and no biome"), *Name), Check.ShapeDiffering, 0);
		}
	};

	// The plane with the biome pipeline (the band after the height rules), and the island with the legacy materials.
	UVoxelBiomeConfiguration* BiomeConfig = NewObject<UVoxelBiomeConfiguration>();
	BiomeConfig->AddToRoot();
	BiomeConfig->InitializeDefaults();
	BiomeConfig->RebuildCaches();
	for (const EWorldMode WorldMode : { EWorldMode::InfinitePlane, EWorldMode::IslandBowl })
	{
		const bool bBiomes = WorldMode == EWorldMode::InfinitePlane;
		const FString World = bBiomes ? TEXT("Plane (biomes)") : TEXT("Island");
		UVoxelWorldConfiguration* Config = MakeConfig(WorldMode);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FVector2D Site(2600.0, 3100.0);
		const float NaturalAtSite = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector(Site.X, Site.Y, 0.0), Config->NoiseParams).Position.Z);
		const auto RequestAt = [&](float Height)
		{
			FVoxelNoiseGenerationRequest Request = MakeRequest(*Config, ChunkAt(*Config, FVector(Site.X, Site.Y, Height)));
			if (bBiomes)
			{
				Request.bEnableBiomes = true;
				Request.BiomeConfiguration = BiomeConfig;
			}
			return Request;
		};
		const FVoxelConditioningLineParams Along = MakeStraight(Site + FVector2D(-5000.0, -3500.0), Site + FVector2D(5000.0, 3500.0), NaturalAtSite + 220.0f, NaturalAtSite + 220.0f, HalfWidth, Falloff);

		// A line that only paints.
		{
			FVoxelConditioning Paint;
			Paint.Lines.Add(FVoxelConditioningLine::Make(WithBand(Along, 0.0f)));
			Report(World + TEXT(", a line that only paints"), CheckBand(*Mode, RequestAt(NaturalAtSite), Paint, FVector::ZeroVector, 0.0), true);
		}
		// A levelled road with a band narrower than its core: painted on the ground it levels.
		{
			FVoxelConditioning Road;
			Road.Lines.Add(FVoxelConditioningLine::Make(WithBand(Along, 1.0f, BandMaterial, 2.0f * VoxelSize)));
			Report(World + TEXT(", a levelled road with a narrower band"), CheckBand(*Mode, RequestAt(NaturalAtSite + 220.0f), Road, FVector::ZeroVector, 0.0), false);
		}
		// Two lines that only paint, crossing at the site: the later id's band where both lie.
		{
			FVoxelConditioningLineParams Across = MakeStraight(Site + FVector2D(-3000.0, 4000.0), Site + FVector2D(3000.0, -4000.0), 0.0f, 0.0f, HalfWidth, Falloff, EVoxelConditioningLineBlend::Level, 2);
			FVoxelConditioning Crossing;
			Crossing.Lines.Add(FVoxelConditioningLine::Make(WithBand(Across, 0.0f, OtherBandMaterial)));
			Crossing.Lines.Add(FVoxelConditioningLine::Make(WithBand(Along, 0.0f)));
			Crossing.SortLines();
			uint8 AtSite = 0;
			TestTrue(World + TEXT(": where two bands cross the later line's is named"), Crossing.GetBandAt(Site.X, Site.Y, AtSite) && AtSite == OtherBandMaterial);
			Report(World + TEXT(", two crossing lines"), CheckBand(*Mode, RequestAt(NaturalAtSite), Crossing, FVector::ZeroVector, 0.0), true);
		}
		Config->RemoveFromRoot();
	}
	BiomeConfig->RemoveFromRoot();

	// A planet: an arc through a site across a cube-face edge.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::SphericalPlanet);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FSphericalPlanetWorldMode& PlanetMode = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(*Config);
		const double R = Planet.PlanetRadius;
		const FVector A = FVector(1.0, 0.95, 0.3).GetSafeNormal();
		const FVector B = FVector(0.95, 1.0, 0.36).GetSafeNormal();
		const FVector Site = (A + B).GetSafeNormal();
		const float NaturalAtSite = PlanetMode.GetTerrainAltitude(Site, Config->NoiseParams);
		const FVoxelConditioningLineParams Arc = MakeArc(A, B, R, NaturalAtSite + 240.0f, NaturalAtSite + 240.0f, HalfWidth, Falloff);
		{
			FVoxelConditioning Paint;
			Paint.Lines.Add(FVoxelConditioningLine::Make(WithBand(Arc, 0.0f)));
			Report(TEXT("Planet, a line that only paints"),
				CheckBand(*Mode, MakeRequest(*Config, ChunkAt(*Config, Planet.PlanetCenter + Site * (R + NaturalAtSite))), Paint, Planet.PlanetCenter, R), true);
		}
		{
			FVoxelConditioning Road;
			Road.Lines.Add(FVoxelConditioningLine::Make(WithBand(Arc, 1.0f)));
			Report(TEXT("Planet, a levelled road"),
				CheckBand(*Mode, MakeRequest(*Config, ChunkAt(*Config, Planet.PlanetCenter + Site * (R + NaturalAtSite + 240.0f))), Road, Planet.PlanetCenter, R), false);
		}
		Config->RemoveFromRoot();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineCaveTest,
	"VoxelWorlds.Generation.ConditioningLine.CaveUnderLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineCaveTest::RunTest(const FString& Parameters)
{
	using namespace VoxelLineSurfaceTestUtils;
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;
	const float LineCaveDepth = 14.0f;

	// One cheese layer that opens at the surface (a minimum depth of 2 voxels, a fade of 2) and carves about half the
	// ground: without a line the chunks along it are full of openings.
	UVoxelCaveConfiguration* Caves = NewObject<UVoxelCaveConfiguration>();
	Caves->AddToRoot();
	Caves->bEnableCaves = true;
	Caves->UnderwaterMinDepth = 0.0f;
	Caves->bOverrideCaveWallMaterial = false;
	Caves->BiomeOverrides.Empty();
	Caves->CaveLayers.Empty();
	{
		FCaveLayerConfig Cheese;
		Cheese.bEnabled = true;
		Cheese.CaveType = ECaveType::Cheese;
		Cheese.SeedOffset = 977;
		Cheese.Frequency = 0.0022f;
		Cheese.Octaves = 2;
		Cheese.Persistence = 0.5f;
		Cheese.Lacunarity = 2.0f;
		Cheese.Threshold = -0.05f;
		Cheese.CarveStrength = 1.0f;
		Cheese.CarveFalloff = 0.2f;
		Cheese.MinDepth = 2.0f;
		Cheese.MaxDepth = 0.0f;
		Cheese.DepthFadeWidth = 2.0f;
		Cheese.VerticalScale = 1.0f;
		Caves->CaveLayers.Add(Cheese);
	}
	const float Fade = Caves->CaveLayers[0].DepthFadeWidth;

	struct FCaveCheck
	{
		/** Voxels on the line's core above its minimum cave depth less the fade: carved without the line, with it */
		int32 CoreShallow = 0;
		int32 CoreShallowCarvedWithout = 0;
		int32 CoreShallowCarvedWith = 0;
		/** Voxels on the core below the depth that stay carved with the line (the caves go on underneath) */
		int32 CoreDeepCarvedWith = 0;
		/** Voxels carved with the line that are not carved without it (none: a line never opens a cave) */
		int32 CarvedOnlyWith = 0;
		/**
		 * Voxels beyond the line's reach, those of them whose density, material or biome differs from the chunk without
		 * the line, and those whose flags alone differ (the underground flag is given by a pass over the chunk's air after
		 * generation, so it follows a cave that the line closed beside the voxel).
		 */
		int32 Beyond = 0;
		int32 BeyondDiffering = 0;
		int32 BeyondFlagsDiffering = 0;
		/** Voxels where the single-voxel reader's solidity differs from the chunk's, of those compared */
		int32 ReaderCompared = 0;
		int32 ReaderWrong = 0;
	};
	// Three chunks for a request: with no caves (the ground alone), with caves, with caves and the line.
	const auto Check = [&](const IVoxelWorldMode& Mode, FVoxelNoiseGenerationRequest Request, const FVoxelConditioning& Conditioning, const FVector& PlanetCentre,
		double PlanetRadius, const FSphericalPlanetParams* PlanetParams)
	{
		FCaveCheck Out;
		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		TArray<FVoxelData> Ground, Without, With;
		Generator.GenerateChunkCPU(Request, Ground);
		Request.bEnableCaves = true;
		Request.CaveConfiguration = Caves;
		Generator.GenerateChunkCPU(Request, Without);
		Request.Conditioning = Conditioning;
		Generator.GenerateChunkCPU(Request, With);
		Generator.Shutdown();
		const bool bPlanet = PlanetRadius > 0.0;
		const FVector ChunkOrigin = Request.GetChunkWorldPosition();
		const FVoxelConditioningLine& Line = *Conditioning.Lines[0];
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
					const FVector WorldPos = ChunkOrigin + FVector(X, Y, Z) * VoxelSize;
					float Depth = 0.0f;
					FVoxelConditioningLineSample At;
					bool bReached = false;
					if (bPlanet)
					{
						const FVector Relative = WorldPos - PlanetCentre;
						const FVector Direction = Relative.GetSafeNormal();
						Depth = static_cast<float>((static_cast<double>(static_cast<const FSphericalPlanetWorldMode&>(Mode).GetTerrainAltitude(Direction, Request.NoiseParams))
							- (Relative.Size() - PlanetRadius)) / VoxelSize);
						bReached = Line.SampleOnSphere(Direction, PlanetRadius, 0.0, At);
					}
					else
					{
						Depth = (static_cast<float>(Mode.GetSurfaceOnUpLine(FVector(WorldPos.X, WorldPos.Y, 0.0), Request.NoiseParams).Position.Z) - static_cast<float>(WorldPos.Z)) / VoxelSize;
						bReached = Line.Sample(WorldPos.X, WorldPos.Y, 0.0, At);
					}
					const bool bCarvedWithout = Without[Index].Density < Ground[Index].Density;
					const bool bCarvedWith = With[Index].Density < Ground[Index].Density;
					Out.CarvedOnlyWith += With[Index].Density < Without[Index].Density ? 1 : 0;
					if (!bReached || At.Profile <= 0.0f)
					{
						++Out.Beyond;
						const bool bGenerated = With[Index].Density != Without[Index].Density || With[Index].MaterialID != Without[Index].MaterialID
							|| With[Index].BiomeID != Without[Index].BiomeID;
						Out.BeyondDiffering += bGenerated ? 1 : 0;
						Out.BeyondFlagsDiffering += (!bGenerated && FMemory::Memcmp(&With[Index], &Without[Index], sizeof(FVoxelData)) != 0) ? 1 : 0;
					}
					else if (At.Distance < At.HalfWidth - 1.0f && Depth > 0.0f)
					{
						if (Depth < LineCaveDepth - Fade - 0.05f)
						{
							++Out.CoreShallow;
							Out.CoreShallowCarvedWithout += bCarvedWithout ? 1 : 0;
							Out.CoreShallowCarvedWith += bCarvedWith ? 1 : 0;
						}
						else if (Depth > LineCaveDepth + 0.05f)
						{
							Out.CoreDeepCarvedWith += bCarvedWith ? 1 : 0;
						}
					}
					// The single-voxel reader, as scatter reads the voxels past a chunk's faces: solid where the chunk is,
					// away from the threshold's own rounding.
					if (FMath::Abs(static_cast<int32>(With[Index].Density) - static_cast<int32>(VOXEL_SURFACE_THRESHOLD)) > 6)
					{
						const FVoxelData Read = FVoxelSurfaceQuery::GenerateVoxelAt(Mode, WorldPos, VoxelSize, Request.NoiseParams, &Conditioning, Caves,
							FVoxelWaterLevels(), false, 0);
						++Out.ReaderCompared;
						Out.ReaderWrong += (Read.Density >= VOXEL_SURFACE_THRESHOLD) != (With[Index].Density >= VOXEL_SURFACE_THRESHOLD) ? 1 : 0;
					}
				}
			}
		}
		(void)PlanetParams;
		return Out;
	};
	const auto Report = [this](const TCHAR* Name, const FCaveCheck& Out)
	{
		AddInfo(FString::Printf(TEXT("%s: on the core above the line's depth %d voxels, %d carved without the line and %d with it; %d carved under the depth with it; %d voxels carved only with the line; beyond the reach %d voxels, %d differing in density, material or biome and %d in their flags alone; the single-voxel reader differs on %d of %d"),
			Name, Out.CoreShallow, Out.CoreShallowCarvedWithout, Out.CoreShallowCarvedWith, Out.CoreDeepCarvedWith, Out.CarvedOnlyWith, Out.Beyond, Out.BeyondDiffering,
			Out.BeyondFlagsDiffering, Out.ReaderWrong, Out.ReaderCompared));
		TestTrue(FString::Printf(TEXT("%s: without the line caves open on the core above its depth (non-vacuous)"), Name), Out.CoreShallowCarvedWithout > 200);
		TestEqual(FString::Printf(TEXT("%s: with the line no cave opens on the core above its depth"), Name), Out.CoreShallowCarvedWith, 0);
		TestTrue(FString::Printf(TEXT("%s: the caves go on under the depth"), Name), Out.CoreDeepCarvedWith > 50);
		TestEqual(FString::Printf(TEXT("%s: the line opens no cave"), Name), Out.CarvedOnlyWith, 0);
		TestTrue(FString::Printf(TEXT("%s: part of the chunk is beyond the reach"), Name), Out.Beyond > 1000);
		TestEqual(FString::Printf(TEXT("%s: beyond the reach the chunk's densities, materials and biomes are the ones without the line"), Name), Out.BeyondDiffering, 0);
		TestEqual(FString::Printf(TEXT("%s: the single-voxel reader is solid where the chunk is"), Name), Out.ReaderWrong, 0);
	};

	// The plane: a line that only paints, so the ground is the natural one and the depth the natural depth.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::InfinitePlane);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FVector2D Site(2600.0, 3100.0);
		const float NaturalAtSite = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector(Site.X, Site.Y, 0.0), Config->NoiseParams).Position.Z);
		FVoxelConditioningLineParams Params = MakeStraight(Site + FVector2D(-5000.0, -3500.0), Site + FVector2D(5000.0, 3500.0), 0.0f, 0.0f, HalfWidth, Falloff);
		Params.Strength = 0.0f;
		Params.Surface.MinCaveDepth = LineCaveDepth;
		FVoxelConditioning Conditioning;
		Conditioning.Lines.Add(FVoxelConditioningLine::Make(Params));
		// The chunk that holds the ground at the site, and the one under it (the caves under the depth).
		FCaveCheck Total;
		const FIntVector Top = ChunkAt(*Config, FVector(Site.X, Site.Y, NaturalAtSite));
		for (const FIntVector& Chunk : { Top, Top - FIntVector(0, 0, 1) })
		{
			const FCaveCheck One = Check(*Mode, MakeRequest(*Config, Chunk), Conditioning, FVector::ZeroVector, 0.0, nullptr);
			Total.CoreShallow += One.CoreShallow;
			Total.CoreShallowCarvedWithout += One.CoreShallowCarvedWithout;
			Total.CoreShallowCarvedWith += One.CoreShallowCarvedWith;
			Total.CoreDeepCarvedWith += One.CoreDeepCarvedWith;
			Total.CarvedOnlyWith += One.CarvedOnlyWith;
			Total.Beyond += One.Beyond;
			Total.BeyondDiffering += One.BeyondDiffering;
			Total.BeyondFlagsDiffering += One.BeyondFlagsDiffering;
			Total.ReaderCompared += One.ReaderCompared;
			Total.ReaderWrong += One.ReaderWrong;
		}
		Report(TEXT("Plane"), Total);
		Config->RemoveFromRoot();
	}

	// A planet: the depth is radial.
	{
		UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::SphericalPlanet);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FSphericalPlanetWorldMode& PlanetMode = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		const FSphericalPlanetParams Planet = FVoxelWorldModeFactory::MakePlanetParams(*Config);
		const double R = Planet.PlanetRadius;
		const FVector A = FVector(1.0, 0.95, 0.3).GetSafeNormal();
		const FVector B = FVector(0.95, 1.0, 0.36).GetSafeNormal();
		const FVector Site = (A + B).GetSafeNormal();
		const float NaturalAtSite = PlanetMode.GetTerrainAltitude(Site, Config->NoiseParams);
		FVoxelConditioningLineParams Params = MakeArc(A, B, R, 0.0f, 0.0f, HalfWidth, Falloff);
		Params.Strength = 0.0f;
		Params.Surface.MinCaveDepth = LineCaveDepth;
		FVoxelConditioning Conditioning;
		Conditioning.Lines.Add(FVoxelConditioningLine::Make(Params));
		FCaveCheck Total;
		for (const float Under : { 300.0f, 1400.0f })
		{
			const FCaveCheck One = Check(*Mode, MakeRequest(*Config, ChunkAt(*Config, Planet.PlanetCenter + Site * (R + NaturalAtSite - Under))), Conditioning,
				Planet.PlanetCenter, R, &Planet);
			Total.CoreShallow += One.CoreShallow;
			Total.CoreShallowCarvedWithout += One.CoreShallowCarvedWithout;
			Total.CoreShallowCarvedWith += One.CoreShallowCarvedWith;
			Total.CoreDeepCarvedWith += One.CoreDeepCarvedWith;
			Total.CarvedOnlyWith += One.CarvedOnlyWith;
			Total.Beyond += One.Beyond;
			Total.BeyondDiffering += One.BeyondDiffering;
			Total.BeyondFlagsDiffering += One.BeyondFlagsDiffering;
			Total.ReaderCompared += One.ReaderCompared;
			Total.ReaderWrong += One.ReaderWrong;
		}
		Report(TEXT("Planet"), Total);
		Config->RemoveFromRoot();
	}
	Caves->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningLineTreesTest,
	"VoxelWorlds.Generation.ConditioningLine.Trees",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningLineTreesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelLineSurfaceTestUtils;

	// A block world's trees over 8 x 8 chunks, many to a chunk and on any slope, and a levelled line through them with
	// a clear width wider than its core.
	UVoxelWorldConfiguration* Config = MakeConfig(EWorldMode::InfinitePlane);
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	TArray<FVoxelTreeTemplate> Templates;
	Templates.AddDefaulted();
	Templates[0].MaxSlopeDegrees = 90.0f;
	const float TreeDensity = 12.0f;
	const float HalfWidth = 3.0f * VoxelSize;
	const float Falloff = 4.0f * VoxelSize;
	const float Clear = 5.0f * VoxelSize;
	const double ChunkWorldSize = static_cast<double>(ChunkSize) * VoxelSize;
	const FVector2D Start(Config->WorldOrigin.X + 0.5 * ChunkWorldSize, Config->WorldOrigin.Y + 1.3 * ChunkWorldSize);
	const FVector2D End(Config->WorldOrigin.X + 7.5 * ChunkWorldSize, Config->WorldOrigin.Y + 6.1 * ChunkWorldSize);
	// The target 300 uu over the natural ground at the line's middle: the ground moves beside it.
	const FVector2D Middle = 0.5 * (Start + End);
	const float Target = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector(Middle.X, Middle.Y, 0.0), Config->NoiseParams).Position.Z) + 300.0f;
	FVoxelConditioningLineParams Params = MakeStraight(Start, End, Target, Target, HalfWidth, Falloff);
	Params.Surface.ClearHalfWidth = Clear;
	FVoxelConditioning Conditioning;
	Conditioning.Lines.Add(FVoxelConditioningLine::Make(Params));
	const FVoxelConditioningLine& Line = *Conditioning.Lines[0];

	struct FTree
	{
		FIntVector Base;
		int32 Template;
		uint32 Seed;
	};
	const auto Plant = [&](const FVoxelConditioning* With)
	{
		TArray<FTree> Trees;
		for (int32 CX = 0; CX < 8; ++CX)
		{
			for (int32 CY = 0; CY < 8; ++CY)
			{
				TArray<FIntVector> Positions;
				TArray<int32> Ids;
				TArray<uint32> Seeds;
				FVoxelTreeInjector::ComputeTreePositionsForChunk(FIntVector(CX, CY, 0), ChunkSize, VoxelSize, Config->WorldOrigin, Config->NoiseParams.Seed,
					Config->NoiseParams, *Mode, TreeDensity, Templates, nullptr, false, 0.0f, Positions, Ids, Seeds, With);
				for (int32 Index = 0; Index < Positions.Num(); ++Index)
				{
					Trees.Add({ Positions[Index], Ids[Index], Seeds[Index] });
				}
			}
		}
		return Trees;
	};
	const auto WorldXY = [&](const FTree& Tree)
	{
		return FVector2D(Config->WorldOrigin.X + Tree.Base.X * VoxelSize, Config->WorldOrigin.Y + Tree.Base.Y * VoxelSize);
	};
	const TArray<FTree> Natural = Plant(nullptr);
	const TArray<FTree> Beside = Plant(&Conditioning);

	// Without the line some trees stand inside the clear width; with it none does.
	int32 NaturalInClear = 0;
	for (const FTree& Tree : Natural)
	{
		const FVector2D P = WorldXY(Tree);
		NaturalInClear += Conditioning.IsClearedAt(P.X, P.Y) ? 1 : 0;
	}
	int32 InClear = 0;
	int32 BeyondReach = 0;
	int32 InFalloff = 0;
	int32 WrongHeight = 0;
	int32 Moved = 0;
	FVoxelConditioningLineSample At;
	for (const FTree& Tree : Beside)
	{
		const FVector2D P = WorldXY(Tree);
		InClear += Conditioning.IsClearedAt(P.X, P.Y) ? 1 : 0;
		const float NaturalHeight = static_cast<float>(Mode->GetSurfaceOnUpLine(FVector(P.X, P.Y, 0.0), Config->NoiseParams).Position.Z);
		const float Height = Conditioning.ApplyToHeight(P.X, P.Y, NaturalHeight);
		if (!Line.Sample(P.X, P.Y, 0.0, At) || At.Profile <= 0.0f)
		{
			++BeyondReach;
			continue;
		}
		// Beside the line: the base is the block under the ground the line leaves.
		++InFalloff;
		WrongHeight += Tree.Base.Z != FMath::FloorToInt32((Height - Config->WorldOrigin.Z) / VoxelSize) ? 1 : 0;
		Moved += FMath::FloorToInt32((Height - Config->WorldOrigin.Z) / VoxelSize) != FMath::FloorToInt32((NaturalHeight - Config->WorldOrigin.Z) / VoxelSize) ? 1 : 0;
	}
	// Beyond the reach the trees are the ones planted without the line: the same bases, templates and seeds.
	int32 NaturalBeyond = 0;
	int32 BeyondMissing = 0;
	for (const FTree& Tree : Natural)
	{
		const FVector2D P = WorldXY(Tree);
		if (Line.Sample(P.X, P.Y, 0.0, At) && At.Profile > 0.0f)
		{
			continue;
		}
		if (Conditioning.IsClearedAt(P.X, P.Y))
		{
			continue;
		}
		++NaturalBeyond;
		BeyondMissing += Beside.ContainsByPredicate([&Tree](const FTree& Other) { return Other.Base == Tree.Base && Other.Template == Tree.Template && Other.Seed == Tree.Seed; }) ? 0 : 1;
	}
	AddInfo(FString::Printf(TEXT("%d trees without the line, %d of them inside the clear width; %d with it, %d inside the clear width; %d beside the line outside the clear width, %d of them on ground the line moved a block or more, %d at the wrong height; %d beyond the reach against %d without the line, %d of those missing"),
		Natural.Num(), NaturalInClear, Beside.Num(), InClear, InFalloff, Moved, WrongHeight, BeyondReach, NaturalBeyond, BeyondMissing));
	TestTrue(TEXT("Without the line trees stand inside its clear width (non-vacuous)"), NaturalInClear > 5);
	TestEqual(TEXT("With the line no tree stands inside its clear width"), InClear, 0);
	TestEqual(TEXT("The trees removed are the ones inside the clear width"), Beside.Num(), Natural.Num() - NaturalInClear);
	TestTrue(TEXT("Some trees stand beside the line on ground it moved"), Moved > 0);
	TestEqual(TEXT("A tree beside the line stands on the ground the line leaves"), WrongHeight, 0);
	TestEqual(TEXT("Beyond the reach the trees are the ones planted without the line"), BeyondMissing, 0);
	TestEqual(TEXT("Beyond the reach no tree is added"), BeyondReach, NaturalBeyond);

	// The chunks a chunk's trees may be planted in: one each way for a canopy narrower than a chunk.
	TestEqual(TEXT("A canopy narrower than a chunk reaches one chunk"), FVoxelTreeInjector::GetSourceChunkRadius(Templates, ChunkSize), 1);
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
