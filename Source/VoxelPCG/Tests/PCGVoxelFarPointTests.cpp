// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelNoiseTypes.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelSeaSurface.h"
#include "VoxelChunkManager.h"
#include "IVoxelWorldMode.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * World modes review G23 (WORLD_MODES_REVIEW_PLAN.md §14.1): the Voxel Surface Sampler's far points on a heightmap (beyond
 * its edit-aware band) stand on the generated ground. Real InfinitePlane terrain from the CPU generator with two
 * conditioning zones on a chunk manager, a pad cut 1,500 uu below the natural height at its centre and one raised 1,500 uu
 * over it. At 8 azimuths x 9 radii around each (the pad, the falloff ring and beyond), with the zones the sampler gathers
 * over its bounds (UVoxelChunkManager::GatherConditioningZones):
 * - FVoxelSurfaceQuery::SampleSurface lands within a voxel of the generated isosurface, and SampleSurfacePoint (which
 *   heightmap modes forward to it) returns the same sample;
 * - the pad reads flat (slope under a degree) and beyond the ring the zones change nothing;
 * - without the zones (the natural height, the sampler's far points before the fix) points in the pad and the ring miss
 *   the ground by more than three voxels.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGVoxelFarPointsConditionedTest,
	"VoxelWorlds.PCG.FarPointsOnConditionedSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCGVoxelFarPointsConditionedTest::RunTest(const FString& Parameters)
{
	constexpr int32 ChunkSize = 32;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr float InnerRadius = 3000.0f;
	constexpr float FalloffWidth = 4000.0f;

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->ChunkSize = ChunkSize;
	Config->VoxelSize = VoxelSize;
	Config->HeightScale = 8000.0f;
	Config->bEnableBiomes = false;
	Config->bEnableCaves = false;
	Config->bEnableWaterLevel = false;
	Config->NoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	Config->NoiseParams.Seed = 4325;
	Config->NoiseParams.Frequency = 0.000025f;
	Config->NoiseParams.Amplitude = 1.0f;
	Config->NoiseParams.Octaves = 6;
	Config->NoiseParams.Lacunarity = 2.0f;
	Config->NoiseParams.Persistence = 0.5f;
	const TSharedRef<IVoxelWorldMode> Mode = FVoxelWorldModeFactory::Create(*Config);
	const FVoxelNoiseParams& Noise = Config->NoiseParams;
	const FVoxelBiomeSnapshot NoBiomes;
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::FromConfig(*Config);

	// The zones live on a chunk manager, where the sampler gathers them.
	UVoxelChunkManager* Manager = NewObject<UVoxelChunkManager>();
	Manager->AddToRoot();
	const FVector2D CutCentre(40000.0, -25000.0);
	const FVector2D RaisedCentre(-60000.0, 70000.0);
	Manager->AddConditioningZone(FVoxelConditioningZone(CutCentre, InnerRadius, FalloffWidth,
		Mode->GetTerrainHeightAt(static_cast<float>(CutCentre.X), static_cast<float>(CutCentre.Y), Noise) - 1500.0f, 1.0f));
	Manager->AddConditioningZone(FVoxelConditioningZone(RaisedCentre, InnerRadius, FalloffWidth,
		Mode->GetTerrainHeightAt(static_cast<float>(RaisedCentre.X), static_cast<float>(RaisedCentre.Y), Noise) + 1500.0f, 1.0f));

	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();
	TMap<FIntVector, TArray<FVoxelData>> Chunks;
	auto GetChunk = [&](const FIntVector& Coord, const TArray<FVoxelConditioningZone>& Zones) -> const TArray<FVoxelData>&
	{
		if (const TArray<FVoxelData>* Found = Chunks.Find(Coord))
		{
			return *Found;
		}
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = Coord;
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = VoxelSize;
		Request.NoiseParams = Noise;
		FVoxelWorldModeFactory::FillGenerationRequest(*Config, Request);
		Request.ConditioningZones = Zones;
		TArray<FVoxelData> Voxels;
		Generator.GenerateChunkCPU(Request, Voxels);
		return Chunks.Add(Coord, MoveTemp(Voxels));
	};
	// The generated isosurface in the voxel column at (X, Y) on the voxel grid, over three chunk layers around ExpectedZ.
	auto RealSurfaceZ = [&](double X, double Y, float ExpectedZ, const TArray<FVoxelConditioningZone>& Zones)
	{
		const int32 CX = FMath::FloorToInt(X / ChunkWorldSize);
		const int32 CY = FMath::FloorToInt(Y / ChunkWorldSize);
		const int32 LX = FMath::RoundToInt((X - CX * ChunkWorldSize) / VoxelSize);
		const int32 LY = FMath::RoundToInt((Y - CY * ChunkWorldSize) / VoxelSize);
		const int32 BaseCZ = FMath::FloorToInt(ExpectedZ / ChunkWorldSize) - 1;
		TArray<FVoxelData> Column;
		for (int32 Layer = 0; Layer < 3; ++Layer)
		{
			const TArray<FVoxelData>& Voxels = GetChunk(FIntVector(CX, CY, BaseCZ + Layer), Zones);
			for (int32 LZ = 0; LZ < ChunkSize; ++LZ)
			{
				Column.Add(Voxels[LX + LY * ChunkSize + LZ * ChunkSize * ChunkSize]);
			}
		}
		float Height = TNumericLimits<float>::Max();
		uint8 Material = 0, Biome = 0;
		FVoxelSurfaceQuery::ExtractSurfaceFromColumn(Column, static_cast<float>(BaseCZ) * ChunkWorldSize, VoxelSize, Height, Material, Biome);
		return Height;
	};

	for (const TPair<const TCHAR*, FVector2D>& Site : { TPair<const TCHAR*, FVector2D>(TEXT("cut pad"), CutCentre), TPair<const TCHAR*, FVector2D>(TEXT("raised pad"), RaisedCentre) })
	{
		// The sampler's gather: the zones over its bounds (here a box around the site's footprint).
		const double Reach = InnerRadius + FalloffWidth + 1000.0;
		TArray<FVoxelConditioningZone> Zones;
		Manager->GatherConditioningZones(FBox2D(Site.Value - FVector2D(Reach), Site.Value + FVector2D(Reach)), Zones);
		TestEqual(*FString::Printf(TEXT("%s: the sampler's gather finds the site's zone"), Site.Key), Zones.Num(), 1);

		Chunks.Reset();
		int32 Points = 0, Off = 0, PointDiffers = 0, Sloped = 0, Changed = 0;
		float WorstWith = 0.0f, WorstNatural = 0.0f;
		for (const float Radius : { 0.0f, 1000.0f, 2000.0f, 2900.0f, 3500.0f, 4500.0f, 5500.0f, 6500.0f, 7600.0f })
		{
			for (int32 Azimuth = 0; Azimuth < 8; ++Azimuth)
			{
				const double Angle = Azimuth * UE_DOUBLE_PI / 4.0;
				const double X = FMath::RoundToDouble((Site.Value.X + Radius * FMath::Cos(Angle)) / VoxelSize) * VoxelSize;
				const double Y = FMath::RoundToDouble((Site.Value.Y + Radius * FMath::Sin(Angle)) / VoxelSize) * VoxelSize;
				const FVoxelSurfaceSample With = FVoxelSurfaceQuery::SampleSurface(*Mode, static_cast<float>(X), static_cast<float>(Y), VoxelSize, Noise, NoBiomes, Noise.Seed, false, 0.0f, &Zones);
				const FVoxelSurfaceSample Natural = FVoxelSurfaceQuery::SampleSurface(*Mode, static_cast<float>(X), static_cast<float>(Y), VoxelSize, Noise, NoBiomes, Noise.Seed, false, 0.0f);
				const FVoxelSurfaceSample Point = FVoxelSurfaceQuery::SampleSurfacePoint(*Mode, FVector(X, Y, 0.0), VoxelSize, Noise, NoBiomes, Sea, &Zones);
				const float Real = RealSurfaceZ(X, Y, With.Height, Zones);
				if (!TestTrue(*FString::Printf(TEXT("%s: a generated surface at (%.0f, %.0f)"), Site.Key, X, Y), Real != TNumericLimits<float>::Max()))
				{
					continue;
				}
				++Points;
				const float ErrWith = FMath::Abs(With.Height - Real);
				const float ErrNatural = FMath::Abs(Natural.Height - Real);
				WorstWith = FMath::Max(WorstWith, ErrWith);
				Off += ErrWith >= VoxelSize ? 1 : 0;
				PointDiffers += (Point.Height != With.Height || !Point.Position.Equals(FVector(X, Y, With.Height), 0.01)) ? 1 : 0;
				if (Radius < InnerRadius + FalloffWidth)
				{
					WorstNatural = FMath::Max(WorstNatural, ErrNatural);
				}
				else
				{
					Changed += FMath::Abs(With.Height - Natural.Height) > 0.01f ? 1 : 0;
				}
				Sloped += (Radius <= 2000.0f && With.SlopeDegrees >= 1.0f) ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %d points; far point vs the generated surface: worst %.1f uu with the zones, %.1f uu at the natural height (pad + ring)"),
			Site.Key, Points, WorstWith, WorstNatural));
		TestEqual(*FString::Printf(TEXT("%s: every point found the generated surface"), Site.Key), Points, 72);
		TestEqual(*FString::Printf(TEXT("%s: every far point with the zones is within a voxel of the generated surface"), Site.Key), Off, 0);
		TestEqual(*FString::Printf(TEXT("%s: SampleSurfacePoint forwards the zones on a heightmap"), Site.Key), PointDiffers, 0);
		TestEqual(*FString::Printf(TEXT("%s: the pad reads flat"), Site.Key), Sloped, 0);
		TestEqual(*FString::Printf(TEXT("%s: beyond the ring the zones change nothing"), Site.Key), Changed, 0);
		TestTrue(*FString::Printf(TEXT("%s: at the natural height the far points miss the ground by more than three voxels (the test reproduces G23)"), Site.Key),
			WorstNatural > 3.0f * VoxelSize);
	}

	Generator.Shutdown();
	Manager->RemoveFromRoot();
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
