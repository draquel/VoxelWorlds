// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Hash/CityHash.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelSeamRegistry.h"
#include "ChunkRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// G115 (Documentation/FINDINGS.md): voxel.Mesh.RemeshAll is how a mesher cvar changed mid-session is seen (the meshers read
// theirs per job), and it re-meshed nothing while streaming was paused, the usual way to hold a scene for an A/B
// (`vox.Freeze 1 streaming`): the paused tick launched the chunks a recreated scene proxy lost (F3.4) and nothing else, so
// the dirtied chunks and their restaled seams waited for the resume and the shots showed the old meshes.
//
// - Streaming.RemeshAll.Planet / .Plane: a real chunk manager streams a small world with three LOD bands (a Dual Contouring
//   planet, a Marching Cubes plane) into a renderer that records what it is given. Once settled, RemeshAllLoadedChunks with
//   streaming running and then with it paused: every loaded chunk gets a new mesh job and its mesh submitted again, every
//   seam bucket is submitted again, no chunk loads, unloads or changes LOD, and the geometry equals what was there (the
//   meshers are deterministic, so a re-mesh with nothing changed gives the same meshes). The paused case resumes after.
// ---------------------------------------------------------------------------

namespace VoxelRemeshAllTest
{
	/** A mesh's positions, normals and indices as one hash. */
	uint64 MeshDigest(const FChunkMeshData& Mesh)
	{
		uint64 H = CityHash64(reinterpret_cast<const char*>(Mesh.Positions.GetData()), Mesh.Positions.Num() * sizeof(FVector3f));
		H = CityHash64WithSeed(reinterpret_cast<const char*>(Mesh.Normals.GetData()), Mesh.Normals.Num() * sizeof(FVector3f), H);
		return CityHash64WithSeed(reinterpret_cast<const char*>(Mesh.Indices.GetData()), Mesh.Indices.Num() * sizeof(uint32), H);
	}

	/** What the renderer holds, by chunk and by seam owner. */
	struct FHeld
	{
		TMap<FIntVector, uint64> Chunks;
		TMap<FIntVector, uint64> Seams;
	};

	FHeld Capture(const VoxelTesting::FRecordingMeshRenderer& Renderer)
	{
		FHeld Held;
		for (const TPair<FIntVector, FChunkMeshData>& Pair : Renderer.ChunkMeshes)
		{
			Held.Chunks.Add(Pair.Key, MeshDigest(Pair.Value));
		}
		for (const TPair<FIntVector, FChunkMeshData>& Pair : Renderer.SeamBuckets)
		{
			Held.Seams.Add(Pair.Key, MeshDigest(Pair.Value));
		}
		return Held;
	}

	/** Keys of A whose value B lacks or holds differently. */
	int32 CountDiffering(const TMap<FIntVector, uint64>& A, const TMap<FIntVector, uint64>& B)
	{
		int32 Differing = 0;
		for (const TPair<FIntVector, uint64>& Pair : A)
		{
			const uint64* Other = B.Find(Pair.Key);
			Differing += (!Other || *Other != Pair.Value) ? 1 : 0;
		}
		return Differing + FMath::Max(0, B.Num() - A.Num());
	}

	/** Keys of Before's renderer map whose submit count did not grow. */
	int32 CountNotResubmitted(const TMap<FIntVector, uint64>& Held, const TMap<FIntVector, int32>& CountsBefore, const TMap<FIntVector, int32>& CountsAfter)
	{
		int32 Missing = 0;
		for (const TPair<FIntVector, uint64>& Pair : Held)
		{
			const int32* Before = CountsBefore.Find(Pair.Key);
			const int32* After = CountsAfter.Find(Pair.Key);
			Missing += ((After ? *After : 0) > (Before ? *Before : 0)) ? 0 : 1;
		}
		return Missing;
	}

	/** Tick until nothing is queued or in flight, the seam registry's job queue included, for 90 ticks. */
	bool Settle(VoxelTesting::FTestChunkWorld& W, double TimeoutSeconds = 180.0)
	{
		const FVoxelSeamRegistry* Seams = W.Manager->GetSeamRegistry();
		int32 IdleTicks = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
		{
			W.Tick();
			const bool bIdle = W.IsIdle() && (!Seams || Seams->GetStats().JobQueueDepth == 0);
			IdleTicks = bIdle ? IdleTicks + 1 : 0;
		}
		return IdleTicks >= 90;
	}

	/** One RemeshAllLoadedChunks on a settled world, streaming running or paused, checked as the header says. */
	void CheckRemeshAll(FAutomationTestBase& Test, VoxelTesting::FTestChunkWorld& W, const FString& Name, bool bPaused)
	{
		const FString Label = FString::Printf(TEXT("%s, streaming %s"), *Name, bPaused ? TEXT("paused") : TEXT("running"));
		if (bPaused)
		{
			W.Manager->SetStreamingEnabled(false);
		}

		TArray<FIntVector> LoadedBefore;
		W.Manager->GetLoadedChunks(LoadedBefore);
		const TMap<FIntVector, int32> LODsBefore = W.Renderer.ChunkLODs;
		const FHeld Before = Capture(W.Renderer);
		const TMap<FIntVector, int32> ChunkCountsBefore = W.Renderer.ChunkSubmitsByChunk;
		const TMap<FIntVector, int32> SeamCountsBefore = W.Renderer.SeamSubmitsByOwner;
		const UVoxelChunkManager::FMeshWorkStats WorkBefore = W.Manager->GetMeshWorkStats();

		const int32 Marked = W.Manager->RemeshAllLoadedChunks();
		const bool bSettled = Settle(W);

		TArray<FIntVector> LoadedAfter;
		W.Manager->GetLoadedChunks(LoadedAfter);
		const FHeld After = Capture(W.Renderer);
		const UVoxelChunkManager::FMeshWorkStats& WorkAfter = W.Manager->GetMeshWorkStats();
		const int64 ChunkJobs = WorkAfter.ChunkMeshJobs - WorkBefore.ChunkMeshJobs;
		const int64 SeamJobs = WorkAfter.SeamJobs - WorkBefore.SeamJobs;
		const int32 ChunksNotResubmitted = CountNotResubmitted(Before.Chunks, ChunkCountsBefore, W.Renderer.ChunkSubmitsByChunk);
		const int32 SeamsNotResubmitted = CountNotResubmitted(Before.Seams, SeamCountsBefore, W.Renderer.SeamSubmitsByOwner);
		int32 LODsChanged = 0;
		for (const TPair<FIntVector, int32>& Pair : LODsBefore)
		{
			const int32* Now = W.Renderer.ChunkLODs.Find(Pair.Key);
			LODsChanged += (!Now || *Now != Pair.Value) ? 1 : 0;
		}

		Test.AddInfo(FString::Printf(TEXT("%s: %d loaded chunks marked, %lld chunk mesh jobs, %lld seam jobs; %d chunk meshes and %d seam buckets held, not resubmitted %d and %d"),
			*Label, Marked, ChunkJobs, SeamJobs, Before.Chunks.Num(), Before.Seams.Num(), ChunksNotResubmitted, SeamsNotResubmitted));
		Test.TestTrue(FString::Printf(TEXT("%s: settles"), *Label), bSettled);
		Test.TestEqual(FString::Printf(TEXT("%s: every loaded chunk marked"), *Label), Marked, LoadedBefore.Num());
		Test.TestEqual(FString::Printf(TEXT("%s: a mesh job per loaded chunk"), *Label), ChunkJobs, static_cast<int64>(LoadedBefore.Num()));
		Test.TestEqual(FString::Printf(TEXT("%s: every chunk mesh submitted again"), *Label), ChunksNotResubmitted, 0);
		Test.TestEqual(FString::Printf(TEXT("%s: every seam bucket submitted again"), *Label), SeamsNotResubmitted, 0);
		Test.TestTrue(FString::Printf(TEXT("%s: seam jobs ran"), *Label), SeamJobs >= Before.Seams.Num());
		Test.TestEqual(FString::Printf(TEXT("%s: no chunk loaded or unloaded"), *Label), LoadedAfter.Num(), LoadedBefore.Num());
		Test.TestEqual(FString::Printf(TEXT("%s: no chunk changed LOD"), *Label), LODsChanged, 0);
		Test.TestEqual(FString::Printf(TEXT("%s: chunk meshes as before"), *Label), CountDiffering(Before.Chunks, After.Chunks), 0);
		Test.TestEqual(FString::Printf(TEXT("%s: seam buckets as before"), *Label), CountDiffering(Before.Seams, After.Seams), 0);

		if (bPaused)
		{
			W.Manager->SetStreamingEnabled(true);
			Test.TestTrue(FString::Printf(TEXT("%s: settles after the resume"), *Label), Settle(W));
		}
	}

	void RunWorld(FAutomationTestBase& Test, const TCHAR* Name, bool bPlanet, EMeshingMode Mode)
	{
		VoxelTesting::FTestChunkWorld W;
		W.Create(Name);
		if (bPlanet)
		{
			W.ConfigureSmallPlanet();
		}
		else
		{
			W.ConfigureSmallPlane();
		}
		W.Config->MeshingMode = Mode;
		W.ConfigureThreeLODBands(5000.0f);
		W.StartManager();
		W.SetViewer(bPlanet ? W.ViewerOverSurface(FVector(0.3, 0.2, 0.93).GetSafeNormal(), 600.0f) : W.ViewerOverGround(1000.0, 700.0, 600.0f));
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the world settles"), Name), Settle(W)))
		{
			return;
		}
		CheckRemeshAll(Test, W, Name, /*bPaused*/ false);
		CheckRemeshAll(Test, W, Name, /*bPaused*/ true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRemeshAllPlanetTest, "VoxelWorlds.Streaming.RemeshAll.Planet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelRemeshAllPlanetTest::RunTest(const FString& Parameters)
{
	VoxelRemeshAllTest::RunWorld(*this, TEXT("RemeshAllPlanetDC"), /*bPlanet*/ true, EMeshingMode::DualContouring);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelRemeshAllPlaneTest, "VoxelWorlds.Streaming.RemeshAll.Plane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelRemeshAllPlaneTest::RunTest(const FString& Parameters)
{
	VoxelRemeshAllTest::RunWorld(*this, TEXT("RemeshAllPlaneMC"), /*bPlanet*/ false, EMeshingMode::MarchingCubes);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
