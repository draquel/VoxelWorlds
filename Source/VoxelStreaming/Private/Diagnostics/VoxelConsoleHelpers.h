// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

class UVoxelChunkManager;

// What the console commands in this folder share.

#if !UE_BUILD_SHIPPING
namespace VoxelStreamingDiagnostics
{
	/**
	 * Add the test conditioning lines voxel.World.TestLines names to a chunk manager (VoxelConditioningLineConsole.cpp):
	 * called at the end of its Initialize, before it builds a generation request. Nothing when the cvar is empty.
	 */
	void AddTestConditioningLines(UVoxelChunkManager& Manager);
}
#endif

namespace VoxelEditAuditHelpers
{
	/** The PIE or Game world: the console's own when it is one, else the first PIE/Game world context. */
	inline UWorld* FindGameWorld(UWorld* World)
	{
		if (World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game))
		{
			return World;
		}
		if (GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.World() && (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game))
				{
					return Ctx.World();
				}
			}
		}
		return nullptr;
	}
}
