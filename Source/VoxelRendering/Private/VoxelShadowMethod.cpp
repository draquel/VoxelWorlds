// Copyright Daniel Raquel. All Rights Reserved.

#include "VoxelShadowMethod.h"
#include "VoxelRendering.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/** Worlds holding the setting, the value from before the first of them, and the value they set last */
	int32 GHolders = 0;
	int32 GSavedValue = 0;
	int32 GAppliedValue = -1;

	IConsoleVariable* GetVirtualShadowMapCVar()
	{
		static IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Enable"));
		return CVar;
	}
}

int32 FVoxelShadowMethodOverride::GetVirtualShadowMapValue(EVoxelShadowMethod Method)
{
	switch (Method)
	{
	case EVoxelShadowMethod::VirtualShadowMaps:
		return 1;
	case EVoxelShadowMethod::ShadowMaps:
		return 0;
	case EVoxelShadowMethod::Project:
	default:
		return -1;
	}
}

bool FVoxelShadowMethodOverride::Acquire(EVoxelShadowMethod Method, const FString& WorldName)
{
	check(IsInGameThread());
	const int32 Value = GetVirtualShadowMapValue(Method);
	IConsoleVariable* CVar = GetVirtualShadowMapCVar();
	if (Value < 0 || !CVar)
	{
		return false;
	}
	// A value typed at the console outranks code: the world's choice cannot apply, and the console's stands
	if ((CVar->GetFlags() & ECVF_SetByMask) == ECVF_SetByConsole)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("%s asks for %s, but r.Shadow.Virtual.Enable was set at the console (%d): it stays"),
			*WorldName, *UEnum::GetValueAsString(Method), CVar->GetInt());
		return false;
	}
	if (GHolders == 0)
	{
		GSavedValue = CVar->GetInt();
	}
	else if (GAppliedValue != Value)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("%s asks for %s while another world holds r.Shadow.Virtual.Enable %d: the last world wins"),
			*WorldName, *UEnum::GetValueAsString(Method), GAppliedValue);
	}
	CVar->Set(Value, ECVF_SetByCode);
	GAppliedValue = Value;
	++GHolders;
	UE_LOG(LogVoxelRendering, Log, TEXT("%s: %s while it runs (r.Shadow.Virtual.Enable %d, %d before)"),
		*WorldName, *UEnum::GetValueAsString(Method), Value, GSavedValue);
	return true;
}

void FVoxelShadowMethodOverride::Release()
{
	check(IsInGameThread());
	if (GHolders <= 0)
	{
		UE_LOG(LogVoxelRendering, Warning, TEXT("FVoxelShadowMethodOverride::Release with no world holding the shadow method"));
		return;
	}
	if (--GHolders == 0)
	{
		if (IConsoleVariable* CVar = GetVirtualShadowMapCVar(); CVar && (CVar->GetFlags() & ECVF_SetByMask) != ECVF_SetByConsole)
		{
			CVar->Set(GSavedValue, ECVF_SetByCode);
			UE_LOG(LogVoxelRendering, Log, TEXT("The last world with its own shadow method ended: r.Shadow.Virtual.Enable back to %d"), GSavedValue);
		}
		GAppliedValue = -1;
	}
}

int32 FVoxelShadowMethodOverride::GetHolders()
{
	return GHolders;
}
