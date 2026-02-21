// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelBiomeDefinition.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelMaterialRegistry.h"

// ==================== Biome Blending Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeBlendStructTest, "VoxelWorlds.Biome.BlendStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeBlendStructTest::RunTest(const FString& Parameters)
{
	// Test single biome blend
	FBiomeBlend SingleBlend(EVoxelBiome::Plains);
	TestEqual(TEXT("Single blend should have 1 biome"), SingleBlend.BiomeCount, 1);
	TestEqual(TEXT("Single blend dominant should be Plains"), SingleBlend.GetDominantBiome(), EVoxelBiome::Plains);
	TestEqual(TEXT("Single blend weight should be 1.0"), SingleBlend.Weights[0], 1.0f);
	TestFalse(TEXT("Single blend should not be blending"), SingleBlend.IsBlending());

	// Test blend normalization
	FBiomeBlend ManualBlend;
	ManualBlend.BiomeCount = 2;
	ManualBlend.BiomeIDs[0] = EVoxelBiome::Plains;
	ManualBlend.BiomeIDs[1] = EVoxelBiome::Forest;
	ManualBlend.Weights[0] = 3.0f;
	ManualBlend.Weights[1] = 1.0f;
	ManualBlend.NormalizeWeights();

	TestTrue(TEXT("After normalization, weight 0 should be ~0.75"),
		FMath::IsNearlyEqual(ManualBlend.Weights[0], 0.75f, 0.01f));
	TestTrue(TEXT("After normalization, weight 1 should be ~0.25"),
		FMath::IsNearlyEqual(ManualBlend.Weights[1], 0.25f, 0.01f));
	TestTrue(TEXT("Multi-biome blend should be blending"), ManualBlend.IsBlending());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeSelectionTest, "VoxelWorlds.Biome.Selection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeSelectionTest::RunTest(const FString& Parameters)
{
	// Test biome selection at various temperature/moisture combinations

	// Cold = Mountain
	const FBiomeDefinition* ColdBiome = FVoxelBiomeRegistry::SelectBiome(-0.8f, 0.0f);
	TestNotNull(TEXT("Cold biome should be found"), ColdBiome);
	if (ColdBiome)
	{
		TestEqual(TEXT("Cold biome should be Mountain"), ColdBiome->BiomeID, EVoxelBiome::Mountain);
	}

	// Humid = Forest
	const FBiomeDefinition* HumidBiome = FVoxelBiomeRegistry::SelectBiome(0.3f, 0.5f);
	TestNotNull(TEXT("Humid biome should be found"), HumidBiome);
	if (HumidBiome)
	{
		TestEqual(TEXT("Humid biome should be Forest"), HumidBiome->BiomeID, EVoxelBiome::Forest);
	}

	// Temperate + moderate moisture = Plains (default)
	const FBiomeDefinition* TempBiome = FVoxelBiomeRegistry::SelectBiome(0.0f, 0.0f);
	TestNotNull(TEXT("Temperate biome should be found"), TempBiome);
	if (TempBiome)
	{
		TestEqual(TEXT("Temperate biome should be Plains"), TempBiome->BiomeID, EVoxelBiome::Plains);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeBlendingTest, "VoxelWorlds.Biome.Blending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeBlendingTest::RunTest(const FString& Parameters)
{
	// Test blending at biome boundaries

	// Well inside Plains - should be single biome
	FBiomeBlend CenterPlains = FVoxelBiomeRegistry::GetBiomeBlend(0.2f, -0.1f, 0.1f);
	TestEqual(TEXT("Center Plains should have dominant Plains"),
		CenterPlains.GetDominantBiome(), EVoxelBiome::Plains);
	TestTrue(TEXT("Center Plains should have high weight for Plains"),
		CenterPlains.Weights[0] > 0.8f);

	// Near Mountain boundary (temperature ~-0.1)
	FBiomeBlend NearMountain = FVoxelBiomeRegistry::GetBiomeBlend(-0.05f, 0.0f, 0.15f);
	AddInfo(FString::Printf(TEXT("Near Mountain blend: BiomeCount=%d, Weights=[%.2f, %.2f]"),
		NearMountain.BiomeCount, NearMountain.Weights[0], NearMountain.Weights[1]));

	// Should have some blending (may be 1 or 2 biomes depending on exact position)
	TestTrue(TEXT("Near boundary blend count should be reasonable"),
		NearMountain.BiomeCount >= 1 && NearMountain.BiomeCount <= MAX_BIOME_BLEND);

	// Well inside Mountain - should be single biome
	FBiomeBlend CenterMountain = FVoxelBiomeRegistry::GetBiomeBlend(-0.8f, 0.0f, 0.1f);
	TestEqual(TEXT("Center Mountain should have dominant Mountain"),
		CenterMountain.GetDominantBiome(), EVoxelBiome::Mountain);

	// Verify blend weights sum to 1.0
	float TotalWeight = 0.0f;
	for (int32 i = 0; i < NearMountain.BiomeCount; ++i)
	{
		TotalWeight += NearMountain.Weights[i];
	}
	TestTrue(TEXT("Blend weights should sum to 1.0"),
		FMath::IsNearlyEqual(TotalWeight, 1.0f, 0.01f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeBlendMaterialTest, "VoxelWorlds.Biome.BlendMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeBlendMaterialTest::RunTest(const FString& Parameters)
{
	// Test material selection from blended biomes

	// Single biome - should use biome's material
	FBiomeBlend SingleBiome(EVoxelBiome::Plains);
	uint8 SurfaceMaterial = FVoxelBiomeRegistry::GetBlendedMaterial(SingleBiome, 0.0f);
	TestEqual(TEXT("Plains surface should be Grass"), SurfaceMaterial, EVoxelMaterial::Grass);

	uint8 DeepMaterial = FVoxelBiomeRegistry::GetBlendedMaterial(SingleBiome, 10.0f);
	TestEqual(TEXT("Plains deep should be Stone"), DeepMaterial, EVoxelMaterial::Stone);

	// Mountain biome
	FBiomeBlend MountainBiome(EVoxelBiome::Mountain);
	uint8 MountainSurface = FVoxelBiomeRegistry::GetBlendedMaterial(MountainBiome, 0.0f);
	TestEqual(TEXT("Mountain surface should be Stone"), MountainSurface, EVoxelMaterial::Stone);

	// Forest biome
	FBiomeBlend ForestBiome(EVoxelBiome::Forest);
	uint8 ForestSurface = FVoxelBiomeRegistry::GetBlendedMaterial(ForestBiome, 0.0f);
	TestEqual(TEXT("Forest surface should be Grass"), ForestSurface, EVoxelMaterial::Grass);

	return true;
}

// ==================== Height Material Override Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeightMaterialRuleTest, "VoxelWorlds.Biome.HeightMaterialRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHeightMaterialRuleTest::RunTest(const FString& Parameters)
{
	// Test FHeightMaterialRule::Applies()

	// Snow above 4000 units, surface only
	FHeightMaterialRule SnowRule(4000.0f, MAX_FLT, EVoxelMaterial::Snow, true, 1.0f, 100);

	// Should apply at high altitude surface
	TestTrue(TEXT("Snow rule should apply at 5000, depth 0"),
		SnowRule.Applies(5000.0f, 0.0f));
	TestTrue(TEXT("Snow rule should apply at 4001, depth 0.5"),
		SnowRule.Applies(4001.0f, 0.5f));

	// Should NOT apply below threshold
	TestFalse(TEXT("Snow rule should NOT apply at 3000, depth 0"),
		SnowRule.Applies(3000.0f, 0.0f));

	// Should NOT apply deep underground (bSurfaceOnly)
	TestFalse(TEXT("Snow rule should NOT apply at 5000, depth 5"),
		SnowRule.Applies(5000.0f, 5.0f));

	// Rock rule in range
	FHeightMaterialRule RockRule(3000.0f, 4000.0f, EVoxelMaterial::Stone, true, 2.0f, 50);

	TestTrue(TEXT("Rock rule should apply at 3500, depth 1"),
		RockRule.Applies(3500.0f, 1.0f));
	TestFalse(TEXT("Rock rule should NOT apply at 4500 (above range)"),
		RockRule.Applies(4500.0f, 0.0f));
	TestFalse(TEXT("Rock rule should NOT apply at 2500 (below range)"),
		RockRule.Applies(2500.0f, 0.0f));

	// Non-surface-only rule
	FHeightMaterialRule DeepRule(0.0f, 1000.0f, EVoxelMaterial::Dirt, false, 0.0f, 10);

	TestTrue(TEXT("Deep rule should apply at any depth"),
		DeepRule.Applies(500.0f, 100.0f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeDefinitionDistanceTest, "VoxelWorlds.Biome.DefinitionDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeDefinitionDistanceTest::RunTest(const FString& Parameters)
{
	// Test FBiomeDefinition distance functions

	FBiomeDefinition TestBiome;
	TestBiome.TemperatureRange = FVector2D(-0.5f, 0.5f);
	TestBiome.MoistureRange = FVector2D(-0.3f, 0.3f);

	// Center should have signed distance ~0.3 (distance to nearest edge)
	float CenterDist = TestBiome.GetSignedDistanceToEdge(0.0f, 0.0f);
	TestTrue(TEXT("Center should have positive signed distance (inside)"),
		CenterDist > 0.0f);
	TestTrue(TEXT("Center signed distance should be ~0.3 (moisture edge)"),
		FMath::IsNearlyEqual(CenterDist, 0.3f, 0.01f));

	// Point outside temperature range
	float OutsideTempDist = TestBiome.GetSignedDistanceToEdge(0.7f, 0.0f);
	TestTrue(TEXT("Point outside temp range should have negative signed distance"),
		OutsideTempDist < 0.0f);

	// Point on edge
	float EdgeDist = TestBiome.GetSignedDistanceToEdge(0.5f, 0.0f);
	TestTrue(TEXT("Point on edge should have ~0 signed distance"),
		FMath::IsNearlyEqual(EdgeDist, 0.0f, 0.01f));

	// Test Contains() method
	TestTrue(TEXT("Center should be contained"), TestBiome.Contains(0.0f, 0.0f));
	TestFalse(TEXT("Outside point should NOT be contained"), TestBiome.Contains(0.7f, 0.0f));

	return true;
}

// ==================== Integration Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeRegistryTest, "VoxelWorlds.Biome.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeRegistryTest::RunTest(const FString& Parameters)
{
	// Test biome registry functions

	// Check biome count
	int32 BiomeCount = FVoxelBiomeRegistry::GetBiomeCount();
	TestTrue(TEXT("Should have at least 4 biomes"), BiomeCount >= 4);

	// Check all biomes are accessible
	const TArray<FBiomeDefinition>& AllBiomes = FVoxelBiomeRegistry::GetAllBiomes();
	TestEqual(TEXT("GetAllBiomes should match GetBiomeCount"), AllBiomes.Num(), BiomeCount);

	// Check GetBiome by ID
	const FBiomeDefinition* Plains = FVoxelBiomeRegistry::GetBiome(EVoxelBiome::Plains);
	TestNotNull(TEXT("Plains biome should exist"), Plains);
	if (Plains)
	{
		TestEqual(TEXT("Plains ID should match"), Plains->BiomeID, EVoxelBiome::Plains);
		TestTrue(TEXT("Plains should have valid name"), Plains->Name.Len() > 0);
	}

	const FBiomeDefinition* Forest = FVoxelBiomeRegistry::GetBiome(EVoxelBiome::Forest);
	TestNotNull(TEXT("Forest biome should exist"), Forest);

	const FBiomeDefinition* Mountain = FVoxelBiomeRegistry::GetBiome(EVoxelBiome::Mountain);
	TestNotNull(TEXT("Mountain biome should exist"), Mountain);

	const FBiomeDefinition* Ocean = FVoxelBiomeRegistry::GetBiome(EVoxelBiome::Ocean);
	TestNotNull(TEXT("Ocean biome should exist"), Ocean);

	// Invalid biome ID should return nullptr
	const FBiomeDefinition* Invalid = FVoxelBiomeRegistry::GetBiome(255);
	TestNull(TEXT("Invalid biome ID should return nullptr"), Invalid);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeMaterialDepthTest, "VoxelWorlds.Biome.MaterialDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeMaterialDepthTest::RunTest(const FString& Parameters)
{
	// Test FBiomeDefinition::GetMaterialAtDepth()

	const FBiomeDefinition* Plains = FVoxelBiomeRegistry::GetBiome(EVoxelBiome::Plains);
	TestNotNull(TEXT("Plains biome should exist"), Plains);
	if (!Plains) return false;

	// Surface (depth 0)
	uint8 Surface = Plains->GetMaterialAtDepth(0.0f);
	TestEqual(TEXT("Plains surface should be Grass"), Surface, EVoxelMaterial::Grass);

	// Subsurface (depth 2)
	uint8 Subsurface = Plains->GetMaterialAtDepth(2.0f);
	TestEqual(TEXT("Plains subsurface should be Dirt"), Subsurface, EVoxelMaterial::Dirt);

	// Deep (depth 10)
	uint8 Deep = Plains->GetMaterialAtDepth(10.0f);
	TestEqual(TEXT("Plains deep should be Stone"), Deep, EVoxelMaterial::Stone);

	// Test Forest
	const FBiomeDefinition* Forest = FVoxelBiomeRegistry::GetBiome(EVoxelBiome::Forest);
	if (Forest)
	{
		TestEqual(TEXT("Forest surface should be Grass"),
			Forest->GetMaterialAtDepth(0.0f), EVoxelMaterial::Grass);
		TestEqual(TEXT("Forest subsurface should be Dirt"),
			Forest->GetMaterialAtDepth(2.0f), EVoxelMaterial::Dirt);
	}

	// Test Mountain
	const FBiomeDefinition* Mountain = FVoxelBiomeRegistry::GetBiome(EVoxelBiome::Mountain);
	if (Mountain)
	{
		TestEqual(TEXT("Mountain surface should be Stone"),
			Mountain->GetMaterialAtDepth(0.0f), EVoxelMaterial::Stone);
		TestEqual(TEXT("Mountain subsurface should be Stone"),
			Mountain->GetMaterialAtDepth(2.0f), EVoxelMaterial::Stone);
	}

	return true;
}

// ==================== UVoxelBiomeConfiguration Tests ====================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeConfigurationInitDefaultsTest, "VoxelWorlds.Biome.Config.InitDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeConfigurationInitDefaultsTest::RunTest(const FString& Parameters)
{
	// Create a new biome configuration
	UVoxelBiomeConfiguration* Config = NewObject<UVoxelBiomeConfiguration>();
	TestNotNull(TEXT("Config should be created"), Config);
	if (!Config) return false;

	// Constructor should call InitializeDefaults
	TestTrue(TEXT("Config should be valid after construction"), Config->IsValid());
	TestTrue(TEXT("Config should have 4 biomes"), Config->GetBiomeCount() >= 4);

	// Check default biomes exist
	const FBiomeDefinition* Plains = Config->GetBiome(0);
	TestNotNull(TEXT("Plains (ID 0) should exist"), Plains);
	if (Plains)
	{
		TestEqual(TEXT("Plains name should be 'Plains'"), Plains->Name, FString(TEXT("Plains")));
		TestEqual(TEXT("Plains surface should be Grass"), Plains->SurfaceMaterial, EVoxelMaterial::Grass);
	}

	const FBiomeDefinition* Forest = Config->GetBiome(1);
	TestNotNull(TEXT("Forest (ID 1) should exist"), Forest);
	if (Forest)
	{
		TestEqual(TEXT("Forest surface should be Grass"), Forest->SurfaceMaterial, EVoxelMaterial::Grass);
	}

	const FBiomeDefinition* Mountain = Config->GetBiome(2);
	TestNotNull(TEXT("Mountain (ID 2) should exist"), Mountain);
	if (Mountain)
	{
		TestEqual(TEXT("Mountain surface should be Stone"), Mountain->SurfaceMaterial, EVoxelMaterial::Stone);
	}

	const FBiomeDefinition* Ocean = Config->GetBiome(3);
	TestNotNull(TEXT("Ocean (ID 3) should exist"), Ocean);
	if (Ocean)
	{
		TestEqual(TEXT("Ocean surface should be Sand"), Ocean->SurfaceMaterial, EVoxelMaterial::Sand);
	}

	// Check default height rules
	TestTrue(TEXT("Height materials should be enabled by default"), Config->bEnableHeightMaterials);
	TestTrue(TEXT("Should have default height rules"), Config->HeightMaterialRules.Num() >= 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeConfigurationSelectionTest, "VoxelWorlds.Biome.Config.Selection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeConfigurationSelectionTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* Config = NewObject<UVoxelBiomeConfiguration>();
	TestNotNull(TEXT("Config should be created"), Config);
	if (!Config) return false;

	// Test biome selection
	// Cold + inland = Mountain
	uint8 ColdBiome = Config->SelectBiomeID(-0.8f, 0.0f, 0.5f);
	TestEqual(TEXT("Cold+inland should select Mountain"), ColdBiome, (uint8)2);

	// Warm + humid = Forest
	uint8 HumidBiome = Config->SelectBiomeID(0.3f, 0.5f, 0.5f);
	TestEqual(TEXT("Warm+humid should select Forest"), HumidBiome, (uint8)1);

	// Temperate + moderate moisture = Plains
	uint8 TempBiome = Config->SelectBiomeID(0.2f, 0.0f, 0.5f);
	TestEqual(TEXT("Temperate should select Plains"), TempBiome, (uint8)0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeConfigurationBlendingTest, "VoxelWorlds.Biome.Config.Blending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeConfigurationBlendingTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* Config = NewObject<UVoxelBiomeConfiguration>();
	TestNotNull(TEXT("Config should be created"), Config);
	if (!Config) return false;

	// Test blending - well inside Plains
	FBiomeBlend CenterBlend = Config->GetBiomeBlend(0.2f, 0.0f, 0.5f);
	TestEqual(TEXT("Center should have Plains dominant"), CenterBlend.GetDominantBiome(), (uint8)0);
	TestTrue(TEXT("Center should have high weight"), CenterBlend.Weights[0] > 0.8f);

	// Test blending - near Mountain boundary
	FBiomeBlend NearMountain = Config->GetBiomeBlend(-0.05f, 0.0f, 0.5f);
	AddInfo(FString::Printf(TEXT("Near Mountain: BiomeCount=%d, Dominant=%d, Weight=%.2f"),
		NearMountain.BiomeCount, NearMountain.GetDominantBiome(), NearMountain.Weights[0]));

	// Weights should sum to 1.0
	float TotalWeight = 0.0f;
	for (int32 i = 0; i < NearMountain.BiomeCount; ++i)
	{
		TotalWeight += NearMountain.Weights[i];
	}
	TestTrue(TEXT("Blend weights should sum to 1.0"),
		FMath::IsNearlyEqual(TotalWeight, 1.0f, 0.01f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeConfigurationHeightRulesTest, "VoxelWorlds.Biome.Config.HeightRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeConfigurationHeightRulesTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* Config = NewObject<UVoxelBiomeConfiguration>();
	TestNotNull(TEXT("Config should be created"), Config);
	if (!Config) return false;

	// Test height rule application
	// At low altitude, grass should stay grass
	uint8 LowAltMaterial = Config->ApplyHeightMaterialRules(EVoxelMaterial::Grass, 1000.0f, 0.0f);
	TestEqual(TEXT("Low altitude grass should stay grass"), LowAltMaterial, EVoxelMaterial::Grass);

	// At high altitude (>4000), should become snow
	uint8 HighAltMaterial = Config->ApplyHeightMaterialRules(EVoxelMaterial::Grass, 5000.0f, 0.0f);
	TestEqual(TEXT("High altitude should become snow"), HighAltMaterial, EVoxelMaterial::Snow);

	// At mid-high altitude (3000-4000), should become stone
	uint8 MidHighMaterial = Config->ApplyHeightMaterialRules(EVoxelMaterial::Grass, 3500.0f, 0.0f);
	TestEqual(TEXT("Mid-high altitude should become stone"), MidHighMaterial, EVoxelMaterial::Stone);

	// Deep underground at high altitude should NOT get snow (surface only rule)
	uint8 DeepHighAlt = Config->ApplyHeightMaterialRules(EVoxelMaterial::Grass, 5000.0f, 10.0f);
	TestEqual(TEXT("Deep at high altitude should stay original"), DeepHighAlt, EVoxelMaterial::Grass);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBiomeConfigurationBlendedMaterialTest, "VoxelWorlds.Biome.Config.BlendedMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBiomeConfigurationBlendedMaterialTest::RunTest(const FString& Parameters)
{
	UVoxelBiomeConfiguration* Config = NewObject<UVoxelBiomeConfiguration>();
	TestNotNull(TEXT("Config should be created"), Config);
	if (!Config) return false;

	// Test single biome blend
	FBiomeBlend PlainsBlend(0); // Plains
	uint8 PlainsSurface = Config->GetBlendedMaterial(PlainsBlend, 0.0f);
	TestEqual(TEXT("Plains blend surface should be Grass"), PlainsSurface, EVoxelMaterial::Grass);

	uint8 PlainsDeep = Config->GetBlendedMaterial(PlainsBlend, 10.0f);
	TestEqual(TEXT("Plains blend deep should be Stone"), PlainsDeep, EVoxelMaterial::Stone);

	// Test Mountain blend
	FBiomeBlend MountainBlend(2); // Mountain
	uint8 MountainSurface = Config->GetBlendedMaterial(MountainBlend, 0.0f);
	TestEqual(TEXT("Mountain blend surface should be Stone"), MountainSurface, EVoxelMaterial::Stone);

	return true;
}
