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
	// Calculate distance from island center (2D)
	const float Distance = CalculateDistanceFromCenter(
		WorldPos.X, WorldPos.Y,
		IslandParams.CenterX, IslandParams.CenterY);

	// If completely outside island bounds, return air
	if (Distance > IslandParams.GetTotalExtent())
	{
		// Return a large negative value (definitely air)
		return -1000.0f;
	}

	// Calculate falloff factor
	const float FalloffFactor = CalculateFalloffFactor(
		Distance,
		IslandParams.IslandRadius,
		IslandParams.FalloffWidth,
		IslandParams.FalloffType);

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
	// Calculate distance from island center
	const float Distance = CalculateDistanceFromCenter(
		X, Y,
		IslandParams.CenterX, IslandParams.CenterY);

	// If outside island bounds, return edge height (or very low for air)
	if (Distance > IslandParams.GetTotalExtent())
	{
		return IslandParams.EdgeHeight - 1000.0f; // Below any reasonable terrain
	}

	// Calculate falloff factor
	const float FalloffFactor = CalculateFalloffFactor(
		Distance,
		IslandParams.IslandRadius,
		IslandParams.FalloffWidth,
		IslandParams.FalloffType);

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
	const float Distance = CalculateDistanceFromCenter(
		X, Y,
		IslandParams.CenterX, IslandParams.CenterY);
	return Distance <= IslandParams.GetTotalExtent();
}
