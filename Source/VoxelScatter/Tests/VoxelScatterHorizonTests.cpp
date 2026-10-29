// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
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
// World modes review G79 (WORLD_MODES_REVIEW_PLAN.md §23.15): a chunk's first extraction once within the scatter horizon.
//
// The scatter manager was handed a chunk only when the chunk submitted a mesh, and the hand-off generated only the types
// whose spawn distance covered the chunk; with none in range it extracted nothing and cached nothing, and the distance
// pass, which streams the distance-limited types, reads only chunks with a surface cache. While walking, every chunk
// arrives at the window's front beyond every spawn distance, so its scatter waited for its next mesh (the LOD refine near
// the viewer), where every type landed at once, the trees inside their fade band: the pop-in in front of the player.
//
// Through the manager's own pipeline on a tilted heightmap plane with the default definitions (grass and rocks at
// ScatterRadius 10,000, trees with a SpawnDistance of 20,000), a hand-off requester standing in for the chunk manager's:
// - Horizon: a chunk submitted with the viewer 40,000 away extracts nothing and waits; with the viewer at 15,000 (within
//   the trees' distance) the distance pass hands it off again and it holds the trees and no grass; with the viewer near
//   it holds the same spawn points as a chunk submitted with the viewer near from the start; with the viewer far again
//   every type is released (the radius types too), and an unloaded chunk is never asked for.
// - RuleBefore (voxel.Scatter.FirstExtractionAtHorizon 0): the same chunk still has nothing with the viewer at 15,000 and
//   with the viewer near, and the radius types stay once spawned: the behaviour before, kept for A/B.
// ---------------------------------------------------------------------------

namespace VoxelScatterHorizonTestUtils
{
	constexpr int32 ChunkSize = 16;
	constexpr float VoxelSize = 100.0f;
	constexpr float ChunkWorldSize = ChunkSize * VoxelSize;
	constexpr float ScatterRadius = 10000.0f;
	constexpr int32 GrassID = 0;
	constexpr int32 RockID = 1;
	constexpr int32 TreeID = 2; // CreateDefaultDefinitions' smooth trees (SpawnDistance 20,000)

	struct FScopedCVar
	{
		IConsoleVariable* Var = nullptr;
		FString Previous;
		EConsoleVariableFlags Priority = ECVF_SetByCode;
		FScopedCVar(const TCHAR* Name, const TCHAR* Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Previous = Var->GetString();
				Priority = static_cast<EConsoleVariableFlags>(FMath::Max(static_cast<uint32>(Var->GetFlags() & ECVF_SetByMask), static_cast<uint32>(ECVF_SetByCode)));
				Var->Set(Value, Priority);
			}
		}
		~FScopedCVar()
		{
			if (Var)
			{
				Var->Set(*Previous, Priority);
			}
		}
	};

	/** A heightmap world mode whose natural terrain is the plane H = Offset + SlopeX * X + SlopeY * Y. */
	struct FTiltedPlaneWorldMode : public IVoxelWorldMode
	{
		float Offset = 0.0f;
		float SlopeX = 0.0f;
		float SlopeY = 0.0f;
		FTiltedPlaneWorldMode(float InOffset, float InSlopeX, float InSlopeY) : Offset(InOffset), SlopeX(InSlopeX), SlopeY(InSlopeY) {}
		virtual float GetTerrainHeightAt(float X, float Y, const FVoxelNoiseParams&) const override { return Offset + SlopeX * X + SlopeY * Y; }
		virtual bool IsHeightmapBased() const override { return true; }
		virtual float GetDensityAt(const FVector&, int32, float) const override { return 0.0f; }
		virtual FIntVector WorldToChunkCoord(const FVector&, int32, float) const override { return FIntVector::ZeroValue; }
		virtual FVector ChunkCoordToWorld(const FIntVector&, int32, float, int32) const override { return FVector::ZeroVector; }
		virtual int32 GetMinZ() const override { return -100000; }
		virtual int32 GetMaxZ() const override { return 100000; }
		virtual EWorldMode GetWorldModeType() const override { return EWorldMode::InfinitePlane; }
		virtual uint8 GetMaterialAtDepth(const FVector&, float, float) const override { return 0; }
	};

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

	FString SpawnKey(const FScatterSpawnPoint& Point)
	{
		return FString::Printf(TEXT("%d %d %d %d %d %d"), Point.ScatterTypeID,
			FMath::RoundToInt32(Point.Position.X * 10.0), FMath::RoundToInt32(Point.Position.Y * 10.0), FMath::RoundToInt32(Point.Position.Z * 10.0),
			FMath::RoundToInt32(Point.RotationYaw * 100.0f), FMath::RoundToInt32(Point.Scale * 1000.0f));
	}

	/** What the manager holds for one chunk. */
	struct FChunkState
	{
		bool bSurface = false;
		TArray<FString> Spawns; // sorted keys
		int32 Grass = 0;
		int32 Rocks = 0;
		int32 Trees = 0;
		int32 Instances = 0;
		bool bAwaiting = false;
	};

	/** Private game world + initialized scatter manager; the "chunk manager" is a set of loaded chunks the requester resubmits from. */
	struct FHarness
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		UVoxelScatterManager* Manager = nullptr;
		FVector Viewer = FVector::ZeroVector;
		TSet<FIntVector> Loaded;
		int32 Requests = 0;
		int32 RequestsForUnloaded = 0;

		explicit FHarness(const TCHAR* WorldName)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, WorldName);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			Config = NewObject<UVoxelWorldConfiguration>();
			Config->ChunkSize = ChunkSize;
			Config->VoxelSize = VoxelSize;
			Config->ScatterRadius = ScatterRadius;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Manager = NewObject<UVoxelScatterManager>();
			Manager->AddToRoot();
			Manager->Initialize(Config, World);
			Manager->SetSurfacePointSpacing(VoxelSize);
			Manager->SetWorldMode(&Plane());
			for (const int32 ID : { GrassID, RockID, TreeID })
			{
				if (const FScatterDefinition* Def = Manager->GetScatterDefinition(ID))
				{
					FScatterDefinition Copy = *Def;
					Copy.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
					Copy.Density = ID == TreeID ? 0.3f : Copy.Density;
					Manager->AddScatterDefinition(Copy);
				}
			}
			// The chunk manager's requester: a loaded chunk is handed off again from its voxels
			Manager->SetChunkHandOffRequester([this](const FIntVector& Coord) -> bool
			{
				++Requests;
				if (!Loaded.Contains(Coord))
				{
					++RequestsForUnloaded;
					return false;
				}
				Manager->OnChunkMeshDataReady(Coord, MakeChunk(Coord), ChunkSize, VoxelSize);
				return true;
			});
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

		/** Load a chunk: its mesh submit hands it off with the viewer where it is. */
		void Load(const FIntVector& Coord)
		{
			Loaded.Add(Coord);
			Manager->OnChunkMeshDataReady(Coord, MakeChunk(Coord), ChunkSize, VoxelSize);
		}

		void Unload(const FIntVector& Coord)
		{
			Loaded.Remove(Coord);
			Manager->OnChunkUnloaded(Coord);
		}

		/** Move the viewer, and pump until the extractions are done and the distance passes and the renderer's adds have had a second. */
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
			for (int32 i = 0; i < 12; ++i)
			{
				Update();
				FPlatformProcess::Sleep(0.005f);
			}
		}

		FChunkState State(const FIntVector& Coord) const
		{
			FChunkState Out;
			Out.bSurface = Manager->GetChunkSurfaceData(Coord) != nullptr;
			if (const FChunkScatterData* Scatter = Manager->GetChunkScatterData(Coord))
			{
				for (const FScatterSpawnPoint& Point : Scatter->SpawnPoints)
				{
					Out.Spawns.Add(SpawnKey(Point));
					Out.Grass += Point.ScatterTypeID == GrassID ? 1 : 0;
					Out.Rocks += Point.ScatterTypeID == RockID ? 1 : 0;
					Out.Trees += Point.ScatterTypeID == TreeID ? 1 : 0;
				}
				Out.Spawns.Sort();
			}
			const UVoxelScatterRenderer* Renderer = Manager->GetScatterRenderer();
			Out.Instances = Renderer ? Renderer->GetChunkInstanceCount(Coord) : 0;
			Out.bAwaiting = Manager->GetChunksAwaitingFirstExtraction().Contains(Coord);
			return Out;
		}

		FString Describe(const TCHAR* Label, const FChunkState& S) const
		{
			return FString::Printf(TEXT("%s: surface %s, %d spawn points (%d grass, %d rocks, %d trees), %d instances, %s; requests %d (%d for unloaded chunks)"),
				Label, S.bSurface ? TEXT("cached") : TEXT("NOT cached"), S.Spawns.Num(), S.Grass, S.Rocks, S.Trees, S.Instances,
				S.bAwaiting ? TEXT("awaiting") : TEXT("not awaiting"), Requests, RequestsForUnloaded);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterFirstExtractionHorizonTest,
	"VoxelWorlds.Scatter.FirstExtraction.Horizon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterFirstExtractionHorizonTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHorizonTestUtils;
	const FScopedCVar Rule(TEXT("voxel.Scatter.FirstExtractionAtHorizon"), TEXT("1"));

	const FIntVector Target(0, 0, 0);
	const FVector Near = ChunkCenter(Target);
	const FVector Far = Near + FVector(40000.0, 0.0, 0.0);      // beyond every type: the window's front
	const FVector TreeRange = Near + FVector(15000.0, 0.0, 0.0); // within the trees' 20,000, beyond the radius types' 10,000

	// The reference: the chunk loaded with the viewer near from the start
	FChunkState Reference;
	{
		FHarness Ref(TEXT("VoxelScatterHorizonReferenceWorld"));
		Ref.Viewer = Near;
		Ref.Update();
		Ref.Load(Target);
		Ref.Settle(Near);
		Reference = Ref.State(Target);
		AddInfo(Ref.Describe(TEXT("reference, loaded near"), Reference));
		TestTrue(TEXT("Scenario: loaded near the chunk has grass, rocks and trees"), Reference.Grass > 0 && Reference.Rocks > 0 && Reference.Trees > 0);
	}

	FHarness H(TEXT("VoxelScatterHorizonWorld"));
	TestEqual(TEXT("The scatter horizon is the trees' spawn distance"), H.Manager->GetScatterHorizon(), 20000.0f);

	// Loaded at the front: nothing extracted, the chunk waits
	H.Viewer = Far;
	H.Update();
	H.Load(Target);
	H.Settle(Far);
	const FChunkState AtFront = H.State(Target);
	AddInfo(H.Describe(TEXT("loaded at the front"), AtFront));
	TestFalse(TEXT("Loaded at the front: no surface cached"), AtFront.bSurface);
	TestEqual(TEXT("Loaded at the front: no spawn point"), AtFront.Spawns.Num(), 0);
	TestTrue(TEXT("Loaded at the front: the chunk awaits its first extraction"), AtFront.bAwaiting);
	TestEqual(TEXT("Loaded at the front: no hand-off asked for"), H.Requests, 0);

	// Within the trees' distance: the distance pass hands it off, the trees come, the radius types not yet
	H.Settle(TreeRange);
	const FChunkState InTreeRange = H.State(Target);
	AddInfo(H.Describe(TEXT("viewer within the trees' distance"), InTreeRange));
	TestEqual(TEXT("Within the trees' distance: one hand-off asked for"), H.Requests, 1);
	TestTrue(TEXT("Within the trees' distance: the surface is cached"), InTreeRange.bSurface);
	TestFalse(TEXT("Within the trees' distance: the chunk no longer waits"), InTreeRange.bAwaiting);
	TestEqual(TEXT("Within the trees' distance: as many trees as the reference"), InTreeRange.Trees, Reference.Trees);
	TestEqual(TEXT("Within the trees' distance: no grass yet"), InTreeRange.Grass, 0);
	TestEqual(TEXT("Within the trees' distance: no rock yet"), InTreeRange.Rocks, 0);
	TestEqual(TEXT("Within the trees' distance: each spawn point renders once"), InTreeRange.Instances, InTreeRange.Spawns.Num());

	// Near: the radius types stream in and the chunk equals the reference
	H.Settle(Near);
	const FChunkState NearState = H.State(Target);
	AddInfo(H.Describe(TEXT("viewer near"), NearState));
	TestTrue(TEXT("Near: the same spawn points as the chunk loaded near from the start"), NearState.Spawns == Reference.Spawns);
	TestEqual(TEXT("Near: each spawn point renders once"), NearState.Instances, NearState.Spawns.Num());
	TestEqual(TEXT("Near: no further hand-off asked for"), H.Requests, 1);

	// Far again: every type released (the radius types at ScatterRadius x 1.1 like the others), the surface kept
	H.Settle(Far);
	const FChunkState FarAgain = H.State(Target);
	AddInfo(H.Describe(TEXT("viewer far again"), FarAgain));
	TestTrue(TEXT("Far again: the surface stays cached"), FarAgain.bSurface);
	TestEqual(TEXT("Far again: every type released"), FarAgain.Spawns.Num(), 0);
	TestEqual(TEXT("Far again: no instance"), FarAgain.Instances, 0);
	TestFalse(TEXT("Far again: a chunk with a surface cache does not wait"), FarAgain.bAwaiting);

	// Back near: the distance pass restores the reference from the cache, no hand-off needed
	H.Settle(Near);
	const FChunkState NearAgain = H.State(Target);
	AddInfo(H.Describe(TEXT("viewer near again"), NearAgain));
	TestTrue(TEXT("Near again: the same spawn points as the reference"), NearAgain.Spawns == Reference.Spawns);
	TestEqual(TEXT("Near again: no further hand-off asked for"), H.Requests, 1);

	// A chunk unloaded while waiting is never asked for
	const FIntVector Other(0, 2, 0);
	H.Settle(ChunkCenter(Other) + FVector(40000.0, 0.0, 0.0));
	H.Load(Other);
	H.Update();
	TestTrue(TEXT("A second chunk loaded at the front waits"), H.State(Other).bAwaiting);
	H.Unload(Other);
	TestFalse(TEXT("Unloaded, it waits no more"), H.State(Other).bAwaiting);
	H.Settle(ChunkCenter(Other));
	TestEqual(TEXT("Unloaded, no hand-off is asked for it"), H.RequestsForUnloaded, 0);
	TestEqual(TEXT("Unloaded, nothing of it is cached"), H.Manager->GetChunkSurfaceData(Other) != nullptr, false);
	AddInfo(FString::Printf(TEXT("end: requests %d (%d for unloaded chunks), deferred first extractions %lld"), H.Requests, H.RequestsForUnloaded,
		H.Manager->GetTotalDeferredFirstExtractions()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScatterFirstExtractionRuleBeforeTest,
	"VoxelWorlds.Scatter.FirstExtraction.RuleBefore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelScatterFirstExtractionRuleBeforeTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScatterHorizonTestUtils;
	const FScopedCVar Rule(TEXT("voxel.Scatter.FirstExtractionAtHorizon"), TEXT("0"));

	const FIntVector Target(0, 0, 0);
	const FVector Near = ChunkCenter(Target);
	const FVector Far = Near + FVector(40000.0, 0.0, 0.0);
	const FVector TreeRange = Near + FVector(15000.0, 0.0, 0.0);

	FHarness H(TEXT("VoxelScatterHorizonRuleBeforeWorld"));
	H.Viewer = Far;
	H.Update();
	H.Load(Target);
	H.Settle(Far);
	H.Settle(TreeRange);
	const FChunkState InTreeRange = H.State(Target);
	AddInfo(H.Describe(TEXT("rule before, viewer within the trees' distance"), InTreeRange));
	TestFalse(TEXT("Rule before: within the trees' distance nothing is cached"), InTreeRange.bSurface);
	TestEqual(TEXT("Rule before: no hand-off asked for"), H.Requests, 0);
	H.Settle(Near);
	const FChunkState NearState = H.State(Target);
	AddInfo(H.Describe(TEXT("rule before, viewer near"), NearState));
	TestFalse(TEXT("Rule before: near, still nothing cached (the chunk waits for its next mesh)"), NearState.bSurface);
	TestEqual(TEXT("Rule before: near, no spawn point"), NearState.Spawns.Num(), 0);

	// Its next mesh: the chunk hands off near and the radius types stay once the viewer leaves (never released, before)
	H.Load(Target);
	H.Settle(Near);
	const FChunkState Meshed = H.State(Target);
	TestTrue(TEXT("Rule before: the next mesh near brings every type"), Meshed.Grass > 0 && Meshed.Rocks > 0 && Meshed.Trees > 0);
	H.Settle(Far);
	const FChunkState FarAgain = H.State(Target);
	AddInfo(H.Describe(TEXT("rule before, viewer far again"), FarAgain));
	TestEqual(TEXT("Rule before: far again, the trees are released"), FarAgain.Trees, 0);
	TestTrue(TEXT("Rule before: far again, the radius types stay"), FarAgain.Grass == Meshed.Grass && FarAgain.Rocks == Meshed.Rocks);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
