// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelWorldConfiguration.h"
#include "VoxelCore.h"

UVoxelWorldConfiguration::UVoxelWorldConfiguration()
{
	// Setup default LOD bands matched to default ViewDistance (10000 units)
	// Default: ChunkSize=32, VoxelSize=100 -> ChunkWorldSize=3200 units
	//
	// Design principles:
	// - Bands should cover up to ViewDistance (not far beyond)
	// - MorphRange ~25% of band width for smooth transitions
	// - Wider bands at distance for stability (less popping)
	// - Last band extends to ViewDistance
	//
	// When changing ViewDistance, adjust bands proportionally or extend the last band.
	LODBands.Empty();
	LODBands.Add(FLODBand(0.0f, 4000.0f, 0, 1, 32));      // LOD 0: 0-4000, full detail
	LODBands.Add(FLODBand(4000.0f, 7000.0f, 1, 2, 32));   // LOD 1: 4000-7000, half detail
	LODBands.Add(FLODBand(7000.0f, 10000.0f, 2, 4, 32));  // LOD 2: 7000-10000, quarter detail

	// Set morph ranges to 25% of band width for smooth transitions
	for (FLODBand& Band : LODBands)
	{
		Band.MorphRange = (Band.MaxDistance - Band.MinDistance) * 0.25f;
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

float UVoxelWorldConfiguration::GetMaterialLODStartDistance() const
{
	if (LODBands.Num() == 0)
	{
		return 0.0f;
	}

	// Start morphing at first band's morph start point
	const FLODBand& FirstBand = LODBands[0];
	return FMath::Max(0.0f, FirstBand.MaxDistance - FirstBand.MorphRange);
}

float UVoxelWorldConfiguration::GetMaterialLODEndDistance() const
{
	if (LODBands.Num() == 0)
	{
		return ViewDistance;
	}

	// End at the last band's max distance, clamped to ViewDistance
	return FMath::Min(LODBands.Last().MaxDistance, ViewDistance);
}

#if WITH_EDITOR
void UVoxelWorldConfiguration::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Validate when properties change in editor
	ValidateConfiguration();
}
#endif
