// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBiomeLookup.h"
#include "VoxelRendering.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeDefinition.h"

#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "UObject/Package.h"

const FName FVoxelBiomeLookup::ParameterName(TEXT("BiomeLUT"));

namespace
{
	/** A value in [-1, 1] mapped to a byte over 0..1 (the weather's reading of the climate). */
	uint8 ClimateByte(double Value)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt((Value * 0.5 + 0.5) * 255.0), 0, 255));
	}
}

void FVoxelBiomeLookup::Encode(const UVoxelBiomeConfiguration* Config, TArray<FColor>& OutTexels)
{
	OutTexels.Init(FColor(128, 128, 0, 255), Width);
	if (!Config)
	{
		return;
	}
	for (int32 i = 0; i < Width; ++i)
	{
		if (const FBiomeDefinition* Biome = Config->GetBiome(static_cast<uint8>(i)))
		{
			OutTexels[i] = FColor(ClimateByte(FMath::Min(Biome->TemperatureRange.X, Biome->TemperatureRange.Y)),
				ClimateByte(FMath::Max(Biome->TemperatureRange.X, Biome->TemperatureRange.Y)), 255, 255);
		}
	}
}

UTexture2D* FVoxelBiomeLookup::Build(const UVoxelBiomeConfiguration* Config, const FName& Name)
{
	TArray<FColor> Texels;
	Encode(Config, Texels);
	// Every world's lookup lives in the transient package: a name of its own for each, or a second world in the process (a
	// PIE server and its clients) would replace the first's texture in place while that world's renderer still holds it (G144)
	const FName UniqueName = MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), Name);
	UTexture2D* Texture = UTexture2D::CreateTransient(Width, 1, PF_B8G8R8A8, UniqueName);
	if (!Texture)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("VoxelBiomeLookup: no transient texture (no RHI?); the materials keep their default"));
		return nullptr;
	}
	Texture->Filter = TF_Nearest;
	Texture->SRGB = false;
	Texture->CompressionSettings = TC_VectorDisplacementmap; // uncompressed RGBA8, read as data
#if WITH_EDITORONLY_DATA
	// Editor-only data: a Game build has no mip generation to turn off (the transient texture has one mip either way)
	Texture->MipGenSettings = TMGS_NoMipmaps;
#endif
	Texture->NeverStream = true;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Data, Texels.GetData(), Texels.Num() * sizeof(FColor));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
	int32 Biomes = 0;
	for (const FColor& T : Texels)
	{
		Biomes += T.B == 255 ? 1 : 0;
	}
	UE_LOG(LogVoxelRendering, Log, TEXT("VoxelBiomeLookup: %d biome(s) encoded for the materials (%s)."), Biomes, *UniqueName.ToString());
	return Texture;
}
