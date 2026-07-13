// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelScatterTypes.h"
#include "VoxelScatterExclusionSubsystem.generated.h"

class UVoxelScatterManager;

/**
 * World-level entry point for persistent scatter exclusion volumes.
 *
 * External systems (e.g. a game-level bridge mapping claim footprints to bare ground) register
 * volumes here rather than reaching for the UVoxelScatterManager instance, which is owned by the
 * chunk manager and may not exist yet when volumes are produced. The subsystem keeps the
 * authoritative volume set and replays it to every scatter manager that initializes later, so
 * registration order between volume producers and the voxel world never matters.
 *
 * VoxelWorlds attaches no semantics to volumes — who registers them (claims, POIs, gameplay
 * systems) is entirely the caller's concern, which keeps this plugin free of any game-plugin
 * dependency (see SCATTER_CLAIM_SUPPRESSION_PLAN.md).
 *
 * Thread Safety: game thread only, like the scatter manager it forwards to.
 */
UCLASS()
class VOXELSCATTER_API UVoxelScatterExclusionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * Convenience accessor: the exclusion subsystem for WorldContext's world, or null.
	 *
	 * @param WorldContext Object whose world to resolve
	 * @return The subsystem, or null if the world has none
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Exclusion",
		meta = (WorldContext = "WorldContext", DisplayName = "Get Scatter Exclusion Subsystem"))
	static UVoxelScatterExclusionSubsystem* Get(const UObject* WorldContext);

	/**
	 * Register (or replace, by Id) an exclusion volume. Cached authoritatively here and forwarded
	 * to every live scatter manager; managers that initialize later receive it via replay.
	 *
	 * @param Volume The volume to register. Must have a valid Id and positive extents.
	 * @return True if the volume was accepted.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Exclusion")
	bool RegisterVolume(const FScatterExclusionVolume& Volume);

	/**
	 * Unregister a volume by Id from the cache and every live scatter manager (foliage regrows).
	 *
	 * @param VolumeId Id the volume was registered under.
	 * @return True if a volume with this Id existed in the cache.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Exclusion")
	bool UnregisterVolume(const FGuid& VolumeId);

	/** True if a volume with this Id is currently cached. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Exclusion")
	bool HasVolume(const FGuid& VolumeId) const { return Volumes.Contains(VolumeId); }

	/** Number of cached exclusion volumes. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Scatter|Exclusion")
	int32 GetVolumeCount() const { return Volumes.Num(); }

	/**
	 * Called by UVoxelScatterManager::Initialize. Tracks the manager and replays every cached
	 * volume to it, so volumes registered before the voxel world spun up still apply.
	 */
	void NotifyManagerInitialized(UVoxelScatterManager* Manager);

	/** Called by UVoxelScatterManager::Shutdown. Stops forwarding to the manager. */
	void NotifyManagerShutdown(UVoxelScatterManager* Manager);

private:
	/** Authoritative volume set — survives scatter-manager lifecycles, replayed on registration. */
	TMap<FGuid, FScatterExclusionVolume> Volumes;

	/** Live scatter managers to forward to (weak — the chunk manager owns them). */
	TArray<TWeakObjectPtr<UVoxelScatterManager>> Managers;
};
