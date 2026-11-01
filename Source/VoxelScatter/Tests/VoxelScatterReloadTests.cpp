// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Testing/VoxelScatterTestDefinitions.h"
#include "Engine/StaticMesh.h"
#include "VoxelScatterManager.h"
#include "VoxelScatterRenderer.h"
#include "VoxelScatterTypes.h"
#include "VoxelWorldConfiguration.h"
#include "IVoxelWorldMode.h"
#include "InfinitePlaneWorldMode.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G30 (WORLD_MODES_REVIEW_PLAN.md §14.1, §19.7): a chunk that streams out and back in. On unload
// UVoxelScatterManager::RemoveChunkScatter removed the surface cache and, through a short-circuit ||, not the scatter cache
// when the surface cache held the chunk. The unloaded chunk's spawn points stayed cached, and when it streamed back in its
// extraction was taken for a supplemental pass: its spawn points were appended to the stale ones and its surface data was
// not cached again, so the distance spawn and the audits skipped it, and anything that re-renders a chunk from the cache
// (a cleared radius, an exclusion volume, a harvest) put two instances on every point.
//
// Through the scatter manager's own pipeline (OnChunkMeshDataReady -> async extraction -> Update), on a tilted heightmap
// plane of grass and dirt and the test definitions (grass, rocks, trees with a SpawnDistance) given an engine mesh so
// the renderer holds real instances:
// - SameAsFresh: four chunks; one unloads and reloads three times. After each unload neither cache nor the renderer holds
//   it; after each reload its surface data is cached and equals the first extraction's, its spawn points equal the first
//   ones (no duplicates), it renders each once, and the digest equals the one before. A cleared radius on the reloaded
//   chunk leaves the same spawn points and instances as on a manager that never unloaded it.
// - DistanceSpawn: the chunk loads with the viewer past the trees' SpawnDistance, the viewer comes near (the distance
//   spawn adds trees from the cached surface), goes away (the trees are released), the chunk unloads and reloads far
//   away and the viewer comes near again: the same spawn points as the first time at each stop, the trees included.
// - ReleaseBeforeFlush: the distance cleanup releases the trees in the Update that queued their adds, before the
//   renderer flushed them. UVoxelScatterRenderer::ReleaseChunkScatterType left those adds queued; flushed, they belonged
//   to a type the chunk no longer tracked, survived its unload (RemoveChunkInstances releases tracked types only) and
//   stood under the trees of its reload. No tree instance after the release, none after the unload, one per point after
//   the reload.
// ---------------------------------------------------------------------------

namespace VoxelScatterReloadTestUtils
{
	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr int32 TreeID = 2; // VoxelScatterTesting::SmoothDefinitions' trees (SpawnDistance 20,000)

	/** A heightmap world mode whose natural terrain is the plane H = Offset + SlopeX * X + SlopeY * Y. */
	struct FTiltedPlaneWorldMode : public IVoxelWorldMode
	{
		float Offset = 0.0f;
		float SlopeX = 0.0f;
		float SlopeY = 0.0f;
		FTiltedPlaneWorldMode(float InOffset, float InSlopeX, float InSlopeY) : Offset(InOffset), SlopeX(InSlopeX), SlopeY(InSlopeY) {}
		float GetTerrainHeightAt(float X, float Y, const FVoxelNoiseParams&) const { return Offset + SlopeX * X + SlopeY * Y; }
		virtual FVoxelTerrainSurfacePoint GetSurfaceOnUpLine(const FVector& P, const FVoxelNoiseParams& N) const override
		{
			FVoxelTerrainSurfacePoint S;
			S.Position = FVector(P.X, P.Y, GetTerrainHeightAt(static_cast<float>(P.X), static_cast<float>(P.Y), N));
			S.Altitude = static_cast<float>(S.Position.Z);
			return S;
		}
		virtual bool IsHeightmapBased() const override { return true; }
		virtual float GetDensityAt(const FVector&, int32, float) const override { return 0.0f; }
		virtual FIntVector WorldToChunkCoord(const FVector&, int32, float) const override { return FIntVector::ZeroValue; }
		virtual FVector ChunkCoordToWorld(const FIntVector&, int32, float, int32) const override { return FVector::ZeroVector; }
		virtual int32 GetMinZ() const override { return -100000; }
		virtual int32 GetMaxZ() const override { return 100000; }
		virtual EWorldMode GetWorldModeType() const override { return EWorldMode::InfinitePlane; }
		virtual uint8 GetMaterialAtDepth(const FVector&, float, float) const override { return 0; }
	};

	/** H = 300.37 + 0.13 X + 0.11 Y: 300 to 1,068 over two chunks each way, inside chunk layer 0, about 10 degrees of slope. */
	const FTiltedPlaneWorldMode& Plane()
	{
		static const FTiltedPlaneWorldMode Mode(300.37f, 0.13f, 0.11f);
		return Mode;
	}

	FVector ChunkCenter(const FIntVector& Coord) { return (FVector(Coord) + FVector(0.5)) * ChunkWorldSize; }

	/** A chunk of the plane with the heightmap generator's SDF: grass, with every third column dirt (rocks grow there). */
	TArray<FVoxelData> MakeChunk(const FIntVector& Coord)
	{
		TArray<FVoxelData> Voxels;
		Voxels.SetNumUninitialized(ChunkSize * ChunkSize * ChunkSize);
		const FVector Origin = FVector(Coord) * ChunkWorldSize;
		for (int32 Y = 0; Y < ChunkSize; ++Y)
		{
			for (int32 X = 0; X < ChunkSize; ++X)
			{
				const float Height = Plane().GetTerrainHeightAt(Origin.X + X * VoxelSize, Origin.Y + Y * VoxelSize, FVoxelNoiseParams());
				const uint8 Material = ((Coord.X * ChunkSize + X) + (Coord.Y * ChunkSize + Y)) % 3 == 0 ? EVoxelMaterial::Dirt : EVoxelMaterial::Grass;
				for (int32 Z = 0; Z < ChunkSize; ++Z)
				{
					const float SignedDistance = FInfinitePlaneWorldMode::CalculateSignedDistance(static_cast<float>(Origin.Z) + Z * VoxelSize, Height);
					Voxels[X + Y * ChunkSize + Z * ChunkSize * ChunkSize] =
						FVoxelData(Material, FInfinitePlaneWorldMode::SignedDistanceToDensity(SignedDistance, VoxelSize));
				}
			}
		}
		return Voxels;
	}

	/** One spawn point as the digest keys it (type, position to 0.1 uu, yaw, scale, local up). */
	FString SpawnKey(const FScatterSpawnPoint& Point)
	{
		return FString::Printf(TEXT("%d %d %d %d %d %d %d %d %d"), Point.ScatterTypeID,
			FMath::RoundToInt32(Point.Position.X * 10.0), FMath::RoundToInt32(Point.Position.Y * 10.0), FMath::RoundToInt32(Point.Position.Z * 10.0),
			FMath::RoundToInt32(Point.RotationYaw * 100.0f), FMath::RoundToInt32(Point.Scale * 1000.0f),
			FMath::RoundToInt32(Point.LocalUp.X * 10000.0), FMath::RoundToInt32(Point.LocalUp.Y * 10000.0), FMath::RoundToInt32(Point.LocalUp.Z * 10000.0));
	}

	/** What the manager holds for one chunk. */
	struct FChunkState
	{
		bool bSurface = false;
		bool bScatter = false;
		TArray<FVoxelSurfacePoint> Surface;
		/** Spawn points' keys, sorted (passes append in their own order). */
		TArray<FString> Spawns;
		int32 Unique = 0;
		int32 Trees = 0;
		int32 Instances = 0;
	};

	/** Two surface point lists equal point for point (position, normal, material, classification). */
	bool SamePoints(const TArray<FVoxelSurfacePoint>& A, const TArray<FVoxelSurfacePoint>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (!A[i].Position.Equals(B[i].Position, 0.0) || !A[i].Normal.Equals(B[i].Normal, 0.0) || A[i].MaterialID != B[i].MaterialID
				|| A[i].bIsUnderground != B[i].bIsUnderground || A[i].bIsUnderwater != B[i].bIsUnderwater)
			{
				return false;
			}
		}
		return true;
	}

	/** Private game world + initialized scatter manager for a smooth world, torn down on destruction. */
	struct FHarness
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelScatterManager* Manager = nullptr;
		FVector Viewer = FVector::ZeroVector;

		explicit FHarness(const TCHAR* WorldName)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			Config = NewObject<UVoxelWorldConfiguration>();
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = 100000.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Manager = NewObject<UVoxelScatterManager>();
			Manager->AddToRoot();
			Manager->Initialize(Config, World);
			VoxelScatterTesting::AddSmoothDefinitions(*Manager);
			Manager->SetSurfacePointSpacing(VoxelSize);
			Manager->SetWorldMode(&Plane());

			// The test definitions have no mesh, so the renderer would make no instance.Give each the engine cube; trees
			// dense enough that every chunk has some.
			for (const int32 ID : { 0, 1, TreeID })
			{
				if (const FScatterDefinition* Def = Manager->GetScatterDefinition(ID))
				{
					FScatterDefinition Copy = *Def;
					Copy.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
					Copy.Density = ID == TreeID ? 0.3f : Copy.Density;
					Manager->AddScatterDefinition(Copy);
				}
			}
		}

		~FHarness()
		{
			if (Manager)
			{
				Manager->SetWorldMode(nullptr);
				Manager->Shutdown();
				Manager->RemoveFromRoot();
			}
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		void Update() { Manager->Update(Viewer, 0.05f); }

		/** Submit a chunk as if its mesh just arrived (the viewer's distance decides which definitions run). */
		void Submit(const FIntVector& Coord) { Manager->OnChunkMeshDataReady(Coord, MakeChunk(Coord), ChunkSize, VoxelSize); }

		/** Move the viewer, and pump until the extractions are done and the distance spawn and the renderer's adds have had a second. */
		void Settle(const FVector& InViewer)
		{
			Viewer = InViewer;
			const double Start = FPlatformTime::Seconds();
			while (FPlatformTime::Seconds() < Start + 15.0)
			{
				Update();
				if (Manager->GetPendingGenerationCount() == 0 && FPlatformTime::Seconds() >= Start + 1.0)
				{
					break;
				}
				FPlatformProcess::Sleep(0.005f);
			}
			// The distance spawn runs every 0.25 s of Update time: a dozen more ticks drain its results and the adds.
			for (int32 i = 0; i < 12; ++i)
			{
				Update();
				FPlatformProcess::Sleep(0.005f);
			}
		}

		FChunkState State(const FIntVector& Coord) const
		{
			FChunkState Out;
			if (const FChunkSurfaceData* Surface = Manager->GetChunkSurfaceData(Coord))
			{
				Out.bSurface = true;
				Out.Surface = Surface->SurfacePoints;
			}
			if (const FChunkScatterData* Scatter = Manager->GetChunkScatterData(Coord))
			{
				Out.bScatter = true;
				TSet<FString> Seen;
				for (const FScatterSpawnPoint& Point : Scatter->SpawnPoints)
				{
					Out.Spawns.Add(SpawnKey(Point));
					Seen.Add(Out.Spawns.Last());
					Out.Trees += Point.ScatterTypeID == TreeID ? 1 : 0;
				}
				Out.Spawns.Sort();
				Out.Unique = Seen.Num();
			}
			const UVoxelScatterRenderer* Renderer = Manager->GetScatterRenderer();
			Out.Instances = Renderer ? Renderer->GetChunkInstanceCount(Coord) : 0;
			return Out;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterReloadSameAsFreshTest,
	"VoxelWorlds.Scatter.Reload.SameAsFresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterReloadSameAsFreshTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterReloadTestUtils;

	const TArray<FIntVector> Coords = { FIntVector(0, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 1, 0), FIntVector(1, 1, 0) };
	const FIntVector Target(1, 0, 0);
	const FVector Near = ChunkCenter(FIntVector(1, 1, 0));

	FHarness Harness(TEXT("VoxelScatterReloadTestWorld"));
	for (const FIntVector& Coord : Coords)
	{
		Harness.Submit(Coord);
	}
	Harness.Settle(Near);
	const FChunkState First = Harness.State(Target);
	TArray<FString> FirstLines;
	const FString FirstDigest = Harness.Manager->ComputeScatterDigest(&FirstLines);
	AddInfo(FString::Printf(TEXT("first load: %s; target %d surface points, %d spawn points (%d trees), %d instances"), *FirstDigest,
		First.Surface.Num(), First.Spawns.Num(), First.Trees, First.Instances));
	TestTrue(TEXT("Scenario: the target chunk has surface data and spawn points of every type, the trees included"),
		First.bSurface && First.Surface.Num() > 0 && First.Spawns.Num() > 0 && First.Trees > 0);
	TestEqual(TEXT("Scenario: the target chunk renders each spawn point once"), First.Instances, First.Spawns.Num());
	TestEqual(TEXT("Scenario: no duplicate spawn point"), First.Unique, First.Spawns.Num());

	for (int32 Cycle = 1; Cycle <= 3; ++Cycle)
	{
		Harness.Manager->OnChunkUnloaded(Target);
		const FChunkState Gone = Harness.State(Target);
		TestFalse(FString::Printf(TEXT("Cycle %d: after the unload no surface data is cached"), Cycle), Gone.bSurface);
		TestFalse(FString::Printf(TEXT("Cycle %d: after the unload no spawn point is cached"), Cycle), Gone.bScatter);
		TestEqual(FString::Printf(TEXT("Cycle %d: after the unload the renderer holds no instance"), Cycle), Gone.Instances, 0);
		TestTrue(FString::Printf(TEXT("Cycle %d: after the unload no chunk has spawn points without surface data (%s)"), Cycle,
			*Harness.Manager->ComputeCacheResidency()), Harness.Manager->ComputeCacheResidency().Contains(TEXT("scatterOnly=0 ")));

		Harness.Submit(Target);
		Harness.Settle(Near);
		const FChunkState Again = Harness.State(Target);
		TArray<FString> AgainLines;
		const FString AgainDigest = Harness.Manager->ComputeScatterDigest(&AgainLines);
		AddInfo(FString::Printf(TEXT("cycle %d reload: %s; target surface %s (%d points), %d spawn points (%d unique, %d trees), %d instances; %s"), Cycle,
			*AgainDigest, Again.bSurface ? TEXT("cached") : TEXT("NOT cached"), Again.Surface.Num(), Again.Spawns.Num(), Again.Unique, Again.Trees,
			Again.Instances, *Harness.Manager->ComputeCacheResidency()));
		TestTrue(FString::Printf(TEXT("Cycle %d: the reloaded chunk's surface data is cached"), Cycle), Again.bSurface);
		TestTrue(FString::Printf(TEXT("Cycle %d: and equals the first extraction's"), Cycle), SamePoints(Again.Surface, First.Surface));
		TestEqual(FString::Printf(TEXT("Cycle %d: as many spawn points as the first load"), Cycle), Again.Spawns.Num(), First.Spawns.Num());
		TestTrue(FString::Printf(TEXT("Cycle %d: the same spawn points as the first load"), Cycle), Again.Spawns == First.Spawns);
		TestEqual(FString::Printf(TEXT("Cycle %d: no duplicate spawn point"), Cycle), Again.Unique, Again.Spawns.Num());
		TestEqual(FString::Printf(TEXT("Cycle %d: the reloaded chunk renders each spawn point once"), Cycle), Again.Instances, Again.Spawns.Num());
		TestEqual(FString::Printf(TEXT("Cycle %d: the digest equals the first load's"), Cycle), AgainDigest, FirstDigest);
		TestTrue(FString::Printf(TEXT("Cycle %d: chunk by chunk"), Cycle), AgainLines == FirstLines);
	}

	// Re-render the reloaded chunk from the cache: a cleared radius (a player edit) replaces each type's instances with the
	// surviving spawn points. A manager that never unloaded the chunk gives the reference.
	FHarness Fresh(TEXT("VoxelScatterReloadFreshWorld"));
	for (const FIntVector& Coord : Coords)
	{
		Fresh.Submit(Coord);
	}
	Fresh.Settle(Near);
	const FVector ClearAt(ChunkCenter(Target).X, ChunkCenter(Target).Y, Plane().GetTerrainHeightAt(ChunkCenter(Target).X, ChunkCenter(Target).Y, FVoxelNoiseParams()));
	Harness.Manager->ClearScatterInRadius(ClearAt, 300.0f);
	Fresh.Manager->ClearScatterInRadius(ClearAt, 300.0f);
	Harness.Settle(Near);
	Fresh.Settle(Near);
	const FChunkState Cleared = Harness.State(Target);
	const FChunkState Reference = Fresh.State(Target);
	AddInfo(FString::Printf(TEXT("after a cleared radius: reloaded %d spawn points, %d instances; never unloaded %d spawn points, %d instances"),
		Cleared.Spawns.Num(), Cleared.Instances, Reference.Spawns.Num(), Reference.Instances));
	TestTrue(TEXT("Scenario: the cleared radius removed spawn points"), Reference.Spawns.Num() < First.Spawns.Num());
	TestTrue(TEXT("After a cleared radius the reloaded chunk has the spawn points of a chunk never unloaded"), Cleared.Spawns == Reference.Spawns);
	TestEqual(TEXT("and renders each once"), Cleared.Instances, Cleared.Spawns.Num());
	TestEqual(TEXT("as many instances as the chunk never unloaded"), Cleared.Instances, Reference.Instances);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterReloadDistanceSpawnTest,
	"VoxelWorlds.Scatter.Reload.DistanceSpawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterReloadDistanceSpawnTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterReloadTestUtils;

	const FIntVector Target(0, 0, 0);
	const FVector Near = ChunkCenter(Target);
	// 40,000 uu off: past the trees' SpawnDistance (20,000) and their release distance (22,000), inside the scatter radius.
	const FVector Far = Near + FVector(40000.0, 0.0, 0.0);

	FHarness Harness(TEXT("VoxelScatterReloadTestWorld"));
	auto Stop = [&](const TCHAR* Label, const FVector& Viewer) -> FChunkState
	{
		Harness.Settle(Viewer);
		const FChunkState State = Harness.State(Target);
		AddInfo(FString::Printf(TEXT("%s: surface %s, %d spawn points (%d unique, %d trees), %d instances"), Label,
			State.bSurface ? TEXT("cached") : TEXT("NOT cached"), State.Spawns.Num(), State.Unique, State.Trees, State.Instances));
		TestEqual(FString::Printf(TEXT("%s: no duplicate spawn point"), Label), State.Unique, State.Spawns.Num());
		TestEqual(FString::Printf(TEXT("%s: each spawn point renders once"), Label), State.Instances, State.Spawns.Num());
		return State;
	};

	Harness.Viewer = Far;
	Harness.Update();
	Harness.Submit(Target);
	const FChunkState FarFirst = Stop(TEXT("loaded far away"), Far);
	TestTrue(TEXT("Scenario: loaded far away the chunk has spawn points and no tree"), FarFirst.Spawns.Num() > 0 && FarFirst.Trees == 0);
	const FChunkState NearFirst = Stop(TEXT("viewer near"), Near);
	TestTrue(TEXT("Scenario: with the viewer near the distance spawn adds trees"), NearFirst.Trees > 0);
	const FChunkState Released = Stop(TEXT("viewer far again"), Far);
	TestTrue(TEXT("Scenario: with the viewer far again the trees are released"), Released.Spawns == FarFirst.Spawns);

	Harness.Manager->OnChunkUnloaded(Target);
	TestFalse(TEXT("After the unload no spawn point is cached"), Harness.State(Target).bScatter);
	Harness.Submit(Target);
	const FChunkState FarAgain = Stop(TEXT("reloaded far away"), Far);
	TestTrue(TEXT("Reloaded far away: the surface data is cached"), FarAgain.bSurface);
	TestTrue(TEXT("Reloaded far away: the same spawn points as the first load"), FarAgain.Spawns == FarFirst.Spawns);
	const FChunkState NearAgain = Stop(TEXT("viewer near after the reload"), Near);
	TestEqual(TEXT("Viewer near after the reload: as many trees as the first time"), NearAgain.Trees, NearFirst.Trees);
	TestTrue(TEXT("Viewer near after the reload: the same spawn points as the first time"), NearAgain.Spawns == NearFirst.Spawns);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterReloadReleaseBeforeFlushTest,
	"VoxelWorlds.Scatter.Reload.ReleaseBeforeFlush",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterReloadReleaseBeforeFlushTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterReloadTestUtils;

	const FIntVector Target(0, 0, 0);
	const FVector Near = ChunkCenter(Target);
	const FVector Far = Near + FVector(40000.0, 0.0, 0.0);

	FHarness Harness(TEXT("VoxelScatterReloadTestWorld"));
	Harness.Viewer = Near;
	Harness.Update();
	Harness.Submit(Target); // the viewer near: the first pass takes the trees

	// From now on the viewer is far and every Update runs the distance check (a time step over its interval), so the Update
	// that takes the extraction's result queues the trees' adds and then releases the trees, before the renderer flushes.
	const double Deadline = FPlatformTime::Seconds() + 15.0;
	while (FPlatformTime::Seconds() < Deadline && Harness.Manager->GetPendingGenerationCount() > 0)
	{
		Harness.Manager->Update(Far, 0.3f);
		FPlatformProcess::Sleep(0.005f);
	}
	for (int32 i = 0; i < 6; ++i)
	{
		Harness.Manager->Update(Far, 0.3f);
	}
	const UVoxelScatterRenderer* Renderer = Harness.Manager->GetScatterRenderer();
	const FChunkState Released = Harness.State(Target);
	const int32 TreeInstances = Renderer ? Renderer->GetChunkInstanceCount(Target, TreeID) : -1;
	AddInfo(FString::Printf(TEXT("released before the flush: %d spawn points (%d trees), %d instances (%d trees); %s"), Released.Spawns.Num(),
		Released.Trees, Released.Instances, TreeInstances, *Harness.Manager->ComputeCacheResidency()));
	TestTrue(TEXT("Scenario: the chunk has spawn points"), Released.Spawns.Num() > 0);
	TestEqual(TEXT("The trees were released: no tree spawn point"), Released.Trees, 0);
	TestEqual(TEXT("and no tree instance"), TreeInstances, 0);
	TestEqual(TEXT("Each spawn point renders once"), Released.Instances, Released.Spawns.Num());

	Harness.Manager->OnChunkUnloaded(Target);
	TestEqual(TEXT("After the unload the renderer holds no instance of the chunk"), Renderer ? Renderer->GetChunkInstanceCount(Target) : -1, 0);

	Harness.Submit(Target);
	Harness.Settle(Near);
	const FChunkState Again = Harness.State(Target);
	AddInfo(FString::Printf(TEXT("reloaded near: %d spawn points (%d trees), %d instances"), Again.Spawns.Num(), Again.Trees, Again.Instances));
	TestTrue(TEXT("Reloaded near: the trees are back"), Again.Trees > 0);
	TestEqual(TEXT("Reloaded near: each spawn point renders once"), Again.Instances, Again.Spawns.Num());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
