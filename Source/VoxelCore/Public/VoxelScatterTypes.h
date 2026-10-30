// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h" // For EScatterMeshType, EScatterPlacementMode
#include "VoxelMaterialAtlas.h" // For EVoxelFaceType
#include "VoxelScatterTypes.generated.h"

class UStaticMesh;
class UTexture2D;
class UMaterialInterface;

/**
 * Which surface categories a scatter type may spawn on, as an independent bitmask.
 *
 * Every surface point falls into exactly one category, chosen by precedence
 * Underground > Underwater > Surface (see FVoxelSurfacePoint classification). A
 * definition opts into any combination of these, so e.g. a rock can be allowed on
 * both dry land and underwater, or a mushroom restricted to underground only.
 *
 * Underwater/Underground classification depends on the water/cave systems: when the
 * world has no water level, no point is ever Underwater, so those flags are simply inert.
 */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EScatterSurfaceLocationFlags : uint8
{
	/** No categories (required 0-entry for default-initialized bitmask properties) */
	None        = 0 UMETA(Hidden),
	/** Dry land: open-sky surface at or above the water level */
	Surface     = 0x01 UMETA(DisplayName = "Surface (dry land)"),
	/** Submerged: open-sky surface below the water level (seabeds, lakebeds) */
	Underwater  = 0x02 UMETA(DisplayName = "Underwater"),
	/** Underground: cave floors and enclosed voids */
	Underground = 0x04 UMETA(DisplayName = "Underground")
};
ENUM_CLASS_FLAGS(EScatterSurfaceLocationFlags);

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

	/** Whether this surface point is underground (cave interior, below terrain) */
	bool bIsUnderground = false;

	/** Whether this surface point is submerged (open-sky surface below the water level) */
	bool bIsUnderwater = false;

	/** Pre-computed slope angle in degrees (0 = flat, 90 = vertical). Computed during surface extraction. */
	float SlopeAngle = -1.0f;

	/** Local up at the point: +Z in heightmap modes, the radial direction on a planet (spherical SP-G1) */
	FVector LocalUp = FVector::UpVector;

	/** On a planet: the point's altitude above the datum, which the elevation filter reads instead of world Z */
	float Altitude = 0.0f;
	bool bHasAltitude = false;

	/**
	 * Share of the definition's density this point carries (1 for the column scan). A planet's extraction lines run
	 * along local up, spaced across the axis nearest it, so they meet level ground more densely by 1 / cos of the angle
	 * between the two; this weight (that cosine) brings the density per area back to the column scan's.
	 */
	float SpawnWeight = 1.0f;

	FVoxelSurfacePoint() = default;

	FVoxelSurfacePoint(const FVector& InPos, const FVector& InNormal, uint8 InMaterial, uint8 InBiome, EVoxelFaceType InFaceType)
		: Position(InPos)
		, Normal(InNormal)
		, MaterialID(InMaterial)
		, BiomeID(InBiome)
		, FaceType(InFaceType)
	{
		ComputeSlopeAngle();
	}

	/** Compute and cache the slope angle from the normal */
	void ComputeSlopeAngle()
	{
		const float CosAngle = FMath::Clamp(FVector::DotProduct(Normal, FVector::UpVector), -1.0f, 1.0f);
		SlopeAngle = FMath::RadiansToDegrees(FMath::Acos(CosAngle));
	}

	/** Compute and cache the slope angle against a local up (a planet's radial direction, spherical SP-G1) */
	void ComputeSlopeAngle(const FVector& Up)
	{
		const float CosAngle = FMath::Clamp(static_cast<float>(FVector::DotProduct(Normal, Up)), -1.0f, 1.0f);
		SlopeAngle = FMath::RadiansToDegrees(FMath::Acos(CosAngle));
	}

	/** Get slope angle in degrees (0 = flat horizontal, 90 = vertical) */
	float GetSlopeAngle() const
	{
		if (SlopeAngle >= 0.0f)
		{
			return SlopeAngle;
		}
		// Fallback: compute on demand if not pre-computed
		const float CosAngle = FMath::Clamp(FVector::DotProduct(Normal, FVector::UpVector), -1.0f, 1.0f);
		return FMath::RadiansToDegrees(FMath::Acos(CosAngle));
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

	/** Local up at the surface point: the instance stands along it when not aligned to the normal (spherical SP-G1) */
	UPROPERTY(BlueprintReadOnly)
	FVector LocalUp = FVector::UpVector;

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
			// Rotate the object's Z-axis (up) to align with the surface normal,
			// then apply yaw rotation around that new up axis
			const FQuat AlignQuat = FQuat::FindBetweenNormals(FVector::UpVector, Normal);
			const FQuat YawQuat(Normal, FMath::DegreesToRadians(RotationYaw));
			return (YawQuat * AlignQuat).Rotator();
		}
		else if (LocalUp == FVector::UpVector)
		{
			return FRotator(0.0f, RotationYaw, 0.0f);
		}
		else
		{
			return GetRotationQuat(bAlignToNormal).Rotator();
		}
	}

	/**
	 * The rotation as a quaternion. On a planet (LocalUp not +Z, spherical SP-G1) the instance's Z axis goes to local up
	 * (or the normal when aligned) and the yaw turns about it, without the gimbal-prone round trip through FRotator; the
	 * near-antiparallel case (local up near -Z, the south pole) turns +Z over first so the alignment stays exact.
	 * Heightmap points: GetRotation's quaternion.
	 */
	FQuat GetRotationQuat(bool bAlignToNormal) const
	{
		if (LocalUp == FVector::UpVector)
		{
			return GetRotation(bAlignToNormal).Quaternion();
		}
		const FVector Up = bAlignToNormal ? Normal : LocalUp;
		const FQuat AlignQuat = FVector::DotProduct(FVector::UpVector, Up) < -0.99
			? FQuat::FindBetweenNormals(FVector::DownVector, Up) * FQuat(FVector::ForwardVector, UE_DOUBLE_PI)
			: FQuat::FindBetweenNormals(FVector::UpVector, Up);
		return FQuat(Up, FMath::DegreesToRadians(RotationYaw)) * AlignQuat;
	}

	/** Get transform for instance spawning */
	FTransform GetTransform(bool bAlignToNormal, float SurfaceOffset = 0.0f) const
	{
		FVector AdjustedPosition = Position + Normal * SurfaceOffset;
		if (LocalUp != FVector::UpVector)
		{
			return FTransform(GetRotationQuat(bAlignToNormal), AdjustedPosition, FVector(Scale));
		}
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

	/**
	 * Gameplay harvest category ("Tree", "Bush", "Rock"...). None = not harvestable. Consumers ask
	 * UVoxelScatterManager::HarvestScatterNear for the nearest harvestable instance; what the
	 * category yields is the game's business.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	FName HarvestCategory;

	/** Debug visualization color */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FColor DebugColor = FColor::Green;

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

	/** Minimum world Z height for placement (on a planet: the altitude above the datum, spherical SP-G1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	float MinElevation = -1000000.0f;

	/** Maximum world Z height for placement (on a planet: the altitude above the datum) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	float MaxElevation = 1000000.0f;

	/** Only place on top faces (FaceType == Top) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	bool bTopFacesOnly = true;

	/**
	 * Which surface categories this scatter type may spawn on — independent toggles
	 * for Surface (dry land), Underwater, and Underground. Enable any combination.
	 * Defaults to dry-land surface only (matches the pre-bitmask "Surface Only" behavior).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (Bitmask, BitmaskEnum = "/Script/VoxelCore.EScatterSurfaceLocationFlags"))
	int32 SurfaceLocationMask = static_cast<int32>(EScatterSurfaceLocationFlags::Surface);

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

	// ==================== Cubic Scatter Settings ====================

	/** How this scatter's mesh is rendered (StaticMesh, CrossBillboard, VoxelInjection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter")
	EScatterMeshType MeshType = EScatterMeshType::StaticMesh;

	/** How positions are determined (SurfaceInterpolated for smooth, BlockFaceSnap for cubic) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter")
	EScatterPlacementMode PlacementMode = EScatterPlacementMode::SurfaceInterpolated;

	// ==================== Billboard Settings ====================

	/** Texture for cross-billboard (only used when MeshType == CrossBillboard and bUseBillboardAtlas is false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard", meta = (EditCondition = "MeshType == EScatterMeshType::CrossBillboard"))
	TSoftObjectPtr<UTexture2D> BillboardTexture;

	/** Width of billboard quad in cm */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard", meta = (ClampMin = "1.0", EditCondition = "MeshType == EScatterMeshType::CrossBillboard"))
	float BillboardWidth = 100.0f;

	/** Height of billboard quad in cm */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard", meta = (ClampMin = "1.0", EditCondition = "MeshType == EScatterMeshType::CrossBillboard"))
	float BillboardHeight = 100.0f;

	// ==================== Billboard Atlas Settings ====================

	/** Use a texture atlas tile instead of a standalone BillboardTexture */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard Atlas", meta = (EditCondition = "MeshType == EScatterMeshType::CrossBillboard"))
	bool bUseBillboardAtlas = false;

	/** Atlas texture (shared across billboard types that use the same atlas) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard Atlas", meta = (EditCondition = "MeshType == EScatterMeshType::CrossBillboard && bUseBillboardAtlas"))
	TSoftObjectPtr<UTexture2D> BillboardAtlasTexture;

	/** Column of this billboard's tile in the atlas (0-based) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard Atlas", meta = (ClampMin = "0", EditCondition = "MeshType == EScatterMeshType::CrossBillboard && bUseBillboardAtlas"))
	int32 BillboardAtlasColumn = 0;

	/** Row of this billboard's tile in the atlas (0-based) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard Atlas", meta = (ClampMin = "0", EditCondition = "MeshType == EScatterMeshType::CrossBillboard && bUseBillboardAtlas"))
	int32 BillboardAtlasRow = 0;

	/** Number of columns in the billboard atlas grid */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard Atlas", meta = (ClampMin = "1", ClampMax = "16", EditCondition = "MeshType == EScatterMeshType::CrossBillboard && bUseBillboardAtlas"))
	int32 BillboardAtlasColumns = 4;

	/** Number of rows in the billboard atlas grid */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|Billboard Atlas", meta = (ClampMin = "1", ClampMax = "16", EditCondition = "MeshType == EScatterMeshType::CrossBillboard && bUseBillboardAtlas"))
	int32 BillboardAtlasRows = 4;

	// ==================== Voxel Injection Settings ====================

	/** Index into Configuration->TreeTemplates (only used when MeshType == VoxelInjection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cubic Scatter|VoxelInjection", meta = (ClampMin = "0", EditCondition = "MeshType == EScatterMeshType::VoxelInjection"))
	int32 TreeTemplateID = 0;

	// ==================== LOD & Culling ====================

	/**
	 * Maximum distance for spawning this scatter type.
	 * 0 = use global ScatterRadius from VoxelWorldConfiguration.
	 * Set higher for trees to spawn them when chunks first load at distance, preventing pop-in.
	 * Scatter is generated when any LOD mesh becomes available for a chunk within this range.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0"))
	float SpawnDistance = 0.0f;

	/** Maximum distance for rendering (HISM cull distance - instances beyond this are invisible) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "100.0"))
	float CullDistance = 50000.0f;

	/**
	 * Distance where LOD transitions begin (instances start using lower LOD meshes).
	 * Also controls shadow distance - shadows only cast from instances closer than this.
	 * Should be less than CullDistance.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "100.0"))
	float LODStartDistance = 5000.0f;

	/**
	 * Minimum screen size for instances (0.0-1.0).
	 * Instances smaller than this on screen are culled.
	 * Higher values = more aggressive culling = better performance.
	 * 0.0 = no screen size culling, 0.01 = cull very small instances.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinScreenSize = 0.0f;

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

		// Cheap integer/bool checks first (ordered by rejection likelihood)

		// Check face type (single bool + enum comparison)
		if (bTopFacesOnly && Point.FaceType != EVoxelFaceType::Top)
		{
			return false;
		}

		// Check surface location filter (dry surface vs underwater vs underground).
		// Each point falls into exactly one category by precedence (Underground >
		// Underwater > Surface); the definition opts into any combination via the mask.
		const EScatterSurfaceLocationFlags PointCategory =
			Point.bIsUnderground ? EScatterSurfaceLocationFlags::Underground
			: Point.bIsUnderwater ? EScatterSurfaceLocationFlags::Underwater
			: EScatterSurfaceLocationFlags::Surface;
		if ((SurfaceLocationMask & static_cast<int32>(PointCategory)) == 0)
		{
			return false;
		}

		// Check elevation (two float comparisons, no trig): world Z, or on a planet the altitude above the datum
		if (Point.bHasAltitude
			? (Point.Altitude < MinElevation || Point.Altitude > MaxElevation)
			: (Point.Position.Z < MinElevation || Point.Position.Z > MaxElevation))
		{
			return false;
		}

		// Check material filter (uint8 linear search)
		if (AllowedMaterials.Num() > 0 && !AllowedMaterials.Contains(Point.MaterialID))
		{
			return false;
		}

		// Check biome filter (uint8 linear search)
		if (AllowedBiomes.Num() > 0 && !AllowedBiomes.Contains(Point.BiomeID))
		{
			return false;
		}

		// Check ambient occlusion (uint8 comparison)
		if (bAvoidShadowedAreas && Point.AmbientOcclusion > MaxAmbientOcclusion)
		{
			return false;
		}

		// Slope check last — uses pre-computed angle (two float comparisons, no Acos)
		if (Point.SlopeAngle < MinSlopeDegrees || Point.SlopeAngle > MaxSlopeDegrees)
		{
			return false;
		}

		return true;
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

	/**
	 * Compute position jitter offset in the tangent plane of a local up (spherical SP-G1): the XY plane for +Z (the
	 * offset above, bit for bit), the plane across the radial direction on a planet.
	 */
	FVector ComputePositionJitter(float RandomX, float RandomY, const FVector& Up) const
	{
		if (Up == FVector::UpVector || PositionJitter <= 0.0f)
		{
			return ComputePositionJitter(RandomX, RandomY);
		}
		const FVector T1 = FVector::CrossProduct(FMath::Abs(Up.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector, Up).GetSafeNormal();
		const FVector T2 = FVector::CrossProduct(Up, T1);
		return T1 * ((RandomX * 2.0f - 1.0f) * PositionJitter) + T2 * ((RandomY * 2.0f - 1.0f) * PositionJitter);
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

	/** Get approximate memory usage */
	SIZE_T GetAllocatedSize() const
	{
		return SpawnPoints.GetAllocatedSize();
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

	/** Current HISM instance count (actual rendered instances) */
	UPROPERTY(BlueprintReadOnly)
	int32 TotalHISMInstances = 0;

	/** Total surface points extracted (cumulative) */
	UPROPERTY(BlueprintReadOnly)
	int64 TotalSurfacePoints = 0;

	/** Total spawn points generated (cumulative) */
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

/**
 * A persistent, world-level scatter exclusion region: an oriented box inside which scatter
 * spawn points are suppressed (no foliage), while the terrain surface itself is untouched.
 *
 * Unlike the transient per-chunk cleared volumes (player-edit spheres that reset when a chunk
 * reloads), exclusion volumes persist across chunk streaming: a chunk that streams in while a
 * volume is active is born bare inside it, and unregistering the volume regrows the foliage.
 * Registered externally (e.g. a game-level bridge mapping claim footprints to volumes) via
 * UVoxelScatterExclusionSubsystem or UVoxelScatterManager directly — VoxelWorlds itself
 * attaches no semantics to who owns a volume.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FScatterExclusionVolume
{
	GENERATED_BODY()

	/** Stable identity. Registering a volume with an existing Id replaces it; unregister removes by Id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Exclusion")
	FGuid Id;

	/** World transform of the box centre (rotation + translation). Scale is IGNORED — bake scale into HalfExtent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Exclusion")
	FTransform Frame = FTransform::Identity;

	/** Oriented-box half-extents in world units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Exclusion")
	FVector HalfExtent = FVector(500.0f, 500.0f, 500.0f);

	/** Usable = has a valid Id and a positive extent on every axis. */
	bool IsUsable() const
	{
		return Id.IsValid() && HalfExtent.X > 0.0f && HalfExtent.Y > 0.0f && HalfExtent.Z > 0.0f;
	}

	/** True if WorldPoint lies inside the oriented box (Frame scale ignored). */
	bool ContainsPoint(const FVector& WorldPoint) const
	{
		const FVector Local = Frame.InverseTransformPositionNoScale(WorldPoint);
		return FMath::Abs(Local.X) <= HalfExtent.X
			&& FMath::Abs(Local.Y) <= HalfExtent.Y
			&& FMath::Abs(Local.Z) <= HalfExtent.Z;
	}

	/** World-space AABB of the oriented box — used to pre-filter which chunks a volume can affect. */
	FBox GetWorldBounds() const
	{
		FTransform NoScaleFrame = Frame;
		NoScaleFrame.SetScale3D(FVector::OneVector);
		return FBox(-HalfExtent, HalfExtent).TransformBy(NoScaleFrame);
	}
};

/**
 * One harvestable scatter instance found by UVoxelScatterManager::HarvestScatterNear.
 */
USTRUCT(BlueprintType)
struct VOXELCORE_API FScatterHarvestResult
{
	GENERATED_BODY()

	/** False when nothing harvestable was in range. */
	UPROPERTY(BlueprintReadOnly, Category = "Scatter|Harvest")
	bool bValid = false;

	/** The scatter type (FScatterDefinition::ScatterID). */
	UPROPERTY(BlueprintReadOnly, Category = "Scatter|Harvest")
	int32 ScatterID = 0;

	/** The definition's display name. */
	UPROPERTY(BlueprintReadOnly, Category = "Scatter|Harvest")
	FString Name;

	/** The definition's harvest category. */
	UPROPERTY(BlueprintReadOnly, Category = "Scatter|Harvest")
	FName HarvestCategory;

	/** World transform of the instance (its base). */
	UPROPERTY(BlueprintReadOnly, Category = "Scatter|Harvest")
	FTransform InstanceTransform;

	/** Distance from the query point to the instance base. */
	UPROPERTY(BlueprintReadOnly, Category = "Scatter|Harvest")
	float Distance = 0.0f;
};
