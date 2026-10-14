// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The vertical candidate window (WORLD_MODES_REVIEW_PLAN.md §13.11). Pure logic, runs headless.
//
// Each world mode streams the chunk layers from the viewer's layer + MinVerticalChunks to the viewer's
// layer + MaxVerticalChunks (InfinitePlane -2..+8, IslandBowl -2..+4), clipped to the terrain's height
// bounds. GetVerticalChunkRange returns those layers as absolute values, and the candidate loops in
// GetVisibleChunks and RebuildBalancedLODCache added the viewer's layer a second time: a viewer on
// layer L got 2L-2..2L+8. On layer 0 that is the same window, so it went unnoticed; above or below it
// the window slid twice as far as the viewer (a camera at z 4,800 lost every layer below 0).
// ---------------------------------------------------------------------------

namespace VerticalWindowTestHelpers
{
	/**
	 * The demo's chunk geometry and bands, with a height scale so large that the terrain-bounds cull
	 * covers hundreds of layers: the vertical window alone decides which layers are candidates.
	 */
	UVoxelWorldConfiguration* MakeConfig(EWorldMode WorldMode)
	{
		UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
		Config->WorldMode = WorldMode;
		Config->MeshingMode = EMeshingMode::DualContouring;
		Config->bEnableLOD = true;
		Config->VoxelSize = 75.0f;
		Config->ChunkSize = 64;
		Config->ViewDistance = 40000.0f;
		Config->LODBands = {
			FLODBand(0.0f, 7000.0f, 0, 1),
			FLODBand(7000.0f, 14000.0f, 1, 2),
			FLODBand(14000.0f, 40000.0f, 2, 4) };
		Config->SeaLevel = 0.0f;
		Config->BaseHeight = 0.0f;
		Config->HeightScale = 1.0e6f;
		Config->bFarBandSurfaceSlabCulling = false;
		Config->bEnableFrustumCulling = false;
		// Island: an island far larger than the view, centred on the viewer's column.
		Config->IslandRadius = 1.0e6f;
		Config->IslandFalloffWidth = 1000.0f;
		Config->IslandCenterX = 0.0f;
		Config->IslandCenterY = 0.0f;
		return Config;
	}

	/** Viewer at the centre of chunk (0, 0, Layer) of the 4,800 uu grid. */
	FLODQueryContext MakeContext(EWorldMode WorldMode, int32 Layer)
	{
		FLODQueryContext Context;
		Context.ViewerPosition = FVector(2400.0f, 2400.0f, Layer * 4800.0f + 2400.0f);
		Context.ViewDistance = 40000.0f;
		Context.WorldMode = WorldMode;
		return Context;
	}

	/** Lowest and highest chunk layer among the strategy's visible chunks. */
	void VisibleLayerRange(FDistanceBandLODStrategy& Strategy, const FLODQueryContext& Context, int32& OutMin, int32& OutMax)
	{
		Strategy.Update(Context, 1.0f / 60.0f);
		OutMin = INT32_MAX;
		OutMax = INT32_MIN;
		for (const FChunkLODRequest& Request : Strategy.GetVisibleChunks(Context))
		{
			OutMin = FMath::Min(OutMin, Request.ChunkCoord.Z);
			OutMax = FMath::Max(OutMax, Request.ChunkCoord.Z);
		}
	}
}

/**
 * The window follows the viewer one layer per layer: viewer layer L streams L-2..L+8 (InfinitePlane) and
 * L-2..L+4 (IslandBowl), on layer 0 and away from it, above and below.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLODVerticalWindowFollowsViewerTest,
	"VoxelWorlds.LOD.VerticalWindow.FollowsViewerLayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLODVerticalWindowFollowsViewerTest::RunTest(const FString& Parameters)
{
	using namespace VerticalWindowTestHelpers;

	struct FModeCase
	{
		EWorldMode Mode;
		const TCHAR* Name;
		int32 Below;
		int32 Above;
	};
	const FModeCase Modes[] = {
		{ EWorldMode::InfinitePlane, TEXT("InfinitePlane"), -2, 8 },
		{ EWorldMode::IslandBowl, TEXT("IslandBowl"), -2, 4 },
	};
	const int32 ViewerLayers[] = { -3, -1, 0, 1, 5 };

	for (const FModeCase& Case : Modes)
	{
		FDistanceBandLODStrategy Strategy;
		Strategy.Initialize(MakeConfig(Case.Mode));
		for (const int32 Layer : ViewerLayers)
		{
			int32 MinZ = 0, MaxZ = 0;
			VisibleLayerRange(Strategy, MakeContext(Case.Mode, Layer), MinZ, MaxZ);
			TestEqual(*FString::Printf(TEXT("%s, viewer on layer %d: lowest candidate layer"), Case.Name, Layer), MinZ, Layer + Case.Below);
			TestEqual(*FString::Printf(TEXT("%s, viewer on layer %d: highest candidate layer"), Case.Name, Layer), MaxZ, Layer + Case.Above);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
