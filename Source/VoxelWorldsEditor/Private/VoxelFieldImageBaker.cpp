// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelFieldImageBaker.h"
#include "VoxelFieldTypes.h"

#include "Engine/Texture2D.h"
#include "TextureResource.h"

bool FVoxelFieldImageBaker::BakeToPixels(
	const FVoxelEditorField& Field,
	const FVoxelFieldSampleContext& Ctx,
	const FVoxelFieldBakeParams& Params,
	TArray<FColor>& OutPixels,
	float& OutRangeMin,
	float& OutRangeMax,
	TArray<int32>* OutPresentCategoryIds)
{
	OutRangeMin = Field.DisplayMin;
	OutRangeMax = Field.DisplayMax;
	if (OutPresentCategoryIds)
	{
		OutPresentCategoryIds->Reset();
	}

	if (!Ctx.IsValid() || !Field.Sample || Params.Resolution <= 0 || Params.RegionSize <= 0.0)
	{
		return false;
	}

	const bool bCollectCategories = (OutPresentCategoryIds != nullptr) && (Field.Kind == EVoxelFieldKind::Categorical);
	TSet<int32> PresentCategories;

	const int32 Res = FMath::Clamp(Params.Resolution, 1, 4096);
	const double Half = Params.RegionSize * 0.5;
	const double Step = (Res > 1) ? (Params.RegionSize / static_cast<double>(Res - 1)) : 0.0;
	const double X0 = Params.Center.X - Half;
	const double Y0 = Params.Center.Y - Half;
	const double Z = Params.SampleZ;

	// Pass 1: sample raw field values, tracking the actual range (for auto-ranged scalar fields).
	TArray<float> Raw;
	Raw.SetNumUninitialized(Res * Res);
	float MinV = TNumericLimits<float>::Max();
	float MaxV = TNumericLimits<float>::Lowest();

	for (int32 PY = 0; PY < Res; ++PY)
	{
		const double WY = Y0 + Step * PY;
		for (int32 PX = 0; PX < Res; ++PX)
		{
			const double WX = X0 + Step * PX;
			const float V = Field.Sample(Ctx, WX, WY, Z);
			Raw[PY * Res + PX] = V;
			MinV = FMath::Min(MinV, V);
			MaxV = FMath::Max(MaxV, V);
			if (bCollectCategories)
			{
				PresentCategories.Add(FMath::Clamp(FMath::RoundToInt(V), 0, 255));
			}
		}
	}

	// Resolve the scalar display range.
	float RangeMin = Field.DisplayMin;
	float RangeMax = Field.DisplayMax;
	if (Field.Kind != EVoxelFieldKind::Categorical && Field.bAutoRange)
	{
		RangeMin = MinV;
		RangeMax = MaxV;
		if (RangeMax <= RangeMin)
		{
			RangeMax = RangeMin + 1.0f; // avoid a zero-width ramp on flat regions
		}
	}
	OutRangeMin = RangeMin;
	OutRangeMax = RangeMax;

	// Pass 2: map to colors.
	OutPixels.SetNumUninitialized(Res * Res);
	const bool bCategorical = (Field.Kind == EVoxelFieldKind::Categorical);
	for (int32 i = 0; i < Res * Res; ++i)
	{
		OutPixels[i] = bCategorical
			? Field.MapCategoricalToColor(Raw[i])
			: Field.MapScalarToColor(Raw[i], RangeMin, RangeMax);
	}

	if (bCollectCategories)
	{
		OutPresentCategoryIds->Append(PresentCategories.Array());
		OutPresentCategoryIds->Sort();
	}

	return true;
}

UTexture2D* FVoxelFieldImageBaker::BakeToTexture(
	const FVoxelEditorField& Field,
	const FVoxelFieldSampleContext& Ctx,
	const FVoxelFieldBakeParams& Params,
	UTexture2D* ReuseTexture,
	float& OutRangeMin,
	float& OutRangeMax,
	TArray<int32>* OutPresentCategoryIds)
{
	TArray<FColor> Pixels;
	if (!BakeToPixels(Field, Ctx, Params, Pixels, OutRangeMin, OutRangeMax, OutPresentCategoryIds))
	{
		return ReuseTexture;
	}

	const int32 Res = FMath::Clamp(Params.Resolution, 1, 4096);

	UTexture2D* Texture = ReuseTexture;
	if (!Texture || Texture->GetSizeX() != Res || Texture->GetSizeY() != Res)
	{
		Texture = UTexture2D::CreateTransient(Res, Res, PF_B8G8R8A8);
		if (!Texture)
		{
			return ReuseTexture;
		}
		Texture->SRGB = true;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
	}

	FTexturePlatformData* PlatformData = Texture->GetPlatformData();
	if (!PlatformData || PlatformData->Mips.Num() == 0)
	{
		return Texture;
	}

	FTexture2DMipMap& Mip = PlatformData->Mips[0];
	void* Dest = Mip.BulkData.Lock(LOCK_READ_WRITE);
	if (Dest)
	{
		FMemory::Memcpy(Dest, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	}
	Mip.BulkData.Unlock();
	Texture->UpdateResource();

	return Texture;
}
