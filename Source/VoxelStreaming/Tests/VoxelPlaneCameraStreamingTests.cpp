// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Testing/VoxelTestChunkWorld.h"
#include "VoxelChunkManager.h"
#include "VoxelWorldConfiguration.h"
#include "DistanceBandLODStrategy.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Mode parity (WORLD_MODES_REVIEW_PLAN.md §23, milestone MP1): two of the planet's streaming rules on a heightmap world.
//
// - WM-MP3, the descent prefetch: under a camera descending faster than voxel.Stream.DescentMinSpeed the streaming viewer
//   goes ahead of it along local down (-Z on a plane) by what it covers in voxel.Stream.DescentLead seconds, at most half
//   its altitude and never under two chunks over the generated surface or the sea, so the window's near band and its LOD
//   refinement arrive before the camera does. The planet had it since spherical Phase 6 (WM-BP10); the plane had the
//   camera as its streaming viewer at any speed.
// - WM-MP4, the streaming decisions after a camera cut: the chunk manager ticks in TG_PrePhysics and reads the camera of
//   the frame before, so the frame of a teleport streamed for the old position. After a cut of more than a chunk the
//   post-update tick (F4.3's, after the camera managers) runs the decisions again for this frame's camera, and the chunks
//   under the new position are requested in that frame.
// ---------------------------------------------------------------------------

namespace VoxelPlaneCameraStreamingTestUtils
{
	/** Set by code, as the planet orbit tests set voxel.Stream.DescentLead: a console-priority set here would outrank theirs. */
	struct FScopedCVar
	{
		IConsoleVariable* Var = nullptr;
		FString Previous;
		FScopedCVar(const TCHAR* Name, const TCHAR* Value)
		{
			Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var)
			{
				Previous = Var->GetString();
				Var->Set(Value, ECVF_SetByCode);
			}
		}
		~FScopedCVar()
		{
			if (Var)
			{
				Var->Set(*Previous, ECVF_SetByCode);
			}
		}
	};

	constexpr float ViewDistance = 6000.0f;

	/** A plane with three LOD bands (as the planet orbit test's planet) and a sea at Z -200. */
	void ConfigurePlaneWithLOD(VoxelTesting::FTestChunkWorld& W)
	{
		W.ConfigureSmallPlane();
		W.Config->bEnableLOD = true;
		W.Config->ViewDistance = ViewDistance;
		W.Config->LODBands = {
			FLODBand(0.0f, 2000.0f, 0, 1),
			FLODBand(2000.0f, 4000.0f, 1, 2),
			FLODBand(4000.0f, ViewDistance, 2, 4) };
		W.Config->bEnableWaterLevel = true;
		W.Config->WaterLevel = -200.0f;
	}
}

/**
 * WM-MP3: the descent prefetch on a plane. A chunk manager on a plane (terrain +-800, chunks of 32 voxels of 50 uu, a sea
 * at -200) with the bench view as the camera: settled on the ground and at 40,000 over it the lead is 0; descending at
 * 18,000 uu/s the streaming viewer lies on the camera's vertical under it and, while led, never under two chunks (3,200)
 * over the surface (the terrain or the sea, whichever is higher); stopped, the viewer is the camera again; a climb has no
 * lead; with voxel.Stream.DescentLead 0 the viewer is the camera throughout. What the lead buys on a heightmap world,
 * whose ground is loaded under a high viewer already (D8) and whose near band is clipped to the terrain's height bounds,
 * is the LOD: the first LOD 1 chunk is drawn with the camera at least 2,000 higher over the ground than without it
 * (12,400 against 5,800 when written).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneDescentPrefetchTest,
	"VoxelWorlds.Streaming.Plane.DescentPrefetch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneDescentPrefetchTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlaneCameraStreamingTestUtils;
	using namespace VoxelTesting;

	double FirstLOD1Altitude[2] = { -1.0, -1.0 }; // [0] without the lead, [1] with it
	for (const bool bLead : { true, false })
	{
		const TCHAR* Rule = bLead ? TEXT("the prefetch") : TEXT("voxel.Stream.DescentLead 0");
		const FScopedCVar LeadVar(TEXT("voxel.Stream.DescentLead"), bLead ? TEXT("3") : TEXT("0"));

		FTestChunkWorld W;
		W.Create(TEXT("PlaneDescentPrefetchWorld"));
		ConfigurePlaneWithLOD(W);
		W.StartManager();
		const double ChunkWorldSize = W.Config->GetChunkWorldSize();
		const double Clearance = 2.0 * ChunkWorldSize;

		// The site: over land (the surface's Z against the sea's).
		const FVector2D Site(4000.0, -2500.0);
		const double Ground = W.Manager->GetGeneratedSurfaceHeight(Site.X, Site.Y);
		const double SurfaceZ = FMath::Max(Ground, static_cast<double>(W.Config->WaterLevel));
		auto At = [&Site, SurfaceZ](double OverSurface) { return FVector(Site.X, Site.Y, SurfaceZ + OverSurface); };

		// On the ground, and far above it: no lead once settled; never a suspended window on a plane.
		W.SetViewer(At(300.0));
		TestTrue(*FString::Printf(TEXT("%s: settled on the ground"), Rule), W.Settle());
		const int32 OnTheGround = W.Manager->GetLoadedChunkCount();
		TestTrue(*FString::Printf(TEXT("%s: the window holds chunks on the ground"), Rule), OnTheGround > 20);
		TestFalse(*FString::Printf(TEXT("%s: a plane's window is never suspended"), Rule), W.Manager->IsVoxelWindowSuspended());
		TestEqual(*FString::Printf(TEXT("%s: settled, the streaming viewer is the camera"), Rule), W.Manager->GetStreamingViewerLead(), 0.0);

		constexpr double Top = 40000.0;
		W.SetViewer(At(Top));
		TestTrue(*FString::Printf(TEXT("%s: settled high above"), Rule), W.Settle());
		TestEqual(*FString::Printf(TEXT("%s: settled high, no lead"), Rule), W.Manager->GetStreamingViewerLead(), 0.0);
		TestTrue(*FString::Printf(TEXT("%s: the ground stays loaded under a high viewer (D8)"), Rule),
			W.Manager->IsChunkLoaded(W.Manager->WorldToChunkCoord(FVector(Site.X, Site.Y, Ground))));

		// The descent at 18,000 uu/s. On a heightmap world the ground is loaded under a high viewer already (D8) and the
		// near band is clipped to the terrain's height bounds, so the lead's work is the LOD: chunks refine for the led
		// viewer before the camera gets near them. Measured as the camera altitude at which the first LOD 1 chunk is drawn.
		constexpr double SpeedPerTick = 300.0;
		double CameraOver = Top;
		double MaxLead = 0.0, MinLedViewerOver = TNumericLimits<double>::Max(), FirstLOD1At = -1.0;
		bool bOnVertical = true, bUnderCamera = true;
		int32 LedTicks = 0;
		while (CameraOver > 300.0)
		{
			CameraOver = FMath::Max(CameraOver - SpeedPerTick, 300.0);
			W.Manager->SetBenchmarkView(true, At(CameraOver));
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			FPlatformProcess::Sleep(0.002f);

			const double Lead = W.Manager->GetStreamingViewerLead();
			const FVector Viewer = W.Manager->GetStreamingViewerPosition();
			const FVector Camera = W.Manager->GetCameraPosition();
			MaxLead = FMath::Max(MaxLead, Lead);
			if (Lead > 0.0)
			{
				++LedTicks;
				MinLedViewerOver = FMath::Min(MinLedViewerOver, Viewer.Z - SurfaceZ);
			}
			bOnVertical &= FMath::IsNearlyEqual(Viewer.X, Camera.X, 0.01) && FMath::IsNearlyEqual(Viewer.Y, Camera.Y, 0.01);
			bUnderCamera &= Viewer.Z <= Camera.Z + 0.01;
			if (FirstLOD1At < 0.0)
			{
				const FVoxelResidencySnapshot Snapshot = W.Manager->GetResidencySnapshot();
				if (Snapshot.ByLOD.IsValidIndex(1) && Snapshot.ByLOD[1] > 0)
				{
					FirstLOD1At = CameraOver;
				}
			}
		}
		FirstLOD1Altitude[bLead ? 1 : 0] = FirstLOD1At;
		AddInfo(FString::Printf(TEXT("%s: descending at %.0f uu/s from %.0f: %d ticks with a lead, the largest %.0f, the led streaming viewer never under %.0f over the surface (clearance %.0f); the first LOD 1 chunk drawn with the camera %.0f over the surface"),
			Rule, SpeedPerTick * 60.0, Top, LedTicks, MaxLead, LedTicks > 0 ? MinLedViewerOver : 0.0, Clearance, FirstLOD1At));
		TestTrue(*FString::Printf(TEXT("%s: the streaming viewer lies on the camera's vertical"), Rule), bOnVertical);
		TestTrue(*FString::Printf(TEXT("%s: ... under the camera"), Rule), bUnderCamera);
		TestTrue(*FString::Printf(TEXT("%s: an LOD 1 chunk was drawn during the descent"), Rule), FirstLOD1At > 0.0);
		if (bLead)
		{
			TestTrue(*FString::Printf(TEXT("%s: the prefetch led the camera"), Rule), MaxLead > 5000.0 && LedTicks > 20);
			TestTrue(*FString::Printf(TEXT("%s: the led streaming viewer stays two chunks over the surface"), Rule), MinLedViewerOver >= Clearance - 0.01);
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: no lead at any speed"), Rule), MaxLead, 0.0);
		}

		// Stopped: the viewer is the camera again; a climb has no lead.
		TestTrue(*FString::Printf(TEXT("%s: settled after the descent"), Rule), W.Settle());
		TestEqual(*FString::Printf(TEXT("%s: stopped, the streaming viewer is the camera"), Rule), W.Manager->GetStreamingViewerLead(), 0.0);
		const int32 Back = W.Manager->GetLoadedChunkCount();
		TestTrue(*FString::Printf(TEXT("%s: back on the ground the window holds what it held before"), Rule), FMath::Abs(Back - OnTheGround) <= OnTheGround / 10);
		double ClimbLead = 0.0;
		for (double Over = 300.0; Over < 12000.0; Over += SpeedPerTick)
		{
			W.Manager->SetBenchmarkView(true, At(Over));
			W.Manager->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			ClimbLead = FMath::Max(ClimbLead, W.Manager->GetStreamingViewerLead());
		}
		TestEqual(*FString::Printf(TEXT("%s: a climb has no lead"), Rule), ClimbLead, 0.0);
		// Settled before the world goes (seam jobs in flight at Shutdown run on their workers after it).
		TestTrue(*FString::Printf(TEXT("%s: settled after the climb"), Rule), W.Settle());

		W.Stop();
	}
	// The lead refines the LOD ahead of the camera: the first LOD 1 chunk is drawn with the camera higher over the ground.
	AddInfo(FString::Printf(TEXT("The first LOD 1 chunk drawn with the camera %.0f over the surface with the prefetch, %.0f without"), FirstLOD1Altitude[1], FirstLOD1Altitude[0]));
	TestTrue(TEXT("With the prefetch the first LOD 1 chunk is drawn with the camera at least 2,000 higher than without"),
		FirstLOD1Altitude[1] >= FirstLOD1Altitude[0] + 2000.0);
	return true;
}

/**
 * WM-MP4: the streaming decisions for the camera a frame renders with. A chunk manager on a plane in a game world with a
 * player controller whose view target is a camera actor, frames stepped in UWorld::Tick's order: the chunk manager's tick
 * (TG_PrePhysics, reading the camera manager's camera), the camera managers, then the camera tick (TG_PostUpdateWork). The
 * window settles around the camera 300 over the ground; the camera then jumps ten chunks east, past the view distance.
 * With voxel.Stream.CameraCutUpdate the chunk under the new camera is requested in that very frame and the first
 * generations are launched; under the rule before that frame streamed for the old camera, the chunk under the new one
 * stayed unloaded, and the next frame requested it. Moving under a chunk a frame runs no second update.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelPlaneCameraAfterCutTest,
	"VoxelWorlds.Streaming.Plane.CameraAfterCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoxelPlaneCameraAfterCutTest::RunTest(const FString& Parameters)
{
	using namespace VoxelPlaneCameraStreamingTestUtils;
	using namespace VoxelTesting;

	constexpr float FrameSeconds = 1.0f / 60.0f;
	for (const bool bCutUpdate : { false, true })
	{
		const TCHAR* Rule = bCutUpdate ? TEXT("streaming for the frame's camera") : TEXT("the rule before");
		const FScopedCVar CutUpdate(TEXT("voxel.Stream.CameraCutUpdate"), bCutUpdate ? TEXT("1") : TEXT("0"));
		const FScopedCVar NoLead(TEXT("voxel.Stream.DescentLead"), TEXT("0"));

		FTestChunkWorld W;
		W.Create(TEXT("PlaneCameraAfterCutWorld"));
		// Actors initialised, so a player controller spawns its camera manager and joins the world's controllers.
		W.World->InitializeActorsForPlay(FURL());
		ConfigurePlaneWithLOD(W);
		W.StartManager();
		const double ChunkWorldSize = W.Config->GetChunkWorldSize();

		const FVector2D Site(4000.0, -2500.0);
		const double Ground = W.Manager->GetGeneratedSurfaceHeight(Site.X, Site.Y);
		const FVector Start(Site.X, Site.Y, FMath::Max(Ground, static_cast<double>(W.Config->WaterLevel)) + 300.0);
		const FVector2D FarSite = Site + FVector2D(10.0 * ChunkWorldSize, 0.0);
		const double FarGround = W.Manager->GetGeneratedSurfaceHeight(FarSite.X, FarSite.Y);
		const FVector Destination(FarSite.X, FarSite.Y, FMath::Max(FarGround, static_cast<double>(W.Config->WaterLevel)) + 300.0);
		const FIntVector DestinationGroundChunk = W.Manager->WorldToChunkCoord(FVector(FarSite.X, FarSite.Y, FarGround));

		// The camera: a camera actor, the controller's view target (as vox.Orbit's camera is).
		APlayerController* PC = W.World->SpawnActor<APlayerController>();
		ACameraActor* Camera = W.World->SpawnActor<ACameraActor>(Start, FRotator::ZeroRotator);
		const bool bReady = PC && PC->PlayerCameraManager && Camera && W.World->GetFirstPlayerController() == PC;
		TestTrue(*FString::Printf(TEXT("%s: a controller with a camera manager, the world's first"), Rule), bReady);
		if (bReady)
		{
			PC->PlayerCameraManager->bUseClientSideCameraUpdates = false;
			PC->SetViewTarget(Camera);
			W.Manager->RegisterAllComponentTickFunctions(true);
			FVoxelFarFieldCameraTickFunction& CameraTick = W.Manager->GetFarFieldCameraTickFunction();
			TestTrue(*FString::Printf(TEXT("%s: the camera tick is registered in TG_PostUpdateWork"), Rule),
				CameraTick.IsTickFunctionRegistered() && CameraTick.TickGroup == TG_PostUpdateWork);

			// A frame as UWorld::Tick runs it.
			UVoxelChunkManager* Manager = W.Manager;
			auto Frame = [Manager, PC, &CameraTick]()
			{
				Manager->TickComponent(FrameSeconds, LEVELTICK_All, nullptr);
				PC->UpdateCameraManager(FrameSeconds);
				CameraTick.ExecuteTick(FrameSeconds, LEVELTICK_All, ENamedThreads::GameThread, FGraphEventRef());
			};
			auto IsIdle = [Manager]()
			{
				return Manager->GetPendingGenerationCount() == 0 && Manager->GetPendingMeshingCount() == 0
					&& Manager->GetAsyncGenerationInProgressCount() == 0 && Manager->GetAsyncMeshingInProgressCount() == 0
					&& Manager->GetPendingMeshUploadCount() == 0 && Manager->GetSeamJobsInFlightCount() == 0;
			};

			// Settle around the start (the camera manager's camera, not the bench view).
			int32 IdleFrames = 0;
			const double Deadline = FPlatformTime::Seconds() + 120.0;
			while (IdleFrames < 30 && FPlatformTime::Seconds() < Deadline)
			{
				Frame();
				FPlatformProcess::Sleep(0.002f);
				IdleFrames = IsIdle() ? IdleFrames + 1 : 0;
			}
			TestTrue(*FString::Printf(TEXT("%s: settles"), Rule), IdleFrames >= 30);
			TestTrue(*FString::Printf(TEXT("%s: the camera manager's camera is the camera actor"), Rule),
				PC->PlayerCameraManager->GetCameraLocation().Equals(Camera->GetActorLocation(), 1.0));
			TestTrue(*FString::Printf(TEXT("%s: the streaming follows the camera manager"), Rule), W.Manager->GetCameraPosition().Equals(Start, 1.0));
			TestTrue(*FString::Printf(TEXT("%s: the destination lies outside the window"), Rule), W.Manager->GetChunkState(DestinationGroundChunk) == EChunkState::Unloaded);

			// Moving under a chunk a frame: no second update.
			const int64 UpdatesBefore = W.Manager->GetCameraCutUpdates();
			for (int32 Step = 1; Step <= 5; ++Step)
			{
				Camera->SetActorLocation(Start + FVector(400.0 * Step, 0.0, 0.0));
				Frame();
			}
			TestEqual(*FString::Printf(TEXT("%s: no second update while moving 400 uu a frame"), Rule), W.Manager->GetCameraCutUpdates(), UpdatesBefore);

			// The cut.
			Camera->SetActorLocation(Destination);
			Frame();
			const EChunkState InTheFrame = W.Manager->GetChunkState(DestinationGroundChunk);
			const int64 CutUpdates = W.Manager->GetCameraCutUpdates() - UpdatesBefore;
			const int32 PendingInTheFrame = W.Manager->GetPendingGenerationCount() + W.Manager->GetAsyncGenerationInProgressCount();
			const bool bViewerMovedInTheFrame = W.Manager->GetStreamingViewerPosition().Equals(Destination, 1.0);
			Frame();
			const EChunkState NextFrame = W.Manager->GetChunkState(DestinationGroundChunk);
			AddInfo(FString::Printf(TEXT("%s: the frame of the cut: the chunk under the camera %s, %d generations pending or in flight, the streaming viewer %s, %lld update(s) for the frame's camera; the next frame: the chunk %s"),
				Rule, InTheFrame == EChunkState::Unloaded ? TEXT("unloaded") : TEXT("requested"), PendingInTheFrame,
				bViewerMovedInTheFrame ? TEXT("at the new camera") : TEXT("still at the old one"), CutUpdates, NextFrame == EChunkState::Unloaded ? TEXT("unloaded") : TEXT("requested")));
			TestTrue(*FString::Printf(TEXT("%s: the frame after the cut has requested the chunk under the camera"), Rule), NextFrame != EChunkState::Unloaded);
			if (bCutUpdate)
			{
				TestTrue(*FString::Printf(TEXT("%s: the frame of the cut requests the chunk under the camera"), Rule), InTheFrame != EChunkState::Unloaded);
				TestTrue(*FString::Printf(TEXT("%s: ... and has generations under way"), Rule), PendingInTheFrame > 0);
				TestTrue(*FString::Printf(TEXT("%s: ... the streaming viewer at the new camera"), Rule), bViewerMovedInTheFrame);
				TestEqual(*FString::Printf(TEXT("%s: one update for the frame's camera"), Rule), CutUpdates, int64(1));
			}
			else
			{
				// Documents the frame this closes (WM-MP4): if this fails the rule before no longer lags the camera here.
				TestTrue(*FString::Printf(TEXT("%s: the frame of the cut streams for the old camera, nothing under the new one"), Rule), InTheFrame == EChunkState::Unloaded);
				TestEqual(*FString::Printf(TEXT("%s: no update for the frame's camera"), Rule), CutUpdates, int64(0));
			}

			// Settled at the destination the window is whole either way.
			IdleFrames = 0;
			while (IdleFrames < 30 && FPlatformTime::Seconds() < Deadline)
			{
				Frame();
				FPlatformProcess::Sleep(0.002f);
				IdleFrames = IsIdle() ? IdleFrames + 1 : 0;
			}
			TestTrue(*FString::Printf(TEXT("%s: settles at the destination"), Rule), IdleFrames >= 30);
			TestTrue(*FString::Printf(TEXT("%s: the chunk under the camera is loaded"), Rule), W.Manager->IsChunkLoaded(DestinationGroundChunk));
		}
		W.Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
