// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMaterialAtlas.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelMaterialAtlasPrivate.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "ImageCore.h"
#include "TextureResource.h"

#if WITH_EDITOR
#include "AssetRegistry/IAssetRegistry.h"
#include "Hash/Blake3.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#endif

DEFINE_LOG_CATEGORY(LogVoxelMaterialAtlas);

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
	if (GetMaterialConfig(MaterialID))
	{
		return GetArraySlice(AlbedoArraySlices, MaterialID);
	}

	// No config found
	return -1;
}

uint8 UVoxelMaterialAtlas::GetArraySlice(const TArray<uint8>& Slices, uint8 MaterialID)
{
	return Slices.IsValidIndex(MaterialID) ? Slices[MaterialID] : MaterialID;
}

int32 UVoxelMaterialAtlas::GetSlopeMaterialID(uint8 MaterialID) const
{
	const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(MaterialID);
	if (!Config || Config->SlopeMaterialID < 0 || Config->SlopeMaterialID > 254 || Config->SlopeMaterialID == MaterialID
		|| !GetMaterialConfig(static_cast<uint8>(Config->SlopeMaterialID)))
	{
		return -1;
	}
	return Config->SlopeMaterialID;
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
	// LUT dimensions: 256 materials x (3 face types + the texture array slices)
	const int32 LUTWidth = 256;
	const int32 LUTHeight = 4;
	const int32 SliceRow = 3;

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

	// The slice row: each material's slice in the texture arrays (smooth terrain's triplanar functions), BGRA; A is the
	// material the triplanar functions blend toward on steep ground (VP2-O3), plus one so that 0 is none
	for (int32 MaterialID = 0; MaterialID < 256; ++MaterialID)
	{
		const int32 PixelIndex = (SliceRow * LUTWidth + MaterialID) * 4;
		PixelData[PixelIndex + 0] = GetArraySlice(RoughnessArraySlices, static_cast<uint8>(MaterialID)); // B
		PixelData[PixelIndex + 1] = GetArraySlice(NormalArraySlices, static_cast<uint8>(MaterialID));    // G
		PixelData[PixelIndex + 2] = GetArraySlice(AlbedoArraySlices, static_cast<uint8>(MaterialID));    // R
		PixelData[PixelIndex + 3] = static_cast<uint8>(GetSlopeMaterialID(static_cast<uint8>(MaterialID)) + 1);  // A
	}

	// Unlock and update
	Mip.BulkData.Unlock();
	MaterialLUT->UpdateResource();

	bLUTDirty = false;

	UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("Built material LUT (%d x %d) with %d material configs"),
		LUTWidth, LUTHeight, MaterialConfigs.Num());
}

bool UVoxelMaterialAtlas::AreTextureArraysDirty() const
{
#if WITH_EDITOR
	return NeedsTextureArrayRebuild();
#else
	return false;
#endif
}

bool UVoxelMaterialAtlas::EnsureTextureArrays()
{
#if WITH_EDITOR
	// A cook saves what is on disk; PreSave reports arrays that are missing or stale there
	FString Reason;
	if (!IsRunningCookCommandlet() && NeedsTextureArrayRebuild(&Reason))
	{
		UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("%s: texture arrays %s; rebuilding them%s"), *GetName(), *Reason,
			BuildsTextureArrayAssets() ? TEXT(" (save the atlas and its arrays to keep them; a packaged game uses the saved ones)") : TEXT(""));
		BuildTextureArrays();
	}
#else
	bool bHasSourceTextures = false;
	for (const FVoxelMaterialTextureConfig& Config : MaterialConfigs)
	{
		bHasSourceTextures |= !Config.AlbedoTexture.IsNull();
	}
	if (bHasSourceTextures && !AlbedoArray && !bReportedMissingTextureArrays)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error,
			TEXT("%s has source textures but no saved texture arrays: smooth terrain draws without textures. Build them in the editor (Build Texture Arrays) and save them."),
			*GetName());
		bReportedMissingTextureArrays = true;
	}
#endif
	return AlbedoArray != nullptr;
}

void UVoxelMaterialAtlas::BuildTextureArrays()
{
#if !WITH_EDITOR
	// Outside the editor there is no source data: the arrays are the saved ones, kept as they are
	UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("%s: BuildTextureArrays is editor-only; using the saved arrays"), *GetName());
#else
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
		AlbedoArraySlices,
		[](const FVoxelMaterialTextureConfig& Config) { return Config.AlbedoTexture; },
		FColor(128, 128, 128, 255), // Gray placeholder
		TEXT("Albedo")
	);

	// Build Normal array
	bool bNormalSuccess = BuildSingleTextureArray(
		NormalArray,
		NormalArraySlices,
		[](const FVoxelMaterialTextureConfig& Config) { return Config.NormalTexture; },
		FColor(128, 128, 255, 255), // Flat normal (pointing up in tangent space)
		TEXT("Normal")
	);

	// Build Roughness array
	bool bRoughnessSuccess = BuildSingleTextureArray(
		RoughnessArray,
		RoughnessArraySlices,
		[](const FVoxelMaterialTextureConfig& Config) { return Config.RoughnessTexture; },
		FColor(128, 128, 128, 255), // 0.5 roughness
		TEXT("Roughness")
	);

	// The inputs these arrays were built from, saved with the atlas (the textures are loaded and unmodified now, so
	// their parts are their saved packages' hashes, as when nothing is loaded)
	TextureArrayBuildKey = ComputeTextureArrayInputs().Key;

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

	// The slice tables changed with the arrays: the material LUT carries them to the shader. A LUT that exists is bound
	// to materials already, and is rebuilt in place so they read the new slices with the new arrays.
	bLUTDirty = true;
	if (MaterialLUT)
	{
		BuildMaterialLUT();
	}

	// Refresh the 2D map colors from the same source textures we just packed.
	BakeMapColors();

	// The atlas now references the arrays and holds their key (set directly: MarkPackageDirty does nothing in PIE)
	if (BuildsTextureArrayAssets())
	{
		GetPackage()->SetDirtyFlag(true);
	}
#endif
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
	return GetMapColor(MaterialID, *MakeMaterialTable());
}

FColor UVoxelMaterialAtlas::GetMapColor(uint8 MaterialID, const FVoxelMaterialTable& Materials) const
{
	if (const FVoxelMaterialTextureConfig* Config = GetMaterialConfig(MaterialID))
	{
		// Alpha 0 = never baked (or bake failed) -> fall through to the table's palette.
		if (Config->MapColor.A > 0)
		{
			return Config->MapColor;
		}
	}

	return Materials.GetMaterialColor(MaterialID);
}

void UVoxelMaterialAtlas::BuildMapPalette(TArray<FColor>& OutPalette) const
{
	const TSharedRef<const FVoxelMaterialTable> Materials = MakeMaterialTable();
	OutPalette.SetNumUninitialized(256);
	for (int32 i = 0; i < 256; ++i)
	{
		OutPalette[i] = GetMapColor(static_cast<uint8>(i), *Materials);
	}
}

TSharedRef<const FVoxelMaterialTable> UVoxelMaterialAtlas::MakeMaterialTable() const
{
	return FVoxelMaterialTable::FromAtlas(MaterialConfigs, AtlasColumns, AtlasRows);
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

	// Missing or stale texture arrays: a packaged game binds the saved ones (an error, so that a cook run with
	// -RunAssetValidation -ValidationErrorsAreFatal fails on them)
	FString ArrayReason;
	if (NeedsTextureArrayRebuild(&ArrayReason))
	{
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("Texture arrays need rebuilding (%s). Click 'Build Texture Arrays' and save the atlas and its arrays."), *ArrayReason)));
		Result = EDataValidationResult::Invalid;
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
		GET_MEMBER_NAME_CHECKED(FVoxelMaterialTextureConfig, SlopeMaterialID),
	};

	if (LUTAffectingProperties.Contains(PropertyName) || LUTAffectingProperties.Contains(MemberPropertyName))
	{
		bLUTDirty = true;
		bConfigIndexCacheDirty = true;
	}

	// The texture arrays need no flag: their build key (ComputeTextureArrayInputs) covers the configs and
	// TextureArraySize, and the next world to bind the atlas rebuilds them when it differs
}

void UVoxelMaterialAtlas::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);

	if (!SaveContext.IsCooking())
	{
		return;
	}
	// Nothing is loaded while saving: a source texture without a saved hash that is not loaded counts as changed
	FString Reason;
	bool bMissing = false;
	if (!NeedsTextureArrayRebuild(&Reason, &bMissing, /*bAllowLoad=*/false))
	{
		return;
	}
	if (bMissing)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error,
			TEXT("Cooking %s: texture arrays %s, so smooth terrain using it lacks those textures in this build. Build Texture Arrays in the editor and save the atlas and its arrays."),
			*GetPathName(), *Reason);
	}
	else
	{
		UE_LOG(LogVoxelMaterialAtlas, Warning,
			TEXT("Cooking %s: its saved texture arrays are stale (%s); the build ships them as they are. Build Texture Arrays in the editor and save to update them."),
			*GetPathName(), *Reason);
	}
}
#endif
