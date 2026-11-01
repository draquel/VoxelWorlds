// Copyright Daniel Raquel. All Rights Reserved.

// UVoxelMaterialAtlas's editor-only texture-array builder (F2.10-F2.12): one array per map, a slice per distinct texture,
// saved next to the atlas and rebuilt when its inputs change. A translation unit of the class.

#include "VoxelMaterialAtlas.h"
#include "VoxelMaterialRegistry.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "ImageCore.h"
#include "TextureResource.h"
#include "VoxelMaterialAtlasPrivate.h"

#if WITH_EDITOR
#include "AssetRegistry/IAssetRegistry.h"
#include "Hash/Blake3.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#endif

#if WITH_EDITOR
bool UVoxelMaterialAtlas::BuildSingleTextureArray(
	TObjectPtr<UTexture2DArray>& OutArray,
	TArray<uint8>& OutSlices,
	TFunctionRef<TSoftObjectPtr<UTexture2D>(const FVoxelMaterialTextureConfig&)> TextureGetter,
	FColor PlaceholderColor,
	const FString& ArrayName)
{
	OutSlices.Reset();
	if (MaterialConfigs.Num() == 0)
	{
		UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): No material configs defined"), *ArrayName);
		return false;
	}

	// One slot per MaterialID up to the highest configured, each with its config's texture or none
	uint8 MaxMaterialID = 0;
	for (const FVoxelMaterialTextureConfig& Config : MaterialConfigs)
	{
		MaxMaterialID = FMath::Max(MaxMaterialID, Config.MaterialID);
	}
	const int32 NumSlots = MaxMaterialID + 1;

	// Collect source textures (load soft pointers); a slot without one stays null
	TArray<UTexture2D*> SourceTextures;
	SourceTextures.SetNumZeroed(NumSlots);

	int32 LoadedCount = 0;
	for (const FVoxelMaterialTextureConfig& Config : MaterialConfigs)
	{
		TSoftObjectPtr<UTexture2D> SoftTexture = TextureGetter(Config);
		if (!SoftTexture.IsNull())
		{
			UTexture2D* LoadedTexture = SoftTexture.LoadSynchronous();
			if (LoadedTexture)
			{
				SourceTextures[Config.MaterialID] = LoadedTexture;
				LoadedCount++;
			}
			else
			{
				UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): Failed to load texture for MaterialID %d (%s)"),
					*ArrayName, Config.MaterialID, *Config.MaterialName);
			}
		}
	}

	if (LoadedCount == 0)
	{
		UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): No textures loaded, skipping array creation"), *ArrayName);
		if (OutArray && BuildsTextureArrayAssets())
		{
			UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): %s is no longer used by %s (not deleted)"),
				*ArrayName, *OutArray->GetPathName(), *GetName());
		}
		OutArray = nullptr;
		return false;
	}

	UTexture2DArray* Array = AcquireTextureArrayObject(ArrayName, OutArray);
	if (!Array)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error, TEXT("BuildSingleTextureArray(%s): Failed to create UTexture2DArray"), *ArrayName);
		return false;
	}
	// An array asset is updated in place the way a reimport does it: a build in flight finishes first, and the
	// materials drawing with it pick up the new data when it is ready
	const bool bAsset = BuildsTextureArrayAssets();
	if (bAsset)
	{
		Array->PreEditChange(nullptr);
	}

	// Configure array settings.
	// TF_Default (NOT TF_Bilinear) so the array inherits the World texture group's
	// anisotropic + trilinear filtering (respects r.MaxAnisotropy). TF_Bilinear disables
	// anisotropy and uses point mip selection, which produces crawling minification
	// shimmer on triplanar terrain viewed at grazing angles.
	Array->Filter = TF_Default;
	Array->LODGroup = TEXTUREGROUP_World;

	// The array's source is built here rather than by UTexture2DArray::UpdateSourceFromSourceTextures(), which sizes
	// the array to its largest slice (4096 on the demo atlas, whatever TextureArraySize says) and converts every slice
	// to RGBA32F when their gamma spaces or formats differ: on the demo atlas 5.9 GB of source each for the albedo and
	// normal arrays, held for the editor session, 24 s of its first PIE and a texture build estimated at tens of GB,
	// then 800 MB of GPU memory. Every slice is stored at
	// TextureArraySize in BGRA8, converted from its own texture's gamma into the array's (sRGB for albedo, linear for
	// normals and roughness), so the shader samples the same values.
	//
	// One slice per distinct texture, in MaterialID order, and each material's slice in OutSlices (the material LUT's
	// slice row): with a slice per MaterialID, the 14 of the demo atlas's 22 materials without an albedo held copies of
	// the first slice (19 of 22 without a roughness map), stored on disk (they do not compress away) and on the GPU.
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 SliceSize = static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::Clamp(TextureArraySize, 64, 4096))));
	const bool bNormalArray = (ArrayName == TEXT("Normal"));
	const bool bSRGB = (ArrayName == TEXT("Albedo"));
	const EGammaSpace ArrayGamma = bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
	const int64 SliceBytes = int64(SliceSize) * SliceSize * sizeof(FColor);

	TArray64<uint8> ArrayData;
	int32 NumSlices = 0;

	// Terrain textures tile: the resize filters across the wrapped edges
	const FImageCore::EResizeImageFilter ResizeFilter = FImageCore::EResizeImageFilter::Default
		| FImageCore::EResizeImageFilter::Flag_WrapX | FImageCore::EResizeImageFilter::Flag_WrapY;

	TMap<UTexture2D*, int32> BuiltSlice; // texture -> its slice (INDEX_NONE: unreadable)
	TArray<int32> SlotSlices;            // each slot's slice (INDEX_NONE: no readable texture)
	SlotSlices.Init(INDEX_NONE, NumSlots);
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		UTexture2D* Texture = SourceTextures[Slot];
		if (!Texture)
		{
			continue;
		}
		if (const int32* Built = BuiltSlice.Find(Texture))
		{
			SlotSlices[Slot] = *Built;
			continue;
		}

		FImage SourceImage;
		if (!Texture->Source.IsValid() || !Texture->Source.GetMipImage(SourceImage, 0))
		{
			UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): %s has no readable source data; its materials take the first slice"),
				*ArrayName, *Texture->GetName());
			BuiltSlice.Add(Texture, INDEX_NONE);
			continue;
		}

		const int32 SliceIndex = NumSlices++;
		ArrayData.AddUninitialized(SliceBytes);
		FImageCore::ResizeImage(SourceImage, FImageView(ArrayData.GetData() + SliceBytes * SliceIndex, SliceSize, SliceSize, 1, ERawImageFormat::BGRA8, ArrayGamma), ResizeFilter);
		BuiltSlice.Add(Texture, SliceIndex);
		SlotSlices[Slot] = SliceIndex;

		UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("  Slice[%d] (from MaterialID %d): %s source %dx%d %s %s -> %dx%d"),
			SliceIndex, Slot, *Texture->GetName(), SourceImage.SizeX, SourceImage.SizeY,
			ERawImageFormat::GetName(SourceImage.Format), SourceImage.GammaSpace == EGammaSpace::Linear ? TEXT("linear") : TEXT("sRGB"),
			SliceSize, SliceSize);
	}

	// With no readable texture at all, the array's one slice is PlaceholderColor
	if (NumSlices == 0)
	{
		UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): no texture had readable source data; its one slice is the placeholder colour"), *ArrayName);
		ArrayData.AddUninitialized(SliceBytes);
		FColor* Pixels = reinterpret_cast<FColor*>(ArrayData.GetData());
		for (int64 PixelIndex = 0; PixelIndex < int64(SliceSize) * SliceSize; ++PixelIndex)
		{
			Pixels[PixelIndex] = PlaceholderColor;
		}
		NumSlices = 1;
	}

	// Each MaterialID's slice. Slots without a readable texture take the first slice, which a slice per slot copied into
	// them; a MaterialID above the highest configured the highest's, where the sampler clamped it then
	int32 SlotsOnFirstSlice = 0;
	for (int32& Slice : SlotSlices)
	{
		if (Slice == INDEX_NONE)
		{
			Slice = 0;
			++SlotsOnFirstSlice;
		}
	}
	OutSlices.SetNumUninitialized(256);
	for (int32 MaterialID = 0; MaterialID < 256; ++MaterialID)
	{
		OutSlices[MaterialID] = static_cast<uint8>(SlotSlices[FMath::Min(MaterialID, NumSlots - 1)]);
	}

	// Settings pinned per array, not taken from whichever texture lands in slice 0: BC1/BC3 colour, BC5 normals
	// (in the normal-map texture group), BC1/BC3 linear roughness
	Array->Source.Init(SliceSize, SliceSize, NumSlices, 1, TSF_BGRA8, ArrayData.GetData());
	Array->SRGB = bSRGB;
	Array->CompressionSettings = bNormalArray ? TC_Normalmap : (bSRGB ? TC_Default : TC_Masks);
	Array->LODGroup = bNormalArray ? TEXTUREGROUP_WorldNormalMap : TEXTUREGROUP_World;
	// A full mip chain (TMGS_FromTextureGroup = the World group's): single-mip slices minify into crawling shimmer
	Array->MipGenSettings = TMGS_FromTextureGroup;
	Array->PowerOfTwoMode = ETexturePowerOfTwoSetting::None;
	Array->NeverStream = true;
	Array->ValidateSettingsAfterImportOrEdit();
	Array->SetLightingGuid();

	// One build of the platform data (compressed, mipped), asynchronous in the editor and cached in the DDC by the
	// source's hash: the same atlas builds once and is fetched in later sessions. For an asset PostEditChange does it
	// (and tells the materials using the array); a new transient array has no users yet.
	if (bAsset)
	{
		Array->PostEditChange();
		// Set directly rather than MarkPackageDirty(), which does nothing while a PIE world runs (when the editor
		// rebuilds stale arrays): the rebuilt arrays are asset changes to save
		Array->GetPackage()->SetDirtyFlag(true);
	}
	else
	{
		Array->UpdateResource();
	}
	OutArray = Array;

	UE_LOG(LogVoxelMaterialAtlas, Log,
		TEXT("BuildSingleTextureArray(%s): Built %d slices at %dx%d BGRA8 %s for %d MaterialIDs (%d distinct textures, %d from configs, %d without a readable texture on the first slice) in %.2f s, compression %d, MipGen %d -> %s"),
		*ArrayName, NumSlices, SliceSize, SliceSize, bSRGB ? TEXT("sRGB") : TEXT("linear"), NumSlots, BuiltSlice.Num(), LoadedCount, SlotsOnFirstSlice,
		FPlatformTime::Seconds() - StartSeconds, static_cast<int32>(Array->CompressionSettings.GetValue()), static_cast<int32>(Array->MipGenSettings.GetValue()),
		*Array->GetPathName());

	return true;
}

bool UVoxelMaterialAtlas::BuildsTextureArrayAssets() const
{
	const UPackage* Package = GetPackage();
	return Package && Package != GetTransientPackage() && !Package->HasAnyFlags(RF_Transient) && IsAsset();
}

UTexture2DArray* UVoxelMaterialAtlas::AcquireTextureArrayObject(const FString& ArrayName, UTexture2DArray* Current)
{
	if (!BuildsTextureArrayAssets())
	{
		// A unique name: a rebuild in the same session must not construct the new array in place of the one the
		// terrain's materials still draw with (NewObject with an existing name reuses that object).
		return NewObject<UTexture2DArray>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
			UTexture2DArray::StaticClass(), FName(*FString::Printf(TEXT("VoxelMaterial%sArray"), *ArrayName))));
	}

	const FString AssetName = FString::Printf(TEXT("%s_%sArray"), *GetName(), *ArrayName);
	const FString PackageName = FPackageName::GetLongPackagePath(GetPackage()->GetName()) / AssetName;
	if (Current && Current->GetPackage()->GetName() == PackageName)
	{
		return Current;
	}

	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	UTexture2DArray* Existing = FindObject<UTexture2DArray>(nullptr, *ObjectPath);
	if (!Existing && FPackageName::DoesPackageExist(PackageName))
	{
		Existing = LoadObject<UTexture2DArray>(nullptr, *ObjectPath);
	}
	if (Existing)
	{
		return Existing;
	}

	UPackage* Package = CreatePackage(*PackageName);
	UTexture2DArray* Created = NewObject<UTexture2DArray>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
	{
		AssetRegistry->AssetCreated(Created);
	}
	UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("Created texture array asset %s for %s"), *Created->GetPathName(), *GetName());
	return Created;
}

UVoxelMaterialAtlas::FTextureArrayInputs UVoxelMaterialAtlas::ComputeTextureArrayInputs(bool bAllowLoad) const
{
	// Bump when BuildSingleTextureArray's output changes for the same inputs (its formats, filters, placeholders).
	// 2: one slice per distinct texture, each material's slice in the slice tables (F2.12)
	static constexpr int32 TextureArrayBuildVersion = 2;

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	FTextureArrayInputs Inputs;

	// A texture's part: its path and what identifies its content. A saved, unmodified package is identified by the
	// hash of its saved bytes (from the asset registry, so nothing is loaded); a texture in memory with unsaved
	// changes, or one outside a saved package (a transient one), by its source's id, which needs it loaded.
	auto TexturePart = [AssetRegistry, bAllowLoad](const TSoftObjectPtr<UTexture2D>& Soft, bool& bOutResolves) -> FString
	{
		if (Soft.IsNull())
		{
			return TEXT("-");
		}
		const FSoftObjectPath Path = Soft.ToSoftObjectPath();
		const FString PathString = Path.ToString();
		auto InMemory = [&PathString, &bOutResolves](const UTexture2D* Texture)
		{
			bOutResolves = true;
			return FString::Printf(TEXT("%s@mem:%s:%d"), *PathString, *Texture->Source.GetId().ToString(), Texture->SRGB ? 1 : 0);
		};

		const UTexture2D* Loaded = Soft.Get();
		const UPackage* LoadedPackage = Loaded ? Loaded->GetPackage() : nullptr;
		if (Loaded && (LoadedPackage->IsDirty() || LoadedPackage == GetTransientPackage() || LoadedPackage->HasAnyFlags(RF_Transient)))
		{
			return InMemory(Loaded);
		}

		const FName PackageName = Path.GetLongPackageFName();
		if (AssetRegistry && FPackageName::IsValidLongPackageName(PackageName.ToString()))
		{
			TOptional<FAssetPackageData> PackageData = AssetRegistry->GetAssetPackageDataCopy(PackageName);
			if (!PackageData.IsSet() && bAllowLoad)
			{
				// Not gathered yet (a commandlet, an early query): read this package's header now
				FString Filename;
				if (FPackageName::DoesPackageExist(PackageName.ToString(), &Filename))
				{
					AssetRegistry->ScanFilesSynchronous({ Filename });
					PackageData = AssetRegistry->GetAssetPackageDataCopy(PackageName);
				}
			}
			if (PackageData.IsSet() && !PackageData->GetPackageSavedHash().IsZero())
			{
				bOutResolves = true;
				return FString::Printf(TEXT("%s@disk:%s"), *PathString, *LexToString(PackageData->GetPackageSavedHash()));
			}
		}

		// No saved hash to go by: the texture itself
		if (const UTexture2D* Texture = bAllowLoad ? Soft.LoadSynchronous() : Soft.Get())
		{
			return InMemory(Texture);
		}
		return PathString + (bAllowLoad ? TEXT("@missing") : TEXT("@unknown"));
	};

	FString Description = FString::Printf(TEXT("v%d;size=%d;"), TextureArrayBuildVersion, TextureArraySize);
	for (const FVoxelMaterialTextureConfig& Config : MaterialConfigs)
	{
		Description += FString::Printf(TEXT("%d:A=%s,N=%s,R=%s;"), Config.MaterialID,
			*TexturePart(Config.AlbedoTexture, Inputs.bAlbedo),
			*TexturePart(Config.NormalTexture, Inputs.bNormal),
			*TexturePart(Config.RoughnessTexture, Inputs.bRoughness));
	}

	const FTCHARToUTF8 Utf8(*Description);
	Inputs.Key = LexToString(FBlake3::HashBuffer(Utf8.Get(), Utf8.Length()));
	return Inputs;
}

bool UVoxelMaterialAtlas::NeedsTextureArrayRebuild(FString* OutReason, bool* bOutMissing, bool bAllowLoad) const
{
	const FTextureArrayInputs Inputs = ComputeTextureArrayInputs(bAllowLoad);
	const bool bMissing = (Inputs.bAlbedo && !AlbedoArray) || (Inputs.bNormal && !NormalArray) || (Inputs.bRoughness && !RoughnessArray);
	if (bOutMissing)
	{
		*bOutMissing = bMissing;
	}
	const TCHAR* Reason = nullptr;
	if (Inputs.bAlbedo && !AlbedoArray)
	{
		Reason = TEXT("incomplete (no albedo array)");
	}
	else if (Inputs.bNormal && !NormalArray)
	{
		Reason = TEXT("incomplete (no normal array)");
	}
	else if (Inputs.bRoughness && !RoughnessArray)
	{
		Reason = TEXT("incomplete (no roughness array)");
	}
	else if (Inputs.Key != TextureArrayBuildKey
		// No source texture and no array (a packed-atlas-only atlas): nothing to build
		&& (Inputs.bAlbedo || Inputs.bNormal || Inputs.bRoughness || AlbedoArray || NormalArray || RoughnessArray))
	{
		Reason = TextureArrayBuildKey.IsEmpty() ? TEXT("never built into this atlas")
			: TEXT("built from other configs or source textures");
	}

	if (OutReason)
	{
		*OutReason = Reason ? Reason : TEXT("up to date");
	}
	return Reason != nullptr;
}
#endif
