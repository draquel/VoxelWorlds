// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelSeamRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Seam-ownership registry — P0 scaffolding (SEAM_OWNERSHIP_ARCHITECTURE.md §2).
// Pure-logic tests (no RHI / no world), so they run headless. They pin the
// invariants P1+ depend on: canonical min-coordinate ownership, exactly-once
// boundary coverage across a multi-chunk lattice (incl. mixed LOD), dirty
// propagation to precisely the incident seams, and the all-participants-
// resident scheduling gate.
// ---------------------------------------------------------------------------

namespace VoxelSeamTestUtils
{
	/** Lexicographic (X, then Y, then Z) minimum of a set of coordinates. */
	static FIntVector LexMin(const TArray<FIntVector>& Coords)
	{
		check(Coords.Num() > 0);
		FIntVector Best = Coords[0];
		for (int32 i = 1; i < Coords.Num(); ++i)
		{
			const FIntVector& C = Coords[i];
			if (C.X < Best.X
				|| (C.X == Best.X && C.Y < Best.Y)
				|| (C.X == Best.X && C.Y == Best.Y && C.Z < Best.Z))
			{
				Best = C;
			}
		}
		return Best;
	}

	/** Register a cubic lattice [0,N)^3 into the registry, all resident, at a per-chunk LOD. */
	static void RegisterLattice(FVoxelSeamRegistry& Reg, int32 N, TFunctionRef<int32(const FIntVector&)> LODFn)
	{
		for (int32 z = 0; z < N; ++z)
		{
			for (int32 y = 0; y < N; ++y)
			{
				for (int32 x = 0; x < N; ++x)
				{
					const FIntVector C(x, y, z);
					Reg.RegisterChunk(C, /*ContentVersion*/ 1, /*MeshedLODLevel*/ LODFn(C));
				}
			}
		}
	}
}

// ===========================================================================
// 1. Ownership is deterministic and min-coordinate.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSeamOwnershipTest,
	"VoxelWorlds.Streaming.SeamRegistry.Ownership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSeamOwnershipTest::RunTest(const FString& Parameters)
{
	using namespace VoxelSeamTestUtils;

	// A spread of chunk origins including negatives (the demo grid is WorldOrigin-offset).
	const TArray<FIntVector> Origins = {
		FIntVector(0, 0, 0), FIntVector(5, -3, 2), FIntVector(-7, -7, -7), FIntVector(-1, 4, -9)
	};

	for (const FIntVector& C : Origins)
	{
		TArray<FVoxelSeamKey> Incident;
		FVoxelSeamRegistry::EnumerateIncidentSeams(C, Incident);

		// A chunk is incident to exactly 26 seams (6 faces + 12 edges + 8 corners), all distinct.
		TestEqual(TEXT("26 incident seams"), Incident.Num(), 26);
		TSet<FVoxelSeamKey> Unique(Incident);
		TestEqual(TEXT("26 incident seams are distinct"), Unique.Num(), 26);

		// Determinism: re-enumeration yields the identical ordered list.
		TArray<FVoxelSeamKey> Incident2 = FVoxelSeamRegistry::EnumerateIncidentSeams(C);
		TestEqual(TEXT("enumeration is deterministic (count)"), Incident2.Num(), Incident.Num());
		bool bSameOrder = true;
		for (int32 i = 0; i < Incident.Num() && i < Incident2.Num(); ++i)
		{
			bSameOrder &= (Incident[i] == Incident2[i]);
		}
		TestTrue(TEXT("enumeration is deterministic (order)"), bSameOrder);

		int32 FaceCount = 0, EdgeCount = 0, CornerCount = 0;
		for (const FVoxelSeamKey& Key : Incident)
		{
			TArray<FIntVector> Participants;
			FVoxelSeamRegistry::GetParticipants(Key, Participants);

			// Participant count matches the seam type.
			const int32 Expected = FVoxelSeamRegistry::GetParticipantCount(Key.Type);
			TestEqual(TEXT("participant count matches type"), Participants.Num(), Expected);

			switch (Key.Type)
			{
			case EVoxelSeamType::Face:   ++FaceCount;   TestEqual(TEXT("face has 2 participants"), Participants.Num(), 2); break;
			case EVoxelSeamType::Edge:   ++EdgeCount;   TestEqual(TEXT("edge has 4 participants"), Participants.Num(), 4); break;
			case EVoxelSeamType::Corner: ++CornerCount; TestEqual(TEXT("corner has 8 participants"), Participants.Num(), 8); break;
			}

			// Owner is the component-wise minimum (== lexicographic minimum for an axis-aligned box)...
			const FIntVector ComputedOwner = FVoxelSeamRegistry::ComputeOwner(Participants);
			TestTrue(TEXT("key owner == component-wise min"), Key.Owner == ComputedOwner);
			TestTrue(TEXT("key owner == lexicographic min"), Key.Owner == LexMin(Participants));

			// ...and the owner is itself one of the participants (the lower corner).
			TestTrue(TEXT("owner is a participant"), Participants.Contains(Key.Owner));

			// This chunk C is always among the participants of every seam it is incident to.
			TestTrue(TEXT("incident chunk is a participant"), Participants.Contains(C));
		}
		TestEqual(TEXT("6 face seams"), FaceCount, 6);
		TestEqual(TEXT("12 edge seams"), EdgeCount, 12);
		TestEqual(TEXT("8 corner seams"), CornerCount, 8);
	}

	return true;
}

// ===========================================================================
// 2. Every physical boundary is owned exactly once across a lattice (no
//    duplication, no gaps), and ownership is independent of LOD (mixed-LOD).
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSeamCoverageTest,
	"VoxelWorlds.Streaming.SeamRegistry.Coverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSeamCoverageTest::RunTest(const FString& Parameters)
{
	using namespace VoxelSeamTestUtils;

	// 2x2x2 lattice of chunk origins {0,1}^3.
	constexpr int32 N = 2;
	TSet<FIntVector> Lattice;
	for (int32 z = 0; z < N; ++z)
		for (int32 y = 0; y < N; ++y)
			for (int32 x = 0; x < N; ++x)
				Lattice.Add(FIntVector(x, y, z));

	// Gather every incident seam key from every chunk, counting how many chunks list each key.
	TMap<FVoxelSeamKey, int32> KeyListCount;
	for (const FIntVector& C : Lattice)
	{
		TArray<FVoxelSeamKey> Incident;
		FVoxelSeamRegistry::EnumerateIncidentSeams(C, Incident);
		for (const FVoxelSeamKey& Key : Incident)
		{
			KeyListCount.FindOrAdd(Key)++;
		}
	}

	// A boundary is "interior" (fully inside the lattice) iff ALL its participants are in the lattice.
	// For each interior seam: every participant must have listed it exactly once, so the list count
	// equals the participant count (2 / 4 / 8) — that is exactly-once ownership with no gaps and no
	// duplication (a canonical key is shared by all sides, never split).
	int32 InteriorFaces = 0, InteriorEdges = 0, InteriorCorners = 0;
	for (const TPair<FVoxelSeamKey, int32>& Pair : KeyListCount)
	{
		const FVoxelSeamKey& Key = Pair.Key;
		TArray<FIntVector> Participants;
		FVoxelSeamRegistry::GetParticipants(Key, Participants);

		bool bAllInside = true;
		for (const FIntVector& P : Participants)
		{
			bAllInside &= Lattice.Contains(P);
		}
		if (!bAllInside)
		{
			continue; // boundary seam extending outside the lattice — expected to be listed fewer times
		}

		const int32 ParticipantCount = FVoxelSeamRegistry::GetParticipantCount(Key.Type);
		// Exactly-once: listed by every participant, no more, no fewer.
		TestEqual(TEXT("interior seam listed once per participant (owned exactly once)"),
			Pair.Value, ParticipantCount);
		// The owner lies inside the lattice.
		TestTrue(TEXT("interior seam owner in lattice"), Lattice.Contains(Key.Owner));

		switch (Key.Type)
		{
		case EVoxelSeamType::Face:   ++InteriorFaces;   break;
		case EVoxelSeamType::Edge:   ++InteriorEdges;   break;
		case EVoxelSeamType::Corner: ++InteriorCorners; break;
		}
	}

	// Combinatorial expectation for a 2x2x2 lattice: 12 shared faces, 6 shared edges, 1 shared corner.
	TestEqual(TEXT("interior faces (no gaps)"), InteriorFaces, 12);
	TestEqual(TEXT("interior edges (no gaps)"), InteriorEdges, 6);
	TestEqual(TEXT("interior corners (no gaps)"), InteriorCorners, 1);

	// No-gaps, explicit: every adjacent face pair maps to the SAME canonical key from both sides.
	for (const FIntVector& C : Lattice)
	{
		static const FIntVector Faces[3] = { FIntVector(1,0,0), FIntVector(0,1,0), FIntVector(0,0,1) };
		for (const FIntVector& F : Faces)
		{
			const FIntVector Neighbor = C + F;
			if (!Lattice.Contains(Neighbor))
			{
				continue;
			}
			// C's +face seam and Neighbor's -face seam must be the identical canonical key.
			const uint8 Axis = (F.X != 0) ? 0 : (F.Y != 0) ? 1 : 2;
			const FVoxelSeamKey FromLow(C, EVoxelSeamType::Face, Axis);
			// The key both sides produce is owned by the min == C.
			TestTrue(TEXT("shared face owned by min-coordinate chunk"), FromLow.Owner == C);
			TArray<FIntVector> P = FVoxelSeamRegistry::GetParticipants(FromLow);
			TestTrue(TEXT("shared face participants include both chunks"),
				P.Contains(C) && P.Contains(Neighbor));
		}
	}

	// Mixed-LOD independence: registering the lattice with wildly different per-chunk LODs must yield
	// the SAME set of seam keys as a uniform-LOD registration (ownership is coordinate-only).
	FVoxelSeamRegistry RegUniform; RegUniform.SetEnabled(true);
	RegisterLattice(RegUniform, N, [](const FIntVector&){ return 0; });

	FVoxelSeamRegistry RegMixed; RegMixed.SetEnabled(true);
	RegisterLattice(RegMixed, N, [](const FIntVector& C){ return (C.X + C.Y + C.Z) % 3; });

	TArray<FVoxelSeamKey> UniformKeysArr; RegUniform.GetAllSeamKeys(UniformKeysArr);
	TArray<FVoxelSeamKey> MixedKeysArr;   RegMixed.GetAllSeamKeys(MixedKeysArr);
	TSet<FVoxelSeamKey> UniformKeys(UniformKeysArr);
	TSet<FVoxelSeamKey> MixedKeys(MixedKeysArr);

	TestEqual(TEXT("mixed-LOD seam count == uniform-LOD seam count"), MixedKeys.Num(), UniformKeys.Num());
	TestTrue(TEXT("mixed-LOD seam set == uniform-LOD seam set"),
		UniformKeys.Num() == MixedKeys.Num() && UniformKeys.Includes(MixedKeys));

	// And the tracked key set equals the independently-derived union of incident keys.
	TSet<FVoxelSeamKey> ExpectedUnion;
	for (const TPair<FVoxelSeamKey, int32>& Pair : KeyListCount)
	{
		ExpectedUnion.Add(Pair.Key);
	}
	TestEqual(TEXT("registry seam set size == static incident union"), UniformKeys.Num(), ExpectedUnion.Num());
	TestTrue(TEXT("registry seam set == static incident union"), ExpectedUnion.Includes(UniformKeys) && UniformKeys.Includes(ExpectedUnion));

	return true;
}

// ===========================================================================
// 3. Dirty propagation marks exactly the incident seams — no more, no fewer.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSeamDirtyPropagationTest,
	"VoxelWorlds.Streaming.SeamRegistry.DirtyPropagation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSeamDirtyPropagationTest::RunTest(const FString& Parameters)
{
	using namespace VoxelSeamTestUtils;

	// 3x3x3 lattice so the centre chunk (1,1,1)'s incident seams all have in-lattice participants.
	FVoxelSeamRegistry Reg; Reg.SetEnabled(true);
	RegisterLattice(Reg, 3, [](const FIntVector&){ return 0; });

	// Registration dirties incident seams; start from a clean slate to isolate one propagation.
	Reg.MarkAllSeamsClean();
	TestEqual(TEXT("no dirty seams after MarkAllSeamsClean"), Reg.GetDirtyCount(), 0);

	const FIntVector Center(1, 1, 1);
	const TSet<FVoxelSeamKey> Incident(FVoxelSeamRegistry::EnumerateIncidentSeams(Center));
	TestEqual(TEXT("centre has 26 incident seams"), Incident.Num(), 26);

	// A content change marks exactly the chunk's incident seams dirty.
	Reg.UpdateChunkContent(Center, /*ContentVersion*/ 2);
	{
		TArray<FVoxelSeamKey> DirtyArr; Reg.GetDirtySeamKeys(DirtyArr);
		TSet<FVoxelSeamKey> Dirty(DirtyArr);
		TestEqual(TEXT("content change dirties exactly 26 seams"), Dirty.Num(), 26);
		TestTrue(TEXT("content change dirties exactly the incident seams"),
			Dirty.Num() == Incident.Num() && Incident.Includes(Dirty) && Dirty.Includes(Incident));
	}

	// A rendered-LOD change also marks exactly the incident seams dirty.
	Reg.MarkAllSeamsClean();
	Reg.UpdateChunkRenderedLOD(Center, /*newLOD*/ 1);
	{
		TArray<FVoxelSeamKey> DirtyArr; Reg.GetDirtySeamKeys(DirtyArr);
		TSet<FVoxelSeamKey> Dirty(DirtyArr);
		TestEqual(TEXT("LOD change dirties exactly 26 seams"), Dirty.Num(), 26);
		TestTrue(TEXT("LOD change dirties exactly the incident seams"),
			Incident.Includes(Dirty) && Dirty.Includes(Incident));
	}

	// A no-op rendered-LOD update (same LOD) dirties nothing.
	Reg.MarkAllSeamsClean();
	Reg.UpdateChunkRenderedLOD(Center, /*newLOD*/ 1);
	TestEqual(TEXT("same-LOD update dirties nothing"), Reg.GetDirtyCount(), 0);

	return true;
}

// ===========================================================================
// 4. Scheduling gates on ALL participants being resident.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelSeamSchedulingGateTest,
	"VoxelWorlds.Streaming.SeamRegistry.Scheduling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelSeamSchedulingGateTest::RunTest(const FString& Parameters)
{
	FVoxelSeamRegistry Reg; Reg.SetEnabled(true);

	const FIntVector A(0, 0, 0);
	const FIntVector B(1, 0, 0);
	const FVoxelSeamKey FaceAB(A, EVoxelSeamType::Face, /*axis X*/ 0); // participants {A, B}

	// Only A is resident: none of A's incident seams can schedule (each needs a not-yet-present neighbour).
	Reg.RegisterChunk(A, /*version*/ 1, /*lod*/ 0);
	TestTrue(TEXT("Face(A,X) exists"), Reg.FindSeam(FaceAB) != nullptr);
	TestFalse(TEXT("Face(A,X) not ready with only A resident"), Reg.IsSeamReady(FaceAB));

	const int32 ScheduledNone = Reg.ScheduleReadySeams(/*viewer*/ A, /*nearRadius*/ 4, /*max*/ 0);
	TestEqual(TEXT("nothing schedules with a single resident chunk"), ScheduledNone, 0);
	TestEqual(TEXT("job queue empty"), Reg.GetJobQueueDepth(), 0);
	TestTrue(TEXT("Face(A,X) still dirty (retryable)"), Reg.IsSeamDirty(FaceAB));

	// Now B is resident: Face(A,X) is the ONLY seam whose participant set is exactly {A,B}; every
	// edge/corner needs 4/8 participants, so exactly one seam becomes schedulable.
	Reg.RegisterChunk(B, /*version*/ 1, /*lod*/ 0);
	TestTrue(TEXT("Face(A,X) ready once B resident"), Reg.IsSeamReady(FaceAB));

	const int32 Scheduled = Reg.ScheduleReadySeams(/*viewer*/ A, /*nearRadius*/ 4, /*max*/ 0);
	TestEqual(TEXT("exactly one seam schedules (only Face(A,X) is fully resident)"), Scheduled, 1);
	TestEqual(TEXT("job queue holds the one seam"), Reg.GetJobQueueDepth(), 1);
	TestFalse(TEXT("Face(A,X) no longer dirty once scheduled"), Reg.IsSeamDirty(FaceAB));

	// Near-viewer priority: A is at the viewer, so the seam gets the near-correction tier (65).
	const float Prio = FVoxelSeamRegistry::ComputeSeamPriority(A, /*viewer*/ A, /*nearRadius*/ 4);
	TestEqual(TEXT("near seam priority is the near-correction tier"), Prio, 65.0f);

	// The (P0 stub) processor drains the queue and produces no geometry.
	const int32 Processed = Reg.ProcessSeamJobQueue(/*max*/ 0);
	TestEqual(TEXT("processed the one queued seam"), Processed, 1);
	TestEqual(TEXT("job queue drained"), Reg.GetJobQueueDepth(), 0);

	const FVoxelSeamRegistryStats S = Reg.GetStats();
	TestEqual(TEXT("lifetime scheduled == 1"), (int32)S.TotalSeamJobsScheduled, 1);
	TestEqual(TEXT("lifetime processed == 1"), (int32)S.TotalSeamJobsProcessed, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
