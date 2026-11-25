// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "VoxelBillboardMeshGenerator.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The scatter definitions' assets loaded ahead (G287, voxel.Scatter.PreloadAssets): every definition's mesh, override
// materials and a billboard's texture and material load in the background as the definitions load, and a type's queued
// instances wait in the add queue until they are in.
//
// Through the scatter manager's own pipeline on a tilted heightmap plane, the trees (type 2) a static definition of the
// engine cube, regions of one chunk:
// - With voxel.Scatter.PreloadAssets 0 (before G287) nothing is loaded ahead and the trees' HISMs load the mesh as they
//   are made: the reference placements.
// - With 1, the load is requested as the definition is added. Held (the loads started stalled, the cube being resident),
//   every chunk's trees wait in the add queue: no HISM is made, each chunk still counts its spawn points
//   (GetChunkInstanceCount, which the scatter digest and the residency audit read), and the waits are counted.
// - Once the load is in, the trees drawn are the reference's, transform for transform, and the queue is empty.
// - A renderer shut down with a load still held drops it: no load left, no callback, nothing drawn.
// (VoxelWorlds.Scatter.PreloadAssets.HoldsUntilLoaded.) And the assets gathered per definition kind
// (VoxelWorlds.Scatter.PreloadAssets.Gather).
// ---------------------------------------------------------------------------

namespace VoxelScatterPreloadTestUtils
{
	using namespace VoxelScatterTesting;

	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr int32 TreeID = 2;
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

	TArray<FVoxelData> MakeChunk(const FIntVector& Coord)
	{
		return MakeHeightmapChunk(FVector(Coord) * ChunkWorldSize, ChunkSize, VoxelSize,
			[](double X, double Y) { return TiltedPlane().GetTerrainHeightAt(X, Y, FVoxelNoiseParams()); },
			[](int32, int32) { return static_cast<uint8>(EVoxelMaterial::Grass); });
	}

	/** A plane world whose trees are the engine cube, regions of one chunk; the loads held from the start when asked. */
	struct FHarness : FScatterTestWorld
	{
		FHarness(const TCHAR* WorldName, bool bStallLoads)
			: FScatterTestWorld(WorldName)
		{
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = 100000.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Manager = NewObject<UVoxelScatterManager>();
			Manager->AddToRoot();
			Manager->Initialize(Config, World);
			// Held before any definition asks for its assets: the cube is resident, so an unheld load is done at once
			Renderer()->SetDefinitionLoadsStalled(bStallLoads);
			AddSmoothDefinitions(*Manager);
			Manager->SetSurfacePointSpacing(VoxelSize);
			UseWorldMode(TiltedPlaneMode());
			FVoxelRenderRegions Regions;
			Regions.RegionChunks = 1;
			Regions.ChunkWorldSize = ChunkWorldSize;
			Renderer()->SetRenderRegions(Regions);
			if (const FScatterDefinition* Def = Manager->GetScatterDefinition(TreeID))
			{
				FScatterDefinition Copy = *Def;
				Copy.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(CubePath));
				Copy.Density = 0.3f;
				Manager->AddScatterDefinition(Copy);
			}
		}

		UVoxelScatterRenderer* Renderer() const { return const_cast<UVoxelScatterRenderer*>(Manager->GetScatterRenderer()); }

		void Submit(const FIntVector& Coord) { Manager->HandOffChunkVoxels(Coord, MakeChunk(Coord), ChunkSize, VoxelSize); }

		/** Pump until the extractions are done and the renderer has run; land held writes. */
		void Settle()
		{
			const double Start = FPlatformTime::Seconds();
			while (FPlatformTime::Seconds() < Start + 15.0)
			{
				Manager->Update(FVector::ZeroVector, 0.05f);
				if (Manager->GetPendingGenerationCount() == 0 && FPlatformTime::Seconds() >= Start + 0.5)
				{
					break;
				}
				FPlatformProcess::Sleep(0.005f);
			}
			for (int32 i = 0; i < 12; ++i)
			{
				Manager->Update(FVector::ZeroVector, 0.05f);
			}
			Renderer()->FlushHISMWrites();
		}

		/** The trees' HISMs */
		int32 TreeHISMs() const
		{
			TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> HISMs;
			Renderer()->GetTypeHISMs(TreeID, HISMs);
			return HISMs.Num();
		}

		/** The trees drawn: every live instance of their HISMs, world transforms. */
		TArray<FTransform> DrawnTrees() const
		{
			TArray<FTransform> Out;
			TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> HISMs;
			Renderer()->GetTypeHISMs(TreeID, HISMs);
			for (const TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>& Pair : HISMs)
			{
				for (int32 i = 0; i < Pair.Value->GetInstanceCount(); ++i)
				{
					FTransform Xf;
					if (Pair.Value->GetInstanceTransform(i, Xf, /*bWorldSpace=*/true) && !Xf.GetScale3D().IsNearlyZero())
					{
						Out.Add(Xf);
					}
				}
			}
			return Out;
		}

		/** A chunk's tree spawn points */
		int32 TreeSpawns(const FIntVector& Coord) const
		{
			int32 Spawns = 0;
			if (const FChunkScatterData* Data = Manager->GetChunkScatterData(Coord))
			{
				for (const FScatterSpawnPoint& Point : Data->SpawnPoints)
				{
					Spawns += Point.ScatterTypeID == TreeID ? 1 : 0;
				}
			}
			return Spawns;
		}
	};

	/** Instances of A with no instance of B within 0.05 uu and 1e-3 in rotation and scale, each B matched once. */
	int32 CountUnmatched(const TArray<FTransform>& A, const TArray<FTransform>& B)
	{
		TArray<bool> Used;
		Used.Init(false, B.Num());
		int32 Unmatched = 0;
		for (const FTransform& Xf : A)
		{
			bool bFound = false;
			for (int32 j = 0; j < B.Num() && !bFound; ++j)
			{
				if (!Used[j] && FVector::Dist(Xf.GetLocation(), B[j].GetLocation()) < 0.05
					&& Xf.GetRotation().Equals(B[j].GetRotation(), 1e-3) && Xf.GetScale3D().Equals(B[j].GetScale3D(), 1e-3))
				{
					Used[j] = true;
					bFound = true;
				}
			}
			Unmatched += bFound ? 0 : 1;
		}
		return Unmatched;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPreloadAssetsTest,
	"VoxelWorlds.Scatter.PreloadAssets.HoldsUntilLoaded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPreloadAssetsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterPreloadTestUtils;
	const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };

	// Before G287: nothing loaded ahead, each HISM loads its mesh as it is made. The reference placements.
	TArray<FTransform> Reference;
	{
		VoxelTesting::FScopedCVar Old(TEXT("voxel.Scatter.PreloadAssets"), 0);
		FHarness Harness(TEXT("VoxelScatterPreloadTest_Old"), /*bStallLoads=*/false);
		TestFalse(TEXT("With voxel.Scatter.PreloadAssets 0 a static definition asks for no load ahead"),
			Harness.Renderer()->IsDefinitionLoadRequested(TreeID));
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
		}
		Harness.Settle();
		Reference = Harness.DrawnTrees();
		TestEqual(TEXT("... and its instances never wait"), Harness.Renderer()->GetTotalDefinitionLoadWaits(), static_cast<int64>(0));
	}
	if (!TestTrue(TEXT("The plane grows trees"), Reference.Num() > 4))
	{
		return false;
	}

	VoxelTesting::FScopedCVar New(TEXT("voxel.Scatter.PreloadAssets"), 1);
	{
		FHarness Harness(TEXT("VoxelScatterPreloadTest_New"), /*bStallLoads=*/true);
		UVoxelScatterManager* Manager = Harness.Manager;
		UVoxelScatterRenderer* Renderer = Harness.Renderer();
		const FScatterDefinition* Trees = Manager->GetScatterDefinition(TreeID);
		if (!TestNotNull(TEXT("The trees' definition"), Trees))
		{
			return false;
		}

		// Requested as the definition was added, before any chunk; held, so not in
		TestTrue(TEXT("The trees' load is requested as the definition is added"), Renderer->IsDefinitionLoadRequested(TreeID));
		TestFalse(TEXT("... and, held, is not in"), Renderer->IsDefinitionLoaded(*Trees));
		TestEqual(TEXT("... one load pending"), Renderer->GetPendingDefinitionLoadCount(), 1);

		// While it loads: the trees wait in the queue, no HISM, each chunk counting its spawn points
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
		}
		Harness.Settle();
		TestEqual(TEXT("While the assets load, no HISM is made for the type"), Harness.TreeHISMs(), 0);
		TestTrue(TEXT("... its instances wait in the add queue"), Renderer->GetPendingInstanceAddCount() > 0);
		TestTrue(TEXT("... and the waits are counted"), Renderer->GetTotalDefinitionLoadWaits() > 0);
		int32 Waiting = 0;
		for (const FIntVector& Coord : Coords)
		{
			const int32 Spawns = Harness.TreeSpawns(Coord);
			Waiting += Spawns;
			TestEqual(*FString::Printf(TEXT("While waiting, chunk %s counts its spawn points"), *Coord.ToString()),
				Renderer->GetChunkInstanceCount(Coord, TreeID), Spawns);
		}
		TestEqual(TEXT("... as many as the reference draws"), Waiting, Reference.Num());

		// The load in: the reference's instances, transform for transform, the queue empty
		Renderer->SetDefinitionLoadsStalled(false);
		Renderer->WaitForDefinitionLoads();
		TestTrue(TEXT("Started, the load is in"), Renderer->IsDefinitionLoaded(*Trees));
		TestEqual(TEXT("... none pending"), Renderer->GetPendingDefinitionLoadCount(), 0);
		Harness.Settle();
		const TArray<FTransform> Drawn = Harness.DrawnTrees();
		TestTrue(TEXT("Once in, the type's HISMs are made"), Harness.TreeHISMs() > 0);
		TestEqual(TEXT("... the reference's instance count"), Drawn.Num(), Reference.Num());
		TestEqual(TEXT("... each a reference instance"), CountUnmatched(Drawn, Reference), 0);
		TestEqual(TEXT("... and each reference instance drawn"), CountUnmatched(Reference, Drawn), 0);
		TestEqual(TEXT("... the add queue empty"), Renderer->GetPendingInstanceAddCount(), 0);
		for (const FIntVector& Coord : Coords)
		{
			TestEqual(*FString::Printf(TEXT("Chunk %s holds its spawn points"), *Coord.ToString()),
				Renderer->GetChunkInstanceCount(Coord, TreeID), Harness.TreeSpawns(Coord));
		}
	}

	// A world torn down with a load held: the renderer drops it, nothing drawn, no load left
	{
		FHarness Harness(TEXT("VoxelScatterPreloadTest_Teardown"), /*bStallLoads=*/true);
		UVoxelScatterRenderer* Renderer = Harness.Renderer();
		for (const FIntVector& Coord : Coords)
		{
			Harness.Submit(Coord);
		}
		Harness.Settle();
		TestEqual(TEXT("Torn down while loading: the load is pending"), Renderer->GetPendingDefinitionLoadCount(), 1);
		TestEqual(TEXT("... nothing drawn"), Harness.TreeHISMs(), 0);
		Harness.Manager->Shutdown();
		TestFalse(TEXT("Shut down, the renderer holds no load"), Renderer->IsDefinitionLoadRequested(TreeID));
		TestEqual(TEXT("... none pending"), Renderer->GetPendingDefinitionLoadCount(), 0);
		TestEqual(TEXT("... no instance queued"), Renderer->GetPendingInstanceAddCount(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterPreloadGatherTest,
	"VoxelWorlds.Scatter.PreloadAssets.Gather",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterPreloadGatherTest::RunTest(const FString& Parameters)
{
	const FSoftObjectPath Mesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	const FSoftObjectPath Skeletal(TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	const FSoftObjectPath Override(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	const FSoftObjectPath Texture(TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	const FSoftObjectPath Atlas(TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	const FSoftObjectPath Billboard(FVoxelBillboardMeshGenerator::BillboardMaterialPath);
	TArray<FSoftObjectPath> Paths;

	FScatterDefinition Static;
	Static.Mesh = TSoftObjectPtr<UStaticMesh>(Mesh);
	Static.OverrideMaterials = { TSoftObjectPtr<UMaterialInterface>(Override), TSoftObjectPtr<UMaterialInterface>(), TSoftObjectPtr<UMaterialInterface>(Override) };
	UVoxelScatterRenderer::GatherDefinitionAssets(Static, Paths);
	TestTrue(TEXT("A static definition: its mesh and its override materials, once each, the empty slot skipped"), Paths == TArray<FSoftObjectPath>{ Mesh, Override });

	FScatterDefinition Debug;
	UVoxelScatterRenderer::GatherDefinitionAssets(Debug, Paths);
	TestEqual(TEXT("A definition with no mesh: nothing"), Paths.Num(), 0);

	FScatterDefinition SkeletalDef;
	SkeletalDef.Mesh = TSoftObjectPtr<UStaticMesh>(Mesh);
	SkeletalDef.SkeletalMesh = TSoftObjectPtr<USkeletalMesh>(Skeletal);
	UVoxelScatterRenderer::GatherDefinitionAssets(SkeletalDef, Paths);
	TestTrue(TEXT("A skeletal definition: its skeletal mesh, not the static one"), Paths == TArray<FSoftObjectPath>{ Skeletal });

	FScatterDefinition Card;
	Card.MeshType = EScatterMeshType::CrossBillboard;
	Card.Mesh = TSoftObjectPtr<UStaticMesh>(Mesh);
	Card.BillboardTexture = TSoftObjectPtr<UTexture2D>(Texture);
	Card.BillboardAtlasTexture = TSoftObjectPtr<UTexture2D>(Atlas);
	UVoxelScatterRenderer::GatherDefinitionAssets(Card, Paths);
	TestTrue(TEXT("A billboard: its texture and the billboard material, no mesh"), Paths == TArray<FSoftObjectPath>{ Texture, Billboard });
	Card.bUseBillboardAtlas = true;
	UVoxelScatterRenderer::GatherDefinitionAssets(Card, Paths);
	TestTrue(TEXT("... with an atlas, the atlas texture"), Paths == TArray<FSoftObjectPath>{ Atlas, Billboard });
	Card.BillboardAtlasTexture.Reset();
	UVoxelScatterRenderer::GatherDefinitionAssets(Card, Paths);
	TestTrue(TEXT("... with none, the billboard material alone"), Paths == TArray<FSoftObjectPath>{ Billboard });
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
