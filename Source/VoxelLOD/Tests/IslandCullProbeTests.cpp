// Copyright Daniel Raquel. All Rights Reserved.

// Review B probe (Documentation/WORLD_MODES_REVIEW_PLAN.md §5 I1, finding KI3): where the LOD strategy's island cull
// keeps chunks, against where generation puts the island. Until WM-A1 generation tested the absolute voxel position
// against the raw IslandCenterX/Y while the cull centred the island on WorldOrigin + IslandCenterX/Y, so with the
// demo's origin 1,098 land columns were culled. Both now take their params from FVoxelWorldModeFactory, and the
// probe asserts: no column that holds island terrain is culled, whatever WorldOrigin is.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "IslandBowlWorldMode.h"
#include "VoxelNoiseTypes.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelWorldModeFactory.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace IslandCullProbe
{
	/** Exposes the protected cull for the probe. */
	class FProbeStrategy : public FDistanceBandLODStrategy
	{
	public:
		using FDistanceBandLODStrategy::ShouldCullIslandBoundary;
	};

	/** Island M at the demo's geometry: 4,800 uu chunks, R 200,000, F 50,000. */
	UVoxelWorldConfiguration* MakeIslandConfig(const FVector& WorldOrigin, float CenterX, float CenterY)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = EWorldMode::IslandBowl;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->ViewDistance = 40000.0f;
		Config->WorldOrigin = WorldOrigin;
		Config->IslandShape = 0;
		Config->IslandRadius = 200000.0f;
		Config->IslandFalloffWidth = 50000.0f;
		Config->IslandCenterX = CenterX;
		Config->IslandCenterY = CenterY;
		Config->IslandEdgeHeight = -3500.0f;
		Config->LODBands = {
			FLODBand(0.0f, 7000.0f, 0, 1),
			FLODBand(7000.0f, 14000.0f, 1, 2),
			FLODBand(14000.0f, 40000.0f, 2, 4) };
		return Config;
	}

	struct FCullCounts
	{
		int32 KeptWithLand = 0;      // cull keeps it and generation has island there
		int32 CulledWithLand = 0;    // cull drops a column that holds island terrain: missing terrain
		int32 KeptWithoutLand = 0;   // cull keeps a column generation leaves as the outer floor
		float MaxMissingDistance = 0.0f; // farthest-in culled land column, distance from the generation centre
		int32 KeptOutsideFootprint = 0;  // kept columns whose box does not touch the footprint at all (WM-E1: none)
		FVector2D GenerationCentre = FVector2D::ZeroVector; // where generation centres the island
	};

	/** Every chunk column within 300,000 uu of the generation centre, classified by the cull and by generation. */
	FCullCounts Classify(const UVoxelWorldConfiguration& Config)
	{
		FProbeStrategy Strategy;
		Strategy.Initialize(&Config);
		FLODQueryContext Context;
		Context.WorldOrigin = Config.WorldOrigin;
		Context.WorldMode = EWorldMode::IslandBowl;
		Context.ViewerPosition = Config.WorldOrigin;
		Strategy.Update(Context, 1.0f / 60.0f);

		// Generation's island: the params a chunk's generation request carries.
		FVoxelNoiseGenerationRequest Request;
		FVoxelWorldModeFactory::FillGenerationRequest(Config, Request);
		const FIslandBowlParams& Island = Request.IslandParams;

		const float ChunkWorldSize = Config.ChunkSize * Config.VoxelSize;
		const FVector2D GenCentre(Island.CenterX, Island.CenterY);
		const int32 Range = FMath::CeilToInt(300000.0f / ChunkWorldSize);
		const int32 CX = FMath::FloorToInt((GenCentre.X - Config.WorldOrigin.X) / ChunkWorldSize);
		const int32 CY = FMath::FloorToInt((GenCentre.Y - Config.WorldOrigin.Y) / ChunkWorldSize);

		FCullCounts Counts;
		Counts.GenerationCentre = GenCentre;
		for (int32 X = CX - Range; X <= CX + Range; ++X)
		{
			for (int32 Y = CY - Range; Y <= CY + Range; ++Y)
			{
				const FIntVector Coord(X, Y, 0);
				const bool bCulled = Strategy.ShouldCullIslandBoundary(Coord, Context);
				// Generation's view: does any corner or the centre of the column lie inside R+F (absolute position)?
				const FVector2D Min(Config.WorldOrigin.X + X * ChunkWorldSize, Config.WorldOrigin.Y + Y * ChunkWorldSize);
				bool bLand = false;
				for (const FVector2D& Offset : { FVector2D(0, 0), FVector2D(1, 0), FVector2D(0, 1), FVector2D(1, 1), FVector2D(0.5, 0.5) })
				{
					const FVector2D P = Min + Offset * ChunkWorldSize;
					bLand |= FIslandBowlWorldMode::IsWithinIslandBounds(P.X, P.Y, Island);
				}
				if (bLand && !bCulled) { ++Counts.KeptWithLand; }
				if (bLand && bCulled)
				{
					++Counts.CulledWithLand;
					const FVector2D C = Min + FVector2D(0.5, 0.5) * ChunkWorldSize;
					// Distance inside the coastline's outer edge: how deep into the island the cull bites.
					Counts.MaxMissingDistance = FMath::Max(Counts.MaxMissingDistance,
						(Island.IslandRadius + Island.FalloffWidth) - FVector2D::Distance(C, GenCentre));
				}
				if (!bLand && !bCulled) { ++Counts.KeptWithoutLand; }
				// The exact test: the column's nearest point to the island centre lies inside R + F.
				const FVector2D Nearest(FMath::Clamp(GenCentre.X, Min.X, Min.X + ChunkWorldSize),
					FMath::Clamp(GenCentre.Y, Min.Y, Min.Y + ChunkWorldSize));
				if (!bCulled && !FIslandBowlWorldMode::IsWithinIslandBounds(Nearest.X, Nearest.Y, Island))
				{
					++Counts.KeptOutsideFootprint;
				}
			}
		}
		return Counts;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIslandCullCentreProbeTest,
	"VoxelWorlds.LOD.Island.CullMatchesGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FIslandCullCentreProbeTest::RunTest(const FString& Parameters)
{
	using namespace IslandCullProbe;

	// WorldOrigin 0: the two centres coincide, so the cull keeps every land column (the control for the probe).
	{
		const FCullCounts Counts = Classify(*MakeIslandConfig(FVector::ZeroVector, 0.0f, 0.0f));
		AddInfo(FString::Printf(TEXT("PROBE origin 0: kept with land %d, culled with land %d, kept without land %d"),
			Counts.KeptWithLand, Counts.CulledWithLand, Counts.KeptWithoutLand));
		TestEqual(TEXT("WorldOrigin 0: the cull never drops a column that holds island terrain"), Counts.CulledWithLand, 0);
		// WM-E1: only columns that touch the footprint stream (the ring out to a chunk diagonal past R + F is gone; the
		// far ocean draws the sea there). Columns that touch it without a corner or the centre inside count as
		// "kept without land" above.
		TestEqual(TEXT("WorldOrigin 0: no column wholly outside R + F is kept"), Counts.KeptOutsideFootprint, 0);
	}

	// The demo's origin with the offset Review_Island_M_DemoOrigin uses: (-12345, -54321), so WorldOrigin + offset is
	// (0, 0). Before WM-A1 generation centred the island at (-12345, -54321) and the cull at (0, 0), 55,706 uu apart,
	// and 1,098 land columns were culled.
	{
		const FVector DemoOrigin(12345.0f, 54321.0f, 0.0f);
		const FCullCounts Counts = Classify(*MakeIslandConfig(DemoOrigin, -12345.0f, -54321.0f));
		AddInfo(FString::Printf(TEXT("PROBE demo origin (12345, 54321), offset (-12345, -54321): generation centres the island at (%.0f, %.0f). Kept with land %d, culled with land %d (up to %.0f uu inside the outer edge), kept without land %d"),
			Counts.GenerationCentre.X, Counts.GenerationCentre.Y, Counts.KeptWithLand, Counts.CulledWithLand,
			Counts.MaxMissingDistance, Counts.KeptWithoutLand));
		TestTrue(TEXT("Demo origin: generation centres the island on WorldOrigin + offset"),
			Counts.GenerationCentre.Equals(FVector2D::ZeroVector, 1.0));
		TestEqual(TEXT("Demo origin: the cull never drops a column that holds island terrain"), Counts.CulledWithLand, 0);
		TestTrue(TEXT("Demo origin: the cull keeps the island"), Counts.KeptWithLand > 8000);
	}

	// An island off the origin: WorldOrigin (20000, -30000) and offset (100000, 50000).
	{
		const FCullCounts Counts = Classify(*MakeIslandConfig(FVector(20000.0f, -30000.0f, 0.0f), 100000.0f, 50000.0f));
		AddInfo(FString::Printf(TEXT("PROBE origin (20000, -30000), offset (100000, 50000): generation centre (%.0f, %.0f), kept with land %d, culled with land %d, kept without land %d"),
			Counts.GenerationCentre.X, Counts.GenerationCentre.Y, Counts.KeptWithLand, Counts.CulledWithLand, Counts.KeptWithoutLand));
		TestTrue(TEXT("Offset island: generation centres the island on WorldOrigin + offset"),
			Counts.GenerationCentre.Equals(FVector2D(120000.0, 20000.0), 1.0));
		TestEqual(TEXT("Offset island: the cull never drops a column that holds island terrain"), Counts.CulledWithLand, 0);
		TestEqual(TEXT("Offset island: no column wholly outside R + F is kept"), Counts.KeptOutsideFootprint, 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
