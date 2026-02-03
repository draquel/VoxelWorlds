// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldConfiguration.h"
#include "VoxelCore.h"

UVoxelWorldConfiguration::UVoxelWorldConfiguration()
{
	// Setup default LOD bands for infinite plane
	// Default: ChunkSize=32, VoxelSize=100 -> ChunkWorldSize=3200 units
	// LOD bands are scaled relative to chunk size for proper visual transitions
	LODBands.Empty();
	LODBands.Add(FLODBand(0.0f, 3200.0f, 0, 1, 32));       // LOD 0: 0-1 chunk, full detail
	LODBands.Add(FLODBand(3200.0f, 6400.0f, 1, 2, 32));    // LOD 1: 1-2 chunks, half detail
	LODBands.Add(FLODBand(6400.0f, 12800.0f, 2, 4, 32));   // LOD 2: 2-4 chunks, quarter detail
	LODBands.Add(FLODBand(12800.0f, 25600.0f, 3, 8, 64));  // LOD 3: 4-8 chunks, coarse
	LODBands.Add(FLODBand(25600.0f, 51200.0f, 4, 16, 128)); // LOD 4: 8-16 chunks, very coarse

	// Set morph ranges for smooth transitions
	for (int32 i = 0; i < LODBands.Num() - 1; ++i)
	{
		LODBands[i].MorphRange = (LODBands[i].MaxDistance - LODBands[i].MinDistance) * 0.25f;
	}
}

float UVoxelWorldConfiguration::GetChunkWorldSizeAtLOD(int32 LODLevel) const
{
	for (const FLODBand& Band : LODBands)
	{
		if (Band.LODLevel == LODLevel)
		{
			return Band.ChunkSize * Band.VoxelStride * VoxelSize;
		}
	}

	// Fallback to base chunk size
	return GetChunkWorldSize();
}

const FLODBand* UVoxelWorldConfiguration::GetLODBandForDistance(float Distance) const
{
	for (const FLODBand& Band : LODBands)
	{
		if (Band.ContainsDistance(Distance))
		{
			return &Band;
		}
	}

	return nullptr;
}

int32 UVoxelWorldConfiguration::GetLODLevelForDistance(float Distance) const
{
	const FLODBand* Band = GetLODBandForDistance(Distance);
	if (Band)
	{
		return Band->LODLevel;
	}

	// Beyond all bands - return coarsest LOD
	if (LODBands.Num() > 0)
	{
		return LODBands.Last().LODLevel;
	}

	return 0;
}

bool UVoxelWorldConfiguration::ValidateConfiguration() const
{
	bool bValid = true;

	// Check voxel size
	if (VoxelSize <= 0.0f)
	{
		UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: VoxelSize must be positive"));
		bValid = false;
	}

	// Check chunk size
	if (ChunkSize < 8 || ChunkSize > 128)
	{
		UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration: ChunkSize %d is outside recommended range [8, 128]"), ChunkSize);
	}

	// Check if chunk size is power of 2
	if ((ChunkSize & (ChunkSize - 1)) != 0)
	{
		UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration: ChunkSize %d is not a power of 2, may cause issues"), ChunkSize);
	}

	// Check LOD bands
	if (LODBands.Num() == 0)
	{
		UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: No LOD bands configured"));
		bValid = false;
	}
	else
	{
		// Check for gaps or overlaps in LOD bands
		for (int32 i = 0; i < LODBands.Num(); ++i)
		{
			const FLODBand& Band = LODBands[i];

			if (Band.MinDistance >= Band.MaxDistance)
			{
				UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: LOD band %d has invalid range [%f, %f]"),
					i, Band.MinDistance, Band.MaxDistance);
				bValid = false;
			}

			if (i > 0)
			{
				const FLODBand& PrevBand = LODBands[i - 1];
				if (FMath::Abs(Band.MinDistance - PrevBand.MaxDistance) > KINDA_SMALL_NUMBER)
				{
					UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration: Gap between LOD bands %d and %d"), i - 1, i);
				}
			}
		}

		// Check first band starts at 0
		if (LODBands[0].MinDistance > KINDA_SMALL_NUMBER)
		{
			UE_LOG(LogVoxelCore, Warning, TEXT("VoxelWorldConfiguration: First LOD band doesn't start at 0"));
		}
	}

	// Check view distance
	if (ViewDistance <= 0.0f)
	{
		UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: ViewDistance must be positive"));
		bValid = false;
	}

	// Check streaming settings
	if (MaxChunksToLoadPerFrame < 1)
	{
		UE_LOG(LogVoxelCore, Error, TEXT("VoxelWorldConfiguration: MaxChunksToLoadPerFrame must be at least 1"));
		bValid = false;
	}

	return bValid;
}

#if WITH_EDITOR
void UVoxelWorldConfiguration::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Validate when properties change in editor
	ValidateConfiguration();
}
#endif
