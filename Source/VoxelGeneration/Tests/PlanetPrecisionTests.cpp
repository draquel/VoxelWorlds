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
#include "VoxelPlanetNoise.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 6, P6.2 (WORLD_MODES_REVIEW_PLAN.md §20.1): precision at the scale of Review_Planet_L (R 1e8 uu,
// 1,000 km), where a float's spacing is 8 uu and a density step 0.59 uu.
//
// WM-BP4 (WorldModes.Planet.PrecisionL): on the CPU the signed distance, the altitude, the surface point and its normal
// are within 0.05 voxel and 0.2 degrees of a double reference at the north pole, 45 degrees, the equator and a cube
// corner. The reference is built from the same surface function (SampleRadialDisplacement); what it checks is how the
// planet assembles R-sized quantities.
//
// WM-BP5 (Noise.SplitOrigin, Noise.PlanetHashNoPeriod): the planet noise (FVoxelPlanetNoise, D22) equals a double
// reference to 1e-5 at origins up to 1e9 noise units, in its per-point form (the CPU) and its origin + offset form (the
// GPU's algebra), and its hashed lattice repeats nowhere within 1e6 noise units, where the heightmap noise's 256-entry
// table repeats every 256.
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNoiseSplitOriginTest,
	"VoxelWorlds.Noise.SplitOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNoiseSplitOriginTest::RunTest(const FString& Parameters)
{
	FRandomStream Random(0x5EED);
	auto RandomDirection = [&Random]() { return FVector(Random.FRandRange(-1.0, 1.0), Random.FRandRange(-1.0, 1.0), Random.FRandRange(-1.0, 1.0)).GetSafeNormal(); };
	const double Magnitudes[] = { 0.0, 1.0e3, 1.0e5, 1.0e7, 1.0e8, 1.0e9 };
	const EVoxelNoiseType Types[] = { EVoxelNoiseType::Simplex, EVoxelNoiseType::Perlin, EVoxelNoiseType::Cellular, EVoxelNoiseType::Voronoi };
	const TCHAR* TypeNames[] = { TEXT("Simplex"), TEXT("Perlin"), TEXT("Cellular"), TEXT("Voronoi") };

	// One octave: the split origin + an offset of up to 8 noise units (a chunk at the planet's highest field frequencies)
	// against the double reference at their sum.
	for (int32 t = 0; t < 4; ++t)
	{
		for (const double Magnitude : Magnitudes)
		{
			double MaxError = 0.0;
			for (int32 i = 0; i < 500; ++i)
			{
				const FVector Origin = RandomDirection() * Magnitude + FVector(Random.FRand(), Random.FRand(), Random.FRand()) * 3.0;
				const FVector3f Offset(Random.FRandRange(-8.0f, 8.0f), Random.FRandRange(-8.0f, 8.0f), Random.FRandRange(-8.0f, 8.0f));
				const float Split = FVoxelPlanetNoise::Sample(FVoxelPlanetNoise::Split(Origin, Types[t]), Offset, Types[t], 1337);
				const double Reference = FVoxelPlanetNoise::ReferenceSample(Origin + FVector(Offset), Types[t], 1337);
				MaxError = FMath::Max(MaxError, FMath::Abs(static_cast<double>(Split) - Reference));
			}
			AddInfo(FString::Printf(TEXT("%s, origin at %.0e noise units: split + offset off the double reference by up to %.2e"), TypeNames[t], Magnitude, MaxError));
			TestTrue(*FString::Printf(TEXT("%s at %.0e: within 1e-5 (%.2e)"), TypeNames[t], Magnitude, MaxError), MaxError <= 1.0e-5);
		}
	}

	// fBm as the planet samples it: Review_Planet_L's terrain (3.9e-7 x 9 octaves) and caves (0.002 x 3) at the radii of the
	// small planet, M and L, per point (the CPU) and as a chunk anchor + the voxel's offset in uu (the GPU's algebra).
	FVoxelNoiseParams Terrain;
	Terrain.NoiseType = EVoxelNoiseType::Simplex;
	Terrain.Seed = 1337;
	Terrain.Frequency = 2.5e-5f / 64.0f;
	Terrain.Octaves = 9;
	Terrain.Lacunarity = 2.0f;
	Terrain.Persistence = 0.5f;
	Terrain.Amplitude = 1.0f;
	FVoxelNoiseParams Caves = Terrain;
	Caves.Frequency = 0.002f;
	Caves.Octaves = 3;
	const TPair<const TCHAR*, FVoxelNoiseParams> Fields[] = { { TEXT("terrain"), Terrain }, { TEXT("caves"), Caves } };
	for (const TPair<const TCHAR*, FVoxelNoiseParams>& Field : Fields)
	{
		for (const double Radius : { 2.5e5, 5.0e6, 1.0e8 })
		{
			double MaxPointError = 0.0;
			double MaxOffsetError = 0.0;
			for (int32 i = 0; i < 300; ++i)
			{
				const FVector Anchor = RandomDirection() * Radius;
				const FVector3f Offset(Random.FRandRange(-2400.0f, 2400.0f), Random.FRandRange(-2400.0f, 2400.0f), Random.FRandRange(-2400.0f, 2400.0f));
				const FVector Point = Anchor + FVector(Offset);
				const double Reference = FVoxelPlanetNoise::ReferenceFBM3D(Point, Field.Value);
				MaxPointError = FMath::Max(MaxPointError, FMath::Abs(FVoxelPlanetNoise::FBM3D(Point, Field.Value) - Reference));
				MaxOffsetError = FMath::Max(MaxOffsetError, FMath::Abs(FVoxelPlanetNoise::FBM3D(Anchor, Offset, Field.Value) - Reference));
			}
			AddInfo(FString::Printf(TEXT("fBm %s at R %.1e: per point off by up to %.2e, anchor + offset by %.2e"), Field.Key, Radius, MaxPointError, MaxOffsetError));
			TestTrue(*FString::Printf(TEXT("fBm %s at R %.1e per point: within 1e-5 (%.2e)"), Field.Key, Radius, MaxPointError), MaxPointError <= 1.0e-5);
			TestTrue(*FString::Printf(TEXT("fBm %s at R %.1e anchor + offset: within 1e-5 (%.2e)"), Field.Key, Radius, MaxOffsetError), MaxOffsetError <= 1.0e-5);
		}
	}

	// For comparison: a float position at 1e8 uu is 8 uu coarse, which the old path fed to the terrain's top octave.
	AddInfo(FString::Printf(TEXT("For comparison: a float position at 1e8 uu is 8 uu coarse, %.1e noise units in the terrain's top octave"),
		8.0 * static_cast<double>(Terrain.Frequency) * 256.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNoisePlanetHashNoPeriodTest,
	"VoxelWorlds.Noise.PlanetHashNoPeriod",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNoisePlanetHashNoPeriodTest::RunTest(const FString& Parameters)
{
	FRandomStream Random(0xC0FFEE);
	const FVector Axes[] = { FVector(1, 0, 0), FVector(0, 1, 0), FVector(0, 0, 1), FVector(1, 1, 1) };
	const EVoxelNoiseType Types[] = { EVoxelNoiseType::Simplex, EVoxelNoiseType::Perlin, EVoxelNoiseType::Cellular };
	const TCHAR* TypeNames[] = { TEXT("Simplex"), TEXT("Perlin"), TEXT("Cellular") };

	// The control: the heightmap noise's table repeats Perlin every 256 lattice units along each axis.
	{
		double MeanDelta = 0.0;
		for (int32 i = 0; i < 256; ++i)
		{
			const FVector P(Random.FRandRange(0.0, 1000.0), Random.FRandRange(0.0, 1000.0), Random.FRandRange(0.0, 1000.0));
			MeanDelta += FMath::Abs(FVoxelCPUNoiseGenerator::Perlin3D(P + FVector(256, 0, 0), 7) - FVoxelCPUNoiseGenerator::Perlin3D(P, 7)) / 256.0;
		}
		AddInfo(FString::Printf(TEXT("Control: the heightmap Perlin 256 units apart along X differs by %.2e on average"), MeanDelta));
		TestTrue(TEXT("Control: the heightmap noise repeats every 256 units"), MeanDelta < 1.0e-3);
	}

	// Every shift of k x 256 units (k = 1 .. 3906, up to 1e6) along each axis and the diagonal, at 32 random points: the
	// planet noise there is as unrelated as two random samples (mean |difference| about 0.3 for Simplex; a repeat gives 0).
	for (int32 t = 0; t < 3; ++t)
	{
		double WorstMean = TNumericLimits<double>::Max();
		double WorstShift = 0.0;
		for (const FVector& Axis : Axes)
		{
			for (int32 k = 1; k <= 3906; ++k)
			{
				const double Shift = 256.0 * k;
				double Mean = 0.0;
				for (int32 i = 0; i < 32; ++i)
				{
					const FVector P(Random.FRandRange(0.0, 1.0e6), Random.FRandRange(0.0, 1.0e6), Random.FRandRange(0.0, 1.0e6));
					const float A = FVoxelPlanetNoise::Sample(FVoxelPlanetNoise::Split(P, Types[t]), FVector3f::ZeroVector, Types[t], 7);
					const float B = FVoxelPlanetNoise::Sample(FVoxelPlanetNoise::Split(P + Axis * Shift, Types[t]), FVector3f::ZeroVector, Types[t], 7);
					Mean += FMath::Abs(A - B) / 32.0;
				}
				if (Mean < WorstMean)
				{
					WorstMean = Mean;
					WorstShift = Shift;
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s: the most alike shift of k x 256 up to 1e6 (%.0f) still differs by %.3f on average"), TypeNames[t], WorstShift, WorstMean));
		TestTrue(*FString::Printf(TEXT("%s repeats nowhere within 1e6 noise units (worst mean difference %.3f)"), TypeNames[t], WorstMean), WorstMean > 0.05);
	}

	// The value range: about [-1, 1], centred.
	double Sum = 0.0, SumSq = 0.0, MinV = 1.0, MaxV = -1.0;
	const int32 Count = 20000;
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector P(Random.FRandRange(-1.0e9, 1.0e9), Random.FRandRange(-1.0e9, 1.0e9), Random.FRandRange(-1.0e9, 1.0e9));
		const double V = FVoxelPlanetNoise::Sample(FVoxelPlanetNoise::Split(P, EVoxelNoiseType::Simplex), FVector3f::ZeroVector, EVoxelNoiseType::Simplex, 3);
		Sum += V;
		SumSq += V * V;
		MinV = FMath::Min(MinV, V);
		MaxV = FMath::Max(MaxV, V);
	}
	const double Mean = Sum / Count;
	const double Std = FMath::Sqrt(FMath::Max(0.0, SumSq / Count - Mean * Mean));
	AddInfo(FString::Printf(TEXT("Simplex over +-1e9: mean %.3f, standard deviation %.3f, range [%.3f, %.3f]"), Mean, Std, MinV, MaxV));
	TestTrue(TEXT("Simplex centred"), FMath::Abs(Mean) < 0.02);
	TestTrue(TEXT("Simplex in [-1, 1]"), MinV >= -1.0 && MaxV <= 1.0);
	TestTrue(TEXT("Simplex spread like the heightmap noise (std 0.15 - 0.45)"), Std > 0.15 && Std < 0.45);
	return true;
}


// ---------------------------------------------------------------------------
// G43 (WORLD_MODES_REVIEW_PLAN.md §14.1): the planet Simplex noise is continuous across the simplex lattice's
// boundaries. The classic corner kernel (0.6 - d^2)^4 reaches past the simplex, so a corner's contribution dropped out
// where a sample crossed into the next one: steps on the boundary planes, every octave's at once on x = y, y = z and
// x = z, the cube sphere's face edges (P6.4 measured up to 136 uu on Review_Planet_L across them). The continuous kernel
// is that one less its value and slope at d^2 = 0.5 (voxel.Planet.ContinuousSimplex 1, PlanetGenerationVersion 6).
//
// - Noise.PlanetSimplexContinuous: on random points of every kind of boundary (the corner order's planes x - y = k,
//   y - z = k, x - z = k, k = 0 among them, and the skewed cells' faces), the noise's step across the plane and the
//   change of its slope there, in the double reference and in the float split-origin form generation uses; and the new
//   noise's spread and shape against the classic one.
// - WorldModes.Planet.FaceEdgeContinuity: the terrain's altitude either side of each of the cube sphere's 12 face edges,
//   directions 2e-9 rad apart: the terrain noise alone on the small planet and on Review_Planet_L's, and the review planets
//   as configured when their assets are there (logging where the largest classic step on land lies). No step: under a
//   tolerance, or no more than a control of random directions off the edges probed the same way (the terrain's own change
//   and its float resolution, which on the configured big planets is the continentalness field's, a few tenths of a uu).
// Each is checked with the classic kernel too, which must step (non-vacuous).
// ---------------------------------------------------------------------------

namespace VoxelPlanetSimplexContinuity
{
	/** Sets voxel.Planet.ContinuousSimplex for a scope. */
	struct FScopedKernel
	{
		IConsoleVariable* Var = nullptr;
		int32 Previous = 1;

		explicit FScopedKernel(int32 Value)
			: Var(IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Planet.ContinuousSimplex")))
		{
			if (Var)
			{
				Previous = Var->GetInt();
				Var->Set(Value, ECVF_SetByConsole);
			}
		}

		~FScopedKernel()
		{
			if (Var)
			{
				Var->Set(Previous, ECVF_SetByConsole);
			}
		}
	};

	/** A point on one of the Simplex lattice's boundaries (noise space) and the boundary's unit normal. */
	struct FBoundaryPoint
	{
		FVector P;
		FVector N;
	};

	/**
	 * A random boundary point. Kinds 0-2: the corner order's planes x - y = k, y - z = k, x - z = k (the unskewed offsets
	 * in a cell compare equal there); kinds 3-5: the skewed cells' faces x + s = n, y + s = n, z + s = n with
	 * s = (x + y + z) / 3. k and n are integers; bThroughOrigin takes k = 0, the planes every octave shares.
	 */
	FBoundaryPoint RandomBoundaryPoint(FRandomStream& Random, int32 Kind, double Extent, bool bThroughOrigin)
	{
		const double A = Random.FRandRange(-Extent, Extent);
		const double B = Random.FRandRange(-Extent, Extent);
		const double K = bThroughOrigin ? 0.0 : static_cast<double>(FMath::RoundToInt64(Random.FRandRange(-Extent, Extent)));
		switch (Kind)
		{
		case 0: return { FVector(A + K, A, B), FVector(1, -1, 0).GetSafeNormal() };
		case 1: return { FVector(B, A + K, A), FVector(0, 1, -1).GetSafeNormal() };
		case 2: return { FVector(A + K, B, A), FVector(1, 0, -1).GetSafeNormal() };
		// 4x + y + z = 3n, with y = A, z = B
		case 3: return { FVector((3.0 * K - A - B) / 4.0, A, B), FVector(4, 1, 1).GetSafeNormal() };
		case 4: return { FVector(A, (3.0 * K - A - B) / 4.0, B), FVector(1, 4, 1).GetSafeNormal() };
		default: return { FVector(A, B, (3.0 * K - A - B) / 4.0), FVector(1, 1, 4).GetSafeNormal() };
		}
	}

	struct FStepStats
	{
		double MaxStep = 0.0;       // |f(P + eN) - f(P - eN)|, double reference
		double MaxSlopeJump = 0.0;  // |slope on the + side - slope on the - side|, double reference
		double MaxFloatStep = 0.0;  // the step in the float split-origin form (FVoxelPlanetNoise::Simplex)
	};

	/** The noise's behaviour across Count random boundary points of each kind. */
	FStepStats MeasureBoundaries(int32 Count, bool bThroughOrigin, int32 Seed)
	{
		FRandomStream Random(bThroughOrigin ? 4301 : 4302);
		FStepStats Out;
		constexpr double StepEps = 1.0e-7;
		constexpr double SlopeEps = 1.0e-6;
		constexpr float FloatEps = 1.0e-5f;
		for (int32 Kind = 0; Kind < 6; ++Kind)
		{
			for (int32 i = 0; i < Count; ++i)
			{
				const FBoundaryPoint B = RandomBoundaryPoint(Random, Kind, 1000.0, bThroughOrigin);
				auto F = [&](double T) { return FVoxelPlanetNoise::ReferenceSample(B.P + B.N * T, EVoxelNoiseType::Simplex, Seed); };
				Out.MaxStep = FMath::Max(Out.MaxStep, FMath::Abs(F(StepEps) - F(-StepEps)));
				const double SlopePlus = (F(2.0 * SlopeEps) - F(SlopeEps)) / SlopeEps;
				const double SlopeMinus = (F(-SlopeEps) - F(-2.0 * SlopeEps)) / SlopeEps;
				Out.MaxSlopeJump = FMath::Max(Out.MaxSlopeJump, FMath::Abs(SlopePlus - SlopeMinus));
				const FVoxelNoiseSplit Split = FVoxelPlanetNoise::Split(B.P, EVoxelNoiseType::Simplex);
				const FVector3f Offset = FVector3f(B.N) * FloatEps;
				Out.MaxFloatStep = FMath::Max(Out.MaxFloatStep, static_cast<double>(FMath::Abs(
					FVoxelPlanetNoise::Simplex(Split, Offset, Seed) - FVoxelPlanetNoise::Simplex(Split, -Offset, Seed))));
			}
		}
		return Out;
	}

	/** Review_Planet's terrain (Scripts/setup_planet_test.py): R 250,000, the demo's noise, a shell of +-6,500. */
	UVoxelWorldConfiguration* MakePlanetS()
	{
		UVoxelWorldConfiguration* Config = VoxelPlanetPrecision::MakePlanetL();
		Config->WorldRadius = 250000.0f;
		Config->PlanetMaxTerrainHeight = 6500.0f;
		Config->PlanetMaxTerrainDepth = 6500.0f;
		Config->PlanetHeightScale = 6000.0f;
		Config->NoiseParams.Frequency = 2.5e-5f;
		Config->NoiseParams.Octaves = 3;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelNoisePlanetSimplexContinuousTest,
	"VoxelWorlds.Noise.PlanetSimplexContinuous",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelNoisePlanetSimplexContinuousTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetSimplexContinuity;

	FStepStats Stats[2][2]; // [kernel: classic, continuous][boundaries: any, through the origin]
	for (int32 Kernel = 0; Kernel < 2; ++Kernel)
	{
		FScopedKernel Scoped(Kernel);
		for (int32 Origin = 0; Origin < 2; ++Origin)
		{
			Stats[Kernel][Origin] = MeasureBoundaries(3000, Origin == 1, 1337);
			AddInfo(FString::Printf(TEXT("%s kernel, %s: step across the boundary up to %.2e (float form %.2e), slope change up to %.2e"),
				Kernel == 1 ? TEXT("Continuous") : TEXT("Classic"), Origin == 1 ? TEXT("the planes through the origin") : TEXT("every kind of boundary"),
				Stats[Kernel][Origin].MaxStep, Stats[Kernel][Origin].MaxFloatStep, Stats[Kernel][Origin].MaxSlopeJump));
		}
	}
	for (int32 Origin = 0; Origin < 2; ++Origin)
	{
		const TCHAR* Where = Origin == 1 ? TEXT("planes through the origin") : TEXT("every boundary");
		TestTrue(*FString::Printf(TEXT("Classic kernel steps on %s (non-vacuous, %.2e)"), Where, Stats[0][Origin].MaxStep), Stats[0][Origin].MaxStep > 1.0e-3);
		TestTrue(*FString::Printf(TEXT("Continuous kernel: no step on %s (%.2e)"), Where, Stats[1][Origin].MaxStep), Stats[1][Origin].MaxStep < 1.0e-5);
		TestTrue(*FString::Printf(TEXT("Continuous kernel: no step in the float form on %s (%.2e)"), Where, Stats[1][Origin].MaxFloatStep), Stats[1][Origin].MaxFloatStep < 1.0e-3);
		TestTrue(*FString::Printf(TEXT("Continuous kernel: the slope is continuous on %s (%.2e)"), Where, Stats[1][Origin].MaxSlopeJump), Stats[1][Origin].MaxSlopeJump < 1.0e-2);
	}

	// The same landscape: the spread kept, the shape within a few percent.
	FRandomStream Random(4303);
	const int32 Count = 40000;
	TArray<FVector> Points;
	for (int32 i = 0; i < Count; ++i)
	{
		Points.Add(FVector(Random.FRandRange(-1.0e6, 1.0e6), Random.FRandRange(-1.0e6, 1.0e6), Random.FRandRange(-1.0e6, 1.0e6)));
	}
	TArray<double> Values[2];
	for (int32 Kernel = 0; Kernel < 2; ++Kernel)
	{
		FScopedKernel Scoped(Kernel);
		for (const FVector& P : Points)
		{
			Values[Kernel].Add(FVoxelPlanetNoise::ReferenceSample(P, EVoxelNoiseType::Simplex, 7));
		}
	}
	double SumC = 0, SumN = 0, SumCC = 0, SumNN = 0, SumCN = 0, MaxAbsN = 0, MaxDiff = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		const double C = Values[0][i];
		const double N = Values[1][i];
		SumC += C; SumN += N; SumCC += C * C; SumNN += N * N; SumCN += C * N;
		MaxAbsN = FMath::Max(MaxAbsN, FMath::Abs(N));
		MaxDiff = FMath::Max(MaxDiff, FMath::Abs(N - C));
	}
	const double MeanC = SumC / Count, MeanN = SumN / Count;
	const double StdC = FMath::Sqrt(SumCC / Count - MeanC * MeanC), StdN = FMath::Sqrt(SumNN / Count - MeanN * MeanN);
	const double Corr = (SumCN / Count - MeanC * MeanN) / (StdC * StdN);
	const double RmsDiff = FMath::Sqrt(FMath::Max(0.0, (SumCC - 2.0 * SumCN + SumNN) / Count));
	AddInfo(FString::Printf(TEXT("Spread: classic std %.4f, continuous std %.4f (max |value| %.3f); correlation %.5f, difference rms %.4f, worst %.4f"),
		StdC, StdN, MaxAbsN, Corr, RmsDiff, MaxDiff));
	TestTrue(TEXT("The continuous kernel keeps the spread (std within 1%)"), FMath::Abs(StdN / StdC - 1.0) < 0.01);
	TestTrue(TEXT("The continuous noise stays in [-1, 1]"), MaxAbsN <= 1.0);
	TestTrue(TEXT("The same landscape (correlation above 0.999)"), Corr > 0.999);
	TestTrue(TEXT("Within a few percent of the classic noise (rms under 0.02)"), RmsDiff < 0.02);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetFaceEdgeContinuityTest,
	"VoxelWorlds.WorldModes.Planet.FaceEdgeContinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetFaceEdgeContinuityTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetSimplexContinuity;

	// The 12 face edges: for each pair of axes (a, b) and signs, the arc where |a| = |b| >= |c|, the plane a = +-b.
	struct FEdge { int32 A; int32 B; double SignA; double SignB; };
	TArray<FEdge> Edges;
	for (const FIntPoint Axes : { FIntPoint(0, 1), FIntPoint(1, 2), FIntPoint(0, 2) })
	{
		for (const double SA : { 1.0, -1.0 })
		{
			for (const double SB : { 1.0, -1.0 })
			{
				Edges.Add({ Axes.X, Axes.Y, SA, SB });
			}
		}
	}

	// The terrain noise alone on the small planet and L (ClassicAtLeast: the classic kernel must step by that much), then
	// the review planets as configured (biomes, continentalness), when their assets are there; SeaAltitude for logging
	// where the largest step lies on land (the live check's site).
	struct FPlanetCase { FString Name; const UVoxelWorldConfiguration* Config; double ClassicAtLeast; double SeaAltitude; };
	// The synthetic configurations are rooted while the review assets load.
	UVoxelWorldConfiguration* SmallNoise = MakePlanetS();
	UVoxelWorldConfiguration* LargeNoise = VoxelPlanetPrecision::MakePlanetL();
	SmallNoise->AddToRoot();
	LargeNoise->AddToRoot();
	ON_SCOPE_EXIT
	{
		SmallNoise->RemoveFromRoot();
		LargeNoise->RemoveFromRoot();
	};
	TArray<FPlanetCase> Cases = {
		{ TEXT("small planet (R 250,000, terrain noise only)"), SmallNoise, 1.0, -500.0 },
		{ TEXT("1,000 km planet (R 1e8, Review_Planet_L's terrain noise only)"), LargeNoise, 10.0, -83333.0 },
	};
	for (const TCHAR* Asset : { TEXT("/Game/PluginTesting/Review/Review_Planet.Review_Planet"), TEXT("/Game/PluginTesting/Review/Review_Planet_M.Review_Planet_M"),
		TEXT("/Game/PluginTesting/Review/Review_Planet_L.Review_Planet_L") })
	{
		const UVoxelWorldConfiguration* Config = LoadObject<UVoxelWorldConfiguration>(nullptr, Asset);
		if (!Config || Config->WorldMode != EWorldMode::SphericalPlanet)
		{
			AddInfo(FString::Printf(TEXT("%s: not found or not a planet, skipped"), Asset));
			continue;
		}
		const double Sea = Config->bEnableWaterLevel ? static_cast<double>(Config->WaterRadius) - static_cast<double>(Config->WorldRadius) : -1.0e30;
		Cases.Add({ Config->GetName(), Config, 0.0, Sea });
	}

	constexpr double Delta = 1.0e-9; // radians either side of the edge's plane
	constexpr int32 PointsPerEdge = 300;
	for (const FPlanetCase& Case : Cases)
	{
		const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Case.Config);
		const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
		// The terrain's own change over the 2 Delta between the samples (about R x 2e-9 x its slope) bounds the step allowed.
		const double Tolerance = Case.Config->WorldRadius >= 1.0e7 ? 0.5 : 0.05;
		double Worst[2] = { 0.0, 0.0 };
		double WorstShared[2] = { 0.0, 0.0 }; // the edges on x = y, y = z, x = z (signs alike): every octave's boundary
		double WorstLand = 0.0;
		FVector WorstLandDir = FVector::ZeroVector;
		double WorstLandAltitude = 0.0;
		FVector WorstContinuousDir = FVector::UpVector, WorstContinuousNormal = FVector::ForwardVector;
		double Control[2] = { 0.0, 0.0 }; // the same probe at random directions off the edges
		for (int32 Kernel = 0; Kernel < 2; ++Kernel)
		{
			FScopedKernel Scoped(Kernel);
			FRandomStream Random(4310);
			for (const FEdge& Edge : Edges)
			{
				const int32 C = 3 - Edge.A - Edge.B;
				FVector N = FVector::ZeroVector; // the plane SignA x a = SignB x b
				N[Edge.A] = Edge.SignA;
				N[Edge.B] = -Edge.SignB;
				N = N.GetSafeNormal();
				for (int32 i = 0; i < PointsPerEdge; ++i)
				{
					FVector D = FVector::ZeroVector;
					D[Edge.A] = Edge.SignA;
					D[Edge.B] = Edge.SignB;
					D[C] = Random.FRandRange(-0.999, 0.999);
					D = D.GetSafeNormal();
					const double Plus = Planet.GetTerrainAltitude((D + N * Delta).GetSafeNormal(), Case.Config->NoiseParams);
					const double Minus = Planet.GetTerrainAltitude((D - N * Delta).GetSafeNormal(), Case.Config->NoiseParams);
					const double Step = FMath::Abs(Plus - Minus);
					if (Kernel == 1 && Step > Worst[1])
					{
						WorstContinuousDir = D;
						WorstContinuousNormal = N;
					}
					Worst[Kernel] = FMath::Max(Worst[Kernel], Step);
					if (Edge.SignA == Edge.SignB)
					{
						WorstShared[Kernel] = FMath::Max(WorstShared[Kernel], Step);
					}
					if (Kernel == 0 && FMath::Min(Plus, Minus) > Case.SeaAltitude && Step > WorstLand)
					{
						WorstLand = Step;
						WorstLandDir = D;
						WorstLandAltitude = 0.5 * (Plus + Minus);
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s: across the 12 face edges (%d directions each, %.0e rad either side) the altitude steps by up to %.3f uu with the classic kernel ")
			TEXT("(%.3f on x = y, y = z, x = z), %.4f uu with the continuous one (%.4f)"),
			*Case.Name, PointsPerEdge, Delta, Worst[0], WorstShared[0], Worst[1], WorstShared[1]));
		AddInfo(FString::Printf(TEXT("%s: the largest classic step on land, %.3f uu, at latitude %.4f, longitude %.4f (altitude %.0f)"), *Case.Name, WorstLand,
			FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(WorstLandDir.Z, -1.0, 1.0))), FMath::RadiansToDegrees(FMath::Atan2(WorstLandDir.Y, WorstLandDir.X)), WorstLandAltitude));
		// The control: as many random directions as the edges had, each probed across a random tangent the same way. Away
		// from the face edges no octave's boundary is near, so this is the terrain's own change over the distance plus its
		// float resolution (on the configured big planets the continentalness field's: its fraction resolves about 7 uu of
		// ground on L, 0.3-0.5 uu of height a step).
		for (int32 Kernel = 0; Kernel < 2; ++Kernel)
		{
			FScopedKernel Scoped(Kernel);
			FRandomStream Random(4311);
			for (int32 i = 0; i < Edges.Num() * PointsPerEdge; ++i)
			{
				const FVector D = Random.GetUnitVector();
				FVector T = FVector::CrossProduct(D, Random.GetUnitVector());
				T = T.GetSafeNormal();
				const double Plus = Planet.GetTerrainAltitude((D + T * Delta).GetSafeNormal(), Case.Config->NoiseParams);
				const double Minus = Planet.GetTerrainAltitude((D - T * Delta).GetSafeNormal(), Case.Config->NoiseParams);
				Control[Kernel] = FMath::Max(Control[Kernel], FMath::Abs(Plus - Minus));
			}
		}
		AddInfo(FString::Printf(TEXT("%s: the control, %d random directions off the edges probed the same way: up to %.4f uu with the classic kernel, %.4f with the continuous one"),
			*Case.Name, Edges.Num() * PointsPerEdge, Control[0], Control[1]));

		// Where the continuous kernel's difference is largest, how it grows with the distance across the edge: in proportion
		// for the terrain's own slope, not at all for a step (or the float resolution).
		double Profile[4];
		{
			FScopedKernel Continuous(1);
			for (int32 k = 0; k < 4; ++k)
			{
				const double D = Delta * FMath::Pow(10.0, k);
				Profile[k] = FMath::Abs(Planet.GetTerrainAltitude((WorstContinuousDir + WorstContinuousNormal * D).GetSafeNormal(), Case.Config->NoiseParams)
					- Planet.GetTerrainAltitude((WorstContinuousDir - WorstContinuousNormal * D).GetSafeNormal(), Case.Config->NoiseParams));
			}
		}
		AddInfo(FString::Printf(TEXT("%s: the continuous kernel's largest difference (latitude %.4f, longitude %.4f) at 1, 10, 100 and 1,000 times the distance: %.4f, %.4f, %.3f, %.2f uu"),
			*Case.Name, FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(WorstContinuousDir.Z, -1.0, 1.0))), FMath::RadiansToDegrees(FMath::Atan2(WorstContinuousDir.Y, WorstContinuousDir.X)),
			Profile[0], Profile[1], Profile[2], Profile[3]));
		if (Case.ClassicAtLeast > 0.0)
		{
			TestTrue(*FString::Printf(TEXT("%s: the classic kernel steps across the face edges (non-vacuous, %.3f uu)"), *Case.Name, WorstShared[0]), WorstShared[0] > Case.ClassicAtLeast);
		}
		TestTrue(*FString::Printf(TEXT("%s: with the classic kernel the face edges step far beyond the control (non-vacuous, %.3f against %.4f uu)"), *Case.Name, WorstShared[0], Control[0]),
			WorstShared[0] > 5.0 * Control[0]);
		// No step: under the tolerance, or no rougher than the terrain anywhere else (the control).
		TestTrue(*FString::Printf(TEXT("%s: no step across any face edge (%.4f uu; the control %.4f)"), *Case.Name, Worst[1], Control[1]),
			Worst[1] < Tolerance || Worst[1] <= 1.5 * Control[1]);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
