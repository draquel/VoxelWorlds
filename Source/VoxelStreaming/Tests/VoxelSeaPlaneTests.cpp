// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelData.h"
#include "VoxelSeaSurface.h"
#include "VoxelWorldConfiguration.h"
#include "Materials/Material.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G19 (WORLD_MODES_REVIEW_PLAN.md §14.1). A heightmap world's sea had two planes: generation, scatter, the map and PCG
// at WaterLevel, and navigation, the character, the NPC spawner and the LOD slab at WaterLevel + WorldOrigin.Z. The
// terrain's heights are absolute (WorldOrigin moves the chunk grid, not the ground), so the water generation floods and
// the water tiles draw is at WaterLevel, and the chunk manager's sea, which every gameplay consumer reads, must be too.
//
// - Streaming.Plane.OneSeaPlane: the small plane with its sea at 0 and WorldOrigin.Z at -3,000 and at 2,600. The highest
//   water-flagged voxel generation made lies within a voxel under WaterLevel; every flagged voxel is under the chunk
//   manager's sea (GetSeaDepthAt >= 0), a voxel above the highest is not, and GetSeaSurface is the WaterLevel plane. Every
//   chunk column holding water has a water tile (a chunk was skipped for a tile when its grid Z, not its world Z, was over
//   WaterLevel).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelOneSeaPlaneTest,
	"VoxelWorlds.Streaming.Plane.OneSeaPlane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelOneSeaPlaneTest::RunTest(const FString& Parameters)
{
	for (const double OriginZ : { -3000.0, 2600.0 })
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("VoxelOneSeaPlaneWorld"));
		W.ConfigureSmallPlane();
		W.Config->WorldOrigin = FVector(12345.0, 54321.0, OriginZ);
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = 0.0f; // the terrain lies within +-800: half of it under the sea
		W.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface); // the water tiles run with a material
		W.StartManager();
		W.SetViewer(FVector(W.Config->WorldOrigin.X, W.Config->WorldOrigin.Y, 200.0));
		const FString Name = FString::Printf(TEXT("WorldOrigin.Z %.0f"), OriginZ);
		if (!TestTrue(*FString::Printf(TEXT("%s: the pipeline settled"), *Name), W.Settle()))
		{
			W.Stop();
			continue;
		}

		const int32 CS = W.Config->ChunkSize;
		const double VS = W.Config->VoxelSize;
		const double WaterLevel = W.Config->WaterLevel;
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		int64 Flagged = 0, Under = 0;
		double TopZ = -1.0e30;
		FVector Top = FVector::ZeroVector;
		TSet<FIntVector2> WetColumns;
		for (const FIntVector& Coord : Loaded)
		{
			const TSharedPtr<const TArray<FVoxelData>> Voxels = W.Manager->GetEditMergedSnapshot(Coord);
			if (!Voxels.IsValid() || Voxels->Num() != CS * CS * CS)
			{
				continue;
			}
			for (int32 Index = 0; Index < Voxels->Num(); ++Index)
			{
				const FVoxelData& V = (*Voxels)[Index];
				if (V.IsSolid() || !V.HasWaterFlag())
				{
					continue;
				}
				const FIntVector Local(Index % CS, (Index / CS) % CS, Index / (CS * CS));
				const FVector Pos = W.Config->WorldOrigin + FVector(Coord * CS + Local) * VS;
				++Flagged;
				WetColumns.Add(FIntVector2(Coord.X, Coord.Y));
				Under += W.Manager->GetSeaDepthAt(Pos) >= 0.0f ? 1 : 0;
				if (Pos.Z > TopZ)
				{
					TopZ = Pos.Z;
					Top = Pos;
				}
			}
		}
		const FVoxelSeaSurface Sea = W.Manager->GetSeaSurface();
		int32 Tiled = 0;
		for (const FIntVector2& Column : WetColumns)
		{
			Tiled += W.Renderer.WaterTiles.Contains(Column) ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("%s: %d chunks, %lld water-flagged voxels, the highest at Z %.0f (WaterLevel %.0f); under the chunk manager's sea %lld; its plane %.0f"),
			*Name, Loaded.Num(), Flagged, TopZ, WaterLevel, Under, Sea.PlaneZ));
		TestTrue(*FString::Printf(TEXT("%s: the sea floods (non-vacuous)"), *Name), Flagged > 1000);
		TestTrue(*FString::Printf(TEXT("%s: generation floods up to WaterLevel"), *Name), TopZ <= WaterLevel && TopZ > WaterLevel - VS);
		TestEqual(*FString::Printf(TEXT("%s: GetSeaSurface is the WaterLevel plane"), *Name), Sea.PlaneZ, WaterLevel);
		TestEqual(*FString::Printf(TEXT("%s: every flagged voxel is under the sea GetSeaDepthAt reads"), *Name), Under, Flagged);
		TestTrue(*FString::Printf(TEXT("%s: a voxel above the highest is above it"), *Name),
			W.Manager->GetSeaDepthAt(Top + FVector(0.0, 0.0, VS)) < 0.0f);
		AddInfo(FString::Printf(TEXT("%s: %d chunk columns hold water, %d of them have a water tile"), *Name, WetColumns.Num(), Tiled));
		TestEqual(*FString::Printf(TEXT("%s: every column holding water has a water tile"), *Name), Tiled, WetColumns.Num());
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
