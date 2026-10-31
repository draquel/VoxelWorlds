// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "VoxelCoreTypes.h"
#include "VoxelTreeTypes.h"
#include "LODTypes.h"
#include "VoxelWorldConfiguration.generated.h"

class UMaterialInterface;
class UVoxelBiomeConfiguration;
class UVoxelCaveConfiguration;
class UVoxelScatterConfiguration;
class URuntimeVirtualTexture;

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

	/**
	 * Block pawns, NPCs and physics bodies at the edge of a bounded world: invisible walls at the world mode's bounds
	 * (an island's R + falloff), so nothing walks, swims or rolls off into the void beyond it (world modes review WM-I5,
	 * decision D9). Unbounded worlds and planets have no walls. The cvar voxel.World.Boundary 0 turns their collision
	 * off at runtime.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World")
	bool bWorldBoundary = true;

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

	/**
	 * Terrain height the falloff fades to at the island's edge (R + falloff width). Beyond the edge the seabed keeps
	 * sloping down (IslandSeabedDepth, IslandSeabedSlopeWidth) instead of dropping to a flat floor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	float IslandEdgeHeight = -1000.0f;

	/**
	 * How far below IslandEdgeHeight the open sea's floor lies beyond the island's edge. Past R + falloff width the
	 * seabed slopes down by this much over IslandSeabedSlopeWidth (a smoothstep, flat where it meets the falloff), then
	 * stays flat, so the island stands on a slope into deep water instead of a cliff (world modes review WM-E1, KI2).
	 * Generation (CPU and GPU), the analytic height and the far ocean's seabed (bIslandFarOcean) all use it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (ClampMin = "0", EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	float IslandSeabedDepth = 6000.0f;

	/**
	 * Horizontal distance past the island's edge over which the seabed slopes down to IslandSeabedDepth (world units).
	 * 0 gives a step at the edge (the floor 1,000 uu down was such a step before WM-E1).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (ClampMin = "0", EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	float IslandSeabedSlopeWidth = 20000.0f;

	/**
	 * Draw the open sea beyond a bounded world's edge (world modes review WM-E1): a water surface from every chunk
	 * column the streaming has not loaded out to IslandFarOceanDistance past the edge, over a seabed that continues
	 * the generated slope. Without it the island floats in a void seen from outside the loaded ring. The water needs
	 * bEnableWaterLevel and WaterMeshMaterial; the seabed is drawn either way. The cvar voxel.World.FarOcean 0 hides
	 * it at runtime.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (EditCondition = "WorldMode == EWorldMode::IslandBowl"))
	bool bIslandFarOcean = true;

	/** How far past the island's edge the far ocean reaches (world units): far enough that its rim sits at the horizon */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Island", meta = (ClampMin = "10000", EditCondition = "WorldMode == EWorldMode::IslandBowl && bIslandFarOcean"))
	float IslandFarOceanDistance = 2000000.0f;

	// ==================== Spherical Planet Mode Settings ====================

	/**
	 * Maximum terrain height above the planet's base radius.
	 * Mountains and peaks can rise this high above WorldRadius. Must cover BaseHeight + PlanetHeightScale / 2,
	 * plus continentalness's highest offset with the scale at its largest multiplier: the highest the terrain
	 * reaches. Terrain above the shell is not streamed and is cut off (GetSettingsWarnings checks it). Up to 10 km
	 * (1,000,000 uu, spherical Phase 6 D24: mountains of a few km on planets up to about 1,000 km).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "100", ClampMax = "1000000", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
	float PlanetMaxTerrainHeight = 5000.0f;

	/**
	 * Maximum terrain depth below the planet's base radius.
	 * Valleys and caves can descend this deep below WorldRadius. Must cover PlanetHeightScale / 2 - BaseHeight,
	 * plus continentalness's lowest offset with the scale at its largest multiplier: the deepest the terrain
	 * reaches (GetSettingsWarnings checks it). Up to 10 km (spherical Phase 6 D24).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "100", ClampMax = "1000000", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
	float PlanetMaxTerrainDepth = 2000.0f;

	/**
	 * Height scale for planetary terrain features: the surface sits at WorldRadius + BaseHeight + noise *
	 * PlanetHeightScale / 2, with noise in [-1, 1], and continentalness (the biome configuration's curves) offsets
	 * BaseHeight and scales PlanetHeightScale as it does BaseHeight and HeightScale in heightmap modes. Planets use
	 * this instead of HeightScale. Up to 10 km (spherical Phase 6 D24).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "100", ClampMax = "1000000", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
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

	/**
	 * How far above the datum a planet's world reaches (spherical Phase 6, D18/D20: orbit and back): the outer
	 * out-of-world bound sits this altitude above WorldRadius, and KillZ one chunk below the planet centre minus it, so a
	 * pawn flying anywhere around the planet up to it is never killed by the engine's Z plane. 0 = 3 x WorldRadius (the
	 * whole planet in view from about 2 R up). See FSphericalPlanetParams::OrbitCeiling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "0", EditCondition = "WorldMode == EWorldMode::SphericalPlanet"))
	float PlanetOrbitCeiling = 0.0f;

	/**
	 * The voxel cut-over (spherical Phase 6, WM-BP10, D18: orbit and back). Above this altitude over the surface under the
	 * streaming viewer (the terrain along its direction, or the sea where that is higher) a planet's voxel window unloads
	 * and the far field alone draws the planet; it streams back, fading in over the far field, once the viewer is below
	 * 0.85 of it (FDistanceBandLODStrategy::CutoverResumeFraction). 0 = three times the view distance
	 * (FDistanceBandLODStrategy::DefaultCutoverViewDistances): from there the window's ground disk is a narrow cone under
	 * the viewer, and a descent at 5,000 uu/s still has about 20 s to stream it back before landing (at the view distance
	 * a 50 km planet's window was 5 s short). voxel.World.VoxelCutover overrides it.
	 *
	 * Heightmap worlds too (world modes review WM-MP2, D38): the same altitude over the generated ground or the sea under
	 * the streaming viewer, in force only while the far field draws the world beyond the window (voxel.World.FarField).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Planet", meta = (ClampMin = "0"))
	float PlanetVoxelCutoverAltitude = 0.0f;

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
	 * On a planet the sea is WaterRadius instead, for every water consumer (FVoxelSeaSurface: generation, swimming,
	 * scatter, navigation, NPC spawning, the map and the PCG sampler; SPHERICAL_PLANET_REQUIREMENTS.md SP-E1).
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
	 * Only used as fallback when WaterMeshMaterial is not set.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Water", meta = (EditCondition = "bEnableWaterLevel"))
	bool bShowWaterPlane = true;

	/**
	 * Material for per-chunk water surface rendering.
	 * Should use translucent or Single Layer Water shading model.
	 * When set, each chunk generates its own cave-aware water mesh (no water inside caves).
	 * If null, falls back to single water plane (bShowWaterPlane) which renders water everywhere.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Water", meta = (EditCondition = "bEnableWaterLevel"))
	TObjectPtr<UMaterialInterface> WaterMeshMaterial;

	// ==================== Terrain Generation Settings ====================

	/** Sea level height - base elevation of terrain (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain", meta = (EditCondition = "WorldMode != EWorldMode::SphericalPlanet"))
	float SeaLevel = 0.0f;

	/** Height scale - multiplier for noise-to-height conversion (world units) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain", meta = (ClampMin = "100", ClampMax = "50000", EditCondition = "WorldMode != EWorldMode::SphericalPlanet"))
	float HeightScale = 5000.0f;

	/**
	 * Base height - additional offset added to terrain height (world units).
	 * On a planet: a radial offset of the whole surface from WorldRadius (SeaLevel and HeightScale are not used there).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World|Terrain")
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

	/**
	 * Parameters for noise-based terrain generation. Frequency is per world unit in every mode: heightmap modes sample
	 * the noise at (X, Y), a planet at the point on its datum sphere (direction from the centre x WorldRadius), so a
	 * feature is about 1 / Frequency uu across whatever the planet's radius (spherical SP-B1).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise Generation")
	FVoxelNoiseParams NoiseParams;

	/**
	 * Use GPU compute shaders for terrain generation (true) or CPU fallback (false), in every world mode: spherical
	 * planets generate on the GPU since spherical Phase 2 (ShouldUseGPUGeneration; SPHERICAL_PLANET_REQUIREMENTS.md
	 * SP-B4).
	 */
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
	 * If null, default biomes (Plains, Forest, Mountain, Ocean) will be used.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (EditCondition = "bEnableBiomes"))
	TObjectPtr<UVoxelBiomeConfiguration> BiomeConfiguration;

	// ==================== Cave Settings ====================

	/** Enable cave generation (underground caverns, tunnels, and passages) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves")
	bool bEnableCaves = false;

	/**
	 * Cave configuration data asset.
	 * Defines cave layers (cheese caverns, spaghetti tunnels, noodle passages),
	 * per-biome overrides, and cave wall material settings.
	 * Create a UVoxelCaveConfiguration asset and assign it here.
	 * If null but bEnableCaves is true, default cave layers will be used.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves", meta = (EditCondition = "bEnableCaves"))
	TObjectPtr<UVoxelCaveConfiguration> CaveConfiguration;

	// ==================== LOD Settings ====================

	/**
	 * Enable Level of Detail system.
	 * When disabled, all chunks render at LOD 0 (full detail).
	 * Useful for debugging LOD seams or when view distance is small.
	 * Ignored in cubic meshing mode, which always runs without LOD: greedy meshing is cubic's
	 * answer to distance. Read the effective value through IsLODEnabled().
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "MeshingMode != EMeshingMode::Cubic"))
	bool bEnableLOD = true;

	/**
	 * LOD distance bands configuration. With the LOD system off (bEnableLOD off, or cubic meshing)
	 * the bands still describe distance: the far-band surface slab starts past band 0, and far-chunk
	 * compression applies from the band whose stride reaches voxel.FarCompression.MinStride.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (EditCondition = "bEnableLOD"))
	TArray<FLODBand> LODBands;

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
	 * Surface-slab culling (heightmap worlds: InfinitePlane and IslandBowl): every column loads only the chunks around
	 * its OWN analytic surface height (± 1.5 chunks of slack, and up to the water plane) at every distance, instead of
	 * the world's full min–max height range, plus the viewer's own layer ± 2 in the columns near the viewer (the first
	 * LOD band plus two chunk widths), for caves, dungeons and digging. That cuts a column from the 8 layers of the
	 * demo's height range to 4-5 and keeps the ground streamed whatever the viewer's altitude (world modes review D8).
	 * Cave interiors and edits deeper than the slab load only near the viewer (edited chunks are also pinned within the
	 * view distance). Off: every column loads the whole height range. The name predates D8, when the slab applied
	 * beyond the first band only.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD")
	bool bFarBandSurfaceSlabCulling = true;

	/**
	 * LOD refine (detail-increasing) hysteresis deadband, in chunk-width units. A chunk
	 * upgrades to a finer LOD once the viewer crosses the band's inner edge by this many
	 * chunk widths. Keep small (eager) so detail lands at/just-before you reach a chunk;
	 * the asymmetry with LODCoarsenHysteresis still prevents flip/flop at the boundary.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0", EditCondition = "bEnableLOD"))
	float LODRefineHysteresis = 0.25f;

	/**
	 * LOD coarsen (detail-decreasing) hysteresis deadband, in chunk-width units. A chunk
	 * downgrades to a coarser LOD only once the viewer is this many chunk widths past the
	 * band's outer edge. Keep larger (damped) so terrain you just left doesn't flicker.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD", meta = (ClampMin = "0", EditCondition = "bEnableLOD"))
	float LODCoarsenHysteresis = 0.5f;

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

	// There is no chunk-count cap (MaxLoadedChunks was never read; world modes review WM-S3). Residency is bounded by
	// geometry: the view distance (unload at ViewDistance x 1.2, measured horizontally in heightmap worlds), each
	// column's vertical window (its surface slab, plus the viewer's layer +-2 near the viewer; the terrain's height
	// bounds), the island's footprint (R + falloff + a chunk diagonal), and edited chunks pinned out to the unload
	// distance. See Documentation/Features/LOD_SYSTEM.md, "What Bounds Residency".

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

	/** Generate collision meshes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering")
	bool bGenerateCollision = true;

	/** LOD level to use for collision (higher = simpler) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering", meta = (ClampMin = "0", ClampMax = "4", EditCondition = "bGenerateCollision"))
	int32 CollisionLODLevel = 0;

	/**
	 * Runtime Virtual Textures the terrain writes into.
	 *
	 * The UVoxelWorldComponent is created at runtime by the custom-VF renderer; this list is
	 * copied onto WorldComponent->RuntimeVirtualTextures BEFORE RegisterComponent() so the voxel
	 * scene proxy (created during registration) opts into the RVT pass and emits a mesh batch per
	 * RuntimeVirtualTextureMaterialType into these pages. Leave empty to disable RVT writes
	 * (zero overhead — the emission loop is a no-op when no RVT is assigned).
	 *
	 * The same URuntimeVirtualTexture asset(s) are referenced by an ARuntimeVirtualTextureVolume
	 * (defines the capture region) and sampled back by read-back materials via the
	 * "Runtime Virtual Texture Sample" node.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rendering|Runtime Virtual Texture")
	TArray<TObjectPtr<URuntimeVirtualTexture>> RuntimeVirtualTextures;

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

	/**
	 * Automatically override scatter definitions to use BlockFaceSnap placement
	 * and cubic-appropriate defaults when MeshingMode is Cubic.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Cubic", meta = (EditCondition = "bEnableScatter"))
	bool bAutoCubicScatterDefaults = true;

	// ==================== Scatter | Tree Settings ====================

	/**
	 * How voxel trees are rendered in cubic mode.
	 * VoxelData = injected into terrain (editable), HISM = mesh instances, Both = near/far split.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Trees", meta = (EditCondition = "bEnableScatter"))
	EVoxelTreeMode TreeMode = EVoxelTreeMode::VoxelData;

	/**
	 * Distance threshold for Both mode: VoxelData trees within this distance, HISM beyond.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Trees", meta = (ClampMin = "1000.0", EditCondition = "bEnableScatter && TreeMode == EVoxelTreeMode::Both"))
	float VoxelTreeMaxDistance = 10000.0f;

	/**
	 * Tree templates defining tree shapes for voxel injection.
	 * Index into this array using FScatterDefinition::TreeTemplateID.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Trees", meta = (EditCondition = "bEnableScatter"))
	TArray<FVoxelTreeTemplate> TreeTemplates;

	/**
	 * Trees per chunk (fractional part is probability).
	 * E.g., 1.5 = 1 guaranteed tree + 50% chance of a second.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Trees", meta = (ClampMin = "0.0", ClampMax = "10.0", EditCondition = "bEnableScatter"))
	float TreeDensity = 1.5f;

	/** Enable scatter debug visualization (spheres at spawn points) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter|Debug", meta = (EditCondition = "bEnableScatter"))
	bool bScatterDebugVisualization = false;

	// ==================== Performance Tuning ====================

	/** Maximum concurrent async noise generation tasks (1-8). Generation runs on thread pool to avoid game thread stalls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxAsyncGenerationTasks = 2;

	/** Maximum concurrent async mesh generation tasks (1-16) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxAsyncMeshTasks = 8;

	/** Maximum LOD level change remeshes queued per frame (1-8) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "1", ClampMax = "8"))
	int32 MaxLODRemeshPerFrame = 4;

	/** Maximum pending meshes before throttling generation (2-16) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "2", ClampMax = "16"))
	int32 MaxPendingMeshes = 8;

	/**
	 * Maximum GPU mesh DISPATCHES launched per frame — the mesh-consumer drain rate (1-32).
	 * Was hardcoded to 2, which was far too conservative: A/B on the demo showed 2->6 cuts
	 * post-traverse queue drain ~2.5x and peak mesh backlog ~35% at zero frame cost (the
	 * per-dispatch render/game-thread cost is small vs frame time at moderate traversal).
	 * Each dispatch builds an RDG graph + ~150KB upload + 4 compute passes, so very high
	 * values trade render-thread frame time for drain rate — raise MaxAsyncMeshTasks /
	 * MaxPendingMeshes alongside it or the in-flight caps bind first. Runtime override:
	 * voxel.Stream.MaxMeshDispatchPerFrame.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "1", ClampMax = "32"))
	int32 MaxMeshDispatchPerFrame = 6;

	/** Target frame rate for adaptive throttling (0 = disabled) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "0", ClampMax = "240"))
	float TargetFrameRate = 60.0f;

	/** Reduce generation work when FPS drops below target */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance")
	bool bAdaptiveThrottling = true;

	/** Maximum concurrent async scatter generation tasks (1-4). Scatter extraction and placement run on thread pool. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "1", ClampMax = "4"))
	int32 MaxAsyncScatterTasks = 2;

	/** Maximum concurrent async collision cooking tasks (1-4). Collision mesh generation runs on thread pool. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance", meta = (ClampMin = "1", ClampMax = "4"))
	int32 MaxAsyncCollisionTasks = 2;

public:
	UVoxelWorldConfiguration();

	/** Get the world size of a single chunk */
	FORCEINLINE float GetChunkWorldSize() const
	{
		return ChunkSize * VoxelSize;
	}

	/**
	 * Whether the LOD system runs for this world: bEnableLOD, except that cubic meshing always runs
	 * without LOD (greedy meshing is its LOD mechanism, so cubic chunks mesh at full detail at every
	 * distance). Read this instead of bEnableLOD.
	 *
	 * @return True when chunks get distance-band LOD levels, false when every chunk is LOD 0.
	 */
	UFUNCTION(BlueprintPure, Category = "LOD")
	bool IsLODEnabled() const
	{
		return bEnableLOD && MeshingMode != EMeshingMode::Cubic;
	}

	/**
	 * Whether terrain generation can run on the GPU: bUseGPUGeneration, in every world mode. Spherical planets were
	 * held on the CPU until the GPU generator had a planet branch (SP-B3: it fell through to generic 3D noise, blobs
	 * instead of a planet); since spherical Phase 2 the GPU generates the CPU's planet (SP-B4), locked by
	 * VoxelWorlds.Generation.GPUvsCPUSphericalParity (WORLD_MODES_REVIEW_PLAN.md §16).
	 * The chunk manager also honours -VoxelForceCPU and voxel.GPUGeneration.Enable on top of this.
	 *
	 * @return True when the configuration allows GPU generation.
	 */
	UFUNCTION(BlueprintPure, Category = "Noise Generation")
	bool ShouldUseGPUGeneration() const
	{
		return bUseGPUGeneration;
	}

	/** Validate configuration and log any issues (including GetSettingsWarnings) */
	bool ValidateConfiguration() const;

	/**
	 * A planet's terrain extent over and under WorldRadius: BaseHeight with continentalness's offsets and PlanetHeightScale
	 * at its largest multiplier, the numbers GetSettingsWarnings checks the shell (PlanetMaxTerrainHeight / Depth)
	 * against (FSphericalPlanetWorldMode::GetDisplacementBounds). Spherical Phase 6's test planets size their shells
	 * from it (Scripts/setup_planet_test.py).
	 * @param OutHighest How far above WorldRadius the terrain can reach
	 * @param OutDeepest How far below WorldRadius it can reach
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	void GetPlanetTerrainExtent(float& OutHighest, float& OutDeepest) const;

	/**
	 * Settings that load and run but produce wrong results, one readable line each: a tree template using a
	 * material id the material registry does not define (KC10); on a planet, a terrain shell that does not cover
	 * the terrain's real amplitude, and water enabled while consumers still read the Z-plane WaterLevel
	 * (SPHERICAL_PLANET_REQUIREMENTS.md SP-A5). Logged as warnings when the asset loads (PostLoad), when a voxel
	 * world initializes with it, and from ValidateConfiguration.
	 *
	 * @return The warnings; empty when nothing is wrong.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Configuration")
	TArray<FString> GetSettingsWarnings() const;

	/**
	 * Get the spawn position for spherical planet mode over the datum (no terrain: the configuration has no world mode).
	 * Returns WorldOrigin + SpawnDirection * (WorldRadius + PlanetSpawnAltitude). For the point over the terrain use
	 * UVoxelChunkManager::GetPlanetSpawnPosition (spherical SP-B11).
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

	/**
	 * Object path of a configuration that replaces the one assigned to the voxel world actor, or an
	 * empty string when none is set. Lets benchmarks and reviews A/B world modes and meshing modes on
	 * one map without editing it.
	 *
	 * Sources, in priority order: the -VoxelConfig=<ObjectPath> command-line switch (headless and
	 * standalone runs), then the voxel.ConfigOverride console variable (set it in the editor before
	 * pressing Play). Read when a voxel world initializes, so a change applies to the next PIE session.
	 *
	 * @return Object path such as /Game/PluginTesting/Review/Review_Cubic, or empty.
	 */
	static FString GetOverridePath();

	/** Logs GetSettingsWarnings, so a bad asset is reported when it loads. */
	virtual void PostLoad() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
