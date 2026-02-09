// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatterPlacement.h"
#include "VoxelScatter.h"

void FVoxelScatterPlacement::GenerateSpawnPoints(
	const FChunkSurfaceData& SurfaceData,
	const TArray<FScatterDefinition>& Definitions,
	uint32 ChunkSeed,
	FChunkScatterData& OutScatterData)
{
	// Initialize output
	OutScatterData = FChunkScatterData(SurfaceData.ChunkCoord);
	OutScatterData.GenerationSeed = ChunkSeed;

	// Validate input
	if (!SurfaceData.bIsValid || SurfaceData.SurfacePoints.Num() == 0)
	{
		OutScatterData.bIsValid = false;
		return;
	}

	if (Definitions.Num() == 0)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("Chunk (%d,%d,%d): GenerateSpawnPoints called with 0 definitions! (surface points=%d)"),
			SurfaceData.ChunkCoord.X, SurfaceData.ChunkCoord.Y, SurfaceData.ChunkCoord.Z,
			SurfaceData.SurfacePoints.Num());
		OutScatterData.bIsValid = true; // Valid but empty
		return;
	}

	// Reserve approximate capacity
	// Estimate: Each definition might generate ~10% of surface points on average
	const int32 EstimatedPointsPerDef = FMath::Max(1, SurfaceData.SurfacePoints.Num() / 10);
	OutScatterData.SpawnPoints.Reserve(EstimatedPointsPerDef * Definitions.Num());

	// Generate spawn points for each scatter type
	int32 EnabledDefCount = 0;
	for (const FScatterDefinition& Definition : Definitions)
	{
		if (!Definition.bEnabled)
		{
			continue;
		}
		++EnabledDefCount;

		// Use unique seed per scatter type to ensure independence
		const uint32 TypeSeed = ChunkSeed ^ (Definition.ScatterID * 2654435761u);
		GenerateSpawnPointsForType(SurfaceData, Definition, TypeSeed, OutScatterData.SpawnPoints);
	}

	OutScatterData.bIsValid = true;

	if (OutScatterData.SpawnPoints.Num() == 0 && SurfaceData.SurfacePoints.Num() > 0)
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("Chunk (%d,%d,%d): 0 spawn from %d defs (%d enabled), %d surface pts"),
			SurfaceData.ChunkCoord.X, SurfaceData.ChunkCoord.Y, SurfaceData.ChunkCoord.Z,
			Definitions.Num(), EnabledDefCount, SurfaceData.SurfacePoints.Num());
	}
	else
	{
		UE_LOG(LogVoxelScatter, Verbose, TEXT("Chunk (%d,%d,%d): Generated %d spawn points from %d surface points"),
			SurfaceData.ChunkCoord.X, SurfaceData.ChunkCoord.Y, SurfaceData.ChunkCoord.Z,
			OutScatterData.SpawnPoints.Num(), SurfaceData.SurfacePoints.Num());
	}
}

int32 FVoxelScatterPlacement::GenerateSpawnPointsForType(
	const FChunkSurfaceData& SurfaceData,
	const FScatterDefinition& Definition,
	uint32 ChunkSeed,
	TArray<FScatterSpawnPoint>& OutSpawnPoints)
{
	int32 PointsGenerated = 0;

	// Use density directly as spawn probability (0-1 range, where 0.1 = 10% of valid points)
	// Clamp to ensure valid probability
	const float SpawnProbability = FMath::Clamp(Definition.Density, 0.0f, 1.0f);

	UE_LOG(LogVoxelScatter, Verbose, TEXT("Scatter '%s': Density=%.4f -> Probability=%.4f, SurfacePoints=%d"),
		*Definition.Name, Definition.Density, SpawnProbability, SurfaceData.SurfacePoints.Num());

	if (SpawnProbability <= 0.0f)
	{
		return 0;
	}

	// Debug counters
	int32 PointsChecked = 0;
	int32 PointsPassedRules = 0;
	int32 PointsPassedRandom = 0;

	// Process each surface point
	for (int32 PointIndex = 0; PointIndex < SurfaceData.SurfacePoints.Num(); ++PointIndex)
	{
		const FVoxelSurfacePoint& SurfacePoint = SurfaceData.SurfacePoints[PointIndex];
		PointsChecked++;

		// Check placement rules
		if (!Definition.CanSpawnAt(SurfacePoint))
		{
			continue;
		}
		PointsPassedRules++;

		// Generate deterministic seed for this point
		uint32 PointSeed = HashPosition(SurfacePoint.Position, ChunkSeed);

		// Probability check
		const float Roll = RandomFromSeed(PointSeed);
		if (Roll >= SpawnProbability)
		{
			continue;
		}
		PointsPassedRandom++;

		// Create spawn point
		FScatterSpawnPoint SpawnPoint;
		SpawnPoint.Position = SurfacePoint.Position;
		SpawnPoint.Normal = SurfacePoint.Normal;
		SpawnPoint.ScatterTypeID = Definition.ScatterID;
		SpawnPoint.InstanceSeed = PointSeed;

		// Compute variation using remaining random values
		SpawnPoint.Scale = Definition.ComputeScale(RandomFromSeed(PointSeed));
		SpawnPoint.RotationYaw = Definition.ComputeRotationYaw(RandomFromSeed(PointSeed));

		// Apply position jitter
		if (Definition.PositionJitter > 0.0f)
		{
			const FVector Jitter = Definition.ComputePositionJitter(
				RandomFromSeed(PointSeed),
				RandomFromSeed(PointSeed)
			);
			SpawnPoint.Position += Jitter;
		}

		// Apply surface offset
		if (Definition.SurfaceOffset != 0.0f)
		{
			SpawnPoint.Position += SpawnPoint.Normal * Definition.SurfaceOffset;
		}

		OutSpawnPoints.Add(SpawnPoint);
		++PointsGenerated;
	}

	UE_LOG(LogVoxelScatter, Verbose, TEXT("Scatter '%s': Spawned %d (Checked=%d, PassedRules=%d, Density=%.4f)"),
		*Definition.Name, PointsGenerated, PointsChecked, PointsPassedRules, Definition.Density);

	return PointsGenerated;
}

uint32 FVoxelScatterPlacement::ComputeChunkSeed(const FIntVector& ChunkCoord, uint32 WorldSeed)
{
	// Combine chunk coordinate with world seed using hash
	uint32 Seed = WorldSeed;

	// FNV-1a hash
	Seed ^= static_cast<uint32>(ChunkCoord.X);
	Seed *= 16777619u;
	Seed ^= static_cast<uint32>(ChunkCoord.Y);
	Seed *= 16777619u;
	Seed ^= static_cast<uint32>(ChunkCoord.Z);
	Seed *= 16777619u;

	return Seed;
}

uint32 FVoxelScatterPlacement::HashPosition(const FVector& Position, uint32 BaseSeed)
{
	// Quantize position to avoid floating point issues
	const int32 X = FMath::RoundToInt(Position.X);
	const int32 Y = FMath::RoundToInt(Position.Y);
	const int32 Z = FMath::RoundToInt(Position.Z);

	// Hash using FNV-1a
	uint32 Hash = BaseSeed ^ 2166136261u;
	Hash ^= static_cast<uint32>(X);
	Hash *= 16777619u;
	Hash ^= static_cast<uint32>(Y);
	Hash *= 16777619u;
	Hash ^= static_cast<uint32>(Z);
	Hash *= 16777619u;

	return Hash;
}

float FVoxelScatterPlacement::RandomFromSeed(uint32& Seed)
{
	// LCG (Linear Congruential Generator)
	// Parameters from Numerical Recipes
	Seed = Seed * 1664525u + 1013904223u;

	// Convert to float in [0, 1)
	return static_cast<float>(Seed & 0x7FFFFFFF) / static_cast<float>(0x80000000u);
}

float FVoxelScatterPlacement::RandomInRange(uint32& Seed, float Min, float Max)
{
	return FMath::Lerp(Min, Max, RandomFromSeed(Seed));
}
