// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Scatter harvest (gameplay): the exclusion volume a felled instance leaves is deterministic
// per base position, so every machine registers the same volume.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterHarvestVolumeTest,
	"VoxelWorlds.Scatter.Harvest.ExclusionVolume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVoxelScatterHarvestVolumeTest::RunTest(const FString& Parameters)
{
	const FVector Base(1234.4f, -5678.6f, 90.2f);
	const FScatterExclusionVolume A = UVoxelScatterManager::MakeHarvestExclusionVolume(Base, 60.0f, 400.0f);
	const FScatterExclusionVolume B = UVoxelScatterManager::MakeHarvestExclusionVolume(Base + FVector(0.04f, -0.03f, 0.02f), 60.0f, 400.0f);
	const FScatterExclusionVolume C = UVoxelScatterManager::MakeHarvestExclusionVolume(Base + FVector(300.0f, 0.0f, 0.0f), 60.0f, 400.0f);

	TestTrue(TEXT("Volume is usable"), A.IsUsable());
	TestTrue(TEXT("Same base (to the cm) -> same id"), A.Id == B.Id);
	TestTrue(TEXT("Another base -> another id"), A.Id != C.Id);
	TestEqual(TEXT("Trunk half-extent X"), static_cast<float>(A.HalfExtent.X), 60.0f, 0.001f);
	TestEqual(TEXT("Half height"), static_cast<float>(A.HalfExtent.Z), 200.0f, 0.001f);
	TestTrue(TEXT("Box starts just below the base"), A.Frame.GetLocation().Z - A.HalfExtent.Z < Base.Z);
	TestTrue(TEXT("Box reaches up the trunk"), A.Frame.GetLocation().Z + A.HalfExtent.Z > Base.Z + 300.0f);

	FScatterHarvestResult Empty;
	TestFalse(TEXT("Default result is invalid"), Empty.bValid);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
