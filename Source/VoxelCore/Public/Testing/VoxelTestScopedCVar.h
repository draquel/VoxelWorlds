// Copyright Daniel Raquel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"

namespace VoxelTesting
{
	/**
	 * Sets a console variable for a scope at the priority it already holds (the one scoped cvar of the plugin's tests, code
	 * review CR2 row 17): a set below that priority is ignored (a test that set it by console earlier in a suite run), and a
	 * set above it would outrank the other tests' code-priority sets. Restores the previous value at the same priority.
	 */
	struct FScopedCVar
	{
		IConsoleVariable* Var = nullptr;
		FString Previous;
		EConsoleVariableFlags Priority = ECVF_SetByCode;

		FScopedCVar(const TCHAR* Name, const TCHAR* Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Previous = Var->GetString();
				Priority = static_cast<EConsoleVariableFlags>(FMath::Max(static_cast<uint32>(Var->GetFlags() & ECVF_SetByMask), static_cast<uint32>(ECVF_SetByCode)));
				Var->Set(Value, Priority);
			}
		}
		FScopedCVar(const TCHAR* Name, int32 Value) : FScopedCVar(Name, *FString::FromInt(Value)) {}
		FScopedCVar(const TCHAR* Name, float Value) : FScopedCVar(Name, *FString::SanitizeFloat(Value)) {}
		FScopedCVar(const FScopedCVar&) = delete;
		FScopedCVar& operator=(const FScopedCVar&) = delete;

		~FScopedCVar()
		{
			if (Var)
			{
				Var->Set(*Previous, Priority);
			}
		}
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
