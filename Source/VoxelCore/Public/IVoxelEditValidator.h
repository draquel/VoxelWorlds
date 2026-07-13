// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelEditTypes.h"

/**
 * Pluggable veto over voxel edits — the edit-protection seam.
 *
 * Registered on UVoxelEditManager (SetEditValidator, mirroring the terrain-conditioner pattern):
 * the manager consults it per voxel inside brush/single-edit paths and silently skips voxels the
 * validator rejects (the rejected count is surfaced for later UI messaging). A brush overlapping
 * a protected region edits only the unprotected voxels.
 *
 * VoxelWorlds attaches no semantics to WHY an edit is rejected — game-level code implements this
 * (e.g. claim-based protection: players cannot dig into POI structures or dungeon shells) and
 * decides per source; typical implementations reject only Player edits and pass System/Editor
 * (stampers, POI flattens, undo).
 *
 * Thread Safety: called on the game thread only. Implementations must be cheap — this runs per
 * voxel inside brush loops (thousands of calls per stroke).
 */
class IVoxelEditValidator
{
public:
	virtual ~IVoxelEditValidator() = default;

	/**
	 * May the voxel at WorldPos be modified by an edit from Source?
	 *
	 * @param WorldPos World-space position of the voxel being edited.
	 * @param Source   Who is editing (Player / System / Editor).
	 * @return False to silently skip this voxel.
	 */
	virtual bool CanApplyEdit(const FVector& WorldPos, EEditSource Source) const = 0;
};
