// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeLookup.h"

// The biome lookup the master materials read for the weather's snow and wetness (P3): one texel per biome ID with the
// ends of the biome's temperature range mapped from [-1, 1] to a byte, B 255 where the biome exists; the middle elsewhere.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelBiomeLookupEncodeTest, "VoxelWorlds.Rendering.BiomeLookup.Encode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelBiomeLookupEncodeTest::RunTest(const FString& Parameters)
{
	TArray<FColor> Texels;
	FVoxelBiomeLookup::Encode(nullptr, Texels);
	TestEqual(TEXT("256 texels without a configuration"), Texels.Num(), FVoxelBiomeLookup::Width);
	TestTrue(TEXT("The middle, no biome, without a configuration"), Texels[0] == FColor(128, 128, 0, 255) && Texels[255] == FColor(128, 128, 0, 255));

	UVoxelBiomeConfiguration* Config = NewObject<UVoxelBiomeConfiguration>();
	Config->AddToRoot();
	Config->Biomes.Empty();
	FBiomeDefinition Tundra;
	Tundra.BiomeID = 3;
	Tundra.Name = TEXT("Tundra");
	Tundra.TemperatureRange = FVector2D(-1.0, -0.6); // low -1 -> 0 -> 0; high -0.6 -> 0.2 -> 51
	Tundra.MoistureRange = FVector2D(0.0, 1.0);
	Config->Biomes.Add(Tundra);
	FBiomeDefinition Desert;
	Desert.BiomeID = 7;
	Desert.Name = TEXT("Desert");
	Desert.TemperatureRange = FVector2D(1.0, 0.4);   // the ends in either order: low 0.4 -> 0.7 -> 178 (178.5 in double, down), high 1 -> 255
	Desert.MoistureRange = FVector2D(-1.0, -0.5);
	Config->Biomes.Add(Desert);
	Config->RebuildCaches(); // the ID index is cached; a direct edit of the array must rebuild it

	FVoxelBiomeLookup::Encode(Config, Texels);
	TestEqual(TEXT("256 texels"), Texels.Num(), FVoxelBiomeLookup::Width);
	TestTrue(*FString::Printf(TEXT("The tundra's texel: its range's ends, present (%s)"), *Texels[3].ToString()), Texels[3] == FColor(0, 51, 255, 255));
	TestTrue(*FString::Printf(TEXT("The desert's texel: its range's ends in order, present (%s)"), *Texels[7].ToString()),
		Texels[7].R >= 178 && Texels[7].R <= 179 && Texels[7].G == 255 && Texels[7].B == 255);
	TestTrue(TEXT("An unused ID keeps the middle and B 0"), Texels[5] == FColor(128, 128, 0, 255));
	Config->RemoveFromRoot();
	return true;
}
