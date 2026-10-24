// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMaterialAtlas.h"
#include "VoxelMaterialRegistry.h"
#include "Engine/Texture2DArray.h"
#include "ImageCore.h"
#include "TextureResource.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogVoxelMaterialAtlas, Log, All);

UVoxelMaterialAtlas::UVoxelMaterialAtlas()
{
	// Default grid size
	AtlasColumns = 4;
	AtlasRows = 4;
}

bool UVoxelMaterialAtlas::HasValidPackedAtlas() const
{
	return PackedAlbedoAtlas != nullptr && AtlasColumns > 0 && AtlasRows > 0;
}

bool UVoxelMaterialAtlas::HasValidTextureArrays() const
{
	return AlbedoArray != nullptr;
}

bool UVoxelMaterialAtlas::IsValid() const
{
	return HasValidPackedAtlas() || HasValidTextureArrays();
}

int32 UVoxelMaterialAtlas::GetMaterialCount() const
{
	return MaterialConfigs.Num();
}

int32 UVoxelMaterialAtlas::GetMaxPackedMaterials() const
{
	return AtlasColumns * AtlasRows;
}

FVector2D UVoxelMaterialAtlas::GetAtlasTileUVOffset(uint8 MaterialID) const
{
	if (AtlasColumns <= 0 || AtlasRows <= 0)
	{
		return FVector2D::ZeroVector;
	}

	// Look up material config to get atlas position
	const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(MaterialID);

	int32 Column = 0;
	int32 Row = 0;

	if (Config)
	{
		Column = FMath::Clamp(Config->AtlasColumn, 0, AtlasColumns - 1);
		Row = FMath::Clamp(Config->AtlasRow, 0, AtlasRows - 1);
	}
	else
	{
		// Fallback: derive position from MaterialID
		Column = MaterialID % AtlasColumns;
		Row = MaterialID / AtlasColumns;
	}

	const float TileWidth = 1.0f / AtlasColumns;
	const float TileHeight = 1.0f / AtlasRows;

	return FVector2D(Column * TileWidth, Row * TileHeight);
}

FVector2D UVoxelMaterialAtlas::GetAtlasTileUVScale() const
{
	if (AtlasColumns <= 0 || AtlasRows <= 0)
	{
		return FVector2D(1.0f, 1.0f);
	}

	return FVector2D(1.0f / AtlasColumns, 1.0f / AtlasRows);
}

int32 UVoxelMaterialAtlas::GetArrayIndex(uint8 MaterialID) const
{
	// For texture arrays, the index typically matches the MaterialID
	// unless we have a sparse configuration
	const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(MaterialID);

	if (Config)
	{
		// Config exists - MaterialID is the array index
		return MaterialID;
	}

	// No config found
	return -1;
}

float UVoxelMaterialAtlas::GetTriplanarScale(uint8 MaterialID) const
{
	const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(MaterialID);

	if (Config)
	{
		return Config->TriplanarScale;
	}

	return 1.0f;
}

const FVoxelMaterialTextureConfig* UVoxelMaterialAtlas::GetMaterialConfig(uint8 MaterialID) const
{
	if (bConfigIndexCacheDirty)
	{
		RebuildConfigIndexCache();
	}

	const int32* IndexPtr = MaterialIDToConfigIndex.Find(MaterialID);
	if (IndexPtr && *IndexPtr >= 0 && *IndexPtr < MaterialConfigs.Num())
	{
		return &MaterialConfigs[*IndexPtr];
	}

	return nullptr;
}

void UVoxelMaterialAtlas::InitializeFromRegistry()
{
	MaterialConfigs.Empty();

	const TArray<FVoxelMaterialDefinition>& Materials = FVoxelMaterialRegistry::GetAllMaterials();

	for (int32 i = 0; i < Materials.Num(); ++i)
	{
		const FVoxelMaterialDefinition& MatDef = Materials[i];

		FVoxelMaterialTextureConfig Config;
		Config.MaterialID = MatDef.MaterialID;
		Config.MaterialName = MatDef.Name;

		// Default atlas position based on material ID
		Config.AtlasColumn = MatDef.MaterialID % AtlasColumns;
		Config.AtlasRow = MatDef.MaterialID / AtlasColumns;

		// Default face variants to same tile (no variation)
		Config.bUseFaceVariants = false;
		Config.TopTile = FVoxelAtlasTile(Config.AtlasColumn, Config.AtlasRow);
		Config.SideTile = FVoxelAtlasTile(Config.AtlasColumn, Config.AtlasRow);
		Config.BottomTile = FVoxelAtlasTile(Config.AtlasColumn, Config.AtlasRow);

		Config.TriplanarScale = 1.0f;
		Config.UVScale = 1.0f;
		Config.bIsMasked = MatDef.bIsMasked;
		Config.bNonOccluding = MatDef.bNonOccluding;

		MaterialConfigs.Add(Config);
	}

	bConfigIndexCacheDirty = true;
	bLUTDirty = true;
}

bool UVoxelMaterialAtlas::IsMaterialMasked(uint8 MaterialID) const
{
	const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(MaterialID);
	return Config && Config->bIsMasked;
}

TSet<uint8> UVoxelMaterialAtlas::GetMaskedMaterialIDs() const
{
	TSet<uint8> Result;
	for (const FVoxelMaterialTextureConfig& Config : MaterialConfigs)
	{
		if (Config.bIsMasked)
		{
			Result.Add(Config.MaterialID);
		}
	}
	return Result;
}

FVoxelAtlasTile UVoxelMaterialAtlas::GetTileForFace(uint8 MaterialID, EVoxelFaceType FaceType) const
{
	const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(MaterialID);

	if (Config)
	{
		return Config->GetTileForFace(FaceType);
	}

	// Fallback: derive from MaterialID
	int32 Column = MaterialID % FMath::Max(1, AtlasColumns);
	int32 Row = MaterialID / FMath::Max(1, AtlasColumns);
	return FVoxelAtlasTile(Column, Row);
}

void UVoxelMaterialAtlas::BuildMaterialLUT()
{
	// LUT dimensions: 256 materials x 3 face types
	const int32 LUTWidth = 256;
	const int32 LUTHeight = 3;

	// Create or recreate the LUT texture
	if (!MaterialLUT || MaterialLUT->GetSizeX() != LUTWidth || MaterialLUT->GetSizeY() != LUTHeight)
	{
		MaterialLUT = UTexture2D::CreateTransient(LUTWidth, LUTHeight, PF_B8G8R8A8, TEXT("VoxelMaterialLUT"));
		if (!MaterialLUT)
		{
			UE_LOG(LogVoxelMaterialAtlas, Error, TEXT("UVoxelMaterialAtlas: Failed to create LUT texture"));
			return;
		}

		// Configure texture settings for point sampling (no filtering)
		MaterialLUT->Filter = TF_Nearest;
		MaterialLUT->SRGB = false;
		MaterialLUT->CompressionSettings = TC_VectorDisplacementmap; // No compression
#if WITH_EDITORONLY_DATA
		MaterialLUT->MipGenSettings = TMGS_NoMipmaps;
#endif
		MaterialLUT->AddressX = TA_Clamp;
		MaterialLUT->AddressY = TA_Clamp;
	}

	// Lock the texture for writing
	FTexture2DMipMap& Mip = MaterialLUT->GetPlatformData()->Mips[0];
	void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);

	if (!TextureData)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error, TEXT("UVoxelMaterialAtlas: Failed to lock LUT texture for writing"));
		return;
	}

	uint8* PixelData = static_cast<uint8*>(TextureData);

	// Fill the LUT
	// Format: BGRA (B8G8R8A8)
	// R = Atlas Column (0-255)
	// G = Atlas Row (0-255)
	// B = UV Scale * 25.5 (0-255 maps to 0.0-10.0)
	// A = Flags (reserved)

	for (int32 FaceTypeIndex = 0; FaceTypeIndex < 3; ++FaceTypeIndex)
	{
		EVoxelFaceType FaceType = static_cast<EVoxelFaceType>(FaceTypeIndex);

		for (int32 MaterialID = 0; MaterialID < 256; ++MaterialID)
		{
			int32 PixelIndex = (FaceTypeIndex * LUTWidth + MaterialID) * 4;

			FVoxelAtlasTile Tile = GetTileForFace(static_cast<uint8>(MaterialID), FaceType);

			// Get UV scale for this material
			float UVScale = 1.0f;
			const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(static_cast<uint8>(MaterialID));
			if (Config)
			{
				UVScale = Config->UVScale;
			}

			// Encode into BGRA
			PixelData[PixelIndex + 0] = static_cast<uint8>(FMath::Clamp(UVScale * 25.5f, 0.0f, 255.0f)); // B = UV Scale
			PixelData[PixelIndex + 1] = static_cast<uint8>(FMath::Clamp(Tile.Row, 0, 255));              // G = Row
			PixelData[PixelIndex + 2] = static_cast<uint8>(FMath::Clamp(Tile.Column, 0, 255));           // R = Column
			// A channel: flags (bit 0 = bIsMasked)
			uint8 Flags = 0;
			if (Config && Config->bIsMasked)
			{
				Flags |= 0x01;
			}
			PixelData[PixelIndex + 3] = Flags;
		}
	}

	// Unlock and update
	Mip.BulkData.Unlock();
	MaterialLUT->UpdateResource();

	bLUTDirty = false;

	UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("Built material LUT (%d x %d) with %d material configs"),
		LUTWidth, LUTHeight, MaterialConfigs.Num());
}

bool UVoxelMaterialAtlas::BuildSingleTextureArray(
	TObjectPtr<UTexture2DArray>& OutArray,
	TFunctionRef<TSoftObjectPtr<UTexture2D>(const FVoxelMaterialTextureConfig&)> TextureGetter,
	FColor PlaceholderColor,
	const FString& ArrayName)
{
	if (MaterialConfigs.Num() == 0)
	{
		UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): No material configs defined"), *ArrayName);
		return false;
	}

	// Find maximum MaterialID to determine array size
	uint8 MaxMaterialID = 0;
	for (const FVoxelMaterialTextureConfig& Config : MaterialConfigs)
	{
		MaxMaterialID = FMath::Max(MaxMaterialID, Config.MaterialID);
	}
	const int32 NumSlices = MaxMaterialID + 1;

	// Collect source textures (load soft pointers); a slot without one stays null
	TArray<UTexture2D*> SourceTextures;
	SourceTextures.SetNumZeroed(NumSlices);

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
		OutArray = nullptr;
		return false;
	}

	// A unique name: a rebuild in the same session must not construct the new array in place of the one the
	// terrain's materials still draw with (NewObject with an existing name reuses that object).
	OutArray = NewObject<UTexture2DArray>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
		UTexture2DArray::StaticClass(), FName(*FString::Printf(TEXT("VoxelMaterial%sArray"), *ArrayName))));
	if (!OutArray)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error, TEXT("BuildSingleTextureArray(%s): Failed to create UTexture2DArray"), *ArrayName);
		return false;
	}

	// Configure array settings.
	// TF_Default (NOT TF_Bilinear) so the array inherits the World texture group's
	// anisotropic + trilinear filtering (respects r.MaxAnisotropy). TF_Bilinear disables
	// anisotropy and uses point mip selection, which produces crawling minification
	// shimmer on triplanar terrain viewed at grazing angles.
	OutArray->Filter = TF_Default;
	OutArray->LODGroup = TEXTUREGROUP_World;

#if WITH_EDITOR
	// The array's source is built here rather than by UTexture2DArray::UpdateSourceFromSourceTextures(), which sizes
	// the array to its largest slice (4096 on the demo atlas, whatever TextureArraySize says) and converts every slice
	// to RGBA32F when their gamma spaces or formats differ: on the demo atlas 5.9 GB of source each for the albedo and
	// normal arrays, held for the editor session, 24 s of its first PIE and a texture build estimated at tens of GB,
	// then 800 MB of GPU memory. Every slice is stored at
	// TextureArraySize in BGRA8, converted from its own texture's gamma into the array's (sRGB for albedo, linear for
	// normals and roughness), so the shader samples the same values; each distinct texture is decoded and resized once
	// (the demo atlas repeats one texture in 14 of its 22 slots).
	const double StartSeconds = FPlatformTime::Seconds();
	const int32 SliceSize = static_cast<int32>(FMath::RoundUpToPowerOfTwo(static_cast<uint32>(FMath::Clamp(TextureArraySize, 64, 4096))));
	const bool bNormalArray = (ArrayName == TEXT("Normal"));
	const bool bSRGB = (ArrayName == TEXT("Albedo"));
	const EGammaSpace ArrayGamma = bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
	const int64 SliceBytes = int64(SliceSize) * SliceSize * sizeof(FColor);

	TArray64<uint8> ArrayData;
	ArrayData.SetNumUninitialized(SliceBytes * NumSlices);
	TArray<bool> SliceFilled;
	SliceFilled.SetNumZeroed(NumSlices);

	// Terrain textures tile: the resize filters across the wrapped edges
	const FImageCore::EResizeImageFilter ResizeFilter = FImageCore::EResizeImageFilter::Default
		| FImageCore::EResizeImageFilter::Flag_WrapX | FImageCore::EResizeImageFilter::Flag_WrapY;

	TMap<UTexture2D*, int32> BuiltSlice; // texture -> the slice it was built into (INDEX_NONE: unreadable)
	int32 FirstFilledSlice = INDEX_NONE;
	for (int32 SliceIndex = 0; SliceIndex < NumSlices; ++SliceIndex)
	{
		UTexture2D* Texture = SourceTextures[SliceIndex];
		if (!Texture)
		{
			continue;
		}
		uint8* SliceData = ArrayData.GetData() + SliceBytes * SliceIndex;

		if (const int32* Built = BuiltSlice.Find(Texture))
		{
			if (*Built != INDEX_NONE)
			{
				FMemory::Memcpy(SliceData, ArrayData.GetData() + SliceBytes * *Built, SliceBytes);
				SliceFilled[SliceIndex] = true;
			}
			continue;
		}

		FImage SourceImage;
		if (!Texture->Source.IsValid() || !Texture->Source.GetMipImage(SourceImage, 0))
		{
			UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): %s has no readable source data; its slices take the placeholder"),
				*ArrayName, *Texture->GetName());
			BuiltSlice.Add(Texture, INDEX_NONE);
			continue;
		}

		FImageCore::ResizeImage(SourceImage, FImageView(SliceData, SliceSize, SliceSize, 1, ERawImageFormat::BGRA8, ArrayGamma), ResizeFilter);
		BuiltSlice.Add(Texture, SliceIndex);
		SliceFilled[SliceIndex] = true;
		if (FirstFilledSlice == INDEX_NONE)
		{
			FirstFilledSlice = SliceIndex;
		}

		UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("  Slice[%d]: %s source %dx%d %s %s -> %dx%d"),
			SliceIndex, *Texture->GetName(), SourceImage.SizeX, SourceImage.SizeY,
			ERawImageFormat::GetName(SourceImage.Format), SourceImage.GammaSpace == EGammaSpace::Linear ? TEXT("linear") : TEXT("sRGB"),
			SliceSize, SliceSize);
	}

	// Slots without a texture take the first built slice, as before; with no readable texture at all, PlaceholderColor
	if (FirstFilledSlice == INDEX_NONE)
	{
		UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): no texture had readable source data; every slice is the placeholder colour"), *ArrayName);
		FColor* Pixels = reinterpret_cast<FColor*>(ArrayData.GetData());
		for (int64 PixelIndex = 0; PixelIndex < int64(SliceSize) * SliceSize; ++PixelIndex)
		{
			Pixels[PixelIndex] = PlaceholderColor;
		}
		SliceFilled[0] = true;
		FirstFilledSlice = 0;
	}
	int32 PlaceholderCount = 0;
	for (int32 SliceIndex = 0; SliceIndex < NumSlices; ++SliceIndex)
	{
		if (!SliceFilled[SliceIndex])
		{
			FMemory::Memcpy(ArrayData.GetData() + SliceBytes * SliceIndex, ArrayData.GetData() + SliceBytes * FirstFilledSlice, SliceBytes);
			++PlaceholderCount;
		}
	}

	// Settings pinned per array, not taken from whichever texture lands in slice 0: BC1/BC3 colour, BC5 normals
	// (in the normal-map texture group), BC1/BC3 linear roughness
	OutArray->Source.Init(SliceSize, SliceSize, NumSlices, 1, TSF_BGRA8, ArrayData.GetData());
	OutArray->SRGB = bSRGB;
	OutArray->CompressionSettings = bNormalArray ? TC_Normalmap : (bSRGB ? TC_Default : TC_Masks);
	OutArray->LODGroup = bNormalArray ? TEXTUREGROUP_WorldNormalMap : TEXTUREGROUP_World;
	// A full mip chain (TMGS_FromTextureGroup = the World group's): single-mip slices minify into crawling shimmer
	OutArray->MipGenSettings = TMGS_FromTextureGroup;
	OutArray->PowerOfTwoMode = ETexturePowerOfTwoSetting::None;
	OutArray->NeverStream = true;
	OutArray->ValidateSettingsAfterImportOrEdit();
	OutArray->SetLightingGuid();

	// One build of the platform data (compressed, mipped), asynchronous in the editor and cached in the DDC by the
	// source's hash: the same atlas builds once and is fetched in later sessions.
	OutArray->UpdateResource();

	UE_LOG(LogVoxelMaterialAtlas, Log,
		TEXT("BuildSingleTextureArray(%s): Built %d slices at %dx%d BGRA8 %s (%d distinct textures, %d from configs, %d placeholders) in %.2f s, compression %d, MipGen %d"),
		*ArrayName, NumSlices, SliceSize, SliceSize, bSRGB ? TEXT("sRGB") : TEXT("linear"), BuiltSlice.Num(), LoadedCount, PlaceholderCount,
		FPlatformTime::Seconds() - StartSeconds, static_cast<int32>(OutArray->CompressionSettings.GetValue()), static_cast<int32>(OutArray->MipGenSettings.GetValue()));
#endif

	return true;
}

void UVoxelMaterialAtlas::BuildTextureArrays()
{
	UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("Building texture arrays from %d material configs..."), MaterialConfigs.Num());

	// Log each config to help debug missing textures
	for (int32 i = 0; i < MaterialConfigs.Num(); ++i)
	{
		const FVoxelMaterialTextureConfig& Config = MaterialConfigs[i];
		UE_LOG(LogVoxelMaterialAtlas, Verbose, TEXT("  Config[%d]: MaterialID=%d, Name=%s, HasAlbedo=%s, HasNormal=%s"),
			i, Config.MaterialID, *Config.MaterialName,
			Config.AlbedoTexture.IsNull() ? TEXT("No") : TEXT("Yes"),
			Config.NormalTexture.IsNull() ? TEXT("No") : TEXT("Yes"));
	}

	// Build Albedo array
	bool bAlbedoSuccess = BuildSingleTextureArray(
		AlbedoArray,
		[](const FVoxelMaterialTextureConfig& Config) { return Config.AlbedoTexture; },
		FColor(128, 128, 128, 255), // Gray placeholder
		TEXT("Albedo")
	);

	// Build Normal array
	bool bNormalSuccess = BuildSingleTextureArray(
		NormalArray,
		[](const FVoxelMaterialTextureConfig& Config) { return Config.NormalTexture; },
		FColor(128, 128, 255, 255), // Flat normal (pointing up in tangent space)
		TEXT("Normal")
	);

	// Build Roughness array
	bool bRoughnessSuccess = BuildSingleTextureArray(
		RoughnessArray,
		[](const FVoxelMaterialTextureConfig& Config) { return Config.RoughnessTexture; },
		FColor(128, 128, 128, 255), // 0.5 roughness
		TEXT("Roughness")
	);

	bTextureArraysDirty = false;

	if (bAlbedoSuccess || bNormalSuccess || bRoughnessSuccess)
	{
		UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("Texture arrays built successfully (Albedo: %s, Normal: %s, Roughness: %s)"),
			bAlbedoSuccess ? TEXT("Yes") : TEXT("No"),
			bNormalSuccess ? TEXT("Yes") : TEXT("No"),
			bRoughnessSuccess ? TEXT("Yes") : TEXT("No"));
	}
	else
	{
		UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("No texture arrays were built - check that MaterialConfigs have source textures assigned"));
	}

	// Refresh the 2D map colors from the same source textures we just packed.
	BakeMapColors();
}

// ---------------------------------------------------------------------------
// 2D map colors
// ---------------------------------------------------------------------------

void UVoxelMaterialAtlas::BakeMapColors()
{
#if WITH_EDITOR
	int32 BakedCount = 0;

	for (FVoxelMaterialTextureConfig& Config : MaterialConfigs)
	{
		if (Config.AlbedoTexture.IsNull())
		{
			continue;
		}

		UTexture2D* Texture = Config.AlbedoTexture.LoadSynchronous();
		if (!Texture || !Texture->Source.IsValid())
		{
			UE_LOG(LogVoxelMaterialAtlas, Warning,
				TEXT("BakeMapColors: MaterialID %d (%s) has no readable source texture — keeping registry palette color"),
				Config.MaterialID, *Config.MaterialName);
			continue;
		}

		// Read mip 0 and convert to linear float; averaging sRGB bytes directly would bias the
		// result (gamma is non-linear), which is why we go through RGBA32F/Linear here.
		FImage SourceImage;
		if (!Texture->Source.GetMipImage(SourceImage, 0))
		{
			UE_LOG(LogVoxelMaterialAtlas, Warning,
				TEXT("BakeMapColors: failed to read mip 0 for MaterialID %d (%s)"),
				Config.MaterialID, *Config.MaterialName);
			continue;
		}

		FImage LinearImage;
		SourceImage.CopyTo(LinearImage, ERawImageFormat::RGBA32F, EGammaSpace::Linear);

		const TArrayView64<FLinearColor> Pixels = LinearImage.AsRGBA32F();

		FLinearColor Sum(0.0f, 0.0f, 0.0f, 0.0f);
		int64 Counted = 0;
		for (const FLinearColor& Pixel : Pixels)
		{
			// Skip fully transparent texels: for masked materials (leaves) the cutout background
			// would otherwise drag the average away from the actual foliage color.
			if (Pixel.A <= 0.0f)
			{
				continue;
			}
			Sum += FLinearColor(Pixel.R, Pixel.G, Pixel.B, 0.0f);
			++Counted;
		}

		if (Counted == 0)
		{
			UE_LOG(LogVoxelMaterialAtlas, Warning,
				TEXT("BakeMapColors: MaterialID %d (%s) albedo is fully transparent — keeping registry palette color"),
				Config.MaterialID, *Config.MaterialName);
			continue;
		}

		const float InvCount = 1.0f / static_cast<float>(Counted);
		const FLinearColor Average(Sum.R * InvCount, Sum.G * InvCount, Sum.B * InvCount, 1.0f);

		// Back to sRGB bytes — the map texture is an sRGB FColor buffer.
		Config.MapColor = Average.ToFColor(/*bSRGB=*/true);
		Config.MapColor.A = 255; // alpha 255 marks "baked"
		++BakedCount;

		UE_LOG(LogVoxelMaterialAtlas, Verbose, TEXT("BakeMapColors: MaterialID %d (%s) -> (%d,%d,%d) from %lld texels"),
			Config.MaterialID, *Config.MaterialName,
			Config.MapColor.R, Config.MapColor.G, Config.MapColor.B, Counted);
	}

	if (BakedCount > 0)
	{
		MarkPackageDirty();
	}

	UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("BakeMapColors: baked %d of %d material map colors"),
		BakedCount, MaterialConfigs.Num());
#else
	// Cooked builds have no texture source data; the colors baked in the editor are already
	// serialized into the asset, so this is a no-op by design.
	UE_LOG(LogVoxelMaterialAtlas, Verbose, TEXT("BakeMapColors: editor-only (using colors baked into the asset)"));
#endif
}

FColor UVoxelMaterialAtlas::GetMapColor(uint8 MaterialID) const
{
	if (const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(MaterialID))
	{
		// Alpha 0 = never baked (or bake failed) -> fall through to the registry palette.
		if (Config->MapColor.A > 0)
		{
			return Config->MapColor;
		}
	}

	return FVoxelMaterialRegistry::GetMaterialColor(MaterialID);
}

void UVoxelMaterialAtlas::BuildMapPalette(TArray<FColor>& OutPalette) const
{
	OutPalette.SetNumUninitialized(256);
	for (int32 i = 0; i < 256; ++i)
	{
		OutPalette[i] = GetMapColor(static_cast<uint8>(i));
	}
}

void UVoxelMaterialAtlas::RebuildConfigIndexCache() const
{
	MaterialIDToConfigIndex.Empty();
	MaterialIDToConfigIndex.Reserve(MaterialConfigs.Num());

	for (int32 i = 0; i < MaterialConfigs.Num(); ++i)
	{
		MaterialIDToConfigIndex.Add(MaterialConfigs[i].MaterialID, i);
	}

	bConfigIndexCacheDirty = false;
}

#if WITH_EDITOR
EDataValidationResult UVoxelMaterialAtlas::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	// Check packed atlas configuration
	if (PackedAlbedoAtlas)
	{
		if (AtlasColumns <= 0 || AtlasRows <= 0)
		{
			Context.AddError(FText::FromString(TEXT("Atlas has textures but invalid grid dimensions (Columns/Rows must be > 0)")));
			Result = EDataValidationResult::Invalid;
		}

		// Warn if normal/roughness missing
		if (!PackedNormalAtlas)
		{
			Context.AddWarning(FText::FromString(TEXT("PackedNormalAtlas is not set - normal mapping will be disabled for cubic terrain")));
		}
		if (!PackedRoughnessAtlas)
		{
			Context.AddWarning(FText::FromString(TEXT("PackedRoughnessAtlas is not set - roughness will use default value for cubic terrain")));
		}
	}

	// Check texture array configuration
	if (AlbedoArray)
	{
		if (!NormalArray)
		{
			Context.AddWarning(FText::FromString(TEXT("NormalArray is not set - normal mapping will be disabled for smooth terrain")));
		}
		if (!RoughnessArray)
		{
			Context.AddWarning(FText::FromString(TEXT("RoughnessArray is not set - roughness will use default value for smooth terrain")));
		}
	}

	// Helper lambda to validate tile position
	auto ValidateTile = [this, &Context, &Result](const FVoxelAtlasTile& Tile, int32 ConfigIndex, const TCHAR* TileName)
	{
		if (Tile.Column < 0 || Tile.Column >= AtlasColumns)
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("MaterialConfigs[%d].%s has invalid Column %d (must be 0-%d)"),
				ConfigIndex, TileName, Tile.Column, AtlasColumns - 1)));
			Result = EDataValidationResult::Invalid;
		}
		if (Tile.Row < 0 || Tile.Row >= AtlasRows)
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("MaterialConfigs[%d].%s has invalid Row %d (must be 0-%d)"),
				ConfigIndex, TileName, Tile.Row, AtlasRows - 1)));
			Result = EDataValidationResult::Invalid;
		}
	};

	// Check material configs
	TSet<uint8> UsedMaterialIDs;
	for (int32 i = 0; i < MaterialConfigs.Num(); ++i)
	{
		const FVoxelMaterialTextureConfig& Config = MaterialConfigs[i];

		// Check for duplicate MaterialIDs
		if (UsedMaterialIDs.Contains(Config.MaterialID))
		{
			Context.AddError(FText::FromString(FString::Printf(
				TEXT("Duplicate MaterialID %d found in MaterialConfigs[%d]"),
				Config.MaterialID, i)));
			Result = EDataValidationResult::Invalid;
		}
		UsedMaterialIDs.Add(Config.MaterialID);

		// Validate atlas positions
		if (PackedAlbedoAtlas)
		{
			if (Config.bUseFaceVariants)
			{
				// Validate face-specific tiles
				ValidateTile(Config.TopTile, i, TEXT("TopTile"));
				ValidateTile(Config.SideTile, i, TEXT("SideTile"));
				ValidateTile(Config.BottomTile, i, TEXT("BottomTile"));
			}
			else
			{
				// Validate default tile position
				if (Config.AtlasColumn < 0 || Config.AtlasColumn >= AtlasColumns)
				{
					Context.AddError(FText::FromString(FString::Printf(
						TEXT("MaterialConfigs[%d] has invalid AtlasColumn %d (must be 0-%d)"),
						i, Config.AtlasColumn, AtlasColumns - 1)));
					Result = EDataValidationResult::Invalid;
				}
				if (Config.AtlasRow < 0 || Config.AtlasRow >= AtlasRows)
				{
					Context.AddError(FText::FromString(FString::Printf(
						TEXT("MaterialConfigs[%d] has invalid AtlasRow %d (must be 0-%d)"),
						i, Config.AtlasRow, AtlasRows - 1)));
					Result = EDataValidationResult::Invalid;
				}
			}
		}
	}

	// Warn if LUT is dirty
	if (bLUTDirty)
	{
		Context.AddWarning(FText::FromString(TEXT("Material LUT needs rebuilding. Click 'Build Material LUT' button.")));
	}

	// Warn if texture arrays are dirty
	if (bTextureArraysDirty)
	{
		Context.AddWarning(FText::FromString(TEXT("Texture arrays need rebuilding. Click 'Build Texture Arrays' button.")));
	}

	// Check if any materials have source textures but arrays aren't built
	bool bHasSourceTextures = false;
	for (const FVoxelMaterialTextureConfig& Config : MaterialConfigs)
	{
		if (!Config.AlbedoTexture.IsNull() || !Config.NormalTexture.IsNull() || !Config.RoughnessTexture.IsNull())
		{
			bHasSourceTextures = true;
			break;
		}
	}
	if (bHasSourceTextures && !AlbedoArray)
	{
		Context.AddWarning(FText::FromString(TEXT("Materials have source textures but texture arrays are not built. Click 'Build Texture Arrays' to generate them.")));
	}

	// Overall validation
	if (!IsValid())
	{
		Context.AddError(FText::FromString(TEXT("Atlas has no valid textures configured (need either PackedAlbedoAtlas or AlbedoArray)")));
		Result = EDataValidationResult::Invalid;
	}

	return Result;
}

void UVoxelMaterialAtlas::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Mark LUT as dirty when relevant properties change
	FName PropertyName = PropertyChangedEvent.GetPropertyName();
	FName MemberPropertyName = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;

	// Properties that affect the LUT
	static const TSet<FName> LUTAffectingProperties = {
		GET_MEMBER_NAME_CHECKED(UVoxelMaterialAtlas, MaterialConfigs),
		GET_MEMBER_NAME_CHECKED(UVoxelMaterialAtlas, AtlasColumns),
		GET_MEMBER_NAME_CHECKED(UVoxelMaterialAtlas, AtlasRows),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, MaterialID),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, bUseFaceVariants),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, TopTile),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, SideTile),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, BottomTile),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, AtlasColumn),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, AtlasRow),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, UVScale),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, bIsMasked),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, bNonOccluding),
	};

	// Properties that affect texture arrays
	static const TSet<FName> TextureArrayAffectingProperties = {
		GET_MEMBER_NAME_CHECKED(UVoxelMaterialAtlas, MaterialConfigs),
		GET_MEMBER_NAME_CHECKED(UVoxelMaterialAtlas, TextureArraySize),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, MaterialID),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, AlbedoTexture),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, NormalTexture),
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, RoughnessTexture),
	};

	if (LUTAffectingProperties.Contains(PropertyName) || LUTAffectingProperties.Contains(MemberPropertyName))
	{
		bLUTDirty = true;
		bConfigIndexCacheDirty = true;
	}

	if (TextureArrayAffectingProperties.Contains(PropertyName) || TextureArrayAffectingProperties.Contains(MemberPropertyName))
	{
		bTextureArraysDirty = true;
		bConfigIndexCacheDirty = true;
	}
}
#endif
