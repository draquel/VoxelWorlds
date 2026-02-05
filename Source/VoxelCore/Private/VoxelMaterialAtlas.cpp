// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMaterialAtlas.h"
#include "VoxelMaterialRegistry.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

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

		MaterialConfigs.Add(Config);
	}

	bConfigIndexCacheDirty = true;
	bLUTDirty = true;
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
			UE_LOG(LogTemp, Error, TEXT("UVoxelMaterialAtlas: Failed to create LUT texture"));
			return;
		}

		// Configure texture settings for point sampling (no filtering)
		MaterialLUT->Filter = TF_Nearest;
		MaterialLUT->SRGB = false;
		MaterialLUT->CompressionSettings = TC_VectorDisplacementmap; // No compression
		MaterialLUT->MipGenSettings = TMGS_NoMipmaps;
		MaterialLUT->AddressX = TA_Clamp;
		MaterialLUT->AddressY = TA_Clamp;
	}

	// Lock the texture for writing
	FTexture2DMipMap& Mip = MaterialLUT->GetPlatformData()->Mips[0];
	void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);

	if (!TextureData)
	{
		UE_LOG(LogTemp, Error, TEXT("UVoxelMaterialAtlas: Failed to lock LUT texture for writing"));
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
			PixelData[PixelIndex + 3] = 255;                                                              // A = Reserved
		}
	}

	// Unlock and update
	Mip.BulkData.Unlock();
	MaterialLUT->UpdateResource();

	bLUTDirty = false;

	UE_LOG(LogTemp, Log, TEXT("UVoxelMaterialAtlas: Built material LUT (%d x %d) with %d material configs"),
		LUTWidth, LUTHeight, MaterialConfigs.Num());
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
	};

	if (LUTAffectingProperties.Contains(PropertyName) || LUTAffectingProperties.Contains(MemberPropertyName))
	{
		bLUTDirty = true;
		bConfigIndexCacheDirty = true;
	}
}
#endif
