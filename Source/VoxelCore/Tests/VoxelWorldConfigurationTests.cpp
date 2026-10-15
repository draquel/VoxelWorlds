// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelWorldConfiguration.h"
#include "VoxelMaterialRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// UVoxelWorldConfiguration::GetSettingsWarnings: settings that load and run but give wrong results, reported
// when the asset loads and when a world starts with it (WORLD_MODES_REVIEW_PLAN.md KC10, WM-H4). Pure, headless.
// ---------------------------------------------------------------------------

namespace VoxelWorldConfigurationTestUtils
{
	bool AnyWarningContains(const TArray<FString>& Warnings, const TCHAR* Text)
	{
		return Warnings.ContainsByPredicate([Text](const FString& W) { return W.Contains(Text); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConfigTreeTemplateMaterialsTest,
	"VoxelWorlds.Configuration.TreeTemplateMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConfigTreeTemplateMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWorldConfigurationTestUtils;

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	TestEqual(TEXT("The default templates (Wood 20, Leaves 21) raise no warning"), Config->GetSettingsWarnings().Num(), 0);

	// DemoSmoothWorldConfig's templates as shipped: leaves 22 and 23, and template 2's trunk on Leaves (21).
	TestTrue(TEXT("Three default templates"), Config->TreeTemplates.Num() == 3);
	if (Config->TreeTemplates.Num() == 3)
	{
		Config->TreeTemplates[0].LeafMaterialID = 22;
		Config->TreeTemplates[1].LeafMaterialID = 23;
		Config->TreeTemplates[2].TrunkMaterialID = EVoxelMaterial::Leaves;
		Config->TreeTemplates[2].AllowedMaterials = { 0, 200 };
	}
	const TArray<FString> Warnings = Config->GetSettingsWarnings();
	for (const FString& Warning : Warnings)
	{
		AddInfo(Warning);
	}
	TestEqual(TEXT("One warning per unregistered id (22, 23, 200)"), Warnings.Num(), 3);
	TestTrue(TEXT("Names the leaf id 22"), AnyWarningContains(Warnings, TEXT("LeafMaterialID 22")));
	TestTrue(TEXT("Names the leaf id 23"), AnyWarningContains(Warnings, TEXT("LeafMaterialID 23")));
	TestTrue(TEXT("Names the allowed ground id 200"), AnyWarningContains(Warnings, TEXT("200")));
	TestFalse(TEXT("A trunk on a registered id is not flagged, even if it is Leaves"), AnyWarningContains(Warnings, TEXT("TrunkMaterialID")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
