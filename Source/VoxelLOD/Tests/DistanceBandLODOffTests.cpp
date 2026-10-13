// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ==================== Helpers ====================

namespace LODOffTestHelpers
{
	/**
	 * The demo's geometry and bands: 64-voxel chunks at 75 uu (4,800 uu wide), stride 1 to 7,000 uu,
	 * stride 2 to 14,000 uu, stride 4 out to the 40,000 uu view distance.
	 */
	UVoxelWorldConfiguration* MakeDemoConfig(EMeshingMode MeshingMode, bool bEnableLOD)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = EWorldMode::InfinitePlane;
		Config->MeshingMode = MeshingMode;
		Config->bEnableLOD = bEnableLOD;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->ViewDistance = 40000.0f;
		Config->LODBands = {
			FLODBand(0.0f, 7000.0f, 0, 1),
			FLODBand(7000.0f, 14000.0f, 1, 2),
			FLODBand(14000.0f, 40000.0f, 2, 4) };
		return Config;
	}

	/** Viewer at the centre of chunk (0,0,0), so chunk (X,0,0) is X * 4,800 uu away. */
	FLODQueryContext MakeContext()
	{
		FLODQueryContext Context;
		Context.ViewerPosition = FVector(2400.0f, 2400.0f, 2400.0f);
		Context.ViewDistance = 40000.0f;
		Context.WorldMode = EWorldMode::InfinitePlane;
		return Context;
	}
}

// ==================== Tests ====================

/**
 * Decision D7 (WORLD_MODES_REVIEW_PLAN.md §8): cubic meshing runs without the LOD system whatever
 * bEnableLOD says, and the config itself is not changed, so the same asset meshed smooth keeps LOD.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODOffCubicForcesOffTest,
	"VoxelWorlds.LOD.LODOff.CubicForcesOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODOffCubicForcesOffTest::RunTest(const FString& Parameters)
{
	using namespace LODOffTestHelpers;

	UVoxelWorldConfiguration* Cubic = MakeDemoConfig(EMeshingMode::Cubic, true);
	TestFalse(TEXT("Cubic with bEnableLOD on: LOD off"), Cubic->IsLODEnabled());
	TestTrue(TEXT("The cubic rule does not write bEnableLOD back to the config"), Cubic->bEnableLOD);
	TestFalse(TEXT("Cubic with bEnableLOD off: LOD off"), MakeDemoConfig(EMeshingMode::Cubic, false)->IsLODEnabled());
	TestTrue(TEXT("Dual Contouring with bEnableLOD on: LOD on"), MakeDemoConfig(EMeshingMode::DualContouring, true)->IsLODEnabled());
	TestFalse(TEXT("Dual Contouring with bEnableLOD off: LOD off"), MakeDemoConfig(EMeshingMode::DualContouring, false)->IsLODEnabled());

	// The strategy follows the effective value: every cubic chunk is LOD 0 at every distance.
	FDistanceBandLODStrategy Strategy;
	Strategy.Initialize(Cubic);
	const FLODQueryContext Context = MakeContext();
	Strategy.Update(Context, 1.0f / 60.0f);
	for (int32 X = 0; X <= 8; ++X)
	{
		TestEqual(*FString::Printf(TEXT("Cubic chunk %d chunks out renders at LOD 0"), X),
			Strategy.GetLODForChunk(FIntVector(X, 0, 0), Context), 0);
	}

	return true;
}

/**
 * KC11: far-chunk compression keys on the chunk's distance band. With LOD off every chunk is LOD 0,
 * so GetDistanceBandLOD must still place it in the band its distance falls in, and on a fresh frame
 * (no hysteresis history) that is the same band the LOD system would give the chunk.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODOffDistanceBandsTest,
	"VoxelWorlds.LOD.LODOff.DistanceBands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODOffDistanceBandsTest::RunTest(const FString& Parameters)
{
	using namespace LODOffTestHelpers;
	const FLODQueryContext Context = MakeContext();

	FDistanceBandLODStrategy LODOff;
	LODOff.Initialize(MakeDemoConfig(EMeshingMode::Cubic, true));
	LODOff.Update(Context, 1.0f / 60.0f);

	// Chunk X sits X * 4,800 uu away: 0 and 4,800 in band 0, 9,600 in band 1, 14,400 and beyond in band 2.
	const int32 ExpectedBand[] = { 0, 0, 1, 2, 2, 2, 2, 2, 2 };
	for (int32 X = 0; X < UE_ARRAY_COUNT(ExpectedBand); ++X)
	{
		TestEqual(*FString::Printf(TEXT("LOD off: chunk %d chunks out is in band %d"), X, ExpectedBand[X]),
			LODOff.GetDistanceBandLOD(FIntVector(X, 0, 0), Context), ExpectedBand[X]);
	}
	TestEqual(TEXT("LOD off: a chunk beyond the view distance reads as the last band"),
		LODOff.GetDistanceBandLOD(FIntVector(12, 0, 0), Context), 2);

	// Same bands with the LOD system on: the distance band is the chunk's LOD.
	FDistanceBandLODStrategy LODOn;
	LODOn.Initialize(MakeDemoConfig(EMeshingMode::DualContouring, true));
	LODOn.Update(Context, 1.0f / 60.0f);
	for (int32 X = 0; X < UE_ARRAY_COUNT(ExpectedBand); ++X)
	{
		const FIntVector Coord(X, 0, 0);
		TestEqual(*FString::Printf(TEXT("LOD on: chunk %d chunks out, distance band equals its LOD"), X),
			LODOn.GetDistanceBandLOD(Coord, Context), LODOn.GetLODForChunk(Coord, Context));
		TestEqual(*FString::Printf(TEXT("LOD on: chunk %d chunks out matches the LOD-off band"), X),
			LODOn.GetLODForChunk(Coord, Context), ExpectedBand[X]);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
