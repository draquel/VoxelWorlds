// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialParameterCollection.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITORONLY_DATA

/**
 * Visual polish VP2-O2 (VISUAL_POLISH_PLAN.md): the texture variation's wiring in the assets (Scripts/setup_texture_variation.py).
 * MPC_VoxelMaterial holds MacroVariation (0.3) alone (VoxelWorlds 8 retired GradientSampling, TileVariation and
 * ConstantRoughness: the graphs take them as constants); each triplanar function's Custom node includes
 * VoxelTextureVariation.ush and has the inputs the variation reads, GradientSampling and TileVariation a constant 1; and
 * every collection parameter node in the two functions carries the id of the collection scalar it names. A node whose id
 * does not resolve reads another scalar without any error (VoxelWorlds 8's first graph edit drew brighter, flatter sand
 * that way), so voxel.Material.MacroVariation would silently do nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTextureVariationAssetTest,
	"VoxelWorlds.Rendering.TextureVariation.Assets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTextureVariationAssetTest::RunTest(const FString& Parameters)
{
	const UMaterialParameterCollection* Collection =
		LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/VoxelWorlds/Materials/MPC_VoxelMaterial.MPC_VoxelMaterial"));
	if (!TestNotNull(TEXT("MPC_VoxelMaterial loads"), Collection))
	{
		return false;
	}
	const FCollectionScalarParameter* Macro = Collection->ScalarParameters.FindByPredicate(
		[](const FCollectionScalarParameter& P) { return P.ParameterName == TEXT("MacroVariation"); });
	TestEqual(TEXT("The collection holds one scalar"), Collection->ScalarParameters.Num(), 1);
	if (TestNotNull(TEXT("MacroVariation in the collection"), Macro))
	{
		TestEqual(TEXT("MacroVariation defaults to 0.3"), Macro->DefaultValue, 0.3f, 1e-6f);
	}

	struct FFunctionCase
	{
		const TCHAR* Path;
		TArray<FName> VariationInputs;
		int32 CollectionInputs;
	};
	// GradientSampling and TileVariation are constants in both; MacroVariation, in the albedo's, the collection's.
	const FFunctionCase Cases[] = {
		{ TEXT("/VoxelWorlds/Materials/Functions/MF_TriplanarSampleAlbedoRoughness.MF_TriplanarSampleAlbedoRoughness"),
			{ TEXT("GradientSampling"), TEXT("TileVariation"), TEXT("MacroVariation") }, 1 },
		{ TEXT("/VoxelWorlds/Materials/Functions/MF_TriplanarSampleNormal.MF_TriplanarSampleNormal"),
			{ TEXT("GradientSampling"), TEXT("TileVariation") }, 0 },
	};
	const FName ConstantInputs[] = { TEXT("GradientSampling"), TEXT("TileVariation") };
	for (const FFunctionCase& Case : Cases)
	{
		const UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, Case.Path);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Case.Path), Function))
		{
			continue;
		}
		const UMaterialExpressionCustom* Custom = nullptr;
		int32 CollectionNodes = 0;
		for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
		{
			if (const UMaterialExpressionCustom* AsCustom = Cast<UMaterialExpressionCustom>(Expression))
			{
				Custom = AsCustom;
			}
			else if (const UMaterialExpressionCollectionParameter* AsParameter = Cast<UMaterialExpressionCollectionParameter>(Expression))
			{
				++CollectionNodes;
				TestTrue(*FString::Printf(TEXT("%s: %s reads MPC_VoxelMaterial"), *Function->GetName(), *AsParameter->ParameterName.ToString()),
					AsParameter->Collection == Collection);
				const FCollectionScalarParameter* Named = Collection->ScalarParameters.FindByPredicate(
					[AsParameter](const FCollectionScalarParameter& P) { return P.ParameterName == AsParameter->ParameterName; });
				if (TestNotNull(*FString::Printf(TEXT("%s: %s is a collection scalar"), *Function->GetName(), *AsParameter->ParameterName.ToString()), Named))
				{
					TestTrue(*FString::Printf(TEXT("%s: %s carries that scalar's id (%s, the collection's %s)"), *Function->GetName(),
						*AsParameter->ParameterName.ToString(), *AsParameter->ParameterId.ToString(), *Named->Id.ToString()),
						AsParameter->ParameterId == Named->Id);
				}
			}
		}
		if (!TestNotNull(*FString::Printf(TEXT("%s has its Custom node"), *Function->GetName()), Custom))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s includes VoxelTextureVariation.ush"), *Function->GetName()),
			Custom->IncludeFilePaths.Contains(TEXT("/Plugin/VoxelWorlds/Private/VoxelTextureVariation.ush")));
		for (const FName& Input : Case.VariationInputs)
		{
			const FCustomInput* Found = Custom->Inputs.FindByPredicate([&Input](const FCustomInput& I) { return I.InputName == Input; });
			if (TestNotNull(*FString::Printf(TEXT("%s: Custom input %s"), *Function->GetName(), *Input.ToString()), Found))
			{
				TestNotNull(*FString::Printf(TEXT("%s: Custom input %s is connected"), *Function->GetName(), *Input.ToString()),
					Found->Input.Expression);
			}
		}
		for (const FName& Input : ConstantInputs)
		{
			const FCustomInput* Found = Custom->Inputs.FindByPredicate([&Input](const FCustomInput& I) { return I.InputName == Input; });
			const UMaterialExpressionConstant* Constant = Found ? Cast<UMaterialExpressionConstant>(Found->Input.Expression) : nullptr;
			if (TestNotNull(*FString::Printf(TEXT("%s: %s is a constant"), *Function->GetName(), *Input.ToString()), Constant))
			{
				TestEqual(*FString::Printf(TEXT("%s: %s is 1"), *Function->GetName(), *Input.ToString()), Constant->R, 1.0f);
			}
		}
		TestEqual(*FString::Printf(TEXT("%s: one collection parameter node per collection input"), *Function->GetName()),
			CollectionNodes, Case.CollectionInputs);
	}
	return true;
}

#endif
