// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelMaterialAtlas.h"
#include "Tasks/Task.h"
#include "HAL/PlatformProcess.h"
#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G100: the material registry is one table for the process, which the meshers' and the map's workers read with no lock
// while the game thread publishes an atlas's table (SetAtlasPositions, for every region component it creates):
//
// - MaterialRegistry.AtlasTable: an atlas sets the whole table from the defaults (grown to its highest MaterialID, its
//   configs applied, hardness kept), not on top of the last atlas's; a pointer into an earlier table stays valid; the
//   same atlas again republishes the table it published before.
// - MaterialRegistry.WorkerReads: workers reading the table while the game thread alternates two atlases see one table
//   whole on every read. Before G100 the game thread rewrote the array the workers read, and they saw a mix.
//
// Both tests leave the registry with the content it had (an atlas built from that table), since later tests read it
// (VoxelWorlds.Configuration.TreeTemplateMaterials expects IDs 22 and 23 unregistered).
// ---------------------------------------------------------------------------

namespace VoxelMaterialRegistryTestUtils
{
	/** The configs of an atlas that gives the registry this table's content. */
	TArray<FVoxelMaterialTextureConfig> ConfigsOf(const TArray<FVoxelMaterialDefinition>& Table)
	{
		TArray<FVoxelMaterialTextureConfig> Configs;
		for (const FVoxelMaterialDefinition& Mat : Table)
		{
			FVoxelMaterialTextureConfig& Config = Configs.AddDefaulted_GetRef();
			Config.MaterialID = Mat.MaterialID;
			Config.MaterialName = Mat.Name;
			Config.AtlasColumn = Mat.AtlasColumn;
			Config.AtlasRow = Mat.AtlasRow;
			Config.TriplanarScale = Mat.TriplanarScale;
			Config.UVScale = Mat.UVScale;
			Config.bIsMasked = Mat.bIsMasked;
			Config.bNonOccluding = Mat.bNonOccluding;
		}
		return Configs;
	}

	/** An atlas configuring IDs 0..Count-1 alike: this UV scale, non-occluding or not, each named for its ID. */
	TArray<FVoxelMaterialTextureConfig> UniformConfigs(int32 Count, float UVScale, bool bNonOccluding)
	{
		TArray<FVoxelMaterialTextureConfig> Configs;
		for (int32 ID = 0; ID < Count; ++ID)
		{
			FVoxelMaterialTextureConfig& Config = Configs.AddDefaulted_GetRef();
			Config.MaterialID = static_cast<uint8>(ID);
			Config.MaterialName = FString::Printf(TEXT("Test%d"), ID);
			Config.AtlasColumn = ID % 4;
			Config.AtlasRow = ID / 4;
			Config.UVScale = UVScale;
			Config.bNonOccluding = bNonOccluding;
		}
		return Configs;
	}

	/** Restores the registry's content when it leaves scope. */
	struct FRestoreRegistry
	{
		TArray<FVoxelMaterialTextureConfig> Configs = ConfigsOf(FVoxelMaterialRegistry::GetAllMaterials());
		~FRestoreRegistry() { FVoxelMaterialRegistry::SetAtlasPositions(Configs, 4, 4); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMaterialRegistryAtlasTableTest,
	"VoxelWorlds.MaterialRegistry.AtlasTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMaterialRegistryAtlasTableTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMaterialRegistryTestUtils;
	const FRestoreRegistry Restore;

	// Atlas A: Stone rescaled and Leaves made occluding, and one material past the defaults (ID 30).
	TArray<FVoxelMaterialTextureConfig> AtlasA = ConfigsOf(FVoxelMaterialRegistry::GetAllMaterials());
	AtlasA.SetNum(EVoxelMaterial::Count);
	AtlasA[EVoxelMaterial::Stone].UVScale = 2.5f;
	AtlasA[EVoxelMaterial::Leaves].bNonOccluding = false;
	FVoxelMaterialTextureConfig& Crystal = AtlasA.AddDefaulted_GetRef();
	Crystal.MaterialID = 30;
	Crystal.MaterialName = TEXT("Crystal");
	Crystal.AtlasColumn = 2;
	Crystal.AtlasRow = 7;
	Crystal.TriplanarScale = 0.5f;
	Crystal.bIsMasked = true;
	Crystal.bNonOccluding = true;

	FVoxelMaterialRegistry::SetAtlasPositions(AtlasA, 4, 8);
	const TArray<FVoxelMaterialDefinition>& TableA = FVoxelMaterialRegistry::GetAllMaterials();
	TestEqual(TEXT("atlas A grows the table to its highest ID"), FVoxelMaterialRegistry::GetMaterialCount(), 31);
	const FVoxelMaterialDefinition* CrystalDef = FVoxelMaterialRegistry::GetMaterial(30);
	TestTrue(TEXT("ID 30 is registered"), CrystalDef != nullptr);
	if (!CrystalDef)
	{
		return false;
	}
	TestEqual(TEXT("ID 30 takes its config's name"), CrystalDef->Name, FString(TEXT("Crystal")));
	TestEqual(TEXT("and its atlas row"), CrystalDef->AtlasRow, 7);
	TestTrue(TEXT("and its flags"), CrystalDef->bIsMasked && CrystalDef->bNonOccluding);
	TestEqual(TEXT("and the default hardness"), CrystalDef->Hardness, FVoxelMaterialRegistry::DefaultHardness);
	const FVoxelMaterialDefinition* Gap = FVoxelMaterialRegistry::GetMaterial(25);
	TestTrue(TEXT("the gap holds placeholders"), Gap && Gap->Name == TEXT("Material25") && Gap->ArrayIndex == 25);
	TestEqual(TEXT("Stone takes the atlas's UV scale"), FVoxelMaterialRegistry::GetUVScale(EVoxelMaterial::Stone), 2.5f);
	TestEqual(TEXT("and keeps its hardness (the atlas does not set it)"), FVoxelMaterialRegistry::GetHardness(EVoxelMaterial::Stone), 1.5f);
	TestFalse(TEXT("Leaves are occluding in atlas A"), FVoxelMaterialRegistry::IsNonOccluding(EVoxelMaterial::Leaves));

	// Atlas B configures the defaults only: the table is the defaults' size again, not atlas A's with B on top.
	TArray<FVoxelMaterialTextureConfig> AtlasB = UniformConfigs(EVoxelMaterial::Count, 1.0f, false);
	FVoxelMaterialRegistry::SetAtlasPositions(AtlasB, 4, 8);
	TestEqual(TEXT("atlas B's table has the defaults' size"), FVoxelMaterialRegistry::GetMaterialCount(), static_cast<int32>(EVoxelMaterial::Count));
	TestTrue(TEXT("ID 30 is not registered under atlas B"), FVoxelMaterialRegistry::GetMaterial(30) == nullptr);
	TestEqual(TEXT("Stone's UV scale is atlas B's"), FVoxelMaterialRegistry::GetUVScale(EVoxelMaterial::Stone), 1.0f);

	// What a reader took from atlas A's table is still there, as it was.
	TestEqual(TEXT("a pointer into atlas A's table still reads ID 30"), CrystalDef->MaterialID, static_cast<uint8>(30));
	TestEqual(TEXT("and its name"), CrystalDef->Name, FString(TEXT("Crystal")));
	TestEqual(TEXT("a reference to atlas A's table keeps its size"), TableA.Num(), 31);

	// Atlas A again republishes its table: no copy.
	FVoxelMaterialRegistry::SetAtlasPositions(AtlasA, 4, 8);
	TestTrue(TEXT("atlas A again publishes the table it published before"), &FVoxelMaterialRegistry::GetAllMaterials() == &TableA);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMaterialRegistryWorkerReadsTest,
	"VoxelWorlds.MaterialRegistry.WorkerReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMaterialRegistryWorkerReadsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMaterialRegistryTestUtils;
	const FRestoreRegistry Restore;

	// Atlas A: 24 materials (two past the defaults), UV scale 2, all non-occluding; atlas B: the same 24, UV scale 3, all
	// occluding. A table read whole is all one or all the other.
	constexpr int32 Count = 24;
	const TArray<FVoxelMaterialTextureConfig> AtlasA = UniformConfigs(Count, 2.0f, true);
	const TArray<FVoxelMaterialTextureConfig> AtlasB = UniformConfigs(Count, 3.0f, false);
	FVoxelMaterialRegistry::SetAtlasPositions(AtlasA, 4, 8);

	// Eight workers read the table over and over while the game thread publishes A and B 400 times each.
	TArray<UE::Tasks::FTask> Workers;
	TArray<int32> Reads;
	TArray<int32> Mixed;
	Reads.SetNumZeroed(8);
	Mixed.SetNumZeroed(8);
	std::atomic<bool> bStop{ false };
	for (int32 W = 0; W < 8; ++W)
	{
		Workers.Add(UE::Tasks::Launch(UE_SOURCE_LOCATION, [&OutReads = Reads[W], &OutMixed = Mixed[W], &bStop]()
		{
			while (!bStop.load() || OutReads < 3)
			{
				const TArray<FVoxelMaterialDefinition>& Table = FVoxelMaterialRegistry::GetAllMaterials();
				const int32 Num = Table.Num();
				const float UVScale = Num > 0 ? Table[0].UVScale : 0.0f;
				bool bWhole = Num == Count && (UVScale == 2.0f || UVScale == 3.0f);
				for (int32 Index = 0; bWhole && Index < Count; ++Index)
				{
					bWhole = Table[Index].UVScale == UVScale && Table[Index].bNonOccluding == (UVScale == 2.0f);
				}
				++OutReads;
				OutMixed += bWhole ? 0 : 1;
			}
		}));
	}
	for (int32 Publish = 0; Publish < 800; ++Publish)
	{
		FVoxelMaterialRegistry::SetAtlasPositions(Publish % 2 == 0 ? AtlasB : AtlasA, 4, 8);
		if (Publish % 100 == 0)
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}
	bStop.store(true);
	UE::Tasks::Wait(Workers);

	int32 ReadsDone = 0;
	int32 MixedReads = 0;
	for (int32 W = 0; W < 8; ++W)
	{
		ReadsDone += Reads[W];
		MixedReads += Mixed[W];
	}
	AddInfo(FString::Printf(TEXT("%d reads by 8 workers during 800 publishes"), ReadsDone));
	TestTrue(TEXT("the workers read"), ReadsDone >= 24);
	TestEqual(TEXT("every read sees one atlas's table whole"), MixedReads, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
