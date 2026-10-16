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

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 1, a correct planet on the CPU (WORLD_MODES_REVIEW_PLAN.md §15; SPHERICAL_PLANET_REQUIREMENTS.md
// SP-A1, SP-A2, SP-B1, SP-B5, SP-I2). Pure CPU, runs headless.
//
// - The analytic surface (GetSurfacePoint) lands on the isosurface generation writes, by direction: poles, equator,
//   45 degrees, cube-face edges and corners.
// - The terrain noise is sampled by 3D direction: the hemispheres differ, nothing steps across a cube-face edge, and
//   a feature's size in uu does not depend on the radius (it matches the plane's).
// - The surface normal points out of the terrain and only the direction from the centre matters.
// - The displacement bounds and the configuration's shell check agree, continentalness included.
// - The surface coordinate's tangent warp keeps equal UV cells within 1.5x in area and round-trips.
// ---------------------------------------------------------------------------

namespace SphericalPlanetTestUtils
{
	constexpr float PlanetTestRadius = 250000.0f;
	constexpr float PlanetTestVoxelSize = 75.0f;
	constexpr int32 PlanetTestChunkSize = 32;

	/** A biome configuration with continentalness on (the default curves: offset -3000..+1000, scale 0.2..1). Rooted. */
	UVoxelBiomeConfiguration* MakeContinentalnessBiomes()
	{
		UVoxelBiomeConfiguration* Biomes = NewObject<UVoxelBiomeConfiguration>();
		Biomes->AddToRoot();
		Biomes->bEnableContinentalness = true;
		Biomes->RebuildBakedCurves();
		return Biomes;
	}

	/** A planet like Review_Planet: R 250,000, PlanetHeightScale 6,000, the demo's terrain noise. Rooted. */
	UVoxelWorldConfiguration* MakePlanetConfig(UVoxelBiomeConfiguration* Biomes, const FVector& Origin = FVector::ZeroVector)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = Origin;
		Config->WorldRadius = PlanetTestRadius;
		Config->PlanetHeightScale = 6000.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = 6500.0f;
		Config->PlanetMaxTerrainDepth = 6500.0f;
		Config->VoxelSize = PlanetTestVoxelSize;
		Config->ChunkSize = PlanetTestChunkSize;
		Config->BiomeConfiguration = Biomes;
		Config->bEnableBiomes = false;
		Config->bEnableCaves = false;
		Config->bEnableWaterLevel = false;
		Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
		Config->NoiseParams.Seed = 248675;
		Config->NoiseParams.Octaves = 3;
		Config->NoiseParams.Frequency = 0.000025f;
		Config->NoiseParams.Lacunarity = 1.681f;
		Config->NoiseParams.Persistence = 0.5f;
		return Config;
	}

	/** Generates chunks on demand through the CPU generator and reads their voxels by global voxel index. */
	struct FPlanetVoxels
	{
		const UVoxelWorldConfiguration* Config = nullptr;
		FVoxelCPUNoiseGenerator Generator;
		TMap<FIntVector, TArray<FVoxelData>> Chunks;

		explicit FPlanetVoxels(const UVoxelWorldConfiguration* InConfig) : Config(InConfig) {}

		FVoxelNoiseGenerationRequest MakeRequest(const FIntVector& ChunkCoord) const
		{
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = ChunkCoord;
			Request.ChunkSize = Config->ChunkSize;
			Request.VoxelSize = Config->VoxelSize;
			Request.LODLevel = 0;
			Request.NoiseParams = Config->NoiseParams;
			FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
			Request.bEnableBiomes = Config->bEnableBiomes;
			Request.BiomeConfiguration = Config->BiomeConfiguration;
			Request.bEnableCaves = Config->bEnableCaves;
			Request.CaveConfiguration = Config->CaveConfiguration;
			Request.bEnableWaterLevel = Config->bEnableWaterLevel;
			Request.WaterLevel = Config->WaterLevel;
			Request.WaterRadius = Config->WaterRadius;
			return Request;
		}

		const TArray<FVoxelData>& Chunk(const FIntVector& ChunkCoord)
		{
			if (const TArray<FVoxelData>* Found = Chunks.Find(ChunkCoord))
			{
				return *Found;
			}
			TArray<FVoxelData>& Data = Chunks.Add(ChunkCoord);
			Generator.GenerateChunkCPU(MakeRequest(ChunkCoord), Data);
			return Data;
		}

		const FVoxelData& Voxel(const FIntVector& Global)
		{
			const int32 N = Config->ChunkSize;
			const FIntVector ChunkCoord(FMath::FloorToInt(Global.X / static_cast<float>(N)), FMath::FloorToInt(Global.Y / static_cast<float>(N)),
				FMath::FloorToInt(Global.Z / static_cast<float>(N)));
			const FIntVector Local = Global - ChunkCoord * N;
			return Chunk(ChunkCoord)[Local.X + Local.Y * N + Local.Z * N * N];
		}

		/** Trilinear density at a world position (voxel samples sit at WorldOrigin + index * VoxelSize). */
		float Density(const FVector& WorldPos)
		{
			const FVector F = (WorldPos - Config->WorldOrigin) / Config->VoxelSize;
			const FIntVector I0(FMath::FloorToInt(F.X), FMath::FloorToInt(F.Y), FMath::FloorToInt(F.Z));
			const FVector T = F - FVector(I0);
			float Result = 0.0f;
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FIntVector O(Corner & 1, (Corner >> 1) & 1, (Corner >> 2) & 1);
				const float W = (O.X ? T.X : 1.0 - T.X) * (O.Y ? T.Y : 1.0 - T.Y) * (O.Z ? T.Z : 1.0 - T.Z);
				Result += W * Voxel(I0 + O).Density;
			}
			return Result;
		}

		/**
		 * The generated surface's distance from the centre along a direction: the outermost crossing of the density
		 * threshold within +-Window of Around. Returns a negative value when there is no crossing.
		 */
		double SurfaceRadius(const FVector& Center, const FVector& Direction, double Around, double Window)
		{
			constexpr float Iso = 127.5f;
			const double Step = Config->VoxelSize * 0.02;
			double Best = -1.0;
			float Prev = Density(Center + Direction * (Around - Window));
			for (double R = Around - Window + Step; R <= Around + Window; R += Step)
			{
				const float Cur = Density(Center + Direction * R);
				if (Prev >= Iso && Cur < Iso)
				{
					Best = (R - Step) + Step * (Prev - Iso) / FMath::Max(Prev - Cur, 1.0e-3f);
				}
				Prev = Cur;
			}
			return Best;
		}
	};

	/** Directions the tests visit: poles, equator, 45 degrees, cube-face edges and corners, and a few others. */
	TArray<FVector> SurveyDirections()
	{
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		return {
			FVector(0, 0, 1), FVector(0, 0, -1),                                // poles
			FVector(1, 0, 0), FVector(0, 1, 0), FVector(-1, 0, 0), FVector(0, -1, 0), // equator
			FVector(C45, 0, C45), FVector(0, -C45, C45), FVector(-C45, 0, -C45), // 45 degrees north and south
			FVector(1, 1, 0).GetSafeNormal(), FVector(-1, 1, 0).GetSafeNormal(), // cube-face edges
			FVector(1, 0, 1).GetSafeNormal(),
			FVector(1, 1, 1).GetSafeNormal(), FVector(-1, 1, -1).GetSafeNormal(), // cube corners
			FVector(0.3, -0.7, 0.2).GetSafeNormal(), FVector(-0.6, -0.2, -0.75).GetSafeNormal(),
		};
	}

	/** Directions spread over the sphere (a Fibonacci lattice). */
	TArray<FVector> FibonacciDirections(int32 Count)
	{
		TArray<FVector> Out;
		const double Golden = UE_DOUBLE_PI * (3.0 - FMath::Sqrt(5.0));
		for (int32 i = 0; i < Count; ++i)
		{
			const double Z = 1.0 - (i + 0.5) * 2.0 / Count;
			const double Rho = FMath::Sqrt(FMath::Max(0.0, 1.0 - Z * Z));
			Out.Add(FVector(Rho * FMath::Cos(Golden * i), Rho * FMath::Sin(Golden * i), Z));
		}
		return Out;
	}

	/** Sign changes of the terrain noise along a path sampled every Step uu. */
	template <typename FSample>
	int32 CountSignChanges(int32 Samples, FSample&& Sample)
	{
		int32 Changes = 0;
		float Prev = Sample(0);
		for (int32 i = 1; i < Samples; ++i)
		{
			const float Cur = Sample(i);
			Changes += (Prev < 0.0f) != (Cur < 0.0f) ? 1 : 0;
			Prev = Cur;
		}
		return Changes;
	}

	/** Solid angle of the spherical triangle (A, B, C), unit vectors. */
	double TriangleSolidAngle(const FVector& A, const FVector& B, const FVector& C)
	{
		const double Numerator = FMath::Abs(FVector::DotProduct(A, FVector::CrossProduct(B, C)));
		const double Denominator = 1.0 + FVector::DotProduct(A, B) + FVector::DotProduct(B, C) + FVector::DotProduct(C, A);
		return 2.0 * FMath::Atan2(Numerator, Denominator);
	}
}

/**
 * SP-A1, SP-I2: GetSurfacePoint and generation agree by direction. For each direction the generated isosurface along
 * the radius (the CPU generator's voxels, trilinear) lies within a quarter voxel of the analytic surface point, with
 * continentalness on, at the poles, the equator, 45 degrees, cube-face edges and corners.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetSurfaceMatchesGenerationTest,
	"VoxelWorlds.WorldModes.Planet.SurfaceMatchesGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetSurfaceMatchesGenerationTest::RunTest(const FString& Parameters)
{
	using namespace SphericalPlanetTestUtils;

	UVoxelBiomeConfiguration* Biomes = MakeContinentalnessBiomes();
	// An origin off zero: the surface is relative to the planet centre, wherever the centre is.
	UVoxelWorldConfiguration* Config = MakePlanetConfig(Biomes, FVector(12345.0, -6789.0, 1000.0));
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVector Center = Config->WorldOrigin;
	FPlanetVoxels Voxels(Config);

	double MaxError = 0.0;
	double MinAltitude = TNumericLimits<double>::Max(), MaxAltitude = TNumericLimits<double>::Lowest();
	for (const FVector& Direction : SurveyDirections())
	{
		const FVoxelTerrainSurfacePoint Surface = Mode->GetSurfacePoint(Center + Direction * 1000.0, Config->NoiseParams);
		const double AnalyticRadius = FVector::Dist(Surface.Position, Center);
		const double GeneratedRadius = Voxels.SurfaceRadius(Center, Direction, AnalyticRadius, 3.0 * PlanetTestVoxelSize);
		if (!TestTrue(*FString::Printf(TEXT("A generated surface crossing along (%.3f, %.3f, %.3f)"), Direction.X, Direction.Y, Direction.Z),
			GeneratedRadius > 0.0))
		{
			continue;
		}
		const double Error = FMath::Abs(GeneratedRadius - AnalyticRadius);
		MaxError = FMath::Max(MaxError, Error);
		MinAltitude = FMath::Min(MinAltitude, static_cast<double>(Surface.Altitude));
		MaxAltitude = FMath::Max(MaxAltitude, static_cast<double>(Surface.Altitude));
		TestTrue(*FString::Printf(TEXT("Along (%.3f, %.3f, %.3f): generated %.1f vs analytic %.1f (within a quarter voxel)"),
			Direction.X, Direction.Y, Direction.Z, GeneratedRadius, AnalyticRadius), Error <= 0.25 * PlanetTestVoxelSize);
		TestEqual(TEXT("The surface point lies on the direction"), FVector::DotProduct((Surface.Position - Center).GetSafeNormal(), Direction), 1.0, 1.0e-6);
		TestEqual(TEXT("Altitude = distance from the centre - PlanetRadius"), static_cast<double>(Surface.Altitude), AnalyticRadius - PlanetTestRadius, 0.5);
	}
	AddInfo(FString::Printf(TEXT("Max analytic/generated gap %.2f uu over %d directions (%d chunks generated); surface altitude %.0f to %.0f"),
		MaxError, SurveyDirections().Num(), Voxels.Chunks.Num(), MinAltitude, MaxAltitude));
	TestTrue(TEXT("The survey sees relief (the surface is not a plain sphere)"), MaxAltitude - MinAltitude > 1000.0);

	Config->RemoveFromRoot();
	Biomes->RemoveFromRoot();
	return true;
}

/**
 * SP-B1, SP-I2: the terrain noise is sampled by 3D direction on the datum sphere. The two hemispheres differ (the old
 * sampling used only X and Y, so z and -z had the same height); nothing steps across a cube-face edge; and a feature's
 * size in uu does not depend on the radius and matches the plane's, for the same noise params.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetNoiseSeamlessTest,
	"VoxelWorlds.WorldModes.Planet.NoiseSeamless",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetNoiseSeamlessTest::RunTest(const FString& Parameters)
{
	using namespace SphericalPlanetTestUtils;

	UVoxelWorldConfiguration* Config = MakePlanetConfig(nullptr);
	const FWorldModeTerrainParams Terrain = FVoxelWorldModeFactory::MakePlanetTerrainParams(*Config);
	const FVoxelNoiseParams& Noise = Config->NoiseParams;
	auto Displacement = [&](const FVector& Direction, float Radius)
	{
		return FSphericalPlanetWorldMode::SampleRadialDisplacement(Direction, Radius, Terrain, Noise, nullptr);
	};

	// 1. No mirror: compare every northern direction with its reflection through the equatorial plane.
	int32 Pairs = 0, Different = 0;
	double MeanAbsDiff = 0.0;
	for (const FVector& Direction : FibonacciDirections(400))
	{
		if (Direction.Z < 0.05)
		{
			continue;
		}
		const double Diff = FMath::Abs(Displacement(Direction, PlanetTestRadius) - Displacement(FVector(Direction.X, Direction.Y, -Direction.Z), PlanetTestRadius));
		++Pairs;
		Different += Diff > 1.0 ? 1 : 0;
		MeanAbsDiff += Diff;
	}
	MeanAbsDiff /= FMath::Max(Pairs, 1);
	AddInfo(FString::Printf(TEXT("Hemispheres: %d of %d mirrored pairs differ by more than 1 uu, mean |diff| %.0f uu"), Different, Pairs, MeanAbsDiff));
	TestTrue(TEXT("The hemispheres are not mirrored (95% of mirrored pairs differ)"), Different >= Pairs * 95 / 100);

	// 2. No step at cube-face edges: the height difference between two directions 2 x 100 uu of arc apart, straddling
	// an edge (they map to different faces), is no larger than the worst difference at the same spacing inside faces.
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const double Half = 100.0 / PlanetTestRadius;
	double WorstAcross = 0.0, WorstInside = 0.0;
	int32 AcrossPairs = 0;
	for (int32 i = 0; i < 64; ++i)
	{
		// Points along the edge between the +X and +Y faces (x = y > |z|) and between +Z and +X (z = x > |y|).
		const double T = -0.6 + 1.2 * i / 63.0;
		for (int32 Edge = 0; Edge < 2; ++Edge)
		{
			const FVector OnEdge = (Edge == 0 ? FVector(1.0, 1.0, T) : FVector(1.0, T, 1.0)).GetSafeNormal();
			const FVector Across = (Edge == 0 ? FVector(1.0, -1.0, 0.0) : FVector(1.0, 0.0, -1.0)).GetSafeNormal();
			const FVector Along = FVector::CrossProduct(OnEdge, Across).GetSafeNormal();
			const FVector A = (OnEdge + Across * Half).GetSafeNormal();
			const FVector B = (OnEdge - Across * Half).GetSafeNormal();
			if (Mode->WorldToSurface(A * PlanetTestRadius).Face == Mode->WorldToSurface(B * PlanetTestRadius).Face)
			{
				continue;
			}
			++AcrossPairs;
			WorstAcross = FMath::Max(WorstAcross, static_cast<double>(FMath::Abs(Displacement(A, PlanetTestRadius) - Displacement(B, PlanetTestRadius))));
			// The same spacing and direction inside a face, away from the edge.
			const FVector InFace = (OnEdge + Across * 0.2 + Along * 0.05).GetSafeNormal();
			const FVector P = (InFace + Across * Half).GetSafeNormal();
			const FVector Q = (InFace - Across * Half).GetSafeNormal();
			WorstInside = FMath::Max(WorstInside, static_cast<double>(FMath::Abs(Displacement(P, PlanetTestRadius) - Displacement(Q, PlanetTestRadius))));
		}
	}
	AddInfo(FString::Printf(TEXT("Cube-face edges: %d pairs 200 uu apart across an edge, worst height step %.2f uu (inside faces %.2f uu)"),
		AcrossPairs, WorstAcross, WorstInside));
	TestTrue(TEXT("Pairs straddle cube-face edges"), AcrossPairs >= 100);
	TestTrue(TEXT("No step across cube-face edges (within 2x the worst inside a face, + 1 uu)"), WorstAcross <= 2.0 * WorstInside + 1.0);

	// 3. Feature size in uu: count the terrain noise's sign changes along 400,000 uu of great circle on planets of two
	// radii, and along a 400,000 uu line on the plane with the same noise params. With the old sampling (direction x a
	// fixed 10,000) a 250,000 uu planet saw a fortieth of the plane's features and a 1,000,000 uu one a hundred and
	// sixtieth.
	constexpr double Length = 400000.0;
	constexpr int32 Samples = 1601; // every 250 uu
	auto GreatCircleChanges = [&](float Radius)
	{
		const FVector Start = FVector(0.3, 0.2, 1.0).GetSafeNormal();
		const FVector Axis = FVector::CrossProduct(Start, FVector(1.0, 0.0, 0.0)).GetSafeNormal();
		return CountSignChanges(Samples, [&](int32 i)
		{
			const double Angle = (Length / Radius) * i / (Samples - 1);
			return FSphericalPlanetWorldMode::SampleSphericalNoise(Start.RotateAngleAxisRad(Angle, Axis), Radius, Noise);
		});
	};
	const int32 Small = GreatCircleChanges(PlanetTestRadius);
	const int32 Large = GreatCircleChanges(1000000.0f);
	const int32 Plane = CountSignChanges(Samples, [&](int32 i)
	{
		return FInfinitePlaneWorldMode::SampleTerrainNoise2D(static_cast<float>(-150000.0 + Length * i / (Samples - 1)), 3000.0f, Noise);
	});
	AddInfo(FString::Printf(TEXT("Sign changes over 400,000 uu: planet R 250,000: %d, planet R 1,000,000: %d, plane: %d"), Small, Large, Plane));
	TestTrue(TEXT("The plane's line sees features"), Plane >= 10);
	TestTrue(TEXT("Feature size does not depend on the radius (counts within 0.6x-1.6x)"), Small >= Large * 0.6 && Small <= Large * 1.6);
	TestTrue(TEXT("Feature size matches the plane's (counts within 0.6x-1.6x)"), Small >= Plane * 0.6 && Small <= Plane * 1.6);

	Config->RemoveFromRoot();
	return true;
}

/**
 * SP-A1: the surface normal points out of the terrain, agrees with the surface's own shape, and is radial where the
 * terrain is flat; only the direction from the centre picks the surface point. The heightmap modes' default
 * implementation matches their height query.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetSurfacePointTest,
	"VoxelWorlds.WorldModes.Planet.SurfacePoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetSurfacePointTest::RunTest(const FString& Parameters)
{
	using namespace SphericalPlanetTestUtils;

	UVoxelBiomeConfiguration* Biomes = MakeContinentalnessBiomes();
	UVoxelWorldConfiguration* Config = MakePlanetConfig(Biomes);
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVector Center = Config->WorldOrigin;

	int32 Outward = 0, Matches = 0, Count = 0;
	for (const FVector& Direction : FibonacciDirections(200))
	{
		const FVoxelTerrainSurfacePoint Near = Mode->GetSurfacePoint(Center + Direction * 10.0, Config->NoiseParams);
		const FVoxelTerrainSurfacePoint Far = Mode->GetSurfacePoint(Center + Direction * 900000.0, Config->NoiseParams);
		TestTrue(TEXT("Only the direction picks the surface point"), Near.Position.Equals(Far.Position, 0.01));
		++Count;
		Outward += FVector::DotProduct(Near.Normal, Direction) > 0.0 ? 1 : 0;

		// The normal is perpendicular to the surface: points 300 uu away along two tangents lie in its tangent plane,
		// to first order (a slope of at most 0.05 off).
		const FVector T1 = FVector::CrossProduct(FMath::Abs(Direction.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector, Direction).GetSafeNormal();
		const FVector T2 = FVector::CrossProduct(Direction, T1);
		bool bTangent = true;
		for (const FVector& T : { T1, T2 })
		{
			const FVoxelTerrainSurfacePoint Side = Mode->GetSurfacePoint(Center + (Direction + T * (300.0 / PlanetTestRadius)).GetSafeNormal(), Config->NoiseParams);
			const FVector Chord = Side.Position - Near.Position;
			bTangent &= FMath::Abs(FVector::DotProduct(Chord.GetSafeNormal(), Near.Normal)) < 0.05;
		}
		Matches += bTangent ? 1 : 0;
	}
	TestEqual(TEXT("Every normal points out of the planet"), Outward, Count);
	TestTrue(TEXT("Normals are perpendicular to the surface (95%)"), Matches >= Count * 95 / 100);
	AddInfo(FString::Printf(TEXT("%d of %d normals perpendicular to the sampled surface"), Matches, Count));

	// Flat terrain: the normal is radial.
	Config->PlanetHeightScale = 100.0f;
	Config->BiomeConfiguration = nullptr;
	const TSharedRef<IVoxelWorldMode> Flat = FVoxelWorldModeFactory::Create(*Config);
	for (const FVector& Direction : SurveyDirections())
	{
		const FVoxelTerrainSurfacePoint Point = Flat->GetSurfacePoint(Center + Direction, Config->NoiseParams);
		TestTrue(TEXT("Nearly flat terrain: the normal is radial"), FVector::DotProduct(Point.Normal, Direction) > 0.999);
	}

	// Heightmap modes (the interface default): the point at (X, Y, height), the normal from the height's gradient.
	UVoxelWorldConfiguration* PlaneConfig = NewObject<UVoxelWorldConfiguration>();
	PlaneConfig->WorldMode = EWorldMode::InfinitePlane;
	PlaneConfig->NoiseParams = Config->NoiseParams;
	PlaneConfig->HeightScale = 8000.0f;
	const TSharedRef<IVoxelWorldMode> PlaneMode = FVoxelWorldModeFactory::Create(*PlaneConfig);
	for (const FVector2D XY : { FVector2D(0.0, 0.0), FVector2D(12345.0, -54321.0), FVector2D(-150000.0, 80000.0) })
	{
		const FVoxelTerrainSurfacePoint Point = PlaneMode->GetSurfacePoint(FVector(XY.X, XY.Y, 99999.0), PlaneConfig->NoiseParams);
		const float Height = PlaneMode->GetTerrainHeightAt(XY.X, XY.Y, PlaneConfig->NoiseParams);
		TestEqual(TEXT("Plane: the point is at the height query's Z"), static_cast<float>(Point.Position.Z), Height, 0.01f);
		TestEqual(TEXT("Plane: the point keeps X"), Point.Position.X, XY.X, 0.001);
		TestTrue(TEXT("Plane: the normal points up"), Point.Normal.Z > 0.0);
		const float DX = PlaneMode->GetTerrainHeightAt(XY.X + 50.0f, XY.Y, PlaneConfig->NoiseParams) - PlaneMode->GetTerrainHeightAt(XY.X - 50.0f, XY.Y, PlaneConfig->NoiseParams);
		TestTrue(TEXT("Plane: the normal leans downhill"), FMath::Abs(DX) < 1.0f || FMath::Sign(Point.Normal.X) == -FMath::Sign(DX));
	}

	Config->RemoveFromRoot();
	Biomes->RemoveFromRoot();
	return true;
}

/**
 * SP-B5, SP-A5: continentalness modulates the planet's radial height (offset and scale), the displacement stays inside
 * GetDisplacementBounds, and the configuration's shell check computes the same bounds.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetContinentalnessTest,
	"VoxelWorlds.WorldModes.Planet.Continentalness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetContinentalnessTest::RunTest(const FString& Parameters)
{
	using namespace SphericalPlanetTestUtils;

	UVoxelBiomeConfiguration* Biomes = MakeContinentalnessBiomes();
	UVoxelWorldConfiguration* Config = MakePlanetConfig(Biomes);
	const FWorldModeTerrainParams Terrain = FVoxelWorldModeFactory::MakePlanetTerrainParams(*Config);
	const FVoxelBiomeSnapshot Snapshot = FVoxelBiomeSnapshot::FromConfig(Biomes);

	// The effective params at a direction are the plane's continentalness math at the datum-sphere point.
	float MinDisp = TNumericLimits<float>::Max(), MaxDisp = TNumericLimits<float>::Lowest();
	float MinCont = 1.0f, MaxCont = -1.0f;
	int32 Modulated = 0, Count = 0;
	for (const FVector& Direction : FibonacciDirections(600))
	{
		float Continentalness = 0.0f;
		const float Disp = FSphericalPlanetWorldMode::SampleRadialDisplacement(Direction, PlanetTestRadius, Terrain, Config->NoiseParams, &Snapshot, &Continentalness);
		const float Raw = FSphericalPlanetWorldMode::SampleRadialDisplacement(Direction, PlanetTestRadius, Terrain, Config->NoiseParams, nullptr);
		float Offset = 0.0f, Mult = 1.0f;
		Snapshot.GetContinentalnessTerrainParams(Continentalness, Offset, Mult);
		const float Noise = FSphericalPlanetWorldMode::SampleSphericalNoise(Direction, PlanetTestRadius, Config->NoiseParams);
		TestEqual(TEXT("Displacement = BaseHeight + offset + noise x PlanetHeightScale x multiplier / 2"),
			Disp, Terrain.BaseHeight + Offset + Noise * Terrain.HeightScale * Mult * 0.5f, 0.05f);
		Modulated += FMath::Abs(Disp - Raw) > 10.0f ? 1 : 0;
		++Count;
		MinDisp = FMath::Min(MinDisp, Disp);
		MaxDisp = FMath::Max(MaxDisp, Disp);
		MinCont = FMath::Min(MinCont, Continentalness);
		MaxCont = FMath::Max(MaxCont, Continentalness);
	}
	float BoundMin = 0.0f, BoundMax = 0.0f;
	FSphericalPlanetWorldMode::GetDisplacementBounds(Terrain, &Snapshot, BoundMin, BoundMax);
	AddInfo(FString::Printf(TEXT("Displacement %.0f to %.0f (bounds %.0f to %.0f), continentalness %.2f to %.2f, %d of %d directions modulated"),
		MinDisp, MaxDisp, BoundMin, BoundMax, MinCont, MaxCont, Modulated, Count));
	TestTrue(TEXT("Continentalness changes the height almost everywhere (90%)"), Modulated >= Count * 90 / 100);
	TestTrue(TEXT("The displacement stays inside its bounds"), MinDisp >= BoundMin && MaxDisp <= BoundMax);
	TestEqual(TEXT("Bounds: BaseHeight + lowest offset - PlanetHeightScale / 2 x largest multiplier"), BoundMin, -3000.0f - 3000.0f, 0.5f);
	TestEqual(TEXT("Bounds: BaseHeight + highest offset + PlanetHeightScale / 2 x largest multiplier"), BoundMax, 1000.0f + 3000.0f, 0.5f);

	// The configuration's shell check reads the same numbers: 6,500 covers them; 5,500 down does not.
	TestEqual(TEXT("A shell of 6,500 covers the terrain"), Config->GetSettingsWarnings().Num(), 0);
	Config->PlanetMaxTerrainDepth = 5500.0f;
	const TArray<FString> Warnings = Config->GetSettingsWarnings();
	TestEqual(TEXT("A depth of 5,500 is short of the valleys"), Warnings.Num(), 1);
	TestTrue(TEXT("... and the warning names the depth and the 6,000 it needs"),
		Warnings.Num() == 1 && Warnings[0].Contains(TEXT("PlanetMaxTerrainDepth")) && Warnings[0].Contains(TEXT("6000")));

	Config->RemoveFromRoot();
	Biomes->RemoveFromRoot();
	return true;
}

/**
 * SP-A2: the surface coordinate's tangent warp. Equal UV cells cover areas within 1.5x of each other on every face (the
 * plain gnomonic map: about 5x), the coordinate round-trips, UV stays in [-1, 1] and face edges sit at UV +-1.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanetSurfaceCoordWarpTest,
	"VoxelWorlds.WorldModes.Planet.SurfaceCoordWarp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlanetSurfaceCoordWarpTest::RunTest(const FString& Parameters)
{
	using namespace SphericalPlanetTestUtils;

	UVoxelWorldConfiguration* Config = MakePlanetConfig(nullptr, FVector(500.0, -700.0, 300.0));
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVector Center = Config->WorldOrigin;
	constexpr int32 Cells = 32;

	double MinArea = TNumericLimits<double>::Max(), MaxArea = 0.0, Total = 0.0;
	int32 RoundTripFailures = 0;
	for (int32 Face = 0; Face < 6; ++Face)
	{
		auto Dir = [&](double U, double V)
		{
			return (Mode->SurfaceToWorld(FVoxelSurfaceCoord(Face, FVector2D(U, V)), 0.0f) - Center).GetSafeNormal();
		};
		for (int32 i = 0; i < Cells; ++i)
		{
			for (int32 j = 0; j < Cells; ++j)
			{
				const double U0 = -1.0 + 2.0 * i / Cells, U1 = -1.0 + 2.0 * (i + 1) / Cells;
				const double V0 = -1.0 + 2.0 * j / Cells, V1 = -1.0 + 2.0 * (j + 1) / Cells;
				const FVector A = Dir(U0, V0), B = Dir(U1, V0), C = Dir(U1, V1), D = Dir(U0, V1);
				const double Area = TriangleSolidAngle(A, B, C) + TriangleSolidAngle(A, C, D);
				MinArea = FMath::Min(MinArea, Area);
				MaxArea = FMath::Max(MaxArea, Area);
				Total += Area;

				// Round trip at the cell centre.
				const FVoxelSurfaceCoord Coord(Face, FVector2D((U0 + U1) * 0.5, (V0 + V1) * 0.5));
				const FVector World = Mode->SurfaceToWorld(Coord, 123.0f);
				const FVoxelSurfaceCoord Back = Mode->WorldToSurface(World);
				RoundTripFailures += (Back.Face == Face && Back.UV.Equals(Coord.UV, 1.0e-6)) ? 0 : 1;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("UV cells (%d x %d per face): area max / min %.3f, total solid angle %.5f (4 pi = %.5f)"),
		Cells, Cells, MaxArea / MinArea, Total, 4.0 * UE_DOUBLE_PI));
	TestTrue(TEXT("Equal UV cells within 1.5x in area"), MaxArea / MinArea <= 1.5);
	TestEqual(TEXT("The six faces cover the sphere once"), Total, 4.0 * UE_DOUBLE_PI, 1.0e-3);
	TestEqual(TEXT("WorldToSurface and SurfaceToWorld round-trip"), RoundTripFailures, 0);

	// A face edge (x = y) and a corner sit at UV +-1.
	const FVoxelSurfaceCoord Edge = Mode->WorldToSurface(Center + FVector(1.0, 1.0, 0.0).GetSafeNormal() * PlanetTestRadius);
	TestEqual(TEXT("A face edge is at |U| = 1"), FMath::Abs(Edge.UV.X), 1.0, 1.0e-6);
	const FVoxelSurfaceCoord Corner = Mode->WorldToSurface(Center + FVector(1.0, 1.0, 1.0).GetSafeNormal() * PlanetTestRadius);
	TestEqual(TEXT("A corner is at |U| = |V| = 1"), FMath::Abs(Corner.UV.X) + FMath::Abs(Corner.UV.Y), 2.0, 1.0e-6);

	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
