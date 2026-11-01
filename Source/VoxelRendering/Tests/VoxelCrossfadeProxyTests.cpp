// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestScopedCVar.h"
#include "RHI.h"             // GUsingNullRHI: no scene proxy under -nullrhi, the test skips there
#include "Materials/MaterialInterface.h"
#include "VoxelCustomVFRenderer.h"
#include "VoxelSceneProxy.h"
#include "VoxelWorldComponent.h"
#include "VoxelWorldConfiguration.h"
#include "ChunkRenderData.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G74 (WORLD_MODES_REVIEW_PLAN.md §22.3): a mesh swap reaches the scene proxy and, once its crossfade
// ends, a frame draws the new mesh alone with the terrain material. A real world component and scene proxy, the demo's
// terrain master with its _Fade companion, and the world's clock advanced by hand: a same-LOD swap (an edit's re-mesh),
// an LOD swap, a swap held for its seams (an LOD transition) and released, a held swap nobody releases (the failsafe)
// and a swap with voxel.MeshFade 0. Needs a render scene: runs in the editor, skips under -nullrhi (no component
// creates a scene proxy there).
// ---------------------------------------------------------------------------

namespace VoxelCrossfadeProxyTest
{
	/** Quads in a row at height Z (chunk-local): 2 triangles each, so a swap is told apart by its triangle count. */
	FChunkMeshData MakeQuads(int32 NumQuads, float Z)
	{
		FChunkMeshData Mesh;
		for (int32 Q = 0; Q < NumQuads; ++Q)
		{
			const uint32 Base = Mesh.Positions.Num();
			const float X = 10.0f + Q * 60.0f;
			Mesh.Positions.Append({ FVector3f(X, 10, Z), FVector3f(X + 50, 10, Z), FVector3f(X + 50, 60, Z), FVector3f(X, 60, Z) });
			for (int32 i = 0; i < 4; ++i)
			{
				Mesh.Normals.Add(FVector3f::UpVector);
				Mesh.UVs.Add(FVector2f::ZeroVector);
			}
			Mesh.Indices.Append({ Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
		}
		return Mesh;
	}

	using FScopedCVar = VoxelTesting::FScopedCVar; // the one scoped cvar (code review CR2 row 17)

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelCrossfadeSwapReachesProxyTest,
	"VoxelWorlds.Rendering.Crossfade.SwapReachesProxy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelCrossfadeSwapReachesProxyTest::RunTest(const FString& Parameters)
{
	using namespace VoxelCrossfadeProxyTest;
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: needs a render scene (run without -nullrhi, where components create no scene proxy)")); return true; }

	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/VoxelWorlds/Materials/M_VoxelMaster.M_VoxelMaster"));
	if (!TestNotNull(TEXT("The terrain master loads"), Master))
	{
		return false;
	}
	const FScopedCVar Fade(TEXT("voxel.MeshFade"), TEXT("1"));
	const FScopedCVar Duration(TEXT("voxel.MeshFade.Duration"), TEXT("0.35"));

	UVoxelWorldConfiguration* Config = NewObject<UVoxelWorldConfiguration>();
	Config->AddToRoot();
	Config->WorldMode = EWorldMode::InfinitePlane;
	Config->WorldOrigin = FVector::ZeroVector;
	Config->VoxelSize = 50.0f;
	Config->ChunkSize = 32;
	Config->MeshingMode = EMeshingMode::DualContouring;

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelCrossfadeProxyTestWorld"));
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	{
		FVoxelCustomVFRenderer Renderer;
		Renderer.SetMaterial(Master);
		Renderer.Initialize(World, Config);
		UVoxelWorldComponent* Component = Renderer.GetWorldComponent();
		if (!TestNotNull(TEXT("The renderer has a world component"), Component))
		{
			Renderer.Shutdown();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			Config->RemoveFromRoot();
			return false;
		}
		TestNotNull(TEXT("The fade material is derived from the master (M_VoxelMaster_Fade)"), Component->GetFadeMaterial());

		// A chunk of render region (0, 0, 0): the world component's own (since MP2 a heightmap world keeps 16-chunk regions
		// too, and a chunk of another region is drawn by that region's component).
		const FIntVector Chunk(2, 1, 0);
		auto Submit = [&](int32 LOD, int32 Quads)
		{
			Renderer.UpdateChunkMeshFromCPU(Chunk, LOD, MakeQuads(Quads, 100.0f + LOD));
			Renderer.FlushPendingOperations();
		};
		// The world's clock and the component's tick (it only ticks while a fade runs; called by hand here)
		auto Advance = [&](double Seconds)
		{
			const int32 Steps = FMath::Max(1, FMath::CeilToInt(Seconds * 60.0));
			for (int32 i = 0; i < Steps; ++i)
			{
				World->RealTimeSeconds += Seconds / Steps;
				Component->TickComponent(static_cast<float>(Seconds / Steps), LEVELTICK_All, nullptr);
				Renderer.FlushPendingOperations();
			}
		};
		auto Info = [&]()
		{
			FVoxelChunkDrawInfo Out;
			Component->GetChunkDrawInfo(Chunk, Out);
			return Out;
		};
		int64 Starts = 0, Restarts = 0;

		// A first mesh, near nothing that would fade it in: drawn at once
		Submit(0, 4);
		FVoxelChunkDrawInfo I = Info();
		if (!TestTrue(TEXT("The proxy holds the first mesh"), I.CurrentTriangles == 8))
		{
			Renderer.Shutdown();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			Config->RemoveFromRoot();
			return false;
		}
		TestTrue(TEXT("First mesh: drawn alone with the terrain material"), I.DrawsCurrentOnly() && !I.bHasFadeState);

		// An edit's re-mesh (same LOD): crossfades from the mesh it replaces, then draws alone
		Submit(0, 6);
		I = Info();
		Component->GetFadeCounters(false, Starts, Restarts);
		TestEqual(TEXT("Edit swap: one interior crossfade started"), Starts, int64(1));
		TestTrue(*FString::Printf(TEXT("Edit swap: crossfades from the old mesh (current %u, previous %u tris)"), I.CurrentTriangles, I.PreviousTriangles),
			I.bCrossfading && I.CurrentTriangles == 12 && I.PreviousTriangles == 8 && !I.bHeld);
		Advance(0.2);
		I = Info();
		TestTrue(*FString::Printf(TEXT("Edit swap: halfway (alpha %.2f)"), I.FadeAlpha), I.bCrossfading && I.FadeAlpha > 0.3f && I.FadeAlpha < 0.9f);
		Advance(0.3);
		I = Info();
		TestTrue(TEXT("Edit swap: after the fade the new mesh is drawn alone, nothing retained"),
			I.DrawsCurrentOnly() && I.CurrentTriangles == 12 && I.PreviousTriangles == 0 && !I.bHasFadeState);
		TestEqual(TEXT("Edit swap: no fade left running"), Component->GetActiveFadeCount(), 0);

		// An LOD swap crossfades the same way
		Submit(1, 3);
		I = Info();
		TestTrue(TEXT("LOD swap: crossfades from LOD 0 to LOD 1"), I.bCrossfading && I.CurrentTriangles == 6 && I.PreviousTriangles == 12 && I.LODLevel == 1);
		Advance(0.5);
		I = Info();
		TestTrue(TEXT("LOD swap: LOD 1 drawn alone after the fade"), I.DrawsCurrentOnly() && I.CurrentTriangles == 6 && I.LODLevel == 1);

		// An LOD transition held for its seams (R24): the old mesh stays on screen until the release, then fades
		Component->SetChunkFadeHold(Chunk, true);
		Submit(0, 5);
		Advance(1.0);
		I = Info();
		TestTrue(TEXT("Held swap: the old mesh stays fully shown while held"),
			I.bCrossfading && I.bHeld && I.FadeAlpha == 0.0f && I.PreviousTriangles == 6 && I.CurrentTriangles == 10);
		Component->SetChunkFadeHold(Chunk, false);
		Advance(0.5);
		I = Info();
		TestTrue(TEXT("Held swap: released, the new mesh is drawn alone after the fade"), I.DrawsCurrentOnly() && I.CurrentTriangles == 10 && I.PreviousTriangles == 0);

		// A hold nobody releases ends at the renderer's failsafe (3 s) and the swap still lands
		Component->SetChunkFadeHold(Chunk, true);
		Submit(1, 2);
		Advance(2.0);
		TestTrue(TEXT("Unreleased hold: still held after 2 s"), Info().bHeld);
		Advance(2.0);
		I = Info();
		TestTrue(TEXT("Unreleased hold: the failsafe lands the swap"), I.DrawsCurrentOnly() && I.CurrentTriangles == 4);

		// With the fade off a swap is drawn at once
		{
			const FScopedCVar Off(TEXT("voxel.MeshFade"), TEXT("0"));
			Submit(0, 7);
			I = Info();
			TestTrue(TEXT("voxel.MeshFade 0: the new mesh is drawn at once"), I.DrawsCurrentOnly() && I.CurrentTriangles == 14 && I.PreviousTriangles == 0);
		}

		Renderer.Shutdown();
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	Config->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
