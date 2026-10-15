// Copyright Daniel Raquel. All Rights Reserved.

#include "IslandBowlWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelMaterialRegistry.h"

FIslandBowlWorldMode::FIslandBowlWorldMode()
	: TerrainParams()
	, IslandParams()
{
}

FIslandBowlWorldMode::FIslandBowlWorldMode(
	const FWorldModeTerrainParams& InTerrainParams,
	const FIslandBowlParams& InIslandParams)
	: TerrainParams(InTerrainParams)
	, IslandParams(InIslandParams)
{
}

float FIslandBowlWorldMode::GetDensityAt(
	const FVector& WorldPos,
	int32 LODLevel,
	float NoiseValue) const
{
	// Base terrain height from noise (reusing InfinitePlane logic), then the island falloff, or the floor
	// beyond the edge. This used to call everything beyond the edge air, where generation writes the floor.
	const float BaseTerrainHeight = FInfinitePlaneWorldMode::NoiseToTerrainHeight(NoiseValue, TerrainParams);
	const float FinalTerrainHeight = ComposeTerrainHeight(WorldPos.X, WorldPos.Y, BaseTerrainHeight, IslandParams);

	// Calculate signed distance (positive = inside/solid)
	return FInfinitePlaneWorldMode::CalculateSignedDistance(WorldPos.Z, FinalTerrainHeight);
}

float FIslandBowlWorldMode::GetTerrainHeightAt(
	float X,
	float Y,
	const FVoxelNoiseParams& NoiseParams) const
{
	// Beyond the island's edge: the flat floor generation writes there (no noise needed).
	if (!IsWithinIslandBounds(X, Y, IslandParams))
	{
		return IslandParams.EdgeHeight - OutsideFloorDepth;
	}

	// Sample base terrain noise (reusing InfinitePlane method)
	const float NoiseValue = FInfinitePlaneWorldMode::SampleTerrainNoise2D(X, Y, NoiseParams);

	// Continentalness height modulation (internal oceans/mountains) — the SAME shared source of truth as
	// InfinitePlane; then the island edge falloff. The two compose: continentalness shapes the terrain
	// inside the island, the falloff fades it toward EdgeHeight at the world edge. Matches generation.
	float Continentalness = 0.0f;
	const FWorldModeTerrainParams EffectiveParams = FInfinitePlaneWorldMode::ComputeEffectiveTerrainParams(
		X, Y, TerrainParams, NoiseParams, &BiomeSnapshot, Continentalness);

	const float BaseTerrainHeight = FInfinitePlaneWorldMode::NoiseToTerrainHeight(NoiseValue, EffectiveParams);

	// Apply falloff and return
	return ComposeTerrainHeight(X, Y, BaseTerrainHeight, IslandParams);
}

void FIslandBowlWorldMode::GetTerrainHeightBounds(float& OutMin, float& OutMax) const
{
	// Base continentalness extent (same as InfinitePlane), then floor the minimum to EdgeHeight — the
	// island fades toward EdgeHeight at the edges, which can be below the base terrain minimum.
	float BaseMin = 0.0f;
	float BaseMax = 0.0f;
	FInfinitePlaneWorldMode::GetTerrainHeightBounds(TerrainParams, &BiomeSnapshot, BaseMin, BaseMax);
	OutMin = FMath::Min(BaseMin, IslandParams.EdgeHeight);
	OutMax = BaseMax;
}

FIntVector FIslandBowlWorldMode::WorldToChunkCoord(
	const FVector& WorldPos,
	int32 ChunkSize,
	float VoxelSize) const
{
	// Same conversion as InfinitePlane - Cartesian grid
	const float ChunkWorldSize = ChunkSize * VoxelSize;
	return FIntVector(
		FMath::FloorToInt(WorldPos.X / ChunkWorldSize),
		FMath::FloorToInt(WorldPos.Y / ChunkWorldSize),
		FMath::FloorToInt(WorldPos.Z / ChunkWorldSize));
}

FVector FIslandBowlWorldMode::ChunkCoordToWorld(
	const FIntVector& ChunkCoord,
	int32 ChunkSize,
	float VoxelSize,
	int32 LODLevel) const
{
	// Same conversion as InfinitePlane
	const float ChunkWorldSize = ChunkSize * VoxelSize;
	return FVector(ChunkCoord) * ChunkWorldSize;
}

int32 FIslandBowlWorldMode::GetMinZ() const
{
	return MIN_Z_CHUNKS;
}

int32 FIslandBowlWorldMode::GetMaxZ() const
{
	return MAX_Z_CHUNKS;
}

uint8 FIslandBowlWorldMode::GetMaterialAtDepth(
	const FVector& WorldPos,
	float SurfaceHeight,
	float DepthBelowSurface) const
{
	// Reuse the same material assignment as InfinitePlane
	// (Grass -> Dirt -> Stone based on depth)
	// The biome system will override this if enabled

	if (DepthBelowSurface < 0.0f)
	{
		// Above surface - air, no material
		return 0;
	}
	else if (DepthBelowSurface < 100.0f) // ~1 voxel at VoxelSize=100
	{
		return EVoxelMaterial::Grass;
	}
	else if (DepthBelowSurface < 400.0f) // ~4 voxels
	{
		return EVoxelMaterial::Dirt;
	}
	else
	{
		return EVoxelMaterial::Stone;
	}
}

// ==================== Static Helpers ====================

float FIslandBowlWorldMode::CalculateDistanceFromCenter(
	float X, float Y,
	float CenterX, float CenterY)
{
	const float DX = X - CenterX;
	const float DY = Y - CenterY;
	return FMath::Sqrt(DX * DX + DY * DY);
}

float FIslandBowlWorldMode::CalculateNormalizedDistance(
	float X, float Y,
	const FIslandBowlParams& IslandParams)
{
	const float DX = X - IslandParams.CenterX;
	const float DY = Y - IslandParams.CenterY;

	if (IslandParams.Shape == EIslandShape::Rectangle)
	{
		// For rectangle, use Chebyshev distance (max of normalized X and Y distances)
		// This creates a smooth rectangular falloff
		const float NormX = FMath::Abs(DX) / IslandParams.IslandRadius;
		const float NormY = FMath::Abs(DY) / IslandParams.SizeY;

		// Use the maximum of the two normalized distances
		// This gives us the "distance to the nearest edge" in normalized space
		return FMath::Max(NormX, NormY);
	}
	else
	{
		// For circular, use Euclidean distance normalized by radius
		return FMath::Sqrt(DX * DX + DY * DY) / IslandParams.IslandRadius;
	}
}

float FIslandBowlWorldMode::CalculateFalloffFactor(
	float Distance,
	float IslandRadius,
	float FalloffWidth,
	EIslandFalloffType FalloffType)
{
	// Inside the island radius - full terrain
	if (Distance <= IslandRadius)
	{
		return 1.0f;
	}

	// Beyond the falloff zone - no terrain
	if (Distance >= IslandRadius + FalloffWidth)
	{
		return 0.0f;
	}

	// In the falloff zone - calculate based on falloff type
	const float T = (Distance - IslandRadius) / FalloffWidth; // Normalized distance in falloff zone [0, 1]
	return ApplyFalloffCurve(T, FalloffType);
}

float FIslandBowlWorldMode::ApplyFalloffCurve(float T, EIslandFalloffType FalloffType)
{
	T = FMath::Clamp(T, 0.0f, 1.0f);
	const float InvT = 1.0f - T;

	switch (FalloffType)
	{
	case EIslandFalloffType::Linear:
		// Simple linear falloff
		return InvT;

	case EIslandFalloffType::Smooth:
		// Smooth hermite (smoothstep) falloff
		return InvT * InvT * (3.0f - 2.0f * InvT);

	case EIslandFalloffType::Squared:
		// Squared falloff (faster drop near edge)
		return InvT * InvT;

	case EIslandFalloffType::Exponential:
	{
		// e^(-3t), rescaled to run from 1 to exactly 0. Unscaled it ended at e^-3 (about 0.05), so the
		// terrain stopped 5% of its relief short of EdgeHeight and stepped down at the edge (KI4).
		const float End = FMath::Exp(-3.0f);
		return (FMath::Exp(-3.0f * T) - End) / (1.0f - End);
	}

	default:
		return InvT;
	}
}

float FIslandBowlWorldMode::CalculateFalloffFactorForPoint(
	float X, float Y,
	const FIslandBowlParams& IslandParams)
{
	if (IslandParams.Shape == EIslandShape::Rectangle)
	{
		// For rectangle, calculate distance to edge in each axis separately
		const float DX = FMath::Abs(X - IslandParams.CenterX);
		const float DY = FMath::Abs(Y - IslandParams.CenterY);

		// Calculate how far into the falloff zone we are for each axis
		const float FalloffStartX = IslandParams.IslandRadius;
		const float FalloffStartY = IslandParams.SizeY;
		const float FalloffEndX = FalloffStartX + IslandParams.FalloffWidth;
		const float FalloffEndY = FalloffStartY + IslandParams.FalloffWidth;

		// Calculate T values for each axis (0 = at edge, 1 = at falloff end)
		float TX = 0.0f;
		if (DX > FalloffStartX)
		{
			TX = FMath::Clamp((DX - FalloffStartX) / IslandParams.FalloffWidth, 0.0f, 1.0f);
		}

		float TY = 0.0f;
		if (DY > FalloffStartY)
		{
			TY = FMath::Clamp((DY - FalloffStartY) / IslandParams.FalloffWidth, 0.0f, 1.0f);
		}

		// Use the maximum T value (the axis that's furthest into falloff determines the factor)
		const float T = FMath::Max(TX, TY);

		// If we're inside the rectangle (not in falloff), return 1
		if (T <= 0.0f)
		{
			return 1.0f;
		}

		// Apply the falloff curve
		return ApplyFalloffCurve(T, IslandParams.FalloffType);
	}
	else
	{
		// Circular: use standard distance-based calculation
		const float Distance = CalculateDistanceFromCenter(
			X, Y,
			IslandParams.CenterX, IslandParams.CenterY);

		return CalculateFalloffFactor(
			Distance,
			IslandParams.IslandRadius,
			IslandParams.FalloffWidth,
			IslandParams.FalloffType);
	}
}

float FIslandBowlWorldMode::ComposeTerrainHeight(
	float X, float Y,
	float BaseHeight,
	const FIslandBowlParams& IslandParams)
{
	if (!IsWithinIslandBounds(X, Y, IslandParams))
	{
		return IslandParams.EdgeHeight - OutsideFloorDepth;
	}
	return ApplyFalloffToHeight(BaseHeight, CalculateFalloffFactorForPoint(X, Y, IslandParams), IslandParams.EdgeHeight);
}

float FIslandBowlWorldMode::ApplyFalloffToHeight(
	float BaseHeight,
	float FalloffFactor,
	float EdgeHeight)
{
	// Lerp between base terrain height and edge height based on falloff
	return FMath::Lerp(EdgeHeight, BaseHeight, FalloffFactor);
}

bool FIslandBowlWorldMode::IsWithinIslandBounds(
	float X, float Y,
	const FIslandBowlParams& IslandParams)
{
	const float DX = FMath::Abs(X - IslandParams.CenterX);
	const float DY = FMath::Abs(Y - IslandParams.CenterY);

	if (IslandParams.Shape == EIslandShape::Rectangle)
	{
		// Rectangle bounds check
		return DX <= IslandParams.GetTotalExtentX() &&
		       DY <= IslandParams.GetTotalExtentY();
	}
	else
	{
		// Circular bounds check
		const float Distance = FMath::Sqrt(DX * DX + DY * DY);
		return Distance <= IslandParams.GetTotalExtent();
	}
}
