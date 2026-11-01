// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VoxelRegionComponent.h"
#include "VoxelVertex.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G77 (WORLD_MODES_REVIEW_PLAN.md §23.14): a UVoxelRegionComponent's bounds cover its water tiles. Since MP2 a renderer
// holds one component per 16^3-chunk region, and a water tile goes to the region of its column's Z-0 chunk; over a
// lake that layer is air with no mesh, so a region whose chunks hold no mesh bounded only a chunk-sized box at its
// location and the engine culled its tiles (the lake drawn without water in a block of chunk columns at a region's
// corner). Runs headless: the component is never registered, so no render command is sent.
// ---------------------------------------------------------------------------

namespace VoxelWaterTileBoundsTestUtils
{
	constexpr double ChunkWorld = 4800.0;
	const FVector WorldOrigin(12345.0, 54321.0, 0.0);
	constexpr double WaterZ = -1500.0;

	FVector TileWorldPos(const FIntVector2& Tile)
	{
		return WorldOrigin + FVector(Tile.X * ChunkWorld, Tile.Y * ChunkWorld, 0.0);
	}

	/** A tile's quad, tile-local as FVoxelWaterMesher makes it (the proxy shifts it by the tile's world position). */
	void SubmitTile(UVoxelRegionComponent& Component, const FIntVector2& Tile)
	{
		TArray<FVoxelVertex> Vertices;
		Vertices.SetNum(4);
		Vertices[0].Position = FVector3f(0.0f, 0.0f, static_cast<float>(WaterZ));
		Vertices[1].Position = FVector3f(static_cast<float>(ChunkWorld), 0.0f, static_cast<float>(WaterZ));
		Vertices[2].Position = FVector3f(static_cast<float>(ChunkWorld), static_cast<float>(ChunkWorld), static_cast<float>(WaterZ));
		Vertices[3].Position = FVector3f(0.0f, static_cast<float>(ChunkWorld), static_cast<float>(WaterZ));
		TArray<uint32> Indices = { 0, 1, 2, 0, 2, 3 };
		Component.UpdateWaterTileFromCPUData(Tile, MoveTemp(Vertices), MoveTemp(Indices), TileWorldPos(Tile));
	}

	FBox TileBox(const FIntVector2& Tile)
	{
		const FVector Min = TileWorldPos(Tile) + FVector(0.0, 0.0, WaterZ);
		return FBox(Min, Min + FVector(ChunkWorld, ChunkWorld, 0.0));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWaterTileBoundsTest,
	"VoxelWorlds.Rendering.WaterTile.ComponentBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWaterTileBoundsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWaterTileBoundsTestUtils;

	UVoxelRegionComponent* Component = NewObject<UVoxelRegionComponent>(GetTransientPackage());
	Component->SetChunkWorldSize(static_cast<float>(ChunkWorld));
	Component->SetWorldOrigin(WorldOrigin);

	// A region component with no chunk mesh: the water tiles of a lake under an air layer (G77's case).
	const FIntVector2 TileA(0, -17);
	const FIntVector2 TileB(2, -18);
	TestFalse(TEXT("A fresh component does not tick"), Component->IsComponentTickEnabled());
	SubmitTile(*Component, TileA);
	// The scene's copy of the bounds is sent by the tick (MarkRenderTransformDirty when they moved), so a tile must
	// schedule one: the component ticks only while fades run, and a region holding only water tiles never fades.
	TestTrue(TEXT("A water tile schedules a tick (the render bounds refresh)"), Component->IsComponentTickEnabled());
	FBox Bounds = Component->CalcBounds(FTransform::Identity).GetBox();
	AddInfo(FString::Printf(TEXT("One tile at (%d, %d): bounds %s"), TileA.X, TileA.Y, *Bounds.ToString()));
	TestTrue(TEXT("The bounds cover a water tile with no chunk mesh in the component"), Bounds.IsInside(TileBox(TileA).Min) && Bounds.IsInside(TileBox(TileA).Max));

	SubmitTile(*Component, TileB);
	Bounds = Component->CalcBounds(FTransform::Identity).GetBox();
	TestTrue(TEXT("... and a second tile"), Bounds.IsInside(TileBox(TileB).Min) && Bounds.IsInside(TileBox(TileB).Max));
	TestTrue(TEXT("... the first still"), Bounds.IsInside(TileBox(TileA).Min) && Bounds.IsInside(TileBox(TileA).Max));

	// A chunk mesh far from the tiles: the union covers both.
	{
		TArray<FVoxelVertex> Vertices;
		Vertices.SetNum(3);
		TArray<uint32> Indices = { 0, 1, 2 };
		Component->UpdateChunkBuffersFromCPUData(FIntVector(5, -5, 0), MoveTemp(Vertices), MoveTemp(Indices), 0, FBox(FVector::ZeroVector, FVector(ChunkWorld)));
	}
	Bounds = Component->CalcBounds(FTransform::Identity).GetBox();
	const FVector ChunkMin = WorldOrigin + FVector(5.0, -5.0, 0.0) * ChunkWorld;
	TestTrue(TEXT("With a chunk mesh the bounds cover it"), Bounds.IsInside(ChunkMin) && Bounds.IsInside(ChunkMin + FVector(ChunkWorld)));
	TestTrue(TEXT("... and the tiles"), Bounds.IsInside(TileBox(TileA).Min) && Bounds.IsInside(TileBox(TileB).Max));

	// A removed tile leaves the bounds; a cleared set leaves only the chunk.
	Component->RemoveWaterTile(TileA);
	Bounds = Component->CalcBounds(FTransform::Identity).GetBox();
	TestTrue(TEXT("A removed tile leaves the bounds"), !Bounds.IsInside(TileBox(TileA).Min));
	TestTrue(TEXT("... the other tile stays"), Bounds.IsInside(TileBox(TileB).Max));
	Component->ClearAllWaterTiles();
	Bounds = Component->CalcBounds(FTransform::Identity).GetBox();
	TestTrue(TEXT("Cleared tiles leave the bounds"), !Bounds.IsInside(TileBox(TileB).Max));
	TestTrue(TEXT("... the chunk stays"), Bounds.IsInside(ChunkMin + FVector(ChunkWorld)));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
