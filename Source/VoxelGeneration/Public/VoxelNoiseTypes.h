// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"
#include "VoxelNoiseTypes.generated.h"

class UVoxelBiomeConfiguration;

// Forward declaration for island falloff type
enum class EIslandFalloffType : uint8;

/**
 * Spherical planet parameters for generation request.
 * Lightweight copy of FSphericalPlanetParams for passing through the pipeline.
 */
struct FSphericalPlanetModeParams
{
	/** Radius of the planet surface in world units */
	float PlanetRadius = 100000.0f;

	/** Maximum terrain height above PlanetRadius */
	float MaxTerrainHeight = 5000.0f;

	/** Maximum terrain depth below PlanetRadius */
	float MaxTerrainDepth = 2000.0f;

	/** Center of the planet in world space (typically WorldOrigin) */
	FVector PlanetCenter = FVector::ZeroVector;

	FSphericalPlanetModeParams() = default;

	/** Get the inner shell radius */
	float GetInnerRadius() const { return PlanetRadius - MaxTerrainDepth; }

	/** Get the outer shell radius */
	float GetOuterRadius() const { return PlanetRadius + MaxTerrainHeight; }
};

/**
 * Island mode parameters for generation request.
 * Lightweight copy of FIslandBowlParams for passing through the pipeline.
 */
struct FIslandModeParams
{
	/** Shape type: 0 = Circular, 1 = Rectangle */
	uint8 Shape = 0;

	/** Radius/SizeX of the island in world units (distance from center to edge start) */
	float IslandRadius = 50000.0f;

	/** Size Y of the island (only used for Rectangle shape) */
	float SizeY = 50000.0f;

	/** Width of the falloff zone where terrain fades to nothing */
	float FalloffWidth = 10000.0f;

	/** Type of falloff curve to use (cast from EIslandFalloffType) */
	uint8 FalloffType = 1;  // Default: Smooth

	/** Center of the island in world X coordinate (relative to WorldOrigin) */
	float CenterX = 0.0f;

	/** Center of the island in world Y coordinate (relative to WorldOrigin) */
	float CenterY = 0.0f;

	/** Minimum terrain height at island edges (can be negative for bowl effect) */
	float EdgeHeight = -1000.0f;

	/** Whether to create a bowl (lowered edges) or plateau (raised center) */
	bool bBowlShape = false;

	FIslandModeParams() = default;

	/** Get the total island extent in X (radius/sizeX + falloff) */
	float GetTotalExtentX() const
	{
		return IslandRadius + FalloffWidth;
	}

	/** Get the total island extent in Y (sizeY + falloff, or same as X for circular) */
	float GetTotalExtentY() const
	{
		return (Shape == 1 ? SizeY : IslandRadius) + FalloffWidth;
	}

	/** Get the maximum total extent (for compatibility) */
	float GetTotalExtent() const
	{
		return FMath::Max(GetTotalExtentX(), GetTotalExtentY());
	}

	/** Check if a point is within island bounds */
	bool IsWithinBounds(float X, float Y) const
	{
		const float DX = FMath::Abs(X - CenterX);
		const float DY = FMath::Abs(Y - CenterY);

		if (Shape == 1)  // Rectangle
		{
			return DX <= GetTotalExtentX() && DY <= GetTotalExtentY();
		}
		else  // Circular
		{
			const float Distance = FMath::Sqrt(DX * DX + DY * DY);
			return Distance <= GetTotalExtent();
		}
	}
};

// Re-export EVoxelNoiseType and FVoxelNoiseParams from VoxelCoreTypes.h
// These are defined in VoxelCore to avoid circular dependencies with VoxelWorldConfiguration

/**
 * Request for generating voxel data for a chunk.
 */
USTRUCT(BlueprintType)
struct VOXELGENERATION_API FVoxelNoiseGenerationRequest
{
	GENERATED_BODY()

	/** Chunk coordinate in chunk space */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** LOD level for this chunk (0 = highest detail) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	int32 LODLevel = 0;

	/** Number of voxels per chunk edge */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	int32 ChunkSize = 32;

	/** Size of each voxel in world units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	float VoxelSize = 100.0f;

	/** Noise parameters for generation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request")
	FVoxelNoiseParams NoiseParams;

	// ==================== World Mode Parameters ====================

	/** World generation mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	EWorldMode WorldMode = EWorldMode::InfinitePlane;

	/** Sea level height for terrain generation (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	float SeaLevel = 0.0f;

	/** Scale factor for noise-to-height conversion */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	float HeightScale = 5000.0f;

	/** Base height offset from sea level */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	float BaseHeight = 0.0f;

	// ==================== Biome Parameters ====================

	/** Whether to enable biome-based material selection */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	bool bEnableBiomes = true;

	/**
	 * Biome configuration containing biome definitions, blending parameters,
	 * and height material rules. Set by VoxelChunkManager from the world config.
	 * Not a UPROPERTY - kept alive by VoxelWorldConfiguration.
	 */
	UVoxelBiomeConfiguration* BiomeConfiguration = nullptr;

	// ==================== World Origin ====================

	/** World origin offset - all chunk positions are relative to this */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Mode")
	FVector WorldOrigin = FVector::ZeroVector;

	// ==================== Island Mode Parameters ====================

	/** Island mode configuration (used when WorldMode == IslandBowl) */
	FIslandModeParams IslandParams;

	// ==================== Spherical Planet Mode Parameters ====================

	/** Spherical planet configuration (used when WorldMode == SphericalPlanet) */
	FSphericalPlanetModeParams SphericalPlanetParams;

	FVoxelNoiseGenerationRequest() = default;

	/** Get the world position of this chunk's origin (includes WorldOrigin offset) */
	FVector GetChunkWorldPosition() const
	{
		// All chunks cover the same world area regardless of LOD level
		// LOD only affects voxel resolution within the chunk, not chunk position
		float ChunkWorldSize = ChunkSize * VoxelSize;
		return WorldOrigin + FVector(ChunkCoord) * ChunkWorldSize;
	}

	/** Get the effective voxel size at this LOD level */
	float GetEffectiveVoxelSize() const
	{
		return VoxelSize * FMath::Pow(2.0f, static_cast<float>(LODLevel));
	}
};

/**
 * Handle for tracking async generation operations.
 */
struct VOXELGENERATION_API FVoxelGenerationHandle
{
	/** Unique identifier for this generation request */
	uint64 RequestId = 0;

	/** Whether generation has completed */
	bool bIsComplete = false;

	/** Whether generation succeeded */
	bool bWasSuccessful = false;

	/** Error message if generation failed */
	FString ErrorMessage;

	FVoxelGenerationHandle() = default;
	explicit FVoxelGenerationHandle(uint64 InRequestId) : RequestId(InRequestId) {}

	bool IsValid() const { return RequestId != 0; }
	bool IsComplete() const { return bIsComplete; }
	bool WasSuccessful() const { return bIsComplete && bWasSuccessful; }
};

/** Delegate called when async generation completes */
DECLARE_DELEGATE_TwoParams(FOnVoxelGenerationComplete, FVoxelGenerationHandle /*Handle*/, bool /*bSuccess*/);
