// Copyright Daniel Raquel. All Rights Reserved.

// The chunk manager's tick budget and the teleport's collision jump (G282):
// - Clock: FVoxelTickBudget on a clock the test drives: no budget lets everything go on; a budget lets a section's first
//   piece go on always and the next ones while time is left.
// - Collision.Jump: a viewer that jumps far in one tick on a plane with collision: with voxel.Collision.JumpIsCut 1 the
//   shell's radius stays its own (the jump is no speed) and the jump is counted; with 0 the jump counts as speed and the
//   radius reaches back over the place left (the behaviour before G282).
// - Collision.DeferredDestroys: after the jump, with a budget too small for more than one piece a tick, the place left loses
//   its collision data at once (HasCollision false) while its components go one a tick, all of them in the end; without a
//   budget they go in the jump's tick.
// - WaterBodies: four lakes at growing distances from the viewer: with that budget their meshes build on the thread pool and
//   a tick gives one built lake its component at most, until all four are drawn; without one all four in the first tick.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Materials/Material.h"
#include "ProceduralMeshComponent.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelTickBudget.h"
#include "VoxelWaterLevels.h"
#include "VoxelWorldConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VoxelTickBudgetTest
{
	/** The test's clock (seconds): moved by hand. */
	double GNow = 0.0;
	double Now() { return GNow; }

	/** A budget small enough that every section does its one piece a tick and no more. */
	const TCHAR* TinyBudget = TEXT("0.0001");

	/** Tick until the stream and the collision pipeline are idle for 60 ticks; false on timeout. */
	bool SettleWithCollision(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds = 180.0)
	{
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (Collision && IdleTicks < 60 && FPlatformTime::Seconds() < Deadline)
		{
			W.Tick();
			const bool bIdle = W.IsIdle() && Collision->GetCookQueueCount() == 0 && Collision->GetCookingCount() == 0
				&& Collision->GetPendingDestroyCount() == 0;
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
		}
		return IdleTicks >= 60;
	}

	/** A small plane with collision, streamed and cooked around a viewer over the ground at (X, Y). */
	bool StartPlaneWithCollision(VoxelTesting::FTestChunkWorld& W, const TCHAR* Name, double X, double Y)
	{
		W.Create(Name);
		W.ConfigureSmallPlane();
		W.Config->bGenerateCollision = true;
		W.StartManager();
		if (!W.Manager->GetCollisionManager())
		{
			return false;
		}
		W.SetViewer(W.ViewerOverGround(X, Y, 200.0f));
		return W.Settle() && SettleWithCollision(W);
	}

	/** The chunks with collision now. */
	TArray<FIntVector> ChunksWithCollision(VoxelTesting::FTestChunkWorld& W)
	{
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		return Loaded.FilterByPredicate([&](const FIntVector& Coord) { return W.Manager->GetCollisionManager()->HasCollision(Coord); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTickBudgetClockTest, "VoxelWorlds.Streaming.TickBudget.Clock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTickBudgetClockTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTickBudgetTest;

	const FVoxelTickBudget None;
	TestFalse(TEXT("No budget is not limited"), None.IsLimited());
	TestTrue(TEXT("No budget lets every piece go on"), None.MayContinue(0) && None.MayContinue(1000) && None.HasTimeLeft());
	const FVoxelTickBudget Zero(0.0, &Now);
	TestTrue(TEXT("A budget of 0 ms is no budget (the behaviour before G282)"), !Zero.IsLimited() && Zero.MayContinue(1000));

	GNow = 100.0;
	const FVoxelTickBudget Budget(2.0, &Now);
	TestTrue(TEXT("A budget of 2 ms is limited"), Budget.IsLimited());
	TestTrue(TEXT("At its start time is left: the first and the next pieces go on"), Budget.MayContinue(0) && Budget.MayContinue(5));
	GNow = 100.0019;
	TestTrue(TEXT("At 1.9 ms time is still left"), Budget.HasTimeLeft() && Budget.MayContinue(3));
	TestEqual(TEXT("ElapsedMs reads the clock"), Budget.ElapsedMs(), 1.9, 1.0e-6);
	GNow = 100.0021;
	TestFalse(TEXT("At 2.1 ms no time is left"), Budget.HasTimeLeft());
	TestFalse(TEXT("... so a section's next piece waits"), Budget.MayContinue(1));
	TestTrue(TEXT("... and a section's first piece still goes on (each section moves every tick)"), Budget.MayContinue(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTickBudgetCollisionJumpTest, "VoxelWorlds.Streaming.TickBudget.Collision.Jump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTickBudgetCollisionJumpTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTickBudgetTest;

	for (const int32 bCut : { 1, 0 })
	{
		const FString Name = bCut ? TEXT("JumpIsCut 1") : TEXT("JumpIsCut 0");
		VoxelTesting::FScopedCVar Cut(TEXT("voxel.Collision.JumpIsCut"), bCut);
		VoxelTesting::FTestChunkWorld W;
		if (!TestTrue(*FString::Printf(TEXT("%s: the plane streams and cooks its collision"), *Name),
			StartPlaneWithCollision(W, bCut ? TEXT("TickBudgetJumpCut") : TEXT("TickBudgetJumpSpeed"), 0.0, 0.0)))
		{
			W.Stop();
			continue;
		}
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		const double Base = Collision->GetCollisionRadius();
		const int64 JumpsBefore = Collision->GetFocusJumps();
		TestTrue(*FString::Printf(TEXT("%s: the shell at rest is its own radius (non-vacuous: %.0f)"), *Name, Base),
			Base > 0.0 && Collision->GetCurrentEffectiveRadius() <= Base + 1.0);

		// 40,000 uu in one tick: 2.4 million uu/s at 60 ticks a second.
		W.SetViewer(W.ViewerOverGround(40000.0, 0.0, 200.0f));
		W.Tick();
		if (bCut)
		{
			TestEqual(*FString::Printf(TEXT("%s: the jump is counted"), *Name), Collision->GetFocusJumps(), JumpsBefore + 1);
			TestTrue(*FString::Printf(TEXT("%s: the shell keeps its own radius after the jump (%.0f)"), *Name, Collision->GetCurrentEffectiveRadius()),
				Collision->GetCurrentEffectiveRadius() <= Base + 1.0);
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: no jump is counted"), *Name), Collision->GetFocusJumps(), JumpsBefore);
			TestTrue(*FString::Printf(TEXT("%s: the jump counts as speed: the radius reaches back past the place left (%.0f)"), *Name,
				Collision->GetCurrentEffectiveRadius()), Collision->GetCurrentEffectiveRadius() > 10.0 * Base);
		}
		W.Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTickBudgetDeferredDestroysTest, "VoxelWorlds.Streaming.TickBudget.Collision.DeferredDestroys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTickBudgetDeferredDestroysTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTickBudgetTest;

	for (const bool bBudget : { true, false })
	{
		const FString Name = bBudget ? TEXT("with a budget") : TEXT("without one");
		VoxelTesting::FScopedCVar Budget(TEXT("voxel.Stream.TickBudgetMs"), bBudget ? TinyBudget : TEXT("0"));
		VoxelTesting::FTestChunkWorld W;
		if (!TestTrue(*FString::Printf(TEXT("%s: the plane streams and cooks its collision"), *Name),
			StartPlaneWithCollision(W, bBudget ? TEXT("TickBudgetDestroys") : TEXT("TickBudgetNoDestroys"), 0.0, 0.0)))
		{
			W.Stop();
			continue;
		}
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		const TArray<FIntVector> Before = ChunksWithCollision(W);
		if (!TestTrue(*FString::Printf(TEXT("%s: chunks have collision before the jump (non-vacuous: %d)"), *Name, Before.Num()), Before.Num() > 2))
		{
			W.Stop();
			continue;
		}

		W.SetViewer(W.ViewerOverGround(40000.0, 0.0, 200.0f));
		W.Tick();
		const int32 Kept = Before.FilterByPredicate([&](const FIntVector& Coord) { return Collision->HasCollision(Coord); }).Num();
		TestEqual(*FString::Printf(TEXT("%s: the place left has no collision data after the jump's tick"), *Name), Kept, 0);
		const int32 Pending = Collision->GetPendingDestroyCount();
		if (bBudget)
		{
			TestTrue(*FString::Printf(TEXT("%s: its components go a few a tick: %d of %d wait after the first"), *Name, Pending, Before.Num()),
				Pending > 0 && Pending < Before.Num());
			int32 Ticks = 0;
			int32 Last = Pending;
			bool bOneATick = true;
			while (Collision->GetPendingDestroyCount() > 0 && Ticks < 10000)
			{
				W.Tick();
				++Ticks;
				const int32 Now = Collision->GetPendingDestroyCount();
				bOneATick &= Now < Last;
				Last = Now;
			}
			TestEqual(*FString::Printf(TEXT("%s: every component goes in the end"), *Name), Collision->GetPendingDestroyCount(), 0);
			TestTrue(*FString::Printf(TEXT("%s: each tick destroys at least one"), *Name), bOneATick);
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: every component goes in the jump's tick (the behaviour before G282)"), *Name), Pending, 0);
		}
		// And the new place gets its collision as it would without a budget.
		TestTrue(*FString::Printf(TEXT("%s: the new place streams and cooks its collision"), *Name), W.Settle() && SettleWithCollision(W));
		TestTrue(*FString::Printf(TEXT("%s: chunks there have collision"), *Name), ChunksWithCollision(W).Num() > 2);
		W.Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelTickBudgetWaterBodiesTest, "VoxelWorlds.Streaming.TickBudget.WaterBodies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelTickBudgetWaterBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VoxelTickBudgetTest;

	for (const bool bBudget : { true, false })
	{
		const FString Name = bBudget ? TEXT("with a budget") : TEXT("without one");
		VoxelTesting::FScopedCVar Budget(TEXT("voxel.Stream.TickBudgetMs"), bBudget ? TinyBudget : TEXT("0"));
		VoxelTesting::FTestChunkWorld W;
		W.Create(bBudget ? TEXT("TickBudgetWaterBodies") : TEXT("TickBudgetWaterBodiesAll"));
		W.ConfigureSmallPlane();
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = -5000.0f;
		W.StartManager();
		UVoxelChunkManager* Manager = W.Manager;
		const FVoxelWaterFrame Frame = Manager->GetWaterLevels().GetFrame();

		// Four lakes of 4 x 4 cells of 100 uu at 1,000, 3,000, 6,000 and 10,000 uu east of the viewer, listed farthest first.
		const TArray<double> Distances = { 10000.0, 6000.0, 3000.0, 1000.0 };
		for (int32 Index = 0; Index < Distances.Num(); ++Index)
		{
			const FVector2D Corner(Distances[Index] - 200.0, -200.0);
			Manager->AddWaterBody(FVoxelWaterBody::MakeLake(100 + Index, 900.0,
				FVoxelWaterLake::Make(Corner, 100.0, FIntPoint(4, 4), TBitArray<>(true, 16)), Frame, 200.0));
		}
		W.Config->WaterMeshMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		W.SetViewer(FVector(0.0, 0.0, 2000.0));
		W.Tick();
		const FVoxelWaterBodyMeshOwner& Meshes = Manager->GetWaterBodyMeshes();
		if (bBudget)
		{
			// The meshes build on the thread pool; the built ones get a component a tick (the budget's first piece).
			TestTrue(*FString::Printf(TEXT("%s: the first tick draws one lake at most (%d)"), *Name, Meshes.Num()), Meshes.Num() <= 1);
			int32 Ticks = 0;
			int32 Last = Meshes.Num();
			bool bOneATick = true;
			while (Meshes.Num() < 4 && Ticks < 2000)
			{
				W.Tick();
				++Ticks;
				bOneATick &= Meshes.Num() - Last <= 1;
				Last = Meshes.Num();
			}
			TestEqual(*FString::Printf(TEXT("%s: all four are drawn in the end (%d ticks)"), *Name, Ticks), Meshes.Num(), 4);
			TestTrue(*FString::Printf(TEXT("%s: one lake a tick at most"), *Name), bOneATick);
			TestTrue(*FString::Printf(TEXT("%s: each lake has its component"), *Name),
				Meshes.Find(100) && Meshes.Find(101) && Meshes.Find(102) && Meshes.Find(103));
			W.Tick();
			TestEqual(*FString::Printf(TEXT("%s: and no more once all four are drawn"), *Name), Meshes.Num(), 4);
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: the first tick draws all four (the behaviour before G282)"), *Name), Meshes.Num(), 4);
		}
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
