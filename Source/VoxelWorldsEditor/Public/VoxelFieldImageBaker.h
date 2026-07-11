// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FVoxelFieldSampleContext;
struct FVoxelEditorField;
class UTexture2D;

/**
 * Region + resolution for a field bake.
 */
struct FVoxelFieldBakeParams
{
	/** World-space XY center of the square region to visualize. */
	FVector2D Center = FVector2D::ZeroVector;

	/** Full side length of the square region in world units (Center +/- RegionSize/2). */
	double RegionSize = 51200.0;

	/** Pixels per side of the output image. */
	int32 Resolution = 256;

	/** World Z at which Scalar3D fields (e.g. cave presence) are sampled. Ignored by 2D fields. */
	double SampleZ = 0.0;
};

/**
 * Bakes a field over a region into an image.
 *
 * Row-major, Resolution x Resolution, row index = world Y ascending, column = world X ascending —
 * the same convention as UVoxelMapSubsystem tiles, so a Height/Biome bake matches the minimap.
 * Synchronous (game thread) — sampling a modest-resolution region is cheap and keeps the field
 * samplers free to read the live config; it can move to async background baking later if needed.
 */
class FVoxelFieldImageBaker
{
public:
	/**
	 * Sample a field over the region into an FColor array.
	 *
	 * @param OutRangeMin,OutRangeMax  The scalar value range used for the ramp (fixed range, or the
	 *        region's actual min/max when the field auto-ranges) — for a UI legend. For categorical
	 *        fields these are left at the field's DisplayMin/Max.
	 * @param OutPresentCategoryIds  Optional — for categorical fields, receives the sorted set of unique
	 *        category ids (biome/material) that appear in the region (for building a legend). Left empty
	 *        for scalar fields.
	 * @return false if the context/field/params are invalid.
	 */
	static bool BakeToPixels(
		const FVoxelEditorField& Field,
		const FVoxelFieldSampleContext& Ctx,
		const FVoxelFieldBakeParams& Params,
		TArray<FColor>& OutPixels,
		float& OutRangeMin,
		float& OutRangeMax,
		TArray<int32>* OutPresentCategoryIds = nullptr);

	/**
	 * Bake into a transient UTexture2D (reusing ReuseTexture when its size matches, else creating one).
	 * Must be called on the game thread. Returns the texture (== ReuseTexture on failure).
	 */
	static UTexture2D* BakeToTexture(
		const FVoxelEditorField& Field,
		const FVoxelFieldSampleContext& Ctx,
		const FVoxelFieldBakeParams& Params,
		UTexture2D* ReuseTexture,
		float& OutRangeMin,
		float& OutRangeMax,
		TArray<int32>* OutPresentCategoryIds = nullptr);
};
