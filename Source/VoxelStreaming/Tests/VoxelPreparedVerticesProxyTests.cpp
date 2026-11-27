// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "RHI.h"             // GUsingNullRHI: no scene proxy under -nullrhi, the draw checks skip there
#include "Materials/MaterialInterface.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "Testing/VoxelTestScopedCVar.h"
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
// G294: a chunk mesh's render vertices are made on its mesh worker (voxel.Mesh.PrepareRenderVertices 1, the default) or at
// the submit on the game thread (0, before G294). The chunk manager streams a small plane into the real renderer twice,
// once each way, and a player digs at the surface (an edit's re-mesh goes through the same mesh jobs): every chunk ends
// with the same vertices, indices and bounds handed to its region (the renderer's record of each submit,
// FVoxelMeshRenderer::SetRecordSubmittedVertexHashes), the same bounds in the renderer, the same triangles drawn by the
// proxy, the same loaded chunks, and the renderer took the worker's vertices on the default path and converted every mesh
// on the other (FVoxelSubmitCounters). Dual Contouring and cubic. The proxy's draw checks need a render scene (the editor);
// the rest runs headless too.
// ---------------------------------------------------------------------------

namespace VoxelPreparedVerticesProxyTest
{
	struct FChunkRecord
	{
		uint64 Hash = 0;
		FBox Bounds = FBox(ForceInit);
		uint32 Triangles = 0;
	};

	struct FRun
	{
		bool bStarted = false;
		bool bSettled = false;
		int32 EditedVoxels = 0;
		TMap<FIntVector, FChunkRecord> Chunks;
		int64 PreparedTaken = 0;
		int64 Converted = 0;
	};

	void TickFrame(VoxelTesting::FTestChunkWorld& W, FVoxelMeshRenderer& Renderer)
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

	bool Settle(VoxelTesting::FTestChunkWorld& W, FVoxelMeshRenderer& Renderer, double TimeoutSeconds = 120.0)
	{
		const FVoxelSeamRegistry* Seams = W.Manager->GetSeamRegistry();
		int32 IdleFrames = 0;
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (IdleFrames < 90 && FPlatformTime::Seconds() < Deadline)
		{
			TickFrame(W, Renderer);
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

	FRun Run(EMeshingMode Mode, int32 Prepare)
	{
		FRun R;
		const VoxelTesting::FScopedCVar PrepareCVar(TEXT("voxel.Mesh.PrepareRenderVertices"), Prepare);
		UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, TEXT("/VoxelWorlds/Materials/M_VoxelMaster.M_VoxelMaster"));

		VoxelTesting::FTestChunkWorld W;
		W.Create(Mode == EMeshingMode::Cubic ? TEXT("PreparedVerticesCubic") : TEXT("PreparedVerticesSmooth"));
		W.ConfigureSmallPlane();
		W.Config->MeshingMode = Mode;
		FVoxelMeshRenderer Renderer;
		Renderer.SetMaterial(Master);
		W.Owner = W.World->SpawnActor<AActor>();
		W.Manager = NewObject<UVoxelChunkManager>(W.Owner);
		W.Manager->RegisterComponent();
		Renderer.Initialize(W.World, W.Config);
		Renderer.SetRecordSubmittedVertexHashes(true);
		W.Manager->Initialize(W.Config, new FDistanceBandLODStrategy(), &Renderer);
		UVoxelRegionComponent* Component = Renderer.GetOriginRegionComponent();
		R.bStarted = Component != nullptr && Master != nullptr;
		const FVoxelSubmitCounters::FCounts Before = FVoxelSubmitCounters::Get();

		if (R.bStarted)
		{
			const double CW = W.Config->GetChunkWorldSize();
			const FVector Site = W.Manager->GetGeneratedSurfacePoint(FVector(1.5 * CW, 1.5 * CW, 0.0));
			W.SetViewer(Site + FVector(0.0, 0.0, 300.0));
			R.bSettled = Settle(W, Renderer);

			// A player's dig of 3 voxels at the surface: the edited chunks' re-meshes go through the mesh jobs too
			UVoxelEditManager* Edits = W.Manager->GetEditManager();
			const EEditSource Previous = Edits->GetEditSource();
			Edits->SetEditSource(EEditSource::Player);
			FVoxelBrushParams Brush;
			Brush.Shape = EVoxelBrushShape::Sphere;
			Brush.Radius = 3.0f * W.Config->VoxelSize;
			Brush.Strength = 1.0f;
			Brush.FalloffType = EVoxelBrushFalloff::Sharp;
			Brush.DensityDelta = 255;
			Brush.MaterialID = EVoxelMaterial::Stone;
			R.EditedVoxels = Edits->ApplyBrushEdit(Site, Brush, EEditMode::Subtract);
			Edits->SetEditSource(Previous);
			R.bSettled = Settle(W, Renderer) && R.bSettled;

			for (const TPair<FIntVector, uint64>& Pair : Renderer.GetSubmittedVertexHashes())
			{
				FChunkRecord& Record = R.Chunks.Add(Pair.Key);
				Record.Hash = Pair.Value;
				Renderer.GetChunkBounds(Pair.Key, Record.Bounds);
				if (!GUsingNullRHI)
				{
					FVoxelChunkDrawInfo Info;
					Component->GetChunkDrawInfo(Pair.Key, Info);
					Record.Triangles = Info.CurrentTriangles;
				}
			}
		}
		const FVoxelSubmitCounters::FCounts After = FVoxelSubmitCounters::Get();
		R.PreparedTaken = After.PreparedTaken - Before.PreparedTaken;
		R.Converted = After.Converted - Before.Converted;

		W.Manager->SetBenchmarkView(false, FVector::ZeroVector);
		W.Manager->Shutdown();
		Renderer.Shutdown();
		W.Stop();
		return R;
	}

	void Check(FAutomationTestBase& Test, EMeshingMode Mode)
	{
		const TCHAR* Name = Mode == EMeshingMode::Cubic ? TEXT("Cubic") : TEXT("DC");
		const FRun OnWorker = Run(Mode, 1);
		const FRun AtSubmit = Run(Mode, 0);
		if (!Test.TestTrue(FString::Printf(TEXT("%s: a renderer and its material"), Name), OnWorker.bStarted && AtSubmit.bStarted))
		{
			return;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: both runs settle"), Name), OnWorker.bSettled && AtSubmit.bSettled);
		Test.TestTrue(FString::Printf(TEXT("%s: the dig changed voxels, the same in both"), Name), OnWorker.EditedVoxels > 0 && OnWorker.EditedVoxels == AtSubmit.EditedVoxels);
		Test.TestTrue(FString::Printf(TEXT("%s: chunks with meshes (non-vacuous)"), Name), OnWorker.Chunks.Num() > 8);
		Test.TestEqual(FString::Printf(TEXT("%s: the same chunks have meshes"), Name), OnWorker.Chunks.Num(), AtSubmit.Chunks.Num());
		int32 Differ = 0;
		for (const TPair<FIntVector, FChunkRecord>& Pair : OnWorker.Chunks)
		{
			const FChunkRecord* Other = AtSubmit.Chunks.Find(Pair.Key);
			if (!Other || Other->Hash != Pair.Value.Hash || !Other->Bounds.Equals(Pair.Value.Bounds, 0.0) || Other->Triangles != Pair.Value.Triangles)
			{
				++Differ;
				if (Differ <= 4)
				{
					Test.AddError(FString::Printf(TEXT("%s: chunk %s differs between the paths (%s)"), Name, *Pair.Key.ToString(),
						Other ? TEXT("its render data") : TEXT("missing in the other run")));
				}
			}
		}
		Test.TestEqual(FString::Printf(TEXT("%s: every chunk's vertices, indices, bounds and drawn triangles the same"), Name), Differ, 0);
		Test.TestTrue(FString::Printf(TEXT("%s: the default path took the workers' vertices"), Name), OnWorker.PreparedTaken > 0);
		Test.TestEqual(FString::Printf(TEXT("%s: with voxel.Mesh.PrepareRenderVertices 0 every mesh is converted at the submit"), Name), AtSubmit.PreparedTaken, int64(0));
		Test.TestTrue(FString::Printf(TEXT("%s: with voxel.Mesh.PrepareRenderVertices 0 meshes were converted"), Name), AtSubmit.Converted > 0);
		Test.AddInfo(FString::Printf(TEXT("%s: %d chunks with meshes, %d voxels dug; on the worker %lld taken, %lld converted; at the submit %lld taken, %lld converted"),
			Name, OnWorker.Chunks.Num(), OnWorker.EditedVoxels, OnWorker.PreparedTaken, OnWorker.Converted, AtSubmit.PreparedTaken, AtSubmit.Converted));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPreparedVerticesProxySmoothTest, "VoxelWorlds.Streaming.PreparedVertices.SmoothSameData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPreparedVerticesProxySmoothTest::RunTest(const FString& Parameters)
{
	VoxelPreparedVerticesProxyTest::Check(*this, EMeshingMode::DualContouring);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPreparedVerticesProxyCubicTest, "VoxelWorlds.Streaming.PreparedVertices.CubicSameData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPreparedVerticesProxyCubicTest::RunTest(const FString& Parameters)
{
	VoxelPreparedVerticesProxyTest::Check(*this, EMeshingMode::Cubic);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
