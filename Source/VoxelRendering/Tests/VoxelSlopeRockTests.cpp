// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelRegionComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "VoxelMaterialRegistry.h"
#include "ShaderCore.h"
#include "Misc/FileHelper.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

// ---------------------------------------------------------------------------
// Visual polish VP2-O3 (decision D-VP2): slope-aware rock on the smooth terrain.
//
// - Rendering.SlopeRock.Assets: the wiring Scripts/setup_slope_rock.py writes. Each triplanar function's Custom node
//   includes VoxelSlopeRock.ush after VoxelTextureVariation.ush; its FromCenter input is the absolute world position less
//   the LocalUpCenter vector parameter (a Subtract of the two), its RadialUp input that parameter's A; the parameter
//   defaults to 0 (world +Z up). A node input left unlinked reads 0, which would put every heightmap pixel's local up
//   along +Z and every planet's too, with no error.
// - Rendering.SlopeRock.LocalUpParameter: the region component's material instance carries LocalUpCenter: 0 on a
//   heightmap world, the planet's centre and 1 once SetRadialFaceUp turns the radial up on, before or after the instance
//   is made.
// - Rendering.SlopeRock.Thresholds: the shader's VOXEL_SLOPE_ROCK_START and _END are FVoxelMaterialTable's SlopeRockStart
//   and SlopeRockEnd, which scatter places by (G106): a change to one without the other fails here.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSlopeRockAssetTest,
	"VoxelWorlds.Rendering.SlopeRock.Assets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSlopeRockAssetTest::RunTest(const FString& Parameters)
{
	const TCHAR* Paths[] = {
		TEXT("/VoxelWorlds/Materials/Functions/MF_TriplanarSampleAlbedoRoughness.MF_TriplanarSampleAlbedoRoughness"),
		TEXT("/VoxelWorlds/Materials/Functions/MF_TriplanarSampleNormal.MF_TriplanarSampleNormal"),
	};
	for (const TCHAR* Path : Paths)
	{
		const UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, Path);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Path), Function))
		{
			continue;
		}
		const FString Name = Function->GetName();
		const UMaterialExpressionCustom* Custom = nullptr;
		for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
		{
			if (const UMaterialExpressionCustom* AsCustom = Cast<UMaterialExpressionCustom>(Expression))
			{
				Custom = AsCustom;
			}
		}
		if (!TestNotNull(*FString::Printf(TEXT("%s has its Custom node"), *Name), Custom))
		{
			continue;
		}
		const int32 Variation = Custom->IncludeFilePaths.IndexOfByKey(TEXT("/Plugin/VoxelWorlds/Private/VoxelTextureVariation.ush"));
		const int32 Slope = Custom->IncludeFilePaths.IndexOfByKey(TEXT("/Plugin/VoxelWorlds/Private/VoxelSlopeRock.ush"));
		TestTrue(*FString::Printf(TEXT("%s includes VoxelSlopeRock.ush after VoxelTextureVariation.ush"), *Name),
			Variation != INDEX_NONE && Slope > Variation);

		auto FindInput = [Custom](const TCHAR* InputName)
		{
			return Custom->Inputs.FindByPredicate([InputName](const FCustomInput& I) { return I.InputName == FName(InputName); });
		};
		const FCustomInput* FromCenter = FindInput(TEXT("FromCenter"));
		const FCustomInput* RadialUp = FindInput(TEXT("RadialUp"));
		const UMaterialExpressionSubtract* Subtract = FromCenter ? Cast<UMaterialExpressionSubtract>(FromCenter->Input.Expression) : nullptr;
		const UMaterialExpressionVectorParameter* Parameter = RadialUp ? Cast<UMaterialExpressionVectorParameter>(RadialUp->Input.Expression) : nullptr;
		if (!TestNotNull(*FString::Printf(TEXT("%s: FromCenter is a Subtract"), *Name), Subtract)
			|| !TestNotNull(*FString::Printf(TEXT("%s: RadialUp is a vector parameter"), *Name), Parameter))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s: the parameter is LocalUpCenter"), *Name), Parameter->ParameterName, FName(TEXT("LocalUpCenter")));
		TestEqual(*FString::Printf(TEXT("%s: RadialUp reads its A (output 4)"), *Name), RadialUp->Input.OutputIndex, 4);
		TestEqual(*FString::Printf(TEXT("%s: LocalUpCenter defaults to 0 (world +Z)"), *Name), Parameter->DefaultValue, FLinearColor(0, 0, 0, 0));
		TestNotNull(*FString::Printf(TEXT("%s: FromCenter = the world position..."), *Name),
			Cast<UMaterialExpressionWorldPosition>(Subtract->A.Expression));
		TestTrue(*FString::Printf(TEXT("%s: ... less LocalUpCenter"), *Name), Subtract->B.Expression == Parameter);
		TestEqual(*FString::Printf(TEXT("%s: ... its RGB (output 0)"), *Name), Subtract->B.OutputIndex, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSlopeRockLocalUpParameterTest,
	"VoxelWorlds.Rendering.SlopeRock.LocalUpParameter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSlopeRockLocalUpParameterTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/VoxelWorlds/Materials/M_VoxelMaster.M_VoxelMaster"));
	if (!TestNotNull(TEXT("M_VoxelMaster loads"), Master))
	{
		return false;
	}
	// The components have no atlas: UpdateMaterialAtlasParameters says so and sets what needs none
	AddExpectedMessagePlain(TEXT("No MaterialAtlas, skipping"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	const FName ParamName(TEXT("LocalUpCenter"));
	auto Read = [&ParamName](UVoxelRegionComponent* Component)
	{
		FLinearColor Value(-1, -1, -1, -1);
		UMaterialInstanceDynamic* Instance = Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0));
		if (Instance)
		{
			Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(ParamName), Value);
		}
		return Value;
	};

	UVoxelRegionComponent* Plane = NewObject<UVoxelRegionComponent>(GetTransientPackage());
	TestNotNull(TEXT("Heightmap: an instance"), Plane->CreateVoxelMaterialInstance(Master));
	TestEqual(TEXT("Heightmap: LocalUpCenter 0, world +Z"), Read(Plane), FLinearColor(0, 0, 0, 0));

	// The renderer sets the radial up before the instance exists (FVoxelMeshRenderer::CreateRegionComponent)
	UVoxelRegionComponent* Before = NewObject<UVoxelRegionComponent>(GetTransientPackage());
	Before->SetRadialFaceUp(true, FVector(1000.0, -2000.0, 3000.0));
	TestNotNull(TEXT("Planet: an instance"), Before->CreateVoxelMaterialInstance(Master));
	TestEqual(TEXT("Planet: the centre and radial"), Read(Before), FLinearColor(1000.0f, -2000.0f, 3000.0f, 1.0f));

	// ... and a change after it reaches the instance
	UVoxelRegionComponent* After = NewObject<UVoxelRegionComponent>(GetTransientPackage());
	TestNotNull(TEXT("Later: an instance"), After->CreateVoxelMaterialInstance(Master));
	After->SetRadialFaceUp(true, FVector(5.0, 6.0, 7.0));
	TestEqual(TEXT("Later: the centre and radial"), Read(After), FLinearColor(5.0f, 6.0f, 7.0f, 1.0f));
	After->SetRadialFaceUp(false, FVector::ZeroVector);
	TestEqual(TEXT("Later: back to world +Z"), Read(After), FLinearColor(0, 0, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSlopeRockThresholdsTest,
	"VoxelWorlds.Rendering.SlopeRock.Thresholds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSlopeRockThresholdsTest::RunTest(const FString& Parameters)
{
	const FString Path = GetShaderSourceFilePath(TEXT("/Plugin/VoxelWorlds/Private/VoxelSlopeRock.ush"));
	FString Source;
	if (!TestTrue(*FString::Printf(TEXT("VoxelSlopeRock.ush reads (%s)"), *Path), FFileHelper::LoadFileToString(Source, *Path)))
	{
		return false;
	}
	auto Define = [&Source](const TCHAR* Name, float& OutValue)
	{
		const FString Key = FString::Printf(TEXT("#define %s "), Name);
		const int32 At = Source.Find(Key, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE)
		{
			return false;
		}
		int32 End = Source.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		End = End == INDEX_NONE ? Source.Len() : End;
		OutValue = FCString::Atof(*Source.Mid(At + Key.Len(), End - At - Key.Len()).TrimStartAndEnd());
		return true;
	};
	float Start = -1.0f, End = -1.0f;
	TestTrue(TEXT("The include defines VOXEL_SLOPE_ROCK_START"), Define(TEXT("VOXEL_SLOPE_ROCK_START"), Start));
	TestTrue(TEXT("and VOXEL_SLOPE_ROCK_END"), Define(TEXT("VOXEL_SLOPE_ROCK_END"), End));
	TestEqual(TEXT("The shader's start is scatter's"), Start, FVoxelMaterialTable::SlopeRockStart);
	TestEqual(TEXT("The shader's end is scatter's"), End, FVoxelMaterialTable::SlopeRockEnd);
	return true;
}

#endif
