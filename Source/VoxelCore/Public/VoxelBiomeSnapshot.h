// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelBiomeDefinition.h"

class UVoxelBiomeConfiguration;

/**
 * Plain-value, UObject-free snapshot of the biome-configuration data the terrain pipeline needs:
 * continentalness height modulation (noise field parameters + baked modulation curves) and the
 * biome/material selection data (biome definitions, blend width, underwater + height-rule settings,
 * temperature/moisture noise parameters).
 *
 * Captured once on the game thread via FromConfig() and then copyable into worker threads or
 * long-lived async tasks that may outlive the world (e.g. VoxelMap tile generation), where holding
 * the UVoxelBiomeConfiguration* is unsafe — the UObject can be GC'd under a running task.
 *
 * World modes store this by value (see IVoxelWorldMode::SetBiomeContext), which makes the analytic
 * GetTerrainHeightAt query safe to share into background tasks while still applying the exact same
 * height modulation as chunk generation.
 *
 * SINGLE SOURCE OF TRUTH: the biome blend / material selection algorithms live in the static
 * Compute- / Select- / Apply- functions below. UVoxelBiomeConfiguration's GetBiomeBlend,
 * GetBlendedMaterial[WithWater] and ApplyHeightMaterialRules delegate to the same statics with the
 * UObject's own members (zero copies on the hot generation path), so the UObject and snapshot
 * paths cannot drift.
 */
struct VOXELCORE_API FVoxelBiomeSnapshot
{
	// ==================== Validity ====================

	/** Mirrors (Config != null && Config->IsValid()) at capture time. False (default) = no biome data. */
	bool bIsValid = false;

	// ==================== Continentalness (height modulation) ====================

	/** Mirrors UVoxelBiomeConfiguration::bEnableContinentalness. False (default) = no modulation. */
	bool bEnableContinentalness = false;

	/** Mirrors UVoxelBiomeConfiguration::ContinentalnessSeedOffset (added to the world seed). */
	int32 ContinentalnessSeedOffset = 9012;

	/** Mirrors UVoxelBiomeConfiguration::ContinentalnessNoiseFrequency. */
	float ContinentalnessNoiseFrequency = 0.00002f;

	/** Baked continentalness→height-offset curve samples (evenly spaced over [-1,1]). */
	TArray<float> BakedHeightCurve;

	/** Baked continentalness→HeightScale-multiplier curve samples (evenly spaced over [-1,1]). */
	TArray<float> BakedHeightScaleCurve;

	// ==================== Biome / material selection ====================

	/** Biome definitions, in the config's array order (the blend visits them in the same order). */
	TArray<FBiomeDefinition> Biomes;

	/** Mirrors UVoxelBiomeConfiguration::BiomeBlendWidth. */
	float BiomeBlendWidth = 0.15f;

	/** Mirrors UVoxelBiomeConfiguration::bEnableUnderwaterMaterials. */
	bool bEnableUnderwaterMaterials = true;

	/** Mirrors UVoxelBiomeConfiguration::DefaultUnderwaterMaterial. */
	uint8 DefaultUnderwaterMaterial = 3;

	/** Mirrors UVoxelBiomeConfiguration::bEnableHeightMaterials. */
	bool bEnableHeightMaterials = true;

	/** Height material rules, PRIORITY-SORTED at capture (mirrors the config's SortedHeightRules cache). */
	TArray<FHeightMaterialRule> SortedHeightRules;

	// Temperature / moisture noise parameters. Defaults match both the config defaults and the
	// CPU generator's no-config fallback (seed +1234 / freq 0.00005, seed +5678 / freq 0.00007),
	// so a default snapshot reproduces the legacy fallback climate sampling.
	int32 TemperatureSeedOffset = 1234;
	float TemperatureNoiseFrequency = 0.00005f;
	int32 MoistureSeedOffset = 5678;
	float MoistureNoiseFrequency = 0.00007f;

	// ==================== Capture ====================

	/**
	 * Build a snapshot from a configuration. Null (or a config with continentalness disabled)
	 * yields the default snapshot, which is a no-op for height modulation and invalid for
	 * biome/material selection.
	 * Game-thread only (reads the UObject); the returned value is thread-safe to copy and share.
	 */
	static FVoxelBiomeSnapshot FromConfig(const UVoxelBiomeConfiguration* Config);

	// ==================== Continentalness queries ====================

	/**
	 * Map continentalness [-1,1] to terrain height offset and HeightScale multiplier by lerping the
	 * baked curve samples — identical math to UVoxelBiomeConfiguration::GetContinentalnessTerrainParams
	 * (both delegate to EvalBakedCurves, so the two paths cannot drift).
	 */
	void GetContinentalnessTerrainParams(float Continentalness, float& OutHeightOffset, float& OutHeightScaleMultiplier) const
	{
		EvalBakedCurves(BakedHeightCurve, BakedHeightScaleCurve, Continentalness, OutHeightOffset, OutHeightScaleMultiplier);
	}

	/**
	 * Shared baked-curve lerp core (the single implementation behind both this snapshot and
	 * UVoxelBiomeConfiguration::GetContinentalnessTerrainParams). Returns the identity modulation
	 * (offset 0, multiplier 1) when either curve has fewer than 2 samples.
	 */
	static void EvalBakedCurves(
		const TArray<float>& HeightCurve,
		const TArray<float>& ScaleCurve,
		float Continentalness,
		float& OutHeightOffset,
		float& OutHeightScaleMultiplier);

	// ==================== Biome / material queries (instance wrappers) ====================

	/** Blended biome selection for smooth transitions. See UVoxelBiomeConfiguration::GetBiomeBlend. */
	FBiomeBlend GetBiomeBlend(float Temperature, float Moisture, float Continentalness = 0.0f) const
	{
		return ComputeBiomeBlend(Biomes, BiomeBlendWidth, Temperature, Moisture, Continentalness);
	}

	/** Material from a biome blend (dithered weighted selection). See UVoxelBiomeConfiguration::GetBlendedMaterial. */
	uint8 GetBlendedMaterial(const FBiomeBlend& Blend, float DepthBelowSurface) const
	{
		return SelectBlendedMaterial(Biomes, Blend, DepthBelowSurface);
	}

	/** Material from a biome blend with underwater handling. See UVoxelBiomeConfiguration::GetBlendedMaterialWithWater. */
	uint8 GetBlendedMaterialWithWater(const FBiomeBlend& Blend, float DepthBelowSurface,
		float TerrainSurfaceHeight, float WaterLevel) const
	{
		return SelectBlendedMaterialWithWater(Biomes, Blend, DepthBelowSurface,
			TerrainSurfaceHeight, WaterLevel, bEnableUnderwaterMaterials, DefaultUnderwaterMaterial);
	}

	/** Height-rule material override (first applicable wins, priority order). See UVoxelBiomeConfiguration::ApplyHeightMaterialRules. */
	uint8 ApplyHeightMaterialRules(uint8 CurrentMaterial, float WorldHeight, float DepthBelowSurface) const
	{
		return (bEnableHeightMaterials && SortedHeightRules.Num() > 0)
			? ApplyHeightRules(SortedHeightRules, CurrentMaterial, WorldHeight, DepthBelowSurface)
			: CurrentMaterial;
	}

	// ==================== Shared algorithm cores (statics — the single implementations) ====================

	/** Find a biome definition by ID (linear scan — biome counts are single-digit in practice). */
	static const FBiomeDefinition* FindBiome(const TArray<FBiomeDefinition>& InBiomes, uint8 BiomeID);

	/** Tiered biome blend: continentalness soft gate × temperature/moisture smoothstep × priority boost. */
	static FBiomeBlend ComputeBiomeBlend(
		const TArray<FBiomeDefinition>& InBiomes, float InBiomeBlendWidth,
		float Temperature, float Moisture, float Continentalness);

	/** Dithered weighted material selection from a blend. */
	static uint8 SelectBlendedMaterial(
		const TArray<FBiomeDefinition>& InBiomes, const FBiomeBlend& Blend, float DepthBelowSurface);

	/** Dithered weighted material selection with underwater material handling. */
	static uint8 SelectBlendedMaterialWithWater(
		const TArray<FBiomeDefinition>& InBiomes, const FBiomeBlend& Blend, float DepthBelowSurface,
		float TerrainSurfaceHeight, float WaterLevel,
		bool bInEnableUnderwaterMaterials, uint8 InDefaultUnderwaterMaterial);

	/** Height-rule override. Rules MUST be pre-sorted by priority descending (first applicable wins). */
	static uint8 ApplyHeightRules(
		const TArray<FHeightMaterialRule>& InSortedRules, uint8 CurrentMaterial,
		float WorldHeight, float DepthBelowSurface);
};
