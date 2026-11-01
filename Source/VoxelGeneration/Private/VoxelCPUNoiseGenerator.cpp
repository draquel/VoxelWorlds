// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUNoiseGenerator.h"
#include "VoxelHash.h"
#include "VoxelGeneration.h"
#include "VoxelTerrainConditioning.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelBiomeSnapshot.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelSeaSurface.h"
#include "VoxelDownLines.h"
#include "VoxelNoise.h"
#include "Async/Async.h"

FVoxelCPUNoiseGenerator::FVoxelCPUNoiseGenerator()
{
}

FVoxelCPUNoiseGenerator::~FVoxelCPUNoiseGenerator()
{
	if (bIsInitialized)
	{
		Shutdown();
	}
}

void FVoxelCPUNoiseGenerator::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	bIsInitialized = true;
	UE_LOG(LogVoxelGeneration, Log, TEXT("CPU Noise Generator initialized"));
}

void FVoxelCPUNoiseGenerator::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	FScopeLock Lock(&ResultsLock);
	StoredResults.Empty();
	bIsInitialized = false;

	UE_LOG(LogVoxelGeneration, Log, TEXT("CPU Noise Generator shutdown"));
}

FVoxelGenerationHandle FVoxelCPUNoiseGenerator::GenerateChunkAsync(
	const FVoxelNoiseGenerationRequest& Request,
	FOnVoxelGenerationComplete OnComplete)
{
	uint64 RequestId = NextRequestId++;
	FVoxelGenerationHandle Handle(RequestId);

	// CPU implementation just runs synchronously on a background thread
	TArray<FVoxelData> VoxelData;
	bool bSuccess = GenerateChunkCPU(Request, VoxelData);

	if (bSuccess)
	{
		FScopeLock Lock(&ResultsLock);
		StoredResults.Add(RequestId, MoveTemp(VoxelData));
	}

	Handle.bIsComplete = true;
	Handle.bWasSuccessful = bSuccess;

	// Call completion callback (async to match GPU behavior pattern)
	if (OnComplete.IsBound())
	{
		AsyncTask(ENamedThreads::GameThread, [OnComplete, Handle, bSuccess]()
		{
			OnComplete.Execute(Handle, bSuccess);
		});
	}

	return Handle;
}

bool FVoxelCPUNoiseGenerator::GenerateChunkCPU(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 TotalVoxels = ChunkSize * ChunkSize * ChunkSize;
	// Always generate at base VoxelSize regardless of LOD level
	// LOD stride is applied during meshing, not generation
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();

	OutVoxelData.SetNum(TotalVoxels);

	// Check world mode and delegate to appropriate generation method
	if (Request.WorldMode == EWorldMode::InfinitePlane)
	{
		// Create world mode with terrain parameters
		FWorldModeTerrainParams TerrainParams(Request.SeaLevel, Request.HeightScale, Request.BaseHeight);
		FInfinitePlaneWorldMode WorldMode(TerrainParams);

		GenerateChunkHeightmap(Request, WorldMode, OutVoxelData);
	}
	else if (Request.WorldMode == EWorldMode::IslandBowl)
	{
		// Create island bowl world mode with terrain and island parameters
		FWorldModeTerrainParams TerrainParams(Request.SeaLevel, Request.HeightScale, Request.BaseHeight);

		// The request carries the island params as FVoxelWorldModeFactory built them (absolute centre).
		FIslandBowlWorldMode WorldMode(TerrainParams, Request.IslandParams);

		GenerateChunkHeightmap(Request, WorldMode, OutVoxelData);
	}
	else if (Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		// Create spherical planet world mode
		FWorldModeTerrainParams TerrainParams(0.0f, Request.HeightScale, Request.BaseHeight);

		FSphericalPlanetWorldMode WorldMode(TerrainParams, Request.SphericalPlanetParams);

		GenerateChunkSphericalPlanet(Request, WorldMode, OutVoxelData);
	}
	else
	{
		// EWorldMode has exactly the three values above
		checkNoEntry();
	}

	// Post-generation: mark water voxels via column scan
	ApplyWaterFillPass(Request, OutVoxelData);

	// Post-generation: classify underground air (caves, enclosed voids)
	ApplyUndergroundClassificationPass(Request, OutVoxelData);

	return true;
}

void FVoxelCPUNoiseGenerator::GenerateChunkHeightmap(
	const FVoxelNoiseGenerationRequest& Request,
	const IVoxelWorldMode& WorldMode,
	TArray<FVoxelData>& OutVoxelData)
{
	// The one heightmap body (code review CR2, CORE-23): the plane's and the island's differ only in the height step
	// below (the island composes its edge falloff on the base height); everything else is one text.
	const FWorldModeTerrainParams& TerrainParams = WorldMode.GetWorldModeType() == EWorldMode::IslandBowl
		? static_cast<const FIslandBowlWorldMode&>(WorldMode).GetTerrainParams()
		: static_cast<const FInfinitePlaneWorldMode&>(WorldMode).GetTerrainParams();
	const FIslandBowlParams* IslandParams = WorldMode.GetWorldModeType() == EWorldMode::IslandBowl
		? &static_cast<const FIslandBowlWorldMode&>(WorldMode).GetIslandParams()
		: nullptr;

	const int32 ChunkSize = Request.ChunkSize;
	// Always generate at base VoxelSize for voxel positioning
	// LOD stride is applied during meshing, not generation
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();
	// The blend zones' dither keys (G39): the chunk's first voxel on the world's grid, plus each voxel's local index.
	const FIntVector DitherBase = FVoxelBiomeBlendDither::GridBase(Request.ChunkCoord, ChunkSize, Request.WorldOrigin, VoxelSize);
	const int32 DitherSeed = Request.NoiseParams.Seed;

	// Get biome configuration (may be null if biomes disabled)
	const UVoxelBiomeConfiguration* BiomeConfig = Request.BiomeConfiguration;

	// Hoisted continentalness snapshot for the per-voxel height modulation below (plain data; the
	// per-voxel ComputeEffectiveTerrainParams call takes this instead of the UObject).
	const FVoxelBiomeSnapshot BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(BiomeConfig);

	// Set up biome noise parameters from configuration
	FVoxelNoiseParams TempNoiseParams;
	TempNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	TempNoiseParams.Octaves = 2;  // Fewer octaves for smoother biome transitions
	TempNoiseParams.Persistence = 0.5f;
	TempNoiseParams.Lacunarity = 2.0f;
	TempNoiseParams.Amplitude = 1.0f;

	FVoxelNoiseParams MoistureNoiseParams;
	MoistureNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	MoistureNoiseParams.Octaves = 2;
	MoistureNoiseParams.Persistence = 0.5f;
	MoistureNoiseParams.Lacunarity = 2.0f;
	MoistureNoiseParams.Amplitude = 1.0f;

	// Use configuration values if available, otherwise use defaults.
	// (Continentalness noise params are built inside FInfinitePlaneWorldMode::ComputeEffectiveTerrainParams
	// — the single source of truth shared with the modes' analytic height.)
	if (BiomeConfig)
	{
		TempNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->TemperatureSeedOffset;
		TempNoiseParams.Frequency = BiomeConfig->TemperatureNoiseFrequency;
		MoistureNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->MoistureSeedOffset;
		MoistureNoiseParams.Frequency = BiomeConfig->MoistureNoiseFrequency;
	}
	else
	{
		// Defaults matching original FVoxelBiomeRegistry behavior
		TempNoiseParams.Seed = Request.NoiseParams.Seed + 1234;
		TempNoiseParams.Frequency = 0.00005f;
		MoistureNoiseParams.Seed = Request.NoiseParams.Seed + 5678;
		MoistureNoiseParams.Frequency = 0.00007f;
	}

	for (int32 Z = 0; Z < ChunkSize; ++Z)
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				// Calculate world position for this voxel (using base VoxelSize)
				FVector WorldPos = ChunkWorldPos + FVector(
					X * VoxelSize,
					Y * VoxelSize,
					Z * VoxelSize
				);

				// Sample 2D noise at X,Y (Z=0 for heightmap)
				float NoiseValue = FInfinitePlaneWorldMode::SampleTerrainNoise2D(
					WorldPos.X, WorldPos.Y, Request.NoiseParams);

				// Continentalness height modulation — shared with the analytic surface query (the modes' own height)
				// so spawn / nav / POI placement matches this generated surface. The sampled
				// continentalness is reused for the biome blend below.
				float Continentalness = 0.0f;
				const FWorldModeTerrainParams EffectiveParams =
					FInfinitePlaneWorldMode::ComputeEffectiveTerrainParams(
						WorldPos.X, WorldPos.Y, TerrainParams,
						Request.NoiseParams, &BiomeSnapshot, Continentalness);

				// Get terrain height from noise (using potentially modulated params); the island's edge falloff composes
				// on it (FIslandBowlWorldMode::ComposeTerrainHeight), as its analytic height does.
				float TerrainHeight = FInfinitePlaneWorldMode::NoiseToTerrainHeight(
					NoiseValue, EffectiveParams);
				if (IslandParams)
				{
					TerrainHeight = FIslandBowlWorldMode::ComposeTerrainHeight(
						WorldPos.X, WorldPos.Y, TerrainHeight, *IslandParams);
				}

				// Phase 6c: blend the natural height toward any terrain conditioning zones
				// (flatten under POIs / claims). Deterministic — the base terrain IS flat here.
				if (Request.ConditioningZones.Num() > 0)
				{
					TerrainHeight = FVoxelTerrainConditioning::ApplyToHeight(
						WorldPos.X, WorldPos.Y, TerrainHeight, Request.ConditioningZones);
				}

				// Calculate signed distance to surface
				float SignedDistance = FInfinitePlaneWorldMode::CalculateSignedDistance(
					WorldPos.Z, TerrainHeight);

				// Convert to density
				uint8 Density = FInfinitePlaneWorldMode::SignedDistanceToDensity(
					SignedDistance, VoxelSize);

				// Calculate depth below surface for material assignment (in voxels)
				float DepthBelowSurface = (TerrainHeight - WorldPos.Z) / VoxelSize;

				// Save pre-cave density to detect cave carving (solid → air transition)
				const uint8 PreCaveDensity = Density;

				// Determine material and biome
				uint8 MaterialID = 0;
				uint8 BiomeID = 0;

				if (Request.bEnableBiomes && BiomeConfig && BiomeConfig->IsValid())
				{
					// Sample biome noise at X,Y (2D, constant for a column)
					// Use 3D function with Z=0 for 2D sampling
					FVector BiomeSamplePos(WorldPos.X, WorldPos.Y, 0.0f);
					float Temperature = FBM3D(BiomeSamplePos, TempNoiseParams);
					float Moisture = FBM3D(BiomeSamplePos, MoistureNoiseParams);

					// Get blended biome selection for smooth transitions
					FBiomeBlend Blend = BiomeConfig->GetBiomeBlend(Temperature, Moisture, Continentalness);

					// Store the dominant biome ID
					BiomeID = Blend.GetDominantBiome();

					// Cave carving: subtract density for underground cavities
					float CaveDensity = 0.0f;
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						const bool bUnderwater = Request.bEnableWaterLevel && TerrainHeight < Request.WaterLevel;
						CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}

					// Get material considering blend weights and water level
					if (Request.bEnableWaterLevel)
					{
						MaterialID = BiomeConfig->GetBlendedMaterialWithWater(
							Blend, DepthBelowSurface, TerrainHeight, Request.WaterLevel, FVoxelBlendDitherKey(DitherBase + FIntVector(X, Y, Z), DitherSeed));
					}
					else
					{
						MaterialID = BiomeConfig->GetBlendedMaterial(Blend, DepthBelowSurface, FVoxelBlendDitherKey(DitherBase + FIntVector(X, Y, Z), DitherSeed));
					}

					// Apply height-based material overrides (snow at peaks, rock at altitude, etc.)
					// Height rules still apply after water (snow on peaks even if base is underwater)
					MaterialID = BiomeConfig->ApplyHeightMaterialRules(MaterialID, WorldPos.Z, DepthBelowSurface);

					// Cave wall material override (solid voxels near cave boundaries)
					if (Request.bEnableCaves && Request.CaveConfiguration
						&& Request.CaveConfiguration->bOverrideCaveWallMaterial
						&& CaveDensity > 0.0f && CaveDensity < 1.0f
						&& Density >= VOXEL_SURFACE_THRESHOLD
						&& DepthBelowSurface >= Request.CaveConfiguration->CaveWallMaterialMinDepth)
					{
						MaterialID = Request.CaveConfiguration->CaveWallMaterialID;
					}

					// Apply ore vein overrides (only for solid voxels well below surface)
					// Use depth > 10 to ensure ores aren't visible on smooth terrain surfaces
					// (smooth mesher scans up to 8 voxels for material selection)
					if (Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 10.0f)
					{
						TArray<FOreVeinConfig> ApplicableOres;
						BiomeConfig->GetOreVeinsForBiome(BiomeID, ApplicableOres);

						uint8 OreMaterial = 0;
						if (CheckOreVeinPlacement(WorldPos, DepthBelowSurface, ApplicableOres, Request.NoiseParams.Seed, OreMaterial))
						{
							MaterialID = OreMaterial;
						}
					}
				}
				else if (Request.bEnableBiomes)
				{
					// Fallback to static registry if no BiomeConfiguration provided
					FVector BiomeSamplePos(WorldPos.X, WorldPos.Y, 0.0f);
					float Temperature = FBM3D(BiomeSamplePos, TempNoiseParams);
					float Moisture = FBM3D(BiomeSamplePos, MoistureNoiseParams);

					FBiomeBlend Blend = FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f);
					BiomeID = Blend.GetDominantBiome();
					MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, DepthBelowSurface, FVoxelBlendDitherKey(DitherBase + FIntVector(X, Y, Z), DitherSeed));

					// Cave carving for fallback path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						const bool bUnderwater = Request.bEnableWaterLevel && TerrainHeight < Request.WaterLevel;
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}
				else
				{
					// Legacy behavior: use world mode's material assignment
					MaterialID = WorldMode.GetMaterialAtDepth(WorldPos, TerrainHeight, DepthBelowSurface * VoxelSize);

					// Cave carving for non-biome path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						const bool bUnderwater = Request.bEnableWaterLevel && TerrainHeight < Request.WaterLevel;
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, 0, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}

				int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;

				// Set cave flag and underground flag if cave carving converted solid to air.
				// Cave flag: temporary barrier for water fill (cleared in Phase 3).
				// Underground flag: persistent — used by scatter filtering and water plane.
				const bool bCaveCarved = (PreCaveDensity >= VOXEL_SURFACE_THRESHOLD && Density < VOXEL_SURFACE_THRESHOLD);
				const uint8 InitMetadata = bCaveCarved
					? ((FVoxelData::VOXEL_FLAG_CAVE | FVoxelData::VOXEL_FLAG_UNDERGROUND) << 4)
					: 0;
				OutVoxelData[Index] = FVoxelData(MaterialID, Density, BiomeID, InitMetadata);
			}
		}
	}
}

float FVoxelCPUNoiseGenerator::SampleNoiseAt(
	const FVector& WorldPosition,
	const FVoxelNoiseParams& Params)
{
	return FBM3D(WorldPosition, Params);
}

bool FVoxelCPUNoiseGenerator::ReadbackToCPU(
	const FVoxelGenerationHandle& Handle,
	TArray<FVoxelData>& OutVoxelData)
{
	if (!Handle.IsValid() || !Handle.bWasSuccessful)
	{
		return false;
	}

	FScopeLock Lock(&ResultsLock);
	TArray<FVoxelData>* StoredData = StoredResults.Find(Handle.RequestId);
	if (StoredData)
	{
		OutVoxelData = *StoredData;
		return true;
	}

	return false;
}

void FVoxelCPUNoiseGenerator::ReleaseHandle(const FVoxelGenerationHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	FScopeLock Lock(&ResultsLock);
	StoredResults.Remove(Handle.RequestId);
}

float FVoxelCPUNoiseGenerator::FBM3D(const FVector& Position, const FVoxelNoiseParams& Params)
{
	// One noise for every world mode (world modes review MP4, WM-MP6 / WM-MP7, decision D39; GenerationVersion 3): the
	// hashed lattice with split-origin sampling and the continuous Simplex kernel the planet has had since spherical Phase
	// 6 and G43. The heightmap fields (terrain, continentalness, climate, caves, ores, the surface queries, the map) all
	// come through here, so they have no 256-unit period, are exact to float precision at any distance from the origin
	// and do not step across the simplex lattice's boundaries. The classic table noise lives with the tests that measure
	// what it did (Tests/VoxelClassicNoiseControl; CR1, CORE-02).
	return FVoxelNoise::FBM3D(Position, Params);
}

// ==================== Cave Generation Helpers ====================

float FVoxelCPUNoiseGenerator::SampleCaveLayer(const FVector& WorldPos, const FCaveLayerConfig& LayerConfig, int32 WorldSeed,
	const FSphericalPlanetParams* Planet)
{
	// Apply vertical scale to flatten caves horizontally: along world Z in heightmap modes, along local up on a planet
	// (the altitude above the datum is scaled, the direction kept; SP-B6). A planet's caves sample the planet noise
	// relative to its centre (WM-BP5, D22): exact at any radius, no period, and they move with the planet.
	const FVector ScaledPos = Planet
		? FSphericalPlanetWorldMode::ScaleAltitudeRelative(WorldPos - Planet->PlanetCenter, Planet->PlanetRadius, LayerConfig.VerticalScale)
		: FVector(WorldPos.X, WorldPos.Y, WorldPos.Z * LayerConfig.VerticalScale);
	auto SampleField = [Planet, &ScaledPos](const FVoxelNoiseParams& Params)
	{
		return Planet ? FVoxelNoise::FBM3D(ScaledPos, Params) : FBM3D(ScaledPos, Params);
	};

	// Build noise params for first noise field
	FVoxelNoiseParams CaveNoiseParams;
	CaveNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	CaveNoiseParams.Seed = WorldSeed + LayerConfig.SeedOffset;
	CaveNoiseParams.Frequency = LayerConfig.Frequency;
	CaveNoiseParams.Octaves = LayerConfig.Octaves;
	CaveNoiseParams.Persistence = LayerConfig.Persistence;
	CaveNoiseParams.Lacunarity = LayerConfig.Lacunarity;
	CaveNoiseParams.Amplitude = 1.0f;

	if (LayerConfig.CaveType == ECaveType::Cheese)
	{
		// Cheese caves: single noise field, carve where noise > threshold
		float Noise = SampleField(CaveNoiseParams);

		// Noise is in [-1, 1], map threshold to that range
		if (Noise <= LayerConfig.Threshold)
		{
			return 0.0f;
		}

		// Smooth falloff above threshold
		float Excess = Noise - LayerConfig.Threshold;
		float FalloffRange = FMath::Max(LayerConfig.CarveFalloff, 0.01f);
		float CarveDensity = FMath::Clamp(Excess / FalloffRange, 0.0f, 1.0f);

		return CarveDensity * LayerConfig.CarveStrength;
	}
	else
	{
		// Spaghetti and Noodle: dual-noise intersection
		// Tunnel forms where BOTH noise fields are near zero simultaneously
		float Noise1 = SampleField(CaveNoiseParams);

		// Second noise field with offset seed and scaled frequency
		FVoxelNoiseParams SecondNoiseParams = CaveNoiseParams;
		SecondNoiseParams.Seed = WorldSeed + LayerConfig.SecondNoiseSeedOffset;
		SecondNoiseParams.Frequency = LayerConfig.Frequency * LayerConfig.SecondNoiseFrequencyScale;

		float Noise2 = SampleField(SecondNoiseParams);

		// Both noise fields must be within [-Threshold, Threshold] for a tunnel
		float AbsNoise1 = FMath::Abs(Noise1);
		float AbsNoise2 = FMath::Abs(Noise2);

		if (AbsNoise1 >= LayerConfig.Threshold || AbsNoise2 >= LayerConfig.Threshold)
		{
			return 0.0f;
		}

		// Carve density is stronger as both approach zero
		float FalloffRange = FMath::Max(LayerConfig.CarveFalloff, 0.01f);
		float Carve1 = FMath::Clamp(1.0f - (AbsNoise1 / LayerConfig.Threshold), 0.0f, 1.0f);
		float Carve2 = FMath::Clamp(1.0f - (AbsNoise2 / LayerConfig.Threshold), 0.0f, 1.0f);

		// Multiply for intersection — both must be near zero
		float CarveDensity = Carve1 * Carve2;

		// Apply smooth falloff
		CarveDensity = FMath::SmoothStep(0.0f, FalloffRange, CarveDensity);

		return CarveDensity * LayerConfig.CarveStrength;
	}
}

float FVoxelCPUNoiseGenerator::CalculateCaveDensity(
	const FVector& WorldPos,
	float DepthBelowSurface,
	uint8 BiomeID,
	const UVoxelCaveConfiguration* CaveConfig,
	int32 WorldSeed,
	bool bIsUnderwater,
	const FSphericalPlanetParams* Planet)
{
	if (!CaveConfig || !CaveConfig->bEnableCaves)
	{
		return 0.0f;
	}

	// Get biome scaling
	float BiomeCaveScale = CaveConfig->GetBiomeCaveScale(BiomeID);
	if (BiomeCaveScale <= 0.0f)
	{
		return 0.0f;
	}

	float BiomeMinDepthOverride = CaveConfig->GetBiomeMinDepthOverride(BiomeID);

	// Apply underwater min depth — catches biome transition zones where the
	// ocean biome isn't assigned but terrain is still below water level
	if (bIsUnderwater && CaveConfig->UnderwaterMinDepth > 0.0f)
	{
		if (BiomeMinDepthOverride < 0.0f)
		{
			BiomeMinDepthOverride = CaveConfig->UnderwaterMinDepth;
		}
		else
		{
			BiomeMinDepthOverride = FMath::Max(BiomeMinDepthOverride, CaveConfig->UnderwaterMinDepth);
		}
	}

	float MaxCarveDensity = 0.0f;

	for (const FCaveLayerConfig& Layer : CaveConfig->CaveLayers)
	{
		if (!Layer.bEnabled)
		{
			continue;
		}

		// Determine effective min depth (biome override or layer default)
		float EffectiveMinDepth = (BiomeMinDepthOverride >= 0.0f) ? BiomeMinDepthOverride : Layer.MinDepth;

		// Check depth constraints
		if (DepthBelowSurface < EffectiveMinDepth - Layer.DepthFadeWidth)
		{
			continue;
		}

		if (Layer.MaxDepth > 0.0f && DepthBelowSurface > Layer.MaxDepth + Layer.DepthFadeWidth)
		{
			continue;
		}

		// Sample this cave layer
		float LayerCarve = SampleCaveLayer(WorldPos, Layer, WorldSeed, Planet);

		if (LayerCarve <= 0.0f)
		{
			continue;
		}

		// Apply depth fade at MinDepth boundary
		if (DepthBelowSurface < EffectiveMinDepth)
		{
			float FadeT = (DepthBelowSurface - (EffectiveMinDepth - Layer.DepthFadeWidth)) / Layer.DepthFadeWidth;
			LayerCarve *= FMath::SmoothStep(0.0f, 1.0f, FadeT);
		}

		// Apply depth fade at MaxDepth boundary
		if (Layer.MaxDepth > 0.0f && DepthBelowSurface > Layer.MaxDepth)
		{
			float FadeT = 1.0f - (DepthBelowSurface - Layer.MaxDepth) / Layer.DepthFadeWidth;
			LayerCarve *= FMath::SmoothStep(0.0f, 1.0f, FadeT);
		}

		// Union composition: take the maximum carve from any layer
		MaxCarveDensity = FMath::Max(MaxCarveDensity, LayerCarve);
	}

	// Apply biome scaling
	return FMath::Clamp(MaxCarveDensity * BiomeCaveScale, 0.0f, 1.0f);
}

// ==================== Ore Vein Helpers ====================

float FVoxelCPUNoiseGenerator::SampleOreVeinNoise(const FVector& WorldPos, const FOreVeinConfig& OreConfig, int32 WorldSeed)
{
	// Create ore-specific noise params
	FVoxelNoiseParams OreNoiseParams;
	OreNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	OreNoiseParams.Seed = WorldSeed + OreConfig.SeedOffset;
	OreNoiseParams.Frequency = OreConfig.Frequency;
	OreNoiseParams.Octaves = 2;  // Simple noise for ores
	OreNoiseParams.Persistence = 0.5f;
	OreNoiseParams.Lacunarity = 2.0f;
	OreNoiseParams.Amplitude = 1.0f;

	FVector SamplePos = WorldPos;

	// A streak squashes the position along one direction per ore type (GetPlanetOreSamplePoint, the planet's linear map;
	// MP4 gives heightmap worlds the same: the direction formula that varied with the absolute position turned into
	// white noise far from the origin, and a linear map lets the GPU sample the ore at a chunk origin + the voxel's offset).
	SamplePos = GetPlanetOreSamplePoint(WorldPos, OreConfig);

	// Sample noise
	float NoiseValue = FBM3D(SamplePos, OreNoiseParams);

	// Normalize from [-1, 1] to [0, 1]
	return (NoiseValue + 1.0f) * 0.5f;
}

FVector FVoxelCPUNoiseGenerator::GetPlanetOreStreakDirection(const FOreVeinConfig& OreConfig)
{
	// The heightmap streak's direction formula at the origin: one direction per ore type (WM-BP5). A direction that varies
	// with the absolute position (the heightmap form) turns into white noise R-sized distances from the origin.
	const double StreakSeed = static_cast<double>(OreConfig.SeedOffset) * 0.1;
	return FVector(FMath::Sin(StreakSeed), FMath::Cos(StreakSeed * 1.5), FMath::Sin(StreakSeed * 2.0)).GetSafeNormal();
}

FVector FVoxelCPUNoiseGenerator::GetPlanetOreSamplePoint(const FVector& PlanetRelativePos, const FOreVeinConfig& OreConfig)
{
	if (OreConfig.Shape != EOreVeinShape::Streak)
	{
		return PlanetRelativePos;
	}
	// Squash the position along the streak direction by StreakStretch (the heightmap streak's projection): a linear map, so
	// a chunk's origin and its voxels' offsets map separately (the GPU's split origin).
	const FVector Dir = GetPlanetOreStreakDirection(OreConfig);
	const double Along = FVector::DotProduct(PlanetRelativePos, Dir);
	return PlanetRelativePos - Dir * (Along * (1.0 - 1.0 / FMath::Max(OreConfig.StreakStretch, 1.0f)));
}

float FVoxelCPUNoiseGenerator::SamplePlanetOreVeinNoise(const FVector& PlanetRelativePos, const FOreVeinConfig& OreConfig, int32 WorldSeed)
{
	// The heightmap ore field (2-octave Simplex at the ore's frequency and seed) by the planet noise, relative to the centre
	const FVoxelNoiseParams Params = FVoxelNoise::MakeFieldParams(WorldSeed + OreConfig.SeedOffset, OreConfig.Frequency);
	return (FVoxelNoise::FBM3D(GetPlanetOreSamplePoint(PlanetRelativePos, OreConfig), Params) + 1.0f) * 0.5f;
}

FIntVector FVoxelCPUNoiseGenerator::OreRarityKey(const FVector& PlanetRelativePos)
{
	return FIntVector(
		FMath::FloorToInt32(PlanetRelativePos.X + 0.5),
		FMath::FloorToInt32(PlanetRelativePos.Y + 0.5),
		FMath::FloorToInt32(PlanetRelativePos.Z + 0.5));
}

float FVoxelCPUNoiseGenerator::OreRarityHash(const FIntVector& Key, int32 Seed)
{
	// Mirrored bit for bit by GpuOreRarityHashKey (BiomeMaterial.ush): the shared hash's top 24 bits as a unit float.
	return VoxelHash::Unit(VoxelHash::Lattice(static_cast<uint32>(Key.X), static_cast<uint32>(Key.Y), static_cast<uint32>(Key.Z), Seed));
}

bool FVoxelCPUNoiseGenerator::CheckOreVeinPlacement(
	const FVector& WorldPos,
	float DepthBelowSurface,
	const TArray<FOreVeinConfig>& OreConfigs,
	int32 WorldSeed,
	uint8& OutMaterialID,
	const FVector* PlanetRelativePos)
{
	// Check each ore type in priority order (assumed to be pre-sorted)
	for (const FOreVeinConfig& OreConfig : OreConfigs)
	{
		// Check depth constraints
		if (!OreConfig.IsValidDepth(DepthBelowSurface))
		{
			continue;
		}

		// Sample ore noise at this position: on a planet the planet noise relative to its centre (WM-BP5)
		float OreNoise = PlanetRelativePos
			? SamplePlanetOreVeinNoise(*PlanetRelativePos, OreConfig, WorldSeed)
			: SampleOreVeinNoise(WorldPos, OreConfig, WorldSeed);

		// Check against threshold
		if (OreNoise >= OreConfig.Threshold)
		{
			// Apply rarity check (if rarity < 1, randomly skip some valid placements)
			if (OreConfig.Rarity < 1.0f)
			{
				// A deterministic value per voxel: an exact integer hash of the position rounded to whole uu, relative to
				// the planet centre on a planet (SP-H3) and absolute on a heightmap world (MP4; before it, frac(sin(p . k)),
				// which the GPU's float and the CPU's double rounded differently)
				const float RandomValue = OreRarityHash(OreRarityKey(PlanetRelativePos ? *PlanetRelativePos : WorldPos), WorldSeed);
				if (RandomValue > OreConfig.Rarity)
				{
					continue;
				}
			}

			OutMaterialID = OreConfig.MaterialID;
			return true;
		}
	}

	return false;
}

void FVoxelCPUNoiseGenerator::GenerateChunkSphericalPlanet(
	const FVoxelNoiseGenerationRequest& Request,
	const FSphericalPlanetWorldMode& WorldMode,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();
	// The blend zones' dither keys (G39): the chunk's first voxel on the world's grid, plus each voxel's local index.
	const FIntVector DitherBase = FVoxelBiomeBlendDither::GridBase(Request.ChunkCoord, ChunkSize, Request.WorldOrigin, VoxelSize);
	const int32 DitherSeed = Request.NoiseParams.Seed;
	const FSphericalPlanetParams& PlanetParams = WorldMode.GetPlanetParams();
	const FVector PlanetCenter = PlanetParams.PlanetCenter;
	const double PlanetRadius = PlanetParams.PlanetRadius;
	// The chunk origin relative to the centre, in double: the ore Rarity hash keys on it (SP-H3), as the GPU's
	// PlanetChunkOffset does.
	const FVector ChunkPlanetRelative = ChunkWorldPos - PlanetCenter;
	// The sea as an altitude (WM-BP4): every comparison with the terrain is between altitudes, never between two radii of
	// size R in float.
	const double WaterAltitude = Request.WaterRadius - PlanetRadius;

	// Get biome configuration (may be null if biomes disabled)
	const UVoxelBiomeConfiguration* BiomeConfig = Request.BiomeConfiguration;

	// Hoisted continentalness snapshot for the per-voxel radial height (plain data), as in the heightmap modes: the
	// planet's surface function (FSphericalPlanetWorldMode::SampleRadialDisplacement) takes it, and GetSurfacePoint
	// applies the same.
	const FVoxelBiomeSnapshot BiomeSnapshot = FVoxelBiomeSnapshot::FromConfig(BiomeConfig);

	// Climate: the heightmap modes' 2-octave fields (seeds and frequencies), sampled by the planet noise (WM-BP5)
	const FVoxelNoiseParams TempNoiseParams = BiomeConfig
		? FVoxelNoise::MakeFieldParams(Request.NoiseParams.Seed + BiomeConfig->TemperatureSeedOffset, BiomeConfig->TemperatureNoiseFrequency)
		: FVoxelNoise::MakeFieldParams(Request.NoiseParams.Seed + 1234, 0.00005f);
	const FVoxelNoiseParams MoistureNoiseParams = BiomeConfig
		? FVoxelNoise::MakeFieldParams(Request.NoiseParams.Seed + BiomeConfig->MoistureSeedOffset, BiomeConfig->MoistureNoiseFrequency)
		: FVoxelNoise::MakeFieldParams(Request.NoiseParams.Seed + 5678, 0.00007f);

	for (int32 Z = 0; Z < ChunkSize; ++Z)
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				// Calculate world position for this voxel
				FVector WorldPos = ChunkWorldPos + FVector(
					X * VoxelSize,
					Y * VoxelSize,
					Z * VoxelSize
				);

				// The voxel relative to the centre, its distance and direction, all in double (WM-BP4)
				const FVector PlanetRelativePos = ChunkPlanetRelative + FVector(X * VoxelSize, Y * VoxelSize, Z * VoxelSize);
				const double DistFromCenter = PlanetRelativePos.Size();
				const FVector Direction = DistFromCenter > UE_KINDA_SMALL_NUMBER ? PlanetRelativePos / DistFromCenter : FVector::UpVector;

				// The surface along this direction: terrain noise and continentalness sampled on the datum sphere, the
				// displacement modulated by continentalness as in the heightmap modes (SP-B1, SP-B5). The sampled
				// continentalness is reused for the biome blend below.
				float Continentalness = 0.0f;
				float TerrainAltitude = FSphericalPlanetWorldMode::SampleRadialDisplacement(
					Direction, PlanetRadius, WorldMode.GetTerrainParams(), Request.NoiseParams, &BiomeSnapshot, &Continentalness);

				// Terrain conditioning (spherical SP-B10, D15): the planet zones (caps on the datum sphere) blend the altitude
				// along this direction toward their targets before the SDF, as the heightmap generators blend the column
				// height, so density, depth, the underwater test, caves and materials all follow the flattened ground.
				if (Request.ConditioningZones.Num() > 0)
				{
					TerrainAltitude = FVoxelTerrainConditioning::ApplyToAltitude(Direction, PlanetRadius, TerrainAltitude, Request.ConditioningZones);
				}
				// The voxel's altitude above the datum, |P - C| - R in double (WM-BP4): the signed distance, the depth and
				// the height rules all read altitudes, quantities of the relief's size, so nothing of size R cancels in
				// float (at R 1e8 a float radius is 8 uu coarse: 13 density steps of terraces).
				const double VoxelAltitude = DistFromCenter - PlanetRadius;

				// Signed distance to the surface (with the shell's quick reject), then density
				const float SignedDistance = FSphericalPlanetWorldMode::ShellSignedDistanceAtAltitude(VoxelAltitude, TerrainAltitude, PlanetParams);
				uint8 Density = FInfinitePlaneWorldMode::SignedDistanceToDensity(SignedDistance, VoxelSize);

				// Depth below surface is radial (terrain altitude - voxel altitude)
				float DepthBelowSurface = static_cast<float>((static_cast<double>(TerrainAltitude) - VoxelAltitude) / VoxelSize);

				// Height-material rules read the voxel's altitude above the datum, as they read Z in the heightmap modes
				// (SP-B2; the radius put every surface past the snow line). Caves under the sea keep their underwater
				// minimum depth (SP-B6).
				const float Altitude = static_cast<float>(VoxelAltitude);
				const bool bUnderwater = Request.bEnableWaterLevel && TerrainAltitude < WaterAltitude;

				// Save pre-cave density to detect cave carving (solid → air transition)
				const uint8 PreCaveDensity = Density;

				// Determine material and biome
				uint8 MaterialID = 0;
				uint8 BiomeID = 0;

				if (Request.bEnableBiomes && BiomeConfig && BiomeConfig->IsValid())
				{
					// Temperature and moisture are sampled like the terrain, on the datum sphere, so biomes keep their size
					// in uu whatever the radius (SP-B1)
					float Temperature, Moisture;
					FSphericalPlanetWorldMode::SampleClimate(Direction, PlanetRadius, TempNoiseParams, MoistureNoiseParams, Temperature, Moisture);

					FBiomeBlend Blend = BiomeConfig->GetBiomeBlend(Temperature, Moisture, Continentalness);
					BiomeID = Blend.GetDominantBiome();

					// Cave carving: subtract density for underground cavities
					float CaveDensity = 0.0f;
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, &PlanetParams);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}

					// Get material considering blend weights and water level
					// For spherical planets: underwater if the terrain's altitude is under the sea's (WM-BP4: altitudes)
					if (Request.bEnableWaterLevel)
					{
						MaterialID = BiomeConfig->GetBlendedMaterialWithWater(
							Blend, DepthBelowSurface, TerrainAltitude, static_cast<float>(WaterAltitude),
							FVoxelBlendDitherKey(DitherBase + FIntVector(X, Y, Z), DitherSeed));
					}
					else
					{
						MaterialID = BiomeConfig->GetBlendedMaterial(Blend, DepthBelowSurface, FVoxelBlendDitherKey(DitherBase + FIntVector(X, Y, Z), DitherSeed));
					}

					// Height rules by altitude above the datum (SP-B2)
					MaterialID = BiomeConfig->ApplyHeightMaterialRules(MaterialID, Altitude, DepthBelowSurface);

					// Cave wall material override (solid voxels near cave boundaries)
					if (Request.bEnableCaves && Request.CaveConfiguration
						&& Request.CaveConfiguration->bOverrideCaveWallMaterial
						&& CaveDensity > 0.0f && CaveDensity < 1.0f
						&& Density >= VOXEL_SURFACE_THRESHOLD
						&& DepthBelowSurface >= Request.CaveConfiguration->CaveWallMaterialMinDepth)
					{
						MaterialID = Request.CaveConfiguration->CaveWallMaterialID;
					}

					// Apply ore vein overrides (only for solid voxels well below surface)
					// Use depth > 10 to ensure ores aren't visible on smooth terrain surfaces
					// (smooth mesher scans up to 8 voxels for material selection)
					if (Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 10.0f)
					{
						TArray<FOreVeinConfig> ApplicableOres;
						BiomeConfig->GetOreVeinsForBiome(BiomeID, ApplicableOres);

						uint8 OreMaterial = 0;
						if (CheckOreVeinPlacement(WorldPos, DepthBelowSurface, ApplicableOres, Request.NoiseParams.Seed, OreMaterial, &PlanetRelativePos))
						{
							MaterialID = OreMaterial;
						}
					}
				}
				else if (Request.bEnableBiomes)
				{
					float Temperature, Moisture;
					FSphericalPlanetWorldMode::SampleClimate(Direction, PlanetRadius, TempNoiseParams, MoistureNoiseParams, Temperature, Moisture);

					FBiomeBlend Blend = FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f);
					BiomeID = Blend.GetDominantBiome();
					MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, DepthBelowSurface, FVoxelBlendDitherKey(DitherBase + FIntVector(X, Y, Z), DitherSeed));

					// Cave carving for fallback path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, &PlanetParams);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}
				else
				{
					MaterialID = WorldMode.GetMaterialAtDepth(WorldPos, TerrainAltitude, DepthBelowSurface * VoxelSize);

					// Cave carving for non-biome path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, 0, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, &PlanetParams);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}

				int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;

				const bool bCaveCarved = (PreCaveDensity >= VOXEL_SURFACE_THRESHOLD && Density < VOXEL_SURFACE_THRESHOLD);
				const uint8 InitMetadata = bCaveCarved
					? ((FVoxelData::VOXEL_FLAG_CAVE | FVoxelData::VOXEL_FLAG_UNDERGROUND) << 4)
					: 0;
				OutVoxelData[Index] = FVoxelData(MaterialID, Density, BiomeID, InitMetadata);
			}
		}
	}
}

// ==================== Water Fill Pass ====================

void FVoxelCPUNoiseGenerator::ApplyPostReadbackPasses(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	// Same order as GenerateChunkCPU: water fill first (uses the per-voxel cave flags as barriers and
	// clears them), then underground classification.
	ApplyWaterFillPass(Request, OutVoxelData);
	ApplyUndergroundClassificationPass(Request, OutVoxelData);
}

void FVoxelCPUNoiseGenerator::ApplyWaterFillPass(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	if (!Request.bEnableWaterLevel)
		return;

	// A planet's sea is the sphere of WaterRadius (SP-B8): the same three phases with the radius in place of the plane.
	if (Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		ApplyRadialWaterFillPass(Request, OutVoxelData);
		return;
	}

	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();
	const float WaterLevel = Request.WaterLevel;
	const int32 SliceSize = ChunkSize * ChunkSize;
	const int32 VolumeSize = SliceSize * ChunkSize;

	// === Phase 1: Column scan — seed water in open ocean ===
	// Cave-flagged voxels are treated as solid barriers so the column scan
	// doesn't flood through cave openings in the seabed.
	const float ChunkTopZ = ChunkWorldPos.Z + ChunkSize * VoxelSize;
	const bool bChunkContainsWaterLevel = (WaterLevel >= ChunkWorldPos.Z && WaterLevel < ChunkTopZ);

	if (bChunkContainsWaterLevel)
	{
		const int32 WaterZ = FMath::Clamp(
			FMath::FloorToInt32((WaterLevel - ChunkWorldPos.Z) / VoxelSize),
			0, ChunkSize - 1);

		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				// When WaterZ < ChunkSize - 1, verify the column is open from
				// the chunk top down to the water level. Solid or cave-flagged
				// voxels above water level mean this column is underground.
				if (WaterZ < ChunkSize - 1)
				{
					bool bColumnOpen = true;
					for (int32 Z = ChunkSize - 1; Z > WaterZ; --Z)
					{
						int32 Index = X + Y * ChunkSize + Z * SliceSize;
						if (OutVoxelData[Index].IsSolid() || OutVoxelData[Index].HasCaveFlag())
						{
							bColumnOpen = false;
							break;
						}
					}
					if (!bColumnOpen)
						continue;
				}

				// Scan from water level downward: flag air, stop at solid or cave barrier
				for (int32 Z = WaterZ; Z >= 0; --Z)
				{
					int32 Index = X + Y * ChunkSize + Z * SliceSize;
					FVoxelData& Voxel = OutVoxelData[Index];

					if (Voxel.IsSolid() || Voxel.HasCaveFlag())
						break;

					Voxel.SetWaterFlag(true);
				}
			}
		}
	}

	// === Phase 2: BFS flood fill — propagate water through non-cave air ===
	// Cave-flagged voxels act as barriers so water stays in the open ocean
	// and doesn't leak into cave interiors.
	TArray<int32> BFSQueue;
	BFSQueue.Reserve(SliceSize);

	for (int32 i = 0; i < VolumeSize; ++i)
	{
		if (OutVoxelData[i].HasWaterFlag())
		{
			BFSQueue.Add(i);
		}
	}

	static constexpr int32 DX[6] = { 1, -1,  0,  0,  0,  0 };
	static constexpr int32 DY[6] = { 0,  0,  1, -1,  0,  0 };
	static constexpr int32 DZ[6] = { 0,  0,  0,  0,  1, -1 };

	int32 QueueHead = 0;
	while (QueueHead < BFSQueue.Num())
	{
		const int32 CurrentIdx = BFSQueue[QueueHead++];
		const int32 CZ = CurrentIdx / SliceSize;
		const int32 CY = (CurrentIdx - CZ * SliceSize) / ChunkSize;
		const int32 CX = CurrentIdx - CZ * SliceSize - CY * ChunkSize;

		for (int32 D = 0; D < 6; ++D)
		{
			const int32 NX = CX + DX[D];
			const int32 NY = CY + DY[D];
			const int32 NZ = CZ + DZ[D];

			if (NX < 0 || NX >= ChunkSize ||
				NY < 0 || NY >= ChunkSize ||
				NZ < 0 || NZ >= ChunkSize)
			{
				continue;
			}

			const int32 NeighborIdx = NX + NY * ChunkSize + NZ * SliceSize;
			FVoxelData& Neighbor = OutVoxelData[NeighborIdx];

			// Skip solid, already-flagged, and cave-carved voxels
			if (Neighbor.IsSolid() || Neighbor.HasWaterFlag() || Neighbor.HasCaveFlag())
				continue;

			const float NeighborWorldZ = ChunkWorldPos.Z + NZ * VoxelSize;
			if (NeighborWorldZ > WaterLevel)
				continue;

			Neighbor.SetWaterFlag(true);
			BFSQueue.Add(NeighborIdx);
		}
	}

	// === Phase 3: Clear cave flags ===
	// Cave flags were temporary barriers to prevent ocean water from flooding
	// cave interiors during the column scan and initial BFS. Now clear them
	// so cave voids become normal air.
	// Note: Phase 4 (cave re-flooding BFS) was removed — caves below water
	// level are intentionally kept dry so players can explore them on foot.
	for (int32 i = 0; i < VolumeSize; ++i)
	{
		if (OutVoxelData[i].HasCaveFlag())
		{
			OutVoxelData[i].SetCaveFlag(false);
		}
	}
}

void FVoxelCPUNoiseGenerator::ApplyRadialWaterFillPass(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();
	const int32 SliceSize = ChunkSize * ChunkSize;
	const int32 VolumeSize = SliceSize * ChunkSize;
	const FVoxelSeaSurface Sea = FVoxelSeaSurface::MakeSphere(Request.SphericalPlanetParams.PlanetCenter, Request.WaterRadius);
	auto VoxelWorldPos = [&](const FIntVector& Local)
	{
		return ChunkWorldPos + FVector(Local) * VoxelSize;
	};

	// === Phase 1: seed the open sea, in a chunk the sea surface passes through ===
	// As the plane's column scan: along each line in local down (FVoxelDownLines, the chunk centre's down), the part above
	// the sea must be clear of solid and cave voxels from where the line enters the chunk (the column is open from the
	// chunk top); from the sea surface down, air takes the water flag until the first solid or cave voxel. A line that
	// enters the chunk already below the sea counts as open, as a column does when the water plane is the chunk's top
	// layer. Chunks wholly under the sea fill from their neighbours (UVoxelChunkManager::PropagateWaterFromNeighbors). The
	// GPU's WaterLineSeedCS walks the same lines.
	const FBox VoxelBox(ChunkWorldPos, ChunkWorldPos + FVector((ChunkSize - 1) * VoxelSize));
	if (Sea.Crosses(VoxelBox))
	{
		struct FSeaLine
		{
			bool bBlocked = false; // solid or cave above the sea, or the flood met its floor
		};
		const FVoxelDownLines DownLines = FVoxelDownLines::Make(ChunkSize, FVoxelDownLines::PlanetChunkDown(Request));
		TArray<FSeaLine> Lines;
		Lines.SetNum(DownLines.NumLines());
		DownLines.ForEach([&](int32 LineId, int32 Index, const FIntVector& Local)
		{
			FSeaLine& Line = Lines[LineId];
			if (Line.bBlocked)
			{
				return;
			}
			FVoxelData& Voxel = OutVoxelData[Index];
			if (Voxel.IsSolid() || Voxel.HasCaveFlag())
			{
				Line.bBlocked = true;
				return;
			}
			if (Sea.IsBelow(VoxelWorldPos(Local)))
			{
				Voxel.SetWaterFlag(true);
			}
		});
	}

	// === Phase 2: BFS flood fill through non-cave air inside the sea radius ===
	TArray<int32> BFSQueue;
	BFSQueue.Reserve(SliceSize);
	for (int32 i = 0; i < VolumeSize; ++i)
	{
		if (OutVoxelData[i].HasWaterFlag())
		{
			BFSQueue.Add(i);
		}
	}

	static constexpr int32 DX[6] = { 1, -1,  0,  0,  0,  0 };
	static constexpr int32 DY[6] = { 0,  0,  1, -1,  0,  0 };
	static constexpr int32 DZ[6] = { 0,  0,  0,  0,  1, -1 };

	int32 QueueHead = 0;
	while (QueueHead < BFSQueue.Num())
	{
		const int32 CurrentIdx = BFSQueue[QueueHead++];
		const int32 CZ = CurrentIdx / SliceSize;
		const int32 CY = (CurrentIdx - CZ * SliceSize) / ChunkSize;
		const int32 CX = CurrentIdx - CZ * SliceSize - CY * ChunkSize;

		for (int32 D = 0; D < 6; ++D)
		{
			const FIntVector N(CX + DX[D], CY + DY[D], CZ + DZ[D]);
			if (N.X < 0 || N.X >= ChunkSize || N.Y < 0 || N.Y >= ChunkSize || N.Z < 0 || N.Z >= ChunkSize)
			{
				continue;
			}

			const int32 NeighborIdx = N.X + N.Y * ChunkSize + N.Z * SliceSize;
			FVoxelData& Neighbor = OutVoxelData[NeighborIdx];
			if (Neighbor.IsSolid() || Neighbor.HasWaterFlag() || Neighbor.HasCaveFlag() || !Sea.IsBelow(VoxelWorldPos(N)))
			{
				continue;
			}

			Neighbor.SetWaterFlag(true);
			BFSQueue.Add(NeighborIdx);
		}
	}

	// === Phase 3: clear the cave flags (temporary barriers), as the plane's pass does ===
	for (int32 i = 0; i < VolumeSize; ++i)
	{
		if (OutVoxelData[i].HasCaveFlag())
		{
			OutVoxelData[i].SetCaveFlag(false);
		}
	}
}

// ==================== Underground Classification Pass ====================

void FVoxelCPUNoiseGenerator::ApplyUndergroundClassificationPass(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	const int32 SliceSize = ChunkSize * ChunkSize;

	// Pass 1: Column scan to flag underground air not already flagged by cave carving.
	// State machine: scan top-down, require seeing open air BEFORE counting solid.
	// Solid voxels at the column top (before any air) are terrain surface geometry
	// (peaks, ridges, chunk-boundary terrain), NOT cave ceilings. Only begin
	// counting solid thickness after the first air gap, so the pattern must be:
	//   [optional solid at top] → air → solid (>= MinSolidThickness) → air = underground
	// This avoids falsely flagging surface air below ridges/overhangs as underground.
	const int32 MinSolidThickness = 3;

	// One scan line's state, top-down.
	struct FLineState
	{
		bool bSeenAir = false;
		bool bUnderground = false;
		int32 ConsecutiveSolid = 0;
	};
	auto Step = [MinSolidThickness](FLineState& Line, FVoxelData& Voxel)
	{
		if (Voxel.IsSolid())
		{
			// Only count solid after we've seen air (sky/surface)
			if (Line.bSeenAir)
			{
				++Line.ConsecutiveSolid;
			}
			return;
		}

		Line.bSeenAir = true;

		// Air voxel — decide if it's underground
		if (!Line.bUnderground && Line.ConsecutiveSolid >= MinSolidThickness)
		{
			// Passed through thick terrain ceiling — everything below is underground
			Line.bUnderground = true;
		}

		if (Line.bUnderground && !Voxel.HasWaterFlag())
		{
			Voxel.SetUndergroundFlag(true);
		}

		// Reset for next solid sequence (only matters while still above ground)
		if (!Line.bUnderground)
		{
			Line.ConsecutiveSolid = 0;
		}
	};

	if (Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		// A planet scans along local down (the chunk centre's), not world -Z (SP-B7): at mid latitudes down is up to 55
		// degrees off every axis, and a world-Z scan reads a hillside as a cave ceiling, or misses a cave whose column
		// runs into the ground to the chunk's top. The same state machine runs on each digital line along down
		// (FVoxelDownLines) from where it enters the chunk; the GPU's UndergroundLinesCS walks the same lines.
		const FVoxelDownLines DownLines = FVoxelDownLines::Make(ChunkSize, FVoxelDownLines::PlanetChunkDown(Request));
		TArray<FLineState> Lines;
		Lines.SetNum(DownLines.NumLines());
		DownLines.ForEach([&](int32 LineId, int32 Index, const FIntVector&)
		{
			Step(Lines[LineId], OutVoxelData[Index]);
		});
	}
	else
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				FLineState Line;
				for (int32 Z = ChunkSize - 1; Z >= 0; --Z)
				{
					Step(Line, OutVoxelData[X + Y * ChunkSize + Z * SliceSize]);
				}
			}
		}
	}

	// Pass 2: Propagate underground flag to solid boundary voxels.
	// Scatter surface points sit at solid-air boundaries. Flagging the solid
	// side ensures the scatter extractors find the flag whichever side of
	// the crossing they read.
	// Two iterations extend the flag deeper into solid geometry for corners
	// and edges where mesh interpolation places surface points further
	// from the cave air.
	// Guard: never flag a solid voxel that has a non-underground air neighbor.
	// Such voxels are surface-exposed (terrain face, cliff, overhang) and
	// flagging them would cause surface scatter to be classified underground.
	//
	// Each iteration is two-phase (collect, then apply) so it reads a consistent
	// pre-iteration snapshot: in-place flagging would let flags set earlier in the
	// same iteration cascade along the scan direction, making the effective depth
	// scan-order dependent — and impossible for the parallel GPU port
	// (VoxelPostPasses.usf UndergroundMark/Apply) to reproduce.
	const int32 PropagationDepth = 2;
	TArray<int32> ToFlag;
	for (int32 Iteration = 0; Iteration < PropagationDepth; ++Iteration)
	{
		ToFlag.Reset();
		for (int32 Z = 0; Z < ChunkSize; ++Z)
		{
			for (int32 Y = 0; Y < ChunkSize; ++Y)
			{
				for (int32 X = 0; X < ChunkSize; ++X)
				{
					const int32 Index = X + Y * ChunkSize + Z * SliceSize;
					const FVoxelData& Voxel = OutVoxelData[Index];

					if (!Voxel.IsSolid() || Voxel.HasUndergroundFlag())
						continue;

					// Check 6-connected neighbors
					const int32 Neighbors[6] = {
						(X > 0)              ? Index - 1 : -1,
						(X < ChunkSize - 1)  ? Index + 1 : -1,
						(Y > 0)              ? Index - ChunkSize : -1,
						(Y < ChunkSize - 1)  ? Index + ChunkSize : -1,
						(Z > 0)              ? Index - SliceSize : -1,
						(Z < ChunkSize - 1)  ? Index + SliceSize : -1,
					};

					bool bAdjacentToUnderground = false;
					bool bAdjacentToSurfaceAir = false;
					for (int32 N = 0; N < 6; ++N)
					{
						if (Neighbors[N] >= 0)
						{
							const FVoxelData& Nbr = OutVoxelData[Neighbors[N]];
							if (Nbr.HasUndergroundFlag())
							{
								bAdjacentToUnderground = true;
							}
							else if (!Nbr.IsSolid())
							{
								// Non-underground air = surface/sky exposure
								bAdjacentToSurfaceAir = true;
							}
						}
					}

					if (bAdjacentToUnderground && !bAdjacentToSurfaceAir)
					{
						ToFlag.Add(Index);
					}
				}
			}
		}

		for (const int32 Index : ToFlag)
		{
			OutVoxelData[Index].SetUndergroundFlag(true);
		}
	}
}
