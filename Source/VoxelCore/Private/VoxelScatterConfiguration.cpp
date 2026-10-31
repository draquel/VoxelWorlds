// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatterConfiguration.h"
#include "VoxelCore.h"

UVoxelScatterConfiguration::UVoxelScatterConfiguration()
{
}

const FScatterDefinition* UVoxelScatterConfiguration::GetScatterDefinition(int32 ScatterID) const
{
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (Def.ScatterID == ScatterID)
		{
			return &Def;
		}
	}
	return nullptr;
}

bool UVoxelScatterConfiguration::ValidateConfiguration() const
{
	bool bValid = true;

	// Check for duplicate IDs
	TSet<int32> SeenIDs;
	for (const FScatterDefinition& Def : ScatterDefinitions)
	{
		if (SeenIDs.Contains(Def.ScatterID))
		{
			UE_LOG(LogVoxelCore, Warning, TEXT("Duplicate scatter ID %d found for '%s'"), Def.ScatterID, *Def.Name);
			bValid = false;
		}
		SeenIDs.Add(Def.ScatterID);

		// Check for valid density
		if (Def.bEnabled && Def.Density <= 0.0f)
		{
			UE_LOG(LogVoxelCore, Warning, TEXT("Scatter '%s' is enabled but has density <= 0"), *Def.Name);
		}

		// Check slope range
		if (Def.MinSlopeDegrees > Def.MaxSlopeDegrees)
		{
			UE_LOG(LogVoxelCore, Warning, TEXT("Scatter '%s' has invalid slope range (min > max)"), *Def.Name);
			bValid = false;
		}

		// Check elevation range
		if (Def.MinElevation > Def.MaxElevation)
		{
			UE_LOG(LogVoxelCore, Warning, TEXT("Scatter '%s' has invalid elevation range (min > max)"), *Def.Name);
			bValid = false;
		}

		// Check scale range
		if (Def.ScaleRange.X > Def.ScaleRange.Y)
		{
			UE_LOG(LogVoxelCore, Warning, TEXT("Scatter '%s' has invalid scale range (min > max)"), *Def.Name);
			bValid = false;
		}
	}

	return bValid;
}

#if WITH_EDITOR
void UVoxelScatterConfiguration::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Validate after changes
	ValidateConfiguration();
}
#endif
