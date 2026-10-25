// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMaterialAtlas.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

// ---------------------------------------------------------------------------
// The material atlas's texture arrays (VISUAL_POLISH_PLAN.md, "Material atlas texture arrays"). They were built by
// UTexture2DArray::UpdateSourceFromSourceTextures(), which sized every slice to the largest source texture and converted
// all of them to RGBA32F when their gamma spaces differed: on the demo atlas 4096 x 4096 x 22 float slices for albedo
// and normals (5.9 GB of source each), 20-25 s of the first PIE of an editor session and 800 MB on the GPU, whatever
// TextureArraySize said. Now the atlas builds the source itself.
//
// - MaterialAtlas.TextureArrays.Build: textures of three sizes and two gamma spaces (64 px BGRA8 sRGB, 512 px RGBA16F
//   linear) build into an albedo array at TextureArraySize (100, rounded up to 128) in BGRA8 sRGB; a constant sRGB colour
//   keeps its bytes, linear 0.5 becomes sRGB 188, a slot without a texture takes the first built slice, a texture used
//   twice gives two equal slices; the normal array is linear BC5 in the normal-map group; no roughness texture, no array.
// - MaterialAtlas.TextureArrays.Rebuild: a second build makes new arrays under new names (NewObject with the old name
//   would have reconstructed the array the materials draw with in place).
// ---------------------------------------------------------------------------

namespace MaterialAtlasArrayTestUtils
{
	/** A transient texture whose source is Size x Size of one value, in BGRA8 (Color) or RGBA16F (Linear). */
	UTexture2D* MakeSourceTexture(int32 Size, bool bFloat, bool bSRGB, FColor Color, FLinearColor Linear)
	{
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Texture->SRGB = bSRGB;
		Texture->Source.Init(Size, Size, 1, 1, bFloat ? TSF_RGBA16F : TSF_BGRA8);
		uint8* Data = Texture->Source.LockMip(0);
		for (int32 Pixel = 0; Pixel < Size * Size; ++Pixel)
		{
			if (bFloat)
			{
				reinterpret_cast<FFloat16Color*>(Data)[Pixel] = FFloat16Color(Linear);
			}
			else
			{
				reinterpret_cast<FColor*>(Data)[Pixel] = Color;
			}
		}
		Texture->Source.UnlockMip(0);
		return Texture;
	}

	/** Pixel (X, Y) of a slice of an 8-bit BGRA array source. */
	FColor ArrayPixel(UTexture2DArray* Array, int32 Slice, int32 X, int32 Y)
	{
		const int64 SizeX = Array->Source.GetSizeX();
		const int64 SliceBytes = SizeX * Array->Source.GetSizeY() * sizeof(FColor);
		const uint8* Data = Array->Source.LockMipReadOnly(0);
		const FColor Color = reinterpret_cast<const FColor*>(Data + SliceBytes * Slice)[Y * SizeX + X];
		Array->Source.UnlockMip(0);
		return Color;
	}

	/** Whether two slices of an array source hold the same bytes. */
	bool SlicesEqual(UTexture2DArray* Array, int32 SliceA, int32 SliceB)
	{
		const int64 SliceBytes = int64(Array->Source.GetSizeX()) * Array->Source.GetSizeY() * sizeof(FColor);
		const uint8* Data = Array->Source.LockMipReadOnly(0);
		const bool bEqual = FMemory::Memcmp(Data + SliceBytes * SliceA, Data + SliceBytes * SliceB, SliceBytes) == 0;
		Array->Source.UnlockMip(0);
		return bEqual;
	}

	bool Near(FColor A, FColor B, int32 Tolerance)
	{
		return FMath::Abs(A.R - B.R) <= Tolerance && FMath::Abs(A.G - B.G) <= Tolerance
			&& FMath::Abs(A.B - B.B) <= Tolerance && FMath::Abs(A.A - B.A) <= Tolerance;
	}

	/** An atlas of four materials: 0 and 3 share a 64 px sRGB albedo, 1 has none, 2 a 512 px linear float albedo;
	 *  material 0 has a 256 px normal map; no roughness. */
	UVoxelMaterialAtlas* MakeAtlas()
	{
		UTexture2D* Sand = MakeSourceTexture(64, false, true, FColor(50, 100, 200, 255), FLinearColor::Black);
		UTexture2D* Grey = MakeSourceTexture(512, true, false, FColor::Black, FLinearColor(0.5f, 0.5f, 0.5f, 1.0f));
		UTexture2D* Flat = MakeSourceTexture(256, false, false, FColor(128, 128, 255, 255), FLinearColor::Black);

		UVoxelMaterialAtlas* Atlas = NewObject<UVoxelMaterialAtlas>(GetTransientPackage(), NAME_None, RF_Transient);
		Atlas->TextureArraySize = 100;
		for (uint8 MaterialID = 0; MaterialID < 4; ++MaterialID)
		{
			FVoxelMaterialTextureConfig& Config = Atlas->MaterialConfigs.AddDefaulted_GetRef();
			Config.MaterialID = MaterialID;
			Config.MaterialName = FString::Printf(TEXT("Test%d"), MaterialID);
		}
		Atlas->MaterialConfigs[0].AlbedoTexture = Sand;
		Atlas->MaterialConfigs[0].NormalTexture = Flat;
		Atlas->MaterialConfigs[2].AlbedoTexture = Grey;
		Atlas->MaterialConfigs[3].AlbedoTexture = Sand;
		return Atlas;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAtlasTextureArrayBuildTest, "VoxelWorlds.MaterialAtlas.TextureArrays.Build",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAtlasTextureArrayBuildTest::RunTest(const FString& Parameters)
{
	using namespace MaterialAtlasArrayTestUtils;

	AddExpectedMessagePlain(TEXT("BuildSingleTextureArray(Roughness): No textures loaded"), ELogVerbosity::Warning);
	UVoxelMaterialAtlas* Atlas = MakeAtlas();
	Atlas->BuildTextureArrays();

	UTexture2DArray* Albedo = Atlas->AlbedoArray;
	if (!TestNotNull(TEXT("Albedo array built"), Albedo))
	{
		return false;
	}
	TestEqual(TEXT("Albedo slice size = TextureArraySize rounded up to a power of two"), static_cast<int32>(Albedo->Source.GetSizeX()), 128);
	TestEqual(TEXT("Albedo slice height"), static_cast<int32>(Albedo->Source.GetSizeY()), 128);
	TestEqual(TEXT("Albedo slices = max MaterialID + 1"), Albedo->Source.GetNumSlices(), 4);
	TestEqual(TEXT("Albedo source is 8-bit BGRA"), static_cast<int32>(Albedo->Source.GetFormat()), static_cast<int32>(TSF_BGRA8));
	TestEqual(TEXT("Albedo source holds one mip"), Albedo->Source.GetNumMips(), 1);
	TestTrue(TEXT("Albedo array is sRGB"), Albedo->SRGB != 0);
	TestEqual(TEXT("Albedo compresses as colour (BC1/BC3)"), static_cast<int32>(Albedo->CompressionSettings.GetValue()), static_cast<int32>(TC_Default));
	TestEqual(TEXT("Albedo mips from the texture group"), static_cast<int32>(Albedo->MipGenSettings.GetValue()), static_cast<int32>(TMGS_FromTextureGroup));

	const FColor SandPixel = ArrayPixel(Albedo, 0, 17, 93);
	TestTrue(*FString::Printf(TEXT("An sRGB colour keeps its bytes (got %s)"), *SandPixel.ToString()), Near(SandPixel, FColor(50, 100, 200, 255), 1));
	const FColor GreyPixel = ArrayPixel(Albedo, 2, 64, 3);
	TestTrue(*FString::Printf(TEXT("Linear 0.5 becomes sRGB 188 (got %s)"), *GreyPixel.ToString()), Near(GreyPixel, FColor(188, 188, 188, 255), 1));
	TestTrue(TEXT("A slot without a texture takes the first built slice"), SlicesEqual(Albedo, 1, 0));
	TestTrue(TEXT("A texture used twice gives equal slices"), SlicesEqual(Albedo, 3, 0));

	UTexture2DArray* Normal = Atlas->NormalArray;
	if (TestNotNull(TEXT("Normal array built"), Normal))
	{
		TestEqual(TEXT("Normal slice size"), static_cast<int32>(Normal->Source.GetSizeX()), 128);
		TestEqual(TEXT("Normal slices"), Normal->Source.GetNumSlices(), 4);
		TestFalse(TEXT("Normal array is linear"), Normal->SRGB != 0);
		TestEqual(TEXT("Normal compresses as a normal map (BC5)"), static_cast<int32>(Normal->CompressionSettings.GetValue()), static_cast<int32>(TC_Normalmap));
		TestEqual(TEXT("Normal array in the normal-map texture group"), static_cast<int32>(Normal->LODGroup.GetValue()), static_cast<int32>(TEXTUREGROUP_WorldNormalMap));
		const FColor FlatPixel = ArrayPixel(Normal, 0, 5, 120);
		TestTrue(*FString::Printf(TEXT("A linear normal keeps its bytes (got %s)"), *FlatPixel.ToString()), Near(FlatPixel, FColor(128, 128, 255, 255), 1));
		TestTrue(TEXT("Normal slots without a texture take the first built slice"), SlicesEqual(Normal, 2, 0));
	}

	TestNull(TEXT("No roughness texture, no roughness array"), Atlas->RoughnessArray.Get());
	TestFalse(TEXT("Arrays no longer dirty"), Atlas->AreTextureArraysDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAtlasTextureArrayRebuildTest, "VoxelWorlds.MaterialAtlas.TextureArrays.Rebuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAtlasTextureArrayRebuildTest::RunTest(const FString& Parameters)
{
	using namespace MaterialAtlasArrayTestUtils;

	AddExpectedMessagePlain(TEXT("BuildSingleTextureArray(Roughness): No textures loaded"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 2);
	UVoxelMaterialAtlas* Atlas = MakeAtlas();
	Atlas->BuildTextureArrays();
	UTexture2DArray* First = Atlas->AlbedoArray;
	const FName FirstName = First ? First->GetFName() : NAME_None;

	Atlas->TextureArraySize = 256;
	Atlas->BuildTextureArrays();
	UTexture2DArray* Second = Atlas->AlbedoArray;

	if (!TestNotNull(TEXT("Both builds made an albedo array"), First) || !TestNotNull(TEXT("Second albedo array"), Second))
	{
		return false;
	}
	TestTrue(TEXT("The rebuild made a new array"), First != Second);
	TestNotEqual(TEXT("Under a new name"), Second->GetFName(), FirstName);
	TestEqual(TEXT("The first array is untouched"), static_cast<int32>(First->Source.GetSizeX()), 128);
	TestEqual(TEXT("The rebuild takes the new TextureArraySize"), static_cast<int32>(Second->Source.GetSizeX()), 256);
	return true;
}

#endif
