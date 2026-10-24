// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "VoxelChunkManager.h"
#include "VoxelEditManager.h"
#include "VoxelEditTypes.h"
#include "VoxelPlanetFarField.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"
#include "IVoxelMeshRenderer.h"
#include "ChunkRenderData.h"
#include "VoxelMeshingTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Orbit and back (spherical Phase 6, WM-BP10, D18; WORLD_MODES_REVIEW_PLAN.md §20.1 and §20.6). A planet's voxel window
// unloads above the voxel cut-over (the far field alone draws the planet up there) and streams back below a fraction of
// it; under a descending camera the streaming viewer goes ahead of it (the descent prefetch), so the window resumes and
// refines before the camera arrives.
//
// - The prefetch's lead (ComputeDescentLead): none for slow descents or climbs, the distance covered in the lead time
//   for fast ones, never closer to the surface than the clearance.
// - A real chunk manager on a small planet (CPU generation, three LOD bands, an edit near the ground): the window loads on
//   the ground; above the cut-over every chunk unloads, the edited one too (its edit stays in the edit layer), the queues
//   hold nothing, the far field cuts no hole; between the resume altitude and the cut-over it stays empty; under the
//   resume altitude it streams back, the edited chunk with it. A camera descending fast from above the cut-over resumes
//   the window while it is still above the resume altitude, with the streaming viewer under it and over the ground.
// - G49 (§20.28): a descent that stops above the cut-over no longer resumes the window for a moment (the lead resumes it
//   with the camera under the cut-over at the earliest); one that stops under it keeps it.
// ---------------------------------------------------------------------------

namespace VoxelPlanetOrbitTestUtils
{
	/** A renderer that records which chunks hold a mesh (the chunk manager needs one to initialise). */
	class FCountingMeshRenderer : public IVoxelMeshRenderer
	{
	public:
		TSet<FIntVector> Chunks;

		virtual void Initialize(UWorld* World, const UVoxelWorldConfiguration* WorldConfig) override { bInitialized = true; }
		virtual void Shutdown() override { bInitialized = false; }
		virtual bool IsInitialized() const override { return bInitialized; }
		virtual void UpdateChunkMesh(const FChunkRenderData& RenderData) override {}
		virtual void UpdateChunkMeshFromCPU(const FIntVector& ChunkCoord, int32 LODLevel, const FChunkMeshData& MeshData) override { Chunks.Add(ChunkCoord); }
		virtual void RemoveChunk(const FIntVector& ChunkCoord) override { Chunks.Remove(ChunkCoord); }
		virtual void ClearAllChunks() override { Chunks.Reset(); }
		virtual void SetChunkVisible(const FIntVector& ChunkCoord, bool bVisible) override {}
		virtual void SetAllChunksVisible(bool bVisible) override {}
		virtual void SetMaterial(UMaterialInterface* Material) override {}
		virtual UMaterialInterface* GetMaterial() const override { return nullptr; }
		virtual void UpdateMaterialParameters() override {}
		virtual void UpdateSeamMeshFromCPU(const FIntVector& OwnerChunkCoord, uint8 SeamSlot, int32 LODLevel, FChunkMeshData&& MeshData, bool bCrossfade) override {}
		virtual void RemoveSeamMesh(const FIntVector& OwnerChunkCoord, uint8 SeamSlot) override {}
		virtual void ClearAllSeamMeshes() override {}
		virtual void GetSeamMeshOwners(TArray<FIntVector>& OutOwners) const override {}
		virtual void UpdateLODTransition(const FIntVector& ChunkCoord, float MorphFactor) override {}
		virtual bool IsChunkLoaded(const FIntVector& ChunkCoord) const override { return Chunks.Contains(ChunkCoord); }
		virtual int32 GetLoadedChunkCount() const override { return Chunks.Num(); }
		virtual void GetLoadedChunks(TArray<FIntVector>& OutChunks) const override { OutChunks = Chunks.Array(); }
		virtual int64 GetGPUMemoryUsage() const override { return 0; }
		virtual int64 GetTotalVertexCount() const override { return 0; }
		virtual int64 GetTotalTriangleCount() const override { return 0; }
		virtual bool GetChunkBounds(const FIntVector& ChunkCoord, FBox& OutBounds) const override { return false; }
		virtual FBox GetTotalBounds() const override { return FBox(ForceInit); }
		virtual FString GetDebugStats() const override { return FString(); }
		virtual FString GetRendererTypeName() const override { return TEXT("Counting (test)"); }

	private:
		bool bInitialized = false;
	};

	constexpr float Radius = 20000.0f;
	constexpr float ViewDistance = 6000.0f;

	/** A small planet (R 20,000, terrain +-800, a sea 200 under the datum) streamed by a real chunk manager. */
	struct FOrbitWorld
	{
		UWorld* World = nullptr;
		UVoxelWorldConfiguration* Config = nullptr;
		AActor* Owner = nullptr;
		UVoxelChunkManager* Manager = nullptr;
		FCountingMeshRenderer Renderer;

		void Start(const TCHAR* Name)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, Name);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
			Config = NewObject<UVoxelWorldConfiguration>();
			Config->AddToRoot();
			Config->WorldMode = EWorldMode::SphericalPlanet;
			Config->WorldOrigin = FVector::ZeroVector;
			Config->WorldRadius = Radius;
			Config->PlanetHeightScale = 1600.0f;
			Config->BaseHeight = 0.0f;
			Config->PlanetMaxTerrainHeight = 1000.0f;
			Config->PlanetMaxTerrainDepth = 1000.0f;
			Config->MeshingMode = EMeshingMode::DualContouring;
			Config->bEnableLOD = true;
			Config->ChunkSize = 32;
			Config->VoxelSize = 50.0f;
			Config->ViewDistance = ViewDistance;
			Config->LODBands = {
				FLODBand(0.0f, 2000.0f, 0, 1),
				FLODBand(2000.0f, 4000.0f, 1, 2),
				FLODBand(4000.0f, ViewDistance, 2, 4) };
			Config->bUseGPUGeneration = false;
			Config->bUseGPUMeshing = false;
			Config->bGenerateCollision = false;
			Config->bEnableScatter = false;
			Config->bEnableCaves = false;
			Config->bEnableBiomes = false;
			Config->bEnableWaterLevel = true;
			Config->WaterRadius = Radius - 200.0f;
			Config->NoiseParams.Seed = 9001;
			Config->NoiseParams.Frequency = 0.0004f;
			Config->NoiseParams.Octaves = 3;
			// The cut-over at the view distance (the default is three: PlanetVoxelCutoverAltitude 0).
			Config->PlanetVoxelCutoverAltitude = ViewDistance;
			Owner = World->SpawnActor<AActor>();
			Manager = NewObject<UVoxelChunkManager>(Owner);
			Manager->RegisterComponent();
			Renderer.Initialize(World, Config);
			Manager->Initialize(Config, new FDistanceBandLODStrategy(), &Renderer);
			if (FVoxelPlanetFarField* FarField = Manager->GetFarFieldMutable())
			{
				FarField->SetSynchronousBuilds(true);
				FarField->SetMaxBuildsInFlight(64);
			}
		}

		void Tick()
		{
			Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FPlatformProcess::Sleep(0.002f);
		}

		bool IsIdle() const
		{
			return Manager->GetPendingGenerationCount() == 0 && Manager->GetPendingMeshingCount() == 0
				&& Manager->GetAsyncGenerationInProgressCount() == 0 && Manager->GetAsyncMeshingInProgressCount() == 0
				&& Manager->GetPendingMeshUploadCount() == 0 && Manager->GetSeamJobsInFlightCount() == 0;
		}

		/** Hold the bench view at a point and tick until the pipeline has been idle for 90 ticks (false past 120 s). */
		bool SettleAt(const FVector& Viewer)
		{
			Manager->SetBenchmarkView(true, Viewer);
			int32 IdleTicks = 0;
			const double Deadline = FPlatformTime::Seconds() + 120.0;
			while (IdleTicks < 90 && FPlatformTime::Seconds() < Deadline)
			{
				Tick();
				IdleTicks = IsIdle() ? IdleTicks + 1 : 0;
			}
			return IdleTicks >= 90;
		}

		void Stop()
		{
			Manager->SetBenchmarkView(false, FVector::ZeroVector);
			Manager->Shutdown();
			Owner->Destroy();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			Config->RemoveFromRoot();
		}
	};
}

/**
 * WM-BP10: the voxel cut-over and the descent prefetch in a chunk manager (above). The cut-over is the view distance
 * (6,000 over the terrain or the sea), resuming below 0.85 of it (5,100).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetCutoverDescentTest,
	"VoxelWorlds.Streaming.Planet.CutoverDescent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetCutoverDescentTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetOrbitTestUtils;

	// The lead: none climbing or descending slowly; the distance covered in the lead time; never past the clearance.
	{
		TestEqual(TEXT("Lead: none while climbing"), UVoxelChunkManager::ComputeDescentLead(5000.0, 50000.0, 3.0, 1500.0, 1000.0), 0.0);
		TestEqual(TEXT("Lead: none for a slow descent (a jump)"), UVoxelChunkManager::ComputeDescentLead(-800.0, 50000.0, 3.0, 1500.0, 1000.0), 0.0);
		TestEqual(TEXT("Lead: off with no lead time"), UVoxelChunkManager::ComputeDescentLead(-20000.0, 50000.0, 0.0, 1500.0, 1000.0), 0.0);
		TestEqual(TEXT("Lead: the distance covered in the lead time"), UVoxelChunkManager::ComputeDescentLead(-4000.0, 50000.0, 3.0, 1500.0, 1000.0), 12000.0);
		TestEqual(TEXT("Lead: at most half the altitude"), UVoxelChunkManager::ComputeDescentLead(-40000.0, 50000.0, 3.0, 1500.0, 1000.0), 25000.0);
		TestEqual(TEXT("Lead: stops the clearance over the surface"), UVoxelChunkManager::ComputeDescentLead(-40000.0, 1600.0, 3.0, 1500.0, 1000.0), 600.0);
		TestEqual(TEXT("Lead: none under the clearance"), UVoxelChunkManager::ComputeDescentLead(-40000.0, 600.0, 3.0, 1500.0, 1000.0), 0.0);
	}

	FOrbitWorld W;
	W.Start(TEXT("VoxelPlanetCutoverTestWorld"));
	const FVector Up = FVector(0.3, -0.2, 1.0).GetSafeNormal();
	const FVoxelTerrainSurfacePoint Ground = W.Manager->GetSurfacePointAt(Up);
	// Over the terrain or the sea, whichever is higher (the cut-over's surface).
	const double SurfaceRadius = FMath::Max(static_cast<double>(Radius) + Ground.Altitude, static_cast<double>(W.Config->WaterRadius));
	auto At = [&Up, SurfaceRadius](double OverSurface) { return Up * (SurfaceRadius + OverSurface); };
	const double Cutover = W.Manager->GetLODStrategy()->GetCutoverAltitude();
	TestTrue(TEXT("The configured cut-over (the view distance)"), FMath::IsNearlyEqual(Cutover, static_cast<double>(ViewDistance), 1.0));

	// On the ground: the window loads; an edit near the ground.
	TestTrue(TEXT("Settled on the ground"), W.SettleAt(At(300.0)));
	const int32 OnTheGround = W.Manager->GetLoadedChunkCount();
	TestFalse(TEXT("On the ground the window is not suspended"), W.Manager->IsVoxelWindowSuspended());
	TestTrue(TEXT("On the ground the window holds chunks"), OnTheGround > 30);
	FVoxelBrushParams Brush;
	Brush.Shape = EVoxelBrushShape::Sphere;
	Brush.Radius = 300.0f;
	Brush.Strength = 1.0f;
	Brush.FalloffType = EVoxelBrushFalloff::Linear;
	Brush.DensityDelta = 255;
	W.Manager->GetEditManager()->ApplyBrushEdit(Ground.Position, Brush, EEditMode::Subtract);
	TestTrue(TEXT("Settled after the edit"), W.SettleAt(At(300.0)));
	TArray<FIntVector> Edited;
	W.Manager->GetEditManager()->GetEditedChunkCoords(Edited);
	TestTrue(TEXT("The edit touched chunks"), Edited.Num() > 0);
	const FIntVector EditedChunk = Edited.Num() > 0 ? Edited[0] : FIntVector::ZeroValue;
	TestTrue(TEXT("The edited chunk is loaded on the ground"), W.Manager->IsChunkLoaded(EditedChunk));

	// Above the cut-over: everything unloads, the edited chunk too (its edit stays); nothing queued; no hole.
	TestTrue(TEXT("Settled above the cut-over"), W.SettleAt(At(Cutover + 2000.0)));
	TestTrue(TEXT("Above the cut-over the window is suspended"), W.Manager->IsVoxelWindowSuspended());
	TestEqual(TEXT("Above the cut-over no chunk is held"), W.Manager->GetLoadedChunkCount(), 0);
	TestEqual(TEXT("Above the cut-over the renderer holds no chunk"), W.Renderer.Chunks.Num(), 0);
	TestFalse(TEXT("The edited chunk unloaded"), W.Manager->IsChunkLoaded(EditedChunk));
	TestTrue(TEXT("Its edit stays in the edit layer"), W.Manager->GetEditManager()->ChunkHasEdits(EditedChunk));
	const FVoxelResidencySnapshot Up1 = W.Manager->GetResidencySnapshot();
	TestTrue(TEXT("The snapshot reports the suspension"), Up1.bWindowSuspended && FMath::IsNearlyEqual(Up1.CutoverAltitude, Cutover, 1.0));
	if (W.Manager->HasFarField())
	{
		W.Manager->UpdateFarFieldForViewer(At(Cutover + 2000.0));
		TestEqual(TEXT("Above the cut-over the far field hides no band patch"), W.Manager->GetFarFieldStats().HiddenBand, 0);
		TestTrue(TEXT("Above the cut-over the far field draws the planet"), W.Manager->GetFarFieldStats().DrawnPatches > 0);
	}

	// Between the resume altitude and the cut-over: still suspended (hysteresis). The bench view jumps there (a descent of
	// thousands of uu in a frame), which the descent prefetch would lead: off for the jumps, on again for the descent.
	IConsoleVariable* LeadVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.DescentLead"));
	const float SavedLead = LeadVar ? LeadVar->GetFloat() : 0.0f;
	if (LeadVar)
	{
		LeadVar->Set(0.0f, ECVF_SetByCode);
	}
	TestTrue(TEXT("Settled between the resume altitude and the cut-over"), W.SettleAt(At(0.5 * (FDistanceBandLODStrategy::CutoverResumeFraction + 1.0) * Cutover)));
	TestTrue(TEXT("Under the cut-over but over the resume altitude the window stays suspended"), W.Manager->IsVoxelWindowSuspended());
	TestEqual(TEXT("... and empty"), W.Manager->GetLoadedChunkCount(), 0);

	// Under the resume altitude: the window streams back, the edited chunk with it.
	TestTrue(TEXT("Settled back on the ground"), W.SettleAt(At(300.0)));
	TestFalse(TEXT("Back on the ground the window resumed"), W.Manager->IsVoxelWindowSuspended());
	const int32 Back = W.Manager->GetLoadedChunkCount();
	TestTrue(TEXT("Back on the ground the window holds what it held before"), FMath::Abs(Back - OnTheGround) <= OnTheGround / 10);
	TestTrue(TEXT("The edited chunk is back"), W.Manager->IsChunkLoaded(EditedChunk));
	if (LeadVar)
	{
		LeadVar->Set(SavedLead, ECVF_SetByCode);
	}

	// A fast descent from above the cut-over: the window resumes while the camera is still over the resume altitude, the
	// streaming viewer under the camera and over the ground.
	TestTrue(TEXT("Settled for the descent"), W.SettleAt(At(3.0 * Cutover)));
	TestTrue(TEXT("Suspended before the descent"), W.Manager->IsVoxelWindowSuspended());
	constexpr double SpeedPerTick = 300.0; // 18,000 uu/s at 60 Hz
	double CameraOver = 3.0 * Cutover;
	double ResumedAt = -1.0, LeadAtResume = 0.0, MinViewerOver = TNumericLimits<double>::Max(), MaxLead = 0.0;
	while (CameraOver > 300.0)
	{
		CameraOver = FMath::Max(CameraOver - SpeedPerTick, 300.0);
		W.Manager->SetBenchmarkView(true, At(CameraOver));
		W.Tick();
		const double Lead = W.Manager->GetStreamingViewerLead();
		MaxLead = FMath::Max(MaxLead, Lead);
		MinViewerOver = FMath::Min(MinViewerOver, W.Manager->GetStreamingViewerPosition().Size() - SurfaceRadius);
		TestTrue(TEXT("The streaming viewer lies on the camera's up-line, under it"),
			FVector::CrossProduct(W.Manager->GetStreamingViewerPosition().GetSafeNormal(), Up).Size() < 1.0e-6
			&& W.Manager->GetStreamingViewerPosition().Size() <= W.Manager->GetCameraPosition().Size() + 0.01);
		if (ResumedAt < 0.0 && !W.Manager->IsVoxelWindowSuspended())
		{
			ResumedAt = CameraOver;
			LeadAtResume = Lead;
		}
	}
	AddInfo(FString::Printf(TEXT("Cut-over %.0f (resumes below %.0f): %d chunks on the ground, %d back; descending at %.0f uu/s the window resumed with the camera %.0f over the surface (lead %.0f), largest lead %.0f, the streaming viewer never under %.0f over it"),
		Cutover, Cutover * FDistanceBandLODStrategy::CutoverResumeFraction, OnTheGround, Back, SpeedPerTick * 60.0, ResumedAt, LeadAtResume, MaxLead, MinViewerOver));
	TestTrue(TEXT("Descending, the window resumes"), ResumedAt > 0.0);
	TestTrue(TEXT("... while the camera is still over the resume altitude (the prefetch)"), ResumedAt > Cutover * FDistanceBandLODStrategy::CutoverResumeFraction);
	TestTrue(TEXT("... and under the cut-over (G49: voxel.World.CutoverResumeGuard)"), ResumedAt <= Cutover);
	TestTrue(TEXT("The prefetch led the camera"), MaxLead > 1000.0);
	TestTrue(TEXT("The streaming viewer stays over the surface"), MinViewerOver >= 0.0);
	TestTrue(TEXT("Settled after the descent"), W.SettleAt(At(300.0)));
	TestEqual(TEXT("Stopped, the streaming viewer is the camera"), W.Manager->GetStreamingViewerLead(), 0.0);

	// voxel.World.VoxelCutover 0: never suspended.
	IConsoleVariable* CutoverVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.VoxelCutover"));
	if (CutoverVar)
	{
		const float Saved = CutoverVar->GetFloat();
		CutoverVar->Set(0.0f, ECVF_SetByCode);
		TestTrue(TEXT("Settled high with the cut-over off"), W.SettleAt(At(3.0 * Cutover)));
		TestFalse(TEXT("With voxel.World.VoxelCutover 0 the window is never suspended"), W.Manager->IsVoxelWindowSuspended());
		TestTrue(TEXT("... and keeps its ground disk"), W.Manager->GetLoadedChunkCount() > 30);
		CutoverVar->Set(Saved, ECVF_SetByCode);
	}

	W.Stop();
	return true;
}

/**
 * G49 (WORLD_MODES_REVIEW_PLAN.md §20.28): a fast descent that stops above the cut-over, in the chunk manager above (the
 * cut-over C 6,000 over the terrain or sea, resuming below 5,100), descending from 3 C at 18,000 uu/s:
 * - stopping at 1.4 C and held 3 s: with voxel.World.CutoverResumeGuard 0 (the rule before) the prefetch's lead resumes
 *   the window and, once the camera has stopped, it suspends again with what it loaded and queued for nothing; with 1 it
 *   never resumes and holds no chunk;
 * - stopping at 0.92 C (under the cut-over, over the resume altitude) and held 3 s: under both rules it resumes during the
 *   descent and stays resumed (the camera under the cut-over keeps it), then settles with its ground disk;
 * - with the cut-over at 40,000 (voxel.World.VoxelCutover; the band between the resume altitude and the cut-over 6,000):
 *   a descent at 1,800 uu/s, whose lead (5,400) stays inside the band, resumes at the same camera altitude under both
 *   rules; one at 6,000 uu/s (lead 18,000) resumes with the camera over the cut-over under the old rule and at the
 *   cut-over under the guard.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlanetCutoverDescentStopTest,
	"VoxelWorlds.Streaming.Planet.CutoverDescentStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlanetCutoverDescentStopTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlanetOrbitTestUtils;

	IConsoleVariable* GuardVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.CutoverResumeGuard"));
	IConsoleVariable* CutoverVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.World.VoxelCutover"));
	IConsoleVariable* LeadVar = IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Stream.DescentLead"));
	TestNotNull(TEXT("voxel.World.CutoverResumeGuard exists"), GuardVar);
	if (!GuardVar || !CutoverVar || !LeadVar)
	{
		return false;
	}
	const int32 SavedGuard = GuardVar->GetInt();
	const float SavedCutover = CutoverVar->GetFloat();
	const float SavedLead = LeadVar->GetFloat();
	LeadVar->Set(3.0f, ECVF_SetByConsole);

	FOrbitWorld W;
	W.Start(TEXT("VoxelPlanetCutoverStopTestWorld"));
	const FVector Up = FVector(0.3, -0.2, 1.0).GetSafeNormal();
	const FVoxelTerrainSurfacePoint Ground = W.Manager->GetSurfacePointAt(Up);
	const double SurfaceRadius = FMath::Max(static_cast<double>(Radius) + Ground.Altitude, static_cast<double>(W.Config->WaterRadius));
	auto At = [&Up, SurfaceRadius](double OverSurface) { return Up * (SurfaceRadius + OverSurface); };
	const double Cutover = W.Manager->GetLODStrategy()->GetCutoverAltitude();
	const double Resume = Cutover * FDistanceBandLODStrategy::CutoverResumeFraction;

	struct FDescent
	{
		double ResumedAt = -1.0;       // the camera over the surface at the first resume (-1: none)
		int32 Resumes = 0;
		int32 Suspensions = 0;
		int32 MaxLoaded = 0;           // chunks loaded at any tick
		int32 HeldAtSuspensions = 0;   // chunks the suspensions unloaded
		int32 Dropped = 0;             // queued generations the suspensions evicted
		int64 Generated = 0;           // chunks generated in the windows the suspensions ended
		bool bSuspendedAtEnd = false;
	};
	// From From down to To at Speed (uu a tick at 60 Hz), held HoldTicks there, stopping at the first resume if asked.
	auto Descend = [&](double From, double To, double SpeedPerTick, int32 HoldTicks, bool bStopAtResume)
	{
		FDescent Out;
		const int64 Start = W.Manager->GetCutoverTransitionCount();
		double CameraOver = From;
		int32 Hold = 0;
		while (Hold < HoldTicks)
		{
			if (CameraOver > To)
			{
				CameraOver = FMath::Max(CameraOver - SpeedPerTick, To);
			}
			else
			{
				++Hold;
			}
			W.Manager->SetBenchmarkView(true, At(CameraOver));
			W.Tick();
			Out.MaxLoaded = FMath::Max(Out.MaxLoaded, W.Manager->GetLoadedChunkCount());
			if (Out.ResumedAt < 0.0 && !W.Manager->IsVoxelWindowSuspended())
			{
				Out.ResumedAt = CameraOver;
				if (bStopAtResume)
				{
					break;
				}
			}
		}
		const TArray<FVoxelCutoverTransition>& Transitions = W.Manager->GetCutoverTransitions();
		const int64 First = W.Manager->GetCutoverTransitionCount() - Transitions.Num();
		for (int32 I = 0; I < Transitions.Num(); ++I)
		{
			if (First + I < Start)
			{
				continue;
			}
			const FVoxelCutoverTransition& T = Transitions[I];
			if (T.bSuspended)
			{
				++Out.Suspensions;
				Out.HeldAtSuspensions += T.ChunksHeld;
				Out.Dropped += T.GenerationsDropped;
				Out.Generated += T.GeneratedSinceResume;
			}
			else
			{
				++Out.Resumes;
			}
		}
		Out.bSuspendedAtEnd = W.Manager->IsVoxelWindowSuspended();
		return Out;
	};
	auto Describe = [](const FDescent& D)
	{
		return FString::Printf(TEXT("resumed with the camera %.0f over the surface, %d resume(s), %d suspension(s), at most %d chunks loaded, %d held and %d queued generations dropped at the suspensions, %lld generated in between, suspended at the end %d"),
			D.ResumedAt, D.Resumes, D.Suspensions, D.MaxLoaded, D.HeldAtSuspensions, D.Dropped, D.Generated, D.bSuspendedAtEnd ? 1 : 0);
	};
	constexpr double Fast = 300.0; // 18,000 uu/s at 60 Hz

	// A stop at 1.4 C, held 3 s.
	FDescent StopHigh[2];
	for (int32 Guard = 0; Guard < 2; ++Guard)
	{
		GuardVar->Set(Guard, ECVF_SetByConsole);
		TestTrue(TEXT("Settled over the cut-over"), W.SettleAt(At(3.0 * Cutover)));
		StopHigh[Guard] = Descend(3.0 * Cutover, 1.4 * Cutover, Fast, 180, false);
		AddInfo(FString::Printf(TEXT("Guard %d, stop at 1.4 C (%.0f): %s"), Guard, 1.4 * Cutover, *Describe(StopHigh[Guard])));
	}
	TestTrue(TEXT("Rule before: the lead resumed the window over the cut-over"), StopHigh[0].Resumes >= 1 && StopHigh[0].ResumedAt > Cutover);
	TestTrue(TEXT("Rule before: ... and it suspended again once the camera stopped"), StopHigh[0].Suspensions >= 1 && StopHigh[0].bSuspendedAtEnd);
	TestTrue(TEXT("Rule before: ... with work done or queued for nothing"), StopHigh[0].HeldAtSuspensions + StopHigh[0].Dropped > 0);
	TestEqual(TEXT("Guard: no resume"), StopHigh[1].Resumes, 0);
	TestEqual(TEXT("Guard: no suspension"), StopHigh[1].Suspensions, 0);
	TestEqual(TEXT("Guard: no chunk loaded"), StopHigh[1].MaxLoaded, 0);
	TestTrue(TEXT("Guard: suspended throughout"), StopHigh[1].bSuspendedAtEnd);

	// A stop at 0.92 C (under the cut-over, over the resume altitude), held 3 s.
	FDescent StopLow[2];
	for (int32 Guard = 0; Guard < 2; ++Guard)
	{
		GuardVar->Set(Guard, ECVF_SetByConsole);
		TestTrue(TEXT("Settled over the cut-over"), W.SettleAt(At(3.0 * Cutover)));
		StopLow[Guard] = Descend(3.0 * Cutover, 0.92 * Cutover, Fast, 180, false);
		AddInfo(FString::Printf(TEXT("Guard %d, stop at 0.92 C (%.0f): %s"), Guard, 0.92 * Cutover, *Describe(StopLow[Guard])));
		TestTrue(*FString::Printf(TEXT("Guard %d: resumed during the descent"), Guard), StopLow[Guard].Resumes == 1 && StopLow[Guard].ResumedAt > Resume);
		TestTrue(*FString::Printf(TEXT("Guard %d: stays resumed under the cut-over"), Guard), StopLow[Guard].Suspensions == 0 && !StopLow[Guard].bSuspendedAtEnd);
		TestTrue(*FString::Printf(TEXT("Guard %d: settles with its ground disk"), Guard), W.SettleAt(At(0.92 * Cutover)) && !W.Manager->IsVoxelWindowSuspended() && W.Manager->GetLoadedChunkCount() > 30);
	}
	TestTrue(TEXT("Rule before: the resume came with the camera over the cut-over"), StopLow[0].ResumedAt > Cutover);
	TestTrue(TEXT("Guard: the resume came with the camera at the cut-over (within a tick)"), StopLow[1].ResumedAt <= Cutover && StopLow[1].ResumedAt >= Cutover - Fast);

	// The cut-over at 40,000: a lead inside the band changes nothing; a lead past it moves the resume to the cut-over.
	CutoverVar->Set(40000.0f, ECVF_SetByConsole);
	const double Wide = 40000.0;
	FDescent Slow[2], Quick[2];
	for (int32 Guard = 0; Guard < 2; ++Guard)
	{
		GuardVar->Set(Guard, ECVF_SetByConsole);
		TestTrue(TEXT("Settled over the wide cut-over"), W.SettleAt(At(Wide + 6000.0)));
		Slow[Guard] = Descend(Wide + 6000.0, 300.0, 30.0, 1, true);
		TestTrue(TEXT("Settled over the wide cut-over"), W.SettleAt(At(1.5 * Wide)));
		Quick[Guard] = Descend(1.5 * Wide, 300.0, 100.0, 1, true);
		AddInfo(FString::Printf(TEXT("Guard %d, C 40,000: 1,800 uu/s %s; 6,000 uu/s %s"), Guard, *Describe(Slow[Guard]), *Describe(Quick[Guard])));
	}
	TestTrue(TEXT("A lead inside the band: the same resume under both rules"), Slow[0].ResumedAt > 0.0 && FMath::IsNearlyEqual(Slow[0].ResumedAt, Slow[1].ResumedAt, 0.5));
	TestTrue(TEXT("... with the camera under the cut-over"), Slow[1].ResumedAt <= Wide);
	TestTrue(TEXT("A lead past the band, rule before: resumed with the camera over the cut-over"), Quick[0].ResumedAt > Wide + 1000.0);
	TestTrue(TEXT("... guard: at the cut-over"), Quick[1].ResumedAt <= Wide && Quick[1].ResumedAt > Wide - 100.0);

	CutoverVar->Set(SavedCutover, ECVF_SetByConsole);
	GuardVar->Set(SavedGuard, ECVF_SetByConsole);
	LeadVar->Set(SavedLead, ECVF_SetByConsole);
	W.Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
