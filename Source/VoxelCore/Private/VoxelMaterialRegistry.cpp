// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelMaterialRegistry.h"

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

	// Define all materials with their colors
	// Colors chosen for clear visual distinction

	// Grass - Forest Green
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Grass,
		TEXT("Grass"),
		FColor(34, 139, 34)
	));

	// Dirt - Brown
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Dirt,
		TEXT("Dirt"),
		FColor(139, 90, 43)
	));

	// Stone - Gray
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Stone,
		TEXT("Stone"),
		FColor(128, 128, 128)
	));

	// Sand - Tan
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Sand,
		TEXT("Sand"),
		FColor(237, 201, 175)
	));

	// Snow - White
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Snow,
		TEXT("Snow"),
		FColor(255, 250, 250)
	));

	// Sandstone - Dark Tan
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::Sandstone,
		TEXT("Sandstone"),
		FColor(210, 180, 140)
	));

	// Frozen Dirt - Gray-Blue
	Materials.Add(FVoxelMaterialDefinition(
		EVoxelMaterial::FrozenDirt,
		TEXT("Frozen Dirt"),
		FColor(119, 110, 120)
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
