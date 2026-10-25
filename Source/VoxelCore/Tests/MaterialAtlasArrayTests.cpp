// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelMaterialAtlas.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Misc/PackageName.h"
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
//   keeps its bytes, linear 0.5 becomes sRGB 188; one slice per distinct texture (F2.12): a texture used twice is one
//   slice both materials read, a material without a texture reads the first; the normal array is linear BC5 in the
//   normal-map group; no roughness texture, no array.
// - MaterialAtlas.TextureArrays.SliceTables (F2.12): each MaterialID's slice per array, as a slice per MaterialID read
//   them (a gap below the highest configured MaterialID the first slice, above it the highest's; a texture with no source
//   data the first slice; no readable texture at all, one placeholder slice); the material LUT's row 3 carries them
//   (R albedo, G normal, B roughness) with the face rows unchanged, an atlas whose arrays predate the tables maps every
//   MaterialID to its own slice, and a rebuild of the arrays rewrites a LUT that exists in place.
// - MaterialAtlas.TextureArrays.Rebuild: a second build of a transient atlas makes new arrays under new names
//   (NewObject with the old name would have reconstructed the array the materials draw with in place).
// - MaterialAtlas.TextureArrays.SavedAssets (F2.11): a packaged game has no source textures, so an atlas asset's arrays
//   are assets of their own next to it (<Atlas>_AlbedoArray, ...): built into them, rebuilt in place, the atlas
//   marked stale (AreTextureArraysDirty) when TextureArraySize or a source texture changes and EnsureTextureArrays()
//   rebuilding only then; an array no config has a texture for anymore is dropped; an atlas with no source texture and
//   no array (packed atlas only) is never stale.
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
	 *  material 0 has a 256 px normal map; no roughness. Transient, or an asset in the package Outer. */
	UVoxelMaterialAtlas* MakeAtlas(UPackage* Outer = nullptr)
	{
		UTexture2D* Sand = MakeSourceTexture(64, false, true, FColor(50, 100, 200, 255), FLinearColor::Black);
		UTexture2D* Grey = MakeSourceTexture(512, true, false, FColor::Black, FLinearColor(0.5f, 0.5f, 0.5f, 1.0f));
		UTexture2D* Flat = MakeSourceTexture(256, false, false, FColor(128, 128, 255, 255), FLinearColor::Black);

		UVoxelMaterialAtlas* Atlas = Outer
			? NewObject<UVoxelMaterialAtlas>(Outer, FName(*FPackageName::GetShortName(Outer)), RF_Public | RF_Standalone)
			: NewObject<UVoxelMaterialAtlas>(GetTransientPackage(), NAME_None, RF_Transient);
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
	TestEqual(TEXT("Albedo slices = distinct textures (sand, grey)"), Albedo->Source.GetNumSlices(), 2);
	TestEqual(TEXT("Albedo source is 8-bit BGRA"), static_cast<int32>(Albedo->Source.GetFormat()), static_cast<int32>(TSF_BGRA8));
	TestEqual(TEXT("Albedo source holds one mip"), Albedo->Source.GetNumMips(), 1);
	TestTrue(TEXT("Albedo array is sRGB"), Albedo->SRGB != 0);
	TestEqual(TEXT("Albedo compresses as colour (BC1/BC3)"), static_cast<int32>(Albedo->CompressionSettings.GetValue()), static_cast<int32>(TC_Default));
	TestEqual(TEXT("Albedo mips from the texture group"), static_cast<int32>(Albedo->MipGenSettings.GetValue()), static_cast<int32>(TMGS_FromTextureGroup));

	const FColor SandPixel = ArrayPixel(Albedo, 0, 17, 93);
	TestTrue(*FString::Printf(TEXT("An sRGB colour keeps its bytes (got %s)"), *SandPixel.ToString()), Near(SandPixel, FColor(50, 100, 200, 255), 1));
	const FColor GreyPixel = ArrayPixel(Albedo, 1, 64, 3);
	TestTrue(*FString::Printf(TEXT("Linear 0.5 becomes sRGB 188 (got %s)"), *GreyPixel.ToString()), Near(GreyPixel, FColor(188, 188, 188, 255), 1));
	TestFalse(TEXT("The two textures' slices differ"), SlicesEqual(Albedo, 0, 1));
	if (TestEqual(TEXT("A slice table entry per MaterialID"), Atlas->AlbedoArraySlices.Num(), 256))
	{
		TestEqual(TEXT("Material 0 (sand) reads slice 0"), static_cast<int32>(Atlas->AlbedoArraySlices[0]), 0);
		TestEqual(TEXT("A material without a texture reads the first slice"), static_cast<int32>(Atlas->AlbedoArraySlices[1]), 0);
		TestEqual(TEXT("Material 2 (grey) reads slice 1"), static_cast<int32>(Atlas->AlbedoArraySlices[2]), 1);
		TestEqual(TEXT("A texture used twice is one slice both read"), static_cast<int32>(Atlas->AlbedoArraySlices[3]), 0);
		TestEqual(TEXT("GetArrayIndex gives the albedo slice"), Atlas->GetArrayIndex(2), 1);
	}

	UTexture2DArray* Normal = Atlas->NormalArray;
	if (TestNotNull(TEXT("Normal array built"), Normal))
	{
		TestEqual(TEXT("Normal slice size"), static_cast<int32>(Normal->Source.GetSizeX()), 128);
		TestEqual(TEXT("Normal slices: one texture, one slice"), Normal->Source.GetNumSlices(), 1);
		TestFalse(TEXT("Normal array is linear"), Normal->SRGB != 0);
		TestEqual(TEXT("Normal compresses as a normal map (BC5)"), static_cast<int32>(Normal->CompressionSettings.GetValue()), static_cast<int32>(TC_Normalmap));
		TestEqual(TEXT("Normal array in the normal-map texture group"), static_cast<int32>(Normal->LODGroup.GetValue()), static_cast<int32>(TEXTUREGROUP_WorldNormalMap));
		const FColor FlatPixel = ArrayPixel(Normal, 0, 5, 120);
		TestTrue(*FString::Printf(TEXT("A linear normal keeps its bytes (got %s)"), *FlatPixel.ToString()), Near(FlatPixel, FColor(128, 128, 255, 255), 1));
		TestEqual(TEXT("A material with an albedo but no normal map reads the first normal slice"),
			Atlas->NormalArraySlices.IsValidIndex(2) ? static_cast<int32>(Atlas->NormalArraySlices[2]) : -1, 0);
	}

	TestNull(TEXT("No roughness texture, no roughness array"), Atlas->RoughnessArray.Get());
	TestEqual(TEXT("... and no roughness slice table"), Atlas->RoughnessArraySlices.Num(), 0);
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

namespace MaterialAtlasArrayTestUtils
{
	/** Texel (X, Row) of the atlas's material LUT (BGRA8), read from its CPU copy. */
	FColor LUTTexel(UVoxelMaterialAtlas* Atlas, int32 X, int32 Row)
	{
		UTexture2D* LUT = Atlas->GetMaterialLUT();
		FTexture2DMipMap& Mip = LUT->GetPlatformData()->Mips[0];
		const FColor* Texels = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
		const FColor Texel = Texels ? Texels[Row * LUT->GetSizeX() + X] : FColor(0, 0, 0, 0);
		Mip.BulkData.Unlock();
		return Texel;
	}

	/** Whether the LUT's slice row (row 3) holds the atlas's slice tables for every MaterialID. */
	bool SliceRowMatches(UVoxelMaterialAtlas* Atlas, FString& OutMismatch)
	{
		for (int32 MaterialID = 0; MaterialID < 256; ++MaterialID)
		{
			const FColor Texel = LUTTexel(Atlas, MaterialID, 3);
			const uint8 ID = static_cast<uint8>(MaterialID);
			const FColor Expected(UVoxelMaterialAtlas::GetArraySlice(Atlas->AlbedoArraySlices, ID),
				UVoxelMaterialAtlas::GetArraySlice(Atlas->NormalArraySlices, ID),
				UVoxelMaterialAtlas::GetArraySlice(Atlas->RoughnessArraySlices, ID), 0);
			if (Texel != Expected)
			{
				OutMismatch = FString::Printf(TEXT("MaterialID %d: %s, expected %s"), MaterialID, *Texel.ToString(), *Expected.ToString());
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAtlasTextureArraySliceTablesTest, "VoxelWorlds.MaterialAtlas.TextureArrays.SliceTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAtlasTextureArraySliceTablesTest::RunTest(const FString& Parameters)
{
	using namespace MaterialAtlasArrayTestUtils;

	AddExpectedMessagePlain(TEXT("has no readable source data"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("has no readable source texture"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("no texture had readable source data"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("No textures loaded"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	// MakeAtlas's four materials, then gaps at 4 and 5; 6 with an albedo that has no source data, its own normal map and a
	// roughness texture; 7 (the highest) grey again. Roughness: 3 grey (the first in MaterialID order), 6 sand.
	UVoxelMaterialAtlas* Atlas = MakeAtlas();
	UTexture2D* Sand = Atlas->MaterialConfigs[0].AlbedoTexture.Get();
	UTexture2D* Grey = Atlas->MaterialConfigs[2].AlbedoTexture.Get();
	UTexture2D* NoSource = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	for (int32 MaterialID : { 6, 7 })
	{
		FVoxelMaterialTextureConfig& Config = Atlas->MaterialConfigs.AddDefaulted_GetRef();
		Config.MaterialID = static_cast<uint8>(MaterialID);
		Config.MaterialName = FString::Printf(TEXT("Test%d"), MaterialID);
	}
	Atlas->MaterialConfigs[4].AlbedoTexture = NoSource;
	Atlas->MaterialConfigs[4].NormalTexture = Grey;
	Atlas->MaterialConfigs[4].RoughnessTexture = Sand;
	Atlas->MaterialConfigs[5].AlbedoTexture = Grey;
	Atlas->MaterialConfigs[3].RoughnessTexture = Grey;
	// Material 0 has face variants, a UV scale and is masked: the LUT's face rows keep them
	FVoxelMaterialTextureConfig& First = Atlas->MaterialConfigs[0];
	First.bUseFaceVariants = true;
	First.TopTile = FVoxelAtlasTile(1, 2);
	First.SideTile = FVoxelAtlasTile(3, 4);
	First.BottomTile = FVoxelAtlasTile(5, 6);
	First.UVScale = 2.0f;
	First.bIsMasked = true;

	// Before any build the tables are empty: every MaterialID reads the slice of its own index (arrays built with a
	// slice per MaterialID)
	Atlas->BuildMaterialLUT();
	UTexture2D* LUT = Atlas->GetMaterialLUT();
	if (!TestNotNull(TEXT("LUT built"), LUT))
	{
		return false;
	}
	TestEqual(TEXT("LUT width"), LUT->GetSizeX(), 256);
	TestEqual(TEXT("LUT height: three face rows and the slice row"), LUT->GetSizeY(), 4);
	TestEqual(TEXT("No tables: MaterialID 5 reads slice 5 in every array"), LUTTexel(Atlas, 5, 3), FColor(5, 5, 5, 0));
	TestEqual(TEXT("No tables: MaterialID 255 reads slice 255"), LUTTexel(Atlas, 255, 3), FColor(255, 255, 255, 0));

	Atlas->BuildTextureArrays();
	UTexture2DArray* Albedo = Atlas->AlbedoArray;
	UTexture2DArray* Normal = Atlas->NormalArray;
	UTexture2DArray* Roughness = Atlas->RoughnessArray;
	if (!TestNotNull(TEXT("Albedo array"), Albedo) || !TestNotNull(TEXT("Normal array"), Normal) || !TestNotNull(TEXT("Roughness array"), Roughness))
	{
		return false;
	}
	TestEqual(TEXT("Albedo: two readable textures, two slices"), Albedo->Source.GetNumSlices(), 2);
	TestEqual(TEXT("Normal: two textures"), Normal->Source.GetNumSlices(), 2);
	TestEqual(TEXT("Roughness: two textures"), Roughness->Source.GetNumSlices(), 2);
	TestTrue(TEXT("Roughness slice 0 is the first in MaterialID order (grey, material 3)"), Near(ArrayPixel(Roughness, 0, 7, 7), FColor(128, 128, 128, 255), 1));

	auto Slices = [](const TArray<uint8>& Table)
	{
		FString Text;
		for (int32 MaterialID : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 255 })
		{
			Text += FString::Printf(TEXT("%d "), Table.IsValidIndex(MaterialID) ? Table[MaterialID] : -1);
		}
		return Text.TrimEnd();
	};
	// MaterialIDs 0-7, 8 and 255: a gap and a texture with no source data take the first slice, above the highest
	// configured the highest's (grey)
	TestEqual(TEXT("Albedo slices"), Slices(Atlas->AlbedoArraySlices), FString(TEXT("0 0 1 0 0 0 0 1 1 1")));
	TestEqual(TEXT("Normal slices"), Slices(Atlas->NormalArraySlices), FString(TEXT("0 0 0 0 0 0 1 0 0 0")));
	TestEqual(TEXT("Roughness slices"), Slices(Atlas->RoughnessArraySlices), FString(TEXT("0 0 0 0 0 0 1 0 0 0")));

	// The build rewrote the LUT in place: the materials bound to it read the new slices
	TestTrue(TEXT("The same LUT object"), Atlas->GetMaterialLUT() == LUT);
	TestFalse(TEXT("The LUT is up to date"), Atlas->IsLUTDirty());
	FString Mismatch;
	const bool bRowMatches = SliceRowMatches(Atlas, Mismatch);
	TestTrue(FString::Printf(TEXT("LUT row 3 = the slice tables %s"), *Mismatch), bRowMatches);
	TestEqual(TEXT("Material 6: albedo 0, normal 1, roughness 1"), LUTTexel(Atlas, 6, 3), FColor(0, 1, 1, 0));
	// Face rows: R column, G row, B UV scale x 25.5, A flags (bit 0 masked)
	TestEqual(TEXT("Top row"), LUTTexel(Atlas, 0, 0), FColor(1, 2, 51, 1));
	TestEqual(TEXT("Side row"), LUTTexel(Atlas, 0, 1), FColor(3, 4, 51, 1));
	TestEqual(TEXT("Bottom row"), LUTTexel(Atlas, 0, 2), FColor(5, 6, 51, 1));

	// A new texture moves the slices after it; the rebuild rewrites the same LUT
	UTexture2D* Red = MakeSourceTexture(32, false, true, FColor(200, 20, 20, 255), FLinearColor::Black);
	Atlas->MaterialConfigs[1].AlbedoTexture = Red;
	Atlas->BuildTextureArrays();
	TestEqual(TEXT("Albedo slices after a new texture at material 1"), Slices(Atlas->AlbedoArraySlices), FString(TEXT("0 1 2 0 0 0 0 2 2 2")));
	TestTrue(TEXT("Still the same LUT object"), Atlas->GetMaterialLUT() == LUT);
	const bool bRowFollows = SliceRowMatches(Atlas, Mismatch);
	TestTrue(FString::Printf(TEXT("LUT row 3 follows %s"), *Mismatch), bRowFollows);
	TestTrue(TEXT("The new texture's slice holds it"), Near(ArrayPixel(Atlas->AlbedoArray, 1, 3, 3), FColor(200, 20, 20, 255), 1));

	// No readable texture at all: one slice of the placeholder colour, every material on it
	UVoxelMaterialAtlas* Unreadable = NewObject<UVoxelMaterialAtlas>(GetTransientPackage(), NAME_None, RF_Transient);
	Unreadable->TextureArraySize = 64;
	FVoxelMaterialTextureConfig& Only = Unreadable->MaterialConfigs.AddDefaulted_GetRef();
	Only.MaterialID = 3;
	Only.AlbedoTexture = NoSource;
	Unreadable->BuildTextureArrays();
	if (TestNotNull(TEXT("A placeholder albedo array"), Unreadable->AlbedoArray.Get()))
	{
		TestEqual(TEXT("One placeholder slice"), Unreadable->AlbedoArray->Source.GetNumSlices(), 1);
		TestTrue(TEXT("Of the placeholder grey"), Near(ArrayPixel(Unreadable->AlbedoArray, 0, 1, 1), FColor(128, 128, 128, 255), 0));
		TestEqual(TEXT("Every material on it"), Slices(Unreadable->AlbedoArraySlices), FString(TEXT("0 0 0 0 0 0 0 0 0 0")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAtlasTextureArraySavedAssetsTest, "VoxelWorlds.MaterialAtlas.TextureArrays.SavedAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialAtlasTextureArraySavedAssetsTest::RunTest(const FString& Parameters)
{
	using namespace MaterialAtlasArrayTestUtils;

	// An atlas asset in memory (never saved): /Temp/VoxelAtlasArrayTest_<guid>/TestAtlas
	const FString Folder = FString::Printf(TEXT("/Temp/VoxelAtlasArrayTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* AtlasPackage = CreatePackage(*(Folder / TEXT("TestAtlas")));
	UVoxelMaterialAtlas* Atlas = MakeAtlas(AtlasPackage);

	AddExpectedMessagePlain(TEXT("BuildSingleTextureArray(Roughness): No textures loaded"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 0);
	TestTrue(TEXT("Never built: stale"), Atlas->AreTextureArraysDirty());
	Atlas->BuildTextureArrays();

	UTexture2DArray* Albedo = Atlas->AlbedoArray;
	UTexture2DArray* Normal = Atlas->NormalArray;
	if (!TestNotNull(TEXT("Albedo array built"), Albedo) || !TestNotNull(TEXT("Normal array built"), Normal))
	{
		return false;
	}
	TestEqual(TEXT("The albedo array is an asset next to the atlas"), Albedo->GetPathName(), Folder / TEXT("TestAtlas_AlbedoArray.TestAtlas_AlbedoArray"));
	TestEqual(TEXT("The normal array is an asset next to the atlas"), Normal->GetPathName(), Folder / TEXT("TestAtlas_NormalArray.TestAtlas_NormalArray"));
	TestTrue(TEXT("The albedo array is an asset (public, standalone, in its own package)"), Albedo->IsAsset() && Albedo->HasAnyFlags(RF_Standalone));
	TestTrue(TEXT("The array's package is left to save"), Albedo->GetPackage()->IsDirty());
	TestTrue(TEXT("So is the atlas's"), AtlasPackage->IsDirty());
	TestNull(TEXT("No roughness texture, no roughness array"), Atlas->RoughnessArray.Get());
	TestEqual(TEXT("Slices at TextureArraySize"), static_cast<int32>(Albedo->Source.GetSizeX()), 128);
	TestFalse(TEXT("A build key is stored"), Atlas->TextureArrayBuildKey.IsEmpty());
	TestFalse(TEXT("Up to date after the build"), Atlas->AreTextureArraysDirty());
	// The slice tables go with the arrays: a packaged game builds its material LUT from the saved ones
	TestEqual(TEXT("Slice tables built with the arrays"), Atlas->AlbedoArraySlices.Num(), 256);
	const FProperty* SlicesProperty = UVoxelMaterialAtlas::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UVoxelMaterialAtlas, AlbedoArraySlices));
	TestTrue(TEXT("... and saved with the atlas in every build"), SlicesProperty && !SlicesProperty->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly));

	// Up to date: binding the atlas rebuilds nothing (a rebuild would dirty the packages again)
	Albedo->GetPackage()->SetDirtyFlag(false);
	AtlasPackage->SetDirtyFlag(false);
	TestTrue(TEXT("EnsureTextureArrays binds the saved arrays"), Atlas->EnsureTextureArrays());
	TestFalse(TEXT("... without rebuilding them"), Albedo->GetPackage()->IsDirty() || AtlasPackage->IsDirty());

	// A changed input makes them stale, and binding rebuilds them in place
	Atlas->TextureArraySize = 256;
	TestTrue(TEXT("A new TextureArraySize: stale"), Atlas->AreTextureArraysDirty());
	AddExpectedMessagePlain(TEXT("texture arrays built from other configs or source textures; rebuilding them"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 2);
	TestTrue(TEXT("EnsureTextureArrays rebuilds"), Atlas->EnsureTextureArrays());
	TestTrue(TEXT("The same albedo asset, rebuilt in place"), Atlas->AlbedoArray == Albedo);
	TestTrue(TEXT("The same normal asset"), Atlas->NormalArray == Normal);
	TestEqual(TEXT("At the new size"), static_cast<int32>(Albedo->Source.GetSizeX()), 256);
	TestTrue(TEXT("Left to save again"), Albedo->GetPackage()->IsDirty() && AtlasPackage->IsDirty());
	TestFalse(TEXT("Up to date again"), Atlas->AreTextureArraysDirty());

	// A source texture's new content makes them stale too
	UTexture2D* Sand = Atlas->MaterialConfigs[0].AlbedoTexture.Get();
	if (TestNotNull(TEXT("Sand texture"), Sand))
	{
		uint8* Data = Sand->Source.LockMip(0);
		reinterpret_cast<FColor*>(Data)[0] = FColor(1, 2, 3, 255);
		Sand->Source.UnlockMip(0);
		TestTrue(TEXT("A source texture changed: stale"), Atlas->AreTextureArraysDirty());
		Atlas->EnsureTextureArrays();
		TestFalse(TEXT("Rebuilt"), Atlas->AreTextureArraysDirty());
	}

	// No config has a normal texture anymore: the atlas drops its normal array (the asset stays, unused)
	Atlas->MaterialConfigs[0].NormalTexture.Reset();
	AddExpectedMessagePlain(TEXT("BuildSingleTextureArray(Normal): No textures loaded"), ELogVerbosity::Warning);
	AddExpectedMessagePlain(TEXT("is no longer used by TestAtlas"), ELogVerbosity::Warning);
	Atlas->BuildTextureArrays();
	TestNull(TEXT("The normal array is dropped"), Atlas->NormalArray.Get());
	TestTrue(TEXT("The albedo array stays"), Atlas->AlbedoArray == Albedo);
	TestFalse(TEXT("Up to date without a normal array"), Atlas->AreTextureArraysDirty());

	// An atlas with no source texture and no array (packed atlas only) has nothing to build, built or not
	UVoxelMaterialAtlas* PackedOnly = NewObject<UVoxelMaterialAtlas>(CreatePackage(*(Folder / TEXT("PackedOnly"))), TEXT("PackedOnly"), RF_Public | RF_Standalone);
	PackedOnly->MaterialConfigs.AddDefaulted();
	TestFalse(TEXT("A packed-only atlas is never stale"), PackedOnly->AreTextureArraysDirty());

	// Leave nothing behind: nothing to save, and the objects free at the next garbage collection
	for (UObject* Object : TArray<UObject*>{ Atlas, Albedo, Normal, PackedOnly })
	{
		Object->GetPackage()->SetDirtyFlag(false);
		Object->ClearFlags(RF_Public | RF_Standalone);
		Object->MarkAsGarbage();
	}
	return true;
}

#endif
