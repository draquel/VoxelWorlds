// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Materials/MaterialParameterCollection.h"
#include "VoxelCoreTypes.h"
#include "LODTypes.h"
#include "VoxelWorldConfiguration.generated.h"

class UVoxelBiomeConfiguration;
class UVoxelScatterConfiguration;

/**
 * Configuration data asset for a voxel world.
 *
 * Contains all settings needed to initialize a voxel world instance.
 * Create as a Data Asset in the editor for reusable configurations.
 *
 * @see Documentation/ARCHITECTURE.md
 */
UCLASS(BlueprintType)
class VOXELCORE_API UVoxelWorldConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	// ==================== World Settings ====================

	/** World generation mode (Infinite, Spherical, Island) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World")
	EWorldMode WorldMode = EWorldMode::InfinitePlane;

	/** Meshing style (Cubic blocks or Smooth terrain) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World")
	EMeshingMode MeshingMode = EMeshingMode::Cubic;

	/** World origin position */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World")
	FVector WorldOrigin = FVector::ZeroVector;

	/** World radius for spherical/island modes (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World", meta = (ClampMin = "1000", EditCondition = "WorldMode != EWorldMode::InfinitePlane"))
	float WorldRadius = 100000.0f;

	// ==================== Island Bowl Mode Settings ====================

	/**
	 * Island shape type:
	 * 0 = Circular (radial distance from center)
	 * 1 = Rectangle (axis-aligned box with separate X/Y sizes)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	int32 IslandShape = 0;  // Default: Circular

	/** Radius/SizeX of the island in world units (distance from center to edge start) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (ClampMin = "1000", EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	float IslandRadius = 50000.0f;

	/** Size Y of the island (only used when IslandShape = Rectangle) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (ClampMin = "1000", EditCondition = "WorldMode == EWorldMode::IslandBowl && IslandShape == 1"))
	float IslandSizeY = 50000.0f;

	/** Width of the falloff zone where terrain fades to nothing */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (ClampMin = "100", EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	float IslandFalloffWidth = 10000.0f;

	/**
	 * Type of falloff curve for island edges:
	 * 0 = Linear (simple but sharp)
	 * 1 = Smooth (hermite, gradual and natural-looking)
	 * 2 = Squared (faster drop near edge)
	 * 3 = Exponential (gradual then sharp drop)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (ClampMin = "0", ClampMax = "3", EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	int32 IslandFalloffType = 1;  // Default: Smooth

	/** Center X offset for the island (relative to WorldOrigin) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	float IslandCenterX = 0.0f;

	/** Center Y offset for the island (relative to WorldOrigin) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	float IslandCenterY = 0.0f;

	/** Minimum terrain height at island edges (can be negative for bowl effect) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	float IslandEdgeHeight = -1000.0f;

	/** Create a bowl shape (lowered edges) instead of plateau (raised center) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	bool bIslandBowlShape = false;

	// ==================== Spherical Planet Mode Settings ====================

	/**
	 * Maximum terrain height above the planet's base radius.
	 * Mountains and peaks can rise this high above WorldRadius.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "100", ClampMax = "50000", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
	float PlanetMaxTerrainHeight = 5000.0f;

	/**
	 * Maximum terrain depth below the planet's base radius.
	 * Valleys and caves can descend this deep below WorldRadius.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "100", ClampMax = "50000", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
	float PlanetMaxTerrainDepth = 2000.0f;

	/**
	 * Height scale for planetary terrain features.
	 * Controls the magnitude of terrain displacement from the base radius.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "100", ClampMax = "50000", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
	float PlanetHeightScale = 5000.0f;

	/**
	 * Spawn location on the planet surface.
	 * 0 = +X (Equator East), 1 = +Y (Equator North), 2 = +Z (North Pole), 3 = -Z (South Pole)
	 * The spawn position will be at WorldOrigin + Direction * (WorldRadius + SpawnAltitude)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "0", ClampMax = "3", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
	int32 PlanetSpawnLocation = 2;  // Default: North Pole (+Z)

	/**
	 * Altitude above the planet surface for spawn point.
	 * Added to WorldRadius to place spawn slightly above terrain.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "0", ClampMax = "10000", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
	float PlanetSpawnAltitude = 500.0f;

	// ==================== Water Settings ====================

	/**
	 * Enable water level for the world.
	 * When enabled, terrain below the water level uses underwater materials.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Water")
	bool bEnableWaterLevel = false;

	/**
	 * Water level height in world units.
	 * Used for InfinitePlane and IslandBowl world modes.
	 * Terrain surface below this height will use underwater materials.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Water", meta = (EditCondition = "bEnableWaterLevel && WorldMode != EWorldMode::SphericalPlanet"))
	float WaterLevel = 0.0f;

	/**
	 * Water radius for spherical planet mode.
	 * Used instead of WaterLevel for SphericalPlanet world mode.
	 * Terrain surface below this radius from planet center will use underwater materials.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Water", meta = (ClampMin = "1000", EditCondition = "bEnableWaterLevel && WorldMode == EWorldMode::SphericalPlanet"))
	float WaterRadius = 100000.0f;

	/**
	 * Show a visual water plane in the world.
	 * Creates a simple static mesh plane at the water level for visualization.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Water", meta = (EditCondition = "bEnableWaterLevel"))
	bool bShowWaterPlane = true;

	// ==================== Terrain Generation Settings ====================

	/** Sea level height - base elevation of terrain (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain", meta = (EditCondition = "WorldMode != EWorldMode::SphericalPlanet"))
	float SeaLevel = 0.0f;

	/** Height scale - multiplier for noise-to-height conversion (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain", meta = (ClampMin = "100", ClampMax = "50000", EditCondition = "WorldMode != EWorldMode::SphericalPlanet"))
	float HeightScale = 5000.0f;

	/** Base height - additional offset added to terrain height (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain", meta = (EditCondition = "WorldMode != EWorldMode::SphericalPlanet"))
	float BaseHeight = 0.0f;

	// ==================== Voxel Settings ====================

	/** Size of one voxel in world units (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel", meta = (ClampMin = "1", ClampMax = "1000"))
	float VoxelSize = 100.0f;

	/** Number of voxels per chunk edge (typically 32) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel", meta = (ClampMin = "8", ClampMax = "128"))
	int32 ChunkSize = VOXEL_DEFAULT_CHUNK_SIZE;

	/** Random seed for world generation (0 = random) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voxel")
	int32 WorldSeed = 0;

	// ==================== Noise Generation Settings ====================

	/** Parameters for noise-based terrain generation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise Generation")
	FVoxelNoiseParams NoiseParams;

	/** Use GPU compute shaders for terrain generation (true) or CPU fallback (false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise Generation")
	bool bUseGPUGeneration = true;

	// ==================== Biome Settings ====================

	/** Enable biome-based material selection (temperature/moisture driven) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	bool bEnableBiomes = true;

	/**
	 * Biome configuration data asset.
	 * Defines biomes, blending parameters, and height-based material rules.
	 * Create a UVoxelBiomeConfiguration asset and assign it here.
	 * If null, default biomes (Plains, Desert, Tundra) will be used.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (EditCondition = "bEnableBiomes"))
	TObjectPtr<UVoxelBiomeConfiguration> BiomeConfiguration;

	// ==================== LOD Settings ====================

	/**
	 * Enable Level of Detail system.
	 * When disabled, all chunks render at LOD 0 (full detail).
	 * Useful for debugging LOD seams or when view distance is small.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bEnableLOD = true;

	/** LOD distance bands configuration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "bEnableLOD"))
	TArray<FLODBand> LODBands;

	/** Enable smooth LOD transitions (morphing) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "bEnableLOD"))
	bool bEnableLODMorphing = true;

	/**
	 * Enable LOD seam handling (skirts, Transvoxel, etc.).
	 * When disabled, no seam geometry is generated between LOD levels.
	 * Useful for debugging or when LOD is disabled entirely.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "bEnableLOD"))
	bool bEnableLODSeams = true;

	/** Enable view frustum culling for chunks */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bEnableFrustumCulling = true;

	/** Maximum view distance for chunk loading (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "1000"))
	float ViewDistance = 10000.0f;

	/**
	 * Material Parameter Collection for LOD morphing.
	 *
	 * Create an MPC asset with these scalar parameters:
	 *   - LODStartDistance: Distance where MorphFactor = 0
	 *   - LODEndDistance: Distance where MorphFactor = 1
	 *   - LODInvRange: 1.0 / (EndDistance - StartDistance)
	 *
	 * Values are auto-calculated from LODBands configuration.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "bEnableLODMorphing"))
	TObjectPtr<UMaterialParameterCollection> LODParameterCollection;

	// ==================== Streaming Settings ====================

	/** Maximum chunks to load per frame (generation, meshing, render submit).
	 * Lower values reduce stuttering but slow initial loading. Mesh generation is expensive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxChunksToLoadPerFrame = 2;

	/** Maximum chunks to unload per frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxChunksToUnloadPerFrame = 16;

	/** Time budget for streaming operations per frame (milliseconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "0.5", ClampMax = "16"))
	float StreamingTimeSliceMS = 4.0f;

	/** Maximum number of chunks to keep loaded */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "100", ClampMax = "50000"))
	int32 MaxLoadedChunks = 5000;

	// ==================== Meshing Settings ====================

	/**
	 * Use greedy meshing to merge adjacent faces with the same material.
	 * Reduces triangle count by 40-60% but requires special material handling for texture atlases.
	 * When disabled, each voxel face is a separate quad with 0-1 UVs (simpler material setup).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meshing")
	bool bUseGreedyMeshing = false;

	/**
	 * Calculate per-vertex ambient occlusion.
	 * Adds subtle shadowing in corners and crevices.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meshing")
	bool bCalculateAO = true;

	/**
	 * UV scale multiplier for texture coordinates.
	 * With greedy meshing: UVs scale with merged quad size (e.g., 5x3 quad = UV 0-5, 0-3).
	 * Without greedy meshing: Each face is 0-1 * UVScale.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meshing", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float UVScale = 1.0f;

	// ==================== Rendering Settings ====================

	/** Use GPU-driven custom vertex factory (true) or PMC fallback (false) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bUseGPURenderer = true;

	/** Generate collision meshes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bGenerateCollision = true;

	/** LOD level to use for collision (higher = simpler) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (ClampMin = "0", ClampMax = "4", EditCondition = "bGenerateCollision"))
	int32 CollisionLODLevel = 1;

	// ==================== Scatter Settings ====================

	/** Enable scatter system (vegetation, rocks, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter")
	bool bEnableScatter = true;

	/**
	 * Scatter configuration data asset.
	 * Defines scatter types (grass, rocks, trees) with meshes and placement rules.
	 * Create a UVoxelScatterConfiguration asset and assign it here.
	 * If null, default scatter definitions will be used.
	 *
	 * To create: Right-click in Content Browser -> Miscellaneous -> Data Asset -> VoxelScatterConfiguration
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter", meta = (EditCondition = "bEnableScatter"))
	TObjectPtr<UVoxelScatterConfiguration> ScatterConfiguration;

	/** Maximum distance for scatter placement (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter", meta = (ClampMin = "1000.0", EditCondition = "bEnableScatter"))
	float ScatterRadius = 10000.0f;

	/** Enable scatter debug visualization (spheres at spawn points) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Debug", meta = (EditCondition = "bEnableScatter"))
	bool bScatterDebugVisualization = false;

public:
	UVoxelWorldConfiguration();

	/** Get the world size of a single chunk */
	FORCEINLINE float GetChunkWorldSize() const
	{
		return ChunkSize * VoxelSize;
	}

	/** Get the world size of a single chunk at a specific LOD */
	float GetChunkWorldSizeAtLOD(int32 LODLevel) const;

	/** Get LOD band for a given distance, returns nullptr if beyond all bands */
	const FLODBand* GetLODBandForDistance(float Distance) const;

	/** Get the LOD level for a given distance */
	int32 GetLODLevelForDistance(float Distance) const;

	/** Validate configuration and log any issues */
	bool ValidateConfiguration() const;

	/**
	 * Get the start distance for material-based LOD morphing.
	 * Derived from LODBands: first band's (MaxDistance - MorphRange)
	 */
	float GetMaterialLODStartDistance() const;

	/**
	 * Get the end distance for material-based LOD morphing.
	 * Derived from LODBands: last band's MaxDistance (clamped to ViewDistance)
	 */
	float GetMaterialLODEndDistance() const;

	/**
	 * Get the spawn position for spherical planet mode.
	 * Returns WorldOrigin + SpawnDirection * (WorldRadius + PlanetSpawnAltitude)
	 *
	 * PlanetSpawnLocation values:
	 *   0 = +X (Equator East)
	 *   1 = +Y (Equator North)
	 *   2 = +Z (North Pole) - Default
	 *   3 = -Z (South Pole)
	 *
	 * Only valid when WorldMode == SphericalPlanet.
	 */
	FVector GetPlanetSpawnPosition() const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
