// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SphericalPlanetWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelNoiseTypes.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelData.h"
#include "VoxelRenderRegions.h"

#include <cmath>

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 6, WM-BP1 (WORLD_MODES_REVIEW_PLAN.md §20): the precision probe behind the baseline survey of the big
// test planets. For each of Review_Planet, Review_Planet_M and Review_Planet_L (when the assets exist) and four sites (the
// north pole, 45 degrees, the equator and a cube corner) it generates the chunk that holds the surface on the CPU, as the
// chunk manager would (caves off, so only the terrain's signed distance shapes the density), and compares it with a
// double-precision reference built from the same surface function:
//
// - density: the voxels within two voxels of the surface whose density differs from the reference's, and by how much
//   (a step is 2 * VoxelSize / 255 uu);
// - the signed-distance error at the surface against the double, in uu: Phase 5's float formula
//   (float(R + h) - float(|P - C|)) and the one generation uses since WM-BP4 (ShellSignedDistanceAtAltitude, double);
// - normals: the angle between the density gradient the mesher sees and the reference density's, at the voxels within a
//   voxel of the surface;
// - vertices: how far a surface point moves when stored as an absolute FVector3f (the scene proxy's offsets up to Phase
//   5) and as a float relative to its render region's origin (WM-BP7, FVoxelRenderRegions with RegionChunks 16);
// - noise: the float spacing of the terrain noise's top octave and of a 100 uu feature at absolute positions (caves,
//   ore), as a fraction of a feature, and how often each field's 256-unit period repeated around the equator (the
//   permutation noise up to Phase 5; planets sample a hashed lattice with no period since WM-BP5).
//
// It reports and does not assert (the thresholds are WM-BP4's WorldModes.Planet.PrecisionL and WM-BP7's
// Rendering.Planet.RegionPrecision); the figures go into the plan's §20.2 baseline and §20.3.
// ---------------------------------------------------------------------------

namespace VoxelPlanetPrecisionProbe
{
	struct FSite
	{
		const TCHAR* Name;
		double Lat;
		double Lon;
	};

	const FSite Sites[] = {
		{ TEXT("north pole"), 90.0, 0.0 },
		{ TEXT("latitude 45"), 45.0, 20.0 },
		{ TEXT("equator"), 0.0, 100.0 },
		{ TEXT("cube corner"), 35.264, 45.0 },
	};

	FVector LatLon(double Lat, double Lon)
	{
		const double La = FMath::DegreesToRadians(Lat);
		const double Lo = FMath::DegreesToRadians(Lon);
		return FVector(FMath::Cos(La) * FMath::Cos(Lo), FMath::Cos(La) * FMath::Sin(Lo), FMath::Sin(La));
	}

	double AngleDeg(const FVector& A, const FVector& B)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A.GetSafeNormal(), B.GetSafeNormal()), -1.0, 1.0)));
	}

	/** Float spacing at a magnitude (one unit in the last place). */
	double Ulp(double Magnitude)
	{
		const float F = static_cast<float>(FMath::Abs(Magnitude));
		return static_cast<double>(FMath::Max(std::nextafter(F, TNumericLimits<float>::Max()) - F, TNumericLimits<float>::Min()));
	}

	struct FSiteResult
	{
		double SurfaceAltitude = 0.0;
		int32 NearVoxels = 0;
		int32 DensityMismatches = 0;
		int32 MaxDensityDelta = 0;
		double FloatSDFErrorMax = 0.0;
		double ShippedSDFErrorMax = 0.0;
		double NormalErrorP95 = 0.0;
		double NormalErrorMax = 0.0;
		double VertexError = 0.0;
		double RegionVertexError = 0.0;
	};

	FSiteResult ProbeSite(const UVoxelWorldConfiguration& Config, const FVector& Direction)
	{
		FSiteResult Out;
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(Config);
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		const FVector Center = Config.WorldOrigin;
		const double R = Config.WorldRadius;
		const int32 N = Config.ChunkSize;
		const float VoxelSize = Config.VoxelSize;

		FVoxelNoiseGenerationRequest Request;
		Request.LODLevel = 0;
		Request.ChunkSize = N;
		Request.VoxelSize = VoxelSize;
		Request.NoiseParams = Config.NoiseParams;
		FVoxelWorldModeFactory::FillGenerationRequest(Config, Request);
		Request.bEnableBiomes = Config.bEnableBiomes;
		Request.BiomeConfiguration = Config.BiomeConfiguration;
		Request.bEnableCaves = false; // the terrain's signed distance only
		Request.bEnableWaterLevel = Config.bEnableWaterLevel;
		Request.WaterLevel = Config.WaterLevel;
		Request.WaterRadius = Config.WaterRadius;

		const FVoxelTerrainSurfacePoint Surface = Planet.GetSurfacePoint(Center + Direction * R, Request.NoiseParams);
		Out.SurfaceAltitude = Surface.Altitude;
		Out.VertexError = FVector::Dist(FVector(FVector3f(Surface.Position)), Surface.Position);
		FVoxelRenderRegions Regions;
		Regions.RegionChunks = 16;
		Regions.WorldOrigin = Config.WorldOrigin;
		Regions.ChunkWorldSize = static_cast<double>(Config.ChunkSize) * Config.VoxelSize;
		const FIntVector Region = Regions.GetRegionAt(Surface.Position);
		Out.RegionVertexError = FVector::Dist(Regions.GetRegionOrigin(Region) + FVector(Regions.ToRegionLocal(Surface.Position, Region)), Surface.Position);
		Request.ChunkCoord = Planet.WorldToChunkCoord(Surface.Position, N, VoxelSize);
		const FVector ChunkWorldPos = Request.GetChunkWorldPosition();

		FVoxelCPUNoiseGenerator Generator;
		Generator.Initialize();
		TArray<FVoxelData> Voxels;
		Generator.GenerateChunkCPU(Request, Voxels);
		Generator.Shutdown();
		if (Voxels.Num() != N * N * N)
		{
			return Out;
		}

		// The double reference: the same surface function, the distance and the sum in double.
		const FWorldModeTerrainParams& Terrain = Planet.GetTerrainParams();
		const FVoxelBiomeSnapshot& Snapshot = Planet.GetBiomeSnapshot();
		TArray<double> RefSD;
		TArray<uint8> RefDensity;
		RefSD.SetNumUninitialized(Voxels.Num());
		RefDensity.SetNumUninitialized(Voxels.Num());
		for (int32 Z = 0; Z < N; ++Z)
		for (int32 Y = 0; Y < N; ++Y)
		for (int32 X = 0; X < N; ++X)
		{
			const int32 i = X + Y * N + Z * N * N;
			const FVector P = ChunkWorldPos + FVector(X, Y, Z) * VoxelSize;
			const FVector Rel = P - Center;
			const double Dist = Rel.Size();
			const FVector Dir = Rel / Dist;
			const double H = FSphericalPlanetWorldMode::SampleRadialDisplacement(Dir, Config.WorldRadius, Terrain, Request.NoiseParams, &Snapshot);
			RefSD[i] = (R + H) - Dist;
			RefDensity[i] = FInfinitePlaneWorldMode::SignedDistanceToDensity(static_cast<float>(RefSD[i]), VoxelSize);
			if (FMath::Abs(RefSD[i]) <= 2.0 * VoxelSize)
			{
				++Out.NearVoxels;
				const int32 Delta = FMath::Abs(static_cast<int32>(Voxels[i].Density) - static_cast<int32>(RefDensity[i]));
				Out.DensityMismatches += Delta > 0 ? 1 : 0;
				Out.MaxDensityDelta = FMath::Max(Out.MaxDensityDelta, Delta);
				// The float formula generation used up to Phase 5: float(R + h) - float(|P - C|).
				const float FloatSD = (static_cast<float>(R) + static_cast<float>(H)) - static_cast<float>(Dist);
				Out.FloatSDFErrorMax = FMath::Max(Out.FloatSDFErrorMax, FMath::Abs(static_cast<double>(FloatSD) - RefSD[i]));
				// The form generation uses since WM-BP4: the terrain's altitude minus the voxel's, both formed in double.
				const float ShippedSD = FSphericalPlanetWorldMode::ShellSignedDistanceAtAltitude(Dist - R, H, Planet.GetPlanetParams());
				Out.ShippedSDFErrorMax = FMath::Max(Out.ShippedSDFErrorMax, FMath::Abs(static_cast<double>(ShippedSD) - RefSD[i]));
			}
		}

		// Normals: the generated density's gradient against the reference density's, a voxel either side.
		TArray<double> Angles;
		for (int32 Z = 1; Z < N - 1; ++Z)
		for (int32 Y = 1; Y < N - 1; ++Y)
		for (int32 X = 1; X < N - 1; ++X)
		{
			const int32 i = X + Y * N + Z * N * N;
			if (FMath::Abs(RefSD[i]) > VoxelSize)
			{
				continue;
			}
			auto Gradient = [&](auto Get)
			{
				return FVector(Get(i + 1) - Get(i - 1), Get(i + N) - Get(i - N), Get(i + N * N) - Get(i - N * N));
			};
			const FVector Generated = Gradient([&](int32 j) { return static_cast<double>(Voxels[j].Density); });
			const FVector Reference = Gradient([&](int32 j) { return static_cast<double>(RefDensity[j]); });
			if (!Generated.IsNearlyZero() && !Reference.IsNearlyZero())
			{
				Angles.Add(AngleDeg(Generated, Reference));
			}
		}
		Angles.Sort();
		if (Angles.Num() > 0)
		{
			Out.NormalErrorP95 = Angles[FMath::Clamp(FMath::RoundToInt32(0.95 * (Angles.Num() - 1)), 0, Angles.Num() - 1)];
			Out.NormalErrorMax = Angles.Last();
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetPrecisionProbeTest,
	"VoxelWorlds.WorldModes.Planet.PrecisionProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetPrecisionProbeTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetPrecisionProbe;
	const TCHAR* Assets[] = {
		TEXT("/Game/PluginTesting/Review/Review_Planet.Review_Planet"),
		TEXT("/Game/PluginTesting/Review/Review_Planet_M.Review_Planet_M"),
		TEXT("/Game/PluginTesting/Review/Review_Planet_L.Review_Planet_L"),
	};
	int32 Probed = 0;
	for (const TCHAR* Asset : Assets)
	{
		const UVoxelWorldConfiguration* Config = LoadObject<UVoxelWorldConfiguration>(nullptr, Asset);
		if (!Config || Config->WorldMode != EWorldMode::SphericalPlanet)
		{
			AddInfo(FString::Printf(TEXT("%s: not found or not a planet, skipped"), Asset));
			continue;
		}
		++Probed;
		const double R = Config->WorldRadius;
		const double Step = 2.0 * Config->VoxelSize / 255.0;
		// Noise: the terrain's top octave at the datum, a 100 uu feature at an absolute position, and the 256-unit period.
		const FVoxelNoiseParams& Noise = Config->NoiseParams;
		const double TopFrequency = Noise.Frequency * FMath::Pow(static_cast<double>(Noise.Lacunarity), FMath::Max(0, Noise.Octaves - 1));
		const double Circumference = 2.0 * UE_DOUBLE_PI * R;
		const UVoxelBiomeConfiguration* Biomes = Config->BiomeConfiguration;
		const double ContinentFrequency = Biomes && Biomes->bEnableContinentalness ? Biomes->ContinentalnessNoiseFrequency : 0.0;
		const double ClimateFrequency = Biomes ? Biomes->TemperatureNoiseFrequency : 0.0;
		AddInfo(FString::Printf(TEXT("%s: R %.0f, voxel %.0f (density step %.3f uu), noise %.3g x %d octaves (top %.3g): top octave spacing %.2e of a feature, ")
			TEXT("a 100 uu feature at |P| = R spaced %.3f of itself; the permutation noise's 256-unit period (planets up to Phase 5, hashed since WM-BP5) repeats around the equator: terrain %.1f (top octave %.0f), continents %.2f, climate %.2f"),
			*Config->GetName(), R, Config->VoxelSize, Step, Noise.Frequency, Noise.Octaves, TopFrequency, Ulp(R * TopFrequency), Ulp(R) / 100.0,
			Circumference * Noise.Frequency / 256.0, Circumference * TopFrequency / 256.0, Circumference * ContinentFrequency / 256.0,
			Circumference * ClimateFrequency / 256.0));
		for (const FSite& Site : Sites)
		{
			const FSiteResult Result = ProbeSite(*Config, LatLon(Site.Lat, Site.Lon));
			AddInfo(FString::Printf(TEXT("%s %s: surface altitude %.0f; %d voxels within 2 of the surface, %d with another density (max %d steps = %.2f uu); ")
				TEXT("SDF error up to %.2f uu (%.1f steps) with Phase 5's float formula, %.4f uu as generated; density-gradient normals off by %.2f deg P95, %.2f max; ")
				TEXT("a vertex moves %.2f uu stored as an absolute FVector3f, %.4f uu relative to its render region"),
				*Config->GetName(), Site.Name, Result.SurfaceAltitude, Result.NearVoxels, Result.DensityMismatches, Result.MaxDensityDelta,
				Result.MaxDensityDelta * Step, Result.FloatSDFErrorMax, Result.FloatSDFErrorMax / Step, Result.ShippedSDFErrorMax,
				Result.NormalErrorP95, Result.NormalErrorMax, Result.VertexError, Result.RegionVertexError));
			TestTrue(*FString::Printf(TEXT("%s %s: the surface chunk generated"), *Config->GetName(), Site.Name), Result.NearVoxels > 0);
		}
	}
	if (Probed == 0)
	{
		AddWarning(TEXT("No planet test configurations found (Scripts/setup_planet_test.py makes them)"));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
