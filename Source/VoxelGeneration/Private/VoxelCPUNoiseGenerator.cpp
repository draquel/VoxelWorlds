// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUNoiseGenerator.h"
#include "VoxelGeneration.h"
#include "InfinitePlaneWorldMode.h"
#include "IslandBowlWorldMode.h"
#include "SphericalPlanetWorldMode.h"
#include "VoxelBiomeRegistry.h"
#include "VoxelBiomeConfiguration.h"
#include "VoxelCaveConfiguration.h"
#include "VoxelMaterialRegistry.h"
#include "Async/Async.h"

// Permutation table for Perlin noise (Ken Perlin's original)
static const int32 PermutationTable[256] = {
	151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
	8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,
	35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
	134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,
	55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,
	18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,
	250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,
	189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,
	172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,
	228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,
	107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
	138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

// Gradient vectors for Simplex noise
static const int32 Grad3[12][3] = {
	{1,1,0}, {-1,1,0}, {1,-1,0}, {-1,-1,0},
	{1,0,1}, {-1,0,1}, {1,0,-1}, {-1,0,-1},
	{0,1,1}, {0,-1,1}, {0,1,-1}, {0,-1,-1}
};

// Skewing factors for 3D simplex noise
static const float F3 = 1.0f / 3.0f;
static const float G3 = 1.0f / 6.0f;

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

		GenerateChunkInfinitePlane(Request, WorldMode, OutVoxelData);
	}
	else if (Request.WorldMode == EWorldMode::IslandBowl)
	{
		// Create island bowl world mode with terrain and island parameters
		FWorldModeTerrainParams TerrainParams(Request.SeaLevel, Request.HeightScale, Request.BaseHeight);

		FIslandBowlParams IslandParams;
		IslandParams.Shape = static_cast<EIslandShape>(Request.IslandParams.Shape);
		IslandParams.IslandRadius = Request.IslandParams.IslandRadius;
		IslandParams.SizeY = Request.IslandParams.SizeY;
		IslandParams.FalloffWidth = Request.IslandParams.FalloffWidth;
		IslandParams.FalloffType = static_cast<EIslandFalloffType>(Request.IslandParams.FalloffType);
		IslandParams.CenterX = Request.IslandParams.CenterX;
		IslandParams.CenterY = Request.IslandParams.CenterY;
		IslandParams.EdgeHeight = Request.IslandParams.EdgeHeight;
		IslandParams.bBowlShape = Request.IslandParams.bBowlShape;

		FIslandBowlWorldMode WorldMode(TerrainParams, IslandParams);

		GenerateChunkIslandBowl(Request, WorldMode, OutVoxelData);
	}
	else if (Request.WorldMode == EWorldMode::SphericalPlanet)
	{
		// Create spherical planet world mode
		FWorldModeTerrainParams TerrainParams(0.0f, Request.HeightScale, Request.BaseHeight);

		FSphericalPlanetParams PlanetParams;
		PlanetParams.PlanetRadius = Request.SphericalPlanetParams.PlanetRadius;
		PlanetParams.MaxTerrainHeight = Request.SphericalPlanetParams.MaxTerrainHeight;
		PlanetParams.MaxTerrainDepth = Request.SphericalPlanetParams.MaxTerrainDepth;
		PlanetParams.PlanetCenter = Request.SphericalPlanetParams.PlanetCenter;

		FSphericalPlanetWorldMode WorldMode(TerrainParams, PlanetParams);

		GenerateChunkSphericalPlanet(Request, WorldMode, OutVoxelData);
	}
	else
	{
		// Default 3D noise generation for other modes
		GenerateChunk3DNoise(Request, OutVoxelData);
	}

	return true;
}

void FVoxelCPUNoiseGenerator::GenerateChunkInfinitePlane(
	const FVoxelNoiseGenerationRequest& Request,
	const FInfinitePlaneWorldMode& WorldMode,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	// Always generate at base VoxelSize for voxel positioning
	// LOD stride is applied during meshing, not generation
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();

	// Get biome configuration (may be null if biomes disabled)
	const UVoxelBiomeConfiguration* BiomeConfig = Request.BiomeConfiguration;

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

	// Set up continentalness noise parameters
	FVoxelNoiseParams ContinentalnessNoiseParams;
	ContinentalnessNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	ContinentalnessNoiseParams.Octaves = 2;
	ContinentalnessNoiseParams.Persistence = 0.5f;
	ContinentalnessNoiseParams.Lacunarity = 2.0f;
	ContinentalnessNoiseParams.Amplitude = 1.0f;

	// Use configuration values if available, otherwise use defaults
	if (BiomeConfig)
	{
		TempNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->TemperatureSeedOffset;
		TempNoiseParams.Frequency = BiomeConfig->TemperatureNoiseFrequency;
		MoistureNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->MoistureSeedOffset;
		MoistureNoiseParams.Frequency = BiomeConfig->MoistureNoiseFrequency;

		if (BiomeConfig->bEnableContinentalness)
		{
			ContinentalnessNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->ContinentalnessSeedOffset;
			ContinentalnessNoiseParams.Frequency = BiomeConfig->ContinentalnessNoiseFrequency;
		}
	}
	else
	{
		// Defaults matching original FVoxelBiomeRegistry behavior
		TempNoiseParams.Seed = Request.NoiseParams.Seed + 1234;
		TempNoiseParams.Frequency = 0.00005f;
		MoistureNoiseParams.Seed = Request.NoiseParams.Seed + 5678;
		MoistureNoiseParams.Frequency = 0.00007f;
	}

	const bool bUseContinentalness = BiomeConfig && BiomeConfig->bEnableContinentalness;

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

				// Sample continentalness and modulate terrain params
				float Continentalness = 0.0f;
				FWorldModeTerrainParams EffectiveParams = WorldMode.GetTerrainParams();
				if (bUseContinentalness)
				{
					FVector BiomeSamplePos2D(WorldPos.X, WorldPos.Y, 0.0f);
					Continentalness = FBM3D(BiomeSamplePos2D, ContinentalnessNoiseParams);

					float HeightOffset, HeightScaleMult;
					BiomeConfig->GetContinentalnessTerrainParams(Continentalness, HeightOffset, HeightScaleMult);
					EffectiveParams.BaseHeight += HeightOffset;
					EffectiveParams.HeightScale *= HeightScaleMult;
				}

				// Get terrain height from noise (using potentially modulated params)
				float TerrainHeight = FInfinitePlaneWorldMode::NoiseToTerrainHeight(
					NoiseValue, EffectiveParams);

				// Calculate signed distance to surface
				float SignedDistance = FInfinitePlaneWorldMode::CalculateSignedDistance(
					WorldPos.Z, TerrainHeight);

				// Convert to density
				uint8 Density = FInfinitePlaneWorldMode::SignedDistanceToDensity(
					SignedDistance, VoxelSize);

				// Calculate depth below surface for material assignment (in voxels)
				float DepthBelowSurface = (TerrainHeight - WorldPos.Z) / VoxelSize;

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
						CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed);
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
							Blend, DepthBelowSurface, TerrainHeight, Request.WaterLevel);
					}
					else
					{
						MaterialID = BiomeConfig->GetBlendedMaterial(Blend, DepthBelowSurface);
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
					MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, DepthBelowSurface);

					// Cave carving for fallback path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed);
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
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, 0, Request.CaveConfiguration, Request.NoiseParams.Seed);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}

				int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
				OutVoxelData[Index] = FVoxelData(MaterialID, Density, BiomeID, 0);
			}
		}
	}
}

void FVoxelCPUNoiseGenerator::GenerateChunk3DNoise(
	const FVoxelNoiseGenerationRequest& Request,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	// Always generate at base VoxelSize for voxel positioning
	// LOD stride is applied during meshing, not generation
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();

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

				// Sample 3D noise at this position
				float NoiseValue = FBM3D(WorldPos, Request.NoiseParams);

				// Convert noise to density
				uint8 Density = NoiseToDensity(NoiseValue);

				// Create voxel data
				// For now, use a simple material based on height
				uint8 MaterialID = (WorldPos.Z < 0) ? 1 : 0; // Stone below 0, grass at/above
				uint8 BiomeID = 0;

				int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
				OutVoxelData[Index] = FVoxelData(MaterialID, Density, BiomeID, 0);
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

FRHIBuffer* FVoxelCPUNoiseGenerator::GetGeneratedBuffer(const FVoxelGenerationHandle& Handle)
{
	// CPU generator doesn't create GPU buffers
	return nullptr;
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

// ==================== Noise Helper Functions ====================

float FVoxelCPUNoiseGenerator::Fade(float T)
{
	// 6t^5 - 15t^4 + 10t^3
	return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f);
}

float FVoxelCPUNoiseGenerator::Lerp(float A, float B, float T)
{
	return A + T * (B - A);
}

float FVoxelCPUNoiseGenerator::Grad(int32 Hash, float X, float Y, float Z)
{
	// Convert lower 4 bits of hash code to gradient direction
	int32 H = Hash & 15;
	float U = H < 8 ? X : Y;
	float V = H < 4 ? Y : (H == 12 || H == 14 ? X : Z);
	return ((H & 1) == 0 ? U : -U) + ((H & 2) == 0 ? V : -V);
}

int32 FVoxelCPUNoiseGenerator::Perm(int32 Index, int32 Seed)
{
	return PermutationTable[(Index + Seed) & 255];
}

int32 FVoxelCPUNoiseGenerator::Hash(int32 I, int32 Seed)
{
	return PermutationTable[(I + Seed) & 255];
}

int32 FVoxelCPUNoiseGenerator::FastFloor(float X)
{
	int32 Xi = static_cast<int32>(X);
	return X < Xi ? Xi - 1 : Xi;
}

float FVoxelCPUNoiseGenerator::SimplexDot(const int32* G, float X, float Y, float Z)
{
	return G[0] * X + G[1] * Y + G[2] * Z;
}

// ==================== Noise Algorithms ====================

float FVoxelCPUNoiseGenerator::Perlin3D(const FVector& Position, int32 Seed)
{
	float X = Position.X;
	float Y = Position.Y;
	float Z = Position.Z;

	// Find unit cube that contains point
	int32 Xi = FastFloor(X) & 255;
	int32 Yi = FastFloor(Y) & 255;
	int32 Zi = FastFloor(Z) & 255;

	// Find relative X, Y, Z of point in cube
	X -= FastFloor(Position.X);
	Y -= FastFloor(Position.Y);
	Z -= FastFloor(Position.Z);

	// Compute fade curves for each of X, Y, Z
	float U = Fade(X);
	float V = Fade(Y);
	float W = Fade(Z);

	// Hash coordinates of the 8 cube corners
	int32 A = Perm(Xi, Seed) + Yi;
	int32 AA = Perm(A, Seed) + Zi;
	int32 AB = Perm(A + 1, Seed) + Zi;
	int32 B = Perm(Xi + 1, Seed) + Yi;
	int32 BA = Perm(B, Seed) + Zi;
	int32 BB = Perm(B + 1, Seed) + Zi;

	// Add blended results from 8 corners of cube
	float Res = Lerp(
		Lerp(
			Lerp(Grad(Perm(AA, Seed), X, Y, Z),
				 Grad(Perm(BA, Seed), X - 1, Y, Z), U),
			Lerp(Grad(Perm(AB, Seed), X, Y - 1, Z),
				 Grad(Perm(BB, Seed), X - 1, Y - 1, Z), U), V),
		Lerp(
			Lerp(Grad(Perm(AA + 1, Seed), X, Y, Z - 1),
				 Grad(Perm(BA + 1, Seed), X - 1, Y, Z - 1), U),
			Lerp(Grad(Perm(AB + 1, Seed), X, Y - 1, Z - 1),
				 Grad(Perm(BB + 1, Seed), X - 1, Y - 1, Z - 1), U), V), W);

	return Res;
}

float FVoxelCPUNoiseGenerator::Simplex3D(const FVector& Position, int32 Seed)
{
	float X = Position.X;
	float Y = Position.Y;
	float Z = Position.Z;

	// Skew the input space to determine which simplex cell we're in
	float S = (X + Y + Z) * F3;
	int32 I = FastFloor(X + S);
	int32 J = FastFloor(Y + S);
	int32 K = FastFloor(Z + S);

	float T = (I + J + K) * G3;
	float X0 = X - (I - T);
	float Y0 = Y - (J - T);
	float Z0 = Z - (K - T);

	// Determine which simplex we're in
	int32 I1, J1, K1;
	int32 I2, J2, K2;

	if (X0 >= Y0)
	{
		if (Y0 >= Z0) { I1 = 1; J1 = 0; K1 = 0; I2 = 1; J2 = 1; K2 = 0; }
		else if (X0 >= Z0) { I1 = 1; J1 = 0; K1 = 0; I2 = 1; J2 = 0; K2 = 1; }
		else { I1 = 0; J1 = 0; K1 = 1; I2 = 1; J2 = 0; K2 = 1; }
	}
	else
	{
		if (Y0 < Z0) { I1 = 0; J1 = 0; K1 = 1; I2 = 0; J2 = 1; K2 = 1; }
		else if (X0 < Z0) { I1 = 0; J1 = 1; K1 = 0; I2 = 0; J2 = 1; K2 = 1; }
		else { I1 = 0; J1 = 1; K1 = 0; I2 = 1; J2 = 1; K2 = 0; }
	}

	// Offsets for corners
	float X1 = X0 - I1 + G3;
	float Y1 = Y0 - J1 + G3;
	float Z1 = Z0 - K1 + G3;
	float X2 = X0 - I2 + 2.0f * G3;
	float Y2 = Y0 - J2 + 2.0f * G3;
	float Z2 = Z0 - K2 + 2.0f * G3;
	float X3 = X0 - 1.0f + 3.0f * G3;
	float Y3 = Y0 - 1.0f + 3.0f * G3;
	float Z3 = Z0 - 1.0f + 3.0f * G3;

	// Hash corner coordinates
	int32 II = I & 255;
	int32 JJ = J & 255;
	int32 KK = K & 255;
	int32 Gi0 = Hash(II + Hash(JJ + Hash(KK, Seed), Seed), Seed) % 12;
	int32 Gi1 = Hash(II + I1 + Hash(JJ + J1 + Hash(KK + K1, Seed), Seed), Seed) % 12;
	int32 Gi2 = Hash(II + I2 + Hash(JJ + J2 + Hash(KK + K2, Seed), Seed), Seed) % 12;
	int32 Gi3 = Hash(II + 1 + Hash(JJ + 1 + Hash(KK + 1, Seed), Seed), Seed) % 12;

	// Calculate contribution from the four corners
	float N0, N1, N2, N3;

	float T0 = 0.6f - X0 * X0 - Y0 * Y0 - Z0 * Z0;
	if (T0 < 0) N0 = 0.0f;
	else
	{
		T0 *= T0;
		N0 = T0 * T0 * SimplexDot(Grad3[Gi0], X0, Y0, Z0);
	}

	float T1 = 0.6f - X1 * X1 - Y1 * Y1 - Z1 * Z1;
	if (T1 < 0) N1 = 0.0f;
	else
	{
		T1 *= T1;
		N1 = T1 * T1 * SimplexDot(Grad3[Gi1], X1, Y1, Z1);
	}

	float T2 = 0.6f - X2 * X2 - Y2 * Y2 - Z2 * Z2;
	if (T2 < 0) N2 = 0.0f;
	else
	{
		T2 *= T2;
		N2 = T2 * T2 * SimplexDot(Grad3[Gi2], X2, Y2, Z2);
	}

	float T3 = 0.6f - X3 * X3 - Y3 * Y3 - Z3 * Z3;
	if (T3 < 0) N3 = 0.0f;
	else
	{
		T3 *= T3;
		N3 = T3 * T3 * SimplexDot(Grad3[Gi3], X3, Y3, Z3);
	}

	// Sum and scale to [-1, 1]
	return 32.0f * (N0 + N1 + N2 + N3);
}

void FVoxelCPUNoiseGenerator::Cellular3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2)
{
	const int32 CellX = FastFloor(Position.X);
	const int32 CellY = FastFloor(Position.Y);
	const int32 CellZ = FastFloor(Position.Z);
	const float FracX = Position.X - CellX;
	const float FracY = Position.Y - CellY;
	const float FracZ = Position.Z - CellZ;

	OutF1 = 100.0f;
	OutF2 = 100.0f;

	// Search 3x3x3 neighborhood
	for (int32 dz = -1; dz <= 1; ++dz)
	{
		for (int32 dy = -1; dy <= 1; ++dy)
		{
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				const int32 NX = CellX + dx;
				const int32 NY = CellY + dy;
				const int32 NZ = CellZ + dz;

				// Hash-based feature point offset [0, 1] per component
				const float OffX = static_cast<float>(Hash(NX + Hash(NY + Hash(NZ, Seed), Seed), Seed)) / 255.0f;
				const float OffY = static_cast<float>(Hash(NX + 127 + Hash(NY + 63 + Hash(NZ + 31, Seed), Seed), Seed)) / 255.0f;
				const float OffZ = static_cast<float>(Hash(NX + 59 + Hash(NY + 113 + Hash(NZ + 97, Seed), Seed), Seed)) / 255.0f;

				// Delta from fractional position to feature point
				const float DeltaX = static_cast<float>(dx) + OffX - FracX;
				const float DeltaY = static_cast<float>(dy) + OffY - FracY;
				const float DeltaZ = static_cast<float>(dz) + OffZ - FracZ;

				const float DistSq = DeltaX * DeltaX + DeltaY * DeltaY + DeltaZ * DeltaZ;

				if (DistSq < OutF1)
				{
					OutF2 = OutF1;
					OutF1 = DistSq;
				}
				else if (DistSq < OutF2)
				{
					OutF2 = DistSq;
				}
			}
		}
	}

	OutF1 = FMath::Sqrt(OutF1);
	OutF2 = FMath::Sqrt(OutF2);
}

void FVoxelCPUNoiseGenerator::Voronoi3D(const FVector& Position, int32 Seed, float& OutF1, float& OutF2, float& OutCellID)
{
	const int32 CellX = FastFloor(Position.X);
	const int32 CellY = FastFloor(Position.Y);
	const int32 CellZ = FastFloor(Position.Z);
	const float FracX = Position.X - CellX;
	const float FracY = Position.Y - CellY;
	const float FracZ = Position.Z - CellZ;

	OutF1 = 100.0f;
	OutF2 = 100.0f;
	int32 NearestCellX = CellX;
	int32 NearestCellY = CellY;
	int32 NearestCellZ = CellZ;

	for (int32 dz = -1; dz <= 1; ++dz)
	{
		for (int32 dy = -1; dy <= 1; ++dy)
		{
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				const int32 NX = CellX + dx;
				const int32 NY = CellY + dy;
				const int32 NZ = CellZ + dz;

				const float OffX = static_cast<float>(Hash(NX + Hash(NY + Hash(NZ, Seed), Seed), Seed)) / 255.0f;
				const float OffY = static_cast<float>(Hash(NX + 127 + Hash(NY + 63 + Hash(NZ + 31, Seed), Seed), Seed)) / 255.0f;
				const float OffZ = static_cast<float>(Hash(NX + 59 + Hash(NY + 113 + Hash(NZ + 97, Seed), Seed), Seed)) / 255.0f;

				const float DeltaX = static_cast<float>(dx) + OffX - FracX;
				const float DeltaY = static_cast<float>(dy) + OffY - FracY;
				const float DeltaZ = static_cast<float>(dz) + OffZ - FracZ;

				const float DistSq = DeltaX * DeltaX + DeltaY * DeltaY + DeltaZ * DeltaZ;

				if (DistSq < OutF1)
				{
					OutF2 = OutF1;
					OutF1 = DistSq;
					NearestCellX = NX;
					NearestCellY = NY;
					NearestCellZ = NZ;
				}
				else if (DistSq < OutF2)
				{
					OutF2 = DistSq;
				}
			}
		}
	}

	OutF1 = FMath::Sqrt(OutF1);
	OutF2 = FMath::Sqrt(OutF2);

	// Cell ID: hash the nearest cell for a stable per-cell value
	OutCellID = static_cast<float>(Hash(NearestCellX + Hash(NearestCellY + Hash(NearestCellZ, Seed + 12345), Seed + 12345), Seed + 12345)) / 255.0f;
}

float FVoxelCPUNoiseGenerator::FBM3D(const FVector& Position, const FVoxelNoiseParams& Params)
{
	float Total = 0.0f;
	float Frequency = Params.Frequency;
	float Amplitude = Params.Amplitude;
	float MaxValue = 0.0f; // Used for normalizing result

	for (int32 i = 0; i < Params.Octaves; ++i)
	{
		FVector ScaledPos = Position * Frequency;

		float NoiseValue;
		if (Params.NoiseType == EVoxelNoiseType::Perlin)
		{
			NoiseValue = Perlin3D(ScaledPos, Params.Seed);
		}
		else if (Params.NoiseType == EVoxelNoiseType::Cellular)
		{
			// Cellular: use F1 distance, map from [0, ~1.5] to [-1, 1]
			float F1, F2;
			Cellular3D(ScaledPos, Params.Seed, F1, F2);
			NoiseValue = F1 * 2.0f - 1.0f;
		}
		else if (Params.NoiseType == EVoxelNoiseType::Voronoi)
		{
			// Voronoi: use F2-F1 edge distance, map from [0, ~1] to [-1, 1]
			float F1, F2, CellID;
			Voronoi3D(ScaledPos, Params.Seed, F1, F2, CellID);
			NoiseValue = (F2 - F1) * 2.0f - 1.0f;
		}
		else
		{
			NoiseValue = Simplex3D(ScaledPos, Params.Seed);
		}

		Total += NoiseValue * Amplitude;
		MaxValue += Amplitude;

		Amplitude *= Params.Persistence;
		Frequency *= Params.Lacunarity;
	}

	// Normalize to [-1, 1] range
	return Total / MaxValue;
}

void FVoxelCPUNoiseGenerator::GenerateChunkIslandBowl(
	const FVoxelNoiseGenerationRequest& Request,
	const FIslandBowlWorldMode& WorldMode,
	TArray<FVoxelData>& OutVoxelData)
{
	const int32 ChunkSize = Request.ChunkSize;
	// Always generate at base VoxelSize for voxel positioning
	// LOD stride is applied during meshing, not generation
	const float VoxelSize = Request.VoxelSize;
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();

	// Get biome configuration (may be null if biomes disabled)
	const UVoxelBiomeConfiguration* BiomeConfig = Request.BiomeConfiguration;

	// Set up biome noise parameters from configuration
	FVoxelNoiseParams TempNoiseParams;
	TempNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	TempNoiseParams.Octaves = 2;
	TempNoiseParams.Persistence = 0.5f;
	TempNoiseParams.Lacunarity = 2.0f;
	TempNoiseParams.Amplitude = 1.0f;

	FVoxelNoiseParams MoistureNoiseParams;
	MoistureNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	MoistureNoiseParams.Octaves = 2;
	MoistureNoiseParams.Persistence = 0.5f;
	MoistureNoiseParams.Lacunarity = 2.0f;
	MoistureNoiseParams.Amplitude = 1.0f;

	// Set up continentalness noise parameters
	FVoxelNoiseParams ContinentalnessNoiseParamsIB;
	ContinentalnessNoiseParamsIB.NoiseType = EVoxelNoiseType::Simplex;
	ContinentalnessNoiseParamsIB.Octaves = 2;
	ContinentalnessNoiseParamsIB.Persistence = 0.5f;
	ContinentalnessNoiseParamsIB.Lacunarity = 2.0f;
	ContinentalnessNoiseParamsIB.Amplitude = 1.0f;

	// Use configuration values if available, otherwise use defaults
	if (BiomeConfig)
	{
		TempNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->TemperatureSeedOffset;
		TempNoiseParams.Frequency = BiomeConfig->TemperatureNoiseFrequency;
		MoistureNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->MoistureSeedOffset;
		MoistureNoiseParams.Frequency = BiomeConfig->MoistureNoiseFrequency;

		if (BiomeConfig->bEnableContinentalness)
		{
			ContinentalnessNoiseParamsIB.Seed = Request.NoiseParams.Seed + BiomeConfig->ContinentalnessSeedOffset;
			ContinentalnessNoiseParamsIB.Frequency = BiomeConfig->ContinentalnessNoiseFrequency;
		}
	}
	else
	{
		TempNoiseParams.Seed = Request.NoiseParams.Seed + 1234;
		TempNoiseParams.Frequency = 0.00005f;
		MoistureNoiseParams.Seed = Request.NoiseParams.Seed + 5678;
		MoistureNoiseParams.Frequency = 0.00007f;
	}

	const bool bUseContinentalnessIB = BiomeConfig && BiomeConfig->bEnableContinentalness;

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

				// Sample continentalness for biome selection (height modulation
				// is not applied for IslandBowl since it has its own falloff system)
				float Continentalness = 0.0f;
				if (bUseContinentalnessIB)
				{
					FVector BiomeSamplePos2D(WorldPos.X, WorldPos.Y, 0.0f);
					Continentalness = FBM3D(BiomeSamplePos2D, ContinentalnessNoiseParamsIB);
				}

				// Sample 2D noise at X,Y (same as InfinitePlane base)
				float NoiseValue = FInfinitePlaneWorldMode::SampleTerrainNoise2D(
					WorldPos.X, WorldPos.Y, Request.NoiseParams);

				// Get density from island bowl world mode (handles falloff)
				float SignedDistance = WorldMode.GetDensityAt(WorldPos, Request.LODLevel, NoiseValue);

				// Convert signed distance to density
				uint8 Density = FInfinitePlaneWorldMode::SignedDistanceToDensity(SignedDistance, VoxelSize);

				// Get terrain height for material assignment (includes island falloff)
				float TerrainHeight = WorldMode.GetTerrainHeightAt(WorldPos.X, WorldPos.Y, Request.NoiseParams);

				// Calculate depth below surface for material assignment
				float DepthBelowSurface = (TerrainHeight - WorldPos.Z) / VoxelSize;

				// Determine material and biome
				uint8 MaterialID = 0;
				uint8 BiomeID = 0;

				if (Request.bEnableBiomes && BiomeConfig && BiomeConfig->IsValid())
				{
					// Sample biome noise at X,Y
					FVector BiomeSamplePos(WorldPos.X, WorldPos.Y, 0.0f);
					float Temperature = FBM3D(BiomeSamplePos, TempNoiseParams);
					float Moisture = FBM3D(BiomeSamplePos, MoistureNoiseParams);

					// Get blended biome selection
					FBiomeBlend Blend = BiomeConfig->GetBiomeBlend(Temperature, Moisture, Continentalness);
					BiomeID = Blend.GetDominantBiome();

					// Cave carving: subtract density for underground cavities
					float CaveDensity = 0.0f;
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed);
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
							Blend, DepthBelowSurface, TerrainHeight, Request.WaterLevel);
					}
					else
					{
						MaterialID = BiomeConfig->GetBlendedMaterial(Blend, DepthBelowSurface);
					}

					// Apply height-based material overrides
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
					// Fallback to static registry
					FVector BiomeSamplePos(WorldPos.X, WorldPos.Y, 0.0f);
					float Temperature = FBM3D(BiomeSamplePos, TempNoiseParams);
					float Moisture = FBM3D(BiomeSamplePos, MoistureNoiseParams);

					FBiomeBlend Blend = FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f);
					BiomeID = Blend.GetDominantBiome();
					MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, DepthBelowSurface);

					// Cave carving for fallback path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}
				else
				{
					// Use world mode's material assignment
					MaterialID = WorldMode.GetMaterialAtDepth(WorldPos, TerrainHeight, DepthBelowSurface * VoxelSize);

					// Cave carving for non-biome path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, 0, Request.CaveConfiguration, Request.NoiseParams.Seed);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}

				int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
				OutVoxelData[Index] = FVoxelData(MaterialID, Density, BiomeID, 0);
			}
		}
	}
}

// ==================== Cave Generation Helpers ====================

float FVoxelCPUNoiseGenerator::SampleCaveLayer(const FVector& WorldPos, const FCaveLayerConfig& LayerConfig, int32 WorldSeed)
{
	// Apply vertical scale to flatten caves horizontally
	FVector ScaledPos(WorldPos.X, WorldPos.Y, WorldPos.Z * LayerConfig.VerticalScale);

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
		float Noise = FBM3D(ScaledPos, CaveNoiseParams);

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
		float Noise1 = FBM3D(ScaledPos, CaveNoiseParams);

		// Second noise field with offset seed and scaled frequency
		FVoxelNoiseParams SecondNoiseParams = CaveNoiseParams;
		SecondNoiseParams.Seed = WorldSeed + LayerConfig.SecondNoiseSeedOffset;
		SecondNoiseParams.Frequency = LayerConfig.Frequency * LayerConfig.SecondNoiseFrequencyScale;

		float Noise2 = FBM3D(ScaledPos, SecondNoiseParams);

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
	int32 WorldSeed)
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
		float LayerCarve = SampleCaveLayer(WorldPos, Layer, WorldSeed);

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

	if (OreConfig.Shape == EOreVeinShape::Streak)
	{
		// For streak shapes, stretch the noise along a pseudo-random direction
		// derived from position to create elongated vein shapes
		const float StreakSeed = static_cast<float>(OreConfig.SeedOffset) * 0.1f;

		// Create direction vector based on position (varies smoothly across world)
		FVector StreakDir;
		StreakDir.X = FMath::Sin(WorldPos.Y * 0.0001f + StreakSeed);
		StreakDir.Y = FMath::Cos(WorldPos.X * 0.0001f + StreakSeed * 1.5f);
		StreakDir.Z = FMath::Sin(WorldPos.Z * 0.0002f + StreakSeed * 2.0f);
		StreakDir.Normalize();

		// Project position onto perpendicular plane to stretch
		FVector Projected = WorldPos - StreakDir * FVector::DotProduct(WorldPos, StreakDir);
		SamplePos = Projected + StreakDir * (FVector::DotProduct(WorldPos, StreakDir) / OreConfig.StreakStretch);
	}

	// Sample noise
	float NoiseValue = FBM3D(SamplePos, OreNoiseParams);

	// Normalize from [-1, 1] to [0, 1]
	return (NoiseValue + 1.0f) * 0.5f;
}

bool FVoxelCPUNoiseGenerator::CheckOreVeinPlacement(
	const FVector& WorldPos,
	float DepthBelowSurface,
	const TArray<FOreVeinConfig>& OreConfigs,
	int32 WorldSeed,
	uint8& OutMaterialID)
{
	// Check each ore type in priority order (assumed to be pre-sorted)
	for (const FOreVeinConfig& OreConfig : OreConfigs)
	{
		// Check depth constraints
		if (!OreConfig.IsValidDepth(DepthBelowSurface))
		{
			continue;
		}

		// Sample ore noise at this position
		float OreNoise = SampleOreVeinNoise(WorldPos, OreConfig, WorldSeed);

		// Check against threshold
		if (OreNoise >= OreConfig.Threshold)
		{
			// Apply rarity check (if rarity < 1, randomly skip some valid placements)
			if (OreConfig.Rarity < 1.0f)
			{
				// Use deterministic random based on position
				float RandomValue = FMath::Frac(
					FMath::Sin(WorldPos.X * 12.9898f + WorldPos.Y * 78.233f + WorldPos.Z * 45.164f) * 43758.5453f
				);
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
	const FVector PlanetCenter = WorldMode.GetPlanetParams().PlanetCenter;

	// Get biome configuration (may be null if biomes disabled)
	const UVoxelBiomeConfiguration* BiomeConfig = Request.BiomeConfiguration;

	// Set up biome noise parameters
	FVoxelNoiseParams TempNoiseParams;
	TempNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	TempNoiseParams.Octaves = 2;
	TempNoiseParams.Persistence = 0.5f;
	TempNoiseParams.Lacunarity = 2.0f;
	TempNoiseParams.Amplitude = 1.0f;

	FVoxelNoiseParams MoistureNoiseParams;
	MoistureNoiseParams.NoiseType = EVoxelNoiseType::Simplex;
	MoistureNoiseParams.Octaves = 2;
	MoistureNoiseParams.Persistence = 0.5f;
	MoistureNoiseParams.Lacunarity = 2.0f;
	MoistureNoiseParams.Amplitude = 1.0f;

	// Set up continentalness noise parameters
	FVoxelNoiseParams ContinentalnessNoiseParamsSP;
	ContinentalnessNoiseParamsSP.NoiseType = EVoxelNoiseType::Simplex;
	ContinentalnessNoiseParamsSP.Octaves = 2;
	ContinentalnessNoiseParamsSP.Persistence = 0.5f;
	ContinentalnessNoiseParamsSP.Lacunarity = 2.0f;
	ContinentalnessNoiseParamsSP.Amplitude = 1.0f;

	if (BiomeConfig)
	{
		TempNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->TemperatureSeedOffset;
		TempNoiseParams.Frequency = BiomeConfig->TemperatureNoiseFrequency;
		MoistureNoiseParams.Seed = Request.NoiseParams.Seed + BiomeConfig->MoistureSeedOffset;
		MoistureNoiseParams.Frequency = BiomeConfig->MoistureNoiseFrequency;

		if (BiomeConfig->bEnableContinentalness)
		{
			ContinentalnessNoiseParamsSP.Seed = Request.NoiseParams.Seed + BiomeConfig->ContinentalnessSeedOffset;
			ContinentalnessNoiseParamsSP.Frequency = BiomeConfig->ContinentalnessNoiseFrequency;
		}
	}
	else
	{
		TempNoiseParams.Seed = Request.NoiseParams.Seed + 1234;
		TempNoiseParams.Frequency = 0.00005f;
		MoistureNoiseParams.Seed = Request.NoiseParams.Seed + 5678;
		MoistureNoiseParams.Frequency = 0.00007f;
	}

	const bool bUseContinentalnessSP = BiomeConfig && BiomeConfig->bEnableContinentalness;

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

				// Get direction from planet center for noise sampling
				FVector Direction = FSphericalPlanetWorldMode::GetDirectionFromCenter(WorldPos, PlanetCenter);

				// Sample continentalness for biome selection (uses direction like other biome noise)
				float Continentalness = 0.0f;
				if (bUseContinentalnessSP)
				{
					FVector BiomeSamplePosSP = Direction * 10000.0f;
					Continentalness = FBM3D(BiomeSamplePosSP, ContinentalnessNoiseParamsSP);
				}

				// Sample spherical noise using direction
				float NoiseValue = FSphericalPlanetWorldMode::SampleSphericalNoise(Direction, Request.NoiseParams);

				// Get density from spherical planet world mode
				float SignedDistance = WorldMode.GetDensityAt(WorldPos, Request.LODLevel, NoiseValue);

				// Convert signed distance to density
				uint8 Density = FInfinitePlaneWorldMode::SignedDistanceToDensity(SignedDistance, VoxelSize);

				// Calculate radial distance for material assignment
				float DistFromCenter = FSphericalPlanetWorldMode::CalculateRadialDistance(WorldPos, PlanetCenter);
				float TerrainRadius = WorldMode.GetPlanetParams().PlanetRadius +
					FSphericalPlanetWorldMode::NoiseToRadialDisplacement(NoiseValue, WorldMode.GetTerrainParams());

				// Depth below surface is radial (terrain radius - distance from center)
				float DepthBelowSurface = (TerrainRadius - DistFromCenter) / VoxelSize;

				// Determine material and biome
				uint8 MaterialID = 0;
				uint8 BiomeID = 0;

				if (Request.bEnableBiomes && BiomeConfig && BiomeConfig->IsValid())
				{
					// For spherical planets, use direction for biome sampling
					// Scale direction to get varied biome coordinates
					FVector BiomeSamplePos = Direction * 10000.0f;
					float Temperature = FBM3D(BiomeSamplePos, TempNoiseParams);
					float Moisture = FBM3D(BiomeSamplePos, MoistureNoiseParams);

					FBiomeBlend Blend = BiomeConfig->GetBiomeBlend(Temperature, Moisture, Continentalness);
					BiomeID = Blend.GetDominantBiome();

					// Cave carving: subtract density for underground cavities
					float CaveDensity = 0.0f;
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}

					// Get material considering blend weights and water level
					// For spherical planets: underwater if terrain radius < water radius
					if (Request.bEnableWaterLevel)
					{
						MaterialID = BiomeConfig->GetBlendedMaterialWithWater(
							Blend, DepthBelowSurface, TerrainRadius, Request.WaterRadius);
					}
					else
					{
						MaterialID = BiomeConfig->GetBlendedMaterial(Blend, DepthBelowSurface);
					}

					// Height rules use radial distance as "height" for spherical planets
					MaterialID = BiomeConfig->ApplyHeightMaterialRules(MaterialID, DistFromCenter, DepthBelowSurface);

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
					FVector BiomeSamplePos = Direction * 10000.0f;
					float Temperature = FBM3D(BiomeSamplePos, TempNoiseParams);
					float Moisture = FBM3D(BiomeSamplePos, MoistureNoiseParams);

					FBiomeBlend Blend = FVoxelBiomeRegistry::GetBiomeBlend(Temperature, Moisture, 0.15f);
					BiomeID = Blend.GetDominantBiome();
					MaterialID = FVoxelBiomeRegistry::GetBlendedMaterial(Blend, DepthBelowSurface);

					// Cave carving for fallback path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, BiomeID, Request.CaveConfiguration, Request.NoiseParams.Seed);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}
				else
				{
					MaterialID = WorldMode.GetMaterialAtDepth(WorldPos, TerrainRadius, DepthBelowSurface * VoxelSize);

					// Cave carving for non-biome path
					if (Request.bEnableCaves && Density >= VOXEL_SURFACE_THRESHOLD && DepthBelowSurface > 0.0f)
					{
						float CaveDensity = CalculateCaveDensity(WorldPos, DepthBelowSurface, 0, Request.CaveConfiguration, Request.NoiseParams.Seed);
						if (CaveDensity > 0.0f)
						{
							float NewDensity = FMath::Max(0.0f, static_cast<float>(Density) - CaveDensity * 255.0f);
							Density = static_cast<uint8>(FMath::Clamp(NewDensity, 0.0f, 255.0f));
						}
					}
				}

				int32 Index = X + Y * ChunkSize + Z * ChunkSize * ChunkSize;
				OutVoxelData[Index] = FVoxelData(MaterialID, Density, BiomeID, 0);
			}
		}
	}
}
