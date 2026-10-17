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

/**
 * Spherical Phase 0 guardrails (SPHERICAL_PLANET_REQUIREMENTS.md SP-A5, SP-B3): the terrain shell must cover the
 * terrain's real amplitude, water on a planet is flagged while consumers still read the Z-plane WaterLevel. Since
 * spherical Phase 2 (SP-B4) a planet may generate on the GPU like any other world mode.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelConfigPlanetSettingsTest,
	"VoxelWorlds.Configuration.PlanetSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelConfigPlanetSettingsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelWorldConfigurationTestUtils;

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->WorldMode = EWorldMode::SphericalPlanet;
	Config->bEnableWaterLevel = false;

	// The defaults: PlanetHeightScale 5,000 puts the terrain 2,500 uu either side of the radius, and the shell
	// is 5,000 up but only 2,000 down, so the deepest valleys are cut off.
	TArray<FString> Warnings = Config->GetSettingsWarnings();
	TestEqual(TEXT("Defaults: one warning"), Warnings.Num(), 1);
	TestTrue(TEXT("Defaults: the depth does not cover the valleys"), AnyWarningContains(Warnings, TEXT("PlanetMaxTerrainDepth")));

	Config->PlanetMaxTerrainDepth = 2500.0f;
	TestEqual(TEXT("A shell that covers the amplitude raises nothing"), Config->GetSettingsWarnings().Num(), 0);

	// BaseHeight shifts the whole surface: 3,000 lifts the peaks to 5,500 and the valleys to 500 above the radius.
	Config->BaseHeight = 3000.0f;
	Warnings = Config->GetSettingsWarnings();
	TestTrue(TEXT("BaseHeight lifts the peaks past the shell"), AnyWarningContains(Warnings, TEXT("PlanetMaxTerrainHeight")));
	TestFalse(TEXT("... and the valleys clear of the depth"), AnyWarningContains(Warnings, TEXT("PlanetMaxTerrainDepth")));
	Config->BaseHeight = 0.0f;

	Config->bEnableWaterLevel = true;
	Warnings = Config->GetSettingsWarnings();
	TestTrue(TEXT("Water on a planet is flagged"), AnyWarningContains(Warnings, TEXT("WaterLevel")));

	// The same settings on a heightmap world raise none of the planet warnings.
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->PlanetMaxTerrainDepth = 100.0f;
	TestEqual(TEXT("Planet checks are planet-only"), Config->GetSettingsWarnings().Num(), 0);

	// GPU generation follows bUseGPUGeneration in every mode: planets too since spherical Phase 2 (SP-B4; SP-B3 held
	// them on the CPU before the GPU generator had a planet branch).
	Config->bUseGPUGeneration = true;
	TestTrue(TEXT("InfinitePlane may generate on the GPU"), Config->ShouldUseGPUGeneration());
	Config->WorldMode = EWorldMode::IslandBowl;
	TestTrue(TEXT("IslandBowl may generate on the GPU"), Config->ShouldUseGPUGeneration());
	Config->WorldMode = EWorldMode::SphericalPlanet;
	TestTrue(TEXT("A planet may generate on the GPU (SP-B4)"), Config->ShouldUseGPUGeneration());
	Config->bUseGPUGeneration = false;
	TestFalse(TEXT("bUseGPUGeneration off keeps a planet on the CPU"), Config->ShouldUseGPUGeneration());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
