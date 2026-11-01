// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Hash/CityHash.h"
#include "HAL/PlatformProcess.h"
#include "RHI.h"             // GUsingNullRHI: no scene proxy under -nullrhi, the tests skip there
#include "Materials/MaterialInterface.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelMeshRenderer.h"
#include "VoxelSceneProxy.h"
#include "VoxelRegionComponent.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelMaterialRegistry.h"
#include "VoxelSeamRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World modes review G74 (WORLD_MODES_REVIEW_PLAN.md §22.3): an edit's re-mesh reaches the screen with voxel.MeshFade on.
// The chunk manager streams a small plane into the real renderer (world component and scene proxy, the terrain master
// with its _Fade companion); a player's dig at the surface re-meshes the chunk under it, the proxy crossfades from the
// mesh it had to the new one, and once the fade ends a frame draws the new mesh alone with the terrain material: the
// same mesh the dig gives with voxel.MeshFade 0, where it is drawn at once. The meshes are told apart by their content
// (a Dual Contouring dent can keep the triangle count). Dual Contouring and cubic. Needs a render
// scene: runs in the editor, skips under -nullrhi (no component creates a scene proxy there).
// ---------------------------------------------------------------------------

namespace VoxelEditReachesProxyTest
{
	/** The real renderer, keeping a hash of the latest mesh it was given for each chunk (positions and indices). */
	class FHashingMeshRenderer : public FVoxelMeshRenderer
	{
	public:
		TMap<FIntVector, uint64> MeshHashes;

		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override
		{
			Record(ChunkCoord, MeshData);
			FVoxelMeshRenderer::UpdateChunkMeshFromCPU(ChunkCoord, LODLevel, MeshData);
		}

		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, FChunkMeshData&& MeshData) override
		{
			Record(ChunkCoord, MeshData);
			FVoxelMeshRenderer::UpdateChunkMeshFromCPU(ChunkCoord, LODLevel, MoveTemp(MeshData));
		}

	private:
		void Record(const FIntVector& ChunkCoord, const FChunkMeshData& MeshData)
		{
			const uint64 Positions = CityHash64(reinterpret_cast<const char*>(MeshData.Positions.GetData()), MeshData.Positions.Num() * sizeof(FVector3f));
			const uint64 Indices = CityHash64(reinterpret_cast<const char*>(MeshData.Indices.GetData()), MeshData.Indices.Num() * sizeof(uint32));
			MeshHashes.Add(ChunkCoord, CityHash128to64(Uint128_64(Positions, Indices)));
		}
	};

	struct FRun
	{
		bool bStarted = false;
		bool bSettled = false;
		FIntVector Chunk = FIntVector::ZeroValue;
		int32 EditedVoxels = 0;
		FVoxelChunkDrawInfo Before;
		FVoxelChunkDrawInfo After;
		/** Hashes of the chunk's mesh as the renderer was given it before the dig and last */
		uint64 HashBefore = 0;
		uint64 HashAfter = 0;
		/** A frame between the dig and the end showed the old mesh dithering out under the new one */
		bool bSawCrossfade = false;
		int64 InteriorFadeStarts = 0;
		int32 ActiveFadesAfter = -1;
	};

	/** voxel.MeshFade at console priority (a value typed in the editor's console outranks a code set). */
	int32 SetMeshFade(int32 Value)
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.MeshFade"));
		const int32 Previous = Var ? Var->GetInt() : 1;
		if (Var)
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
		return Previous;
	}

	/** One frame: the chunk manager, the world's clock and the renderer's components (they only tick while a fade runs). */
	void TickFrame(VoxelTesting::FTestChunkWorld& W, FHashingMeshRenderer& Renderer)
	{
		constexpr float Dt = 1.0f / 60.0f;
		W.Manager->TickComponent(Dt, LEVELTICK_All, nullptr);
		W.World->RealTimeSeconds += Dt;
		TArray<TPair<FIntVector, UVoxelRegionComponent*>> Components;
		Renderer.GetRegionComponents(Components);
		for (const TPair<FIntVector, UVoxelRegionComponent*>& Pair : Components)
		{
			if (Pair.Value)
			{
				Pair.Value->TickComponent(Dt, LEVELTICK_All, nullptr);
			}
		}
		Renderer.FlushPendingOperations();
	}

	/** Tick until the pipeline has been idle for 90 frames and no fade runs; false if not within TimeoutSeconds. */
	bool Settle(VoxelTesting::FTestChunkWorld& W, FHashingMeshRenderer& Renderer, TFunctionRef<void()> PerFrame, double TimeoutSeconds = 120.0)
	{
		const FVoxelSeamRegistry* Seams = W.Manager->GetSeamRegistry();
		int32 IdleFrames = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleFrames < 90 && FPlatformTime::Seconds() < Deadline)
		{
			TickFrame(W, Renderer);
			PerFrame();
			const bool bIdle = W.Manager->GetPendingGenerationCount() == 0 && W.Manager->GetPendingMeshingCount() == 0
				&& W.Manager->GetAsyncGenerationInProgressCount() == 0 && W.Manager->GetAsyncMeshingInProgressCount() == 0
				&& W.Manager->GetPendingMeshUploadCount() == 0 && W.Manager->GetSeamJobsInFlightCount() == 0
				&& (!Seams || Seams->GetStats().JobQueueDepth == 0)
				&& (!Renderer.GetOriginRegionComponent() || Renderer.GetOriginRegionComponent()->GetActiveFadeCount() == 0);
			IdleFrames = bIdle ? IdleFrames + 1 : 0;
			FPlatformProcess::Sleep(0.002f);
		}
		return IdleFrames >= 90;
	}

	/** A player's sphere brush (dig) of RadiusVoxels at Center, as the character's dig and the demo's brush apply it. */
	int32 PlayerDig(VoxelTesting::FTestChunkWorld& W, const FVector& Center, float RadiusVoxels)
	{
		UVoxelEditManager* Edits = W.Manager->GetEditManager();
		const EEditSource Previous = Edits->GetEditSource();
		Edits->SetEditSource(EEditSource::Player);
		FVoxelBrushParams Brush;
		Brush.Shape = EVoxelBrushShape::Sphere;
		Brush.Radius = RadiusVoxels * W.Config->VoxelSize;
		Brush.Strength = 1.0f;
		Brush.FalloffType = EVoxelBrushFalloff::Sharp;
		Brush.DensityDelta = 255;
		Brush.MaterialID = EVoxelMaterial::Stone;
		const int32 Count = Edits->ApplyBrushEdit(Center, Brush, EEditMode::Subtract);
		Edits->SetEditSource(Previous);
		return Count;
	}

	FRun Run(FAutomationTestBase& Test, EMeshingMode Mode, bool bFade)
	{
		FRun R;
		const int32 PreviousFade = SetMeshFade(bFade ? 1 : 0);
		UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/VoxelWorlds/Materials/M_VoxelMaster.M_VoxelMaster"));

		VoxelTesting::FTestChunkWorld W;
		W.Create(Mode == EMeshingMode::Cubic ? TEXT("EditReachesProxyCubic") : TEXT("EditReachesProxySmooth"));
		W.ConfigureSmallPlane();
		W.Config->MeshingMode = Mode;
		FHashingMeshRenderer Renderer;
		Renderer.SetMaterial(Master);
		W.Owner = W.World->SpawnActor<AActor>();
		W.Manager = NewObject<UVoxelChunkManager>(W.Owner);
		W.Manager->RegisterComponent();
		Renderer.Initialize(W.World, W.Config);
		W.Manager->Initialize(W.Config, new FDistanceBandLODStrategy(), &Renderer);
		UVoxelRegionComponent* Component = Renderer.GetOriginRegionComponent();

		// A site at the middle of a chunk's footprint where the surface lies at least 6 voxels from the chunk's top and
		// bottom, so a dig of 3 voxels stays inside the chunk's interior mesh
		const double CW = W.Config->GetChunkWorldSize();
		const float VS = W.Config->VoxelSize;
		FVector Site = FVector::ZeroVector;
		for (int32 j = 0; j < 6 && !R.bStarted; ++j)
		for (int32 i = 0; i < 6 && !R.bStarted; ++i)
		{
			const double X = (i + 0.5) * CW, Y = (j + 0.5) * CW;
			const double Z = W.Manager->GetGeneratedSurfacePoint(FVector(X, Y, 0.0)).Z;
			const double InChunk = Z - FMath::FloorToDouble(Z / CW) * CW;
			if (InChunk > 6.0 * VS && InChunk < CW - 6.0 * VS)
			{
				Site = FVector(X, Y, Z);
				R.bStarted = Component && Master;
			}
		}
		if (R.bStarted)
		{
			R.Chunk = W.Manager->WorldToChunkCoord(Site - FVector(0.0, 0.0, 0.5 * VS));
			W.SetViewer(Site + FVector(0.0, 0.0, 300.0));
			R.bSettled = Settle(W, Renderer, [] {});
			Component->GetChunkDrawInfo(R.Chunk, R.Before);
			R.HashBefore = Renderer.MeshHashes.FindRef(R.Chunk);
			int64 Restarts = 0;
			int64 StartsBefore = 0;
			Component->GetFadeCounters(false, StartsBefore, Restarts);

			R.EditedVoxels = PlayerDig(W, Site, 3.0f);
			const FVoxelChunkDrawInfo Old = R.Before;
			R.bSettled = Settle(W, Renderer, [&]
			{
				FVoxelChunkDrawInfo Now;
				Component->GetChunkDrawInfo(R.Chunk, Now);
				R.bSawCrossfade |= Now.bCrossfading && Now.VertexFactory != Old.VertexFactory && Now.PreviousTriangles == Old.CurrentTriangles;
			}) && R.bSettled;
			Component->GetChunkDrawInfo(R.Chunk, R.After);
			R.HashAfter = Renderer.MeshHashes.FindRef(R.Chunk);
			int64 StartsAfter = 0;
			Component->GetFadeCounters(false, StartsAfter, Restarts);
			R.InteriorFadeStarts = StartsAfter - StartsBefore;
			R.ActiveFadesAfter = Component->GetActiveFadeCount();
		}

		W.Manager->SetBenchmarkView(false, FVector::ZeroVector);
		W.Manager->Shutdown();
		Renderer.Shutdown();
		W.Stop();
		SetMeshFade(PreviousFade);
		return R;
	}

	void Check(FAutomationTestBase& Test, EMeshingMode Mode)
	{
		const TCHAR* Name = Mode == EMeshingMode::Cubic ? TEXT("Cubic") : TEXT("DC");
		const FRun On = Run(Test, Mode, /*bFade*/ true);
		const FRun Off = Run(Test, Mode, /*bFade*/ false);
		if (!Test.TestTrue(FString::Printf(TEXT("%s: a site and a renderer"), Name), On.bStarted && Off.bStarted))
		{
			return;
		}
		Test.AddInfo(FString::Printf(TEXT("%s: chunk %s, %d voxels dug; fade on: %u -> %u tris (%lld interior fades, crossfade seen %d); fade off: %u -> %u tris"),
			Name, *On.Chunk.ToString(), On.EditedVoxels, On.Before.CurrentTriangles, On.After.CurrentTriangles, On.InteriorFadeStarts,
			On.bSawCrossfade ? 1 : 0, Off.Before.CurrentTriangles, Off.After.CurrentTriangles));
		Test.TestTrue(FString::Printf(TEXT("%s: both runs settle"), Name), On.bSettled && Off.bSettled);
		Test.TestTrue(FString::Printf(TEXT("%s: the dig changed voxels"), Name), On.EditedVoxels > 0 && On.EditedVoxels == Off.EditedVoxels);
		Test.TestTrue(FString::Printf(TEXT("%s: before the dig the chunk is drawn alone"), Name), On.Before.DrawsCurrentOnly() && Off.Before.DrawsCurrentOnly());

		// Fade on: the re-mesh crossfades from the mesh it replaces, then the new mesh is drawn alone
		Test.TestTrue(FString::Printf(TEXT("%s, fade on: the re-mesh started an interior crossfade"), Name), On.InteriorFadeStarts >= 1);
		Test.TestTrue(FString::Printf(TEXT("%s, fade on: a frame crossfaded from the old mesh to the new one"), Name), On.bSawCrossfade);
		Test.TestTrue(FString::Printf(TEXT("%s, fade on: after the fade the new mesh is drawn alone, nothing retained"), Name),
			On.After.DrawsCurrentOnly() && On.After.PreviousTriangles == 0 && !On.After.bHasFadeState && On.ActiveFadesAfter == 0);
		Test.TestTrue(FString::Printf(TEXT("%s, fade on: the dig changed the chunk's mesh, and the drawn mesh is a new upload (%u -> %u tris)"), Name,
			On.Before.CurrentTriangles, On.After.CurrentTriangles),
			On.HashBefore != 0 && On.HashAfter != On.HashBefore && On.After.VertexFactory != On.Before.VertexFactory);

		// Fade off: drawn at once, and the same mesh
		Test.TestEqual(FString::Printf(TEXT("%s, fade off: no interior crossfade"), Name), Off.InteriorFadeStarts, int64(0));
		Test.TestTrue(FString::Printf(TEXT("%s, fade off: the new mesh is drawn alone"), Name), Off.After.DrawsCurrentOnly() && Off.After.PreviousTriangles == 0);
		Test.TestEqual(FString::Printf(TEXT("%s: the same streamed mesh before the dig in both runs"), Name), On.Before.CurrentTriangles, Off.Before.CurrentTriangles);
		Test.TestTrue(FString::Printf(TEXT("%s: the same streamed mesh before the dig and the same mesh after it in both runs"), Name),
			On.HashBefore == Off.HashBefore && On.HashAfter == Off.HashAfter);
		Test.TestEqual(FString::Printf(TEXT("%s: the fade changes the transition, not the mesh drawn after it"), Name), On.After.CurrentTriangles, Off.After.CurrentTriangles);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditReachesProxySmoothTest, "VoxelWorlds.Streaming.EditReachesProxy.Smooth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditReachesProxySmoothTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: needs a render scene (run without -nullrhi, where components create no scene proxy)")); return true; }
	VoxelEditReachesProxyTest::Check(*this, EMeshingMode::DualContouring);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelEditReachesProxyCubicTest, "VoxelWorlds.Streaming.EditReachesProxy.Cubic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelEditReachesProxyCubicTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI) { AddInfo(TEXT("Skipped: needs a render scene (run without -nullrhi, where components create no scene proxy)")); return true; }
	VoxelEditReachesProxyTest::Check(*this, EMeshingMode::Cubic);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
