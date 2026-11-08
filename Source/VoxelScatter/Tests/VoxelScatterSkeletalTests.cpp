// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Animation/TransformProviderData.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The skeletal definitions (the foliage and bolt plan's D68, F3): a definition with a SkeletalMesh is drawn through one
// UInstancedSkinnedMeshComponent per type and region, and the rest of the pipeline is a static definition's.
//
// Through the scatter manager's own pipeline on a tilted heightmap plane (the test definitions given an engine mesh), the
// trees (type 2) once static and once skeletal (the engine's SkeletalCube), regions of one chunk:
// - Its mesh's load is requested as the definition is added, before any instance; its instances wait in the add queue
//   while it loads, and none is drawn as a HISM.
// - Its components are made with no tick (none registered), at their region's origin, with the definition's fade start
//   and cull distance, shadows, no collision, the Dynamic Wind provider when the plugin is loaded, and the definition's
//   dynamic material instance.
// - The instances drawn are the static definition's, transform for transform; each chunk holds as many as its spawn
//   points; a harvest query finds them; the component's bounds hold them.
// - A chunk unloaded takes its instances out of its region's component; Shutdown destroys the components.
// ---------------------------------------------------------------------------

namespace VoxelScatterSkeletalTestUtils
{
	using namespace VoxelScatterTesting;

	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr int32 TreeID = 2;
	const TCHAR* const SkeletalCubePath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

	TArray<FVoxelData> MakeChunk(const FIntVector& Coord)
	{
		return MakeHeightmapChunk(FVector(Coord) * ChunkWorldSize, ChunkSize, VoxelSize,
			[](double X, double Y) { return TiltedPlane().GetTerrainHeightAt(X, Y, FVoxelNoiseParams()); },
			[](int32, int32) { return static_cast<uint8>(EVoxelMaterial::Grass); });
	}

	/** A plane world whose trees are static or skeletal, regions of one chunk. */
	struct FHarness : FScatterTestWorld
	{
		explicit FHarness(const TCHAR* WorldName, bool bSkeletal, UMaterialInterface* Override)
			: FScatterTestWorld(WorldName)
		{
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = 100000.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			StartManager(/*bSmoothDefinitions=*/true);
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
				Copy.bEnableCollision = false;
				if (bSkeletal)
				{
					Copy.SkeletalMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(SkeletalCubePath));
					if (Override)
					{
						Copy.OverrideMaterials = { TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(Override->GetPathName())) };
						Copy.bDynamicMaterials = true;
					}
				}
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

		/** The trees drawn: every live instance of their HISMs or skinned components, world transforms. */
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
			TArray<TPair<FIntVector, UInstancedSkinnedMeshComponent*>> Skinned;
			Renderer()->GetTypeSkinnedComponents(TreeID, Skinned);
			for (const TPair<FIntVector, UInstancedSkinnedMeshComponent*>& Pair : Skinned)
			{
				for (int32 i = 0; i < Pair.Value->GetInstanceCount(); ++i)
				{
					FTransform Xf;
					if (Pair.Value->GetInstanceTransform(Pair.Value->GetInstanceId(i), Xf, /*bWorldSpace=*/true))
					{
						Out.Add(Xf);
					}
				}
			}
			return Out;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterSkeletalTest,
	"VoxelWorlds.Scatter.Skeletal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterSkeletalTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterSkeletalTestUtils;
	const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
	UMaterialInterface* Override = UMaterial::GetDefaultMaterial(MD_Surface);

	// The trees as a static definition: the reference placements.
	TArray<FTransform> StaticTrees;
	{
		FHarness Static(TEXT("VoxelScatterSkeletalTest_Static"), /*bSkeletal=*/false, nullptr);
		for (const FIntVector& Coord : Coords)
		{
			Static.Submit(Coord);
		}
		Static.Settle();
		StaticTrees = Static.DrawnTrees();
		TestEqual(TEXT("A static definition makes no skinned component"), Static.Renderer()->GetSkinnedComponentCount(), 0);
		TestFalse(TEXT("... and requests no load"), Static.Renderer()->IsDefinitionLoadRequested(TreeID));
	}
	if (!TestTrue(TEXT("The plane grows trees"), StaticTrees.Num() > 4))
	{
		return false;
	}

	FHarness Harness(TEXT("VoxelScatterSkeletalTest_Skeletal"), /*bSkeletal=*/true, Override);
	UVoxelScatterManager* Manager = Harness.Manager;
	UVoxelScatterRenderer* Renderer = Harness.Renderer();
	const FScatterDefinition* Trees = Manager->GetScatterDefinition(TreeID);
	if (!TestNotNull(TEXT("The trees' definition"), Trees) || !TestTrue(TEXT("... skeletal"), Trees->IsSkeletal()))
	{
		return false;
	}

	// Loaded ahead: requested as the definition was added, before any chunk; instances wait while it loads.
	TestTrue(TEXT("The skeletal mesh's load is requested as the definition is added"), Renderer->IsDefinitionLoadRequested(TreeID));
	TestEqual(TEXT("... with no component yet"), Renderer->GetSkinnedComponentCount(), 0);
	for (const FIntVector& Coord : Coords)
	{
		Harness.Submit(Coord);
	}
	if (!Renderer->IsDefinitionLoaded(*Trees))
	{
		// Still loading (nothing else had loaded the cube): one pass leaves the trees queued, no component made
		Manager->Update(FVector::ZeroVector, 0.05f);
		TestEqual(TEXT("While it loads, no component is made"), Renderer->GetSkinnedComponentCount(), 0);
	}
	Renderer->WaitForDefinitionLoads();
	Harness.Settle();

	// The instances: the static definition's, never in a HISM, each chunk as many as its spawn points.
	TArray<TPair<FIntVector, UHierarchicalInstancedStaticMeshComponent*>> HISMs;
	Renderer->GetTypeHISMs(TreeID, HISMs);
	TestEqual(TEXT("No HISM for a skeletal definition"), HISMs.Num(), 0);
	const TArray<FTransform> SkinnedTrees = Harness.DrawnTrees();
	TestEqual(TEXT("As many instances as the static definition"), SkinnedTrees.Num(), StaticTrees.Num());
	TestEqual(TEXT("... the same transforms"), CountUnmatched(SkinnedTrees, StaticTrees), 0);
	TestEqual(TEXT("The renderer counts them"), Renderer->GetSkinnedInstanceCount(), SkinnedTrees.Num());
	for (const FIntVector& Coord : Coords)
	{
		int32 Spawns = 0;
		if (const FChunkScatterData* Data = Manager->GetChunkScatterData(Coord))
		{
			for (const FScatterSpawnPoint& Point : Data->SpawnPoints)
			{
				Spawns += Point.ScatterTypeID == TreeID ? 1 : 0;
			}
		}
		TestEqual(*FString::Printf(TEXT("Chunk %s holds its spawn points"), *Coord.ToString()), Renderer->GetChunkInstanceCount(Coord, TreeID), Spawns);
	}

	// The components: no tick, the region's origin, the definition's fade and cull, shadows, no collision, the provider,
	// the dynamic material.
	TArray<TPair<FIntVector, UInstancedSkinnedMeshComponent*>> Components;
	Renderer->GetTypeSkinnedComponents(TreeID, Components);
	TestTrue(TEXT("A component per region with trees"), Components.Num() > 1 && Components.Num() <= Coords.Num());
	const TArray<UMaterialInstanceDynamic*> Published = Manager->GetDynamicMaterials(Trees->Name);
	TestEqual(TEXT("One dynamic instance published for the slot"), Published.Num(), 1);
	UClass* ProviderClass = UVoxelScatterRenderer::GetSkinnedTransformProviderClass();
	for (const TPair<FIntVector, UInstancedSkinnedMeshComponent*>& Pair : Components)
	{
		UInstancedSkinnedMeshComponent* Component = Pair.Value;
		const FString Where = Pair.Key.ToString();
		TestFalse(*FString::Printf(TEXT("Region %s: no tick"), *Where), Component->IsComponentTickEnabled());
		TestFalse(*FString::Printf(TEXT("Region %s: ... none can be"), *Where), Component->PrimaryComponentTick.bCanEverTick);
		TestFalse(*FString::Printf(TEXT("Region %s: ... none registered"), *Where), Component->PrimaryComponentTick.IsTickFunctionRegistered());
		TestTrue(*FString::Printf(TEXT("Region %s: at its origin"), *Where),
			Component->GetComponentLocation().Equals(Renderer->GetRenderRegions().GetRegionOrigin(Pair.Key), 0.01));
		int32 StartCull = 0, EndCull = 0;
		Component->GetCullDistances(StartCull, EndCull);
		TestEqual(*FString::Printf(TEXT("Region %s: fades from the definition's start"), *Where), StartCull, static_cast<int32>(Trees->LODStartDistance));
		TestEqual(*FString::Printf(TEXT("Region %s: culls at its distance"), *Where), EndCull, static_cast<int32>(Trees->CullDistance));
		TestEqual(*FString::Printf(TEXT("Region %s: shadows as the definition"), *Where), static_cast<bool>(Component->CastShadow), Trees->bCastShadows);
		TestEqual(*FString::Printf(TEXT("Region %s: no collision"), *Where), Component->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
		TestTrue(*FString::Printf(TEXT("Region %s: the definition's mesh"), *Where),
			Component->GetSkinnedAsset() && Component->GetSkinnedAsset()->GetPathName() == SkeletalCubePath);
		if (ProviderClass)
		{
			TestTrue(*FString::Printf(TEXT("Region %s: the Dynamic Wind provider"), *Where),
				Component->GetTransformProvider() && Component->GetTransformProvider()->GetClass() == ProviderClass);
		}
		TestTrue(*FString::Printf(TEXT("Region %s: the dynamic material"), *Where), Published.Num() == 1 && Component->GetMaterial(0) == Published[0]);
		const FBoxSphereBounds Bounds = Component->CalcBounds(Component->GetComponentTransform());
		for (int32 i = 0; i < Component->GetInstanceCount(); ++i)
		{
			FTransform Xf;
			if (Component->GetInstanceTransform(Component->GetInstanceId(i), Xf, /*bWorldSpace=*/true)
				&& !Bounds.GetBox().ExpandBy(1.0).IsInside(Xf.GetLocation()))
			{
				AddError(FString::Printf(TEXT("Region %s: instance %d at %s outside its bounds %s"), *Where, i, *Xf.GetLocation().ToString(), *Bounds.GetBox().ToString()));
				break;
			}
		}
	}
	if (!ProviderClass)
	{
		AddInfo(TEXT("The Dynamic Wind plugin is not loaded: the components are drawn with no provider (at rest)."));
	}

	// A harvest query finds a skeletal instance.
	{
		const FVector Target = SkinnedTrees[0].GetLocation();
		int32 Index = INDEX_NONE;
		FTransform Found;
		TestTrue(TEXT("FindNearestInstance finds a skinned instance"), Renderer->FindNearestInstance(TreeID, Target + FVector(10.0, 0.0, 0.0), 200.0f, Index, Found));
		TestTrue(TEXT("... the one there"), Found.GetLocation().Equals(Target, 1.0));
	}

	// A chunk unloaded: its instances leave its region's component, the others stay.
	const FIntVector Unloaded = Coords[0];
	const int32 Before = Renderer->GetSkinnedInstanceCount();
	const int32 InChunk = Renderer->GetChunkInstanceCount(Unloaded, TreeID);
	Manager->OnChunkUnloaded(Unloaded);
	TestEqual(TEXT("Unloaded: the chunk holds no tree"), Renderer->GetChunkInstanceCount(Unloaded, TreeID), 0);
	TestEqual(TEXT("... the others stay"), Renderer->GetSkinnedInstanceCount(), Before - InChunk);
	int32 InComponents = 0;
	Renderer->GetTypeSkinnedComponents(TreeID, Components);
	for (const TPair<FIntVector, UInstancedSkinnedMeshComponent*>& Pair : Components)
	{
		InComponents += Pair.Value->GetInstanceCount();
		if (Pair.Key == Renderer->GetRenderRegions().GetRegion(Unloaded))
		{
			TestEqual(TEXT("... its region's component is empty"), Pair.Value->GetInstanceCount(), 0);
		}
	}
	TestEqual(TEXT("... the components hold the rest"), InComponents, Before - InChunk);

	// Shutdown: the components go.
	TWeakObjectPtr<UInstancedSkinnedMeshComponent> Any = Components.Num() > 0 ? Components[0].Value : nullptr;
	Manager->Shutdown();
	TestTrue(TEXT("Shutdown destroys the skinned components"), !Any.IsValid() || Any->IsBeingDestroyed() || !Any->IsRegistered());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
