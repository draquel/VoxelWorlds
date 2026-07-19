// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UVoxelBiomeConfiguration;

/**
 * Plain-value, UObject-free snapshot of the biome-configuration data the terrain HEIGHT math needs
 * (continentalness modulation: noise field parameters + baked modulation curves).
 *
 * Captured once on the game thread via FromConfig() and then copyable into worker threads or
 * long-lived async tasks that may outlive the world (e.g. VoxelMap tile generation), where holding
 * the UVoxelBiomeConfiguration* is unsafe — the UObject can be GC'd under a running task.
 *
 * World modes store this by value (see IVoxelWorldMode::SetBiomeContext), which makes the analytic
 * GetTerrainHeightAt query safe to share into background tasks while still applying the exact same
 * height modulation as chunk generation.
 *
 * Phase 2 of the map-accuracy plan extends this with the material/biome-selection data
 * (see Documentation/Research/MAP_ACCURACY_PLAN.md).
 */
struct VOXELCORE_API FVoxelBiomeSnapshot
{
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

	/**
	 * Build a snapshot from a configuration. Null (or a config with continentalness disabled)
	 * yields the default snapshot, which is a no-op for height modulation.
	 * Game-thread only (reads the UObject); the returned value is thread-safe to copy and share.
	 */
	static FVoxelBiomeSnapshot FromConfig(const UVoxelBiomeConfiguration* Config);

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
};
