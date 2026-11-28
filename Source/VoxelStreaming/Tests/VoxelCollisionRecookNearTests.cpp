// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Testing/VoxelTestScopedCVar.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G299, D201 (open, the user's decision): where a neighbour re-cook stands in the cook queue.
//
// A cook reads a missing neighbour as air and is cooked again when the neighbour arrives (G32); that re-cook queues below
// every first cook. On a machine whose first cooks never run out (a dedicated server process: four workers, the CPU
// generating its window) it was never launched, and the server kept the cook that read air under a walking player: a
// few uu off the client's collision at each chunk face the walk crossed (Scripts/qa/server_process.py).
// voxel.Collision.RecookNearChunks N > 0 queues a re-cook within N chunk widths of a focus as a first cook at its
// distance would be; 0, the default until the decision, keeps it below every first cook.
//
// On a plane's chunk manager (its collision manager initialized from it), the priorities GetDirtyPriority gives:
// - 0: a re-cook near the focus and one far from it are both below the furthest first cook in the shell;
// - 2: the near one (one chunk width out) has the first cook's priority at its distance, the far one (three widths) is
//   still below every first cook; an edit's re-cook is above a first cook's at the same distance either way.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionRecookNearTest, "VoxelWorlds.Collision.RecookNear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCollisionRecookNearTest::RunTest(const FString& Parameters)
{
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("CollisionRecookNear"));
	W.ConfigureSmallPlane();
	W.Config->bGenerateCollision = true;
	W.StartManager();
	UVoxelCollisionManager* Collision = W.Manager ? W.Manager->GetCollisionManager() : nullptr;
	if (!TestNotNull(TEXT("The chunk manager has a collision manager"), Collision))
	{
		W.Stop();
		return false;
	}
	const float Width = W.Config->GetChunkWorldSize();
	Collision->SetCollisionRadius(4.0f * Width);
	const float Radius = Collision->GetCollisionRadius();
	// The furthest first cook in the shell: at the larger of the configured and the effective radius.
	const float FurthestFirstCook = Radius - FMath::Max(Collision->GetCurrentEffectiveRadius(), Radius);

	FChunkCollisionData Recook;
	Recook.bNeedsUpdate = true;
	Recook.bNeighborRecookOnly = true;
	FChunkCollisionData Edit;
	Edit.bNeedsUpdate = true;
	Edit.bNeighborRecookOnly = false;
	const float NearDistance = 1.0f * Width;
	const float FarDistance = 3.0f * Width;

	float Off[3], On[3];
	{
		VoxelTesting::FScopedCVar Switch(TEXT("voxel.Collision.RecookNearChunks"), 0.0f);
		Off[0] = Collision->GetDirtyPriority(Recook, NearDistance);
		Off[1] = Collision->GetDirtyPriority(Recook, FarDistance);
		Off[2] = Collision->GetDirtyPriority(Edit, NearDistance);
	}
	{
		VoxelTesting::FScopedCVar Switch(TEXT("voxel.Collision.RecookNearChunks"), 2.0f);
		On[0] = Collision->GetDirtyPriority(Recook, NearDistance);
		On[1] = Collision->GetDirtyPriority(Recook, FarDistance);
		On[2] = Collision->GetDirtyPriority(Edit, NearDistance);
	}
	AddInfo(FString::Printf(TEXT("Chunk width %.0f, shell %.0f, the furthest first cook %.0f, a first cook at one width %.0f: off near %.0f far %.0f edit %.0f; at 2 near %.0f far %.0f edit %.0f"),
		Width, Radius, FurthestFirstCook, Radius - NearDistance, Off[0], Off[1], Off[2], On[0], On[1], On[2]));

	TestTrue(TEXT("Off: a re-cook near the focus is below every first cook"), Off[0] < FurthestFirstCook);
	TestTrue(TEXT("Off: a re-cook far from it too"), Off[1] < FurthestFirstCook);
	TestEqual(TEXT("At 2 widths: a re-cook one width out has a first cook's priority at its distance"), On[0], Radius - NearDistance);
	TestTrue(TEXT("At 2 widths: a re-cook three widths out is still below every first cook"), On[1] < FurthestFirstCook);
	TestTrue(TEXT("An edit's re-cook is above a first cook at the same distance, off"), Off[2] > Radius - NearDistance);
	TestEqual(TEXT("... and the same at 2 widths"), On[2], Off[2]);
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
