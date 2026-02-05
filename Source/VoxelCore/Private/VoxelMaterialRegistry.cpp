// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMaterialRegistry.h"
#include "VoxelMaterialAtlas.h"

TArray<FVoxelMaterialDefinition> FVoxelMaterialRegistry::Materials;
bool FVoxelMaterialRegistry::bInitialized = false;

void FVoxelMaterialRegistry::EnsureInitialized()
{
	if (bInitialized)
	{
		return;
	}

	Materials.Empty();
	Materials.Reserve(EVoxelMaterial::Count);

	// Define all materials with their colors and default atlas positions
	// Colors chosen for clear visual distinction
	// Default atlas layout: 4x2 grid (row 0: Grass, Dirt, Stone, Sand; row 1: Snow, Sandstone, FrozenDirt, ...)

	// Grass - Forest Green (Col 0, Row 0)
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Grass,
		TEXT("Grass"),
		FColor(34, 139, 34),
		0, 0  // AtlasColumn, AtlasRow
	));

	// Dirt - Brown (Col 1, Row 0)
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Dirt,
		TEXT("Dirt"),
		FColor(139, 90, 43),
		1, 0
	));

	// Stone - Gray (Col 2, Row 0)
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Stone,
		TEXT("Stone"),
		FColor(128, 128, 128),
		2, 0
	));

	// Sand - Tan (Col 3, Row 0)
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Sand,
		TEXT("Sand"),
		FColor(237, 201, 175),
		3, 0
	));

	// Snow - White (Col 0, Row 1)
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Snow,
		TEXT("Snow"),
		FColor(255, 250, 250),
		0, 1
	));

	// Sandstone - Dark Tan (Col 1, Row 1)
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Sandstone,
		TEXT("Sandstone"),
		FColor(210, 180, 140),
		1, 1
	));

	// Frozen Dirt - Gray-Blue (Col 2, Row 1)
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::FrozenDirt,
		TEXT("Frozen Dirt"),
		FColor(119, 110, 120),
		2, 1
	));

	bInitialized = true;
}

const FVoxelMaterialDefinition* FVoxelMaterialRegistry::GetMaterial(uint8 MaterialID)
{
	EnsureInitialized();

	if (MaterialID < Materials.Num())
	{
		return &Materials[MaterialID];
	}

	return nullptr;
}

FColor FVoxelMaterialRegistry::GetMaterialColor(uint8 MaterialID)
{
	EnsureInitialized();

	if (MaterialID < Materials.Num())
	{
		return Materials[MaterialID].Color;
	}

	// Return magenta for invalid/unknown materials (easy to spot)
	return FColor::Magenta;
}

int32 FVoxelMaterialRegistry::GetMaterialCount()
{
	EnsureInitialized();
	return Materials.Num();
}

const TArray<FVoxelMaterialDefinition>& FVoxelMaterialRegistry::GetAllMaterials()
{
	EnsureInitialized();
	return Materials;
}

// ===== Atlas Support =====

FVector2D FVoxelMaterialRegistry::GetAtlasUVOffset(uint8 MaterialID, int32 Columns, int32 Rows)
{
	EnsureInitialized();

	if (Columns <= 0 || Rows <= 0)
	{
		return FVector2D::ZeroVector;
	}

	int32 Column = 0;
	int32 Row = 0;

	if (MaterialID < Materials.Num())
	{
		Column = FMath::Clamp(Materials[MaterialID].AtlasColumn, 0, Columns - 1);
		Row = FMath::Clamp(Materials[MaterialID].AtlasRow, 0, Rows - 1);
	}
	else
	{
		// Fallback: derive position from MaterialID
		Column = MaterialID % Columns;
		Row = MaterialID / Columns;
	}

	const float TileWidth = 1.0f / Columns;
	const float TileHeight = 1.0f / Rows;

	return FVector2D(Column * TileWidth, Row * TileHeight);
}

int32 FVoxelMaterialRegistry::GetArrayIndex(uint8 MaterialID)
{
	EnsureInitialized();

	if (MaterialID < Materials.Num())
	{
		return Materials[MaterialID].ArrayIndex;
	}

	return -1;
}

float FVoxelMaterialRegistry::GetTriplanarScale(uint8 MaterialID)
{
	EnsureInitialized();

	if (MaterialID < Materials.Num())
	{
		return Materials[MaterialID].TriplanarScale;
	}

	return 1.0f;
}

float FVoxelMaterialRegistry::GetUVScale(uint8 MaterialID)
{
	EnsureInitialized();

	if (MaterialID < Materials.Num())
	{
		return Materials[MaterialID].UVScale;
	}

	return 1.0f;
}

void FVoxelMaterialRegistry::SetAtlasPositions(const TArray<FVoxelMaterialTextureConfig>& Configs, int32 AtlasColumns, int32 AtlasRows)
{
	EnsureInitialized();

	for (const FVoxelMaterialTextureConfig& Config : Configs)
	{
		if (Config.MaterialID < Materials.Num())
		{
			FVoxelMaterialDefinition& Mat = Materials[Config.MaterialID];
			Mat.AtlasColumn = Config.AtlasColumn;
			Mat.AtlasRow = Config.AtlasRow;
			Mat.ArrayIndex = Config.MaterialID;  // Array index matches MaterialID
			Mat.TriplanarScale = Config.TriplanarScale;
			Mat.UVScale = Config.UVScale;
		}
	}
}
