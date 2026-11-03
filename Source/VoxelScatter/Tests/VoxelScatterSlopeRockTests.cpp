// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "VoxelScatterExtraction.h"
#include "VoxelScatterManager.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelMaterialRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G106: on the smooth terrain the shader draws a material's slope rock on its steep ground (VP2-O3), so scatter counts a
// surface point there as the rock: planes of grass at 45 and 35 degrees extract Stone points with a world table that turns
// Grass to Stone, the chunk's edge columns too, and Grass ones at 25, 15 and 7 degrees (the rock's weight is under a half
// there, the border about 30), on the cubic path (no slope rock) and with no table or a table without slope materials.
// The slope comes from the height field of the columns' crossings, exact on a plane. Pure: the extractor on generated
// chunks.
// ---------------------------------------------------------------------------

namespace VoxelScatterSlopeRockTestUtils
{
	using namespace VoxelScatterTesting;

	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;

	/** Atlas configs for the defaults, Grass turning to Stone on steep ground when bSlope. */
	TSharedRef<const FVoxelMaterialTable> MakeTable(bool bSlope)
	{
		TArray<FVoxelMaterialTextureConfig> Configs;
		for (const FVoxelMaterialDefinition& Mat : FVoxelMaterialTable::Defaults().GetAll())
		{
			FVoxelMaterialTextureConfig& Config = Configs.AddDefaulted_GetRef();
			Config.MaterialID = Mat.MaterialID;
			Config.MaterialName = Mat.Name;
			Config.SlopeMaterialID = (bSlope && Mat.MaterialID == EVoxelMaterial::Grass) ? EVoxelMaterial::Stone : -1;
		}
		return FVoxelMaterialTable::FromAtlas(Configs, 4, 8);
	}

	struct FCounts
	{
		int32 Grass = 0;
		int32 Stone = 0;
		int32 Other = 0;
	};

	/** The open points of a grass chunk under the plane H = 150.37 + Slope * X, extracted with this table. */
	FCounts Extract(float Slope, bool bCubic, const TSharedPtr<const FVoxelMaterialTable>& Table)
	{
		const FTiltedPlaneWorldMode Mode(150.37f, Slope, 0.0f);
		FScatterExtractionContext Context;
		Context.WorldMode = &Mode;
		Context.Materials = Table;
		const FIntVector Coord(0, 0, 0);
		const TArray<FVoxelData> Voxels = MakeHeightmapChunk(FVector::ZeroVector, ChunkSize, VoxelSize,
			[&Mode](double X, double Y) { return Mode.HeightAt(X, Y); },
			[](int32, int32) { return static_cast<uint8>(EVoxelMaterial::Grass); });
		FChunkSurfaceData Surface;
		UVoxelScatterManager::ExtractChunkSurfacePoints(bCubic, Voxels, Coord, FVector::ZeroVector, ChunkSize, VoxelSize, VoxelSize, {},
			Context, Surface);
		FCounts Counts;
		for (const FVoxelSurfacePoint& Point : Surface.SurfacePoints)
		{
			if (Point.bIsUnderground)
			{
				continue;
			}
			Counts.Grass += Point.MaterialID == EVoxelMaterial::Grass ? 1 : 0;
			Counts.Stone += Point.MaterialID == EVoxelMaterial::Stone ? 1 : 0;
			Counts.Other += (Point.MaterialID != EVoxelMaterial::Grass && Point.MaterialID != EVoxelMaterial::Stone) ? 1 : 0;
		}
		return Counts;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterSlopeRockTest,
	"VoxelWorlds.Scatter.Extraction.SlopeRockIsStone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterSlopeRockTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterSlopeRockTestUtils;
	const TSharedPtr<const FVoxelMaterialTable> Slope = MakeTable(true);
	const TSharedPtr<const FVoxelMaterialTable> NoSlope = MakeTable(false);
	const float Steep = FMath::Tan(FMath::DegreesToRadians(45.0f));
	const float Mid = FMath::Tan(FMath::DegreesToRadians(15.0f));
	const float JustSteep = FMath::Tan(FMath::DegreesToRadians(35.0f));
	const float JustGentle = FMath::Tan(FMath::DegreesToRadians(25.0f));
	const float Gentle = FMath::Tan(FMath::DegreesToRadians(7.0f));

	const FCounts SteepRock = Extract(Steep, false, Slope);
	AddInfo(FString::Printf(TEXT("45 degrees: %d stone, %d grass"), SteepRock.Stone, SteepRock.Grass));
	TestTrue(TEXT("The steep plane has points"), SteepRock.Stone + SteepRock.Grass > 50);
	TestEqual(TEXT("At 45 degrees every open point is the rock, the chunk's edge columns too"), SteepRock.Grass, 0);
	TestEqual(TEXT("and nothing else"), SteepRock.Other, 0);

	const FCounts Rock35 = Extract(JustSteep, false, Slope);
	TestTrue(TEXT("The 35-degree plane has points"), Rock35.Stone > 50);
	TestEqual(TEXT("At 35 degrees every open point is the rock"), Rock35.Grass, 0);
	const FCounts Grass25 = Extract(JustGentle, false, Slope);
	TestTrue(TEXT("The 25-degree plane has points"), Grass25.Grass > 50);
	TestEqual(TEXT("At 25 degrees: grass"), Grass25.Stone, 0);

	const FCounts MidGrass = Extract(Mid, false, Slope);
	TestTrue(TEXT("The 15-degree plane has points"), MidGrass.Grass > 50);
	TestEqual(TEXT("At 15 degrees the rock's weight is under a half: grass"), MidGrass.Stone, 0);
	const FCounts GentleGrass = Extract(Gentle, false, Slope);
	TestTrue(TEXT("The gentle plane has points"), GentleGrass.Grass > 50);
	TestEqual(TEXT("At 7 degrees: grass"), GentleGrass.Stone, 0);

	const FCounts Cubic = Extract(Steep, true, Slope);
	TestTrue(TEXT("The cubic path has points"), Cubic.Grass > 0);
	TestEqual(TEXT("The cubic path draws no slope rock: grass"), Cubic.Stone, 0);
	TestEqual(TEXT("With no table: the voxels' grass"), Extract(Steep, false, nullptr).Stone, 0);
	TestEqual(TEXT("With a table without slope materials: grass"), Extract(Steep, false, NoSlope).Stone, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
