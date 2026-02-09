// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "VoxelMaterialAtlas.generated.h"

/**
 * Face type for directional texture mapping.
 */
UENUM(BlueprintType)
enum class EVoxelFaceType : uint8
{
	Top = 0,    // +Z facing (up)
	Side = 1,   // X/Y facing (sides)
	Bottom = 2  // -Z facing (down)
};

/**
 * Atlas tile position (column, row).
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelAtlasTile
{
	GENERATED_BODY()

	/** Column in atlas (0-based) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Atlas")
	int32 Column = 0;

	/** Row in atlas (0-based) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Atlas")
	int32 Row = 0;

	FVoxelAtlasTile() = default;
	FVoxelAtlasTile(int32 InColumn, int32 InRow) : Column(InColumn), Row(InRow) {}

	bool operator==(const FVoxelAtlasTile& Other) const
	{
		return Column == Other.Column && Row == Other.Row;
	}
};

/**
 * Configuration for a single material in the atlas.
 * Maps MaterialID to atlas position and source textures.
 * Supports per-face texture variants (top/side/bottom).
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelMaterialTextureConfig
{
	GENERATED_BODY()

	/** Material ID this config applies to (0-255) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Material")
	uint8 MaterialID = 0;

	/** Display name for this material */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Material")
	FString MaterialName;

	// ===== Face Variants =====

	/** Enable different textures for top/side/bottom faces */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Face Variants")
	bool bUseFaceVariants = false;

	/** Atlas tile for top faces (+Z normal). Used when bUseFaceVariants is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Face Variants", meta = (EditCondition = "bUseFaceVariants"))
	FVoxelAtlasTile TopTile;

	/** Atlas tile for side faces (X/Y normals). Used when bUseFaceVariants is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Face Variants", meta = (EditCondition = "bUseFaceVariants"))
	FVoxelAtlasTile SideTile;

	/** Atlas tile for bottom faces (-Z normal). Used when bUseFaceVariants is true. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Face Variants", meta = (EditCondition = "bUseFaceVariants"))
	FVoxelAtlasTile BottomTile;

	// ===== Default Atlas Position (when not using face variants) =====

	/** Column position in packed atlas (0-based). Used when bUseFaceVariants is false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Packed Atlas", meta = (EditCondition = "!bUseFaceVariants"))
	int32 AtlasColumn = 0;

	/** Row position in packed atlas (0-based). Used when bUseFaceVariants is false. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Packed Atlas", meta = (EditCondition = "!bUseFaceVariants"))
	int32 AtlasRow = 0;

	// ===== Source Textures for Texture2DArray (Smooth Terrain) =====

	/** Albedo/BaseColor texture for this material */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Textures")
	TSoftObjectPtr<UTexture2D> AlbedoTexture;

	/** Normal map texture for this material */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Textures")
	TSoftObjectPtr<UTexture2D> NormalTexture;

	/** Roughness texture for this material (R channel) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Textures")
	TSoftObjectPtr<UTexture2D> RoughnessTexture;

	// ===== Per-Material Properties =====

	/** Scale for triplanar projection (smooth terrain). Higher = more tiling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Properties", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float TriplanarScale = 1.0f;

	/** UV scale multiplier for packed atlas sampling */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Properties", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float UVScale = 1.0f;

	/** Use masked (alpha cutout) blending for this material.
	 *  Requires albedo texture to have alpha channel with cutout pattern. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Properties")
	bool bIsMasked = false;

	/** Non-occluding material (like glass or leaves).
	 *  When true, faces between this material and different materials are always generated.
	 *  Same-material adjacency still culls normally (no internal face explosion). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Properties")
	bool bNonOccluding = false;

	FVoxelMaterialTextureConfig() = default;

	/** Get the atlas tile for a specific face type */
	FVoxelAtlasTile GetTileForFace(EVoxelFaceType FaceType) const
	{
		if (!bUseFaceVariants)
		{
			return FVoxelAtlasTile(AtlasColumn, AtlasRow);
		}

		switch (FaceType)
		{
		case EVoxelFaceType::Top:
			return TopTile;
		case EVoxelFaceType::Side:
			return SideTile;
		case EVoxelFaceType::Bottom:
			return BottomTile;
		default:
			return SideTile;
		}
	}
};

/**
 * Data asset defining the material atlas configuration for voxel terrain.
 *
 * Supports two rendering modes:
 * - Cubic Terrain: Uses packed texture atlases with UV-based sampling
 * - Smooth Terrain: Uses Texture2DArrays with triplanar projection
 *
 * Create this asset in the Content Browser, configure the atlas textures,
 * and assign it to your VoxelWorldComponent.
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelMaterialAtlas : public UDataAsset
{
	GENERATED_BODY()

public:
	UVoxelMaterialAtlas();

	// ===== Packed Atlas (Cubic Terrain) =====

	/** Packed albedo atlas texture for cubic meshing (UV-based sampling) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Packed Atlas")
	TObjectPtr<UTexture2D> PackedAlbedoAtlas;

	/** Packed normal map atlas for cubic meshing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Packed Atlas")
	TObjectPtr<UTexture2D> PackedNormalAtlas;

	/** Packed roughness atlas for cubic meshing (R = Roughness, G = Metallic optional) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Packed Atlas")
	TObjectPtr<UTexture2D> PackedRoughnessAtlas;

	/** Number of columns in the packed atlas grid */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Packed Atlas", meta = (ClampMin = "1", ClampMax = "16"))
	int32 AtlasColumns = 4;

	/** Number of rows in the packed atlas grid */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Packed Atlas", meta = (ClampMin = "1", ClampMax = "16"))
	int32 AtlasRows = 4;

	// ===== Texture Arrays (Smooth Terrain) - Auto-generated from MaterialConfigs =====

	/** Albedo Texture2DArray for triplanar sampling (auto-built from MaterialConfigs) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Texture Arrays")
	TObjectPtr<UTexture2DArray> AlbedoArray;

	/** Normal map Texture2DArray for triplanar sampling (auto-built from MaterialConfigs) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Texture Arrays")
	TObjectPtr<UTexture2DArray> NormalArray;

	/** Roughness Texture2DArray for triplanar sampling (auto-built from MaterialConfigs) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Texture Arrays")
	TObjectPtr<UTexture2DArray> RoughnessArray;

	/** Target texture size for generated arrays (textures will be resized to match) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Arrays", meta = (ClampMin = "64", ClampMax = "4096"))
	int32 TextureArraySize = 512;

	// ===== Per-Material Configuration =====

	/** Configuration for each material in the atlas */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Materials")
	TArray<FVoxelMaterialTextureConfig> MaterialConfigs;

	// ===== API =====

	/**
	 * Check if the atlas has valid packed atlas textures.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	bool HasValidPackedAtlas() const;

	/**
	 * Check if the atlas has valid texture arrays.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	bool HasValidTextureArrays() const;

	/**
	 * Check if the atlas is valid for rendering (either mode).
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	bool IsValid() const;

	/**
	 * Get the number of configured materials.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	int32 GetMaterialCount() const;

	/**
	 * Get the maximum number of materials the packed atlas can hold.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	int32 GetMaxPackedMaterials() const;

	/**
	 * Get UV offset for a material in the packed atlas.
	 * @param MaterialID The material ID to look up
	 * @return UV offset (0-1 range) for the material's tile
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	FVector2D GetAtlasTileUVOffset(uint8 MaterialID) const;

	/**
	 * Get the UV scale for a single tile in the packed atlas.
	 * @return UV scale (1/Columns, 1/Rows)
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	FVector2D GetAtlasTileUVScale() const;

	/**
	 * Get the texture array index for a material.
	 * @param MaterialID The material ID to look up
	 * @return Index into the texture arrays (-1 if not found)
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	int32 GetArrayIndex(uint8 MaterialID) const;

	/**
	 * Get the triplanar scale for a material.
	 * @param MaterialID The material ID to look up
	 * @return Triplanar scale (defaults to 1.0 if not found)
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	float GetTriplanarScale(uint8 MaterialID) const;

	/**
	 * Get material config by MaterialID.
	 * @param MaterialID The material ID to look up
	 * @return Pointer to config, or nullptr if not found
	 */
	const FVoxelMaterialTextureConfig* GetMaterialConfig(uint8 MaterialID) const;

	/**
	 * Check if a material uses masked (alpha cutout) blending.
	 * @param MaterialID The material ID to check
	 * @return true if the material has bIsMasked set
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	bool IsMaterialMasked(uint8 MaterialID) const;

	/**
	 * Initialize material configs from the material registry defaults.
	 * Call this to auto-populate configs with registered materials.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel|Material Atlas")
	void InitializeFromRegistry();

	// ===== Material Lookup Table (LUT) =====

	/**
	 * Build or rebuild the material lookup table texture.
	 * This texture encodes per-material, per-face atlas positions.
	 * Call this after modifying MaterialConfigs.
	 *
	 * LUT Format: 256 x 3 texture (MaterialID x FaceType)
	 * - Row 0: Top face tiles
	 * - Row 1: Side face tiles
	 * - Row 2: Bottom face tiles
	 * - R channel: Atlas column (0-255)
	 * - G channel: Atlas row (0-255)
	 * - B channel: Reserved (UV scale * 25.5, clamped to 0-255)
	 * - A channel: Flags (bit 0 = bIsMasked)
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel|Material Atlas")
	void BuildMaterialLUT();

	/**
	 * Get the material lookup table texture.
	 * Returns nullptr if not built yet - call BuildMaterialLUT() first.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	UTexture2D* GetMaterialLUT() const { return MaterialLUT; }

	/**
	 * Check if the LUT needs rebuilding (configs changed since last build).
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	bool IsLUTDirty() const { return bLUTDirty; }

	/**
	 * Get the set of MaterialIDs that use masked blending.
	 * Useful for efficient triangle partitioning during rendering.
	 */
	TSet<uint8> GetMaskedMaterialIDs() const;

	// ===== Texture Array Building =====

	/**
	 * Build or rebuild texture arrays from individual MaterialConfig textures.
	 * Creates AlbedoArray, NormalArray, and RoughnessArray from the per-material
	 * source textures defined in MaterialConfigs.
	 *
	 * All source textures will be resized to TextureArraySize x TextureArraySize.
	 * Materials without textures will use a placeholder (solid color).
	 *
	 * Call this after modifying MaterialConfigs or source textures.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Voxel|Material Atlas")
	void BuildTextureArrays();

	/**
	 * Check if texture arrays need rebuilding.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	bool AreTextureArraysDirty() const { return bTextureArraysDirty; }

	/**
	 * Get the atlas tile for a material and face type.
	 * @param MaterialID The material ID
	 * @param FaceType The face direction (Top, Side, Bottom)
	 * @return Atlas tile position
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Material Atlas")
	FVoxelAtlasTile GetTileForFace(uint8 MaterialID, EVoxelFaceType FaceType) const;

#if WITH_EDITOR
	/**
	 * Validate the atlas configuration and report any issues.
	 */
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;

	/**
	 * Called when a property changes in the editor.
	 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Cached mapping from MaterialID to config index for fast lookup */
	mutable TMap<uint8, int32> MaterialIDToConfigIndex;
	mutable bool bConfigIndexCacheDirty = true;

	/** Rebuild the MaterialID to config index cache */
	void RebuildConfigIndexCache() const;

	/**
	 * Material lookup table texture (runtime generated).
	 * Not saved - rebuilt when needed.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> MaterialLUT;

	/** Whether the LUT needs rebuilding */
	mutable bool bLUTDirty = true;

	/** Whether texture arrays need rebuilding */
	mutable bool bTextureArraysDirty = true;

	/**
	 * Helper to create a solid color placeholder texture.
	 * Used when a material config doesn't have a source texture assigned.
	 */
	UTexture2D* CreatePlaceholderTexture(FColor Color, int32 Size) const;

	/**
	 * Helper to build a single texture array from source textures.
	 * @param OutArray The array to populate
	 * @param TextureGetter Function to get the source texture from a config
	 * @param PlaceholderColor Color for missing textures
	 * @param ArrayName Name for the created array (for debugging)
	 * @return true if successful
	 */
	bool BuildSingleTextureArray(
		TObjectPtr<UTexture2DArray>& OutArray,
		TFunctionRef<TSoftObjectPtr<UTexture2D>(const FVoxelMaterialTextureConfig&)> TextureGetter,
		FColor PlaceholderColor,
		const FString& ArrayName);
};
