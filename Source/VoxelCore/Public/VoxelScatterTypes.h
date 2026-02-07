// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelMaterialAtlas.h" // For EVoxelFaceType
#include "VoxelScatterTypes.generated.h"

class UStaticMesh;
class UMaterialInterface;

/**
 * A single extracted surface point from mesh data.
 * Used as input for scatter placement decisions.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FVoxelSurfacePoint
{
	GENERATED_BODY()

	/** World-space position on surface */
	UPROPERTY(BlueprintReadOnly)
	FVector Position = FVector::ZeroVector;

	/** Surface normal (normalized) */
	UPROPERTY(BlueprintReadOnly)
	FVector Normal = FVector::UpVector;

	/** Material ID at this point */
	UPROPERTY(BlueprintReadOnly)
	uint8 MaterialID = 0;

	/** Biome ID at this point */
	UPROPERTY(BlueprintReadOnly)
	uint8 BiomeID = 0;

	/** Face type: Top, Side, or Bottom */
	UPROPERTY(BlueprintReadOnly)
	EVoxelFaceType FaceType = EVoxelFaceType::Top;

	/** Ambient occlusion value (0-3) */
	UPROPERTY(BlueprintReadOnly)
	uint8 AmbientOcclusion = 0;

	FVoxelSurfacePoint() = default;

	FVoxelSurfacePoint(const FVector& InPos, const FVector& InNormal, uint8 InMaterial, uint8 InBiome, EVoxelFaceType InFaceType)
		: Position(InPos)
		, Normal(InNormal)
		, MaterialID(InMaterial)
		, BiomeID(InBiome)
		, FaceType(InFaceType)
	{
	}

	/** Get slope angle in degrees (0 = flat horizontal, 90 = vertical) */
	float GetSlopeAngle() const
	{
		// Dot product with up vector gives cos(angle)
		const float CosAngle = FMath::Clamp(FVector::DotProduct(Normal, FVector::UpVector), -1.0f, 1.0f);
		return FMath::RadiansToDegrees(FMath::Acos(CosAngle));
	}

	/** Is this a horizontal surface? (slope less than threshold) */
	bool IsHorizontal(float MaxSlopeDegrees = 30.0f) const
	{
		return GetSlopeAngle() <= MaxSlopeDegrees;
	}

	/** Is this a vertical surface? (slope greater than threshold) */
	bool IsVertical(float MinSlopeDegrees = 60.0f) const
	{
		return GetSlopeAngle() >= MinSlopeDegrees;
	}
};

/**
 * Per-chunk cache of extracted surface points.
 * Generated once after meshing, reused for all scatter types.
 */
USTRUCT()
struct VOXELCORE_API FChunkSurfaceData
{
	GENERATED_BODY()

	/** Chunk coordinate */
	UPROPERTY()
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** All extracted surface points (downsampled from mesh) */
	TArray<FVoxelSurfacePoint> SurfacePoints;

	/** LOD level this was extracted from */
	UPROPERTY()
	int32 LODLevel = 0;

	/** Whether data is valid */
	bool bIsValid = false;

	/** Estimated surface area in square units (for density calculations) */
	float SurfaceAreaEstimate = 0.0f;

	/** Average spacing between sample points */
	float AveragePointSpacing = 100.0f;

	FChunkSurfaceData() = default;

	explicit FChunkSurfaceData(const FIntVector& InChunkCoord)
		: ChunkCoord(InChunkCoord)
	{
	}

	/** Get approximate memory usage */
	SIZE_T GetAllocatedSize() const
	{
		return SurfacePoints.GetAllocatedSize();
	}

	/** Clear all data */
	void Reset()
	{
		SurfacePoints.Empty();
		bIsValid = false;
		SurfaceAreaEstimate = 0.0f;
	}
};

/**
 * A scatter spawn point - output of placement algorithm.
 * Contains position + metadata for instance spawning.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FScatterSpawnPoint
{
	GENERATED_BODY()

	/** World-space spawn position */
	UPROPERTY(BlueprintReadOnly)
	FVector Position = FVector::ZeroVector;

	/** Surface normal for alignment */
	UPROPERTY(BlueprintReadOnly)
	FVector Normal = FVector::UpVector;

	/** Scatter type ID (which definition spawned this) */
	UPROPERTY(BlueprintReadOnly)
	int32 ScatterTypeID = 0;

	/** Random seed for this instance (scale, rotation variation) */
	uint32 InstanceSeed = 0;

	/** Computed scale (from ScatterDefinition range) */
	UPROPERTY(BlueprintReadOnly)
	float Scale = 1.0f;

	/** Computed rotation yaw in degrees */
	UPROPERTY(BlueprintReadOnly)
	float RotationYaw = 0.0f;

	FScatterSpawnPoint() = default;

	FScatterSpawnPoint(const FVector& InPos, const FVector& InNormal, int32 InTypeID, uint32 InSeed)
		: Position(InPos)
		, Normal(InNormal)
		, ScatterTypeID(InTypeID)
		, InstanceSeed(InSeed)
	{
	}

	/** Get full rotation (including alignment to normal if needed) */
	FRotator GetRotation(bool bAlignToNormal) const
	{
		if (bAlignToNormal)
		{
			// Align Z axis to surface normal, rotate around it by yaw
			FRotator AlignedRotation = Normal.Rotation();
			AlignedRotation.Yaw += RotationYaw;
			return AlignedRotation;
		}
		else
		{
			return FRotator(0.0f, RotationYaw, 0.0f);
		}
	}

	/** Get transform for instance spawning */
	FTransform GetTransform(bool bAlignToNormal, float SurfaceOffset = 0.0f) const
	{
		FVector AdjustedPosition = Position + Normal * SurfaceOffset;
		FRotator Rotation = GetRotation(bAlignToNormal);
		return FTransform(Rotation, AdjustedPosition, FVector(Scale));
	}
};

/**
 * Defines a scatter type and its placement rules.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FScatterDefinition
{
	GENERATED_BODY()

	/** Unique ID for this scatter type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	int32 ScatterID = 0;

	/** Display name for debugging */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	FString Name = TEXT("Unnamed");

	/** Debug visualization color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FColor DebugColor = FColor::Green;

	/** Debug sphere radius for visualization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "1.0", ClampMax = "100.0"))
	float DebugSphereRadius = 10.0f;

	// ==================== Placement Rules ====================

	/** Enable this scatter type */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	bool bEnabled = true;

	/** Spawn probability per valid surface point (0.0-1.0, where 0.1 = 10% chance) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Density = 0.1f;

	/** Minimum slope angle in degrees (0 = flat horizontal) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MinSlopeDegrees = 0.0f;

	/** Maximum slope angle in degrees (90 = vertical) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MaxSlopeDegrees = 45.0f;

	/** Allowed material IDs (empty = all materials allowed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TArray<uint8> AllowedMaterials;

	/** Allowed biome IDs (empty = all biomes allowed) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TArray<uint8> AllowedBiomes;

	/** Minimum world Z height for placement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	float MinElevation = -1000000.0f;

	/** Maximum world Z height for placement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	float MaxElevation = 1000000.0f;

	/** Only place on top faces (FaceType == Top) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	bool bTopFacesOnly = true;

	/** Avoid placement in shadowed areas (high AO) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	bool bAvoidShadowedAreas = false;

	/** Maximum AO value for placement (0-3, only used if bAvoidShadowedAreas) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0", ClampMax = "3", EditCondition = "bAvoidShadowedAreas"))
	uint8 MaxAmbientOcclusion = 2;

	// ==================== Instance Variation ====================

	/** Scale range - random between X and Y */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation")
	FVector2D ScaleRange = FVector2D(0.8f, 1.2f);

	/** Apply random yaw rotation (0-360) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation")
	bool bRandomYawRotation = true;

	/** Align instance up vector to surface normal */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation")
	bool bAlignToSurfaceNormal = false;

	/** Offset from surface along normal (cm) - positive = away from surface */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation")
	float SurfaceOffset = 0.0f;

	/** Random position jitter within this radius (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation", meta = (ClampMin = "0.0"))
	float PositionJitter = 0.0f;

	// ==================== Mesh Settings ====================

	/** Static mesh to instance (nullptr = debug visualization only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Override materials for the mesh (empty = use mesh defaults) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	TArray<TSoftObjectPtr<UMaterialInterface>> OverrideMaterials;

	/** Enable collision for mesh instances */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	bool bEnableCollision = false;

	/** Cast shadows from mesh instances */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	bool bCastShadows = true;

	/** Receives decals on mesh instances */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	bool bReceivesDecals = true;

	// ==================== LOD & Culling ====================

	/**
	 * Maximum distance for spawning this scatter type.
	 * 0 = use global ScatterRadius from VoxelWorldConfiguration.
	 * Set higher for trees to spawn them when chunks first load at distance, preventing pop-in.
	 * Scatter is generated when any LOD mesh becomes available for a chunk within this range.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0"))
	float SpawnDistance = 0.0f;

	/** Maximum distance for rendering (HISM cull distance) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	float CullDistance = 50000.0f;

	/** Start distance for LOD transitions */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	float LODStartDistance = 5000.0f;

	FScatterDefinition() = default;

	// ==================== Methods ====================

	/**
	 * Check if a surface point passes all placement rules.
	 *
	 * @param Point The surface point to evaluate
	 * @return True if this point is valid for scatter placement
	 */
	bool CanSpawnAt(const FVoxelSurfacePoint& Point) const
	{
		if (!bEnabled)
		{
			return false;
		}

		// Check slope
		const float SlopeAngle = Point.GetSlopeAngle();
		if (SlopeAngle < MinSlopeDegrees || SlopeAngle > MaxSlopeDegrees)
		{
			return false;
		}

		// Check elevation
		if (Point.Position.Z < MinElevation || Point.Position.Z > MaxElevation)
		{
			return false;
		}

		// Check face type
		if (bTopFacesOnly && Point.FaceType != EVoxelFaceType::Top)
		{
			return false;
		}

		// Check material filter
		if (AllowedMaterials.Num() > 0 && !AllowedMaterials.Contains(Point.MaterialID))
		{
			return false;
		}

		// Check biome filter
		if (AllowedBiomes.Num() > 0 && !AllowedBiomes.Contains(Point.BiomeID))
		{
			return false;
		}

		// Check ambient occlusion
		if (bAvoidShadowedAreas && Point.AmbientOcclusion > MaxAmbientOcclusion)
		{
			return false;
		}

		return true;
	}

	/**
	 * Get spawn probability (now just returns Density directly).
	 * Density is interpreted as direct probability (0-1).
	 *
	 * @return Probability (0-1) of spawning at a valid point
	 */
	float GetSpawnProbability() const
	{
		return FMath::Clamp(Density, 0.0f, 1.0f);
	}

	/**
	 * Compute instance scale from seed.
	 */
	float ComputeScale(float Random01) const
	{
		return FMath::Lerp(ScaleRange.X, ScaleRange.Y, Random01);
	}

	/**
	 * Compute instance rotation yaw from seed.
	 */
	float ComputeRotationYaw(float Random01) const
	{
		return bRandomYawRotation ? Random01 * 360.0f : 0.0f;
	}

	/**
	 * Compute position jitter offset.
	 */
	FVector ComputePositionJitter(float RandomX, float RandomY) const
	{
		if (PositionJitter <= 0.0f)
		{
			return FVector::ZeroVector;
		}

		// Random offset in XY plane
		return FVector(
			(RandomX * 2.0f - 1.0f) * PositionJitter,
			(RandomY * 2.0f - 1.0f) * PositionJitter,
			0.0f
		);
	}
};

/**
 * Per-chunk scatter result - spawn points for all scatter types.
 */
USTRUCT()
struct VOXELCORE_API FChunkScatterData
{
	GENERATED_BODY()

	/** Chunk coordinate */
	UPROPERTY()
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	/** All spawn points for this chunk */
	TArray<FScatterSpawnPoint> SpawnPoints;

	/** Whether data is valid */
	bool bIsValid = false;

	/** Seed used for generation (for verification) */
	uint32 GenerationSeed = 0;

	FChunkScatterData() = default;

	explicit FChunkScatterData(const FIntVector& InChunkCoord)
		: ChunkCoord(InChunkCoord)
	{
	}

	/** Get spawn points for a specific scatter type */
	void GetSpawnPointsForType(int32 ScatterTypeID, TArray<FScatterSpawnPoint>& OutPoints) const
	{
		for (const FScatterSpawnPoint& Point : SpawnPoints)
		{
			if (Point.ScatterTypeID == ScatterTypeID)
			{
				OutPoints.Add(Point);
			}
		}
	}

	/** Get count for a specific scatter type */
	int32 GetCountForType(int32 ScatterTypeID) const
	{
		int32 Count = 0;
		for (const FScatterSpawnPoint& Point : SpawnPoints)
		{
			if (Point.ScatterTypeID == ScatterTypeID)
			{
				++Count;
			}
		}
		return Count;
	}

	/** Get approximate memory usage */
	SIZE_T GetAllocatedSize() const
	{
		return SpawnPoints.GetAllocatedSize();
	}

	/** Clear all data */
	void Reset()
	{
		SpawnPoints.Empty();
		bIsValid = false;
	}
};

/**
 * Statistics for scatter system debugging.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FScatterStatistics
{
	GENERATED_BODY()

	/** Total chunks with scatter data */
	UPROPERTY(BlueprintReadOnly)
	int32 ChunksWithScatter = 0;

	/** Total surface points extracted */
	UPROPERTY(BlueprintReadOnly)
	int64 TotalSurfacePoints = 0;

	/** Total spawn points generated */
	UPROPERTY(BlueprintReadOnly)
	int64 TotalSpawnPoints = 0;

	/** Memory used for surface data (bytes) */
	UPROPERTY(BlueprintReadOnly)
	int64 SurfaceDataMemory = 0;

	/** Memory used for scatter data (bytes) */
	UPROPERTY(BlueprintReadOnly)
	int64 ScatterDataMemory = 0;

	/** Average surface points per chunk */
	float GetAverageSurfacePointsPerChunk() const
	{
		return ChunksWithScatter > 0 ? static_cast<float>(TotalSurfacePoints) / ChunksWithScatter : 0.0f;
	}

	/** Average spawn points per chunk */
	float GetAverageSpawnPointsPerChunk() const
	{
		return ChunksWithScatter > 0 ? static_cast<float>(TotalSpawnPoints) / ChunksWithScatter : 0.0f;
	}
};
