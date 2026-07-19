// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelBiomeSnapshot.h"
#include "VoxelBiomeConfiguration.h"

FVoxelBiomeSnapshot FVoxelBiomeSnapshot::FromConfig(const UVoxelBiomeConfiguration* Config)
{
	FVoxelBiomeSnapshot Snapshot;
	if (Config)
	{
		Snapshot.bEnableContinentalness = Config->bEnableContinentalness;
		Snapshot.ContinentalnessSeedOffset = Config->ContinentalnessSeedOffset;
		Snapshot.ContinentalnessNoiseFrequency = Config->ContinentalnessNoiseFrequency;
		Snapshot.BakedHeightCurve = Config->BakedHeightCurve;
		Snapshot.BakedHeightScaleCurve = Config->BakedHeightScaleCurve;
	}
	return Snapshot;
}

void FVoxelBiomeSnapshot::EvalBakedCurves(
	const TArray<float>& HeightCurve,
	const TArray<float>& ScaleCurve,
	float Continentalness,
	float& OutHeightOffset,
	float& OutHeightScaleMultiplier)
{
	// Identity modulation when the baked arrays are absent/degenerate (matches the former
	// UVoxelBiomeConfiguration::GetContinentalnessTerrainParams fallback).
	const int32 N = FMath::Min(HeightCurve.Num(), ScaleCurve.Num());
	if (N < 2)
	{
		OutHeightOffset = 0.0f;
		OutHeightScaleMultiplier = 1.0f;
		return;
	}

	// Map continentalness [-1,1] to float index [0, N-1], lerp between adjacent samples.
	// Deliberately no clamp on the input — bit-identical to the original generator math (an
	// out-of-range value extrapolates off the end samples exactly as before).
	const float FIdx = (Continentalness + 1.0f) * 0.5f * static_cast<float>(N - 1);
	const int32 Idx0 = FMath::Clamp(FMath::FloorToInt(FIdx), 0, N - 2);
	const float Frac = FIdx - static_cast<float>(Idx0);

	OutHeightOffset = FMath::Lerp(HeightCurve[Idx0], HeightCurve[Idx0 + 1], Frac);
	OutHeightScaleMultiplier = FMath::Lerp(ScaleCurve[Idx0], ScaleCurve[Idx0 + 1], Frac);
}
