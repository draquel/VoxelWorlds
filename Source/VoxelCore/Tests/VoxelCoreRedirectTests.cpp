// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/CoreRedirects.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The plugin's [CoreRedirects] (Config/DefaultVoxelWorlds.ini) for the reflected names the code review's CR3 changed
// (CODE_REVIEW_PLAN.md §17, decision D-CR4): each old name resolves to the new one, so an asset or Blueprint saved with the
// old name loads. A redirect in a plugin's own ini is read when the plugin mounts; this test fails if the file stops
// being read or a line stops parsing.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelWorldsCoreRedirectsTest,
	"VoxelWorlds.Config.CoreRedirects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelWorldsCoreRedirectsTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		ECoreRedirectFlags Type;
		const TCHAR* OldName;
		const TCHAR* NewName;
	};
	const FCase Cases[] = {
		{ ECoreRedirectFlags::Type_Property, TEXT("/Script/VoxelCore.VoxelWorldConfiguration.PlanetVoxelCutoverAltitude"),
			TEXT("/Script/VoxelCore.VoxelWorldConfiguration.VoxelCutoverAltitude") },
		{ ECoreRedirectFlags::Type_Property, TEXT("/Script/VoxelCore.VoxelWorldConfiguration.bFarBandSurfaceSlabCulling"),
			TEXT("/Script/VoxelCore.VoxelWorldConfiguration.bSurfaceSlabCulling") },
	};
	for (const FCase& Case : Cases)
	{
		const FCoreRedirectObjectName Old(Case.OldName);
		const FCoreRedirectObjectName Expected(Case.NewName);
		const FCoreRedirectObjectName Redirected = FCoreRedirects::GetRedirectedName(Case.Type, Old);
		TestTrue(FString::Printf(TEXT("%s redirects to %s (got %s)"), Case.OldName, Case.NewName, *Redirected.ToString()),
			Redirected == Expected);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
