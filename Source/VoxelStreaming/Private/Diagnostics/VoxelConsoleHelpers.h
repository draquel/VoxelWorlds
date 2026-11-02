// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

// What the console commands in this folder share.

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
