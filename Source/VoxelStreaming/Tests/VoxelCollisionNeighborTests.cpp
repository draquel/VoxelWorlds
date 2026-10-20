// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "VoxelChunkManager.h"
#include "VoxelCollisionManager.h"
#include "VoxelWorldConfiguration.h"
#include "Testing/VoxelTestChunkWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G32 (WORLD_MODES_REVIEW_PLAN.md section 14.1): stale collision strips along chunk faces. A chunk's collision is cooked
// whole-chunk with its 26 neighbours' voxels, and the mesher reads a neighbour without data as air; a chunk cooked before
// its +X / +Y neighbour had data kept boundary cells built against that air (voxel-wide strips beside the face where
// down-traces miss the ground), and nothing re-cooked it when the neighbour arrived.
//
// - Collision.NeighborArrival: a streamed plane with collision on every loaded chunk. The window's edge chunks are cooked
//   while their outward neighbours are not loaded; the viewer then moves two chunks along +X +Y, so those neighbours
//   load. Down-traces along each such face (inside the chunk's last voxel cell, at every voxel along the face) are
//   compared with the same traces after re-cooking those chunks from scratch. With voxel.Collision.NeighborRecook 0 (the
//   code before the fix) they differ: the scenario reproduces G32. With 1 (the fix) they are identical, and the re-cooks
//   came from neighbour arrivals.
// ---------------------------------------------------------------------------

namespace VoxelCollisionNeighborTest
{
	/** Tick until the stream and the collision pipeline have both been idle for 90 ticks; false on timeout. */
	bool WaitForCollisionIdle(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds)
	{
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		if (!Collision)
		{
			return false;
		}
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
		{
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			const bool bIdle = Collision->GetCookQueueCount() == 0 && Collision->GetCookingCount() == 0
				&& W.Manager->GetPendingGenerationCount() == 0 && W.Manager->GetPendingMeshingCount() == 0
				&& W.Manager->GetAsyncGenerationInProgressCount() == 0 && W.Manager->GetAsyncMeshingInProgressCount() == 0;
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleTicks >= 90;
	}

	/** A trace's result when it hits nothing. */
	constexpr double MissZ = -1.0e30;

	/** A chunk face whose outward neighbour (+X when Axis is 0, +Y when 1) had no data when the chunk was cooked. */
	struct FFace
	{
		FIntVector Chunk;
		int32 Axis = 0;
	};

	/**
	 * Down-traces inside the chunk's last voxel cell along the face, one per voxel along it, confined to the chunk's own
	 * height. The first world-static hit's Z, or MissZ for a miss.
	 */
	TArray<double> TraceFace(const VoxelTesting::FTestChunkWorld& W, const FFace& Face)
	{
		const UVoxelWorldConfiguration* Config = W.Config;
		const double ChunkWorld = Config->GetChunkWorldSize();
		const double Voxel = Config->VoxelSize;
		const int32 ChunkSize = Config->ChunkSize;
		const FVector ChunkMin = Config->WorldOrigin + FVector(Face.Chunk) * ChunkWorld;
		const int32 Along = 1 - Face.Axis;
		FCollisionQueryParams Params(FName(TEXT("CollisionNeighborArrival")), /*bTraceComplex=*/true);

		TArray<double> Out;
		Out.Reserve(ChunkSize);
		for (int32 I = 0; I < ChunkSize; ++I)
		{
			FVector P = ChunkMin;
			P[Face.Axis] = ChunkMin[Face.Axis] + ChunkWorld - 0.25 * Voxel;
			P[Along] = ChunkMin[Along] + (I + 0.5) * Voxel;
			const FVector Start(P.X, P.Y, ChunkMin.Z + ChunkWorld - 1.0);
			const FVector End(P.X, P.Y, ChunkMin.Z + 1.0);
			FHitResult Hit;
			const bool bHit = W.World->LineTraceSingleByObjectType(Hit, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), Params);
			Out.Add(bHit ? Hit.ImpactPoint.Z : MissZ);
		}
		return Out;
	}

	struct FPassResult
	{
		int32 Faces = 0;
		int32 Probes = 0;      // samples that hit in the fresh cook
		int32 Mismatches = 0;  // samples whose stale result differs from the fresh one (a hit against a miss, or > 1 uu)
		int32 StaleMisses = 0; // probes the stale cook missed
		int64 NeighborRecooks = 0;
		int64 ArrivalsSkipped = 0; // arrivals with air on both sides of the boundary: no re-cook needed
		int64 ArrivalsSkippedSolid = 0; // arrivals with solid on both sides: the wall stays buried in the rock
		int32 BuriedHits = 0;  // stale hits where a fresh cook has nothing (walls inside solid rock)
		int32 AuditStale = 0;  // AuditNeighborCooks: cooks that read as air a neighbour with data now, no re-cook pending
		int32 AuditCooked = 0;
		bool bSettled = false;
	};

	FPassResult RunPass(FAutomationTestBase& Test, bool bRecook)
	{
		FPassResult R;
		// Set at console priority: a value typed in the editor's console outranks a code set, which would be ignored.
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Collision.NeighborRecook"));
		const int32 Previous = CVar ? CVar->GetInt() : 1;
		if (CVar)
		{
			CVar->Set(bRecook ? 1 : 0, ECVF_SetByConsole);
		}
		if (!CVar || CVar->GetInt() != (bRecook ? 1 : 0))
		{
			Test.AddError(FString::Printf(TEXT("voxel.Collision.NeighborRecook could not be set to %d"), bRecook ? 1 : 0));
		}

		VoxelTesting::FTestChunkWorld W;
		W.Create(TEXT("CollisionNeighborArrival"));
		W.ConfigureSmallPlane();
		W.Config->bGenerateCollision = true;
		W.Config->CollisionLODLevel = 0;
		W.StartManager();
		UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
		if (!Collision)
		{
			Test.AddError(TEXT("The chunk manager has a collision manager"));
			W.Stop();
			if (CVar) { CVar->Set(Previous, ECVF_SetByConsole); }
			return R;
		}
		// Cook every loaded chunk, the window's edge included (the default radius is half the view distance).
		Collision->SetCollisionRadius(W.Config->ViewDistance * 3.0f);

		const double ChunkWorld = W.Config->GetChunkWorldSize();
		const FVector V0(ChunkWorld * 0.5, ChunkWorld * 0.5, 1200.0);
		W.SetViewer(V0);
		R.bSettled = W.Settle() && WaitForCollisionIdle(W, 120.0);

		// The edge faces: a chunk with a cooked body whose +X or +Y neighbour is not loaded.
		TArray<FIntVector> Loaded;
		W.Manager->GetLoadedChunks(Loaded);
		TArray<FFace> Candidates;
		for (const FIntVector& Chunk : Loaded)
		{
			if (!Collision->GetChunkBodySetup(Chunk))
			{
				continue;
			}
			for (int32 Axis = 0; Axis < 2; ++Axis)
			{
				FIntVector Next = Chunk;
				Next[Axis] += 1;
				if (W.Manager->GetChunkState(Next) == EChunkState::Unloaded)
				{
					Candidates.Add({ Chunk, Axis });
				}
			}
		}

		// Two chunks along +X +Y: the outward neighbours of those faces load, the chunks stay in the window.
		W.SetViewer(V0 + FVector(2.0 * ChunkWorld, 2.0 * ChunkWorld, 0.0));
		R.bSettled = W.Settle() && WaitForCollisionIdle(W, 120.0) && R.bSettled;

		TArray<FFace> Faces;
		TSet<FIntVector> FaceChunks;
		for (const FFace& Face : Candidates)
		{
			FIntVector Next = Face.Chunk;
			Next[Face.Axis] += 1;
			if (Collision->GetChunkBodySetup(Face.Chunk) && W.Manager->GetChunkState(Next) == EChunkState::Loaded)
			{
				Faces.Add(Face);
				FaceChunks.Add(Face.Chunk);
			}
		}
		R.Faces = Faces.Num();

		TArray<TArray<double>> Stale;
		for (const FFace& Face : Faces)
		{
			Stale.Add(TraceFace(W, Face));
		}
		R.NeighborRecooks = Collision->GetTotalNeighborRecooks();
		R.ArrivalsSkipped = Collision->GetTotalNeighborArrivalsSkipped();
		R.ArrivalsSkippedSolid = Collision->GetTotalNeighborArrivalsSkippedSolid();
		{
			int32 WithMissing = 0, Awaiting = 0;
			Collision->AuditNeighborCooks(R.AuditCooked, WithMissing, R.AuditStale, Awaiting);
		}

		// The reference: the same chunks cooked from scratch, every neighbour loaded.
		for (const FIntVector& Chunk : FaceChunks)
		{
			Collision->RegenerateChunkCollision(Chunk);
		}
		R.bSettled = WaitForCollisionIdle(W, 120.0) && R.bSettled;

		for (int32 F = 0; F < Faces.Num(); ++F)
		{
			const TArray<double> Fresh = TraceFace(W, Faces[F]);
			for (int32 I = 0; I < Fresh.Num(); ++I)
			{
				const bool bFreshHit = Fresh[I] != MissZ;
				const bool bStaleHit = Stale[F][I] != MissZ;
				R.Probes += bFreshHit ? 1 : 0;
				R.StaleMisses += (bFreshHit && !bStaleHit) ? 1 : 0;
				// The ground a fresh cook has: missing or moved in the stale cook is G32. A stale hit where the fresh cook has
				// nothing is a wall buried in solid rock (an arrival with solid on both sides keeps it; no trace from the open
				// reaches it), counted apart.
				if (bFreshHit && (!bStaleHit || FMath::Abs(Fresh[I] - Stale[F][I]) > 1.0))
				{
					++R.Mismatches;
				}
				R.BuriedHits += (!bFreshHit && bStaleHit) ? 1 : 0;
			}
		}

		W.Stop();
		if (CVar)
		{
			CVar->Set(Previous, ECVF_SetByConsole);
		}
		return R;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionNeighborArrivalTest, "VoxelWorlds.Collision.NeighborArrival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelCollisionNeighborArrivalTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCollisionNeighborTest;

	const FPassResult Off = RunPass(*this, /*bRecook=*/false);
	AddInfo(FString::Printf(TEXT("NeighborRecook 0 (before the fix): %d faces cooked before their neighbour loaded, %d probes, %d differ from a fresh cook (%d missed), %d hits buried in rock; %lld neighbour re-cooks (arrivals needing none: %lld air, %lld solid on both sides); audit: %d of %d cooked chunks stale"),
		Off.Faces, Off.Probes, Off.Mismatches, Off.StaleMisses, Off.BuriedHits, Off.NeighborRecooks, Off.ArrivalsSkipped, Off.ArrivalsSkippedSolid, Off.AuditStale, Off.AuditCooked));
	TestTrue(TEXT("NeighborRecook 0: streaming and collision settle"), Off.bSettled);
	TestTrue(TEXT("NeighborRecook 0: faces cooked before their neighbour loaded (non-vacuous)"), Off.Faces > 0 && Off.Probes > 0);
	TestTrue(TEXT("NeighborRecook 0: the stale faces differ from a fresh cook (the scenario reproduces G32)"), Off.Mismatches > 0);
	TestTrue(TEXT("NeighborRecook 0: the audit finds the stale cooks"), Off.AuditStale > 0);

	const FPassResult On = RunPass(*this, /*bRecook=*/true);
	AddInfo(FString::Printf(TEXT("NeighborRecook 1 (the fix): %d faces cooked before their neighbour loaded, %d probes, %d differ from a fresh cook (%d missed), %d hits buried in rock; %lld neighbour re-cooks (arrivals needing none: %lld air, %lld solid on both sides); audit: %d of %d cooked chunks stale"),
		On.Faces, On.Probes, On.Mismatches, On.StaleMisses, On.BuriedHits, On.NeighborRecooks, On.ArrivalsSkipped, On.ArrivalsSkippedSolid, On.AuditStale, On.AuditCooked));
	TestTrue(TEXT("NeighborRecook 1: streaming and collision settle"), On.bSettled);
	TestTrue(TEXT("NeighborRecook 1: faces cooked before their neighbour loaded (non-vacuous)"), On.Faces > 0 && On.Probes > 0);
	TestEqual(TEXT("NeighborRecook 1: every face equals a fresh cook once its neighbour loaded"), On.Mismatches, 0);
	TestTrue(TEXT("NeighborRecook 1: the re-cooks came from neighbour arrivals"), On.NeighborRecooks > 0);
	TestEqual(TEXT("NeighborRecook 1: no cook reads as air a neighbour that has data"), On.AuditStale, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCollisionEditNeighborsTest, "VoxelWorlds.Collision.EditNeighbors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelCollisionEditNeighborsTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCollisionNeighborTest;

	// A collision cook reads its edge and corner neighbours' voxels too, so an edit near a shared edge or corner re-cooks
	// those neighbours (with arrivals that have solid on both sides skipped, this also re-cooks a wall buried there before
	// a dig exposes it). The chunk manager marks the chunk and its face neighbours itself.
	VoxelTesting::FTestChunkWorld W;
	W.Create(TEXT("CollisionEditNeighbors"));
	W.ConfigureSmallPlane();
	W.Config->bGenerateCollision = true;
	W.Config->CollisionLODLevel = 0;
	W.StartManager();
	UVoxelCollisionManager* Collision = W.Manager->GetCollisionManager();
	if (!TestNotNull(TEXT("The chunk manager has a collision manager"), Collision))
	{
		W.Stop();
		return false;
	}
	Collision->SetCollisionRadius(W.Config->ViewDistance * 3.0f);
	const double ChunkWorld = W.Config->GetChunkWorldSize();
	const double Voxel = W.Config->VoxelSize;
	W.SetViewer(FVector(ChunkWorld * 0.5, ChunkWorld * 0.5, 1200.0));
	const bool bSettled = W.Settle() && WaitForCollisionIdle(W, 120.0);
	TestTrue(TEXT("Streaming and collision settle"), bSettled);

	// The chunk at the origin and its 26 neighbours, all cooked (the plane's surface layer is z 0 or -1; take the chunk
	// whose neighbourhood is fully cooked).
	FIntVector Center(0, 0, 0);
	bool bFound = false;
	for (const int32 Z : { 0, -1 })
	{
		bool bAll = true;
		for (int32 DZ = -1; DZ <= 1 && bAll; ++DZ)
		for (int32 DY = -1; DY <= 1 && bAll; ++DY)
		for (int32 DX = -1; DX <= 1 && bAll; ++DX)
		{
			bAll = Collision->HasCollision(FIntVector(DX, DY, Z + DZ));
		}
		if (bAll)
		{
			Center = FIntVector(0, 0, Z);
			bFound = true;
			break;
		}
	}
	TestTrue(TEXT("A chunk with its 26 neighbours cooked (non-vacuous)"), bFound);

	const FVector ChunkMin = W.Config->WorldOrigin + FVector(Center) * ChunkWorld;
	const int32 MidEdit = Collision->MarkNeighborsDirtyForEdit(Center, ChunkMin + FVector(ChunkWorld * 0.5), 2.0f * Voxel);
	const int32 FaceEdit = Collision->MarkNeighborsDirtyForEdit(Center, ChunkMin + FVector(ChunkWorld - Voxel, ChunkWorld * 0.5, ChunkWorld * 0.5), 2.0f * Voxel);
	const int32 EdgeEdit = Collision->MarkNeighborsDirtyForEdit(Center, ChunkMin + FVector(ChunkWorld - Voxel, ChunkWorld - Voxel, ChunkWorld * 0.5), 2.0f * Voxel);
	const int32 CornerEdit = Collision->MarkNeighborsDirtyForEdit(Center, ChunkMin + FVector(ChunkWorld - Voxel), 2.0f * Voxel);
	const int32 Unknown = Collision->MarkNeighborsDirtyForEdit(Center, ChunkMin + FVector(ChunkWorld * 0.5), 0.0f);
	AddInfo(FString::Printf(TEXT("Edge and corner neighbours marked: an edit mid-chunk %d, by a face %d, by an edge %d, by a corner %d, radius unknown %d"),
		MidEdit, FaceEdit, EdgeEdit, CornerEdit, Unknown));
	TestEqual(TEXT("An edit mid-chunk reaches no edge or corner neighbour"), MidEdit, 0);
	TestEqual(TEXT("An edit by a face reaches no edge or corner neighbour"), FaceEdit, 0);
	TestEqual(TEXT("An edit by an edge reaches the edge neighbour there"), EdgeEdit, 1);
	TestEqual(TEXT("An edit by a corner reaches its three edge neighbours and the corner neighbour"), CornerEdit, 4);
	TestEqual(TEXT("An edit of unknown radius (undo, redo) marks every edge and corner neighbour"), Unknown, 20);
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
