// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCoreTypes.h"

/**
 * A world's shadow method while it runs (D164, UVoxelWorldConfiguration::ShadowMethod). The shadow method is one setting
 * for the whole process (r.Shadow.Virtual.Enable), so a world that asks for one sets it when its renderer starts and the
 * value from before the first such world comes back when the last one ends; worlds that disagree are logged, and the one
 * that started last wins until its end. A value typed at the console wins over every world's (set at a higher priority).
 *
 * Game thread only.
 */
struct VOXELRENDERING_API FVoxelShadowMethodOverride
{
	/**
	 * @param Method A world's shadow method
	 * @return The value r.Shadow.Virtual.Enable takes for it (1 or 0), or -1 for EVoxelShadowMethod::Project (left as it is)
	 */
	static int32 GetVirtualShadowMapValue(EVoxelShadowMethod Method);

	/**
	 * A world that renders starts with Method: set the shadow method for it.
	 *
	 * @param Method   The world's shadow method
	 * @param WorldName For the log
	 * @return Whether the world holds the setting now (call Release when it ends); false for Project, or when the console
	 *         set the value and wins
	 */
	static bool Acquire(EVoxelShadowMethod Method, const FString& WorldName);

	/** A world that Acquire returned true for ends: the last one gives the value from before back */
	static void Release();

	/** @return How many worlds hold the setting now */
	static int32 GetHolders();
};
