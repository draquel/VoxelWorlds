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
	// Check if completely outside island bounds
	if (!IsWithinIslandBounds(WorldPos.X, WorldPos.Y, IslandParams))
	{
		// Return a large negative value (definitely air)
		return -1000.0f;
	}

	// Calculate falloff factor (handles both circular and rectangle)
	const float FalloffFactor = CalculateFalloffFactorForPoint(
		WorldPos.X, WorldPos.Y,
		IslandParams);

	// Get base terrain height from noise (reusing InfinitePlane logic)
	const float BaseTerrainHeight = FInfinitePlaneWorldMode::NoiseToTerrainHeight(
		NoiseValue, TerrainParams);

	// Apply falloff to terrain height
	const float FinalTerrainHeight = ApplyFalloffToHeight(
		BaseTerrainHeight,
		FalloffFactor,
		IslandParams.EdgeHeight);

	// Calculate signed distance (positive = inside/solid)
	return FInfinitePlaneWorldMode::CalculateSignedDistance(WorldPos.Z, FinalTerrainHeight);
}

float FIslandBowlWorldMode::GetTerrainHeightAt(
	float X,
	float Y,
	const FVoxelNoiseParams& NoiseParams) const
{
	// If outside island bounds, return edge height (or very low for air)
	if (!IsWithinIslandBounds(X, Y, IslandParams))
	{
		return IslandParams.EdgeHeight - 1000.0f; // Below any reasonable terrain
	}

	// Calculate falloff factor (handles both circular and rectangle)
	const float FalloffFactor = CalculateFalloffFactorForPoint(X, Y, IslandParams);

	// Sample base terrain noise (reusing InfinitePlane method)
	const float NoiseValue = FInfinitePlaneWorldMode::SampleTerrainNoise2D(X, Y, NoiseParams);

	// Get base terrain height
	const float BaseTerrainHeight = FInfinitePlaneWorldMode::NoiseToTerrainHeight(
		NoiseValue, TerrainParams);

	// Apply falloff and return
	return ApplyFalloffToHeight(BaseTerrainHeight, FalloffFactor, IslandParams.EdgeHeight);
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

	switch (FalloffType)
	{
	case EIslandFalloffType::Linear:
		// Simple linear falloff
		return 1.0f - T;

	case EIslandFalloffType::Smooth:
		// Smooth hermite (smoothstep) falloff
		{
			const float InvT = 1.0f - T;
			return InvT * InvT * (3.0f - 2.0f * InvT);
		}

	case EIslandFalloffType::Squared:
		// Squared falloff (faster drop near edge)
		{
			const float InvT = 1.0f - T;
			return InvT * InvT;
		}

	case EIslandFalloffType::Exponential:
		// Exponential falloff (gradual then sharp)
		return FMath::Exp(-T * 3.0f); // e^(-3t) gives nice falloff curve

	default:
		return 1.0f - T;
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
		switch (IslandParams.FalloffType)
		{
		case EIslandFalloffType::Linear:
			return 1.0f - T;

		case EIslandFalloffType::Smooth:
			{
				const float InvT = 1.0f - T;
				return InvT * InvT * (3.0f - 2.0f * InvT);
			}

		case EIslandFalloffType::Squared:
			{
				const float InvT = 1.0f - T;
				return InvT * InvT;
			}

		case EIslandFalloffType::Exponential:
			return FMath::Exp(-T * 3.0f);

		default:
			return 1.0f - T;
		}
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
