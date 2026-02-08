// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UStaticMesh;
class UMaterialInstanceDynamic;
class UTexture2D;

/**
 * Generates runtime cross-billboard meshes for cubic scatter (grass, flowers).
 *
 * Each billboard consists of two quads intersecting at 90 degrees,
 * pivot at bottom center. Meshes are cached by size to avoid re-creation.
 *
 * Thread Safety: Must be called from game thread only (creates UObjects).
 */
class VOXELSCATTER_API FVoxelBillboardMeshGenerator
{
public:
	/**
	 * Get or create a cross-billboard static mesh of the given size.
	 * Meshes are cached by (Width, Height, UVMin, UVMax) key.
	 *
	 * @param Width Width of each quad in cm
	 * @param Height Height of each quad in cm
	 * @param UVMin Minimum UV coordinate (default 0,0 for full texture)
	 * @param UVMax Maximum UV coordinate (default 1,1 for full texture)
	 * @return Static mesh with 2 intersecting quads, or nullptr on failure
	 */
	static UStaticMesh* GetOrCreateBillboardMesh(float Width, float Height,
		FVector2f UVMin = FVector2f(0.0f, 0.0f), FVector2f UVMax = FVector2f(1.0f, 1.0f));

	/**
	 * Create a billboard material instance from a texture.
	 * Uses an alpha-tested, two-sided material.
	 *
	 * @param Texture The billboard texture (with alpha channel)
	 * @param Outer Outer object for the material instance
	 * @return Material instance, or nullptr on failure
	 */
	static UMaterialInstanceDynamic* CreateBillboardMaterial(UTexture2D* Texture, UObject* Outer);

	/**
	 * Clear all cached meshes. Call on shutdown.
	 */
	static void ClearCache();

private:
	/** Cache key from width/height */
	static int64 MakeCacheKey(float Width, float Height);

	/** Cached billboard meshes by size key */
	static TMap<int64, TWeakObjectPtr<UStaticMesh>> CachedMeshes;
};
