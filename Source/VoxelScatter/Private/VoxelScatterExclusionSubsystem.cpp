// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelScatterExclusionSubsystem.h"
#include "VoxelScatterManager.h"
#include "VoxelScatter.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

UVoxelScatterExclusionSubsystem* UVoxelScatterExclusionSubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext || !GEngine)
	{
		return nullptr;
	}
	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	return World ? World->GetSubsystem<UVoxelScatterExclusionSubsystem>() : nullptr;
}

bool UVoxelScatterExclusionSubsystem::RegisterVolume(const FScatterExclusionVolume& Volume)
{
	if (!Volume.IsUsable())
	{
		UE_LOG(LogVoxelScatter, Warning, TEXT("ScatterExclusionSubsystem: rejected volume with invalid Id or non-positive extent"));
		return false;
	}

	Volumes.Add(Volume.Id, Volume);

	// Forward to every live manager (drop stale entries as a side effect)
	Managers.RemoveAll([](const TWeakObjectPtr<UVoxelScatterManager>& Entry) { return !Entry.IsValid(); });
	for (const TWeakObjectPtr<UVoxelScatterManager>& Entry : Managers)
	{
		Entry->RegisterScatterExclusionVolume(Volume);
	}
	return true;
}

bool UVoxelScatterExclusionSubsystem::UnregisterVolume(const FGuid& VolumeId)
{
	if (Volumes.Remove(VolumeId) == 0)
	{
		return false;
	}

	Managers.RemoveAll([](const TWeakObjectPtr<UVoxelScatterManager>& Entry) { return !Entry.IsValid(); });
	for (const TWeakObjectPtr<UVoxelScatterManager>& Entry : Managers)
	{
		Entry->UnregisterScatterExclusionVolume(VolumeId);
	}
	return true;
}

void UVoxelScatterExclusionSubsystem::NotifyManagerInitialized(UVoxelScatterManager* Manager)
{
	if (!Manager)
	{
		return;
	}

	Managers.RemoveAll([](const TWeakObjectPtr<UVoxelScatterManager>& Entry) { return !Entry.IsValid(); });
	Managers.AddUnique(Manager);

	// Replay every cached volume: volumes registered before this manager existed still apply.
	for (const auto& Pair : Volumes)
	{
		Manager->RegisterScatterExclusionVolume(Pair.Value);
	}

	if (Volumes.Num() > 0)
	{
		UE_LOG(LogVoxelScatter, Log, TEXT("ScatterExclusionSubsystem: replayed %d exclusion volume(s) to scatter manager"),
			Volumes.Num());
	}
}

void UVoxelScatterExclusionSubsystem::NotifyManagerShutdown(UVoxelScatterManager* Manager)
{
	Managers.RemoveAll([Manager](const TWeakObjectPtr<UVoxelScatterManager>& Entry)
	{
		return !Entry.IsValid() || Entry.Get() == Manager;
	});
}
