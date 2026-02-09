// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMaterialAtlas.h"
#include "VoxelMaterialRegistry.h"
#include "Engine/Texture2DArray.h"
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

UTexture2D* UVoxelMaterialAtlas::CreatePlaceholderTexture(FColor Color, int32 Size) const
{
	UTexture2D* Texture = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8);
	if (!Texture)
	{
		return nullptr;
	}

	// Configure texture
	Texture->SRGB = true;
	Texture->Filter = TF_Bilinear;
	Texture->CompressionSettings = TC_Default;
#if WITH_EDITORONLY_DATA
	Texture->MipGenSettings = TMGS_NoMipmaps;
#endif

	// Fill with solid color
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	if (TextureData)
	{
		uint8* PixelData = static_cast<uint8*>(TextureData);
		const int32 NumPixels = Size * Size;
		for (int32 i = 0; i < NumPixels; ++i)
		{
			// BGRA format
			PixelData[i * 4 + 0] = Color.B;
			PixelData[i * 4 + 1] = Color.G;
			PixelData[i * 4 + 2] = Color.R;
			PixelData[i * 4 + 3] = Color.A;
		}
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
	}

	return Texture;
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

	// Collect source textures (load soft pointers)
	TArray<UTexture2D*> SourceTextures;
	SourceTextures.SetNum(NumSlices);

	// Initialize all to nullptr
	for (int32 i = 0; i < NumSlices; ++i)
	{
		SourceTextures[i] = nullptr;
	}

	// Load textures from configs
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

	// Determine texture size and format from first valid loaded texture
	int32 ActualTextureSize = TextureArraySize;
	EPixelFormat SourceFormat = PF_B8G8R8A8;
	UTexture2D* FirstValidTexture = nullptr;
	for (int32 i = 0; i < NumSlices; ++i)
	{
		if (SourceTextures[i] && SourceTextures[i]->GetSizeX() > 0)
		{
			FirstValidTexture = SourceTextures[i];
			ActualTextureSize = SourceTextures[i]->GetSizeX();
			if (SourceTextures[i]->GetPlatformData() && SourceTextures[i]->GetPlatformData()->Mips.Num() > 0)
			{
				SourceFormat = SourceTextures[i]->GetPlatformData()->PixelFormat;
			}
			UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("BuildSingleTextureArray(%s): Detected texture size %dx%d format %d from slot %d"),
				*ArrayName, ActualTextureSize, ActualTextureSize, (int32)SourceFormat, i);
			break;
		}
	}

	// For placeholder, we'll use the first valid texture itself as a template
	// This avoids format mismatch issues with compressed textures
	UTexture2D* Placeholder = FirstValidTexture ? FirstValidTexture : CreatePlaceholderTexture(PlaceholderColor, ActualTextureSize);
	if (!Placeholder)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error, TEXT("BuildSingleTextureArray(%s): Failed to create placeholder texture"), *ArrayName);
		return false;
	}

	UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("BuildSingleTextureArray(%s): Using %s as placeholder: %dx%d"),
		*ArrayName, FirstValidTexture ? TEXT("first valid texture") : TEXT("generated placeholder"),
		Placeholder->GetSizeX(), Placeholder->GetSizeY());

	// Fill missing slots with placeholder
	for (int32 i = 0; i < NumSlices; ++i)
	{
		if (!SourceTextures[i])
		{
			SourceTextures[i] = Placeholder;
		}
	}

	// Verify all textures have valid dimensions
	int32 FirstValidWidth = 0, FirstValidHeight = 0;
	for (int32 i = 0; i < NumSlices; ++i)
	{
		if (SourceTextures[i])
		{
			int32 W = SourceTextures[i]->GetSizeX();
			int32 H = SourceTextures[i]->GetSizeY();
			if (W > 0 && H > 0)
			{
				if (FirstValidWidth == 0)
				{
					FirstValidWidth = W;
					FirstValidHeight = H;
				}
				else if (W != FirstValidWidth || H != FirstValidHeight)
				{
					UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): Texture[%d] size mismatch: %dx%d vs expected %dx%d"),
						*ArrayName, i, W, H, FirstValidWidth, FirstValidHeight);
				}
			}
			else
			{
				UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("BuildSingleTextureArray(%s): Texture[%d] has invalid size: %dx%d"),
					*ArrayName, i, W, H);
			}
		}
	}

	// Create the Texture2DArray
	OutArray = NewObject<UTexture2DArray>(GetTransientPackage(), *FString::Printf(TEXT("VoxelMaterial%sArray"), *ArrayName));
	if (!OutArray)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error, TEXT("BuildSingleTextureArray(%s): Failed to create UTexture2DArray"), *ArrayName);
		return false;
	}

	// Configure array settings
	OutArray->Filter = TF_Bilinear;
	OutArray->SRGB = (ArrayName != TEXT("Normal")); // Normal maps should not be sRGB
	OutArray->LODGroup = TEXTUREGROUP_World;

#if WITH_EDITOR
	// In editor, we can use the source textures directly
	OutArray->SourceTextures.Empty();

	// Check format compatibility of all textures
	EPixelFormat ExpectedFormat = PF_Unknown;
	bool bFormatMismatch = false;

	for (int32 i = 0; i < SourceTextures.Num(); ++i)
	{
		UTexture2D* Texture = SourceTextures[i];
		if (Texture)
		{
			OutArray->SourceTextures.Add(Texture);

			// Get texture format for logging
			EPixelFormat TexFormat = PF_Unknown;
			if (Texture->GetPlatformData() && Texture->GetPlatformData()->Mips.Num() > 0)
			{
				TexFormat = Texture->GetPlatformData()->PixelFormat;
			}

			if (ExpectedFormat == PF_Unknown)
			{
				ExpectedFormat = TexFormat;
			}
			else if (TexFormat != ExpectedFormat)
			{
				bFormatMismatch = true;
				UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("  Slice[%d]: %s (%dx%d) FORMAT MISMATCH: %d vs expected %d"),
					i, *Texture->GetName(), Texture->GetSizeX(), Texture->GetSizeY(), (int32)TexFormat, (int32)ExpectedFormat);
			}

			UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("  Slice[%d]: %s (%dx%d, format=%d, sRGB=%s)"),
				i, *Texture->GetName(), Texture->GetSizeX(), Texture->GetSizeY(),
				(int32)TexFormat, Texture->SRGB ? TEXT("Yes") : TEXT("No"));
		}
		else
		{
			UE_LOG(LogVoxelMaterialAtlas, Warning, TEXT("  Slice[%d]: NULL texture!"), i);
			OutArray->SourceTextures.Add(Placeholder); // Ensure we add something
		}
	}

	if (bFormatMismatch)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error, TEXT("BuildSingleTextureArray(%s): FORMAT MISMATCH detected! All textures must have the same compression format. Array may fail to build."), *ArrayName);
	}

	// Update the array resource from source textures
	UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("BuildSingleTextureArray(%s): Calling UpdateSourceFromSourceTextures with %d textures..."), *ArrayName, OutArray->SourceTextures.Num());
	OutArray->UpdateSourceFromSourceTextures();

	// Check dimensions after update
	int32 ResultSizeX = OutArray->GetSizeX();
	int32 ResultSizeY = OutArray->GetSizeY();

	if (ResultSizeX == 0 || ResultSizeY == 0)
	{
		UE_LOG(LogVoxelMaterialAtlas, Error, TEXT("BuildSingleTextureArray(%s): UpdateSourceFromSourceTextures FAILED - result is %dx%d! Check texture formats and compression settings."),
			*ArrayName, ResultSizeX, ResultSizeY);
	}
	else
	{
		// Upload to GPU
		OutArray->UpdateResource();
		UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("BuildSingleTextureArray(%s): SUCCESS - Array dimensions: %dx%dx%d"),
			*ArrayName, ResultSizeX, ResultSizeY, OutArray->SourceTextures.Num());
	}
#endif

	UE_LOG(LogVoxelMaterialAtlas, Log, TEXT("BuildSingleTextureArray(%s): Created array with %d slices (%d from configs, %d placeholders)"),
		*ArrayName, NumSlices, LoadedCount, NumSlices - LoadedCount);

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
