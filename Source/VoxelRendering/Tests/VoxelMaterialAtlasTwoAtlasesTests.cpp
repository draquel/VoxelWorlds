// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMaterialAtlas.h"
#include "VoxelRegionComponent.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "RenderingThread.h"
#include "RHI.h"             // GUsingNullRHI: no GPU texture under -nullrhi, the GPU half skips there
#include "RHICommandList.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G211: two atlases drawn in turn in one editor session each draw with their own material LUT.
//
// Each atlas built its LUT under one name in the transient package, and UTexture2D::CreateTransient with a name an object
// there already has reconstructs that object in place: the second atlas's build (a PIE on VoxelWorldsTest after one on a
// map with the demo atlas) rewrote the first atlas's LUT, which the first atlas kept and bound again on every later PIE.
// Materials whose slice differs between the two atlases' slice rows (the demo atlas's 4 and 5) then sampled the wrong
// texture array slices until the editor restarted.
//
// The test draws atlas A, then atlas B, then A again on a new component (the next PIE), and checks that each component's
// material instance holds its own atlas's LUT, that A's LUT still carries A's slice row, on the CPU and, with a real RHI,
// in the GPU texture the material samples (-nullrhi has no GPU texture: that half is skipped there, not failed).
// ---------------------------------------------------------------------------

namespace VoxelMaterialAtlasTwoAtlasesTest
{
	// The LUT's slice row (BuildMaterialLUT: row 3, R the albedo slice)
	constexpr int32 SliceRow = 3;

	/** A transient atlas with materials 0-5 and an albedo slice table that sends materials 4 and 5 to the given slices. */
	UVoxelMaterialAtlas* MakeAtlas(const TCHAR* Name, uint8 Slice4, uint8 Slice5)
	{
		UVoxelMaterialAtlas* Atlas = NewObject<UVoxelMaterialAtlas>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UVoxelMaterialAtlas::StaticClass(), FName(Name)));
		for (uint8 MaterialID = 0; MaterialID < 6; ++MaterialID)
		{
			FVoxelMaterialTextureConfig Config;
			Config.MaterialID = MaterialID;
			Config.MaterialName = FString::Printf(TEXT("Material%d"), MaterialID);
			Atlas->MaterialConfigs.Add(Config);
		}
		Atlas->AlbedoArraySlices.SetNumZeroed(256);
		for (int32 MaterialID = 0; MaterialID < 256; ++MaterialID)
		{
			Atlas->AlbedoArraySlices[MaterialID] = static_cast<uint8>(FMath::Min(MaterialID, 3));
		}
		Atlas->AlbedoArraySlices[4] = Slice4;
		Atlas->AlbedoArraySlices[5] = Slice5;
		return Atlas;
	}

	/** The material LUT a component's material instance samples (nullptr without one). */
	UTexture* BoundLUT(UVoxelRegionComponent* Component)
	{
		UTexture* Value = nullptr;
		if (UMaterialInstanceDynamic* Instance = Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0)))
		{
			Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(TEXT("MaterialLUT"))), Value);
		}
		return Value;
	}

	/** The albedo slice (R) of a material in the LUT's CPU copy, or -1 where the texture keeps none. */
	int32 CpuSlice(UTexture2D* LUT, int32 MaterialID)
	{
		FTexturePlatformData* PlatformData = LUT ? LUT->GetPlatformData() : nullptr;
		if (!PlatformData || PlatformData->Mips.Num() == 0)
		{
			return -1;
		}
		FByteBulkData& BulkData = PlatformData->Mips[0].BulkData;
		const uint8* Pixels = static_cast<const uint8*>(BulkData.LockReadOnly());
		const int32 Slice = Pixels ? Pixels[(SliceRow * LUT->GetSizeX() + MaterialID) * 4 + 2] : -1; // BGRA: R at 2
		BulkData.Unlock();
		return Slice;
	}

	/** The albedo slice (R) of a material in the LUT's GPU texture, or -1 without a GPU texture (-nullrhi). */
	int32 GpuSlice(UTexture2D* LUT, int32 MaterialID)
	{
		FlushRenderingCommands(); // the texture's resource is created on the render thread
		FTextureResource* Resource = LUT ? LUT->GetResource() : nullptr;
		if (!Resource)
		{
			return -1;
		}
		TArray<FColor> Texels;
		const FIntRect Rect(0, 0, LUT->GetSizeX(), LUT->GetSizeY());
		ENQUEUE_RENDER_COMMAND(VoxelReadMaterialLUT)([Resource, Rect, &Texels](FRHICommandListImmediate& RHICmdList)
		{
			if (Resource->TextureRHI)
			{
				RHICmdList.ReadSurfaceData(Resource->TextureRHI, Rect, Texels, FReadSurfaceDataFlags());
			}
		});
		FlushRenderingCommands();
		const int32 Index = SliceRow * LUT->GetSizeX() + MaterialID;
		return Texels.IsValidIndex(Index) ? Texels[Index].R : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMaterialAtlasTwoAtlasesTest, "VoxelWorlds.Rendering.MaterialAtlas.TwoAtlasesInTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelMaterialAtlasTwoAtlasesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelMaterialAtlasTwoAtlasesTest;

	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/VoxelWorlds/Materials/M_VoxelMaster.M_VoxelMaster"));
	if (!TestNotNull(TEXT("M_VoxelMaster loads"), Master))
	{
		return false;
	}
	// The atlas is set before the instance exists, and the test atlases have no packed atlas and no texture arrays: the
	// components bind the LUT and say so of the rest
	AddExpectedMessagePlain(TEXT("No DynamicMaterialInstance, skipping"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("PackedAlbedoAtlas is NULL"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("has no AlbedoArray to bind"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	// The demo atlas's materials 4 and 5 and the test atlas's land on different slices
	TStrongObjectPtr<UVoxelMaterialAtlas> AtlasA(MakeAtlas(TEXT("G211AtlasA"), 4, 5));
	TStrongObjectPtr<UVoxelMaterialAtlas> AtlasB(MakeAtlas(TEXT("G211AtlasB"), 1, 9));

	auto Draw = [Master](UVoxelMaterialAtlas* Atlas)
	{
		UVoxelRegionComponent* Component = NewObject<UVoxelRegionComponent>(GetTransientPackage());
		Component->SetMaterialAtlas(Atlas); // as FVoxelMeshRenderer does: the atlas first, then the instance
		Component->CreateVoxelMaterialInstance(Master);
		return TStrongObjectPtr<UVoxelRegionComponent>(Component);
	};

	// The first world draws atlas A, the second atlas B
	TStrongObjectPtr<UVoxelRegionComponent> FirstWorld = Draw(AtlasA.Get());
	UTexture2D* LUTA = AtlasA->GetMaterialLUT();
	if (!LUTA)
	{
		AddInfo(TEXT("No transient texture here (no RHI); nothing to compare"));
		return true;
	}
	TStrongObjectPtr<UVoxelRegionComponent> SecondWorld = Draw(AtlasB.Get());
	UTexture2D* LUTB = AtlasB->GetMaterialLUT();
	TestNotNull(TEXT("Atlas B has a LUT"), LUTB);
	TestTrue(TEXT("Each atlas has a LUT of its own"), LUTA != LUTB);
	TestNotEqual(TEXT("Under a name of its own"), LUTA->GetFName(), LUTB ? LUTB->GetFName() : NAME_None);
	TestTrue(TEXT("The first world's instance samples atlas A's LUT"), BoundLUT(FirstWorld.Get()) == LUTA);
	TestTrue(TEXT("The second world's instance samples atlas B's LUT"), BoundLUT(SecondWorld.Get()) == LUTB);

	// A third world draws atlas A again (the next PIE on the first map): A's LUT, with A's slices
	TStrongObjectPtr<UVoxelRegionComponent> ThirdWorld = Draw(AtlasA.Get());
	TestTrue(TEXT("Atlas A keeps its LUT"), AtlasA->GetMaterialLUT() == LUTA);
	TestTrue(TEXT("The third world's instance samples atlas A's LUT"), BoundLUT(ThirdWorld.Get()) == LUTA);

	const int32 CpuA4 = CpuSlice(LUTA, 4);
	if (CpuA4 >= 0)
	{
		TestEqual(TEXT("A's LUT (CPU): material 4 on A's slice"), CpuA4, 4);
		TestEqual(TEXT("A's LUT (CPU): material 5 on A's slice"), CpuSlice(LUTA, 5), 5);
		TestEqual(TEXT("B's LUT (CPU): material 4 on B's slice"), CpuSlice(LUTB, 4), 1);
		TestEqual(TEXT("B's LUT (CPU): material 5 on B's slice"), CpuSlice(LUTB, 5), 9);
	}
	else
	{
		AddInfo(TEXT("The LUT keeps no CPU copy; the slices are compared on the GPU only"));
	}

	if (GUsingNullRHI)
	{
		AddInfo(TEXT("Skipped: the GPU half needs a real RHI (-nullrhi has no GPU texture to read back)"));
		return true;
	}
	TestEqual(TEXT("A's LUT (GPU): material 4 on A's slice"), GpuSlice(LUTA, 4), 4);
	TestEqual(TEXT("A's LUT (GPU): material 5 on A's slice"), GpuSlice(LUTA, 5), 5);
	TestEqual(TEXT("B's LUT (GPU): material 4 on B's slice"), GpuSlice(LUTB, 4), 1);
	TestEqual(TEXT("B's LUT (GPU): material 5 on B's slice"), GpuSlice(LUTB, 5), 9);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
