// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h" // MovementBaseUtility
#include "Interfaces/MovementBaseInterface.h"
#include "UObject/UObjectIterator.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelTestChunkWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G215 (Documentation/FINDINGS.md): a chunk's collision component kept a scene component's default mobility, Movable, so
// the engine took the terrain for a dynamic movement base. A character on a dynamic base sends and receives its position
// relative to the base, and the component exists on each machine on its own, so a client dropped every server correction
// and a dedicated server corrected every move of a client on terrain.
//
// - Collision.StaticBase: in each kind of world (a smooth plane, a block world, a planet) every cooked chunk's component
//   is static, attached to the container's static root, registered with a physics body, at its chunk's corner (it is
//   placed before it is registered: a static component is not moved afterwards), and is not a dynamic base to the engine
//   (MovementBaseUtility::IsDynamicBase, what the character movement asks). A re-cook, as an edit makes, replaces the
//   component with another that holds the same: the body is made again, never moved.
//
// The replicated behaviour (a client follows a server correction on terrain; a client's walk logs none) needs a net
// driver: VoxelEngine.Replication.TerrainBase in the game module, a dedicated server and a client in PIE.
// ---------------------------------------------------------------------------

namespace VoxelCollisionStaticBaseTest
{
	/** Tick until the stream and the collision pipeline have both been idle for 60 ticks; false on timeout. */
	bool WaitForStreamAndCollision(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds)
	{
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		if (!Collision)
		{
			return false;
		}
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleTicks < 60 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			const bool bIdle = W.IsIdle() && Collision->GetCookQueueCount() == 0 && Collision->GetCookingCount() == 0;
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 60;
	}

	/** What one sweep over a world's live chunk collision components found. */
	struct FSweep
	{
		int32 Components = 0;
		int32 NotStatic = 0;       // the component's own mobility
		int32 DynamicBases = 0;    // what the character movement asks of a base (or not usable as one)
		int32 Unattached = 0;      // no parent, or a parent that is not the container's static root
		int32 NoBody = 0;          // not registered, or no physics body
		int32 Misplaced = 0;       // not at its chunk's corner
		/** The component of one chunk (the first found), to follow across a re-cook. */
		TWeakObjectPtr<UVoxelCollisionComponent> Sample;
		FIntVector SampleChunk = FIntVector::ZeroValue;
	};

	FSweep SweepComponents(const VoxelTesting::FTestChunkWorld& W)
	{
		FSweep S;
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		for (TObjectIterator<UVoxelCollisionComponent> It; It; ++It)
		{
			UVoxelCollisionComponent* Component = *It;
			if (!IsValid(Component) || Component->GetWorld() != W.World)
			{
				continue;
			}
			++S.Components;
			S.NotStatic += Component->Mobility != EComponentMobility::Static ? 1 : 0;

			// As the character movement holds its base. An invalid one would read as "not dynamic" whatever its mobility.
			const FMovementBaseInterfaceData Base{ TObjectPtr<UObject>(Component) };
			S.DynamicBases += (!Base.IsValid() || MovementBaseUtility::IsDynamicBase(&Base)) ? 1 : 0;

			const USceneComponent* Parent = Component->GetAttachParent();
			const AActor* Container = Component->GetOwner();
			S.Unattached += (!Parent || !Container || Parent != Container->GetRootComponent() || Parent->Mobility != EComponentMobility::Static) ? 1 : 0;
			S.NoBody += (!Component->IsRegistered() || !Component->BodyInstance.IsValidBodyInstance()) ? 1 : 0;

			// The chunk the component's box is in, and that chunk's corner.
			const FVector Location = Component->GetComponentLocation();
			const FIntVector Chunk = W.Manager->WorldToChunkCoord(Location + FVector(ChunkWorld * 0.5));
			const FVector Corner = W.Config->WorldOrigin + FVector(Chunk) * ChunkWorld;
			S.Misplaced += !Location.Equals(Corner, 0.01) ? 1 : 0;

			if (!S.Sample.IsValid())
			{
				S.Sample = Component;
				S.SampleChunk = Chunk;
			}
		}
		return S;
	}

	/** The live component of a chunk, or null. */
	UVoxelCollisionComponent* FindComponent(const VoxelTesting::FTestChunkWorld& W, const FIntVector& Chunk)
	{
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		for (TObjectIterator<UVoxelCollisionComponent> It; It; ++It)
		{
			UVoxelCollisionComponent* Component = *It;
			if (IsValid(Component) && Component->GetWorld() == W.World && Component->IsRegistered()
				&& W.Manager->WorldToChunkCoord(Component->GetComponentLocation() + FVector(ChunkWorld * 0.5)) == Chunk)
			{
				return Component;
			}
		}
		return nullptr;
	}

	void RunWorld(FAutomationTestBase& Test, const TCHAR* Name, TFunctionRef<void(VoxelTesting::FTestChunkWorld&)> Configure,
		TFunctionRef<FVector(const VoxelTesting::FTestChunkWorld&)> Viewer)
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(Name);
		Configure(W);
		W.Config->bGenerateCollision = true;
		W.Config->CollisionLODLevel = 0;
		W.StartManager();
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		if (!Test.TestNotNull(FString::Printf(TEXT("%s: the chunk manager has a collision manager"), Name), Collision))
		{
			W.Stop();
			return;
		}
		W.SetViewer(Viewer(W));
		Test.TestTrue(FString::Printf(TEXT("%s: streaming and collision settle"), Name), W.Settle() && WaitForStreamAndCollision(W, 120.0));

		const FSweep First = SweepComponents(W);
		Test.AddInfo(FString::Printf(TEXT("%s: %d chunk collision components"), Name, First.Components));
		Test.TestTrue(FString::Printf(TEXT("%s: chunks have collision components (non-vacuous)"), Name), First.Components > 0);
		Test.TestEqual(FString::Printf(TEXT("%s: every component is static"), Name), First.NotStatic, 0);
		Test.TestEqual(FString::Printf(TEXT("%s: no component is a dynamic movement base"), Name), First.DynamicBases, 0);
		Test.TestEqual(FString::Printf(TEXT("%s: every component is attached to the container's static root"), Name), First.Unattached, 0);
		Test.TestEqual(FString::Printf(TEXT("%s: every component is registered with a physics body"), Name), First.NoBody, 0);
		Test.TestEqual(FString::Printf(TEXT("%s: every component is at its chunk's corner"), Name), First.Misplaced, 0);

		// A re-cook, as an edit makes: the old body stays until the new one replaces it, and the new one is another component.
		UVoxelCollisionComponent* Before = First.Sample.Get();
		if (Before)
		{
			Collision->MarkChunkDirty(First.SampleChunk);
			Test.TestTrue(FString::Printf(TEXT("%s: the re-cook settles"), Name), WaitForStreamAndCollision(W, 120.0));
			UVoxelCollisionComponent* After = FindComponent(W, First.SampleChunk);
			Test.TestNotNull(FString::Printf(TEXT("%s: the re-cooked chunk has a component"), Name), After);
			Test.TestTrue(FString::Printf(TEXT("%s: the re-cook replaced the component (the body is made again, not moved)"), Name),
				After && After != Before && (!IsValid(Before) || !Before->IsRegistered()));

			const FSweep Second = SweepComponents(W);
			Test.TestEqual(FString::Printf(TEXT("%s: after the re-cook every component is static, attached, registered, in place and no dynamic base"), Name),
				Second.NotStatic + Second.DynamicBases + Second.Unattached + Second.NoBody + Second.Misplaced, 0);
			Test.TestEqual(FString::Printf(TEXT("%s: the re-cook left as many components"), Name), Second.Components, First.Components);
		}
		W.Stop();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionStaticBaseTest, "VoxelWorlds.Collision.StaticBase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelCollisionStaticBaseTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCollisionStaticBaseTest;

	auto OverPlane = [](const VoxelTesting::FTestChunkWorld& W)
	{
		const double ChunkWorld = W.Config->GetChunkWorldSize();
		return W.ViewerOverGround(ChunkWorld * 0.5, ChunkWorld * 0.5, 200.0f);
	};

	RunWorld(*this, TEXT("CollisionStaticBasePlane"),
		[](VoxelTesting::FTestChunkWorld& W) { W.ConfigureSmallPlane(); }, OverPlane);

	RunWorld(*this, TEXT("CollisionStaticBaseBlocks"),
		[](VoxelTesting::FTestChunkWorld& W)
		{
			W.ConfigureSmallPlane();
			W.Config->MeshingMode = EMeshingMode::Cubic;
		}, OverPlane);

	RunWorld(*this, TEXT("CollisionStaticBasePlanet"),
		[](VoxelTesting::FTestChunkWorld& W) { W.ConfigureSmallPlanet(); },
		[](const VoxelTesting::FTestChunkWorld& W) { return W.ViewerOverSurface(FVector(0.0, 0.0, 1.0), 200.0f); });

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
