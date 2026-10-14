// Copyright Daniel Raquel. All Rights Reserved.

// Review B probe (Documentation/WORLD_MODES_REVIEW_PLAN.md §5 I1, finding KI3): where the LOD strategy's island cull
// keeps chunks, against where generation puts the island. Generation tests the absolute voxel position against the
// raw IslandCenterX/Y (FIslandBowlWorldMode::IsWithinIslandBounds); the cull centres the island on
// WorldOrigin + IslandCenterX/Y. The probe records the disagreement and passes; turn the PROBE lines into assertions
// with the fix.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "IslandBowlWorldMode.h"
#include "VoxelWorldConfiguration.h"

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
	};

	/** Every chunk column within 300,000 uu of the generation centre, classified by the cull and by generation. */
	FCullCounts Classify(const UVoxelWorldConfiguration& Config)
	{
		FProbeStrategy Strategy;
		Strategy.Initialize(&Config);
		FLODQueryContext Context;
		Context.WorldOrigin = Config.WorldOrigin;
		Context.WorldMode = EWorldMode::IslandBowl;
		Context.ViewDistance = Config.ViewDistance;
		Context.ViewerPosition = Config.WorldOrigin;
		Strategy.Update(Context, 1.0f / 60.0f);

		FIslandBowlParams Island;
		Island.IslandRadius = Config.IslandRadius;
		Island.FalloffWidth = Config.IslandFalloffWidth;
		Island.CenterX = Config.IslandCenterX;
		Island.CenterY = Config.IslandCenterY;

		const float ChunkWorldSize = Config.ChunkSize * Config.VoxelSize;
		const FVector2D GenCentre(Island.CenterX, Island.CenterY);
		const int32 Range = FMath::CeilToInt(300000.0f / ChunkWorldSize);
		const int32 CX = FMath::FloorToInt((GenCentre.X - Config.WorldOrigin.X) / ChunkWorldSize);
		const int32 CY = FMath::FloorToInt((GenCentre.Y - Config.WorldOrigin.Y) / ChunkWorldSize);

		FCullCounts Counts;
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
			}
		}
		return Counts;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIslandCullCentreProbeTest,
	"VoxelWorlds.Review.Island.CullCentreProbe",
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
	}

	// The demo's origin with the offset Review_Island_M_DemoOrigin uses: (-12345, -54321), so WorldOrigin + offset is (0, 0).
	{
		const FVector DemoOrigin(12345.0f, 54321.0f, 0.0f);
		const FCullCounts Counts = Classify(*MakeIslandConfig(DemoOrigin, -12345.0f, -54321.0f));
		const float CentreGap = FVector2D(DemoOrigin.X, DemoOrigin.Y).Size();
		AddInfo(FString::Printf(TEXT("PROBE demo origin (12345, 54321), offset (-12345, -54321): generation centres the island at (-12345, -54321), the cull at (0, 0), %.0f uu apart. Kept with land %d, culled with land %d (up to %.0f uu inside the outer edge), kept without land %d"),
			CentreGap, Counts.KeptWithLand, Counts.CulledWithLand, Counts.MaxMissingDistance, Counts.KeptWithoutLand));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
