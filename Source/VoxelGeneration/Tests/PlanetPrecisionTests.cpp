// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SphericalPlanetWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 6, P6.2 (WORLD_MODES_REVIEW_PLAN.md §20.1): precision at the scale of Review_Planet_L (R 1e8 uu,
// 1,000 km), where a float's spacing is 8 uu and a density step 0.59 uu.
//
// WM-BP4 (WorldModes.Planet.PrecisionL): on the CPU the signed distance, the altitude, the surface point and its normal
// are within 0.05 voxel and 0.2 degrees of a double reference at the north pole, 45 degrees, the equator and a cube
// corner. The reference is built from the same surface function (SampleRadialDisplacement); what it checks is how the
// planet assembles R-sized quantities.
// ---------------------------------------------------------------------------

namespace VoxelPlanetPrecision
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

	/**
	 * A configuration with Review_Planet_L's terrain (Scripts/setup_planet_test.py, K 64): R 1e8, the demo's terrain noise
	 * with its base wavelength and height scale 64 times larger and 6 more octaves, a shell of +-762,000. No biomes, caves
	 * or water: the terrain's signed distance alone.
	 */
	UVoxelWorldConfiguration* MakePlanetL(const FVector& Origin = FVector::ZeroVector)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = Origin;
		Config->WorldRadius = 100000000.0f;
		Config->PlanetMaxTerrainHeight = 762000.0f;
		Config->PlanetMaxTerrainDepth = 762000.0f;
		Config->PlanetHeightScale = 384000.0f;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 1337;
		Config->NoiseParams.Frequency = 2.5e-5f / 64.0f;
		Config->NoiseParams.Octaves = 9;
		Config->NoiseParams.Lacunarity = 2.0f;
		Config->NoiseParams.Persistence = 0.5f;
		Config->NoiseParams.Amplitude = 1.0f;
		Config->bEnableBiomes = false;
		Config->BiomeConfiguration = nullptr;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		return Config;
	}

	FVoxelNoiseGenerationRequest MakeRequest(const UVoxelWorldConfiguration& Config)
	{
		FVoxelNoiseGenerationRequest Request;
		Request.LODLevel = 0;
		Request.ChunkSize = Config.ChunkSize;
		Request.VoxelSize = Config.VoxelSize;
		Request.NoiseParams = Config.NoiseParams;
		FVoxelWorldModeFactory::FillGenerationRequest(Config, Request);
		Request.bEnableBiomes = false;
		Request.BiomeConfiguration = nullptr;
		Request.bEnableCaves = false;
		Request.bEnableWaterLevel = false;
		return Request;
	}

	/** The surface point along a direction, every sum in double: centre + direction x (R + altitude). */
	FVector ReferenceSurfaceAt(const FSphericalPlanetWorldMode& Planet, const FVector& Direction, const FVoxelNoiseParams& Noise)
	{
		const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
		const double Altitude = FSphericalPlanetWorldMode::SampleRadialDisplacement(Direction, Params.PlanetRadius,
			Planet.GetTerrainParams(), Noise, &Planet.GetBiomeSnapshot());
		return Params.PlanetCenter + Direction * (Params.PlanetRadius + Altitude);
	}

	/**
	 * The reference normal: the surface's tangent derivatives by a fourth-order central difference (steps h and 2h of arc,
	 * h = SurfaceNormalStep) in double, so it differs from GetSurfacePoint's second-order difference only by truncation.
	 */
	FVector ReferenceNormal(const FSphericalPlanetWorldMode& Planet, const FVector& Dir, const FVoxelNoiseParams& Noise)
	{
		const double R = Planet.GetPlanetParams().PlanetRadius;
		const FVector T1 = FVector::CrossProduct(FMath::Abs(Dir.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector, Dir).GetSafeNormal();
		const FVector T2 = FVector::CrossProduct(Dir, T1);
		const double H = IVoxelWorldMode::SurfaceNormalStep / R;
		auto Derivative = [&](const FVector& T)
		{
			auto At = [&](double K) { return ReferenceSurfaceAt(Planet, (Dir + T * (K * H)).GetSafeNormal(), Noise); };
			return (At(-2.0) - At(-1.0) * 8.0 + At(1.0) * 8.0 - At(2.0)) / 12.0;
		};
		FVector Normal = FVector::CrossProduct(Derivative(T1), Derivative(T2)).GetSafeNormal();
		return FVector::DotProduct(Normal, Dir) < 0.0 ? -Normal : Normal;
	}

	struct FChunkComparison
	{
		int32 NearVoxels = 0;
		int32 MaxDensityDelta = 0;
		double MaxSDFError = 0.0;
		double MaxAltitudeError = 0.0;
		double NormalP95 = 0.0;
		double NormalMax = 0.0;
	};

	/** The CPU-generated chunk holding Position against the double reference (as the precision probe compares it). */
	FChunkComparison CompareChunk(const UVoxelWorldConfiguration& Config, const FSphericalPlanetWorldMode& Planet, const FVector& Position)
	{
		FChunkComparison Out;
		FVoxelNoiseGenerationRequest Request = MakeRequest(Config);
		const int32 N = Request.ChunkSize;
		const float VoxelSize = Request.VoxelSize;
		Request.ChunkCoord = Planet.WorldToChunkCoord(Position, N, VoxelSize);
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

		const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
		const double R = Params.PlanetRadius;
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
			const FVector Rel = P - Params.PlanetCenter;
			const double Dist = Rel.Size();
			const FVector Dir = Rel / Dist;
			const double H = FSphericalPlanetWorldMode::SampleRadialDisplacement(Dir, R, Planet.GetTerrainParams(), Request.NoiseParams, &Planet.GetBiomeSnapshot());
			RefSD[i] = (R + H) - Dist;
			RefDensity[i] = FInfinitePlaneWorldMode::SignedDistanceToDensity(static_cast<float>(RefSD[i]), VoxelSize);
			if (FMath::Abs(RefSD[i]) <= 2.0 * VoxelSize)
			{
				++Out.NearVoxels;
				Out.MaxDensityDelta = FMath::Max(Out.MaxDensityDelta, FMath::Abs(static_cast<int32>(Voxels[i].Density) - static_cast<int32>(RefDensity[i])));
				// The planet's own signed distance and altitude, as generation and every consumer call them.
				const double SDF = FSphericalPlanetWorldMode::ShellSignedDistance(FSphericalPlanetWorldMode::CalculateRadialDistance(P, Params.PlanetCenter), R + H, Params);
				Out.MaxSDFError = FMath::Max(Out.MaxSDFError, FMath::Abs(SDF - RefSD[i]));
				Out.MaxAltitudeError = FMath::Max(Out.MaxAltitudeError, FMath::Abs(static_cast<double>(Planet.GetAltitude(P)) - (Dist - R)));
			}
		}

		// Density-gradient normals (what the mesher sees) against the reference density's.
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
			Out.NormalP95 = Angles[FMath::Clamp(FMath::RoundToInt32(0.95 * (Angles.Num() - 1)), 0, Angles.Num() - 1)];
			Out.NormalMax = Angles.Last();
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetPrecisionLTest,
	"VoxelWorlds.WorldModes.Planet.PrecisionL",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetPrecisionLTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetPrecision;

	// Off the origin too: the planet centre is WorldOrigin, and everything is relative to it.
	for (const FVector& Origin : { FVector::ZeroVector, FVector(3.0e7, -2.0e7, 1.0e7) })
	{
		UVoxelWorldConfiguration* Config = MakePlanetL(Origin);
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		const FSphericalPlanetParams& Params = Planet.GetPlanetParams();
		const double Voxel = Config->VoxelSize;
		const double Step = 2.0 * Voxel / 255.0;
		const double Tolerance = 0.05 * Voxel;

		TestEqual(TEXT("L: the radius is exact"), Params.PlanetRadius, 1.0e8);

		for (const FSite& Site : Sites)
		{
			const FVector Dir = LatLon(Site.Lat, Site.Lon);
			const FString Where = FString::Printf(TEXT("%s (origin %s)"), Site.Name, *Origin.ToCompactString());

			// The surface point and its altitude against centre + direction x (R + h) in double.
			const FVoxelTerrainSurfacePoint Surface = Planet.GetSurfacePoint(Params.PlanetCenter + Dir * Params.PlanetRadius, Config->NoiseParams);
			const FVector RefPoint = ReferenceSurfaceAt(Planet, Dir, Config->NoiseParams);
			const double RefAltitude = (RefPoint - Params.PlanetCenter).Size() - Params.PlanetRadius;
			const double PointError = FVector::Dist(Surface.Position, RefPoint);
			const double AltitudeError = FMath::Abs(static_cast<double>(Surface.Altitude) - RefAltitude);
			const double NormalError = AngleDeg(Surface.Normal, ReferenceNormal(Planet, Dir, Config->NoiseParams));

			// The surface chunk generated on the CPU against the double reference.
			const FChunkComparison Chunk = CompareChunk(*Config, Planet, Surface.Position);

			AddInfo(FString::Printf(TEXT("%s: surface altitude %.0f; point off %.4f uu, altitude off %.4f uu, normal off %.4f deg; ")
				TEXT("%d voxels near the surface: density off by up to %d steps (%.3f uu), signed distance off by up to %.4f uu, altitude by %.4f uu; ")
				TEXT("density-gradient normals off %.3f deg P95, %.3f max"),
				*Where, RefAltitude, PointError, AltitudeError, NormalError, Chunk.NearVoxels, Chunk.MaxDensityDelta, Chunk.MaxDensityDelta * Step,
				Chunk.MaxSDFError, Chunk.MaxAltitudeError, Chunk.NormalP95, Chunk.NormalMax));

			TestTrue(*FString::Printf(TEXT("%s: the surface chunk generated"), *Where), Chunk.NearVoxels > 0);
			TestTrue(*FString::Printf(TEXT("%s: surface point within 0.05 voxel (%.4f uu)"), *Where, PointError), PointError <= Tolerance);
			TestTrue(*FString::Printf(TEXT("%s: surface altitude within 0.05 voxel (%.4f uu)"), *Where, AltitudeError), AltitudeError <= Tolerance);
			TestTrue(*FString::Printf(TEXT("%s: surface normal within 0.2 deg (%.4f)"), *Where, NormalError), NormalError <= 0.2);
			TestTrue(*FString::Printf(TEXT("%s: generated density within 0.05 voxel (%d steps)"), *Where, Chunk.MaxDensityDelta), Chunk.MaxDensityDelta * Step <= Tolerance);
			TestTrue(*FString::Printf(TEXT("%s: signed distance within 0.05 voxel (%.4f uu)"), *Where, Chunk.MaxSDFError), Chunk.MaxSDFError <= Tolerance);
			TestTrue(*FString::Printf(TEXT("%s: voxel altitude within 0.05 voxel (%.4f uu)"), *Where, Chunk.MaxAltitudeError), Chunk.MaxAltitudeError <= Tolerance);
			TestTrue(*FString::Printf(TEXT("%s: density-gradient normals within 0.2 deg at P95 (%.3f)"), *Where, Chunk.NormalP95), Chunk.NormalP95 <= 0.2);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
