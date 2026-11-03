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
// The material tables (G100, G101). The registry answers from the built-in defaults, an immutable table; an atlas's
// materials are its world's table (FVoxelMaterialTable::FromAtlas), which the world holds and its workers read with no
// lock:
//
// - MaterialRegistry.AtlasTable: an atlas's table is the defaults grown to its highest MaterialID, its configs applied,
//   hardness kept; another atlas's is built from the defaults, not on top of the first; neither changes the registry.
// - MaterialRegistry.WorkerReads: workers reading two atlases' tables at once (two worlds) while the game thread builds
//   more see each table whole and their own on every read (G100's torn reads, G101's one table for the process).
//
// - MaterialRegistry.SlopeMaterials: an atlas's table names each material's slope material as the atlas's material LUT
//   does (UVoxelMaterialAtlas::GetSlopeMaterialID), which scatter reads for the material drawn on steep ground (G106).
//
// The chunk managers holding their atlases' tables: VoxelWorlds.Streaming.Cubic.TwoWorldsOwnAtlases.
// ---------------------------------------------------------------------------

namespace VoxelMaterialRegistryTestUtils
{
	/** The configs of an atlas whose table has this table's content. */
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

	/** Whether the registry answers the built-in defaults: 22 materials, Leaves masked and non-occluding, no ID 22. */
	bool RegistryIsDefaults()
	{
		return FVoxelMaterialRegistry::GetMaterialCount() == EVoxelMaterial::Count
			&& FVoxelMaterialRegistry::GetMaterial(EVoxelMaterial::Count) == nullptr
			&& FVoxelMaterialRegistry::IsNonOccluding(EVoxelMaterial::Leaves)
			&& FVoxelMaterialRegistry::GetUVScale(EVoxelMaterial::Stone) == 1.0f
			&& &FVoxelMaterialRegistry::GetAllMaterials() == &FVoxelMaterialTable::Defaults().GetAll();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMaterialRegistryAtlasTableTest,
	"VoxelWorlds.MaterialRegistry.AtlasTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMaterialRegistryAtlasTableTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMaterialRegistryTestUtils;
	TestTrue(TEXT("the registry answers the defaults"), RegistryIsDefaults());

	// Atlas A: Stone rescaled and Leaves made occluding, and one material past the defaults (ID 30).
	TArray<FVoxelMaterialTextureConfig> AtlasA = ConfigsOf(FVoxelMaterialTable::Defaults().GetAll());
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

	const TSharedRef<const FVoxelMaterialTable> TableA = FVoxelMaterialTable::FromAtlas(AtlasA, 4, 8);
	TestEqual(TEXT("atlas A grows its table to its highest ID"), TableA->Num(), 31);
	const FVoxelMaterialDefinition* CrystalDef = TableA->GetMaterial(30);
	TestTrue(TEXT("ID 30 is in atlas A's table"), CrystalDef != nullptr);
	if (!CrystalDef)
	{
		return false;
	}
	TestEqual(TEXT("ID 30 takes its config's name"), CrystalDef->Name, FString(TEXT("Crystal")));
	TestEqual(TEXT("and its atlas row"), CrystalDef->AtlasRow, 7);
	TestTrue(TEXT("and its flags"), CrystalDef->bIsMasked && CrystalDef->bNonOccluding);
	TestEqual(TEXT("and the default hardness"), CrystalDef->Hardness, FVoxelMaterialRegistry::DefaultHardness);
	const FVoxelMaterialDefinition* Gap = TableA->GetMaterial(25);
	TestTrue(TEXT("the gap holds placeholders"), Gap && Gap->Name == TEXT("Material25") && Gap->ArrayIndex == 25);
	TestEqual(TEXT("a placeholder is grey"), TableA->GetMaterialColor(25), FColor(100, 100, 100));
	TestEqual(TEXT("Stone takes the atlas's UV scale"), TableA->GetMaterial(EVoxelMaterial::Stone)->UVScale, 2.5f);
	TestEqual(TEXT("and keeps its hardness (the atlas does not set it)"), TableA->GetMaterial(EVoxelMaterial::Stone)->Hardness, 1.5f);
	TestFalse(TEXT("Leaves are occluding in atlas A"), TableA->IsNonOccluding(EVoxelMaterial::Leaves));
	TestTrue(TEXT("building atlas A's table leaves the registry the defaults"), RegistryIsDefaults());

	// Atlas B configures the defaults only: its table is the defaults' size, not atlas A's with B on top.
	const TSharedRef<const FVoxelMaterialTable> TableB = FVoxelMaterialTable::FromAtlas(UniformConfigs(EVoxelMaterial::Count, 1.0f, false), 4, 8);
	TestEqual(TEXT("atlas B's table has the defaults' size"), TableB->Num(), static_cast<int32>(EVoxelMaterial::Count));
	TestTrue(TEXT("ID 30 is not in atlas B's table"), TableB->GetMaterial(30) == nullptr);
	TestEqual(TEXT("an unknown ID is magenta"), TableB->GetMaterialColor(30), FColor::Magenta);

	// Atlas A's table is as it was.
	TestEqual(TEXT("atlas A's table keeps its size"), TableA->Num(), 31);
	TestEqual(TEXT("and Stone's UV scale"), TableA->GetMaterial(EVoxelMaterial::Stone)->UVScale, 2.5f);
	TestTrue(TEXT("the registry is still the defaults"), RegistryIsDefaults());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMaterialRegistryWorkerReadsTest,
	"VoxelWorlds.MaterialRegistry.WorkerReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMaterialRegistryWorkerReadsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMaterialRegistryTestUtils;

	// Two worlds' atlases: A has 24 materials (two past the defaults), UV scale 2, all non-occluding; B the same 24, UV
	// scale 3, all occluding. A read of A's table whole is all A; of B's, all B.
	constexpr int32 Count = 24;
	const TArray<FVoxelMaterialTextureConfig> AtlasA = UniformConfigs(Count, 2.0f, true);
	const TArray<FVoxelMaterialTextureConfig> AtlasB = UniformConfigs(Count, 3.0f, false);
	const TSharedRef<const FVoxelMaterialTable> TableA = FVoxelMaterialTable::FromAtlas(AtlasA, 4, 8);
	const TSharedRef<const FVoxelMaterialTable> TableB = FVoxelMaterialTable::FromAtlas(AtlasB, 4, 8);

	// Eight workers, half meshing world A and half world B, read their world's table over and over, and the registry,
	// while the game thread builds 800 more tables from the two atlases (as each region component once republished).
	TArray<UE::Tasks::FTask> Workers;
	TArray<int32> Reads;
	TArray<int32> Wrong;
	Reads.SetNumZeroed(8);
	Wrong.SetNumZeroed(8);
	std::atomic<bool> bStop{ false };
	for (int32 W = 0; W < 8; ++W)
	{
		const TSharedRef<const FVoxelMaterialTable> Own = (W % 2 == 0) ? TableA : TableB;
		const float OwnScale = (W % 2 == 0) ? 2.0f : 3.0f;
		Workers.Add(UE::Tasks::Launch(UE_SOURCE_LOCATION, [Own, OwnScale, &OutReads = Reads[W], &OutWrong = Wrong[W], &bStop]()
		{
			while (!bStop.load() || OutReads < 3)
			{
				const TArray<FVoxelMaterialDefinition>& Table = Own->GetAll();
				bool bOwn = Table.Num() == Count;
				for (int32 Index = 0; bOwn && Index < Count; ++Index)
				{
					bOwn = Table[Index].UVScale == OwnScale && Own->IsNonOccluding(static_cast<uint8>(Index)) == (OwnScale == 2.0f);
				}
				bOwn = bOwn && FVoxelMaterialRegistry::GetMaterialCount() == EVoxelMaterial::Count
					&& FVoxelMaterialRegistry::IsNonOccluding(EVoxelMaterial::Leaves);
				++OutReads;
				OutWrong += bOwn ? 0 : 1;
			}
		}));
	}
	int32 Built = 0;
	for (int32 Build = 0; Build < 800; ++Build)
	{
		Built += FVoxelMaterialTable::FromAtlas(Build % 2 == 0 ? AtlasB : AtlasA, 4, 8)->Num() == Count ? 1 : 0;
		if (Build % 100 == 0)
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}
	bStop.store(true);
	UE::Tasks::Wait(Workers);

	int32 ReadsDone = 0;
	int32 WrongReads = 0;
	for (int32 W = 0; W < 8; ++W)
	{
		ReadsDone += Reads[W];
		WrongReads += Wrong[W];
	}
	AddInfo(FString::Printf(TEXT("%d reads by 8 workers while 800 tables were built"), ReadsDone));
	TestEqual(TEXT("the game thread built its tables"), Built, 800);
	TestTrue(TEXT("the workers read"), ReadsDone >= 24);
	TestEqual(TEXT("every read sees its own world's table whole and the registry's defaults"), WrongReads, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMaterialRegistrySlopeMaterialsTest,
	"VoxelWorlds.MaterialRegistry.SlopeMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMaterialRegistrySlopeMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMaterialRegistryTestUtils;

	// Grass turns to Stone, Frozen Dirt to Sand, Dirt to itself, Snow to an ID with no config (40), Sandstone to none.
	UVoxelMaterialAtlas* Atlas = NewObject<UVoxelMaterialAtlas>();
	Atlas->MaterialConfigs = ConfigsOf(FVoxelMaterialTable::Defaults().GetAll());
	Atlas->MaterialConfigs[EVoxelMaterial::Grass].SlopeMaterialID = EVoxelMaterial::Stone;
	Atlas->MaterialConfigs[EVoxelMaterial::FrozenDirt].SlopeMaterialID = EVoxelMaterial::Sand;
	Atlas->MaterialConfigs[EVoxelMaterial::Dirt].SlopeMaterialID = EVoxelMaterial::Dirt;
	Atlas->MaterialConfigs[EVoxelMaterial::Snow].SlopeMaterialID = 40;
	const TSharedRef<const FVoxelMaterialTable> Table = Atlas->MakeMaterialTable();

	int32 Agree = 0;
	for (int32 ID = 0; ID < 256; ++ID)
	{
		const FVoxelMaterialDefinition* Mat = Table->GetMaterial(static_cast<uint8>(ID));
		Agree += (Mat ? Mat->SlopeMaterialID : -1) == Atlas->GetSlopeMaterialID(static_cast<uint8>(ID)) ? 1 : 0;
	}
	TestEqual(TEXT("Every ID's slope material is the atlas LUT's"), Agree, 256);
	TestEqual(TEXT("Grass turns to Stone"), Table->GetMaterial(EVoxelMaterial::Grass)->SlopeMaterialID, static_cast<int32>(EVoxelMaterial::Stone));
	TestEqual(TEXT("Dirt to itself: none"), Table->GetMaterial(EVoxelMaterial::Dirt)->SlopeMaterialID, -1);
	TestEqual(TEXT("Snow to an ID with no config: none"), Table->GetMaterial(EVoxelMaterial::Snow)->SlopeMaterialID, -1);
	TestTrue(TEXT("The table has slope materials"), Table->HasSlopeMaterials());
	TestFalse(TEXT("The defaults have none"), FVoxelMaterialTable::Defaults().HasSlopeMaterials());

	// The drawn material: past the middle of the rock's start and end (cos 0.865, about 30 degrees) Grass draws as Stone.
	const float Middle = 1.0f - 0.5f * (FVoxelMaterialTable::SlopeRockStart + FVoxelMaterialTable::SlopeRockEnd);
	TestEqual(TEXT("Flat grass is grass"), Table->GetDrawnMaterial(EVoxelMaterial::Grass, 1.0f), EVoxelMaterial::Grass);
	TestEqual(TEXT("Grass just under the middle is grass"), Table->GetDrawnMaterial(EVoxelMaterial::Grass, Middle + 0.01f), EVoxelMaterial::Grass);
	TestEqual(TEXT("Grass past the middle is stone"), Table->GetDrawnMaterial(EVoxelMaterial::Grass, Middle - 0.01f), EVoxelMaterial::Stone);
	TestEqual(TEXT("Steep sand stays sand"), Table->GetDrawnMaterial(EVoxelMaterial::Sand, 0.0f), EVoxelMaterial::Sand);
	TestEqual(TEXT("An unknown ID stays itself"), Table->GetDrawnMaterial(200, 0.0f), static_cast<uint8>(200));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
