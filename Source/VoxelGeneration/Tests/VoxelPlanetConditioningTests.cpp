// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelData.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "SphericalPlanetWorldMode.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Spherical Phase 5: conditioning on a planet (WORLD_MODES_REVIEW_PLAN.md §19, WM-PL1; SP-B10, decision D15). A planet
// zone is a cap on the datum sphere: its weight goes by the great-circle distance from its direction (Strength inside
// InnerRadius, the heightmap's smoothstep across FalloffWidth, 0 beyond) and ApplyToAltitude blends the terrain's
// altitude toward its target. Each kind of zone is ignored by the other kind's functions.
// ---------------------------------------------------------------------------

namespace VoxelPlanetConditioningTestUtils
{
	/** The direction at a distance along the surface (on a sphere of Radius) from Direction, toward Bearing. */
	FVector DirectionAtDistance(const FVector& Direction, const FVector& Bearing, double Distance, double Radius)
	{
		const FVector Tangent = (Bearing - Direction * FVector::DotProduct(Bearing, Direction)).GetSafeNormal();
		const double Angle = Distance / Radius;
		return (Direction * FMath::Cos(Angle) + Tangent * FMath::Sin(Angle)).GetSafeNormal();
	}

	/** A planet like Review_Planet (R 250,000, PlanetHeightScale 6,000, the demo's terrain noise), no biomes. Rooted. */
	UVoxelWorldConfiguration* MakeConditioningPlanet()
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->AddToRoot();
		Config->WorldMode = EWorldMode::SphericalPlanet;
		Config->WorldOrigin = FVector::ZeroVector;
		Config->WorldRadius = 250000.0f;
		Config->PlanetHeightScale = 6000.0f;
		Config->BaseHeight = 0.0f;
		Config->PlanetMaxTerrainHeight = 6500.0f;
		Config->PlanetMaxTerrainDepth = 6500.0f;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 32;
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

	/** CPU-generated planet chunks (with the given zones on every request), read by trilinear density. */
	struct FConditionedPlanetVoxels
	{
		const UVoxelWorldConfiguration* Config = nullptr;
		TArray<FVoxelConditioningZone> Zones;
		FVoxelCPUNoiseGenerator Generator;
		TMap<FIntVector, TArray<FVoxelData>> Chunks;

		FConditionedPlanetVoxels(const UVoxelWorldConfiguration* InConfig, const TArray<FVoxelConditioningZone>& InZones)
			: Config(InConfig), Zones(InZones)
		{
		}

		const FVoxelData& Voxel(const FIntVector& Global)
		{
			const int32 N = Config->ChunkSize;
			const FIntVector ChunkCoord(FMath::FloorToInt(Global.X / static_cast<float>(N)), FMath::FloorToInt(Global.Y / static_cast<float>(N)),
				FMath::FloorToInt(Global.Z / static_cast<float>(N)));
			TArray<FVoxelData>* Data = Chunks.Find(ChunkCoord);
			if (!Data)
			{
				FVoxelNoiseGenerationRequest Request;
				Request.ChunkCoord = ChunkCoord;
				Request.ChunkSize = N;
				Request.VoxelSize = Config->VoxelSize;
				Request.LODLevel = 0;
				Request.NoiseParams = Config->NoiseParams;
				FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
				Request.ConditioningZones = Zones;
				Data = &Chunks.Add(ChunkCoord);
				Generator.GenerateChunkCPU(Request, *Data);
			}
			const FIntVector Local = Global - ChunkCoord * N;
			return (*Data)[Local.X + Local.Y * N + Local.Z * N * N];
		}

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

		/** The outermost density crossing along a direction within +-Window of Around (negative when none). */
		double SurfaceRadius(const FVector& Direction, double Around, double Window)
		{
			constexpr float Iso = 127.5f;
			const double Step = Config->VoxelSize * 0.02;
			const FVector& Center = Config->WorldOrigin;
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningRadialTest,
	"VoxelWorlds.Generation.Conditioning.Radial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningRadialTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetConditioningTestUtils;

	const double Radius = 250000.0;
	const float Inner = 4000.0f;
	const float Falloff = 1500.0f;
	const float Target = 320.0f;
	const TArray<TPair<const TCHAR*, FVector>> Sites = {
		{ TEXT("north pole"), FVector(0, 0, 1) },
		{ TEXT("south pole"), FVector(0, 0, -1) },
		{ TEXT("equator"), FVector(0, 1, 0) },
		{ TEXT("cube corner"), FVector(1, 1, 1).GetSafeNormal() },
	};

	for (const TPair<const TCHAR*, FVector>& Site : Sites)
	{
		const FVoxelConditioningZone Zone = FVoxelConditioningZone::MakeRadial(Site.Value * 3.0, Inner, Falloff, Target);
		TestTrue(*FString::Printf(TEXT("%s: a planet zone"), Site.Key), Zone.IsRadial());
		TestTrue(*FString::Printf(TEXT("%s: the direction is normalized"), Site.Key), FMath::IsNearlyEqual(Zone.Direction.Size(), 1.0, 1e-9));
		const TArray<FVoxelConditioningZone> Zones = { Zone };

		// Distances along the surface are what the weight reads, toward any bearing.
		const FVector Bearings[] = { FVector(1, 0, 0), FVector(0, 1, 0), FVector(0, 0, 1), FVector(1, -1, 0.3) };
		int32 Checked = 0;
		for (const FVector& RawBearing : Bearings)
		{
			const FVector Bearing = RawBearing.GetSafeNormal();
			if (FMath::Abs(FVector::DotProduct(Bearing, Site.Value)) > 0.99)
			{
				continue;
			}
			++Checked;
			const FVector AtCentre = Site.Value;
			const FVector Inside = DirectionAtDistance(Site.Value, Bearing, 0.9 * Inner, Radius);
			const FVector Mid = DirectionAtDistance(Site.Value, Bearing, Inner + 0.5 * Falloff, Radius);
			const FVector Beyond = DirectionAtDistance(Site.Value, Bearing, Inner + Falloff + 10.0, Radius);

			TestEqual(*FString::Printf(TEXT("%s: the centre reaches the target"), Site.Key),
				FVoxelTerrainConditioning::ApplyToAltitude(AtCentre, Radius, -900.0f, Zones), Target);
			TestEqual(*FString::Printf(TEXT("%s: inside the inner radius too"), Site.Key),
				FVoxelTerrainConditioning::ApplyToAltitude(Inside, Radius, -900.0f, Zones), Target);
			TestEqual(*FString::Printf(TEXT("%s: the ring's middle is the smoothstep's half"), Site.Key),
				Zone.GetWeightAtDirection(Mid, Radius), 0.5f, 1e-4f);
			TestEqual(*FString::Printf(TEXT("%s: beyond the ring, untouched"), Site.Key),
				FVoxelTerrainConditioning::ApplyToAltitude(Beyond, Radius, -900.0f, Zones), -900.0f);

			// The great-circle distance against the construction.
			TestEqual(*FString::Printf(TEXT("%s: the great-circle distance"), Site.Key),
				FVoxelConditioningZone::GreatCircleDistance(Mid, Site.Value, Radius), static_cast<double>(Inner + 0.5 * Falloff), 1e-3);
		}
		TestTrue(*FString::Printf(TEXT("%s: bearings checked"), Site.Key), Checked >= 3);

		// The cap test: a cap that the zone's ring reaches, and one it does not.
		const double ZoneAngle = Zone.GetAngularRadius(Radius);
		const FVector Bearing = FVector::CrossProduct(Site.Value, FVector(0.3, 0.5, 0.8)).GetSafeNormal();
		const FVector Near = DirectionAtDistance(Site.Value, Bearing, (Inner + Falloff) + 500.0, Radius);
		TestTrue(*FString::Printf(TEXT("%s: a cap of 600 uu 500 uu past the ring meets it"), Site.Key),
			Zone.MeetsCap(Near, 600.0 / Radius, Radius));
		TestFalse(*FString::Printf(TEXT("%s: a cap of 400 uu there does not"), Site.Key),
			Zone.MeetsCap(Near, 400.0 / Radius, Radius));
		TestTrue(*FString::Printf(TEXT("%s: the whole sphere meets it"), Site.Key), Zone.MeetsCap(-Site.Value, UE_DOUBLE_PI, Radius));
		TestEqual(*FString::Printf(TEXT("%s: the angular radius"), Site.Key), ZoneAngle, (Inner + Falloff) / Radius, 1e-12);
	}

	// Layering in order, and Strength, as on the plane.
	{
		const FVector D(0, 0, 1);
		const TArray<FVoxelConditioningZone> Zones = {
			FVoxelConditioningZone::MakeRadial(D, 1000.0f, 500.0f, 100.0f),
			FVoxelConditioningZone::MakeRadial(D, 1000.0f, 500.0f, 300.0f, 0.5f),
		};
		TestEqual(TEXT("Later zones blend on top of earlier ones"), FVoxelTerrainConditioning::ApplyToAltitude(D, Radius, 0.0f, Zones), 200.0f);
	}

	// Each kind ignores the other: a planet zone in ApplyToHeight and a heightmap zone in ApplyToAltitude contribute nothing.
	{
		const FVoxelConditioningZone Planet = FVoxelConditioningZone::MakeRadial(FVector(0, 0, 1), 1.0e6f, 0.0f, 999.0f);
		const FVoxelConditioningZone Plane(FVector2D::ZeroVector, 1.0e6f, 0.0f, 999.0f);
		TestFalse(TEXT("A heightmap zone is not radial"), Plane.IsRadial());
		TestEqual(TEXT("ApplyToHeight skips a planet zone"), FVoxelTerrainConditioning::ApplyToHeight(0.0, 0.0, 12.0f, { Planet }), 12.0f);
		TestEqual(TEXT("ApplyToAltitude skips a heightmap zone"), FVoxelTerrainConditioning::ApplyToAltitude(FVector(0, 0, 1), Radius, 12.0f, { Plane }), 12.0f);
		TestFalse(TEXT("A heightmap zone meets no cap"), Plane.MeetsCap(FVector(0, 0, 1), UE_DOUBLE_PI, Radius));
		TestEqual(TEXT("ApplyToHeight still applies a heightmap zone"), FVoxelTerrainConditioning::ApplyToHeight(0.0, 0.0, 12.0f, { Plane }), 999.0f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// WM-PL2: planet generation on the CPU takes the zones. On a planet like Review_Planet, a raised and a lowered zone at
// 45 degrees and at a cube corner: inside the inner radius every surface crossing along the radius is within half a voxel
// of the target altitude, the ring lies between the natural and the target altitude, and beyond the ring the crossings
// equal the unconditioned planet's exactly.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConditioningFlattensPlanetTest,
	"VoxelWorlds.Generation.Conditioning.FlattensPlanet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConditioningFlattensPlanetTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetConditioningTestUtils;

	UVoxelWorldConfiguration* Config = MakeConditioningPlanet();
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FSphericalPlanetWorldMode& Planet = static_cast<const FSphericalPlanetWorldMode&>(*Mode);
	const double Radius = Config->WorldRadius;
	const float VoxelSize = Config->VoxelSize;
	const float Inner = 1500.0f;
	const float Falloff = 1000.0f;

	const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
	const TArray<TPair<const TCHAR*, FVector>> Sites = {
		{ TEXT("45 degrees"), FVector(C45, 0, C45) },
		{ TEXT("cube corner"), FVector(1, 1, 1).GetSafeNormal() },
	};
	FConditionedPlanetVoxels Natural(Config, {});
	double WorstInside = 0.0;
	int32 Crossings = 0;
	for (const TPair<const TCHAR*, FVector>& Site : Sites)
	{
		for (const float Shift : { 400.0f, -400.0f })
		{
			const float Target = Planet.GetTerrainAltitude(Site.Value, Config->NoiseParams) + Shift;
			FConditionedPlanetVoxels Conditioned(Config, { FVoxelConditioningZone::MakeRadial(Site.Value, Inner, Falloff, Target) });
			const FVector Bearings[] = { FVector(0, 1, 0), FVector(-1, 0.4, 0.2), FVector(0.2, -1, 0.5) };
			for (const FVector& Bearing : Bearings)
			{
				// Inside the inner radius: the target.
				for (const double Distance : { 0.0, 700.0, 1400.0 })
				{
					const FVector D = DirectionAtDistance(Site.Value, Bearing, Distance, Radius);
					const double R = Conditioned.SurfaceRadius(D, Radius + Target, 600.0);
					const double Error = FMath::Abs(R - (Radius + Target));
					WorstInside = FMath::Max(WorstInside, Error);
					++Crossings;
					TestTrue(*FString::Printf(TEXT("%s, shift %.0f, %.0f uu out: the surface at the target (off by %.1f uu)"), Site.Key, Shift, Distance, Error),
						R > 0.0 && Error <= 0.5 * VoxelSize);
				}
				// The ring's middle: between the natural and the target altitude.
				{
					const FVector D = DirectionAtDistance(Site.Value, Bearing, Inner + 0.5 * Falloff, Radius);
					const double NaturalAltitude = Planet.GetTerrainAltitude(D, Config->NoiseParams);
					const double Expected = FMath::Lerp(NaturalAltitude, static_cast<double>(Target), 0.5);
					const double R = Conditioned.SurfaceRadius(D, Radius + Expected, 600.0);
					TestTrue(*FString::Printf(TEXT("%s, shift %.0f: the ring's middle is half way (%.1f uu off)"), Site.Key, Shift, R - (Radius + Expected)),
						R > 0.0 && FMath::Abs(R - (Radius + Expected)) <= 0.5 * VoxelSize);
				}
				// Beyond the ring: the unconditioned planet, voxel for voxel.
				{
					const FVector D = DirectionAtDistance(Site.Value, Bearing, Inner + Falloff + 2.0 * VoxelSize, Radius);
					const double Around = Radius + Planet.GetTerrainAltitude(D, Config->NoiseParams);
					const double R = Conditioned.SurfaceRadius(D, Around, 300.0);
					const double R0 = Natural.SurfaceRadius(D, Around, 300.0);
					TestTrue(*FString::Printf(TEXT("%s, shift %.0f: beyond the ring the natural surface"), Site.Key, Shift), R > 0.0 && R == R0);
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d crossings inside the inner radius: worst %.2f uu from the target"), Crossings, WorstInside));
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
