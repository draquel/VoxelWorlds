// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelCPUNoiseGenerator.h"
#include "VoxelGeneration.h"
#include "InfinitePlaneWorldMode.h"
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
	const float EffectiveVoxelSize = Request.GetEffectiveVoxelSize();
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
	const float EffectiveVoxelSize = Request.GetEffectiveVoxelSize();
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();

	for (int32 Z = 0; Z < ChunkSize; ++Z)
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				// Calculate world position for this voxel
				FVector WorldPos = ChunkWorldPos + FVector(
					X * EffectiveVoxelSize,
					Y * EffectiveVoxelSize,
					Z * EffectiveVoxelSize
				);

				// Sample 2D noise at X,Y (Z=0 for heightmap)
				float NoiseValue = FInfinitePlaneWorldMode::SampleTerrainNoise2D(
					WorldPos.X, WorldPos.Y, Request.NoiseParams);

				// Get terrain height from noise
				float TerrainHeight = FInfinitePlaneWorldMode::NoiseToTerrainHeight(
					NoiseValue, WorldMode.GetTerrainParams());

				// Calculate signed distance to surface
				float SignedDistance = FInfinitePlaneWorldMode::CalculateSignedDistance(
					WorldPos.Z, TerrainHeight);

				// Convert to density
				uint8 Density = FInfinitePlaneWorldMode::SignedDistanceToDensity(
					SignedDistance, EffectiveVoxelSize);

				// Calculate depth below surface for material assignment
				float DepthBelowSurface = TerrainHeight - WorldPos.Z;

				// Determine material based on depth
				uint8 MaterialID = WorldMode.GetMaterialAtDepth(WorldPos, TerrainHeight, DepthBelowSurface);

				// Default biome
				uint8 BiomeID = 0;

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
	const float EffectiveVoxelSize = Request.GetEffectiveVoxelSize();
	const FVector ChunkWorldPos = Request.GetChunkWorldPosition();

	for (int32 Z = 0; Z < ChunkSize; ++Z)
	{
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				// Calculate world position for this voxel
				FVector WorldPos = ChunkWorldPos + FVector(
					X * EffectiveVoxelSize,
					Y * EffectiveVoxelSize,
					Z * EffectiveVoxelSize
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
