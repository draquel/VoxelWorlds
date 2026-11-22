// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelChunkValleySegments.h"
#include "VoxelCPUNoiseGenerator.h"
#include "VoxelData.h"
#include "VoxelNoiseTypes.h"
#include "VoxelSurfaceQuery.h"
#include "VoxelValleyNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The valleys in the height (Documentation/VALLEYS_PLAN.md, section 6.2; phase V2).
//
// - Generation.Valleys.Carve: on made hills falling to a sea, the network's point query (the world modes') and a
//   chunk's segment list (the generators') give the same height at every column of chunks over valleys; on a floor's
//   middle the height is the floor; away from every valley and under the sea the height is the natural one; along lines
//   through the valleys the height moves with the ground and the floors, with no step.
// - Generation.HeightParity.Valleys: on a plane world with valleys, the analytic height (GetTerrainHeightAt) sits on the
//   surface the CPU generator makes, within a voxel, on valley floors, sides and heads.
// - Generation.Valleys.Slab: a column's surface range (GetSurfaceRange) never lies over the lowest carved height in its
//   footprint, sampled densely along valleys.
// The GPU's parity with valleys is VoxelWorlds.Generation.GPUvsCPUValleyParity (VoxelNoiseGeneratorTests.cpp).
// ---------------------------------------------------------------------------

namespace VoxelValleyCarveTest
{
	double Hills(double X, double Y)
	{
		return 6000.0 - 0.01 * FMath::Abs(X) + 3000.0 * FMath::Sin(X / 80000.0) * FMath::Cos(Y / 65000.0) + 1500.0 * FMath::Sin((X - Y) / 31000.0);
	}

	TSharedRef<FVoxelValleyNetwork> MakeHillsNetwork()
	{
		FVoxelValleyGround Ground;
		Ground.Height = &Hills;
		Ground.bSea = true;
		Ground.SeaLevel = 0.0;
		Ground.Seed = 1234;
		return MakeShared<FVoxelValleyNetwork>(FVoxelValleySettings(), Ground);
	}

	/** The middles of the valley segments on full floors over tiles around the origin. */
	TArray<FVoxelValleySegment> FloorSegments(const FVoxelValleyNetwork& Network, int32 TileRange)
	{
		TArray<FVoxelValleySegment> Out;
		for (int32 TY = -TileRange; TY < TileRange; ++TY)
		{
			for (int32 TX = -TileRange; TX < TileRange; ++TX)
			{
				for (const FVoxelValleySegment& Segment : Network.GetTile(FIntPoint(TX, TY))->Segments)
				{
					if (Segment.TaperA >= 1.0f && Segment.TaperB >= 1.0f)
					{
						Out.Add(Segment);
					}
				}
			}
		}
		return Out;
	}

	/** The plane world the height parity and the GPU test generate: wavelengths of kilometres, 100 m of relief, no sea. */
	FVoxelNoiseParams MakeNoise()
	{
		FVoxelNoiseParams P;
		P.NoiseType = EVoxelNoiseType::Simplex;
		P.Seed = 248675;
		P.Frequency = 6e-6f;
		P.Octaves = 5;
		P.Lacunarity = 2.0f;
		P.Persistence = 0.5f;
		return P;
	}

	FWorldModeTerrainParams MakeTerrain() { return FWorldModeTerrainParams(0.0f, 8000.0f, 3000.0f); }

	TSharedRef<FVoxelValleyNetwork> MakePlaneNetwork()
	{
		FVoxelValleyGround Ground;
		const FVoxelNoiseParams Noise = MakeNoise();
		const FWorldModeTerrainParams Terrain = MakeTerrain();
		Ground.Height = [Noise, Terrain](double X, double Y)
		{
			return static_cast<double>(FInfinitePlaneWorldMode::SampleTerrainHeight(static_cast<float>(X), static_cast<float>(Y), Terrain, Noise, nullptr));
		};
		Ground.bSea = false;
		Ground.SeaLevel = 0.0;
		Ground.Seed = MakeNoise().Seed;
		return MakeShared<FVoxelValleyNetwork>(FVoxelValleySettings(), Ground);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleyCarveTest,
	"VoxelWorlds.Generation.Valleys.Carve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleyCarveTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyCarveTest;
	const TSharedRef<FVoxelValleyNetwork> Network = MakeHillsNetwork();
	const TArray<FVoxelValleySegment> Floors = FloorSegments(*Network, 2);
	TestTrue(TEXT("the hills have valley floors"), Floors.Num() > 20);

	// The chunk's list and the point query: every column of chunks over a few valley floors
	const int32 ChunkSize = 32;
	const float VoxelSize = 75.0f;
	const double ChunkWorldSize = ChunkSize * VoxelSize;
	double MostApart = 0.0;
	int32 Columns = 0, Carved = 0;
	for (int32 K = 0; K < Floors.Num(); K += FMath::Max(1, Floors.Num() / 8))
	{
		const FVector2D Middle = 0.5 * (Floors[K].A + Floors[K].B);
		FVoxelNoiseGenerationRequest Request;
		Request.ChunkCoord = FIntVector(FMath::FloorToInt32(Middle.X / ChunkWorldSize), FMath::FloorToInt32(Middle.Y / ChunkWorldSize), 0);
		Request.ChunkSize = ChunkSize;
		Request.VoxelSize = VoxelSize;
		Request.WorldMode = EWorldMode::InfinitePlane;
		Request.Valleys = Network;
		FVoxelChunkValleySegments Segments;
		FVoxelChunkValleySegments::Build(Request, Segments);
		FVoxelChunkValleyColumns ValleyColumns;
		Segments.EvaluateColumns(ChunkSize, VoxelSize, ValleyColumns);
		const FVector Corner = Request.GetChunkWorldPosition();
		for (int32 Y = 0; Y < ChunkSize; Y += 3)
		{
			for (int32 X = 0; X < ChunkSize; X += 3)
			{
				const double WX = Corner.X + X * VoxelSize, WY = Corner.Y + Y * VoxelSize;
				const float Natural = static_cast<float>(Hills(WX, WY));
				const float FromChunk = ValleyColumns.IsEmpty() ? Natural : ValleyColumns.Apply(X + Y * ChunkSize, Natural);
				const float FromList = Segments.Apply(X * VoxelSize, Y * VoxelSize, Natural);
				const double FromPoint = Network->ApplyToHeight(WX, WY, Natural);
				MostApart = FMath::Max3(MostApart, FMath::Abs(FromChunk - FromPoint), static_cast<double>(FMath::Abs(FromChunk - FromList)));
				++Columns;
				Carved += FMath::Abs(FromChunk - Natural) > 1.0f ? 1 : 0;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d columns, %d carved, the point query and the chunk's list at most %.4f uu apart"), Columns, Carved, MostApart));
	TestTrue(TEXT("the columns over valley floors are carved"), Carved > Columns / 2);
	TestTrue(TEXT("the point query and the chunk's list agree within 0.05 uu"), MostApart < 0.05);

	// On a floor's middle the height is the floor, unless another valley there cuts deeper
	int32 OnFloor = 0, Deeper = 0, Off = 0;
	for (const FVoxelValleySegment& Segment : Floors)
	{
		const FVector2D Middle = 0.5 * (Segment.A + Segment.B);
		const double Natural = Hills(Middle.X, Middle.Y);
		if (Natural < 0.0)
		{
			continue;
		}
		const double Floor = 0.5 * (Segment.FloorA + Segment.FloorB);
		const double Height = Network->ApplyToHeight(Middle.X, Middle.Y, Natural);
		if (FMath::Abs(Height - Floor) < 1.0)
		{
			++OnFloor;
		}
		else if (Height < Floor)
		{
			++Deeper;
		}
		else
		{
			++Off;
		}
	}
	// Where valleys overlap the deepest cut wins, and where two fill the higher fill: a middle stands over its own floor
	// only on another valley's fill
	AddInfo(FString::Printf(TEXT("floor middles: %d on the floor, %d cut deeper by another valley, %d on another's fill"), OnFloor, Deeper, Off));
	TestTrue(TEXT("floor middles hold their floor or lie deeper (95 %)"), OnFloor + Deeper >= 0.95 * (OnFloor + Deeper + Off));
	TestTrue(TEXT("floor middles hold their floor"), OnFloor > 0);

	// Under the sea, and away from every valley, the natural height
	int32 Moved = 0, Away = 0;
	for (int32 K = 0; K < 2000; ++K)
	{
		const double X = -900000.0 + 900.0 * K, Y = 37000.0 + 210.0 * K;
		const double Natural = Hills(X, Y);
		TArray<FVoxelValleySegment> Near;
		Network->GatherSegments(FBox2D(FVector2D(X, Y), FVector2D(X, Y)), Near);
		if (Natural < 0.0 || Near.Num() == 0)
		{
			++Away;
			Moved += Network->ApplyToHeight(X, Y, Natural) != Natural ? 1 : 0;
		}
	}
	TestTrue(TEXT("points away from the valleys or under the sea were sampled"), Away > 100);
	TestEqual(TEXT("away from every valley and under the sea the height is the natural one"), Moved, 0);

	// No step: along lines through the valleys, the carve's change between two points 30 uu apart stays small
	double WorstStep = 0.0;
	for (int32 Line = 0; Line < 6; ++Line)
	{
		const double Y = -200000.0 + 70000.0 * Line;
		double PrevCarve = 0.0;
		for (int32 Step = 0; Step <= 20000; ++Step)
		{
			const double X = -300000.0 + 30.0 * Step;
			const double Natural = Hills(X, Y);
			const double Carve = Network->ApplyToHeight(X, Y, Natural) - Natural;
			if (Step > 0 && Natural >= 0.0)
			{
				WorstStep = FMath::Max(WorstStep, FMath::Abs(Carve - PrevCarve));
			}
			PrevCarve = Carve;
		}
	}
	AddInfo(FString::Printf(TEXT("the carve's largest change over 30 uu: %.1f uu"), WorstStep));
	TestTrue(TEXT("the carve has no step (under 120 uu over 30 uu)"), WorstStep < 120.0);

	// The point query's cost near valleys, its tiles and reaches already kept (a measure, not a limit)
	{
		double Sink = 0.0;
		const int32 Samples = 200000;
		const double T0 = FPlatformTime::Seconds();
		for (int32 K = 0; K < Samples; ++K)
		{
			const FVoxelValleySegment& Segment = Floors[K % Floors.Num()];
			const FVector2D P = FMath::Lerp(Segment.A, Segment.B, (K % 7) / 7.0) + FVector2D(K % 13 * 900.0, 0.0);
			Sink += Network->ApplyToHeight(P.X, P.Y, 3000.0);
		}
		AddInfo(FString::Printf(TEXT("the point query near valleys: %.3f us a sample (%d samples, %.0f)"), (FPlatformTime::Seconds() - T0) * 1.0e6 / Samples, Samples, Sink * 0.0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelHeightValleyParityTest,
	"VoxelWorlds.Generation.HeightParity.Valleys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelHeightValleyParityTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyCarveTest;
	const TSharedRef<FVoxelValleyNetwork> Network = MakePlaneNetwork();
	const FVoxelNoiseParams Noise = MakeNoise();
	const FWorldModeTerrainParams Terrain = MakeTerrain();
	FInfinitePlaneWorldMode Analytic(Terrain);
	Analytic.SetValleyNetwork(Network);

	FVoxelCPUNoiseGenerator Generator;
	Generator.Initialize();
	const int32 ChunkSize = 32;
	const float VoxelSize = 75.0f;
	const float ChunkWorldSize = ChunkSize * VoxelSize;

	// The real surface in a column: three chunks around the expected height
	auto RealSurface = [&](float X, float Y, float Expected) -> float
	{
		const int32 CX = FMath::FloorToInt(X / ChunkWorldSize), CY = FMath::FloorToInt(Y / ChunkWorldSize);
		const int32 CZ = FMath::FloorToInt(Expected / ChunkWorldSize) - 1;
		const int32 Lx = FMath::Clamp(FMath::RoundToInt((X - CX * ChunkWorldSize) / VoxelSize), 0, ChunkSize - 1);
		const int32 Ly = FMath::Clamp(FMath::RoundToInt((Y - CY * ChunkWorldSize) / VoxelSize), 0, ChunkSize - 1);
		TArray<FVoxelData> Column;
		for (int32 Z = 0; Z < 3; ++Z)
		{
			FVoxelNoiseGenerationRequest Request;
			Request.ChunkCoord = FIntVector(CX, CY, CZ + Z);
			Request.ChunkSize = ChunkSize;
			Request.VoxelSize = VoxelSize;
			Request.WorldMode = EWorldMode::InfinitePlane;
			Request.SeaLevel = Terrain.SeaLevel;
			Request.HeightScale = Terrain.HeightScale;
			Request.BaseHeight = Terrain.BaseHeight;
			Request.bEnableBiomes = false;
			Request.bEnableCaves = false;
			Request.NoiseParams = Noise;
			Request.Valleys = Network;
			TArray<FVoxelData> Data;
			if (!Generator.GenerateChunkCPU(Request, Data))
			{
				return FLT_MAX;
			}
			for (int32 Lz = 0; Lz < ChunkSize; ++Lz)
			{
				Column.Add(Data[Lx + Ly * ChunkSize + Lz * ChunkSize * ChunkSize]);
			}
		}
		float Height = 0.0f;
		uint8 Material = 0, Biome = 0;
		return FVoxelSurfaceQuery::ExtractSurfaceFromColumn(Column, CZ * ChunkWorldSize, VoxelSize, Height, Material, Biome) ? Height : FLT_MAX;
	};

	// Points on floors, on sides and at heads: voxel-aligned, where the carve moves the ground
	TArray<FVector2D> Points;
	for (int32 TY = -2; TY < 2 && Points.Num() < 24; ++TY)
	{
		for (int32 TX = -2; TX < 2 && Points.Num() < 24; ++TX)
		{
			const TSharedRef<const FVoxelValleyTile> Tile = Network->GetTile(FIntPoint(TX, TY));
			for (int32 K = 0; K < Tile->Segments.Num() && Points.Num() < 24; K += 17)
			{
				const FVoxelValleySegment& Segment = Tile->Segments[K];
				const FVector2D Middle = 0.5 * (Segment.A + Segment.B);
				const FVector2D Across = (Segment.B - Segment.A).GetSafeNormal().GetRotated(90.0);
				for (const double Offset : { 0.0, 0.5 * (Segment.FloorHalfWidth + Segment.ValleyHalfWidth) })
				{
					const FVector2D P = Middle + Across * Offset;
					Points.Add(FVector2D(FMath::RoundToDouble(P.X / VoxelSize) * VoxelSize, FMath::RoundToDouble(P.Y / VoxelSize) * VoxelSize));
				}
			}
		}
	}
	TestTrue(TEXT("valley points were found"), Points.Num() >= 12);
	int32 Moved = 0;
	double Worst = 0.0;
	for (const FVector2D& P : Points)
	{
		const float Analytic3 = Analytic.GetTerrainHeightAt(static_cast<float>(P.X), static_cast<float>(P.Y), Noise);
		const float Natural = FInfinitePlaneWorldMode::SampleTerrainHeight(static_cast<float>(P.X), static_cast<float>(P.Y), Terrain, Noise, nullptr);
		Moved += FMath::Abs(Analytic3 - Natural) > VoxelSize ? 1 : 0;
		const float Real = RealSurface(static_cast<float>(P.X), static_cast<float>(P.Y), Analytic3);
		if (!TestTrue(FString::Printf(TEXT("a surface near (%.0f, %.0f)"), P.X, P.Y), Real != FLT_MAX))
		{
			continue;
		}
		Worst = FMath::Max(Worst, static_cast<double>(FMath::Abs(Analytic3 - Real)));
		TestTrue(FString::Printf(TEXT("(%.0f, %.0f): analytic %.1f on the real surface %.1f within a voxel"), P.X, P.Y, Analytic3, Real), FMath::Abs(Analytic3 - Real) < VoxelSize);
	}
	AddInfo(FString::Printf(TEXT("%d points, %d moved by a voxel or more from the natural height, the worst |analytic - real| %.2f voxels"), Points.Num(), Moved, Worst / VoxelSize));
	TestTrue(TEXT("the valleys move the ground at most of the points (non-vacuous)"), Moved > Points.Num() / 3);
	Generator.Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelValleySlabTest,
	"VoxelWorlds.Generation.Valleys.Slab",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelValleySlabTest::RunTest(const FString& Parameters)
{
	using namespace VoxelValleyCarveTest;
	const TSharedRef<FVoxelValleyNetwork> Network = MakePlaneNetwork();
	const FVoxelNoiseParams Noise = MakeNoise();
	FInfinitePlaneWorldMode Mode(MakeTerrain());
	Mode.SetValleyNetwork(Network);
	const double Column = 32 * 75.0;

	// Footprints along valleys, each straddling the centre curve; the lowest carved height in each, sampled every 75 uu
	int32 Footprints = 0, Under = 0;
	for (int32 TY = -1; TY < 1; ++TY)
	{
		for (int32 TX = -1; TX < 1; ++TX)
		{
			const TSharedRef<const FVoxelValleyTile> Tile = Network->GetTile(FIntPoint(TX, TY));
			for (int32 K = 0; K < Tile->Segments.Num(); K += 23)
			{
				const FVector2D Middle = 0.5 * (Tile->Segments[K].A + Tile->Segments[K].B);
				const FVector2D Min(FMath::FloorToDouble(Middle.X / Column) * Column, FMath::FloorToDouble(Middle.Y / Column) * Column);
				const FVector2D Max = Min + FVector2D(Column);
				const FFloatInterval Range = Mode.GetSurfaceRange(Min, Max, Noise);
				float Lowest = MAX_flt;
				for (double Y = Min.Y; Y <= Max.Y; Y += 75.0)
				{
					for (double X = Min.X; X <= Max.X; X += 75.0)
					{
						Lowest = FMath::Min(Lowest, Mode.GetTerrainHeightAt(static_cast<float>(X), static_cast<float>(Y), Noise));
					}
				}
				++Footprints;
				Under += Lowest < Range.Min - 1.0f ? 1 : 0;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d footprints over valleys"), Footprints));
	TestTrue(TEXT("footprints over valleys were tried"), Footprints > 10);
	TestEqual(TEXT("no footprint's carved ground lies under its surface range"), Under, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
