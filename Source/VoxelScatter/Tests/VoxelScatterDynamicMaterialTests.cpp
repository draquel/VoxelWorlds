// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "VoxelScatterTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The scatter's dynamic materials (the foliage and bolt plan's D67): a definition with bDynamicMaterials is drawn through
// one dynamic instance per override slot, made from the override material when its first component is, the same one on
// its component in every region, published by the manager under the definition's name with one OnDynamicMaterialCreated;
// a definition without the flag draws its override material as it is and publishes nothing; Shutdown releases every
// instance with one OnDynamicMaterialReleased each and publishes nothing after.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterDynamicMaterialsTest,
	"VoxelWorlds.Scatter.DynamicMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterDynamicMaterialsTest::RunTest(const FString& Parameters)
{
	VoxelScatterTesting::FScatterTestWorld Harness(TEXT("VoxelScatterDynamicMaterialsTest"));
	Harness.Config->ChunkSize = 16;
	Harness.Config->VoxelSize = 100.0f;
	Harness.StartManager(/*bSmoothDefinitions=*/true);
	UVoxelScatterManager* Manager = Harness.Manager;
	UVoxelScatterRenderer* Renderer = const_cast<UVoxelScatterRenderer*>(Manager->GetScatterRenderer());
	if (!TestNotNull(TEXT("A renderer"), Renderer))
	{
		return false;
	}

	UMaterialInterface* Parent = UMaterial::GetDefaultMaterial(MD_Surface);
	const TSoftObjectPtr<UStaticMesh> Cube(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
	constexpr int32 DynamicID = 40;
	constexpr int32 PlainID = 41;
	{
		FScatterDefinition Dynamic;
		Dynamic.ScatterID = DynamicID;
		Dynamic.Name = TEXT("WindyGrass");
		Dynamic.Mesh = Cube;
		Dynamic.OverrideMaterials = { TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(Parent->GetPathName())) };
		Dynamic.bDynamicMaterials = true;
		Manager->AddScatterDefinition(Dynamic);
		FScatterDefinition Plain = Dynamic;
		Plain.ScatterID = PlainID;
		Plain.Name = TEXT("StillGrass");
		Plain.bDynamicMaterials = false;
		Manager->AddScatterDefinition(Plain);
	}

	int32 Created = 0, Released = 0;
	FString CreatedName;
	UMaterialInstanceDynamic* CreatedMaterial = nullptr;
	UMaterialInstanceDynamic* ReleasedMaterial = nullptr;
	Manager->OnDynamicMaterialCreated.AddLambda([&](const FString& Name, UMaterialInstanceDynamic* Material)
	{
		++Created;
		CreatedName = Name;
		CreatedMaterial = Material;
	});
	Manager->OnDynamicMaterialReleased.AddLambda([&](const FString& Name, UMaterialInstanceDynamic* Material)
	{
		++Released;
		ReleasedMaterial = Material;
		TestTrue(TEXT("Released after it is unpublished"), Manager->GetDynamicMaterials(Name).Num() == 0);
	});

	TestEqual(TEXT("Nothing published before a component"), Manager->GetDynamicMaterials(TEXT("WindyGrass")).Num(), 0);

	// Two regions' components of the dynamic definition (regions of one chunk, so each has its own component): one
	// instance, made once.
	FVoxelRenderRegions Regions;
	Regions.RegionChunks = 1;
	Regions.ChunkWorldSize = 1600.0;
	Renderer->SetRenderRegions(Regions);
	UHierarchicalInstancedStaticMeshComponent* RegionA = Renderer->GetOrCreateHISM(DynamicID, FIntVector(0, 0, 0));
	UHierarchicalInstancedStaticMeshComponent* RegionB = Renderer->GetOrCreateHISM(DynamicID, FIntVector(1, 0, 0));
	if (!TestNotNull(TEXT("Region A's component"), RegionA) || !TestNotNull(TEXT("Region B's component"), RegionB))
	{
		return false;
	}
	UMaterialInstanceDynamic* MaterialA = Cast<UMaterialInstanceDynamic>(RegionA->GetMaterial(0));
	TestNotNull(TEXT("Region A draws a dynamic instance"), MaterialA);
	TestTrue(TEXT("Two components"), RegionA != RegionB);
	TestTrue(TEXT("... the same one as region B"), MaterialA && RegionB->GetMaterial(0) == MaterialA);
	TestTrue(TEXT("... made from the override material"), MaterialA && MaterialA->Parent == Parent);
	TestEqual(TEXT("One creation event"), Created, 1);
	TestEqual(TEXT("... under the definition's name"), CreatedName, FString(TEXT("WindyGrass")));
	TestTrue(TEXT("... with the instance drawn"), CreatedMaterial == MaterialA);
	const TArray<UMaterialInstanceDynamic*> Published = Manager->GetDynamicMaterials(TEXT("WindyGrass"));
	TestTrue(TEXT("Published under the name"), Published.Num() == 1 && Published[0] == MaterialA);
	TArray<FVoxelScatterDynamicMaterial> All;
	Manager->GetAllDynamicMaterials(All);
	TestTrue(TEXT("Every instance: the one, with its type and slot"), All.Num() == 1 && All[0].ScatterID == DynamicID && All[0].Slot == 0 && All[0].Material == MaterialA);

	// A definition without the flag: its override as it is, nothing published.
	UHierarchicalInstancedStaticMeshComponent* PlainHISM = Renderer->GetOrCreateHISM(PlainID, FIntVector(0, 0, 0));
	TestTrue(TEXT("Without the flag: the override material itself"), PlainHISM && PlainHISM->GetMaterial(0) == Parent);
	TestEqual(TEXT("... nothing published"), Manager->GetDynamicMaterials(TEXT("StillGrass")).Num(), 0);
	TestEqual(TEXT("... no creation event"), Created, 1);

	// The scatter goes: every instance released once, nothing published after.
	Manager->Shutdown();
	TestEqual(TEXT("One release event"), Released, 1);
	TestTrue(TEXT("... for the instance made"), ReleasedMaterial == MaterialA);
	Manager->GetAllDynamicMaterials(All);
	TestEqual(TEXT("Nothing published after Shutdown"), All.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
