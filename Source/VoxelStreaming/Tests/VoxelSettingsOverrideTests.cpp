// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "VoxelWorldsSettings.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Config.SettingsOverride (code review CR4 row 16, the plan's §10.4): the tuning values a shipped world or project sets live
// on UVoxelWorldConfiguration and UVoxelWorldsSettings, each with a console variable of the same meaning that overrides it
// once something sets the variable (VoxelSettings::Resolve). The rule on a variable of the test's own; and every moved
// value's default the variable's default, so a world that sets nothing computes what it did before the move.
// ---------------------------------------------------------------------------

static TAutoConsoleVariable<float> CVarVoxelTestsSettingsOverride(
	TEXT("voxel.Tests.SettingsOverride"),
	1.0f,
	TEXT("A variable only Config.SettingsOverride reads (the override rule of VoxelSettings::Resolve)."),
	ECVF_Default);

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSettingsOverrideTest,
	"VoxelWorlds.Config.SettingsOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSettingsOverrideTest::RunTest(const FString& Parameters)
{
	// The rule: never set, the configured value; once set, the variable's (until the end of the session).
	if (!VoxelSettings::IsOverridden(CVarVoxelTestsSettingsOverride.AsVariable()))
	{
		TestEqual(TEXT("Unset: the configured value"), VoxelSettings::Resolve(CVarVoxelTestsSettingsOverride, 5.0f), 5.0f);
	}
	{
		const VoxelTesting::FScopedCVar Set(TEXT("voxel.Tests.SettingsOverride"), TEXT("3"));
		TestTrue(TEXT("Set: overridden"), VoxelSettings::IsOverridden(CVarVoxelTestsSettingsOverride.AsVariable()));
		TestEqual(TEXT("Set: the variable's value"), VoxelSettings::Resolve(CVarVoxelTestsSettingsOverride, 5.0f), 3.0f);
		TestEqual(TEXT("Set: the variable's value on any thread"), VoxelSettings::ResolveAnyThread(CVarVoxelTestsSettingsOverride, 5.0f), 3.0f);
	}

	// Every moved value's default is its variable's default.
	auto Default = [this](const TCHAR* Name) -> float
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		if (!TestNotNull(*FString::Printf(TEXT("%s exists"), Name), Var))
		{
			return TNumericLimits<float>::Max();
		}
		return FCString::Atof(*Var->GetDefaultValue());
	};
	const UVoxelWorldsSettings& S = UVoxelWorldsSettings::Get();
	const UVoxelWorldConfiguration* C = GetDefault<UVoxelWorldConfiguration>();
	const TPair<const TCHAR*, float> Pairs[] = {
		{ TEXT("voxel.Collision.LeadSeconds"), S.CollisionLeadSeconds },
		{ TEXT("voxel.Collision.ResweepSeconds"), S.CollisionResweepSeconds },
		{ TEXT("voxel.Collision.MoveUpdateThreshold"), S.CollisionMoveUpdateThreshold },
		{ TEXT("voxel.Collision.MaxAppliesPerFrame"), static_cast<float>(S.CollisionMaxAppliesPerFrame) },
		{ TEXT("voxel.Collision.LookAheadChunks"), static_cast<float>(S.CollisionLookAheadChunks) },
		{ TEXT("voxel.Nav.CellSize"), S.NavCellSize },
		{ TEXT("voxel.Nav.MaxNodes"), static_cast<float>(S.NavMaxNodes) },
		{ TEXT("voxel.Nav.MaxStepRise"), S.NavMaxStepRise },
		{ TEXT("voxel.Nav.MaxWadeDepth"), S.NavMaxWadeDepth },
		{ TEXT("voxel.Nav.MaxRadius"), S.NavMaxRadius },
		{ TEXT("voxel.MeshFade.Duration"), S.MeshFadeDuration },
		{ TEXT("voxel.MeshFade.AppearMinDistance"), S.MeshFadeAppearMinDistance },
		{ TEXT("voxel.Mesh.MCBoundaryMorphWidth"), S.MCBoundaryMorphWidth },
		{ TEXT("voxel.Scatter.HISMBatchSeconds"), S.ScatterHISMBatchSeconds },
		{ TEXT("voxel.Scatter.HISMCompactMinPooled"), static_cast<float>(S.ScatterHISMCompactMinPooled) },
		{ TEXT("voxel.Edit.UndoHistoryMB"), S.EditUndoHistoryMB },
		{ TEXT("voxel.World.FarFieldError"), C->FarFieldScreenError },
		{ TEXT("voxel.World.FarFieldHeightmapError"), C->FarFieldHeightmapScreenError },
		{ TEXT("voxel.World.FarFieldQuads"), static_cast<float>(C->FarFieldPatchQuads) },
		{ TEXT("voxel.World.FarFieldRootChunks"), static_cast<float>(C->FarFieldRootChunks) },
		{ TEXT("voxel.World.FarFieldDistance"), C->FarFieldDistance },
		{ TEXT("voxel.World.FarFieldMaxBuilds"), static_cast<float>(C->FarFieldMaxBuilds) },
		{ TEXT("voxel.Render.RegionChunks"), static_cast<float>(C->RenderRegionChunks) },
		{ TEXT("voxel.Scatter.RegionChunks"), static_cast<float>(C->ScatterRegionChunks) },
	};
	for (const TPair<const TCHAR*, float>& Pair : Pairs)
	{
		TestEqual(*FString::Printf(TEXT("%s: the setting's default is the variable's"), Pair.Key), Pair.Value, Default(Pair.Key));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
