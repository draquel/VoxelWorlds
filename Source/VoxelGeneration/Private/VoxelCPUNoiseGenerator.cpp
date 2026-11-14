// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUNoiseGenerator.h"
#include "VoxelHash.h"
#include "VoxelGeneration.h"
#include "VoxelTerrainConditioning.h"
#include "VoxelChunkLineSegments.h"
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

	FVoxelNoiseParams MoistureNoiseParams;
	MoistureNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	MoistureNoiseParams.Octaves = 2;
	MoistureNoiseParams.Persistence = 0.5f;
	MoistureNoiseParams.Lacunarity = 2.0f;

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

	// Terrain conditioning. The lines: the chunk's segment list (the floats the GPU uploads), evaluated once per XY
	// column here, since a line depends on (X, Y) alone and the loop below computes the height for every voxel: each
	// column's blends of the height and the surface the lines leave there (the band, the caves' minimum depth). A chunk
	// no line reaches pays the count test. The zones after them, per voxel as before.
	FVoxelChunkLineColumns LineColumns;
	if (Request.Conditioning.Lines.Num() > 0)
	{
		FVoxelChunkLineSegments LineSegments;
		FVoxelChunkLineSegments::Build(Request, LineSegments);
		LineSegments.EvaluateColumns(ChunkSize, VoxelSize, LineColumns);
	}
	const bool bLines = !LineColumns.IsEmpty();
	const bool bLineSurfaces = LineColumns.HasSurfaces();
	const bool bZones = Request.Conditioning.Zones.Num() > 0;
	const FVoxelChunkLineSurface NoLineSurface;

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

				// Phase 6c: blend the natural height toward the chunk's terrain conditioning (level along lines, flatten
				// under POIs / claims): the lines' blends at this column, then the zones. Deterministic — the base
				// terrain IS flat here.
				if (bLines)
				{
					TerrainHeight = LineColumns.Apply(X + Y * ChunkSize, TerrainHeight);
				}
				if (bZones)
				{
					TerrainHeight = Request.Conditioning.ApplyZonesToHeight(WorldPos.X, WorldPos.Y, TerrainHeight);
				}
				// The surface the lines leave at this column: the band's material and depth, the caves' minimum depth.
				const FVoxelChunkLineSurface& LineSurface = bLineSurfaces ? LineColumns.Surfaces[X + Y * ChunkSize] : NoLineSurface;
				const float LineCaveDepth = LineSurface.MinCaveDepth;

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
					// Shaped as every climate reader shapes it (equalised, the lapse over the column's height)
					const float Temperature = BiomeSnapshot.ShapeTemperature(FBM3D(BiomeSamplePos, TempNoiseParams), TerrainHeight, 0.0f, false);
					const float Moisture = BiomeSnapshot.ShapeMoisture(FBM3D(BiomeSamplePos, MoistureNoiseParams));

					// Get blended biome selection for smooth transitions
					FBiomeBlend Blend = BiomeConfig->GetBiomeBlend(Temperature, Moisture, Continentalness);

					// Store the dominant biome ID
					BiomeID = Blend.GetDominantBiome();

					// Cave carving: subtract density for underground cavities
					float CaveDensity = 0.0f;
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						const bool bUnderwater = Request.bEnableWaterLevel && TerrainHeight < Request.WaterLevel;
						CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, nullptr, LineCaveDepth);
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

					// A conditioning line's band: its material to the band's depth, after the height rules and before
					// the cave walls and the ores.
					if (LineSurface.HasBandAt(DepthBelowSurface))
					{
						MaterialID = static_cast<uint8>(LineSurface.BandMaterial);
					}

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
					if (LineSurface.HasBandAt(DepthBelowSurface))
					{
						MaterialID = static_cast<uint8>(LineSurface.BandMaterial);
					}

					// Cave carving for fallback path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						const bool bUnderwater = Request.bEnableWaterLevel && TerrainHeight < Request.WaterLevel;
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, nullptr, LineCaveDepth);
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
					if (LineSurface.HasBandAt(DepthBelowSurface))
					{
						MaterialID = static_cast<uint8>(LineSurface.BandMaterial);
					}

					// Cave carving for non-biome path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						const bool bUnderwater = Request.bEnableWaterLevel && TerrainHeight < Request.WaterLevel;
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, 0, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, nullptr, LineCaveDepth);
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

	// Terrain conditioning. The lines: the chunk's segment list (the floats the GPU uploads), in the frame of the chunk's
	// anchor. A planet has no columns, so a voxel first tests the list's bounds, then walks the list. A chunk no line
	// reaches pays the count test. The zones after them.
	FVoxelChunkLineSegments LineSegments;
	if (Request.Conditioning.Lines.Num() > 0)
	{
		FVoxelChunkLineSegments::Build(Request, LineSegments);
	}
	const bool bLines = LineSegments.Num() > 0;
	const bool bZones = Request.Conditioning.Zones.Num() > 0;
	const FVector AnchorDirection = FSphericalPlanetWorldMode::GetDirectionFromCenter(Request.GetPlanetAnchorRelative(), FVector::ZeroVector);

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

				// Terrain conditioning (spherical SP-B10, D15): the planet lines, then the planet zones (caps on the datum
				// sphere), blend the altitude along this direction toward their targets before the SDF, as the heightmap
				// generators blend the column height, so density, depth, the underwater test, caves and materials all
				// follow the conditioned ground. A line reads the voxel's point on the datum sphere less the anchor's,
				// the shader's SurfaceOffset, formed in double.
				// The lines also leave a surface along this direction: the band's material and depth, the caves'
				// minimum depth.
				FVoxelChunkLineSurface LineSurface;
				if (bLines)
				{
					const FVector3f SurfaceOffset((Direction - AnchorDirection) * PlanetRadius);
					if (LineSegments.MayReach(SurfaceOffset))
					{
						TerrainAltitude = LineSegments.Apply(SurfaceOffset, TerrainAltitude, LineSurface);
					}
				}
				if (bZones)
				{
					TerrainAltitude = Request.Conditioning.ApplyZonesToAltitude(Direction, PlanetRadius, TerrainAltitude);
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
					// Shaped as every climate reader shapes it: equalised, toward latitude, the lapse over the column's altitude
					Temperature = BiomeSnapshot.ShapeTemperature(Temperature, TerrainAltitude, static_cast<float>(Direction.Z), true);
					Moisture = BiomeSnapshot.ShapeMoisture(Moisture);

					FBiomeBlend Blend = BiomeConfig->GetBiomeBlend(Temperature, Moisture, Continentalness);
					BiomeID = Blend.GetDominantBiome();

					// Cave carving: subtract density for underground cavities
					float CaveDensity = 0.0f;
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, &PlanetParams, LineSurface.MinCaveDepth);
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

					// A conditioning line's band, as in the heightmap modes: after the height rules, before the cave
					// walls and the ores.
					if (LineSurface.HasBandAt(DepthBelowSurface))
					{
						MaterialID = static_cast<uint8>(LineSurface.BandMaterial);
					}

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
					if (LineSurface.HasBandAt(DepthBelowSurface))
					{
						MaterialID = static_cast<uint8>(LineSurface.BandMaterial);
					}

					// Cave carving for fallback path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, &PlanetParams, LineSurface.MinCaveDepth);
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
					if (LineSurface.HasBandAt(DepthBelowSurface))
					{
						MaterialID = static_cast<uint8>(LineSurface.BandMaterial);
					}

					// Cave carving for non-biome path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, 0, Request.CaveConfiguration, Request.NoiseParams.Seed, bUnderwater, &PlanetParams, LineSurface.MinCaveDepth);
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
